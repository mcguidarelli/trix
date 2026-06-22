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

//===--- Actor Operators ---===//
//
// Implements the Actor model of concurrent computation.  Based on:
//
//   Carl Hewitt, Peter Bishop, Richard Steiger, "A Universal Modular ACTOR
//   Formalism for Artificial Intelligence", IJCAI 1973.  (Original actor
//   model: independent agents communicating solely via asynchronous messages.)
//
//   Joe Armstrong, "Making reliable distributed systems in the presence of
//   software errors", PhD thesis, 2003.  (Erlang/OTP: practical actors with
//   mailboxes, selective receive, process isolation, and fault tolerance.)
//
// The specific design follows Erlang's model: actors are lightweight
// processes (coroutines) with per-actor mailboxes, asynchronous send,
// blocking receive, and selective receive with pattern matching.
//
// --- Core concepts for maintainers ---
//
// ACTORS
//   An actor is a coroutine with an attached mailbox.  Created with
//   `actor-spawn` (default mailbox capacity) or `actor-spawn-capacity`
//   (explicit capacity).  Actors communicate exclusively through message
//   passing -- there is no shared mutable state between actors.
//
//   Any Trix Object can be a message: integers, strings, arrays, dicts,
//   tagged values, records, etc.  Messages are moved (not cloned) from
//   the sender's operand stack into the mailbox -- ownership transfers.
//
// MAILBOXES
//   Each actor has a bounded circular buffer (MailboxHeader) allocated on
//   the VM heap.  The mailbox is many-to-one: any coroutine can send, but
//   only the owning actor can receive.
//
//   When the mailbox is full, `actor-send` blocks the sender.  Multiple
//   blocked senders are maintained in a linked list through
//   CoroutineContext::m_blocked_sender_next.  When a slot is freed, the
//   head sender is woken for retry via @actor-send-retry.
//
//   When the mailbox is empty, `actor-recv` blocks the receiver until a
//   message arrives.  The sender wakes the receiver directly via targeted
//   wakeup -- O(1), no scheduler scan.
//
// SEND SEMANTICS
//   `actor-send` is asynchronous and fire-and-forget:
//     - If the target is dead, the message is silently dropped (and its
//       ExtValue freed if any).  No error.
//     - If the mailbox has space, the message is enqueued immediately.
//     - If the mailbox is full, the sender blocks until space is available.
//     - A restore-fragile local-VM message (storage allocated above the save
//       barrier) cannot be delivered into the global mailbox: the message's
//       ExtValue is freed and /invalid-access is raised.  BASE-local, global,
//       and pure-value messages pass through.
//   This matches Erlang's send semantics (! operator) except for the
//   blocking-on-full behavior (Erlang mailboxes are unbounded).
//
// RECEIVE SEMANTICS
//   Four receive variants:
//     actor-recv           -- block until a message arrives (FIFO order)
//     actor-recv-timeout   -- block with deadline, return false on timeout
//     actor-recv-match     -- selective receive: scan mailbox for a message
//                             matching a predicate, block if none match
//     actor-recv-match-timeout -- selective receive with a deadline; returns
//                             'msg true' if matched, 'null false' on timeout
//
//   Selective receive (actor-recv-match) is Erlang's core receive mechanism.
//   It evaluates a predicate proc against each message in the mailbox.  The
//   first match is removed and returned; non-matching messages remain in
//   order.  If no message matches, the actor blocks until a new message
//   arrives, then re-scans from the beginning.
//
// DEAD TARGET HANDLING
//   Sending to a dead actor is a silent no-op (message dropped).  This
//   matches Erlang's behavior and prevents cascading failures when actors
//   die.  Monitoring (ops_supervision.inl) provides explicit death
//   notification for actors that need to know.
//
// MAILBOX REPRESENTATION
//   Actors reuse the coroutine type (Type::Coroutine).  The CoroutineContext
//   struct has an m_mailbox field (vm_offset_t) pointing to a MailboxHeader
//   on the VM heap.  Plain coroutines have m_mailbox == nulloffset; actors
//   have a valid mailbox offset.  MailboxHeader is followed immediately by
//   an Object[capacity] data array (same layout as PipeBufferHeader).
//
// --- Operators ---
//
//   actor-spawn              mark obj* proc -- coroutine      Spawn actor (default mailbox)
//   actor-spawn-capacity     mark obj* proc int -- coroutine  Spawn with explicit capacity
//   actor-send               msg coroutine --                 Send message (async, blocks if full)
//   actor-recv               -- msg                           Receive next message (blocks if empty)
//   actor-recv-timeout       int -- msg bool                  Receive with timeout (ms)
//   actor-recv-match         proc -- msg                      Selective receive by predicate
//   is-actor                 any -- bool                      Test if value is an actor
//   actor-mailbox-count      coroutine -- int                 Number of messages in mailbox
//   actor-mailbox-empty?     coroutine -- bool                True if mailbox is empty
//   actor-self               -- coroutine                     Push current actor's handle
//   actor-recv-match-timeout proc int -- msg bool             Selective receive with timeout (ms)
//   actor-mailbox-capacity   coroutine -- int                 Logical capacity of mailbox
//   actor-status             coroutine -- dict                Read-only introspection dict for actor
//   actor-try-send           msg coroutine -- bool            Non-blocking send; false if full/dead
//   actor-broadcast          msg array --                     Best-effort send to each actor in array
//   actor-flush              -- int                           Drain own mailbox, return count
//   actor-set-name           name coroutine --                Attach a name to an actor
//   actor-name               coroutine -- name true | false   Read an actor's name (false if unset)
//
// Control operators (internal, not user-visible):
//   @actor-send-retry          Resume blocked sender
//   @actor-recv-check          Resume blocked receiver
//   @actor-recv-timeout-check  Resume blocked receiver with deadline
//   @actor-recv-match-check    Resume selective receive scan
//   @actor-recv-match-timeout-check  Resume selective-receive scan with deadline
//

//===--- Helpers ---===//

// Get the MailboxHeader data array (Object[capacity] immediately after header).
static Object *mailbox_data(MailboxHeader *mbx) {
    return reinterpret_cast<Object *>(mbx + 1);
}

// Round a requested (logical) mailbox capacity up to the nearest pool size
// class.  Returns {class_index, class_capacity}.  Returns {-1, requested}
// if the request exceeds the largest class -- such mailboxes bypass the
// pool entirely (allocated exact, abandoned on death).
static std::pair<int, length_t> mailbox_class_for_capacity(length_t requested) {
    for (size_t i = 0; i < MaxMailboxPoolSize; ++i) {
        if (requested <= sm_mailbox_pool_classes[i]) {
            return {static_cast<int>(i), sm_mailbox_pool_classes[i]};
        }
    }
    return {-1, requested};
}

