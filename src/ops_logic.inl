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

//===--- Logic / Backtracking Operators ---===//
//
// Implements a subset of Prolog's logic programming model, adapted for a
// stack-based VM.  Based on the unification and backtracking concepts from
// Warren's Abstract Machine (WAM), the standard execution model for Prolog:
//
//   David H.D. Warren, "An Abstract Prolog Instruction Set", SRI Technical
//   Note 309, 1983.
//
//   Hassan Ait-Kaci, "Warren's Abstract Machine: A Tutorial Reconstruction",
//   MIT Press, 1991.
//
// --- Core concepts for maintainers ---
//
// LOGIC VARIABLES (lvars)
//   A logic variable is an "unknown" that can be bound to a value exactly
//   once.  Once bound, the binding is permanent within the current search
//   branch.  If the search backtracks, the binding is automatically undone.
//
//   Representation: a tagged value /lvar wrapping a 1- or 2-element array.
//     - Unbound: arr[0] holds null.
//     - Bound:   arr[0] holds the bound value.
//     - Named:   arr[1] holds the debug Name (2-element array via named-var).
//   The array indirection allows in-place mutation (bind = array put),
//   while Trix's save/restore journal automatically captures and undoes
//   the mutation on backtrack.
//
// UNIFICATION
//   Unification is the process of making two terms equal by finding
//   consistent bindings for any logic variables they contain.
//
//     unify(X, 42)        -> binds X to 42
//     unify([X, 2], [1, Y]) -> binds X to 1 and Y to 2
//     unify(1, 2)         -> fails (no binding can make them equal)
//
//   The algorithm recursively walks both terms in parallel:
//     1. Dereference both sides (follow binding chains to concrete values).
//     2. If identical, succeed.
//     3. If either is an unbound lvar, bind it to the other side.
//     4. If both are arrays/records/tagged with matching structure, unify
//        each element pairwise (recursive).
//     5. Otherwise, fail.
//
//   This is standard Martelli-Montanari unification, without occurs check
//   (no cycle detection -- kept simple since circular terms are unlikely
//   in embedded scripting).
//
// BACKTRACKING (choice points)
//   Backtracking is the ability to try multiple alternatives and undo
//   side effects when one fails.  This is how Prolog searches for solutions.
//
//   In Trix, backtracking piggybacks on the existing save/restore system:
//     - `choice` creates a save point (checkpoint) and tries the first
//       alternative from an array of procedures.
//     - If the alternative succeeds, @choice-barrier just discards the
//       choice frame; the save level stays active (bindings persist)
//       so the result is kept.
//     - If the alternative calls `fail`, @choice-fail restores the save
//       point (undoing all bindings from the failed attempt) and tries
//       the next alternative.
//     - If all alternatives fail, `choice` itself fails.
//
//   This reuse of save/restore is the key design insight: Prolog's "trail"
//   (the undo log for variable bindings) is exactly Trix's save journal.
//   No separate trail stack is needed.
//
//   `guard` (Prolog's "require") fails the current branch if a condition
//   is false, triggering backtrack to the most recent choice point.
//
//   `cut` commits to the current choice, discarding remaining alternatives.
//   Implemented by nulling the 4-slot choice frame on the exec stack
//   (the interpreter treats Null as a no-op, so the proc body continues).
//
// DEREFERENCING
//   Logic variables can be bound to other logic variables, forming chains:
//     X -> Y -> 42
//   `deref` follows the chain to find the concrete value (42), or returns
//   the last unbound variable if the chain is incomplete.
//   Depth-limited to MaxDerefDepth to guard against cycles.
//
// --- Operators ---
//
//   logic-var      -- lvar             Create an unbound logic variable
//   named-var      name -- lvar        Create a named logic variable
//   lvar-name      lvar -- name t|f    Get debug name of a logic variable
//   unify          a b -- bool         Unify two terms (bind lvars as needed)
//   deref          any -- any          Follow binding chain to concrete value
//   fail           --                  Trigger backtrack to nearest choice point
//   choice         [procs] -- any      Try alternatives with automatic backtrack
//   guard          bool --             Fail current branch if false
//   cut            --                  Commit to current choice (discard alternatives)
//   is-logic-var   any -- bool         Test if value is a logic variable
//   bound?         lvar -- bool        Test if logic variable has been bound
//   copy-term      term -- term'       Deep copy with fresh logic variables
//   find-all       [procs] -- [results]  Collect all succeeding alternatives
//   find-n         n [procs] -- [results] Bounded collection (short-circuits)
//   once           proc -- ?           Single-attempt choice
//   naf            proc -- bool        Negation-as-failure
//   unify-match    val [[pat body]...] -- ? Unification-based dispatch
//   choice-count   [procs] -- int      Count successes without collecting
//   aggregate      init reducer [alts] -- result  Reduce over all solutions
//   for-each-solution  body [alts] --  Stream-process each solution
//
// Control operators (internal, not user-visible):
//   @choice-barrier      Success handler: discard frame, keep bindings, return result
//   @choice-fail         Failure handler: restore save, try next alternative
//   @find-all-barrier    Success handler: capture result, restore, try next
//   @find-all-fail       Failure handler: skip on Fail, re-raise others
//   @find-n-barrier      Success handler: capture result, restore, try next (bounded)
//   @find-n-fail         Failure handler: skip on Fail, re-raise others
//   @choice-count-barrier  Increment counter, restore, advance
//   @choice-count-fail     Restore, advance (skip failed)
//   @for-each-solution-barrier  Body ran, restore, advance
//   @for-each-solution-fail     Restore, advance or re-raise
//   @once-barrier        Success handler: pop save companion, done
//   @once-fail           Failure handler: restore save, re-raise error
//   @naf-barrier         Body succeeded: restore, result is false
//   @naf-fail            Body failed: restore, result is true (or re-raise)
//   @unify-match-barrier   Pattern matched, body ran: cleanup, return result
//   @unify-match-fail      Body failed: restore save, re-raise error (no clause retry)
//   @aggregate-barrier   Alt succeeded: schedule reducer with (acc result)
//   @aggregate-fail      Alt failed: restore, advance or re-raise
//   @aggregate-reduce-barrier  Reducer ran: store new acc, advance
//   @aggregate-reduce-fail     Reducer failed: restore, re-raise
//

//===--- Helpers ---===//

// Test whether an Object is a logic variable (tagged with /lvar).
[[nodiscard]] static bool is_lvar(Trix *trx, Object obj) {
    if (obj.is_tagged()) {
        auto pair = obj.tagged_storage(trx);
        return (pair[0].is_name() && (pair[0].name_offset() == trx->wellknown_offset(WellKnownName::LVar)));
    } else {
        return false;
    }
}

// Test whether a logic variable is unbound (inner array[0] == null).
[[nodiscard]] static bool is_lvar_unbound(Trix *trx, Object obj) {
    auto pair = obj.tagged_storage(trx);
    auto payload = pair[1];
    auto elem_data = payload.array_objects(trx);
    return elem_data[0].is_null();
}

// Get the bound value of a logic variable.
[[nodiscard]] static Object lvar_get_value(Trix *trx, Object obj) {
    auto pair = obj.tagged_storage(trx);
    auto payload = pair[1];
    auto elem_data = payload.array_objects(trx);
    return elem_data[0];
}

// Bind a logic variable to a value (journaled array put).
// At BASE save level, the write is unjournaled (same as regular put).
// At any higher level, the old value is journaled so restore undoes the binding.
//
// value_consumable distinguishes the two ownership scenarios:
//   - true  (transfer): the caller (unify_impl at top level) owns `value` and
//     is handing it off.  We move m_offset into the binding via make_copy --
//     a shallow Object struct copy that preserves m_offset.  The caller MUST
//     NOT call maybe_free_extvalue on this operand afterwards, since the
//     binding is now the live owner of the ExtValue/WideValue.
//   - false (clone): `value` is owned by something still alive (a container
//     slot, a binding chain we deref'd through).  Aliasing m_offset would
//     leave two live owners of the same ExtValue, and the next restore that
//     walks the binding journal would maybe_free_extvalue the shared slot
//     out from under the other owner.  Allocate a fresh ExtValue via
//     make_clone so the binding's m_offset is independent.  This avoids a
//     partial-bind-then-restore use-after-free of the shared slot.
//
// For types that neither use ExtValue nor WideValue (Tagged, Array, Record,
// Boolean, ...) make_clone is the same machine cost as make_copy (just an
// 8-byte struct copy), so the consumability flag has no perf cost on those.
// The clone cost (one ExtValue::alloc + 8-byte payload memcpy) only lands
// on scalar Long/Double/Address/ULong binds done through deep recursion.
static void lvar_bind(Trix *trx, Object lvar, Object value, bool value_consumable) {
    auto pair = lvar.tagged_storage(trx);
    auto payload = pair[1];
    auto elem_data = payload.array_objects(trx);
    auto curr_save_level = trx->m_curr_save_level;
    if (elem_data[0].save_level() != curr_save_level) {
        // Journal the OLD binding so the per-alternative restore (find-all /
        // choice / aggregate / for-each-solution) reverts it.  An lvar created
        // inside ${...} has a GLOBAL inner array, and a plain save_object would
        // SKIP the global slot -- leaving the binding un-undoable, so every
        // alternative after the first unifies against the stale binding and
        // silently fails.  Route a global binding slot through the LvarBinding
        // journal path (recorded despite being global; gc_walk_chain pins the
        // inner-array block so the slot stays valid until restore); a local slot
        // takes the ordinary path.  Splitting by region keeps Flavor::LvarBinding
        // a crisp "binding cell in global VM" tag, disjoint from Flavor::Object.
        if (trx->is_global(trx->ptr_to_offset(&elem_data[0]))) {
            Save::save_object_journal_global(trx, &elem_data[0]);
        } else {
            Save::save_object(trx, &elem_data[0]);
        }
    } else {
        elem_data[0].maybe_free_extvalue(trx);
    }
    if (value_consumable) {
        elem_data[0] = value.make_copy(curr_save_level);
    } else {
        elem_data[0] = value.make_clone(trx).make_copy(curr_save_level);
    }
}

