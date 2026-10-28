/*
 * The A/B boot record: libboot_control's bootloader_control, 32 bytes at
 * offset 2048 of misc. The bootloader boots the bootable slot with the
 * highest priority.
 */
#ifndef TWINSLOT_BCTL_H
#define TWINSLOT_BCTL_H

#include "common.h"

#define BCTL_OFFSET 2048
#define BCTL_SIZE 32
#define BCTL_MAGIC 0x42414342u /* "BCAB" */
/* MediaTek's own boot control (Android 9/10 era) in para; only recognised, never written. */
#define BCTL_MAGIC_MTK_LEGACY 0x19191100u

/* MergeStatus from android.hardware.boot */
enum { MERGE_NONE, MERGE_UNKNOWN, MERGE_SNAPSHOTTED, MERGE_MERGING, MERGE_CANCELLED };

struct bctl_slot {
	unsigned priority, tries, successful, corrupted;
};

struct bctl {
	u8 raw[BCTL_SIZE];
	unsigned nb_slot, merge_status;
	struct bctl_slot slot[2];
};

/* Reads and decodes the record from misc. */
int bctl_read(struct bctl *b);
/* Encodes, writes, syncs and reads back. */
int bctl_write(struct bctl *b);

bool bctl_magic_ok(const struct bctl *b);
bool bctl_crc_ok(const struct bctl *b);
/* EXIT_OK, or EXIT_REFUSED after saying why. */
int bctl_check(const struct bctl *b, bool allow_bad_crc);

bool bctl_bootable(const struct bctl_slot *s);
/* The slot the bootloader tries first, -1 if none is bootable. */
int bctl_next_slot(const struct bctl *b);
bool bctl_merge_pending(const struct bctl *b);
const char *bctl_merge_name(unsigned m);

/* Target becomes the next boot: priority 15, 7 tries; the other stays a fallback. */
void bctl_set_active(struct bctl *b, int target);
/*
 * Slot can no longer boot until set active again (used while it is rewritten).
 * Priority 0 with no tries left, the same mark AOSP's boot control and
 * update_engine leave on a slot whose images are half written.
 */
void bctl_set_unbootable(struct bctl *b, int slot);
/*
 * Slot does not boot on its own, but its images are whole: priority 0 with
 * tries left. Setup and remove park the owner this way; switch may bring it back.
 */
void bctl_set_parked(struct bctl *b, int slot);
/* Marked by bctl_set_unbootable (or a bootloader that ran out of tries): switch refuses it. */
bool bctl_half_written(const struct bctl_slot *s);

#endif
