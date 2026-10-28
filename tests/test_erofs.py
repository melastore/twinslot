"""Rebinding the fstab inside vendor's EROFS, across the layouts mkfs.erofs makes.

Each case builds vendor with mkfs.erofs, rebinds slot b, then extracts the
image with fsck.erofs (which checks every inode and the superblock checksum)
and compares every file.
"""
import os

from lib import MiB, PRIMARY_FSTAB, PhoneTest, erofs_extract, lp_extract, twin_text

LONG_FSTAB = "".join("# padding line %05d to spread the fstab over many clusters\n" % i
                     for i in range(600)) + PRIMARY_FSTAB + "# end\n" * 300
RANDOM_FSTAB = "# " + os.urandom(6000).hex() + "\n" + PRIMARY_FSTAB  # hex compresses badly in lz4
OTHER_FILES = {
    "etc/init/hw/init.rc": b"on init\n" * 500,
    "lib64/libbig.so": os.urandom(3 * MiB),
    "etc/compressible.xml": b"<feature name=\"x\"/>\n" * 50000,
    "etc/fstab.notdata": b"/dev/block/by-name/cache /cache ext4 defaults defaults\n",
}


CONTEXTS = "etc/selinux/vendor_file_contexts"
# The CK7n's vendor_file_contexts, cut down: the lines that label the data partitions.
PRIMARY_CONTEXTS = r"""/dev/block/platform/mtk-\b(msdc|ufs)\b\.0/[0-9]+\.\b(msdc0|ufs0)\b/by-name/userdata  u:object_r:userdata_block_device:s0
/dev/block/platform/mtk-\b(msdc|ufs)\b\.0/[0-9]+\.\b(msdc0|ufs0)\b/by-name/metadata  u:object_r:metadata_block_device:s0
/dev/block/by-name/md_udc u:object_r:metadata_block_device:s0
/dev/block/by-name/userdata                         u:object_r:userdata_block_device:s0
/dev/block/by-name/metadata                         u:object_r:metadata_block_device:s0
/dev/block/by-name/cache                            u:object_r:cache_block_device:s0
/data/vendor/userdata(/.*)?                         u:object_r:vendor_data_file:s0
/dev/block/bootdevice/by-name/userdata(/.*)?        u:object_r:userdata_block_device:s0
/dev/block/by-name/userdata_bak                     u:object_r:userdata_block_device:s0
"""


class ErofsTest(PhoneTest):
    def test_rebind_labels_the_twin_partitions(self):
        """vold can only open /data's device when SELinux labels it userdata_block_device."""
        p = self.full_phone(vendor_files={CONTEXTS: PRIMARY_CONTEXTS.encode()})
        p.twinslot("setup", "--size", "8G", "--apply")
        p.twinslot("rebind", "b")
        img = os.path.join(self.tmp, "vendor_b.img")
        with open(img, "wb") as f:
            f.write(lp_extract(p.path("super"), "vendor_b", self.tmp))
        twin = erofs_extract(img, self.tmp)[CONTEXTS].decode()
        self.assertIn("/dev/block/by-name/twindata                         u:object_r:userdata_block_device", twin)
        self.assertIn("/dev/block/by-name/twinmeta                         u:object_r:metadata_block_device", twin)
        self.assertIn("(msdc0|ufs0)\\b/by-name/twindata  u:object_r:userdata", twin)
        self.assertIn("/dev/block/by-name/md_udc u:", twin)
        self.assertIn("/data/vendor/userdata(/.*)?", twin)
        # A regex after the name still names the partition; a longer name is another partition.
        self.assertIn("/dev/block/bootdevice/by-name/twindata(/.*)?", twin)
        self.assertIn("/dev/block/by-name/userdata_bak ", twin)
        self.assertEqual(p.slot_fields("b")["data"], "twin")
        p.twinslot("rebind", "b", "primary", "--force")
        with open(img, "wb") as f:
            f.write(lp_extract(p.path("super"), "vendor_b", self.tmp))
        self.assertEqual(erofs_extract(img, self.tmp)[CONTEXTS].decode(), PRIMARY_CONTEXTS)

    def rebind(self, options, fstab=PRIMARY_FSTAB, expect_ok=True):
        p = self.full_phone(erofs_options=options, fstab=fstab, vendor_files=OTHER_FILES)
        p.twinslot("setup", "--size", "8G", "--apply")
        before = lp_extract(p.path("super"), "vendor_b", self.tmp)
        r = p.twinslot("rebind", "b", check=False)
        after = lp_extract(p.path("super"), "vendor_b", self.tmp)
        img = os.path.join(self.tmp, "vendor_b.img")
        with open(img, "wb") as f:
            f.write(after)
        if not expect_ok:
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(after, before, "a refused image must be left alone")
            return r
        self.assertEqual(r.returncode, 0, r.stderr)
        files = erofs_extract(img, self.tmp)
        self.assertEqual(files.pop("etc/fstab.mt6789").decode(), twin_text(fstab))
        expected = dict(OTHER_FILES)
        for name, data in expected.items():
            self.assertEqual(files.get(name), data, name)
        self.assertEqual(p.slot_fields("b")["data"], "twin")
        # And back again: the original text (recompressed bytes may differ).
        p.twinslot("rebind", "b", "primary", "--force")
        with open(img, "wb") as f:
            f.write(lp_extract(p.path("super"), "vendor_b", self.tmp))
        files = erofs_extract(img, self.tmp)
        self.assertEqual(files.pop("etc/fstab.mt6789").decode(), fstab)
        self.assertEqual(files, {**files, **expected})
        return r


