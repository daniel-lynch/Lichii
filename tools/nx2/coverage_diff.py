#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execution coverage snapshots of a game's main module, and what code ran between two of them
(docs/nx2/exec-coverage.md).

Run Lichii with EDEN_NX2_COVERAGE=1 EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545, then:

  python3 -I tools/nx2/coverage_diff.py ctl reset            # start a window now
  python3 -I tools/nx2/coverage_diff.py snap before.json     # ... stand in front of the chest
  python3 -I tools/nx2/coverage_diff.py snap after.json      # ... after opening it
  python3 -I tools/nx2/coverage_diff.py diff before.json after.json --functions functions.tsv

`diff` lists the code hit in AFTER but not in BEFORE as Ghidra addresses (live address - module
base + --image-base, default 0x7100000000). --functions names them from a tab-separated
`address<TAB>name[<TAB>size]` file exported from Ghidra; the file is only read, and no game data
belongs in this repository. Standard library only.
"""

import argparse
import base64
import bisect
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SERVER = "127.0.0.1:6545"
GHIDRA_BASE = 0x7100000000


# ------------------------------------------------------------------ snapshots
def load_snapshot(path):
    with open(path) as file:
        return json.load(file)


def snapshot_ranges(snapshot):
    """[(start, end)] byte offsets from the module base, sorted and merged."""
    encoding = snapshot.get("encoding", "ranges")
    if encoding == "ranges":
        flat = snapshot["ranges"]
        if len(flat) % 2:
            raise ValueError("odd-length ranges list")
        pairs = [(flat[i], flat[i] + flat[i + 1]) for i in range(0, len(flat), 2)]
    elif encoding == "bitmap":
        granule = int(snapshot.get("granule", 4))
        bits = base64.b64decode(snapshot["bitmap_base64"])
        pairs = []
        for index, byte in enumerate(bits):
            while byte:
                low = byte & -byte
                slot = index * 8 + low.bit_length() - 1
                pairs.append((slot * granule, (slot + 1) * granule))
                byte ^= low
    else:
        raise ValueError(f"unknown snapshot encoding {encoding!r}")
    return merge(pairs)


def merge(pairs):
    out = []
    for start, end in sorted(pairs):
        if end <= start:
            continue
        if out and start <= out[-1][1]:
            if end > out[-1][1]:
                out[-1] = (out[-1][0], end)
        else:
            out.append((start, end))
    return out


def subtract(new, base):
    """Parts of the sorted, merged intervals `new` not covered by `base`."""
    out = []
    j = 0
    for start, end in new:
        cursor = start
        while j < len(base) and base[j][1] <= cursor:
            j += 1
        k = j
        while k < len(base) and base[k][0] < end:
            if base[k][0] > cursor:
                out.append((cursor, base[k][0]))
            cursor = max(cursor, base[k][1])
            if cursor >= end:
                break
            k += 1
        if cursor < end:
            out.append((cursor, end))
    return out


def parse_base(value):
    return int(value, 16) if isinstance(value, str) else int(value)


# ------------------------------------------------------------------ Ghidra function names
class Functions:
    """Ghidra function starts (address<TAB>name[<TAB>size]), lines that do not parse skipped."""

    def __init__(self, rows=()):
        rows = sorted(rows)
        self.starts = [row[0] for row in rows]
        self.names = [row[1] for row in rows]
        self.sizes = [row[2] for row in rows]

    @classmethod
    def load(cls, path):
        rows = []
        with open(path, encoding="utf-8", errors="replace") as file:
            for line in file:
                fields = line.rstrip("\r\n").split("\t")
                if len(fields) < 2:
                    continue
                try:
                    address = int(fields[0].strip(), 16)
                except ValueError:
                    continue  # a header or comment line
                size = None
                if len(fields) > 2 and fields[2].strip():
                    try:
                        size = int(fields[2].strip(), 0)
                    except ValueError:
                        size = None
                rows.append((address, fields[1].strip(), size))
        return cls(rows)

    def __len__(self):
        return len(self.starts)

    def containing(self, address):
        """(index, name+offset) of the function containing address, or (None, None)."""
        i = bisect.bisect_right(self.starts, address) - 1
        if i < 0:
            return None, None
        size = self.sizes[i]
        if size is not None and address >= self.starts[i] + size:
            return None, None
        offset = address - self.starts[i]
        return i, self.names[i] + (f"+{offset:#x}" if offset else "")

    def touched(self, start, end):
        """Indexes of the functions overlapping [start, end), in address order."""
        out = []
        first, _ = self.containing(start)
        if first is not None:
            out.append(first)
        i = bisect.bisect_right(self.starts, start)
        while i < len(self.starts) and self.starts[i] < end:
            out.append(i)
            i += 1
        return out


# ------------------------------------------------------------------ report
def diff_report(base_snap, new_snap, functions=None, image_base=GHIDRA_BASE, limit=0,
                base_name="base", new_name="new"):
    base_ranges = snapshot_ranges(base_snap)
    new_ranges = snapshot_ranges(new_snap)
    fresh = subtract(new_ranges, base_ranges)
    live_base = parse_base(new_snap["base"])
    total = sum(end - start for start, end in fresh)
    lines = [
        f"# coverage diff: {base_name} ({len(base_ranges)} ranges) -> {new_name} "
        f"({len(new_ranges)} ranges)",
        f"# module main: live base {live_base:#x} (base snapshot {parse_base(base_snap['base']):#x}), "
        f"Ghidra image base {image_base:#x}",
        f"# {len(fresh)} newly hit ranges, {total} bytes"
        + (f", resolved against {len(functions)} functions" if functions else ""),
        "#",
        "# ghidra_start\tghidra_end\tbytes\tlive_start\tfunction",
    ]
    seen = {}
    shown = fresh if not limit else fresh[:limit]
    for start, end in fresh:
        if functions:
            for index in functions.touched(image_base + start, image_base + end):
                seen.setdefault(index, image_base + start)
    for start, end in shown:
        name = ""
        if functions:
            index, name = functions.containing(image_base + start)
            name = name or "?"
            more = len(functions.touched(image_base + start, image_base + end)) - (index is not None)
            if more > 0:
                name += f" (+{more} more)"
        lines.append(f"{image_base + start:#x}\t{image_base + end:#x}\t{end - start}\t"
                     f"{live_base + start:#x}\t{name}".rstrip())
    if limit and len(fresh) > limit:
        lines.append(f"# ... {len(fresh) - limit} more ranges (--limit)")
    if functions:
        lines.append("#")
        lines.append(f"# {len(seen)} functions newly touched, in address order:")
        for index in sorted(seen):
            lines.append(f"{functions.starts[index]:#x}\t{functions.names[index]}")
    return "\n".join(lines) + "\n", fresh


# ------------------------------------------------------------------ emulator side
def emulator(server):
    sys.path.insert(0, HERE)  # -I drops the script dir; debug_mcp.py is a sibling
    import debug_mcp  # noqa: E402

    return debug_mcp.Emulator(server), debug_mcp.EmulatorError


def take_snapshot(server, path, extents=True, encoding="ranges", timeout=60):
    em, error = emulator(server)
    try:
        result = em.call("coverage_snapshot", {"extents": extents, "encoding": encoding}, timeout)
    except error as failure:
        raise SystemExit(f"coverage_snapshot: {failure}") from failure
    finally:
        em.close()
    with open(path, "w") as file:
        json.dump(result, file)
    return result


def summary(result):
    return {k: v for k, v in result.items() if k not in ("ranges", "bitmap_base64")}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default=os.environ.get("LICHII_DEBUG_SERVER", DEFAULT_SERVER),
                    help=f"host:port of EDEN_NX2_DEBUG_SERVER (default {DEFAULT_SERVER})")
    sub = ap.add_subparsers(dest="command", required=True)
    s = sub.add_parser("snap", help="save a coverage_snapshot to a JSON file")
    s.add_argument("out")
    s.add_argument("--starts-only", action="store_true", help="block starts, not whole blocks")
    s.add_argument("--bitmap", action="store_true", help="raw bitmap (base64) instead of ranges")
    c = sub.add_parser("ctl", help="coverage_start / coverage_reset / coverage_stop")
    c.add_argument("action", choices=("start", "reset", "stop"))
    d = sub.add_parser("diff", help="code hit in NEW but not in BASE")
    d.add_argument("base")
    d.add_argument("new")
    d.add_argument("--functions", metavar="TSV", help="address<TAB>name[<TAB>size] from Ghidra")
    d.add_argument("--image-base", default=hex(GHIDRA_BASE), help="Ghidra image base (default %(default)s)")
    d.add_argument("--limit", type=int, default=0, help="list at most N ranges (0 = all)")
    d.add_argument("-o", "--out", help="write the report here instead of stdout")
    a = ap.parse_args(argv)

    if a.command == "snap":
        result = take_snapshot(a.server, a.out, extents=not a.starts_only,
                               encoding="bitmap" if a.bitmap else "ranges")
        print(json.dumps(summary(result)))
        return 0
    if a.command == "ctl":
        em, error = emulator(a.server)
        try:
            print(json.dumps(em.call(f"coverage_{a.action}", {}, 60)))
        except error as failure:
            print(f"coverage_{a.action}: {failure}", file=sys.stderr)
            return 1
        return 0
    functions = Functions.load(a.functions) if a.functions else None
    report, _ = diff_report(load_snapshot(a.base), load_snapshot(a.new), functions,
                            int(a.image_base, 16), a.limit, os.path.basename(a.base),
                            os.path.basename(a.new))
    if a.out:
        with open(a.out, "w") as file:
            file.write(report)
    else:
        sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
