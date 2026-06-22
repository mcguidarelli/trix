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

//===--- Type Conversion, Comparison, and Predicate Operators ---===//
//
// Implements type casting, attribute modification, type/attribute predicates,
// deep structural equality, and type-name resolution.  Based on:
//
//   PostScript type operators: type, cvn (to-name), cvs (to-string), cvi
//   (to integer), cvr (to real), executeonly, readonly, rcheck, wcheck,
//   xcheck.  Trix extends these with cast (explicit numeric coercion),
//   reinterpret (bit-preserving type change), and deep-eq (recursive
//   structural comparison).
//
// --- Core concepts for maintainers ---
//
// TYPE CASTING (cast, reinterpret)
//   cast: numeric type coercion with value conversion (e.g., real to integer
//     truncates, integer to double promotes).  Raises TypeCheck for
//     non-numeric types.  Uses cast_to_type<T> for the per-type numeric
//     conversion via static_cast; out-of-range values truncate or wrap
//     silently (no overflow detection or saturation).
//   reinterpret: changes the type tag without converting the value.  The raw
//     bit pattern is preserved (e.g., integer 0x3F800000 reinterpreted as
//     real gives 1.0).  Only valid between same-width numeric types.
//
// PROMOTE CONVERSION
//   promote_convert performs explicit widening when the user calls the
//   `promote` operator (e.g., byte to integer, real to double).  It is
//   NOT used implicitly by arithmetic or comparison operators -- Trix
//   requires matched types for all binary operations.
//
// DEEP EQUALITY (deep-eq, deep-ne)
//   Recursive structural comparison of compound types: arrays/packed,
//   records, sets, dicts, tagged values, curries, and thunks.
//   Depth-limited by MaxRecursionDepth to guard
//   against cycles.  Leaf values use Object::equal() (shallow).
//
// ATTRIBUTE MODIFICATION
//   make-executable / make-literal:  set/clear the Execute bit
//   make-readonly:  set the Access bit to read-only (one-way; there is no
//                   make-readwrite operator to clear it)
//   These modify the attribute bits of the top operand in place (Object is a
//   value type; the operand-stack cell is overwritten, not cloned).
//
// TYPE PREDICATES
//   is-<type>: any -- bool (per-type tests: is-array, is-byte, is-integer,
//              is-number, ... one for each Object type, no generic is-type)
//   type: any -- name (returns the type name of an object)
//   All 31 types (Object::TypeCount) have a corresponding type name in systemdict.
//
// Type casting (cast, reinterpret), type promotion, numeric conversion
// (to-name, to-number, to-string), attribute modification (make-executable,
// make-readonly), attribute predicates (executable?, readable?, writable?),
// type predicates (is-number), equality comparison (deep-eq, deep-ne),
// element coercion (coerce), dict merge (merge), type-name / error-name lookup.
//

// make-executable: any :- any
// Sets the top operand to executable.
// Tagged must remain literal (packed encoding relies on X=0).
// Address uses X for cache state (not execution attribute).
// throws: opstack-underflow, type-check
static void make_executable_op(Trix *trx) {
    trx->verify_operands(VerifyExecutable);

    trx->m_op_ptr->set_executable();
}

// to-name: str :- name
// Converts a string to a name.
// throws: vm-full, range-check, limit-check, opstack-underflow, type-check
static void to_name_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto str_ptr = trx->m_op_ptr;
    auto sv = str_ptr->sv_value(trx);
    auto length = sv_length(sv);

    if (length == 0) {
        // A zero-length Name is an unwritable shadow value: the scanner can
        // never produce one (a lone '/' or '\' becomes the one-char name "/"
        // or "\" by delimiter substitution -- see scan_name), it PRINTS as
        // "/" yet compares unequal to that name, and the empty-name lookup
        // path needed its own m_data[0] over-read guard in name_search.
        // Reject at the only user-controlled creation point.
        trx->error(Error::RangeCheck, "to-name: empty string cannot become a name");
    } else if (length <= MaxNameLength) {
        auto attrib = str_ptr->attrib();
        *str_ptr = Name::make(trx, sv, attrib);
    } else {
        trx->error(Error::LimitCheck, "name length of {} exceeds maximum {}", length, MaxNameLength);
    }
}

// to-number: str :- num
// Parses a string as a number.
// throws: opstack-underflow, type-check
static void to_number_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto str_ptr = trx->m_op_ptr;
    auto sv = str_ptr->sv_value(trx);

    const Number number(trx, sv);
    if (number.is_valid()) {
        *str_ptr = number.make_object();
    } else {
        trx->error(Error::TypeCheck, "to-number: {} is not a valid number", sv);
    }
}

// Recursive deep equality helper. Returns true if two Objects are
// structurally equal -- compares composite types by contents, not identity.
// throws: limit-check (depth > 64)
// Deeply compared: Array/Packed (element-wise), Curry, Tagged, Thunk, Record
// (field-wise), Set (order-independent, O(n^2)), Dict (same keys, deep values).
// deep-eq compares by abstract structure and contents, not by representation.
// Array and Packed are both ordered sequences of elements -- Packed is simply a
// compressed encoding of the same abstract data structure.  All sequence operators
// (get, length, for-all, map, filter, sort-by) treat them polymorphically, and
// deep-eq follows suit: [1 2 3] deep-eq { 1 2 3 } is true if elements match.
// This does NOT extend to cross-type numeric comparison (e.g. 1b vs 1l): value
// types have distinct ranges, overflow semantics, and are explicitly chosen by
// the programmer.

//===--- deep_equal helpers ---===//

