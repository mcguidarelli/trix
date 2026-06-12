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

//===--- GenServer Operators ---===//
//
// Standardized actor patterns inspired by Erlang's gen_server.
// A GenServer is an actor running a receive loop that dispatches to
// user-defined handler procs for calls (sync), casts (async), and
// info (non-protocol) messages.
//
// Internal message protocol (not user-visible):
//   Call: 4-element array [/gen-call, ref, from-coroutine, user-message]
//   Cast: 2-element array [/gen-cast, user-message]
//   Stop: 2-element array [/gen-stop, reason]
//   Reply: 2-element array [ref, reply-value]
//
// Exec stack frame for gen-server control operators (4 slots, fixed):
//   frame[-3] = spec-dict (immutable, contains handler procs)
//   frame[-2] = state (current server state, updated each iteration)
//   frame[-1] = call-ref (pending call ref, null when idle)
//   frame[0]  = call-from (pending caller handle, null when idle)
//
//   gen-server         spec-dict -- coroutine
//   gen-call           server message -- reply
//   gen-call-timeout   server message ms -- reply
//   gen-cast           server message --
//   gen-stop           server reason --
//   gen-reply          from-token reply --
//
// (No gen-state: state queries belong in a /query handle-call arm.)

// Helper: look up a proc in the spec dict.  Returns nullptr if not found.
[[nodiscard]] static Object *gen_spec_get(Trix *trx, Object *spec_obj, Object key) {
    auto spec_dict = spec_obj->dict_value(trx);
    return spec_dict->get(trx, key);
}

// Helper: free ExtValues owned by elements inside a message (array or scalar).
static void gen_free_message(Trix *trx, Object msg_obj) {
    if (msg_obj.is_array()) {
        auto [elements, count] = msg_obj.array_value(trx);
        for (length_t i = 0; i < count; ++i) {
            elements[i].maybe_free_extvalue(trx);
        }
    } else {
        msg_obj.maybe_free_extvalue(trx);
    }
}

// Helper: start the recv loop by pushing actor-recv + @gen-server-recv.
static void gen_start_recv(Trix *trx) {
    trx->require_exec_capacity(2);
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atGenServerRecv);
    *++trx->m_exec_ptr = Object::make_operator(SystemName::ActorRecv);
}

// gen-server: spec-dict -- coroutine
// Spawns a GenServer actor.  The spec-dict must contain at least /init.
// The actor runs: call /init to get state, then enter the receive loop.
//
// The startup proc is: { spec-literal init-proc @gen-server-init }
// When executed in the new coroutine:
//   1. spec-literal pushes spec-dict onto op stack
//   2. init-proc executes (state result on op stack: spec state)
//   3. @gen-server-init fires: pops state + spec from op stack, builds
//      the persistent exec frame, starts the recv loop.
// throws: opstack-overflow, opstack-underflow, type-check, undefined, vm-full
static void gen_server_op(Trix *trx) {
    trx->verify_operands(VerifyDict);

    auto spec_obj = *trx->m_op_ptr;
    auto spec_dict = spec_obj.dict_value(trx);

    // Verify /init exists
    auto init_proc = spec_dict->get(trx, trx->wellknown_name(WellKnownName::Init));
    if (init_proc == nullptr) {
        trx->error(Error::Undefined, "gen-server: spec-dict must contain /init");
    } else {
        // Build startup procedure: [spec(literal), mark, init-proc, exec, @gen-server-init]
        // spec is literal so it gets pushed to op stack when the proc executes.
        // The mark is pushed between spec and init-proc's invocation so that
        // @gen-server-init can detect if /init left the wrong number of values
        // on the op stack.  init-proc is pushed as data by pushop_direct
        // (Array/Packed inside a proc body are treated as values, not executed).
        // exec then pops it from the op stack and executes it on the exec stack,
        // producing the initial state.  @gen-server-init fires afterwards and
        // verifies [spec, mark, state] by checking the mark's position.
        auto spec_literal = spec_obj.make_clone(trx);
        spec_literal.set_literal();

        auto [startup_elements, startup_offset] = trx->vm_alloc_n<Object>(5);
        startup_elements[0] = spec_literal;
        startup_elements[1] = Object::make_mark();
        startup_elements[2] = init_proc->make_clone(trx);
        startup_elements[3] = Object::make_operator(SystemName::Exec);
        startup_elements[4] = Object::make_control_operator(SystemName::atGenServerInit);

        auto startup_proc = Object::make_array(startup_offset, 5, Object::ExecutableAttrib, Object::ReadOnlyAccess);

        // Replace spec-dict on op stack with mark + startup proc for actor-spawn
        *trx->m_op_ptr = Object::make_mark();
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = startup_proc;

        // Delegate to actor-spawn
        actor_spawn_op(trx);
    }
}

