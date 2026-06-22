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

//===--- Pattern Matching Operators ---===//
//
// Three layers of destructuring and dispatch:
//
//   Layer A -- let: bind stack values to names in a new dict scope
//   Layer B -- destructure: extract fields/elements into a new dict scope
//   Layer C -- match/cond: multi-arm dispatch with test/body pairs
//
//   let          val* name-array --          Bind N stack values to names
//   destructure  value name-array --         Extract fields/elements by name
//   match        value pairs-array -- result Multi-arm dispatch (value passed)
//   match-all    value pairs-array -- arr    Gather form: collect all matching arms
//   when         value test body -- result   Single-arm match (sugar)
//   cond         pairs-array -- result       General cond (no value)
//
// match, when, and cond use the @match-test trampoline (when is sugar over
// match).  match-all uses its own two-phase @match-all-test trampoline.
//
// @match-test exec-stack layout (after interpreter pops @match-test):
//   m_exec_ptr[0]  = pairs-array (body first, then remaining test/body pairs)
//   m_exec_ptr[-1] = value (the original value being matched; Mark for cond)
//
// When value is Mark (@match-test from cond), the body does NOT receive a
// dup'd value on the operand stack; it operates on whatever is already there.
//
// Test proc contract: each test proc receives a dup'd copy of the value on
// the operand stack.  The test proc MUST consume that copy and push a bool.
// Type predicates (is-integer, etc.) satisfy this naturally.  For a "match
// anything" default arm, use { pop true } (not { true }).

// Shared validation for name arrays used by let and destructure.
static void match_verify_name_array(Trix *trx, const Object *names_ptr, length_t count, const char *op_name) {
    if (count == 0) {
        trx->error(Error::RangeCheck, "{}: name array must not be empty", op_name);
    } else {
        for (length_t i = 0; i < count; ++i) {
            if (!names_ptr[i].is_name()) {
                trx->error(Error::TypeCheck, "{}: name array must contain only names", op_name);
            }
        }
    }
}

// let: val1 val2 ... valN [/name1 /name2 ... /nameN] let
// Creates a new dict, pushes on dict stack, binds each name to the
// corresponding stack value.  name1 = val1 (deepest), nameN = valN (top).
// User calls `end` to pop the dict scope.
// throws: dict-full, dictstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void let_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto names_obj = *trx->m_op_ptr;
    auto [names_ptr, names_length] = names_obj.array_value(trx);

    match_verify_name_array(trx, names_ptr, names_length, "let");
    trx->require_op_count(names_length + 1);
    trx->require_dict_capacity(1);

    auto [dict, dict_offset] = Dict::create_or_recycle(trx, names_length);

    // Keep the names array rooted at m_op_ptr across the bind loop -- its
    // backing storage is read through names_ptr, and put()/make_clone() can
    // allocate (and fire a GC).  The N value slots sit just below the array
    // (name1=val1 deepest .. nameN=valN just below), so bind by index off
    // their base, then drop the values + names array together.  Mirrors
    // destructure_op and stays GC-safe if Name ever gains heap-cell backing.
    auto values_base = (trx->m_op_ptr - names_length);
    for (length_t i = 0; i < names_length; ++i) {
        static_cast<void>(dict->put(trx, names_ptr[i].make_clone(trx), values_base[i], Dict::BindingMode::Bind));
    }
    trx->m_op_ptr -= (names_length + 1);

    *++trx->m_dict_ptr = Object::make_dict(dict_offset);
    dict->set_name_bindings(trx);
}

