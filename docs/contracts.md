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

# Contracts: Technical Reference

Precondition and postcondition checking for Trix.

---

## 1. What Contracts Are

Contracts are lightweight runtime assertions that guard domain invariants.
A **precondition** asserts that inputs are valid before proceeding.  A
**postcondition** asserts that outputs are valid after computation completes.

```
% Precondition: value must be non-negative
25.0 dup 0.0 ge precondition sqrt    % => 5.0

% Postcondition: result must be positive
{ 0 gt } { 3 4 mul } postcondition   % => 12
```

Contracts are not type checks -- Trix's type system handles that
(31 types, 30 user-visible, no implicit coercion, `verify_operands` in
every operator).
Contracts check **domain constraints**: "this integer must be positive",
"this array must be non-empty", "this string must match a pattern."

### 1.1 Why Contracts Exist

Trix's static type system catches type mismatches (passing a String where
an Integer is expected).  But many bugs involve values of the correct type
with the wrong domain:

```
% Type-correct but domain-wrong (the engine catches each domain error):
-1.0 sqrt            % => /range-check (negative input; type is fine)
[] 0 get             % => /index-check (index past empty array)
10 0 div             % => /div-by-zero (zero divisor; types are fine)
```

Contracts catch these at the boundary where they originate, not where they
crash.  The error message says "precondition failed" at the point of
violation, not "division by zero" three procs deep.

### 1.2 Contracts in the Trix Ecosystem

| Mechanism         | Checks             | When                 | Error                               |
| ----------------- | ------------------ | -------------------- | ----------------------------------- |
| `verify_operands` | Type correctness   | Every operator entry | `/type-check`, `/opstack-underflow` |
| `precondition`    | Domain constraints | User-chosen points   | `/require`                          |
| `postcondition`   | Output validity    | After computation    | `/ensure`                           |
| `try`/`try-catch` | Any error          | User-chosen scope    | Catches all                         |

Contracts complement, not replace, the type system.  They are opt-in
assertions placed at API boundaries and critical computation points.


## 2. Quick Reference

```
precondition        bool --
                    % Pop boolean.  If false: raise /require error.
                    % If true: no-op (value already consumed).
                    % Error: non-boolean argument (/type-check).

postcondition       check body -- result
                    % check: proc that takes a value, returns bool.
                    % body: proc that produces the result.
                    % Execute body.  Dup result.  Run check on the copy.
                    % If check returns false: raise /ensure error.
                    % If true: body's result remains on stack.
                    % Error: non-proc arguments (/type-check),
                    %        check returns non-bool (/type-check).

postcondition-verify bool --
                    % Internal operator used by @ensure-check.
                    % Like precondition but raises /ensure instead of /require.
                    % Not typically called directly by users.
```


## 3. Usage Patterns

### 3.1 Simple Precondition

The pattern: compute the condition, then `precondition`.  The value being
checked is typically dup'd first so it remains on the stack:

```
42 dup 0 ge precondition       % passes: 42 >= 0, stack: 42
-1 dup 0 ge precondition       % fails: raises /require
```

### 3.2 Precondition in a Proc

Guard function entry points:

```
/safe-sqrt { dup 0.0 ge precondition sqrt } def

25.0 safe-sqrt       % => 5.0
-1.0 safe-sqrt       % raises /require
```

### 3.3 String Precondition

```
/parse-name {
    dup length 0 gt precondition     % non-empty
    % ... parse logic ...
} def

(hello) parse-name   % passes
() parse-name        % raises /require
```

### 3.4 Chained Preconditions

Multiple domain constraints on the same value:

```
42
dup 0 ge precondition        % non-negative
dup 100 lt precondition      % less than 100
dup is-integer precondition  % is integer (type + domain)
dup 2 mod 0 eq precondition  % is even
% stack: 42 (passed all checks)
```

### 3.5 Simple Postcondition

The pattern: `{ check } { body } postcondition`.  The check proc receives
a copy of the body's result and must return a boolean:

```
{ 0 ge } { 42 } postcondition        % => 42 (42 >= 0)
{ 0 gt } { -5 } postcondition        % raises /ensure (-5 not > 0)
```

### 3.6 Postcondition on Computation

```
{ 0 gt } { 3 4 mul } postcondition   % => 12 (12 > 0)
```

### 3.7 Postcondition with Type Check

```
{ is-string } { (hello) } postcondition    % => (hello)
{ is-string } { 42 } postcondition         % raises /ensure
```

### 3.8 Postcondition on Complex Result

```
{ length 3 eq } { [1 2 3] sort } postcondition
% => [1 2 3] (sorted array has length 3)
```

### 3.9 Nested Postconditions

Postconditions compose -- an outer postcondition can wrap an inner one:

```
{ 0 gt } { { 0 ge } { 42 } postcondition } postcondition
% => 42 (inner: 42 >= 0, outer: 42 > 0)
```

### 3.10 Postcondition Preserves Stack Below

