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

# Closures: Technical Reference

Explicit-capture closures for Trix.

---

## 1. What Closures Are

A closure is a proc bundled with a snapshot of named bindings from its
creation scope.  When the closure executes, the captured bindings are
temporarily visible on the dict stack, making the captured names
available to the proc body as if they were local definitions.

```
/x 5 def
{ x x mul } [/x] closure-capture    % captures x=5

/x 999 def % change x in current scope
exec       % => 25 (uses captured x=5)
```

The captured values are frozen at creation time.  Changes to the original
names after capture have no effect on the closure.

### 1.1 Why Closures Exist

Trix already has several mechanisms for binding values to procs:

| Mechanism    | Syntax                                | Values Captured                   |
| ------------ | ------------------------------------- | --------------------------------- |
| `curry`      | `value proc curry`                    | 1 value (pushed before proc runs) |
| Nested curry | `v1 { v2 proc curry } curry`          | 2+ values (verbose, nests)        |
| `//` binding | `{ //name ... }`                      | 1 name (scan-time, immediate)     |
| `compose`    | `proc1 proc2 compose`                 | 0 values (chains two procs)       |
| **Closure**  | `{ body } [/a /b /c] closure-capture` | N values (explicit, clean)        |

The gap: capturing 3+ values requires verbose nested curries.  An explicit
capture operator provides ergonomic multi-value closure creation in one
expression.

```
% Without closures: nested curry (captures a, b, c)
/a 10 def /b 20 def /c 30 def
a { exch b { exch c { 3 -1 roll 3 -1 roll add add } curry } curry } curry
% ... painful

% With closures:
{ a b add c mul } [/a /b /c] closure-capture
% ... done
```

### 1.2 Closures vs Other Approaches

**`//` (scan-time binding)**: Resolves one name at scan time, embedding the
value in the proc.  Fast, zero overhead, but only works for values known
at scan time and only binds one name per `//`.

**`curry`**: Pairs one value with a proc.  The value is pushed on the
operand stack before the proc runs.  The proc must `exch` or `roll` to
position it.  Works well for 1 value; awkward for many.

**Closures**: Look up N names in the current dict stack, build a ReadOnly
dict, and wrap it with the proc.  When executed, the dict is pushed on
the dict stack, so captured names are accessible by name -- no stack
juggling.


## 2. Quick Reference

```
closure-capture     proc name-array -- closure
                    % Look up each name in current dict stack.
                    % Build ReadOnly dict with those bindings.
                    % Return Curry [dict, callable].
                    % Error: empty name array, non-name element,
                    %        undefined name.

closure-with-dict   dict proc --
                    % Push dict on dict stack, execute proc, pop dict.
                    % The primitive: closure-capture builds on this.
                    % Error: non-dict, non-proc arguments.

closure-env         closure -- dict
                    % Extract captured bindings dict from a closure.
                    % The closure must be a Curry whose value is a Dict.
                    % Error: non-curry argument, or curry value is not dict.
```


## 3. Usage Patterns

### 3.1 Basic Capture

Capture one or more names from the current scope:

```
/a 10 def /b 20 def
{ a b add } [/a /b] closure-capture /add-ab exch def

add-ab               % => 30
```

### 3.2 Frozen Values

Captured values are snapshots.  The closure is immune to later changes:

```
/x 5 def
{ x x mul } [/x] closure-capture /sq5 exch def

/x 999 def % modify x
sq5        % => 25 (still uses x=5)
```

### 3.3 Closure as Callback

Closures are regular Trix values.  Pass them to higher-order operators:

```
/threshold 10 def
{ threshold gt } [/threshold] closure-capture /above10 exch def

[5 15 3 20 8 12] { above10 } filter
% => [15 20 12]
```

### 3.4 Closure Factory

A proc that returns closures with different captured values:

```
/make-adder {
    /n exch def
    { n add } [/n] closure-capture
} def

3 make-adder /add3 exch def
10 make-adder /add10 exch def

5 add3                % => 8
5 add10               % => 15
```

### 3.5 Capturing Closures (Nested Closures)

A closure can capture another closure, enabling composition:

```
/x 10 def
{ x mul } [/x] closure-capture /times-x exch def

/y 3 def
{ y times-x } [/y /times-x] closure-capture exec
% => 30 (y=3, times-x multiplies by 10)
```

### 3.6 Multiple Closures, Independent Captures

Each `closure-capture` call creates an independent snapshot:

```
/v 1 def
{ v } [/v] closure-capture /cl1 exch def
/v 2 def
{ v } [/v] closure-capture /cl2 exch def
/v 3 def
{ v } [/v] closure-capture /cl3 exch def

cl1        % => 1
cl2        % => 2
cl3        % => 3
```

