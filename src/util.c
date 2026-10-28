#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/fs.h>
#endif

const char *g_dev_dir = DEV_DIR_PHONE;
static bool dev_dir_given;

void msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("twinslot: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void out(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	putchar('\n');
	va_end(ap);
}

void progress(unsigned permille, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("progress %u ", MIN(permille, 1000u));
	vprintf(fmt, ap);
	putchar('\n');
	fflush(stdout);
	va_end(ap);
}

static void oom(void)
{
	msg("out of memory");
	exit(EXIT_IO);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		oom();
	return p;
}

void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p)
		oom();
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		oom();
	return p;
}

char *xstrdup(const char *s)
{
	char *p = strdup(s);
	if (!p)
		oom();
	return p;
}

int io_open(const char *path, bool write)
{
	int fd = open(path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	if (fd < 0 && write && (errno == EACCES || errno == EROFS || errno == EPERM)) {
		/* Android marks some block devices read-only; root may lift that. */
		int ro = open(path, O_RDONLY | O_CLOEXEC), zero = 0;
		struct stat st;
		if (ro >= 0 && !fstat(ro, &st) && S_ISBLK(st.st_mode) && !ioctl(ro, BLKROSET, &zero))
			fd = open(path, O_RDWR | O_CLOEXEC);
		if (ro >= 0)
			close(ro);
		if (fd < 0)
			errno = EACCES;
	}
	if (fd < 0)
		msg("cannot open %s%s: %s", path, write ? " for writing" : "", strerror(errno));
	return fd;
}

int io_pread(int fd, void *buf, size_t n, u64 off)
{
	u8 *p = buf;
	while (n) {
		ssize_t r = pread(fd, p, n, (off_t)off);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			msg("read failed at offset %llu: %s", (unsigned long long)off,
			    r ? strerror(errno) : "past the end");
			return -1;
		}
		p += r;
		off += (u64)r;
		n -= (size_t)r;
	}
	return 0;
}

int io_pwrite(int fd, const void *buf, size_t n, u64 off)
{
	const u8 *p = buf;
	while (n) {
		ssize_t r = pwrite(fd, p, n, (off_t)off);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			msg("write failed at offset %llu: %s", (unsigned long long)off,
			    r ? strerror(errno) : "no space");
			return -1;
		}
		p += r;
		off += (u64)r;
		n -= (size_t)r;
	}
	return 0;
}

int io_sync(int fd)
{
	if (fsync(fd) < 0 && errno != EINVAL) {
		msg("fsync failed: %s", strerror(errno));
		return -1;
	}
	return 0;
}

void io_uncache(int fd, u64 off, size_t n)
{
	/* After io_sync the pages are clean, so dropping them is safe; the next read comes from the medium. */
	posix_fadvise(fd, (off_t)off, (off_t)n, POSIX_FADV_DONTNEED);
}

int io_size(int fd, u64 *size)
{
	struct stat st;
	if (fstat(fd, &st) < 0)
		return -1;
	if (S_ISREG(st.st_mode)) {
		*size = (u64)st.st_size;
		return 0;
	}
#ifdef BLKGETSIZE64
	if (S_ISBLK(st.st_mode) && ioctl(fd, BLKGETSIZE64, size) == 0)
		return 0;
#endif
	off_t end = lseek(fd, 0, SEEK_END);
	if (end < 0)
		return -1;
	*size = (u64)end;
	return 0;
}

char *read_text_file(const char *path, size_t max)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	char *buf = xmalloc(max + 1);
	size_t len = 0;
	ssize_t r;
	while (len < max && (r = read(fd, buf + len, max - len)) > 0)
		len += (size_t)r;
	close(fd);
	buf[len] = 0;
	return buf;
}

const char *dev_path(const char *name)
{
	static char path[4096];
	snprintf(path, sizeof(path), "%s/%s", g_dev_dir, name);
	return path;
}

bool dev_exists(const char *name)
{
	return access(dev_path(name), F_OK) == 0;
}

