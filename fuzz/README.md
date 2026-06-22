# Fuzz Testing

Coverage-guided fuzz testing for the Trix interpreter using
[libFuzzer](https://llvm.org/docs/LibFuzzer.html).  Requires clang++-20
(or later) with the fuzzer runtime.

## Build

```bash
./fuzz/build.sh
```

## Run

```bash
./fuzz/run.sh                              # no time limit; stops on first event
./fuzz/run.sh -max_total_time=300          # budget-aware loop, 5 minutes total
./fuzz/run.sh -max_len=65536               # larger inputs for deeper execution
./fuzz/run.sh --vm-size=1M -fork=4         # 1MB VM, 4 parallel workers
./fuzz/run.sh -overnight                   # shorthand for -max_total_time=28800
                                           #   --vm-size=2M (explicit flags win)
```

Crash reproducers are saved to `fuzz/crashes/`.  After each fuzzer exit,
`run.sh` auto-triages every new `crash-*` artifact (see **Triage** below)
and moves false positives to `fuzz/crashes_rejected/`.  The corpus is
merged (minimized) once at the end.

### Budget-aware loop

When `-max_total_time=N` is supplied, `run.sh` wraps the fuzzer in an
outer loop.  libFuzzer exits on the first crash in single-process mode,
and on aarch64 the false-positive rate is non-trivial; without the loop a
single false crash cuts an 8-hour session short after 5 minutes.

Each iteration:

1. Compute `remaining = N - elapsed` and relaunch the fuzzer with
   `-max_total_time=remaining` (preserving the total wall-clock budget).
2. Triage any new artifacts.  FALSE / CLEAN get moved to
   `fuzz/crashes_rejected/`.
3. If anything real lands in `fuzz/crashes/`, exit 1.  Otherwise continue
   until the budget is spent or the user interrupts (Ctrl-C).

Without `-max_total_time`, the script prints a banner and runs the fuzzer
exactly once -- it stops on the first crash event, real or false.  Use
this mode for quick ad-hoc runs; use the budget-aware form for overnight
runs.

## Parallelism: use `-fork=N`, never `-jobs=N`

For multi-worker fuzzing, always pass `-fork=N` (single libFuzzer parent,
N fork-server children).  **Do not use `-jobs=N`.**

`-jobs=N` spawns N independent libFuzzer processes whose inter-process
pipes get inherited by saved crash artifacts.  When such an artifact is
replayed standalone (`./fuzz/fuzz_trix crash-<hash>`), the orphan pipes
have no writer and the binary blocks on them for ~2 minutes before
exiting with rc=0 and no sanitizer output -- indistinguishable from a
real slow-path crash on first inspection, but actually false.

`-fork=N` gives the same throughput without the jobs-mode IPC; the
parent aggregates coverage from children transparently and crashes stay
isolated to the child that produced them.

## Triage

libFuzzer on aarch64 (and, separately, stale `-jobs=N` artifacts) can
produce **false-positive crash files** that do not correspond to any
actual bug.  These are the result of signal-handling / pipe-read
interactions between libFuzzer, the sanitizer runtimes, and the target
-- not a Trix defect.  They have a distinctive replay signature:

| signal                    | real crash     | false positive      |
| ------------------------- | -------------- | ------------------- |
| time to report            | milliseconds   | exactly 2 minutes   |
| exit code                 | non-zero       | 0                   |
| sanitizer / libFuzzer log | present        | empty (banner only) |
| `trix` on same input      | ordinary error | clean exit          |

A 15-second timeout cleanly separates the two cases -- real crashes
always dump their report within a second, false positives always hang
the full two minutes.

**Why the weeding is safe.** The same aarch64 race can also *mask* a real bug:
if an `assert()` fires while `fuzz_trix` has stderr silenced, libFuzzer catches
the `SIGABRT`, tries to write its report to the closed stderr, and deadlocks on
the signal pipe -- the identical two-minute hang signature, but with a real bug
underneath.  So triage never trusts the hang alone: it runs **`trix` first** (no
stderr redirect, no libFuzzer signal handling), which surfaces the assert or
sanitizer report directly, and only falls back to `fuzz_trix` when `trix` comes
back clean.  A masked crash is caught by that first pass, not silently weeded out
as a false positive.

### Automatic triage (run.sh)

After every `./fuzz/run.sh` invocation, the script runs
`./fuzz/triage.sh` on every artifact libFuzzer dropped into
`fuzz/crashes/`.  libFuzzer uses distinct prefixes per artifact type --
`crash-*` (sanitizer / SIGSEGV / etc), `timeout-*` (`-timeout` fired),
`leak-*` (LSan finding), `oom-*` (`-rss_limit_mb` exceeded) -- and all
four are triaged.  Artifacts classified as FALSE or CLEAN (includes
signal-pipe hangs and slow-input timeouts with no sanitizer diagnostic)
are moved to `fuzz/crashes_rejected/`.  Only artifacts classified as
REAL (sanitizer or `libFuzzer:` diagnostic within 15 seconds) remain in
`fuzz/crashes/`.

The intent: when you come back to a finished run, anything still in
`fuzz/crashes/` is worth looking at.  Nothing in there is a two-minute
orphan-pipe or an exponential-output `repeat` that happened to trip
libFuzzer's 5-second unit timeout.

The harness also bounds wall-clock parks: `m_max_ops` cannot tick while
a coroutine is parked, so a mutated huge `coroutine-sleep` operand (the
2026-06-06 overnight artifacts decoded a binary-token int32 into a
~9.4-day sleep) used to stall a unit until `-timeout` flagged a false
positive.  `fuzz_trix.cpp` sets `cfg.m_sleep_budget_ms = 500`
(`--sleep-budget` on the CLI), capping TOTAL granted park time per
unit; spent budget turns timed parks into immediate wakes.

### Manual triage (triage.sh)

For ad-hoc checks, deleting stale artifacts, or auditing what landed in
`crashes_rejected/`:

```bash
./fuzz/triage.sh fuzz/crashes/crash-<hash>               # single file
./fuzz/triage.sh fuzz/crashes/crash-*                    # batch
./fuzz/triage.sh --move-false-to /tmp/rejects crashes/*  # triage + move
```

Each artifact is classified as `REAL`, `FALSE`, or `CLEAN`, with a
single-line summary at the end.  Exit status is 0 only if every artifact
is REAL.

## VM heap size

By default the harness uses MinVmSize (256KB).  Use `--vm-size=` to
set a larger heap -- accepts plain bytes or K/M suffixes:

```bash
./fuzz/run.sh --vm-size=1M -max_total_time=28800
./fuzz/run.sh --vm-size=2M -fork=4
```

Larger VMs exercise deeper allocation paths but reduce executions per
second.  For overnight runs, 1M-2M is a good balance.

## Reproduce a crash

```bash
# Triage first -- confirms the artifact is real, not a 2-minute false positive
./fuzz/triage.sh fuzz/crashes/crash-<hash>

# Full reproducer with ASan/UBSan output
./fuzz/fuzz_trix fuzz/crashes/crash-<hash>

# Minimal reproducer with ordinary error output
./trix fuzz/crashes/crash-<hash>
```

If `triage.sh` reports FALSE, `fuzz_trix` will hang for two minutes and
produce no sanitizer output; skip it and delete the artifact.  If it
reports REAL, the other two commands will surface the defect.

## Seed corpus

`fuzz/seeds/` contains small hand-crafted inputs (~50 bytes each)
covering all syntax forms, operator categories, and binary token types.
libFuzzer mutates these to discover new coverage.

The evolving corpus is stored in `fuzz/corpus/` (gitignored, auto-merged).
