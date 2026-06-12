#!/usr/bin/env python3
"""Align ragged trailing `%` comments inside Trix code fences.

Trix examples line their inline `% ...` comments up in a column for
readability, but hand-editing leaves the column ragged.  This tool finds
each run of consecutive comment-bearing lines inside a code fence and pads
the space before `%` so every comment in the run starts at the same column
(the longest line in the run, plus one space).

Scope and safety:
  * Only `%`-comment languages are touched: bare ``` fences (the Trix
    default), ```trix, and ```postscript.  Fences tagged cpp / python /
    text / console / bash / mermaid / ebnf / ... are skipped, because `%`
    there is modulo, a format specifier, a prompt, or a percentage -- not
    a comment.
  * The comment marker is found string- (paren), escape- and brace-aware:
    a `%` inside (...) or inside {...} (a proc body or a PrintFmt directive
    like `{0:I%Y...}`) is not mistaken for the trailing comment.  A `%`
    must be preceded by whitespace to count, so `50%` and `a%b` are safe.
  * `%{` / `%}` block-comment lines are left alone.
  * Only RAGGED runs change; a run already aligned is untouched (so blocks
    that intentionally use a wide column keep it).  Only spaces before the
    comment marker ever change -- never code, never comment text.
  * Idempotent: running twice changes nothing.

Usage:
    ./tools/align_code_comments.py [--check] FILE...
    ./tools/align_code_comments.py --check docs/*.md   # CI mode

--check exits 1 (listing files) if any file would change, without writing.
"""
import sys
from pathlib import Path

# Fence languages whose `%` is a comment.  Empty string == a bare ``` fence.
ALLOW = {"", "trix", "postscript"}


def comment_col(line: str):
    """Index of the trailing-comment `%`, or None.  See module docstring."""
    sdepth = 0  # paren-string depth
    bdepth = 0  # brace depth
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if c == "\\":
            i += 2
            continue
        if sdepth > 0:
            if c == "(":
                sdepth += 1
            elif c == ")":
                sdepth -= 1
            i += 1
            continue
        if c == "(":
            sdepth += 1
        elif c == "{":
            bdepth += 1
        elif c == "}":
            bdepth = max(0, bdepth - 1)
        elif c == "%" and bdepth == 0:
            nxt = line[i + 1] if i + 1 < n else ""
            if nxt in "{}":
                return None
            if i > 0 and line[i - 1] in " \t":
                return i
        i += 1
    return None


def trailing(line: str):
    """Comment column if `line` is `code % comment`, else None."""
    col = comment_col(line)
    if col is None or line[:col].rstrip() == "":
        return None
    return col


def align_run(lines: list[str], run: list[int]) -> bool:
    """Align a run of line indices to a common column.  Returns True if any
    line changed."""
    cols = [comment_col(lines[i]) for i in run]
    if len(set(cols)) <= 1:
        return False
    target = max(len(lines[i][: cols[k]].rstrip()) for k, i in enumerate(run)) + 1
    changed = False
    for k, i in enumerate(run):
        code = lines[i][: cols[k]].rstrip()
        new = code + " " * (target - len(code)) + lines[i][cols[k] :]
        if new != lines[i]:
            lines[i] = new
            changed = True
    return changed


def format_text(text: str) -> str:
    lines = text.split("\n")
    in_fence = False
    fence_ok = False
    run: list[int] = []

    def flush():
        if len(run) >= 2:
            align_run(lines, run)
        run.clear()

    for idx, line in enumerate(lines):
        if line.lstrip().startswith("```"):
            flush()
            if in_fence:
                in_fence = False
            else:
                in_fence = True
                lang = line.lstrip()[3:].strip().split()[:1]
                fence_ok = (lang[0] if lang else "") in ALLOW
            continue
        if in_fence and fence_ok and trailing(line) is not None:
            run.append(idx)
        else:
            flush()
    flush()
    return "\n".join(lines)


def main() -> int:
    args = sys.argv[1:]
    check = "--check" in args
    files = [Path(a) for a in args if a != "--check"]
    if not files:
        print(__doc__)
        return 2
    changed = []
    for f in files:
        original = f.read_text()
        formatted = format_text(original)
        if formatted != original:
            changed.append(str(f))
            if not check:
                f.write_text(formatted)
    if changed:
        verb = "would change" if check else "aligned"
        print(f"{verb} {len(changed)} file(s):")
        for c in changed:
            print(f"  {c}")
        return 1 if check else 0
    print("all comment columns already aligned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
