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

# Pipeline Library: Technical Reference

A comprehensive technical document for Trix maintainers and programmers
covering the architecture, implementation, usage patterns, costs, design
choices, and real-world scenarios of the concurrent pipeline library.

**Audience**: Trix maintainers (C++ implementation details) and Trix
programmers (usage patterns and performance characteristics).

**Prerequisite reading**: `docs/coroutines.md` (cooperative scheduling model),
`docs/vm-internals.md` (VM heap, Object representation).

---

## 1. Architectural Overview

The pipeline library is a concurrent dataflow system built on three
existing Trix primitives:

1. **Coroutines** -- cooperative multitasking (9 operators, stackful,
   round-robin scheduled, ~4.7 KB per coroutine)
2. **Lazy sequences** -- demand-driven iteration (48 operators, thunk-based)
3. **Bounded buffers** -- fixed-capacity circular queues with SPSC
   enforcement and targeted wakeup (new: Type::PipeBuffer)

A pipeline is a chain of stages connected by bounded buffers:

```
source -> [buf0] -> stage1 -> [buf1] -> stage2 -> [buf2] -> sink
   co0                co1                 co2               (caller)
```

Each stage runs in its own coroutine.  The sink runs inline in the
caller's coroutine -- no synchronization infrastructure needed.  The
caller blocks on `pipe-get` when waiting for data, yielding to the
scheduler so upstream stages run.

### 1.1 Execution Model

Pipeline execution proceeds in four phases:

1. **Construction** -- `pipe-map`, `pipe-filter`, etc. build a linked list
   of read-only descriptor dicts.  No coroutines, no buffers.  O(1) per
   stage.

2. **Materialization** -- A terminal operator (`pipe-run`, `pipe-collect`,
   `pipe-reduce`) walks the descriptor chain, allocates all buffers, and
   launches all stage coroutines.

3. **Streaming** -- The scheduler round-robins between stage coroutines.
   Data flows through buffers.  Backpressure throttles fast producers.

4. **Teardown** -- Cascading close: source exhausts, closes buf[0];
   stage 1 drains buf[0], closes buf[1]; cascade reaches sink.  All
   coroutines die naturally.  On error: error-close cascades instead.

### 1.2 Why Not Eager Arrays?

```
% Eager: allocates 3 intermediate arrays
data { f } map { p } filter { g } map

% Pipeline: streams one item at a time, bounded memory
data { f } pipe-map { p } pipe-filter { g } pipe-map 8 pipe-collect
```

The eager approach allocates O(N) per stage.  The pipeline approach
allocates O(capacity) total for all buffers, regardless of source size.
For a 1,000,000-item source with capacity 16: eager uses ~24 MB
(3 arrays); the pipeline uses ~640 bytes (4 buffers * 160 bytes).

### 1.3 Why Not Lazy Sequences Alone?

Lazy sequences avoid intermediate allocation but execute synchronously:
each `lazy-map` / `lazy-filter` call chains through the entire pipeline
for every element.  For I/O-bound stages (file reads, network), this
serializes all work.

Pipelines provide cooperative concurrency: while one stage is blocked on
a full buffer, another stage can run.  This overlaps computation across
stages when the scheduler time-slices between coroutines.

---

## 2. Type System: PipeBuffer

### 2.1 Object Representation

PipeBuffer occupies Type slot 25 (of 32).  The Object's 4-byte union
field stores a `vm_offset_t` pointing to a `PipeBufferHeader` on the
VM heap.

```
Object (8 bytes):
  m_aat:              PushOpDirect | IgnoresExecute | UsesVM | PipeBuffer
  m_object_save_level: current save level
  m_pipe_buffer:      vm_offset_t -> PipeBufferHeader
```

Attributes: `PushOpDirect | IgnoresExecute | UsesVM`.  This means:
- Inside a proc body, a PipeBuffer value is pushed to the operand
  stack as data (PushOpDirect).
- Via name lookup, a PipeBuffer value is pushed to the operand stack
  (IgnoresExecute) -- it never "executes".
