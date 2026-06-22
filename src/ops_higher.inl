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

//===--- Higher-Order, Functional, and Module Operators ---===//
//
// A collection of operators that don't belong to a single subsystem but
// share a common theme: they treat code as data (higher-order programming)
// or provide language infrastructure (modules, error inspection, creation
// helpers).  Based on concepts from:
//
//   Haskell / ML: higher-order functions (map, filter, fold), partial
//   application (curry), function composition, and thunks for lazy
//   evaluation.
//
//   PostScript: the `exec` operator (run a proc from the stack), `token`
//   (scan one value from a string), and the mark-based array/string
//   construction pattern ([ ... ] and ( ... )).
//
//   Standard ML modules / OCaml modules: named, encapsulated namespaces
//   with explicit import/export.
//
// --- Contents by category ---
//
// CURRY AND COMPOSITION (partial application, function pipelines)
//   curry          -- capture a value + callable as a single callable
//   uncurry        -- extract the captured value and callable
//   compose        -- chain two callables: `f g compose` means "run f then g"
//
//   Curry representation: 2 Objects [value, callable] on the VM heap.
//   When executed, the value is pushed (or executed if marked executable,
//   as in compose), then the callable is executed.  This is the standard
//   partial-application pattern from functional programming.
//
// THUNKS (lazy evaluation, memoization)
//   thunk              -- wrap a proc for deferred evaluation
//   force              -- evaluate a thunk (or return cached result)
//   thunk-evaluated?   -- test if already forced
//   thunk-reset        -- clear cached result, allow re-evaluation
//
//   Thunk representation: 3 Objects [state, proc, result] on the VM heap.
//   State is Unevaluated (0), Evaluating (1), or Evaluated (2).  Circular
//   force detection (state == Evaluating when force is called) raises an
//   error.  Thunks are the building block for lazy sequences (ops_lazy.inl)
//   and computed reactive cells (ops_reactive.inl).
//
// HIGHER-ORDER CONTAINER OPERATIONS (eager, on arrays)
//   map, map-indexed   -- transform elements
//   flat-map           -- transform and flatten one level
//   filter             -- keep elements matching predicate
//   reduce             -- fold to single value
//   sum                -- numeric sum of elements
//   min-of, max-of     -- minimum/maximum element
//   unique             -- remove duplicates (preserving first-occurrence order)
//   dedupe             -- remove consecutive (adjacent) duplicate elements
//   swap-at            -- swap two elements by index
//
//   (sort-by and group-by share this theme but are implemented in
//   ops_array_iteration.inl.)
//
//   These are the eager (immediate, array-in/array-out) counterparts to
//   the lazy sequence transformers in ops_lazy.inl.  Use these for small
//   collections; use lazy sequences for large or infinite data.
//
// MARK-BASED CONSTRUCTORS (PostScript heritage)
//   readonly-array-from-mark    -- [ ... ] into read-only array
//   =array-from-mark            -- [ ... ] into =array
//   readonly-=array-from-mark   -- [ ... ] into read-only =array
//
//   These consume all operands above a mark and construct a composite
//   object.  The mark pattern is Trix's primary mechanism for variadic
//   argument passing.
//
// ERROR INSPECTION AND CONTROL
//   try-result       -- run a proc, wrapping its result in /ok or the caught error in /err
//
//   (last-error / last-error-data / last-error-message / throw-with share this
//   theme but are implemented in ops_system.inl and ops_flow.inl.)
//
// MODULES (namespace management)
//   module           -- begin a module definition (scoped dict)
//   use              -- push a module's dict onto the dict stack (pair with `end`)
//   import           -- import selected names from a module
//   module-dict      -- look up a registered module and push its dict
//   module?          -- test whether a module is registered
//   require          -- idempotent file load (run once per canonical path)
//   require-module   -- load a module if not already loaded
//
//   Modules are dicts stored in a global modules dict.  `module` creates
//   a new dict scope; definitions within it are local.  `import` copies
//   selected bindings into the caller's dictionary; `use` pushes the module
//   dict onto the dict stack so its names resolve until `end`.
//
// CLOSURES (lexical capture, built on the Curry representation)
//   closure-capture    -- capture named bindings into a callable
//   closure-with-dict  -- run a proc within an explicit environment dict (push dict, exec, pop)
//   closure-env        -- extract a closure's captured environment dict
//
// MISCELLANEOUS
//   string           -- create a string of N zero bytes
//   token            -- scan one value from a string or stream
//

// readonly-array-from-mark: mark any0 ... anyN :- arr
// Collects operands above mark into a new read-only array.
// throws: vm-full, unmatched-mark
static void readonlyarrayfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_array_from_mark(trx, Object::LiteralAttrib, Object::ReadOnlyAccess);
}

// =array-from-mark: mark any0 ... anyN :- arr
// Collects operands above mark into a new =array.
// throws: limit-check, unmatched-mark
static void eqarrayfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_eqarray_from_mark(trx);
}

// readonly-=array-from-mark: mark any0 ... anyN :- arr
// Collects operands above mark into a new read-only =array.
// throws: limit-check, unmatched-mark
static void readonlyeqarrayfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_eqarray_from_mark(trx, Object::LiteralAttrib, Object::ReadOnlyAccess);
}

// noop: :- --
// No operation.
// throws: (none)
static void noop_op(Trix *) {
    // no operation
}

// @call: (exec stack: sourceloc name)
// Consumes the companion Name and SourceLoc from exec stack.
// throws: internal-error
static void at_call_op(Trix *trx) {
    // Consume the companion literal Name and SourceLoc that execute_name() pushed below @call.
    if (trx->m_exec_ptr < (trx->m_exec_base + 1)) {
        trx->error(Error::InternalError, "@call: exec stack underflow -- expected Name and SourceLoc below @call");
    } else if (!trx->m_exec_ptr->is_name() || !(trx->m_exec_ptr - 1)->is_sourceloc()) {
        trx->error(Error::InternalError, "@call: expected [SourceLoc][Name] below @call on exec stack");
    } else {
        trx->m_exec_ptr -= 2;
    }
}

// unique: arr -- arr'
// Remove duplicate elements, preserving first-occurrence order. Array only (not packed).
// throws: opstack-underflow, type-check, vm-full
static void unique_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto container_ptr = trx->m_op_ptr;
    auto elem_data = container_ptr->array_objects(trx);
    auto length = container_ptr->arrays_length();

    // pass 1: count unique elements
    length_t count = 0;
    for (length_t i = 0; i < length; ++i) {
        auto duplicate = false;
        for (length_t j = 0; j < i; ++j) {
            if (elem_data[i].equal(trx, elem_data[j])) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            ++count;
        }
    }

    // pass 2: allocate exact-size result and fill.  Null and root the result
    // across the element clones: under ${...} each make_clone is a global
    // ExtValue alloc that fires the GC, and the (local) result array is not yet
    // on any stack (the source stays rooted via container_ptr until the final
    // store below).
    auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(count, ChunkKind::Array);
    std::fill_n(dst_ptr, count, Object::make_null());
    auto result_obj = Object::make_array(dst_offset, count);
    trx->gc_root_push_oneoff(result_obj);
    length_t k = 0;
    for (length_t i = 0; i < length; ++i) {
        auto duplicate = false;
        for (length_t j = 0; j < i; ++j) {
            if (elem_data[i].equal(trx, elem_data[j])) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            dst_ptr[k++] = elem_data[i].make_clone(trx);
        }
    }
    trx->gc_root_pop_oneoff();

    *container_ptr = result_obj;
}

