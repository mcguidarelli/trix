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

//===--- Supervision Framework ---===//
//
// Implements Erlang/OTP-style fault tolerance through process monitoring,
// linking, and supervised restart policies.  Based on:
//
//   Joe Armstrong, "Making reliable distributed systems in the presence of
//   software errors", PhD thesis, 2003.  Chapters 4-5: supervision trees,
//   "let it crash" philosophy, and restart strategies.
//
//   OTP Design Principles, Erlang/OTP documentation: supervisor behaviour,
//   one_for_one / one_for_all / rest_for_one strategies, child specifications,
//   intensity/period restart throttling.
//
// The key insight from Erlang: instead of writing defensive code to handle
// every possible failure, let processes crash and have a supervisor restart
// them in a known good state.  This separates error handling (the supervisor)
// from business logic (the worker).
//
// --- Core concepts for maintainers ---
//
// MONITORS
//   A monitor is a one-way observation: actor A monitors actor B.  When B
//   dies (for any reason), A receives a /down tagged message in its
//   mailbox containing the dead actor's handle and exit reason.  The
//   monitor is automatically removed after firing.
//
//   Monitors are non-intrusive: B does not know it is being monitored.
//   Multiple monitors can exist on the same target, each generating an
//   independent notification with a unique reference ID.
//
//   If the target is already dead when `monitor` is called, the /down
//   message is delivered immediately.
//
// LINKS
//   A link is a bidirectional relationship: if either actor dies with
//   a non-normal reason, the other is killed (or receives an /exit
//   message if it has trap-exit enabled).
//
//   Links implement the "fail together" pattern: a group of cooperating
//   actors that cannot function independently.  If one crashes, the
//   others should not continue with stale assumptions.
//
//   `trap-exit` converts fatal link signals into messages, allowing an
//   actor to handle peer death without dying itself.  This is how
//   supervisors survive their children's crashes.
//
// MONITOR/LINK REPRESENTATION
//   Each relationship is stored as a MonitorEntry struct on the VM heap.
//   A MonitorEntry participates in two linked lists simultaneously:
//     - The target's m_monitors list ("who is watching me")
//     - The source's m_monitoring list ("who I am watching")
//   The high bit of m_ref_id distinguishes monitors (0) from links (1).
//
// SUPERVISORS
//   A supervisor is an actor that manages a set of child actors.  It is
//   created with the `supervisor` operator, which takes a specification
//   dict describing the children and restart policy.
//
//   Supervisor specification:
//     /strategy    -- restart strategy: /one-for-one, /one-for-all,
//                     or /rest-for-one
//     /intensity   -- max restarts allowed within the period (0 = no restarts)
//     /period      -- time window in milliseconds for intensity counting
//     /children    -- array of child specs, each a dict with:
//         /id      -- name for identification
//         /start   -- executable proc to launch the child
//         /restart -- /permanent (always restart), /temporary (never restart),
//                     or /transient (restart only on abnormal exit)
//
// RESTART STRATEGIES
//   one-for-one:  Only the crashed child is restarted.  Other children
//                 are unaffected.  Use when children are independent.
//
//   one-for-all:  All children are killed and restarted when any one
//                 crashes.  Use when children have interdependencies and
//                 cannot function correctly after a peer crash.
//
//   rest-for-one: The crashed child and all children started after it
//                 are killed and restarted.  Use when children have a
//                 sequential dependency (B depends on A, C depends on B).
//
// RESTART THROTTLING (intensity / period)
//   If the number of restarts within the period exceeds the intensity
//   limit, the supervisor itself dies with reason /shutdown.  This
//   prevents infinite restart loops when a child has a persistent bug.
//   The supervisor's own supervisor (if any) then handles the escalation.
//
// DEATH NOTIFICATION FLOW
//   When a coroutine dies (coroutine_kill in ops_coroutine.inl), the
//   supervision system is notified via supervision_notify_death():
//     1. Walk the dying actor's m_monitors list (who is watching me).
//     2. For each monitor entry: deliver /down message to the watcher.
//     3. For each link entry: kill the linked actor (or deliver /exit
//        if it has trap-exit enabled).
//   This runs synchronously during coroutine_kill, before the dead
//   actor's stacks are freed.
//
// --- Operators ---
//
//   monitor          coroutine -- int       Monitor an actor (returns ref ID)
//   demonitor        int --                 Cancel a monitor by ref ID
//   link             coroutine --           Create bidirectional link
//   unlink           coroutine --           Remove bidirectional link
//   actor-exit       any --                 Exit current actor with reason
//   actor-trap-exit  bool --                Enable/disable exit trapping
//   spawn-link       mark obj* proc -- coroutine    Spawn + link atomically
//   spawn-monitor    mark obj* proc -- coroutine int  Spawn + monitor atomically
//   supervisor       dict -- coroutine      Create a supervisor from spec
//   supervisor-which-children  coroutine -- array  List active children
//   supervisor-start-child      supervisor child-spec --   Add a child (async)
//   supervisor-terminate-child  supervisor child-id --     Kill a child by id (async)
//   supervisor-restart-child    supervisor child-id --     Restart a stopped child by id (async)
//   supervisor-stop             supervisor --              Stop supervisor + all children (async)
//   supervisor-count-children   coroutine -- int           Count active children
//   supervisor-get-child        coroutine name -- coroutine true | false  Look up child by id
//   supervisor-spec             coroutine -- dict          Read-only supervisor config dict
//   is-supervisor               any -- bool                True if value is a live supervisor
//
// Control operators (internal, not user-visible):
//   @supervisor-init     Read state_offset from the context's m_return_value,
//                        push it as the exec-stack companion, enter recv loop
//                        (children are already spawned synchronously in
//                        supervisor_op before the coroutine ever runs)
//   @supervisor-check    Recv-loop wakeup handler: clear blocked-reader,
//                        re-enter the recv loop
//

//===--- Helpers ---===//

// (supervision_wake removed -- use trx->coroutine_wake(offset) instead)

// Deliver a tagged message to an actor's mailbox.
// Returns true if delivered, false if mailbox is full or actor has no mailbox.
static bool supervision_deliver_message(Trix *trx, vm_offset_t target_offset, Object message) {
    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
    if (target->m_mailbox != nulloffset) {
        auto mbox = trx->offset_to_ptr<MailboxHeader>(target->m_mailbox);
        if (mbox->m_count < mbox->m_capacity) {
            auto data = mailbox_data(mbox);
            data[mbox->m_tail] = message;
            mbox->m_tail = static_cast<length_t>((mbox->m_tail + 1) % mbox->m_capacity);
            ++mbox->m_count;
            // Wake the actor if it's sleeping on recv
            auto reader_offset = mbox->m_blocked_reader;
            mbox->m_blocked_reader = nulloffset;
            trx->coroutine_wake(reader_offset);
            return true;
        }
    }
    return false;
}

// Build a /down tagged message: /down << /ref ref_id /actor handle /reason reason >> tag
// Payload is a dict with keys /ref, /actor, /reason.
// Allocates: 2 Objects (tagged) + dict header + 3 entries.
// Uses Dict::create_dict (not pool) because these messages are consumed and abandoned.
static Object supervision_build_down_message(Trix *trx, uint32_t ref_id, vm_offset_t actor_offset, Object reason) {
    auto [dict, dict_offset] = Dict::create_dict(trx, 4);
    dict->put(
            trx, trx->wellknown_name(WellKnownName::RefKey), Object::make_integer(static_cast<integer_t>(ref_id & MonitorRefMask)));
    dict->put(trx, trx->wellknown_name(WellKnownName::ActorKey), Object::make_coroutine(actor_offset));
    dict->put(trx, trx->wellknown_name(WellKnownName::ReasonKey), reason);
    // Tag it as /down -- allocate 2 Objects: [tag-name, payload]
    auto payload = Object::make_dict(dict_offset);
    return Object::make_tagged(trx, trx->wellknown_name(WellKnownName::Down), payload);
}

// Build a /exit tagged message: /exit << /actor handle /reason reason >> tag
static Object supervision_build_exit_message(Trix *trx, vm_offset_t actor_offset, Object reason) {
    auto [dict, dict_offset] = Dict::create_dict(trx, 3);
    dict->put(trx, trx->wellknown_name(WellKnownName::ActorKey), Object::make_coroutine(actor_offset));
    dict->put(trx, trx->wellknown_name(WellKnownName::ReasonKey), reason);
    // Tag it as /exit -- allocate 2 Objects: [tag-name, payload]
    auto payload = Object::make_dict(dict_offset);
    return Object::make_tagged(trx, trx->wellknown_name(WellKnownName::Exit), payload);
}

// Sentinel stored in m_ref_id while a MonitorEntry is parked on the free list.
// Detects double-recycle at O(1) (principle from Dict::RecycleSentinel).
static constexpr uint32_t MonitorEntryRecycleSentinel = 0xDEADBEEFu;

