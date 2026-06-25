#!/usr/bin/env python3
"""Generate (or verify) src/op_effects.inl -- the operator stack-effect arity
table consumed by the scan-time stack-effect checker (src/scanner_stackeffect.inl).

The table maps each user-facing system operator to its fixed (in, out) operand
arity.  It is built by JOINING two sources of truth:

  * src/dispatch.inl  -- the canonical SystemName <-> operator-name mapping
                         (the sysoperator_rows() table).
  * docs/trix-reference.md section 3 -- the `before -- after` stack effects.

Only operators whose arity is STATICALLY RELIABLE for abstract interpretation
are emitted.  An operator is OMITTED (so the checker bails -- never
false-positives -- on it) when ANY of:

  * its stack effect is variadic (contains `...` or a `*` count marker);
  * an operand is a procedure (it would SPLICE the proc's runtime effect into
    the current operand stack -- e.g. if / repeat / dip / map's proc arg);
  * it is on the explicit splicer denylist below (array-of-procs / dynamic
    control ops whose notation hides the splice -- exec / case / cond / match
    / ...);
  * the docs give it conflicting arities in different places.

The combinators if / if-else / repeat ARE omitted here (they take procs) but
are special-cased by the checker itself -- it reasons about their branch nets
directly, so they need no table row.

Usage:
  tools/gen_op_effects.py --write          regenerate src/op_effects.inl
  tools/gen_op_effects.py --check          verify it is in sync (CI gate; exit 1 on drift)
  tools/gen_op_effects.py                   print the table to stdout (dry run)
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISPATCH = os.path.join(ROOT, "src", "dispatch.inl")
REFERENCE = os.path.join(ROOT, "docs", "trix-reference.md")
OUTPUT = os.path.join(ROOT, "src", "op_effects.inl")

# Operators that EXECUTE a proc (or array of procs) in the CURRENT operand-stack
# context, so their `before -- after` notation does not capture their real
# effect.  Their notation lacks a lone `proc` token (it uses `any`, `dict key`,
# `pairs-array`, `value test body`, ...), so auto-detection cannot flag them --
# they must be listed by hand.  When in doubt, ADD a name here: a denied op is
# merely un-checked (safe), an un-denied splicer would false-positive.
EXPLICIT_SPLICERS = {
    "exec", "exec-n", "case", "type-case", "cond", "match", "match-all",
    "when", "unless", "let", "destructure", "default-handler",
    # The following already carry a proc token (auto-denied); listed for clarity.
    "if", "if-else", "repeat", "loop", "while", "for", "do-while", "stopped",
    "dip", "bi", "keep", "try", "try-catch", "try-result", "finally",
}

# Operand tokens that denote a procedure (executed by the op) -> auto-deny.
# Any token CONTAINING "proc" (proc / handler-proc / cond-proc / ...) is also
# treated as proc-ish; these are the names that do not.
PROC_TOKENS = {"pred", "handler", "step", "server", "test", "body"}


def parse_operator_rows():
    """Return [(enumerator, name)] for every NON-@ operator in dispatch.inl's
    sysoperator_rows() table, in source order."""
    text = open(DISPATCH, encoding="utf-8").read()
    start = text.index("sysoperator_rows()")
    brace = text.index("{", start)
    # Find the matching close brace of the function body.
    depth = 0
    end = brace
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    body = text[brace:end]
    # Skip rows inside any preprocessor conditional (e.g. #ifdef TRIX_DEBUGGER): those
    # SystemName enumerators are themselves gated out of non-debugger builds, but
    # op_effects.inl is included unconditionally, so referencing them would not compile.
    rows = []
    cond_depth = 0
    pat = re.compile(r"\{\s*SystemName::(\w+)\s*,\s*\w+\s*,\s*\"([^\"]*)\"sv\s*\}")
    for line in body.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("#if"):
            cond_depth += 1
            continue
        if stripped.startswith("#endif"):
            cond_depth = max(0, cond_depth - 1)
            continue
        if (stripped.startswith("#else") or stripped.startswith("#elif")):
            continue
        if cond_depth != 0:
            continue
        m = pat.search(line)
        if m:
            enumerator, name = m.group(1), m.group(2)
            if not name.startswith("@"):
                rows.append((enumerator, name))
    return rows


def is_proc_ish(token):
    return (token in PROC_TOKENS) or ("proc" in token)


def parse_arities():
    """Parse docs/trix-reference.md section 3 plain code fences into
    {name: (in, out)} for fixed-arity, non-splicing operators.  Returns the
    map plus the set of names ruled out (variadic / proc / conflicting)."""
    arity = {}
    denied = set()
    in_section3 = False
    in_fence = False
    fence_is_plain = False
    for raw in open(REFERENCE, encoding="utf-8").read().splitlines():
        if raw.startswith("## "):
            in_section3 = raw.startswith("## 3.") or raw.startswith("## 3 ")
            continue
        if raw.startswith("### "):
            continue
        stripped = raw.strip()
        if stripped.startswith("```"):
            if not in_fence:
                in_fence = True
                fence_is_plain = (stripped == "```")
            else:
                in_fence = False
            continue
        if not (in_section3 and in_fence and fence_is_plain):
            continue
        # A catalog line: `name [name2 ...]<2+ spaces>before -- after   % comment`.
        # Several ops can share one effect line (`ceil floor trunc round  num -- num`),
        # so the name column is split from the effect column on a run of 2+ spaces.
        if (not raw) or raw[0].isspace() or raw[0] in "%/":
            continue
        line = raw.split("%", 1)[0].rstrip()
        if "--" not in line:
            continue
        cols = re.split(r"\s{2,}", line, maxsplit=1)
        if len(cols) == 2:
            names_group, effect = cols[0], cols[1]
        else:
            names_group, _, effect = line.partition(" ")  # un-aligned single-name fallback
        names = names_group.split()
        # `--` is a standalone token; before/after may be empty (0-in / 0-out ops).
        effect_toks = effect.split()
        if (not names) or (effect_toks.count("--") != 1):
            continue
        sep = effect_toks.index("--")
        before_toks = effect_toks[:sep]
        after_toks = effect_toks[sep + 1:]
        all_toks = before_toks + after_toks
        variadic = any(("..." in t) or t.endswith("*") for t in all_toks)
        alternation = "|" in all_toks            # `name true | false` -- variant outputs
        prose = any(("(" in t) or (")" in t) for t in all_toks)  # `(yields ...)` annotations
        proc = any(is_proc_ish(t) for t in all_toks)
        in_n, out_n = len(before_toks), len(after_toks)
        for name in names:
            if variadic or alternation or prose or proc or (name in EXPLICIT_SPLICERS):
                denied.add(name)
            elif (name in arity) and (arity[name] != (in_n, out_n)):
                denied.add(name)      # conflicting arities -> unreliable
            elif name not in denied:
                arity[name] = (in_n, out_n)
    for name in denied:
        arity.pop(name, None)
    return arity


def build_rows():
    operators = parse_operator_rows()
    arity = parse_arities()
    rows = []
    for enumerator, name in operators:
        if name in arity:
            in_n, out_n = arity[name]
            rows.append((enumerator, name, in_n, out_n))
    return rows


BANNER = """//===----------------------------------------------------------------------===//
//                                                                            //
//    ______    _                                                             //
//   /_  __/___(_)_  __                                                       //
//    / / / __/ /\\ \\/ /       Stack-Based Interpreter & VM                    //
//   / / / / / /  > · <      C++23 · Single-Header Library                    //
//  /_/ /_/ /_/  /_/\\_\\     Copyright 2026 Mark Guidarelli                    //
//                                                                            //
// Licensed under the Apache License, Version 2.0 (the "License");            //
// you may not use this file except in compliance with the License.           //
// You may obtain a copy of the License at                                    //
//                                                                            //
//     https://www.apache.org/licenses/LICENSE-2.0                            //
//                                                                            //
// Unless required by applicable law or agreed to in writing, software        //
// distributed under the License is distributed on an "AS IS" BASIS,          //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   //
// See the License for the specific language governing permissions and        //
// limitations under the License.                                             //
//                                                                            //
//===----------------------------------------------------------------------===//"""


