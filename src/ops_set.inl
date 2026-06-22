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

//===--- Set Operators ---===//
//
// Implements mathematical set operations on unordered collections of
// unique elements.  Based on:
//
//   Mathematical set theory: union, intersection, difference, symmetric
//   difference, subset, disjoint tests.  The standard algebraic operations
//   on finite sets.
//
//   Python frozenset / Java HashSet / C++ std::unordered_set: hash-based
//   set implementations with O(1) average membership test and O(n) set
//   algebra operations.
//
// --- Core concepts for maintainers ---
//
// SET REPRESENTATION
//   Sets share the Dict class infrastructure (dict.inl) with the SetFlag
//   bit set in the access byte.  A Set uses SetEntry (12 bytes: m_next +
//   key) instead of DictEntry (20 bytes: m_next + key + value).  The hash
//   table structure (buckets, chains, pool) is identical.
//
//   This shared representation means sets get dict's bucket hashing,
//   save/restore journaling, and pool-based allocation for free.  The
//   set-specific methods in Dict (set_put, set_remove, find_set_entry,
//   etc.) operate on SetEntry instead of DictEntry.
//
// HETEROGENEOUS ELEMENTS
//   Sets can hold any hashable Trix Object: integers, reals, strings,
//   names, booleans, etc.  Elements of different types can coexist in
//   the same set.  Equality uses Object::equal() (same as dict key
//   comparison).
//
// IMMUTABLE OPERATIONS
//   Set algebra operations (union, intersection, difference, symmetric
//   difference) create new sets -- they do not modify the input sets.
//   Only set-add, set-add-persist, set-remove, and set-remove-persist
//   mutate in place (on read-write sets).
//
// SET CREATION
//   set             -- create an empty set with a given integer capacity
//   set-from-array  -- create a set from an array
//   =set-from-mark  -- create an eq-set (temporary =set storage) from mark-delimited elements
//   Duplicates are silently ignored during creation.
//
// ITERATION
//   for-all iterates over all elements (order is hash-dependent and
//   not guaranteed).  set-filter creates a new set from elements matching
//   a predicate.  members converts to an array.
//
// --- Operators ---
//
//   set                     int -- set          Create empty set of given capacity
//   set-from-array          array -- set        Create from array
//   set-from-mark           mark any* -- set    Create from mark-delimited elements
//   readonly-set-from-mark  mark any* -- set    Create read-only set from mark
//   =set-from-mark          mark any* -- set    Create eq-set from mark
//   readonly-=set-from-mark mark any* -- set    Create read-only eq-set from mark
//   set-add                 set key -- set      Add element (mutating)
//   set-add-persist         set key -- set      Add element, not journaled
//   set-remove              set key -- set      Remove element (mutating)
//   set-remove-persist      set key -- set      Remove element, not journaled
//   set-member?             set key -- bool     Membership test
//   set-union               set set -- set      Union (new set)
//   set-intersection        set set -- set      Intersection (new set)
//   set-difference          set set -- set      Difference (new set)
//   symmetric-difference    set set -- set      Symmetric difference (new set)
//   subset?                 set set -- bool     Subset test
//   disjoint?               set set -- bool     Disjoint test
//   members                 set -- array        Convert to array
//   set-filter              set proc -- set     Filter by predicate (new set)
//   for-all                 set proc --         Iterate elements (generic)
//   is-set                  any -- bool         Type test
//
// Control operators (internal, not user-visible):
//   @set-filter    Filter iteration step (test predicate, collect matches)
//   @set-for-all   Forall iteration step
//

