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

//===--- Stack Manipulation Operators ---===//
//
// Implements operand stack manipulation and container copying.  Based on:
//
//   PostScript stack operators: pop, dup, exch, roll, index, clear, count,
//   copy, mark, count-to-mark.  These are the fundamental data-movement
//   primitives in a stack-based language -- all computation depends on
//   arranging operands correctly on the stack.
//
//   Forth: rot, over, nip, tuck.
//   Trix includes both PostScript and Forth names where conventions differ.
//
// --- Core concepts for maintainers ---
//
// STACK MODEL
//   The operand stack is a contiguous array of Objects (m_op_base to
//   m_op_limit).  m_op_ptr points to the current top.  Stack growth is
//   upward (push increments m_op_ptr, pop decrements).  The stack is
//   bounds-checked: push checks capacity (require_op_capacity), pop
//   checks count (require_op_count).
//
// EXTVALUE AWARENESS
//   Stack operations that discard Objects (pop, clear, roll) must call
//   maybe_free_extvalue() on discarded values to avoid ExtValue leaks.
//   Operations that duplicate Objects (dup, copy, index) must call
//   make_clone() to create independent ExtValue copies.
//
// COPY SEMANTICS
//   `copy` is polymorphic over read/write containers (it does NOT accept an
//   integer -- the PostScript-style N-element stack copy is the separate
//   `dup-n` operator):
//   - Array/Packed to Array: element-wise clone into destination
//   - Dict to Dict: copy all entries (key/value cloned)
//   - String to String: byte copy into destination
//   - Set to Set: element-wise clone into destination
//
// MARK SYSTEM
//   `mark` pushes a sentinel Object (Type::Mark) onto the stack.
//   `count-to-mark` counts elements above the nearest mark.
//   Many operators use marks for variadic arguments: `[ 1 2 3 ]` pushes
//   a mark, three values, then `]` collects them into an array.
//
// --- Operators ---
//
//   pop          any --                Discard top
//   pop-n        any1 ... anyN n --    Discard top N+1
//   dup          any -- any any        Duplicate top
//   dup-n        anyN ... any1 n -- anyN ... any1 anyN ... any1  Duplicate top N
//   exch         a b -- b a            Swap top two
//   roll         any_n ... any_0 n j --  Rotate N elements by J
//   index        any_n ... any_0 n -- any_n ... any_0 any_n  Pick by index
//   clear        any* --               Discard all
//   count        -- int                Stack depth
//   copy         src dst -- dst        Copy container contents (array/string/dict/set)
//   mark         -- mark               Push mark sentinel
//   count-to-mark  mark any* -- mark any* int  Count above mark
//   clear-to-mark  mark any* --         Discard down to and including nearest mark
//   rot          a b c -- b c a        Rotate top three
//   rev-rot      a b c -- c a b        Reverse rot
//   over         a b -- a b a          Copy second element
//   nip          a b -- b              Discard second element
//   tuck         a b -- b a b          Copy top below second
//   dip          any proc -- any       Execute proc with top temporarily removed
//   bi           x q1 q2 -- r1 r2      Apply two quotations to one value
//   keep         any proc -- any       Run proc, restore a clone of any
//

// pop: any :- --
// Discards the top operand.
// throws: opstack-underflow
static void pop_op(Trix *trx) {
    trx->require_op_count(1);

    auto any_obj = *trx->m_op_ptr--;
    any_obj.maybe_free_extvalue(trx);
}

// pop-n: any1 ... anyN int :- --
// Discards the top N+1 operands.
// throws: opstack-underflow, range-check, type-check
static void popn_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative);

    auto n_ptr = trx->m_op_ptr;
    auto op_count = (trx->op_count() - 1);
    auto [valid, n] = n_ptr->integer_value(trx, 0, op_count);
    if (valid) {
        auto bottom_ptr = (n_ptr - n);
        maybe_free_extvalue_opstack(trx, bottom_ptr);
        trx->m_op_ptr = (bottom_ptr - 1);
    } else {
        trx->error(Error::RangeCheck, "operand N {} is not in the range of 0..{}", n, op_count);
    }
}