// destructure: value [/field1 /field2 ...] destructure
// Extracts fields from a structured value into a new dict scope.
// Array: by index.  Record: by field name.  Dict: by key name.
// Value is consumed.  User calls `end` to pop the dict scope.
// throws: dict-full, dictstack-overflow, opstack-underflow, range-check, type-check, undefined, vm-full
static void destructure_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyAny);

    auto names_obj = *trx->m_op_ptr;
    auto value_obj = *(trx->m_op_ptr - 1);
    auto [names_ptr, names_length] = names_obj.array_value(trx);

    match_verify_name_array(trx, names_ptr, names_length, "destructure");
    trx->require_dict_capacity(1);

    // Validate type-specific constraints before allocating the dict
    if (value_obj.is_array()) {
        auto arr_length = value_obj.arrays_length();
        if (names_length > arr_length) {
            trx->error(Error::RangeCheck, "destructure: name count ({}) exceeds array length ({})", names_length, arr_length);
        }
    } else if (!value_obj.is_record() && !value_obj.is_dict()) {
        trx->error(Error::TypeCheck, "destructure: expected array, record, or dict");
    }

    auto [dict, dict_offset] = Dict::create_or_recycle(trx, names_length);

    if (value_obj.is_array()) {
        auto [elem_ptr, _] = value_obj.array_value(trx);
        for (length_t i = 0; i < names_length; ++i) {
            static_cast<void>(dict->put(trx, names_ptr[i].make_clone(trx), elem_ptr[i].make_clone(trx), Dict::BindingMode::Bind));
        }
    } else if (value_obj.is_record()) {
        auto instance = value_obj.record_instance(trx);
        auto schema = trx->offset_to_ptr<RecordSchema>(instance->m_schema);
        auto field_count = schema->m_field_count;

        for (length_t i = 0; i < names_length; ++i) {
            auto idx = record_find_field(trx, schema, field_count, names_ptr[i]);
            if (idx == field_count) {
                trx->error(Error::Undefined, "destructure: field /{} not found in record", names_ptr[i].name_sv(trx));
            } else {
                static_cast<void>(dict->put(
                        trx, names_ptr[i].make_clone(trx), instance->m_fields[idx].make_clone(trx), Dict::BindingMode::Bind));
            }
        }
    } else if (value_obj.is_dict()) {
        auto src_dict = value_obj.dict_value(trx);
        for (length_t i = 0; i < names_length; ++i) {
            auto val_ptr = src_dict->get(trx, names_ptr[i]);
            if (val_ptr == nullptr) {
                trx->error(Error::Undefined, "destructure: key /{} not found in dict", names_ptr[i].name_sv(trx));
            } else {
                static_cast<void>(dict->put(trx, names_ptr[i].make_clone(trx), val_ptr->make_clone(trx), Dict::BindingMode::Bind));
            }
        }
    }

    value_obj.maybe_free_extvalue(trx);
    trx->m_op_ptr -= 2;

    *++trx->m_dict_ptr = Object::make_dict(dict_offset);
    dict->set_name_bindings(trx);
}

// match_start_trampoline: shared setup for match and cond.
// Pops operands, sets up the @match-test exec frame, dup's value for first test.
static void match_start_trampoline(Trix *trx, Object value_obj, Object pairs_obj, stack_depth_t op_pop_count) {
    auto pairs_length = pairs_obj.arrays_length();

    if (pairs_length == 0) {
        trx->error(Error::RangeCheck, "match: pairs array must not be empty");
    } else if ((pairs_length & 1) != 0) {
        trx->error(Error::RangeCheck, "match: pairs array must have even length (test/body pairs)");
    } else {
        auto first_test_obj = pairs_obj.arrays_pop_clone_head(trx);

        trx->require_exec_capacity(4);
        *++trx->m_exec_ptr = value_obj;
        *++trx->m_exec_ptr = pairs_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atMatchTest);
        *++trx->m_exec_ptr = first_test_obj;

        trx->m_op_ptr -= op_pop_count;

        // For match (value is non-mark): dup value for the test proc
        if (!value_obj.is_mark()) {
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = value_obj.make_clone(trx);
        }
    }
}

