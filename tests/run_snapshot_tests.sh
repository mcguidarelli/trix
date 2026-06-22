#!/bin/bash
# ====================================================================
# run_snapshot_tests.sh -- orchestrated snap-shot / thaw test suite
# ====================================================================
# Runs multi-phase tests that require separate ./trix invocations.
# Exit code: 0 if all tests pass, 1 if any fail.

set -euo pipefail

PASS=0
FAIL=0
PS=./trix
TMPDIR=/tmp/trix_snap_test_$$

mkdir -p "$TMPDIR"

cleanup() {
    rm -rf "$TMPDIR"
    rm -f tests/snapshot.img tests/actor_snapshot.img tests/supervision_snapshot.img \
        tests/memory_stream_snapshot.img tests/eqref_binding_snapshot.img tests/frame_dict_snapshot.img
}
trap cleanup EXIT

pass() {
    echo "  PASS: $1"
    PASS=$((PASS + 1))
}

fail() {
    echo "  FAIL: $1"
    FAIL=$((FAIL + 1))
}

# Verify binary exists
if [ ! -x "$PS" ]; then
    echo "ERROR: $PS not found. Run ./build.sh first."
    exit 1
fi

echo "===================================="
echo "  Snap-shot / Thaw Orchestrated Tests"
echo "===================================="

# ====================================================================
# TEST GROUP 1: Round-Trip Verification
# ====================================================================
echo ""
echo "--- Group 1: Round-Trip Verification ---"

# Phase 1: run test_snapshot.trx (creates tests/snapshot.img)
if $PS tests/test_snapshot.trx > "$TMPDIR/phase1.out" 2>"$TMPDIR/phase1.err"; then
    pass "phase-1 (snap-shot) succeeded"
else
    fail "phase-1 (snap-shot) failed"
    cat "$TMPDIR/phase1.err" >&2
fi

# Check no FAILED lines in phase-1
if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/phase1.out" 2>/dev/null; then
    fail "phase-1 has test failures"
    grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/phase1.out"
else
    pass "phase-1 all assertions passed"
fi

# Phase 2: thaw and re-run assertions
if [ -f tests/snapshot.img ]; then
    if $PS tests/snapshot_thaw.trx > "$TMPDIR/phase2.out" 2>"$TMPDIR/phase2.err"; then
        pass "phase-2 (thaw) succeeded"
    else
        fail "phase-2 (thaw) failed"
        cat "$TMPDIR/phase2.err" >&2
    fi

    # Check no FAILED lines in phase-2
    if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/phase2.out" 2>/dev/null; then
        fail "phase-2 has test failures"
        grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/phase2.out"
    else
        pass "phase-2 all assertions passed"
    fi

    # Phase-2 output should match phase-1 post-snap-shot output.
    # Post-snap-shot assertion lines start with "s:" prefix.
    grep "^s:" "$TMPDIR/phase1.out" > "$TMPDIR/phase1_snap.out" 2>/dev/null || true
    grep "^s:" "$TMPDIR/phase2.out" > "$TMPDIR/phase2_snap.out" 2>/dev/null || true

    if [ -s "$TMPDIR/phase2_snap.out" ] && diff -q "$TMPDIR/phase1_snap.out" "$TMPDIR/phase2_snap.out" > /dev/null 2>&1; then
        pass "phase-2 output matches phase-1 post-snap-shot"
    else
        fail "phase-2 output differs from phase-1 post-snap-shot"
        diff "$TMPDIR/phase1_snap.out" "$TMPDIR/phase2_snap.out" || true
    fi
else
    fail "tests/snapshot.img not created"
fi

# ====================================================================
# TEST GROUP 1b: Actor Mailbox Round-Trip
# ====================================================================
echo ""
echo "--- Group 1b: Actor Mailbox Round-Trip ---"

# Phase 1: spawn actor, fill mailbox, snap-shot
if $PS tests/test_actor_snapshot.trx > "$TMPDIR/actor_p1.out" 2>"$TMPDIR/actor_p1.err"; then
    pass "actor phase-1 (snap-shot) succeeded"
else
    fail "actor phase-1 (snap-shot) failed"
    cat "$TMPDIR/actor_p1.err" >&2
fi

if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/actor_p1.out" 2>/dev/null; then
    fail "actor phase-1 has test failures"
    grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/actor_p1.out"
else
    pass "actor phase-1 all assertions passed"
fi

# Phase 2: thaw and re-run
if [ -f tests/actor_snapshot.img ]; then
    if $PS tests/actor_snapshot_thaw.trx > "$TMPDIR/actor_p2.out" 2>"$TMPDIR/actor_p2.err"; then
        pass "actor phase-2 (thaw) succeeded"
    else
        fail "actor phase-2 (thaw) failed"
        cat "$TMPDIR/actor_p2.err" >&2
    fi

    if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/actor_p2.out" 2>/dev/null; then
        fail "actor phase-2 has test failures"
        grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/actor_p2.out"
    else
        pass "actor phase-2 all assertions passed"
    fi

    # Output should match between phases
    grep "^sa:" "$TMPDIR/actor_p1.out" > "$TMPDIR/actor_p1_snap.out" 2>/dev/null || true
    grep "^sa:" "$TMPDIR/actor_p2.out" > "$TMPDIR/actor_p2_snap.out" 2>/dev/null || true

    if [ -s "$TMPDIR/actor_p2_snap.out" ] && diff -q "$TMPDIR/actor_p1_snap.out" "$TMPDIR/actor_p2_snap.out" > /dev/null 2>&1; then
        pass "actor phase-2 output matches phase-1"
    else
        fail "actor phase-2 output differs from phase-1"
        diff "$TMPDIR/actor_p1_snap.out" "$TMPDIR/actor_p2_snap.out" || true
    fi

    rm -f tests/actor_snapshot.img
