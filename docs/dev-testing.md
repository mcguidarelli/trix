<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Testing and Validation in Trix

Everything about how Trix is tested: the philosophy, the suites and
harnesses, the box taxonomy, the intrusive instruments, fuzzing,
coverage measurement, and the how-to for adding tests.  This is the
developer-side companion to
[trix-reference.md § 6](trix-reference.md#6-test-writing-patterns)
(the `.trx` assertion idioms) and the *Testing* section of
[CONTRIBUTING.md](../CONTRIBUTING.md) (the entry-point quick table).

---

## 1. Philosophy

1. **A test failure is an engine bug until proven otherwise.**  When a
   test fails, the engine is investigated first; the test (or the doc)
   is only "fixed" after the implementation is shown correct.  The
   inverse rule also holds: any bug found while doing *other* work is
   fixed immediately, never filed away.
2. **Every correctness fix lands with its regression test, in the same
   commit.**  The test must fail before the fix and pass after --
   non-vacuity is part of the fix.
3. **Corner cases are not "known limitations."**  A reachable surprise
   in semantics gets either a fix or an error raised at the point the
   hazard is created -- never a silent footgun documented as the user's
   problem.
4. **Runners validate positively.**  Every harness greps for explicit
   success markers (`__test_ok__`, `0 failures`, named scenario
   markers) *and* failure signatures (`FAILED:`, sanitizer reports,
   abnormal exits).  Silence is never success; a hang is a failure
   (timeouts are part of the harness).
5. **Operators are tested along three axes**, not just "does it work":
   - *Behavior*: happy path, sad path (every documented error), and
     boundary values (zero, max length, wraparound).
   - *Operand kind*: every accepted type arm -- including the
     heap-extension kinds (Long/ULong/Double/Address in ExtValue
     slots; Int128/UInt128 in WideValue slots).
   - *Allocation region*: local VM **and** global VM (`${...}` /
     `set-global`), so ExtValue/WideValue traffic exercises both heaps
     and their free lists.
6. **Read the reference first.**  Before writing or editing any `.trx`
   test, consult [trix-reference.md](trix-reference.md) for the op's
   stack effect and error contract.  Tests written from memory encode
   the author's guess, not the specification.
7. **The full matrix gates engine changes.**  Any change to `trix.h` /
   `src/*.inl` runs the whole battery on both binaries (debug+ASan and
   optimized) before it lands: `./runtests.sh` plus the gc-stress and
   backtrace runners.
8. **Determinism is a feature under test.**  Golden outputs are
   byte-exact and verified identical across the debug and optimized
   binaries before being blessed.

---

## 2. The validation stack at a glance

| Layer | Box | Harness | What it catches |
| --- | --- | --- | --- |
| Compile-time proofs | white | `static_assert` + `consteval` in `trix.h` | Table/spec drift: scanner character classes (`verify_chattr`), numeric-hint table (`verify_hint_table`), dispatch completeness (`verify_dispatch_tables`) -- every build, release included |
| Sanitizers | white | `./build.sh` default (ASan+UBSan, `-O0`) | Memory errors, UB (e.g. null-pointer arithmetic), leaks -- on every debug test run |
| Static analysis | white | CI: clang-tidy (curated checks), CodeQL | Bug patterns, security issues |
| In-process suite | gray | `tests/test_all.trx` via `runtests.sh` step 1 | Cross-test interaction in one VM: save/restore depth, dict-state bleed, pool reuse |
| Standalone sweep | gray | `tests/run_all.sh` | Per-test correctness in a fresh default VM; catches state leaks the in-process runner masks |
| Golden stdout | black | `tests/run_golden_tests.sh` | Byte-exact output of the print family and any op whose *output* is the contract |
| PTY scenarios | black | `tests/run_pty_tests.sh` | Terminal-dependent behavior: blocking reads, REPL recovery, banner contract |
| Snapshot phases | black/gray | `tests/run_snapshot_tests.sh` | snap-shot/thaw round trips across separate processes |
| GC stress | intrusive | `tests/run_gc_stress_tests.sh` | GC-before-every-alloc + freed-payload poisoning (debug binary only) |
| Backtrace scenarios | black | `tests/run_backtrace_tests.sh` | Boundary annotations in crash backtraces, one crash per control structure |
| Fuzzing | gray, coverage-guided | `fuzz/` (libFuzzer) | Crashes/UB on adversarial input; regressions replayed in every battery run |
| Coverage measurement | white | gcov instrumentation + per-op gap analysis | What is *not* tested: per-op, per-type-arm, per-line |
| CI | -- | `.github/workflows/` | {gcc-15, clang-18} x {Debug, Release} matrix, README example execution, doc-link integrity, clang-format |

`./runtests.sh` is the battery CI runs: in-process suite, standalone
sweep, golden, PTY, snapshot, eqref-overflow, sandbox-mode, and
fuzz-regression replay, with a single summary table and a non-zero exit
on any failure.

---

## 3. Box taxonomy

**Black box** -- drives the binary exactly as a user would and asserts
on externally visible behavior only:

- Golden tests: a `.trx` file runs in a fresh interpreter; combined
  stdout+stderr is diffed **byte-for-byte** against a blessed
  `.expected` file (15s hang cap, exit-code check).
- PTY tests: the binary runs under a real pseudo-terminal
  (`script -qec "stty rows 24 cols 80; ..."`), input arrives from
  timed feeders, assertions are marker greps (`!marker` asserts
  absence).  This is the only way to reach raw-mode blocking reads and
  interactive-REPL recovery.
- Backtrace tests: each scenario crashes a one-liner inside a specific
  control structure and asserts the `[X boundary]` annotation appears.
- CI's README-example step: code blocks are extracted from README.md
  and executed; documentation that stops compiling fails the build.

**Gray box** -- `.trx` tests that use the language's own introspection
to see inside the VM while still running through the public surface.
The `:status:` dictionary is the main porthole: `vm-used`,
`vm-global-used`, `extvalue-active`, `save-level`, scheduler and pool
counters.  Canonical gray-box patterns:

- *Leak checks*: read a counter, run N rounds of the operation, assert
  the counter returned to baseline (see § 7 for the measurement
  gotchas).
- *Save/restore isolation*: assert `vm-used` net-zero across a
  `save ... restore` of the workload.
- *Scheduler probes*: coroutine tests asserting ready-queue depths and
  timer wakes.

**White box** -- validates internals directly, not through program
behavior:

- The three consteval proofs in `trix.h` (`verify_chattr`,
  `verify_hint_table`, `verify_dispatch_tables`): table/spec drift
  fails *every* build at compile time.  Each was probed for
  non-vacuity when introduced (corrupt one row, watch the build fail).
- ASan/UBSan on the default debug build: every debug-side test run is
  also a memory/UB audit.
- Accessor asserts (`assert(is_long())` in `long_value()` etc.) --
  defensive tripwires active in every debug run, by policy never
  removed.
- gcov coverage runs (§ 9) -- the inverse view: which lines, branches,
  and type arms no test reaches.

---

## 4. Intrusive instruments (fault injection)

Debug-build operators and CLI flags that deliberately destabilize the
runtime to surface latent bugs:

| Instrument | Kind | What it does |
| --- | --- | --- |
| `vm-gc-stress` | debug op | Forces a global-VM GC before **every** global allocation -- any object not properly rooted at an allocation point gets swept mid-operation.  The gc-stress runner wraps its scenarios in this mode. |
| `vm-gc-poison` | debug op | Scribbles freed global-VM payloads, turning use-after-free into deterministic garbage instead of flaky luck. |
| `--test-eqgen-preload=N` | CLI flag | Preloads all five eq-storage generation counters (e.g. to `UINT32_MAX`) so the 2^32 wrap/LimitCheck path is reachable in one step instead of four billion. |
| `--max-ops=N` | CLI flag | Hard execution-limit watchdog: raises `/execution-limit` after N operations.  Used to bound runaway-loop tests. |
| `--sleep-budget=N` | CLI flag | Wall-clock companion to `--max-ops` (op counter cannot tick while parked): caps TOTAL granted sleep/timeout park time at N ms, then timed parks wake immediately.  The fuzz harness sets 500. |
| `--vm-size` / `--vm-global` / `--userdict-size` / depth flags | CLI flags | Starvation harnesses: a tiny VM or stack turns "works in practice" into deterministic `vm-full` / overflow paths.  Fuzz seeds use tight `--vm-global` to force GC mid-operation. |
| `--sandbox` | CLI flag | Verifies the host-op ban is airtight: every filesystem/system/terminal op must raise `/unsupported` (a dedicated battery step runs `test_raw_mode_sandbox.trx` this way). |
| `TRIX_HEAP_TRACKING` | build flag (debug default) | Per-callsite allocation accounting: `alloc-stats`, `vm-heap-snapshot` / `vm-heap-diff` let tests assert *which sites* allocated between two points. |
| ASan/UBSan | build mode | The default `./build.sh` binary; 10-30x slower, which is why per-test VM budgets and timeouts are calibrated against it. |

---

## 5. The suites in detail

### 5.1 `runtests.sh` -- the battery

Eight steps, each validated positively; any failure flips the exit code:

1. **`tests/test_all.trx`** in-process master runner, at
   `--vm-size=2M --userdict-size=1024` (the debug binary's baseline is
   ~495 KB of heap-tracking/debugger substrate, and the unwrapped tail
   legitimately retains pool memory and accumulates userdict entries).
   The runner's own `N failures` summary, per-assert `FAILED:` lines,
   and fatal `Trix exiting` markers are all checked.
2. **`tests/run_all.sh`** standalone per-test sweep (below).
3. **`tests/run_golden_tests.sh`** byte-exact stdout pinning.
4. **`tests/run_pty_tests.sh`** PTY scenarios (graceful SKIP if
   util-linux `script` is unavailable).
5. **Snapshot tests** -- orchestrated multi-phase snap-shot/thaw
   across separate invocations.
6. **eqref overflow** -- a separate invocation with
   `--test-eqgen-preload=4294967295`.
7. **Sandbox-mode tests** -- `--sandbox` host-op rejection.
8. **Fuzz-regression replay** -- every artifact in
   `tests/fuzz_regressions/` runs against the current binary; a
   sanitizer report, assert, or signal kill is a regression (normal
   Trix *errors* are fine -- malformed input should be rejected, not
   crash).

### 5.2 `tests/test_all.trx` -- in-process master runner

Runs 148 test files inside **one** interpreter.  Structure:

- The bulk runs through `run-test`, which wraps each file in
  `save ... restore` so per-file state is rolled back.
- A trailing **unwrapped concurrency section** runs the
  coroutine/actor/pipeline tests outside any save barrier (spawning
  coroutines above a save level and restoring across them is illegal
  by design), ending with a kill-zombies sweep.
- A **delist comment block** names the tests that *cannot* run in a
  shared process and why: needs `SPECIAL_ARGS` (VM size, preload
  flags), valid only at save-level 0 (restore-fragile messages), or
  raw `choice` save-level interplay.  Delisted tests still run via
  `run_all.sh`.

What this suite uniquely catches: cross-test interaction.  Userdict
shadowing (a test `def`ing an operator name like `count` breaks later
tests), save-level leaks (a successful raw `choice` holds its level
open *by design* -- logic tests must run save-wrapped), pool retention,
cumulative dict pressure.

### 5.3 `tests/run_all.sh` -- standalone sweep

Every `tests/test_*.trx` in its own interpreter with default config,
parameterized by three tables:

- `SPECIAL_ARGS` -- per-test extra flags (`--vm-size=2M` for the
  VM-hungry tests, `--vm-size=8M` for the overflow-guard trigger
  arrays, `--scratch-depth`, preload flags).  Each entry carries a
  comment justifying the budget.
- `SPECIAL_ENV` -- per-test environment.
- `SKIP_REASON` -- tests that need a gated-out feature (interactive
  debugger, heap tracking, the tetrix binary) or belong to another
  orchestrator (snapshot phases), so a release-build run skips them
  *visibly* instead of failing or silently passing.

Run against the optimized binary with `TRIX_BIN=./trix.opt` (required
for the perf tests, which time out under `-O0`+ASan).

### 5.4 Golden harness

`tests/golden/<name>.trx` + `<name>.expected`(+ optional
`<name>.exitcode`).  Each test runs in a fresh interpreter via a
**repo-relative path** (backtraces echo the path as invoked; goldens
must not bake in a checkout location), with a 15-second cap and an
exit-code check.  To bless an intentional change:

```bash
./trix tests/golden/<name>.trx < /dev/null > tests/golden/<name>.expected 2>&1
git diff tests/golden/   # review like source code -- this IS the spec
```

Anything printed must be deterministic: no live handles, no clock, no
VM offsets.  Verify the output is identical under `./trix` and
`./trix.opt` before blessing (hash-order or precision drift across
builds disqualifies the test from golden treatment).

### 5.5 PTY harness

For behavior a pipe/null stdin cannot reach: raw-mode blocking reads
(`@read-key-retry` continuations), interactive-REPL error recovery,
banner contracts.  Recipe per scenario:

```bash
script -qec "stty rows 24 cols 80; ./trix ..." /dev/null < <(feeder)
```

- A fresh `script` PTY is **0x0** -- the `stty` is load-bearing
  (`make-screen`/`terminal-size` raise `/range-check` without it).
- Feeders are timed (`sleep 2; printf 'A'; sleep 3`) so input arrives
  *after* the state under test (e.g. a blocked coroutine) is
  established.
- Assertions are marker greps on the combined capture; `!marker`
  asserts absence.  Raw PTY output carries echo, `\r`, and
  bracketed-paste noise -- byte-exact diffs do not fit here.
- `--quiet` silences ALL diagnostics (the fuzz-harness contract);
  scenarios that assert on error reports use `--no-banner` or neither.

### 5.6 GC stress, snapshot, backtrace

- **GC stress** (`run_gc_stress_tests.sh`): debug binary only; runs
  its scenarios under `vm-gc-stress` (+ poisoning) so any
  insufficiently-rooted allocation dies deterministically.
- **Snapshot** (`run_snapshot_tests.sh`): multi-phase scenarios --
  build state, `snap-shot`, kill the process, `thaw` in a new one,
  assert continuity (including coroutines, actors, streams).
- **Backtrace** (`run_backtrace_tests.sh`): 20 one-liner crashes, one
  per control structure, asserting the `[X boundary]` annotation that
  `format_backtrace()` must emit for each.

---

## 6. Adding tests

### 6.1 Where does it go?

| You are adding... | Put it... |
| --- | --- |
| Behavior assertions for an op | `tests/test_<subsystem>.trx` (or a new file); auto-discovered by `run_all.sh`; add a `run-test` line in `test_all.trx` unless it needs the delist treatment |
| A test needing special flags | Same, plus a justified `SPECIAL_ARGS` entry in `run_all.sh` |
| Exact-output behavior (print family, escape emission) | `tests/golden/` pair; wire-free (the runner globs) |
| Terminal-dependent behavior | A scenario in `tests/run_pty_tests.sh` (+ a `tests/pty/*.trx` file if it needs a script) |
| A crash-shaped input the fuzzer found | The minimized artifact into `tests/fuzz_regressions/` (replayed by every battery run) |
| An input *shape* the fuzzer should explore | A seed in `fuzz/seeds/` (§ 8.3) |

A new **operator** needs the full checklist of
[dev-adding-operators.md](dev-adding-operators.md) step 6: happy, sad
(every documented error), boundary, stress, and -- if it dispatches on
numeric type -- regeneration of the shim matrix (§ 6.4).

### 6.2 The `.trx` idioms

See [trix-reference.md § 6](trix-reference.md#6-test-writing-patterns)
for the canonical patterns: `rt-ok`, `save`/`restore` isolation,
VM-leak detection, `try` / `try-catch` error assertions.  House rules
that complement them:

- End every file with a completion line and `(__test_ok__) =`.
- `clear` after every assertion -- operand-stack discipline is part of
  the test.
- Error names: assert the *exact* name (`/type-check`, `/read-only`),
  never just "it errored".
- Never `def` a name that collides with an operator (`count`, `sum`,
  `product`, `store`, ...) -- in the shared-process suite the
  definition outlives your file and shadows systemdict for every test
  after it.
- Tests using raw `choice` (logic) must run save-wrapped: a successful
  raw `choice` holds its save level open by design.
- Eq-container (`#=`) values are one-shot per kind: use them before
  creating the next one, or the generation check raises
  `/unsupported`.

### 6.3 The three coverage axes

For an op that takes numeric operands, "tested" means:

1. **Type arms.**  One assertion per accepted operand type, operands
   type-matched (there is no implicit promotion -- `1 2.0 add` is a
   `/type-check`, and that sad arm wants an assertion too).  Suffix
   cheat sheet: `5b 5 5u 5l 5ul 5q 5uq 5a 5.0 5.0d`.
2. **Both regions.**  Run the same arms once normally and once under
   `true set-global ... false set-global` (or inside `${...}`): the
   ExtValue (l/ul/d/a) and WideValue (q/uq) traffic then exercises the
   global heap's allocator, free lists, and GC interplay.
3. **Sad paths.**  A mismatched pair, an out-of-domain operand, and
   any op-specific range/limit errors.

### 6.4 The shim matrix is generated

`tests/test_shim_matrix.trx` (~1,000 assertions) is **generated** (not
hand-written) from the dispatch rows and a per-op oracle table
(python-computed expecteds, Real-vs-Double agreement for functions the
stdlib lacks, custom range checks for precision-dependent ops).  Do not
hand-edit the `.trx`; change the generator and regenerate.

### 6.5 Measurement gotchas (gray-box tests)

- **Hoist literals out of the measured region.**  Scanning a proc
  literal allocates its embedded ExtValue/WideValue constants -- `def`
  the probe procs *before* reading the baseline counter, or the scan
  itself pollutes the delta.
- **Scale the workload.**  Before claiming an X-bytes-per-call leak,
  run the same workload at multiple N; constant overhead and pool
  warm-up look like leaks at a single N.
- **`< /dev/null` on every scripted `./trix` invocation.**  An
  automation harness handing the interpreter a never-EOF stdin turns
  any `read-key` path into a silent hang.
- **Use the optimized binary for anything iteration-heavy.**  The
  debug binary is 10-30x slower; a "quick" 4-billion-iteration loop is
  a half-hour mistake.

---

## 7. CI

Four workflows (`.github/workflows/`):

- **ci.yml** -- the gate: {gcc-15, clang-18} x {Debug, Release} build
  matrix, each leg running `./runtests.sh`; then README example
  execution and doc link/anchor verification
  (`tests/check_doc_links.py`); plus a clang-format job.
- **clang-tidy.yml** -- curated static analysis over the single TU
  (checks in `.clang-tidy`).
- **codeql.yml** -- security analysis, also weekly.
- **release.yml** -- tag-driven artifact build.

"Green before review" is all of ci.yml, clang-tidy.yml, and codeql.yml
-- see [CONTRIBUTING.md](../CONTRIBUTING.md).

---

## 8. Fuzzing

Coverage-guided libFuzzer harness in `fuzz/` (clang-20 + sanitizers);
[fuzz/README.md](../fuzz/README.md) is the operational manual.  The
short form:

### 8.1 Build and run

```bash
./fuzz/build.sh                      # clang-20; see README for build prerequisites
./fuzz/run.sh -max_total_time=3600   # budget-aware loop, auto-triage
```

One standing build quirk: keep the fuzz `LIBS` in lockstep with
`build.sh`'s (a library added to the main build otherwise breaks the
fuzz link weeks later).

**Use `-fork=N` for parallelism, never `-jobs=N`** -- jobs-mode
artifacts inherit orphan IPC pipes and replay as 2-minute false hangs.

### 8.2 Triage

`fuzz/triage.sh` classifies artifacts REAL / FALSE / CLEAN; the run
loop applies it automatically, parking false positives in
`fuzz/crashes_rejected/`.  A REAL crash gets: root-cause, fix,
minimized artifact promoted to `tests/fuzz_regressions/` (so the
battery replays it forever), and usually a sibling seed (below).

### 8.3 Seeds

`fuzz/seeds/` (~440 files) teaches the fuzzer the language's shapes;
the merged corpus (`fuzz/corpus/`, ~20k entries) is machine-evolved
from them.  What makes a good seed:

- **One construct, minimal scaffolding** -- the fuzzer mutates better
  from small valid programs than from kitchen-sink files.
- **Shape over value** -- a seed exercising `${...}` under a tight
  `--vm-global` (GC mid-operation), extreme `for` bounds, forged
  tagged payloads, deep continuation/effect nesting is worth more
  than a hundred arithmetic variations.
- **Every confirmed bug contributes its shape back** as a seed, so the
  fuzzer hunts the *class*, not just the instance: narrowing before a
  bounds check, GC-rooting windows, local/global region mismatches,
  error-walk completeness.
- Verify op names and stack order against the reference before
  committing a seed -- a seed that errors at scan time explores
  nothing.

### 8.4 What the fuzzer is allowed to find

Normal Trix errors on garbage input are correct behavior.  The fuzzer
hunts: sanitizer reports, asserts, signals, hangs past the timeout,
and (via the gray-box harness flags) leak counters that do not return
to baseline.

---

## 9. Coverage measurement

Coverage is measured with a gcov-instrumented build (`--coverage`, no
sanitizers) over `./trix`, run against the full battery -- the
standalone sweep, snapshot, backtrace, gc-stress, golden, PTY, the
in-process suite, eqref, sandbox, and the fuzz-regression replays --
emitting three gcov views: per-file summary, per-function, and per-line.

A gap analysis joins the per-function data against the dispatch rows:
`*_op` functions and `is_type_pred_op<...>` instances map 1:1 to
operators, and **shim template instantiations attribute to their op**
via the lambda in the mangled name -- so the report lists untested ops,
partially-covered ops, and per-op operand-type arms that never ran,
plus the ExtValue/WideValue machinery and allocator uncovered-line
drill-down.  Known accepted gaps -- intentionally-unreachable control
ops, the 2^32-iteration natural-drain arm of `@ulong-repeat`, and
debugger-UI-bound ops -- are tracked alongside the report.

Coverage percent is a *map*, not a goal: the targets that matter are
"zero untested operators," "every shim arm executed," and "every
documented error reachable by a test."

---

## See also

- [trix-reference.md § 6](trix-reference.md#6-test-writing-patterns) --
  the `.trx` assertion idioms (rt-ok, save-isolation, try patterns).
- [dev-adding-operators.md](dev-adding-operators.md) -- the new-operator
  checklist this doc's § 6 plugs into.
- [CONTRIBUTING.md](../CONTRIBUTING.md) -- entry-point quick table and
  the PR workflow.
- [fuzz/README.md](../fuzz/README.md) -- fuzzer operations in depth.
- [dev-invariants.md](dev-invariants.md) -- the runtime rules many of
  these tests exist to defend.