// dedupe: array -- array'
// Remove consecutive duplicate elements (adjacent elements that are equal).
// Array only (not packed). [1 1 2 2 3 3 1] dedupe -> [1 2 3 1]
// throws: opstack-underflow, type-check, vm-full
static void dedupe_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto container_ptr = trx->m_op_ptr;
    auto elem_data = container_ptr->array_objects(trx);
    auto length = container_ptr->arrays_length();

    if (length > 1) {
        // 0 or 1 elements: already deduped
        // pass 1: count output elements (skip consecutive duplicates)
        length_t count = 1;  // first element always included
        for (length_t i = 1; i < length; ++i) {
            if (!elem_data[i].equal(trx, elem_data[i - 1])) {
                ++count;
            }
        }

        if (count != length) {
            // pass 2: allocate exact-size result and fill.  Null and root the result
            // across the element clones (see unique_op): under ${...} each make_clone is
            // a global ExtValue alloc that fires the GC, and the (local) result array is
            // not yet on any stack.
            auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(count, ChunkKind::Array);
            std::fill_n(dst_ptr, count, Object::make_null());
            auto result_obj = Object::make_array(dst_offset, count);
            trx->gc_root_push_oneoff(result_obj);
            dst_ptr[0] = elem_data[0].make_clone(trx);
            length_t k = 1;
            for (length_t i = 1; i < length; ++i) {
                if (!elem_data[i].equal(trx, elem_data[i - 1])) {
                    dst_ptr[k++] = elem_data[i].make_clone(trx);
                }
            }
            trx->gc_root_pop_oneoff();

            *container_ptr = result_obj;
        }
    }
}

// Clamped index value for an operand, discarding the in-range flag (the clamped
// value exceeding the array length is caught by the caller's bounds check).
static integer_t clamped_index(Trix *trx, Object obj) {
    auto [_, idx] = obj.integer_value(trx, 0, MaxLength);
    return idx;
}

// swap-at: array i j :- array
// Swaps the elements at indices i and j in the array (in place).
// throws: index-check, opstack-underflow, type-check
static void swapat_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative, VerifyIntegers | VerifyNotNegative, VerifyRWArray);

    auto j_ptr = trx->m_op_ptr;
    auto i_ptr = (j_ptr - 1);
    auto arr_ptr = (i_ptr - 1);

    auto i = clamped_index(trx, *i_ptr);
    auto j = clamped_index(trx, *j_ptr);
    auto length = arr_ptr->arrays_length();

    if ((i >= length) || (j >= length)) {
        trx->error(Error::IndexCheck, "swap-at: index out of bounds");
    } else {
        if (i != j) {
            auto elem_data = arr_ptr->array_objects(trx);
            auto tmp_obj = elem_data[i];
            elem_data[i] = elem_data[j];
            elem_data[j] = tmp_obj;
        }

        j_ptr->maybe_free_extvalue(trx);
        i_ptr->maybe_free_extvalue(trx);
        trx->m_op_ptr -= 2;
    }
}

// sum: arr :- number
// Returns the sum of all elements in the array.
// Empty array returns integer 0.  Element types must be uniform.
// throws: numerical-overflow, opstack-underflow, type-check
static void sum_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);

    auto container_ptr = trx->m_op_ptr;
    auto count = container_ptr->arrays_length();

    if (count == 0) {
        *container_ptr = Object::make_integer(0);
    } else {
        trx->require_op_capacity(2);

        // peek at first element type to determine the zero value
        Object first_obj;
        if (container_ptr->is_array()) {
            first_obj = container_ptr->array_objects(trx)[0];
        } else {
            auto packed_data = container_ptr->const_packed_span(trx);
            auto [next, element] = Object::extract_next_packed(trx, packed_data.data());
            first_obj = element;
        }

        if (first_obj.is_number()) {
            // push container, zero of matching type, { add } as proc for reduce
            // set up reduce: exec stack gets [add-operator] [container] [saved-depth] [@for-all]
            trx->require_exec_capacity(4);
            push_forall_frame(trx, Object::make_operator(SystemName::Add), *container_ptr);

            switch (+first_obj.type()) {
            case +Object::Type::Byte:
                *container_ptr = Object::make_byte(0);
                break;

            case +Object::Type::Integer:
                *container_ptr = Object::make_integer(0);
                break;

            case +Object::Type::UInteger:
                *container_ptr = Object::make_uinteger(0);
                break;

            case +Object::Type::Long:
                *container_ptr = Object::make_long(trx, 0);
                break;

            case +Object::Type::ULong:
                *container_ptr = Object::make_ulong(trx, 0);
                break;

            case +Object::Type::Real:
                *container_ptr = Object::make_real(0.0f);
                break;

            case +Object::Type::Double:
                *container_ptr = Object::make_double(trx, 0.0);
                break;

            case +Object::Type::Int128:
                *container_ptr = Object::make_int128(trx, 0);
                break;

            case +Object::Type::UInt128:
                *container_ptr = Object::make_uint128(trx, 0);
                break;

            default:
                std::unreachable();
            }
        } else {
            trx->error(Error::TypeCheck, "sum: array elements must be numeric");
        }
    }
}

// Shared implementation for min-of and max-of.
// Reduces array/packed with the given operator using the first element as accumulator.
static void extremum_impl(Trix *trx, SystemName op, const char *name) {
    trx->verify_operands(VerifyArrays);

    auto container_ptr = trx->m_op_ptr;
    auto count = container_ptr->arrays_length();

    if (count == 0) {
        trx->error(Error::RangeCheck, "{}: empty array", name);
    } else {
        // first element becomes the accumulator -- clone for ownership
        Object first_obj;
        if (container_ptr->is_array()) {
            first_obj = container_ptr->array_objects(trx)[0].make_clone(trx);
        } else {
            auto packed_data = container_ptr->const_packed_span(trx);
            auto [next, element] = Object::extract_next_packed(trx, packed_data.data());
            first_obj = element.make_clone(trx);
        }

        if (count != 1) {
            trx->require_exec_capacity(4);
            push_forall_frame(trx, Object::make_operator(op), *container_ptr);

            // operand stack: replace container with first element (accumulator)
        }
        *container_ptr = first_obj;
    }
}

// min-of: arr|packed :- element
// throws: execstack-overflow, opstack-underflow, range-check, type-check
static void minof_op(Trix *trx) {
    extremum_impl(trx, SystemName::Min, "min-of");
}

