/*
 * Full A/B OTA payloads (payload.bin), read straight out of the ROM zip.
 *
 * Only full payloads are supported: every operation writes new data
 * (REPLACE, REPLACE_BZ, REPLACE_XZ, REPLACE_ZSTD, ZERO, DISCARD). Incremental
 * OTAs need the old partitions and are refused.
 */
#ifndef TWINSLOT_PAYLOAD_H
#define TWINSLOT_PAYLOAD_H

#include "common.h"
#include "part.h"

struct pl_extent {
	u64 start, count; /* blocks */
};

struct pl_op {
	u32 type;
	u64 data_offset, data_length;
	struct pl_extent *dst;
	u32 ndst;
	u8 hash[32];
	bool has_hash;
};

struct pl_partition {
	char name[64];
	u64 size;
	u8 hash[32];
	bool has_hash;
	struct pl_op *ops;
	u32 nops;
	bool dynamic;
};

struct pl_group {
	char name[64];
	u64 size;
};

struct payload {
	int fd;
	u64 data_base; /* where operation data starts in the file */
	u64 version;
	u32 block_size;
	struct pl_partition *parts;
	u32 nparts;
	struct pl_group *groups;
	u32 ngroups;
};

/* Opens a ROM zip (payload.bin stored uncompressed) or a bare payload.bin. */
int payload_open(const char *path, struct payload *pl);
void payload_close(struct payload *pl);
int payload_find(const struct payload *pl, const char *name);

/*
 * Writes partition `index` to dst and checks its SHA-256 afterwards.
 * done/total are bytes for the progress line.
 */
/* Checks every operation's data hash and that it decompresses; writes nothing. */
int payload_verify(const struct payload *pl, u32 index, u64 *done, u64 total);
int payload_write(const struct payload *pl, u32 index, const struct part *dst, u64 *done, u64 total);

#endif