// Allocate a MonitorEntry and link it into both source's m_monitoring and
// target's m_monitors lists.  Returns the entry offset.
//
// Reuse first: pull from the per-save-level free-list pool if present.
// Free-list link is stored in m_next_target (unused while parked);
// m_ref_id = MonitorEntryRecycleSentinel flags the parked state.  Fresh
// allocations fall back to vm_alloc.
//
// Caller must re-fetch any CoroutineContext pointers after this call (vm_alloc
// may bump m_vm_ptr; source/target are re-fetched below).
static vm_offset_t supervision_create_monitor(Trix *trx, vm_offset_t source_offset, vm_offset_t target_offset, uint32_t ref_id) {
    MonitorEntry *entry;
    vm_offset_t entry_offset;
    // Pool is a single class-free head (MonitorEntry is a fixed 24-byte
    // struct; no class buckets needed).  MonitorEntries live in global VM
    // (BASE-immune to save/restore), so the pool needs no per-save-level
    // dimension.
    auto pooled = trx->m_monitor_pool;
    if (pooled != nulloffset) {
        entry_offset = pooled;
        entry = trx->offset_to_ptr<MonitorEntry>(pooled);
        if (entry->m_ref_id != MonitorEntryRecycleSentinel) {
            trx->error(Error::InternalError, "supervision_create_monitor: recycled entry missing sentinel");
        } else {
            trx->m_monitor_pool = entry->m_next_target;  // unlink from free list
        }
    } else {
        // Monitors live in global VM, tagged ChunkKind::Monitor.
        auto [fresh, fresh_offset] = trx->gvm_alloc<MonitorEntry>(sizeof(MonitorEntry), Trix::ChunkKind::Monitor);
        entry = fresh;
        entry_offset = fresh_offset;
    }
    auto source = trx->offset_to_ptr<CoroutineContext>(source_offset);
    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
    entry->m_next_target = target->m_monitors;
    entry->m_next_source = source->m_monitoring;
    entry->m_source = source_offset;
    entry->m_target = target_offset;
    entry->m_ref_id = ref_id;
    entry->m_padding = 0;
    target->m_monitors = entry_offset;
    source->m_monitoring = entry_offset;
    return entry_offset;
}

// Return a fully-unlinked MonitorEntry to the per-save-level free pool.
// Precondition: the caller has removed this entry from both the source's
// m_monitoring list and the target's m_monitors list.  Re-uses m_next_target
// as the free-list link (no need for m_next_source; only the head matters).
static void supervision_free_monitor(Trix *trx, MonitorEntry *entry, vm_offset_t entry_offset) {
    if (entry->m_ref_id == MonitorEntryRecycleSentinel) {
        trx->error(Error::InternalError, "supervision_free_monitor: double-recycle at offset {}", entry_offset);
    } else {
        entry->m_next_target = trx->m_monitor_pool;
        entry->m_ref_id = MonitorEntryRecycleSentinel;
        trx->m_monitor_pool = entry_offset;
    }
}

// Remove a MonitorEntry from a singly-linked list (given the list head offset pointer).
// Returns true if found and removed.
static bool supervision_remove_from_list(Trix *trx, vm_offset_t *head, vm_offset_t entry_offset, bool target_list) {
    auto prev_offset_ptr = head;
    auto curr_offset = *head;
    while (curr_offset != nulloffset) {
        auto entry = trx->offset_to_ptr<MonitorEntry>(curr_offset);
        if (curr_offset == entry_offset) {
            *prev_offset_ptr = target_list ? entry->m_next_target : entry->m_next_source;
            return true;
        } else {
            prev_offset_ptr = trx->offset_to_ptr<vm_offset_t>(
                    curr_offset + static_cast<vm_offset_t>(target_list ? offsetof(MonitorEntry, m_next_target)
                                                                       : offsetof(MonitorEntry, m_next_source)));
            curr_offset = target_list ? entry->m_next_target : entry->m_next_source;
        }
    }
    return false;
}

//===--- Death Notification (called from coroutine_kill) ---===//

// Process all monitors/links when a coroutine dies.
// Called after ctx->m_status is set to Dead.
void supervision_notify_death(CoroutineContext *ctx, vm_offset_t ctx_offset) {
    auto trx = this;

    auto reason = ctx->m_exit_reason;
    const bool is_normal = reason.is_name() && (reason.name_offset() == wellknown_offset(WellKnownName::Normal));

    // 1. Walk ctx->m_monitors ("who is watching me") and notify
    auto entry_offset = ctx->m_monitors;
    while (entry_offset != nulloffset) {
        auto entry = offset_to_ptr<MonitorEntry>(entry_offset);
        auto next = entry->m_next_target;
        auto source_offset = entry->m_source;
        const bool is_link = (entry->m_ref_id & MonitorTypeMask) != 0;

        // Remove this entry from source's m_monitoring list
        auto source = offset_to_ptr<CoroutineContext>(source_offset);
        if (source->m_status != CoroutineContext::Dead) {
            supervision_remove_from_list(trx, &source->m_monitoring, entry_offset, false);

            if (is_link) {
                // Link: propagate exit signal (unless normal)
                if (!is_normal) {
                    if (source->m_trap_exit) {
                        // Convert to message
                        auto msg = supervision_build_exit_message(trx, ctx_offset, reason);
                        supervision_deliver_message(trx, source_offset, msg);
                    } else {
                        // Kill the linked partner with same reason
                        coroutine_kill(source, reason);
                    }
                }
                // Also remove the reverse link entry from our m_monitors list
                // (it's in the remaining entries we haven't walked yet, or already processed)
                // We handle this by walking source's m_monitors to find the reverse entry pointing to us
                auto rev_offset = source->m_monitors;
                vm_offset_t *rev_prev = &source->m_monitors;
                while (rev_offset != nulloffset) {
                    auto rev = offset_to_ptr<MonitorEntry>(rev_offset);
                    if ((rev->m_source == ctx_offset) && ((rev->m_ref_id & MonitorTypeMask) != 0)) {
                        // Found reverse link -- remove from source's m_monitors list
                        *rev_prev = rev->m_next_target;
                        break;
                    } else {
                        rev_prev = &rev->m_next_target;
                        rev_offset = rev->m_next_target;
                    }
                }
            } else {
                // Monitor: deliver /down message
                auto ref_id = entry->m_ref_id & MonitorRefMask;
                auto msg = supervision_build_down_message(trx, ref_id, ctx_offset, reason);
                supervision_deliver_message(trx, source_offset, msg);
            }

            // Entry was unlinked from source->m_monitoring above and is about
            // to be unlinked from ctx->m_monitors when the walk moves past.
            // Safe to recycle.  We only free inside this branch -- the "source
            // already dead" case skips unlink (its m_monitoring is empty), but
            // the entry may still be referenced by other still-walking lists
            // further up the call stack (recursive link propagation), so
            // abandoning it is safer than risking use-after-free.  The
            // abandoned-entries leak is bounded by the link fan-out and
            // gets reclaimed on save/restore rollback.
            supervision_free_monitor(trx, entry, entry_offset);
        }
        entry_offset = next;
    }
    ctx->m_monitors = nulloffset;

    // 2. Walk ctx->m_monitoring ("who I was watching") and remove self from their lists
    entry_offset = ctx->m_monitoring;
    while (entry_offset != nulloffset) {
        auto entry = offset_to_ptr<MonitorEntry>(entry_offset);
        auto next = entry->m_next_source;
        auto target_offset = entry->m_target;
        auto target = offset_to_ptr<CoroutineContext>(target_offset);

        if (target->m_status != CoroutineContext::Dead) {
            // Remove this entry from target's m_monitors list
            supervision_remove_from_list(trx, &target->m_monitors, entry_offset, true);
            // Now fully unlinked (walk will move past in a moment).  Safe to recycle.
            // See parallel comment in loop 1 above for why we only free in the alive branch.
            supervision_free_monitor(trx, entry, entry_offset);
        }
        entry_offset = next;
    }
    ctx->m_monitoring = nulloffset;
}

//===--- Monitor Primitives ---===//

// monitor: coroutine -- int
// Register for death notification. Returns ref-id for demonitor.
// Caller must be an actor (have a mailbox). Target can be any coroutine.
// If target is already dead, immediately delivers /down message.
// throws: invalid-access, opstack-underflow, type-check, vm-full
static void monitor_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();

    // Caller must be an actor
    auto caller = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    if (caller->m_mailbox == nulloffset) {
        trx->error(Error::InvalidAccess, "monitor: caller must be an actor (has no mailbox)");
    } else if (target_offset == trx->m_running_coroutine) {
        // Cannot monitor self
        trx->error(Error::InvalidAccess, "monitor: cannot monitor self");
    } else {
        auto ref_id = trx->m_next_monitor_ref++;
        auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

        if (target->m_status == CoroutineContext::Dead) {
            // Immediately deliver /down message
            auto msg = supervision_build_down_message(trx, ref_id, target_offset, target->m_exit_reason);
            supervision_deliver_message(trx, trx->m_running_coroutine, msg);
        } else {
            supervision_create_monitor(trx, trx->m_running_coroutine, target_offset, ref_id);
        }
        *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(ref_id));
    }
}

// demonitor: int --
// Cancel a monitor by ref-id.
// throws: opstack-underflow, type-check
static void demonitor_op(Trix *trx) {
    trx->verify_operands(VerifyInteger);

    auto ref_id = static_cast<uint32_t>(trx->m_op_ptr->integer_value());
    --trx->m_op_ptr;

    auto caller = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);

    // Walk caller's m_monitoring list to find the entry with this ref_id
    auto prev_ptr = &caller->m_monitoring;
    auto curr_offset = caller->m_monitoring;
    while (curr_offset != nulloffset) {
        auto entry = trx->offset_to_ptr<MonitorEntry>(curr_offset);
        if (((entry->m_ref_id & MonitorRefMask) == ref_id) && ((entry->m_ref_id & MonitorTypeMask) == 0)) {
            // Found it -- remove from caller's m_monitoring list
            *prev_ptr = entry->m_next_source;
            // Remove from target's m_monitors list
            auto target = trx->offset_to_ptr<CoroutineContext>(entry->m_target);
            if (target->m_status != CoroutineContext::Dead) {
                supervision_remove_from_list(trx, &target->m_monitors, curr_offset, true);
            }
            // Fully unlinked: recycle.
            supervision_free_monitor(trx, entry, curr_offset);
            break;
        } else {
            prev_ptr = &entry->m_next_source;
            curr_offset = entry->m_next_source;
        }
    }
}