// match: value [{test}{body} {test}{body} ...] match
// Multi-arm pattern dispatch.  Pairs array has even length (test/body pairs).
// For each pair: dup value, execute test-proc (returns bool).
// If true: execute body-proc with value on stack.
// If false: try next pair.  No match: Error::Match.
// throws: execstack-overflow, opstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void match_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyAny);

    auto pairs_obj = *trx->m_op_ptr;
    auto value_obj = *(trx->m_op_ptr - 1);

    auto clone_obj = value_obj.make_clone(trx);
    (trx->m_op_ptr - 1)->maybe_free_extvalue(trx);

    match_start_trampoline(trx, clone_obj, pairs_obj, 2);
}

// @match-test: control op for match/cond trampoline.
// Exec frame (after popping @match-test):
//   m_exec_ptr[0]  = pairs (body first, then remaining)
//   m_exec_ptr[-1] = value (Mark sentinel for cond)
// Operand stack: bool from test-proc on top.
static void at_match_test_op(Trix *trx) {
    auto pairs_ptr = trx->m_exec_ptr;
    auto value_ptr = (pairs_ptr - 1);

    trx->require_op_count(1);
    if (!trx->m_op_ptr->is_boolean()) {
        // Free the value clone's ExtValue before discarding the frame, matching
        // the no-match error path below (a no-op for cond's Mark value).
        value_ptr->maybe_free_extvalue(trx);
        trx->m_exec_ptr -= 2;
        trx->error(Error::TypeCheck, "match: test proc must return a boolean");
    } else {
        auto result = trx->m_op_ptr->boolean_value();
        --trx->m_op_ptr;

        if (result) {
            // Match: extract body, execute with value on stack (if not cond mode)
            auto body_obj = pairs_ptr->arrays_pop_clone_head(trx);

            if (!value_ptr->is_mark()) {
                trx->require_op_capacity(1);
                *++trx->m_op_ptr = *value_ptr;
            }

            trx->m_exec_ptr -= 2;

            trx->require_exec_capacity(1);
            *++trx->m_exec_ptr = body_obj;
        } else {
            // Skip the body for this pair
            auto body_obj = pairs_ptr->arrays_pop_clone_head(trx);
            body_obj.maybe_free_extvalue(trx);

            if (pairs_ptr->arrays_length() == 0) {
                value_ptr->maybe_free_extvalue(trx);
                trx->m_exec_ptr -= 2;
                trx->error(Error::Match, "match: no matching pattern");
            } else {
                // Set up next test
                auto next_test_obj = pairs_ptr->arrays_pop_clone_head(trx);

                // Dup value for next test (if not cond mode)
                if (!value_ptr->is_mark()) {
                    trx->require_op_capacity(1);
                    *++trx->m_op_ptr = value_ptr->make_clone(trx);
                }

                trx->require_exec_capacity(2);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atMatchTest);
                *++trx->m_exec_ptr = next_test_obj;
            }
        }
    }
}

// match-all: value [{test}{body} ...] match-all -- results-array
// Gather form of match: runs EVERY test against (a clone of) value, in
// pair order; for each test returning true the body runs with a fresh
// value clone on the stack, and whatever the bodies push accumulates --
// collect-everything semantics, so a body may contribute zero, one, or
// several results.  After the last pair the accumulated values become a
// single array (region-aware via make_array_from_mark).  Zero matching
// arms raises /match.
//
// Mechanism: mark-accumulation.  match-all plants a Mark on the operand
// stack; body results pile above it between trampoline bounces, so the
// operand stack itself roots every partial result (no GC-rooting window,
// and W04 try-rollback discards mark + partials on a caught error).
// Bodies must keep mark balance: a body that leaves its own unbalanced
// Mark shifts the collection boundary (the nearest mark wins, exactly as
// with any [ ... ] construction in progress).
//
// Exec frame (bottom to top): [value] [pairs] [state] [@match-all-test]
// with the test or body proc pushed above per bounce.  state is an
// Integer companion holding two bits: "any arm matched yet" and "this
// bounce resumes after a body" (the two-phase trampoline below).  The
// 3-companion layout is baked into the control op as popcount 3
// (make_control_operator), which `exit` unwinding and exec-validate use
// to step over the frame.
static constexpr integer_t MatchAllMatchedBit{1};
static constexpr integer_t MatchAllPostBodyBit{2};

