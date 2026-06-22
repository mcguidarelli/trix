#!/usr/bin/env python3
"""Audit `if (cond) { return; } else { ... }` redundant-else control flow.

Finds the bare-return-else pattern across the source: an `if` whose then-branch
is *solely* a valueless `return;` (modulo comments) immediately followed by an
`else`.  Reports two-way (`} else {`) and chained (`} else if`) sites separately.
This is the noise the 2026-06 control-flow flattening sweep removed -- a leaf
guard inverts to `if (!cond) { ... }`, a cascade flattens to flat guard clauses,
and folds collapse to one predicate (see feedback_invert_single_guard /
feedback_explicit_control_flow in the agent memory).

It only matches a bare `return;` in the THEN-branch (first arm).  It does NOT
see: a return in a trailing `else if` arm, a *work-then-return* tail (use
find_tail_return.py for that), or `break`/valued-return branches (out of scope by
design).  Comment-line and trailing-comment returns ARE matched (the flag column
marks them -- a stricter `== "return;"` scan misses whole files otherwise).

Read every hit before editing -- this is a locator, not an auto-fixer.

Usage:
  ./tools/find_redundant_else.py                 # scan src/*.inl (default)
  ./tools/find_redundant_else.py src/gc.inl ...  # scan specific files
"""
import glob
import re
import sys

COMMENT = re.compile(r"^\s*(//.*)?$")
IF = re.compile(r"^(\s*)if(\s+constexpr)?\s*\((.*)\)\s*\{\s*(?://.*)?$")
RET = re.compile(r"^\s*return;\s*(?://.*)?$")
ELSEL = re.compile(r"^\s*\}\s*else\b")


def main(argv):
    files = sorted(argv[1:]) if len(argv) > 1 else sorted(glob.glob("src/*.inl"))
    two_way = []
    chained = []
    for f in files:
        lines = open(f).read().split("\n")
        for i, l in enumerate(lines):
            m = IF.match(l)
            if not m:
                continue
            j = i + 1
            while j < len(lines) and COMMENT.match(lines[j]):
                j += 1
            if j >= len(lines) or not RET.match(lines[j]):
                continue
            k = j + 1
            while k < len(lines) and COMMENT.match(lines[k]):
                k += 1
            if k >= len(lines) or not ELSEL.match(lines[k]):
                continue
            # flag = the return carried a trailing comment or a comment line sat
            # between the `if` and the `return` (the strict-scan blind spot).
            flagged = j > i + 1 or lines[j].strip() != "return;"
            rec = (f, i + 1, m.group(3)[:50], flagged)
            (chained if "else if" in lines[k] else two_way).append(rec)
    print("=== 2-way (else {) ===")
    for f, ln, cond, flagged in two_way:
        flag = "  <-- trailing-comment/comment-line" if flagged else ""
        print(f"{f}:{ln}  if({cond}){flag}")
    print(f"\n=== chained (else if) : {len(chained)} ===")
    for f, ln, cond, _ in chained:
        print(f"{f}:{ln}  if({cond})")
    print(f"\n2-way: {len(two_way)}   chained: {len(chained)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