void dev_dir_set(const char *dir)
{
	g_dev_dir = dir;
	dev_dir_given = strcmp(dir, DEV_DIR_PHONE) != 0;
}

void dev_dir_find(void)
{
	static char found[PATH_MAX];
	if (dev_dir_given || !access(DEV_DIR_PHONE "/userdata", F_OK))
		return;
	/* Some recoveries and older MediaTek ROMs only make the platform links. */
	static const char *const patterns[] = {"/dev/block/platform/bootdevice/by-name", "/dev/block/bootdevice/by-name",
					       "/dev/block/platform/*/by-name", "/dev/block/platform/*/*/by-name"};
	for (size_t i = 0; i < ARRAY_SIZE(patterns); i++) {
		glob_t g;
		if (glob(patterns[i], 0, NULL, &g))
			continue;
		for (size_t k = 0; k < g.gl_pathc; k++) {
			char probe[PATH_MAX];
			snprintf(probe, sizeof(probe), "%s/userdata", g.gl_pathv[k]);
			if (!access(probe, F_OK)) {
				snprintf(found, sizeof(found), "%s", g.gl_pathv[k]);
				g_dev_dir = found;
				globfree(&g);
				return;
			}
		}
		globfree(&g);
	}
}

bool dev_dir_is_phone(void)
{
	return !dev_dir_given;
}

bool in_recovery(void)
{
	/* For tests of the running-ROM refusals on image files. */
	const char *forced = getenv("TWINSLOT_IN_RECOVERY");
	if (forced && (forced[0] == '0' || forced[0] == '1') && !forced[1])
		return forced[0] == '1';
	if (!dev_dir_is_phone())
		return true; /* image files: nothing is running from them */
	const char *files[] = {"/proc/bootconfig", "/proc/cmdline"};
	for (size_t f = 0; f < ARRAY_SIZE(files); f++) {
		char *buf = read_text_file(files[f], 65536);
		/* The last one counts: bootloaders override a key by appending it again. */
		char *p = NULL;
		for (char *q = buf ? strstr(buf, "androidboot.force_normal_boot") : NULL; q;
		     q = strstr(q + 1, "androidboot.force_normal_boot"))
			p = q;
		bool normal = false;
		if (p) {
			/* cmdline: key=1; bootconfig: key = "1" */
			p += strlen("androidboot.force_normal_boot");
			p += strspn(p, " =\"");
			normal = *p == '1';
		}
		free(buf);
		if (normal)
			return false;
	}
	/* Recovery ramdisks carry the recovery binary at the root's /system/bin. */
	return !access("/system/bin/recovery", F_OK) || !access("/sbin/recovery", F_OK);
}

int detect_current_slot(void)
{
	static const char key[] = "androidboot.slot_suffix";
	const char *files[] = {"/proc/bootconfig", "/proc/cmdline"};
	const char *forced = getenv("TWINSLOT_CURRENT_SLOT");

	if (forced && (forced[0] == 'a' || forced[0] == 'b') && !forced[1])
		return forced[0] - 'a';
	for (size_t f = 0; f < ARRAY_SIZE(files); f++) {
		char *buf = read_text_file(files[f], 65536);
		if (!buf)
			continue;
		/* cmdline: key=_b; bootconfig: key = "_b"; the underscore may be missing. The last one counts. */
		int slot = -1;
		for (char *p = strstr(buf, key); p; p = strstr(p + 1, key)) {
			char *q = p + sizeof(key) - 1;
			q += strspn(q, " =\"");
			if (*q == '_')
				q++;
			if ((q[0] == 'a' || q[0] == 'b') && (!q[1] || strchr(" \t\n\"", q[1])))
				slot = q[0] - 'a';
		}
		free(buf);
		if (slot >= 0)
			return slot;
	}
	return -1;
}

void hex(char *outbuf, const u8 *in, size_t n)
{
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		outbuf[2 * i] = digits[in[i] >> 4];
		outbuf[2 * i + 1] = digits[in[i] & 15];
	}
	outbuf[2 * n] = 0;
}
