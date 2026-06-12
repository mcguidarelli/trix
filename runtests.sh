#!/bin/bash
# Run the full Trix test suite with required configuration flags.
# Each test step validates its output -- a crash or missing success marker is a FAIL.
set -euo pipefail

FAIL=0
ERRORS=""

# --- Step 1: test_all.trx (master test runner) ---
# 2 MB VM: the debug binary's baseline is ~495 KB (heap-tracking tables +
# debugger substrate) and the unwrapped concurrency tail legitimately
# retains pool memory across tests (mailbox/pipe/coroutine free-lists are
# kept for reuse, not collected).  --userdict-size=1024: the unwrapped
# tail's defs all land in one shared userdict (no per-test restore), and
# the cumulative count exceeds the 512 default.  Per-test budgets are
# enforced by tests/run_all.sh (fresh default-config VM per test;
# documented SPECIAL_ARGS exceptions), not here -- this step's job is
# cross-test interaction.
echo "===================================="
echo "  Running tests/test_all.trx"
echo "===================================="

OUTPUT=$(./trix --vm-size=2M --userdict-size=1024 tests/test_all.trx 2>&1)
echo "$OUTPUT"

# Extract results line for final summary
STX_RESULTS=$(grep -m1 "tests," <<< "$OUTPUT" | sed 's/^ *//' || echo "?? tests, ?? failures")

# Validate: the master runner summary must report "0 failures"
if ! grep -q "0 failures" <<< "$OUTPUT"; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}  FAIL: tests/test_all.trx did not report 0 failures\n"
fi

# Validate: no individual rt-ok assertion failed inside any test file.
# test_all.trx's fail-count only increments on file-level crashes, so an
# rt-ok that prints "FAILED: ..." (or a non-zero "FAILURES = N" summary)
# without crashing the file silently passes the top-level summary.  Catch
# those here -- a test must not be relied upon to self-abort on failure.
FAIL_RE="^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]"
RTOK_FAILS=$(grep -cE "$FAIL_RE" <<< "$OUTPUT" || true)
if [ "$RTOK_FAILS" -gt 0 ]; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}  FAIL: ${RTOK_FAILS} rt-ok assertion(s) failed inside test files:\n"
    while IFS= read -r line; do
        ERRORS="${ERRORS}    ${line}\n"
    done < <(grep -E "$FAIL_RE" <<< "$OUTPUT")
fi

# Validate: no fatal Trix crash ("Trix exiting" only appears on unrecoverable errors;
# other "Trix <error>" lines may be expected output from error-handling tests)
if grep -q "^Trix exiting" <<< "$OUTPUT"; then
    FAIL=$((FAIL + 1))
    ERRLINE=$(grep -m1 -B1 "^Trix exiting" <<< "$OUTPUT" | head -1)
    ERRORS="${ERRORS}  FAIL: tests/test_all.trx crashed: ${ERRLINE}\n"
fi

echo ""

# --- Step 1b: standalone per-test sweep (tests/run_all.sh) ---
# Complements step 1: every tests/test_*.trx in its own interpreter with
# the per-test flags it requires (SPECIAL_ARGS / SPECIAL_ENV), including
# the standalone-only tests that cannot run inside test_all.trx (see the
# delist comment there).  Catches dict-state leaks and enforces the
# per-test 1 MB default budget.
echo ""
echo "===================================="
echo "  Running tests/run_all.sh (standalone per-test)"
echo "===================================="

set +e
RUNALL_OUTPUT=$(tests/run_all.sh 2>&1)
RUNALL_EXIT=$?
set -e
# The per-test runner is verbose; show failures and the trailing summary.
grep -E "^  (FAIL|SKIP)" <<< "$RUNALL_OUTPUT" || true
tail -3 <<< "$RUNALL_OUTPUT"

if [ "$RUNALL_EXIT" -ne 0 ]; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}  FAIL: tests/run_all.sh failed (exit ${RUNALL_EXIT})\n"
    RUNALL_RESULTS="FAILED (exit ${RUNALL_EXIT})"
else
    RUNALL_RESULTS="all passed"
fi

# --- Step 1c: golden-stdout tests (exact print-format pinning) ---
echo ""
echo "===================================="
echo "  Running tests/run_golden_tests.sh (golden stdout)"
echo "===================================="

set +e
GOLDEN_OUTPUT=$(tests/run_golden_tests.sh 2>&1)
GOLDEN_EXIT=$?
set -e
echo "$GOLDEN_OUTPUT"

