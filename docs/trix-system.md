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

# Trix: System Architecture

How all subsystems compose into a unified programming system.

---

## 1. What Trix Is

Trix is a stack-based programming language and virtual machine implemented
as a header-only C++23 library.  It provides 838 operators spanning eight
programming paradigms in a deterministic runtime -- GC-free on the local
execution path -- with a 1 MB default VM heap (256KB minimum).

```
Source code (.trx)
     |
     v
  Scanner (tokenizer: text and binary formats)
     |
     v
  Interpreter (exec stack dispatch, 31 object types dispatched)
     |
     v
  36 operator categories (~85K lines C++)
     |
     v
  VM Heap (1 MB default, single contiguous allocation, offset-addressed)
```

**Key numbers:**

| Metric                | Value                                      |
| --------------------- | ------------------------------------------ |
| Operators             | 838                                        |
| Operator categories   | 36 (`ops_*.inl` files)                     |
| Type system           | 32 types (5-bit tag, 32 slots, 0 reserved) |
| VM heap               | 1 MB default (256KB minimum)               |
| Source code           | ~85K lines C++ (header-only)               |
| Test assertions       | 20,200+ across 270 test files              |
| External dependencies | readline, zlib                             |

### 1.1 The Eight Paradigms

| Paradigm             | Subsystems                                                  | Operators |
| -------------------- | ----------------------------------------------------------- | --------- |
| **Stack/Imperative** | Stack, Math, Bitwise, Convert, String, Format               | ~277      |
| **Functional**       | Higher-order, Lazy sequences, Transducers                   | ~97       |
| **Data-Structural**  | Array, Dict, Set, Record, Tagged                            | ~116      |
| **Concurrent**       | Coroutines, Pipelines, Actors, Continuations, Effects       | ~62       |
| **Fault-Tolerant**   | Supervision, Error handling, Save/Restore                   | ~40       |
| **Logic**            | Backtracking, Unification, Choice points                    | ~20       |
| **Reactive**         | Cells, Dependency tracking, Watchers                        | ~21       |
| **Composability**    | Protocols, Pattern matching, GenServer, Closures, Contracts | ~23       |

(Counts are release-build user-facing operators grouped by defining
source file; the remaining ~170 operators are the I/O and host surface
-- streams, terminal/screen, chrono, system, regex, snapshot --
infrastructure rather than paradigm.)

No single paradigm dominates.  Trix users choose the paradigm that fits
the problem, and combine paradigms freely within a single program.  A
GenServer handler might use pattern matching for argument binding,
protocols for polymorphic dispatch, closures for captured configuration,
contracts for input validation, and transducers for batch data processing
-- all within one proc.

### 1.2 What This Document Covers

This document describes **how all the subsystems fit together** -- the
architectural principles, the composition patterns, the design decisions
that make cross-subsystem interaction possible, and the concrete ways
that features combine to express programs that would require multiple
frameworks in other languages.

For individual subsystem details, see the per-feature technical references
listed in the Documentation Map (section 11).


## 2. The Composition Pyramid

Trix is not a bag of features.  Each layer builds on the layers below,
and every layer composes with every other.  The pyramid from foundation
to application:

```
                    Application Patterns
            GenServer, Supervision trees, Data pipelines
                           |
                  Composability Glue
       Protocols, Pattern matching, Closures, Contracts, Transducers
                           |
                   Concurrent Runtime
       Coroutines, Pipelines, Actors, Supervision, Reactive cells
                           |
                       Data Model
            32 types, Records, Tagged ADTs, Sets, Dicts
                           |
                    Functional Core
        Higher-order (map/filter/reduce), Lazy sequences, Curry/Compose
                           |
                    Imperative Base
             Stack ops, Math, Strings, I/O, Flow control
                           |
                     VM Foundation
          Heap, Scanner, Interpreter, Save/Restore, Snap-shot/Thaw
```

### 2.1 Why the Pyramid Matters

Each layer is designed to be unaware of the layers above it.  Coroutines
do not know about actors.  Actors do not know about GenServer.  Protocols
do not know about supervision.  This strict layering means:

- **Layers can be used independently.** You can use coroutines without
  actors, records without protocols, closures without GenServer.  There
  is no "you must buy the whole framework" tax.

- **Layers compose freely.** A supervised GenServer can use protocols
  for message dispatch, pattern matching for argument binding, closures
  for captured configuration, contracts for input validation, and
  transducers for data processing -- all in a single handler.  None
  of these features were designed for this specific combination; they
  compose because they share the same foundation.

- **New layers do not break old ones.** Adding protocols did not change
  a single line in the coroutine, actor, or supervision code.  Adding
  GenServer did not change the protocol or pattern matching code.  This
  is the payoff of layered design: each addition is localized.

- **Testing is compositional.** Each layer has its own test suite that
  tests the layer in isolation.  Cross-layer tests (e.g., GenServer tests)
  exercise composition without needing to test the lower layers again.

### 2.2 The Layer Contract

Every layer follows the same contract:

1. **All state lives on the VM heap** (via `vm_offset_t` offsets).  No
   external state, no heap-allocated C++ objects, no global variables.
2. **All persistent state survives snap-shot/thaw.**  New member variables
   must be added to `SnapShotHeader`.
3. **All mutable state is journaled by save/restore** where appropriate.
   Dict::put operations are automatically journaled.
4. **All blocking operations use the trampoline pattern.**  Push
   continuation on exec stack, return to interpreter.
5. **All 64-bit values (ExtValue) have explicit ownership.**  Consume
   or free; never discard silently.


## 3. VM Foundation: The Substrate

Everything in Trix runs on a single contiguous block of memory.

### 3.1 The Heap Model

```
m_vm_base                                              m_vm_limit
  |                                                        |
  v                                                        v
  [systemdict|protocoldict|userdict|errordict|stacks|...data...| free ]
                                                   ^
                                                   |
                                              m_vm_ptr
```

