//===----------------------------------------------------------------------===//
//                                                                            //
//    ______    _                                                             //
//   /_  __/___(_)_  __                                                       //
//    / / / __/ /\ \/ /       Stack-Based Interpreter & VM                    //
//   / / / / / /  > · <      C++23 · Single-Header Library                    //
//  /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli                    //
//                                                                            //
// Licensed under the Apache License, Version 2.0 (the "License");            //
// you may not use this file except in compliance with the License.           //
// You may obtain a copy of the License at                                    //
//                                                                            //
//     https://www.apache.org/licenses/LICENSE-2.0                            //
//                                                                            //
// Unless required by applicable law or agreed to in writing, software        //
// distributed under the License is distributed on an "AS IS" BASIS,          //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   //
// See the License for the specific language governing permissions and        //
// limitations under the License.                                             //
//                                                                            //
//===----------------------------------------------------------------------===//

//===--- Coroutine Support ---===//
//
// Implements cooperative (non-preemptive) coroutines for concurrent execution
// within a single OS thread.  This is the foundation layer for the entire
// concurrency stack -- pipelines, actors, and supervision all build on it.
//
// Based on the coroutine models from:
//
//   Lua 5.x coroutines (Roberto Ierusalimschy, "Revisiting Coroutines",
//   2004): asymmetric coroutines with explicit yield, single-threaded,
//   shared-nothing between coroutines except by explicit transfer.
//
//   Erlang processes (lightweight, isolated, cooperative scheduling):
//   the isolation model (per-process stacks, shared heap) and the
//   scheduler design (round-robin walk of a process list).
//
// The specific design is closest to Erlang's lightweight processes: each
// coroutine has isolated stacks but shares the VM heap.  Unlike Lua's
// asymmetric coroutines (caller/callee relationship), Trix coroutines are
// symmetric -- any coroutine can yield and the scheduler picks the next
// runnable one.
//
// --- Core concepts for maintainers ---
//
// COROUTINE CONTEXT
//   Each coroutine is represented by a CoroutineContext struct on the VM
//   heap.  It stores the coroutine's saved state: four stack pointer sets
//   (operand, execution, error, dictionary), scanner stream, status,
//   return value, and scheduling metadata (wake time, join handle).
//
//   CoroutineContext (static_assert <= 232):
//   +-------------------------------------------+
//   | m_status (uint8_t)     Created/Sleeping/   |
//   | m_has_return (bool)    Ready/Running/Dead  |
//   | m_activation_sl        save level at last  |
//   |                        context switch      |
//   | m_trap_exit (bool)     supervision flag    |
//   | m_flags (uint8_t)      Suspended|Blocked|  |
//   |                        WasActor|Supervisor |
//   +-------------------------------------------+
//   | m_context_offset       self-offset         |
//   | m_wake_time_ns (u64)   timer expiry        |
//   | m_suspend_remaining_ns preserved delta     |
//   | m_next (vm_offset_t)   registry list link  |
//   | m_ready_next/prev      ready queue links   |
//   | m_timer_next           timer list link     |
//   | m_id (uint32_t)        sequential ID       |
//   | m_quantum (uint32_t)   ops per time slice  |
//   | m_ops_remaining        quantum countdown   |
//   | m_priority (uint8_t)   0=normal, 1=high    |
//   | m_blocked_sender_next  mailbox backpressure|
//   | m_last_run_time_ns     starvation tracking |
//   | m_joiner               waiting coroutine   |
//   | m_return_value (Object) captured on death  |
//   +-------------------------------------------+
//   | m_stacks_offset        -> stack block      |
//   | m_op_base/ptr/limit    operand stack ptrs  |
//   | m_exec_base/ptr/limit  execution stack     |
//   | m_err_base/ptr/limit   error stack         |
//   | m_dict_base/ptr/limit  dictionary stack    |
//   +-------------------------------------------+
//   | m_scanner_stream (ptr) per-coroutine scan  |
//   | m_mailbox (vm_offset_t) actor mailbox      |
//   | m_monitors             who watches me      |
//   | m_monitoring           who I watch         |
//   | m_exit_reason (Object) death reason        |
//   | m_debug_name           optional debug name |
//   +-------------------------------------------+
//
//   Stack block (contiguous, from free list or vm_alloc):
//   +---------+----------+-------+------------+---------+
//   | operand | execution| error | dictionary | scratch |
//   | [128]   | [256]    | [16]  | [32]       | [128]   |
//   +---------+----------+-------+------------+---------+
//   The first four are DefaultCoroutine{Operand,Execution,Error,Dictionary}Depth
//   Objects; scratch is m_scratch_depth (default DefaultCoroutineScratchDepth).
//
//   The "running" coroutine's state lives in the Trix member variables
//   (m_op_ptr, m_exec_ptr, etc.) for fast access.  On context switch,
//   coroutine_flush_running() saves the member vars into the context,
//   and coroutine_load() restores a new context into the member vars.
//
// STACK ISOLATION
//   Each coroutine has its own operand, execution, error, and dictionary
//   stacks, allocated as a contiguous block on the VM heap.  This provides
//   full isolation -- one coroutine cannot corrupt another's stacks.
//
//   The VM heap itself is shared.  Objects on the heap (arrays, strings,
//   dicts, etc.) can be referenced by any coroutine.  This is safe because
//   Trix is single-threaded -- only one coroutine runs at a time.
//
// SCHEDULING
//   The scheduler pops the next runnable coroutine from two two-tier
//   doubly-linked FIFO ready queues (high-priority drained before normal)
//   and sweeps a sorted timer list for expired sleepers.  It is O(1) for
//   the common case (ready queue non-empty), falling back to a timer
//   sweep and then a real-thread park (poll(stdin) when a coroutine is
//   blocked in read-key-byte, otherwise nanosleep) until the next wake.
//
//   Coroutines yield cooperatively via:
//     - coroutine-yield (explicit yield)
//     - coroutine-sleep (yield with timer)
//     - Blocking operations (pipe-put/get, actor-send/recv, join)
//   Each of these calls coroutine_flush_running() to save state, then
//   coroutine_schedule() to pick the next coroutine.
//
//   The circular m_next registry list still links all live coroutines
//   (used by GC and coroutine enumeration, e.g. coroutine-by-id), but is
//   no longer the scheduling
//   mechanism.
//
// LIFECYCLE
//   Created  -> Ready (on launch)
//   Ready    -> Running (when scheduled)
//   Running  -> Ready (on yield)
//   Running  -> Sleeping (on sleep/block)
//   Sleeping -> Ready (on wake/timer expiry)
//   Running  -> Dead (on die/kill/completion)
//
//   Dead coroutines are unlinked from the scheduler list by
//   coroutine_unlink().  Their stack blocks (coroutine_free_stacks) and
//   context structs (coroutine_free_context) are returned to free lists
//   for reuse by future coroutine launches.
//
// COROUTINE LAUNCH
//   coroutine-launch / actor-spawn create a new CoroutineContext via
//   coroutine_launch_common():
//     1. Validate the proc argument (must be executable).
//     2. Find the mark and count parameters above it.
//     3. Allocate context + stack block (from free list or VM heap).
//     4. Initialize four stacks: copy parameters to operand stack, push
//        @coroutine-complete + proc onto execution stack, push systemdict
//        + protocoldict + userdict onto dictionary stack.
//     5. Insert into the circular scheduler list.
//   The new coroutine starts in Ready state and will run when scheduled.
//
// TARGETED WAKEUP
//   When a coroutine blocks (e.g., waiting for a pipe item or actor
//   message), it stores its context offset in the blocking structure
//   (PipeBufferHeader, MailboxHeader, or joiner field).  The producer/
//   sender/dying coroutine wakes it directly by setting its status to
//   Ready -- O(1), no scheduler involvement.
//
// RESOURCE MANAGEMENT
//   Stack blocks and CoroutineContext structs are pooled via free lists
//   (m_coroutine_stack_free, m_coroutine_ctx_free).  This amortizes
//   allocation cost when coroutines are frequently created and destroyed.
//   The free lists are singly-linked via the first word of each block.
//
// --- Operators ---
//
//   coroutine-launch   mark obj* proc -- coroutine  Launch a coroutine
//   coroutine-yield    --                           Yield to scheduler
//   coroutine-sleep    int --                       Sleep for N milliseconds
//   coroutine-die      --                           Kill current coroutine
//   coroutine-kill     coroutine --                 Kill another coroutine
//   coroutine-kill-all --                           Kill all other coroutines
//   coroutine-join     coroutine -- value bool      Wait for completion
//   coroutine-await    coroutine -- value           Await completion; push value or rethrow on death
//   coroutine-wait-all --                           Wait for all to complete
//   coroutine-suspend  coroutine --                 Pause a coroutine
//   coroutine-resume   coroutine --                 Resume a suspended coroutine
//   coroutine-release  coroutine --                 Free dead coroutine's resources
//   coroutine-status   coroutine -- name            Query status
//   coroutine-self     -- coroutine                 Push current handle
//   coroutine-id       coroutine -- int             Sequential ID of a coroutine
//   coroutine-by-id    int -- coroutine             Look up coroutine by ID
//   coroutine-quantum  int --                       Set the running coroutine's op quantum
//   coroutine-priority coroutine int --             Set scheduling priority
//   coroutine-count    -- int                       Number of live coroutines
//   coroutine-last-error -- name                    Last joined exit reason
//   is-coroutine       any -- bool                  Type test
//
//   (Authoritative operator registry is dispatch.inl.)
//
// Control operators (internal, not user-visible):
//   @coroutine-complete      Coroutine proc finished: capture return, die
//   @coroutine-join-check    Join target still alive: re-sleep
//   @coroutine-await-check   Await target still alive: re-sleep
//   @coroutine-wait-all-check  Others still alive: re-sleep
//

struct CoroutineContext {
    static constexpr uint8_t Created = 0;
    static constexpr uint8_t Sleeping = 1;
    static constexpr uint8_t Ready = 2;
    static constexpr uint8_t Running = 3;
    static constexpr uint8_t Dead = 4;

    // Flags (orthogonal to m_status)
    static constexpr uint8_t FlagSuspended = 0x01;   // explicitly paused by user
    static constexpr uint8_t FlagBlocked = 0x02;     // waiting on producer (pipe/actor/supervision/join)
    static constexpr uint8_t FlagWasActor = 0x04;    // was spawned as an actor (preserved past death for is-actor)
    static constexpr uint8_t FlagSupervisor = 0x08;  // owns a SupervisorState block; m_return_value holds its offset (GC root)

    // Priority levels (for two-tier ready queue)
    static constexpr uint8_t PriorityNormal = 0;
    static constexpr uint8_t PriorityHigh = 1;

    uint8_t m_status;
    bool m_has_return;
    save_level_t m_activation_sl;  // save level at last context switch (for cross-coroutine restore check)
    bool m_trap_exit;              // supervision: convert exit signals to messages
    uint8_t m_flags;               // FlagSuspended | FlagBlocked (orthogonal to m_status)
    vm_offset_t m_context_offset;  // self-offset (for restore barrier check)
    uint64_t m_wake_time_ns;
    uint64_t m_suspend_remaining_ns;    // preserved timer delta when suspended (ns)
    vm_offset_t m_next;                 // registry linked list (circular, all coroutines)
    vm_offset_t m_ready_next;           // ready queue forward link (nulloffset = not in queue)
    vm_offset_t m_ready_prev;           // ready queue backward link (nulloffset = head)
    vm_offset_t m_timer_next;           // timer list forward link (nulloffset = not in list)
    uint32_t m_id;                      // sequential coroutine ID (never reused)
    uint32_t m_quantum;                 // operations per time quantum (0 = unlimited, no auto-yield)
    uint32_t m_ops_remaining;           // operations left in current quantum slice
    uint8_t m_priority;                 // 0 = normal, 1 = high (for two-tier ready queue)
    vm_offset_t m_blocked_sender_next;  // linked list of senders blocked on a full mailbox
    uint64_t m_last_run_time_ns;        // monotonic timestamp of last context switch in (0 = never run)
    vm_offset_t m_joiner;
    Object m_return_value;

    vm_offset_t m_stacks_offset;  // offset to contiguous stack block (for free list)

    vm_offset_t m_op_base;
    vm_offset_t m_op_ptr;
    vm_offset_t m_op_limit;
    vm_offset_t m_op_high_water;

    vm_offset_t m_exec_base;
    vm_offset_t m_exec_ptr;
    vm_offset_t m_exec_limit;
    vm_offset_t m_exec_high_water;

    vm_offset_t m_err_base;
    vm_offset_t m_err_ptr;
    vm_offset_t m_err_limit;
    vm_offset_t m_err_high_water;

    vm_offset_t m_dict_base;
    vm_offset_t m_dict_ptr;
    vm_offset_t m_dict_limit;
    vm_offset_t m_dict_high_water;

    // Per-coroutine scratch arena, carved from the back of the contiguous
    // stack block.  Bump-and-collect storage for find-all / find-n /
    // aggregate.  Base/limit are immutable post-launch;
    // only m_scratch_ptr mutates.
    vm_offset_t m_scratch_base;
    vm_offset_t m_scratch_ptr;
    vm_offset_t m_scratch_limit;

    vm_offset_t m_debug_name;  // offset to Name for debug display (nulloffset = unnamed)

    Stream *m_scanner_stream;            // per-coroutine scanner stream state
    vm_offset_t m_mailbox;               // offset to MailboxHeader (nulloffset = no mailbox or recycled on death)
    vm_offset_t m_monitors;              // head of "who's watching me" monitor/link list (nulloffset = none)
    vm_offset_t m_monitoring;            // head of "who I'm watching" monitor/link list (nulloffset = none)
    vm_offset_t m_binding_table;         // offset to BindingBucketCount-sized bucket array (nulloffset = no bindings cached yet)
    Object m_exit_reason;                // exit reason (default /normal)
    Object m_last_joined_exit_reason;    // diagnostic cache: exit_reason of the most recently
                                         // joined/awaited/released coroutine, or /no-error if
                                         // none yet.  /normal is mapped to /no-error so the
                                         // joiner sees a single sentinel for "clean" outcomes.
                                         // Read by `coroutine-last-error -- /name`.
    length_t m_last_mailbox_capacity{};  // captured before mailbox recycle so actor-status on dead actors can still report it
    bool m_curr_alloc_global{false};     // saved Trix::m_curr_alloc_global at last context switch.
                                         // Without per-coroutine save/restore, A entering ${...}
                                         // and yielding leaks the global flag into B's allocations.
                                         // SnapShotHeader::curr_alloc_global is the process-wide
                                         // snapshot of "the running coroutine's flag at save time".
};
static_assert(sizeof(CoroutineContext) <= 232, "CoroutineContext size check");

