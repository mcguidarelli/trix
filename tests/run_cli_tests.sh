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
# the Error enum doubles as the exit code (resolved symbolically here)
run_case max-ops-limit execution-limit none '' - -- -q --max-ops=50 "$CLI/loop.trx"
run_case max-ops-unlimited 0 exact '1001' - -- -q --max-ops=0 "$CLI/loop.trx"
# --timeout: wall-clock deadline.  An infinite loop trips /time-limit; a quick
# script finishes well within a generous deadline; 0 means unlimited.
run_case timeout-trips time-limit none '' - -- -q --timeout=100 -e '{ } loop'
run_case timeout-generous 0 exact 'cli-ok' - -- -q --timeout=10000 "$CLI/ok.trx"
run_case timeout-zero-unlimited 0 exact 'cli-ok' - -- -q --timeout=0 "$CLI/ok.trx"
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

#--- -e/--eval inline source ---#
run_case eval-basic 0 exact '5' - -- -q -e '2 3 add ='
run_case eval-long 0 exact 'eval-ok' - -- -q --eval '(eval-ok) ='
run_case eval-empty 0 none '' - -- -q -e ''
# tokens after EXPR are the script's argv tail (no filename is consumed)
run_case eval-args 0 exact '[(alpha)#lr (beta)#lr]' - -- -q -e 'command-line-args ==' alpha beta
# -e then -i: inline source runs, then the REPL takes over
run_case eval-then-repl 0 grep 'eval-ok
repl2' '(repl2) = quit' -- -q -e '(eval-ok) =' -i

#--- -c/--check scan-only (never executes) ---#
run_case check-clean-file 0 none '' - -- -q -c "$CLI/ok.trx"
# loop.trx prints 1001 when run; under -c it must stay silent (not executed)
run_case check-no-execute 0 none '' - -- -q -c "$CLI/loop.trx"
run_case check-eval-clean 0 none '' - -- -q -c -e '2 3 add ='
# composites ([ ], << >>, {{ }}) build via the operand stack while scanning, so
# check mode must keep that machinery working (not just discard tokens)
run_case check-composites 0 none '' - -- -q -c -e '[ 1 [ 2 3 ] 4 ] << /a 1 >> {{ /s }} =='
# many unconsumed top-level values (300 literals, 128-deep stack) must not
# overflow under -c: the drain between top-level tokens keeps the stack bounded
CHECK_MANY="$(yes '1' | head -300 | tr '\n' ' ')"
run_case check-no-opstack-overflow 0 none '' - -- -q --operand-depth=128 -c -e "$CHECK_MANY"
# unbalanced brace (EOF inside proc) and a stray close brace both fail to scan
run_case check-syntax-error-file syntax-error none '' - -- -q -c "$CLI/bad-syntax.trx"
run_case check-stray-brace syntax-error none '' - -- -q -c -e '1 2 } add'

#--- scan-time stack-effect check ( |params -- outputs| ) ---#
# a body matching its declared effect scans cleanly (the proc is scanned then dropped)
run_case stack-effect-ok 0 exact '99' - -- -q -e '{ |a b -- s| a b add } pop 99 ='
# ...and runs correctly when invoked
run_case stack-effect-ok-run 0 exact '12' - -- -q -e '/f { |a b -- p| a b mul } def 3 4 f ='
# zero-input declaration ( -- r ) is allowed (minimal scratch frame)
run_case stack-effect-zero-in 0 exact '42' - -- -q -e '/g { | -- r| 42 } def g ='
# a |...| with no `--` is unchecked, exactly as before
run_case stack-effect-no-decl 0 exact '7' - -- -q -e '/h { |a b| a b add } def 3 4 h ='
# body leaves the wrong number of outputs -> /stack-effect (exit 60), at scan time
run_case stack-effect-mismatch stack-effect none '' - -- -q -e '{ |x -- y| x dup } pop'
# body consumes below its declared inputs -> /stack-effect
run_case stack-effect-underflow stack-effect none '' - -- -q -e '{ |x -- y| x add } pop'
# a bare if whose branch is not stack-neutral -> /stack-effect
run_case stack-effect-if-imbalance stack-effect none '' - -- -q -e '{ |flag -- r| flag { 1 } if } pop'
# if-else branches that disagree on net effect -> /stack-effect
run_case stack-effect-ifelse-mismatch stack-effect none '' - -- -q -e '{ |flag -- r| flag { 1 } { 2 3 } if-else } pop'
# --no-stack-check disables the gate: the mismatch scans without error
run_case stack-effect-disabled 0 none '' - -- -q --no-stack-check -e '{ |x -- y| x dup } pop'
# duplicate `--` separator is a syntax error
run_case stack-effect-dup-separator syntax-error none '' - -- -q -e '{ |a -- -- b| a } pop'
# a `/`-prefixed output name is a syntax error
run_case stack-effect-slash-output syntax-error none '' - -- -q -e '{ |a -- /b| a } pop'

