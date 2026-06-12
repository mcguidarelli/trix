#!/usr/bin/env python3
"""assert_blank_line.py -- enforce the STYLE.md "Formatting" rule:

    A function that opens with a precondition assert(...) -- or a run of
    consecutive precondition asserts -- gets ONE blank line after the last
    of them, separating the precondition guard from the function body.

Scope (deliberately conservative -- review the report before --apply):

  * Only asserts in a function/lambda body PROLOGUE qualify: the assert is
    reached from the body's opening brace through nothing but local
    declarations (`auto x = ...;`) and other asserts.  An assert preceded by
    any real statement, or sitting inside an if/else/for/while/switch/case,
    is NOT a prologue precondition and is left alone (that is the
    "interspersed mid-body" exclusion).
  * Idempotent: a run already followed by a blank line is COMPLIANT and never
    touched.  A run that ends the function (next line is `}`) needs no blank.
  * `static_assert` and terminal `assert(false ...)` are ignored.

Comment- and string-aware (block comments tracked across lines; // and string
/char contents stripped before structural matching).

Usage:
  assert_blank_line.py --check  [files...]   # CI gate: exit 1 if any remain
  assert_blank_line.py --report [files...]   # list sites (default)
  assert_blank_line.py --diff   [files...]   # unified diff of the fix
  assert_blank_line.py --apply  [files...]   # insert the blank lines in place

Default file set: src/*.inl
"""
import sys
import os
import re
import glob
import argparse

ASSERT_START = re.compile(r'^\s*assert\s*\(')
STATIC_ASSERT = re.compile(r'^\s*static_assert\s*\(')
ASSERT_FALSE = re.compile(r'^\s*assert\s*\(\s*false\b')
# A local declaration: optional qualifiers, then `auto`/Type (maybe templated
# or pointer), then a name, then `=` / `{` / `[`.  Excludes assignments
# (`m_x = ...`) because those have a name where the type would be, with no
# second identifier before the `=`.
DECL = re.compile(
    r'^\s*(?:const\s+|constexpr\s+|static\s+|mutable\s+|volatile\s+)*'
    r'(?:auto|[A-Za-z_][\w:]*\s*(?:<[^;{]*>)?)\s*[*&]?\s*'
    r'[A-Za-z_]\w*\s*[\[={]')
CONTROL_KW = re.compile(r'^\s*(?:\}\s*)?(?:if|for|while|switch|else|do|try|catch)\b')
TYPE_KW = re.compile(r'\b(?:class|struct|union|enum|namespace)\b')


def strip_code(lines):
    """Return per-line code with // comments dropped, /* */ removed (tracked
    across lines), and string/char literal contents blanked.  Length and
    indentation are preserved as far as the leading whitespace; trailing parts
    differ.  Returned strings are for structural matching only."""
    out = []
    in_block = False
    for raw in lines:
        buf = []
        i, n = 0, len(raw)
        while i < n:
            c = raw[i]
            if in_block:
                if c == '*' and i + 1 < n and raw[i + 1] == '/':
                    in_block = False
                    i += 2
                    continue
                i += 1
                continue
            if c == '/' and i + 1 < n and raw[i + 1] == '/':
                break
            if c == '/' and i + 1 < n and raw[i + 1] == '*':
                in_block = True
                i += 2
                continue
            if c == '"' or c == "'":
                quote = c
                buf.append(c)
                i += 1
                while i < n:
                    if raw[i] == '\\' and i + 1 < n:
                        i += 2
                        continue
                    if raw[i] == quote:
                        break
                    i += 1
                buf.append(quote)
                i += 1
                continue
            buf.append(c)
            i += 1
        out.append(''.join(buf))
    return out


def indent_of(s):
    return len(s) - len(s.lstrip(' '))


def is_opener(code):
    """code is a stripped line ending in '{'.  Classify what it opens."""
    body = code.rstrip()
    if not body.endswith('{'):
        return None
    head = body[:-1].rstrip()
    if CONTROL_KW.match(code):
        return 'control'
    if TYPE_KW.search(code):
        return 'type'
    # function / lambda: signature ends in ')' possibly + trailing qualifiers
    # or a trailing-return type.
    if re.search(r'\)\s*(?:const|noexcept|override|final|mutable'
                 r'|->[\w:<>,\s*&]+)*\s*$', head):
        return 'func'
    return 'other'


def assert_run_end(code, start):
    """Given stripped lines and the index of an assert-start, return the
    physical index of the last line of that assert statement (handles
    multi-line asserts that close on a later `);`)."""
    depth = 0
    seen_paren = False
    i = start
    while i < len(code):
        for ch in code[i]:
            if ch == '(':
                depth += 1
                seen_paren = True
            elif ch == ')':
                depth -= 1
        if seen_paren and depth <= 0:
            return i
        i += 1
    return start