- The VM heap holds the actual buffer data (UsesVM).

PipeBuffer is **not packable** (same as Coroutine and Thunk).  Attempting
to pack it raises `type-check`.

### 2.2 PipeBufferHeader Layout

```cpp
struct PipeBufferHeader {              // 32 bytes (Object-aligned)
    vm_offset_t m_data;                // offset to Object[capacity] array
    length_t    m_head;                // read index (circular)
    length_t    m_tail;                // write index (circular)
    length_t    m_count;               // current occupancy
    length_t    m_capacity;            // maximum items
    bool        m_closed;              // true when producer is done
    bool        m_consumer_dead;       // true once consumer has observed end
    Error       m_error;               // NoError or error code for cascade
    vm_offset_t m_producer;            // bound coroutine (nulloffset = unbound)
    vm_offset_t m_consumer;            // bound coroutine (nulloffset = unbound)
    vm_offset_t m_blocked_writer;      // sleeping coroutine waiting to put
    vm_offset_t m_blocked_reader;      // sleeping coroutine waiting to get
};
```

**VM allocation**: `PipeBufferHeader` and the `Object[capacity]` data array
are allocated as ONE contiguous `gvm_alloc` block of
`sizeof(PipeBufferHeader) + capacity*sizeof(Object)` bytes; the data array
immediately follows the header (`m_data = hdr_offset + sizeof(PipeBufferHeader)`).

**Circular buffer**: `m_head` is the read index, `m_tail` is the write
index.  Both advance modulo `m_capacity`.  `m_count` tracks occupancy
to distinguish full from empty (head == tail could be either).

### 2.3 SPSC Enforcement

Single-producer / single-consumer is enforced at runtime, not at the
type level.  The first coroutine to call `pipe-put` on a buffer binds
`m_producer` to its CoroutineContext offset.  Subsequent puts from a
different coroutine raise `/invalid-access`.  Same for `m_consumer` and
`pipe-get`.

**Design choice**: Runtime enforcement was chosen over static (type-level)
enforcement because Trix is dynamically typed and pipeline stages are
created dynamically.  The runtime check costs one comparison per put/get
-- negligible vs. the cost of the value transfer itself.

### 2.4 Targeted Wakeup

When `pipe-put` finds the buffer full:
1. Store `m_running_coroutine` in `m_blocked_writer`
2. Push `@pipe-put-retry` trampoline on exec stack
3. Set coroutine status to `Sleeping` with `wake_time = max`
4. Call `coroutine_schedule()` (loads another coroutine, returns)
5. Return immediately -- nothing after schedule() executes in this context

When `pipe-get` removes an item, it wakes the blocked writer:
```cpp
auto writer = hdr->m_blocked_writer;
hdr->m_blocked_writer = nulloffset;
trx->coroutine_wake(writer);   // sets status=Ready, enqueues on the ready queue
```

The woken coroutine resumes at `@pipe-put-retry`, which re-checks the
buffer and either completes the put or re-sleeps.

**No polling**: Each wakeup is O(1) targeted, not O(N) scheduler scan.
This is critical for pipeline throughput -- a scheduler scan would cost
O(stages) per item transferred.

### 2.5 Trampoline Pattern (Critical)

`coroutine_schedule()` does NOT do fiber/longjmp switching.  It overwrites
the Trix member variables (`m_op_ptr`, `m_exec_ptr`, etc.) with the loaded
coroutine's saved state and returns.  After schedule() returns, the C
function returns to the interpreter loop, which processes the newly-loaded
coroutine's exec stack.

**Consequence**: Any C++ code after `coroutine_schedule()` operates on the
WRONG coroutine's context.  Blocking operators MUST:

1. Push a continuation control operator onto the exec stack
2. `coroutine_flush_running()` -- saves current stacks
3. Set status to Sleeping, set wake_time
4. `coroutine_schedule()` -- loads another coroutine
5. **Return immediately**

When the sleeping coroutine is woken and rescheduled, the interpreter pops
the continuation from its exec stack and executes it.

See `@pipe-put-retry` and `@pipe-get-retry` for the canonical implementation.

