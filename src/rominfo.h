/*
 * What is installed in a slot: read from the build.prop files of its system, system_ext and product images, so
 * both ROMs see the same names without storing anything.
 */
#ifndef TWINSLOT_ROMINFO_H
#define TWINSLOT_ROMINFO_H

#include "common.h"

struct rom_info {
	bool installed;   /* system_<slot> exists in super with extents */
	bool readable;    /* its build.prop could be read */
	char name[96];    /* the ROM's own version string, else the build id */
	char android[16]; /* ro.build.version.release */
	char build[24];   /* ro.build.date.utc */
	u64 system_bytes; /* size of system_<slot>: Settings adds it to /data for the total */
};

int rom_info(int slot, struct rom_info *ri);

#endif
