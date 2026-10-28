# TwinSlot design

TwinSlot reuses what A/B phones already have: two copies of every boot partition, and `super` holding
the dynamic partitions of both slots. It adds one thing they lack: a second `/data`. Everything lives in
one static C binary (`src/`, no dependencies outside `third_party/`). The app, the tile and the recovery
zips all run that binary.

Principles:

- **Check before writing, read back after.** Every write is followed by a read and a compare: the A/B
  record, both GPT copies, super metadata, every payload partition (sha256), the patched fstab files, the
  re-signed vbmeta.
- **A slot is unbootable while it is inconsistent.** Before an install or rebind changes a slot's
  images, that slot is marked unbootable (priority 0, no tries left, as AOSP's boot control and
  update_engine mark it). It becomes bootable only when everything has verified, and `switch` refuses
  a slot in that state without `--force`. A rebind restores the record it found, so it never revives a
  slot an install left half written; `--force rebind` parks one whose own rebind was cut short. Setup and remove only *park* the owner slot (priority 0 with
  tries left): its images are whole, only the data under it changed, so `switch` may bring it back once
  its fstabs name the right data.
- **Refuse rather than guess.** Unknown layouts, a damaged record, merges in progress and mounted data
  all stop the command with a message saying why.

## 1. The A/B record (`bctl.c`)

`bootloader_control` sits at offset 2048 of `misc`:

| bytes | field |
|---|---|
| 0–3 | slot suffix (`_a`, or `a` on some bootloaders; the spelling found is kept) |
| 4–7 | magic `BCAB` (0x42414342) |
| 8 | version |
| 9 | nb_slot:3, recovery_tries:3 |
| 10 | merge_status:3 (0 none, 1 unknown, 2 snapshotted, 3 merging, 4 cancelled) |
| 12–19 | slot_info × 4: priority:4, tries:3, successful:1; byte 1 bit 0 verity_corrupted |
| 28–31 | CRC32 of bytes 0–27 |

`switch X` sets slot X to priority 15 with 7 tries, and lowers the other slot to 14 if needed, so the
bootloader falls back to it. It refuses when:

- the magic is bad (always);
- the CRC is bad (unless `--force`);
- a merge is pending (rebind and wipe refuse only for the slots that are part of it: the running one,
  whose data holds the COW and whose vendor is the merge's base, and the one that boots next, which
  holds an update written but not booted yet; any other slot is left out of it);
- slot X has no `system_X` in super;
- slot X's ROM is not bound to the data it must use (§3).

Only this record is written. Older MediaTek boot control (Android 9/10 era) kept a record of its own in
`para`; when `misc` has none, TwinSlot looks there and names what it found, but writes nothing, since it
cannot know that lk reads it.

## 2. Dual data (`gpt.c`, `twin.c`)

`setup` carves two partitions off the end of `userdata`. It doesn't move or shrink any other partition:

```
before:  ... | metadata | ... | userdata ·································· |
after:   ... | metadata | ... | userdata ············ | twinmeta | twindata |
```

- `twinmeta` is as large as `metadata`; `twindata` is the requested size (default: half). Both are
  aligned to 1 MiB. The type GUIDs are copied from `metadata`/`userdata`, the unique GUIDs are new, and
  attribute bit 48 records the owner slot (set = slot b).
- At least 8 GiB must stay in `userdata`, and the GPT must have two free entries.
- The GPT is written backup first, then primary, each as entries, fsync, header, fsync (so a header
  never reaches the disk before the entries it describes), then both copies are read back. Partition
  names are kept byte for byte, as are the attribute and reserved bytes the code doesn't interpret. Both
  512- and 4096-byte sectors are supported. Bit 48 is in the type-specific attribute range; Qualcomm uses
  48-55 for its slot flags on boot partitions, which is one more reason Qualcomm is not supported.
- The kernel learns the new table without a reboot: `BLKRRPART`, else `BLKPG` resize and add one
  partition at a time, which works while other partitions on the same disk are in use.
