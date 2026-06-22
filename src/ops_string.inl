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

//===--- String Operators ---===//
//
// Implements string creation, manipulation, searching, and encoding.  Based on:
//
//   PostScript string operations: strings are mutable byte arrays with
//   length.  Operations include search, anchorsearch (starts-with), get,
//   put, getinterval (sub-string), putinterval (put-interval), and
//   comparison.  Trix names in parentheses where they differ.
//
//   C string.h / Python str: case conversion, trim, pad, find, replace,
//   split, join, starts-with, ends-with, contains, reverse.
//
// --- Core concepts for maintainers ---
//
// STRING REPRESENTATION
//   A String Object holds a vm_offset_t to a byte array on the VM heap
//   and a length in the header.  Owned strings allocate length+1 bytes so
//   the backing buffer has a trailing nul (every make_string / transform
//   site writes data[length] = '\0').  Substring views from get-interval
//   alias into the parent buffer and may not be nul-terminated at the view
//   boundary, so C-API consumers must respect the length and copy to a
//   nul-terminated VM buffer when needed (see ops_regex.inl).  Strings can
//   be ReadOnly or ReadWrite; read-only strings reject put/putinterval.
//
// BYTE-ORIENTED
//   Trix strings are byte strings, not Unicode strings.  All operations
//   work on individual bytes (0-255).  There is no character encoding
//   awareness -- the string is an opaque byte sequence.  This matches
//   PostScript's model and is appropriate for an embedded scripting VM.
//
// NEW-STRING vs NON-MUTATING
//   Transform operations (uppercase, lowercase, reverse, trim, pad)
//   allocate a new string on the VM heap and replace the stack operand.
//   They do not modify the original string bytes in place.
//   Search/test operations (find, contains, starts-with) are non-mutating.
//   All operations that produce string output (join, replace, uppercase,
//   etc.) create new strings via vm_alloc_dispatch(..., ChunkKind::String),
//   which routes to global VM inside a `${...}` block (so the result
//   survives save/restore) and to local VM otherwise.  String LITERALS are
//   the exception: their bytes are baked at scan time and stay local unless
//   the `(...)#$` force-global suffix is used.
//
// HELPER PATTERN
//   Several operations share a common pattern: transform_string_bytes()
//   applies a per-byte function to every byte of a string.  Similarly,
//   trim_impl(left, right) backs the trim variants (trim / trim-left /
//   trim-right), and pad_string_impl(pad_left, op_name) backs pad-left and
//   pad-right (left/right only).
//

// Transform each byte of a string using a per-byte function.
static void transform_string_bytes(Trix *trx, vm_t (*transform)(vm_t)) {
    trx->verify_operands(VerifyString);

    auto src_ptr = trx->m_op_ptr;
    auto src_data = src_ptr->string_vptr(trx);
    auto src_length = src_ptr->string_length();

    auto [dst_data, dst_offset] = trx->vm_alloc_dispatch<vm_t>(src_length + 1, ChunkKind::String);
    for (length_t i = 0; i < src_length; ++i) {
        dst_data[i] = transform(src_data[i]);
    }
    dst_data[src_length] = '\0';
    *src_ptr = Object::make_string(dst_offset, src_length);
}

// uppercase: str :- str
// Returns a new string with lowercase ASCII letters uppercased.
// throws: vm-full, opstack-underflow, type-check
static void uppercase_op(Trix *trx) {
    transform_string_bytes(trx, to_uppercase);
}

// lowercase: str :- str
// Returns a new string with uppercase ASCII letters lowercased.
// throws: vm-full, opstack-underflow, type-check
static void lowercase_op(Trix *trx) {
    transform_string_bytes(trx, to_lowercase);
}

// capitalize: str -- str
// Returns a new string with the first byte uppercased, rest unchanged.
// Empty string returns empty string.
// throws: vm-full, opstack-underflow, type-check
static void capitalize_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto src_ptr = trx->m_op_ptr;
    auto src_data = src_ptr->string_vptr(trx);
    auto src_length = src_ptr->string_length();

    auto [dst_data, dst_offset] = trx->vm_alloc_dispatch<vm_t>(src_length + 1, ChunkKind::String);
    if (src_length > 0) {
        dst_data[0] = to_uppercase(src_data[0]);
        for (length_t i = 1; i < src_length; ++i) {
            dst_data[i] = src_data[i];
        }
    }
    dst_data[src_length] = '\0';
    *src_ptr = Object::make_string(dst_offset, src_length);
}

