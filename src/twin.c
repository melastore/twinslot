#include "twin.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <linux/blkpg.h>
#include <linux/fs.h>
#include <linux/magic.h>

#include "avb.h"
#include "bctl.h"
#include "bootimg.h"
#include "erofs.h"
#include "gpt.h"
#include "lp.h"
#include "part.h"

#define MiB (1ull << 20)
#define GiB (1ull << 30)
#define MIN_USERDATA (8 * GiB)
#define DEFAULT_META (32 * MiB)
#define DATA_HEAD_WIPE (16 * MiB)

const char *g_disk;
const char *g_table_dir;
const char *g_kept_copy;
bool g_kept_copy_earlier;

/* ---- finding the disk ---- */

/* Probes are quiet: most LUNs are not the one. */
static int quiet_begin(void)
{
	int saved = dup(2), devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
	if (saved >= 0 && devnull >= 0)
		dup2(devnull, 2);
	if (devnull >= 0)
		close(devnull);
	return saved;
}

static void quiet_end(int saved)
{
	if (saved >= 0) {
		dup2(saved, 2);
		close(saved);
	}
}

static bool disk_has_userdata(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	struct gpt g;
	bool found = false;
	int q = quiet_begin();
	if (gpt_read(fd, &g) == EXIT_OK) {
		found = gpt_find(&g, "userdata") >= 0;
		gpt_free(&g);
	}
	quiet_end(q);
	close(fd);
	return found;
}

/*
 * The whole disk a block device lives on, by name ("sdc"): /sys/dev/block/MAJ:MIN
 * of a partition is .../block/<disk>/<partition>, and a device-mapper device
 * (dm-crypt /data) is followed down its slaves. False when it is not a block
 * device (tmpfs, FUSE).
 */
static bool backing_disk(dev_t dev, char *name, size_t cap)
{
	char real[PATH_MAX], probe[PATH_MAX + 300];
	snprintf(probe, sizeof(probe), "/sys/dev/block/%u:%u", major(dev), minor(dev));
	if (!major(dev) || !realpath(probe, real))
		return false;
	for (int depth = 0; depth < 8; depth++) {
		snprintf(probe, sizeof(probe), "%s/slaves", real);
		DIR *d = opendir(probe);
		struct dirent *de = NULL;
		while (d && (de = readdir(d)) && de->d_name[0] == '.')
			;
		if (!de) {
			if (d)
				closedir(d);
			break;
		}
		snprintf(probe, sizeof(probe), "%s/slaves/%s", real, de->d_name);
		closedir(d);
		if (!realpath(probe, real))
			return false;
	}
	snprintf(probe, sizeof(probe), "%s/partition", real);
	bool part = !access(probe, F_OK);
	char *slash = strrchr(real, '/');
	if (!slash)
		return false;
	if (part) {
		*slash = 0;
		slash = strrchr(real, '/');
		if (!slash)
			return false;
	}
	snprintf(name, cap, "%s", slash + 1);
	return true;
}

/* The disk by-name/userdata lives on: a USB stick that carries a "userdata" partition is not picked. */
static bool disk_of_userdata(char *path, size_t cap)
{
	struct stat st;
	char name[64];
	if (stat(dev_path("userdata"), &st) || !S_ISBLK(st.st_mode) || !backing_disk(st.st_rdev, name, sizeof(name)))
		return false;
	snprintf(path, cap, "/dev/block/%s", name);
	return !access(path, F_OK);
}

int twin_find_disk(char *path, size_t cap)
{
	if (g_disk) {
		snprintf(path, cap, "%s", g_disk);
		return EXIT_OK;
	}
	if (!dev_dir_is_phone()) {
		/* Image directories hold the whole disk as "disk". */
		snprintf(path, cap, "%s", dev_path("disk"));
		if (!access(path, F_OK))
			return EXIT_OK;
		msg("%s has no disk image (use --disk)", g_dev_dir);
		return EXIT_IO;
	}
	if (disk_of_userdata(path, cap) && disk_has_userdata(path))
		return EXIT_OK;
	for (char c = 'a'; c <= 'z'; c++) {
		snprintf(path, cap, "/dev/block/sd%c", c);
		if (access(path, F_OK))
			continue;
		if (disk_has_userdata(path))
			return EXIT_OK;
	}
	snprintf(path, cap, "/dev/block/mmcblk0");
	if (!access(path, F_OK) && disk_has_userdata(path))
		return EXIT_OK;
	msg("cannot find the disk that holds userdata (use --disk)");
	return EXIT_IO;
}

/* Whether a saved table fits this disk (its size and sector size), quietly. */
static bool copy_fits(const char *path, u8 *area, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	u64 bytes;
	u32 lbs;
	u8 guid[16];
	int q = quiet_begin();
	bool ok = !io_size(fd, &bytes) && gpt_area_check(area, len, bytes, &lbs, guid) == EXIT_OK;
	quiet_end(q);
	close(fd);
	return ok;
}

/*
 * The disk a saved copy goes back to. Its table may be gone, so userdata cannot
 * be looked for: the copy's name says which disk it came from
 * (twinslot-gpt-<disk>-<time>.bin), and failing that the one disk of its size.
 */
static int restore_disk(const char *file, u8 *area, size_t len, char *path, size_t cap)
{
	if (g_disk || !dev_dir_is_phone())
		return twin_find_disk(path, cap);
	const char *base = strrchr(file, '/');
	base = base ? base + 1 : file;
	static const char prefix[] = "twinslot-gpt-";
	if (!strncmp(base, prefix, sizeof(prefix) - 1)) {
		const char *tag = base + sizeof(prefix) - 1;
		size_t n = strspn(tag, "abcdefghijklmnopqrstuvwxyz0123456789");
		if (n && n < 32 && tag[n] == '-') {
			snprintf(path, cap, "/dev/block/%.*s", (int)n, tag);
			if (copy_fits(path, area, len))
				return EXIT_OK;
		}
	}
	char found[64] = "";
	int matches = 0;
	for (int i = 0; i <= 26; i++) {
		char cand[64];
		if (i < 26)
			snprintf(cand, sizeof(cand), "/dev/block/sd%c", 'a' + i);
		else
			snprintf(cand, sizeof(cand), "/dev/block/mmcblk0");
		if (access(cand, F_OK) || !copy_fits(cand, area, len))
			continue;
		if (matches++)
			msg("%s and %s both fit this copy", found, cand);
		snprintf(found, sizeof(found), "%s", cand);
	}
	if (matches == 1) {
		snprintf(path, cap, "%s", found);
		return EXIT_OK;
	}
	msg(matches ? "name the disk the copy belongs to (--disk /dev/block/...)"
		    : "no disk here has the size this copy was saved from (--disk /dev/block/... names one)");
	return EXIT_IO;
}