---

## 3. Pipeline Construction

### 3.1 Descriptor Chain

Each construction operator builds a read-only dict and returns it:

```
[1 2 3] { 10 mul } pipe-map
% => << /type /pipe-map  /source [1 2 3]  /proc { 10 mul } >>
```

Chaining wraps the previous descriptor:

```
[1 2 3] { 10 mul } pipe-map { 5 gt } pipe-filter
% => << /type /pipe-filter
%       /source << /type /pipe-map  /source [1 2 3]  /proc { 10 mul } >>
%       /proc { 5 gt } >>
```

The descriptor is a singly-linked list read from outside-in.  The terminal
operator reverses it to get execution order (source to sink).

**Cost**: Each construction operator allocates a 3-entry `Dict` on the VM
heap (~80 bytes per stage) plus Name interning for the keys.  No coroutines,
no buffers, no scheduling.

### 3.2 Stage Types

| Type     | Operator        | Input:Output | Descriptor Fields                               |
| -------- | --------------- | :----------: | ----------------------------------------------- |
| Map      | `pipe-map`      |     1:1      | `/type /pipe-map`, `/source`, `/proc`           |
| Filter   | `pipe-filter`   |   1:0-or-1   | `/type /pipe-filter`, `/source`, `/proc`        |
| Flat-map | `pipe-flat-map` |     1:N      | `/type /pipe-flat-map`, `/source`, `/proc`      |
| Batch    | `pipe-batch`    |     N:1      | `/type /pipe-batch`, `/source`, `/n`            |
| Take     | `pipe-take`     |     1:1      | `/type /pipe-take`, `/source`, `/n`             |
| Tap      | `pipe-tap`      |     1:1      | `/type /pipe-tap`, `/source`, `/proc`           |
| Drop     | `pipe-drop`     |     1:1      | `/type /pipe-drop`, `/source`, `/n`             |
| Distinct | `pipe-distinct` |   1:0-or-1   | `/type /pipe-distinct`, `/source`               |
| Scan     | `pipe-scan`     |     1:1      | `/type /pipe-scan`, `/source`, `/proc`, `/init` |
| Window   | `pipe-window`   |     N:1      | `/type /pipe-window`, `/source`, `/n`           |
| Merge    | `pipe-merge`    |     N:1      | `/type /pipe-merge`, `/sources`                 |
| Zip      | `pipe-zip`      |     N:1      | `/type /pipe-zip`, `/sources`                   |

### 3.3 Proc Requirements

All procs (`pipe-map`, `pipe-filter`, `pipe-flat-map`, `pipe-tap`) must be
executable: an Array, Packed, or Curry with the executable attribute set.
Literal procs or non-proc types raise `/type-check`.

Stage proc contracts:

- **map**: `value -- result`.  Consumes value, pushes one result.
- **filter**: `value -- bool`.  Consumes value, pushes true/false.
- **flat-map**: `value -- array-or-packed`.  Must return a container.
  Each element is emitted separately.  Returning an empty container
  emits nothing (acts as combined map+filter).
- **tap**: `value -- any`.  Receives a COPY of the value.  Return value
  is discarded; the original passes through.  Do not pop the value.
- **batch**: No proc.  Collects `n` items into an array.

---

## 4. Pipeline Materialization

### 4.1 Chain Walking

`pipe_walk_chain()` traverses the descriptor linked list from outermost
to root source, collecting `PipeStageInfo` structs.  It reverses them
into execution order (source-to-sink).  Maximum 32 stages (`MaxPipeStages`).

The root source is whatever is NOT a pipeline descriptor dict -- typically
an Array, Packed, or lazy-seq node.

**Ordering constraint**: Lazy-seq detection must happen BEFORE array
detection because a lazy-seq node IS a 2-element array (with a thunk at
index 1).  The check order in `pipe_materialize()`:

```cpp
if (root_source.is_lazy_seq(trx)) { ... }
else if (root_source.is_sequence()) { ... }
else { error(...); }
```

### 4.2 Buffer Allocation

