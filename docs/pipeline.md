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

# Pipeline Library Reference

Concurrent pipeline / dataflow library built on coroutines and bounded
buffers.  Compose lazy sequences, records, and functions into declarative
concurrent data processing pipelines.

---

## 1. Overview

A pipeline is a chain of stages connected by bounded buffers.  Data flows
from a source through zero or more transformation stages to a sink.

```
source -> [buffer] -> stage1 -> [buffer] -> stage2 -> [buffer] -> sink
   co0       buf0        co1       buf1        co2       buf2    (caller)
```

Each stage runs in its own coroutine.  The sink runs inline in the caller's
coroutine.  Buffers have fixed capacity; producers block when the consumer
falls behind (backpressure).

### What it looks like

```
% Three-stage concurrent pipeline
[1 2 3 4 5 6 7 8 9 10]
    { 2 mul } pipe-map              % double each
    { 10 gt } pipe-filter           % keep > 10
    3 pipe-batch                    % group into batches of 3
    8 { = } pipe-run                % print each batch
```

Each `pipe-*` construction operator wraps the previous source in a new
stage descriptor (a read-only dict).  The terminal operator (`pipe-run`,
`pipe-collect`, or `pipe-reduce`) materializes the pipeline: allocates
buffers, launches coroutines, and runs the sink.

---

## 2. Source Types

The root source of a pipeline can be:

| Type          | Description                                                       |
| ------------- | ----------------------------------------------------------------- |
| Array         | Iterates elements via `for-all`                                   |
| Packed        | Iterates elements via `for-all`                                   |
| Lazy sequence | Iterates via `lazy-head` / `lazy-tail`; supports infinite sources |
| `null`        | Empty lazy sequence; produces zero items                          |

```
% Array source
[1 2 3 4 5] { 1 add } pipe-map 4 pipe-collect

% Lazy-seq source (infinite, but filter+collect is finite due to pipeline)
1 { 1 add } lazy-iterate 100 lazy-take
    { 2 mod 0 eq } pipe-filter
    8 pipe-collect
```

---

## 3. Stage Types

### 3.1 pipe-map

```
source proc -- descriptor
```

1:1 transform.  Calls `proc` on each item; the return value is the output.

```
[1 2 3] { 10 mul } pipe-map 4 pipe-collect   % => [10 20 30]
```

### 3.2 pipe-filter

```
source pred -- descriptor
```

1:0-or-1 predicate filter.  Calls `pred` on each item; `true` passes the
item through, `false` drops it.

```
[1 2 3 4 5] { 2 mod 0 eq } pipe-filter 4 pipe-collect   % => [2 4]
```

### 3.3 pipe-flat-map

```
source proc -- descriptor
```

1:N expansion.  Calls `proc` on each item; `proc` must return an array or
packed array.  Each element of the result is emitted separately.

```
[1 2 3] { dup mark exch dup array-from-mark } pipe-flat-map 4 pipe-collect
% => [1 1 2 2 3 3]
```

Returning an empty array produces zero output items for that input (acts as
a combined map+filter).

### 3.4 pipe-batch

```
source n -- descriptor
```

N:1 batching.  Collects `n` items into an array, then emits the array.
The last batch may be shorter than `n` if the source is not an exact
multiple.

```
[1 2 3 4 5 6 7] 3 pipe-batch 4 pipe-collect
% => [[1 2 3] [4 5 6] [7]]
```

### 3.5 pipe-take

```
source n -- descriptor
```

Pass the first N items through, then close downstream.  Useful for
limiting infinite sources (e.g., lazy-seq) or truncating long pipelines.
N must be a positive integer.

```
1 { 1 add } lazy-iterate 5 pipe-take 4 pipe-collect
% => [1 2 3 4 5]

[1 2 3 4 5 6 7 8 9 10] { 2 mul } pipe-map 3 pipe-take 4 pipe-collect
% => [2 4 6]
```

### 3.6 pipe-tap

```
source proc -- descriptor
```

1:1 passthrough with side effect.  Calls `proc` with a copy of each item
for its side effect; the original item passes through unchanged.  The
return value of `proc` is discarded.

```
/cnt 0 def
[1 2 3] { /cnt cnt 1 add def } pipe-tap 4 pipe-collect
% cnt = 3, result = [1 2 3]
```

### 3.7 pipe-drop

```
source n -- descriptor
```

Skip the first N items, then pass the rest through.  The mirror of
`pipe-take`.  N must be a positive integer.

```
[1 2 3 4 5] 2 pipe-drop 8 pipe-collect
% => [3 4 5]
```

### 3.8 pipe-distinct

```
source -- descriptor
```

Drop duplicate items via a seen-set, emitting only the first occurrence of
each distinct value (global dedup across the whole stream, not just
consecutive runs).

```
[1 1 2 2 3 3] pipe-distinct 8 pipe-collect
% => [1 2 3]
```

### 3.9 pipe-scan

```
source init proc -- descriptor
```

