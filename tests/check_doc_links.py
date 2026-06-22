#!/usr/bin/env python3
"""Verify every relative markdown link + anchor in the shipped docs resolves.

Scans docs/*.md (the shipped set -- docs/internal/ is excluded by design;
those are historical records) plus the top-level README.md.  For each
[text](target) link:

  * http(s)/mailto targets are skipped,
  * a relative file target must exist on disk,
  * a #fragment into a .md file must match a real heading, slugged with
    GitHub's algorithm (lowercase, backticks dropped, punctuation stripped,
    EACH space becomes a hyphen -- no collapsing, so "5. `:` command
    reference" slugs to "5--command-reference"; duplicate slugs get -1/-2
    suffixes).

Added in Phase-2 Session 2 after an audit found 19 broken template-TOC
anchors that had rotted silently.  Run from the repo root:

    ./tests/check_doc_links.py
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SHIPPED = sorted((REPO_ROOT / "docs").glob("*.md")) + [REPO_ROOT / "README.md"]


def slugify(heading: str) -> str:
    h = re.sub(r"`", "", heading.strip().lower())
    h = re.sub(r"[^\w\s-]", "", h)
    return h.replace(" ", "-")  # GitHub: each space -> hyphen, no collapsing


_anchor_cache: dict[Path, set[str]] = {}


def anchors_for(path: Path) -> set[str]:
    if path not in _anchor_cache:
        slugs: set[str] = set()
        counts: dict[str, int] = {}
        for m in re.finditer(r"^#{1,6}\s+(.+)$", path.read_text(), re.M):
            slug = slugify(m.group(1))
            n = counts.get(slug, 0)
            counts[slug] = n + 1
            slugs.add(slug if n == 0 else f"{slug}-{n}")
        _anchor_cache[path] = slugs
    return _anchor_cache[path]


LINK = re.compile(r"\[([^\]]*)\]\(([^)\s]+)\)")

broken: list[str] = []
total = 0
for f in SHIPPED:
    text = f.read_text()
    for m in LINK.finditer(text):
        target = m.group(2)
        if target.startswith(("http://", "https://", "mailto:")):
            continue
        total += 1
        line = text[: m.start()].count("\n") + 1
        path_part, _, frag = target.partition("#")
        dest = f if not path_part else (f.parent / path_part).resolve()
        rel = f.relative_to(REPO_ROOT)
        if path_part and not dest.exists():
            broken.append(f"{rel}:{line} -> {target} (FILE MISSING)")
            continue
        if frag and dest.suffix == ".md" and frag not in anchors_for(dest):
            broken.append(f"{rel}:{line} -> {target} (ANCHOR MISSING)")

print(f"checked {total} relative links across {len(SHIPPED)} shipped files")
if broken:
    print(f"{len(broken)} BROKEN:")
    for b in broken:
        print(f"  {b}")
    sys.exit(1)
print("all links and anchors resolve")
