#include "avb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootimg.h"
#include "hash.h"
#include "testkey.h"

/* libavb's on-disk format, all big-endian. */
#define HEADER_SIZE 256
#define MAX_BLOB (1u << 20)
#define FLAGS_OFF 120
#define FLAG_HASHTREE_DISABLED 1u
#define FLAG_VERIFICATION_DISABLED 2u
#define ALG_SHA256_RSA2048 1u

#define TAG_HASHTREE 1
#define TAG_HASH 2
#define TAG_CHAIN 4
#define HASHTREE_FIXED 164 /* after the 16-byte tag/length */
#define HASH_FIXED 116
#define CHAIN_FIXED 76

#define KEY_BYTES 256
#define LIMBS (KEY_BYTES / 4)

static void put_be64(void *p, u64 v)
{
	put_be32(p, (u32)(v >> 32));
	put_be32((u8 *)p + 4, (u32)v);
}

/* ---- AVB footers ---- */

int avb_footer_read(const struct part *p, struct avb_footer *f)
{
	u8 raw[AVB_FOOTER_SIZE];
	if (p->size < 2 * AVB_FOOTER_SIZE || part_read(p, raw, sizeof(raw), p->size - AVB_FOOTER_SIZE) ||
	    memcmp(raw, "AVBf", 4) || get_be32(raw + 4) != 1)
		return EXIT_BAD_INPUT;
	f->original_size = get_be64(raw + 12);
	f->vbmeta_offset = get_be64(raw + 20);
	f->vbmeta_size = get_be64(raw + 28);
	if (f->original_size > f->vbmeta_offset || f->vbmeta_size < HEADER_SIZE || f->vbmeta_size > MAX_BLOB ||
	    f->vbmeta_offset > p->size - AVB_FOOTER_SIZE ||
	    f->vbmeta_size > p->size - AVB_FOOTER_SIZE - f->vbmeta_offset)
		return EXIT_BAD_INPUT;
	return EXIT_OK;
}

void avb_footer_encode(const struct avb_footer *f, u8 out[AVB_FOOTER_SIZE])
{
	memset(out, 0, AVB_FOOTER_SIZE);
	memcpy(out, "AVBf", 4);
	put_be32(out + 4, 1);
	put_be32(out + 8, 0);
	put_be64(out + 12, f->original_size);
	put_be64(out + 20, f->vbmeta_offset);
	put_be64(out + 28, f->vbmeta_size);
}

/* ---- RSA-2048 PKCS#1 v1.5 signing with the test key ---- */

static void limbs_from_be(u32 *l, const u8 *be)
{
	for (int i = 0; i < LIMBS; i++)
		l[i] = get_be32(be + KEY_BYTES - 4 * (i + 1));
}

static void limbs_to_be(u8 *be, const u32 *l)
{
	for (int i = 0; i < LIMBS; i++)
		put_be32(be + KEY_BYTES - 4 * (i + 1), l[i]);
}

/* r = a * b / 2^2048 mod n (Montgomery, CIOS); n0inv = -1/n mod 2^32. */
static void mont_mul(u32 *r, const u32 *a, const u32 *b, const u32 *n, u32 n0inv)
{
	u32 t[LIMBS + 2] = {0};
	for (int i = 0; i < LIMBS; i++) {
		u64 c = 0;
		for (int j = 0; j < LIMBS; j++) {
			c += (u64)t[j] + (u64)a[j] * b[i];
			t[j] = (u32)c;
			c >>= 32;
		}
		c += t[LIMBS];
		t[LIMBS] = (u32)c;
		t[LIMBS + 1] = (u32)(c >> 32);

		u32 m = t[0] * n0inv;
		c = ((u64)t[0] + (u64)m * n[0]) >> 32;
		for (int j = 1; j < LIMBS; j++) {
			c += (u64)t[j] + (u64)m * n[j];
			t[j - 1] = (u32)c;
			c >>= 32;
		}
		c += t[LIMBS];
		t[LIMBS - 1] = (u32)c;
		t[LIMBS] = t[LIMBS + 1] + (u32)(c >> 32);
	}
	bool ge = t[LIMBS] != 0;
	if (!ge) {
		ge = true;
		for (int i = LIMBS - 1; i >= 0; i--)
			if (t[i] != n[i]) {
				ge = t[i] > n[i];
				break;
			}
	}
	if (ge) {
		u64 borrow = 0;
		for (int i = 0; i < LIMBS; i++) {
			u64 d = (u64)t[i] - n[i] - borrow;
			t[i] = (u32)d;
			borrow = (d >> 32) & 1;
		}
	}
	memcpy(r, t, LIMBS * 4);
}

