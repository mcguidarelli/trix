#!/bin/bash
# triage.sh -- Classify fuzz artifacts as REAL / FALSE / CLEAN.
#
# Real crashes print a sanitizer report, assert message, or signal trap
# within ~1 second and exit non-zero.  False positives come in two flavors,
# both producing no diagnostic within a reasonable window:
#   - libFuzzer/aarch64 signal-pipe hangs (2-minute pipe_read deadlock,
#     same signature for stale -jobs=N orphan pipes and the aarch64
#     signal-handling race)
#   - slow-input timeout-* artifacts (a legal Trix program that just takes
#     longer than libFuzzer's -timeout=5 to run; not a bug).  The harness's
#     m_sleep_budget_ms = 500 bounds the sleep-driven flavor (huge
#     coroutine-sleep / timeout operands), so survivors of this class are
#     compute-slow, not park-slow
#
# CRITICAL: the aarch64 race can *also mask real bugs*.  When fuzz_trix's
# LLVMFuzzerTestOneInput redirects stderr to /dev/null and an assert()
# fires, libFuzzer catches SIGABRT, tries to write its report to the
# silenced stderr, and deadlocks on the signal pipe -- producing the
# false-positive signature but with a real bug underneath.  To catch these,
# triage runs trix (no stderr redirect, no libFuzzer signal handling)
# first, then fuzz_trix as a backstop.
#
# A 15-second timeout per binary separates the cases: >10x the time any
# real crash needs to dump its report, short enough that slow-input
# timeouts and pipe-read hangs both get killed with no diagnostic.
#
# Usage:
#   ./fuzz/triage.sh <artifact> [artifact ...]
#   ./fuzz/triage.sh --move-false-to DIR <artifact> [artifact ...]
#
# Classifications:
#   REAL   genuine crash (sanitizer / assert / signal diagnostic within 15s)
#   FALSE  timeout fired with no diagnostic (known false positive)
#   CLEAN  fast exit, no diagnostic (not a crash; shouldn't land here normally)
#
# With --move-false-to DIR, FALSE and CLEAN artifacts are moved to DIR.
#
# Exit status: 0 if every artifact is REAL, 1 otherwise.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FUZZ_BIN="${SCRIPT_DIR}/fuzz_trix"
TRIX_BIN="${SCRIPT_DIR}/../trix"

# Patterns that indicate a real finding.  Each maps to a distinct bug class:
#   SUMMARY: / ERROR: / *Sanitizer       -- ASan/UBSan/LSan/MSan/TSan
#   libFuzzer: deadly signal / OOM       -- libFuzzer's own diagnostics
#   Assertion .* failed                  -- glibc assert() via __assert_fail
#   runtime error:                       -- UBSan line (e.g. signed overflow)
#   Aborted / Segmentation fault         -- shell-reported signal traps
REAL_PATTERN='SUMMARY:|ERROR:|[A-Za-z]+Sanitizer|libFuzzer: deadly signal|libFuzzer: out-of-memory|Assertion .* failed|runtime error:|^Aborted|^Segmentation fault'

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
}

MOVE_TO=""
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --move-false-to) MOVE_TO="$2"; shift 2 ;;
        -h|--help)       usage ;;
        --)              shift; ARGS+=("$@"); break ;;
        -*)              echo "Unknown option: $1" >&2; usage ;;
        *)               ARGS+=("$1"); shift ;;
    esac
done

[ ${#ARGS[@]} -eq 0 ] && usage
[ -x "${FUZZ_BIN}" ] || { echo "Error: ${FUZZ_BIN} not found. Run fuzz/build.sh first." >&2; exit 2; }
[ -n "${MOVE_TO}" ] && mkdir -p "${MOVE_TO}"

TMPOUT="$(mktemp)"
trap 'rm -f "${TMPOUT}"' EXIT

real=0
false_=0
clean=0

for artifact in "${ARGS[@]}"; do
    if [ ! -f "${artifact}" ]; then
        echo "SKIP:  ${artifact} (not a file)"
        continue
    fi

    classification=""
    diag_source=""

    # Pass 1: trix (fast, no stderr redirect, no aarch64 signal race).
    # Catches asserts, sanitizer reports, and crashes that fuzz_trix masks.
    if [ -x "${TRIX_BIN}" ]; then
        timeout 15 "${TRIX_BIN}" "${artifact}" >"${TMPOUT}" 2>&1
        rc_trix=$?
        if grep -qE "${REAL_PATTERN}" "${TMPOUT}"; then
            classification="REAL"
            diag_source="trix rc=${rc_trix}"
        elif [ "${rc_trix}" -ge 128 ] && [ "${rc_trix}" -ne 143 ]; then
            # Signal trap (134=SIGABRT, 139=SIGSEGV, 135=SIGBUS, 136=SIGFPE,
            # 132=SIGILL).  143 is our own SIGTERM from timeout.
            classification="REAL"
            diag_source="trix rc=${rc_trix} (signal)"
        fi
    fi

    # Pass 2: fuzz_trix (matches original fuzzer environment).  Only run if
    # trix didn't already classify, since fuzz_trix may hang 2 minutes
    # on aarch64 for both real and false cases.
    if [ -z "${classification}" ]; then
        timeout 15 "${FUZZ_BIN}" "${artifact}" >"${TMPOUT}" 2>&1
        rc_fuzz=$?
        if grep -qE "${REAL_PATTERN}" "${TMPOUT}"; then
            classification="REAL"
            diag_source="fuzz_trix rc=${rc_fuzz}"
        elif [ "${rc_fuzz}" -eq 124 ]; then
            classification="FALSE"
            diag_source="fuzz_trix hang (no diagnostic within 15s)"
        else
            classification="CLEAN"
            diag_source="fuzz_trix rc=${rc_fuzz} no diagnostic"
        fi
    fi

    case "${classification}" in
        REAL)
            echo "REAL:  ${artifact}  (${diag_source})"
            real=$((real+1))
            ;;
        FALSE)
            echo "FALSE: ${artifact}  (${diag_source})"
            [ -n "${MOVE_TO}" ] && mv "${artifact}" "${MOVE_TO}/"
            false_=$((false_+1))
            ;;
        CLEAN)
            echo "CLEAN: ${artifact}  (${diag_source})"
            [ -n "${MOVE_TO}" ] && mv "${artifact}" "${MOVE_TO}/"
            clean=$((clean+1))
            ;;
    esac
done

echo
echo "Summary: ${real} REAL, ${false_} FALSE, ${clean} CLEAN"
[ "${false_}" -eq 0 ] && [ "${clean}" -eq 0 ]
