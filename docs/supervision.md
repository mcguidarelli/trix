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

# Supervision Framework: Technical Reference

## 1. Overview

The supervision framework adds Erlang/OTP-style fault tolerance to Trix
actors.  It provides three capabilities:

1. **Monitoring** -- unidirectional death observation via `/down` messages
2. **Linking** -- bidirectional death propagation between actors
3. **Supervision** -- automatic child restart with configurable policies

All three are implemented in C++ within `src/ops_supervision.inl` (1813
lines), compiled into the single-header `trix.h`.  Zero external
dependencies.  The supervisor itself is an actor whose main loop is a pair
of control operators (`@supervisor-init`, `@supervisor-check`) that manage
a state machine on the VM heap.

### Why supervision?

Actors provide fault isolation -- a crashing actor does not corrupt other
actors' state.  But without supervision, crashed actors stay dead and the
system degrades silently.  Supervision adds:

- **Death notification**: actors observe when other actors die and why
- **Automatic restart**: crashed actors are restarted according to policy
- **Escalation**: repeated failures propagate up the supervision tree
- **Fault domains**: groups of related actors restart together

This is the mechanism that makes Erlang systems "self-healing."  Trix
achieves the same in a 256KB embeddable runtime.

---

## 2. Concepts

### 2.1 Exit Reasons

Every actor death carries a reason:

| Exit path                      | Reason                          | Type     |
| ------------------------------ | ------------------------------- | -------- |
| Proc returns (falls off end)   | `/normal`                       | Normal   |
| `coroutine-die`                | `/normal`                       | Normal   |
| `actor-exit /normal`           | `/normal`                       | Normal   |
| `actor-exit` with other reason | that reason                     | Abnormal |
| Uncaught error                 | error name (e.g. `/type-check`) | Abnormal |
| `coroutine-kill` (external)    | `/killed`                       | Abnormal |
| Supervisor kills child         | `/shutdown`                     | Abnormal |

The normal/abnormal distinction drives all supervision behavior: normal
exits do not propagate through links or trigger transient restarts.

### 2.2 Monitors

A monitor is a unidirectional death-notification registration.  The
monitored actor is unaware it is being watched.

```
target monitor  % -- ref-id
```

When the target dies, a `/down` tagged message is delivered to the
monitor's mailbox:

<!-- doctest: skip (message-shape illustration with placeholder ref-id/handle/reason) -->
```
/down << /ref ref-id /actor handle /reason reason >> tag
```

Key properties:
- Multiple monitors on the same target create independent entries, each
  with its own ref-id
- Monitoring a dead actor delivers `/down` immediately (no race in the
  cooperative scheduler)
- `demonitor` cancels by ref-id; if the target already died, it is a no-op
- The caller must be an actor (must have a mailbox)

### 2.3 Links

A link is a bidirectional death relationship.  When either linked actor
dies:

- **Normal exit**: no propagation (partner stays alive)
- **Abnormal exit**: the exit signal propagates to the partner

The partner's behavior depends on `actor-trap-exit`:

| `trap-exit`       | Behavior on abnormal partner death                            |
| ----------------- | ------------------------------------------------------------- |
| `false` (default) | Partner is killed with the same reason                        |
| `true`            | Exit signal converted to `/exit` message in partner's mailbox |

<!-- doctest: skip (message-shape illustration with placeholder handle/reason) -->
```
/exit << /actor handle /reason reason >> tag
```

Key properties:
- Links are idempotent: second `link` to the same target is a no-op
- Linking to a dead actor with abnormal reason immediately signals
- Each link creates TWO MonitorEntry structs (one per direction, 48 bytes)
- `unlink` removes both entries; no-op if no link exists

### 2.4 Restart Policies

Per-child restart policy determines whether a dead child is restarted:

| Policy       | Restart on normal exit? | Restart on abnormal exit? |
| ------------ | ----------------------- | ------------------------- |
| `/permanent` | Yes                     | Yes                       |
| `/temporary` | No                      | No                        |
| `/transient` | No                      | Yes                       |

### 2.5 Restart Strategies

How failure of one child affects siblings:

| Strategy        | Behavior                                                            |
| --------------- | ------------------------------------------------------------------- |
| `/one-for-one`  | Restart only the failed child                                       |
| `/one-for-all`  | Kill and restart ALL children when one fails                        |
| `/rest-for-one` | Kill and restart the failed child and all children started after it |

For `/one-for-all` and `/rest-for-one`, children are killed in reverse
start order and restarted in forward start order.

### 2.6 Restart Intensity

A sliding window limits restart rate: maximum N restarts within T
milliseconds.  If exceeded, the supervisor dies with reason `/shutdown`,
escalating to its parent supervisor (if any).

This prevents infinite restart loops when a child crashes on startup.

---

## 3. Operator Reference

### 3.1 Monitor Primitives

```
monitor         coroutine -- int
demonitor       int --
spawn-monitor   mark obj* proc -- coroutine int
```

`monitor` allocates a MonitorEntry on the VM heap (24 bytes), linking it
into both the target's `m_monitors` list and the caller's `m_monitoring`
list.  Returns a monotonically increasing ref-id (31 bits, from
`m_next_monitor_ref`).