//===--- MailboxHeader ---===//
// Per-actor message queue, allocated as a single contiguous block on the VM heap:
// [MailboxHeader (16 bytes)][Object[m_capacity]].
// Many-to-one: any coroutine can send, only the owning actor receives.
// Targeted wakeup for single reader; linked-list wakeup for blocked senders.
// Multiple senders can block on a full mailbox; each links through
// CoroutineContext::m_blocked_sender_next.

struct MailboxHeader {
    length_t m_head;               // read index (circular)
    length_t m_tail;               // write index (circular)
    length_t m_count;              // current occupancy
    length_t m_capacity;           // maximum messages
    vm_offset_t m_blocked_reader;  // actor waiting on recv (nulloffset = none)
    vm_offset_t m_blocked_sender;  // head of blocked sender list (nulloffset = none)
};
static_assert(sizeof(MailboxHeader) == 16, "MailboxHeader must be 16 bytes for Object alignment");

//===--- MonitorEntry ---===//
// Represents a monitor or link relationship between two coroutines.
// Each entry participates in two linked lists simultaneously:
//   - the target's m_monitors list ("who is watching me")
//   - the source's m_monitoring list ("who I am watching")
// Type bit: high bit of m_ref_id (0 = monitor, 1 = link).
// Allocated on the VM heap; reclaimed by save/restore or VM reset.

struct MonitorEntry {
    vm_offset_t m_next_target;  // next entry in target's m_monitors list
    vm_offset_t m_next_source;  // next entry in source's m_monitoring list
    vm_offset_t m_source;       // the monitoring coroutine
    vm_offset_t m_target;       // the monitored coroutine
    uint32_t m_ref_id;          // unique ID; high bit: 0=monitor, 1=link
    uint32_t m_padding;         // alignment to 8-byte boundary
};
static_assert(sizeof(MonitorEntry) == 24, "MonitorEntry must be 24 bytes");

static constexpr uint32_t MonitorTypeMask = 0x80000000u;  // high bit = link
static constexpr uint32_t MonitorRefMask = 0x7FFFFFFFu;   // low 31 bits = ref_id

// Save the live stack pointers (Trix member vars) into the running CoroutineContext.
void coroutine_flush_running() {
    auto ctx = offset_to_ptr<CoroutineContext>(m_running_coroutine);
    ctx->m_op_base = ptr_to_offset(m_op_base);
    ctx->m_op_ptr = ptr_to_offset(m_op_ptr + 1);  // convert one-past-empty to base-relative
    ctx->m_op_limit = ptr_to_offset(m_op_limit);
    ctx->m_op_high_water = ptr_to_offset(m_op_high_water_ptr);
    ctx->m_exec_base = ptr_to_offset(m_exec_base);
    ctx->m_exec_ptr = ptr_to_offset(m_exec_ptr + 1);
    ctx->m_exec_limit = ptr_to_offset(m_exec_limit);
    ctx->m_exec_high_water = ptr_to_offset(m_exec_high_water_ptr);
    ctx->m_err_base = ptr_to_offset(m_err_base);
    ctx->m_err_ptr = ptr_to_offset(m_err_ptr + 1);
    ctx->m_err_limit = ptr_to_offset(m_err_limit);
    ctx->m_err_high_water = ptr_to_offset(m_err_high_water_ptr);
    ctx->m_dict_base = ptr_to_offset(m_dict_base);
    ctx->m_dict_ptr = ptr_to_offset(m_dict_ptr + 1);
    ctx->m_dict_limit = ptr_to_offset(m_dict_limit);
    ctx->m_dict_high_water = ptr_to_offset(m_dict_high_water_ptr);
    // Scratch arena ptr.  Base/limit are immutable post-launch
    // and don't need re-saving here (the load path also re-fetches them
    // from ctx for symmetry with the other stack pointers).
    ctx->m_scratch_ptr = ptr_to_offset(m_scratch_ptr);
    // Save scanner stream state (nullptr is valid for "no active scanner")
    ctx->m_scanner_stream = m_scanner_stream;
    // Save quantum counter
    ctx->m_ops_remaining = m_coroutine_ops_remaining;
    // Save the global-allocation flag.  Without this, a coroutine that
    // yields mid-${...} leaks the flag into whichever coroutine the
    // scheduler runs next -- that other coroutine's allocations would
    // route to global VM unintentionally.
    ctx->m_curr_alloc_global = m_curr_alloc_global;
}

// Restore the Trix member vars from a CoroutineContext (make it the running coroutine).
void coroutine_load(CoroutineContext *ctx) {
    m_op_base = offset_to_ptr<Object>(ctx->m_op_base);
    m_op_ptr = offset_to_ptr<Object>(ctx->m_op_ptr) - 1;
    m_op_limit = offset_to_ptr<Object>(ctx->m_op_limit);
    m_op_push_limit = (m_op_limit - 1);
    m_op_high_water_ptr = offset_to_ptr<Object>(ctx->m_op_high_water);
    m_exec_base = offset_to_ptr<Object>(ctx->m_exec_base);
    m_exec_ptr = offset_to_ptr<Object>(ctx->m_exec_ptr) - 1;
    m_exec_limit = offset_to_ptr<Object>(ctx->m_exec_limit);
    m_exec_high_water_ptr = offset_to_ptr<Object>(ctx->m_exec_high_water);
    m_err_base = offset_to_ptr<Object>(ctx->m_err_base);
    m_err_ptr = offset_to_ptr<Object>(ctx->m_err_ptr) - 1;
    m_err_limit = offset_to_ptr<Object>(ctx->m_err_limit);
    m_err_high_water_ptr = offset_to_ptr<Object>(ctx->m_err_high_water);
    m_dict_base = offset_to_ptr<Object>(ctx->m_dict_base);
    m_dict_ptr = offset_to_ptr<Object>(ctx->m_dict_ptr) - 1;
    m_dict_limit = offset_to_ptr<Object>(ctx->m_dict_limit);
    m_dict_high_water_ptr = offset_to_ptr<Object>(ctx->m_dict_high_water);
    // Scratch arena live pointers.
    m_scratch_base = offset_to_ptr<Object>(ctx->m_scratch_base);
    m_scratch_limit = offset_to_ptr<Object>(ctx->m_scratch_limit);
    m_scratch_ptr = offset_to_ptr<Object>(ctx->m_scratch_ptr);
    ctx->m_status = CoroutineContext::Running;
    ctx->m_activation_sl = m_curr_save_level;
    ctx->m_last_run_time_ns = monotonic_ns();
    m_running_coroutine = ctx->m_context_offset;
    // Restore scanner stream state
    m_scanner_stream = ctx->m_scanner_stream;
    // Restore quantum counter
    m_coroutine_ops_remaining = ctx->m_ops_remaining;
    // Restore the global-allocation flag (per-coroutine isolation;
    // see CoroutineContext::m_curr_alloc_global rationale).
    m_curr_alloc_global = ctx->m_curr_alloc_global;
}

// Walk a coroutine's stacks and free any ExtValue objects, then clear name bindings
// for dicts above the base two (systemdict, userdict).
void coroutine_cleanup_stacks(CoroutineContext *ctx) {
    auto trx = this;

    // Free ExtValues on the operand stack
    auto op_base = offset_to_ptr<Object>(ctx->m_op_base);
    auto op_ptr = offset_to_ptr<Object>(ctx->m_op_ptr) - 1;
    for (auto obj = op_base; obj <= op_ptr; ++obj) {
        obj->maybe_free_extvalue(trx);
    }

    // Free ExtValues on the exec stack
    auto exec_base = offset_to_ptr<Object>(ctx->m_exec_base);
    auto exec_ptr = offset_to_ptr<Object>(ctx->m_exec_ptr) - 1;
    for (auto obj = exec_base; obj <= exec_ptr; ++obj) {
        obj->maybe_free_extvalue(trx);
    }

    // Free ExtValues on the error stack
    auto err_base = offset_to_ptr<Object>(ctx->m_err_base);
    auto err_ptr = offset_to_ptr<Object>(ctx->m_err_ptr) - 1;
    for (auto obj = err_base; obj <= err_ptr; ++obj) {
        obj->maybe_free_extvalue(trx);
    }

    // Clear name bindings on dicts the coroutine pushed itself, i.e. above the
    // permanent dicts (systemdict, protocoldict, userdict = PermanentDictCount).
    // Starting at dict_base+2 instead clears the SHARED userdict's bindings on
    // every coroutine death, tombstoning every live coroutine's binding cache.
    auto dict_base = offset_to_ptr<Object>(ctx->m_dict_base);
    auto dict_ptr = offset_to_ptr<Object>(ctx->m_dict_ptr) - 1;
    for (auto obj = dict_base + PermanentDictCount; obj <= dict_ptr; ++obj) {
        if (obj->is_dict()) {
            obj->dict_value(trx)->clear_name_bindings(trx);
        }
    }

    // Free ExtValues in the scratch arena (find-all/find-n in flight when
    // the coroutine was killed).  The slots between m_scratch_base and
    // m_scratch_ptr hold cloned results that own their ExtValue references.
    auto scratch_base = offset_to_ptr<Object>(ctx->m_scratch_base);
    auto scratch_ptr = offset_to_ptr<Object>(ctx->m_scratch_ptr);
    for (auto obj = scratch_base; obj < scratch_ptr; ++obj) {
        obj->maybe_free_extvalue(trx);
    }
}

// Walk a dead actor's mailbox and free any ExtValue objects in queued messages,
// wake any sender blocked on the full mailbox, then return the mailbox block
// to the per-save-level free-list pool for reuse.
void coroutine_cleanup_mailbox(CoroutineContext *ctx) {
    if (ctx->m_mailbox != nulloffset) {
        auto trx = this;
        auto mbx_offset = ctx->m_mailbox;
        auto mbx = offset_to_ptr<MailboxHeader>(mbx_offset);

        // Free ExtValues in queued messages
        auto data = reinterpret_cast<Object *>(mbx + 1);
        auto idx = mbx->m_head;
        for (length_t i = 0; i < mbx->m_count; ++i) {
            data[idx].maybe_free_extvalue(trx);
            idx = static_cast<length_t>((idx + 1) % mbx->m_capacity);
        }

        // Wake all blocked senders (they will retry, find dead target, and silent-drop)
        auto blocked = mbx->m_blocked_sender;
        while (blocked != nulloffset) {
            auto sender = offset_to_ptr<CoroutineContext>(blocked);
            auto next = sender->m_blocked_sender_next;
            sender->m_blocked_sender_next = nulloffset;
            if (sender->m_status == CoroutineContext::Sleeping) {
                sender->m_flags &= ~CoroutineContext::FlagBlocked;
                if (sender->m_flags & CoroutineContext::FlagSuspended) {
                    sender->m_status = CoroutineContext::Ready;
                } else {
                    sender->m_status = CoroutineContext::Ready;
                    ready_queue_push(sender);
                }
            }
            blocked = next;
        }
        mbx->m_blocked_sender = nulloffset;

        // Remember capacity so actor-status / actor-mailbox-capacity queries on
        // the dead actor can still report it after the mailbox is recycled.
        ctx->m_last_mailbox_capacity = mbx->m_capacity;

        // Return the block to the per-save-level mailbox pool (class-bucketed).
        mailbox_recycle(trx, mbx, mbx_offset);
        ctx->m_mailbox = nulloffset;
    }
}

// Return the stacks block to the free list.
void coroutine_free_stacks(CoroutineContext *ctx) {
    if (ctx->m_stacks_offset != nulloffset) {
        // Use the first vm_offset_t of the stacks block as a free list next pointer
        auto block = offset_to_ptr<vm_offset_t>(ctx->m_stacks_offset);
        *block = m_coroutine_stack_free;
        m_coroutine_stack_free = ctx->m_stacks_offset;
        ctx->m_stacks_offset = nulloffset;
    }
}

// Remove a dead coroutine from the scheduler's linked list.
void coroutine_unlink(CoroutineContext *ctx) {
    auto target = ctx->m_context_offset;
    if (target == m_coroutine_head) {
        // Skip if it's main (main is never unlinked)
        if (target != m_main_context) {
            // Find predecessor and update head
            auto prev_offset = m_coroutine_head;
            auto prev = offset_to_ptr<CoroutineContext>(prev_offset);
            while (prev->m_next != target) {
                prev_offset = prev->m_next;
                prev = offset_to_ptr<CoroutineContext>(prev_offset);
            }
            prev->m_next = ctx->m_next;
            m_coroutine_head = ctx->m_next;
        }
    } else {
        // Find predecessor
        auto prev_offset = m_coroutine_head;
        auto prev = offset_to_ptr<CoroutineContext>(prev_offset);
        while (prev->m_next != target) {
            prev_offset = prev->m_next;
            prev = offset_to_ptr<CoroutineContext>(prev_offset);
            if (prev_offset == m_coroutine_head) {
                return;  // not found in list (already unlinked)
            }
        }
        prev->m_next = ctx->m_next;
    }
}

// Return the context to the free list.
void coroutine_free_context(CoroutineContext *ctx) {
    // Remove from scheduler linked list first
    coroutine_unlink(ctx);
    // Use the m_next field as the free list link
    ctx->m_next = m_coroutine_ctx_free;
    m_coroutine_ctx_free = ctx->m_context_offset;
}

