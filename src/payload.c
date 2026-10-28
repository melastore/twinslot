#include "payload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "decompress.h"
#include "hash.h"

/* InstallOperation.Type from update_metadata.proto */
enum {
	OP_REPLACE = 0,
	OP_REPLACE_BZ = 1,
	OP_ZERO = 6,
	OP_DISCARD = 7,
	OP_REPLACE_XZ = 8,
	OP_REPLACE_ZSTD = 14,
};

#define MAX_OP_BYTES (256u << 20)

/* ---- finding payload.bin ---- */

#define ZIP_EOCD 0x06054b50u
#define ZIP64_LOCATOR 0x07064b50u
#define ZIP64_EOCD 0x06064b50u
#define ZIP_CENTRAL 0x02014b50u
#define ZIP_LOCAL 0x04034b50u

/* Offset and size of a STORED member, from the central directory. */
static int zip_find(int fd, u64 file_size, const char *want, u64 *data_off, u64 *data_len)
{
	size_t tail_len = (size_t)MIN(file_size, (u64)(65535 + 22 + 20));
	u8 *tail = xmalloc(tail_len);
	int r = EXIT_BAD_INPUT;
	u8 *cd = NULL;

	if (io_pread(fd, tail, tail_len, file_size - tail_len)) {
		r = EXIT_IO;
		goto done;
	}
	long eocd = -1;
	for (long i = (long)tail_len - 22; i >= 0; i--)
		if (get_le32(tail + i) == ZIP_EOCD) {
			eocd = i;
			break;
		}
	if (eocd < 0) {
		msg("not a zip file and not a payload.bin");
		goto done;
	}
	u64 entries = get_le16(tail + eocd + 10);
	u64 cd_size = get_le32(tail + eocd + 12);
	u64 cd_off = get_le32(tail + eocd + 16);
	if (entries == 0xffff || cd_size == 0xffffffff || cd_off == 0xffffffff) {
		u8 loc[20], z64[56];
		if (eocd < 20 || get_le32(tail + eocd - 20) != ZIP64_LOCATOR)
			goto done;
		memcpy(loc, tail + eocd - 20, 20);
		if (io_pread(fd, z64, sizeof(z64), get_le64(loc + 8)) || get_le32(z64) != ZIP64_EOCD)
			goto done;
		entries = get_le64(z64 + 32);
		cd_size = get_le64(z64 + 40);
		cd_off = get_le64(z64 + 48);
	}
	if (cd_size > (64u << 20) || cd_off > file_size || cd_size > file_size - cd_off)
		goto done;
	cd = xmalloc((size_t)cd_size);
	if (io_pread(fd, cd, (size_t)cd_size, cd_off)) {
		r = EXIT_IO;
		goto done;
	}

	size_t want_len = strlen(want);
	for (u64 i = 0, pos = 0; i < entries && pos + 46 <= cd_size; i++) {
		const u8 *e = cd + pos;
		if (get_le32(e) != ZIP_CENTRAL)
			break;
		u16 method = get_le16(e + 10);
		u64 csize = get_le32(e + 20), usize = get_le32(e + 24);
		u16 nlen = get_le16(e + 28), xlen = get_le16(e + 30), clen = get_le16(e + 32);
		u64 lho = get_le32(e + 42);
		if (pos + 46 + nlen + xlen > cd_size)
			break;
		if (nlen == want_len && !memcmp(e + 46, want, nlen)) {
			/* zip64 extra field: the 0xffffffff values follow, in order. */
			const u8 *x = e + 46 + nlen;
			for (u32 k = 0; k + 4 <= xlen;) {
				u16 id = get_le16(x + k), sz = get_le16(x + k + 2);
				if (k + 4u + sz > xlen)
					break; /* a damaged field: read nothing past the entry */
				if (id == 1) {
					const u8 *v = x + k + 4, *v_end = v + sz;
					if (usize == 0xffffffff && v + 8 <= v_end)
						usize = get_le64(v), v += 8;
					if (csize == 0xffffffff && v + 8 <= v_end)
						csize = get_le64(v), v += 8;
					if (lho == 0xffffffff && v + 8 <= v_end)
						lho = get_le64(v);
				}
				k += 4u + sz;
			}
			if (method != 0 || csize != usize) {
				msg("%s is compressed inside the zip; it must be stored", want);
				goto done;
			}
			u8 lh[30];
			if (io_pread(fd, lh, sizeof(lh), lho) || get_le32(lh) != ZIP_LOCAL)
				goto done;
			*data_off = lho + 30 + get_le16(lh + 26) + get_le16(lh + 28);
			*data_len = usize;
			r = *data_off <= file_size && usize <= file_size - *data_off ? EXIT_OK : EXIT_BAD_INPUT;
			goto done;
		}
		pos += 46u + nlen + xlen + clen;
	}
	msg("the zip has no %s (is it an A/B ROM zip?)", want);
done:
	free(tail);
	free(cd);
	return r;
}