//===--- Link Primitives ---===//

// link: coroutine --
// Bidirectional death link (idempotent).
// Caller must be an actor. Target can be any coroutine.
// If target is already dead and exited abnormally, immediately signals caller.
// throws: invalid-access, opstack-underflow, type-check, vm-full
static void link_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();
    --trx->m_op_ptr;

    auto caller = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    if (caller->m_mailbox == nulloffset) {
        trx->error(Error::InvalidAccess, "link: caller must be an actor (has no mailbox)");
    } else if (target_offset == trx->m_running_coroutine) {
        trx->error(Error::InvalidAccess, "link: cannot link to self");
    } else {
        auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

        // Check idempotency: walk caller's m_monitoring for existing link to target
        auto curr_offset = caller->m_monitoring;
        while (curr_offset != nulloffset) {
            auto entry = trx->offset_to_ptr<MonitorEntry>(curr_offset);
            if ((entry->m_target == target_offset) && ((entry->m_ref_id & MonitorTypeMask) != 0)) {
                return;  // already linked
            }
            curr_offset = entry->m_next_source;
        }

        if (target->m_status == CoroutineContext::Dead) {
            auto reason = target->m_exit_reason;
            const bool is_normal =
                    reason.is_name() && (reason.name_offset() == trx->wellknown_name(WellKnownName::Normal).name_offset());
            if (!is_normal) {
                if (caller->m_trap_exit) {
                    auto msg = supervision_build_exit_message(trx, target_offset, reason);
                    supervision_deliver_message(trx, trx->m_running_coroutine, msg);
                } else {
                    // Kill caller with same reason
                    trx->coroutine_flush_running();
                    trx->coroutine_kill(caller, reason);
                    trx->coroutine_schedule();
                }
            }
        } else {
            auto ref_id = trx->m_next_monitor_ref++;

            // Create forward entry: caller monitors target
            supervision_create_monitor(trx, trx->m_running_coroutine, target_offset, ref_id | MonitorTypeMask);
            // Create reverse entry: target monitors caller
            supervision_create_monitor(trx, target_offset, trx->m_running_coroutine, ref_id | MonitorTypeMask);
        }
    }
}

// unlink: coroutine --
// Remove bidirectional link.
// throws: opstack-underflow, type-check
static void unlink_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_offset = trx->m_op_ptr->coroutine_offset();
    --trx->m_op_ptr;

    auto caller = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);

    // Find and remove forward link (caller->target) from caller's m_monitoring and target's m_monitors
    auto prev_ptr = &caller->m_monitoring;
    auto curr_offset = caller->m_monitoring;
    while (curr_offset != nulloffset) {
        auto entry = trx->offset_to_ptr<MonitorEntry>(curr_offset);
        if ((entry->m_target == target_offset) && ((entry->m_ref_id & MonitorTypeMask) != 0)) {
            *prev_ptr = entry->m_next_source;
            if (target->m_status != CoroutineContext::Dead) {
                supervision_remove_from_list(trx, &target->m_monitors, curr_offset, true);
            }
            supervision_free_monitor(trx, entry, curr_offset);
            break;
        } else {
            prev_ptr = &entry->m_next_source;
            curr_offset = entry->m_next_source;
        }
    }

    // Find and remove reverse link (target->caller) from target's m_monitoring and caller's m_monitors
    if (target->m_status != CoroutineContext::Dead) {
        prev_ptr = &target->m_monitoring;
        curr_offset = target->m_monitoring;
        while (curr_offset != nulloffset) {
            auto entry = trx->offset_to_ptr<MonitorEntry>(curr_offset);
            if ((entry->m_target == trx->m_running_coroutine) && ((entry->m_ref_id & MonitorTypeMask) != 0)) {
                *prev_ptr = entry->m_next_source;
                supervision_remove_from_list(trx, &caller->m_monitors, curr_offset, true);
                supervision_free_monitor(trx, entry, curr_offset);
                break;
            } else {
                prev_ptr = &entry->m_next_source;
                curr_offset = entry->m_next_source;
            }
        }
    }
}

//===--- Actor Exit Primitives ---===//

// actor-exit: any --
// Kill self with specified reason.  /normal = clean exit.
// throws: invalid-exit, opstack-underflow
static void actor_exit_op(Trix *trx) {
    trx->require_op_count(1);

    if (trx->m_running_coroutine == trx->m_main_context) {
        trx->error(Error::InvalidExit, "actor-exit: main coroutine cannot exit");
    } else {
        auto reason_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;

        trx->coroutine_flush_running();
        auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
        trx->coroutine_kill(ctx, reason_obj);
        trx->coroutine_schedule();
    }
}

// actor-trap-exit: bool --
// Enable/disable exit trapping.  When true, exit signals from linked actors
// are converted to /exit messages instead of killing this actor.
// throws: invalid-access, opstack-underflow, type-check
static void actor_trap_exit_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);

    auto val = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    if (ctx->m_mailbox == nulloffset) {
        trx->error(Error::InvalidAccess, "actor-trap-exit: caller must be an actor (has no mailbox)");
    } else {
        ctx->m_trap_exit = val;
    }
}

//===--- Spawn Variants ---===//

// spawn-link: mark obj* proc -- coroutine
// Atomic actor-spawn + link.
// throws: invalid-access, opstack-overflow, opstack-underflow, type-check, unmatched-mark, vm-full
static void spawn_link_op(Trix *trx) {
    auto caller = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    if (caller->m_mailbox == nulloffset) {
        trx->error(Error::InvalidAccess, "spawn-link: caller must be an actor (has no mailbox)");
    } else {
        auto [ctx, ctx_offset] = coroutine_launch_common(trx, "spawn-link");

        // Allocate mailbox (default capacity)
        auto [mbx, mbx_offset] = actor_alloc_mailbox(trx, MaxActorMailboxCapacity);
        // Re-fetch pointers after allocation
        ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);
        caller = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
        ctx->m_mailbox = mbx_offset;
        ctx->m_flags |= CoroutineContext::FlagWasActor;

        // Create bidirectional link
        auto ref_id = trx->m_next_monitor_ref++;
        supervision_create_monitor(trx, trx->m_running_coroutine, ctx_offset, ref_id | MonitorTypeMask);
        supervision_create_monitor(trx, ctx_offset, trx->m_running_coroutine, ref_id | MonitorTypeMask);

        // Push coroutine handle
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_coroutine(ctx_offset);
    }
}

// spawn-monitor: mark obj* proc -- coroutine int
// Atomic actor-spawn + monitor.
// throws: invalid-access, opstack-overflow, opstack-underflow, type-check, unmatched-mark, vm-full
static void spawn_monitor_op(Trix *trx) {
    auto caller = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    if (caller->m_mailbox == nulloffset) {
        trx->error(Error::InvalidAccess, "spawn-monitor: caller must be an actor (has no mailbox)");
    } else {
        auto [ctx, ctx_offset] = coroutine_launch_common(trx, "spawn-monitor");

        // Allocate mailbox (default capacity)
        auto [mbx, mbx_offset] = actor_alloc_mailbox(trx, MaxActorMailboxCapacity);
        // Re-fetch pointers after allocation
        ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);
        caller = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
        ctx->m_mailbox = mbx_offset;
        ctx->m_flags |= CoroutineContext::FlagWasActor;

        // Create monitor
        auto ref_id = trx->m_next_monitor_ref++;
        supervision_create_monitor(trx, trx->m_running_coroutine, ctx_offset, ref_id);

        // Push coroutine handle and ref-id
        trx->require_op_capacity(2);
        *++trx->m_op_ptr = Object::make_coroutine(ctx_offset);
        *++trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(ref_id));
    }
}

//===--- Supervisor ---===//

// Internal state for a supervisor actor, stored on the VM heap.
// Layout: [SupervisorState header][ChildEntry[max_children]][uint32_t timestamps[max_intensity]]
// Timestamps are stored as uint32_t milliseconds (monotonic, ~49-day range).
struct SupervisorState {
    enum Strategy : uint8_t { OneForOne, OneForAll, RestForOne };
    enum RestartPolicy : uint8_t { Permanent, Temporary, Transient };

    struct ChildEntry {        // 32 bytes
        Object id;             // Name (literal)
        Object start;          // Executable proc (array/packed/curry)
        vm_offset_t actor;     // current actor handle (nulloffset if not running)
        uint32_t monitor_ref;  // monitor ref_id for current actor
        length_t capacity;     // mailbox capacity (0 = use default)
        RestartPolicy policy;
        uint8_t active;  // 1 = running, 0 = stopped
        uint32_t padding;
    };
    static_assert(sizeof(ChildEntry) == 32, "SupervisorState::ChildEntry size");

    Strategy strategy;
    uint8_t padding1[3];
    uint32_t max_intensity;  // max restarts in sliding window
    uint32_t period_ms;      // window size in milliseconds
    length_t child_count;    // current number of child entries (grows with start-child)
    length_t max_children;   // allocated capacity for ChildEntry array
    uint32_t restart_count;  // valid entries in timestamps array
    uint32_t padding3;       // pad to 24 bytes
};
static_assert(sizeof(SupervisorState) == 24, "SupervisorState header size");

