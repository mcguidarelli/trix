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

//===--- Delimited Continuation Operators ---===//
//
// Implements one-shot delimited continuations via three user-facing operators:
//
//   delimit   -- body delimit -> result
//                Establishes a delimiter boundary on the exec stack, then
//                executes body.  Body's result is the result of delimit.
//
//   capture   -- handler-proc capture -> (yields to handler)
//                Inside a delimit body: captures the exec stack segment
//                between the current position and the nearest delimiter as
//                a Continuation K, removes the delimiter, pushes K onto
//                the operand stack, then executes handler-proc.
//
//   abort-exec  -- resume-value K :- (does not return)
//                Abort variant of exec on a Continuation: trims the
//                resumer's exec/err/dict/op stacks down to K's matching
//                @handler-scope before splicing K's captured segment, so
//                receiver frames between the handler-push point and the
//                abort-exec call are discarded (Scheme-style call/cc).
//
// A Continuation is a first-class callable Object.  When executed (via exec
// or interpreter dispatch), it restores the captured exec stack segment,
// pushes a fresh delimiter for nested captures, and uses the top of the
// operand stack as the resume value (the "return value of capture" in the
// body).  One-shot: after resume mark_spent sets m_err_count to the Spent
// sentinel (= length_t::max()), which is_spent() detects, so re-invoking the
// same ctx (even through a dup'd Continuation sharing the offset) raises
// /invalid-access.

//===--- ContinuationContext ---===//
//
// Variable-length struct stored on the VM heap.  Header holds six length_t
// counts (exec/err/dict/op-slice plus op-rewrite and exec-rewrite counts)
// and a bool m_alloc_global_at_capture, followed by the captured Objects.
//
// Layout of m_objects[]:
//   [0 .. E-1]                       captured exec stack Objects
//   [E .. E+R-1]                     captured error stack Objects
//   [E+R .. E+R+D-1]                 captured frame dict Objects (outermost first)
//   [E+R+D .. E+R+D+O-1]             captured op stack slice (ambient)
//   [E+R+D+O .. E+R+D+O+OR-1]        err-segment op-depth rewrite offsets
//   [E+R+D+O+OR .. +ER-1]            exec-segment op-depth rewrite offsets
//     where E = m_exec_count, R = m_err_count, D = m_dict_count,
//           O = m_op_count, OR = m_op_rewrite_count,
//           ER = m_exec_rewrite_count.
//   (The two trailing tables hold Integer offsets, not GC-traced Object refs.)
//
// Objects are stored bottom-first (lowest stack address first) so that
// execute_continuation can restore each segment with a single memcpy.
//
// The op-slice captures the ambient operand-stack state between the
// matched @delimit-barrier and the capture point (excluding the popped
// handler proc).  `delimit_op` records the op-stack depth at its entry
// on the error stack as an integer companion to @delimit-barrier;
// capture_op reads that snapshot, saves the slice above it, and trims
// m_op_ptr back to the snapshot so the handler runs with a clean op
// stack.  execute_continuation restores the slice above the resumer's
// m_op_ptr, then puts the resume value on top -- so the captured body
// sees the same op-stack layout it was captured with.
//
// Restore-dangle detection: the save level at capture time is stashed in
// the Continuation Object itself (Object::m_continuation_save_level), NOT
// in this ctx.  That lets execute_continuation reject a post-restore
// resume without first dereferencing the ctx, which may have been freed
// by the restore that invalidated it.

struct ContinuationContext {
    static constexpr length_t Spent{std::numeric_limits<length_t>::max()};

