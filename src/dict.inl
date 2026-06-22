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

//===--- Dict: Hash-Table Dictionary ---===//
//
// Implements the dictionary (associative array) type, the primary key-value
// data structure in Trix.  Also serves as the underlying implementation for
// Sets (via SetFlag).  Based on:
//
//   PostScript dictionaries: the foundational data structure for the language.
//   Variable binding (`/name value def`), operator dispatch, and scope
//   management all use dictionaries.  The dictionary stack provides lexical
//   scoping.
//
//   Separate chaining hash table: each bucket holds a linked list of entries.
//   Standard approach from Knuth, "The Art of Computer Programming", Vol. 3,
//   Section 6.4 (Hashing).
//
// --- Core concepts for maintainers ---
//
// STRUCTURE
//   A Dict is allocated as a single contiguous block on the VM heap:
//     [Dict header (16 bytes)]
//     [bucket array: vm_offset_t[bucket_count]]
//     [entry pool: DictEntry[maxlength] or SetEntry[maxlength]]
//
//   The header contains length, maxlength, bucket_count, access mode,
//   pool free list head, and save level.  Buckets are vm_offset_t heads
//   of collision chains.  The entry pool is pre-allocated; entries are
//   taken from the pool on put and returned on undef.
//
// ENTRY TYPES
//   DictEntry (20 bytes): m_next + key Object + value Object.
//   SetEntry  (12 bytes): m_next + key Object (no value).
//   The SetFlag bit in m_access distinguishes which layout is in use.
//   All set-specific methods assert is_set_data() as a guard.
//
// ACCESS MODES
//   ReadOnly:          immutable (systemdict, numbersdict, etc.)
//   ReadWriteFixed:    mutable, fixed capacity (userdict, handlersdict)
//   ReadWriteDynamic:  mutable, grows by doubling when full (vm_alloc)
//
// HEAP LAYOUT
//
//   Dict on VM heap (contiguous allocation):
//   +--------------------------------------+
//   | Dict header (16 bytes, offsetof m_buckets = 16) |
//   |   m_pool          (vm_offset_t)      |
//   |   m_length        (length_t)         |
//   |   m_maxlength     (length_t)         |
//   |   m_bucket_count  (bucket_count_t)   |
//   |   m_access        (access_t)         |
//   |   m_dict_save_level (save_level_t)   |
//   |   m_next_in_visit (vm_offset_t)      |
//   +--------------------------------------+
//   | Bucket array                         |
//   |   [0] vm_offset_t -> chain head      |
//   |   [1] vm_offset_t -> chain head      |
//   |   ...                                |
//   |   [N-1] vm_offset_t -> chain head    |
//   +--------------------------------------+
//   | Entry pool (DictEntry or SetEntry)   |
//   |   [0] DictEntry {next, key, value}   |  <- 20 bytes each
//   |   [1] DictEntry {next, key, value}   |     (or 12 bytes for SetEntry)
//   |   ...                                |
//   |   [maxlength-1]                      |
//   +--------------------------------------+
//
//   DictEntry (20 bytes):         SetEntry (12 bytes):
//   +---+---+---+---+             +---+---+---+---+
//   | m_next (4)    |             | m_next (4)    |
//   +---+---+---+---+---+---+    +---+---+---+---+---+---+
//   | m_key   (8 = Object)  |    | m_key   (8 = Object)  |
//   +---+---+---+---+---+---+    +---+---+---+---+---+---+
//   | m_value (8 = Object)  |
//   +---+---+---+---+---+---+
//
// POOL-BASED ALLOCATION
//   Entries are never individually allocated or freed.  The pool is a
//   free list threaded through DictEntry/SetEntry.m_next.  put() takes
//   from the pool; undef() returns to the pool.  This avoids per-entry
//   heap allocation and gives O(1) entry management.
//
//   Dynamic dicts grow by allocating a new pool block (doubling capacity)
//   appended to the heap.  Bucket count stays fixed, so load factor
//   increases with expansion -- intended for dicts that grow infrequently.
//
// SAVE/RESTORE JOURNALING
//   Dict mutations are journaled for save/restore (transaction rollback):
//   - Dict header: captured on first mutation after save()
//   - DictEntry/SetEntry: captured before value update or key insertion
//   - Bucket chain head: captured before prepending a new entry
//   - Predecessor m_next: captured before unlinking an entry
//   On restore, the journal replays in reverse, restoring the dict to
//   its state at the save point.
//
// NAME BINDING CACHE
//   Dict entries for Name keys support a one-word binding cache:
//   Name::m_binding stores a vm_offset_t pointing directly into
//   DictEntry::m_value.  This gives O(1) name lookup (bypassing hash
//   computation and bucket traversal) when the cache is valid.  The
//   cache is invalidated by save/restore, dict stack changes, and
//   explicit clear_name_bindings().  DictEntries are never relocated
//   (no rehashing), so cached offsets remain valid for the entry's
//   lifetime.
//
// DICT STACK
//   The dictionary stack (m_dict_base..m_dict_ptr) holds the active
//   dictionaries for name lookup.  Bottom-to-top: systemdict (built-in
//   operators), protocoldict (protocol dispatch procs), userdict (user
//   definitions).  Additional dicts are pushed by
//   `begin`/`end`, module scope, try-catch handlers, etc.  Name lookup
//   walks from top to bottom, returning the first match.
//
// BUILT-IN DICTIONARIES (created by Dict::init)
//   systemdict:   all 829 operators + system streams/variables (ReadOnly)
//   protocoldict: protocol dispatch procs (ReadWriteDynamic; def-protocol fills)
//   userdict:     user definitions (ReadWriteFixed, configurable capacity)
//   errordict:    error state: /command, /error-name, /error-message,
//                 /error-data, /handlersdict, /ostack, /dstack, /estack (ReadOnly)
//   handlersdict: error handlers keyed by error name (ReadWriteFixed)
//   numbersdict:  numeric type metadata (ReadOnly)
//
class Dict {
public:
    struct InitConfig {
        length_t m_userdict_maxlength;
        const Operator *m_useroperators;
    };

    // m_access bit layout (Dict-local; independent of Object::m_aat layout):
    //  7 6 5 4 3 2 1 0
    // +-+-+-+-+-+-+-+-+
    // |-|-|-|N|F|S|W|D|
    // +-+-+-+-+-+-+-+-+
    //       | | | | |
    //       | | | | +-- D: 0 = ReadWriteFixed, 1 = ReadWriteDynamic
    //       | | | +---- W: 0 = ReadOnly, 1 = ReadWrite
    //       | | +------ S: 0 = Dict, 1 = Set
    //       | +-------- F: 0 = User dict, 1 = Frame dict (|locals|#N scope)
    //       +---------- N: 1 = guaranteed no global refs (GC walker short-circuits)
    //
    // Flags live in the low 5 bits so this header byte has no direct bit-position
    // coupling with Object::m_aat.  Dict::access() rebases bit 1 into Object's bit 6
    // so callers that compare against Object::ReadOnlyAccess/ReadWriteAccess still work.
    static constexpr uint8_t ReadOnlyAccess{0x00};
    static constexpr uint8_t ReadWriteAccess{0x02};
    static constexpr uint8_t AccessMask{0x02};
    static constexpr uint8_t DynamicFlag{0x01};
    static constexpr uint8_t SetFlag{0x04};
    static constexpr uint8_t IsFrame{0x08};
    // NoGlobalRefs: set on dicts whose contents are guaranteed local-VM only,
    // so gc_walk_contents short-circuits.  Set on systemdict + every readonly
    // sub-dict created during init_systemdict (numbersdict, errordict, the
    // convenience dicts, etc.) -- all populated when m_curr_alloc_global=false
    // and locked read-only at the user-facing put-op level, so no path can
    // ever introduce a global ref.  Saves ~700 entry-walks per GC pass.
    static constexpr uint8_t NoGlobalRefs{0x10};

    [[nodiscard]] bool is_set_data() const { return ((m_access & SetFlag) != 0); }

    [[nodiscard]] bool is_frame() const { return ((m_access & IsFrame) != 0); }

    // True if this dict's contents are guaranteed local-VM only (no global
    // refs, transitively).  GC walker short-circuits on these.  See the
    // NoGlobalRefs flag comment above for the invariant.
    [[nodiscard]] bool has_no_global_refs() const { return ((m_access & NoGlobalRefs) != 0); }
    void set_no_global_refs_no_save() { m_access |= NoGlobalRefs; }

    // Tag a freshly-created dict as a frame dict (|locals|#N scope).  Set
    // without journaling: the dict is not yet observable anywhere else, so
    // there is no prior state to preserve.  Do not call on a dict that has
    // escaped to user code.
    void set_frame_no_save() { m_access |= IsFrame; }

    [[nodiscard]] Object::access_t access() const {
        return (((m_access & ReadWriteAccess) != 0) ? Object::ReadWriteAccess : Object::ReadOnlyAccess);
    }

    [[nodiscard]] bool has_write_access() const { return ((m_access & ReadWriteAccess) != 0); }

    void set_readonly_access(Trix *trx) {
        save(trx);

        // clear all access bits via AccessMask, leaving ReadOnly (0x00)
        m_access &= ~AccessMask;
    }

    // Set readonly without journaling.  For freshly allocated dicts that
    // have no prior state to preserve (e.g. introspection result dicts).
    void set_readonly_access_no_save() { m_access &= ~AccessMask; }

    [[nodiscard]] bool is_dynamic() const { return ((m_access & DynamicFlag) != 0); }

    // True iff this Dict / Set header lives in the global VM region.
    // Used by the R6 pointer-hygiene check in put / set_put paths to
    // gate Save::reject_local_into_global on whether the destination
    // container is restore-immune (and therefore must not reference
    // restore-fragile local-VM storage).
    [[nodiscard]] bool is_global(const Trix *trx) const { return (reinterpret_cast<const vm_t *>(this) >= trx->m_vm_global); }

    [[nodiscard]] length_t length() const { return m_length; }

    [[nodiscard]] length_t maxlength() const { return m_maxlength; }

    [[nodiscard]] length_t capacity() const { return static_cast<length_t>(m_maxlength - m_length); }

    // GC visit-list link accessors.  Used by Trix::gc_visit_push /
    // gc_visit_pop only; mutations are not journaled (the field is
    // transient state, always nulloffset between GC passes).  Public
    // because the Trix GC helpers live outside the Dict class but
    // need direct access to this field.
    [[nodiscard]] vm_offset_t next_in_visit() const { return m_next_in_visit; }
    void set_next_in_visit_no_save(vm_offset_t off) { m_next_in_visit = off; }
private:
    // Dispatches to make_real (single arg) or make_double (trx + arg) by type.
    // Used by init_numbers_float_type and init_numbers_integral_type to create
    // a typed Object from a numeric value without duplicating factory dispatch.
    template<typename T>
    static Object make_numeric_value(Trix *trx, T v) {
        if constexpr (std::is_same_v<T, vm_t>) {
            static_cast<void>(trx);
            return Object::make_byte(v);
        } else if constexpr (std::is_same_v<T, integer_t>) {
            static_cast<void>(trx);
            return Object::make_integer(v);
        } else if constexpr (std::is_same_v<T, uinteger_t>) {
            static_cast<void>(trx);
            return Object::make_uinteger(v);
        } else if constexpr (std::is_same_v<T, long_t>) {
            return Object::make_long(trx, v);
        } else if constexpr (std::is_same_v<T, ulong_t>) {
            return Object::make_ulong(trx, v);
        } else if constexpr (std::is_same_v<T, int128_t>) {
            return Object::make_int128(trx, v);
        } else if constexpr (std::is_same_v<T, uint128_t>) {
            return Object::make_uint128(trx, v);
        } else if constexpr (std::is_same_v<T, real_t>) {
            static_cast<void>(trx);
            return Object::make_real(v);
        } else {
            static_assert(std::is_same_v<T, double_t>);
            return Object::make_double(trx, v);
        }
    }

    // Populates a 3-entry integral numeric-type sub-dict and registers it in numbersdict.
    // Instantiated for vm_t, integer_t, uinteger_t, long_t, ulong_t, int128_t, uint128_t.
    template<typename T>
    static void init_numbers_integral_type(Trix *trx, Dict *numbersdict, Object::Type type) {
        using limits = std::numeric_limits<T>;

        auto [dict, offset] = create_dict(trx, 3, Object::DictMode::ReadOnly);
        numbersdict->put(trx, trx->type_name(type), Object::make_dict(offset));
        dict->put(trx, SystemName::IsSigned, Object::make_boolean(limits::is_signed));
        dict->put(trx, SystemName::Min, make_numeric_value(trx, limits::min()));
        dict->put(trx, SystemName::Max, make_numeric_value(trx, limits::max()));
    }

    // Populates a 33-entry float numeric-type sub-dict and registers it in numbersdict.
    // Instantiated for real_t (Object::Type::Real) and double_t (Object::Type::Double).
    template<typename T>
    static void init_numbers_float_type(Trix *trx, Dict *numbersdict, Object::Type type) {
        using namespace std::numbers;
        using limits = std::numeric_limits<T>;

        auto [dict, offset] = create_dict(trx, 33, Object::DictMode::ReadOnly);
        numbersdict->put(trx, trx->type_name(type), Object::make_dict(offset));
        dict->put(trx, SystemName::IsSigned, Object::make_boolean(limits::is_signed));
        dict->put(trx, SystemName::Min, make_numeric_value(trx, limits::min()));
        dict->put(trx, SystemName::Max, make_numeric_value(trx, limits::max()));
        dict->put(trx, SystemName::IsIEC559, Object::make_boolean(limits::is_iec559));
        dict->put(trx, SystemName::TinynessBefore, Object::make_boolean(limits::tinyness_before));
        dict->put(trx, SystemName::Digits, Object::make_integer(limits::digits));
        dict->put(trx, SystemName::Digits10, Object::make_integer(limits::digits10));
        dict->put(trx, SystemName::MaxDigits10, Object::make_integer(limits::max_digits10));
        dict->put(trx, SystemName::Radix, Object::make_integer(limits::radix));
        dict->put(trx, SystemName::MinExponent, Object::make_integer(limits::min_exponent));
        dict->put(trx, SystemName::MinExponent10, Object::make_integer(limits::min_exponent10));
        dict->put(trx, SystemName::MaxExponent, Object::make_integer(limits::max_exponent));
        dict->put(trx, SystemName::MaxExponent10, Object::make_integer(limits::max_exponent10));
        dict->put(trx, SystemName::Lowest, make_numeric_value(trx, limits::lowest()));
        dict->put(trx, SystemName::Epsilon, make_numeric_value(trx, limits::epsilon()));
        dict->put(trx, SystemName::Infinity, make_numeric_value(trx, limits::infinity()));
        dict->put(trx, SystemName::RoundError, make_numeric_value(trx, limits::round_error()));
        dict->put(trx, SystemName::QuietNaN, make_numeric_value(trx, limits::quiet_NaN()));
        dict->put(trx, SystemName::SignalingNaN, make_numeric_value(trx, limits::signaling_NaN()));
        dict->put(trx, SystemName::DenormMin, make_numeric_value(trx, limits::denorm_min()));
        dict->put(trx, SystemName::E, make_numeric_value(trx, e_v<T>));
        dict->put(trx, SystemName::Log2e, make_numeric_value(trx, log2e_v<T>));
        dict->put(trx, SystemName::Log10e, make_numeric_value(trx, log10e_v<T>));
        dict->put(trx, SystemName::Pi, make_numeric_value(trx, pi_v<T>));
        dict->put(trx, SystemName::InvPi, make_numeric_value(trx, inv_pi_v<T>));
        dict->put(trx, SystemName::InvSqrtPi, make_numeric_value(trx, inv_sqrtpi_v<T>));
        dict->put(trx, SystemName::Ln2, make_numeric_value(trx, ln2_v<T>));
        dict->put(trx, SystemName::Ln10, make_numeric_value(trx, ln10_v<T>));
        dict->put(trx, SystemName::Sqrt2, make_numeric_value(trx, sqrt2_v<T>));
        dict->put(trx, SystemName::Sqrt3, make_numeric_value(trx, sqrt3_v<T>));
        dict->put(trx, SystemName::InvSqrt3, make_numeric_value(trx, inv_sqrt3_v<T>));
        dict->put(trx, SystemName::EGamma, make_numeric_value(trx, egamma_v<T>));
        dict->put(trx, SystemName::Phi, make_numeric_value(trx, phi_v<T>));
    }