else
    fail "tests/actor_snapshot.img not created"
fi

# ====================================================================
# TEST GROUP 1c: Supervision Round-Trip
# ====================================================================
echo ""
echo "--- Group 1c: Supervision Round-Trip ---"

# Phase 1: create supervisor with children, snap-shot
if $PS tests/test_supervision_snapshot.trx > "$TMPDIR/sup_p1.out" 2>"$TMPDIR/sup_p1.err"; then
    pass "supervision phase-1 (snap-shot) succeeded"
else
    fail "supervision phase-1 (snap-shot) failed"
    cat "$TMPDIR/sup_p1.err" >&2
fi

if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/sup_p1.out" 2>/dev/null; then
    fail "supervision phase-1 has test failures"
    grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/sup_p1.out"
else
    pass "supervision phase-1 all assertions passed"
fi

# Phase 2: thaw and re-run
if [ -f tests/supervision_snapshot.img ]; then
    if $PS tests/supervision_snapshot_thaw.trx > "$TMPDIR/sup_p2.out" 2>"$TMPDIR/sup_p2.err"; then
        pass "supervision phase-2 (thaw) succeeded"
    else
        fail "supervision phase-2 (thaw) failed"
        cat "$TMPDIR/sup_p2.err" >&2
    fi

    if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/sup_p2.out" 2>/dev/null; then
        fail "supervision phase-2 has test failures"
        grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/sup_p2.out"
    else
        pass "supervision phase-2 all assertions passed"
    fi

    # Output should match between phases
    grep "^ss:" "$TMPDIR/sup_p1.out" > "$TMPDIR/sup_p1_snap.out" 2>/dev/null || true
    grep "^ss:" "$TMPDIR/sup_p2.out" > "$TMPDIR/sup_p2_snap.out" 2>/dev/null || true

    if [ -s "$TMPDIR/sup_p2_snap.out" ] && diff -q "$TMPDIR/sup_p1_snap.out" "$TMPDIR/sup_p2_snap.out" > /dev/null 2>&1; then
        pass "supervision phase-2 output matches phase-1"
    else
        fail "supervision phase-2 output differs from phase-1"
        diff "$TMPDIR/sup_p1_snap.out" "$TMPDIR/sup_p2_snap.out" || true
    fi

    rm -f tests/supervision_snapshot.img
else
    fail "tests/supervision_snapshot.img not created"
fi

# ====================================================================
# TEST GROUP 1d: Per-coroutine Binding Cache Round-Trip
# ====================================================================
echo ""
echo "--- Group 1d: Per-coroutine Binding Cache Round-Trip ---"

# Phase 1: spawn actor with populated binding_table, snap-shot
if $PS tests/test_eqref_binding_snapshot.trx > "$TMPDIR/eqb_p1.out" 2>"$TMPDIR/eqb_p1.err"; then
    pass "binding-cache phase-1 (snap-shot) succeeded"
else
    fail "binding-cache phase-1 (snap-shot) failed"
    cat "$TMPDIR/eqb_p1.err" >&2
fi

if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/eqb_p1.out" 2>/dev/null; then
    fail "binding-cache phase-1 has test failures"
    grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/eqb_p1.out"
else
    pass "binding-cache phase-1 all assertions passed"
fi

# Phase 2: thaw and re-run
if [ -f tests/eqref_binding_snapshot.img ]; then
    if $PS tests/eqref_binding_snapshot_thaw.trx > "$TMPDIR/eqb_p2.out" 2>"$TMPDIR/eqb_p2.err"; then
        pass "binding-cache phase-2 (thaw) succeeded"
    else
        fail "binding-cache phase-2 (thaw) failed"
        cat "$TMPDIR/eqb_p2.err" >&2
    fi

    if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/eqb_p2.out" 2>/dev/null; then
        fail "binding-cache phase-2 has test failures"
        grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/eqb_p2.out"
    else
        pass "binding-cache phase-2 all assertions passed"
    fi

    # Output should match between phases
    grep "^sb:" "$TMPDIR/eqb_p1.out" > "$TMPDIR/eqb_p1_snap.out" 2>/dev/null || true
    grep "^sb:" "$TMPDIR/eqb_p2.out" > "$TMPDIR/eqb_p2_snap.out" 2>/dev/null || true

    if [ -s "$TMPDIR/eqb_p2_snap.out" ] && diff -q "$TMPDIR/eqb_p1_snap.out" "$TMPDIR/eqb_p2_snap.out" > /dev/null 2>&1; then
        pass "binding-cache phase-2 output matches phase-1"
    else
        fail "binding-cache phase-2 output differs from phase-1"
        diff "$TMPDIR/eqb_p1_snap.out" "$TMPDIR/eqb_p2_snap.out" || true
    fi

    rm -f tests/eqref_binding_snapshot.img
else
    fail "tests/eqref_binding_snapshot.img not created"
fi

# ====================================================================
# TEST GROUP 1e: Frame-Dict Pool Round-Trip
# ====================================================================
echo ""
echo "--- Group 1e: Frame-Dict Pool Round-Trip ---"

# Phase 1: populate m_frame_dict_pool via |locals|#N proc returns, snap-shot
if $PS tests/test_frame_dict_snapshot.trx > "$TMPDIR/fdp_p1.out" 2>"$TMPDIR/fdp_p1.err"; then
    pass "frame-dict phase-1 (snap-shot) succeeded"
else
    fail "frame-dict phase-1 (snap-shot) failed"
    cat "$TMPDIR/fdp_p1.err" >&2
fi

if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/fdp_p1.out" 2>/dev/null; then
    fail "frame-dict phase-1 has test failures"
    grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/fdp_p1.out"
