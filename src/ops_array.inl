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

//===--- Array Operators ---===//
//
// Implements array creation, element access, manipulation, sorting, and
// set-like operations on arrays.  Based on:
//
//   PostScript array operations: array, get, put, length, getinterval
//   (get-interval), putinterval (put-interval).  Trix names in parentheses
//   where they differ.  (copy and for-all are implemented elsewhere --
//   ops_stack.inl and ops_flow.inl.)
//
//   Functional collection operations implemented here (map/filter/reduce
//   are in ops_higher.inl and ops_array_iteration.inl): sort, intersect,
//   union, difference.  (reverse and flatten live in ops_string.inl;
//   unique in ops_higher.inl.)
//
// --- Core concepts for maintainers ---
//
// ARRAY REPRESENTATION
//   An Array Object holds a vm_offset_t to a contiguous block of Objects
//   on the VM heap and a length in the header.  Arrays can be ReadOnly
//   or ReadWrite.  Packed arrays (compressed representation) share many
//   operators via the arrays_length() / array_objects() accessors but
//   are always ReadOnly and sequential-access.
//
// ELEMENT OWNERSHIP
//   Array elements are Objects.  When copying between arrays, elements
//   must be cloned (make_clone) to preserve ExtValue ownership.  Direct
//   Object copy creates shared references, which would lead to double-free
//   on ExtValue cleanup.
//
// SORTING
//   sort uses std::sort with the file-local object_less_than() comparator
//   (per-type ordering, numeric promotion across types, lexicographic for
//   strings, type-ordinal fallback for non-comparable types).
//   sort allocates a new array (it is not in-place on the original).
//
// SET-LIKE OPERATIONS ON ARRAYS
//   intersect and difference use a temporary hash dict for O(n+m) lookup:
//   build a dict from one array, scan the other, keep/reject matches.
//   Shared implementation via intersect_difference_impl (bool parameter
//   selects keep-if-found vs keep-if-not-found).
//   union similarly builds a first-occurrence-tracking 'seen' dict to dedup
//   the concatenation of both inputs.
//

// array: int :- arr
// Creates an array of N null elements.
// throws: limit-check, opstack-underflow, type-check
static void array_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative);

    auto n_ptr = trx->m_op_ptr;
    auto [valid, n] = n_ptr->integer_value(trx, 0, MaxArrayLength);
    if (valid) {
        n_ptr->maybe_free_extvalue(trx);
        *n_ptr = Object::make_empty_array(trx, static_cast<length_t>(n));
    } else {
        trx->error(Error::LimitCheck, "operand N {} is not in the range of 0..{}", n, MaxArrayLength);
    }
}

// length: any :- int
// Returns the length of an array, packed, dict, string, or name.
// throws: opstack-underflow, type-check
static void length_op(Trix *trx) {
    trx->verify_operands(VerifyHasLength);

    auto length_ptr = trx->m_op_ptr;
    auto length = length_ptr->length(trx);
    *length_ptr = Object::make_integer(length);
}

// Common implementation for keys and values.
static void dict_extract_op(Trix *trx, bool extract_keys) {
    trx->verify_operands(VerifyDict);

    auto dict_ptr = trx->m_op_ptr;
    auto dict = dict_ptr->dict_value(trx);
    auto count = dict->length();

    auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(count, ChunkKind::Array);
    // Null + root the result across the clones: under ${...} each make_clone is a
    // global alloc that can fire vm_global_gc, and the result array is not yet on
    // any stack, so its already-written global clones would be swept.
    std::fill_n(dst_ptr, count, Object::make_null());
    auto result_obj = Object::make_array(dst_offset, count);
    trx->gc_root_push_oneoff(result_obj);
    auto i = length_t{0};
    auto entry_offset = nulloffset;
    auto bucket_idx = integer_t{-1};
    while (true) {
        auto [next_offset, next_idx, key, value] = dict->next(trx, entry_offset, bucket_idx);
        if (next_offset == nulloffset) {
            break;
        } else {
            dst_ptr[i++] = (extract_keys ? key : value).make_clone(trx);
            entry_offset = next_offset;
            bucket_idx = next_idx;
        }
    }
    trx->gc_root_pop_oneoff();
    *dict_ptr = result_obj;
}

// keys: dict :- arr
// Returns a new array of all keys in the dictionary.
// throws: vm-full, opstack-underflow, type-check
static void keys_op(Trix *trx) {
    dict_extract_op(trx, true);
}

// values: dict :- arr
// Returns a new array of all values in the dictionary.
// throws: vm-full, opstack-underflow, type-check
static void values_op(Trix *trx) {
    dict_extract_op(trx, false);
}

// get: container key :- any
// Retrieves an element by key (dict, record, tagged) or index (array/packed/string).
// Tagged containers expose two pseudo-fields: /tag returns the tag name, /value
// returns the payload -- enabling the `t.tag` / `t.value` field-access sugar.
// throws: vm-full, index-check, limit-check, opstack-underflow, type-check, undefined
static void get_op(Trix *trx) {
    trx->require_op_count(2);

    auto index_ptr = trx->m_op_ptr;
    auto container_ptr = (index_ptr - 1);
    if (container_ptr->is_tagged()) {
        trx->verify_operands(VerifyName, VerifyTagged);

        auto key_sv = index_ptr->name_sv(trx);
        auto pair = trx->offset_to_ptr<Object>(container_ptr->tagged_offset());
        if (key_sv == "tag") {
            *container_ptr = pair[Object::TaggedNameIndex].make_clone(trx);
        } else if (key_sv == "value") {
            *container_ptr = pair[Object::TaggedValueIndex].make_clone(trx);
        } else {
            trx->error(Error::Undefined, "get: field /{} not found on tagged value (expected /tag or /value)", key_sv);
        }
        trx->m_op_ptr = container_ptr;
        return;
    } else if (container_ptr->is_record()) {
        trx->verify_operands(VerifyName, VerifyRecord);

        auto inst = container_ptr->record_instance(trx);
        auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);
        auto field_count = length_t{schema->m_field_count};
        for (length_t i = 0; i < field_count; ++i) {
            if (schema->m_names[i].equal(trx, *index_ptr)) {
                *container_ptr = inst->m_fields[i].make_clone(trx);
                trx->m_op_ptr = container_ptr;
                return;
            }
        }
        trx->error(Error::Undefined, "get: field /{} not found in record", index_ptr->name_sv(trx));
    } else if (container_ptr->is_dict()) {
        trx->verify_operands(VerifyKey, VerifyDict);

        auto dict = container_ptr->dict_value(trx);
        auto value = dict->get(trx, index_ptr);
        if (value == nullptr) {
            if (index_ptr->is_name()) {
                trx->error(Error::Undefined, "get: {} not found in dict", index_ptr->name_sv(trx));
            } else {
                trx->error(Error::Undefined, "get: key not found in dict");
            }
        } else {
            *container_ptr = value->make_clone(trx);
            index_ptr->maybe_free_extvalue(trx);
        }
    } else {
        trx->verify_operands(VerifyIntegers, VerifyIndexable);

        auto length = container_ptr->object_length();
        auto [valid, index] = index_ptr->integer_value(trx, -static_cast<integer_t>(length), MaxLength);
        if (valid) {
            // negative indexing: -1 = last, -2 = second-to-last, etc.
            if (index < 0) {
                index = index + static_cast<integer_t>(length);
            }
            if ((index >= 0) && (index < length)) {
                if (container_ptr->is_string()) {
                    auto str_data = container_ptr->string_vptr(trx);
                    *container_ptr = Object::make_byte(str_data[index]);
                } else if (container_ptr->is_array()) {
                    auto elem_data = container_ptr->array_objects(trx);
                    *container_ptr = elem_data[index].make_clone(trx);
                } else {
                    auto packed_span = container_ptr->const_packed_span(trx);
                    auto packed_data = packed_span.data();

                    packed_data = Object::skip_packed(packed_data, static_cast<length_t>(index));
                    auto [_, object] = Object::extract_next_packed(trx, packed_data);
                    *container_ptr = object.make_clone(trx);
                }
                index_ptr->maybe_free_extvalue(trx);
            } else {
                trx->error(Error::IndexCheck, "operand index {} exceeds container length {}", index, length);
            }
        } else {
            trx->error(Error::LimitCheck, "operand index {} is not in the range of -{}..{}", index, length, MaxLength);
        }
    }
    trx->m_op_ptr = container_ptr;
}

