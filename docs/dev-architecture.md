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

# Architecture Overview

A map of the whole engine for new maintainers: how the source is organised,
how a program flows from text to result, where each subsystem lives, and the
handful of data structures everything else is built on.  Read this first, then
follow the cross-links into the deep-dive docs for any subsystem you need to
change.

**Jump to:** [Big picture](#1-the-big-picture) |
[Execution pipeline](#2-the-execution-pipeline) |
[Subsystem map](#3-subsystem-map) | [Core data structures](#4-core-data-structures) |
[Control operators](#5-the-control-operator-mechanism) |
[Navigating the source](#6-navigating-the-source) | [Where to read next](#7-where-to-read-next)

---

## 1. The big picture

Trix is a single-header C++23 library.  The entire engine is **one class**,
`class Trix`, whose body is assembled from source fragments.  `trix.h` opens the
class and `#include`s 64 `src/*.inl` fragments directly (four more --
`scanner.inl`, `scanner_infix.inl`, `binary_tokens.inl`, and `ops_debugger.inl`
-- are pulled in transitively, for 68 fragments in all), then closes the class.
Immediately after the closing brace, three compile-time `static_assert`s fire
-- `verify_chattr()` (scanner char-class table), `Number::verify_hint_table()`,
and `verify_dispatch_tables()` -- the last proving the operator dispatch tables
are complete.

```text
trix.h
  +-- (preamble, type aliases, class Trix {  ... )
  |
  +-- #include "src/build_config.inl"     // feature gates
  +-- #include "src/types.inl"            // constants, enums, sizes
  +-- #include "src/object.inl"           // the 8-byte Object
  +-- ... 60 more fragments ...           // scanner, heap, dict, ops_*, ...
  +-- #include "src/member_vars.inl"      // instance fields (LAST)
  |
  +-- };                                  // class Trix closes
  +-- static_assert(verify_dispatch_tables(), ...);
```

Why one class?  Every fragment is member code, so any operator implementation
can touch any VM field (the operand stack, the heap pointers, the dictionary
stack) without crossing a module boundary or threading a context pointer
through call sites.  The cost -- a single large translation unit -- is paid once
at build time and bought back as a tiny, dependency-light artifact.

`member_vars.inl` comes **last** because it declares the instance fields, and
those declarations name types (Object, Dict, Stream, the stacks) that the
earlier fragments define.  Its hot inner-loop pointers (`m_op_ptr`,
`m_exec_ptr`, `m_exec_base`, `m_dict_ptr`) are deliberately grouped into the
first cache line.

**Dependencies.** The engine needs only the C++23 standard library plus two
optional system libraries: **zlib** (deflate/inflate) and **libreadline** (the
REPL line editor).  Both are compile-time-gated -- build with `-DTRIX_NO_ZLIB`
or `-DTRIX_NO_READLINE` to drop them (see [§6](#6-navigating-the-source) and
[BUILDING.md](../BUILDING.md)).

---

## 2. The execution pipeline

Trix follows the PostScript execution model.  There is no bytecode and no
program-counter register: **the execution stack *is* the program counter.**
Each entry on the exec stack is an Object waiting to run; the interpreter pops
one per iteration, dispatches it by type, and repeats.

```text
  source text / REPL line / file
            |
            v
   +------------------+      tokens        +-----------------------+
   |     Scanner      | -----------------> |   Execution stack     |
   | scanner.inl      |   (one at a time)  |  (the program counter) |
   | scanner_tables   |                    +-----------------------+
   | scanner_infix    |                               |
   | binary_tokens    |                               | pop top, dispatch by type
   +------------------+                               v
                                            +-----------------------+
                                            |  Interpreter loop     |
                                            |  interpreter.inl      |
                                            +-----------------------+
                                              |          |
                       literal -> push        |          | operator / name / proc
                       to operand stack       v          v
                                   +-----------+   +--------------------+
                                   |  Operand  |   |  Dispatch          |
                                   |  stack    |<--|  dispatch.inl ->   |
                                   +-----------+   |  ops_*.inl (C++)   |
                                         ^         +--------------------+
                                         |                  |
                                         +------------------+
                                            results, via the VM heap
                                            (vm_heap / gvm_heap / gc)
```

The interpreter dispatches each Object along one of **ten executable paths**,
keyed by its type tag:

| Object type            | What the interpreter does                             |
| ---------------------- | ----------------------------------------------------- |
| Literal / ignores-exec | push to the operand stack (it is data)                |
| Null                   | no-op (silently ignored)                              |
| Operator               | call the native C++ function                          |
| Name                   | look up in the dictionary stack, execute the result   |
| Array / Packed         | execute as a procedure (pop head, push the tail back) |
| String                 | scan one token from it, execute that                  |
| Stream                 | scan one token from the stream, execute that          |
| Curry                  | push the captured value, then execute the callable    |
| Thunk                  | force evaluation, push the cached result              |
| Continuation           | restore a captured exec/error stack segment           |

Because control flow is just exec-stack manipulation, `if` pushes a procedure,
`loop` pushes a procedure plus a continuation marker, and `stop` unwinds to a
marker -- no special interpreter cases.  Executable **names** resolve through a
three-level lookup (a per-call binding cache, then `//:dict:key` path search,
then a top-to-bottom dictionary-stack walk), and tail calls in the last
position of a procedure reuse the current `@call` frame so tail recursion runs
in constant space.  See [Interpreter](interpreter.md),
[Name Lookup](name-lookup.md), and
[Tail Call Optimization](tail-call-optimization.md) for the full mechanics.

---

## 3. Subsystem map

Where each area lives in `src/` and which doc covers it in depth.

| Area | Key fragments | Deep-dive doc(s) |
| --- | --- | --- |
| Build / feature gates | `build_config.inl` | [BUILDING.md](../BUILDING.md), [STYLE.md](../STYLE.md) |
| Constants and enums | `types.inl`, `enums.inl`, `op_descriptor.inl` | [Type System](type-system.md) |
| Scanning / tokenizing | `scanner.inl`, `scanner_tables.inl`, `scanner_infix.inl`, `binary_tokens.inl` | [Scanner](scanner.md), [Scanner Syntax](scanner-syntax.md), [Infix Design](infix-design.md) |
| The Object and verification | `object.inl`, `verify.inl`, `number.inl` | [Type System](type-system.md) |
| VM heap and GC | `vm_heap.inl`, `gvm_heap.inl`, `gc.inl` | [VM Internals](vm-internals.md), [Global Heap / GC](gvm-heap-gc.md), [VM Regions](vm-regions.md) |
| Names, binding, dictionaries | `name.inl`, `binding_table.inl`, `dict.inl` | [Name Lookup](name-lookup.md) |
| Streams and I/O | `stream.inl`, `ops_stream_io.inl`, `ops_screen.inl` | [Streams and I/O](streams-io.md), [Terminal I/O](terminal-io.md) |
| Save / restore, snapshot | `save.inl`, `snapshot.inl`, `ops_snapshot.inl` | [Save/Restore](save-restore.md), [Snapshot/Thaw](snapshot-thaw.md) |
| Formatting and chrono | `printfmt.inl`, `scanfmt.inl`, `chrono.inl` | [Format Cheat Sheet](format-cheatsheet.md), [Dates and Times](chrono.md) |
| Interpreter and runtime | `runtime.inl`, `interpreter.inl`, `shims.inl` | [Interpreter](interpreter.md) |
| Operator dispatch | `dispatch.inl` | [Interpreter](interpreter.md), [Adding Operators](dev-adding-operators.md) |
| Operator implementations | the `ops_*.inl` family (see below) | the feature docs in the [index](index.md) |
| Interactive debugger | `ops_debugger.inl` | [Debugger](debugger.md) |
| Host embedding and startup | `api.inl`, `init.inl`, `member_vars.inl` | [Host Integration](host-integration.md), [Running Trix](cli.md) |

The operator implementations are split by feature family across the `ops_*.inl`
fragments: core data (`ops_stack`, `ops_array`, `ops_array_iteration`,
`ops_string`, `ops_dict`, `ops_set`, `ops_record`, `ops_tagged`), computation
(`ops_math`, `ops_bitwise`, `ops_convert`, `ops_regex`, `ops_match`),
control and higher-order (`ops_flow`, `ops_higher`, `ops_lazy`,
`ops_transducer`, `ops_continuation`, `ops_effect`, `ops_protocol`), the
concurrency runtime (`ops_coroutine`, `ops_scratch`, `ops_pipeline`,
`ops_actor`, `ops_supervision`, `ops_genserver`, `ops_logic`, `ops_reactive`),
and the rest (`ops_format`, `ops_chrono`, `ops_stream_io`, `ops_screen`,
`ops_memory`, `ops_snapshot`, `ops_system`).  To add an operator, follow
[Adding Operators](dev-adding-operators.md).

---

## 4. Core data structures

### 4.1 The Object

Everything a Trix program manipulates is an **Object**: a value type exactly
**8 bytes** wide (`static_assert(sizeof(Object) == 8)`).  One of those bytes is
the *attribute byte*, a tagged-union discriminator:

```text
  7 6 5 4 3 2 1 0
 +-+-+-+-+-+-+-+-+
 |X|W|F|T T T T T|
 +-+-+-+-+-+-+-+-+
  | | |  \_____/
  | | |     +----- T: 5-bit type tag (31 types: Null=0 .. OpaqueHandle=30)
  | | +----------- F: per-type flag (meaning depends on T)
  | +------------- W: 0 = ReadOnly, 1 = ReadWrite
  +--------------- X: 0 = Literal,  1 = Executable
```

The 5-bit `T` field names 31 types (30 user-visible; `SourceLoc` is internal).
The remaining seven bytes hold the value: an immediate scalar (Integer, Real,
Boolean, ...) inline, or a VM offset for a composite (Array, String, Dict,
Record, ...) that lives in the heap.  The `W` and `X` bits are the VM-enforced
access and executability controls -- the same `#r` / executable-name semantics
visible at the language level.  The `F` bit is polymorphic: it carries the
Boolean value for Booleans, an address-cache state for Addresses, and the
"eqref" marker for the shared `#=` storage buffers.  See
[Type System](type-system.md) and [VM Internals](vm-internals.md).

### 4.2 The stacks

The VM runs on four principal stacks, each an array of Objects, plus a
per-coroutine scratch stack:

| Stack      | Role                                                     | Default depth |
| ---------- | -------------------------------------------------------- | ------------- |
| Operand    | the data stack operators push and pop                    | 1024          |
| Execution  | the program counter -- Objects waiting to run            | 2048          |
| Dictionary | the name-resolution scope chain (`begin` / `end`, `use`) | 64            |
| Error      | the handler / `$error` machinery for `try` and `throw`   | 64            |
| Scratch    | per-coroutine working space (named/local args, `let`)    | 128           |

The operand and execution pointers (`m_op_ptr`, `m_exec_ptr`, `m_exec_base`)
plus the dictionary-stack top (`m_dict_ptr`) share the first cache line because
the inner loop touches them on every iteration.

### 4.3 Names and dictionaries

A **Name** is an interned reference; a **Dict** is a hash table of name ->
Object.  Name resolution walks the dictionary stack top-to-bottom, short-cut by
a per-call-site binding cache.  System operators live in `systemdict` at the
bottom of the stack; `protocoldict` (protocol-dispatch procs, filled by
`def-protocol`) sits above it, then `userdict`; `begin` / `use` push further
scopes on top.  See [Name Lookup](name-lookup.md) and [Collections](collections.md).

### 4.4 The VM heap

All composite Objects live in a single `malloc`'d block, partitioned into a
**main region** (permanent allocations, grown by a bump pointer), a **temp
region** (short-lived scratch, grown downward from the global region's edge),
and a **global region** with its own `dlmalloc`-style allocator and mark-sweep
garbage collector.  Local-region allocation is O(1) and reclaimed wholesale;
only the global region is GC'd.  The region invariants -- and the temp-clobber
rule maintainers must respect -- are in [VM Internals](vm-internals.md),
[Global Heap / GC](gvm-heap-gc.md), and [VM Regions](vm-regions.md).

---

## 5. The control-operator mechanism

The interpreter loop is plain C++ that cannot itself block, yield, or suspend.
Any operator that needs to -- `coroutine-sleep`, an actor receive, a pipeline
read, the `try` / `with-stream` cleanup -- is implemented as a **control
operator**: an `@`-prefixed internal Operator that the op pushes onto the
execution stack to schedule its own continuation, then returns to the loop.
When the loop re-enters the control operator, the second stage resumes the
work.  This trampoline is the load-bearing pattern behind every blocking or
resumable feature, and it is why control flow lives on the exec stack rather
than in C++ recursion.  The full mechanism -- the control-op enum range,
`make_control_operator`, re-entrant state, and a worked trace -- is covered in
[Control Operators and the Coroutine Trampoline](dev-control-operators.md).

---

## 6. Navigating the source

**The `.inl` include pattern.** Each `src/*.inl` is a fragment of `class Trix`,
not a standalone header -- it has no include guard of its own and is only valid
`#include`d at the right point inside the class body in `trix.h`.  Edit the
fragment, not `trix.h`, when changing a subsystem; touch `trix.h` only to add or
reorder a fragment.

**Feature gates (`build_config.inl`).** Five compile-time macros gate optional
code, in two families:

| Macro                | Default | Effect when set                                                    |
| -------------------- | ------- | ------------------------------------------------------------------ |
| `TRIX_DEBUGGER`      | off     | compile *in* the interactive debugger ops (`ops_debugger.inl`)     |
| `TRIX_HEAP_TRACKING` | off     | compile *in* heap-accounting instrumentation                       |
| `TRIX_NO_BACKTRACE`  | unset   | compile *out* `format_backtrace` (`BacktraceEnabled`)              |
| `TRIX_NO_ZLIB`       | unset   | drop the zlib dependency; deflate/inflate ops raise `/unsupported` |
| `TRIX_NO_READLINE`   | unset   | drop libreadline; the REPL falls back to a plain prompt            |

`TRIX_DEBUGGER` and `TRIX_HEAP_TRACKING` are **opt-in** diagnostics -- off by
default; set the macro to compile the feature *in*.  The three `TRIX_NO_*`
macros are **opt-out** trims -- the feature is on by default; set the macro to
drop it.  `TRIX_NO_ZLIB` and `TRIX_NO_READLINE` are *dependency* trims that
keep the operator surface but raise `/unsupported` (zlib) or degrade gracefully
(readline); `TRIX_NO_BACKTRACE` simply omits backtrace capture.  `STYLE.md`
documents the convention for adding a new gate.

**Naming conventions.** Two suffixes carry load-bearing meaning throughout the
source: `_ptr` is a pointer **into live VM storage** (a write through it mutates
the heap), and `_obj` is a **local copy or clone** (mutations are private until
written back).  Both take a semantic role prefix -- `proc_ptr`, `key_obj`,
`elem_ptr`, `field_obj`.  See [STYLE.md](../STYLE.md) and the
[Glossary](dev-glossary.md).

**The dispatch tables.** `dispatch.inl` builds three tables (system-variable
names, well-known names, and system-operator function entries) at *constant
evaluation* time, one row per enum index.  `verify_dispatch_tables()` -- the
`static_assert` right after `class Trix` closes -- proves by pigeonhole that
every enum slot is filled exactly once, so a missing or duplicated operator
registration is a build error, not a runtime surprise.

---

## 7. Where to read next

- [Interpreter](interpreter.md) -- the dispatch loop in full, with the
  `@call` frame and the control-operator trampoline.
- [Type System](type-system.md) and [VM Internals](vm-internals.md) -- the
  Object, the heap, and the stacks in depth.
- [Global Heap / GC](gvm-heap-gc.md) and [VM Regions](vm-regions.md) -- the
  global allocator, garbage collection, and the region invariants.
- [Name Lookup](name-lookup.md) -- resolution, the binding cache, path syntax.
- [Adding Operators](dev-adding-operators.md) -- the step-by-step checklist for
  landing a new operator.
- [Invariants](dev-invariants.md) and [Glossary](dev-glossary.md) -- the rules
  the runtime depends on, and the terminology.
- [Testing and Validation](dev-testing.md) -- the suites, harnesses, and how to
  verify a change.