GOLDEN_RESULTS=$(grep -m1 "Golden results:" <<< "$GOLDEN_OUTPUT" | sed 's/.*Golden results: //' || echo "?? tests")
if [ "$GOLDEN_EXIT" -ne 0 ]; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}  FAIL: golden-stdout tests failed (exit ${GOLDEN_EXIT})\n"
fi

# --- Step 1d: PTY scenarios (terminal-dependent ops) ---
echo ""
echo "===================================="
echo "  Running tests/run_pty_tests.sh (PTY scenarios)"
echo "===================================="

set +e
PTY_OUTPUT=$(tests/run_pty_tests.sh 2>&1)
PTY_EXIT=$?
set -e
echo "$PTY_OUTPUT"

PTY_RESULTS=$(grep -m1 "PTY results:" <<< "$PTY_OUTPUT" | sed 's/.*PTY results: //' || echo "?? scenarios")
if [ "$PTY_EXIT" -ne 0 ]; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}  FAIL: PTY scenarios failed (exit ${PTY_EXIT})\n"
fi

# --- Step 1e: CLI matrix (parse_args flags, exit codes, startup modes) ---
echo ""
echo "===================================="
echo "  Running tests/run_cli_tests.sh (CLI matrix)"
echo "===================================="

set +e
CLI_OUTPUT=$(tests/run_cli_tests.sh 2>&1)
CLI_EXIT=$?
set -e
echo "$CLI_OUTPUT"

CLI_RESULTS=$(grep -m1 "CLI results:" <<< "$CLI_OUTPUT" | sed 's/.*CLI results: //' || echo "?? cases")
if [ "$CLI_EXIT" -ne 0 ]; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}  FAIL: CLI matrix failed (exit ${CLI_EXIT})\n"
fi

# --- Step 2: snapshot tests ---
echo ""
echo "===================================="
echo "  Running snapshot tests"
echo "===================================="

# Suspend `set -e` for the capture so a non-zero exit from the snapshot
# runner does not abort this script before the summary prints.
set +e
SNAP_OUTPUT=$(tests/run_snapshot_tests.sh 2>&1)
SNAP_EXIT=$?
set -e
echo "$SNAP_OUTPUT"

# Extract results line for final summary
SNAP_RESULTS=$(grep -m1 "tests," <<< "$SNAP_OUTPUT" | sed 's/.*Results: //' || echo "?? tests, ?? passed, ?? failed")

if [ "$SNAP_EXIT" -ne 0 ]; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}  FAIL: snapshot tests failed (exit ${SNAP_EXIT})\n"
fi

# Also catch any rt-ok FAILED lines propagated through the snapshot runner
# (run_snapshot_tests.sh re-emits them from its phase outputs).
SNAP_RTOK_FAILS=$(grep -c "^FAILED:" <<< "$SNAP_OUTPUT" || true)
if [ "$SNAP_RTOK_FAILS" -gt 0 ]; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}  FAIL: ${SNAP_RTOK_FAILS} rt-ok assertion(s) failed inside snapshot tests:\n"
    while IFS= read -r line; do
        ERRORS="${ERRORS}    ${line}\n"
    done < <(grep "^FAILED:" <<< "$SNAP_OUTPUT")
fi

# --- Step 3: eqref overflow test (requires a separate invocation with
#     --test-eqgen-preload so all five eq-storage generation counters start
#     at UINT32_MAX; the very next `#=` then trips the 2^32-wrap LimitCheck) ---
echo ""
echo "===================================="
echo "  Running eqref overflow test"
echo "===================================="

set +e
OVF_OUTPUT=$(./trix --test-eqgen-preload=4294967295 tests/test_eqref_overflow.trx 2>&1)
OVF_EXIT=$?
set -e
echo "$OVF_OUTPUT"

if [ "$OVF_EXIT" -ne 0 ]; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}  FAIL: eqref overflow test exited ${OVF_EXIT}\n"
fi

OVF_RTOK_FAILS=$(grep -c "^FAILED:" <<< "$OVF_OUTPUT" || true)
if [ "$OVF_RTOK_FAILS" -gt 0 ]; then
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}  FAIL: ${OVF_RTOK_FAILS} rt-ok assertion(s) failed inside eqref overflow test:\n"
    while IFS= read -r line; do
        ERRORS="${ERRORS}    ${line}\n"
    done < <(grep "^FAILED:" <<< "$OVF_OUTPUT")
fi