`spawn-monitor` is atomic `actor-spawn` + `monitor`.  Equivalent in
behavior but avoids the (theoretical) gap between spawn and monitor.  In
practice, Trix's cooperative scheduler eliminates the race that motivates
this in Erlang, but the combined operator is cleaner for common patterns.

### 3.2 Link Primitives

```
link            coroutine --
unlink          coroutine --
spawn-link      mark obj* proc -- coroutine
```

`link` creates two MonitorEntry structs (48 bytes total): one in each
direction.  Both entries share the same ref_id with the high bit set
(`MonitorTypeMask = 0x80000000`).

`spawn-link` is atomic `actor-spawn` + `link`.

### 3.3 Exit Control

```
actor-exit      any --
actor-trap-exit bool --
```

`actor-exit` pops a reason and kills the calling actor.  Cannot be called
from the main coroutine (`InvalidExit` error).

`actor-trap-exit` sets the `m_trap_exit` flag.  Requires the caller to be
an actor (have a mailbox).

### 3.4 Supervisor

```
supervisor                dict -- coroutine
supervisor-which-children coroutine -- array
supervisor-count-children coroutine -- int
supervisor-get-child      coroutine name -- coroutine true | false
supervisor-spec           coroutine -- dict
supervisor-stop           coroutine --
is-supervisor             any -- bool
```

`supervisor` validates the spec, allocates internal state on the VM heap,
and spawns a supervisor actor.  Returns the supervisor's actor handle.
Child spawning is **synchronous**: by the time `supervisor` returns,
every child in the spec has been spawned, registered, and monitored, so
queries such as `supervisor-get-child` and `supervisor-which-children`
work immediately.  This matches Erlang OTP's `supervisor:start_link`
contract.

`supervisor-which-children` returns an array of dicts for active children:
```
[ << /id /name /actor handle /restart /permanent >> ... ]
```

`supervisor-count-children` returns the number of *active* children as an
Integer.  It raises `/invalid-access` if the supervisor is already dead
and `/type-check` if the handle is not a supervisor.

`supervisor-get-child` looks a child up by its `/id` name, pushing
`handle true` if found or `false` if there is no such (active) child.

`supervisor-spec` returns a read-only configuration dict with `/strategy`,
`/intensity`, `/period`, `/child-count`, `/active-count`, `/max-children`,
and `/restart-count`.  It raises `/invalid-access` on a dead supervisor and
`/type-check` if the handle is not a supervisor.

`is-supervisor` returns `true` only for a live supervisor handle (a
coroutine carrying supervisor state); it returns `false` for ordinary
actors, dead supervisors, and non-coroutine values.

### 3.5 Control Operators (Internal)

| Operator | Purpose |
| --- | --- |
| `@supervisor-init` | Runs when supervisor coroutine first executes: enters the recv loop (children are already spawned synchronously inside `supervisor`) |
| `@supervisor-check` | Wakeup handler: clears blocked reader, re-enters recv loop |

### 3.6 Dynamic Child Management

Children are **not** fixed at creation time.  Three operators add, stop,
and restart children on a running supervisor.  All three are **async**
(fire-and-forget): they post a control message to the supervisor's
mailbox and return immediately, so the effect is visible only after the
supervisor next runs (yield with `coroutine-sleep` between a mutation and
a query).

```
supervisor-start-child     coroutine dict --
supervisor-terminate-child coroutine name --
supervisor-restart-child   coroutine name --
```

- `supervisor-start-child` takes the supervisor handle and a **child
  spec** dict (the same shape as a `/children` entry: `/id`, `/start`,
  `/restart`).  The child is spawned, registered, and monitored.
- `supervisor-terminate-child` stops the child whose `/id` matches the
  given name.  The slot is marked inactive but the `/id` is remembered so
  the child can be restarted later.
- `supervisor-restart-child` respawns a previously terminated child by
  its `/id`.

Each raises `/opstack-underflow` on too few operands and `/type-check`
when the operand types are wrong (supervisor handle must be a coroutine;
the spec must be a dict; the id must be a name).

Worked example -- start, count, terminate, and restart children on a live
supervisor:

```
/yield { 0 1 49 { pop 0 coroutine-sleep } for } def

<<
    /strategy /one-for-one  /intensity 3  /period 30000
    /children [
        << /id /w1  /start { { actor-recv pop } loop }  /restart /permanent >>
    ]
>> supervisor /sup exch def
yield

% One child from the spec.
(start: 1 child) sup supervisor-count-children 1 eq assert

% Add a second child dynamically.
sup << /id /w2  /start { { actor-recv pop } loop }  /restart /permanent >> supervisor-start-child
yield
(after start-child: 2 children) sup supervisor-count-children 2 eq assert

% Terminate one child.
sup /w1 supervisor-terminate-child
yield
(after terminate-child: 1 child) sup supervisor-count-children 1 eq assert

% Restart the terminated child by its id.
sup /w1 supervisor-restart-child
yield
(after restart-child: 2 children) sup supervisor-count-children 2 eq assert

sup supervisor-stop
yield yield clear
```

Because the mutations are async, each is followed by `yield` (a
cooperative-scheduling helper that lets the supervisor drain its mailbox)
before the count is asserted.