    // Builds the systemdict::numbers sub-tree and registers it under SystemName::Numbers.
    static void init_numbers(Trix *trx, Dict *systemdict) {
        // classification, environment,
        // byte-type, integer-type, uinteger-type, long-type, ulong-type,
        // int128-type, uint128-type, real-type, double-type
        auto [numbersdict, numbersdict_offset] = create_dict(trx, 11, Object::DictMode::ReadOnly);
        systemdict->put(trx, SystemName::Numbers, Object::make_dict(numbersdict_offset));

        // numbers::classification (fp-normal, fp-subnormal, fp-zero, fp-infinite, fp-nan, fp-other)
        {
            auto [classificationdict, classificationdict_offset] = create_dict(trx, 6, Object::DictMode::ReadOnly);
            numbersdict->put(trx, SystemName::Classification, Object::make_dict(classificationdict_offset));
            classificationdict->put(trx, SystemName::FP_Normal, Name::make_number_classification(trx, FP_NORMAL));
            classificationdict->put(trx, SystemName::FP_SubNormal, Name::make_number_classification(trx, FP_SUBNORMAL));
            classificationdict->put(trx, SystemName::FP_Zero, Name::make_number_classification(trx, FP_ZERO));
            classificationdict->put(trx, SystemName::FP_Infinite, Name::make_number_classification(trx, FP_INFINITE));
            classificationdict->put(trx, SystemName::FP_NaN, Name::make_number_classification(trx, FP_NAN));
            classificationdict->put(trx, SystemName::FP_Other, Name::make_number_classification(trx, -1));
        }

        // numbers::environment (fe-div-by-zero, fe-inexact, fe-invalid, fe-overflow, fe-underflow,
        //                       fe-all-except, fe-downward, fe-to-nearest, fe-toward-zero, fe-upward,
        //                       fe-other, fe-default)
        {
            auto [environmentdict, environmentdict_offset] = create_dict(trx, 12, Object::DictMode::ReadOnly);
            numbersdict->put(trx, SystemName::Environment, Object::make_dict(environmentdict_offset));
            static_assert(sizeof(uinteger_t) >= sizeof(FE_ALL_EXCEPT));
            environmentdict->put(trx, SystemName::FE_DivByZero, Object::make_uinteger(FE_DIVBYZERO));
            environmentdict->put(trx, SystemName::FE_Inexact, Object::make_uinteger(FE_INEXACT));
            environmentdict->put(trx, SystemName::FE_Invalid, Object::make_uinteger(FE_INVALID));
            environmentdict->put(trx, SystemName::FE_Overflow, Object::make_uinteger(FE_OVERFLOW));
            environmentdict->put(trx, SystemName::FE_Underflow, Object::make_uinteger(FE_UNDERFLOW));
            environmentdict->put(trx, SystemName::FE_All_Except, Object::make_uinteger(FE_ALL_EXCEPT));
            environmentdict->put(trx, SystemName::FE_Downward, Name::make_number_rounding(trx, FE_DOWNWARD));
            environmentdict->put(trx, SystemName::FE_ToNearest, Name::make_number_rounding(trx, FE_TONEAREST));
            environmentdict->put(trx, SystemName::FE_TowardZero, Name::make_number_rounding(trx, FE_TOWARDZERO));
            environmentdict->put(trx, SystemName::FE_Upward, Name::make_number_rounding(trx, FE_UPWARD));
            environmentdict->put(trx, SystemName::FE_Other, Name::make_number_rounding(trx, -1));
            environmentdict->put(trx, SystemName::FE_Default, Name::make_number_rounding(trx, std::fegetround()));
        }

        // numbers::*-type: 3 entries each (is-signed, min, max)
        init_numbers_integral_type<vm_t>(trx, numbersdict, Object::Type::Byte);
        init_numbers_integral_type<integer_t>(trx, numbersdict, Object::Type::Integer);
        init_numbers_integral_type<uinteger_t>(trx, numbersdict, Object::Type::UInteger);
        init_numbers_integral_type<long_t>(trx, numbersdict, Object::Type::Long);
        init_numbers_integral_type<ulong_t>(trx, numbersdict, Object::Type::ULong);
        init_numbers_integral_type<int128_t>(trx, numbersdict, Object::Type::Int128);
        init_numbers_integral_type<uint128_t>(trx, numbersdict, Object::Type::UInt128);

        // numbers::real-type and double-type: 33 entries each (numeric limits + math constants)
        init_numbers_float_type<real_t>(trx, numbersdict, Object::Type::Real);
        init_numbers_float_type<double_t>(trx, numbersdict, Object::Type::Double);
    }
public:
    static void init_systemdict(Trix *trx) {
        auto dict_count = 20;      // systemdict, protocoldict, userdict, errordict, numbers + 15 convenience dicts
        auto var_count = 9;        // false, true, null, inf, nan, inf#r, nan#r, inf#d, nan#d
        auto stdstream_count = 4;  // stdin, stdedit, stdout, stderr
        auto entry_count = static_cast<length_t>(SYSTEMNAME_STD_OP_COUNT + dict_count + var_count + stdstream_count);
        auto [systemdict, systemdict_offset] = create_dict(trx, entry_count, Object::DictMode::ReadOnly);
        systemdict->put(trx, SystemName::SystemDict, Object::make_dict(systemdict_offset));
        trx->m_systemdict = systemdict;

        // built-in Operators
        for (auto i = +SystemName::FIRST_STD_OP; i <= +SystemName::LAST_STD_OP; ++i) {
            auto sys_name = static_cast<SystemName>(i);
            auto name_obj = Name::make_system(trx, sys_name);
            auto op_obj = Object::make_operator(sys_name);
            systemdict->put(trx, name_obj, op_obj);
        }

        // /false
        systemdict->put(trx, SystemName::False, Object::make_boolean(false));

        // /true
        systemdict->put(trx, SystemName::True, Object::make_boolean(true));

        // /null - the null object
        systemdict->put(trx, SystemName::Null, Object::make_null());

        // Real IEEE 754 special values: bare /inf and /nan match the
        // language's default-is-bare convention for Real (cf. 3.14 vs
        // 3.14d).  /inf#r and /nan#r are explicit-qualifier aliases of
        // the bare forms, parallel to the #X suffix family on literals.
        auto real_inf = Object::make_real(std::numeric_limits<real_t>::infinity());
        auto real_nan = Object::make_real(std::numeric_limits<real_t>::quiet_NaN());
        systemdict->put(trx, SystemName::Inf, real_inf);
        systemdict->put(trx, SystemName::NaNR, real_nan);
        systemdict->put(trx, SystemName::InfRSuffix, real_inf);
        systemdict->put(trx, SystemName::NaNRSuffix, real_nan);

        // /inf#d, /nan#d -- Double IEEE 754 special values (no bare form;
        // Double is non-default and always carries the explicit suffix).
        systemdict->put(trx, SystemName::InfD, Object::make_double(trx, std::numeric_limits<double_t>::infinity()));
        systemdict->put(trx, SystemName::NaND, Object::make_double(trx, std::numeric_limits<double_t>::quiet_NaN()));

        // /stdin, /stdedit, /stdout, /stderr
        auto init_stream = [trx, systemdict](Stream *stream, SystemName name) {
            auto stream_obj = (stream != nullptr) ? Object::make_stream(trx, stream) : Object::make_invalid_stream();
            systemdict->put(trx, name, stream_obj);
        };
        init_stream(trx->m_stdin, SystemName::StdIn);
        init_stream(trx->m_stdedit, SystemName::StdEdit);
        init_stream(trx->m_stdout, SystemName::StdOut);
        init_stream(trx->m_stderr, SystemName::StdErr);

        init_numbers(trx, systemdict);
        init_convenience_dicts(trx, systemdict);
    }

    // Creates a ReadOnly convenience dict with short-name aliases for prefixed operators.
    // Each entry maps an unprefixed short name to the corresponding system operator Object.
    static void make_convenience_dict(Trix *trx,
                                      Dict *systemdict,
                                      const char *dict_name,
                                      std::initializer_list<std::pair<const char *, SystemName>> entries) {
        auto [dict, dict_offset] = create_dict(trx, static_cast<length_t>(entries.size()), Object::DictMode::ReadOnly);
        for (auto &[short_name, sys] : entries) {
            dict->put(trx, Name::make(trx, short_name), Object::make_operator(sys));
        }
        systemdict->put(trx, Name::make(trx, dict_name), Object::make_dict(dict_offset));
    }