def render(rows):
    lines = [BANNER, "private:", ""]
    lines.append("//===--- Operator Stack-Effect Arity Table (GENERATED) ---===//")
    lines.append("//")
    lines.append("// GENERATED by tools/gen_op_effects.py from src/dispatch.inl (operator")
    lines.append("// names) and docs/trix-reference.md section 3 (stack effects).  DO NOT EDIT")
    lines.append("// BY HAND -- run `tools/gen_op_effects.py --write` to regenerate, and")
    lines.append("// `tools/gen_op_effects.py --check` (CI gate) to verify it is in sync.")
    lines.append("//")
    lines.append("// Each row gives a user-facing operator's fixed (in, out) operand arity for")
    lines.append("// the scan-time stack-effect checker (src/scanner_stackeffect.inl).  Only")
    lines.append("// operators whose arity is statically reliable appear here: variadic ops")
    lines.append("// (`...`), proc-splicing control/combinator ops, and pattern-match ops are")
    lines.append("// deliberately omitted so the checker bails (never false-positives) on them.")
    lines.append("// The combinators if / if-else / repeat are special-cased by the checker and")
    lines.append("// need no row here.")
    lines.append("")
    lines.append("struct OpEffect {")
    lines.append("    uint8_t in{0};")
    lines.append("    uint8_t out{0};")
    lines.append("    bool known{false};")
    lines.append("};")
    lines.append("")
    lines.append("struct OpEffectRow {")
    lines.append("    SystemName m_name;")
    lines.append("    uint8_t m_in;")
    lines.append("    uint8_t m_out;")
    lines.append("};")
    lines.append("")
    lines.append(f"// {len(rows)} operators with a statically reliable fixed arity.")
    lines.append("static constexpr OpEffectRow sm_op_effect_rows[] = {")
    width = max((len(e) for e, _, _, _ in rows), default=0)
    for enumerator, name, in_n, out_n in rows:
        pad = " " * (width - len(enumerator))
        lines.append(f"        {{SystemName::{enumerator},{pad} {in_n}, {out_n}}},  // {name}")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def clang_format(content):
    """Run the rendered content through clang-format so the generated file is
    byte-identical to what the clang-format CI gate expects (else --check and the
    format gate would disagree on row alignment)."""
    try:
        r = subprocess.run(
            ["clang-format", "--style=file", "--assume-filename=op_effects.inl"],
            input=content, capture_output=True, text=True, cwd=ROOT)
    except FileNotFoundError:
        print("warning: clang-format not found; emitting unformatted output", file=sys.stderr)
        return content
    if r.returncode != 0:
        print(f"warning: clang-format failed ({r.stderr.strip()}); emitting unformatted output", file=sys.stderr)
        return content
    return r.stdout