// @gen-server-init: after /init proc returns, op stack has: spec mark state.
// Build the 4-slot exec frame and start the recv loop.
// This control op has popcount=0 since there's no existing frame yet.
//
// The mark between spec and state is a sentinel placed by the startup proc
// so that a misbehaving /init that leaves the wrong number of values on
// the op stack can be detected cleanly (rather than crashing later when
// the frame's spec slot turns out to hold a non-dict).
static void at_gen_server_init_op(Trix *trx) {
    trx->require_op_count(3);
    auto state_obj = *trx->m_op_ptr;
    auto mark_check = *(trx->m_op_ptr - 1);
    auto spec_obj = *(trx->m_op_ptr - 2);
    // Both slots must match the startup wrapper's pattern.  If /init pushed an
    // extra mark above its state value, mark_check alone passes spuriously and
    // spec_obj ends up holding the wrapper mark -- verifying is_dict catches
    // that case and any other stack-shape drift.
    if (!mark_check.is_mark() || !spec_obj.is_dict()) {
        trx->error(Error::TypeCheck, "gen-server: /init proc must leave exactly one state value on op stack");
    } else {
        trx->m_op_ptr -= 3;

        // Build the persistent exec frame: [spec, state, null-ref, null-from]
        trx->require_exec_capacity(4);
        *++trx->m_exec_ptr = spec_obj;
        *++trx->m_exec_ptr = state_obj;
        *++trx->m_exec_ptr = Object::make_null();
        *++trx->m_exec_ptr = Object::make_null();

        // Start the receive loop
        gen_start_recv(trx);
    }
}