else
    pass "frame-dict phase-1 all assertions passed"
fi

# Phase 2: thaw and re-run post-snap-shot assertions
if [ -f tests/frame_dict_snapshot.img ]; then
    if $PS tests/frame_dict_snapshot_thaw.trx > "$TMPDIR/fdp_p2.out" 2>"$TMPDIR/fdp_p2.err"; then
        pass "frame-dict phase-2 (thaw) succeeded"
    else
        fail "frame-dict phase-2 (thaw) failed"
        cat "$TMPDIR/fdp_p2.err" >&2
    fi

    if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/fdp_p2.out" 2>/dev/null; then
        fail "frame-dict phase-2 has test failures"
        grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/fdp_p2.out"
    else
        pass "frame-dict phase-2 all assertions passed"
    fi

    # Output should match between phases (sf: prefix only; sfp: is
    # phase-1-only and expected not to appear in phase 2).
    grep "^sf:" "$TMPDIR/fdp_p1.out" > "$TMPDIR/fdp_p1_snap.out" 2>/dev/null || true
    grep "^sf:" "$TMPDIR/fdp_p2.out" > "$TMPDIR/fdp_p2_snap.out" 2>/dev/null || true

    if [ -s "$TMPDIR/fdp_p2_snap.out" ] && diff -q "$TMPDIR/fdp_p1_snap.out" "$TMPDIR/fdp_p2_snap.out" > /dev/null 2>&1; then
        pass "frame-dict phase-2 output matches phase-1"
    else
        fail "frame-dict phase-2 output differs from phase-1"
        diff "$TMPDIR/fdp_p1_snap.out" "$TMPDIR/fdp_p2_snap.out" || true
    fi

    rm -f tests/frame_dict_snapshot.img
else
    fail "tests/frame_dict_snapshot.img not created"
fi

# ====================================================================
# TEST GROUP 1f: Borrowed Memory-Stream (make-memory-stream) Round-Trip
# ====================================================================
echo ""
echo "--- Group 1f: Borrowed Memory-Stream Round-Trip ---"

# Phase 1: build a 256-byte payload, wrap it via make-memory-stream,
# drain the first 64 bytes through the regular read path, snap-shot.
if $PS tests/test_memory_stream_snapshot.trx > "$TMPDIR/mms_p1.out" 2>"$TMPDIR/mms_p1.err"; then
    pass "memory-stream phase-1 (snap-shot) succeeded"
else
    fail "memory-stream phase-1 (snap-shot) failed"
    cat "$TMPDIR/mms_p1.err" >&2
fi

if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/mms_p1.out" 2>/dev/null; then
    fail "memory-stream phase-1 has test failures"
    grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/mms_p1.out"
else
    pass "memory-stream phase-1 all assertions passed"
fi

# Phase 2: thaw and re-run the post-snap-shot assertions.  After thaw
# the borrowed memory-stream's tail should be reachable through the
# regular read path; phase-2 checks tail length, byte-exact equality
# with payload[64..256), and post-tail EOF.
if [ -f tests/memory_stream_snapshot.img ]; then
    if $PS tests/memory_stream_snapshot_thaw.trx > "$TMPDIR/mms_p2.out" 2>"$TMPDIR/mms_p2.err"; then
        pass "memory-stream phase-2 (thaw) succeeded"
    else
        fail "memory-stream phase-2 (thaw) failed"
        cat "$TMPDIR/mms_p2.err" >&2
    fi

    if grep -qE "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/mms_p2.out" 2>/dev/null; then
        fail "memory-stream phase-2 has test failures"
        grep -E "^FAILED:|^FAIL #|^[[:space:]]*FAIL:|FAILURES = [1-9]" "$TMPDIR/mms_p2.out"
    else
        pass "memory-stream phase-2 all assertions passed"
    fi

    # Compare the post-snapshot section across phases.  Phase-1 lines
    # before the snap-shot use a different prefix; the post-snapshot
    # checks all start with "s: ".
    grep "^s: " "$TMPDIR/mms_p1.out" | tail -3 > "$TMPDIR/mms_p1_snap.out" 2>/dev/null || true
    grep "^s: " "$TMPDIR/mms_p2.out"           > "$TMPDIR/mms_p2_snap.out" 2>/dev/null || true

    if [ -s "$TMPDIR/mms_p2_snap.out" ] && diff -q "$TMPDIR/mms_p1_snap.out" "$TMPDIR/mms_p2_snap.out" > /dev/null 2>&1; then
        pass "memory-stream phase-2 output matches phase-1 post-snap-shot"
    else
        fail "memory-stream phase-2 output differs from phase-1 post-snap-shot"
        diff "$TMPDIR/mms_p1_snap.out" "$TMPDIR/mms_p2_snap.out" || true
    fi

    rm -f tests/memory_stream_snapshot.img
else
    fail "tests/memory_stream_snapshot.img not created"
fi

# ====================================================================
# TEST GROUP 2: User File Stream Reconnection
# ====================================================================
echo ""
echo "--- Group 2: User File Stream Reconnection ---"

# --- 2a: Read mode: open file, advance, snap-shot, read more after thaw ---
printf 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' > "$TMPDIR/ufs_read_data.txt"

cat > "$TMPDIR/ufs_read_phase1.trx" << SCRIPTEOF
($TMPDIR/ufs_read_data.txt) (r)#b stream
/s exch def
10 string s exch read-string pop pop
($TMPDIR/ufs_read.img) snap-shot
5 string s exch read-string pop =
clear
SCRIPTEOF

cat > "$TMPDIR/ufs_read_phase2.trx" << SCRIPTEOF
($TMPDIR/ufs_read.img) thaw
SCRIPTEOF