### 3.7 Capturing Shadowed Names

`closure-capture` looks up names in the current dict stack, respecting
shadowing.  If a name is shadowed by `begin`/`end` scope, the shadowed
value is captured:

```
/w 100 def
<< /w 42 >> begin
  { w } [/w] closure-capture       % capture the shadowed w=42
end
/cl exch def                        % def AFTER end -- def inside would land in
                                    % the pushed dict, which end then discards

cl                    % => 42 (captured the shadowed w, not outer w=100)
```

### 3.8 closure-with-dict (The Primitive)

`closure-with-dict` is the foundation that `closure-capture` builds on.
It pushes a dict onto the dict stack, runs a proc, and pops the dict:

```
<< /x 10 /y 20 >> { x y mul } closure-with-dict
% => 200
```

This is like `begin ... end` but with an existing dict instead of
creating a new one.  Scope isolation is guaranteed:

```
/z 100 def
<< /z 7 >> { z } closure-with-dict
% => 7 (z is shadowed by the dict)
z                     % => 100 (original z is intact)
```

### 3.9 Inspecting Captured Bindings

`closure-env` extracts the captured dict from a closure:

```
/k1 10 def /k2 20 def
{ k1 k2 add } [/k1 /k2] closure-capture /cl exch def

/cl load closure-env
% => << /k1 10 /k2 20 >> (ReadOnly dict)

dup /k1 get =        % => 10
/k2 get =            % => 20
```

The returned dict is ReadOnly (same dict that is inside the closure).

### 3.10 Closure Invoked Multiple Times

Closures are reusable.  Each invocation temporarily pushes the captured
dict, runs the body, and pops the dict:

```
/n 7 def
{ n n mul } [/n] closure-capture /sq7 exch def

sq7        % => 49
sq7        % => 49
sq7        % => 49
```

### 3.11 Error in Closure Body

If the closure body raises an error, the captured dict is still popped
from the dict stack.  The `@closure-end` control operator and its handlers
in `exit_op`, `stop_op`, and `try_catch_handler` ensure cleanup:

```
/guard 5 def
{ << /guard 99 >> { 1 0 div } closure-with-dict } try
% => /div-by-zero (error caught)
guard                 % => 5 (dict stack properly restored)
```

### 3.12 Large Capture

Capture many names efficiently:

```
/s1 1 def /s2 2 def /s3 3 def /s4 4 def
/s5 5 def /s6 6 def /s7 7 def /s8 8 def

{ s1 s2 add s3 add s4 add s5 add s6 add s7 add s8 add }
[/s1 /s2 /s3 /s4 /s5 /s6 /s7 /s8] closure-capture exec
% => 36
```


## 4. Real-World Scenarios

### 4.1 Event Handler Registration

```
/on-click {
    /handler-name exch def
    /action exch def
    { action handler-name (: fired) concat = }
    [/action /handler-name] closure-capture
} def

{ (save completed) = } (save-btn) on-click /save-handler exch def
{ (data loaded) = }    (load-btn) on-click /load-handler exch def

save-handler           % prints: save completed\nsave-btn: fired
```

### 4.2 Accumulator Pattern

```
/make-counter {
    /count 0 def
    {
        count 1 add /count exch store
        count
    } [/count] closure-capture
} def
```

Note: This captures count=0 at creation time.  The `store` inside the
closure body finds `count` in the captured **ReadOnly** dict and raises
`/read-only` (the closure crashes) — it does **not** fall through to
userdict.  For mutable state, use a Cell (`cell` / `cell-set` /
`cell-get`) and capture the cell reference.

### 4.3 Configuration Injection

```
/make-formatter {
    /config exch def
    {
        config /prefix get exch concat
        config /suffix get concat
    } [/config] closure-capture
} def

<< /prefix ([ ) /suffix ( ]) >> make-formatter /bracket exch def
(hello) bracket       % => ([ hello ])
```

### 4.4 Closure + Protocol Integration

Register closures as protocol method implementations:

```
[/to-url] /UrlProto def-protocol

/base-url (https://api.example.com) def
{ base-url (/) concat exch concat } [/base-url] closure-capture
/to-url-impl exch def

% A closure is a Curry; bind it to a name and register a proc that calls it,
% because def-method requires a plain Proc.
{ to-url-impl } /to-url /string-type def-method

(users) to-url        % => (https://api.example.com/users)
```

### 4.5 Closure in Transducer Pipeline

`xf-map`/`xf-filter` require a `{ }` Proc, not a Curry — and a closure *is* a
Curry.  Bind the closure to a name and wrap that name in a proc (`{ shift }`)
for the transducer step:

```
/offset 100 def

{ offset add } [/offset] closure-capture /shift exch def
[1 2 3 4 5] { shift } xf-map into
% => [101 102 103 104 105]
```


## 5. Design Choices

### 5.1 Closure = Curry [Dict, Callable]

A closure is represented as a Curry object.  The Curry value slot holds the
captured dict; the Curry callable slot holds the wrapped proc.

**Why**: Curry is the existing mechanism for pairing a value with a callable.
No new type required.  The Curry execution path pushes the value (dict) onto
the operand stack, then executes the callable.  The callable moves the dict
to the dict stack via `closure-with-dict`.

**Alternative considered**: New closure type (using the 1 remaining type
slot -- 31 of the 32 five-bit slots are defined).  Rejected -- Curry already
does exactly what is needed, and type slots are scarce.

### 5.2 Captured Dict is ReadOnly

The dict built by `closure-capture` is set to ReadOnly after construction.

**Why**: Captures are snapshots, not live references.  A ReadOnly dict
prevents accidental mutation of captured bindings.  A `def` inside a
closure body does **not** reach userdict: `def` targets the topmost
non-frame dict, which while the closure runs is the captured ReadOnly
dict, so it raises `/read-only`.  Mutate state through a captured Cell
instead (see §4.2).

### 5.3 Explicit Name List

`closure-capture` requires an explicit list of names to capture.  There is
no "capture everything" or "capture by reference" mode.

**Why**: Explicitness eliminates ambiguity about what is captured and
prevents accidentally dragging large or sensitive values into the closure.
The name list serves as documentation of the closure's dependencies.

### 5.4 exit Across Closure Boundaries

`exit` (loop exit) works correctly across closure boundaries.  If a closure
is called inside a loop and the closure body calls `exit`, the `@closure-end`
handler on the exec stack is cleaned up by `exit_op`, which scans for and
pops `@closure-end` control operators.

**Why**: Closures should be transparent to control flow.  A closure is a
scoping mechanism, not a control boundary.  Users expect `exit` to exit the
enclosing loop, not to be trapped by the closure.


## 6. Implementation Internals

### 6.1 closure-capture Mechanics

<!-- doctest: skip (notation: annotated synopsis of the mechanism, not runnable) -->
```
closure-capture: proc [/a /b /c] closure-capture

1. Validate: non-empty name array, all elements are names.
2. For each name, look up in dict stack via Name::name_search.
   Error if any name is undefined.
3. Create ReadWriteFixed Dict with capacity = name count.
4. Put each name -> value (cloned) into the dict.
5. Set dict to ReadOnly.
6. Build callable: [proc-literal, make-executable, closure-with-dict]
   - proc-literal: the user's proc with literal attribute (so Curry
     pushes it to op stack without executing it).
   - make-executable: restores executable attribute on the op-stack proc.
   - closure-with-dict: pops dict + proc, pushes dict on dict stack,
     executes proc, pops dict.
7. Build Curry [dict, callable] via make_curry_pair.
8. Push closure on op stack.
```

### 6.2 Execution Flow

When a closure is invoked (the Curry object is executed):

```
Curry execution:
  1. Push dict (Curry value) onto operand stack.
  2. Execute callable (Curry callable) on exec stack.

Callable = [proc-literal, make-executable, closure-with-dict]:
  3. proc-literal pushes the proc onto operand stack.
  4. make-executable makes it executable.
  5. closure-with-dict:
     a. Pop proc and dict from operand stack.
     b. Push dict onto dict stack, set name bindings.
     c. Push @closure-end control op onto exec stack.
     d. Push proc onto exec stack.
  6. Proc executes with captured names visible via dict stack.
  7. @closure-end fires: pop dict from dict stack, clear name bindings.
```

### 6.3 @closure-end Cleanup

`@closure-end` is a ControlOp that pops the closure's dict from the dict
stack.  It fires in three situations:

- **Normal completion**: After the closure body finishes, `@closure-end`
  is the next item on the exec stack.
- **Error/stop**: `try_catch_handler` and `stop_op` scan for `@closure-end`
  on the exec stack and execute cleanup.
- **Loop exit**: `exit_op` scans for `@closure-end` on the exec stack
  (between the current position and the loop boundary) and executes cleanup.

This three-way cleanup ensures the dict stack is never corrupted, regardless
of how control leaves the closure body.

### 6.4 Memory Cost

