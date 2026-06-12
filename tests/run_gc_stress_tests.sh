#!/bin/bash
# ====================================================================
# run_gc_stress_tests.sh -- GC-stress phase of the test battery
# ====================================================================
# Runs the tests/test_gc_stress_*.trx programs under the debug-only
# `vm-gc-stress` operator, which makes gvm_alloc fire a full
# vm_global_gc before EVERY global user allocation.  Inside ${...}
# (global alloc path) that turns every make_curry / make_lazy /
# make_clone in a builder's chain into a GC barrier, so any in-flight
# node held only in a C local across the next allocation -- a dropped
# GC root -- is collected right before the allocation that needs it.
#
# `vm-gc-stress` is gated behind TRIX_DEBUGGER, so it exists ONLY in the
# debug build (./trix), never in ./trix.opt.  run_all.sh therefore skips
# these tests (they would "fail" as undefined-operator on the optimized
# build); this runner is their dedicated home and insists on a build that
# actually has the operator.
#
# Binary: defaults to ./trix; override with TRIX_BIN (must be a
# TRIX_DEBUGGER build).  Exit code: 0 all passed, 1 a test failed,
# 2 the binary is missing or lacks vm-gc-stress (wrong build).

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TRIX="${TRIX_BIN:-${PROJECT_DIR}/trix}"

if [ ! -x "$TRIX" ]; then
    echo "FATAL: trix binary not found or not executable: $TRIX" >&2
    echo "       Run ./build.sh first (the debug build has vm-gc-stress)." >&2
    exit 2
fi

# Probe for the debug-only operator.  An undefined operator still exits 0
# (the error is reported but the load does not fault the process), so the
# exit code is useless here -- detect by the marker reaching stdout, which
# only happens when `vm-gc-stress` is a real operator.
PROBE=$(printf 'true vm-gc-stress false vm-gc-stress (gc-stress-probe-ok) =\n' | "$TRIX" /dev/stdin 2>/dev/null)
if [ "$PROBE" != "gc-stress-probe-ok" ]; then
    echo "FATAL: $TRIX does not support 'vm-gc-stress'." >&2
    echo "       It is gated behind TRIX_DEBUGGER -- use the debug build (./trix)," >&2
    echo "       not the optimized one.  Set TRIX_BIN to a debug build." >&2
    exit 2
fi

PASS=0
FAIL=0

echo "===================================================="
echo "  GC-stress tests ($TRIX)"
echo "===================================================="

shopt -s nullglob
TESTS=("$SCRIPT_DIR"/test_gc_stress_*.trx)
shopt -u nullglob

if [ "${#TESTS[@]}" -eq 0 ]; then
    echo "  (no tests/test_gc_stress_*.trx found)"
    exit 0
fi

for t in "${TESTS[@]}"; do
    name="$(basename "$t")"
    out=$("$TRIX" "$t" < /dev/null 2>&1)
    code=$?
    # A test passes only if it exits cleanly, prints its __test_ok__
    # sentinel, and shows no failure marker (rt-ok prints "FAILED: " on a
    # deep-eq mismatch; ASan/UBSan and internal-errors surface non-zero
    # exits or their own banners).  Mirrors run_all.sh's detection.
    if [ "$code" -eq 0 ] \
        && grep -q '__test_ok__' <<<"$out" \
        && ! grep -qE '^FAILED:|^FAIL #|FAILED:' <<<"$out"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name  (exit $code)"
        echo "$out" | tail -20 | sed 's/^/         /'
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "===================================================="
echo "  Results: $((PASS + FAIL)) tests, $PASS passed, $FAIL failed"
echo "===================================================="

if [ "$FAIL" -eq 0 ]; then
    echo "  ALL GC-STRESS TESTS PASSED"
    exit 0
else
    echo "  SOME GC-STRESS TESTS FAILED"
    exit 1
fi