For N mid-stages, the pipeline allocates N+1 buffers:

```
buf[0]: source -> stage[0]
buf[1]: stage[0] -> stage[1]
...
buf[N]: stage[N-1] -> sink
```

All buffers share the same capacity (the `cap` argument to the terminal
operator).  Each buffer costs:

```
PipeBufferHeader:  32 bytes
Object[capacity]:  capacity * 8 bytes
Total per buffer:  32 + (capacity * 8) bytes
```

### 4.3 Coroutine Creation

Stage coroutines are created directly (not via `coroutine-launch`) with
custom exec stack layouts.  Each stage coroutine's exec stack:

<!-- doctest: skip (exec-stack layout diagram, not runnable code) -->
```
bottom -> top:
[@coroutine-complete]     % standard coroutine cleanup on death
[output-buf]              % companion for @pipe-cleanup
[@pipe-cleanup]           % error propagation handler
[@try-barrier]            % catches errors from stage proc
[stage-proc]              % the Trix proc for this stage
```

The `@try-barrier` catches any error from the stage proc.  On normal
completion, it pushes `/no-error`.  On error, it pushes the error name.
Then `@pipe-cleanup` runs: if the error name is not `/no-error`, it
error-closes the downstream buffer.  Then `@coroutine-complete` kills
the coroutine.

### 4.4 Stage Proc Construction

Stage procs are built from C++ as executable VM arrays using
`Object::make_operator()`.  This embeds Operator Objects directly in
the proc body, bypassing dict name lookup at runtime.

Example: the map stage proc is equivalent to:

```
{ { input-buf pipe-get
    { user-proc exec output-buf pipe-put }
    { output-buf pipe-close exit }
    if-else } loop }
```

Built in C++ as a nested array structure:

<!-- doctest: skip (C++ array-structure pseudo-code illustration) -->
```
outer = [body-proc, loop-op]
body  = [input-buf, pipe-get-op, true-proc, false-proc, if-else-op]
true  = [user-proc, exec-op, output-buf, pipe-put-op]
false = [output-buf, pipe-close-op, exit-op]
```

**Key rules for proc construction**:
- Data values (buffers, integers): call `set_literal()` so they are
  pushed to the operand stack.
- Operator Objects: leave as executable (no PushOpDirect flag) so they
  execute when encountered in the proc body.
- Inner procs (executable Arrays): leave as executable.  They have
  PushOpDirect, so `execute_proc` pushes them to the operand stack
  regardless.  Operators like `loop`, `if`, `if-else`, `exec`, `for-all`
  require executable procs -- calling `set_literal()` on them breaks
  these operators' type checks.

### 4.5 Lazy-Seq Source Handling

The lazy-seq source feeder is the most complex source proc.  It expects
the lazy-seq node to be pre-loaded on the coroutine's operand stack
(via `pipe_create_lazy_source_coroutine`).

The proc iterates using `lazy-head` / `lazy-tail`:

```
% lazy-seq node on op stack
{ dup null eq { pop output-buf pipe-close exit } if
  dup lazy-head output-buf pipe-put lazy-tail } loop
```

`lazy-tail` calls `force`, which may push `@force-complete` and the
thunk's evaluation proc onto the exec stack.  The interpreter handles
this transparently -- the forced result replaces the thunk on the op
stack, and the loop body continues with the next element.

### 4.6 Batch Stage (@pipe-stage)

The batch stage uses a C++ control operator (`@pipe-stage`) rather than
a Trix proc because batch accumulation requires mutable state (a
partially-filled array and a write index) that would require complex
stack manipulation in Trix.

The control operator is a 5-phase state machine with 6 exec-stack
companions:

```
Exec stack: [input-buf] [output-buf] [batch-arr-offset]
            [batch-idx] [batch-size] [phase] [@pipe-stage]
```

