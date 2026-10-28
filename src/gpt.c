#include "gpt.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hash.h"

#define GPT_SIGNATURE "EFI PART"
#define GPT_HEADER_SIZE 92
#define GPT_ENTRY_MIN 128

static bool header_valid(u8 *h, u32 lbs)
{
	if (memcmp(h, GPT_SIGNATURE, 8))
		return false;
	u32 size = get_le32(h + 12);
	if (size < GPT_HEADER_SIZE || size > lbs)
		return false;
	u32 want = get_le32(h + 16);
	put_le32(h + 16, 0);
	u32 got = crc32(h, size);
	put_le32(h + 16, want);
	return want == got;
}

static int read_header(int fd, u32 lbs, u64 lba, u8 *h)
{
	if (io_pread(fd, h, lbs, lba * lbs))
		return EXIT_IO;
	return header_valid(h, lbs) ? EXIT_OK : EXIT_BAD_INPUT;
}

/* The entry array, as whole blocks; *capacity = entries those blocks hold. */
static u8 *read_entries(int fd, u32 lbs, const u8 *h, u32 *capacity, size_t *raw_bytes)
{
	u64 lba = get_le64(h + 72);
	u32 num = get_le32(h + 80), size = get_le32(h + 84);
	if (num == 0 || num > 1024 || size < GPT_ENTRY_MIN || size > 1024 || size % 8)
		return NULL;
	size_t bytes = (size_t)num * size, blocks = (bytes + lbs - 1) / lbs;
	u8 *buf = xmalloc(blocks * lbs);
	if (io_pread(fd, buf, blocks * lbs, lba * lbs) || crc32(buf, bytes) != get_le32(h + 88)) {
		free(buf);
		return NULL;
	}
	*capacity = (u32)(blocks * lbs / size);
	if (raw_bytes)
		*raw_bytes = blocks * lbs;
	return buf;
}

static bool all_zero(const u8 *p, size_t n)
{
	while (n--)
		if (*p++)
			return false;
	return true;
}

static void decode_entry(struct gpt_entry *e, const u8 *raw)
{
	memset(e, 0, sizeof(*e));
	memcpy(e->type, raw, 16);
	memcpy(e->guid, raw + 16, 16);
	e->first_lba = get_le64(raw + 32);
	e->last_lba = get_le64(raw + 40);
	e->attrs = get_le64(raw + 48);
	memcpy(e->raw_name, raw + 56, sizeof(e->raw_name));
	for (int i = 0; i < GPT_NAME_CHARS; i++) {
		u16 c = get_le16(raw + 56 + 2 * i);
		e->name[i] = c < 128 ? (char)c : '?';
		if (!c)
			break;
	}
	e->name[GPT_NAME_CHARS] = 0;
}

static void encode_entry(const struct gpt_entry *e, const u8 *old, u8 *raw, u32 size)
{
	struct gpt_entry before;
	decode_entry(&before, old);
	if (!memcmp(&before, e, sizeof(before))) {
		memcpy(raw, old, size); /* unchanged: every byte as read */
		return;
	}
	if (!gpt_entry_used(e)) {
		memset(raw, 0, size);
		return;
	}
	memcpy(raw, old, size);
	memcpy(raw, e->type, 16);
	memcpy(raw + 16, e->guid, 16);
	put_le64(raw + 32, e->first_lba);
	put_le64(raw + 40, e->last_lba);
	put_le64(raw + 48, e->attrs);
	memcpy(raw + 56, e->raw_name, sizeof(e->raw_name));
}