/* ---- protobuf wire format ---- */

struct pb {
	const u8 *p, *end;
};

struct pb_field {
	u32 num, wire;
	u64 v;          /* varint value */
	const u8 *data; /* length-delimited payload */
	u64 len;
};

static bool pb_varint(struct pb *b, u64 *v)
{
	*v = 0;
	for (int shift = 0; b->p < b->end && shift < 64; shift += 7) {
		u8 c = *b->p++;
		*v |= (u64)(c & 0x7f) << shift;
		if (!(c & 0x80))
			return true;
	}
	return false;
}

/* Next field; false at the end or on malformed input (then *bad is set). */
static bool pb_next(struct pb *b, struct pb_field *f, bool *bad)
{
	u64 key;
	if (b->p >= b->end)
		return false;
	if (!pb_varint(b, &key))
		goto malformed;
	f->num = (u32)(key >> 3);
	f->wire = key & 7;
	f->data = NULL;
	f->len = 0;
	switch (f->wire) {
	case 0:
		if (!pb_varint(b, &f->v))
			goto malformed;
		return true;
	case 1:
		if (b->end - b->p < 8)
			goto malformed;
		f->v = get_le64(b->p);
		b->p += 8;
		return true;
	case 2:
		if (!pb_varint(b, &f->len) || f->len > (u64)(b->end - b->p))
			goto malformed;
		f->data = b->p;
		b->p += f->len;
		return true;
	case 5:
		if (b->end - b->p < 4)
			goto malformed;
		f->v = get_le32(b->p);
		b->p += 4;
		return true;
	}
malformed:
	*bad = true;
	return false;
}

static struct pb pb_sub(const struct pb_field *f) { return (struct pb){f->data, f->data + f->len}; }

static void pb_string(char *dst, size_t cap, const struct pb_field *f)
{
	size_t n = (size_t)MIN(f->len, (u64)cap - 1);
	memcpy(dst, f->data, n);
	dst[n] = 0;
}

static bool parse_op(struct pb b, struct pl_op *op)
{
	struct pb_field f;
	bool bad = false;
	while (pb_next(&b, &f, &bad)) {
		switch (f.num) {
		case 1:
			op->type = (u32)f.v;
			break;
		case 2:
			op->data_offset = f.v;
			break;
		case 3:
			op->data_length = f.v;
			break;
		case 4: /* src_extents: only incremental payloads read old data */
			msg("this is an incremental OTA; use a full ROM zip");
			return false;
		case 6: {
			struct pl_extent x = {0, 0};
			struct pb sub = pb_sub(&f);
			struct pb_field g;
			while (pb_next(&sub, &g, &bad)) {
				if (g.num == 1)
					x.start = g.v;
				else if (g.num == 2)
					x.count = g.v;
			}
			op->dst = xrealloc(op->dst, (op->ndst + 1) * sizeof(*op->dst));
			op->dst[op->ndst++] = x;
			break;
		}
		case 8:
			if (f.len == 32) {
				memcpy(op->hash, f.data, 32);
				op->has_hash = true;
			}
			break;
		}
	}
	return !bad;
}

static bool parse_partition(struct pb b, struct pl_partition *p)
{
	struct pb_field f;
	bool bad = false;
	while (pb_next(&b, &f, &bad)) {
		if (f.num == 1 && f.wire == 2) {
			pb_string(p->name, sizeof(p->name), &f);
		} else if (f.num == 6 && f.wire == 2) {
			/* old_partition_info: full payloads leave it out or empty */
			struct pb sub = pb_sub(&f);
			struct pb_field g;
			while (pb_next(&sub, &g, &bad))
				if (g.num == 1 && g.v) {
					msg("%s: this is an incremental OTA; use a full ROM zip", p->name);
					return false;
				}
		} else if (f.num == 7 && f.wire == 2) {
			struct pb sub = pb_sub(&f);
			struct pb_field g;
			while (pb_next(&sub, &g, &bad)) {
				if (g.num == 1)
					p->size = g.v;
				else if (g.num == 2 && g.len == 32) {
					memcpy(p->hash, g.data, 32);
					p->has_hash = true;
				}
			}
		} else if (f.num == 8 && f.wire == 2) {
			p->ops = xrealloc(p->ops, (p->nops + 1) * sizeof(*p->ops));
			struct pl_op *op = &p->ops[p->nops++];
			memset(op, 0, sizeof(*op));
			if (!parse_op(pb_sub(&f), op))
				return false;
		}
	}
	return !bad;
}

