<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Trix Code Style

This document describes the code style Trix enforces for all C++
source.  Architectural invariants (VM heap discipline, save/restore
semantics, sentinel conventions) live in
[docs/dev-invariants.md](docs/dev-invariants.md); the step-by-step
checklist for adding a new operator is in
[docs/dev-adding-operators.md](docs/dev-adding-operators.md).

---

## Formatting

Every `.cpp`, `.h`, and `.inl` file is formatted by `clang-format`
using `.clang-format` at the repo root.  Run before compiling or
committing:

```bash
clang-format -i --style=file <file>
```

CI enforces this; PRs with unformatted files will not pass.

**Blank line after top-of-function precondition asserts.**  When a
function opens with a precondition `assert(...)` -- or a block of
consecutive precondition asserts -- leave one blank line after the
last of them to separate the precondition guard from the function
body.  Applies to asserts at/near the top that check preconditions,
not to asserts interspersed mid-body.  clang-format does not insert
this; `tools/assert_blank_line.py` checks (`--check`, a CI gate) and
fixes (`--apply`) it.

---

## Braces and Parentheses

**Braces on every block.**  Every `if` / `else` / `for` / `while` / `do`
body is wrapped in `{ }`, even a single statement -- there are no braceless
bodies anywhere.  `readability-braces-around-statements` enforces this in CI.

**Parenthesize every compound sub-expression.**  Make operator precedence
explicit instead of leaning on the precedence table.  In a compound
expression, each operand that *itself* contains an operator -- logical
(`&&` / `||`), comparison (`==` `!=` `<` `>` `<=` `>=`), bitwise
(`&` `|` `^` `<<` `>>`), or arithmetic (`+` `-` `*` `/` `%`) -- is wrapped
in its own parentheses, at every nesting level; a `return` value carries an
outer wrap as well.

```cpp
if ((entry->m_target == target_offset) && ((entry->m_ref_id & Mask) != 0)) { ... }
return ((m_aat & SpecialFlag) != 0);
return (a + (b * c));
```

A same-precedence run of one operator stays flat -- write `a && b && c`,
not `((a && b) && c)`.  The only operand left bare is a lone primary with
no operator of its own: an identifier, member access, call, named cast,
unary expression, or an already-parenthesized group (`is_address()`,
`!is_free`, `s->m_raw_mode`).  It gets wrapped the instant it gains an
operator.

This is the intent of MISRA C 12.1 / CERT EXP00-C, and is stricter than the
compiler's `-Wparentheses` (which only warns on the ambiguous subset).  No
mainline clang-tidy check enforces the full rule; `tools/cpp_style.py`
checks and auto-fixes it (along with the `(void)` cast rule below).

---

## Control Flow

Prefer **a single top-to-bottom flow** through a function.  Priorities, in
order: (1) correctness, (2) performance, (3) one flow from top to bottom.

Do **not** leave a validity check and an early `return` that could be folded
into the flow.  An `if (cond) { ...; return; }` guard followed by more code in
the same scope is folded in -- invert it (`if (!cond) { <rest> }`), or make it
an `if (cond) {...} else {...}` when the guard does work; a cascade of such
guards becomes a nested `if / else if` chain.  Deeper nesting is fine; when it
gets *too* deep that is a signal to extract a helper function, not to fall back
on guard clauses.

The one exception is **performance**: a `return` (or `break`) that exits a
`for`/`while`/`do` loop early is a real win and stays.  A per-element *skip*
inside an iteration callback (e.g. a `gvm_for_each` lambda's
`if (is_free) return;`) is a `continue`, not an early-exit -- it inverts to
single-flow like any other guard (`if (!is_free) { ... }`).

`tools/find_guard_returns.py` locates the foldable guards (read each hit:
compound conditions need De Morgan, comments may need relocating).  This is a
deliberate reversal of an earlier guard-clause-flattening pass; see also
**Avoid `continue`** and **No `goto`** under Language Restrictions, which are
specific cases of the same single-flow preference.

---

## Const Correctness

Use `const` for locals (and parameters / member functions) that never
mutate.  **West const only** -- `const T`, `const T *`, `const auto`,
never east const (`T const`); `.clang-format` sets
`QualifierAlignment: Left`, so east const is normalized to west on format.

