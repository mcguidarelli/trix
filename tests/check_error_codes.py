#!/usr/bin/env python3
# check_error_codes.py -- guard the error-code contract in the docs against
# the Error enum, which is the single source of truth.
#
# The Error enum in src/types.inl doubles as the process exit code (declaration
# order == numeric code), and error_sv() maps each entry to its kebab-case
# name.  This script derives name<->code straight from those two, then asserts
# docs/trix-reference.md agrees on every front:
#   1. the "Error names" list enumerates EXACTLY the enum's names (no missing
#      name like the /invalid-name we nearly shipped undocumented)
#   2. the exit-code range `1`..`N` upper bound equals the highest code
#   3. every `/name` = N citation in "Process exit codes" matches the enum
#   4. the `N` (`/io-read-error`) table row matches
#
# When a trix binary is available (via $TRIX, else ./trix or ./trix.opt) it
# also confirms the binary's own --error-codes dump matches the source-derived
# mapping, pinning source, binary, and docs together.
#
# Exit 0 on agreement, 1 on any drift.  Wired into runtests.sh / CI.

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TYPES = ROOT / "src" / "types.inl"
REFERENCE = ROOT / "docs" / "trix-reference.md"
# Other docs that pin specific code<->name mappings (tables / shell examples).
CLI_DOC = ROOT / "docs" / "cli.md"
CHEATSHEET = ROOT / "docs" / "errors-cheatsheet.md"


def derive_from_source():
    """name->code and code->name, straight from the Error enum + error_sv()."""
    text = TYPES.read_text()

    # 1. Enum entries in declaration order -> numeric code.
    m = re.search(r"enum struct Error\s*:\s*Error_t\s*\{(.*?)\n\};", text, re.DOTALL)
    if not m:
        sys.exit("check_error_codes.py: could not locate the Error enum in types.inl")
    idents = []
    for raw in m.group(1).split("\n"):
        s = raw.strip()
        if not s or s.startswith("//"):
            continue
        em = re.match(r"([A-Za-z_]\w*)", s)
        if em:
            idents.append(em.group(1))

    # 2. error_sv() case -> kebab name (\s spans the newline before `return`).
    sv = dict(re.findall(r'case\s+Error::(\w+):\s*return\s+"([^"]+)"sv;', text))

    missing = [i for i in idents if i not in sv]
    if missing:
        sys.exit(f"check_error_codes.py: error_sv() has no name for: {missing}")

    name_to_code = {}
    code_to_name = {}
    ident_to_code = {}
    for code, ident in enumerate(idents):
        name = sv[ident]
        name_to_code[name] = code
        code_to_name[code] = name
        ident_to_code[ident] = code
    return name_to_code, code_to_name, ident_to_code


def find_binary():
    cands = []
    env = os.environ.get("TRIX")
    if env:
        cands.append(env)
    cands += [str(ROOT / "trix"), str(ROOT / "trix.opt")]
    for c in cands:
        p = Path(c)
        if p.is_file() and os.access(c, os.X_OK):
            return c
    return None


def cross_check_binary(name_to_code):
    """If a binary exists, assert its --error-codes dump matches the source."""
    bin_path = find_binary()
    if not bin_path:
        print("  (no trix binary found -- skipping binary cross-check)")
        return []
    out = subprocess.run(
        [bin_path, "--error-codes"], capture_output=True, text=True
    )
    if out.returncode != 0:
        return [f"binary `{bin_path} --error-codes` exited {out.returncode}"]
    dump = {}
    for line in out.stdout.splitlines():
        if not line.strip():
            continue
        code, name = line.split("\t", 1)
        dump[name] = int(code)
    failures = []
    if dump != name_to_code:
        only_bin = set(dump) - set(name_to_code)
        only_src = set(name_to_code) - set(dump)
        mismatch = {
            n: (dump[n], name_to_code[n])
            for n in set(dump) & set(name_to_code)
            if dump[n] != name_to_code[n]
        }
        if only_bin:
            failures.append(f"binary has names absent from source: {sorted(only_bin)}")
        if only_src:
            failures.append(f"source has names absent from binary: {sorted(only_src)}")
        if mismatch:
            failures.append(f"binary/source code mismatch (bin,src): {mismatch}")
    else:
        print(f"  binary {Path(bin_path).name} --error-codes matches source ({len(dump)} entries)")
    return failures


# Structured `code <-> /name` pairings, in any of the forms the docs use:
#   * a markdown table row:   | 40 | `/undefined` | ... |
#   * a shell case arm:       40) echo "..." ;;   # /undefined
#   * an inline citation:     `/undefined` = 40
# Every such pairing in a guarded doc must agree with the enum.
PAIR_RES = [
    re.compile(r"^\|\s*(\d+)\s*\|\s*`/([a-z0-9-]+)`", re.MULTILINE),  # table row
    re.compile(r"^\s*(\d+)\)\s.*?#\s*/([a-z0-9-]+)", re.MULTILINE),    # case arm
    re.compile(r"`/([a-z0-9-]+)`\s*=\s*(\d+)"),                        # inline (name,code)
]