// count-substring: str sub -- int
// Counts non-overlapping occurrences of sub in str.
// Empty sub returns 0.
// throws: opstack-underflow, type-check
static void count_substring_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto sub_ptr = trx->m_op_ptr;
    auto str_ptr = (sub_ptr - 1);
    auto str_sv = str_ptr->sv_value(trx);
    auto sub_sv = sub_ptr->sv_value(trx);

    integer_t count = 0;
    auto sub_len = sub_sv.size();
    if ((sub_len > 0) && (sub_len <= str_sv.size())) {
        auto remaining = str_sv;
        while (true) {
            auto pos = remaining.find(sub_sv);
            if (pos == std::string_view::npos) {
                break;
            } else {
                ++count;
                remaining.remove_prefix(pos + sub_len);
            }
        }
    }

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_integer(count);
}

// concat: str str :- str
// Concatenates two strings.
// throws: vm-full, opstack-underflow, type-check
static void concat_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto s2_ptr = trx->m_op_ptr;
    auto s1_ptr = (s2_ptr - 1);
    auto s1_data = s1_ptr->string_vptr(trx);
    auto s1_len = s1_ptr->string_length();
    auto s2_data = s2_ptr->string_vptr(trx);
    auto s2_len = s2_ptr->string_length();
    auto total32 = static_cast<vm_size_t>(s1_len) + s2_len;
    if (total32 > MaxStringLength) {
        trx->error(Error::LimitCheck, "concat: result length {} exceeds maximum {}", total32, MaxStringLength);
    } else {
        auto total = static_cast<length_t>(total32);

        // optimization: if s1 (including its nul terminator) ends at top of heap, extend
        // in place by appending s2.  This turns O(N^2) concat loops into O(N).
        // The old nul at s1[s1_len] is overwritten by s2 data; a new nul is written at the end.
        // Disabled when m_curr_alloc_global is set: the optimization extends in
        // LOCAL VM (m_vm_ptr is the local top), so the result would be local
        // even though the caller asked for global; fall through to the slow
        // path so vm_alloc_dispatch honors the flag.
        auto s1_end = s1_data + s1_len;
        if (!trx->m_curr_alloc_global && ((s1_end + 1) == trx->m_vm_ptr)) {
            static_cast<void>(trx->vm_alloc_ptr<vm_t>(s2_len));
            std::copy_n(s2_data, s2_len, s1_end);
            s1_end[s2_len] = '\0';
            --trx->m_op_ptr;
            *trx->m_op_ptr = Object::make_string(s1_ptr->offset(), total);
        } else {
            auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch<vm_t>(total + 1, ChunkKind::String);
            std::copy_n(s1_data, s1_len, dst_ptr);
            std::copy_n(s2_data, s2_len, dst_ptr + s1_len);
            dst_ptr[total] = '\0';
            --trx->m_op_ptr;
            *trx->m_op_ptr = Object::make_string(dst_offset, total);
        }
    }
}

// replace: str old new :- str
// Replaces all occurrences of old with new in str.
// throws: vm-full, opstack-underflow, type-check
static void replace_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString, VerifyString);

    auto new_ptr = trx->m_op_ptr;
    auto old_ptr = (new_ptr - 1);
    auto str_ptr = (new_ptr - 2);

    auto str_sv = str_ptr->sv_value(trx);
    auto old_sv = old_ptr->sv_value(trx);
    auto new_sv = new_ptr->sv_value(trx);

    if (old_sv.empty()) {
        // empty search string: return original unchanged
        trx->m_op_ptr -= 2;
    } else {
        // pass 1: count occurrences
        length_t count = 0;
        {
            auto haystack = str_sv;
            while (true) {
                auto it = std::search(haystack.begin(), haystack.end(), old_sv.begin(), old_sv.end());
                if (it == haystack.end()) {
                    break;
                } else {
                    ++count;
                    haystack = haystack.substr(static_cast<size_t>(it - haystack.begin()) + old_sv.size());
                }
            }
        }

        if (count == 0) {
            // not found: return copy of original
            auto len = sv_length(str_sv);
            auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch<vm_t>(len + 1, ChunkKind::String);
            std::copy_n(reinterpret_cast<const vm_t *>(str_sv.data()), len, dst_ptr);
            dst_ptr[len] = '\0';
            trx->m_op_ptr -= 2;
            *trx->m_op_ptr = Object::make_string(dst_offset, len);
        } else {
            // pass 2: compute result length and build
            auto old_len = sv_length(old_sv);
            auto new_len = sv_length(new_sv);
            auto str_len = sv_length(str_sv);
            auto result_len32 = static_cast<vm_size_t>(str_len) - (static_cast<vm_size_t>(count) * old_len) +
                                (static_cast<vm_size_t>(count) * new_len);
            if (result_len32 > MaxStringLength) {
                trx->error(Error::LimitCheck, "replace: result length {} exceeds maximum {}", result_len32, MaxStringLength);
            } else {
                auto result_len = static_cast<length_t>(result_len32);
                auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch<vm_t>(result_len + 1, ChunkKind::String);
                auto write = dst_ptr;
                auto haystack = str_sv;
                while (true) {
                    auto it = std::search(haystack.begin(), haystack.end(), old_sv.begin(), old_sv.end());
                    // copy prefix before match
                    auto prefix_len = static_cast<length_t>(it - haystack.begin());
                    std::copy_n(reinterpret_cast<const vm_t *>(haystack.data()), prefix_len, write);
                    write += prefix_len;
                    if (it == haystack.end()) {
                        break;
                    } else {
                        // copy replacement
                        std::copy_n(reinterpret_cast<const vm_t *>(new_sv.data()), new_len, write);
                        write += new_len;
                        haystack = haystack.substr(static_cast<size_t>(prefix_len) + old_sv.size());
                    }
                }
                dst_ptr[result_len] = '\0';

                trx->m_op_ptr -= 2;
                *trx->m_op_ptr = Object::make_string(dst_offset, result_len);
            }
        }
    }
}

