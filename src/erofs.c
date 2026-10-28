#include "erofs.h"

#include "hash.h"

#include <stdlib.h>
#include <string.h>

#include "decompress.h"

#define EROFS_SUPER_OFFSET 1024
#define EROFS_MAGIC 0xe0f5e1e2u
#define EROFS_ISLOT 32

#define COMPAT_SB_CHKSUM 0x1u
#define INCOMPAT_ZERO_PADDING 0x1u
#define INCOMPAT_CHUNKED_FILE 0x4u
#define INCOMPAT_DEVICE_TABLE 0x8u /* also COMPR_HEAD2 */
#define INCOMPAT_ZTAILPACKING 0x10u
#define INCOMPAT_FRAGMENTS 0x20u /* also DEDUPE */

enum { FLAT_PLAIN = 0, COMPRESSED_FULL = 1, FLAT_INLINE = 2, COMPRESSED_COMPACT = 3, CHUNK_BASED = 4 };

#define ADVISE_COMPACTED_2B 0x0001
#define ADVISE_BIG_PCLUSTER_1 0x0002
#define ADVISE_BIG_PCLUSTER_2 0x0004
#define ADVISE_INLINE_PCLUSTER 0x0008
#define ADVISE_INTERLACED_PCLUSTER 0x0010
#define ADVISE_FRAGMENT_PCLUSTER 0x0020

enum { LC_PLAIN = 0, LC_HEAD1 = 1, LC_NONHEAD = 2, LC_HEAD2 = 3 };
#define LI_PARTIAL_REF (1u << 15)
#define LI_D0_CBLKCNT (1u << 11)

#define ALG_LZ4 0

#define S_IFMT_ 0170000
#define S_IFDIR_ 0040000
#define S_IFREG_ 0100000

#define MAX_FILE (16u << 20)

bool erofs_is_dir(const struct erofs_inode *ino) { return (ino->mode & S_IFMT_) == S_IFDIR_; }
bool erofs_is_reg(const struct erofs_inode *ino) { return (ino->mode & S_IFMT_) == S_IFREG_; }

static int sb_checksum(const struct erofs *fs, u32 *crc);

int erofs_open(struct erofs *fs, const struct part *p)
{
	u8 sb[128];
	memset(fs, 0, sizeof(*fs));
	fs->p = p;
	if (p->size < EROFS_SUPER_OFFSET + sizeof(sb) || part_read(p, sb, sizeof(sb), EROFS_SUPER_OFFSET))
		return EXIT_BAD_INPUT;
	if (get_le32(sb) != EROFS_MAGIC)
		return EXIT_BAD_INPUT;
	fs->blkbits = sb[12];
	if (fs->blkbits < 9 || fs->blkbits > 16) {
		msg("%s: EROFS block size 2^%u is not supported", p->name, fs->blkbits);
		return EXIT_BAD_INPUT;
	}
	fs->blksz = 1u << fs->blkbits;
	fs->root_nid = get_le16(sb + 14);
	fs->meta_blkaddr = get_le32(sb + 40);
	fs->feature_compat = get_le32(sb + 8);
	fs->feature_incompat = get_le32(sb + 80);
	if (fs->feature_incompat & INCOMPAT_DEVICE_TABLE && get_le16(sb + 86)) {
		msg("%s: multi-device EROFS images are not supported", p->name);
		return EXIT_BAD_INPUT;
	}
	u32 crc;
	if (fs->feature_compat & COMPAT_SB_CHKSUM && (sb_checksum(fs, &crc) || crc != get_le32(sb + 4))) {
		msg("%s: EROFS superblock checksum is wrong; not touching this image", p->name);
		return EXIT_BAD_INPUT;
	}
	return EXIT_OK;
}

/*
 * The kernel refuses to mount an image whose superblock checksum is wrong. It
 * covers the rest of block 0 after the superblock offset, which also holds
 * inodes when the metadata starts at block 0: any patch must reseal it.
 */
