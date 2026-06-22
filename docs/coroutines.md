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

# Trix Coroutines: Technical Reference

---

## 1. What Are Trix Coroutines?

Trix coroutines are cooperative, stackful execution contexts, scheduled with
two priority tiers (FIFO within each tier), that share a single VM heap.
Each coroutine has its own operand,
exec, error, and dict stacks.  All coroutines share the VM heap,
save stack, interrupt system, I/O streams, systemdict, protocoldict, and userdict.

A coroutine yields control explicitly by calling `coroutine-sleep`.  The
scheduler picks the next eligible coroutine and switches to it by swapping
the Trix interpreter's stack pointers.  The interpreter loop itself is
completely unmodified -- context switching happens inside operator
implementations, invisible to the dispatch loop.

There is no preemption.  A coroutine runs until it yields, dies, or its
launch proc returns.  This makes reasoning about shared state trivial:
between two consecutive yield points, a coroutine has exclusive access to
the entire VM.

### 1.1 Why Coroutines in Trix?

Trix is inherently sequential: a single interpreter loop pops from the
exec stack and dispatches.  Without coroutines, any task requiring
interleaved execution (producers/consumers, cooperative I/O, simulation
ticks, background maintenance) requires the programmer to manually
decompose work into callbacks or state machines wired through the
dictionary.

Coroutines provide:

- **Natural control flow** for concurrent tasks.  Each coroutine has its own
  call stack, loop state, try/catch scopes, and local variables.  No manual
  state machine construction.

- **Deterministic scheduling**.  Two priority tiers with explicit yield points
  mean no data races, no locks, no atomics.  High-priority coroutines
  (`coroutine-priority 1`) are always run before normal ones; round-robin
  describes only same-priority ordering.  The programmer controls exactly where
  interleaving occurs.

- **Zero interpreter overhead**.  The interpreter loop is unchanged.  Context
  switching is a pointer swap in a handful of operator implementations.
  Single-coroutine programs pay nothing.

- **Bounded, predictable memory**.  Each coroutine's stacks are a fixed-size
  contiguous block (~4.4 KB) allocated in the **global VM region**
  (`ChunkKind::CoroutineStacks`); the control block (`CoroutineContext`,
  232 bytes) is a separate global allocation.  Both survive `save` /
  `restore` and are reclaimed by mark-sweep GC after the coroutine
  dies and the two-tier free list lets go.  No GC pauses on the
  scheduler hot path; collection fires only on `VMFull` or explicit
  `vm-global-gc`.

### 1.2 Design Philosophy

Three principles guided the design:

**Cooperative, not preemptive.**  Preemptive scheduling would require the
interpreter loop to check a "should I yield?" flag on every dispatch cycle,
adding overhead to every single operation.  Cooperative scheduling keeps the
hot path untouched and gives the programmer explicit control over
interleaving points.

**Stackful, not stackless.**  Each coroutine gets full operand, exec,
error, and dict stacks.  This means a coroutine can yield from inside
a nested procedure call, a loop body, or a try/catch block.  Stackless
coroutines (which save only the top-level continuation) cannot do this.

**Shared heap, isolated stacks.**  Coroutines compute independently (each has
its own stacks) but communicate naturally through shared VM heap objects.
Dicts, arrays, and strings allocated by any coroutine are visible to all
others.  This matches Trix's existing single-heap model and avoids the
complexity of message passing or channel primitives.

---

## 2. The Coroutine Type

Coroutines are first-class Trix objects with type value 24 (`Type::Coroutine`).

```
Object attributes: PushOpDirect | IgnoresExecute | UsesVM
Storage:           vm_offset_t pointing to a CoroutineContext on the VM heap
Equality:          identity (two coroutine Objects are equal iff they point
                   to the same CoroutineContext)
Hashing:           based on the VM offset (identity hash)
Execution:         not executable (make-executable raises type-check)
Packing:           not supported (coroutines are mutable runtime state)
```

A coroutine Object is a handle -- a lightweight 8-byte value that refers to
a CoroutineContext allocated on the VM heap.  Multiple copies of the same
handle all refer to the same underlying context.

### 2.1 Type Predicates

```
42 is-coroutine =      % -> false
mark { } coroutine-launch
dup is-coroutine =     % -> true
dup type =             % -> coroutine-type
coroutine-kill
```

`is-coroutine` is the standard type predicate.  `type` returns
`/coroutine-type`.  Coroutines participate in all standard type dispatch
(verify, convert, print) but cannot be used as numbers, keys, or procedure
bodies.

---

## 3. CoroutineContext: Internal Structure

Every coroutine (including main) is backed by a `CoroutineContext` struct
allocated on the VM heap:

```
CoroutineContext (232 bytes; assert <= 232)
+----------------------+----------------------------------------------+
| field group          | purpose                                      |
+----------------------+----------------------------------------------+
| m_status, m_flags    | Created/Ready/Running/Dead + Suspended/Blocked |
| m_has_return         | true if return value captured                |
| m_activation_sl      | save level at last context switch            |
| m_context_offset     | self-offset (restore barrier)                |
| m_wake_time_ns       | monotonic ns; 0 = immediate                  |
| m_suspend_remaining  | preserved timer delta when suspended         |
| scheduler links      | m_next, m_ready_next/prev, m_timer_next      |
| m_id/m_quantum/...   | id, time quantum, priority, ops-remaining    |
| m_joiner             | coroutine blocked on join                    |
| m_return_value       | captured top-of-stack at death (GC root)     |
| stack bounds (x4)    | op / exec / err / dict base/ptr/limit/hw     |
| m_scanner_stream     | active scanner stream pointer                |
| m_mailbox            | MailboxHeader offset (actors)                |
| m_monitors/monitoring| monitor + link lists (supervision)           |
| m_exit_reason, ...   | exit reason + diagnostic caches              |
+----------------------+----------------------------------------------+
```

