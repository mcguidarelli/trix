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

//===--- Array Iteration Operators ---===//
//
// Implements callback-based iteration over arrays and packed arrays:
// for-all, any, all, find, count-if, scan, take-while, drop-while,
// min-by, max-by, sort-by, group-by, partition, plus dict variants
// (map-dict, filter-dict).  The map, map-indexed, and filter entry ops
// live in ops_higher.inl; this file holds only their per-element step
// ops (@array-map, @array-map-indexed, @array-filter).  Based on:
//
//   PostScript for-all: iterate a proc over each element of a container.
//   Trix extends this with the full range of functional iteration patterns
//   from Haskell/Clojure/Ruby (any, all, find, group-by, partition, etc.).
//
// --- Core concepts for maintainers ---
//
// TRAMPOLINE PATTERN
//   All callback-based iteration uses the exec-stack trampoline: the
//   operator pushes state (container, index, proc, accumulators) onto the
//   exec stack, followed by a control operator and the user proc.  The
//   interpreter runs the proc; when it completes, the control operator
//   fires, reads the result, advances the index, and either pushes the
//   next iteration or terminates.
//
//   This avoids C++ recursion for iteration -- the loop is driven entirely
//   by the exec stack, with one exec-stack round-trip per element.
//
// PACKED ARRAY HANDLING
//   Packed arrays cannot be indexed (sequential-access only).  Iteration
//   operators that need random access (sort-by, group-by) first expand
//   the packed array into a temporary Array via expand_packed_to_temp().
//   This uses the temp allocation region (vm_temp_alloc) so the expansion
//   is automatically reclaimed.
//
// SHORT-CIRCUIT OPERATIONS
//   any/all use short-circuit evaluation: `any` stops on the first true,
//   `all` stops on the first false.  Implemented via at_array_any_all_impl
//   with a `short_circuit_on` parameter.
//
// FOR-ALL VARIANTS
//   @array-for-all:  iterate elements of an array
//   @packed-for-all: iterate elements of a packed array (sequential decode)
//   @string-for-all: iterate characters of a string (in this file)
//   @dict-for-all:   iterate key-value pairs of a dict
//   @set-for-all:    iterate elements of a set (in ops_set.inl)
//   @record-for-all: iterate name-value pairs of a record (in ops_record.inl)
//

// Shared callback for any/all: short-circuits on short_circuit_on, continues otherwise.
// short_circuit_on == true: short-circuit when proc returns true (any)
// short_circuit_on == false: short-circuit when proc returns false (all)
static void at_array_any_all_impl(Trix *trx, bool short_circuit_on, SystemName control_op) {
    auto src_ptr = trx->m_exec_ptr;
    auto proc_ptr = (src_ptr - 1);

    trx->verify_operands(VerifyBoolean);
    auto result = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (result == short_circuit_on) {
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_boolean(short_circuit_on);
        trx->m_exec_ptr -= 2;
    } else if (src_ptr->arrays_length() == 0) {
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_boolean(!short_circuit_on);
        trx->m_exec_ptr -= 2;
    } else {
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        auto elem_obj = src_ptr->arrays_pop_clone_head(trx);
        *++trx->m_op_ptr = elem_obj;

        *++trx->m_exec_ptr = Object::make_control_operator(control_op);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

static void at_array_any_op(Trix *trx) {
    at_array_any_all_impl(trx, true, SystemName::atArrayAny);
}

// Shared initializer for any/all.
static void any_all_impl(Trix *trx, bool empty_result, SystemName control_op) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto src_length = container_ptr->arrays_length();

    if (src_length == 0) {
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = Object::make_boolean(empty_result);
    } else {
        auto src_obj = *container_ptr;
        auto first_obj = src_obj.arrays_pop_clone_head(trx);

        trx->require_exec_capacity(4);
        *++trx->m_exec_ptr = *proc_ptr;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(control_op);
        *++trx->m_exec_ptr = *proc_ptr;

        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = first_obj;
    }
}

// any: arr proc :- bool
// True if proc returns true for any element.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void any_op(Trix *trx) {
    any_all_impl(trx, false, SystemName::atArrayAny);
}

// all: arr proc :- bool
// True if proc returns true for all elements.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void all_op(Trix *trx) {
    any_all_impl(trx, true, SystemName::atArrayAll);
}

static void at_array_all_op(Trix *trx) {
    at_array_any_all_impl(trx, false, SystemName::atArrayAll);
}

// @array-count: (exec stack: proc src-arr counter)
// Increments counter on true, continues on false.
// throws: execstack-overflow, opstack-overflow
static void at_array_count_op(Trix *trx) {
    auto counter_ptr = trx->m_exec_ptr;
    auto src_ptr = (counter_ptr - 1);
    auto proc_ptr = (counter_ptr - 2);

    trx->verify_operands(VerifyBoolean);
    auto result = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (result) {
        counter_ptr->update_integer(counter_ptr->integer_value() + 1);
    }

    if (src_ptr->arrays_length() == 0) {
        // done: push count to operand stack, clean up frame
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = *counter_ptr;
        trx->m_exec_ptr -= 3;
    } else {
        // more elements: push next element, re-push frame + proc
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        auto elem_obj = src_ptr->arrays_pop_clone_head(trx);
        *++trx->m_op_ptr = elem_obj;

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayCount);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// count-if: arr proc :- int
// Counts elements for which proc returns true.
// throws: execstack-overflow, opstack-underflow, type-check
static void countif_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto src_length = container_ptr->arrays_length();

    if (src_length == 0) {
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = Object::make_integer(0);
    } else {
        auto src_obj = *container_ptr;
        auto first_obj = src_obj.arrays_pop_clone_head(trx);

        // exec stack frame: [proc] [src] [counter] [@array-count] [proc]
        trx->require_exec_capacity(5);
        *++trx->m_exec_ptr = *proc_ptr;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayCount);
        *++trx->m_exec_ptr = *proc_ptr;

        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = first_obj;
    }
}