// @set-filter callback
// exec stack frame: [proc] [result-set] [src-set] [entry-offset] [bucket-idx] [saved-key] [@set-filter]
// Collects elements where proc returns true.
// throws: execstack-overflow, opstack-overflow, vm-full
static void at_set_filter_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);

    // collect the boolean result from the proc
    auto keep = trx->m_op_ptr->boolean_value();

    auto key_ptr = trx->m_exec_ptr;
    auto bucket_ptr = (key_ptr - 1);
    auto offset_ptr = (bucket_ptr - 1);
    auto src_set_ptr = (offset_ptr - 1);
    auto result_set_ptr = (src_set_ptr - 1);
    auto proc_ptr = (result_set_ptr - 1);

    if (keep) {
        auto result_set = result_set_ptr->set_value(trx);
        result_set->set_put(trx, key_ptr->make_clone(trx));
    }
    // NOTE: key_ptr (the saved-key exec slot) holds a BORROWED alias of the source
    // set's key -- set_next returns entry->m_key as-is, not a clone (see the two
    // writes to this slot, both from set_next).  The source set owns that key and
    // frees it when the set is reclaimed, so this iterator must NOT free it.  An
    // earlier maybe_free_extvalue here freed the source set's own ExtValue/WideValue
    // key (Long/Double/Int128/...), corrupting the source mid-filter -- a UAF the
    // GC's freed-block check trips under ${...}.  Name/immediate keys masked it
    // (maybe_free_extvalue is a no-op for them).  @set-for-all has the same borrow
    // model and correctly never frees the key.

    // advance to next entry
    auto entry_offset = vm_offset_t{offset_ptr->uinteger_value()};
    auto bucket_idx = bucket_ptr->integer_value();
    auto src_set = src_set_ptr->set_value(trx);
    auto [next_offset, next_idx, key] = src_set->set_next(trx, entry_offset, bucket_idx);

    if (next_offset == nulloffset) {
        // done: push result set to operand stack
        *trx->m_op_ptr = *result_set_ptr;
        trx->m_exec_ptr -= 6;
    } else {
        trx->require_exec_capacity(2);

        offset_ptr->update_uinteger(static_cast<uinteger_t>(next_offset));
        bucket_ptr->update_integer(next_idx);
        *key_ptr = key;

        // push key clone to operand stack for proc
        *trx->m_op_ptr = key.make_clone(trx);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atSetFilter);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// set-filter: set proc :- set'
// Returns a new set containing only elements where proc returns true.
// proc receives: key -- bool
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void set_filter_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifySet);

    auto proc = trx->m_op_ptr;
    auto set_ptr = (proc - 1);
    auto src_set = set_ptr->set_value(trx);
    auto count = src_set->length();

    if (count == 0) {
        // empty set: return empty dynamic set (region-aware -- survives restore under ${...})
        auto [_, offset] = create_result_set(trx, 0, Object::DictMode::ReadWriteDynamic);
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_set(offset);
    } else {
        trx->require_exec_capacity(8);

        // create result set.  Region-aware so it survives save/restore under ${...};
        // it is parked on the exec stack (GC-scanned) below, and pre-sized to the
        // source length so set_put never expands, so no extra rooting is needed.
        auto [result_set, result_offset] = create_result_set(trx, count, Object::DictMode::ReadWriteDynamic);

        // get first entry
        auto [first_offset, first_idx, first_key] = src_set->set_next(trx, nulloffset, -1);

        // exec stack frame: [proc] [result-set] [src-set] [entry-offset] [bucket-idx] [saved-key] [@set-filter] [proc]
        auto saved_proc = *proc;
        *++trx->m_exec_ptr = saved_proc;
        *++trx->m_exec_ptr = Object::make_set(result_offset);
        *++trx->m_exec_ptr = *set_ptr;
        *++trx->m_exec_ptr = Object::make_uinteger(static_cast<uinteger_t>(first_offset));
        *++trx->m_exec_ptr = Object::make_integer(first_idx);
        *++trx->m_exec_ptr = first_key;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atSetFilter);
        *++trx->m_exec_ptr = saved_proc;

        // push first key clone to operand stack for proc
        --trx->m_op_ptr;
        *trx->m_op_ptr = first_key.make_clone(trx);
    }
}

//===--- Set Operators ---===//