// Result of unify_impl.  matched=true on successful unification, false on
// structural mismatch.
//
// a_transferred / b_transferred report whether the corresponding top-level
// input operand had its ExtValue ownership moved into an lvar binding via
// the transfer path of lvar_bind (value_consumable=true).  When true, the
// caller (unify_op / unify_match_op) MUST NOT call maybe_free_extvalue on
// that operand -- the binding is the live owner and an explicit free would
// yank the ExtValue out from under it.  When false, the operand's slot is
// still the sole owner and the caller's normal free path applies.
//
// Recursive sub-calls into containers (array, record, tagged pair) ALWAYS
// receive a_consumable=false, b_consumable=false from the parent, so sub-
// binds take the deep-clone path and never set a sub-result transfer bit.
// Sub-results therefore propagate {matched, false, false}; the parent's
// transfer bits are decided purely by the parent's own consumable inputs.
struct UnifyResult {
    bool matched;
    bool a_transferred;
    bool b_transferred;
};

// Dereference: chase variable binding chain to find concrete value.
// Returns the lvar itself if unbound.  Depth-limited to prevent cycles.
[[nodiscard]] static Object deref_impl(Trix *trx, Object term, int depth) {
    while ((depth < MaxDerefDepth) && is_lvar(trx, term)) {
        if (is_lvar_unbound(trx, term)) {
            return term;
        } else {
            term = lvar_get_value(trx, term);
            ++depth;
        }
    }
    return term;
}

// Recursive structural unification.  Returns {matched, a_transferred,
// b_transferred} -- see UnifyResult above.
//
// a_consumable / b_consumable signal whether the caller owns the
// corresponding operand's ExtValue/WideValue and is offering to transfer
// it on a top-level lvar bind.  Recursive sub-calls into container slots
// pass false, false (the container is the live owner, sub-elements are
// not consumable).  When deref walks through a bound-lvar chain, the
// post-deref value comes from the binding slot -- still a live owner --
// so we flip the corresponding consumable flag to false.
[[nodiscard]] static UnifyResult unify_impl(Trix *trx, Object a, Object b, bool a_consumable, bool b_consumable, int depth) {
    if (depth > MaxUnifyDepth) {
        trx->error(Error::LimitCheck, "unify: recursion depth exceeded (max {})", MaxUnifyDepth);
    } else {
        // Track whether the pre-deref operand was a bound lvar -- if so, the
        // post-deref value is owned by the binding chain, not by the caller.
        auto a_was_bound_lvar = is_lvar(trx, a) && !is_lvar_unbound(trx, a);
        auto b_was_bound_lvar = is_lvar(trx, b) && !is_lvar_unbound(trx, b);
        a = deref_impl(trx, a, 0);
        b = deref_impl(trx, b, 0);
        if (a_was_bound_lvar) {
            a_consumable = false;
        }
        if (b_was_bound_lvar) {
            b_consumable = false;
        }

        // Identical objects (same type, same value)
        if (a.equal(trx, b)) {
            return {true, false, false};
        } else if (is_lvar(trx, a) && is_lvar_unbound(trx, a)) {
            // a is an unbound lvar -- bind a to b.  b's ownership may transfer.
            lvar_bind(trx, a, b, b_consumable);
            return {true, false, b_consumable};
        } else if (is_lvar(trx, b) && is_lvar_unbound(trx, b)) {
            // b is an unbound lvar -- bind b to a.  a's ownership may transfer.
            lvar_bind(trx, b, a, a_consumable);
            return {true, a_consumable, false};
        } else if (a.is_array() && b.is_array()) {
            // Array pairwise unification.  Sub-elements are owned by their
            // containers -- pass consumable=false on the recursion so any
            // deep bind takes the make_clone path.
            auto [a_ptr, a_len] = a.array_value(trx);
            auto [b_ptr, b_len] = b.array_value(trx);
            if (a_len != b_len) {
                return {false, false, false};
            } else {
                for (length_t i = 0; i < a_len; ++i) {
                    auto sub = unify_impl(trx, a_ptr[i], b_ptr[i], false, false, depth + 1);
                    if (!sub.matched) {
                        return {false, false, false};
                    }
                }
                return {true, false, false};
            }
        } else if (a.is_record() && b.is_record()) {
            // Record pairwise unification (same schema)
            auto a_len = a.object_length();
            auto b_len = b.object_length();
            if (a_len != b_len) {
                return {false, false, false};
            } else {
                auto a_inst = a.record_instance(trx);
                auto b_inst = b.record_instance(trx);
                // Compare schemas: same offset means identical; different offsets require
                // field-name comparison (record with identical name arrays produces
                // separate schema allocations).
                if (a_inst->m_schema != b_inst->m_schema) {
                    auto a_schema = trx->offset_to_ptr<RecordSchema>(a_inst->m_schema);
                    auto b_schema = trx->offset_to_ptr<RecordSchema>(b_inst->m_schema);
                    for (length_t i = 0; i < a_len; ++i) {
                        if (a_schema->m_names[i].name_offset() != b_schema->m_names[i].name_offset()) {
                            return {false, false, false};
                        }
                    }
                }
                for (length_t i = 0; i < a_len; ++i) {
                    auto sub = unify_impl(trx, a_inst->m_fields[i], b_inst->m_fields[i], false, false, depth + 1);
                    if (!sub.matched) {
                        return {false, false, false};
                    }
                }
                return {true, false, false};
            }
        } else if (a.is_tagged() && b.is_tagged()) {
            // Tagged pairwise unification (same tag name)
            auto a_pair = a.tagged_storage(trx);
            auto b_pair = b.tagged_storage(trx);
            if (a_pair[0].name_offset() != b_pair[0].name_offset()) {
                return {false, false, false};
            } else {
                return unify_impl(trx, a_pair[1], b_pair[1], false, false, depth + 1);
            }
        } else {
            return {false, false, false};
        }
    }
}

//===--- Standard Operators ---===//

// logic-var: -- lvar
// Creates an unbound logic variable: /lvar [null] tag
// throws: vm-full, opstack-overflow
static void logic_var_op(Trix *trx) {
    trx->require_op_capacity(1);

    // Root the inner array on the operand stack across make_tagged: both
    // make_empty_array and make_tagged route through the global-aware dispatch
    // allocator, so inside ${...} make_tagged can fire vm_global_gc and would
    // otherwise sweep the just-built, still-unrooted array.
    auto arr_obj = Object::make_empty_array(trx, 1);
    arr_obj.set_save_level(trx->m_curr_save_level);
    *++trx->m_op_ptr = arr_obj;
    *trx->m_op_ptr = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::LVar), arr_obj);
}

// named-var: name -- lvar
// Creates an unbound logic variable with a debug name: /lvar [null, name] tag
// The debug name is stored as the second element of the inner array.
// throws: opstack-underflow, type-check, vm-full
static void named_var_op(Trix *trx) {
    trx->verify_operands(VerifyName);

    auto debug_name_obj = *trx->m_op_ptr;
    auto curr_save_level = trx->m_curr_save_level;

    // Allocate the 2-element [null, debug_name] array via the global-aware
    // dispatch allocator (mirroring logic_var_op): inside ${...} the make_tagged
    // wrapper lands in global VM, so its payload array must too -- a local array
    // under a global lvar dangles once the enclosing save level unwinds.  The
    // name operand stays rooted at *m_op_ptr across the array allocation; root
    // the finished array across make_tagged's own (GC-capable) allocation.
    auto [arr, arr_offset] = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Array);
    arr[0] = Object::make_null(curr_save_level);
    arr[1] = debug_name_obj;
    auto arr_obj = Object::make_array(arr_offset, 2);
    arr_obj.set_save_level(curr_save_level);

    trx->require_op_capacity(1);
    *++trx->m_op_ptr = arr_obj;
    auto tagged = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::LVar), arr_obj);
    --trx->m_op_ptr;
    *trx->m_op_ptr = tagged;
}

// lvar-name: lvar -- name true | false
// Returns the debug name of a logic variable, or false if unnamed.
// throws: opstack-underflow, type-check
static void lvar_name_op(Trix *trx) {
    trx->require_op_count(1);

    auto lvar_obj = *trx->m_op_ptr;
    if (!is_lvar(trx, lvar_obj)) {
        trx->error(Error::TypeCheck, "lvar-name: operand is not a logic variable");
    } else {
        auto pair = lvar_obj.tagged_storage(trx);
        auto payload = pair[1];
        auto elem_data = payload.array_objects(trx);
        auto arr_len = payload.object_length();

        if ((arr_len >= 2) && elem_data[1].is_name()) {
            trx->require_op_capacity(1);
            *trx->m_op_ptr = elem_data[1];
            *++trx->m_op_ptr = Object::make_boolean(true);
        } else {
            *trx->m_op_ptr = Object::make_boolean(false);
        }
    }
}