// exch: any1 any2 :- any2 any1
// Exchanges the top two operands.
// throws: opstack-underflow
static void exch_op(Trix *trx) {
    trx->require_op_count(2);

    auto any2_ptr = trx->m_op_ptr;
    auto any1_ptr = (any2_ptr - 1);
    auto tmp_obj = *any2_ptr;
    *any2_ptr = *any1_ptr;
    *any1_ptr = tmp_obj;
}

// over: any1 any2 :- any1 any2 any1
// Copies the second operand to the top (shallow clone).
// throws: vm-full, opstack-overflow, opstack-underflow
static void over_op(Trix *trx) {
    trx->require_op_count(2);
    trx->require_op_capacity(1);

    auto any1_ptr = (trx->m_op_ptr - 1);
    *++trx->m_op_ptr = any1_ptr->make_clone(trx);
}

// nip: any1 any2 :- any2
// Drops the second element.
// throws: opstack-underflow
static void nip_op(Trix *trx) {
    trx->require_op_count(2);

    auto top_ptr = trx->m_op_ptr;
    auto second_ptr = (top_ptr - 1);
    second_ptr->maybe_free_extvalue(trx);
    *second_ptr = *top_ptr;
    --trx->m_op_ptr;
}

// tuck: any1 any2 :- any2 any1 any2
// Copies the top element under the second (shallow clone).
// throws: vm-full, opstack-overflow, opstack-underflow
static void tuck_op(Trix *trx) {
    trx->require_op_count(2);
    trx->require_op_capacity(1);

    auto top_ptr = trx->m_op_ptr;
    auto second_ptr = (top_ptr - 1);
    auto clone_obj = top_ptr->make_clone(trx);
    *++trx->m_op_ptr = *top_ptr;
    *top_ptr = *second_ptr;
    *second_ptr = clone_obj;
}

// rot: any1 any2 any3 :- any2 any3 any1
// Rotates the third element to the top.
// throws: opstack-underflow
static void rot_op(Trix *trx) {
    trx->require_op_count(3);

    auto c_ptr = trx->m_op_ptr;
    auto b_ptr = (c_ptr - 1);
    auto a_ptr = (b_ptr - 1);
    auto tmp_obj = *a_ptr;
    *a_ptr = *b_ptr;
    *b_ptr = *c_ptr;
    *c_ptr = tmp_obj;
}

// rev-rot: any1 any2 any3 :- any3 any1 any2
// Rotates the top element down to third position (reverse rot).
// throws: opstack-underflow
static void revrot_op(Trix *trx) {
    trx->require_op_count(3);

    auto c_ptr = trx->m_op_ptr;
    auto b_ptr = (c_ptr - 1);
    auto a_ptr = (b_ptr - 1);
    auto tmp_obj = *c_ptr;
    *c_ptr = *b_ptr;
    *b_ptr = *a_ptr;
    *a_ptr = tmp_obj;
}

// @dip: (exec stack: hidden-value)
// Restores hidden value from exec stack to operand stack.
// throws: opstack-overflow
static void at_dip_op(Trix *trx) {
    trx->require_op_capacity(1);

    *++trx->m_op_ptr = *trx->m_exec_ptr--;
}

// dip: any proc :- any
// Hides any, executes proc, then restores any.
// throws: execstack-overflow, opstack-underflow, type-check
static void dip_op(Trix *trx) {
    trx->verify_operands(VerifyProc);
    trx->require_op_count(2);
    trx->require_exec_capacity(3);

    auto proc_obj = *trx->m_op_ptr--;
    auto hidden_obj = *trx->m_op_ptr--;
    *++trx->m_exec_ptr = hidden_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDip);
    *++trx->m_exec_ptr = proc_obj;
}

