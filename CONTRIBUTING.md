# Contributing to Trix

Thanks for your interest in Trix.

Trix is a solo-authored project with a specific technical direction
(embeddable stack-based VM, cooperative concurrency, deterministic
memory, Apache 2.0).  Contributions are welcome, but be aware that
feature PRs that don't fit the direction may be declined.  Bug fixes
and documentation improvements are almost always welcome.

This document explains how to report issues, propose changes, and
submit pull requests.

---

## Reporting Bugs

Open a GitHub issue with:

- **What happened:** the symptom, including any error messages or
  stack traces.
- **Minimal reproducer:** a small `.trx` script (or C++ harness) that
  triggers the problem.  Smaller is better.
- **Environment:** `trix --about` output, OS, compiler version.
- **Expected vs actual behavior.**

For security-sensitive issues, see [SECURITY.md](SECURITY.md) --
report privately, not in a public issue.

---

## Proposing Features

Before writing code, **open an issue describing the proposed feature
and your design sketch**.  This avoids wasted effort on changes that
don't fit the project direction.  If the proposal is accepted in
principle, you'll get a go-ahead to implement it.

Small, self-contained improvements (a missing operator, a
documentation fix, a clearer error message) don't need a pre-issue --
a PR with a clear description is fine.

---

## Building

The day-to-day developer build is `./build.sh` (not CMake):

| Command | What you get |
| --- | --- |
| `./build.sh` | Debug build of `./trix`, `./tetrix`, and `./chip8`: `-O0`, ASan + UBSan, `-Werror`, all optional features (debugger, vm-heap-tracking) enabled.  Silent on success. |
| `./build.sh -v` | Same, echoing the compile commands. |
| `./build.sh --release` | Feature-stripped debug build: optional features disabled to validate the compile-time gating.  Still `-O0` + ASan -- not an optimized release. |
| `./build.sh --optimized` | Optimized build, written to `./trix.opt`, `./tetrix.opt`, and `./chip8.opt`: `-O3`, `-DNDEBUG`, no sanitizers, stripped, `-fhardened`.  The sanitized `./trix` is left in place. |

Requirements: `gcc-15`, `libreadline-dev`, `zlib1g-dev`.
Override the compiler with `CXX=<compiler> ./build.sh`.

CMake (`cmake -B build -DCMAKE_BUILD_TYPE=Release`) is the canonical
path for packaged artifacts and what CI builds; `build.sh` and
`CMakeLists.txt` carry the same flags and must stay in lockstep -- if
you change flags in one, change the other.

---

## Testing

Nine entry points, layered from "everything" to targeted phases:

| Entry point | Covers |
| --- | --- |
| `./runtests.sh` | The full battery: `test_all.trx` master runner, standalone per-test sweep, golden-stdout tests, PTY scenarios, CLI matrix, snapshot tests, eqref overflow, sandbox-mode tests, fuzz-regression replay.  This is what CI runs; a crash or missing success marker is a FAIL. |
| `tests/run_all.sh` | Every `tests/test_*.trx` standalone, one interpreter per test (catches dict-state leaks that the in-process master runner cannot). |
| `tests/run_golden_tests.sh` | Exact-stdout pinning for the print family (`=`, `==`, `stack`, ...): each `tests/golden/<name>.trx` is diffed byte-for-byte against its `.expected` blessing, with a 15s hang cap and an exit-code check. |
| `tests/run_pty_tests.sh` | Terminal-dependent scenarios under a real PTY (`script` + `stty`): the read-key blocking continuations and interactive-REPL error recovery.  Marker-grep assertions (raw PTY output carries echo/CR noise). |
| `tests/run_gc_stress_tests.sh` | GC-stress phase: runs under the debug-only `vm-gc-stress` operator, so it requires a debug binary. |
| `tests/run_snapshot_tests.sh` | Orchestrated multi-phase snap-shot / thaw scenarios across separate invocations. |
| `tests/run_backtrace_tests.sh` | Backtrace boundary annotations: each scenario crashes inside a specific control structure and the stderr backtrace must carry the expected `[X boundary]` line. |
| `tests/run_cli_tests.sh` | CLI matrix: `parse_args` flags, exit codes, and startup modes. |
| `tests/run_sched_stress_tests.sh` | Scheduler-stress phase (cooperative scheduling under load). |

The runners default to the debug `./trix`.  To run against the
optimized binary (required for the perf tests, which time out under
`-O0` + ASan): `TRIX_BIN=./trix.opt tests/run_all.sh`.

