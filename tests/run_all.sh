#!/bin/bash
# ====================================================================
# tests/run_all.sh -- run every tests/test_*.trx in standalone mode
# ====================================================================
# Each test is run in a separate ./trix invocation (unlike test_all.trx
# which runs everything in one process via `run`, which leaks dict state
# across tests and masks "missing /rt-ok def"-style breakage).
#
# Output: classified summary at the end.  Per-test config (extra trix
# args, expected-skip reason) lives in the SPECIAL_ARGS / SKIP_REASON
# tables below; everything else runs with default args.
#
# Exit code: 0 if zero unexpected failures, 1 otherwise.
# ====================================================================

set -u
cd "$(dirname "$0")/.."  # run from repo root

# Binary under test.  Defaults to the debug ./trix; override with
# TRIX_BIN=./trix.opt to run the suite (including perf tests) against the
# optimized build.
BIN="${TRIX_BIN:-./trix}"

if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN not built (run ./build.sh first; for the optimized build use ./build.sh --optimized and TRIX_BIN=./trix.opt)" >&2
    exit 1
fi

# Per-test extra args (space-separated).  Anything not listed runs with no extras.
declare -A SPECIAL_ARGS=(
    [test_eqref_overflow.trx]="--test-eqgen-preload=4294967295"
    # Tests below exhaust the default 1 MB VM.  Their cumulative
    # allocations plus a single large request (e.g. 200x80 screen,
    # 65535-slot |locals|, 10k random names) tip past 1 MB.  Bumped to
    # 2 MB to give headroom; trimming the tests is the alternative but
    # they exist precisely to stress these paths.
    [test_debug_ui_pure.trx]="--vm-size=2M"
    [test_lazy.trx]="--vm-size=2M"
    [test_name_torture.trx]="--vm-size=2M"
    [test_screen.trx]="--vm-size=2M"
    [test_screen_render_direct.trx]="--vm-size=2M"
    # Builds 65535-element trigger arrays to prove the overflow guards reject
    # them before allocating the (would-be undersized) result.
    [test_b5_safety.trx]="--vm-size=8M"
    # B7: 32767-cell cell-combine (reactive:1-1) + 65535-elem proc-disasm
    # (debugger:1-1) build large trigger arrays before the guard fires.
    [test_b7_safety.trx]="--vm-size=8M"
    [test_b7_debugger_safety.trx]="--vm-size=8M"
    [test_b7_gc.trx]="--vm-size=8M"
    # Fills a dynamic dict and set to the 65,535-entry cap; the fill plus
    # expansion pool blocks exceed the default VM.
    [test_dict_expand_max.trx]="--vm-size=8M"
    # Overflow arms for every fixed stack at its documented MINIMUM
    # depth, so each fires after a handful of iterations.
    [test_stack_limits.trx]="--operand-depth=128 --exec-depth=128 --dict-depth=16 --error-depth=8 --save-depth=4 --scratch-depth=16"
    # VM Redux Phase 7 retired the persist region in favor of a fixed
    # per-coroutine scratch arena (--scratch-depth, default 128 Objects).
    # Stress sections of find-all push >1000 results in one call.
    [test_find_all_persist.trx]="--scratch-depth=2048"
    # Sandbox-block tests verify the host-op ban (raw memory and
    # filesystem families).
    [test_memory_sandbox.trx]="--sandbox"
    [test_fs_sandbox.trx]="--sandbox"
    [test_snapshot_sandbox.trx]="--sandbox"
)

# Per-test extra environment variables (space-separated KEY=value pairs).
# Anything not listed inherits the runner's environment unchanged.  Set
# variables that affect deterministic output -- e.g. TZ for chrono local-
# zone tests that hard-code New-York-City offsets.
declare -A SPECIAL_ENV=(
    [test_chrono_local.trx]="TZ=America/New_York"
)