static bool parse_dynamic(struct pb b, struct payload *pl)
{
	struct pb_field f;
	bool bad = false;
	while (pb_next(&b, &f, &bad)) {
		if (f.num != 1 || f.wire != 2)
			continue;
		pl->groups = xrealloc(pl->groups, (pl->ngroups + 1) * sizeof(*pl->groups));
		struct pl_group *g = &pl->groups[pl->ngroups++];
		memset(g, 0, sizeof(*g));
		struct pb sub = pb_sub(&f);
		struct pb_field h;
		while (pb_next(&sub, &h, &bad)) {
			if (h.num == 1 && h.wire == 2) {
				pb_string(g->name, sizeof(g->name), &h);
			} else if (h.num == 2) {
				g->size = h.v;
			} else if (h.num == 3 && h.wire == 2) {
				char name[64];
				pb_string(name, sizeof(name), &h);
				int i = payload_find(pl, name);
				if (i >= 0)
					pl->parts[i].dynamic = true;
			}
		}
	}
	return !bad;
}

static int parse_manifest(struct payload *pl, const u8 *buf, u64 len)
{
	struct pb b = {buf, buf + len};
	struct pb_field f;
	bool bad = false;
	struct pb dynamic = {NULL, NULL};

	pl->block_size = 4096;
	while (pb_next(&b, &f, &bad)) {
		if (f.num == 3) {
			pl->block_size = (u32)f.v;
		} else if (f.num == 12 && f.v != 0) {
			msg("this is an incremental OTA (minor version %llu); use a full ROM zip", (unsigned long long)f.v);
			return EXIT_BAD_INPUT;
		} else if (f.num == 13 && f.wire == 2) {
			pl->parts = xrealloc(pl->parts, (pl->nparts + 1) * sizeof(*pl->parts));
			struct pl_partition *p = &pl->parts[pl->nparts++];
			memset(p, 0, sizeof(*p));
			if (!parse_partition(pb_sub(&f), p))
				return EXIT_BAD_INPUT;
		} else if (f.num == 15 && f.wire == 2) {
			dynamic = pb_sub(&f);
		} else if (f.num == 16 && f.v) {
			msg("this is a partial update payload; use a full ROM zip");
			return EXIT_BAD_INPUT;
		}
	}
	/* After the partitions, so their dynamic flag can be set. */
	if (bad || (dynamic.p && !parse_dynamic(dynamic, pl))) {
		msg("the payload manifest is malformed");
		return EXIT_BAD_INPUT;
	}
	if (pl->block_size != 4096 || !pl->nparts) {
		msg("unexpected payload (block size %u, %u partitions)", pl->block_size, pl->nparts);
		return EXIT_BAD_INPUT;
	}
	for (u32 i = 0; i < pl->nparts; i++) {
		struct pl_partition *p = &pl->parts[i];
		if (!p->has_hash || p->size % pl->block_size) {
			msg("%s: no size or hash in the payload", p->name);
			return EXIT_BAD_INPUT;
		}
		for (u32 k = 0; k < p->nops; k++) {
			u32 t = p->ops[k].type;
			if (t != OP_REPLACE && t != OP_REPLACE_BZ && t != OP_REPLACE_XZ && t != OP_REPLACE_ZSTD &&
			    t != OP_ZERO && t != OP_DISCARD) {
				msg("%s: operation type %u is not in a full OTA", p->name, t);
				return EXIT_BAD_INPUT;
			}
		}
	}
	return EXIT_OK;
}

