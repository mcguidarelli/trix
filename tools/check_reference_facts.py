#!/usr/bin/env python3
"""Regression gate for facts corrected by the reference/cheatsheet doc audit
(trix-reference.md, operator-cheatsheet.md, errors-cheatsheet.md,
format-cheatsheet.md, error-handling.md, cli.md, host-integration.md,
trix-system.md, index.md), the concurrency-subsystem doc audit
(actors.md, coroutines.md, reactive.md), and the functional/control-subsystem
doc audit (closures.md, contracts.md, pattern-matching.md, effects.md,
functional-programming.md) and the I/O & data doc audit
(terminal-io.md, binary-pack.md), and the VM/memory/persistence doc audit
(vm-internals.md, save-restore.md, gvm-heap-gc.md, name-lookup.md, modules.md, snapshot-thaw.md, interpreter.md), and the dev/maintainer doc audit
(dev-invariants.md, dev-glossary.md, dev-testing.md, dev-architecture.md, dev-control-operators.md, dev-adding-operators.md, debugger.md).

Each probe below was a documentation defect the audit found and fixed -- a stack
effect that had drifted, an error name attributed to the wrong operator, a format
spec whose behaviour the doc misstated, or a convenience-dict key count. Locking
it as an executable probe means a future engine change that re-breaks the
documented behaviour fails CI, and the gate doubles as executable documentation
of the corrected facts. Companion to tools/check_scanner_grammar.py,
tools/check_type_facts.py, and tools/check_collection_facts.py.

Run: python3 tools/check_reference_facts.py [trix-binary]   (default ./trix.opt)
Exit 0 = all conform; 1 = mismatch (every offending case printed).
"""
import os
import subprocess
import sys
import tempfile


def run_line(binary, program):
    """Run a complete program verbatim; return (returncode, first stdout line)."""
    with tempfile.NamedTemporaryFile("w", suffix=".trx", delete=False) as fh:
        fh.write(program + "\n")
        path = fh.name
    try:
        r = subprocess.run([binary, path], capture_output=True, text=True)
    finally:
        os.unlink(path)
    line = r.stdout.splitlines()[0] if r.stdout.strip() else ""
    return r.returncode, line