// set: int -- set
// Creates an empty set with the given capacity.  Routes to global VM
// when m_curr_alloc_global is set (e.g. ${ N set }).
// throws: opstack-underflow, type-check, range-check, vm-full
static void set_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative);

    auto top = trx->m_op_ptr;
    auto [valid, maxlength] = top->integer_value(trx, 0, MaxSetLength);
    if (valid) {
        top->maybe_free_extvalue(trx);
        auto [_, offset] =
                trx->m_curr_alloc_global
                        ? Dict::create_global_set(trx, static_cast<length_t>(maxlength), Object::DictMode::ReadWriteDynamic)
                        : Dict::create_set(trx, static_cast<length_t>(maxlength), Object::DictMode::ReadWriteDynamic);
        *top = Object::make_set(offset);
    } else {
        trx->error(Error::LimitCheck, "set capacity {} exceeds maximum {}", maxlength, MaxSetLength);
    }
}

// set-add: set key -- set
// Adds a key to the set.  No-op if already present.
// Key may be any hashable type (same as dict key); Null is rejected.
// throws: opstack-underflow, type-check, read-only, dict-full, vm-full
static void set_add_op(Trix *trx) {
    trx->verify_operands(VerifyKey, VerifyRWSet);

    auto key_ptr = trx->m_op_ptr;
    auto set_ptr = (key_ptr - 1);
    auto set = set_ptr->set_value(trx);
    set->set_put(trx, *key_ptr);
    --trx->m_op_ptr;
}

// set-add-persist: set key -- set
// Like set-add, but the membership change is NOT journaled -- it
// persists across the enclosing save/restore.  At sl=0 silently
// degrades to set-add.  At sl > 0:
//   - above-barrier key raises /above-barrier
//   - missing keys are ADDED (new SetEntry allocated in global
//     VM so the membership survives restore).  Already-a-member is a
//     silent no-op (matching set-add).
// throws: above-barrier, opstack-underflow, type-check, read-only, vm-full
static void set_add_persist_op(Trix *trx) {
    trx->verify_operands(VerifyKey, VerifyRWSet);

    auto key_ptr = trx->m_op_ptr;
    auto set_ptr = (key_ptr - 1);
    auto key = *key_ptr;
    auto set = set_ptr->set_value(trx);

    if (Save::is_active(trx)) {
        if (Save::is_above_barrier(trx, key)) {
            trx->error(Error::AboveBarrier, "set-add-persist: key lives above the save barrier and would dangle on restore");
        } else {
            set->set_add_persist_or_create(trx, key);
        }
    } else {
        // sl=0: silent degrade to set-add.
        set->set_put(trx, key);
    }
    --trx->m_op_ptr;
}

// set-remove: set key -- set
// Removes a key from the set.
// throws: opstack-underflow, type-check, read-only, undefined
static void set_remove_op(Trix *trx) {
    trx->verify_operands(VerifyKey, VerifyRWSet);

    auto key_ptr = trx->m_op_ptr;
    auto set_ptr = (key_ptr - 1);
    auto set = set_ptr->set_value(trx);
    set->set_remove(trx, key_ptr);
    --trx->m_op_ptr;
}

// set-remove-persist: set key -- set
// Like set-remove, but the removal is NOT journaled -- it persists
// across the enclosing save/restore.  At sl=0 silently degrades to
// set-remove.  No allocation occurs, so no above-barrier value check
// is needed; missing key raises /undefined matching set-remove.
// throws: opstack-underflow, type-check, read-only, undefined
static void set_remove_persist_op(Trix *trx) {
    trx->verify_operands(VerifyKey, VerifyRWSet);

    auto key_ptr = trx->m_op_ptr;
    auto set_ptr = (key_ptr - 1);
    auto set = set_ptr->set_value(trx);
    if (Save::is_active(trx)) {
        set->set_remove_persist(trx, key_ptr);
    } else {
        set->set_remove(trx, key_ptr);
    }
    --trx->m_op_ptr;
}