int payload_open(const char *path, struct payload *pl)
{
	memset(pl, 0, sizeof(*pl));
	pl->fd = io_open(path, false);
	if (pl->fd < 0)
		return EXIT_IO;

	u64 size, base = 0, len;
	u8 head[24];
	int r = EXIT_BAD_INPUT;
	if (io_size(pl->fd, &size) || size < sizeof(head) || io_pread(pl->fd, head, sizeof(head), 0)) {
		r = EXIT_IO;
		goto fail;
	}
	if (memcmp(head, "CrAU", 4)) {
		r = zip_find(pl->fd, size, "payload.bin", &base, &len);
		if (r)
			goto fail;
		if (io_pread(pl->fd, head, sizeof(head), base)) {
			r = EXIT_IO;
			goto fail;
		}
	} else {
		len = size;
	}
	if (memcmp(head, "CrAU", 4)) {
		msg("payload.bin has a bad header");
		goto fail;
	}
	pl->version = get_be64(head + 4);
	u64 manifest_len = get_be64(head + 12);
	u32 sig_len = get_be32(head + 20);
	if (pl->version != 2 || manifest_len > (64u << 20) || 24 + manifest_len + sig_len > len) {
		msg("unsupported payload (version %llu)", (unsigned long long)pl->version);
		goto fail;
	}
	u8 *manifest = xmalloc((size_t)manifest_len);
	if (io_pread(pl->fd, manifest, (size_t)manifest_len, base + 24)) {
		free(manifest);
		r = EXIT_IO;
		goto fail;
	}
	r = parse_manifest(pl, manifest, manifest_len);
	free(manifest);
	if (r)
		goto fail;
	pl->data_base = base + 24 + manifest_len + sig_len;
	for (u32 i = 0; i < pl->nparts; i++)
		for (u32 k = 0; k < pl->parts[i].nops; k++) {
			const struct pl_op *op = &pl->parts[i].ops[k];
			if (pl->data_base > size || op->data_offset > size - pl->data_base ||
			    op->data_length > size - pl->data_base - op->data_offset) {
				msg("%s: operation data lies past the end of the file (truncated download?)",
				    pl->parts[i].name);
				r = EXIT_BAD_INPUT;
				goto fail;
			}
		}
	return EXIT_OK;
fail:
	payload_close(pl);
	return r;
}

void payload_close(struct payload *pl)
{
	for (u32 i = 0; i < pl->nparts; i++) {
		for (u32 k = 0; k < pl->parts[i].nops; k++)
			free(pl->parts[i].ops[k].dst);
		free(pl->parts[i].ops);
	}
	free(pl->parts);
	free(pl->groups);
	if (pl->fd >= 0)
		close(pl->fd);
	memset(pl, 0, sizeof(*pl));
	pl->fd = -1;
}

int payload_find(const struct payload *pl, const char *name)
{
	for (u32 i = 0; i < pl->nparts; i++)
		if (!strcmp(pl->parts[i].name, name))
			return (int)i;
	return -1;
}

static int write_extents(const struct payload *pl, const struct pl_op *op, const struct part *dst, const u8 *data,
			 u64 *done, u64 total)
{
	u64 pos = 0;
	for (u32 i = 0; i < op->ndst; i++) {
		u64 off = op->dst[i].start * pl->block_size, len = op->dst[i].count * pl->block_size;
		int r = part_write(dst, data + pos, (size_t)len, off);
		if (r)
			return r;
		pos += len;
		*done += len;
	}
	progress((unsigned)(*done * 1000 / (total ? total : 1)), "writing %s", dst->name);
	return EXIT_OK;
}

/* ZERO and DISCARD: the destination becomes zeros, written a chunk at a time. */
static int zero_extents(const struct payload *pl, const struct pl_op *op, const struct part *dst, u64 *done,
			u64 total)
{
	enum { CHUNK = 1 << 20 };
	u8 *z = xcalloc(1, CHUNK);
	int r = EXIT_OK;
	for (u32 i = 0; i < op->ndst && !r; i++) {
		u64 off = op->dst[i].start * pl->block_size, len = op->dst[i].count * pl->block_size;
		for (u64 pos = 0; pos < len && !r; pos += CHUNK)
			r = part_write(dst, z, (size_t)MIN((u64)CHUNK, len - pos), off + pos);
		*done += len;
	}
	free(z);
	progress((unsigned)(*done * 1000 / (total ? total : 1)), "writing %s", dst->name);
	return r;
}