int gpt_read(int fd, struct gpt *g)
{
	static const u32 sizes[] = {4096, 512};
	u8 *ph = NULL, *bh = NULL, *pe = NULL, *be = NULL;
	int r = EXIT_BAD_INPUT;
	u64 disk_bytes;
	u32 cap = 0, bcap = 0;

	memset(g, 0, sizeof(*g));
	g->fd = fd;
	if (io_size(fd, &disk_bytes))
		return EXIT_IO;

	for (size_t i = 0; i < ARRAY_SIZE(sizes) && !ph; i++) {
		u8 *h = xmalloc(sizes[i]);
		if (disk_bytes >= 2ull * sizes[i] && read_header(fd, sizes[i], 1, h) == EXIT_OK) {
			ph = h;
			g->lbs = sizes[i];
		} else {
			free(h);
		}
	}
	if (!ph) {
		msg("no valid primary GPT");
		return EXIT_BAD_INPUT;
	}
	g->disk_lbas = disk_bytes / g->lbs;
	if (get_le64(ph + 24) != 1 || get_le64(ph + 32) != g->disk_lbas - 1) {
		msg("primary GPT does not describe this whole disk");
		goto done;
	}
	pe = read_entries(fd, g->lbs, ph, &cap, &g->raw_bytes);
	if (!pe) {
		msg("primary GPT entries are damaged");
		goto done;
	}
	g->num_entries = get_le32(ph + 80);
	g->entry_size = get_le32(ph + 84);
	g->first_usable = get_le64(ph + 40);
	g->last_usable = get_le64(ph + 48);
	g->primary_entries_lba = get_le64(ph + 72);
	/* The array must not run into the usable area (a malformed header). */
	if (g->primary_entries_lba < 2 ||
	    g->primary_entries_lba + DIV_ROUND_UP((u64)cap * g->entry_size, g->lbs) > g->first_usable) {
		msg("primary GPT entries overlap the partitions");
		goto done;
	}

	bh = xmalloc(g->lbs);
	g->backup_lba = get_le64(ph + 32);
	if (io_pread(fd, bh, g->lbs, g->backup_lba * g->lbs)) {
		r = EXIT_IO;
		goto done;
	}
	if (all_zero(bh, g->lbs)) {
		g->has_backup = false; /* no backup copy kept (common on MediaTek) */
	} else {
		/*
		 * The primary checked out, so it is the table. A backup that is damaged
		 * or differs (power lost between the two writes) is rewritten from it on
		 * the next change, as other GPT tools do.
		 */
		bool header_ok = read_header(fd, g->lbs, g->backup_lba, bh) == EXIT_OK &&
				 get_le64(bh + 24) == g->backup_lba && get_le64(bh + 32) == 1;
		be = header_ok ? read_entries(fd, g->lbs, bh, &bcap, NULL) : NULL;
		if (!be || get_le32(bh + 80) != g->num_entries || get_le32(bh + 84) != g->entry_size ||
		    memcmp(pe, be, (size_t)g->num_entries * g->entry_size)) {
			msg("warning: the backup GPT is damaged or differs from the primary; the primary is used");
			g->backup_stale = true;
		}
		g->has_backup = true;
		g->backup_entries_lba = header_ok ? get_le64(bh + 72) : g->last_usable + 1;
		if (g->backup_entries_lba <= g->last_usable || g->backup_entries_lba >= g->backup_lba) {
			msg("backup GPT entries would overlap the partitions");
			goto done;
		}
		/* A damaged backup is rewritten where the primary's array fits right before its header. */
		u64 need = DIV_ROUND_UP((u64)g->num_entries * g->entry_size, g->lbs);
		if (g->backup_stale && (g->backup_lba - g->backup_entries_lba) * g->lbs / g->entry_size < g->num_entries &&
		    g->backup_lba - need > g->last_usable)
			g->backup_entries_lba = g->backup_lba - need;
		/* Growing must also fit before the backup header. */
		cap = MIN(cap, (u32)((g->backup_lba - g->backup_entries_lba) * g->lbs / g->entry_size));
	}
	/* g->e holds capacity entries and everything indexes up to num_entries. */
	if (cap < g->num_entries) {
		msg("the GPT has no room for its own %u entries", g->num_entries);
		goto done;
	}

	g->capacity = cap;
	memcpy(g->disk_guid, ph + 56, 16);
	g->e = xcalloc(g->capacity, sizeof(*g->e));
	for (u32 i = 0; i < g->capacity; i++)
		decode_entry(&g->e[i], pe + (size_t)i * g->entry_size);
	g->raw = pe;
	g->header = ph;
	pe = ph = NULL;
	r = EXIT_OK;
done:
	free(ph);
	free(bh);
	free(pe);
	free(be);
	return r;
}

/* The primary header as read, with the fields TwinSlot changes updated. */
static void build_header(const struct gpt *g, u8 *h, bool backup, u32 entries_crc)
{
	memcpy(h, g->header, g->lbs);
	if (backup) {
		put_le64(h + 24, g->backup_lba);
		put_le64(h + 32, 1);
		put_le64(h + 72, g->backup_entries_lba);
	}
	put_le32(h + 80, g->num_entries);
	put_le32(h + 88, entries_crc);
	put_le32(h + 16, 0);
	put_le32(h + 16, crc32(h, get_le32(h + 12)));
}

