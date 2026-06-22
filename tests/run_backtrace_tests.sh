#!/bin/bash
# run_backtrace_tests.sh -- Verify format_backtrace() boundary annotations.
#
# Each scenario fires a one-liner Trix program designed to crash inside a
# specific control structure.  The stderr backtrace must contain the
# expected `[X boundary]` line.  Fatal errors are how format_backtrace()
# gets invoked, so each scenario runs in its own ./trix invocation.
#
# Exit code: 0 if every scenario's expected line is present, 1 otherwise.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TRIX="${TRIX:-${PROJECT_DIR}/trix.opt}"

if [ ! -x "$TRIX" ]; then
    echo "FATAL: trix binary not found or not executable: $TRIX" >&2
    exit 2
fi

PASS=0
FAIL=0

check() {
    local scenario="$1"
    local trix_code="$2"
    local expected="$3"
    local stderr_out
    stderr_out=$(echo "$trix_code" | "$TRIX" /dev/stdin 2>&1 1>/dev/null)
    if echo "$stderr_out" | grep -qF "$expected"; then
        echo "  PASS: $scenario  (saw: $expected)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $scenario  (expected: $expected)"
        echo "         got stderr:"
        echo "$stderr_out" | sed 's/^/           /'
        FAIL=$((FAIL + 1))
    fi
}

echo "=== format_backtrace() Phase 1 boundary annotations ==="

check "loop_boundary"      '{ 1 0 div } loop'                       '[loop boundary]'
check "dowhile_boundary"   '{ true } { 1 0 div } do-while'          '[do-while boundary]'
check "repeat_boundary"    '3 { 1 0 div } repeat'                   '[repeat boundary]'
check "forall_boundary"    '[2 0] { 1 exch div pop } for-all'       '[for-all boundary]'
check "dip_boundary"       '1 0 1 { div } dip'                      '[dip boundary]'
check "locals_boundary"    '/t { |x| x 1 0 div pop } def 42 t'      '[locals boundary]'
check "inglobal_boundary"  '${ 1 0 div }'                            '[in-global/local boundary]'
check "time_boundary"      '{ 1 0 div } time'                       '[time boundary]'

echo ""
echo "=== format_backtrace() Phase 2: state header + last-error-data ==="

check "state_header_main"  '1 0 div'                                'state: sl=0 coro=main'
check "state_header_sl1"   'save 1 0 div'                           'state: sl=1 coro=main'
check "last_error_data"    '/range-check << /actual 200 >> throw-with' 'last-error-data:'

echo ""
echo "=== format_backtrace() Phase 3: companion-rich barriers ==="

check "effect_handles"     '<< /ask { } /tell { } >> { /missing perform } handle-effect'  '[effect boundary] handles:'
check "delimit_depth"      '{ undef-name } delimit'                 '[delimit boundary] snapshot-depth='

echo ""
echo "=== format_backtrace() Phase 4: inside-summary line ==="

check "inside_loop"        '{ 1 0 div } loop'                       'inside: loop'
check "inside_delimit_forall" '[1 2 3] { { undef-name } delimit pop } for-all'  'inside: for-all / delimit'
check "inside_effect_loop" '<< /ping { } >> { { /missing perform } loop } handle-effect'  'inside: effect / loop'

echo ""
echo "=== format_backtrace() Phase 6: composite operand preview ==="

check "preview_array"      '[1 /foo (hi)] 0 div'                    'preview [#2]: ['
check "preview_string"     '(hello world testing) 0 div'            'preview [#2]: "hello'

echo ""
echo "=== format_backtrace() Phase 7: TRIX_BT_VERBOSE ==="

check_verbose() {
    local scenario="$1"
    local trix_code="$2"
    local expected="$3"
    local stderr_out
    stderr_out=$(echo "$trix_code" | TRIX_BT_VERBOSE=1 "$TRIX" /dev/stdin 2>&1 1>/dev/null)
    if echo "$stderr_out" | grep -qF "$expected"; then
        echo "  PASS: $scenario  (saw: $expected)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $scenario  (expected: $expected)"
        echo "         got stderr:"
        echo "$stderr_out" | sed 's/^/           /'
        FAIL=$((FAIL + 1))
    fi
}

# Default mode caps preview string at 30 chars + ellipsis.  Verbose shows the full ~50-char string.
check_verbose "verbose_full_string" '(this is a long string that exceeds the thirty char limit) 0 div' \
    'preview [#2]: "this is a long string that exceeds the thirty char limit"'
# Default mode previews only the first composite.  Verbose previews both string AND array.
check_verbose "verbose_both_composites" '[1 2 3] (hello world) 0 div' \
    'preview [#3]:'

echo ""
echo "===================================================="
echo "  Results: $((PASS + FAIL)) scenarios, $PASS passed, $FAIL failed"
echo "===================================================="

[ "$FAIL" -eq 0 ]