// max-of: arr|packed :- element
// throws: execstack-overflow, opstack-underflow, range-check, type-check
static void maxof_op(Trix *trx) {
    extremum_impl(trx, SystemName::Max, "max-of");
}

// reduce: arr init proc :- any
// Folds arr with proc using init as the initial accumulator.
// throws: execstack-overflow, opstack-underflow, type-check
static void reduce_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyAny, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto init_ptr = (proc_ptr - 1);
    auto container_ptr = (proc_ptr - 2);

    // Capture proc / container / init BEFORE rewriting m_op_ptr.  The
    // body-effect check needs push_forall_frame to capture saved_depth at
    // the per-iteration depth (i.e. the depth the body must restore to after
    // each pass), so do the operand-stack rewrite first.
    auto proc_obj = *proc_ptr;
    auto container_obj = *container_ptr;
    auto saved_init_obj = *init_ptr;
    trx->m_op_ptr -= 2;
    *trx->m_op_ptr = saved_init_obj;
    if (container_obj.arrays_length() != 0) {
        trx->require_exec_capacity(4);
        push_forall_frame(trx, proc_obj, container_obj);
    }
}

// map: arr proc :- arr
// Applies proc to each element, collecting results into a new array.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void map_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto src_length = container_ptr->arrays_length();

    if (src_length == 0) {
        // empty: return empty array
        auto [dst_ptr, offset] = trx->vm_alloc_dispatch_n<Object>(0, ChunkKind::Array);
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_array(offset, 0);
    } else {
        // allocate destination array
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(src_length, ChunkKind::Array);
        // initialize all slots to null (exit/error cleanup, and -- under ${...} where
        // dst is a GLOBAL block walked by gvm_for_each -- so the GC fired by the
        // pop-clone below walks valid (null) slots, not uninitialized garbage).
        for (length_t i = 0; i < src_length; ++i) {
            dst_ptr[i] = Object::make_null();
        }
        auto dst_obj = Object::make_array(dst_offset, src_length);

        // Root dst across the first pop-clone: under ${...} dst is global but not yet on
        // any stack, so the GC fired by arrays_pop_clone_head would sweep it; the oneoff
        // root holds it until it is parked on the exec stack (which then keeps it marked).
        trx->gc_root_push_oneoff(dst_obj);
        auto src_obj = *container_ptr;

        // pop first element from source
        auto first_obj = src_obj.arrays_pop_clone_head(trx);

        // set up exec stack frame: [proc] [src] [dst] [write-index] [@array-map] [proc-to-exec]
        trx->require_exec_capacity(6);
        *++trx->m_exec_ptr = *proc_ptr;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = dst_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayMap);
        *++trx->m_exec_ptr = *proc_ptr;
        trx->gc_root_pop_oneoff();

        // push first element to operand stack (proc will consume it)
        --trx->m_op_ptr;
        *trx->m_op_ptr = first_obj;
    }
}

// mapindexed: arr proc :- arr
// Like map but proc receives elem and index: proc: elem index -- new-elem
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void mapindexed_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto src_length = container_ptr->arrays_length();

    if (src_length == 0) {
        auto [dst_ptr, offset] = trx->vm_alloc_dispatch_n<Object>(0, ChunkKind::Array);
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_array(offset, 0);
    } else {
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(src_length, ChunkKind::Array);
        for (length_t i = 0; i < src_length; ++i) {
            dst_ptr[i] = Object::make_null();
        }
        auto dst_obj = Object::make_array(dst_offset, src_length);

        // Root dst across the first pop-clone (see map_op): under ${...} dst is global
        // but not yet on a stack, so the pop's GC would sweep it before it is parked.
        trx->gc_root_push_oneoff(dst_obj);
        auto src_obj = *container_ptr;
        auto first_obj = src_obj.arrays_pop_clone_head(trx);

        trx->require_exec_capacity(6);
        *++trx->m_exec_ptr = *proc_ptr;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = dst_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayMapIndexed);
        *++trx->m_exec_ptr = *proc_ptr;
        trx->gc_root_pop_oneoff();

        *container_ptr = first_obj;
        *proc_ptr = Object::make_integer(0);
    }
}

// flat-map: arr proc :- arr
// Maps proc over each element, then flattens the result.
// Equivalent to: arr proc map flatten
// throws: vm-full, execstack-overflow, limit-check, opstack-underflow, type-check
static void flatmap_op(Trix *trx) {
    trx->require_exec_capacity(1);

    *++trx->m_exec_ptr = Object::make_operator(SystemName::Flatten);
    map_op(trx);
}

// filter: arr proc :- arr
// Keeps elements for which proc returns true in a new array.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void filter_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays);

    auto proc_ptr = trx->m_op_ptr;
    auto container_ptr = (proc_ptr - 1);
    auto src_length = container_ptr->arrays_length();

    if (src_length == 0) {
        // empty: return empty array
        auto [dst_ptr, offset] = trx->vm_alloc_dispatch_n<Object>(0, ChunkKind::Array);
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_array(offset, 0);
    } else {
        // allocate destination array (max possible size)
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(src_length, ChunkKind::Array);
        for (length_t i = 0; i < src_length; ++i) {
            dst_ptr[i] = Object::make_null();
        }
        auto dst_obj = Object::make_array(dst_offset, src_length);

        // Root dst across the first pop-clone (see map_op): under ${...} dst is global
        // but not yet on a stack, so the pop's GC would sweep it before it is parked.
        trx->gc_root_push_oneoff(dst_obj);
        auto src_obj = *container_ptr;

        // pop first element from source
        // packed_pop_head returns a borrowed ref -- clone for ownership
        auto first_obj = src_obj.arrays_pop_clone_head(trx);

        // set up exec stack frame: [proc] [src] [dst] [write-index] [saved-element] [@array-filter] [proc]
        trx->require_exec_capacity(7);
        *++trx->m_exec_ptr = *proc_ptr;
        *++trx->m_exec_ptr = src_obj;
        *++trx->m_exec_ptr = dst_obj;
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = first_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayFilter);
        *++trx->m_exec_ptr = *proc_ptr;
        trx->gc_root_pop_oneoff();

        // push first element clone to operand stack (proc will test it)
        --trx->m_op_ptr;
        *trx->m_op_ptr = first_obj.make_clone(trx);
    }
}

// packed: any0 ... anyN int :- pk
// Packs the top N operands into a packed array.
// throws: vm-full, limit-check, opstack-underflow, type-check
static void packed_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative);

    auto n_ptr = trx->m_op_ptr;
    auto [valid, n] = n_ptr->integer_value(trx, 0, MaxArrayLength);
    if (valid) {
        trx->require_op_count(n + 1);

        auto first_ptr = (n_ptr - n);
        auto length = static_cast<length_t>(n);
        auto [ptr, offset] = Object::make_packed_data(trx, first_ptr, length);
        if (ptr != nullptr) {
            n_ptr->maybe_free_extvalue(trx);

            *first_ptr = Object::make_packed(offset, length);
            trx->m_op_ptr = first_ptr;
        } else {
            trx->error(Error::VMFull, "while constructing a packed array");
        }
    } else {
        trx->error(Error::LimitCheck, "packed length {} exceeds maximum {}", n, MaxArrayLength);
    }
}

