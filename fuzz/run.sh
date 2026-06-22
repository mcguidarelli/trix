#!/bin/bash
# run.sh -- Run the Trix fuzzer.
#
# Usage:
#   ./fuzz/run.sh                              # no time limit; stops on first event
#   ./fuzz/run.sh -max_total_time=300          # budget-aware loop, 5 minutes total
#   ./fuzz/run.sh -max_len=65536               # larger inputs
#   ./fuzz/run.sh --vm-size=1M                 # 1MB VM heap
#   ./fuzz/run.sh --vm-size=2M -fork=4         # 2MB VM, 4 parallel workers
#   ./fuzz/run.sh -overnight                   # 8-hour run, 2MB VM heap
#
# Crash reproducers are saved to fuzz/crashes/.  After each fuzzer exit the
# script auto-triages every new crash-* artifact.  libFuzzer / aarch64 false
# positives (2-minute pipe_read hang, rc=0, no sanitizer output) are moved to
# fuzz/crashes_rejected/; genuine crashes stay in fuzz/crashes/.
#
# If -max_total_time=N is supplied, the script loops until the total wall-clock
# budget is exhausted or the user interrupts.  Each relaunch uses a reduced
# -max_total_time=(N - elapsed) so the total time stays bounded.  Real crashes
# found during a budgeted run are moved to fuzz/crashes/kept/ so the loop can
# keep going; the exit code is 1 if any were found.  Without -max_total_time
# the script runs the fuzzer exactly once and exits on the first event (real
# crash or false-positive artifact).
#
# -overnight is shorthand for -max_total_time=28800 --vm-size=2M.  Explicit
# flags win: `-overnight --vm-size=4M` uses a 4MB VM for 8 hours.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FUZZ_BIN="${SCRIPT_DIR}/fuzz_trix"
TRIAGE="${SCRIPT_DIR}/triage.sh"
CORPUS="${SCRIPT_DIR}/corpus"
SEEDS="${SCRIPT_DIR}/seeds"
CRASHES="${SCRIPT_DIR}/crashes"
REJECTED="${SCRIPT_DIR}/crashes_rejected"

if [ ! -x "${FUZZ_BIN}" ]; then
    echo "Error: ${FUZZ_BIN} not found. Run fuzz/build.sh first."
    exit 1
fi

# Parse args: extract --vm-size, -max_total_time, and -overnight; pass the
# rest to libFuzzer.
FUZZ_ARGS=()
TOTAL_BUDGET=""
OVERNIGHT=0
for arg in "$@"; do
    case "$arg" in
        --vm-size=*)       export TRIX_FUZZ_VM_SIZE="${arg#--vm-size=}" ;;
        -max_total_time=*) TOTAL_BUDGET="${arg#-max_total_time=}" ;;
        -overnight)        OVERNIGHT=1 ;;
        *)                 FUZZ_ARGS+=("$arg") ;;
    esac
done

# -overnight supplies defaults; explicit --vm-size / -max_total_time win.
if [ "${OVERNIGHT}" -eq 1 ]; then
    [ -z "${TOTAL_BUDGET}" ] && TOTAL_BUDGET=28800
    [ -z "${TRIX_FUZZ_VM_SIZE:-}" ] && export TRIX_FUZZ_VM_SIZE=2M
fi

# libFuzzer treats -max_total_time=0 as "no limit"; mirror that here.
if [ "${TOTAL_BUDGET}" = "0" ]; then
    TOTAL_BUDGET=""
fi

mkdir -p "${CORPUS}" "${CRASHES}"

# Capture each iteration's final-stats line so we can sum exec counts at end.
STATS_LOG="$(mktemp)"
trap 'rm -f "${STATS_LOG}"' EXIT

INTERRUPTED=0
trap 'INTERRUPTED=1' INT

if [ -z "${TOTAL_BUDGET}" ]; then
    echo "=================================================================="
    echo " No -max_total_time provided."
    echo " Running until the first crash event (real or false positive) or"
    echo " an interrupt.  Pass -max_total_time=N to enable the budget-aware"
    echo " loop that rides through libFuzzer/aarch64 false positives."
    echo "=================================================================="
    echo
fi

START_TIME=$(date +%s)
REAL_CRASH_FOUND=0
ITERATIONS=0

