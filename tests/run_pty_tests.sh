#!/bin/bash
# run_pty_tests.sh -- terminal-dependent scenarios under a real PTY.
#
# Covers the ops a pipe/null stdin cannot reach: the read-key blocking
# continuations (@read-key-retry, @read-key-timeout-retry need raw-mode,
# which requires a controlling tty) and the interactive REPL recovery
# pair (@repl-barrier / @repl-recover need the stdedit readline loop).
#
# Recipe: `script -qec "stty rows 24 cols 80; <cmd>" /dev/null` allocates
# the PTY (a fresh script PTY is 0x0 -- make-screen and friends raise
# /range-check without the stty); input is fed through script's stdin
# from a timed process substitution.  Assertions are marker greps on the
# combined capture -- raw PTY output carries echo and \r noise, so
# byte-exact golden diffs do not fit here.
#
# Exit code: 0 if every scenario passes, 1 otherwise.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TRIX="${TRIX_BIN:-./trix}"
cd "$PROJECT_DIR" || exit 2

if [ ! -x "$TRIX" ]; then
    echo "FATAL: trix binary not found or not executable: $TRIX" >&2
    exit 2
fi

# Symbolic Error-name -> exit-code resolution (sourced; auto-loads from $TRIX),
# so scenarios that assert a process exit code do not hardcode the number.
source "$SCRIPT_DIR/error_codes.sh"
if [ "${#TRIX_ERR_CODE[@]}" -eq 0 ]; then
    echo "FATAL: could not load error codes from '$TRIX --error-codes'" >&2
    exit 2
fi

if ! command -v script > /dev/null 2>&1; then
    echo "  SKIP  all pty scenarios -- 'script' (util-linux) not available"
    exit 0
fi

PASS=0
FAIL=0

# run_scenario name command feeder-proc timeout expected-marker...
run_scenario() {
    local name="$1" cmd="$2" feeder="$3" cap="$4"
    shift 4
    local out rc
    out="$(TERM=xterm-256color timeout -s KILL "$cap" \
           script -qec "stty rows 24 cols 80; $cmd" /dev/null < <($feeder) 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "  FAIL  $name -- exit $rc (expected 0; 137 = hang killed)"
        printf '%s\n' "$out" | tail -8 | sed 's/^/        /'
        FAIL=$((FAIL + 1))
        return
    fi
    local marker
    for marker in "$@"; do
        # a leading '!' inverts the assertion: the text must be ABSENT
        if [ "${marker#!}" != "$marker" ]; then
            if grep -qF "${marker#!}" <<< "$out"; then
                echo "  FAIL  $name -- forbidden marker present: ${marker#!}"
                printf '%s\n' "$out" | tail -12 | sed 's/^/        /'
                FAIL=$((FAIL + 1))
                return
            fi
        elif ! grep -qF "$marker" <<< "$out"; then
            echo "  FAIL  $name -- missing marker: $marker"
            printf '%s\n' "$out" | tail -12 | sed 's/^/        /'
            FAIL=$((FAIL + 1))
            return
        fi
    done
    echo "  ok    $name"
    PASS=$((PASS + 1))
}

# S1: read-key-byte blocks (no input pending), feeder sends 'A' after
# the block is established -> poll wake -> @read-key-retry.  `=` prints
# a Byte in simple form as its character, hence the 'A' marker.
feed_s1() { sleep 2; printf 'A'; sleep 3; }
run_scenario "read-key-block (@read-key-retry)" \
    "$TRIX tests/pty/read_key_block.trx" feed_s1 20 \
    "got-byte: A" "S1-done"

# S2: read-key-byte-timeout, both wake arms -- 'B' arrives inside the
# 8s deadline (poll wake), then a 400ms wait expires with no data
# (timer wake -> false).
feed_s2() { sleep 2; printf 'B'; sleep 4; }
run_scenario "read-key-timeout (@read-key-timeout-retry)" \
    "$TRIX tests/pty/read_key_timeout.trx" feed_s2 25 \
    "t1-byte: B" "t2-none" "S2-done"