static int sb_checksum(const struct erofs *fs, u32 *crc)
{
	/* As the kernel: blocks of 1 KiB or less are covered whole, from the superblock on. */
	size_t len = fs->blksz > EROFS_SUPER_OFFSET ? fs->blksz - EROFS_SUPER_OFFSET : fs->blksz;
	u8 *b = xmalloc(len);
	int r = part_read(fs->p, b, len, EROFS_SUPER_OFFSET);
	if (!r) {
		put_le32(b + 4, 0);
		*crc = crc32c_raw(~0u, b, len);
	}
	free(b);
	return r;
}

static int sb_reseal(const struct erofs *fs)
{
	if (!(fs->feature_compat & COMPAT_SB_CHKSUM))
		return EXIT_OK;
	u32 crc;
	u8 b[4];
	int r = sb_checksum(fs, &crc);
	if (r)
		return r;
	put_le32(b, crc);
	return part_write(fs->p, b, 4, EROFS_SUPER_OFFSET + 4);
}

static int read_inode(struct erofs *fs, u64 nid, struct erofs_inode *ino)
{
	u8 b[64];
	memset(ino, 0, sizeof(*ino));
	ino->nid = nid;
	ino->loc = (u64)fs->meta_blkaddr * fs->blksz + nid * EROFS_ISLOT;
	if (part_read(fs->p, b, 32, ino->loc))
		return EXIT_IO;
	u16 format = get_le16(b);
	u16 icount = get_le16(b + 2);
	ino->mode = get_le16(b + 4);
	ino->layout = (format >> 1) & 7;
	if (format & 1) {
		if (part_read(fs->p, b, 64, ino->loc))
			return EXIT_IO;
		ino->isize = 64;
		ino->size = get_le64(b + 8);
	} else {
		ino->isize = 32;
		ino->size = get_le32(b + 8);
	}
	ino->raw_blkaddr = get_le32(b + 16);
	ino->xattr_isize = icount ? 12 + (icount - 1u) * 4 : 0;
	return EXIT_OK;
}

/* ---- compressed files: logical clusters to extents ---- */

struct lcluster {
	int type;
	u32 clusterofs;
	u64 pblk;
	u32 cblks; /* from a CBLKCNT NONHEAD, else 0 */
	u32 delta0;
	bool partialref;
};

struct zinfo {
	u16 advise;
	u8 alg[2];
	u32 lclusterbits;
	u64 iend; /* end of inode + xattrs */
};

struct zext {
	u64 la, llen;
	u64 pa;
	u32 plen;
	int type;
	bool partialref;
};

static int load_full(struct erofs *fs, const struct erofs_inode *ino, const struct zinfo *z, u64 lcn,
		     struct lcluster *m)
{
	u8 di[8];
	u64 pos = ALIGN_UP(z->iend, 8) + 8 + 8 + lcn * 8;
	if (part_read(fs->p, di, sizeof(di), pos))
		return EXIT_IO;
	u16 advise = get_le16(di);
	memset(m, 0, sizeof(*m));
	m->type = advise & 3;
	if (m->type == LC_NONHEAD) {
		m->clusterofs = 1u << z->lclusterbits;
		m->delta0 = get_le16(di + 4);
		if (m->delta0 & LI_D0_CBLKCNT) {
			if (!(z->advise & (ADVISE_BIG_PCLUSTER_1 | ADVISE_BIG_PCLUSTER_2)))
				return EXIT_BAD_INPUT;
			m->cblks = m->delta0 & ~LI_D0_CBLKCNT;
			m->delta0 = 1;
		}
	} else {
		m->partialref = advise & LI_PARTIAL_REF;
		m->clusterofs = get_le16(di + 2);
		if (m->clusterofs >= 1u << z->lclusterbits)
			return EXIT_BAD_INPUT;
		m->pblk = get_le32(di + 4);
	}
	(void)ino;
	return EXIT_OK;
}

