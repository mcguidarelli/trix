#!/usr/bin/env python3
"""Audit redundant work-then-return tails: `if (c) { ...; return; } else { ... }`.

Complements find_redundant_else.py.  Finds an `if` whose then-branch does work
and ends in a bare `return;` immediately before a two-way `} else {`, where the
whole `if/else` is the LAST statement of a void function (so the `return;` is
redundant -- the function returns by falling off the end anyway).  A symmetric
binary collapses by dropping the return and keeping the else; a nested cascade
flattens to flat guard clauses instead (read each site -- this is a locator, not
an auto-fixer).

It tracks brace scope to require genuine fn-tail (no enclosing loop/switch makes
the return load-bearing) and a clean closing chain to the function brace.

KNOWN LIMITATIONS (sites it can miss or misreport -- always read the hit):
  * `same_indent_close` walks PAST `} else if`, so a bare return in a MIDDLE
    else-if arm reads as a false binary-tail positive (cooked-mode was one).
  * `open_kind` infers the function brace from a line with `(` and `)`.  A
    multi-line signature puts `{` on a `) {` continuation line; the PATCH below
    treats `) {` (non-control) as the fn brace so wrapped signatures still
    anchor the fn-tail check.  Without it, such functions are silently skipped.
  * Single-conditional-action folds and `else { single if } -> else if`
    collapses are separate simplifications this does not detect.

Usage:
  ./tools/find_tail_return.py                 # scan src/*.inl (default)
  ./tools/find_tail_return.py src/gc.inl ...  # scan specific files
"""
import glob
import re
import sys

IF = re.compile(r"^(\s*)if\s*\(")
RET = re.compile(r"^\s*return;\s*(?://.*)?$")
ELSE = re.compile(r"^(\s*)\}\s*else\s*\{\s*(?://.*)?$")
BARE = re.compile(r"^\s*\}\s*(?://.*)?$")
COMMENT = re.compile(r"^\s*(//.*)?$")
CTRL = re.compile(r"^\}?\s*(if|else|for|while|switch|catch|do)\b")


def strip(line):
    line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    line = re.sub(r"'(\\.|[^'\\])*'", "''", line)
    return re.sub(r"//.*$", "", line)


def open_kind(line):
    s = strip(line).strip()
    if re.match(r"^\}?\s*else\s+if\b", s) or re.match(r"^if\b", s):
        return "if"
    if re.match(r"^\}?\s*else\b", s):
        return "else"
    if re.match(r"^(for|while)\b", s) or s.startswith("do ") or s == "do {":
        return "loop"
    if re.match(r"^switch\b", s):
        return "switch"
    if re.match(r"^\}?\s*catch\b", s) or s.startswith("try"):
        return "block"
    if re.match(r"^(namespace|class|struct|enum|union)\b", s):
        return "block"
    if s.startswith("["):
        return "fn"
    if "(" in s and ")" in s:
        return "fn"
    # A `) {` continuation that is NOT a control construct is a multi-line
    # signature's (or wrapped-condition's) brace -> treat as fn so the fn-tail
    # check can anchor (else such functions are silently skipped).
    if re.search(r"\)\s*\{$", s) and not CTRL.match(s):
        return "fn"
    return "block"


def same_indent_close(L, n, start, ind):
    c = start
    while c < n:
        if L[c].startswith(ind + "}") and not L[c][len(ind):].startswith("} "):
            return c
        e = ELSE.match(L[c])
        if e and e.group(1) == ind:
            return c
        c += 1
    return -1


def main(argv):
    files = sorted(argv[1:]) if len(argv) > 1 else sorted(glob.glob("src/*.inl"))
    total = 0
    for f in files:
        L = open(f).read().split("\n")
        n = len(L)
        stack = []
        for i in range(n):
            m = IF.match(L[i])
            if m and stack and stack[-1] not in ("loop", "switch"):
                ind = m.group(1)
                c = same_indent_close(L, n, i + 1, ind)
                em = ELSE.match(L[c]) if c >= 0 else None
                if em and em.group(1) == ind:
                    k = c - 1
                    while k > i and COMMENT.match(L[k]):
                        k -= 1
                    if k > i and RET.match(L[k]):
                        kk = k - 1
                        while kk > i and COMMENT.match(L[kk]):
                            kk -= 1
                        e = same_indent_close(L, n, c + 1, ind)
                        fn_idx = max((ix for ix, kx in enumerate(stack) if kx == "fn"), default=-1)
                        need = (len(stack) - fn_idx) if fn_idx >= 0 else 999
                        above = stack[fn_idx + 1:] if fn_idx >= 0 else stack
                        no_loop = not any(kx in ("loop", "switch") for kx in above)
                        ok = e >= 0
                        t = e + 1
                        cnt = 0
                        while ok and cnt < need:
                            while t < n and L[t].strip() == "":
                                t += 1
                            if t >= n or not BARE.match(L[t]) or "else" in L[t]:
                                ok = False
                                break
                            cnt += 1
                            t += 1
                        if kk > i and ok and no_loop:
                            total += 1
                            print(f"{f}:{k+1}  :: {L[i].strip()[:50]}")
            s = strip(L[i])
            for ch in s:
                if ch == "{":
                    stack.append(open_kind(L[i]))
                elif ch == "}" and stack:
                    stack.pop()
    print(f"\nTotal fn-tail work-then-return: {total}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