// Search `raw` against the module search path: each colon-separated entry of
// trx->m_module_path is tried first (joined as `<entry>/<raw>`), then the
// implicit binary-relative `lib/<raw>` if trx->m_bin_dir is set.  Absolute
// paths return false (search-path doesn't apply -- caller falls back to
// realpath of `raw` itself).  `cand_buf` and `resolved_buf` are caller-
// supplied scratch buffers of size PATH_MAX (kept out of this function to
// avoid bloating the require_op stack frame past -Wstack-usage).  On hit,
// writes the canonical path into `resolved_buf` and returns true.
static bool require_search_path(Trix *trx, const char *raw, size_t raw_len, char *cand_buf, char *resolved_buf) {
    if (raw[0] == '/') {
        return false;
    } else {
        auto try_dir = [&](const char *dir, size_t dir_len) -> bool {
            if ((dir_len + 1 + raw_len + 1) > PATH_MAX) {
                return false;
            } else {
                std::memcpy(cand_buf, dir, dir_len);
                cand_buf[dir_len] = '/';
                std::memcpy(cand_buf + dir_len + 1, raw, raw_len);
                cand_buf[dir_len + 1 + raw_len] = '\0';
                return (::realpath(cand_buf, resolved_buf) != nullptr);
            }
        };

        if (trx->m_module_path != nullptr) {
            auto p = trx->m_module_path;
            while (*p != '\0') {
                auto colon = std::strchr(p, ':');
                auto entry_len = (colon != nullptr) ? static_cast<size_t>(colon - p) : std::strlen(p);
                if ((entry_len > 0) && try_dir(p, entry_len)) {
                    return true;
                } else if (colon == nullptr) {
                    break;
                } else {
                    p = colon + 1;
                }
            }
        }
        if (trx->m_bin_dir != nullptr) {
            // <bin_dir>/lib/<raw> -- compose directly into cand_buf to skip an
            // intermediate "<bin>/lib" buffer.
            constexpr std::string_view lib_seg = "/lib";
            auto bin_len = std::strlen(trx->m_bin_dir);
            if ((bin_len + lib_seg.size() + 1 + raw_len + 1) > PATH_MAX) {
                return false;
            } else {
                std::memcpy(cand_buf, trx->m_bin_dir, bin_len);
                std::memcpy(cand_buf + bin_len, lib_seg.data(), lib_seg.size());
                cand_buf[bin_len + lib_seg.size()] = '/';
                std::memcpy(cand_buf + bin_len + lib_seg.size() + 1, raw, raw_len);
                cand_buf[bin_len + lib_seg.size() + 1 + raw_len] = '\0';
                return (::realpath(cand_buf, resolved_buf) != nullptr);
            }
        }
        return false;
    }
}

// require: str -- --
// Idempotent file loading: runs the file on first call, silently skips on
// subsequent calls for the same canonical path.  Uses realpath() to
// canonicalize the filename so that relative and absolute references to the
// same file are recognized as identical.  If cwd-resolution fails on a
// relative path, the module search path (--module-path entries, then
// binary-relative `lib/`) is consulted in order.  The loaded-file tracking
// dict participates in save/restore (entries added after a save are rolled
// back on restore).  On load failure, the entry is retained to prevent
// infinite retry loops; use save/restore to reset if retry is needed.
// throws: execstack-overflow, file-open-error, filename-exists, filename-not-found,
// io-seek-error, limit-check, opstack-underflow, type-check
static void require_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    // Canonicalize the path so ./foo.trx and foo.trx and /abs/foo.trx all match.
    auto sv = trx->m_op_ptr->sv_value(trx);
    auto sv_len = sv_length(sv);
    if (sv_len > PATH_MAX) {
        trx->error(Error::LimitCheck, "require: filename length {} exceeds PATH_MAX {}", sv_len, PATH_MAX);
    } else {
        char raw_buf[PATH_MAX + 1];
        std::memcpy(raw_buf, sv.data(), sv_len);
        raw_buf[sv_len] = '\0';

        char cand_buf[PATH_MAX];
        char resolved[PATH_MAX];
        std::string_view canonical;
        bool rewrote_op = false;
        if (::realpath(raw_buf, resolved) != nullptr) {
            canonical = std::string_view{resolved};
        } else if (require_search_path(trx, raw_buf, sv_len, cand_buf, resolved)) {
            canonical = std::string_view{resolved};
            rewrote_op = true;
        } else {
            canonical = std::string_view{raw_buf, static_cast<size_t>(sv_len)};
        }

        // When the search path resolved the filename, replace the operand-stack
        // string with the canonical path so the downstream run_op opens the
        // correct file (otherwise it would re-open the unresolved raw_buf and
        // fail with file-open-error).
        if (rewrote_op) {
            trx->m_op_ptr->maybe_free_extvalue(trx);
            *trx->m_op_ptr = Object::make_string(trx, canonical);
        }

        // Intern the canonical path as a Name for O(1) dict lookup.
        auto name_offset = Name::add(trx, canonical);
        auto name_obj = Object::make_name(name_offset, static_cast<length_t>(canonical.size()));

        // Check if already loaded.
        auto require_dict = trx->offset_to_ptr<Dict>(trx->m_require_dict_offset);
        if (require_dict->get(trx, name_obj) != nullptr) {
            --trx->m_op_ptr;  // pop the filename string, noop
        } else {
            // Mark as loaded BEFORE running (prevents circular require loops).
            require_dict->put(trx, name_obj, Object::make_boolean(true));

            // Delegate to run_op -- the filename string is still on the operand stack.
            run_op(trx);
        }
    }
}

// -- Module System ----------------------------------------------------

// @end-module: (internal control operator)
// Pops module dict from dict stack, makes it ReadOnly, and registers it
// in the modules dict.  The module name was saved on the exec stack as
// a literal Name object directly below this control op.
static void at_end_module_op(Trix *trx) {
    // The exec stack layout when @end-module fires:
    //   [module-name-literal]  <-- next to pop after @end-module was popped
    // Pop the saved module name from exec stack.
    auto name_obj = *trx->m_exec_ptr--;

    // Pop the module dict from dict stack.
    if (trx->m_dict_ptr <= (trx->m_dict_base + PermanentDictCount - 1)) {
        trx->error(Error::DictStackUnderflow, "@end-module: dict stack underflow");
    } else {
        auto dict_offset = trx->m_dict_ptr->m_dict;
        auto dict = trx->offset_to_ptr<Dict>(dict_offset);
        --trx->m_dict_ptr;
        dict->clear_name_bindings(trx);

        // Make the dict ReadOnly.
        dict->set_readonly_access(trx);

        // Register in modules dict.  Duplicate module name is an error.
        auto modules_dict = trx->offset_to_ptr<Dict>(trx->m_modules_dict_offset);
        if (modules_dict->get(trx, &name_obj) != nullptr) {
            trx->error(Error::Undefined, "module: '{}' is already defined", name_obj.name_sv(trx));
        } else {
            auto name_for_dict = name_obj.make_clone(trx);
            auto dict_value = Object::make_dict(dict_offset);
            modules_dict->put(trx, name_for_dict, dict_value);
        }
    }
}

