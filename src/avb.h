/*
 * Android Verified Boot: keeping a slot's vbmeta true to images twinslot
 * edited (the rebound vendor_boot or boot, and vendor).
 */
#ifndef TWINSLOT_AVB_H
#define TWINSLOT_AVB_H

#include "common.h"
#include "part.h"

/* The 64 bytes at the end of a partition that point at the vbmeta of the image before them. */
#define AVB_FOOTER_SIZE 64
struct avb_footer {
	u64 original_size; /* the image, as hashed */
	u64 vbmeta_offset, vbmeta_size;
};
/* EXIT_OK and *f if p ends in a valid footer, else EXIT_BAD_INPUT. */
int avb_footer_read(const struct part *p, struct avb_footer *f);
void avb_footer_encode(const struct avb_footer *f, u8 out[AVB_FOOTER_SIZE]);

/*
 * Brings vbmeta_<slot>, the vbmeta partitions it chains to, and the edited
 * image's own footer in line with the images as they are now: the hash
 * descriptor of ramdisk (named ramdisk_name: "vendor_boot", or "boot" on a
 * phone without vendor_boot) gets the image's size and digest, vendor's
 * dm-verity tree is rebuilt in place (without FEC), the top vbmeta's
 * verification-disabled flags are cleared, and every signed vbmeta that
 * changed is re-signed with the AOSP test key (an unsigned footer stays
 * unsigned). Relaxing the flags instead does not work on every bootloader:
 * the CK7n's reports a locked device and drops the androidboot.vbmeta
 * arguments, and first-stage init refuses to mount. With correct digests an
 * unlocked bootloader boots whatever the key, as it does any custom ROM.
 * *changed gets the number of descriptors and flags rewritten (0: in line).
 */
int avb_reseal(int slot, const struct part *ramdisk, const char *ramdisk_name, const struct part *vendor, int *changed);

#endif