The VM heap is a single `malloc`-allocated block (1 MB default, 256KB minimum).
`m_vm_ptr` is the allocation frontier: `vm_alloc_ptr` advances it.
All composite objects -- strings, arrays, dicts, names, records, tagged
values, coroutine contexts, mailboxes, pipe buffers, cell graphs -- live
here.  Nothing is allocated on the C++ heap during normal operation.

**Offsets, not pointers.**  Every reference between objects is a
`vm_offset_t` (32-bit unsigned offset from `m_vm_base`).  This has
three consequences:

1. The entire VM can be serialized by writing the raw byte range
   `[m_vm_base, m_vm_ptr)` -- all internal references remain valid.
2. References are 4 bytes, not 8.  A Dict entry is 20 bytes (next
   offset + key Object + value Object); a Set entry is 12.
3. The maximum addressable heap is 4 GB (`vm_size_t` is `uint32_t`).
   The 1 MB default is policy, not an architectural ceiling --
   `--vm-size` scales it.

### 3.2 The Object Model

Every value is exactly 8 bytes:

```
  7 6 5 4 3 2 1 0
  +------- X: Execution attribute (0 = Literal, 1 = Executable)
  | +----- W: Access mode (0 = ReadOnly, 1 = ReadWrite)
  | | +--- F: per-type flag (Boolean value, eqref marker, address-probe cache)
  | | | +- TYPE: 5-bit type tag (32 slots, 31 used)
  | | | |
  [X W F T T T T T] [--- 7 bytes of payload ---]
```

31 of 32 type slots are used: Null, Byte, Integer, UInteger, Long, ULong,
Address, Real, Double, Boolean, Operator, Mark, Name, Array, Packed,
String, Stream, Dict, SourceLoc, Curry, Thunk, Set, Tagged, Record,
Coroutine, PipeBuffer, Cell, Continuation, Int128, UInt128, OpaqueHandle.
One slot remains for a future type.

For 64-bit values that do not fit in 7 payload bytes (Long, ULong, Double,
Address), the payload contains an `ExtValue` offset pointing to an 8-byte
extension on the heap.  ExtValues have their own free-list for efficient
reuse.  For 128-bit values (Int128, UInt128), the payload contains a
`WideValue` offset pointing to a 16-byte extension on the heap, with its
own free-list, save-level record, and offset-XOR'd sentinel scheme.

### 3.3 The Four Stacks

| Stack   | Default Depth | Purpose                                         |
| ------- | ------------- | ----------------------------------------------- |
| Operand | 1024          | Data values -- the primary workspace            |
| Exec    | 2048          | Objects pending execution -- the call stack     |
| Dict    | 64            | Active dicts for name lookup -- the scope stack |
| Error   | 64            | Error context during exception handling         |

Plus a **save stack** for transaction checkpoints.  All stacks are
contiguous arrays within the VM heap.

### 3.4 The Interpreter Loop

The interpreter is a tight loop that pops objects from the exec stack
and dispatches by type:

- **Literal** objects (integers, strings, names) push to the operand stack.
- **Executable** names are looked up in the dict stack; the found
  value is pushed back onto the exec stack for further dispatch.
- **Operators** call native C++ functions (the 838 built-in operators).
- **Executable arrays** (procs) push their elements in reverse onto
  the exec stack, so the first element executes first.
- **Strings and streams** are scanned for tokens (invoking the scanner).

This dispatch loop is the only place where execution happens.  There is no
separate "call" mechanism, no function frames, no return addresses.  The
exec stack IS the call stack.  This simplicity is what makes coroutine
context switching trivial: swap the stack pointers, resume the loop.

### 3.5 Save/Restore: Transactions

`save` creates a checkpoint.  `restore` rolls back all Dict mutations,
Array mutations, and Packed Array mutations since the checkpoint, and
reclaims heap space.  This is used for:

- **Error recovery**: `try` creates an implicit save; catching an error
  restores to it.
- **Scoping**: Temporary modifications that should not persist.
- **Exploration**: Logic programming's choice points use save/restore
  for backtracking.

### 3.6 Snap-Shot/Thaw: Full Serialization

`snap-shot` writes the complete VM state to a byte stream:

- All 4 stacks (contents + pointers)
- The entire heap `[m_vm_base, m_vm_ptr)`
- All member variables (via `SnapShotHeader`, currently 592 bytes,
  guarded by a `static_assert` against silent layout drift)
- Stream reconnection data
- Coroutine contexts and scheduler state

`thaw` restores from the byte stream.  The restored VM is indistinguishable
from the original.  Every feature in Trix -- protocols, GenServer, reactive
cells, supervision trees -- survives this cycle because every feature stores
its state on the VM heap using `vm_offset_t` offsets.


## 4. The Subsystems

### 4.1 Data Model: 31 Types, Zero Waste

Trix's type system is the foundation that all higher features build on.
The key types for composition:

**Record**: Immutable named-field composite.  `3 4 point` creates a record
with fields `/x` and `/y`.  Records are the "struct" of Trix -- they give
named access to structured data without the overhead of a hash table.
4 + N*8 bytes for N fields.  Schema-safe: accessing a nonexistent field
is a hard error.  Immutable: updates return new records.

**Tagged**: Discriminated union (algebraic data type).  `42 /ok tag`
creates a tagged value.  Tags carry a name and a payload.  `tag-match`
dispatches on the tag name.  Tagged values are the "enum with data" of
Trix -- they model Result types, Option types, AST nodes, messages.

**Dict**: Hash table mapping names to values.  Three modes: ReadOnly
(immutable after construction), ReadWriteFixed (fixed capacity, fast),
ReadWriteDynamic (growable).  Dicts serve as namespaces (systemdict,
userdict), protocol registries, GenServer spec dicts, and closure captures.

**Curry**: A pair of [value, callable].  When executed, the value is pushed
onto the operand stack, then the callable executes.  Curry is the
representation for closures (value=dict, callable=wrapped proc), partial
application, and GenServer ref-matching predicates.