// Append the running coroutine to the tail of a mailbox's blocked-sender list.
// Shared by actor_send_op (mailbox-full path) and at_actor_send_retry_op
// (still-full re-block path) so the list discipline lives in one place.
static void mailbox_append_blocked_sender(Trix *trx, MailboxHeader *mbx) {
    auto running = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    running->m_blocked_sender_next = nulloffset;
    if (mbx->m_blocked_sender == nulloffset) {
        mbx->m_blocked_sender = trx->m_running_coroutine;
    } else {
        auto tail = mbx->m_blocked_sender;
        while (trx->offset_to_ptr<CoroutineContext>(tail)->m_blocked_sender_next != nulloffset) {
            tail = trx->offset_to_ptr<CoroutineContext>(tail)->m_blocked_sender_next;
        }
        trx->offset_to_ptr<CoroutineContext>(tail)->m_blocked_sender_next = trx->m_running_coroutine;
    }
}

// Sentinel stored in m_head while a mailbox is parked on the free-list pool.
// Used to detect double-recycle at O(1) (same principle as Dict::RecycleSentinel).
static constexpr length_t MailboxRecycleSentinel{0xFFFF};

// Allocate a MailboxHeader + Object[physical] as a single contiguous block.
// Returns {header_ptr, header_offset}.
//
// Physical capacity is the user's `capacity` rounded up to the nearest size
// class.  Logical capacity (stored in mbx->m_capacity and used by all
// bounds/modulus code) is the user's exact request, so user-visible
// behavior -- backpressure threshold, mailbox-capacity reporting -- matches
// what was requested.  Slots [logical..physical-1] are dead weight: never
// read, never written, modulus skips them.
//
// Pool lookup: consult the bucket for the size class that covers `capacity`.
// On hit: unlink the parked mailbox (same class, physical >= requested),
// reset header fields, set m_capacity to the *new* logical.  On miss:
// bump-allocate a fresh block at the class's physical size.
//
// Requests above the largest class bypass the pool -- allocated exact,
// abandoned on death.
static std::pair<MailboxHeader *, vm_offset_t> actor_alloc_mailbox(Trix *trx, length_t capacity) {
    auto [class_idx, class_cap] = mailbox_class_for_capacity(capacity);
    if (class_idx >= 0) {
        // Pool is class-indexed only.  Mailboxes live in
        // global VM (BASE-immune to save/restore) so the per-save-level
        // dimension that protected against above-barrier reclaim is no
        // longer needed.
        auto pool_idx = static_cast<size_t>(class_idx);
        auto head = trx->m_mailbox_pool[pool_idx];
        if (head != nulloffset) {
            auto mbx = trx->offset_to_ptr<MailboxHeader>(head);
            if (mbx->m_head != MailboxRecycleSentinel) {
                trx->error(Error::InternalError, "actor_alloc_mailbox: recycled mailbox missing sentinel");
            } else {
                trx->m_mailbox_pool[pool_idx] = mbx->m_blocked_sender;  // next-free link
                mbx->m_head = 0;
                mbx->m_tail = 0;
                mbx->m_count = 0;
                mbx->m_capacity = capacity;  // new user's logical cap; physical (class_cap) is already the buffer size
                mbx->m_blocked_reader = nulloffset;
                mbx->m_blocked_sender = nulloffset;
                return {mbx, head};
            }
        }
    }
    auto physical = (class_idx >= 0) ? class_cap : capacity;
    auto total = static_cast<vm_size_t>(sizeof(MailboxHeader) + physical * sizeof(Object));
    // Mailboxes live in global VM (BASE-immune to
    // save/restore).  Tagged ChunkKind::Mailbox for vm-global-info.
    auto [mbx, mbx_offset] = trx->gvm_alloc<MailboxHeader>(total, Trix::ChunkKind::Mailbox);
    mbx->m_head = 0;
    mbx->m_tail = 0;
    mbx->m_count = 0;
    mbx->m_capacity = capacity;  // logical; physical buffer = class_cap >= capacity
    mbx->m_blocked_reader = nulloffset;
    mbx->m_blocked_sender = nulloffset;
    return {mbx, mbx_offset};
}

// Return a dead actor's mailbox to the per-save-level pool for reuse.
// Pool is class-bucketed, so we route by the size class that covers the
// mailbox's current logical capacity -- which equals the original physical
// size the mailbox was allocated at (physical = class_for(any_prior_logical),
// and class mapping is monotonic: any logical that once fit this buffer
// still maps to the same class).  Above-class mailboxes are abandoned.
// Free-list link lives in m_blocked_sender (unused while parked);
// m_head = MailboxRecycleSentinel flags parked state for double-recycle guard.
static void mailbox_recycle(Trix *trx, MailboxHeader *mbx, vm_offset_t mbx_offset) {
    auto [class_idx, _] = mailbox_class_for_capacity(mbx->m_capacity);
    if (class_idx >= 0) {
        if (mbx->m_head == MailboxRecycleSentinel) {
            trx->error(Error::InternalError, "mailbox_recycle: double-recycle at offset {}", mbx_offset);
        } else {
            auto pool_idx = static_cast<size_t>(class_idx);
            mbx->m_blocked_sender = trx->m_mailbox_pool[pool_idx];
            mbx->m_head = MailboxRecycleSentinel;
            trx->m_mailbox_pool[pool_idx] = mbx_offset;
        }
    }
}

// (No restore-time mailbox scrub.  A historical
// mailbox_scrub_above_barrier nulled any m_blocked_reader and unlinked
// any blocked sender whose offset sat at or above the restore barrier,
// guarding a restore-time coroutine kill walk from the era when
// CoroutineContexts lived in local VM.  Contexts now live in global VM
// -- above every local barrier by construction -- and restore kills no
// coroutine, so the offset test matched EVERY parked actor: any
// save/restore between an actor's recv-park and the next send wiped
// the registration and the wakeup was lost forever (ENGINE BUG #12,
// the tetrix --ai-peek wedge: best-placement-peek's internal restore
// wiped the game actor's registration while it awaited the AI reply).
// Death-driven cleanup belongs to coroutine_kill, which runs
// supervision_notify_death and recycles the mailbox.)

//===--- Actor Lifecycle ---===//

