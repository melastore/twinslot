#include "lp.h"

#include <stdlib.h>
#include <string.h>

#include "hash.h"

/* liblp metadata_format.h */
#define LP_RESERVED_BYTES 4096
#define LP_GEOMETRY_SIZE 4096
#define LP_GEOMETRY_MAGIC 0x616c4467u
#define LP_GEOMETRY_STRUCT_SIZE 52
#define LP_HEADER_MAGIC 0x414c5030u
#define LP_HEADER_V1_0_SIZE 128
#define LP_HEADER_V1_2_SIZE 256

#define LP_PARTITION_ENTRY 52
#define LP_EXTENT_ENTRY 24
#define LP_GROUP_ENTRY 48
#define LP_BDEV_ENTRY 64

static u64 primary_offset(const struct lp_metadata *m, u32 slot)
{
	return LP_RESERVED_BYTES + 2 * LP_GEOMETRY_SIZE + (u64)slot * m->metadata_max_size;
}

static u64 backup_offset(const struct lp_metadata *m, u32 slot)
{
	return primary_offset(m, m->metadata_slot_count) + (u64)slot * m->metadata_max_size;
}

static void copy_name(char *dst, const u8 *src)
{
	memcpy(dst, src, LP_NAME_LEN);
	dst[LP_NAME_LEN] = 0;
}

static int read_geometry(int fd, struct lp_metadata *m)
{
	u8 g[LP_GEOMETRY_STRUCT_SIZE];
	for (int copy = 0; copy < 2; copy++) {
		if (io_pread(fd, g, sizeof(g), LP_RESERVED_BYTES + (u64)copy * LP_GEOMETRY_SIZE))
			return EXIT_IO;
		if (get_le32(g) != LP_GEOMETRY_MAGIC || get_le32(g + 4) != LP_GEOMETRY_STRUCT_SIZE)
			continue;
		u8 want[32], got[32];
		memcpy(want, g + 8, 32);
		memset(g + 8, 0, 32);
		sha256(g, sizeof(g), got);
		if (memcmp(want, got, 32))
			continue;
		m->metadata_max_size = get_le32(g + 40);
		m->metadata_slot_count = get_le32(g + 44);
		m->logical_block_size = get_le32(g + 48);
		if (m->metadata_max_size < LP_HEADER_V1_2_SIZE || m->metadata_max_size > (16u << 20) ||
		    m->metadata_slot_count == 0 || m->metadata_slot_count > 3 || !m->logical_block_size ||
		    m->logical_block_size % LP_SECTOR)
			continue;
		return EXIT_OK;
	}
	msg("super has no valid dynamic partition geometry");
	return EXIT_BAD_INPUT;
}