def normalize(content):
    """Strip ALL whitespace so a comparison ignores layout entirely -- clang-format
    inserts spaces around punctuation ({a,b} -> { a, b }) and aligns columns, so a
    single-space collapse is not enough; every token here is punctuation-delimited,
    so removing whitespace cannot merge distinct values.  --check verifies the
    operator/arity DATA is in sync; the file's *formatting* is the separate
    clang-format CI gate's job.  Decoupling them keeps --check working with any
    clang-format version, or none at all (a CI runner without clang-format must not
    turn a data check into a format check)."""
    return "".join(content.split())


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "--dry"
    rows = build_rows()
    rendered = render(rows)
    if mode == "--check":
        # Compare DATA only (whitespace-insensitive): no clang-format dependency.
        existing = open(OUTPUT, encoding="utf-8").read() if os.path.exists(OUTPUT) else ""
        if normalize(rendered) != normalize(existing):
            print("src/op_effects.inl is OUT OF SYNC with dispatch.inl / trix-reference.md.",
                  file=sys.stderr)
            print("Regenerate with: tools/gen_op_effects.py --write", file=sys.stderr)
            return 1
        print(f"src/op_effects.inl in sync ({len(rows)} operators)")
        return 0
    content = clang_format(rendered)  # --write / dry-run emit the formatted file
    if mode == "--write":
        open(OUTPUT, "w", encoding="utf-8").write(content)
        print(f"wrote {OUTPUT} ({len(rows)} operators)")
        return 0
    print(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