int twin_state(struct twin_state *st)
{
	memset(st, 0, sizeof(*st));
	st->owner = -1;
	int r = twin_find_disk(st->disk, sizeof(st->disk));
	if (r)
		return r;
	int fd = io_open(st->disk, false);
	if (fd < 0)
		return EXIT_IO;
	struct gpt g;
	r = gpt_read(fd, &g);
	close(fd);
	if (r)
		return r;
	int ud = gpt_find(&g, "userdata"), td = gpt_find(&g, TWIN_DATA), tm = gpt_find(&g, TWIN_META);
	if (ud >= 0)
		st->userdata_bytes = gpt_entry_bytes(&g, &g.e[ud]);
	if (td >= 0 && tm >= 0) {
		st->ready = true;
		st->owner = g.e[td].attrs & TWIN_ATTR_OWNER_B ? 1 : 0;
		st->data_bytes = gpt_entry_bytes(&g, &g.e[td]);
		st->meta_bytes = gpt_entry_bytes(&g, &g.e[tm]);
	}
	gpt_free(&g);
	return EXIT_OK;
}

enum binding slot_expected_binding(const struct twin_state *st, int slot)
{
	return st->ready && slot == st->owner ? BIND_TWIN : BIND_PRIMARY;
}

/* ---- a slot's fstabs ---- */

struct fstab_walk {
	struct erofs *fs;
	enum binding to; /* BIND_NONE: only look */
	enum binding seen;
	int changes;
};

static int edit_fstab(u8 *data, size_t len, void *ctx)
{
	struct fstab_walk *w = ctx;
	w->seen = binding_merge(w->seen, fstab_binding(data, len));
	return fstab_rebind(data, len, w->to);
}

static int rebind_file(struct fstab_walk *w, const struct erofs_inode *ino)
{
	if (w->to == BIND_NONE) {
		u8 *data;
		int r = erofs_read(w->fs, ino, &data);
		if (r)
			return r;
		w->seen = binding_merge(w->seen, fstab_binding(data, (size_t)ino->size));
		free(data);
		return EXIT_OK;
	}
	int changes = 0;
	int r = erofs_patch(w->fs, ino, edit_fstab, w, &changes);
	if (!r)
		w->changes += changes;
	return r;
}

static int visit_etc(const char *name, const struct erofs_inode *ino, void *ctx)
{
	if (strncmp(name, "fstab.", 6) || !erofs_is_reg(ino))
		return EXIT_OK;
	return rebind_file(ctx, ino);
}

/*
 * Non-EROFS vendor images (ext4) keep small files unencoded: scan for fstab
 * lines that name the data partitions and rebind those lines where they lie.
 */
static int scan_rebind(const struct part *p, enum binding to, enum binding *seen, int *changes)
{
	enum { CHUNK = 8 << 20, OVERLAP = 4096 };
	static const char *needles[] = {"/userdata", "/metadata", "/" TWIN_DATA, "/" TWIN_META};
	u8 *buf = xmalloc(CHUNK);
	int r = EXIT_OK;
	for (u64 base = 0; base < p->size && !r; base += CHUNK - OVERLAP) {
		size_t len = (size_t)MIN((u64)CHUNK, p->size - base);
		if ((r = part_read(p, buf, len, base)))
			break;
		bool last = base + len >= p->size;
		for (size_t i = 0; i + 9 <= len; i++) {
			if (buf[i] != '/')
				continue;
			bool hit = false;
			for (size_t k = 0; k < ARRAY_SIZE(needles) && !hit; k++)
				hit = i + strlen(needles[k]) <= len && !memcmp(buf + i, needles[k], strlen(needles[k]));
			if (!hit)
				continue;
			size_t s = i, e = i;
			while (s > 0 && buf[s - 1] != '\n' && i - s < 1024)
				s--;
			while (e < len && buf[e] != '\n' && e - i < 2048)
				e++;
			/* Lines starting in the overlap are handled by the next chunk. */
			if (!last && s >= len - OVERLAP)
				continue;
			*seen = binding_merge(*seen, fstab_binding(buf + s, e - s));
			if (to != BIND_NONE) {
				int c = fstab_rebind(buf + s, e - s, to);
				if (c) {
					*changes += c;
					r = part_write(p, buf + s, e - s, base + s);
				}
			}
			i = e;
		}
	}
	free(buf);
	return r;
}

/* Super, its metadata for slot and slot's vendor in it; refused if vendor is not installed. */
static int slot_vendor(int slot, bool write, struct part *super, struct lp_metadata *m, struct part *vendor)
{
	int r = super_read_slot(super, write, slot, m);
	if (r)
		return r;
	char name[32];
	snprintf(name, sizeof(name), "vendor_%c", slot_letter(slot));
	int i = lp_find_partition(m, name);
	if (i < 0 || !m->parts[i].nextents) {
		msg("%s is not installed", name);
		lp_free(m);
		part_close(super);
		return EXIT_REFUSED;
	}
	part_logical(vendor, super->fd, &m->parts[i]);
	return EXIT_OK;
}

/* The ext4 superblock magic, at byte 1080. */
static bool is_ext4(const struct part *p)
{
	u8 magic[2];
	return p->size > 2048 && part_read(p, magic, 2, 1080) == EXIT_OK && magic[0] == 0x53 && magic[1] == 0xef;
}