// getinterval: container int int :- sub-container
// Returns a subrange of an array, packed, or string.
// throws: index-check, limit-check, opstack-underflow, type-check
static void getinterval_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers, VerifyIntegers, VerifyIndexable);

    auto count_ptr = trx->m_op_ptr;
    auto [count_valid, count] = count_ptr->integer_value(trx, 0, MaxLength);
    if (!count_valid) {
        trx->error(Error::LimitCheck, "operand count {} is not in the range of 0..{}", count, MaxLength);
    } else {
        auto index_ptr = (count_ptr - 1);
        auto [index_valid, index] = index_ptr->integer_value(trx, 0, MaxLength);
        if (!index_valid) {
            trx->error(Error::LimitCheck, "operand index {} is not in the range of 0..{}", index, MaxLength);
        } else {
            auto container_ptr = (index_ptr - 1);
            auto container_length = container_ptr->object_length();
            if ((index + count) > container_length) {
                count_ptr->maybe_free_extvalue(trx);
                index_ptr->maybe_free_extvalue(trx);
                trx->error(Error::IndexCheck, "index {} plus count {} exceeds container length {}", index, count, container_length);
            } else {
                container_interval(trx, container_ptr, static_cast<length_t>(index), static_cast<length_t>(count));
                count_ptr->maybe_free_extvalue(trx);
                index_ptr->maybe_free_extvalue(trx);
            }

            trx->m_op_ptr = container_ptr;
        }
    }
}

// take: indexable int :- indexable
// Returns the first N elements of an array, packed array, or string.
// throws: index-check, limit-check, opstack-underflow, type-check
static void take_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers, VerifyIndexable);

    auto count_ptr = trx->m_op_ptr;
    auto container_ptr = (count_ptr - 1);
    auto container_length = container_ptr->object_length();

    auto [count_valid, count] = count_ptr->integer_value(trx, 0, MaxLength);
    if (!count_valid) {
        trx->error(Error::LimitCheck, "take: count {} is not in the range of 0..{}", count, MaxLength);
    } else if (count > container_length) {
        trx->error(Error::IndexCheck, "take: count {} exceeds container length {}", count, container_length);
    } else {
        container_interval(trx, container_ptr, 0, static_cast<length_t>(count));
        count_ptr->maybe_free_extvalue(trx);
        trx->m_op_ptr = container_ptr;
    }
}

// drop: indexable int :- indexable
// Removes the first N elements of an array, packed array, or string.
// throws: index-check, limit-check, opstack-underflow, type-check
static void drop_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers, VerifyIndexable);

    auto count_ptr = trx->m_op_ptr;
    auto container_ptr = (count_ptr - 1);
    auto container_length = container_ptr->object_length();

    auto [count_valid, count] = count_ptr->integer_value(trx, 0, MaxLength);
    if (!count_valid) {
        trx->error(Error::LimitCheck, "drop: count {} is not in the range of 0..{}", count, MaxLength);
    } else if (count > container_length) {
        trx->error(Error::IndexCheck, "drop: count {} exceeds container length {}", count, container_length);
    } else {
        container_interval(trx, container_ptr, static_cast<length_t>(count), static_cast<length_t>(container_length - count));
        count_ptr->maybe_free_extvalue(trx);
        trx->m_op_ptr = container_ptr;
    }
}

// putinterval: dst int src :- --
// Copies src elements into dst starting at index.
// throws: vm-full, index-check, opstack-underflow, type-check
static void putinterval_op(Trix *trx) {
    trx->verify_operands(VerifyIndexable, VerifyIntegers, VerifyRWString | VerifyRWArray);

    auto src_ptr = trx->m_op_ptr;
    auto index_ptr = (src_ptr - 1);
    auto dst_ptr = (index_ptr - 1);

    auto valid = (src_ptr->is_string()) ? (dst_ptr->is_string()) : dst_ptr->is_array();
    if (!valid) {
        trx->error(Error::TypeCheck, "putinterval: source and destination types are incompatible");
    } else {
        auto [index_valid, index_int] = index_ptr->integer_value(trx, 0);
        auto src_length = src_ptr->object_length();
        auto dst_length = dst_ptr->object_length();
        if (index_valid && ((static_cast<long_t>(index_int) + src_length) <= dst_length)) {
            if (dst_ptr->is_string()) {
                // length is in vm_t/chars
                auto src_data = src_ptr->string_vptr(trx);
                auto dst_data = dst_ptr->string_vptr(trx);
                dst_data += index_int;
                std::memmove(dst_data, src_data, src_length);
            } else {
                // length is in Objects
                auto dst_is_eqarray = dst_ptr->is_eqarray(trx);
                auto dst_elem_ptr = (dst_ptr->array_objects(trx) + index_int);
                if (src_ptr->is_array()) {
                    auto curr_save_level = trx->m_curr_save_level;
                    auto src_elem_ptr = src_ptr->array_objects(trx);
                    while (src_length-- != 0) {
                        if ((dst_elem_ptr->save_level() == curr_save_level) || dst_is_eqarray) {
                            dst_elem_ptr->maybe_free_extvalue(trx);
                        } else {
                            Save::save_object(trx, dst_elem_ptr);
                        }
                        *dst_elem_ptr = src_elem_ptr->make_clone(trx);
                        dst_elem_ptr->set_save_level(curr_save_level);

                        ++src_elem_ptr;
                        ++dst_elem_ptr;
                    }
                } else {
                    auto packed_data = src_ptr->const_packed_span(trx);
                    auto destination =
                            dst_is_eqarray ? Object::ExtractPackedDestination::EqArray : Object::ExtractPackedDestination::Array;
                    Object::extract_packed(trx, packed_data.data(), dst_elem_ptr, src_length, destination);
                }
            }

            index_ptr->maybe_free_extvalue(trx);
            trx->m_op_ptr = --dst_ptr;
        } else if (index_valid) {
            trx->error(Error::IndexCheck,
                       "putinterval: index {} + source length {} exceeds destination length {}",
                       index_int,
                       src_length,
                       dst_length);
        } else {
            trx->error(Error::IndexCheck, "putinterval: index {} out of range", index_int);
        }
    }
}

// range: stop :- arr
// Generates the integer sequence [0, stop) as an Array.  Strict 1-arg
// form: the 2-arg `start stop` sequence lives in `range-from` instead,
// so each op has exactly one signature.  (A single arity-overloaded op
// would silently grab an integer below the top -- e.g. an unrelated
// loop counter -- and build wrong-shape arrays.)
// throws: vm-full, opstack-underflow, type-check
static void range_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers);

    auto top_ptr = trx->m_op_ptr;
    auto [valid, stop] = top_ptr->integer_value(trx, 0);
    top_ptr->maybe_free_extvalue(trx);
    --trx->m_op_ptr;

    auto start = integer_t{0};
    if ((stop - start) > MaxArrayLength) {
        trx->error(Error::LimitCheck, "range: length {} exceeds maximum {}", stop - start, MaxArrayLength);
    } else {
        auto count = (stop > start) ? static_cast<length_t>(stop - start) : length_t{0};
        // Immediate Integer elements (no heap refs) -> no rooting; just route the
        // array to the current region so a ${...} result survives save/restore.
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(count, ChunkKind::Array);
        for (length_t i = 0; i < count; ++i) {
            dst_ptr[i] = Object::make_integer(static_cast<integer_t>(start + i));
        }
        *++trx->m_op_ptr = Object::make_array(dst_offset, count);
    }
}