/* Parses one serialized metadata copy; EXIT_BAD_INPUT if anything is off. */
static int parse(struct lp_metadata *m, u8 *buf)
{
	if (get_le32(buf) != LP_HEADER_MAGIC)
		return EXIT_BAD_INPUT;
	m->major = get_le16(buf + 4);
	m->minor = get_le16(buf + 6);
	m->header_size = get_le32(buf + 8);
	if (m->major != 10 || (m->header_size != LP_HEADER_V1_0_SIZE && m->header_size != LP_HEADER_V1_2_SIZE))
		return EXIT_BAD_INPUT;

	u32 tables_size = get_le32(buf + 44);
	if ((u64)m->header_size + tables_size > m->metadata_max_size)
		return EXIT_BAD_INPUT;

	u8 want[32], got[32];
	memcpy(want, buf + 12, 32);
	memset(buf + 12, 0, 32);
	sha256(buf, m->header_size, got);
	memcpy(buf + 12, want, 32);
	if (memcmp(want, got, 32))
		return EXIT_BAD_INPUT;
	sha256(buf + m->header_size, tables_size, got);
	if (memcmp(buf + 48, got, 32))
		return EXIT_BAD_INPUT;
	m->flags = m->header_size >= LP_HEADER_V1_2_SIZE ? get_le32(buf + 128) : 0;

	const u8 *tables = buf + m->header_size;
	struct {
		u32 off, num, size;
	} d[4];
	static const u32 min_size[4] = {LP_PARTITION_ENTRY, LP_EXTENT_ENTRY, LP_GROUP_ENTRY, LP_BDEV_ENTRY};
	for (int i = 0; i < 4; i++) {
		d[i].off = get_le32(buf + 80 + 12 * i);
		d[i].num = get_le32(buf + 84 + 12 * i);
		d[i].size = get_le32(buf + 88 + 12 * i);
		if (d[i].num && (d[i].size < min_size[i] || (u64)d[i].off + (u64)d[i].num * d[i].size > tables_size))
			return EXIT_BAD_INPUT;
	}

	m->ngroups = d[2].num;
	m->groups = xcalloc(m->ngroups, sizeof(*m->groups));
	for (u32 i = 0; i < m->ngroups; i++) {
		const u8 *e = tables + d[2].off + (u64)i * d[2].size;
		copy_name(m->groups[i].name, e);
		m->groups[i].flags = get_le32(e + 36);
		m->groups[i].max_size = get_le64(e + 40);
	}

	m->nbdevs = d[3].num;
	m->bdevs = xcalloc(m->nbdevs, sizeof(*m->bdevs));
	for (u32 i = 0; i < m->nbdevs; i++) {
		const u8 *e = tables + d[3].off + (u64)i * d[3].size;
		struct lp_bdev *b = &m->bdevs[i];
		b->first_logical_sector = get_le64(e);
		b->alignment = get_le32(e + 8);
		b->alignment_offset = get_le32(e + 12);
		b->size = get_le64(e + 16);
		copy_name(b->name, e + 24);
		b->flags = get_le32(e + 60);
	}

	m->nparts = d[0].num;
	m->parts = xcalloc(m->nparts, sizeof(*m->parts));
	for (u32 i = 0; i < m->nparts; i++) {
		const u8 *e = tables + d[0].off + (u64)i * d[0].size;
		struct lp_partition *p = &m->parts[i];
		copy_name(p->name, e);
		p->attrs = get_le32(e + 36);
		u32 first = get_le32(e + 40);
		p->nextents = get_le32(e + 44);
		p->group = get_le32(e + 48);
		if ((u64)first + p->nextents > d[1].num || p->group >= m->ngroups)
			return EXIT_BAD_INPUT;
		p->ext = xcalloc(p->nextents, sizeof(*p->ext));
		for (u32 k = 0; k < p->nextents; k++) {
			const u8 *x = tables + d[1].off + (u64)(first + k) * d[1].size;
			p->ext[k].num_sectors = get_le64(x);
			p->ext[k].target_type = get_le32(x + 8);
			p->ext[k].target_data = get_le64(x + 12);
			p->ext[k].target_source = get_le32(x + 20);
			if (p->ext[k].target_source >= m->nbdevs)
				return EXIT_BAD_INPUT;
			/* A linear extent must lie inside its block device (zero extents have no target). */
			u64 dev_sectors = m->bdevs[p->ext[k].target_source].size / LP_SECTOR;
			if (p->ext[k].target_type == LP_TARGET_LINEAR && (p->ext[k].target_data > dev_sectors ||
							   p->ext[k].num_sectors > dev_sectors - p->ext[k].target_data))
				return EXIT_BAD_INPUT;
		}
	}
	return EXIT_OK;
}

void lp_free(struct lp_metadata *m)
{
	for (u32 i = 0; i < m->nparts; i++)
		free(m->parts[i].ext);
	free(m->parts);
	free(m->groups);
	free(m->bdevs);
	memset(m, 0, sizeof(*m));
}

int lp_read(int fd, u32 slot, struct lp_metadata *m)
{
	memset(m, 0, sizeof(*m));
	int r = read_geometry(fd, m);
	if (r)
		return r;
	if (slot >= m->metadata_slot_count)
		slot = 0;

	u8 *buf = xmalloc(m->metadata_max_size);
	u64 offsets[2] = {primary_offset(m, slot), backup_offset(m, slot)};
	for (int copy = 0; copy < 2; copy++) {
		struct lp_metadata geo = *m;
		if (io_pread(fd, buf, m->metadata_max_size, offsets[copy])) {
			free(buf);
			return EXIT_IO;
		}
		if (parse(m, buf) == EXIT_OK) {
			free(buf);
			if (m->nbdevs != 1) {
				msg("super spans %u block devices; only one is supported", m->nbdevs);
				lp_free(m);
				return EXIT_BAD_INPUT;
			}
			return EXIT_OK;
		}
		lp_free(m);
		*m = geo;
	}
	free(buf);
	msg("super metadata slot %u is damaged (primary and backup)", slot);
	return EXIT_BAD_INPUT;
}

/* Serializes m into buf (metadata_max_size bytes, zero filled). */