// @array-scan callback
// exec stack frame: [proc] [src] [result-arr] [accumulator] [index] [@array-scan]
// Stores proc result as new accumulator and in result array, then feeds next element.
// throws: execstack-overflow, opstack-overflow, vm-full
static void at_array_scan_op(Trix *trx) {
    auto index_ptr = trx->m_exec_ptr;
    auto acc_ptr = (index_ptr - 1);
    auto result_ptr = (index_ptr - 2);
    auto src_ptr = (index_ptr - 3);
    auto proc_ptr = (index_ptr - 4);

    // pop new accumulator (result of proc on [acc, element])
    trx->require_op_count(1);
    auto new_acc_obj = *trx->m_op_ptr;
    --trx->m_op_ptr;

    // Install the new accumulator into its exec-stack slot BEFORE any allocation.
    // new_acc_obj was just popped into a C-local; the result-array make_clone below
    // allocates (global mode), and a GC there would sweep an unrooted ${...}-global
    // accumulator -- then *acc_ptr would hold a dangling reference that the next GC
    // walk of the exec stack reports as a freed block.  Freeing the old acc first and
    // storing makes the slot a valid root across the clone, which reads from it.
    acc_ptr->maybe_free_extvalue(trx);
    *acc_ptr = new_acc_obj;

    // store in result array (clone from the now-rooted accumulator slot)
    auto idx = static_cast<length_t>(index_ptr->integer_value());
    result_ptr->array_objects(trx)[idx] = acc_ptr->make_clone(trx);
    index_ptr->update_integer(static_cast<integer_t>(idx + 1));

    if (src_ptr->arrays_length() == 0) {
        // done: push result array
        trx->require_op_capacity(1);
        acc_ptr->maybe_free_extvalue(trx);
        *++trx->m_op_ptr = *result_ptr;
        trx->m_exec_ptr -= 5;
    } else {
        // more elements: push accumulator and next element, re-push frame + proc
        trx->require_op_capacity(2);
        trx->require_exec_capacity(2);

        // Push the accumulator clone FIRST (acc_ptr is rooted on the exec stack), then
        // pop+push the next element with no allocation between the pop-clone and its
        // push -- so neither value is orphaned across an alloc.
        *++trx->m_op_ptr = acc_ptr->make_clone(trx);
        *++trx->m_op_ptr = src_ptr->arrays_pop_clone_head(trx);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayScan);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// scan: arr init proc scan -- arr
// Like reduce but collects all intermediate accumulators.
// Result has N+1 elements: [init, proc(init,e0), proc(prev,e1), ...].
// throws: execstack-overflow, opstack-underflow, type-check, vm-full
static void scan_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyAny, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto init_ptr = (proc_ptr - 1);
    auto container_ptr = (proc_ptr - 2);
    auto count = container_ptr->arrays_length();

    if (count == 0) {
        // empty array: result is [init]
        auto [result_ptr, result_offset] = trx->vm_alloc_dispatch_n<Object>(1, ChunkKind::Array);
        // Root the result across the init clone: under ${...} the clone is a global
        // alloc that can fire vm_global_gc and would sweep the unrooted result.
        result_ptr[0] = Object::make_null();
        auto result_obj = Object::make_array(result_offset, 1);
        trx->gc_root_push_oneoff(result_obj);
        result_ptr[0] = init_ptr->make_clone(trx);
        trx->gc_root_pop_oneoff();
        trx->m_op_ptr -= 3;
        *++trx->m_op_ptr = result_obj;
    } else {
        if (count >= MaxArrayLength) {
            trx->error(
                    Error::LimitCheck, "scan: source length {} too large; result would exceed maximum {}", count, MaxArrayLength);
        }
        // allocate result array of size count+1
        auto result_count = static_cast<length_t>(count + 1);
        auto [result_ptr, result_offset] = trx->vm_alloc_dispatch_n<Object>(result_count, ChunkKind::Array);

        // Null ALL slots and root the result BEFORE the first clone.  Under ${...} the
        // result is a GLOBAL block, and gvm_for_each walks every global block by its
        // stamped obj_count on the next GC -- so an uninitialized slot read during the
        // GC fired by init's make_clone below would be a garbage offset and crash the
        // walk.  Null first (so the walk is safe), then root (so the block is not swept
        // as unreachable), THEN clone init into slot 0.  first_obj is rooted too: it
        // lives only in a C-local across its own pop-clone's GC.
        for (length_t i = 0; i < result_count; ++i) {
            result_ptr[i] = Object::make_null();
        }
        auto result_obj = Object::make_array(result_offset, result_count);

        trx->require_gc_root_capacity(2);
        *++trx->m_gc_roots_ptr = result_obj;
        result_ptr[0] = init_ptr->make_clone(trx);

        // pop first element from source
        auto src_obj = *container_ptr;
        auto first_obj = src_obj.arrays_pop_clone_head(trx);
        *++trx->m_gc_roots_ptr = first_obj;

        auto saved_init_obj = init_ptr->make_clone(trx);

        // exec stack frame: [proc] [src] [result-arr] [accumulator] [index=1] [@control] [proc]
        trx->require_exec_capacity(7);
        auto saved_proc_obj = *proc_ptr;

        *++trx->m_exec_ptr = saved_proc_obj;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = result_obj;
        *++trx->m_exec_ptr = saved_init_obj;           // accumulator
        *++trx->m_exec_ptr = Object::make_integer(1);  // index (next write slot)
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayScan);
        *++trx->m_exec_ptr = saved_proc_obj;

        // push accumulator and first element to operand stack for proc
        trx->m_op_ptr -= 3;
        *++trx->m_op_ptr = saved_init_obj.make_clone(trx);
        *++trx->m_op_ptr = first_obj;
        trx->reset_gc_root(2);
    }
}

// @array-min-by / @array-max-by callback
// exec stack frame: [proc] [src] [best-elem] [best-key] [saved-elem] [@control]
// Compares proc result (key) with best-key; updates best if better.
// throws: execstack-overflow, opstack-overflow
static void at_array_minby_op(Trix *trx) {
    at_array_extremeby_impl(trx, true);
}

static void at_array_maxby_op(Trix *trx) {
    at_array_extremeby_impl(trx, false);
}