**Array**: Mutable, indexed, variable-length.  Procs are executable
arrays.  Transducer composed steps are literal arrays.  GenServer messages
are arrays.  Match pair lists are arrays.  The workhorse container.

These five types -- Record, Tagged, Dict, Curry, Array -- are the building
blocks that the six composability features are constructed from.  No new
types were needed.

### 4.2 Functional Core: Composition Without Closures

Trix's functional programming model is unusual: it achieves composition
without requiring closures as a default mechanism.

**Procs are data.**  `{ 2 mul }` is an executable array of two
objects: the integer 2 and the operator `mul`.  It is not a heap-allocated
closure with an environment pointer.  It is 16 bytes of flat data on the
heap.

**Curry pairs** provide single-value partial application: `10 { add } curry`
creates a Curry [10, {add}] that, when executed, pushes 10 then runs {add}.
This covers the common case of binding one value.

**Compose** chains two procs: `{ f } { g } compose` creates a
proc that runs f then g.  Combined with curry, this covers most
functional patterns without closure allocation.

**Explicit closures** (`closure-capture`) are available for the cases where
curry/compose are insufficient (capturing 3+ values).  But they are opt-in,
not the default.  Most Trix functional code runs with zero closure overhead.

**Lazy sequences** are demand-driven: `1 lazy-from { 2 mul } lazy-map`
creates an infinite sequence of powers of 2, but only computes values
when pulled.  Lazy sequences bridge the finite (arrays) and the infinite
(generators) with a uniform interface.

**Transducers** unify transformations across targets: define once as tagged
data, apply to arrays (`into`), lazy sequences (`lazy-into`), or pipelines
(`pipe-into`).  No closures, no duplication.

### 4.3 Concurrent Runtime: Cooperative, Not Preemptive

Trix uses single-threaded cooperative multitasking.  There is one OS
thread, one interpreter loop, and one VM heap.  Concurrency comes from
context switching at explicit yield points.

**Coroutines** (~4KB each) are the foundation.  Each coroutine has its
own operand, exec, dict, and error stacks plus a scratch arena (saved in
a CoroutineContext on the heap).  Context switching swaps these pointers.  The scheduler is
round-robin with targeted wakeup for blocked coroutines.

**Pipelines** are linear dataflow chains.  Each stage is a coroutine that
reads from an input buffer and writes to an output buffer.  Buffers are
bounded (SPSC).  Pipeline stages block when the input is empty or the output
is full, creating natural backpressure.

**Actors** are coroutines with attached mailboxes.  Any coroutine can send
to an actor's mailbox; only the owning actor can receive.  This is
many-to-one communication with backpressure (send blocks if mailbox full).

**Why cooperative, not preemptive?**  Three reasons:

1. **No locks, no races.**  A single-threaded cooperative model eliminates
   all data race issues by construction.  Shared heap access is safe because
   only one coroutine runs at a time.

2. **Deterministic.**  The scheduler runs in a known order.  There are no
   "sometimes it crashes" race conditions.  If a bug exists, it reproduces
   every time.

3. **Serializable.**  Cooperative scheduling means coroutine state is always
   consistent at yield points.  snap-shot captures a coherent state.
   Preemptive scheduling would require stopping all threads at a safe point,
   which is fundamentally harder.

The cost: a single coroutine that never yields (infinite loop without I/O
or explicit yield) blocks all other coroutines.  This is an application bug,
not a runtime limitation.

### 4.4 Fault Tolerance: Let It Crash

Trix's supervision framework implements the Erlang/OTP "let it crash"
philosophy:

- **Links**: If actor A is linked to actor B and B dies, A receives a
  death notification.  Links are bidirectional.
- **Monitors**: One-way observation.  If B dies, A gets notified but is
  not killed.
- **Supervisors**: Actors that monitor children and restart them according
  to a strategy (one-for-one, one-for-all, rest-for-one).

This composes with GenServer naturally: a GenServer is an actor, actors can
be supervised, and supervisors are themselves actors (supervisable).  The
entire supervision tree is built from the same primitives.

### 4.5 Logic and Reactive: Reasoning and Incremental Computation

**Logic programming** (20 operators) provides Prolog-style choice points
and backtracking.  `choice` creates a save point; `fail` triggers
backtracking to the last choice point and tries the next alternative.
`unify` performs structural unification.  Logic programming uses
save/restore for backtracking -- no special memory management.

**Reactive cells** (21 operators) provide incremental computation with
automatic dependency tracking.  Base cells hold explicit values; computed
cells derive their value from a proc that references other cells.  When a
base cell changes, all transitive dependents are marked dirty and
recomputed on demand (pull-based with push invalidation).


## 5. The Composability Layer

The six features added in the composability layer are the focus of this
document.  They do not add new capabilities -- everything they do was
already possible with raw stack manipulation, `type-case`, `begin`/`end`,
manual recv loops, and inline assertions.  What they add is **ergonomics
and composability**: the ability to express complex patterns concisely and
combine them freely.

### 5.1 Protocols: Open Extension

Protocols let types opt into named interfaces without modifying existing
code.  A protocol defines method names; types register implementations.
At call time, the runtime dispatches to the correct implementation based on
the argument's type.

The key architectural choice: protocol dispatch reuses `type-case`.  The
dispatch proc for each method is literally `{ dup <dispatch-dict>
type-case }` -- a 3-element executable array.  No new dispatch machinery
was needed.  The dispatch dict is a mutable Dict that grows as types
register.

Protocols enable **ecosystem-level interop**: a library defines a protocol;
user types opt in without touching the library.  This breaks the coupling
between algorithms and data types that plagues stack-based languages.

### 5.2 Pattern Matching: Named Access to Stack Values

Three layers of increasing power:

- **let**: Bind N stack values to names in a dict scope.  `10 20 [/x /y]
  let ... end`.  Eliminates stack juggling for multi-argument procs.