// actor-spawn: mark obj* proc -- coroutine
// Launch actor with default-capacity mailbox.
// throws: internal-error, opstack-overflow, opstack-underflow, type-check, unmatched-mark, vm-full
static void actor_spawn_op(Trix *trx) {
    auto [ctx, ctx_offset] = coroutine_launch_common(trx, "actor-spawn");

    // Allocate mailbox (default capacity)
    auto [mbx, mbx_offset] = actor_alloc_mailbox(trx, MaxActorMailboxCapacity);
    // Re-fetch ctx: vm_alloc may have bumped m_vm_ptr but does not relocate
    ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);
    ctx->m_mailbox = mbx_offset;
    ctx->m_flags |= CoroutineContext::FlagWasActor;

    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_coroutine(ctx_offset);
}

// actor-spawn-capacity: mark obj* proc int -- coroutine
// Launch actor with specified mailbox capacity.
// throws: internal-error, opstack-overflow, opstack-underflow, range-check, type-check, unmatched-mark, vm-full
static void actor_spawn_capacity_op(Trix *trx) {
    trx->verify_operands(VerifyInteger);
    auto capacity = trx->m_op_ptr->integer_value();
    if (capacity <= 0) {
        trx->error(Error::RangeCheck, "actor-spawn-capacity: capacity must be positive");
    } else if (capacity > std::numeric_limits<length_t>::max()) {
        trx->error(Error::RangeCheck,
                   "actor-spawn-capacity: capacity {} exceeds maximum {}",
                   capacity,
                   std::numeric_limits<length_t>::max());
    }
    --trx->m_op_ptr;

    auto [ctx, ctx_offset] = coroutine_launch_common(trx, "actor-spawn-capacity");

    auto [mbx, mbx_offset] = actor_alloc_mailbox(trx, static_cast<length_t>(capacity));
    ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);
    ctx->m_mailbox = mbx_offset;
    ctx->m_flags |= CoroutineContext::FlagWasActor;

    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_coroutine(ctx_offset);
}

// is-actor: any -- bool
// True if argument is a coroutine that was ever spawned as an actor.  The
// FlagWasActor bit is set at spawn time and preserved past death, so this
// remains true even after the mailbox has been recycled to the free pool.
// throws: opstack-underflow
static void is_actor_op(Trix *trx) {
    trx->require_op_count(1);
    auto obj = *trx->m_op_ptr;
    auto result = false;
    if (obj.is_coroutine()) {
        auto ctx = obj.coroutine_context(trx);
        result = ((ctx->m_flags & CoroutineContext::FlagWasActor) != 0);
    }
    *trx->m_op_ptr = Object::make_boolean(result);
}

//===--- Message Passing ---===//

// Internal: complete the send (mailbox has space, target alive).
static void actor_send_complete(Trix *trx, MailboxHeader *mbx, Object message) {
    // R6 pointer hygiene: the mailbox is a GLOBAL, journal-skipped subsystem block
    // (actor_alloc_mailbox -> gvm_alloc), so a restore-fragile LOCAL message -- storage
    // above the save barrier -- would be reclaimed by a future restore while the slot
    // lives on, leaving a dangling offset that the parked actor reads as freed memory on
    // resume (the restore-time coroutine scan walks stacks, not mailbox contents).  Reject
    // at the delivery site, mirroring Dict::put's guard for a global container.  The
    // message is owned here (every caller popped/cloned it), so free it before erroring.
    // BASE-local / global / pure-value messages are restore-immune and pass through.
    // actor-try-send pre-screens and drops instead (it has a bool return to signal the
    // drop); the blocking senders (actor-send / @actor-send-retry / actor-broadcast)
    // reach this reject.  The error framework prepends the operator name, so the message
    // stays op-neutral.
    if (Save::is_restore_fragile_local(trx, message)) {
        auto type_sv = Object::type_sv(message.type());
        message.maybe_free_extvalue(trx);
        trx->error(Error::InvalidAccess, "cannot deliver restore-fragile local-VM {} into a global mailbox", type_sv);
    } else {
        auto data = mailbox_data(mbx);
        data[mbx->m_tail] = message;
        mbx->m_tail = static_cast<length_t>((mbx->m_tail + 1) % mbx->m_capacity);
        ++mbx->m_count;

        // Wake blocked receiver if any
        trx->coroutine_wake(mbx->m_blocked_reader);
        mbx->m_blocked_reader = nulloffset;
    }
}

// actor-send: message coroutine --
// Send message to actor's mailbox.  Silent drop if target is dead.
// Blocks if mailbox is full (targeted wakeup or polling).
// throws: execstack-overflow, invalid-access, opstack-underflow, type-check
static void actor_send_op(Trix *trx) {
    trx->require_op_count(2);
    auto target_obj = *trx->m_op_ptr;
    if (target_obj.is_coroutine()) {
        auto target_offset = target_obj.coroutine_offset();
        auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

        // Dead target: silent drop (free message ExtValue if any)
        if (target->m_status == CoroutineContext::Dead) {
            (trx->m_op_ptr - 1)->maybe_free_extvalue(trx);  // message is below target
            trx->m_op_ptr -= 2;
        } else {
            // No mailbox: error
            if (target->m_mailbox == nulloffset) {
                trx->error(Error::InvalidAccess, "actor-send: target coroutine has no mailbox");
            } else {
                auto mbx = trx->offset_to_ptr<MailboxHeader>(target->m_mailbox);

                // Mailbox full: block sender (append to blocked sender list)
                if (mbx->m_count >= mbx->m_capacity) {
                    // Pop target from op stack (saved on exec stack as companion).
                    // Message remains on op stack for retry.
                    --trx->m_op_ptr;
                    // Exec stack: [target-obj] [@actor-send-retry]
                    trx->require_exec_capacity(2);
                    *++trx->m_exec_ptr = target_obj;

                    // Append to tail of blocked sender list
                    mailbox_append_blocked_sender(trx, mbx);

                    trx->coroutine_sleep_and_schedule(SystemName::atActorSendRetry, std::numeric_limits<uint64_t>::max());
                } else {
                    // Space available: complete send
                    auto message = *(trx->m_op_ptr - 1);
                    trx->m_op_ptr -= 2;
                    actor_send_complete(trx, mbx, message);
                }
            }
        }
    } else {
        trx->error(Error::TypeCheck, "actor-send: target must be a coroutine");
    }
}