`misc-const-correctness` is enabled in `.clang-tidy` and runs in CI, so a
local that is never mutated must be declared `const`.  The lone exception
is `main()`'s `Trix trx`: a background `my-raise-interrupt` worker thread
mutates it through a `Trix*` captured inside the constructor, which the
check cannot see -- it is suppressed with a justified `// NOLINTNEXTLINE`.
Exclude the mutable `Trix *trx` world-handle from const-pointer conversions.

Leaf pointer parameters that are only read take `const T *` (e.g.
`scan_interpolation_escape_impl`'s `string_limit`, `vm_trim_alloc`'s `ptr`).
Member functions that do not mutate `*this` take a trailing `const`.

The one carve-out: a small handle struct (`Object`, `Dict`, `Stream`) is a
descriptor over VM memory, so a method may mutate the *pointee* through the
`Trix*` parameter while leaving the descriptor itself untouched.  Clang's
`readability-make-member-function-const` proves only that `*this` is
unmodified, so it flags such mutators -- `Object::update_long` (writes the
VM-resident value via the non-const `ExtValue*` from the `const extvalue()`),
the sibling `update_ulong/int128/uint128/double`, and `array_clear /
string_clear / dict_clear`.  These stay **non-const**: marking a named
mutator `const` would erase the reader/writer distinction it shares with its
`const` getter (`update_long` beside `long_value`).  That check is therefore
NOT enabled in CI; the matching read-only methods (`string_length`, `get`,
`expand_size`, `get_filepos`, ...) are `const` as a one-time judgement sweep.

---

## Language Standard

C++23.  Target compilers are GCC 15 and Clang 20.  Use modern
C++23 standard-library features (`std::expected`, `std::print`,
`std::span`, `std::ranges`, etc.) in preference to hand-rolled
equivalents.

---

## Warning Discipline

The project compiles with `-Werror` and an extensive warning set (the
principal flags are in [README.md](README.md) under *Compiler Discipline*).  Key
implications:

- `-Wconversion` -- no implicit narrowing.  Cast explicitly with
  `static_cast<>` where narrowing is intentional.
- `-Wswitch-enum` -- every enum case must be handled (or explicitly
  defaulted with a comment explaining why).
- `-Wshadow` -- no variable shadowing.  Rename inner scopes.
- `-Wduplicated-cond` / `-Wduplicated-branches` -- catches
  copy-paste bugs; respect them.

---

## Language Restrictions

**No preprocessor `#define` macros.**  Use `constexpr` variables,
`inline` functions, or templates instead.  The only sanctioned
preprocessor directives are `#include`, `#pragma`, and the four
feature-gate `#ifdef`s below.

**Compile-time feature gates are the one exception to both the macro/
preprocessor ban and the STL ban.**  There are four, in two families.
The matching `constexpr bool`s in `src/build_config.inl` (`Debugger`,
`HeapTracking`, `ZlibEnabled`, `ReadlineEnabled`) are for code that
compiles in BOTH builds -- prefer `if constexpr (...)` there, with no
preprocessor.  Reach for `#ifdef` only at declaration-/symbol-level
sites `if constexpr` cannot reach: a member field, an enum entry, a
dispatch switch case, an init-list string, or a body that references a
symbol absent from the gated-off build.  Use a plain `#ifdef` block, not
a function-like gate macro.

*Diagnostic gates* (opt-in) -- the interactive debugger (`TRIX_DEBUGGER`)
and vm-heap-tracking (`TRIX_HEAP_TRACKING`) -- compile their *whole*
surface out of release builds.  Inside such a gate, heap-allocating STL
(`std::string`, `std::unordered_map`, `std::vector`) is permitted,
because ops, member vars, Config fields, CLI flags, `--help` text, and
GC hooks all vanish, leaving zero residue and zero heap-allocating-STL
surface linked.  Gate the *whole* surface -- e.g. the `--inspect` family
in `api.inl` (enum, `longopts` rows, switch cases, help text) is gated
alongside `push_inspect_boot`, so a debugger-off build rejects
`--inspect` with "unrecognized option" rather than advertising a flag it
cannot honour -- not just the hot path.