// join: arr str :- str
// Joins array of strings with separator.
// throws: vm-full, opstack-underflow, type-check
static void join_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyArrays);

    auto sep_ptr = trx->m_op_ptr;
    auto arr_ptr = (sep_ptr - 1);
    auto sep_data = sep_ptr->string_vptr(trx);
    auto sep_len = sep_ptr->string_length();
    auto arr_len = arr_ptr->arrays_length();

    if (arr_len == 0) {
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch<vm_t>(1, ChunkKind::String);
        dst_ptr[0] = '\0';
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = Object::make_string(dst_offset, 0);
    } else {
        vm_offset_t result_offset{};
        length_t result_len{};

        if (arr_ptr->is_array()) {
            // array path: direct element access
            auto elem_data = arr_ptr->array_objects(trx);

            auto total = uint64_t{0};
            for (length_t i = 0; i < arr_len; ++i) {
                if (!elem_data[i].is_string()) {
                    trx->error(Error::TypeCheck, "join: element {} is not a string", i);
                } else {
                    total += elem_data[i].string_length();
                }
            }
            total += static_cast<vm_size_t>(sep_len) * (arr_len - 1);

            if (total > MaxStringLength) {
                trx->error(Error::LimitCheck, "join: result length {} exceeds maximum {}", total, MaxStringLength);
            } else {
                result_len = static_cast<length_t>(total);
                auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch<vm_t>(result_len + 1, ChunkKind::String);
                result_offset = dst_offset;
                vm_t *write = dst_ptr;
                for (length_t i = 0; i < arr_len; ++i) {
                    if (i > 0) {
                        std::copy_n(sep_data, sep_len, write);
                        write += sep_len;
                    }
                    std::copy_n(elem_data[i].string_vptr(trx), elem_data[i].string_length(), write);
                    write += elem_data[i].string_length();
                }
                dst_ptr[result_len] = '\0';
            }
        } else {
            // packed path: two-pass iteration, no extraction allocation
            auto packed_span = arr_ptr->const_packed_span(trx);

            // pass 1: compute total length and type-check
            auto total = uint64_t{0};
            {
                auto data_ptr = packed_span.data();
                for (length_t i = 0; i < arr_len; ++i) {
                    auto [next, element] = Object::extract_next_packed(trx, data_ptr);
                    if (!element.is_string()) {
                        trx->error(Error::TypeCheck, "join: element {} is not a string", i);
                    } else {
                        total += element.string_length();
                        data_ptr = next;
                    }
                }
            }
            total += static_cast<vm_size_t>(sep_len) * (arr_len - 1);

            if (total > MaxStringLength) {
                trx->error(Error::LimitCheck, "join: result length {} exceeds maximum {}", total, MaxStringLength);
            } else {
                // pass 2: copy string bytes into result
                result_len = static_cast<length_t>(total);
                auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch<vm_t>(result_len + 1, ChunkKind::String);
                result_offset = dst_offset;
                vm_t *write = dst_ptr;
                {
                    auto data_ptr = packed_span.data();
                    for (length_t i = 0; i < arr_len; ++i) {
                        if (i > 0) {
                            std::copy_n(sep_data, sep_len, write);
                            write += sep_len;
                        }
                        auto [next, element] = Object::extract_next_packed(trx, data_ptr);
                        auto elem_len = element.string_length();
                        std::copy_n(element.string_vptr(trx), elem_len, write);
                        write += elem_len;
                        data_ptr = next;
                    }
                }
                dst_ptr[result_len] = '\0';
            }
        }

        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = Object::make_string(result_offset, result_len);
    }
}