if $PS "$TMPDIR/ufs_read_phase1.trx" > "$TMPDIR/ufs_read_p1.out" 2>&1; then
    P1_OUT=$(cat "$TMPDIR/ufs_read_p1.out")
    if [ "$P1_OUT" = "KLMNO" ]; then
        pass "UFS read phase-1 correct"
    else
        fail "UFS read phase-1: expected 'KLMNO', got '$P1_OUT'"
    fi
else
    fail "UFS read phase-1 failed"
fi

if [ -f "$TMPDIR/ufs_read.img" ]; then
    if $PS "$TMPDIR/ufs_read_phase2.trx" > "$TMPDIR/ufs_read_p2.out" 2>&1; then
        P2_OUT=$(cat "$TMPDIR/ufs_read_p2.out")
        if [ "$P2_OUT" = "KLMNO" ]; then
            pass "UFS read phase-2 (thaw) correct"
        else
            fail "UFS read phase-2: expected 'KLMNO', got '$P2_OUT'"
        fi
    else
        fail "UFS read phase-2 failed"
    fi
else
    fail "UFS read .img not created"
fi

# --- 2b: Write mode: open file for writing, write, snap-shot, write more after thaw ---
cat > "$TMPDIR/ufs_write_phase1.trx" << SCRIPTEOF
($TMPDIR/ufs_write_out.txt) (w)#b stream
/s exch def
s (BEFORE) write-string
($TMPDIR/ufs_write.img) snap-shot
s (AFTER) write-string
s close-stream
clear
SCRIPTEOF

cat > "$TMPDIR/ufs_write_phase2.trx" << SCRIPTEOF
($TMPDIR/ufs_write.img) thaw
SCRIPTEOF

# Phase 1
if $PS "$TMPDIR/ufs_write_phase1.trx" > /dev/null 2>&1; then
    CONTENT=$(cat "$TMPDIR/ufs_write_out.txt" 2>/dev/null || echo "")
    if [ "$CONTENT" = "BEFOREAFTER" ]; then
        pass "UFS write phase-1 correct"
    else
        fail "UFS write phase-1: expected 'BEFOREAFTER', got '$CONTENT'"
    fi
else
    fail "UFS write phase-1 failed"
fi

# Phase 2: thaw re-opens at saved position, writes AFTER again
if [ -f "$TMPDIR/ufs_write.img" ]; then
    if $PS "$TMPDIR/ufs_write_phase2.trx" > /dev/null 2>"$TMPDIR/ufs_write_p2.err"; then
        CONTENT=$(cat "$TMPDIR/ufs_write_out.txt" 2>/dev/null || echo "")
        if echo "$CONTENT" | grep -q "AFTER"; then
            pass "UFS write phase-2 (thaw) wrote AFTER"
        else
            fail "UFS write phase-2: expected AFTER in output, got '$CONTENT'"
        fi
    else
        fail "UFS write phase-2 failed"
        cat "$TMPDIR/ufs_write_p2.err" >&2
    fi
else
    fail "UFS write .img not created"
fi

# --- 2c: Multiple streams: two files open simultaneously ---
printf 'AAAAAABBBBB' > "$TMPDIR/ufs_multi_a.txt"
printf '1111122222' > "$TMPDIR/ufs_multi_b.txt"

cat > "$TMPDIR/ufs_multi_phase1.trx" << SCRIPTEOF
($TMPDIR/ufs_multi_a.txt) (r)#b stream /sa exch def
($TMPDIR/ufs_multi_b.txt) (r)#b stream /sb exch def
6 string sa exch read-string pop pop
5 string sb exch read-string pop pop
($TMPDIR/ufs_multi.img) snap-shot
5 string sa exch read-string pop =
5 string sb exch read-string pop =
clear
SCRIPTEOF

cat > "$TMPDIR/ufs_multi_phase2.trx" << SCRIPTEOF
($TMPDIR/ufs_multi.img) thaw
SCRIPTEOF

if $PS "$TMPDIR/ufs_multi_phase1.trx" > "$TMPDIR/ufs_multi_p1.out" 2>&1; then
    EXPECTED=$'BBBBB\n22222'
    ACTUAL=$(cat "$TMPDIR/ufs_multi_p1.out")
    if [ "$ACTUAL" = "$EXPECTED" ]; then
        pass "UFS multi-stream phase-1 correct"
    else
        fail "UFS multi-stream phase-1: expected '$EXPECTED', got '$ACTUAL'"
    fi
else
    fail "UFS multi-stream phase-1 failed"
fi

if [ -f "$TMPDIR/ufs_multi.img" ]; then
    if $PS "$TMPDIR/ufs_multi_phase2.trx" > "$TMPDIR/ufs_multi_p2.out" 2>&1; then
        EXPECTED=$'BBBBB\n22222'
        ACTUAL=$(cat "$TMPDIR/ufs_multi_p2.out")
        if [ "$ACTUAL" = "$EXPECTED" ]; then
            pass "UFS multi-stream phase-2 (thaw) correct"
        else
            fail "UFS multi-stream phase-2: expected '$EXPECTED', got '$ACTUAL'"
        fi
    else
        fail "UFS multi-stream phase-2 failed"
    fi
else
    fail "UFS multi .img not created"
fi

# --- 2d: Startup-file tail: a startup script larger than the stream buffer is
#         snap-shot mid-read; the bytes past the buffer ("the tail") are
#         serialized as a memory block and, on thaw, must be executed from the
#         restored ext buffer (the original fd becomes /dev/null).  Forced with
#         --stream-buffer 128 so a tiny script has a tail, and the marker sits
#         well past byte 128 -- i.e. in the tail, not the in-buffer VM blob.
#         (snapshot_op streams the tail straight from the fd; this is the focused
#         check for that path, which test_snapshot.trx only exercises implicitly.)
cat > "$TMPDIR/tail_phase1.trx" << SCRIPTEOF
($TMPDIR/tail.img) snap-shot
% padding to push the marker past the 128-byte startup-stream buffer so it lands in the serialized tail, not the VM blob xxxxxxxxxxxxxxxxx
(TAIL_AFTER_BUFFER) print
SCRIPTEOF