- **destructure**: Extract fields from arrays (by index), records (by
  field name), or dicts (by key).  Eliminates manual `get`/`index` chains.
- **match**: Multi-arm dispatch with test/body pairs.  The test proc
  receives a dup'd value and must consume it, pushing a boolean.  First
  true test wins.  Eliminates nested `if-else` chains for type and value
  dispatch.

The key architectural choice: the match trampoline uses a Mark sentinel
(not null) for `cond` mode, allowing null to be a valid match value.
Test procs must consume their input -- this strict contract keeps the
stack predictable between match arms.

### 5.3 Closures: Explicit Capture

`closure-capture` looks up N names in the current dict stack, builds a
ReadOnly dict with those bindings, and wraps it with the proc as a
Curry object.  When invoked, the dict is pushed on the dict stack, making
captured names visible to the proc body.

The key architectural choice: closures are Curry objects, not a new type.
The Curry value is the captured dict; the Curry callable is a wrapped proc
that invokes `closure-with-dict`.  This reuses the Curry execution path
(push value, execute callable) with zero changes to the interpreter.

### 5.4 Contracts: Domain Constraints

`precondition` pops a boolean; raises `/require` if false.
`postcondition` takes a check proc and a body proc; runs the body, dups
the result, runs the check, raises `/ensure` if false.

The key architectural choice: `precondition` takes a boolean (not a proc),
because preconditions naturally arise from inline computation (`dup 0 ge
precondition`).  `postcondition` takes procs because the body must run
first and the check must see the result.  Separate error codes (`/require`
vs `/ensure`) distinguish caller bugs from callee bugs.

### 5.5 Transducers: Write Once, Apply Anywhere

Transducers are tagged data -- each step is a Tagged value like
`{ 2 mul } /xf-map tag`.  Composition concatenates arrays.  Application
operators (`into`, `lazy-into`, `pipe-into`) interpret the spec and
translate each step to the target's native operator.

The key architectural choice: data, not higher-order functions.  Trix
transducers are inspectable, serializable, and cheap (16 bytes per step).
The translation from spec to native operators happens once at application
time, not per element.  There is no per-element overhead beyond what the
native operators would cost.

### 5.6 GenServer: Standardized Actor Patterns

GenServer wraps the actor receive loop with state threading, message
categorization, and request/reply delivery.  The user provides a spec dict
with handler procs; the runtime handles the machinery.

The key architectural choice: the GenServer loop state lives in 4 exec
stack slots (spec, state, ref, from).  This is free -- the exec stack is
already saved/restored by coroutine context switching.  No heap allocation
for the loop beyond the initial setup.  `gen-call` uses a Curry-based
ref-matching predicate with `actor-recv-match` for reply correlation.


## 6. Cross-Layer Composition Patterns

The true power of Trix is not any single feature but how features combine.
This section walks through concrete composition patterns with full code.

### 6.1 Protocol + Record + Destructure: Typed Data Processing

```
% Define a type
/Measurement [/sensor /value /unit] record-type def

% Define a protocol
[/display /validate] /Sensor def-protocol

% Implement for Record type
{
    [/sensor /value /unit] destructure
        (Sensor ) sensor concat
        (: ) concat
        value 10 string to-string concat
        ( ) concat unit concat
    end
} /display /record-type def-method

{
    [/value] destructure
        value 0.0 ge value 1000.0 lt and
    end
} /validate /record-type def-method

% Use
(temp) 72.5 (F) Measurement
dup display =             % => (Sensor temp: 72.5 F)
validate =                % => true
```

Three subsystems compose: Record (structured data), Protocol (polymorphic
dispatch on `/record-type`), Destructure (named field access in handlers).
Each subsystem is unaware of the others; they compose because they share
the same Object model and dict/stack semantics.

### 6.2 Transducer + Lazy Sequence + Closure: Infinite Stream Processing

```
% Processing parameters, available to the transducer steps via dict-stack lookup.
% (xf-filter/xf-map require plain procs -- a Curry from closure-capture is
%  rejected -- so the steps reference the captured values by name instead.)
/threshold 50 def
/scale 2.5 def

/sensor-xf
    { threshold gt }               xf-filter
    { /real-type cast scale mul }  xf-map xf-compose
def

% Apply to infinite stream of sensor readings
1 lazy-from                    % [1, 2, 3, 4, ...]
sensor-xf lazy-into            % filter > 50, scale by 2.5
10 lazy-take lazy-to-array     % take first 10 results
% => [127.5 130.0 132.5 135.0 137.5 140.0 142.5 145.0 147.5 150.0]
```

Four subsystems compose: dict-stack lookup (the steps read `threshold` and
`scale` by name), Transducers (define the pipeline as data), Lazy sequences
(infinite source, demand-driven), and higher-order operators (the underlying
filter/map).  The transducer is defined once and could equally be applied to
an eager array or pipeline.  Transducer steps must be plain procs; closures and
currys are not accepted by `xf-filter`/`xf-map`.

### 6.3 GenServer + Match + Contract: Validated Stateful Service

```
% handle-call ends with the op stack reading  new-state reply-value /reply.
% match leaves the matched value on the stack below the arm result, so each
% arm pops that leftover first.
mark {
    <<
        /init { << /total 0 /count 0 >> }
        /handle-call {
            [/msg /from /state] let
                msg [
                    { is-integer } {
                        pop                                       % drop match's leftover msg
                        msg 0 ge precondition                     % contract: non-negative
                        state /total state /total get msg add put % total += msg
                        state /count state /count get 1 add put   % count += 1
                        state state /total get /reply             % reply the new total
                    }
                    { /average eq } {
                        pop
                        state /total get state /count get
                        dup 0 gt { div } { pop pop 0 } if-else
                        state exch /reply                         % reply the average
                    }
                    { pop true } {
                        pop
                        state /unknown /reply                     % reply /unknown
                    }
                ] match
            end
        }
    >> gen-server /stats exch def

    stats 10 gen-call pop     % add 10  -> total 10
    stats 20 gen-call pop     % add 20  -> total 30
    stats 30 gen-call pop     % add 30  -> total 60
    stats /average gen-call   % => 20

    stats /done gen-stop
    100 coroutine-sleep
} actor-spawn coroutine-join pop
```