| Component                   | Size                                    |
| --------------------------- | --------------------------------------- |
| Captured dict (N names)     | 40 + N*20 bytes (Dict header + entries) |
| Callable array (3 elements) | 24 bytes (3 Objects)                    |
| Curry pair                  | 16 bytes (2 Objects)                    |
| **Total per closure**       | **80 + N*20 bytes**                     |

A closure capturing 3 names: ~140 bytes.
A closure capturing 8 names: ~240 bytes.

### 6.5 Execution Cost

Closure invocation:

1. Curry dispatch: push value + execute callable (~3 ops).
2. `closure-with-dict`: dict stack push + set name bindings (~2 ops).
3. Body execution: same as any proc.
4. `@closure-end`: dict stack pop + clear name bindings (~2 ops).

Overhead vs direct proc: ~7 extra operations per invocation, dominated by
the dict stack push/pop.  Name lookups inside the body hit the captured dict
first (top of dict stack), so they are fast -- same as looking up names in
a `begin`/`end` scope.

### 6.6 Snap-Shot/Thaw

Closures are Curry objects.  Curry objects live on the VM heap.  The
captured dict lives on the VM heap.  Both are captured by snap-shot and
restored by thaw.  No special handling required.

### 6.7 Source Files

- Closure operators: `src/ops_higher.inl` (`closure_capture_op` /
  `closure_with_dict_op` / `closure_env_op`)
- `@closure-end` handler in `exit_op`: `src/ops_flow.inl`
- `@closure-end` handler in `try_catch_handler`: `src/ops_system.inl`
- `@closure-end` handler in `stop_op`: `src/ops_flow.inl`


## 7. Composability

### 7.1 Closures + Protocols

Closures can serve as protocol method implementations that carry
configuration:

```
[/display] /DisplayProto def-protocol

/prefix (>> ) def
{ prefix exch 32 string to-string concat } [/prefix] closure-capture /display-impl exch def

% A closure is a Curry; bind it to a name and register a proc that calls it,
% because def-method requires a plain Proc.
{ display-impl } /display /integer-type def-method
```

### 7.2 Closures + Pattern Matching

Use closures as match test or body procs:

<!-- doctest: skip (synopsis: value/process-small/process-large are stand-ins) -->
```
/limit 100 def
{ limit lt } [/limit] closure-capture /below-limit exch def

value [
    { below-limit } { process-small }
    { pop true }    { process-large }
] match
```

### 7.3 Closures + Transducers

Closures in transducer steps.  A closure is a Curry, but `xf-map` needs a
`{ }` Proc — bind the closure and wrap it in a proc for the step:

```
/base 1000 def
{ base add } [/base] closure-capture /add-base exch def
[1 2 3 4 5] { add-base } xf-map into
% => [1001 1002 1003 1004 1005]
```

### 7.4 Closures + Actors

Capture configuration for actor message handlers:

```
/config << /max-retries 3 /timeout 5000 >> def
{ config /max-retries get ... } [/config] closure-capture
```

### 7.5 Closures + Reactive Cells

Capture cell references for watcher callbacks:

```
/log-cell (log) cell def
{
    log-cell cell-get exch concat log-cell cell-set
} [/log-cell] closure-capture /log-fn exch def
```


## 8. Error Handling

| Error          | Condition                                          |
| -------------- | -------------------------------------------------- |
| `/range-check` | Empty name array (`closure-capture`)               |
| `/type-check`  | Non-name element in name array (`closure-capture`) |
| `/undefined`   | Name not found in dict stack (`closure-capture`)   |
| `/type-check`  | Non-dict first argument (`closure-with-dict`)      |
| `/type-check`  | Non-proc second argument (`closure-with-dict`)     |
| `/type-check`  | Non-curry argument (`closure-env`)                 |
| `/type-check`  | Curry value is not a dict (`closure-env`)          |


## 9. Limitations

- **No capture-by-reference.** Closures snapshot values.  Mutations to the
  original names are not reflected.  For shared mutable state, use Cells.

- **No automatic capture.** The name list is mandatory.  There is no
  "capture all free variables" mode.  This is by design (see 5.3).

- **ReadOnly captured dict.** Inside the closure body, both `def` and a
  `store` of an unbound name raise `/read-only`: the resolver picks the
  topmost non-frame dict, which during closure execution is the captured
  ReadOnly dict.  To mutate state, capture a Cell (see 4.2) rather than
  relying on `def`/`store` falling through to userdict.

- **Dict stack depth.** Each closure invocation uses one dict stack slot.
  Deep nesting of closure calls can exhaust the dict stack (default depth
  64, `--dictionary-depth`).  This is rarely a practical issue.

- **No partial application sugar.** To partially apply a closure, you must
  create a new closure that captures the partial arguments.  There is no
  built-in `partial` operator.