static void at_array_extremeby_impl(Trix *trx, bool want_min) {
    trx->require_op_count(1);
    auto saved_elem = trx->m_exec_ptr;
    auto best_key = (saved_elem - 1);
    auto best_elem = (saved_elem - 2);
    auto src_ptr = (saved_elem - 3);
    auto proc_ptr = (saved_elem - 4);

    // pop key from operand stack (result of proc on current element)
    auto new_key_obj = *trx->m_op_ptr;
    --trx->m_op_ptr;

    if (best_key->is_null()) {
        // first callback: store key for initial best-elem
        *best_key = new_key_obj;
    } else {
        // compare: is new key better than best key?
        const bool better =
                want_min ? object_less_than(trx, new_key_obj, *best_key) : object_less_than(trx, *best_key, new_key_obj);

        if (better) {
            // replace best with current
            best_elem->maybe_free_extvalue(trx);
            best_key->maybe_free_extvalue(trx);
            *best_elem = *saved_elem;
            *best_key = new_key_obj;
        } else {
            // discard current
            saved_elem->maybe_free_extvalue(trx);
            new_key_obj.maybe_free_extvalue(trx);
        }
    }

    if (src_ptr->arrays_length() == 0) {
        // done: push best element to operand stack
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = best_elem->make_clone(trx);
        best_elem->maybe_free_extvalue(trx);
        best_key->maybe_free_extvalue(trx);
        trx->m_exec_ptr -= 5;
    } else {
        // more elements: pop next, save it, push clone to op stack for proc
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        auto elem_obj = src_ptr->arrays_pop_clone_head(trx);
        *saved_elem = elem_obj;
        *++trx->m_op_ptr = elem_obj.make_clone(trx);

        auto control_name = want_min ? SystemName::atArrayMinBy : SystemName::atArrayMaxBy;
        *++trx->m_exec_ptr = Object::make_control_operator(control_name);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// min-by: arr proc :- any
// Returns the element whose key (produced by proc) is smallest.
// throws: execstack-overflow, opstack-underflow, range-check, type-check
static void minby_op(Trix *trx) {
    extremeby_impl(trx, true);
}

// max-by: arr proc :- any
// Returns the element whose key (produced by proc) is largest.
// throws: execstack-overflow, opstack-underflow, range-check, type-check
static void maxby_op(Trix *trx) {
    extremeby_impl(trx, false);
}

static void extremeby_impl(Trix *trx, bool want_min) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto count = container_ptr->arrays_length();

    if (count == 0) {
        trx->error(Error::RangeCheck, "{}: empty array", want_min ? "min-by" : "max-by");
    } else {
        // pop first element -- borrowed reference; clone at each use site
        auto src_obj = *container_ptr;
        auto first_obj = src_obj.is_array() ? src_obj.array_pop_head(trx) : src_obj.packed_pop_head(trx);

        if (count == 1) {
            // single element: return it directly, no proc evaluation needed
            trx->m_op_ptr -= 2;
            *++trx->m_op_ptr = first_obj.make_clone(trx);
        } else {
            // exec stack frame: [proc] [src] [best-elem] [best-key=null] [first-as-saved] [@control] [proc]
            // First callback: proc evaluated on first element, key stored as best-key.
            // Subsequent callbacks: compare new key with best-key.
            trx->require_exec_capacity(7);
            auto saved_proc_obj = *proc_ptr;
            auto control_name = want_min ? SystemName::atArrayMinBy : SystemName::atArrayMaxBy;

            *++trx->m_exec_ptr = saved_proc_obj;
            *++trx->m_exec_ptr = src_obj;
            *++trx->m_exec_ptr = first_obj.make_clone(trx);  // best-elem (independent owned copy)
            *++trx->m_exec_ptr = Object::make_null();        // best-key (sentinel: not yet computed)
            *++trx->m_exec_ptr = first_obj.make_clone(trx);  // saved-elem (independent owned copy)
            *++trx->m_exec_ptr = Object::make_control_operator(control_name);
            *++trx->m_exec_ptr = saved_proc_obj;

            // push first element clone to operand stack for proc evaluation
            trx->m_op_ptr -= 2;
            *++trx->m_op_ptr = first_obj.make_clone(trx);
        }
    }
}

// @array-sort-by callback
// exec stack frame: [proc] [src] [keys-arr] [elems-arr] [index] [@array-sort-by]
// Collects key results, then sorts using VM-allocated index array with in-place permutation.
// throws: execstack-overflow, opstack-overflow, vm-full
static void at_array_sortby_op(Trix *trx) {
    auto index_ptr = trx->m_exec_ptr;
    auto elems_ptr = (index_ptr - 1);
    auto keys_ptr = (index_ptr - 2);
    auto src_ptr = (index_ptr - 3);
    auto proc_ptr = (index_ptr - 4);
    auto temp_ptr = (index_ptr - 5);
    auto flag_ptr = (index_ptr - 6);  // saved m_curr_alloc_global (see sortby_op)

    // store key result from proc
    trx->require_op_count(1);
    auto key_obj = *trx->m_op_ptr;
    --trx->m_op_ptr;
    auto idx = static_cast<length_t>(index_ptr->integer_value());
    keys_ptr->array_objects(trx)[idx] = key_obj;
    index_ptr->update_integer(static_cast<integer_t>(idx + 1));

    if (src_ptr->arrays_length() == 0) {
        // all keys collected -- sort and produce result
        auto count = elems_ptr->arrays_length();
        auto keys_data = keys_ptr->array_objects(trx);
        auto elems_data = elems_ptr->array_objects(trx);

        // index array: use C++ stack for small arrays, temp region for large
        static constexpr length_t StackIndexLimit = 256;
        length_t stack_idx[StackIndexLimit];
        length_t *sort_idx;
        if (count <= StackIndexLimit) {
            sort_idx = stack_idx;
        } else {
            sort_idx = trx->vm_temp_alloc<length_t>(count);
        }
        for (length_t i = 0; i < count; ++i) {
            sort_idx[i] = i;
        }

        std::stable_sort(sort_idx, sort_idx + count, [trx, keys_data](length_t a, length_t b) {
            return object_less_than(trx, keys_data[a], keys_data[b]);
        });

        // allocate sorted result array.  make_clone_local: the keys/elems temp
        // arrays are still live (read here and freed below), and a GLOBAL alloc
        // under ${...} would grow the global region down over them (gvm_alloc
        // bounds-checks only the local heap top).  Result stays LOCAL, as the
        // top-level path allocates.
        auto [result_ptr, result_offset] = trx->vm_alloc_n<Object>(count);
        for (length_t i = 0; i < count; ++i) {
            result_ptr[i] = elems_data[sort_idx[i]].make_clone_local(trx);
        }

        // free ExtValues from keys and elements
        for (length_t i = 0; i < count; ++i) {
            keys_data[i].maybe_free_extvalue(trx);
            elems_data[i].maybe_free_extvalue(trx);
        }

        // free temp allocations (sort_idx if large, elems, keys)
        auto saved_temp = trx->m_vm_base + temp_ptr->integer_value();
        trx->vm_temp_restore(saved_temp);

        // restore the allocation region suppressed by sortby_op, push result, drop frame
        trx->m_curr_alloc_global = flag_ptr->boolean_value();
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(result_offset, count));
        trx->m_exec_ptr -= 7;
    } else {
        // more elements: pop next, store in elems, push clone for proc
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        auto next_obj = src_ptr->arrays_pop_clone_head(trx);
        auto next_idx = static_cast<length_t>(index_ptr->integer_value());
        elems_ptr->array_objects(trx)[next_idx] = next_obj;
        *++trx->m_op_ptr = next_obj.make_clone(trx);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArraySortBy);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// sort-by: arr proc sort-by -- sorted-arr
// Sorts array elements by key produced by proc.
// Two-phase: collect keys via exec-stack callbacks, then std::stable_sort with
// VM-allocated index array and in-place cycle-following permutation.
// throws: execstack-overflow, opstack-underflow, type-check, vm-full
static void sortby_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto count = container_ptr->arrays_length();

    if (count == 0) {
        // empty: return empty array
        auto [_, offset] = trx->vm_alloc_n<Object>(0);
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(offset, 0));
    } else if (count == 1) {
        // single element: return as-is, no proc evaluation needed
        auto src_obj = *container_ptr;
        auto elem_obj = src_obj.arrays_pop_clone_head(trx);
        trx->m_op_ptr -= 2;
        auto [dst_ptr, dst_offset] = trx->vm_alloc_n<Object>(1);
        dst_ptr[0] = elem_obj;
        *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(dst_offset, 1));
    } else {
        // Run the whole two-phase collect-and-sort with LOCAL allocation, even under
        // ${...}.  The keys/elems arrays live in the temp region across every proc
        // invocation, and a GLOBAL allocation (the user proc's, the popped-element
        // clone's, or the result's) grows the global region DOWN over them (gvm_alloc
        // bounds-checks only the local heap top, never the temp region).  Suppressing
        // the global flag keeps the proc + all clones local, so nothing clobbers the
        // temp scratch -- exactly as the (working) top-level path runs.  The saved
        // flag rides the bottom of the exec frame; at_array_sortby_op restores it at
        // finalization.  (Restored on the error-unwind path by the enclosing ${...}
        // barrier.)  The result is a local sorted array (tracked region-inconsistency).
        auto saved_global = trx->m_curr_alloc_global;
        trx->m_curr_alloc_global = false;

        // allocate keys and elements arrays in top-end temp region
        auto saved_temp = trx->m_vm_temp_ptr;
        auto keys_ptr = trx->vm_temp_alloc<Object>(count);
        auto keys_offset = static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(keys_ptr) - trx->m_vm_base);
        auto elems_ptr = trx->vm_temp_alloc<Object>(count);
        auto elems_offset = static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(elems_ptr) - trx->m_vm_base);

        // initialize keys to null
        for (length_t i = 0; i < count; ++i) {
            keys_ptr[i] = Object::make_null();
        }

        // pop first element and store in elems[0]
        auto src_obj = *container_ptr;
        auto first_obj = src_obj.arrays_pop_clone_head(trx);
        elems_ptr[0] = first_obj;

        // exec stack frame: [saved-global] [saved-temp] [proc] [src] [keys-arr] [elems-arr] [index=0] [@control] [proc]
        // saved-global rides the bottom so the existing index-relative offsets stay put.
        trx->require_exec_capacity(9);
        auto saved_proc_obj = *proc_ptr;
        auto keys_obj = Object::make_array(keys_offset, count);
        auto elems_obj = Object::make_array(elems_offset, count);
        auto temp_offset = static_cast<integer_t>(saved_temp - trx->m_vm_base);

        *++trx->m_exec_ptr = Object::make_boolean(saved_global);
        *++trx->m_exec_ptr = Object::make_integer(temp_offset);
        *++trx->m_exec_ptr = saved_proc_obj;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = keys_obj;
        *++trx->m_exec_ptr = elems_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);  // index
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArraySortBy);
        *++trx->m_exec_ptr = saved_proc_obj;

        // push first element clone to operand stack for proc evaluation
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = first_obj.make_clone(trx);
    }
}