// set-member?: set key -- bool
// Tests if a key is in the set.
// throws: opstack-underflow, type-check
static void set_member_pred_op(Trix *trx) {
    trx->verify_operands(VerifyKey, VerifySet);

    auto key_ptr = trx->m_op_ptr;
    auto set_ptr = (key_ptr - 1);
    auto key = *key_ptr;
    auto set = set_ptr->set_value(trx);
    auto result = set->set_member(trx, key);
    key_ptr->maybe_free_extvalue(trx);
    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_boolean(result);
}

// set-union: set1 set2 -- set3
// Returns a new set containing elements from either set.
// throws: opstack-underflow, type-check, vm-full
static void set_union_op(Trix *trx) {
    trx->verify_operands(VerifySet, VerifySet);

    auto set2_ptr = trx->m_op_ptr;
    auto set1_ptr = (set2_ptr - 1);
    auto set1 = set1_ptr->set_value(trx);
    auto set2 = set2_ptr->set_value(trx);

    auto capacity = static_cast<length_t>(
            std::min(static_cast<vm_size_t>(set1->length()) + set2->length(), static_cast<vm_size_t>(MaxLength)));
    // Region-aware result so the union survives save/restore under ${...}.  Each
    // key.make_clone allocates GLOBALLY under ${...} and fires GC; root the result
    // across the whole fill so its already-added global key clones are not swept
    // (the result is a bare C local until pushed on the op stack at the tail).  The
    // sources stay alive on the op stack; the result is pre-sized so set_put never
    // expands (no alloc between a clone and its set_put).
    auto [result, offset] = create_result_set(trx, capacity, Object::DictMode::ReadWriteDynamic);
    trx->gc_root_push_oneoff(Object::make_set(offset));

    // add all from set1
    auto entry_offset = nulloffset;
    integer_t bucket_idx = -1;
    while (true) {
        auto [next, idx, key] = set1->set_next(trx, entry_offset, bucket_idx);
        if (next == nulloffset) {
            break;
        } else {
            result->set_put(trx, key.make_clone(trx));
            entry_offset = next;
            bucket_idx = idx;
        }
    }
    // add all from set2 (duplicates are no-ops -- set_put frees the clone)
    entry_offset = nulloffset;
    bucket_idx = -1;
    while (true) {
        auto [next, idx, key] = set2->set_next(trx, entry_offset, bucket_idx);
        if (next == nulloffset) {
            break;
        } else {
            result->set_put(trx, key.make_clone(trx));
            entry_offset = next;
            bucket_idx = idx;
        }
    }
    trx->gc_root_pop_oneoff();

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_set(offset);
}

// set-intersection: set1 set2 -- set3
// Returns a new set containing elements in both sets.
// throws: opstack-underflow, type-check, vm-full
static void set_intersection_op(Trix *trx) {
    trx->verify_operands(VerifySet, VerifySet);

    auto set2_ptr = trx->m_op_ptr;
    auto set1_ptr = (set2_ptr - 1);
    auto set1 = set1_ptr->set_value(trx);
    auto set2 = set2_ptr->set_value(trx);

    auto capacity = std::min(set1->length(), set2->length());
    // Region-aware + rooted across the fill (see set-union); set2->set_member is a
    // read-only probe of an op-stack-rooted source, so no temp scratch is involved.
    auto [result, offset] = create_result_set(trx, capacity, Object::DictMode::ReadWriteDynamic);
    trx->gc_root_push_oneoff(Object::make_set(offset));

    // iterate set1, add only those also in set2
    auto entry_offset = nulloffset;
    integer_t bucket_idx = -1;
    while (true) {
        auto [next, idx, key] = set1->set_next(trx, entry_offset, bucket_idx);
        if (next == nulloffset) {
            break;
        } else if (set2->set_member(trx, key)) {
            result->set_put(trx, key.make_clone(trx));
        }
        entry_offset = next;
        bucket_idx = idx;
    }
    trx->gc_root_pop_oneoff();

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_set(offset);
}

