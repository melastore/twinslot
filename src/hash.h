/* CRC-32 (zlib polynomial, as GPT and bootloader_control use) and SHA-256. */
#ifndef TWINSLOT_HASH_H
#define TWINSLOT_HASH_H

#include "common.h"

u32 crc32_update(u32 crc, const void *buf, size_t n);
static inline u32 crc32(const void *buf, size_t n) { return crc32_update(0, buf, n); }

/* CRC-32C (Castagnoli) as the Linux kernel's crc32c(): no inversion in or out. */
u32 crc32c_raw(u32 crc, const void *buf, size_t n);

struct sha256 {
	u32 state[8];
	u64 bytes;
	u8 block[64];
	size_t fill;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *buf, size_t n);
void sha256_final(struct sha256 *s, u8 digest[32]);
void sha256(const void *buf, size_t n, u8 digest[32]);

struct sha1 {
	u32 state[5];
	u64 bytes;
	u8 block[64];
	size_t fill;
};

void sha1_init(struct sha1 *s);
void sha1_update(struct sha1 *s, const void *buf, size_t n);
void sha1_final(struct sha1 *s, u8 digest[20]);

#endif