// @gen-server-recv: after actor-recv, message on op stack.
// Frame: [spec, state, null-ref, null-from]
static void at_gen_server_recv_op(Trix *trx) {
    auto from_obj = trx->m_exec_ptr;
    auto ref_obj = (from_obj - 1);
    auto state_obj = (ref_obj - 1);
    auto spec_obj = (state_obj - 1);

    trx->require_op_count(1);
    auto msg_obj = *trx->m_op_ptr;
    --trx->m_op_ptr;

    // Categorize message
    auto is_protocol_msg = false;
    Object msg_tag = Object::make_null();

    if (msg_obj.is_array()) {
        auto msg_len = msg_obj.arrays_length();
        if (msg_len >= 2) {
            auto msg_elements = msg_obj.array_objects(trx);
            msg_tag = msg_elements[0];
            is_protocol_msg = msg_tag.is_name();
        }
    }

    if (is_protocol_msg && (msg_tag.name_offset() == trx->wellknown_offset(WellKnownName::GenCall))) {
        // Call: [/gen-call, ref, from, user-msg] -- must have exactly 4 elements
        if (msg_obj.arrays_length() < 4) {
            trx->error(Error::RangeCheck, "gen-server: malformed call message (expected 4 elements)");
        } else {
            auto msg_elements = msg_obj.array_objects(trx);
            auto call_ref = msg_elements[1];
            auto call_from = msg_elements[2];
            auto user_msg = msg_elements[3];

            // Store ref and from in frame for @gen-server-call-done
            *ref_obj = call_ref.make_clone(trx);
            *from_obj = call_from.make_clone(trx);

            // Look up handle-call
            auto handler = gen_spec_get(trx, spec_obj, trx->wellknown_name(WellKnownName::HandleCall));
            if (handler == nullptr) {
                trx->error(Error::Undefined, "gen-server: spec-dict missing /handle-call");
            } else {
                // Build opaque from-token: [ref, from-coroutine]
                // This token is passed to handle-call as the `from` argument.
                // gen-reply uses it to send a deferred reply to the caller.
                auto [token_elems, token_offset] = trx->vm_alloc_n<Object>(2);
                token_elems[0] = call_ref.make_clone(trx);
                token_elems[1] = call_from.make_clone(trx);
                auto from_token = Object::make_array(token_offset, 2);

                // Push args: message from-token state (handle-call signature)
                trx->require_op_capacity(3);
                *++trx->m_op_ptr = user_msg.make_clone(trx);
                *++trx->m_op_ptr = from_token;
                *++trx->m_op_ptr = state_obj->make_clone(trx);

                // Push handler + after-call
                trx->require_exec_capacity(2);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atGenServerCallDone);
                *++trx->m_exec_ptr = handler->make_clone(trx);

                gen_free_message(trx, msg_obj);
            }
        }

    } else if (is_protocol_msg && (msg_tag.name_offset() == trx->wellknown_offset(WellKnownName::GenCast))) {
        // Cast: [/gen-cast, user-msg]
        auto msg_elements = msg_obj.array_objects(trx);
        auto user_msg = msg_elements[1];

        auto handler = gen_spec_get(trx, spec_obj, trx->wellknown_name(WellKnownName::HandleCast));
        if (handler == nullptr) {
            // No handle-cast: ignore, loop
            gen_free_message(trx, msg_obj);
            gen_start_recv(trx);
        } else {
            // Push args: message state
            trx->require_op_capacity(2);
            *++trx->m_op_ptr = user_msg.make_clone(trx);
            *++trx->m_op_ptr = state_obj->make_clone(trx);

            trx->require_exec_capacity(2);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atGenServerCastDone);
            *++trx->m_exec_ptr = handler->make_clone(trx);

            gen_free_message(trx, msg_obj);
        }

    } else if (is_protocol_msg && (msg_tag.name_offset() == trx->wellknown_offset(WellKnownName::GenStop))) {
        // Stop: [/gen-stop, reason]
        auto msg_elements = msg_obj.array_objects(trx);
        auto reason = msg_elements[1];

        auto handler = gen_spec_get(trx, spec_obj, trx->wellknown_name(WellKnownName::Terminate));

        // Clone `reason` BEFORE freeing the message: reason aliases the message spine,
        // and gen_free_message frees each element's ExtValue -- cloning after the free
        // would read a freed cell (use-after-free for an ExtValue reason, e.g. a Long).
        // Mirrors the Call branch, which clones its args before gen_free_message.
        auto reason_clone = reason.make_clone(trx);

        gen_free_message(trx, msg_obj);

        if (handler != nullptr) {
            // Push args: reason state, exec terminate then die
            trx->require_op_capacity(2);
            *++trx->m_op_ptr = reason_clone;
            *++trx->m_op_ptr = state_obj->make_clone(trx);

            // Free frame ExtValues before teardown (only state can hold ExtValue)
            state_obj->maybe_free_extvalue(trx);
            trx->m_exec_ptr -= 4;
            trx->require_exec_capacity(2);
            *++trx->m_exec_ptr = Object::make_operator(SystemName::Die);
            *++trx->m_exec_ptr = handler->make_clone(trx);
        } else {
            // No terminate handler: reason is unused on the die path -- free the clone's
            // ExtValue so it does not leak.
            reason_clone.maybe_free_extvalue(trx);
            // Free frame ExtValues before teardown (only state can hold ExtValue)
            state_obj->maybe_free_extvalue(trx);
            trx->m_exec_ptr -= 4;
            die_op(trx);
        }

    } else {
        // Info (any non-protocol message)
        auto handler = gen_spec_get(trx, spec_obj, trx->wellknown_name(WellKnownName::HandleInfo));
        if (handler == nullptr) {
            // No handle-info: ignore, loop
            gen_free_message(trx, msg_obj);
            gen_start_recv(trx);
        } else {
            trx->require_op_capacity(2);
            *++trx->m_op_ptr = msg_obj.make_clone(trx);
            *++trx->m_op_ptr = state_obj->make_clone(trx);

            trx->require_exec_capacity(2);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atGenServerCastDone);
            *++trx->m_exec_ptr = handler->make_clone(trx);

            gen_free_message(trx, msg_obj);
        }
    }
}

