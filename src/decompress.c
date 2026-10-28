#include "decompress.h"

#include <limits.h>

#include "bzlib.h"
#include "lz4.h"
#include "lz4hc.h"
#include "xz.h"
#include "zstd.h"

static int decompress_xz(const u8 *in, size_t inlen, u8 *out, size_t outlen)
{
	static bool crc_ready;
	if (!crc_ready) {
		xz_crc32_init();
		xz_crc64_init();
		crc_ready = true;
	}
	struct xz_dec *dec = xz_dec_init(XZ_SINGLE, 0);
	if (!dec)
		return EXIT_IO;
	struct xz_buf b = {.in = in, .in_size = inlen, .out = out, .out_size = outlen};
	enum xz_ret r = xz_dec_run(dec, &b);
	xz_dec_end(dec);
	return r == XZ_STREAM_END && b.out_pos == outlen ? EXIT_OK : EXIT_BAD_INPUT;
}

static int decompress_bzip2(const u8 *in, size_t inlen, u8 *out, size_t outlen)
{
	if (inlen > UINT_MAX || outlen > UINT_MAX)
		return EXIT_BAD_INPUT;
	unsigned int got = (unsigned int)outlen;
	int r = BZ2_bzBuffToBuffDecompress((char *)out, &got, (char *)in, (unsigned int)inlen, 0, 0);
	return r == BZ_OK && got == outlen ? EXIT_OK : EXIT_BAD_INPUT;
}

static int decompress_zstd(const u8 *in, size_t inlen, u8 *out, size_t outlen)
{
	size_t r = ZSTD_decompress(out, outlen, in, inlen);
	return !ZSTD_isError(r) && r == outlen ? EXIT_OK : EXIT_BAD_INPUT;
}

int decompress(enum codec c, const u8 *in, size_t inlen, u8 *out, size_t outlen)
{
	switch (c) {
	case CODEC_XZ:
		return decompress_xz(in, inlen, out, outlen);
	case CODEC_BZIP2:
		return decompress_bzip2(in, inlen, out, outlen);
	case CODEC_ZSTD:
		return decompress_zstd(in, inlen, out, outlen);
	}
	return EXIT_BAD_INPUT;
}

long lz4_block_decompress(const u8 *in, size_t inlen, u8 *out, size_t cap)
{
	if (inlen > INT_MAX || cap > INT_MAX)
		return -1;
	int n = LZ4_decompress_safe((const char *)in, (char *)out, (int)inlen, (int)cap);
	return n < 0 ? -1 : n;
}

size_t lz4_block_compress_hc(const u8 *in, size_t inlen, u8 *out, size_t cap)
{
	if (inlen > INT_MAX || cap > INT_MAX)
		return 0;
	int n = LZ4_compress_HC((const char *)in, (char *)out, (int)inlen, (int)cap, LZ4HC_CLEVEL_MAX);
	return n > 0 ? (size_t)n : 0;
}