// unify: a b -- bool
// Structural unification.  Binds unbound logic variables.
// throws: opstack-underflow, limit-check
//
// ExtValue ownership: lvar_bind transfers the bound value's m_offset into
// the lvar's binding storage on the consumable path (top-level binds) and
// deep-clones on the non-consumable path (recursive sub-binds and binds
// of values that came through a deref'd binding chain).  The transferred
// path is the live owner; freeing that operand here would yank the
// ExtValue out from under the lvar.  The clone path leaves the caller's
// slot as the sole owner of its original ExtValue, so it must be freed.
// Two operands, two transfer bits: free each one independently based on
// whether its ownership actually moved.
// throws: limit-check, opstack-underflow, vm-full
static void unify_op(Trix *trx) {
    trx->require_op_count(2);

    // Keep BOTH operands rooted on the op stack across unify_impl: a scalar
    // bind during the recursion can clone an ExtValue via gvm_alloc (in ${...}),
    // which may fire vm_global_gc -- a popped operand's backing block would then
    // be unrooted and swept.  Pop only after unify_impl returns.
    auto b = *trx->m_op_ptr;
    auto a = *(trx->m_op_ptr - 1);

    auto [matched, a_transferred, b_transferred] = unify_impl(trx, a, b, true, true, 0);
    if (!a_transferred) {
        a.maybe_free_extvalue(trx);
    }
    if (!b_transferred) {
        b.maybe_free_extvalue(trx);
    }
    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_boolean(matched);
}

// deref: term -- value
// Walks variable binding chain to find concrete value.
// Returns the lvar itself if unbound.
// throws: opstack-underflow
static void deref_op(Trix *trx) {
    trx->require_op_count(1);

    auto input = *trx->m_op_ptr;
    auto result = deref_impl(trx, input, 0);
    // When the input is a BOUND lvar, deref_impl returns the value stored in
    // the binding slot -- a borrowed alias of the ExtValue/WideValue the
    // binding owns.  This is the same ownership rule unify_impl enforces (it
    // flips a_consumable/b_consumable to false for a deref'd bound lvar): the
    // binding chain is the live owner, not the caller.  But the op-stack slot
    // must be the SOLE owner of what it holds -- a later pop / consuming op
    // calls maybe_free_extvalue on it, which would free the binding's ExtValue
    // and leave the lvar's binding dangling (a use-after-free the global GC
    // trips on as "references a freed block").  Clone so the result is an
    // independent owner.  (An unbound-lvar input derefs to the lvar itself --
    // cloning would forge a distinct lvar; a non-lvar input derefs to itself,
    // already owned by the slot -- cloning would orphan it.  So guard on the
    // bound-lvar case.  For container bindings make_clone is a shallow struct
    // copy, which is harmless: pop never frees a composite.)
    if (is_lvar(trx, input) && !is_lvar_unbound(trx, input)) {
        result = result.make_clone(trx);
    }
    *trx->m_op_ptr = result;
}

// fail: --
// Triggers backtracking.  Raises Error::Fail, caught by the nearest
// choice point (@choice-barrier).  If no choice point exists, propagates
// as a normal error (catchable by user try).
// throws: fail
static void fail_op(Trix *trx) {
    trx->error(Error::Fail, "fail");
}

// guard: bool --
// If false, calls fail.  A guard for use inside choice alternatives.
// throws: opstack-underflow, type-check, fail
static void guard_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);

    auto value = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;
    if (!value) {
        trx->error(Error::Fail, "guard: condition is false");
    }
}

// is-logic-var: any -- bool
// True if the operand is a tagged value with tag /lvar.
// throws: opstack-underflow
static void is_logic_var_op(Trix *trx) {
    trx->require_op_count(1);

    *trx->m_op_ptr = Object::make_boolean(is_lvar(trx, *trx->m_op_ptr));
}

// bound?: lvar -- bool
// True if the logic variable's inner array[0] is not null.
// throws: opstack-underflow, type-check
static void is_bound_op(Trix *trx) {
    trx->require_op_count(1);

    if (is_lvar(trx, *trx->m_op_ptr)) {
        *trx->m_op_ptr = Object::make_boolean(!is_lvar_unbound(trx, *trx->m_op_ptr));
    } else {
        trx->error(Error::TypeCheck, "bound?: expected logic variable");
    }
}

//===--- Copy-Term ---===//

// Lvar mapping for copy-term: parallel arrays of old offsets and new lvars.
// Linear scan (O(n^2) worst case, but n = distinct unbound lvars is small).
struct CopyTermState {
    vm_offset_t *old_offsets;
    Object *new_lvars;
    length_t count;
    length_t capacity;
};

// Look up an lvar in the mapping; returns null offset if not found.
[[nodiscard]] static Object copy_term_find_lvar(CopyTermState &state, vm_offset_t offset) {
    for (length_t i = 0; i < state.count; ++i) {
        if (state.old_offsets[i] == offset) {
            return state.new_lvars[i];
        }
    }
    return Object::make_null();
}

// Record a mapping from old lvar offset to new lvar.
static void copy_term_add_lvar(Trix *trx, CopyTermState &state, vm_offset_t offset, Object new_lvar) {
    if (state.count >= state.capacity) {
        trx->error(Error::LimitCheck, "copy-term: too many distinct logic variables");
    } else {
        state.old_offsets[state.count] = offset;
        state.new_lvars[state.count] = new_lvar;
        ++state.count;
    }
}

// Recursive term copier.  Clones the term, replacing each unbound lvar
// with a fresh one (preserving sharing: same old lvar -> same new lvar).
// Bound lvars are dereferenced and the bound value is copied.
[[nodiscard]] static Object copy_term_impl(Trix *trx, Object term, CopyTermState &state, int depth) {
    if (depth >= MaxCopyTermDepth) {
        trx->error(Error::LimitCheck, "copy-term: depth limit exceeded");
    } else if (is_lvar(trx, term)) {
        // Dereference logic variables
        if (is_lvar_unbound(trx, term)) {
            // Unbound lvar: check mapping, or create fresh
            auto offset = term.tagged_offset();
            auto mapped = copy_term_find_lvar(state, offset);
            if (mapped.is_null()) {
                // Create fresh lvar (same as logic_var_op).  Root the inner
                // array across make_tagged: both allocate via the global-aware
                // dispatch path, so make_tagged can fire vm_global_gc and would
                // otherwise sweep the just-built (unrooted) array.
                auto arr_obj = Object::make_empty_array(trx, 1);
                arr_obj.set_save_level(trx->m_curr_save_level);
                trx->require_op_capacity(1);
                *++trx->m_op_ptr = arr_obj;
                auto fresh = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::LVar), arr_obj);
                --trx->m_op_ptr;
                copy_term_add_lvar(trx, state, offset, fresh);
                return fresh;
            } else {
                return mapped;
            }
        } else {
            // Bound lvar: deref and copy the bound value
            auto value = deref_impl(trx, term, 0);
            return copy_term_impl(trx, value, state, depth + 1);
        }
    } else if (term.is_array()) {
        // Array: recursively copy each element.  Build the spine via the
        // global-aware dispatch allocator (so a copy made inside ${...} lands
        // wholly in global VM and survives save/restore) and root it on the
        // operand stack across the per-element recursion: each child allocation
        // can fire vm_global_gc, which walks only the op/exec stacks -- an
        // unrooted half-built spine (and the fresh global lvars already written
        // into it) would otherwise be swept.
        auto length = term.arrays_length();
        if (length == 0) {
            return term.make_clone(trx);
        } else {
            auto src_data = term.array_objects(trx);
            auto [dst, dst_offset] = trx->vm_alloc_dispatch_n<Object>(length, Trix::ChunkKind::Array);
            auto null_obj = Object::make_null(trx->m_curr_save_level);
            std::fill_n(dst, length, null_obj);
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = Object::make_array(dst_offset, length);
            for (length_t i = 0; i < length; ++i) {
                dst[i] = copy_term_impl(trx, src_data[i], state, depth + 1);
            }
            --trx->m_op_ptr;
            return Object::make_array(dst_offset, length);
        }
    } else if (term.is_tagged()) {
        // Tagged (non-lvar): copy tag name, recursively copy payload.  make_tagged
        // allocates the [name, payload] pair via the global-aware dispatch
        // allocator before storing payload, so it can fire vm_global_gc inside
        // ${...}; root the freshly built payload on the operand stack across
        // that allocation so a sweep cannot reclaim it out from under the pair.
        auto pair = term.tagged_storage(trx);
        auto tag_name = pair[0];
        auto payload = pair[1];
        auto new_payload = copy_term_impl(trx, payload, state, depth + 1);
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = new_payload;
        auto new_tagged = Object::make_tagged(trx, tag_name, new_payload);
        --trx->m_op_ptr;
        return new_tagged;
    } else if (term.is_record()) {
        // Record: same schema, recursively copy each field value.  Mirror
        // make_record_op's rooting (ops_record.inl:302-340): allocate via the
        // global-aware dispatch allocator, stamp the field count, set the schema
        // and null-init the fields so the instance is GC-walkable, then root it
        // on the operand stack across the per-field recursion (each child alloc
        // can fire vm_global_gc, which would otherwise sweep the half-built
        // instance and the fresh global children already written into it).
        auto inst = term.record_instance(trx);
        auto field_count = term.object_length();
        auto [new_inst, new_offset] =
                trx->vm_alloc_dispatch<RecordInstance>(RecordInstance::alloc_size(field_count), Trix::ChunkKind::Record);
        if (trx->m_curr_alloc_global) {
            trx->gvm_set_obj_count(new_offset, field_count);
        }
        new_inst->m_schema = inst->m_schema;
        auto null_obj = Object::make_null();
        for (length_t i = 0; i < field_count; ++i) {
            new_inst->m_fields[i] = null_obj;
        }
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_record(new_offset, field_count);
        for (length_t i = 0; i < field_count; ++i) {
            new_inst->m_fields[i] = copy_term_impl(trx, inst->m_fields[i], state, depth + 1);
        }
        --trx->m_op_ptr;
        return Object::make_record(new_offset, field_count);
    } else {
        // All other types: clone (scalars, strings, names, etc.)
        return term.make_clone(trx);
    }
}

