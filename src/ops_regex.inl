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

//===--- Regular Expression Operators ---===//
//
// Implements regular expression matching, searching, and replacement using
// POSIX Extended Regular Expressions (ERE) via the system regex library
// (regex.h: regcomp/regexec/regfree).
//
// Design choice: POSIX ERE rather than PCRE or std::regex because:
//   - Available on all POSIX systems with zero dependencies.
//   - Predictable performance (no catastrophic backtracking in most
//     implementations, unlike PCRE and std::regex).
//   - Sufficient for the embedded scripting use case (no lookahead,
//     backreferences, or Unicode properties needed).
//   - std::regex is notoriously slow in libstdc++ and libc++.
//
// All operators compile the pattern on each call (no cached compiled
// regex objects).  This is intentional: regex compilation is fast for
// typical patterns, and caching would require a new VM type or a
// lifetime-managed cache structure.  If profiling shows regex compilation
// as a bottleneck, caching can be added later.
//
// --- Core concepts for maintainers ---
//
// PATTERN SYNTAX
//   POSIX Extended Regular Expressions (ERE):
//     .          any character
//     [abc]      character class
//     [^abc]     negated class
//     ^  $       anchors (start/end of string)
//     *  +  ?    quantifiers (zero+, one+, optional)
//     {n,m}      counted repetition
//     (...)      capturing group
//     |          alternation
//     \          escape
//
//   Full-match semantics require explicit anchoring: use ^pattern$ to
//   match the entire string.  Without anchors, regexec matches any
//   substring.
//
// REGEXGUARD
//   A stack-allocated RAII wrapper around regex_t.  Calls regcomp in
//   compile() and regfree in the destructor.  If regcomp fails, raises
//   a Trix SyntaxError with the error message from regerror().
//
// STRING HANDLING
//   The subject and pattern -- the strings the POSIX regex API reads to a
//   NUL -- are passed via string_cstr(), which returns the buffer directly
//   when it is already NUL-terminated and otherwise copies into a
//   NUL-terminated VM buffer.  The replacement string is copied by length
//   (sv_value()) and does not need termination.  Results (match groups,
//   replacements) are constructed as new Trix strings on the VM heap.
//
// --- Operators ---
//
//   regex-match      str pattern -- bool          Full match test
//   regex-search     str pattern -- [groups] bool  Search with capture groups
//   regex-find-all   str pattern -- array          All non-overlapping matches
//   regex-replace    str pattern repl -- str        Replace all occurrences
//   regex-split      str pattern -- array          Split string by pattern
//

// RAII wrapper for POSIX regex_t
struct RegexGuard {
    RegexGuard() : m_compiled{false} {}

    ~RegexGuard() {
        if (m_compiled) {
            ::regfree(&m_re);
        }
    }

    RegexGuard(const RegexGuard &) = delete;
    RegexGuard &operator=(const RegexGuard &) = delete;

    void compile(Trix *trx, std::string_view pattern, int extra_flags = 0) {
        const int rc = ::regcomp(&m_re, pattern.data(), REG_EXTENDED | extra_flags);
        if (rc != 0) {
            char errbuf[MaxErrorLength];
            ::regerror(rc, &m_re, errbuf, sizeof(errbuf));
            ::regfree(&m_re);
            trx->error(Error::SyntaxError, "regex: {}", errbuf);
        } else {
            m_compiled = true;
        }
    }

    [[nodiscard]] size_t nsub() const { return m_re.re_nsub; }

    regex_t m_re;
    bool m_compiled;
};

// regex-match: string pattern :- bool
// Tests whether the entire string matches the ERE pattern.
// throws: opstack-underflow, type-check, syntax-error
static void regexmatch_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto pattern_ptr = trx->m_op_ptr;
    auto str_ptr = (pattern_ptr - 1);

    // NUL-terminate for the POSIX regex C API: regcomp/regexec read to a NUL, so a
    // non-terminated substring would over-read into adjacent VM bytes (string_cstr is
    // zero-copy when the string is already terminated, which the common case is).
    auto pattern_sv = std::string_view(pattern_ptr->string_cstr(trx), pattern_ptr->string_length());
    auto string_sv = std::string_view(str_ptr->string_cstr(trx), str_ptr->string_length());

    RegexGuard guard;
    guard.compile(trx, pattern_sv, REG_NOSUB);

    const int rc = ::regexec(&guard.m_re, string_sv.data(), 0, nullptr, 0);

    // regexec reports whether any substring matches the pattern.
    // For full-match semantics, the user should anchor with ^...$.
    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_boolean(rc == 0);
}

