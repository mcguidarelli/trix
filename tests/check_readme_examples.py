#!/usr/bin/env python3
"""Verify that every ```trix block in README.md executes cleanly.

Extracts all fenced code blocks tagged `trix` from the top-level
README.md, writes each to a temp file, and runs it through ./trix.
Any block whose run exits non-zero fails the script.

Used in CI to prevent README examples from silently rotting the way
they did before commit 67d7c00 (which fixed a broken `computed`
operator reference).  Run from the repo root:

    ./tests/check_readme_examples.py

The binary is picked up from ./trix by default; set TRIX_BIN env var
to point elsewhere.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
README = REPO_ROOT / "README.md"
TRIX = Path(os.environ.get("TRIX_BIN", REPO_ROOT / "trix"))

if not README.exists():
    sys.exit(f"error: {README} not found")
if not TRIX.exists() or not os.access(TRIX, os.X_OK):
    sys.exit(f"error: {TRIX} not found or not executable")

# Only ```trix blocks are treated as runnable Trix programs.
# Other fences (bash, cpp, etc.) are left alone.
PATTERN = re.compile(r"```trix\n(.*?)\n```", re.DOTALL)
blocks = PATTERN.findall(README.read_text())

if not blocks:
    sys.exit("error: no ```trix blocks found in README.md")

print(f"README.md: {len(blocks)} postscript block(s) to verify against {TRIX}")

failures = []
for i, source in enumerate(blocks, 1):
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".trx", delete=False, dir="/tmp"
    ) as f:
        f.write(source)
        path = f.name
    try:
        result = subprocess.run(
            [str(TRIX), path],
            capture_output=True,
            text=True,
            timeout=30,
        )
    finally:
        os.unlink(path)

    lines = source.count("\n") + 1
    head = source.splitlines()[0][:60]
    if result.returncode != 0:
        failures.append((i, head, result.returncode, result.stderr))
        print(f"  FAIL  block {i} ({lines} lines): exit {result.returncode}")
        print(f"         first line: {head}")
        if result.stderr:
            for line in result.stderr.strip().splitlines()[:5]:
                print(f"         stderr: {line}")
    else:
        print(f"  OK    block {i} ({lines} lines): {head}")

if failures:
    print(f"\n{len(failures)} of {len(blocks)} README example(s) failed.")
    sys.exit(1)

print(f"\nAll {len(blocks)} README example(s) pass.")