// set-difference: set1 set2 -- set3
// Returns a new set containing elements in set1 but not in set2.
// throws: opstack-underflow, type-check, vm-full
static void set_difference_op(Trix *trx) {
    trx->verify_operands(VerifySet, VerifySet);

    auto set2_ptr = trx->m_op_ptr;
    auto set1_ptr = (set2_ptr - 1);
    auto set1 = set1_ptr->set_value(trx);
    auto set2 = set2_ptr->set_value(trx);

    auto capacity = set1->length();
    // Region-aware + rooted across the fill (see set-union).
    auto [result, offset] = create_result_set(trx, capacity, Object::DictMode::ReadWriteDynamic);
    trx->gc_root_push_oneoff(Object::make_set(offset));

    // iterate set1, add only those NOT in set2
    auto entry_offset = nulloffset;
    integer_t bucket_idx = -1;
    while (true) {
        auto [next, idx, key] = set1->set_next(trx, entry_offset, bucket_idx);
        if (next == nulloffset) {
            break;
        } else if (!set2->set_member(trx, key)) {
            result->set_put(trx, key.make_clone(trx));
        }
        entry_offset = next;
        bucket_idx = idx;
    }
    trx->gc_root_pop_oneoff();

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_set(offset);
}

// subset?: set1 set2 -- bool
// Tests if set1 is a subset of set2 (every element of set1 is in set2).
// throws: opstack-underflow, type-check
static void set_subset_pred_op(Trix *trx) {
    trx->verify_operands(VerifySet, VerifySet);

    auto set2_ptr = trx->m_op_ptr;
    auto set1_ptr = (set2_ptr - 1);
    auto set1 = set1_ptr->set_value(trx);
    auto set2 = set2_ptr->set_value(trx);

    auto is_subset = true;
    auto entry_offset = nulloffset;
    integer_t bucket_idx = -1;
    while (true) {
        auto [next, idx, key] = set1->set_next(trx, entry_offset, bucket_idx);
        if (next == nulloffset) {
            break;
        } else if (!set2->set_member(trx, key)) {
            is_subset = false;
            break;
        } else {
            entry_offset = next;
            bucket_idx = idx;
        }
    }

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_boolean(is_subset);
}

// symmetric-difference: set1 set2 -- set3
// Returns a new set containing elements in either set but not both.
// throws: opstack-underflow, type-check, vm-full
static void set_symmetric_difference_op(Trix *trx) {
    trx->verify_operands(VerifySet, VerifySet);

    auto set2_ptr = trx->m_op_ptr;
    auto set1_ptr = (set2_ptr - 1);
    auto set1 = set1_ptr->set_value(trx);
    auto set2 = set2_ptr->set_value(trx);

    auto capacity = static_cast<length_t>(
            std::min(static_cast<vm_size_t>(set1->length()) + set2->length(), static_cast<vm_size_t>(MaxLength)));
    // Region-aware + rooted across both fill loops (see set-union).
    auto [result, offset] = create_result_set(trx, capacity, Object::DictMode::ReadWriteDynamic);
    trx->gc_root_push_oneoff(Object::make_set(offset));

    // iterate set1, add elements NOT in set2
    auto entry_offset = nulloffset;
    integer_t bucket_idx = -1;
    while (true) {
        auto [next, idx, key] = set1->set_next(trx, entry_offset, bucket_idx);
        if (next == nulloffset) {
            break;
        } else if (!set2->set_member(trx, key)) {
            result->set_put(trx, key.make_clone(trx));
        }
        entry_offset = next;
        bucket_idx = idx;
    }

    // iterate set2, add elements NOT in set1
    entry_offset = nulloffset;
    bucket_idx = -1;
    while (true) {
        auto [next, idx, key] = set2->set_next(trx, entry_offset, bucket_idx);
        if (next == nulloffset) {
            break;
        } else if (!set1->set_member(trx, key)) {
            result->set_put(trx, key.make_clone(trx));
        }
        entry_offset = next;
        bucket_idx = idx;
    }
    trx->gc_root_pop_oneoff();

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_set(offset);
}