cat > "$TMPDIR/tail_phase2.trx" << SCRIPTEOF
($TMPDIR/tail.img) thaw
SCRIPTEOF

# Phase 1 (live): snap-shot captures the tail, then the rest of the script runs
# from the real file and prints the marker.
if $PS --stream-buffer 128 "$TMPDIR/tail_phase1.trx" > "$TMPDIR/tail_p1.out" 2>&1 &&
    grep -q "TAIL_AFTER_BUFFER" "$TMPDIR/tail_p1.out"; then
    pass "startup-tail phase-1 ran the post-buffer marker"
else
    fail "startup-tail phase-1: marker missing (got '$(cat "$TMPDIR/tail_p1.out")')"
fi

# Phase 2 (thaw): the resumed exec stack reads the rest of the startup script
# from the RESTORED tail (its fd is now /dev/null), so the marker prints again.
# Non-vacuous: without tail restore the scanner hits EOF past the buffer and the
# marker never executes.
if [ -f "$TMPDIR/tail.img" ]; then
    if $PS "$TMPDIR/tail_phase2.trx" > "$TMPDIR/tail_p2.out" 2>&1 &&
        grep -q "TAIL_AFTER_BUFFER" "$TMPDIR/tail_p2.out"; then
        pass "startup-tail phase-2 (thaw) executed the restored tail"
    else
        fail "startup-tail phase-2: marker missing (got '$(cat "$TMPDIR/tail_p2.out")')"
    fi
else
    fail "startup-tail .img not created"
fi

# ====================================================================
# TEST GROUP 3: Corruption / Error Paths
# ====================================================================
echo ""
echo "--- Group 3: Corruption / Error Paths ---"

# Create a valid .img file to corrupt.
# Use startup_image mode (not thaw operator) so errors cause non-zero exit.
cat > "$TMPDIR/corrupt_base.trx" << 'SCRIPTEOF'
/x 1 def
(/tmp/trix_corrupt_base.img) snap-shot
clear
SCRIPTEOF

$PS "$TMPDIR/corrupt_base.trx" > /dev/null 2>&1 || true

if [ -f /tmp/trix_corrupt_base.img ]; then
    # --- 3a: Bad magic bytes ---
    cp /tmp/trix_corrupt_base.img "$TMPDIR/bad_magic.img"
    printf '\x00' | dd of="$TMPDIR/bad_magic.img" bs=1 count=1 conv=notrunc 2>/dev/null

    # Use startup_image mode: pass .img directly to Trix constructor.
    # startup_image prints error to stderr and returns false (exit 0 but no output).
    # Instead, use thaw operator with try to detect the error.
    cat > "$TMPDIR/bad_magic.trx" << SCRIPTEOF
{ ($TMPDIR/bad_magic.img) thaw } try
/invalid-image-file eq { (REJECTED) } { (UNEXPECTED) } if-else =
SCRIPTEOF

    OUT=$($PS "$TMPDIR/bad_magic.trx" 2>/dev/null || echo "CRASHED")
    if [ "$OUT" = "REJECTED" ]; then
        pass "bad magic: thaw rejected"
    else
        fail "bad magic: got '$OUT'"
    fi

    # --- 3b: Truncated file ---
    cp /tmp/trix_corrupt_base.img "$TMPDIR/truncated.img"
    truncate -s 100 "$TMPDIR/truncated.img"

    cat > "$TMPDIR/truncated.trx" << SCRIPTEOF
{ ($TMPDIR/truncated.img) thaw } try
/invalid-image-file eq { (REJECTED) } { (UNEXPECTED) } if-else =
SCRIPTEOF

    OUT=$($PS "$TMPDIR/truncated.trx" 2>/dev/null || echo "CRASHED")
    if [ "$OUT" = "REJECTED" ]; then
        pass "truncated: thaw rejected"
    else
        fail "truncated: got '$OUT'"
    fi

    # --- 3c: CRC corruption (flip a byte in the VM blob area) ---
    cp /tmp/trix_corrupt_base.img "$TMPDIR/bad_crc.img"
    FSIZE=$(stat -c %s "$TMPDIR/bad_crc.img")
    FLIP_POS=$((FSIZE - 100))
    if [ $FLIP_POS -gt 416 ]; then
        printf '\xFF' | dd of="$TMPDIR/bad_crc.img" bs=1 count=1 seek=$FLIP_POS conv=notrunc 2>/dev/null

        cat > "$TMPDIR/bad_crc.trx" << SCRIPTEOF
{ ($TMPDIR/bad_crc.img) thaw } try
/invalid-image-file eq { (REJECTED) } { (UNEXPECTED) } if-else =
SCRIPTEOF

        OUT=$($PS "$TMPDIR/bad_crc.trx" 2>/dev/null || echo "CRASHED")
        if [ "$OUT" = "REJECTED" ]; then
            pass "bad CRC: thaw rejected"
        else
            fail "bad CRC: got '$OUT'"
        fi
    else
        fail "bad CRC: .img file too small to corrupt VM blob"
    fi

    # --- 3d: Empty file ---
    > "$TMPDIR/empty.img"

    cat > "$TMPDIR/empty.trx" << SCRIPTEOF
{ ($TMPDIR/empty.img) thaw } try
/invalid-image-file eq { (REJECTED) } { (UNEXPECTED) } if-else =
SCRIPTEOF

    OUT=$($PS "$TMPDIR/empty.trx" 2>/dev/null || echo "CRASHED")
    if [ "$OUT" = "REJECTED" ]; then
        pass "empty file: thaw rejected"
    else
        fail "empty file: got '$OUT'"
    fi

    # --- 3e: Version mismatch (change major version byte) ---
    cp /tmp/trix_corrupt_base.img "$TMPDIR/bad_version.img"
    printf '\xFF' | dd of="$TMPDIR/bad_version.img" bs=1 count=1 seek=4 conv=notrunc 2>/dev/null

    cat > "$TMPDIR/bad_version.trx" << SCRIPTEOF
{ ($TMPDIR/bad_version.img) thaw } try
/invalid-image-file eq { (REJECTED) } { (UNEXPECTED) } if-else =
SCRIPTEOF

    OUT=$($PS "$TMPDIR/bad_version.trx" 2>/dev/null || echo "CRASHED")
    if [ "$OUT" = "REJECTED" ]; then
        pass "bad version: thaw rejected"
    else
        fail "bad version: got '$OUT'"
    fi

    rm -f /tmp/trix_corrupt_base.img