static int vendor_rebind(int slot, enum binding to, enum binding *seen, int *changes)
{
	*seen = BIND_NONE;
	*changes = 0;

	struct part super, vendor;
	struct lp_metadata m;
	int r = slot_vendor(slot, to != BIND_NONE, &super, &m, &vendor);
	if (r)
		return r;
	const char *name = vendor.name;

	struct erofs fs;
	if (erofs_open(&fs, &vendor) == EXIT_OK) {
		struct erofs_inode etc;
		r = erofs_lookup(&fs, "/etc", &etc);
		if (r) {
			msg("%s has no /etc", name);
			goto done;
		}
		struct fstab_walk w = {&fs, to, BIND_NONE, 0};
		r = erofs_readdir(&fs, &etc, visit_etc, &w);
		struct erofs_inode fc;
		if (!r && erofs_lookup(&fs, "/etc/selinux/vendor_file_contexts", &fc) == EXIT_OK && erofs_is_reg(&fc))
			r = rebind_file(&w, &fc);
		*seen = w.seen;
		*changes = w.changes;
	} else if (is_ext4(&vendor)) {
		r = scan_rebind(&vendor, to, seen, changes);
		if (!r && *changes)
			r = part_sync(&vendor);
	} else {
		/* Not a filesystem twinslot knows: a raw scan could patch whatever lies there. */
		msg("%s is neither EROFS nor ext4; cannot read its fstab", name);
		r = EXIT_BAD_INPUT;
	}
done:
	lp_free(&m);
	part_close(&super);
	return r;
}

/*
 * The image whose ramdisks hold the slot's first-stage and recovery fstabs:
 * vendor_boot_X, or boot_X on a phone without vendor_boot. *base gets the
 * name vbmeta uses for it.
 */
static void ramdisk_image(int slot, char *name, size_t cap, const char **base)
{
	snprintf(name, cap, "vendor_boot_%c", slot_letter(slot));
	*base = "vendor_boot";
	if (!dev_exists(name)) {
		snprintf(name, cap, "boot_%c", slot_letter(slot));
		*base = "boot";
	}
}

static int ramdisk_binding(int slot, enum binding to, enum binding *seen, int *changes)
{
	char name[32];
	const char *base;
	ramdisk_image(slot, name, sizeof(name), &base);
	struct part p;
	int r = part_open(&p, name, to != BIND_NONE);
	if (r)
		return r;
	r = bootimg_rebind(&p, to, to != BIND_NONE, seen, changes);
	part_close(&p);
	return r;
}

int slot_binding(int slot, enum binding *ramdisk, enum binding *vendor)
{
	int changes;
	int r = ramdisk_binding(slot, BIND_NONE, ramdisk, &changes);
	if (!r)
		r = vendor_rebind(slot, BIND_NONE, vendor, &changes);
	return r;
}

/* vbmeta re-signed over the slot's boot image and vendor as they are now. */
static int slot_reseal(int slot, int *changed)
{
	char name[32];
	const char *base;
	struct part rd, super, vendor;
	struct lp_metadata m;
	ramdisk_image(slot, name, sizeof(name), &base);
	int r = part_open(&rd, name, true);
	if (r)
		return r;
	if (!(r = slot_vendor(slot, true, &super, &m, &vendor))) {
		r = avb_reseal(slot, &rd, base, &vendor, changed);
		lp_free(&m);
		part_close(&super);
	}
	part_close(&rd);
	return r;
}

int slot_rebind(int slot, enum binding to)
{
	enum binding seen;
	int rd_changes, v_changes;
	char name[32];
	const char *base;
	ramdisk_image(slot, name, sizeof(name), &base);
	int r = ramdisk_binding(slot, to, &seen, &rd_changes);
	if (r)
		return r;
	if (seen == BIND_NONE) {
		msg("%s has no fstab that mounts /data; cannot rebind this ROM", name);
		return EXIT_BAD_INPUT;
	}
	r = vendor_rebind(slot, to, &seen, &v_changes);
	if (r)
		return r;
	if (seen == BIND_NONE) {
		msg("vendor_%c has no fstab that mounts /data; cannot rebind this ROM", slot_letter(slot));
		return EXIT_BAD_INPUT;
	}
	int avb_changes = 0;
	r = slot_reseal(slot, &avb_changes);
	out("rebound slot %c to %s data (%d fstab entries in %s, %d in vendor, %d vbmeta changes)", slot_letter(slot),
	    binding_name(to), rd_changes, base, v_changes, avb_changes);
	return r;
}

/* ---- setup and remove ---- */

/* "major:minor" of a kernel block device (sysfs name such as sda12 or dm-5). */
static bool sys_devnum(const char *sysname, char *out, size_t cap)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/sys/class/block/%s/dev", sysname);
	char *t = read_text_file(path, 64);
	if (!t)
		return false;
	t[strcspn(t, "\n")] = 0;
	snprintf(out, cap, "%s", t);
	free(t);
	return true;
}

/* Does path name the block device major:minor? */
static bool path_is_devnum(const char *path, const char *devnum)
{
	struct stat st;
	char id[32];
	if (stat(path, &st) || !S_ISBLK(st.st_mode))
		return false;
	snprintf(id, sizeof(id), "%u:%u", major(st.st_rdev), minor(st.st_rdev));
	return !strcmp(id, devnum);
}

/*
 * Is device major:minor mounted or used as swap? mountinfo's own device
 * field is not enough (btrfs reports an anonymous one), so the mount source
 * after " - " is checked too.
 */
static bool devnum_in_use(const char *devnum)
{
	bool used = false;
	char *mi = read_text_file("/proc/self/mountinfo", 4 << 20);
	for (char *line = mi; line && *line && !used;) {
		char *next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		char id[32], source[PATH_MAX];
		const char *tail = strstr(line, " - ");
		used = (sscanf(line, "%*s %*s %31s", id) == 1 && !strcmp(id, devnum)) ||
		       (tail && sscanf(tail, " - %*s %4095s", source) == 1 && path_is_devnum(source, devnum));
		line = next;
	}
	free(mi);
	char *swaps = read_text_file("/proc/swaps", 1 << 16);
	for (char *line = swaps; line && *line && !used;) {
		char *next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		char dev[PATH_MAX];
		used = sscanf(line, "%4095s", dev) == 1 && path_is_devnum(dev, devnum);
		line = next;
	}
	free(swaps);
	return used;
}