// disjoint?: set1 set2 -- bool
// Tests if two sets have no elements in common.
// Iterates the smaller set for efficiency.
// throws: opstack-underflow, type-check
static void set_disjoint_pred_op(Trix *trx) {
    trx->verify_operands(VerifySet, VerifySet);

    auto set2_ptr = trx->m_op_ptr;
    auto set1_ptr = (set2_ptr - 1);
    auto set1 = set1_ptr->set_value(trx);
    auto set2 = set2_ptr->set_value(trx);

    // iterate the smaller set, test membership in the larger
    auto iterate = set1;
    auto test = set2;
    if (set1->length() > set2->length()) {
        iterate = set2;
        test = set1;
    }

    auto is_disjoint = true;
    auto entry_offset = nulloffset;
    integer_t bucket_idx = -1;
    while (true) {
        auto [next, idx, key] = iterate->set_next(trx, entry_offset, bucket_idx);
        if (next == nulloffset) {
            break;
        } else if (test->set_member(trx, key)) {
            is_disjoint = false;
            break;
        } else {
            entry_offset = next;
            bucket_idx = idx;
        }
    }

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_boolean(is_disjoint);
}

// members: set -- arr
// Returns an array of all name elements in the set.
// throws: opstack-underflow, type-check, vm-full
static void set_members_op(Trix *trx) {
    trx->verify_operands(VerifySet);

    auto set_ptr = trx->m_op_ptr;
    auto set = set_ptr->set_value(trx);
    auto count = set->length();

    auto [array_ptr, array_offset] = trx->vm_alloc_dispatch_n<Object>(count, ChunkKind::Array);
    // Region-aware result so the members array survives save/restore under ${...}.
    // Null + root the result across the clones: under ${...} each make_clone is a
    // global alloc that can fire vm_global_gc, and the result array is not yet on
    // any stack -- the block-walker would walk uninitialized slots, and any
    // already-written global clones would be swept.
    std::fill_n(array_ptr, count, Object::make_null());
    auto result_obj = Object::make_array(array_offset, count);
    trx->gc_root_push_oneoff(result_obj);
    auto i = length_t{0};
    auto entry_offset = nulloffset;
    integer_t bucket_idx = -1;
    while (true) {
        auto [next, idx, key] = set->set_next(trx, entry_offset, bucket_idx);
        if (next == nulloffset) {
            break;
        } else {
            array_ptr[i++] = key.make_clone(trx);
            entry_offset = next;
            bucket_idx = idx;
        }
    }
    trx->gc_root_pop_oneoff();

    *set_ptr = result_obj;
}

