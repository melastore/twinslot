/*
 * GUID partition table of one disk (a UFS LUN such as /dev/block/sdc).
 * Both copies are read and checked; edits are written to both, backup first.
 *
 * MediaTek phones may keep no backup header at all (the last block is zero)
 * and a record of their own past the counted entries. Such a table is used
 * primary-only, and every byte TwinSlot does not change is written back as
 * it was read.
 */
#ifndef TWINSLOT_GPT_H
#define TWINSLOT_GPT_H

#include "common.h"

#define GPT_NAME_CHARS 36

struct gpt_entry {
	u8 type[16];
	u8 guid[16];
	u64 first_lba, last_lba; /* inclusive */
	u64 attrs;
	char name[GPT_NAME_CHARS + 1]; /* ASCII view of the UTF-16 name, for matching */
	u8 raw_name[2 * GPT_NAME_CHARS]; /* the name as stored; rewritten only by gpt_set_name */
};

struct gpt {
	int fd;
	u32 lbs; /* logical block size: 512 or 4096 */
	u64 disk_lbas;
	u64 first_usable, last_usable;
	u64 primary_entries_lba, backup_entries_lba, backup_lba;
	u8 disk_guid[16];
	u32 num_entries, entry_size;
	/* Entries that fit in the blocks the array already occupies (>= num_entries). */
	u32 capacity;
	bool has_backup; /* false: the disk keeps no backup copy (last block all zero) */
	bool backup_stale; /* the backup is damaged or differs; the next write fixes it */
	struct gpt_entry *e; /* capacity entries */
	u8 *raw;    /* the entry blocks as read, so bytes we do not model survive a rewrite */
	size_t raw_bytes;
	u8 *header; /* the primary header block as read */
};

/*
 * Reads and verifies both copies. EXIT_BAD_INPUT if either is damaged or they
 * differ; a backup header block that is entirely zero means "no backup".
 */
int gpt_read(int fd, struct gpt *g);
/* Writes both copies (backup first, if the disk keeps one), syncs, reads them back. */
int gpt_write(struct gpt *g);
void gpt_free(struct gpt *g);

/*
 * The primary table area: LBA 0 (protective MBR) up to the first usable
 * block. A copy of it is all a restore needs; MediaTek disks keep no backup.
 */
#define GPT_AREA_MAX (8u << 20)
int gpt_primary_area(const struct gpt *g, u8 **buf, size_t *len);
/* Is buf a whole, intact primary area for a disk of disk_bytes? Gives its block size and disk GUID. */
int gpt_area_check(u8 *buf, size_t len, u64 disk_bytes, u32 *lbs, u8 disk_guid[16]);
/* Writes a checked area back: MBR and entries, sync, header, sync, read back. */
int gpt_area_write(int fd, const u8 *buf, size_t len, u32 lbs);

bool gpt_entry_used(const struct gpt_entry *e);
int gpt_find(const struct gpt *g, const char *name);
/*
 * An entry to fill: an all-zero one among the counted entries, else the next
 * all-zero one in the existing entry blocks (num_entries grows to include it).
 * -1 if there is none.
 */
int gpt_new_entry(struct gpt *g);
/* Text stored after the name's terminating NUL (TwinSlot keeps an adopted partition's old name there). */
void gpt_name_stash(const struct gpt_entry *e, char *out, size_t cap);
void gpt_set_name_stash(struct gpt_entry *e, const char *name, const char *stash);
u64 gpt_entry_bytes(const struct gpt *g, const struct gpt_entry *e);
void gpt_set_name(struct gpt_entry *e, const char *name);
void gpt_random_guid(u8 guid[16]);

#endif