Running fold.  Threads an accumulator (starting at `init`) through `proc`
(`acc item -- acc'`) and emits the accumulator after each item, so the
output is the sequence of intermediate results.

```
[1 2 3 4] 0 { add } pipe-scan 8 pipe-collect
% => [1 3 6 10]   (running totals)
```

### 3.10 pipe-window

```
source n -- descriptor
```

Sliding window of width N.  Emits each consecutive run of N items as an
array, advancing one item at a time.  N must be a positive integer.

```
[1 2 3 4 5] 2 pipe-window 8 pipe-collect
% => [[1 2] [2 3] [3 4] [4 5]]
```

### 3.11 pipe-merge

```
[sources] -- descriptor
```

Combine two or more sources by round-robin interleaving.  Takes an array
of at least two sources and emits one item from each in turn.

```
[ [1 3 5] [2 4 6] ] pipe-merge 8 pipe-collect
% => [1 2 3 4 5 6]
```

### 3.12 pipe-zip

```
[sources] -- descriptor
```

Pairwise combination.  Takes an array of at least two sources and emits
one array per cycle holding the next item from each, stopping when any
source is exhausted.

```
[ [1 2 3] [10 20 30] ] pipe-zip 8 pipe-collect
% => [[1 10] [2 20] [3 30]]
```

Transducers can also be applied as a pipeline stage with `pipe-into`
(`pipe xf -- pipe'`), documented in `transducers.md`.

---

## 4. Terminal Operators

Terminal operators materialize the pipeline: walk the descriptor chain,
allocate bounded buffers, launch stage coroutines, and run the sink inline
in the caller's coroutine.

The `capacity` argument specifies the bounded buffer size for **all**
inter-stage buffers.  It must be a positive integer.

### 4.1 pipe-run

```
source capacity proc --
```

Executes the pipeline.  Calls `proc` on each item from the final stage.
Returns nothing.

```
[1 2 3] { 10 mul } pipe-map 4 { = } pipe-run
% prints: 10  20  30
```

### 4.2 pipe-collect

```
source capacity -- array
```

Executes the pipeline.  Collects all output items into an array.

```
[1 2 3] { 1 add } pipe-map 4 pipe-collect   % => [2 3 4]
```

### 4.3 pipe-reduce

```
source capacity init proc -- value
```

Executes the pipeline.  Folds all output items with `proc(acc, item)`.
Returns the final accumulator.  If the source is empty, returns `init`.

```
[1 2 3 4 5] 4 0 { add } pipe-reduce   % => 15
```

---

## 5. Buffer Primitives

Low-level bounded buffer operators for custom inter-coroutine
communication.  Used internally by the pipeline library; also available
for direct use.

### 5.1 pipe-buffer

```
capacity -- buffer
```

Allocates a PipeBufferHeader + Object[capacity] in the global VM region
(`ChunkKind::PipeBuffer`, VM Redux Phase 5 follow-up).  Capacity must be
a positive integer.  Returns a `pipebuffer-type` object.  The buffer
survives `save` / `restore` and is reclaimed by mark-sweep GC once
unreachable.

### 5.2 pipe-put

```
value buffer --
```

Writes `value` to the buffer's tail.  If the buffer is full, blocks the
current coroutine until space is available (targeted wakeup, not polling).
Raises `/invalid-access` if the buffer is closed.

Single-producer enforcement: the first coroutine to call `pipe-put` on a
buffer is bound as its producer.  Any other coroutine calling `pipe-put`
raises `/invalid-access`.

### 5.3 pipe-get

```
buffer -- value true | false
```

Reads from the buffer's head.  If data is available, pushes `value true`.
If the buffer is closed, drained, and has no error, pushes `false`
(end-of-stream).  If the buffer is closed with an error, re-raises the
stored error.

If the buffer is empty and not closed, blocks the current coroutine until
data arrives.

Single-consumer enforcement mirrors single-producer (see `pipe-put`).

### 5.4 pipe-close

```
buffer --
```

Sets the buffer's closed flag.  Wakes any blocked reader so it can see
end-of-stream.  Idempotent.

### 5.5 pipe-error-close

```
error-code buffer --
```

Sets the buffer's closed flag and stores the error code (an integer
corresponding to an Error enum ordinal).  Wakes both blocked reader and
writer.  Used for error propagation in pipelines.

### 5.6 pipe-status

```
buffer -- dict
```

Returns a read-only introspection dict for a live buffer with `/count`,
`/capacity`, `/closed`, `/error`, `/has-producer`, and `/has-consumer`.
Raises `invalid-access` on a recycled buffer (both sides already done).

```
4 pipe-buffer /buf exch def
buf pipe-status /capacity get      % => 4
42 buf pipe-put
buf pipe-status /count get         % => 1
buf pipe-close
```

### 5.7 is-pipebuffer

```
any -- bool
```

True if the value is a pipe buffer, false otherwise (mirrors `is-coroutine`).

```
4 pipe-buffer is-pipebuffer      % => true
5 is-pipebuffer                  % => false
```

---

## 6. Error Handling

### 6.1 Error Containment

