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

# Control Operators and the Coroutine Trampoline

This is the mechanism behind every operator that can **block, yield, or
resume** -- `coroutine-sleep`, an actor receive, a pipeline read, a `try` /
`with-stream` cleanup, a logic `choice` backtrack.  It is referenced from many
docs and assumed by the concurrency runtime; this is the single explainer.  If
you are adding any operator that cannot finish in one pass, read this first.

**Jump to:** [Why they exist](#1-why-they-exist) |
[What a control operator is](#2-what-a-control-operator-is) |
[The trampoline](#3-the-trampoline-pattern) | [Re-entrant state](#4-re-entrant-state) |
[Gotchas and invariants](#5-gotchas-and-invariants) |
[Adding a blocking-aware op](#6-adding-a-blocking-aware-operator)

---

## 1. Why they exist

The interpreter loop ([§2 of the Architecture Overview](dev-architecture.md#2-the-execution-pipeline))
is plain C++: it pops one Object off the execution stack, dispatches it, and
repeats.  It runs on a single OS thread, and every coroutine, actor, and
pipeline stage shares that one thread cooperatively.

So a C++ operator function **must not block the thread**.  If
`coroutine-sleep` called `sleep()`, or a pipeline read spun waiting for data,
the whole VM -- every other coroutine included -- would freeze.  An operator
that cannot complete in a single call therefore **splits in two**: a *start*
stage that does what it can and then yields, and a *resume* stage that runs
later when the blocking condition clears.  Both stages are driven through the
execution stack, because in Trix the exec stack *is* the program counter --
there is nowhere else to "leave off and come back."

A **control operator** is the resume stage made first-class: an internal
Operator the engine schedules onto the exec stack to re-enter itself.

---

## 2. What a control operator is

A control operator is an `@`-prefixed system Operator that exists only inside
the engine.  The full set occupies a contiguous block at the front of the
`SystemName` enum (`src/enums.inl`):

```text
  SystemName enum layout
  +-----------------------------------------------------------+
  | FIRST_CONTROL_OP = atRun ("@run")                         |
  |   atStop, atTryBarrier, atCatchError, atFinallyReraise,   |  158 control ops
  |   atWithStream, atLoop, atCall, atCoroutineJoinCheck,     |  (atRun .. atEndInGlobal;
  |   atPipeGetRetry, atActorRecvCheck, atChoiceBarrier, ...  |   +atDebugErrorResume
  | LAST_CONTROL_OP = atEndInGlobal ("@end-in-global")        |   under TRIX_DEBUGGER)
  +-----------------------------------------------------------+
  | FIRST_STD_OP = ArrayFromMark ("array-from-mark")          |
  |   ... all the user-callable operators ...                 |  the standard operators
  | LAST_STD_OP = CurrentGlobal                               |
  +-----------------------------------------------------------+
```

Three properties follow from that layout:

1. **Created only in C++.** A control op is built by
   `Object::make_control_operator(SystemName)` (`src/object.inl`), which asserts
   the name is in `[FIRST_CONTROL_OP, LAST_CONTROL_OP]` and tags the Operator
   with a flag class (`RunOp` / `StopOp` / `ControlOp`) and a *popcount* -- how
   many execution-stack entries make up its frame (e.g. `@loop` owns
   `[proc] [@loop]`, popcount 2).  Scripts cannot construct one.

2. **Unreachable by name.** `systemdict` is populated by a single loop over
   `[FIRST_STD_OP, LAST_STD_OP]` (`src/dict.inl`) -- the control-op block sits
   *before* `FIRST_STD_OP`, so no control op is ever bound to a name.  Typing
   `@coroutine-join-check` resolves to nothing; only the engine pushes these.
   Their `@...` strings exist for **diagnostics** -- they surface in backtraces,
   `frame-source-locs`, and disassembly, never as a call target.

3. **Dispatched like any operator.** When the interpreter pops a control-op
   Object off the exec stack, it calls its C++ function through the ordinary
   Operator path.  The resume stage *is* that function.

`@call` is the one most maintainers meet first: it is the frame marker the
interpreter pushes around a procedure call so backtraces and tail-call
optimization have something to walk (see [Tail Call
Optimization](tail-call-optimization.md)).

---

## 3. The trampoline pattern

The shape is always the same:

```text
  start stage (the user-facing op)
    | do whatever can be done now
    | can we finish?
    |   yes -> push the result, return        (no control op needed)
    |   no  -> push a CONTINUATION control op onto the exec stack,
    |          stash any state the resume stage needs,
    |          yield (return to the loop / context-switch away)
    v
  ...time passes; the blocking condition clears...
    |
  resume stage (the @control op, re-entered by the loop)
    | re-check the condition
    |   ready     -> push the result, clean up
    |   not ready -> re-arm: push the control op again and yield  (loop back)
```

### A worked trace: `coroutine-join`

`coroutine-join` ( `coroutine -- value bool` ) blocks until a target coroutine
dies, then yields its return value.  The start stage is `coroutine_join_op`; the
resume stage is `at_coroutine_join_check_op` (both in `src/ops_coroutine.inl`).

**Start stage.** If the target is already dead, join finishes immediately.
Otherwise it records itself as the target's joiner and sleeps, scheduling its
own resume:

```cpp
// start: the target is still alive -- leave its handle on the operand
// stack for the resume stage, register as joiner, and sleep.
target->m_joiner = m_running_coroutine;
coroutine_sleep_and_schedule(SystemName::atCoroutineJoinCheck, /*wake*/ UINT64_MAX);
```

`coroutine_sleep_and_schedule` is where the control op reaches the exec stack:

```cpp
void coroutine_sleep_and_schedule(SystemName continuation_op, uint64_t wake_time) {
    require_exec_capacity(1);
    *++m_exec_ptr = Object::make_control_operator(continuation_op); // resume stage, on top
    coroutine_flush_running();         // save this coroutine's stacks into its context
    auto ctx = offset_to_ptr<CoroutineContext>(m_running_coroutine);
    ctx->m_status = CoroutineContext::Sleeping;
    ctx->m_flags |= CoroutineContext::FlagBlocked;   // a producer will wake us
    coroutine_schedule();              // context-switch to another ready coroutine
}
```

The continuation Object is pushed **on top of** the coroutine's execution
stack, then the whole stack is saved into the coroutine's context.  The
coroutine is now parked.

**Resume stage.** When the target finally dies it wakes the joiner; the
scheduler restores the joiner's exec stack -- with `@coroutine-join-check` on
top -- so the interpreter's very next step calls it:

```cpp
// resume: the target's handle is still on the operand stack.
if (target->m_status != Dead) {
    coroutine_sleep_and_schedule(SystemName::atCoroutineJoinCheck, UINT64_MAX); // re-arm
} else {
    *m_op_ptr = target->m_has_return ? target->m_return_value : Object::make_null();
    *++m_op_ptr = Object::make_boolean(target->m_has_return);   // value, then bool
    coroutine_free_context(target);
}
```

If the target is somehow still alive, the resume stage simply re-arms the
trampoline; otherwise it replaces the handle on the operand stack with the
result.  From the script's side none of this is visible -- the join just
"waits":

```trix
(join blocks via the trampoline until the sleeping coroutine returns)
mark { 0 coroutine-sleep 42 } coroutine-launch coroutine-join pop 42 eq assert
```

The same shape powers `@pipe-get-retry` / `@pipe-put-retry` (a full bounded
buffer), `@actor-recv-check` (an empty mailbox), `@read-key-retry` (no key yet),
and `@coroutine-wait-all-check`.  The `*-barrier` control ops
(`@try-barrier`, `@finally-barrier`, `@with-stream`, `@choice-barrier`,
`@effect-barrier`, ...) are the same idea applied to *non-local exit*: they mark
a spot on the exec stack that `stop` / error unwinding searches for.

---

## 4. Re-entrant state

The C++ call frame of the start stage is **gone** by the time the resume stage
runs -- they are two separate function calls. Any state that must survive the
yield therefore lives somewhere the resume stage can reach:

- **On the operand stack.** The cheapest channel: `coroutine-join` leaves the
  target's coroutine handle on the operand stack, and the resume stage reads it
  straight back.  The operand stack is saved and restored with the coroutine, so
  it rides across the yield untouched.
- **In the coroutine's saved context.** `CoroutineContext` fields
  (`m_joiner`, `m_status`, `m_return_value`, `m_wake_time_ns`, the flags) hold
  per-coroutine state across many context switches.
- **On the execution stack, below the control op.** A control op's frame can
  carry its own operands (the popcount from [§2](#2-what-a-control-operator-is)
  records how many) -- e.g. `@loop` keeps the loop body right under itself.

The execution stack is itself part of the saved state: `coroutine_flush_running`
stores it into the context, so the continuation control op pushed by the start
stage is still on top when the coroutine is resumed.  **Never** stash
cross-stage state in a C++ local or a `static` -- it will not be there on
resume, and with coroutines it would be shared across the wrong context.

---

## 5. Gotchas and invariants

- **No orphaned sleeper.** Every `Sleeping` coroutine must be reachable from at
  least one wake source -- a timer-list entry, a ready-queue slot, a mailbox
  reader registration, a pipe blocked-reader/writer link, or a joiner link --
  or it can never run again.  A start stage that parks a coroutine without
  registering a wake source is the classic bug; `scheduler_validate()`
  (`src/ops_coroutine.inl`, wired into `//:status:vm-validate`) checks the whole
  graph after every test.  Engine bugs #10/#11/#12 were all members of this
  family (a cycled timer list, a stale stdin reader slot, and a restore that
  wiped parked mailbox registrations).
- **Exec-stack discipline.** Push the continuation only after the operands the
  resume stage needs are in place, and make sure each stage leaves the stacks
  balanced.  A barrier control op must be matched: the unwinder relies on
  finding it.
- **Save-level interaction.** Control ops that span a `save` / `restore` window
  must respect the save level.  A successful raw logic `choice`, for instance,
  deliberately leaves its save level open so backtracking can roll back to it --
  code that resumes across such a point must not assume the level returned to
  zero.  See [Save/Restore](save-restore.md) and [Logic](logic.md).
- **Interrupts and error propagation.** Before each iteration the interpreter
  services pending interrupts (`src/types.inl` defines the eight bits:
  `Level0IRQ`, `ErrorIRQ`, `Level1IRQ`, `SuspendIRQ`, `ResumeIRQ`, `InvokeIRQ`,
  `Level2IRQ`, `ExitIRQ`) in priority order
  `Level0 > Error > Level1 > Suspend > Invoke > Level2 > Exit`
  (`ResumeIRQ` is consumed inside the suspend wait).  `ErrorIRQ` resets the exec
  stack to the nearest barrier, which is how an error raised mid-trampoline
  unwinds cleanly past parked control ops instead of resuming them.
- **No member-var access after schedule.** Once `coroutine_schedule()` /
  `coroutine_sleep_and_schedule()` returns, `m_op_ptr`, `m_exec_ptr` and
  every other Trix member belong to whatever coroutine resumed next -- never
  read or write them after scheduling.  Stash everything the resume stage
  needs (operand stack / `CoroutineContext` / exec-stack companions) BEFORE
  the schedule call.  See [dev-invariants.md](dev-invariants.md), "Operator
  Must Not Access Member Vars After Schedule."

---

## 6. Adding a blocking-aware operator

When a new operator may need to wait, follow the existing pattern (and the
general [Adding Operators](dev-adding-operators.md) checklist):

1. **Add the resume-stage `SystemName`** in the control-op range of
   `src/enums.inl` (before `FIRST_STD_OP`), inside the same `#ifdef` as any
   feature it belongs to, with its `@name` string.
2. **Give it a `make_control_operator` case** (`src/object.inl`): choose the
   flag class (`ControlOp` for an ordinary resume, a barrier flag if `stop` /
   error unwinding must find it) and the correct popcount for its frame.
3. **Write the resume-stage function** -- re-check the condition; complete or
   re-arm.
4. **In the start stage**, stash cross-stage state on the operand stack or in
   the context, push the control op (directly, or via
   `coroutine_sleep_and_schedule` for the coroutine case), and return without
   blocking.
5. **Register a wake source** so the parked coroutine is reachable -- or
   `scheduler_validate()` will flag an orphaned sleeper.
6. **Wire the dispatch row** so the new control op's function is reachable, and
   confirm `verify_dispatch_tables()` still passes (it proves every enum slot is
   filled).

---

## 7. Where to read next

- [Interpreter](interpreter.md) -- the dispatch loop, `@call` frames, and the
  IRQ service order in full.
- [Architecture Overview](dev-architecture.md) -- where this fits in the whole
  engine.
- [Coroutines](coroutines.md) -- the user-facing model the trampoline serves,
  plus pipelines, actors, and supervision.
- [Adding Operators](dev-adding-operators.md) and
  [Invariants](dev-invariants.md) -- the broader checklist and the rules the
  runtime depends on.
