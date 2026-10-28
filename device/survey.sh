#!/system/bin/sh
# Read-only survey of a phone for twinslot. Run as root; writes only into $OUT.
#   sh survey.sh OUT_DIR TWINSLOT_BINARY

OUT=${1:-/data/local/tmp/twinslot-survey}
BIN=${2:-/data/local/tmp/twinslot}

rm -rf "$OUT"
mkdir -p "$OUT" || exit 1
cd "$OUT" || exit 1

run() {
	name=$1
	shift
	"$@" > "$name" 2>&1
}

# Read the first COUNT 4 KiB blocks of a block device (never writes it).
head_of() {
	[ -e "$1" ] && dd if="$1" of="$2" bs=4096 count="$3" 2>/dev/null
}

run getprop.txt getprop
run cmdline.txt cat /proc/cmdline
run bootconfig.txt cat /proc/bootconfig
run partitions.txt cat /proc/partitions
run by-name.txt ls -l /dev/block/by-name/
run mounts.txt cat /proc/mounts
run lpdump-slot0.txt lpdump --slot=0
run lpdump-slot1.txt lpdump --slot=1
run snapshot.txt snapshotctl dump
run twinslot-status.txt "$BIN" status
run metadata-tree.txt ls -laR /metadata
run fstab.txt sh -c 'for f in /vendor/etc/fstab.* /odm/etc/fstab.* /first_stage_ramdisk/fstab.* /fstab.*; do [ -f "$f" ] && { echo "== $f"; cat "$f"; }; done'
run f2fs.txt sh -c 'for d in /sys/fs/f2fs/*/; do echo "== $d"; for k in features encoding; do [ -f "$d$k" ] && echo "$k: $(cat "$d$k")"; done; done; cat /proc/fs/f2fs/*/segment_info'

# Slot record, boot image headers (they carry the OS patch level), super metadata.
head_of /dev/block/by-name/misc misc-head.bin 1
for p in boot vendor_boot dtbo vbmeta; do
	for s in _a _b; do
		head_of /dev/block/by-name/$p$s $p$s-head.bin 1
	done
done
head_of /dev/block/by-name/super super-meta.bin 256

# Partition tables: the first and last 8 blocks of every UFS LUN.
for d in /sys/block/sd*; do
	[ -e "$d" ] || continue # eMMC phones have no UFS LUNs
	n=${d##*/}
	lbs=$(cat "$d/queue/logical_block_size")
	sectors=$(cat "$d/size")
	echo "$n logical_block_size=$lbs size_512=$sectors" >> luns.txt
	dd if=/dev/block/$n of=gpt-$n-head.bin bs="$lbs" count=8 2>/dev/null
	dd if=/dev/block/$n of=gpt-$n-tail.bin bs="$lbs" skip=$((sectors / (lbs / 512) - 8)) count=8 2>/dev/null
done

# Where userdata sits, and its filesystem superblock.
readlink -f /dev/block/by-name/userdata > userdata-dev.txt
head_of /dev/block/by-name/userdata userdata-sb.bin 1

chmod -R a+rX "$OUT"
echo "survey written to $OUT"