// @gen-server-call-done: after handle-call, op stack has state' reply /reply or state' /noreply.
// Frame: [spec, old-state, ref, from]
static void at_gen_server_call_done_op(Trix *trx) {
    auto from_obj = trx->m_exec_ptr;
    auto ref_obj = (from_obj - 1);
    auto state_obj = (ref_obj - 1);

    trx->require_op_count(2);
    auto sentinel_obj = *trx->m_op_ptr;
    --trx->m_op_ptr;

    if (sentinel_obj.is_name() && (sentinel_obj.name_offset() == trx->wellknown_offset(WellKnownName::Reply))) {
        // /reply: pop reply, send to caller
        trx->require_op_count(2);
        auto reply_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;
        auto new_state_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;

        // Build reply message: [ref, reply]
        auto [reply_elements, reply_offset] = trx->vm_alloc_n<Object>(2);
        reply_elements[0] = ref_obj->make_clone(trx);
        reply_elements[1] = reply_obj;
        auto reply_msg = Object::make_array(reply_offset, 2);

        auto from_coroutine = *from_obj;

        // Update state, clear ref/from BEFORE send (trampoline safety:
        // actor-send may block if mailbox full, so all frame mutations
        // and exec-stack continuations must be set up first).
        state_obj->maybe_free_extvalue(trx);
        *state_obj = new_state_obj;
        *ref_obj = Object::make_null();
        *from_obj = Object::make_null();

        // Push recv continuation (executes after send completes)
        gen_start_recv(trx);

        // Push actor-send on exec stack (executes first)
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = Object::make_operator(SystemName::ActorSend);

        // Set up op stack for actor-send: reply_msg, target_coroutine
        trx->require_op_capacity(2);
        *++trx->m_op_ptr = reply_msg;
        *++trx->m_op_ptr = from_coroutine;
    } else if (sentinel_obj.is_name() && (sentinel_obj.name_offset() == trx->wellknown_offset(WellKnownName::NoReply))) {
        // /noreply: update state, clear frame ref/from (deferred reply
        // uses the opaque from-token stored by the handler, not the frame).
        auto new_state_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;

        state_obj->maybe_free_extvalue(trx);
        *state_obj = new_state_obj;
        *ref_obj = Object::make_null();
        *from_obj = Object::make_null();
        gen_start_recv(trx);
    } else {
        sentinel_obj.maybe_free_extvalue(trx);
        trx->error(Error::TypeCheck, "gen-server: handle-call must return /reply or /noreply sentinel");
    }
}

// @gen-server-cast-done: after handle-cast or handle-info, op stack has state'.
// Frame: [spec, old-state, ref, from]
static void at_gen_server_cast_done_op(Trix *trx) {
    auto from_obj = trx->m_exec_ptr;
    auto ref_obj = (from_obj - 1);
    auto state_obj = (ref_obj - 1);

    trx->require_op_count(1);
    auto new_state_obj = *trx->m_op_ptr;
    --trx->m_op_ptr;

    state_obj->maybe_free_extvalue(trx);
    *state_obj = new_state_obj;

    gen_start_recv(trx);
}

