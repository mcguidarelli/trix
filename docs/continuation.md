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

# Trix Delimited Continuations

Technical reference for the Continuation type.

---

## Part 1: For the End Programmer

### What is a continuation?

A continuation is a saved computation you can resume later.  When you
call `capture` inside a `delimit` body, Trix snapshots everything the
body still had left to do, packages it as a Continuation object (K),
and hands K to your handler proc.  Your handler can then:

- **Invoke K via `exec`** to resume the body where it left off with a
  value; the handler's pending frames keep running after the body
  completes (splice).
- **Invoke K via `abort-exec`** to resume the body with a value AFTER
  first discarding the handler's pending frames (Scheme-call/cc-style
  abort).  The handler, and any frames the resumer pushed between the
  capture point and the abort-exec call, are trimmed.
- **Discard K** to abort the body and return a different result
- **Store K** in a variable, array, or curry for later invocation

### delimit / capture by example

**Basic: handler ignores K (abort pattern)**

```trix
{ { pop 42 } capture 10 add } delimit   % result: 42
```

The body starts running `{ pop 42 } capture 10 add`.  When `capture`
fires, it sees `10 add` still pending.  It packages that as K, pushes
K onto the stack, and runs the handler `{ pop 42 }`.  The handler
drops K and returns 42.  Since the handler didn't resume K, the pending
`10 add` never executes.  42 becomes the result of `delimit`.

**Resume: handler invokes K**

```trix
{ { 99 exch exec } capture 1 add } delimit   % result: 100
```

The handler pushes 99, swaps it below K, and executes K.  K resumes
the body with 99 as the "return value of capture".  The body then
adds 1, producing 100.

**Handler code after resume (splice)**

```trix
{ { 42 exch exec 100 add } capture 10 add } delimit   % result: 152
```

The handler resumes K with 42.  The body runs `10 add` producing 52.
52 flows back to the handler as the return value of `exec`.  The
handler then adds 100, producing 152.

**Abort on resume (`abort-exec`)**

```trix
10 { { 99 exch abort-exec } capture 5 add } delimit add   % result: 114
```

The handler invokes `abort-exec` instead of `exec`.  `abort-exec`
scans the exec stack for the `@handler-scope` barrier belonging to K
(installed by `capture` when the handler was set up), trims everything
above it (discarding handler frames and running any cleanup), then
splices K's captured segment on top with `99` as the resume value.
The captured body `5 add` runs: `99 + 5 = 104`.  Outer `add`:
`10 + 104 = 114`.

Under plain `exec` in the same shape, the handler's pending frames
would stay in place and the captured body's result would unwind
through them -- fine for some patterns, wrong for the classic Scheme
`(+ 10 (call/cc (lambda (k) (k 42))))` where `(k v)` is meant to
exit the enclosing call/cc receiver.

**Choosing `exec` vs `abort-exec`**

| Pattern                                                              | Right choice    |
| -------------------------------------------------------------------- | --------------- |
| shift/reset, generators, coroutine-as-iterator                       | `exec` (splice) |
| Scheme-style `(k v)` early-exit from a receiver                      | `abort-exec`    |
| Any case where the handler wants to run code AFTER the captured body | `exec`          |
| Any case where calling K is conceptually the receiver's final act    | `abort-exec`    |

`abort-exec` requires K's `@handler-scope` to still be on the exec
stack.  If the handler returned normally and you later invoke K on a
stored reference, `abort-exec` raises `/invalid-access` with "handler
scope no longer active".  `exec` works regardless (splice doesn't
need the scope).

### Passing values through capture/resume

The resume value is whatever is on top of the operand stack when K is
executed.  Inside the body, that value appears as if `capture` had
returned it.

```trix
{ { (hello) exch exec } capture length } delimit   % result: 5
```

The handler resumes K with the string `(hello)`.  The body sees
`(hello)` as the result of capture, computes its length: 5.

### One-shot semantics

A continuation can only be invoked once.  After the first invocation,
the context is marked spent.  A second invocation raises
`/invalid-access`.  This is true even if you `dup` the continuation --
both copies share the same underlying context.