// @actor-send-retry: retry send after yield when mailbox was full.
// Exec stack on entry: [target-obj] (companion below us)
static void at_actor_send_retry_op(Trix *trx) {
    // Companion: target coroutine object
    auto target_obj = *trx->m_exec_ptr;
    auto target_offset = target_obj.coroutine_offset();
    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

    // Dead target: silent drop (free message ExtValue if any)
    if (target->m_status == CoroutineContext::Dead) {
        --trx->m_exec_ptr;                        // pop companion
        trx->m_op_ptr->maybe_free_extvalue(trx);  // message is on top (target was popped before sleep)
        --trx->m_op_ptr;
    } else {
        auto mbx = trx->offset_to_ptr<MailboxHeader>(target->m_mailbox);

        // We were woken by recv (removed from blocked list).  Try to send.
        if (mbx->m_count < mbx->m_capacity) {
            // Space available: complete send
            --trx->m_exec_ptr;  // pop companion
            auto message = *trx->m_op_ptr;
            --trx->m_op_ptr;
            actor_send_complete(trx, mbx, message);
        } else {
            // Still full: re-append to blocked sender list and sleep
            mailbox_append_blocked_sender(trx, mbx);
            trx->coroutine_sleep_and_schedule(SystemName::atActorSendRetry, std::numeric_limits<uint64_t>::max());
        }
    }
}

// Internal: complete recv (mailbox non-empty).  Pushes message onto op stack.
static void actor_recv_complete(Trix *trx, MailboxHeader *mbx) {
    auto data = mailbox_data(mbx);
    auto message = data[mbx->m_head];
    mbx->m_head = static_cast<length_t>((mbx->m_head + 1) % mbx->m_capacity);
    --mbx->m_count;

    // Wake head blocked sender (one slot freed = one sender can proceed)
    if (mbx->m_blocked_sender != nulloffset) {
        auto head = mbx->m_blocked_sender;
        auto head_ctx = trx->offset_to_ptr<CoroutineContext>(head);
        mbx->m_blocked_sender = head_ctx->m_blocked_sender_next;
        head_ctx->m_blocked_sender_next = nulloffset;
        trx->coroutine_wake(head);
    }

    trx->require_op_capacity(1);
    *++trx->m_op_ptr = message;
}

// actor-recv: -- message
// Receive next message from own mailbox.  Blocks if empty.
// throws: execstack-overflow, invalid-access, opstack-overflow
static void actor_recv_op(Trix *trx) {
    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    if (ctx->m_mailbox == nulloffset) {
        trx->error(Error::InvalidAccess, "actor-recv: current coroutine has no mailbox");
    } else {
        auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);

        if (mbx->m_count > 0) {
            actor_recv_complete(trx, mbx);
        } else {
            // Empty: block until message arrives
            mbx->m_blocked_reader = trx->m_running_coroutine;
            trx->coroutine_sleep_and_schedule(SystemName::atActorRecvCheck, std::numeric_limits<uint64_t>::max());
        }
    }
}

// @actor-recv-check: retry recv after wakeup.
static void at_actor_recv_check_op(Trix *trx) {
    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);

    mbx->m_blocked_reader = nulloffset;

    if (mbx->m_count > 0) {
        actor_recv_complete(trx, mbx);
    } else {
        // Still empty: re-sleep
        mbx->m_blocked_reader = trx->m_running_coroutine;
        trx->coroutine_sleep_and_schedule(SystemName::atActorRecvCheck, std::numeric_limits<uint64_t>::max());
    }
}

// actor-recv-timeout: int -- message bool
// Receive with timeout (ms).  Returns 'message true' or 'null false'.
// throws: execstack-overflow, invalid-access, opstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void actor_recv_timeout_op(Trix *trx) {
    trx->verify_operands(VerifyInteger | VerifyNotNegative);

    auto timeout_ms = trx->m_op_ptr->integer_value();
    --trx->m_op_ptr;

    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    if (ctx->m_mailbox == nulloffset) {
        trx->error(Error::InvalidAccess, "actor-recv-timeout: current coroutine has no mailbox");
    } else {
        auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);

        if (mbx->m_count > 0) {
            actor_recv_complete(trx, mbx);
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = Object::make_boolean(true);
        } else {
            // Empty: block with deadline (budgeted -- see sleep_budget_grant)
            auto deadline_ns = monotonic_ns() + trx->sleep_budget_grant(static_cast<uint64_t>(timeout_ms)) * 1'000'000ULL;

            // Store deadline as a Long on exec stack for the check operator
            mbx->m_blocked_reader = trx->m_running_coroutine;
            trx->require_exec_capacity(2);
            *++trx->m_exec_ptr = Object::make_long(trx, static_cast<int64_t>(deadline_ns));
            trx->coroutine_sleep_and_schedule(SystemName::atActorRecvTimeoutCheck, deadline_ns);
        }
    }
}

// @actor-recv-timeout-check: retry recv-timeout after wakeup, check deadline.
// Exec stack on entry: [deadline-long-obj] (companion below us)
static void at_actor_recv_timeout_check_op(Trix *trx) {
    auto deadline_obj = *trx->m_exec_ptr;
    auto deadline_ns = static_cast<uint64_t>(deadline_obj.long_value(trx));
    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);

    mbx->m_blocked_reader = nulloffset;

    if (mbx->m_count > 0) {
        // Message available: deliver
        deadline_obj.maybe_free_extvalue(trx);
        --trx->m_exec_ptr;  // pop companion
        actor_recv_complete(trx, mbx);
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_boolean(true);
    } else if (monotonic_ns() >= deadline_ns) {
        // Timed out
        deadline_obj.maybe_free_extvalue(trx);
        --trx->m_exec_ptr;  // pop companion
        trx->require_op_capacity(2);
        *++trx->m_op_ptr = Object::make_null();
        *++trx->m_op_ptr = Object::make_boolean(false);
    } else {
        // Still empty and deadline not reached: re-sleep
        mbx->m_blocked_reader = trx->m_running_coroutine;
        trx->coroutine_sleep_and_schedule(SystemName::atActorRecvTimeoutCheck, deadline_ns);
    }
}

// actor-self: -- coroutine
// Push own coroutine handle.  Works in any coroutine.
// throws: opstack-overflow
static void actor_self_op(Trix *trx) {
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_coroutine(trx->m_running_coroutine);
}

//===--- Selective Receive ---===//

// Remove message at logical index from the mailbox (shift subsequent messages down).
static void mailbox_remove_at(MailboxHeader *mbx, length_t logical_idx) {
    auto data = mailbox_data(mbx);
    for (length_t i = logical_idx; (i + 1) < mbx->m_count; ++i) {
        auto dst = static_cast<length_t>((mbx->m_head + i) % mbx->m_capacity);
        auto src = static_cast<length_t>((mbx->m_head + i + 1) % mbx->m_capacity);
        data[dst] = data[src];
    }
    mbx->m_tail = static_cast<length_t>((mbx->m_tail + mbx->m_capacity - 1) % mbx->m_capacity);
    --mbx->m_count;
}

