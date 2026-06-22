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

# Actor Framework: Technical Reference

A comprehensive technical document for Trix maintainers and programmers
covering the architecture, implementation, usage patterns, costs, design
choices, and real-world scenarios of the actor message-passing framework.

**Audience**: Trix maintainers (C++ implementation details) and Trix
programmers (usage patterns and performance characteristics).

**Prerequisite reading**: `docs/coroutines.md` (cooperative scheduling model),
`docs/pipeline-internals.md` (targeted wakeup pattern, PipeBufferHeader),
`docs/vm-internals.md` (VM heap, Object representation).

---

## 1. Architectural Overview

The actor framework is a structured communication layer built on a single
existing primitive:

- **Coroutines** -- cooperative multitasking (round-robin scheduled,
  ~4.7 KB per coroutine, shared VM heap)

An actor is a coroutine with an attached **mailbox** -- a bounded circular
buffer on the VM heap.  Any coroutine can send messages to an actor; only
the owning actor can receive from its mailbox.  This is many-to-one
communication with backpressure.

```
                        actor-send
  sender-1 --------.
  sender-2 --------+-----> [ mailbox ] ----> actor-recv (owner only)
  sender-3 --------'       (bounded)
  main     --------'
```

No new types are introduced.  An actor is referenced by the same
coroutine handle returned by `actor-spawn`.  The `is-actor` predicate
distinguishes actors from plain coroutines by checking the
CoroutineContext's `FlagWasActor` flag (set at spawn, preserved past
death).

### 1.1 Relationship to Other Systems

| System | Purpose | Communication |
| --- | --- | --- |
| **Coroutines** | Cooperative multitasking | Shared heap, explicit yield |
| **Pipelines** | Linear dataflow (source -> stages -> sink) | Implicit via bounded buffers, SPSC |
| **Actors** | Arbitrary-topology message passing | Explicit via `actor-send`/`actor-recv`, many-to-one |

Actors complement pipelines.  Pipelines handle linear data transformation
chains; actors handle arbitrary communication topologies (request/response,
pub/sub, supervision trees, state machines).  A pipeline stage is a
coroutine that reads from one buffer and writes to the next; an actor is
a coroutine that reads from its own mailbox and sends to any other actor.

### 1.2 Design Philosophy

**Actors are coroutines.**  No new type, no new scheduler, no new memory
model.  The framework adds exactly one thing: a per-coroutine mailbox.
Everything else -- scheduling, stack management, save/restore, snapshot/thaw,
coroutine-join -- works unchanged.

**Bounded mailboxes with backpressure.**  Unbounded mailboxes risk VM
memory exhaustion.  Bounded mailboxes provide automatic flow control: a
fast sender blocks when the mailbox is full, naturally throttling
production.  This is the same model used by pipeline buffers.

**Move semantics for messages.**  Messages are moved from the sender's
operand stack into the mailbox.  No cloning, no allocation.  The sender
no longer holds the message after sending.  For value types (int, real,
bool, name, null), move is identical to copy.  For VM-backed types (arrays,
strings, dicts), both sender and mailbox hold references to the same heap
data -- safe because Trix is single-threaded with cooperative scheduling.

**Silent drop on send to dead actor.**  Sending to a dead actor discards
the message without error.  This prevents cascading failures when actors
die and restart (Layer 3: Supervision).  Monitoring is the correct
mechanism for death notification, not error handling on every send.

---

## 2. Memory Layout

### 2.1 CoroutineContext Extension

A single field was added to `CoroutineContext`:

```cpp
struct CoroutineContext {
    // ... existing fields (status, stacks, joiner, etc.) ...
    Stream *m_scanner_stream;  // per-coroutine scanner stream state
    vm_offset_t m_mailbox;     // offset to MailboxHeader (nulloffset = no mailbox)
};
static_assert(sizeof(CoroutineContext) <= 232, "CoroutineContext size check");
```

Adding `m_mailbox` was the first of several growth steps; with the
scheduler, supervision, and monitor/link fields the struct is now 232
bytes (the assert bounds it at `<= 232`).  Non-actor coroutines have
`m_mailbox = nulloffset` and pay no extra mailbox storage -- only the
4-byte field itself.

The field is initialized to `nulloffset` in all coroutine creation paths:
- `coroutine_launch_common()` (used by `coroutine-launch` and `actor-spawn`)
- `pipe_create_stage_coroutine()` (pipeline stage creation)
- `pipe_create_source_coroutine()` (pipeline source creation)
- Main context initialization in `init.inl`

### 2.2 MailboxHeader

```cpp
struct MailboxHeader {                 // 16 bytes
    length_t    m_head;                // read index (circular)
    length_t    m_tail;                // write index (circular)
    length_t    m_count;               // current occupancy
    length_t    m_capacity;            // maximum messages
    vm_offset_t m_blocked_reader;      // actor waiting on recv (nulloffset = none)
    vm_offset_t m_blocked_sender;      // first sender blocked on full (nulloffset = none)
};
static_assert(sizeof(MailboxHeader) == 16, "MailboxHeader must be 16 bytes for Object alignment");
```

The mailbox is allocated as a **single contiguous block** in the
**global VM region** (`ChunkKind::Mailbox`, VM Redux Phase 3):

```
[MailboxHeader (16 bytes)][Object[capacity] (capacity * 8 bytes)]
```