// @array-group-by: (exec stack: proc src keys-arr elems-arr index)
// Callback for group-by key collection phase. Identical to sort-by key collection,
// but finalization builds a dict of arrays instead of sorting.
// throws: vm-full, execstack-overflow, opstack-overflow
static void at_array_groupby_op(Trix *trx) {
    auto index_ptr = trx->m_exec_ptr;
    auto elems_ptr = (index_ptr - 1);
    auto keys_ptr = (index_ptr - 2);
    auto src_ptr = (index_ptr - 3);
    auto proc_ptr = (index_ptr - 4);
    auto temp_ptr = (index_ptr - 5);
    auto flag_ptr = (index_ptr - 6);  // saved m_curr_alloc_global (see groupby_op)

    // store key result from proc
    trx->require_op_count(1);
    auto key_obj = *trx->m_op_ptr;
    --trx->m_op_ptr;
    auto idx = static_cast<length_t>(index_ptr->integer_value());
    keys_ptr->array_objects(trx)[idx] = key_obj;
    index_ptr->update_integer(static_cast<integer_t>(idx + 1));

    if (src_ptr->arrays_length() == 0) {
        // all keys collected -- build dict of arrays grouped by key
        auto count = elems_ptr->arrays_length();
        auto keys_data = keys_ptr->array_objects(trx);
        auto elems_data = elems_ptr->array_objects(trx);

        // Reject invalid keys (null/NaN/Inf).  Match the success-path cleanup:
        // free extvalues, restore temp, then pop the exec frame.  NaN is the
        // motivator here -- equal(NaN, NaN) is false, so NaN keys would slip
        // past put() as new entries and then fail get() on the very next pass.
        auto bad = trx->find_invalid_key_index(keys_data, count);
        if (bad.index < count) {
            for (length_t j = 0; j < count; ++j) {
                keys_data[j].maybe_free_extvalue(trx);
                elems_data[j].maybe_free_extvalue(trx);
            }
            auto saved_temp = trx->m_vm_base + temp_ptr->integer_value();
            trx->vm_temp_restore(saved_temp);
            trx->m_curr_alloc_global = flag_ptr->boolean_value();
            trx->m_exec_ptr -= 7;
            trx->error(bad.err, "group-by: key for element {} is {} and cannot be used as a dict key", bad.index, bad.reason);
        } else {
            // pass 1: count elements per group using a temp-region dict
            auto [count_dict, count_dict_offset] = Dict::create_temp_dict(trx, count);
            for (length_t i = 0; i < count; ++i) {
                auto existing = count_dict->get(trx, &keys_data[i]);
                if (existing != nullptr) {
                    existing->update_integer(existing->integer_value() + 1);
                } else {
                    count_dict->put(trx, keys_data[i], Object::make_integer(1));
                }
            }

            // pass 2: create result dict and allocate per-group arrays
            // iterate keys to find unique keys; for each new key, look up its
            // count, allocate the group array, and store in result dict.
            // reuse count_dict values as write indices (reset to 0).
            auto [result_dict, result_dict_offset] = Dict::create_dict(trx, count, Object::DictMode::ReadWriteDynamic);
            for (length_t i = 0; i < count; ++i) {
                if (result_dict->get(trx, &keys_data[i]) == nullptr) {
                    auto count_val = count_dict->get(trx, &keys_data[i]);
                    if (count_val == nullptr) {
                        trx->error(Error::InternalError, "group-by: key missing from count_dict after pass 1 (element {})", i);
                    } else {
                        auto group_size = static_cast<length_t>(count_val->integer_value());
                        auto [group_ptr, group_offset] = trx->vm_alloc_n<Object>(group_size);
                        for (length_t j = 0; j < group_size; ++j) {
                            group_ptr[j] = Object::make_null();
                        }
                        // make_clone_local: count_dict (a temp-region dict) is live here, and a
                        // GLOBAL alloc under ${...} would grow the global region down over it
                        // (gvm_alloc bounds-checks only the local heap top).  Keep the result
                        // dict's key and its group arrays LOCAL, as the top-level path does.
                        result_dict->put(trx, keys_data[i].make_clone_local(trx), Object::make_array(group_offset, group_size));
                        count_val->update_integer(0);  // reset to use as write index
                    }
                }
            }

            // pass 3: distribute elements into groups
            for (length_t i = 0; i < count; ++i) {
                auto group_arr = result_dict->get(trx, &keys_data[i]);
                auto write_idx = count_dict->get(trx, &keys_data[i]);
                if ((group_arr == nullptr) || (write_idx == nullptr)) {
                    trx->error(Error::InternalError, "group-by: key missing from result_dict/count_dict in pass 3 (element {})", i);
                } else {
                    auto wi = static_cast<length_t>(write_idx->integer_value());
                    group_arr->array_objects(trx)[wi] = elems_data[i].make_clone_local(trx);  // local: temp dicts live (see pass 2)
                    write_idx->update_integer(write_idx->integer_value() + 1);
                }
            }

            // free ExtValues from keys and elements
            for (length_t i = 0; i < count; ++i) {
                keys_data[i].maybe_free_extvalue(trx);
                elems_data[i].maybe_free_extvalue(trx);
            }

            // free temp allocations (keys, elems)
            auto saved_temp = trx->m_vm_base + temp_ptr->integer_value();
            trx->vm_temp_restore(saved_temp);

            // restore the allocation region suppressed by groupby_op, push result, drop frame
            trx->m_curr_alloc_global = flag_ptr->boolean_value();
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_dict(result_dict_offset));
            trx->m_exec_ptr -= 7;
        }
    } else {
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        auto next_obj = src_ptr->arrays_pop_clone_head(trx);
        auto next_idx = static_cast<length_t>(index_ptr->integer_value());
        elems_ptr->array_objects(trx)[next_idx] = next_obj;
        *++trx->m_op_ptr = next_obj.make_clone(trx);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayGroupBy);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// group-by: arr proc group-by -- dict
// Groups array elements by key produced by proc. Returns a dict mapping
// each key to an array of elements that produced that key.
// Two-phase: collect keys via exec-stack callbacks, then build dict.
// throws: execstack-overflow, opstack-underflow, type-check, vm-full
static void groupby_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto count = container_ptr->arrays_length();

    if (count == 0) {
        // empty: return empty dict
        auto [dict, offset] = Dict::create_dict(trx, 0, Object::DictMode::ReadWriteDynamic);
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_dict(offset));
    } else {
        // Run the whole two-phase collect-and-group with LOCAL allocation, even under
        // ${...}: the keys/elems temp arrays live across every proc invocation, and a
        // GLOBAL allocation (proc/element-clone/result) grows the global region DOWN
        // over them (gvm_alloc bounds-checks only the local heap top).  Suppressing the
        // flag keeps the proc + all clones local -- like the (working) top-level path.
        // The saved flag rides the bottom of the exec frame; at_array_groupby_op
        // restores it.  (Error-unwind: restored by the enclosing ${...} barrier.)  The
        // result is a local dict-of-local-arrays (tracked region-inconsistency).
        auto saved_global = trx->m_curr_alloc_global;
        trx->m_curr_alloc_global = false;

        // allocate keys and elements arrays in top-end temp region
        auto saved_temp = trx->m_vm_temp_ptr;
        auto keys_ptr = trx->vm_temp_alloc<Object>(count);
        auto keys_offset = static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(keys_ptr) - trx->m_vm_base);
        auto elems_ptr = trx->vm_temp_alloc<Object>(count);
        auto elems_offset = static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(elems_ptr) - trx->m_vm_base);

        // initialize keys to null
        for (length_t i = 0; i < count; ++i) {
            keys_ptr[i] = Object::make_null();
        }

        // pop first element and store in elems[0]
        auto src_obj = *container_ptr;
        auto first_obj = src_obj.arrays_pop_clone_head(trx);
        elems_ptr[0] = first_obj;

        // exec stack frame: [saved-global] [saved-temp] [proc] [src] [keys-arr] [elems-arr] [index=0] [@control] [proc]
        // saved-global rides the bottom so the existing index-relative offsets stay put.
        trx->require_exec_capacity(9);
        auto saved_proc_obj = *proc_ptr;
        auto keys_obj = Object::make_array(keys_offset, count);
        auto elems_obj = Object::make_array(elems_offset, count);
        auto temp_offset = static_cast<integer_t>(saved_temp - trx->m_vm_base);

        *++trx->m_exec_ptr = Object::make_boolean(saved_global);
        *++trx->m_exec_ptr = Object::make_integer(temp_offset);
        *++trx->m_exec_ptr = saved_proc_obj;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = keys_obj;
        *++trx->m_exec_ptr = elems_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);  // index
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayGroupBy);
        *++trx->m_exec_ptr = saved_proc_obj;

        // push first element clone to operand stack for proc evaluation
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = first_obj.make_clone(trx);
    }
}

// @array-find: (exec stack: proc src-arr saved-elem)
// Tests proc result; returns saved element on true, continues on false.
// throws: vm-full, execstack-overflow, opstack-overflow
static void at_array_find_op(Trix *trx) {
    auto saved_ptr = trx->m_exec_ptr;
    auto src_ptr = (saved_ptr - 1);
    auto proc_ptr = (saved_ptr - 2);

    trx->verify_operands(VerifyBoolean);
    auto found = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (found) {
        // found: push saved element + true
        trx->require_op_capacity(2);
        *++trx->m_op_ptr = saved_ptr->make_clone(trx);
        *++trx->m_op_ptr = Object::make_boolean(true);
        saved_ptr->maybe_free_extvalue(trx);
        trx->m_exec_ptr -= 3;
    } else {
        saved_ptr->maybe_free_extvalue(trx);

        if (src_ptr->arrays_length() == 0) {
            // not found: push false
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = Object::make_boolean(false);
            trx->m_exec_ptr -= 3;
        } else {
            // more elements: save next, push to op stack, re-push frame + proc
            trx->require_op_capacity(1);
            trx->require_exec_capacity(2);

            auto elem_obj = src_ptr->arrays_pop_clone_head(trx);
            *saved_ptr = elem_obj;
            *++trx->m_op_ptr = elem_obj.make_clone(trx);

            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayFind);
            *++trx->m_exec_ptr = *proc_ptr;
        }
    }
}

// find: arr proc :- any true | false
// Returns the first element for which proc returns true.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void find_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto src_length = container_ptr->arrays_length();

    if (src_length == 0) {
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = Object::make_boolean(false);
    } else {
        auto src_obj = *container_ptr;
        auto first_obj = src_obj.arrays_pop_clone_head(trx);

        // frame: [proc] [src] [saved-element] [@array-find] [proc]
        trx->require_exec_capacity(5);
        *++trx->m_exec_ptr = *proc_ptr;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = first_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayFind);
        *++trx->m_exec_ptr = *proc_ptr;

        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = first_obj.make_clone(trx);
    }
}

