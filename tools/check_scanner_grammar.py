#!/usr/bin/env python3
"""Conformance gate for the documented scanner grammar (suffixes + infix).

WHY THIS EXISTS
    The scanner's grammar is documented as EBNF prose and precedence tables in
    docs/scanner.md, docs/scanner-syntax.md, and docs/infix-design.md.  Prose
    is invisible to tools/check_doc_examples.py (which only runs code fences),
    so a grammar line can drift from the scanner for years unnoticed -- it did:
    scanner.md advertised a `#g` storage class the scanner never accepted and
    dropped `#$`/`#$$` from every container; scanner-syntax.md lumped unary `-`
    with `+`/`!`/`~` even though `-` binds looser than `**`.

    This gate ties the documented grammar to the actual parser two ways:
      (A) IMPL conformance -- every accepted form (and curated near-misses) is
          run through the real binary and its accept / reject / result must
          match.  Change the scanner and this fails.
      (B) DOC conformance -- each canonical grammar string must appear in the
          docs and none of the known-bad forms may.  Edit a grammar line or
          precedence table back to a wrong shape and this fails.

GROUND TRUTH (src/scanner.inl, src/scanner_infix.inl):
    scan_string_suffix / scan_byte_array_suffix : string forms
    scan_proc_suffix                            : { proc }
    scan_eq_rw_suffix                           : [ arr ] << dict >> {{ set }}
    infix_binary_op / infix_primary / infix_expr: $( ) strict, $[ ] promoted

Usage: python3 tools/check_scanner_grammar.py [trix-binary]   (default ./trix.opt)
Exit 0 = conforms; 1 = mismatch (every offending case printed).
"""
import itertools
import os
import subprocess
import sys
import tempfile

# Trix CLI exits with the numeric error code; these two are load-bearing here.
EXIT_SYNTAX_ERROR = 39
EXIT_UNSUPPORTED = 45

OK, SYNTAX, UNSUPPORTED = "ok", "syntax", "unsupported"

# A representative literal per collection kind.  '(A)' is length 1 so it is
# legal for the string #b (single-byte) production as well as every other.
LITERAL = {
    "string": "(A)",
    "array": "[1 2 3]",
    "dict": "<< /a 1 >>",
    "set": "{{ 1 2 3 }}",
    "proc": "{ dup mul }",
}


def run_program(binary, program):
    with tempfile.NamedTemporaryFile("w", suffix=".trx", delete=False) as fh:
        fh.write(program)
        path = fh.name
    try:
        return subprocess.run([binary, path], capture_output=True, text=True)
    finally:
        os.unlink(path)


# ---- (A1) suffix grammar -------------------------------------------------

def classify_suffix(binary, literal, suffix):
    """Run `<literal>#<suffix>` as proc-stored data and classify the outcome."""
    host = literal if suffix == "" else f"{literal}#{suffix}"
    # Wrap in a deferred `{ ... }` body: an executable string (#x) or name
    # auto-executes the instant the interpreter reaches it at top level, which
    # would misclassify a perfectly-scanned #x as a runtime /undefined.  Inside
    # a proc body the value is collected as data and never run, while scan-time
    # errors (SyntaxError, the #pw Unsupported) still fire during the inner scan.
    rc = run_program(binary, f"/__probe {{ {host} }} def clear\n").returncode
    if rc == 0:
        return OK
    if rc == EXIT_SYNTAX_ERROR:
        return SYNTAX
    if rc == EXIT_UNSUPPORTED:
        return UNSUPPORTED
    return f"other(exit={rc})"


def opt(*values):
    """An optional grammar slot: empty string plus each alternative."""
    return ("",) + values


def suffix_accept_cases():
    """Yield (kind, suffix, expected_class) for every suffix the grammar admits."""
    for kind in LITERAL:
        yield kind, "", OK  # bare literal (no '#')

    # string: a[r|w] | b | [=|$|$$][l|x][r|w]
    for acc in opt("r", "w"):
        yield "string", "a" + acc, OK
    yield "string", "b", OK
    for store, form, acc in itertools.product(opt("=", "$", "$$"), opt("l", "x"), opt("r", "w")):
        if store + form + acc:
            yield "string", store + form + acc, OK

    # array / dict / set: [=|$|$$][r|w]
    for kind in ("array", "dict", "set"):
        for store, acc in itertools.product(opt("=", "$", "$$"), opt("r", "w")):
            if store + acc:
                yield kind, store + acc, OK

    # proc: [=|$|$$][a|p][e|l][r|w]; packed (no 'a') + explicit 'w' -> Unsupported
    for store, form, bind, acc in itertools.product(
        opt("=", "$", "$$"), opt("a", "p"), opt("e", "l"), opt("r", "w")
    ):
        sfx = store + form + bind + acc
        if not sfx:
            continue
        packed = form != "a"
        yield "proc", sfx, (UNSUPPORTED if (packed and acc == "w") else OK)