// gen-call: server message -- reply
// Synchronous call to a GenServer.  Blocks until reply received.
// Caller must be an actor (have a mailbox).
// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, vm-full
static void gen_call_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyCoroutine);

    auto msg_obj = *trx->m_op_ptr;
    auto server_obj = *(trx->m_op_ptr - 1);

    // Generate unique ref
    auto ref = static_cast<integer_t>(++trx->m_gen_ref_counter);
    auto ref_obj = Object::make_integer(ref);

    // Get self (caller's coroutine handle)
    auto self_offset = trx->m_running_coroutine;
    auto self_obj = Object::make_coroutine(self_offset);

    // Under global allocation (${...}) every composite below lands in the GC
    // heap and each allocation can fire vm_global_gc; the C locals are not GC
    // roots.  Build the spines region-aware (vm_alloc_dispatch_n) and root each
    // in-flight composite on the gc-root stack so a later allocation cannot sweep
    // it; reset once the last spine alloc (get_proc) is done, after which the
    // C-local Objects stay valid through the alloc-free op/exec setup below.
    // Pushes are unconditional (the gc-root stack is cheap, and in local mode no
    // global GC fires so the roots are inert), matching the ops_record idiom.
    trx->require_gc_root_capacity(5);

    // Build call message: [/gen-call, ref, self, user-msg].  The user-msg slot is
    // a clone of msg_obj, and make_clone of an ExtValue/WideValue message
    // (Double/Long/ULong/Address) allocates -> can fire vm_global_gc.  Root the
    // spine FIRST (with the message slot null-filled so a GC walk of the rooted,
    // not-yet-complete array is safe), THEN clone into it -- otherwise that clone's
    // GC sweeps the unrooted spine and the call message is corrupted.
    auto [call_elements, call_offset] = trx->vm_alloc_dispatch_n<Object>(4, Trix::ChunkKind::Array);
    call_elements[0] = trx->wellknown_name(WellKnownName::GenCall);
    call_elements[1] = ref_obj;
    call_elements[2] = self_obj;
    call_elements[3] = Object::make_null();
    auto call_msg = Object::make_array(call_offset, 4);
    *++trx->m_gc_roots_ptr = call_msg;
    call_elements[3] = msg_obj.make_clone(trx);

    // Build ref-matching predicate for actor-recv-match.
    // Predicate: curry [ref, { exch dup is-array { 0 get eq } { pop pop false } if-else }]
    // When called with msg on stack, curry pushes ref -> body runs with msg ref:
    //   exch dup is-array -> test if msg is array
    //   true:  0 get eq   -> compare msg[0] with ref
    //   false: pop pop false -> discard both, return false

    // True branch: { 0 get eq }
    auto [true_elems, true_offset] = trx->vm_alloc_dispatch_n<Object>(3, Trix::ChunkKind::Array);
    true_elems[0] = Object::make_integer(0);
    true_elems[1] = Object::make_operator(SystemName::Get);
    true_elems[2] = Object::make_operator(SystemName::Eq);
    auto true_proc = Object::make_array(true_offset, 3, Object::ExecutableAttrib, Object::ReadOnlyAccess);
    *++trx->m_gc_roots_ptr = true_proc;

    // False branch: { pop pop false }
    auto [false_elems, false_offset] = trx->vm_alloc_dispatch_n<Object>(3, Trix::ChunkKind::Array);
    false_elems[0] = Object::make_operator(SystemName::Pop);
    false_elems[1] = Object::make_operator(SystemName::Pop);
    false_elems[2] = Object::make_boolean(false);
    auto false_proc = Object::make_array(false_offset, 3, Object::ExecutableAttrib, Object::ReadOnlyAccess);
    *++trx->m_gc_roots_ptr = false_proc;

    // Body: { exch dup is-array true_proc false_proc if-else }
    auto [body_elems, body_offset] = trx->vm_alloc_dispatch_n<Object>(6, Trix::ChunkKind::Array);
    body_elems[0] = Object::make_operator(SystemName::Exch);
    body_elems[1] = Object::make_operator(SystemName::Dup);
    body_elems[2] = Object::make_operator(SystemName::IsArray);
    body_elems[3] = true_proc;
    body_elems[4] = false_proc;
    body_elems[5] = Object::make_operator(SystemName::IfElse);
    auto body = Object::make_array(body_offset, 6, Object::ExecutableAttrib, Object::ReadOnlyAccess);
    *++trx->m_gc_roots_ptr = body;

    // Curry [ref, body] -- executable predicate for actor-recv-match
    auto pred = Object::make_curry_pair(trx, ref_obj, body);
    *++trx->m_gc_roots_ptr = pred;

    // Build: { 1 get } -- extract reply from [ref, reply].  Last allocation; no
    // further alloc before it is exec-pushed (a root), so it needs no temp root.
    auto [get_elements, get_offset] = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Array);
    get_elements[0] = Object::make_integer(1);
    get_elements[1] = Object::make_operator(SystemName::Get);
    auto get_proc = Object::make_array(get_offset, 2, Object::ExecutableAttrib, Object::ReadOnlyAccess);

    trx->reset_gc_root(5);  // spine allocs done; the C-local temps stay valid through the alloc-free setup below

    // Set up op stack: call_msg, server (for actor-send)
    trx->m_op_ptr->maybe_free_extvalue(trx);
    trx->m_op_ptr -= 2;
    trx->require_op_capacity(2);
    *++trx->m_op_ptr = call_msg;
    *++trx->m_op_ptr = server_obj;

    // Push continuation on exec stack (LIFO: last pushed = first executed).
    // Execution order: actor-send -> pred(push to op) -> actor-recv-match -> { 1 get }
    auto pred_literal = pred;
    pred_literal.set_literal();

    trx->require_exec_capacity(4);
    *++trx->m_exec_ptr = get_proc;
    *++trx->m_exec_ptr = Object::make_operator(SystemName::ActorRecvMatch);
    *++trx->m_exec_ptr = pred_literal;
    *++trx->m_exec_ptr = Object::make_operator(SystemName::ActorSend);
}

