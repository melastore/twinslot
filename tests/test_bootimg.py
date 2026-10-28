"""The boot image side of a rebind on phones other than the CK7n: AVB footers kept and re-sealed,
phones without vendor_boot (boot header v2, recovery in boot), and gzip ramdisks."""
import hashlib
import os
import struct
import subprocess

from lib import (FOOTER_PAD, PRIMARY_FSTAB, TWRP_FLAGS, PhoneTest, avb_verify, boot_v2_sections, cpio_read,
                 decompress_ramdisk, load, make_boot_v2, pad_to, save, twin_text, vendor_boot_files, vendor_boot_length)
from lib import MiB


def info(path):
    return subprocess.run(["avbtool", "info_image", "--image", path], capture_output=True, text=True,
                          check=True).stdout


def field(text, name, nth=0):
    return [line.split(":", 1)[1].strip() for line in text.splitlines() if line.strip().startswith(name + ":")][nth]


def digests(text):
    """{partition name: digest} of the hash descriptors in avbtool info_image output."""
    out, name = {}, None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("Partition Name:"):
            name = line.split(":", 1)[1].strip()
        elif line.startswith("Digest:") and name:
            out[name] = line.split(":", 1)[1].strip()
    return out


class BootImageTest(PhoneTest):
    def split(self, p):
        p.twinslot("setup", "--size", "8G", "--apply")

    def test_vendor_boot_footer_is_kept_and_resealed(self):
        """As on the CK7n: vendor_boot ends in an unsigned AVB footer whose original size is padded."""
        p = self.full_phone(avb=True, footer=True)
        before = info(p.path("vendor_boot_b"))
        original = int(field(before, "Original image size").split()[0])
        old_len = vendor_boot_length(p.path("vendor_boot_b"))
        self.assertEqual(load(p.path("vendor_boot_b"))[old_len:original], FOOTER_PAD)
        self.split(p)
        p.twinslot("rebind", "b")
        # What lay between the image and the original size is hashed, and stays.
        keep = max(old_len, vendor_boot_length(p.path("vendor_boot_b")))
        self.assertLess(keep, original)
        self.assertEqual(load(p.path("vendor_boot_b"))[keep:original], FOOTER_PAD[keep - old_len:])

        after = info(p.path("vendor_boot_b"))
        self.assertEqual(int(field(after, "Original image size").split()[0]), original)  # the padding stays hashed
        self.assertEqual(field(after, "Algorithm"), "NONE")  # it was unsigned, and stays so
        footer_digest = digests(after)["vendor_boot"]
        self.assertNotEqual(footer_digest, digests(before)["vendor_boot"])
        image = load(p.path("vendor_boot_b"))[:original]
        salt = bytes.fromhex("13ef4de1" * 8)
        self.assertEqual(footer_digest, hashlib.sha256(salt + image).hexdigest())
        vbmeta = avb_verify(p, "b", self.tmp)
        self.assertEqual(digests(vbmeta)["vendor_boot"], footer_digest)
        files = vendor_boot_files(p.path("vendor_boot_b"), self.tmp)
        self.assertEqual(files[0]["first_stage_ramdisk/system/etc/fstab.mt6789"].decode(), twin_text(PRIMARY_FSTAB))

    def test_phone_without_vendor_boot(self):
        """boot header v2, gzip ramdisk with the first-stage fstab and recovery, boot chained in vbmeta
        and signed with the ROM's own key."""
        p = self.full_phone(boot_v2="gzip", avb=True)
        old = boot_v2_sections(load(p.path("boot_b")))
        self.split(p)
        self.assertEqual(p.slot_fields("b")["data"], "primary")
        r = p.twinslot("rebind", "b")
        self.assertIn("in boot, 2 in vendor", r.stdout)
        self.assertIn("signed with another key", r.stderr)

        data = load(p.path("boot_b"))
        new = boot_v2_sections(data)
        files = cpio_read(decompress_ramdisk(new["ramdisk"], self.tmp))
        self.assertEqual(new["ramdisk"][:2], b"\x1f\x8b")  # still gzip
        self.assertEqual(files["first_stage_ramdisk/fstab.mt6768"].decode(), twin_text(PRIMARY_FSTAB))
        self.assertEqual(files["system/etc/recovery.fstab"].decode(), twin_text(PRIMARY_FSTAB))
        self.assertEqual(files["system/etc/twrp.flags"].decode(), twin_text(TWRP_FLAGS))
        for name in ("kernel", "dtb", "recovery_dtbo"):
            self.assertEqual(new[name], old[name], name)
        self.assertTrue(new["recovery_dtbo_offset_ok"])
        self.assertTrue(new["id_ok"])
        subprocess.run(["unpack_bootimg", "--boot_img", p.path("boot_b"), "--out", os.path.join(self.tmp, "u")],
                       check=True, capture_output=True)

        # The footer is re-signed with the test key and the chain in vbmeta points at it.
        vbmeta = avb_verify(p, "b", self.tmp)
        self.assertIn("cdbb77177f731920bbe0a0f94f84d9038ae0617d", field(info(p.path("boot_b")), "Public key (sha1)"))
        self.assertEqual(vbmeta.count("cdbb77177f731920bbe0a0f94f84d9038ae0617d"), 3)  # vbmeta, both chains
        st = p.slot_fields("b")
        self.assertEqual((st["data"], st["expected"]), ("twin", "twin"))
        self.assertEqual(p.slot_fields("a")["data"], "primary")

        p.twinslot("switch", "b")  # its binding checks read the boot image too
        self.assertEqual(p.slot_fields("b")["bootable"], "yes")
        p.twinslot("remove", "--apply")
        files = cpio_read(decompress_ramdisk(boot_v2_sections(load(p.path("boot_b")))["ramdisk"], self.tmp))
        self.assertEqual(files["first_stage_ramdisk/fstab.mt6768"].decode(), PRIMARY_FSTAB)
        avb_verify(p, "b", self.tmp)

    def test_boot_v2_ramdisk_formats(self):
        for compress in ("lz4", False):
            with self.subTest(compress=compress):
                self.tearDown()
                self.setUp()
                p = self.full_phone(boot_v2=compress)
                self.split(p)
                p.twinslot("rebind", "b")
                new = boot_v2_sections(load(p.path("boot_b")))
                files = cpio_read(decompress_ramdisk(new["ramdisk"], self.tmp))
                self.assertEqual(files["system/etc/recovery.fstab"].decode(), twin_text(PRIMARY_FSTAB))
                self.assertTrue(new["id_ok"])
                self.assertEqual(p.slot_fields("b")["data"], "twin")

    def test_gzip_vendor_ramdisk(self):
        p = self.full_phone()
        from lib import make_vendor_boot
        for s in ("a", "b"):
            make_vendor_boot(p.path("vendor_boot_" + s), self.tmp, compress="gzip")
        self.split(p)
        p.twinslot("rebind", "b")
        files = vendor_boot_files(p.path("vendor_boot_b"), self.tmp)
        self.assertEqual(files[1]["system/etc/recovery.fstab"].decode(), twin_text(PRIMARY_FSTAB))
        self.assertEqual(p.slot_fields("b")["data"], "twin")

    def test_unreadable_fragment_is_left_alone(self):
        """A v4 fragment in a format TwinSlot does not read (a DLKM in LZ4 frames) holds no fstab: kept byte for byte."""
        p = self.full_phone()
        from lib import LZ4_FRAME_FRAGMENT, make_vendor_boot
        for s in ("a", "b"):
            make_vendor_boot(p.path("vendor_boot_" + s), self.tmp, extra_fragment=LZ4_FRAME_FRAGMENT)
        self.split(p)
        r = p.twinslot("rebind", "b")
        self.assertIn("fragment 2 is in a format TwinSlot does not read; left unchanged", r.stderr)
        files = vendor_boot_files(p.path("vendor_boot_b"), self.tmp)
        self.assertEqual(files[0]["first_stage_ramdisk/system/etc/fstab.mt6789"].decode(), twin_text(PRIMARY_FSTAB))
        self.assertEqual(files[1]["system/etc/recovery.fstab"].decode(), twin_text(PRIMARY_FSTAB))
        self.assertEqual(files[2]["<raw>"], LZ4_FRAME_FRAGMENT)
        self.assertEqual(p.slot_fields("b")["data"], "twin")

    def test_mediatek_wrapped_ramdisk_is_refused(self):
        p = self.full_phone(boot_v2="gzip")
        img, _ = make_boot_v2(self.tmp, ramdisk=struct.pack("<II", 0x58881688, 1000) + bytes(1000))
        save(p.path("boot_b"), pad_to(img, 32 * MiB))
        before = load(p.path("boot_b"))
        self.split(p)
        r = p.twinslot("rebind", "b", check=False)
        self.assertEqual(r.returncode, 4)
        self.assertIn("MediaTek header", r.stderr)
        self.assertEqual(load(p.path("boot_b")), before)
        self.assertEqual(p.slot_fields("b")["bootable"], "no")