// set-from-array: arr -- set
// Creates a set from an array of names.
// throws: opstack-underflow, type-check, vm-full
static void set_from_array_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);

    auto arr_ptr = trx->m_op_ptr;
    auto count = arr_ptr->length(trx);
    auto elem_data = arr_ptr->is_array() ? arr_ptr->array_objects(trx) : nullptr;

    // validate: no Null elements
    if (elem_data != nullptr) {
        for (length_t i = 0; i < count; ++i) {
            if (elem_data[i].is_null()) {
                trx->error(Error::TypeCheck, "set-from-array: element {} must not be null", i);
            }
        }
    } else {
        auto [pdata, plen] = arr_ptr->packed_value(trx);
        auto scan = pdata;
        for (length_t i = 0; i < count; ++i) {
            auto [next, elem] = Object::extract_next_packed(trx, scan);
            if (elem.is_null()) {
                trx->error(Error::TypeCheck, "set-from-array: element {} must not be null", i);
            } else {
                scan = next;
            }
        }
    }

    auto capacity = std::max(count, static_cast<length_t>(4));
    // Region-aware result so the set survives save/restore under ${...} (mirrors
    // the `set` constructor's dispatch).  make_clone clones each element globally
    // and set_put can expand the dynamic set -- both are global allocs that fire
    // GC -- so root the set across the whole fill and the in-flight clone across
    // each set_put (the clone is a bare C local until set_put links it in).
    auto [set, offset] = create_result_set(trx, capacity, Object::DictMode::ReadWriteDynamic);
    trx->gc_root_push_oneoff(Object::make_set(offset));
    trx->require_gc_root_capacity_more(1);
    if (elem_data != nullptr) {
        for (length_t i = 0; i < count; ++i) {
            auto clone = elem_data[i].make_clone(trx);
            *++trx->m_gc_roots_ptr = clone;
            set->set_put(trx, clone);
            trx->gc_root_pop_n(1);
        }
    } else {
        auto [pdata, plen] = arr_ptr->packed_value(trx);
        auto scan = pdata;
        for (length_t i = 0; i < count; ++i) {
            auto [next, elem] = Object::extract_next_packed(trx, scan);
            auto clone = elem.make_clone(trx);
            *++trx->m_gc_roots_ptr = clone;
            set->set_put(trx, clone);
            trx->gc_root_pop_n(1);
            scan = next;
        }
    }
    trx->gc_root_pop_oneoff();

    *trx->m_op_ptr = Object::make_set(offset);
}

// @set-for-all: control operator for set iteration.
// Exec stack frame: [proc] [set] [bucket-idx] [entry-offset] [saved-depth] [@set-for-all]
// Verifies that the body returned the operand stack to the depth
// it had BEFORE the iteration's key was pushed; mismatch raises
// /range-check.
static void at_set_forall_op(Trix *trx) {
    auto saved_depth_ptr = trx->m_exec_ptr;
    auto offset_ptr = (saved_depth_ptr - 1);
    auto bucket_ptr = (offset_ptr - 1);
    auto set_ptr = (offset_ptr - 2);

    auto saved_depth = saved_depth_ptr->integer_value();
    auto current_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);
    if (current_depth != saved_depth) {
        trx->error(Error::RangeCheck,
                   "for-all: body left {} item(s) on the operand stack (expected stack effect 'key -- ')",
                   current_depth - saved_depth);
    }

    auto entry_offset = vm_offset_t{offset_ptr->uinteger_value()};
    auto bucket_idx = bucket_ptr->integer_value();
    auto set = set_ptr->set_value(trx);
    auto [next_offset, next_idx, key] = set->set_next(trx, entry_offset, bucket_idx);
    if (next_offset != nulloffset) {
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        offset_ptr->update_uinteger(static_cast<uinteger_t>(next_offset));
        bucket_ptr->update_integer(next_idx);

        *++trx->m_op_ptr = key.make_clone(trx);

        auto proc = set_ptr[-1];
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atSetForAll);
        *++trx->m_exec_ptr = proc;
    } else {
        trx->m_exec_ptr -= 5;
    }
}

// set-from-mark: mark elem0 ... elemN -- set
// Collects operands above mark into a new set.
// throws: vm-full, unmatched-mark
static void setfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_set_from_mark(trx);
}

// readonly-set-from-mark: mark elem0 ... elemN -- set
// Collects operands above mark into a new read-only set.
// throws: vm-full, unmatched-mark
static void readonlysetfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_set_from_mark(trx, Object::DictMode::ReadOnly);
}

// =set-from-mark: mark elem0 ... elemN -- set
// Collects operands above mark into a new =set.
// throws: limit-check, unmatched-mark
static void eqsetfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_eqset_from_mark(trx);
}

// readonly-=set-from-mark: mark elem0 ... elemN -- set
// Collects operands above mark into a new read-only =set.
// throws: limit-check, unmatched-mark
static void readonlyeqsetfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_eqset_from_mark(trx, Object::DictMode::ReadOnly);
}
