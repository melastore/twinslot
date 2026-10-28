#ifndef TWINSLOT_DECOMPRESS_H
#define TWINSLOT_DECOMPRESS_H

#include "common.h"

enum codec { CODEC_XZ, CODEC_BZIP2, CODEC_ZSTD };

/* Decompresses exactly outlen bytes; anything else is an error. */
int decompress(enum codec c, const u8 *in, size_t inlen, u8 *out, size_t outlen);

/* Raw LZ4 block: returns the decompressed size, or -1. */
long lz4_block_decompress(const u8 *in, size_t inlen, u8 *out, size_t cap);
/* LZ4 HC at the highest level; returns the compressed size, 0 if it does not fit in cap. */
size_t lz4_block_compress_hc(const u8 *in, size_t inlen, u8 *out, size_t cap);

#endif