```trix
{ { dup 1 exch exec pop 2 exch exec } capture } delimit
% second exec raises /invalid-access
```

### Common patterns

**Early return from a computation:**

The captured K is the tail after `capture` (here, the empty path back to
`delimit`).  Stash it as a "return" continuation, then `abort-exec` it to
exit early -- `abort-exec` trims the handler frames that follow, so the
code after the early return never runs.

```trix
{
    {
        % handler: K is the "return" continuation
        /return exch def
        10 20 add                    % = 30
        30 /return load abort-exec   % early return with 30 -- trims the rest
        999                          % never runs
    }
    capture
} delimit
% result: 30
```

**Yielding values (generator-like):**

```trix
{ { } capture } delimit   % K1 returned
/k1 exch def

{ { } capture } delimit   % K2 returned
/k2 exch def

1 /k1 load exec           % resume K1 with 1
2 /k2 load exec           % resume K2 with 2
```

### Relationship to other Trix callables

| Type                | What it captures       | When it runs                                           |
| ------------------- | ---------------------- | ------------------------------------------------------ |
| Proc (Array/Packed) | Static code            | Every invocation                                       |
| Curry               | Value + callable       | Every invocation (unpacks value, dispatches callable)  |
| Thunk               | Proc + cached result   | First force evaluates, subsequent forces return cached |
| **Continuation**    | **Exec stack segment** | **Once (one-shot), resumes captured computation**      |

Continuations are the only callable that captures *dynamic* execution
state rather than static code.

Use `is-continuation` (`any -- bool`) to test whether a value is a
captured continuation:

```trix
{ { is-continuation } capture } delimit % => true (K is a continuation)
42 is-continuation                      % => false
```

### Algebraic effects

Trix provides built-in `handle-effect` and `perform` operators that
use the continuation mechanism internally.  The sketch below shows
how a simplified version *could* be built from raw delimit/capture
(demonstrating the power of the primitives), but the built-in
operators add proper nested handler delegation, per-coroutine safety,
error unwinding integration, and deep handler semantics.

```trix
% Simplified library sketch (for illustration -- use the built-in operators):
/handle-effect-simple {
    % handler-dict body -- result
    exch /handlers exch def
    { exec } delimit
} def

/perform-simple {
    % ...args effect-name -- (yields to handler)
    {
        exch /k exch def         % save K
        handlers exch get        % look up effect handler
        /k load exch curry exec  % call handler(K, ...args)
    } capture
} def
```

**Caveat (non-runnable pseudocode):** the `/perform-simple` sketch above
will not run as written.  Its `exch` step elides the fact that `capture`'s
handler proc runs on a *clean* op stack with only K on top (see the
"Operand stack capture" gotcha below) -- the ambient `...args effect-name`
slice is trimmed into the captured context before the handler runs, so
`exch` underflows.  A real version must thread the args another way (e.g.
stash them in a name before `perform`); this is one of the things the
built-in operators handle for you.

See `docs/trix-reference.md` section 3.41 for the built-in operators.

### Gotchas

- **Locals across capture:** `capture` may cross any number of `|locals|`
  boundaries.  Each `@end-locals` in the captured segment pairs with a
  dict-stack entry; `capture` pops those dicts, stores them in the
  continuation, and `execute_continuation` pushes them back (with name
  bindings reinstated) before restoring the exec segment.

- **Operand stack capture:** `capture` also saves the ambient op-slice
  between the matched `@delimit-barrier` and the capture point
  (everything pushed by the delimit body that the handler didn't pop
  off).  The handler runs on a clean op stack with K on top; on resume,
  the slice is spliced below the resume value so the captured body
  sees its original op layout.  Does NOT apply to `perform` -- effect
  handler deep-restore would need per-barrier op-depth rewrite,
  tracked separately.

- **Two resume operators, explicit choice.**  `exec K` splices the
  captured segment above the resumer's pending frames; `abort-exec K`
  trims the resumer's frames down to the handler-push point first
  (running cleanup for any barriers, streams, or frame dicts in the
  range) and then splices.  See "Choosing `exec` vs `abort-exec`"
  above.  Implicit invoke forms (executable name dispatch) always
  use splice; `abort-exec` must be called explicitly.