---

## 4. Spec Format

<!-- doctest: skip (spec-format illustration with `|` alternation syntax) -->
```
<<
    /strategy   /one-for-one | /one-for-all | /rest-for-one
    /intensity  int            % non-negative; 0 means no restarts allowed
    /period     int            % positive; window size in milliseconds
    /children   [
        << /id /name  /start { proc }  /restart /permanent >>
        << /id /name  /start { proc }  /restart /temporary >>
        << /id /name  /start { proc }  /restart /transient  /capacity 32 >>
    ]
>>
```

### Required Fields

| Key          | Type    | Constraint                                             |
| ------------ | ------- | ------------------------------------------------------ |
| `/strategy`  | Name    | One of `/one-for-one`, `/one-for-all`, `/rest-for-one` |
| `/intensity` | Integer | >= 0                                                   |
| `/period`    | Integer | > 0 (milliseconds)                                     |
| `/children`  | Array   | Non-empty array of child spec dicts                    |

### Child Spec Fields

| Key         | Type    | Constraint                                  | Required?        |
| ----------- | ------- | ------------------------------------------- | ---------------- |
| `/id`       | Name    | Identifier for the child                    | Yes              |
| `/start`    | Proc    | Executable (array, packed, or curry)        | Yes              |
| `/restart`  | Name    | `/permanent`, `/temporary`, or `/transient` | Yes              |
| `/capacity` | Integer | > 0; mailbox capacity                       | No (default: 64) |

### Validation Errors

All validation happens at creation time (inside `supervisor_op`).  If any
field is missing, wrong type, or out of range, the `supervisor` operator
raises `TypeCheck` or `RangeCheck`.  No supervisor is created.

---

## 5. Architecture

### 5.1 Supervisor as an Actor

The supervisor is a regular actor -- it has a CoroutineContext, a mailbox,
and stacks.  Its distinguishing feature is that its execution is driven
entirely by C++ control operators rather than Trix procs.

**Supervisor creation flow:**

```
supervisor_op (runs on caller's coroutine):
  1. Validate spec dict
  2. Allocate SupervisorState on VM heap
  3. Fill child entries from spec
  4. Create 1-element proc array: { @supervisor-init }
  5. Push [mark, proc] on caller's op stack (no state_offset operand)
  6. Call coroutine_launch_common (creates supervisor coroutine)
  7. Allocate mailbox (capacity = max_children + 4)
  8. Store state_offset in supervisor's m_return_value
  9. Push supervisor handle to caller's op stack
```

When the scheduler first runs the supervisor coroutine:

```
Interpreter pops proc from exec stack
  -> execute_proc pushes @supervisor-init
    -> interpreter pops @supervisor-init, dispatches to at_supervisor_init_op
```

### 5.2 SupervisorState (Global VM)

The supervisor's persistent state is a contiguous block in the **global
VM region** (`ChunkKind::Supervisor`).  It survives `save` / `restore`
and is reclaimed by mark-sweep GC once the supervisor coroutine dies:

```
+---------------------------+  offset 0
| SupervisorState (24 bytes)|
+---------------------------+  offset 24
| ChildEntry[0]  (32 bytes) |
| ChildEntry[1]  (32 bytes) |
| ...                        |
| ChildEntry[N-1] (32 bytes)|
+---------------------------+  offset 24 + N*32
| timestamps[0] (4 bytes)   |
| timestamps[1] (4 bytes)   |
| ...                        |
| timestamps[I-1] (4 bytes) |
+---------------------------+  offset 24 + N*32 + I*4
```

Where N = max_children = child_count + 8 (8 spare slots for
runtime-added children; see §3.6), I = max_intensity.

**SupervisorState header (24 bytes):**

| Field           | Type       | Size | Description                            |
| --------------- | ---------- | ---- | -------------------------------------- |
| `strategy`      | `uint8_t`  | 1    | OneForOne=0, OneForAll=1, RestForOne=2 |
| `padding1[3]`   | `uint8_t`  | 3    | Alignment                              |
| `max_intensity` | `uint32_t` | 4    | Maximum restarts in sliding window     |
| `period_ms`     | `uint32_t` | 4    | Window size in milliseconds            |
| `child_count`   | `uint16_t` | 2    | Current number of child entries        |
| `max_children`  | `uint16_t` | 2    | Allocated ChildEntry slots             |
| `restart_count` | `uint32_t` | 4    | Valid entries in timestamps array      |
| `padding3`      | `uint32_t` | 4    | Pad to 24 bytes                        |

**ChildEntry (32 bytes):**

| Field         | Type          | Size | Description                                      |
| ------------- | ------------- | ---- | ------------------------------------------------ |
| `id`          | `Object`      | 8    | Name literal (child identifier)                  |
| `start`       | `Object`      | 8    | Executable proc (array/packed/curry)             |
| `actor`       | `vm_offset_t` | 4    | Current actor handle (nulloffset if not running) |
| `monitor_ref` | `uint32_t`    | 4    | Monitor ref-id for current actor                 |
| `capacity`    | `uint16_t`    | 2    | Mailbox capacity (0 = default 64)                |
| `policy`      | `uint8_t`     | 1    | Permanent=0, Temporary=1, Transient=2            |
| `active`      | `uint8_t`     | 1    | 1 = running, 0 = stopped                         |
| `padding`     | `uint32_t`    | 4    | Alignment                                        |