| Phase | Name      | Action                                                            |
| ----- | --------- | ----------------------------------------------------------------- |
| 0     | GET       | Push input-buf, push pipe-get operator, advance to phase 1        |
| 1     | AFTER_GET | Check pipe-get result: data -> accumulate; EOS -> flush           |
| 2     | AFTER_PUT | Full batch was pipe-put; allocate new array, reset index, go to 0 |
| 3     | FLUSH_PUT | Partial batch was pipe-put at EOS; close output buffer            |
| 4     | CLOSE     | Close output buffer, pop all companions                           |

The batch array is allocated on the VM heap.  When a full batch is emitted
via `pipe-put`, a new array is allocated for the next batch.  The old
array is now owned by the downstream consumer.

---

## 5. Error Handling

### 5.1 Error Containment

Each stage coroutine's exec stack includes a `@try-barrier` that catches
any error from the stage proc.  Without this barrier, an unhandled error
in a stage coroutine would reach the global error handler and potentially
terminate the interpreter.

### 5.2 Error Propagation (Cascade)

When stage K errors:

```
1. Stage K's proc throws (e.g., user proc divides by zero)
2. @try-barrier catches, pushes /error-name on op stack
3. @pipe-cleanup reads /error-name, sees it != /no-error
4. @pipe-cleanup error-closes buf[K] with trx->m_last_error
5. Stage K's coroutine dies via @coroutine-complete
6. Stage K+1 calls pipe-get on buf[K]
7. pipe-get sees closed + drained + error -> re-raises the stored error
8. Stage K+1's @try-barrier catches, cascade continues...
9. Eventually the sink's pipe-get re-raises the error to the caller
```

The caller can catch pipeline errors with `try`:

```
{ data { risky-proc } pipe-map 4 pipe-collect } try
% => /error-name or /no-error
```

### 5.3 Error Code Storage

`@pipe-cleanup` uses `trx->m_last_error` (the Error enum value from the
most recent error) to set the buffer's `m_error` field.  This works
because no other error can occur between the @try-barrier firing and
@pipe-cleanup executing -- the interpreter resumes directly at @pipe-cleanup.

### 5.4 Upstream Stages After Error

When an error cascades downstream, upstream stages may still be alive and
blocked on full buffers.  Cascading close handles most cleanup, but the
terminal operator does not rely on it alone: the sink proc is wrapped by
`pipe_build_cleanup_wrapper`, which unconditionally kills **every** pipeline
coroutine once the sink finishes — on success or error — so no zombie upstream
stages remain.  See §9.5 for the mechanism.

---

## 6. Terminal Operators

### 6.1 pipe-run

```
source capacity proc --
```

Builds and pushes a sink loop proc onto the exec stack:

```
{ { sink-buf pipe-get { user-proc exec } { exit } if-else } loop }
```

The interpreter executes this proc in the caller's coroutine.  Each
`pipe-get` may block (yielding to the scheduler for upstream stages to
run), then the user proc processes the item.

**Stack discipline**: The user proc receives one value on the operand
stack per iteration and should consume it.  The operand stack is clean
between iterations.

### 6.2 pipe-collect

```
source capacity -- array
```

Builds a collection proc:

```
{ mark { sink-buf pipe-get { } { exit } if-else } loop array-from-mark }
```

Pushes a mark, loops pipe-get (each item accumulates above the mark),
then `array-from-mark` collects all items into an array.

**Memory note**: All items accumulate on the operand stack between the
mark and array creation.  For large collections, this uses
`O(item-count)` operand stack slots.  The operand stack depth is 1024
for the main coroutine.

### 6.3 pipe-reduce

```
source capacity init proc -- value
```

Builds a fold proc:

```
{ init { sink-buf pipe-get { user-proc exec } { exit } if-else } loop }
```

The `init` value is pushed first as the initial accumulator.  Each
iteration: accumulator is on the stack, `pipe-get` pushes a value on top,
then `proc` consumes both and pushes the new accumulator.

**Stack discipline**: `proc` receives `(acc value)` and must push exactly
one result.  After the loop, the final accumulator is on the stack.

If the source is empty, `init` remains on the stack (the loop body never
executes).

---

## 7. Usage Patterns

### 7.1 ETL (Extract-Transform-Load)

