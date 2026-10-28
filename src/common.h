/* Shared helpers: types, byte order, I/O, messages, device paths. */
#ifndef TWINSLOT_COMMON_H
#define TWINSLOT_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* Exit codes, also returned by the command functions. */
enum {
	EXIT_OK = 0,
	EXIT_USAGE = 1,
	EXIT_REFUSED = 2, /* a safety check failed; nothing was written */
	EXIT_IO = 3,
	EXIT_BAD_INPUT = 4, /* an image, zip or table is not what we expected */
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a) (((x) + (a) - 1) / (a) * (a))
#define DIV_ROUND_UP(x, a) (((x) + (a) - 1) / (a))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static inline u16 get_le16(const void *p)
{
	const u8 *b = p;
	return (u16)(b[0] | b[1] << 8);
}

static inline u32 get_le32(const void *p)
{
	const u8 *b = p;
	return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24;
}

static inline u64 get_le64(const void *p) { return get_le32(p) | (u64)get_le32((const u8 *)p + 4) << 32; }

static inline u32 get_be32(const void *p)
{
	const u8 *b = p;
	return (u32)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

static inline u64 get_be64(const void *p) { return (u64)get_be32(p) << 32 | get_be32((const u8 *)p + 4); }

static inline void put_le16(void *p, u16 v)
{
	u8 *b = p;
	b[0] = (u8)v;
	b[1] = (u8)(v >> 8);
}

static inline void put_le32(void *p, u32 v)
{
	u8 *b = p;
	b[0] = (u8)v;
	b[1] = (u8)(v >> 8);
	b[2] = (u8)(v >> 16);
	b[3] = (u8)(v >> 24);
}

static inline void put_le64(void *p, u64 v)
{
	put_le32(p, (u32)v);
	put_le32((u8 *)p + 4, (u32)(v >> 32));
}

static inline void put_be32(void *p, u32 v)
{
	u8 *b = p;
	b[0] = (u8)(v >> 24);
	b[1] = (u8)(v >> 16);
	b[2] = (u8)(v >> 8);
	b[3] = (u8)v;
}

/* ---- messages ---- */

/* "twinslot: ..." on stderr. */
void msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Plain stdout line; status output that the app parses. */
void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* "progress <permille> <text>" on stdout, flushed, for the app's progress bar. */
void progress(unsigned permille, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- memory ---- */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* ---- I/O: whole-buffer reads and writes, -1 on error with a message ---- */

int io_open(const char *path, bool write);
int io_pread(int fd, void *buf, size_t n, u64 off);
int io_pwrite(int fd, const void *buf, size_t n, u64 off);
int io_sync(int fd);
/* Drops cached pages of [off, off+n) so a check read after io_sync sees the medium, not the cache. */
void io_uncache(int fd, u64 off, size_t n);
/* Size of a regular file or block device. */
int io_size(int fd, u64 *size);
/* Reads a whole small file (proc, sysfs). Returns a NUL-terminated buffer or NULL. */
char *read_text_file(const char *path, size_t max);

/* ---- devices ---- */

/*
 * Where partitions are found: /dev/block/by-name on a phone, or a directory of
 * image files (--dev-dir) for tests and for working on dumps on a PC.
 */
#define DEV_DIR_PHONE "/dev/block/by-name"
extern const char *g_dev_dir;
/* --dev-dir: partitions are image files in dir (unless dir is DEV_DIR_PHONE). */
void dev_dir_set(const char *dir);
/* On a phone without DEV_DIR_PHONE, uses the platform by-name directory that holds userdata. */
void dev_dir_find(void);
/* g_dev_dir is the phone's own partitions, not a directory of images. */
bool dev_dir_is_phone(void);
/* Running from recovery (or on image files), so no ROM is using the partitions. */
bool in_recovery(void);
/* Path of partition `name` (e.g. "boot_b"); valid until the next call. */
const char *dev_path(const char *name);
bool dev_exists(const char *name);

/* 0 = a, 1 = b, -1 = unknown. From bootconfig or the kernel command line. */
int detect_current_slot(void);
static inline char slot_letter(int slot) { return (char)('a' + slot); }

/* SHA-256 as 64 hex chars + NUL. */
void hex(char *out, const u8 *in, size_t n);

#endif
