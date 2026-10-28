"""setup / remove / rebind / wipe: the twin data partitions and the fstabs that use them."""
import os
import subprocess

from lib import (avb_verify, GiB, MiB, PRIMARY_FSTAB, TWRP_FLAGS, PhoneTest, disk_tables, erofs_extract, gpt_parts, gpt_raw,
                 load, lp_extract, make_bctl, make_disk, make_super, twin_text, vendor_boot_files)


class DualDataTest(PhoneTest):
    def setup_twin(self, *extra):
        p = self.phone
        r = p.twinslot("setup", "--size", "8G", "--apply", *extra)
        self.assertIn("setup done", r.stdout)
        return r

    def vendor_fstab(self, slot):
        img = os.path.join(self.tmp, "v.img")
        with open(img, "wb") as f:
            f.write(lp_extract(self.phone.path("super"), "vendor_" + slot, self.tmp))
        return erofs_extract(img, self.tmp)

    def test_setup_dry_run_writes_nothing(self):
        p = self.full_phone()
        before = disk_tables(p.path("disk"))
        r = p.twinslot("setup", "--size", "8G")
        self.assertIn("dry run", r.stdout)
        self.assertEqual(disk_tables(p.path("disk")), before)

    def test_setup_layout(self):
        p = self.full_phone()
        old = gpt_parts(p.path("disk"))
        self.setup_twin()
        new = gpt_parts(p.path("disk"))  # sfdisk --verify passes inside
        sec = 4096
        ud, tm, td = new["userdata"], new["twinmeta"], new["twindata"]
        self.assertEqual(ud["start"], old["userdata"]["start"])
        self.assertEqual(ud["start"] + ud["size"], tm["start"])
        self.assertEqual(tm["start"] + tm["size"], td["start"])
        self.assertEqual(td["start"] + td["size"], old["userdata"]["start"] + old["userdata"]["size"])
        self.assertEqual(td["size"] * sec, 8 * GiB)
        self.assertEqual(tm["size"] * sec, 32 * MiB)  # as big as metadata
        self.assertEqual(td["start"] * sec % MiB, 0)
        self.assertEqual(tm["type"], new["metadata"]["type"])
        self.assertEqual(td["type"], new["userdata"]["type"])
        self.assertEqual(td["attrs"], "GUID:48")  # owner: slot b
        self.assertNotEqual(td["uuid"], old["userdata"]["uuid"])
        for name in ("frp", "metadata", "nvdata"):
            self.assertEqual(new[name], old[name])

        st = p.status()
        self.assertEqual((st["dual_data"], st["dual_owner"], st["twindata_mib"]), ("ready", "b", "8192"))
        # The owner slot still mounts userdata, so it must not boot until rebound or reinstalled.
        self.assertEqual(p.slot_fields("b")["bootable"], "no")
        self.assertEqual(p.slot_fields("b")["expected"], "twin")
        self.assertEqual(p.slot_fields("a")["bootable"], "yes")

    def test_setup_blanks_every_data_partition(self):
        p = self.full_phone()
        with open(p.path("disk"), "r+b") as f:
            f.seek(gpt_parts(p.path("disk"))["metadata"]["start"] * 4096)
            f.write(b"M" * 4096)
            f.seek(gpt_parts(p.path("disk"))["userdata"]["start"] * 4096 + 1024)
            f.write(b"\x10\x20\xf5\xf2" * 64)  # an f2fs superblock magic
        self.setup_twin()
        parts = gpt_parts(p.path("disk"))
        with open(p.path("disk"), "rb") as f:
            for name in ("metadata", "userdata", "twinmeta", "twindata"):
                f.seek(parts[name]["start"] * 4096)
                self.assertEqual(f.read(MiB), bytes(MiB), name)

    def test_setup_refusals(self):
        p = self.full_phone()
        r = p.twinslot("setup", "--size", "16G", "--apply", check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("cannot give", r.stderr)
        # In Android only: recovery runs the slot the zip was made on, which may own the split.
        r = p.twinslot("setup", "--owner", "a", check=False, android=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("running", r.stderr)
        p.misc(make_bctl(merge=2))
        r = p.twinslot("setup", "--size", "8G", "--apply", check=False)
        self.assertEqual(r.returncode, 2)
        self.assertNotIn("twindata", gpt_parts(p.path("disk")))
        p.misc(make_bctl())
        self.setup_twin()
        r = p.twinslot("setup", "--size", "8G", "--apply", check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("already", r.stderr)

    def test_setup_refused_while_android_runs(self):
        p = self.full_phone()
        r = p.twinslot("setup", "--size", "8G", "--apply", check=False, android=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("recovery", r.stderr)

    def test_default_size_is_half(self):
        p = self.full_phone()
        r = p.twinslot("setup")
        ud = gpt_parts(p.path("disk"))["userdata"]["size"] * 4096
        self.assertIn("twindata=%d MiB" % (ud // 2 // MiB), r.stdout)

    def test_512_byte_sectors(self):
        p = self.full_phone()
        make_disk(p.path("disk"), sector=512)
        self.setup_twin()
        parts = gpt_parts(p.path("disk"), sector=512)
        self.assertEqual(parts["twindata"]["size"] * 512, 8 * GiB)

    def test_remove_restores_userdata(self):
        p = self.full_phone()
        old = gpt_parts(p.path("disk"))
        self.setup_twin()
        p.twinslot("rebind", "b")
        p.twinslot("switch", "b")
        r = p.twinslot("remove", check=False)
        self.assertIn("dry run", r.stdout)
        p.twinslot("remove", "--apply")
        new = gpt_parts(p.path("disk"))
        self.assertEqual(set(new), set(old))
        self.assertEqual(new["userdata"]["size"], old["userdata"]["size"])
        self.assertEqual(p.status()["dual_data"], "off")
        # Slot b's ROM was bound to data that is gone: it must not boot.
        self.assertEqual(p.bctl()[0], b"_b")
        self.assertEqual(p.slot_fields("b")["bootable"], "no")
        self.assertEqual(p.status()["next"], "a")
        # ...and it points at userdata again, so it is a plain other slot, not a broken twin one.
        self.assertEqual(self.vendor_fstab("b")["etc/fstab.mt6789"].decode(), PRIMARY_FSTAB)
        self.assertEqual((p.slot_fields("b")["data"], p.slot_fields("b")["expected"]), ("primary", "primary"))

    def test_resize_in_one_go(self):
        # The app's resize zip: remove, then what the setup zip runs.
        p = self.full_phone()
        self.setup_twin()
        p.twinslot("rebind", "b")
        p.twinslot("remove", "--apply")
        r = p.twinslot("setup", "--size", "9G", "--owner", "b", "--apply")
        self.assertIn("setup done", r.stdout)
        # The zip's last lines: rebind the owner, let it boot again, stay on slot a.
        p.twinslot("rebind", "b")
        p.twinslot("switch", "b")
        p.twinslot("switch", "a")
        self.assertEqual(gpt_parts(p.path("disk"))["twindata"]["size"] * 4096, 9 * GiB)
        self.assertEqual(p.status()["next"], "a")
        self.assertEqual(p.slot_fields("b")["bootable"], "yes")
        self.assertEqual(p.slot_fields("a")["bootable"], "yes")
        self.assertEqual(self.vendor_fstab("b")["etc/fstab.mt6789"].decode(), twin_text(PRIMARY_FSTAB))
        self.assertEqual((p.slot_fields("b")["data"], p.slot_fields("b")["expected"]), ("twin", "twin"))

    def test_rebind_every_fstab_copy(self):
        p = self.full_phone()
        self.setup_twin()
        r = p.twinslot("rebind", "b")
        self.assertIn("6 fstab entries in vendor_boot, 2 in vendor", r.stdout)
        vb = vendor_boot_files(p.path("vendor_boot_b"), self.tmp)
        self.assertEqual(vb[0]["first_stage_ramdisk/system/etc/fstab.mt6789"].decode(), twin_text(PRIMARY_FSTAB))
        self.assertEqual(vb[1]["system/etc/recovery.fstab"].decode(), twin_text(PRIMARY_FSTAB))
        self.assertEqual(vb[1]["system/etc/twrp.flags"].decode(), twin_text(TWRP_FLAGS))
        self.assertEqual(len(vb[0]["first_stage_ramdisk/init"]), 300000)
        files = self.vendor_fstab("b")  # fsck.erofs checks the whole image
        self.assertEqual(files["etc/fstab.mt6789"].decode(), twin_text(PRIMARY_FSTAB))
        self.assertEqual(files["etc/init/hw/init.rc"], b"on init\n" * 500)
        self.assertIn("Flags:                    0", subprocess.run(
            ["avbtool", "info_image", "--image", p.path("vbmeta_b")], capture_output=True, text=True).stdout)
        st = p.slot_fields("b")
        self.assertEqual((st["data"], st["expected"]), ("twin", "twin"))
        # Slot a was not touched.
        self.assertEqual(self.vendor_fstab("a")["etc/fstab.mt6789"].decode(), PRIMARY_FSTAB)
        p.twinslot("switch", "b")

    def test_rebind_is_idempotent_and_reversible(self):
        p = self.full_phone()
        self.setup_twin()
        original = vendor_boot_files(p.path("vendor_boot_b"), self.tmp)
        p.twinslot("rebind", "b")
        once = load(p.path("vendor_boot_b"))
        self.assertIn("0 fstab entries in vendor_boot, 0 in vendor", p.twinslot("rebind", "b").stdout)
        self.assertEqual(load(p.path("vendor_boot_b")), once)
        self.assertEqual(p.twinslot("rebind", "b", "primary", check=False).returncode, 2)
        p.twinslot("rebind", "b", "primary", "--force")
        self.assertEqual(vendor_boot_files(p.path("vendor_boot_b"), self.tmp), original)
        self.assertEqual(self.vendor_fstab("b")["etc/fstab.mt6789"].decode(), PRIMARY_FSTAB)

    def test_rebind_refuses_running_slot(self):
        p = self.full_phone()
        self.setup_twin("--owner", "a", "--force")
        r = p.twinslot("rebind", "a", check=False, android=True)
        self.assertEqual(r.returncode, 2)

    def test_rebind_needs_fstab(self):
        p = self.full_phone()
        p.add_slot_images("b", fstab="# nothing here\n", flags="")
        self.setup_twin()
        r = p.twinslot("rebind", "b", check=False)
        self.assertEqual(r.returncode, 4)
        self.assertIn("no fstab", r.stderr)

    def test_wipe_uses_the_partition_table(self):
        p = self.full_phone()
        self.setup_twin()
        parts = gpt_parts(p.path("disk"))
        with open(p.path("disk"), "r+b") as f:
            for name in ("twindata", "twinmeta", "userdata"):
                f.seek(parts[name]["start"] * 4096)
                f.write(b"X" * 8192)
        r = p.twinslot("wipe", "b")
        self.assertIn("wiped twindata", r.stdout)
        with open(p.path("disk"), "rb") as f:
            for name, blank in (("twindata", True), ("twinmeta", True), ("userdata", False)):
                f.seek(parts[name]["start"] * 4096)
                self.assertEqual(f.read(8192) == bytes(8192), blank, name)


    def test_wipe_refused_without_the_split(self):
        """Without the split slot b's data is slot a's too."""
        p = self.full_phone()
        r = p.twinslot("wipe", "b", check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("dual data is off", r.stderr)

    def test_rebind_keeps_the_slot_state_or_switches_it_off(self):
        p = self.full_phone()
        self.setup_twin()
        p.twinslot("rebind", "b")
        p.twinslot("switch", "b")
        p.twinslot("switch", "a")
        before = p.bctl()[2][1]
        self.assertTrue(before[0] > 0)
        p.twinslot("rebind", "b", "primary", "--force")
        self.assertEqual(p.bctl()[2][1], before)
        # A rebind that fails part way must not leave a bootable half-edited slot.
        p.add_slot_images("b", fstab="# nothing here\n", flags="")
        self.assertEqual(p.twinslot("rebind", "b", check=False).returncode, 4)
        self.assertEqual(p.bctl()[2][1][:2], (0, 0))
        # A later rebind that succeeds leaves it off: an unfinished install looks just the same.
        p.add_slot_images("b")
        r = p.twinslot("rebind", "b")
        self.assertIn("--force rebind b", r.stderr)
        self.assertEqual(p.bctl()[2][1][:2], (0, 0))
        self.assertEqual(p.twinslot("switch", "b", check=False).returncode, 2)
        # --force says only the rebind was cut short: parked, so switch takes it.
        p.twinslot("rebind", "b", "--force")
        self.assertEqual(p.bctl()[2][1][:2], (0, 7))
        p.twinslot("switch", "b")

    def test_merge_blocks_rebind_and_wipe_only_of_the_merging_slot(self):
        """The merging update lives in the running slot: its vendor and its data (the COW)."""
        p = self.full_phone()
        self.setup_twin()
        p.twinslot("rebind", "b")
        p.misc(make_bctl(merge=2))
        for args in (("wipe", "a"), ("rebind", "a")):
            r = p.twinslot(*args, check=False)
            self.assertEqual(r.returncode, 2, args)
            self.assertIn("merging", r.stderr)
        p.twinslot("rebind", "b")
        self.assertIn("wiped", p.twinslot("wipe", "b").stdout)
        # Written into slot b but not booted yet: slot b holds the update, so it is part of it too.
        p.misc(make_bctl(slots=((14, 0, 1), (15, 0, 1)), merge=2))
        for args in (("wipe", "b"), ("rebind", "b")):
            r = p.twinslot(*args, check=False)
            self.assertEqual(r.returncode, 2, args)
            self.assertIn("slot b is part of it", r.stderr)
        self.assertIn("wiped", p.twinslot("--force", "wipe", "b").stdout)

    def test_stale_metadata_copy_is_refused(self):
        """After an OTA slot b's metadata copy can list extents that slot a's ROM now uses."""
        p = self.full_phone()
        self.setup_twin()
        # A second super whose slot b partitions come first, where this one keeps slot a's: its metadata, put
        # in slot b's place, is what an OTA leaves behind.
        parts = [(n + "_" + s, "main_" + s, os.path.join(self.tmp, "%s_%s.img" % (n, s)), None)
                 for s in ("b", "a") for n in ("system", "vendor")]
        other = os.path.join(self.tmp, "super2")
        make_super(other, self.tmp, 512 * MiB, [("main_a", 255 * MiB), ("main_b", 255 * MiB)], parts)
        meta, slots, base = 65536, 3, 3 * 4096
        with open(other, "rb") as src, open(p.path("super"), "r+b") as dst:
            for off in (base + meta, base + (slots + 1) * meta):
                src.seek(off)
                dst.seek(off)
                dst.write(src.read(meta))
        before = load(p.path("super"))
        r = p.twinslot("rebind", "b", check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("out of date", r.stderr)
        self.assertEqual(load(p.path("super")), before)
        self.assertEqual(p.slot_fields("b")["installed"], "unknown")
        self.assertEqual(p.slot_fields("a")["installed"], "yes")


class VerifiedBootTest(PhoneTest):
    """A rebound slot must still verify: the CK7n's bootloader rejects relaxed vbmeta flags."""

    def test_rebind_reseals_vbmeta(self):
        p = self.full_phone(avb=True)
        avb_verify(p, "b", self.tmp)
        p.twinslot("setup", "--size", "8G", "--apply")
        r = p.twinslot("rebind", "b")
        self.assertIn("2 vbmeta changes", r.stdout)  # vendor_boot's hash, vendor's hashtree
        info = avb_verify(p, "b", self.tmp)
        self.assertIn("Flags:                    0", info)
        self.assertIn("Public key (sha1):        cdbb77177f731920bbe0a0f94f84d9038ae0617d", info)
        self.assertIn("0 vbmeta changes", p.twinslot("rebind", "b").stdout)
        p.twinslot("rebind", "b", "primary", "--force")
        avb_verify(p, "b", self.tmp)
        avb_verify(p, "a", self.tmp)  # never touched

    def test_sha1_vendor_hashtree_is_rebuilt(self):
        """avbtool's default hashtree hash is SHA-1; each digest takes 32 bytes in the tree."""
        p = self.full_phone(avb={"hash_alg": "sha1"})
        avb_verify(p, "b", self.tmp)
        p.twinslot("setup", "--size", "8G", "--apply")
        self.assertIn("2 vbmeta changes", p.twinslot("rebind", "b").stdout)
        avb_verify(p, "b", self.tmp)
        info = subprocess.run(["avbtool", "info_image", "--image", p.path("vbmeta_vendor_b")], capture_output=True,
                              text=True, check=True).stdout
        self.assertRegex(info, r"Hash Algorithm:\s+sha1")

    def test_vendor_chained_footer_is_resealed(self):
        """BOARD_AVB_VENDOR_KEY_PATH: vbmeta chains to vendor, whose hashtree lives in vendor's own footer."""
        p = self.full_phone(avb={"vendor_chain": True})
        avb_verify(p, "b", self.tmp)
        p.twinslot("setup", "--size", "8G", "--apply")
        r = p.twinslot("rebind", "b")
        self.assertIn("2 vbmeta changes", r.stdout)  # vendor_boot's hash, vendor's hashtree in its footer
        self.assertNotIn("does not describe", r.stderr)
        avb_verify(p, "b", self.tmp)
        self.assertIn("0 vbmeta changes", p.twinslot("rebind", "b").stdout)

    def test_relaxed_vbmeta_is_sealed_again(self):
        """Slots rebound by twinslot 0.2 had verification turned off; a rebind turns it back on."""
        p = self.full_phone(avb=True)
        p.twinslot("setup", "--size", "8G", "--apply")
        p.twinslot("rebind", "b")
        with open(p.path("vbmeta_b"), "r+b") as f:
            f.seek(123)
            f.write(b"\x03")
        self.assertIn("1 vbmeta changes", p.twinslot("rebind", "b").stdout)
        self.assertIn("Flags:                    0", avb_verify(p, "b", self.tmp))


class MediaTekTableTest(PhoneTest):
    """A table like the CK7n's: every entry in use, MediaTek's own zero-type
    record right after them, no backup copy, userdata not the last partition."""

    def mtk_phone(self, **kw):
        p = self.full_phone()
        make_disk(p.path("disk"), mtk=True, **kw)
        return p

    def tail(self, p):
        with open(p.path("disk"), "rb") as f:
            f.seek(-8 * 4096, 2)
            return f.read()

    def test_setup_adopts_spare_partition(self):
        p = self.mtk_phone()
        hd0, raw0, old = gpt_raw(p.path("disk"))
        with open(p.path("disk"), "r+b") as f:
            f.seek(old["tranfs"]["start"] * 4096)
            f.write(b"T" * MiB)
        r = p.twinslot("setup", "--size", "8G", "--apply")
        self.assertIn("twinmeta=64 MiB, was tranfs", r.stdout)
        self.assertIn("no backup copy", r.stdout)
        hd, raw, new = gpt_raw(p.path("disk"))

        # One new entry, after MediaTek's record, which stays byte for byte.
        self.assertEqual(hd["num"], 8)
        self.assertEqual(new["twindata"]["index"], 7)
        self.assertEqual(raw[6 * 128:7 * 128], raw0[6 * 128:7 * 128])
        self.assertEqual(self.tail(p), bytes(8 * 4096))  # still no backup copy
        for k in ("my", "alt", "first", "last", "guid", "entries", "esize"):
            self.assertEqual(hd[k], hd0[k], k)
        for name in ("frp", "metadata", "nvdata", "flashinfo"):
            i = old[name]["index"]
            self.assertEqual(raw[i * 128:(i + 1) * 128], raw0[i * 128:(i + 1) * 128], name)

        # tranfs is now twinmeta, in place; its old name is kept after the new one.
        tm, ud, td = new["twinmeta"], new["userdata"], new["twindata"]
        self.assertEqual((tm["index"], tm["start"], tm["size"]), (3, old["tranfs"]["start"], old["tranfs"]["size"]))
        self.assertEqual(tm["raw_name"][:36].decode("utf-16-le"), "twinmeta\0tranfs\0\0\0")
        self.assertEqual(tm["attrs"], td["attrs"])
        self.assertEqual(td["attrs"], 1 << 48)
        self.assertEqual(ud["start"] + ud["size"], td["start"])
        self.assertEqual(td["start"] + td["size"], old["userdata"]["start"] + old["userdata"]["size"])
        self.assertEqual(td["size"] * 4096, 8 * GiB)
        self.assertEqual(new["flashinfo"]["start"], old["flashinfo"]["start"])
        with open(p.path("disk"), "rb") as f:
            f.seek(tm["start"] * 4096)
            self.assertEqual(f.read(MiB), bytes(MiB))

        st = p.status()
        self.assertEqual((st["dual_data"], st["twinmeta_mib"], st["twindata_mib"]), ("ready", "64", "8192"))

    def test_remove_gives_back_the_exact_table(self):
        for extra in ((), ("--meta-from", "none")):
            with self.subTest(extra=extra):
                p = self.mtk_phone()
                head0 = disk_tables(p.path("disk"))[:32 * 4096]
                p.twinslot("setup", "--size", "8G", "--apply", *extra)
                self.assertEqual(gpt_raw(p.path("disk"))[0]["num"], 8 if not extra else 9)
                p.twinslot("remove", "--apply")
                self.assertEqual(disk_tables(p.path("disk"))[:32 * 4096], head0)
                self.assertEqual(self.tail(p), bytes(8 * 4096))
                self.assertEqual(p.status()["dual_data"], "off")
                self.tearDown()
                self.setUp()

    def test_full_table_refused(self):
        layout = [("p%02d" % i, 1) for i in range(26)] + [("metadata", 32), ("tranfs", 64), ("userdata", None),
                                                          ("frp", 1), ("flashinfo", 16)]
        p = self.mtk_phone(layout=layout)  # 31 entries + MediaTek's record = the whole block
        before = disk_tables(p.path("disk"))
        r = p.twinslot("setup", "--size", "8G", "--apply", check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("no room for one more", r.stderr)
        self.assertEqual(disk_tables(p.path("disk")), before)

    def test_essential_partitions_never_adopted(self):
        p = self.mtk_phone()
        for name in ("metadata", "flashinfo", "frp", "nvdata", "boot_a", "missing"):
            r = p.twinslot("setup", "--size", "8G", "--meta-from", name, check=False)
            self.assertEqual(r.returncode, 2, name)
            self.assertIn("cannot become", r.stderr)