    length_t m_exec_count;           // captured exec stack Objects
    length_t m_err_count;            // captured error stack entries (Spent sentinel = already resumed)
    length_t m_dict_count;           // captured frame dict Objects (one per @end-locals in exec segment)
    length_t m_op_count;             // captured op stack slice (ambient between delimit and capture)
    length_t m_op_rewrite_count;     // captured err-segment offsets pointing at op-depth slots that must
                                     // be rewritten at restore to the resumer's fresh op-floor; populated
                                     // for @delimit-barrier (1 slot) and @handler-scope (slot 2 of 2) so
                                     // a second capture inside the resumed body doesn't read a stale
                                     // capturer-time op-depth and trim the live op stack incorrectly.
    length_t m_exec_rewrite_count;   // captured EXEC-segment offsets pointing at op-depth slots that must
                                     // be rewritten at restore.  Populated for @try-rollback (whose saved-
                                     // depth Integer lives on the exec stack one slot below the barrier;
                                     // see ops_flow.inl:try_op), so an error inside the resumed try body
                                     // rolls m_op_ptr back to the resumer's saved-depth, not the stale
                                     // capturer-time value.
    bool m_alloc_global_at_capture;  // m_curr_alloc_global at capture time; restored on resume so a body captured
                                     // inside ${...} resumes with the right allocation routing even if the
                                     // handler in between mutated the flag (set-global, ran past an outer
                                     // @end-in-global, etc).  Boolean -- packs in 1 byte, struct padding absorbs the rest.
    Object m_objects[1];             // variable-length: exec, err, frame-dicts, op-slice, err-rewrites,
                                     // then exec-rewrites

    [[nodiscard]] bool is_spent() const { return (m_err_count == Spent); }

    void mark_spent() {
        m_exec_count = 0;
        m_err_count = Spent;
        m_dict_count = 0;
        m_op_count = 0;
        m_op_rewrite_count = 0;
        m_exec_rewrite_count = 0;
    }

    [[nodiscard]] static vm_size_t alloc_size(length_t exec_count,
                                              length_t err_count,
                                              length_t dict_count,
                                              length_t op_count,
                                              length_t op_rewrite_count,
                                              length_t exec_rewrite_count) {
        auto total = static_cast<vm_size_t>(exec_count) + err_count + dict_count + op_count + op_rewrite_count + exec_rewrite_count;
        return static_cast<vm_size_t>(offsetof(ContinuationContext, m_objects) + (total * sizeof(Object)));
    }
};

// GC: mark the Object cells captured inside a (LOCAL) ContinuationContext.
//
// capture / perform allocate the ctx via vm_alloc (LOCAL), but a body or
// effect handler-dict captured inside ${...} lands in GLOBAL VM.  Once the
// captured exec / err / dict / op-slice segments are lifted off the live
// stacks -- perform moves the effect handler-dict OFF the err stack INTO the
// ctx -- this descent is the ONLY root that reaches those global refs.
// Without it a stress GC sweeps the handler-dict (or a captured global body)
// while the continuation is still resumable, and the resumed handler reads a
// freed (poisoned) block.  The captured Object segments are the first
// (exec + err + dict + op) cells of m_objects; the trailing op-/exec-rewrite
// tables are Integers and need no marking.  A spent continuation
// (m_err_count == Spent, other counts zeroed by mark_spent) has already
// consumed its segments -- nothing to mark.  Called from gc_mark_object's
// local-VM descent, which owns the visited-set cycle-break before dispatching
// here (so this walker does no dedup of its own).
static void gc_walk_continuation(Trix *trx, vm_offset_t offset) {
    auto *ctx = trx->offset_to_ptr<ContinuationContext>(offset);
    if (!ctx->is_spent()) {
        auto object_count = static_cast<vm_size_t>(ctx->m_exec_count) + ctx->m_err_count + ctx->m_dict_count + ctx->m_op_count;
        for (vm_size_t i = 0; i < object_count; ++i) {
            trx->gc_mark_object(ctx->m_objects[i]);
        }
    }
}

//===--- Operators ---===//

// @delimit-barrier: internal barrier on exec stack.
// Normal completion path: body finished without capture.  Pop the
// op-depth companion from the error stack (placed there by delimit_op
// so capture_op knows where to slice the operand stack).
// throws: (none)
static void at_delimit_barrier_op(Trix *trx) {
    // Pop op-depth companion (recorded at delimit entry for capture_op).
    --trx->m_err_ptr;
}