static void rsa_sign(const u8 digest[32], u8 sig[KEY_BYTES])
{
	static const u8 sha256_info[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
					 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
	u8 em[KEY_BYTES];
	size_t ps = KEY_BYTES - 3 - sizeof(sha256_info) - 32;
	em[0] = 0;
	em[1] = 1;
	memset(em + 2, 0xff, ps);
	em[2 + ps] = 0;
	memcpy(em + 3 + ps, sha256_info, sizeof(sha256_info));
	memcpy(em + KEY_BYTES - 32, digest, 32);

	u32 n[LIMBS], rr[LIMBS], m[LIMBS], one[LIMBS] = {1}, x[LIMBS], am[LIMBS];
	u32 n0inv = get_be32(testkey_avb_public + 4);
	limbs_from_be(n, testkey_avb_public + 8);
	limbs_from_be(rr, testkey_avb_public + 8 + KEY_BYTES);
	limbs_from_be(m, em);
	mont_mul(am, m, rr, n, n0inv);  /* m in Montgomery form */
	mont_mul(x, one, rr, n, n0inv); /* 1 in Montgomery form */
	for (int i = 0; i < KEY_BYTES * 8; i++) {
		mont_mul(x, x, x, n, n0inv);
		if (testkey_d[i / 8] & (0x80 >> (i % 8)))
			mont_mul(x, x, am, n, n0inv);
	}
	mont_mul(x, x, one, n, n0inv);
	limbs_to_be(sig, x);
}

/* ---- the images vbmeta describes ---- */

static void salted_sha256(const u8 *salt, u32 salt_len, const u8 *data, size_t n, u8 out[32])
{
	struct sha256 s;
	sha256_init(&s);
	sha256_update(&s, salt, salt_len);
	sha256_update(&s, data, n);
	sha256_final(&s, out);
}

/* avbtool's default hashtree hash; out gets 20 bytes. */
static void salted_sha1(const u8 *salt, u32 salt_len, const u8 *data, size_t n, u8 *out)
{
	struct sha1 s;
	sha1_init(&s);
	sha1_update(&s, salt, salt_len);
	sha1_update(&s, data, n);
	sha1_final(&s, out);
}

static void salted_digest(bool sha1, const u8 *salt, u32 salt_len, const u8 *data, size_t n, u8 *out)
{
	if (sha1)
		salted_sha1(salt, salt_len, data, n, out);
	else
		salted_sha256(salt, salt_len, data, n, out);
}

static int image_digest(const struct part *p, u64 size, const u8 *salt, u32 salt_len, u8 out[32])
{
	enum { CHUNK = 4 << 20 };
	if (size > p->size)
		return EXIT_BAD_INPUT;
	u8 *buf = xmalloc(CHUNK);
	struct sha256 s;
	sha256_init(&s);
	sha256_update(&s, salt, salt_len);
	int r = EXIT_OK;
	for (u64 off = 0; off < size && !r; off += CHUNK) {
		size_t n = (size_t)MIN((u64)CHUNK, size - off);
		if (!(r = part_read(p, buf, n, off)))
			sha256_update(&s, buf, n);
	}
	free(buf);
	if (!r)
		sha256_final(&s, out);
	return r;
}

/*
 * dm-verity tree as avbtool builds it: level 0 hashes the data blocks, each
 * level is padded to a whole block, the top level is stored first, and the
 * root is the hash of the top level. Every entry takes 32 bytes: a SHA-1
 * digest is zero-padded to the next power of two, as avbtool does.
 */
static int hashtree(const struct part *p, u64 image_size, u32 bs, bool sha1, const u8 *salt, u32 salt_len,
		    u8 **tree_out, u64 *tree_size, u8 root[32])
{
	enum { MAX_LEVELS = 16, CHUNK = 4 << 20 };
	u64 sizes[MAX_LEVELS], offsets[MAX_LEVELS], total = 0;
	int levels = 0;
	/* Whole blocks per chunk, so the zero padding of the last one stays inside buf. */
	if (bs < 512 || bs > CHUNK || bs & (bs - 1))
		return EXIT_BAD_INPUT;
	for (u64 size = image_size; size > bs; levels++) {
		if (levels == MAX_LEVELS)
			return EXIT_BAD_INPUT;
		size = ALIGN_UP((size + bs - 1) / bs * 32, bs);
		sizes[levels] = size;
		total += size;
	}
	if (!levels || image_size > p->size)
		return EXIT_BAD_INPUT;
	for (int l = 0; l < levels; l++) {
		offsets[l] = 0;
		for (int k = l + 1; k < levels; k++)
			offsets[l] += sizes[k];
	}

	u8 *tree = xcalloc(1, (size_t)total);
	u8 *buf = xmalloc(CHUNK);
	int r = EXIT_OK;
	u64 block = 0;
	for (u64 off = 0; off < image_size && !r; off += CHUNK) {
		size_t n = (size_t)MIN((u64)CHUNK, image_size - off);
		if ((r = part_read(p, buf, n, off)))
			break;
		if (n % bs) /* a short last block hashes as if zero-padded */
			memset(buf + n, 0, bs - n % bs);
		for (size_t i = 0; i < n; i += bs, block++)
			salted_digest(sha1, salt, salt_len, buf + i, bs, tree + offsets[0] + block * 32);
		progress((unsigned)(off * 1000 / image_size), "hashing %s", p->name);
	}
	free(buf);
	for (int l = 1; l < levels && !r; l++)
		for (u64 i = 0; i < sizes[l - 1] / bs; i++)
			salted_digest(sha1, salt, salt_len, tree + offsets[l - 1] + i * bs, bs, tree + offsets[l] + i * 32);
	if (r) {
		free(tree);
		return r;
	}
	salted_digest(sha1, salt, salt_len, tree + offsets[levels - 1], bs, root);
	*tree_out = tree;
	*tree_size = total;
	return EXIT_OK;
}

/* ---- vbmeta structs ---- */

struct vbmeta {
	struct part part;
	u64 offset; /* of the blob in the partition: 0, or where an AVB footer points */
	bool in_footer;
	struct avb_footer footer;
	u8 *blob;
	size_t len;
	u8 *desc; /* rebuilt descriptors */
	size_t desc_len;
};

struct reseal {
	int slot;
	const struct part *ramdisk, *vendor; /* the edited images */
	const char *ramdisk_name;            /* "vendor_boot" or "boot" */
	int changed;
	bool saw_ramdisk, saw_vendor;
	bool ramdisk_footer_done, ramdisk_footer_resigned;
	bool vendor_footer_done, vendor_footer_resigned;
};

/* The blob at v->offset, within room bytes; the caller has opened v->part. */
static int vbmeta_load_blob(struct vbmeta *v, u64 room)
{
	u8 h[HEADER_SIZE];
	if (room < HEADER_SIZE)
		return EXIT_BAD_INPUT;
	int r = part_read(&v->part, h, sizeof(h), v->offset);
	if (r)
		return r;
	u64 auth = get_be64(h + 12), aux = get_be64(h + 20);
	if (memcmp(h, "AVB0", 4) || auth > MAX_BLOB || aux > MAX_BLOB || HEADER_SIZE + auth + aux > room) {
		msg("%s holds no vbmeta image", v->part.name);
		return EXIT_BAD_INPUT;
	}
	v->len = (size_t)(HEADER_SIZE + auth + aux);
	v->blob = xmalloc(v->len);
	if ((r = part_read(&v->part, v->blob, v->len, v->offset)))
		return r;
	u64 doff = get_be64(h + 96), dsize = get_be64(h + 104);
	if (doff > aux || dsize > aux - doff) {
		msg("%s: descriptors out of range", v->part.name);
		return EXIT_BAD_INPUT;
	}
	return EXIT_OK;
}

static int vbmeta_load(struct vbmeta *v, const char *name, bool write)
{
	memset(v, 0, sizeof(*v));
	int r = part_open(&v->part, name, write);
	return r ? r : vbmeta_load_blob(v, v->part.size);
}

/* The vbmeta an image's AVB footer points at; EXIT_BAD_INPUT if it has none. p stays the caller's. */
static int vbmeta_load_footer(struct vbmeta *v, const struct part *p)
{
	memset(v, 0, sizeof(*v));
	v->part = *p;
	v->part.own_fd = false;
	if (avb_footer_read(p, &v->footer))
		return EXIT_BAD_INPUT;
	v->in_footer = true;
	v->offset = v->footer.vbmeta_offset;
	return vbmeta_load_blob(v, p->size - AVB_FOOTER_SIZE - v->offset);
}

static void vbmeta_free(struct vbmeta *v)
{
	free(v->blob);
	free(v->desc);
	part_close(&v->part);
}

static void desc_append(struct vbmeta *v, const u8 *d, size_t n)
{
	v->desc = xrealloc(v->desc, v->desc_len + n);
	memcpy(v->desc + v->desc_len, d, n);
	v->desc_len += n;
}

/*
 * Writes v with its rebuilt descriptors. vbmeta partitions and signed footer
 * blobs (which a chain verifies) are signed with the test key; an unsigned
 * footer blob (algorithm NONE: its descriptors are only copied into vbmeta at
 * build time) stays unsigned.
 */
static int vbmeta_sign_write(struct vbmeta *v)
{
	const u8 *h = v->blob;
	const u8 *aux_old = v->blob + HEADER_SIZE + get_be64(h + 12);
	u32 old_alg = get_be32(h + 28);
	bool sign = old_alg != 0 || !v->in_footer;
	u64 md_size = sign ? get_be64(h + 88) : 0;
	const u8 *md = aux_old + get_be64(h + 80);
	if (get_be64(h + 80) > get_be64(h + 20) || md_size > get_be64(h + 20) - get_be64(h + 80))
		md_size = 0;
	u64 old_ko = get_be64(h + 64), old_kl = get_be64(h + 72);
	if (old_alg && old_ko <= get_be64(h + 20) && old_kl <= get_be64(h + 20) - old_ko &&
	    (old_kl != sizeof(testkey_avb_public) || memcmp(aux_old + old_ko, testkey_avb_public, old_kl)))
		msg("note: %s was signed with another key; it is now signed with the AOSP test key, which an "
		    "unlocked bootloader accepts and a locked one may refuse", v->part.name);
	size_t key_len = sign ? sizeof(testkey_avb_public) : 0;
	size_t aux_len = ALIGN_UP(v->desc_len + key_len + md_size, 64);
	size_t auth_len = sign ? ALIGN_UP(32 + KEY_BYTES, 64) : 0;
	size_t len = HEADER_SIZE + auth_len + aux_len;
	u64 room = v->in_footer ? v->part.size - AVB_FOOTER_SIZE - v->offset : v->part.size;
	if (len > room) {
		msg("%s is too small for the re-signed vbmeta", v->part.name);
		return EXIT_BAD_INPUT;
	}

	u8 *out = xcalloc(1, MAX(len, v->len));
	memcpy(out, h, HEADER_SIZE);
	put_be64(out + 12, auth_len);
	put_be64(out + 20, aux_len);
	put_be32(out + 28, sign ? ALG_SHA256_RSA2048 : 0);
	put_be64(out + 32, 0); /* hash */
	put_be64(out + 40, sign ? 32 : 0);
	put_be64(out + 48, sign ? 32 : 0); /* signature */
	put_be64(out + 56, sign ? KEY_BYTES : 0);
	put_be64(out + 64, v->desc_len); /* public key */
	put_be64(out + 72, key_len);
	put_be64(out + 80, v->desc_len + key_len); /* public key metadata */
	put_be64(out + 88, md_size);
	put_be64(out + 96, 0); /* descriptors */
	put_be64(out + 104, v->desc_len);

	u8 *aux = out + HEADER_SIZE + auth_len;
	memcpy(aux, v->desc, v->desc_len);
	if (sign) {
		memcpy(aux + v->desc_len, testkey_avb_public, sizeof(testkey_avb_public));
		memcpy(aux + v->desc_len + sizeof(testkey_avb_public), md, md_size);
		struct sha256 s;
		u8 digest[32];
		sha256_init(&s);
		sha256_update(&s, out, HEADER_SIZE);
		sha256_update(&s, aux, aux_len);
		sha256_final(&s, digest);
		memcpy(out + HEADER_SIZE, digest, 32);
		rsa_sign(digest, out + HEADER_SIZE + 32);
	}

	size_t n = MAX(len, v->len); /* zeroes what the old blob left beyond the new one */
	int r = part_write(&v->part, out, n, v->offset);
	if (!r && v->in_footer && v->footer.vbmeta_size != len) {
		u8 raw[AVB_FOOTER_SIZE];
		v->footer.vbmeta_size = len;
		avb_footer_encode(&v->footer, raw);
		r = part_write(&v->part, raw, sizeof(raw), v->part.size - AVB_FOOTER_SIZE);
	}
	if (!r)
		r = part_sync(&v->part);
	if (!r) {
		u8 *check = xmalloc(n);
		if (part_read(&v->part, check, n, v->offset) || memcmp(check, out, n)) {
			msg("%s did not read back as written", v->part.name);
			r = EXIT_IO;
		}
		free(check);
	}
	free(out);
	return r;
}

/* ---- descriptors ---- */

static int hash_desc(struct reseal *rs, struct vbmeta *v, u8 *d, size_t n, int *dirty)
{
	u8 *f = d + 16;
	u32 nl = get_be32(f + 40), sl = get_be32(f + 44), dl = get_be32(f + 48);
	if (HASH_FIXED + (u64)nl + sl + dl > n - 16)
		return EXIT_BAD_INPUT;
	const u8 *name = f + HASH_FIXED, *salt = name + nl;
	u8 *digest = (u8 *)salt + sl;
	if (nl != strlen(rs->ramdisk_name) || memcmp(name, rs->ramdisk_name, nl))
		return EXIT_OK;
	rs->saw_ramdisk = true;
	if (strncmp((const char *)f + 8, "sha256", 32) || dl != 32) {
		msg("%s: %s is not hashed with sha256", v->part.name, rs->ramdisk_name);
		return EXIT_BAD_INPUT;
	}
	u64 size;
	u8 now[32];
	int r = bootimg_hashed_size(rs->ramdisk, &size);
	if (!r)
		r = image_digest(rs->ramdisk, size, salt, sl, now);
	if (r)
		return r;
	if (get_be64(f) != size || memcmp(digest, now, 32)) {
		put_be64(f, size);
		memcpy(digest, now, 32);
		(*dirty)++;
	}
	return EXIT_OK;
}

static int hashtree_desc(struct reseal *rs, struct vbmeta *v, u8 *d, size_t n, int *dirty)
{
	u8 *f = d + 16;
	u32 nl = get_be32(f + 88), sl = get_be32(f + 92), rl = get_be32(f + 96);
	if (HASHTREE_FIXED + (u64)nl + sl + rl > n - 16)
		return EXIT_BAD_INPUT;
	const u8 *name = f + HASHTREE_FIXED, *salt = name + nl;
	u8 *root = (u8 *)salt + sl;
	if (nl != 6 || memcmp(name, "vendor", 6))
		return EXIT_OK;
	rs->saw_vendor = true;
	u64 image_size = get_be64(f + 4), tree_offset = get_be64(f + 12), tree_size = get_be64(f + 20);
	u32 bs = get_be32(f + 28);
	bool sha1 = !strncmp((const char *)f + 56, "sha1", 32);
	if ((!sha1 && strncmp((const char *)f + 56, "sha256", 32)) || rl != (sha1 ? 20u : 32u) ||
	    bs != get_be32(f + 32) || bs < 512 || bs > 65536 || bs & (bs - 1) || tree_offset < image_size ||
	    tree_offset > rs->vendor->size || tree_size > rs->vendor->size - tree_offset) {
		msg("%s: vendor's hashtree is not one twinslot can rebuild", v->part.name);
		return EXIT_BAD_INPUT;
	}
	u8 *tree, now[32];
	u64 size;
	int r = hashtree(rs->vendor, image_size, bs, sha1, salt, sl, &tree, &size, now);
	if (r)
		return r;
	if (size != tree_size) {
		msg("%s: vendor's hashtree is %llu bytes, expected %llu", v->part.name, (unsigned long long)size,
		    (unsigned long long)tree_size);
		free(tree);
		return EXIT_BAD_INPUT;
	}
	if (memcmp(root, now, rl)) {
		r = part_write(rs->vendor, tree, (size_t)size, tree_offset);
		if (!r)
			r = part_sync(rs->vendor);
		/* The old error-correction data no longer matches; dm-verity works without it. */
		put_be32(f + 36, 0);
		put_be64(f + 40, 0);
		put_be64(f + 48, 0);
		memcpy(root, now, rl);
		(*dirty)++;
	}
	free(tree);
	return r;
}

static int reseal_blob(struct reseal *rs, struct vbmeta *v, bool top, bool *resigned);

/* The vbmeta in the edited image's footer, once (a chain may reach it, or nothing does). */
static int reseal_ramdisk_footer(struct reseal *rs, bool *resigned)
{
	if (!rs->ramdisk_footer_done) {
		struct vbmeta v;
		rs->ramdisk_footer_done = true;
		int r = vbmeta_load_footer(&v, rs->ramdisk);
		if (!r)
			r = reseal_blob(rs, &v, false, &rs->ramdisk_footer_resigned);
		else if (!v.in_footer)
			r = EXIT_OK; /* no footer: vbmeta's own descriptor is the only one */
		vbmeta_free(&v);
		if (r)
			return r;
	}
	*resigned = rs->ramdisk_footer_resigned;
	return EXIT_OK;
}

/* The same for vendor's own footer (BOARD_AVB_VENDOR_KEY_PATH chains vbmeta to it). */
static int reseal_vendor_footer(struct reseal *rs, bool *resigned)
{
	if (!rs->vendor_footer_done) {
		struct vbmeta v;
		rs->vendor_footer_done = true;
		int r = vbmeta_load_footer(&v, rs->vendor);
		if (!r)
			r = reseal_blob(rs, &v, false, &rs->vendor_footer_resigned);
		else if (!v.in_footer)
			r = EXIT_OK;
		vbmeta_free(&v);
		if (r)
			return r;
	}
	*resigned = rs->vendor_footer_resigned;
	return EXIT_OK;
}

static int chain_desc(struct reseal *rs, struct vbmeta *v, const u8 *d, size_t n, int *dirty)
{
	const u8 *f = d + 16;
	u32 nl = get_be32(f + 4), kl = get_be32(f + 8);
	if (CHAIN_FIXED + (u64)nl + kl > n - 16 || nl >= 48) {
		desc_append(v, d, n);
		return EXIT_OK;
	}
	char part[64];
	memcpy(part, f + CHAIN_FIXED, nl);
	part[nl] = 0;
	bool resigned = false;
	int r = EXIT_OK;
	if (!strcmp(part, rs->ramdisk_name)) {
		/* boot or vendor_boot verified through its own footer. */
		r = reseal_ramdisk_footer(rs, &resigned);
	} else if (!strcmp(part, "vendor")) {
		r = reseal_vendor_footer(rs, &resigned);
	} else if (!strncmp(part, "vbmeta_", 7)) {
		snprintf(part + nl, sizeof(part) - nl, "_%c", slot_letter(rs->slot));
		if (dev_exists(part)) {
			struct vbmeta cv;
			if (!(r = vbmeta_load(&cv, part, true)))
				r = reseal_blob(rs, &cv, false, &resigned);
			vbmeta_free(&cv);
		}
	}
	if (r)
		return r;
	const u8 *key = f + CHAIN_FIXED + nl;
	if (!resigned || (kl == sizeof(testkey_avb_public) && !memcmp(key, testkey_avb_public, kl))) {
		desc_append(v, d, n);
		return EXIT_OK;
	}
	/* The chained vbmeta is now signed with the test key: point the chain at it. */
	size_t body = CHAIN_FIXED + nl + sizeof(testkey_avb_public), len = 16 + ALIGN_UP(body, 8);
	u8 *c = xcalloc(1, len);
	put_be64(c, TAG_CHAIN);
	put_be64(c + 8, len - 16);
	memcpy(c + 16, f, CHAIN_FIXED);
	put_be32(c + 16 + 8, sizeof(testkey_avb_public));
	memcpy(c + 16 + CHAIN_FIXED, f + CHAIN_FIXED, nl);
	memcpy(c + 16 + CHAIN_FIXED + nl, testkey_avb_public, sizeof(testkey_avb_public));
	desc_append(v, c, len);
	free(c);
	(*dirty)++;
	return EXIT_OK;
}

static int reseal_blob(struct reseal *rs, struct vbmeta *v, bool top, bool *resigned)
{
	int dirty = 0, r = EXIT_OK;
	const u8 *h = v->blob;
	u8 *desc = v->blob + HEADER_SIZE + get_be64(h + 12) + get_be64(h + 96);
	u64 dsize = get_be64(h + 104);
	for (u64 off = 0; off + 16 <= dsize && !r;) {
		u8 *d = desc + off;
		u64 body = get_be64(d + 8);
		if (body > dsize - off - 16) {
			msg("%s: a descriptor runs past the end", v->part.name);
			r = EXIT_BAD_INPUT;
			break;
		}
		u64 n = 16 + body;
		switch (get_be64(d)) {
		case TAG_HASH:
			if (!(r = hash_desc(rs, v, d, (size_t)n, &dirty)))
				desc_append(v, d, (size_t)n);
			break;
		case TAG_HASHTREE:
			if (!(r = hashtree_desc(rs, v, d, (size_t)n, &dirty)))
				desc_append(v, d, (size_t)n);
			break;
		case TAG_CHAIN:
			r = chain_desc(rs, v, d, (size_t)n, &dirty);
			break;
		default:
			desc_append(v, d, (size_t)n);
		}
		off += n;
	}
	if (!r && top) {
		u32 flags = get_be32(v->blob + FLAGS_OFF);
		if (flags & (FLAG_HASHTREE_DISABLED | FLAG_VERIFICATION_DISABLED)) {
			put_be32(v->blob + FLAGS_OFF, flags & ~(FLAG_HASHTREE_DISABLED | FLAG_VERIFICATION_DISABLED));
			dirty++;
		}
	}
	if (!r && dirty)
		r = vbmeta_sign_write(v);
	if (!r) {
		rs->changed += dirty;
		*resigned = dirty > 0;
	}
	return r;
}

int avb_reseal(int slot, const struct part *ramdisk, const char *ramdisk_name, const struct part *vendor, int *changed)
{
	char name[32];
	snprintf(name, sizeof(name), "vbmeta_%c", slot_letter(slot));
	*changed = 0;
	struct reseal rs = {.slot = slot, .ramdisk = ramdisk, .vendor = vendor, .ramdisk_name = ramdisk_name};
	bool resigned, top = dev_exists(name);
	int r = EXIT_OK;
	if (top) {
		struct vbmeta v;
		if (!(r = vbmeta_load(&v, name, true)))
			r = reseal_blob(&rs, &v, true, &resigned);
		vbmeta_free(&v);
	}
	/* A footer no chain reached is kept true too, though only vbmeta's copy of its descriptor is used. */
	if (!r)
		r = reseal_ramdisk_footer(&rs, &resigned);
	/* Vendor's footer only when nothing else described it: then it is the one a chain would verify. */
	if (!r && !rs.saw_vendor)
		r = reseal_vendor_footer(&rs, &resigned);
	/* An image whose descriptor lives elsewhere still holds the old digest. */
	if (!r && top && (!rs.saw_ramdisk || !rs.saw_vendor))
		msg("warning: the vbmeta chain of slot %c does not describe %s; its old digest is not updated",
		    slot_letter(slot), !rs.saw_ramdisk && !rs.saw_vendor ? "the boot image or vendor"
				       : rs.saw_vendor ? ramdisk_name : "vendor");
	*changed = rs.changed;
	return r;
}