/*
 * Mounted, used as swap, or under a device-mapper stack that is. A stack
 * that is set up but not mounted (recovery after decrypting, then unmounting
 * Data) is not a user: nothing writes through it.
 */
static bool sys_busy(const char *sysname, int depth)
{
	char devnum[32];
	if (depth > 8 || !sys_devnum(sysname, devnum, sizeof(devnum)))
		return false;
	if (devnum_in_use(devnum))
		return true;
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/sys/class/block/%s/holders", sysname);
	DIR *d = opendir(path);
	bool busy = false;
	if (d) {
		struct dirent *e;
		while (!busy && (e = readdir(d)))
			if (e->d_name[0] != '.')
				busy = sys_busy(e->d_name, depth + 1);
		closedir(d);
	}
	return busy;
}

/* Is the partition with this GPT name in use by a mounted filesystem? */
static bool partition_busy(const char *name)
{
	if (!dev_dir_is_phone())
		return false; /* image files: nothing mounts them */
	char real[PATH_MAX];
	if (!realpath(dev_path(name), real))
		return false; /* no node yet (new partition): nothing can use it */
	const char *base = strrchr(real, '/');
	return sys_busy(base ? base + 1 : real, 0);
}

static int zero_range(int fd, u64 off, u64 len)
{
	enum { CHUNK = 4 << 20 };
	u8 *z = xcalloc(1, CHUNK);
	int r = EXIT_OK;
	for (u64 pos = 0; pos < len && !r; pos += CHUNK)
		r = io_pwrite(fd, z, (size_t)MIN((u64)CHUNK, len - pos), off + pos) ? EXIT_IO : EXIT_OK;
	free(z);
	return r;
}

/* Blank a partition's start so its ROM formats it on the next boot, as after fastboot -w. */
static int wipe_head(const struct gpt *g, const struct gpt_entry *e, u64 bytes)
{
	return zero_range(g->fd, e->first_lba * g->lbs, MIN(bytes, gpt_entry_bytes(g, e)));
}

/*
 * Tells the running kernel about the new table so the partitions appear
 * without a reboot: a full reread when nothing on the disk is in use, else
 * one change at a time (BLKPG works while other partitions are mounted).
 */
struct pchange {
	int index; /* GPT entry; the kernel numbers partitions index + 1 */
	bool remove;
};

static bool blkpg_apply(int fd, const struct gpt *g, const struct pchange *ch, int n)
{
	for (int i = 0; i < n; i++) {
		const struct gpt_entry *e = &g->e[ch[i].index];
		struct blkpg_partition bp = {.pno = ch[i].index + 1};
		struct blkpg_ioctl_arg a = {.datalen = sizeof(bp), .data = &bp};
		if (ch[i].remove) {
			a.op = BLKPG_DEL_PARTITION;
			if (ioctl(fd, BLKPG, &a) && errno != ENXIO)
				return false;
			continue;
		}
		bp.start = (long long)(e->first_lba * g->lbs);
		bp.length = (long long)((e->last_lba - e->first_lba + 1) * g->lbs);
		/* The kernel's PARTNAME: ueventd makes the by-name link from it, which partition_busy follows. */
		snprintf(bp.volname, sizeof(bp.volname), "%s", e->name);
		a.op = BLKPG_RESIZE_PARTITION;
		if (!ioctl(fd, BLKPG, &a))
			continue;
		a.op = BLKPG_ADD_PARTITION;
		if (ioctl(fd, BLKPG, &a))
			return false;
	}
	return true;
}

static void kernel_update(int fd, const struct gpt *g, const struct pchange *ch, int n)
{
	struct stat st;
	if (fstat(fd, &st) || !S_ISBLK(st.st_mode))
		return; /* an image file: there is no kernel view to update */
	if (!ioctl(fd, BLKRRPART) || blkpg_apply(fd, g, ch, n))
		out("the kernel sees the new partitions");
	else
		out("reboot (to recovery or Android) so the kernel sees the new partitions");
}

/* Parked, not half written: the images stay whole, only the data under them changes. */
static int mark_unbootable(int slot)
{
	struct bctl b;
	int r = bctl_read(&b);
	if (!r)
		r = bctl_check(&b, false);
	if (r)
		return r;
	bctl_set_parked(&b, slot);
	return bctl_write(&b);
}

static int busy_refusal(const char *extra)
{
	const char *names[] = {"userdata", "metadata", TWIN_DATA, TWIN_META, extra};
	for (size_t i = 0; i < ARRAY_SIZE(names); i++)
		if (names[i] && partition_busy(names[i])) {
			msg("%s is mounted; run this from recovery with Data and Metadata unmounted", names[i]);
			return EXIT_REFUSED;
		}
	return EXIT_OK;
}

static int merge_refusal(void)
{
	struct bctl b;
	int r = bctl_read(&b);
	if (!r)
		r = bctl_check(&b, false);
	if (!r && bctl_merge_pending(&b)) {
		msg("a system update is still merging; boot Android once to finish it first");
		r = EXIT_REFUSED;
	}
	return r;
}

/* ---- copies of the partition table ---- */

/* Where a copy goes when no directory is named: somewhere writable that is not the disk being changed. */
static const char *default_table_dir(void)
{
	if (!dev_dir_is_phone())
		return g_dev_dir;
	static const char *const dirs[] = {"/tmp", "/data/local/tmp"};
	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		struct stat st;
		if (!stat(dirs[i], &st) && S_ISDIR(st.st_mode) && !access(dirs[i], W_OK))
			return dirs[i];
	}
	return NULL;
}

