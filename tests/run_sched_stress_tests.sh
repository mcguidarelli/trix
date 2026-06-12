#!/bin/bash
# ====================================================================
# run_sched_stress_tests.sh -- scheduler-stress phase of the battery
# ====================================================================
# Sweeps tests/test_sched_stress.trx (the seeded random-topology
# scheduler stressor) across many base seeds.  Each invocation runs
# ROUNDS rounds; every round spawns a random actor/coroutine/pipe
# topology, churns it with sends, nested save/restore, vm-global-gc,
# kills, suspend/resume and context-recycling spawns, then requires the
# full integrity validator (heap + scheduler invariants, including the
# orphaned-sleeper rule) to hold.  The in-suite run covers the fixed
# default ladder; this runner is the wider campaign.
#
# A failure prints the offending base seed -- replay with:
#   ./trix tests/test_sched_stress.trx --seed=<seed> --rounds=1
# (bisect the round by narrowing --rounds).
#
# Knobs: TRIX_BIN (binary, default ./trix), SEED_BASE (first seed,
# default 1000003), SEED_COUNT (invocations, default 12), ROUNDS
# (rounds per invocation, default 10).  Exit: 0 all passed, 1 failure,
# 2 missing binary.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TRIX="${TRIX_BIN:-${PROJECT_DIR}/trix}"
SEED_BASE="${SEED_BASE:-1000003}"
SEED_COUNT="${SEED_COUNT:-12}"
ROUNDS="${ROUNDS:-10}"

if [ ! -x "$TRIX" ]; then
    echo "FATAL: trix binary not found or not executable: $TRIX" >&2
    exit 2
fi

PASS=0
FAIL=0

echo "===================================================="
echo "  sched-stress campaign: $SEED_COUNT seeds x $ROUNDS rounds"
echo "  binary: $TRIX  base seed: $SEED_BASE"
echo "===================================================="

i=0
while [ "$i" -lt "$SEED_COUNT" ]; do
    seed=$((SEED_BASE + i * 7919))
    out=$(timeout 120 "$TRIX" tests/test_sched_stress.trx --seed="$seed" --rounds="$ROUNDS" 2>&1)
    rc=$?
    if [ "$rc" -eq 0 ] && grep -qF "__test_ok__" <<< "$out"; then
        echo "  ok    seed $seed"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  seed $seed -- rc=$rc (124 = hang killed)"
        printf '%s\n' "$out" | tail -6 | sed 's/^/        /'
        echo "        replay: $TRIX tests/test_sched_stress.trx --seed=$seed --rounds=$ROUNDS"
        FAIL=$((FAIL + 1))
    fi
    i=$((i + 1))
done

echo "===================================================="
echo "  sched-stress results: $((PASS + FAIL)) seeds, ${PASS} passed, ${FAIL} failed"
echo "===================================================="

[ "$FAIL" -eq 0 ] || exit 1
exit 0
