tests/fuzz_regressions/ -- replay-as-regression for fuzz-found bugs

Each file here is a fuzzer-minimized input that previously crashed Trix
via a sanitizer report, assert, or signal.  After the bug is fixed, the
file moves here from fuzz/crashes/kept/ and runtests.sh replays it
against the binary; a fresh crash on any of these fails the suite.

Kept as opaque inputs (not rewritten as clean .trx regression tests)
because the fuzzer's exact byte sequence is the regression -- mutating
it into "cleaner" code can lose the path that triggered the bug.

Naming: crash-<sha1> matches the libFuzzer artifact filename.  Commit
message that added the file should name the root-cause fix.

Current contents:
- crash-51ee... / crash-5e96... / crash-b40a... : all three reproduced
  the same op_count underflow in capture_op (|locals| inside delimit
  body consuming pre-delimit op items).  Fixed in commit 5915156.
- crash-c722... / crash-c782... : both hit `is_integer()` assert in
  Object::integer_value() via stack_probe_op.  Multi-type integer
  operand (Long, etc.) accepted by `verify_operands(VerifyIntegers)`
  was then read with bare `integer_value()`, which only handles
  Type::Integer.  Audit also caught chmod_op and write_op with the
  same pattern; all three switched to `integer_value(trx, lo, hi)`.