- **Executable names:** When a continuation is stored via `/k exch def`,
  bare `k` and `\\k` both invoke it immediately (executable name
  dispatch).  To push K as a value without invoking, use `/k load`.

- **Empty captures:** If `capture` is the last operation in the body,
  K captures zero pending operations.  Resuming K is essentially a
  no-op -- the resume value passes through.

---

## Part 2: For the Trix Maintainer

### ContinuationContext struct

Defined in `src/ops_continuation.inl`.  Variable-length, allocated on
the VM heap.

```cpp
struct ContinuationContext {
    static constexpr length_t Spent{std::numeric_limits<length_t>::max()};

    length_t m_exec_count;        // captured exec stack Objects
    length_t m_err_count;         // captured error stack entries (Spent sentinel = spent)
    length_t m_dict_count;        // captured frame dict Objects (one per @end-locals)
    length_t m_op_count;          // captured op-slice (ambient between delimit and capture)
    length_t m_op_rewrite_count;    // captured err-segment offsets pointing at op-depth slots
                                    // (@delimit-barrier, @handler-scope) that get rewritten to
                                    // the resumer's fresh op-floor at restore time
    length_t m_exec_rewrite_count;  // captured EXEC-segment offsets pointing at op-depth slots
                                    // (@try-rollback's saved-depth) that get rewritten at restore
    bool m_alloc_global_at_capture; // m_curr_alloc_global at capture time; restored on resume so a
                                    // body captured inside ${...} resumes with the right alloc routing
    Object m_objects[1];            // [exec..., err..., locals-dicts (outermost first),
                                    //  op-slice, err-rewrite-offsets, exec-rewrite-offsets]

    bool is_spent() const;
    void mark_spent();              // sets counts=0, m_err_count=Spent
    static vm_size_t alloc_size(length_t exec_count, length_t err_count,
                                 length_t dict_count, length_t op_count,
                                 length_t op_rewrite_count,
                                 length_t exec_rewrite_count);
};
```

Header is 16 bytes (six `length_t` = `uint16_t`, plus one `bool` padded
to `alignof(Object)` = 4).  Objects are stored
bottom-first (lowest stack address first for each segment) so
`execute_continuation` can restore each with a single `std::memcpy`.

**Op-depth rewrite tables.**  Several barriers record op-stack depths
as absolute `m_op_ptr - m_op_base` values at push time.  When such a
barrier is captured inside a K's segment (typical with `perform`'s
deep-handler semantics: an inner `delimit { ... perform ... }` or
`try { ... perform ... }` rides along) and K is later restored at a
different op-stack position, the captured op-depth is stale -- it
points at the capturer's frame.  A subsequent `capture` inside the
resumed body, or the @try-rollback error path, would consult that
stale depth and trim the live op stack to the wrong floor (silent
operand corruption: items below the resumer's K-invocation frame
disappear).

Two rewrite tables fix this:

| Table | Storage | Targets |
| --- | --- | --- |
| `m_op_rewrite_count` + err-rewrite array | err segment | `@delimit-barrier` (1 slot: op-depth); `@handler-scope` (slot 2 of 2: op-depth; identity slot stays) |
| `m_exec_rewrite_count` + exec-rewrite array | EXEC segment | `@try-rollback`, `@try-result-complete`, `@tag-update-complete` (saved-depth at scan-1; try/try-result/tag-update families); `@array/@packed/@string/@dict/@set/@record-for-all` (saved-depth at scan-1; for-all family); `@naf-barrier`, `@choice-count-barrier`, `@for-each-solution-barrier`, `@aggregate-barrier`, `@aggregate-reduce-barrier` (op_depth at scan-2; logic-programming family).  The exec-rewrite set is exactly the descriptor rows with `needs_exec_rewrite=true` in `op_descriptor.inl` -- not a hand-enumerated subset (`operator_is_forall_family()` exists but is unused dead code, NOT the mechanism). |

