# Tecno CK7n (MT6789 / Helio G99)

Known from earlier work on this phone; the survey (`tools/survey.sh`) fills in the rest.

| Fact | Value |
|---|---|
| A/B, Virtual A/B | yes; the inactive slot normally holds no logical partitions |
| `misc` | `/dev/block/sdc3`; `bootloader_control` at offset 2048 |
| `fastboot set_active` | answers OKAY, changes nothing |
| recovery `bootctl set-active-boot-slot` | switches but leaves 1 try |
| Exhausted slot | marked unbootable, bootloader falls back; with an empty fallback the phone hangs at the logo |
| `super` | 9,126,805,504 bytes; ~4.3 GB free with one slim ROM |
| `/data` | f2fs, FBE `aes-256-xts:aes-256-cts:v2+inlinecrypt_optimized`, `checkpoint=fs`; `keydirectory=/metadata/vold/metadata_encryption` turns on metadata encryption, so each ROM needs its own metadata partition (`twinmeta`) |
| Bootloader | fenrir-patched lk, the same image on `lk_a` and `lk_b` |
| Recovery | OrangeFox, merged into `vendor_boot` per slot. Reflashing it into the second slot's `vendor_boot` undoes the rebind: run `twinslot rebind b` (or the app's *Fix*) afterwards |
| BROM recovery | antumbra DA flasher |
| boot / vendor_boot / dtbo | must come from the same build, per slot, or the slot hangs at the logo |
| Writing partitions from recovery | `adb push` to `/dev/block/...` lands in recovery's tmpfs, not the partition: push to `/tmp`, then `dd`, then hash the block device |

## Checked on real CK7n ROM files

- VoltageOS 6.0 (Android 17) zip: `payload.bin` STORED, full payload (987 XZ, 57 bzip2, 43 plain ops), group
  `mtk_dynamic_partitions`; 24 partitions, 2.1 GB, including `lk` (kept) and `tkv`. It installs cleanly
  into image files with every hash matching.
- Its `vendor.img` is LZ4 EROFS with `sb_csum`, `0padding`, big pclusters and metadata at block 0. The
  rebind changes only `etc/fstab.*` and passes `fsck.erofs`.
- Its `vendor_boot.img` is v4 with two LZ4-legacy ramdisks. The rebind changes 4 lines: 2 in the
  first-stage fstab, 2 in `recovery.fstab`.

## Learned on the phone (2026-10)

- userdata is on `/dev/block/sdc` (UFS LU2), not the last partition there (`flashinfo` follows). The GPT
  has no backup copy, and a zero-type MediaTek record follows the counted entries; growing the table by
  two entries moved past it. `tranfs` (an empty Transsion ext4) is adopted as `twinmeta`.
- A ROM sepolicy line labelled `/dev/block/sdc62` as `tranfs`; with the split in place that node is
  `tee_b`, and update_engine could not open it. ROM trees should label partitions by name, never by `sdcNN`.
- `vendor_boot` ends in an unsigned AVB footer (original size 31,379,456 bytes, padded past the header's
  length); `boot` has a footer signed with the AOSP test key and is chained from `vbmeta`.
- vbmeta with the verification-disabled flags set does not boot: lk reports `device_state=locked` and
  drops the `androidboot.vbmeta.*` arguments (see DESIGN §3).
- The bootloader keeps `slot_suffix` as `_a`/`_b`.
- pstore/`SYSTEM_LAST_KMSG` can hold a stale record from an older boot; the lk log per boot is in `expdb`.