// Common implementation for trim, trim-left, trim-right.
static void trim_impl(Trix *trx, bool left, bool right) {
    trx->verify_operands(VerifyString);

    auto src_ptr = trx->m_op_ptr;
    auto str_data = src_ptr->string_vptr(trx);
    auto len = src_ptr->string_length();
    length_t start = 0;
    if (left) {
        while ((start < len) && Trix::is_whitespace(str_data[start])) {
            ++start;
        }
    }
    length_t end = len;
    if (right) {
        while ((end > start) && Trix::is_whitespace(str_data[end - 1])) {
            --end;
        }
    }
    auto result = *src_ptr;
    result.string_interval(trx, start, static_cast<length_t>(end - start));
    *src_ptr = result;
}

// trim: str :- str
// Strips whitespace from both ends.
// throws: opstack-underflow, type-check
static void trim_op(Trix *trx) {
    trim_impl(trx, true, true);
}

// trim-left: str :- str
// Strips whitespace from the left.
// throws: opstack-underflow, type-check
static void trimleft_op(Trix *trx) {
    trim_impl(trx, true, false);
}

// trim-right: str :- str
// Strips whitespace from the right.
// throws: opstack-underflow, type-check
static void trimright_op(Trix *trx) {
    trim_impl(trx, false, true);
}

// reverse: arr :- arr | str :- str
// Returns a new array or string with elements reversed.
// throws: vm-full, opstack-underflow, type-check
static void reverse_op(Trix *trx) {
    trx->verify_operands(VerifyArrays | VerifyString);

    auto src_ptr = trx->m_op_ptr;
    if (src_ptr->is_string()) {
        auto src_data = src_ptr->string_vptr(trx);
        auto src_length = src_ptr->string_length();

        auto [dst_data, dst_offset] = trx->vm_alloc_dispatch<vm_t>(src_length + 1, ChunkKind::String);
        for (length_t i = 0; i < src_length; ++i) {
            dst_data[i] = src_data[src_length - 1 - i];
        }
        dst_data[src_length] = '\0';
        *src_ptr = Object::make_string(dst_offset, src_length);
    } else {
        auto src_length = src_ptr->arrays_length();

        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(src_length, ChunkKind::Array);
        // Null + root the result across the element clones: under ${...} each clone
        // (or packed decode) is a global alloc that can fire vm_global_gc, and the
        // result array is not yet on any stack, so its already-written global clones
        // would be swept.  The std::reverse below is alloc-free.
        std::fill_n(dst_ptr, src_length, Object::make_null());
        auto result_obj = Object::make_array(dst_offset, src_length);
        trx->gc_root_push_oneoff(result_obj);
        clone_array_elements(trx, src_ptr, dst_ptr, src_length);
        std::reverse(dst_ptr, dst_ptr + src_length);
        trx->gc_root_pop_oneoff();
        *src_ptr = result_obj;
    }
}

// Recursive helper: count leaf (non-array/non-packed) elements.  Accumulates in
// vm_size_t -- a length_t would wrap at 65536 leaves and undersize the result array
// that flatten_collect then overruns.  flatten_op bound-checks the total before alloc.
static vm_size_t flatten_count(Trix *trx, Object container, int depth) {
    if (depth > MaxRecursionDepth) {
        trx->error(Error::LimitCheck, "flatten: recursion depth exceeds {}", MaxRecursionDepth);
    } else {
        auto total = vm_size_t{0};
        auto type = container.type();

        if (type == Object::Type::Array) {
            auto [ptr, count] = container.array_value(trx);
            for (length_t i = 0; i < count; ++i) {
                auto elem_type = ptr[i].type();
                if ((elem_type == Object::Type::Array) || (elem_type == Object::Type::Packed)) {
                    total += flatten_count(trx, ptr[i], depth + 1);
                } else {
                    ++total;
                }
            }
        } else if (type == Object::Type::Packed) {
            auto packed_data = trx->offset_to_ptr<const packed_data_t>(container.m_packed);
            auto length = container.arrays_length();
            for (length_t i = 0; i < length; ++i) {
                auto [next, elem] = Object::extract_next_packed(trx, packed_data);
                auto elem_type = elem.type();
                if ((elem_type == Object::Type::Array) || (elem_type == Object::Type::Packed)) {
                    total += flatten_count(trx, elem, depth + 1);
                } else {
                    ++total;
                }
                packed_data = next;
            }
        }

        return total;
    }
}