# Curated near-misses: each must be a hard SyntaxError.  These pin the exact
# failure modes the docs got wrong (a bogus `g` class, dropped mutex
# enforcement, letters borrowed from the wrong kind, slot order).
SUFFIX_REJECT = {
    "string": ["g", "z", "ax", "e", "p", "=$", "$=", "=$$", "$$$", "ap"],
    "array": ["g", "a", "p", "e", "l", "x", "b", "=$", "$=", "$$$", "lr"],
    "dict": ["g", "a", "p", "e", "l", "x", "b", "=$", "$=", "$$$"],
    "set": ["g", "a", "p", "e", "l", "x", "b", "=$", "$=", "$$$"],
    "proc": ["g", "z", "ax", "ea", "xl", "ap", "pa", "b", "x", "=$", "$=", "$$$"],
}


def suffix_reject_cases():
    for kind, suffixes in SUFFIX_REJECT.items():
        for sfx in suffixes:
            yield kind, sfx, SYNTAX


# ---- (A2) infix grammar --------------------------------------------------

REJECT_INFIX = object()  # sentinel: the expression must error (nonzero exit)


def eval_infix(binary, expr):
    """Run `<expr> =` and return (returncode, first stdout line)."""
    r = run_program(binary, f"{expr} =\n")
    line = r.stdout.splitlines()[0] if r.stdout.strip() else ""
    return r.returncode, line


# (expr, expected) -- expected is the exact `=` printout, or REJECT_INFIX.
# Operand types satisfy each operator (and/or/xor want unsigned; shift wants an
# unsigned value + signed count; pow is float-only) so every probe isolates
# GRAMMAR (precedence / associativity / dispatch / promotion), not typing.
INFIX = [
    ("$( 2 + 3 * 4 )", "14"),            # * (12) binds tighter than + (11)
    ("$( 2 * 3 + 4 )", "10"),
    ("$( (2 + 3) * 4 )", "20"),          # grouping overrides
    ("$( 10 - 2 - 3 )", "5"),            # - left-associative
    ("$( 20 / 2 / 2 )", "5"),            # / left-associative
    ("$( 10 % 3 )", "1"),
    ("$( 2.0 ** 3.0 )", "8"),            # ** (13), float-only
    ("$( 2.0 ** 3.0 ** 2.0 )", "512"),   # ** right-associative -> 2**(3**2)
    ("$( -2.0 ** 2.0 )", "-4"),          # unary - binds LOOSER than ** -> -(2**2)
    ("$( -5 )", "-5"),
    ("$( +5 )", "5"),                    # unary + identity (binds tightest)
    ("$( !true )", "false"),             # unary not (binds tightest)
    ("$( !false )", "true"),
    ("$( 7u & 3u )", "3"),               # and (7)
    ("$( 7u && 3u )", "3"),              # && is an alias of &
    ("$( 5u | 2u )", "7"),               # or (5)
    ("$( 5u || 2u )", "7"),              # || is an alias of |
    ("$( 6u ^ 3u )", "5"),               # xor (6)
    ("$( 8u >> 1 )", "4"),               # shift-right (10)
    ("$( 1u << 4 )", "16"),              # shift-left (10)
    ("$( 1u << 2 + 1 )", "8"),           # + (11) tighter than << (10) -> 1<<3
    ("$( 3 < 5 )", "true"),              # comparisons (9)
    ("$( 5 <= 5 )", "true"),
    ("$( 3 == 3 )", "true"),             # equality (8)
    ("$( 3 != 4 )", "true"),
    ("$( true ? 10 : 20 )", "10"),       # ternary (4), eager
    ("$( false ? 10 : 20 )", "20"),
    ("$( max(3, 7) )", "7"),             # function call: args then name
    ("$( abs(-5) )", "5"),
    ("$( sqrt(9.0) )", "3"),
    ("$[ 3 + 4.0 ]", "7"),               # $[ ] auto-promotes int + real
    ("$( 3 + 4.0 )", REJECT_INFIX),      # $( ) strict: mixed types reject
    ("$(  )", REJECT_INFIX),             # empty expression
    ("$( 1 + 2", REJECT_INFIX),          # unclosed
    ("$( 1 ? 2 )", REJECT_INFIX),        # malformed ternary (missing ':')
]


# ---- (A3) miscellaneous grammar facts ------------------------------------
# Behaviours the full scanner-grammar audit corrected in the docs, locked here so a
# future parser change that re-breaks them fails the gate. Each value is probe-confirmed.

