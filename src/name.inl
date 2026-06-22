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

//===--- Name: Interned String Identifier ---===//
//
// Implements interned string identifiers (names / symbols / atoms).  Based on:
//
//   PostScript names: the primary identifier type, used for variable binding,
//   operator names, dictionary keys, and type names.  `/foo` is a literal
//   name; `foo` (without slash) is an executable name that triggers lookup.
//
//   Lisp symbols / Erlang atoms / Ruby symbols: interned strings where
//   equality is an O(1) pointer comparison rather than O(n) string compare.
//
// --- Core concepts for maintainers ---
//
// INTERNING
//   Every unique name string is stored exactly once in VM heap memory.
//   Name::make() looks up the string in the name hash table; if found, it
//   returns the existing Name; if not, it allocates a new Name struct and
//   inserts it.  After interning, two Names with the same string have the
//   same vm_offset_t, so equality is a single integer comparison.
//
// NAME STRUCTURE
//   Each Name is stored on the VM heap as:
//     [next (vm_offset_t)] [binding (vm_offset_t)] [hash (hash_t)]
//     [length (length_t)] [data (vm_t[length])]
//   binding is the single-coroutine fast-path cache (nulloffset = miss);
//   The hash is precomputed (wyhash, 32-bit output) for O(1) hash table lookup.
//   The next field threads the bucket chain in the name hash table.
//
// BINDING CACHE
//   The name->value binding cache is hybrid, selected by live-coroutine count.
//   Single-coroutine mode (m_live_coroutine_count == 0, the common case)
//   reads the per-Name m_binding fast-path cache -- one deref.  Once any
//   coroutine spawns, name_search() (via cached_binding()) switches to the
//   running coroutine's chained hash table keyed by Name offset (each
//   CoroutineContext owns one; see binding_table.inl).  A miss in either
//   path falls back to the dict-stack walk.  Every m_binding is flushed at
//   the 0 -> 1 spawn transition, so the old cross-actor m_binding race
//   cannot occur once the per-coroutine tables are in use.
//
// NAME CATEGORIES
//   System names:  pre-allocated during init for all operators, type names,
//     error names, and well-known constants.  Indexed by SystemName enum.
//   User names:    allocated on demand by Name::make() during scanning or
//     explicit construction.
//
// NAME LOOKUP
//   Name resolution follows three paths (checked in order):
//   1. Binding cache hit: O(1) when the cache for the current mode holds the
//      name (per-Name m_binding in single-coroutine mode, else the running
//      coroutine's binding table).  No per-name save-level tracking.
//   2. Path lookup: hierarchical `//:dict:key` syntax for explicit dict access.
//   3. Dictionary stack walk: top to bottom, hash lookup in each dict.
//
// HASH TABLE
//   Names are stored in a separate hash table (m_name_buckets) distinct
//   from dictionaries.  Bucket count is configurable (--name-buckets),
//   snapped to a prime from a precomputed table for good distribution.
//   Chains are validated by validate_chains() (bounded by
//   MaxNameBucketChainLength to detect corruption/cycles).
//
class Name {
public:
    // Single-coroutine fast-path accessors.  See m_binding declaration for
    // gating semantics.  No save-level tracking: gate transition (0 -> 1
    // spawn count) flushes every m_binding; restore() in single-coroutine
    // mode does the same.
    [[nodiscard]] vm_offset_t binding() const { return m_binding; }
    void set_binding(vm_offset_t value_offset) { m_binding = value_offset; }
    void clear_binding() { m_binding = nulloffset; }

    [[nodiscard]] hash_t hash() const { return m_hash; }

    [[nodiscard]] vm_offset_t next() const { return m_next; }

    [[nodiscard]] length_t length() const { return m_length; }