// Recursive helper: copy leaf elements into destination array.  Depth is bounded
// by flatten_count, which runs first and enforces MaxRecursionDepth.
static length_t flatten_collect(Trix *trx, Object container, Object *dst, length_t write_idx) {
    auto type = container.type();

    if (type == Object::Type::Array) {
        auto [ptr, count] = container.array_value(trx);
        for (length_t i = 0; i < count; ++i) {
            auto elem_type = ptr[i].type();
            if ((elem_type == Object::Type::Array) || (elem_type == Object::Type::Packed)) {
                write_idx = flatten_collect(trx, ptr[i], dst, write_idx);
            } else {
                dst[write_idx++] = ptr[i].make_clone(trx);
            }
        }
    } else if (type == Object::Type::Packed) {
        auto packed_data = trx->offset_to_ptr<const packed_data_t>(container.m_packed);
        auto length = container.arrays_length();
        for (length_t i = 0; i < length; ++i) {
            auto [next, elem] = Object::extract_next_packed(trx, packed_data);
            auto elem_type = elem.type();
            if ((elem_type == Object::Type::Array) || (elem_type == Object::Type::Packed)) {
                write_idx = flatten_collect(trx, elem, dst, write_idx);
            } else {
                // extract_next_packed returns a borrowed reference -- clone for ownership
                dst[write_idx++] = elem.make_clone(trx);
            }
            packed_data = next;
        }
    }
    return write_idx;
}

// flatten: array :- array
// Recursively flattens nested arrays/packed into a single 1D array.
// throws: limit-check, opstack-underflow, type-check, vm-full
static void flatten_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);

    auto container = *trx->m_op_ptr;
    auto count = flatten_count(trx, container, 0);

    if (count > MaxArrayLength) {
        trx->error(Error::LimitCheck, "flatten: result length {} exceeds maximum {}", count, MaxArrayLength);
    } else if (count == 0) {
        auto [ptr, offset] = trx->vm_alloc_dispatch_n<Object>(0, ChunkKind::Array);
        *trx->m_op_ptr = Object::make_array(offset, 0);
    } else {
        auto len = static_cast<length_t>(count);
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(len, ChunkKind::Array);
        // Null + root the result across flatten_collect: under ${...} each leaf
        // clone is a global alloc that can fire vm_global_gc, and the result array
        // is not yet on any stack, so its already-written global clones would be
        // swept.  Rooting marks its slots; null-init keeps the unwritten tail safe.
        std::fill_n(dst_ptr, len, Object::make_null());
        auto result_obj = Object::make_array(dst_offset, len);
        trx->gc_root_push_oneoff(result_obj);
        flatten_collect(trx, container, dst_ptr, 0);
        trx->gc_root_pop_oneoff();
        *trx->m_op_ptr = result_obj;
    }
}

// search: str seek :- post match pre true | str false
// Searches for seek in str; splits on first match.
// throws: opstack-overflow, opstack-underflow, type-check
static void search_op(Trix *trx) {
    constexpr verify_t SEEK{VerifyString};
    constexpr verify_t STRING{VerifyString};
    trx->verify_operands(SEEK, STRING);

    auto seek_ptr = trx->m_op_ptr;
    auto str_ptr = (seek_ptr - 1);

    auto seek_sv = seek_ptr->sv_value(trx);
    auto string_sv = str_ptr->sv_value(trx);

    auto found = false;
    const char *ptr = nullptr;
    if (seek_sv.empty()) {
        found = true;
        ptr = string_sv.data();
    } else {
        auto it = std::search(string_sv.begin(), string_sv.end(), seek_sv.begin(), seek_sv.end());
        found = (it != string_sv.end());
        ptr = found ? std::to_address(it) : nullptr;
    }
    if (found) {
        trx->require_op_capacity(2);

        auto string = *str_ptr;
        auto seek_length = sv_length(seek_sv);
        auto string_data = string_sv.data();
        auto string_length = sv_length(string_sv);
        auto string_index = static_cast<length_t>(ptr - string_data);

        // post
        {
            auto post = str_ptr++;
            auto index = static_cast<length_t>(string_index + seek_length);
            auto count = static_cast<length_t>(string_length - index);
            post->string_interval(trx, index, count);
        }

        // match
        {
            auto match = str_ptr++;
            *match = string;
            match->string_interval(trx, string_index, seek_length);
        }

        // pre
        {
            auto pre = str_ptr++;
            *pre = string;
            pre->string_interval(trx, 0, string_index);
        }

        // result
        *str_ptr = Object::make_boolean(true);

        trx->m_op_ptr = str_ptr;
    } else {
        // result
        *seek_ptr = Object::make_boolean(false);
    }
}

// starts-with?: str str :- bool
// Tests if the first string starts with the second.
// throws: opstack-underflow, type-check
static void startswith_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto needle_ptr = trx->m_op_ptr;
    auto haystack_ptr = (needle_ptr - 1);
    auto result = haystack_ptr->sv_value(trx).starts_with(needle_ptr->sv_value(trx));
    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_boolean(result);
}

