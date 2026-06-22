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

# Algebraic Effects: Technical Reference

## 1. Overview

Trix exposes **algebraic effects** as a pair of user-facing operators:

- `handle-effect` -- establish a handler scope (a Dict mapping effect names
  to handler procs) around a body
- `perform` -- inside the body, dispatch a named effect; the matching
  handler is invoked with a captured continuation `K` representing the
  rest of the body

Effects let code decouple **what** it asks for (`/read-line perform`) from
**how** the request is satisfied (the handler-dict in scope decides).
Handlers can resume the captured `K` to continue the body, abort to
return a different value, or store `K` to invoke later.

The system is built on Trix's existing delimited-continuation machinery
(`delimit` / `capture`) and the per-barrier error-stack-companion pattern
shared by `try` / `finally` / `with-stream` / `delimit`.  No new runtime
machinery; one new SystemName (`@effect-barrier`) and one new error
(`/effect-not-handled`).

All operators are implemented in `src/ops_effect.inl` (~266 lines).  Zero
external dependencies.  Per-coroutine safe by construction (the handler
scope lives on the per-coroutine exec stack).

### Why algebraic effects in a stack-based VM?

Effects are the missing primitive that distinguishes "I want to read a
line" (the program's intent) from "read it from stdin / read it from a
mock array / replay it from a log" (the runtime decision).  Closures and
function pointers solve part of this, but they require threading the
configuration down every call site.  Effects let the program perform
the operation and the *enclosing scope* decides how, even across many
levels of indirection.

Concretely, effects make these patterns natural:

- **Mockable I/O**: same program, real handler in production, scripted
  handler in tests.  No D.I. framework, no monkey-patching.
- **State as an explicit, swappable resource**: pass the same algorithm
  a different state implementation (Dict-backed, memoized, traced).
- **Configurable interpreters**: an evaluator performs `/env-lookup` and
  `/apply-prim`; the handler decides the language.
- **Generators / non-deterministic choice / capability checks**: each is
  a small handler that intercepts a named effect.

Trix's design choice is that effects are **built-in operators**, not a
library on top of `delimit` / `capture`.  The deciding factors were
per-coroutine safety, integration with `try_catch_handler` for error
unwinding, and clear error messages on misuse.

## 2. The Two Operators

### `handle-effect`

```
( handler-dict body -- result )
```

`handler-dict` is a Dict whose keys are Names (effect names) and whose
values are callable procs (handlers).  `body` is callable.

`handle-effect` pushes an internal `@effect-barrier` onto the exec stack,
pushes `handler-dict` as its companion on the error stack, then executes
`body`.  If the body completes normally, the barrier is popped (along
with its companion) and the body's result is the result of
`handle-effect`.  If the body calls `perform` for an effect that this
handler-dict (or an outer one) handles, the handler is invoked instead
of the body's completion.

```trix
<< /ask { pop 42 } >>
{ /ask perform 1 add } handle-effect
% => 42
```

The handler `{ pop 42 }` receives the captured continuation `K` on the
operand stack and ignores it (`pop`), returning `42` instead.  The
`1 add` in the body never runs because the handler aborted.

### `perform`

```
( ...args effect-name -- result )
```

`effect-name` must be a Name.  Args are whatever the body wants the
handler to see -- they're left on the operand stack below `K` when the
handler runs.

`perform` scans the exec stack outward for an `@effect-barrier` whose
companion handler-dict contains `effect-name`.  If found:

1. The continuation `K` from the current position back to (and including)
   that barrier is captured into a `Continuation Object`.
2. The captured segment is removed from the exec stack.
3. `K` is pushed onto the operand stack.
4. The handler proc is executed (it sees `K` on top of stack, with any
   `...args` below).

If no matching barrier is found, `/effect-not-handled` is raised with
the effect name in the message.

```trix
<< /read-line { (hello) exch exec } >>
{ /read-line perform length } handle-effect
% => 5
```

Handler stack at entry: `[K]`.  `(hello)` pushes `(hello)`: `[K (hello)]`.
`exch` swaps: `[(hello) K]`.  `exec` resumes K, passing the body the
value `(hello)` (which is what was below K on the stack at the time of
resume).  The body's `length` runs on `(hello)`, yielding `5`.