static u32 compacted_bits(u32 lobits, const u8 *in, u32 pos, int *type)
{
	u32 v = get_le32(in + pos / 8) >> (pos & 7);
	*type = (int)((v >> lobits) & 3);
	return v & ((1u << lobits) - 1);
}

static int load_compact(struct erofs *fs, const struct erofs_inode *ino, const struct zinfo *z, u64 lcn,
			struct lcluster *m)
{
	const u64 ebase = ALIGN_UP(z->iend, 8) + 8;
	const u32 totalidx = (u32)((ino->size + fs->blksz - 1) >> fs->blkbits);
	const u32 lclusterbits = z->lclusterbits;
	const bool big = z->advise & ADVISE_BIG_PCLUSTER_1;

	if (lcn >= totalidx || lclusterbits > 14)
		return EXIT_BAD_INPUT;
	memset(m, 0, sizeof(*m));

	u32 initial_4b = (u32)(((32 - ebase % 32) / 4) & 7);
	u32 compacted_2b = 0;
	if ((z->advise & ADVISE_COMPACTED_2B) && initial_4b < totalidx)
		compacted_2b = (totalidx - initial_4b) / 16 * 16;

	u64 pos = ebase;
	u32 shift = 2;
	u64 i_lcn = lcn;
	if (i_lcn >= initial_4b) {
		pos += initial_4b * 4;
		i_lcn -= initial_4b;
		if (i_lcn < compacted_2b) {
			shift = 1;
		} else {
			pos += compacted_2b * 2;
			i_lcn -= compacted_2b;
		}
	}
	pos += i_lcn << shift;

	u32 vcnt;
	if (shift == 2 && lclusterbits <= 14)
		vcnt = 2;
	else if (shift == 1 && lclusterbits <= 12)
		vcnt = 16;
	else
		return EXIT_BAD_INPUT;

	const u32 packsize = vcnt << shift;
	const u32 lobits = MAX(lclusterbits, 12u);
	const u32 encodebits = (packsize - 4) * 8 / vcnt;
	u64 base = pos / packsize * packsize;
	u8 in[32 + 4];
	if (part_read(fs->p, in, packsize, base))
		return EXIT_IO;
	int i = (int)((pos - base) >> shift);
	int type;
	u32 lo = compacted_bits(lobits, in, encodebits * (u32)i, &type);
	m->type = type;

	if (type == LC_NONHEAD) {
		m->clusterofs = 1u << lclusterbits;
		if (lo & LI_D0_CBLKCNT) {
			if (!big)
				return EXIT_BAD_INPUT;
			m->cblks = lo & ~LI_D0_CBLKCNT;
			m->delta0 = 1;
			return EXIT_OK;
		}
		if (i + 1 != (int)vcnt) {
			m->delta0 = lo;
			return EXIT_OK;
		}
		/* The last lcluster of a pack stores delta[1]; derive delta[0] from the one before. */
		lo = compacted_bits(lobits, in, encodebits * (u32)(i - 1), &type);
		if (type != LC_NONHEAD)
			lo = 0;
		else if (lo & LI_D0_CBLKCNT)
			lo = 1;
		m->delta0 = lo + 1;
		return EXIT_OK;
	}

	m->clusterofs = lo;
	u32 nblk;
	if (!big) {
		nblk = 1;
		while (i > 0) {
			--i;
			lo = compacted_bits(lobits, in, encodebits * (u32)i, &type);
			if (type == LC_NONHEAD)
				i -= (int)lo;
			if (i >= 0)
				++nblk;
		}
	} else {
		nblk = 0;
		while (i > 0) {
			--i;
			lo = compacted_bits(lobits, in, encodebits * (u32)i, &type);
			if (type == LC_NONHEAD) {
				if (lo & LI_D0_CBLKCNT) {
					--i;
					nblk += lo & ~LI_D0_CBLKCNT;
					continue;
				}
				if (lo <= 1)
					return EXIT_BAD_INPUT;
				i -= (int)lo - 2;
				continue;
			}
			++nblk;
		}
	}
	m->pblk = get_le32(in + packsize - 4) + (u64)nblk;
	return EXIT_OK;
}