// copy-term: term -- term'
// Deep copy of a term with fresh logic variables.  Unbound lvars are
// replaced with new lvars; the same old lvar maps to the same new lvar
// (shared variable preservation).  Bound lvars are dereferenced.
// throws: opstack-underflow, limit-check, vm-full
static void copy_term_op(Trix *trx) {
    trx->require_op_count(1);

    // Lvar mapping scratch (max 256 distinct lvars) lives on the C stack, NOT
    // the VM temp region.  copy_term_impl allocates inside ${...} via the
    // global-aware dispatch allocators, and each global bump moves m_vm_global
    // DOWN into any live temp slice (vm_heap.inl:606-625 warns of exactly this:
    // a vm_temp_alloc caller that keeps temp live across user-driven global
    // allocation).  Holding the map on the C stack removes that overlap hazard
    // entirely; the fresh lvars it indexes stay GC-reachable through the rooted
    // result spine, so the table itself need not be a GC root.
    static constexpr length_t MaxLvarMappings{256};
    vm_offset_t old_offsets[MaxLvarMappings];
    Object new_lvars[MaxLvarMappings];

    CopyTermState state{old_offsets, new_lvars, 0, MaxLvarMappings};
    auto result = copy_term_impl(trx, *trx->m_op_ptr, state, 0);
    trx->m_op_ptr->maybe_free_extvalue(trx);
    *trx->m_op_ptr = result;
}

//===--- Find-All ---===//

// Exec stack frame for find-all (4 slots, below @find-all-barrier):
//   [alt_array] [index_int] [save_obj] [@find-all-barrier]
// Operand stack accumulates results above a mark:
//   mark result0 result1 ... resultN

// Begin the next find-all attempt: save, push barrier + proc.
// Exec stack on entry: [alt_array] [index_int]
static void find_all_try_next(Trix *trx) {
    auto index = static_cast<length_t>(trx->m_exec_ptr->integer_value());
    auto alt_obj = *(trx->m_exec_ptr - 1);
    auto [alts, alt_len] = alt_obj.array_value(trx);

    if (index >= alt_len) {
        // All alternatives tried.  Collect results from running coroutine's scratch.
        trx->m_exec_ptr -= 2;  // pop alt_array + index
        trx->require_op_capacity(1);
        if (trx->m_scratch_ptr == trx->m_scratch_base) {
            *++trx->m_op_ptr = Object::make_empty_array(trx, 0);
        } else {
            *++trx->m_op_ptr = trx->template scratch_collect<Object>();
        }
    } else {
        // Save for this attempt
        trx->require_save_capacity(1);
        Save::save(trx);
        auto save_obj = *trx->m_op_ptr--;

        // Push save + barrier + proc on exec stack
        trx->require_exec_capacity(3);
        *++trx->m_exec_ptr = save_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atFindAllBarrier);
        *++trx->m_exec_ptr = alts[index];
    }
}

// find-all: array -- array
// Tries all alternatives, collecting results from those that succeed
// into the running coroutine's scratch arena.  Alternatives that call
// fail are silently skipped.  Non-fail errors propagate.  Results must
// be persistable types (inline scalars, ExtValue types, Names).
// Heap-referencing composites (Array, String, Dict, etc.) cannot survive
// the per-alternative save/restore cycle.
// throws: opstack-underflow, type-check, vm-full, limit-check
static void find_all_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto alt_obj = *trx->m_op_ptr--;
    auto [alts, alt_len] = alt_obj.array_value(trx);

    if (alt_len == 0) {
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_empty_array(trx, 0);
    } else {
        // Running coroutine's scratch arena must be idle (no nesting)
        if (trx->m_scratch_ptr != trx->m_scratch_base) {
            trx->error(Error::LimitCheck, "find-all: nested find-all/find-n not supported");
        } else {
            // Push exec frame: [alt_array, 0]
            trx->require_exec_capacity(2);
            *++trx->m_exec_ptr = alt_obj;
            *++trx->m_exec_ptr = Object::make_integer(0);

            find_all_try_next(trx);
        }
    }
}

// @find-all-barrier: (control operator -- success path)
// Fires when an alternative completes without error.  Captures the result,
// restores the save point, clones the result into the running coroutine's
// scratch arena, and advances to the next alternative.
static void at_find_all_barrier_op(Trix *trx) {
    // Interpreter already popped @find-all-barrier.
    // Exec stack: ... | alt_array | index_int | save_obj
    // Op stack: ... | result

    trx->require_op_count(1);

    // IMPORTANT: do NOT pop the result before Save::restore.  ExtValue types
    // (Long, ULong, Double, Address) store their 64-bit value in the VM heap.
    // Save::restore calls ExtValue::restore which scans the op and exec stacks
    // and relocates any ExtValues from above the save barrier to below it.
    // If the result were popped, its ExtValue would not be relocated and would
    // point at reclaimed memory after restore -- causing a double-free or crash.
    auto result_ptr = trx->m_op_ptr;

    if (!result_ptr->is_persistable_now(trx)) {
        --trx->m_op_ptr;
        trx->m_exec_ptr -= 3;  // pop save+index+alt
        trx->error(Error::TypeCheck,
                   "find-all: result type {} cannot survive restore; use deref to extract scalars, "
                   "or wrap the result construction in ${{...}} to allocate it in global VM",
                   Object::type_sv(result_ptr->type()));
    }

    // Restore save point.  Result on op stack is relocated by ExtValue::restore.
    auto save_obj = *trx->m_exec_ptr--;
    auto token = static_cast<vm_offset_t>(save_obj.integer_value());
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = save_obj;
    Save::restore_token(trx, token);

    // NOW safe to read the result -- its ExtValue has been relocated below the
    // barrier.  Clone it WHILE IT IS STILL ON THE OP STACK: make_clone allocates
    // (globally under ${...}), and under vm-gc-stress that GC fires mid-clone.
    // Had we popped the result to a bare C local first, it would be off every
    // GC-scanned root, so the GC would sweep it and make_clone would read a freed
    // (poisoned) cell -- every find-all result then comes back 0xA5A5..  Clone
    // from the rooted op-stack slot, THEN pop.
    auto clone = trx->m_op_ptr->make_clone(trx);
    --trx->m_op_ptr;
    trx->template scratch_push<Object>(clone);

    // Advance index
    auto index_ptr = trx->m_exec_ptr;
    *index_ptr = Object::make_integer(index_ptr->integer_value() + 1);

    find_all_try_next(trx);
}

// @find-all-fail: (control operator -- failure path)
// Fires when try_catch_handler replaces @find-all-barrier after an error.
// If the error is Fail, skip this alternative.  Non-fail errors propagate
// (error() calls scratch_free on the running coroutine).
static void at_find_all_fail_op(Trix *trx) {
    // Interpreter already popped @find-all-fail.
    // Exec stack: ... | alt_array | index_int | save_obj

    if (trx->m_last_error != Error::Fail) {
        // Non-fail error: clean up and re-raise (error() handles scratch_free)
        trx->m_exec_ptr -= 3;
        trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
    } else {
        // Restore save point (undo bindings from failed alternative).
        // Save::restore clamps m_vm_temp_ptr to m_vm_global, which suffices
        // since scratch lives in the coroutine's stack block.
        auto save_obj = *trx->m_exec_ptr--;
        auto token = static_cast<vm_offset_t>(save_obj.integer_value());
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = save_obj;
        Save::restore_token(trx, token);

        // Advance index
        auto index_ptr = trx->m_exec_ptr;
        *index_ptr = Object::make_integer(index_ptr->integer_value() + 1);

        find_all_try_next(trx);
    }
}

//===--- Choice Point Helpers ---===//