The Object array immediately follows the header.  This design provides:
- Cache locality (header and data are adjacent)
- Single allocation (no separate header + data allocs)
- Natural alignment (16 bytes is a multiple of 8 = sizeof(Object))
- Survival across `save` / `restore`: journal-skipped.  On actor death the
  block is recycled to a class-bucketed free-list pool (`m_mailbox_pool`)
  for reuse by later spawns, not reclaimed by per-death GC

The data array is accessed via `reinterpret_cast<Object *>(mbx + 1)`.

### 2.3 Memory Budget Per Actor

| Component | Bytes | Notes |
| --- | --- | --- |
| CoroutineContext | 232 | Global VM (`ChunkKind::CoroutineContext`); pool-recycled on death (`m_coroutine_ctx_free` free list) |
| Stacks block | 4,480 | 560 Objects (128 op + 256 exec + 16 err + 32 dict + 128 scratch); global VM (`ChunkKind::CoroutineStacks`) |
| MailboxHeader | 16 | Global VM (`ChunkKind::Mailbox`); recycled to a class-bucketed free-list pool on death |
| Message array | 512 | 64 Objects * 8 bytes (default capacity) |
| **Total** | **~5,240** |  |

On a 256 KB VM with ~200 KB free after initialization: **~39 simultaneous
actors** at default capacity.  Custom capacity (`actor-spawn-capacity`)
trades mailbox size for actor count.

| Capacity | Mailbox bytes | Total per actor | Max actors (200 KB) |
| -------- | ------------- | --------------- | ------------------- |
| 1        | 24            | ~4,736          | ~43                 |
| 4        | 48            | ~4,760          | ~43                 |
| 16       | 144           | ~4,856          | ~42                 |
| **64**   | **528**       | **~5,240**      | **~39**             |
| 256      | 2,064         | ~6,776          | ~30                 |

### 2.4 Default Capacity

```cpp
static constexpr length_t MaxActorMailboxCapacity{64};  // in types.inl
```

Chosen to balance burst absorption, memory cost, and backpressure
responsiveness.  64 messages = 512 bytes of buffer.  Enough to absorb
short bursts without blocking; small enough that backpressure kicks in
quickly to prevent memory exhaustion.

---

## 3. Operator Reference

### 3.1 Actor Lifecycle

| Operator | Stack Effect | Description |
| --- | --- | --- |
| `actor-spawn` | `mark obj* proc -- coroutine` | Launch actor with default-capacity mailbox |
| `actor-spawn-capacity` | `mark obj* proc int -- coroutine` | Launch actor with specified mailbox capacity |
| `is-actor` | `any -- bool` | True if argument is a coroutine with a mailbox |

**`actor-spawn`**: Pops proc and mark (with optional parameters between
them), creates a new CoroutineContext + stacks + mailbox, inserts into
the scheduler, pushes the coroutine handle.  Parameters between the mark
and proc are moved to the actor's operand stack (same as `coroutine-launch`).
The proc becomes the actor's body.  Shares implementation with
`coroutine-launch` via `coroutine_launch_common()`.

```
[ 10 20 { add = } actor-spawn   % actor starts with 10, 20 on its stack
```

**`actor-spawn-capacity`**: Same as `actor-spawn` but with an explicit
capacity.  The integer capacity is popped before the proc.

```
[ { actor-recv = } 256 actor-spawn-capacity   % 256-message mailbox
```

Errors:
- `type-check`: proc is not executable (array, packed, or curry)
- `unmatched-mark`: no mark found on operand stack
- `range-check`: capacity is zero or negative
- `vm-full`: insufficient heap space

**`is-actor`**: Returns true if and only if the argument is a Coroutine
whose CoroutineContext has the `FlagWasActor` flag set (set at spawn,
preserved past death).  Returns false for all non-coroutine types and for
plain (non-actor) coroutines.  Works on dead actors because the flag — not
the mailbox field — is the marker; the mailbox is recycled and `m_mailbox`
is cleared on death.

### 3.2 Message Passing

| Operator             | Stack Effect           | Description                                |
| -------------------- | ---------------------- | ------------------------------------------ |
| `actor-send`         | `message coroutine --` | Send message to actor's mailbox            |
| `actor-recv`         | `-- message`           | Receive from own mailbox (blocks if empty) |
| `actor-recv-timeout` | `int -- message bool`  | Receive with timeout in milliseconds       |
| `actor-recv-match`   | `pred -- message`      | Receive first message matching predicate   |
| `actor-self`         | `-- coroutine`         | Push own coroutine handle                  |

**`actor-send`**: Moves the message from the sender's operand stack into
the target actor's mailbox.  Behavior depends on target state:

| Target state              | Behavior                                           |
| ------------------------- | -------------------------------------------------- |
| Alive, space available    | Message delivered; receiver woken if sleeping      |
| Alive, mailbox full       | Sender blocks until space available (backpressure) |
| Dead                      | Message silently discarded (no error)              |
| Not a coroutine           | Error: `type-check`                                |
| Coroutine without mailbox | Error: `invalid-access`                            |

When the mailbox is full, the sender blocks and is appended to a FIFO
linked list of blocked senders (threaded through
`CoroutineContext::m_blocked_sender_next`, with `m_blocked_sender` as the
head).  When a slot frees, `actor-recv` wakes the head sender (O(1)
targeted) and advances the head to the next; on actor death or flush all
blocked senders are woken.  There is no polling fallback.