// @array-for-all: (exec stack: proc arr saved-depth)
// Verifies that the body returned the operand stack to the depth
// it had BEFORE the iteration's value was pushed; mismatch raises
// /range-check.  Then pops next element from arr, pushes it to op
// stack, and re-pushes the frame.
// throws: range-check, vm-full, execstack-overflow, opstack-overflow
static void at_array_forall_op(Trix *trx) {
    auto saved_depth_ptr = trx->m_exec_ptr;
    auto arr_ptr = saved_depth_ptr - 1;
    auto proc_ptr = arr_ptr - 1;
    auto saved_depth = saved_depth_ptr->integer_value();
    auto current_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);
    if (current_depth != saved_depth) {
        trx->error(Error::RangeCheck,
                   "for-all: body left {} item(s) on the operand stack (expected stack effect 'value -- ')",
                   current_depth - saved_depth);
    }
    if (arr_ptr->arrays_length() == 0) {
        trx->m_exec_ptr -= 3;
    } else {
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        *++trx->m_op_ptr = arr_ptr->array_pop_clone_head(trx);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayForAll);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// @array-map: (exec stack: proc src-arr dst-arr write-idx)
// Collects proc result into dst, pushes next element, re-pushes frame.
// throws: vm-full, execstack-overflow, opstack-overflow
static void at_array_map_op(Trix *trx) {
    auto index_ptr = trx->m_exec_ptr;
    auto dst_ptr = (index_ptr - 1);
    auto src_ptr = (index_ptr - 2);
    auto proc_ptr = (index_ptr - 3);

    // collect the result from the proc that just ran
    trx->require_op_count(1);
    auto write_index = index_ptr->integer_value();
    auto dst_data = dst_ptr->array_objects(trx);
    dst_data[write_index] = *trx->m_op_ptr--;
    index_ptr->update_integer(write_index + 1);

    if (src_ptr->arrays_length() == 0) {
        // done: push dst array to operand stack, clean up frame
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = *dst_ptr;
        trx->m_exec_ptr -= 4;
    } else {
        // more elements: push next element, re-push frame + proc
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        auto elem_obj = src_ptr->arrays_pop_clone_head(trx);
        *++trx->m_op_ptr = elem_obj;

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayMap);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// @array-map-indexed: (exec stack: proc src dst write-index)
// Collects proc result, advances index, pushes next elem+index or finishes.
// throws: vm-full, execstack-overflow, opstack-overflow
static void at_array_map_indexed_op(Trix *trx) {
    auto index_ptr = trx->m_exec_ptr;
    auto dst_ptr = (index_ptr - 1);
    auto src_ptr = (index_ptr - 2);
    auto proc_ptr = (index_ptr - 3);

    trx->require_op_count(1);
    auto write_index = index_ptr->integer_value();
    auto dst_data = dst_ptr->array_objects(trx);
    dst_data[write_index] = *trx->m_op_ptr--;
    index_ptr->update_integer(write_index + 1);

    if (src_ptr->arrays_length() == 0) {
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = *dst_ptr;
        trx->m_exec_ptr -= 4;
    } else {
        trx->require_op_capacity(2);
        trx->require_exec_capacity(2);

        auto elem_obj = src_ptr->arrays_pop_clone_head(trx);
        *++trx->m_op_ptr = elem_obj;
        *++trx->m_op_ptr = Object::make_integer(write_index + 1);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayMapIndexed);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// @array-filter: (exec stack: proc src-arr dst-arr write-idx saved-elem)
// Keeps or discards saved element based on bool result, continues.
// throws: vm-full, execstack-overflow, opstack-overflow
static void at_array_filter_op(Trix *trx) {
    auto saved_ptr = trx->m_exec_ptr;
    auto index_ptr = (saved_ptr - 1);
    auto dst_ptr = (saved_ptr - 2);
    auto src_ptr = (saved_ptr - 3);
    auto proc_ptr = (saved_ptr - 4);

    // collect the boolean result from the proc that just ran
    trx->verify_operands(VerifyBoolean);
    auto keep = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (keep) {
        auto write_index = index_ptr->integer_value();
        auto dst_data = dst_ptr->array_objects(trx);
        dst_data[write_index] = saved_ptr->make_clone(trx);
        index_ptr->update_integer(write_index + 1);
    }
    saved_ptr->maybe_free_extvalue(trx);

    if (src_ptr->arrays_length() == 0) {
        // done: push dst array with trimmed length to operand stack
        trx->require_op_capacity(1);
        auto write_index = index_ptr->integer_value();
        auto max_length = dst_ptr->arrays_length();
        dst_ptr->set_array_length(static_cast<length_t>(write_index));

        // reclaim unused destination slots if array is at top of heap
        auto unused = static_cast<vm_size_t>(max_length - write_index) * vm_sizeof<Object>();
        if (unused != 0) {
            auto dst_end = reinterpret_cast<vm_t *>(dst_ptr->array_objects(trx) + max_length);
            trx->vm_trim_alloc(dst_end, unused);
        }

        *++trx->m_op_ptr = *dst_ptr;
        trx->m_exec_ptr -= 5;
    } else {
        // more elements: save next, push to op stack, re-push frame + proc
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        auto elem_obj = src_ptr->arrays_pop_clone_head(trx);
        *saved_ptr = elem_obj;
        *++trx->m_op_ptr = elem_obj.make_clone(trx);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayFilter);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// @array-take-while: (exec stack: proc src dst-array write-index saved-elem)
// Collects elements while predicate returns true; stops at first false.
// throws: vm-full, execstack-overflow, opstack-overflow, type-check
static void at_array_takewhile_op(Trix *trx) {
    auto saved_ptr = trx->m_exec_ptr;
    auto index_ptr = (saved_ptr - 1);
    auto dst_ptr = (saved_ptr - 2);
    auto src_ptr = (saved_ptr - 3);
    auto proc_ptr = (saved_ptr - 4);

    // collect the boolean result from the proc that just ran
    trx->verify_operands(VerifyBoolean);
    auto keep = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (keep) {
        // predicate true: store element in destination
        auto write_index = index_ptr->integer_value();
        auto dst_data = dst_ptr->array_objects(trx);
        dst_data[write_index] = saved_ptr->make_clone(trx);
        index_ptr->update_integer(write_index + 1);
        saved_ptr->maybe_free_extvalue(trx);

        if (src_ptr->arrays_length() == 0) {
            // source exhausted: push trimmed result
            trx->require_op_capacity(1);
            auto final_len = static_cast<length_t>(write_index + 1);
            auto max_length = dst_ptr->arrays_length();
            dst_ptr->set_array_length(final_len);

            auto unused = static_cast<vm_size_t>((max_length - final_len) * vm_sizeof<Object>());
            if (unused != 0) {
                auto dst_end = reinterpret_cast<vm_t *>(dst_ptr->array_objects(trx) + max_length);
                trx->vm_trim_alloc(dst_end, unused);
            }

            *++trx->m_op_ptr = *dst_ptr;
            trx->m_exec_ptr -= 5;
        } else {
            // more elements: pop next, save, push clone for proc
            trx->require_op_capacity(1);
            trx->require_exec_capacity(2);

            auto elem_obj = src_ptr->arrays_pop_clone_head(trx);
            *saved_ptr = elem_obj;
            *++trx->m_op_ptr = elem_obj.make_clone(trx);

            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayTakeWhile);
            *++trx->m_exec_ptr = *proc_ptr;
        }
    } else {
        // predicate false: stop collecting, free saved element, push trimmed result
        saved_ptr->maybe_free_extvalue(trx);

        // free remaining source elements if unpacked
        // (packed elements are not owned, no free needed)

        trx->require_op_capacity(1);
        auto write_index = index_ptr->integer_value();
        auto max_length = dst_ptr->arrays_length();
        dst_ptr->set_array_length(static_cast<length_t>(write_index));

        auto unused = static_cast<vm_size_t>(max_length - write_index) * vm_sizeof<Object>();
        if (unused != 0) {
            auto dst_end = reinterpret_cast<vm_t *>(dst_ptr->array_objects(trx) + max_length);
            trx->vm_trim_alloc(dst_end, unused);
        }

        *++trx->m_op_ptr = *dst_ptr;
        trx->m_exec_ptr -= 5;
    }
}

// take-while: arr proc :- arr'
// Returns the longest prefix of elements for which proc returns true.
// throws: vm-full, opstack-underflow, type-check
static void takewhile_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto src_length = container_ptr->arrays_length();

    if (src_length == 0) {
        // empty: return empty array
        auto [dst_ptr, offset] = trx->vm_alloc_dispatch_n<Object>(0, ChunkKind::Array);
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = Object::make_array(offset, 0);
    } else {
        // allocate destination array (max possible size).  Once parked on the exec
        // stack the result stays marked across the per-element proc callbacks; but the
        // first pop-clone below fires a GC BEFORE that park, so root dst across it (under
        // ${...} dst is a global block that would otherwise be swept as unreachable).
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(src_length, ChunkKind::Array);
        for (length_t i = 0; i < src_length; ++i) {
            dst_ptr[i] = Object::make_null();
        }
        auto dst_obj = Object::make_array(dst_offset, src_length);

        trx->gc_root_push_oneoff(dst_obj);
        auto src_obj = *container_ptr;

        // pop first element from source
        auto first_obj = src_obj.arrays_pop_clone_head(trx);

        // exec stack frame: [proc] [src] [dst] [write-index] [saved-element] [@array-take-while] [proc]
        trx->require_exec_capacity(7);
        *++trx->m_exec_ptr = *proc_ptr;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = dst_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = first_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayTakeWhile);
        *++trx->m_exec_ptr = *proc_ptr;
        trx->gc_root_pop_oneoff();

        // push first element clone to operand stack (proc will test it)
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = first_obj.make_clone(trx);
    }
}