Tests are written with the `rt-ok` / `rt-error` assertion patterns;
see the test-writing patterns section of
[docs/trix-reference.md](docs/trix-reference.md) and any existing
`tests/test_*.trx` for the idiom.  New operators need happy-path,
sad-path, stress, and boundary coverage.

For the full picture -- testing philosophy, every suite and harness in
detail, the black/gray/white-box taxonomy, fault-injection instruments,
fuzz-seed guidance, and the coverage-measurement methodology -- see
[docs/dev-testing.md](docs/dev-testing.md).

---

## Pull Request Workflow

1. Fork the repository and create a topic branch from `master`.
2. Make your changes.  Keep commits focused and write descriptive
   commit messages.
3. Run `clang-format -i --style=file` on every `.cpp`, `.h`, and
   `.inl` file you touched.  CI enforces this.
4. Build and run the full test suite locally:
   ```bash
   ./build.sh
   ./runtests.sh
   ```
   All tests must pass before the PR is reviewed.
5. If your change adds or modifies behavior, **add tests** in
   `tests/` using the `rt-ok` / `rt-error` assertion patterns (see
   existing tests for examples).
6. Sign off your commits (see **DCO** below).
7. Open a PR against `master` with a description of what the change
   does and why.

CI runs on every PR.  It must be green before review.

---

## Continuous Integration

Four workflows under `.github/workflows/`:

| Workflow | Trigger | What it does |
| --- | --- | --- |
| `ci.yml` | push to `master`, every PR | Build matrix {gcc-15, clang-20} x {Debug, Release}; stages the binary and runs `./runtests.sh`, then verifies the README examples still execute and all doc links/anchors resolve.  The build-and-test job also runs the fact/doc gate suite (operator-count, error-codes, per-operator throws, operator-shadows, doc-example execution, scanner-grammar, type-facts, collection-facts, reference-facts).  A separate `signed-char` job rebuilds under `-fsigned-char` with `-Wsign-conversion` to catch signedness defects.  The `clang-format` check job also runs `cpp_style.py` and `assert_blank_line.py`. |
| `clang-tidy.yml` | push / PR on `master` | Static analysis (clang-tidy-20 over the single TU; checks curated in `.clang-tidy`). |
| `codeql.yml` | push / PR on `master` | GitHub CodeQL security analysis. |
| `release.yml` | semver tag push (`v*.*.*`) | Release artifact build (see *Cutting a Release*). |

"Green before review" means `ci.yml` (all four matrix legs plus the
clang-format job), `clang-tidy.yml`, and `codeql.yml`.  Do not "fix"
unrelated clang-tidy noise in a feature PR.

---

## Cutting a Release (Maintainers)

Releases are tag-driven.  Pushing a semver tag runs `release.yml`:

```bash
git tag -a v0.9.0 -m "Trix 0.9.0"
git push origin v0.9.0
```

The workflow checks out with tag history, builds a Release binary
with gcc-15 (CMake, no sanitizers), strips it, smoke-tests
`trix --version`, then packages a tarball with a SHA256 checksum and
attaches both to a GitHub Release for the tag.

---

## Developer Certificate of Origin (DCO)

All commits must be signed off under the [Developer Certificate of
Origin 1.1](https://developercertificate.org/).  Sign-off is a simple
statement that you wrote the patch yourself, or otherwise have the
right to submit it under the project's Apache 2.0 license.

To sign off a commit, add the `-s` flag to `git commit`:

```bash
git commit -s -m "Your commit message"
```

This appends a line like:

```
Signed-off-by: Your Name <your.email@example.com>
```

to the commit message.  The name and email must match your git
configuration.  PRs with unsigned commits will not be merged.

There is no CLA (Contributor License Agreement); DCO is the only
provenance requirement.

---

## Signed Commits (Recommended)

Cryptographically signed commits (`git commit -S`) are recommended
but not required.  GitHub will display a "Verified" badge on signed
commits.  See [GitHub's signing
guide](https://docs.github.com/en/authentication/managing-commit-signature-verification)
for setup instructions (SSH or GPG both work).

---

## Code Style

See [STYLE.md](STYLE.md) for code style rules (formatting,
naming conventions, language restrictions, comment style).  See
[docs/dev-invariants.md](docs/dev-invariants.md) for architectural
invariants the runtime depends on.

New operators have a step-by-step checklist in
[docs/dev-adding-operators.md](docs/dev-adding-operators.md).

---

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md).
Be civil.  Report unacceptable behavior to the email in that file.

---

## License

By contributing, you agree that your contributions will be licensed
under the [Apache License 2.0](LICENSE) -- the same license as the
project.  The DCO sign-off is how that agreement is recorded.