### The resume idiom

A handler that wants to resume `K` with a value writes:

```trix
{ <value> exch exec }
```

Reading the stack effect: enter with `[K]`, push value `[K value]`, exch
to `[value K]`, exec K.  The body's continuation runs starting with
`value` on the operand stack.

A handler that wants to abort writes:

```trix
{ pop <result> }
```

Pops K, leaves `<result>`.  Body's continuation never runs.

A handler that wants to invoke K later writes:

<!-- doctest: skip (handler sketch; `<handler-side-stuff>` is a placeholder) -->
```trix
{ /saved-k exch def <handler-side-stuff> }
```

Stores K in a userdict variable.  Later code can `saved-k exec` to
resume.  Important: K is **one-shot** -- the second `exec` raises
`/invalid-access`.

## 3. Deep-Handler Semantics

The captured `K` includes the matched `@effect-barrier` and its
handler-dict companion.  When `K` is resumed, the barrier and companion
are reinstated on the exec/err stacks.

Practical effect: a `perform` inside the resumed body finds the same
handler again.

```trix
/counter 0 def
<< /tick { /counter counter 1 add def null exch exec } >>
{
    /tick perform pop
    /tick perform pop
    /tick perform pop
} handle-effect
counter % => 3
```

Each `/tick perform` re-enters the handler.  The handler bumps the
counter, then resumes.  Because the captured K reinstates the
`@effect-barrier`, the next `/tick perform` in the body finds the same
handler.

This is the **deep-handler** pattern (as opposed to **shallow-handler**,
where each effect would tear down the handler scope).  Algebraic-effects
researchers (Eff, Koka, OCaml 5) all default to deep handlers for the
same reason: it's what users almost always want, and shallow handlers
can be built from deep ones with explicit re-installation.

## 4. Nested Handlers and Delegation

Handler scopes nest.  An inner `handle-effect` whose handler-dict does
NOT contain the performed effect-name delegates outward automatically:
`perform`'s exec-stack scan walks past the inner barrier and tries the
next one out.

```trix
<< /log { (handled by outer) exch exec } >>      % outer: handles /log
{
    << /ask { (handled by inner) exch exec } >>  % inner: handles /ask only
    {
        /ask perform =                           % "handled by inner"  (inner handles, resumes)
        /log perform =                           % "handled by outer"  (inner lacks /log -> delegates outward)
    } handle-effect
} handle-effect
```

If two scopes both handle the same name, the innermost wins -- standard
lexical-scoping semantics.

## 5. Interaction with Try/Catch

`@effect-barrier` is **not** an error catcher.  When an error fires in
the body or in a handler, `try_catch_handler` unwinds normally: each
`@effect-barrier` along the way has its handler-dict companion popped
and the search continues outward to the nearest `try`.

```trix
{
    << /ask { pop 42 } >>
    { 1 0 div } handle-effect           % /div-by-zero
} try
% => /div-by-zero
```

The error propagates past the handler scope, the companion is popped
during unwinding, the `try` catches normally.

`try` inside a body works the same way:

```trix
<< /ask { pop 42 } >>
{
    { 1 0 div } try /div-by-zero eq
} handle-effect
% => true
```

The `try` catches before the error reaches `@effect-barrier`.

## 6. Interaction with `|locals|` Frame Dicts

`perform`'s captured segment can cross `|locals|` boundaries.  Frame
dicts inside the captured range are popped from the dict stack at
capture, stored inside the `ContinuationContext`, and re-pushed when `K`
is resumed.  The handler-selected body sees the same locals scope it
was captured in.

```trix
<< /ask { 1 exch exec } >>
{
    99 {
        |x|                     % x bound to 99
        /ask perform x add      % perform resumes with 1, x adds to 100
    } exec
} handle-effect
% => 100
```

The handler resumes with `1` on the stack.  The body's continuation is
`x add`, which runs in a context where `|x|` has been re-pushed onto the
dict stack with its original value `99`.

This works for arbitrary nesting depth (test §8 covers 3-deep).

## 7. Interaction with Coroutines

The exec stack is per-coroutine.  Handler scopes are too.

