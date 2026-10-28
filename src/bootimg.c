#include "bootimg.h"

#include <stdlib.h>
#include <string.h>

/* zlib declares a crc32() of its own; TwinSlot's is in hash.h, and only zlib's (de)compressor is used here. */
#define crc32 zlib_crc32
#include <zlib.h>
#undef crc32

#include "avb.h"
#include "decompress.h"
#include "hash.h"

#define VENDOR_BOOT_MAGIC "VNDRBOOT"
#define V3_HEADER_SIZE 2112
#define V4_HEADER_SIZE 2128
#define TABLE_ENTRY_MIN 108

#define BOOT_MAGIC "ANDROID!"
#define BOOT_V0_HEADER_SIZE 1632
#define BOOT_V1_HEADER_SIZE 1648
#define BOOT_V2_HEADER_SIZE 1660
#define MTK_HEADER_MAGIC 0x58881688u /* MediaTek's 512-byte wrapper around old ramdisks */

#define LZ4_LEGACY_MAGIC 0x184c2102u
#define LZ4_LEGACY_BLOCK (8u << 20)
#define MAX_RAMDISK (256u << 20)

/* ---- ramdisk compression ---- */

enum rd_format { RD_LZ4_LEGACY, RD_GZIP, RD_CPIO };

static int lz4_legacy_decode(const u8 *in, size_t len, u8 **out, size_t *out_len)
{
	size_t cap = 0, n = 0, pos = 0;
	u8 *buf = NULL;
	while (pos + 4 <= len) {
		u32 word = get_le32(in + pos);
		pos += 4;
		if (word == LZ4_LEGACY_MAGIC)
			continue; /* concatenated streams */
		if (word == 0 || word > len - pos)
			break; /* trailing padding */
		if (n + LZ4_LEGACY_BLOCK > MAX_RAMDISK) {
			free(buf);
			return EXIT_BAD_INPUT;
		}
		if (cap < n + LZ4_LEGACY_BLOCK) {
			cap = n + LZ4_LEGACY_BLOCK;
			buf = xrealloc(buf, cap);
		}
		long got = lz4_block_decompress(in + pos, word, buf + n, LZ4_LEGACY_BLOCK);
		if (got < 0) {
			free(buf);
			return EXIT_BAD_INPUT;
		}
		n += (size_t)got;
		pos += word;
	}
	*out = buf ? buf : xmalloc(1);
	*out_len = n;
	return EXIT_OK;
}

static u8 *lz4_legacy_encode(const u8 *in, size_t len, size_t *out_len)
{
	size_t blocks = (len + LZ4_LEGACY_BLOCK - 1) / LZ4_LEGACY_BLOCK;
	size_t cap = 4 + blocks * (4 + LZ4_LEGACY_BLOCK + LZ4_LEGACY_BLOCK / 255 + 16);
	u8 *out = xmalloc(cap);
	size_t o = 4;
	put_le32(out, LZ4_LEGACY_MAGIC);
	for (size_t pos = 0; pos < len; pos += LZ4_LEGACY_BLOCK) {
		size_t n = MIN((size_t)LZ4_LEGACY_BLOCK, len - pos);
		size_t c = lz4_block_compress_hc(in + pos, n, out + o + 4, cap - o - 4);
		if (!c) {
			free(out);
			return NULL;
		}
		put_le32(out + o, (u32)c);
		o += 4 + c;
	}
	*out_len = o;
	return out;
}

/* One gzip member, as the kernel unpacks it; bytes after it must be padding. */
static int gzip_decode(const u8 *in, size_t len, u8 **out, size_t *out_len)
{
	z_stream z = {0};
	if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK)
		return EXIT_IO;
	size_t cap = MIN(MAX(len * 4, (size_t)1 << 20), (size_t)MAX_RAMDISK), n = 0;
	u8 *buf = xmalloc(cap);
	z.next_in = (Bytef *)in;
	z.avail_in = (uInt)len;
	int zr = Z_OK;
	while (zr == Z_OK) {
		if (n == cap) {
			if (cap >= MAX_RAMDISK)
				break;
			cap = MIN(cap * 2, (size_t)MAX_RAMDISK);
			buf = xrealloc(buf, cap);
		}
		z.next_out = buf + n;
		z.avail_out = (uInt)(cap - n);
		zr = inflate(&z, Z_NO_FLUSH);
		n = cap - z.avail_out;
	}
	size_t used = len - z.avail_in;
	inflateEnd(&z);
	bool tail_zero = true;
	for (size_t i = used; i < len && tail_zero; i++)
		tail_zero = !in[i];
	if (zr != Z_STREAM_END || !tail_zero) {
		free(buf);
		return EXIT_BAD_INPUT;
	}
	*out = buf;
	*out_len = n;
	return EXIT_OK;
}