# Per-test skip reason.  Listed tests are not run; reason printed in summary.
# These are tests that genuinely cannot run in a plain shell context
# (need a tty, need piped stdin, are fragments of multi-phase orchestration).
declare -A SKIP_REASON=(
    [test_actor_snapshot.trx]="phase-1 of run_snapshot_tests.sh"
    [test_memory_stream_snapshot.trx]="phase-1 of run_snapshot_tests.sh"
    [test_gc_stress_lazy.trx]="GC-stress phase -- run via tests/run_gc_stress_tests.sh (debug-only vm-gc-stress / TRIX_DEBUGGER, cannot run under ./trix.opt)"
    [test_gc_stress_record.trx]="GC-stress phase -- run via tests/run_gc_stress_tests.sh (debug-only vm-gc-stress / TRIX_DEBUGGER, cannot run under ./trix.opt)"
    [test_gc_stress_b7.trx]="GC-stress phase -- run via tests/run_gc_stress_tests.sh (debug-only vm-gc-stress / TRIX_DEBUGGER, cannot run under ./trix.opt)"
    [test_gc_stress_container_extvalue.trx]="GC-stress phase -- run via tests/run_gc_stress_tests.sh (debug-only vm-gc-stress / TRIX_DEBUGGER, cannot run under ./trix.opt)"
    [test_gc_stress_continuation.trx]="GC-stress phase -- run via tests/run_gc_stress_tests.sh (debug-only vm-gc-stress / TRIX_DEBUGGER, cannot run under ./trix.opt)"
    [test_gc_stress_logic.trx]="GC-stress phase -- run via tests/run_gc_stress_tests.sh (debug-only vm-gc-stress / TRIX_DEBUGGER, cannot run under ./trix.opt)"
    [test_gc_stress_r6_clone.trx]="GC-stress phase -- run via tests/run_gc_stress_tests.sh (debug-only vm-gc-stress / TRIX_DEBUGGER, cannot run under ./trix.opt)"
    [test_command_line_args_helper.trx]="helper file (invoked by test_command_line_args.trx)"
    [test_interactive.trx]="needs interactive REPL stdin"
    [test_raw_mode_sandbox.trx]="needs a tty (raw-mode requires it)"
    [test_stdin.trx]="needs piped stdin input"
    # test_all.trx runs every other test in a single process via `(file) run`.
    # Per memory note in reference_test_runners.md, this single-process model
    # masks "missing helper def" bugs because dict state leaks across
    # save/restore boundaries inside the run-test wrapper.  Now that uncaught
    # errors propagate as non-zero exit codes (PATCH 22), state leaks that
    # were silent before surface as real failures here.  run_all.sh covers
    # every individual test in process-per-test mode, which is the
    # authoritative coverage; test_all.trx is the redundant compat path.
    [test_all.trx]="redundant single-process runner (run_all.sh covers all tests strictly)"
)

# Detect compile-time-gated features by probing the binary.  Tests that
# exercise a gated-out feature are added to SKIP_REASON so a release
# build (./build.sh --release) doesn't see spurious failures from
# missing operators.  Trix's default error handler exits 0 even on
# undefined-name failures, so probe via `where` (which returns false
# cleanly for an undefined name) instead of relying on exit codes.
probe=$(echo "/clear-alloc-stats where { pop (yes) = } { (no) = } if-else" | "$BIN" --stdin 2>/dev/null)
if [ "$probe" != "yes" ]; then
    SKIP_REASON[test_alloc_stats.trx]="vm-heap-tracking disabled in this build"
    SKIP_REASON[test_vm_heap_track.trx]="vm-heap-tracking disabled in this build"
fi

# zlib gate (TRIX_NO_ZLIB): the deflate/inflate operators stay registered
# but raise /unsupported, so `where` can't detect the gate -- probe by
# actually running deflate.  The compression suites assume a working zlib;
# skip them when it's gated out (the build-portable test_no_zlib.trx still
# verifies the /unsupported behaviour in both builds).
zprobe=$(echo "<< /unsupported { (no) = } >> { (x) deflate pop (yes) = } try-catch" | "$BIN" --stdin 2>/dev/null)
if [ "$zprobe" != "yes" ]; then
    SKIP_REASON[test_compress.trx]="zlib disabled in this build (TRIX_NO_ZLIB)"
    SKIP_REASON[test_compress_stream.trx]="zlib disabled in this build (TRIX_NO_ZLIB)"
    SKIP_REASON[test_memory_errors.trx]="zlib disabled in this build (TRIX_NO_ZLIB) -- deflate/inflate error-path coverage needs zlib"
fi

# Detect the build mode from --version ("build: optimized" vs "build: debug").
build_line=$("$BIN" --version 2>/dev/null | grep '^build:')

# The interactive debugger (debug-* ops like debug-call-depth) is compiled out
# unless the build reports the "debugger" feature (TRIX_DEBUGGER) -- the
# optimized and --release builds drop it.  These tests exercise those ops, so
# skip them (rather than fail with "undefined 'debug-*'") when it is absent.
if [[ "$build_line" != *"debugger"* ]]; then
    dbg_reason="needs the interactive debugger (TRIX_DEBUGGER); compiled out of this build"
    SKIP_REASON[test_debug_hooks.trx]="$dbg_reason"
    SKIP_REASON[test_debug_snapshot_ops.trx]="$dbg_reason"
    SKIP_REASON[test_debug_ui_pure.trx]="$dbg_reason"
    SKIP_REASON[test_b7_debugger_safety.trx]="$dbg_reason"
fi

# test_tetrix_native exercises the tetrix-binary-only user ops
# (field-copy-fast, score-board-fast, etc.).  Under ./trix these aren't
# registered; skip the test rather than self-quit so the sentinel check
# doesn't flag it as a mid-file abort.
probe=$(echo "userdict /field-copy-fast known? { (yes) = } { (no) = } if-else" | "$BIN" --stdin 2>/dev/null)
if [ "$probe" != "yes" ]; then
    SKIP_REASON[test_tetrix_native.trx]="needs ./tetrix binary (./trix lacks tetrix user ops)"
    SKIP_REASON[test_b8_tetrix_safety.trx]="needs ./tetrix binary (./trix lacks tetrix user ops)"
    # test_b8_tetrix_capacity.trx is an expected-FAILURE probe (exits 28 with
    # opstack-overflow under ./tetrix); always skipped here -- run it by hand.
    SKIP_REASON[test_b8_tetrix_capacity.trx]="expected-failure probe; run under ./tetrix --operand-depth 128 (see file header)"