// Sequence (Array/Packed): element-by-element recursive comparison.
// Array elements are accessed by pointer; packed elements are extracted one at a time.
// extract_next_packed returns borrowed references for ExtValue types -- the Object
// aliases an ExtValue owned by the packed source, not a new allocation.
static bool deep_equal_sequences(Trix *trx, Object a, Object b, int depth) {
    auto a_length = a.arrays_length();
    if (a_length == b.arrays_length()) {
        auto a_type = a.type();
        auto b_type = b.type();

        const Object *a_ptr = nullptr;
        const packed_data_t *a_data = nullptr;
        if (a_type == Object::Type::Array) {
            a_ptr = a.array_objects(trx);
        } else {
            a_data = trx->offset_to_ptr<const packed_data_t>(a.m_packed);
        }

        const Object *b_ptr = nullptr;
        const packed_data_t *b_data = nullptr;
        if (b_type == Object::Type::Array) {
            b_ptr = b.array_objects(trx);
        } else {
            b_data = trx->offset_to_ptr<const packed_data_t>(b.m_packed);
        }

        for (length_t i = 0; i < a_length; ++i) {
            Object a_elem, b_elem;

            if (a_ptr != nullptr) {
                a_elem = a_ptr[i];
            } else {
                auto [next, elem] = Object::extract_next_packed(trx, a_data);
                a_elem = elem;
                a_data = next;
            }

            if (b_ptr != nullptr) {
                b_elem = b_ptr[i];
            } else {
                auto [next, elem] = Object::extract_next_packed(trx, b_data);
                b_elem = elem;
                b_data = next;
            }

            if (!deep_equal(trx, a_elem, b_elem, depth + 1)) {
                return false;
            }
        }
        return true;
    } else {
        return false;
    }
}

// Record: same schema (field names in same order) + all fields recursively equal.
static bool deep_equal_records(Trix *trx, Object a, Object b, int depth) {
    auto a_count = a.object_length();
    if (a_count == b.object_length()) {
        auto a_inst = a.record_instance(trx);
        auto b_inst = b.record_instance(trx);

        if (a_inst->m_schema != b_inst->m_schema) {
            auto a_sch = trx->offset_to_ptr<RecordSchema>(a_inst->m_schema);
            auto b_sch = trx->offset_to_ptr<RecordSchema>(b_inst->m_schema);
            for (length_t i = 0; i < a_count; ++i) {
                if (!a_sch->m_names[i].equal(trx, b_sch->m_names[i])) {
                    return false;
                }
            }
        }

        for (length_t i = 0; i < a_count; ++i) {
            if (!deep_equal(trx, a_inst->m_fields[i], b_inst->m_fields[i], depth + 1)) {
                return false;
            }
        }
        return true;
    } else {
        return false;
    }
}

// Set: order-independent structural equality.  O(n^2) scan because hash-based
// set_member uses equal(), not deep_equal -- a set containing e.g. [1 2] would
// fail to match a separately-allocated [1 2].
static bool deep_equal_sets(Trix *trx, Object a, Object b, int depth) {
    auto a_set = a.set_value(trx);
    auto b_set = b.set_value(trx);
    if (a_set->length() == b_set->length()) {
        auto a_offset = nulloffset;
        integer_t a_idx = -1;
        while (true) {
            auto [a_next, a_next_idx, a_key] = a_set->set_next(trx, a_offset, a_idx);
            if (a_next == nulloffset) {
                return true;
            } else {
                auto b_offset = nulloffset;
                integer_t b_idx = -1;
                while (true) {
                    auto [b_next, b_next_idx, b_key] = b_set->set_next(trx, b_offset, b_idx);
                    if (b_next == nulloffset) {
                        return false;
                    } else if (deep_equal(trx, a_key, b_key, depth + 1)) {
                        a_offset = a_next;
                        a_idx = a_next_idx;
                        break;
                    } else {
                        b_offset = b_next;
                        b_idx = b_next_idx;
                    }
                }
            }
        }
    } else {
        return false;
    }
}

// Dict: same keys (by equal) with recursively deep-equal values.  Key lookup
// uses hash-based get() (correct for Name/String keys).
static bool deep_equal_dicts(Trix *trx, Object a, Object b, int depth) {
    auto a_dict = a.dict_value(trx);
    auto b_dict = b.dict_value(trx);
    if (a_dict->length() == b_dict->length()) {
        auto entry_offset = nulloffset;
        integer_t bucket_idx = -1;
        while (true) {
            auto [next, idx, key, value] = a_dict->next(trx, entry_offset, bucket_idx);
            if (next == nulloffset) {
                return true;
            } else {
                auto b_value = b_dict->get(trx, key);
                if ((b_value != nullptr) && deep_equal(trx, value, *b_value, depth + 1)) {
                    entry_offset = next;
                    bucket_idx = idx;
                } else {
                    return false;
                }
            }
        }
    } else {
        return false;
    }
}