/* Writes the table area to a new file in dir and reads it back. */
static int save_area(const u8 *area, size_t len, const char *disk, const char *dir, char *path, size_t cap)
{
	char stamp[32];
	time_t now = time(NULL);
	struct tm tm;
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime_r(&now, &tm));
	const char *base = strrchr(disk, '/');
	base = base ? base + 1 : disk;
	int fd = -1;
	for (int n = 1; n < 100 && fd < 0; n++) {
		if (n == 1)
			snprintf(path, cap, "%s/twinslot-gpt-%s-%s.bin", dir, base, stamp);
		else
			snprintf(path, cap, "%s/twinslot-gpt-%s-%s-%d.bin", dir, base, stamp, n);
		fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0 && errno != EEXIST)
			break;
	}
	if (fd < 0) {
		msg("cannot save the partition table in %s: %s", dir, strerror(errno));
		return EXIT_IO;
	}
	int r = io_pwrite(fd, area, len, 0) || io_sync(fd) ? EXIT_IO : EXIT_OK;
	if (!r) {
		io_uncache(fd, 0, len);
		u8 *check = xmalloc(len);
		if (io_pread(fd, check, len, 0) || memcmp(check, area, len))
			r = EXIT_IO;
		free(check);
	}
	close(fd);
	if (r) {
		msg("the copy of the partition table in %s did not read back", dir);
		unlink(path);
	}
	return r;
}

/*
 * Whether a copy in dir outlives a power cut that leaves the disk without a
 * table: not in RAM (recovery's /tmp is tmpfs) and not on that disk itself
 * (/sdcard, /data). Unknown counts as no.
 */
static bool survives_off_disk(const char *dir, const char *disk)
{
	if (!dev_dir_is_phone())
		return true; /* image files: nothing loses power */
	struct statfs sf;
	struct stat st;
	char name[64];
	if (statfs(dir, &sf) || sf.f_type == TMPFS_MAGIC || sf.f_type == RAMFS_MAGIC || stat(dir, &st) ||
	    !backing_disk(st.st_dev, name, sizeof(name)))
		return false;
	const char *base = strrchr(disk, '/');
	return strcmp(name, base ? base + 1 : disk) != 0;
}

/*
 * Copies the table as it is now to --table-dir and the default directory.
 * Returns the number of copies; each is listed on stdout as table_copy=PATH.
 * *kept counts those that survive a power cut (survives_off_disk).
 */
static int save_table_copies(const struct gpt *g, const char *disk, int *kept)
{
	u8 *area;
	size_t len;
	if (gpt_primary_area(g, &area, &len))
		return 0;
	const char *dirs[] = {g_table_dir, default_table_dir()};
	int copies = 0;
	for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
		char path[PATH_MAX];
		if (!dirs[i] || (i == 1 && dirs[0] && !strcmp(dirs[0], dirs[1])))
			continue;
		if (!save_area(area, len, disk, dirs[i], path, sizeof(path))) {
			out("table_copy=%s", path);
			copies++;
			if (survives_off_disk(dirs[i], disk))
				(*kept)++;
		}
	}
	free(area);
	return copies;
}

/*
 * --kept-copy: a copy held off the phone (the zip's table.bin) counts if it is this very table.
 * --kept-copy-earlier: an earlier step of this same zip changed the table (remove, then setup),
 * so the copy is from before that step; it counts if it is a sound table for this disk.
 */
static bool kept_copy_matches(const struct gpt *g, const u8 *area, size_t len)
{
	if (!g_kept_copy)
		return false;
	int fd = open(g_kept_copy, O_RDONLY | O_CLOEXEC);
	u64 n, disk_bytes;
	bool same = false, before = false;
	if (fd >= 0 && !io_size(fd, &n) && n && n <= len * 4) {
		u8 *buf = xmalloc(n);
		if (!io_pread(fd, buf, n, 0)) {
			same = n == len && !memcmp(buf, area, len);
			u32 lbs;
			u8 guid[16];
			if (!same && g_kept_copy_earlier && !io_size(g->fd, &disk_bytes)) {
				int q = quiet_begin();
				before = gpt_area_check(buf, n, disk_bytes, &lbs, guid) == EXIT_OK && lbs == g->lbs &&
					 !memcmp(guid, g->disk_guid, 16);
				quiet_end(q);
			}
		}
		free(buf);
	}
	if (fd >= 0)
		close(fd);
	if (same)
		out("the copy of the table in this zip (table.bin) is the table as it is: keep the zip");
	else if (before)
		out("the copy of the table in this zip (table.bin) is the table from before this zip: keep the zip");
	else
		msg("the copy of the table in this zip does not match the table now (made before another change?)");
	return same || before;
}

/*
 * Before the table changes: a disk without a sound backup table (MediaTek has
 * none) has nothing to fall back on if power fails mid-write, so a copy that
 * survives that is required: on an SD card or USB stick, or held off the phone.
 * *area gets the table as it is, for write_table_or_undo.
 */
static int table_copies_before_write(const struct gpt *g, const char *disk, bool force, u8 **area, size_t *len)
{
	int r = gpt_primary_area(g, area, len);
	if (r)
		return r;
	int kept = 0;
	int copies = save_table_copies(g, disk, &kept);
	if (kept_copy_matches(g, *area, *len))
		kept++;
	if (kept || (g->has_backup && !g->backup_stale))
		return EXIT_OK;
	const char *why = copies ? "every copy of it is in RAM or on this same disk, so a power cut mid-write "
				   "would leave nothing to restore"
				 : "no copy of it could be saved";
	if (force) {
		msg("warning: this disk keeps no sound backup partition table and %s; going on (--force)", why);
		return EXIT_OK;
	}
	msg("this disk keeps no sound backup partition table and %s; refusing", why);
	msg("save one to an SD card or USB stick (--table-dir DIR), or flash the zip from one");
	free(*area);
	*area = NULL;
	return EXIT_REFUSED;
}

/* gpt_write; if it fails while the phone is on, the table read before (area) goes back at once. */
static int write_table_or_undo(struct gpt *g, const char *disk, const u8 *area, size_t len)
{
	int r = gpt_write(g);
	if (r && area) {
		msg("writing the new partition table failed; writing the old one back");
		if (gpt_area_write(g->fd, area, len, g->lbs))
			msg("the old table could not be written back either: before rebooting, run "
			    "'twinslot --disk %s table-restore COPY' with a table_copy above", disk);
		else
			msg("the old partition table is back; nothing else was changed");
	}
	return r;
}