// bi: x q1 q2 :- r1 r2
// Applies two quotations to the same value.
// Clones x, runs q1 on clone producing r1, then runs q2 on original x producing r2.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void bi_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyProc);
    trx->require_op_count(3);
    trx->require_exec_capacity(5);

    auto q2_obj = *trx->m_op_ptr--;
    auto q1_obj = *trx->m_op_ptr--;
    auto x_ptr = trx->m_op_ptr;

    // clone x for q1; original x stays for q2
    auto cloned_obj = x_ptr->make_clone(trx);

    // exec stack (bottom to top, first pushed = runs last):
    //   q2                  -- runs on original x (after @dip restores it)
    //   original-x, @dip   -- hides r1, pushes original x for q2
    //   q1                  -- runs on cloned x (runs first)
    *++trx->m_exec_ptr = q2_obj;
    *++trx->m_exec_ptr = *x_ptr;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDip);
    *++trx->m_exec_ptr = q1_obj;

    // replace x on opstack with clone (q1 consumes this)
    *x_ptr = cloned_obj;
}

// keep: any proc :- any
// Executes proc with any on stack, then restores a clone of any.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void keep_op(Trix *trx) {
    trx->verify_operands(VerifyProc);
    trx->require_op_count(2);
    trx->require_exec_capacity(3);

    auto proc_obj = *trx->m_op_ptr--;
    auto any_ptr = trx->m_op_ptr;
    auto saved_obj = any_ptr->make_clone(trx);
    *++trx->m_exec_ptr = saved_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDip);
    *++trx->m_exec_ptr = proc_obj;
}

// dup: any :- any any
// Pushes a shallow clone of the top operand.
// throws: vm-full, opstack-overflow, opstack-underflow
static void dup_op(Trix *trx) {
    trx->require_op_count(1);
    trx->require_op_capacity(1);

    auto any_ptr = trx->m_op_ptr;
    auto dup_obj = any_ptr->make_clone(trx);
    *++trx->m_op_ptr = dup_obj;
}

// dup-n: any1 ... anyN int :- any1 ... anyN any1 ... anyN
// Duplicates the top N operands.
// throws: vm-full, opstack-overflow, opstack-underflow, range-check, type-check
static void dupn_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative);

    auto n_ptr = trx->m_op_ptr;
    auto op_count = (trx->op_count() - 1);
    auto [valid, n] = n_ptr->integer_value(trx, 0, op_count);
    if (valid) {
        n_ptr->maybe_free_extvalue(trx);
        if (n != 0) {
            trx->require_op_count(n + 1);
            // n-1: the integer operand's slot is reused for the first clone
            trx->require_op_capacity(n - 1);

            auto dst_ptr = n_ptr;
            auto src_ptr = (n_ptr - n);
            std::transform(src_ptr, (src_ptr + n), dst_ptr, [trx](Object o) { return o.make_clone(trx); });
        }
        trx->m_op_ptr = (n_ptr + (n - 1));
    } else {
        trx->error(Error::RangeCheck, "operand N {} is not in the range of 0..{}", n, op_count);
    }
}

static void copy_string(Trix *trx, const Object *src, Object *dst) {
    auto src_ptr = src->string_vptr(trx);
    auto src_length = src->string_length();
    auto dst_ptr = dst->string_vptr(trx);
    auto dst_length = dst->string_length();
    if (dst_length >= src_length) {
        if (dst_ptr != src_ptr) {
            std::copy_n(src_ptr, src_length, dst_ptr);
        }
        dst->set_string_length(trx, src_length);
    } else {
        trx->error(Error::RangeCheck, "destination 'string' length {} less than source 'string' length {}", dst_length, src_length);
    }
}

static void copy_set(Trix *trx, const Object *src, Object *dst) {
    auto src_set = src->set_value(trx);
    auto dst_set = dst->set_value(trx);
    if (dst_set != src_set) {
        auto dst_capacity = dst_set->capacity();
        auto src_length = src_set->length();
        if ((dst_capacity >= src_length) || dst_set->is_dynamic()) {
            dst_set->copy_set(trx, src_set);
        } else {
            trx->error(
                    Error::RangeCheck, "destination 'set' capacity {} less than source 'set' length {}", dst_capacity, src_length);
        }
    }
}