static int load_lcluster(struct erofs *fs, const struct erofs_inode *ino, const struct zinfo *z, u64 lcn,
			 struct lcluster *m)
{
	return ino->layout == COMPRESSED_FULL ? load_full(fs, ino, z, lcn, m) : load_compact(fs, ino, z, lcn, m);
}

static int zinfo_read(struct erofs *fs, const struct erofs_inode *ino, struct zinfo *z)
{
	u8 h[8];
	memset(z, 0, sizeof(*z));
	z->iend = ino->loc + ino->isize + ino->xattr_isize;
	if (part_read(fs->p, h, sizeof(h), ALIGN_UP(z->iend, 8)))
		return EXIT_IO;
	if (h[7] & 0x80) {
		msg("%s: file nid %llu is stored in the packed inode (fragments); not supported", fs->p->name,
		    (unsigned long long)ino->nid);
		return EXIT_BAD_INPUT;
	}
	z->advise = get_le16(h + 4);
	z->alg[0] = h[6] & 15;
	z->alg[1] = h[6] >> 4;
	z->lclusterbits = fs->blkbits + (h[7] & 7);
	if (z->advise & (ADVISE_INLINE_PCLUSTER | ADVISE_FRAGMENT_PCLUSTER)) {
		msg("%s: file nid %llu uses tail packing or fragments; not supported", fs->p->name,
		    (unsigned long long)ino->nid);
		return EXIT_BAD_INPUT;
	}
	if (ino->layout == COMPRESSED_COMPACT &&
	    !(z->advise & ADVISE_BIG_PCLUSTER_1) != !(z->advise & ADVISE_BIG_PCLUSTER_2))
		return EXIT_BAD_INPUT;
	return EXIT_OK;
}

/* The file's extents in logical order. */
static int zmap(struct erofs *fs, const struct erofs_inode *ino, struct zinfo *z, struct zext **out, u32 *count)
{
	int r = zinfo_read(fs, ino, z);
	if (r)
		return r;
	const u64 lsz = 1ull << z->lclusterbits;
	const u64 nl = (ino->size + lsz - 1) / lsz;
	const bool big1 = z->advise & ADVISE_BIG_PCLUSTER_1, big2 = z->advise & ADVISE_BIG_PCLUSTER_2;
	struct zext *ext = NULL;
	u32 n = 0;

	for (u64 lcn = 0; lcn < nl; lcn++) {
		struct lcluster m;
		r = load_lcluster(fs, ino, z, lcn, &m);
		if (r)
			goto fail;
		if (m.type == LC_NONHEAD)
			continue;
		u64 la = lcn * lsz + m.clusterofs;
		if (la >= ino->size)
			continue;
		/* Extents must start in order, or the length of the one before wraps around. */
		if ((lcn == 0 && m.clusterofs) || m.clusterofs >= lsz || (n && la <= ext[n - 1].la)) {
			r = EXIT_BAD_INPUT;
			goto fail;
		}
		if (n)
			ext[n - 1].llen = la - ext[n - 1].la;

		u32 cblks = 0;
		if ((m.type == LC_HEAD1 && !big1) || ((m.type == LC_PLAIN || m.type == LC_HEAD2) && !big2) ||
		    (lcn + 1) * lsz >= ino->size) {
			cblks = 1;
		} else {
			struct lcluster next;
			r = load_lcluster(fs, ino, z, lcn + 1, &next);
			if (r)
				goto fail;
			if (next.type != LC_NONHEAD)
				cblks = 1;
			else if (next.delta0 == 1 && next.cblks)
				cblks = next.cblks;
			else {
				r = EXIT_BAD_INPUT;
				goto fail;
			}
		}
		ext = xrealloc(ext, (n + 1) * sizeof(*ext));
		ext[n++] = (struct zext){la, 0, m.pblk << fs->blkbits, cblks << fs->blkbits, m.type, m.partialref};
	}
	if (!n) {
		r = EXIT_BAD_INPUT;
		goto fail;
	}
	ext[n - 1].llen = ino->size - ext[n - 1].la;
	*out = ext;
	*count = n;
	return EXIT_OK;
fail:
	free(ext);
	if (r == EXIT_BAD_INPUT)
		msg("%s: compressed file nid %llu has an index we cannot follow", fs->p->name,
		    (unsigned long long)ino->nid);
	return r;
}