// Get the ChildEntry array following a SupervisorState header.
static SupervisorState::ChildEntry *supervisor_children(Trix *trx, vm_offset_t state_offset) {
    auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
    return reinterpret_cast<SupervisorState::ChildEntry *>(state + 1);
}

// Get the timestamps array following the ChildEntry array.
// Uses max_children (allocated capacity) to find the correct offset.
static uint32_t *supervisor_timestamps(Trix *trx, vm_offset_t state_offset) {
    auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
    auto children = reinterpret_cast<SupervisorState::ChildEntry *>(state + 1);
    return reinterpret_cast<uint32_t *>(children + state->max_children);
}

// Spawn a single child actor, monitor it, and update the ChildEntry.
// Called from @supervisor-init and during restart.  source_offset is the
// coroutine that becomes the monitor source (the supervisor itself); it is
// passed explicitly so callers outside the supervisor's own execution context
// (e.g. supervisor_op, which runs in the caller's coroutine) can still spawn
// children with the monitor wired to the supervisor.
static void supervisor_spawn_child(Trix *trx, vm_offset_t source_offset, vm_offset_t state_offset, length_t child_idx) {
    auto children = supervisor_children(trx, state_offset);
    auto start_proc = children[child_idx].start;
    auto cap = children[child_idx].capacity;

    // Push mark + start proc onto supervisor's op stack
    trx->require_op_capacity(2);
    *++trx->m_op_ptr = Object::make_mark();
    *++trx->m_op_ptr = start_proc;

    // Spawn child coroutine (inserts into scheduler, increments live count)
    auto [ctx, ctx_offset] = coroutine_launch_common(trx, "supervisor");

    // Allocate mailbox for child
    auto mbox_cap = (cap > 0) ? cap : MaxActorMailboxCapacity;
    auto [mbx, mbx_offset] = actor_alloc_mailbox(trx, mbox_cap);
    ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);
    ctx->m_mailbox = mbx_offset;
    ctx->m_flags |= CoroutineContext::FlagWasActor;

    // Monitor the child (supervisor watches child)
    auto ref_id = trx->m_next_monitor_ref++;
    supervision_create_monitor(trx, source_offset, ctx_offset, ref_id);

    // Update child entry
    children = supervisor_children(trx, state_offset);
    children[child_idx].actor = ctx_offset;
    children[child_idx].monitor_ref = ref_id;
    children[child_idx].active = 1;
}

// Demonitor and kill a single child.  Used by one-for-all / rest-for-one strategies
// and supervisor shutdown.
static void supervisor_kill_child(Trix *trx, vm_offset_t state_offset, length_t child_idx) {
    auto children = supervisor_children(trx, state_offset);
    if ((children[child_idx].actor != nulloffset) && children[child_idx].active) {
        auto child_offset = children[child_idx].actor;
        auto child_ref = children[child_idx].monitor_ref;
        auto child_ctx = trx->offset_to_ptr<CoroutineContext>(child_offset);

        if (child_ctx->m_status == CoroutineContext::Dead) {
            children[child_idx].active = 0;
            children[child_idx].actor = nulloffset;
        } else {
            // Demonitor first (prevent /down when we kill the child)
            auto sup_ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
            auto prev_ptr = &sup_ctx->m_monitoring;
            auto curr_offset = sup_ctx->m_monitoring;
            while (curr_offset != nulloffset) {
                auto me = trx->offset_to_ptr<MonitorEntry>(curr_offset);
                if (((me->m_ref_id & MonitorRefMask) == child_ref) && ((me->m_ref_id & MonitorTypeMask) == 0)) {
                    *prev_ptr = me->m_next_source;
                    child_ctx = trx->offset_to_ptr<CoroutineContext>(child_offset);
                    supervision_remove_from_list(trx, &child_ctx->m_monitors, curr_offset, true);
                    supervision_free_monitor(trx, me, curr_offset);
                    break;
                } else {
                    prev_ptr = &me->m_next_source;
                    curr_offset = me->m_next_source;
                }
            }

            // Kill the child with /shutdown reason
            child_ctx = trx->offset_to_ptr<CoroutineContext>(child_offset);
            if (child_ctx->m_status != CoroutineContext::Dead) {
                trx->coroutine_kill(child_ctx, trx->wellknown_name(WellKnownName::Shutdown));
            }

            children = supervisor_children(trx, state_offset);
            children[child_idx].active = 0;
            children[child_idx].actor = nulloffset;
        }
    }
}

// Kill all active children (reverse start order).  Used on supervisor shutdown.
static void supervisor_kill_all_children(Trix *trx, vm_offset_t state_offset) {
    auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
    for (length_t i = state->child_count; i > 0; --i) {
        supervisor_kill_child(trx, state_offset, static_cast<length_t>(i - 1));
    }
}

// Check restart intensity.  Returns true if restart is allowed, false if exceeded.
// On success, records a new timestamp.
static bool supervisor_check_intensity(Trix *trx, vm_offset_t state_offset) {
    auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
    auto timestamps = supervisor_timestamps(trx, state_offset);

    if (state->max_intensity != 0) {
        auto now_ms = static_cast<uint32_t>((monotonic_ns() / 1'000'000ULL) & 0xFFFFFFFFu);

        // Compact: drop entries older than the window.  Compare ages via unsigned
        // modular subtraction so the ~49.7-day uint32_t-ms wrap is handled (a
        // clamped cutoff would briefly retain pre-wrap timestamps after a wrap).
        // period_ms is validated > 0 in supervisor_op, so a just-recorded
        // timestamp (age 0 < period_ms) is always kept.
        uint32_t write = 0;
        for (uint32_t i = 0; i < state->restart_count; ++i) {
            if (static_cast<uint32_t>(now_ms - timestamps[i]) < state->period_ms) {
                timestamps[write++] = timestamps[i];
            }
        }
        state->restart_count = write;

        // Check if we can restart
        auto can_restart = write < state->max_intensity;
        if (can_restart) {
            // Record this restart
            timestamps[state->restart_count++] = now_ms;
        }
        return can_restart;
    } else {
        return false;
    }
}

// Handle a /down message.  Returns true if the supervisor died (intensity exceeded).
static bool supervisor_handle_down(Trix *trx, vm_offset_t state_offset, Object payload) {
    if (!payload.is_dict()) {
        return false;  // malformed
    } else {
        auto dict = payload.dict_value(trx);
        auto ref_obj = dict->get(trx, trx->wellknown_name(WellKnownName::RefKey));
        auto actor_obj = dict->get(trx, trx->wellknown_name(WellKnownName::ActorKey));
        auto reason_obj = dict->get(trx, trx->wellknown_name(WellKnownName::ReasonKey));
        if ((ref_obj == nullptr) || (actor_obj == nullptr) || (reason_obj == nullptr)) {
            return false;  // malformed
        } else {
            auto ref_id = static_cast<uint32_t>(ref_obj->integer_value());
            auto actor_offset = actor_obj->coroutine_offset();
            auto reason = *reason_obj;
            const bool is_normal =
                    reason.is_name() && (reason.name_offset() == trx->wellknown_name(WellKnownName::Normal).name_offset());

            // Find child by ref_id and actor handle
            auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
            auto children = supervisor_children(trx, state_offset);
            length_t child_idx = state->child_count;  // sentinel: not found
            for (length_t i = 0; i < state->child_count; ++i) {
                if ((children[i].monitor_ref == ref_id) && (children[i].actor == actor_offset)) {
                    child_idx = i;
                    break;
                }
            }

            if (child_idx >= state->child_count) {
                return false;  // stale message (child already restarted)
            } else {
                // Mark child as inactive
                children[child_idx].active = 0;
                children[child_idx].actor = nulloffset;

                // Decide if restart is needed based on policy
                bool should_restart = false;
                switch (children[child_idx].policy) {
                case SupervisorState::Permanent:
                    should_restart = true;
                    break;

                case SupervisorState::Temporary:
                    should_restart = false;
                    break;

                case SupervisorState::Transient:
                    should_restart = !is_normal;
                    break;
                }

                if (!should_restart) {
                    return false;
                } else {
                    // Check restart intensity
                    if (!supervisor_check_intensity(trx, state_offset)) {
                        // Intensity exceeded: kill all children and die with /shutdown
                        supervisor_kill_all_children(trx, state_offset);
                        --trx->m_exec_ptr;  // pop state companion from exec stack
                        trx->coroutine_flush_running();
                        auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
                        trx->coroutine_kill(ctx, trx->wellknown_name(WellKnownName::Shutdown));
                        trx->coroutine_schedule();
                        return true;
                    } else {
                        // Execute restart strategy
                        state = trx->offset_to_ptr<SupervisorState>(state_offset);
                        children = supervisor_children(trx, state_offset);

                        switch (state->strategy) {
                        case SupervisorState::OneForOne:
                            supervisor_spawn_child(trx, trx->m_running_coroutine, state_offset, child_idx);
                            break;

                        case SupervisorState::OneForAll: {
                            // Kill all other active children (reverse order); mark the children this
                            // wave actually terminates (transient padding marker) so only they --
                            // plus the failed child -- are restarted.  An already-stopped /temporary
                            // child must not be resurrected by a sibling failure.
                            state = trx->offset_to_ptr<SupervisorState>(state_offset);
                            for (length_t i = state->child_count; i > 0; --i) {
                                auto idx = static_cast<length_t>(i - 1);
                                children = supervisor_children(trx, state_offset);
                                if ((idx != child_idx) && children[idx].active) {
                                    children[idx].padding = 1;
                                    supervisor_kill_child(trx, state_offset, idx);
                                }
                            }
                            // Restart the failed child and every child we just terminated, start order.
                            state = trx->offset_to_ptr<SupervisorState>(state_offset);
                            for (length_t i = 0; i < state->child_count; ++i) {
                                children = supervisor_children(trx, state_offset);
                                if ((i == child_idx) || (children[i].padding == 1)) {
                                    children[i].padding = 0;
                                    supervisor_spawn_child(trx, trx->m_running_coroutine, state_offset, i);
                                }
                            }
                            break;
                        }

                        case SupervisorState::RestForOne: {
                            // Kill children AFTER the failed one (reverse order); mark the ones this
                            // wave terminates so /temporary children that already exited are not
                            // resurrected.
                            state = trx->offset_to_ptr<SupervisorState>(state_offset);
                            for (length_t i = state->child_count; i > (child_idx + 1); --i) {
                                auto idx = static_cast<length_t>(i - 1);
                                children = supervisor_children(trx, state_offset);
                                if (children[idx].active) {
                                    children[idx].padding = 1;
                                    supervisor_kill_child(trx, state_offset, idx);
                                }
                            }
                            // Restart the failed child and every child we just terminated.
                            state = trx->offset_to_ptr<SupervisorState>(state_offset);
                            for (length_t i = child_idx; i < state->child_count; ++i) {
                                children = supervisor_children(trx, state_offset);
                                if ((i == child_idx) || (children[i].padding == 1)) {
                                    children[i].padding = 0;
                                    supervisor_spawn_child(trx, trx->m_running_coroutine, state_offset, i);
                                }
                            }
                            break;
                        }
                        }

                        return false;
                    }
                }
            }
        }
    }
}