// @array-drop-while: (exec stack: proc src saved-elem)
// During dropping phase, discards elements while predicate is true.
// When predicate returns false, copies current + remaining elements into result.
// throws: vm-full, execstack-overflow, opstack-overflow, type-check
static void at_array_dropwhile_op(Trix *trx) {
    auto saved_ptr = trx->m_exec_ptr;
    auto src_ptr = (saved_ptr - 1);
    auto proc_ptr = (saved_ptr - 2);

    // collect the boolean result from the proc that just ran
    trx->verify_operands(VerifyBoolean);
    auto pred_true = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (pred_true) {
        // predicate true: still dropping -- discard this element
        saved_ptr->maybe_free_extvalue(trx);

        if (src_ptr->arrays_length() == 0) {
            // source exhausted while still dropping: return empty array
            trx->require_op_capacity(1);
            auto [ptr, offset] = trx->vm_alloc_dispatch_n<Object>(0, ChunkKind::Array);
            *++trx->m_op_ptr = Object::make_array(offset, 0);
            trx->m_exec_ptr -= 3;
        } else {
            // more elements: pop next, save, push clone for proc
            trx->require_op_capacity(1);
            trx->require_exec_capacity(2);

            auto elem_obj = src_ptr->arrays_pop_clone_head(trx);
            *saved_ptr = elem_obj;
            *++trx->m_op_ptr = elem_obj.make_clone(trx);

            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayDropWhile);
            *++trx->m_exec_ptr = *proc_ptr;
        }
    } else {
        // predicate false: stop dropping -- collect this element + all remaining
        auto remaining = src_ptr->arrays_length();
        auto result_len = static_cast<length_t>(remaining + 1);
        auto [result_ptr, result_offset] = trx->vm_alloc_dispatch_n<Object>(result_len, ChunkKind::Array);
        // Null and root the result across the element clones below: under ${...}
        // each make_clone is a global ExtValue alloc that fires the GC, and the
        // (local) result array is not yet on any stack.
        std::fill_n(result_ptr, result_len, Object::make_null());
        auto result_obj = Object::make_array(result_offset, result_len);
        trx->gc_root_push_oneoff(result_obj);

        // first element is the saved one that failed the predicate
        result_ptr[0] = saved_ptr->make_clone(trx);
        saved_ptr->maybe_free_extvalue(trx);

        // copy remaining elements
        if (src_ptr->is_array()) {
            auto src_data = src_ptr->array_objects(trx);
            for (length_t i = 0; i < remaining; ++i) {
                result_ptr[i + 1] = src_data[i].make_clone(trx);
            }
        } else {
            // packed: extract each element -- clone for ownership
            auto src_copy_obj = *src_ptr;
            for (length_t i = 0; i < remaining; ++i) {
                result_ptr[i + 1] = src_copy_obj.packed_pop_clone_head(trx);
            }
        }
        trx->gc_root_pop_oneoff();

        trx->require_op_capacity(1);
        *++trx->m_op_ptr = result_obj;
        trx->m_exec_ptr -= 3;
    }
}

// drop-while: arr proc :- arr'
// Drops the longest prefix of elements for which proc returns true, returns the rest.
// throws: vm-full, opstack-underflow, type-check
static void dropwhile_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto src_length = container_ptr->arrays_length();

    if (src_length == 0) {
        // empty: return empty array
        auto [ptr, offset] = trx->vm_alloc_dispatch_n<Object>(0, ChunkKind::Array);
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = Object::make_array(offset, 0);
    } else {
        auto src_obj = *container_ptr;

        // pop first element from source
        auto first_obj = src_obj.arrays_pop_clone_head(trx);

        // exec stack frame: [proc] [src] [saved-element] [@array-drop-while] [proc]
        trx->require_exec_capacity(5);
        *++trx->m_exec_ptr = *proc_ptr;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = first_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayDropWhile);
        *++trx->m_exec_ptr = *proc_ptr;

        // push first element clone to operand stack (proc will test it)
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = first_obj.make_clone(trx);
    }
}

// @array-partition: (exec stack: proc src dst-pass pass-idx dst-fail fail-idx saved-elem)
// Routes saved element to pass or fail array based on bool result, continues.
// throws: vm-full, execstack-overflow, opstack-overflow
static void at_array_partition_op(Trix *trx) {
    auto saved_ptr = trx->m_exec_ptr;
    auto fail_index_ptr = (saved_ptr - 1);
    auto fail_dst_ptr = (saved_ptr - 2);
    auto pass_index_ptr = (saved_ptr - 3);
    auto pass_dst_ptr = (saved_ptr - 4);
    auto src_ptr = (saved_ptr - 5);
    auto proc_ptr = (saved_ptr - 6);
    auto temp_ptr = (saved_ptr - 7);
    auto flag_ptr = (saved_ptr - 8);  // saved m_curr_alloc_global (see partition_op)

    // collect the boolean result from the proc that just ran
    trx->verify_operands(VerifyBoolean);
    auto keep = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (keep) {
        auto write_index = pass_index_ptr->integer_value();
        auto dst_data = pass_dst_ptr->array_objects(trx);
        dst_data[write_index] = saved_ptr->make_clone(trx);
        pass_index_ptr->update_integer(write_index + 1);
    } else {
        auto write_index = fail_index_ptr->integer_value();
        auto dst_data = fail_dst_ptr->array_objects(trx);
        dst_data[write_index] = saved_ptr->make_clone(trx);
        fail_index_ptr->update_integer(write_index + 1);
    }

    if (src_ptr->arrays_length() == 0) {
        // done: allocate exact-sized result arrays and copy from temp buffers
        auto pass_count = static_cast<length_t>(pass_index_ptr->integer_value());
        auto fail_count = static_cast<length_t>(fail_index_ptr->integer_value());

        // allocate exact pass result and transfer elements (bitwise copy moves ExtValue ownership)
        auto [result_pass, result_pass_offset] = trx->vm_alloc_n<Object>(pass_count);
        std::copy_n(pass_dst_ptr->array_objects(trx), pass_count, result_pass);

        // allocate exact fail result
        auto [result_fail, result_fail_offset] = trx->vm_alloc_n<Object>(fail_count);
        std::copy_n(fail_dst_ptr->array_objects(trx), fail_count, result_fail);

        // free last saved element's ExtValue (exec stack slot is about to be popped)
        saved_ptr->maybe_free_extvalue(trx);

        // reclaim temp buffers
        auto saved_temp = trx->m_vm_base + temp_ptr->integer_value();
        trx->vm_temp_restore(saved_temp);

        // restore the allocation region suppressed by partition_op, push results, drop frame
        trx->m_curr_alloc_global = flag_ptr->boolean_value();
        trx->require_op_capacity(2);
        *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(result_pass_offset, pass_count));
        *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(result_fail_offset, fail_count));
        trx->m_exec_ptr -= 9;
    } else {
        // more elements: save next, push to op stack, re-push frame + proc
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        auto elem_obj = src_ptr->arrays_pop_clone_head(trx);
        saved_ptr->maybe_free_extvalue(trx);  // free previous element's ExtValue before overwrite
        *saved_ptr = elem_obj;
        *++trx->m_op_ptr = elem_obj.make_clone(trx);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayPartition);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// partition: arr proc :- arr-pass arr-fail