int twin_table_save(const char *dir)
{
	char disk[256];
	int r = twin_find_disk(disk, sizeof(disk));
	if (r)
		return r;
	int fd = io_open(disk, false);
	if (fd < 0)
		return EXIT_IO;
	struct gpt g;
	r = gpt_read(fd, &g);
	if (!r) {
		const char *where = dir ? dir : default_table_dir();
		u8 *area;
		size_t len;
		char path[PATH_MAX];
		if (!where) {
			msg("no writable directory for the copy; name one");
			r = EXIT_IO;
		} else if (!(r = gpt_primary_area(&g, &area, &len))) {
			if (!(r = save_area(area, len, disk, where, path, sizeof(path))))
				out("table_copy=%s", path);
			free(area);
		}
		gpt_free(&g);
	}
	close(fd);
	return r;
}

int twin_table_restore(const char *file, bool force)
{
	char disk[256];
	int r;
	int ffd = io_open(file, false);
	if (ffd < 0)
		return EXIT_IO;
	u64 flen;
	if (io_size(ffd, &flen) || flen > GPT_AREA_MAX) {
		close(ffd);
		msg("%s is not a saved partition table", file);
		return EXIT_BAD_INPUT;
	}
	u8 *area = xmalloc((size_t)flen);
	r = io_pread(ffd, area, (size_t)flen, 0) ? EXIT_IO : EXIT_OK;
	close(ffd);
	if (!r)
		r = restore_disk(file, area, (size_t)flen, disk, sizeof(disk));
	int fd = r ? -1 : io_open(disk, true);
	if (fd < 0) {
		free(area);
		return EXIT_IO;
	}
	u64 disk_bytes;
	u32 lbs;
	u8 saved_guid[16];
	struct gpt g;
	if ((r = io_size(fd, &disk_bytes) ? EXIT_IO : EXIT_OK) ||
	    (r = gpt_area_check(area, (size_t)flen, disk_bytes, &lbs, saved_guid)))
		goto done;
	/* An intact table is changed with remove, not overwritten; a broken one is what this is for. */
	if (gpt_read(fd, &g) == EXIT_OK) {
		bool same_disk = !memcmp(g.disk_guid, saved_guid, 16);
		bool same_lbs = g.lbs == lbs;
		gpt_free(&g);
		if (!same_disk || !same_lbs) {
			msg("%s was saved from another disk; refusing", file);
			r = EXIT_REFUSED;
			goto done;
		}
		if (!force) {
			msg("the partition table on %s is intact; 'remove' undoes setup (--force restores anyway)", disk);
			r = EXIT_REFUSED;
			goto done;
		}
	}
	if ((r = busy_refusal(NULL)))
		goto done;
	out("restoring the partition table of %s from %s", disk, file);
	if ((r = gpt_area_write(fd, area, (size_t)flen, lbs)))
		goto done;
	/* A disk that keeps a backup table gets it rewritten from the restored primary. */
	if ((r = gpt_read(fd, &g)) == EXIT_OK) {
		if (g.has_backup && g.backup_stale)
			r = gpt_write(&g);
		gpt_free(&g);
	}
	if (!r) {
		struct stat st;
		if (!fstat(fd, &st) && S_ISBLK(st.st_mode) && ioctl(fd, BLKRRPART))
			out("reboot so the kernel sees the restored partitions");
		out("restored");
	}
done:
	free(area);
	close(fd);
	return r;
}

/*
 * Partitions known to be spare on some phones, which can become the second
 * ROM's metadata when the partition table has no room for two new entries.
 * tranfs: Transsion (Tecno, Infinix, itel); an empty ext4 that AOSP-based ROMs
 * never mount.
 */
static const char *const spare_meta[] = {"tranfs"};

/* Never adopted, whatever the user asks. */
static bool essential(const char *name)
{
	static const char *const keep[] = {"userdata", "metadata", "super", "misc", "flashinfo", "persist",
					   "nvdata", "nvcfg", "nvram", "proinfo", "protect1", "protect2",
					   "seccfg", "frp", "expdb", "boot_para", "para", "otp", "efuse",
					   "sec1", "dram_para", "md_udc", TWIN_DATA, TWIN_META};
	size_t n = strlen(name);
	if (n > 2 && name[n - 2] == '_' && (name[n - 1] == 'a' || name[n - 1] == 'b'))
		return true;
	for (size_t i = 0; i < ARRAY_SIZE(keep); i++)
		if (!strcmp(name, keep[i]))
			return true;
	return false;
}