Five subsystems compose: GenServer (stateful server), Pattern matching
(`let` for argument binding, `match` for message dispatch), Contracts
(`precondition` for input validation), Dicts (state representation),
and Actors (the underlying message passing).

### 6.4 Supervision + GenServer + Protocol: Fault-Tolerant Service

```
% Protocol for processable work items
[/process] /Workable def-protocol
{ 2 mul }             /process /integer-type def-method
{ dup concat }        /process /string-type def-method
{ pop null }          /process def-default-method

% Worker GenServer
/worker-spec <<
    /init { 0 }
    /handle-cast {
        exch                          % msg state -> state msg
        dup /Workable protocol-satisfies?
        { process }                   % dispatch via protocol
        { pop }                       % discard unprocessable
        if-else
        add                           % accumulate (will error on non-int)
    }
    /terminate { pop pop }
>> def

% Spawn under supervision
% worker-spec gen-server
% ... supervisor watches, restarts on crash ...
```

If the worker crashes (e.g., `process` returns a string and `add` fails
with a type error), the supervisor detects the death, runs the restart
strategy, and spawns a fresh worker.  The protocol dispatch, the GenServer
state management, and the supervision restart are all independent -- each
does its job without knowing about the others.

### 6.5 Reactive Cell + Transducer + Contract: Validated Derived State

```
% Base data cell
[1 2 3 4 5 6 7 8 9 10] cell /raw-data exch def

% Computed cell: process through transducer pipeline.
% cell-computed is `proc -- cell`; the body reads each dependency
% explicitly with cell-get (the dependency value is NOT auto-pushed).
{
    raw-data cell-get
    { 2 mod 0 eq } xf-filter
    { 2 mul } xf-map xf-compose
    into
} cell-computed /processed exch def

% Computed cell: validate and summarize
{
    processed cell-get
    dup length 0 gt precondition    % must have data
    0 { add } reduce
} cell-computed /total exch def

% Read derived values (lazy -- only computed on demand)
processed cell-get     % => [4 8 12 16 20]
total cell-get         % => 60

% Update base data -> derived values auto-invalidate
[10 20 30 40 50] raw-data cell-set
total cell-get         % => 300 (recomputed automatically: all even -> *2 -> sum)
```

Four subsystems compose: Reactive cells (dependency tracking), Transducers
(data transformation), Contracts (validation), and higher-order operators
(reduce).  The reactive graph automatically tracks that `total` depends on
`processed` which depends on `raw-data`, and recomputes only what is needed
when the base data changes.

### 6.6 Logic + Backtracking: Constrained Search

```
% Find a pair (x, y) where x + y = 10 and both are positive.
% choice takes an ARRAY of alternative branches; it commits to the first
% branch whose guards all succeed, backtracking past any that fail.
logic-var /x exch def
logic-var /y exch def
[
    { x 2 unify guard  y 7 unify guard          % 2 + 7 = 9: fails the sum guard
      x deref y deref add 10 eq guard }
    { x 4 unify guard  y 6 unify guard          % 4 + 6 = 10, both positive: succeeds
      x deref y deref add 10 eq guard
      x deref 0 gt guard
      y deref 0 gt guard }
] choice
x deref y deref        % => 4 6
% guard prunes a branch by triggering backtracking; choice explores the
% alternatives in order and binds x/y from the first branch that survives.
```


## 7. What Makes Trix Unique

### 7.1 Everything in One Compact Heap

The default VM heap is 1 MB (256KB minimum).  That is enough for coroutines, actors,
supervision trees, reactive cell graphs, protocol registries, lazy
sequences, and transducer pipelines -- all simultaneously.  This is
possible because of four design decisions:

**GC-free local arena.** Memory management on the local arena is
deterministic: `save`/`restore` for transactions (automatic rollback),
`free` for explicit deallocation, ExtValue free lists for 64-bit values.
The durable global region (`${...}`) uses a precise mark-sweep GC, but
collection there is confined and demand-driven -- the local execution
path never pauses for GC.

**Procs are data, not closures.** `{ 2 mul }` is a 16-byte executable
array, not a heap-allocated closure object with an environment pointer.
Most functional programming in Trix requires zero closure allocation.
Explicit closures are opt-in and rare.

**Compact types.** Every value is exactly 8 bytes.  An array of 1000
integers is 8000 bytes.  In a language with boxed objects (Java, Python),
the same array is 24,000+ bytes (object headers, type pointers, padding).
In Trix, the ratio of useful data to overhead is near 1:1 for primitive
types.

**Offset-based addressing.** Every internal reference is a `vm_offset_t`
(4 bytes), not a pointer (8 bytes).  A Dict entry is 20 bytes; a Set
entry is 12.  Halving every reference is part of what makes even the
256KB minimum heap viable for complex programs.

### 7.2 Unified Abstraction: One Model, Many Paradigms

Every programming paradigm in Trix shares the same foundations: the same
8-byte Object, the same dict stack for name resolution, the same exec stack
for control flow, the same VM heap for storage.  There is no "functional
mode" vs "concurrent mode" -- a GenServer handler IS a functional proc
that happens to run inside an actor that happens to be supervised.

This means:

- **Tools transfer.** Learning `map`/`filter`/`reduce` for arrays means
  you already know how transducers work (same step functions).  Learning
  `begin`/`end` dict scoping means you already know how `let` and
  `destructure` work (same dict stack push/pop).  Learning `type-case`
  means you already understand protocol dispatch (same dict-based lookup).

- **Debugging is uniform.** `pstack` shows the operand stack regardless of
  whether you are in a GenServer handler, a transducer step, a match body,
  or a closure.  The stack trace (exec stack) shows the full continuation
  through all abstractions.