Messages are any Trix Object: integers, reals, strings, arrays, dicts,
records, tagged values, names, booleans, null, Longs, Doubles, ULongs,
Addresses, bytes.  ExtValue types (Long, ULong, Double, Address) are
moved without cloning -- the sender's Object is copied into the mailbox
slot, transferring the ExtValue reference.

**`actor-recv`**: Removes and returns the oldest message from the calling
actor's mailbox (FIFO order).  If the mailbox is empty, the actor blocks
until a message arrives.  Blocking uses targeted wakeup: the actor sleeps
with `wake_time_ns = UINT64_MAX` and is woken by the next `actor-send`.

Errors:
- `invalid-access`: called from a coroutine without a mailbox

**`actor-recv-timeout`**: Like `actor-recv` but with a deadline.  The
integer argument is a timeout in milliseconds.

| Outcome                        | Stack result   |
| ------------------------------ | -------------- |
| Message arrives before timeout | `message true` |
| Timeout expires                | `null false`   |

The timeout uses the scheduler's time-based wakeup: `wake_time_ns` is
set to the deadline.  The deadline is stored as a Long on the exec stack
(companion to the `@actor-recv-timeout-check` control operator).

A timeout of 0 checks the mailbox once and returns immediately.

**`actor-recv-match`**: Selective receive.  Scans the mailbox from oldest
to newest.  For each message, executes the predicate `pred` with the
message on the operand stack.  The first message for which `pred` returns
`true` is removed from the mailbox and returned.  Non-matching messages
remain in the queue in their original order.

If no messages match, the actor blocks until a new message arrives and
re-scans from the beginning.

```
% Receive the first message > 25, skip others
{ 25 gt } actor-recv-match
```

The predicate must consume the message from the operand stack and push a
boolean.  The predicate is executed by the Trix interpreter (not in C++),
so it can be any executable proc, packed array, or curry.

Implementation uses a trampoline with a scan-index sentinel: `-1` means
"just woke from sleep, start fresh scan"; `>= 0` means "pred just
returned bool for message at this index".

The mailbox removal is O(n) in mailbox occupancy (shift subsequent
messages down).  For capacity-16 mailboxes this is at most 15 Object
copies = 120 bytes.

**`actor-recv-match-timeout`**: Selective receive with timeout.  Hybrid
of `actor-recv-match` and `actor-recv-timeout`.  Scans the mailbox for
a matching message; returns `msg true` if found, `null false` on timeout.

```
% Receive first message > 25, or timeout after 1000ms
{ 25 gt } 1000 actor-recv-match-timeout
```

| Outcome                    | Stack Result   |
| -------------------------- | -------------- |
| Match found before timeout | `message true` |
| Timeout expires            | `null false`   |

Completes the receive matrix:

|          | FIFO                 | Selective                  |
| -------- | -------------------- | -------------------------- |
| Blocking | `actor-recv`         | `actor-recv-match`         |
| Timed    | `actor-recv-timeout` | `actor-recv-match-timeout` |

**`actor-self`**: Pushes the currently running coroutine's handle.  Works
in any coroutine, not just actors.  This is how an actor includes its own
address in messages for reply-to patterns.

<!-- doctest: skip (synopsis: server is a stand-in actor handle) -->
```
<< /value 42 /reply-to actor-self >> server actor-send
```

### 3.3 Mailbox Inspection

| Operator                 | Stack Effect        | Description                           |
| ------------------------ | ------------------- | ------------------------------------- |
| `actor-mailbox-count`    | `coroutine -- int`  | Number of messages in mailbox         |
| `actor-mailbox-empty?`   | `coroutine -- bool` | True if mailbox has no messages       |
| `actor-mailbox-capacity` | `coroutine -- int`  | Mailbox capacity (default 64)         |
| `actor-flush`            | `-- int`            | Drain own mailbox; push drained count |

Non-blocking inspection operators.  Can be used on any actor (not just
self).  Raise `invalid-access` if the coroutine has no mailbox.
`actor-flush` operates on the *calling* actor's own mailbox, frees the
remaining messages, wakes any senders that were blocked on a full mailbox,
and pushes the number of messages it drained.

### 3.4 Naming, Status, and Broadcast

| Operator | Stack Effect | Description |
| --- | --- | --- |
| `actor-set-name` | `name coroutine --` | Attach a debug name to an actor |
| `actor-name` | `coroutine -- name true \| -- false` | Push the debug name + `true`, or just `false` if unnamed |
| `actor-status` | `coroutine -- dict` | Read-only introspection dict (see below) |
| `actor-try-send` | `message coroutine -- bool` | Non-blocking send; `true` if delivered, `false` if full/dead |
| `actor-broadcast` | `message array --` | Send a clone of `message` to each actor in `array` |

`actor-set-name` / `actor-name` attach and read a diagnostic label; an
unnamed actor pushes only `false`.  `actor-status` returns a read-only
dict with `/status`, `/mailbox-count`, `/mailbox-capacity`, `/has-joiner`,
`/trap-exit`, `/monitor-count`, and `/name` (`null` if unnamed).
`actor-try-send` never blocks: it returns `false` instead of waiting when
the target's mailbox is full or the target is dead.  `actor-broadcast` is
best-effort -- dead and full-mailbox targets are silently skipped; a
non-coroutine element raises `type-check`.

### 3.5 Control Operators (Internal)

