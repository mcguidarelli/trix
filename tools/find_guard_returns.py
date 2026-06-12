#!/usr/bin/env python3
"""Find early-return GUARD clauses with fall-through -- the convertible pattern:

    if (cond) {        if (cond) {            // -> invert + nest (no work):
        return;            ...; return;       //    if (!cond) { <fallthrough> }
    }                  }                       // -> if/else (work before return):
    <fallthrough>      <fallthrough>           //    if (cond) { ... } else { <fallthrough> }

i.e. an `if` whose body ENDS in a bare `return;` and that is followed by sibling
statements in the same scope (the code that would be folded in).  These collapse
the function toward a single top-to-bottom flow.

SKIPPED (by design, per the preferred style):
  * a `return;` inside an enclosing for/while/do loop -> kept (a loop early-exit
    is a perf win).  NOTE: a gvm_for_each (or similar) CALLBACK LAMBDA is not a
    detected loop, so its per-chunk `if (skip) return;` continues ARE reported --
    and that is correct: they invert to single-flow like any other guard.
  * an `if (...) { return; } else { ... }` (has an else already) -> not a guard.
  * an `if` that is the last statement of its scope (nothing to fold in).
  * valued returns `return x;` (only bare `return;` void guards are listed).

Heuristic / line-based (like find_tail_return.py); READ each hit -- compound
conditions need De Morgan when inverted, and comments may need relocating.
A clean tree prints zero sites.

Usage: find_guard_returns.py [files...]    # default src/*.inl
"""
import glob
import re
import sys

IF = re.compile(r"^(\s*)if\s*\(")
RET = re.compile(r"^\s*return;\s*(?://.*)?$")
ELSE = re.compile(r"^\s*\}?\s*else\b")
COMMENT = re.compile(r"^\s*(//.*)?$")
CTRL = re.compile(r"^\}?\s*(if|else|for|while|switch|catch|do)\b")
FNDEF = re.compile(r"(\w+)\s*\(")


def strip(line):
    line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    line = re.sub(r"'(\\.|[^'\\])*'", "''", line)
    return re.sub(r"//.*$", "", line)


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


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
    if re.search(r"\)\s*\{$", s) and not CTRL.match(s):
        return "fn"
    return "block"


def body_close(L, n, start, ind):
    """First line at indent `ind` that closes the block opened above (a `}` or
    `} else`), scanning from `start`."""
    c = start
    while c < n:
        st = L[c]
        if indent_of(st) == len(ind) and st.lstrip().startswith("}"):
            return c
        c += 1
    return -1


def capture_condition(L, i):
    text = ""
    depth = 0
    started = False
    k = i
    while k < len(L):
        for ch in strip(L[k]):
            if ch == "(":
                depth += 1
                started = True
                if depth == 1:
                    continue
            if ch == ")":
                depth -= 1
                if depth == 0:
                    return re.sub(r"\s+", " ", text).strip()
            if started and depth >= 1:
                text += ch
        text += " "
        k += 1
    return re.sub(r"\s+", " ", text).strip()


def main(argv):
    files = sorted(argv[1:]) if len(argv) > 1 else sorted(glob.glob("src/*.inl"))
    by_file = {}
    total = 0
    for f in files:
        L = open(f).read().split("\n")
        n = len(L)
        stack = []
        cur_fn = "?"
        hits = []
        for i in range(n):
            line = L[i]
            # function-name tracking: an identifier-bearing line at column 0
            if line and line[0] not in " \t}/#*":
                m = FNDEF.search(strip(line))
                if m and not CTRL.match(strip(line).strip()):
                    cur_fn = m.group(1)
            mi = IF.match(line)
            if mi and "loop" not in stack:
                ind = mi.group(1)
                close = body_close(L, n, i + 1, ind)
                if close >= 0:
                    closing = L[close].lstrip()
                    if closing.startswith("}") and not closing.startswith("} else") \
                            and "else" not in closing:
                        # last real statement of the body is `return;`?
                        k = close - 1
                        while k > i and COMMENT.match(L[k]):
                            k -= 1
                        if k > i and RET.match(L[k]):
                            # fall-through: next non-blank sibling is code, not a
                            # scope-close and not else
                            t = close + 1
                            while t < n and L[t].strip() == "":
                                t += 1
                            falls = (t < n and indent_of(L[t]) == len(ind)
                                     and not L[t].lstrip().startswith("}")
                                     and not ELSE.match(L[t]))
                            if falls:
                                # has work before the return?
                                b = i + 1
                                while b < k and COMMENT.match(L[b]):
                                    b += 1
                                has_work = b < k
                                cond = capture_condition(L, i)
                                compound = bool(re.search(r"&&|\|\||\bor\b|\band\b", cond))
                                hits.append((i + 1, cur_fn, cond, has_work, compound))
                                total += 1
            s = strip(line)
            for ch in s:
                if ch == "{":
                    stack.append(open_kind(line))
                elif ch == "}" and stack:
                    stack.pop()
        if hits:
            by_file[f] = hits
    # group cascades: consecutive hits in the same function
    for f in sorted(by_file):
        hits = by_file[f]
        # count per function to flag cascades
        fn_counts = {}
        for _, fn, *_ in hits:
            fn_counts[fn] = fn_counts.get(fn, 0) + 1
        print(f"\n=== {f}  ({len(hits)}) ===")
        for ln, fn, cond, has_work, compound in hits:
            shape = "if/else" if has_work else "invert "
            tags = []
            if fn_counts[fn] > 1:
                tags.append(f"cascade x{fn_counts[fn]}")
            if compound:
                tags.append("DeMorgan")
            tag = ("  [" + ", ".join(tags) + "]") if tags else ""
            c = cond if len(cond) <= 56 else cond[:53] + "..."
            print(f"  {ln:5}  {fn:28} {shape}  if ({c}){tag}")
    print(f"\n=== {total} guard-with-fall-through candidates across "
          f"{len(by_file)} files ===")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