// Push candidate message + pred for evaluation, with @check as the continuation.
// Exec stack: [...] [scan-index] [pred] [@check] [pred]  (pred duplicated for execution)
// Op stack:   ... candidate
static void recv_match_eval_candidate(Trix *trx, MailboxHeader *mbx, Object pred, length_t scan_idx, SystemName check_op) {
    auto data = mailbox_data(mbx);
    auto phys_idx = static_cast<length_t>((mbx->m_head + scan_idx) % mbx->m_capacity);
    // Clone the candidate: the pred may free ExtValues (e.g. Long comparison),
    // and a raw copy would share the mailbox entry's ExtValue, causing a
    // double-free when the mailbox is cleaned up on actor death.
    auto candidate = data[phys_idx].make_clone(trx);

    // Update scan index on exec stack companion (always one below pred)
    *(trx->m_exec_ptr - 1) = Object::make_integer(scan_idx);

    // Push @check then pred (pred executes first, @check fires after)
    trx->require_exec_capacity(2);
    *++trx->m_exec_ptr = Object::make_control_operator(check_op);
    *++trx->m_exec_ptr = pred;

    // Push candidate for pred to consume
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = candidate;
}

// actor-recv-match: pred -- message
// Receive first message matching predicate.  Blocks if none match.
//
// Trampoline design:
//   Exec stack companions: [scan-index] [pred]
//   scan-index == -1: sentinel meaning "just woke from sleep, start fresh scan"
//   scan-index >= 0: pred just returned a bool for message at this index
//
// For each candidate: push message on op stack, push pred on exec stack.
// Pred executes, consumes message, pushes bool.  @check fires, reads bool.
// throws: execstack-overflow, invalid-access, opstack-overflow, opstack-underflow, type-check, vm-full
static void actor_recv_match_op(Trix *trx) {
    trx->verify_operands(VerifyCallable);
    auto pred_obj = *trx->m_op_ptr;
    {
        pred_obj.set_executable();  // force executable for eval_candidate trampoline
        --trx->m_op_ptr;

        auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
        if (ctx->m_mailbox == nulloffset) {
            trx->error(Error::InvalidAccess, "actor-recv-match: current coroutine has no mailbox");
        } else {
            auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);

            // Push companions onto exec stack: [scan-index] [pred]
            trx->require_exec_capacity(2);
            *++trx->m_exec_ptr = Object::make_integer(0);  // scan index
            *++trx->m_exec_ptr = pred_obj;

            if (mbx->m_count == 0) {
                // Empty: block until message arrives
                *(trx->m_exec_ptr - 1) = Object::make_integer(-1);  // sentinel: wakeup
                mbx->m_blocked_reader = trx->m_running_coroutine;
                trx->coroutine_sleep_and_schedule(SystemName::atActorRecvMatchCheck, std::numeric_limits<uint64_t>::max());
            } else {
                // Have messages: evaluate first candidate
                recv_match_eval_candidate(trx, mbx, pred_obj, 0, SystemName::atActorRecvMatchCheck);
            }
        }
    }
}

// @actor-recv-match-check: trampoline for selective receive.
// Exec stack on entry: [scan-index] [pred] (companions below us)
//
// Two cases:
//   scan_idx == -1: woken from sleep, start fresh scan at index 0
//   scan_idx >= 0:  pred just returned bool for message at scan_idx (bool on op stack)
static void at_actor_recv_match_check_op(Trix *trx) {
    auto pred_obj = *trx->m_exec_ptr;
    auto scan_idx = int{(*(trx->m_exec_ptr - 1)).integer_value()};

    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);

    // Case A: woken from sleep -- start fresh scan
    if (scan_idx == -1) {
        mbx->m_blocked_reader = nulloffset;
        if (mbx->m_count > 0) {
            recv_match_eval_candidate(trx, mbx, pred_obj, 0, SystemName::atActorRecvMatchCheck);
        } else {
            // Still empty after wakeup (spurious): re-sleep
            *(trx->m_exec_ptr - 1) = Object::make_integer(-1);
            mbx->m_blocked_reader = trx->m_running_coroutine;
            trx->coroutine_sleep_and_schedule(SystemName::atActorRecvMatchCheck, std::numeric_limits<uint64_t>::max());
        }
    } else {
        // Case B: pred returned a bool for message at scan_idx
        auto result = *trx->m_op_ptr;
        if (!result.is_boolean()) {
            trx->m_exec_ptr -= 2;  // pop companions before error
            trx->error(
                    Error::TypeCheck, "actor-recv-match: predicate must return a boolean, got {}", Object::type_sv(result.type()));
        } else {
            --trx->m_op_ptr;  // pop bool

            if (result.boolean_value()) {
                // Match found: remove message from mailbox and return it
                auto data = mailbox_data(mbx);
                auto phys_idx = static_cast<length_t>((mbx->m_head + scan_idx) % mbx->m_capacity);
                auto message = data[phys_idx];
                mailbox_remove_at(mbx, static_cast<length_t>(scan_idx));

                // Wake head blocked sender (one slot freed)
                if (mbx->m_blocked_sender != nulloffset) {
                    auto head = mbx->m_blocked_sender;
                    auto head_ctx = trx->offset_to_ptr<CoroutineContext>(head);
                    mbx->m_blocked_sender = head_ctx->m_blocked_sender_next;
                    head_ctx->m_blocked_sender_next = nulloffset;
                    trx->coroutine_wake(head);
                }

                trx->m_exec_ptr -= 2;  // pop companions [pred] [scan-index]
                trx->require_op_capacity(1);
                *++trx->m_op_ptr = message;
            } else {
                // No match: advance to next message
                auto next_idx = scan_idx + 1;
                if (next_idx < static_cast<int>(mbx->m_count)) {
                    recv_match_eval_candidate(
                            trx, mbx, pred_obj, static_cast<length_t>(next_idx), SystemName::atActorRecvMatchCheck);
                } else {
                    // Scanned all messages, no match: block until new message arrives
                    *(trx->m_exec_ptr - 1) = Object::make_integer(-1);  // sentinel
                    mbx->m_blocked_reader = trx->m_running_coroutine;
                    trx->coroutine_sleep_and_schedule(SystemName::atActorRecvMatchCheck, std::numeric_limits<uint64_t>::max());
                }
            }
        }
    }
}