// range-from: start stop :- arr
// Generates the integer sequence [start, stop) as an Array.  Strict
// 2-arg form; the inverted-bounds case (start >= stop) returns an
// empty array.  See `range` for the 1-arg form (start defaults to 0).
// throws: vm-full, opstack-underflow, type-check
static void range_from_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers, VerifyIntegers);

    auto top_ptr = trx->m_op_ptr;
    auto [v1, stop] = top_ptr->integer_value(trx, 0);
    auto [v2, start] = (top_ptr - 1)->integer_value(trx, 0);
    top_ptr->maybe_free_extvalue(trx);
    (top_ptr - 1)->maybe_free_extvalue(trx);
    trx->m_op_ptr -= 2;

    // compute the span in long_t -- stop - start in int32 would overflow (UB) for
    // a very negative start and very positive stop.
    auto span = static_cast<long_t>(stop) - start;
    if (span > MaxArrayLength) {
        trx->error(Error::LimitCheck, "range-from: length {} exceeds maximum {}", span, MaxArrayLength);
    } else {
        auto count = (span > 0) ? static_cast<length_t>(span) : length_t{0};
        // Immediate Integer elements (no heap refs) -> no rooting; region-aware array.
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(count, ChunkKind::Array);
        for (length_t i = 0; i < count; ++i) {
            dst_ptr[i] = Object::make_integer(static_cast<integer_t>(start + i));
        }
        *++trx->m_op_ptr = Object::make_array(dst_offset, count);
    }
}

// step-range: start stop step :- array
// Creates an array from start to stop (exclusive) with the given step.
// throws: vm-full, opstack-underflow, range-check, type-check
static void steprange_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers, VerifyIntegers, VerifyIntegers);

    auto step_ptr = trx->m_op_ptr;
    auto stop_ptr = (step_ptr - 1);
    auto start_ptr = (stop_ptr - 1);

    auto [v1, step] = step_ptr->integer_value(trx);
    auto [v2, stop] = stop_ptr->integer_value(trx);
    auto [v3, start] = start_ptr->integer_value(trx);

    if (step == 0) {
        trx->error(Error::RangeCheck, "step-range: step cannot be zero");
    } else {
        step_ptr->maybe_free_extvalue(trx);
        stop_ptr->maybe_free_extvalue(trx);
        start_ptr->maybe_free_extvalue(trx);

        // compute count in long_t: the span and the (stop - start + step - 1) numerator
        // overflow int32 (UB) for far-apart bounds, and -step is UB when step is INT_MIN.
        auto count_wide = long_t{0};
        if (step > 0) {
            auto span = static_cast<long_t>(stop) - start;
            if (span > 0) {
                auto stepw = static_cast<long_t>(step);
                count_wide = (span + stepw - 1) / stepw;
            }
        } else {
            auto span = static_cast<long_t>(start) - stop;
            if (span > 0) {
                auto stepw = -static_cast<long_t>(step);
                count_wide = (span + stepw - 1) / stepw;
            }
        }
        if (count_wide > MaxArrayLength) {
            trx->error(Error::LimitCheck, "step-range: length {} exceeds maximum {}", count_wide, MaxArrayLength);
        } else {
            auto count = static_cast<length_t>(count_wide);

            // Immediate Integer elements (no heap refs) -> no rooting; region-aware array.
            auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(count, ChunkKind::Array);
            auto value = start;
            for (length_t i = 0; i < count; ++i) {
                dst_ptr[i] = Object::make_integer(value);
                value += step;
            }

            trx->m_op_ptr -= 3;
            *++trx->m_op_ptr = Object::make_array(dst_offset, count);
        }
    }
}

// append: arr any :- arr
// Returns a new array with any appended.
// throws: vm-full, opstack-underflow, type-check
static void append_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyArrays);

    auto elem_ptr = trx->m_op_ptr;
    auto arr_ptr = (elem_ptr - 1);
    auto src_length = arr_ptr->arrays_length();
    if (src_length >= MaxArrayLength) {
        trx->error(Error::LimitCheck,
                   "append: result length {} exceeds maximum {}",
                   static_cast<vm_size_t>(src_length) + 1,
                   MaxArrayLength);
    }
    auto new_length = static_cast<length_t>(src_length + 1);

    auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(new_length, ChunkKind::Array);
    // Null the slots and root the result before the element clones below.  Under
    // ${...} each clone is a global ExtValue allocation that fires vm_global_gc;
    // the (local) result array is not yet on any stack, so without a root its
    // already-written global element clones would be swept.  Rooting result_obj
    // marks its slots (the GC descends array roots); the null-init keeps the
    // not-yet-written tail safe to walk.
    std::fill_n(dst_ptr, new_length, Object::make_null());
    auto result_obj = Object::make_array(dst_offset, new_length);
    trx->gc_root_push_oneoff(result_obj);
    clone_array_elements(trx, arr_ptr, dst_ptr, src_length);
    dst_ptr[src_length] = elem_ptr->make_clone(trx);
    trx->gc_root_pop_oneoff();

    elem_ptr->maybe_free_extvalue(trx);
    --trx->m_op_ptr;
    *trx->m_op_ptr = result_obj;
}

// sort: arr :- arr
// Returns a new array with elements sorted in ascending order.
// throws: vm-full, opstack-underflow, type-check
static void sort_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);

    auto src_ptr = trx->m_op_ptr;
    auto src_length = src_ptr->arrays_length();

    if (src_length <= 1) {
        // 0 or 1 elements: already sorted, return as-is (clone if packed)
        if (src_ptr->is_packed()) {
            auto [dst_ptr, offset] = trx->vm_alloc_dispatch_n<Object>(src_length, ChunkKind::Array);
            auto result_obj = Object::make_array(offset, src_length);
            if (src_length == 1) {
                // Root the result across the single packed clone: under ${...} the
                // clone is a global alloc that can fire vm_global_gc, which would
                // otherwise sweep the not-yet-rooted global result array.
                std::fill_n(dst_ptr, src_length, Object::make_null());
                trx->gc_root_push_oneoff(result_obj);
                *dst_ptr = src_ptr->packed_pop_clone_head(trx);
                trx->gc_root_pop_oneoff();
            }
            *src_ptr = result_obj;
        }
    } else {
        // allocate new array and clone all elements
        auto [dst_ptr, offset] = trx->vm_alloc_dispatch_n<Object>(src_length, ChunkKind::Array);
        // Null and root the result across the element clones: under ${...} each
        // clone is a global ExtValue alloc that fires vm_global_gc, and the (local)
        // result array is not yet on any stack, so its already-written global
        // clones would be swept.  Rooting marks its slots; the sort is alloc-free.
        std::fill_n(dst_ptr, src_length, Object::make_null());
        auto result_obj = Object::make_array(offset, src_length);
        trx->gc_root_push_oneoff(result_obj);
        clone_array_elements(trx, src_ptr, dst_ptr, src_length);

        // sort using natural ordering comparator
        std::sort(dst_ptr, dst_ptr + src_length, [trx](Object a, Object b) -> bool { return object_less_than(trx, a, b); });
        trx->gc_root_pop_oneoff();

        *src_ptr = result_obj;
    }
}