- **Performance is predictable.** Every operation is a known number of
  dict lookups, stack operations, and heap allocations.  There are no
  hidden costs from abstraction layers.  A protocol dispatch is exactly
  one `dup` + one `type-case`.  A closure invocation is exactly one dict
  stack push + proc exec + one dict stack pop.

### 7.3 Deterministic Execution

No GC pauses on the local execution path (the global region's mark-sweep
runs only when global allocation demands it).  No thread scheduling surprises.  No
allocation failures disguised as undefined behavior (heap exhaustion
is a clean Error::VMError).  Cooperative scheduling means context switches
happen at known points (yield, recv, pipe-read/write, actor-send to full
mailbox).

**Every execution is reproducible.** Given the same input and the same
heap size, Trix produces the same output, in the same order, with the
same memory layout.  There is no source of non-determinism except explicit
randomness (the `rand-*` family, e.g. `rand-real`).

For embedded, real-time, or safety-critical contexts, determinism is
not a nice-to-have -- it is a requirement.  Trix meets it by construction.

### 7.4 Full Serializability

`snap-shot` captures the entire VM state -- stacks, heap, coroutines,
actors, protocol registries, reactive cells, GenServer frames, supervision
trees, everything -- into a byte stream.  `thaw` restores it exactly.

This enables:

- **Process persistence.** Save a running computation to disk.  Restart
  the program later.  Resume exactly where you left off -- including
  mid-flight GenServer handlers, blocked actors, pending messages.
- **Process migration.** Serialize on machine A, deserialize on machine B,
  resume.  The VM is entirely self-contained.
- **Time-travel debugging.** Snapshot before a suspected bug.  Reproduce.
  Snapshot at the crash point.  Compare heap states.
- **Fault recovery.** Periodic snapshots provide a recovery point.  If the
  process crashes, thaw the last snapshot and resume.

Every feature in Trix was designed to survive snap-shot/thaw.  This is not
an afterthought -- it is a load-bearing architectural requirement.  Every
new member variable must be added to `SnapShotHeader`.  Every new persistent
data structure must live on the VM heap.  Every new feature is tested with
snap-shot/thaw as part of its validation suite.

### 7.5 Single-File Deployment

Trix is header-only.  `#include "trix.h"` and compile.  The entire
language -- VM, scanner, interpreter, all 838 operators, all 32 types --
is one compilation unit.  No separate runtime, no shared libraries, no
build system complexity, no version-mismatch headaches.

The 68 `.inl` files are `#include`d inside the `class Trix {}` body.
They are implementation fragments, not separate translation units.  The
compiler sees one class with one set of member functions.  Link-time
optimization works at full effectiveness.


## 8. Comparison to Other Systems

### 8.1 Trix vs Erlang/OTP

| Aspect           | Erlang/OTP                        | Trix                                           |
| ---------------- | --------------------------------- | ---------------------------------------------- |
| Concurrency      | Preemptive, multi-core BEAM VM    | Cooperative, single-threaded                   |
| Memory           | Per-process heaps, GC per process | Shared heap, GC on global region only          |
| Message passing  | Copy between processes            | Zero-copy (shared heap)                        |
| Supervision      | Built-in, mature, battle-tested   | Built-in, same patterns                        |
| Hot code swap    | Yes                               | No                                             |
| Type system      | Dynamic, no static types          | 31-type tagged union, no coercion              |
| Footprint        | ~50MB BEAM VM                     | 1 MB heap (256KB min), ~1.8MB optimized binary |
| Protocols        | Behaviours + protocols (Elixir)   | Built-in, 6 operators                          |
| Pattern matching | Native, deep, compiler-optimized  | Runtime, 6 operators                           |

Trix draws heavily from Erlang's actor model and supervision philosophy.
The key difference is scale: Erlang is designed for millions of processes
across clusters; Trix is designed for bounded, deterministic, serializable
computation within a single small heap (1 MB default).

### 8.2 Trix vs Forth

| Aspect          | Forth                                  | Trix                                           |
| --------------- | -------------------------------------- | ---------------------------------------------- |
| Types           | Untyped cells (1 or 2 cells per value) | 32 types, tagged, verified                     |
| Safety          | None (raw memory access)               | Type-checked, bounds-checked                   |
| Data structures | User-built from raw memory             | Built-in Array, Dict, Record, Set, Tagged      |
| Concurrency     | None (typically)                       | Coroutines, Actors, Pipelines                  |
| Functional      | None                                   | map, filter, reduce, curry, compose, lazy seqs |
| Persistence     | None                                   | snap-shot/thaw                                 |

Trix is what Forth would be if you started with Forth's execution model
(exec stack, postfix) but added a modern type system, first-class data
structures, and multi-paradigm programming.

### 8.3 Trix vs PostScript

| Aspect       | PostScript                                            | Trix                          |
| ------------ | ----------------------------------------------------- | ----------------------------- |
| Purpose      | Page description                                      | General programming           |
| Types        | ~10 (int, real, bool, string, array, dict, name, ...) | 31                            |
| Dict stack   | Yes (same model)                                      | Yes (same model, extended)    |
| Save/Restore | Graphics state only                                   | Full VM state                 |
| Concurrency  | None                                                  | Coroutines, Actors, Pipelines |
| Named fields | No                                                    | Records                       |
| ADTs         | No                                                    | Tagged values                 |

Trix inherits PostScript's scanner, dict stack, and name resolution model,
then extends them far beyond PostScript's page-description scope.

### 8.4 Trix vs Clojure

