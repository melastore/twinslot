#include "part.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hash.h"

int part_open(struct part *p, const char *name, bool write)
{
	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), "%s", name);
	p->fd = io_open(dev_path(name), write);
	if (p->fd < 0)
		return EXIT_IO;
	p->own_fd = true;
	if (io_size(p->fd, &p->size)) {
		msg("cannot get the size of %s", name);
		part_close(p);
		return EXIT_IO;
	}
	return EXIT_OK;
}

void part_logical(struct part *p, int super_fd, const struct lp_partition *lp)
{
	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), "%s", lp->name);
	p->fd = super_fd;
	p->lp = lp;
	p->size = lp_partition_size(lp);
}

void part_close(struct part *p)
{
	if (p->own_fd && p->fd >= 0)
		close(p->fd);
	p->fd = -1;
}

static int range_ok(const struct part *p, size_t n, u64 off)
{
	if (off > p->size || n > p->size - off) {
		msg("%s: access past its end (%llu + %zu > %llu)", p->name, (unsigned long long)off, n,
		    (unsigned long long)p->size);
		return -1;
	}
	return 0;
}

static int xfer(const struct part *p, void *buf, size_t n, u64 off, bool write)
{
	if (range_ok(p, n, off))
		return EXIT_BAD_INPUT;
	u8 *b = buf;
	while (n) {
		u64 phys = off, run = n;
		if (p->lp && !lp_map(p->lp, off, &phys, &run)) {
			msg("%s: offset %llu is not mapped", p->name, (unsigned long long)off);
			return EXIT_BAD_INPUT;
		}
		size_t chunk = (size_t)MIN((u64)n, run);
		int r = write ? io_pwrite(p->fd, b, chunk, phys) : io_pread(p->fd, b, chunk, phys);
		if (r)
			return EXIT_IO;
		b += chunk;
		off += chunk;
		n -= chunk;
	}
	return EXIT_OK;
}

int part_read(const struct part *p, void *buf, size_t n, u64 off) { return xfer(p, buf, n, off, false); }

int part_write(const struct part *p, const void *buf, size_t n, u64 off)
{
	return xfer(p, (void *)buf, n, off, true);
}

int part_sync(const struct part *p)
{
	if (io_sync(p->fd))
		return EXIT_IO;
	/* Reads after this (the checks) come from the medium. */
	io_uncache(p->fd, 0, 0);
	return EXIT_OK;
}

int part_sha256(const struct part *p, u64 len, u8 digest[32])
{
	enum { CHUNK = 1 << 20 };
	u8 *buf = xmalloc(CHUNK);
	struct sha256 s;
	sha256_init(&s);
	int r = EXIT_OK;
	for (u64 off = 0; off < len && !r; off += CHUNK) {
		size_t n = (size_t)MIN((u64)CHUNK, len - off);
		r = part_read(p, buf, n, off);
		if (!r)
			sha256_update(&s, buf, n);
	}
	free(buf);
	sha256_final(&s, digest);
	return r;
}

int super_read_slot(struct part *super, bool write, int slot, struct lp_metadata *m)
{
	int r = part_open(super, "super", write);
	if (r)
		return r;
	if ((r = lp_read(super->fd, (u32)slot, m))) {
		part_close(super);
		return r;
	}
	int cur = detect_current_slot();
	struct lp_metadata live;
	if (cur >= 0 && cur != slot && lp_read(super->fd, (u32)cur, &live) == EXIT_OK) {
		const char *hit = lp_stale_overlap(m, slot_letter(slot), &live, slot_letter(cur));
		if (hit) {
			msg("slot %c's partition list is out of date: %s lies where slot %c's ROM is now", slot_letter(slot),
			    hit, slot_letter(cur));
			msg("install a ROM into slot %c with twinslot first", slot_letter(slot));
			r = EXIT_REFUSED;
		}
		lp_free(&live);
	}
	if (r) {
		lp_free(m);
		part_close(super);
	}
	return r;
}