- The first 16 MiB of both data partitions and all of both metadata partitions are zeroed. Each ROM
  formats its data on next boot, exactly as after `fastboot -w`. Shrinking f2fs in place isn't
  possible, and a ROM must never mount another ROM's data.
- The owner slot is parked (priority 0, tries left): its ROM (if any) still names `userdata` until
  rebound, which `switch` refuses by its binding check.

Why a separate `twinmeta`: with metadata encryption (on CK7n the fstab's `keydirectory=` alone turns it
on), `/data`'s key lives in `/metadata/vold/metadata_encryption`. Two ROMs sharing `/metadata` would
overwrite each other's keys. AOSP recovery's wipe also erases `/metadata`.

"Mounted" means: the partition's device, or anything stacked on it (dm-default-key, dm-crypt, …), is in
`/proc/self/mountinfo` (by device number and by mount source) or `/proc/swaps`. A device-mapper stack
that is set up but not mounted (recovery after decrypting, then unmounting Data) does not count.

`remove` is the inverse: it needs `twinmeta`+`twindata` to directly follow `userdata`. It deletes them,
grows `userdata` back, wipes both data partitions, and parks the owner slot. If pointing the owner's ROM
back at `userdata` fails, its images may be half edited, so the slot is marked unbootable instead.

**Table copies.** Many MediaTek disks keep no backup GPT, and where one exists lk is not known to fall
back to it (the preloader and lk read the primary at LBA 1). So between the primary's entries and its
header there is a moment (milliseconds) in which a power cut leaves lk with no valid table and no
partitions. That cannot be made atomic, so it is made recoverable:

- Before `setup` or `remove` writes, the primary area (LBA 0 up to the first usable LBA: protective MBR,
  header, every entry block, MediaTek's own records included) is saved to `--table-dir` and to a default
  directory (`/tmp` on a phone, the image directory on a PC), each copy written with `O_EXCL`, fsync'd,
  dropped from the page cache and read back from the medium.
- On a disk without a sound backup table (none, or one that does not check out), a copy must survive the
  power cut it is for: not on tmpfs/ramfs (recovery's `/tmp`) and not on the disk being changed
  (`/sdcard`, `/data`; dm devices are followed down to their disk via `/sys/dev/block/*/slaves`). A
  `--kept-copy FILE` counts too when it matches the table byte for byte. Without one of these, setup
  and remove refuse (`--force` overrides).
- The app saves a copy into `/sdcard/TwinSlot` before it makes a setup/remove zip, and puts the same
  bytes into the zip as `table.bin`. Sideloaded, the zip stays on the computer, so the recovery script
  passes it as `--kept-copy`; the engine counts it only while it is still the table as it is (a zip
  made before another change does not count). From an SD card or USB stick the script also passes
  `--table-dir` with the zip's own folder (quoted: any name works).
- If `gpt_write` fails while the phone is on, the old area (read before the write) is written back at
  once: entries and MBR, fsync, header, fsync, read back.
- `table-restore FILE` writes a copy back. The disk cannot be found by its `userdata` partition, since
  its table may be gone: without `--disk` it is the one the copy's name says (`twinslot-gpt-<disk>-…`)
  if that disk has the copy's size, else the only `sd*`/`mmcblk0` that has it (two or none: `--disk`).
  The copy must be a whole, checksummed primary area for a disk of this size, from this disk (same disk
  GUID) when the current table is readable, and it refuses to replace an intact table without `--force`. A disk that keeps a backup table gets it rewritten from the restored
  primary. If the phone reaches nothing, the same file goes back at offset 0 of that disk with a BROM
  tool (README, "If a flash is cut off").

## 3. Binding a ROM to its data (`fstab.c`, `erofs.c`, `bootimg.c`)

A ROM's data partitions are named in three fstab copies, and all three must agree:

| copy | where | used by |
|---|---|---|
| `/vendor/etc/fstab.<hw>` | EROFS (or ext4) `vendor_X` in super | second-stage init: mounts `/data` |
| `first_stage_ramdisk/.../fstab.<hw>` | ramdisk 0 of `vendor_boot_X` (or `boot_X`'s ramdisk) | first-stage init: mounts `/metadata` |
| `system/etc/recovery.fstab`, `twrp.flags` | recovery ramdisk of `vendor_boot_X` (or `boot_X`'s ramdisk) | recovery |

Phones that launched with Android 10 or 11 often have no `vendor_boot`: their boot image (header v0-v2)
carries one ramdisk with first-stage init, the fstab and recovery. TwinSlot uses `boot_X` whenever
`vendor_boot_X` does not exist.

**fstab edit.** On any line whose mount point is exactly `/data` or `/metadata`, the block-device path
(`/dev/block/...`) ending in `userdata`/`metadata` is renamed to `twindata`/`twinmeta`, and back. The
names have equal length, so the file's size never changes. Mount points and options such as
`keydirectory=/metadata/...` are never touched. Both AOSP order (device first) and `twrp.flags` order
(mount point first) work.

**EROFS.** Vendor images are usually LZ4-compressed EROFS, so a byte patch is impossible. TwinSlot:

1. Reads the inode (compact or extended) and its compression map. It supports compact 2B/4B and full
   indexes, big pclusters, `0padding`, and plain extents with shifted or interlaced placement.
2. Decompresses, edits, and recompresses each changed pcluster into exactly its old physical size.
   LZ4HC is tried first. If the output is too short, a literal tail is grown until the compressed size
   lands in `(plen − blksz, plen]`, because the kernel only looks for the stream's start in the first
   block of a `0padding` pcluster. Plain (incompressible) extents are written as-is.
3. Refuses partially referenced (deduplicated) pclusters, ztailpacking and fragments.
4. Re-reads the file through the same decoder and compares.
5. Recomputes the **superblock checksum** (CRC32C over the rest of block 0) when `sb_csum` is set.
   Images whose metadata starts at block 0 keep inodes in that block, so a patch changes it, and the
   kernel refuses to mount an image with a stale checksum. It also checks the checksum when opening an
   image and refuses images that fail.

The format code follows erofs-utils 1.8.5 (`lib/zmap.c`). The tests run every case through
`fsck.erofs --extract`, which validates the whole image.

**Boot images.** `vendor_boot` header v3/v4: the ramdisks are found through the v4 ramdisk table (or as
the single v3 ramdisk). `boot` header v0-v2: one ramdisk between the kernel and the second stage. Each
ramdisk is decoded (LZ4 legacy frames, gzip through zlib, or plain cpio; a MediaTek 512-byte wrapper is
refused), the cpio (`newc`) is edited in place, and the ramdisk is re-encoded the way it came (LZ4HC in
8 MiB legacy blocks, gzip level 9). The image is rebuilt with corrected sizes and offsets: the v4 ramdisk
table; for `boot`, the absolute `recovery_dtbo_offset` (v1/v2) and mkbootimg's SHA-1 `id` (rewritten only
if the old one was mkbootimg's). It must still fit the partition, and is written and read back.

**AVB footers.** Images built with AVB end in a 64-byte footer pointing at a vbmeta blob just past the image
(on the CK7n, `vendor_boot` has an unsigned one whose original size is padded past the image, and `boot`
a signed, chained one). A rebuild keeps the footer: the blob moves past the new image if it grew, the
original size stays if it did not (the padding up to it remains part of what is hashed), and the blob's
hash descriptor is then updated like vbmeta's (below).

**SELinux labels.** vendor's `/etc/selinux/vendor_file_contexts` labels `by-name/userdata` and
`by-name/metadata` as `userdata_block_device` and `metadata_block_device`. A twin partition without such
a line keeps the generic `block_device` label, vold cannot open it, `/data` never formats and recovery
loops on "Erasing". Rebind swaps the names on those lines exactly as in the fstabs.

**vbmeta (`avb.c`).** The edited images no longer match vbmeta, so rebind re-seals the slot: the
boot image's hash descriptor (`vendor_boot`, or `boot`) gets the image's hashed length (the footer's
original size, else the header's) and salted digest, vendor's dm-verity tree is rebuilt in place (same salt
and block size; FEC is dropped and its descriptor fields zeroed), the vbmeta blobs that changed are
re-signed with the AOSP test key (`third_party/avb`, embedded by `tools/testkey.py`), and the top vbmeta's
verification-disabled flags are cleared. The tree is SHA-256 or SHA-1 (avbtool's default; each digest
padded to 32 bytes as avbtool does). That covers `vbmeta_X`, the `vbmeta_*` partitions it chains to,
vendor's own footer when vbmeta chains to `vendor` (or nothing else describes vendor), and the edited
image's own footer: through a chain descriptor (its key is then swapped for the test key's
if it was another), or on its own when nothing chains to it. An unsigned footer blob stays unsigned. A
blob that is already in line is left untouched.

Why the test key and cleared flags: relaxing the flags instead does not boot on the CK7n. Its lk reports a
locked device and, with verification off, leaves out every `androidboot.vbmeta.*` argument, so first-stage
init refuses to mount and the kernel panics. With correct digests, an unlocked bootloader (orange state)
accepts any key, and the CK7n's lk accepts the test key its ROMs already use. When a blob was signed with
another key, rebind prints a note saying so. A truly locked bootloader accepts neither, nor any custom ROM.

## 4. Installing a ROM (`payload.c`, `lp.c`, `install.c`)

`install ZIP` targets the slot that isn't running. It may target the running slot only from recovery.

1. **Read the zip:** the end-of-central-directory (zip64 too) points to `payload.bin`, which must be
   STORED. Parse the `CrAU` v2 header and the protobuf manifest. Refuse incremental payloads (minor
   version ≠ 0, `src_extents`, a non-empty `old_partition_info`) and unknown operation types.
2. **Plan** (`--dry-run` stops here):
   - Read super's metadata (liblp: geometry ×2, header and tables checksummed with sha256, primary then
     backup).
   - The target group is the running ROM's system group with the slot letter swapped (`main_a` →
     `main_b`), else the payload's group name plus the suffix.
   - Remove the target slot's old partitions and their Virtual A/B `-cow` partitions.
   - Allocate each dynamic partition first-fit, aligned to super's alignment, within the group's
     maximum.
   - Physical partitions map to `<name>_<slot>`. A partition the device doesn't have is skipped, and a
     non-A/B one is refused (writing it would change the other ROM). The ROM's `lk`/`preloader` are
     skipped unless `--bootloader`.
3. **Write:**
   - Mark the target slot unbootable.
   - Write the new super metadata to every metadata slot (backups first) and read it back.
   - For each partition and operation, check the data's sha256, decompress (REPLACE, BZ, XZ, ZSTD;
     ZERO/DISCARD stream zeros of any size), write, then fsync.
   - Compare each written partition's sha256 to the manifest.
4. **Bind:** if the target slot owns `twindata`, rebind (§3). Then read the binding back from the
   images, and stop if it isn't what the slot must mount.
5. Optionally wipe the target's data (`--wipe`), through the GPT, so it also works right after setup
   before the kernel has a device node for the new partitions.
6. Make the slot bootable: switch to it, or with `--no-switch` set priority one below the running slot.

Logical partitions are written straight to their extents in super. Device-mapper isn't needed, so the
same code runs in Android, any recovery, or on a PC against image files (`--dev-dir`).

`verify ZIP` performs step 1 and the per-operation checks of step 3 without writing. On a real VoltageOS
17 zip (2.1 GB), verify takes ~2.5 minutes and a full install into image files ~3.5 minutes on a desktop
CPU, mostly XZ decompression.

## 5. What both ROMs share

- **Bootloader and firmware.** `lk`, `tee`, modem and the rest have `_a`/`_b` copies, and each ROM's zip
  writes its own firmware into its slot. The bootloader (`lk`, `preloader`) is the exception: TwinSlot
  keeps the device's own unless `--bootloader` is given. A separate `lk` per slot is possible (the
  preloader starts the active slot's copy), but `lk` is also what counts boot tries and falls back to the
  other slot. An `lk` that does not start on this phone, such as one built for a sibling model that a
  ROM zip carries, never gets that far: the phone stays at the logo until it is flashed over USB.
- **Security patch level.** Keymint rejects a key blob made under a newer OS version or patch level
  than the running boot's. With the split, each ROM's blobs (in its own `/data` and `/metadata`) are
  made and used under its own levels, so they never look like they are from the future. Two ROMs on the
  same `userdata` (no split) are different: the older one cannot use keys the newer one made. AOSP
  keymint keeps no device-wide patch-level counter that would lock the older ROM out of its own keys,
  and CK7n has run Android 16 and 17 ROMs side by side. But the MediaTek TEEs are closed, so keep the
  levels close anyway. The other shared state in RPMB is AVB's rollback indexes. lk only enforces and
  raises them on a locked bootloader, and TwinSlot needs an unlocked one; reseal keeps each image's
  index as it was.
- **OTA.** An in-ROM OTA writes the other slot, which destroys the other ROM. Keep OTA off. Update a ROM
  by installing its new full zip into its slot.
- **Recovery** lives in each slot's `vendor_boot` (or `boot`). A recovery flashed into the owner slot's
  `vendor_boot` loses the rebind. `status` shows that (`data:primary expected:twin`), `switch` refuses,
  and `rebind` fixes it.

## 6. Status output (for the app and scripts)

`twinslot status` prints `key=value` lines:

```
version=1.0.0
current=a            next=a          merge_status=none     recovery=no
dual_data=ready      dual_owner=b    userdata_mib=…        twindata_mib=…   twinmeta_mib=…
disk=/dev/block/sdc
slot_a=priority:15 tries:0 successful:1 corrupted:0 bootable:yes installed:yes data:primary expected:primary
rom_a=<android>|<build utc>|<ROM name from its build.prop>
slot_b=…
rom_b=…
```

Long commands print `progress <0-1000> <step>` lines; `setup`, `remove` and `table-save` print a
`table_copy=PATH` line per saved table copy. Exit codes: 0 ok, 1 usage, 2 refused (safe, nothing
written), 3 I/O error, 4 bad input.

## 7. The app's long jobs

`install` (and `verify` before it) take minutes, so the app does not run them in its own process. It writes
the commands into `/data/adb/twinslot/job.sh` and starts that as root, detached, with every
descriptor on `/dev/null`; the screen and a foreground service then read `job.log` (the engine's
`progress <permille> <step>` lines, its messages, and a final `exit N`).

The directory is root's alone: it is made (or taken over) `chown 0:0`, `chmod 0700` before anything is
written into it. Not `/data/local/tmp`: the `shell` user owns that, so `adb shell` could swap the script
between the write and root running it.

Three things this has to get right, each learned the hard way:

- **Start with one exec'd command.** `a && b && cmd > /dev/null &` backgrounds the whole list as a subshell
  that keeps a saved copy of su's stdout until the job ends, and the app, reading that stdout to its end,
  waits for the whole install before it shows any progress.
- **Leave the app's cgroup.** With KernelSU or APatch, `su` is a child of the app, so the job starts in
  `/sys/fs/cgroup/apps/uid_N/pid_M`. Android freezes that group while the app is in the background (the
  install stalls) and kills everything in it when the app dies (the install dies half-written: no `exit`
  line, "ended without a result"). `setsid` does not help. The script's first line moves itself to the
  root cgroup (and `/acct` on Android 11 and older). Magisk's su runs from its daemon and was never
  affected.
- **Hold a wake lock the system honours.** Writing to `/sys/power/wake_lock` as root is not enough on
  every ROM (on the CK7n's VoltageOS it is dropped within half a second). The foreground service holds a
  `PowerManager` partial wake lock while the job runs and shows the progress notification.

If the job dies anyway (the phone restarts), the log has no `exit` line and the app says so. The slot was
marked unbootable before the first write, so nothing half-written can boot.

## 8. Later

- A power-on chooser in lk (volume keys at boot). It is the only part that could hard-brick, so it comes
  last and only with a BROM recovery path.
- Shared storage between the ROMs (a third partition for photos and downloads).
- Installing GSIs and super images (`.img`) besides payload zips.
