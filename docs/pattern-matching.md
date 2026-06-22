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

# Pattern Matching: Technical Reference

Name binding, destructuring, and multi-arm dispatch for Trix.

---

## 1. What Pattern Matching Is

Pattern matching in Trix is three layers of increasing power:

| Layer | Operator                  | Purpose                                                   |
| ----- | ------------------------- | --------------------------------------------------------- |
| A     | `let`                     | Bind stack values to names in a new dict scope            |
| B     | `destructure`             | Extract fields from structured data into a new dict scope |
| C     | `match` / `cond` / `when` | Multi-arm dispatch: test values, execute the first match  |

Each layer solves a specific pain point of stack-based programming:

<!-- doctest: skip (synopsis; placeholder person/response receivers + undefined helpers) -->
```
% Layer A: name N stack values instead of juggling with exch/roll
10 20 30 [/x /y /z] let
  x y add z mul        % clear, readable
end

% Layer B: extract fields by name from structured data
person [/name /age] destructure
  name ( is ) concat age 10 string to-string concat =
end

% Layer C: dispatch on value properties
response [
  { tag-name /ok eq }    { tag-value process }
  { tag-name /error eq } { tag-value log-error }
  { pop true }           { pop (unrecognized) }
] match
```

### 1.1 Why Pattern Matching Exists

Stack-based languages have a well-known ergonomic problem: values are
positional, not named.  When a proc receives 4 arguments, the
programmer must mentally track which value is at which stack depth.
`exch`, `roll`, `index`, and `copy` provide access but not clarity.

**Without pattern matching:**

<!-- doctest: skip (illustrative "before" fragment; assumes a response array + process-* helpers) -->
```
% Process a response: [status code message]
dup 0 get /status exch def
dup 1 get /code exch def
2 get /message exch def
status /ok eq { code message process-ok } { code message process-err } if-else
```

**With pattern matching:**

<!-- doctest: skip (illustrative "after" fragment; placeholder response + process-* helpers) -->
```
response [/status /code /message] destructure
  status [
    { /ok eq }    { pop code message process-ok }
    { /error eq } { pop code message process-err }
  ] match
end
```

The pattern matching version is:
- Self-documenting (names instead of indices)
- Scope-safe (names live in a dict scope, cleaned up by `end`)
- Exhaustiveness-visible (match arms are explicit, default catches all)


## 2. Quick Reference

### Layer A: Name Binding

```
let             val1 val2 ... valN [/n1 /n2 ... /nN] let
                % Pop N values from stack, bind to names in new dict scope.
                % name1 = val1 (deepest), nameN = valN (top of stack).
                % Close scope with `end`.
                % Error: empty name array, non-name element.
```

### Layer B: Destructuring

```
destructure     value [/field1 /field2 ...] destructure
                % Extract from value into new dict scope.
                % Array: by index (field1 = elem[0], field2 = elem[1], ...).
                % Record: by field name.
                % Dict: by key name.
                % Value is consumed.  Close scope with `end`.
                % Error: empty names, non-name element, field not found,
                %        name count > array length, wrong source type.
```

### Layer C: Multi-Arm Dispatch

```
match           value [{test}{body} {test}{body} ...] match -- result
                % Test procs receive a dup'd copy of value. MUST consume
                % it and push a boolean.
                % First true test: body executes with value on stack.
                % No match: raises /match.

when            value {test} {body} when -- result
                % Single-arm match.  If test true: body runs with value.
                % If false: value passes through unchanged on stack.

cond            [{test}{body} {test}{body} ...] cond -- result
                % General conditional (no value argument).
                % Test procs operate on current stack, push bool.
                % First true: body executes.  No match: raises /match.
```


## 3. Usage Patterns

### 3.1 let -- Basic Name Binding

```
10 20 [/x /y] let
  x y add               % => 30
end
```

Binding order: leftmost name gets the deepest stack value:

```
1 2 3 [/a /b /c] let % a=1, b=2, c=3
  a 1 eq             % true
  c 3 eq             % true
end
```

### 3.2 let -- Mixed Types

```
(hello) 3.14 true [/s /r /b] let
  s (hello) eq           % true
  r 3.14 eq              % true
  b true eq              % true
end
```

### 3.3 let -- Single Binding

```
42 [/x] let
  x 42 eq                % true
end
```

### 3.4 destructure -- Arrays

Extract by position:

```
[10 20 30] [/x /y /z] destructure
  x    % => 10
  y    % => 20
  z    % => 30
end
```

Partial extraction (fewer names than elements):

