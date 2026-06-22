#!/usr/bin/env python3
# check_operator_throws.py -- keep the per-operator "throws" documentation in
# sync with the `// throws:` annotations in src/, the single source of truth.
#
# Each operator implementation carries a `// throws: a, b, c` doc-comment (or
# `// throws: (none)`) immediately above its definition.  This script:
#   1. extracts every annotation and maps it to the operator name(s) it backs
#      (via the dispatch table: fn -> "name"); handles both definition forms
#      (`static void fn(Trix *` and dispatch.inl's `constexpr auto fn{...}`)
#   2. VALIDATES every token is a real Error name (paren-aware; a trailing
#      "(qualifier)" is stripped), allowing two documented pseudo-tokens:
#         <...>  -- re-raises a contextual error not statically known
#                   (e.g. whatever a pipeline stage threw)
#         exit   -- terminates the process (control flow, not a catchable error)
#   3. enforces COMPLETENESS: every shipping (opt) user-facing (non-@) operator
#      must carry a `// throws:` annotation
#   4. GENERATES the "Operator Error Reference" table in docs/trix-reference.md
#      and fails on drift.  Run with --write to (re)generate that table.
#
# Exit 0 on agreement, 1 on any drift/violation.  Wired into CI.

import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TYPES = ROOT / "src" / "types.inl"
DISPATCH = ROOT / "src" / "dispatch.inl"
REFERENCE = ROOT / "docs" / "trix-reference.md"

BEGIN = "<!-- BEGIN GENERATED: operator-throws -->"
END = "<!-- END GENERATED: operator-throws -->"

DEF_RE = re.compile(r"static void (\w+)\(Trix \*|constexpr auto (\w+)\{")


def valid_error_names():
    text = TYPES.read_text()
    return {v for _, v in re.findall(r'case\s+Error::(\w+):\s*return\s+"([^"]+)"sv;', text)}


def dispatch_rows():
    """[(fn, name, debug_only)] from sysoperator_rows(), #ifdef-aware."""
    lines = DISPATCH.read_text().split("\n")
    start = next(i for i, l in enumerate(lines) if "sysoperator_rows()" in l)
    depth = 0
    rows = []
    seen = False
    for l in lines[start:]:
        s = l.strip()
        if s.startswith("#if"):
            depth += 1
            continue
        if s.startswith("#endif"):
            depth = max(0, depth - 1)
            continue
        m = re.search(r'SystemName::\w+,\s+(\w+),\s+"([^"]*)"sv', l)
        if m:
            rows.append((m.group(1), m.group(2), depth > 0))
            seen = True
        if s.startswith("};") and depth == 0 and seen:
            break
    return rows


def extract_annotations():
    """fn -> raw throws string.  Handles annotations wrapped across several
    `//` continuation lines (clang-format wraps past the 132-col limit)."""
    ann = {}
    for f in sorted(glob.glob(str(ROOT / "src" / "*.inl"))):
        lines = Path(f).read_text().split("\n")
        for i, ln in enumerate(lines):
            m = re.match(r"\s*// throws:\s*(.*)", ln)
            if not m:
                continue
            parts = [m.group(1).strip()]
            j = i + 1
            # gather contiguous `//` continuation lines (wrapped throws list)
            while j < len(lines) and lines[j].strip().startswith("//") \
                    and not lines[j].strip().startswith("// throws:"):
                parts.append(re.sub(r"^\s*//\s?", "", lines[j]).strip())
                j += 1
            # a blank line may sit between the comment block and the def
            while j < len(lines) and lines[j].strip() == "":
                j += 1
            if j < len(lines):
                dm = DEF_RE.search(lines[j])
                if dm:
                    ann[dm.group(1) or dm.group(2)] = " ".join(parts).strip()
    return ann


def paren_split(s):
    out, cur, depth = [], "", 0
    for ch in s:
        if ch == "(":
            depth += 1
            cur += ch
        elif ch == ")":
            depth -= 1
            cur += ch
        elif ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


