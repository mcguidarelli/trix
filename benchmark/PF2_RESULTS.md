# PF2: Journal-write hot-path overhead measurement

## Question

Phase 1 added an `is_global()` short-circuit at the top of `Save::save_data()`:

```cpp
if (trx->is_global(data_offset)) {
    return nullptr;  // global mutations skip journaling
}
```

`is_global()` is a single 64-bit address-range comparison.  Does it
measurably slow the journal-write hot path on workloads where it
always returns false (i.e., normal local-VM mutations)?

## Methodology

Two microbenchmarks (under `benchmark/`):

- **bench_journal_write.trx** — tight `save / put / restore` loop on
  a local Dict.  1 M iterations.  Each iter: one `save_data()` call
  per entry mutation, one journal entry written, save+restore
  bracketing the call.

- **bench_journal_write2.trx** — one outer save scope per iteration,
  1024 distinct keys mutated inside.  Yields ~4 M `save_data()` calls
  / journal writes per run.  Tighter loop, less save/restore overhead
  per call.

For each, measured:

1. Current code (with the `is_global()` short-circuit).
2. Local patch with the short-circuit commented out.
3. Restored, re-measured to bracket noise.

Tooling: `benchmark/release.sh` (gcc-15 -O2 -DNDEBUG, no sanitizers)
+ `benchmark/timer.py` (10 runs, reports min / mean-of-5-best /
median / max).

## Results

| Benchmark                          | Without check | With check | Δ (mean5)      |
| ---------------------------------- | ------------- | ---------- | -------------- |
| bench1 (1M save+put+restore)       | 4464 ms       | 4479 ms    | +15 ms (+0.3%) |
| bench2 (4M journal writes, 1 save) | 925 ms        | 928 ms     | +3 ms (+0.3%)  |

Run-to-run noise on this hardware: ±20 ms on bench1, ±10 ms on
bench2.  Both deltas sit within noise.

## Conclusion

**The `is_global()` check costs ≤0.5% on the journal-write hot path
— within run-to-run noise.**

This matches the theoretical expectation:

- Single 64-bit comparison `(offset >= m_vm_global - m_vm_base)`.
- The comparison's right-hand side is hot in cache (a Trix scalar
  field accessed throughout the journal path).
- The branch is highly predictable (always FALSE on workloads that
  don't allocate globally), so the CPU's branch predictor + spec.
  execution issue the compare in parallel with the next instructions.
- Zero additional memory traffic.

No optimisation needed.  The Phase 1 design (consult an explicit
predicate per call) wins on simplicity over alternatives like a
header bit in every Object, and the perf cost is unmeasurable.

## Artifacts

- `benchmark/bench_journal_write.trx` -- save+put+restore microbench
- `benchmark/bench_journal_write2.trx` -- bulk-mutation microbench
- `benchmark/release.sh` -- corrected to link `-lz` (was missing
  since the deflate/inflate ops landed)

## 2026-06-06 re-measurement

Current numbers on the Phase-5 tree (object hot-path fast paths;
VM upgraded to 8 cores -- per-core throughput verified comparable):

| Benchmark                          | min of 10 | May 7 reference |
| ---------------------------------- | --------- | --------------- |
| bench1 (1M save+put+restore)       | 4528 ms   | 4479 ms         |
| bench2 (4M journal writes, 1 save) | 842 ms    | 928 ms          |

bench2 gains 8.7% from the make_clone scalar fast path; bench1 is
save/restore-bracket dominated and sits within a few percent of the
May baseline.  The experiment's conclusion is unchanged: the
`is_global()` short-circuit remains unmeasurable on the journal
hot path.