At capture time, capture_op / perform_op walk the captured exec range
bottom-up and record one offset per op-depth-bearing target -- the
position within the appropriate captured segment of the slot to
overwrite.  At restore time, `execute_continuation` computes the
resumer's segment-start floor (`fresh_op_depth`) and overwrites every
recorded slot with that single value, in-place in the ctx buffer,
before the corresponding `memcpy` to the live stack.  Per-barrier
semantics don't matter at restore: every captured inner barrier's
"scope floor" collapses to the segment's start in the resumer's
frame -- one common value.

Barriers that do NOT get rewritten (their err companion isn't an
op-depth): `@try-catch-barrier` (handler-dict), `@finally-barrier`
(finally-proc), `@with-stream` (stream), `@effect-barrier`
(handler-dict), `@end-in-global` (bool flag).

The in-buffer mutation is safe because K is one-shot (already
`mark_spent` by the time the rewrite fires).  If Trix ever goes
multi-shot, move the rewrites to operate on the live stacks
post-`memcpy`.

**Spent sentinel:** `m_err_count == Spent` (0xFFFF).  Not `m_exec_count`
because zero-element captures are valid (capture as last body operation).

**Restore-dangle detection:** the save level at capture time is stashed
in the Continuation *Object* (`Object::m_continuation_save_level`, 1 byte
in the header union), NOT in the context.  That way
`execute_continuation` can reject a post-restore resume with
`/invalid-access` without first dereferencing the possibly-freed ctx.
The `restore` operator also refuses to complete when a Continuation
above the target save level is reachable from the op/exec/dict/err
stacks, so reaching the save-level check normally requires an escape
path (e.g. a Continuation carried by a save-excluded structure).

### Exec stack capture mechanics

When `capture_op` fires:

1. Scan exec stack top-down for `@delimit-barrier` (identity check via
   `operator_is_delimitbarrier()`).
2. Count Objects between `m_exec_ptr` and barrier (exclusive).
3. Scan captured range: count inner barriers (try-catch, finally,
   with-stream, nested delimit, effect, handler-scope) -- each owns
   err companions on the error stack -- and `@end-locals` markers --
   each pairs with one top-of-dict-stack frame dict.  Handler-scope
   counts as 2 err entries; others count as 1.
4. Read the matched barrier's op-depth companion from the error
   stack (at position `m_err_ptr - err_count`, just below the inner
   companions that will be captured).  Compute
   `op_count = max(0, m_op_ptr - (m_op_base + op_depth))`.  The
   clamp covers the case where the delimit body consumed op items
   from below the snapshot (e.g. `|locals|#N` binding a pre-delimit
   item); in that case the slice is empty and `m_op_ptr` is left
   where it is (moving it up would expose stale bytes).
5. Allocate `ContinuationContext` on VM heap (sized for exec + err +
   dict + op-slice Objects).
6. `memcpy` exec segment (barrier+1 to m_exec_ptr, bottom-first).
7. `memcpy` inner error entries (top `err_count` entries, bottom-first).
8. Pop `err_count` inner entries + the matched barrier's own op-depth
   companion from error stack (total `err_count + 1` entries).
9. `memcpy` top `dict_count` dicts from dict stack (outermost first).
   For each popped dict, call `clear_name_bindings`.
10. Pop `dict_count` entries from dict stack.
11. If `op_count > 0`: `memcpy` the slice into the ctx's trailing
    region and trim `m_op_ptr` down to the snapshot.  If `op_count
    == 0`: skip both.
12. Cut exec stack back to below barrier.
13. Install `@handler-scope` with two err companions (identity =
    Continuation `ctx_offset`, op-depth = current `m_op_ptr`
    position just before K push).  The identity lets `abort-exec`
    match its scope to its Continuation; the op-depth lets
    `abort-exec` restore the pre-handler op state.
14. Push Continuation Object (executable) to operand stack.  The
    Object records `m_curr_save_level` in its
    `m_continuation_save_level` slot.
15. Set handler executable, push to exec stack.  Handler runs with a
    clean op stack containing only K, above the newly-installed
    `@handler-scope` barrier.

### execute_continuation (interpreter.inl)

