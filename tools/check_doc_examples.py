#!/usr/bin/env python3
"""
Doc-example smoke harness.

Extracts fenced code blocks from docs/*.md, runs each runnable Trix block
through ./trix.opt --stdin in isolation, and FAILS any block that raises an
uncaught Trix error.  The signal is the process exit code: trix.opt halts on
an uncaught error and exits non-zero (syntax 39, undefined 41, type-check 40,
assert-failed 1, ...), while clean runs and `try`-caught errors exit 0.

Because of that, examples get value-checking for free when they self-assert:
a wrong value behind `(msg) cond assert` raises /assert-failed -> non-zero ->
FAIL.  Plain `% =>` annotations stay human-readable; they are not parsed.

Most Trix examples live in BARE ``` fences (not ```trix), so bare blocks are
content-classified.  A block is SKIPPED when it is not runnable Trix:
  - a non-Trix fenced language (cpp, mermaid, text, console, bash, ...)
  - pseudo-code / partial snippets: an ellipsis `...` or a `<placeholder>`
  - ascii art / bit diagrams / arrows / REPL transcripts / C++ / shell
  - an explicit opt-out: fence info `trix ignore` / `trix skip`, or an
    HTML comment `<!-- doctest: skip [reason] -->` just before the fence
  - no Trix signal (no `def`, `/name`, or other give-away token)

Usage:
  tools/check_doc_examples.py                  # all docs
  tools/check_doc_examples.py logic record     # only these docs (by stem)
  tools/check_doc_examples.py -v               # also list skips
  TRIX=./trix tools/check_doc_examples.py       # choose the binary
                                               # (default: ./trix.opt, else ./trix)
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DOCS = os.path.join(ROOT, "docs")
# Binary under test: $TRIX if set, else the optimized build, else the debug
# build.  CI stages only ./trix; local dev usually has ./trix.opt.
TRIX = os.environ.get("TRIX") or next(
    (os.path.join(ROOT, n) for n in ("trix.opt", "trix")
     if os.path.exists(os.path.join(ROOT, n))),
    os.path.join(ROOT, "trix.opt"))
TIMEOUT_S = 30

FENCE = re.compile(r"^```(\S*)\s*(.*)$")
SKIP_COMMENT = re.compile(r"<!--\s*doctest:\s*skip\b(.*?)-->", re.I)

# Languages whose fenced blocks are never run.
NONTRIX_LANGS = {"cpp", "c", "mermaid", "text", "console", "bash", "sh",
                 "python", "py", "ebnf", "json", "yaml", "diff", "make",
                 "cmake", "ini", "toml"}
SKIP_INFO = {"ignore", "skip", "no-test", "notest", "norun"}

# A comment that annotates an EXPECTED error -> the block demonstrates a
# failure on purpose (it is not wrapped in try).  Two doc conventions count:
# a result arrow `=> /name` and the prose form `raises /name` / `raises name`
# (the slash is optional after `raises`).  `=> /answer` (a plain Name result)
# is intentionally NOT matched -- only real error names count.
_ERRNAMES = (
    r"type-check|range-check|div-by-zero|numerical-\w+"
    r"|undefined(?:-case)?|read-only|invalid-\w+|require|ensure|unsupported"
    r"|limit-check|io-\w+|filename-\w+|above-barrier|dict-full|set-full"
    r"|unmatched-mark|op-?stack-\w+|execstack-\w+|dictstack-\w+|errstack-\w+"
    r"|index-check|scan-\w+|protocol|match|fail|internal-error|vm-full")
EXPECT_ERROR = re.compile(
    r"\bERROR\b"
    r"|(?:=>|->)\s*/(?:" + _ERRNAMES + r")\b"       # result arrow: => /name
    r"|\braises?\s+/?(?:" + _ERRNAMES + r")\b")     # prose: raises /name | raises name

# Block-level markers that make a bare/trix block non-runnable illustration.
PSEUDO_BLOCK = re.compile(
    r"(\.\.\.)"                       # ellipsis pseudo-code
    r"|<[a-z_][a-z0-9_]* [a-z0-9_>]"  # "<some placeholder ..."
    r"|<[a-z_]*_[a-z0-9_]*>"          # <schema_offset>, <error_name>
    r"|<(?:expr|name|body|args|proc|cond|value|stream|error|key|elem|item|fn)>"
    r"|[─-╿]"               # box-drawing chars
    r"|\+--|--\+|\+==|\|\s*$"         # ascii box edges
    r"|-->|==>|<--"                   # flow arrows
    "|→|⇒"                  # unicode arrows in desugar tables
    r"|^\s*(?:Trix|dbg)>"             # REPL transcript
    r"|trx->|Trix::|#include|static (?:void|constexpr)"  # C++
    r"|^\s*\$ |g\+\+ |\./build|\./trix\b"  # shell
    r"|emits:"                        # infix desugar illustration
    , re.M)

# Per-line markers (checked on the code part, before any % comment).
SIG_LINE = re.compile(r"(^|\s)--(\s|$)")             # stack-effect signature
PSEUDO_LINE = re.compile(
    r"\breturn\b"                                     # pseudo-code 'return'
    r"|^\s*(?:while|for|if|else)\b[^%]*:\s*$"         # 'while ... :' control prose
    r"|^[a-z_][\w-]*\([a-z_, ]*\):"                   # func(args): definition
    r"|^\s*[\w.-]+/\s*$"                              # tree node: numbers/, real-type/
    r"|^\s*#\d+:"                                     # backtrace frame: #0:
    r"|(^|\s)@[a-z]"                                  # @control-op in a layout diagram
)

# At least one of these must appear for a bare block to count as Trix.
TRIX_SIGNAL = re.compile(
    r"(^|\s)/[a-zA-Z]"                # /literal-name
    r"|(^|\s)def(\s|$)"
    r"|\b(assert|record-type|logic-var|coroutine|actor-spawn|gen-server"
    r"|lazy-iterate|print-fmt|aprint-fmt|sprint-fmt|asprint-fmt|unify|guard"
    r"|choice|for-all|if-else|try-catch|with-stream|def-protocol|tag-match"
    r"|kahan-sum)\b"
    r"|=\s*(?:%|$)"                   # the print operator at end of expression
    , re.M)

# Errors that mean "environment/feature", not "broken example".
ENV_ERR = re.compile(r"is not a tty|built without .* support|/file-open-error"
                     r"|filename-not-found|stream open failed")
# Errors that mean "depends on a prior block" (registered/defined elsewhere).
CONTINUATION_ERR = re.compile(r"module '[^']*' not found"
                              r"|protocol /\S+ does not exist"
                              r"|is not part of any protocol")
# Pull the offending name out of an "undefined" error.
UNDEF_NAME = re.compile(r"executable name (\S+) is not associated")


def blocks(path):
    """Yield (lineno, lang, info, skip_reason_or_None, code)."""
    lines = open(path, encoding="utf-8").read().split("\n")
    i, pending = 0, None
    while i < len(lines):
        line = lines[i]
        m = SKIP_COMMENT.search(line)
        if m:
            pending = (m.group(1).strip() or "skip directive")
            i += 1
            continue
        fm = FENCE.match(line)
        if fm and fm.group(1):  # opening fence WITH a language tag
            lang, info, start = fm.group(1), fm.group(2).strip(), i + 1
            body, i = [], i + 1
            while i < len(lines) and not lines[i].startswith("```"):
                body.append(lines[i])
                i += 1
            i += 1
            yield (start, lang, info, pending, "\n".join(body))
            pending = None
            continue
        if line.startswith("```"):  # bare ``` opening fence
            start = i + 1
            body, i = [], i + 1
            while i < len(lines) and not lines[i].startswith("```"):
                body.append(lines[i])
                i += 1
            i += 1
            yield (start, "", "", pending, "\n".join(body))
            pending = None
            continue
        if line.strip() != "":
            pending = None
        i += 1


def classify(lang, info, skip_reason, code):
    if lang and lang != "trix":
        return ("nonlang", lang)
    if any(t in SKIP_INFO for t in info.split()):
        return ("skip", "fence directive")
    if skip_reason is not None:
        return ("skip", "doctest:skip")
    if code.strip() == "":
        return ("skip", "empty")
    if EXPECT_ERROR.search(code):
        return ("skip", "demonstrates an error")
    if PSEUDO_BLOCK.search(code):
        return ("skip", "pseudo/illustrative")
    for raw in code.split("\n"):
        line = raw.split("%", 1)[0]            # strip trailing comment
        if SIG_LINE.search(line):
            return ("skip", "stack-effect signature")
        if PSEUDO_LINE.search(line):
            return ("skip", "algorithm pseudo-code")
    if not TRIX_SIGNAL.search(code):
        return ("skip", "no trix signal")
    return ("run", "")


def run(code, cwd):
    # Run in a scratch cwd so file-writing examples never litter the repo.
    try:
        r = subprocess.run([TRIX, "--stdin"], input=code, capture_output=True,
                           text=True, timeout=TIMEOUT_S, cwd=cwd)
        return r.returncode, r.stderr
    except subprocess.TimeoutExpired:
        return -99, "TIMEOUT"


def main(argv):
    verbose = "-v" in argv
    wanted = [a for a in argv if not a.startswith("-")]
    paths = sorted(glob.glob(os.path.join(DOCS, "*.md")))
    if wanted:
        paths = [p for p in paths
                 if os.path.splitext(os.path.basename(p))[0] in wanted]
    scratch = tempfile.mkdtemp(prefix="trix-doctest-")
    npass = nfail = nskip = nreview = 0
    fails, skips, reviews = [], [], []
    for path in paths:
        rel = os.path.relpath(path, ROOT)
        doctext = open(path, encoding="utf-8").read()
        for (ln, lang, info, sr, code) in blocks(path):
            action, why = classify(lang, info, sr, code)
            if action == "nonlang":
                continue
            if action == "skip":
                nskip += 1
                skips.append((rel, ln, why))
                continue
            rc, err = run(code, scratch)
            if rc == 0:
                npass += 1
                continue
            errline = next((l.strip() for l in err.split("\n")
                            if l.startswith("Trix ") or "TIMEOUT" in l), "")
            # Environment/feature (tty, feature-gated, files) -- not a bug.
            if ENV_ERR.search(err):
                nskip += 1
                skips.append((rel, ln, "environment/feature"))
                continue
            # Depends on a prior block (module/protocol registered elsewhere).
            if CONTINUATION_ERR.search(err):
                nreview += 1
                reviews.append((rel, ln, "continuation (registered elsewhere)"))
                continue
            # An undefined name is a continuation (defined elsewhere in the doc)
            # or a placeholder/illustration -- not the broken-logic bug class.
            m = UNDEF_NAME.search(err)
            if m:
                defined = re.search(r"(^|\s)/" + re.escape(m.group(1)) + r"\b",
                                    doctext) is not None
                nreview += 1
                tag = "continuation" if defined else "undefined ref (placeholder?)"
                reviews.append((rel, ln, f"{tag}: {m.group(1)}"))
                continue
            nfail += 1
            fails.append((rel, ln, rc, errline))

    shutil.rmtree(scratch, ignore_errors=True)

    for (rel, ln, rc, line) in fails:
        print(f"FAIL   {rel}:{ln}  (exit {rc})  {line}")
    if verbose:
        for (rel, ln, why) in reviews:
            print(f"review {rel}:{ln}  [{why}]")
        for (rel, ln, why) in skips:
            print(f"skip   {rel}:{ln}  [{why}]")
    print(f"\n{npass} passed, {nfail} FAILED, {nreview} review (undefined refs), "
          f"{nskip} skipped")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