// @handler-scope: internal barrier marking the trim target for
// abort-exec.  Pushed by capture_op above the captured delimit range
// so abort-exec can identify which handler's frames to discard.
// Normal completion path: handler body finished without calling
// abort-exec -- pop the two err companions (op-depth on top,
// identity below).
// throws: (none)
static void at_handler_scope_op(Trix *trx) {
    // Pop op-depth and identity companions (pushed by capture_op).
    trx->m_err_ptr -= 2;
}

// delimit: body -- result
// Establish a delimiter boundary, then execute body.  Records the
// op-stack depth at entry on the error stack as an integer companion
// to @delimit-barrier; capture_op uses that snapshot to save the
// "ambient" op-slice between delimiter and the capture point.
// throws: opstack-underflow, type-check, execstack-overflow, errstack-overflow
static void delimit_op(Trix *trx) {
    trx->verify_operands(VerifyCallable);
    trx->require_exec_capacity(2);
    trx->require_error_capacity(1);

    auto body = *trx->m_op_ptr--;

    // Record op-depth companion BEFORE pushing the barrier, so it pairs
    // with @delimit-barrier in the barrier/err ordering used by scan
    // sites (try_catch_handler, exit_op, stop_op, perform_op match).
    auto op_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);
    *++trx->m_err_ptr = Object::make_integer(op_depth);

    // Push barrier, then body will execute above it
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDelimitBarrier);

    // Execute body
    body.set_executable();
    *++trx->m_exec_ptr = body;
}