// regex-search: string pattern :- [matches] true | false
// Finds the first match.  Returns array of [full, group1, ...] and true,
// or just false if no match.
// throws: opstack-underflow, type-check, syntax-error
static void regexsearch_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto pattern_ptr = trx->m_op_ptr;
    auto str_ptr = (pattern_ptr - 1);

    // NUL-terminate for the POSIX regex C API (see regex-match).
    auto pattern_sv = std::string_view(pattern_ptr->string_cstr(trx), pattern_ptr->string_length());
    auto string_sv = std::string_view(str_ptr->string_cstr(trx), str_ptr->string_length());

    RegexGuard guard;
    guard.compile(trx, pattern_sv);

    static constexpr size_t MAX_REGEX_GROUPS = 32;
    auto nmatch = std::min(guard.nsub() + 1, MAX_REGEX_GROUPS);
    regmatch_t pmatch[MAX_REGEX_GROUPS];

    const int rc = ::regexec(&guard.m_re, string_sv.data(), nmatch, pmatch, 0);

    if (rc != 0) {
        // no match
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_boolean(false);
    } else {
        // Build an array of all groups (group 0 is the whole match).  An unmatched
        // optional group (rm_so == -1) may sit BEFORE a matched one (e.g. "(a)(b)?(c)"
        // on "ac"), so walk the full group count and represent an unmatched group
        // as an empty string -- group N stays at index N rather than being dropped.
        auto group_count = static_cast<length_t>(nmatch);
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(group_count, ChunkKind::Array);
        // Region-aware group strings: under ${...} make_string_region copies each group
        // into global VM (so the result array survives save/restore) and each copy can
        // fire vm_global_gc -- null-init + root the array across the fill.
        std::fill_n(dst_ptr, group_count, Object::make_null());
        auto result_obj = Object::make_array(dst_offset, group_count);
        trx->gc_root_push_oneoff(result_obj);
        for (length_t i = 0; i < group_count; ++i) {
            if (pmatch[i].rm_so == -1) {
                dst_ptr[i] = Object::make_string_region(trx, string_sv.data(), 0);
            } else {
                auto start = pmatch[i].rm_so;
                auto end = pmatch[i].rm_eo;
                dst_ptr[i] = Object::make_string_region(trx, string_sv.data() + start, static_cast<length_t>(end - start));
            }
        }
        trx->gc_root_pop_oneoff();

        --trx->m_op_ptr;
        *trx->m_op_ptr = result_obj;
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_boolean(true);
    }
}

// regex_advance: shared cursor-advance logic for regex iteration.
// Advances cursor past the current match, handling zero-length matches
// by advancing one byte.  Returns false when iteration should stop.
[[nodiscard]] static bool
regex_advance(const char *&cursor, std::string_view &remaining, std::string_view string_sv, const regmatch_t &match) {
    if (match.rm_so == match.rm_eo) {
        // zero-length match: advance one byte to avoid infinite loop
        if (*cursor == '\0') {
            return false;
        } else {
            ++cursor;
            remaining = std::string_view(cursor, static_cast<size_t>(string_sv.data() + string_sv.size() - cursor));
            return true;  // continue but this iteration produced no match
        }
    }
    cursor += match.rm_eo;
    remaining = std::string_view(cursor, static_cast<size_t>(string_sv.data() + string_sv.size() - cursor));
    return true;
}