# (program, expected `=`/print output on the first stdout line)
OK = [
    # `range` is `stop -- arr`, half-open [0, stop) -- ONE operand, upper bound
    # EXCLUDED (operator-cheatsheet.md 3.10, trix-reference.md 3.10).
    ("5 range ==", "[0i 1i 2i 3i 4i]"),
    # `range-from` is `start stop -- arr`, half-open [start, stop).
    ("1 5 range-from ==", "[1i 2i 3i 4i]"),
    # Precision on the integer `d` spec is NOT ignored: it sets the output radix
    # base (2..36), printed as BASE#digits (format-cheatsheet.md 2). 7 in base 3.
    ("({:.3d}) [ 7 ] aprint-fmt", "3#21"),
    # The `#` (alt-form) flag is a no-op for decimal: `{:#d}` == `{:d}`, no sign
    # change, no prefix (format-cheatsheet.md 3).
    ("({:#d}) [ 255 ] aprint-fmt", "255"),
    # `#` DOES add the radix prefix for hex/oct/bin.
    ("({:#x}) [ 255 ] aprint-fmt", "0xff"),
    # The `logic` convenience dict carries 17 short-name keys
    # (trix-reference.md 3.31).
    ("systemdict /logic get keys length =", "17"),
    # The `screen` convenience dict carries 14 keys (trix-reference.md 3.17.1).
    ("systemdict /screen get keys length =", "14"),
    # `unify` is NOT atomic: a structural mismatch leaves the lvars bound by the
    # earlier matching sub-elements bound (logic.md 3.2).  Here `[ c 1 ]` vs
    # `[ 9 99 ]` mismatches on element 1, yet c stays bound to 9.
    ("/c logic-var def [ c 1 ] [ 9 99 ] unify pop c deref =", "9"),
    # `choice` returns the first succeeding alternative's bindings (logic.md 4.1).
    ("/x logic-var def [ { x 1 unify guard } { x 2 unify guard } ] choice x deref =", "1"),
    # ---- Concurrency-subsystem audit (actors.md, coroutines.md, reactive.md) ----
    # The derived-cell operator is `cell-computed`, NOT `computed` (reactive.md
    # 3.2 titled it `computed`; the name `computed` does not exist).
    ("/cell-computed where =", "true"),
    ("/computed where =", "false"),
    # `=` prints the TYPE name for a non-scalar; on an array it prints `array`,
    # not the elements (reactive.md 3.12 showed `[2 4 6]` for an `=` invocation).
    ("[ 2 4 6 ] =", "array"),
    # `=` prints whole-valued doubles WITHOUT a trailing `.0` (reactive.md 5.2
    # wrote `540.0`); fractional doubles keep their decimals.
    ("540.0 =", "540"),
    ("540.5d =", "540.5"),
    # `actor-exit` / `actor-trap-exit` ARE registered (implemented in
    # ops_supervision.inl); the actors.md File Map count omitted them.
    ("/actor-exit where =", "true"),
    ("/actor-trap-exit where =", "true"),
    # `is-actor` and `is-coroutine` exist as standard predicates -- counted in
    # the 20-standard-actor / 19-standard-coroutine File-Map figures.
    ("/is-actor where =", "true"),
    ("/is-coroutine where =", "true"),
    # `coroutine-die` does NOT opt out of returning a value: a non-empty-stack
    # die captures its top as the return value (coroutines.md 5.7 listed it as a
    # suppress-the-return path).  coroutine-join's flag is true iff a value came
    # back -- true for `7 coroutine-die`, false for an empty-stack die.
    ("mark { 7 coroutine-die } coroutine-launch coroutine-join =", "true"),
    ("mark { coroutine-die } coroutine-launch coroutine-join =", "false"),
    # Default actor mailbox capacity is MaxActorMailboxCapacity = 64, not 16
    # (actors.md 5.6 Ring Pipeline claimed capacity-16).
    ("mark { actor-self actor-mailbox-capacity = } actor-spawn coroutine-join", "64"),
    # ---- Functional/control audit (closures, contracts, pattern-matching, effects, functional-programming) ----
    # `precondition` takes a BOOLEAN, not a proc: `true precondition` is a
    # no-op (contracts.md 5.1).  (`false precondition` -> /require, in ERR.)
    ("true precondition (ok) =", "ok"),
    # `to-str` does not exist -- the real op is `to-string` (closures.md 7.1
    # used the nonexistent `to-str`).
    ("/to-str where =", "false"),
    # There is no bare `die` operator -- program termination is `quit`
    # (pattern-matching.md 4.1 used the nonexistent `die`).
    ("/die where =", "false"),
    ("/quit where =", "true"),
    # `handle-effect` leaves the body's value -- stack effect ends `-- result`
    # (effects.md 10 table dropped the result).
    ("<< /ask { pop 42 } >> { /ask perform } handle-effect =", "42"),
    # The typed `for` loop CLAMPS on control-variable overflow and terminates
    # gracefully -- it never raises /numerical-overflow (functional-programming.md
    # 15.5 claimed signed types raise it).  Counting near INT32_MAX completes.
    ("2147483645 1 2147483647 { pop } for (clamped-ok) =", "clamped-ok"),
    # ---- I/O & data audit (terminal-io) ----
    # `screen?` is the 15th screen op -- the terminal-io.md 4.5 table listed 14
    # and omitted the type predicate.  It exists and tests screen handles.
    ("/screen? where =", "true"),
    ("80 24 make-screen screen? =", "true"),
    # ---- VM/memory/persistence audit (gvm-heap-gc, name-lookup) ----
    # `protocoldict` is the 3rd PERMANENT dict on the lookup chain (systemdict +
    # protocoldict + userdict) -- name-lookup.md 6 omitted it (said 2 permanent).
    ("/protocoldict where =", "true"),
    # `vm-global-gc` exists and returns the byte count reclaimed by the sweep
    # (gvm-heap-gc.md showed it with no stack effect).
    ("/vm-global-gc where =", "true"),
    # ---- Dev/maintainer audit (dev-invariants) ----
    # ExtValue/WideValue active counts carry a fixed NONZERO idle baseline (28/4)
    # from the systemdict `numbers` constants -- dev-invariants.md said they
    # "should be 0 when idle".  A leak is a DRIFT above this baseline, not nonzero.
    ("/extvalue-active query-status =", "28"),
    ("/widevalue-active query-status =", "4"),
]
# NOTE: `breakpoint` being debug-build-only (where -> false in ./trix.opt, true
# in the debug ./trix) is a real doc fact (trix-reference.md 3.22) but is
# deliberately NOT probed here: its result flips with the build, and this gate
# must hold identically against both binaries.