    static void init_pipeline_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "pipeline",
                              {
                                      {     "buffer",     S::PipeBuffer},
                                      {        "put",        S::PipePut},
                                      {        "get",        S::PipeGet},
                                      {      "close",      S::PipeClose},
                                      {"error-close", S::PipeErrorClose},
                                      {        "map",        S::PipeMap},
                                      {     "filter",     S::PipeFilter},
                                      {   "flat-map",    S::PipeFlatMap},
                                      {      "batch",      S::PipeBatch},
                                      {        "tap",        S::PipeTap},
                                      {        "run",        S::PipeRun},
                                      {    "collect",    S::PipeCollect},
                                      {     "reduce",     S::PipeReduce},
                                      {       "take",       S::PipeTake},
                                      {       "drop",       S::PipeDrop},
                                      {       "scan",       S::PipeScan},
                                      {   "distinct",   S::PipeDistinct},
                                      {     "window",     S::PipeWindow},
                                      {      "merge",      S::PipeMerge},
                                      {        "zip",        S::PipeZip},
                                      {     "status",     S::PipeStatus},
                                      {       "into",       S::PipeInto}
        });
    }

    static void init_actors_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "actors",
                              {
                                      {             "spawn",            S::ActorSpawn},
                                      {    "spawn-capacity",    S::ActorSpawnCapacity},
                                      {              "self",             S::ActorSelf},
                                      {              "send",             S::ActorSend},
                                      {          "try-send",          S::ActorTrySend},
                                      {              "recv",             S::ActorRecv},
                                      {      "recv-timeout",      S::ActorRecvTimeout},
                                      {        "recv-match",        S::ActorRecvMatch},
                                      {"recv-match-timeout", S::ActorRecvMatchTimeout},
                                      {     "mailbox-count",     S::ActorMailboxCount},
                                      {  "mailbox-capacity",  S::ActorMailboxCapacity},
                                      {    "mailbox-empty?", S::ActorMailboxEmptyPred},
                                      {         "broadcast",        S::ActorBroadcast},
                                      {             "flush",            S::ActorFlush},
                                      {              "name",             S::ActorName},
                                      {          "set-name",          S::ActorSetName},
                                      {            "status",           S::ActorStatus},
                                      {              "exit",             S::ActorExit},
                                      {         "trap-exit",         S::ActorTrapExit}
        });
    }

    static void init_coroutine_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "coroutine",
                              {
                                      {    "launch",             S::Launch},
                                      {    "resume",    S::CoroutineResume},
                                      {   "suspend",   S::CoroutineSuspend},
                                      {   "release",   S::CoroutineRelease},
                                      {      "self",      S::CoroutineSelf},
                                      {        "id",        S::CoroutineId},
                                      {     "by-id",      S::CoroutineById},
                                      {      "kill",               S::Kill},
                                      {  "kill-all",   S::CoroutineKillAll},
                                      {       "die",                S::Die},
                                      {      "join",      S::CoroutineJoin},
                                      {     "await",     S::CoroutineAwait},
                                      {  "wait-all",   S::CoroutineWaitAll},
                                      {"last-error", S::CoroutineLastError},
                                      {    "status",    S::CoroutineStatus},
                                      {  "priority",  S::CoroutinePriority},
                                      {   "quantum",   S::CoroutineQuantum},
                                      {     "sleep",              S::Sleep}
        });
    }

    static void init_lazy_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "lazy",
                              {
                                      {       "seq?",       S::IsLazySeq},
                                      {     "empty?",      S::LazyEmptyQ},
                                      {       "head",        S::LazyHead},
                                      {       "tail",        S::LazyTail},
                                      {        "nil",         S::LazyNil},
                                      {       "cons",        S::LazyCons},
                                      {        "seq",         S::LazySeq},
                                      {       "from",        S::LazyFrom},
                                      {      "range",       S::LazyRange},
                                      {     "repeat",      S::LazyRepeat},
                                      {   "repeat-n",     S::LazyRepeatN},
                                      {    "iterate",     S::LazyIterate},
                                      {      "cycle",       S::LazyCycle},
                                      {     "unfold",      S::LazyUnfold},
                                      {        "map",         S::LazyMap},
                                      {     "filter",      S::LazyFilter},
                                      { "filter-not",   S::LazyFilterNot},
                                      { "filter-map",   S::LazyFilterMap},
                                      {"map-indexed",  S::LazyMapIndexed},
                                      {       "take",        S::LazyTake},
                                      {       "drop",        S::LazyDrop},
                                      { "take-while",   S::LazyTakeWhile},
                                      { "drop-while",   S::LazyDropWhile},
                                      {     "dedupe",      S::LazyDedupe},
                                      {"intersperse", S::LazyIntersperse},
                                      {    "step-by",      S::LazyStepBy},
                                      {       "scan",        S::LazyScan},
                                      {   "flat-map",     S::LazyFlatMap},
                                      {    "flatten",     S::LazyFlatten},
                                      {        "zip",         S::LazyZip},
                                      {   "zip-with",     S::LazyZipWith},
                                      {      "chain",       S::LazyChain},
                                      { "interleave",  S::LazyInterleave},
                                      {  "enumerate",   S::LazyEnumerate},
                                      {    "chunked",     S::LazyChunked},
                                      {   "windowed",    S::LazyWindowed},
                                      {   "pairwise",    S::LazyPairwise},
                                      {   "to-array",     S::LazyToArray},
                                      {       "fold",        S::LazyFold},
                                      {   "for-each",     S::LazyForEach},
                                      {        "any",         S::LazyAny},
                                      {        "all",         S::LazyAll},
                                      {       "find",        S::LazyFind},
                                      { "find-index",   S::LazyFindIndex},
                                      {        "nth",         S::LazyNth},
                                      {      "count",       S::LazyCount},
                                      {        "sum",         S::LazySum},
                                      {       "into",        S::LazyInto}
        });
    }

    static void init_xf_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "xf",
                              {
                                      {     "map",      S::XfMap},
                                      {  "filter",   S::XfFilter},
                                      {    "take",     S::XfTake},
                                      {    "drop",     S::XfDrop},
                                      {    "scan",     S::XfScan},
                                      { "flatten",  S::XfFlatten},
                                      {"distinct", S::XfDistinct},
                                      { "compose",  S::XfCompose},
                                      {  "reduce",   S::XfReduce}
        });
    }

    static void init_tagged_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "tagged",
                              {
                                      {     "tag",        S::Tag},
                                      {    "tag?",    S::TagPred},
                                      {    "name",    S::TagName},
                                      {   "value",   S::TagValue},
                                      {"value-or", S::TagValueOr},
                                      {   "match",   S::TagMatch},
                                      {  "update",  S::TagUpdate},
                                      {    "bind",    S::TagBind},
                                      {   "untag",      S::Untag},
                                      {      "is",   S::IsTagged}
        });
    }

    static void init_records_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "records",
                              {
                                      {     "type",   S::RecordTypeOp},
                                      {   "schema",   S::RecordSchema},
                                      {   "fields",   S::RecordFields},
                                      {   "values",   S::RecordValues},
                                      {    "known",    S::RecordKnown},
                                      {"map-field", S::RecordMapField},
                                      {      "map",      S::RecordMap},
                                      {    "merge",    S::RecordMerge},
                                      {   "select",   S::RecordSelect},
                                      {  "to-dict",   S::RecordToDict},
                                      {"from-dict", S::RecordFromDict},
                                      {      "zip",      S::RecordZip},
                                      { "group-by",  S::RecordGroupBy},
                                      {   "update",   S::RecordUpdate},
                                      {       "is",       S::IsRecord}
        });
    }

    static void init_cells_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "cells",
                              {
                                      {     "cell",          S::Cell},
                                      { "computed",      S::Computed},
                                      {"validated", S::CellValidated},
                                      {      "get",       S::CellGet},
                                      {      "set",       S::CellSet},
                                      {    "value",     S::CellValue},
                                      {   "update",    S::CellUpdate},
                                      {   "dirty?",     S::CellDirty},
                                      {      "map",       S::CellMap},
                                      {  "combine",   S::CellCombine},
                                      {"transduce", S::CellTransduce},
                                      {     "deps",      S::CellDeps},
                                      {    "rdeps",     S::CellRdeps},
                                      { "watchers",  S::CellWatchers},
                                      {  "dispose",   S::CellDispose},
                                      {     "name",      S::CellName},
                                      { "set-name",   S::CellSetName},
                                      {    "watch",         S::Watch},
                                      {  "unwatch",       S::Unwatch},
                                      {    "batch",         S::Batch},
                                      {       "is",        S::IsCell}
        });
    }

    static void init_genserver_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "genserver",
                              {
                                      {      "server",      S::GenServer},
                                      {        "call",        S::GenCall},
                                      {"call-timeout", S::GenCallTimeout},
                                      {        "cast",        S::GenCast},
                                      {        "stop",        S::GenStop},
                                      {       "reply",       S::GenReply}
        });
    }

    static void init_logic_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "logic",
                              {
                                      {              "var",        S::LogicVar},
                                      {           "is-var",      S::IsLogicVar},
                                      {           "bound?",         S::IsBound},
                                      {            "unify",           S::Unify},
                                      {           "choose",          S::Choice},
                                      {             "fail",            S::Fail},
                                      {              "cut",             S::Cut},
                                      {        "aggregate",       S::Aggregate},
                                      {            "deref",           S::Deref},
                                      {            "guard",           S::Guard},
                                      {         "find-all",         S::FindAll},
                                      {           "find-n",           S::FindN},
                                      {     "choice-count",     S::ChoiceCount},
                                      {"for-each-solution", S::ForEachSolution},
                                      {              "naf",             S::Naf},
                                      {             "once",            S::Once},
                                      {      "unify-match",      S::UnifyMatch}
        });
    }

    static void init_supervision_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "supervision",
                              {
                                      {    "start-child",     S::SupervisorStartChild},
                                      {"terminate-child", S::SupervisorTerminateChild},
                                      {  "restart-child",   S::SupervisorRestartChild},
                                      {           "stop",           S::SupervisorStop},
                                      { "count-children",  S::SupervisorCountChildren},
                                      {      "get-child",       S::SupervisorGetChild},
                                      { "which-children",  S::SupervisorWhichChildren},
                                      {           "spec",           S::SupervisorSpec},
                                      {     "supervisor",               S::Supervisor},
                                      {        "monitor",                  S::Monitor},
                                      {      "demonitor",                S::Demonitor},
                                      {           "link",                     S::Link},
                                      {         "unlink",                   S::Unlink},
                                      {     "spawn-link",                S::SpawnLink},
                                      {  "spawn-monitor",             S::SpawnMonitor}
        });
    }

    static void init_rand_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "rand",
                              {
                                      {            "seed",            S::RandSeed},
                                      {        "uinteger",        S::RandUInteger},
                                      {"bounded-uinteger", S::RandBoundedUInteger},
                                      {           "ulong",           S::RandULong},
                                      {   "bounded-ulong",    S::RandBoundedULong},
                                      {          "int128",          S::RandInt128},
                                      {         "uint128",         S::RandUInt128},
                                      { "bounded-uint128",  S::RandBoundedUInt128},
                                      {            "real",            S::RandReal},
                                      {          "double",          S::RandDouble}
        });
    }

    static void init_regex_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "regex",
                              {
                                      {   "match",   S::RegexMatch},
                                      {  "search",  S::RegexSearch},
                                      {"find-all", S::RegexFindAll},
                                      { "replace", S::RegexReplace},
                                      {   "split",   S::RegexSplit}
        });
    }

    static void init_screen_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "screen",
                              {
                                      {           "cols",          S::ScreenCols},
                                      {           "rows",          S::ScreenRows},
                                      {          "clear",         S::ScreenClear},
                                      {         "resize",        S::ScreenResize},
                                      {       "put-cell",       S::ScreenPutCell},
                                      {     "put-string",     S::ScreenPutString},
                                      {"put-utf8-string", S::ScreenPutUtf8String},
                                      {      "fill-rect",      S::ScreenFillRect},
                                      {           "blit",          S::ScreenBlit},
                                      {         "render",        S::ScreenRender},
                                      {      "render-to",      S::ScreenRenderTo},
                                      {    "park-cursor",    S::ScreenParkCursor},
                                      {       "get-cell",       S::ScreenGetCell},
                                      {             "is",            S::IsScreen}
        });
    }

    static void init_chrono_dict(Trix *trx, Dict *systemdict) {
        using S = SystemName;
        make_convenience_dict(trx,
                              systemdict,
                              "chrono",
                              {
                                      // Clock
                                      {        "epoch-time",           S::EpochTime},
                                      // Instant accessors -- UTC (drops `instant-` prefix).
                                      {              "year",         S::InstantYear},
                                      {             "month",        S::InstantMonth},
                                      {               "day",          S::InstantDay},
                                      {              "hour",         S::InstantHour},
                                      {            "minute",       S::InstantMinute},
                                      {            "second",       S::InstantSecond},
                                      {       "millisecond",  S::InstantMillisecond},
                                      {           "weekday",      S::InstantWeekday},
                                      // Instant accessors -- local zone.
                                      {        "year-local",    S::InstantYearLocal},
                                      {       "month-local",   S::InstantMonthLocal},
                                      {         "day-local",     S::InstantDayLocal},
                                      {        "hour-local",    S::InstantHourLocal},
                                      {      "minute-local",  S::InstantMinuteLocal},
                                      {      "second-local",  S::InstantSecondLocal},
                                      {     "weekday-local", S::InstantWeekdayLocal},
                                      // Date construction + accessors (date- prefix kept where the
                                      // bare name collides with an instant accessor above).
                                      {         "make-date",            S::MakeDate},
                                      {         "date-year",            S::DateYear},
                                      {        "date-month",           S::DateMonth},
                                      {          "date-day",             S::DateDay},
                                      {      "date-weekday",         S::DateWeekday},
                                      {       "day-of-year",           S::DayOfYear},
                                      // Calendar arithmetic (no collision -- drop `date-` prefix).
                                      {          "add-days",         S::DateAddDays},
                                      {        "add-months",       S::DateAddMonths},
                                      {         "add-years",        S::DateAddYears},
                                      {         "diff-days",        S::DateDiffDays},
                                      // Predicates.
                                      {        "leap-year?",          S::IsLeapYear},
                                      {     "days-in-month",         S::DaysInMonth},
                                      // Instant <-> date conversion.
                                      {           "to-date",       S::InstantToDate},
                                      {     "to-date-local",  S::InstantToDateLocal},
                                      {      "make-instant",         S::MakeInstant},
                                      {"make-instant-local",    S::MakeInstantLocal}
        });
    }

    static void init_convenience_dicts(Trix *trx, Dict *systemdict) {
        init_pipeline_dict(trx, systemdict);
        init_actors_dict(trx, systemdict);
        init_coroutine_dict(trx, systemdict);
        init_lazy_dict(trx, systemdict);
        init_tagged_dict(trx, systemdict);
        init_xf_dict(trx, systemdict);
        init_records_dict(trx, systemdict);
        init_cells_dict(trx, systemdict);
        init_genserver_dict(trx, systemdict);
        init_logic_dict(trx, systemdict);
        init_supervision_dict(trx, systemdict);
        init_rand_dict(trx, systemdict);
        init_regex_dict(trx, systemdict);
        init_screen_dict(trx, systemdict);
        init_chrono_dict(trx, systemdict);
    }

    static void init_userdict(Trix *trx, InitConfig cfg) {
        auto [userdict, userdict_offset] = create_dict(trx, cfg.m_userdict_maxlength);
        trx->m_systemdict->put(trx, SystemName::UserDict, Object::make_dict(userdict_offset));
        trx->m_userdict = userdict;

        // user-defined Operators
        if (cfg.m_useroperators != nullptr) {
            operator_index_t index = -1;
            for (auto ptr = cfg.m_useroperators; ptr->m_func != nullptr; ++ptr, --index) {
                userdict->put(trx, Name::make(trx, ptr->m_sv), Object::make_user_operator(index));
            }
        }
        trx->m_useroperators = cfg.m_useroperators;
    }

    static void init_errordict(Trix *trx) {
        auto systemdict = trx->m_systemdict;

        // command, error-name, error-message, error-data, handlersdict, ostack, dstack, estack
        auto [errordict, errordict_offset] = create_dict(trx, 8, Object::DictMode::ReadOnly);
        systemdict->put(trx, Name::make_system(trx, SystemName::ErrorDict), Object::make_dict(errordict_offset));
        trx->m_errordict = errordict;

        {
            // /command /interpreter def
            auto name_obj = Name::make(trx, "command");
            auto val_obj = Object::make_operator(SystemName::Interpreter);
            trx->m_last_operator_ptr = errordict->put(trx, name_obj, val_obj);
        }

        {
            // /error-name /no-error def
            auto name_obj = Name::make(trx, "error-name");
            auto val_obj = trx->error_name(Error::NoError);
            trx->m_last_error_name_ptr = errordict->put(trx, name_obj, val_obj);
        }

        {
            // /error-message () def
            auto name_obj = Name::make(trx, "error-message");
            auto val_obj = Object::make_empty_string(trx, 0, Object::LiteralAttrib, Object::ReadWriteAccess);
            trx->m_last_error_msg_ptr = errordict->put(trx, name_obj, val_obj);
        }

        {
            // /error-data null def
            auto name_obj = Name::make(trx, "error-data");
            auto val_obj = Object::make_null();
            trx->m_last_error_data_ptr = errordict->put(trx, name_obj, val_obj);
        }

        {
            // /handlersdict << /no-error //default-handler /assert-failed //default-handler ... >> def
            auto [handlersdict, handlersdict_offset] = create_dict(trx, ErrorCount);
            errordict->put(trx, Name::make_system(trx, SystemName::HandlersDict), Object::make_dict(handlersdict_offset));
            trx->m_handlersdict = handlersdict;

            auto handler_obj = Object::make_operator(SystemName::DefaultHandler);
            for (int i = 0; i < ErrorCount; ++i) {
                auto name_obj = trx->error_name(static_cast<Error>(i));
                handlersdict->put(trx, name_obj, handler_obj);
            }
        }

        {
            // /ostack /dstack /estack -- empty read-only arrays, updated on error
            auto empty_obj = Object::make_empty_array(trx, 0, Object::LiteralAttrib, Object::ReadOnlyAccess);
            trx->m_ostack_ptr = errordict->put(trx, Name::make(trx, "ostack"), empty_obj);
            trx->m_dstack_ptr = errordict->put(trx, Name::make(trx, "dstack"), empty_obj);
            trx->m_estack_ptr = errordict->put(trx, Name::make(trx, "estack"), empty_obj);
        }
    }

    static void init(Trix *trx, InitConfig cfg) {
        init_systemdict(trx);
        init_protocoldict(trx);
        init_userdict(trx, cfg);
        init_errordict(trx);

        // Dict stack (bottom to top): systemdict, protocoldict, userdict
        // def-protocol binds dispatch procs in protocoldict; user def goes to userdict.
        *++trx->m_dict_ptr = Object::make_dict(trx->ptr_to_offset(trx->m_systemdict));    // bottom
        *++trx->m_dict_ptr = Object::make_dict(trx->ptr_to_offset(trx->m_protocoldict));  // middle
        *++trx->m_dict_ptr = Object::make_dict(trx->ptr_to_offset(trx->m_userdict));      // top

        // set name binding for all three permanent dicts
        trx->m_systemdict->set_name_bindings(trx);
        trx->m_protocoldict->set_name_bindings(trx);
        trx->m_userdict->set_name_bindings(trx);
    }

    static void init_protocoldict(Trix *trx) {
        // Protocol dispatch procs live here, isolated from userdict.
        // Starts empty; def-protocol populates it at runtime.
        auto [protocoldict, protocoldict_offset] =
                create_dict(trx, DefaultInternalDictCapacity, Object::DictMode::ReadWriteDynamic);
        trx->m_systemdict->put(trx, Name::make(trx, "protocoldict"), Object::make_dict(protocoldict_offset));
        trx->m_protocoldict = protocoldict;
    }

    // Deferred save: records the dict header in the save journal at most once per save level.
    // m_dict_save_level tracks when the header was last journaled; if it already matches
    // the current level, the header has not changed since the last save point and no new
    // journal entry is needed.  m_eqdict and m_eqset (the temporary = / {{}}#= workspaces)
    // are excluded: they are not subject to save/restore and must never be rolled back.
    // For eq-storage, per-entry journaling is also skipped (see set_put/set_remove), so
    // rolling back the header alone would desync the pool free-list from the actual entries.
    void save(Trix *trx) {
        auto dict = this;
        if ((m_dict_save_level != trx->m_curr_save_level) && (dict != trx->m_eqdict) && (dict != trx->m_eqset)) {
            Save::save_dict_header(trx, dict, m_bucket_count);
            m_dict_save_level = trx->m_curr_save_level;
        }
    }

    void clear(Trix *trx) { reset_dict(trx, m_maxlength, access()); }

    void reset_dict(Trix *trx, length_t maxlength, Object::access_t obj_access) {
        // =dict is temp storage which in not preserved across save/restore barriers
        auto dict = this;
        auto is_eqdict_or_eqset = (dict == trx->m_eqdict) || (dict == trx->m_eqset);
        save(trx);

        dict_forall_clear(trx, dict);
        m_maxlength = maxlength;
        m_access = (obj_access == Object::ReadWriteAccess) ? ReadWriteAccess : ReadOnlyAccess;
        m_dict_save_level = trx->m_curr_save_level;
        if (m_length != 0) {
            auto null_obj = Object::make_null(trx->m_curr_save_level);
            auto curr_save_level = trx->m_curr_save_level;
            for (dict_bucket_count_t i = 0; i < m_bucket_count; ++i) {
                auto offset = m_buckets[i];
                while (offset != nulloffset) {
                    auto entry = trx->offset_to_ptr<DictEntry>(offset);
                    // check if this DictEntry has been saved at the current save level
                    if (((entry->m_key.save_level() == curr_save_level) && (entry->m_value.save_level() == curr_save_level)) ||
                        is_eqdict_or_eqset) {
                        entry->m_key.maybe_free_extvalue(trx);
                        entry->m_value.maybe_free_extvalue(trx);
                    } else {
                        Save::save_dict_entry(trx, entry);
                    }

                    // clear name binding cache so names don't resolve to this nulled entry
                    if (entry->m_key.is_name()) {
                        entry->m_key.clear_name_binding(trx);
                    }

                    // reset the key/value back to the null Object
                    entry->m_key = null_obj;
                    entry->m_value = null_obj;

                    // return entry to pool
                    auto next = entry->m_next;
                    entry->m_next = m_pool;
                    m_pool = offset;
                    offset = next;
                }
                m_buckets[i] = nulloffset;
            }
            m_length = 0;
        }
    }

    // Wipe all entries without dereferencing m_key / m_value.  Called during Save::restore
    // on m_eqdict / m_eqset after VM heap rollback: their entries may reference Names or
    // ExtValues that have been reclaimed by the heap rewind, so clear_name_binding() and
    // maybe_free_extvalue() would touch dangling VM offsets (corrupting live data at the
    // reused location or double-freeing a freelist).  The entries themselves live in the
    // dict's permanent pool memory (allocated at init), so overwriting their m_next field
    // to requeue them is safe.
    void quiet_flush(Trix *trx) {
        auto null_obj = Object::make_null(trx->m_curr_save_level);
        for (dict_bucket_count_t i = 0; i < m_bucket_count; ++i) {
            auto offset = m_buckets[i];
            while (offset != nulloffset) {
                auto entry = trx->offset_to_ptr<DictEntry>(offset);
                auto next = entry->m_next;
                entry->m_key = null_obj;
                entry->m_value = null_obj;
                entry->m_next = m_pool;
                m_pool = offset;
                offset = next;
            }
            m_buckets[i] = nulloffset;
        }
        m_length = 0;
    }

    // Set variant of quiet_flush -- same rationale as Dict's, but for SetEntry layout.
    void quiet_flush_set(Trix *trx) {
        assert(is_set_data());

        auto null_obj = Object::make_null(trx->m_curr_save_level);
        for (dict_bucket_count_t i = 0; i < m_bucket_count; ++i) {
            auto offset = m_buckets[i];
            while (offset != nulloffset) {
                auto entry = trx->offset_to_ptr<SetEntry>(offset);
                auto next = entry->m_next;
                entry->m_key = null_obj;
                entry->m_next = m_pool;
                m_pool = offset;
                offset = next;
            }
            m_buckets[i] = nulloffset;
        }
        m_length = 0;
    }