When the interpreter encounters an executable Continuation:

1. Check `m_curr_save_level >= continuation.continuation_save_level()`
   -- error `/invalid-access "continuation invalidated by restore"` if
   the current save level has dropped below capture level.
2. Dereference ctx.  Check `is_spent()` -- error if true.
3. Read `exec_count`, `err_count`, `dict_count`, `op_count`.
4. Verify exec, error, dict, op stack capacities.
5. `mark_spent()`.
6. `memcpy` frame dicts from context to dict stack; for each, call
   `set_name_bindings` so name references inside the restored exec
   segment resolve into the restored dicts.
7. Pop resume value from operand stack; record the resulting
   `m_op_ptr - m_op_base` as the fresh barrier's `op-depth`
   companion ("scope floor" BELOW the restored slice).
8. `memcpy` op-slice onto the operand stack; push resume value back
   on top.
9. Push integer op-depth companion on error stack, then push fresh
   `@delimit-barrier`.
10. `memcpy` exec Objects from context to exec stack.
11. `memcpy` error Objects from context to error stack (if
    `err_count > 0`).
12. Restore `trx->m_curr_alloc_global` from
    `ctx->m_alloc_global_at_capture` so a body captured inside `${...}`
    resumes with its capture-time allocation routing.

Control returns to the dispatch loop; the restored exec segment
runs first (top of exec), operating on the spliced op layout
`[resumer..., slice..., resume-value]`.  After the captured segment
completes, the fresh `@delimit-barrier` pops its op-depth companion
and control flows back to the resumer's pending frames.  For Scheme-
style `(k v)` abort patterns where the resumer's pending frames
should be discarded, callers use `abort-exec` instead (see
`abort_exec_op` below).

### abort_exec_op

When the user calls `abort-exec` (found in `ops_continuation.inl`):

1. Verify top is Continuation, second is any (resume value).  Peek
   K without popping.
2. Save-level check against `K.continuation_save_level()` -- reject
   with `/invalid-access "continuation invalidated by restore"` if
   the save level dropped below capture.
3. Scan exec top-down for `@handler-scope` whose identity companion
   (`err_scan[-1]`) equals K's `ctx_offset`.  Track `err_scan` past
   each barrier's companions (1 per single-companion barrier, 2 per
   `@handler-scope`).  If none matches, raise `/invalid-access
   "handler scope no longer active; use exec for post-scope
   invocation"`.
4. Read the matched scope's op-depth companion
   (`err_scan[0]`); save as `matched_op_depth`.
5. Pop K and resume value from op.
6. Call `unwind_exec_to(handler_scope)` -- shared helper with
   `exit_op` and `stop_op` -- which walks the discarded exec range
   and runs cleanup per operator: pop err companions for barriers,
   close streams for `@with-stream` / `@run`, pop dicts for
   `@end-locals` / other dict-pops, free ExtValues on non-operator
   slots.
7. Pop `@handler-scope`'s two err companions (identity + op-depth)
   and drop the barrier itself from exec.
8. Trim `m_op_ptr` to `m_op_base + matched_op_depth`, freeing
   ExtValues in the discarded range.  Push resume value back on top.
9. Call `execute_continuation(K)` -- reusing the splice restore
   path.  The pre-trim state is the only difference between
   `exec K` and `abort-exec K`; restoration is identical.

The same spent-check and save-level check in `execute_continuation`
apply -- one-shot discipline is enforced in a single place.

### @handler-scope barrier

Installed by `capture_op` after the matched `@delimit-barrier` is
consumed and before the handler frame is pushed.  Two err companions
(deeper-first): Continuation identity, then op-depth at handler-push.

- **Normal completion:** `at_handler_scope_op` pops both companions.
  Fired when the handler body runs off the end without calling
  `abort-exec`.
- **During abort-exec:** the scan matches identity, the trim
  discards the barrier + companions along with the handler frames.
- **Error unwinding:** `try_catch_handler` treats `@handler-scope`
  as a non-catching barrier (like `@delimit-barrier` and
  `@effect-barrier`) -- pops the 2 companions and keeps searching
  for a try/try-catch.