// Compare two Objects for less-than ordering, used by sort_op.
// Handles all numeric types with promotion, strings lexicographically,
// and falls back to type ordinal for non-comparable types.
[[nodiscard]] static bool object_less_than(Trix *trx, Object a, Object b) {
    auto a_type = a.type();
    auto b_type = b.type();

    // fast path: same type
    if (a_type == b_type) {
        switch (+a_type) {
        case +Object::Type::Byte:
            return (a.byte_value() < b.byte_value());

        case +Object::Type::Integer:
            return (a.integer_value() < b.integer_value());

        case +Object::Type::UInteger:
            return (a.uinteger_value() < b.uinteger_value());

        case +Object::Type::Long:
            return (a.long_value(trx) < b.long_value(trx));

        case +Object::Type::ULong:
            return (a.ulong_value(trx) < b.ulong_value(trx));

        case +Object::Type::Int128:
            return (a.int128_value(trx) < b.int128_value(trx));

        case +Object::Type::UInt128:
            return (a.uint128_value(trx) < b.uint128_value(trx));

        case +Object::Type::Address:
            return (a.address_value(trx) < b.address_value(trx));

        case +Object::Type::Real:
            return (a.real_value() < b.real_value());

        case +Object::Type::Double:
            return (a.double_value(trx) < b.double_value(trx));

        case +Object::Type::Boolean:
            return (!a.boolean_value() && b.boolean_value());

        case +Object::Type::String:
            return (a.string_compare(trx, &b) < 0);

        case +Object::Type::Name:
            return (a.name_sv(trx) < b.name_sv(trx));

        default:
            return false;
        }
    }

    // cross-type numeric promotion
    if (a.is_number() && b.is_number()) {
        return (object_to_double(trx, a, a_type) < object_to_double(trx, b, b_type));
    } else {
        // non-comparable: order by type ordinal
        return (+a_type < +b_type);
    }
}

[[nodiscard]] static double object_to_double(Trix *trx, Object o, Object::Type t) {
    switch (+t) {
    case +Object::Type::Byte:
        return static_cast<double>(o.byte_value());

    case +Object::Type::Integer:
        return static_cast<double>(o.integer_value());

    case +Object::Type::UInteger:
        return static_cast<double>(o.uinteger_value());

    case +Object::Type::Long:
        return static_cast<double>(o.long_value(trx));

    case +Object::Type::ULong:
        return static_cast<double>(o.ulong_value(trx));

    case +Object::Type::Int128:
        return static_cast<double>(o.int128_value(trx));

    case +Object::Type::UInt128:
        return static_cast<double>(o.uint128_value(trx));

    case +Object::Type::Real:
        return static_cast<double>(o.real_value());

    case +Object::Type::Double:
        return o.double_value(trx);

    default:
        return 0.0;
    }
}

// index-of: arr any :- int
// Returns the index of the first matching element, or -1.
// throws: opstack-underflow, type-check
static void indexof_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyArrays);

    auto needle_ptr = trx->m_op_ptr;
    auto arr_ptr = (needle_ptr - 1);
    auto arr_len = arr_ptr->arrays_length();

    if (arr_ptr->is_array()) {
        auto elem_ptr = arr_ptr->array_objects(trx);
        for (length_t i = 0; i < arr_len; ++i) {
            if (elem_ptr[i].equal(trx, *needle_ptr)) {
                needle_ptr->maybe_free_extvalue(trx);
                --trx->m_op_ptr;
                *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(i));
                return;
            }
        }
    } else {
        auto packed_data = arr_ptr->const_packed_span(trx);
        auto data_ptr = packed_data.data();
        for (length_t i = 0; i < arr_len; ++i) {
            auto [next, element] = Object::extract_next_packed(trx, data_ptr);
            if (element.equal(trx, *needle_ptr)) {
                needle_ptr->maybe_free_extvalue(trx);
                --trx->m_op_ptr;
                *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(i));
                return;
            } else {
                data_ptr = next;
            }
        }
    }
    needle_ptr->maybe_free_extvalue(trx);
    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_integer(-1);
}

// zip: arr1 arr2 :- arr-of-pairs
// Combines two arrays pairwise into an array of 2-element sub-arrays.
// Result length is min(len1, len2); excess elements are dropped.
// throws: vm-full, opstack-underflow, type-check
static void zip_op(Trix *trx) {
    trx->verify_operands(VerifyArrays, VerifyArrays);

    auto arr2_ptr = trx->m_op_ptr;
    auto arr1_ptr = (arr2_ptr - 1);
    auto len1 = arr1_ptr->arrays_length();
    auto len2 = arr2_ptr->arrays_length();
    auto count = std::min(len1, len2);

    if (count == 0) {
        auto [ptr, offset] = trx->vm_alloc_n<Object>(0);
        --trx->m_op_ptr;
        *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(offset, 0));
    } else {
        // expand packed arrays to temp buffer for indexed access
        auto saved_temp = trx->m_vm_temp_ptr;
        auto [src1_ptr, src1_buf] = expand_packed_to_temp(trx, arr1_ptr, len1);
        auto [src2_ptr, src2_buf] = expand_packed_to_temp(trx, arr2_ptr, len2);

        // allocate result array (count slots).  Element clones are make_clone_local:
        // this op holds the temp packed-expansion buffers (src1_buf/src2_buf) live
        // across the whole build, and a GLOBAL alloc under ${...} would grow the
        // global region down over them (see expand_packed_to_temp).  Keeping every
        // clone LOCAL means no global alloc fires -- so no temp clobber, no GC, no
        // rooting -- exactly as the (working) top-level path allocates.  The result
        // is a local array (the region-inconsistency tracked for computed results).
        auto [result_ptr, result_offset] = trx->vm_alloc_n<Object>(count);

        // allocate all pair sub-arrays (count x 2-element arrays)
        for (length_t i = 0; i < count; ++i) {
            auto [pair_ptr, pair_offset] = trx->vm_alloc_n<Object>(2);
            pair_ptr[0] = src1_ptr[i].make_clone_local(trx);
            pair_ptr[1] = src2_ptr[i].make_clone_local(trx);
            result_ptr[i] = Object::make_array(pair_offset, 2);
        }

        // free temporary packed expansions
        release_packed_temp(trx, src1_buf, len1);
        release_packed_temp(trx, src2_buf, len2);
        trx->vm_temp_restore(saved_temp);

        --trx->m_op_ptr;
        *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(result_offset, count));
    }
}

// zip-longest: arr1 arr2 fill :- arr-of-pairs
// Like zip but uses fill-value for the shorter array.  Result length is max(len1,len2).
// throws: vm-full, opstack-underflow, type-check
static void zip_longest_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyArrays, VerifyArrays);

    auto fill_ptr = trx->m_op_ptr;
    auto arr2_ptr = (fill_ptr - 1);
    auto arr1_ptr = (fill_ptr - 2);
    auto len1 = arr1_ptr->arrays_length();
    auto len2 = arr2_ptr->arrays_length();
    auto count = std::max(len1, len2);
    auto fill_obj = *fill_ptr;

    if (count == 0) {
        auto [ptr, offset] = trx->vm_alloc_n<Object>(0);
        trx->m_op_ptr -= 3;
        *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(offset, 0));
    } else {
        // expand packed arrays to temp buffer for indexed access
        auto saved_temp = trx->m_vm_temp_ptr;
        auto [src1_ptr, src1_buf] = expand_packed_to_temp(trx, arr1_ptr, len1);
        auto [src2_ptr, src2_buf] = expand_packed_to_temp(trx, arr2_ptr, len2);

        // allocate result array.  Element clones are make_clone_local: temp packed-
        // expansion buffers are live across the build, so all clones stay LOCAL to
        // avoid clobbering them under ${...} (see zip_op).  Result is a local array.
        auto [result_ptr, result_offset] = trx->vm_alloc_n<Object>(count);

        // build pairs, using fill-value when past the end of either source
        for (length_t i = 0; i < count; ++i) {
            auto [pair_ptr, pair_offset] = trx->vm_alloc_n<Object>(2);
            pair_ptr[0] = (i < len1) ? src1_ptr[i].make_clone_local(trx) : fill_obj.make_clone_local(trx);
            pair_ptr[1] = (i < len2) ? src2_ptr[i].make_clone_local(trx) : fill_obj.make_clone_local(trx);
            result_ptr[i] = Object::make_array(pair_offset, 2);
        }

        // free temporary packed expansions
        release_packed_temp(trx, src1_buf, len1);
        release_packed_temp(trx, src2_buf, len2);
        trx->vm_temp_restore(saved_temp);

        trx->m_op_ptr -= 3;
        *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(result_offset, count));
    }
}