**Timestamps** are `uint32_t` millisecond values (monotonic clock,
truncated to 32 bits -- ~49-day range before wrap).

### 5.3 Exec Stack Trampoline

The supervisor's main loop uses the exec stack companion pattern:

```
Init phase (before supervisor coroutine ever runs):
  supervisor_op (in caller's context):
    - allocates state, launches supervisor coroutine
    - spawns children, monitors them with supervisor as source
    - sets m_trap_exit on the supervisor
  supervisor coroutine starts Ready with:
  exec: @coroutine-complete | { @supervisor-init }

First tick:
  @supervisor-init runs:
    - reads state_offset from the supervisor context's m_return_value
      (set in supervisor_op before launch)
    - pushes state_offset as exec companion
    - calls supervisor_enter_recv
  exec: @coroutine-complete | state_offset_integer

Recv loop:
  supervisor_enter_recv pushes:
  exec: @coroutine-complete | state_offset_integer | @supervisor-check
  supervisor sleeps (wake_time = max)

Wakeup:
  interpreter pops @supervisor-check:
  exec: @coroutine-complete | state_offset_integer
  @supervisor-check reads state_offset from *m_exec_ptr
  calls supervisor_enter_recv -> processes messages -> re-blocks
```

The `state_offset_integer` companion persists across all recv iterations.
It is only popped when the supervisor dies (intensity exceeded).

### 5.4 Message Processing

The supervisor's recv loop is a direct mailbox scan (not `actor-recv-match`):

1. Pop head message from mailbox
2. If tagged, dispatch on the tag name to one of five control handlers:
   - `/down` -> `supervisor_handle_down` (child death notification)
   - `/start-child` -> `supervisor_handle_start_child` (dynamic add)
   - `/terminate-child` -> `supervisor_handle_terminate_child`
   - `/restart-child` -> `supervisor_handle_restart_child`
   - `/stop` -> `supervisor_handle_stop` (shutdown)
3. If the tag matches none of the five: discard (free ExtValue if applicable)
4. Repeat until mailbox empty
5. Block: set `m_blocked_reader`, push `@supervisor-check`, sleep

This is simpler than `actor-recv-match` because the supervisor handles
`/down` death notifications plus the four dynamic-management control
messages (`/start-child`, `/terminate-child`, `/restart-child`, `/stop`),
discarding only unrecognized tags.  The cooperative scheduler guarantees no
new messages arrive during processing (the supervisor holds the CPU until
it blocks).

### 5.5 Death Notification Integration

When any coroutine dies, `coroutine_kill(ctx, reason)` calls
`supervision_notify_death(ctx, ctx_offset)`.  This function:

1. Walks `ctx->m_monitors` (who is watching me):
   - **Monitor entries**: builds `/down` message, delivers to source's mailbox
   - **Link entries**: propagates exit signal (kill or convert to `/exit`)
2. Walks `ctx->m_monitoring` (who I was watching):
   - Removes self from each target's `m_monitors` list
3. Clears both lists to `nulloffset`

For the supervisor, step 1 delivers `/down` to the supervisor's mailbox
and wakes it (via `supervision_deliver_message`, which wakes the blocked
reader via `coroutine_wake`).

### 5.6 Restart Flow

When the supervisor receives a `/down` for a child:

```
supervisor_handle_down:
  1. Extract ref_id, actor_offset, reason from payload dict
  2. Find child by (ref_id, actor_offset) match
  3. If not found: skip (stale message from previous incarnation)
  4. Mark child inactive
  5. Apply restart policy:
     - Permanent: always restart
     - Temporary: never restart
     - Transient: restart only if reason != /normal
  6. If no restart needed: return
  7. Check restart intensity (sliding window)
  8. If exceeded: kill all children, die with /shutdown
  9. Execute strategy:
     - OneForOne: spawn replacement for failed child
     - OneForAll: kill all others (reverse), restart ALL (forward)
     - RestForOne: kill children after failed (reverse), restart those (forward)
```

**Stale message detection**: after a restart, the child entry has a new
`monitor_ref`.  `/down` messages from previous incarnations carry the old
ref-id, which no longer matches.  These are silently skipped.

### 5.7 Intensity Tracking

The sliding window algorithm:

```
supervisor_check_intensity:
  1. Get current time in milliseconds
  2. Compute cutoff = now - period_ms
  3. Compact: remove all timestamps < cutoff
  4. If remaining count >= max_intensity: return false (exceeded)
  5. Record current timestamp, return true
```

Timestamps are stored in a flat array (not a ring buffer) that is
compacted on each check.  With typical intensity values (3-10), the linear
scan is negligible.

---

## 6. Memory Costs

### 6.1 Per-Supervisor