// ends-with?: str str :- bool
// Tests if the first string ends with the second.
// throws: opstack-underflow, type-check
static void endswith_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto needle_ptr = trx->m_op_ptr;
    auto haystack_ptr = (needle_ptr - 1);
    auto result = haystack_ptr->sv_value(trx).ends_with(needle_ptr->sv_value(trx));
    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_boolean(result);
}

// contains?: str str :- bool
// Tests if the first string contains the second.
// throws: opstack-underflow, type-check
static void contains_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto needle_ptr = trx->m_op_ptr;
    auto haystack_ptr = (needle_ptr - 1);
    auto hsv = haystack_ptr->sv_value(trx);
    auto nsv = needle_ptr->sv_value(trx);
    auto result = hsv.contains(nsv);
    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_boolean(result);
}

// string-index-of: str str :- int
// Returns position of first occurrence, or -1.
// throws: opstack-underflow, type-check
static void stringindexof_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto needle_ptr = trx->m_op_ptr;
    auto haystack_ptr = (needle_ptr - 1);
    auto hsv = haystack_ptr->sv_value(trx);
    auto nsv = needle_ptr->sv_value(trx);
    auto pos = hsv.find(nsv);
    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_integer((pos != std::string_view::npos) ? static_cast<integer_t>(pos) : -1);
}

// remove-prefix: str str :- str
// Removes prefix if present.
// throws: opstack-underflow, type-check
static void removeprefix_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto prefix = trx->m_op_ptr;
    auto str = (prefix - 1);
    auto ssv = str->sv_value(trx);
    auto psv = prefix->sv_value(trx);
    if (ssv.starts_with(psv)) {
        auto result = *str;
        result.string_interval(trx, sv_length(psv), static_cast<length_t>(ssv.size() - psv.size()));
        *str = result;
    }
    --trx->m_op_ptr;
}

// remove-suffix: str str :- str
// Removes suffix if present.
// throws: opstack-underflow, type-check
static void removesuffix_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto suffix = trx->m_op_ptr;
    auto str = (suffix - 1);
    auto ssv = str->sv_value(trx);
    auto xsv = suffix->sv_value(trx);
    if (ssv.ends_with(xsv) && !xsv.empty()) {
        auto result = *str;
        result.string_interval(trx, 0, static_cast<length_t>(ssv.size() - xsv.size()));
        *str = result;
    }
    --trx->m_op_ptr;
}

// repeat-string: str int :- str
// Repeats a string N times.
// throws: vm-full, opstack-underflow, range-check, type-check
static void repeatstring_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers, VerifyString);

    auto n_ptr = trx->m_op_ptr;
    auto str_ptr = (n_ptr - 1);
    auto [valid, n] = n_ptr->integer_value(trx, 0);
    if (!valid || (n < 0)) {
        trx->error(Error::RangeCheck, "repeat-string: count must be non-negative");
    } else {
        auto src_ptr = str_ptr->string_vptr(trx);
        auto src_len = str_ptr->string_length();
        auto total64 = static_cast<uint64_t>(src_len) * static_cast<uint64_t>(n);
        if (total64 > MaxStringLength) {
            trx->error(Error::LimitCheck, "repeat-string: result length {} exceeds maximum {}", total64, MaxStringLength);
        } else {
            auto total = static_cast<length_t>(total64);

            auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch<vm_t>(total + 1, ChunkKind::String);
            for (integer_t i = 0; i < n; ++i) {
                std::copy_n(src_ptr, src_len, dst_ptr + (static_cast<size_t>(i) * src_len));
            }
            dst_ptr[total] = '\0';
            n_ptr->maybe_free_extvalue(trx);
            --trx->m_op_ptr;
            *trx->m_op_ptr = Object::make_string(dst_offset, total);
        }
    }
}

