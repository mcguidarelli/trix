#!/bin/bash
# run_cli_tests.sh -- command-line interface matrix (parse_args coverage).
#
# Pins the CLI contract: exit-printing flags (-h/-v/--about), every
# parse_scaled/parse_clamped/parse_plain error and clamp arm, --stream-io
# parsing, --name-buckets prime snapping, startup-mode resolution and the
# mutual-exclusion usage errors, and the startup-failure exit codes
# (missing script, bad image, disabled stdio modes, stream-slot
# exhaustion).  The Error enum doubles as the process exit code, so the
# rc values pinned here are part of the public contract (types.inl).
#
# Error-enum exit codes are written SYMBOLICALLY (e.g. "undefined",
# "file-open-error") and resolved to numbers from the binary's own
# --error-codes dump via error_codes.sh, so the matrix follows the enum
# automatically and a renumber cannot silently break it.  The non-enum
# codes (0 = clean, 1 = usage/getopt failure, 124 = timeout) stay numeric.
# tests/check_error_codes.py pins the documented catalog against the enum.
#
# Each case = expected exit code + output match (exact or fixed-string
# needles) + flags.  Build-variant text (--version build line, debugger
# flag set) is matched by needle, never byte-exact, so the matrix runs
# unchanged against both ./trix (debug) and ./trix.opt via TRIX_BIN.
# Debugger-only flags (--inspect family, -d) are skipped when --version
# does not list the debugger feature.
#
# Exit code: 0 if every case passes, 1 otherwise.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TRIX="${TRIX_BIN:-${PROJECT_DIR}/trix}"
CLI="tests/cli"

if [ ! -x "$TRIX" ]; then
    echo "FATAL: trix binary not found or not executable: $TRIX" >&2
    exit 2
fi

# Symbolic Error-name -> exit-code resolution (sourced; auto-loads from $TRIX).
source "$SCRIPT_DIR/error_codes.sh"
if [ "${#TRIX_ERR_CODE[@]}" -eq 0 ]; then
    echo "FATAL: could not load error codes from '$TRIX --error-codes'" >&2
    exit 2
fi

WORK_DIR="$(mktemp -d /tmp/trix-cli-tests.XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

PASS=0
FAIL=0

# run_case <name> <want_rc> <match> <expected> <stdin_data> -- <args...>
#   match = exact : combined output equals <expected> byte-for-byte
#           grep  : every line of <expected> appears (fixed-string)
#           none  : combined output is empty
#   stdin_data = "-" for /dev/null, anything else is piped in
#   CASE_TIMEOUT (seconds, default 15) caps the run; want_rc=124 turns the
#   timeout into the EXPECTED outcome (pins that --resident stays parked).
run_case() {
    local name="$1" match="$3" expected="$4" stdin_data="$5"
    # want_rc may be a bare integer (0/1/124) or a symbolic Error name; an
    # unknown name is fatal (resolve_rc returns non-zero -> abort the suite).
    local want_rc
    want_rc="$(resolve_rc "$2")" || {
        echo "FATAL: run_case '$name': unknown Error name '$2'" >&2
        exit 2
    }
    shift 5
    [ "$1" = "--" ] && shift
    local case_timeout="${CASE_TIMEOUT:-15}"

    local actual rc
    if [ "$stdin_data" = "-" ]; then
        actual="$(cd "$PROJECT_DIR" && timeout "$case_timeout" "$TRIX" "$@" < /dev/null 2>&1)"
    else
        actual="$(printf '%s\n' "$stdin_data" | (cd "$PROJECT_DIR" && timeout "$case_timeout" "$TRIX" "$@" 2>&1))"
    fi
    rc=$?

    if [ "$rc" -eq 124 ] && [ "$want_rc" -ne 124 ]; then
        echo "  FAIL  $name -- TIMEOUT (hung; killed after ${case_timeout}s)"
        FAIL=$((FAIL + 1))
        return
    fi
    if [ "$rc" -ne "$want_rc" ]; then
        echo "  FAIL  $name -- exit code $rc (expected $want_rc)"
        echo "        output: $(printf '%s' "$actual" | head -3)"
        FAIL=$((FAIL + 1))
        return
    fi

    case "$match" in
    exact)
        if [ "$actual" != "$expected" ]; then
            echo "  FAIL  $name -- output differs:"
            diff <(printf '%s\n' "$actual") <(printf '%s\n' "$expected") | sed 's/^/        /' | head -10
            FAIL=$((FAIL + 1))
            return
        fi
        ;;
    grep)
        local needle
        while IFS= read -r needle; do
            [ -z "$needle" ] && continue
            if ! printf '%s\n' "$actual" | grep -qF -- "$needle"; then
                echo "  FAIL  $name -- missing needle: $needle"
                echo "        output: $(printf '%s' "$actual" | head -3)"
                FAIL=$((FAIL + 1))
                return
            fi
        done <<< "$expected"
        ;;
    none)
        if [ -n "$actual" ]; then
            echo "  FAIL  $name -- expected no output, got: $(printf '%s' "$actual" | head -3)"
            FAIL=$((FAIL + 1))
            return
        fi
        ;;
    esac

    echo "  ok    $name"
    PASS=$((PASS + 1))
}

