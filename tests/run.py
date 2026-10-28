#!/usr/bin/env python3
"""Runs every tests/test_*.py against out/twinslot-host (make host first).

TWINSLOT_REAL_ZIP=/path/rom.zip and TWINSLOT_REAL_IMAGES=/dir (vendor.img,
vendor_boot.img) add the tests on real ROM files.
"""
import os
import sys
import unittest

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
suite = unittest.defaultTestLoader.discover(here, pattern="test_*.py", top_level_dir=here)
result = unittest.TextTestRunner(verbosity=2 if "-v" in sys.argv else 1).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