// regex-find-all: string pattern :- array
// Finds all non-overlapping matches.  When pattern has groups, each
// element is an array [full, g1, ...]; without groups, each element
// is the matched string.  Two-pass: count matches, then allocate and fill.
// throws: opstack-underflow, type-check, syntax-error
static void regexfindall_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto pattern_ptr = trx->m_op_ptr;
    auto str_ptr = (pattern_ptr - 1);

    // NUL-terminate for the POSIX regex C API (see regex-match).
    auto pattern_sv = std::string_view(pattern_ptr->string_cstr(trx), pattern_ptr->string_length());
    auto string_sv = std::string_view(str_ptr->string_cstr(trx), str_ptr->string_length());

    RegexGuard guard;
    guard.compile(trx, pattern_sv);

    static constexpr size_t MAX_REGEX_GROUPS = 32;
    auto nmatch = std::min(guard.nsub() + 1, MAX_REGEX_GROUPS);
    auto has_groups = guard.nsub() > 0;
    regmatch_t pmatch[MAX_REGEX_GROUPS];

    // pass 1: count matches
    length_t match_count = 0;
    const char *cursor = string_sv.data();
    auto remaining = string_sv;
    while (!remaining.empty()) {
        const int rc = ::regexec(&guard.m_re, cursor, nmatch, pmatch, 0);
        if (rc != 0) {
            break;
        } else if (pmatch[0].rm_so == pmatch[0].rm_eo) {
            // zero-length match: advance past one byte and retry (no match recorded here)
            if (!regex_advance(cursor, remaining, string_sv, pmatch[0])) {
                break;
            }
        } else {
            ++match_count;
            static_cast<void>(regex_advance(cursor, remaining, string_sv, pmatch[0]));
        }
    }

    if (!has_groups) {
        // pass 2 (no groups): allocate array, re-run regex filling strings.  Region-
        // aware match strings (make_string_region) survive save/restore under ${...};
        // each copy can fire vm_global_gc, so null-init + root the array across the fill.
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(match_count, ChunkKind::Array);
        std::fill_n(dst_ptr, match_count, Object::make_null());
        auto result_obj = Object::make_array(dst_offset, match_count);
        trx->gc_root_push_oneoff(result_obj);
        length_t k = 0;
        cursor = string_sv.data();
        remaining = string_sv;
        while (!remaining.empty() && (k < match_count)) {
            const int rc = ::regexec(&guard.m_re, cursor, 1, pmatch, 0);
            if (rc != 0) {
                break;
            } else if (pmatch[0].rm_so == pmatch[0].rm_eo) {
                // zero-length match: advance past one byte and retry (no match recorded here)
                if (!regex_advance(cursor, remaining, string_sv, pmatch[0])) {
                    break;
                }
            } else {
                dst_ptr[k++] = Object::make_string_region(
                        trx, cursor + pmatch[0].rm_so, static_cast<length_t>(pmatch[0].rm_eo - pmatch[0].rm_so));
                static_cast<void>(regex_advance(cursor, remaining, string_sv, pmatch[0]));
            }
        }
        trx->gc_root_pop_oneoff();
        --trx->m_op_ptr;
        *trx->m_op_ptr = result_obj;
    } else {
        // pass 2 (with groups): allocate outer array first, then fill each match
        // with a sub-array of [full, g1, g2, ...].  Region-aware outer + sub arrays
        // and group strings survive save/restore under ${...}; root the outer across
        // the fill and each sub-array across ITS fill (allocs can fire vm_global_gc).
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(match_count, ChunkKind::Array);
        std::fill_n(dst_ptr, match_count, Object::make_null());
        auto result_obj = Object::make_array(dst_offset, match_count);
        trx->gc_root_push_oneoff(result_obj);
        length_t k = 0;
        cursor = string_sv.data();
        remaining = string_sv;
        while (!remaining.empty() && (k < match_count)) {
            const int rc = ::regexec(&guard.m_re, cursor, nmatch, pmatch, 0);
            if (rc != 0) {
                break;
            } else if (pmatch[0].rm_so == pmatch[0].rm_eo) {
                // zero-length match: advance past one byte and retry (no match recorded here)
                if (!regex_advance(cursor, remaining, string_sv, pmatch[0])) {
                    break;
                }
            } else {
                // Allocate a sub-array of all groups [full, g1, g2, ...].  An
                // unmatched optional group (rm_so == -1) may precede a matched one,
                // so walk the full group count and emit an empty string for an
                // unmatched group -- keeping group N at index N.
                auto group_count = static_cast<length_t>(nmatch);
                auto [sub_ptr, sub_offset] = trx->vm_alloc_dispatch_n<Object>(group_count, ChunkKind::Array);
                std::fill_n(sub_ptr, group_count, Object::make_null());
                // Link the sub-array into the (rooted) outer result BEFORE filling it, so
                // the outer root keeps the sub-array and its in-progress group strings
                // marked across the per-group make_string_region allocs.  A separate
                // gc_root_push_oneoff for the sub-array would be illegal here (the outer
                // root is already on the gc-root stack, and oneoff asserts it is empty).
                dst_ptr[k] = Object::make_array(sub_offset, group_count);
                for (length_t g = 0; g < group_count; ++g) {
                    if (pmatch[g].rm_so == -1) {
                        sub_ptr[g] = Object::make_string_region(trx, cursor, 0);
                    } else {
                        sub_ptr[g] = Object::make_string_region(
                                trx, cursor + pmatch[g].rm_so, static_cast<length_t>(pmatch[g].rm_eo - pmatch[g].rm_so));
                    }
                }
                ++k;
                static_cast<void>(regex_advance(cursor, remaining, string_sv, pmatch[0]));
            }
        }
        trx->gc_root_pop_oneoff();
        --trx->m_op_ptr;
        *trx->m_op_ptr = result_obj;
    }
}