The struct grew well past its original 96 bytes as the scheduler,
supervision, and actor subsystems landed; the field groups above are
representative, not a byte-accurate map.  It stores VM offsets (not raw
pointers) for all stack bounds.
This makes the entire coroutine state relocatable within the VM heap --
critical for snapshot/thaw support.

### 3.1 Status Lifecycle

```
                 coroutine-launch
                      |
                      v
    Created -----> Ready -------> Running
                     ^               |
                     |    schedule   |
                     +-------+------+
                             |
                     coroutine-sleep
                             |
                             v
                         Sleeping
                             |
                     wake_time <= now
                             |
                             v
                          Ready -----> Running
                                         |
                           coroutine-die / return / error / coroutine-kill
                                         |
                                         v
                                       Dead
```

Status values: Created=0, Sleeping=1, Ready=2, Running=3, Dead=4.

A coroutine transitions to Dead when:
- Its launch proc returns normally (hits `@coroutine-complete`)
- It calls `coroutine-die` (self-terminate)
- Another coroutine calls `coroutine-kill` on it
- An uncaught error propagates out of the launch proc

Once Dead, the context stays allocated (holding the return value) until
consumed by `coroutine-join` or `coroutine-release`, or reclaimed by
save/restore.

### 3.2 Per-Coroutine vs Shared State

| Resource         | Ownership     | Rationale                                  |
| ---------------- | ------------- | ------------------------------------------ |
| Operand stack    | Per-coroutine | Independent computation                    |
| Exec stack       | Per-coroutine | Own call chain, loops, control ops         |
| Error stack      | Per-coroutine | Own try/catch scopes                       |
| Dict stack       | Per-coroutine | Own begin/end scopes                       |
| Scanner stream   | Per-coroutine | Each may be scanning different input       |
| VM heap          | **Shared**    | Single allocator, all objects visible      |
| Save stack       | **Shared**    | Transactions are global                    |
| Interrupt system | **Shared**    | Serviced by whichever coroutine is running |
| I/O streams      | **Shared**    | stdin/stdout/stderr are global             |
| systemdict       | **Shared**    | All operators available to all coroutines  |
| userdict         | **Shared**    | Communication through shared definitions   |

The dict stack deserves special attention.  Each coroutine's dict stack
starts with `[systemdict, protocoldict, userdict]` -- the same Dict objects shared across
all coroutines.  A `def` in any coroutine writes to the shared userdict,
immediately visible to all others.  A coroutine can `begin` its own private
dicts without affecting other coroutines' dict stacks.

This design means:
- Global definitions are naturally shared (no message passing needed)
- Private scope is available via `begin`/`end`
- No synchronization is needed (cooperative scheduling = no concurrent access)

---

## 4. The Scheduler

### 4.1 Data Structures

The scheduler pops from two ready queues (high-priority drained first), each a
doubly-linked FIFO, and sweeps a sorted timer list for sleeping coroutines.
The circular `m_next` list is a separate all-coroutines *registry* (used by
`coroutine-by-id`, `coroutine-kill-all`, and GC marking), not the scheduling
queue.  Member variables track the scheduler state:

<!-- doctest: skip (scheduler member-variable listing, not runnable code) -->
```
m_ready_head / m_ready_tail            normal-priority ready FIFO
m_ready_high_head / m_ready_high_tail  high-priority ready FIFO (drained first)
m_timer_head                           sorted timer list (sleeping coroutines)
m_coroutine_head                       all-coroutines registry (circular list via m_next)
m_running_coroutine                    currently executing CoroutineContext offset
m_main_context                         coroutine #0 (the main program, never killed)
m_live_coroutine_count                 number of non-dead coroutines excluding main
```

Main (coroutine #0) is always in the registry and is never removed.  Its
context uses the Trix interpreter's own stack arrays (allocated during
`init_and_interpret`), not a separate coroutine stack block.

### 4.2 Context Switching

Context switching is a pointer swap, not a system call.  The running
coroutine's stack pointers are saved from the Trix member variables into
its CoroutineContext (`coroutine_flush_running`), and the target
coroutine's pointers are loaded back (`coroutine_load`):

```
coroutine_flush_running():
    ctx->m_op_base    = ptr_to_offset(m_op_base)
    ctx->m_op_ptr     = ptr_to_offset(m_op_ptr + 1)     // one-past convention
    ctx->m_exec_base  = ptr_to_offset(m_exec_base)
    ctx->m_exec_ptr   = ptr_to_offset(m_exec_ptr + 1)
    ... (error stack, dict stack, scanner stream)

coroutine_load(target):
    m_op_base    = offset_to_ptr(ctx->m_op_base)
    m_op_ptr     = offset_to_ptr(ctx->m_op_ptr) - 1     // restore convention
    m_exec_base  = offset_to_ptr(ctx->m_exec_base)
    m_exec_ptr   = offset_to_ptr(ctx->m_exec_ptr) - 1
    ... (error stack, dict stack, scanner stream)
    m_running_coroutine = ctx->m_context_offset
    ctx->m_status = Running
```

The `+1`/`-1` adjustments handle the Trix stack pointer convention where
`m_op_ptr` points at the top element (not one past), while the stored
offset uses a one-past-empty convention for clean empty-stack representation.

After `coroutine_load` returns, the interpreter loop continues executing --
but now it's operating on different stack arrays.  The interpreter itself
has no knowledge that a context switch occurred.

### 4.3 Scheduling Algorithm

The scheduler (`coroutine_schedule`) is invoked by `coroutine-sleep`,
`coroutine-die`, `coroutine-kill`, and `@coroutine-complete`.  It returns
to the interpreter loop (not to the C++ caller), so operators must not
perform any work after calling it.