| Operator | Popcount | Companion | Purpose |
| --- | --- | --- | --- |
| `@actor-send-retry` | 0 | `[target]` | Retry send after blocked on full mailbox |
| `@actor-recv-check` | 0 | (none) | Retry recv after woken from empty mailbox |
| `@actor-recv-timeout-check` | 0 | `[deadline-long]` | Retry recv-timeout, check deadline |
| `@actor-recv-match-check` | 0 | `[scan-index] [pred]` | Process pred result for selective receive |
| `@actor-recv-match-timeout-check` | 0 | `[deadline-long] [scan-index] [pred]` | Selective receive with deadline |

All control operators follow the coroutine trampoline pattern established
by `@pipe-put-retry` / `@pipe-get-retry`: push continuation state onto
the exec stack, flush, sleep, yield.  When woken, the control operator
fires, checks the condition, and either completes the operation or
re-sleeps.

---

## 4. Implementation Internals

### 4.1 Targeted Wakeup

Actors reuse the same targeted wakeup pattern proven in the pipeline
library and coroutine-join.  The pattern is four lines of C++:

```cpp
void coroutine_wake(vm_offset_t blocked_offset) {
    if (blocked_offset != nulloffset) {
        auto *ctx = offset_to_ptr<CoroutineContext>(blocked_offset);
        if (ctx->m_status == CoroutineContext::Sleeping) {
            if (!(ctx->m_flags & CoroutineContext::FlagBlocked)) {
                timer_list_remove(ctx);  // only if it was in a timed sleep
            }
            ctx->m_flags &= ~CoroutineContext::FlagBlocked;
            ctx->m_status = CoroutineContext::Ready;
            ready_queue_push(ctx);
        }
    }
}
```

Clearing `FlagBlocked`, setting `m_status = Ready`, and pushing the
coroutine onto the two-tier ready queue makes it immediately eligible for
scheduling.  It is popped O(1) by `ready_queue_pop()` on the next
scheduler pass.

**Reader wakeup** (recv side): The mailbox stores `m_blocked_reader`, the
offset of the owning actor when it's sleeping on `actor-recv`.  When
`actor-send` delivers a message, it calls `coroutine_wake(m_blocked_reader)`
and clears the field.  Always single reader (the owning actor).

**Sender wakeup** (send side): The mailbox stores `m_blocked_sender`, the
head of a FIFO linked list of senders blocked on a full mailbox.  When
`actor-recv` drains a message, it wakes the head sender and advances
`m_blocked_sender` to that sender's `m_blocked_sender_next`.

Additional senders that block while `m_blocked_sender` is occupied are
appended to the tail of the list; each sleeps with `wake_time_ns =
UINT64_MAX` and receives a targeted wakeup when its turn comes.  The
`m_blocked_sender_next` field threads the waiter queue — there is no
polling, and no separate data structure beyond that one field.

### 4.2 Send Trampoline (`actor-send` + `@actor-send-retry`)

When the mailbox is full:

```
actor-send:
  1. Pop target from op stack (save on exec stack as companion)
     Op stack:  [message]
     Exec stack: [...] [target] [@actor-send-retry]
  2. Register as m_blocked_sender (if slot available)
  3. Flush, sleep (UINT64_MAX -- targeted wakeup only), yield

@actor-send-retry (woken by receiver draining a slot):
  1. Read target from exec companion
  2. If target dead: pop companion, pop message, return (silent drop)
  3. Clear m_blocked_sender if it was us
  4. If space available: pop companion, pop message, complete send
  5. If still full: push new @retry, re-sleep
```

Key detail: the target is popped from the op stack before sleeping and
saved on the exec stack.  The message remains on the op stack.  This
ensures the retry can access both correctly.

### 4.3 Recv Trampoline (`actor-recv` + `@actor-recv-check`)

When the mailbox is empty:

```
actor-recv:
  1. Set m_blocked_reader = self
  2. Push @actor-recv-check on exec stack
  3. Flush, sleep (UINT64_MAX), yield

@actor-recv-check (woken by sender delivering a message):
  1. Clear m_blocked_reader
  2. If messages available: deliver oldest, wake blocked sender
  3. If still empty: re-sleep with new @recv-check
```

### 4.4 Timeout Trampoline (`actor-recv-timeout` + `@actor-recv-timeout-check`)

```
actor-recv-timeout:
  1. Compute deadline_ns = now + timeout_ms * 1,000,000
  2. Push [deadline-long] [@actor-recv-timeout-check] on exec stack
  3. Set m_blocked_reader = self
  4. Flush, sleep (wake_time = deadline_ns), yield

@actor-recv-timeout-check:
  1. Read deadline from exec companion (Long ExtValue)
  2. Clear m_blocked_reader
  3. If messages available: free deadline ExtValue, deliver, push true
  4. Else if now >= deadline: free deadline ExtValue, push null, push false
  5. Else: re-sleep until deadline
```

The deadline is stored as a Long on the exec stack because it must survive
across yield/resume cycles.  The Long uses an ExtValue (8 bytes on heap),
which is freed via `maybe_free_extvalue()` when the timeout completes or
expires.

### 4.5 Selective Receive Trampoline (`actor-recv-match` + `@actor-recv-match-check`)

The most complex trampoline.  Must execute an arbitrary Trix predicate for
each candidate message, which requires returning to the interpreter loop
between candidates.

**Exec stack companions**: `[scan-index] [pred]`

**State machine** (scan-index field):
- `-1`: Sentinel -- actor was sleeping, just woke up.  Start fresh scan.
- `>= 0`: Predicate just returned a bool for the message at this index.