static u8 *gzip_encode(const u8 *in, size_t len, size_t *out_len)
{
	z_stream z = {0};
	if (deflateInit2(&z, 9, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
		return NULL;
	size_t cap = deflateBound(&z, (uLong)len);
	u8 *out = xmalloc(cap);
	z.next_in = (Bytef *)in;
	z.avail_in = (uInt)len;
	z.next_out = out;
	z.avail_out = (uInt)cap;
	int zr = deflate(&z, Z_FINISH);
	*out_len = cap - z.avail_out;
	deflateEnd(&z);
	if (zr != Z_STREAM_END) {
		free(out);
		return NULL;
	}
	return out;
}

/* ---- cpio (newc) ---- */

static u32 cpio_hex(const u8 *p)
{
	u32 v = 0;
	for (int i = 0; i < 8; i++) {
		u8 c = p[i];
		v = v << 4 | (u32)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
	}
	return v;
}

static bool fstab_like(const char *name)
{
	const char *base = strrchr(name, '/');
	base = base ? base + 1 : name;
	return !strncmp(base, "fstab.", 6) || strstr(base, ".fstab") || !strcmp(base, "fstab") ||
	       !strcmp(base, "twrp.flags");
}

/* Rebinds fstab-like regular files of a cpio archive in place. */
static int cpio_rebind(u8 *a, size_t len, enum binding to, enum binding *seen, int *changes)
{
	size_t pos = 0;
	while (pos + 110 <= len) {
		if (memcmp(a + pos, "070701", 6) && memcmp(a + pos, "070702", 6))
			return EXIT_BAD_INPUT;
		u32 mode = cpio_hex(a + pos + 14);
		u32 filesize = cpio_hex(a + pos + 54);
		u32 namesize = cpio_hex(a + pos + 94);
		size_t name_at = pos + 110;
		size_t data_at = ALIGN_UP(name_at + namesize, 4);
		if (!namesize || data_at > len || filesize > len - data_at)
			return EXIT_BAD_INPUT;
		const char *name = (const char *)a + name_at;
		if (a[name_at + namesize - 1])
			return EXIT_BAD_INPUT;
		if (!strcmp(name, "TRAILER!!!"))
			return EXIT_OK;
		if ((mode & 0170000) == 0100000 && fstab_like(name)) {
			*seen = binding_merge(*seen, fstab_binding(a + data_at, filesize));
			if (to != BIND_NONE)
				*changes += fstab_rebind(a + data_at, filesize, to);
		}
		pos = ALIGN_UP(data_at + filesize, 4);
	}
	return EXIT_BAD_INPUT;
}

/* ---- ramdisks ---- */

/* lz4 legacy, gzip, plain cpio, or a MediaTek-wrapped one (which rebind_ramdisk refuses by name). */
static bool rd_format_known(const u8 *rd, u32 size)
{
	return (size >= 4 && (get_le32(rd) == LZ4_LEGACY_MAGIC || get_le32(rd) == MTK_HEADER_MAGIC)) ||
	       (size >= 3 && rd[0] == 0x1f && rd[1] == 0x8b && rd[2] == 8) || (size >= 6 && !memcmp(rd, "0707", 4));
}

static int rebind_ramdisk(const char *where, const u8 *rd, u32 size, enum binding to, enum binding *seen, int *changes,
			  u8 **out, size_t *out_len)
{
	u8 *cpio;
	size_t cpio_len;
	enum rd_format fmt;
	*out = NULL;

	if (size >= 4 && get_le32(rd) == LZ4_LEGACY_MAGIC) {
		fmt = RD_LZ4_LEGACY;
		if (lz4_legacy_decode(rd, size, &cpio, &cpio_len))
			return EXIT_BAD_INPUT;
	} else if (size >= 3 && rd[0] == 0x1f && rd[1] == 0x8b && rd[2] == 8) {
		fmt = RD_GZIP;
		if (gzip_decode(rd, size, &cpio, &cpio_len)) {
			msg("%s: the gzip ramdisk is damaged or has more than one stream", where);
			return EXIT_BAD_INPUT;
		}
	} else if (size >= 6 && !memcmp(rd, "0707", 4)) {
		fmt = RD_CPIO;
		cpio = xmalloc(size);
		memcpy(cpio, rd, size);
		cpio_len = size;
	} else if (size >= 4 && get_le32(rd) == MTK_HEADER_MAGIC) {
		msg("%s: the ramdisk carries a MediaTek header, which TwinSlot does not rebuild", where);
		return EXIT_BAD_INPUT;
	} else {
		msg("%s has a ramdisk in a format other than lz4, gzip or plain cpio", where);
		return EXIT_BAD_INPUT;
	}

	int before = *changes;
	int r = cpio_rebind(cpio, cpio_len, to, seen, changes);
	if (!r && *changes > before) {
		if (fmt == RD_CPIO) {
			*out = cpio;
			*out_len = cpio_len;
			cpio = NULL;
		} else {
			*out = fmt == RD_GZIP ? gzip_encode(cpio, cpio_len, out_len) : lz4_legacy_encode(cpio, cpio_len, out_len);
			if (!*out)
				r = EXIT_IO;
		}
	}
	free(cpio);
	return r;
}

/* ---- writing an image back ---- */

/*
 * Writes a rebuilt image over p and reads it back. An AVB footer stays: the
 * vbmeta it points at moves past the image if the image grew, and the footer
 * keeps the original size if the image did not (the padding up to it stays
 * part of what is hashed, and keeps the bytes it had past the old image,
 * old_len). avb_reseal then updates its hash descriptor.
 */
static int write_image(const struct part *p, const u8 *img, size_t len, u64 old_len)
{
	struct avb_footer f;
	bool footer = avb_footer_read(p, &f) == EXIT_OK;
	u8 *blob = NULL;
	u64 image_size = len, vbmeta_off = 0;
	int r = EXIT_OK;
	if (footer) {
		image_size = MAX(f.original_size, (u64)len);
		vbmeta_off = ALIGN_UP(image_size, 4096);
		if (vbmeta_off + f.vbmeta_size > p->size - AVB_FOOTER_SIZE) {
			msg("the rebuilt image leaves no room for %s's AVB footer", p->name);
			return EXIT_REFUSED;
		}
		blob = xmalloc((size_t)f.vbmeta_size);
		if ((r = part_read(p, blob, (size_t)f.vbmeta_size, f.vbmeta_offset))) {
			free(blob);
			return r;
		}
	}
	u8 *full = xcalloc(1, (size_t)p->size);
	memcpy(full, img, len);
	/* Whatever the vendor put between the image and original_size is hashed: it stays. */
	u64 keep = MAX((u64)len, old_len);
	if (footer && keep < f.original_size &&
	    (r = part_read(p, full + keep, (size_t)(f.original_size - keep), keep))) {
		free(full);
		free(blob);
		return r;
	}
	if (footer) {
		memcpy(full + vbmeta_off, blob, (size_t)f.vbmeta_size);
		f.original_size = image_size;
		f.vbmeta_offset = vbmeta_off;
		avb_footer_encode(&f, full + p->size - AVB_FOOTER_SIZE);
	}
	r = part_write(p, full, (size_t)p->size, 0);
	if (!r)
		r = part_sync(p);
	if (!r) {
		u8 *check = xmalloc((size_t)p->size);
		r = part_read(p, check, (size_t)p->size, 0);
		if (!r && memcmp(check, full, (size_t)p->size)) {
			msg("%s did not read back as written", p->name);
			r = EXIT_IO;
		}
		free(check);
	}
	free(full);
	free(blob);
	return r;
}

/* ---- vendor_boot (header v3, v4) ---- */

struct vendor_boot {
	u8 *img;
	size_t len;
	u32 version, page;
	u32 header_size, ramdisk_size, dtb_size;
	u32 table_size, table_num, table_entry, bootconfig_size;
};

static size_t pages(u64 n, u32 page) { return (size_t)ALIGN_UP(n, page); }

static int vb_parse(struct vendor_boot *v)
{
	const u8 *h = v->img;
	if (v->len < V3_HEADER_SIZE || memcmp(h, VENDOR_BOOT_MAGIC, 8))
		return EXIT_BAD_INPUT;
	v->version = get_le32(h + 8);
	v->page = get_le32(h + 12);
	v->ramdisk_size = get_le32(h + 24);
	v->header_size = get_le32(h + 2096);
	v->dtb_size = get_le32(h + 2100);
	if ((v->version != 3 && v->version != 4) || v->page < 2048 || v->page > 65536 || v->page & (v->page - 1))
		return EXIT_BAD_INPUT;
	if (v->version == 4) {
		v->table_size = get_le32(h + 2112);
		v->table_num = get_le32(h + 2116);
		v->table_entry = get_le32(h + 2120);
		v->bootconfig_size = get_le32(h + 2124);
		if (v->table_entry < TABLE_ENTRY_MIN || (u64)v->table_num * v->table_entry > v->table_size)
			return EXIT_BAD_INPUT;
	}
	return EXIT_OK;
}

static u64 vb_length(const struct vendor_boot *v)
{
	return pages(v->header_size, v->page) + pages(v->ramdisk_size, v->page) + pages(v->dtb_size, v->page) +
	       pages(v->table_size, v->page) + pages(v->bootconfig_size, v->page);
}

struct ramdisk {
	u32 offset, size; /* within the vendor ramdisk section */
	u8 *data;         /* new compressed bytes, or NULL if unchanged */
	size_t new_size;
};

static int vb_rebind(const struct part *p, struct vendor_boot *v, enum binding to, bool write, enum binding *binding,
		     int *changes)
{
	struct ramdisk *rds = NULL;
	u32 nrd = v->version == 4 ? v->table_num : 1;
	int r = EXIT_BAD_INPUT;
	size_t rd_section = pages(v->header_size, v->page);
	size_t dtb_section = rd_section + pages(v->ramdisk_size, v->page);
	size_t table_section = dtb_section + pages(v->dtb_size, v->page);
	size_t bc_section = table_section + pages(v->table_size, v->page);

	/* v3 has one ramdisk; v4 lists fragments in the ramdisk table. */
	rds = xcalloc(nrd, sizeof(*rds));
	if (v->version == 4) {
		for (u32 i = 0; i < nrd; i++) {
			const u8 *e = v->img + table_section + (size_t)i * v->table_entry;
			rds[i].size = get_le32(e);
			rds[i].offset = get_le32(e + 4);
			if ((u64)rds[i].offset + rds[i].size > v->ramdisk_size)
				goto done;
		}
	} else {
		rds[0].size = v->ramdisk_size;
	}

	for (u32 i = 0; i < nrd; i++) {
		const u8 *rd = v->img + rd_section + rds[i].offset;
		/* A fragment of another kind (a DLKM in LZ4 frames, say) holds no fstab: it is kept as it is. */
		if (nrd > 1 && !rd_format_known(rd, rds[i].size)) {
			msg("note: %s: ramdisk fragment %u is in a format TwinSlot does not read; left unchanged", p->name, i);
			continue;
		}
		r = rebind_ramdisk(p->name, rd, rds[i].size, to, binding, changes, &rds[i].data, &rds[i].new_size);
		if (r)
			goto done;
	}
	if (!*changes || !write) {
		r = EXIT_OK;
		goto done;
	}

	/* Rebuild: ramdisks packed in table order, then dtb, table and bootconfig, page aligned. */
	size_t new_rd_size = 0;
	for (u32 i = 0; i < nrd; i++)
		new_rd_size += rds[i].data ? rds[i].new_size : rds[i].size;
	size_t new_len = pages(v->header_size, v->page) + pages(new_rd_size, v->page) + pages(v->dtb_size, v->page) +
			 pages(v->table_size, v->page) + pages(v->bootconfig_size, v->page);
	if (new_len > p->size) {
		msg("the patched vendor_boot (%zu bytes) does not fit in %s", new_len, p->name);
		r = EXIT_REFUSED;
		goto done;
	}
	u8 *img = xcalloc(1, new_len);
	memcpy(img, v->img, v->header_size);
	put_le32(img + 24, (u32)new_rd_size);
	size_t o = pages(v->header_size, v->page), rd_off = 0;
	for (u32 i = 0; i < nrd; i++) {
		const u8 *src = rds[i].data ? rds[i].data : v->img + rd_section + rds[i].offset;
		size_t n = rds[i].data ? rds[i].new_size : rds[i].size;
		memcpy(img + o + rd_off, src, n);
		rds[i].offset = (u32)rd_off;
		rds[i].size = (u32)n;
		rd_off += n;
	}
	o += pages(new_rd_size, v->page);
	memcpy(img + o, v->img + dtb_section, v->dtb_size);
	o += pages(v->dtb_size, v->page);
	if (v->version == 4) {
		memcpy(img + o, v->img + table_section, v->table_size);
		for (u32 i = 0; i < nrd; i++) {
			put_le32(img + o + (size_t)i * v->table_entry, rds[i].size);
			put_le32(img + o + (size_t)i * v->table_entry + 4, rds[i].offset);
		}
		o += pages(v->table_size, v->page);
		memcpy(img + o, v->img + bc_section, v->bootconfig_size);
	}
	r = write_image(p, img, new_len, vb_length(v));
	free(img);
done:
	for (u32 i = 0; i < nrd; i++)
		free(rds[i].data);
	free(rds);
	return r;
}

/* ---- boot (header v0, v1, v2): the ramdisk of phones without vendor_boot ---- */

struct boot_v2 {
	u8 *img;
	size_t len;
	u32 version, page;
	u32 kernel_size, ramdisk_size, second_size, recovery_dtbo_size, dtb_size;
};

static int boot_parse(struct boot_v2 *b)
{
	const u8 *h = b->img;
	if (b->len < BOOT_V0_HEADER_SIZE || memcmp(h, BOOT_MAGIC, 8))
		return EXIT_BAD_INPUT;
	b->version = get_le32(h + 40);
	if (b->version > 2)
		return EXIT_BAD_INPUT;
	b->kernel_size = get_le32(h + 8);
	b->ramdisk_size = get_le32(h + 16);
	b->second_size = get_le32(h + 24);
	b->page = get_le32(h + 36);
	b->recovery_dtbo_size = b->version >= 1 ? get_le32(h + 1632) : 0;
	b->dtb_size = b->version >= 2 ? get_le32(h + 1648) : 0;
	if (b->page < 2048 || b->page > 65536 || b->page & (b->page - 1))
		return EXIT_BAD_INPUT;
	return EXIT_OK;
}

static u64 boot_length(const struct boot_v2 *b)
{
	return b->page + pages(b->kernel_size, b->page) + pages(b->ramdisk_size, b->page) +
	       pages(b->second_size, b->page) + pages(b->recovery_dtbo_size, b->page) + pages(b->dtb_size, b->page);
}

/* mkbootimg's id: SHA-1 over each section followed by its size. */
static void boot_id(const struct boot_v2 *b, const u8 *img, u8 id[32])
{
	const u32 sizes[] = {b->kernel_size, b->ramdisk_size, b->second_size, b->recovery_dtbo_size, b->dtb_size};
	const size_t n = b->version == 0 ? 3 : b->version == 1 ? 4 : 5;
	struct sha1 s;
	sha1_init(&s);
	size_t o = b->page;
	for (size_t i = 0; i < n; i++) {
		u8 le[4];
		sha1_update(&s, img + o, sizes[i]);
		put_le32(le, sizes[i]);
		sha1_update(&s, le, 4);
		o += pages(sizes[i], b->page);
	}
	memset(id, 0, 32);
	sha1_final(&s, id);
}

static int boot_rebind(const struct part *p, struct boot_v2 *b, enum binding to, bool write, enum binding *binding,
		       int *changes)
{
	size_t rd_at = b->page + pages(b->kernel_size, b->page);
	size_t after_rd = rd_at + pages(b->ramdisk_size, b->page);
	u8 *rd = NULL;
	size_t rd_len = 0;
	int r = rebind_ramdisk(p->name, b->img + rd_at, b->ramdisk_size, to, binding, changes, &rd, &rd_len);
	if (r || !*changes || !write) {
		free(rd);
		return r;
	}

	struct boot_v2 nb = *b;
	nb.ramdisk_size = (u32)rd_len;
	size_t new_len = (size_t)boot_length(&nb), tail = (size_t)(boot_length(b) - after_rd);
	if (new_len > p->size) {
		msg("the patched boot image (%zu bytes) does not fit in %s", new_len, p->name);
		free(rd);
		return EXIT_REFUSED;
	}
	u8 *img = xcalloc(1, new_len);
	memcpy(img, b->img, rd_at); /* header and kernel */
	memcpy(img + rd_at, rd, rd_len);
	memcpy(img + rd_at + pages(rd_len, b->page), b->img + after_rd, tail); /* second, recovery dtbo, dtb */
	put_le32(img + 16, (u32)rd_len);
	/* recovery_dtbo_offset is absolute: it moves with the ramdisk's end (only when it pointed at the section). */
	if (b->version >= 1 && b->recovery_dtbo_size) {
		u64 old_at = after_rd + pages(b->second_size, b->page);
		if (get_le64(b->img + 1636) == old_at)
			put_le64(img + 1636, old_at - after_rd + rd_at + pages(rd_len, b->page));
	}
	/* The id is rewritten only if it was mkbootimg's: some bootloaders compare it, none needs a stale one. */
	u8 old_id[32], new_id[32];
	boot_id(b, b->img, old_id);
	if (!memcmp(old_id, b->img + 576, 32)) {
		boot_id(&nb, img, new_id);
		memcpy(img + 576, new_id, 32);
	}
	r = write_image(p, img, new_len, boot_length(b));
	free(img);
	free(rd);
	return r;
}

/* ---- either kind ---- */

int bootimg_rebind(const struct part *p, enum binding to, bool write, enum binding *binding, int *changes)
{
	*binding = BIND_NONE;
	*changes = 0;
	if (p->size > (256u << 20)) {
		msg("%s is unexpectedly large", p->name);
		return EXIT_BAD_INPUT;
	}
	u8 *data = xmalloc((size_t)p->size);
	int r = part_read(p, data, (size_t)p->size, 0);
	if (r) {
		free(data);
		return r;
	}
	struct vendor_boot v = {.img = data, .len = (size_t)p->size};
	struct boot_v2 b = {.img = data, .len = (size_t)p->size};
	if (!vb_parse(&v) && vb_length(&v) <= p->size) {
		r = vb_rebind(p, &v, to, write, binding, changes);
	} else if (!boot_parse(&b) && boot_length(&b) <= p->size) {
		r = boot_rebind(p, &b, to, write, binding, changes);
	} else {
		msg("%s holds neither a vendor_boot (v3/v4) nor a boot image (v0-v2)", p->name);
		r = EXIT_BAD_INPUT;
	}
	free(data);
	return r;
}

int bootimg_size(const struct part *p, u64 *size)
{
	u8 h[4096];
	if (p->size < sizeof(h) || part_read(p, h, sizeof(h), 0))
		return EXIT_IO;
	struct vendor_boot v = {.img = h, .len = sizeof(h)};
	struct boot_v2 b = {.img = h, .len = sizeof(h)};
	if (!vb_parse(&v))
		*size = vb_length(&v);
	else if (!boot_parse(&b))
		*size = boot_length(&b);
	else
		*size = 0;
	if (!*size || *size > p->size) {
		msg("%s holds no boot image twinslot can read", p->name);
		return EXIT_BAD_INPUT;
	}
	return EXIT_OK;
}

int bootimg_hashed_size(const struct part *p, u64 *size)
{
	struct avb_footer f;
	if (avb_footer_read(p, &f) == EXIT_OK) {
		*size = f.original_size;
		return EXIT_OK;
	}
	return bootimg_size(p, size);
}