// capture: handler-proc -- (yields to handler)
// Capture the exec stack segment up to the nearest @delimit-barrier as a
// Continuation K, remove the barrier, push K onto the operand stack, and
// execute handler-proc.
//
// If the captured segment crosses one or more |locals| boundaries (each
// marked by an @end-locals on the exec stack), the matching frame dicts
// are popped from the dict stack and saved inside the ContinuationContext.
// The ambient operand-stack slice between the matched @delimit-barrier
// and the capture point is also saved (and trimmed from the live op
// stack) so the handler runs on a clean op stack and the resumed body
// sees its original op layout.
// throws: opstack-underflow, type-check, unhandled-capture
static void capture_op(Trix *trx) {
    trx->verify_operands(VerifyCallable);

    auto handler = *trx->m_op_ptr--;

    // Find the nearest @delimit-barrier on the exec stack
    auto barrier = static_cast<Object *>(nullptr);
    for (auto scan = trx->m_exec_ptr; scan >= trx->m_exec_base; --scan) {
        if (scan->is_operator() && scan->operator_is_delimitbarrier()) {
            barrier = scan;
            break;
        }
    }
    if (barrier == nullptr) {
        trx->error(Error::UnhandledCapture, "capture: no enclosing delimit");
    } else {
        // Count exec Objects to capture: everything above the barrier
        auto exec_count = static_cast<length_t>(trx->m_exec_ptr - barrier);

        // Count inner-barrier err-stack entries, @end-locals markers, and
        // op-depth/exec-depth rewrite targets in the captured segment.  Each
        // control op contributes its descriptor fields uniformly -- the
        // op_descriptor.inl table is the single source of truth.  This also
        // closes the IN-H1 bug class (missed @end-in-global would have been
        // a missing table row, caught by static_assert).
        length_t err_count = 0;
        length_t dict_count = 0;
        length_t op_rewrite_count = 0;
        length_t exec_rewrite_count = 0;
        for (auto scan = trx->m_exec_ptr; scan > barrier; --scan) {
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

        // Read the matched @delimit-barrier's op-depth companion.  It sits on
        // the error stack just below the err_count inner companions that will
        // be captured.  The snapshot is the op-depth at delimit entry; the
        // ambient slice is whatever was pushed above it and is still there.
        //
        // If the body consumed items from below the snapshot (e.g. |locals|
        // bound a name to a pre-delimit op item), m_op_ptr sits BELOW the
        // snapshot.  In that case the slice is empty -- the captured body
        // will restore whatever state it needs (the |locals| dict, etc.)
        // through the dict-count path, not through the op slice.
        auto matched_op_depth = trx->m_err_ptr[-err_count].integer_value();
        auto op_snapshot_ptr = trx->m_op_base + matched_op_depth;
        auto op_count = (trx->m_op_ptr > op_snapshot_ptr) ? static_cast<length_t>(trx->m_op_ptr - op_snapshot_ptr)
                                                          : static_cast<length_t>(0);

        // Allocate ContinuationContext on VM heap
        auto alloc =
                ContinuationContext::alloc_size(exec_count, err_count, dict_count, op_count, op_rewrite_count, exec_rewrite_count);
        auto [ctx, ctx_offset] = trx->vm_alloc<ContinuationContext>(alloc);

        ctx->m_exec_count = exec_count;
        ctx->m_err_count = err_count;
        ctx->m_dict_count = dict_count;
        ctx->m_op_count = op_count;
        ctx->m_op_rewrite_count = op_rewrite_count;
        ctx->m_exec_rewrite_count = exec_rewrite_count;
        ctx->m_alloc_global_at_capture = trx->m_curr_alloc_global;

        // Copy exec segment bottom-first: barrier+1 is the bottom, m_exec_ptr is the top.
        // Stored contiguously so execute_continuation can memcpy to restore.
        auto exec_base = barrier + 1;
        std::memcpy(&ctx->m_objects[0], exec_base, exec_count * sizeof(Object));

        // Copy inner-barrier error stack entries bottom-first (oldest first for memcpy restore).
        auto err_base = trx->m_err_ptr - err_count + 1;
        std::memcpy(&ctx->m_objects[exec_count], err_base, err_count * sizeof(Object));
        // Pop inner companions + matched @delimit-barrier's op-depth companion.
        trx->m_err_ptr -= (err_count + 1);

        // Copy and pop top dict_count frame dicts (outermost first for memcpy restore).
        // The @end-locals markers in the exec segment appear innermost-on-top because
        // begin-locals pushes them bottom-first (@end-locals below, body above), so
        // the topmost K dicts on the dict stack correspond to K @end-locals markers.
        // Each popped dict has its cached name bindings cleared from the running
        // coroutine's binding table; execute_continuation repopulates on resume.
        auto dict_base = trx->m_dict_ptr - dict_count + 1;
        std::memcpy(&ctx->m_objects[exec_count + err_count], dict_base, dict_count * sizeof(Object));
        for (length_t i = 0; i < dict_count; ++i) {
            auto dict = dict_base[i].dict_value(trx);
            dict->clear_name_bindings(trx);
        }
        trx->m_dict_ptr -= dict_count;

        // Copy and trim the ambient op slice.  When op_count > 0 the slice
        // runs from op_snapshot_ptr+1 through m_op_ptr; memcpy it into the
        // ctx and trim m_op_ptr back to the snapshot.  When op_count == 0
        // either (a) the body pushed nothing above the snapshot or (b) the
        // body consumed items from below it (e.g. |locals| bound to a
        // pre-delimit item); leave m_op_ptr where it is -- moving it up to
        // the snapshot would expose stale bytes that were logically
        // consumed.
        if (op_count > 0) {
            auto op_slice_base = op_snapshot_ptr + 1;
            std::memcpy(&ctx->m_objects[exec_count + err_count + dict_count], op_slice_base, op_count * sizeof(Object));
            trx->m_op_ptr = op_snapshot_ptr;
        }

        // Emit op-depth rewrite-offset tables.  Two parallel arrays:
        //   - err_rewrite_array: each captured op-depth-bearing barrier
        //     (@delimit-barrier, @handler-scope) records one offset (in
        //     Object units, 0-based from err-segment start) of the slot
        //     to rewrite.
        //   - exec_rewrite_array: each captured @try-rollback records one
        //     offset (in Object units, 0-based from exec-segment start) of
        //     the saved-depth Integer that lives one slot below @try-rollback
        //     in the captured exec range.
        // Walk the captured exec range BOTTOM-UP to match the bottom-first
        // err memcpy ordering; running_err_offset tracks position within the
        // captured err segment.  Captured-exec start (for offset math) is
        // barrier+1 in capture_op (matched @delimit-barrier excluded).
        // For err-companion rewrites (delimit-barrier, handler-scope), the
        // rewrite target sits at running_err_offset + (err_companions - 1)
        // -- the LAST err slot the op owns is the op-depth Integer.  For
        // exec-companion rewrites (try-rollback / try-result / forall-family
        // exec_companions=1, logic-family exec_companions=2), the saved/op
        // depth Integer sits at scan - exec_companions.  Captured-exec start
        // is barrier+1 here (matched @delimit-barrier excluded from capture).
        if ((op_rewrite_count > 0) || (exec_rewrite_count > 0)) {
            auto err_rewrite_array = &ctx->m_objects[exec_count + err_count + dict_count + op_count];
            auto exec_rewrite_array = &ctx->m_objects[exec_count + err_count + dict_count + op_count + op_rewrite_count];
            length_t err_rw_idx = 0;
            length_t exec_rw_idx = 0;
            length_t running_err_offset = 0;
            for (auto scan = barrier + 1; scan <= trx->m_exec_ptr; ++scan) {
                if (scan->is_operator() && is_control_op_atom(scan->m_operator)) {
                    const auto &desc = op_descriptor_for(scan->m_operator);
                    if (desc.needs_op_rewrite) {
                        auto target = static_cast<length_t>(running_err_offset + desc.err_companions - 1);
                        err_rewrite_array[err_rw_idx++] = Object::make_integer(static_cast<integer_t>(target));
                    }
                    if (desc.needs_exec_rewrite) {
                        assert(scan - desc.exec_companions >= barrier + 1);
                        assert((scan - desc.exec_companions)->is_integer());
                        auto captured_exec_offset = static_cast<length_t>((scan - desc.exec_companions) - (barrier + 1));
                        exec_rewrite_array[exec_rw_idx++] = Object::make_integer(static_cast<integer_t>(captured_exec_offset));
                    }
                    running_err_offset = static_cast<length_t>(running_err_offset + desc.err_companions);
                }
            }
            assert(err_rw_idx == op_rewrite_count);
            assert(exec_rw_idx == exec_rewrite_count);
            assert(running_err_offset == err_count);
        }

        // Remove the barrier and captured segment from exec stack (move, not free)
        trx->m_exec_ptr = barrier - 1;

        // Install @handler-scope above the trimmed exec with two err
        // companions: K's identity (ctx_offset -- abort-exec scans for
        // the scope whose identity matches the invoked K), and op-depth
        // at handler-push time (pre-K-push op position, so abort-exec
        // can restore the pre-handler op state).  Push order: identity
        // first (deeper), op-depth on top.
        trx->require_error_capacity(2);
        trx->require_exec_capacity(2);
        auto handler_op_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);
        *++trx->m_err_ptr = Object::make_integer(static_cast<integer_t>(ctx_offset));
        *++trx->m_err_ptr = Object::make_integer(handler_op_depth);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atHandlerScope);

        // Create Continuation Object and push onto operand stack.  The capture
        // save level is stashed in the Object itself (not only in the ctx) so
        // execute_continuation can reject post-restore resume without first
        // dereferencing the possibly-freed ctx.
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_continuation(ctx_offset, trx->m_curr_save_level);

        // Execute handler (handler sees a clean op stack with K on top)
        handler.set_executable();
        *++trx->m_exec_ptr = handler;
    }
}