// Shared implementation for intersect and difference.
// keep_if_found == true:  keep arr1 elements that ARE in arr2 (intersect)
// keep_if_found == false: keep arr1 elements that are NOT in arr2 (difference)
static void intersect_difference_impl(Trix *trx, bool keep_if_found) {
    trx->verify_operands(VerifyArrays, VerifyArrays);

    auto arr2_ptr = trx->m_op_ptr;
    auto arr1_ptr = (arr2_ptr - 1);
    auto len1 = arr1_ptr->arrays_length();
    auto len2 = arr2_ptr->arrays_length();

    if ((len1 == 0) || (keep_if_found && (len2 == 0))) {
        auto [ptr, offset] = trx->vm_alloc_n<Object>(0);
        --trx->m_op_ptr;
        *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(offset, 0));
    } else {
        // expand packed arrays to temp buffer for indexed access
        auto saved_temp = trx->m_vm_temp_ptr;
        auto [src1_ptr, src1_buf] = expand_packed_to_temp(trx, arr1_ptr, len1);
        auto [src2_ptr, src2_buf] = expand_packed_to_temp(trx, arr2_ptr, len2);

        // Reject null elements before using them as dict keys.  Trix arrays
        // can legitimately contain null; treating null as a key would trip
        // Dict::get's defensive assert.
        auto cleanup = [&]() {
            release_packed_temp(trx, src1_buf, len1);
            release_packed_temp(trx, src2_buf, len2);
            trx->vm_temp_restore(saved_temp);
        };
        auto op_name = keep_if_found ? "intersect" : "difference";
        auto bad1 = trx->find_invalid_key_index(src1_ptr, len1);
        if (bad1.index < len1) {
            cleanup();
            trx->error(bad1.err, "{}: arr1 element {} is {} and cannot be used as a dict key", op_name, bad1.index, bad1.reason);
        } else {
            auto bad2 = trx->find_invalid_key_index(src2_ptr, len2);
            if (bad2.index < len2) {
                cleanup();
                trx->error(
                        bad2.err, "{}: arr2 element {} is {} and cannot be used as a dict key", op_name, bad2.index, bad2.reason);
            } else {
                // This op holds temp-region dicts (lookup/seen) live across the whole scan.
                // The temp region sits directly below m_vm_global, and a GLOBAL allocation
                // grows the global region DOWNWARD over it (gvm_alloc only bounds-checks the
                // local heap top, not the temp region), clobbering the live temp dicts.  So
                // under ${...} every clone here must stay LOCAL -- make_clone_local for the
                // membership keys AND the result elements -- exactly as the (working) top-
                // level path allocates.  The result is therefore a local array (the same
                // region-inconsistency tracked for the other computed-array results); no
                // global alloc fires, so no GC and no result rooting is needed.
                auto lookup_capacity = (len2 > 0) ? len2 : length_t{1};
                auto [lookup, lookup_offset] = Dict::create_temp_dict(trx, lookup_capacity);
                for (length_t i = 0; i < len2; ++i) {
                    if (lookup->get(trx, &src2_ptr[i]) == nullptr) {
                        lookup->put(trx, src2_ptr[i].make_clone_local(trx), Object::make_boolean(true));
                    }
                }

                // scan arr1: keep or reject elements based on lookup membership (first occurrence only)
                auto [seen, seen_offset] = Dict::create_temp_dict(trx, len1);
                auto [result_ptr, result_offset] = trx->vm_alloc_n<Object>(len1);
                length_t write = 0;
                for (length_t i = 0; i < len1; ++i) {
                    auto in_lookup = (lookup->get(trx, &src1_ptr[i]) != nullptr);
                    if ((in_lookup == keep_if_found) && (seen->get(trx, &src1_ptr[i]) == nullptr)) {
                        result_ptr[write++] = src1_ptr[i].make_clone_local(trx);
                        seen->put(trx, src1_ptr[i].make_clone_local(trx), Object::make_boolean(true));
                    }
                }

                // trim result
                auto unused = static_cast<vm_size_t>((len1 - write) * vm_sizeof<Object>());
                if (unused != 0) {
                    trx->vm_trim_alloc(reinterpret_cast<vm_t *>(result_ptr + len1), unused);
                }

                // free temporary packed expansions
                release_packed_temp(trx, src1_buf, len1);
                release_packed_temp(trx, src2_buf, len2);
                trx->vm_temp_restore(saved_temp);

                --trx->m_op_ptr;
                *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(result_offset, write));
            }
        }
    }
}

// intersect: arr1 arr2 :- arr
// Returns elements in arr1 that also appear in arr2, preserving arr1 order.
// Removes duplicates.  Elements must be hashable (no Null/Array/Packed/Dict).
// throws: vm-full, opstack-underflow, type-check
static void intersect_op(Trix *trx) {
    intersect_difference_impl(trx, true);
}