static void copy_dict(Trix *trx, const Object *src, Object *dst) {
    auto src_dict = src->dict_value(trx);
    auto dst_dict = dst->dict_value(trx);
    if (dst_dict != src_dict) {
        auto dst_capacity = dst_dict->capacity();
        auto src_length = src_dict->length();
        if ((dst_capacity >= src_length) || dst_dict->is_dynamic()) {
            dst_dict->copy_dict(trx, src_dict);
        } else {
            trx->error(Error::RangeCheck,
                       "destination 'dict' capacity {} less than source 'dict' length {}",
                       dst_capacity,
                       src_length);
        }
    }
}

// copy: src dst :- dst
// Copies elements from src into dst (array, string, or dict).
// throws: vm-full, dict-full, opstack-underflow, range-check, type-check
static void copy_op(Trix *trx) {
    trx->verify_operands(VerifyRWArray | VerifyRWString | VerifyRWDict | VerifyRWSet);

    auto dst_ptr = trx->m_op_ptr;
    auto src_ptr = (dst_ptr - 1);
    if (dst_ptr->is_array()) {
        trx->verify_operands(VerifyRWArray, VerifyArrays);

        copy_array(trx, src_ptr, dst_ptr);
    } else if (dst_ptr->is_string()) {
        trx->verify_operands(VerifyRWString, VerifyString);

        copy_string(trx, src_ptr, dst_ptr);
    } else if (dst_ptr->is_set()) {
        trx->verify_operands(VerifyRWSet, VerifySet);

        copy_set(trx, src_ptr, dst_ptr);
    } else {
        trx->verify_operands(VerifyRWDict, VerifyDict);

        copy_dict(trx, src_ptr, dst_ptr);
    }

    *src_ptr = *dst_ptr;
    trx->m_op_ptr = src_ptr;
}

// index: anyN ... any0 int :- anyN ... any0 anyN
// Duplicates the Nth operand (shallow clone).
// throws: vm-full, index-check, opstack-underflow, type-check
static void index_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative);

    auto n_ptr = trx->m_op_ptr;
    auto op_count = (trx->op_count() - 1);
    auto n_max = (op_count - 1);
    auto [valid, n] = n_ptr->integer_value(trx, 0, n_max);
    if (valid) {
        trx->require_op_count(n + 2);
        n_ptr->maybe_free_extvalue(trx);

        auto src_ptr = n_ptr - (n + 1);
        *n_ptr = src_ptr->make_clone(trx);
    } else {
        trx->error(Error::IndexCheck, "operand N {} is not in the range of 0..{}", n, n_max);
    }
}

// roll: any(n-1) ... any0 n j :- --
// Circular shift of the top n operands by amount j.
// throws: opstack-underflow, range-check, type-check
static void roll_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers, VerifyIntegers | VerifyNotNegative);

    auto j_ptr = trx->m_op_ptr;
    auto [j_valid, j] = j_ptr->integer_value(trx);
    if (!j_valid) {
        trx->error(Error::RangeCheck, "operand J {} is out of range", j);
    } else {
        auto n_ptr = (j_ptr - 1);
        auto op_count = (trx->op_count() - 2);
        auto [n_valid, n] = n_ptr->integer_value(trx, 0, op_count);
        if (!n_valid) {
            trx->error(Error::RangeCheck, "operand N {} is not in the range of 0..{}", n, op_count);
        } else {
            trx->require_op_count(n + 2);

            j_ptr->maybe_free_extvalue(trx);
            n_ptr->maybe_free_extvalue(trx);

            auto top_ptr = (n_ptr - 1);
            if (n == 0) {
                trx->m_op_ptr = top_ptr;
            } else {
                if (j != 0) {
                    auto is_downward = (j < 0);
                    // Cast to long_t before negating to avoid UB when j == INT32_MIN
                    j = is_downward ? static_cast<integer_t>(-(static_cast<long_t>(j)) % n) : (j % n);

                    auto first_ptr = (top_ptr - (n - 1));
                    auto last_ptr = (top_ptr + 1);
                    if (is_downward) {
                        std::rotate(first_ptr, first_ptr + j, last_ptr);
                    } else {
                        std::rotate(first_ptr, last_ptr - j, last_ptr);
                    }
                }
                trx->m_op_ptr = top_ptr;
            }
        }
    }
}