# (program, expected process exit code)  -- error codes per src/types.inl Error enum
ERR = [
    # `aggregate` is `init reducer [alts] -- result` (3 operands; alts is ONE
    # array). The old 4-operand `init step template proc` form type-checks.
    ("0 {add} /t {1} aggregate", 40),
    # `find-all` is `[alts] -- arr` (a single array of procs); the old
    # `template proc` two-operand form type-checks (operator-cheatsheet.md 3.31).
    ("/x { 1 } find-all", 40),
    # `perform` is `...args effect-name -- result` (Name on TOP). The doc's old
    # `effect-name args` order puts a non-Name on top -> /type-check.
    ("<< /ask { } >> { /ask 100 perform } handle-effect", 40),
    # `actor-spawn` / `coroutine-launch` are `mark obj* proc -- coroutine`: a
    # bare `{...} op` with no mark raises /unmatched-mark (operator-cheatsheet).
    ("{ 1 } actor-spawn", 44),
    ("{ 1 } coroutine-launch", 44),
    # Not every transducer is valid for a pipe target: xf-take / xf-scan raise
    # /unsupported (not /type-check) via pipe-into (trix-reference.md 3.36).
    ("4 pipe-buffer 3 xf-take pipe-into", 45),
    ("4 pipe-buffer 0 { add } xf-scan pipe-into", 45),
    # `coroutine-release` on a LIVE coroutine raises /invalid-exit, not
    # /invalid-access (trix-reference.md 3.33).
    ("mark { 1 2 3 } coroutine-launch coroutine-release", 17),
    # `make-executable` rejects Tagged/Record/Coroutine/Address/OpaqueHandle
    # (VerifyExecutable mask, src/verify.inl) -> /type-check on a Tagged value.
    ("1 /x tag make-executable", 40),
    # /undefined-case is raised by `case` (no default) and `type-case` -- NOT by
    # `tag-match` (which raises /type-check) (errors-cheatsheet.md).
    ("mark /x << /a 1 >> /missing case", 42),
    ("42 mark << /string { } >> type-case", 42),
    # /undefined-result is raised by fmod/remainder with an INFINITE operand
    # (errors-cheatsheet.md) -- not by log/sqrt of a negative...
    ("inf 5.0 fmod", 43),
    ("inf 5.0 remainder", 43),
    # ...which actually raise /range-check (the doc previously misattributed
    # these to /undefined-result).
    ("-1.0 log", 31),
    ("-1.0 sqrt", 31),
    # And fmod BY ZERO is /div-by-zero, a third distinct outcome.
    ("5.0 0.0 fmod", 5),
    # Indexing past an array with `get` raises /index-check (14), NOT
    # /range-check (contracts.md 1.1 misattributed `[] 0 get` to /range-check).
    ("[] 0 get", 14),
    # `false precondition` raises /require (52); precondition is the contract
    # precondition primitive over a Boolean (contracts.md 5.1/5.2).
    ("false precondition", 52),
]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "trix.opt")
    if not os.path.exists(binary):
        binary = os.path.join(root, "trix")
    if not os.path.exists(binary):
        print("FAIL: no trix binary found (looked for ./trix.opt, ./trix)", file=sys.stderr)
        return 1

    fails = []
    n = 0
    for prog, exp in OK:
        n += 1
        rc, line = run_line(binary, prog)
        if rc != 0 or line != exp:
            got = f"exit={rc}" if rc != 0 else repr(line)
            fails.append(f"OK  {prog[:62]:62} expected {exp!r}, got {got}")
    for prog, code in ERR:
        n += 1
        rc, _ = run_line(binary, prog)
        if rc != code:
            fails.append(f"ERR {prog[:62]:62} expected exit {code}, got exit {rc}")

    if fails:
        print(f"check_reference_facts: {len(fails)} MISMATCH (of {n} probes)\n")
        for f in fails:
            print("  " + f)
        return 1
    print(f"check_reference_facts: {n} reference facts all conform")
    return 0


if __name__ == "__main__":
    sys.exit(main())
