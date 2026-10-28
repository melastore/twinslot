"""Shared test fixtures: a fake phone made of image files.

Every image is built with the reference Android and Linux tools (lpmake,
mkfs.erofs, mkbootimg, avbtool, sfdisk, lz4) and every result is checked with
them too (lpdump, lpunpack, fsck.erofs, unpack_bootimg, sfdisk --json), so the
C code is never only checked against itself.
"""
import gzip
import hashlib
import json
import sys
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "out", "twinslot-host")
MiB = 1 << 20
TESTKEY = os.path.join(ROOT, "third_party", "avb", "testkey_rsa2048.pem")
GiB = 1 << 30

BCTL_OFFSET = 2048
BCTL_MAGIC = 0x42414342

PRIMARY_FSTAB = (
    "# test fstab\n"
    "system /system erofs ro wait,slotselect,avb=vbmeta_system,logical,first_stage_mount\n"
    "vendor /vendor erofs ro wait,slotselect,avb,logical,first_stage_mount\n"
    "/dev/block/by-name/metadata /metadata ext4 noatime,nosuid,nodev,discard "
    "wait,check,formattable,first_stage_mount,metadata_csum\n"
    "/dev/block/by-name/userdata /data f2fs noatime,nosuid,nodev,discard,inlinecrypt "
    "wait,check,formattable,quota,latemount,fileencryption=aes-256-xts:aes-256-cts:v2,"
    "keydirectory=/metadata/vold/metadata_encryption,fsverity\n"
    "/dev/block/by-name/misc /misc emmc defaults defaults\n"
)
TWRP_FLAGS = (
    "/metadata ext4 /dev/block/by-name/metadata flags=display=\"Metadata\";backup=1\n"
    "/data f2fs /dev/block/by-name/userdata flags=fileencryption=aes-256-xts\n"
)


def twin_text(text):
    """What a primary fstab must look like once rebound to twin data."""
    return (text.replace("/dev/block/by-name/userdata", "/dev/block/by-name/twindata")
            .replace("/dev/block/by-name/metadata", "/dev/block/by-name/twinmeta"))


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def have(*tools):
    return all(shutil.which(t) for t in tools)


def save(path, data):
    with open(path, "wb") as f:
        f.write(data)


def save_tmp(tmp, name, data):
    path = os.path.join(tmp, name)
    save(path, data)
    return path


def load(path):
    with open(path, "rb") as f:
        return f.read()


def sparse_file(path, size):
    with open(path, "wb") as f:
        f.truncate(size)


# ---- the A/B record ----

def make_bctl(suffix=b"_a", slots=((15, 0, 1), (14, 0, 1)), merge=0, nb_slot=2, bad_crc=False):
    """slots: (priority, tries, successful) for a and b."""
    raw = bytearray(32)
    raw[0:4] = suffix.ljust(4, b"\0")
    struct.pack_into("<IB", raw, 4, BCTL_MAGIC, 1)
    raw[9] = nb_slot | (7 << 3)
    raw[10] = merge
    for i, (prio, tries, ok) in enumerate(slots):
        raw[12 + 2 * i] = prio | tries << 4 | ok << 7
    crc = zlib.crc32(bytes(raw[:28])) ^ (1 if bad_crc else 0)
    struct.pack_into("<I", raw, 28, crc)
    return bytes(raw)


def decode_bctl(raw):
    assert struct.unpack_from("<I", raw, 4)[0] == BCTL_MAGIC
    assert struct.unpack_from("<I", raw, 28)[0] == zlib.crc32(raw[:28]), "crc"
    slots = []
    for i in range(2):
        b0, b1 = raw[12 + 2 * i], raw[13 + 2 * i]
        slots.append((b0 & 15, (b0 >> 4) & 7, b0 >> 7, b1 & 1))
    return raw[0:4].rstrip(b"\0"), raw[10] & 7, slots


# ---- cpio, ramdisks, vendor_boot ----

def cpio_newc(files):
    """files: {path: bytes or None for a directory}."""
    out = bytearray()
    ino = 1

    def entry(name, mode, data):
        nonlocal ino
        name_b = name.encode() + b"\0"
        hdr = b"070701" + b"".join(b"%08X" % v for v in (
            ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name_b), 0))
        ino += 1
        out.extend(hdr + name_b)
        out.extend(b"\0" * (-len(out) % 4))
        out.extend(data)
        out.extend(b"\0" * (-len(out) % 4))

    for path in sorted(files):
        data = files[path]
        entry(path, 0o040755 if data is None else 0o100644, b"" if data is None else data)
    entry("TRAILER!!!", 0, b"")
    return bytes(out)