/* Start of LZ4 data in a pcluster: zero padding is only ever inside the first block. */
static long lz4_start(const struct erofs *fs, const u8 *raw, u32 plen)
{
	if (!(fs->feature_incompat & INCOMPAT_ZERO_PADDING))
		return 0;
	for (u32 i = 0; i < MIN(plen, fs->blksz); i++)
		if (raw[i])
			return i;
	return -1;
}

/*
 * Decodes extent e. For LZ4, *full gets everything the stream holds (may run
 * past llen); out gets the extent's llen bytes.
 */
static int zext_decode(struct erofs *fs, const struct zinfo *z, const struct zext *e, u8 *out, u8 **full,
		       size_t *full_len)
{
	u8 *raw = xmalloc(e->plen);
	int r = part_read(fs->p, raw, e->plen, e->pa);
	if (r)
		goto done;

	if (e->type == LC_PLAIN) {
		if (e->llen > e->plen) {
			r = EXIT_BAD_INPUT;
			goto done;
		}
		if (z->advise & ADVISE_INTERLACED_PCLUSTER) {
			if (e->plen > fs->blksz) {
				r = EXIT_BAD_INPUT;
				goto done;
			}
			u32 skip = (u32)(e->la & (fs->blksz - 1));
			u32 right = (u32)MIN(e->llen, (u64)(fs->blksz - skip));
			memcpy(out, raw + skip, right);
			memcpy(out + right, raw, (size_t)e->llen - right);
		} else {
			memcpy(out, raw, (size_t)e->llen);
		}
		goto done;
	}

	u8 alg = z->alg[e->type == LC_HEAD2];
	if (alg != ALG_LZ4) {
		msg("%s: only LZ4-compressed EROFS files are supported (found algorithm %u)", fs->p->name, alg);
		r = EXIT_BAD_INPUT;
		goto done;
	}
	long start = lz4_start(fs, raw, e->plen);
	size_t cap = (size_t)e->llen + 65536;
	u8 *buf = xmalloc(cap);
	long got = start < 0 ? -1 : lz4_block_decompress(raw + start, e->plen - (size_t)start, buf, cap);
	if (got < 0 || (u64)got < e->llen) {
		free(buf);
		r = EXIT_BAD_INPUT;
		msg("%s: a compressed cluster does not decode", fs->p->name);
		goto done;
	}
	memcpy(out, buf, (size_t)e->llen);
	if (full) {
		*full = buf;
		*full_len = (size_t)got;
	} else {
		free(buf);
	}
done:
	free(raw);
	return r;
}

/* ---- plain and inline files ---- */

/* Physical byte of logical offset off in a plain/inline file; *run = contiguous bytes. */
static u64 flat_map(const struct erofs *fs, const struct erofs_inode *ino, u64 off, u64 *run)
{
	u64 lastblk = (ino->size + fs->blksz - 1) / fs->blksz - 1;
	u64 blk = off / fs->blksz;
	if (ino->layout == FLAT_INLINE && blk == lastblk) {
		*run = ino->size - off;
		return ino->loc + ino->isize + ino->xattr_isize + (off - blk * fs->blksz);
	}
	u64 end = ino->layout == FLAT_INLINE ? lastblk * fs->blksz : ino->size;
	*run = end - off;
	return (u64)ino->raw_blkaddr * fs->blksz + off;
}