// abort-exec: resume-value K :- (does not return; splices K's captured segment)
//
// Abort variant of `exec` on Continuations: trims the resumer's
// exec/err/dict/op stacks down to K's matching @handler-scope before
// splicing the captured segment, so any receiver frames between the
// handler-push point and the abort-exec call are discarded.  Pair with
// `exec K` (splice, which leaves resumer frames in place).  Caller
// picks per site: splice for delimited-control idioms (shift/reset,
// generators), abort for Scheme-style call/cc.
//
// Matching: each @handler-scope carries its K's identity (ctx_offset)
// as an err companion.  abort-exec scans the exec stack for the scope
// whose identity equals the invoked K's ctx_offset, so nested captures
// resolve correctly (inner handler's `abort-exec OUTER-K` trims through
// @handler-scope(inner) down to @handler-scope(outer)).
//
// Stale scope: if K's @handler-scope has already been popped (handler
// returned normally, K stored and invoked later), abort-exec raises
// /invalid-access.  Users wanting post-scope invocation should use
// `exec K`, which doesn't require the scope.
//
// throws: opstack-underflow, type-check, invalid-access
static void abort_exec_op(Trix *trx) {
    trx->verify_operands(VerifyContinuation, VerifyAny);

    auto k_obj = *trx->m_op_ptr;
    auto k_offset = k_obj.continuation_offset();

    // Restore-dangle detection: if save level fell below capture level,
    // the ctx and any offsets it references have been reclaimed.  Reject
    // without dereferencing the ctx.  Belt to Save::reject_stale_value_
    // into_eqref's braces at the put site -- the common reachable path
    // (K into root-EqArray during save scope) is now rejected there, but
    // this check stays for any internal path that slips a stale K to
    // abort-exec.
    if (trx->m_curr_save_level < k_obj.continuation_save_level()) {
        trx->error(Error::InvalidAccess, "abort-exec: continuation invalidated by restore");
    } else {
        // Scan exec for the @handler-scope whose identity companion matches
        // k_offset.  Track err_scan past each barrier's companion(s) to keep
        // it aligned with the current barrier's companion slot.
        auto err_scan = trx->m_err_ptr;
        auto handler_scope = static_cast<Object *>(nullptr);
        integer_t matched_op_depth = 0;
        for (auto scan = trx->m_exec_ptr; scan >= trx->m_exec_base; --scan) {
            if (scan->is_operator()) {
                if (scan->operator_is_handlerscope()) {
                    // Companions on err: [identity (deeper), op-depth (on top)].
                    // err_scan points at op-depth; identity is at err_scan[-1].
                    auto identity = static_cast<vm_offset_t>(err_scan[-1].integer_value());
                    if (identity == k_offset) {
                        handler_scope = scan;
                        matched_op_depth = err_scan[0].integer_value();
                        break;
                    } else {
                        err_scan -= 2;
                    }
                } else if (scan->operator_is_trycatchbarrier() || scan->operator_is_finallybarrier() ||
                           scan->operator_is_withstream() || scan->operator_is_delimitbarrier() ||
                           scan->operator_is_effectbarrier() || scan->operator_is_endinglobal()) {
                    // @end-in-global owns one err companion (the saved alloc flag);
                    // a ${...} between the abort-exec site and the @handler-scope
                    // would otherwise leave err_scan one slot high for every barrier
                    // below it -> wrong identity/op-depth reads.
                    --err_scan;
                }
            }
        }

        if (handler_scope == nullptr) {
            trx->error(Error::InvalidAccess,
                       "abort-exec: continuation's handler scope no longer active; use exec for post-scope invocation");
        }

        // Pop K and resume value from op.  resume_value is preserved across
        // the trim and pushed back on the trimmed op stack so execute_continuation
        // pops it as the splice's resume value.
        --trx->m_op_ptr;
        auto resume_value = *trx->m_op_ptr--;

        // Unwind exec range above @handler-scope (pops barrier companions,
        // closes streams, pops locals, frees extvalues).
        unwind_exec_to(trx, handler_scope);

        // Pop @handler-scope's two err companions (identity + op-depth), then
        // drop the barrier itself from exec.
        trx->m_err_ptr -= 2;
        trx->m_exec_ptr = handler_scope - 1;

        // Trim op stack to the pre-handler state, freeing ExtValues on any
        // items the handler pushed before the abort-exec call.
        auto op_target = trx->m_op_base + matched_op_depth;
        maybe_free_extvalue_opstack(trx, op_target + 1);
        trx->m_op_ptr = op_target;

        // Place resume value on top of op; execute_continuation will pop it
        // as the splice's resume value.  Capacity is guaranteed (we only
        // shrank the op stack above).
        *++trx->m_op_ptr = resume_value;

        // Splice the captured segment.  execute_continuation handles the
        // spent check, capacity checks, frame dict restoration, op-slice
        // restore, fresh @delimit-barrier + op-depth companion, and exec/err
        // memcpy.  Same restore path as splice; only the pre-trim state
        // differs between the two ops.
        trx->execute_continuation(k_obj);
    }
}