fi

TIMEOUT=60
TOTAL=0
PASSED=0
FAILED=0
SKIPPED=0
FAIL_LINES=()
FAIL_OUTPUT=()   # parallel to FAIL_LINES: captured stdout/stderr of each failing test
SKIP_LINES=()

for f in tests/test_*.trx; do
    name=$(basename "$f")
    TOTAL=$((TOTAL+1))

    if [ -n "${SKIP_REASON[$name]:-}" ]; then
        SKIPPED=$((SKIPPED+1))
        SKIP_LINES+=("  SKIP  $name -- ${SKIP_REASON[$name]}")
        continue
    fi

    extra="${SPECIAL_ARGS[$name]:-}"
    env_pairs="${SPECIAL_ENV[$name]:-}"
    # Capture stdout only -- stderr is left inherited.  Two reasons:
    #   1. A handful of tests probe `stderr` for /io-seek-error to verify
    #      stderr is not seekable.  Redirecting stderr to a file or pipe
    #      (including /dev/null) makes it seekable and the tests fail
    #      spuriously.  Inheriting (typically a tty) preserves the
    #      non-seekable property the tests assume.
    #   2. Many tests intentionally trigger errors and catch them; the
    #      runtime still logs "Trix <error>" lines to stderr.  Inheriting
    #      shows the user the same noise they'd see running tests by
    #      hand -- nothing to suppress, the test's exit code and stdout
    #      determine the verdict.
    if [ -n "$env_pairs" ]; then
        out=$(timeout $TIMEOUT env $env_pairs "$BIN" $extra "$f")
    else
        out=$(timeout $TIMEOUT "$BIN" $extra "$f")
    fi
    ec=$?

    # A test fails if:
    #   - non-zero exit code (uncaught error), or timeout
    #   - stdout shows a failure marker: "FAILED:", "FAIL #", a (possibly
    #     indented) "FAIL:", or a non-zero "FAILURES = N" count summary
    #   - stdout is missing the "__test_ok__" sentinel printed at end of file
    # The sentinel check catches mid-file aborts: Trix's default error
    # handler exits 0 on uncaught errors, so a test that throws partway
    # through (e.g. vm-full) would otherwise be classified as PASS.  The
    # failure-marker check is the inverse guard -- a test that prints a
    # failed assertion (or a non-zero FAILURES summary) but still reaches
    # its unconditional sentinel must not be classified as PASS.
    # stderr "Trix <error>" lines are ignored: many tests intentionally
    # trigger errors and catch them; the runtime still logs to stderr.
    if [ $ec -ne 0 ]; then
        FAILED=$((FAILED+1))
        if [ $ec -eq 124 ]; then
            FAIL_LINES+=("  FAIL  $name -- TIMEOUT (>${TIMEOUT}s)")
        else
            FAIL_LINES+=("  FAIL  $name -- exit=$ec")
        fi
        FAIL_OUTPUT+=("$out")
    elif echo "$out" | grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]"; then
        FAILED=$((FAILED+1))
        first=$(echo "$out" | grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" | head -1)
        FAIL_LINES+=("  FAIL  $name -- $first")
        FAIL_OUTPUT+=("$out")
    elif ! echo "$out" | grep -qE "^__test_ok__$"; then
        FAILED=$((FAILED+1))
        FAIL_LINES+=("  FAIL  $name -- missing sentinel (likely mid-file abort)")
        FAIL_OUTPUT+=("$out")
    else
        PASSED=$((PASSED+1))
    fi
done

echo "===================================="
echo "  Trix Standalone Test Runner"
echo "===================================="
echo "  total:   $TOTAL"
echo "  passed:  $PASSED"
echo "  failed:  $FAILED"
echo "  skipped: $SKIPPED"
echo

if [ ${#FAIL_LINES[@]} -gt 0 ]; then
    echo "Failures:"
    fi_idx=0
    for line in "${FAIL_LINES[@]}"; do
        echo "$line"
        # Echo the tail of the failing test's captured output so a CI failure is
        # self-diagnosing -- the per-test output is otherwise discarded, leaving
        # only "exit=N" with no clue what actually went wrong.
        printf '%s\n' "${FAIL_OUTPUT[$fi_idx]}" | tail -n 30 | sed 's/^/        | /'
        echo
        fi_idx=$((fi_idx+1))
    done
    echo
fi

if [ ${#SKIP_LINES[@]} -gt 0 ]; then
    echo "Skipped:"
    for line in "${SKIP_LINES[@]}"; do
        echo "$line"
    done
    echo
fi

echo "===================================="
if [ $FAILED -eq 0 ]; then
    echo "  ALL TESTS PASSED"
    exit 0
else
    echo "  $FAILED TEST(S) FAILED"
    exit 1
fi