// union: arr1 arr2 :- arr
// Returns unique elements from arr1 followed by unique elements from arr2 not in arr1.
// Preserves first-occurrence order.
// throws: vm-full, opstack-underflow, type-check
static void union_op(Trix *trx) {
    trx->verify_operands(VerifyArrays, VerifyArrays);

    auto arr2_ptr = trx->m_op_ptr;
    auto arr1_ptr = (arr2_ptr - 1);
    auto len1 = arr1_ptr->arrays_length();
    auto len2 = arr2_ptr->arrays_length();

    if ((len1 == 0) && (len2 == 0)) {
        auto [ptr, offset] = trx->vm_alloc_n<Object>(0);
        --trx->m_op_ptr;
        *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(offset, 0));
    } else {
        // expand packed arrays to temp buffer for indexed access
        auto saved_temp = trx->m_vm_temp_ptr;
        auto [src1_ptr, src1_buf] = expand_packed_to_temp(trx, arr1_ptr, len1);
        auto [src2_ptr, src2_buf] = expand_packed_to_temp(trx, arr2_ptr, len2);

        // Reject null elements (not valid dict keys).
        auto cleanup = [&]() {
            release_packed_temp(trx, src1_buf, len1);
            release_packed_temp(trx, src2_buf, len2);
            trx->vm_temp_restore(saved_temp);
        };
        auto bad1 = trx->find_invalid_key_index(src1_ptr, len1);
        if (bad1.index < len1) {
            cleanup();
            trx->error(bad1.err, "union: arr1 element {} is {} and cannot be used as a dict key", bad1.index, bad1.reason);
        } else {
            auto bad2 = trx->find_invalid_key_index(src2_ptr, len2);
            if (bad2.index < len2) {
                cleanup();
                trx->error(bad2.err, "union: arr2 element {} is {} and cannot be used as a dict key", bad2.index, bad2.reason);
            } else {
                // build seen dict (temp region) and result array
                auto total = static_cast<vm_size_t>(len1) + len2;
                if (total > MaxArrayLength) {
                    cleanup();
                    trx->error(Error::LimitCheck, "union: result length {} exceeds maximum {}", total, MaxArrayLength);
                } else {
                    auto max_count = static_cast<length_t>(total);
                    auto [seen, seen_offset] = Dict::create_temp_dict(trx, max_count);
                    auto [result_ptr, result_offset] = trx->vm_alloc_n<Object>(max_count);
                    // `seen` is a temp-region dict held live across both scans; a GLOBAL alloc
                    // would grow the global region down over it (see intersect_difference_impl),
                    // so every clone here stays LOCAL -- result elements and membership keys
                    // alike.  The result is a local array (tracked region-inconsistency); no
                    // global alloc fires, so no GC and no rooting is needed.
                    length_t write = 0;

                    // add unique elements from arr1
                    for (length_t i = 0; i < len1; ++i) {
                        if (seen->get(trx, &src1_ptr[i]) == nullptr) {
                            result_ptr[write++] = src1_ptr[i].make_clone_local(trx);
                            seen->put(trx, src1_ptr[i].make_clone_local(trx), Object::make_boolean(true));
                        }
                    }

                    // add unique elements from arr2 not already in seen
                    for (length_t i = 0; i < len2; ++i) {
                        if (seen->get(trx, &src2_ptr[i]) == nullptr) {
                            result_ptr[write++] = src2_ptr[i].make_clone_local(trx);
                            seen->put(trx, src2_ptr[i].make_clone_local(trx), Object::make_boolean(true));
                        }
                    }

                    // trim result
                    auto unused = static_cast<vm_size_t>((max_count - write) * vm_sizeof<Object>());
                    if (unused != 0) {
                        trx->vm_trim_alloc(reinterpret_cast<vm_t *>(result_ptr + max_count), unused);
                    }

                    // free temporary packed expansions
                    release_packed_temp(trx, src1_buf, len1);
                    release_packed_temp(trx, src2_buf, len2);
                    trx->vm_temp_restore(saved_temp);

                    --trx->m_op_ptr;
                    *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(result_offset, write));
                }
            }
        }
    }
}

// difference: arr1 arr2 :- arr
// Returns elements in arr1 that do NOT appear in arr2, preserving arr1 order.
// Removes duplicates.
// throws: vm-full, opstack-underflow, type-check
static void difference_op(Trix *trx) {
    intersect_difference_impl(trx, false);
}

// interpose: arr val :- arr'
// Inserts val between each element: [1 2 3] 0 => [1 0 2 0 3]
// throws: vm-full, opstack-underflow, type-check
static void interpose_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyArrays);

    auto separator_ptr = trx->m_op_ptr;
    auto container_ptr = (separator_ptr - 1);
    auto count = container_ptr->arrays_length();

    if (count <= 1) {
        // 0 or 1 elements: return copy as-is, no separator needed
        if (count == 0) {
            auto [ptr, offset] = trx->vm_alloc_n<Object>(0);
            trx->m_op_ptr -= 2;
            *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(offset, 0));
        } else {
            // single element
            Object *src_ptr{nullptr};
            if (container_ptr->is_array()) {
                src_ptr = container_ptr->array_objects(trx);
            } else {
                auto [buf, _] = trx->vm_alloc_n<Object>(1);
                clone_array_elements(trx, container_ptr, buf, 1);
                src_ptr = buf;
            }
            auto [dst_ptr, dst_offset] = trx->vm_alloc_n<Object>(1);
            dst_ptr[0] = src_ptr[0].make_clone_local(trx);  // local: see else-branch
            trx->m_op_ptr -= 2;
            *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(dst_offset, 1));
        }
    } else {
        // expand packed to temp buffer if needed
        auto saved_temp = trx->m_vm_temp_ptr;
        auto [src_ptr, src_buf] = expand_packed_to_temp(trx, container_ptr, count);

        auto sep_obj = *separator_ptr;
        auto total = static_cast<vm_size_t>(count) * 2 - 1;
        if (total > MaxArrayLength) {
            release_packed_temp(trx, src_buf, count);
            trx->vm_temp_restore(saved_temp);
            trx->error(Error::LimitCheck, "interpose: result length {} exceeds maximum {}", total, MaxArrayLength);
        } else {
            auto result_len = static_cast<length_t>(total);
            auto [dst_ptr, dst_offset] = trx->vm_alloc_n<Object>(result_len);
            // Element/separator clones are make_clone_local: the temp packed-expansion
            // buffer (src_buf) is live across the build, so all clones stay LOCAL to
            // avoid clobbering it under ${...} (see zip_op / expand_packed_to_temp).
            length_t wi = 0;
            for (length_t i = 0; i < count; ++i) {
                if (i > 0) {
                    dst_ptr[wi] = sep_obj.make_clone_local(trx);
                    ++wi;
                }
                dst_ptr[wi] = src_ptr[i].make_clone_local(trx);
                ++wi;
            }

            // free temporary packed expansion
            release_packed_temp(trx, src_buf, count);
            trx->vm_temp_restore(saved_temp);

            trx->m_op_ptr -= 2;
            *++trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(dst_offset, result_len));
        }
    }
}

// enumerate: arr :- arr-of-pairs
// Pairs each element with its 0-based index: [x y z] => [[0 x] [1 y] [2 z]]
// throws: vm-full, opstack-underflow, type-check
static void enumerate_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);

    auto src_ptr = trx->m_op_ptr;
    auto count = src_ptr->arrays_length();

    if (count == 0) {
        auto [ptr, offset] = trx->vm_alloc_n<Object>(0);
        *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(offset, 0));
    } else {
        // expand packed to temp buffer for indexed access
        auto saved_temp = trx->m_vm_temp_ptr;
        auto [expanded_ptr, src_buf] = expand_packed_to_temp(trx, src_ptr, count);

        // allocate result array.  Element clones are make_clone_local: the temp
        // packed-expansion buffer is live across the build, so all clones stay LOCAL
        // to avoid clobbering it under ${...} (see zip_op / expand_packed_to_temp).
        auto [result_ptr, result_offset] = trx->vm_alloc_n<Object>(count);

        // allocate pair sub-arrays
        for (length_t i = 0; i < count; ++i) {
            auto [pair_ptr, pair_offset] = trx->vm_alloc_n<Object>(2);
            pair_ptr[0] = Object::make_integer(static_cast<integer_t>(i));
            pair_ptr[1] = expanded_ptr[i].make_clone_local(trx);
            result_ptr[i] = Object::make_array(pair_offset, 2);
        }

        // free temporary packed expansion
        release_packed_temp(trx, src_buf, count);
        trx->vm_temp_restore(saved_temp);

        *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(result_offset, count));
    }
}