// Set up and try the next alternative in a choice array.
// Exec stack on entry: ... | alternatives_array | index_int
// (caller has already verified index < length)
//
// Choice frame layout (4 slots on exec stack):
//   [alt_array] [index_int] [save_token] [@choice-barrier]
// The save token is stored on the exec stack (not the error stack)
// so that `cut` can remove the entire frame by nulling 4 slots.
static void choice_try_next(Trix *trx) {
    // Read alternatives and index from exec stack
    auto index = static_cast<length_t>(trx->m_exec_ptr->integer_value());
    auto alt_obj = *(trx->m_exec_ptr - 1);
    auto [alts, alt_len] = alt_obj.array_value(trx);

    // Create a save point for this alternative
    trx->require_save_capacity(1);
    Save::save(trx);
    // save pushed an Integer save token on op stack -- move to exec stack
    auto save_obj = *trx->m_op_ptr--;

    // Push save, @choice-barrier, and the alternative proc on exec stack
    trx->require_exec_capacity(3);
    *++trx->m_exec_ptr = save_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atChoiceBarrier);
    *++trx->m_exec_ptr = alts[index];
}

//===--- Choice Operator ---===//

// choice: array -- ?
// Tries alternatives with automatic backtracking via save/restore.
// On success: returns whatever the successful alternative left on the
// operand stack.  On failure (all alternatives exhausted): raises Fail.
// throws: opstack-underflow, type-check, fail
static void choice_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto alt_obj = *trx->m_op_ptr--;
    auto [alts, alt_len] = alt_obj.array_value(trx);

    if (alt_len == 0) {
        trx->error(Error::Fail, "choice: empty alternatives array");
    } else {
        // Push alternatives array and initial index on exec stack
        trx->require_exec_capacity(2);
        *++trx->m_exec_ptr = alt_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);

        choice_try_next(trx);
    }
}

//===--- Control Operators ---===//

// @choice-barrier: (control operator -- success path)
// Fires when an alternative completes without raising Fail.
// Cleans up the exec stack companions (save_obj, index, alternatives).
// The save level stays active (bindings persist).
static void at_choice_barrier_op(Trix *trx) {
    // Pop save_obj, index, and alternatives from exec stack
    // (interpreter already popped @choice-barrier)
    trx->m_exec_ptr -= 3;
}

// @choice-fail: (control operator -- failure path)
// Fires when try_catch_handler replaces @choice-barrier after an error.
// If the error is Fail, restores the save point and tries the next
// alternative.  Non-fail errors are re-raised (propagate through choice).
static void at_choice_fail_op(Trix *trx) {
    // Check if this is a Fail error -- only Fail triggers backtracking.
    // Non-fail errors (division by zero, type check, etc.) propagate.
    if (trx->m_last_error != Error::Fail) {
        // Re-raise the original error.  Pop save+index+alternatives from exec stack.
        trx->m_exec_ptr -= 3;
        trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
    } else {
        // Recover save token from exec stack
        // (interpreter already popped @choice-fail, m_exec_ptr -> save_obj)
        auto save_obj = *trx->m_exec_ptr--;
        auto token = static_cast<vm_offset_t>(save_obj.integer_value());

        // Push save token on op stack for Save::restore
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = save_obj;
        Save::restore_token(trx, token);

        // After restore: exec stack still has | alternatives_array | index_int
        // (stacks survive restore)
        auto index = trx->m_exec_ptr->integer_value();
        auto alt_obj = *(trx->m_exec_ptr - 1);
        auto [alts, alt_len] = alt_obj.array_value(trx);

        if (++index >= static_cast<integer_t>(alt_len)) {
            // All alternatives exhausted -- propagate Fail to outer choice
            trx->m_exec_ptr -= 2;
            trx->error(Error::Fail, "choice: all alternatives exhausted");
        } else {
            // Update index on exec stack
            *trx->m_exec_ptr = Object::make_integer(index);

            // Try next alternative
            choice_try_next(trx);
        }
    }
}

// cut: --
// Commits to the current choice branch by removing the nearest choice
// point from the exec stack.  After cut, a subsequent `fail` will not
// backtrack to this choice point -- it propagates to the enclosing one.
// throws: invalid-exit (no enclosing choice point, or boundary crossed)
static void cut_op(Trix *trx) {
    // Scan exec stack for nearest @choice-barrier
    auto ptr = trx->m_exec_ptr;
    while (ptr >= trx->m_exec_base) {
        if (ptr->is_operator()) {
            if (ptr->operator_is_choicebarrier()) {
                break;
            } else if (ptr->operator_is_invalid_exit()) {
                trx->error(Error::InvalidExit, "cut: cannot cross @run or @stop boundary");
            }
        }
        --ptr;
    }
    if (ptr < trx->m_exec_base) {
        trx->error(Error::InvalidExit, "cut: no enclosing choice point");
    } else {
        // Null out the 4-slot choice frame:
        //   ptr=@choice-barrier, -1=save_obj, -2=index_int, -3=alt_array
        // Executable Null on the exec stack is silently ignored by the
        // interpreter (case Type::Null: break), so the remaining proc body
        // above continues executing normally.
        auto nop = Object::make_null();
        nop.set_executable();
        *ptr = nop;
        *(ptr - 1) = nop;
        *(ptr - 2) = nop;
        *(ptr - 3) = nop;
    }
}

//===--- Once Operator ---===//
//
// once: proc -- ?
// Single-attempt choice.  Tries proc once; if it succeeds, the result
// stays on the operand stack.  If it fails (Fail), the failure propagates.
// Cleaner than `[proc] choice` (avoids array allocation).
// Exec frame: [save_obj] [@once-barrier]

// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, vm-full
static void once_op(Trix *trx) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation);
    auto proc_obj = *trx->m_op_ptr--;

    trx->require_save_capacity(1);
    Save::save(trx);
    auto save_obj = *trx->m_op_ptr--;

    trx->require_exec_capacity(3);
    *++trx->m_exec_ptr = save_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atOnceBarrier);
    *++trx->m_exec_ptr = proc_obj;
}

// @once-barrier: proc succeeded -- pop save, done (save level stays active).
static void at_once_barrier_op(Trix *trx) {
    --trx->m_exec_ptr;  // pop save_obj companion
}

// @once-fail: proc failed -- restore save, re-raise error.
static void at_once_fail_op(Trix *trx) {
    if (trx->m_last_error != Error::Fail) {
        --trx->m_exec_ptr;
        trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
    } else {
        auto save_obj = *trx->m_exec_ptr--;
        auto token = static_cast<vm_offset_t>(save_obj.integer_value());
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = save_obj;
        Save::restore_token(trx, token);
        trx->error(Error::Fail, "once: goal failed");
    }
}

//===--- Naf Operator ---===//
//
// naf: proc -- bool
// Negation-as-failure.  Returns true if proc fails (Fail), false if it
// succeeds.  All variable bindings are rolled back regardless of outcome.
// Non-fail errors propagate.
// Exec frame: [save_obj] [@naf-barrier]

// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, vm-full
static void naf_op(Trix *trx) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation);
    auto proc_obj = *trx->m_op_ptr--;

    trx->require_save_capacity(1);
    Save::save(trx);
    auto save_obj = *trx->m_op_ptr--;

    // Save op stack depth so barrier can discard proc results
    auto op_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);

    trx->require_exec_capacity(4);
    *++trx->m_exec_ptr = Object::make_integer(op_depth);
    *++trx->m_exec_ptr = save_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atNafBarrier);
    *++trx->m_exec_ptr = proc_obj;
}

// @naf-barrier: proc succeeded -- goal IS satisfiable, so naf returns false.
// Always restore (naf rolls back bindings regardless).
static void at_naf_barrier_op(Trix *trx) {
    auto save_obj = *trx->m_exec_ptr--;
    // saved_op_depth is the signed offset m_op_ptr - m_op_base at naf
    // entry; -1 when the op stack was empty (m_op_ptr sits below
    // m_op_base by convention).  Earlier this site cast to length_t
    // (uint16_t), which wrapped -1 -> 65535 and put target above any
    // real m_op_ptr -- the trim silently no-op'd.  Kept benign in
    // practice because the rewrite-table fix at K-resume now overwrites
    // the captured Integer with the resumer's fresh_op_depth (also
    // signed; same convention).  Other logic barriers in this family
    // already used integer_t -- this one was the outlier.
    auto saved_op_depth = trx->m_exec_ptr->integer_value();
    --trx->m_exec_ptr;
    auto token = static_cast<vm_offset_t>(save_obj.integer_value());

    // Discard any results the proc left on the op stack (free ExtValues)
    auto target = trx->m_op_base + saved_op_depth;
    while (trx->m_op_ptr > target) {
        trx->m_op_ptr->maybe_free_extvalue(trx);
        --trx->m_op_ptr;
    }

    trx->require_op_capacity(1);
    *++trx->m_op_ptr = save_obj;
    Save::restore_token(trx, token);

    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_boolean(false);
}

// @naf-fail: proc failed -- goal is NOT satisfiable, so naf returns true.
static void at_naf_fail_op(Trix *trx) {
    if (trx->m_last_error != Error::Fail) {
        trx->m_exec_ptr -= 2;  // pop save + op_depth companions
        trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
    } else {
        auto save_obj = *trx->m_exec_ptr--;
        --trx->m_exec_ptr;  // pop op_depth (not needed on fail path)
        auto token = static_cast<vm_offset_t>(save_obj.integer_value());
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = save_obj;
        Save::restore_token(trx, token);

        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_boolean(true);
    }
}

//===--- Unify-Match Operator ---===//
//
// unify-match: value [[pattern body] ...] -- ?
// Unification-based pattern dispatch.  Tries each [pattern body] pair in
// order: saves, attempts unify(value, pattern), and if unification succeeds
// executes the body with logic variable bindings intact.  First match wins;
// no backtracking through the body.  If no pattern matches, raises Fail.
// Exec frame on match: [save_obj] [@unify-match-barrier]