Each stage coroutine wraps its work in a `@try-barrier`.  If the stage
proc raises an error, the barrier catches it.  A `@pipe-cleanup` control
operator then error-closes the stage's downstream buffer with the error
code.

### 6.2 Error Propagation

Errors propagate downstream through buffers, stage by stage:

1. Stage K errors; `@pipe-cleanup` error-closes buffer[K].
2. Stage K+1 calls `pipe-get` on buffer[K]; the stored error is re-raised.
3. Stage K+1's `@pipe-cleanup` error-closes buffer[K+1].
4. Cascade continues to the sink.
5. The terminal operator's `pipe-get` re-raises the error to the caller.

```
{ [1 2 3 4 5]
    { dup 3 eq { /div-by-zero throw } if } pipe-map
    4 pipe-collect
} try   % => /div-by-zero
```

### 6.3 SPSC Enforcement

Each buffer enforces single-producer / single-consumer at runtime.  The
first coroutine to `pipe-put` is bound as the producer; the first to
`pipe-get` is bound as the consumer.  Violations raise `/invalid-access`.

---

## 7. Descriptor Format

Construction operators build read-only dicts:

```
<< /type /pipe-map  /source <upstream>  /proc { ... } >>
<< /type /pipe-batch  /source <upstream>  /n 3 >>
```

Terminal operators walk the `/source` chain to find the root source, then
build the pipeline in forward order.

---

## 8. Memory Usage

Per pipeline with S transformation stages:
- S+1 bounded buffers (one per coroutine): `sizeof(PipeBufferHeader)`
  (32 bytes) + `capacity * 8` bytes each
- S+1 coroutines (one source feeder + one per stage): ~4.7 KB each
  (stacks + context)
- Sink runs in caller's coroutine (no extra allocation)

Example: 3-stage pipeline, capacity 16:
- 4 buffers: 4 * (32 + 128) = 640 bytes
- 4 coroutines: 4 * 4,712 = 18,848 bytes
- Total: ~19.5 KB

---

## 9. Composability Patterns

The six composability features -- protocols, closures, contracts, pattern
matching, transducers, and GenServer -- compose naturally with pipelines.

### 9.1 Closures as Stage Procs

Capture configuration in a closure, then use it as a pipeline stage proc.
The frozen threshold travels with the closure:

```
/threshold 10 def
{ threshold gt } [/threshold] closure-capture /above exch def
[5 15 3 20 8 12] { above } pipe-filter 8 pipe-collect
% => [15 20 12]
```

Changing `/threshold` later does not affect the closure -- the captured
value is frozen at capture time.

### 9.2 Protocol Methods in Stages

A protocol method works as a `pipe-map` transformation.  Define the
protocol once, register implementations per type, then use the method
name directly:

```
[/double-it] /Doubler def-protocol
{ 2 mul } /double-it /integer-type def-method

[1 2 3] { double-it } pipe-map 8 pipe-collect
% => [2 4 6]
```

### 9.3 Pattern Matching in Stages

Use `destructure` inside a `pipe-map` proc to extract fields from
structured items (arrays, records, dicts):

```
[[ 1 10 ] [ 2 20 ] [ 3 30 ]]
{ [/id /val] destructure  val id add end } pipe-map
8 pipe-collect
% => [11 22 33]
```

Each pipeline item is an array; `destructure` binds its elements to
names for readable computation.

### 9.4 Contracts via pipe-tap

Use `pipe-tap` with `precondition` to assert invariants at any point
in a pipeline without affecting the data flow:

```
[1 2 3 4 5]
{ dup 0 gt precondition } pipe-tap   % assert: all items positive
{ 2 mul } pipe-map
8 pipe-collect
% => [2 4 6 8 10]
```

If any item violates the precondition, the pipeline aborts with
`/require`.  The tap does not modify items -- they pass through unchanged.

### 9.5 GenServer + Pipelines

A pipeline's terminal stage can feed values to a GenServer for
accumulation or further processing:

```
mark {
    << /init { 0 }
       /handle-cast { add }
       /handle-call {
           [/msg /from /state] let
               state state /reply
           end
       }
    >> gen-server /acc exch def

    [1 2 3 4 5]
    { 2 mul } pipe-map
    8 { acc exch gen-cast } pipe-run   % send each item to GenServer

    acc /get gen-call =    % -> 30  (2+4+6+8+10)
    acc /done gen-stop
} actor-spawn coroutine-join pop
```

The pipeline runs concurrently; `pipe-run` sends each doubled value to
the GenServer via `gen-cast`.

### 9.6 Transducers + Pipeline Output

Collect pipeline results, then post-process with a transducer.  This
combines concurrent pipeline execution with single-pass transducer
filtering:

```
[1 2 3 4 5 6 7 8 9 10]
{ dup mul } pipe-map
8 pipe-collect
% => [1 4 9 16 25 36 49 64 81 100]

{ 50 gt } xf-filter into
% => [64 81 100]
```

The pipeline squares values concurrently; the transducer filters the
collected results in a single pass.
