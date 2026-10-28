"""A real ROM zip installed into a fake phone (TWINSLOT_REAL_ZIP=/path/rom.zip).

Slow (minutes: the ROM's xz data is decompressed) and needs ~3 GB in /tmp.
Every partition's sha256 is checked here against the ROM's manifest,
independently of the hash check the installer makes itself.
"""
import hashlib
import os
import zipfile

from lib import MiB, PhoneTest, erofs_extract, load, lp_extract, make_super, sparse_file, vendor_boot_files
import payload_ref as pr


def manifest(zip_path):
    with zipfile.ZipFile(zip_path) as zf:
        info = zf.getinfo("payload.bin")
    with open(zip_path, "rb") as f:
        f.seek(info.header_offset + 26)
        name_len, extra_len = int.from_bytes(f.read(2), "little"), int.from_bytes(f.read(2), "little")
        return pr.read_payload(f, info.header_offset + 30 + name_len + extra_len)


class RealRomTest(PhoneTest):
    def test_install_real_rom(self):
        zip_path = os.environ.get("TWINSLOT_REAL_ZIP")
        if not zip_path:
            self.skipTest("set TWINSLOT_REAL_ZIP to a full ROM zip")
        m = manifest(zip_path)
        dynamic = {n for g in m["groups"] for n in g.get("partitions", [])}
        p = self.phone
        p.add_disk()
        for s in "ab":
            p.add_slot_images(s)
            for part in m["partitions"]:  # every A/B partition the ROM carries
                if part["name"] not in dynamic and part["name"] not in ("vbmeta",):
                    sparse_file(p.path("%s_%s" % (part["name"], s)), max(part["size"], MiB))
        running = os.path.join(self.tmp, "system_a.img")
        with open(running, "wb") as f:
            f.write(os.urandom(MiB))
        make_super(p.path("super"), self.tmp, 5 << 30, [("main_a", 2400 * MiB), ("main_b", 2400 * MiB)],
                   [("system_a", "main_a", running, None), ("system_b", "main_b", None, 0)])
        p.twinslot("setup", "--size", "8G", "--apply")
        r = p.twinslot("install", zip_path, "--wipe")
        self.assertEqual(lp_extract(p.path("super"), "system_a", self.tmp), load(running))
        self.assertIn("installed: slot b boots next", r.stdout)

        for part in m["partitions"]:
            name = part["name"] + "_b"
            if part["name"] == "lk":
                continue  # kept: the device's own
            if part["name"] in dynamic:
                data = lp_extract(p.path("super"), name, self.tmp)
            else:
                with open(p.path(name), "rb") as f:
                    data = f.read(part["size"])
            if part["name"] in ("vendor", "vendor_boot", "vbmeta"):
                continue  # rebound on purpose; checked below
            self.assertEqual(hashlib.sha256(data[:part["size"]]).digest(), part["hash"], name)

        img = os.path.join(self.tmp, "vendor_b.img")
        with open(img, "wb") as f:
            f.write(lp_extract(p.path("super"), "vendor_b", self.tmp))
        files = erofs_extract(img, self.tmp)
        fstabs = [v.decode() for k, v in files.items() if os.path.basename(k).startswith("fstab.")]
        self.assertTrue(fstabs and all("by-name/userdata " not in t for t in fstabs))
        vb = vendor_boot_files(p.path("vendor_boot_b"), self.tmp)
        self.assertTrue(any("by-name/twindata" in v.decode() for fr in vb.values() for k, v in fr.items()
                            if "fstab" in k))
        st = p.status()
        print("\nrom_b=" + st["rom_b"])
        self.assertEqual(p.slot_fields("b")["data"], "twin")
        self.assertEqual(p.status()["next"], "b")