// Scrub every wakeup registration that names a dying coroutine: pipe
// blocked reader/writer slots, other mailboxes' blocked-sender chains,
// and joiner links on other contexts.  Called from coroutine_kill.
// Without this, the registration dangles past death -- coroutine_wake
// no-ops on Dead, but contexts are pool-recycled, so the stale link
// can later wake an unrelated coroutine once the context is reused
// (ENGINE BUG #13).  The dying coroutine's OWN mailbox is recycled by
// coroutine_cleanup_mailbox; the stdin slot is cleared at the call
// site.  O(global chunks + registry) per kill; kills are infrequent.
void coroutine_scrub_registrations(vm_offset_t dying) {
    auto trx = this;

    // Pipe waiter slots + other mailboxes' blocked-sender chains.
    gvm_for_each([trx, dying](vm_offset_t payload_offset,
                              ChunkKind kind,
                              vm_size_t /*payload_size*/,
                              vm_size_t /*block_size*/,
                              bool is_free) {
        if (!is_free) {
            if (kind == ChunkKind::Mailbox) {
                auto mbx = trx->offset_to_ptr<MailboxHeader>(payload_offset);
                // skip pool-parked mailboxes (m_blocked_sender is a free-list link)
                if (mbx->m_head != MailboxRecycleSentinel) {
                    auto prev_link = &mbx->m_blocked_sender;
                    auto curr = mbx->m_blocked_sender;
                    while (curr != nulloffset) {
                        auto sender = trx->offset_to_ptr<CoroutineContext>(curr);
                        auto next = sender->m_blocked_sender_next;
                        if (curr == dying) {
                            *prev_link = next;
                            sender->m_blocked_sender_next = nulloffset;
                        } else {
                            prev_link = &sender->m_blocked_sender_next;
                        }
                        curr = next;
                    }
                }
            } else if (kind == ChunkKind::PipeBuffer) {
                auto hdr = trx->offset_to_ptr<PipeBufferHeader>(payload_offset);
                // skip pool-parked pipes (m_blocked_writer is a free-list link)
                if (hdr->m_head != PipeRecycleSentinel) {
                    if (hdr->m_blocked_reader == dying) {
                        hdr->m_blocked_reader = nulloffset;
                    }
                    if (hdr->m_blocked_writer == dying) {
                        hdr->m_blocked_writer = nulloffset;
                    }
                }
            }
        }
    });

    // Joiner links: any target awaiting its death-notification would
    // wake the dying (soon recycled) context.
    if (m_coroutine_head != nulloffset) {
        auto curr = m_coroutine_head;
        do {
            auto target = offset_to_ptr<CoroutineContext>(curr);
            if (target->m_joiner == dying) {
                target->m_joiner = nulloffset;
            }
            curr = target->m_next;
        } while (curr != m_coroutine_head);
    }
}

// Kill a coroutine: capture return value, store exit reason, cleanup stacks, set Dead,
// deliver death notifications to monitors/links.
void coroutine_kill(CoroutineContext *ctx, Object reason) {
    auto trx = this;

    // Store exit reason before any cleanup
    ctx->m_exit_reason = reason;

    // Capture return value only on normal exit (not error death).
    // Error death leaves stale operands on the stack that should not be
    // reported as a return value -- the coroutine did not complete its proc.
    auto is_normal = reason.is_name() && (reason.name_offset() == trx->wellknown_offset(WellKnownName::Normal));
    auto op_base = offset_to_ptr<Object>(ctx->m_op_base);
    auto op_ptr = offset_to_ptr<Object>(ctx->m_op_ptr) - 1;
    if (is_normal && (op_ptr >= op_base)) {
        // Clone top value (ExtValue/WideValue need fresh copies since stacks are about to be freed)
        auto top = *op_ptr;
        if (top.uses_extvalue()) {
            if (top.is_long()) {
                top = Object::make_long(trx, top.long_value(trx));
            } else if (top.is_ulong()) {
                top = Object::make_ulong(trx, top.ulong_value(trx));
            } else if (top.is_double()) {
                top = Object::make_double(trx, top.double_value(trx));
            } else if (top.is_address()) {
                top = Object::make_address(trx, top.address_value(trx));
            }
        } else if (top.uses_widevalue()) {
            if (top.is_int128()) {
                top = Object::make_int128(trx, top.int128_value(trx));
            } else if (top.is_uint128()) {
                top = Object::make_uint128(trx, top.uint128_value(trx));
            }
        }
        ctx->m_return_value = top;
        ctx->m_has_return = true;
    }

    // Remove from scheduling lists before setting Dead.
    // Suspended coroutines are not in any scheduling list.
    if (ctx->m_flags & CoroutineContext::FlagSuspended) {
        // Not in any list -- nothing to remove
    } else if (ctx->m_status == CoroutineContext::Ready) {
        ready_queue_remove(ctx);
    } else if ((ctx->m_status == CoroutineContext::Sleeping) && !(ctx->m_flags & CoroutineContext::FlagBlocked)) {
        timer_list_remove(ctx);
    }

    // If the dying coroutine was the single stdin reader, clear the slot so a
    // future read-key-byte from another coroutine doesn't see a stale offset.
    if (m_stdin_blocked_reader == ctx->m_context_offset) {
        m_stdin_blocked_reader = nulloffset;
    }

    // Scrub every other wakeup registration that names the dying
    // coroutine (ENGINE BUG #13, found by scheduler_validate's first
    // run): a coroutine killed while parked on a pipe, parked in
    // another actor's blocked-sender chain, or recorded as a joiner
    // left a dangling registration.  coroutine_wake no-ops on Dead,
    // but contexts are pool-recycled, so a stale link can later wake
    // an UNRELATED coroutine once the context is reused.  Death owns
    // registration cleanup -- restore never did (see the deleted
    // mailbox_scrub_above_barrier).
    coroutine_scrub_registrations(ctx->m_context_offset);

    coroutine_cleanup_stacks(ctx);
    coroutine_cleanup_mailbox(ctx);
    coroutine_free_stacks(ctx);
    binding_table_free_all(ctx->m_binding_table);
    ctx->m_binding_table = nulloffset;
    ctx->m_status = CoroutineContext::Dead;
    ctx->m_flags &= CoroutineContext::FlagWasActor;  // clear Suspended/Blocked on death; preserve FlagWasActor for is-actor
    coroutine_count_dec();

    // Deliver death notifications to monitors/links
    if ((ctx->m_monitors != nulloffset) || (ctx->m_monitoring != nulloffset)) {
        supervision_notify_death(ctx, ctx->m_context_offset);
    }

    // Wake joiner if any
    if (ctx->m_joiner != nulloffset) {
        auto joiner = offset_to_ptr<CoroutineContext>(ctx->m_joiner);
        if (joiner->m_status == CoroutineContext::Sleeping) {
            joiner->m_flags &= ~CoroutineContext::FlagBlocked;
            if (joiner->m_flags & CoroutineContext::FlagSuspended) {
                joiner->m_status = CoroutineContext::Ready;
            } else {
                joiner->m_status = CoroutineContext::Ready;
                ready_queue_push(joiner);
            }
        }
        ctx->m_joiner = nulloffset;
    }
}

//===--- Ready Queues (two-tier doubly-linked FIFOs) ---===//
//
// Two independent queues: high-priority (m_ready_high_head/tail) drained
// before normal-priority (m_ready_head/tail).  ctx->m_priority determines
// which queue a coroutine is pushed to.  Scheduler always pops high first.

// Push a coroutine onto the ready queue tail (respects ctx->m_priority).
void ready_queue_push(CoroutineContext *ctx) {
    assert((ctx->m_ready_next == nulloffset) && (ctx->m_ready_prev == nulloffset));

    ctx->m_ready_next = nulloffset;
    if (ctx->m_priority == CoroutineContext::PriorityHigh) {
        ctx->m_ready_prev = m_ready_high_tail;
        if (m_ready_high_tail != nulloffset) {
            offset_to_ptr<CoroutineContext>(m_ready_high_tail)->m_ready_next = ctx->m_context_offset;
        } else {
            m_ready_high_head = ctx->m_context_offset;
        }
        m_ready_high_tail = ctx->m_context_offset;
        ++m_ready_high_queue_depth;
    } else {
        ctx->m_ready_prev = m_ready_tail;
        if (m_ready_tail != nulloffset) {
            offset_to_ptr<CoroutineContext>(m_ready_tail)->m_ready_next = ctx->m_context_offset;
        } else {
            m_ready_head = ctx->m_context_offset;
        }
        m_ready_tail = ctx->m_context_offset;
        ++m_ready_queue_depth;
    }
}

// Pop from ready queues: high-priority first, then normal.  Returns nullptr if both empty.
[[nodiscard]] CoroutineContext *ready_queue_pop() {
    // Try high-priority queue first
    if (m_ready_high_head != nulloffset) {
        auto ctx = offset_to_ptr<CoroutineContext>(m_ready_high_head);
        m_ready_high_head = ctx->m_ready_next;
        if (m_ready_high_head != nulloffset) {
            offset_to_ptr<CoroutineContext>(m_ready_high_head)->m_ready_prev = nulloffset;
        } else {
            m_ready_high_tail = nulloffset;
        }
        ctx->m_ready_next = nulloffset;
        ctx->m_ready_prev = nulloffset;
        --m_ready_high_queue_depth;
        ++m_sched_ready_pops;
        return ctx;
    } else {
        // Fall through to normal-priority queue
        if (m_ready_head == nulloffset) {
            return nullptr;
        } else {
            auto ctx = offset_to_ptr<CoroutineContext>(m_ready_head);
            m_ready_head = ctx->m_ready_next;
            if (m_ready_head != nulloffset) {
                offset_to_ptr<CoroutineContext>(m_ready_head)->m_ready_prev = nulloffset;
            } else {
                m_ready_tail = nulloffset;
            }
            ctx->m_ready_next = nulloffset;
            ctx->m_ready_prev = nulloffset;
            --m_ready_queue_depth;
            ++m_sched_ready_pops;
            return ctx;
        }
    }
}

// Remove a specific coroutine from whichever ready queue it's in.  O(1).
// Uses ctx->m_priority to determine which queue's head/tail to update.
// Safe to call even if ctx is not in the queue (checks head/tail).
void ready_queue_remove(CoroutineContext *ctx) {
    auto offset = ctx->m_context_offset;
    auto is_high = (ctx->m_priority == CoroutineContext::PriorityHigh);
    auto &head = is_high ? m_ready_high_head : m_ready_head;
    auto &tail = is_high ? m_ready_high_tail : m_ready_tail;
    if (ctx->m_ready_prev != nulloffset) {
        offset_to_ptr<CoroutineContext>(ctx->m_ready_prev)->m_ready_next = ctx->m_ready_next;
    } else if (head == offset) {
        head = ctx->m_ready_next;
    } else {
        return;  // not in queue
    }
    if (ctx->m_ready_next != nulloffset) {
        offset_to_ptr<CoroutineContext>(ctx->m_ready_next)->m_ready_prev = ctx->m_ready_prev;
    } else if (tail == offset) {
        tail = ctx->m_ready_prev;
    }
    ctx->m_ready_next = nulloffset;
    ctx->m_ready_prev = nulloffset;
    if (is_high) {
        --m_ready_high_queue_depth;
    } else {
        --m_ready_queue_depth;
    }
}

//===--- Timer List (sorted singly-linked, ascending by wake_time_ns) ---===//

// Insert into timer list at sorted position.
void timer_list_insert(CoroutineContext *ctx) {
    auto offset = ctx->m_context_offset;
    auto wake = ctx->m_wake_time_ns;
    // Double-insert protection: inserting a node that is already linked
    // creates a cycle and the walk below never terminates.  Catches the
    // sole-entry case (head == ctx, m_timer_next == nulloffset) and any
    // mid-list case (m_timer_next != nulloffset).
    assert(ctx->m_timer_next == nulloffset);
    assert(m_timer_head != offset);

    if ((m_timer_head == nulloffset) || (wake < offset_to_ptr<CoroutineContext>(m_timer_head)->m_wake_time_ns)) {
        ctx->m_timer_next = m_timer_head;
        m_timer_head = offset;
        ++m_timer_list_depth;
    } else {
        auto prev = offset_to_ptr<CoroutineContext>(m_timer_head);
        while (prev->m_timer_next != nulloffset) {
            auto next = offset_to_ptr<CoroutineContext>(prev->m_timer_next);
            if (wake < next->m_wake_time_ns) {
                break;
            } else {
                prev = next;
            }
        }
        ctx->m_timer_next = prev->m_timer_next;
        prev->m_timer_next = offset;
        ++m_timer_list_depth;
    }
}

// Remove a specific coroutine from the timer list.  O(n) walk.
void timer_list_remove(CoroutineContext *ctx) {
    auto offset = ctx->m_context_offset;
    if (m_timer_head == offset) {
        m_timer_head = ctx->m_timer_next;
        ctx->m_timer_next = nulloffset;
        --m_timer_list_depth;
    } else {
        auto prev_offset = m_timer_head;
        while (prev_offset != nulloffset) {
            auto prev = offset_to_ptr<CoroutineContext>(prev_offset);
            if (prev->m_timer_next == offset) {
                prev->m_timer_next = ctx->m_timer_next;
                ctx->m_timer_next = nulloffset;
                --m_timer_list_depth;
                return;
            } else {
                prev_offset = prev->m_timer_next;
            }
        }
    }
}