// Enter the supervisor recv loop: process all /down messages, then block.
// Exec stack on entry: ... | state_offset_integer  (companion at m_exec_ptr)
static void supervisor_enter_recv(Trix *trx) {
    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);

    while (mbx->m_count > 0) {
        // Pop head message
        auto data = mailbox_data(mbx);
        auto message = data[mbx->m_head];
        mbx->m_head = static_cast<length_t>((mbx->m_head + 1) % mbx->m_capacity);
        --mbx->m_count;

        // Process tagged control messages.  A recognized control message is consumed
        // in place by its handler (which owns tag_data); only messages that fall
        // through unconsumed are freed below.
        auto message_consumed = false;
        if (message.is_tagged()) {
            auto tag_data = message.tagged_storage(trx);
            if (tag_data[0].is_name()) {
                auto tag_offset = tag_data[0].name_offset();
                auto state_offset = static_cast<vm_offset_t>(trx->m_exec_ptr->integer_value());

                if (tag_offset == trx->wellknown_name(WellKnownName::Down).name_offset()) {
                    if (supervisor_handle_down(trx, state_offset, tag_data[1])) {
                        return;  // supervisor died
                    }
                    ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
                    mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
                    message_consumed = true;
                } else if (tag_offset == trx->wellknown_offset(WellKnownName::StartChild)) {
                    supervisor_handle_start_child(trx, state_offset, tag_data[1]);
                    ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
                    mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
                    message_consumed = true;
                } else if (tag_offset == trx->wellknown_offset(WellKnownName::TerminateChild)) {
                    supervisor_handle_terminate_child(trx, state_offset, tag_data[1]);
                    ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
                    mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
                    message_consumed = true;
                } else if (tag_offset == trx->wellknown_offset(WellKnownName::RestartChild)) {
                    supervisor_handle_restart_child(trx, state_offset, tag_data[1]);
                    ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
                    mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
                    message_consumed = true;
                } else if (tag_offset == trx->wellknown_offset(WellKnownName::SupStop)) {
                    if (supervisor_handle_stop(trx, state_offset)) {
                        return;  // supervisor died
                    }
                }
            }
        }
        if (!message_consumed) {
            // Discard unrecognized messages
            message.maybe_free_extvalue(trx);
        }
    }

    // No more messages: block until delivery wakes us
    mbx->m_blocked_reader = trx->m_running_coroutine;
    trx->require_exec_capacity(1);
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atSupervisorCheck);
    trx->coroutine_flush_running();
    auto running = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    running->m_status = CoroutineContext::Sleeping;
    running->m_flags |= CoroutineContext::FlagBlocked;
    running->m_wake_time_ns = std::numeric_limits<uint64_t>::max();
    trx->coroutine_schedule();
}