# Feature detection (run_all.sh pattern): the --version build line lists
# compiled-in features; debugger-only flag cases are skipped without it.
VERSION_OUT="$("$TRIX" --version < /dev/null 2>&1)"
HAS_DEBUGGER=0
printf '%s\n' "$VERSION_OUT" | grep -q ' debugger' && HAS_DEBUGGER=1

#--- exit-printing flags ---#
run_case help-short 0 grep 'Usage: trix [options] [filename] [script-args...]
--vm-size=BYTES
--stream-io=MODE
--module-path=PATH
--resident' - -- -h
run_case help-long 0 grep 'Usage: trix [options] [filename] [script-args...]' - -- --help
run_case version 0 grep 'the cat in concatenative
build: ' - -- -v
run_case about 0 grep 'Licensed under the Apache License
Default configuration:
VM size:' - -- --about

#--- unknown options (getopt-generated text; argv[0]-prefixed) ---#
run_case unknown-long 1 grep "unrecognized option '--bogus'" - -- --bogus "$CLI/ok.trx"
run_case unknown-short 1 grep "invalid option -- 'x'" - -- -x "$CLI/ok.trx"

#--- parse_scaled / parse_clamped arms ---#
run_case vm-size-invalid 1 exact "trix: --vm-size: invalid value 'abc'" - -- --vm-size=abc "$CLI/ok.trx"
run_case vm-size-erange 1 exact "trix: --vm-size: invalid value '99999999999999999999'" - -- --vm-size=99999999999999999999 "$CLI/ok.trx"
run_case vm-size-suffix-overflow 1 exact "trix: --vm-size: invalid value '99999999999999999G'" - -- --vm-size=99999999999999999G "$CLI/ok.trx"
run_case vm-size-trailing-garbage 1 exact "trix: --vm-size: invalid value '2X'" - -- --vm-size=2X "$CLI/ok.trx"
run_case vm-size-post-suffix-garbage 1 exact "trix: --vm-size: invalid value '2KB'" - -- --vm-size=2KB "$CLI/ok.trx"
run_case vm-size-below-min-clamps 0 grep 'trix: --vm-size: 1 is below minimum 262144
cli-ok' - -- --vm-size=1 "$CLI/ok.trx"
# regression: a tracking build must boot at the advertised MinVmSize
# (heap-tracking tables scale with the heap)
run_case vm-size-min-boots 0 exact 'cli-ok' - -- -q --vm-size=256K "$CLI/ok.trx"
run_case eq-string-above-max-clamps 0 grep 'trix: --eq-string: 999999 exceeds maximum 256
cli-ok' - -- -q --eq-string=999999 "$CLI/ok.trx"
run_case g-suffix-multiplies 0 grep 'trix: --eq-string: 1073741824 exceeds maximum 256' - -- -q --eq-string=1G "$CLI/ok.trx"
run_case eq-set-above-max-clamps 0 grep 'trix: --eq-set: 999999 exceeds maximum 256
cli-ok' - -- -q --eq-set=999999 "$CLI/ok.trx"

#--- parse_plain arms (no K/M/G suffixes) ---#
run_case save-depth-invalid 1 exact "trix: --save-depth: invalid value 'abc'" - -- --save-depth=abc "$CLI/ok.trx"
run_case save-depth-rejects-suffix 1 exact "trix: --save-depth: invalid value '2K'" - -- --save-depth=2K "$CLI/ok.trx"
run_case save-depth-below-min-clamps 0 grep 'trix: --save-depth: 1 is below minimum 4
cli-ok' - -- -q --save-depth=1 "$CLI/ok.trx"
run_case save-depth-above-max-clamps 0 grep 'trix: --save-depth: 999 exceeds maximum 255
cli-ok' - -- -q --save-depth=999 "$CLI/ok.trx"

#--- --stream-io parsing ---#
run_case stream-io-unknown 1 exact "trix: --stream-io: unknown 'bogus' (use none, all, or comma-separated: stdin,stdout,stderr,stdedit)" - -- --stream-io=bogus "$CLI/ok.trx"
run_case stream-io-mixed-unknown 1 grep "unknown 'bogus'" - -- --stream-io=stdin,bogus "$CLI/ok.trx"
run_case stream-io-list 0 exact 'cli-ok' - -- -q --stream-io=stdout,stderr "$CLI/ok.trx"
run_case stream-io-all 0 exact 'cli-ok' - -- -q --stream-io=all "$CLI/ok.trx"
# stdout disabled: the script's print raises unsupported (rc 44), silently
run_case stream-io-none-print unsupported none '' - -- -q --stream-io=none "$CLI/ok.trx"
run_case stream-io-empty-print unsupported none '' - -- -q --stream-io= "$CLI/ok.trx"