// gen-call-timeout: server message ms -- reply
// Like gen-call but with timeout.  Raises /limit-check on timeout.
// Caller must be an actor (have a mailbox).
// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void gen_call_timeout_op(Trix *trx) {
    trx->verify_operands(VerifyInteger | VerifyNotNegative, VerifyAny, VerifyCoroutine);

    auto timeout_ms = trx->m_op_ptr->integer_value();
    auto msg_obj = *(trx->m_op_ptr - 1);
    auto server_obj = *(trx->m_op_ptr - 2);

    // Generate unique ref
    auto ref = static_cast<integer_t>(++trx->m_gen_ref_counter);
    auto ref_obj = Object::make_integer(ref);

    // Get self (caller's coroutine handle)
    auto self_obj = Object::make_coroutine(trx->m_running_coroutine);

    // Region-aware spines + gc-root-stack rooting under ${...}, exactly as
    // gen_call_op: each global composite is rooted on the gc-root stack across the
    // next allocation (make_curry_pair is the last allocating call, so pred needs
    // no root); reset once it is built.  Pushes unconditional (ops_record idiom).
    trx->require_gc_root_capacity(4);

    // Build call message: [/gen-call, ref, self, user-msg].  Root the spine BEFORE
    // cloning the (possibly ExtValue/WideValue) message into it -- see gen_call_op
    // for why the message slot is null-filled and the clone runs after the root.
    auto [call_elements, call_offset] = trx->vm_alloc_dispatch_n<Object>(4, Trix::ChunkKind::Array);
    call_elements[0] = trx->wellknown_name(WellKnownName::GenCall);
    call_elements[1] = ref_obj;
    call_elements[2] = self_obj;
    call_elements[3] = Object::make_null();
    auto call_msg = Object::make_array(call_offset, 4);
    *++trx->m_gc_roots_ptr = call_msg;
    call_elements[3] = msg_obj.make_clone(trx);

    // Build ref-matching predicate (same as gen-call)
    auto [true_elems, true_offset] = trx->vm_alloc_dispatch_n<Object>(3, Trix::ChunkKind::Array);
    true_elems[0] = Object::make_integer(0);
    true_elems[1] = Object::make_operator(SystemName::Get);
    true_elems[2] = Object::make_operator(SystemName::Eq);
    auto true_proc = Object::make_array(true_offset, 3, Object::ExecutableAttrib, Object::ReadOnlyAccess);
    *++trx->m_gc_roots_ptr = true_proc;

    auto [false_elems, false_offset] = trx->vm_alloc_dispatch_n<Object>(3, Trix::ChunkKind::Array);
    false_elems[0] = Object::make_operator(SystemName::Pop);
    false_elems[1] = Object::make_operator(SystemName::Pop);
    false_elems[2] = Object::make_boolean(false);
    auto false_proc = Object::make_array(false_offset, 3, Object::ExecutableAttrib, Object::ReadOnlyAccess);
    *++trx->m_gc_roots_ptr = false_proc;

    auto [body_elems, body_offset] = trx->vm_alloc_dispatch_n<Object>(6, Trix::ChunkKind::Array);
    body_elems[0] = Object::make_operator(SystemName::Exch);
    body_elems[1] = Object::make_operator(SystemName::Dup);
    body_elems[2] = Object::make_operator(SystemName::IsArray);
    body_elems[3] = true_proc;
    body_elems[4] = false_proc;
    body_elems[5] = Object::make_operator(SystemName::IfElse);
    auto body = Object::make_array(body_offset, 6, Object::ExecutableAttrib, Object::ReadOnlyAccess);
    *++trx->m_gc_roots_ptr = body;

    auto pred = Object::make_curry_pair(trx, ref_obj, body);

    trx->reset_gc_root(4);  // last spine alloc (make_curry_pair) done; pred + temps stay valid in C-locals

    // Set up op stack: call_msg, server (for actor-send)
    // Free the original message (cloned into call_msg).  Stack is [server message ms]
    // with ms on top, so the message is the slot BELOW the top, not the top (ms is
    // an integer and owns no ExtValue).
    (trx->m_op_ptr - 1)->maybe_free_extvalue(trx);
    trx->m_op_ptr -= 3;
    trx->require_op_capacity(2);
    *++trx->m_op_ptr = call_msg;
    *++trx->m_op_ptr = server_obj;

    // Push continuation on exec stack (LIFO: last pushed = first executed).
    // Execution order: actor-send -> pred(push) -> ms(push) -> actor-recv-match-timeout -> @done
    auto pred_literal = pred;
    pred_literal.set_literal();
    auto ms_literal = Object::make_integer(timeout_ms);
    ms_literal.set_literal();

    trx->require_exec_capacity(5);
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atGenCallTimeoutDone);
    *++trx->m_exec_ptr = Object::make_operator(SystemName::ActorRecvMatchTimeout);
    *++trx->m_exec_ptr = ms_literal;
    *++trx->m_exec_ptr = pred_literal;
    *++trx->m_exec_ptr = Object::make_operator(SystemName::ActorSend);
}

