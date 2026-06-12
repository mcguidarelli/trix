# Trix Repo Tools

Small maintenance scripts for the repository.  None are part of the
interpreter or the build; they operate on source, documentation, and asset
files (`check_doc_examples.py` additionally runs the built binary as a test
oracle).

| Script                     | Purpose                                                              |
| -------------------------- | -------------------------------------------------------------------- |
| `align_code_comments.py`   | Align ragged trailing `%` comments in Trix code fences (idempotent)  |
| `check_doc_examples.py`    | Smoke-run every runnable fenced doc example through the binary       |
| `check_signed_char.py`     | Gate: clang `-fsigned-char` catches `char`<->`vm_t` sign conversions |
| `cpp_style.py`             | Check/fix STYLE.md parens + `(void)` -> `static_cast<void>` rules    |
| `find_guard_returns.py`    | Locate early-return guards with fall-through (fold to single-flow)   |
| `find_redundant_else.py`   | Locate `if (c) { return; } else {...}` redundant-else control flow   |
| `find_tail_return.py`      | Locate redundant work-then-return tails before a two-way `} else {`  |
| `format_md_tables.py`      | Align markdown pipe-table columns in the docs (idempotent)           |
| `make_trix_logo.py`        | Render the Trix logo badge (`assets/trix-logo.{png,svg}`)            |
| `migrate_dollar_dollar.py` | One-off: rewrite `$$(...)` (old promoted-infix) to `$[...]`          |
| `migrate_g_to_dollar.py`   | One-off: rewrite the `#g` storage suffix to `#$`                     |

## align_code_comments.py