// module: name proc :- --
// Defines a named module.  Executes proc in a fresh dict scope, then makes
// the dict ReadOnly and registers it in the modules registry.  The `undefined`
// error is surfaced asynchronously by @end-module when the module name is
// already registered.
// throws: dictstack-overflow, execstack-overflow, opstack-underflow, type-check, undefined, vm-full
static void module_op(Trix *trx) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation, VerifyName);

    auto proc_obj = *trx->m_op_ptr;
    auto name_obj = *(trx->m_op_ptr - 1);

    trx->require_dict_capacity(1);
    trx->require_exec_capacity(3);

    trx->m_op_ptr -= 2;

    // Create a fresh ReadWriteDynamic dict for the module body.
    auto [dict, dict_offset] = Dict::create_dict(trx, 32, Object::DictMode::ReadWriteDynamic);

    // Push dict onto dict stack so def's go there.
    auto dict_obj = Object::make_dict(dict_offset);
    *++trx->m_dict_ptr = dict_obj;
    dict->set_name_bindings(trx);

    // Push exec stack: [name-literal] [@end-module] [proc-body]
    // When proc-body finishes, @end-module fires, pops name-literal.
    *++trx->m_exec_ptr = name_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atEndModule);
    *++trx->m_exec_ptr = proc_obj;
}

// module-dict: name :- dict
// Looks up a registered module by name and pushes its dict.
// throws: opstack-underflow, type-check, undefined
static void module_dict_op(Trix *trx) {
    trx->verify_operands(VerifyName);

    auto name_obj = *trx->m_op_ptr;
    auto modules_dict = trx->offset_to_ptr<Dict>(trx->m_modules_dict_offset);
    auto value = modules_dict->get(trx, &name_obj);
    if (value != nullptr) {
        *trx->m_op_ptr = *value;
    } else {
        trx->error(Error::Undefined, "module-dict: module '{}' not found", name_obj.name_sv(trx));
    }
}

// module?: name :- bool
// Tests whether a module with the given name is registered.
// throws: opstack-underflow, type-check
static void module_pred_op(Trix *trx) {
    trx->verify_operands(VerifyName);

    auto name_obj = *trx->m_op_ptr;
    auto modules_dict = trx->offset_to_ptr<Dict>(trx->m_modules_dict_offset);
    auto value = modules_dict->get(trx, &name_obj);
    *trx->m_op_ptr = Object::make_boolean(value != nullptr);
}

// use: name :- --
// Pushes a module's dict onto the dict stack.  Pair with `end` to pop.
// throws: dictstack-overflow, opstack-underflow, type-check, undefined
static void use_op(Trix *trx) {
    trx->verify_operands(VerifyName);
    trx->require_dict_capacity(1);

    auto name_obj = *trx->m_op_ptr;
    auto modules_dict = trx->offset_to_ptr<Dict>(trx->m_modules_dict_offset);
    auto value = modules_dict->get(trx, &name_obj);
    if (value != nullptr) {
        --trx->m_op_ptr;

        // Push module dict onto dict stack (like begin).
        *++trx->m_dict_ptr = *value;
        value->dict_value(trx)->set_name_bindings(trx);
    } else {
        trx->error(Error::Undefined, "use: module '{}' not found", name_obj.name_sv(trx));
    }
}

// import: name names... n :- --
// Copies n named entries from a module into the current dict.
// Stack: module-name entry-name1 ... entry-nameN count
// throws: dict-full, opstack-underflow, range-check, read-only, type-check, undefined, vm-full
static void import_op(Trix *trx) {
    trx->verify_operands(VerifyInteger);
    auto n = trx->m_op_ptr->integer_value();
    // Upper bound excludes integer_t_max so that (n + 1) cannot overflow below.
    if ((n <= 0) || (n == std::numeric_limits<integer_t>::max())) {
        trx->error(Error::RangeCheck, "import: count {} out of range", n);
    } else {
        --trx->m_op_ptr;

        // Need n entry names + module name on stack.
        trx->require_op_count(n + 1);

        // Module name is below all entry names.
        auto module_name_obj = *(trx->m_op_ptr - n);
        if (!module_name_obj.is_name()) {
            trx->error(Error::TypeCheck, "import: module name must be a name");
        } else {
            // Look up module.
            auto modules_dict = trx->offset_to_ptr<Dict>(trx->m_modules_dict_offset);
            auto mod_value = modules_dict->get(trx, &module_name_obj);
            if (mod_value == nullptr) {
                trx->error(Error::Undefined, "import: module '{}' not found", module_name_obj.name_sv(trx));
            } else {
                auto mod_dict = mod_value->dict_value(trx);

                // Current dict must be writable.  Walk past any |...| frames to
                // the first non-frame dict (matches def / store / current-dict).
                auto current = Dict::dict_stack_first_nonframe(trx)->dict_value(trx);
                if (!current->has_write_access()) {
                    trx->error(Error::ReadOnly, "import: current dict is read-only");
                } else {
                    // For each entry name, get from module dict and put into current dict.
                    auto names_base = trx->m_op_ptr - (n - 1);
                    for (integer_t i = 0; i < n; ++i) {
                        auto entry_name_obj = names_base[i];
                        if (!entry_name_obj.is_name()) {
                            trx->error(Error::TypeCheck, "import: entry name must be a name");
                        } else {
                            auto entry_value = mod_dict->get(trx, &entry_name_obj);
                            if (entry_value != nullptr) {
                                auto cloned_name = entry_name_obj.make_clone(trx);
                                auto cloned_value = entry_value->make_clone(trx);
                                // NoBinding: writes the first non-frame dict; a frame above
                                // may shadow (see def_op, engine bug #21).
                                current->put(trx, cloned_name, cloned_value, Dict::BindingMode::NoBinding);
                            } else {
                                trx->error(Error::Undefined, "import: '{}' not found in module", entry_name_obj.name_sv(trx));
                            }
                        }
                    }

                    // Pop all operands: module-name + n entry names.
                    trx->m_op_ptr = names_base - 2;
                }
            }
        }
    }
}

// @check-module: (internal control operator)
// Fires after require completes.  Verifies the expected module was registered.
// Exec stack layout: [module-name-literal] [@check-module]
static void at_check_module_op(Trix *trx) {
    auto name_obj = *trx->m_exec_ptr--;

    auto modules_dict = trx->offset_to_ptr<Dict>(trx->m_modules_dict_offset);
    if (modules_dict->get(trx, &name_obj) == nullptr) {
        trx->error(Error::Undefined, "require-module: file did not define module '{}'", name_obj.name_sv(trx));
    }
}

