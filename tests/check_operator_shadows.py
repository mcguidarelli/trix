#!/usr/bin/env python3
# check_operator_shadows.py -- flag .trx names that shadow a built-in operator.
#
# A name bound by `def` / `def-persist` (global), or declared as a frame local
# (`|locals|` preamble, `local-def`, `bind-locals`), that collides with a system
# operator name is a SHADOW.  It works (late binding resolves to the user's
# binding) but is a readability foot-gun AND it breaks early binding (`#e` /
# `bind`): early binding freezes a name to the operator at SCAN time, so a frame
# local that shadows an operator is frozen to the OPERATOR (the local does not
# exist yet), and a body scanned after a global redefinition diverges from one
# scanned before -- a silent split.
#
# This is a LINT, not a hard language rule: redefinition is a deliberate Trix
# feature and operator names overlap with common variable names, so it is run on
# demand / on the curated showcases (default: examples/zmachine.trx), NOT on every
# scan.  Pass file paths to scan others.
#
# Classification is BINDER-AWARE -- the binding operator is what matters, not
# indentation:
#   * `def` / `def-persist`        -> GLOBAL operator redefinition (the real
#                                     foot-gun) -> exits 1.
#   * `local-def` / `bind-locals`  -> frame local; and `|locals|` preamble tokens
#     / `|...|` preamble             -> frame local.  Both are REPORTED but do not
#                                     fail -- operator names overlap heavily with
#                                     good variable names (sum, count, max), so
#                                     failing on them would cry wolf.
#   * `def-prim` / `put` / `store` / anything else -> NOT a shadow.  These install
#     into a USER or domain dict (e.g. mini-scheme's Scheme primitives-dict via
#     `def-prim`), never the Trix dict stack, so they cannot shadow an operator.
#     The previous line-only heuristic mis-flagged these as global redefinitions.
#
# `@`-operators are excluded: leading-`@` names are reserved and cannot be user
# bound, so they are unshadowable.  Run before using `#e` to find frame-local
# hazards; CI runs it over examples/*.trx (`--quiet`) as a guard against NEW
# global operator redefinitions in the showcases (all are currently 0-global).
# tests/*.trx are NOT guarded -- they deliberately exercise redefinition.

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DISPATCH = ROOT / "src" / "dispatch.inl"

DEFAULT_TARGETS = [ROOT / "examples" / "zmachine.trx"]

# A /name or local token: starts with a letter, then Trix name chars.
NAME_CHARS = r"[A-Za-z][A-Za-z0-9_?<>=!*+./-]*"
NAME_CONT = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_?<>=!*+./-")
NAME_START = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
LINE_LEAD_DEF = re.compile(r"^[ \t]*/(" + NAME_CHARS + r")")
LOCALS_PREAMBLE = re.compile(r"\{\s*\|([^|]*)\|")

# Binder words that bind a GLOBAL name (into the current/system dict).
GLOBAL_BINDERS = {"def", "def-persist"}
# Binder words that declare a frame local.
LOCAL_BINDERS = {"local-def", "bind-locals"}
# All recognized binders (used to terminate the forward scan).
ALL_BINDERS = GLOBAL_BINDERS | LOCAL_BINDERS | {"def-prim", "def-str-prim", "store", "put"}


def operator_names():
    """Non-@ operator names from the dispatch table's `"name"sv` column."""
    text = DISPATCH.read_text()
    rows = re.findall(r'SystemName::\w+,\s*\w+,\s*"([^"]*)"sv', text)
    return {n for n in rows if n and not n.startswith("@")}