static int flat_io(struct erofs *fs, const struct erofs_inode *ino, u8 *buf, u64 off, u64 len, bool write)
{
	while (len) {
		u64 run, phys = flat_map(fs, ino, off, &run);
		size_t n = (size_t)MIN(run, len);
		int r = write ? part_write(fs->p, buf, n, phys) : part_read(fs->p, buf, n, phys);
		if (r)
			return r;
		buf += n;
		off += n;
		len -= n;
	}
	return EXIT_OK;
}

int erofs_read(struct erofs *fs, const struct erofs_inode *ino, u8 **data)
{
	if (ino->size > MAX_FILE) {
		msg("%s: file nid %llu is too large to load", fs->p->name, (unsigned long long)ino->nid);
		return EXIT_BAD_INPUT;
	}
	u8 *buf = xmalloc((size_t)ino->size + 1);
	int r;
	if (!ino->size) {
		r = EXIT_OK;
	} else if (ino->layout == FLAT_PLAIN || ino->layout == FLAT_INLINE) {
		r = flat_io(fs, ino, buf, 0, ino->size, false);
	} else if (ino->layout == COMPRESSED_FULL || ino->layout == COMPRESSED_COMPACT) {
		struct zinfo z;
		struct zext *ext = NULL;
		u32 n = 0;
		r = zmap(fs, ino, &z, &ext, &n);
		for (u32 i = 0; !r && i < n; i++)
			r = zext_decode(fs, &z, &ext[i], buf + ext[i].la, NULL, NULL);
		free(ext);
	} else {
		msg("%s: file nid %llu uses data layout %u; not supported", fs->p->name, (unsigned long long)ino->nid,
		    ino->layout);
		r = EXIT_BAD_INPUT;
	}
	if (r) {
		free(buf);
		return r;
	}
	buf[ino->size] = 0;
	*data = buf;
	return EXIT_OK;
}

/* ---- directories ---- */

int erofs_readdir(struct erofs *fs, const struct erofs_inode *dir,
		  int (*fn)(const char *name, const struct erofs_inode *ino, void *ctx), void *ctx)
{
	u8 *data;
	int r = erofs_read(fs, dir, &data);
	if (r)
		return r;
	for (u64 blk = 0; blk * fs->blksz < dir->size && !r; blk++) {
		const u8 *b = data + blk * fs->blksz;
		u64 blen = MIN((u64)fs->blksz, dir->size - blk * fs->blksz);
		if (blen < 12)
			break;
		u32 count = get_le16(b + 8) / 12;
		if (!count || count * 12ull > blen) {
			r = EXIT_BAD_INPUT;
			break;
		}
		for (u32 i = 0; i < count && !r; i++) {
			u32 off = get_le16(b + i * 12 + 8);
			u32 end = i + 1 < count ? get_le16(b + (i + 1) * 12 + 8) : (u32)blen;
			if (off > end || end > blen) {
				r = EXIT_BAD_INPUT;
				break;
			}
			char name[256];
			u32 len = 0;
			while (off + len < end && b[off + len] && len < sizeof(name) - 1) {
				name[len] = (char)b[off + len];
				len++;
			}
			name[len] = 0;
			if (!strcmp(name, ".") || !strcmp(name, ".."))
				continue;
			struct erofs_inode child;
			r = read_inode(fs, get_le64(b + i * 12), &child);
			if (!r)
				r = fn(name, &child, ctx);
		}
	}
	free(data);
	if (r == EXIT_BAD_INPUT)
		msg("%s: a directory is malformed", fs->p->name);
	return r;
}

struct find_ctx {
	const char *name;
	size_t len;
	struct erofs_inode *out;
	bool found;
};

static int find_one(const char *name, const struct erofs_inode *ino, void *ctx)
{
	struct find_ctx *f = ctx;
	if (!f->found && strlen(name) == f->len && !memcmp(name, f->name, f->len)) {
		*f->out = *ino;
		f->found = true;
	}
	return EXIT_OK;
}

