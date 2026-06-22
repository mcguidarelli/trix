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

# Transducers: Technical Reference

Composable, target-independent sequence transformations for Trix.

---

## 1. What Transducers Are

A transducer is a reusable transformation that can be applied to any
sequence type -- eager arrays, lazy sequences, or pipelines.  Define the
transformation once, apply it everywhere.

```
% Define once
/xf { 2 mod 0 eq } xf-filter { 3 mul } xf-map xf-compose def

% Apply to eager array
[1 2 3 4 5 6] xf into               % => [6 12 18]

% Apply to lazy sequence
1 lazy-from xf lazy-into 3 lazy-take lazy-to-array   % => [6 12 18]
```

A transducer is **data**, not a closure.  Each step is a Tagged value
(e.g., `{ pred } /xf-filter tag`).  Composed transducers are arrays of
steps.  The application operators (`into`, `lazy-into`, `pipe-into`)
interpret the spec and translate each step into the target's native
operations.

### 1.1 The Problem Transducers Solve

Trix has three sequence abstractions, each with separate transformation
operators:

| Operation | Eager Array    | Lazy Sequence       | Pipeline            |
| --------- | -------------- | ------------------- | ------------------- |
| Map       | `{ f } map`    | `{ f } lazy-map`    | `{ f } pipe-map`    |
| Filter    | `{ f } filter` | `{ f } lazy-filter` | `{ f } pipe-filter` |
| Take      | `n take`       | `n lazy-take`       | --                  |
| Drop      | `n drop`       | `n lazy-drop`       | --                  |
| Flatten   | `flatten`      | `lazy-flatten`      | --                  |

A transformation like "filter evens, multiply by 3, take first 5" must be
written three different ways for three different targets.  If you change the
logic, you change it in three places.

Transducers eliminate this duplication.  One spec, three applications:

<!-- doctest: skip (placeholder data source applied three ways) -->
```
/xf { 2 mod 0 eq } xf-filter { 3 mul } xf-map 5 xf-take xf-compose xf-compose def

data xf into              % eager
data xf lazy-into         % lazy
data xf pipe-into         % pipeline
```

### 1.2 Transducers as Data

Unlike Clojure's transducers (which are higher-order functions that
transform reducers), Trix transducers are **tagged data**:

```
{ 2 mul } xf-map         % => { 2 mul } /xf-map tag
3 xf-take                % => 3 /xf-take tag
xf-flatten               % => null /xf-flatten tag
```

This design avoids closures entirely.  A transducer step is a 2-element
Tagged pair `[tag-name, payload]` on the VM heap.  Composition concatenates
arrays.  Application dispatches on tag names to invoke the target's native
operators.

**Why data, not closures?**  Three reasons:
1. No closure overhead (no dict allocation, no dict-stack manipulation).
2. Inspectable -- you can examine a transducer's steps.
3. Serializable -- tagged values survive snap-shot/thaw.


## 2. Quick Reference

### Step Constructors

```
xf-map       proc -- xf           % mapping step
xf-filter    proc -- xf           % filtering step
xf-take      n -- xf              % take first N elements
xf-drop      n -- xf              % drop first N elements
xf-scan      init proc -- xf      % running accumulation
xf-flatten   -- xf                % flatten one level
xf-distinct  -- xf                % remove consecutive duplicates (dedupe)
```

### Composition

```
xf-compose   xf1 xf2 -- xf       % compose two transducers
                                   % steps execute left-to-right: xf1 then xf2
```

### Application

```
into         array xf -- array'          % apply to eager array
lazy-into    lazy-seq xf -- lazy-seq'    % apply to lazy sequence
pipe-into    pipe xf -- pipe'            % apply as pipeline stage
xf-reduce    coll init xf rf -- result   % apply + reduce with custom reducer
```


## 3. Usage Patterns

### 3.1 xf-map -- Transform Elements

```
[1 2 3 4 5] { 2 mul } xf-map into
% => [2 4 6 8 10]
```

### 3.2 xf-filter -- Select Elements

```
[1 2 3 4 5 6] { 2 mod 0 eq } xf-filter into
% => [2 4 6]
```

### 3.3 xf-take -- Limit Output

```
[10 20 30 40 50] 3 xf-take into
% => [10 20 30]
```

### 3.4 xf-drop -- Skip Leading Elements