// throws: execstack-overflow, fail, limit-check, opstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void unify_match_op(Trix *trx) {
    trx->require_op_count(2);

    auto pairs_obj = *trx->m_op_ptr;
    if (!pairs_obj.is_array()) {
        trx->error(Error::TypeCheck, "unify-match: pairs must be an array");
    } else {
        auto [pairs, pairs_len] = pairs_obj.array_value(trx);

        if (pairs_len == 0) {
            trx->m_op_ptr -= 2;
            trx->error(Error::Fail, "unify-match: empty pairs array");
        } else {
            // Validate structure: each element must be a 2-element array [pattern body]
            for (length_t i = 0; i < pairs_len; ++i) {
                if (!pairs[i].is_array()) {
                    trx->error(Error::TypeCheck, "unify-match: each pair must be a [pattern body] array");
                } else if (pairs[i].arrays_length() != 2) {
                    trx->error(Error::RangeCheck, "unify-match: each pair must have exactly 2 elements");
                } else {
                    auto pair_elems = pairs[i].array_objects(trx);
                    if (pair_elems[1].is_literal() || !pair_elems[1].is_callable()) {
                        trx->error(Error::TypeCheck, "unify-match: body must be executable");
                    }
                }
            }

            // Keep value_obj (m_op_ptr-1) and pairs_obj (m_op_ptr) rooted across the
            // whole try-loop: unify_impl can clone an ExtValue via gvm_alloc in ${...}
            // (-> vm_global_gc), and pairs[i]/pattern are raw pointers into pairs_obj's
            // payload.  Save::save pushes its token ABOVE both; pop the pair only at the
            // two loop exits (match / all-fail).
            auto value_obj = *(trx->m_op_ptr - 1);

            // Try each pattern in order: save, unify, match or restore.
            // value_obj came from the op stack and is consumable; pattern lives in
            // the pairs array and is NOT consumable (sub-binds against the pattern
            // take the deep-clone path).  value_obj can only have its ExtValue
            // transferred when the pattern is itself an unbound lvar at top level
            // ([[x { body }]] shape) -- in that case the bind succeeds and we exit
            // the loop, so partial-bind-then-restore on value_obj is unreachable.
            // For all other pattern shapes value_obj is never transferred and must
            // be freed by the explicit paths below (match -> match-success body,
            // all-fail -> error).
            for (length_t i = 0; i < pairs_len; ++i) {
                auto pair_elems = pairs[i].array_objects(trx);
                auto pattern = pair_elems[0];
                auto body = pair_elems[1];

                // Save for this attempt
                trx->require_save_capacity(1);
                Save::save(trx);
                auto save_obj = *trx->m_op_ptr--;

                auto [matched, value_transferred, pattern_transferred] = unify_impl(trx, value_obj, pattern, true, false, 0);
                // pattern is non-consumable; pattern_transferred is always false.
                assert(!pattern_transferred);
                static_cast<void>(pattern_transferred);
                if (matched) {
                    // Match found -- free value_obj if not transferred, pop the two
                    // rooted operands, then push barrier frame and body.
                    if (!value_transferred) {
                        value_obj.maybe_free_extvalue(trx);
                    }
                    trx->m_op_ptr -= 2;
                    trx->require_exec_capacity(3);
                    *++trx->m_exec_ptr = save_obj;
                    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atUnifyMatchBarrier);
                    *++trx->m_exec_ptr = body;
                    return;
                } else {
                    // Unification failed -- restore and try next pair.  Because
                    // value_obj was never transferred on a partial-bind path
                    // (recursive sub-binds use deep-clone), value_obj's m_offset
                    // still references its original ExtValue after restore.  The
                    // restore reclaims any deep-cloned ExtValues from the failed
                    // bind attempt (they were allocated above the save barrier).
                    trx->require_op_capacity(1);
                    *++trx->m_op_ptr = save_obj;
                    Save::restore_token(trx, static_cast<vm_offset_t>(save_obj.integer_value()));
                }
            }

            // All patterns exhausted -- pop the two rooted operands, then free
            // value_obj's ExtValue (solely owned now) before propagating Fail.
            trx->m_op_ptr -= 2;
            value_obj.maybe_free_extvalue(trx);
            trx->error(Error::Fail, "unify-match: no matching pattern");
        }
    }
}

// @unify-match-barrier: body succeeded -- pop save companion (save level stays active).
static void at_unify_match_barrier_op(Trix *trx) {
    --trx->m_exec_ptr;  // pop save_obj companion
}

// @unify-match-fail: body failed -- restore save, re-raise error.
static void at_unify_match_fail_op(Trix *trx) {
    if (trx->m_last_error != Error::Fail) {
        --trx->m_exec_ptr;
        trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
    } else {
        auto save_obj = *trx->m_exec_ptr--;
        auto token = static_cast<vm_offset_t>(save_obj.integer_value());
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = save_obj;
        Save::restore_token(trx, token);
        trx->error(Error::Fail, "unify-match: body raised fail");
    }
}

//===--- Find-N Operator ---===//
//
// find-n: n array -- array
// Like find-all but stops after collecting n results.  Short-circuits:
// alternatives after the Nth success are never evaluated.
// Exec frame: [alt_array] [index_int] [remaining_int]
// Per attempt: [save_obj] [@find-n-barrier]

// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void find_n_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyInteger);

    auto alt_obj = *trx->m_op_ptr--;
    auto n_obj = *trx->m_op_ptr--;
    auto n = n_obj.integer_value();
    if (n < 0) {
        trx->error(Error::RangeCheck, "find-n: count must be non-negative");
    } else {
        auto [alts, alt_len] = alt_obj.array_value(trx);

        if ((n == 0) || (alt_len == 0)) {
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = Object::make_empty_array(trx, 0);
        } else {
            // Running coroutine's scratch arena must be idle (no nesting)
            if (trx->m_scratch_ptr != trx->m_scratch_base) {
                trx->error(Error::LimitCheck, "find-n: nested find-all/find-n not supported");
            } else {
                // Push exec frame: [alt_array, index=0, remaining=n]
                trx->require_exec_capacity(3);
                *++trx->m_exec_ptr = alt_obj;
                *++trx->m_exec_ptr = Object::make_integer(0);
                *++trx->m_exec_ptr = Object::make_integer(n);

                find_n_try_next(trx);
            }
        }
    }
}

// Collect results from running coroutine's scratch into an array.
static void find_n_collect(Trix *trx) {
    trx->m_exec_ptr -= 3;  // pop alt_array, index, remaining
    trx->require_op_capacity(1);
    if (trx->m_scratch_ptr == trx->m_scratch_base) {
        *++trx->m_op_ptr = Object::make_empty_array(trx, 0);
    } else {
        *++trx->m_op_ptr = trx->template scratch_collect<Object>();
    }
}

static void find_n_try_next(Trix *trx) {
    auto remaining = trx->m_exec_ptr[0].integer_value();
    auto index = static_cast<length_t>(trx->m_exec_ptr[-1].integer_value());
    auto alt_obj = trx->m_exec_ptr[-2];
    auto [alts, alt_len] = alt_obj.array_value(trx);

    if ((remaining <= 0) || (index >= alt_len)) {
        find_n_collect(trx);
    } else {
        // Save for this attempt
        trx->require_save_capacity(1);
        Save::save(trx);
        auto save_obj = *trx->m_op_ptr--;

        trx->require_exec_capacity(3);
        *++trx->m_exec_ptr = save_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atFindNBarrier);
        *++trx->m_exec_ptr = alts[index];
    }
}

// @find-n-barrier: alternative succeeded -- validate result, restore, clone into
// persistent region, decrement remaining.
static void at_find_n_barrier_op(Trix *trx) {
    // Exec stack: ... | alt_array | index | remaining | save_obj
    trx->require_op_count(1);

    // IMPORTANT: do NOT pop the result before Save::restore -- see comment in
    // at_find_all_barrier_op.  ExtValue::restore must see the result on the
    // op stack to relocate its heap-resident 64-bit value.
    auto result_ptr = trx->m_op_ptr;

    if (!result_ptr->is_persistable_now(trx)) {
        --trx->m_op_ptr;
        trx->m_exec_ptr -= 4;
        trx->error(Error::TypeCheck,
                   "find-n: result type {} cannot survive restore; use deref to extract scalars, "
                   "or wrap the result construction in ${{...}} to allocate it in global VM",
                   Object::type_sv(result_ptr->type()));
    }

    // Restore save point.  Result on op stack is relocated by ExtValue::restore.
    auto save_obj = *trx->m_exec_ptr--;
    auto token = static_cast<vm_offset_t>(save_obj.integer_value());
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = save_obj;
    Save::restore_token(trx, token);

    // NOW safe to read the result -- ExtValue relocated below the barrier.  Clone
    // it WHILE STILL ON THE OP STACK (mirrors at_find_all_barrier_op): make_clone
    // allocates globally under ${...}, and the vm-gc-stress GC mid-clone would
    // sweep the result if it had been popped to a bare C local first -- the clone
    // would then read a freed (poisoned) cell.  Clone from the rooted slot, pop.
    auto clone = trx->m_op_ptr->make_clone(trx);
    --trx->m_op_ptr;
    trx->template scratch_push<Object>(clone);

    // Decrement remaining, advance index
    auto remaining_ptr = trx->m_exec_ptr;
    *remaining_ptr = Object::make_integer(remaining_ptr->integer_value() - 1);
    auto index_ptr = (trx->m_exec_ptr - 1);
    *index_ptr = Object::make_integer(index_ptr->integer_value() + 1);

    find_n_try_next(trx);
}

