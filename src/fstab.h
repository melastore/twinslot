/*
 * Rebinding a ROM to the twin data partitions.
 *
 * On lines that mount /data or /metadata, the device's last path component
 * changes userdata <-> twindata and metadata <-> twinmeta. The names have the
 * same length, so a file keeps its size and can be patched in place inside
 * any image. Works for fstab, recovery.fstab and TWRP/OrangeFox twrp.flags.
 */
#ifndef TWINSLOT_FSTAB_H
#define TWINSLOT_FSTAB_H

#include "common.h"

#define TWIN_DATA "twindata"
#define TWIN_META "twinmeta"

enum binding {
	BIND_NONE = 0,    /* no /data or /metadata line */
	BIND_PRIMARY = 1, /* userdata, metadata */
	BIND_TWIN = 2,    /* twindata, twinmeta */
	BIND_MIXED = 3,   /* some of each: a half-patched file */
};

/* Rewrites buf to `to` (BIND_PRIMARY or BIND_TWIN); returns the number of names changed. */
int fstab_rebind(u8 *buf, size_t len, enum binding to);
/* Which partitions buf binds /data and /metadata to. */
enum binding fstab_binding(const u8 *buf, size_t len);
/* Combines two bindings (NONE is neutral, disagreement is MIXED). */
enum binding binding_merge(enum binding a, enum binding b);
const char *binding_name(enum binding b);

#endif