#--- stack-effect: more happy cases (varied valid shapes) ---#
# zero outputs: body must consume back to net 0
run_case stack-effect-zero-out 0 exact 'ok' - -- -q -e '{ |a b -- | a b add pop } pop (ok) ='
# multiple outputs ( 1 -- 2 )
run_case stack-effect-multi-out 0 exact '2' - -- -q -e '{ |a -- x y| a a } pop 2 ='
# count-based: a type-converting op (string -> number) keeps arity ( 1 -- 1 )
run_case stack-effect-conversion 0 exact '23' - -- -q -e '/cv { | -- r| 1 (22) to-number add } def cv ='
# correct if (stack-neutral branch)
run_case stack-effect-if-ok 0 exact '1' - -- -q -e '{ |f -- | f { } if } pop 1 ='
# correct if-else (branches agree on net), exercised at run time
run_case stack-effect-ifelse-ok 0 exact '10' - -- -q -e '/pick { |f -- r| f { 10 } { 20 } if-else } def true pick ='
# correct repeat (stack-neutral body), exercised at run time
run_case stack-effect-repeat-ok 0 exact '5' - -- -q -e '{ |n -- s| 0 n { 1 add } repeat } 5 exch exec ='
# declared local (/t) combined with a declared effect (local-def is `/name value --`)
run_case stack-effect-local-decl 0 exact '14' - -- -q -e '/k { |a /t -- r| /t a 2 mul local-def t } def 7 k ='
# capacity suffix combined with a declared effect
run_case stack-effect-capacity 0 exact '14' - -- -q -e '/k2 { |a -- r|#4 a 2 mul } def 7 k2 ='
# `when` (single-arm match) is unanalyzable -> bails (accepted), runs fine
run_case stack-effect-when-bails 0 exact 'big' - -- -q -e '/w { |v -- r| v { dup 3 gt } { pop (big) } when } def 5 w ='
#--- stack-effect: inter-procedural (Phase 2) -- calls to already-bound procs are checked ---#
# caller verified through a call to a declared-effect proc (g is ( 2 -- 1 ))
run_case stack-effect-interproc-ok 0 exact '6' - -- -q -e '/g { |a b -- c| a b add } def /h { |x -- y| x x g } def 3 h ='
# callee has no declared effect -> its effect is inferred from its body and applied
run_case stack-effect-interproc-plain 0 exact '7' - -- -q -e '/inc { 1 add } def /add2 { |n -- r| n inc inc } def 5 add2 ='
# multi-output callee, both outputs consumed by the caller
run_case stack-effect-interproc-multiout 0 exact '8' - -- -q -e '/pair { |x -- a b| x x } def /sum2 { |x -- r| x pair add } def 4 sum2 ='
# three-deep call chain, each link declared and checked
run_case stack-effect-interproc-chain 0 exact '11' - -- -q -e '/a { |x -- y| x 1 add } def /b { |x -- y| x a } def /c { |x -- y| x b } def 10 c ='
# callee whose body is itself unanalyzable (exec splicer) -> the call bails, caller accepted
run_case stack-effect-interproc-callee-bails 0 exact '6' - -- -q -e '/runit { |p -- r| p exec } def /useit { |x -- r| x { 1 add } runit } def 5 useit ='
# mutual recursion: neither name is bound during its own scan -> bails, no false positive, no hang
run_case stack-effect-interproc-mutual 0 exact 'ok' - -- -q -e '/ping { |n -- r| n pong } def /pong { |n -- r| n ping } def (ok) ='