```
[10 20 30 40 50] 2 xf-drop into
% => [30 40 50]
```

### 3.5 xf-flatten -- Flatten Nested Arrays

```
[[1 2] [3 4] [5]] xf-flatten into
% => [1 2 3 4 5]
```

### 3.6 xf-distinct -- Remove Consecutive Duplicates

`xf-distinct` performs consecutive deduplication (like Unix `uniq`), not
global deduplication.  Non-adjacent duplicates are preserved:

```
[1 1 2 2 3 3 1] xf-distinct into
% => [1 2 3 1]

[1 2 3 4 5] xf-distinct into
% => [1 2 3 4 5]  (no consecutive dups)

[9 9 9 9 9] xf-distinct into
% => [9]
```

### 3.7 xf-scan -- Running Accumulation

`xf-scan` produces a running accumulation.  The init value is emitted
first, then each element is combined with the accumulator:

```
[1 2 3 4 5] 0 { add } xf-scan into
% => [0 1 3 6 10 15]
%     ^  ^   ^  ^  ^   ^
%     init +1 +2  +3 +4  +5
```

### 3.8 xf-compose -- Compose Steps

Steps execute left-to-right (xf1 processes input first, xf2 processes
xf1's output):

```
% filter evens, then multiply by 3
{ 2 mod 0 eq } xf-filter { 3 mul } xf-map xf-compose
[1 2 3 4 5 6] exch into
% => [6 12 18]
```

Three or more steps:

```
{ 2 mod 0 eq } xf-filter
{ 10 mul } xf-map xf-compose
2 xf-take xf-compose

[1 2 3 4 5 6 7 8] exch into
% => [20 40]
```

### 3.9 Single Step (No Compose Needed)

A bare Tagged step is a valid transducer.  `into` and friends accept both
single steps and composed arrays:

```
[1 2 3] { 1 add } xf-map into
% => [2 3 4]
```

### 3.10 Reuse the Same Transducer

Transducers are data -- they can be applied multiple times to different
sources:

```
/xf { 2 mul } xf-map 3 xf-take xf-compose def

[1 2 3 4 5] xf into          % => [2 4 6]
[10 20 30 40 50] xf into     % => [20 40 60]
```

### 3.11 lazy-into -- Lazy Application

Apply a transducer to a lazy sequence.  The result is a new lazy sequence:

```
1 lazy-from { 2 mul } xf-map lazy-into 5 lazy-take lazy-to-array
% => [2 4 6 8 10]

1 lazy-from { 2 mod 0 eq } xf-filter lazy-into 3 lazy-take lazy-to-array
% => [2 4 6]
```

### 3.12 xf-reduce -- Transduce + Reduce

Apply a transducer, then reduce the result with a custom reducer:

```
[1 2 3 4 5 6] 0 { 2 mod 0 eq } xf-filter { add } xf-reduce
% => 12  (2 + 4 + 6)
```

With composed transducer:

```
[1 2 3 4 5 6 7 8 9 10] 0
{ 2 mod 0 eq } xf-filter { 1 add } xf-map xf-compose
{ add } xf-reduce
% => 35  ((2+1) + (4+1) + (6+1) + (8+1) + (10+1))
```

### 3.13 pipe-into -- Pipeline Application

Apply a transducer as a pipeline transformation stage:

```
source-pipe { 2 mul } xf-map pipe-into
% => new pipe with map stage applied
```

Note: `xf-take`, `xf-drop`, `xf-scan`, `xf-flatten`, and `xf-distinct`
are not supported for pipeline targets (they raise errors).  Only
`xf-map` and `xf-filter` translate to pipeline stages.

### 3.14 Empty Source

All application operators handle empty sources correctly:

```
[] { 2 mul } xf-map into         % => []
[] 0 xf-take into                % => []
[] { pop true } xf-filter into   % => []
```

### 3.15 Boundary Cases

```
[1 2 3] 0 xf-take into               % => []       (take 0)
[1 2 3] 0 xf-drop into               % => [1 2 3]  (drop 0)
[1 2 3] 3 xf-take into               % => [1 2 3]  (take exact length)
[1 2 3] 3 xf-drop into               % => []       (drop all)
[1 2 3] { } xf-map into              % => [1 2 3]  (identity map)
[1 2 3] { pop true } xf-filter into  % => [1 2 3]  (filter all pass)
[1 2 3] { pop false } xf-filter into % => []       (filter none pass)
```


## 4. Real-World Scenarios

### 4.1 Data Pipeline: ETL

<!-- doctest: skip (placeholder all-records ETL source) -->
```
% Extract-Transform-Load: filter valid records, normalize, take batch

/normalize { /name get length 0 gt } xf-filter def
/transform { /name get } xf-map def
/batch 100 xf-take def

/etl normalize transform xf-compose batch xf-compose def

all-records etl into    % => first 100 non-empty names
```

### 4.2 Numeric Stream Processing

```
% Running average of positive values from a sensor stream
/sensor-xf
  { 0 gt } xf-filter
  0 { add } xf-scan xf-compose
def

[5 -1 3 -2 7 0 2] sensor-xf into
% => [0 5 8 15 17]
```

### 4.3 Log Processing

```
% From a lazy log stream: filter errors, extract messages, take first 10
/log-xf
  { /level get (ERROR) eq } xf-filter
  { /message get } xf-map xf-compose
  10 xf-take xf-compose
def

log-stream log-xf lazy-into lazy-to-array
% => first 10 error messages
```

### 4.4 Deduplication Pipeline

```
% Remove consecutive duplicate sensor readings, then average
[72 72 73 73 73 74 74 72 72]
xf-distinct into
% => [72 73 74 72]
```

### 4.5 Composed Transducer with All Step Types

```
% Complex pipeline: add 1, double, filter < 100, take 3, dedupe
{ 1 add } xf-map
{ 2 mul } xf-map xf-compose
{ 100 lt } xf-filter xf-compose
3 xf-take xf-compose
xf-distinct xf-compose
/complex-xf exch def

[10 10 20 20 30 30 40 40] complex-xf into
% => [22 42]
% (+1,*2 -> [22 22 42 42 62 62 82 82]; filter<100 keeps all; take 3 cuts to
%  [22 22 42] before 62 is reached; distinct -> [22 42])
```


## 5. Design Choices

### 5.1 Data-Driven Specs (Not Higher-Order Functions)

Trix transducers are tagged data, not reducer-transforming functions.

**Why**: Trix's stack-based execution model makes higher-order reducer
composition awkward.  Clojure-style transducers are `(rf -> rf)` -- a
function that takes a reducing function and returns a new one.  In a
stack machine, this requires closures for every step, with closure
allocation and dict-stack manipulation per step per element.

Tagged data is:
- **Cheap**: 16 bytes per step (2 Objects), no closure overhead.
- **Composable**: Array concatenation is O(N) and allocation-free
  beyond the result array.
- **Inspectable**: You can examine the steps programmatically.
- **Serializable**: Tagged values survive snap-shot/thaw with no
  special handling.

### 5.2 Left-to-Right Composition

`xf1 xf2 xf-compose` means xf1 executes first, xf2 second.  This matches
reading order and pipeline intuition (data flows left to right).

**Why**: Consistency with `compose` for procs.  `f g compose` means
"do f then g" in Trix (postfix order).  Transducer composition follows
the same convention.

### 5.3 Consecutive Dedup (Not Global Unique)

`xf-distinct` removes consecutive duplicates, like Unix `uniq`.  It does
not maintain a seen-set for global deduplication.

**Why**: Global dedup requires an unbounded seen-set, which conflicts with
Trix's bounded memory model.  Consecutive dedup is O(1) state (just the
previous element) and handles the common case (sorted or locally-repeated
data).  For global dedup on arrays, use `unique`.