<!-- doctest: skip (placeholder sensor-reading / write-alert-batch procs) -->
```
% Extract sensor data from a lazy stream
0 { dup 1000 lt } { |i|
    i i 100 mod 25 add /celsius clock
    sensor-reading
    i 1 add
} lazy-unfold

% Transform: Celsius to Fahrenheit, filter alerts
    { |r| r /temp get 9 mul 5 div 32 add
          r /temp rot record-update } pipe-map
    { /temp get 95.0 gt } pipe-filter

% Load: batch and write
    10 pipe-batch
    16 { write-alert-batch } pipe-run
```

**Cost**: 3 stages = 4 buffers (cap 16 = 640 bytes) + 4 coroutines
(source feeder + 3 stages, ~18.4 KB) + lazy-seq overhead (~120 bytes for
thunk chain).  Total ~19.2 KB regardless of how many sensor readings are
processed.

### 7.2 Log Processing

<!-- doctest: skip (external log file + placeholder parse-log-line / unique) -->
```
% Read log file line by line as a lazy-seq
(server.log) (r) stream
    { read-line dup null ne } lazy-unfold

% Parse, filter errors, extract module names
    { parse-log-line } pipe-map
    { /level get (ERROR) eq } pipe-filter
    { /module get } pipe-map

% Collect unique error modules
    8 pipe-collect
    unique
```

### 7.3 Map-Reduce

```
% Sum of squares of even numbers in 1..3000.
% The running total (4,504,501,000) overflows a 32-bit Integer, so we
% square in Long arithmetic (cast each value to /long-type and seed the
% reduce with the Long literal 0l).  Without the cast the reduce raises
% /numerical-overflow.
1 { 1 add } lazy-iterate 3000 lazy-take
    { 2 mod 0 eq } pipe-filter
    { /long-type cast dup mul } pipe-map
    32 0l { add } pipe-reduce
% => 4504501000
```

**Capacity choice**: 32 is chosen because the filter passes ~50% of items.
A capacity of 32 means the source can run ahead by 32 items before the
filter needs to process them.  This amortizes scheduler overhead.

### 7.4 Flat-Map: Expanding Records

<!-- doctest: skip (placeholder orders source) -->
```
% Each order has multiple line items
orders
    { /line-items get } pipe-flat-map
    { /amount get } pipe-map
    16 0 { add } pipe-reduce
% => total amount across all line items
```

### 7.5 Batch Processing

<!-- doctest: skip (placeholder records source / db-bulk-insert) -->
```
% Insert records in batches of 100
records
    100 pipe-batch
    { |batch| batch db-bulk-insert } pipe-tap
    8 0 { length add } pipe-reduce
% => total records inserted
```

**Why batch + tap**: `pipe-tap` receives each batch, performs the side
effect (bulk insert), and passes the batch through.  `pipe-reduce` then
counts the total items.

### 7.6 Multi-Stage Validation Pipeline

<!-- doctest: skip (placeholder raw-input source and validate/normalize/enrich procs) -->
```
% Validate, normalize, enrich, then collect
raw-input
    { validate } pipe-map               % returns /ok or /err tagged
    { untag /ok eq } pipe-filter        % keep only valid items
    { normalize-record } pipe-map       % standardize fields
    { enrich-from-cache } pipe-map      % add derived fields
    16 pipe-collect
```

### 7.7 Direct Source (No Stages)

A terminal operator can be called with just a source and no construction
operators:

```
[1 2 3 4 5] 4 pipe-collect             % => [1 2 3 4 5]
[1 2 3 4 5] 4 0 { add } pipe-reduce    % => 15
```

This creates one source-feeder coroutine and one buffer.  Useful when the
source is a lazy-seq and you want pipeline-style blocking semantics.

---

## 8. Costs and Limits

### 8.1 VM Memory Budget