private:
    friend class Save;

    // A Dict::DictEntry is pooled and never relocated: a Dict grows its entry pool by appending
    // a new vm_alloc block rather than rehashing.  This invariant is load-bearing for the
    // Name binding cache: Name::m_binding stores a raw vm_offset_t pointing directly into
    // DictEntry::m_value.  If entries were ever relocated (e.g. by a rehash), every cached
    // binding across every Name in the VM would silently point to stale data.
    struct DictEntry {
        vm_offset_t m_next;  // next DictEntry in bucket chain
        Object m_key;        // usually Name, never Null
        Object m_value;      // Object associated with key
    };
    static_assert(sizeof(DictEntry) == 20);

    // SetEntry: compact entry for Set mode -- key only, no value.
    // 12 bytes vs DictEntry's 20 bytes (no value Object).
    struct SetEntry {
        vm_offset_t m_next;  // next SetEntry in bucket chain
        Object m_key;        // any hashable type (same as Dict key, never Null)
    };
    static_assert(sizeof(SetEntry) == 12);

    void init_dict_pool(Trix *trx, length_t count, DictEntry *entries) {
        assert(count != 0);

        m_pool = trx->ptr_to_offset(entries);
        auto null_obj = Object::make_null(trx->m_curr_save_level);
        for (length_t i = 0; i < count; ++i) {
            entries[i].m_next = (i + 1 < count) ? trx->ptr_to_offset(&entries[i + 1]) : nulloffset;
            entries[i].m_key = null_obj;
            entries[i].m_value = null_obj;
        }
    }

    void init_set_pool(Trix *trx, length_t count, SetEntry *entries) {
        assert(count != 0);

        m_pool = trx->ptr_to_offset(entries);
        auto null_obj = Object::make_null(trx->m_curr_save_level);
        for (length_t i = 0; i < count; ++i) {
            entries[i].m_next = (i + 1 < count) ? trx->ptr_to_offset(&entries[i + 1]) : nulloffset;
            entries[i].m_key = null_obj;
        }
    }

    // Bucket count selection for a given maxlength.  All counts are prime to
    // minimize clustering.  Sized so every maxlength in [2^k, 2^(k+1)) maps to
    // a prime in roughly the same range, giving load factor 1-2 at full
    // capacity (LF ~1 at the bottom of each power-of-2 range, ~2 at the top).
    // Indexed directly by std::bit_width(maxlength) -- no loop.
    [[nodiscard]] static dict_bucket_count_t bucket_count_for_capacity(length_t maxlength) {
        static constexpr dict_bucket_count_t table[] = {
                1,      //  k=0   maxlength == 0      (degenerate)
                1,      //  k=1   maxlength == 1      (degenerate)
                3,      //  k=2   maxlength [2, 4)
                5,      //  k=3   maxlength [4, 8)
                11,     //  k=4   maxlength [8, 16)
                17,     //  k=5   maxlength [16, 32)
                37,     //  k=6   maxlength [32, 64)
                67,     //  k=7   maxlength [64, 128)
                131,    //  k=8   maxlength [128, 256)
                257,    //  k=9   maxlength [256, 512)
                521,    //  k=10  maxlength [512, 1024)
                1031,   //  k=11  maxlength [1024, 2048)
                2053,   //  k=12  maxlength [2048, 4096)
                4099,   //  k=13  maxlength [4096, 8192)
                8209,   //  k=14  maxlength [8192, 16384)
                16411,  //  k=15  maxlength [16384, 32768)
                32771,  //  k=16  maxlength [32768, 65536)
        };
        auto k = std::bit_width(static_cast<uint32_t>(maxlength));
        return table[k];
    }

    // Lemire fastmod magic for every bucket count returned by bucket_count_for_capacity()
    // (plus 1 for the zero-capacity degenerate case).  Replaces `hash % m_bucket_count`
    // (~20-cycle division) with `fastmod_u32(hash, bucket_magic_for(count), count)`
    // (~3-4 cycles after a compiler-emitted jump table).  The switch is exhaustive
    // over every value the dict creation path produces; std::unreachable() makes
    // any other value UB.
    [[nodiscard]] static constexpr uint64_t bucket_magic_for(dict_bucket_count_t count) {
        switch (count) {
        case 32771:
            return fastmod_magic_u32(32771);

        case 16411:
            return fastmod_magic_u32(16411);

        case 8209:
            return fastmod_magic_u32(8209);

        case 4099:
            return fastmod_magic_u32(4099);

        case 2053:
            return fastmod_magic_u32(2053);

        case 1031:
            return fastmod_magic_u32(1031);

        case 521:
            return fastmod_magic_u32(521);

        case 257:
            return fastmod_magic_u32(257);

        case 131:
            return fastmod_magic_u32(131);

        case 67:
            return fastmod_magic_u32(67);

        case 37:
            return fastmod_magic_u32(37);

        case 17:
            return fastmod_magic_u32(17);

        case 11:
            return fastmod_magic_u32(11);

        case 5:
            return fastmod_magic_u32(5);

        case 3:
            return fastmod_magic_u32(3);

        case 2:
            return fastmod_magic_u32(2);

        case 1:
            return fastmod_magic_u32(1);

        default:
            std::unreachable();
        }
    }