int gpt_write(struct gpt *g)
{
	size_t blocks_bytes = g->raw_bytes;
	size_t bytes = (size_t)g->num_entries * g->entry_size;
	u8 *entries = xmalloc(blocks_bytes);
	memcpy(entries, g->raw, blocks_bytes);
	/* Past num_entries too: an entry dropped off the end must not linger. */
	for (u32 i = 0; i < g->capacity; i++)
		encode_entry(&g->e[i], g->raw + (size_t)i * g->entry_size, entries + (size_t)i * g->entry_size,
			     g->entry_size);
	u32 crc = crc32(entries, bytes);
	u8 *bh = xmalloc(g->lbs), *ph = xmalloc(g->lbs);
	build_header(g, ph, false, crc);

	/* Backup first: if this is cut short the primary still describes the old layout. */
	int r = EXIT_IO;
	/* Each copy: entries, sync, header, sync, so a header never lands before the entries it describes. */
	if (g->has_backup) {
		build_header(g, bh, true, crc);
		if (io_pwrite(g->fd, entries, (size_t)g->capacity * g->entry_size, g->backup_entries_lba * g->lbs) ||
		    io_sync(g->fd) || io_pwrite(g->fd, bh, g->lbs, g->backup_lba * g->lbs) || io_sync(g->fd))
			goto done;
	}
	if (io_pwrite(g->fd, entries, blocks_bytes, g->primary_entries_lba * g->lbs) || io_sync(g->fd))
		goto done;
	/* For tests of the undo: the write stops here, new entries under the old header. */
	if (getenv("TWINSLOT_TEST_CUT_TABLE")) {
		msg("test: table write cut off before the header");
		goto done;
	}
	if (io_pwrite(g->fd, ph, g->lbs, g->lbs) || io_sync(g->fd))
		goto done;
	io_uncache(g->fd, 0, 0);

	struct gpt check;
	if (gpt_read(g->fd, &check) != EXIT_OK) {
		msg("the partition table did not read back correctly");
		goto done;
	}
	/* A smaller count can mean fewer blocks: compare what the new table covers. */
	bool same = check.num_entries == g->num_entries && check.has_backup == g->has_backup && !check.backup_stale &&
		    check.raw_bytes <= blocks_bytes && !memcmp(check.raw, entries, check.raw_bytes) &&
		    !memcmp(check.header, ph, g->lbs);
	gpt_free(&check);
	if (!same) {
		msg("the partition table did not read back as written");
		goto done;
	}
	memcpy(g->raw, entries, blocks_bytes);
	memcpy(g->header, ph, g->lbs);
	r = EXIT_OK;
done:
	free(entries);
	free(bh);
	free(ph);
	return r;
}

int gpt_primary_area(const struct gpt *g, u8 **buf, size_t *len)
{
	u64 n = g->first_usable * g->lbs;
	if (n > GPT_AREA_MAX) {
		msg("the partition table area is unexpectedly large (%llu bytes)", (unsigned long long)n);
		return EXIT_BAD_INPUT;
	}
	*buf = xmalloc((size_t)n);
	*len = (size_t)n;
	if (io_pread(g->fd, *buf, *len, 0)) {
		free(*buf);
		*buf = NULL;
		return EXIT_IO;
	}
	return EXIT_OK;
}

int gpt_area_check(u8 *buf, size_t len, u64 disk_bytes, u32 *lbs_out, u8 disk_guid[16])
{
	static const u32 sizes[] = {4096, 512};
	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		u32 lbs = sizes[i];
		if (len < 3 * (size_t)lbs || len % lbs || !header_valid(buf + lbs, lbs))
			continue;
		const u8 *h = buf + lbs;
		u64 entries_lba = get_le64(h + 72), first_usable = get_le64(h + 40);
		u32 num = get_le32(h + 80), size = get_le32(h + 84);
		if (num == 0 || num > 1024 || size < GPT_ENTRY_MIN || size > 1024 || size % 8 || entries_lba < 2 ||
		    entries_lba * lbs + (u64)num * size > len || first_usable * lbs != len) {
			msg("the saved table is incomplete or malformed");
			return EXIT_BAD_INPUT;
		}
		if (crc32(buf + entries_lba * lbs, (size_t)num * size) != get_le32(h + 88)) {
			msg("the saved table's entries are damaged");
			return EXIT_BAD_INPUT;
		}
		if (get_le64(h + 24) != 1 || get_le64(h + 32) != disk_bytes / lbs - 1) {
			msg("the saved table is for a disk of another size");
			return EXIT_BAD_INPUT;
		}
		*lbs_out = lbs;
		memcpy(disk_guid, h + 56, 16);
		return EXIT_OK;
	}
	msg("no partition table in the saved file");
	return EXIT_BAD_INPUT;
}