// Recursive structural equality.  Dispatches to type-specific helpers for
// compound types; delegates to Object::equal() for scalars and identity types.
static bool deep_equal(Trix *trx, Object a, Object b, int depth) {
    if (depth > MaxRecursionDepth) {
        trx->error(Error::LimitCheck, "deep-eq: recursion depth exceeds {}", MaxRecursionDepth);
    } else if (a.is_sequence() && b.is_sequence()) {
        return deep_equal_sequences(trx, a, b, depth);
    } else {
        auto a_type = a.type();

        if ((a_type == b.type()) && a.is_deepeq()) {
            if (a_type == Object::Type::Curry) {
                auto a_pair = a.curry_storage(trx);
                auto b_pair = b.curry_storage(trx);
                return (deep_equal(trx, a_pair[0], b_pair[0], depth + 1) && deep_equal(trx, a_pair[1], b_pair[1], depth + 1));
            } else if (a_type == Object::Type::Tagged) {
                auto a_pair = a.tagged_storage(trx);
                auto b_pair = b.tagged_storage(trx);
                return (deep_equal(trx, a_pair[0], b_pair[0], depth + 1) && deep_equal(trx, a_pair[1], b_pair[1], depth + 1));
            } else if (a_type == Object::Type::Thunk) {
                auto a_storage = a.thunk_storage(trx);
                auto b_storage = b.thunk_storage(trx);
                return (deep_equal(trx, a_storage[Object::ThunkStorageState], b_storage[Object::ThunkStorageState], depth + 1) &&
                        deep_equal(trx, a_storage[Object::ThunkStorageProc], b_storage[Object::ThunkStorageProc], depth + 1) &&
                        deep_equal(trx, a_storage[Object::ThunkStorageResult], b_storage[Object::ThunkStorageResult], depth + 1));
            } else if (a_type == Object::Type::Record) {
                return deep_equal_records(trx, a, b, depth);
            } else if (a_type == Object::Type::Set) {
                return deep_equal_sets(trx, a, b, depth);
            } else if (a_type == Object::Type::Dict) {
                return deep_equal_dicts(trx, a, b, depth);
            }
        }

        // Stream: identity comparison only (via equal()).  Streams are inherently
        // stateful -- position, buffer, EOF state -- so two different streams are
        // never structurally "equal" even if they read the same source.  The sid
        // is a lifecycle token for open/close detection, not a structural property.
        //
        // All other types: delegate to equal()
        return a.equal(trx, b);
    }
}

// Common implementation for deep-eq and deep-ne.
static void compare_deep_eq_impl(Trix *trx, bool negate) {
    trx->require_op_count(2);

    auto y_ptr = trx->m_op_ptr--;
    auto x_ptr = trx->m_op_ptr;
    auto result = (deep_equal(trx, *x_ptr, *y_ptr, 0) != negate);
    x_ptr->maybe_free_extvalue(trx);
    y_ptr->maybe_free_extvalue(trx);
    *x_ptr = Object::make_boolean(result);
}

// deep-eq: any any :- bool
// Structural equality -- like eq but compares array/packed elements recursively.
// throws: opstack-underflow, limit-check
static void deepeq_op(Trix *trx) {
    compare_deep_eq_impl(trx, false);
}

// deep-ne: any any :- bool
// Structural inequality -- negation of deep-eq.
// throws: opstack-underflow, limit-check
static void deepne_op(Trix *trx) {
    compare_deep_eq_impl(trx, true);
}

// Find the narrowest numeric type that can losslessly represent both a and b.
// Promotion ladder: Byte < Integer < Long, Byte < UInteger < ULong,
// Integer + UInteger -> Long, any integer + Real -> Real (or Double if 64-bit),
// anything + Double -> Double.
[[nodiscard]] static Object::Type promote_common_type(Object::Type a, Object::Type b) {
    using T = Object::Type;

    if ((a == T::Double) || (b == T::Double)) {
        // Rule 1: Double dominates
        return T::Double;
    } else if ((a == T::UInt128) || (b == T::UInt128)) {
        // Rule 2a: UInt128 + Real -> Double (Real can't represent 128-bit)
        if ((a == T::Real) || (b == T::Real)) {
            return T::Double;
        } else {
            // Rule 2b: UInt128 + Integer -> Int128 (pick type representing both;
            //         mirrors UInteger+Integer -> Long).
            if ((a == T::Integer) || (b == T::Integer)) {
                return T::Int128;
            } else {
                // Rule 2c: UInt128 dominates Int128 and all smaller integers
                //         (mirrors ULong+Long -> ULong sign-loss).
                return T::UInt128;
            }
        }
    } else if ((a == T::Int128) || (b == T::Int128)) {
        // Rule 3a: Int128 + Real -> Double (Real can't represent 128-bit)
        if ((a == T::Real) || (b == T::Real)) {
            return T::Double;
        } else {
            // Rule 3b: Int128 dominates all 64-bit-and-smaller types
            return T::Int128;
        }
    } else if ((a == T::Real) || (b == T::Real)) {
        // Rule 4: Real + 64-bit -> Double; Real + smaller -> Real
        auto other = (a == T::Real) ? b : a;
        return (((other == T::Long) || (other == T::ULong)) ? T::Double : T::Real);
    } else if ((a == T::ULong) || (b == T::ULong)) {
        // Rule 5: ULong dominates integers
        return T::ULong;
    } else if ((a == T::Long) || (b == T::Long)) {
        // Rule 6: Long dominates 32-bit
        return T::Long;
    } else if (((a == T::Integer) && (b == T::UInteger)) || ((a == T::UInteger) && (b == T::Integer))) {
        // Rule 7: Integer + UInteger -> Long (safe widening)
        return T::Long;
    } else if ((a == T::UInteger) || (b == T::UInteger)) {
        // Rule 8: UInteger + Byte -> UInteger
        return T::UInteger;
    } else {
        // Rule 9: Integer + Byte -> Integer
        return T::Integer;
    }
}