```
1. Sweep expired timers off the sorted timer list (wake_time <= now),
   moving each woken coroutine onto its ready queue
2. Pop the ready queue -- high-priority FIFO (m_ready_high_head) drained
   before the normal FIFO (m_ready_head).  If a coroutine pops: switch to
   it, return.  This pop is O(1)
3. If both ready queues are empty, sweep timers again, then park the OS
   thread (poll for a stdin reader / nanosleep until the earliest timer
   expiry) and retry
4. If nothing is runnable: reload main
```

The scheduling decision is O(1) -- a head pop from a ready queue -- not a
walk over all coroutines.  The only linear work is the timer sweep, which
touches just the already-expired prefix of the sorted timer list.

### 4.4 How Context Switching Integrates with the Interpreter

A critical design insight: `coroutine_schedule()` does not return to its
C++ caller.  It swaps the stack pointers and returns to the interpreter
loop.  This means:

- Operators that call `coroutine_schedule()` must not have any code after
  the call that depends on the current coroutine's state.
- Blocking operators (like `coroutine-join` and `coroutine-wait-all`) use
  exec-stack sentinel control operators (`@coroutine-join-check`,
  `@coroutine-wait-all-check`) instead of C++ while-loops.  The control
  operator is pushed onto the exec stack before yielding; when the coroutine
  resumes, the interpreter pops and executes the sentinel, which checks
  whether the blocking condition is met and either completes or re-yields.

This pattern preserves the invariant that all Trix control flow lives on
the exec stack, never on the C++ call stack.

---

## 5. Operator Reference

### 5.1 coroutine-launch

```
mark obj1 ... objN proc  coroutine-launch  --  coroutine
```

Creates a new coroutine.  The proc (which must be executable: array, packed,
or curry) becomes the coroutine's entry point.  Objects between the mark and
the proc are moved (not copied) onto the new coroutine's operand stack as
initial parameters.  The mark and parameters are consumed from the
launcher's stack.  Returns a coroutine handle.

**Parameter passing**: Objects are transferred by moving the 8-byte Object
values.  For value types (integer, real, boolean, byte, null, mark, name),
this is a simple copy.  For VM-backed types (arrays, strings, dicts), both
coroutines now hold references to the same heap data -- which is the
intended shared-heap semantics.  For ExtValue types (long, ulong, double,
address), ownership transfers naturally: the Object's offset points to the
same ExtValue, and the coroutine that eventually frees it (via stack cleanup
at death) is responsible for returning it to the free list.

**Errors**: `type-check` if proc is not executable; `unmatched-mark` if no
mark found; `opstack-overflow` if parameter count exceeds coroutine operand
stack depth (128); `vm-full` if allocation fails.

**Example**:
```
% Launch with no parameters
[ { (hello) = } coroutine-launch

% Launch with parameters
[ 10 20 { add = } coroutine-launch

% Store handle for later use
[ { 0 coroutine-sleep 42 } coroutine-launch /worker exch def
```

### 5.2 coroutine-sleep

```
int  coroutine-sleep  --
```

Cooperatively yields to the scheduler.  N is the minimum number of
milliseconds before this coroutine becomes re-eligible for scheduling.
`0 coroutine-sleep` yields immediately and is re-eligible on the next
scheduling pass.

When no coroutines are active (`m_live_coroutine_count == 0`), this operator
falls back to sleeping the real OS thread via `nanosleep` -- preserving
backward compatibility with pre-coroutine code.

**Errors**: `type-check` if operand is not an integer; `range-check` if
operand is negative.

**Example**:
<!-- doctest: skip (polling-loop illustration with placeholder work-available?/do-work) -->
```
% Yield immediately (give other coroutines a chance to run)
0 coroutine-sleep

% Sleep at least 100ms
100 coroutine-sleep

% Polling loop
{
    work-available? { do-work } if
    10 coroutine-sleep
} loop
```

### 5.3 coroutine-die

```
coroutine-die  --
```

Self-terminates the currently running coroutine.  The coroutine's return
value is captured from the top of its operand stack (if non-empty), stacks
are freed, status transitions to Dead, and the scheduler switches to the
next coroutine.