| Aspect       | Clojure                              | Trix                                    |
| ------------ | ------------------------------------ | --------------------------------------- |
| Host         | JVM (~200MB+ runtime)                | Self-contained (256KB heap)             |
| Protocols    | First-class, `defprotocol`           | Built-in, 6 operators                   |
| Transducers  | Higher-order functions (rf -> rf)    | Tagged data (inspectable, serializable) |
| Immutability | Default (persistent data structures) | Records immutable, arrays mutable       |
| Concurrency  | STM, agents, core.async              | Coroutines, actors, pipelines           |
| Typing       | Dynamic                              | 31-type tagged union                    |
| GC           | JVM GC                               | Global region only (mark-sweep)         |

Trix's protocols and transducers are directly inspired by Clojure's, but
adapted to a stack machine.  The key difference: Trix's transducers are
data (tagged values), not closures, making them cheaper and serializable.


## 9. Design Principles

### 9.1 Features Compose Through Shared Foundations

Every feature in Trix -- from basic stack operations to GenServer -- shares
the same Object representation, the same dict stack for name resolution,
the same exec stack for control flow, and the same heap for storage.  New
features compose with existing features automatically because they operate
on the same data and use the same execution model.

This is not accidental.  It is the core architectural constraint: a new
feature must work within the existing foundations, not create parallel
mechanisms.  Protocols dispatch via `type-case` (existing).  Closures
execute via Curry (existing).  GenServer loops via `actor-recv` (existing).
The result is that every new feature immediately works with every existing
feature -- no adapters, no conversion layers, no compatibility shims.

### 9.2 Reuse Existing Machinery

| Feature                | Reuses                                         |
| ---------------------- | ---------------------------------------------- |
| Protocol dispatch      | `type-case` operator                           |
| Match trampoline       | Control-operator pattern from `for`/`repeat`   |
| Closures               | Curry execution path                           |
| GenServer recv loop    | `actor-recv`, `actor-send`, `actor-recv-match` |
| Transducer application | `map`, `filter`, `take`, `drop`, etc.          |
| Contract postcondition | Control-operator trampoline pattern            |

No feature required new interpreter dispatch modes, new stack types, or
new memory management strategies.  The most complex addition was 4 new
control operators (@match-test, @gen-server-recv, @gen-server-call-done,
@gen-server-cast-done) -- all following the established control-operator
pattern.

### 9.3 Trampoline Everything That Blocks

Any operation that may suspend the current coroutine must use the
trampoline pattern:

1. Set up all state on the VM stacks (not C++ local variables).
2. Push the continuation onto the exec stack.
3. Push the blocking operation onto the exec stack.
4. Return to the interpreter.

The interpreter executes the blocking operation.  If it blocks, the
coroutine context switches.  When it resumes, the continuation fires.

This principle was violated in early GenServer code (direct C++ calls to
`actor_send_op`) and caused crashes when the mailbox was full.  All four
call sites were converted to the trampoline pattern during code review.

### 9.4 ExtValue Ownership is Non-Negotiable

Every code path that consumes or discards an Object must call
`maybe_free_extvalue(trx)` if the Object might hold a 64-bit ExtValue
(Long, ULong, Double, Address).  The audit found ExtValue leaks in:
- `match_op` (original value not freed after cloning)
- `when_op` (same pattern)
- GenServer message handling (array element ExtValues not freed)

Each leaked 8 bytes per occurrence.  In a long-running GenServer processing
Long values, this would exhaust the ExtValue free list.

### 9.5 Snap-Shot/Thaw is a Feature Tax

Every new member variable must be added to `SnapShotHeader` and the
snap-shot/thaw operators.  The audit found 19 new fields that needed
addition (protocol registry, GenServer pre-interned names, transducer
tag names, ref counter).  Missing any one would cause snap-shot/thaw to
produce incorrect results -- silently.

### 9.6 Tests Are Not Optional

Every operator has happy-path, sad-path, stress, and boundary tests.
The 6 composability features have 324 dedicated assertions across 6 test
files.  The full test suite is 20,200+ assertions across 270 test files.  The
test suite is the specification: if a behavior is not tested, it is not
guaranteed.

Test structure for each feature follows the same pattern:
1. **Happy path**: Normal usage, expected outputs.
2. **Sad path**: Error conditions, expected error codes.
3. **Stress**: Many operations, large inputs, rapid sequences.
4. **Boundary**: Edge cases (empty inputs, single elements, exact limits).
5. **Integrity**: `vm-validate` and `extvalue-validate` at the end.


## 10. The Complete Stack: A Worked Example

Putting it all together -- a realistic Trix application that uses every
layer of the composition pyramid:

```
% =============================================
% Application: Supervised data processing service
% =============================================

% --- Types ---
/Measurement [/sensor /value /timestamp] record-type def

% --- Protocols ---
% Open dispatch: any type can register a /validate and /normalize impl
[/validate /normalize] /DataProto def-protocol

% Record implementation: validate range, normalize to 0-1
{
    [/value] destructure
        value 0.0 ge value 1000.0 lt and   % returns a single bool
    end
} /validate /record-type def-method

{ /value get 1000.0 div } /normalize /record-type def-method

% Default: unknown types are invalid
{ pop false } /validate def-default-method
{ pop 0.0 }  /normalize def-default-method

% --- Transducer ---
% Reusable transformation: filter valid measurements, then normalize.
% xf-filter/xf-map steps are `elem -- bool` / `elem -- elem'`: the filter
% predicate consumes the element and pushes only the bool (the step keeps
% the element internally), so the predicate is `{ validate }`, not
% `{ dup validate }`.
/process-xf
    { validate }  xf-filter                 % keep only valid measurements
    { normalize } xf-map xf-compose         % normalize the survivors
def

