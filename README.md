# Trix

```
  ______    _
 /_  __/___(_)_  __
  / / / __/ /\ \/ /       Stack-Based Interpreter & VM
 / / / / / /  > · <      C++23 · Single-Header Library
/_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli
```

**Embeddable stack-based scripting VM with cooperative concurrency and fault tolerance.**

[![CI](https://github.com/mcguidarelli/trix/actions/workflows/ci.yml/badge.svg)](https://github.com/mcguidarelli/trix/actions/workflows/ci.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Version](https://img.shields.io/badge/version-0.9.0-green.svg)](https://github.com/mcguidarelli/trix/releases)

C++23 | Single-Header | ASan/UBSan clean | Apache 2.0

```trix
3 4 add =                        % 7
/square { dup mul } def
5 square =                       % 25
[1 2 3] { square } map ==        % [1i 4i 9i]
```

## Why Trix?

Lua gives you embeddable scripting but no concurrency beyond
coroutines.  Erlang/BEAM gives you actors and supervision but won't
fit in your binary.  Trix sits in the gap: an actor/supervision
model as a header-only C++23 library, deterministic memory on the
local arena (no GC there -- save/restore reclaims it) backed by a
durable global region with a precise mark-sweep GC, and a serializable
whole-VM state.

And the name?  It's a nod to the author's cat: the `(_)` eye and `>·<`
whiskered nose in the logo are an homage to her.  Trixie is the cat
in concatenative.

## The Language

Trix is a concatenative (PostScript/Forth-style) stack language: values go on the stack,
operators consume them.  It is implemented as a single-header C++23 library
(68 `.inl` files assembled by `trix.h`).

Output conventions in the examples below: `=` pops and prints a value
(shows the type name for composites like arrays), while `==` pretty-prints
the full structure.  The `i` suffix on printed integers (e.g., `1i`) is
the numeric-type tag -- Trix distinguishes `Integer`, `UInteger`, `Long`,
`ULong`, and others in the tagged-union value representation.

### Basics

```trix
% Variables and procs
/double { 2 mul } def
[1 2 3] { double } map ==        % [2i 4i 6i]

% Recursion
/factorial { dup 1 le { pop 1 } { dup 1 sub factorial mul } if-else } def
10 factorial =                    % 3628800

% Strings with interpolation
/name (world) def
(hello \{name}!) =               % hello world!

% Infix math
$(3 + 4 * 2) =                   % 11

% Dicts
<< /name (Alice) /age 30 >>
/name get =                       % Alice
```

### Lazy sequences

Infinite data structures with streaming evaluation -- nothing materializes
until you ask for it:

```trix
% Infinite sequence: generate all even squares, take the first 5
1 lazy-from { dup mul } lazy-map
{ 2 mod 0 eq } lazy-filter
5 lazy-take lazy-to-array ==      % [4i 16i 36i 64i 100i]
```

### Reactive cells

Spreadsheet-style incremental computation -- derived values recompute
automatically when their dependencies change:

```trix
/width 10 cell def
/doubled { width cell-get 2 mul } cell-computed def
doubled cell-get =                % 20
15 width cell-set
doubled cell-get =                % 30 (recomputed automatically)
```

### Actors and supervision

Isolated processes with mailboxes, supervised under Erlang/OTP-style
restart policies.  This worker prints messages, throws on `/crash`,
and is restarted by its supervisor:

```trix
<<
  /strategy /one-for-one
  /intensity 5  /period 1000
  /children [
    << /id /printer
       /start  { { actor-recv dup /crash eq { /crash throw } if (got: ) print = } loop }
       /restart /permanent >>
  ]
>> supervisor /sup exch def

sup /printer supervisor-get-child pop /w exch def
(hello) w actor-send  100 coroutine-sleep         % => got: hello
/crash  w actor-send  200 coroutine-sleep         % worker dies; supervisor restarts
sup /printer supervisor-get-child pop /w exch def % handle for the restarted actor
(after-restart) w actor-send  100 coroutine-sleep % => got: after-restart
```

### Logic programming

Prolog-style unification and backtracking, built on the save/restore
transaction mechanism:

```trix
/x logic-var def
[
  { x 1 unify guard  x deref 2 mod 0 eq guard }
  { x 2 unify guard  x deref 2 mod 0 eq guard }
  { x 3 unify guard  x deref 2 mod 0 eq guard }
  { x 4 unify guard  x deref 2 mod 0 eq guard }
] choice
x deref =                         % 2 (first even number)
```

## What It Does

Trix provides cooperative concurrency primitives -- coroutines, pipelines,
actors, and supervision trees -- without threads or external runtimes.

The local execution arena is GC-free: `save` creates a checkpoint and
`restore` rolls back to it, reclaiming everything deterministically.  The
durable global region uses a precise mark-sweep GC.  The entire VM state
can be serialized to disk and resumed later (snap-shot/thaw).

| Capability         | Description                                           |
| ------------------ | ----------------------------------------------------- |
| **Coroutines**     | Cooperative scheduling with sleep, yield, join        |
| **Pipelines**      | Bounded-buffer pipelines with automatic backpressure  |
| **Actors**         | Isolated message-passing processes with mailboxes     |
| **Supervision**    | Erlang/OTP-style restart policies and fault isolation |
| **Logic**          | Prolog-style unification and backtracking             |
| **Reactive cells** | Spreadsheet-style incremental computation             |
| **Lazy sequences** | Infinite streams with deferred evaluation             |
| **Durability**     | Serialize entire VM state to disk and resume          |

A **source-level interactive debugger** ships with Trix: `./trix --inspect
FILE` opens a terminal TUI for single-stepping, plain/conditional/one-shot
breakpoints, watch expressions, and live inspection of every VM stack, plus a
sandboxed `:p` prompt for evaluating Trix at the halt point.  Its entire
user-facing UI is written in Trix itself
([`lib/debugger.trx`](lib/debugger.trx), ~1900 lines) over a thin layer of C++
intrinsics -- a working demonstration of the language.  See
[docs/debugger.md](docs/debugger.md).

## Example Programs

Two dozen example programs in [examples/](examples/README.md) showcase the
language end to end -- from logic solvers and reactive models to full
emulators and interpreters:

| Program | Demonstrates |
| --- | --- |
| [zmachine.trx](examples/zmachine.trx) | Full Infocom Z-machine (V1-V8) -- plays Zork and 50+ interactive-fiction titles |
| [mini-scheme.trx](examples/mini-scheme.trx) | Metacircular Scheme: CEK machine, first-class continuations, `call/cc` |
| [tetrix.trx](examples/tetrix.trx) | Playable terminal Tetris -- SRS rotation, T-spins, lookahead AI |
| [chip8.trx](examples/chip8.trx) | CHIP-8 / Super-CHIP emulator running 1977 ROMs |
| [raycaster.trx](examples/raycaster.trx) | First-person 3D dungeon via DDA ray casting |
| [regex.trx](examples/regex.trx) | Thompson-NFA + Pike-VM regex engine with a ReDoS head-to-head |
| [minidb.trx](examples/minidb.trx) | In-memory SQL: records-as-schemas, `save`/`restore` transactions |
| [symdiff.trx](examples/symdiff.trx) | Symbolic differentiator -- tagged-AST rewrite to a fixpoint |
| [sudoku.trx](examples/sudoku.trx) | Logic solver -- `unify` / `choice` / backtracking with automatic rollback |
| [village.trx](examples/village.trx) | Actors + supervision + reactive cells, snapshotted and resumed in forked processes |
| [effects_mini_scheme.trx](examples/effects_mini_scheme.trx) | Algebraic effects -- swap evaluator semantics by stacking handlers |
| [amazing.trx](examples/amazing.trx) | Pure-Trix PNG encoder + ten-algorithm maze zoo |

See [examples/README.md](examples/README.md) for the full catalog and run
instructions.

## Architecture

```
Concurrency                          Computation
+---------------------------------+  +---------------------------+
| Layer 3: Supervision            |  | Reactive Cells            |
|   monitor, link, restart trees  |  |   incremental recompute   |
+---------------------------------+  +---------------------------+
| Layer 2: Actors                 |  | Logic / Backtracking      |
|   mailboxes, send/recv, match   |  |   unify, choice, guard    |
+---------------------------------+  +---------------------------+
| Layer 1: Pipelines              |
|   bounded buffers, backpressure |
+---------------------------------+
| Coroutines                      |
|   cooperative scheduling        |
+---------------------------------+
| Snap-shot / Thaw (durability)   |
|   serialize entire VM to bytes  |
+---------------------------------+
```

The concurrency stack is the core of the project: an embeddable
actor/supervision model in the style of Erlang/OTP.  The computation
side adds Prolog-style constraint solving and reactive dataflow, both
useful in scripting and difficult to add retroactively.

All scheduling is cooperative and single-threaded.  There is no
preemptive multitasking.

## Quick Start

### Prebuilt binary (Linux x86_64)

```bash
curl -LO https://github.com/mcguidarelli/trix/releases/latest/download/trix-linux-x86_64.tar.gz
curl -LO https://github.com/mcguidarelli/trix/releases/latest/download/trix-linux-x86_64.sha256
sha256sum -c trix-linux-x86_64.sha256
tar xzf trix-linux-x86_64.tar.gz
cd trix-v*-linux-x86_64
./trix --version
```

Every release also ships a versioned tarball (`trix-vX.Y.Z-linux-x86_64.tar.gz`) for archival; the unversioned name above always points to the latest release.  See the [releases page](https://github.com/mcguidarelli/trix/releases) for older versions and full source snapshots.

Runtime dependencies: `libreadline` and `zlib` (both optional — a
`--no-readline` / `--no-zlib` build drops them).  On Ubuntu/Debian:
`sudo apt-get install libreadline8 zlib1g`.

### Build from source

```bash
git clone https://github.com/mcguidarelli/trix.git
cd trix
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/trix                 # interactive REPL
./build/trix script.trx      # run a script
./build.sh && ./runtests.sh  # quick rebuild at repo root + run the test suite
```

Requires: C++23 compiler (gcc-15 or clang-18+), libreadline-dev, zlib1g-dev, cmake 3.25+
(readline and zlib are optional — see `--no-readline` / `--no-zlib` in BUILDING.md).

See [BUILDING.md](BUILDING.md) for the full build reference -- `build.sh` modes,
CMake options, the build directories, and testing/fuzzing/benchmarking.

**Platforms:** Linux (tested, gcc-15 and clang-18).  macOS is expected
to work (POSIX-based, no platform-specific code) but is not regularly
tested.  Windows is not currently supported.

CMake options:

| Option            | Default | Effect                                     |
| ----------------- | ------- | ------------------------------------------ |
| `TRIX_SANITIZERS` | ON      | Enable ASan + UBSan                        |
| `TRIX_WERROR`     | ON      | Treat warnings as errors                   |
| `TRIX_FUZZ`       | OFF     | Build libFuzzer harnesses (requires clang) |

Release build (no sanitizers, optimized):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DTRIX_SANITIZERS=OFF
cmake --build build
```

### Running Trix

```bash
trix                       # interactive REPL (no filename)
trix script.trx            # run a script, then exit
trix script.trx a b c      # pass args (script reads them via command-line-args)
echo "1 2 add =" | trix --stdin   # run a program piped on stdin
trix --image snap.trix     # thaw and resume a saved VM snapshot
trix --version             # print version and feature flags
trix --help                # full flag list
```

Common flags: `-i` / `--stdedit` (force the REPL), `-q` / `--quiet`
(suppress diagnostics), `--sandbox` (disable filesystem / system / raw-memory
ops), `--module-path DIR:DIR` (extra `require` search dirs), and the VM-tuning
knobs (`--vm-size`, `--operand-depth`, ...).  See
[docs/cli.md](docs/cli.md) for every flag, the five startup modes, the module
search order, and the exit-code mapping.

## Embedding in C++

```cpp
#include "trix.h"

int main() {
    Trix trx;  // default 1 MB VM, interactive REPL
    return 0;
}
```

Or with configuration:

```cpp
#include "trix.h"

int main(int argc, char *argv[]) {
    auto result = Trix::parse_args(argc, argv);
    if (result.should_exit) return result.exit_code;

    Trix trx(result.vm_size, result.config);
    return 0;
}
```

Add custom operators:

```cpp
static void my_square_op(Trix *trx) {
    trx->verify_operands(Trix::VerifyInteger);
    auto val = trx->m_op_ptr->integer_value();
    *trx->m_op_ptr = Trix::Object::make_integer(val * val);
}

static constexpr Trix::Operator user_ops[] = {
    {my_square_op, "my-square"},
    {nullptr, {}},  // null terminator
};

// ...
result.config.m_useroperators = user_ops;
```

The interpreter runs synchronously inside the `Trix` constructor -- the script
(or REPL) has fully executed by the time the constructor returns, and
`exit_code()` is read after it.

For a long-lived **compute server**, pass `--resident` (or set
`Config::m_resident`): instead of exiting when its startup script drains, the
instance parks and serves work delivered from host threads (`invoke()` runs a
Trix buffer; `raise_interrupt()` fires a handler), until a `quit` or
`Trix::ExitIRQ` stops it. See
[Host Integration](docs/host-integration.md) for the full embedding surface:
the VM lifecycle and [resident/server mode](docs/host-integration.md#resident--server-mode),
the thread-safe `raise_interrupt` / `raise_error` / `invoke` methods, the
user-operator and interrupt APIs, and the complete Config table.

## Operator Summary

| Category                                                            | Operators |
| ------------------------------------------------------------------- | --------- |
| Core (stack, math, string, array, dict, set, coroutines, lazy, ...) | 660       |
| Pipelines (bounded buffers, backpressure)                           | 21        |
| Actors (mailboxes, selective receive)                               | 18        |
| Supervision (monitor, link, restart trees)                          | 18        |
| Logic / Backtracking (unify, choice, cut, find-all)                 | 20        |
| Reactive Cells (incremental computation)                            | 21        |
| Composability (protocols, matching, GenServer, transducers, ...)    | 36        |
| Infix expressions, modules, contracts                               | 34        |
| **Total**                                                           | **828**   |

## Metrics

| Metric               | Value                                                           |
| -------------------- | --------------------------------------------------------------- |
| Source               | ~85,000 lines C++23 (68 .inl files)                             |
| Operators            | 828                                                             |
| Test assertions      | 20,200+ across 283 test files                                   |
| Fuzz testing         | libFuzzer harness (full interpreter)                            |
| Interpreter dispatch | ~47M ops/sec (representative mix; see [benchmark/](benchmark/)) |
| Default VM size      | 1 MB (configurable, minimum 256KB)                              |
| Sanitizers           | ASan + UBSan clean (zero undefined behavior)                    |
| Dependencies         | readline, zlib                                                  |
| License              | Apache 2.0                                                      |

### Compiler Discipline

Trix compiles cleanly under GCC 15 with `-Werror` and an extensive warning
set (44 flags across GCC and Clang).  The principal GCC flags:

```
-Wall -Wextra -Wpedantic -Wformat=2 -Wformat-overflow=2
-Wformat-truncation=2 -Wformat-security -Wnull-dereference
-Wstack-protector -Wtrampolines -Walloca -Wvla -Warray-bounds=2
-Wimplicit-fallthrough=3 -Wshift-overflow=2 -Wcast-qual
-Wstringop-overflow=4 -Wconversion -Warith-conversion -Wlogical-op
-Wduplicated-cond -Wduplicated-branches -Wformat-signedness
-Wshadow -Wundef -Wstack-usage=16000 -Wswitch-enum -Wnrvo
```

Includes `-Wconversion` (no implicit narrowing), `-Wswitch-enum`
(every enum case handled), `-Wshadow` (no variable shadowing), and
`-Wduplicated-cond` / `-Wduplicated-branches` (no copy-paste bugs).

## Type System

31 types in an 8-byte tagged union (30 user-visible + SourceLoc, 1
reserved slot).  Every value on every stack and in every container is
an Object.

**Numeric:** Byte, Integer, UInteger, Long, ULong, Int128, UInt128, Address, Real, Double |
**Logical:** Boolean, Null |
**Containers:** Array, Packed, String, Dict, Set |
**Named:** Name, Operator, Mark |
**Composite:** Tagged, Record, Curry, Thunk |
**Runtime:** Stream, Coroutine, PipeBuffer, Cell, Continuation, OpaqueHandle, SourceLoc

64-bit values (Long, ULong, Double, Address) use heap-allocated
extension slots with automatic save/restore journaling.  128-bit values
(Int128, UInt128) use 16-byte wide-value slots.

See [Type System](docs/type-system.md) for details.

## Comparison

### Embeddable scripting peers

|  | Trix | Lua | Wren | Janet | Squirrel |
| --- | --- | --- | --- | --- | --- |
| Concurrency primitives | Coroutines + pipelines + actors + supervision | Coroutines | Fibers | Fibers | Threads (cooperative) |
| Fault tolerance | Supervision trees (Erlang/OTP-style) | Manual | Manual | `try` / `protect` | Manual |
| Logic programming | Unify + backtrack | No | No | No | No |
| Reactive computation | Cells + watchers | No | No | No | No |
| Whole-VM durability | Snap-shot / thaw to disk | No | No | `marshal` (values only) | No |
| Memory model | Local arena (save/restore) + mark-sweep GC for the global region | Incremental GC | Mark-sweep GC | Mark-sweep GC | Reference counting |
| Maturity | New (single author) | 30+ yrs, broad ecosystem | ~10 yrs | ~10 yrs | ~20 yrs |

Trix is positioned in the gap left by these languages: a header-only
embeddable scripting VM with built-in actor/supervision primitives.
The cost is a smaller ecosystem, a stack-based syntax that's
unfamiliar to most modern developers, and single-author maturity.

### vs. Erlang/BEAM

Trix takes the Erlang/OTP supervision model and compiles it down to a
header-only library you can embed in a C++ application.  It is **not**
a replacement for BEAM in production distributed systems: cooperative
single-threaded scheduling, no clustering, no hot code reload, no
preemptive process isolation across CPU cores.  If you need any of
those, use BEAM.

## Fuzz Testing

A libFuzzer harness covering the full interpreter is in `fuzz/`.
Requires clang (libFuzzer is LLVM-only):

```bash
./fuzz/build.sh
./fuzz/run.sh                              # single-shot; stops on first event
./fuzz/run.sh -max_total_time=300          # budget-aware loop, 5 minutes
./fuzz/run.sh -overnight                   # 8-hour run with 2MB VM heap
```

See [fuzz/README.md](fuzz/README.md) for the full CLI, triage workflow,
and corpus layout.

## Documentation

### Language Reference
- [Getting Started](docs/getting-started.md) -- a from-zero tour of the language
- [Cookbook](docs/cookbook.md) -- task-oriented "how do I..." recipes, each a short runnable block
- [Trix Reference](docs/trix-reference.md) -- all operators, stack effects, types
- [Running Trix](docs/cli.md) -- command-line reference: every flag, startup modes, exit codes
- [From PostScript](docs/from-postscript.md) -- guide for PostScript programmers: what carries over, what's renamed, what's new

### Concurrency
- [Coroutines](docs/coroutines.md) -- cooperative scheduling, yield, join
- [Pipelines](docs/pipeline.md) -- bounded buffers, backpressure
- [Actors](docs/actors.md) -- mailboxes, send/recv, selective receive
- [Supervision](docs/supervision.md) -- monitors, links, restart strategies

### Computation
- [Logic](docs/logic.md) -- unification, backtracking, choice points
- [Reactive Cells](docs/reactive.md) -- incremental computation, watchers
- [Lazy Sequences](docs/lazy-sequences.md) -- infinite streams, transformers
- [Functional Programming](docs/functional-programming.md) -- map, filter, fold, curry

### VM Internals
- [Architecture Overview](docs/dev-architecture.md) -- whole-engine map: source layout, execution pipeline, subsystem-to-file table
- [VM Internals](docs/vm-internals.md) -- heap, stacks, object system
- [Save/Restore](docs/save-restore.md) -- transaction journaling
- [Snapshot/Thaw](docs/snapshot-thaw.md) -- VM serialization
- [Type System](docs/type-system.md) -- 8-byte tagged union design

### Additional Features
- [Tagged Values](docs/tagged-values.md) -- discriminated unions, ADTs
- [Records](docs/record.md) -- immutable named-field composites
- [Pattern Matching](docs/pattern-matching.md) -- let, destructure, match
- [Protocols](docs/protocols.md) -- open type dispatch
- [Modules](docs/modules.md) -- scoped namespaces, require/use/import
- [Transducers](docs/transducers.md) -- composable data transformations
- [Contracts](docs/contracts.md) -- precondition/postcondition checking
- [Dates and Times](docs/chrono.md) -- instants, calendar dates, formatting
- [Source-Level Debugger](docs/debugger.md) -- interactive `--inspect` TUI, self-hosted in `lib/debugger.trx`

### Developer Guides
- [Architecture Overview](docs/dev-architecture.md) -- start here: how the source is organised and how a program flows from text to result
- [Adding Operators](docs/dev-adding-operators.md) -- step-by-step checklist
- [Control Operators and the Coroutine Trampoline](docs/dev-control-operators.md) -- the `@`-control-op mechanism behind every blocking/yielding/resumable op
- [Invariants](docs/dev-invariants.md) -- critical rules for contributors
- [Glossary](docs/dev-glossary.md) -- terminology reference

## Project Status

The language and runtime are feature-complete.  All concurrency layers
(coroutines, pipelines, actors, supervision) and computation features
(logic, reactive cells) are implemented and tested.

Current release: 0.9.0 -- the release-readiness pass is complete
(documentation audit, sad-path test expansion, fuzzing campaigns,
example showcase hardening, and a performance pass).  Remaining before
1.0: API stability soak and any feedback-driven adjustments.

56 reference documents cover all subsystems, indexed at
[docs/index.md](docs/index.md).  See
[docs/vm-internals.md](docs/vm-internals.md) and
[docs/gvm-heap-gc.md](docs/gvm-heap-gc.md) for the VM architecture,
[CONTRIBUTING.md](CONTRIBUTING.md) for how to build and test,
[STYLE.md](STYLE.md) for code conventions, and
[CHANGELOG.md](CHANGELOG.md) for release notes.

## License

Copyright 2026 Mark Guidarelli.  Licensed under the Apache License 2.0.
See [LICENSE](LICENSE) for details.
