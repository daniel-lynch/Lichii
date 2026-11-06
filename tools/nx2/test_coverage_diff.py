#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import base64
import json
import os
import subprocess
import sys
import tempfile
import unittest

import coverage_diff as cd

HERE = os.path.dirname(os.path.abspath(__file__))


def snap(base, pairs):
    flat = []
    for offset, length in pairs:
        flat += [offset, length]
    return {"module": "main", "base": hex(base), "granule": 4, "encoding": "ranges", "ranges": flat}


class Intervals(unittest.TestCase):
    def test_merge(self):
        self.assertEqual(cd.merge([(8, 12), (0, 4), (4, 8), (20, 24), (22, 30), (40, 40)]),
                         [(0, 12), (20, 30)])

    def test_subtract(self):
        new = [(0, 100), (200, 220), (300, 304)]
        base = [(10, 20), (30, 40), (90, 210), (300, 304)]
        self.assertEqual(cd.subtract(new, base), [(0, 10), (20, 30), (40, 90), (210, 220)])
        self.assertEqual(cd.subtract(new, []), new)
        self.assertEqual(cd.subtract([], base), [])

    def test_bitmap_snapshot(self):
        # bits 0 and 9: offsets 0 and 36
        bits = bytes([0x01, 0x02])
        s = {"base": "0x1000", "granule": 4, "encoding": "bitmap",
             "bitmap_base64": base64.b64encode(bits).decode()}
        self.assertEqual(cd.snapshot_ranges(s), [(0, 4), (36, 40)])


class Report(unittest.TestCase):
    def setUp(self):
        self.functions = cd.Functions([
            (0x7100000100, "FUN_7100000100", None),
            (0x7100000200, "Chest_Open", 0x40),
            (0x7100000300, "Item_Get", None),
        ])

    def test_containing(self):
        self.assertEqual(self.functions.containing(0x7100000104)[1], "FUN_7100000100+0x4")
        self.assertEqual(self.functions.containing(0x7100000200)[1], "Chest_Open")
        self.assertEqual(self.functions.containing(0x7100000250), (None, None))  # past its size
        self.assertEqual(self.functions.containing(0x7100000000), (None, None))
        self.assertEqual(self.functions.touched(0x71000001f0, 0x7100000310), [0, 1, 2])

    def test_diff_uses_ghidra_addresses(self):
        # Two boots at different ASLR bases: offsets are what count.
        base = snap(0x8000_0000, [(0x100, 0x10)])
        new = snap(0x9000_0000, [(0x100, 0x20), (0x200, 8), (0x2f0, 0x20)])
        text, fresh = cd.diff_report(base, new, self.functions)
        self.assertEqual(fresh, [(0x110, 0x120), (0x200, 0x208), (0x2f0, 0x310)])
        lines = [l for l in text.splitlines() if not l.startswith("#")]
        self.assertEqual(lines[0], "0x7100000110\t0x7100000120\t16\t0x90000110\tFUN_7100000100+0x10")
        self.assertEqual(lines[1], "0x7100000200\t0x7100000208\t8\t0x90000200\tChest_Open")
        self.assertEqual(lines[2], "0x71000002f0\t0x7100000310\t32\t0x900002f0\t? (+1 more)")
        self.assertIn("# 3 functions newly touched, in address order:", text)
        self.assertEqual(lines[-1], "0x7100000300\tItem_Get")

    def test_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b, tsv, out = (os.path.join(tmp, n) for n in ("a.json", "b.json", "f.tsv", "o.txt"))
            with open(a, "w") as f:
                json.dump(snap(0x1000, []), f)
            with open(b, "w") as f:
                json.dump(snap(0x1000, [(0x200, 4)]), f)
            with open(tsv, "w") as f:
                f.write("address\tname\n7100000200\tChest_Open\n")
            done = subprocess.run([sys.executable, "-I", os.path.join(HERE, "coverage_diff.py"), "diff",
                                   a, b, "--functions", tsv, "-o", out], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            with open(out) as f:
                self.assertIn("0x7100000200\t0x7100000204\t4\t0x1200\tChest_Open\n", f.read())


if __name__ == "__main__":
    unittest.main()
