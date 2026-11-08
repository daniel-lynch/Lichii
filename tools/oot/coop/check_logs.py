#!/usr/bin/env python3
"""Check C3 emulator logs. Requires positive suppression evidence, not just silence."""
import argparse
from pathlib import Path
import re

GRANT = re.compile(r"OoT co-op: (observed|applied|suppressed) class=(bool|bit) "
                   r"key=([0-9a-f]{16}) value=1 path=([a-z-]+)(?: echo=(\S+))?")
FAILURES = ("quarantined", "queue full", "handshake refused", "relay rejected", "allocation failed",
            "sentinel refused")


def check(a, b):
    results = {}
    results["relay connected"] = all("OoT co-op: relay connected" in log for log in (a, b))
    results["baseline exchanged"] = all("OoT co-op: baseline exported" in log and
                                        "OoT co-op: baseline exchanged" in log for log in (a, b))
    grants_a, grants_b = list(GRANT.finditer(a)), list(GRANT.finditer(b))
    first = next((hit for hit in grants_a if hit[1] == "observed"), None)
    results["first grant observed on A"] = first is not None and sum(
        hit[1] == "observed" and (hit[2], hit[3]) == (first[2], first[3])
        for hit in grants_a) == 1
    applied = []
    suppressed = []
    echoed = []
    if first:
        matching = [hit for hit in grants_b if (hit[2], hit[3]) == (first[2], first[3])]
        applied = [hit for hit in matching if hit[1] == "applied" and hit[4] in ("setter", "data-or")]
        suppressed = [hit for hit in matching if hit[1] == "suppressed"]
        echoed = [hit for hit in matching if hit[1] == "observed"]
    results["applied on B"] = len(applied) == 1 and bool(applied[0][5])
    results["baseline before grant"] = bool(first) and results["applied on B"] and all(
        0 <= log.find("OoT co-op: baseline exported") <
        log.find("OoT co-op: baseline exchanged") < position
        for log, position in ((a, first.start()), (b, applied[0].start())))
    results["no echo"] = (results["applied on B"] and bool(suppressed) and not echoed and
                          all(hit[5] == applied[0][5] for hit in suppressed))
    results["no runtime failures"] = not any(
        f"OoT co-op: {failure}" in log or
        any(failure in line for line in log.splitlines() if "OoT co-op" in line)
        for log in (a, b) for failure in FAILURES)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    args = parser.parse_args()
    results = check(args.a.read_text(errors="replace"), args.b.read_text(errors="replace"))
    for label, passed in results.items():
        print(f"{'PASS' if passed else 'FAIL'}: {label}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