// @find-n-fail: alternative failed -- restore and try next.
static void at_find_n_fail_op(Trix *trx) {
    if (trx->m_last_error != Error::Fail) {
        trx->m_exec_ptr -= 4;
        trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
    } else {
        // Save::restore clamps m_vm_temp_ptr to m_vm_global, which suffices
        // since scratch lives in the coroutine's stack block.
        auto save_obj = *trx->m_exec_ptr--;
        auto token = static_cast<vm_offset_t>(save_obj.integer_value());
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = save_obj;
        Save::restore_token(trx, token);

        auto index_ptr = trx->m_exec_ptr - 1;
        *index_ptr = Object::make_integer(index_ptr->integer_value() + 1);

        find_n_try_next(trx);
    }
}

//===--- Choice-Count Operator ---===//
//
// choice-count: array -- int
// Counts how many alternatives succeed without collecting results.
// Like find-all but only accumulates a counter, not the result array.
// Exec frame: [alt_array] [index_int] [count_int]
// Per attempt: [save_obj] [@choice-count-barrier]

// choice-count: array -- int
// throws: opstack-underflow, type-check
static void choice_count_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto alt_obj = *trx->m_op_ptr--;
    auto [alts, alt_len] = alt_obj.array_value(trx);

    if (alt_len == 0) {
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_integer(0);
    } else {
        // Push exec frame: [alt_array, index=0, count=0]
        trx->require_exec_capacity(3);
        *++trx->m_exec_ptr = alt_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = Object::make_integer(0);

        choice_count_try_next(trx);
    }
}

static void choice_count_try_next(Trix *trx) {
    auto count = trx->m_exec_ptr[0].integer_value();
    auto index = static_cast<length_t>(trx->m_exec_ptr[-1].integer_value());
    auto alt_obj = trx->m_exec_ptr[-2];
    auto [alts, alt_len] = alt_obj.array_value(trx);

    if (index >= alt_len) {
        // All alternatives tried -- push count
        trx->m_exec_ptr -= 3;
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_integer(count);
    } else {
        // Record op stack depth before alternative runs (for cleanup in barrier/fail)
        auto op_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);

        // Save for this attempt
        trx->require_save_capacity(1);
        Save::save(trx);
        auto save_obj = *trx->m_op_ptr--;

        trx->require_exec_capacity(4);
        *++trx->m_exec_ptr = Object::make_integer(op_depth);
        *++trx->m_exec_ptr = save_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atChoiceCountBarrier);
        *++trx->m_exec_ptr = alts[index];
    }
}

// @choice-count-barrier: alternative succeeded -- discard result, increment counter,
// restore save, advance index.
static void at_choice_count_barrier_op(Trix *trx) {
    // Exec stack: ... | alt_array | index | count | op_depth | save_obj
    auto save_obj = *trx->m_exec_ptr--;
    auto op_depth = trx->m_exec_ptr->integer_value();
    --trx->m_exec_ptr;

    // Discard any results the alternative left on the op stack.
    // Reset to saved depth -- restore will reclaim any ExtValues above the barrier.
    trx->m_op_ptr = trx->m_op_base + op_depth;

    auto token = static_cast<vm_offset_t>(save_obj.integer_value());
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = save_obj;
    Save::restore_token(trx, token);

    // Increment count, advance index
    auto count_ptr = trx->m_exec_ptr;
    *count_ptr = Object::make_integer(count_ptr->integer_value() + 1);
    auto index_ptr = (trx->m_exec_ptr - 1);
    *index_ptr = Object::make_integer(index_ptr->integer_value() + 1);

    choice_count_try_next(trx);
}

// @choice-count-fail: alternative failed -- restore save, advance (skip).
static void at_choice_count_fail_op(Trix *trx) {
    if (trx->m_last_error != Error::Fail) {
        trx->m_exec_ptr -= 5;
        trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
    } else {
        auto save_obj = *trx->m_exec_ptr--;
        auto op_depth = trx->m_exec_ptr->integer_value();
        --trx->m_exec_ptr;

        // Discard any partial results the failed alternative left on the op stack
        trx->m_op_ptr = trx->m_op_base + op_depth;

        auto token = static_cast<vm_offset_t>(save_obj.integer_value());
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = save_obj;
        Save::restore_token(trx, token);

        auto index_ptr = trx->m_exec_ptr - 1;
        *index_ptr = Object::make_integer(index_ptr->integer_value() + 1);

        choice_count_try_next(trx);
    }
}

//===--- For-Each-Solution Operator ---===//
//
// for-each-solution: body-proc alternatives-array --
// For each alternative that succeeds, executes body-proc with the
// alternative's result on the stack.  Bindings are rolled back after
// each body execution (same save/restore cycle as find-all).
// Primarily useful for I/O side effects during search.
// Exec frame: [body_proc] [alt_array] [index_int]
// Per attempt: [save_obj] [@for-each-solution-barrier] [body_proc] [alt_proc]

// for-each-solution: body alternatives --
// throws: opstack-underflow, type-check
static void for_each_solution_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyProc | VerifyCurry | VerifyContinuation);

    auto alt_obj = *trx->m_op_ptr--;
    auto body_obj = *trx->m_op_ptr--;
    auto [alts, alt_len] = alt_obj.array_value(trx);

    if (alt_len == 0) {
        // No alternatives -- nothing to do
    } else {
        // Push exec frame: [body_proc, alt_array, index=0]
        trx->require_exec_capacity(3);
        *++trx->m_exec_ptr = body_obj;
        *++trx->m_exec_ptr = alt_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);

        for_each_solution_try_next(trx);
    }
}

static void for_each_solution_try_next(Trix *trx) {
    auto index = static_cast<length_t>(trx->m_exec_ptr[0].integer_value());
    auto alt_obj = trx->m_exec_ptr[-1];
    auto body_obj = trx->m_exec_ptr[-2];
    auto [alts, alt_len] = alt_obj.array_value(trx);

    if (index >= alt_len) {
        // All alternatives tried -- done
        trx->m_exec_ptr -= 3;
    } else {
        // Record op stack depth before alternative+body runs (for cleanup in barrier/fail)
        auto op_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);

        // Save for this attempt
        trx->require_save_capacity(1);
        Save::save(trx);
        auto save_obj = *trx->m_op_ptr--;

        // Push: [op_depth] [save_obj] [@for-each-solution-barrier] [body_proc] [alt_proc]
        trx->require_exec_capacity(5);
        *++trx->m_exec_ptr = Object::make_integer(op_depth);
        *++trx->m_exec_ptr = save_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atForEachSolutionBarrier);
        *++trx->m_exec_ptr = body_obj;
        *++trx->m_exec_ptr = alts[index];
    }
}

// @for-each-solution-barrier: alt succeeded, body ran -- restore save, advance.
// Exec stack: ... | body_proc | alt_array | index | op_depth | save_obj
static void at_for_each_solution_barrier_op(Trix *trx) {
    auto save_obj = *trx->m_exec_ptr--;
    auto op_depth = trx->m_exec_ptr->integer_value();
    --trx->m_exec_ptr;

    // Discard any results the body left on the op stack.
    // Reset to saved depth -- restore will reclaim any ExtValues above the barrier.
    trx->m_op_ptr = trx->m_op_base + op_depth;

    auto token = static_cast<vm_offset_t>(save_obj.integer_value());
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = save_obj;
    Save::restore_token(trx, token);

    // Advance index
    auto index_ptr = trx->m_exec_ptr;
    *index_ptr = Object::make_integer(index_ptr->integer_value() + 1);

    for_each_solution_try_next(trx);
}

// @for-each-solution-fail: alt or body failed -- restore save, advance or re-raise.
static void at_for_each_solution_fail_op(Trix *trx) {
    if (trx->m_last_error != Error::Fail) {
        trx->m_exec_ptr -= 5;
        trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
    } else {
        auto save_obj = *trx->m_exec_ptr--;
        auto op_depth = trx->m_exec_ptr->integer_value();
        --trx->m_exec_ptr;

        // Discard any partial results from failed alternative/body
        trx->m_op_ptr = trx->m_op_base + op_depth;

        auto token = static_cast<vm_offset_t>(save_obj.integer_value());
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = save_obj;
        Save::restore_token(trx, token);

        // Advance index
        auto index_ptr = trx->m_exec_ptr;
        *index_ptr = Object::make_integer(index_ptr->integer_value() + 1);

        for_each_solution_try_next(trx);
    }
}