def cpio_read(data):
    files, pos = {}, 0
    while True:
        hdr = data[pos:pos + 110]
        assert hdr[:6] == b"070701", hdr[:6]
        f = [int(hdr[6 + 8 * i:14 + 8 * i], 16) for i in range(13)]
        size, namesize = f[6], f[11]
        name = data[pos + 110:pos + 110 + namesize - 1].decode()
        pos += 110 + namesize
        pos += -pos % 4
        if name == "TRAILER!!!":
            return files
        files[name] = data[pos:pos + size]
        pos += size
        pos += -pos % 4


def lz4_legacy(data, tmp):
    src, dst = os.path.join(tmp, "rd.raw"), os.path.join(tmp, "rd.lz4")
    save(src, data)
    run(["lz4", "-l", "-12", "-f", "-q", src, dst])
    out = load(dst)
    os.unlink(src)
    os.unlink(dst)
    return out


def lz4_legacy_decode(data, tmp):
    src, dst = os.path.join(tmp, "x.lz4"), os.path.join(tmp, "x.raw")
    save(src, data)
    run(["lz4", "-d", "-f", "-q", src, dst])
    out = load(dst)
    os.unlink(src)
    os.unlink(dst)
    return out


def compress_ramdisk(data, how, tmp):
    """how: True or "lz4" (legacy frames, as MediaTek builds), "gzip", or False for plain cpio."""
    if how is True or how == "lz4":
        return lz4_legacy(data, tmp)
    if how == "gzip":
        return gzip.compress(data, compresslevel=9, mtime=0)
    return data


def decompress_ramdisk(data, tmp):
    if data[:4] == b"\x02\x21\x4c\x18":
        return lz4_legacy_decode(data, tmp)
    if data[:2] == b"\x1f\x8b":
        return gzip.decompress(data)
    return data


FOOTER_PAD = b"pad!" * (16 * 1024)


LZ4_FRAME_FRAGMENT = b"\x04\x22\x4d\x18" + b"dlkm modules in LZ4 frames" * 100


def make_vendor_boot(path, tmp, fstab=PRIMARY_FSTAB, recovery_fstab=None, flags=TWRP_FLAGS,
                     part_size=16 * MiB, compress=True, extra_fragment=None):
    """extra_fragment: raw bytes of a third fragment (type dlkm), stored as they are."""
    rd0 = cpio_newc({
        "first_stage_ramdisk": None,
        "first_stage_ramdisk/system": None,
        "first_stage_ramdisk/system/etc": None,
        "first_stage_ramdisk/system/etc/fstab.mt6789": fstab.encode(),
        "first_stage_ramdisk/init": os.urandom(300000),
    })
    rd1 = cpio_newc({
        "system": None,
        "system/etc": None,
        "system/etc/recovery.fstab": (recovery_fstab or fstab).encode(),
        "system/etc/twrp.flags": flags.encode(),
        "system/bin/recovery": b"\x7fELF" + b"r" * 200000,
    })
    frags = []
    for i, rd in enumerate((rd0, rd1)):
        p = os.path.join(tmp, "frag%d" % i)
        save(p, compress_ramdisk(rd, compress, tmp))
        frags.append(p)
    dtb = os.path.join(tmp, "dtb")
    save(dtb, b"\xd0\x0d\xfe\xed" + bytes(2000))
    img = os.path.join(tmp, "vendor_boot.img")
    run(["mkbootimg", "--header_version", "4", "--pagesize", "4096", "--vendor_boot", img,
         "--dtb", dtb, "--vendor_cmdline", "bootopt=64S3,32N2,64N2",
         "--ramdisk_type", "platform", "--ramdisk_name", "", "--vendor_ramdisk_fragment", frags[0],
         "--ramdisk_type", "recovery", "--ramdisk_name", "recovery", "--vendor_ramdisk_fragment", frags[1],
         *(["--ramdisk_type", "dlkm", "--ramdisk_name", "dlkm", "--vendor_ramdisk_fragment",
             save_tmp(tmp, "frag2", extra_fragment)] if extra_fragment else [])])
    data = load(img)
    assert len(data) <= part_size
    save(path, data + bytes(part_size - len(data)))