# S3: interactive REPL -- an error at the prompt unwinds to @repl-barrier,
# which try_catch_handler rewrites to @repl-recover (prints the error and
# re-arms the prompt); the next line proves the session survived; ctrl-D
# EOF drains through @repl-barrier to Quit (clean exit 0).
# Invoked as `trix -i` (explicit REPL request; bare `trix` reaches the
# same mode), and NOT --quiet: that flag silences ALL diagnostics
# (fuzz-harness contract), including the @repl-recover error report
# this scenario asserts on.
feed_s3() {
    sleep 2
    printf '1 0 div\n'
    sleep 2
    printf '(repl-recovered) =\n'
    sleep 2
    printf '\004'
    sleep 2
}
run_scenario "repl-recover (@repl-barrier/@repl-recover)" \
    "$TRIX -i" feed_s3 30 \
    "div-by-zero" "repl-recovered"

# S4: --no-banner suppresses ONLY the banner -- the REPL prompt still
# appears and diagnostics (the @repl-recover error report) still print.
# Contrast with --quiet, which silences the diagnostics too.
feed_s4() {
    sleep 2
    printf '1 0 div\n'
    sleep 2
    printf '(no-banner-works) =\n'
    sleep 2
    printf '\004'
    sleep 1
}
run_scenario "no-banner (banner off, diagnostics on)" \
    "$TRIX -i --no-banner" feed_s4 30 \
    "div-by-zero" "no-banner-works" '!ctrl-D or '

# S5: single-reader discipline -- a spawned coroutine blocks on stdin,
# then the main coroutine's read-key-byte reaches the same block path
# and raises /invalid-access; the feeder byte then releases the child.
feed_s5() { sleep 3; printf 'C'; sleep 3; }
run_scenario "read-key-conflict (single stdin reader)" \
    "$TRIX tests/pty/read_key_conflict.trx" feed_s5 25 \
    "conflict-detected" "child-byte: C" "S5-done" "!NO-CONFLICT"

# S6: coroutine parked in read-key-byte while main cycles 20ms sleep
# timers.  Regression gate: an early real-sleep wake (poll ms truncation
# or EINTR) must loop back to waiting, not fall through and load a
# coroutine still linked in the timer list (list cycle -> dead keyboard
# at 99% CPU).  The 'a' (byte 97) arrives well past the pre-fix
# corruption window.
feed_s6() { sleep 2; printf 'a'; sleep 3; }
run_scenario "coro-key-sleeppoll (timer-mix scheduler wake)" \
    "$TRIX tests/pty/coro_key_sleeppoll.trx" feed_s6 20 \
    "got-byte: 97" "S6-done"

# S7: same regression gate from the ACTOR context -- the tetrix/chip8
# input-actor shape the dead-keyboard reports came from.
feed_s7() { sleep 2; printf 'a'; sleep 3; }
run_scenario "actor-key (actor reader + timer mix)" \
    "$TRIX tests/pty/actor_key.trx" feed_s7 20 \
    "actor-got: 97" "S7-done"

# S8: read-key-byte-timeout loop -- every 50ms timer wake must release
# the single-reader slot or the next call raises /invalid-access against
# its own stale registration.  ~40 timeout cycles before the byte.
feed_s8() { sleep 2; printf 'a'; sleep 3; }
run_scenario "read-key-timeout-loop (slot re-registration)" \
    "$TRIX tests/pty/read_key_timeout_loop.trx" feed_s8 20 \
    "got-byte: 97" "S8-done"