int twin_setup(u64 data_bytes, u64 meta_bytes, const char *meta_from, int owner, bool apply, bool force)
{
	char disk[256];
	int r = twin_find_disk(disk, sizeof(disk));
	if (r)
		return r;
	int fd = io_open(disk, apply);
	if (fd < 0)
		return EXIT_IO;
	struct gpt g;
	u8 *old_area = NULL;
	size_t old_len = 0;
	r = gpt_read(fd, &g);
	if (r) {
		close(fd);
		return r;
	}

	int ud = gpt_find(&g, "userdata"), md = gpt_find(&g, "metadata"), adopt = -1;
	if (ud < 0) {
		msg("%s has no userdata partition", disk);
		r = EXIT_BAD_INPUT;
		goto done;
	}
	if (gpt_find(&g, TWIN_DATA) >= 0 || gpt_find(&g, TWIN_META) >= 0) {
		msg("dual data is already set up (run 'remove' first to change it)");
		r = EXIT_REFUSED;
		goto done;
	}
	if (meta_from && !strcmp(meta_from, "auto")) {
		meta_from = NULL;
		for (size_t i = 0; i < ARRAY_SIZE(spare_meta) && !meta_from; i++)
			if (gpt_find(&g, spare_meta[i]) >= 0)
				meta_from = spare_meta[i];
	}
	if (meta_from) {
		adopt = gpt_find(&g, meta_from);
		if (adopt < 0 || essential(meta_from) || gpt_entry_bytes(&g, &g.e[adopt]) < 16 * MiB) {
			msg("%s cannot become the second ROM's metadata partition", meta_from);
			r = EXIT_REFUSED;
			goto done;
		}
	}
	if (!meta_bytes)
		meta_bytes = md >= 0 ? gpt_entry_bytes(&g, &g.e[md]) : DEFAULT_META;
	meta_bytes = MAX(meta_bytes, 16 * MiB);

	struct gpt_entry *u = &g.e[ud];
	const u64 align = MiB / g.lbs;
	const u64 data_lbas = ALIGN_UP(data_bytes, MiB) / g.lbs;
	const u64 meta_lbas = adopt >= 0 ? 0 : ALIGN_UP(meta_bytes, MiB) / g.lbs;
	if (data_lbas + meta_lbas + MIN_USERDATA / g.lbs > u->last_lba - u->first_lba + 1) {
		msg("userdata is %llu GiB; it cannot give %llu GiB and keep %llu GiB",
		    (unsigned long long)(gpt_entry_bytes(&g, u) / GiB), (unsigned long long)(data_bytes / GiB),
		    (unsigned long long)(MIN_USERDATA / GiB));
		r = EXIT_REFUSED;
		goto done;
	}
	u64 tdata_first = (u->last_lba + 1 - data_lbas) / align * align;
	u64 tmeta_first = (tdata_first - meta_lbas) / align * align;
	struct gpt_entry tdata = *u, tmeta = adopt >= 0 ? g.e[adopt] : md >= 0 ? g.e[md] : *u;
	tdata.first_lba = tdata_first;
	tdata.last_lba = u->last_lba;
	gpt_set_name(&tdata, TWIN_DATA);
	gpt_random_guid(tdata.guid);
	if (adopt >= 0) {
		/* Same place and size; the old name is kept after the new one, for 'remove'. */
		gpt_set_name_stash(&tmeta, TWIN_META, meta_from);
	} else {
		tmeta.first_lba = tmeta_first;
		tmeta.last_lba = tdata_first - 1;
		gpt_set_name(&tmeta, TWIN_META);
		gpt_random_guid(tmeta.guid);
	}
	tdata.attrs = owner ? TWIN_ATTR_OWNER_B : 0;
	tmeta.attrs = (tmeta.attrs & ~TWIN_ATTR_OWNER_B) | tdata.attrs;
	u64 new_ud_last = (adopt >= 0 ? tdata_first : tmeta_first) - 1;

	/* Entries: one new one (adopting) or two; the table may grow into its spare room. */
	u32 num_before = g.num_entries;
	int slot_meta = adopt, slot_data;
	if (slot_meta < 0 && (slot_meta = gpt_new_entry(&g)) >= 0)
		g.e[slot_meta] = tmeta;
	slot_data = slot_meta >= 0 ? gpt_new_entry(&g) : -1;
	if (slot_data < 0) {
		msg("the partition table has no room for %s new entries%s", adopt >= 0 ? "one more" : "two",
		    adopt >= 0 ? "" : "; a spare partition can hold the metadata instead (--meta-from NAME)");
		r = EXIT_REFUSED;
		goto done;
	}
	if (g.num_entries > num_before) {
		/* The table grew: remember its old size so 'remove' gives back the exact table. */
		char count[16];
		snprintf(count, sizeof(count), "n%u", num_before);
		gpt_set_name_stash(&tdata, TWIN_DATA, count);
	}
	g.e[slot_meta] = tmeta;
	g.e[slot_data] = tdata;

	out("disk=%s", disk);
	out("table=%u entries%s", g.num_entries, g.has_backup ? "" : ", no backup copy (kept that way)");
	out("userdata=%llu MiB -> %llu MiB", (unsigned long long)(gpt_entry_bytes(&g, u) / MiB),
	    (unsigned long long)((new_ud_last - u->first_lba + 1) * g.lbs / MiB));
	out("twinmeta=%llu MiB%s%s", (unsigned long long)(gpt_entry_bytes(&g, &tmeta) / MiB),
	    adopt >= 0 ? ", was " : "", adopt >= 0 ? meta_from : "");
	out("twindata=%llu MiB", (unsigned long long)(gpt_entry_bytes(&g, &tdata) / MiB));
	out("owner=%c", slot_letter(owner));
	if (!apply) {
		out("dry run: nothing written (add --apply)");
		goto done;
	}

	if ((r = busy_refusal(meta_from)) || (r = merge_refusal()))
		goto done;
	/* The copy is read from the disk, so it is the table before this change. */
	if ((r = table_copies_before_write(&g, disk, force, &old_area, &old_len)))
		goto done;
	u->last_lba = new_ud_last;

	/* The owner slot's ROM, if any, still points at userdata: it must not boot now. */
	if ((r = mark_unbootable(owner)))
		goto done;
	progress(100, "writing the partition table");
	if ((r = write_table_or_undo(&g, disk, old_area, old_len)))
		goto done;
	struct pchange ch[] = {{ud, false}, {slot_meta, false}, {slot_data, false}};
	kernel_update(fd, &g, ch, 3);
	progress(500, "blanking data partitions");
	r = wipe_head(&g, u, DATA_HEAD_WIPE);
	if (!r && md >= 0)
		r = wipe_head(&g, &g.e[md], gpt_entry_bytes(&g, &g.e[md]));
	if (!r)
		r = wipe_head(&g, &g.e[slot_meta], gpt_entry_bytes(&g, &g.e[slot_meta]));
	if (!r)
		r = wipe_head(&g, &g.e[slot_data], DATA_HEAD_WIPE);
	if (!r)
		r = io_sync(fd) ? EXIT_IO : EXIT_OK;
	progress(1000, "done");
	if (!r)
		out("setup done: both ROMs start with empty data. Install the second ROM into slot %c.",
		    slot_letter(owner));
done:
	free(old_area);
	gpt_free(&g);
	close(fd);
	return r;
}