// clear: any1 ... anyN :- --
// Discards all operands.
// throws: (none)
static void clear_op(Trix *trx) {
    maybe_free_extvalue_opstack(trx, trx->m_op_base);
    trx->m_op_ptr = (trx->m_op_base - 1);
}

// count: :- int
// Pushes the number of operands on the stack.
// throws: opstack-overflow
static void count_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto count_ptr = ++trx->m_op_ptr;
    auto count = (count_ptr - trx->m_op_base);
    *count_ptr = Object::make_integer(static_cast<integer_t>(count));
}

// mark: :- mark
// Pushes a mark object.
// throws: opstack-overflow
static void mark_op(Trix *trx) {
    trx->require_op_capacity(1);

    *++trx->m_op_ptr = Object::make_mark();
}

// clear-to-mark: mark any1 ... anyN :- --
// Discards operands down to and including the nearest mark.
// throws: unmatched-mark
static void clear_to_mark_op(Trix *trx) {
    auto [mark_ptr, _] = trx->find_opstack_mark();
    maybe_free_extvalue_opstack(trx, mark_ptr);
    trx->m_op_ptr = (mark_ptr - 1);
}

// count-to-mark: mark any1 ... anyN :- mark any1 ... anyN int
// Counts operands above the nearest mark.
// throws: opstack-overflow, unmatched-mark
static void count_to_mark_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto [_, count] = trx->find_opstack_mark();
    *++trx->m_op_ptr = Object::make_integer(count);
}

//
// Array Operators
//

// array-store: any0 ... anyN arr :- arr
// Pops N elements from the stack into the array.
// throws: opstack-underflow, type-check
static void array_store_op(Trix *trx) {
    trx->verify_operands(VerifyRWArray);

    auto arr_obj = *trx->m_op_ptr;
    auto [dst_ptr, array_length] = arr_obj.array_value(trx);
    trx->require_op_count(array_length + 1);

    auto src_ptr = (trx->m_op_ptr - array_length);
    auto curr_save_level = trx->m_curr_save_level;
    auto dst_is_eqarray = arr_obj.is_eqarray(trx);

    // R6 pointer hygiene: a global array clones each fragile-local SCALAR element
    // into global and rejects a NON-scalar fragile-local (see Dict::put).  Elements
    // are on the op stack -- reject first (no free), then clone + write each clone
    // back to its slot, so it stays GC-rooted (the next element's clone-alloc can
    // fire a GC) and the store loop below reads the cloned values.
    auto dst_is_global = trx->is_global(arr_obj.offset());
    if (dst_is_global) {
        auto check_ptr = src_ptr;
        for (length_t i = 0; i < array_length; ++i) {
            Save::reject_local_into_global(trx, true, *check_ptr, "array-store");
            *check_ptr = Save::clone_fragile_scalar_into_global(trx, true, *check_ptr);
            ++check_ptr;
        }
    }

    for (length_t i = 0; i < array_length; ++i) {
        if ((dst_ptr->save_level() == curr_save_level) || dst_is_eqarray) {
            dst_ptr->maybe_free_extvalue(trx);
        } else {
            Save::save_object(trx, dst_ptr);
        }
        *dst_ptr = src_ptr->make_copy(curr_save_level);

        ++src_ptr;
        ++dst_ptr;
    }

    trx->m_op_ptr -= array_length;
    *trx->m_op_ptr = arr_obj;
}