# (program, expected `=` output)
MISC_OK = [
    ("16#FF =", "255"),               # radix closing # is OPTIONAL
    ("16#FF# =", "255"),
    ("2#1010 =", "10"),
    ("36#Z =", "35"),                 # radix base up to 36
    ("5q type =", "int128-type"),     # q suffix -> Int128
    ("5uq type =", "uint128-type"),   # uq suffix -> UInt128
    ("inf#r type =", "real-type"),    # inf#r is a Real alias of inf
    ("[] length =", "0"),             # empty array has length 0 (no minimum capacity)
    ("(\\501) 0 get =", "A"),         # octal escape mod 256: \501 (0o501=321) -> byte 65 'A'
    ("/$$$ type =", "name-type"),     # $$$ is an ordinary name, not a special prefix
    ("{{ inf }} length =", "1"),      # Inf is a valid SET element
]

# (program, expected process exit code)
MISC_ERR = [
    ("[1 2 3]#r 0 5 put", 32),        # write to ReadOnly -> /read-only (NOT invalid-access)
    ("1 2 3 ]", 44),                  # lone ] with no mark -> /unmatched-mark (NOT syntax-error)
    ("<< inf 1 >> length =", 26),     # Inf dict KEY -> /numerical-inf (rejected, unlike sets)
    ("25 sqrt =", 40),                # sqrt of Integer -> /type-check (sqrt is float-only)
]


def run_line(binary, program):
    """Run a complete program verbatim; return (returncode, first stdout line)."""
    r = run_program(binary, program + "\n")
    line = r.stdout.splitlines()[0] if r.stdout.strip() else ""
    return r.returncode, line


# ---- (B) doc conformance -------------------------------------------------

CANONICAL = {
    "docs/scanner.md": [
        "#[a[r|w] | b | [=|$|$$][l|x][r|w]]",
        "#[=|$|$$][r|w]",
        "#[=|$|$$][a|p][e|l][r|w]",
    ],
    "docs/scanner-syntax.md": [
        "#[=|$|$$][l|x][r|w]",
        "#a[r|w]",
        "#[=|$|$$][r|w]",
        "#[=|$|$$][a|p][e|l][r|w]",
        "binds **looser** than",  # the unary `-` precedence exception
    ],
    "docs/infix-design.md": [
        "binds **looser** than",
    ],
}
# Forms that must NOT reappear (storage-class drift, fictional `g`, the
# unary `-` lumped in with the tight-binding prefix ops).
FORBIDDEN = {
    "docs/scanner.md": ["#[=][r|w]", "#[=][a|p]", "[l|x][r|w] | g]", "#[=][l|x]"],
    "docs/scanner-syntax.md": ["#[=][a|p]", "#[=][r|w]", "unary `-` `+` `!` `~`"],
}


def check_docs(root):
    failures = []
    for rel, needles in CANONICAL.items():
        text = open(os.path.join(root, rel), encoding="utf-8").read()
        for needle in needles:
            if needle not in text:
                failures.append(f"DOC  {rel}: missing canonical grammar  {needle!r}")
    for rel, bads in FORBIDDEN.items():
        text = open(os.path.join(root, rel), encoding="utf-8").read()
        for bad in bads:
            if bad in text:
                failures.append(f"DOC  {rel}: forbidden (drifted) grammar  {bad!r}")
    return failures


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "trix.opt")
    if not os.path.exists(binary):
        binary = os.path.join(root, "trix")
    if not os.path.exists(binary):
        print("FAIL: no trix binary found (looked for ./trix.opt, ./trix)", file=sys.stderr)
        return 1

    failures = []
    n = 0

    for kind, sfx, expected in itertools.chain(suffix_accept_cases(), suffix_reject_cases()):
        n += 1
        got = classify_suffix(binary, LITERAL[kind], sfx)
        if got != expected:
            shown = f"{LITERAL[kind]}#{sfx}" if sfx else LITERAL[kind]
            failures.append(f"SUFFIX {kind:6} {shown:24} expected {expected}, got {got}")

    for expr, expected in INFIX:
        n += 1
        rc, line = eval_infix(binary, expr)
        if expected is REJECT_INFIX:
            if rc == 0:
                failures.append(f"INFIX  {expr:26} expected error, got ok -> {line!r}")
        elif rc != 0 or line != expected:
            got = f"exit={rc}" if rc != 0 else repr(line)
            failures.append(f"INFIX  {expr:26} expected {expected!r}, got {got}")

    for prog, expected in MISC_OK:
        n += 1
        rc, line = run_line(binary, prog)
        if rc != 0 or line != expected:
            got = f"exit={rc}" if rc != 0 else repr(line)
            failures.append(f"MISC   {prog:24} expected {expected!r}, got {got}")

    for prog, code in MISC_ERR:
        n += 1
        rc, _ = run_line(binary, prog)
        if rc != code:
            failures.append(f"MISC   {prog:24} expected exit {code}, got exit {rc}")

    failures.extend(check_docs(root))

    if failures:
        print(f"check_scanner_grammar: {len(failures)} MISMATCH (of {n} probes + doc checks)\n")
        for line in failures:
            print("  " + line)
        return 1
    print(f"check_scanner_grammar: {n} grammar probes + doc strings all conform")
    return 0


if __name__ == "__main__":
    sys.exit(main())