/* Reads, checks and decompresses one operation into a fresh buffer of its output size. */
static int op_decode(const struct payload *pl, const struct pl_partition *p, u32 k, u8 **out_buf, u64 *out_size)
{
	const struct pl_op *op = &p->ops[k];
	u64 out_len = 0;
	for (u32 i = 0; i < op->ndst; i++) {
		u64 blocks = p->size / pl->block_size;
		if (op->dst[i].start > blocks || op->dst[i].count > blocks - op->dst[i].start) {
			msg("%s: an operation writes past the partition's end", p->name);
			return EXIT_BAD_INPUT;
		}
		out_len += op->dst[i].count * pl->block_size;
	}
	if (op->type == OP_ZERO || op->type == OP_DISCARD) {
		/* No data: written in pieces by the caller, however large. */
		*out_buf = NULL;
		*out_size = out_len;
		return EXIT_OK;
	}
	if (out_len > MAX_OP_BYTES || op->data_length > MAX_OP_BYTES) {
		msg("%s: an operation is too large (%llu bytes)", p->name, (unsigned long long)out_len);
		return EXIT_BAD_INPUT;
	}

	int r = EXIT_OK;
	u8 *out = xcalloc(1, (size_t)MAX(out_len, 1));
	{
		u8 *in = xmalloc((size_t)MAX(op->data_length, 1));
		r = io_pread(pl->fd, in, (size_t)op->data_length, pl->data_base + op->data_offset) ? EXIT_IO : EXIT_OK;
		if (!r && op->has_hash) {
			u8 h[32];
			sha256(in, (size_t)op->data_length, h);
			if (memcmp(h, op->hash, 32)) {
				msg("%s: operation %u data is corrupt (hash mismatch; damaged download?)", p->name, k);
				r = EXIT_BAD_INPUT;
			}
		}
		if (!r) {
			switch (op->type) {
			case OP_REPLACE:
				if (op->data_length != out_len)
					r = EXIT_BAD_INPUT;
				else
					memcpy(out, in, (size_t)op->data_length);
				break;
			case OP_REPLACE_BZ:
				r = decompress(CODEC_BZIP2, in, (size_t)op->data_length, out, (size_t)out_len);
				break;
			case OP_REPLACE_XZ:
				r = decompress(CODEC_XZ, in, (size_t)op->data_length, out, (size_t)out_len);
				break;
			case OP_REPLACE_ZSTD:
				r = decompress(CODEC_ZSTD, in, (size_t)op->data_length, out, (size_t)out_len);
				break;
			}
			if (r)
				msg("%s: operation %u does not decompress", p->name, k);
		}
		free(in);
	}
	if (r) {
		free(out);
		return r;
	}
	*out_buf = out;
	*out_size = out_len;
	return EXIT_OK;
}

int payload_verify(const struct payload *pl, u32 index, u64 *done, u64 total)
{
	const struct pl_partition *p = &pl->parts[index];
	for (u32 k = 0; k < p->nops; k++) {
		u8 *buf;
		u64 len;
		int r = op_decode(pl, p, k, &buf, &len);
		if (r)
			return r;
		free(buf);
		*done += len;
		progress(total ? (unsigned)(*done * 1000 / total) : 0, "checking %s", p->name);
	}
	return EXIT_OK;
}

int payload_write(const struct payload *pl, u32 index, const struct part *dst, u64 *done, u64 total)
{
	const struct pl_partition *p = &pl->parts[index];
	if (p->size > dst->size) {
		msg("%s (%llu bytes) does not fit in %s (%llu bytes)", p->name, (unsigned long long)p->size, dst->name,
		    (unsigned long long)dst->size);
		return EXIT_REFUSED;
	}

	int r = EXIT_OK;
	for (u32 k = 0; k < p->nops && !r; k++) {
		u8 *out_buf;
		u64 out_len;
		if ((r = op_decode(pl, p, k, &out_buf, &out_len)))
			break;
		if (out_buf)
			r = write_extents(pl, &p->ops[k], dst, out_buf, done, total);
		else
			r = zero_extents(pl, &p->ops[k], dst, done, total);
		free(out_buf);
	}
	if (r)
		return r;

	if (part_sync(dst))
		return EXIT_IO;
	u8 h[32];
	r = part_sha256(dst, p->size, h);
	if (r)
		return r;
	if (memcmp(h, p->hash, 32)) {
		msg("%s: what is on %s does not match the ROM (hash mismatch)", p->name, dst->name);
		return EXIT_IO;
	}
	return EXIT_OK;
}