| Component                      | Size                                          |
| ------------------------------ | --------------------------------------------- |
| PipeBufferHeader               | 32 bytes                                      |
| Buffer data (per buffer)       | capacity * 8 bytes                            |
| Coroutine context              | 232 bytes                                     |
| Coroutine stacks               | (128 + 256 + 16 + 32 + 128) * 8 = 4,480 bytes |
| Stage proc (VM array)          | ~40-120 bytes (varies by stage type)          |
| Descriptor dict (construction) | ~80 bytes per stage                           |

**Per-pipeline formula** (S mid-stages, capacity C):

```
buffers:    (S + 1) * (32 + C * 8)
coroutines: (S + 1) * (232 + 4480)
procs:      (S + 1) * ~80
total:      (S + 1) * (4824 + C * 8)
```

Example: 3-stage pipeline, capacity 16 = 4 * (4824 + 128) = ~20 KB.

### 8.2 Operand Stack Budget (pipe-collect)

`pipe-collect` accumulates all items on the operand stack between a mark
and `array-from-mark`.  The main coroutine operand stack has 1024 slots.
Subtracting the mark and overhead, `pipe-collect` can collect approximately
**1000 items** before overflow.

For larger collections, use `pipe-run` with explicit array building or
`pipe-reduce` with an accumulator.

### 8.3 Exec Stack Budget (per coroutine)

Each coroutine has an exec stack of 256 slots (128 is the operand depth).  The pipeline framework
uses 4-5 slots (try-barrier, cleanup, coroutine-complete, stage proc
frame).  The remaining ~251 slots are available for user proc execution.
Deeply nested user procs may exhaust this.

### 8.4 Stage Count Limit

`MaxPipeStages = 32`.  This is a compile-time constant for the C-stack
arrays used during materialization.  Exceeding it raises `/limit-check`.

### 8.5 Capacity Selection Guide

| Scenario               | Recommended Capacity | Rationale                                  |
| ---------------------- | -------------------- | ------------------------------------------ |
| Small data, quick test | 4                    | Minimal memory                             |
| Balanced throughput    | 16-32                | Good amortization of scheduler overhead    |
| CPU-bound stages       | 32-64                | Larger batches reduce context-switch ratio |
| Memory-constrained     | 2-4                  | Minimum viable backpressure                |
| I/O-interleaved        | 8-16                 | Enough slack for I/O latency hiding        |

**Rule of thumb**: Start with 16.  Increase if profiling shows the
scheduler is a bottleneck (stages frequently block on single-item
transfers).

---

## 9. Design Decisions

### 9.1 Sink-in-Caller

The sink does NOT run in its own coroutine.  The caller's coroutine IS
the sink -- it calls `pipe-get` in a loop on the last buffer.

**Rationale**: No synchronization infrastructure needed.  The caller
naturally receives completion and error signals through the buffer.  One
fewer coroutine per pipeline (~4.7 KB savings).  Backpressure works
correctly: `pipe-get` blocks when the buffer is empty, yielding to the
scheduler.

### 9.2 Trix Procs vs. C++ Control Operators

Five stage types (map, filter, flat-map, tap, scan) use Trix procs built
from C++.  Seven (batch, take, drop, distinct, window, merge, zip) use the
C++ control operator (@pipe-stage).

**Rationale**: Trix procs leverage existing interpreter machinery
(exec stack management, error handling, operator dispatch) with zero
new control flow.  The `exit` operator exits the loop cleanly; the
`@try-barrier` catches errors; `pipe-get` and `pipe-put` handle blocking
via their own trampolines.  Building the procs from `Object::make_operator()`
embeds Operator Objects directly, skipping dict name lookup.

Batch requires mutable state (partially-filled array + write index) that
would need complex stack manipulation in a Trix proc.  The C++ control
operator manages this state in exec-stack companions.

### 9.3 Uniform Buffer Capacity

All buffers in a pipeline share the same capacity (the single `cap`
argument to the terminal operator).

**Rationale**: Simplicity.  Per-buffer capacity would require either
per-stage arguments (complicating the API) or descriptor annotations
(complicating construction).  In practice, uniform capacity works well:
all stages process the same item stream, so their throughput is correlated.

### 9.4 Error Propagation via Buffers

