"""install / verify: flashing a full ROM zip into one slot."""
import hashlib
import os
import struct
import subprocess

from lib import (MiB, PRIMARY_FSTAB, PhoneTest, erofs_extract, load, lp_extract, lpdump, make_bctl, make_rom_zip,
                 twin_text, vendor_boot_files)
import payload_ref as pr


def manifest_len(blob):
    return struct.unpack_from(">Q", blob, 12)[0]


class InstallTest(PhoneTest):
    def setUp(self):
        super().setUp()
        p = self.full_phone()
        for s in "ab":
            with open(p.path("lk_" + s), "wb") as f:
                f.write(b"LK-" + s.encode() + bytes(8188))

    def twin(self):
        self.phone.twinslot("setup", "--size", "8G", "--apply")

    def logical(self, name):
        return lp_extract(self.phone.path("super"), name, self.tmp)

    def test_install_second_rom(self):
        p = self.phone
        self.twin()
        keep = {n: self.logical(n) for n in ("system_a", "vendor_a")}
        keep_files = {n: load(p.path(n)) for n in ("vendor_boot_a", "boot_a", "vbmeta_a", "lk_a", "lk_b")}
        zip_path, imgs = make_rom_zip(self.tmp)
        r = p.twinslot("install", zip_path)
        self.assertIn("installed: slot b boots next", r.stdout)
        self.assertIn("keep the device's lk", r.stdout)

        self.assertEqual(self.logical("system_b"), imgs["system"])
        self.assertEqual(load(p.path("boot_b"))[:len(imgs["boot"])], imgs["boot"])
        vendor = os.path.join(self.tmp, "vb.img")
        with open(vendor, "wb") as f:
            f.write(self.logical("vendor_b"))
        files = erofs_extract(vendor, self.tmp)
        self.assertEqual(files["etc/fstab.mt6789"].decode(), twin_text(PRIMARY_FSTAB))
        vb = vendor_boot_files(p.path("vendor_boot_b"), self.tmp)
        self.assertEqual(vb[0]["first_stage_ramdisk/system/etc/fstab.mt6789"].decode(), twin_text(PRIMARY_FSTAB))

        # The running ROM and the device's bootloader are untouched.
        for n, data in keep.items():
            self.assertEqual(self.logical(n), data, n)
        for n, data in keep_files.items():
            self.assertEqual(load(p.path(n)), data, n)

        dump = {x["name"]: x for x in lpdump(p.path("super"))["partitions"]}
        self.assertEqual(dump["system_b"]["group_name"], "main_b")
        self.assertEqual(int(dump["system_b"]["size"]), len(imgs["system"]))
        for slot in (1, 2):  # every metadata slot carries the new table
            self.assertEqual({x["name"] for x in lpdump(p.path("super"), slot)["partitions"]}, set(dump))

        self.assertEqual(p.bctl(), (b"_b", 0, [(14, 0, 1, 0), (15, 7, 0, 0)]))
        st = p.status()
        self.assertEqual(st["rom_b"], "18|1770000000|OtherROM 2.0")
        self.assertEqual(p.slot_fields("b")["data"], "twin")

    def test_reinstall_reuses_space(self):
        p = self.phone
        self.twin()
        for name in ("First 1", "Second 2", "Third 3"):
            zip_path, imgs = make_rom_zip(self.tmp, name=name, system_size=20 * MiB)
            p.twinslot("install", zip_path)
            self.assertEqual(p.status()["rom_b"].split("|")[2], name)
        self.assertEqual(self.logical("system_b"), imgs["system"])

    def test_bare_version_is_no_name(self):
        # EvoX sets ro.modversion=12.3: a number alone does not say which ROM it is.
        p = self.phone
        self.twin()
        zip_path, _ = make_rom_zip(self.tmp, name="12.3", system_size=20 * MiB)
        p.twinslot("install", zip_path)
        self.assertEqual(p.status()["rom_b"].split("|")[2], "")

    def test_no_switch_keeps_current(self):
        p = self.phone
        self.twin()
        zip_path, _ = make_rom_zip(self.tmp)
        r = p.twinslot("install", zip_path, "--no-switch")
        self.assertIn("not switched", r.stdout)
        self.assertEqual(p.bctl(), (b"_a", 0, [(15, 0, 1, 0), (14, 7, 0, 0)]))
        p.twinslot("switch", "b")

    def test_bootloader_on_request(self):
        p = self.phone
        self.twin()
        zip_path, imgs = make_rom_zip(self.tmp)
        p.twinslot("install", zip_path, "--bootloader")
        self.assertEqual(load(p.path("lk_b")), imgs["lk"])
        self.assertEqual(load(p.path("lk_a"))[:4], b"LK-a")

    def test_wipe(self):
        p = self.phone
        self.twin()
        zip_path, _ = make_rom_zip(self.tmp)
        r = p.twinslot("install", zip_path, "--wipe")
        self.assertIn("wiped twindata", r.stdout)
        self.assertIn("wiped twinmeta", r.stdout)

    def test_needs_dual_data(self):
        p = self.phone
        zip_path, _ = make_rom_zip(self.tmp)
        r = p.twinslot("install", zip_path, check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("setup", r.stderr)
        self.assertEqual(p.bctl()[0], b"_a")
        p.twinslot("install", zip_path, "--force")  # shared data: binds as primary
        self.assertEqual(p.slot_fields("b")["data"], "primary")

    def test_dry_run_writes_nothing(self):
        p = self.phone
        self.twin()
        before = {n: load(p.path(n)) for n in ("super", "misc", "vendor_boot_b", "boot_b")}
        zip_path, _ = make_rom_zip(self.tmp)
        r = p.twinslot("install", zip_path, "--dry-run")
        self.assertIn("add system_b", r.stdout)
        for n, data in before.items():
            self.assertEqual(load(p.path(n)), data, n)

    def test_refuses_pending_merge(self):
        p = self.phone
        self.twin()
        p.misc(make_bctl(merge=3))
        zip_path, _ = make_rom_zip(self.tmp)
        r = p.twinslot("install", zip_path, check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("merging", r.stderr)

    def test_running_slot_only_from_recovery(self):
        p = self.phone
        self.twin()
        zip_path, _ = make_rom_zip(self.tmp, name="Primary 3")
        r = p.twinslot("install", zip_path, "--slot", "a", check=False, android=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("recovery", r.stderr)
        p.twinslot("install", zip_path, "--slot", "a")  # from recovery
        self.assertEqual(p.slot_fields("a")["data"], "primary")
        self.assertEqual(p.status()["rom_a"].split("|")[2], "Primary 3")

    def test_reinstall_running_slot_keeps_the_other_rom(self):
        """In recovery on slot a, reinstalling a: an OTA into b rewrites only b's metadata copy, so a's copy
        may not know where b's ROM lies now. The install must plan from b's copy."""
        p = self.phone
        self.twin()
        meta, slots, base = 65536, 3, 3 * 4096
        offs = (base, base + slots * meta)  # slot a's copy: primary, backup

        def copy_a():
            with open(p.path("super"), "rb") as f:
                return [(f.seek(o), f.read(meta))[1] for o in offs]

        stale = copy_a()
        zip_b, imgs_b = make_rom_zip(self.tmp, name="OtherROM 2.0", system_size=40 * MiB)
        p.twinslot("install", zip_b, "--no-switch")
        with open(p.path("super"), "r+b") as f:
            for o, data in zip(offs, stale):
                f.seek(o)
                f.write(data)

        def extents(slot):
            text = subprocess.run(["lpdump", "--slot=%d" % slot, p.path("super")], capture_output=True, text=True,
                                  check=True).stdout
            return text.split("Name: system_b", 1)[1].split("Name:", 1)[0]

        self.assertNotEqual(extents(0), extents(1))  # a's copy is out of date about b
        zip_a, _ = make_rom_zip(self.tmp, name="Primary 3", system_size=60 * MiB)
        p.twinslot("install", zip_a, "--slot", "a")
        self.assertEqual(self.logical("system_b"), imgs_b["system"])
        self.assertEqual(p.status()["rom_b"].split("|")[2], "OtherROM 2.0")

    def test_large_zero_operation(self):
        p = self.phone
        self.twin()
        big = ("product", os.urandom(4096) + bytes(300 * MiB - 4096), [(pr.REPLACE, lambda d: d), (pr.ZERO, None)])
        zip_path, imgs = make_rom_zip(self.tmp, extra=[big], extra_dynamic=["product"], group_size=400 * MiB)
        p.twinslot("install", zip_path)
        self.assertEqual(self.logical("product_b"), imgs["product"])

    def test_corrupt_zip_leaves_slot_unbootable(self):
        p = self.phone
        self.twin()

        def flip(blob):
            blob[24 + manifest_len(blob) + 5000] ^= 0xFF

        zip_path, _ = make_rom_zip(self.tmp, corrupt=flip)
        r = p.twinslot("verify", zip_path, check=False)
        self.assertEqual(r.returncode, 4)
        self.assertIn("corrupt", r.stderr)
        r = p.twinslot("install", zip_path, check=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(p.bctl(), (b"_a", 0, [(15, 0, 1, 0), (0, 0, 0, 0)]))
        self.assertEqual(p.twinslot("switch", "b", check=False).returncode, 2)

    def test_refuses_incremental(self):
        p = self.phone
        self.twin()

        def incremental(blob):
            end = 24 + manifest_len(blob)
            self.assertEqual(bytes(blob[end - 2:end]), b"\x60\x00")  # minor_version = 0
            blob[end - 1] = 1

        zip_path, _ = make_rom_zip(self.tmp, corrupt=incremental)
        r = p.twinslot("install", zip_path, check=False)
        self.assertEqual(r.returncode, 4)
        self.assertIn("incremental", r.stderr)

    def test_super_too_small(self):
        p = self.phone
        self.twin()
        before = load(p.path("super"))
        big = ("product", bytes(600 * MiB), [(pr.ZERO, None)])
        zip_path, _ = make_rom_zip(self.tmp, extra=[big], extra_dynamic=["product"], group_size=700 * MiB)
        r = p.twinslot("install", zip_path, check=False)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("super cannot hold product_b", r.stderr)
        self.assertEqual(hashlib.sha256(load(p.path("super"))).digest(), hashlib.sha256(before).digest())
        self.assertEqual(p.bctl()[2][1], (0, 7, 0, 0))  # parked by setup, unchanged