// array-store-persist: any0 ... anyN arr :- arr
// Like array-store, but the element writes are NOT journaled -- they
// persist across the enclosing save/restore.  At sl=0 silently degrades
// to array-store.  At sl > 0, every source element is checked at write
// time: any element above the save barrier raises /above-barrier (the
// post-restore array would hold a dangling offset).  Pre-checks the
// entire batch before mutating any slot, so the array is unchanged on
// rejection.
// throws: above-barrier, opstack-underflow, type-check
static void array_store_persist_op(Trix *trx) {
    trx->verify_operands(VerifyRWArray);

    auto arr_obj = *trx->m_op_ptr;
    auto [dst_ptr, array_length] = arr_obj.array_value(trx);
    trx->require_op_count(array_length + 1);

    auto src_ptr = (trx->m_op_ptr - array_length);
    auto save_active = Save::is_active(trx);

    if (save_active) {
        // Pre-check the entire batch -- the array must be unmodified if any
        // element fails the ref-check.
        auto check_ptr = src_ptr;
        for (length_t i = 0; i < array_length; ++i) {
            if (Save::is_above_barrier(trx, *check_ptr)) {
                trx->error(Error::AboveBarrier,
                           "array-store-persist: element {} lives above the save barrier and would dangle on restore",
                           i);
            }
            ++check_ptr;
        }
    }

    // R6 pointer hygiene: a global array clones each fragile-local SCALAR element
    // into global and rejects a NON-scalar fragile-local (see Dict::put / array-store)
    // -- reject first (no free), then clone + write each clone back to its op-stack
    // slot so it stays GC-rooted and the store loop below reads the cloned values.
    auto dst_is_global = trx->is_global(arr_obj.offset());
    if (dst_is_global) {
        auto check_ptr = src_ptr;
        for (length_t i = 0; i < array_length; ++i) {
            Save::reject_local_into_global(trx, true, *check_ptr, "array-store-persist");
            *check_ptr = Save::clone_fragile_scalar_into_global(trx, true, *check_ptr);
            ++check_ptr;
        }
    }

    auto dst_is_eqarray = arr_obj.is_eqarray(trx);
    for (length_t i = 0; i < array_length; ++i) {
        if (dst_is_eqarray) {
            Save::reject_stale_value_into_eqref(trx, *src_ptr, "array-store-persist");
        }
        // No Save::save_object call: the overwrite must persist across the
        // enclosing save/restore.  Free the previous ExtValue (if any) since
        // the journal will not preserve it.  Stamp BASE on the new value so
        // a later put() at sl=0 doesn't see a stale save_level above current
        // and try to journal at a vanished level (raising /invalid-restore
        // from save_data because no save is active) -- see Dict::put_persist
        // for the same fix.
        dst_ptr->maybe_free_extvalue(trx);
        *dst_ptr = src_ptr->make_copy(Save::BASE);

        ++src_ptr;
        ++dst_ptr;
    }

    trx->m_op_ptr -= array_length;
    *trx->m_op_ptr = arr_obj;
}

// array-load: arr :- any0 ... anyN arr
// Pushes all array elements onto the stack.
// throws: vm-full, opstack-overflow, opstack-underflow, type-check
static void array_load_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);

    auto arr_obj = *trx->m_op_ptr;
    auto length = arr_obj.arrays_length();
    trx->require_op_capacity(length);

    auto dst_ptr = trx->m_op_ptr;
    if (arr_obj.is_array()) {
        auto src_ptr = arr_obj.array_objects(trx);
        std::transform(src_ptr, (src_ptr + length), dst_ptr, [trx](Object o) { return o.make_clone(trx); });
    } else {
        auto packed_data = arr_obj.const_packed_span(trx);

        Object::extract_packed(trx, packed_data.data(), dst_ptr, length, Object::ExtractPackedDestination::Stack);
    }

    trx->m_op_ptr += length;
    *trx->m_op_ptr = arr_obj;
}