def check_structured_pairs(text, label, name_to_code):
    """Validate every code<->/name pairing found in `text` against the enum."""
    failures = []
    pairs = []
    for i, rx in enumerate(PAIR_RES):
        for m in rx.finditer(text):
            # the inline form captures (name, code); the others (code, name)
            if i == 2:
                name, code = m.group(1), m.group(2)
            else:
                code, name = m.group(1), m.group(2)
            pairs.append((name, int(code)))
    for name, code in pairs:
        if name not in name_to_code:
            failures.append(f"{label}: cited `/{name}` is not an Error name")
        elif name_to_code[name] != code:
            failures.append(f"{label}: `/{name}` = {code} != enum code {name_to_code[name]}")
    if not pairs:
        failures.append(f"{label}: no structured code<->name pairings found (guard may be stale)")
    return failures


def check_docs(name_to_code, code_to_name, ident_to_code):
    text = REFERENCE.read_text()
    failures = []

    # --- Check 1: the "Error names" fenced list == exactly the enum names ---
    en = text.index("**Error names**")
    f0 = text.index("```", en)
    f1 = text.index("```", f0 + 3)
    block = text[f0 + 3 : f1]
    doc_names = set(re.findall(r"/([a-z0-9-]+)", block))
    enum_names = set(name_to_code)
    missing = enum_names - doc_names
    extra = doc_names - enum_names
    if missing:
        failures.append(f'"Error names" list is MISSING: {sorted(missing)}')
    if extra:
        failures.append(f'"Error names" list has UNKNOWN names: {sorted(extra)}')

    # --- Bound the "Process exit codes" section ---
    sec0 = text.index("**Process exit codes.**")
    sec1 = text.index("\n### ", sec0)
    section = text[sec0:sec1]
    max_code = max(code_to_name)

    # --- Check 2: the `1`..`N` range upper bound == highest code ---
    rng = re.search(r"`1`\.\.`(\d+)`", section)
    if not rng:
        failures.append('"Process exit codes" range `1`..`N` not found')
    elif int(rng.group(1)) != max_code:
        failures.append(f"exit-code range upper bound {rng.group(1)} != highest code {max_code}")

    # --- Check 4: the `N` (`/io-read-error`) table row ---
    row = re.search(r"`(\d+)`\s*\(`/([a-z0-9-]+)`\)", section)
    if row:
        num, nm = int(row.group(1)), row.group(2)
        if name_to_code.get(nm) != num:
            failures.append(f"table row `{num}` (/{nm}) != enum code {name_to_code.get(nm)}")

    # --- Check 3: every structured code<->name pairing in the section ---
    failures += check_structured_pairs(section, "trix-reference Process exit codes", name_to_code)

    # --- The OTHER docs that cite specific codes (table + shell examples) ---
    # cli.md "commonly-seen codes" table, and errors-cheatsheet.md's `case`
    # arms.  These were unguarded before -- the gap that let the docs drift.
    failures += check_structured_pairs(CLI_DOC.read_text(), "cli.md", name_to_code)
    cheat = CHEATSHEET.read_text()
    failures += check_structured_pairs(cheat, "errors-cheatsheet.md", name_to_code)
    failures += check_ident_prose(cheat, "errors-cheatsheet.md", ident_to_code)

    return failures


def check_ident_prose(text, label, ident_to_code):
    """Validate `EnumIdent = N` prose (e.g. "UserError = 58") against the enum."""
    failures = []
    for m in re.finditer(r"\b([A-Z][A-Za-z]+)\s*=\s*(\d+)\b", text):
        ident, code = m.group(1), int(m.group(2))
        if ident in ident_to_code and ident_to_code[ident] != code:
            failures.append(f"{label}: {ident} = {code} != enum code {ident_to_code[ident]}")
    return failures


def main():
    name_to_code, code_to_name, ident_to_code = derive_from_source()
    print(f"Derived {len(name_to_code)} Error names from src/types.inl (codes 0..{max(code_to_name)}).")

    failures = check_docs(name_to_code, code_to_name, ident_to_code)
    failures += cross_check_binary(name_to_code)

    if failures:
        print("\nERROR-CODE DRIFT:")
        for f in failures:
            print(f"  FAIL: {f}")
        print("\nFix: re-sync the docs (trix-reference Error names + Process exit codes, "
              "cli.md table, errors-cheatsheet.md) with the enum.")
        return 1
    print("OK: docs + binary agree with the Error enum.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