// Convert a numeric Object to the target type, freeing any ExtValue.
static void promote_convert(Trix *trx, Object *val_ptr, Object::Type target) {
    using T = Object::Type;

    Object result_obj;
    switch (+target) {
    case +T::Byte: {
        auto v = cast_to_type<vm_t>(trx, val_ptr);
        result_obj = Object::make_byte(v);
        break;
    }

    case +T::Integer: {
        auto v = cast_to_type<integer_t>(trx, val_ptr);
        result_obj = Object::make_integer(v);
        break;
    }

    case +T::UInteger: {
        auto v = cast_to_type<uinteger_t>(trx, val_ptr);
        result_obj = Object::make_uinteger(v);
        break;
    }

    case +T::Long: {
        auto v = cast_to_type<long_t>(trx, val_ptr);
        result_obj = Object::make_long(trx, v);
        break;
    }

    case +T::ULong: {
        auto v = cast_to_type<ulong_t>(trx, val_ptr);
        result_obj = Object::make_ulong(trx, v);
        break;
    }

    case +T::Int128: {
        auto v = cast_to_type<int128_t>(trx, val_ptr);
        result_obj = Object::make_int128(trx, v);
        break;
    }

    case +T::UInt128: {
        auto v = cast_to_type<uint128_t>(trx, val_ptr);
        result_obj = Object::make_uint128(trx, v);
        break;
    }

    case +T::Real: {
        auto v = cast_to_type<real_t>(trx, val_ptr);
        result_obj = Object::make_real(v);
        break;
    }

    case +T::Double: {
        auto v = cast_to_type<double_t>(trx, val_ptr);
        result_obj = Object::make_double(trx, v);
        break;
    }

    default:
        assert(false && "promote_convert: logic error");
        std::unreachable();
    }
    val_ptr->maybe_free_extvalue(trx);
    *val_ptr = result_obj;
}

// make-readonly: any :- any
// Sets the access mode to read-only.
// throws: opstack-underflow, type-check
static void make_readonly_op(Trix *trx) {
    trx->verify_operands(VerifyHasAccess);

    auto val_ptr = trx->m_op_ptr;
    if (val_ptr->has_object_access()) {
        val_ptr->set_readonly_access();
    } else {
        auto dict = val_ptr->is_set() ? val_ptr->set_value(trx) : val_ptr->dict_value(trx);
        dict->set_readonly_access(trx);
    }
}

// is-*: type predicate helper
// Replaces top operand with boolean indicating whether it matches the given type test.
template<bool (Object::*Pred)() const>
static void is_type_pred_op(Trix *trx) {
    trx->require_op_count(1);

    auto result = (trx->m_op_ptr->*Pred)();
    trx->m_op_ptr->maybe_free_extvalue(trx);
    *trx->m_op_ptr = Object::make_boolean(result);
}

// is-number: any :- bool  (true for all 6 numeric types)
// throws: opstack-underflow
static void is_number_op(Trix *trx) {
    trx->require_op_count(1);

    auto result = trx->m_op_ptr->is_number();
    trx->m_op_ptr->maybe_free_extvalue(trx);
    *trx->m_op_ptr = Object::make_boolean(result);
}

// is-signed, is-unsigned, is-float: category type predicates using existing Object methods
// is-signed:   any :- bool  (true for Integer, Long)
// is-unsigned: any :- bool  (true for Byte, UInteger, ULong)
// is-float:    any :- bool  (true for Real, Double)

// merge: dict1 dict2 :- dict3
// Creates a new dict containing all entries from dict1 and dict2.
// dict2 values win on key conflicts.
// throws: opstack-underflow, type-check, vm-full
static void merge_op(Trix *trx) {
    trx->verify_operands(VerifyDict, VerifyDict);

    auto dict2_ptr = trx->m_op_ptr;
    auto dict1_ptr = (dict2_ptr - 1);
    auto dict1 = dict1_ptr->dict_value(trx);
    auto dict2 = dict2_ptr->dict_value(trx);
    auto capacity = static_cast<length_t>(dict1->length() + dict2->length());
    // Region-aware result: under ${...} the merged dict lands in global VM so it
    // survives save/restore like the `dict` constructor (a LOCAL dict produced
    // inside a GLOBAL block is a region inconsistency that R6-rejects when stored
    // into a global container).  copy_dict clones each entry -- firing GC under
    // memory pressure -- before the result is on any GC-scanned stack, so root it
    // across both copies; the two source dicts stay alive on the op stack.
    auto [result, offset] = create_result_dict(trx, capacity, Object::DictMode::ReadWriteDynamic);
    trx->gc_root_push_oneoff(Object::make_dict(offset));
    result->copy_dict(trx, dict1);
    result->copy_dict(trx, dict2);
    trx->gc_root_pop_oneoff();

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_dict(offset);
}

// writable?: any :- bool
// Tests whether the object has read-write access.
// throws: opstack-underflow
static void is_writable_op(Trix *trx) {
    trx->require_op_count(1);

    auto result = true;
    auto top_ptr = trx->m_op_ptr;
    if (top_ptr->is_stream()) {
        // The cached write-access bit is stamped once at stream creation and
        // never re-validated against m_sid, so a closed/recycled handle would
        // keep reporting its stale bit (writable?=true) while readable? -- which
        // queries live state -- already reports false.  Gate the cached bit on
        // is_open(sid) so a stale handle reports false in lockstep with
        // readable?, while a live stream keeps its object-access semantics
        // (mode 'r'/'R' grant ReadOnly object access -> writable? false).
        auto [stream, sid] = top_ptr->stream_value(trx);
        result = stream->is_open(sid) && top_ptr->has_write_access();
    } else if (top_ptr->has_access()) {
        if (top_ptr->has_object_access()) {
            result = top_ptr->has_write_access();
        } else {
            auto dict = top_ptr->is_set() ? top_ptr->set_value(trx) : top_ptr->dict_value(trx);
            result = dict->has_write_access();
        }
    }
    top_ptr->maybe_free_extvalue(trx);
    *top_ptr = Object::make_boolean(result);
}