```
[1 2 3 4 5] [/first /second] destructure
  first    % => 1
  second   % => 2
end
```

### 3.5 destructure -- Records

Extract by field name:

```
/Person [/name /age] record-type def
(Alice) 30 Person

[/name /age] destructure
  name    % => (Alice)
  age     % => 30
end
```

Field order in the name array does not need to match the record schema
order -- lookup is by name, not position:

```
/Person [/name /age] record-type def
(Alice) 30 Person /p exch def
p [/age /name] destructure % age first, name second
  age                      % => 30
  name                     % => (Alice)
end
```

### 3.6 destructure -- Dicts

Extract by key:

```
<< /host (localhost) /port 8080 /proto (https) >> [/host /port] destructure
  host    % => (localhost)
  port    % => 8080
end
```

### 3.7 match -- Type Dispatch

```
42 [
  { is-string }  { pop (string) }
  { is-integer } { pop (integer) }
  { pop true }   { pop (other) }
] match
% => (integer)
```

The test procs receive a dup'd copy of the value.  Type predicates like
`is-string` and `is-integer` consume the value and push a boolean --
exactly the contract `match` requires.

### 3.8 match -- Value Dispatch

```
(hello) [
  { (world) eq }  { pop (greeting) }
  { (hello) eq }  { pop (salutation) }
  { pop true }    { pop (unknown) }
] match
% => (salutation)
```

### 3.9 match -- Tagged Value Dispatch

```
42 /ok tag [
  { tag-name /ok eq }    { tag-value 2 mul }
  { tag-name /error eq } { tag-value length }
  { pop true }           { pop -1 }
] match
% => 84
```

Each test proc receives its own dup'd copy of the value and must
consume it, pushing only a boolean.  `tag-name` consumes the Tagged
copy and pushes the tag's name, so `{ tag-name /ok eq }` satisfies the
contract directly -- no `dup` is needed (the body receives a separate
copy of the original value).  A test that does NOT consume its copy
(e.g. `{ dup tag-name /ok eq }`) still selects the right arm, but the
leftover copy stays on the operand stack beneath the result -- and in
`match-all` it is collected into the results.

### 3.10 match -- Default Arm

The default arm pattern is `{ pop true }` -- discard the dup'd value,
always match:

```
value [
  { is-integer } { 2 mul }
  { is-string }  { length }
  { pop true }   { pop null }    % default: discard value, push null
] match
```

### 3.11 match -- Computation in Body

The body proc receives the original value on the operand stack.  It can
transform it:

```
42 [
  { is-integer } { 10 mul }     % value on stack: 42 -> 420
] match
% => 420
```

### 3.12 when -- Single-Arm Match

```
42 { is-integer } { 2 mul } when
% => 84 (test passed, body executed)

(hello) { is-integer } { 2 mul } when
% => (hello) (test failed, value unchanged)
```

`when` is sugar for a 2-arm match where the default arm is a no-op.

### 3.13 cond -- Stack-Based Conditional

`cond` has no value argument.  Test procs operate on whatever is on the
stack and must push a boolean:

```
42
[
  { dup 100 gt }  { (big) }
  { dup 0 gt }    { (positive) }
  { true }        { (other) }
] cond
% => 42 (positive)  -- note: 42 is still on stack below (positive)
```

In cond mode, the body does NOT receive a value on the stack from the
match machinery.  It operates on whatever the current stack state is.

### 3.14 match with null Values

`match` correctly handles null values.  The internal sentinel for cond mode
is a Mark object (not null), so null values can be matched:

```
null [
  { is-null } { pop (was null) }
  { pop true } { pop (other) }
] match
% => (was null)
```

### 3.15 Nested match

```
[42 (hello)] [
  { is-array } {
    0 get [
      { is-integer } { 2 mul }
      { pop true }   { pop 0 }
    ] match
  }
  { pop true } { pop 0 }
] match
% => 84
```


## 4. Real-World Scenarios

### 4.1 Command Processor

```
/process-command {
  [
    { tag-name /quit eq }
      { pop (goodbye) = quit }
    { tag-name /echo eq }
      { tag-value = }
    { tag-name /add eq }
      { tag-value dup 0 get exch 1 get add }
    { pop true }
      { pop (unknown command) = }
  ] match
} def

(hello) /echo tag process-command     % prints: hello
[3 4] /add tag process-command        % => 7
```

### 4.2 HTTP Response Handler