Values below the postcondition's scope are undisturbed:

```
99 { 0 gt } { 42 } postcondition
% stack: 99 42 (99 is preserved)
```

### 3.11 Catching Contract Failures

Use `try` to catch contract violations without crashing:

```
{ -1 dup 0 ge precondition } try
% => /require (error name on stack)

{ { 0 gt } { -5 } postcondition } try
% => /ensure (error name on stack)
```

### 3.12 Precondition in Loops

Guard loop body invariants:

```
42 10 { dup 0 ge precondition } repeat pop
% Checks that 42 >= 0 ten times (always passes)
```

### 3.13 postcondition-verify (Direct Use)

Rarely needed, but available for custom postcondition patterns:

```
true postcondition-verify    % passes (no-op)
false postcondition-verify   % raises /ensure
```

### 3.14 ExtValue Types

Contracts work with all types, including 64-bit ExtValue types:

```
{ 0l gt } { 42l } postcondition         % => 42l
{ 0.0d gt } { 3.14d } postcondition     % => 3.14d
```


## 4. Real-World Scenarios

### 4.1 API Input Validation

```
/create-user {
    % name age email on stack
    dup length 0 gt precondition       % email non-empty
    exch dup 0 gt precondition         % age positive
    dup 150 lt precondition            % age reasonable
    exch dup length 0 gt precondition  % name non-empty
    % ... proceed with valid inputs ...
} def
```

### 4.2 Safe Division

```
/safe-div {
    dup 0 ne precondition
    div
} def

10 3 safe-div    % => 3 (integer division)
10 0 safe-div    % raises /require (not /div-by-zero)
```

The value of catching at the precondition: the error says "precondition
failed" at the `safe-div` call site, not "division by zero" inside the
arithmetic.  The programmer knows immediately that the bug is in the
caller (who passed 0), not in the division logic.

### 4.3 Sorted Array Guarantee

```
/is-sorted {
    dup length 2 lt
    { pop true }
    {
        /arr exch def
        true                          % accumulator
        0 1 arr length 2 sub
        { /i exch def
          arr i get arr i 1 add get le
          and
        } for
    } if-else
} def

/sorted-merge {
    % two sorted arrays on stack
    { is-sorted } { concat sort } postcondition
} def
```

### 4.4 Record Field Validation

```
/point [/x /y] record-type def

/make-valid-point {
    % x y on stack
    exch dup is-integer precondition   % x is integer
    exch dup is-integer precondition   % y is integer
    point
} def

3 4 make-valid-point    % => record{x=3, y=4}
```

### 4.5 Defensive GenServer Handler

```
<<
  /init { << /count 0 >> }
  /handle-call {
    [/msg /from /state] let
      msg dup is-integer precondition   % only accept integers
      state /count get add              % new count = old count + msg
      state exch /count exch put        % store new count back into state
      state                             % state'
      state /count get                  % reply value = updated count
      /reply
    end
  }
>> gen-server
```


## 5. Design Choices

### 5.1 precondition Takes a Boolean, Not a Proc

`precondition` consumes a boolean from the stack.  It does not take a
predicate proc like postcondition does.

**Why**: Preconditions check values that are already on the stack.  The
natural pattern is `dup <condition> precondition` -- compute the condition
inline, then assert.  This is simpler and more flexible than passing a
proc, because the condition can involve any number of stack values in
any combination.

**Alternative considered**: `value { pred } precondition` (proc-based).
Rejected -- it forces the check into a proc even for simple expressions
like `0 ge`, adding overhead and verbosity for the common case.

### 5.2 Separate Error Codes (/require vs /ensure)

Precondition failures raise `/require`.  Postcondition failures raise
`/ensure`.  These are distinct error codes.