/* Names are fixed 36-byte fields, NUL-padded, not necessarily NUL-terminated. */
static void put_name(u8 *field, const char *name)
{
	memset(field, 0, LP_NAME_LEN);
	memcpy(field, name, strnlen(name, LP_NAME_LEN));
}

static int serialize(const struct lp_metadata *m, u8 *buf)
{
	u32 nextents = 0;
	for (u32 i = 0; i < m->nparts; i++)
		nextents += m->parts[i].nextents;

	u32 part_off = 0;
	u32 ext_off = part_off + m->nparts * LP_PARTITION_ENTRY;
	u32 group_off = ext_off + nextents * LP_EXTENT_ENTRY;
	u32 bdev_off = group_off + m->ngroups * LP_GROUP_ENTRY;
	u32 tables_size = bdev_off + m->nbdevs * LP_BDEV_ENTRY;
	if ((u64)m->header_size + tables_size > m->metadata_max_size) {
		msg("the partition table does not fit in super's metadata area");
		return EXIT_REFUSED;
	}

	memset(buf, 0, m->metadata_max_size);
	u8 *t = buf + m->header_size;
	u32 ext_index = 0;
	for (u32 i = 0; i < m->nparts; i++) {
		const struct lp_partition *p = &m->parts[i];
		u8 *e = t + part_off + i * LP_PARTITION_ENTRY;
		put_name(e, p->name);
		put_le32(e + 36, p->attrs);
		put_le32(e + 40, p->nextents ? ext_index : 0);
		put_le32(e + 44, p->nextents);
		put_le32(e + 48, p->group);
		for (u32 k = 0; k < p->nextents; k++, ext_index++) {
			u8 *x = t + ext_off + ext_index * LP_EXTENT_ENTRY;
			put_le64(x, p->ext[k].num_sectors);
			put_le32(x + 8, p->ext[k].target_type);
			put_le64(x + 12, p->ext[k].target_data);
			put_le32(x + 20, p->ext[k].target_source);
		}
	}
	for (u32 i = 0; i < m->ngroups; i++) {
		u8 *e = t + group_off + i * LP_GROUP_ENTRY;
		put_name(e, m->groups[i].name);
		put_le32(e + 36, m->groups[i].flags);
		put_le64(e + 40, m->groups[i].max_size);
	}
	for (u32 i = 0; i < m->nbdevs; i++) {
		const struct lp_bdev *b = &m->bdevs[i];
		u8 *e = t + bdev_off + i * LP_BDEV_ENTRY;
		put_le64(e, b->first_logical_sector);
		put_le32(e + 8, b->alignment);
		put_le32(e + 12, b->alignment_offset);
		put_le64(e + 16, b->size);
		put_name(e + 24, b->name);
		put_le32(e + 60, b->flags);
	}

	put_le32(buf, LP_HEADER_MAGIC);
	put_le16(buf + 4, m->major);
	put_le16(buf + 6, m->minor);
	put_le32(buf + 8, m->header_size);
	put_le32(buf + 44, tables_size);
	u32 descs[4][3] = {
		{part_off, m->nparts, LP_PARTITION_ENTRY},
		{ext_off, nextents, LP_EXTENT_ENTRY},
		{group_off, m->ngroups, LP_GROUP_ENTRY},
		{bdev_off, m->nbdevs, LP_BDEV_ENTRY},
	};
	for (int i = 0; i < 4; i++)
		for (int k = 0; k < 3; k++)
			put_le32(buf + 80 + 12 * i + 4 * k, descs[i][k]);
	if (m->header_size >= LP_HEADER_V1_2_SIZE)
		put_le32(buf + 128, m->flags);
	sha256(t, tables_size, buf + 48);
	sha256(buf, m->header_size, buf + 12);
	return EXIT_OK;
}

int lp_write(int fd, const struct lp_metadata *m)
{
	u8 *buf = xmalloc(m->metadata_max_size);
	u8 *check = xmalloc(m->metadata_max_size);
	int r = serialize(m, buf);

	/* Backups first: a crash part way leaves every slot with one good copy. */
	for (int pass = 0; !r && pass < 2; pass++) {
		for (u32 s = 0; !r && s < m->metadata_slot_count; s++) {
			u64 off = pass == 0 ? backup_offset(m, s) : primary_offset(m, s);
			if (io_pwrite(fd, buf, m->metadata_max_size, off))
				r = EXIT_IO;
		}
		if (!r && io_sync(fd))
			r = EXIT_IO;
	}
	/* Read back from the medium, not the page cache. */
	for (u32 s = 0; !r && s < m->metadata_slot_count; s++) {
		io_uncache(fd, backup_offset(m, s), m->metadata_max_size);
		io_uncache(fd, primary_offset(m, s), m->metadata_max_size);
	}
	for (u32 s = 0; !r && s < m->metadata_slot_count; s++) {
		for (int pass = 0; !r && pass < 2; pass++) {
			u64 off = pass == 0 ? backup_offset(m, s) : primary_offset(m, s);
			if (io_pread(fd, check, m->metadata_max_size, off) || memcmp(buf, check, m->metadata_max_size)) {
				msg("super metadata did not read back as written");
				r = EXIT_IO;
			}
		}
	}
	free(buf);
	free(check);
	return r;
}