// readable?: any :- bool
// Tests whether the object is readable.
// throws: opstack-underflow
static void is_readable_op(Trix *trx) {
    trx->require_op_count(1);

    auto top_ptr = trx->m_op_ptr;
    auto result = true;
    if (top_ptr->is_stream()) {
        auto [stream, sid] = top_ptr->stream_value(trx);
        result = stream->is_readable(sid);
    }
    top_ptr->maybe_free_extvalue(trx);
    *top_ptr = Object::make_boolean(result);
}

// executable?: any :- bool
// Tests whether the object is executable.
// throws: opstack-underflow
static void is_executable_op(Trix *trx) {
    trx->require_op_count(1);

    auto top_ptr = trx->m_op_ptr;
    auto result = top_ptr->is_executable();
    top_ptr->maybe_free_extvalue(trx);
    *top_ptr = Object::make_boolean(result);
}

// assert: str bool :- --
// Raises assert-failed if bool is false.
template<typename T>
static T cast_to_type(Trix *trx, const Object *val_ptr) {
    switch (+val_ptr->type()) {
    case +Object::Type::Byte:
        if constexpr (std::is_same_v<T, boolean_t>) {
            return (val_ptr->byte_value() != 0);
        } else {
            return static_cast<T>(val_ptr->byte_value());
        }

    case +Object::Type::Integer:
        if constexpr (std::is_same_v<T, boolean_t>) {
            return (val_ptr->integer_value() != 0);
        } else {
            return static_cast<T>(val_ptr->integer_value());
        }

    case +Object::Type::UInteger:
        if constexpr (std::is_same_v<T, boolean_t>) {
            return (val_ptr->uinteger_value() != 0);
        } else {
            return static_cast<T>(val_ptr->uinteger_value());
        }

    case +Object::Type::Long:
        if constexpr (std::is_same_v<T, boolean_t>) {
            return (val_ptr->long_value(trx) != 0);
        } else {
            return static_cast<T>(val_ptr->long_value(trx));
        }

    case +Object::Type::ULong:
        if constexpr (std::is_same_v<T, boolean_t>) {
            return (val_ptr->ulong_value(trx) != 0);
        } else {
            return static_cast<T>(val_ptr->ulong_value(trx));
        }

    case +Object::Type::Int128:
        if constexpr (std::is_same_v<T, boolean_t>) {
            return (val_ptr->int128_value(trx) != 0);
        } else {
            return static_cast<T>(val_ptr->int128_value(trx));
        }

    case +Object::Type::UInt128:
        if constexpr (std::is_same_v<T, boolean_t>) {
            return (val_ptr->uint128_value(trx) != 0);
        } else {
            return static_cast<T>(val_ptr->uint128_value(trx));
        }

    case +Object::Type::Real:
        if constexpr (std::is_same_v<T, boolean_t>) {
            return (std::fpclassify(val_ptr->real_value()) != FP_ZERO);
        } else {
            return static_cast<T>(val_ptr->real_value());
        }

    case +Object::Type::Double:
        if constexpr (std::is_same_v<T, boolean_t>) {
            return (std::fpclassify(val_ptr->double_value(trx)) != FP_ZERO);
        } else {
            return static_cast<T>(val_ptr->double_value(trx));
        }

    case +Object::Type::Boolean:
        if constexpr (std::is_same_v<T, boolean_t>) {
            return val_ptr->boolean_value();
        } else {
            return static_cast<T>(val_ptr->boolean_value());
        }

    default:
        assert(false && "cast_to_type: logic error");
        std::unreachable();
    }
}

// cast: num name :- num
// Converts a numeric or boolean value to the named type.
// throws: vm-full, opstack-underflow, type-check
static void cast_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyNumbers | VerifyBoolean);

    auto name_ptr = trx->m_op_ptr;
    auto val_ptr = (name_ptr - 1);

    auto [is_name, type] = trx->is_type_name(name_ptr);
    if (is_name) {
        if (type != val_ptr->type()) {
            Object result_obj;

            switch (+type) {
            case +Object::Type::Byte: {
                auto b = cast_to_type<vm_t>(trx, val_ptr);
                result_obj = Object::make_byte(b);
                break;
            }

            case +Object::Type::Integer: {
                auto i = cast_to_type<integer_t>(trx, val_ptr);
                result_obj = Object::make_integer(i);
                break;
            }

            case +Object::Type::UInteger: {
                auto ui = cast_to_type<uinteger_t>(trx, val_ptr);
                result_obj = Object::make_uinteger(ui);
                break;
            }

            case +Object::Type::Long: {
                auto l = cast_to_type<long_t>(trx, val_ptr);
                result_obj = Object::make_long(trx, l);
                break;
            }

            case +Object::Type::ULong: {
                auto ul = cast_to_type<ulong_t>(trx, val_ptr);
                result_obj = Object::make_ulong(trx, ul);
                break;
            }

            case +Object::Type::Int128: {
                auto h = cast_to_type<int128_t>(trx, val_ptr);
                result_obj = Object::make_int128(trx, h);
                break;
            }

            case +Object::Type::UInt128: {
                auto uh = cast_to_type<uint128_t>(trx, val_ptr);
                result_obj = Object::make_uint128(trx, uh);
                break;
            }

            case +Object::Type::Real: {
                auto r = cast_to_type<real_t>(trx, val_ptr);
                result_obj = Object::make_real(r);
                break;
            }

            case +Object::Type::Double: {
                auto d = cast_to_type<double_t>(trx, val_ptr);
                result_obj = Object::make_double(trx, d);
                break;
            }

            case +Object::Type::Boolean: {
                auto b = cast_to_type<boolean_t>(trx, val_ptr);
                result_obj = Object::make_boolean(b);
                break;
            }

            default:
                trx->error(Error::TypeCheck, "cast: cannot cast to non-numeric type");
            }

            val_ptr->maybe_free_extvalue(trx);
            *val_ptr = result_obj;
        }
        trx->m_op_ptr = val_ptr;
    } else {
        trx->error(Error::TypeCheck, "cast: not a type name");
    }
}