| Component | Size | Notes |
| --- | --- | --- |
| SupervisorState header | 24 bytes | Fixed |
| ChildEntry array | max_children * 32 bytes | max_children = child_count + 8 (runtime-add headroom) |
| Timestamp array | I * 4 bytes | I = max_intensity |
| Supervisor CoroutineContext | 232 bytes | Standard coroutine |
| Supervisor stacks | ~4.4 KB | 128 op + 256 exec + 16 err + 32 dict + 128 scratch Objects |
| Supervisor mailbox | 16 + (max_children+4) * 8 bytes | Header + message slots |
| Proc array (1 Object) | 8 bytes | Contains @supervisor-init |

**Total overhead for the supervisor actor itself** (excluding children):

For N=3 children (max_children=11), I=5:
- State: 24 + 352 + 20 = 396 bytes
- Context + stacks: ~4.7 KB
- Mailbox: 16 + 120 = 136 bytes
- Proc: 8 bytes
- **Total: ~5.3 KB**

### 6.2 Per-Child

| Component              | Size             | Notes                         |
| ---------------------- | ---------------- | ----------------------------- |
| ChildEntry in state    | 32 bytes         | Already counted in supervisor |
| Child CoroutineContext | 232 bytes        |                               |
| Child stacks           | ~4.4 KB          |                               |
| Child mailbox          | 16 + C * 8 bytes | C = capacity (default 64)     |
| MonitorEntry           | 24 bytes         | Supervisor's monitor on child |

**Total per child** (default capacity):
- Context + stacks: ~4.7 KB
- Mailbox: 16 + 512 = 528 bytes
- MonitorEntry: 24 bytes
- **Total: ~5.3 KB per child**

### 6.3 Per-Link Relationship

Each link creates two MonitorEntry structs: **48 bytes**.

### 6.4 VM Pressure Under Restart

Each restart allocates:
- New CoroutineContext (232 bytes)
- New stacks block (~4.4 KB)
- New mailbox (~144 bytes)
- New MonitorEntry (24 bytes)

The old child's allocations become garbage but are reclaimed by mark-sweep
GC -- automatically on `Error::VMFull`, or on an explicit `vm-global-gc`.
Between collections, dead incarnations accumulate.  With intensity=5 and
period=30000ms, the worst case is 5 restarts before the supervisor dies,
churning ~24 KB of VM heap per burst.

In a 256 KB VM with roughly half the heap already in use (~128 KB free), a
burst of that size leaves ample headroom.  For predictable latency in
real-time systems, place explicit `vm-global-gc` calls at quiescent points
rather than waiting for auto-GC to fire at the next allocation.

---

## 7. Design Decisions

### 7.1 Monitors vs Links for Supervisor-Child Relationship

The supervisor uses **monitors** (not links) to watch children.  Rationale:

- Monitors are one-directional: the child is unaware the supervisor is
  watching.  This is the correct relationship -- the supervisor manages the
  child, not the other way around.
- Links would create bidirectional coupling.  When the supervisor dies, it
  would automatically kill all children through link propagation.  While
  convenient for cleanup, this conflates the supervisor's identity with its
  management role.
- Erlang supervisors also use monitors internally (the `link` behavior
  visible in Erlang is an implementation detail of the supervisor module).

**Trade-off**: when the supervisor is killed externally (via
`coroutine-kill`), children are orphaned.  The controlled death paths
(intensity exceeded, `actor-exit`) explicitly kill children before the
supervisor dies.

### 7.2 C++ Loop vs Trix Proc

The supervisor's main loop is implemented entirely in C++ via control
operators.  An alternative would be a Trix proc that calls
`actor-recv-match` with a predicate.

C++ was chosen because:
- The restart logic (strategy dispatch, intensity tracking, child
  spawning, demonitoring) is complex internal bookkeeping that would be
  awkward to express in Trix
- Direct mailbox access avoids the overhead of `actor-recv-match`'s
  predicate evaluation loop
- The supervisor state machine is performance-critical (it runs on every
  child death)
- Error handling in the supervisor loop is simpler in C++ (no risk of
  uncaught errors in the supervisor proc)

### 7.3 Cooperative Scheduling Implications

Trix uses cooperative (non-preemptive) scheduling.  This eliminates
several race conditions that complicate Erlang's supervision:

- **No spawn-monitor race**: in Erlang, a child can die between `spawn`
  and `monitor`.  In Trix, the child cannot run until the supervisor
  yields, so `spawn-monitor` is atomically equivalent to separate `spawn`
  + `monitor`.
- **No concurrent death processing**: when the supervisor processes a
  `/down`, no other coroutine runs.  Child spawning and monitoring are
  atomic from the scheduler's perspective.
- **Deterministic restart order**: children are spawned in spec order and
  inserted into the scheduler in a deterministic position (after the
  running coroutine).

### 7.4 Timestamp Resolution

Restart timestamps use `uint32_t` milliseconds (truncated from
`monotonic_ns()`).  This gives ~49.7 days before wrap-around.  The
alternative (64-bit nanoseconds) would require 8-byte aligned storage,
complicating the VM heap layout.  Since restart periods are typically
seconds to minutes, millisecond resolution is more than sufficient.

### 7.5 Pre-Interned Names

The supervision framework relies on 8 pre-interned well-known names
(`WellKnownName` enum entries in the shared well-known name table, reached
via `wellknown_name(...)`) to avoid runtime `Name::make` allocations:

| Enumerator                 | Name        | Used for                                 |
| -------------------------- | ----------- | ---------------------------------------- |
| `WellKnownName::Normal`    | `/normal`   | Default exit reason                      |
| `WellKnownName::Killed`    | `/killed`   | External `coroutine-kill` reason         |
| `WellKnownName::Shutdown`  | `/shutdown` | Supervisor shutdown reason               |
| `WellKnownName::Down`      | `/down`     | `/down` tag name                         |
| `WellKnownName::Exit`      | `/exit`     | `/exit` tag name                         |
| `WellKnownName::RefKey`    | `/ref`      | Dict key in `/down` payload              |
| `WellKnownName::ActorKey`  | `/actor`    | Dict key in `/down` and `/exit` payloads |
| `WellKnownName::ReasonKey` | `/reason`   | Dict key in `/down` and `/exit` payloads |

They are interned once during initialization and round-trip through the
snapshot's well-known offset table.

---

## 8. Use Cases

### 8.1 Resilient Worker Pool

A pool of identical workers that process tasks from a shared source.  If a
worker crashes, it is restarted and immediately available for new tasks.

```
/worker-proc {
    { actor-recv process-task 0 coroutine-sleep } loop
} def

<<
    /strategy /one-for-one
    /intensity 10
    /period 60000        % up to 10 restarts per minute
    /children [
        << /id /w1 /start { worker-proc } /restart /permanent >>
        << /id /w2 /start { worker-proc } /restart /permanent >>
        << /id /w3 /start { worker-proc } /restart /permanent >>
    ]
>> supervisor /pool exch def
```

**Why one-for-one**: workers are independent.  A crash in one worker does
not affect others.

**Why permanent**: workers should always be available.

**Cost**: supervisor (~5.3 KB) + 3 workers (~16 KB) = ~21 KB total.

### 8.2 Database + Cache (Coupled Services)

A cache depends on a database connection.  If either crashes, both must
restart to ensure consistency.

```
<<
    /strategy /one-for-all
    /intensity 3
    /period 10000        % max 3 restarts in 10 seconds
    /children [
        << /id /database /start { db-connect-proc } /restart /permanent >>
        << /id /cache    /start { cache-init-proc } /restart /permanent >>
    ]
>> supervisor
```

**Why one-for-all**: cache state depends on database state.  Restarting
just the database leaves the cache with stale data.

**Why low intensity**: if the database connection fails 3 times in 10
seconds, escalating to a parent supervisor is more appropriate than
retrying.

### 8.3 Pipeline Stages with Fault Tolerance

An ordered processing pipeline where later stages depend on earlier ones.

```
<<
    /strategy /rest-for-one
    /intensity 5
    /period 30000
    /children [
        << /id /parser    /start { parser-proc }    /restart /permanent >>
        << /id /validator /start { validator-proc }  /restart /permanent >>
        << /id /writer    /start { writer-proc }     /restart /permanent >>
    ]
>> supervisor
```

**Why rest-for-one**: if the validator crashes, the writer (which depends
on validated data) must also restart.  But the parser (upstream) is
unaffected.

### 8.4 Mixed Policies

Not all children need the same restart behavior.

```
<<
    /strategy /one-for-one
    /intensity 5
    /period 30000
    /children [
        << /id /logger   /start { logger-proc }   /restart /permanent >>
        << /id /metrics  /start { metrics-proc }   /restart /transient >>
        << /id /warmup   /start { warmup-proc }    /restart /temporary >>
    ]
>> supervisor
```

- `/logger`: must always be running (permanent)
- `/metrics`: should survive normal shutdown but restart on crashes (transient)
- `/warmup`: runs once at startup, not restarted (temporary)

### 8.5 Nested Supervision Tree

A two-level tree where a parent supervisor manages child supervisors.

```
/worker-sup-spec <<
    /strategy /one-for-one
    /intensity 5
    /period 10000
    /children [
        << /id /w1 /start { worker-proc } /restart /permanent >>
        << /id /w2 /start { worker-proc } /restart /permanent >>
    ]
>> def

/logger-sup-spec <<
    /strategy /one-for-one
    /intensity 3
    /period 30000
    /children [
        << /id /file-logger /start { file-logger-proc } /restart /permanent >>
        << /id /net-logger  /start { net-logger-proc }  /restart /transient >>
    ]
>> def

<<
    /strategy /one-for-one
    /intensity 3
    /period 60000
    /children [
        << /id /workers /start { worker-sup-spec supervisor
            { actor-recv pop } loop } /restart /permanent >>
        << /id /loggers /start { logger-sup-spec supervisor
            { actor-recv pop } loop } /restart /permanent >>
    ]
>> supervisor /root exch def
```

**Escalation behavior**: if the worker supervisor exceeds its intensity
(5 restarts in 10 seconds), it dies with `/shutdown`.  The root supervisor
receives `/down` for the worker supervisor and restarts it.  The new
worker supervisor spawns fresh workers.

If the root supervisor itself exceeds intensity (3 restarts in 60 seconds),
it dies.  At this point, the system has failed irrecoverably and a human
or external system should intervene.