// Shared implementation for pad-left and pad-right.
static void pad_string_impl(Trix *trx, bool pad_left, std::string_view op_name) {
    trx->verify_operands(VerifyByte, VerifyIntegers, VerifyString);

    auto fill_ptr = trx->m_op_ptr;
    auto width_ptr = (fill_ptr - 1);
    auto str_ptr = (fill_ptr - 2);

    auto fill = fill_ptr->byte_value();
    auto [valid, width] = width_ptr->integer_value(trx, 0, MaxLength);
    if (!valid || (width < 0)) {
        trx->error(Error::RangeCheck, "{}: width must be non-negative", op_name);
    } else {
        auto src_ptr = str_ptr->string_vptr(trx);
        auto src_len = str_ptr->string_length();

        if (src_len >= static_cast<length_t>(width)) {
            // already wide enough, return as-is
            width_ptr->maybe_free_extvalue(trx);
            trx->m_op_ptr -= 2;
        } else {
            auto target = static_cast<length_t>(width);
            auto pad_len = static_cast<length_t>(target - src_len);
            auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch<vm_t>(target + 1, ChunkKind::String);
            if (pad_left) {
                std::memset(dst_ptr, fill, pad_len);
                std::copy_n(src_ptr, src_len, dst_ptr + pad_len);
            } else {
                std::copy_n(src_ptr, src_len, dst_ptr);
                std::memset(dst_ptr + src_len, fill, pad_len);
            }
            dst_ptr[target] = '\0';
            width_ptr->maybe_free_extvalue(trx);
            trx->m_op_ptr -= 2;
            *trx->m_op_ptr = Object::make_string(dst_offset, target);
        }
    }
}

// pad-left: str width byte :- str'
// Pads string on the left to target width with fill byte.
// If string is already >= width, returns unchanged.
// throws: vm-full, opstack-underflow, type-check, range-check
static void padleft_op(Trix *trx) {
    pad_string_impl(trx, true, "pad-left");
}

// pad-right: str width byte :- str'
// Pads string on the right to target width with fill byte.
// If string is already >= width, returns unchanged.
// throws: vm-full, opstack-underflow, type-check, range-check
static void padright_op(Trix *trx) {
    pad_string_impl(trx, false, "pad-right");
}

// Emit one split piece into a result-array slot.  Under ${...} the piece must be
// a GLOBAL copy (make_string_region) so a global result array does not hold a
// restore-fragile local string-view into the source; otherwise it stays a cheap
// zero-copy view into the source string.  When emitting global copies the caller
// must null-init + root the result array across the fill (each copy can fire GC).
static void split_emit_piece(Trix *trx, Object *slot, Object orig, const char *src, length_t base_offset, length_t piece_len) {
    if (trx->m_curr_alloc_global) {
        *slot = Object::make_string_region(trx, src + base_offset, piece_len);
    } else {
        *slot = orig;
        slot->string_interval(trx, base_offset, piece_len);
    }
}

// split: str str :- arr
// Splits string by delimiter into an array of substrings.
// throws: vm-full, opstack-underflow, type-check
static void split_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto delim_ptr = trx->m_op_ptr;
    auto str_ptr = (delim_ptr - 1);

    auto delim_sv = delim_ptr->sv_value(trx);
    auto string_sv = str_ptr->sv_value(trx);

    if (delim_sv.empty()) {
        // empty delimiter: split into individual bytes
        auto len = sv_length(string_sv);
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(len, ChunkKind::Array);
        auto orig = *str_ptr;
        auto src = string_sv.data();
        auto result_obj = Object::make_array(dst_offset, len);
        auto rooted = trx->m_curr_alloc_global;
        if (rooted) {
            std::fill_n(dst_ptr, len, Object::make_null());
            trx->gc_root_push_oneoff(result_obj);
        }
        for (length_t i = 0; i < len; ++i) {
            split_emit_piece(trx, &dst_ptr[i], orig, src, i, 1);
        }
        if (rooted) {
            trx->gc_root_pop_oneoff();
        }
        trx->m_op_ptr -= 2;
        *++trx->m_op_ptr = result_obj;
    } else {
        // pass 1: count occurrences
        length_t count = 0;
        {
            auto haystack = string_sv;
            while (true) {
                auto it = std::search(haystack.begin(), haystack.end(), delim_sv.begin(), delim_sv.end());
                if (it == haystack.end()) {
                    break;
                } else {
                    ++count;
                    auto pos = static_cast<size_t>(it - haystack.begin()) + delim_sv.size();
                    haystack = haystack.substr(pos);
                }
            }
        }

        // allocate result array: count+1 pieces
        auto pieces32 = static_cast<vm_size_t>(count) + 1;
        if (pieces32 > MaxArrayLength) {
            trx->error(Error::LimitCheck, "split: result array length {} exceeds maximum {}", pieces32, MaxArrayLength);
        } else {
            auto pieces = static_cast<length_t>(pieces32);
            auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(pieces, ChunkKind::Array);

            // pass 2: split and fill.  Under ${...} split_emit_piece writes GLOBAL copies
            // (each can fire vm_global_gc), so null-init + root the result across the fill.
            auto orig = *str_ptr;
            auto src = string_sv.data();
            auto result_obj = Object::make_array(dst_offset, pieces);
            auto rooted = trx->m_curr_alloc_global;
            if (rooted) {
                std::fill_n(dst_ptr, pieces, Object::make_null());
                trx->gc_root_push_oneoff(result_obj);
            }
            auto haystack = string_sv;
            auto base_offset = length_t{0};
            length_t i = 0;
            while (i < count) {
                auto it = std::search(haystack.begin(), haystack.end(), delim_sv.begin(), delim_sv.end());
                auto piece_len = static_cast<length_t>(it - haystack.begin());
                split_emit_piece(trx, &dst_ptr[i], orig, src, base_offset, piece_len);
                auto skip = static_cast<length_t>(piece_len + sv_length(delim_sv));
                base_offset = static_cast<length_t>(base_offset + skip);
                haystack = haystack.substr(static_cast<size_t>(skip));
                ++i;
            }
            // last piece: remainder after final delimiter
            split_emit_piece(trx, &dst_ptr[i], orig, src, base_offset, static_cast<length_t>(haystack.size()));
            if (rooted) {
                trx->gc_root_pop_oneoff();
            }

            trx->m_op_ptr -= 2;
            *++trx->m_op_ptr = result_obj;
        }
    }
}

