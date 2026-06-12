#!/usr/bin/env python3
"""Align markdown pipe-table columns in the shipped docs.

GitHub renders pipe tables identically whether or not the source columns
line up -- but humans reading the raw files do not.  This tool pads every
cell so the pipes align column-perfect within each table, preserving the
separator row's alignment markers (`:---`, `:---:`, `---:`).

Rules:
  * Tables inside ``` code fences are never touched.
  * A table whose aligned width would exceed WIDE_LIMIT columns is instead
    normalized to the compact one-space style (`| cell | cell |`) --
    consistent without creating enormous lines.
  * Escaped pipes (`\\|`) inside cells are respected.
  * Idempotent: running twice changes nothing.

Usage:
    ./tools/format_md_tables.py [--check] FILE...
    ./tools/format_md_tables.py --check docs/*.md README.md   # CI mode

--check exits 1 (listing files) if any file would change, without writing.
"""
import re
import sys
from pathlib import Path

WIDE_LIMIT = 110

SEP_RE = re.compile(r"^\s*\|[\s:|-]+\|?\s*$")
SPLIT_RE = re.compile(r"(?<!\\)\|")


def split_cells(row: str) -> list[str]:
    body = row.strip()
    if body.startswith("|"):
        body = body[1:]
    if body.endswith("|") and not body.endswith("\\|"):
        body = body[:-1]
    return [c.strip() for c in SPLIT_RE.split(body)]


def is_sep_cell(cell: str) -> bool:
    return bool(re.fullmatch(r":?-{2,}:?", cell.replace(" ", "")))


def alignment(cell: str) -> str:
    c = cell.replace(" ", "")
    left, right = c.startswith(":"), c.endswith(":")
    if left and right:
        return "center"
    if right:
        return "right"
    return "left"


def render(indent: str, rows: list[list[str]], aligns: list[str], sep_idx: int) -> list[str]:
    ncols = max(len(r) for r in rows)
    rows = [r + [""] * (ncols - len(r)) for r in rows]
    aligns = (aligns + ["left"] * ncols)[:ncols]
    widths = [
        max(3, *(len(r[i]) for k, r in enumerate(rows) if k != sep_idx))
        for i in range(ncols)
    ]

    total = len(indent) + 1 + sum(w + 3 for w in widths)
    compact = total > WIDE_LIMIT

    out = []
    for k, r in enumerate(rows):
        if k == sep_idx:
            cells = []
            for i in range(ncols):
                w = 3 if compact else widths[i]
                a = aligns[i]
                if a == "center":
                    cells.append(":" + "-" * max(1, w - 2) + ":")
                elif a == "right":
                    cells.append("-" * max(2, w - 1) + ":")
                else:
                    cells.append("-" * w)
            out.append(indent + "| " + " | ".join(cells) + " |")
        else:
            cells = []
            for i in range(ncols):
                cell = r[i]
                if compact:
                    cells.append(cell)
                elif aligns[i] == "right":
                    cells.append(cell.rjust(widths[i]))
                elif aligns[i] == "center":
                    pad = widths[i] - len(cell)
                    cells.append(" " * (pad // 2) + cell + " " * (pad - pad // 2))
                else:
                    cells.append(cell.ljust(widths[i]))
            out.append((indent + "| " + " | ".join(cells) + " |").rstrip())
    return out


def format_text(text: str) -> str:
    lines = text.split("\n")
    out: list[str] = []
    in_fence = False
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            out.append(line)
            i += 1
            continue
        if (
            not in_fence
            and line.lstrip().startswith("|")
            and i + 1 < len(lines)
            and SEP_RE.match(lines[i + 1])
        ):
            indent = line[: len(line) - len(line.lstrip())]
            block = []
            while i < len(lines) and lines[i].lstrip().startswith("|"):
                block.append(lines[i])
                i += 1
            rows = [split_cells(r) for r in block]
            sep_idx = 1
            aligns = [alignment(c) if is_sep_cell(c) else "left" for c in rows[sep_idx]]
            out.extend(render(indent, rows, aligns, sep_idx))
            continue
        out.append(line)
        i += 1
    return "\n".join(out)


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
        verb = "would change" if check else "formatted"
        print(f"{verb} {len(changed)} file(s):")
        for c in changed:
            print(f"  {c}")
        return 1 if check else 0
    print("all tables already aligned")
    return 0


if __name__ == "__main__":
    sys.exit(main())