// Wake a blocked coroutine by setting its status to Ready and pushing to ready queue.
// Removes from timer list if the coroutine was in a timed sleep.
// If the coroutine is suspended, marks it Ready but does not push to ready queue
// (it will be enqueued when resumed).
// No-op if blocked_offset is nulloffset.
void coroutine_wake(vm_offset_t blocked_offset) {
    if (blocked_offset != nulloffset) {
        auto ctx = offset_to_ptr<CoroutineContext>(blocked_offset);
        if (ctx->m_status == CoroutineContext::Sleeping) {
            if (!(ctx->m_flags & CoroutineContext::FlagBlocked)) {
                timer_list_remove(ctx);
            }
            // Waking the registered stdin reader by any path (producer wake,
            // poll-driven wake) releases the single-reader slot; the woken
            // coroutine's retry continuation re-registers if it blocks again.
            // The scheduler's poll path clears the slot before calling here,
            // so this is idempotent for that caller.
            if (m_stdin_blocked_reader == ctx->m_context_offset) {
                m_stdin_blocked_reader = nulloffset;
            }
            ctx->m_flags &= ~CoroutineContext::FlagBlocked;
            if (ctx->m_flags & CoroutineContext::FlagSuspended) {
                ctx->m_status = CoroutineContext::Ready;
            } else {
                ctx->m_status = CoroutineContext::Ready;
                ready_queue_push(ctx);
            }
        }
    }
}

// Sweep all expired timers into the ready queue.
// Skips entries already woken by a producer (status != Sleeping).
void timer_sweep_expired(uint64_t now) {
    while (m_timer_head != nulloffset) {
        auto ctx = offset_to_ptr<CoroutineContext>(m_timer_head);
        if (ctx->m_wake_time_ns > now) {
            break;
        } else {
            m_timer_head = ctx->m_timer_next;
            ctx->m_timer_next = nulloffset;
            --m_timer_list_depth;
            // A timed stdin reader (read-key-byte-timeout) whose timer fires must
            // release the single-reader slot; otherwise its next read attempt
            // raises /invalid-access against its own stale registration.
            if (m_stdin_blocked_reader == ctx->m_context_offset) {
                m_stdin_blocked_reader = nulloffset;
            }
            if (ctx->m_status == CoroutineContext::Sleeping) {
                ctx->m_status = CoroutineContext::Ready;
                ready_queue_push(ctx);
                ++m_sched_timer_wakes;
            }
        }
    }
}

//===--- Scheduler ---===//

