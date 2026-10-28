/*
 * Dynamic partitions: the liblp metadata at the start of super, which lists
 * every logical partition (system_a, vendor_b, ...) and where its extents lie.
 *
 * Only what TwinSlot needs: a single block device ("super"), linear extents,
 * read with checksum verification, edit, and write every metadata slot.
 */
#ifndef TWINSLOT_LP_H
#define TWINSLOT_LP_H

#include "common.h"

#define LP_SECTOR 512
#define LP_NAME_LEN 36

#define LP_ATTR_READONLY 1u
#define LP_ATTR_SLOT_SUFFIXED 2u
#define LP_ATTR_UPDATED 4u
#define LP_ATTR_DISABLED 8u

#define LP_TARGET_LINEAR 0u
#define LP_TARGET_ZERO 1u

struct lp_extent {
	u64 num_sectors;
	u32 target_type;
	u64 target_data; /* first physical sector, for linear extents */
	u32 target_source;
};

struct lp_partition {
	char name[LP_NAME_LEN + 1];
	u32 attrs;
	u32 group;
	u32 nextents;
	struct lp_extent *ext;
};

struct lp_group {
	char name[LP_NAME_LEN + 1];
	u32 flags;
	u64 max_size;
};

struct lp_bdev {
	u64 first_logical_sector;
	u32 alignment;
	u32 alignment_offset;
	u64 size;
	char name[LP_NAME_LEN + 1];
	u32 flags;
};

struct lp_metadata {
	u32 metadata_max_size, metadata_slot_count, logical_block_size;
	u16 major, minor;
	u32 header_size, flags;
	struct lp_partition *parts;
	u32 nparts;
	struct lp_group *groups;
	u32 ngroups;
	struct lp_bdev *bdevs;
	u32 nbdevs;
};

/* Reads metadata slot `slot` (primary, else backup), checksums verified. */
int lp_read(int fd, u32 slot, struct lp_metadata *m);
/* Writes the same metadata to every slot, backups first, then reads it all back. */
int lp_write(int fd, const struct lp_metadata *m);
void lp_free(struct lp_metadata *m);

int lp_find_partition(const struct lp_metadata *m, const char *name);
int lp_find_group(const struct lp_metadata *m, const char *name);
int lp_add_group(struct lp_metadata *m, const char *name, u64 max_size);
u64 lp_partition_size(const struct lp_partition *p);
u64 lp_group_used(const struct lp_metadata *m, u32 group);
/* Bytes not covered by any extent (what new partitions can use). */
u64 lp_free_bytes(const struct lp_metadata *m);

void lp_remove_partition(struct lp_metadata *m, int index);
/* Adds `name` in `group` with `size` bytes allocated from free space. */
int lp_add_partition(struct lp_metadata *m, const char *name, u32 group, u32 attrs, u64 size);

/* Byte offset in super of logical byte `off` of partition p; *run = contiguous bytes there. */
bool lp_map(const struct lp_partition *p, u64 off, u64 *phys, u64 *run);

/*
 * A partition of m whose name ends in "_<slot>" and whose extents overlap one
 * of live's partitions ending in "_<live_slot>"; NULL if there is none. After
 * an OTA the old slot's metadata can still list extents the new slot now uses.
 */
const char *lp_stale_overlap(const struct lp_metadata *m, char slot, const struct lp_metadata *live, char live_slot);

#endif