// supervisor: dict -- coroutine
// Validate spec, allocate supervisor state, spawn supervisor actor, then
// synchronously spawn + monitor every child in the spec.  By the time this
// op returns, all children are Ready and registered on SupervisorState, so
// supervisor-get-child / supervisor-which-children work immediately.
// Matches Erlang OTP's supervisor:start_link contract.
// Spec: << /strategy name /intensity int /period int /children [ child-specs ] >>
// Child spec: << /id name /start proc /restart name  [/capacity int] >>
// throws: limit-check, opstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void supervisor_op(Trix *trx) {
    trx->verify_operands(VerifyDict);
    auto spec_obj = *trx->m_op_ptr--;

    // Root the popped spec across pass-1 validation.  The Name::make interning below
    // can fire vm_global_gc (global mode + a not-yet-interned name), and spec_obj was
    // popped into a C-local, so a ${...}-global spec would be swept -- corrupting the
    // re-fetched dict and the child-spec reads in pass 1.  Reset before the op-stack
    // re-root below, which covers pass 2 (and must leave the gc-root stack empty for
    // the coroutine_launch_common in pass 2).
    trx->gc_root_push_oneoff(spec_obj);

    auto dict = spec_obj.dict_value(trx);

    // Pre-intern validation key names (allocated once, before state allocation)
    auto strategy_key = Name::make(trx, "strategy");
    auto intensity_key = Name::make(trx, "intensity");
    auto period_key = Name::make(trx, "period");
    auto children_key = Name::make(trx, "children");
    auto id_key = Name::make(trx, "id");
    auto start_key = Name::make(trx, "start");
    auto restart_key = Name::make(trx, "restart");
    auto capacity_key = Name::make(trx, "capacity");
    auto one_for_one_name = Name::make(trx, "one-for-one");
    auto one_for_all_name = Name::make(trx, "one-for-all");
    auto rest_for_one_name = Name::make(trx, "rest-for-one");
    auto permanent_name = Name::make(trx, "permanent");
    auto temporary_name = Name::make(trx, "temporary");
    auto transient_name = Name::make(trx, "transient");

    // Re-fetch dict after Name::make allocations
    dict = spec_obj.dict_value(trx);

    // Validate /strategy
    auto strategy_val = dict->get(trx, strategy_key);
    if ((strategy_val == nullptr) || !strategy_val->is_name()) {
        trx->error(Error::TypeCheck, "supervisor: /strategy must be a name (/one-for-one, /one-for-all, /rest-for-one)");
    } else {
        SupervisorState::Strategy strategy;
        auto strat_offset = strategy_val->name_offset();
        if (strat_offset == one_for_one_name.name_offset()) {
            strategy = SupervisorState::OneForOne;
        } else if (strat_offset == one_for_all_name.name_offset()) {
            strategy = SupervisorState::OneForAll;
        } else if (strat_offset == rest_for_one_name.name_offset()) {
            strategy = SupervisorState::RestForOne;
        } else {
            trx->error(Error::RangeCheck, "supervisor: /strategy must be /one-for-one, /one-for-all, or /rest-for-one");
        }

        // Validate /intensity
        auto intensity_val = dict->get(trx, intensity_key);
        if ((intensity_val == nullptr) || !intensity_val->is_integer()) {
            trx->error(Error::TypeCheck, "supervisor: /intensity must be a non-negative integer");
        } else {
            auto intensity = intensity_val->integer_value();
            if (intensity < 0) {
                trx->error(Error::RangeCheck, "supervisor: /intensity must be non-negative");
            } else {
                // Validate /period
                auto period_val = dict->get(trx, period_key);
                if ((period_val == nullptr) || !period_val->is_integer()) {
                    trx->error(Error::TypeCheck, "supervisor: /period must be a positive integer (milliseconds)");
                } else {
                    auto period = period_val->integer_value();
                    if (period <= 0) {
                        trx->error(Error::RangeCheck, "supervisor: /period must be positive");
                    } else {
                        // Validate /children
                        auto children_val = dict->get(trx, children_key);
                        if ((children_val == nullptr) || !children_val->is_array()) {
                            trx->error(Error::TypeCheck, "supervisor: /children must be an array of child spec dicts");
                        } else {
                            auto [child_specs, child_count] = children_val->array_value(trx);
                            if (child_count == 0) {
                                trx->error(Error::RangeCheck, "supervisor: /children must not be empty");
                            } else {
                                auto max_children = static_cast<length_t>(child_count + 8);

                                // Pass 1: validate every child spec BEFORE launching the supervisor
                                // coroutine, so a malformed spec errors cleanly without leaving a half-built
                                // supervisor in the scheduler.  The SupervisorState is allocated + filled
                                // AFTER the launch (pass 2) so it can be rooted on the supervisor context
                                // (FlagSupervisor + m_return_value) the instant it exists -- never unrooted
                                // across a global allocation (coroutine launch / mailbox / child spawn) that
                                // could fire vm_global_gc and sweep it.
                                for (length_t i = 0; i < child_count; ++i) {
                                    if (!child_specs[i].is_dict()) {
                                        trx->error(Error::TypeCheck, "supervisor: child spec {} must be a dict", i);
                                    } else {
                                        auto cdict = child_specs[i].dict_value(trx);
                                        auto cid = cdict->get(trx, id_key);
                                        if ((cid == nullptr) || !cid->is_name()) {
                                            trx->error(Error::TypeCheck, "supervisor: child {}: /id must be a name", i);
                                        } else {
                                            auto cstart = cdict->get(trx, start_key);
                                            if (cstart == nullptr) {
                                                trx->error(Error::TypeCheck, "supervisor: child {}: /start is required", i);
                                            } else if (cstart->is_literal() || !cstart->is_callable()) {
                                                trx->error(Error::TypeCheck,
                                                           "supervisor: child {}: /start must be executable (array, packed, or "
                                                           "curry)",
                                                           i);
                                            } else {
                                                auto crestart = cdict->get(trx, restart_key);
                                                if ((crestart == nullptr) || !crestart->is_name()) {
                                                    trx->error(
                                                            Error::TypeCheck, "supervisor: child {}: /restart must be a name", i);
                                                } else {
                                                    auto rname_offset = crestart->name_offset();
                                                    if ((rname_offset != permanent_name.name_offset()) &&
                                                        (rname_offset != temporary_name.name_offset()) &&
                                                        (rname_offset != transient_name.name_offset())) {
                                                        trx->error(Error::RangeCheck,
                                                                   "supervisor: child {}: /restart must be /permanent, "
                                                                   "/temporary, or /transient",
                                                                   i);
                                                    }
                                                    auto ccap = cdict->get(trx, capacity_key);
                                                    if (ccap != nullptr) {
                                                        if (!ccap->is_integer()) {
                                                            trx->error(Error::TypeCheck,
                                                                       "supervisor: child {}: /capacity must be a positive "
                                                                       "integer",
                                                                       i);
                                                        } else if (ccap->integer_value() <= 0) {
                                                            trx->error(Error::RangeCheck,
                                                                       "supervisor: child {}: /capacity must be positive",
                                                                       i);
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                // Pass 1 done: hand spec_obj's rooting from the gc-root stack to the operand
                                // stack for pass 2.  No global allocation occurs between the pop and the push,
                                // and the empty gc-root stack is required before pass 2's coroutine_launch_common.
                                trx->gc_root_pop_oneoff();

                                // Keep the spec dict rooted on the op stack across the launch + state
                                // allocation: it was popped into the C local spec_obj (not a GC root), and
                                // pass 2 still reads child_specs out of it after allocations that can GC.
                                trx->require_op_capacity(1);
                                *++trx->m_op_ptr = spec_obj;

                                // Create supervisor proc: a 1-element array containing @supervisor-init.
                                // When the supervisor coroutine runs, the interpreter executes this proc,
                                // which pushes @supervisor-init onto the exec stack -> at_supervisor_init_op.
                                // It lives on the supervisor's exec stack for the supervisor's lifetime, so
                                // it is global (CoroutineContext + stacks are restore-immune; a sl>0 local
                                // Array offset on the surviving exec stack would trip the cross-restore
                                // validator).
                                auto [proc_storage, proc_offset] = trx->gvm_alloc_n<Object>(1, Trix::ChunkKind::Array);
                                trx->gvm_set_obj_count(proc_offset, 1);
                                proc_storage[0] = Object::make_control_operator(SystemName::atSupervisorInit);
                                auto proc = Object::make_array(proc_offset, 1, Object::ExecutableAttrib);
                                proc.set_save_level(Save::BASE);

                                // Launch the supervisor coroutine with NO op-stack parameter -- the
                                // state_offset reaches @supervisor-init via the context's m_return_value
                                // (set in pass 2), keeping the state rooted from allocation onward.
                                trx->require_op_capacity(2);
                                *++trx->m_op_ptr = Object::make_mark();
                                *++trx->m_op_ptr = proc;

                                auto [sup_ctx, sup_offset] = coroutine_launch_common(trx, "supervisor");

                                // Pass 2: allocate + populate the SupervisorState now that the context
                                // exists.  Route it through global VM so a supervisor spawned in a save
                                // scope survives an outer restore (CoroutineContext + stacks are global; a
                                // local state would be rejected by the cross-coroutine restore validator).
                                auto state_size = static_cast<vm_size_t>(sizeof(SupervisorState) +
                                                                         max_children * sizeof(SupervisorState::ChildEntry) +
                                                                         static_cast<uint32_t>(intensity) * sizeof(uint32_t));
                                auto [state, state_offset] =
                                        trx->gvm_alloc<SupervisorState>(state_size, Trix::ChunkKind::Supervisor);
                                state->strategy = strategy;
                                state->padding1[0] = state->padding1[1] = state->padding1[2] = 0;
                                state->max_intensity = static_cast<uint32_t>(intensity);
                                state->period_ms = static_cast<uint32_t>(period);
                                state->child_count = child_count;
                                state->max_children = max_children;
                                state->restart_count = 0;
                                state->padding3 = 0;

                                // Zero timestamps
                                auto timestamps = supervisor_timestamps(trx, state_offset);
                                for (uint32_t i = 0; i < static_cast<uint32_t>(intensity); ++i) {
                                    timestamps[i] = 0;
                                }

                                // Zero-initialize all ChildEntry slots (including headroom)
                                auto entries = supervisor_children(trx, state_offset);
                                std::memset(entries, 0, max_children * sizeof(SupervisorState::ChildEntry));

                                // Fill each child entry (re-fetch child_specs after the state allocation;
                                // every field was validated in pass 1, so there is no error path here).
                                child_specs = children_val->array_objects(trx);
                                entries = supervisor_children(trx, state_offset);
                                for (length_t i = 0; i < child_count; ++i) {
                                    auto cdict = child_specs[i].dict_value(trx);
                                    auto cid = cdict->get(trx, id_key);
                                    auto cstart = cdict->get(trx, start_key);
                                    auto crestart = cdict->get(trx, restart_key);
                                    auto rname_offset = crestart->name_offset();
                                    SupervisorState::RestartPolicy policy;
                                    if (rname_offset == permanent_name.name_offset()) {
                                        policy = SupervisorState::Permanent;
                                    } else if (rname_offset == temporary_name.name_offset()) {
                                        policy = SupervisorState::Temporary;
                                    } else {
                                        policy = SupervisorState::Transient;
                                    }
                                    length_t capacity = 0;
                                    auto ccap = cdict->get(trx, capacity_key);
                                    if (ccap != nullptr) {
                                        capacity = static_cast<length_t>(ccap->integer_value());
                                    }
                                    entries[i].id = *cid;
                                    entries[i].start = *cstart;
                                    entries[i].actor = nulloffset;
                                    entries[i].monitor_ref = 0;
                                    entries[i].capacity = capacity;
                                    entries[i].policy = policy;
                                    entries[i].active = 0;
                                    entries[i].padding = 0;
                                }

                                // Root the state on the context (gc_walk_coroutine_context marks it via the
                                // FlagSupervisor gate, so it survives the mailbox + child-spawn allocations
                                // below and any later vm_global_gc) and mark the supervisor a trap-exit actor.
                                sup_ctx = trx->offset_to_ptr<CoroutineContext>(sup_offset);
                                sup_ctx->m_return_value = Object::make_integer(static_cast<integer_t>(state_offset));
                                sup_ctx->m_flags |=
                                        static_cast<uint8_t>(CoroutineContext::FlagWasActor | CoroutineContext::FlagSupervisor);
                                sup_ctx->m_trap_exit = true;

                                // Allocate mailbox (capacity = max_children + headroom for control messages)
                                auto mbox_cap = static_cast<length_t>(max_children + 4);
                                auto [mbx, mbx_offset] = actor_alloc_mailbox(trx, mbox_cap);
                                sup_ctx = trx->offset_to_ptr<CoroutineContext>(sup_offset);
                                sup_ctx->m_mailbox = mbx_offset;

                                // Spawn and monitor every child synchronously, in the caller's context,
                                // with the supervisor as the monitor source.  Children are Ready but will
                                // not run until scheduled; their handles are populated on the ChildEntry
                                // before we return, so supervisor-get-child works immediately.
                                auto state_after = trx->offset_to_ptr<SupervisorState>(state_offset);
                                auto initial_child_count = state_after->child_count;
                                for (length_t i = 0; i < initial_child_count; ++i) {
                                    supervisor_spawn_child(trx, sup_offset, state_offset, i);
                                }

                                // Drop the spec-dict root, then push the supervisor handle.
                                --trx->m_op_ptr;
                                trx->require_op_capacity(1);
                                *++trx->m_op_ptr = Object::make_coroutine(sup_offset);
                            }
                        }
                    }
                }
            }
        }
    }
}

// supervisor-which-children: coroutine -- array
// Returns an array of dicts for each active child: << /id name /actor handle /restart policy >>
// throws: invalid-access, opstack-overflow, opstack-underflow, type-check, vm-full
static void supervisor_which_children_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);
    auto target_offset = trx->m_op_ptr->coroutine_offset();
    --trx->m_op_ptr;

    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
    if (target->m_status == CoroutineContext::Dead) {
        trx->error(Error::InvalidAccess, "supervisor-which-children: supervisor is dead");
    } else if (target->m_return_value.is_null() || !target->m_return_value.is_integer()) {
        trx->error(Error::TypeCheck, "supervisor-which-children: argument is not a supervisor");
    } else {
        auto state_offset = static_cast<vm_offset_t>(target->m_return_value.integer_value());
        auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
        auto children = supervisor_children(trx, state_offset);

        // Count active children
        length_t active_count = 0;
        for (length_t i = 0; i < state->child_count; ++i) {
            if (children[i].active) {
                ++active_count;
            }
        }

        // Pre-intern names used in result dicts
        auto id_key = Name::make(trx, "id");
        auto restart_key = Name::make(trx, "restart");
        auto permanent_name = Name::make(trx, "permanent");
        auto temporary_name = Name::make(trx, "temporary");
        auto transient_name = Name::make(trx, "transient");

        // Allocate result array
        auto result = trx->vm_alloc_n_ptr<Object>(active_count);
        auto result_offset = trx->ptr_to_offset(result);

        // Build a dict for each active child
        state = trx->offset_to_ptr<SupervisorState>(state_offset);
        children = supervisor_children(trx, state_offset);
        length_t j = 0;
        for (length_t i = 0; i < state->child_count; ++i) {
            if (children[i].active) {
                auto child_id = children[i].id;
                auto child_actor = children[i].actor;
                auto child_policy = children[i].policy;

                auto [d, d_offset] = Dict::create_or_recycle(trx, 4);
                d->put(trx, id_key, child_id);
                d->put(trx, trx->wellknown_name(WellKnownName::ActorKey), Object::make_coroutine(child_actor));
                Object policy_name;
                switch (child_policy) {
                case SupervisorState::Permanent:
                    policy_name = permanent_name;
                    break;

                case SupervisorState::Temporary:
                    policy_name = temporary_name;
                    break;

                case SupervisorState::Transient:
                    policy_name = transient_name;
                    break;
                }
                d->put(trx, restart_key, policy_name);

                auto entry_obj = Object::make_dict(d_offset);
                entry_obj.set_save_level(trx->m_curr_save_level);

                // Re-fetch result after Dict::create_dict allocations
                result = trx->offset_to_ptr<Object>(result_offset);
                result[j++] = entry_obj;

                // Re-fetch state for next iteration
                state = trx->offset_to_ptr<SupervisorState>(state_offset);
                children = supervisor_children(trx, state_offset);
            }
        }

        auto arr = Object::make_array(result_offset, active_count);
        arr.set_save_level(trx->m_curr_save_level);

        trx->require_op_capacity(1);
        *++trx->m_op_ptr = arr;
    }
}

//===--- Dynamic Child Management ---===//

// Helper: build a tagged message and send via actor-send trampoline.
// Pushes [msg, target] on op stack, actor-send on exec stack.
static void supervisor_send_control_message(Trix *trx, Object target, Object tag_name, Object payload) {
    trx->require_op_capacity(2);
    trx->require_exec_capacity(1);

    // Root payload across make_tagged: it allocates the [name, payload] pair BEFORE
    // storing payload, so a ${...}-global payload (the spec dict from
    // supervisor-start-child) passed by value would be swept by a GC during that
    // alloc, leaving a dangling tagged message.  tag_name is a well-known Name (a
    // permanent root) and target is a registry-rooted coroutine handle -- neither
    // needs rooting; only payload does.
    trx->gc_root_push_oneoff(payload);
    *++trx->m_op_ptr = Object::make_tagged(trx, tag_name, payload);
    trx->gc_root_pop_oneoff();

    *++trx->m_op_ptr = target;
    *++trx->m_exec_ptr = Object::make_operator(SystemName::ActorSend);
}

// supervisor-start-child: supervisor child-spec --
// Send /start-child message to supervisor.  Async (fire-and-forget).
// throws: opstack-underflow, type-check
static void supervisor_start_child_op(Trix *trx) {
    trx->verify_operands(VerifyDict, VerifyCoroutine);

    auto spec_obj = *trx->m_op_ptr--;
    auto target_obj = *trx->m_op_ptr--;

    supervisor_send_control_message(trx, target_obj, trx->wellknown_name(WellKnownName::StartChild), spec_obj);
}

// supervisor-terminate-child: supervisor child-id --
// Send /terminate-child message to supervisor.  Async (fire-and-forget).
// throws: opstack-underflow, type-check
static void supervisor_terminate_child_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyCoroutine);

    auto id_obj = *trx->m_op_ptr--;
    auto target_obj = *trx->m_op_ptr--;

    supervisor_send_control_message(trx, target_obj, trx->wellknown_name(WellKnownName::TerminateChild), id_obj);
}

// supervisor-stop: supervisor --
// Send /stop message to supervisor.  Async (fire-and-forget).
// Supervisor kills all children (reverse order) and exits with /shutdown.
// throws: opstack-underflow, type-check
static void supervisor_stop_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);

    auto target_obj = *trx->m_op_ptr--;

    supervisor_send_control_message(
            trx, target_obj, trx->wellknown_name(WellKnownName::SupStop), trx->wellknown_name(WellKnownName::Shutdown));
}