while true; do
    ITER_ARGS=("${FUZZ_ARGS[@]}")
    if [ -n "${TOTAL_BUDGET}" ]; then
        ELAPSED=$(($(date +%s) - START_TIME))
        REMAINING=$((TOTAL_BUDGET - ELAPSED))
        if [ "${REMAINING}" -le 10 ]; then
            echo
            echo "Budget exhausted (${ELAPSED}s of ${TOTAL_BUDGET}s) -- stopping."
            break
        fi
        ITER_ARGS+=("-max_total_time=${REMAINING}")
        echo
        echo "Starting fuzzer with ${REMAINING}s remaining (elapsed ${ELAPSED}s of ${TOTAL_BUDGET}s)..."
    fi

    set +e
    # tee forwards libFuzzer output to the terminal (same live visibility as
    # before) and appends to STATS_LOG so we can sum exec counts at the end.
    # PIPESTATUS[0] is the fuzzer's exit code; PIPESTATUS[1] is tee's (ignored).
    "${FUZZ_BIN}" "${CORPUS}" "${SEEDS}" \
        -artifact_prefix="${CRASHES}/" \
        -max_len=4096 \
        -timeout=5 \
        -ignore_timeouts=1 \
        -ignore_ooms=1 \
        -rss_limit_mb=512 \
        -print_final_stats=1 \
        "${ITER_ARGS[@]}" 2>&1 | tee -a "${STATS_LOG}"
    STATUS=${PIPESTATUS[0]}
    set -e
    ITERATIONS=$((ITERATIONS + 1))

    # Triage any artifacts this iteration produced.  FALSE / CLEAN get
    # moved to crashes_rejected/; REAL stay in crashes/.  libFuzzer uses
    # distinct prefixes per artifact type: crash-* (sanitizer/SIGSEGV/etc),
    # timeout-* (-timeout fired), leak-* (LSan), oom-* (-rss_limit_mb).
    shopt -s nullglob
    crash_artifacts=("${CRASHES}"/crash-* "${CRASHES}"/timeout-* "${CRASHES}"/leak-* "${CRASHES}"/oom-*)
    shopt -u nullglob
    if [ ${#crash_artifacts[@]} -gt 0 ] && [ -x "${TRIAGE}" ]; then
        echo
        echo "Triaging ${#crash_artifacts[@]} artifact(s)..."
        "${TRIAGE}" --move-false-to "${REJECTED}" "${crash_artifacts[@]}" || true
    fi

    # Anything left in crashes/ (excluding the kept/ subdir) is REAL.  In
    # single-shot mode we stop and leave it in place; in budget mode we move
    # it to crashes/kept/ so the loop can keep going against a fresh dir.
    shopt -s nullglob
    remaining_artifacts=("${CRASHES}"/crash-* "${CRASHES}"/timeout-* "${CRASHES}"/leak-* "${CRASHES}"/oom-*)
    shopt -u nullglob
    if [ ${#remaining_artifacts[@]} -gt 0 ]; then
        REAL_CRASH_FOUND=$((REAL_CRASH_FOUND + ${#remaining_artifacts[@]}))
        if [ -z "${TOTAL_BUDGET}" ]; then
            echo
            echo "Real crash artifact(s) retained in ${CRASHES}/ -- stopping."
            break
        fi
        mkdir -p "${CRASHES}/kept"
        mv "${remaining_artifacts[@]}" "${CRASHES}/kept/"
        echo
        echo "Real crash artifact(s) moved to ${CRASHES}/kept/ (total kept: ${REAL_CRASH_FOUND}) -- continuing."
    fi

    if [ "${INTERRUPTED}" -ne 0 ]; then
        echo
        echo "Interrupted -- stopping loop."
        break
    fi

    # No budget == single-shot run; exit after one iteration regardless of outcome.
    if [ -z "${TOTAL_BUDGET}" ]; then
        break
    fi
done

# Capture elapsed BEFORE the corpus merge so the final stat line reflects
# only the time spent fuzzing, not the merge phase (which can be long on
# a large corpus and would muddy throughput numbers).
FUZZ_ELAPSED=$(($(date +%s) - START_TIME))

# Merge corpus: keep only inputs that contribute unique coverage.
if [ -d "${CORPUS}" ] && [ "$(ls -A "${CORPUS}" 2>/dev/null)" ]; then
    CORPUS_MIN="${CORPUS}_min"
    rm -rf "${CORPUS_MIN}"
    mkdir -p "${CORPUS_MIN}"
    echo "Merging corpus..."
    if "${FUZZ_BIN}" "${CORPUS_MIN}" "${CORPUS}" -merge=1 2>/dev/null && \
       [ "$(ls -A "${CORPUS_MIN}" 2>/dev/null)" ]; then
        rm -rf "${CORPUS}"
        mv "${CORPUS_MIN}" "${CORPUS}"
        echo "Corpus minimized to $(ls "${CORPUS}" | wc -l) files."
    else
        echo "Merge failed or produced empty result; keeping original corpus."
        rm -rf "${CORPUS_MIN}"
    fi
fi

# Sum per-iteration exec counts.  libFuzzer's -print_final_stats=1 emits a
# single line like "stat::number_of_executed_units: 123456" at each exit.
TOTAL_EXECS=$(awk '/stat::number_of_executed_units:/ {s+=$NF} END {print s+0}' "${STATS_LOG}")
echo
if [ "${ITERATIONS}" -gt 1 ]; then
    echo "Total: ${TOTAL_EXECS} executions across ${ITERATIONS} iterations in ${FUZZ_ELAPSED}s"
else
    echo "Total: ${TOTAL_EXECS} executions in ${FUZZ_ELAPSED}s"
fi

if [ "${REAL_CRASH_FOUND}" -ne 0 ]; then
    echo "Real crash(es) found: ${REAL_CRASH_FOUND} (see ${CRASHES}/ or ${CRASHES}/kept/)."
    exit 1
fi
exit 0