### 5.4 Limited Pipeline Support

Only `xf-map` and `xf-filter` translate to pipeline targets.  `xf-take`,
`xf-drop`, `xf-scan`, `xf-flatten`, and `xf-distinct` raise errors when
applied via `pipe-into`.

**Why**: Pipeline stages are concurrent coroutines.  Stateful transducer
steps (take counter, drop counter, scan accumulator, distinct previous-value)
require per-element state that must be maintained across coroutine yields.
The pipeline's SPSC buffer protocol does not have a natural place for this
state.  Rather than implement a complex stateful pipeline stage, these
steps are restricted to eager and lazy targets where state management is
straightforward.

### 5.5 xf-reduce Unfolds to into + reduce

`xf-reduce` is equivalent to `coll xf into init rf reduce`.  It pushes
the `reduce` operator and its arguments onto the exec stack, followed by
the transducer steps.

**Why**: Reuses existing operators.  No new reduction machinery.  The
transducer steps transform the collection first (producing an array),
then `reduce` folds it.  This is not streaming reduction (each step
produces a full intermediate array), but it is correct and simple.

### 5.6 Steps Pushed in Reverse on Exec Stack

When applying a transducer, steps are pushed onto the exec stack in
reverse order (last step first).  This ensures they execute in the correct
left-to-right order.