// Splits array into two arrays: elements where proc returns true, and the rest.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void partition_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto src_length = container_ptr->arrays_length();

    if (src_length == 0) {
        // empty: return two empty arrays
        auto [pass_ptr, pass_offset] = trx->vm_alloc_n<Object>(0);
        auto [fail_ptr, fail_offset] = trx->vm_alloc_n<Object>(0);
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(pass_offset, 0));
        *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(fail_offset, 0));
    } else {
        // Run the whole two-phase collect-and-split with LOCAL allocation, even under
        // ${...}: the pass/fail temp arrays live across every proc invocation, and a
        // GLOBAL allocation (proc/element-clone) grows the global region DOWN over
        // them (gvm_alloc bounds-checks only the local heap top).  Suppressing the
        // flag keeps the proc + all clones local -- as the top-level path runs.  The
        // saved flag rides the bottom of the exec frame; at_array_partition_op
        // restores it.  (Error-unwind: restored by the enclosing ${...} barrier.)
        auto saved_global = trx->m_curr_alloc_global;
        trx->m_curr_alloc_global = false;

        // allocate two temp buffers (reclaimed by vm_temp_restore in finalization)
        auto saved_temp = trx->m_vm_temp_ptr;
        auto pass_ptr = trx->vm_temp_alloc<Object>(src_length);
        auto pass_offset = static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(pass_ptr) - trx->m_vm_base);
        std::fill_n(pass_ptr, src_length, Object::make_null());
        auto fail_ptr = trx->vm_temp_alloc<Object>(src_length);
        auto fail_offset = static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(fail_ptr) - trx->m_vm_base);
        std::fill_n(fail_ptr, src_length, Object::make_null());

        auto src_obj = *container_ptr;

        // pop first element from source
        auto first_obj = src_obj.arrays_pop_clone_head(trx);

        auto temp_offset = static_cast<integer_t>(saved_temp - trx->m_vm_base);

        // exec stack frame: [saved-global] [saved-temp] [proc] [src] [dst-pass] [pass-idx] [dst-fail] [fail-idx]
        // [saved-elem] [@array-partition] [proc].  saved-global rides the bottom so the
        // existing saved_ptr-relative offsets stay put.
        trx->require_exec_capacity(11);
        *++trx->m_exec_ptr = Object::make_boolean(saved_global);
        *++trx->m_exec_ptr = Object::make_integer(temp_offset);
        *++trx->m_exec_ptr = *proc_ptr;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = Object::make_array(pass_offset, src_length);
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = Object::make_array(fail_offset, src_length);
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = first_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayPartition);
        *++trx->m_exec_ptr = *proc_ptr;

        // push first element clone to operand stack (proc will test it)
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = first_obj.make_clone(trx);
    }
}

// @packed-for-all: (exec stack: proc pk saved-depth)
// See @array-for-all for the body stack-effect contract.
// throws: range-check, vm-full, execstack-overflow, opstack-overflow
static void at_packed_forall_op(Trix *trx) {
    auto saved_depth_ptr = trx->m_exec_ptr;
    auto packed_ptr = saved_depth_ptr - 1;
    auto saved_depth = saved_depth_ptr->integer_value();
    auto current_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);
    if (current_depth != saved_depth) {
        trx->error(Error::RangeCheck,
                   "for-all: body left {} item(s) on the operand stack (expected stack effect 'value -- ')",
                   current_depth - saved_depth);
    }
    if (packed_ptr->arrays_length() == 0) {
        trx->m_exec_ptr -= 3;
    } else {
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        *++trx->m_op_ptr = packed_ptr->packed_pop_clone_head(trx);

        auto proc_obj = packed_ptr[-1];
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPackedForAll);
        *++trx->m_exec_ptr = proc_obj;
    }
}

// @string-for-all: (exec stack: proc str saved-depth)
// See @array-for-all for the body stack-effect contract.
// throws: range-check, execstack-overflow, opstack-overflow
static void at_string_forall_op(Trix *trx) {
    auto saved_depth_ptr = trx->m_exec_ptr;
    auto string_ptr = saved_depth_ptr - 1;
    auto saved_depth = saved_depth_ptr->integer_value();
    auto current_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);
    if (current_depth != saved_depth) {
        trx->error(Error::RangeCheck,
                   "for-all: body left {} item(s) on the operand stack (expected stack effect 'byte -- ')",
                   current_depth - saved_depth);
    }
    if (string_ptr->string_length() == 0) {
        trx->m_exec_ptr -= 3;
    } else {
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        *++trx->m_op_ptr = string_ptr->string_pop_head(trx);

        auto proc_obj = string_ptr[-1];
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atStringForAll);
        *++trx->m_exec_ptr = proc_obj;
    }
}

// @dict-for-all: (exec stack: proc dict bucket-idx offset saved-depth)
// Verifies that the body returned the operand stack to the depth
// it had BEFORE the iteration's key/value pair was pushed; mismatch
// raises /range-check.
// throws: range-check, vm-full, execstack-overflow, opstack-overflow
static void at_dict_forall_op(Trix *trx) {
    auto saved_depth_ptr = trx->m_exec_ptr;
    auto offset_ptr = (saved_depth_ptr - 1);
    auto bucket_ptr = (offset_ptr - 1);
    auto dict_ptr = (offset_ptr - 2);

    auto saved_depth = saved_depth_ptr->integer_value();
    auto current_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);
    if (current_depth != saved_depth) {
        trx->error(Error::RangeCheck,
                   "for-all: body left {} item(s) on the operand stack (expected stack effect 'key value -- ')",
                   current_depth - saved_depth);
    }

    auto entry_offset = vm_offset_t{offset_ptr->uinteger_value()};
    auto bucket_idx = bucket_ptr->integer_value();
    auto dict = dict_ptr->dict_value(trx);
    auto [next_offset, next_idx, key, value] = dict->next(trx, entry_offset, bucket_idx);
    if (next_offset != nulloffset) {
        trx->require_op_capacity(2);
        trx->require_exec_capacity(2);

        offset_ptr->update_uinteger(static_cast<uinteger_t>(next_offset));
        bucket_ptr->update_integer(next_idx);

        *++trx->m_op_ptr = key.make_clone(trx);
        *++trx->m_op_ptr = value.make_clone(trx);

        auto proc_obj = dict_ptr[-1];
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDictForAll);
        *++trx->m_exec_ptr = proc_obj;
    } else {
        trx->m_exec_ptr -= 5;
    }
}

// Region-aware result-dict allocation for map-dict / filter-dict.  Mirrors
// create_dict (ops_dict.inl): under ${...} (m_curr_alloc_global) the produced dict
// lands in global VM and survives save/restore like the `dict` / `dynamic-dict`
// constructors, instead of being unconditionally local (a LOCAL dict held under a
// GLOBAL context -- a region inconsistency).  No extra GC-rooting is needed at the
// call sites: the only steps between this allocation and pushing the result onto a
// GC-scanned stack (Dict::next, require_exec_capacity) do not themselves allocate,
// so the fresh dict cannot be swept before it is rooted on the op/exec stack.
[[nodiscard]] static std::pair<Dict *, vm_offset_t> create_result_dict(Trix *trx, length_t capacity, Object::DictMode mode) {
    return (trx->m_curr_alloc_global ? Dict::create_global_dict(trx, capacity, mode) : Dict::create_dict(trx, capacity, mode));
}

// Set twin of create_result_dict: routes a set result to global VM under ${...}
// (set-filter, set-from-array) so it survives save/restore like the `set`
// constructor.  Same rooting caveat as create_result_dict: if the caller clones
// elements into the set in a fill loop (rather than parking it on a GC-scanned
// stack first), it must root the set across the fill.
[[nodiscard]] static std::pair<Dict *, vm_offset_t> create_result_set(Trix *trx, length_t capacity, Object::DictMode mode) {
    return (trx->m_curr_alloc_global ? Dict::create_global_set(trx, capacity, mode) : Dict::create_set(trx, capacity, mode));
}