//===--- Selective Receive with Timeout ---===//

// actor-recv-match-timeout: pred ms -- msg bool
// Selective receive with timeout.  Scans mailbox for first message where
// pred returns true.  Returns 'msg true' if found, 'null false' on timeout.
//
// Trampoline design -- hybrid of actor-recv-match and actor-recv-timeout:
//   Exec stack companions: [deadline-long] [scan-index] [pred]
//   scan-index == -1: sentinel for "just woke from sleep, start fresh scan"
//   scan-index >= 0:  pred just returned a bool for message at this index
// throws: execstack-overflow, invalid-access, opstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void actor_recv_match_timeout_op(Trix *trx) {
    trx->verify_operands(VerifyInteger | VerifyNotNegative, VerifyCallable);
    auto timeout_obj = *trx->m_op_ptr;     // ms on top
    auto pred_obj = *(trx->m_op_ptr - 1);  // pred below
    pred_obj.set_executable();             // force executable for eval_candidate trampoline
    auto timeout_ms = timeout_obj.integer_value();
    // Defer the op-stack pop until after make_long below: when m_curr_alloc_global
    // is set, make_long's gvm_alloc can fire a global GC, and the op-stack slots
    // are what keep pred_obj rooted across it (C locals are not GC roots).

    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    if (ctx->m_mailbox == nulloffset) {
        trx->error(Error::InvalidAccess, "actor-recv-match-timeout: current coroutine has no mailbox");
    } else {
        auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
        auto deadline_ns = monotonic_ns() + trx->sleep_budget_grant(static_cast<uint64_t>(timeout_ms)) * 1'000'000ULL;

        // Push companions: [deadline-long] [scan-index] [pred]
        trx->require_exec_capacity(3);
        *++trx->m_exec_ptr = Object::make_long(trx, static_cast<int64_t>(deadline_ns));
        // Re-read ctx/mbx after potential ExtValue allocation
        ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
        mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
        trx->m_op_ptr -= 2;                            // pred_obj/timeout_obj snapshotted; safe to pop after make_long
        *++trx->m_exec_ptr = Object::make_integer(0);  // scan index
        *++trx->m_exec_ptr = pred_obj;

        if (mbx->m_count == 0) {
            // Empty: block with deadline
            *(trx->m_exec_ptr - 1) = Object::make_integer(-1);  // sentinel: wakeup
            mbx->m_blocked_reader = trx->m_running_coroutine;
            trx->coroutine_sleep_and_schedule(SystemName::atActorRecvMatchTimeoutCheck, deadline_ns);
        } else {
            // Have messages: evaluate first candidate
            recv_match_eval_candidate(trx, mbx, pred_obj, 0, SystemName::atActorRecvMatchTimeoutCheck);
        }
    }
}

// @actor-recv-match-timeout-check: trampoline for selective receive with timeout.
// Exec stack on entry: [deadline-long] [scan-index] [pred] (companions below us)
//
// Two cases:
//   scan_idx == -1: woken from sleep -- check timeout, then scan
//   scan_idx >= 0:  pred just returned bool for message at scan_idx
static void at_actor_recv_match_timeout_check_op(Trix *trx) {
    auto pred_obj = *trx->m_exec_ptr;
    auto scan_idx = int{(*(trx->m_exec_ptr - 1)).integer_value()};
    auto deadline_obj = *(trx->m_exec_ptr - 2);
    auto deadline_ns = static_cast<uint64_t>(deadline_obj.long_value(trx));

    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);

    // Case A: woken from sleep
    if (scan_idx == -1) {
        mbx->m_blocked_reader = nulloffset;
        if (mbx->m_count > 0) {
            // Deliver queued messages even past the deadline (Erlang receive/after:
            // a message already in the mailbox when the timer fires is still
            // delivered).  Case B's exhausted-scan branch honors the deadline if
            // none of the queued messages match.  Mirrors actor-recv-timeout,
            // which also checks m_count before the deadline.
            recv_match_eval_candidate(trx, mbx, pred_obj, 0, SystemName::atActorRecvMatchTimeoutCheck);
        } else if (monotonic_ns() >= deadline_ns) {
            // Empty and timed out
            deadline_obj.maybe_free_extvalue(trx);
            trx->m_exec_ptr -= 3;  // pop companions [pred] [scan-index] [deadline]
            trx->require_op_capacity(2);
            *++trx->m_op_ptr = Object::make_null();
            *++trx->m_op_ptr = Object::make_boolean(false);
        } else {
            // Still empty, deadline not reached: re-sleep
            *(trx->m_exec_ptr - 1) = Object::make_integer(-1);
            mbx->m_blocked_reader = trx->m_running_coroutine;
            trx->coroutine_sleep_and_schedule(SystemName::atActorRecvMatchTimeoutCheck, deadline_ns);
        }
    } else {
        // Case B: pred returned a bool for message at scan_idx
        auto result = *trx->m_op_ptr;
        if (!result.is_boolean()) {
            deadline_obj.maybe_free_extvalue(trx);
            trx->m_exec_ptr -= 3;  // pop companions before error
            trx->error(Error::TypeCheck,
                       "actor-recv-match-timeout: predicate must return a boolean, got {}",
                       Object::type_sv(result.type()));
        } else {
            --trx->m_op_ptr;  // pop bool

            if (result.boolean_value()) {
                // Match found: remove message from mailbox and return it
                auto data = mailbox_data(mbx);
                auto phys_idx = static_cast<length_t>((mbx->m_head + scan_idx) % mbx->m_capacity);
                auto message = data[phys_idx];
                mailbox_remove_at(mbx, static_cast<length_t>(scan_idx));

                // Wake head blocked sender (one slot freed)
                if (mbx->m_blocked_sender != nulloffset) {
                    auto head = mbx->m_blocked_sender;
                    auto head_ctx = trx->offset_to_ptr<CoroutineContext>(head);
                    mbx->m_blocked_sender = head_ctx->m_blocked_sender_next;
                    head_ctx->m_blocked_sender_next = nulloffset;
                    trx->coroutine_wake(head);
                }

                deadline_obj.maybe_free_extvalue(trx);
                trx->m_exec_ptr -= 3;  // pop companions
                trx->require_op_capacity(2);
                *++trx->m_op_ptr = message;
                *++trx->m_op_ptr = Object::make_boolean(true);
            } else {
                // No match: advance to next message or check timeout
                auto next_idx = scan_idx + 1;
                if (next_idx < static_cast<int>(mbx->m_count)) {
                    recv_match_eval_candidate(
                            trx, mbx, pred_obj, static_cast<length_t>(next_idx), SystemName::atActorRecvMatchTimeoutCheck);
                } else if (monotonic_ns() >= deadline_ns) {
                    // Scanned all, no match, deadline reached
                    deadline_obj.maybe_free_extvalue(trx);
                    trx->m_exec_ptr -= 3;
                    trx->require_op_capacity(2);
                    *++trx->m_op_ptr = Object::make_null();
                    *++trx->m_op_ptr = Object::make_boolean(false);
                } else {
                    // Scanned all, no match, deadline not reached: block
                    *(trx->m_exec_ptr - 1) = Object::make_integer(-1);
                    mbx->m_blocked_reader = trx->m_running_coroutine;
                    trx->coroutine_sleep_and_schedule(SystemName::atActorRecvMatchTimeoutCheck, deadline_ns);
                }
            }
        }
    }
}