# S11: --inspect-on-error must drop into the inspector at the error
# (ENGINE BUG #15: the break-on-error flag had no engine reader, so this
# path silently never halted).  Three signals: the FATAL status bar
# rendered, q ended the session, and the error then finished on the
# real terminal with its diagnostic and the /undefined exit code (the
# `test $? -eq <undefined>` wrapper maps the expected code to 0 for the
# harness; the code is resolved symbolically so an enum renumber follows).
if "$TRIX" --version 2>/dev/null | grep -q ' debugger'; then
    feed_s11() { sleep 4; printf 'q'; sleep 2; }
    run_scenario "inspect-on-error (S11: inspector halts at fatal error)" \
        "$TRIX --inspect-on-error tests/pty/break_on_error.trx; test \$? -eq $(err undefined)" feed_s11 20 \
        "FATAL error" "before-error" "no-such-op-zzz is not associated"
else
    echo "  SKIP  inspect-on-error -- no debugger feature in this build"
fi

# S9/S10: progress gates -- the showcase games must actually PLAY
# themselves.  Two signals: (1) the quit key works (a wedged scheduler
# has a dead keyboard -- the process outlives the feeder and is killed,
# rc 137), and (2) the capture carries real diff volume (a wedge
# renders one static frame, ~3-6KB; healthy runs stream multiples of
# that).  ENGINE BUGS #10-#13 all presented here first: the showcases
# are the only programs exercising the full actor/timer/stdin mix, and
# these gates keep them load-bearing.
run_progress() {
    local name="$1" cmd="$2" feeder="$3" cap="$4" minbytes="$5"
    local out rc bytes
    out="$(TERM=xterm-256color timeout -s KILL "$cap" \
           script -qec "stty rows 30 cols 100; $cmd" /dev/null < <($feeder) 2>&1)"
    rc=$?
    bytes=${#out}
    if [ "$rc" -ne 0 ]; then
        echo "  FAIL  $name -- exit $rc (expected 0; 137 = quit key dead / hang)"
        FAIL=$((FAIL + 1))
        return
    fi
    if [ "$bytes" -lt "$minbytes" ]; then
        echo "  FAIL  $name -- $bytes bytes of output (< $minbytes): no visible progress"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  ok    $name (${bytes} bytes)"
    PASS=$((PASS + 1))
}

if [ -x ./tetrix ]; then
    # AI plays for ~8s (native kernel), then q must quit cleanly.
    # Healthy: ~20KB+ of board diffs.  Wedged: ~6KB static frame.
    feed_s9() { sleep 8; printf 'q'; sleep 2; }
    run_progress "tetrix-ai-progress (game plays itself)" \
        "./tetrix --ai-peek" feed_s9 16 12000
else
    echo "  SKIP  tetrix-ai-progress -- ./tetrix not built"
fi

if [ -x ./chip8 ]; then
    # Drive the emulator with whatever ROM is present; ESC must quit
    # cleanly.  The repo bundles no ROMs -- run examples/chip8-roms/
    # fetch-ch8.py first (the alphabetically-first CC0 ROM, 1dcell.ch8, is
    # a continuously-drawing automaton ideal for this size check).
    # Healthy: ~3.8-4.8KB (PTY timing varies).  Wedged: ~2.5KB initial frame.
    chip8_rom=$(ls examples/chip8-roms/*.ch8 2>/dev/null | head -1)
    if [ -n "$chip8_rom" ]; then
        feed_s10() { sleep 6; printf '\033'; sleep 2; }
        run_progress "chip8-progress (emulator runs + ESC quits)" \
            "./chip8 --cpu-kernel=native --ips=1500 $chip8_rom" feed_s10 14 3000
    else
        echo "  SKIP  chip8-progress -- no ROM (run examples/chip8-roms/fetch-ch8.py)"
    fi
else
    echo "  SKIP  chip8-progress -- ./chip8 not built"
fi

echo "===================================="
echo "  PTY results: $((PASS + FAIL)) scenarios, ${PASS} passed, ${FAIL} failed"
echo "===================================="

[ "$FAIL" -eq 0 ] || exit 1
exit 0