// Scheduler: find the next ready coroutine and switch to it.
// Called from sleep, die, kill, @coroutine-complete.
//
// O(1) for the common case (ready queue non-empty).
// Falls back to timer sweep, then real-thread sleep (poll(stdin) when a
// coroutine is blocked in read-key-byte; nanosleep otherwise), then main.
void coroutine_schedule() {
    ++m_sched_count;

    // 0. Sweep already-expired timers BEFORE popping ready work.  A busy
    // ready set must not starve timed sleepers: a hot pair of coroutines
    // that never blocks keeps the ready queue non-empty forever, and the
    // step-2 sweep below would then never run -- every coroutine-sleep in
    // the system wedges (ENGINE BUG #14, found by the sched-stress
    // harness: a pipe producer/consumer ping-pong starved main's 15ms
    // sleep indefinitely).  O(1) when nothing expired: a head peek; the
    // clock read only happens when timers exist at all.
    if (m_timer_head != nulloffset) {
        auto now = monotonic_ns();
        if (offset_to_ptr<CoroutineContext>(m_timer_head)->m_wake_time_ns <= now) {
            timer_sweep_expired(now);
        }
    }

    // 1. Try ready queue -- O(1)
    auto found = ready_queue_pop();
    if (found != nullptr) {
        coroutine_load(found);
    } else {
        // 2./3. Sweep expired timers, then sleep the real thread until the next
        // timer expiry or stdin readability.  Looped: poll() timeouts are clamped
        // to whole milliseconds (and either sleep call can be cut short by a
        // signal), so a sleep may wake just BEFORE the earliest timer expires.
        // When that happens nothing is ready yet and we must wait again --
        // falling through to step 4 would load main while it is still linked in
        // the timer list, and its next sleep would re-insert the linked node,
        // corrupting the list into a cycle (timer_list_insert walks forever).
        while (true) {
            auto now = monotonic_ns();
            timer_sweep_expired(now);
            found = ready_queue_pop();
            if (found != nullptr) {
                coroutine_load(found);
                return;
            } else {
                auto has_timer = (m_timer_head != nulloffset);
                auto has_stdin_reader = (m_stdin_blocked_reader != nulloffset);
                if (!has_timer && !has_stdin_reader) {
                    break;  // nothing to wait for
                } else {
                    ++m_sched_real_sleeps;

                    // Compute timeout in milliseconds.  No timer + reader present == -1
                    // (block forever; only stdin can wake us).  Timer present == clamp
                    // delta_ns to a poll-compatible int millisecond count.
                    int poll_timeout_ms = -1;
                    if (has_timer) {
                        auto earliest = offset_to_ptr<CoroutineContext>(m_timer_head)->m_wake_time_ns;
                        now = monotonic_ns();
                        if (earliest <= now) {
                            poll_timeout_ms = 0;
                        } else {
                            auto delta_ns = earliest - now;
                            if (delta_ns >= 1'000'000'000'000ULL) {
                                poll_timeout_ms = 1'000'000'000;  // ~11.5 days; effectively unbounded
                            } else {
                                poll_timeout_ms = static_cast<int>(delta_ns / 1'000'000ULL);
                                if ((poll_timeout_ms == 0) && (delta_ns > 0)) {
                                    poll_timeout_ms = 1;  // round tiny deltas up so poll doesn't busy-spin
                                }
                            }
                        }
                    }

                    if (has_stdin_reader) {
                        pollfd pfd{STDIN_FILENO, POLLIN, 0};
                        static_cast<void>(::poll(&pfd, 1, poll_timeout_ms));
                        if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                            auto reader = m_stdin_blocked_reader;
                            m_stdin_blocked_reader = nulloffset;
                            coroutine_wake(reader);
                        }
                    } else {
                        // Timer-only path: nanosleep.  delta_ns recomputed for precision
                        // (poll_timeout_ms was clamped to milliseconds).
                        auto earliest = offset_to_ptr<CoroutineContext>(m_timer_head)->m_wake_time_ns;
                        now = monotonic_ns();
                        if (earliest > now) {
                            auto delta_ns = earliest - now;
                            timespec ts{};
                            ts.tv_sec = static_cast<time_t>(delta_ns / 1'000'000'000ULL);
                            ts.tv_nsec = static_cast<long>(delta_ns % 1'000'000'000ULL);
                            ::nanosleep(&ts, nullptr);
                        }
                    }
                    // Loop: re-sweep timers and re-check the ready queue.
                }
            }
        }

        // 4. No ready, no timers, no stdin reader -- fall back to main
        coroutine_load(offset_to_ptr<CoroutineContext>(m_main_context));
    }
}

// Grant up to `ms` of wall-clock park time against the cumulative sleep
// budget (m_sleep_budget_ms, 0 = unlimited).  m_max_ops cannot tick while
// a coroutine is parked, so a bounded run is still stallable for arbitrary
// wall time by one huge sleep/timeout operand; embedders that need a hard
// wall-clock bound (e.g. the fuzz harness) set the budget, and once it is
// spent every timed park degrades to an immediate wake.
uint64_t sleep_budget_grant(uint64_t ms) {
    if (m_sleep_budget_ms == 0) {
        return ms;
    } else {
        auto remaining{m_sleep_budget_ms - m_sleep_granted_ms};
        auto granted{std::min(ms, remaining)};
        m_sleep_granted_ms += granted;
        return granted;
    }
}

// Push a continuation control operator, flush the running coroutine to Sleeping
// with the given wake time, and schedule the next runnable coroutine.
void coroutine_sleep_and_schedule(SystemName continuation_op, uint64_t wake_time) {
    require_exec_capacity(1);
    *++m_exec_ptr = Object::make_control_operator(continuation_op);
    coroutine_flush_running();
    auto ctx = offset_to_ptr<CoroutineContext>(m_running_coroutine);
    ctx->m_status = CoroutineContext::Sleeping;
    ctx->m_wake_time_ns = wake_time;
    if (wake_time == std::numeric_limits<uint64_t>::max()) {
        // Blocked (pipe/actor/supervision) -- producer will push to ready queue
        ctx->m_flags |= CoroutineContext::FlagBlocked;
    } else if (wake_time <= monotonic_ns()) {
        // Already expired (e.g. yield) -- go directly to ready queue tail
        ctx->m_status = CoroutineContext::Ready;
        ready_queue_push(ctx);
    } else {
        timer_list_insert(ctx);
    }
    coroutine_schedule();
}

//===--- Scheduler / wakeup invariant validation ---===//
//
// Cross-subsystem invariants between the coroutine registry, the
// scheduler lists, and every wakeup registration: timer list, ready
// queues, stdin reader slot, mailbox reader + blocked-sender chain,
// pipe blocked reader/writer, and joiner links.  The load-bearing
// invariant is NO ORPHANED SLEEPER: every Sleeping coroutine must be
// reachable from at least one wake source, or it can never run again.
// ENGINE BUGS #10/#11/#12 were all members of this family (a cycled
// timer list, a stale stdin reader slot, and a restore that wiped
// parked mailbox registrations); each would have tripped here at the
// moment of corruption instead of presenting as a downstream hang.
//
// Wired into //:status:vm-validate, so the trailing integrity check of
// every test file covers the scheduler for free.  All walks are
// bounded by the registry size so a corrupted (cyclic) list reports
// false instead of hanging the validator.  Violations print one line
// each to stderr: scheduler state is not inspectable from script, so a
// bare false would be undebuggable.
[[nodiscard]] bool scheduler_validate() {
    auto trx = this;
    auto valid = true;

    // 0. The registry must be a closed circle.  Its size bounds every
    // other walk below.
    constexpr uint32_t kWalkCap{1u << 20};
    uint32_t registry_count{0};
    if (m_coroutine_head != nulloffset) {
        auto curr = m_coroutine_head;
        do {
            ++registry_count;
            curr = offset_to_ptr<CoroutineContext>(curr)->m_next;
        } while ((curr != m_coroutine_head) && (registry_count < kWalkCap));
        if (curr != m_coroutine_head) {
            fprintf(stderr, "sched-validate: coroutine registry does not close (cycle bound hit)\n");
            return false;
        }
    }

    // Bounded membership probes (value captures only).
    auto in_timer_list = [trx, registry_count](vm_offset_t offset) -> bool {
        auto curr = trx->m_timer_head;
        uint32_t steps{0};
        while ((curr != nulloffset) && (steps <= registry_count)) {
            if (curr == offset) {
                return true;
            } else {
                curr = trx->offset_to_ptr<CoroutineContext>(curr)->m_timer_next;
                ++steps;
            }
        }
        return false;
    };
    auto in_ready_queues = [trx, registry_count](vm_offset_t offset) -> bool {
        vm_offset_t heads[2] = {trx->m_ready_high_head, trx->m_ready_head};
        for (auto head : heads) {
            auto curr = head;
            uint32_t steps{0};
            while ((curr != nulloffset) && (steps <= registry_count)) {
                if (curr == offset) {
                    return true;
                } else {
                    curr = trx->offset_to_ptr<CoroutineContext>(curr)->m_ready_next;
                    ++steps;
                }
            }
        }
        return false;
    };

    // 1. Timer list: closed within bound, ascending wake times, depth
    // counter exact, every node a timed sleeper (Sleeping, not Blocked,
    // not Suspended), and never simultaneously ready-queued.
    {
        uint32_t count{0};
        uint64_t prev_wake{0};
        auto curr = m_timer_head;
        while ((curr != nulloffset) && (count <= registry_count)) {
            auto ctx = offset_to_ptr<CoroutineContext>(curr);
            if (ctx->m_wake_time_ns < prev_wake) {
                fprintf(stderr, "sched-validate: timer list out of order at ctx=%u\n", curr);
                valid = false;
            }
            prev_wake = ctx->m_wake_time_ns;
            if (ctx->m_status != CoroutineContext::Sleeping) {
                fprintf(stderr, "sched-validate: timer node ctx=%u status=%d (want Sleeping)\n", curr, +ctx->m_status);
                valid = false;
            }
            if ((ctx->m_flags & CoroutineContext::FlagBlocked) != 0) {
                fprintf(stderr, "sched-validate: timer node ctx=%u has FlagBlocked\n", curr);
                valid = false;
            }
            if ((ctx->m_flags & CoroutineContext::FlagSuspended) != 0) {
                fprintf(stderr, "sched-validate: timer node ctx=%u has FlagSuspended\n", curr);
                valid = false;
            }
            if (in_ready_queues(curr)) {
                fprintf(stderr, "sched-validate: ctx=%u in BOTH timer list and a ready queue\n", curr);
                valid = false;
            }
            ++count;
            curr = ctx->m_timer_next;
        }
        if (curr != nulloffset) {
            fprintf(stderr, "sched-validate: timer list does not terminate (cycle)\n");
            return false;
        } else if (count != m_timer_list_depth) {
            fprintf(stderr, "sched-validate: timer depth %u != counter %u\n", count, m_timer_list_depth);
            valid = false;
        }
    }

    // 2. Ready queues: closed, back-links consistent, tail correct,
    // depth counters exact, every node Ready with the queue's priority.
    auto check_ready_queue =
            [trx, registry_count, in_timer_list](vm_offset_t head, vm_offset_t tail, uint32_t depth, bool want_high) -> bool {
        auto ok = true;
        uint32_t count{0};
        vm_offset_t prev{nulloffset};
        auto curr = head;
        while ((curr != nulloffset) && (count <= registry_count)) {
            auto ctx = trx->offset_to_ptr<CoroutineContext>(curr);
            if (ctx->m_ready_prev != prev) {
                fprintf(stderr, "sched-validate: ready back-link broken at ctx=%u\n", curr);
                ok = false;
            }
            if (ctx->m_status != CoroutineContext::Ready) {
                fprintf(stderr, "sched-validate: ready node ctx=%u status=%d (want Ready)\n", curr, +ctx->m_status);
                ok = false;
            }
            if ((ctx->m_priority == CoroutineContext::PriorityHigh) != want_high) {
                fprintf(stderr, "sched-validate: ctx=%u priority/queue mismatch\n", curr);
                ok = false;
            }
            if (in_timer_list(curr)) {
                fprintf(stderr, "sched-validate: ctx=%u in BOTH ready queue and timer list\n", curr);
                ok = false;
            }
            prev = curr;
            curr = ctx->m_ready_next;
            ++count;
        }
        if (curr != nulloffset) {
            fprintf(stderr, "sched-validate: ready queue does not terminate (cycle)\n");
            return false;
        } else if (prev != tail) {
            fprintf(stderr, "sched-validate: ready tail %u != last node %u\n", tail, prev);
            ok = false;
        }
        if (count != depth) {
            fprintf(stderr, "sched-validate: ready depth %u != counter %u\n", count, depth);
            ok = false;
        }
        return ok;
    };
    valid = check_ready_queue(m_ready_high_head, m_ready_high_tail, m_ready_high_queue_depth, true) && valid;
    valid = check_ready_queue(m_ready_head, m_ready_tail, m_ready_queue_depth, false) && valid;

    // 3. The stdin reader slot must point at a Sleeping coroutine (a
    // stale slot was ENGINE BUG #11's signature).
    if (m_stdin_blocked_reader != nulloffset) {
        auto reader = offset_to_ptr<CoroutineContext>(m_stdin_blocked_reader);
        if (reader->m_status != CoroutineContext::Sleeping) {
            fprintf(stderr,
                    "sched-validate: stdin slot ctx=%u status=%d (want Sleeping)\n",
                    m_stdin_blocked_reader,
                    +reader->m_status);
            valid = false;
        }
    }

    // 4. Every mailbox / pipe registration must point at a Sleeping
    // coroutine; blocked-sender chains must be closed and well-formed.
    {
        auto valid_ptr = &valid;
        gvm_for_each([trx, registry_count, valid_ptr](vm_offset_t payload_offset,
                                                      ChunkKind kind,
                                                      vm_size_t /*payload_size*/,
                                                      vm_size_t /*block_size*/,
                                                      bool is_free) {
            if (!is_free) {
                if (kind == ChunkKind::Mailbox) {
                    auto mbx = trx->offset_to_ptr<MailboxHeader>(payload_offset);
                    // skip pool-parked mailboxes (m_blocked_sender is a free-list link)
                    if (mbx->m_head != MailboxRecycleSentinel) {
                        if (mbx->m_blocked_reader != nulloffset) {
                            auto ctx = trx->offset_to_ptr<CoroutineContext>(mbx->m_blocked_reader);
                            if (ctx->m_status != CoroutineContext::Sleeping) {
                                fprintf(stderr,
                                        "sched-validate: mailbox %u reader ctx=%u status=%d (want Sleeping)\n",
                                        payload_offset,
                                        mbx->m_blocked_reader,
                                        +ctx->m_status);
                                *valid_ptr = false;
                            }
                        }
                        auto curr = mbx->m_blocked_sender;
                        uint32_t steps{0};
                        while ((curr != nulloffset) && (steps <= registry_count)) {
                            auto sender = trx->offset_to_ptr<CoroutineContext>(curr);
                            if ((sender->m_status != CoroutineContext::Sleeping) ||
                                ((sender->m_flags & CoroutineContext::FlagBlocked) == 0)) {
                                fprintf(stderr,
                                        "sched-validate: mailbox %u blocked sender ctx=%u not Sleeping+Blocked\n",
                                        payload_offset,
                                        curr);
                                *valid_ptr = false;
                            }
                            curr = sender->m_blocked_sender_next;
                            ++steps;
                        }
                        if (curr != nulloffset) {
                            fprintf(stderr, "sched-validate: mailbox %u sender chain does not terminate\n", payload_offset);
                            *valid_ptr = false;
                        }
                    }
                } else if (kind == ChunkKind::PipeBuffer) {
                    auto hdr = trx->offset_to_ptr<PipeBufferHeader>(payload_offset);
                    // skip pool-parked pipes (m_blocked_writer is a free-list link)
                    if (hdr->m_head != PipeRecycleSentinel) {
                        vm_offset_t waiters[2] = {hdr->m_blocked_reader, hdr->m_blocked_writer};
                        for (auto waiter : waiters) {
                            if (waiter != nulloffset) {
                                auto ctx = trx->offset_to_ptr<CoroutineContext>(waiter);
                                if (ctx->m_status != CoroutineContext::Sleeping) {
                                    fprintf(stderr,
                                            "sched-validate: pipe %u waiter ctx=%u status=%d (want Sleeping)\n",
                                            payload_offset,
                                            waiter,
                                            +ctx->m_status);
                                    *valid_ptr = false;
                                }
                            }
                        }
                    }
                }
            }
        });
    }

    // 5. Per-coroutine checks over the registry: joiner sanity, the
    // live counter, and THE ORPHAN RULE.
    if (m_coroutine_head != nulloffset) {
        // Reachability probe for blocked sleepers: scan mailbox/pipe
        // registrations for one target.  O(global chunks) per blocked
        // sleeper; sleeper counts are tiny.
        auto blocked_registration_exists = [trx, registry_count](vm_offset_t target) -> bool {
            auto found = false;
            auto found_ptr = &found;
            trx->gvm_for_each([trx, registry_count, target, found_ptr](vm_offset_t payload_offset,
                                                                       ChunkKind kind,
                                                                       vm_size_t /*payload_size*/,
                                                                       vm_size_t /*block_size*/,
                                                                       bool is_free) {
                if (!is_free && !*found_ptr) {
                    if (kind == ChunkKind::Mailbox) {
                        auto mbx = trx->offset_to_ptr<MailboxHeader>(payload_offset);
                        if (mbx->m_head != MailboxRecycleSentinel) {
                            if (mbx->m_blocked_reader == target) {
                                *found_ptr = true;
                            } else {
                                auto curr = mbx->m_blocked_sender;
                                uint32_t steps{0};
                                while ((curr != nulloffset) && (steps <= registry_count)) {
                                    if (curr == target) {
                                        *found_ptr = true;
                                        break;
                                    } else {
                                        curr = trx->offset_to_ptr<CoroutineContext>(curr)->m_blocked_sender_next;
                                        ++steps;
                                    }
                                }
                            }
                        }
                    } else if (kind == ChunkKind::PipeBuffer) {
                        auto hdr = trx->offset_to_ptr<PipeBufferHeader>(payload_offset);
                        if ((hdr->m_head != PipeRecycleSentinel) &&
                            ((hdr->m_blocked_reader == target) || (hdr->m_blocked_writer == target))) {
                            *found_ptr = true;
                        }
                    }
                }
            });
            return found;
        };
        auto is_someones_joiner = [trx](vm_offset_t target) -> bool {
            auto curr = trx->m_coroutine_head;
            do {
                auto ctx = trx->offset_to_ptr<CoroutineContext>(curr);
                if (ctx->m_joiner == target) {
                    return true;
                } else {
                    curr = ctx->m_next;
                }
            } while (curr != trx->m_coroutine_head);
            return false;
        };

        uint32_t live_non_main{0};
        auto curr = m_coroutine_head;
        do {
            auto ctx = offset_to_ptr<CoroutineContext>(curr);
            if ((ctx->m_status != CoroutineContext::Dead) && (curr != m_main_context)) {
                ++live_non_main;
            }
            // Joiner links point at a coroutine that is parked waiting.
            // (A killed waiter's stale link is scrubbed at death by
            // coroutine_scrub_registrations.)
            if (ctx->m_joiner != nulloffset) {
                auto joiner = offset_to_ptr<CoroutineContext>(ctx->m_joiner);
                if (joiner->m_status != CoroutineContext::Sleeping) {
                    fprintf(stderr,
                            "sched-validate: ctx=%u joiner ctx=%u status=%d (want Sleeping)\n",
                            curr,
                            ctx->m_joiner,
                            +joiner->m_status);
                    valid = false;
                }
            }
            // THE ORPHAN RULE: every Sleeping coroutine must have a
            // waker.  Suspended sleepers are exempt (coroutine-resume
            // is their waker).
            if ((ctx->m_status == CoroutineContext::Sleeping) && ((ctx->m_flags & CoroutineContext::FlagSuspended) == 0)) {
                if ((ctx->m_flags & CoroutineContext::FlagBlocked) == 0) {
                    if (!in_timer_list(curr)) {
                        fprintf(stderr, "sched-validate: ORPHANED SLEEPER ctx=%u (timed, not in timer list)\n", curr);
                        valid = false;
                    }
                } else {
                    auto reachable =
                            (m_stdin_blocked_reader == curr) || is_someones_joiner(curr) || blocked_registration_exists(curr);
                    if (!reachable) {
                        fprintf(stderr, "sched-validate: ORPHANED SLEEPER ctx=%u (blocked, no wake source)\n", curr);
                        valid = false;
                    }
                }
            }
            curr = ctx->m_next;
        } while (curr != m_coroutine_head);

        if (live_non_main != m_live_coroutine_count) {
            fprintf(stderr,
                    "sched-validate: live count %u != counter %u\n",
                    live_non_main,
                    static_cast<uint32_t>(m_live_coroutine_count));
            valid = false;
        }
    }

    return valid;
}

// Kill all non-main coroutines (called when main finishes).
void coroutine_kill_all() {
    if (m_live_coroutine_count != 0) {
        auto curr_offset = m_coroutine_head;
        do {
            auto curr = offset_to_ptr<CoroutineContext>(curr_offset);
            auto next = curr->m_next;
            if ((curr_offset != m_main_context) && (curr->m_status != CoroutineContext::Dead)) {
                coroutine_kill(curr, wellknown_name(WellKnownName::Killed));
            }
            curr_offset = next;
        } while (curr_offset != m_coroutine_head);

        // Defensive reset: the per-coroutine_kill decrement path should have
        // already brought the count to 0 and fired the 1 -> 0 gate transition.
        // If for any reason the count is still non-zero here, force it down
        // and fire the transition explicitly so main's m_binding_table is
        // dropped.
        if (m_live_coroutine_count != 0) {
            m_live_coroutine_count = 0;
            on_last_death_transition();
        }

        // Defensive reset: all scheduling structures should be empty after killing
        // every non-main coroutine.  Reset them to a known-good state.
        m_ready_head = nulloffset;
        m_ready_tail = nulloffset;
        m_ready_high_head = nulloffset;
        m_ready_high_tail = nulloffset;
        m_timer_head = nulloffset;
        m_ready_queue_depth = 0;
        m_ready_high_queue_depth = 0;
        m_timer_list_depth = 0;
    }
}

// Allocate a contiguous stack block for a coroutine.
// Layout: [operand][execution][error][dictionary].  Fresh allocations
// route through gvm_alloc into global VM so the block is BASE-immune
// across save/restore -- coroutines spawned at
// save level N now survive an outer restore past N.  The class-keyed
// free list (m_coroutine_stack_free) keeps its in-block free-list
// link in slot 0 of the payload; entries land back here after a
// coroutine dies.
[[nodiscard]] vm_offset_t coroutine_alloc_stacks() {
    // Check free list first
    if (m_coroutine_stack_free != nulloffset) {
        auto offset = m_coroutine_stack_free;
        auto block = offset_to_ptr<vm_offset_t>(offset);
        m_coroutine_stack_free = *block;
        return offset;
    } else {
        // Scratch arena lives at the back of the stack
        // block.  Sum of the per-region depths = total Object count;
        // gvm_alloc_n<Object>(N) handles the byte multiplication itself.
        auto total_objects = static_cast<vm_size_t>(DefaultCoroutineOperandDepth + DefaultCoroutineExecutionDepth +
                                                    DefaultCoroutineErrorDepth + DefaultCoroutineDictionaryDepth + m_scratch_depth);
        auto [_, offset] = gvm_alloc_n<Object>(total_objects, ChunkKind::CoroutineStacks);
        return offset;
    }
}

// Allocate a CoroutineContext.  Fresh allocations route through
// gvm_alloc into global VM (see
// coroutine_alloc_stacks comment for rationale).
//
// Main coroutine special case: the very first call (when
// m_main_context == nulloffset, i.e. init.inl is setting up coroutine #0)
// allocates LOCALLY via vm_alloc.  Reasons:
//
//   1. Main never dies, never gets killed, never gets restored across
//      a save barrier -- the cross-restore survival rationale that
//      put other CoroutineContexts in global VM doesn't apply.
//   2. With main local, programs that never do a user gvm_alloc
//      (no ${...} / set-global / <lit>#$) never trigger
//      ensure_gc_scratch, saving the 96 KiB scratch overhead and
//      keeping vm-global-gc a no-op for global-free workloads.
//
// GC handles the locality split via walk_all_roots: the registry walk
// (mark_global_offset on each context offset) silent-skips main's
// local offset, but walk_all_roots calls gc_walk_coroutine_context
// directly on m_main_context as a separate root pass.  See gc.inl.
[[nodiscard]] std::pair<CoroutineContext *, vm_offset_t> coroutine_alloc_context() {
    // Check free list first
    if (m_coroutine_ctx_free != nulloffset) {
        auto offset = m_coroutine_ctx_free;
        auto ctx = offset_to_ptr<CoroutineContext>(offset);
        m_coroutine_ctx_free = ctx->m_next;
        return {ctx, offset};
    } else if (m_main_context == nulloffset) {
        // Main coroutine -- first call ever, never reaches the free list.
        return vm_alloc<CoroutineContext>(sizeof(CoroutineContext));
    } else {
        return gvm_alloc<CoroutineContext>(sizeof(CoroutineContext), ChunkKind::CoroutineContext);
    }
}

// Bump the live-coroutine count and fire the 0 -> 1 gate transition on
// the first spawn.  Every site that spawns a coroutine (launch, actor-
// spawn, pipeline stage, supervisor child) must go through this helper
// so the Name::m_binding fast-path cache is correctly flushed before
// concurrent writes start contending.
void coroutine_count_inc() {
    if (m_live_coroutine_count == 0) {
        on_first_spawn_transition();
    }
    ++m_live_coroutine_count;
}

// Decrement and fire the 1 -> 0 gate transition when the last spawned
// coroutine dies.  Every coroutine_kill path must go through this
// helper so main's per-coroutine table (now stale) is dropped and the
// global Name::m_binding fast path resumes.
void coroutine_count_dec() {
    if (m_live_coroutine_count > 0) {
        --m_live_coroutine_count;
    }
    if (m_live_coroutine_count == 0) {
        on_last_death_transition();
    }
}

// Common launch logic for coroutine-launch and actor-spawn.
// Pops proc (top of op stack) and mark+params from the caller's operand stack,
// creates a CoroutineContext with stacks, moves params, inserts into scheduler.
// Does NOT push the coroutine handle (caller does that).
// Returns {ctx_ptr, ctx_offset}.
static std::pair<CoroutineContext *, vm_offset_t> coroutine_launch_common(Trix *trx, const char *op_name) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation);

    auto proc_obj = *trx->m_op_ptr--;

    // Root the popped proc across the context + stacks allocations below.  Each
    // of coroutine_alloc_context / coroutine_alloc_stacks can fire vm_global_gc,
    // and a proc scanned inside ${...} embeds GLOBAL literal ExtValues (Long /
    // Double) reachable ONLY through this now-off-stack Object until it lands on
    // the new coroutine's exec stack -- a stress GC would otherwise sweep them
    // and leave the body proc referencing freed blocks.  (A proc scanned outside
    // ${...} holds local literals, which a global GC never sweeps, so the bug is
    // ${...}-specific -- but rooting unconditionally is correct and cheap.)
    trx->gc_root_push_oneoff(proc_obj);

    // Find mark
    auto mark_ptr = trx->m_op_ptr;
    while (mark_ptr >= trx->m_op_base) {
        if (mark_ptr->is_mark()) {
            break;
        } else {
            --mark_ptr;
        }
    }
    if ((mark_ptr < trx->m_op_base) || !mark_ptr->is_mark()) {
        trx->error(Error::UnmatchedMark, "{}: no mark found", op_name);
    } else {
        auto param_count = static_cast<length_t>(trx->m_op_ptr - mark_ptr);

        // Check parameter count fits in coroutine operand stack
        if (param_count > DefaultCoroutineOperandDepth) {
            trx->error(Error::OpStackOverflow,
                       "{}: {} parameters exceed coroutine operand depth {}",
                       op_name,
                       param_count,
                       DefaultCoroutineOperandDepth);
        }

        // Allocate CoroutineContext
        auto [ctx, ctx_offset] = trx->coroutine_alloc_context();
        ctx->m_status = CoroutineContext::Ready;
        ctx->m_has_return = false;
        ctx->m_activation_sl = trx->m_curr_save_level;
        ctx->m_trap_exit = false;
        ctx->m_flags = 0;
        ctx->m_context_offset = ctx_offset;
        ctx->m_wake_time_ns = 0;
        ctx->m_suspend_remaining_ns = 0;
        ctx->m_ready_next = nulloffset;
        ctx->m_ready_prev = nulloffset;
        ctx->m_timer_next = nulloffset;
        ctx->m_id = trx->m_next_coroutine_id++;
        ctx->m_quantum = trx->m_default_coroutine_quantum;
        ctx->m_ops_remaining = trx->m_default_coroutine_quantum;
        ctx->m_priority = CoroutineContext::PriorityNormal;
        ctx->m_blocked_sender_next = nulloffset;
        ctx->m_last_run_time_ns = 0;
        ctx->m_joiner = nulloffset;
        ctx->m_return_value = Object::make_null();
        ctx->m_debug_name = nulloffset;
        ctx->m_mailbox = nulloffset;
        ctx->m_monitors = nulloffset;
        ctx->m_monitoring = nulloffset;
        ctx->m_binding_table = nulloffset;
        ctx->m_exit_reason = trx->wellknown_name(WellKnownName::Normal);
        ctx->m_last_joined_exit_reason = trx->wellknown_name(WellKnownName::NoError);
        // New coroutines start with global-alloc disabled regardless of the
        // launching coroutine's flag.  Per-coroutine isolation: ${...} only
        // affects its lexical body; a coroutine spawned inside ${...} is a
        // fresh execution context and doesn't inherit the spawner's flag.
        ctx->m_curr_alloc_global = false;

        // Root the context in the scheduler registry BEFORE allocating stacks:
        // coroutine_alloc_stacks can fire vm_global_gc, and an unlinked context is
        // unreachable from walk_all_roots and would be swept.  m_stacks_offset stays
        // nulloffset across that alloc so gc_walk_coroutine_context skips the
        // not-yet-initialized stack-region walk.
        ctx->m_stacks_offset = nulloffset;
        auto running = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
        ctx->m_next = running->m_next;
        running->m_next = ctx_offset;

        // Allocate stacks
        ctx->m_stacks_offset = trx->coroutine_alloc_stacks();

        // Partition the stack block: [operand][execution][error][dictionary][scratch]
        // Scratch is the per-coroutine arena, sized by Trix::m_scratch_depth.
        auto base_ptr = trx->offset_to_ptr<Object>(ctx->m_stacks_offset);
        auto op_base = base_ptr;
        auto exec_base = op_base + DefaultCoroutineOperandDepth;
        auto err_base = exec_base + DefaultCoroutineExecutionDepth;
        auto dict_base = err_base + DefaultCoroutineErrorDepth;
        auto scratch_base = dict_base + DefaultCoroutineDictionaryDepth;

        ctx->m_op_base = trx->ptr_to_offset(op_base);
        ctx->m_op_limit = trx->ptr_to_offset(op_base + DefaultCoroutineOperandDepth);
        ctx->m_op_high_water = trx->ptr_to_offset(op_base - 1);
        ctx->m_exec_base = trx->ptr_to_offset(exec_base);
        ctx->m_exec_limit = trx->ptr_to_offset(exec_base + DefaultCoroutineExecutionDepth);
        ctx->m_exec_high_water = trx->ptr_to_offset(exec_base - 1);
        ctx->m_err_base = trx->ptr_to_offset(err_base);
        ctx->m_err_limit = trx->ptr_to_offset(err_base + DefaultCoroutineErrorDepth);
        ctx->m_err_high_water = trx->ptr_to_offset(err_base - 1);
        ctx->m_dict_base = trx->ptr_to_offset(dict_base);
        ctx->m_dict_limit = trx->ptr_to_offset(dict_base + DefaultCoroutineDictionaryDepth);
        ctx->m_dict_high_water = trx->ptr_to_offset(dict_base - 1);
        ctx->m_scratch_base = trx->ptr_to_offset(scratch_base);
        ctx->m_scratch_ptr = ctx->m_scratch_base;
        ctx->m_scratch_limit = trx->ptr_to_offset(scratch_base + trx->m_scratch_depth);

        // Move parameters from launcher's op stack to coroutine's op stack
        auto src = mark_ptr + 1;  // first param (above mark)
        for (length_t i = 0; i < param_count; ++i) {
            op_base[i] = src[i];
        }
        ctx->m_op_ptr = trx->ptr_to_offset(op_base + param_count);

        // Init exec stack: push @coroutine-complete then proc
        exec_base[0] = Object::make_control_operator(SystemName::atCoroutineComplete);
        exec_base[1] = proc_obj;
        ctx->m_exec_ptr = trx->ptr_to_offset(exec_base + 2);

        // proc_obj now lives on the new coroutine's (registry-reachable) exec stack;
        // drop the temporary root.
        trx->gc_root_pop_oneoff();

        // Init error stack: empty
        ctx->m_err_ptr = trx->ptr_to_offset(err_base);

        // Init dict stack: [systemdict, protocoldict, userdict]
        dict_base[0] = Object::make_dict(trx->ptr_to_offset(trx->m_systemdict));
        dict_base[1] = Object::make_dict(trx->ptr_to_offset(trx->m_protocoldict));
        dict_base[2] = Object::make_dict(trx->ptr_to_offset(trx->m_userdict));
        ctx->m_dict_ptr = trx->ptr_to_offset(dict_base + PermanentDictCount);

        ctx->m_scanner_stream = nullptr;

        // Pop mark and parameters from launcher's operand stack
        trx->m_op_ptr = mark_ptr - 1;

        // (Registry insert moved above, before coroutine_alloc_stacks, so the
        // context is GC-reachable across that allocation.)

        // Push to ready queue for scheduling
        trx->ready_queue_push(ctx);
        trx->coroutine_count_inc();

        return {ctx, ctx_offset};
    }
}