// require-module: str name :- --
// Requires a file (idempotent), then verifies the named module was defined.
// The verification happens asynchronously via @check-module after the file executes.
// throws: execstack-overflow, file-open-error, opstack-underflow, type-check, undefined
static void require_module_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyString);

    auto name_obj = *trx->m_op_ptr--;

    // Push @check-module with saved name BELOW require's exec stack entries.
    // require_op will push @run + stream on top; when those finish,
    // @check-module fires and verifies the module was registered.
    trx->require_exec_capacity(2);
    *++trx->m_exec_ptr = name_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atCheckModule);

    // Perform idempotent require (pushes @run + stream above our entries).
    require_op(trx);
}

// -- End Module System ------------------------------------------------

// string: int :- str
// Creates a string of N zero bytes.
// throws: limit-check, opstack-underflow, type-check
static void string_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative);

    auto top_ptr = trx->m_op_ptr;
    auto [valid, n] = top_ptr->integer_value(trx, 0, MaxStringLength);
    if (valid) {
        auto str_obj = Object::make_empty_string(trx, static_cast<length_t>(n));
        top_ptr->maybe_free_extvalue(trx);
        *top_ptr = str_obj;
    } else {
        trx->error(Error::LimitCheck, "string: count {} exceeds maximum {}", n, MaxStringLength);
    }
}

// token: str :- str any true | false
// Scans one token from string or stream.
// throws: vm-full, io-read-error, opstack-overflow, opstack-underflow, set-file-position-required, syntax-error,
// type-check
static void token_op(Trix *trx) {
    trx->verify_operands(VerifyString | VerifyStream);

    auto valid = false;
    auto source_obj = *trx->m_op_ptr;
    if (source_obj.is_string()) {
        auto [token, object, substring] = Stream::scanner(trx, source_obj);
        if ((token == Lexeme::LiteralValue) || (token == Lexeme::ExecutableValue)) {
            trx->require_op_capacity(2);

            *trx->m_op_ptr++ = substring;
            *trx->m_op_ptr++ = object;
            valid = true;
        } else if (token == Lexeme::EndOfProcedure) {
            trx->error(Error::SyntaxError, "token: unexpected end-of-procedure in string");
        } else /* EndOfStream or SyntaxError */ {
            valid = false;
        }
    } else {
        auto [stream, sid] = source_obj.stream_value(trx);
        if (stream->is_readable(sid)) {
            auto [token, object] = stream->scanner(trx);
            if ((token == Lexeme::LiteralValue) || (token == Lexeme::ExecutableValue)) {
                trx->require_op_capacity(1);

                *trx->m_op_ptr++ = object;
                valid = true;
            } else if (token == Lexeme::EndOfProcedure) {
                trx->error(Error::SyntaxError, "token: unexpected end-of-procedure in stream");
            } else /* EndOfStream or SyntaxError */ {
                valid = false;
            }
        }
    }
    *trx->m_op_ptr = Object::make_boolean(valid);
}

// curry: value callable :- curry
// Creates a curry Object that captures value and callable.
// When executed, the curry pushes value then executes callable.
// throws: opstack-underflow, vm-full
static void curry_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyAny);

    auto callable_obj = *trx->m_op_ptr;
    auto val_obj = *(trx->m_op_ptr - 1);

    // Allocate the curry pair BEFORE dropping the operand slots: make_curry can fire
    // vm_global_gc (global mode), and a callable scanned inside ${...} is a GLOBAL
    // composite reachable only through its op-stack slot -- decrementing first would
    // orphan it across the alloc and let the GC sweep it (mirror of compose_op).
    auto result = Object::make_curry(trx, val_obj, callable_obj);

    --trx->m_op_ptr;
    // Transfer value, callable ownership from op stack to array
    *trx->m_op_ptr = result;
}

// uncurry: curry :- value callable
// Decomposes a curry into its captured value and callable.
// Clones both stored Objects so the operand stack owns its own ExtValues
// (same pattern as array get).
// throws: opstack-underflow, type-check
static void uncurry_op(Trix *trx) {
    trx->verify_operands(VerifyCurry);
    trx->require_op_capacity(1);

    auto pair = trx->m_op_ptr->curry_storage(trx);
    auto val_obj = pair[0].make_clone(trx);
    auto callable_obj = pair[1].make_clone(trx);

    // Replace curry with the two cloned components
    *trx->m_op_ptr = val_obj;
    *++trx->m_op_ptr = callable_obj;
}

// try-result: proc :- tagged
// Executes proc. On success, wraps the result in /ok tag.
// On error, rolls back the operand stack and wraps the error name in /err tag.
// throws: execstack-overflow, opstack-underflow, type-check, vm-full
static void try_result_op(Trix *trx) {
    trx->verify_operands(VerifyProc);
    auto proc_obj = *trx->m_op_ptr--;

    auto saved_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);

    trx->require_exec_capacity(4);
    *++trx->m_exec_ptr = Object::make_integer(saved_depth);
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atTryResultComplete);
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atTryBarrier);
    *++trx->m_exec_ptr = proc_obj;
}

// @try-result-complete: internal control operator
// On success (@try-barrier pushed /no-error): pops /no-error, wraps result in /ok tag.
// On error (try_catch_handler pushed error name): rolls back op stack, wraps in /err tag.
static void at_try_result_complete_op(Trix *trx) {
    auto saved_depth = trx->m_exec_ptr->integer_value();
    --trx->m_exec_ptr;

    trx->require_op_count(1);
    auto status_ptr = trx->m_op_ptr;
    if (status_ptr->equal(trx, trx->error_name(Error::NoError))) {
        // pop /no-error
        --trx->m_op_ptr;
        trx->require_op_count(1);

        auto result_obj = *trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::Ok), result_obj);
    } else {
        auto error_name_obj = *status_ptr;
        for (auto target = trx->m_op_base + saved_depth; trx->m_op_ptr > target; --trx->m_op_ptr) {
            trx->m_op_ptr->maybe_free_extvalue(trx);
        }

        trx->require_op_capacity(1);

        *++trx->m_op_ptr = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::Err), error_name_obj);
    }
}

// compose: callable1 callable2 :- curry
// Creates a curry whose execution semantics are: execute callable1, then
// execute callable2.  This extends curry's normal behavior (push value, exec
// callable) by marking the captured value as executable so execute_curry
// dispatches it via execute_value rather than pushing it as data.
// Semantics: (f g compose exec) = (f exec g exec).
// throws: opstack-underflow, vm-full
static void compose_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyCallable);

    auto g = *trx->m_op_ptr;
    auto f = *(trx->m_op_ptr - 1);

    auto result = Object::make_compose_pair(trx, f, g);

    --trx->m_op_ptr;
    *trx->m_op_ptr = result;
}