// throws: execstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void match_all_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyAny);

    auto pairs_obj = *trx->m_op_ptr;
    auto value_obj = *(trx->m_op_ptr - 1);

    auto pairs_length = pairs_obj.arrays_length();
    if (pairs_length == 0) {
        trx->error(Error::RangeCheck, "match-all: pairs array must not be empty");
    } else if ((pairs_length & 1) != 0) {
        trx->error(Error::RangeCheck, "match-all: pairs array must have even length (test/body pairs)");
    } else {
        // Frame-owned value clone; the original's ExtValue is released with
        // its operand slot.  Clone BEFORE freeing (construct-before-free).
        auto frame_value = value_obj.make_clone(trx);
        (trx->m_op_ptr - 1)->maybe_free_extvalue(trx);

        auto first_test_obj = pairs_obj.arrays_pop_clone_head(trx);

        // Push the exec frame first so frame_value is rooted before the
        // test-dup clone below can allocate.
        trx->require_exec_capacity(5);
        *++trx->m_exec_ptr = frame_value;
        *++trx->m_exec_ptr = pairs_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atMatchAllTest);
        *++trx->m_exec_ptr = first_test_obj;

        // Replace the two operands with the accumulation Mark and the first
        // test's value dup.
        *(trx->m_op_ptr - 1) = Object::make_mark();
        *trx->m_op_ptr = frame_value.make_clone(trx);
    }
}

// @match-all-test: two-phase control op for the match-all trampoline.
// Exec frame (after popping @match-all-test):
//   m_exec_ptr[0]  = state (Integer: matched / post-body bits)
//   m_exec_ptr[-1] = pairs (remaining test/body pairs)
//   m_exec_ptr[-2] = value (frame-owned clone)
//
// Test phase (post-body bit clear): operand top is the test proc's bool.
//   true  -> set matched bit + post-body bit, push value clone for the
//            body, re-arm [@match-all-test] beneath the body so the
//            post-body phase runs after it.
//   false -> discard the body, advance to the next test or finalize.
// Post-body phase (bit set): no bool expected -- the body's results are
//   already accumulating above the Mark.  Clear the bit and advance to
//   the next test or finalize.
// Finalize: zero matches -> /match; otherwise collect everything above
//   the Mark into the result array.
static void at_match_all_test_op(Trix *trx) {
    auto state_ptr = trx->m_exec_ptr;
    auto pairs_ptr = (state_ptr - 1);
    auto value_ptr = (state_ptr - 2);

    auto state = state_ptr->integer_value();

    if ((state & MatchAllPostBodyBit) == 0) {
        // Test phase: consume the test proc's boolean.
        trx->require_op_count(1);
        if (!trx->m_op_ptr->is_boolean()) {
            value_ptr->maybe_free_extvalue(trx);
            trx->m_exec_ptr -= 3;
            // Sweep the accumulation Mark and partial results so the error
            // leaves the operand stack as the caller had it.
            auto [mark_ptr, residue_length] = trx->find_opstack_mark(nullptr);
            static_cast<void>(residue_length);
            Object::cleanup_opstack_to_mark(trx, mark_ptr);
            trx->error(Error::TypeCheck, "match-all: test proc must return a boolean");
        } else {
            auto result = trx->m_op_ptr->boolean_value();
            --trx->m_op_ptr;

            if (result) {
                // Match: run the body with a fresh value clone; re-arm this op
                // beneath it so the post-body phase advances the trampoline.
                auto body_obj = pairs_ptr->arrays_pop_clone_head(trx);
                state_ptr->update_integer(state | MatchAllMatchedBit | MatchAllPostBodyBit);

                trx->require_exec_capacity(2);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atMatchAllTest);
                *++trx->m_exec_ptr = body_obj;

                trx->require_op_capacity(1);
                *++trx->m_op_ptr = value_ptr->make_clone(trx);
                return;
            } else {
                // No match for this arm: discard the body.
                auto body_obj = pairs_ptr->arrays_pop_clone_head(trx);
                body_obj.maybe_free_extvalue(trx);
            }
        }
    } else {
        // Post-body phase: the body's results are on the operand stack.
        state_ptr->update_integer(state & ~MatchAllPostBodyBit);
    }

    if (pairs_ptr->arrays_length() != 0) {
        // Arm the next test with a fresh value dup.
        auto next_test_obj = pairs_ptr->arrays_pop_clone_head(trx);

        trx->require_exec_capacity(2);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atMatchAllTest);
        *++trx->m_exec_ptr = next_test_obj;

        trx->require_op_capacity(1);
        *++trx->m_op_ptr = value_ptr->make_clone(trx);
    } else {
        // Finalize.
        auto matched = ((state_ptr->integer_value() & MatchAllMatchedBit) != 0);
        value_ptr->maybe_free_extvalue(trx);
        trx->m_exec_ptr -= 3;

        if (!matched) {
            // Sweep the accumulation Mark (and any residue a misbehaving test
            // left above it) before raising, so the no-match error leaves the
            // operand stack as the caller had it.
            auto [mark_ptr, residue_length] = trx->find_opstack_mark(nullptr);
            static_cast<void>(residue_length);
            Object::cleanup_opstack_to_mark(trx, mark_ptr);
            trx->error(Error::Match, "match-all: no matching pattern");
        } else {
            auto result_obj = Object::make_array_from_mark(trx);
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = result_obj;
        }
    }
}