```
actor-recv-match:
  1. Pop pred from op stack
  2. Push companions [scan-index=0] [pred] on exec stack
  3. If mailbox empty: set scan-index to -1 (sentinel), sleep
  4. If messages exist: call recv_match_eval_candidate(index=0)

recv_match_eval_candidate(scan_idx):
  1. Update scan-index companion to scan_idx
  2. Push [@check] then [pred] on exec stack (pred executes first)
  3. Push candidate message on op stack
  4. Return to interpreter -- pred runs, consumes message, pushes bool

@actor-recv-match-check:
  Case A (scan_idx == -1, woken from sleep):
    1. Clear m_blocked_reader
    2. If messages: eval candidate at index 0
    3. If still empty: re-sleep

  Case B (scan_idx >= 0, pred returned bool):
    1. Pop bool from op stack
    2. If true: remove message at scan_idx, push it, pop companions, done
    3. If false: advance to scan_idx+1
       a. If more messages: eval next candidate
       b. If exhausted: set sentinel, sleep
```

The removal of a matched message uses `mailbox_remove_at()`, which shifts
subsequent messages down.  This is O(n) in mailbox occupancy but bounded
by mailbox capacity (default 64).

### 4.6 Actor Death Cleanup

When a coroutine dies (via `@coroutine-complete`, `coroutine-die`, or
unhandled error), `coroutine_kill()` calls `coroutine_cleanup_mailbox()`:

```cpp
void coroutine_cleanup_mailbox(CoroutineContext *ctx) {
    if (ctx->m_mailbox == nulloffset) return;  // not an actor
    auto *mbx = offset_to_ptr<MailboxHeader>(ctx->m_mailbox);

    // Free ExtValues in queued messages
    auto *data = reinterpret_cast<Object *>(mbx + 1);
    auto idx = mbx->m_head;
    for (length_t i = 0; i < mbx->m_count; ++i) {
        data[idx].maybe_free_extvalue(trx);
        idx = (idx + 1) % mbx->m_capacity;
    }

    // Wake every blocked sender (walk the FIFO list; each retries,
    // finds the target dead, and silent-drops)
    auto blocked = mbx->m_blocked_sender;
    while (blocked != nulloffset) {
        auto *sender = offset_to_ptr<CoroutineContext>(blocked);
        auto next = sender->m_blocked_sender_next;
        sender->m_blocked_sender_next = nulloffset;
        if (sender->m_status == CoroutineContext::Sleeping) {
            sender->m_flags &= ~CoroutineContext::FlagBlocked;
            sender->m_status = CoroutineContext::Ready;
            ready_queue_push(sender);
        }
        blocked = next;
    }
    mbx->m_blocked_sender = nulloffset;

    // Remember capacity for post-death queries, then recycle the block
    // to the class-bucketed mailbox pool and clear the field
    ctx->m_last_mailbox_capacity = mbx->m_capacity;
    mailbox_recycle(trx, mbx, ctx->m_mailbox);
    ctx->m_mailbox = nulloffset;
}
```

This frees ExtValue objects (Long, ULong, Double, Address) in queued
messages, then walks the FIFO blocked-sender list and wakes every sender
(each retries `actor-send`, discovers the target is dead, and silently
drops).  Finally it records the capacity (so post-death `actor-status` /
`actor-mailbox-capacity` queries still report it) and recycles the block.

On death the mailbox block is returned to a class-bucketed free-list pool
(`m_mailbox_pool`, size classes {8, 16, 32, 64, 128, 256}) for reuse by the
next actor-spawn of the same class.  GC drains the pool at GC entry, but the
pool — not GC — is the per-death reclaim path.

### 4.7 Scheduler Fix

An early O(n) round-robin scheduler walked the circular coroutine list
starting from `m_running_coroutine`.  When the running coroutine died (via
`@coroutine-complete`) and was unlinked from the list by
`coroutine_unlink()`, the walk start was no longer in the list, causing an
infinite loop — a bug that manifested when an actor died while the main
context was sleeping (e.g., `50 coroutine-sleep`).

The current `coroutine_schedule()` no longer walks the list.  It sweeps
expired timers, then `ready_queue_pop()` (O(1)) to select the next
coroutine, falling back to a `poll()`/timer wait when the ready queue is
empty.  Because selection comes from the explicit ready queue rather than a
circular-list walk, the old failure mode no longer applies.

---

## 5. Usage Patterns

### 5.1 Fire-and-Forget

The simplest pattern: send a message and don't wait for a response.

```
/logger [ {
    { actor-recv = } loop
} actor-spawn def

(starting up) logger actor-send
(processing item 1) logger actor-send
(shutting down) logger actor-send
0 1 29 { pop 0 coroutine-sleep } for   % drain the scheduler
```

Cost: one `actor-send` per message (no blocking unless mailbox full).
No reply overhead.  Ideal for logging, event recording, metric collection.

### 5.2 Request/Response

Send a message that includes a reply-to address.  The server processes
the request and sends the result back.

```
% Server: doubles the value and sends back to the reply-to actor
/doubler [ {
    actor-recv dup
    /value get 2 mul
    exch /reply-to get actor-send
} actor-spawn def

% Client: only an actor has a mailbox, so the request side runs inside an
% actor and passes its own address (actor-self) as the reply-to.
mark {
    << /value 21 /reply-to actor-self >> doubler actor-send
    actor-recv =   % -> 42
} actor-spawn coroutine-join pop
```