def case(options, fstab=PRIMARY_FSTAB, expect_ok=True):
    return lambda self: self.rebind(options, fstab, expect_ok)


for _name, _opts in {
    "lz4hc": ["-zlz4hc"],
    "lz4": ["-zlz4"],
    "lz4hc_big_pcluster": ["-zlz4hc,12", "-C65536"],
    "lz4hc_legacy_index": ["-zlz4hc", "-Elegacy-compress"],
    "uncompressed": [],
    "uncompressed_noinline": ["-Enoinline_data"],
    # Blocks smaller than a page: the superblock checksum then covers a whole block from offset 1024.
    "lz4hc_512_blocks": ["-zlz4hc", "-b512", "-C4096"],
    "uncompressed_1k_blocks": ["-b1024"],
}.items():
    for _fname, _fstab in {"short": PRIMARY_FSTAB, "long": LONG_FSTAB, "random": RANDOM_FSTAB}.items():
        setattr(ErofsTest, "test_%s_%s" % (_name, _fname), case(_opts, _fstab))

for _name, _opts in {"ztailpacking": ["-zlz4hc", "-Eztailpacking"], "fragments": ["-zlz4hc", "-Eall-fragments"]}.items():
    setattr(ErofsTest, "test_refuses_%s" % _name, case(_opts, expect_ok=False))
# 512-byte pclusters: an edit that no longer compresses into one is refused, the image left alone.
ErofsTest.test_refuses_lz4hc_512_tight_long = case(["-zlz4hc", "-b512"], LONG_FSTAB, expect_ok=False)


class ErofsVoltageTest(PhoneTest):
    """The real VoltageOS 17 vendor image, when TWINSLOT_REAL_IMAGES points at it."""

    def test_voltage_vendor(self):
        d = os.environ.get("TWINSLOT_REAL_IMAGES")
        if not d or not os.path.exists(os.path.join(d, "vendor.img")):
            self.skipTest("set TWINSLOT_REAL_IMAGES to a dir with vendor.img and vendor_boot.img")
        from lib import make_super, vendor_boot_files
        import shutil
        p = self.phone
        p.add_disk()
        p.add_slot_images("a")
        shutil.copy(os.path.join(d, "vendor_boot.img"), p.path("vendor_boot_b"))
        p.add_slot_images("b")
        shutil.copy(os.path.join(d, "vendor_boot.img"), p.path("vendor_boot_b"))
        vendor = os.path.join(d, "vendor.img")
        make_super(p.path("super"), self.tmp, 1536 * MiB, [("main_a", 700 * MiB), ("main_b", 760 * MiB)],
                   [("system_a", "main_a", None, 0), ("system_b", "main_b", None, 4096),
                    ("vendor_b", "main_b", vendor, None)])
        original = erofs_extract(vendor, self.tmp)
        p.twinslot("setup", "--size", "8G", "--apply")
        r = p.twinslot("rebind", "b")
        img = os.path.join(self.tmp, "v.img")
        with open(img, "wb") as f:
            f.write(lp_extract(p.path("super"), "vendor_b", self.tmp))
        files = erofs_extract(img, self.tmp)
        changed = sorted(n for n in original if files[n] != original[n])
        self.assertTrue(changed and all(os.path.basename(n).startswith("fstab.") or n == CONTEXTS
                                        for n in changed), changed)
        for n in changed:
            self.assertEqual(files[n].decode(), twin_text(original[n].decode()))
        vb = vendor_boot_files(p.path("vendor_boot_b"), self.tmp)
        texts = [v.decode() for frag in vb.values() for k, v in frag.items() if "fstab" in k]
        self.assertTrue(any("/dev/block/by-name/twindata" in t for t in texts))
        self.assertFalse(any("/dev/block/by-name/userdata " in t for t in texts))
        print("\n" + r.stdout.strip())
