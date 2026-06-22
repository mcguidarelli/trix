#!/usr/bin/env python3
"""Regression gate for Trix collection behaviours corrected by the collections
doc audit (collections.md / record.md / string-processing.md).

Each probe below was a documentation defect the audit found and fixed. Locking it
as an executable probe means a future engine change that re-breaks the documented
behaviour fails CI, and the gate doubles as executable documentation of the
corrected facts. Companion to tools/check_scanner_grammar.py and
tools/check_type_facts.py.

Run: python3 tools/check_collection_facts.py [trix-binary]   (default ./trix.opt)
Exit 0 = all conform; 1 = mismatch (every offending case printed).
"""
import os
import subprocess
import sys
import tempfile


def run_line(binary, program):
    """Run a complete program verbatim; return (returncode, first stdout line)."""
    with tempfile.NamedTemporaryFile("w", suffix=".trx", delete=False) as fh:
        fh.write(program + "\n")
        path = fh.name
    try:
        r = subprocess.run([binary, path], capture_output=True, text=True)
    finally:
        os.unlink(path)
    line = r.stdout.splitlines()[0] if r.stdout.strip() else ""
    return r.returncode, line


# A 2-element-array unfold proc straight from docs/lazy-sequences.md 4.10.
_UNFOLD = ("1 { dup 10 gt { pop null } { dup 1 add mark 3 1 roll array-from-mark } "
           "if-else } lazy-unfold lazy-to-array length =")

# (program, expected `=` output)
OK = [
    # get-interval on a STRING returns an aliasing VIEW: a put through the
    # sub-string is visible in the source (string-processing.md 16.3/16.4).
    ("/o (hello world) def o 6 5 get-interval /s exch def s 0 88b put o =", "hello Xorld"),
    # lazy-seq is `array -- lazy` (ONE array operand), not head+thunk (collections.md 3.8).
    ("[1 2 3] lazy-seq lazy-to-array length =", "3"),
    # lazy-unfold is `seed proc -- lazy` (TWO operands), not seed+pred+emit (collections.md 3.8).
    (_UNFOLD, "10"),
    # No base64 operators exist; binary pack/unpack do (collections.md 3.3).
    ("/base64-encode where =", "false"),
    ("/base64-decode where =", "false"),
    ("/pack where =", "true"),
    ("/unpack where =", "true"),
    # bind verifies exactly ONE proc on top; an array below is left orphaned
    # (collections.md 3.2 Packed creation example).
    ("[1 2 3] { } bind count =", "2"),
    # The lazy `lazy-into` path CLAMPS xf-take to available elements
    # (transducers.md 9): take 10 of a 3-element lazy seq -> 3, no error.
    ("[1 2 3] lazy-seq 10 xf-take lazy-into lazy-to-array length =", "3"),
    # lazy-map forces the HEAD element eagerly at construction time
    # (lazy-sequences.md 5 intro): the proc runs once before consumption.
    ("/n 0 def 1 lazy-from { /n n 1 add def 10 mul } lazy-map pop n =", "1"),
]

# (program, expected process exit code)  -- error codes per src/enums.inl
ERR = [
    ("1 2 lazy-seq", 40),                          # lazy-seq on a non-array operand -> /type-check
    ("1 { dup 10 lt } { dup 1 add } lazy-unfold", 40),  # the old 3-operand lazy-unfold form -> /type-check
    ("[1 2 3] 10 xf-take into", 14),               # eager `into` xf-take N>length -> /index-check (lazy path clamps)
]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "trix.opt")
    if not os.path.exists(binary):
        binary = os.path.join(root, "trix")
    if not os.path.exists(binary):
        print("FAIL: no trix binary found (looked for ./trix.opt, ./trix)", file=sys.stderr)
        return 1

    fails = []
    n = 0
    for prog, exp in OK:
        n += 1
        rc, line = run_line(binary, prog)
        if rc != 0 or line != exp:
            got = f"exit={rc}" if rc != 0 else repr(line)
            fails.append(f"OK  {prog[:62]:62} expected {exp!r}, got {got}")
    for prog, code in ERR:
        n += 1
        rc, _ = run_line(binary, prog)
        if rc != code:
            fails.append(f"ERR {prog[:62]:62} expected exit {code}, got exit {rc}")

    if fails:
        print(f"check_collection_facts: {len(fails)} MISMATCH (of {n} probes)\n")
        for f in fails:
            print("  " + f)
        return 1
    print(f"check_collection_facts: {n} collection facts all conform")
    return 0


if __name__ == "__main__":
    sys.exit(main())