// supervisor-count-children: coroutine -- int
// Returns the number of active children.
// throws: opstack-underflow, type-check, invalid-access
static void supervisor_count_children_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);
    auto target_offset = trx->m_op_ptr->coroutine_offset();

    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
    if (target->m_status == CoroutineContext::Dead) {
        trx->error(Error::InvalidAccess, "supervisor-count-children: supervisor is dead");
    } else if (target->m_return_value.is_null() || !target->m_return_value.is_integer()) {
        trx->error(Error::TypeCheck, "supervisor-count-children: argument is not a supervisor");
    } else {
        auto state_offset = static_cast<vm_offset_t>(target->m_return_value.integer_value());
        auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
        auto children = supervisor_children(trx, state_offset);

        length_t count = 0;
        for (length_t i = 0; i < state->child_count; ++i) {
            if (children[i].active) {
                ++count;
            }
        }
        *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(count));
    }
}

// is-supervisor: any -- bool
// True if argument is a live actor whose return value holds a SupervisorState offset.
// Uses the same detection pattern as supervisor-count-children.
// throws: opstack-underflow
static void is_supervisor_op(Trix *trx) {
    trx->require_op_count(1);
    auto val_obj = *trx->m_op_ptr;
    auto result = false;
    if (val_obj.is_coroutine()) {
        auto ctx = val_obj.coroutine_context(trx);
        result = (ctx->m_mailbox != nulloffset) && (ctx->m_status != CoroutineContext::Dead) && ctx->m_return_value.is_integer();
    }
    *trx->m_op_ptr = Object::make_boolean(result);
}

// supervisor-spec: coroutine -- dict
// Returns a read-only dict with supervisor configuration:
//   /strategy, /intensity, /period, /child-count, /active-count,
//   /max-children, /restart-count
// throws: opstack-underflow, type-check, invalid-access
static void supervisor_spec_op(Trix *trx) {
    trx->verify_operands(VerifyCoroutine);
    auto target_offset = trx->m_op_ptr->coroutine_offset();

    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
    if (target->m_status == CoroutineContext::Dead) {
        trx->error(Error::InvalidAccess, "supervisor-spec: supervisor is dead");
    } else if (target->m_return_value.is_null() || !target->m_return_value.is_integer()) {
        trx->error(Error::TypeCheck, "supervisor-spec: argument is not a supervisor");
    } else {
        auto state_offset = static_cast<vm_offset_t>(target->m_return_value.integer_value());
        auto state = trx->offset_to_ptr<SupervisorState>(state_offset);

        // Capture values before dict allocation (which may move pointers)
        auto strategy = state->strategy;
        auto intensity = state->max_intensity;
        auto period = state->period_ms;
        auto child_count = state->child_count;
        auto max_children = state->max_children;
        auto restart_count = state->restart_count;

        // Count active children
        auto children = supervisor_children(trx, state_offset);
        length_t active_count = 0;
        for (length_t i = 0; i < child_count; ++i) {
            if (children[i].active) {
                ++active_count;
            }
        }

        auto [dict, dict_offset] = Dict::create_or_recycle(trx, 8);

        // /strategy
        Object strategy_name;
        switch (strategy) {
        case SupervisorState::OneForOne:
            strategy_name = Name::make(trx, "one-for-one");
            break;

        case SupervisorState::OneForAll:
            strategy_name = Name::make(trx, "one-for-all");
            break;

        case SupervisorState::RestForOne:
            strategy_name = Name::make(trx, "rest-for-one");
            break;
        }
        dict = trx->offset_to_ptr<Dict>(dict_offset);  // re-fetch after Name::make
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyStrategy), strategy_name);
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyIntensity), Object::make_integer(static_cast<integer_t>(intensity)));
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyPeriod), Object::make_integer(static_cast<integer_t>(period)));
        dict->put(
                trx, trx->wellknown_name(WellKnownName::IKeyChildCount), Object::make_integer(static_cast<integer_t>(child_count)));
        dict->put(trx,
                  trx->wellknown_name(WellKnownName::IKeyActiveCount),
                  Object::make_integer(static_cast<integer_t>(active_count)));
        dict->put(trx,
                  trx->wellknown_name(WellKnownName::IKeyMaxChildren),
                  Object::make_integer(static_cast<integer_t>(max_children)));
        dict->put(trx,
                  trx->wellknown_name(WellKnownName::IKeyRestartCount),
                  Object::make_integer(static_cast<integer_t>(restart_count)));

        dict->set_readonly_access_no_save();
        *trx->m_op_ptr = Object::make_dict(dict_offset);
    }
}