def parse_throws(s):
    """Return (base_tokens, invalid_tokens). `(none)`/`none` -> empty."""
    s = s.strip()
    if s.lower() in ("(none)", "none", ""):
        return [], []
    bases, invalid = [], []
    valid = valid_error_names()
    for tok in paren_split(s):
        tok = tok.strip()
        if not tok:
            continue
        base = re.sub(r"\s*\(.*$", "", tok).strip()  # drop trailing "(qualifier)"
        if base.lower() in ("none", ""):
            continue
        if base in valid or base == "exit" or re.fullmatch(r"<[^>]+>", base):
            bases.append(base)
        else:
            invalid.append(base)
    return bases, invalid


def build_table(ann, rows):
    """Sorted markdown rows for every shipping (opt) non-@ operator."""
    names = sorted({name for fn, name, dbg in rows if not name.startswith("@") and not dbg})
    fn_for = {}
    for fn, name, dbg in rows:
        if not name.startswith("@") and not dbg:
            fn_for[name] = fn
    out = []
    for name in names:
        fn = fn_for[name]
        bases, _ = parse_throws(ann.get(fn, "(none)"))
        uniq = sorted(set(bases))
        cell = ", ".join(f"`{b}`" for b in uniq) if uniq else "(none)"
        out.append(f"| `{name}` | {cell} |")
    return names, out


def generate_block(table_rows):
    return "\n".join([
        BEGIN,
        "### 3.44 Operator Error Reference",
        "",
        "For each shipping operator, the errors it may raise (the `/error` names",
        "a `try` / `try-catch` sees).  This table is generated from the",
        "`// throws:` annotations in `src/` and kept in sync by",
        "`tests/check_operator_throws.py` (regenerate with `--write`).  Two",
        "pseudo-tokens appear: `<...>` means the operator re-raises a contextual",
        "error not known statically (e.g. whatever a pipeline stage threw), and",
        "`exit` means it terminates the process (control flow, not a catchable",
        "error).  `(none)` means the operator raises no error.",
        "",
        "| Operator | May raise |",
        "| --- | --- |",
        *table_rows,
        END,
    ])


def main():
    write = "--write" in sys.argv
    ann = extract_annotations()
    rows = dispatch_rows()
    failures = []

    # --- 1. validate every annotation's tokens ---
    for fn, raw in sorted(ann.items()):
        _, invalid = parse_throws(raw)
        if invalid:
            failures.append(f"{fn}: invalid throws token(s) {invalid} in `// throws: {raw}`")

    # --- 2. completeness: every opt non-@ operator has an annotation ---
    opt_uf = [(fn, name) for fn, name, dbg in rows if not name.startswith("@") and not dbg]
    missing = sorted({name for fn, name in opt_uf if fn not in ann})
    if missing:
        failures.append(f"{len(missing)} operator(s) missing `// throws:`: {missing[:15]}{' ...' if len(missing) > 15 else ''}")

    # --- 3. generate / drift-check the doc table ---
    names, table_rows = build_table(ann, rows)
    block = generate_block(table_rows)
    doc = REFERENCE.read_text()
    if write:
        if BEGIN in doc and END in doc:
            doc = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END), lambda m: block, doc, flags=re.DOTALL)
        else:
            doc = doc.replace("## 4. Built-in Variables and Constants", block + "\n\n## 4. Built-in Variables and Constants", 1)
        REFERENCE.write_text(doc)
        print(f"WROTE Operator Error Reference: {len(names)} operators.")
        return 0

    if BEGIN not in doc or END not in doc:
        failures.append("trix-reference.md is missing the operator-throws GENERATED block (run --write)")
    else:
        current = doc[doc.index(BEGIN):doc.index(END) + len(END)]
        if current.strip() != block.strip():
            failures.append("docs/trix-reference.md Operator Error Reference is STALE (run tests/check_operator_throws.py --write)")

    print(f"Annotations: {len(ann)}; shipping non-@ operators: {len(opt_uf)}; table rows: {len(names)}.")
    if failures:
        print("\nOPERATOR-THROWS DRIFT:")
        for f in failures:
            print(f"  FAIL: {f}")
        return 1
    print("OK: every operator annotated with valid Error names; doc table in sync.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