int lp_find_partition(const struct lp_metadata *m, const char *name)
{
	for (u32 i = 0; i < m->nparts; i++)
		if (!strcmp(m->parts[i].name, name))
			return (int)i;
	return -1;
}

int lp_find_group(const struct lp_metadata *m, const char *name)
{
	for (u32 i = 0; i < m->ngroups; i++)
		if (!strcmp(m->groups[i].name, name))
			return (int)i;
	return -1;
}

int lp_add_group(struct lp_metadata *m, const char *name, u64 max_size)
{
	if (strlen(name) > LP_NAME_LEN)
		return -1;
	m->groups = xrealloc(m->groups, (m->ngroups + 1) * sizeof(*m->groups));
	struct lp_group *g = &m->groups[m->ngroups];
	memset(g, 0, sizeof(*g));
	strcpy(g->name, name);
	g->max_size = max_size;
	return (int)m->ngroups++;
}

u64 lp_partition_size(const struct lp_partition *p)
{
	u64 sectors = 0;
	for (u32 k = 0; k < p->nextents; k++)
		sectors += p->ext[k].num_sectors;
	return sectors * LP_SECTOR;
}

u64 lp_group_used(const struct lp_metadata *m, u32 group)
{
	u64 used = 0;
	for (u32 i = 0; i < m->nparts; i++)
		if (m->parts[i].group == group)
			used += lp_partition_size(&m->parts[i]);
	return used;
}

void lp_remove_partition(struct lp_metadata *m, int index)
{
	free(m->parts[index].ext);
	memmove(&m->parts[index], &m->parts[index + 1], (m->nparts - (u32)index - 1) * sizeof(*m->parts));
	m->nparts--;
}

struct range {
	u64 start, end; /* sectors, end exclusive */
};

static int cmp_range(const void *a, const void *b)
{
	const struct range *x = a, *y = b;
	return x->start < y->start ? -1 : x->start > y->start;
}

/* Sorted list of sectors in use on block device 0. */
static struct range *used_ranges(const struct lp_metadata *m, u32 *count)
{
	u32 n = 0;
	for (u32 i = 0; i < m->nparts; i++)
		n += m->parts[i].nextents;
	struct range *r = xcalloc(n, sizeof(*r));
	n = 0;
	for (u32 i = 0; i < m->nparts; i++)
		for (u32 k = 0; k < m->parts[i].nextents; k++) {
			const struct lp_extent *e = &m->parts[i].ext[k];
			if (e->target_type != LP_TARGET_LINEAR)
				continue;
			r[n].start = e->target_data;
			r[n].end = e->target_data + e->num_sectors;
			n++;
		}
	qsort(r, n, sizeof(*r), cmp_range);
	*count = n;
	return r;
}

/* Next aligned sector at or after s. */
static u64 align_sector(const struct lp_bdev *b, u64 s)
{
	u64 align = b->alignment / LP_SECTOR, offset = b->alignment_offset / LP_SECTOR;
	if (align <= 1)
		return s;
	if (s < offset)
		return offset;
	return ALIGN_UP(s - offset, align) + offset;
}

/* Calls fn for each free gap [start, end) in sectors, in order. */
static void for_each_gap(const struct lp_metadata *m, void (*fn)(u64, u64, void *), void *ctx)
{
	const struct lp_bdev *b = &m->bdevs[0];
	u32 n;
	struct range *used = used_ranges(m, &n);
	u64 pos = b->first_logical_sector, end = b->size / LP_SECTOR;
	for (u32 i = 0; i <= n; i++) {
		u64 gap_end = i < n ? MIN(used[i].start, end) : end;
		if (gap_end > pos)
			fn(pos, gap_end, ctx);
		if (i < n && used[i].end > pos)
			pos = used[i].end;
	}
	free(used);
}