#--- --name-buckets prime snapping (observable via status dict) ---#
run_case name-buckets-snap 0 exact '1031' - -- -q --name-buckets=1000 "$CLI/status-name-buckets.trx"
run_case name-buckets-below-min 0 grep 'trix: --name-buckets: 50 is below minimum 131
131' - -- -q --name-buckets=50 "$CLI/status-name-buckets.trx"

#--- value flags wired through to the VM (observable via status dict) ---#
run_case stream-buffer-k-suffix 0 exact '8192' - -- -q --stream-buffer=8K "$CLI/status-stream-buffer.trx"
run_case userdict-size 0 exact '2048' - -- -q --userdict-size=2048 "$CLI/status-userdict.trx"

#--- remaining tuning flags: acceptance (boots and runs) ---#
run_case depth-flags-accept 0 exact 'cli-ok' - -- -q --operand-depth=128 --exec-depth=256 --dict-depth=16 --error-depth=16 --scratch-depth=128 "$CLI/ok.trx"
run_case eq-flags-accept 0 exact 'cli-ok' - -- -q --eq-string=64 --eq-array=32 --eq-proc=32 --eq-dict=16 --eq-set=16 "$CLI/ok.trx"
run_case misc-flags-accept 0 exact 'cli-ok' - -- -q --stream-count=8 --save-depth=8 --module-path=/tmp --quantum=100 "$CLI/ok.trx"
run_case quantum-above-max-clamps 0 grep 'trix: --quantum: 2000000000 exceeds maximum 1000000000
cli-ok' - -- -q --quantum=2000000000 "$CLI/ok.trx"
# the Error enum doubles as the exit code: ExecutionLimit = 53
run_case max-ops-limit execution-limit none '' - -- -q --max-ops=50 "$CLI/loop.trx"
run_case max-ops-unlimited 0 exact '1001' - -- -q --max-ops=0 "$CLI/loop.trx"
# sleep-budget: the cumulative park grant caps a ~28h sleep to ~0.1s; a
# regression here surfaces as the runner's CASE_TIMEOUT killing the case
run_case sleep-budget-bounds 0 exact 'sleep-ok' - -- -q --sleep-budget=100 "$CLI/sleep-huge.trx"
run_case sleep-budget-zero-unlimited 0 exact 'cli-ok' - -- -q --sleep-budget=0 "$CLI/ok.trx"
# example exit-code contract: invalid argument VALUES raise /range-check
# (rc 30, sibling convention); bare runs print guidance and exit 0
run_case schedule-usage-error range-check grep 'schedule: --kind must be daily|weekly|monthly|yearly (got bogus)' - -- -q examples/schedule.trx --kind bogus
run_case schedule-bare-exit-zero 0 grep 'schedule: --kind is required' - -- -q examples/schedule.trx

#--- startup modes ---#
run_case repl-pipe 0 grep 'repl-ok' '(repl-ok) = quit' -- -q
run_case file-then-repl 0 grep 'cli-ok
repl2' '(repl2) = quit' -- -q -i "$CLI/ok.trx"
run_case stdin-mode 0 exact 'stdin-ok' '(stdin-ok) =' -- -q --stdin
run_case argv-tail-not-parsed 0 grep '[(--help)#lr (-x)#lr (extra)#lr]' - -- -q "$CLI/echo-args.trx" --help -x extra

#--- mode mutual exclusions (usage errors, exit 1) ---#
run_case stdin-excludes-i 1 exact 'trix: --stdin and -i/--stdedit are mutually exclusive' 'quit' -- --stdin -i
run_case stdin-excludes-file 1 exact 'trix: --stdin and filename are mutually exclusive' - -- --stdin "$CLI/ok.trx"
run_case image-requires-file 1 exact 'trix: -l/--image requires an image filename' - -- -l
run_case image-excludes-i 1 exact 'trix: --image and -i/--stdedit are mutually exclusive' - -- -l -i "$CLI/ok.trx"

#--- startup failures exit non-zero (Error enum = exit code) ---#
run_case missing-script file-open-error grep 'Trix could not open startup file' - -- "$WORK_DIR/no_such.trx"
run_case image-missing invalid-image-file grep 'thaw: cannot open' - -- -l "$WORK_DIR/no_such.snap"
run_case image-not-an-image invalid-image-file grep 'thaw: header read failed' - -- -l "$CLI/ok.trx"
run_case stdin-mode-disabled invalid-stream-access grep 'Trix stdin is not available (disabled via --stream-io)' 'quit' -- --stream-io=none --stdin
run_case stdedit-mode-disabled invalid-stream-access grep 'Trix stdedit is not available (disabled via --stream-io)' - -- --stream-io=none -i
run_case startup-no-stream-slot limit-check grep "Trix could not allocate a 'stream' for startup file" - -- --stream-count=0 "$CLI/ok.trx"