// chunk: arr int chunk -- arr-of-arrs
// Splits array into fixed-size groups; last group may be shorter.
// throws: opstack-underflow, type-check, range-check, vm-full
static void chunk_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyArrays);

    auto size_obj = trx->m_op_ptr;
    auto container_ptr = (size_obj - 1);
    auto chunk_size_int = size_obj->integer_value();
    auto count = container_ptr->arrays_length();

    if (chunk_size_int <= 0) {
        trx->error(Error::RangeCheck, "chunk: chunk size must be positive");
    } else {
        auto chunk_size = static_cast<length_t>(chunk_size_int);

        if (count == 0) {
            auto [_, offset] = trx->vm_alloc_n<Object>(0);
            --trx->m_op_ptr;
            *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(offset, 0));
        } else {
            // expand packed to temp buffer if needed
            auto saved_temp = trx->m_vm_temp_ptr;
            auto [src_ptr, src_buf] = expand_packed_to_temp(trx, container_ptr, count);

            // compute number of groups
            auto num_groups = static_cast<length_t>((count + chunk_size - 1) / chunk_size);

            // allocate result array + all sub-arrays.  Element clones are
            // make_clone_local: the temp packed-expansion buffer is live across the
            // build, so all clones stay LOCAL to avoid clobbering it under ${...}
            // (see zip_op / expand_packed_to_temp).
            auto [result_ptr, result_offset] = trx->vm_alloc_n<Object>(num_groups);

            for (length_t g = 0; g < num_groups; ++g) {
                auto start = static_cast<length_t>(g * chunk_size);
                auto remaining = static_cast<length_t>(count - start);
                auto group_len = (remaining < chunk_size) ? remaining : chunk_size;

                auto [group_ptr, group_offset] = trx->vm_alloc_n<Object>(group_len);
                for (length_t i = 0; i < group_len; ++i) {
                    group_ptr[i] = src_ptr[start + i].make_clone_local(trx);
                }
                result_ptr[g] = Object::make_array(group_offset, group_len);
            }

            // free temporary packed expansion
            release_packed_temp(trx, src_buf, count);
            trx->vm_temp_restore(saved_temp);

            --trx->m_op_ptr;
            *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(result_offset, num_groups));
        }
    }
}

// sliding-window: arr int sliding-window -- arr-of-arrs
// Produces overlapping sub-sequences of the given window size.
// throws: opstack-underflow, type-check, range-check, vm-full
static void sliding_window_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyArrays);

    auto size_obj = trx->m_op_ptr;
    auto container_ptr = (size_obj - 1);
    auto window_int = size_obj->integer_value();
    auto count = container_ptr->arrays_length();

    if (window_int <= 0) {
        trx->error(Error::RangeCheck, "sliding-window: window size must be positive");
    } else {
        auto window = static_cast<length_t>(window_int);

        if (window > count) {
            // window larger than array: return empty
            auto [_, offset] = trx->vm_alloc_n<Object>(0);
            --trx->m_op_ptr;
            *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(offset, 0));
        } else {
            // expand packed to temp buffer if needed
            auto saved_temp = trx->m_vm_temp_ptr;
            auto [src_ptr, src_buf] = expand_packed_to_temp(trx, container_ptr, count);

            // number of windows: count - window + 1
            auto num_windows = static_cast<length_t>(count - window + 1);

            // allocate result array + all sub-arrays.  Element clones are
            // make_clone_local: the temp packed-expansion buffer is live across the
            // build, so all clones stay LOCAL to avoid clobbering it under ${...}
            // (see zip_op / expand_packed_to_temp).
            auto [result_ptr, result_offset] = trx->vm_alloc_n<Object>(num_windows);

            for (length_t w = 0; w < num_windows; ++w) {
                auto [win_ptr, win_offset] = trx->vm_alloc_n<Object>(window);
                for (length_t i = 0; i < window; ++i) {
                    win_ptr[i] = src_ptr[w + i].make_clone_local(trx);
                }
                result_ptr[w] = Object::make_array(win_offset, window);
            }

            // free temporary packed expansion
            release_packed_temp(trx, src_buf, count);
            trx->vm_temp_restore(saved_temp);

            --trx->m_op_ptr;
            *trx->m_op_ptr = promote_result_if_global(trx, Object::make_array(result_offset, num_windows));
        }
    }
}

// frequencies: arr frequencies -- dict
// Counts occurrences of each element, returning a dynamic dictionary.
// throws: opstack-underflow, type-check, vm-full
static void frequencies_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);

    auto container_ptr = trx->m_op_ptr;
    auto count = container_ptr->arrays_length();

    if (count == 0) {
        auto [dict, offset] = Dict::create_dict(trx, 0, Object::DictMode::ReadWriteDynamic);
        *container_ptr = promote_result_if_global(trx, Object::make_dict(offset));
    } else {
        // expand packed to temp buffer if needed
        auto saved_temp = trx->m_vm_temp_ptr;
        auto [src_ptr, src_buf] = expand_packed_to_temp(trx, container_ptr, count);

        // Reject null/NaN/Inf elements (not valid dict keys).
        auto bad = trx->find_invalid_key_index(src_ptr, count);
        if (bad.index < count) {
            release_packed_temp(trx, src_buf, count);
            trx->vm_temp_restore(saved_temp);
            trx->error(bad.err, "frequencies: element {} is {} and cannot be used as a dict key", bad.index, bad.reason);
        } else {
            // create dynamic dict with initial capacity
            auto [dict, dict_offset] = Dict::create_dict(trx, count, Object::DictMode::ReadWriteDynamic);

            // count each element
            for (length_t i = 0; i < count; ++i) {
                auto existing = dict->get(trx, &src_ptr[i]);
                if (existing != nullptr) {
                    existing->update_integer(existing->integer_value() + 1);
                } else {
                    // New key: store an independent clone so the persistent dict
                    // owns its own ExtValue.  A non-packed source's src_ptr[i] is
                    // the source array's own element -- putting it un-cloned would
                    // alias the array's ExtValue (later key-free -> UAF/double-free).
                    // make_clone_local: for a packed source the temp expansion buffer
                    // (src_buf) is live here, so a GLOBAL clone under ${...} would grow
                    // the global region down over it (see expand_packed_to_temp); the
                    // dict is local, so a local key is region-consistent.
                    dict->put(trx, src_ptr[i].make_clone_local(trx), Object::make_integer(1));
                }
                // The dict now holds its own clone in both branches, so the packed
                // temp clone is never transferred out; free it every iteration.
                // No-op (src_buf == nullptr) for a non-packed source, whose
                // elements remain owned by the source array.
                if (src_buf != nullptr) {
                    src_buf[i].maybe_free_extvalue(trx);
                }
            }
            trx->vm_temp_restore(saved_temp);

            *container_ptr = promote_result_if_global(trx, Object::make_dict(dict_offset));
        }
    }
}

// array-from-mark: mark any0 ... anyN :- arr
// Collects operands above mark into a new array.
// throws: vm-full, unmatched-mark
static void arrayfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_array_from_mark(trx);
}

// dict-from-mark: mark key1 val1 ... :- dict
// Collects key/value pairs above mark into a new dict.
// throws: vm-full, dict-full, range-check, type-check, unmatched-mark
static void dictfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_dict_from_mark(trx);
}

// ro-dict-from-mark: mark key1 val1 ... :- dict
// Collects key/value pairs above mark into a new read-only dict.
// throws: vm-full, dict-full, range-check, type-check, unmatched-mark
static void readonlydictfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_dict_from_mark(trx, Object::DictMode::ReadOnly);
}

// =dict-from-mark: mark key1 val1 ... :- dict
// Collects key/value pairs above mark into a new =dict.
// throws: vm-full, dict-full, range-check, type-check, unmatched-mark
static void eqdictfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_eqdict_from_mark(trx);
}

// ro-=dict-from-mark: mark key1 val1 ... :- dict
// Collects key/value pairs above mark into a new read-only =dict.
// throws: vm-full, dict-full, range-check, type-check, unmatched-mark
static void readonlyeqdictfrommark_op(Trix *trx) {
    *++trx->m_op_ptr = Object::make_eqdict_from_mark(trx, Object::DictMode::ReadOnly);
}