// @gen-call-timeout-done: process result from actor-recv-match-timeout.
// Op stack on entry: [msg bool] (match) or [null false] (timeout)
static void at_gen_call_timeout_done_op(Trix *trx) {
    trx->require_op_count(2);
    auto flag_obj = *trx->m_op_ptr--;

    if (flag_obj.is_boolean() && flag_obj.boolean_value()) {
        // Match found: msg is [ref, reply-value], extract reply.  The
        // ref-matching predicate accepts any array whose element 0 equals the
        // pending ref, including a 1-element [ref], so bound the read here.
        auto msg_obj = *trx->m_op_ptr;
        if (!msg_obj.is_array() || (msg_obj.arrays_length() < 2)) {
            trx->error(Error::RangeCheck, "gen-call-timeout: malformed reply message");
        } else {
            auto elem_data = msg_obj.array_objects(trx);
            auto reply_obj = elem_data[1].make_clone(trx);
            *trx->m_op_ptr = reply_obj;
        }
    } else {
        // Timeout: pop null, raise error
        trx->m_op_ptr->maybe_free_extvalue(trx);
        --trx->m_op_ptr;
        trx->error(Error::LimitCheck, "gen-call-timeout: server did not reply within timeout");
    }
}

// gen-cast: server message --
// Asynchronous cast to a GenServer.  Fire and forget.
// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, vm-full
static void gen_cast_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyCoroutine);

    auto msg_obj = *trx->m_op_ptr;
    auto server_obj = *(trx->m_op_ptr - 1);

    // Build cast message: [/gen-cast, user-msg], region-aware so under ${...} it
    // lands in global VM (a global message into the global mailbox) like gen-call,
    // instead of a LOCAL spine into a GLOBAL mailbox.  Root the spine BEFORE cloning
    // the (possibly ExtValue/WideValue) message into it -- see gen_call_op.
    trx->require_gc_root_capacity(1);
    auto [cast_elements, cast_offset] = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Array);
    cast_elements[0] = trx->wellknown_name(WellKnownName::GenCast);
    cast_elements[1] = Object::make_null();
    auto cast_msg = Object::make_array(cast_offset, 2);
    *++trx->m_gc_roots_ptr = cast_msg;
    cast_elements[1] = msg_obj.make_clone(trx);
    trx->reset_gc_root(1);

    // Set up op stack for actor-send and push on exec stack
    // (trampoline-safe: actor-send may block if mailbox full)
    trx->m_op_ptr->maybe_free_extvalue(trx);  // free the original message (cloned into cast_msg)
    trx->m_op_ptr -= 2;
    trx->require_op_capacity(2);
    *++trx->m_op_ptr = cast_msg;
    *++trx->m_op_ptr = server_obj;
    trx->require_exec_capacity(1);
    *++trx->m_exec_ptr = Object::make_operator(SystemName::ActorSend);
}