def vendor_boot_files(path, tmp):
    """{fragment index: {path: bytes}} via unpack_bootimg, the reference parser."""
    out = os.path.join(tmp, "unpacked")
    shutil.rmtree(out, ignore_errors=True)
    run(["unpack_bootimg", "--boot_img", path, "--out", out])
    result = {}
    for i in range(8):
        frag = os.path.join(out, "vendor_ramdisk%02d" % i)
        if not os.path.exists(frag):
            break
        data = load(frag)
        # A fragment TwinSlot leaves alone (LZ4 frames): its raw bytes.
        result[i] = {"<raw>": data} if data[:4] == LZ4_FRAME_FRAGMENT[:4] else cpio_read(decompress_ramdisk(data, tmp))
    shutil.rmtree(out)
    return result


BOOT_V2_PAGE = 2048


def make_boot_v2(tmp, fstab=PRIMARY_FSTAB, flags=TWRP_FLAGS, compress="gzip", ramdisk=None):
    """A boot image header v2 as phones without vendor_boot carry (Android 10/11 launch): the
    ramdisk holds the first-stage fstab and recovery. Returns (image bytes, {section: bytes})."""
    rd = ramdisk if ramdisk is not None else compress_ramdisk(cpio_newc({
        "first_stage_ramdisk": None,
        "first_stage_ramdisk/fstab.mt6768": fstab.encode(),
        "system": None,
        "system/etc": None,
        "system/etc/recovery.fstab": fstab.encode(),
        "system/etc/twrp.flags": flags.encode(),
        "init": b"\x7fELF" + os.urandom(150000),
    }), compress, tmp)
    sections = {"kernel": b"\x1f\x8b" + os.urandom(700000), "ramdisk": rd,
                "dtb": b"\xd0\x0d\xfe\xed" + os.urandom(3000), "recovery_dtbo": b"\xd7\xb7\xab\x1e" + os.urandom(5000)}
    files = {}
    for k, v in sections.items():
        files[k] = os.path.join(tmp, "boot-" + k)
        save(files[k], v)
    img = os.path.join(tmp, "boot-v2.img")
    run(["mkbootimg", "--header_version", "2", "--pagesize", str(BOOT_V2_PAGE), "--kernel", files["kernel"],
         "--ramdisk", files["ramdisk"], "--dtb", files["dtb"], "--recovery_dtbo", files["recovery_dtbo"],
         "--cmdline", "bootopt=64S3,32N2,64N2", "--os_version", "11.0.0", "--os_patch_level", "2021-06",
         "--output", img])
    return load(img), sections


def boot_v2_sections(data):
    """Header fields and sections of a boot image v2, parsed here from the format."""
    page = struct.unpack_from("<I", data, 36)[0]
    kernel, ramdisk, second = (struct.unpack_from("<I", data, o)[0] for o in (8, 16, 24))
    rdtbo, rdtbo_off = struct.unpack_from("<IQ", data, 1632)
    dtb = struct.unpack_from("<I", data, 1648)[0]
    out, o = {}, page
    for name, n in (("kernel", kernel), ("ramdisk", ramdisk), ("second", second), ("recovery_dtbo", rdtbo),
                    ("dtb", dtb)):
        if name == "recovery_dtbo":
            out["recovery_dtbo_offset_ok"] = rdtbo_off == o
        out[name] = data[o:o + n]
        o += ALIGN(n, page)
    sha = hashlib.sha1()
    for name in ("kernel", "ramdisk", "second", "recovery_dtbo", "dtb"):
        sha.update(out[name])
        sha.update(struct.pack("<I", len(out[name])))
    out["id_ok"] = data[576:608] == sha.digest().ljust(32, b"\0")
    out["length"] = o
    return out


# ---- EROFS ----