```trix
<< /shared-effect { pop (from-A) } >>
{
    mark { /shared-effect perform } coroutine-launch coroutine-join
    % B has no handler: /effect-not-handled is raised *inside* B, which
    % exits abnormally -- coroutine-join observes this as null + false.
} handle-effect
```

Coroutine B starts with an empty exec stack -- the handler installed in
A is invisible.  Performing inside B raises `/effect-not-handled` unless
B installs its own handler; because the error is confined to B, the
launching coroutine sees it via `coroutine-join`'s abnormal-exit signal
(`null false`) rather than as a propagated throw.

This is the central reason effects ship as built-ins rather than a
library: a userdict-backed handler stack would leak handlers across
coroutines.  Per-coroutine isolation is automatic and free.

## 8. Interaction with Save/Restore

`@effect-barrier` lives on the exec stack and its handler-dict companion
lives on the error stack.  Both are inside save scope.  A `restore` that
crosses a `handle-effect` boundary tears down the barrier and companion
naturally.

A continuation `K` captured by `perform` remembers the save level at
capture time.  If a captured `K` is still live -- on the operand stack --
when a `restore` crosses its capture point, `restore` raises
`/invalid-restore` rather than leaving a dangling continuation.

```trix
save /S exch def
<< /ask { } >>                  % no-op handler: leaves K on the operand stack
{ /ask perform 1 add } handle-effect
% K is now on the operand stack, captured above the save barrier
S restore                       % => /invalid-restore (stale K detected on opstack)
```

This is the same staleness enforcement as ordinary continuations --
effects don't introduce a new staleness model.

## 9. Error: `/effect-not-handled`

Raised by `perform` when no matching handler is found in any enclosing
scope.  The error message includes the effect name.

```
{ /no-such-effect perform } try
% => /effect-not-handled
% message: "perform: no handler for effect /no-such-effect"
```

This is the only error specific to algebraic effects.  Other errors
(`/type-check`, `/invalid-access`, `/opstack-underflow`,
`/execstack-overflow`, `/errorstack-overflow`) come from the operand /
type / stack-capacity machinery shared with the rest of Trix.

## 10. Implementation Notes (for VM maintainers)

### Operator pair structure

| Operator          | Stack effect                  | Where                                        |
| ----------------- | ----------------------------- | -------------------------------------------- |
| `handle-effect`   | `handler-dict body -- result` | `ops_effect.inl:73`                          |
| `perform`         | `...args /name -- result`     | `ops_effect.inl:97`                          |
| `@effect-barrier` | (internal)                    | `ops_effect.inl:65`, enum at `enums.inl:263` |

The barrier follows the standard "barrier on exec stack + companion on
error stack" pattern shared with `try-catch-barrier`,
`finally-barrier`, `with-stream`, `delimit-barrier`.

### Companion bookkeeping

`@effect-barrier` pushes exactly **one** error-stack companion (the
handler-dict).  This count must be reflected in:

1. `perform`'s find-handler scan loop (`ops_effect.inl` "Walk 1", from line 108): each
   barrier crossing decrements `err_scan` by 1; `@handler-scope`
   decrements by 2.
2. `perform`'s companion-count scan loop
   (`ops_effect.inl` "Walk 2", from line 149): increments `err_count` by 1 per
   `@effect-barrier` in the captured range.
3. the `@effect-barrier` descriptor's `UnwindAction::PopAndContinue`
   with `err_companions == 1` (`op_descriptor.inl`; applied by
   `try_catch_handler` at `ops_system.inl:1800`): pops one entry per
   `@effect-barrier` crossed.

**Maintenance contract**: any new barrier type that adds error-stack
companions must be enumerated in all three lists, with the correct
companion count.

### Op-stack capture omission

`perform` deliberately captures with `op_count = 0`.  Cited at
`ops_effect.inl:182` (`ctx->m_op_count = 0`).  Rationale: avoiding per-captured-barrier
op-depth rewrites on top of the already-needed exec-depth rewrites.

Consequence: a handler that resumes `K` after pushing values onto the
op stack will pass those values into the body's continuation.  This is
the standard "resume-with-value" idiom and is intentional.

### Locals capture / restore