//===--- Mailbox Inspection ---===//

// actor-mailbox-count: coroutine -- int
// Number of messages in mailbox.  Dead actors report 0 (mailbox recycled).
// throws: invalid-access, opstack-underflow, type-check
static void actor_mailbox_count_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto ctx = trx->m_op_ptr->coroutine_context(trx);
    if ((ctx->m_flags & CoroutineContext::FlagWasActor) == 0) {
        trx->error(Error::InvalidAccess, "actor-mailbox-count: coroutine has no mailbox");
    } else if (ctx->m_mailbox == nulloffset) {
        *trx->m_op_ptr = Object::make_integer(0);  // dead actor: mailbox drained on death
    } else {
        auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
        *trx->m_op_ptr = Object::make_integer(mbx->m_count);
    }
}

// actor-mailbox-capacity: coroutine -- int
// Returns the maximum capacity of the actor's mailbox.  Dead actors report
// the capacity they had at death (remembered before the mailbox was recycled).
// throws: opstack-underflow, type-check, invalid-access
static void actor_mailbox_capacity_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto ctx = trx->m_op_ptr->coroutine_context(trx);
    if ((ctx->m_flags & CoroutineContext::FlagWasActor) == 0) {
        trx->error(Error::InvalidAccess, "actor-mailbox-capacity: coroutine has no mailbox");
    } else if (ctx->m_mailbox == nulloffset) {
        *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(ctx->m_last_mailbox_capacity));
    } else {
        auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
        *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(mbx->m_capacity));
    }
}

// actor-status: coroutine -- dict
// Returns a read-only dict with comprehensive actor introspection data:
//   /status, /mailbox-count, /mailbox-capacity, /has-joiner,
//   /trap-exit, /monitor-count, /name
// throws: opstack-underflow, type-check, invalid-access
static void actor_status_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();
    auto ctx = trx->offset_to_ptr<CoroutineContext>(target_offset);
    if ((ctx->m_flags & CoroutineContext::FlagWasActor) == 0) {
        trx->error(Error::InvalidAccess, "actor-status: coroutine has no mailbox");
    } else {
        auto [dict, dict_offset] = Dict::create_or_recycle(trx, 8);
        ctx = trx->offset_to_ptr<CoroutineContext>(target_offset);  // re-fetch after alloc

        // /status -- coroutine status name
        Object status_name;
        switch (ctx->m_status) {
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

        default:
            status_name = trx->wellknown_name(WellKnownName::Dead);
            break;
        }
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyStatus), status_name);

        // /mailbox-count, /mailbox-capacity
        // Dead actors have recycled mailboxes; report 0 count + remembered capacity.
        length_t mbx_count = 0;
        length_t mbx_cap = ctx->m_last_mailbox_capacity;
        if (ctx->m_mailbox != nulloffset) {
            auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
            mbx_count = mbx->m_count;
            mbx_cap = mbx->m_capacity;
        }
        dict->put(
                trx, trx->wellknown_name(WellKnownName::IKeyMailboxCount), Object::make_integer(static_cast<integer_t>(mbx_count)));
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyMailboxCap), Object::make_integer(static_cast<integer_t>(mbx_cap)));

        // /has-joiner
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyHasJoiner), Object::make_boolean(ctx->m_joiner != nulloffset));

        // /trap-exit
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyTrapExit), Object::make_boolean(ctx->m_trap_exit));

        // /monitor-count -- walk m_monitors linked list
        integer_t mon_count = 0;
        auto mon_offset = ctx->m_monitors;
        while (mon_offset != nulloffset) {
            ++mon_count;
            mon_offset = trx->offset_to_ptr<MonitorEntry>(mon_offset)->m_next_target;
        }
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyMonitorCount), Object::make_integer(mon_count));

        // /name -- debug name or null
        if (ctx->m_debug_name != nulloffset) {
            auto name_ptr = trx->offset_to_ptr<Name>(ctx->m_debug_name);
            dict->put(trx, trx->wellknown_name(WellKnownName::IKeyName), Object::make_name(ctx->m_debug_name, name_ptr->length()));
        } else {
            dict->put(trx, trx->wellknown_name(WellKnownName::IKeyName), Object::make_null());
        }

        dict->set_readonly_access_no_save();
        *trx->m_op_ptr = Object::make_dict(dict_offset);
    }
}

// actor-mailbox-empty?: coroutine -- bool
// True if mailbox has no messages.  A dead actor's mailbox is recycled
// when it dies (see coroutine_cleanup_mailbox); FlagWasActor is preserved
// so callers can still query post-death state.  In that case there are
// no messages by construction, so return true rather than erroring --
// this matches actor-mailbox-count's "report 0 capacity for dead actors"
// pattern.  Non-actor coroutines (FlagWasActor unset) still error since
// the caller is asking the wrong question.
// throws: invalid-access, opstack-underflow, type-check
static void actor_mailbox_empty_pred_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto ctx = trx->m_op_ptr->coroutine_context(trx);
    if (ctx->m_mailbox == nulloffset) {
        if ((ctx->m_flags & CoroutineContext::FlagWasActor) != 0) {
            *trx->m_op_ptr = Object::make_boolean(true);
        } else {
            trx->error(Error::InvalidAccess, "actor-mailbox-empty?: coroutine has no mailbox");
        }
    } else {
        auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
        *trx->m_op_ptr = Object::make_boolean(mbx->m_count == 0);
    }
}