// chars: string :- array
// Explodes a string into an array of byte integers.
// throws: opstack-underflow, type-check, vm-full
static void chars_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto str_ptr = trx->m_op_ptr;
    auto sv = str_ptr->sv_value(trx);
    auto length = sv_length(sv);

    // Byte elements are immediate (no heap refs), so the array needs no rooting --
    // just route it to the current region so a ${...} result survives save/restore.
    auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(length, ChunkKind::Array);
    for (length_t i = 0; i < length; ++i) {
        dst_ptr[i] = Object::make_byte(static_cast<vm_t>(sv[i]));
    }
    *str_ptr = Object::make_array(dst_offset, length);
}

// String: returns true iff non-empty and every byte matches the mask.
template<chattr_t Mask>
static void char_predicate_op(Trix *trx) {
    trx->verify_operands(VerifyByte | VerifyString);
    auto top_ptr = trx->m_op_ptr;

    if (top_ptr->is_byte()) {
        auto ch = top_ptr->byte_value();
        *top_ptr = Object::make_boolean((sm_chattr[ch] & Mask) != 0);
    } else {
        auto sv = top_ptr->sv_value(trx);
        bool result = !sv.empty();
        for (auto ch : sv) {
            if ((sm_chattr[static_cast<unsigned char>(ch)] & Mask) == 0) {
                result = false;
                break;
            }
        }
        *top_ptr = Object::make_boolean(result);
    }
}

// printable?: byte|str :- bool
// Byte: true if 0x20..0x7E (POSIX isprint). String: true if non-empty and all bytes printable.
// throws: opstack-underflow, type-check
static void charprintable_op(Trix *trx) {
    trx->verify_operands(VerifyByte | VerifyString);
    auto top_ptr = trx->m_op_ptr;

    if (top_ptr->is_byte()) {
        auto ch = top_ptr->byte_value();
        *top_ptr = Object::make_boolean(ch >= 0x20 && ch <= 0x7E);
    } else {
        auto sv = top_ptr->sv_value(trx);
        bool result = !sv.empty();
        for (auto ch : sv) {
            auto uch = static_cast<unsigned char>(ch);
            if ((uch < 0x20) || (uch > 0x7E)) {
                result = false;
                break;
            }
        }
        *top_ptr = Object::make_boolean(result);
    }
}

// alnum?: byte|str :- bool       -- a-z A-Z 0-9
// throws: opstack-underflow, type-check
static void charalnum_op(Trix *trx) {
    char_predicate_op<LC | UC | DD>(trx);
}

// alpha?: byte|str :- bool       -- a-z A-Z
// throws: opstack-underflow, type-check
static void charalpha_op(Trix *trx) {
    char_predicate_op<LC | UC>(trx);
}

// digit?: byte|str :- bool       -- 0-9
// throws: opstack-underflow, type-check
static void chardigit_op(Trix *trx) {
    char_predicate_op<DD>(trx);
}

// hex-digit?: byte|str :- bool   -- 0-9 A-F a-f
// throws: opstack-underflow, type-check
static void charhexdigit_op(Trix *trx) {
    char_predicate_op<HD>(trx);
}

// lower?: byte|str :- bool       -- a-z
// throws: opstack-underflow, type-check
static void charlower_op(Trix *trx) {
    char_predicate_op<LC>(trx);
}

// space?: byte|str :- bool       -- HT LF VT FF CR SP
// throws: opstack-underflow, type-check
static void charspace_op(Trix *trx) {
    char_predicate_op<WS>(trx);
}

// upper?: byte|str :- bool       -- A-Z
// throws: opstack-underflow, type-check
static void charupper_op(Trix *trx) {
    char_predicate_op<UC>(trx);
}
