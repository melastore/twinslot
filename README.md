<img src="docs/icon.svg" width="96" alt="" align="right">

# TwinSlot

Two Android ROMs on one A/B phone, like dual-booting Linux on a PC. Each ROM lives in one slot and has
**its own data**. You pick which one boots from an app, a Quick Settings tile, or recovery.

> **Status: alpha.** Works on my Tecno CK7n (VoltageOS 17 on slot b, Evolution X on slot a with its own
> data, switching both ways). Other A/B MediaTek phones: not tried yet. The code handles the layouts
> they use (see [Devices](#devices)), and the test suite runs on image files of fake phones, checked
> with the real Android tools. **Use it at your own risk:** it rewrites the partition table and the
> boot partitions. Keep a full backup and a way to flash from scratch (on MediaTek: a BROM/DA flasher
> such as mtkclient or SP Flash Tool) before you try it. The app shows this warning on first open and
> waits until you accept it.

## What it does

| | |
|---|---|
| **Switch** | Writes the A/B record in `misc` (priority, tries, CRC) and reads it back. It doesn't rely on `fastboot set_active`, which on some phones answers OKAY and changes nothing. |
| **Dual data** | Splits `userdata` once: the second ROM gets `twindata` + `twinmeta` partitions of its own, so its apps, accounts and encryption keys never touch the first ROM's. |
| **Install** | Flashes a full ROM zip (`payload.bin`: xz, bzip2, zstd, zero ops) into the slot that is *not* running, while you use the phone. Every chunk and every partition is checked against the ROM's sha256 hashes. |
| **Rebind** | Points the second ROM at its own data. It edits all three fstab copies: vendor's `/etc/fstab.*` inside EROFS or ext4, plus the first-stage and recovery fstabs in `vendor_boot` (or in `boot` on phones without `vendor_boot`; lz4, gzip or plain ramdisks). It swaps the partitions' SELinux labels in `vendor_file_contexts`, rebuilds vendor's dm-verity tree, and re-seals that slot's verified boot (see [below](#verified-boot-and-the-aosp-test-key)) so the edits still verify. AVB footers are kept and brought up to date. |
| **App + tile** | Shows both ROMs (name, Android version, build date, which data each one mounts). From it you can reboot into the other ROM, install a ROM (progress in plain words, in the app and a notification), and make the setup, resize and remove zips with step-by-step flashing help. Settings: theme and color, keep the screen on, install defaults, an app lock (PIN or pattern, fingerprint unlock) and a diagnostic log to send with bug reports; a built-in guide. Without root it shows which root manager to allow it in, and a device check that says whether the phone can dual boot at all. |
| **Recovery zips** | `switch`, `rebind`, `setup` and `remove` zips that work in any recovery (OrangeFox, TWRP, AOSP). |
| **Table copies** | Before `setup` or `remove` writes the partition table, a copy of it is saved (the app puts one next to the zip for you to keep on a computer). A write that fails while the phone is on is undone at once; `table-restore` puts a saved copy back after a power cut. |

The second ROM keeps working even when the bootloader falls back to it on its own. Each ROM's own fstab
names its own data partitions, so a ROM can never boot on the other ROM's data.

## How the pieces fit

```
             super (dynamic partitions)              userdata (split once by setup)
  slot a:  system_a vendor_a product_a ...   ──▶   userdata + metadata    (ROM 1's data)
  slot b:  system_b vendor_b product_b ...   ──▶   twindata + twinmeta    (ROM 2's data)
                  ▲
                  └── vendor_b's fstab and vendor_boot_b's (or boot_b's) fstabs are rebound to twindata/twinmeta
```

See [docs/DESIGN.md](docs/DESIGN.md) for the details: on-disk formats, the order of writes, and why each
safety check exists.

## Getting started

1. **Back up everything.** Setup erases the data of both ROMs.
2. Install the TwinSlot app on your current ROM (root needed: Magisk, KernelSU or APatch).
3. **Dual data:** in the app, choose how much space the second ROM gets and tap *Make the setup zip*.
   Boot to recovery, unmount Data, and flash `/sdcard/TwinSlot/twinslot-setup-*.zip` by **ADB sideload**
   (or from an SD card or USB stick). It can't run from internal storage, because Data must be unmounted.
4. Boot your ROM again (it starts fresh) and set it up.
5. **Install the second ROM:** in the app, *Install a ROM* → choose the ROM zip. The app checks it,
   flashes it into the other slot and points it at its own data. You can keep using the phone meanwhile:
   the job runs as root outside the app, so closing the app (or Android closing it) does not stop it.
6. Reboot. Switch back and forth with the app, the tile, or `twinslot-switch-other.zip` in recovery.

From a shell (root), the same as the app does:

```sh
twinslot status
twinslot verify /sdcard/Download/rom.zip
twinslot install /sdcard/Download/rom.zip --wipe     # into the slot that is not running
twinslot switch other && reboot
```

Run `twinslot` with no arguments for all commands and options.

## Rules that keep you safe

- **Turn OTA updates off on both ROMs.** An OTA installs into the other slot, which is the other ROM.
  Update a ROM by installing its new full zip with TwinSlot (leave *empty data* unticked for the same ROM).
- **Don't let a recovery reflash `vendor_boot`** of the second slot after installing. That undoes the
  rebind. TwinSlot then refuses to switch to that slot, and the app offers *Fix*.
- **Keep the same bootloader on both slots.** TwinSlot never writes a ROM zip's `lk`/`preloader`
  unless you pass `--bootloader` (in the app: *Write the zip's bootloader*, off by default).
- Both ROMs should carry a similar security patch level. Each ROM's keys live in its own data, so a gap
  is mostly harmless, but the TEE is closed code and may refuse keys after an apparent downgrade.

## Verified boot and the AOSP test key

Rebinding changes `vendor_boot` (or `boot`) and `vendor`, so their digests in vbmeta change too. TwinSlot
computes the new digests and dm-verity tree, then re-signs every vbmeta it changed with the **AOSP test key**
(`external/avb/test/data/testkey_rsa2048.pem`). That key is public; it is embedded in `src/testkey.h`
on purpose, and it is not a secret anyone could leak.

- On an **unlocked** bootloader (orange state) this boots like any custom ROM: libavb checks the digests,
  which are correct, and accepts the unknown key because the device is unlocked.
- Many custom ROMs already sign with the test key, so the key does not even change. When a ROM used its
  own key, TwinSlot says so (`note: ... was signed with another key`).
- A truly **locked** bootloader only accepts its maker's key, and no custom ROM boots there anyway.
- TwinSlot clears the "verification disabled" flags rather than setting them. On the CK7n a vbmeta
  with flags 3 does not boot: lk then leaves out the `androidboot.vbmeta.*` arguments and first-stage init
  refuses to mount. With correct digests, verification can stay on everywhere. See
  [docs/DESIGN.md §3](docs/DESIGN.md).

TwinSlot does **not** check who signed a ROM zip. It checks every operation's sha256 against the zip's own
manifest, which catches a damaged download but not a forged ROM, the same as flashing a zip in a custom
recovery. Get ROMs from people you trust.

## If a flash is cut off

`setup` and `remove` rewrite the partition table. Many MediaTek disks keep no backup copy of it, and lk
reads only the main one, so a power cut in the moment of that write (a few milliseconds) would leave the
phone unable to find its partitions. TwinSlot plans for this:

1. Before writing, it saves the table (LBA 0 up to the first partition) as `twinslot-gpt-<disk>-<time>.bin`:
   to `--table-dir`, to `/tmp` in recovery, and, when the zip runs from an SD card or USB stick, next to the
   zip. The app saves one into `/sdcard/TwinSlot` before it makes the zip, puts the same copy inside the
   zip as `table.bin` (so a sideloaded zip leaves one on your computer), and asks you to copy that folder
   off the phone. On a disk without a sound backup table, setup and remove refuse to run unless a copy
   would outlive a power cut: `/tmp` is RAM and `/sdcard` is on the very disk being changed, so it takes a
   copy on an SD card or USB stick, or the zip's own `table.bin` while it still matches the table.
   The generic `twinslot-setup.zip` from `make zips` has no `table.bin`: flash it from a card or stick.
2. If the write itself fails while the phone is on, TwinSlot writes the old table back before it exits.
3. If the phone still reaches a recovery with a shell (TWRP, OrangeFox):
   `twinslot table-restore /path/twinslot-gpt-....bin` (a `table.bin` taken out of the zip works too). It
   finds the disk from the copy's name, or as the only disk of the copy's size; when it cannot tell, name
   it: `twinslot --disk /dev/block/sdc table-restore ...`. It refuses a copy from another disk, and refuses
   to overwrite an intact table without `--force` (use `remove` for that).
4. If it reaches nothing: write the copy back from a computer at offset 0 of the storage area that holds
   userdata. `twinslot status` shows that disk (`disk=`): on UFS phones `/dev/block/sda`, `sdb`, `sdc` are
   LU0, LU1, LU2; on eMMC `mmcblk0` is the user area. With mtkclient, for example:
   `python mtk.py wo 0 <size of the .bin in bytes> twinslot-gpt-....bin --parttype lu2` (the LU of that disk,
   `sdc` is `lu2`; eMMC: `user`). Check the exact options against your mtkclient version first.

You can save a copy yourself at any time: `twinslot table-save /path/to/dir`.

## What it refuses (and why)

| Refusal | Why |
|---|---|
| Switch to a slot with nothing in `super` | the phone would hang at the logo with no adb and no fastboot |
| Switch to a ROM bound to the wrong data | it would boot on the other ROM's data and wreck both |
| Switch, install, setup or remove while a Virtual A/B update is merging; rebind or wipe of a slot that is part of it (the running one, or the one the update boots next) | the update would be lost half-applied (its COW lives in the running slot's data) |
| Install over the running ROM from Android | do that from recovery |
| Setup/remove/wipe while Data is mounted | nothing writes under a mounted filesystem |
| Incremental OTA zips | they patch the old ROM's blocks; only full ROM zips are supported |
| EROFS layouts it can't patch safely (ztailpacking, fragments) | it leaves the image untouched rather than guess |
| setup/remove on a disk without a sound backup table when no copy would outlive a power cut | nothing could undo a write cut short |
| Switch to a slot an install or rebind left unfinished (priority 0, no tries left) | it would boot half a ROM; install it again, or after a rebind cut short `twinslot --force rebind <slot>` (`switch --force` overrides) |
| A boot record that is not AOSP's in `misc` (older MediaTek keep their own in `para`) | it would switch nothing, or the wrong thing |
| Ramdisks wrapped in MediaTek's old 512-byte header | the rebuilt image would not be what the bootloader expects |

A failed or interrupted install leaves the target slot marked unbootable, and `switch` will not bring it
back until an install finishes. A rebind cannot tell an unfinished install from its own interrupted run, so
it brings a slot back only with `--force`. It is never half-booted.

## Build

Needs a C compiler with zlib, the Android NDK (for the phone binary; its static `libz.a` is linked in),
Python 3 for tests and zips, and JDK 17+ for the app.

```sh
make            # out/twinslot (phone, static arm64) and out/twinslot-host (for tests)
make test       # 109 tests on fake phones; needs lpmake, mkfs.erofs, mkbootimg, avbtool, sfdisk, lz4, openssl
make zips       # recovery zips in out/
make app        # app/app/build/outputs/apk/release/app-release.apk
```

Tests on real ROM files (optional, slow):

```sh
TWINSLOT_REAL_ZIP=/path/rom.zip TWINSLOT_REAL_IMAGES=/dir/with/vendor.img+vendor_boot.img make test
```

## Devices

Written on a Tecno CK7n (MT6789): see [docs/devices/CK7n.md](docs/devices/CK7n.md). The code itself
targets **A/B MediaTek phones** in general:

| Needs | Handled |
|---|---|
| A/B slots with dynamic partitions (`super`), Android 10 or later | Virtual A/B too (merges must be finished) |
| AOSP boot record (`bootloader_control`) in `misc`, which MediaTek's lk reads | an older MediaTek record in `para` is detected and refused |
| GPT on UFS (`sda`-`sdz`, 4096-byte sectors) or eMMC (`mmcblk0`, 512) | with or without a backup table; MediaTek's own records kept byte for byte |
| `/dev/block/by-name` | or the platform `.../by-name` links some recoveries make |
| first-stage and recovery fstabs in `vendor_boot` v3/v4 | or in `boot` v0-v2 on phones without `vendor_boot` (Android 10/11 launch) |
| ramdisks in lz4 (legacy frames), gzip or plain cpio | MediaTek-wrapped ramdisks are refused |
| vendor in EROFS (LZ4) or ext4 | |
| AVB 2 (vbmeta, chains, footers) | footers signed or unsigned, chained or not |

Not supported: Qualcomm (slots live in GPT attributes), Exynos and Tensor (their own boot control),
phones without A/B. The app's device check says this before it asks for root. `tools/survey.sh` collects a
new phone's facts read-only, and *Settings › Diagnostic log* gives the basics (slots, super, Virtual A/B,
verified boot, root manager, `twinslot status`) without a computer. Reports from other phones are welcome.

## License

MIT, provided as is, without warranty: see [LICENSE](LICENSE). The bundled decompressors in `third_party/`
keep their own licenses: LZ4 (BSD-2), zstd (BSD-3), bzip2 (bzip2 license), XZ Embedded (0BSD), and AOSP's
public AVB test key in `third_party/avb/` (Apache-2.0). zlib (zlib license) is linked from the system or the
NDK.
