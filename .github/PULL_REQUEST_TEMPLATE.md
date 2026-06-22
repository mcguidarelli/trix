<!-- Thanks for the PR!  Please fill out the sections below. -->

## Summary

<!-- What does this PR do, and why? -->

## Linked issue

<!-- #issue-number if this PR implements a discussed proposal; "none"
for trivial fixes and typos. -->

## Changes

<!-- Brief bullet list of what changed.  Keep it high-level; the
commit messages carry the detail. -->

-

## Test plan

- [ ] Added tests for new behavior in `tests/` (or `benchmark/` if
      performance-relevant).
- [ ] Ran `clang-format -i --style=file` on every modified `.cpp`,
      `.h`, or `.inl`.
- [ ] Built clean with `cmake -B build && cmake --build build`.
- [ ] `./runtests.sh` reports 0 failures (Trix + snapshot suites).

## Checklist

- [ ] Commits are signed off (`git commit -s`) per the DCO in
      [CONTRIBUTING.md](../CONTRIBUTING.md).
- [ ] CI is green (GitHub Actions will run automatically on push).
- [ ] Docs updated (`README.md`, `docs/`, `CHANGELOG.md`) where
      user-visible behavior changed.
