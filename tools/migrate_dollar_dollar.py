#!/usr/bin/env python3
"""Migrate `$$(...)` (old promoted-infix spelling) to `$[...]` (new spelling).

Walks each file character-by-character, skipping line comments (`% ...`),
block comments (`%{ ... %}`, nestable), Trix strings (`(...)` with `\(`
and `\)` escapes), and raw strings (`<(...)>`, nestable parens, no
escapes).  Inside an `$$(` body the parser tracks infix paren depth
only (`(`/`)` are grouping, not strings -- infix expressions cannot
contain Trix string literals).

Usage:
    tools/migrate_dollar_dollar.py path1 [path2 ...]

In-place edit; prints a count per file.  Files with no `$$(` are
untouched.
"""
import sys
from pathlib import Path


def migrate(text: str) -> tuple[str, int]:
    out: list[str] = []
    i = 0
    n = len(text)
    count = 0
    while i < n:
        c = text[i]
        # Block comment: %{ ... %} -- nestable
        if c == '%' and i + 1 < n and text[i + 1] == '{':
            depth = 1
            out.append('%{')
            i += 2
            while i < n and depth > 0:
                if text[i] == '%' and i + 1 < n and text[i + 1] == '{':
                    depth += 1
                    out.append('%{')
                    i += 2
                elif text[i] == '%' and i + 1 < n and text[i + 1] == '}':
                    depth -= 1
                    out.append('%}')
                    i += 2
                else:
                    out.append(text[i])
                    i += 1
        # Line comment: %...\n
        elif c == '%':
            while i < n and text[i] != '\n':
                out.append(text[i])
                i += 1
        # $$( --- migrate to $[
        elif c == '$' and i + 2 < n and text[i + 1] == '$' and text[i + 2] == '(':
            out.append('$[')
            i += 3
            depth = 1
            while i < n and depth > 0:
                ch = text[i]
                if ch == '(':
                    depth += 1
                    out.append('(')
                elif ch == ')':
                    depth -= 1
                    if depth == 0:
                        out.append(']')
                    else:
                        out.append(')')
                else:
                    out.append(ch)
                i += 1
            count += 1
        # Trix string literal (...) -- escape-aware, nestable
        elif c == '(':
            out.append('(')
            i += 1
            sdepth = 1
            while i < n and sdepth > 0:
                ch = text[i]
                if ch == '\\' and i + 1 < n:
                    out.append(ch)
                    out.append(text[i + 1])
                    i += 2
                elif ch == '(':
                    sdepth += 1
                    out.append('(')
                    i += 1
                elif ch == ')':
                    sdepth -= 1
                    out.append(')')
                    i += 1
                else:
                    out.append(ch)
                    i += 1
        # Raw string <(...)> -- nestable parens, no escapes
        elif c == '<' and i + 1 < n and text[i + 1] == '(':
            out.append('<(')
            i += 2
            sdepth = 1
            while i < n and sdepth > 0:
                ch = text[i]
                if ch == '(':
                    sdepth += 1
                    out.append('(')
                elif ch == ')':
                    sdepth -= 1
                    out.append(')')
                else:
                    out.append(ch)
                i += 1
            if i < n and text[i] == '>':
                out.append('>')
                i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out), count


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    total = 0
    for arg in argv[1:]:
        path = Path(arg)
        text = path.read_text()
        new_text, count = migrate(text)
        if count > 0:
            path.write_text(new_text)
            print(f"{path}: {count} $$(...) -> $[...]")
            total += count
        else:
            print(f"{path}: no $$( hits")
    print(f"total: {total}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