else
    fail "could not create base .img for corruption tests"
fi

# ====================================================================
# TEST GROUP 4: C++ Helper Tests (VM too small, user-op mismatch)
# ====================================================================
echo ""
echo "--- Group 4: C++ Helper Tests ---"

# Build the helper binary.  It is a standalone C++23 program (it builds its own
# Trix, not linked against the trix under test), so any C++23 compiler works.
# Honor $CXX, else pick the first available C++ driver -- the clang CI leg has
# no gcc-15, so the compiler must NOT be hardcoded.  A C++ driver auto-links the
# standard libraries; only -lstdc++exp (std::print) needs to be explicit.
HELPER=snapshot_test_helper
HELPER_CXX="${CXX:-}"
if [ -z "$HELPER_CXX" ]; then
    for c in g++-15 clang++-20 g++ c++; do
        command -v "$c" >/dev/null 2>&1 && { HELPER_CXX="$c"; break; }
    done
fi
if "$HELPER_CXX" -O0 -g -std=c++23 \
    -DTRX_DEBUG \
    -fno-rtti -fexceptions -fsanitize=address -fsanitize=undefined \
    -pthread -I. \
    -o "$TMPDIR/$HELPER" tests/snapshot_test_helper.cpp \
    -lreadline -lz -lm -lpthread -lrt -lstdc++exp \
    2>"$TMPDIR/helper_build.err"; then
    pass "helper binary compiled ($HELPER_CXX)"

    # Create a .img from the full-size VM (1MB) for mismatch tests.  The big
    # array pushes vm_used above 256KB so the --small-vm (256KB) thaw trips the
    # capacity check; a trivial image's vm_used stays under 256KB and would
    # instead fall through to the user-op-CRC path, duplicating --wrong-ops.
    cat > "$TMPDIR/helper_snap.trx" << SCRIPTEOF
/big 30000 array def
($TMPDIR/helper.img) snap-shot
clear
SCRIPTEOF
    $PS "$TMPDIR/helper_snap.trx" > /dev/null 2>&1 || true

    if [ -f "$TMPDIR/helper.img" ]; then
        # --- 4a: VM too small ---
        if "$TMPDIR/$HELPER" --small-vm "$TMPDIR/helper.img" > /dev/null 2>"$TMPDIR/smallvm.err"; then
            fail "small-vm: thaw should have failed"
        else
            pass "small-vm: thaw rejected (VM capacity mismatch)"
        fi

        # --- 4b: User-op mismatch ---
        if "$TMPDIR/$HELPER" --wrong-ops "$TMPDIR/helper.img" > /dev/null 2>"$TMPDIR/wrongops.err"; then
            fail "wrong-ops: thaw should have failed"
        else
            pass "wrong-ops: thaw rejected (user-op CRC mismatch)"
        fi
    else
        fail "could not create helper.img"
    fi
else
    fail "helper binary failed to compile"
    cat "$TMPDIR/helper_build.err" >&2
fi

# ====================================================================
# TEST GROUP: Adversarial images (cross-version / corruption matrix)
# ====================================================================
# Pristine fixture images are byte-patched into precisely-corrupted
# variants by tests/snapshot/patch_image.py; each variant must make the
# runtime `thaw` operator fail with its specific validation arm (the
# uncaught error prints the arm's message and exits non-zero) while a
# pristine control still thaws.  patch_image.py self-calibrates the
# header size / checksum offset against the pristine image and fails
# loudly on snapshot-format drift.
echo ""
echo "--- Group: Adversarial images ---"

ADV="$TMPDIR/adv"
mkdir -p "$ADV"
TRIX_ABS="$PWD/${PS#./}"
PATCH="python3 tests/snapshot/patch_image.py"

# adv_thaw <name> <image> <expected-message-fragment>
# Thaw WITHOUT try: the arm's diag prints and the process exits
# non-zero.  (try-caught errors are silent, so the per-arm message is
# only observable on the uncaught path.)
adv_thaw() {
    local out rc
    set +e
    out=$(printf '(%s) thaw (THAW-UNEXPECTEDLY-OK) =\n' "$2" | "$TRIX_ABS" --vm-size=2M --stdin 2>&1)
    rc=$?
    set -e
    if [ "$rc" -ne 0 ] && grep -qF "$3" <<< "$out"; then
        pass "adv: $1"
    else
        fail "adv: $1 (rc=$rc, missing '$3')"
        printf '%s\n' "$out" | head -3 >&2
    fi
}