public:
    enum struct BindingMode { NoBinding, Bind };

    [[nodiscard]] static std::pair<Dict *, vm_offset_t>
    create_dict(Trix *trx, length_t maxlength, Object::DictMode mode = Object::DictMode::ReadWriteFixed) {
        // A dynamic dict created empty must still be growable -- expand doubles the
        // current capacity, so a 0 base never grows.  Clamp up so the entry pool (and
        // the bucket array sized below) start non-empty.  See MinDynamicDictCapacity.
        if ((mode == Object::DictMode::ReadWriteDynamic) && (maxlength == 0)) {
            maxlength = MinDynamicDictCapacity;
        }
        auto num_buckets = bucket_count_for_capacity(maxlength);
        auto entry_offset = alloc_size(num_buckets);
        auto pool_size = (sizeof(DictEntry) * maxlength);
        auto [dict, offset] = trx->vm_alloc<Dict>(static_cast<vm_size_t>(entry_offset + pool_size));
        dict->m_length = 0;
        dict->m_maxlength = maxlength;
        dict->m_bucket_count = num_buckets;
        if (mode == Object::DictMode::ReadWriteFixed) {
            dict->m_access = ReadWriteAccess;
        } else if (mode == Object::DictMode::ReadWriteDynamic) {
            dict->m_access = (ReadWriteAccess | DynamicFlag);
        } else {
            dict->m_access = ReadOnlyAccess;
            // ReadOnly dicts created with m_curr_alloc_global=false (i.e., during
            // init.inl: systemdict, numbersdict, errordict, the convenience
            // dicts, etc.) cannot ever acquire a global ref -- the user-facing
            // put op enforces read-only at runtime, and init's puts all run
            // with the global flag off.  Flag them so gc_walk_contents
            // short-circuits.  See the NoGlobalRefs flag comment.
            if (!trx->m_curr_alloc_global) {
                dict->m_access |= NoGlobalRefs;
            }
        }
        dict->m_dict_save_level = trx->m_curr_save_level;
        dict->m_next_in_visit = nulloffset;
        std::fill_n(dict->buckets(), num_buckets, nulloffset);
        if (maxlength != 0) {
            auto entries = reinterpret_cast<DictEntry *>(reinterpret_cast<vm_t *>(dict) + entry_offset);
            dict->init_dict_pool(trx, maxlength, entries);
        } else {
            dict->m_pool = nulloffset;
        }
        return std::pair{dict, offset};
    }

    // Allocate a Dict in the global region (top of heap, journal-skipped,
    // never reclaimed by save/restore).  Mirrors create_dict but allocates
    // via gvm_alloc instead of vm_alloc.  Called from create_dict()
    // dispatch (when m_curr_alloc_global is set) and from
    // make_global_dict_from_mark (the <<...>>#$ literal path).
    [[nodiscard]] static std::pair<Dict *, vm_offset_t>
    create_global_dict(Trix *trx, length_t maxlength, Object::DictMode mode = Object::DictMode::ReadWriteFixed) {
        // Mirror create_dict: a dynamic dict created empty must stay growable.
        if ((mode == Object::DictMode::ReadWriteDynamic) && (maxlength == 0)) {
            maxlength = MinDynamicDictCapacity;
        }
        auto num_buckets = bucket_count_for_capacity(maxlength);
        auto entry_offset = alloc_size(num_buckets);
        auto pool_size = (sizeof(DictEntry) * maxlength);
        auto [dict, offset] = trx->gvm_alloc<Dict>(static_cast<vm_size_t>(entry_offset + pool_size), Trix::ChunkKind::Dict);
        dict->m_length = 0;
        dict->m_maxlength = maxlength;
        dict->m_bucket_count = num_buckets;
        if (mode == Object::DictMode::ReadWriteFixed) {
            dict->m_access = ReadWriteAccess;
        } else if (mode == Object::DictMode::ReadWriteDynamic) {
            dict->m_access = (ReadWriteAccess | DynamicFlag);
        } else {
            dict->m_access = ReadOnlyAccess;
        }
        dict->m_dict_save_level = trx->m_curr_save_level;
        dict->m_next_in_visit = nulloffset;
        std::fill_n(dict->buckets(), num_buckets, nulloffset);
        if (maxlength != 0) {
            auto entries = reinterpret_cast<DictEntry *>(reinterpret_cast<vm_t *>(dict) + entry_offset);
            dict->init_dict_pool(trx, maxlength, entries);
        } else {
            dict->m_pool = nulloffset;
        }
        return std::pair{dict, offset};
    }

    // Create a Set: Dict header with SetFlag, SetEntry pool (12 bytes each, no value).
    [[nodiscard]] static std::pair<Dict *, vm_offset_t>
    create_set(Trix *trx, length_t maxlength, Object::DictMode mode = Object::DictMode::ReadWriteFixed) {
        // Mirror create_dict: a dynamic set created empty (e.g. `0 set`, or a set op
        // whose result is empty) must stay growable -- expand_set doubles capacity.
        if ((mode == Object::DictMode::ReadWriteDynamic) && (maxlength == 0)) {
            maxlength = MinDynamicDictCapacity;
        }
        auto num_buckets = bucket_count_for_capacity(maxlength);
        auto entry_offset = alloc_size(num_buckets);
        auto pool_size = (sizeof(SetEntry) * maxlength);
        auto [dict, offset] = trx->vm_alloc<Dict>(static_cast<vm_size_t>(entry_offset + pool_size));
        dict->m_length = 0;
        dict->m_maxlength = maxlength;
        dict->m_bucket_count = num_buckets;
        if (mode == Object::DictMode::ReadWriteFixed) {
            dict->m_access = (ReadWriteAccess | SetFlag);
        } else if (mode == Object::DictMode::ReadWriteDynamic) {
            dict->m_access = (ReadWriteAccess | DynamicFlag | SetFlag);
        } else {
            dict->m_access = (ReadOnlyAccess | SetFlag);
        }
        dict->m_dict_save_level = trx->m_curr_save_level;
        dict->m_next_in_visit = nulloffset;
        std::fill_n(dict->buckets(), num_buckets, nulloffset);
        if (maxlength != 0) {
            auto entries = reinterpret_cast<SetEntry *>(reinterpret_cast<vm_t *>(dict) + entry_offset);
            dict->init_set_pool(trx, maxlength, entries);
        } else {
            dict->m_pool = nulloffset;
        }
        return std::pair{dict, offset};
    }

    // Allocate a Set in the global region.  Mirrors create_set but allocates
    // via gvm_alloc instead of vm_alloc.  Backs the {{...}}#$ literal
    // suffix; the set struct + bucket array + entry pool all live above
    // m_vm_global so save/restore never reclaims them.
    [[nodiscard]] static std::pair<Dict *, vm_offset_t>
    create_global_set(Trix *trx, length_t maxlength, Object::DictMode mode = Object::DictMode::ReadWriteFixed) {
        // Mirror create_set: a dynamic set created empty must stay growable.
        if ((mode == Object::DictMode::ReadWriteDynamic) && (maxlength == 0)) {
            maxlength = MinDynamicDictCapacity;
        }
        auto num_buckets = bucket_count_for_capacity(maxlength);
        auto entry_offset = alloc_size(num_buckets);
        auto pool_size = (sizeof(SetEntry) * maxlength);
        auto [dict, offset] = trx->gvm_alloc<Dict>(static_cast<vm_size_t>(entry_offset + pool_size), Trix::ChunkKind::Set);
        dict->m_length = 0;
        dict->m_maxlength = maxlength;
        dict->m_bucket_count = num_buckets;
        if (mode == Object::DictMode::ReadWriteFixed) {
            dict->m_access = (ReadWriteAccess | SetFlag);
        } else if (mode == Object::DictMode::ReadWriteDynamic) {
            dict->m_access = (ReadWriteAccess | DynamicFlag | SetFlag);
        } else {
            dict->m_access = (ReadOnlyAccess | SetFlag);
        }
        dict->m_dict_save_level = trx->m_curr_save_level;
        dict->m_next_in_visit = nulloffset;
        std::fill_n(dict->buckets(), num_buckets, nulloffset);
        if (maxlength != 0) {
            auto entries = reinterpret_cast<SetEntry *>(reinterpret_cast<vm_t *>(dict) + entry_offset);
            dict->init_set_pool(trx, maxlength, entries);
        } else {
            dict->m_pool = nulloffset;
        }
        return std::pair{dict, offset};
    }

    // Allocate a temporary Dict in the top-end temp region (vm_temp_alloc_ptr).
    //
    // Uses ReadWriteFixed mode -- expansion is prohibited because expand_dict()
    // allocates from the normal heap; that memory would be orphaned when
    // vm_temp_restore reclaims the temp region.  Caller must provide
    // sufficient maxlength to hold all entries without expansion.
    //
    // Safety invariants:
    //   1. No save/restore journaling occurs: the Dict is created at the
    //      current save level, so Dict::save() and per-entry save checks
    //      are no-ops.  All put() calls must occur at the same save level
    //      as creation -- guaranteed when the Dict is used entirely within
    //      a single synchronous operator or callback handler.
    //   2. vm_offset_t addressing works: the temp region is within
    //      [m_vm_base, m_vm_limit), so ptr_to_offset/offset_to_ptr are valid.
    //   3. Caller must vm_temp_restore after use to reclaim the Dict.
    //   4. Never expose the returned offset as Object::make_dict on a
    //      stack that outlives the temp region.
    [[nodiscard]] static std::pair<Dict *, vm_offset_t> create_temp_dict(Trix *trx, length_t maxlength) {
        auto num_buckets = bucket_count_for_capacity(maxlength);
        auto entry_offset = alloc_size(num_buckets);
        auto pool_size = (sizeof(DictEntry) * maxlength);
        auto dict = trx->vm_temp_alloc_ptr<Dict>(static_cast<vm_size_t>(entry_offset + pool_size));
        auto offset = static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(dict) - trx->m_vm_base);
        dict->m_length = 0;
        dict->m_maxlength = maxlength;
        dict->m_bucket_count = num_buckets;
        dict->m_access = ReadWriteAccess;  // ReadWriteFixed (no DynamicFlag)
        dict->m_dict_save_level = trx->m_curr_save_level;
        dict->m_next_in_visit = nulloffset;
        std::fill_n(dict->buckets(), num_buckets, nulloffset);
        if (maxlength != 0) {
            auto entries = reinterpret_cast<DictEntry *>(reinterpret_cast<vm_t *>(dict) + entry_offset);
            dict->init_dict_pool(trx, maxlength, entries);
        } else {
            dict->m_pool = nulloffset;
        }
        return std::pair{dict, offset};
    }

    // Try the per-save-level dict pool before allocating a new dict.
    // Only reuses dicts at the current save level (prevents use-after-restore).
    // Sentinel value stored in m_buckets[0] when a dict is on the free list.
    // Detects double-recycle at O(1), same principle as ExtValue's dead sentinel.
    static constexpr vm_offset_t RecycleSentinel{0xDEAD};

    // Re-initialize a dict that came off one of the free lists.  Shared between
    // the regular create_or_recycle path and the frame-dict path.
    static void reinit_recycled(Trix *trx, Dict *dict, length_t maxlength) {
        dict->m_length = 0;
        dict->m_access = ReadWriteAccess;
        dict->m_dict_save_level = trx->m_curr_save_level;
        // Defensive: a recycled dict should already have m_next_in_visit ==
        // nulloffset (GC always drains the visit list before sweep).  Reset
        // anyway to keep the off-list invariant explicit at recycling boundaries.
        dict->m_next_in_visit = nulloffset;
        std::fill_n(dict->buckets(), dict->m_bucket_count, nulloffset);
        auto entry_offset = alloc_size(dict->m_bucket_count);
        auto entries = reinterpret_cast<DictEntry *>(reinterpret_cast<vm_t *>(dict) + entry_offset);
        dict->init_dict_pool(trx, maxlength, entries);
    }

    // Try the per-save-level dict pool before allocating a new dict.
    // Only reuses dicts at the current save level (prevents use-after-restore).
    [[nodiscard]] static std::pair<Dict *, vm_offset_t> create_or_recycle(Trix *trx, length_t maxlength) {
        auto pool_idx = trx->m_curr_save_level * MaxDictPoolSize + (maxlength - 1);
        if ((maxlength > 0) && (maxlength <= MaxDictPoolSize) && (trx->m_dict_pool[pool_idx] != nulloffset)) {
            auto offset = trx->m_dict_pool[pool_idx];
            auto dict = trx->offset_to_ptr<Dict>(offset);
            // verify recycled sentinel (detect corruption)
            if (dict->m_buckets[0] != RecycleSentinel) {
                trx->error(Error::InternalError, "Dict::create_or_recycle: recycled dict missing sentinel (internal error)");
            } else {
                // unlink from free list (next pointer stored in m_pool)
                trx->m_dict_pool[pool_idx] = dict->m_pool;
                reinit_recycled(trx, dict, maxlength);
                return std::pair{dict, offset};
            }
        }
        return create_dict(trx, maxlength);
    }

    // Frame-dict allocator for |locals|#N.  Checks the save-level-free frame
    // pool first (m_frame_dict_pool[capacity-1]) so a |locals|#N enter-exit
    // cycle inside a for-all / loop reuses the same dict across iterations
    // even when a save fires between iterations.  Falls through to a fresh
    // VM allocation on miss -- deliberately does NOT fall through to the
    // generic per-save-level pool: frame and non-frame dicts live in
    // disjoint pools to keep the save/restore transparency invariant simple.
    // For maxlength > MaxDictPoolSize (rare in user code, common in showcase
    // procs like best-placement-oracle's |...|#25), check the overflow chain
    // m_frame_dict_overflow next: a single linked list scanned linearly for a
    // parked dict with matching maxlength.  The returned dict already has
    // Dict::IsFrame set.
    [[nodiscard]] static std::pair<Dict *, vm_offset_t> create_or_recycle_frame_dict(Trix *trx, length_t maxlength) {
        if ((maxlength > 0) && (maxlength <= MaxDictPoolSize) && (trx->m_frame_dict_pool[maxlength - 1] != nulloffset)) {
            auto offset = trx->m_frame_dict_pool[maxlength - 1];
            auto dict = trx->offset_to_ptr<Dict>(offset);
            if (dict->m_buckets[0] != RecycleSentinel) {
                trx->error(Error::InternalError,
                           "Dict::create_or_recycle_frame_dict: recycled dict missing sentinel (internal error)");
            } else {
                // unlink from frame pool (next pointer stored in m_pool)
                trx->m_frame_dict_pool[maxlength - 1] = dict->m_pool;
                reinit_recycled(trx, dict, maxlength);
                dict->m_access |= IsFrame;
                return std::pair{dict, offset};
            }
        }
        if ((maxlength > MaxDictPoolSize) && (trx->m_frame_dict_overflow != nulloffset)) {
            auto *prev_link = &trx->m_frame_dict_overflow;
            while (*prev_link != nulloffset) {
                auto offset = *prev_link;
                auto dict = trx->offset_to_ptr<Dict>(offset);
                if (dict->m_maxlength == maxlength) {
                    if (dict->m_buckets[0] != RecycleSentinel) {
                        trx->error(Error::InternalError,
                                   "Dict::create_or_recycle_frame_dict: recycled dict missing sentinel (internal error)");
                    } else {
                        *prev_link = dict->m_pool;
                        reinit_recycled(trx, dict, maxlength);
                        dict->m_access |= IsFrame;
                        return std::pair{dict, offset};
                    }
                }
                prev_link = &dict->m_pool;
            }
        }
        auto [dict, offset] = create_dict(trx, maxlength);
        dict->m_access |= IsFrame;
        return std::pair{dict, offset};
    }

    // Return a dict to its appropriate free list for reuse.  Frame dicts
    // (IsFrame set) go to the save-level-free m_frame_dict_pool when their
    // maxlength fits the indexed pool, else onto m_frame_dict_overflow
    // (single chain scanned by maxlength on alloc).  Non-frame dicts go
    // to the per-save-level m_dict_pool bucketed by the dict's own save
    // level (same principle as ExtValue::free); non-frame dicts with
    // maxlength > MaxDictPoolSize are abandoned (uncommon -- generic
    // `dict` op users rarely exceed 16 entries; if a future use case
    // hits this path, mirror the frame-dict overflow pattern here).
    static void recycle(Trix *trx, Dict *dict, vm_offset_t offset) {
        auto maxlength = dict->m_maxlength;
        if (maxlength != 0) {
            // detect double-recycle
            if (dict->m_buckets[0] == RecycleSentinel) {
                trx->error(Error::InternalError, "Dict::recycle: double-recycle of dict at offset {} (internal error)", offset);
            } else if (dict->is_frame()) {
                if (maxlength <= MaxDictPoolSize) {
                    // Frame dicts: save-level-free pool (indexed by capacity only).
                    // Save/restore treats frame dicts as transparent so no level-
                    // bucketing; on restore, the m_frame_dict_pool walk drops any
                    // entries whose offset sits above the rollback barrier.
                    dict->m_pool = trx->m_frame_dict_pool[maxlength - 1];
                    dict->m_buckets[0] = RecycleSentinel;
                    trx->m_frame_dict_pool[maxlength - 1] = offset;
                } else {
                    // Oversized frame dicts: overflow chain (also save-level-free,
                    // also pruned on restore).  Linear-scan on alloc, but the
                    // chain only contains sizes actually used, which is typically
                    // 1-3 distinct maxlengths even in heavy showcase code.
                    dict->m_pool = trx->m_frame_dict_overflow;
                    dict->m_buckets[0] = RecycleSentinel;
                    trx->m_frame_dict_overflow = offset;
                }
            } else if (maxlength <= MaxDictPoolSize) {
                auto pool_idx = dict->m_dict_save_level * MaxDictPoolSize + (maxlength - 1);
                dict->m_pool = trx->m_dict_pool[pool_idx];
                dict->m_buckets[0] = RecycleSentinel;
                trx->m_dict_pool[pool_idx] = offset;
            }
        }
    }

    // key_lookup() is the write-through path: besides returning the looked-up
    // value it mutates the VM heap (name bindings).  On a Name-key hit it calls
    // set_name_binding() to populate the binding cache, so subsequent name_lookup()
    // calls for the same Name hit the cache and bypass the full hash walk entirely.
    // The side effect is intentional -- this is how the cache is populated on
    // first access.
    [[nodiscard]] static std::pair<Object *, const Object *> key_lookup(Trix *trx, Object *key_ptr) {
        assert(!key_ptr->is_null());

        auto hash = key_ptr->hash(trx);
        if (key_ptr->is_name()) {
            // Name keys: offset comparison against Name entries, content compare against
            // String entries (binary-token-stream dicts).  Mirrors find_in_chain / name_lookup_in_stack.
            auto name_offset = key_ptr->name_offset();
            for (auto dict_obj_ptr = trx->m_dict_ptr; (dict_obj_ptr >= trx->m_dict_base); --dict_obj_ptr) {
                auto dict = dict_obj_ptr->dict_value(trx);
                auto offset = dict->m_buckets[fastmod_u32(hash, bucket_magic_for(dict->m_bucket_count), dict->m_bucket_count)];
                while (offset != nulloffset) {
                    auto entry = trx->offset_to_ptr<DictEntry>(offset);
                    auto matched = false;
                    if (entry->m_key.is_name()) {
                        matched = (name_offset == entry->m_key.name_offset());
                    } else if (entry->m_key.is_string()) {
                        matched = key_ptr->name_value(trx)->equal(entry->m_key.sv_value(trx));
                    }
                    if (matched) {
                        key_ptr->set_name_binding(trx, &entry->m_value);
                        return std::pair{dict_obj_ptr, &entry->m_value};
                    } else {
                        offset = entry->m_next;
                    }
                }
            }
        } else {
            for (auto dict_obj_ptr = trx->m_dict_ptr; (dict_obj_ptr >= trx->m_dict_base); --dict_obj_ptr) {
                auto dict = dict_obj_ptr->dict_value(trx);
                auto offset = dict->m_buckets[fastmod_u32(hash, bucket_magic_for(dict->m_bucket_count), dict->m_bucket_count)];
                while (offset != nulloffset) {
                    auto entry = trx->offset_to_ptr<DictEntry>(offset);
                    if (key_ptr->equal(trx, entry->m_key)) {
                        return std::pair{dict_obj_ptr, &entry->m_value};
                    } else {
                        offset = entry->m_next;
                    }
                }
            }
        }
        return std::pair{nullptr, nullptr};
    }

    // Dict-stack walk for a name key without consulting the binding cache.
    // Caller has already determined the cache is empty and has the Name pointer in hand.
    // Populates the cache on a successful match.
    //   Name entries:   offset equality (VM-interned names hash-collide-free by identity).
    //   String entries: content comparison against Name::equal (binary-token-stream dicts).
    [[nodiscard]] static const Object *name_lookup_in_stack(Trix *trx, const Object *key_ptr, const Name *name) {
        auto hash = name->hash();
        auto name_offset = trx->ptr_to_offset(name);
        for (auto dict_obj_ptr = trx->m_dict_ptr; (dict_obj_ptr >= trx->m_dict_base); --dict_obj_ptr) {
            auto dict = dict_obj_ptr->dict_value(trx);
            auto offset = dict->m_buckets[fastmod_u32(hash, bucket_magic_for(dict->m_bucket_count), dict->m_bucket_count)];
            while (offset != nulloffset) {
                auto entry = trx->offset_to_ptr<DictEntry>(offset);
                auto matched = false;
                if (entry->m_key.is_name()) {
                    matched = (name_offset == entry->m_key.name_offset());
                } else if (entry->m_key.is_string()) {
                    matched = name->equal(entry->m_key.sv_value(trx));
                }
                if (matched) {
                    auto value_ptr = &entry->m_value;
                    key_ptr->set_name_binding(trx, value_ptr);
                    return value_ptr;
                } else {
                    offset = entry->m_next;
                }
            }
        }
        return nullptr;
    }

    // Walks the dict stack from top to bottom and returns the first dict
    // whose Dict::IsFrame bit is NOT set.  At BASE the bottom dict is
    // userdict (non-frame), so the walk always terminates with a valid
    // result.  Used by def / store-fallback / current-dict / import to
    // skip past |...| locals frames so their writes land in the
    // surrounding module scope.  Counterpart `local-def` requires the
    // top to BE a frame and writes there directly.
    [[nodiscard]] static Object *dict_stack_first_nonframe(Trix *trx) {
        for (auto dict_obj_ptr = trx->m_dict_ptr; (dict_obj_ptr >= trx->m_dict_base); --dict_obj_ptr) {
            if (!dict_obj_ptr->dict_value(trx)->is_frame()) {
                return dict_obj_ptr;
            }
        }
        // PermanentDictCount guarantees userdict is non-frame at the bottom.
        // Returning nullptr here would indicate VM corruption.
        assert(false);
        std::unreachable();

        return nullptr;
    }

    // Searches a bucket chain for a key match, handling Name/String duality.
    // Name keys match by offset against Name entries and by content against String entries
    // (binary token stream dicts use String keys for the same logical names).
    // Non-Name keys use Object::equal().  Returns the matching entry or nullptr.
    template<typename EntryType>
    [[nodiscard]] EntryType *find_in_chain(Trix *trx, Object key_obj, vm_offset_t offset) const {
        if (key_obj.is_name()) {
            // Defer name_value(trx) into the String-entry arm: for the overwhelming majority
            // of dicts every entry is Name-keyed, so we pay for the Name resolution only
            // when we actually need it (binary-token-stream dicts with String keys).
            auto name_offset = key_obj.name_offset();
            while (offset != nulloffset) {
                auto entry = trx->offset_to_ptr<EntryType>(offset);
                if (entry->m_key.is_name()) {
                    if (name_offset == entry->m_key.name_offset()) {
                        return entry;
                    }
                } else if (entry->m_key.is_string()) {
                    if (key_obj.name_value(trx)->equal(entry->m_key.sv_value(trx))) {
                        return entry;
                    }
                }
                offset = entry->m_next;
            }
        } else {
            while (offset != nulloffset) {
                auto entry = trx->offset_to_ptr<EntryType>(offset);
                if (key_obj.equal(trx, entry->m_key)) {
                    return entry;
                } else {
                    offset = entry->m_next;
                }
            }
        }
        return nullptr;
    }

    [[nodiscard]] Object *get(Trix *trx, const Object *key_ptr) const {
        assert(!key_ptr->is_null());

        auto hash = key_ptr->hash(trx);
        auto entry = find_in_chain<DictEntry>(
                trx, *key_ptr, m_buckets[fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count)]);
        return (entry ? &entry->m_value : nullptr);
    }

    [[nodiscard]] Object *get(Trix *trx, Object key_obj) const { return get(trx, &key_obj); }

    [[nodiscard]] Object get(Trix *trx, SystemName systemname_key) const {
        auto value_ptr = get(trx, Name::make_system(trx, systemname_key));
        if (value_ptr == nullptr) {
            trx->error(Error::InternalError, "systemdict: system name not found");
        } else {
            return *value_ptr;
        }
    }

    [[nodiscard]] DictEntry *find_dict_entry(Trix *trx, Object key_obj, uint32_t hash) const {
        return find_in_chain<DictEntry>(
                trx, key_obj, m_buckets[fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count)]);
    }

    std::pair<length_t, vm_size_t> expand_size(length_t need_count) const {
        auto max_count = std::numeric_limits<length_t>::max();
        auto grow_count = std::max(m_maxlength, need_count);
        grow_count = std::min(grow_count, static_cast<length_t>(max_count - m_maxlength));
        return std::pair{grow_count, static_cast<vm_size_t>(grow_count * sizeof(DictEntry))};
    }

    // Set counterpart of expand_size: clamped-doubling growth math sized for
    // SetEntry.  Shared by expand_set and copy_set.
    std::pair<length_t, vm_size_t> expand_set_size(length_t need_count) const {
        auto max_count = std::numeric_limits<length_t>::max();
        auto grow_count = std::max(m_maxlength, need_count);
        grow_count = std::min(grow_count, static_cast<length_t>(max_count - m_maxlength));
        return std::pair{grow_count, static_cast<vm_size_t>(grow_count * sizeof(SetEntry))};
    }

    // Grow a dynamic dict's entry pool by doubling capacity.
    // NOTE: bucket count stays fixed, so the load factor grows proportionally with each
    // expansion and average lookup degrades from O(1) toward O(N).
    // Intended for dicts that grow infrequently and are not on hot lookup paths.
    // Allocate the auxiliary entry pool block when the dict outgrows
    // its initial pool.  Region-aware: a Dict allocated in the global
    // region must place its auxiliary pool in the global region too;
    // a local-VM Dict places it in local VM.  Cross-region pools would
    // dangle on save/restore (local pool reclaimed while the global
    // Dict header survives) or worse, corrupt the Dict header reads
    // when the local-VM region is reused after rewind.
    //
    // Global Dicts: gc_walk_contents marks these auxiliary pool blocks too.
    // It back-walks each bucket-chain and free-list entry to the owning
    // expansion block (via gvm_find_owning_payload) and marks that block, so a
    // vm-global-gc pass after expansion does not reclaim the pools as orphans.
    void expand_dict(Trix *trx) {
        assert(m_pool == nulloffset);

        if (m_maxlength == 0) {
            // grow_count would be zero -> init_dict_pool assert + UB in release builds
            trx->error(Error::DictFull, "dict with zero capacity cannot be expanded");
        } else if (m_maxlength == std::numeric_limits<length_t>::max()) {
            trx->error(Error::DictFull, "dict too large to expand");
        } else {
            auto [grow_count, grow_size] = expand_size(1);
            DictEntry *entries;
            if (is_global(trx)) {
                // A bare DictEntry[] expansion block is NOT a Dict header -- tag it
                // HashEntry (a GC leaf) so the back-walk in gc_walk_contents keeps it
                // alive without re-walking it as a Dict header (which mis-reads an
                // entry's m_key/m_value bytes as bucket offsets).  Entries are marked
                // by the owning dict's bucket walk.
                entries = trx->gvm_alloc<DictEntry>(grow_size, Trix::ChunkKind::HashEntry).first;
            } else {
                entries = trx->vm_alloc_ptr<DictEntry>(grow_size);
            }
            init_dict_pool(trx, grow_count, entries);
            m_maxlength = static_cast<length_t>(m_maxlength + grow_count);
        }
    }

    // Region-aware Set expansion -- mirrors expand_dict's reasoning.
    void expand_set(Trix *trx) {
        assert(m_pool == nulloffset);

        if (m_maxlength == 0) {
            trx->error(Error::DictFull, "set with zero capacity cannot be expanded");
        } else if (m_maxlength == std::numeric_limits<length_t>::max()) {
            trx->error(Error::DictFull, "set too large to expand");
        } else {
            auto [grow_count, grow_size] = expand_set_size(static_cast<length_t>(1));
            SetEntry *entries;
            if (is_global(trx)) {
                // HashEntry (GC leaf), not Set -- see expand_dict: a bare SetEntry[]
                // is not a Set header and must not be re-walked as one.
                entries = trx->gvm_alloc<SetEntry>(grow_size, Trix::ChunkKind::HashEntry).first;
            } else {
                entries = trx->vm_alloc_ptr<SetEntry>(grow_size);
            }
            init_set_pool(trx, grow_count, entries);
            m_maxlength = static_cast<length_t>(m_maxlength + grow_count);
        }
    }

    // Copy a String key's bytes into the container's region and return the rewrapped
    // key.  A global container is journal-skipped (its mutations persist across restore
    // -- see Save::save_data), so its key must be a GLOBAL, GC-TAGGED String block:
    // gvm_alloc_n with ChunkKind::String -- NOT vm_alloc_global_ptr, an internal
    // allocator primitive whose untagged raw block corrupts gvm_for_each.  A local
    // container keeps the local copy.  The global alloc can fire vm_global_gc, so the
    // caller must keep key_obj reachable across this call (put_op / set-add leave it on
    // the operand stack); the source bytes are read after the allocation.
    Object copy_string_key_into_region(Trix *trx, Object key_obj, bool to_global) {
        auto length = key_obj.string_length();
        auto [base, offset] =
                to_global ? trx->gvm_alloc_n<vm_t>(length + 1, Trix::ChunkKind::String) : trx->vm_alloc<vm_t>(length + 1);
        // No gvm_set_obj_count: String is a GC leaf (walk_block_contents never
        // reads m_obj_count for it), and length+1 is a byte count that would
        // truncate to 0 at MaxStringLength anyway -- stamping it is a no-op.
        std::copy_n(key_obj.string_vptr(trx), length, base);
        base[length] = '\0';
        return Object::make_string(offset, length, key_obj.attrib(), Object::ReadOnlyAccess);
    }

    // Takes ownership of key and value (moves ExtValue).  Caller must not use
    // the Objects after put; clone before calling if the caller needs to retain them.
    // The result is often discarded when populating dicts, hence the lack of [[nodiscard]].
    Object *put(Trix *trx, Object key_obj, Object val_obj, BindingMode mode = BindingMode::NoBinding) {
        // ReadOnly is "enforced by caller convention", Dict::init() uses put() to populate dicts during start up
        assert(!key_obj.is_null());

        // R6 pointer hygiene: a global dict CLONES a fragile-local SCALAR key/value
        // into the global region (so it can hold any value across restore), done
        // LATE -- immediately before each move-store, after expand_dict /
        // copy_string_key_into_region / journaling -- so no vm_global_gc or throw
        // intervenes.  NON-scalar fragile-locals (Name/String/composite) can't be
        // moved by make_clone, so they are rejected here UPFRONT (before any clone/
        // free, so the caller's operands stay intact on the throw).
        auto container_is_global = is_global(trx);
        Save::reject_local_into_global(trx, container_is_global, key_obj, "put");
        Save::reject_local_into_global(trx, container_is_global, val_obj, "put");

        auto dict = this;
        auto is_eqdict = (dict == trx->m_eqdict);
        auto hash = key_obj.hash(trx);
        auto bucket_index = fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count);
        auto entry = find_in_chain<DictEntry>(trx, key_obj, m_buckets[bucket_index]);

        if ((entry == nullptr) && (m_length == m_maxlength) && (m_maxlength != 0) && is_dynamic()) {
            // save() BEFORE expand_dict(): expansion allocates new VM entries and mutates
            // m_pool / m_maxlength in the Dict header.  If save() ran after expansion, restore()
            // would write back the post-expansion header while the VM barrier discards the entry
            // storage, leaving m_pool dangling and m_maxlength inflated.
            save(trx);
            expand_dict(trx);
        }

        if ((entry != nullptr) || (m_length < m_maxlength)) {
            // make the Dict, buckets, freelist restorable (no-op if already saved above)
            save(trx);

            auto curr_save_level = trx->m_curr_save_level;
            if (entry != nullptr) {
                // free key's ExtValue
                // move value's ExtValue
                key_obj.maybe_free_extvalue(trx);

                // check if this DictEntry has been saved at the current save level
                if ((entry->m_value.save_level() == curr_save_level) || is_eqdict) {
                    entry->m_value.maybe_free_extvalue(trx);
                } else {
                    Save::save_dict_entry(trx, entry);
                }

                // set new value (moving ExtValue from parameter) and update the Object save_level.
                // Clone a fragile-local SCALAR into global for a global dict; the existing
                // entry is already chained (GC-reachable) and no alloc follows the clone, so
                // no rooting is needed.
                val_obj = Save::clone_fragile_scalar_into_global(trx, container_is_global, val_obj);
                entry->m_value = val_obj;
                entry->m_value.set_save_level(curr_save_level);
            } else {
                // add new DictEntry

                // For String keys: copy the data BEFORE touching the pool -- the alloc
                // can throw VMFull, and if the entry had already been removed from the
                // pool, a caught VMFull would leave the pool permanently short by one
                // entry.  Copy into the container's region so a global dict's key is not
                // a restore-fragile local block (see copy_string_key_into_region).
                if (key_obj.is_string()) {
                    key_obj = copy_string_key_into_region(trx, key_obj, container_is_global);
                }

                // get DictEntry from memory pool
                auto entry_offset = m_pool;
                entry = trx->offset_to_ptr<DictEntry>(entry_offset);
                m_pool = entry->m_next;

                // check if this DictEntry has been saved at the current save level
                // initial/Type::Null m_value.m_object_save_level == Dict.m_dict_save_level
                if (((entry->m_key.save_level() != curr_save_level) || (entry->m_value.save_level() != curr_save_level)) &&
                    !is_eqdict) {
                    Save::save_dict_entry(trx, entry);
                }

                // Clone fragile-local SCALAR key/value into global for a global dict.
                // The entry is NOT yet chained (added to m_buckets below), so it is not
                // GC-reachable via gc_walk_contents -- the value clone's gvm_alloc fires a
                // GC under stress that would sweep a key already written into this floating
                // entry.  So clone BOTH first, rooting the key clone across the value
                // clone's alloc, THEN store both (the stores + chain-add allocate nothing).
                // String keys were already region-copied above (copy_string_key_into_region),
                // so the helper passes them through.
                key_obj = Save::clone_fragile_scalar_into_global(trx, container_is_global, key_obj);
                trx->require_gc_root_capacity_more(1);
                *++trx->m_gc_roots_ptr = key_obj;
                val_obj = Save::clone_fragile_scalar_into_global(trx, container_is_global, val_obj);
                trx->gc_root_pop_n(1);

                entry->m_key = key_obj;
                entry->m_key.set_save_level(curr_save_level);
                entry->m_value = val_obj;
                entry->m_value.set_save_level(curr_save_level);

                // add DictEntry to head of bucket chain
                auto chain = &m_buckets[bucket_index];
                entry->m_next = *chain;
                *chain = entry_offset;

                ++m_length;
            }
            if (entry->m_key.is_name()) {
                // Bind: immediately cache the new value for this name -- used by the `bind`
                //   operator to pre-cache operator bindings without waiting for a lookup.
                // NoBinding (normal put): clear any existing binding so stale cache entries
                //   from a differently-ordered dict stack do not shadow the updated value.
                if (mode == BindingMode::Bind) {
                    entry->m_key.set_name_binding(trx, &entry->m_value);
                } else {
                    entry->m_key.clear_name_binding(trx);
                }
            }
            return &entry->m_value;
        } else {
            trx->error(Error::DictFull, "dict fixed capacity of {} exceeded", m_maxlength);
        }
    }

    // Overwrite-only, non-journaled put for the -persist family.  Returns
    // a pointer to the entry's value slot on success, or nullptr if no
    // entry matches the key (caller raises /above-barrier in that case
    // because allocating a fresh entry under save would leak: the
    // DictEntry header mutations -- m_pool, m_length, bucket-chain head --
    // happen above the barrier and would dangle on restore).  No journal
    // call, no binding-cache update -- the mutation persists across the
    // enclosing save/restore.
    [[nodiscard]] Object *put_persist(Trix *trx, Object key_obj, Object val_obj) {
        assert(!key_obj.is_null());

        // R6 pointer hygiene: a global dict clones a fragile-local SCALAR value into
        // global, and rejects a NON-scalar fragile-local upfront (see Dict::put).
        // Only an existing entry is overwritten here (no new key stored), so only
        // the value is cloned, right before the move-store -- no alloc/throw between.
        auto container_is_global = is_global(trx);
        Save::reject_local_into_global(trx, container_is_global, key_obj, "put-persist");
        Save::reject_local_into_global(trx, container_is_global, val_obj, "put-persist");

        auto hash = key_obj.hash(trx);
        auto bucket_index = fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count);
        auto entry = find_in_chain<DictEntry>(trx, key_obj, m_buckets[bucket_index]);
        if (entry == nullptr) {
            return nullptr;
        } else {
            // Input key is no longer needed; release any ExtValue it held.
            key_obj.maybe_free_extvalue(trx);

            // Free the existing value's ExtValue (no journaling -- we want the
            // overwrite to persist across restore).
            entry->m_value.maybe_free_extvalue(trx);
            val_obj = Save::clone_fragile_scalar_into_global(trx, container_is_global, val_obj);
            entry->m_value = val_obj;
            // Mark the value as belonging to BASE: a persisted value survives
            // every restore, so it must appear to have been set at sl=0.  If
            // we used m_curr_save_level here, a later restore back to BASE
            // would leave m_value.save_level() pinned above current; the next
            // regular put() would then mistake the entry for "set at a higher
            // level" and try to journal it -- raising /invalid-restore from
            // save_data() because no save is active.
            entry->m_value.set_save_level(Save::BASE);

            // Invalidate any cached binding for a Name key: the binding cache is
            // pruned on restore anyway, but the dict's value is now authoritative.
            if (entry->m_key.is_name()) {
                entry->m_key.clear_name_binding(trx);
            }
            return &entry->m_value;
        }
    }

    // Like put_persist, but allocates a fresh DictEntry
    // in global VM when the key is missing, so def-persist and put-persist
    // can create new bindings at any save level.  The new entry survives
    // restore (global VM is journal-skipped); the chain-head write into
    // m_buckets[bucket_index] is non-journaled, mirroring put_persist's
    // overwrite path -- safe when the dict header itself outlives the
    // save scope (dict at BASE, or global dict).
    Object *put_persist_or_create(Trix *trx, Object key_obj, Object val_obj) {
        assert(!key_obj.is_null());

        // R6 pointer hygiene: a global dict clones a fragile-local SCALAR key/value
        // into global, and rejects a NON-scalar fragile-local upfront (see Dict::put).
        auto container_is_global = is_global(trx);
        Save::reject_local_into_global(trx, container_is_global, key_obj, "put-persist");
        Save::reject_local_into_global(trx, container_is_global, val_obj, "put-persist");

        auto hash = key_obj.hash(trx);
        auto bucket_index = fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count);
        auto entry = find_in_chain<DictEntry>(trx, key_obj, m_buckets[bucket_index]);

        if (entry != nullptr) {
            // Existing entry: overwrite (matches put_persist).  Only the value is
            // stored (key freed); clone it right before the move-store -- no
            // alloc/throw intervenes.
            key_obj.maybe_free_extvalue(trx);
            entry->m_value.maybe_free_extvalue(trx);
            val_obj = Save::clone_fragile_scalar_into_global(trx, container_is_global, val_obj);
            entry->m_value = val_obj;
            entry->m_value.set_save_level(Save::BASE);
            if (entry->m_key.is_name()) {
                entry->m_key.clear_name_binding(trx);
            }
            return &entry->m_value;
        } else {
            // Missing key -- allocate a fresh DictEntry in global VM.  Clone fragile-local
            // key/value into global FIRST (the entry is global, so both must be), then
            // root BOTH across the GC-capable entry alloc.  Clone the value before
            // string-copying/cloning the key so the key, once global, is rooted before
            // the value clone's own alloc could fire a GC.  String keys: copy into a
            // GC-tagged global String so they survive restore (the -persist family
            // creates permanent, journal-skipped bindings).
            val_obj = Save::clone_fragile_scalar_into_global(trx, container_is_global, val_obj);
            trx->require_gc_root_capacity_more(2);
            *++trx->m_gc_roots_ptr = val_obj;
            if (key_obj.is_string()) {
                key_obj = copy_string_key_into_region(trx, key_obj, true);
            } else {
                key_obj = Save::clone_fragile_scalar_into_global(trx, container_is_global, key_obj);
            }
            *++trx->m_gc_roots_ptr = key_obj;

            // The entry is a standalone ChunkKind::HashEntry block (not part of the
            // dict's pool); Dict::gc_walk_contents back-marks it via the bucket chain.
            // Both key and value are rooted across the entry alloc, which can fire
            // vm_global_gc before the entry references them.
            auto [new_entry, new_offset] = trx->gvm_alloc<DictEntry>(sizeof(DictEntry), Trix::ChunkKind::HashEntry);
            trx->gc_root_pop_n(2);
            new_entry->m_key = key_obj;
            new_entry->m_key.set_save_level(Save::BASE);
            new_entry->m_value = val_obj;
            new_entry->m_value.set_save_level(Save::BASE);
            new_entry->m_next = m_buckets[bucket_index];

            // Non-journaled chain-head write + length bump.  Persists across
            // restore as long as the dict header itself does (BASE or global).
            m_buckets[bucket_index] = new_offset;
            ++m_length;

            return &new_entry->m_value;
        }
    }

    // Non-journaled undef for the -persist family.  Mirrors undef but
    // skips every Save:: call -- the removal must persist across the
    // enclosing save/restore.  No allocation, so no above-barrier
    // hazard: the dict mutations only reorganize existing slots that
    // were allocated when the dict was created.  Silent no-op if the
    // key is not present, matching undef's contract.
    void undef_persist(Trix *trx, Object *key_ptr) {
        if (key_ptr->is_null()) {
            trx->error(Error::TypeCheck, "undef-persist: key must not be null");
        } else {
            auto hash = key_ptr->hash(trx);
            auto prev_offset = &m_buckets[fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count)];
            auto offset = *prev_offset;

            DictEntry *entry = nullptr;
            if (key_ptr->is_name()) {
                // also match String-keyed entries with equal content
                auto name = key_ptr->name_value(trx);
                auto name_offset = key_ptr->name_offset();
                while (offset != nulloffset) {
                    auto target = trx->offset_to_ptr<DictEntry>(offset);
                    auto match = false;
                    if (target->m_key.is_name()) {
                        match = (name_offset == target->m_key.name_offset());
                    } else if (target->m_key.is_string()) {
                        match = name->equal(target->m_key.sv_value(trx));
                    }
                    if (match) {
                        dict_forall_update(trx, offset);
                        entry = target;
                        break;
                    } else {
                        prev_offset = &target->m_next;
                        offset = *prev_offset;
                    }
                }
            } else {
                while (offset != nulloffset) {
                    auto target = trx->offset_to_ptr<DictEntry>(offset);
                    if (key_ptr->equal(trx, target->m_key)) {
                        dict_forall_update(trx, offset);
                        entry = target;
                        break;
                    } else {
                        prev_offset = &target->m_next;
                        offset = *prev_offset;
                    }
                }
            }

            if (entry != nullptr) {
                // No journaling: free ExtValues immediately and reset directly.
                entry->m_key.maybe_free_extvalue(trx);
                entry->m_value.maybe_free_extvalue(trx);

                if (entry->m_key.is_name()) {
                    entry->m_key.clear_name_binding(trx);
                }

                // Mark the cleared slots as belonging to BASE: a persisted undef
                // survives every restore, so the freelist entry must look like
                // an sl=0 slot.  If we used m_curr_save_level here, a later
                // restore back to BASE would leave the freelist entry's
                // save_level pinned above current, and the next put() reusing
                // this entry would mistake it for "set at a higher level" and
                // try to journal it -- raising /invalid-restore from save_data()
                // because no save is active.
                auto null_obj = Object::make_null(Save::BASE);
                entry->m_key = null_obj;
                entry->m_value = null_obj;

                // Unlink from bucket chain and return to freelist.
                *prev_offset = entry->m_next;
                entry->m_next = m_pool;
                m_pool = offset;

                --m_length;
            }
        }
    }

    void undef(Trix *trx, Object *key_ptr) {
        if (key_ptr->is_null()) {
            trx->error(Error::TypeCheck, "undef: key must not be null");
        } else {
            auto dict = this;
            auto is_eqdict = (dict == trx->m_eqdict);
            auto hash = key_ptr->hash(trx);
            auto prev_offset = &m_buckets[fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count)];
            auto offset = *prev_offset;

            DictEntry *prev_entry = nullptr;
            DictEntry *entry = nullptr;
            if (key_ptr->is_name()) {
                // also match String-keyed entries with equal content (binary token stream dicts)
                auto name = key_ptr->name_value(trx);
                auto name_offset = key_ptr->name_offset();
                while (offset != nulloffset) {
                    auto target = trx->offset_to_ptr<DictEntry>(offset);
                    auto match = false;
                    if (target->m_key.is_name()) {
                        match = (name_offset == target->m_key.name_offset());
                    } else if (target->m_key.is_string()) {
                        match = name->equal(target->m_key.sv_value(trx));
                    }
                    if (match) {
                        dict_forall_update(trx, offset);
                        entry = target;
                        break;
                    } else {
                        prev_entry = target;
                        prev_offset = &target->m_next;
                        offset = *prev_offset;
                    }
                }
            } else {
                while (offset != nulloffset) {
                    auto target = trx->offset_to_ptr<DictEntry>(offset);
                    if (key_ptr->equal(trx, target->m_key)) {
                        dict_forall_update(trx, offset);
                        entry = target;
                        break;
                    } else {
                        prev_entry = target;
                        prev_offset = &target->m_next;
                        offset = *prev_offset;
                    }
                }
            }

            if (entry != nullptr) {
                // make the Dict and buckets restorable
                save(trx);

                auto curr_save_level = trx->m_curr_save_level;
                // check if this DictEntry has been saved at the current save level
                if (((entry->m_key.save_level() == curr_save_level) && (entry->m_value.save_level() == curr_save_level)) ||
                    is_eqdict) {
                    entry->m_key.maybe_free_extvalue(trx);
                    entry->m_value.maybe_free_extvalue(trx);
                } else {
                    Save::save_dict_entry(trx, entry);
                }

                // clear binding so the name reverts to a full dict-stack lookup
                if (entry->m_key.is_name()) {
                    entry->m_key.clear_name_binding(trx);
                }

                // reset the key/value back to the null Object
                auto null_obj = Object::make_null(curr_save_level);
                entry->m_key = null_obj;
                entry->m_value = null_obj;

                // remove DictEntry from bucket chain
                // save_dict_entry_next is only needed when prev_entry predates curr_save_level.
                // If prev_entry was put at curr_save_level its full snapshot -- including m_next --
                // was already captured by save_dict_entry at put time; on restore the entire entry
                // is replayed, which implicitly restores m_next.  Saving m_next again would be
                // redundant and would shadow the entry-level snapshot with a stale value.
                if ((prev_entry != nullptr) && (prev_entry->m_value.save_level() != curr_save_level) && !is_eqdict) {
                    Save::save_dict_entry_next(trx, prev_entry);
                }
                *prev_offset = entry->m_next;

                // return DictEntry to memory pool
                entry->m_next = m_pool;
                m_pool = offset;

                --m_length;
            }
        }
    }

    // Iterate all key-value pairs, calling fn(key, value) for each entry.
    template<typename Fn>
    void for_each(Trix *trx, Fn fn) const {
        for (dict_bucket_count_t i = 0; i < m_bucket_count; ++i) {
            auto offset = m_buckets[i];
            while (offset != nulloffset) {
                auto entry = trx->offset_to_ptr<DictEntry>(offset);
                fn(entry->m_key, entry->m_value);
                offset = entry->m_next;
            }
        }
    }

    void copy_dict(Trix *trx, const Dict *src) {
        auto remaining = static_cast<length_t>(m_maxlength - m_length);
        if (src->m_length > remaining) {
            auto fail = !is_dynamic();
            if (!fail) {
                auto needed = static_cast<length_t>(src->m_length - remaining);
                auto [_, grow_size] = expand_size(needed);
                fail = !trx->vm_size_available<DictEntry>(grow_size);
            }
            if (fail) {
                trx->error(Error::DictFull,
                           "copy: source length {} exceeds destination capacity {}",
                           src->m_length,
                           static_cast<length_t>(m_maxlength - m_length));
            }
        }
        // Under ${...} make_clone allocates the cloned key/value in the GLOBAL
        // region and each clone -- plus put's possible dynamic expansion -- can
        // fire vm_global_gc.  The destination dict is kept alive by the caller
        // (on the op stack for `copy`, or explicitly rooted by `merge`), but the
        // freshly cloned key/value are bare C locals until `put` links them in:
        // a GC firing between the key clone and the put would sweep the unrooted
        // global clones, leaving a dangling key (lookups miss) or value.  Root
        // both across the value-clone and the put.  require_..._more (not the
        // empty-asserting guard) so this composes with a caller that already
        // holds roots (merge's result-dict root); the entry pointer stays valid
        // because the source is rooted and GC does not move blocks.
        trx->require_gc_root_capacity_more(2);
        for (dict_bucket_count_t i = 0; i < src->m_bucket_count; ++i) {
            auto offset = src->m_buckets[i];
            while (offset != nulloffset) {
                auto entry = trx->offset_to_ptr<DictEntry>(offset);

                auto key_obj = entry->m_key.make_clone(trx);
                *++trx->m_gc_roots_ptr = key_obj;
                auto val_obj = entry->m_value.make_clone(trx);
                *++trx->m_gc_roots_ptr = val_obj;
                put(trx, key_obj, val_obj);
                trx->gc_root_pop_n(2);

                offset = entry->m_next;
            }
        }
    }

    struct DictIterEntry {
        vm_offset_t next_offset;
        integer_t next_bucket;
        Object key;
        Object value;
    };

    // Shared bucket-chain iteration: advance to the next entry.
    // Returns {offset, bucket_idx} of the next entry, or {nulloffset, 0} at end.
    template<typename EntryType>
    [[nodiscard]] std::pair<vm_offset_t, integer_t> next_impl(Trix *trx, vm_offset_t entry_offset, integer_t bucket_idx) {
        auto offset = nulloffset;

        if (bucket_idx == -1) {
            // bucket_idx signals first call, scan from bucket 0 for first entry
            bucket_idx = 0;
            for (dict_bucket_count_t idx = 0; idx < m_bucket_count; ++idx) {
                offset = m_buckets[idx];
                if (offset != nulloffset) {
                    bucket_idx = static_cast<integer_t>(idx);
                    break;
                }
            }
        } else if (entry_offset != nulloffset) {
            // scan forward from the last returned entry: O(1) pointer follow
            auto entry = trx->offset_to_ptr<EntryType>(entry_offset);
            offset = entry->m_next;

            if (offset == nulloffset) {
                // end of chain -- advance to next non-empty bucket using stored bucket_idx
                auto idx = static_cast<dict_bucket_count_t>(bucket_idx);
                while (++idx < m_bucket_count) {
                    offset = m_buckets[idx];
                    if (offset != nulloffset) {
                        bucket_idx = static_cast<integer_t>(idx);
                        break;
                    }
                }
            }
        }

        return std::pair{offset, bucket_idx};
    }

    [[nodiscard]] DictIterEntry next(Trix *trx, vm_offset_t entry_offset, integer_t bucket_idx) {
        auto [offset, idx] = next_impl<DictEntry>(trx, entry_offset, bucket_idx);
        if (offset != nulloffset) {
            auto entry = trx->offset_to_ptr<DictEntry>(offset);
            return DictIterEntry{offset, idx, entry->m_key, entry->m_value};
        } else {
            return DictIterEntry{nulloffset, 0, Object::make_null(), Object::make_null()};
        }
    }

    // Dict-stack push hook (called by `begin`, closure entry, etc.).
    //
    // Single-coroutine fast path: write-through each Name-keyed entry's
    // binding directly into Name::m_binding.  This matches the pre-per-
    // coroutine-cache behavior -- the new topmost dict's bindings are
    // cached immediately so the next lookup hits cache, and any stale
    // lower-stack cache entry for the same name is overwritten.
    //
    // Multi-coroutine path: invalidate each Name in the running coroutine's
    // table.  Invalidation (rather than write-through) keeps the per-
    // coroutine table small -- the next lookup walks the dict stack, finds
    // the new topmost binding, and populates lazily.  Only the running
    // coroutine is affected: dict-stack pushes don't reach peer stacks.
    void set_name_bindings(Trix *trx) {
        if (trx->m_live_coroutine_count == 0) {
            for (dict_bucket_count_t i = 0; i < m_bucket_count; ++i) {
                auto offset = m_buckets[i];
                while (offset != nulloffset) {
                    auto entry = trx->offset_to_ptr<DictEntry>(offset);
                    if (entry->m_key.is_name()) {
                        auto name = entry->m_key.name_value(trx);
                        name->set_binding(trx->ptr_to_offset(&entry->m_value));
                    }
                    offset = entry->m_next;
                }
            }
        } else {
            auto running = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
            if (running->m_binding_table != nulloffset) {
                for (dict_bucket_count_t i = 0; i < m_bucket_count; ++i) {
                    auto offset = m_buckets[i];
                    while (offset != nulloffset) {
                        auto entry = trx->offset_to_ptr<DictEntry>(offset);
                        if (entry->m_key.is_name()) {
                            trx->binding_table_remove(running->m_binding_table, entry->m_key.name_offset());
                        }
                        offset = entry->m_next;
                    }
                }
            }
        }
    }

    // Dict-stack pop hook (called by `end`, `restore`, @end-locals, etc.):
    // clears cached binding pointers into this dict so a popped DictEntry
    // slot (now pooled for reuse) isn't reachable via a stale cache entry.
    //
    // Single-coroutine fast path: walk the dict entries, null each Name's
    // m_binding directly.
    // Multi-coroutine: walk every live coroutine's table and drop any entry
    // whose value_offset falls in this dict's allocated range.  Conservative
    // on coroutine scope since the popped dict (e.g. userdict) may have
    // been reachable from peer actor stacks.
    void clear_name_bindings(Trix *trx) {
        if (trx->m_live_coroutine_count == 0) {
            for (dict_bucket_count_t i = 0; i < m_bucket_count; ++i) {
                auto offset = m_buckets[i];
                while (offset != nulloffset) {
                    auto entry = trx->offset_to_ptr<DictEntry>(offset);
                    if (entry->m_key.is_name()) {
                        entry->m_key.name_value(trx)->clear_binding();
                    }
                    offset = entry->m_next;
                }
            }
        } else {
            auto dict_offset = trx->ptr_to_offset(this);
            auto dict_end = static_cast<vm_offset_t>(dict_offset + vm_size());
            trx->binding_clear_all_coroutines_pointing_into(dict_offset, dict_end);
        }
    }
    // Returns the total VM bytes consumed by this dict or set:
    // header + buckets + entry pool.
    [[nodiscard]] vm_size_t vm_size() const {
        auto header_and_buckets = alloc_size(m_bucket_count);
        auto entry_size = is_set_data() ? sizeof(SetEntry) : sizeof(DictEntry);
        auto entry_pool = static_cast<vm_size_t>(m_maxlength * entry_size);
        return (header_and_buckets + entry_pool);
    }

    //===--- Set-specific methods ---===//
    // Sets store any hashable key.  SetEntry is 12 bytes (m_next + Object key).
    // These methods must only be called on Dict headers with SetFlag set.

    // Look up a key in the set.  Returns the SetEntry or nullptr.
    [[nodiscard]] SetEntry *find_set_entry(Trix *trx, Object key_obj) const {
        assert(is_set_data());

        auto hash = key_obj.hash(trx);
        return find_in_chain<SetEntry>(
                trx, key_obj, m_buckets[fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count)]);
    }

    // Test membership: is the key in the set?
    [[nodiscard]] bool set_member(Trix *trx, Object key_obj) const { return (find_set_entry(trx, key_obj) != nullptr); }

    // Copy all entries from src set into this set.
    void copy_set(Trix *trx, const Dict *src) {
        assert(is_set_data());
        assert(src->is_set_data());

        auto remaining = static_cast<length_t>(m_maxlength - m_length);
        if (src->m_length > remaining) {
            auto fail = !is_dynamic();
            if (!fail) {
                auto needed = static_cast<length_t>(src->m_length - remaining);
                auto [_, grow_size] = expand_set_size(needed);
                fail = !trx->vm_size_available<SetEntry>(grow_size);
            }
            if (fail) {
                trx->error(Error::DictFull,
                           "copy: source set length {} exceeds destination capacity {}",
                           src->m_length,
                           static_cast<length_t>(m_maxlength - m_length));
            }
        }
        for (dict_bucket_count_t i = 0; i < src->m_bucket_count; ++i) {
            auto offset = src->m_buckets[i];
            while (offset != nulloffset) {
                auto entry = trx->offset_to_ptr<SetEntry>(offset);
                set_put(trx, entry->m_key.make_clone(trx));
                offset = entry->m_next;
            }
        }
    }

    // Reset a set for reuse (eq-set pattern).  Clears all entries, restores pool.
    void reset_set(Trix *trx, length_t maxlength, Object::access_t obj_access) {
        assert(is_set_data());

        save(trx);

        m_maxlength = maxlength;
        m_access = ((obj_access == Object::ReadWriteAccess) ? ReadWriteAccess : ReadOnlyAccess) | SetFlag;
        m_dict_save_level = trx->m_curr_save_level;
        if (m_length != 0) {
            for (dict_bucket_count_t i = 0; i < m_bucket_count; ++i) {
                auto offset = m_buckets[i];
                auto null_obj = Object::make_null(trx->m_curr_save_level);
                while (offset != nulloffset) {
                    auto entry = trx->offset_to_ptr<SetEntry>(offset);
                    auto next = entry->m_next;

                    // free ExtValue (no name binding clear -- sets are not on dict stack)
                    entry->m_key.maybe_free_extvalue(trx);

                    entry->m_key = null_obj;
                    entry->m_next = m_pool;
                    m_pool = offset;
                    offset = next;
                }
                m_buckets[i] = nulloffset;
            }
            m_length = 0;
        }
    }

    // Add a key to the set.  No-op if already present.
    // Key must not be Null.  Transfers ExtValue ownership from caller.
    void set_put(Trix *trx, Object key_obj) {
        assert(is_set_data());
        assert(!key_obj.is_null());

        // R6 pointer hygiene: a global set clones a fragile-local SCALAR key into
        // global (late, before the move-store) and rejects a NON-scalar fragile-local
        // upfront (see Dict::put).
        auto container_is_global = is_global(trx);
        Save::reject_local_into_global(trx, container_is_global, key_obj, "set-add");

        auto is_eqset = (this == trx->m_eqset);

        if (find_set_entry(trx, key_obj) != nullptr) {
            key_obj.maybe_free_extvalue(trx);
        } else {
            if ((m_length == m_maxlength) && (m_maxlength != 0) && is_dynamic()) {
                save(trx);
                expand_set(trx);
            }

            if (m_length < m_maxlength) {
                // save header (m_pool, m_length, buckets) -- no-op if already saved above
                save(trx);

                // For String keys: copy the data BEFORE touching the pool -- the alloc can
                // throw VMFull, and if the entry had already been removed from the pool, a
                // caught VMFull would leave the pool permanently short by one entry.  Copy
                // into the container's region (see Dict::put / copy_string_key_into_region).
                if (key_obj.is_string()) {
                    key_obj = copy_string_key_into_region(trx, key_obj, container_is_global);
                }

                auto hash = key_obj.hash(trx);
                auto bucket_index = fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count);

                // get SetEntry from pool
                auto entry_offset = m_pool;
                auto entry = trx->offset_to_ptr<SetEntry>(entry_offset);
                m_pool = entry->m_next;

                // journal entry before mutation so restore can undo.
                // Check save level: if entry's key was written at the current save level
                // (or this is eq-set), the entry is already captured -- no journal needed.
                auto curr_save_level = trx->m_curr_save_level;
                if ((entry->m_key.save_level() != curr_save_level) && !is_eqset) {
                    Save::save_set_entry(trx, entry);
                }

                // Clone a fragile-local SCALAR key into global for a global set (String
                // keys already region-copied above; a non-String scalar fragile key is
                // cloned here).  No alloc/throw between here and the store -> GC-safe.
                key_obj = Save::clone_fragile_scalar_into_global(trx, container_is_global, key_obj);
                entry->m_key = key_obj;
                entry->m_key.set_save_level(curr_save_level);

                // add to head of bucket chain
                auto chain = &m_buckets[bucket_index];
                entry->m_next = *chain;
                *chain = entry_offset;

                ++m_length;
            } else {
                trx->error(Error::DictFull, "set fixed capacity of {} exceeded", m_maxlength);
            }
        }
    }

    // Like set_put, but allocates a fresh SetEntry in
    // global VM (rather than from the local-VM pool) so set-add-persist
    // can add new members at any save level.  Already-a-member is a
    // silent no-op (matching set_put).  The chain-head write into
    // m_buckets[bucket_index] is non-journaled, mirroring set_remove_persist's
    // unlink path -- safe when the set header itself outlives the save
    // scope (set at BASE, or global set).
    void set_add_persist_or_create(Trix *trx, Object key_obj) {
        assert(is_set_data());
        assert(!key_obj.is_null());

        // R6 pointer hygiene: a global set clones a fragile-local SCALAR key into
        // global and rejects a NON-scalar fragile-local upfront (see Dict::put).
        auto container_is_global = is_global(trx);
        Save::reject_local_into_global(trx, container_is_global, key_obj, "set-add-persist");

        if (find_set_entry(trx, key_obj) != nullptr) {
            // Already a member: matches set_put's silent no-op.
            key_obj.maybe_free_extvalue(trx);
        } else {
            // String keys: copy into a GC-tagged global String so they survive restore
            // (the -persist family creates permanent, journal-skipped members); a
            // non-String scalar fragile-local key is cloned into global.  Either way the
            // key is then GC-managed and rooted (oneoff below) across the entry alloc.
            if (key_obj.is_string()) {
                key_obj = copy_string_key_into_region(trx, key_obj, true);
            } else {
                key_obj = Save::clone_fragile_scalar_into_global(trx, container_is_global, key_obj);
            }

            auto hash = key_obj.hash(trx);
            auto bucket_index = fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count);

            // The entry is a standalone ChunkKind::HashEntry block (not part of the
            // set's pool); Dict::gc_walk_contents back-marks it via the bucket chain.
            // Root the (now GC-managed) key across the entry alloc, which can fire
            // vm_global_gc before the entry references the fresh global String.
            trx->gc_root_push_oneoff(key_obj);
            auto [new_entry, new_offset] = trx->gvm_alloc<SetEntry>(sizeof(SetEntry), Trix::ChunkKind::HashEntry);
            trx->gc_root_pop_oneoff();
            new_entry->m_key = key_obj;
            new_entry->m_key.set_save_level(Save::BASE);
            new_entry->m_next = m_buckets[bucket_index];

            // Non-journaled chain-head write + length bump.  Persists across
            // restore as long as the set header itself does (BASE or global).
            m_buckets[bucket_index] = new_offset;
            ++m_length;
        }
    }

    // Non-journaled set-remove for the -persist family.  Mirrors set_remove
    // but skips every Save:: call -- the removal must persist across the
    // enclosing save/restore.  No allocation, so no above-barrier hazard:
    // the bucket-chain unlink and freelist-push only reorganize existing
    // slots that were allocated when the set was created.  Errors with
    // /undefined if the key is not present (matching set_remove).
    void set_remove_persist(Trix *trx, Object *key_ptr) {
        assert(is_set_data());

        if (key_ptr->is_null()) {
            trx->error(Error::TypeCheck, "set-remove-persist: key must not be null");
        } else {
            auto hash = key_ptr->hash(trx);
            auto bucket_index = fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count);
            auto prev_chain = &m_buckets[bucket_index];
            auto offset = *prev_chain;

            while (offset != nulloffset) {
                auto entry = trx->offset_to_ptr<SetEntry>(offset);
                if (key_ptr->equal(trx, entry->m_key)) {
                    // No journaling: free ExtValues immediately and reset directly.
                    entry->m_key.maybe_free_extvalue(trx);
                    key_ptr->maybe_free_extvalue(trx);

                    // Unlink from bucket chain.
                    *prev_chain = entry->m_next;

                    // Return entry to pool.  Stamp BASE on the cleared slot so
                    // a future set_put reusing this freelist entry doesn't see
                    // a stale save_level above the current one and try to
                    // journal at a vanished level -- see Dict::put_persist for
                    // the same fix.
                    entry->m_key = Object::make_null(Save::BASE);
                    entry->m_next = m_pool;
                    m_pool = offset;

                    --m_length;
                    return;
                } else {
                    prev_chain = &entry->m_next;
                    offset = entry->m_next;
                }
            }
            trx->error(Error::Undefined, "set-remove-persist: key not found");
        }
    }

    // Remove a key from the set.  Error if not found.
    // Frees the lookup key's ExtValue.  The stored entry's ExtValue is freed
    // only if at the current save level (otherwise journaled for restore).
    void set_remove(Trix *trx, Object *key_ptr) {
        assert(is_set_data());

        if (key_ptr->is_null()) {
            trx->error(Error::TypeCheck, "set-remove: key must not be null");
        } else {
            auto is_eqset = (this == trx->m_eqset);
            auto hash = key_ptr->hash(trx);
            auto bucket_index = fastmod_u32(hash, bucket_magic_for(m_bucket_count), m_bucket_count);
            auto prev_chain = &m_buckets[bucket_index];
            auto offset = *prev_chain;
            SetEntry *prev_entry = nullptr;

            while (offset != nulloffset) {
                auto entry = trx->offset_to_ptr<SetEntry>(offset);
                if (key_ptr->equal(trx, entry->m_key)) {
                    // save header (m_pool, m_length, buckets)
                    save(trx);

                    auto curr_save_level = trx->m_curr_save_level;

                    // check if this entry has been saved at the current save level
                    if ((entry->m_key.save_level() == curr_save_level) || is_eqset) {
                        entry->m_key.maybe_free_extvalue(trx);
                    } else {
                        Save::save_set_entry(trx, entry);
                    }

                    // save predecessor's m_next if it predates current save level
                    // (same logic as Dict::undef -- if prev was put at curr level its
                    // full snapshot already captured m_next)
                    if ((prev_entry != nullptr) && (prev_entry->m_key.save_level() != curr_save_level) && !is_eqset) {
                        Save::save_set_entry_next(trx, prev_entry);
                    }

                    // free the lookup key's ExtValue
                    key_ptr->maybe_free_extvalue(trx);

                    // unlink from bucket chain
                    *prev_chain = entry->m_next;

                    // return to pool
                    entry->m_key = Object::make_null(curr_save_level);
                    entry->m_next = m_pool;
                    m_pool = offset;

                    --m_length;
                    return;
                } else {
                    prev_entry = entry;
                    prev_chain = &entry->m_next;
                    offset = entry->m_next;
                }
            }
            trx->error(Error::Undefined, "set-remove: key not found");
        }
    }

    struct SetIterEntry {
        vm_offset_t next_offset;
        integer_t next_bucket;
        Object element;
    };

    // Iterate over set entries.  Returns {next_offset, bucket_idx, key}.
    // First call: entry_offset = nulloffset, bucket_idx = -1.
    // End: returned next_offset == nulloffset.
    [[nodiscard]] SetIterEntry set_next(Trix *trx, vm_offset_t entry_offset, integer_t bucket_idx) {
        assert(is_set_data());

        auto [offset, idx] = next_impl<SetEntry>(trx, entry_offset, bucket_idx);
        if (offset != nulloffset) {
            auto entry = trx->offset_to_ptr<SetEntry>(offset);
            return SetIterEntry{offset, idx, entry->m_key};
        } else {
            return SetIterEntry{nulloffset, static_cast<integer_t>(0), Object::make_null()};
        }
    }