static void sum_gap(u64 start, u64 end, void *ctx)
{
	*(u64 *)ctx += end - start;
}

u64 lp_free_bytes(const struct lp_metadata *m)
{
	u64 sectors = 0;
	for_each_gap(m, sum_gap, &sectors);
	return sectors * LP_SECTOR;
}

struct alloc {
	const struct lp_metadata *m;
	u64 need; /* sectors still to place */
	u64 block; /* sectors per logical block */
	struct lp_extent *ext;
	u32 n;
};

static void take_gap(u64 start, u64 end, void *ctx)
{
	struct alloc *a = ctx;
	if (!a->need)
		return;
	start = align_sector(&a->m->bdevs[0], start);
	if (start >= end)
		return;
	u64 len = (end - start) / a->block * a->block;
	len = MIN(len, a->need);
	if (!len)
		return;
	a->ext = xrealloc(a->ext, (a->n + 1) * sizeof(*a->ext));
	a->ext[a->n++] = (struct lp_extent){len, LP_TARGET_LINEAR, start, 0};
	a->need -= len;
}

int lp_add_partition(struct lp_metadata *m, const char *name, u32 group, u32 attrs, u64 size)
{
	if (strlen(name) > LP_NAME_LEN || lp_find_partition(m, name) >= 0 || group >= m->ngroups) {
		msg("cannot add partition %s", name);
		return EXIT_BAD_INPUT;
	}
	if (size % m->logical_block_size) {
		msg("%s: size %llu is not a multiple of %u", name, (unsigned long long)size, m->logical_block_size);
		return EXIT_BAD_INPUT;
	}
	const struct lp_group *g = &m->groups[group];
	if (g->max_size && lp_group_used(m, group) + size > g->max_size) {
		msg("%s does not fit in group %s", name, g->name);
		return EXIT_REFUSED;
	}

	struct alloc a = {m, size / LP_SECTOR, m->logical_block_size / LP_SECTOR, NULL, 0};
	if (a.need)
		for_each_gap(m, take_gap, &a);
	if (a.need) {
		free(a.ext);
		msg("not enough free space in super for %s (%llu MiB more needed)", name,
		    (unsigned long long)(a.need * LP_SECTOR >> 20));
		return EXIT_REFUSED;
	}

	m->parts = xrealloc(m->parts, (m->nparts + 1) * sizeof(*m->parts));
	struct lp_partition *p = &m->parts[m->nparts++];
	memset(p, 0, sizeof(*p));
	strcpy(p->name, name);
	p->attrs = attrs;
	p->group = group;
	p->nextents = a.n;
	p->ext = a.ext;
	return EXIT_OK;
}

bool lp_map(const struct lp_partition *p, u64 off, u64 *phys, u64 *run)
{
	u64 base = 0;
	for (u32 k = 0; k < p->nextents; k++) {
		u64 len = p->ext[k].num_sectors * LP_SECTOR;
		if (off < base + len) {
			if (p->ext[k].target_type != LP_TARGET_LINEAR)
				return false;
			*phys = p->ext[k].target_data * LP_SECTOR + (off - base);
			*run = base + len - off;
			return true;
		}
		base += len;
	}
	return false;
}

static bool has_suffix(const char *name, char slot)
{
	size_t n = strlen(name);
	return n > 2 && name[n - 2] == '_' && name[n - 1] == slot;
}

static bool extents_overlap(const struct lp_partition *a, const struct lp_partition *b)
{
	for (u32 i = 0; i < a->nextents; i++) {
		const struct lp_extent *x = &a->ext[i];
		if (x->target_type != LP_TARGET_LINEAR)
			continue;
		for (u32 k = 0; k < b->nextents; k++) {
			const struct lp_extent *y = &b->ext[k];
			if (y->target_type == LP_TARGET_LINEAR && x->target_source == y->target_source &&
			    x->target_data < y->target_data + y->num_sectors && y->target_data < x->target_data + x->num_sectors)
				return true;
		}
	}
	return false;
}

const char *lp_stale_overlap(const struct lp_metadata *m, char slot, const struct lp_metadata *live, char live_slot)
{
	for (u32 i = 0; i < m->nparts; i++) {
		if (!has_suffix(m->parts[i].name, slot))
			continue;
		for (u32 k = 0; k < live->nparts; k++)
			if (has_suffix(live->parts[k].name, live_slot) && extents_overlap(&m->parts[i], &live->parts[k]))
				return m->parts[i].name;
	}
	return NULL;
}