#--- snap-shot image round trip through -l ---#
run_case image-make 0 none '' - -- -q "$CLI/make-image.trx" "$WORK_DIR/cli.snap"
run_case image-load 0 none '' - -- -q -l "$WORK_DIR/cli.snap"

#--- --resident: no Quit floor; drained startup work parks the interpreter
#--- on the IRQ wait, serving raise_interrupt()/invoke() until ExitIRQ ---#
# full serve cycle: script ends -> park -> Level0 IRQ served -> handler
# schedules ExitIRQ -> parked instance wakes and exits 0
run_case resident-serves-after-script 0 grep 'script-done
irq-served' - -- -q --resident "$CLI/resident.trx"
# park proof: with no work delivered, a resident instance does NOT exit
CASE_TIMEOUT=2 run_case resident-stays-parked 124 grep 'cli-ok' - -- -q --resident "$CLI/ok.trx"

#--- resource-failure process contracts ---#
# A COMPLETELY full operand stack leaves try no slot for the error
# name: the interpreter escalates past it to the global handler and
# the process exits with the opstack-overflow code.
run_case try-bypass-full-opstack opstack-overflow grep 'try-catch bypassed: operand stack full' - -- --operand-depth=128 "$CLI/op-full.trx"

# VM exhausted so thoroughly that the global handler cannot allocate
# its stack-preservation arrays: the 'insufficient VM for vm-full
# handler' short-circuit must still exit with the VMFull code (45),
# not 0 (ENGINE BUG #9: the global_handler short-circuit arms threw
# Exception::Exit without recording m_exit_code).
run_case vmfull-starved-handler vm-full grep 'insufficient VM for vm-full handler' - -- --vm-size=256K "$CLI/vm-starve.trx"

# C-heap exhaustion in the raw-memory `alloc` op (malloc returns null
# under an address-space rlimit).  Sanitizer builds reserve huge
# shadow mappings and cannot start under ulimit -v, so this case runs
# only when --version reports no sanitizers.
if printf '%s\n' "$VERSION_OUT" | grep -q 'sanitizers: none'; then
    ULIMIT_OUT="$(cd "$PROJECT_DIR" && bash -c "ulimit -v 400000; timeout 15 '$TRIX' -q --vm-size=2M --stdin" <<< '{ 999999999 alloc } try == quit' 2>&1)"
    ULIMIT_RC=$?
    if [ "$ULIMIT_RC" -eq 0 ] && printf '%s\n' "$ULIMIT_OUT" | grep -qF '/limit-check'; then
        echo "  ok    alloc-malloc-fail-ulimit"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  alloc-malloc-fail-ulimit -- rc=$ULIMIT_RC out: $(printf '%s' "$ULIMIT_OUT" | head -2)"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  skip  alloc-malloc-fail-ulimit (sanitizer build cannot run under ulimit -v)"
fi

#--- debugger-only flags ---#
if [ "$HAS_DEBUGGER" -eq 1 ]; then
    run_case debug-flag-accept 0 exact 'cli-ok' - -- -q -d "$CLI/ok.trx"
    run_case inspect-requires-file 1 exact 'trix: --inspect / --inspect-on-error / --inspect-at requires a script filename' - -- --inspect
    run_case inspect-on-error-requires-file 1 exact 'trix: --inspect / --inspect-on-error / --inspect-at requires a script filename' - -- --inspect-on-error
    run_case inspect-at-requires-file 1 exact 'trix: --inspect / --inspect-on-error / --inspect-at requires a script filename' - -- --inspect-at=/foo
    run_case inspect-excludes-i 1 exact 'trix: --inspect and -i/--stdedit are mutually exclusive' - -- --no-color --inspect -i "$CLI/ok.trx"
    # break-on-error: the /error event fires for an uncaught name, then the
    # error completes with its Error-enum exit code (undefined = 40); an
    # actor's uncaught error halts too, then the actor dies as usual
    run_case break-on-error-halts undefined grep 'armed
halt-tag=error' - -- -q "$CLI/break-on-error.trx"
    run_case break-on-error-actor 0 exact 'actor-running
halt-in-actor=error
main-survives' - -- -q "$CLI/break-on-error-actor.trx"
else
    echo "  skip  debugger-flag cases (no debugger feature in this build)"
fi

echo "===================================="
echo "  CLI results: $((PASS + FAIL)) cases, ${PASS} passed, ${FAIL} failed"
echo "===================================="

[ "$FAIL" -eq 0 ] || exit 1
exit 0
