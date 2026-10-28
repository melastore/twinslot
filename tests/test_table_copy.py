"""Copies of the partition table: saved before setup/remove write, and written back after a cut-off write."""
import glob
import os
import stat
import subprocess

from lib import BIN, GiB, PhoneTest, gpt_parts, gpt_raw, load, make_disk


def table_area(path, sector=4096, first_usable=None):
    """LBA 0 up to the first usable block, as the binary saves it."""
    with open(path, "rb") as f:
        if first_usable is None:
            f.seek(sector + 40)
            first_usable = int.from_bytes(f.read(8), "little")
        f.seek(0)
        return f.read(first_usable * sector)


def point_backup_entries_at_its_header(path, sector=4096):
    """A backup header with a valid CRC whose entry array starts one block before it: no room for 128 entries."""
    import struct
    import zlib
    nlba = os.path.getsize(path) // sector
    with open(path, "r+b") as f:
        f.seek((nlba - 1) * sector)
        h = bytearray(f.read(92))
        struct.pack_into("<Q", h, 72, nlba - 2)
        struct.pack_into("<I", h, 16, 0)
        struct.pack_into("<I", h, 16, zlib.crc32(h))
        f.seek((nlba - 1) * sector)
        f.write(h)
    return nlba


class TableCopyTest(PhoneTest):
    def copies(self, r):
        return [line.split("=", 1)[1] for line in r.stdout.splitlines() if line.startswith("table_copy=")]

    def test_setup_saves_the_table_before_writing(self):
        p = self.full_phone()
        before = table_area(p.path("disk"))
        extra = os.path.join(self.tmp, "sdcard")
        os.makedirs(extra)
        r = p.twinslot("--table-dir", extra, "setup", "--size", "8G", "--apply")
        copies = self.copies(r)
        self.assertEqual(len(copies), 2)  # --table-dir and the image directory
        self.assertTrue(copies[0].startswith(extra + "/twinslot-gpt-disk-"))
        for c in copies:
            self.assertEqual(load(c), before)
            self.assertEqual(stat.S_IMODE(os.stat(c).st_mode), 0o600)
        self.assertNotEqual(table_area(p.path("disk")), before)

    def test_remove_saves_the_table_too(self):
        p = self.full_phone()
        p.twinslot("setup", "--size", "8G", "--apply")
        split = table_area(p.path("disk"))
        r = p.twinslot("remove", "--apply")
        self.assertEqual([load(c) for c in self.copies(r)], [split])

    def test_no_backup_table_and_no_copy_is_refused(self):
        self.require("lpmake", "mkfs.erofs", "mkbootimg", "avbtool", "lz4")
        p = self.phone
        p.add_disk(mtk=True)
        for s in ("a", "b"):
            p.add_slot_images(s)
        p.add_super()
        before = table_area(p.path("disk"))
        os.chmod(p.dir, 0o555)  # no copy can be made next to the images
        try:
            r = p.twinslot("setup", "--size", "4G", "--apply", check=False)
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertIn("keeps no sound backup partition table", r.stderr)
            self.assertEqual(table_area(p.path("disk")), before)
            r = p.twinslot("setup", "--size", "4G", "--apply", "--force", check=False)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("no copy of it could be saved; going on (--force)", r.stderr)
        finally:
            os.chmod(p.dir, 0o755)

    def test_restore_after_a_cut_off_write(self):
        """A MediaTek disk (no backup table) whose primary header was lost mid-write comes back whole."""
        self.require("lpmake", "mkfs.erofs", "mkbootimg", "avbtool", "lz4")
        p = self.phone
        p.add_disk(mtk=True)
        for s in ("a", "b"):
            p.add_slot_images(s)
        p.add_super()
        before = table_area(p.path("disk"))
        copy = self.copies(p.twinslot("table-save", self.tmp))[0]
        with open(p.path("disk"), "r+b") as f:  # the header block, as a power cut between the writes leaves it
            f.seek(4096)
            f.write(bytes(4096))
        self.assertIn("no valid primary GPT", p.twinslot("status", check=False).stderr)

        r = p.twinslot("table-restore", copy)
        self.assertIn("restored", r.stdout)
        self.assertEqual(table_area(p.path("disk")), before)
        gpt_raw(p.path("disk"))  # both checksums hold
        self.assertEqual(p.status()["dual_data"], "off")

    def test_restore_rewrites_the_backup_table(self):
        p = self.full_phone()
        copy = self.copies(p.twinslot("table-save", self.tmp))[0]
        with open(p.path("disk"), "r+b") as f:
            f.seek(4096)
            f.write(bytes(4096))
        p.twinslot("table-restore", copy)
        gpt_parts(p.path("disk"))  # sfdisk --verify: primary and backup agree
        self.assertNotIn("backup GPT", p.twinslot("status").stderr)

    def test_restore_refusals(self):
        p = self.full_phone()
        copy = self.copies(p.twinslot("table-save", self.tmp))[0]
        before = table_area(p.path("disk"))

        r = p.twinslot("table-restore", copy, check=False)  # the table is intact: that is remove's job
        self.assertEqual(r.returncode, 2)
        self.assertIn("intact", r.stderr)

        other = os.path.join(self.tmp, "other-disk")
        make_disk(other)
        r = p.twinslot("--disk", other, "table-restore", copy, "--force", check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("another disk", r.stderr)

        small = os.path.join(self.tmp, "small-disk")
        make_disk(small, size=8 * GiB)
        with open(small, "r+b") as f:
            f.seek(4096)
            f.write(bytes(4096))
        r = p.twinslot("--disk", small, "table-restore", copy, check=False)
        self.assertEqual(r.returncode, 4)
        self.assertIn("another size", r.stderr)

        damaged = os.path.join(self.tmp, "damaged.bin")
        data = bytearray(load(copy))
        data[2 * 4096 + 60] ^= 1  # inside the entries
        with open(damaged, "wb") as f:
            f.write(data)
        with open(p.path("disk"), "r+b") as f:
            f.seek(4096)
            f.write(bytes(4096))
        r = p.twinslot("table-restore", damaged, check=False)
        self.assertEqual(r.returncode, 4)
        self.assertIn("damaged", r.stderr)

        r = p.twinslot("table-restore", copy, android=True, check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("from recovery", r.stderr)
        p.twinslot("table-restore", copy)
        self.assertEqual(table_area(p.path("disk")), before)

    def test_save_names_never_collide(self):
        p = self.full_phone()
        names = {self.copies(p.twinslot("table-save", self.tmp))[0] for _ in range(3)}
        self.assertEqual(len(names), 3)
        self.assertEqual(len(glob.glob(os.path.join(self.tmp, "twinslot-gpt-*.bin"))), 3)

    def test_failed_write_is_undone_at_once(self):
        """A table write that fails while the phone runs puts the old table back before it returns."""
        self.require("lpmake", "mkfs.erofs", "mkbootimg", "avbtool", "lz4")
        p = self.phone
        p.add_disk(mtk=True)
        for s in ("a", "b"):
            p.add_slot_images(s)
        p.add_super()
        before = table_area(p.path("disk"))
        env = dict(os.environ, TWINSLOT_TEST_CUT_TABLE="1")
        r = subprocess.run([BIN, "--dev-dir", p.dir, "--current", "a", "setup", "--size", "4G", "--apply"],
                           capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 3, r.stderr)
        self.assertIn("the old partition table is back", r.stderr)
        self.assertEqual(table_area(p.path("disk")), before)
        self.assertEqual(p.status()["dual_data"], "off")

    def test_backup_with_no_room_for_its_entries(self):
        """The array goes back where it fits before the backup header (it once overran the entries in memory)."""
        p = self.full_phone()
        nlba = point_backup_entries_at_its_header(p.path("disk"))
        self.assertIn("backup GPT is damaged", p.twinslot("status").stderr)
        p.twinslot("setup", "--size", "8G", "--apply")
        gpt_parts(p.path("disk"))  # sfdisk --verify: primary and backup agree
        with open(p.path("disk"), "rb") as f:
            f.seek((nlba - 1) * 4096 + 72)
            self.assertEqual(int.from_bytes(f.read(8), "little"), nlba - 1 - 128 * 128 // 4096)
        self.assertNotIn("backup GPT", p.twinslot("status").stderr)

    def test_damaged_backup_is_no_fallback(self):
        """A backup table that does not check out cannot replace a copy: setup still needs one."""
        p = self.full_phone()
        point_backup_entries_at_its_header(p.path("disk"))
        os.chmod(p.dir, 0o555)
        try:
            r = p.twinslot("setup", "--size", "8G", "--apply", check=False)
            self.assertEqual(r.returncode, 2, r.stderr)
            self.assertIn("keeps no sound backup partition table", r.stderr)
        finally:
            os.chmod(p.dir, 0o755)
        p.twinslot("setup", "--size", "8G", "--apply")