Errors propagate downstream through buffer error fields, stage by stage.
An error in stage K error-closes buf[K]; stage K+1 reads the error from
buf[K] and re-raises it; its @pipe-cleanup error-closes buf[K+1]; and so
on until the sink.

**Alternative considered**: Out-of-band error channel (global flag or
shared error object).  Rejected because it requires synchronization and
breaks the clean buffer-based flow model.  Buffer-level error propagation
is lock-free, deterministic, and piggybacks on existing `pipe-get`
semantics.

### 9.5 Kill-All on Completion

The terminal operators (`pipe-run`, `pipe-collect`, `pipe-reduce`) wrap their
sink proc with `pipe_build_cleanup_wrapper`, which runs the sink under `try`,
then unconditionally kills **every** pipeline coroutine (on success *or* error)
via `coroutine-kill`, and re-throws any caught error.  Each coroutine handle is
baked as a literal into the wrapper array, so no zombie upstream stages remain
after the pipeline finishes -- whether it completed normally or errored.

---

## 10. Implementation Files

| File | Contents |
| --- | --- |
| `src/ops_pipeline.inl` | All pipeline C++: PipeBufferHeader, buffer primitives, construction ops, proc builders, materialization, terminal ops, @pipe-stage, @pipe-cleanup |
| `src/enums.inl` | SystemName entries: 23 standard ops (incl. `pipe-into`) + 4 control ops (PipePutRetry, PipeGetRetry, PipeStage, PipeCleanup) |
| `src/object.inl` | Type::PipeBuffer (slot 25), make_pipe_buffer, is_pipe_buffer, type switch cases |
| `src/dispatch.inl` | Operator dispatch entries |
| `src/verify.inl` | VerifyPipeBuffer |
| `src/types.inl` | Error::InvalidAccess |
| `tests/test_pipeline.trx` | 403 assertions, 123 sections |
| `docs/pipeline.md` | API reference |

---

## 11. Extending the Pipeline Library

### 11.1 Adding a New Stage Type

1. Add a construction operator in `ops_pipeline.inl` (follow `pipe_map_op`)
2. Add a `pipe_build_*_proc()` function (or use @pipe-stage for stateful stages)
3. Add `PipeStage*` constant and handle it in `pipe_materialize()`
4. Add the type string to `pipe_walk_chain()` switch
5. Add SystemName entries, dispatch entries
6. Test: happy path, error propagation, empty source, boundary cases

### 11.2 Adding a New Terminal Operator

1. Add the operator in `ops_pipeline.inl` (follow `pipe_run_op`)
2. Call `pipe_materialize()` to set up the pipeline
3. Build a sink proc or loop that reads from `pipeline.sink_buf`
4. Push the sink proc on the exec stack
5. Add SystemName, dispatch, tests

### 11.3 Adding a New Source Type

1. Add detection in `pipe_materialize()` (BEFORE array check if the type
   could be confused with an array)
2. Add a `pipe_build_*_source_proc()` function
3. Add a coroutine creation variant if the source needs pre-loaded state

---

## 12. Testing Methodology

The test suite (`tests/test_pipeline.trx`) follows the 4-tier coverage
model:

| Tier | Sections | Focus |
| --- | --- | --- |
| Happy path | 1-5, 11, 14-21, 23, 34-35, 37 | Basic functionality, all stage types, all terminals |
| Sad/Error | 3, 8-10, 25-29 | Type checks, range checks, error cascade, invalid sources |
| Boundary | 12, 22-23, 30-33, 38 | Capacity 1, empty source, single item, batch edge cases, filter all/none |
| Stress | 13, 24, 36, 39-40 | 100/1000 items, 5-stage deep chain, type diversity, batch backpressure |

Key scenarios:
- Error cascade through 3 mid-stages (section 27)
- Lazy-seq source with map, filter, reduce (section 37)
- Mixed value types: integer, string, boolean, name (section 34)
- Batch: exact multiple, size 1, size > count, empty (section 30)
- Filter: all filtered (empty result), none filtered (section 31)
- Reduce: empty source returns init, single item (section 32)
