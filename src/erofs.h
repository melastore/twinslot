/*
 * Read-mostly EROFS: find a file in an image and change bytes of it in place.
 *
 * Plain and inline files are rewritten directly. LZ4-compressed files are
 * patched one physical cluster at a time: decompress, edit, recompress with
 * LZ4HC into the same blocks (the format right-aligns compressed data, so a
 * shorter stream fits). The file's size and layout never change.
 */
#ifndef TWINSLOT_EROFS_H
#define TWINSLOT_EROFS_H

#include "common.h"
#include "part.h"

struct erofs {
	const struct part *p;
	u32 blkbits, blksz;
	u32 meta_blkaddr;
	u64 root_nid;
	u32 feature_compat, feature_incompat;
};

struct erofs_inode {
	u64 nid;
	u64 loc;         /* byte offset of the inode */
	u32 isize;       /* 32 or 64 */
	u32 xattr_isize;
	u16 mode;
	u32 layout;
	u64 size;
	u32 raw_blkaddr;
};

/* Returns EXIT_BAD_INPUT quietly when the partition is not EROFS. */
int erofs_open(struct erofs *fs, const struct part *p);
int erofs_lookup(struct erofs *fs, const char *path, struct erofs_inode *ino);
/* Calls fn(name, ino) for each entry of a directory (not . and ..). */
int erofs_readdir(struct erofs *fs, const struct erofs_inode *dir,
		  int (*fn)(const char *name, const struct erofs_inode *ino, void *ctx), void *ctx);
/* Whole file into a new buffer. */
int erofs_read(struct erofs *fs, const struct erofs_inode *ino, u8 **data);
/*
 * Runs edit(data, len, ctx) on the file's contents; edit returns how many
 * changes it made (-1 to abort). Changed bytes are written back in place and
 * the file is read again to prove it. *changes gets edit's count.
 */
int erofs_patch(struct erofs *fs, const struct erofs_inode *ino, int (*edit)(u8 *data, size_t len, void *ctx),
		void *ctx, int *changes);

bool erofs_is_dir(const struct erofs_inode *ino);
bool erofs_is_reg(const struct erofs_inode *ino);

#endif