private:
    // 16 bytes + (vm_offset_t * bucket_count) + (sizeof(DictEntry/SetEntry) * m_maxLength)
    [[nodiscard]] static vm_size_t alloc_size(dict_bucket_count_t bucket_count) {
        auto dict_size = offsetof(Dict, m_buckets);
        auto bucket_size = (bucket_count * sizeof(vm_offset_t));
        return static_cast<vm_size_t>(dict_size + bucket_size);
    }

    // the result is often discarded when populating dicts, hence the lack of [[nodiscard]]
    Object *put(Trix *trx, SystemName systemname_key, Object val_obj, BindingMode mode = BindingMode::NoBinding) {
        return put(trx, Name::make_system(trx, systemname_key), val_obj, mode);
    }

    // m_buckets[1] is a flexible-array anchor.  Dict is allocated in VM as:
    //   sizeof(Dict header fields) + bucket_count*sizeof(vm_offset_t) + maxlength*sizeof(DictEntry)
    // The single declared element gives offsetof(Dict, m_buckets) a stable address used by
    // alloc_size().  Actual bucket array has m_bucket_count entries; DictEntry pool follows.

    vm_offset_t m_pool;                  // next available DictEntry in pool
    length_t m_length;                   // number of DictEntry defined
    length_t m_maxlength;                // dictionary capacity
    dict_bucket_count_t m_bucket_count;  // number of hash buckets
    uint8_t m_access;                    // Dict-local flag byte (see bit layout above)
    save_level_t m_dict_save_level;      // save level
    vm_offset_t m_next_in_visit;         // GC visit-list link (nulloffset = off-list, VisitListEnd = end-of-list,
                                         // otherwise = offset of next visited Dict/Set).  Used only during the
                                         // global GC mark phase to track local-VM Dict/Set containers visited from
                                         // global walks (cycle-break + work queue), removing the dependency on the
                                         // capped m_gc_local_visited array for these dominant types.  See
                                         // gc.inl::gc_visit_push / gc_visit_pop.  Snapshot/thaw: must always be
                                         // nulloffset at snapshot time (GC is not in-progress).
    vm_offset_t m_buckets[1];            // hash buckets[m_bucket_count]
                                         // DictEntry m_entries[m_maxlength];

    [[nodiscard]] vm_offset_t *buckets() { return m_buckets; }
    [[nodiscard]] const vm_offset_t *buckets() const { return m_buckets; }