// regex-replace: string pattern replacement :- string
// Replaces all non-overlapping matches of pattern with replacement.
// Two-pass: compute result length, then allocate and fill.
// throws: opstack-underflow, type-check, syntax-error
static void regexreplace_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString, VerifyString);

    auto repl_ptr = trx->m_op_ptr;
    auto pattern_ptr = (repl_ptr - 1);
    auto str_ptr = (repl_ptr - 2);

    // NUL-terminate the subject + pattern for the POSIX regex C API (see regex-match);
    // the replacement is copied by length, so it does not need termination.
    auto string_sv = std::string_view(str_ptr->string_cstr(trx), str_ptr->string_length());
    auto pattern_sv = std::string_view(pattern_ptr->string_cstr(trx), pattern_ptr->string_length());
    auto replacement_sv = repl_ptr->sv_value(trx);

    RegexGuard guard;
    guard.compile(trx, pattern_sv);

    regmatch_t pmatch[1];

    // pass 1: compute result length
    vm_size_t result_len32 = 0;
    const char *cursor = string_sv.data();
    auto remaining = string_sv;
    while (!remaining.empty()) {
        const int rc = ::regexec(&guard.m_re, cursor, 1, pmatch, 0);
        if (rc != 0) {
            break;
        } else if (pmatch[0].rm_so == pmatch[0].rm_eo) {
            // zero-length match: advance past one byte and retry (no match recorded here)
            if (!regex_advance(cursor, remaining, string_sv, pmatch[0])) {
                break;
            } else {
                ++result_len32;  // the skipped byte
            }
        } else {
            result_len32 += static_cast<vm_size_t>(pmatch[0].rm_so);        // prefix before match
            result_len32 += static_cast<vm_size_t>(replacement_sv.size());  // replacement
            static_cast<void>(regex_advance(cursor, remaining, string_sv, pmatch[0]));
        }
    }
    // tail after last match
    result_len32 += static_cast<vm_size_t>(string_sv.data() + string_sv.size() - cursor);
    if (result_len32 > MaxStringLength) {
        trx->error(Error::LimitCheck, "regex-replace: result length {} exceeds maximum {}", result_len32, MaxStringLength);
    } else {
        auto result_len = static_cast<length_t>(result_len32);

        // pass 2: allocate VM string and fill
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch<char>(result_len + 1, ChunkKind::String);
        char *out = dst_ptr;
        cursor = string_sv.data();
        remaining = string_sv;
        while (!remaining.empty()) {
            const int rc = ::regexec(&guard.m_re, cursor, 1, pmatch, 0);
            if (rc != 0) {
                break;
            } else if (pmatch[0].rm_so == pmatch[0].rm_eo) {
                // zero-length match: advance past one byte and retry (no match recorded here)
                if (!regex_advance(cursor, remaining, string_sv, pmatch[0])) {
                    break;
                } else {
                    *out++ = *(cursor - 1);  // copy the skipped byte
                }
            } else {
                // copy prefix before match
                std::copy_n(cursor, pmatch[0].rm_so, out);
                out += pmatch[0].rm_so;
                // copy replacement
                std::copy_n(replacement_sv.data(), replacement_sv.size(), out);
                out += replacement_sv.size();
                static_cast<void>(regex_advance(cursor, remaining, string_sv, pmatch[0]));
            }
        }
        // copy tail
        auto tail_len = string_sv.data() + string_sv.size() - cursor;
        std::copy_n(cursor, tail_len, out);
        out += tail_len;
        *out = '\0';  // nul-terminate

        trx->m_op_ptr -= 2;
        *trx->m_op_ptr = Object::make_string(dst_offset, result_len);
    }
}

