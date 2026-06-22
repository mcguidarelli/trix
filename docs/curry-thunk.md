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

# Curry and Thunk in Trix

Curry and Thunk are the two native types that make Trix a genuine functional
programming language.  Curry provides partial application and function
composition.  Thunk provides lazy evaluation with memoization.  Together they
enable closures, pipelines, deferred computation, and the lazy sequence
library -- all without garbage collection.

This document covers the complete Curry and Thunk system: all 9 operators,
patterns from simple partial application through memoized recursive
algorithms, internal architecture, and design rationale.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Reference](#2-quick-reference)
3. [Curry Tutorial](#3-curry-tutorial)
4. [Partial Application Patterns](#4-partial-application-patterns)
5. [Function Composition](#5-function-composition)
6. [Thunk Tutorial](#6-thunk-tutorial)
7. [Lazy Evaluation Patterns](#7-lazy-evaluation-patterns)
8. [Combined Patterns](#8-combined-patterns)
9. [Real-World Scenarios](#9-real-world-scenarios)
10. [Architecture](#10-architecture)
11. [Curry vs Closures](#11-curry-vs-closures)
12. [Design Decisions](#12-design-decisions)

---

## 1. Overview

Trix is a concatenative language: functions do not take named parameters.
Composition is implicit -- writing `f g` applies `f` then `g`.  This means
there are no closures in the traditional sense: procs do not capture an
environment at definition time.

Curry and Thunk fill this gap with two complementary mechanisms:

**Curry** pairs a captured value with a callable.  When executed, it pushes
the value onto the operand stack, then dispatches the callable.  This is
partial application: fixing one argument of a function produces a new
function.  `compose` creates curries where both slots are executable,
providing function composition as a first-class operation.

**Thunk** wraps a proc in a lazy container.  The proc is not
executed until `force` is called.  Once forced, the result is cached
(memoized) -- subsequent `force` calls return the cached value in O(1) time
without re-executing the proc.

Both are native VM types with dedicated packed encoding, save/restore
integration, deep equality, and snap-shot/thaw support.

```
% Curry: partial application
5 //add curry           % => curry(5, add)
3 exch exec             % => 8  (pushed 5, ran add)

% Thunk: lazy evaluation
{ 3 4 mul } thunk       % => thunk (proc not yet run)
dup force               % => 12  (proc runs, result cached)
force                   % => 12  (cached, proc does NOT run again)
```

---

## 2. Quick Reference

### Curry Operators

| Operator   | Stack Effect              | Description                               |
| ---------- | ------------------------- | ----------------------------------------- |
| `curry`    | `value callable -- curry` | Create partial application                |
| `uncurry`  | `curry -- value callable` | Decompose into components                 |
| `compose`  | `f g -- curry`            | Function composition (exec runs f then g) |
| `is-curry` | `any -- bool`             | Type predicate                            |

### Thunk Operators

| Operator           | Stack Effect     | Description                                  |
| ------------------ | ---------------- | -------------------------------------------- |
| `thunk`            | `proc -- thunk`  | Create lazy wrapper                          |
| `force`            | `any -- value`   | Evaluate thunk (pass-through for non-thunks) |
| `thunk-evaluated?` | `thunk -- bool`  | Has thunk been forced?                       |
| `thunk-reset`      | `thunk -- thunk` | Reset to unevaluated state                   |
| `is-thunk`         | `any -- bool`    | Type predicate                               |

### Type Properties

| Property          | Curry                     | Thunk                          |
| ----------------- | ------------------------- | ------------------------------ |
| Attribute         | Always Executable         | Always Literal                 |
| `type`            | `/curry-type`             | `/thunk-type`                  |
| `make-literal`    | Raises `type-check`       | Allowed (no-op)                |
| `make-executable` | Allowed (no-op)           | Allowed                        |
| `deep-eq`         | Compares value + callable | Compares state + proc + result |
| `eq`              | Identity (same offset)    | Identity (same offset)         |
| VM storage        | 2 Objects (16 bytes)      | 3 Objects (24 bytes)           |
| Packed encoding   | `PackedType::Curry` X=1   | Not packed (runtime-only)      |
| Save/restore      | Journaled                 | Journaled (state reverts)      |

---

## 3. Curry Tutorial

### 3.1 Creating and Executing Curries

`curry` pops a callable and a value, stores them as a pair in VM memory, and
pushes a Curry object.  When the Curry is executed (via `exec`, name lookup,
or interpreter dispatch), it pushes the value then dispatches the callable:

```
% value callable curry => curry-object
% exec: push value, dispatch callable

5 //add curry          % create curry(5, add)
3 exch exec            % stack: 3 curry => push 5 => 3 5 => add => 8

10 { 2 mul } curry     % create curry(10, {2 mul})
exec                   % push 10, exec {2 mul} => 20
```

The callable can be any executable object: an operator, a packed procedure,
an executable array, an executable name, or another curry.

### 3.2 Decomposing with uncurry

`uncurry` decomposes a curry back into its value and callable:

```
5 //add curry uncurry
% stack: 5 add-operator

% Round-trip: curry then uncurry preserves both components
42 //sub curry uncurry
//sub eq         % true (callable is sub)
exch 42 eq       % true (value is 42)
```

### 3.3 Naming Curries

Curries are first-class values.  Store them in variables and call them by
name:

```
/add5 5 //add curry def
3 add5            % => 8
7 add5            % => 12

/double 2 //mul curry def
[1 2 3] { double } map    % => [2 4 6]
```

When the interpreter encounters an executable name bound to a curry, it
dispatches the curry directly.  In tail position, this gets TCO -- the
`@call` frame is reused:

```
/dec -1 //add curry def
/countdown {
    dup 0 le { pop } { dec countdown } if-else
} def
100000 countdown    % constant exec stack depth
```

### 3.4 Type Properties

Curries are always Executable.  `make-literal` raises `type-check`:

```
5 //add curry type /curry-type eq        % true
5 //add curry is-curry                   % true
42 is-curry not                          % true

% (operate on a curry held as data — building one inside the try proc would
%  execute the embedded //add; see §11)
5 //add curry /c exch def
{ /c load make-literal } try            % => /type-check
```

### 3.5 Equality

`eq` compares by identity (same VM offset).  `deep-eq` compares structurally:

```
5 //add curry dup eq                        % true (same object)
5 //add curry 5 //add curry eq not          % true (different objects)
5 //add curry 5 //add curry deep-eq         % true (same structure)
5 //add curry 6 //add curry deep-eq not     % true (different value)
```

---

## 4. Partial Application Patterns

### 4.1 Single-Argument Partial Application

The most common pattern: fix one argument of a binary operator:

```
/add10 10 //add curry def
/mul3 3 //mul curry def

5 add10          % => 15
7 mul3           % => 21
```

A curry pushes its captured value **first**, then runs the callable on
`captured input`.  For commutative operators (`add`, `mul`) the operand order
does not matter.  For a non-commutative operator it does: `sub` is `a b -- a-b`,
so `100 //sub curry` applied to `40` computes `40 - 100` (the input is `a`, the
captured `100` is `b`), which is `-60`, not `100 - 40`.  To fix the captured
value as the *left* operand, wrap the operator in a proc that swaps:

```
/sub-from-100 100 { exch sub } curry def

40 sub-from-100  % => 60  (100 - 40: exch puts 100 below the input)
```

### 4.2 Multi-Argument Partial Application

Nested curries close over multiple arguments.  Each curry captures one value;
nesting captures several:

```
% curry(1, curry(2, add))
% Exec: push 1, exec inner => push 2, add => 1 + 2 = 3
1 2 //add curry curry exec    % => 3

% Three values
10 20 30 { add add } curry curry curry exec    % => 60
```

### 4.3 Currying Higher-Order Operations

Curry an operator with a proc to create a specialized transformer:

```
% "filter evens" as a reusable proc.  (Currying //filter does not work: filter
%  needs a real array to operate on, and //filter inside a proc body executes
%  on the wrong stack — wrap the operation in a proc instead.)
/filter-evens { { 2 mod 0 eq } filter } def

[1 2 3 4 5 6 7 8 9 10] filter-evens
% => [2 4 6 8 10]
```

### 4.4 Curries in Data Structures

Curries can be stored in arrays, dicts, and sets:

```
% Dispatch table of operations
<< /inc 1 //add curry
   /dec -1 //add curry
   /double 2 //mul curry
>> /ops exch def

10 ops /inc get exec       % => 11
10 ops /double get exec    % => 20

% Array of transformations
[ 1 //add curry 2 //mul curry -3 //add curry ]
{ 10 exch exec } map       % => [11 20 7]
```

### 4.5 Curry as Callback

Pass a curry as a callback to a higher-order operator:

```
/with-logging {
    % value callback -- result
    exch                          % callback value
    dup (Processing: ) print =    % print the value (= prints with newline)
    exch exec                     % value callback -- run callback on value
} def

% A "multiply by 10" curry, passed as DATA via /name load (a name bound to a
% curry auto-executes on reference; load pushes it unevaluated).
10 //mul curry /times10 exch def
5 /times10 load with-logging      % prints "Processing: 5", returns 50
```

---

## 5. Function Composition

### 5.1 Basic Composition

`compose` creates a curry where the first slot is executable.  When executed,
the first callable runs, then the second:

```
% compose: f g -- curry
% exec: run f, then run g

{ 2 mul } { 1 add } compose % "double then increment"
5 exch exec                 % => 11  (5*2=10, 10+1=11)
```

### 5.2 Composing Operators

Operators can be composed directly:

```
/square //dup //mul compose def
7 square            % => 49

/my-abs //dup { 0 lt { 0 exch sub } if } compose def
-5 my-abs              % => 5
```

### 5.3 Pipeline Construction

Build multi-step pipelines by chaining `compose`:

```
{ 1 add } { 2 mul } compose { 3 sub } compose
10 exch exec         % => 19  ((10+1)*2 - 3)

% Named pipeline
/process
    { lowercase } { ( ) split } compose
    { { length 3 gt } filter } compose
def

(The Lazy Brown Spider) process
% => [(lazy) (brown) (spider)]  (lowercased, split, filtered to length > 3)
```

### 5.4 Compose Properties

`compose` returns a curry, so all curry properties apply:

```
{ 2 mul } { 1 add } compose is-curry    % true

% deep-eq works on composed values
//dup //mul compose //dup //mul compose deep-eq    % true

% Store in variables, pass to map
/square //dup //mul compose def
[3 4 5 6] { square } map    % => [9 16 25 36]
```

### 5.5 Identity Composition

The empty proc `{ }` is the identity function.  Composing with identity
produces an equivalent pipeline:

```
{ } { 2 mul } compose 5 exch exec 10 eq    % true (identity . f = f)
{ 2 mul } { } compose 5 exch exec 10 eq    % true (f . identity = f)
```

---

## 6. Thunk Tutorial

### 6.1 Creating and Forcing Thunks

`thunk` wraps a proc without executing it.  `force` evaluates the
proc and caches the result:

```
{ 3 4 add } thunk % create thunk (proc not yet run)
force             % => 7 (proc runs, result cached)
```

### 6.2 Memoization

Once forced, subsequent `force` calls return the cached result without
re-executing the proc:

```
{ 3 4 add } thunk
dup force       % => 7 (first time: proc runs)
force           % => 7 (second time: cached, proc does NOT run)
```

### 6.3 Proving Memoization with Side Effects

```
<< /n 0 >> /counter exch def
{ counter /n counter /n get 1 add put 100 } thunk
dup force           % => 100 (counter /n = 1)
dup force           % => 100 (counter /n still 1 -- proc did not run again)
pop
counter /n get      % => 1 (proc ran exactly once)
```

### 6.4 Pass-Through for Non-Thunks

`force` is a no-op for non-thunk values.  This enables generic code that
works with both lazy and eager values:

```
42 force              % => 42 (pass-through)
(hello) force         % => (hello) (pass-through)
[1 2 3] force         % => [1 2 3] (pass-through)
```

### 6.5 Inspection and Reset

Check whether a thunk has been evaluated, and reset it to force
re-evaluation:

```
{ 42 } thunk
dup thunk-evaluated?    % => false (not yet forced)
dup force pop
dup thunk-evaluated?    % => true  (has been forced)
thunk-reset
thunk-evaluated?        % => false (reset, will re-evaluate on next force)
```

`thunk-reset` frees the cached result's ExtValue (if any) and resets the
state to Unevaluated.  The next `force` will re-execute the proc.

### 6.6 Circular Detection

Forcing a thunk during its own evaluation raises `undefined-result`:

```
<< >> /d exch def
d /t { d /t get force } thunk put
{ d /t get force } try    % => /undefined-result
```

### 6.7 Error Handling

If the thunk's proc raises an error during `force`, the error
**propagates** and the thunk's state is **reset to Unevaluated** — it is not
left half-evaluated.  This is a deliberate auto-retry contract: the next
`force` simply **re-runs the proc**.  If the underlying problem is still
present, the same original error is replayed; if it has since been resolved,
the force succeeds and the result is cached as usual.  (`thunk-reset` forces
re-evaluation too, but is not required here — the error path already reset the
state.)

```
{ /range-check throw } thunk /t exch def
{ t force } try /range-check eq      % true (error propagated)
t thunk-evaluated? not               % true (state reset to Unevaluated)
{ t force } try /range-check eq      % true (re-ran the proc, same error)
```

A thunk whose underlying issue is resolved between forces succeeds on the
retry.  Here the proc throws only on its first attempt (tracked via an
external counter), then succeeds and caches:

```
<< /n 0 >> /retry-state exch def
{ retry-state /n get 1 add /m exch def
  retry-state /n m put
  m 1 eq { /range-check throw } if
  m
} thunk /retry-thunk exch def

{ retry-thunk force } try /range-check eq % true  (first attempt throws)
retry-thunk force 2 eq                    % true  (retry succeeds, returns 2)
retry-thunk force 2 eq                    % true  (result now cached)
```

---

## 7. Lazy Evaluation Patterns

### 7.1 Deferred Computation

Delay expensive computation until the result is actually needed:

<!-- doctest: skip (synopsis: complex-query is a stand-in for an expensive computation) -->
```
/expensive-result { complex-query } thunk def

% Later, only when needed:
expensive-result force    % runs complex-query (only once)
```

### 7.2 Lazy Initialization

Initialize a resource on first access, cache for subsequent accesses:

```
/config {
    (config.json) (r)#b { read-all } with-stream
    % ... parse config ...
} thunk def

% First access reads the file
config force /timeout get

% Subsequent accesses use cached result
config force /retries get
```

### 7.3 Memoized Recursive Functions

Use thunks to memoize recursive computations.  Each thunk computes its
value once, referencing other thunks that also compute once:

```
% Fibonacci via lazy thunk network
{ 0 } thunk /fib0 exch def
{ 1 } thunk /fib1 exch def
{ fib0 force fib1 force add } thunk /fib2 exch def
{ fib1 force fib2 force add } thunk /fib3 exch def
{ fib2 force fib3 force add } thunk /fib4 exch def
{ fib3 force fib4 force add } thunk /fib5 exch def

fib5 force    % => 5 (each intermediate computed exactly once)
```

### 7.4 Lazy Data Structures

Trix's lazy sequence library uses thunks as the tail of each node.  A lazy
sequence is `null` (empty) or `[head, tail-thunk]`:

```
% A lazy sequence is null (empty) or [head, tail-thunk].  lazy-from is the
% built-in that produces an infinite sequence; inspect a node's shape:
1 lazy-from
dup 0 get =          % => 1     (the head)
1 get is-thunk =     % => true  (the tail is a thunk, forced on demand)
```

### 7.5 Conditional Evaluation

Use thunks to defer branches of a conditional:

```
/lazy-if-else {
    % cond then-thunk else-thunk -- result
    rot { pop force } { exch pop force } if-else
} def

% Only the chosen branch is evaluated
true
{ (expensive true computation) } thunk
{ (expensive false computation) } thunk
lazy-if-else    % => evaluates only the true thunk
```

---

## 8. Combined Patterns

### 8.1 Curry + Thunk: Lazy Partial Application

Create a partially applied function that defers its remaining computation:

```
% A thunk that, when forced, creates a specialized processor.
% Wrap the operator as { gt }: a bare //gt inside the proc body would execute
% when the thunk is forced instead of being captured (see §11).
/make-processor {
    % config -- thunk
    { |cfg|
        cfg /threshold get { gt } curry
    } thunk
} def

<< /threshold 10 >> make-processor
force           % => curry(10, {gt})
15 exch exec    % => true (15 > 10)
```

### 8.2 Memoized Curries

Cache the result of a curry-based computation:

```
% Expensive key function, memoized
/cached-key {
    { expensive-computation } thunk
    /key exch def
    { key force } curry
} def
```

### 8.3 Compose + Thunk: Lazy Pipeline

Build a pipeline and defer its execution:

`compose` yields a Curry, but `thunk` requires a Proc — so wrap the composed
curry in a `{ ... exec }` proc before thunking:

```
/lazy-pipeline {
    % str -- thunk
    /s exch def
    { s { lowercase } { length } compose exec } thunk
} def

(Hello World) lazy-pipeline force    % => 11 (deferred until forced)
```

Or capture the value into the composed pipeline via `curry`, then thunk a proc
that fires it:

```
/make-lazy-transform {
    % value pipeline -- thunk    (pipeline is a composed curry)
    curry /c exch def
    { c exec } thunk
} def

(Hello World) { lowercase } { length } compose
make-lazy-transform
force    % => 11 (deferred until forced)
```

### 8.4 Thunk Chains

Each thunk depends on the previous, forming a computation chain:

```
{ 1 } thunk /a exch def
{ a force 1 add } thunk /b exch def
{ b force 1 add } thunk /c exch def

c force    % => 3 (forces b, which forces a, then caches all)
b force    % => 2 (cached)
a force    % => 1 (cached)
```

---

## 9. Real-World Scenarios

### 9.1 Configuration with Lazy Defaults

A configuration system where default values are computed lazily:

```
/make-config {
    % defaults-dict overrides-dict -- config-dict
    /overrides exch def
    /defaults exch def

    % Merge: override wins, default is forced only if no override.  Dict
    % for-all pushes key then value (value on top) and requires a net
    % `key value --` stack effect, so accumulate into a named dict.
    /config 0 dynamic-dict def
    defaults { /v exch def /k exch def
        config k
        overrides k known?
        { overrides k get }
        { v force }
        if-else
        put
    } for-all
    config
} def

% Defaults are thunks -- expensive ones are never computed if overridden
/defaults << /timeout { 30 } thunk
             /retries { 3 } thunk
             /db-url  { (read-db-config-from-file) } thunk
          >> def

defaults << /timeout 60 >> make-config
% db-url default WAS forced (present, not overridden); overridden keys (timeout) skip their thunk
```

### 9.2 Memoized Fibonacci (Practical Version)

Using a dict as a memoization cache.  The cache must be able to grow, so use a
growable `0 dynamic-dict` — a `<< >>` literal has a fixed capacity of 4 and
would overflow with `dict-full` once the cache exceeds four entries.

```
0 dynamic-dict /fib-cache exch def

/fib { |n|
    fib-cache n known-get {
        % cache hit, value on stack
    } {
        n 2 lt {
            n
        } {
            n 1 sub fib
            n 2 sub fib
            add
        } if-else
        dup fib-cache exch n exch put
    } if-else
} def

30 fib    % => 832040 (computed efficiently via memoization)
```

### 9.3 Callback Registry (Event Handlers)

Store curries as event handlers in a dispatch dict:

```
/make-event-system {
    << >> /handlers exch def

    % `on` keeps the callback proc as DATA on the operand stack.  Binding
    % it to a |locals| name would AUTO-EXECUTE it on lookup (a proc in a
    % locals frame runs on bare-name reference -- see 4.5), so the handler
    % is juggled on the stack and appended as a value, never named.
    /on {
        % stack: callback event
        /ev exch def
        handlers ev known?
        { handlers ev get exch append }   % existing event: append to its array
        { 0 array exch append }           % first handler: [ callback ]
        if-else
        handlers exch ev exch put
    } def

    /emit { |event data|
        handlers event known?
        {
            handlers event get
            { data exch exec } for-all
        } if
    } def
} def

make-event-system

% Register handlers (curries bind context)
/user-name (Alice) def
{ pop user-name (: logged in) concat = } /login on
{ pop ( session started) = } /login on

/login null emit
% prints: Alice: logged in
% prints:  session started
```

### 9.4 Strategy Pattern

Select algorithms at runtime via curries stored in a dict:

```
% Sorting strategies.  sort-by needs a real Proc + array, so store the whole
% { { key } sort-by } operation as a proc (a //sort-by curry would not work).
<< /by-name   { { 0 get } sort-by }
   /by-age    { { 1 get } sort-by }
   /by-score  { { 2 get } sort-by }
>> /strategies exch def

/sort-users {
    % users strategy-name -- sorted-users
    strategies exch get exec
} def

[ [(Alice) 30 85] [(Bob) 25 92] [(Carol) 35 78] ] /users exch def
users /by-age   sort-users   % sorted by age:   Bob, Alice, Carol
users /by-score sort-users   % sorted by score: Carol, Alice, Bob
```

### 9.5 Dependency Injection

Build a service with injected dependencies via curries:

Inject the dependencies by **currying** them into the service callable — bundle
them in a dict and curry that dict into the service proc, so each service keeps
its own deps (frame locals are not captured by a returned proc):

```
/make-service {
    % logger-proc db-proc -- service-curry
    /db exch def
    /log exch def
    << /log /log load  /db /db load >>     % capture the deps in a dict
    { |deps|
        (Starting request)  deps /log get exec
        deps /db get exec
        (Request complete)  deps /log get exec
    } curry
} def

% Production
{ = } { production-query } make-service /prod-service exch def

% Test (mock dependencies): mock logger drops its arg, mock db returns 42.
% A name bound to a curry executes when referenced, so just name it:
{ pop } { 42 } make-service /test-service exch def
test-service    % => 42 (uses mock db, silent logger)
```

### 9.6 Iterator Protocol

Build an iterator using a thunk that produces the next value:

```
/make-counter-iter {
    % start -- iterator-thunk
    /n exch def
    {
        n
        /n n 1 add def
    } thunk
} def

0 make-counter-iter
dup force =               % => 0
dup thunk-reset force =   % => 1
dup thunk-reset force =   % => 2
```

Note: this pattern uses side effects in the thunk proc.  For pure lazy
sequences, use the built-in lazy sequence library (48 operators) which uses
`[head, tail-thunk]` convention without mutation.

### 9.7 Builder Pattern with Compose

Accumulate transformations as a composed pipeline, then apply:

```
/make-builder {
    % -- builder (starts with identity)
    { }
} def

/add-step {
    % builder step-proc -- builder'   (compose: f g -- curry)
    compose
} def

/build {
    % input builder -- result   (builder on top, input below; run it)
    exec
} def

% Build a text processing pipeline
make-builder
{ lowercase } add-step
{ ( ) split } add-step
{ { length 3 gt } filter } add-step
/my-pipeline exch def

% A name bound to a curry auto-executes on reference, so pass the pipeline as
% DATA with `/my-pipeline load` (unevaluated); `build` then runs it on the input.
(The Quick Brown Fox) /my-pipeline load build
% => [(quick) (brown)]
```

---

## 10. Architecture

### 10.1 Curry Memory Layout

A Curry is an 8-byte Object with a `vm_offset_t` pointing to a 2-Object pair
in VM memory:

```
Object (8 bytes):
  m_aat:    ExecutableAttrib | Type::Curry    (1 byte)
  m_object_save_level:                        (1 byte)
  m_length: 0                                 (2 bytes, unused)
  m_curry:  vm_offset_t                       (4 bytes, offset to pair)

VM Heap (16 bytes at offset):
  storage[0]:  Object  (value -- Literal for curry, Executable for compose)
  storage[1]:  Object  (callable)
```

Total cost: 24 bytes (8-byte Object + 16-byte VM pair).  If the captured
value is an ExtValue type, an additional 8-byte ExtValue is allocated.

### 10.2 Curry Dispatch (execute_curry)

When the interpreter encounters a Curry on the exec stack, it calls
`execute_curry()`:

1. Clone the value and callable from the VM pair.
2. If the value is Literal (normal curry): push it to the operand stack.
   If the value is Executable (compose): push the callable to the exec
   stack, then dispatch the value via `execute_value()`.
3. Dispatch the callable: if Literal, push to operand stack; if Executable,
   dispatch via `execute_value()`.

For normal curries (partial application), the dispatch is:
```
push value -> dispatch callable
```

For composed curries, the dispatch is:
```
push callable to exec stack -> dispatch first callable
(when first completes, interpreter pops and dispatches second)
```

### 10.3 Thunk Memory Layout

A Thunk is an 8-byte Object with a `vm_offset_t` pointing to a 3-Object
triple in VM memory:

```
Object (8 bytes):
  m_aat:    LiteralAttrib | Type::Thunk      (1 byte)
  m_object_save_level:                        (1 byte)
  m_length: 0                                 (2 bytes, unused)
  m_thunk:  vm_offset_t                       (4 bytes, offset to triple)

VM Heap (24 bytes at offset):
  storage[0]:  Object  (state -- Integer: 0=Unevaluated, 1=Evaluating, 2=Evaluated)
  storage[1]:  Object  (proc -- the proc to execute)
  storage[2]:  Object  (result -- cached result after evaluation, null before)
```

Total cost: 32 bytes (8-byte Object + 24-byte VM triple).

### 10.4 Thunk Evaluation (force_op and @force-complete)

When `force` is called on an unevaluated thunk:

1. Set state to Evaluating (prevents circular evaluation).
2. Push `@force-complete` control operator to exec stack.
3. Push the thunk object (literal) to exec stack (will be pushed to op stack
   after proc completes).
4. Execute the proc via `execute_value()`.

After the proc completes, the interpreter pops the thunk (pushed to operand
stack as a literal), then pops `@force-complete`:

5. `@force-complete` finds the proc's result and the thunk on the operand
   stack.
6. Clones the result into `storage[2]` (the cache slot).
7. Sets state to Evaluated.
8. Pops the thunk from the operand stack, leaving only the result.

```
Exec stack during evaluation:

    [@force-complete]  [thunk-literal]  [proc]
     ^                                  ^
     fires last                         fires first
```

### 10.5 Thunk States

```
ThunkUnevaluated (0):  proc has not been called
ThunkEvaluating  (1):  proc is currently running (circular detection sentinel)
ThunkEvaluated   (2):  result is cached in storage[2]
```

State transitions:
```
Unevaluated --force--> Evaluating --proc completes--> Evaluated
                          |    |                          |
                          |    +--re-entrant force--> Unevaluated   (circular; raises
                          |                            undefined-result, then
                          |                            @force-fail resets to Unevaluated)
                          |
                          +--proc raises error--> Unevaluated   (error propagates,
                          |                        state reset; next force retries)
                          |
                       Evaluated --thunk-reset--> Unevaluated
```

Both error edges reset the thunk to Unevaluated. A circular force (forcing a
thunk while it is already evaluating) raises `undefined-result`; that error
unwinds through the same `@force-complete` barrier, which `@force-fail` resets
to Unevaluated -- exactly like a proc error -- so a subsequent force gets a
clean retry (see §6.7, §12.6).

### 10.6 Packed Encoding

Curry uses `PackedType::Curry` in packed arrays.  The X-bit (bit 7 of the
packed header) determines whether the extracted type is Curry (X=1,
Executable) or Tagged (X=0, Literal).

Thunks are **not** packed.  They are runtime-only objects that hold mutable
state (the evaluation status and cached result).  Packing a thunk would
require either serializing its state (which may include side effects) or
discarding the cache (which changes semantics).  Since thunks are typically
created at runtime and are not stored in proc bodies, this limitation
has no practical impact.

### 10.7 Save/Restore

Both Curry and Thunk participate in save/restore transactions:

**Curry:** The 2-Object pair is allocated in the VM heap.  If either Object
is modified after a save point, the modification is journaled and rolled back
on `restore`.  The VM heap pointer is reset on `restore`, discarding curries
allocated after the save point.

**Thunk:** The 3-Object triple is journaled per-Object.  When `force` changes
the state from Unevaluated to Evaluated, `Save::save_object()` is called on
the state and result Objects.  On `restore`, the state reverts to Unevaluated
and the cached result is discarded.  This means a thunk that was forced after
a save point will re-evaluate on the next `force` after `restore`.

```
{ 42 } thunk /t exch def
save /sv exch def
    t force =    % => 42 (state = Evaluated)
sv restore
t thunk-evaluated? = % => false (state reverted to Unevaluated)
t force =            % => 42 (proc runs again)
```

### 10.8 Deep Equality

**Curry:** `deep-eq` recursively compares `storage[0]` (value) and
`storage[1]` (callable).  Two curries are structurally equal if both their
captured values and callables are `deep-eq`.

**Thunk:** `deep-eq` recursively compares all three slots: `storage[0]`
(state), `storage[1]` (proc), and `storage[2]` (result).  This means an
evaluated thunk is not `deep-eq` to an unevaluated thunk with the same proc,
because their states differ.

### 10.9 PrintFmt

| Type  | Default (`%s`) | Alt (`%#s`) | Object (`%O`)                           |
| ----- | -------------- | ----------- | --------------------------------------- |
| Curry | `curry`        | `CURRY`     | `--curry value callable--`              |
| Thunk | `thunk`        | `THUNK`     | `--thunk result--` / `--thunk(state)--` |

The `%O` format for a Curry shows its captured value and callable (e.g.
`--curry 5 add--`).  For a Thunk it shows the cached **result** once evaluated
(`--thunk 7--`), or the state word otherwise (`--thunk(unevaluated)--` /
`--thunk(evaluating)--`).

---

## 11. Curry vs Closures

Most languages use closures to capture environment:

```python
# Python closure
def make_adder(n):
    def adder(x):
        return x + n    # n captured from enclosing scope
    return adder

add5 = make_adder(5)
add5(3)    # => 8
```

Trix uses curry instead of closures:

```
% Trix curry
/make-adder { { add } curry } def
5 make-adder /add5 exch def
3 add5      % => 8
```

The `{ add }` wrapper is required because a bare `//add` inside a proc body is
an embedded executable that *runs* when `make-adder` runs (the proc-bound-name
vs. operator asymmetry; see `scanner-syntax.md` §4.3).  Wrap the operator in a
proc — `{ add } curry` — or load it at runtime — `/add load curry`.  Top-level
`5 //add curry` (not inside a factory proc) works as-is.

Key differences:

| Aspect         | Closure (Python, JS, Lua) | Curry (Trix)            |
| -------------- | ------------------------- | ----------------------- |
| Captures       | Entire environment        | One value               |
| Multi-argument | Natural                   | Nested curries          |
| Inspectable    | No (opaque)               | Yes (`uncurry`)         |
| Comparable     | No                        | Yes (`deep-eq`)         |
| Serializable   | No                        | Yes (packed, snap-shot) |
| Memory         | Heap-allocated env        | 16 bytes in VM          |
| GC required    | Yes                       | No                      |

---

## 12. Design Decisions

### 12.1 Why Native Types?

Curry and Thunk could be implemented with arrays and conventions.  Native
types provide:

- **Type safety.** `verify_operands(VerifyCurry)` and
  `verify_operands(VerifyThunk)` reject other types at the operator level.
- **Efficient dispatch.** `execute_curry()` is a direct C++ function call,
  not an interpreted proc.
- **Packed encoding.** Curry uses `PackedType::Curry` for compact storage in
  proc bodies.
- **Inspectability.** `uncurry`, `thunk-evaluated?`, `is-curry`, `is-thunk`
  provide type-specific operations that would be impossible or convention-
  dependent with arrays.

### 12.2 Why Curry Instead of Closures?

Closures require a captured environment -- a snapshot of the enclosing scope.
This requires either:
- A garbage-collected heap to manage environment lifetimes.
- Reference counting with cycle detection.
- Copy-on-capture semantics with deep copies.

Curry avoids all of these by capturing exactly one value.  The value is stored
in the VM heap (bump-allocated, no GC) and reclaimed by save/restore or
destructor.  For multi-argument capture, nested curries compose cleanly.

### 12.3 Why Curry is Always Executable

If a Curry could be made Literal, it would lose its execution semantics --
pushing it to the operand stack would always push the Curry object rather than
dispatching it.  Since the entire purpose of Curry is to be executed, making
it Literal would render it inert.  The `make-literal` restriction prevents
accidental deactivation.

Additionally, the packed encoding uses the X-bit (Executable attribute) to
discriminate Curry (X=1) from Tagged (X=0).  If Curry could be Literal (X=0),
this discrimination would break.

### 12.4 Why Thunk is Always Literal

Thunks are data containers, not procs.  Making a thunk Executable would
cause the interpreter to attempt to dispatch it, which would be meaningless --
the thunk's proc should only run via `force`, which manages the
evaluation state and caching.

### 12.5 Why force Passes Through Non-Thunks

Pass-through enables generic code that works with both lazy and eager values.
A function that accepts "a value or a thunk that produces a value" can simply
call `force` without checking the type first:

```
/get-value { force } def      % works for both thunks and raw values

42 get-value                   % => 42 (pass-through)
{ 42 } thunk get-value         % => 42 (forced)
```

This is the same design choice as Scheme's `force` (SRFI-45), which is a
no-op on non-promise values.

### 12.6 Why Thunk Auto-Retries After Error

When a thunk's proc raises an error during evaluation, the error
propagates to the caller and the thunk's state is **reset to Unevaluated** (it
is *not* left half-evaluated or cached).  The next `force` therefore simply
**re-runs the proc**.  This is deliberate:

- A force that ends in an error caches nothing — only a successful force
  records a result — so there is no stale value to return.
- If the underlying problem persists, the retry replays the same original
  error; if it has been resolved (e.g. a transient resource is now available),
  the retry succeeds and caches the result.
- This mirrors the reactive-cell evaluation contract (the unwinder swaps
  `@force-complete` for `@force-fail`, which journals and resets the state).

`thunk-reset` also forces re-evaluation, but is not required for error recovery
— the error path already reset the state.  Note that `save`/`restore` likewise
resets thunk state, providing a transactional retry mechanism.  (See §6.7 for
a worked retry example.)

### 12.7 Why Thunks Are Not Packed

Packed arrays encode proc bodies and data for compact storage.  Thunks
contain mutable state (evaluation status, cached result) that changes at
runtime.  Packing a thunk would require serializing this state, which raises
questions:

- Should the packed thunk be unevaluated (losing the cache)?
- Should it be evaluated (freezing the cached value, including side effects)?
- What if the cached result references other thunks?

Since thunks are created at runtime (via the `thunk` operator) and are not
typically stored in proc bodies, the lack of packed encoding has no
practical impact.  The lazy sequence library creates thunks at runtime as
tail elements of `[head, thunk]` pairs.