// actor-try-send: message coroutine -- bool
// Non-blocking send.  Returns true if delivered, false if target is dead
// or mailbox is full.  Never blocks the caller.
// throws: opstack-underflow, type-check, invalid-access
static void actor_try_send_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine, VerifyAny);
    auto target_obj = *trx->m_op_ptr;

    auto target_offset = target_obj.coroutine_offset();
    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

    if (target->m_status == CoroutineContext::Dead) {
        // Dead: free message, return false
        (trx->m_op_ptr - 1)->maybe_free_extvalue(trx);
        trx->m_op_ptr -= 2;
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_boolean(false);
    } else if (target->m_mailbox == nulloffset) {
        trx->error(Error::InvalidAccess, "actor-try-send: target coroutine has no mailbox");
    } else {
        auto mbx = trx->offset_to_ptr<MailboxHeader>(target->m_mailbox);

        if (mbx->m_count >= mbx->m_capacity) {
            // Full: free message, return false
            (trx->m_op_ptr - 1)->maybe_free_extvalue(trx);
            trx->m_op_ptr -= 2;
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = Object::make_boolean(false);
        } else if (Save::is_restore_fragile_local(trx, *(trx->m_op_ptr - 1))) {
            // A restore-fragile local message would dangle in the global mailbox after a
            // future restore (see actor_send_complete).  actor-try-send is best-effort and
            // non-erroring, so treat it like a full mailbox: drop the message, return false.
            // (actor-send rejects loudly instead, since it has no bool channel to signal.)
            (trx->m_op_ptr - 1)->maybe_free_extvalue(trx);
            trx->m_op_ptr -= 2;
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = Object::make_boolean(false);
        } else {
            // Deliver
            auto message = *(trx->m_op_ptr - 1);
            trx->m_op_ptr -= 2;
            actor_send_complete(trx, mbx, message);
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = Object::make_boolean(true);
        }
    }
}

// actor-broadcast: message array --
// Send cloned copies of message to each actor in the array.
// Dead actors silently skipped.  Non-actor elements raise type-check.
// throws: opstack-underflow, type-check, invalid-access
static void actor_broadcast_op(Trix *trx) {
    trx->require_op_count(2);

    auto arr_obj = *trx->m_op_ptr;
    if (!arr_obj.is_array()) {
        trx->error(Error::TypeCheck, "actor-broadcast: second argument must be an array of actors");
    } else {
        --trx->m_op_ptr;

        auto message = *trx->m_op_ptr;
        --trx->m_op_ptr;

        // Root the array and message across the per-target clone loop: message.make_clone
        // fires vm_global_gc (global mode), and both were popped into C-locals above -- a
        // ${...}-global array/message would otherwise be swept mid-broadcast (the re-fetch
        // below only guards a non-compacting move, which never happens; it does NOT cover
        // the sweep of an unrooted block).
        trx->require_gc_root_capacity(2);
        *++trx->m_gc_roots_ptr = arr_obj;
        *++trx->m_gc_roots_ptr = message;

        auto [targets, count] = arr_obj.array_value(trx);
        for (length_t i = 0; i < count; ++i) {
            // Re-fetch targets after each send (clone may allocate)
            targets = arr_obj.array_objects(trx);
            if (!targets[i].is_coroutine()) {
                message.maybe_free_extvalue(trx);
                trx->error(Error::TypeCheck, "actor-broadcast: element {} is not a coroutine", i);
            } else {
                auto target_offset = targets[i].coroutine_offset();
                auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

                // Skip dead or no-mailbox targets
                if ((target->m_status != CoroutineContext::Dead) && (target->m_mailbox != nulloffset)) {
                    auto mbx = trx->offset_to_ptr<MailboxHeader>(target->m_mailbox);
                    if (mbx->m_count < mbx->m_capacity) {
                        auto msg_copy = message.make_clone(trx);
                        // Re-fetch mbx after clone allocation
                        mbx = trx->offset_to_ptr<MailboxHeader>(target->m_mailbox);
                        actor_send_complete(trx, mbx, msg_copy);
                    }
                    // Full mailbox: silently skip (broadcast is best-effort)
                }
            }
        }

        trx->reset_gc_root(2);
        // Free the original message
        message.maybe_free_extvalue(trx);
    }
}

// actor-flush: -- int
// Drain all messages from own mailbox, free ExtValues, return count.
// throws: invalid-access (no mailbox)
static void actor_flush_op(Trix *trx) {
    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    if (ctx->m_mailbox == nulloffset) {
        trx->error(Error::InvalidAccess, "actor-flush: current coroutine has no mailbox");
    } else {
        auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
        auto flushed = integer_t{mbx->m_count};
        auto data = mailbox_data(mbx);

        // Free ExtValues in all remaining messages
        while (mbx->m_count > 0) {
            data[mbx->m_head].maybe_free_extvalue(trx);
            mbx->m_head = static_cast<length_t>((mbx->m_head + 1) % mbx->m_capacity);
            --mbx->m_count;
        }

        // Wake all blocked senders (mailbox drained, all can retry)
        auto blocked = mbx->m_blocked_sender;
        while (blocked != nulloffset) {
            auto sender = trx->offset_to_ptr<CoroutineContext>(blocked);
            auto next = sender->m_blocked_sender_next;
            sender->m_blocked_sender_next = nulloffset;
            trx->coroutine_wake(blocked);
            blocked = next;
        }
        mbx->m_blocked_sender = nulloffset;

        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_integer(flushed);
    }
}

// actor-set-name: name coroutine --
// Sets a debug name on an actor (coroutine with mailbox) for diagnostic display.
// throws: opstack-underflow, type-check
static void actor_set_name_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine, VerifyName);

    auto actor_obj = *trx->m_op_ptr--;
    auto name_obj = *trx->m_op_ptr--;

    auto ctx = actor_obj.coroutine_context(trx);
    if (ctx->m_mailbox == nulloffset) {
        trx->error(Error::TypeCheck, "actor-set-name: coroutine is not an actor (no mailbox)");
    } else {
        ctx->m_debug_name = name_obj.name_offset();
    }
}

// actor-name: coroutine -- name true | false
// Returns the debug name of an actor, or false if unnamed.
// throws: opstack-underflow, type-check
static void actor_name_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto actor_obj = *trx->m_op_ptr;
    auto ctx = actor_obj.coroutine_context(trx);

    if (ctx->m_debug_name != nulloffset) {
        trx->require_op_capacity(1);
        auto name = trx->offset_to_ptr<Name>(ctx->m_debug_name);
        *trx->m_op_ptr = Object::make_name(ctx->m_debug_name, name->length());
        *++trx->m_op_ptr = Object::make_boolean(true);
    } else {
        *trx->m_op_ptr = Object::make_boolean(false);
    }
}