# --- Fixtures.  --stdin runs produce tail-free images (no startup
# file), which is what the patcher's layout calibration relies on.
printf '/marker 42 def (%s) snap-shot\n' "$ADV/plain.img" | "$TRIX_ABS" -q --vm-size=2M --stdin
"$TRIX_ABS" -q --vm-size=2M --stdin <<EOF
/payload 20000 string def
0 1 19999 {|i| payload i  i 255 mod /byte-type cast  put} for
/mms payload make-memory-stream def
/prefix 64 string def
mms prefix read-string pop pop
($ADV/mem.img) snap-shot
EOF
printf 'snapshot-ufs-payload-0123456789\n' > "$ADV/ufs_data.txt"
"$TRIX_ABS" -q --vm-size=2M --stdin <<EOF
/ufs ($ADV/ufs_data.txt) (r)#b stream def
/buf 8 string def
ufs buf read-string pop pop
($ADV/ufs.img) snap-shot
EOF
printf 'mark 45 { 60000 string } repeat array-from-mark /big exch def (%s) snap-shot\n' "$ADV/big.img" \
    | "$TRIX_ABS" -q --vm-size=4M --stdin

if [ ! -s "$ADV/plain.img" ] || [ ! -s "$ADV/mem.img" ] || [ ! -s "$ADV/ufs.img" ] || [ ! -s "$ADV/big.img" ]; then
    fail "adv: fixture image creation failed"