def is_prologue_assert(code, i):
    """True iff the assert starting at line i is in its function body's
    prologue (reached from the opening brace through only declarations and
    other asserts)."""
    ind = indent_of(code[i])
    j = i - 1
    while j >= 0:
        s = code[j]
        if s.strip() == '':
            j -= 1
            continue
        jind = indent_of(s)
        if jind > ind:
            # A more-indented line above means a nested block already ran
            # before this assert -> not a clean prologue.
            return False
        if jind == ind:
            if ASSERT_START.match(s) and not STATIC_ASSERT.match(s):
                j -= 1
                continue
            if DECL.match(s) and not CONTROL_KW.match(s):
                j -= 1
                continue
            return False  # a real statement precedes the assert
        # jind < ind: this should be the brace that opened our block.
        stripped = s.rstrip()
        if stripped.endswith('{'):
            kind = is_opener(s)
            if kind == 'func':
                return True
            return False  # control/type/other opener -> not a func prologue
        if stripped.endswith('{') is False and stripped == '{':
            return False
        # An opener whose '{' is on its own line: the signature is above.
        if stripped == '{':
            return False
        return False
    return False


def find_sites(lines):
    """Return list of (insert_after_index, assert_line_index) violations."""
    code = strip_code(lines)
    sites = []
    n = len(lines)
    i = 0
    while i < n:
        s = code[i]
        if (ASSERT_START.match(s) and not STATIC_ASSERT.match(s)
                and not ASSERT_FALSE.match(s)):
            if is_prologue_assert(code, i):
                end = assert_run_end(code, i)
                nxt = end + 1
                # part of a longer run? let the run's last assert handle it.
                next_is_assert = (nxt < n and ASSERT_START.match(code[nxt])
                                  and not STATIC_ASSERT.match(code[nxt]))
                if not next_is_assert:
                    if nxt < n:
                        nxt_s = code[nxt]
                        already_blank = (nxt_s.strip() == '')
                        closes = (nxt_s.lstrip().startswith('}'))
                        macro = lines[end].rstrip().endswith('\\')
                        if not already_blank and not closes and not macro:
                            sites.append((end, i))
                i = end + 1
                continue
        i += 1
    return sites


def process(path, mode):
    with open(path) as f:
        lines = f.readlines()
    sites = find_sites(lines)
    if not sites:
        return 0, []
    if mode == 'apply':
        # insert blank lines from the bottom up so indices stay valid
        for end, _ in sorted(sites, reverse=True):
            lines.insert(end + 1, '\n')
        with open(path, 'w') as f:
            f.writelines(lines)
    return len(sites), sites


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--check', action='store_true', help='exit 1 if any remain')
    g.add_argument('--report', action='store_true', help='list sites (default)')
    g.add_argument('--diff', action='store_true', help='show unified diff')
    g.add_argument('--apply', action='store_true', help='insert blank lines')
    ap.add_argument('files', nargs='*')
    args = ap.parse_args()

    files = args.files
    if not files:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        files = sorted(glob.glob(os.path.join(root, 'src', '*.inl')))
        # Match the clang-format CI scope: src/*.inl + trix.h + trix.cpp.
        for extra in ('trix.h', 'trix.cpp'):
            p = os.path.join(root, extra)
            if os.path.exists(p):
                files.append(p)

    mode = 'apply' if args.apply else ('diff' if args.diff
                                       else ('check' if args.check else 'report'))
    total = 0
    for path in files:
        if args.diff:
            with open(path) as f:
                before = f.readlines()
            sites = find_sites(before)
            if not sites:
                continue
            after = list(before)
            for end, _ in sorted(sites, reverse=True):
                after.insert(end + 1, '\n')
            import difflib
            sys.stdout.writelines(difflib.unified_diff(
                before, after, fromfile=path, tofile=path))
            total += len(sites)
            continue
        count, sites = process(path, mode)
        total += count
        if count and mode in ('report', 'check'):
            rel = os.path.relpath(path)
            for end, ai in sites:
                print(f'{rel}:{ai + 1}: precondition assert needs a blank '
                      f'line after (insert after line {end + 1})')
            print(f'  -- {rel}: {count} site(s)')

    if mode == 'apply':
        print(f'assert_blank_line: inserted {total} blank line(s)')
    elif mode == 'check':
        print(f'assert_blank_line: {total} violation(s)')
        return 1 if total else 0
    else:
        print(f'assert_blank_line: {total} site(s) across {len(files)} file(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