**Why**: When a `try` catches a contract violation, the error name tells you
whether the bug is in the inputs (caller's fault, `/require`) or the
outputs (callee's fault, `/ensure`).  Conflating them loses this diagnostic
information.

### 5.3 postcondition = { check } { body } postcondition

Postcondition takes two procs: a check proc and a body proc.  It runs the
body, dups the result, runs the check, and verifies.

**Why**: The check proc pattern keeps the postcondition check separate from
the computation.  The check proc is pure (receives a copy, must not modify
the result).  The body proc is the "unit of work" whose output is being
validated.

**Alternative considered**: `body postcondition-after { check }` (post-hoc
annotation).  Rejected -- it requires remembering that the check is attached
after the body, which is error-prone.  The `{ check } { body } postcondition`
pattern reads as "check that body produces...".

### 5.4 @ensure-check is a StandardOp, Not a ControlOp

The `@ensure-check` trampoline operator is a StandardOp (transparent to
`exit`), not a ControlOp.

**Why**: Postconditions should be transparent to loop control flow.  If a
postcondition is used inside a loop and the body calls `exit`, the exit
should propagate normally.  Making `@ensure-check` a ControlOp would cause
`exit` to treat the postcondition as a loop boundary, trapping the exit.

### 5.5 No invariant Operator

The original plan included an `invariant` operator that would attach a
check proc to a container, verified on every mutation.  This was not
implemented.

**Why**: Mutation hooks on Dicts and Arrays would require changes deep in
the Dict and Array mutation paths, adding overhead to every `put`, `append`,
and indexed `put`.  The cost is paid by all users, not just those using
invariants.  The benefit is marginal -- preconditions at mutation call sites
achieve the same result with explicit, visible, zero-overhead-when-absent
checks.


## 6. Implementation Internals

### 6.1 precondition

Trivial: `verify_operands(VerifyBoolean)`, read value, decrement op pointer,
error if false.  ~8 lines of C++.  Zero allocation.

### 6.2 postcondition

Three-stage trampoline:

```
{ check } { body } postcondition

Stage 1 (postcondition_op):
  Pop check and body from op stack.
  Push onto exec stack: [check-proc, @ensure-check, body-proc]
  Body executes first (top of exec stack).

Stage 2 (@ensure-check / at_ensure_check_op):
  Body has completed; result is on op stack.
  Dup the result (clone for check proc to consume).
  Pop @ensure-check frame (1 slot: check-proc).
  Push: [postcondition-verify, check-proc]
  Check proc executes, consuming the dup'd value, pushing bool.

Stage 3 (postcondition-verify / postcondition_verify_op):
  Pop boolean from op stack.
  If false: raise Error::Ensure.
  If true: no-op.  Body's original result remains on stack.
```

### 6.3 Memory Cost

- `precondition`: Zero allocation.  Consumes one boolean from the stack.
- `postcondition`: One `make_clone` of the result (for the check proc).
  If the result is an ExtValue (Long, Double), this allocates 8 bytes
  on the ExtValue free list.  Otherwise zero allocation.

### 6.4 Execution Cost

- `precondition`: 1 operation (boolean check + conditional error).
- `postcondition`: body execution + 1 clone + check proc execution +
  1 boolean check.  Overhead beyond the body and check: ~4 operations.

### 6.5 Source File

Contract operators are in `src/ops_flow.inl` (lines ~1554-1632).
`postcondition-verify` is dispatched in `src/dispatch.inl`.


## 7. Composability

### 7.1 Contracts + Protocols

Guard protocol method implementations:

```
[/safe-sqrt] /Sqrtable def-protocol
{ dup 0 ge precondition sqrt } /safe-sqrt /real-type def-method
```

### 7.2 Contracts + Pattern Matching

Use preconditions inside match bodies:

```
value [
    { is-integer } {
        dup 0 ge precondition
        process-positive-int
    }
    { is-string } {
        dup length 0 gt precondition
        process-non-empty-string
    }
] match
```

### 7.3 Contracts + Closures

Capture domain bounds and use them in preconditions:

```
/min-val 0 def /max-val 100 def
/validate {
    dup min-val ge precondition
    dup max-val lt precondition
} [/min-val /max-val] closure-capture def

42 validate          % passes
-1 validate          % raises /require
```

### 7.4 Contracts + GenServer

Guard handler inputs:

```
<<
  /init { 0 }
  /handle-cast {
    exch dup is-integer precondition % message must be integer
    add                              % add to state
  }
>>
```

### 7.5 Contracts + Transducers

Postcondition on transduced results:

```
{ length 5 eq }
{ [1 2 3 4 5 6 7 8 9 10] { 2 mod 0 eq } xf-filter 5 xf-take xf-compose into }
postcondition
% => [2 4 6 8 10]
```


## 8. Error Handling

| Error         | Condition                                      |
| ------------- | ---------------------------------------------- |
| `/require`    | `precondition` with false boolean              |
| `/ensure`     | `postcondition` check returns false            |
| `/ensure`     | `postcondition-verify` with false boolean      |
| `/type-check` | `precondition` with non-boolean argument       |
| `/type-check` | `postcondition` check arg is not a proc        |
| `/type-check` | `postcondition` body arg is not a proc         |
| `/type-check` | `postcondition` check proc returns non-boolean |


## 9. Limitations

- **No invariant operator.** Mutation-time checks are not supported.
  Use preconditions at mutation call sites instead.

- **No custom error messages.** Contract failures produce generic
  "precondition: check failed" or "postcondition: check failed" messages.
  To provide context, wrap the contract in a `try` and re-raise with a
  descriptive message:
  ```
  { -1 dup 0 ge precondition } try
  /require eq { /require (age must be non-negative) throw-with } if
  ```

- **precondition is a keyword, not a higher-order combinator.** It does not
  compose with other operators.  It is a statement, not a value.

- **postcondition clones the result.** For large Arrays or Dicts, the
  clone for the check proc has non-trivial cost.  The check proc should
  be lightweight.

- **No compile-time contracts.** All checking is at runtime.  There is no
  static analysis pass that can prove a contract will always hold.