int erofs_lookup(struct erofs *fs, const char *path, struct erofs_inode *ino)
{
	int r = read_inode(fs, fs->root_nid, ino);
	while (!r && *path) {
		while (*path == '/')
			path++;
		if (!*path)
			break;
		const char *end = strchr(path, '/');
		size_t len = end ? (size_t)(end - path) : strlen(path);
		if (!erofs_is_dir(ino))
			return EXIT_BAD_INPUT;
		struct erofs_inode dir = *ino;
		struct find_ctx f = {path, len, ino, false};
		r = erofs_readdir(fs, &dir, find_one, &f);
		if (!r && !f.found)
			return EXIT_BAD_INPUT;
		path += len;
	}
	return r;
}

/* ---- patching ---- */

/*
 * LZ4HC of plain[0..keep) whose final literal run is extended to cover
 * plain[keep..n): a valid stream of all n bytes that is larger than plain
 * HC. Every LZ4 block ends with a literals-only sequence, so extending it
 * keeps the end-of-block rules. Returns 0 if it would not fit in cap.
 */
static size_t lz4_with_literal_tail(const u8 *plain, size_t keep, size_t n, u8 *dst, size_t cap)
{
	u8 *head = xmalloc(cap + 16);
	size_t hlen = keep ? lz4_block_compress_hc(plain, keep, head, cap) : 0;
	if (keep && !hlen) {
		free(head);
		return 0;
	}

	size_t pos = 0, last_seq = 0, lit_start = 0, out_pos = 0;
	while (pos < hlen) {
		last_seq = pos;
		u8 token = head[pos++];
		size_t lit = token >> 4;
		if (lit == 15) {
			u8 c;
			do {
				c = head[pos++];
				lit += c;
			} while (c == 255);
		}
		lit_start = out_pos;
		pos += lit;
		out_pos += lit;
		if (pos >= hlen)
			break;
		pos += 2;
		size_t ml = token & 15;
		if (ml == 15) {
			u8 c;
			do {
				c = head[pos++];
				ml += c;
			} while (c == 255);
		}
		out_pos += ml + 4;
	}

	size_t lits = n - lit_start, o = last_seq;
	size_t need = o + 1 + (lits >= 15 ? (lits - 15) / 255 + 1 : 0) + lits;
	if (need > cap) {
		free(head);
		return 0;
	}
	memcpy(dst, head, last_seq);
	free(head);
	if (lits >= 15) {
		dst[o++] = 0xf0;
		size_t rest = lits - 15;
		for (; rest >= 255; rest -= 255)
			dst[o++] = 255;
		dst[o++] = (u8)rest;
	} else {
		dst[o++] = (u8)(lits << 4);
	}
	memcpy(dst + o, plain + lit_start, lits);
	return o + lits;
}

/*
 * An LZ4 stream of plain[0..n) whose length lies in (lo, hi]. The kernel
 * finds the stream by skipping zero padding inside the first block only, so
 * a stream shorter than lo cannot be placed: then the tail is stored as
 * literals, 256 bytes more at a time, until the stream grows into the window.
 */
static size_t lz4_into_window(const u8 *plain, size_t n, u8 *dst, size_t lo, size_t hi)
{
	size_t got = lz4_block_compress_hc(plain, n, dst, hi);
	if (!got || got > lo)
		return got;
	for (size_t tail = 256; tail < n; tail += 256) {
		size_t sz = lz4_with_literal_tail(plain, n - tail, n, dst, hi);
		if (!sz)
			return 0;
		if (sz > lo)
			return sz;
	}
	size_t sz = lz4_with_literal_tail(plain, 0, n, dst, hi);
	return sz > lo ? sz : 0;
}

