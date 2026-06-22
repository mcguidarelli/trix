#!/usr/bin/env python3
"""Regression gate for Trix type-system behaviours corrected by the type-system
doc audit (type-system.md / tagged-values.md / types-cheatsheet.md).

Each probe below was a documentation defect the audit found and fixed. Locking it
as an executable probe means a future engine change that re-breaks the documented
behaviour fails CI, and the gate doubles as executable documentation of the
corrected facts. Companion to tools/check_scanner_grammar.py.

Run: python3 tools/check_type_facts.py [trix-binary]   (default ./trix.opt)
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


_S1 = "/s1 3 string def s1 0 97b put s1 1 98b put s1 2 99b put"
_S2 = "/s2 3 string def s2 0 97b put s2 1 98b put s2 2 99b put"

# (program, expected `=` output)
OK = [
    (f"{_S1} {_S2} s1 s2 eq =", "true"),       # eq compares STRINGS by content (not identity/interning)
    ("[1 2 3] [1 2 3] eq =", "false"),         # eq compares ARRAYS by identity (distinct offsets)
    ("/add 42 override add =", "42"),          # override deliberately shadows a builtin into userdict
    ("<< /pi 3.14 >> begin /pi 0 def pi =", "0"),  # bare << >> is ReadWriteFixed (value reassign ok)
    ("[1b 2 3l] /long-type coerce 0 get type =", "long-type"),  # coerce widens container elements
    ("3.7 /integer-type cast =", "3"),         # cast is the value-preserving Real->Int op (truncates)
    ("5uq 3 promote type =", "int128-type"),   # promote: UInt128 + Integer -> Int128
    ("5uq 3l promote type =", "uint128-type"), # promote: UInt128 + Long -> UInt128
    ("3 5u promote type =", "long-type"),      # promote: Integer + UInteger -> Long
    ("[ /x /y ] record-type /Point exch def 1 2 Point length =", "2"),  # Record is length-bearing
    ("/is-opaquehandle where =", "false"),     # OpaqueHandle has NO is-* predicate
]

# (program, expected process exit code)  -- error codes per src/enums.inl
ERR = [
    ("systemdict /add 42 put", 32),            # writing systemdict directly -> /read-only
    ("<< /pi 3.14 >>#r begin /pi 0 def", 32),  # #r read-only dict -> /read-only on reassignment
    ("5uq 3 add", 40),                         # no implicit coercion: UInt128 + Integer -> /type-check
    ("1.8e308d =", 28),                        # Double literal above DBL_MAX -> /numerical-overflow (scan)
    ("1.7e308d 2.0d mul", 26),                 # Double runtime overflow -> /numerical-inf
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
        print(f"check_type_facts: {len(fails)} MISMATCH (of {n} probes)\n")
        for f in fails:
            print("  " + f)
        return 1
    print(f"check_type_facts: {n} type-system facts all conform")
    return 0


if __name__ == "__main__":
    sys.exit(main())