def mask(text):
    """Blank `(...)` string interiors and %{ %} / % comments (preserving length
    and newlines) so the binder scan only sees code -- Trix strings can contain
    literal braces (e.g. format specs `({0:X})`) that would corrupt depth."""
    out = []
    i, n = 0, len(text)
    block_depth = 0
    paren_depth = 0
    while i < n:
        c = text[i]
        if block_depth > 0:
            if text.startswith("%}", i):
                block_depth -= 1
                out.append("  ")
                i += 2
            elif text.startswith("%{", i):
                block_depth += 1
                out.append("  ")
                i += 2
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
            continue
        if paren_depth > 0:
            if c == "\\" and i + 1 < n:
                out.append("  ")
                i += 2
            elif c == "(":
                paren_depth += 1
                out.append(" ")
                i += 1
            elif c == ")":
                paren_depth -= 1
                out.append(" ")
                i += 1
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
            continue
        if text.startswith("%{", i):
            block_depth += 1
            out.append("  ")
            i += 2
        elif c == "%":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "(":
            paren_depth += 1
            out.append(" ")
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def find_binder(masked, name_end):
    """The binder word that consumes a definition starting at name_end, or None.
    Scans forward -- across balanced { } [ ] << >> containers and intervening
    operators (make-readonly, load, ...) -- to the first recognized binder at
    container depth <= 0, bounded by the next column-0 `/` (the next top-level
    definition), so a value-use name-literal mid-expression returns None."""
    m = re.search(r"\n/", masked[name_end:])
    seg = masked[name_end:(name_end + m.start()) if m else len(masked)]
    depth = 0
    i, n = 0, len(seg)
    while i < n:
        if seg.startswith("<<", i):
            depth += 1
            i += 2
            continue
        if seg.startswith(">>", i):
            depth -= 1
            if depth < 0:  # exited the container holding /name: it was a dict key, not a def
                return None
            i += 2
            continue
        c = seg[i]
        if c in "{[":
            depth += 1
            i += 1
            continue
        if c in "}]":
            depth -= 1
            if depth < 0:  # exited /name's enclosing proc/array: a key or value-use, not a def
                return None
            i += 1
            continue
        if c == "/":  # name literal -- a value, never a binder; skip it
            i += 1
            while i < n and seg[i] in NAME_CONT:
                i += 1
            continue
        if c in NAME_START:
            j = i
            while j < n and seg[j] in NAME_CONT:
                j += 1
            word = seg[i:j]
            if depth <= 0 and word in ALL_BINDERS:
                return word
            i = j
            continue
        i += 1
    return None


def scan_file(path, ops):
    shadows = []  # (lineno, kind, name); kind in global-def / local-def / local-var
    text = path.read_text()
    masked = mask(text)
    lines = masked.split("\n")
    offset = 0  # char offset of the start of the current line in `masked`
    for lineno, line in enumerate(lines, 1):
        m = LINE_LEAD_DEF.match(line)
        if m and m.group(1) in ops:
            name_end = offset + m.end()
            binder = find_binder(masked, name_end)
            if binder in GLOBAL_BINDERS:
                shadows.append((lineno, "global-def", m.group(1)))
            elif binder in LOCAL_BINDERS:
                shadows.append((lineno, "local-def", m.group(1)))
            # def-prim / put / store / None: a dict install or a value-use, not a shadow
        for pm in LOCALS_PREAMBLE.finditer(line):
            for tok in pm.group(1).split():
                if tok in ops:
                    shadows.append((lineno, "local-var", tok))
        offset += len(line) + 1  # +1 for the '\n' removed by split
    return shadows


def main():
    args = [a for a in sys.argv[1:] if a != "--quiet"]
    quiet = "--quiet" in sys.argv[1:]  # suppress the benign local-shadow listing (CI)
    targets = [Path(a) for a in args] if args else DEFAULT_TARGETS
    ops = operator_names()
    global_total = 0
    local_total = 0
    for path in targets:
        if not path.exists():
            print(f"  SKIP (not found): {path}")
            continue
        try:
            rel = path.relative_to(ROOT)
        except ValueError:
            rel = path
        shadows = scan_file(path, ops)
        globals_ = [s for s in shadows if s[1] == "global-def"]
        locals_ = [s for s in shadows if s[1] != "global-def"]
        global_total += len(globals_)
        local_total += len(locals_)
        if not shadows:
            if not quiet:
                print(f"OK: no operator shadows in {rel}")
            continue
        if globals_:
            print(f"\nGLOBAL operator shadows in {rel} (operator redefinition -- fix these):")
            for lineno, _, name in globals_:
                print(f"  {rel}:{lineno}: global def '{name}' shadows built-in operator '{name}'")
        if locals_ and not quiet:
            print(f"\nLocal operator shadows in {rel} (scoped; usually benign natural names):")
            for lineno, kind, name in locals_:
                print(f"  {rel}:{lineno}: {kind} '{name}' shadows built-in operator '{name}'")
    print(f"\nChecked {len(ops)} operators across {len(targets)} file(s); "
          f"{global_total} global, {local_total} local shadow(s).")
    return 1 if global_total else 0


if __name__ == "__main__":
    sys.exit(main())