- **exit_op / stop_op via `unwind_exec_to`:** pops 2 companions.
- **Backtrace:** `format_backtrace` prints `[handler-scope boundary]`.
- **capture / perform scans in the captured range:** contribute 2
  to `err_count` (rather than 1) so the trailing memcpy and pop
  counts stay correct when a captured body contains a
  `@handler-scope`.

Identity is the Continuation Object's own `vm_offset_t`.  Unique
for the Object's lifetime; no separate ID counter needed.  Nested
captures resolve correctly: inner handler's `abort-exec OUTER-K`
skips `@handler-scope(inner)` during the identity scan and trims
through it to reach `@handler-scope(outer)`.

### unwind_exec_to helper

Defined in `ops_flow.inl`, shared by `exit_op`, `stop_op`, and
`abort_exec_op`.  Walks `m_exec_ptr` down to (but not including)
the target position and runs per-operator cleanup:

- `@try-catch-barrier` / `@finally-barrier` / `@delimit-barrier` /
  `@effect-barrier`: pop 1 err companion.
- `@handler-scope`: pop 2 err companions (identity + op-depth).
- `@with-stream`: pop err companion, close stream if still open.
- `@end-in-global`: pop err companion (saved alloc flag) and restore
  `m_curr_alloc_global` from it (mirror `at_end_in_global_op`).
- `@run`: close the stream at `scan + 1` if its sid still matches.
- `@end-locals` / other dict-pop: pop dict-stack entry, clear cached
  name bindings, recycle if locals.
- Non-operator slots: free any ExtValue/WideValue.

On exit `m_exec_ptr = target`.  The slot AT target is left for the
caller to handle (drop, replace, popcount-adjust, or splice above).

### @delimit-barrier behavior

- **Normal completion:** `at_delimit_barrier_op` is a no-op.  The body
  finished without calling `capture`.  Result is on the operand stack.
- **During capture:** Barrier is found by `capture_op`'s exec stack
  scan.  Removed when exec stack is cut back.
- **Error unwinding:** `try_catch_handler` recognizes `@delimit-barrier`
  via `operator_is_barrier()` flag, but the delimit-specific check
  skips it (`operator_is_delimitbarrier()` -> continue searching).
  No error stack entry to pop.
- **Backtrace:** `format_backtrace` prints `[delimit boundary]`.
- **exit_op / stop_op:** No cleanup needed -- barrier has no error
  stack companion.  Discarded naturally when exec stack is cut back.

### Interaction with save/restore

`ContinuationContext` and the captured frame dicts are VM-heap
allocated at the current save level.  A `restore` below the capture
level reclaims them.  Two lines of defense:

1. **`restore` operator** refuses to proceed when any Continuation
   above the target save level is reachable via the op/exec/dict/err
   stacks.  This prevents the common case (user holds K in a name or
   stack slot then tries to restore past it) from producing a
   dangling continuation.

2. **Save-level check in `execute_continuation`** fires
   `/invalid-access "continuation invalidated by restore"` if the
   current save level has dropped below the level captured in the
   Continuation Object.  This covers escape paths where restore's
   reachability scan didn't see the Continuation (e.g. stored in a
   save-excluded structure such as an eqdict or eqarray entry).

The Object-level save level stash (not inside the ctx) is what makes
(2) safe: even if the ctx memory was reclaimed and possibly reused,
the Continuation Object on the stack still knows the original capture
level.

### Interaction with coroutines

Works naturally.  The exec stack is per-coroutine.  `ContinuationContext`
is on the shared VM heap.  A continuation captured in one coroutine could
be resumed in another (the captured segment is just Objects -- no
pointer-to-stack references).  This is safe for single-threaded execution.

### ExtValue lifecycle during capture

Objects on the exec stack are **moved** into the `ContinuationContext`,
not copied.  The exec stack pointer is adjusted down without calling
`maybe_free_extvalue_execstack`.  The context now owns any ExtValue
references in the captured Objects.  On resume, the Objects are
memcpy'd back to the exec stack -- no clone/free cycle.  If K is
never resumed, the ExtValues in the context are leaked (same as any
abandoned heap allocation -- reclaimed on `restore` or process exit).