Cost: two `actor-send` + one `actor-recv` per round-trip.  The client
blocks on `actor-recv` until the server responds.  Uses dict messages for
structured data.  The reply-to address must belong to an actor (the main
coroutine has no mailbox), so the client is itself spawned as an actor.

### 5.3 Worker Pool

Distribute work across N actors for concurrent processing.

```
% Worker: receives job, executes task, sends result back
/make-worker {
    [ {
        actor-recv dup
        /task get exec       % execute the task
        exch /reply-to get   % get reply-to
        actor-send           % send result
    } actor-spawn
} def

% The dispatcher runs as an actor so it can receive the results.
mark {
    % Spawn 4 workers
    /w0 make-worker def
    /w1 make-worker def
    /w2 make-worker def
    /w3 make-worker def

    % Distribute jobs round-robin (reply-to = this dispatcher actor)
    << /task { 10 20 add } /reply-to actor-self >> w0 actor-send
    << /task { 30 40 add } /reply-to actor-self >> w1 actor-send
    actor-recv =   % -> 30
    actor-recv =   % -> 70
} actor-spawn coroutine-join pop
```

Cost: ~5,240 bytes per worker.  4 workers = ~21 KB.  Each worker processes
one job at a time.  The mailbox provides natural load buffering.  The
dispatcher itself is an actor so the results have somewhere to be received;
`actor-recv` blocks until each result arrives (no explicit drain needed).

### 5.4 Publish/Subscribe

A broker actor maintains a subscriber list and forwards published
messages.

```
/subscribers [ ] def

/broker [ {
    {
        actor-recv dup type /dict-type eq {
            /type get /subscribe eq {
                % Add subscriber (simplified: uses shared array)
            } if
        } {
            % Forward to all subscribers
            subscribers { over actor-send } for-all
            pop
        } if-else
    } loop
} actor-spawn def
```

Cost: one `actor-send` per subscriber per published message.  The broker
serializes all message distribution.  For N subscribers and M messages:
O(N * M) sends total.

### 5.5 Finite State Machine

Each state is a proc.  Transitions happen by calling the next state's
proc.  Messages drive transitions.

```
% Each state is a named proc.  A transition is just a call to the next
% state's proc by name (Trix has no `self`; bind the states to names).
/light-green {
    (GREEN) =
    30 actor-recv-timeout {
        dup /stop eq { pop } {
            /emergency eq { light-red } { light-yellow } if-else
        } if-else
    } { pop light-yellow } if-else        % timed out: age to YELLOW
} def
/light-yellow {
    (YELLOW) =
    30 coroutine-sleep
    light-red
} def
/light-red {
    (RED) =
    30 coroutine-sleep
    light-green
} def

/traffic-light [ { light-green } actor-spawn def

% Cycle on timeouts, interrupt with /emergency, then halt with /stop.
0 1 60 { pop 1 coroutine-sleep } for
/emergency traffic-light actor-send
0 1 30 { pop 1 coroutine-sleep } for
/stop traffic-light actor-send
0 1 60 { pop 1 coroutine-sleep } for
```

`actor-recv-timeout` enables time-driven transitions with message-driven
overrides.  The `/emergency` message can interrupt the green phase; `/stop`
halts the light.  (The timeouts are scaled down to milliseconds so the demo
terminates quickly; a real light would use longer phases.)

### 5.6 Ring Pipeline (Actor-Based)

Actors can form a pipeline by forwarding messages.  Less efficient than
the dedicated pipeline library but supports arbitrary topologies.

```
% Terminal: count messages
/terminal [ {
    0 1 1 1000 { pop actor-recv pop 1 add } for =
} actor-spawn def

% Forwarder: recv and forward to next
/fwd [ terminal {
    1 1 1000 { pop actor-recv over actor-send } for pop
} actor-spawn def

% Inject
1 1 1000 { fwd actor-send } for
```

Tested with 5 stages, 1000 messages, default capacity-64 mailboxes.
Messages traverse the ring with backpressure at each stage.

### 5.7 Selective Receive (Priority Messages)

`actor-recv-match` enables processing urgent messages before normal ones.

<!-- doctest: skip (actor-body sketch; spawn-and-bind fragment with an infinite recv loop) -->
```
[ {
    {
        % Check for urgent messages first
        { dup type /dict-type eq { /priority get /urgent eq } { pop false } if-else }
        actor-recv-match
        (URGENT: ) print /payload get =
    } loop
} actor-spawn def
```

Non-matching messages remain in the queue for later processing.  The
predicate is any executable Trix proc -- it can inspect message type,
fields, tags, or any other property.

---

## 6. Design Decisions

### 6.1 Why Not Channels?

Go-style channels are standalone objects that any coroutine can send to
or receive from.  Actors tie the queue to the coroutine.

Actors were chosen because:
- **Simpler model**: the address (coroutine handle) IS the communication
  endpoint.  No separate channel object to manage, share, or close.
- **Natural fit**: coroutine handles are already first-class Objects.
- **No new types**: a channel would require a new Type slot (only 6
  remaining).  Actors reuse the existing Coroutine type.
- **Erlang validation**: the actor model has 40+ years of production
  experience for concurrent systems.

Channels can be emulated on top of actors (an actor that forwards messages
between any sender and any receiver).

### 6.2 Why Bounded Mailboxes?

Unbounded mailboxes can cause VM memory exhaustion if a producer outpaces
a consumer.  In a 256 KB VM, this can happen quickly.