// Returns true if coercion from src to target is a valid widening conversion.
// Same-type is always valid.  Boolean conversions are allowed in both directions.
// Numeric conversions follow the promotion hierarchy (widening only).
[[nodiscard]] static bool is_valid_coercion(Object::Type src, Object::Type target) {
    using T = Object::Type;
    if (src == target) {
        return true;
    } else if ((src == T::Boolean) || (target == T::Boolean)) {
        return true;
    } else {
        // Widening paths from the promotion hierarchy
        switch (+src) {
        case +T::Byte:
            return ((target == T::Integer) || (target == T::UInteger) || (target == T::Long) || (target == T::ULong) ||
                    (target == T::Int128) || (target == T::UInt128) || (target == T::Real) || (target == T::Double));
        case +T::Integer:
            return ((target == T::Long) || (target == T::Int128) || (target == T::Real) || (target == T::Double));

        case +T::UInteger:
            return ((target == T::Long) || (target == T::ULong) || (target == T::Int128) || (target == T::UInt128) ||
                    (target == T::Real) || (target == T::Double));
        case +T::Long:
            return ((target == T::Int128) || (target == T::Double));

        case +T::ULong:
            return ((target == T::Int128) || (target == T::UInt128) || (target == T::Double));

        case +T::Int128:
            return (target == T::Double);

        case +T::UInt128:
            return (target == T::Double);

        case +T::Real:
            return (target == T::Double);

        default:
            return false;
        }
    }
}

// Helper: convert a single Object in-place to the target type using cast_to_type<T>.
// Caller has already validated is_valid_coercion().
static void coerce_element(Trix *trx, Object *val_ptr, Object::Type target) {
    using T = Object::Type;
    Object result_obj;
    switch (+target) {
    case +T::Byte:
        result_obj = Object::make_byte(cast_to_type<vm_t>(trx, val_ptr));
        break;

    case +T::Integer:
        result_obj = Object::make_integer(cast_to_type<integer_t>(trx, val_ptr));
        break;

    case +T::UInteger:
        result_obj = Object::make_uinteger(cast_to_type<uinteger_t>(trx, val_ptr));
        break;

    case +T::Long:
        result_obj = Object::make_long(trx, cast_to_type<long_t>(trx, val_ptr));
        break;

    case +T::ULong:
        result_obj = Object::make_ulong(trx, cast_to_type<ulong_t>(trx, val_ptr));
        break;

    case +T::Int128:
        result_obj = Object::make_int128(trx, cast_to_type<int128_t>(trx, val_ptr));
        break;

    case +T::UInt128:
        result_obj = Object::make_uint128(trx, cast_to_type<uint128_t>(trx, val_ptr));
        break;

    case +T::Real:
        result_obj = Object::make_real(cast_to_type<real_t>(trx, val_ptr));
        break;

    case +T::Double:
        result_obj = Object::make_double(trx, cast_to_type<double_t>(trx, val_ptr));
        break;

    case +T::Boolean:
        result_obj = Object::make_boolean(cast_to_type<boolean_t>(trx, val_ptr));
        break;

    default:
        assert(false && "coerce_element: unsupported target type");
        std::unreachable();
    }
    val_ptr->maybe_free_extvalue(trx);
    *val_ptr = result_obj;
}

