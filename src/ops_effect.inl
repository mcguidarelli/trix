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
//     http://www.apache.org/licenses/LICENSE-2.0                             //
//                                                                            //
// Unless required by applicable law or agreed to in writing, software        //
// distributed under the License is distributed on an "AS IS" BASIS,          //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   //
// See the License for the specific language governing permissions and        //
// limitations under the License.                                             //
//                                                                            //
//===----------------------------------------------------------------------===//

//===--- Algebraic Effect Operators ---===//
//
// Implements algebraic effects via two user-facing operators:
//
//   handle-effect  -- handler-dict body handle-effect -> result
//                     Establishes an effect handler scope.  handler-dict maps
//                     effect names to handler procs.  Executes body.  If the
//                     body completes normally, its result is the result of
//                     handle-effect.  If the body calls perform, the matching
//                     handler is invoked with the captured continuation K.
//
//   perform        -- ...args effect-name perform -> result
//                     Inside a handle-effect body: looks up effect-name in
//                     the nearest matching handler-dict, captures a
//                     continuation K from the current position to that
//                     handler's @effect-barrier, pushes K onto the operand
//                     stack, and executes the handler proc.  The handler
//                     sees K and any args on the stack.  If no handler is
//                     found, raises /effect-not-handled.
//
// Handler procs receive the continuation K on the operand stack.  They can:
//   - Resume K with a value: push value, exec K (one-shot)
//   - Abort: ignore K, return a value directly
//   - Store K for later invocation
//
// Nested handle-effect scopes delegate automatically: if the innermost
// handler-dict does not contain the effect-name, perform continues scanning
// outward to the next @effect-barrier.
//
// The continuation captured by perform is a Continuation Object identical to
// those created by capture -- one-shot, callable, resumable with exec.
//
// Deep handler semantics: perform captures the @effect-barrier and its
// handler-dict companion as part of the continuation.  When K is resumed,
// the barrier and companion are restored, reinstating the handler scope.
// This means subsequent performs in the resumed body find the same handler
// automatically (the standard algebraic effects "deep handler" pattern).

// @effect-barrier: internal barrier on exec stack.
// Normal completion: body finished without performing an effect handled by
// this barrier.  Pop the handler-dict companion from the error stack.
// throws: (none)
static void at_effect_barrier_op(Trix *trx) {
    // Pop handler-dict companion from error stack
    --trx->m_err_ptr;
}

// handle-effect: handler-dict body -- result
// Establish an effect handler scope, then execute body.
// throws: opstack-underflow, type-check, execstack-overflow, errstack-overflow
static void handle_effect_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyDict);

    auto body = *trx->m_op_ptr--;
    auto handler_dict = *trx->m_op_ptr--;

    trx->require_exec_capacity(2);
    trx->require_error_capacity(1);

    // Push @effect-barrier on exec stack
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atEffectBarrier);

    // Execute body above the barrier
    body.set_executable();
    *++trx->m_exec_ptr = body;

    // Push handler-dict as companion on error stack
    *++trx->m_err_ptr = handler_dict;
}