**Note**: the child supervisor's `/start` proc must keep the supervisor
alive after spawning.  The `{ actor-recv pop } loop` pattern blocks
forever on recv, keeping the actor alive.  Without it, the supervisor
actor would return from its proc and die normally.

### 8.6 Graceful Shutdown

To shut down a supervision tree:

```
/worker-proc { { actor-recv pop 0 coroutine-sleep } loop } def

<<
    /strategy /one-for-one
    /intensity 5
    /period 60000
    /children [
        << /id /w1 /start { worker-proc } /restart /permanent >>
    ]
>> supervisor /root-supervisor exch def

0 1 30 { pop 0 coroutine-sleep } for     % let the children start

root-supervisor coroutine-kill
0 1 30 { pop 0 coroutine-sleep } for     % let the scheduler reap it
```

This kills the root supervisor.  Its children are orphaned (monitors, not
links) and continue running.  To kill the entire tree, the caller must
also kill the children.  (The drain loops give the scheduler a chance to
run the spawned actors and reap the killed supervisor; `coroutine-kill`
itself only marks the target.)

For a cleaner approach, use `actor-exit /shutdown` from within the
supervisor's context (not currently exposed as a user-facing operation).

**Limitation**: external `coroutine-kill` does not trigger the supervisor's
controlled shutdown path.  The supervisor is killed immediately and its
children are orphaned.  This is a deliberate design choice -- the caller
who kills the supervisor should manage the cleanup.

### 8.7 Monitoring a Supervisor

A parent actor can monitor the supervisor itself to detect escalation:

```
[ {
    sup monitor /ref exch def
    actor-recv            % blocks until supervisor dies
    tag-value /reason get % /shutdown if intensity exceeded
    % ... handle escalation ...
} actor-spawn pop
```

This is how nested supervision works: the parent supervisor monitors the
child supervisor and restarts it when it dies.

---

## 9. Interactions with Other Subsystems

### 9.1 Save/Restore

VM Redux Phase 4 moved `MonitorEntry` to global VM
(`ChunkKind::Monitor`); the Phase 5 follow-up moved `SupervisorState`
and the `/start`-proc array to global VM
(`ChunkKind::Supervisor`).  Together with the Phase 5 globalisation
of `CoroutineContext` and `CoroutineStacks`, this means a supervisor
created inside a save scope is **not** rolled back on `restore` --
its state, its children's contexts, its monitor/link entries, and its
mailbox (Phase 3) all live in journal-skipped global VM.

The practical rule is: **make sure the supervisor's `/start` procs and
their captured environment are also addressable after restore**.  Use
pre-interned global Names (`$/foo`) for child identifiers, and place
the `/start` body itself in global (`${...}` around the supervisor
construction) so the proc array entries remain resolvable.  Without
these precautions, the supervisor survives but its child-start logic
may reference local-VM names that vanished at rollback.

### 9.2 Snapshot/Thaw

The snapshot header includes all pre-interned supervision names and
`m_next_monitor_ref`.  `MonitorEntry` structs (now in the global VM
region, `ChunkKind::Monitor`) are serialized as part of the global
VM blob.  After thaw, all monitor/link relationships are intact.

The supervisor's internal state (`SupervisorState`, `ChildEntry`
array, timestamps) is also in global VM (`ChunkKind::Supervisor`)
and survives snapshot/thaw.

**Caveat**: the supervisor's `CoroutineContext` and `CoroutineStacks`
(VM Redux Phase 5) must also survive.  Since both are in the global
region, they round-trip cleanly with the rest of the global blob.

### 9.3 Coroutine Scheduler

The supervisor interacts with the cooperative scheduler in three ways:

1. **Child spawning**: `coroutine_launch_common` inserts children
   immediately after the running coroutine in the circular list.  Children
   spawned last are scheduled first (LIFO insertion).
2. **Blocking on recv**: the supervisor sets
   `wake_time_ns = numeric_limits<uint64_t>::max()` (targeted wakeup
   only).  It does not consume scheduler cycles while waiting.
3. **Wakeup**: `supervision_deliver_message` wakes the supervisor by
   setting its status to Ready and `wake_time_ns` to 0.

### 9.4 Error Handling

If a child's `/start` proc throws an uncaught error, the global error
handler kills the child with the error name as the exit reason.  The
supervisor receives `/down` with that reason and restarts according to
policy.  This is the standard behavior -- no special error handling is
needed.

If the supervisor itself encounters an error (e.g., VM full during child
spawn), the supervisor dies.  Its parent supervisor (if any) can restart
it.

### 9.5 Composability Patterns

The six composability features -- protocols, closures, contracts, pattern
matching, transducers, and GenServer -- compose with supervision.

#### 9.5.1 Supervised Child with GenServer-Style Loop

A supervised child can implement a GenServer-style receive loop.  The
supervisor restarts it on crash, and the loop resumes from its `/start`
proc:

```
/COUNT 0 def
<<
    /strategy /one-for-one  /intensity 5  /period 30000
    /children [
        << /id /worker  /start {
            /COUNT COUNT 1 add def
            COUNT 1 eq { /crash actor-exit } if    % crash on first run
            { actor-recv pop 0 coroutine-sleep } loop
        }  /restart /permanent >>
    ]
>> supervisor
```

