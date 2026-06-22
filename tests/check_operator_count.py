#!/usr/bin/env python3
# check_operator_count.py -- guard the operator counts cited in the docs
# against the actual dispatch table, so they cannot silently drift.
#
# The "headline" figure the docs advertise is the USER-FACING operator count of
# the SHIPPING (optimized) build: dispatch rows whose name is not `@`-prefixed
# (the `@`-names are internal operators), excluding the debug-only rows that an
# optimized build #ifdef's out.  This script derives that number straight from
# src/dispatch.inl and asserts every doc that quotes it agrees.
#
# Only HEADLINE phrasings are checked (e.g. "NNN operators spanning", a
# "| Operators | NNN |" table cell, "all NNN ops", "Total user-facing ops:
# NNN").  Other legitimate, different counts -- the @-prefix internal total
# ("159 operator names in systemdict") or an approximate category subset
# ("~170 operators are the I/O surface") -- are deliberately NOT matched.
#
# Exit 0 on agreement, 1 on any drift.  Wired into runtests.sh.

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DISPATCH = ROOT / "src" / "dispatch.inl"

DOC_FILES = [
    "README.md",
    "docs/trix-reference.md",
    "docs/trix-system.md",
    "docs/functional-programming.md",
    "docs/operator-cheatsheet.md",
]

ROW_RE = re.compile(r'SystemName::\w+,\s+\w+,\s+"([^"]*)"sv')

# Patterns that capture a HEADLINE operator count.  Each captured number must
# equal the derived headline.  Crafted to skip "operator names in systemdict"
# (the @-internal count) and "operators are the I/O surface" (a subset).
HEADLINE_RE = [
    re.compile(r"(\d{3,4})\s+operators\s+spanning"),
    re.compile(r"the\s+(\d{3,4})\s+built-in\s+operators"),
    re.compile(r"\ball\s+(\d{3,4})\s+operators\b"),
    re.compile(r"\ball\s+(\d{3,4})\s+ops\b"),
    re.compile(r"types,\s+(\d{3,4})\s+operators"),
    re.compile(r"(\d{3,4})\s+user-facing\s+operators"),
    re.compile(r"Total user-facing ops:\s*~?(\d{3,4})"),
    re.compile(r"\|\s*\*{0,2}Operators?\*{0,2}\s*\|\s*\*{0,2}(\d{3,4})"),
    re.compile(r"\*{0,2}Total\*{0,2}\s*\|[^\n|]*\|\s*\*{0,2}(\d{3,4})"),
]


def derive_headline():
    """Opt-build user-facing operator count, straight from sysoperator_rows()."""
    lines = DISPATCH.read_text().split("\n")
    start = next(i for i, ln in enumerate(lines) if "sysoperator_rows()" in ln)
    ifdef_depth = 0
    opt_user_facing = 0
    seen = False
    for ln in lines[start : start + 1400]:
        s = ln.strip()
        if s.startswith("#ifdef") or s.startswith("#if "):
            ifdef_depth += 1
            continue
        if s.startswith("#endif"):
            ifdef_depth = max(0, ifdef_depth - 1)
            continue
        if s.startswith("};") and ifdef_depth == 0 and seen:
            break
        m = ROW_RE.search(ln)
        if m:
            seen = True
            if ifdef_depth == 0 and not m.group(1).startswith("@"):
                opt_user_facing += 1
    return opt_user_facing


def main():
    headline = derive_headline()
    print(f"Derived headline (opt user-facing) operator count: {headline}")

    failures = []
    for rel in DOC_FILES:
        text = (ROOT / rel).read_text()
        hits = [int(m.group(1)) for rx in HEADLINE_RE for m in rx.finditer(text)]
        if not hits:
            failures.append(f"{rel}: no headline operator count found")
            continue
        for n in hits:
            if n != headline:
                failures.append(f"{rel}: headline count {n} != derived {headline}")

    if failures:
        print("\nOPERATOR-COUNT DRIFT:")
        for f in failures:
            print(f"  FAIL: {f}")
        print(f"\nFix: update the docs to {headline} (or re-derive if the table changed).")
        return 1
    print(f"OK: every doc's headline operator count == {headline}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
