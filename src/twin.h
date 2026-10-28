/*
 * Dual data: the twindata/twinmeta partitions and which slot owns them.
 *
 * The primary slot's ROM mounts userdata and metadata as built. The owner
 * (secondary) slot's ROM is rebound to twindata and twinmeta in all three
 * fstab copies: vendor's /etc/fstab.*, and the first-stage and recovery
 * fstabs in vendor_boot (or boot, on phones without vendor_boot). Each ROM therefore always mounts its own data, even
 * when the bootloader falls back to it on its own.
 */
#ifndef TWINSLOT_TWIN_H
#define TWINSLOT_TWIN_H

#include "common.h"
#include "fstab.h"

/* GPT attribute bit marking twindata/twinmeta as owned by slot b (else a). */
#define TWIN_ATTR_OWNER_B (1ull << 48)

struct twin_state {
	bool ready;      /* both twin partitions exist */
	int owner;       /* slot whose ROM uses them */
	u64 data_bytes, meta_bytes, userdata_bytes;
	char disk[256];
};

/* Disk holding userdata: --disk, else the UFS LUN / eMMC whose GPT lists it. */
extern const char *g_disk;
int twin_find_disk(char *path, size_t cap);
int twin_state(struct twin_state *st);

/* What a slot's ROM mounts: its boot image's ramdisks (vendor_boot, else boot) and vendor's fstab. */
int slot_binding(int slot, enum binding *ramdisk, enum binding *vendor);
/* Rebinds a slot's ROM to `to` and re-seals its vbmeta over the edited images. */
int slot_rebind(int slot, enum binding to);
/* The binding a slot's ROM must have: TWIN for the owner, else PRIMARY. */
enum binding slot_expected_binding(const struct twin_state *st, int slot);

/*
 * Blanks the data partitions a slot's ROM mounts (its expected binding) so
 * that ROM starts fresh, as after a factory reset. Refuses mounted partitions.
 */
int twin_wipe_slot_data(const struct twin_state *st, int slot);

/*
 * setup: carve twindata (and twinmeta) out of userdata's end. meta_from names
 * a spare partition to become twinmeta instead ("auto": a known spare one if
 * the phone has it; NULL: always carve).
 */
int twin_setup(u64 data_bytes, u64 meta_bytes, const char *meta_from, int owner, bool apply, bool force);
/* remove: give the space back to userdata. */
int twin_remove(bool apply, bool force);

/*
 * Copies of the partition table. setup and remove save one before they write
 * (to --table-dir and to /tmp or the image directory) and refuse on a disk
 * without a backup table when no copy could be saved.
 */
extern const char *g_table_dir;
extern const char *g_kept_copy;
extern bool g_kept_copy_earlier;
/* Saves the table area to dir (NULL: the default directory); prints table_copy=PATH. */
int twin_table_save(const char *dir);
/* Writes a saved copy back. Refuses another disk's copy, and an intact table without force. */
int twin_table_restore(const char *file, bool force);

#endif
