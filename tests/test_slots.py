"""Reading and switching the A/B slot record in misc."""
import os

from lib import PhoneTest, load, make_bctl, save


class SlotTest(PhoneTest):
    def test_status(self):
        p = self.full_phone()
        st = p.status()
        self.assertEqual(st["current"], "a")
        self.assertEqual(st["next"], "a")
        self.assertEqual(st["merge_status"], "none")
        self.assertEqual(st["dual_data"], "off")
        self.assertEqual(p.slot_fields("b")["installed"], "yes")
        self.assertEqual(p.slot_fields("b")["data"], "primary")
        self.assertEqual(st["rom_b"], "17|1760000000|TestROM 1.0 (b)")

    def test_rom_name_from_product(self):
        p = self.full_phone(product_props="ro.axion.build.version=2.8\nro.axion.version=2.8-ONEIRA-UNOFFICIAL\n")
        self.assertEqual(p.status()["rom_a"], "17|1760000000|AxionOS 2.8")

    def test_switch_to_b_and_back(self):
        p = self.dual_phone()
        p.twinslot("switch", "b")
        self.assertEqual(p.bctl(), (b"_b", 0, [(14, 0, 1, 0), (15, 7, 0, 0)]))
        self.assertEqual(p.status()["next"], "b")
        p.twinslot("switch", "a")
        self.assertEqual(p.bctl(), (b"_a", 0, [(15, 7, 0, 0), (14, 7, 0, 0)]))

    def test_switch_other(self):
        p = self.dual_phone()
        p.twinslot("switch", "other")
        self.assertEqual(p.bctl()[0], b"_b")
        r = p.twinslot("switch", "other", check=False, current="")
        self.assertEqual(r.returncode, 1, r.stderr)

    def test_refuses_shared_data(self):
        """Without dual data the other ROM would start on this ROM's /data."""
        p = self.full_phone()
        r = p.twinslot("switch", "b", check=False)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("same /data", r.stderr)
        p.twinslot("switch", "a")  # the running slot: nothing shared changes
        p.twinslot("switch", "b", "--force")
        self.assertEqual(p.bctl()[0], b"_b")

    def test_suffix_without_underscore_kept(self):
        p = self.dual_phone()
        p.misc(make_bctl(suffix=b"a"))
        p.twinslot("switch", "b")
        self.assertEqual(p.bctl()[0], b"b")

    def test_refuses_empty_slot(self):
        p = self.full_phone(installed=("a",))
        r = p.twinslot("switch", "b", check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("no ROM is installed", r.stderr)
        self.assertEqual(p.bctl()[0], b"_a")
        p.twinslot("switch", "b", "--force")
        self.assertEqual(p.bctl()[0], b"_b")

    def test_refuses_pending_merge(self):
        p = self.dual_phone()
        for merge in (2, 3):
            p.misc(make_bctl(merge=merge))
            r = p.twinslot("switch", "b", check=False)
            self.assertEqual(r.returncode, 2)
            self.assertIn("merging", r.stderr)
        p.misc(make_bctl(merge=4))  # cancelled: nothing left to lose
        p.twinslot("switch", "b")

    def test_bad_crc_needs_force(self):
        p = self.full_phone()
        p.misc(make_bctl(bad_crc=True))
        self.assertEqual(p.twinslot("switch", "b", check=False).returncode, 2)
        self.assertIn("checksum", p.twinslot("status").stderr)
        p.twinslot("switch", "b", "--force")
        self.assertEqual(p.bctl()[0], b"_b")

    def test_bad_magic_refused_even_forced(self):
        p = self.full_phone()
        save(p.path("misc"), bytes(64 * 1024))
        self.assertNotEqual(p.twinslot("switch", "b", "--force", check=False).returncode, 0)
        self.assertEqual(load(p.path("misc")), bytes(64 * 1024))

    def test_unreadable_super_refuses(self):
        p = self.full_phone()
        os.unlink(p.path("super"))
        self.assertNotEqual(p.twinslot("switch", "b", check=False).returncode, 0)
        self.assertEqual(p.bctl()[0], b"_a")

    def test_parked_target_becomes_bootable(self):
        """Setup and remove park a slot (priority 0, tries left): its images are whole, so switch revives it."""
        p = self.dual_phone()
        p.misc(make_bctl(slots=((15, 0, 1), (0, 7, 0))))
        self.assertEqual(p.slot_fields("b")["bootable"], "no")
        p.twinslot("switch", "b")
        self.assertEqual(p.bctl()[2][1], (15, 7, 0, 0))

    def test_half_written_target_is_refused(self):
        """No tries left and never booted: an install or rebind that did not finish. It would hang at the logo."""
        p = self.full_phone()
        p.misc(make_bctl(slots=((15, 0, 1), (0, 0, 0))))
        before = p.bctl()
        r = p.twinslot("switch", "b", check=False)
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("did not finish", r.stderr)
        self.assertEqual(p.bctl(), before)
        p.twinslot("--force", "switch", "b")
        self.assertEqual(p.bctl()[2][1], (15, 7, 0, 0))

    def test_usage_errors(self):
        p = self.phone
        for args in ((), ("switch",), ("switch", "c"), ("bogus",), ("install",), ("--nope", "status")):
            self.assertEqual(p.twinslot(*args, check=False).returncode, 1, args)


class RecordElsewhereTest(PhoneTest):
    def test_record_in_para_is_named(self):
        """MediaTek's older boot control keeps the slots in para: TwinSlot says so and writes nothing."""
        p = self.phone
        p.misc(bytes(32))
        para = bytearray(64 * 1024)
        para[2048:2080] = make_bctl()
        save(p.path("para"), bytes(para))
        r = p.twinslot("switch", "b", check=False)
        self.assertEqual(r.returncode, 2)
        self.assertIn("para holds an A/B record", r.stderr)
        self.assertEqual(load(p.path("para"))[2048:2080], make_bctl())