// thunk: proc :- thunk
// Creates a thunk that wraps proc for lazy evaluation.
// The proc is not executed until the thunk is forced.
// throws: opstack-underflow, type-check, vm-full
static void thunk_op(Trix *trx) {
    trx->verify_operands(VerifyProc);

    *trx->m_op_ptr = Object::make_lazy_thunk(trx, *trx->m_op_ptr);
}

// force: any :- value
// If top is a thunk: evaluates if needed, returns cached result.
// If top is not a thunk: pass-through (no-op).
//
// Failure handling: when the thunk's proc raises an error, the unwinder
// swaps @force-complete with @force-fail (see ops_system.inl
// try_catch_handler).  @force-fail resets the thunk state from Evaluating
// back to Unevaluated and re-raises -- so a caller catching the error and
// retrying the force gets a clean attempt instead of a misleading
// "circular thunk" diagnostic.  Genuine reentrancy (proc transitively
// forces the same thunk) still hits the Evaluating branch below.
//
// throws: opstack-underflow, undefined-result (circular thunk), vm-full
static void force_op(Trix *trx) {
    trx->require_op_count(1);

    auto top_ptr = trx->m_op_ptr;
    if (top_ptr->is_thunk()) {
        auto storage = top_ptr->thunk_storage(trx);
        auto state = storage[Object::ThunkStorageState].integer_value();

        if (state == Object::ThunkEvaluated) {
            // Already evaluated: replace thunk with cached result
            *top_ptr = storage[Object::ThunkStorageResult].make_clone(trx);
        } else if (state == Object::ThunkUnevaluated) {
            trx->require_exec_capacity(3);

            // Unevaluated: set Evaluating, push continuation, then execute proc
            auto curr_save_level = trx->m_curr_save_level;
            if (storage[Object::ThunkStorageState].save_level() != curr_save_level) {
                Save::save_object(trx, &storage[Object::ThunkStorageState]);
            }
            storage[Object::ThunkStorageState].update_integer(Object::ThunkEvaluating);

            // Exec stack layout (bottom to top, first popped = top):
            //   thunk (literal)  -- companion below the barrier so the fail
            //                       handler can identify which thunk to reset.
            //                       Read by both @force-complete (success) and
            //                       @force-fail (error swap).
            //   @force-complete  -- BarrierOp; captures result, marks Evaluated.
            //                       Unwinder swaps to @force-fail on error.
            //   proc             -- the thunk's procedure, runs first (pushed
            //                       below by execute_value).
            *++trx->m_exec_ptr = *top_ptr;
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atForceComplete);
            --trx->m_op_ptr;

            auto proc_obj = storage[Object::ThunkStorageProc].make_clone(trx);
            trx->execute_value(proc_obj);
        } else {
            trx->error(Error::UndefinedResult, "circular thunk: force during evaluation");
        }
    }
}

// @force-complete: (internal control operator -- success path)
// Captures the proc's result, stores it in the thunk's VM storage, marks
// the thunk Evaluated, and pops the thunk-literal companion from exec.
//
// Exec stack on entry: [..., thunk-literal] (m_exec_ptr at thunk-literal;
// the dispatcher already popped @force-complete).
// Op stack on entry:   [..., result]
static void at_force_complete_op(Trix *trx) {
    trx->require_op_count(1);

    auto thunk_obj = *trx->m_exec_ptr--;
    auto result_obj = trx->m_op_ptr;

    if (thunk_obj.is_thunk()) {
        // Root the thunk across the result clone: thunk_obj was just popped off the
        // exec stack (line above), so it is a bare C local.  Under ${...} result_obj's
        // make_clone is a global alloc that fires vm_global_gc, which would sweep the
        // unrooted thunk block -- leaving `storage` dangling for the writes below (and
        // freeing the result ExtValue reachable only through it).  Rooting also keeps
        // it alive across save_object's journal allocation.
        trx->require_gc_root_capacity_more(1);
        *++trx->m_gc_roots_ptr = thunk_obj;
        auto storage = thunk_obj.thunk_storage(trx);

        // Journal state and result Objects if crossing save levels
        auto curr_save_level = trx->m_curr_save_level;
        if (storage[Object::ThunkStorageState].save_level() != curr_save_level) {
            Save::save_object(trx, &storage[Object::ThunkStorageState]);
        }
        if (storage[Object::ThunkStorageResult].save_level() != curr_save_level) {
            Save::save_object(trx, &storage[Object::ThunkStorageResult]);
        }

        // Store result in thunk (clone to transfer ownership to VM)
        auto result = result_obj->make_clone(trx);
        result.set_save_level(curr_save_level);
        storage[Object::ThunkStorageResult].maybe_free_extvalue(trx);
        storage[Object::ThunkStorageResult] = result;

        // Mark as Evaluated
        storage[Object::ThunkStorageState].update_integer(Object::ThunkEvaluated);
        trx->gc_root_pop_n(1);

        // Result stays on op stack; thunk-literal already popped from exec.
    } else {
        trx->error(Error::InternalError, "@force-complete: expected thunk on exec stack");
    }
}

// @force-fail: (internal control operator -- failure path)
// Replaces @force-complete on the exec stack when the unwinder catches an
// error during the thunk's proc evaluation.  Resets the thunk state from
// Evaluating back to Unevaluated so the caller can retry, then re-raises
// the original error.
//
// Exec stack on entry: [..., thunk-literal] (m_exec_ptr at thunk-literal).
// The unwinder already cleaned up exec-stack frames above @force-complete
// and freed any ExtValues in that range.
static void at_force_fail_op(Trix *trx) {
    auto thunk_obj = *trx->m_exec_ptr--;

    if (thunk_obj.is_thunk()) {
        auto storage = thunk_obj.thunk_storage(trx);

        // Journal state if crossing save levels (mirrors force_op's journal
        // when it set Evaluating); the original Unevaluated value journaled
        // by force_op is what restore would replay, but we explicitly reset
        // here in case no save crosses the call.
        auto curr_save_level = trx->m_curr_save_level;
        if (storage[Object::ThunkStorageState].save_level() != curr_save_level) {
            Save::save_object(trx, &storage[Object::ThunkStorageState]);
        }
        storage[Object::ThunkStorageState].update_integer(Object::ThunkUnevaluated);
    } else {
        trx->error(Error::InternalError, "@force-fail: expected thunk on exec stack");
    }

    // Re-raise the original error (the message and name are still in trx's
    // m_last_error / m_last_error_msg_ptr from the unwinder).
    trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
}

// thunk-evaluated?: thunk :- bool
// Returns true if the thunk has been forced and has a cached result.
// throws: opstack-underflow, type-check
static void thunk_evaluated_op(Trix *trx) {
    trx->verify_operands(VerifyThunk);

    auto top_ptr = trx->m_op_ptr;
    auto storage = top_ptr->thunk_storage(trx);
    *top_ptr = Object::make_boolean(storage[Object::ThunkStorageState].integer_value() == Object::ThunkEvaluated);
}