int twin_remove(bool apply, bool force)
{
	char disk[256];
	int r = twin_find_disk(disk, sizeof(disk));
	if (r)
		return r;
	int fd = io_open(disk, apply);
	if (fd < 0)
		return EXIT_IO;
	struct gpt g;
	u8 *old_area = NULL;
	size_t old_len = 0;
	r = gpt_read(fd, &g);
	if (r) {
		close(fd);
		return r;
	}
	int ud = gpt_find(&g, "userdata"), md = gpt_find(&g, "metadata");
	int td = gpt_find(&g, TWIN_DATA), tm = gpt_find(&g, TWIN_META);
	if (ud < 0 || td < 0 || tm < 0) {
		msg("dual data is not set up");
		r = EXIT_REFUSED;
		goto done;
	}
	/* Carved: userdata | twinmeta | twindata. Adopted: userdata | twindata, twinmeta elsewhere. */
	bool carved = g.e[tm].first_lba == g.e[ud].last_lba + 1 && g.e[td].first_lba == g.e[tm].last_lba + 1;
	char old_name[GPT_NAME_CHARS + 1];
	gpt_name_stash(&g.e[tm], old_name, sizeof(old_name));
	if (!carved && (g.e[td].first_lba != g.e[ud].last_lba + 1 || !old_name[0])) {
		msg("twinmeta and twindata are not where setup put them; not merging them back automatically");
		r = EXIT_REFUSED;
		goto done;
	}
	int owner = g.e[td].attrs & TWIN_ATTR_OWNER_B ? 1 : 0;
	out("userdata=%llu MiB -> %llu MiB", (unsigned long long)(gpt_entry_bytes(&g, &g.e[ud]) / MiB),
	    (unsigned long long)((g.e[td].last_lba - g.e[ud].first_lba + 1) * g.lbs / MiB));
	if (!carved)
		out("twinmeta becomes %s again (blank)", old_name);
	out("slot %c's ROM is pointed back at userdata and marked unbootable", slot_letter(owner));
	if (!apply) {
		out("dry run: nothing written (add --apply)");
		goto done;
	}
	if ((r = busy_refusal(NULL)) || (r = merge_refusal()) ||
	    (r = table_copies_before_write(&g, disk, force, &old_area, &old_len)) || (r = mark_unbootable(owner)))
		goto done;
	/* Left bound to twindata, the owner's ROM would name partitions that are about to go. An empty
	 * slot has no fstab to rebind; that is not a reason to keep the split. */
	if (slot_rebind(owner, BIND_PRIMARY)) {
		msg("slot %c's ROM still names the twin data; install a ROM there before booting it", slot_letter(owner));
		/* Its images may be half edited now: switch must not bring it back. */
		struct bctl b;
		if (!bctl_read(&b) && !bctl_check(&b, false)) {
			bctl_set_unbootable(&b, owner);
			bctl_write(&b);
		}
	}
	char count[GPT_NAME_CHARS + 1];
	gpt_name_stash(&g.e[td], count, sizeof(count));
	g.e[ud].last_lba = g.e[td].last_lba;
	memset(&g.e[td], 0, sizeof(g.e[td]));
	if (carved) {
		memset(&g.e[tm], 0, sizeof(g.e[tm]));
	} else {
		gpt_set_name(&g.e[tm], old_name);
		g.e[tm].attrs &= ~TWIN_ATTR_OWNER_B;
	}
	/* Setup grew the table: shrink it back if nothing else moved in since. */
	unsigned long before = count[0] == 'n' ? strtoul(count + 1, NULL, 10) : 0;
	if (before && before < g.num_entries) {
		bool spare = true;
		for (u32 i = (u32)before; i < g.num_entries; i++)
			spare &= !gpt_entry_used(&g.e[i]);
		if (spare)
			g.num_entries = (u32)before;
	}
	if ((r = write_table_or_undo(&g, disk, old_area, old_len)))
		goto done;
	struct pchange ch[] = {{td, true}, {tm, carved}, {ud, false}};
	kernel_update(fd, &g, ch, 3);
	r = wipe_head(&g, &g.e[ud], DATA_HEAD_WIPE);
	if (!r && md >= 0)
		r = wipe_head(&g, &g.e[md], gpt_entry_bytes(&g, &g.e[md]));
	if (!r && !carved)
		r = wipe_head(&g, &g.e[tm], gpt_entry_bytes(&g, &g.e[tm]));
	if (!r)
		r = io_sync(fd) ? EXIT_IO : EXIT_OK;
	if (!r)
		out("removed: userdata has all its space back and starts empty");
done:
	free(old_area);
	gpt_free(&g);
	close(fd);
	return r;
}

int twin_wipe_slot_data(const struct twin_state *st, int slot)
{
	bool twin = slot_expected_binding(st, slot) == BIND_TWIN;
	const char *names[] = {twin ? TWIN_DATA : "userdata", twin ? TWIN_META : "metadata"};
	for (size_t i = 0; i < ARRAY_SIZE(names); i++)
		if (partition_busy(names[i])) {
			msg("%s is mounted; cannot wipe the data of a running ROM", names[i]);
			return EXIT_REFUSED;
		}

	/*
	 * Through the partition table, not /dev/block/by-name: right after setup
	 * the new partitions may have no device node until a reboot.
	 */
	int fd = io_open(st->disk, true);
	if (fd < 0)
		return EXIT_IO;
	struct gpt g;
	int r = gpt_read(fd, &g);
	for (size_t i = 0; i < ARRAY_SIZE(names) && !r; i++) {
		int k = gpt_find(&g, names[i]);
		if (k < 0) {
			if (i == 0) {
				msg("%s is not in the partition table", names[i]);
				r = EXIT_BAD_INPUT;
			}
			continue;
		}
		/* The filesystem headers sit at the data partition's start; metadata is small. */
		r = wipe_head(&g, &g.e[k], i == 0 ? DATA_HEAD_WIPE : gpt_entry_bytes(&g, &g.e[k]));
		if (!r)
			out("wiped %s", names[i]);
	}
	if (!r)
		r = io_sync(fd) ? EXIT_IO : EXIT_OK;
	gpt_free(&g);
	close(fd);
	return r;
}