#--- stack-effect: local-def / store value-kind tracking ---#
# an undeclared (capacity-reserved) local-def'd VALUE resolves, so the body is checked
run_case stack-effect-localdef-value 0 exact '14' - -- -q -e '/v { |x -- r|#+1 /t x 2 mul local-def t } def 7 v ='
# a real mismatch is caught THROUGH a tracked value local (declares 2 outputs, leaves 1)
run_case stack-effect-localdef-mismatch stack-effect none '' - -- -q -e '{ |x -- a b|#+1 /t x 2 mul local-def t } pop'
# a local bound to a PROC auto-execs on a bare reference -> bails (no false positive), runs fine
run_case stack-effect-localdef-proc-bails 0 exact '6' - -- -q -e '/vp { |x -- r|#+1 /q { 1 add } local-def x q } def 5 vp ='
# regression: a DECLARED local bound to a proc, bare-ref'd via slot-ref, is accepted (was a false positive)
run_case stack-effect-localproc-accepted 0 exact 'ok' - -- -q -e '/f { |x /p -- | /p { pop } local-def x p } def 5 f (ok) ='

#--- stack-effect: more sad cases ---#
# body leaves fewer than declared outputs
run_case stack-effect-too-few-out stack-effect none '' - -- -q -e '{ |a b -- x y z| a b add } pop'
# repeat whose body is not stack-neutral
run_case stack-effect-repeat-imbalance stack-effect none '' - -- -q -e '{ |n -- r| n { 1 } repeat } pop'
# zero-output declaration but body leaves a value
run_case stack-effect-zero-out-violated stack-effect none '' - -- -q -e '{ |a -- | a } pop'
# inter-proc: callee changes the net so the caller's declared output is wrong
run_case stack-effect-interproc-mismatch stack-effect none '' - -- -q -e '/pair { |x -- a b| x x } def /bad { |x -- r| x pair } def'
# inter-proc: callee consumes more than the caller supplies -> caller underflow
run_case stack-effect-interproc-underflow stack-effect none '' - -- -q -e '/drop2 { |a b -- | } def /bad2 { |x -- r| x drop2 } def'

#--- stack-effect: stress (must never crash or false-positive; run under ASan) ---#
# abstract stack deeper than the cap -> bail (accept), not crash, not false error
SE_OVERFLOW="{ |a -- r| $(yes '1' | head -70 | tr '\n' ' ')}"
run_case stack-effect-overflow-bails 0 none '' - -- -q -e "$SE_OVERFLOW pop"
# deeply nested proc literal inside a declared-effect proc -> recursion stays bounded
SE_DEEP="{ |x -- r| $(printf '{%.0s' $(seq 1 60))$(printf '}%.0s' $(seq 1 60)) pop x }"
run_case stack-effect-deep-nest 0 none '' - -- -q -e "$SE_DEEP pop"
# many params/outputs (30) just under the cap -> fully analyzed and balanced
SE_NAMES="$(seq 1 30 | sed 's/^/p/' | tr '\n' ' ')"
run_case stack-effect-many-params 0 none '' - -- -q -e "{ |${SE_NAMES}-- ${SE_NAMES}| ${SE_NAMES}} pop"

#--- mode mutual exclusions (usage errors, exit 1) ---#
run_case stdin-excludes-i 1 exact 'trix: --stdin and -i/--stdedit are mutually exclusive' 'quit' -- --stdin -i
run_case stdin-excludes-file 1 exact 'trix: --stdin and filename are mutually exclusive' - -- --stdin "$CLI/ok.trx"
run_case image-requires-file 1 exact 'trix: -l/--image requires an image filename' - -- -l
run_case image-excludes-i 1 exact 'trix: --image and -i/--stdedit are mutually exclusive' - -- -l -i "$CLI/ok.trx"
run_case eval-twice 1 exact 'trix: -e/--eval may be given only once' - -- -e '1' -e '2'
run_case eval-excludes-image 1 exact 'trix: -e/--eval and -l/--image are mutually exclusive' - -- -e '1' -l
run_case eval-excludes-stdin 1 exact 'trix: -e/--eval and --stdin are mutually exclusive' - -- -e '1' --stdin
run_case check-excludes-image 1 exact 'trix: -c/--check cannot be combined with -l/--image' - -- -c -l "$CLI/ok.trx"
run_case check-excludes-i 1 exact 'trix: -c/--check cannot be combined with -i/--stdedit' - -- -c -i "$CLI/ok.trx"
run_case check-requires-source 1 exact 'trix: -c/--check requires a script file, --stdin, or -e/--eval' - -- -c
run_case check-excludes-resident 1 exact 'trix: -c/--check and --resident are mutually exclusive' - -- -c --resident "$CLI/ok.trx"

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