// perform: ...args effect-name -- result
// Look up effect-name in the handler chain, capture a continuation to the
// matching handler's @effect-barrier, and invoke the handler proc.
// throws: opstack-underflow, type-check, effect-not-handled, invalid-access
static void perform_op(Trix *trx) {
    trx->verify_operands(VerifyName);

    auto effect_name = *trx->m_op_ptr--;

    // Scan exec stack for an @effect-barrier whose handler-dict contains effect-name.
    // Track error stack position to correlate barriers with their companions.
    auto err_scan = trx->m_err_ptr;
    auto barrier = static_cast<Object *>(nullptr);
    auto handler_proc = Object{};

    // Walk 1: find the matching @effect-barrier.  err_scan tracks where
    // the matching err-stack companion sits as we walk past barriers
    // that own err entries.  The descriptor table tells us exactly how
    // many err entries each control op consumes -- no manual chain.
    for (auto scan = trx->m_exec_ptr; scan >= trx->m_exec_base; --scan) {
        if (scan->is_operator() && is_control_op_atom(scan->m_operator)) {
            if (scan->operator_is_effectbarrier()) {
                // This barrier's handler-dict companion is at *err_scan.
                auto handler_dict = err_scan->dict_value(trx);
                auto result = handler_dict->get(trx, effect_name);
                if (result != nullptr) {
                    handler_proc = *result;
                    barrier = scan;
                    break;
                } else {
                    --err_scan;
                }
            } else {
                err_scan -= op_descriptor_for(scan->m_operator).err_companions;
            }
        }
    }

    if (barrier == nullptr) {
        trx->error(Error::EffectNotHandled, "perform: no handler for effect /{}", effect_name.name_sv(trx));
    } else {
        // --- Capture continuation from current position to the matched barrier ---
        // Deep handler semantics: the capture INCLUDES the @effect-barrier and its
        // handler-dict companion.  When K is resumed via execute_continuation, the
        // barrier and companion are restored, reinstating the handler scope so that
        // subsequent performs in the resumed body find the same handler.
        //
        // If the captured segment crosses any |locals| boundaries (marked by
        // @end-locals on the exec stack above the effect barrier), the matching
        // frame dicts are popped from the dict stack and saved inside the
        // ContinuationContext; execute_continuation restores them on resume so the
        // handler-selected body sees the same locals scope it was captured in.

        // Count exec Objects to capture: everything from barrier to m_exec_ptr (inclusive)
        auto exec_count = static_cast<length_t>(trx->m_exec_ptr - barrier + 1);

        // Walk 2: count companion entries in the captured range (inclusive of
        // the matched @effect-barrier).  Each control-op atom contributes its
        // descriptor's err/dict/rewrite fields uniformly -- the descriptor
        // table is the single source of truth.
        length_t err_count = 0;
        length_t dict_count = 0;
        length_t op_rewrite_count = 0;
        length_t exec_rewrite_count = 0;
        for (auto scan = trx->m_exec_ptr; scan >= barrier; --scan) {
            if (scan->is_operator() && is_control_op_atom(scan->m_operator)) {
                const auto &desc = op_descriptor_for(scan->m_operator);
                err_count = static_cast<length_t>(err_count + desc.err_companions);
                dict_count = static_cast<length_t>(dict_count + desc.dict_companions);
                if (desc.needs_op_rewrite) {
                    ++op_rewrite_count;
                }
                if (desc.needs_exec_rewrite) {
                    ++exec_rewrite_count;
                }
            }
        }

        // Allocate ContinuationContext on VM heap.  perform does NOT capture
        // an operand-stack slice (op_count=0); deep-handler semantics mean
        // the resumed body runs wherever the handler's resume call was made,
        // and reconstructing per-barrier op-depths across restore would
        // require per-captured-barrier rewriting.  Separate language follow-up.
        auto alloc = ContinuationContext::alloc_size(exec_count, err_count, dict_count, 0, op_rewrite_count, exec_rewrite_count);
        auto [ctx, ctx_offset] = trx->vm_alloc<ContinuationContext>(alloc);

        ctx->m_exec_count = exec_count;
        ctx->m_err_count = err_count;
        ctx->m_dict_count = dict_count;
        ctx->m_op_count = 0;
        ctx->m_op_rewrite_count = op_rewrite_count;
        ctx->m_exec_rewrite_count = exec_rewrite_count;
        // Capture the live allocation-routing flag so resume restores it (mirror
        // capture_op); vm_alloc does not zero, so an uninitialized field would
        // feed garbage into m_curr_alloc_global on resume.
        ctx->m_alloc_global_at_capture = trx->m_curr_alloc_global;

        // Copy exec segment bottom-first: barrier is the bottom, m_exec_ptr is the top
        std::memcpy(&ctx->m_objects[0], barrier, exec_count * sizeof(Object));

        // Copy error stack entries bottom-first (includes the barrier's own companion)
        auto err_base = trx->m_err_ptr - err_count + 1;
        std::memcpy(&ctx->m_objects[exec_count], err_base, err_count * sizeof(Object));
        trx->m_err_ptr -= err_count;

        // Copy and pop top dict_count frame dicts from dict stack (outermost first).
        // Each popped dict has its cached name bindings cleared from the running
        // coroutine's binding table; execute_continuation repopulates on resume.
        auto dict_base = trx->m_dict_ptr - dict_count + 1;
        std::memcpy(&ctx->m_objects[exec_count + err_count], dict_base, dict_count * sizeof(Object));
        for (length_t i = 0; i < dict_count; ++i) {
            auto dict = dict_base[i].dict_value(trx);
            dict->clear_name_bindings(trx);
        }
        trx->m_dict_ptr -= dict_count;

        // Emit op-depth rewrite-offset tables for captured op-depth-bearing
        // inner barriers + @try-rollback.  See capture_op for full discussion;
        // perform's captured exec range INCLUDES the matched @effect-barrier
        // (whose err companion is the handler-dict, no rewrite needed), so
        // the bottom-up walk starts AT `barrier` rather than `barrier + 1`.
        // Walk 3: emit rewrite-offset tables for captured op-depth-bearing
        // ops.  For err-companion rewrites (delimit-barrier, handler-scope),
        // the rewrite target sits at running_err_offset + (err_companions-1)
        // -- the LAST err entry the op owns is the op-depth slot.  For
        // exec-companion rewrites (try-rollback / try-result / forall-family
        // with exec_companions=1, and the logic-family with exec_companions=2),
        // the saved/op depth Integer sits at scan - exec_companions.
        if ((op_rewrite_count > 0) || (exec_rewrite_count > 0)) {
            // op_count is 0 for perform, so the rewrite arrays start directly
            // after the dict segment.
            auto err_rewrite_array = &ctx->m_objects[exec_count + err_count + dict_count];
            auto exec_rewrite_array = &ctx->m_objects[exec_count + err_count + dict_count + op_rewrite_count];
            length_t err_rw_idx = 0;
            length_t exec_rw_idx = 0;
            length_t running_err_offset = 0;
            for (auto scan = barrier; scan <= trx->m_exec_ptr; ++scan) {
                if (scan->is_operator() && is_control_op_atom(scan->m_operator)) {
                    const auto &desc = op_descriptor_for(scan->m_operator);
                    if (desc.needs_op_rewrite) {
                        // op-depth Integer sits at the last err slot this op owns.
                        auto target = static_cast<length_t>(running_err_offset + desc.err_companions - 1);
                        err_rewrite_array[err_rw_idx++] = Object::make_integer(static_cast<integer_t>(target));
                    }
                    if (desc.needs_exec_rewrite) {
                        // saved/op depth Integer at scan - exec_companions.
                        assert(scan - desc.exec_companions >= barrier);
                        assert((scan - desc.exec_companions)->is_integer());
                        auto captured_exec_offset = static_cast<length_t>((scan - desc.exec_companions) - barrier);
                        exec_rewrite_array[exec_rw_idx++] = Object::make_integer(static_cast<integer_t>(captured_exec_offset));
                    }
                    running_err_offset = static_cast<length_t>(running_err_offset + desc.err_companions);
                }
            }
            assert(err_rw_idx == op_rewrite_count);
            assert(exec_rw_idx == exec_rewrite_count);
            assert(running_err_offset == err_count);
        }

        // Remove the barrier and captured segment from exec stack
        trx->m_exec_ptr = barrier - 1;

        // Create Continuation Object and push onto operand stack.  The capture
        // save level is stashed in the Object itself (not only in the ctx) so
        // execute_continuation can reject post-restore resume without first
        // dereferencing the possibly-freed ctx.
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_continuation(ctx_offset, trx->m_curr_save_level);

        // Execute handler proc (handler sees K on the operand stack)
        handler_proc.set_executable();
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = handler_proc;
    }
}