% --- GenServer ---
% Stateful service: accumulates processed results, answers queries
/processor-spec <<
    /init { << /results [] /processed 0 >> }

    % handle-call ends with the op stack reading  new-state reply-value /reply.
    % All messages here are tagged, so each match test consumes the value with
    % `tag-name` and pushes a bool; the body pops the value match re-pushes and
    % works through the `msg` / `state` locals.
    /handle-call {
        [/msg /from /state] let
            msg [
                { tag-name /batch eq } {
                    pop
                    msg tag-value process-xf into        % new-vals
                    state /processed                     % processed += count(new-vals)
                        state /processed get  2 index length add
                    put
                    state /results                       % results ++= new-vals
                        rot  state /results get  { append } reduce
                    put
                    state state /reply                   % reply with new state
                }
                { tag-name /query eq } {
                    pop
                    state state /reply                   % return current state
                }
                { tag-name /reset eq } {
                    pop
                    << /results [] /processed 0 >> dup /reply   % reply fresh state
                }
                { pop true } {
                    pop
                    state /unknown /reply                % unrecognized message
                }
            ] match
        end
    }

    /handle-cast {
        % Fire-and-forget: process a single measurement.  handle-cast is
        % `msg state -- state'` -- it returns the new state, no reply.
        [/msg /state] let
            msg validate {
                state /results                          % append normalized value
                    msg normalize  state /results get  exch append
                put
                state /processed
                    state /processed get 1 add
                put
                state
            } {
                state                                   % invalid: skip
            } if-else
        end
    }

    /terminate {
        pop pop                                     % cleanup (log, flush, etc.)
    }
>> def

% --- Launch ---
% gen-call needs a mailbox on the caller, so drive the server from an actor.
mark {
    processor-spec gen-server /worker exch def

    (s1) 500.0  1000l Measurement /m1 exch def
    (s2) 9000.0 2000l Measurement /m2 exch def   % out of range -> filtered
    (s3) 250.0  3000l Measurement /m3 exch def

    % A query payload is null tagged /query; a batch carries the array.
    worker [ m1 m2 m3 ] /batch tag gen-call /processed get % => 2 (2 of 3 valid)
    worker [ m1 m3 ]    /batch tag gen-call /results get   % => [0.5 0.25 0.5 0.25]
    worker null /query  tag gen-call /processed get        % => 4
    worker null /reset  tag gen-call /processed get        % => 0
    worker null /bogus  tag gen-call                       % => /unknown

    % Graceful shutdown
    worker /done gen-stop
    100 coroutine-sleep
} actor-spawn coroutine-join pop
```

This application uses:
- **Records** for structured measurement data
- **Protocols** for open-ended validation and normalization dispatch
- **Default methods** for fallback handling of unknown types
- **Transducers** for the reusable processing pipeline
- **Pattern matching** (`let` for argument binding, `match` for message dispatch)
- **Tagged values** for message categorization
- **GenServer** for stateful request/reply and fire-and-forget processing
- **Contracts** (would add `precondition` in handlers for production)

And it integrates with:
- **Supervision** (spawn under a supervisor for fault tolerance)
- **Snap-shot/thaw** (periodic snapshots for crash recovery)
- **Reactive cells** (could make results a cell for derived computations)

All within a 256KB heap.  No external frameworks.  No runtime dependencies.
One `#include`.


## 11. Documentation Map

### VM & Internals

| Document            | Covers                                    |
| ------------------- | ----------------------------------------- |
| `trix-reference.md` | Complete operator reference (all 838 ops) |
| `vm-internals.md`   | Heap, allocation, offset addressing       |
| `interpreter.md`    | Exec stack dispatch, operator execution   |
| `scanner.md`        | Tokenizer, binary tokens                  |
| `scanner-syntax.md` | Full syntax reference                     |
| `type-system.md`    | 32 types, Object encoding, verify_t       |
| `name-lookup.md`    | Name resolution, dict stack search        |
| `save-restore.md`   | Transaction journaling                    |
| `snapshot-thaw.md`  | Full state serialization                  |
| `binary-pack.md`    | Packed array compression                  |

### Data Types

| Document               | Covers                           |
| ---------------------- | -------------------------------- |
| `collections.md`       | Arrays, dicts, sets              |
| `record.md`            | Immutable named-field composites |
| `tagged-values.md`     | Discriminated unions (ADTs)      |
| `string-processing.md` | String operations, regex         |
| `ieee754.md`           | Floating point handling          |

### Functional

| Document                    | Covers                               |
| --------------------------- | ------------------------------------ |
| `functional-programming.md` | Map, filter, reduce, curry, compose  |
| `curry-thunk.md`            | Partial application, lazy evaluation |
| `lazy-sequences.md`         | Infinite sequences, demand-driven    |
| `transducers.md`            | Composable transformations           |
| `closures.md`               | Explicit capture, scope control      |

### Concurrent

| Document                                | Covers                                      |
| --------------------------------------- | ------------------------------------------- |
| `coroutines.md`                         | Cooperative multitasking                    |
| `pipeline.md` / `pipeline-internals.md` | Linear dataflow, SPSC buffers               |
| `actors.md`                             | Message passing, mailboxes                  |
| `supervision.md`                        | Fault tolerance, restart strategies         |
| `reactive.md`                           | Cells, dependency tracking                  |
| `logic.md`                              | Backtracking, unification                   |
| `continuation.md`                       | Delimited continuations (one-shot)          |
| `effects.md`                            | Algebraic effects (handle-effect / perform) |

### Composability

| Document              | Covers                              |
| --------------------- | ----------------------------------- |
| `protocols.md`        | Type-dispatched polymorphism        |
| `pattern-matching.md` | let, destructure, match, cond, when |
| `genserver.md`        | Standardized actor patterns         |
| `closures.md`         | Explicit capture, scope control     |
| `contracts.md`        | Preconditions, postconditions       |

### System & Developer

| Document                    | Covers                           |
| --------------------------- | -------------------------------- |
| `error-handling.md`         | Error codes, try/catch, recovery |
| `tail-call-optimization.md` | TCO in the interpreter           |
| `streams-io.md`             | I/O abstraction                  |
| `host-integration.md`       | Embedding Trix in C++ programs   |
| `modules.md`                | Code organization                |
| `dev-adding-operators.md`   | How to add new operators         |
| `dev-invariants.md`         | Internal invariants              |
| `dev-glossary.md`           | Terminology                      |