Bounded mailboxes provide:
- **Automatic backpressure**: fast senders block, naturally throttling
- **Predictable memory**: capacity * 8 bytes, known at spawn time
- **Tunable**: `actor-spawn-capacity` for special cases

### 6.3 Why Silent Drop on Send to Dead Actor?

Alternatives considered:

| Approach           | Pro                      | Con                                              |
| ------------------ | ------------------------ | ------------------------------------------------ |
| **Error**          | Debuggable, catches bugs | Every sender needs try-catch; cascading failures |
| **Silent drop**    | Fault-tolerant, simple   | Lost messages are invisible                      |
| **Boolean return** | Flexible                 | `pop` after every send when you don't care       |

Silent drop was chosen because:
- Layer 3 (Supervision) makes actor death normal, not exceptional
- Monitoring is the correct death-notification mechanism
- Try-catch around every send defeats the actor model's fault isolation
- Erlang has validated this approach for decades

### 6.4 Why a Linked-List Waiter Queue?

The mailbox stores `m_blocked_sender`, the head of a FIFO linked list
threaded through `CoroutineContext::m_blocked_sender_next`.  Every blocked
sender gets a real targeted wakeup — there is no polling fallback.

| Approach              | Common case (1 sender) | Rare case (N senders) | Complexity |
| --------------------- | ---------------------- | --------------------- | ---------- |
| Single-slot + polling | O(1) targeted          | O(N) polling          | Low        |
| Linked-list queue     | O(1) targeted          | O(1) targeted         | Low        |
| Pure polling          | O(coroutines)          | O(coroutines)         | Trivial    |

The linked-list queue was chosen because:
- It reuses one existing CoroutineContext field (`m_blocked_sender_next`),
  so the cost over a single slot is one offset per blocked sender
- Both the common case (one blocked sender) and the rare case (N senders)
  are O(1) targeted wakeups — no polling, no scheduler churn
- On actor death or flush the whole list is woken in a single pass

### 6.5 Why Single Contiguous Allocation?

Both `MailboxHeader` and `PipeBufferHeader` use a single contiguous
allocation: one `gvm_alloc` of `sizeof(header) + physical * sizeof(Object)`,
with the data array embedded just past the header.

Single allocation provides:
- Better cache locality (header and data adjacent)
- One fewer `vm_alloc` call
- Less heap fragmentation
- Simpler cleanup (one offset to track)

