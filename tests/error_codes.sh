# error_codes.sh -- resolve Trix Error names to their process exit codes.
#
# `trix --error-codes` dumps one `code<TAB>name` line per Error enum entry
# (declaration order == process exit code; name from error_sv()).  That dump
# is the runtime single source of truth for the exit-code contract.  Source
# this file with $TRIX pointing at the binary under test, then resolve codes
# symbolically instead of hardcoding the numbers:
#
#     source "$SCRIPT_DIR/error_codes.sh"
#     want="$(err undefined)"        # -> 40, derived from the binary
#     code="$(resolve_rc "$tok")"    # bare integer passes through unchanged
#
# Tests stay readable and renumber-proof: if the enum shifts, the codes
# follow the binary automatically.  tests/check_error_codes.py separately
# pins the *documented* catalog against the same enum, so a renumber that
# actually changes the public contract still fails loudly there.
#
# This file defines functions and a global associative array; it is meant to
# be sourced, not executed.

declare -gA TRIX_ERR_CODE

# Load the name->code table from a trix binary (defaults to $TRIX).  Returns
# non-zero (and leaves the array empty) if the binary is missing or the dump
# is empty, so callers can fail fast.
load_error_codes() {
    local bin="${1:-${TRIX:-}}"
    if [ -z "$bin" ] || [ ! -x "$bin" ]; then
        echo "error_codes.sh: no usable trix binary (set \$TRIX)" >&2
        return 2
    fi
    TRIX_ERR_CODE=()
    local code name
    while IFS=$'\t' read -r code name; do
        [ -n "$name" ] && TRIX_ERR_CODE["$name"]="$code"
    done < <("$bin" --error-codes 2>/dev/null)
    if [ "${#TRIX_ERR_CODE[@]}" -eq 0 ]; then
        echo "error_codes.sh: '$bin --error-codes' produced no entries" >&2
        return 2
    fi
    return 0
}

# err <name> -- echo the numeric exit code for an Error name (e.g. "undefined"
# -> 40).  An unknown name returns 2 with a diagnostic; never resolve a typo
# to a silent success.
err() {
    local name="$1"
    local code="${TRIX_ERR_CODE[$name]:-}"
    if [ -z "$code" ]; then
        echo "error_codes.sh: unknown Error name '$name'" >&2
        return 2
    fi
    printf '%s' "$code"
}

# resolve_rc <token> -- echo a numeric exit code for either a bare integer
# (passed through unchanged: 0 clean, 1 usage error, 124 timeout -- none of
# which are Error-enum exits) or a symbolic Error name (resolved via err).
# Unknown names return 2 so the caller can abort the run.
resolve_rc() {
    local tok="$1"
    case "$tok" in
    '' | *[!0-9]*)
        err "$tok"
        ;;
    *)
        printf '%s' "$tok"
        ;;
    esac
}

# Auto-load on source when $TRIX is already a valid binary.
if [ -n "${TRIX:-}" ] && [ -x "${TRIX:-}" ]; then
    load_error_codes "$TRIX"
fi
