/*
 * One partition to read or write: a block device (boot_b, vendor_boot_b), or
 * a logical partition inside super reached through its extents (system_b).
 * Writing logical partitions directly at their extents needs no device-mapper,
 * so it works the same in Android, in recovery and on image files.
 */
#ifndef TWINSLOT_PART_H
#define TWINSLOT_PART_H

#include "common.h"
#include "lp.h"

struct part {
	char name[64];
	int fd;
	bool own_fd;
	u64 size;
	const struct lp_partition *lp; /* NULL for a block device */
};

int part_open(struct part *p, const char *name, bool write);
void part_logical(struct part *p, int super_fd, const struct lp_partition *lp);
void part_close(struct part *p);

int part_read(const struct part *p, void *buf, size_t n, u64 off);
int part_write(const struct part *p, const void *buf, size_t n, u64 off);
int part_sync(const struct part *p);
/* SHA-256 of the first len bytes. */
int part_sha256(const struct part *p, u64 len, u8 digest[32]);

/*
 * Opens super and reads slot's copy of its metadata. A copy whose slot
 * partitions overlap the running slot's is stale (an OTA moved the running
 * ROM over them) and refused: writing through it would hit the running ROM.
 */
int super_read_slot(struct part *super, bool write, int slot, struct lp_metadata *m);

#endif