def make_erofs(path, files, tmp, options=("-zlz4hc",), size=None):
    """files: {relative path: bytes}."""
    root = tempfile.mkdtemp(dir=tmp)
    for rel, data in files.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        save(p, data)
    run(["mkfs.erofs", "-T", "1700000000", "-U", "c0ffee00-1111-4222-8333-444455556666",
         *options, path, root])
    shutil.rmtree(root)
    # lpmake takes whole 4 KiB blocks; an image of smaller blocks may end mid-block.
    size = size or -(-os.path.getsize(path) // 4096) * 4096
    with open(path, "r+b") as f:
        f.truncate(size)


def erofs_extract(img, tmp):
    """{relative path: bytes} via fsck.erofs, which also checks the image."""
    out = tempfile.mkdtemp(dir=tmp)
    run(["fsck.erofs", "--extract=" + out, "--overwrite", img])
    files = {}
    for dirpath, _, names in os.walk(out):
        for n in names:
            p = os.path.join(dirpath, n)
            files[os.path.relpath(p, out)] = (b"symlink:" + os.readlink(p).encode()) if os.path.islink(p) else load(p)
    shutil.rmtree(out)
    return files


# ---- super ----

def make_super(path, tmp, size, groups, partitions, slots=3):
    """partitions: [(name, group, image path or None, size or None)]."""
    sparse = os.path.join(tmp, "super.simg")
    cmd = ["lpmake", "--metadata-size", "65536", "--super-name", "super", "--metadata-slots", str(slots),
           "--device", "super:%d" % size, "--virtual-ab", "--sparse", "-o", sparse]
    for name, gsize in groups:
        cmd += ["--group", "%s:%d" % (name, gsize)]
    for name, group, image, psize in partitions:
        if psize is None:
            psize = os.path.getsize(image) if image else 0
        cmd += ["--partition", "%s:readonly:%d:%s" % (name, psize, group)]
        if image:
            cmd += ["--image", "%s=%s" % (name, image)]
    run(cmd)
    # Without any partition image lpmake writes a metadata-only file instead.
    assert any(image for _, _, image, _ in partitions), "give at least one partition an image"
    run(["simg2img", sparse, path])
    os.unlink(sparse)


def lpdump(path, slot=0):
    return json.loads(run(["lpdump", "--json", "--slot=%d" % slot, path]).stdout)


def ALIGN(n, a):
    return (n + a - 1) // a * a


def pad_to(data, size):
    return data + bytes(size - len(data))


def vendor_boot_length(path):
    """Header, ramdisks, dtb, ramdisk table and bootconfig, each page-aligned (header v4)."""
    with open(path, "rb") as f:
        h = f.read(4096)
    page, ramdisk = struct.unpack_from("<I", h, 12)[0], struct.unpack_from("<I", h, 24)[0]
    header, dtb = struct.unpack_from("<II", h, 2096)
    table, bootconfig = struct.unpack_from("<I", h, 2112)[0], struct.unpack_from("<I", h, 2124)[0]
    return sum(ALIGN(n, page) for n in (header, ramdisk, dtb, table, bootconfig))


def avb_verify(phone, slot, tmp):
    """avbtool verify_image over the slot as the bootloader and init see it; returns the vbmeta info."""
    d = tempfile.mkdtemp(dir=tmp)
    shutil.copy(phone.path("vbmeta_" + slot), os.path.join(d, "vbmeta.img"))
    if os.path.exists(phone.path("vbmeta_vendor_" + slot)):
        shutil.copy(phone.path("vbmeta_vendor_" + slot), os.path.join(d, "vbmeta_vendor.img"))
    for name in ("vendor_boot", "boot"):
        if os.path.exists(phone.path("%s_%s" % (name, slot))):
            shutil.copy(phone.path("%s_%s" % (name, slot)), os.path.join(d, name + ".img"))
    save(os.path.join(d, "vendor.img"), lp_extract(phone.path("super"), "vendor_" + slot, tmp))
    run(["avbtool", "verify_image", "--image", os.path.join(d, "vbmeta.img"), "--key", TESTKEY,
         "--follow_chain_partitions"])
    return subprocess.run(["avbtool", "info_image", "--image", os.path.join(d, "vbmeta.img")],
                          capture_output=True, text=True, check=True).stdout


def lp_extract(path, name, tmp):
    out = tempfile.mkdtemp(dir=tmp)
    run(["lpunpack", "--partition=" + name, path, out])
    data = load(os.path.join(out, name + ".img"))
    shutil.rmtree(out)
    return data


# ---- GPT disk ----

LINUX_DATA = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"


MTK_LAYOUT = [("frp", 1), ("metadata", 32), ("nvdata", 8), ("tranfs", 64), ("userdata", None), ("flashinfo", 16)]


def make_disk(path, size=20 * GiB, sector=4096, layout=None, mtk=False):
    """A GPT disk like a UFS LUN. sfdisk ignores the sector size of plain
    files, so the table is written here and only checked with sfdisk.

    layout: [(name, size in MiB or None for the rest)].
    mtk: like MediaTek's: the entry count is exactly the partitions, a
    zero-type record of MediaTek's own follows them, first_usable is 34 and
    there is no backup copy (the last block is zero).
    """
    import uuid
    layout = layout or (MTK_LAYOUT if mtk else [("frp", 1), ("metadata", 32), ("nvdata", 8), ("userdata", None)])
    nlba = size // sector
    entries_lbas = 128 * 128 // sector
    num = len(layout) if mtk else 128
    first_usable = 34 if mtk else 2 + entries_lbas
    last_usable = nlba - 9 if mtk else nlba - 2 - entries_lbas
    align = MiB // sector
    table = bytearray(128 * 128)
    lba = align
    for i, (name, mib) in enumerate(layout):
        after = sum(m or 0 for _, m in layout[i + 1:]) * MiB // sector
        count = mib * MiB // sector if mib else (last_usable + 1 - after - lba) // align * align
        e = uuid.UUID(LINUX_DATA).bytes_le + uuid.uuid4().bytes_le
        e += struct.pack("<QQQ", lba, lba + count - 1, 0) + name.encode("utf-16-le").ljust(72, b"\0")
        table[i * 128:(i + 1) * 128] = e
        lba += count
    if mtk:
        table[num * 128 + 32:num * 128 + 48] = struct.pack("<QQ", nlba - 4101, nlba - 6)
    table_crc = zlib.crc32(table[:num * 128])
    disk_guid = uuid.uuid4().bytes_le

    def header(my, alt, entries):
        h = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, my, alt,
                                  first_usable, last_usable, disk_guid, entries, num, 128, table_crc))
        struct.pack_into("<I", h, 16, zlib.crc32(h))
        return bytes(h) + bytes(sector - 92)

    sparse_file(path, size)
    mbr = bytearray(512)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, min(nlba - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    with open(path, "r+b") as f:
        f.write(mbr)
        f.seek(sector)
        f.write(header(1, nlba - 1, 2))
        f.seek(2 * sector)
        f.write(table)
        if not mtk:
            f.seek((nlba - 1 - entries_lbas) * sector)
            f.write(table)
            f.write(header(nlba - 1, 1, nlba - 1 - entries_lbas))


def gpt_raw(path, sector=4096):
    """The primary GPT parsed here (sfdisk insists on a backup copy):
    (header fields, raw entry bytes up to first_usable, {name: entry})."""
    import uuid
    with open(path, "rb") as f:
        f.seek(sector)
        h = f.read(92)
        f.seek(2 * sector)
        raw = f.read(32 * sector)
    names = ("sig", "rev", "hsize", "crc", "res", "my", "alt", "first", "last", "guid", "entries", "num",
             "esize", "ecrc")
    hd = dict(zip(names, struct.unpack("<8sIIIIQQQQ16sQIII", h)))
    z = bytearray(h)
    z[16:20] = bytes(4)
    assert zlib.crc32(z) == hd["crc"], "header crc"
    assert zlib.crc32(raw[:hd["num"] * 128]) == hd["ecrc"], "entries crc"
    parts = {}
    for i in range(hd["num"]):
        e = raw[i * 128:(i + 1) * 128]
        if e[:16] != bytes(16):
            first, last, attrs = struct.unpack("<QQQ", e[32:56])
            name = e[56:].decode("utf-16-le").split("\0")[0]
            parts[name] = {"index": i, "start": first, "size": last - first + 1, "attrs": attrs,
                           "type": str(uuid.UUID(bytes_le=e[:16])), "raw_name": e[56:]}
    return hd, raw, parts


def disk_tables(path):
    """The first and last MiB of a disk: both GPT copies, without reading the rest."""
    with open(path, "rb") as f:
        head = f.read(MiB)
        f.seek(-MiB, 2)
        return head + f.read()


def sfdisk_json(path, sector=4096):
    t = json.loads(run(["sfdisk", "--sector-size", str(sector), "--json", path]).stdout)["partitiontable"]
    assert t["label"] == "gpt", t["label"]
    run(["sfdisk", "--sector-size", str(sector), "--verify", path])
    return t


def gpt_parts(path, sector=4096):
    t = sfdisk_json(path, sector)
    return {p["name"]: p for p in t["partitions"]}


# ---- the whole fake phone ----

class Phone:
    """A directory of partition images the binary works on through --dev-dir."""

    def __init__(self, tmp, current="a"):
        self.tmp = tmp
        self.dir = os.path.join(tmp, "dev")
        os.makedirs(self.dir)
        self.current = current
        self.misc(make_bctl())

    def path(self, name):
        return os.path.join(self.dir, name)

    def misc(self, raw):
        img = bytearray(64 * 1024)
        img[BCTL_OFFSET:BCTL_OFFSET + 32] = raw
        save(self.path("misc"), bytes(img))

    def bctl(self):
        with open(self.path("misc"), "rb") as f:
            f.seek(BCTL_OFFSET)
            return decode_bctl(f.read(32))

    def twinslot(self, *args, check=True, current=None, android=False):
        """android=True: act as if Android (not recovery) is running."""
        cur = current if current is not None else self.current
        cmd = [BIN, "--dev-dir", self.dir] + (["--current", cur] if cur else []) + list(args)
        env = dict(os.environ, TWINSLOT_IN_RECOVERY="0") if android else None
        p = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if check and p.returncode:
            raise AssertionError("%s -> %d\nstdout: %s\nstderr: %s" % (args, p.returncode, p.stdout, p.stderr))
        return p

    def status(self):
        out = {}
        for line in self.twinslot("status").stdout.splitlines():
            k, _, v = line.partition("=")
            out[k] = v
        return out

    def slot_fields(self, slot):
        return dict(f.split(":", 1) for f in self.status()["slot_" + slot].split())

    def add_disk(self, **kw):
        make_disk(self.path("disk"), **kw)

    def add_slot_images(self, slot, fstab=PRIMARY_FSTAB, flags=TWRP_FLAGS, vbmeta=True, boot_v2=None):
        """boot_v2: a phone without vendor_boot; its boot (header v2) has a ramdisk compressed so
        ("gzip", "lz4" or False)."""
        if boot_v2 is not None:
            img, _ = make_boot_v2(self.tmp, fstab=fstab, flags=flags, compress=boot_v2)
            save(self.path("boot_" + slot), pad_to(img, 32 * MiB))
            sparse_file(self.path("dtbo_%s" % slot), MiB)
            sparse_file(self.path("vbmeta_system_%s" % slot), 64 * 1024)
            if vbmeta:
                img = os.path.join(self.tmp, "vbmeta.img")
                run(["avbtool", "make_vbmeta_image", "--output", img])
                save(self.path("vbmeta_" + slot), pad_to(load(img), 64 * 1024))
            return
        make_vendor_boot(self.path("vendor_boot_" + slot), self.tmp, fstab=fstab, flags=flags)
        if vbmeta:
            img = os.path.join(self.tmp, "vbmeta.img")
            run(["avbtool", "make_vbmeta_image", "--output", img])
            data = load(img)
            save(self.path("vbmeta_" + slot), data + bytes(64 * 1024 - len(data)))
        for name, size in (("boot", 8 * MiB), ("dtbo", MiB), ("vbmeta_system", 64 * 1024)):
            sparse_file(self.path("%s_%s" % (name, slot)), size)

    def sign_slot(self, slot, venimg, footer=False, hash_alg="sha256", vendor_chain=False):
        """vbmeta as the CK7n's ROMs build it, signed with the AOSP test key: a vendor_boot hash
        descriptor and a chain to vbmeta_vendor, which holds vendor's hashtree (added to venimg).
        footer: vendor_boot keeps its unsigned AVB footer, its original size padded past the image
        as on the CK7n. On a phone without vendor_boot, boot is chained instead, its footer signed
        with a key of the ROM's own (not the test key). hash_alg: vendor's hashtree hash (avbtool's
        default is sha1). vendor_chain: no vbmeta_vendor; vbmeta chains to vendor's own signed footer
        (BOARD_AVB_VENDOR_KEY_PATH)."""
        size = os.path.getsize(venimg)
        sign = ["--key", TESTKEY, "--algorithm", "SHA256_RSA2048"] if vendor_chain else ["--algorithm", "NONE"]
        run(["avbtool", "add_hashtree_footer", "--image", venimg, "--partition_name", "vendor",
             "--partition_size", str(ALIGN(size + size // 50 + MiB, MiB)), "--hash_algorithm", hash_alg,
             "--do_not_generate_fec", "--salt", "bd2924e9" * 8] + sign)
        pub = os.path.join(self.tmp, "testkey.avbpubkey")
        run(["avbtool", "extract_public_key", "--key", TESTKEY, "--output", pub])
        top = os.path.join(self.tmp, "vbmeta_top.img")
        args = ["avbtool", "make_vbmeta_image", "--output", top, "--key", TESTKEY, "--algorithm", "SHA256_RSA2048"]
        if vendor_chain:
            args += ["--chain_partition", "vendor:3:" + pub]
        else:
            vv = os.path.join(self.tmp, "vbmeta_vendor.img")
            run(["avbtool", "make_vbmeta_image", "--output", vv, "--key", TESTKEY, "--algorithm", "SHA256_RSA2048",
                 "--include_descriptors_from_image", venimg, "--rollback_index", "1"])
            save(self.path("vbmeta_vendor_" + slot), pad_to(load(vv), 64 * 1024))
            args += ["--chain_partition", "vbmeta_vendor:3:" + pub]
        if not os.path.exists(self.path("vendor_boot_" + slot)):
            key, keypub = self.rom_key()
            boot = os.path.join(self.tmp, "boot_signed.img")
            save(boot, load(self.path("boot_" + slot))[:boot_v2_sections(load(self.path("boot_" + slot)))["length"]])
            run(["avbtool", "add_hash_footer", "--image", boot, "--partition_name", "boot", "--partition_size",
                 str(32 * MiB), "--salt", "5a17b007" * 8, "--key", key, "--algorithm", "SHA256_RSA2048"])
            shutil.copy(boot, self.path("boot_" + slot))
            args += ["--chain_partition", "boot:2:" + keypub]
        else:
            vb = os.path.join(self.tmp, "vendor_boot_signed.img")
            image = load(self.path("vendor_boot_" + slot))[:vendor_boot_length(self.path("vendor_boot_" + slot))]
            # The padding is not zeros, so a rebind that loses it shows (vendors may keep data there).
            save(vb, image + (FOOTER_PAD if footer else b""))
            run(["avbtool", "add_hash_footer", "--image", vb, "--partition_name", "vendor_boot", "--partition_size",
                 str(16 * MiB), "--salt", "13ef4de1" * 8, "--algorithm", "NONE"])
            if footer:
                shutil.copy(vb, self.path("vendor_boot_" + slot))
            args += ["--include_descriptors_from_image", vb]
        run(args)
        save(self.path("vbmeta_" + slot), pad_to(load(top), 64 * 1024))

    def rom_key(self):
        """A signing key of the ROM's own, as a release-signed ROM has."""
        key = os.path.join(self.tmp, "romkey.pem")
        pub = os.path.join(self.tmp, "romkey.avbpubkey")
        if not os.path.exists(key):
            run(["openssl", "genrsa", "-out", key, "2048"])
            run(["avbtool", "extract_public_key", "--key", key, "--output", pub])
        return key, pub

    def add_super(self, slots=("a", "b"), size=512 * MiB, fstab=PRIMARY_FSTAB, rom="TestROM 1.0",
                  erofs_options=("-zlz4hc",), vendor_files=None, installed=("a", "b"), avb=False, product_props=None,
                  footer=False):
        """Each installed slot gets an EROFS system (build.prop) and vendor (etc/fstab.mt6789).
        With product_props, the ROM's version is only in product's etc/build.prop, as on AxionOS."""
        parts = []
        for s in slots:
            if s in installed:
                sysimg = os.path.join(self.tmp, "system_%s.img" % s)
                version = ("ro.build.display.id=BP4A.251205.006 release-keys\n" if product_props
                           else "ro.modversion=%s (%s)\n" % (rom, s))
                make_erofs(sysimg, {"system/build.prop": (
                    "ro.build.version.release=17\nro.build.date.utc=1760000000\n" + version).encode()}, self.tmp)
                if product_props:
                    prodimg = os.path.join(self.tmp, "product_%s.img" % s)
                    make_erofs(prodimg, {"etc/build.prop": product_props.encode()}, self.tmp)
                    parts.append(("product_" + s, "main_" + s, prodimg, None))
                venimg = os.path.join(self.tmp, "vendor_%s.img" % s)
                files = {"etc/fstab.mt6789": fstab.encode(), "etc/init/hw/init.rc": b"on init\n" * 500}
                files.update(vendor_files or {})
                make_erofs(venimg, files, self.tmp, erofs_options)
                if avb:
                    self.sign_slot(s, venimg, footer=footer, **(avb if isinstance(avb, dict) else {}))
                parts += [("system_" + s, "main_" + s, sysimg, None), ("vendor_" + s, "main_" + s, venimg, None)]
            else:
                parts += [("system_" + s, "main_" + s, None, 0)]
        make_super(self.path("super"), self.tmp, size, [("main_a", size // 2 - MiB), ("main_b", size // 2 - MiB)],
                   parts)


class PhoneTest(unittest.TestCase):
    def setUp(self):
        if not os.path.exists(BIN):
            self.fail("build first: make host")
        self._td = tempfile.TemporaryDirectory(prefix="twinslot-")
        self.tmp = self._td.name
        self.phone = Phone(self.tmp)

    def tearDown(self):
        self._td.cleanup()

    def require(self, *tools):
        if not have(*tools):
            self.skipTest("needs " + ", ".join(tools))

    def full_phone(self, boot_v2=None, **kw):
        self.require("lpmake", "simg2img", "mkfs.erofs", "mkbootimg", "avbtool", "lz4", "sfdisk")
        p = self.phone
        p.add_disk()
        for s in ("a", "b"):
            p.add_slot_images(s, boot_v2=boot_v2)
        p.add_super(**kw)
        return p

    def dual_phone(self, **kw):
        """A full phone with dual data: slot b owns twindata and is bound to it, boot control reset."""
        p = self.full_phone(**kw)
        p.twinslot("setup", "--size", "8G", "--owner", "b", "--apply")
        p.twinslot("rebind", "b", "twin")
        p.misc(make_bctl())
        return p


# ---- ROM zips ----

def zstd_compress(data):
    return subprocess.run(["zstd", "-q", "-c"], input=data, capture_output=True, check=True).stdout


def pad4k(data):
    return data + bytes(-len(data) % 4096)


def make_rom_zip(tmp, name="OtherROM 2.0", fstab=PRIMARY_FSTAB, extra=(), extra_dynamic=(), corrupt=None,
                 group_size=200 * MiB, system_size=3 * MiB):
    """A full-OTA zip like a ROM build's. Returns (zip path, {partition: image bytes})."""
    import bz2
    import lzma
    import zipfile
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import payload_ref as pr

    sysimg = os.path.join(tmp, "rom_system.img")
    make_erofs(sysimg, {"system/build.prop": (
        "ro.build.version.release=18\nro.build.date.utc=1770000000\nro.modversion=%s\n" % name).encode(),
        "system/big": os.urandom(system_size)}, tmp)
    venimg = os.path.join(tmp, "rom_vendor.img")
    make_erofs(venimg, {"etc/fstab.mt6789": fstab.encode(), "lib/x.so": os.urandom(MiB)}, tmp)
    vbimg = os.path.join(tmp, "rom_vendor_boot.img")
    make_vendor_boot(vbimg, tmp, fstab=fstab, part_size=12 * MiB)
    same = lambda d: d  # noqa: E731
    parts = [
        ("system", pad4k(load(sysimg)),
         [(pr.REPLACE_XZ, lzma.compress), (pr.REPLACE_BZ, bz2.compress), (pr.REPLACE_ZSTD, zstd_compress)]),
        ("vendor", pad4k(load(venimg)), [(pr.REPLACE, same), (pr.REPLACE_XZ, lzma.compress)]),
        ("vendor_boot", load(vbimg), [(pr.REPLACE_XZ, lzma.compress)]),
        ("boot", os.urandom(MiB) + bytes(MiB), [(pr.REPLACE, same), (pr.ZERO, None)]),
        ("lk", os.urandom(8192), [(pr.REPLACE, same)]),
    ] + list(extra)
    blob = bytearray(pr.build_payload(parts, [("mtk_dynamic_partitions", group_size, ["system", "vendor", *extra_dynamic])]))
    if corrupt:
        corrupt(blob)  # gets the payload: 24-byte header, then the manifest, then the data
    path = os.path.join(tmp, "rom.zip")
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr(zipfile.ZipInfo("payload.bin"), bytes(blob), compress_type=zipfile.ZIP_STORED)
        zf.writestr("payload_properties.txt", "FILE_HASH=x\n", compress_type=zipfile.ZIP_DEFLATED)
        zf.writestr("META-INF/com/android/metadata", "ota-type=AB\n", compress_type=zipfile.ZIP_DEFLATED)
    return path, {n: img for n, img, _ in parts}