int gpt_area_write(int fd, const u8 *buf, size_t len, u32 lbs)
{
	/* Everything but the header, then the header: until the last write the old header (or none) stands. */
	if (io_pwrite(fd, buf, lbs, 0) || io_pwrite(fd, buf + 2 * lbs, len - 2 * lbs, 2 * (u64)lbs) || io_sync(fd) ||
	    io_pwrite(fd, buf + lbs, lbs, lbs) || io_sync(fd))
		return EXIT_IO;
	io_uncache(fd, 0, len);
	u8 *check = xmalloc(len);
	int r = io_pread(fd, check, len, 0) || memcmp(check, buf, len) ? EXIT_IO : EXIT_OK;
	free(check);
	if (r)
		msg("the restored table did not read back as written");
	return r;
}

void gpt_free(struct gpt *g)
{
	free(g->e);
	free(g->raw);
	free(g->header);
	g->e = NULL;
	g->raw = NULL;
	g->header = NULL;
	g->raw_bytes = 0;
}

bool gpt_entry_used(const struct gpt_entry *e)
{
	static const u8 zero[16];
	return memcmp(e->type, zero, 16) != 0;
}

int gpt_find(const struct gpt *g, const char *name)
{
	for (u32 i = 0; i < g->num_entries; i++)
		if (gpt_entry_used(&g->e[i]) && !strcmp(g->e[i].name, name))
			return (int)i;
	return -1;
}

int gpt_new_entry(struct gpt *g)
{
	for (u32 i = 0; i < g->capacity; i++) {
		/* Zero type is "unused", but a vendor may keep a record of its own in it. */
		if (gpt_entry_used(&g->e[i]) || !all_zero(g->raw + (size_t)i * g->entry_size, g->entry_size))
			continue;
		if (i >= g->num_entries)
			g->num_entries = i + 1;
		return (int)i;
	}
	return -1;
}

void gpt_name_stash(const struct gpt_entry *e, char *out, size_t cap)
{
	size_t n = 0;
	int i = 0;
	while (i < GPT_NAME_CHARS && get_le16(e->raw_name + 2 * i))
		i++;
	for (i++; i < GPT_NAME_CHARS && n + 1 < cap; i++) {
		u16 c = get_le16(e->raw_name + 2 * i);
		if (!c)
			break;
		out[n++] = c < 128 ? (char)c : '?';
	}
	out[n] = 0;
}

void gpt_set_name_stash(struct gpt_entry *e, const char *name, const char *stash)
{
	gpt_set_name(e, name);
	size_t at = strlen(e->name) + 1;
	for (size_t k = 0; stash[k] && at + k < GPT_NAME_CHARS; k++)
		put_le16(e->raw_name + 2 * (at + k), (u8)stash[k]);
}

u64 gpt_entry_bytes(const struct gpt *g, const struct gpt_entry *e)
{
	return (e->last_lba - e->first_lba + 1) * g->lbs;
}

void gpt_set_name(struct gpt_entry *e, const char *name)
{
	memset(e->name, 0, sizeof(e->name));
	memset(e->raw_name, 0, sizeof(e->raw_name));
	strncpy(e->name, name, GPT_NAME_CHARS);
	for (int i = 0; i < GPT_NAME_CHARS && e->name[i]; i++)
		put_le16(e->raw_name + 2 * i, (u8)e->name[i]);
}

void gpt_random_guid(u8 guid[16])
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || read(fd, guid, 16) != 16) {
		/* Never expected; still produce a unique-enough value. */
		u64 t = (u64)(uintptr_t)guid ^ (u64)getpid() << 32;
		for (int i = 0; i < 16; i++)
			guid[i] = (u8)(t >> (i % 8 * 8)) ^ (u8)i;
	}
	if (fd >= 0)
		close(fd);
	guid[7] = (guid[7] & 0x0f) | 0x40; /* version 4 (mixed-endian field 3) */
	guid[8] = (guid[8] & 0x3f) | 0x80; /* variant 1 */
}