Aligns the trailing `% ...` comment column inside Trix code fences.  Within each
run of consecutive comment-bearing lines, it pads the space before `%` so every
comment starts at the same column (the run's longest line plus one).  Only
`%`-comment fences are touched -- bare ```` ``` ````, ```` ```trix ````, and
```` ```postscript ```` -- so cpp / python / text / console / bash / mermaid
blocks (where `%` is modulo, a format specifier, a prompt, or a percentage) are
skipped.  Comment detection is paren-, escape-, and brace-aware, so a `%` inside
`(...)` or `{...}` (a proc body or a PrintFmt directive like `{0:I%Y...}`) is not
mistaken for a comment.  Only ragged runs change, only spaces before `%` ever
move, and the pass is idempotent.

```bash
./tools/align_code_comments.py docs/*.md                     # rewrite in place
./tools/align_code_comments.py --check docs/*.md              # CI mode: exit 1 if any file would change
```

## check_doc_examples.py

Extracts fenced code blocks from `docs/*.md`, runs each runnable Trix block
through the interpreter (`$TRIX`, default `./trix.opt`, else `./trix`) in an
isolated scratch directory, and fails any block that raises an uncaught error
-- the oracle is the process exit code.  Self-asserting examples
(`(msg) cond assert`) get value-checking for free.  Non-runnable blocks are
skipped by content-classification or an explicit
`<!-- doctest: skip [reason] -->` comment before the fence; intentional-error
demos are recognized by their `% ERROR`, `=> /name`, or `raises /name`
annotation.  Wired into CI.

```bash
./tools/check_doc_examples.py                 # all docs (exit 0 = green)
./tools/check_doc_examples.py logic record    # only these docs (by stem)
./tools/check_doc_examples.py -v              # also list review/skip lines
```

## check_signed_char.py

Catches `char`<->`vm_t` (and any `char`<->unsigned) sign conversions before they
reach x86 CI.  Trix is a single-header library embedded into arbitrary HOST
programs, so it must compile clean under whatever `char` signedness the host's
target uses -- SIGNED on x86-64, UNSIGNED on ARM/AArch64.  `vm_t` is `uint8_t`
(always unsigned), so every `char`<->`vm_t` crossing is a signedness change on
x86, which clang's `-Wsign-conversion` flags there.  On the ARM dev box `char`
is *already* unsigned, so that warning is structurally **blind** to the class
and the leak only surfaces as a red x86 CI run.

The gate does a syntax-only clang compile with `-fsigned-char` so `char` becomes
signed, then `-Wsign-conversion -Werror` lights up the crossings -- it makes the
ARM box see exactly what x86 CI's clang sees.  It must be **clang**: gcc's
`-Wsign-conversion` does not flag `char`<->`unsigned char` (which is why these
leaks compiled clean under the gcc build).  It compiles with the maximal feature
set (`-DTRIX_DEBUGGER -DTRIX_HEAP_TRACKING`), so it also type-checks the debugger
/ heap-tracking code that the CMake CI build (which defines only `TRX_DEBUG`)
leaves out.  Fixes go in SOURCE as explicit `static_cast<vm_t>` /
`static_cast<char>` at the crossing -- a header cannot dictate the host's flags.
Pairs with `./build.sh --signed-char`, which covers the RUNTIME half (build with
signed `char` and run the suite).  Wire `--check` into CI.  Compiler:
`$CLANGXX`, else `clang++-20`, else `clang++` (clang >= 20 -- the source uses
`[[assume]]`).

```bash
./tools/check_signed_char.py              # run the gate (exit 1 on any finding)
./tools/check_signed_char.py --check      # same; explicit alias for CI steps
./tools/check_signed_char.py -v           # also echo the clang command
```

## cpp_style.py

Checks and auto-fixes the two STYLE.md rules that no mainline clang-tidy check
covers (see STYLE.md "Braces and Parentheses" and "No C-style casts"):

1. **Full parenthesization** -- every operator-bearing operand of a compound
   condition / return value is wrapped in its own parentheses at every nesting
   level (`a == b && (c & M) != 0` -> `(a == b) && ((c & M) != 0)`).  A real
   tokenizer + precedence-climbing parser inserts the parens at the correct
   operand boundaries; same-precedence chains stay flat, lone primaries and
   unary operands stay bare, and only `(` / `)` are ever inserted, so unrelated
   code is byte-preserved.  Run `clang-format` afterwards for any line wrapping.
2. **Void-cast discards** -- `(void)expr` -> `static_cast<void>(expr)`.  A
   function-parameter `void` and a `(void *)` pointer cast are left untouched.

Both transforms are idempotent.  Expressions it cannot confidently parse (an
ambiguous template `<>`, an unexpected token) are SKIPPED and reported on
stderr, never mis-transformed; the known safe skips on the current tree are a
few variable templates (`std::numbers::e_v<real_t>`) that need no edit.  Wire
`--check` into CI as the enforcement gate.

```bash
./tools/cpp_style.py --check                  # CI gate: exit 1 if any violation
./tools/cpp_style.py --report                 # list sites + per-file counts
./tools/cpp_style.py --diff src/gc.inl        # before/after for one file
./tools/cpp_style.py --apply                  # rewrite in place
./tools/cpp_style.py --apply --void-only      # restrict to one rule
```

## find_guard_returns.py

Read-only locator for the **single-flow** control-flow style (STYLE.md "Control
Flow"): an early-return *guard with fall-through* -- `if (cond) { ...; return; }`
followed by sibling statements in the same scope -- which folds into one
top-to-bottom flow (`if (!cond) { <rest> }`, or `if (cond) {...} else {...}`
when the guard does work; a cascade becomes a nested if / else-if chain).

This is the **inverse direction** of `find_redundant_else.py` /
`find_tail_return.py` below, which served the earlier (now superseded)
guard-clause-flattening sweep.  Per the current rule, a `return;` inside a
for/while/do loop is a kept early-exit and is skipped; a `gvm_for_each`-style
callback lambda is *not* a detected loop, so its per-chunk skips ARE reported
and do get inverted.  Heuristic / line-based -- read each hit (compound
conditions need De Morgan; comments may need relocating).  A clean tree prints
zero sites.

```bash
./tools/find_guard_returns.py             # scan src/*.inl
./tools/find_guard_returns.py src/gc.inl  # scan one file
```

## find_redundant_else.py / find_tail_return.py

Read-only control-flow auditors (locators, not auto-fixers -- read every hit
before editing) used by the 2026-06 flattening sweep that removed redundant
`else` and bare/redundant `return` noise from `src/*.inl`.  NOTE: that sweep's
direction was later reversed in favour of single-flow nesting (see
`find_guard_returns.py` above); these two remain as historical auditors.

`find_redundant_else.py` flags `if (cond) { return; } else { ... }` -- a then-
branch that is *solely* a valueless `return;` (comment-line and trailing-comment
returns included) immediately before an `else`, reported as two-way (`} else {`)
or chained (`} else if`).  `find_tail_return.py` flags the *work-then-return*
tail -- `if (cond) { ...; return; } else { ... }` where the `if/else` is a void
function's last statement, so the `return;` is redundant.  It tracks brace scope
to require genuine fn-tail (no enclosing loop/switch) and handles multi-line
signatures; its header documents the known blind spots (middle `else if` arms,
fold/`else { single if }` collapses it does not detect).

Both default to `src/*.inl` and accept explicit file arguments.  A clean tree
prints zero sites.

```bash
./tools/find_redundant_else.py            # scan src/*.inl
./tools/find_tail_return.py src/gc.inl    # scan one file
```

## format_md_tables.py

Pads every pipe-table cell so the columns line up in the raw source (GitHub
renders aligned and unaligned tables identically, but humans read the source).
Tables inside ``` code fences are never touched, escaped pipes (`\|`) are
respected, and very wide tables are normalized to the compact one-space style
instead.  The pass is idempotent.

```bash
./tools/format_md_tables.py docs/*.md README.md      # rewrite in place
./tools/format_md_tables.py --check docs/*.md         # CI mode: exit 1 if any file would change
```

## make_trix_logo.py

Renders the Trix logo badge in `assets/` from the canonical ASCII logo.  Each
glyph is positioned on a fixed cell grid so the art aligns exactly regardless of
how a renderer measures a monospace advance: the PNG is rasterized with Pillow +
DejaVu Sans Mono, and the SVG embeds that same font as a data-URI `@font-face`
so every viewer draws identical glyphs (a plain font-family reference renders
"off" wherever the font is substituted).  Requires `Pillow` and the DejaVu Sans
Mono TTFs (`fonts-dejavu-core` on Debian/Ubuntu).

```bash
./tools/make_trix_logo.py --out assets/trix-logo            # regenerate the committed badge
./tools/make_trix_logo.py --dark --landscape --no-caption   # theme / shape / subtitle variants
./tools/make_trix_logo.py --accent '#8b5cf6'                # recolor the accent dot
```

## migrate_dollar_dollar.py / migrate_g_to_dollar.py

Historical one-off migrations from earlier syntax spellings, kept for the
record.  Both walk each file character-by-character, skipping line comments,
nestable block comments, text strings, and raw strings so they only rewrite
real code tokens.  They have already been applied across the tree; they remain
useful as references for how to write a syntax-aware source rewrite.