**Why**: The exec stack is LIFO.  Pushing step N, then step N-1, ..., then
step 1 means step 1 pops first and executes first.


## 6. Implementation Internals

### 6.1 Tagged Step Format

Each step is a 2-element Tagged value on the VM heap:

```
offset -> [tag-name, payload]
          /xf-map    { 2 mul }     -- proc payload
          /xf-take   3             -- integer payload
          /xf-scan   [0, { add }]  -- array payload [init, proc]
          /xf-flatten null         -- no payload
```

Tag names are `WellKnownName` entries (e.g., `WellKnownName::XfMap`,
`WellKnownName::XfFilter`), interned once at startup, so step dispatch is an
offset comparison.

### 6.2 xf-compose Mechanics

```
xf-compose: xf1 xf2 -- composed

1. Normalize each operand:
   - Tagged (single step): treat as 1-element array.
   - Array (composed): use existing elements.
   - Other: type error.
2. Check: len1 + len2 <= MaxLength.
3. Allocate result array of size len1 + len2.
4. Clone xf1's steps into positions [0, len1).
5. Clone xf2's steps into positions [len1, len1+len2).
6. Return ReadOnly literal array.
```

### 6.3 Application Mechanics (into, lazy-into, pipe-into)

All three application operators share `xf_push_steps_for_target`:

```
xf_push_steps_for_target(steps, step_count, target):

For each step (in reverse order):
  1. Read tag-name from step's Tagged pair.
  2. Dispatch on tag-name + target to find the native operator:
     xf-map  + Array -> Map
     xf-map  + Lazy  -> LazyMap
     xf-map  + Pipe  -> PipeMap
     xf-filter + Array -> Filter
     ... etc.
  3. Push the native operator onto exec stack.
  4. If the step has a proc payload:
     Push proc (as literal) onto exec stack.
     Push make-executable onto exec stack (before the operator).
  5. If the step has a scalar payload (integer):
     Push scalar (as literal) onto exec stack.
  6. If the step is xf-scan:
     Push init, make-executable, proc, then the scan operator.
```

The result: after the application operator returns, the exec stack has
a sequence of native operations that will execute in order, transforming
the source on the operand stack.

### 6.4 xf-reduce Mechanics

```
xf-reduce: coll init xf rf -- result

1. Pop rf, xf, init from op stack (leave coll).
2. Push onto exec stack (LIFO order):
   - Reduce operator
   - make-executable
   - rf (literal)
   - init (literal)
3. Push transducer steps via xf_push_steps_for_target(Array target).
4. Steps execute first (transform coll to array), then reduce folds.
```

### 6.5 Memory Cost

| Component                     | Size                                           |
| ----------------------------- | ---------------------------------------------- |
| Single step (Tagged)          | 16 bytes (2 Objects)                           |
| Composed transducer (N steps) | 16*N + 8*N bytes (Tagged pairs + array)        |
| `into` application            | No allocation (pushes operators on exec stack) |
| `lazy-into` application       | No allocation (pushes lazy operators)          |
| Intermediate arrays           | Depends on data size (normal array allocation) |

### 6.6 Execution Cost

Transducer application adds no per-element overhead beyond what the native
operators would cost.  The translation from transducer spec to native
operators happens once at application time, not per element.

The cost is:
- Application-time: O(N) where N = step count (push N operators).
- Per-element: Same as calling the native operators directly.

### 6.7 Snap-Shot/Thaw

Transducer tag names are `WellKnownName` entries; the well-known name offset
table is part of the serialized VM state (`wellknown_offsets_offset` in
`SnapShotHeader`).  Tagged steps on the VM heap are captured automatically.
Transducers survive snap-shot/thaw.

### 6.8 Source File