*Dependency-trim gates* (opt-out) -- `TRIX_NO_ZLIB` and
`TRIX_NO_READLINE` -- drop an external library (zlib, GNU readline) for a
smaller embed.  They are ON by default; the `TRIX_NO_*` symbol opts out.
Unlike the diagnostic gates they **preserve the user-facing surface**:
the zlib-backed `deflate`/`inflate` operators stay registered and raise
`/unsupported` (callers catch it) rather than vanishing, and the
interactive REPL degrades to a plain `getline` prompt rather than
disappearing.  Gate only the external-symbol code -- the `#include`, the
library calls, the bodies that need the dropped header -- and keep the
operator / Name / CLI surface intact, so a program written for the full
build fails gracefully (`/unsupported`), not with `/undefined`.

Verify all build profiles -- `./build.sh` (all gates on), `./build.sh
--optimized` (diagnostic gates off), `./build.sh --no-zlib`, and
`./build.sh --no-readline` -- compile, link, and pass tests.  No
preprocessor conditionals beyond these four gates.

**No C-style casts.**  Use the named C++ casts (`static_cast`,
`reinterpret_cast`, `const_cast`).  Discard an unused value with
`static_cast<void>(expr)`, never `(void)expr`.  clang-tidy's
`cppcoreguidelines-pro-type-cstyle-cast` deliberately exempts the `(void)`
discard idiom, so `tools/cpp_style.py` enforces this one.

**No `alignas()` in Trix code.**  The VM allocator handles alignment;
over-aligning individual values breaks heap packing.

**No heap-allocating STL in operators.**  The rule is about *heap
allocation*, not specific type names: `std::vector`, `std::string`,
`std::unordered_map`, `std::set`, `std::function`, `std::deque` --
anything that allocates on the C/C++ heap -- is banned.  Use the VM
heap via the `vm_alloc_ptr` / `vm_end_alloc_ptr` / `vm_trim_alloc`
helpers instead, so all storage belongs to the VM heap and
save/restore and snapshot/thaw see it.  Non-allocating STL is fine
and used throughout: `std::string_view`, `std::span`, `std::array`,
`std::pair`/`std::tuple` of scalars, and the `<type_traits>` /
`<concepts>` machinery never touch the heap.

**Pointers, not references -- except for performance.**  Trix uses
pointer parameters throughout (e.g., `Trix *trx`); default to
pointers, not `&` references, in new code.  The exception is
performance: where a reference avoids copying a heavy / non-trivial
object or matters on a hot path -- a `const T &` parameter, or a
`const auto &` in a range-for over non-trivial elements -- a
reference is the right tool and is allowed.  Pointer/value stays the
default everywhere a copy is cheap (scalars, pointers, small
structs).  (Note the next rule still applies: a reference is not a
license for a hidden in/out parameter.)

**No in/out pointer parameters.**  Helpers that both read and
modify a value via a pointer hide mutation behind an innocuous-
looking call site.  Return a struct or tuple and let the caller
destructure instead.

**Avoid `continue`.**  Prefer inverting the skip-test (`if (!skip)
{ ... }`) or restructuring so the loop body reads top-to-bottom.  A
bare `continue` buries the real work beneath a guard and is easy to
miss when scanning; reach for it only when inverting would nest the
body too deeply to read.

**No `goto`.**  The control-flow equivalent of the `continue`
guidance, with no escape hatch -- restructure with a helper and an
early `return`, a boolean flag, or a small state machine instead.

---

## ASCII-Only Source

Use only printable ASCII (0x20-0x7E, plus `\n` and `\t`) in C++
source files -- `.cpp`, `.h`, and `.inl`.  No Unicode characters:
no em-dashes, curly quotes, arrows, box-drawing glyphs, math
symbols, or section signs.

This ban is for source code ONLY.  Markdown docs (`.md`) are exempt;
Unicode is fine there.

There are two narrow exceptions, both because the rule governs
*internal* source -- identifiers, comments, and non-output literals --
not bytes that get printed to a human:

1. The middle-dot bullet (U+00B7), used in `//===--- ... ---===//`
   file-header / section-banner art and as the cat-nose in the logo
   banner.
2. Unicode in **human-facing output** string literals -- notably the
   `--version` / `--about` banner, whose `... the cat in concatenative`
   line carries a U+2014 em-dash.  Do NOT rewrite these to ASCII.

