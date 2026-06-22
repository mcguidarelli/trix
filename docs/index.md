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

# Trix Documentation

Reference documentation for Trix, an embeddable stack-based scripting
VM written as a single-header C++23 library.

**New to Trix?** Take the [**Getting Started**](getting-started.md) tour --
it walks from zero through values, the stack, names, control flow,
collections, errors, and a first program.  Once the basics click, the
[**Cookbook**](cookbook.md) has ready-made recipes for common tasks.  (The
[project README](https://github.com/mcguidarelli/trix) has a 30-second taste.)

**Coming from PostScript?** [Trix for PostScript
Programmers](from-postscript.md) maps the operand stack, dictionary
stack, names, and `save`/`restore` you already know onto Trix -- and
shows what Trix adds.

**Jump to:** [Language reference](#language-reference) |
[Getting around the language](#language-features) |
[Concurrency](#concurrency) | [Computation](#computation) |
[Tooling](#tooling) | [VM internals](#vm-internals) |
[For contributors](#for-contributors)

---

## Language Reference

The one-stop reference for every operator, type, stack effect, and
syntactic form.  When in doubt, search this first.

- [**Trix Reference**](trix-reference.md) -- complete operator
  catalog (820+), stack effects, types, error names, test-writing
  patterns, scanner syntax.
- [**Operator Cheat Sheet**](operator-cheatsheet.md) -- the
  everyday subset (~300 ops) in one printable table, grouped by
  category.  Pairs with the full reference for at-a-glance lookup.
- [**Errors Cheat Sheet**](errors-cheatsheet.md) -- every named
  error (58) with when it fires, who raises it, and the
  idiomatic `try` / `try-catch` recovery pattern.  Also includes
  the process-exit-code mapping for shell-script integration.
- [**Format String Cheat Sheet**](format-cheatsheet.md) --
  Python-style format specs accepted by `print-fmt`, the
  `sscan-fmt`/`fscan-fmt` scan family, and string interpolation:
  alignment, width, precision, sign,
  alt form, type letters (including Trix-specific `T` / `O` / `I`
  chrono), full op family with stack effects, common recipes.
- [**Type Cheat Sheet**](types-cheatsheet.md) -- all 30 user-visible
  types, the empirically-verified promotion lattice for `$[ ... ]`
  infix (signed/unsigned/precision rules with edge cases), strict-
  typing semantics for bare ops, `eq` cross-type behavior, type
  predicate idioms, and the conversion-op vocabulary.
- [**Getting Started**](getting-started.md) -- a from-zero tour of the
  language: values, the stack, names, control flow, collections, errors,
  and a first program.
- [**From PostScript**](from-postscript.md) -- orientation for
  PostScript programmers: the operand stack, dict stack, names, and
  `save`/`restore` you know, what is renamed, and what Trix adds.
- [**Cookbook**](cookbook.md) -- task-oriented "how do I..." recipes:
  parse text, transform collections, handle errors, read files, model
  state, run a coroutine, and more -- each a short, runnable block.

---

## Language Features

How the language's building blocks work end-to-end.

### Core
- [Type System](type-system.md) -- 8-byte tagged union, 32 types (30 user-visible)
- [Scanner](scanner.md) and [Scanner Syntax](scanner-syntax.md) --
  lexer, tokenization, binary tokens
- [Infix Design](infix-design.md) -- `$(...)` infix arithmetic
- [Name Lookup](name-lookup.md) -- path resolution, hierarchy
- [Error Handling](error-handling.md) -- try/catch, handler dicts
- [IEEE 754](ieee754.md) -- floating-point semantics, NaN/Inf
- [String Processing](string-processing.md) -- UTF-8, scan/format
- [Dates and Times](chrono.md) -- instants, calendar dates,
  formatting and parsing
- [Collections](collections.md) -- Array, Dict, Set

### Functional and Higher-Order
- [Functional Programming](functional-programming.md) -- map,
  filter, fold, curry
- [Lazy Sequences](lazy-sequences.md) -- infinite streams,
  transformers
- [Closures](closures.md) -- captured environments
- [Curry / Thunk](curry-thunk.md) -- partial application, deferred
  evaluation
- [Transducers](transducers.md) -- composable data transformations
- [Pattern Matching](pattern-matching.md) -- let, destructure, match
- [Protocols](protocols.md) -- open type dispatch
- [Contracts](contracts.md) -- precondition / postcondition checking

### Data
- [Tagged Values](tagged-values.md) -- discriminated unions, ADTs
- [Records](record.md) -- immutable named-field composites
- [Binary Pack](binary-pack.md) -- structured binary data:
  pack/unpack values to/from byte strings
- [Modules](modules.md) -- scoped namespaces, require/use/import
- [Streams and I/O](streams-io.md) -- stdin/stdout, files, memory
  streams, zlib deflate/inflate
- [Terminal I/O](terminal-io.md) -- raw key input, timeouts,
  screen drawing
- [Host Integration](host-integration.md) -- C++ API, custom
  operators
- [Trix System](trix-system.md) -- system-architecture overview:
  how all subsystems compose into one programming system

---

## Concurrency

The 5-layer durable process runtime: the differentiating feature
that sets Trix apart from Lua and other embeddable scripting VMs.

- [Coroutines](coroutines.md) -- cooperative scheduling, yield, join
- [Pipelines](pipeline.md) -- bounded buffers, backpressure (see
  also [Pipeline internals](pipeline-internals.md))
- [Actors](actors.md) -- mailboxes, send/recv, selective receive
- [Supervision](supervision.md) -- monitors, links, restart
  strategies (Erlang/OTP style)
- [GenServer](genserver.md) -- request/reply server pattern on top
  of actors
- [Continuations](continuation.md) -- delimited continuations
  (the substrate for effects)
- [Algebraic Effects](effects.md) -- perform/handle, effect
  handlers built on delimited continuations

---

## Computation

- [Logic](logic.md) -- unification, backtracking, choice points,
  cut
- [Reactive Cells](reactive.md) -- incremental computation,
  watchers, dependency tracking
- [Save/Restore](save-restore.md) -- transactional journaling
  (the foundation for logic backtracking)
- [Local and Global VM](local-global-vm.md) -- values vs
  containers, `${...}`, what survives restore
- [Snapshot/Thaw](snapshot-thaw.md) -- whole-VM serialization
- [Tail Call Optimization](tail-call-optimization.md) -- TCO
  mechanics

---

## Tooling

- [**Running Trix**](cli.md) -- command-line reference: every flag,
  the five startup modes, module search path, `--sandbox`, the
  snapshot workflow, exit codes
- [**Interactive Debugger**](debugger.md) -- `./trix --inspect FILE`
  five-pane TUI: step, breakpoints (plain / conditional / one-shot),
  watches, sandboxed expression eval, frame navigation, stack
  introspection.  Implemented in Trix itself
  (`lib/debugger.trx`, ~1880 LOC) on top of a thin layer of C++
  intrinsics; conditionally compiled out of release builds via
  `TRIX_DEBUGGER`.
- [**DWARF Host Introspection**](dwarf.md) -- scriptable, type-aware
  introspection of the host C++ program Trix is embedded in: resolve a
  global or function by name to its address, a type to its layout, and read
  a global's live value as a typed Trix Record.  Reads the host's own
  ELF/DWARF (v2-v5, 32/64-bit, both byte orders) from a Trix library
  (`lib/dwarf.trx`) on eight C++ primitives; Linux/ELF64 only, opt-out via
  `TRIX_NO_DWARF`.  The manual doubles as a reader's primer on the DWARF
  format for maintainers.

---

## VM Internals

For people implementing custom operators, debugging the VM, or
porting Trix.

- [**Architecture Overview**](dev-architecture.md) -- the whole-engine
  map and "you are here": source layout, the execution pipeline, the
  subsystem-to-file table, and the core data structures.  Start here,
  then dive into the specifics below.
- [VM Internals](vm-internals.md) -- heap layout, stacks, object
  system, allocation (local bump arena)
- [Global VM allocator + GC](gvm-heap-gc.md) -- the global VM region, its
  dlmalloc-style allocator, mark-sweep garbage collection, and the
  `${...}` / `set-global` user surface
- [VM Memory Regions](vm-regions.md) -- local/global/temp invariants,
  the temp-clobber rule, region-aware results, GC-rooting discipline
  (for maintainers)
- [Interpreter](interpreter.md) -- the main dispatch loop

---

## For Contributors

- [**Architecture Overview**](dev-architecture.md) -- orientation for
  new maintainers: how the source is organised and how a program flows
  from text to result
- [Adding Operators](dev-adding-operators.md) -- step-by-step
  checklist for landing a new operator
- [Control Operators and the Coroutine Trampoline](dev-control-operators.md)
  -- the `@`-control-op mechanism behind every blocking, yielding, or
  resumable operator
- [Testing and Validation](dev-testing.md) -- philosophy, every suite
  and harness, box taxonomy, fault injection, fuzzing, coverage
- [Invariants](dev-invariants.md) -- critical rules the runtime
  depends on
- [Glossary](dev-glossary.md) -- terminology reference
- [Fuzzing harness](../fuzz/README.md) -- libFuzzer target, corpus,
  crash triage workflow
- [Benchmarks](../benchmark/README.md) -- the hash/dispatch
  micro-benchmark harness

See also [`CONTRIBUTING.md`](../CONTRIBUTING.md) and
[`STYLE.md`](../STYLE.md) at the repo root.