// coerce: container name :- container'
// Converts all elements of an array/packed or all values of a dict to the named type.
// Only widening conversions are allowed; narrowing raises TypeCheck.
// throws: vm-full, opstack-underflow, type-check
static void coerce_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyAny);

    auto name_ptr = trx->m_op_ptr;
    auto container_ptr = (name_ptr - 1);

    auto [is_name, target] = trx->is_type_name(name_ptr);
    if (!is_name) {
        trx->error(Error::TypeCheck, "coerce: not a type name");
    } else {
        // Validate target is numeric or boolean
        auto target_ok = (target == Object::Type::Byte) || (target == Object::Type::Integer) ||
                         (target == Object::Type::UInteger) || (target == Object::Type::Long) || (target == Object::Type::ULong) ||
                         (target == Object::Type::Int128) || (target == Object::Type::UInt128) || (target == Object::Type::Real) ||
                         (target == Object::Type::Double) || (target == Object::Type::Boolean);
        if (!target_ok) {
            trx->error(Error::TypeCheck, "coerce: target must be a numeric or boolean type");
        } else if (container_ptr->is_sequence()) {
            // Array/packed path
            auto count = container_ptr->arrays_length();
            if (count == 0) {
                auto [_, offset] = trx->vm_alloc_dispatch_n<Object>(0, ChunkKind::Array);
                container_ptr->maybe_free_extvalue(trx);
                *container_ptr = Object::make_array(offset, 0);
                trx->m_op_ptr = container_ptr;
                return;
            } else {
                // Expand packed to temp buffer if needed
                Object *src_ptr = nullptr;
                Object *src_buf = nullptr;
                if (container_ptr->is_packed()) {
                    auto [buf, _] = trx->vm_alloc_n<Object>(count);
                    // Expand LOCALLY: clone_array_elements would otherwise materialize global
                    // ExtValues under ${...} into this unrooted local buffer, which a later GC
                    // (from the global result alloc/fill below) would sweep.  Local scratch
                    // clones are safe and freed below; mirrors expand_packed_to_temp.
                    auto saved_global = trx->m_curr_alloc_global;
                    trx->m_curr_alloc_global = false;
                    clone_array_elements(trx, container_ptr, buf, count);
                    trx->m_curr_alloc_global = saved_global;
                    src_ptr = buf;
                    src_buf = buf;
                } else {
                    src_ptr = container_ptr->array_objects(trx);
                }

                // Allocate result and convert each element.  Null + root across the clones:
                // under ${...} the result is a global block -- null first (gvm_for_each walks it
                // on each clone's GC) and root it (not yet on any stack).  error() resets the
                // gc-root stack, so the mid-loop type-check error paths below need no manual pop.
                auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(count, ChunkKind::Array);
                std::fill_n(dst_ptr, count, Object::make_null());
                auto result_obj = Object::make_array(dst_offset, count);
                trx->gc_root_push_oneoff(result_obj);
                for (length_t i = 0; i < count; ++i) {
                    if (!src_ptr[i].is_number() && !src_ptr[i].is_boolean()) {
                        // Free already-converted results and packed temp
                        for (length_t j = 0; j < i; ++j) {
                            dst_ptr[j].maybe_free_extvalue(trx);
                        }
                        if (src_buf != nullptr) {
                            for (length_t j = 0; j < count; ++j) {
                                src_buf[j].maybe_free_extvalue(trx);
                            }
                        }
                        trx->error(Error::TypeCheck, "coerce: element {} is not numeric or boolean", i);
                    } else if (src_ptr[i].type() == target) {
                        dst_ptr[i] = src_ptr[i].make_clone(trx);
                    } else {
                        if (!is_valid_coercion(src_ptr[i].type(), target)) {
                            for (length_t j = 0; j < i; ++j) {
                                dst_ptr[j].maybe_free_extvalue(trx);
                            }
                            if (src_buf != nullptr) {
                                for (length_t j = 0; j < count; ++j) {
                                    src_buf[j].maybe_free_extvalue(trx);
                                }
                            }
                            trx->error(Error::TypeCheck,
                                       "coerce: cannot narrow {} to {}",
                                       Object::type_sv(src_ptr[i].type()),
                                       Object::type_sv(target));
                        }
                        dst_ptr[i] = src_ptr[i].make_clone(trx);
                        coerce_element(trx, &dst_ptr[i], target);
                    }
                }
                trx->gc_root_pop_oneoff();

                // Free packed temp buffer
                if (src_buf != nullptr) {
                    for (length_t j = 0; j < count; ++j) {
                        src_buf[j].maybe_free_extvalue(trx);
                    }
                }

                container_ptr->maybe_free_extvalue(trx);
                *container_ptr = result_obj;
                trx->m_op_ptr = container_ptr;
            }

        } else if (container_ptr->is_dict()) {
            // Dict path: create a new dict, iterate the source, convert each value.
            // Region-aware so the coerced dict survives save/restore under ${...} (a
            // LOCAL dict produced in a GLOBAL block R6-rejects when stored globally).
            auto src_dict = container_ptr->dict_value(trx);
            auto [dst_dict, dst_offset] = create_result_dict(trx, src_dict->maxlength(), Object::DictMode::ReadWriteFixed);

            // Each key/value make_clone -- and coerce_element's widening alloc -- fires
            // GC under ${...}+pressure while the result is not yet on any stack and the
            // cloned key/value are bare C locals.  Root the result dict (one-off) plus
            // the in-flight key/value (slots 2/3) across the clone+coerce+put so none
            // is swept mid-fill.  A type-check error inside the lambda is cleaned up by
            // error()'s gc-root reset, so no manual pop is needed on the throw path.
            trx->gc_root_push_oneoff(Object::make_dict(dst_offset));
            trx->require_gc_root_capacity_more(2);
            src_dict->for_each(trx, [&](Object key, Object value) {
                if (!value.is_number() && !value.is_boolean()) {
                    trx->error(Error::TypeCheck, "coerce: dict value is not numeric or boolean");
                } else if ((value.type() != target) && !is_valid_coercion(value.type(), target)) {
                    trx->error(Error::TypeCheck,
                               "coerce: cannot narrow {} to {}",
                               Object::type_sv(value.type()),
                               Object::type_sv(target));
                }
                auto key_clone = key.make_clone(trx);
                *++trx->m_gc_roots_ptr = key_clone;
                auto val_clone = value.make_clone(trx);
                *++trx->m_gc_roots_ptr = val_clone;
                if (value.type() != target) {
                    coerce_element(trx, &val_clone, target);  // widening alloc may fire GC
                    *trx->m_gc_roots_ptr = val_clone;         // re-stamp: coerce reallocated the value
                }
                static_cast<void>(dst_dict->put(trx, key_clone, val_clone));
                trx->gc_root_pop_n(2);
            });
            trx->gc_root_pop_oneoff();

            container_ptr->maybe_free_extvalue(trx);
            *container_ptr = Object::make_dict(dst_offset);
            trx->m_op_ptr = container_ptr;

        } else {
            trx->error(Error::TypeCheck, "coerce: operand must be an array, packed array, or dict");
        }
    }
}