The tradeoff (can't resize data independently) doesn't apply because
mailboxes are bounded and never resized.

### 6.6 Why `actor-*` Prefix?

Consistent with `coroutine-*` and `pipe-*`.  Keeps the namespace clean
and self-documenting.  `actor-send` is unambiguous; `send` could conflict
with future systems.

---

## 7. Constraints and Interactions

### 7.1 Save/Restore

VM Redux Phase 3 moved actor mailboxes to global VM
(`ChunkKind::Mailbox`); Phase 5 moved `CoroutineContext` and
`CoroutineStacks` there as well.  The result: an actor spawned inside
a save scope **survives** `restore` -- its mailbox content, blocked
reader/sender offsets, and scheduling state are all in journal-skipped
global memory.  Mailbox writes have never been journaled and they
still aren't; what changed is that the mailbox block itself no longer
gets reclaimed by rollback.

The remaining `save`/`restore` interaction is the standard
suspended-coroutine check: if a suspended actor's operand /
exec / error / dict stack holds a composite Object
allocated in the **local** VM above the save barrier,
`restore` raises `/invalid-restore`.  Scalar values and global-VM
Objects on the stack are always safe.  See `coroutines.md` § 7.5 for
the full rule.

Supervision (Layer 3) remains the fault-recovery mechanism for
actors -- crashes propagate through monitor / link relationships
(now also in global VM since Phase 4) rather than through `restore`.

### 7.2 Snapshot/Thaw

The mailbox lives in global VM and uses `vm_offset_t` references.
Snap-shot captures both VM regions as byte blobs, including all
MailboxHeaders and their data arrays.  The `m_mailbox` field is inside
CoroutineContext, also in global VM.

After thaw:
- Actor status is preserved (Sleeping, Ready, Dead)
- Mailbox contents are preserved (messages, head/tail/count)
- Blocked reader/sender offsets are preserved
- Sleeping actors with `wake_time_ns = UINT64_MAX` remain blocked until
  explicitly woken by a new `actor-send`
- Sleeping actors with time-based deadlines resume when the scheduler
  detects the deadline has passed

No SnapShotHeader changes were needed.

### 7.3 Coroutine-Join

`coroutine-join` works unchanged on actors.  The joiner sleeps until the
actor dies, then receives the return value (top of actor's operand stack).
The mailbox is cleaned up before join completes.

### 7.4 Pipelines

Actors and pipelines are independent systems that share the coroutine
scheduler.  An actor can create and run pipelines.  A pipeline stage
cannot be an actor (pipeline stages are created by pipe-map/filter/etc.,
not by actor-spawn).

### 7.5 Coroutine-Kill

`coroutine-kill` on an actor:
1. Captures return value from operand stack
2. Cleans up stacks (frees ExtValues)
3. Cleans up mailbox (frees ExtValues in queued messages, wakes blocked sender)
4. Frees stacks block to free list
5. Sets status to Dead, decrements live count
6. Wakes joiner if any

The blocked sender, once woken, retries `actor-send`, discovers the
target is Dead, and silently drops.

### 7.6 Composability Patterns

The six composability features -- protocols, closures, contracts, pattern
matching, transducers, and GenServer -- compose with the actor system.

#### 7.6.1 Protocol Dispatch in Handlers

An actor recv loop can dispatch on message type via a protocol method.
The protocol resolves the correct implementation based on the message's
type:

```
[/process] /Processable def-protocol
{ 2 mul } /process /integer-type def-method
{ length } /process /string-type def-method

/R null def
/W [ {
    actor-recv process /R exch def
} actor-spawn def
21 W actor-send
0 1 29 { pop 0 coroutine-sleep } for % drain the scheduler
R =                                  % -> 42
```

The actor receives a message and calls `process`, which dispatches to
the integer or string implementation based on the message type.

#### 7.6.2 Cell Watcher to Actor Bridge

A `watch` callback can bridge reactive cells to the actor system.  When
the cell changes, the watcher sends the new value to an actor:

```
/R null def
/W [ {
    actor-recv /R exch def
} actor-spawn def

/c 0 cell def
c { exch pop W actor-send } watch    % on change, send new value to actor
10 c cell-set
0 1 29 { pop 0 coroutine-sleep } for % drain the scheduler
R =                                  % -> 10
```

The watcher proc receives `old new` on the stack.  `exch pop` discards
the old value, then `actor-send` delivers the new value to the actor.

#### 7.6.3 Transducer Batch Processing

An actor can accumulate messages into an array, then process the batch
with a composed transducer:

```
/R null def
/W [ {
    [ actor-recv actor-recv actor-recv actor-recv actor-recv ]
    { 2 mod 0 eq } xf-filter { 10 mul } xf-map xf-compose
    into
    /R exch def
} actor-spawn def
1 W actor-send  2 W actor-send  3 W actor-send
4 W actor-send  5 W actor-send
0 1 29 { pop 0 coroutine-sleep } for % drain the scheduler
R                                    % => [20 40]
```

The actor blocks on `actor-recv` until all five messages arrive, collects
them into an array, then filters for even values and scales by 10 in a
single transducer pass.

#### 7.6.4 Closures + Contracts in Handlers

Capture validation bounds in a closure, then use the closure with
`precondition` to validate incoming messages:

```
/min-val 0 def  /max-val 100 def
{ dup min-val ge exch max-val le and } [/min-val /max-val] closure-capture
/in-range exch def

/R null def
/W [ {
    actor-recv
    dup { in-range } exec precondition    % validate: 0 <= msg <= 100
    /R exch def
} actor-spawn def
42 W actor-send
0 1 29 { pop 0 coroutine-sleep } for % drain the scheduler
R =                                  % -> 42
```

The closure freezes the bounds at capture time.  The `precondition`
raises `/require` if validation fails, crashing the actor.  A supervisor
can then restart it (see supervision.md section 9.5.3).

---

## 8. Error Catalog

| Error | Operator | Cause | Fix |
| --- | --- | --- | --- |
| `type-check` | `actor-send` | Target is not a coroutine | Check target type; use `is-coroutine` |
| `type-check` | `actor-spawn` | Proc is not executable | Ensure proc is `{ }`, packed, or curry |
| `type-check` | `actor-spawn-capacity` | Capacity is not an integer | Pass integer capacity |
| `type-check` | `actor-recv-match` | Predicate is not executable | Ensure pred is `{ }`, packed, or curry |
| `invalid-access` | `actor-send` | Target coroutine has no mailbox | Use `is-actor` to check; use `actor-spawn` |
| `invalid-access` | `actor-recv` | Current coroutine has no mailbox | Only call from inside an actor |
| `invalid-access` | `actor-recv-timeout` | Current coroutine has no mailbox | Only call from inside an actor |
| `invalid-access` | `actor-recv-match` | Current coroutine has no mailbox | Only call from inside an actor |
| `invalid-access` | `actor-mailbox-count` | Target coroutine has no mailbox | Use `is-actor` to check |
| `invalid-access` | `actor-mailbox-empty?` | Target coroutine has no mailbox | Use `is-actor` to check |
| `range-check` | `actor-spawn-capacity` | Capacity is zero or negative | Use a positive integer |
| `unmatched-mark` | `actor-spawn` | No mark on operand stack | Ensure `[` before arguments |
| `vm-full` | `actor-spawn` | Insufficient heap space | Reduce VM population or increase VM size |
| `opstack-overflow` | `actor-spawn` | Too many parameters | Reduce parameter count (max 128) |

---

## 9. File Map

| File | Content |
| --- | --- |
| `src/types.inl` | `MaxActorMailboxCapacity` constant |
| `src/enums.inl` | 19 `actor-*` + `is-actor` standard + 5 control SystemName entries |
| `src/object.inl` | `make_control_operator` cases for 5 actor control ops |
| `src/ops_coroutine.inl` | MailboxHeader struct, m_mailbox field, `coroutine_cleanup_mailbox()`, `coroutine_launch_common()`, scheduler dead-start fix |
| `src/ops_actor.inl` | 17 `actor-*` + `is-actor` standard + 5 control operator implementations + helpers (`actor-exit`/`actor-trap-exit` implemented in `ops_supervision.inl`) |
| `src/ops_pipeline.inl` | m_mailbox initialization in stage/source creation |
| `src/dispatch.inl` | Dispatch entries for 19 `actor-*` + `is-actor` standard + 5 control operators |
| `src/init.inl` | m_mailbox initialization for main context |
| `trix.h` | `#include "src/ops_actor.inl"` |
| `tests/test_actor.trx` | 147-assertion test suite (77 sections) |