// launch: mark obj* proc -- coroutine
// Creates a new coroutine.  Objects between the mark and proc are moved to the
// coroutine's operand stack.  Returns a coroutine handle.
// throws: execstack-overflow, opstack-underflow, type-check, unmatched-mark, vm-full
static void launch_op(Trix *trx) {
    auto [_, ctx_offset] = coroutine_launch_common(trx, "coroutine-launch");
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_coroutine(ctx_offset);
}

// sleep: int -- (modified for coroutine support)
// If coroutines are active (live_coroutine_count > 0), cooperatively yields to scheduler.
// N is minimum ms before re-eligible.
// If no coroutines, sleeps the real thread (original behavior).
// throws: opstack-underflow, range-check, type-check
static void sleep_op(Trix *trx) {
    trx->verify_operands(VerifyInteger | VerifyNotNegative);

    auto ms = trx->m_op_ptr->integer_value();
    --trx->m_op_ptr;

    auto granted_ms{trx->sleep_budget_grant(static_cast<uint64_t>(ms))};

    if (trx->m_live_coroutine_count > 0) {
        // Coroutine mode: yield to scheduler
        if (granted_ms == 0) {
            // Pure yield (ms == 0, or sleep budget spent): go to ready queue
            // tail (behind all currently-ready coroutines)
            trx->coroutine_flush_running();
            auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
            ctx->m_status = CoroutineContext::Ready;
            trx->ready_queue_push(ctx);
            trx->coroutine_schedule();
        } else {
            auto wake_time = monotonic_ns() + granted_ms * 1'000'000ULL;
            trx->coroutine_flush_running();
            auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
            ctx->m_status = CoroutineContext::Sleeping;
            ctx->m_wake_time_ns = wake_time;
            trx->timer_list_insert(ctx);
            trx->coroutine_schedule();
        }
    } else if (granted_ms > 0) {
        // No coroutines: sleep the real thread (original behavior)
        timespec ts{};
        ts.tv_sec = static_cast<time_t>(granted_ms / 1000);
        ts.tv_nsec = static_cast<long>((granted_ms % 1000) * 1'000'000ULL);
        ::nanosleep(&ts, nullptr);
    }
}

// die: --
// Self-terminate the currently running coroutine.  Main (coroutine #0) cannot die.
// throws: invalid-exit
static void die_op(Trix *trx) {
    if (trx->m_running_coroutine == trx->m_main_context) {
        trx->error(Error::InvalidExit, "coroutine-die: main coroutine cannot die");
    } else {
        trx->coroutine_flush_running();
        auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
        trx->coroutine_kill(ctx, trx->wellknown_name(WellKnownName::Normal));
        trx->coroutine_schedule();
    }
}

// kill: coroutine --
// Kill another coroutine by handle.
// throws: invalid-exit, opstack-underflow, type-check
static void kill_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();
    --trx->m_op_ptr;

    if (target_offset == trx->m_running_coroutine) {
        trx->error(Error::InvalidExit, "coroutine-kill: cannot kill self (use coroutine-die)");
    } else if (target_offset == trx->m_main_context) {
        trx->error(Error::InvalidExit, "coroutine-kill: cannot kill main coroutine");
    } else {
        auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
        if (target->m_status != CoroutineContext::Dead) {
            trx->coroutine_kill(target, trx->wellknown_name(WellKnownName::Killed));
        }
    }
}

// suspend: coroutine --
// Suspend a coroutine.  Removes it from the ready queue or timer list.
// If the coroutine was in a timed sleep, preserves the remaining delta.
// No-op if already suspended.  Error on Dead, Running (self), or main.
// throws: invalid-access, invalid-exit, opstack-underflow, type-check
static void suspend_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();
    --trx->m_op_ptr;

    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

    if (target->m_status == CoroutineContext::Dead) {
        trx->error(Error::InvalidAccess, "coroutine-suspend: coroutine is dead");
    } else if (target_offset == trx->m_running_coroutine) {
        trx->error(Error::InvalidExit, "coroutine-suspend: cannot suspend self");
    } else if (target_offset == trx->m_main_context) {
        trx->error(Error::InvalidExit, "coroutine-suspend: cannot suspend main coroutine");
    } else if (target->m_flags & CoroutineContext::FlagSuspended) {
        // Already suspended -- no-op
    } else if (target->m_status == CoroutineContext::Ready) {
        trx->ready_queue_remove(target);
        target->m_flags |= CoroutineContext::FlagSuspended;
        target->m_suspend_remaining_ns = 0;
    } else if (target->m_status == CoroutineContext::Sleeping) {
        if (target->m_flags & CoroutineContext::FlagBlocked) {
            // Blocked on producer (pipe/actor/supervision/join) -- not in timer list
            target->m_flags |= CoroutineContext::FlagSuspended;
            target->m_suspend_remaining_ns = 0;
        } else {
            // Timed sleep -- in timer list
            trx->timer_list_remove(target);
            auto now = trx->monotonic_ns();
            if (target->m_wake_time_ns > now) {
                target->m_suspend_remaining_ns = target->m_wake_time_ns - now;
            } else {
                target->m_suspend_remaining_ns = 0;
            }
            target->m_flags |= CoroutineContext::FlagSuspended;
        }
    }
}

// resume: coroutine --
// Resume a suspended coroutine.  If it was Ready, pushes to ready queue.
// If it had a remaining timer delta, reinserts into timer list with adjusted
// wake time.  No-op if not suspended.  Error on Dead.
// throws: invalid-access, opstack-underflow, type-check
static void resume_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();
    --trx->m_op_ptr;

    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

    if (target->m_status == CoroutineContext::Dead) {
        trx->error(Error::InvalidAccess, "coroutine-resume: coroutine is dead");
    } else if (!(target->m_flags & CoroutineContext::FlagSuspended)) {
        // Not suspended -- no-op
    } else {
        target->m_flags &= ~CoroutineContext::FlagSuspended;
        if (target->m_status == CoroutineContext::Ready) {
            // Was ready (or woken by producer while suspended) -- push to ready queue
            trx->ready_queue_push(target);
        } else if (target->m_status == CoroutineContext::Sleeping) {
            if (target->m_flags & CoroutineContext::FlagBlocked) {
                // Still blocked on producer -- leave as Sleeping+Blocked
            } else if (target->m_suspend_remaining_ns == 0) {
                // Timer already expired -- go to ready queue
                target->m_status = CoroutineContext::Ready;
                trx->ready_queue_push(target);
            } else {
                // Reinsert into timer list with adjusted wake time
                target->m_wake_time_ns = trx->monotonic_ns() + target->m_suspend_remaining_ns;
                trx->timer_list_insert(target);
            }
        }
    }
}