```
/handle-response {
  [/status /body] destructure
    status [
      { 200 eq }    { pop body process-success }
      { 404 eq }    { pop body log-not-found }
      { 500 eq }    { pop body log-server-error }
      { pop true }  { pop body log-unknown-status }
    ] match
  end
} def
```

### 4.3 Tree Traversal with match

```
% Tree nodes: leaf values or /node tagged [left right]
/tree-sum {
  [
    { is-integer }            { }                % leaf: value is the sum
    { tag-name /node eq } {
      tag-value
      dup 0 get tree-sum       % sum left subtree
      exch 1 get tree-sum      % sum right subtree
      add
    }
    { pop true } { pop 0 }    % unknown: contribute 0
  ] match
} def
```

### 4.4 State Machine with cond

```
/state /idle def

/tick {
  [
    { state /idle eq }
      { /state /running store (started) }
    { state /running eq }
      { /state /done store (finished) }
    { true }
      { (already done) }
  ] cond
} def

tick    % => (started)
tick    % => (finished)
tick    % => (already done)
```

### 4.5 Record Pipeline with destructure

```
/Order [/item /qty /price] record-type def

/order-total {
  [/qty /price] destructure
    qty price mul
  end
} def

/order-summary {
  [/item /qty /price] destructure
    item (: ) concat
    qty price mul 10 string to-string concat
  end
} def

% qty is a Real (5.0) so qty*price type-checks — Trix arithmetic requires
% matching operand types; 5 (Integer) * 9.99 (Real) would raise /type-check.
(Widget) 5.0 9.99 Order
dup order-total =        % => 49.949997   (49.95 is not exactly representable)
order-summary =          % => (Widget: 49.949997)
```


## 5. Design Choices

### 5.1 let Creates a Dict Scope (Like begin/end)

`let` always pushes a new dict on the dict stack.  Names are defined in
this new dict.  `end` pops it.

**Why**: Scope safety.  The names created by `let` are automatically cleaned
up by `end`.  They cannot leak into outer scopes.  This matches the existing
`begin`/`end` pattern that Trix users already understand.

**Alternative considered**: Define names in the current dict.  Rejected --
this pollutes the current scope and provides no cleanup mechanism.

### 5.2 destructure Consumes the Value

The source value (array, record, dict) is consumed by `destructure`.  It
is not left on the stack.

**Why**: The extracted fields are the useful parts.  Keeping the source on
the stack requires an explicit `pop` in the common case.  If the user needs
the original, they can `dup` before destructuring.

### 5.3 match Test Procs Must Consume the Dup'd Value

Test procs receive a copy of the value and must consume it, leaving only
a boolean.  This is a strict contract.

**Why**: If test procs could leave the value on the stack, every failed
test would accumulate uncleaned values.  The match trampoline cannot know
what the test proc left behind.  By requiring consumption, the stack state
is predictable: one boolean on top, nothing else added.

Type predicates (`is-integer`, `is-string`, etc.) naturally satisfy this
contract -- they consume one value and push a boolean.  For value
comparisons, use patterns like `{ 42 eq }` (consumes value, pushes bool).

### 5.4 Mark Sentinel for cond Mode

Internally, `match` stores the value on the exec stack.  `cond` stores a
Mark object instead.  The `@match-test` trampoline checks for Mark to
decide whether to dup a value for the test proc.

**Why**: Null is a valid match value.  Using null as the "no value" sentinel
would make `null [...] match` impossible.  Mark is never a user value (it is
a syntax delimiter), so it is an unambiguous sentinel.

### 5.5 when as 2-Arm match Sugar

`when` is implemented as a match with two arms: `[test, body, always-true, nop]`.
The always-true arm is `{ pop true }` and the nop body is `{ }`.

**Why**: Reuses the match trampoline exactly.  No new control flow machinery.
The cost is three array allocations per `when` call: the 4-element pairs
array, a 2-element `{ pop true }` proc, and an empty `{ }` proc.

### 5.6 match-all (Gather Form)

`match-all: value pairs-array -- result-array` runs **every** test in pair
order against (a fresh copy of) the value; for each test returning true the
body runs with its own copy of the value, and everything the bodies push
accumulates -- in arm order -- into the result array.  A body may
contribute zero, one, or several values (collect-everything semantics).
Zero matching arms raises `/match`.

```
5 [ { 3 gt }       { pop (big) }
    { is-integer } { 2 mul }
    { pop false }  { pop (never) } ] match-all
% => [(big) 10]
```

