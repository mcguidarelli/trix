#!/bin/bash
# run_golden_tests.sh -- exact-stdout golden tests.
#
# For each tests/golden/<name>.trx, run it in a fresh default-config
# interpreter and diff combined stdout+stderr byte-for-byte against
# tests/golden/<name>.expected.  This is the harness for operators whose
# OUTPUT is the contract (=, ==, stack, print-stack, print-fmt) -- rt-ok
# string asserts cannot cover them.
#
# To bless a new/changed expectation after an INTENTIONAL output change:
#   ./trix tests/golden/<name>.trx < /dev/null > tests/golden/<name>.expected 2>&1
# and review the diff like source code -- the .expected files are the
# print-format specification pinned byte-for-byte.
#
# Each run is capped at 15s (a hang is a FAIL, not a stuck suite) and the
# interpreter exit code must be 0 unless tests/golden/<name>.exitcode
# holds a different expected value.  An optional tests/golden/<name>.args
# file carries whitespace-separated script arguments appended after the
# filename (script-side argv, read by command-line-args) -- used by the
# example-* wrappers whose target example needs flags (e.g. a fixed
# --start date or --no-timing) to stay byte-deterministic.
#
# Exit code: 0 if every golden file matches, 1 otherwise.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TRIX="${TRIX_BIN:-${PROJECT_DIR}/trix}"

if [ ! -x "$TRIX" ]; then
    echo "FATAL: trix binary not found or not executable: $TRIX" >&2
    exit 2
fi

PASS=0
FAIL=0

for trx in "$SCRIPT_DIR"/golden/*.trx; do
    [ -e "$trx" ] || continue
    name="$(basename "$trx" .trx)"
    expected="$SCRIPT_DIR/golden/$name.expected"
    if [ ! -f "$expected" ]; then
        echo "  FAIL  $name -- missing $name.expected"
        FAIL=$((FAIL + 1))
        continue
    fi
    # Invoke with a repo-relative path: error backtraces echo the filename
    # as given, and the .expected files pin that text -- an absolute path
    # would bake the checkout location into the goldens.
    extra_args=()
    if [ -f "$SCRIPT_DIR/golden/$name.args" ]; then
        read -r -a extra_args < "$SCRIPT_DIR/golden/$name.args"
    fi
    actual="$(cd "$PROJECT_DIR" && timeout 15 "$TRIX" "tests/golden/$name.trx" "${extra_args[@]}" < /dev/null 2>&1)"
    rc=$?
    want_rc=0
    [ -f "$SCRIPT_DIR/golden/$name.exitcode" ] && want_rc="$(cat "$SCRIPT_DIR/golden/$name.exitcode")"
    if [ "$rc" -eq 124 ]; then
        echo "  FAIL  $name -- TIMEOUT (hung; killed after 15s)"
        FAIL=$((FAIL + 1))
    elif [ "$rc" -ne "$want_rc" ]; then
        echo "  FAIL  $name -- exit code $rc (expected $want_rc)"
        FAIL=$((FAIL + 1))
    elif [ "$actual" = "$(cat "$expected")" ]; then
        echo "  ok    $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name -- output differs from $name.expected:"
        diff <(printf '%s\n' "$actual") "$expected" | sed 's/^/        /' | head -40
        FAIL=$((FAIL + 1))
    fi
done

echo "===================================="
echo "  Golden results: $((PASS + FAIL)) tests, ${PASS} passed, ${FAIL} failed"
echo "===================================="

[ "$FAIL" -eq 0 ] || exit 1
exit 0