`perform` counts `@end-locals` markers in the captured range and pops
that many frame dicts from the dict stack, storing them inside the
`ContinuationContext`.  `execute_continuation` re-pushes them when `K`
is invoked.  See `ops_effect.inl:198-207` (`@end-locals` counting / `clear_name_bindings`).

### Op-depth / exec-depth rewrite tables

For inner barriers in the captured range that carry saved depths --
`@delimit-barrier`, `@handler-scope`, `@try-rollback`,
`@try-result-complete`, the six `@*-for-all` operators, and the
logic-programming barrier family -- `perform` emits rewrite-offset
tables so the saved depths can be relocated when `K` is resumed.  See
`ops_effect.inl` "Walk 3" (from line 214).

When a new barrier type with saved depths is added, BOTH the count loop
("Walk 2") AND the rewrite-table emission loop ("Walk 3") need to
recognize it.

### Continuation Object lifetime

`K` is a Continuation Object whose context lives in VM heap.  The
context is **NOT** journaled by save/restore -- restoring across the
capture point reclaims the context's memory, and the Object's stamped
save level then exceeds `m_curr_save_level`, triggering
`/invalid-restore` at the next `exec K`.

One-shot enforcement is via the `m_err_count = 0xFFFF` "spent
sentinel" in `ContinuationContext`.  Second `exec` raises
`/invalid-access`.

## 11. Idioms

### Mockable I/O

```trix
% Production handler: real stdin/stdout
/io-real <<
    /read-line { stdin 256 string read-line pop exch exec }
    /write-line { /k exch def print nl /k load exec }
    /exit { /code exch def code exit }
>> def

% Test handler: scripted input, captured output, fake exit
/io-mock {
    /test-input  exch def       % array of input lines
    /test-output [] def
    /test-exit-code -1 def
    <<
        /read-line {
            test-input length 0 eq
                { (EOF) exch exec }
                { test-input 0 get
                  test-input 1 test-input length 1 sub get-interval
                  /test-input exch def
                  exch exec }
            if-else
        }
        /write-line {
            /k exch def
            /test-output test-output exch append def
            /k load exec
        }
        /exit {
            pop /test-exit-code exch def
        }
    >>
} def

% Same program runs against either handler.
```

### State as effect

```trix
/state 10 def                                % initialize the backing cell
<< /get { /k exch def /state load /k load exec }
   /set { /k exch def /state exch def null /k load exec }
>>
{
    /get perform 1 add /set perform pop % increment
    /get perform                        % read => 11
} handle-effect
```

### Generators via effects (lightweight coroutines)

```trix
<< /yield { } >>                    % handler stores K in saved-k
{ 1 /yield perform 2 /yield perform 3 /yield perform } handle-effect
% Each /yield: stores K, returns the yielded value as the perform result.
% Caller picks up the K and exec's it to get the next yield.
```

(Note: this competes with Trix's native coroutines for generator-style
code.  Coroutines are usually the better choice for that pattern; effects
shine when the *semantics* of the operation need to be swappable, not
just when sequential values need yielding.)

## 12. When NOT to use effects

- **Plain configuration**: pass the value directly.  Don't build a
  handler scope to pass a string.
- **Synchronous data flow**: return values or use closures.  Effects
  add ceremony without payoff for direct data passing.
- **Cross-coroutine notification**: use the actor / mailbox layer.
  Effects are per-coroutine by design.
- **Persistent subscriptions**: reactive cells are the dedicated
  primitive.

Use effects when the operation has a clear name, the implementation
choice is genuinely orthogonal to the call site, and the handler scope
boundary is meaningful (e.g. a test harness, a transaction, a
sandboxed eval).

## 13. See Also

- `docs/continuation.md` -- `delimit` / `capture` continuations, the
  foundation `perform` builds on
- `docs/trix-reference.md` -- operator reference card
- `docs/errors-cheatsheet.md` -- `/effect-not-handled` entry
- `examples/effects_mini_scheme.trx` -- env-lookup / apply-prim / stuck
  as effects; three handlers (real-eval / tracing+real / synthetic)
  evaluate the same AST under three different semantics
- `tests/test_effects.trx` -- 58-assertion test suite (canonical
  behavioural reference)