**Mechanism**: mark-accumulation.  `match-all` plants an internal Mark on
the operand stack; the two-phase `@match-all-test` trampoline interleaves
tests, bodies, and collection bookkeeping, and the operand stack itself
roots every partial result (no GC-rooting window; a caught error discards
the mark and partials via the normal try rollback).

**Contracts and edges**:
- Tests follow the `match` contract: consume the dup'd value, push a bool.
  A test that leaks extra values adds them to the results.
- Bodies must keep mark balance: an unbalanced Mark left by a body shifts
  the collection boundary to it (the nearest mark wins, exactly as in any
  `[ ... ]` construction in progress).
- `exit` inside a test or body raises `/invalid-exit` -- exit cannot
  reconcile the accumulation mark.  Use `throw` + `try` for early
  termination.
- Empty or odd-length pairs arrays raise `/range-check`; a non-boolean
  test result raises `/type-check`.  Both sweep the partial results before
  raising, so the caller's stack is unchanged.


## 6. Implementation Internals

### 6.1 let Mechanics

```
let: val1 val2 ... valN [/n1 /n2 ... /nN] let

1. Verify: top of stack is an array.
2. Validate: non-empty, all elements are names.
3. Verify: op stack has N+1 values (array + N values).
4. Require: dict stack has capacity for 1 more dict.
5. Create ReadWriteFixed Dict with capacity = N.
6. Pop names array.
7. For i = N down to 1:
   Pop value from op stack.
   Put (name[i-1] -> value) in dict.
8. Push dict on dict stack.
9. Set name bindings on the dict.
```

The user calls `end` to pop the dict and clear its name bindings.

### 6.2 destructure Mechanics

```
destructure: value [/f1 /f2 ...] destructure

1. Verify: top = array (names), second = any (value).
2. Validate name array: non-empty, all names.
3. Type dispatch on value:
   - Array: verify names_count <= array_length, extract by index.
   - Record: for each name, find field index in schema, extract.
   - Dict: for each name, get by key.
   - Other: type error.
4. Create ReadWriteFixed Dict with capacity = N.
5. Put each (name -> extracted_value_clone) in dict.
6. Free ExtValue on source value.
7. Pop both operands.
8. Push dict on dict stack + set name bindings.
```

### 6.3 match Trampoline

The match trampoline uses the exec stack as a continuation:

```
match: value [test1 body1 test2 body2 ...] match

Setup (match_op):
  1. Clone value (for safe ownership).
  2. Free original value's ExtValue.
  3. Pop first test from pairs array.
  4. Push exec frame: [value, remaining-pairs, @match-test, first-test]
  5. Push dup'd value on op stack for first test proc.

@match-test (at_match_test_op):
  On entry: bool on op stack (from test proc).
  Exec frame: [..., value, remaining-pairs]

  If true:
    Pop body from remaining-pairs.
    Push value on op stack (for body to use).
    Pop exec frame.
    Push body on exec stack.

  If false:
    Pop body (skip it).
    If no more pairs: raise /match error.
    Pop next test from remaining-pairs.
    Push dup'd value on op stack for next test.
    Push @match-test + next-test on exec stack.
```

### 6.4 Memory Cost

- `let`: One Dict allocation: 16-byte header + bucket array (bucket_count_for_capacity(N) * 4 bytes) + N*20-byte entries (e.g. N=2 -> 16 + 12 + 40 = 68 bytes).
- `destructure`: One Dict allocation: 16-byte header + bucket array (bucket_count_for_capacity(N) * 4 bytes) + N*20-byte entries (e.g. N=2 -> 16 + 12 + 40 = 68 bytes) + N value clones.
- `match`: One value clone per test attempt (freed on failure).  One
  pairs array (4-element minimum).  `when` allocates a 4-element array,
  a 2-element true-proc, and an empty nop-proc.

### 6.5 Execution Cost

- `let`: N dict-put operations + 1 dict-stack push.  O(N).
- `destructure`: N field lookups + N dict-put operations.
  Array: O(N) (index access).  Record: O(N*F) worst case where F = field
  count (linear scan per field).  Dict: O(N) amortized (hash lookup).
- `match`: Per arm: 1 value clone + test proc execution + 1 boolean check.
  Best case (first arm matches): 1 test.  Worst case: K tests where K is
  the number of arms.

### 6.6 Source File

All pattern matching operators are in `src/ops_match.inl` (~475 lines).


## 7. Composability

### 7.1 Pattern Matching + Protocols

Use `protocol-satisfies?` as match predicates:

