#include "hash.h"

#include <string.h>

u32 crc32c_raw(u32 crc, const void *buf, size_t n)
{
	static u32 table[256];
	if (!table[1])
		for (u32 i = 0; i < 256; i++) {
			u32 c = i;
			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0x82F63B78u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	const u8 *p = buf;
	while (n--)
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return crc;
}

u32 crc32_update(u32 crc, const void *buf, size_t n)
{
	static u32 table[256];
	const u8 *p = buf;

	if (!table[1]) {
		for (u32 i = 0; i < 256; i++) {
			u32 c = i;
			for (int k = 0; k < 8; k++)
				c = (c >> 1) ^ (0xedb88320u & -(c & 1));
			table[i] = c;
		}
	}
	crc = ~crc;
	while (n--)
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

/* ---- SHA-256 (FIPS 180-4) ---- */

static const u32 K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline u32 ror(u32 x, int n) { return x >> n | x << (32 - n); }

static void sha256_block(u32 st[8], const u8 *p)
{
	u32 w[64];
	for (int i = 0; i < 16; i++)
		w[i] = get_be32(p + 4 * i);
	for (int i = 16; i < 64; i++) {
		u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
		u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	u32 a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
	for (int i = 0; i < 64; i++) {
		u32 t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		u32 t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	st[0] += a;
	st[1] += b;
	st[2] += c;
	st[3] += d;
	st[4] += e;
	st[5] += f;
	st[6] += g;
	st[7] += h;
}

void sha256_init(struct sha256 *s)
{
	static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
				  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(s->state, iv, sizeof(iv));
	s->bytes = 0;
	s->fill = 0;
}

void sha256_update(struct sha256 *s, const void *buf, size_t n)
{
	const u8 *p = buf;
	s->bytes += n;
	if (s->fill) {
		size_t take = MIN(n, 64 - s->fill);
		memcpy(s->block + s->fill, p, take);
		s->fill += take;
		p += take;
		n -= take;
		if (s->fill < 64)
			return;
		sha256_block(s->state, s->block);
		s->fill = 0;
	}
	for (; n >= 64; p += 64, n -= 64)
		sha256_block(s->state, p);
	memcpy(s->block, p, n);
	s->fill = n;
}

void sha256_final(struct sha256 *s, u8 digest[32])
{
	u64 bits = s->bytes * 8;
	u8 pad[72] = {0x80};
	size_t padlen = (s->fill < 56 ? 56 : 120) - s->fill;
	for (int i = 0; i < 8; i++)
		pad[padlen + i] = (u8)(bits >> (56 - 8 * i));
	sha256_update(s, pad, padlen + 8);
	for (int i = 0; i < 8; i++)
		put_be32(digest + 4 * i, s->state[i]);
}

void sha256(const void *buf, size_t n, u8 digest[32])
{
	struct sha256 s;
	sha256_init(&s);
	sha256_update(&s, buf, n);
	sha256_final(&s, digest);
}

/* ---- SHA-1: only for the id field mkbootimg writes into boot image headers v0-v2 ---- */

static u32 rol32(u32 x, int n) { return x << n | x >> (32 - n); }

static void sha1_block(struct sha1 *s, const u8 *p)
{
	u32 w[80];
	for (int i = 0; i < 16; i++)
		w[i] = get_be32(p + 4 * i);
	for (int i = 16; i < 80; i++)
		w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	u32 a = s->state[0], b = s->state[1], c = s->state[2], d = s->state[3], e = s->state[4];
	for (int i = 0; i < 80; i++) {
		u32 f, k;
		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ c ^ d;
			k = 0xca62c1d6;
		}
		u32 t = rol32(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = rol32(b, 30);
		b = a;
		a = t;
	}
	s->state[0] += a;
	s->state[1] += b;
	s->state[2] += c;
	s->state[3] += d;
	s->state[4] += e;
}

void sha1_init(struct sha1 *s)
{
	static const u32 iv[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
	memcpy(s->state, iv, sizeof(iv));
	s->bytes = 0;
	s->fill = 0;
}

void sha1_update(struct sha1 *s, const void *buf, size_t n)
{
	const u8 *p = buf;
	s->bytes += n;
	while (n) {
		size_t take = MIN(n, 64 - s->fill);
		memcpy(s->block + s->fill, p, take);
		s->fill += take;
		p += take;
		n -= take;
		if (s->fill == 64) {
			sha1_block(s, s->block);
			s->fill = 0;
		}
	}
}

void sha1_final(struct sha1 *s, u8 digest[20])
{
	u64 bits = s->bytes * 8;
	u8 pad[72] = {0x80};
	size_t padlen = (s->fill < 56 ? 56 : 120) - s->fill;
	sha1_update(s, pad, padlen);
	for (int i = 0; i < 8; i++)
		pad[i] = (u8)(bits >> (56 - 8 * i));
	sha1_update(s, pad, 8);
	for (int i = 0; i < 5; i++)
		put_be32(digest + 4 * i, s->state[i]);
}