// thunk-reset: thunk :- thunk
// Resets an evaluated thunk back to unevaluated state, clearing the cached result.
// throws: opstack-underflow, type-check, undefined-result (if evaluating)
static void thunk_reset_op(Trix *trx) {
    trx->verify_operands(VerifyThunk);

    auto top_ptr = trx->m_op_ptr;
    auto storage = top_ptr->thunk_storage(trx);
    auto state = storage[Object::ThunkStorageState].integer_value();

    if (state == Object::ThunkEvaluating) {
        trx->error(Error::UndefinedResult, "thunk-reset: cannot reset thunk during evaluation");
    } else if (state == Object::ThunkEvaluated) {
        // Journal if crossing save levels
        auto curr_save_level = trx->m_curr_save_level;
        if (storage[Object::ThunkStorageState].save_level() != curr_save_level) {
            Save::save_object(trx, &storage[Object::ThunkStorageState]);
        }
        if (storage[Object::ThunkStorageResult].save_level() != curr_save_level) {
            Save::save_object(trx, &storage[Object::ThunkStorageResult]);
        }

        // Free cached result ExtValue if any
        storage[Object::ThunkStorageResult].maybe_free_extvalue(trx);

        // Reset to Unevaluated
        storage[Object::ThunkStorageState].update_integer(Object::ThunkUnevaluated);
        storage[Object::ThunkStorageResult] = Object::make_null(curr_save_level);
    }
}

//===--- Closure Operators ---===//
//
// Explicit capture closures: proc + captured-bindings dict.
//
//   closure-capture   proc name-array -- closure
//   closure-with-dict dict proc --
//   closure-env       closure -- dict

// closure-capture: proc [/name1 /name2 ...] closure-capture -- closure
// Looks up each name in the current dict stack, builds a ReadOnly dict
// with those bindings, and creates a Curry object [dict, proc] where
// the proc is wrapped to execute within the dict's scope.
// throws: opstack-underflow, range-check, type-check, undefined, vm-full
static void closure_capture_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyProc);

    auto names_obj = *trx->m_op_ptr;
    auto proc_obj = *(trx->m_op_ptr - 1);
    auto [names, names_length] = names_obj.array_value(trx);

    if (names_length == 0) {
        trx->error(Error::RangeCheck, "closure-capture: name array must not be empty");
    } else {
        for (length_t i = 0; i < names_length; ++i) {
            if (!names[i].is_name()) {
                trx->error(Error::TypeCheck, "closure-capture: name array must contain only names");
            }
        }

        // The dict, the callable array, and the Curry must all land in the SAME VM
        // region.  make_curry_pair routes through the dispatch allocator (global
        // under ${...}); create_dict/vm_alloc_n are local-only.  A global Curry
        // pointing at local dict/callable offsets would dangle once the enclosing
        // save level unwinds (restore reclaims local VM) -> use-after-free.  On the
        // global path we also root each freshly-built global block on the op stack
        // across the subsequent allocating calls so a vm_global_gc mid-build cannot
        // sweep it.  (names_obj/proc_obj stay rooted below as the input operands.)
        auto global = trx->m_curr_alloc_global;

        // Build a ReadOnly dict with captured bindings
        auto [dict, dict_offset] = global ? Dict::create_global_dict(trx, names_length)
                                          : Dict::create_dict(trx, names_length, Object::DictMode::ReadWriteFixed);
        auto dict_obj = Object::make_dict(dict_offset);
        if (global) {
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = dict_obj;  // root across the put-clone loop + later allocs
        }

        for (length_t i = 0; i < names_length; ++i) {
            auto value = Name::name_search(trx, &names[i]);
            if (value == nullptr) {
                trx->error(Error::Undefined, "closure-capture: name /{} not found", names[i].name_sv(trx));
            } else {
                static_cast<void>(dict->put(trx, names[i].make_clone(trx), value->make_clone(trx)));
            }
        }

        // Make the dict ReadOnly (snapshot, not live reference)
        dict->set_readonly_access(trx);

        // Closure = Curry [dict, {proc make-executable closure-with-dict}].
        // Curry execution pushes dict on op stack, then runs the callable.
        // The callable: proc lands on op stack as literal (PushOpDirect),
        // make-executable restores its executable attribute, then
        // closure-with-dict pops dict+proc and does begin/exec/end.

        // Build the callable: [proc-element, make-executable, closure-with-dict]
        auto [callable_elements, callable_offset] =
                global ? trx->vm_alloc_dispatch_n<Object>(3, Trix::ChunkKind::Array) : trx->vm_alloc_n<Object>(3);
        callable_elements[0] = proc_obj;
        callable_elements[1] = Object::make_operator(SystemName::MakeExecutable);
        callable_elements[2] = Object::make_operator(SystemName::ClosureWithDict);
        auto callable = Object::make_array(callable_offset, 3, Object::ExecutableAttrib, Object::ReadOnlyAccess);
        if (global) {
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = callable;  // root across make_curry_pair's alloc
        }

        // Build Curry [dict, callable]
        auto closure = Object::make_curry_pair(trx, dict_obj, callable);

        if (global) {
            trx->m_op_ptr -= 2;  // pop the dict_obj + callable temp roots
        }
        trx->m_op_ptr -= 2;
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = closure;
    }
}

// @closure-end: pops dict from dict stack after closure body completes.
// throws: none
static void at_closure_end_op(Trix *trx) {
    if (trx->m_dict_ptr > (trx->m_dict_base + PermanentDictCount - 1)) {
        auto dict = trx->m_dict_ptr->dict_value(trx);
        --trx->m_dict_ptr;
        dict->clear_name_bindings(trx);
    }
}

// closure-with-dict: dict proc closure-with-dict
// Pushes dict on dict stack, executes proc, pops dict.
// throws: dictstack-overflow, execstack-overflow, opstack-underflow, type-check
static void closure_with_dict_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyDict);
    trx->require_dict_capacity(1);
    trx->require_exec_capacity(2);

    auto proc_obj = *trx->m_op_ptr;
    auto dict_obj = *(trx->m_op_ptr - 1);

    // Push dict on dict stack
    auto dict = dict_obj.dict_value(trx);
    *++trx->m_dict_ptr = dict_obj;
    dict->set_name_bindings(trx);

    // Push cleanup + proc on exec
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atClosureEnd);
    *++trx->m_exec_ptr = proc_obj;

    trx->m_op_ptr -= 2;
}

// closure-env: closure -- dict
// Extract the captured bindings dict from a closure (Curry [dict, callable]).
// throws: opstack-underflow, type-check
static void closure_env_op(Trix *trx) {
    trx->verify_operands(VerifyCurry);

    auto closure_obj = *trx->m_op_ptr;
    auto pair = closure_obj.curry_storage(trx);
    if (pair[0].is_dict()) {
        *trx->m_op_ptr = pair[0].make_clone(trx);
    } else {
        trx->error(Error::TypeCheck, "closure-env: not a valid closure (expected dict in curry value)");
    }
}
