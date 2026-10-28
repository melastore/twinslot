"""The recovery zip script, run by sh as a recovery would run it."""
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

from lib import BIN, ROOT, PhoneTest

sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkzip  # noqa: E402


class RecoveryZipTest(PhoneTest):
    def flash(self, *args, dual=False):
        """Builds a zip around the host binary and runs its update-binary."""
        p = self.dual_phone() if dual else self.full_phone()
        zip_path = os.path.join(self.tmp, "t.zip")
        script = mkzip.read(os.path.join(ROOT, "recovery", "update-binary"))
        with open(os.devnull, "w") as quiet:
            old, sys.stdout = sys.stdout, quiet
            try:
                mkzip.build(zip_path, ["--dev-dir", p.dir, "--current", "a", *args], mkzip.read(BIN), script)
            finally:
                sys.stdout = old
        out = os.path.join(self.tmp, "outfd")
        # A recovery calls: update-binary <api> <outfd> <zip>
        r = subprocess.run(["sh", "-c", 'sh "$0" 3 9 "$1" 9>"$2"', os.path.join(ROOT, "recovery", "update-binary"),
                            zip_path, out], capture_output=True, text=True)
        with open(out) as f:
            return r.returncode, f.read()

    def test_switch(self):
        rc, text = self.flash("switch", "b", dual=True)
        self.assertEqual(rc, 0, text)
        self.assertIn("ui_print   switched: next boot uses slot b", text)
        self.assertIn("ui_print TwinSlot: done", text)
        self.assertEqual(self.phone.bctl()[0], b"_b")

    def test_failure_is_reported(self):
        rc, text = self.flash("install", "/nonexistent.zip")
        self.assertEqual(rc, 1)
        self.assertIn("failed (exit", text)

    def test_progress_bar(self):
        rc, text = self.flash("setup", "--size", "8G", "--apply")
        self.assertEqual(rc, 0, text)
        self.assertIn("set_progress 0.100", text)
        self.assertIn("set_progress 1.000", text)
        self.assertNotIn("ui_print   progress", text)
        self.assertIn("ui_print   partition table saved: ", text)


class RecoveryTableCopyTest(PhoneTest):
    """setup/remove zips: the table copy next to the zip, and the copy the app puts inside it."""

    def flash_at(self, zip_dir, args, extra=None):
        """The zip's command is exactly args (as the app writes it); the tool it carries finds the fake phone."""
        tool = ("#!/bin/sh\nexec %s --dev-dir %s --current a \"$@\"\n"
                % (shlex.quote(BIN), shlex.quote(self.phone.dir))).encode()
        zip_path = os.path.join(zip_dir, "twinslot-setup.zip")
        script = mkzip.read(os.path.join(ROOT, "recovery", "update-binary"))
        with open(os.devnull, "w") as quiet:
            old, sys.stdout = sys.stdout, quiet
            try:
                mkzip.build(zip_path, args, tool, script, extra=extra)
            finally:
                sys.stdout = old
        out = os.path.join(self.tmp, "outfd")
        r = subprocess.run(["sh", "-c", 'sh "$0" 3 9 "$1" 9>"$2"', os.path.join(ROOT, "recovery", "update-binary"),
                            zip_path, out], capture_output=True, text=True)
        with open(out) as f:
            return r.returncode, f.read()

    def card_dir(self, name):
        """A folder outside /tmp, like an SD card's: update-binary saves no copy next to zips under /tmp."""
        base = tempfile.mkdtemp(prefix="card-", dir=os.path.join(ROOT, "out"))
        self.addCleanup(shutil.rmtree, base)
        d = os.path.join(base, name)
        os.makedirs(d)
        return d

    def test_folder_with_spaces(self):
        self.full_phone()
        d = self.card_dir("ROM backups")
        rc, text = self.flash_at(d, ["setup", "--size", "8G", "--apply"])
        self.assertEqual(rc, 0, text)
        copies = [f for f in os.listdir(d) if f.startswith("twinslot-gpt-")]
        self.assertEqual(len(copies), 1, text)
        self.assertIn("ui_print   partition table saved: %s/%s" % (d, copies[0]), text)

    def test_copy_inside_the_zip(self):
        from test_table_copy import table_area
        p = self.full_phone()
        before = table_area(p.path("disk"))
        rc, text = self.flash_at(self.tmp, ["setup", "--size", "8G", "--apply"], extra={"table.bin": before})
        self.assertEqual(rc, 0, text)
        self.assertIn("(table.bin) is the table as it is", text)
        # The same zip's copy after the table changed: it does not count.
        rc, text = self.flash_at(self.tmp, ["remove", "--apply"], extra={"table.bin": before})
        self.assertEqual(rc, 0, text)
        self.assertIn("does not match the table now", text)

    def test_resize_zip_on_a_disk_without_backup(self):
        """The app's resize zip: remove then setup. After remove, table.bin is from before the zip; it still counts."""
        self.require("lpmake", "mkfs.erofs", "mkbootimg", "avbtool", "lz4")
        from test_table_copy import table_area
        p = self.phone
        p.add_disk(mtk=True)
        for s in ("a", "b"):
            p.add_slot_images(s)
        p.add_super()
        p.twinslot("setup", "--size", "8G", "--owner", "b", "--apply")
        before = table_area(p.path("disk"))
        os.chmod(p.dir, 0o555)  # like the phone: no copy survives a power cut
        try:
            rc, text = self.flash_at(self.tmp, ["remove", "--apply", "+", "setup", "--size", "4G", "--owner", "b",
                                                "--apply"], extra={"table.bin": before})
            self.assertEqual(rc, 0, text)
            self.assertIn("(table.bin) is the table as it is", text)
            self.assertIn("(table.bin) is the table from before this zip", text)
            # Without the earlier step a stale copy is still refused.
            rc, text = self.flash_at(self.tmp, ["remove", "--apply"], extra={"table.bin": before})
            self.assertEqual(rc, 1, text)
            self.assertIn("refusing", text)
        finally:
            os.chmod(p.dir, 0o755)

    def test_setup_for_the_running_slot_in_recovery(self):
        """Recovery boots the slot the zip was made on; setup may give that slot the new data there."""
        self.full_phone()
        rc, text = self.flash_at(self.tmp, ["setup", "--size", "8G", "--owner", "a", "--apply"])
        self.assertEqual(rc, 0, text)