// gen-stop: server reason --
// Send a graceful stop request to a GenServer.
// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, vm-full
static void gen_stop_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyCoroutine);

    auto reason_obj = *trx->m_op_ptr;
    auto server_obj = *(trx->m_op_ptr - 1);

    // Build stop message: [/gen-stop, reason], region-aware (see gen_cast_op).
    // Root the spine BEFORE cloning the (possibly ExtValue/WideValue) reason in.
    trx->require_gc_root_capacity(1);
    auto [stop_elements, stop_offset] = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Array);
    stop_elements[0] = trx->wellknown_name(WellKnownName::GenStop);
    stop_elements[1] = Object::make_null();
    auto stop_msg = Object::make_array(stop_offset, 2);
    *++trx->m_gc_roots_ptr = stop_msg;
    stop_elements[1] = reason_obj.make_clone(trx);
    trx->reset_gc_root(1);

    // Set up op stack for actor-send and push on exec stack
    // (trampoline-safe: actor-send may block if mailbox full)
    trx->m_op_ptr->maybe_free_extvalue(trx);  // free the original reason (cloned into stop_msg)
    trx->m_op_ptr -= 2;
    trx->require_op_capacity(2);
    *++trx->m_op_ptr = stop_msg;
    *++trx->m_op_ptr = server_obj;
    trx->require_exec_capacity(1);
    *++trx->m_exec_ptr = Object::make_operator(SystemName::ActorSend);
}

// gen-reply: from-token reply --
// Explicit reply for deferred call responses (when handle-call returns /noreply).
// from-token is the opaque [ref, from-coroutine] array passed to handle-call.
// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, vm-full
static void gen_reply_op(Trix *trx) {
    trx->require_op_count(2);

    auto reply_obj = *trx->m_op_ptr;
    auto from_token = *(trx->m_op_ptr - 1);

    // Validate from-token: must be 2-element array [ref, from-coroutine]
    if (!from_token.is_array() || (from_token.arrays_length() != 2)) {
        trx->error(Error::TypeCheck, "gen-reply: from must be a [ref, coroutine] token from handle-call");
    } else {
        auto token_elems = from_token.array_objects(trx);
        if (!token_elems[0].is_integer()) {
            trx->error(Error::TypeCheck, "gen-reply: from-token ref must be integer");
        } else if (!token_elems[1].is_coroutine()) {
            trx->error(Error::TypeCheck, "gen-reply: from-token target must be coroutine");
        } else {
            auto ref = token_elems[0];
            auto from_coroutine = token_elems[1];

            // Build reply message: [ref, reply], region-aware (see gen_cast_op).  Root the
            // spine BEFORE the clones: reply can be an ExtValue/WideValue whose make_clone
            // allocates and would otherwise sweep the unrooted spine.  ref is an integer
            // token element (immediate clone), but the slots are null-filled regardless so
            // the rooted, not-yet-complete array is GC-walk-safe.
            trx->require_gc_root_capacity(1);
            auto [reply_elems, reply_offset] = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Array);
            reply_elems[0] = Object::make_null();
            reply_elems[1] = Object::make_null();
            auto reply_msg = Object::make_array(reply_offset, 2);
            *++trx->m_gc_roots_ptr = reply_msg;
            reply_elems[0] = ref.make_clone(trx);
            reply_elems[1] = reply_obj.make_clone(trx);
            trx->reset_gc_root(1);

            // Set up actor-send: reply_msg from-coroutine
            trx->m_op_ptr -= 2;
            trx->require_op_capacity(2);
            *++trx->m_op_ptr = reply_msg;
            *++trx->m_op_ptr = from_coroutine;

            // Trampoline-safe: push actor-send on exec stack
            trx->require_exec_capacity(1);
            *++trx->m_exec_ptr = Object::make_operator(SystemName::ActorSend);
        }
    }
}

// There is deliberately no gen-state operator.  Cross-coroutine state
// introspection is the wrong abstraction at the runtime layer -- state
// queries belong in the handler: add a /query arm to /handle-call that
// returns the state unchanged (see docs/genserver.md).
