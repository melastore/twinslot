#include "bctl.h"

#include <string.h>
#include <unistd.h>

#include "hash.h"

/*
 * Layout, as the compiler lays out libboot_control's bitfields:
 *   0  slot_suffix[4]   4  magic   8  version
 *   9  nb_slot:3 recovery_tries_remaining:3
 *   10 merge_status:3 (a 3-bit field does not fit the 2 bits left in byte 9)
 *   11 reserved0        12 slot_info[4], 2 bytes each
 *   20 reserved1[8]     28 crc32 of bytes 0..27
 * slot_info byte 0: priority:4 tries_remaining:3 successful_boot:1;
 * byte 1: verity_corrupted:1.
 */
static void decode(struct bctl *b)
{
	b->nb_slot = b->raw[9] & 7;
	b->merge_status = b->raw[10] & 7;
	for (int i = 0; i < 2; i++) {
		u8 b0 = b->raw[12 + 2 * i], b1 = b->raw[13 + 2 * i];
		b->slot[i].priority = b0 & 15;
		b->slot[i].tries = (b0 >> 4) & 7;
		b->slot[i].successful = b0 >> 7;
		b->slot[i].corrupted = b1 & 1;
	}
}

static void encode(struct bctl *b)
{
	for (int i = 0; i < 2; i++) {
		struct bctl_slot *s = &b->slot[i];
		b->raw[12 + 2 * i] = (u8)((s->priority & 15) | (s->tries & 7) << 4 | (s->successful & 1) << 7);
		b->raw[13 + 2 * i] = (u8)((b->raw[13 + 2 * i] & ~1u) | (s->corrupted & 1));
	}
	put_le32(b->raw + 28, crc32(b->raw, 28));
}

int bctl_read(struct bctl *b)
{
	int fd = io_open(dev_path("misc"), false);
	if (fd < 0)
		return EXIT_IO;
	int r = io_pread(fd, b->raw, BCTL_SIZE, BCTL_OFFSET);
	close(fd);
	if (r)
		return EXIT_IO;
	decode(b);
	return EXIT_OK;
}

int bctl_write(struct bctl *b)
{
	encode(b);
	int fd = io_open(dev_path("misc"), true);
	if (fd < 0)
		return EXIT_IO;
	int r = io_pwrite(fd, b->raw, BCTL_SIZE, BCTL_OFFSET);
	if (!r)
		r = io_sync(fd);
	if (!r)
		io_uncache(fd, BCTL_OFFSET, BCTL_SIZE);
	close(fd);

	struct bctl check;
	if (r || bctl_read(&check) || memcmp(check.raw, b->raw, BCTL_SIZE)) {
		msg("the boot record write to misc did not stick");
		return EXIT_IO;
	}
	return EXIT_OK;
}

bool bctl_magic_ok(const struct bctl *b) { return get_le32(b->raw + 4) == BCTL_MAGIC; }

bool bctl_crc_ok(const struct bctl *b) { return get_le32(b->raw + 28) == crc32(b->raw, 28); }

/*
 * Where else a record may be: MediaTek's older boot control kept its own
 * record in para. TwinSlot does not write that format, but says it is there.
 */
static void explain_missing(void)
{
	if (!dev_exists("para"))
		return;
	int fd = io_open(dev_path("para"), false);
	u8 raw[BCTL_SIZE];
	if (fd < 0)
		return;
	bool read = !io_pread(fd, raw, sizeof(raw), BCTL_OFFSET);
	close(fd);
	if (!read)
		return;
	u32 magic = get_le32(raw + 4);
	if (magic == BCTL_MAGIC || magic == BCTL_MAGIC_MTK_LEGACY)
		msg("para holds an A/B record (magic 0x%08x): this bootloader keeps its slots there, and TwinSlot "
		    "only switches slots through misc", magic);
}

int bctl_check(const struct bctl *b, bool allow_bad_crc)
{
	if (!bctl_magic_ok(b)) {
		msg("misc has no A/B boot record (bad magic); refusing");
		explain_missing();
		return EXIT_REFUSED;
	}
	if (b->nb_slot < 2) {
		msg("the boot record says this is not an A/B device; refusing");
		return EXIT_REFUSED;
	}
	if (!bctl_crc_ok(b) && !allow_bad_crc) {
		msg("the boot record checksum is wrong; refusing (--force rewrites it)");
		return EXIT_REFUSED;
	}
	return EXIT_OK;
}

bool bctl_bootable(const struct bctl_slot *s) { return s->priority > 0 && (s->successful || s->tries > 0); }

int bctl_next_slot(const struct bctl *b)
{
	int best = -1;
	for (int i = 0; i < 2; i++)
		if (bctl_bootable(&b->slot[i]) && (best < 0 || b->slot[i].priority > b->slot[best].priority))
			best = i;
	return best;
}

bool bctl_merge_pending(const struct bctl *b)
{
	return b->merge_status == MERGE_SNAPSHOTTED || b->merge_status == MERGE_MERGING;
}

const char *bctl_merge_name(unsigned m)
{
	static const char *names[] = {"none", "unknown", "snapshotted", "merging", "cancelled"};
	return m < ARRAY_SIZE(names) ? names[m] : "invalid";
}

static void set_suffix(struct bctl *b, int slot)
{
	/* Keep the spelling the bootloader wrote ("_b" or "b"). */
	if (b->raw[0] == 'a' || b->raw[0] == 'b') {
		b->raw[0] = (u8)slot_letter(slot);
		b->raw[1] = 0;
	} else {
		b->raw[0] = '_';
		b->raw[1] = (u8)slot_letter(slot);
		b->raw[2] = 0;
	}
}

void bctl_set_active(struct bctl *b, int target)
{
	struct bctl_slot *t = &b->slot[target], *o = &b->slot[!target];
	t->priority = 15;
	t->tries = 7;
	t->successful = 0;
	t->corrupted = 0;
	if (o->priority >= 15)
		o->priority = 14;
	set_suffix(b, target);
}

void bctl_set_unbootable(struct bctl *b, int slot)
{
	struct bctl_slot *s = &b->slot[slot];
	s->priority = 0;
	s->tries = 0;
	s->successful = 0;
}

void bctl_set_parked(struct bctl *b, int slot)
{
	struct bctl_slot *s = &b->slot[slot];
	s->priority = 0;
	s->tries = 7;
	s->successful = 0;
}

bool bctl_half_written(const struct bctl_slot *s) { return !s->priority && !s->tries && !s->successful; }