//===--- Aggregate Operator ---===//
//
// aggregate: init reducer [alts] -- result
// Folds solutions on-the-fly with a reducer procedure.  For each alternative
// that succeeds, calls the reducer with (accumulator result) on the operand
// stack; the reducer must produce a new accumulator.  The accumulator must be
// persistable (inline scalar, ExtValue, or Name) since it survives
// save/restore cycles.  The alternative's result has no such constraint --
// it is consumed by the reducer before save/restore.
//
// Two-phase trampoline:
//   Phase 1: run alternative under @aggregate-barrier
//   Phase 2: run reducer under @aggregate-reduce-barrier
//
// Exec frame: [acc] [reducer] [alt_array] [index_int]
// Per attempt (phase 1): [op_depth] [save_obj] [@aggregate-barrier] [alt_proc]
// Per attempt (phase 2): [op_depth] [save_obj] [@aggregate-reduce-barrier] [reducer]

// aggregate: init reducer [alts] -- result
// throws: opstack-underflow, type-check, limit-check
static void aggregate_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyProc | VerifyCurry | VerifyContinuation, VerifyAny);

    auto alt_obj = *trx->m_op_ptr--;
    auto reducer_obj = *trx->m_op_ptr--;
    auto init_obj = *trx->m_op_ptr;
    if (!init_obj.is_persistable_now(trx)) {
        trx->error(Error::TypeCheck,
                   "aggregate: initial accumulator type {} must be persistable "
                   "(scalar, or composite allocated in global VM via ${{...}})",
                   Object::type_sv(init_obj.type()));
    }
    --trx->m_op_ptr;

    auto [alts, alt_len] = alt_obj.array_value(trx);

    if (alt_len == 0) {
        // No alternatives -- return init unchanged
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = init_obj;
    } else {
        // Push exec frame: [acc, reducer, alt_array, index=0]
        trx->require_exec_capacity(4);
        *++trx->m_exec_ptr = init_obj;
        *++trx->m_exec_ptr = reducer_obj;
        *++trx->m_exec_ptr = alt_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);

        aggregate_try_next(trx);
    }
}

static void aggregate_try_next(Trix *trx) {
    auto index = static_cast<length_t>(trx->m_exec_ptr->integer_value());
    auto alt_obj = trx->m_exec_ptr[-1];
    auto [alts, alt_len] = alt_obj.array_value(trx);

    if (index >= alt_len) {
        // All alternatives tried -- return accumulator
        // Transfer ownership from exec frame to op stack (no clone needed)
        auto acc_obj = trx->m_exec_ptr[-3];
        trx->m_exec_ptr -= 4;
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = acc_obj;
    } else {
        auto op_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);

        trx->require_save_capacity(1);
        Save::save(trx);
        auto save_obj = *trx->m_op_ptr--;

        trx->require_exec_capacity(4);
        *++trx->m_exec_ptr = Object::make_integer(op_depth);
        *++trx->m_exec_ptr = save_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atAggregateBarrier);
        *++trx->m_exec_ptr = alts[index];
    }
}

// @aggregate-barrier: alt succeeded -- schedule reducer with (acc result) on stack.
// Exec stack: ... | acc | reducer | alt_array | index | op_depth | save_obj
static void at_aggregate_barrier_op(Trix *trx) {
    auto save_obj = *trx->m_exec_ptr--;
    auto op_depth_val = trx->m_exec_ptr->integer_value();
    --trx->m_exec_ptr;
    // Exec: ... | acc | reducer | alt_array | index

    trx->require_op_count(1);
    auto result_obj = *trx->m_op_ptr;

    // Read frame slots
    auto reducer_obj = trx->m_exec_ptr[-2];
    auto acc_obj = trx->m_exec_ptr[-3];

    // Clone acc NOW, while result_obj is still rooted at *m_op_ptr.  In a ${...}
    // aggregate the acc clone (ExtValue/WideValue) routes through gvm_alloc and
    // can fire vm_global_gc; doing it after the op-stack reset (when result_obj
    // sits above m_op_ptr, unrooted) would let GC sweep a global composite result.
    // This is the sole allocating call here, so result_obj stays GC-reachable.
    auto acc_clone = acc_obj.make_clone(trx);

    // Reset op stack, push acc and result for reducer
    auto target = trx->m_op_base + op_depth_val;
    --trx->m_op_ptr;
    while (trx->m_op_ptr > target) {
        trx->m_op_ptr->maybe_free_extvalue(trx);
        --trx->m_op_ptr;
    }

    trx->require_op_capacity(2);
    *++trx->m_op_ptr = acc_clone;
    *++trx->m_op_ptr = result_obj;

    // Schedule phase 2: [op_depth, save_obj, @aggregate-reduce-barrier, reducer]
    trx->require_exec_capacity(4);
    *++trx->m_exec_ptr = Object::make_integer(op_depth_val);
    *++trx->m_exec_ptr = save_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atAggregateReduceBarrier);
    *++trx->m_exec_ptr = reducer_obj;
}

// @aggregate-fail: alt failed -- restore, advance or re-raise.
// Exec stack: ... | acc | reducer | alt_array | index | op_depth | save_obj
static void at_aggregate_fail_op(Trix *trx) {
    if (trx->m_last_error != Error::Fail) {
        trx->m_exec_ptr -= 2;
        trx->m_exec_ptr[-3].maybe_free_extvalue(trx);
        trx->m_exec_ptr -= 4;
        trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
    } else {
        auto save_obj = *trx->m_exec_ptr--;
        auto op_depth_val = trx->m_exec_ptr->integer_value();
        --trx->m_exec_ptr;
        // Exec: ... | acc | reducer | alt_array | index

        // Reset op stack
        auto target = trx->m_op_base + op_depth_val;
        while (trx->m_op_ptr > target) {
            trx->m_op_ptr->maybe_free_extvalue(trx);
            --trx->m_op_ptr;
        }

        auto token = static_cast<vm_offset_t>(save_obj.integer_value());
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = save_obj;
        Save::restore_token(trx, token);

        // Advance index
        auto index_ptr = trx->m_exec_ptr;
        *index_ptr = Object::make_integer(index_ptr->integer_value() + 1);

        aggregate_try_next(trx);
    }
}

// @aggregate-reduce-barrier: reducer succeeded -- capture new acc, restore, advance.
// Exec stack: ... | acc | reducer | alt_array | index | op_depth | save_obj
static void at_aggregate_reduce_barrier_op(Trix *trx) {
    auto save_obj = *trx->m_exec_ptr--;
    auto op_depth_val = trx->m_exec_ptr->integer_value();
    --trx->m_exec_ptr;
    // Exec: ... | acc | reducer | alt_array | index

    trx->require_op_count(1);
    auto new_acc_obj = *trx->m_op_ptr;

    if (!new_acc_obj.is_persistable_now(trx)) {
        auto target = trx->m_op_base + op_depth_val;
        while (trx->m_op_ptr > target) {
            trx->m_op_ptr->maybe_free_extvalue(trx);
            --trx->m_op_ptr;
        }
        trx->m_exec_ptr[-3].maybe_free_extvalue(trx);
        trx->m_exec_ptr -= 4;
        trx->error(Error::TypeCheck,
                   "aggregate: reducer result type {} must be persistable "
                   "(scalar, or composite allocated in global VM via ${{...}})",
                   Object::type_sv(new_acc_obj.type()));
    }

    // Discard everything except new_acc
    --trx->m_op_ptr;
    auto target = trx->m_op_base + op_depth_val;
    while (trx->m_op_ptr > target) {
        trx->m_op_ptr->maybe_free_extvalue(trx);
        --trx->m_op_ptr;
    }

    // Push new_acc and save_obj for restore (new_acc must be on stack for ExtValue relocation)
    trx->require_op_capacity(2);
    *++trx->m_op_ptr = new_acc_obj;
    *++trx->m_op_ptr = save_obj;
    Save::restore_token(trx, static_cast<vm_offset_t>(save_obj.integer_value()));

    // Clone the relocated new_acc into the frame's acc slot.  Clone WHILE it is still
    // rooted at *m_op_ptr: make_clone's ExtValue/WideValue alloc routes through gvm_alloc
    // and can fire vm_global_gc under ${...}; popping first would leave new_acc above
    // m_op_ptr (unrooted) so the GC would sweep its source and the clone would read freed
    // memory (the accumulator silently collapses -- aggregate returned the init unchanged).
    auto clone = trx->m_op_ptr->make_clone(trx);
    --trx->m_op_ptr;
    trx->m_exec_ptr[-3].maybe_free_extvalue(trx);
    trx->m_exec_ptr[-3] = clone;

    // Advance index
    auto index_ptr = trx->m_exec_ptr;
    *index_ptr = Object::make_integer(index_ptr->integer_value() + 1);

    aggregate_try_next(trx);
}

// @aggregate-reduce-fail: reducer failed -- restore, clean up, re-raise.
// Exec stack: ... | acc | reducer | alt_array | index | op_depth | save_obj
static void at_aggregate_reduce_fail_op(Trix *trx) {
    auto save_obj = *trx->m_exec_ptr--;
    auto op_depth_val = trx->m_exec_ptr->integer_value();
    --trx->m_exec_ptr;
    // Exec: ... | acc | reducer | alt_array | index

    auto target = trx->m_op_base + op_depth_val;
    while (trx->m_op_ptr > target) {
        trx->m_op_ptr->maybe_free_extvalue(trx);
        --trx->m_op_ptr;
    }

    auto token = static_cast<vm_offset_t>(save_obj.integer_value());
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = save_obj;
    Save::restore_token(trx, token);

    // Clean up frame and re-raise
    trx->m_exec_ptr[-3].maybe_free_extvalue(trx);
    trx->m_exec_ptr -= 4;
    trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
}