// regex-split: string pattern :- array
// Splits string on all non-overlapping matches of pattern.
// Two-pass: count segments, then allocate and fill.
// throws: opstack-underflow, type-check, syntax-error
static void regexsplit_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto pattern_ptr = trx->m_op_ptr;
    auto str_ptr = (pattern_ptr - 1);

    // NUL-terminate for the POSIX regex C API (see regex-match).
    auto pattern_sv = std::string_view(pattern_ptr->string_cstr(trx), pattern_ptr->string_length());
    auto string_sv = std::string_view(str_ptr->string_cstr(trx), str_ptr->string_length());

    RegexGuard guard;
    guard.compile(trx, pattern_sv);

    regmatch_t pmatch[1];

    // pass 1: count segments (= number of matches + 1)
    length_t match_count = 0;
    const char *cursor = string_sv.data();
    auto remaining = string_sv;
    while (!remaining.empty()) {
        const int rc = ::regexec(&guard.m_re, cursor, 1, pmatch, 0);
        if (rc != 0) {
            break;
        } else if (pmatch[0].rm_so == pmatch[0].rm_eo) {
            // zero-length match: advance past one byte and retry (no match recorded here)
            if (!regex_advance(cursor, remaining, string_sv, pmatch[0])) {
                break;
            }
        } else {
            ++match_count;
            static_cast<void>(regex_advance(cursor, remaining, string_sv, pmatch[0]));
        }
    }
    auto seg_count32 = static_cast<vm_size_t>(match_count) + 1;
    if (seg_count32 > MaxArrayLength) {
        trx->error(Error::LimitCheck, "regex-split: result array length {} exceeds maximum {}", seg_count32, MaxArrayLength);
    } else {
        auto seg_count = static_cast<length_t>(seg_count32);

        // pass 2: allocate array and fill with segment strings.  Region-aware segment
        // strings (make_string_region) survive save/restore under ${...}; each copy can
        // fire vm_global_gc, so null-init + root the array across the fill.
        auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(seg_count, ChunkKind::Array);
        std::fill_n(dst_ptr, seg_count, Object::make_null());
        auto result_obj = Object::make_array(dst_offset, seg_count);
        trx->gc_root_push_oneoff(result_obj);
        length_t k = 0;
        cursor = string_sv.data();
        remaining = string_sv;
        while (!remaining.empty() && (k < match_count)) {
            const int rc = ::regexec(&guard.m_re, cursor, 1, pmatch, 0);
            if (rc != 0) {
                break;
            } else if (pmatch[0].rm_so == pmatch[0].rm_eo) {
                // zero-length match: advance past one byte and retry (no match recorded here)
                if (!regex_advance(cursor, remaining, string_sv, pmatch[0])) {
                    break;
                }
            } else {
                // segment before match
                dst_ptr[k++] = Object::make_string_region(trx, cursor, static_cast<length_t>(pmatch[0].rm_so));
                static_cast<void>(regex_advance(cursor, remaining, string_sv, pmatch[0]));
            }
        }
        // tail segment
        dst_ptr[k] = Object::make_string_region(trx, cursor, static_cast<length_t>(string_sv.data() + string_sv.size() - cursor));
        trx->gc_root_pop_oneoff();

        --trx->m_op_ptr;
        *trx->m_op_ptr = result_obj;
    }
}
