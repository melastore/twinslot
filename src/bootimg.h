/*
 * The image whose ramdisks hold a slot's first-stage and recovery fstabs:
 * vendor_boot (header v3, v4), or on phones without vendor_boot, boot
 * (header v0-v2, where recovery lives in boot's ramdisk too). Ramdisks may be
 * lz4 (legacy frames), gzip or plain cpio.
 */
#ifndef TWINSLOT_BOOTIMG_H
#define TWINSLOT_BOOTIMG_H

#include "common.h"
#include "fstab.h"
#include "part.h"

/*
 * Reads the image from p, rebinds every fstab-like file in its ramdisks to
 * `to`, and, when anything changed and `write` is set, writes the rebuilt
 * image back (keeping an AVB footer) and verifies it. *binding gets what the
 * image binds to before the change (BIND_NONE when no file mentions /data or
 * /metadata).
 */
int bootimg_rebind(const struct part *p, enum binding to, bool write, enum binding *binding, int *changes);

/* The image's length from its header: every section, page-aligned. */
int bootimg_size(const struct part *p, u64 *size);
/* What vbmeta hashes: the AVB footer's original size, else the header's length. */
int bootimg_hashed_size(const struct part *p, u64 *size);

#endif