Main (coroutine #0) cannot die -- calling `coroutine-die` from main raises
`invalid-exit`.

**Errors**: `invalid-exit` if called from main.

**Example**:
```
[ {
    some-condition? { coroutine-die } if % early exit, no return value
    42                                   % normal exit, returns 42
} coroutine-launch
```

### 5.4 coroutine-kill

```
coroutine  coroutine-kill  --
```

Kills another coroutine by handle.  The target's stacks are cleaned up,
ExtValues freed, status set to Dead.  If the target has a joiner blocked on
it, the joiner is woken with `null false` (no return value).

Killing an already-dead coroutine is a no-op.  Killing self raises
`invalid-exit` (use `coroutine-die` instead).  Killing main raises
`invalid-exit`.

**Errors**: `type-check` if operand is not a coroutine; `invalid-exit` if
target is self or main.

**Example**:
```
[ { { 10 coroutine-sleep } loop } coroutine-launch /bg exch def
% ... do work ...
bg coroutine-kill       % terminate the background coroutine
```

### 5.5 is-coroutine

```
any  is-coroutine  --  bool
```

Type predicate.  Returns true if the operand is a coroutine Object.

### 5.6 coroutine-status

```
coroutine  coroutine-status  --  name
```

Returns the coroutine's current status as a name: `/sleeping`, `/ready`,
`/running`, `/suspended`, or `/dead`.  (`/suspended` is produced by
`coroutine-suspend`; see section 5.11.)  The handle remains valid even
after the coroutine dies (until freed by `coroutine-join` or
`coroutine-release`).

**Errors**: `type-check` if operand is not a coroutine.

### 5.7 coroutine-join

```
coroutine  coroutine-join  --  value bool
```

Blocks the calling coroutine until the target coroutine dies.  When the
target dies, pushes the target's return value (or null if none) and a
boolean flag indicating whether a return value was available.

If the target is already dead when `coroutine-join` is called, the result
is returned immediately without blocking.

After `coroutine-join` returns, the target's CoroutineContext is freed to
the context free list.  The handle should not be used again.

**Return value protocol**: A coroutine opts in to returning a value by
leaving it on the top of its operand stack when it exits.  It opts out by
leaving the stack empty (including an empty-stack `coroutine-die`), being
killed via `coroutine-kill`, or dying from an uncaught error.  A
`coroutine-die` with a non-empty stack returns its top value exactly like
normal completion, as section 5.3 describes.

```
% Target returns 42
[ { 42 } coroutine-launch coroutine-join
% stack: 42 true

% Target has no return value
[ { } coroutine-launch coroutine-join
% stack: null false

% Target dies from error
[ { 1 0 div } coroutine-launch coroutine-join
% stack: null false
```

**Errors**: `type-check` if operand is not a coroutine; `invalid-exit` if
target is self.

### 5.8 coroutine-wait-all

```
coroutine-wait-all  --
```

Blocks the calling coroutine until all other coroutines have completed
(m_live_coroutine_count reaches 0).  If no coroutines are active, returns
immediately.

This is a convenience operator for "fire and forget" patterns where you
launch several coroutines and want to wait for all of them to finish
without tracking individual handles.

**Example**:
```
[ { task-a } coroutine-launch pop
[ { task-b } coroutine-launch pop
[ { task-c } coroutine-launch pop
coroutine-wait-all      % blocks until all three are done
```

### 5.9 coroutine-release

```
coroutine  coroutine-release  --
```

Discards a dead coroutine's return value without reading it and frees the
CoroutineContext to the context free list.  Use this when you don't need
the return value but want to reclaim the 232-byte context memory.

**Errors**: `type-check` if operand is not a coroutine; `invalid-exit` if
the coroutine is not dead.

### 5.10 Control Operators (Internal)

Four control operators support the coroutine system.  These are pushed
onto the exec stack by standard operators and are not directly
accessible to user code.

| Control Operator            | Purpose                                                        |
| --------------------------- | -------------------------------------------------------------- |
| `@coroutine-complete`       | Bottom of every coroutine's exec stack; handles normal exit    |
| `@coroutine-join-check`     | Sentinel for blocking join; checks if target is dead           |
| `@coroutine-await-check`    | Sentinel for blocking await; pushes value or rethrows on death |
| `@coroutine-wait-all-check` | Sentinel for wait-all; checks if all coroutines done           |

### 5.11 Identity, Scheduling, and Introspection

Beyond the lifecycle operators above, the scheduler exposes a set of
identity, suspension, priority, and link operators.

| Operator | Stack Effect | Description |
| --- | --- | --- |
| `coroutine-self` | `-- coroutine` | Push the running coroutine's own handle |
| `coroutine-id` | `coroutine -- int` | Stable integer id of a coroutine |
| `coroutine-by-id` | `int -- coroutine` | Look up a live coroutine by its id |
| `coroutine-suspend` | `coroutine --` | Suspend a coroutine (status becomes `/suspended`); no-op if already suspended |
| `coroutine-resume` | `coroutine --` | Resume a suspended coroutine; no-op if not suspended |
| `coroutine-priority` | `coroutine int --` | Set scheduling priority (`0` = normal, `1` = high) |
| `coroutine-quantum` | `int --` | Set the running coroutine's op-count quantum (`0` = unlimited) |
| `coroutine-kill-all` | `--` | Kill every coroutine except the caller |
| `coroutine-await` | `coroutine -- value` | Block until the target dies; push its value or rethrow its error (link semantics) |
| `coroutine-last-error` | `-- name` | Exit reason of the most recently observed coroutine, or `/no-error` |

`coroutine-await` is the link-flavoured dual of `coroutine-join`: a normal
exit yields the return value, but an abnormal exit (`/killed`, `/shutdown`,
or an uncaught error) is rethrown in the awaiter's context.
`coroutine-priority` and `coroutine-quantum` raise `range-check` for values
outside their allowed ranges.

---

## 6. Memory Model

### 6.1 Allocation Costs

Each coroutine requires two heap allocations:

**CoroutineContext** (232 bytes): The control block holding status, stack
pointers, return value, and scheduler/supervision linkage.

**Stack block** (~4,480 bytes): A contiguous allocation partitioned into
four interpreter stacks plus a scratch arena:

```
Stack block layout (560 Objects = 4,480 bytes):
+--------------+----------------+-----------+-----------+--------------+
| Operand (128)| Execution (256)| Error (16)| Dict (32) | Scratch (128)|
| 1,024 bytes  | 2,048 bytes    | 128 bytes | 256 bytes | 1,024 bytes  |
+--------------+----------------+-----------+-----------+--------------+
```

**Total per coroutine: ~4,712 bytes** (232 + 4,480).

Compare with main's stacks: 1,024 + 2,048 + 64 + 64 = 3,200 Objects =
25,600 bytes.  Coroutine stacks are still markedly smaller.

**Capacity on default 1 MB VM**: Because coroutine stacks live in the
separately-sized global VM region, the per-coroutine local-heap arithmetic
does not bound the count directly.  Empirically the default 1 MB VM
supports ~174 simultaneous live coroutines; a forced 256 KB VM (the
minimum) supports ~10.  With the free list recycling stack blocks from dead
coroutines, the throughput over time is much higher.

### 6.2 Two-Tier Free List

Dead coroutines leave behind two allocations.  The free list reclaims them
at different times:

**Tier 1 -- Stack block (~4.4 KB)**: Freed immediately at coroutine death.
The first 4 bytes of the stack block are repurposed as a free-list link
pointer.  The next `coroutine-launch` checks this free list before
allocating fresh heap space.

**Tier 2 -- CoroutineContext (232 bytes)**: Stays alive after death because
it holds the return value and status.  Handles remain valid for
`coroutine-status`, `coroutine-join`, and `coroutine-release`.  Freed when
consumed by `coroutine-join` or `coroutine-release`, or discarded during
save/restore.

This means:
- The bulk of memory (~96% of per-coroutine cost) is reclaimed immediately
- Dead handles are always safe to inspect
- Context leak for un-joined coroutines is 232 bytes (negligible)
- save/restore is the nuclear option for bulk reclaim

### 6.3 Memory Lifecycle Example

```
% VM state: 200KB free, 0 coroutines

[ { 0 coroutine-sleep 42 } coroutine-launch /co exch def
% VM state: ~195.3KB free, 1 coroutine (4,712 bytes allocated)

co coroutine-join pop pop
% Stack block returned to free list (~4,480 bytes reclaimable)
% Context freed to context free list (232 bytes reclaimable)
% VM state: all coroutine memory reclaimable via free lists

[ { 99 } coroutine-launch /co2 exch def
% Reuses stack block and context from free lists -- no new allocation
```

---

## 7. Interactions with Trix Subsystems

### 7.1 try/catch/finally

try/catch works correctly inside coroutines.  Each coroutine has its own
error stack, so error handlers are scoped to the coroutine that installed
them.

```
[ {
    { 1 0 div } try    % catches div-by-zero within this coroutine
    /div-by-zero eq { (caught!) = } if
} coroutine-launch
```

### 7.2 Uncaught Errors

An uncaught error in a non-main coroutine kills the coroutine (status ->
Dead, no return value captured) and schedules the next one.  The error does
NOT propagate to other coroutines.  Error details are stored in
`last-error` / `last-error-message` (shared state, so they may be
overwritten by subsequent errors in other coroutines).

If a joiner is blocked on the errored coroutine, the joiner wakes and
receives `null false`.

```
[ { 1 0 div } coroutine-launch coroutine-join
% stack: null false
% The div-by-zero killed the coroutine; joiner sees no return value
```

### 7.3 Loops

Loops (`for`, `repeat`, `loop`, `while`) work correctly inside coroutines.
Loop control operators live on the coroutine's own exec stack.  A
coroutine can yield from inside a loop body:

```
[ {
    1 1 100 {
        dup process-item
        0 coroutine-sleep       % yield after each item
    } for
} coroutine-launch
```

### 7.4 Local Variables (begin/end, begin-locals)

The dict stack is per-coroutine, so `begin`/`end` and local variable
bindings (`{ |x y| ... }`) work correctly and independently in each
coroutine:

```
[ {
    << /counter 0 >> begin
    10 { /counter counter 1 add def 0 coroutine-sleep } repeat
    counter    % returns 10
    end
} coroutine-launch
```

### 7.5 Save/Restore

Save/restore interacts with coroutines through a safety constraint:
`restore` must not leave any coroutine's stacks holding dangling
references into the rolled-back local-VM region.

Since VM Redux Phase 5, `CoroutineContext` and `CoroutineStacks` are
both allocated in the **global VM region**.  They are
journal-skipped and survive `restore` unconditionally; mark-sweep GC
reclaims them once the coroutine is dead and the free list releases
the block.  The control structures themselves are therefore never
the issue at restore time.

What `restore` does still check:

**Suspended-coroutine stack content**: If a suspended (non-running)
coroutine's operand, exec, error, or dict stack holds a
composite Object (Array, String, Record, etc.) allocated in the
**local** VM above the save barrier, `restore` raises
`/invalid-restore`.  Otherwise that reference would dangle into
reclaimed bytes after rollback.  Scalar values (Integer, Real,
Boolean, Name, ExtValue) on a suspended coroutine's stack are always
safe; composite Objects allocated in the global VM (`${...}`,
`#$`, `set-global true` etc.) are also safe.

An optimization avoids scanning all coroutines: each
`CoroutineContext` records the save level at its last context switch
(`m_activation_sl`).  Coroutines that were never active during the
save scope are skipped — they could not have acquired post-save
local-heap references.

**Practical implication**: A coroutine launched inside a save scope
will survive `restore` of that scope as a live, GCed entity in
global VM, even if all its `def`s and locals were rolled back.  To
keep work it accumulated, allocate the accumulator in global
(`${...}` or `set-global`) or use the persist family.  Coroutines
that hold local-heap composite references at suspend time block
`restore` -- which is correct: that's data they will need on resume.

### 7.6 Snapshot/Thaw

Snapshot captures the entire VM heap, which includes all CoroutineContexts
and their stack blocks.  Before snapshot, the running coroutine's live
stack pointers (Trix member variables) are flushed into its
CoroutineContext so the heap copy is complete.

On thaw, the scheduler state is restored from the SnapShotHeader.  Sleeping
coroutines' `wake_time_ns` values will be in the past relative to the
current monotonic clock -- this is correct behavior: they all become
immediately re-eligible and resume execution.

### 7.7 Interrupts

The interrupt system is shared across all coroutines.  An interrupt raised
by the host program (via `raise_interrupt`) is serviced by whichever
coroutine is currently running when the interpreter checks the interrupt
flag.  There is no mechanism to direct an interrupt to a specific coroutine.

### 7.8 Stopped/Exit

`stop` and `exit` walk the exec stack looking for their corresponding
sentinel operators.  Since each coroutine has its own exec stack, these
operators are correctly scoped to the coroutine that calls them.

### 7.9 TCO (Tail Call Optimization)

TCO examines the active exec stack to detect tail position.  Since
each coroutine has its own exec stack segment, TCO works correctly
and independently in each coroutine.  The @call detection mechanism is
identical -- when the coroutine's exec stack top is @call, the frame is
reused.

Tested: 10,000-depth self-recursion and 5,000-depth mutual recursion inside
coroutines (test_coroutine.trx sections 21-22).

---

## 8. Programming Patterns

### 8.1 Fire and Forget

Launch coroutines for side effects, don't need results:

```
[ { log-file flush-and-rotate } coroutine-launch pop
[ { metrics send-to-server } coroutine-launch pop
coroutine-wait-all
```

### 8.2 Parallel Computation with Join

Launch workers, collect results:

```
[ 1000 { |n| n factorial } coroutine-launch /f1 exch def
[ 2000 { |n| n factorial } coroutine-launch /f2 exch def

f1 coroutine-join pop /result1 exch def
f2 coroutine-join pop /result2 exch def
```

While `f1` is being joined (and potentially still running), `f2` continues
to execute during scheduling rounds.

### 8.3 Producer/Consumer

Use shared state for communication between coroutines:

```
/buffer [ ] def
/done false def

% Producer: generates items.  `append` returns a new array, so rebind the
% shared /buffer name to the grown queue (arrays are value types).
[ {
    0 1 99 {
        /buffer buffer over append def
        pop
        0 coroutine-sleep
    } for
    /done true def
} coroutine-launch pop

% Consumer: takes the head item, then rebinds /buffer to the rest.
% `1 drop` slices off the front -- there is no in-place delete-at-index op.
[ {
    {
        buffer length 0 gt {
            buffer 0 get process-item
            /buffer buffer 1 drop def
        } if
        done buffer length 0 eq and { exit } if
        0 coroutine-sleep
    } loop
} coroutine-launch pop

coroutine-wait-all
```

### 8.4 Periodic Background Task

```
/should-run true def

[ {
    {
        should-run not { exit } if
        do-periodic-maintenance
        1000 coroutine-sleep          % run every ~1 second
    } loop
} coroutine-launch /bg exch def

% ... main work ...
/should-run false def
bg coroutine-join pop pop
```

### 8.5 Timeout Pattern

```
/timed-out false def

% Worker
[ { expensive-computation } coroutine-launch /worker exch def

% Timeout watchdog
[ worker { |w|
    5000 coroutine-sleep              % 5-second deadline
    w coroutine-status /dead ne {
        /timed-out true def
        w coroutine-kill
    } if
} coroutine-launch pop

worker coroutine-join
timed-out { (timed out!) = pop } { (result: ) print = } if-else
```

### 8.6 Round-Robin Task Pool

```
/tasks [ { task-a } { task-b } { task-c } { task-d } ] def

tasks { [ exch coroutine-launch pop } for-all
coroutine-wait-all
```

### 8.7 Cooperative Pipeline

<!-- doctest: skip (illustrative pipeline; needs input-data/transform-* helpers) -->
```
/stage1-out [ ] def
/stage2-out [ ] def
/stage1-done false def

% Stage 1: transform each input item, append to stage1-out (rebind the
% shared name, since `append` returns a new array).
[ {
    input-data {
        transform-a
        /stage1-out exch stage1-out exch append def
        0 coroutine-sleep
    } for-all
    /stage1-done true def
} coroutine-launch pop

% Stage 2: drain stage1-out head-first, transform, append to stage2-out.
[ {
    {
        stage1-out length 0 gt {
            stage1-out 0 get transform-b
            /stage2-out exch stage2-out exch append def
            /stage1-out stage1-out 1 drop def
        } if
        stage1-done stage1-out length 0 eq and { exit } if
        0 coroutine-sleep
    } loop
} coroutine-launch pop

coroutine-wait-all
```

### 8.8 Barrier Synchronization

```
/barrier-count 0 def
/barrier-target 3 def

/barrier-wait {
    /barrier-count barrier-count 1 add def
    { barrier-count barrier-target ge { exit } if 0 coroutine-sleep } loop
} def

[ { phase-1-work-a barrier-wait phase-2-work-a } coroutine-launch pop
[ { phase-1-work-b barrier-wait phase-2-work-b } coroutine-launch pop
[ { phase-1-work-c barrier-wait phase-2-work-c } coroutine-launch pop
coroutine-wait-all
```

### 8.9 Recursive Parallel Decomposition

```
/parallel-fib {
    dup 20 lt { fib } {     % below threshold: compute directly
        dup 1 sub
        [ exch { |n| n parallel-fib } coroutine-launch
        exch 2 sub parallel-fib
        exch coroutine-join pop
        add
    } if-else
} def
```

This spawns coroutines for the recursive calls, allowing the scheduler to
interleave the work.  The threshold (20) prevents spawning too many
coroutines for small subproblems.

---

## 9. Performance Characteristics

### 9.1 Context Switch Cost

A context switch consists of:
1. `coroutine_flush_running`: 14 pointer-to-offset conversions + 1 pointer store
2. Scheduler decision: O(1) head pop from the two-tier ready queue (plus an
   expired-timer sweep over the already-due prefix of the sorted timer list)
3. `coroutine_load`: 14 offset-to-pointer conversions + 2 stores

The total cost is dominated by the ~28 pointer conversions -- roughly 30
pointer arithmetic operations, comparable to a single dictionary lookup --
and is independent of the number of live coroutines.

### 9.2 Launch Cost

`coroutine-launch` performs:
- 1 context allocation (free list check + possible heap alloc: 232 bytes)
- 1 stack block allocation (free list check + possible heap alloc: ~4,480 bytes)
- N object moves for parameters
- 4 stack pointer initializations
- 3 dict stack entries (systemdict + protocoldict + userdict references)
- 1 linked list insertion

With free list reuse, launch after a previous coroutine has died requires
zero new heap allocation.

### 9.3 Death/Cleanup Cost

Coroutine death walks all four stacks to free ExtValues and clear dict
name bindings.  For a coroutine with D operand stack entries, E exec stack
entries, and K pushed dicts, the cost is O(D + E + K).  The stack block is
returned to the free list in O(1).

### 9.4 Memory Overhead

| Component        | Size         | When freed                    |
| ---------------- | ------------ | ----------------------------- |
| CoroutineContext | 232 bytes    | On join/release               |
| Stack block      | 4,480 bytes  | On death (immediate)          |
| Main context     | 232 bytes    | Never (lives for VM lifetime) |
| Main stacks      | 25,600 bytes | Never (Trix member arrays)    |
| Status names     | ~40 bytes    | Never (pre-interned)          |

For N simultaneously live coroutines: ~4,712 * N bytes.

### 9.5 Scheduling Overhead

The scheduling decision is O(1): a head pop from the two-tier ready queue
(the high-priority FIFO drained before the normal one).  The only linear
work is sweeping the sorted timer list, which touches just the already-due
prefix.  Dead coroutines are unlinked from the registry as they die, so they
are never scanned during a scheduling pass.

When all coroutines are sleeping with future wake times, the scheduler
sleeps the real OS thread until the earliest wake time, avoiding busy-wait.

---

## 10. Diagnostics

### 10.1 Status Introspection Keys

Six `:status` keys provide runtime visibility into the coroutine system:

```
//:status:coroutine-count        % int: live non-main coroutines
//:status:coroutine-total        % int: total entries in scheduler list
//:status:coroutine-stack-free   % int: reusable stack blocks in free list
//:status:coroutine-ctx-free     % int: reusable contexts in free list
//:status:coroutine-is-main      % bool: true if running coroutine is main
//:status:coroutine-running      % int: VM offset of running CoroutineContext
```

These can also be accessed at runtime via `query-status`:

```
/coroutine-count query-status    % -> integer
```

### 10.2 Debugging Techniques

**Monitor coroutine count**:
```
(live coroutines: ) print /coroutine-count query-status =
```

**Check free list health** (detect leaks):
```
% After all coroutines should be dead:
/coroutine-count query-status 0 eq
    { (all coroutines completed) } { (LEAK: coroutines still alive) } if-else =
```

**Verify coroutine identity**:
```
[ { 100 coroutine-sleep } coroutine-launch
dup coroutine-status =           % -> ready
dup /co exch def
co coroutine-status =            % -> ready (same handle)
co coroutine-kill
co coroutine-status =            % -> dead
```

**Instrument yields** (count context switches):
```
/yield-count 0 def
/counted-sleep {
    /yield-count yield-count 1 add def
    coroutine-sleep
} def

[ { 0 counted-sleep 0 counted-sleep 0 counted-sleep } coroutine-launch
coroutine-join pop pop
yield-count =                    % -> 3
```

---

## 11. Design Rationale

### 11.1 Why Cooperative, Not Preemptive?

Preemptive scheduling would require the interpreter loop to check a
"should I yield?" flag on every dispatch cycle.  At ~47 million dispatched
operations per second (see `benchmark/`), even a single branch adds
measurable overhead to every operation -- including programs that never
use coroutines.

Cooperative scheduling has zero cost when not in use.  The interpreter loop
is completely unmodified.  The only runtime cost is the `if
(m_live_coroutine_count > 0)` check in `coroutine-sleep`, which is a single
branch on a cached member variable.

Additionally, cooperative scheduling eliminates all concurrency hazards.
Between two yield points, a coroutine has exclusive access to the entire
VM.  No locks, no atomics, no memory barriers, no race conditions.  This
matches Trix's philosophy of deterministic, predictable execution.

### 11.2 Why Stackful, Not Stackless?

Stackless coroutines can only yield at the top level of the coroutine body.
You cannot yield from inside a nested procedure, a loop, or a try/catch
block.  This forces the programmer to restructure their code into
non-blocking segments -- essentially building a state machine manually.

Trix coroutines are stackful: each has a full exec stack (256 slots),
so you can yield from any call depth.  This means natural control flow:

```
% Stackful: yield from inside a nested call
/process-batch {
    { |item|
        item validate
        item transform
        item store
        0 coroutine-sleep        % yield between items
    } for-all
} def

[ data { process-batch } coroutine-launch
```

### 11.3 Why Sleep-Based Yield, Not send/recv?

Many coroutine systems use bidirectional value passing through yield points
(send a value out, receive a value in).  Trix chose a simpler model:

- `coroutine-sleep` yields with a time parameter, no value exchange
- Communication happens through shared heap objects (dicts, arrays)
- Results are retrieved via `coroutine-join` at death

This choice reflects several considerations:

1. **Shared heap makes channels redundant.**  Since all coroutines share the
   same VM heap, a shared dict or array is already a communication channel.
   Adding a built-in channel primitive would duplicate existing capability.

2. **Simpler operator semantics.**  Sleep takes one integer and returns
   nothing.  No special "generator" protocol, no "yield expression" concept,
   no asymmetric vs symmetric confusion.

3. **Extensibility.**  If channel/mailbox primitives prove necessary,
   `send`/`recv`/`self` operators (+3) can be added later without
   architectural changes.  The current design does not preclude them.

### 11.4 Why Main Is Coroutine #0?

Making the main program a coroutine (rather than a special "not a coroutine"
execution context) simplifies the scheduler:

- The circular linked list always has at least one entry (main)
- `coroutine-sleep` in main works naturally (yields to other coroutines)
- The `@coroutine-complete` control operator handles both main-finishes
  (kill all remaining coroutines) and coroutine-finishes (normal death)
- No special-case code paths for "is there a scheduler or not?"

Main's context uses the Trix interpreter's own stack arrays (not a separate
coroutine stack block), so there is no memory overhead for the "main is a
coroutine" abstraction.  Main's `m_stacks_offset` is `nulloffset`,
indicating its stacks are the Trix member arrays.

### 11.5 Why No Preemptive Kill?

`coroutine-kill` sets the target's status to Dead, frees its stacks, and
wakes any joiner -- but it does not interrupt the target mid-execution.
The target must not be the currently running coroutine (use `coroutine-die`
for self-termination).

This is because the target might be in the middle of a multi-step operation
(e.g., a `for` loop with control operators on the exec stack, a `try` with
error handlers on the error stack, a `begin` with dicts on the dict stack).
The cleanup code (`coroutine_cleanup_stacks`) walks all four stacks to
free resources, which is safe because the target is not running.

### 11.6 Why Not Clone Launch Parameters?

Launch parameters are moved, not cloned.  This means:

- Zero-copy for all types (no redundant allocation)
- ExtValue ownership transfers naturally (no double-free risk)
- Composite objects (arrays, dicts, strings) are shared by reference,
  which is consistent with Trix's shared-heap semantics
- The launcher loses access to the moved objects (they're consumed from
  its stack along with the mark)

Cloning would require allocating new ExtValues for long/ulong/double/address
parameters and deep-copying composites.  Since the shared heap means both
coroutines see the same underlying data anyway, cloning would be wasteful.

### 11.7 Why Two-Tier Free List?

A single free list would require either:
- Freeing the CoroutineContext immediately at death (losing the return
  value, making join impossible), or
- Keeping the entire ~4.7 KB allocation alive until join (wasting memory)

The two-tier design gives us the best of both worlds: the expensive stack
memory (~4.4 KB) is reclaimed immediately, while the cheap context (232
bytes) stays alive just long enough to deliver the return value.

---

## 12. Limitations and Constraints

### 12.1 Stack Size Is Fixed

Coroutine stacks are fixed at 128 operand + 256 execution + 16 error + 32
dictionary slots.  There is no dynamic growth.  Deep recursion or deeply
nested procedure calls within a coroutine will hit the stack limit.

For comparison, main has 1,024 operand + 2,048 execution + 64 error + 64
dictionary slots.  Algorithms that need deep stacks should run on main
or decompose work across multiple coroutines.

### 12.2 No Preemption

A coroutine that enters an infinite loop without yielding will starve all
other coroutines.  There is no watchdog timer or involuntary preemption.
Defensive programming requires placing `0 coroutine-sleep` calls at
appropriate points in long-running loops.

### 12.3 Save/Restore Constraint

Coroutines launched inside a save scope are **not** destroyed by `restore`.
Since VM Redux Phase 5, `CoroutineContext` and `CoroutineStacks` live in the
**global VM region**, are journal-skipped, and survive `restore`
unconditionally (mark-sweep GC reclaims them once the coroutine is dead).
See section 7.5 for the full model.

```
save /lvl exch def
[ { 42 } coroutine-launch /co exch def % launched above the save barrier
0 1 5 { pop 0 coroutine-sleep } for    % let co run to completion
co coroutine-status =                  % -> dead
lvl restore                            % succeeds: co survived restore
```

What `restore` does still enforce is the dangling-reference check: if a
**suspended** coroutine's operand/exec/error/dict stack holds a *local*-VM
composite (Array, String, packed proc, ...) allocated above the save
barrier, `restore` raises `/invalid-restore` rather than leave that
reference dangling into reclaimed bytes.  Scalars and global-region
composites (`${...}`, `#$`, `set-global`) on a coroutine's stack are always
safe.  The control structures themselves are never the problem.

### 12.4 No Cross-Coroutine Error Propagation

An uncaught error in a coroutine kills only that coroutine.  The error is
not propagated to other coroutines or to the joiner.  The joiner sees
`null false` from `coroutine-join` but must check `last-error` to
determine the cause.

### 12.5 Shared State Hazards

While cooperative scheduling eliminates data races, shared mutable state
can still cause logical errors if coroutines make assumptions about when
other coroutines modify shared data.  Between any two yield points, the
state of shared dicts/arrays may have changed.

<!-- doctest: skip (placeholder shared-dict, shared-state hazard illustration) -->
```
% Hazard: value may change between reads
shared-dict /key get       % read value
0 coroutine-sleep          % yield -- another coroutine may modify /key
shared-dict /key get       % value may be different now
```

### 12.6 Handle Validity After Join/Release

After `coroutine-join` or `coroutine-release`, the coroutine handle points
to freed memory (returned to the context free list).  Using the handle
after this point is undefined behavior.  The handle should be discarded
(popped from the stack).

---

## 13. Implementation File Map

| File                       | Role                                                          |
| -------------------------- | ------------------------------------------------------------- |
| `src/ops_coroutine.inl`    | CoroutineContext struct, scheduler, all operators, free lists |
| `src/object.inl`           | Type::Coroutine (value 24), make/is/offset accessors, attribs |
| `src/enums.inl`            | SystemName entries for operators and control ops              |
| `src/types.inl`            | DefaultCoroutine*Depth constants                              |
| `src/member_vars.inl`      | Scheduler state, status name Objects, free list heads         |
| `src/init.inl`             | Main context allocation, status name interning                |
| `src/dispatch.inl`         | Dispatch entries for 19 standard + 4 control operators        |
| `src/verify.inl`           | VerifyCoroutine constant                                      |
| `src/save.inl`             | Save barrier check, kill coroutines above barrier             |
| `src/ops_system.inl`       | Uncaught error handler (kills coroutine, schedules next)      |
| `src/ops_snapshot.inl`     | Flush running context before snapshot, save/restore state     |
| `src/snapshot.inl`         | SnapShotHeader coroutine fields                               |
| `src/printfmt.inl`         | Coroutine print formatting                                    |
| `tests/test_coroutine.trx` | 89-assertion test suite across 24 sections                    |

---

## 14. Quick Reference

```
LAUNCH      [ obj* proc coroutine-launch    -- coroutine
YIELD       int coroutine-sleep             --
SELF-KILL   coroutine-die                   --
KILL OTHER  coroutine coroutine-kill        --
TYPE TEST   any is-coroutine                -- bool
STATUS      coroutine coroutine-status      -- name
JOIN        coroutine coroutine-join        -- value bool
WAIT ALL    coroutine-wait-all              --
RELEASE     coroutine coroutine-release     --

STATUS NAMES: /sleeping /ready /running /suspended /dead
STACK SIZES:  128 op, 256 exec, 16 err, 32 dict, 128 scratch (4,480 bytes)
CONTEXT SIZE: 232 bytes
TOTAL COST:   ~4,712 bytes per coroutine
CAPACITY:     ~174 simultaneous on default 1 MB VM
```