# --- Step 4: sandbox-mode tests (require --sandbox; cannot live inside
#     test_all.trx because run-test cannot toggle sandbox at runtime). ---
echo ""
echo "===================================="
echo "  Running sandbox-mode tests"
echo "===================================="

# Every tests/test_*_sandbox.trx runs under --sandbox; each must exit 0
# with no failed rt-ok assertions.
for SBX_FILE in tests/test_*_sandbox.trx; do
    set +e
    SBX_OUTPUT=$(./trix --sandbox "$SBX_FILE" 2>&1)
    SBX_EXIT=$?
    set -e
    echo "$SBX_OUTPUT"

    if [ "$SBX_EXIT" -ne 0 ]; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}  FAIL: ${SBX_FILE} exited ${SBX_EXIT}\n"
    fi

    SBX_RTOK_FAILS=$(grep -c "^FAILED:" <<< "$SBX_OUTPUT" || true)
    if [ "$SBX_RTOK_FAILS" -gt 0 ]; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}  FAIL: ${SBX_RTOK_FAILS} rt-ok assertion(s) failed inside ${SBX_FILE}:\n"
        while IFS= read -r line; do
            ERRORS="${ERRORS}    ${line}\n"
        done < <(grep "^FAILED:" <<< "$SBX_OUTPUT")
    fi
done

# --- Step 5: fuzz regression replay ---
# Replay each previously-crashing fuzz artifact against the current binary.
# An ASan/UBSan report, assert, or abnormal signal on any replay counts as
# a regression.  Normal error output (type-check, undefined, invalid-access
# etc.) is fine -- the binary should reject malformed input, not crash on it.
echo ""
echo "===================================="
echo "  Running fuzz regression replays"
echo "===================================="

FUZZ_REGR_DIR="tests/fuzz_regressions"
FUZZ_REGR_COUNT=0
FUZZ_REGR_FAILS=0
if [ -d "$FUZZ_REGR_DIR" ]; then
    for artifact in "$FUZZ_REGR_DIR"/crash-*; do
        [ -e "$artifact" ] || continue
        FUZZ_REGR_COUNT=$((FUZZ_REGR_COUNT + 1))
        set +e
        # Cap at 15s just in case a regression somehow introduces a hang.
        REGR_OUTPUT=$(timeout 15 ./trix "$artifact" 2>&1)
        REGR_EXIT=$?
        set -e
        # Sanitizer reports and aborts are the signals we watch for.  Any
        # other exit status (including non-zero from a Trix error) is fine.
        if grep -qE "AddressSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:|Assertion .* failed" <<< "$REGR_OUTPUT"; then
            FUZZ_REGR_FAILS=$((FUZZ_REGR_FAILS + 1))
            ERRORS="${ERRORS}  FAIL: fuzz regression $(basename "$artifact") -- sanitizer/assert fired\n"
        elif [ "$REGR_EXIT" -ge 128 ]; then
            # 128+N = killed by signal N (SIGABRT, SIGSEGV, etc.).
            FUZZ_REGR_FAILS=$((FUZZ_REGR_FAILS + 1))
            ERRORS="${ERRORS}  FAIL: fuzz regression $(basename "$artifact") -- killed by signal $((REGR_EXIT - 128))\n"
        fi
    done
    echo "  Replayed ${FUZZ_REGR_COUNT} fuzz artifact(s); ${FUZZ_REGR_FAILS} regression(s)"
    if [ "$FUZZ_REGR_FAILS" -gt 0 ]; then
        FAIL=$((FAIL + 1))
    fi
else
    echo "  (no tests/fuzz_regressions/ directory; skipping)"
fi

# --- Summary ---
echo ""
echo "===================================="
echo "  Trix:       ${STX_RESULTS}"
echo "  Standalone: ${RUNALL_RESULTS}"
echo "  Golden:     ${GOLDEN_RESULTS}"
echo "  PTY:        ${PTY_RESULTS}"
echo "  CLI:        ${CLI_RESULTS}"
echo "  Snapshot:   ${SNAP_RESULTS}"
echo "  Fuzz regr:  ${FUZZ_REGR_COUNT} replays, ${FUZZ_REGR_FAILS} failed"
echo "===================================="
if [ "$FAIL" -eq 0 ]; then
    echo "  ALL TEST STEPS PASSED"
else
    echo "  FAILURES DETECTED ($FAIL)"
    echo ""
    echo -e "$ERRORS"
fi
echo "===================================="

exit "$FAIL"