    // Validate all name bucket chains: every offset is within [0, vm_ptr),
    // chain lengths are bounded (no cycles).
    [[nodiscard]] static bool validate_chains(Trix *trx) {
        for (name_bucket_count_t i = 0; i < trx->m_name_bucket_count; ++i) {
            auto offset = trx->m_name_buckets[i];
            integer_t chain_len = 0;
            while (offset != nulloffset) {
                if (trx->valid_active_offset<vm_t>(offset)) {
                    auto name = trx->offset_to_ptr<Name>(offset);
                    offset = name->m_next;
                    if (++chain_len > MaxNameBucketChainLength) {
                        return false;  // probable cycle or corruption
                    }
                } else {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] std::string_view sv() const { return std::string_view{reinterpret_cast<const char *>(m_data), m_length}; }

    [[nodiscard]] bool equal(std::string_view sv) const {
        return ((m_length == sv_length(sv)) && (std::memcmp(m_data, sv.data(), m_length) == 0));
    }

    [[nodiscard]] static Object make(Trix *trx, std::string_view sv, Object::attrib_t attrib = Object::LiteralAttrib) {
        return Object::make_name(add(trx, sv), sv_length(sv), attrib);
    }

    [[nodiscard]] static Object make_system(Trix *trx, SystemName name, Object::attrib_t attrib = Object::LiteralAttrib) {
        auto offset = trx->m_systemname_offsets[+name];
        if (offset == nulloffset) {
            trx->error(Error::Undefined, "internal operator not accessible");
        } else {
            auto op = sysname_value(+name);
            return Object::make_name(offset, sv_length(op.m_sv), attrib);
        }
    }

    struct EnumMapping {
        int value;
        SystemName name;
    };

    static constexpr EnumMapping sm_fp_classification_map[] = {
            {   FP_NORMAL,    SystemName::FP_Normal},
            {FP_SUBNORMAL, SystemName::FP_SubNormal},
            {     FP_ZERO,      SystemName::FP_Zero},
            { FP_INFINITE,  SystemName::FP_Infinite},
            {      FP_NAN,       SystemName::FP_NaN},
    };

    static constexpr EnumMapping sm_fe_rounding_map[] = {
            {  FE_DOWNWARD,   SystemName::FE_Downward},
            { FE_TONEAREST,  SystemName::FE_ToNearest},
            {FE_TOWARDZERO, SystemName::FE_TowardZero},
            {    FE_UPWARD,     SystemName::FE_Upward},
    };

    template<std::size_t N>
    [[nodiscard]] static Object lookup_enum_name(Trix *trx, int value, const EnumMapping (&map)[N], SystemName default_name) {
        for (const auto &entry : map) {
            if (entry.value == value) {
                return make_system(trx, entry.name);
            }
        }
        return make_system(trx, default_name);
    }

    [[nodiscard]] static Object make_number_classification(Trix *trx, int classification) {
        return lookup_enum_name(trx, classification, sm_fp_classification_map, SystemName::FP_Other);
    }

    [[nodiscard]] static Object make_number_rounding(Trix *trx, int rounding) {
        return lookup_enum_name(trx, rounding, sm_fe_rounding_map, SystemName::FE_Other);
    }

    [[nodiscard]] bool is_systemname(Trix *trx, SystemName name) const {
        return (trx->ptr_to_offset(this) == trx->m_systemname_offsets[+name]);
    }

    // Look up a Name's cached binding offset (nulloffset if uncached).
    // Single-coroutine mode (the common case for any script that does not spawn
    // actors / pipelines) reads the global Name::m_binding cache -- one deref,
    // matching the pre-per-coroutine-cache cost; multi-coroutine mode consults
    // the running coroutine's binding table.  Shared by name_search /
    // string_search.
    [[nodiscard]] static vm_offset_t cached_binding(Trix *trx, Name *name, vm_offset_t name_offset) {
        if (trx->m_live_coroutine_count == 0) {
            return name->binding();
        } else {
            auto running = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
            return trx->binding_table_lookup(running->m_binding_table, name_offset);
        }
    }

    [[nodiscard]] static const Object *name_search(Trix *trx, const Object *key_ptr) {
        assert(key_ptr->is_name());

        auto name = key_ptr->name_value(trx);

        auto binding_offset = cached_binding(trx, name, key_ptr->name_offset());
        if (binding_offset != nulloffset) {
            return trx->offset_to_ptr<Object>(binding_offset);
        } else {
            // Path syntax (`:rootdict:...`) is only meaningful when the name begins with ':'.
            // Gate the dict_path_search call on the first-char check so non-path names (the
            // overwhelming majority) skip the function-call boundary entirely.  The length
            // guard keeps the zero-length name (`() cvn`) from reading m_data[0], which lies
            // one byte past a 0-char Name's allocation.
            if ((name->length() != 0) && (name->data()[0] == ':')) {
                auto result_ptr = dict_path_search(trx, name->sv());
                if (result_ptr != nullptr) {
                    return result_ptr;
                }
            }

            // Fall through to the dict-stack walk.  name_lookup_in_stack populates
            // whichever cache the gate selected via set_name_binding on the way out.
            return Dict::name_lookup_in_stack(trx, key_ptr, name);
        }
    }

    [[nodiscard]] static const Object *string_search(Trix *trx, std::string_view sv) {
        auto result_ptr = dict_path_search(trx, sv);
        if (result_ptr == nullptr) {
            auto [found, key_obj] = find(trx, sv);
            if (found) {
                auto name = key_obj.name_value(trx);
                auto binding_offset = cached_binding(trx, name, key_obj.name_offset());
                if (binding_offset != nulloffset) {
                    result_ptr = trx->offset_to_ptr<Object>(binding_offset);
                } else {
                    result_ptr = Dict::name_lookup_in_stack(trx, &key_obj, name);
                }
            }
        }
        return result_ptr;
    }

    static void restore(Trix *trx, vm_offset_t barrier) {
        // Restore the name table: unlink any Name allocated above the barrier.
        // Per-coroutine binding caches are cleared separately by save::restore's
        // context sweep.
        //
        // In-global: Name structs allocated under m_curr_alloc_global
        // live in the global VM region.  Their offsets are numerically
        // above-barrier (globals sit at high addresses) but they never roll
        // back -- skip them via !is_global().  This breaks the prior
        // "first above-barrier offset terminates the chain" invariant: a
        // global Name appended after a local one means the chain is no
        // longer monotonic in offset, so we walk the FULL chain and unlink
        // entries individually.  Same O(chain length) cost.
        auto bucket_count = trx->m_name_bucket_count;
        for (name_bucket_count_t i = 0; i < bucket_count; ++i) {
            vm_offset_t *prev_link = &trx->m_name_buckets[i];
            while (*prev_link != nulloffset) {
                const vm_offset_t offset = *prev_link;
                auto name = trx->offset_to_ptr<Name>(offset);
                if (!trx->is_global(offset) && (offset >= barrier)) {
                    *prev_link = name->m_next;
                } else {
                    prev_link = &name->m_next;
                }
            }
        }
    }

    struct NameStatus {
        integer_t count{0};
        uinteger_t vm_used{0};
        integer_t max_chain{0};
        real_t avg_chain{0.0f};
    };

    // Complementary Sum Technique (Tail Sum Formula):
    //  total = Sum_{i=1}^{L_max} i - |{chains with length >= i}|
    //  This equals Sum_j L_j(L_j+1)/2, the correct total comparisons for successful lookup
    [[nodiscard]] static NameStatus status(Trix *trx) {
        auto [chain_lengths, available] = trx->vm_start_alloc<integer_t>();
        auto bucket_count = trx->m_name_bucket_count;
        auto required = (bucket_count * sizeof(integer_t));
        if (available >= required) {
            auto vm_used = vm_size_t{0};
            auto name_count = integer_t{0};
            auto max_chain_length = integer_t{0};
            for (name_bucket_count_t i = 0; i < bucket_count; ++i) {
                auto curr_chain_length = integer_t{0};
                auto offset = trx->m_name_buckets[i];
                while (offset != nulloffset) {
                    auto name = trx->offset_to_ptr<Name>(offset);
                    constexpr auto align_mask = vm_size_t{alignof(Name) - 1};
                    vm_used += (alloc_size(name->length()) + align_mask) & ~align_mask;
                    ++curr_chain_length;
                    ++name_count;
                    offset = name->m_next;
                }
                chain_lengths[i] = curr_chain_length;
                max_chain_length = std::max(max_chain_length, curr_chain_length);
            }

            long_t total = 0;
            for (integer_t i = 1; i <= max_chain_length; ++i) {
                long_t count = 0;
                for (name_bucket_count_t j = 0; j < bucket_count; ++j) {
                    if (chain_lengths[j] >= i) {
                        ++count;
                    }
                }
                total += (i * count);
            }
            trx->vm_end_alloc();

            const real_t avg = (name_count != 0) ? (static_cast<real_t>(total) / static_cast<real_t>(name_count)) : 0.0f;
            return NameStatus{name_count, static_cast<uinteger_t>(vm_used), max_chain_length, avg};
        } else {
            trx->vm_end_alloc();
            trx->error(Error::VMFull, "cannot allocate {} bytes of temporary storage", required);
        }
    }

    // add a string to the Name table.  When m_curr_alloc_global is set
    // (entered via the `${...}` scanner block, the `$/foo`/`$\foo`
    // prefixes, or explicitly via `set-global`), the new Name struct
    // is allocated in global VM and survives every save/restore cycle.
    // Otherwise it goes to local VM and is reclaimed by Name::restore
    // if interned above a save barrier.  Mirrors PostScript Level 2/3
    // (PLRM section 3.7.2): the current-global flag picks the destination at
    // intern time.
    [[nodiscard]] static vm_offset_t add(Trix *trx, std::string_view sv) {
        const hash_t hash = wyhash32_sv(sv);
        vm_offset_t *chain = &trx->m_name_buckets[fastmod_u32(hash, trx->m_name_bucket_magic, trx->m_name_bucket_count)];
        while (true) {
            const vm_offset_t offset = *chain;
            if (offset == nulloffset) {
                // end of hash chain, add the string
                auto length = sv_length(sv);
                auto alloc_bytes = alloc_size(length);
                auto [name, name_offset] = trx->vm_alloc_dispatch<Name>(alloc_bytes, ChunkKind::Name);
                name->m_next = nulloffset;
                name->m_binding = nulloffset;
                name->m_hash = hash;
                name->m_length = length;
                std::copy_n(sv.data(), length, name->data());

                *chain = name_offset;
                return name_offset;
            } else {
                auto name = trx->offset_to_ptr<Name>(offset);
                if ((hash == name->m_hash) && name->equal(sv)) {
                    // already in Name table
                    return offset;
                } else {
                    // walk the hash chain
                    chain = &name->m_next;
                }
            }
        }
    }
private:
    Name() = default;

    [[nodiscard]] static vm_size_t alloc_size(length_t length) { return static_cast<vm_size_t>(offsetof(Name, m_data) + length); }

    [[nodiscard]] static std::pair<bool, Object> find(Trix *trx, std::string_view sv) {
        const hash_t hash = wyhash32_sv(sv);
        vm_offset_t offset = trx->m_name_buckets[fastmod_u32(hash, trx->m_name_bucket_magic, trx->m_name_bucket_count)];
        while (offset != nulloffset) {
            auto name = trx->offset_to_ptr<Name>(offset);
            if ((hash == name->m_hash) && name->equal(sv)) {
                return std::pair{true, Object::make_name(offset, name->m_length)};
            } else {
                offset = name->m_next;
            }
        }
        return std::pair{false, Object::make_null()};
    }

    [[nodiscard]] static std::pair<Dict *, const char *> check_systemdicts(Trix *trx, std::string_view sv) {
        using namespace std::literals::string_view_literals;

        if (sv.starts_with(":systemdict:"sv)) {
            return std::pair{trx->m_systemdict, sv.data() + ":systemdict:"sv.size()};
        } else if (sv.starts_with(":userdict:"sv)) {
            return std::pair{trx->m_userdict, sv.data() + ":userdict:"sv.size()};
        } else if (sv.starts_with(":errordict:"sv)) {
            return std::pair{trx->m_errordict, sv.data() + ":errordict:"sv.size()};
        } else if (sv.starts_with(":handlersdict:"sv)) {
            return std::pair{trx->m_handlersdict, sv.data() + ":handlersdict:"sv.size()};
        } else if (sv.starts_with(":modules:"sv)) {
            return std::pair{trx->offset_to_ptr<Dict>(trx->m_modules_dict_offset), sv.data() + ":modules:"sv.size()};
        } else {
            // Fallback: try looking up the first path segment as a dict entry in systemdict.
            // This makes any systemdict sub-dict accessible as a path root, e.g.
            // //:records:type, //:pipeline:map, //:actors:spawn.
            auto colon_pos = sv.find(':', 1);
            if (colon_pos != std::string_view::npos) {
                auto root_name = sv.substr(1, colon_pos - 1);
                auto [found, name_obj] = find(trx, root_name);
                if (found) {
                    auto val_ptr = trx->m_systemdict->get(trx, &name_obj);
                    if ((val_ptr != nullptr) && val_ptr->is_dict()) {
                        return std::pair{val_ptr->dict_value(trx), sv.data() + colon_pos + 1};
                    }
                }
            }
            return std::pair{nullptr, nullptr};
        }
    }

    // Walks intermediate ':'-separated path segments through nested dicts.
    // Returns the final Dict if all segments resolve, or nullptr on any failure.
    [[nodiscard]] static Dict *walk_dict_path(Trix *trx, Dict *dict, const char *seg_start, const char *name_start) {
        while (seg_start < name_start) {
            const char *seg_end = seg_start;
            // Unbounded scan with no sentinel: the caller (dict_path_search) sets
            // name_start to the byte just past the path's last ':', so every
            // segment in [seg_start, name_start) is guaranteed ':'-terminated.
            while (*seg_end != ':') {
                ++seg_end;
            }

            auto seg_length = static_cast<length_t>(seg_end - seg_start);
            auto [path_name_found, path_name_obj] = find(trx, std::string_view{seg_start, seg_length});
            if (path_name_found) {
                auto value_ptr = dict->get(trx, &path_name_obj);
                if ((value_ptr != nullptr) && value_ptr->is_dict()) {
                    dict = value_ptr->dict_value(trx);
                    seg_start = (seg_end + 1);
                } else {
                    return nullptr;
                }
            } else {
                return nullptr;
            }
        }
        return dict;
    }

    // Hierarchical path-based name lookup using ':' as separator.
    // A name starting with ':' is treated as a path rather than a simple name.
    // Path format: :rootdict:segment0:...:leafname
    //   The root dict must be one of: systemdict, userdict, errordict, handlersdict.
    //   Each intermediate segment is looked up as a Name in the preceding Dict.
    //   Returns nullptr if the path does not resolve.
    // Examples:
    //   :systemdict:numbers:real:pi
    //   :userdict:user_symbol
    //   :errordict:command
    //   :handlersdict:dict-full
    //   :status:vm-used
    [[nodiscard]] static const Object *dict_path_search(Trix *trx, std::string_view sv) {
        auto data = sv.data();
        auto length = sv_length(sv);

        assert(length > 0);

        if (*data == ':') {
            // //:status:key -- on-demand VM introspection (no Dict; computed per key)
            {
                using namespace std::literals::string_view_literals;
                if (sv.starts_with(":status:"sv)) {
                    return trx->status_lookup(sv.substr(8));
                }
            }

            auto [dict, path] = check_systemdicts(trx, sv);
            if (dict != nullptr) {
                const char *end = (data + length);

                // single forward scan from path to find the last ':', which precedes the name
                const char *last_sep = (path - 1);
                for (const char *p = path; p < end; ++p) {
                    if (*p == ':') {
                        last_sep = p;
                    }
                }

                const char *name_string = (last_sep + 1);
                auto name_length = static_cast<length_t>(end - name_string);

                auto [name_found, name_obj] = find(trx, std::string_view{name_string, name_length});
                if (name_found) {
                    dict = walk_dict_path(trx, dict, path, name_string);
                    if (dict != nullptr) {
                        return dict->get(trx, &name_obj);
                    }
                }
            }
        }
        return nullptr;
    }

    // m_binding is the single-coroutine fast-path cache -- an offset into
    // a DictEntry::m_value slot, or nulloffset.  Only consulted when
    // Trix::m_live_coroutine_count == 0 (main is the only live coroutine).
    // Any spawn tips the count and Name::name_search flips to the
    // per-coroutine table in binding_table.inl.  Correctness is enforced
    // by flushing every m_binding to nulloffset at the 0 -> 1 transition;
    // no per-name save-level tracking.
    vm_offset_t m_next;     // next Name in bucket chain
    vm_offset_t m_binding;  // single-coroutine binding cache (nulloffset = miss)
    hash_t m_hash;          // hash value of name
    length_t m_length;      // length of name string
    vm_t m_data[1];         // first character of name string

    [[nodiscard]] vm_t *data() { return m_data; }
    [[nodiscard]] const vm_t *data() const { return m_data; }
};
static_assert(sizeof(Name) == 16);

[[nodiscard]] static bool name_bucket_count_valid(name_bucket_count_t count) {
    return std::ranges::contains(sm_name_bucket_counts, count);
}

// Resolve AutoNameBucketCount: one bucket per NameBucketVmDivisor bytes of
// VM, snapped up to the prime table.  Scan-time interning walks bucket
// chains, and the array never grows -- name-heavy programs measured 31%
// faster scans at 32771 buckets.
// 256K -> 521, 1M (default) -> 2053, 4M -> 8209, 16M -> 32771, >=32M ->
// 65537.  Cost is 4 B/slot: 0.8% of VM at every scale point.
[[nodiscard]] static name_bucket_count_t scaled_name_bucket_count(vm_size_t vm_size) {
    const auto target = vm_size / NameBucketVmDivisor;
    for (auto count : sm_name_bucket_counts) {
        if (count >= target) {
            return count;
        }
    }
    return sm_name_bucket_counts[std::size(sm_name_bucket_counts) - 1];
}

// Well-known name accessors (table populated in init.inl, enum/strings in enums.inl/dispatch.inl)
[[nodiscard]] vm_offset_t wellknown_offset(WellKnownName name) const {
    return m_wellknown_offsets[+name];
}

[[nodiscard]] Object wellknown_name(WellKnownName name, Object::attrib_t attrib = Object::LiteralAttrib) const {
    // Prebaked at init / thaw by populate_wellknown_cache() with LiteralAttrib.
    // Callers requesting ExecutableAttrib flip the X bit via set_attrib(); the cache
    // itself holds one Object per name.  Avoids the per-call offset_to_ptr<Name>
    // bounds check on the hot infix/field-access emission paths.
    auto obj = m_wellknown_cache[+name];
    obj.set_attrib(attrib);
    return obj;
}

// Populate m_wellknown_cache from m_wellknown_offsets.  Called once after init
// populates the offset table, and again after thaw rebinds it.
void populate_wellknown_cache() {
    for (name_index_t i = 0; i < WELLKNOWN_COUNT; ++i) {
        auto offset = m_wellknown_offsets[i];
        auto length = offset_to_ptr<Name>(offset)->length();
        m_wellknown_cache[i] = Object::make_name(offset, length, Object::LiteralAttrib);
    }
}