// is-coroutine: any -- bool
// Type predicate for coroutines.
// (uses the same template as other type predicates, but defined here for locality)

// coroutine-status: coroutine -- name
// Returns the status of a coroutine as a name: /sleeping, /ready, /running, /dead, /suspended.
// If FlagSuspended is set, returns /suspended regardless of base status.
// throws: opstack-underflow, type-check
static void coroutine_status_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();
    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

    Object status_name;
    if (target->m_flags & CoroutineContext::FlagSuspended) {
        status_name = trx->wellknown_name(WellKnownName::Suspended);
    } else {
        switch (target->m_status) {
        case CoroutineContext::Created:
        case CoroutineContext::Ready:
            status_name = trx->wellknown_name(WellKnownName::Ready);
            break;

        case CoroutineContext::Sleeping:
            status_name = trx->wellknown_name(WellKnownName::Sleeping);
            break;

        case CoroutineContext::Running:
            status_name = trx->wellknown_name(WellKnownName::Running);
            break;

        case CoroutineContext::Dead:
            status_name = trx->wellknown_name(WellKnownName::Dead);
            break;

        default:
            status_name = trx->wellknown_name(WellKnownName::Dead);
            break;
        }
    }

    *trx->m_op_ptr = status_name;
}

// coroutine-id: coroutine -- int
// Returns the sequential ID assigned to a coroutine at launch time.
// Main coroutine is always ID 0.
// throws: opstack-underflow, type-check
static void coroutine_id_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);
    auto ctx = trx->m_op_ptr->coroutine_context(trx);
    *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(ctx->m_id));
}

// coroutine-by-id: int -- coroutine
// Looks up a live coroutine by its sequential ID.  Raises /undefined if
// no live coroutine has that ID.
// throws: opstack-underflow, type-check, undefined
static void coroutine_by_id_op(Trix *trx) {
    trx->verify_operands(VerifyInteger | VerifyNotNegative);
    auto id = static_cast<uint32_t>(trx->m_op_ptr->integer_value());
    auto start = trx->m_coroutine_head;
    auto curr = start;
    do {
        auto ctx = trx->offset_to_ptr<CoroutineContext>(curr);
        if ((ctx->m_id == id) && (ctx->m_status != CoroutineContext::Dead)) {
            *trx->m_op_ptr = Object::make_coroutine(curr);
            return;
        } else {
            curr = ctx->m_next;
        }
    } while (curr != start);
    trx->error(Error::Undefined, "'coroutine-by-id': no live coroutine with id {}", id);
}

// coroutine-self: -- coroutine
// Pushes the currently running coroutine's handle onto the operand stack.
// throws: opstack-overflow
static void coroutine_self_op(Trix *trx) {
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_coroutine(trx->m_running_coroutine);
}

// coroutine-quantum: int --
// Set the running coroutine's time quantum (operations per scheduling slice).
// 0 = unlimited (no auto-yield, cooperative-only scheduling).
// Non-zero = auto-yield after N operations, preventing starvation.
// throws: opstack-underflow, type-check, range-check
static void coroutine_quantum_op(Trix *trx) {
    trx->verify_operands(VerifyInteger | VerifyNotNegative);
    auto quantum = trx->m_op_ptr->integer_value();
    --trx->m_op_ptr;
    auto q = static_cast<uint32_t>(quantum);
    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    ctx->m_quantum = q;
    ctx->m_ops_remaining = q;
    trx->m_coroutine_ops_remaining = q;
}

// coroutine-priority: coroutine int --
// Set a coroutine's scheduling priority.  0 = normal, 1 = high.
// High-priority coroutines are always scheduled before normal-priority ones.
// If the target is currently in the ready queue, it is moved to the correct queue.
// throws: opstack-underflow, type-check, range-check
static void coroutine_priority_op(Trix *trx) {
    trx->verify_operands(VerifyInteger);
    auto prio = trx->m_op_ptr->integer_value();
    --trx->m_op_ptr;
    trx->verify_operands(VerifyCoroutine);
    auto target_offset = trx->m_op_ptr->coroutine_offset();
    --trx->m_op_ptr;

    if ((prio < 0) || (prio > 1)) {
        trx->error(Error::RangeCheck, "coroutine-priority: priority must be 0 (normal) or 1 (high), got {}", prio);
    } else {
        auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
        if (target->m_status == CoroutineContext::Dead) {
            trx->error(Error::InvalidAccess, "coroutine-priority: coroutine is dead");
        } else {
            auto new_prio = static_cast<uint8_t>(prio);
            if (new_prio != target->m_priority) {
                // If in ready queue, remove from old and re-insert into new
                if ((target->m_status == CoroutineContext::Ready) && !(target->m_flags & CoroutineContext::FlagSuspended) &&
                    (target_offset != trx->m_running_coroutine)) {
                    trx->ready_queue_remove(target);
                    target->m_priority = new_prio;
                    trx->ready_queue_push(target);
                } else {
                    target->m_priority = new_prio;
                }
            }
        }
    }
}

// coroutine-kill-all: --
// Kill all non-main coroutines.  Used for cleanup between tests or before exit.
// throws: (none)
static void coroutine_kill_all_op(Trix *trx) {
    trx->coroutine_kill_all();
}

// @coroutine-complete: (control operator)
// Reached when a coroutine's launch proc returns normally.
// For main: kill all remaining coroutines and let the interpreter exit.
// For others: kill this coroutine and schedule the next one.
static void at_coroutine_complete_op(Trix *trx) {
    if (trx->m_running_coroutine == trx->m_main_context) {
        // Main finished -- kill all remaining coroutines
        trx->coroutine_kill_all();
        // Let the interpreter loop exit normally (exec stack is empty)
    } else {
        // Non-main coroutine finished
        trx->coroutine_flush_running();
        auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
        trx->coroutine_kill(ctx, trx->wellknown_name(WellKnownName::Normal));
        trx->coroutine_schedule();
    }
}

// @coroutine-join-check: (control operator)
// Resumes after a join-sleep.  Checks if the target coroutine (stored on op stack) is dead.
// If dead: pushes return value + flag.  If still alive: re-sleeps.
static void at_coroutine_join_check_op(Trix *trx) {
    // The coroutine Object is on top of the operand stack (put there by coroutine-join)
    auto target_offset = trx->m_op_ptr->coroutine_offset();
    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

    if (target->m_status != CoroutineContext::Dead) {
        // Still alive -- re-sleep until joiner wakes us
        target->m_joiner = trx->m_running_coroutine;
        trx->coroutine_sleep_and_schedule(SystemName::atCoroutineJoinCheck, std::numeric_limits<uint64_t>::max());
    } else {
        // Target is dead -- replace coroutine handle with return value + flag
        *trx->m_op_ptr = target->m_has_return ? target->m_return_value : Object::make_null();
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_boolean(target->m_has_return);
        trx->capture_joined_exit_reason(target);
        trx->coroutine_free_context(target);
    }
}

// coroutine-join: coroutine -- value bool
// Blocks until the target coroutine dies, then pushes its return value and a flag
// indicating whether a return value was available.
// throws: invalid-exit, opstack-underflow, type-check
static void coroutine_join_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();

    if (target_offset == trx->m_running_coroutine) {
        trx->error(Error::InvalidExit, "coroutine-join: cannot join self");
    } else {
        auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
        if (target->m_status == CoroutineContext::Dead) {
            // Already dead -- push result immediately
            *trx->m_op_ptr = target->m_has_return ? target->m_return_value : Object::make_null();
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = Object::make_boolean(target->m_has_return);
            trx->capture_joined_exit_reason(target);
            trx->coroutine_free_context(target);
        } else {
            // Target is alive -- leave coroutine handle on op stack for the check op,
            // sleep until joiner wakes us.
            if (target->m_joiner != nulloffset) {
                trx->error(Error::InvalidAccess, "coroutine-join: another coroutine is already joining this target");
            } else {
                target->m_joiner = trx->m_running_coroutine;
                trx->coroutine_sleep_and_schedule(SystemName::atCoroutineJoinCheck, std::numeric_limits<uint64_t>::max());
            }
        }
    }
}

// @coroutine-await-check: (control operator)
// Resumes after an await-sleep.  Same trampoline as @coroutine-join-check
// except the dead-target outcome is "push value or rethrow", not "push
// value + bool flag".
//
// On dead target:
//   m_has_return        -> push m_return_value
//   m_exit_reason /normal && !m_has_return -> push null (graceful coroutine-die)
//   otherwise           -> rethrow m_exit_reason (error / kill / shutdown)
static void at_coroutine_await_check_op(Trix *trx) {
    auto target_offset = trx->m_op_ptr->coroutine_offset();
    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

    if (target->m_status != CoroutineContext::Dead) {
        target->m_joiner = trx->m_running_coroutine;
        trx->coroutine_sleep_and_schedule(SystemName::atCoroutineAwaitCheck, std::numeric_limits<uint64_t>::max());
    } else if (target->m_has_return) {
        *trx->m_op_ptr = target->m_return_value;
        trx->capture_joined_exit_reason(target);
        trx->coroutine_free_context(target);
    } else if (target->m_exit_reason.is_name() &&
               (target->m_exit_reason.name_offset() == trx->wellknown_offset(WellKnownName::Normal))) {
        // Graceful coroutine-die: no value, no error -- normalize to null.
        *trx->m_op_ptr = Object::make_null();
        trx->capture_joined_exit_reason(target);
        trx->coroutine_free_context(target);
    } else {
        // Abnormal exit -- discard the coroutine handle and rethrow.
        --trx->m_op_ptr;
        trx->rethrow_coroutine_exit_reason(target);
    }
}

// coroutine-await: coroutine -- value
// Blocks until the target coroutine dies and either pushes its return value
// (on normal exit) or rethrows the same error name in the awaiter's
// context (on abnormal exit).  Use coroutine-join when you want to observe
// without inheriting the failure (the dual of Erlang `monitor`); use
// coroutine-await when you want the link semantics ("if it dies, I die",
// modulo a wrapping try / try-catch).
//
// Mapping for dead target:
//   normal exit (return value present) -> push that value
//   coroutine-die / no-return /normal  -> push null
//   /killed, /shutdown                 -> rethrow as user-error /killed or /shutdown
//   uncaught error                     -> rethrow with original error name (matches handler dicts)
//
// throws: invalid-access (already being joined), invalid-exit (await-self),
//         opstack-underflow, type-check,
//         <target error> (passthrough on abnormal target exit)
static void coroutine_await_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();
    if (target_offset == trx->m_running_coroutine) {
        trx->error(Error::InvalidExit, "coroutine-await: cannot await self");
    } else {
        auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
        if (target->m_status != CoroutineContext::Dead) {
            if (target->m_joiner != nulloffset) {
                trx->error(Error::InvalidAccess, "coroutine-await: another coroutine is already joining this target");
            } else {
                target->m_joiner = trx->m_running_coroutine;
                trx->coroutine_sleep_and_schedule(SystemName::atCoroutineAwaitCheck, std::numeric_limits<uint64_t>::max());
            }
        } else if (target->m_has_return) {
            *trx->m_op_ptr = target->m_return_value;
            trx->capture_joined_exit_reason(target);
            trx->coroutine_free_context(target);
        } else if (target->m_exit_reason.is_name() &&
                   (target->m_exit_reason.name_offset() == trx->wellknown_offset(WellKnownName::Normal))) {
            *trx->m_op_ptr = Object::make_null();
            trx->capture_joined_exit_reason(target);
            trx->coroutine_free_context(target);
        } else {
            --trx->m_op_ptr;
            trx->rethrow_coroutine_exit_reason(target);
        }
    }
}

// rethrow_coroutine_exit_reason: handle the abnormal-exit branch of
// coroutine-await.  Captures the exit reason, frees the target, then
// rethrows the captured Name in the awaiter's context so any wrapping
// try / try-catch catches it as if the error had originated locally.
// Known error names (e.g. /div-by-zero) rethrow with the matching Error
// enum so existing handler dicts match exactly.  User-thrown / non-error
// names (/killed, /shutdown, /my-custom-name) rethrow as Error::UserError
// carrying the original Name in *m_last_error_name_ptr.
//
// throw-with data is NOT preserved across the rethrow (the dying
// coroutine's PendingErrorData was consumed by its global_handler before
// the joiner ever resumed); a follow-up coroutine-last-error-data op can
// be added if a use case appears.
[[noreturn]] void rethrow_coroutine_exit_reason(CoroutineContext *target) {
    auto trx = this;
    auto reason = target->m_exit_reason;
    capture_joined_exit_reason(target);
    coroutine_free_context(target);
    auto [valid, err] = is_error_name(&reason);
    if (valid) {
        trx->error(err, "coroutine-await: rethrow {}", reason.name_sv(trx));
    } else {
        *trx->m_last_error_name_ptr = reason;
        trx->error(Error::UserError, "coroutine-await: rethrow {}", reason.name_sv(trx));
    }
}

// Capture target->m_exit_reason into the running coroutine's
// m_last_joined_exit_reason cache, with /normal mapped to /no-error so the
// joiner sees a single sentinel for clean outcomes.  Called from
// coroutine-join, coroutine-await, and coroutine-release immediately before
// the target context is freed.  Pairs with coroutine-last-error.
void capture_joined_exit_reason(CoroutineContext *target) {
    auto trx = this;
    auto joiner = offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    auto reason = target->m_exit_reason;
    if (reason.is_name() && (reason.name_offset() == trx->wellknown_offset(WellKnownName::Normal))) {
        reason = trx->wellknown_name(WellKnownName::NoError);
    }
    joiner->m_last_joined_exit_reason = reason;
}