else
    pass "adv: fixture images created"

    # --- Calibration on the section-free image, then CRC-verify the
    # section-bearing fixtures against the same structural offsets.
    if CALIB=$($PATCH calibrate "$ADV/plain.img") &&
        HDR=${CALIB% *} && CS=${CALIB#* } &&
        $PATCH verify "$ADV/mem.img" "$CS" &&
        $PATCH verify "$ADV/ufs.img" "$CS"; then
        pass "adv: patcher calibrated (header=$HDR checksum-offset=$CS)"

        # --- Pristine control: an unpatched image must thaw cleanly --
        # the resumed exec stack (frozen mid-snap-shot, --stdin mode)
        # runs to EOF and exits 0 with no thaw diagnostic.  (Successful
        # resume OUTPUT is pinned by the -l round trip in
        # run_cli_tests.sh; here the control proves the corrupted
        # variants fail because of their patches, not the harness.)
        set +e
        CTL_OUT=$(printf '(%s) thaw\n' "$ADV/plain.img" | "$TRIX_ABS" -q --vm-size=2M --stdin 2>&1)
        CTL_RC=$?
        set -e
        if [ "$CTL_RC" -eq 0 ] && ! grep -q "thaw:" <<< "$CTL_OUT"; then
            pass "adv: pristine image thaws (control)"
        else
            fail "adv: pristine image failed to thaw (rc=$CTL_RC)"
            printf '%s\n' "$CTL_OUT" | head -3 >&2
        fi

        # --- Header-field arms (validated before the overall CRC).
        $PATCH apply "$ADV/plain.img" "$ADV/badver.img"   "$CS" patch:4:EE
        adv_thaw bad-version "$ADV/badver.img" "invalid snap-shot file (bad magic or version)"
        $PATCH apply "$ADV/plain.img" "$ADV/badmagic.img" "$CS" xor:0:FF
        adv_thaw bad-magic "$ADV/badmagic.img" "bad magic or version"
        $PATCH apply "$ADV/plain.img" "$ADV/badend.img"   "$CS" xor:8:01
        adv_thaw endian-mismatch "$ADV/badend.img" "thaw: endian mismatch"
        $PATCH apply "$ADV/plain.img" "$ADV/vmhuge.img"   "$CS" p32:24:0xFFFFFF00
        adv_thaw vm-used-huge "$ADV/vmhuge.img" "thaw: VM too small"
        $PATCH apply "$ADV/plain.img" "$ADV/gcap.img"     "$CS" p32:28:64 p32:32:12345
        adv_thaw global-capacity-mismatch "$ADV/gcap.img" "VM capacity mismatch with global region present"
        $PATCH apply "$ADV/plain.img" "$ADV/gover.img"    "$CS" p32:28:0xFFFFFF00
        adv_thaw local-plus-global-over "$ADV/gover.img" "exceeds VM capacity"
        # The two stream-count fields sit one extra 8 bytes back from the
        # checksum since v174 (operator_table_signature + operator_count occupy
        # CS-8 / CS-4): mem count is CS-24, user-file count CS-16.
        $PATCH apply "$ADV/plain.img" "$ADV/memcnt.img"   "$CS" p32:$((CS - 24)):300
        adv_thaw mem-count-over-max "$ADV/memcnt.img" "memory stream count 300 exceeds maximum"
        $PATCH apply "$ADV/plain.img" "$ADV/ufscnt.img"   "$CS" p32:$((CS - 16)):300
        adv_thaw ufs-count-over-max "$ADV/ufscnt.img" "user file stream count 300 exceeds maximum"
        # v174 build-compatibility guard: corrupt operator_table_signature
        # (CS-8) and the image must be rejected as an incompatible build.
        $PATCH apply "$ADV/plain.img" "$ADV/badsig.img"   "$CS" xor:$((CS - 8)):FF
        adv_thaw operator-signature-mismatch "$ADV/badsig.img" "incompatible Trix"

        # --- Truncations and broad corruption.
        $PATCH apply "$ADV/plain.img" "$ADV/trunchdr.img" "$CS" trunc:300
        adv_thaw truncated-header "$ADV/trunchdr.img" "thaw: header read failed"
        $PATCH apply "$ADV/plain.img" "$ADV/truncblob.img" "$CS" trunc:5000
        adv_thaw truncated-vm-blob "$ADV/truncblob.img" "VM blob read failed during CRC check"
        $PATCH apply "$ADV/plain.img" "$ADV/badcrc.img"   "$CS" xor:1000:FF
        adv_thaw overall-crc-mismatch "$ADV/badcrc.img" "thaw: checksum mismatch"

        # --- Post-CRC arms (corruption that must SURVIVE the overall
        # CRC check; --fixcrc recomputes it after the patch).
        $PATCH apply "$ADV/plain.img" "$ADV/badsent.img"  "$CS" xor:"$HDR":08 --fixcrc
        adv_thaw vm-sentinel-mismatch "$ADV/badsent.img" "VM-base sentinel mismatch"

        # --- Memory-stream block arms (block 0 lives at header end:
        # offset u32, remaining u64, then data).
        $PATCH apply "$ADV/mem.img" "$ADV/memzero.img"   "$CS" p64:$((HDR + 4)):0
        adv_thaw mem-block-zero-size "$ADV/memzero.img" "has zero size (corrupt file)"
        $PATCH apply "$ADV/mem.img" "$ADV/memtrunc1.img" "$CS" trunc:$((HDR + 2))
        adv_thaw mem-block-header-truncated "$ADV/memtrunc1.img" "memory stream block 0 header read failed"
        $PATCH apply "$ADV/mem.img" "$ADV/memtrunc2.img" "$CS" trunc:$((HDR + 16))
        adv_thaw mem-block-data-truncated "$ADV/memtrunc2.img" "memory stream block 0 data read failed"
        $PATCH apply "$ADV/mem.img" "$ADV/memcrc.img"    "$CS" xor:$((HDR + 16)):FF --fixcrc
        adv_thaw mem-section-crc "$ADV/memcrc.img" "memory stream checksum mismatch"

        # malloc-failure arm: a huge `remaining` makes std::malloc fail
        # cleanly on a plain build; ASan instead hard-aborts on the
        # oversized request, so the variant runs only without sanitizers.
        if "$TRIX_ABS" --version < /dev/null 2>&1 | grep -q 'sanitizers: none'; then
            $PATCH apply "$ADV/mem.img" "$ADV/memhuge.img" "$CS" p64:$((HDR + 4)):35184372088832
            adv_thaw mem-block-malloc-fail "$ADV/memhuge.img" "malloc failed for memory stream block"
        else
            echo "  SKIP: adv: mem-block-malloc-fail (sanitizer build aborts oversized malloc)"
        fi

        # --- User-file-stream block arms (block 0 at header end:
        # stream_offset u32, file_offset i64, mode u8, flags u8,
        # filename_length u16, filename bytes).
        $PATCH apply "$ADV/ufs.img" "$ADV/ufslong.img"   "$CS" patch:$((HDR + 14)):60EA
        adv_thaw ufs-filename-too-long "$ADV/ufslong.img" "filename too long (60000 bytes)"
        $PATCH apply "$ADV/ufs.img" "$ADV/ufstrunc1.img" "$CS" trunc:$((HDR + 6))
        adv_thaw ufs-block-header-truncated "$ADV/ufstrunc1.img" "user file stream block 0 header read failed"
        $PATCH apply "$ADV/ufs.img" "$ADV/ufstrunc2.img" "$CS" trunc:$((HDR + 16))
        adv_thaw ufs-filename-truncated "$ADV/ufstrunc2.img" "filename read failed"
        # flip an UNUSED flags bit: the block still parses and the file
        # reopens, so the per-section CRC comparison is what fires (a
        # filename flip would hit the reopen arm first)
        $PATCH apply "$ADV/ufs.img" "$ADV/ufscrc.img"    "$CS" xor:$((HDR + 13)):02 --fixcrc
        adv_thaw ufs-section-crc "$ADV/ufscrc.img" "user file stream checksum mismatch"

        # --- Underlying-file drift (no byte patching: the image is
        # pristine but the file it reconnects has changed).
        mv "$ADV/ufs_data.txt" "$ADV/ufs_data.txt.bak"
        adv_thaw ufs-underlying-deleted "$ADV/ufs.img" "thaw: cannot reopen"
        printf 'xx' > "$ADV/ufs_data.txt"
        adv_thaw ufs-underlying-truncated "$ADV/ufs.img" "truncated (size=2, need 8)"
        mv "$ADV/ufs_data.txt.bak" "$ADV/ufs_data.txt"

        # --- Semantic capacity arm: a 4M image cannot thaw into a 2M
        # heap (real flow, no patching), via runtime thaw AND -l startup.
        adv_thaw vm-too-small-real "$ADV/big.img" "thaw: VM too small (need"
        if "$TRIX_ABS" -q --vm-size=2M -l "$ADV/big.img" < /dev/null 2>&1; then
            fail "adv: -l of oversized image should exit non-zero"
        else
            pass "adv: -l of oversized image exits non-zero"
        fi

        # --- Cross-binary useroperator mismatch: tetrix images carry a
        # different user-op table, so the runtime thaw arm rejects them.
        if [ -x ./tetrix ]; then
            printf '/m 1 def (%s) snap-shot\n' "$ADV/tetrix.img" | ./tetrix -q --vm-size=2M --stdin
            adv_thaw useroperator-mismatch "$ADV/tetrix.img" "incompatible useroperator table"
        else
            echo "  SKIP: adv: useroperator-mismatch (no ./tetrix binary)"
        fi
    else
        fail "adv: patcher calibration failed (snapshot format drift?)"
    fi
fi

# ====================================================================
# Summary
# ====================================================================
echo ""
echo "===================================="
TOTAL=$((PASS + FAIL))
echo "  Results: $TOTAL tests, $PASS passed, $FAIL failed"
echo "===================================="

if [ $FAIL -eq 0 ]; then
    echo "  ALL SNAPSHOT TESTS PASSED"
    exit 0
else
    echo "  SOME SNAPSHOT TESTS FAILED"
    exit 1
fi