All transducer operators are in `src/ops_transducer.inl` (~400 lines).
The `dedupe` array operator is in `src/ops_higher.inl` (~40 lines).


## 7. Composability

### 7.1 Transducers + Lazy Sequences

The primary composition: define a transducer, apply to an infinite lazy
sequence, take what you need:

```
/xf { 2 mod 0 eq } xf-filter { 3 mul } xf-map xf-compose def

1 lazy-from xf lazy-into 5 lazy-take lazy-to-array
% => [6 12 18 24 30]
```

### 7.2 Transducers + Protocols

Use protocol methods as transducer step functions:

```
[/to-str] /Stringify def-protocol
{ 10 string to-string } /to-str /integer-type def-method

[1 2 3] { to-str } xf-map into
% => [(1) (2) (3)]
```

### 7.3 Transducers + Closures

Capture configuration in transducer step procs.  A closure is a Curry, but
`xf-filter`/`xf-map` need a `{ }` Proc — bind the closure to a name and wrap it
in a proc for the step:

```
/threshold 50 def
{ threshold gt } [/threshold] closure-capture /above exch def
/xf
  { above } xf-filter
  { 2 mul } xf-map xf-compose
def

[10 60 30 80 40 90] xf into    % => [120 160 180]
```

### 7.4 Transducers + Pattern Matching

Use `match` inside transducer step procs:

```
/classify {
  [
    { 0 gt } { pop (positive) }
    { 0 eq } { pop (zero) }
    { pop true } { pop (negative) }
  ] match
} def

[-1 0 1 2 -3] { classify } xf-map into
% => [(negative) (zero) (positive) (positive) (negative)]
```

### 7.5 Transducers + Pipelines

Limited to map and filter, but useful for consistent transformation
definitions:

<!-- doctest: skip (placeholder source-pipe pipeline source) -->
```
/clean-xf
  { length 0 gt } xf-filter
  { dup length 1 exch take } xf-map xf-compose
def

source-pipe clean-xf pipe-into    % filter empty, take first char
```

### 7.6 Transducers + xf-reduce + Contracts

```
{ 0 ge }
{
  [1 2 3 4 5 6 7 8 9 10] 0
  { 2 mod 0 eq } xf-filter
  { add } xf-reduce
} postcondition
% => 30 (2+4+6+8+10, verified non-negative)
```


## 8. Error Handling

| Error          | Condition                                                                              |
| -------------- | -------------------------------------------------------------------------------------- |
| `/type-check`  | Unknown transducer step tag (not a recognized /xf-* name)                              |
| `/type-check`  | `xf-compose` operand is not a transducer (not tagged or array)                         |
| `/type-check`  | Transducer step is not a tagged value (in composed array)                              |
| `/type-check`  | `pipe-into` with non-pipe-buffer source                                                |
| `/unsupported` | `xf-take`, `xf-drop`, `xf-scan`, `xf-flatten`, or `xf-distinct` on pipeline target     |
| `/index-check` | eager `into` with `xf-take`/`xf-drop` N greater than the (post-preceding-steps) length |
| `/limit-check` | `xf-compose` result exceeds maximum array length                                       |


## 9. Limitations

- **No streaming reduction.** `xf-reduce` materializes the full
  intermediate array before reducing.  For large datasets, this uses
  more memory than a true streaming transduction.

- **Pipeline support is limited.** Only `xf-map` and `xf-filter` work
  with `pipe-into`.  Stateful steps require eager or lazy targets.

- **No early termination for eager arrays.** `xf-take` in an eager
  `into` processes the full source array through preceding steps, then
  takes N from the result.  There is no short-circuit.  For lazy
  sequences, `xf-take` via `lazy-into` naturally short-circuits.
  Note the eager path uses the strict native `take`/`drop`, so eager
  `xf-take`/`xf-drop` raise `/index-check` if N exceeds the
  post-preceding-steps length; the lazy `lazy-into` path instead clamps
  to the available elements.

- **Transducer steps are not validated at compose time.** Errors like
  "unknown step tag" or "unsupported for pipeline" are raised at
  application time, not when composing.

- **No stateful step reset.** Stateful steps (scan, distinct) create
  fresh state per application.  Reusing a transducer across multiple
  `into` calls is safe -- each call gets independent state via the
  native operators.