// coroutine-last-error: -- /name
// Side-channel diagnostic accessor.  Returns the exit reason of the most
// recently joined/awaited/released coroutine in the calling context, or
// /no-error if no coroutine has been observed yet (or all observed
// coroutines exited normally).  Read-only, non-blocking; does not consume
// or modify any coroutine.
//
// Mapping from observed m_exit_reason:
//   /normal           -> /no-error  (clean proc return)
//   /killed           -> /killed    (peer coroutine-kill)
//   /shutdown         -> /shutdown  (supervisor terminate)
//   <other Name>      -> <Name>     (uncaught error: name passes through)
//
// Typical pattern paired with coroutine-join:
//
//   game-actor coroutine-join not {
//       coroutine-last-error
//       (game-actor died with: ) print = flush
//   } if
//
// throws: opstack-overflow
static void coroutine_last_error_op(Trix *trx) {
    auto running = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = running->m_last_joined_exit_reason;
}

// @coroutine-wait-all-check: (control operator)
// Resumes after a wait-all-sleep.  Checks if all coroutines are done.
// If not: re-sleeps and re-pushes the check.
static void at_coroutine_wait_all_check_op(Trix *trx) {
    // The caller is itself counted in m_live_coroutine_count unless it is main,
    // so it must wait until everyone *else* is gone, not until the count hits 0
    // (which a non-main caller can never reach -> infinite busy-loop).
    auto self_slot = static_cast<length_t>((trx->m_running_coroutine != trx->m_main_context) ? 1 : 0);
    if (trx->m_live_coroutine_count > self_slot) {
        trx->coroutine_sleep_and_schedule(SystemName::atCoroutineWaitAllCheck, 0);
    }
}

// coroutine-wait-all: --
// Blocks the current coroutine until all other coroutines have completed.
// throws: (none beyond internal errors)
static void coroutine_wait_all_op(Trix *trx) {
    auto self_slot = static_cast<length_t>((trx->m_running_coroutine != trx->m_main_context) ? 1 : 0);
    if (trx->m_live_coroutine_count > self_slot) {
        trx->coroutine_sleep_and_schedule(SystemName::atCoroutineWaitAllCheck, 0);
    }
}

// coroutine-release: coroutine --
// Discards a dead coroutine's result without reading it.  Frees the context.
// throws: invalid-exit, opstack-underflow, type-check
static void coroutine_release_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();
    --trx->m_op_ptr;

    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
    if (target->m_status != CoroutineContext::Dead) {
        trx->error(Error::InvalidExit, "coroutine-release: coroutine is not dead");
    } else {
        // Free the return value's ExtValue/WideValue if present
        if (target->m_has_return) {
            if (target->m_return_value.uses_extvalue()) {
                target->m_return_value.free_extvalue(trx);
            } else if (target->m_return_value.uses_widevalue()) {
                target->m_return_value.free_widevalue(trx);
            }
        }

        trx->capture_joined_exit_reason(target);
        trx->coroutine_free_context(target);
    }
}

//===--- GC walkers ---===//
//
// gc_walk_saved_region(base_off, ptr_off): mark every Object in a SUSPENDED
// coroutine context's saved stack region [base_off, ptr_off).  Distinct from
// gc.inl's walk_object_range (which takes LIVE pointers and walks the INCLUSIVE
// [base..ptr] of a RUNNING coroutine's hot stack): the saved ptr_off is the
// "one past last live" form stored by coroutine_flush_running (ptr_to_offset of
// m_*_ptr + 1), so iteration is HALF-OPEN and an empty stack has
// ptr_off == base_off (zero slots).  Called for each of the op / exec / err /
// dict / scratch regions by gc_walk_coroutine_context.
//
void gc_walk_saved_region(vm_offset_t base_off, vm_offset_t ptr_off) {
    auto *base = offset_to_ptr<Object>(base_off);
    auto *limit = offset_to_ptr<Object>(ptr_off);
    for (auto *p = base; p < limit; ++p) {
        gc_mark_object(*p);
    }
}

// scan_range_lowest: over [base, top] (inclusive), find the lowest local-VM
// ExtValue (want_wide == false) or WideValue (want_wide == true) whose storage is
// at or above the restore barrier, updating *low / *low_off when a lower one is
// found.  Globals are journal-skipped and never relocate, so they are excluded
// (matching Save::check_stack).  Pointer out-params (no captures) so it composes
// across the several stack ranges find_lowest_relocatable visits.
void scan_range_lowest(Object *base, Object *top, vm_offset_t barrier, bool want_wide, Object **low, vm_offset_t *low_off) {
    for (auto *o = base; o <= top; ++o) {
        auto match = want_wide ? o->uses_widevalue() : o->uses_extvalue();
        if (match) {
            auto offset = o->offset();
            if ((offset >= barrier) && (offset < *low_off) && !is_global(offset)) {
                *low = o;
                *low_off = offset;
            }
        }
    }
}

// find_lowest_relocatable: the lowest at-or-above-barrier local ExtValue/WideValue
// across the RUNNING coroutine's op/exec stacks AND every SUSPENDED coroutine that
// ran since the save (its saved op/exec/err stacks).  ExtValue/WideValue::restore
// call this repeatedly to relocate such scalars below the barrier in ascending-offset
// order (ascending so a relocation's freed slot can back the next alloc without
// overwriting an un-relocated one).  Previously only the running coroutine's scalars
// were relocated; a suspended coroutine's were neither relocated nor -- being scalars
// -- rejected by check_stack, so they dangled on restore (the B7(b)-class hole).
// check_stack still rejects above-barrier COMPOSITES everywhere (those cannot be
// relocated); this moves the scalars.  Saved m_*_ptr is one-past-top (coroutine_save),
// so the suspended ranges end at ptr-1; the running m_op_ptr points at the live top
// (the save token was already popped before restore reaches relocation).  The filter
// (non-running, non-dead, m_activation_sl >= restore_level) matches the check_stack
// suspended scan so the relocation count agrees.  Returns nullptr when none remain.
[[nodiscard]] Object *find_lowest_relocatable(save_level_t restore_level, bool want_wide) {
    auto barrier = ptr_to_offset(m_vm_ptr);
    Object *low = nullptr;
    auto low_off = std::numeric_limits<vm_offset_t>::max();

    scan_range_lowest(m_op_base, m_op_ptr, barrier, want_wide, &low, &low_off);
    scan_range_lowest(m_exec_base, m_exec_ptr, barrier, want_wide, &low, &low_off);

    if (m_live_coroutine_count > 0) {
        auto curr_offset = m_coroutine_head;
        do {
            auto *curr = offset_to_ptr<CoroutineContext>(curr_offset);
            if ((curr_offset != m_running_coroutine) && (curr->m_status != CoroutineContext::Dead) &&
                (curr->m_activation_sl >= restore_level)) {
                scan_range_lowest(offset_to_ptr<Object>(curr->m_op_base),
                                  offset_to_ptr<Object>(curr->m_op_ptr) - 1,
                                  barrier,
                                  want_wide,
                                  &low,
                                  &low_off);
                scan_range_lowest(offset_to_ptr<Object>(curr->m_exec_base),
                                  offset_to_ptr<Object>(curr->m_exec_ptr) - 1,
                                  barrier,
                                  want_wide,
                                  &low,
                                  &low_off);
                scan_range_lowest(offset_to_ptr<Object>(curr->m_err_base),
                                  offset_to_ptr<Object>(curr->m_err_ptr) - 1,
                                  barrier,
                                  want_wide,
                                  &low,
                                  &low_off);
            }
            curr_offset = curr->m_next;
        } while (curr_offset != m_coroutine_head);
    }
    return low;
}

//
// gc_walk_coroutine_context: marks per-coroutine subsystem block
// offsets (mailbox / monitor lists / binding table / stacks block /
// debug-name) and the few Object-cell fields stored on the context
// itself (return value + exit reasons).
//
// Subsystem blocks are marked but NOT walked here -- the work-queue
// drain pops them and dispatches into their own walkers (gc_walk_
// mailbox, gc_walk_monitor, gc_walk_pipebuffer, etc.).  Same for the
// stacks block: marking m_stacks_offset enqueues it; when popped, the
// Coroutine kind dispatch (which uses payload-size to tell stacks
// from context) routes it to gc_walk_object_array, walking every
// stack slot.
//
// CoroutineContext fields NOT marked here:
//   * m_next, m_ready_next, m_ready_prev, m_timer_next,
//     m_blocked_sender_next, m_joiner: links to other coroutines.
//     Reached via the coroutine-list root walk in step 7.
//   * m_op_base..m_op_ptr (and other stack pointers): live state of
//     the running coroutine lives in the trx's hot fields, not on
//     the context block; for SUSPENDED coroutines, these are
//     offsets INTO the stacks block, walked by gc_walk_object_array
//     when the stacks block is popped from the work queue.
//   * m_scanner_stream: C++ pointer, not a vm_offset_t.
//
void gc_walk_coroutine_context(vm_offset_t payload_offset) {
    auto *ctx = offset_to_ptr<CoroutineContext>(payload_offset);

    gc_mark_object(ctx->m_return_value);
    gc_mark_object(ctx->m_exit_reason);
    gc_mark_object(ctx->m_last_joined_exit_reason);

    // Walk the stacks block: mark the block itself + walk only the
    // user-active region of each per-coroutine stack region.  walk_block_contents'
    // Coroutine >512 arm is a no-op for stacks blocks; the active-region
    // walk lives here so we know the per-region tip pointers.
    //
    // For the RUNNING coroutine, walk_all_roots section 1 has already
    // walked the live trix-fields (m_op_base..m_op_ptr etc.); ctx fields
    // are stale (last sync at suspend) so we MUST NOT walk them here for
    // the running coroutine -- doing so would walk old state and miss
    // the current top-of-stack.  Skip and let section 1 cover it.
    //
    // For SUSPENDED coroutines, ctx fields are authoritative (synced at
    // last suspend via coroutine_flush_running).  ctx->m_*_ptr stores
    // the "one past last live" form (base-relative offset of m_*_ptr+1);
    // walk [base_offset .. ptr_offset) using gc_mark_object on each Object.
    // Empty stacks have ptr_offset == base_offset and walk zero slots.
    // The scratch arena uses the same half-open form ([base, ptr)) so
    // shares the same loop.
    //
    // Main (m_main_context) uses the PRIMORDIAL VM stacks, so its m_stacks_offset is
    // nulloffset even while its saved stacks are live -- it must therefore be walked
    // explicitly when suspended (a coroutine is running).  Otherwise a global block
    // parked on main's operand stack across that coroutine's GC is swept, surfacing
    // as a use-after-free / double-free.  A DEAD coroutine also has
    // m_stacks_offset == nulloffset, but its
    // stacks were freed -- the m_main_context check excludes those (main never dies).
    mark_global_offset(ctx->m_stacks_offset);
    if (((ctx->m_stacks_offset != nulloffset) || (payload_offset == m_main_context)) && (payload_offset != m_running_coroutine)) {
        gc_walk_saved_region(ctx->m_op_base, ctx->m_op_ptr);
        gc_walk_saved_region(ctx->m_exec_base, ctx->m_exec_ptr);
        gc_walk_saved_region(ctx->m_err_base, ctx->m_err_ptr);
        gc_walk_saved_region(ctx->m_dict_base, ctx->m_dict_ptr);
        gc_walk_saved_region(ctx->m_scratch_base, ctx->m_scratch_ptr);
    }

    mark_global_offset(ctx->m_debug_name);
    mark_global_offset(ctx->m_mailbox);
    mark_global_offset(ctx->m_monitors);
    mark_global_offset(ctx->m_monitoring);
    mark_global_offset(ctx->m_binding_table);

    // A supervisor's SupervisorState lives in global VM but is referenced only as
    // an integer offset in m_return_value; mark it (gated on FlagSupervisor) so
    // gc_walk_supervisor runs and the state + its child handles survive
    // vm_global_gc.  The flag is dropped on death, after which the state is
    // unreachable and reclaimable (accessor ops reject a dead supervisor).
    if (((ctx->m_flags & CoroutineContext::FlagSupervisor) != 0) && ctx->m_return_value.is_integer()) {
        mark_global_offset(static_cast<vm_offset_t>(ctx->m_return_value.integer_value()));
    }
}

//
// gc_walk_mailbox: walk the live messages in a mailbox's circular
// buffer, marking each Object.  Bails out for parked mailboxes
// (m_head == MailboxRecycleSentinel) so stale free-list bytes don't
// get interpreted as messages.
//
void gc_walk_mailbox(vm_offset_t payload_offset) {
    auto *mbx = offset_to_ptr<MailboxHeader>(payload_offset);
    if ((mbx->m_head != MailboxRecycleSentinel) && (mbx->m_capacity != 0)) {
        auto *messages = offset_to_ptr<Object>(static_cast<vm_offset_t>(payload_offset + sizeof(MailboxHeader)));
        for (length_t i = 0; i < mbx->m_count; ++i) {
            auto idx = static_cast<length_t>((mbx->m_head + i) % mbx->m_capacity);
            gc_mark_object(messages[idx]);
        }
    }
}

//
// gc_walk_monitor: each MonitorEntry participates in two linked
// lists.  The walker marks both forward links (m_next_target,
// m_next_source) so the entire monitor / monitoring graph stays
// reachable from a single root entry.  m_source / m_target point
// at coroutines reached via the coroutine-list root walk; defensive
// marking here is redundant but cheap (cycle break short-circuits).
//
void gc_walk_monitor(vm_offset_t payload_offset) {
    auto *entry = offset_to_ptr<MonitorEntry>(payload_offset);
    mark_global_offset(entry->m_next_target);
    mark_global_offset(entry->m_next_source);
    mark_global_offset(entry->m_source);
    mark_global_offset(entry->m_target);
}