After the first crash, the supervisor restarts the child.  `COUNT`
reaches 2, the precondition passes, and the child enters its recv loop.

#### 9.5.2 Closures in Child Specs

Capture configuration in a closure and use it as the child's `/start`
proc.  The frozen configuration travels with the closure:

```
/config-port 8080 def
/result-cell null cell def
{
    config-port result-cell cell-set
    { actor-recv pop 0 coroutine-sleep } loop
} [/config-port /result-cell] closure-capture /start-proc exch def

<<
    /strategy /one-for-one  /intensity 5  /period 30000
    /children [
        << /id /worker  /start { start-proc }  /restart /permanent >>
    ]
>> supervisor
```

The child reads `config-port` from the captured closure and writes it
to the shared cell.  Changing `/config-port` later does not affect
already-captured closures.

#### 9.5.3 Contracts, Crash, Restart

A `precondition` failure inside a child raises `/require`, which
crashes the child.  The supervisor detects the crash and restarts it:

```
/COUNT 0 def
<<
    /strategy /one-for-one  /intensity 5  /period 30000
    /children [
        << /id /strict  /start {
            /COUNT COUNT 1 add def
            COUNT 1 gt precondition    % fails on first run
            { actor-recv pop 0 coroutine-sleep } loop
        }  /restart /permanent >>
    ]
>> supervisor
```

First run: `COUNT` is 1, `1 gt` is false, `precondition` raises
`/require`, child crashes.  Supervisor restarts.  Second run: `COUNT`
is 2, precondition passes, child enters its loop.

#### 9.5.4 Cell State on Restart

Cells are heap-resident objects.  Because the supervisor restarts a
child by calling `/start` again (not resetting the heap), cells written
by a previous incarnation persist:

```
/c 0 cell def
/COUNT 0 def
<<
    /strategy /one-for-one  /intensity 5  /period 30000
    /children [
        << /id /cell-child  /start {
            /COUNT COUNT 1 add def
            c cell-get 1 add c cell-set        % increment cell
            COUNT 1 eq { /crash actor-exit } if
            { actor-recv pop 0 coroutine-sleep } loop
        }  /restart /permanent >>
    ]
>> supervisor
```

First run: cell goes from 0 to 1, child crashes.  Second run: cell
goes from 1 to 2.  `c cell-get` returns 2 -- the cell preserved state
across the restart boundary.

---

## 10. Limitations

1. **Restart memory churn**: each child restart allocates a fresh
   `CoroutineContext`, `CoroutineStacks`, and (if the actor uses a
   mailbox) `Mailbox`.  These live in the global VM region and are
   reclaimed by mark-sweep GC after the old child dies.  GC fires
   automatically on `Error::VMFull`; for predictable latency in
   real-time systems, place explicit `vm-global-gc` calls at known
   quiescent points instead of letting auto-GC fire at the next
   allocation.

2. **Orphaned children on external kill**: killing the supervisor via
   `coroutine-kill` does not kill its children.  They continue running
   without supervision.

3. **No `delete_child` and no dynamic *strategy* change**: children can be
   added, terminated, and restarted at runtime (see §3.6 -- this is no
   longer a limitation), but there is no operator to permanently *delete*
   a terminated child's slot (it is retained so it can be restarted by
   `/id`), and a supervisor's `/strategy`, `/intensity`, and `/period` are
   fixed at creation time.

4. **Timestamp wrap-around**: the `uint32_t` millisecond timestamp wraps
   after ~49.7 days of continuous operation.  This could cause incorrect
   intensity calculations at the wrap point.

5. **No child start arguments**: the `/start` proc receives no arguments.
   Shared state must be passed via userdict or closures.

6. **Single-VM only**: no distributed supervision.  Monitors and links
   operate within a single Trix instance.

7. **Mailbox capacity**: the supervisor's mailbox capacity is
   `max_children + 4` (with `max_children = child_count + 8`).  If many
   children die simultaneously (e.g., cascade
   through links), messages may be dropped.  The supervisor handles this
   gracefully (stale message detection).

8. **No `simple_one_for_one`**: Erlang's dynamic worker pool strategy is
   not implemented.  Use a fixed child list with the desired number of
   workers.

---

## 11. File Map

| File                         | Content                                        |
| ---------------------------- | ---------------------------------------------- |
| `src/ops_supervision.inl`    | All supervision operators (~1813 lines)        |
| `src/ops_coroutine.inl`      | CoroutineContext, MonitorEntry, coroutine_kill |
| `src/ops_actor.inl`          | MailboxHeader, actor_alloc_mailbox             |
| `src/enums.inl`              | SystemName entries for all operators           |
| `src/dispatch.inl`           | Operator dispatch entries                      |
| `src/object.inl`             | make_control_operator cases                    |
| `src/init.inl`               | Pre-interned name initialization               |
| `src/member_vars.inl`        | Supervision member variables                   |
| `src/snapshot.inl`           | SnapShotHeader fields for supervision          |
| `src/ops_snapshot.inl`       | Save/restore of supervision header fields      |
| `tests/test_supervision.trx` | Monitor/link primitives (69 assertions)        |
| `tests/test_supervisor.trx`  | Supervisor operator (103 assertions)           |