// reinterpret: num name :- num
// Reinterprets bits of a value as the named type.
// throws: opstack-underflow, type-check
static void reinterpret_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyNumbers | VerifyAddress | VerifyBoolean);

    auto name_ptr = trx->m_op_ptr;
    auto val_ptr = (name_ptr - 1);

    auto [is_name, new_type] = trx->is_type_name(name_ptr);
    if (is_name) {
        auto curr_type = val_ptr->type();
        if (new_type != curr_type) {
            switch (+new_type) {
            case +Object::Type::Byte:
            case +Object::Type::Boolean:
                if (val_ptr->is_byte() || val_ptr->is_boolean()) {
                    val_ptr->set_type(new_type);
                } else {
                    trx->error(Error::TypeCheck, "reinterpret: cannot reinterpret to 'byte'/'boolean' from 32/64-bit type");
                }
                break;

            case +Object::Type::Integer:
            case +Object::Type::UInteger:
            case +Object::Type::Real: {
                if (val_ptr->is_integer() || val_ptr->is_uinteger() || val_ptr->is_real()) {
                    val_ptr->set_type(new_type);
                } else {
                    trx->error(Error::TypeCheck, "reinterpret: cannot reinterpret to 32-bit type from incompatible size");
                }
                break;
            }

            case +Object::Type::Long:
            case +Object::Type::ULong:
            case +Object::Type::Address:
            case +Object::Type::Double: {
                if (val_ptr->is_long() || val_ptr->is_ulong() || val_ptr->is_address() || val_ptr->is_double()) {
                    val_ptr->set_type(new_type);
                } else {
                    trx->error(Error::TypeCheck, "reinterpret: cannot reinterpret to 64-bit type from incompatible size");
                }
                break;
            }

            case +Object::Type::Int128:
            case +Object::Type::UInt128: {
                // Int128 and UInt128 are both 16-byte WideValues with identical layout;
                // reinterpreting between them is just a tag flip.  Reinterpreting across
                // width boundaries (to/from 64-bit or smaller types) is not meaningful.
                if (val_ptr->is_int128() || val_ptr->is_uint128()) {
                    val_ptr->set_type(new_type);
                } else {
                    trx->error(Error::TypeCheck, "reinterpret: cannot reinterpret to 128-bit type from incompatible size");
                }
                break;
            }

            default:
                trx->error(Error::TypeCheck, "reinterpret: cannot reinterpret to non-numeric type");
            }
        }
        trx->m_op_ptr = val_ptr;
    } else {
        trx->error(Error::TypeCheck, "reinterpret: not a type name");
    }
}

// peek: addr name :- num
// Reads a typed value from the given address.
// throws: vm-full, opstack-underflow, range-check, type-check
[[nodiscard]] Object error_name(Error error) {
    auto sv = error_sv(error);
    return Object::make_name(m_errorname_offsets[+error], sv_length(sv));
}

[[nodiscard]] Object type_name(Object::Type type) {
    auto sv = Object::type_sv(type);
    return Object::make_name(m_typename_offsets[+type], sv_length(sv));
}

static constexpr int ObjectNameBufferSize = 24;

[[nodiscard]] length_t
object_name(const Object *val_ptr, char buffer[ObjectNameBufferSize], bool upper = false, bool dashes = true) {
    auto sv = Object::type_sv(val_ptr->type());
    auto suffix_sv = Object::type_suffix_sv();
    auto length = static_cast<length_t>(sv_length(sv) - sv_length(suffix_sv));
    auto ptr = buffer;
    if (dashes) {
        *ptr++ = '-';
        *ptr++ = '-';
    }

    std::copy_n(sv.data(), length, ptr);
    if (upper) {
        std::ranges::for_each(ptr, ptr + length, [](char &ch) {
            if ((ch >= 'a') && (ch <= 'z')) {
                ch = static_cast<char>(to_uppercase(ch));
            }
        });
    }
    ptr += length;

    if (dashes) {
        *ptr++ = '-';
        *ptr++ = '-';
        length = static_cast<length_t>(length + 4);
    }
    *ptr = '\0';
    return length;
}

[[nodiscard]] std::pair<bool, Object::Type> is_type_name(const Object *val_ptr) {
    if (val_ptr->is_name()) {
        for (int i = 0; i < Object::TypeCount; ++i) {
            if (m_typename_offsets[i] == val_ptr->m_name) {
                return std::pair{true, static_cast<Object::Type>(i)};
            }
        }
    }
    return std::pair{false, Object::Type::Null};
}

[[nodiscard]] std::pair<bool, Error> is_error_name(const Object *val_ptr) {
    if (val_ptr->is_name()) {
        for (int i = 0; i < ErrorCount; ++i) {
            // UserError is the internal slot for user-thrown Names, not a
            // distinct throwable builtin.  Report it as "not a builtin error" so
            // throw / throw-with / coroutine-rethrow take the pass-through branch
            // that copies the thrown Name into *m_last_error_name_ptr.  Otherwise
            // a literal `/user-error throw` raises UserError without writing the
            // name (error() leaves it intact for UserError), so last-error would
            // report a stale name instead of /user-error.
            if ((m_errorname_offsets[i] == val_ptr->m_name) && (static_cast<Error>(i) != Error::UserError)) {
                return std::pair{true, static_cast<Error>(i)};
            }
        }
    }
    return std::pair{false, Error::NoError};
}

// to-string: any rwstr :- str
// Formats any's textual representation into the provided RW string buffer.
// throws: opstack-underflow, type-check, range-check
static void to_string_op(Trix *trx) {
    trx->verify_operands(VerifyRWString, VerifyAny);

    auto str_ptr = trx->m_op_ptr;
    auto any_ptr = (str_ptr - 1);
    auto tmp_obj = *str_ptr;

    auto [output_count, dropped_count] = PrintFmt::process_object(trx, any_ptr, tmp_obj.string_vptr(trx), tmp_obj.string_length());
    if (dropped_count == 0) {
        any_ptr->maybe_free_extvalue(trx);

        tmp_obj.set_string_length(trx, static_cast<length_t>(output_count));
        *any_ptr = tmp_obj;
        trx->m_op_ptr = any_ptr;
    } else {
        trx->error(Error::RangeCheck, "to-string: buffer capacity {} insufficient for formatted output", tmp_obj.string_length());
    }
}