// make-literal: any :- any
// Sets the top operand to literal.
// Curry must remain executable (packed encoding relies on X=1).
// Address uses X for cache state (not execution attribute).
// throws: opstack-underflow, type-check
static void make_literal_op(Trix *trx) {
    trx->verify_operands(VerifyLiteral);

    trx->m_op_ptr->set_literal();
}

// put: container key any :- --
// Stores a value at key (dict) or index (array/string).
// throws: vm-full, dict-full, index-check, limit-check, opstack-underflow, type-check
static void put_op(Trix *trx) {
    trx->require_op_count(3);

    auto any_ptr = trx->m_op_ptr;
    auto index_ptr = any_ptr - 1;
    auto container_ptr = index_ptr - 1;

    if (container_ptr->is_dict()) {
        trx->verify_operands(VerifyAny, VerifyKey, VerifyRWDict);

        auto dict = container_ptr->dict_value(trx);
        dict->put(trx, *index_ptr, *any_ptr);
    } else {
        trx->verify_operands(VerifyAny, VerifyIntegers, VerifyRWArray | VerifyRWString);

        auto length = container_ptr->object_length();
        auto [valid, index] = index_ptr->integer_value(trx, -static_cast<integer_t>(length), MaxLength);
        if (valid) {
            // negative indexing: -1 = last, -2 = second-to-last, etc.
            if (index < 0) {
                index = index + static_cast<integer_t>(length);
            }
            if ((index >= 0) && (index < length)) {
                if (container_ptr->is_string()) {
                    trx->verify_operands(VerifyByte, VerifyIntegers, VerifyRWString);

                    auto b = any_ptr->byte_value();
                    auto string_ptr = container_ptr->string_vptr(trx);
                    string_ptr[index] = b;
                } else {
                    // R6 pointer hygiene: a global array clones a fragile-local SCALAR
                    // value into global and rejects a NON-scalar fragile-local (see
                    // Dict::put).  The value is on the op stack -- reject first (no
                    // free), then clone and WRITE THE CLONE BACK to that slot, so it
                    // stays GC-rooted and the freed original no longer aliases the
                    // operand (throw-safe).
                    auto dst_global = trx->is_global(container_ptr->offset());
                    Save::reject_local_into_global(trx, dst_global, *any_ptr, "put");
                    *any_ptr = Save::clone_fragile_scalar_into_global(trx, dst_global, *any_ptr);
                    auto curr_save_level = trx->m_curr_save_level;
                    auto dst_ptr = container_ptr->array_objects(trx) + index;
                    if (container_ptr->is_eqarray(trx)) {
                        Save::reject_stale_value_into_eqref(trx, *any_ptr, "put");
                        dst_ptr->maybe_free_extvalue(trx);
                    } else if (dst_ptr->save_level() == curr_save_level) {
                        dst_ptr->maybe_free_extvalue(trx);
                    } else {
                        Save::save_object(trx, dst_ptr);
                    }
                    *dst_ptr = any_ptr->make_copy(curr_save_level);
                }
                index_ptr->maybe_free_extvalue(trx);
            } else {
                trx->error(Error::IndexCheck, "index {} exceeds container length {}", index, length);
            }
        } else {
            trx->error(Error::LimitCheck, "operand index {} is not in the range of -{}..{}", index, length, MaxLength);
        }
    }

    trx->m_op_ptr = (container_ptr - 1);
}

// put-persist: container key any :- --
// Like put, but the mutation is NOT journaled -- it persists across the
// enclosing save/restore boundary.  Intended for long-running stateful
// actors that wrap each iteration in save/restore.
//
//   At sl=0: silently degrades to put (no save active, no journal needed).
//   At sl > 0:
//     - rejects /above-barrier if the value being stored would be
//       reclaimed by the next restore (vm storage above the save barrier,
//       or ExtValue cell allocated at the current save level).
//     - dict path: overwrites existing entries; missing keys are CREATED
//       (new DictEntry allocated in global VM so the binding
//       survives restore).
//     - array path: in-place store at index, no journal call.
//     - string path: identical to put (string byte writes are already
//       non-journaling).
// throws: above-barrier, vm-full, dict-full, index-check, limit-check, opstack-underflow, type-check
static void put_persist_op(Trix *trx) {
    trx->require_op_count(3);

    auto any_ptr = trx->m_op_ptr;
    auto index_ptr = any_ptr - 1;
    auto container_ptr = index_ptr - 1;

    auto save_active = Save::is_active(trx);
    if (save_active && Save::is_above_barrier(trx, *any_ptr)) {
        trx->error(Error::AboveBarrier, "put-persist: value lives above the save barrier and would dangle on restore");
    } else {
        if (container_ptr->is_dict()) {
            trx->verify_operands(VerifyAny, VerifyKey, VerifyRWDict);

            auto dict = container_ptr->dict_value(trx);
            if (save_active) {
                if (Save::is_above_barrier(trx, *index_ptr)) {
                    trx->error(Error::AboveBarrier, "put-persist: key lives above the save barrier");
                } else {
                    dict->put_persist_or_create(trx, *index_ptr, *any_ptr);
                }
            } else {
                // sl=0: silent degrade to put.
                dict->put(trx, *index_ptr, *any_ptr);
            }
        } else {
            trx->verify_operands(VerifyAny, VerifyIntegers, VerifyRWArray | VerifyRWString);

            auto length = container_ptr->object_length();
            auto [valid, index] = index_ptr->integer_value(trx, -static_cast<integer_t>(length), MaxLength);
            if (valid) {
                if (index < 0) {
                    index = index + static_cast<integer_t>(length);
                }
                if ((index >= 0) && (index < length)) {
                    if (container_ptr->is_string()) {
                        trx->verify_operands(VerifyByte, VerifyIntegers, VerifyRWString);

                        auto b = any_ptr->byte_value();
                        auto string_ptr = container_ptr->string_vptr(trx);
                        string_ptr[index] = b;
                    } else {
                        // R6 pointer hygiene: a global array clones a fragile-local SCALAR
                        // value into global, rejects a NON-scalar fragile-local (reject
                        // first, then clone + write back to the op-stack slot; see Dict::put).
                        auto dst_global = trx->is_global(container_ptr->offset());
                        Save::reject_local_into_global(trx, dst_global, *any_ptr, "put-persist");
                        *any_ptr = Save::clone_fragile_scalar_into_global(trx, dst_global, *any_ptr);
                        auto dst_ptr = container_ptr->array_objects(trx) + index;
                        if (container_ptr->is_eqarray(trx)) {
                            Save::reject_stale_value_into_eqref(trx, *any_ptr, "put-persist");
                        }
                        // No Save::save_object call: the overwrite must persist
                        // across the enclosing save/restore.  Free the previous
                        // ExtValue (if any) since the journal will not preserve it.
                        // Stamp BASE on the new value so a later put() at sl=0
                        // doesn't see a stale save_level above current and try
                        // to journal at a vanished level -- see Dict::put_persist
                        // for the same fix.
                        dst_ptr->maybe_free_extvalue(trx);
                        *dst_ptr = any_ptr->make_copy(Save::BASE);
                    }
                    index_ptr->maybe_free_extvalue(trx);
                } else {
                    trx->error(Error::IndexCheck, "index {} exceeds container length {}", index, length);
                }
            } else {
                trx->error(Error::LimitCheck, "operand index {} is not in the range of -{}..{}", index, length, MaxLength);
            }
        }

        trx->m_op_ptr = (container_ptr - 1);
    }
}

// type: any :- name
// Pushes the type name of the top operand.
// throws: opstack-underflow
static void type_op(Trix *trx) {
    trx->require_op_count(1);

    auto top_ptr = trx->m_op_ptr;
    auto type = top_ptr->type();
    top_ptr->maybe_free_extvalue(trx);
    *top_ptr = trx->type_name(type);
}
