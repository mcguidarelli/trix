# Trix Test Suite

Functional tests, golden-output tests, terminal/PTY scenarios, and
static-analysis gates for the Trix interpreter.  Tests are written in Trix
itself (`*.trx`) and driven by shell runners; a handful of Python scripts
guard the docs/source consistency contracts.

## Running the suite

```bash
./build.sh && ./runtests.sh          # full suite against the debug build (./trix)
TRIX_BIN=./trix.opt ./tests/run_all.sh   # the .trx suite against the optimized build
```

`runtests.sh` (repo root) runs the whole battery with the required flags and
fails on the first crash or missing success marker.  `tests/run_all.sh` runs
every `test_*.trx` in its own `./trix` invocation (one process per test, so
dict state cannot leak across tests) and prints a classified summary.  The
binary under test defaults to `./trix`; set `TRIX_BIN=./trix.opt` to exercise
the optimized build (required for the perf-sensitive tests).

## Layout

| Path | What it holds |
| --- | --- |
| `test_*.trx` | 266 feature/unit tests (the bulk of the suite); a clean run prints the `__test_ok__` sentinel that `run_all.sh` checks for |
| `golden/` | Exact-stdout tests: `name.trx` + `name.expected` (+ optional `name.args`) |
| `cli/` | Command-line / `parse_args` matrix |
| `pty/` | Terminal-dependent scenarios run under a real pseudo-terminal |
| `snapshot/` | Snap-shot / thaw image round-trips |
| `integration/` | Multi-feature integration scenarios |
| `smoke/` | Minimal smoke checks |
| `fuzz_regressions/` | `crash-*` reproducers from the fuzzer, replayed as regressions |

## Specialized runners

Each runner drives one slice of the battery and exits non-zero on failure:

| Runner                      | Scope                                     |
| --------------------------- | ----------------------------------------- |
| `run_all.sh`                | Every `test_*.trx`, one process per test  |
| `run_golden_tests.sh`       | Exact-stdout golden tests in `golden/`    |
| `run_cli_tests.sh`          | CLI flag / argument-parsing matrix        |
| `run_pty_tests.sh`          | Terminal scenarios under a real PTY       |
| `run_snapshot_tests.sh`     | Orchestrated snap-shot / thaw suite       |
| `run_backtrace_tests.sh`    | `format_backtrace()` boundary annotations |
| `run_gc_stress_tests.sh`    | Garbage-collector stress phase            |
| `run_sched_stress_tests.sh` | Scheduler stress phase                    |
| `error_codes.sh`            | Process exit-code mapping                 |

## Static-analysis gates

These Python scripts keep the documentation and the source in lockstep --
each treats the source as the single source of truth and exits non-zero on
drift:

| Script                      | Contract guarded                                                     |
| --------------------------- | -------------------------------------------------------------------- |
| `check_doc_links.py`        | Every relative markdown link + anchor in the shipped docs resolves   |
| `check_error_codes.py`      | Error-code docs match the `Error` enum                               |
| `check_operator_count.py`   | Operator counts cited in the docs match the dispatch table           |
| `check_operator_throws.py`  | Per-operator "throws" docs match the `// throws:` annotations in src |
| `check_operator_shadows.py` | Flags `.trx` names that shadow a built-in operator                   |
| `check_readme_examples.py`  | Every ` ```trix ` block in `README.md` executes cleanly              |

## Writing a test

Most `test_*.trx` files define a restore-immune failure counter and assert
with a small `rt-ok` helper, then throw at EOF if any assertion failed (so a
print-only failure cannot slip the suite verdict).  See an existing
`test_*.trx` for the pattern, and consult
[`docs/trix-reference.md`](../docs/trix-reference.md) for stack effects and
error names before writing new `.trx` code.