static int patch_compressed(struct erofs *fs, const struct erofs_inode *ino, const u8 *old, const u8 *new_data)
{
	struct zinfo z;
	struct zext *ext;
	u32 n;
	int r = zmap(fs, ino, &z, &ext, &n);
	if (r)
		return r;

	/*
	 * Without zero padding a stream starts the pcluster and must end it exactly: a shorter one leaves
	 * trailing bytes no strict decoder accepts. Refused before the first write, not after it.
	 */
	for (u32 i = 0; i < n && !(fs->feature_incompat & INCOMPAT_ZERO_PADDING); i++)
		if (ext[i].type != LC_PLAIN && memcmp(old + ext[i].la, new_data + ext[i].la, (size_t)ext[i].llen)) {
			msg("%s: compressed files without zero padding cannot be patched in place", fs->p->name);
			free(ext);
			return EXIT_REFUSED;
		}

	for (u32 i = 0; i < n && !r; i++) {
		const struct zext *e = &ext[i];
		if (!memcmp(old + e->la, new_data + e->la, (size_t)e->llen))
			continue;
		if (e->type == LC_PLAIN) {
			/* Stored uncompressed: overwrite in place (shifted layout only). */
			if (z.advise & ADVISE_INTERLACED_PCLUSTER) {
				msg("%s: interlaced plain clusters are not supported", fs->p->name);
				r = EXIT_BAD_INPUT;
			} else {
				r = part_write(fs->p, new_data + e->la, (size_t)e->llen, e->pa);
			}
			continue;
		}
		if (e->partialref) {
			msg("%s: the cluster is shared with other files (dedupe); not patching", fs->p->name);
			r = EXIT_BAD_INPUT;
			break;
		}

		u8 *full, *plain_out = xmalloc((size_t)e->llen);
		size_t full_len;
		r = zext_decode(fs, &z, e, plain_out, &full, &full_len);
		free(plain_out);
		if (r)
			break;
		memcpy(full, new_data + e->la, (size_t)e->llen);

		u8 *packed = xcalloc(1, e->plen);
		size_t lo = e->plen > fs->blksz ? e->plen - fs->blksz : 0;
		u8 *stream = xmalloc(e->plen + 16);
		size_t clen = lz4_into_window(full, full_len, stream, lo, e->plen);
		if (!clen) {
			msg("%s: the edited cluster does not fit back in %u bytes", fs->p->name, e->plen);
			r = EXIT_REFUSED;
		} else {
			/* Right-aligned after zero padding, as the image was written. */
			memcpy(packed + e->plen - clen, stream, clen);
			r = part_write(fs->p, packed, e->plen, e->pa);
		}
		free(stream);
		free(packed);
		free(full);
	}
	free(ext);
	return r;
}

int erofs_patch(struct erofs *fs, const struct erofs_inode *ino, int (*edit)(u8 *data, size_t len, void *ctx),
		void *ctx, int *changes)
{
	u8 *old, *check;
	int r = erofs_read(fs, ino, &old);
	if (r)
		return r;
	u8 *new_data = xmalloc((size_t)ino->size + 1);
	memcpy(new_data, old, (size_t)ino->size);
	*changes = edit(new_data, (size_t)ino->size, ctx);
	if (*changes < 0) {
		r = EXIT_REFUSED;
		goto done;
	}
	if (!memcmp(old, new_data, (size_t)ino->size))
		goto done;

	if (ino->layout == FLAT_PLAIN || ino->layout == FLAT_INLINE)
		r = flat_io(fs, ino, new_data, 0, ino->size, true);
	else
		r = patch_compressed(fs, ino, old, new_data);
	if (!r)
		r = sb_reseal(fs);
	if (r)
		goto done;
	if (part_sync(fs->p)) {
		r = EXIT_IO;
		goto done;
	}
	r = erofs_read(fs, ino, &check);
	if (!r) {
		if (memcmp(check, new_data, (size_t)ino->size)) {
			msg("%s: the patched file did not read back as written", fs->p->name);
			r = EXIT_IO;
		}
		free(check);
	}
done:
	free(old);
	free(new_data);
	return r;
}