<!-- doctest: skip (illustrative; placeholder value + undefined protocols/process helpers) -->
```
value [
  { dup /Stringify protocol-satisfies? }
    { to-str process-string }
  { dup /Renderable protocol-satisfies? }
    { render process-rendered }
  { pop true }
    { pop (unknown) }
] match
```

### 7.2 Pattern Matching + Records

`destructure` works natively with records:

```
/Point [/x /y] record-type def
% Real coordinates: sqrt requires a Real/Double (an Integer sum raises /type-check)
3.0 4.0 Point [/x /y] destructure
  x x mul y y mul add sqrt    % distance from origin => 5.0
end
```

### 7.3 Pattern Matching + Tagged ADTs

Dispatch on the tag name to process an algebraic data type.  Node shapes:

- `N /lit tag` — a literal leaf holding a number.
- `[leftNode rightNode] /add tag` / `[leftNode rightNode] /mul tag` — a binary
  node whose payload is an array of its two child nodes.

The binary arms recurse on both children, so a helper with per-call `|locals|`
holds the operands across the recursion (frame locals survive recursion, unlike
a global `def`).  Explicit `tag-name` dispatch keeps the stack predictable here;
`match` passes the value to the body as a single copy on the stack, so a
recursive evaluator must manage that one value explicitly.

```
/eval-binop { |left right opname|
  left eval-expr
  right eval-expr
  opname /add eq { add } { mul } if-else
} def

/eval-expr { |node|
  node tag-name /lit eq
  { node tag-value }
  { node tag-value 0 get
    node tag-value 1 get
    node tag-name
    eval-binop
  } if-else
} def

5 /lit tag /five exch def
3 /lit tag /three exch def
[ five three ] /add tag eval-expr                          % => 8
[ [ five three ] /add tag  2 /lit tag ] /mul tag eval-expr % => 16  ((5+3)*2)
```

### 7.4 Pattern Matching + Closures

Capture match context in closures:

<!-- doctest: skip (illustrative; placeholder response + undefined process/handle-unexpected) -->
```
/expected /ok def
{ tag-name expected eq } [/expected] closure-capture /is-ok exch def

response [
  { is-ok }     { tag-value process }
  { pop true }  { pop handle-unexpected }
] match
```

### 7.5 Pattern Matching + Contracts

<!-- doctest: skip (illustrative; placeholder value receiver) -->
```
value [
  { is-integer } {
    dup 0 ge precondition      % guard in match body
    sqrt
  }
  { pop true } { pop 0.0 }
] match
```

### 7.6 let + destructure (Layered)

```
% Bind computed values, then destructure a field
config [/db-config /app-name] destructure
  db-config [/host /port] destructure
    host port app-name
    % ... use all three names ...
  end
end
```


## 8. Error Handling

| Error                | Condition                                               |
| -------------------- | ------------------------------------------------------- |
| `/range-check`       | Empty name array (`let`, `destructure`)                 |
| `/range-check`       | Name count > array length (`destructure` with array)    |
| `/range-check`       | Empty pairs array (`match`, `cond`)                     |
| `/range-check`       | Odd-length pairs array (`match`, `cond`)                |
| `/type-check`        | Non-name in name array (`let`, `destructure`)           |
| `/type-check`        | Non-array/record/dict source (`destructure`)            |
| `/type-check`        | Test proc returns non-boolean (`match`, `cond`)         |
| `/undefined`         | Field/key not found (`destructure` with record or dict) |
| `/match`             | No test matched (`match`, `cond`, `match-all`)          |
| `/opstack-underflow` | Fewer stack values than names (`let`)                   |
| `/invalid-exit`      | `exit` inside a `match-all` test or body (see §5.6)     |


## 9. Limitations

- **No nested destructuring.** `destructure` extracts one level.  For nested
  structures, chain multiple `destructure` calls or use `get` to reach
  deeper levels.

- **No wildcard patterns.** Every name in the name array is bound.  There is
  no `_` or "skip this position" syntax.  For arrays, use a dummy name.

- **`match` is first-match; `match-all` is the gather form.** Both are implemented (see Section 5.6).

- **destructure for arrays is positional.** Field names map to indices
  (name[0] -> elem[0]).  There is no way to skip elements or match by value
  in the array.

- **cond test procs must not leave extra values.** Since cond has no value
  argument, test procs must carefully manage the stack.  Leaving extra
  values on the stack between test and body is a user bug that the runtime
  cannot detect.

- **match body consumes the value.** After match, the original value is
  gone (it was passed to the body proc).  If you need the value after
  match, the body must explicitly leave it on the stack or dup it.