// @dict-map: (exec stack: proc result-dict src-dict entry-offset bucket-idx saved-key)
// Callback for map-dict. Stores proc result as value for saved-key in result dict,
// then advances to next entry.
// throws: vm-full, execstack-overflow, opstack-overflow
static void at_dict_map_op(Trix *trx) {
    auto key_ptr = trx->m_exec_ptr;
    auto bucket_ptr = (key_ptr - 1);
    auto offset_ptr = (key_ptr - 2);
    auto src_dict_ptr = (key_ptr - 3);
    auto result_dict_ptr = (key_ptr - 4);
    auto proc_ptr = (key_ptr - 5);

    // store key -> proc-result in the result dict.  Clone the saved key BEFORE
    // popping the proc's result value: under ${...} make_clone allocates the key
    // clone globally and can fire vm_global_gc, and the proc result is only kept
    // alive by the op stack (a GC root).  Popping it first and then cloning the
    // key would leave the result value an unrooted global temp that the key
    // clone's GC sweeps -> a poisoned value lands in the result dict.  key_ptr is
    // exec-parked and result_dict is pre-sized, so the put itself never allocates.
    trx->require_op_count(1);
    auto result_dict = result_dict_ptr->dict_value(trx);
    auto key_clone = key_ptr->make_clone(trx);
    auto new_value_obj = *trx->m_op_ptr;
    --trx->m_op_ptr;
    result_dict->put(trx, key_clone, new_value_obj);

    // advance to next entry
    auto entry_offset = vm_offset_t{offset_ptr->uinteger_value()};
    auto bucket_idx = bucket_ptr->integer_value();
    auto src_dict = src_dict_ptr->dict_value(trx);
    auto [next_offset, next_idx, key, value] = src_dict->next(trx, entry_offset, bucket_idx);

    if (next_offset != nulloffset) {
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        offset_ptr->update_uinteger(static_cast<uinteger_t>(next_offset));
        bucket_ptr->update_integer(next_idx);
        *key_ptr = key;

        // push value clone to operand stack for proc
        *++trx->m_op_ptr = value.make_clone(trx);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDictMap);
        *++trx->m_exec_ptr = *proc_ptr;
    } else {
        // done: push result dict to operand stack
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = *result_dict_ptr;
        trx->m_exec_ptr -= 6;
    }
}

// map-dict: dict proc :- dict'
// Applies proc to each value in the dict, producing a new dict with
// the same keys mapped to the proc results.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void mapdict_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyDict);

    auto proc_ptr = trx->m_op_ptr;
    auto dict_ptr = (proc_ptr - 1);
    auto src_dict = dict_ptr->dict_value(trx);
    auto count = src_dict->length();

    if (count == 0) {
        // empty dict: return empty dynamic dict
        auto [result, offset] = create_result_dict(trx, 0, Object::DictMode::ReadWriteDynamic);
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = Object::make_dict(offset);
    } else {
        // create result dict
        auto [result_dict, result_offset] = create_result_dict(trx, count, Object::DictMode::ReadWriteDynamic);

        // get first entry
        auto [first_offset, first_idx, first_key, first_value] = src_dict->next(trx, nulloffset, -1);

        // exec stack frame: [proc] [result-dict] [src-dict] [entry-offset] [bucket-idx] [saved-key] [@dict-map] [proc]
        trx->require_exec_capacity(8);
        auto saved_proc_obj = *proc_ptr;
        *++trx->m_exec_ptr = saved_proc_obj;
        *++trx->m_exec_ptr = Object::make_dict(result_offset);
        *++trx->m_exec_ptr = *dict_ptr;
        *++trx->m_exec_ptr = Object::make_uinteger(static_cast<uinteger_t>(first_offset));
        *++trx->m_exec_ptr = Object::make_integer(first_idx);
        *++trx->m_exec_ptr = first_key;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDictMap);
        *++trx->m_exec_ptr = saved_proc_obj;

        // push first value clone to operand stack for proc
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = first_value.make_clone(trx);
    }
}

// @dict-filter callback
// exec stack frame: [proc] [result-dict] [src-dict] [entry-offset] [bucket-idx] [saved-key] [saved-value] [@dict-filter]
// Collects entries where proc returns true.
// throws: execstack-overflow, opstack-overflow, vm-full
static void at_dict_filter_op(Trix *trx) {
    auto value_ptr = trx->m_exec_ptr;
    auto key_ptr = (value_ptr - 1);
    auto bucket_ptr = (key_ptr - 1);
    auto offset_ptr = (bucket_ptr - 1);
    auto src_dict_ptr = (offset_ptr - 1);
    auto result_dict_ptr = (src_dict_ptr - 1);
    auto proc_ptr = (result_dict_ptr - 1);

    // collect the boolean result from the proc
    trx->verify_operands(VerifyBoolean);
    auto keep = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (keep) {
        auto result_dict = result_dict_ptr->dict_value(trx);
        // Clone key and value into the result.  make_clone allocates each clone
        // GLOBALLY under ${...} and can fire vm_global_gc, so the first clone must
        // be rooted across the second clone's allocation -- otherwise whichever
        // clone the compiler evaluates first is an unrooted global temp that the
        // second clone's GC sweeps (a poisoned key/value would reach the result).
        // result_dict is exec-parked (rooted) and pre-sized, so put never expands.
        auto key_clone = key_ptr->make_clone(trx);
        trx->gc_root_push_oneoff(key_clone);
        auto value_clone = value_ptr->make_clone(trx);
        trx->gc_root_pop_oneoff();
        result_dict->put(trx, key_clone, value_clone);
    }
    // saved key/value remain BORROWED aliases of the source-dict entries (the result
    // got its own clones above); the source owns them, so this iterator must NOT free
    // them -- freeing a borrowed ExtValue/WideValue key corrupts the source (the
    // @set-filter UAF this whole family was audited for).

    // advance to next entry
    auto entry_offset = vm_offset_t{offset_ptr->uinteger_value()};
    auto bucket_idx = bucket_ptr->integer_value();
    auto src_dict = src_dict_ptr->dict_value(trx);
    auto [next_offset, next_idx, key, value] = src_dict->next(trx, entry_offset, bucket_idx);

    if (next_offset != nulloffset) {
        trx->require_op_capacity(2);
        trx->require_exec_capacity(2);

        offset_ptr->update_uinteger(static_cast<uinteger_t>(next_offset));
        bucket_ptr->update_integer(next_idx);
        *key_ptr = key;
        *value_ptr = value;

        // push key and value clones to operand stack for proc
        *++trx->m_op_ptr = key.make_clone(trx);
        *++trx->m_op_ptr = value.make_clone(trx);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDictFilter);
        *++trx->m_exec_ptr = *proc_ptr;
    } else {
        // done: push result dict to operand stack
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = *result_dict_ptr;
        trx->m_exec_ptr -= 7;
    }
}

// filter-dict: dict proc :- dict'
// Returns a new dict containing only entries where proc returns true.
// proc receives: key value -- bool
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void filterdict_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyDict);

    auto proc_ptr = trx->m_op_ptr;
    auto dict_ptr = (proc_ptr - 1);
    auto src_dict = dict_ptr->dict_value(trx);
    auto count = src_dict->length();

    if (count == 0) {
        // empty dict: return empty dynamic dict
        auto [result, offset] = create_result_dict(trx, 0, Object::DictMode::ReadWriteDynamic);
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = Object::make_dict(offset);
    } else {
        // create result dict
        auto [result_dict, result_offset] = create_result_dict(trx, count, Object::DictMode::ReadWriteDynamic);

        // get first entry
        auto [first_offset, first_idx, first_key, first_value] = src_dict->next(trx, nulloffset, -1);

        // exec stack frame: [proc] [result-dict] [src-dict] [entry-offset] [bucket-idx] [saved-key] [saved-value]
        // [@dict-filter] [proc]
        trx->require_exec_capacity(9);
        auto saved_proc_obj = *proc_ptr;
        *++trx->m_exec_ptr = saved_proc_obj;
        *++trx->m_exec_ptr = Object::make_dict(result_offset);
        *++trx->m_exec_ptr = *dict_ptr;
        *++trx->m_exec_ptr = Object::make_uinteger(static_cast<uinteger_t>(first_offset));
        *++trx->m_exec_ptr = Object::make_integer(first_idx);
        *++trx->m_exec_ptr = first_key;
        *++trx->m_exec_ptr = first_value;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDictFilter);
        *++trx->m_exec_ptr = saved_proc_obj;

        // push first key and value clones to operand stack for proc
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = first_key.make_clone(trx);
        *++trx->m_op_ptr = first_value.make_clone(trx);
    }
}