Replacements (in source): `--` for em-dash; straight `"` and `'`
for curly quotes; `->` for arrows; `>=` and `<=` for relational
glyphs; `x` for the multiplication sign; `section` for the section
sign; spell out anything else (e.g. `approximately`).

---

## Naming Conventions

**Object variables** follow a universal pattern for any `Object` read
or copy, across stacks, heap, arrays, dicts, records, tagged pairs,
mailboxes, pipe buffers, etc.:

- `_ptr` suffix for pointers into live storage (`Object *`).  A
  write through `_ptr` updates the storage.
- `_obj` suffix for local copies or clones (`Object`).  Mutations are
  scoped to the local.

Prefix with a semantic role:  `proc_ptr`, `target_obj`, `val_ptr`,
`key_obj`, `elem_ptr`, `field_obj`, etc.  Generic `o`, `v`, `x` are
not acceptable for `Object` bindings.

**Local integers and enums** use descriptive names that reflect
their role (`count`, `index`, `save_level`), not single letters.

---

## Comments

Default to writing no comments.  Add one only when the **why** is
non-obvious: a hidden constraint, a subtle invariant, a workaround
for a specific bug, or behavior that would surprise a reader.

Do not explain what the code does -- well-named identifiers do that
job.  Do not reference the current task, fix, or callers ("used by
X", "added for the Y flow"); those belong in the PR description and
rot as the codebase evolves.

---

## Switch Over Indexed Dispatch

For dispatching on an enum (notably `SystemName`), prefer a `switch`
statement over an indexed array of function pointers.  Switch is
ordering-independent and `-Wswitch-enum` will flag missing cases.
Indexed arrays silently break when the enum is reordered.

---

## Sentinels

VM-heap offsets use `nulloffset` (0) as the null sentinel.  Raw
`Stream *` pointers still use `nullptr`.  Be explicit -- don't mix
the two conventions in a single structure.

---

## GC-Root Stack Idiom

When an operator builds a global composite Object across more than one
allocation, root the in-flight temporaries on the **gc-root stack** rather
than the old `if (m_curr_alloc_global) { /* op-stack push */ } ...` block.
The full contract — why mid-build GC can sweep an unrooted temporary, the
empty-at-boundary invariant, and the clone-into-slot rule — is in
[docs/dev-invariants.md](docs/dev-invariants.md).  The call-site idiom:

```cpp
trx->require_gc_root_capacity(3);             // INITIAL guard: asserts the stack is empty

*++trx->m_gc_roots_ptr = head_obj;     auto *head = trx->m_gc_roots_ptr;
*++trx->m_gc_roots_ptr = source_obj;   auto *r    = trx->m_gc_roots_ptr;  // rolling slot
*r = Object::make_curry_pair(trx, *r, EQ);    // *r stays rooted during the alloc, then re-stamp
*r = Object::make_lazy_thunk(trx, *r);
*trx->m_op_ptr = Object::make_lazy(trx, *head, *r);

trx->reset_gc_root(3);                        // tail: validates the count, clears to empty
```

Style points:

- **Push unconditionally** — no `if (global)` guard.  The roots are harmless
  when allocation is local; the branch was noise.
- **Use `require_gc_root_capacity(n)` (INITIAL) when rooting starts**; it
  debug-asserts the stack is empty.  Use `require_gc_root_capacity_more(n)`
  (PHASED) only for a second-or-later guard *within the same operator* — a
  branch that needs more roots on top of an already-rooted set.  Mixing them
  up trips the assert.
- **One rolling slot for chains/loops**, not one root per step.  Capture
  `auto *r = m_gc_roots_ptr` once and re-stamp `*r` as you fold; the prior
  `*r` stays rooted across each allocation.
- **Reset on every path that pushed** — `reset_gc_root(n)` with the exact
  push count (it asserts the match), or skip it entirely on an early-exit
  path that never pushed.
- **Breathing space:** one blank line after the capacity guard and one
  before the reset, so the rooted region reads as a unit.
- **Bounded builders only.**  Unbounded-recursive rooting (copy-term, deep
  walks) can exceed `MaxGcRootDepth` and stays on the operand stack.

## Questions

If a rule feels wrong for a particular change, open an issue with
the context.  The style is intentional but not sacred; exceptions
exist where justified.