### Locals-dict capture (across `|locals|` boundaries)

Each `@end-locals` in the captured segment pairs with one top-of-dict-
stack frame dict.  `capture` / `perform` pop those dicts (running
`clear_name_bindings` on each, so the running coroutine's binding
cache drops stale pointers into them) and store them in the trailing
`m_objects[]` region of `ContinuationContext` -- outermost-first, so
the memcpy on resume produces the right dict-stack order.

On resume, `execute_continuation` pushes the dicts back and calls
`set_name_bindings` on each before restoring the exec segment, so any
name references inside the restored `@end-locals`-bounded frames
resolve correctly and the restored `@end-locals` markers, when they
fire, pop the right dicts.

**Operand stack is captured for `capture`, not for `perform`.** The
`capture` path saves the ambient op-slice between the matched
`@delimit-barrier` and the capture point into the context and
restores it on resume so the captured body sees its original op
layout.  `perform` does NOT capture an op-slice today (deep-handler
semantics would need per-barrier op-depth rewriting on restore);
`perform_op` passes `op_count=0` to `ContinuationContext::alloc_size`.
Tracked as a separate language follow-up.

### Algebraic effects and deep handlers (ops_effect.inl)

The built-in `handle-effect`/`perform` operators use the continuation
mechanism internally.  `perform` captures a continuation just like
`capture`, but with two key differences:

1. **Scan target:** `perform` scans for `@effect-barrier` (not
   `@delimit-barrier`), checking each barrier's handler-dict companion
   for the requested effect name.  If not found, continues scanning
   (automatic nested handler delegation).

2. **Inclusive capture (deep handlers):** `perform` captures the
   `@effect-barrier` and its handler-dict companion as part of the
   continuation segment.  When K is resumed via `execute_continuation`,
   the barrier and companion are restored alongside the exec/error
   entries, reinstating the handler scope.  This means subsequent
   performs in the resumed body find the same handler automatically --
   the standard "deep handler" pattern from Koka, Eff, and OCaml 5.

   By contrast, `capture` uses exclusive capture -- the `@delimit-barrier`
   is excluded from the captured segment, and `execute_continuation`
   pushes a fresh `@delimit-barrier` instead.

The `@effect-barrier` has a handler-dict companion on the error stack
(pushed by `handle_effect_op`, popped by `at_effect_barrier_op` on
normal completion).  During error unwinding, `try_catch_handler` pops
the companion and continues searching -- the effect barrier does not
catch errors.

### File map

| File | Role |
| --- | --- |
| `src/object.inl` | Type::Continuation, factory, accessors, sm_object_attrib, hash/eq |
| `src/ops_continuation.inl` | ContinuationContext struct, delimit_op, capture_op, at_delimit_barrier_op |
| `src/ops_effect.inl` | handle_effect_op, perform_op (deep handler capture), at_effect_barrier_op |
| `src/interpreter.inl` | execute_continuation, dispatch case in interpreter loop |
| `src/verify.inl` | VerifyContinuation in VerifyCallable, VerifyKey, VerifyAny |
| `src/enums.inl` | SystemName::Delimit, Capture, AbortExec, IsContinuation, atDelimitBarrier, atHandlerScope, HandleEffect, Perform, atEffectBarrier |
| `src/types.inl` | Error::UnhandledCapture, Error::EffectNotHandled |
| `src/dispatch.inl` | Operator dispatch entries |
| `src/printfmt.inl` | print_continuation |
| `src/ops_system.inl` | try_catch_handler pass-through (delimit + effect + handler-scope barriers), format_backtrace |
| `src/ops_flow.inl` | unwind_exec_to helper (shared by exit_op / stop_op / abort_exec_op) |
| `tests/test_continuation.trx` | 114-assertion test suite (21 sections including abort-exec coverage + fuzz regression) |
| `tests/test_effects.trx` | 58-assertion test suite |
| `tests/fuzz_regressions/crash-*` | Replay-as-regression for fuzz-found bugs |
