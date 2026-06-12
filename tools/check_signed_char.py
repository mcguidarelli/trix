#!/usr/bin/env python3
"""check_signed_char.py -- catch char<->vm_t (and any char<->unsigned) sign
conversions BEFORE they reach x86 CI.

Trix is a single-header library embedded into arbitrary HOST programs, so it
must compile cleanly under whatever `char` signedness the host's target uses:
SIGNED on x86-64, UNSIGNED on ARM/AArch64.  `vm_t` is `uint8_t` (always
unsigned), so every `char <-> vm_t` crossing is a signedness change on x86 --
clang's -Wsign-conversion flags it there.  On the ARM dev box `char` is already
unsigned, so that warning is structurally BLIND to the whole class and the leak
only surfaces as a red x86 CI run.

This gate makes the ARM box see exactly what x86 CI's clang sees: it does a
syntax-only clang compile with `-fsigned-char` so `char` becomes signed, then
`-Wsign-conversion -Werror` lights up any char<->unsigned crossing.  It must be
CLANG -- gcc's -Wsign-conversion does NOT flag char<->unsigned char, which is
why these leaks compiled clean under the gcc build.

It compiles with the MAXIMAL feature set (-DTRIX_DEBUGGER -DTRIX_HEAP_TRACKING)
so it also covers the debugger / heap-tracking code that the CMake CI build
leaves out (CMake defines only TRX_DEBUG) -- code an embedder may well enable.

Usage:
  check_signed_char.py            # run the gate (exit 1 on any finding)
  check_signed_char.py --check    # same; explicit alias for CI steps
  check_signed_char.py -v         # also echo the clang command

Compiler: $CLANGXX, else clang++-20, else clang++ (clang >= 20 -- the source
uses [[assume]]).
"""

import argparse
import os
import shutil
import subprocess
import sys

# Maximal feature set: TRX_DEBUG enables the debug-only ops, TRIX_DEBUGGER and
# TRIX_HEAP_TRACKING pull in the debugger / heap-tracking translation units that
# the CMake CI build (TRX_DEBUG only) never type-checks.
FEATURE_DEFINES = ["-DTRX_DEBUG", "-DTRIX_HEAP_TRACKING", "-DTRIX_DEBUGGER"]

# Only -Wsign-conversion: it is the one warning the native (unsigned-char) build
# cannot see for char<->vm_t.  Every other warning is already enforced by
# build.sh + CI, so keeping the net narrow means this gate can never go red for
# an unrelated clang-vs-gcc warning difference -- it is purely the signedness
# axis.
WARN_FLAGS = ["-Wsign-conversion", "-Werror"]


def find_clang() -> str | None:
    for cand in (os.environ.get("CLANGXX"), "clang++-20", "clang++"):
        if cand and shutil.which(cand):
            return cand
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="run the gate (default); exit 1 on any finding")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="echo the clang command before running it")
    args = parser.parse_args()

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    trix_cpp = os.path.join(repo_root, "trix.cpp")
    if not os.path.isfile(trix_cpp):
        print(f"check_signed_char: {trix_cpp} not found", file=sys.stderr)
        return 2

    clang = find_clang()
    if clang is None:
        print("check_signed_char: no clang++ found (set $CLANGXX, or install "
              "clang++-20 -- the source uses [[assume]], so clang >= 20)",
              file=sys.stderr)
        return 2

    cmd = [
        clang, "-std=c++23", "-fsyntax-only", "-fsigned-char",
        *FEATURE_DEFINES, *WARN_FLAGS, "-ferror-limit=0", "trix.cpp",
    ]
    if args.verbose:
        print("+ " + " ".join(cmd))

    result = subprocess.run(cmd, cwd=repo_root, capture_output=True, text=True)
    if result.returncode == 0:
        print("check_signed_char: OK -- no char<->unsigned sign conversions "
              f"under -fsigned-char ({clang})")
        return 0

    sys.stderr.write(result.stderr)
    n = result.stderr.count("-Wsign-conversion")
    print(f"\ncheck_signed_char: FAILED -- {n} sign-conversion finding(s) under "
          "-fsigned-char.\nFix in SOURCE with explicit static_cast<vm_t> / "
          "static_cast<char> at the char<->vm_t crossing (the house idiom); a\n"
          "single-header library cannot dictate the host's compiler flags.",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