// supervisor-get-child: coroutine name -- coroutine true | false
// Looks up a child by its /id name.  Returns handle + true if active, false otherwise.
// throws: opstack-underflow, type-check, invalid-access
static void supervisor_get_child_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyCoroutine);

    auto id_obj = *trx->m_op_ptr--;
    auto target_obj = *trx->m_op_ptr;

    auto target_offset = target_obj.coroutine_offset();
    auto target = trx->offset_to_ptr<CoroutineContext>(target_offset);
    if (target->m_status == CoroutineContext::Dead) {
        trx->error(Error::InvalidAccess, "supervisor-get-child: supervisor is dead");
    } else if (target->m_return_value.is_null() || !target->m_return_value.is_integer()) {
        trx->error(Error::TypeCheck, "supervisor-get-child: argument is not a supervisor");
    } else {
        auto state_offset = static_cast<vm_offset_t>(target->m_return_value.integer_value());
        auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
        auto children = supervisor_children(trx, state_offset);
        auto target_name_offset = id_obj.name_offset();

        for (length_t i = 0; i < state->child_count; ++i) {
            if (children[i].active && (children[i].id.name_offset() == target_name_offset)) {
                *trx->m_op_ptr = Object::make_coroutine(children[i].actor);
                trx->require_op_capacity(1);
                *++trx->m_op_ptr = Object::make_boolean(true);
                return;
            }
        }
        *trx->m_op_ptr = Object::make_boolean(false);
    }
}

// supervisor-restart-child: coroutine name --
// Send /restart-child tagged message to supervisor.  Async (fire-and-forget).
// Restarts a previously terminated child by its /id name.
// throws: opstack-underflow, type-check
static void supervisor_restart_child_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyCoroutine);

    auto id_obj = *trx->m_op_ptr--;
    auto target_obj = *trx->m_op_ptr--;

    supervisor_send_control_message(trx, target_obj, trx->wellknown_name(WellKnownName::RestartChild), id_obj);
}

// Handler: process /restart-child control message inside supervisor recv loop.
// Finds child by id; if inactive, respawns it.
static void supervisor_handle_restart_child(Trix *trx, vm_offset_t state_offset, Object payload) {
    if (payload.is_name()) {
        auto target_name_offset = payload.name_offset();
        auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
        auto children = supervisor_children(trx, state_offset);

        for (length_t i = 0; i < state->child_count; ++i) {
            if ((children[i].id.name_offset() == target_name_offset) && !children[i].active) {
                supervisor_spawn_child(trx, trx->m_running_coroutine, state_offset, i);
                return;
            }
        }
        // Not found or already active: silently discard
    }
}

// Parse + validate a /start-child spec dict.  Fills the outputs and returns
// true for a well-formed spec; false (discard) for any malformed field.
static bool supervisor_parse_child_spec(Trix *trx,
                                        Object payload,
                                        Object *out_id,
                                        Object *out_start,
                                        SupervisorState::RestartPolicy *out_policy,
                                        length_t *out_capacity) {
    if (!payload.is_dict()) {
        return false;  // malformed, discard
    }

    auto dict = payload.dict_value(trx);
    auto id_key = Name::make(trx, "id");
    auto start_key = Name::make(trx, "start");
    auto restart_key = Name::make(trx, "restart");
    auto capacity_key = Name::make(trx, "capacity");
    auto permanent_name = Name::make(trx, "permanent");
    auto temporary_name = Name::make(trx, "temporary");
    auto transient_name = Name::make(trx, "transient");

    // Re-fetch dict after Name::make allocations
    dict = payload.dict_value(trx);

    auto cid = dict->get(trx, id_key);
    if ((cid == nullptr) || !cid->is_name()) {
        return false;  // malformed
    }

    auto cstart = dict->get(trx, start_key);
    if ((cstart == nullptr) || cstart->is_literal() || !cstart->is_callable()) {
        return false;  // malformed
    }

    auto crestart = dict->get(trx, restart_key);
    if ((crestart == nullptr) || !crestart->is_name()) {
        return false;  // malformed
    }

    auto rname_offset = crestart->name_offset();
    if (rname_offset == permanent_name.name_offset()) {
        *out_policy = SupervisorState::Permanent;
    } else if (rname_offset == temporary_name.name_offset()) {
        *out_policy = SupervisorState::Temporary;
    } else if (rname_offset == transient_name.name_offset()) {
        *out_policy = SupervisorState::Transient;
    } else {
        return false;  // malformed
    }

    *out_capacity = 0;
    auto ccap = dict->get(trx, capacity_key);
    if ((ccap != nullptr) && ccap->is_integer() && (ccap->integer_value() > 0)) {
        *out_capacity = static_cast<length_t>(ccap->integer_value());
    }

    *out_id = *cid;
    *out_start = *cstart;
    return true;
}

// Handler: process /start-child control message inside supervisor recv loop.
// Validates spec, finds an empty slot, spawns and monitors the new child.
static void supervisor_handle_start_child(Trix *trx, vm_offset_t state_offset, Object payload) {
    auto cid_obj = Object::make_null();
    auto cstart_obj = Object::make_null();
    auto policy = SupervisorState::Permanent;
    length_t capacity = 0;
    if (supervisor_parse_child_spec(trx, payload, &cid_obj, &cstart_obj, &policy, &capacity)) {
        // Reject a duplicate /id, respect capacity, then fill the slot + spawn.
        auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
        auto children = supervisor_children(trx, state_offset);
        auto duplicate = false;
        for (length_t i = 0; i < state->child_count; ++i) {
            if (children[i].active && (children[i].id.name_offset() == cid_obj.name_offset())) {
                duplicate = true;  // discard silently
                break;
            }
        }
        if (!duplicate && (state->child_count < state->max_children)) {
            // Fill new entry at end
            auto idx = state->child_count;
            children[idx].id = cid_obj;
            children[idx].start = cstart_obj;
            children[idx].actor = nulloffset;
            children[idx].monitor_ref = 0;
            children[idx].capacity = capacity;
            children[idx].policy = policy;
            children[idx].active = 0;
            children[idx].padding = 0;
            state->child_count = static_cast<length_t>(idx + 1);

            // Spawn the child
            supervisor_spawn_child(trx, trx->m_running_coroutine, state_offset, idx);
        }
    }
}

// Handler: process /terminate-child control message inside supervisor recv loop.
// Finds child by id, kills it, marks inactive.  No restart on subsequent /down
// because supervisor_kill_child demonitors first.
static void supervisor_handle_terminate_child(Trix *trx, vm_offset_t state_offset, Object payload) {
    if (payload.is_name()) {
        auto target_name_offset = payload.name_offset();
        auto state = trx->offset_to_ptr<SupervisorState>(state_offset);
        auto children = supervisor_children(trx, state_offset);

        for (length_t i = 0; i < state->child_count; ++i) {
            if (children[i].active && (children[i].id.name_offset() == target_name_offset)) {
                supervisor_kill_child(trx, state_offset, i);
                return;
            }
        }
        // Not found: silently discard
    }
}

// Handler: process /stop control message inside supervisor recv loop.
// Kills all children in reverse order, then exits supervisor with /shutdown.
static bool supervisor_handle_stop(Trix *trx, vm_offset_t state_offset) {
    supervisor_kill_all_children(trx, state_offset);
    --trx->m_exec_ptr;  // pop state companion from exec stack
    trx->coroutine_flush_running();
    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    trx->coroutine_kill(ctx, trx->wellknown_name(WellKnownName::Shutdown));
    trx->coroutine_schedule();
    return true;
}

//===--- Control Operators ---===//

// @supervisor-init: (control operator)
// Called when the supervisor coroutine first runs.  Children are already
// spawned and monitored synchronously in supervisor_op (the caller's
// context); m_trap_exit is already set there too.  This op just moves the
// state_offset from the op stack to the exec stack and enters the recv loop.
static void at_supervisor_init_op(Trix *trx) {
    // The state_offset is carried on the supervisor context's m_return_value
    // (set in supervisor_op before this coroutine was scheduled), NOT the op
    // stack -- passing it via the context keeps the SupervisorState rooted from
    // the moment it is allocated, with no op-stack-parameter window.
    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    auto state_offset = static_cast<vm_offset_t>(ctx->m_return_value.integer_value());

    // Push state_offset as exec stack companion (persists across recv iterations)
    trx->require_exec_capacity(1);
    *++trx->m_exec_ptr = Object::make_integer(static_cast<integer_t>(state_offset));

    // Enter recv loop
    supervisor_enter_recv(trx);
}

// @supervisor-check: (control operator)
// Wakeup handler for the supervisor recv loop.  Reads state_offset from exec
// companion, clears blocked reader, and re-enters the recv loop.
static void at_supervisor_check_op(Trix *trx) {
    // Companion is at m_exec_ptr (interpreter already popped @supervisor-check)
    auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    auto mbx = trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
    mbx->m_blocked_reader = nulloffset;

    supervisor_enter_recv(trx);
}

//===--- GC walkers ---===//
//
// gc_walk_supervisor: walks every live ChildEntry in a supervisor
// state block.  Each ChildEntry carries two Object cells (id, start
// proc) and one coroutine-block offset (actor handle).  Iterates
// only [0..child_count) -- entries beyond that index are unused
// allocator slack with stale bytes; gc_mark_object's defensive checks
// would skip them but explicit bounding is cheaper.
//
// monitor_ref / capacity / policy / active are metadata; padding
// is unused.  The timestamps array trailing the ChildEntry array
// holds uint32_t milliseconds, no Object cells.
//
void gc_walk_supervisor(vm_offset_t payload_offset) {
    auto *state = offset_to_ptr<SupervisorState>(payload_offset);
    auto *children = reinterpret_cast<SupervisorState::ChildEntry *>(state + 1);
    for (length_t i = 0; i < state->child_count; ++i) {
        gc_mark_object(children[i].id);
        gc_mark_object(children[i].start);
        mark_global_offset(children[i].actor);
    }
}
