/*
 * Installing a full ROM (an OTA zip with payload.bin) into one slot, while the
 * other slot's ROM keeps running.
 */
#ifndef TWINSLOT_INSTALL_H
#define TWINSLOT_INSTALL_H

#include "common.h"

struct install_opts {
	const char *zip;
	int target;          /* 0/1, or -1 for the slot that is not running */
	bool switch_after;   /* make the target the next boot (else: a fallback) */
	bool wipe;           /* blank the target ROM's data: needed when the ROM changes */
	bool bootloader;     /* also write the ROM's lk/preloader (normally the device's are kept) */
	bool dry_run;        /* plan and check, write nothing */
	bool force;          /* allow installing without dual data */
};

int install_rom(const struct install_opts *o);

#endif