// when: value {test} {body} when
// Single-arm conditional match.  Dup's value, runs test.  If true: executes
// body with value on stack.  If false: value remains on stack unchanged.
// Implemented as: match with [test body {true} {}] -- default arm preserves value.
// throws: execstack-overflow, opstack-overflow, opstack-underflow, type-check, vm-full
static void when_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyProc, VerifyAny);

    auto body_obj = *trx->m_op_ptr;
    auto test_obj = *(trx->m_op_ptr - 1);
    auto value_obj = *(trx->m_op_ptr - 2);

    // Build a 4-element pairs array: [test, body, always-true-proc, nop-proc]
    // The always-true proc pushes true (discarding the dup'd value).
    // The nop proc is empty -- value passes through unchanged.
    auto [true_body_ptr, true_body_offset] = trx->vm_alloc_n<Object>(2);
    true_body_ptr[0] = Object::make_operator(SystemName::Pop);
    true_body_ptr[1] = Object::make_boolean(true);
    auto true_proc_obj = Object::make_array(true_body_offset, 2, Object::ExecutableAttrib, Object::ReadOnlyAccess);

    auto [_, nop_offset] = trx->vm_alloc_n<Object>(0);
    auto nop_proc_obj = Object::make_array(nop_offset, 0, Object::ExecutableAttrib, Object::ReadOnlyAccess);

    auto [elem_ptr, array_offset] = trx->vm_alloc_n<Object>(4);
    elem_ptr[0] = test_obj;
    elem_ptr[1] = body_obj;
    elem_ptr[2] = true_proc_obj;
    elem_ptr[3] = nop_proc_obj;

    auto pairs_obj = Object::make_array(array_offset, 4, Object::LiteralAttrib, Object::ReadOnlyAccess);

    auto clone_obj = value_obj.make_clone(trx);
    (trx->m_op_ptr - 2)->maybe_free_extvalue(trx);
    trx->m_op_ptr -= 3;

    match_start_trampoline(trx, clone_obj, pairs_obj, 0);
}

// cond: [{test}{body} {test}{body} ...] cond
// General conditional dispatch (no value argument).  Test procs operate on
// the current stack and must push a bool.  First true test wins; its body
// executes.  No match: Error::Match.
// throws: execstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void cond_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto pairs_obj = *trx->m_op_ptr;
    match_start_trampoline(trx, Object::make_mark(), pairs_obj, 1);
}