public:
    //
    // GC mark-phase walker for ChunkKind::Dict and ChunkKind::Set
    // blocks.  Both kinds share the Dict on-disk layout -- a Dict
    // header followed by a bucket array followed by a pool of
    // DictEntry (Dict mode) or SetEntry (Set mode) records.  The
    // SetFlag bit in m_access selects which record layout is in use.
    //
    // Walks each bucket's chained entries and calls gc_mark_object on
    // every key (Dict + Set) and every value (Dict only).
    //
    // Auxiliary expansion-pool blocks: a dynamic global Dict that
    // outgrows its initial pool (expand_dict / expand_set) gets a
    // SEPARATE global-VM block holding the new entry slots.  Bucket-
    // chain heads and the m_pool free-list head can point mid-block
    // into those expansion blocks -- they are NOT payload-start
    // offsets, so mark_global_offset cannot mark them directly.
    // Without an extra walk, a vm-global-gc pass after expansion
    // would sweep the expansion block while the Dict header still
    // references entries inside it (silent corruption on next put).
    // For every entry we visit, we range-check its offset against the
    // Dict's own block (entries in the initial pool are already
    // marked by the Dict's own block-level mark) and back-walk via
    // gvm_find_owning_payload to the expansion block when outside.
    // The mark-gen short-circuit ensures each expansion block is
    // back-walked at most once per GC pass.  We also walk the m_pool
    // free-list to catch the all-free expansion-block case (Dict
    // expanded, then every entry from the new block was put + then
    // removed -- no bucket-chain entry references it any more).
    //
    // Why traverse buckets and not the entry pool directly: free
    // entries in the pool are not zeroed; their m_key / m_value
    // bytes hold whatever was last written before they were freed.
    // Bucket traversal touches only live entries.  (gc_mark_object's
    // slack-noise tolerance would catch the alternative anyway, but
    // bucket traversal is the natural canonical iteration -- mirrors
    // quiet_flush() and the rest of the Dict iteration sites.)
    //
    static void gc_walk_contents(Trix *trx, vm_offset_t payload_offset) {
        auto *dict = trx->offset_to_ptr<Dict>(payload_offset);
        // Short-circuit when the dict is flagged as guaranteed-no-global-refs
        // (set on systemdict + every init-time readonly sub-dict).  Saves
        // ~700 entry-walks per GC pass.  Invariant: NoGlobalRefs is set
        // ONLY at create time when m_curr_alloc_global is false AND access
        // is ReadOnly, so the dict's contents are immutable + local-only.
        if (!dict->has_no_global_refs()) {
            auto is_set = dict->is_set_data();

            // Range used to skip back-walks for entries in the Dict's own
            // block (already covered by the Dict's mark).  Only meaningful
            // for global Dicts: local-VM Dicts are never swept by GC, so
            // their expansion blocks (also local-VM) don't need a walker.
            auto dict_is_global = trx->is_global(payload_offset);
            vm_offset_t dict_payload_end = nulloffset;
            if (dict_is_global) {
                auto block_size = trx->gvm_get_block_size(payload_offset);
                dict_payload_end = static_cast<vm_offset_t>(payload_offset + trx->gvm_get_payload_size(block_size));
            }
            auto mark_aux_if_external = [&](vm_offset_t entry_offset) {
                if (dict_is_global && ((entry_offset < payload_offset) || (entry_offset >= dict_payload_end))) {
                    // Entries outside the Dict's own block are standalone ChunkKind::HashEntry
                    // blocks: expansion-pool blocks (expand_dict / expand_set) AND -persist
                    // entries (put_persist_or_create / set_add_persist_or_create).  They are
                    // reachable ONLY through this bucket / free-list chain, so we mark the
                    // owning block here or a vm_global_gc pass sweeps it out from under the
                    // live Dict header.  HashEntry is a GC LEAF -- the entries' key/value are
                    // marked by the bucket walk below; the block just needs to survive the
                    // sweep (and must NOT be tagged Dict/Set, or it gets re-walked as a header
                    // and mis-reads an entry's m_key/m_value bytes as bucket offsets).
                    auto owning = trx->gvm_find_owning_payload(entry_offset, ChunkKind::HashEntry);
                    if (owning != nulloffset) {
                        trx->mark_global_offset(owning);
                    }
                }
            };

            for (auto i = dict_bucket_count_t{0}; i < dict->m_bucket_count; ++i) {
                auto offset = dict->m_buckets[i];
                while (offset != nulloffset) {
                    mark_aux_if_external(offset);
                    if (is_set) {
                        auto *entry = trx->offset_to_ptr<SetEntry>(offset);
                        trx->gc_mark_object(entry->m_key);
                        offset = entry->m_next;
                    } else {
                        auto *entry = trx->offset_to_ptr<DictEntry>(offset);
                        trx->gc_mark_object(entry->m_key);
                        trx->gc_mark_object(entry->m_value);
                        offset = entry->m_next;
                    }
                }
            }

            // Free-list walk: handles expansion blocks where every entry is
            // back on the pool (put-then-remove cycle leaves no bucket-chain
            // reference).  No Object marking needed -- free entries hold
            // null Objects -- we just need to discover the owning blocks.
            if (dict_is_global) {
                auto free_offset = dict->m_pool;
                while (free_offset != nulloffset) {
                    mark_aux_if_external(free_offset);
                    if (is_set) {
                        auto *entry = trx->offset_to_ptr<SetEntry>(free_offset);
                        free_offset = entry->m_next;
                    } else {
                        auto *entry = trx->offset_to_ptr<DictEntry>(free_offset);
                        free_offset = entry->m_next;
                    }
                }
            }
        }
    }

    //
    // GC pre-mark helper: walk the m_pool-threaded overflow chain
    // rooted at *head_link and excise nodes whose offset is global.
    // Used by vm_global_gc_impl to drop globally-allocated frame
    // dicts that would dangle after sweep (the chain head and link
    // pointers are not GC roots).  Local-VM nodes stay in the chain.
    //
    // Encapsulated here because m_pool is private; callers (Trix
    // member fns) cannot reach into the chain directly.
    //
    static void gc_drain_global_from_overflow(Trix *trx, vm_offset_t *head_link) {
        auto *prev_link = head_link;
        while (*prev_link != nulloffset) {
            auto offset = *prev_link;
            auto *dict = trx->offset_to_ptr<Dict>(offset);
            if (trx->is_global(offset)) {
                *prev_link = dict->m_pool;
            } else {
                prev_link = &dict->m_pool;
            }
        }
    }
};
static_assert(sizeof(Dict) == 20);
