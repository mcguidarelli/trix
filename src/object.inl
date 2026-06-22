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
// Forward declarations for Object accessor return types (defined in later .inl files).
// These must be in `private:` context (inherited from vm_heap.inl) to match the
// access level of the actual struct definitions in ops_record/coroutine/pipeline/reactive.
struct RecordInstance;
struct CoroutineContext;
struct PipeBufferHeader;
struct CellHeader;
struct ContinuationContext;
public:
class Dict;
class Name;
class Save;
class Stream;

//===--- Object: 8-byte Tagged-Union Value Type ---===//
//
// The fundamental value type of the Trix VM.  Every value on every stack,
// in every array, dict entry, and VM structure is an Object.  Based on:
//
//   PostScript objects: typed values with attribute bits (literal/executable,
//   read-only/read-write).  The PostScript execution model dispatches
//   entirely by Object type and attributes.
//
//   Tagged pointer / tagged union techniques from Lua, Ruby (MRI), and
//   SpiderMonkey: encoding type information and small values in a compact
//   fixed-size representation to avoid heap allocation for common types.
//
// --- Core concepts for maintainers ---
//
// 8-BYTE LAYOUT
//   Every Object is exactly 8 bytes, enabling dense packing on stacks
//   and in arrays with no padding.  The layout is:
//
//     Bytes 0-3: HEADER
//       [0]   aat_t    -- attribute byte: type (5 bits), special flag,
//                        access mode (RO/RW), execute attribute (lit/exec)
//       [1]   save_level_t -- save level for journal tracking
//       [2-3] length/flags -- type-specific: string length, array length,
//                            operator index, dict bucket count, etc.
//
//     Bytes 4-7: VALUE
//       Inline types (fit in 32 bits): Null, Byte, Integer, UInteger,
//         Real, Boolean, Operator, Mark -- value stored directly.
//         Save tokens are also inline Integers (level | gen-xor-barrier,
//         packed); they are not a distinct type.
//       Offset types (reference heap data): Long, ULong, Address, Double,
//         Int128, UInt128, Name, String, Array, Packed, Dict, Stream,
//         Curry, Thunk, Tagged, Record, Coroutine, PipeBuffer, Cell, Set,
//         Continuation, OpaqueHandle -- vm_offset_t pointing to heap-
//         resident storage.
//       Special: SourceLoc reuses save_level byte as stream_id, the length
//         field as source column/pos, and the value field as source line
//         (internal only, never user-visible).
//
// EXTVALUE / WIDEVALUE (64- and 128-bit extensions)
//   Types whose native value requires 64 bits (Long, ULong, Double,
//   Address) store their value in an ExtValue: an 8-byte slot on the VM
//   heap managed by a free list.  Types requiring 128 bits (Int128,
//   UInt128) use a parallel WideValue: a 16-byte slot with its own free
//   list.  The Object's VALUE field holds the vm_offset_t of the
//   ExtValue or WideValue.
//
//   ExtValues have save-level-aware lifecycle: they are journaled on
//   mutation and freed on restore.  make_clone() allocates a new ExtValue
//   (independent copy); maybe_free_extvalue() returns it to the free list.
//   Failure to free an ExtValue is a resource leak; double-free corrupts
//   the free list.  The active count (m_extvalue_active_count) tracks
//   live ExtValues for leak detection.
//
// TYPE SYSTEM (31 types, 5-bit encoding, 32 slots, 1 remaining)
//   Null, Byte, Integer, UInteger, Long, ULong, Address, Real, Double,
//   Boolean, Operator, Mark, Name, Array, Packed, String, Stream, Dict,
//   SourceLoc, Curry, Thunk, Set, Tagged, Record, Coroutine, PipeBuffer,
//   Cell, Continuation, Int128, UInt128, OpaqueHandle, + 1 reserved slot.
//
// ATTRIBUTE BYTE (aat_t)
//   Bit 7 (X): Execute attribute -- 0 = Literal (data), 1 = Executable.
//     Literal objects push to the operand stack; executable objects are
//     dispatched by type (operators call C++, names are looked up, procs
//     iterate their bodies, etc.).
//   Bit 6 (W): Access mode -- 0 = ReadOnly, 1 = ReadWrite.
//     Controls mutability of strings, arrays, and dicts.
//   Bit 5 (F): Special flag -- type-specific.  F holds the address-probe
//     cache state for Address, and an eqref marker for
//     Array/Packed/String/Dict/Set; it is unused (must be 0) for all other
//     types (Boolean stores its value in the VALUE word, not F).  (See the
//     per-type breakdown on the aat_t diagram below.)
//   Bits 0-4 (T): Type tag -- 31 defined types.
//
// SAVE LEVEL
//   Every Object carries a save_level_t (byte 1) recording the save level
//   at which it was created or last modified.  This is the backbone of the
//   save/restore journaling system: on restore, objects with a save level
//   higher than the restore target are rolled back.
//
// MEMORY LAYOUT
//
//   Object (8 bytes):
//   +---+---+---+---+---+---+---+---+
//   | 0 | 1 | 2   3 | 4   5   6   7 |
//   +---+---+---+---+---+---+---+---+
//   |aat|slv| length | value/offset  |
//   +---+---+---+---+---+---+---+---+
//     |   |     |          |
//     |   |     |          +-- Inline: Null, Byte, Integer, UInteger,
//     |   |     |              Real, Boolean, Operator, Mark (32-bit)
//     |   |     |              (save tokens are inline Integers).
//     |   |     |              Offset: vm_offset_t to heap data for
//     |   |     |              Long, ULong, Double, Address, Name,
//     |   |     |              String, Array, Packed, Dict, Stream,
//     |   |     |              Curry, Thunk, Tagged, Record, Coroutine,
//     |   |     |              PipeBuffer, Cell, Set, Continuation,
//     |   |     |              Int128, UInt128, OpaqueHandle
//     |   |     |
//     |   |     +-- Type-specific: string length, array length,
//     |   |         operator index, dict bucket count, etc.
//     |   |
//     |   +-- save_level_t: save level for journal tracking
//     |
//     +-- aat_t: type (5 bits) + special (1) + access (1) + execute (1)
//
//   ExtValue (8 bytes, heap-resident):
//   +---+---+---+---+---+---+---+---+
//   | 0   1   2   3 | 4   5   6   7 |
//   +---+---+---+---+---+---+---+---+
//   | free-list next| dead sentinel |
//   |   (m_next)    | (m_free_      |
//   +---+---+---+---+   sentinel)   +
//   |  64-bit value (whole union)   |
//   +---+---+---+---+---+---+---+---+
//   Live: the full 8-byte union holds the 64-bit datum (Long, ULong,
//         Double, Address); m_free_sentinel reads as something other than
//         the slot's dead value.
//   Free: m_next (bytes 0-3) holds the free-list next offset; m_free_sentinel
//         (bytes 4-7) holds (offset ^ sm_dead_sentinel_magic), with
//         sm_dead_sentinel_magic == 0xDEADBEEF.
//
// OWNERSHIP AND CLONING
//   Objects are value types (8 bytes, copyable).  However, offset-type
//   Objects reference shared heap data.  Copying an Object copies the
//   offset, creating a shared reference.  make_clone() creates an
//   independent copy (allocating new ExtValues, heap data as needed).
//   Containers (arrays, dicts) must clone elements when copying between
//   containers to preserve ExtValue ownership invariants.
//
// Object is exactly 8 bytes: a 4-byte HEADER (aat, save_level, and a type-specific
// length/flags field) followed by a 4-byte VALUE.  Types whose native value fits in
// 32 bits (Null, Byte, Integer, UInteger, Real, Boolean, Operator, Mark) are stored inline.
// Types whose value requires 64 bits (Long, ULong, Address, Double) cannot fit in the
// 4-byte VALUE slot and instead store a vm_offset_t pointing to an ExtValue in VM
// heap memory.  Name, String, Array, Packed, Dict, Stream similarly use the
// VALUE slot as a vm_offset_t referencing their heap-resident storage.
class Object {
public:
    // aat_t -- Object Attribute Byte
    //  7 6 5 4 3 2 1 0
    // +-+-+-+-+-+-+-+-+
    // |X|W|F|T|T|T|T|T|
    // +-+-+-+-+-+-+-+-+
    //  | | | \       /
    //  | | |  ---+---
    //  | | |     |
    //  | | |     +----- T:     5 bits
    //  | | |                   00000: Null
    //  | | |                   00001: Byte
    //  | | |                   00010: Integer
    //  | | |                   00011: UInteger
    //  | | |                   00100: Long
    //  | | |                   00101: ULong
    //  | | |                   00110: Address
    //  | | |                   00111: Real
    //  | | |                   01000: Double
    //  | | |                   01001: Boolean
    //  | | |                   01010: Operator
    //  | | |                   01011: Mark
    //  | | |                   01100: Name
    //  | | |                   01101: Array
    //  | | |                   01110: Packed
    //  | | |                   01111: String
    //  | | |                   10000: Stream
    //  | | |                   10001: Dict
    //  | | |                   10010: SourceLoc
    //  | | |                   10011: Curry
    //  | | |                   10100: Thunk
    //  | | |                   10101: Set
    //  | | |                   10110: Tagged
    //  | | |                   10111: Record
    //  | | |                   11000: Coroutine
    //  | | |                   11001: PipeBuffer
    //  | | |                   11010: Cell
    //  | | |                   11011: Continuation
    //  | | |                   11100: Int128
    //  | | |                   11101: UInt128
    //  | | |                   11110: OpaqueHandle
    //  | | +----------- F: per-type flag (see below); constant SpecialFlag = 0x20
    //  | +------------- W: 0 = ReadOnly, 1 = ReadWrite
    //  +--------------- X: 0 = Literal,  1 = Executable
    //
    //  The F bit is polymorphic -- its meaning depends on Type:
    //
    //  Boolean (T=01001):
    //    F is unused (must be 0).  The Boolean value (0 = false, 1 = true)
    //    is stored in the VALUE word (m_boolean), not in the F bit.
    //
    //  Address (T=00110):
    //    F combines with X to encode a 4-state address-probe cache:
    //      F=0, X=0   uncached (must probe via address_state)
    //      F=0, X=1   cached invalid/null
    //      F=1, X=0   cached read-only
    //      F=1, X=1   cached read-write
    //
    //  Array/Packed/String/Dict/Set (T=01101/01110/01111/10001/10101):
    //    F set marks the Object as an "eqref" -- a reference into one of the
    //    per-kind shared eq-storage buffers created by the `#=` suffix or the
    //    =string/=array/=dict runtime operators.  When F is set, the value slot
    //    is read through the m_generation union alias (a uint32_t creation-time
    //    counter) rather than as a VM offset.  Every access routes through a
    //    resolver (array_objects, packed_data_ptr, string_data_ptr, dict_value,
    //    set_value) that validates m_generation against the current per-kind
    //    counter and raises Unsupported on mismatch.  Per-kind disambiguation
    //    uses the (type, attrib) pair: executable Array or Packed + F is
    //    is_eqproc_ref; literal Array + F is is_eqarray_ref; String + F is
    //    is_eqstring_ref; Dict + F is is_eqdict_ref; Set + F is is_eqset_ref.
    //    make_packed_data routes eqrefs through the PackedExt slot using one
    //    subcode per (kind, access, length-class) tuple plus the source
    //    object's generation counter, so the staleness check propagates
    //    transparently across packing -- see emit_packed_element.
    //
    //  All other types:
    //    F is unused and must be 0.
    using aat_t = uint8_t;
    using attrib_t = aat_t;
    using access_t = aat_t;
    using type_t = aat_t;

    // Attributes
    static constexpr attrib_t LiteralAttrib{0x00};
    static constexpr attrib_t ExecutableAttrib{0x80};
    static constexpr attrib_t AttribMask{0x80};

    [[nodiscard]] attrib_t attrib() const { return (m_aat & AttribMask); }

    // Raw attribute byte: type (bits 0-4), special flag (5), access (6), execute (7).
    // Useful in tight loops that want to fetch once and extract multiple fields.
    [[nodiscard]] aat_t aat_raw() const { return m_aat; }

    [[nodiscard]] bool is_literal() const { return ((m_aat & AttribMask) == LiteralAttrib); }

    [[nodiscard]] bool is_executable() const { return ((m_aat & AttribMask) == ExecutableAttrib); }

    void set_literal() { m_aat = ((m_aat & ~AttribMask) | LiteralAttrib); }

    void set_executable() { m_aat = ((m_aat & ~AttribMask) | ExecutableAttrib); }

    void set_attrib(attrib_t attrib) { m_aat = ((m_aat & ~AttribMask) | attrib); }

    // Access
    static constexpr access_t ReadOnlyAccess{0x00};
    static constexpr access_t ReadWriteAccess{0x40};
    static constexpr access_t AccessMask{0x40};

    [[nodiscard]] access_t access() const {
        assert(has_object_access());

        return (m_aat & AccessMask);
    }

    [[nodiscard]] bool has_readonly_access() const {
        assert(has_object_access());

        return ((m_aat & AccessMask) == ReadOnlyAccess);
    }

    [[nodiscard]] bool has_write_access() const {
        assert(has_object_access());

        return ((m_aat & AccessMask) == ReadWriteAccess);
    }

    void set_readonly_access() { m_aat = ((m_aat & ~AccessMask) | ReadOnlyAccess); }

    enum struct DictMode { ReadOnly, ReadWriteFixed, ReadWriteDynamic };

    // Object attribute
    static constexpr aat_t SpecialFlag{0x20};

    // Types
    enum struct Type : type_t {
        Null,          // 00000
        Byte,          // 00001
        Integer,       // 00010
        UInteger,      // 00011
        Long,          // 00100
        ULong,         // 00101
        Address,       // 00110
        Real,          // 00111
        Double,        // 01000
        Boolean,       // 01001
        Operator,      // 01010
        Mark,          // 01011
        Name,          // 01100
        Array,         // 01101
        Packed,        // 01110
        String,        // 01111
        Stream,        // 10000
        Dict,          // 10001
        SourceLoc,     // 10010 -- internal call-site location; never user-visible
        Curry,         // 10011 -- partial application: value + callable
        Thunk,         // 10100 -- lazy evaluation: state + proc + cached result
        Set,           // 10101 -- unordered collection of unique names (Dict without values)
        Tagged,        // 10110 -- discriminated value: [Name tag, Object payload]
        Record,        // 10111 -- immutable named-field composite [schema, Object[0..N-1]]
        Coroutine,     // 11000 -- cooperative coroutine handle [CoroutineContext in VM]
        PipeBuffer,    // 11001 -- bounded buffer for inter-coroutine pipelines [PipeBufferHeader in VM]
        Cell,          // 11010 -- reactive cell: base value or computed derivation [CellHeader in VM]
        Continuation,  // 11011 -- one-shot delimited continuation [ContinuationContext in VM]
        Int128,        // 11100 -- signed 128-bit integer [WideValue in VM]
        UInt128,       // 11101 -- unsigned 128-bit integer [WideValue in VM]
        OpaqueHandle   // 11110 -- sub-typed by HandleKind (Screen, future Tilemap/Sample/...).
                       //          attrib_index() expands the sm_object_attrib table so each kind has its own row.
    };
    static constexpr type_t TypeCount{+Type::OpaqueHandle + 1};
    static constexpr type_t TypeMask{0x1F};

    // HandleKind: sub-type discriminator for Type::OpaqueHandle.  Stored in the
    // Object's existing m_length slot (length_t, 2 bytes) so no new union member
    // is needed and type checks remain a single bit-pattern comparison without
    // touching the underlying state struct.  sm_object_attrib has a per-kind
    // row at offset TypeCount + kind; future kinds (Tilemap, Sample, ...) just
    // append a kind value here and a row in the table.
    using handle_kind_t = length_t;
    enum struct HandleKind : handle_kind_t {
        Screen = 0,  // terminal screen surface: cells + prev + diff render
                     // future: Tilemap, Sample, ...
    };
    static constexpr handle_kind_t HandleKindCount{+HandleKind::Screen + 1};

    [[nodiscard]] static constexpr std::string_view type_sv(Type typ) {
        using namespace std::literals::string_view_literals;

        switch (typ) {
        case Type::Null:
            return "null-type"sv;

        case Type::Byte:
            return "byte-type"sv;

        case Type::Integer:
            return "integer-type"sv;

        case Type::UInteger:
            return "uinteger-type"sv;

        case Type::Long:
            return "long-type"sv;

        case Type::ULong:
            return "ulong-type"sv;

        case Type::Address:
            return "address-type"sv;

        case Type::Real:
            return "real-type"sv;

        case Type::Double:
            return "double-type"sv;

        case Type::Boolean:
            return "boolean-type"sv;

        case Type::Operator:
            return "operator-type"sv;

        case Type::Mark:
            return "mark-type"sv;

        case Type::Name:
            return "name-type"sv;

        case Type::Array:
            return "array-type"sv;

        case Type::Packed:
            return "packed-type"sv;

        case Type::String:
            return "string-type"sv;

        case Type::Stream:
            return "stream-type"sv;

        case Type::Dict:
            return "dict-type"sv;

        case Type::SourceLoc:
            return "sourceloc-type"sv;

        case Type::Curry:
            return "curry-type"sv;

        case Type::Thunk:
            return "thunk-type"sv;

        case Type::Set:
            return "set-type"sv;

        case Type::Tagged:
            return "tagged-type"sv;

        case Type::Record:
            return "record-type"sv;

        case Type::Coroutine:
            return "coroutine-type"sv;

        case Type::PipeBuffer:
            return "pipebuffer-type"sv;

        case Type::Cell:
            return "cell-type"sv;

        case Type::Continuation:
            return "continuation-type"sv;

        case Type::Int128:
            return "int128-type"sv;

        case Type::UInt128:
            return "uint128-type"sv;

        case Type::OpaqueHandle:
            return "opaque-handle-type"sv;

        default:
            assert(false && "type_sv: unknown Type");
            std::unreachable();
        }
    }

    [[nodiscard]] static constexpr std::string_view type_suffix_sv() {
        using namespace std::literals::string_view_literals;

        return "-type"sv;
    }

    [[nodiscard]] Type type() const { return static_cast<Type>(m_aat & TypeMask); }

    void set_type(Type type) { m_aat = ((m_aat & ~TypeMask) | +type); }

    // Object information based on type
    [[nodiscard]] bool ignores_execute() const { return ((sm_object_attrib[attrib_index()] & IgnoresExecute) != 0); }

    [[nodiscard]] bool honors_execute() const { return ((sm_object_attrib[attrib_index()] & IgnoresExecute) == 0); }

    [[nodiscard]] bool has_access() const { return ((sm_object_attrib[attrib_index()] & (HasObjectAccess | HasValueAccess)) != 0); }

    [[nodiscard]] bool has_object_access() const { return ((sm_object_attrib[attrib_index()] & HasObjectAccess) != 0); }

    [[nodiscard]] bool has_length() const { return ((sm_object_attrib[attrib_index()] & (HasObjectLength | HasValueLength)) != 0); }

    [[nodiscard]] bool has_object_length() const { return ((sm_object_attrib[attrib_index()] & HasObjectLength) != 0); }

    [[nodiscard]] bool has_value_length() const { return ((sm_object_attrib[attrib_index()] & HasValueLength) != 0); }

    [[nodiscard]] bool uses_vm() const { return ((sm_object_attrib[attrib_index()] & UsesVM) != 0); }

    [[nodiscard]] bool uses_extvalue() const { return ((sm_object_attrib[attrib_index()] & UsesExtValue) != 0); }

    // Int128/UInt128 back their value with a WideValue (16-byte heap cell, own free list).
    // Direct Type check keeps the object_attrib_t table at uint16_t (all 16 bits used).
    [[nodiscard]] bool uses_widevalue() const {
        auto t = type();
        return ((t == Type::Int128) || (t == Type::UInt128));
    }

    // Unified heap-cell predicates.  ExtValue (Long/ULong/Double/Address; 8 bytes)
    // and WideValue (Int128/UInt128; 16 bytes) share the same ownership story:
    // the Object carries a vm_offset_t into an independently-managed heap with
    // per-save-level free lists and sentinel-based validity checks.  Most call
    // sites that care about one also care about the other; forgetting the
    // WideValue branch is a live source of regressions.  These helpers let
    // callers write a single check.
    [[nodiscard]] bool uses_heap_cell() const { return (uses_extvalue() || uses_widevalue()); }

    // Precondition: uses_heap_cell().  True iff the heap cell is still live
    // (not on the free list, offset within bounds).
    [[nodiscard]] bool heap_cell_valid(Trix *trx) const {
        assert(uses_heap_cell());

        return (uses_extvalue() ? ExtValue::valid(trx, m_offset) : WideValue::valid(trx, m_offset));
    }

    // True iff this Object has a heap cell AND that cell is invalid (freed or
    // never populated).  Safe on any Object -- returns false for types that
    // don't use a heap cell at all.  Use as the standard "skip this, its
    // backing is gone" guard in cleanup loops over scratch regions that may
    // have been repurposed by error recovery.
    [[nodiscard]] bool heap_cell_freed(Trix *trx) const { return (uses_heap_cell() && !heap_cell_valid(trx)); }

    [[nodiscard]] bool pushop_direct() const { return ((sm_object_attrib[attrib_index()] & PushOpDirect) != 0); }

    [[nodiscard]] bool needs_exec_slot() const { return ((sm_object_attrib[attrib_index()] & NeedsExecSlot) != 0); }

    [[nodiscard]] bool is_signed_integral() const { return ((sm_object_attrib[attrib_index()] & IsSignedIntegral) != 0); }

    [[nodiscard]] bool is_unsigned_integral() const { return ((sm_object_attrib[attrib_index()] & IsUnsignedIntegral) != 0); }

    [[nodiscard]] bool is_integral() const {
        return ((sm_object_attrib[attrib_index()] & (IsSignedIntegral | IsUnsignedIntegral)) != 0);
    }

    [[nodiscard]] bool is_floating_point() const { return ((sm_object_attrib[attrib_index()] & IsFloatingPoint) != 0); }

    [[nodiscard]] bool is_signed_number() const {
        return ((sm_object_attrib[attrib_index()] & (IsSignedIntegral | IsFloatingPoint)) != 0);
    }

    [[nodiscard]] bool is_number() const {
        return ((sm_object_attrib[attrib_index()] & (IsSignedIntegral | IsUnsignedIntegral | IsFloatingPoint)) != 0);
    }

    [[nodiscard]] bool is_sequence() const { return ((sm_object_attrib[attrib_index()] & IsSequence) != 0); }

    // True for types whose m_offset can advance past the owning block's
    // payload start as the container is consumed: Array (advances by
    // sizeof(Object) per array_pop_head), Packed (advances by 1-8 bytes
    // per packed_pop_head), and String (advances by 1+ bytes per
    // string_pop_count / string_interval).  GC's gc_mark_object uses this
    // to know when to resolve the offset back to its owning block via
    // gvm_find_owning_payload, instead of treating a non-payload-start
    // offset as slack-noise garbage and silently skipping.  All other
    // types maintain m_offset == payload_start for their lifetime, so
    // a misaligned m_offset on those is a real bug and should remain
    // a silent skip in GC.
    [[nodiscard]] bool offset_can_advance() const {
        auto t = type();
        return ((t == Type::Packed) || (t == Type::Array) || (t == Type::String));
    }

    [[nodiscard]] bool is_callable() const { return ((sm_object_attrib[attrib_index()] & IsCallable) != 0); }

    [[nodiscard]] bool is_deepeq() const { return ((sm_object_attrib[attrib_index()] & IsDeepeq) != 0); }

    [[nodiscard]] bool is_persistable() const { return ((sm_object_attrib[attrib_index()] & IsPersistable) != 0); }

    // is_persistable_now(trx): like is_persistable(), but also returns
    // true for heap-referencing composites (Array, String, Dict, Set,
    // Tagged, Curry, Thunk, Record, Packed) whose payload lives in
    // GLOBAL VM.  Used by find-all / find-n / aggregate to gate result
    // values across the per-alternative save/restore cycle.
    //
    // Why a global composite is persistable: the global VM region is
    // journal-skipped (Save::save_data short-circuits is_global) and
    // never reclaimed by Save::restore, so a global block survives any
    // restore.  R6 pointer hygiene (Save::reject_local_into_global)
    // ensures global containers cannot hold local-VM offsets, so the
    // composite is transitively safe -- every reachable offset stays
    // valid past restore.
    //
    // Eqref values are excluded: their m_offset slot holds a generation
    // counter, not a VM offset, so the global-region range check
    // doesn't apply.  Their persistability is governed by the eqref
    // subsystem (m_eqdict / m_eqset / m_eqproc_storage are all
    // restore-immune by construction).
    [[nodiscard]] bool is_persistable_now(const Trix *trx) const {
        if (is_persistable()) {
            return true;
        } else if (!uses_vm() || is_eqref()) {
            return false;
        } else {
            return trx->is_global(m_offset);
        }
    }

    // Save Level
    [[nodiscard]] save_level_t save_level() const {
        assert(!is_sourceloc() && "save_level: called on SourceLoc (m_object_save_level holds stream_id, not save level)");

        return m_object_save_level;
    }

    void set_save_level(save_level_t save_level) { m_object_save_level = save_level; }

    // Operators types
    using operator_flags_t = uint8_t;
    static constexpr operator_flags_t StandardOp{0x00};
    static constexpr operator_flags_t ControlOp{0x01};
    static constexpr operator_flags_t RunOp{0x02};
    static constexpr operator_flags_t StopOp{0x04};
    static constexpr operator_flags_t DictForAllOp{0x08};
    static constexpr operator_flags_t BarrierOp{0x10};    // try/try-catch/finally/with-stream/choice/cell-eval/batch/find-all
    static constexpr operator_flags_t ErrorPushOp{0x20};  // barriers that push /error-name to op stack (try, try-catch)
    static constexpr operator_flags_t DictPopOp{
            0x40};  // operators that pop a dict from dict stack (end-locals, end-module, closure-end)
    static constexpr operator_flags_t ExitFenceOp{0x80};  // exit may not cross this op (collection in progress whose operand-stack
                                                          // state -- e.g. match-all's accumulation Mark -- exit cannot reconcile)

    using operator_popcount_t = uint8_t;

    // long, ulong, address, and double Objects use an ExtValue to hold their value
    // ExtValue are unique per Object, they are not shared
    // make_long(), make_ulong(), make_address(), make_double() alloc an ExtValue
    // Name/Dict lookup and copying from an array/packed container alloc an ExtValue
    // when op_stack, exec_stack pop an ExtValue Object it is freed
    // when a container ExtValue Object is updated:
    //   if (Object.m_save_level == curr_save_level): Object is overwritten and ExtValue is freed
    //   if (Object.m_save_level != curr_save_level): Object and ExtValue are saved for future restore
    // when Save::restore unwinds changes to a ExtValue Object the ExtValue is freed
    // Save::restore checks the operand/execution stacks
    //   1) determine the number of obsolete ExtValue Object on the stack
    //   2) allocates a new ExtValue at restored save_level
    //   3) updates ExtValue Object
    //   4) stale ExtValue Object updates are processed in lowest to highest offset order
    class ExtValue {
    public:
        ExtValue() = delete;

        [[nodiscard]] static vm_offset_t make(Trix *trx, long_t value) {
            auto [ext, offset] = alloc(trx, ChunkKind::Long);
            ext->m_long = value;
            return offset;
        }

        [[nodiscard]] static vm_offset_t make(Trix *trx, ulong_t value) {
            auto [ext, offset] = alloc(trx, ChunkKind::ULong);
            ext->m_ulong = value;
            return offset;
        }

        [[nodiscard]] static vm_offset_t make(Trix *trx, address_t value) {
            auto [ext, offset] = alloc(trx, ChunkKind::Address);
            ext->m_address = value;
            return offset;
        }

        [[nodiscard]] static vm_offset_t make(Trix *trx, double_t value) {
            auto [ext, offset] = alloc(trx, ChunkKind::Double);
            ext->m_double = value;
            return offset;
        }

        // Validate all per-save-level free lists for structural integrity.
        // Returns true if: every node is a valid aligned offset within [0, vm_ptr),
        // every node has the dead sentinel set, no cycles exist (tortoise-hare),
        // and the active count is non-negative.
        [[nodiscard]] static bool validate_free_lists(Trix *trx) {
            for (save_level_t lvl = 0; lvl <= trx->m_curr_save_level; ++lvl) {
                auto slow = trx->m_extvalue_free_list[lvl];
                auto fast = slow;
                bool advance_slow = false;

                while (fast != nulloffset) {
                    if (!trx->valid_active_offset<ExtValue>(fast)) {
                        return false;
                    } else {
                        auto ext = trx->offset_to_ptr<ExtValue>(fast);
                        if ((ext->s.m_free_sentinel ^ fast) != sm_dead_sentinel_magic) {
                            return false;  // node is not marked as freed
                        } else {
                            fast = ext->s.m_next;

                            // advance slow pointer every other step (tortoise-hare)
                            if (advance_slow) {
                                slow = trx->offset_to_ptr<ExtValue>(slow)->s.m_next;
                                if (slow == fast) {
                                    return false;  // cycle detected
                                }
                            }
                            advance_slow = !advance_slow;
                        }
                    }
                }
            }

            return (trx->m_extvalue_active_count >= 0);
        }

        // O(1): live ExtValues have a sentinel slot that XOR's with the offset to something
        // other than sm_dead_sentinel_magic; freed ones hold (offset ^ magic) exactly.
        // Returns false for nulloffset, out-of-range, or freed ExtValues so that
        // error-cascade paths (format_backtrace, scan_procedure cleanup) can
        // detect already-freed Objects without asserting.
        [[nodiscard]] static bool valid(Trix *trx, vm_offset_t target) {
            if (!trx->valid_offset<ExtValue>(target)) {
                return false;
            } else {
                auto ext = trx->offset_to_ptr<ExtValue>(target);
                return ((ext->s.m_free_sentinel ^ target) != sm_dead_sentinel_magic);
            }
        }
    private:
        friend class Object;
        friend class Save;

        // ExtValues are recycled through per-save-level free lists.
        // When restore() rolls the VM heap back to a barrier, all VM memory above the
        // barrier is implicitly reclaimed -- but ExtValues that were explicitly freed
        // (e.g. when an operator consumed a Long from the operand stack) need to be
        // reusable before the next restore.  Per-level lists ensure that a freed
        // ExtValue at level N is not handed out at a lower level that would survive
        // a subsequent restore(N), which would then try to free a VM address that is
        // now below the new barrier.
        //
        // alloc() consults m_curr_alloc_global: when set, bypasses the free list
        // entirely and routes through gvm_alloc with the supplied ChunkKind tag.
        // Global ExtValues are journal-skipped; the local free-list machinery
        // does not apply to them (reclamation is the global allocator's job,
        // via coalescing / GC).  The kind parameter is consulted only
        // on the global path; locals share one untyped pool.
        //
        // Globals do NOT participate in m_extvalue_active_count -- that counter is
        // save/restore-snapshotted (m_extvalue_active_save[]) and tracks LOCAL
        // ExtValues only.  Global counts are reported separately via vm-global-info's
        // /by-kind histogram (gvm_for_each).
        //
        // alloc_local() is the local-only variant for paths that must stay below the
        // restore barrier regardless of the runtime flag (notably restore()'s
        // relocation loop, which moves above-barrier ExtValues back below; routing
        // those through global VM would change the Object's storage class as a
        // side-effect of a `set-global true; save; ...; restore` pattern).
        [[nodiscard]] static std::pair<ExtValue *, vm_offset_t> alloc(Trix *trx, ChunkKind kind) {
            if (trx->m_curr_alloc_global) {
                auto [ext, offset] = trx->gvm_alloc<ExtValue>(sizeof(ExtValue), kind);
                ext->s.m_free_sentinel = 0;  // mark live (matches local-path post-pop state)
                return std::pair{ext, offset};
            } else {
                return alloc_local(trx);
            }
        }

        [[nodiscard]] static std::pair<ExtValue *, vm_offset_t> alloc_local(Trix *trx) {
            auto bucket = &trx->m_extvalue_free_list[trx->m_curr_save_level];
            auto offset = *bucket;
            if (offset != nulloffset) {
                auto ext = trx->offset_to_ptr<ExtValue>(offset);
                *bucket = ext->s.m_next;
                // Clear the sentinel slot.  Any value whose XOR with `offset` differs from
                // sm_dead_sentinel_magic reads as live; writing 0 works because valid VM
                // offsets (< vm_capacity, well under 2^32) cannot equal 0xDEADBEEF.
                ext->s.m_free_sentinel = 0;
                ++trx->m_extvalue_active_count;
                return std::pair{ext, offset};
            } else {
                ++trx->m_extvalue_active_count;
                return trx->vm_alloc<ExtValue>();
            }
        }

        static void free(Trix *trx, vm_offset_t offset, save_level_t save_level) {
            // Global path: route through gvm_free for boundary-tag coalescing.
            // The ExtValue's payload offset is what was returned by gvm_alloc,
            // so gvm_free can find the block header at offset - HeaderSize.
            // No active_count adjustment -- globals don't participate in that
            // counter (see alloc).
            if (trx->is_global(offset)) {
                trx->gvm_free(offset);
            } else if (!valid(trx, offset)) {
                trx->error(Error::InternalError, "ExtValue::free: double-free or invalid ExtValue at offset {}", offset);
            } else if (save_level > trx->m_curr_save_level) {
                trx->error(Error::RangeCheck,
                           "ExtValue::free: save_level {} exceeds current save level {}",
                           save_level,
                           trx->m_curr_save_level);
            } else {
                auto bucket = &trx->m_extvalue_free_list[save_level];
                auto ext = trx->offset_to_ptr<ExtValue>(offset);
                ext->s.m_next = *bucket;
                // Store the offset-XOR'd sentinel so each slot's expected "dead" value is
                // unique (see class-level comment).  A 64-bit payload whose top 32 bits
                // happen to equal `offset ^ sm_dead_sentinel_magic` can still false-positive
                // at that one slot (1 in 2^32), but no algorithmic pattern (e.g. hex
                // literals or pointers containing 0xDEADBEEF) hits every slot -- which was
                // the failure mode of the earlier fixed-sentinel scheme.
                ext->s.m_free_sentinel = (offset ^ sm_dead_sentinel_magic);
                *bucket = offset;
                --trx->m_extvalue_active_count;
            }
        }

        // Relocate the `count` above-barrier local ExtValue scalars (running +
        // suspended coroutine stacks) below the restore barrier so they survive.  The
        // lowest-offset one is moved first (Trix::find_lowest_relocatable returns the
        // global lowest each call): ascending order ensures a relocation's freed slot
        // can back the next alloc_local without overwriting an un-relocated ExtValue.
        // `count` is the total the caller tallied via check_stack (running op/exec +
        // every qualifying suspended coroutine's op/exec/err); the scan filter in
        // find_lowest_relocatable matches that tally.  restore_level gates which
        // suspended coroutines are scanned (those active since the save).
        static void restore(Trix *trx, save_level_t restore_level, integer_t count) {
            // restore can roll back multiple save levels
            for (auto i = (trx->m_curr_save_level + 1); i < trx->m_max_save_level; ++i) {
                trx->m_extvalue_free_list[i] = nulloffset;
            }

            // Assert: no free-list entry at the restored level or below
            // should point at or above the restore barrier.  If one does,
            // it means an ExtValue was freed to the wrong per-level bucket
            // (a bug in the caller, not something to silently clean up).
            auto barrier = static_cast<vm_offset_t>(trx->m_vm_ptr - trx->m_vm_base);
            for (save_level_t lvl = 0; lvl <= trx->m_curr_save_level; ++lvl) {
                auto offset = trx->m_extvalue_free_list[lvl];
                while (offset != nulloffset) {
                    if (offset >= barrier) {
                        trx->error(Error::InternalError,
                                   "ExtValue::restore: free-list[{}] entry at offset {} is above restore barrier {}",
                                   lvl,
                                   offset,
                                   barrier);
                    }
                    offset = trx->offset_to_ptr<ExtValue>(offset)->s.m_next;
                }
            }

            auto curr_save_level = trx->m_curr_save_level;
            for (auto remaining = count; remaining != 0; --remaining) {
                auto *object = trx->find_lowest_relocatable(restore_level, false);
                if (object == nullptr) {
                    trx->error(Error::InternalError, "ExtValue::restore: no object found with {} pending", remaining);
                } else {
                    auto old_ext = object->extvalue(trx);
                    // allocate a new ExtValue at the curr_save_level and move data.
                    // Use alloc_local so a `set-global true; save; ...; restore`
                    // pattern doesn't silently relocate the recovered ExtValue into
                    // global VM (which would change the Object's storage class
                    // across a restore, surprising callers).
                    auto [new_ext, new_offset] = alloc_local(trx);
                    new_ext->m_value = old_ext->m_value;

                    // update Object
                    object->m_extvalue_save_level = curr_save_level;
                    object->m_offset = new_offset;

                    // Invalidate address cache -- the pointed-to memory may
                    // have changed accessibility across the save/restore boundary.
                    if (object->is_address()) {
                        object->clear_address_cache();
                    }
                }
            }
        }

        // Constant XOR'd with the slot's own vm_offset to form that slot's unique
        // "dead" sentinel.  When the ExtValue is on a free list, bytes 4-7 hold
        // (offset ^ sm_dead_sentinel_magic); bytes 0-3 hold the free-list link.  Both
        // halves are overwritten by the first value assignment after alloc(), so valid()
        // simply checks the XOR result instead of walking all free lists.
        //
        // Staying at 8 bytes is a cost-vs-false-positive tradeoff.  Bumping to 16 bytes
        // with an out-of-band sentinel would be watertight but doubles the memory per
        // Long/ULong/Double/Address.  Using a fixed sentinel (0xDEADBEEF) at a fixed
        // location was unacceptable because any payload whose top 32 bits match false-
        // positives at every slot (e.g. a Long with value 0xDEADBEEF12345678, or any
        // Address in that range).  Offset-XOR'd sentinels preserve the 8-byte size while
        // making the expected "dead" value unique per slot: a given payload pattern can
        // false-positive at most ONE slot (where
        // `offset == payload_upper_32 ^ sm_dead_sentinel_magic`), with probability
        // ~1/2^32 per (slot, payload) pair.
        static constexpr uint32_t sm_dead_sentinel_magic{0xDEADBEEF};

        union {
            struct {
                vm_offset_t m_next;        // (bytes 0-3) next ExtValue in free list
                uint32_t m_free_sentinel;  // (bytes 4-7) sm_dead_sentinel when free, 0 when inuse
            } s;                           //
            uint64_t m_value;              // generic access for blind copying
            long_t m_long;                 // value when associated with a Long Object
            ulong_t m_ulong;               // value when associated with a ULong Object
            address_t m_address;           // value when associated with an Address Object
            double_t m_double;             // value when associated with a Double Object
        };
    };
    static_assert(sizeof(ExtValue) == 8);

    // WideValue: 16-byte heap cell backing Int128 and UInt128 Objects.  Mirrors ExtValue's
    // per-save-level free-list design exactly, just with a wider payload.  Allocator and
    // restore machinery are parallel to ExtValue -- the save/restore loop calls both so
    // 128-bit values above a save barrier get relocated the same way 64-bit values do.
    //
    // The free-state sentinel at bytes 12..15 is XOR'd with the slot's own offset, so every
    // slot has a unique expected value.  For a 128-bit user value to be misidentified as
    // freed, the upper 32 bits of the high half would have to equal exactly
    // (offset ^ sm_dead_sentinel_magic) -- a condition specific to one slot, not a pattern
    // that collides across the whole heap.  This defeats algorithmic patterns like users
    // writing 0xDEADBEEF.... hex literals, which would hit a fixed sentinel at every slot
    // but only hit the offset-XOR'd sentinel at the single slot where
    // offset == 0xDEADBEEF ^ sm_dead_sentinel_magic.
    class WideValue {
    public:
        WideValue() = delete;

        [[nodiscard]] static vm_offset_t make(Trix *trx, int128_t value) {
            auto [wide, offset] = alloc(trx, ChunkKind::Int128);
            wide->write_int128(value);
            return offset;
        }

        [[nodiscard]] static vm_offset_t make(Trix *trx, uint128_t value) {
            auto [wide, offset] = alloc(trx, ChunkKind::UInt128);
            wide->write_uint128(value);
            return offset;
        }

        // Walk every per-save-level free list and return the total reusable cell
        // count.  Symmetric with ExtValue's `extvalue-free` :status companion.
        [[nodiscard]] static integer_t free_list_count(Trix *trx) {
            integer_t count = 0;
            for (save_level_t lvl = 0; lvl <= trx->m_curr_save_level; ++lvl) {
                auto offset = trx->m_widevalue_free_list[lvl];
                while (offset != nulloffset) {
                    ++count;
                    offset = trx->offset_to_ptr<WideValue>(offset)->s.m_next;
                }
            }
            return count;
        }

        // Validate all per-save-level free lists: every node must be aligned within
        // [0, vm_ptr), carry all three offset-XOR'd dead sentinels, and no cycles via
        // tortoise-hare.
        [[nodiscard]] static bool validate_free_lists(Trix *trx) {
            for (save_level_t lvl = 0; lvl <= trx->m_curr_save_level; ++lvl) {
                auto slow = trx->m_widevalue_free_list[lvl];
                auto fast = slow;
                bool advance_slow = false;

                while (fast != nulloffset) {
                    if (!trx->valid_active_offset<WideValue>(fast)) {
                        return false;
                    } else {
                        auto wide = trx->offset_to_ptr<WideValue>(fast);
                        if (((wide->s.m_free_sentinel_1 ^ fast) != sm_dead_sentinel_magic_1) ||
                            ((wide->s.m_free_sentinel_2 ^ fast) != sm_dead_sentinel_magic_2) ||
                            ((wide->s.m_free_sentinel_3 ^ fast) != sm_dead_sentinel_magic_3)) {
                            return false;
                        }

                        fast = wide->s.m_next;

                        if (advance_slow) {
                            slow = trx->offset_to_ptr<WideValue>(slow)->s.m_next;
                            if (slow == fast) {
                                return false;
                            }
                        }
                        advance_slow = !advance_slow;
                    }
                }
            }

            return (trx->m_widevalue_active_count >= 0);
        }

        // O(1) liveness check.  Bytes 4..7, 8..11, and 12..15 each carry an offset-XOR'd
        // sentinel when free; all three are zeroed on alloc and overwritten by the first
        // write_int128/write_uint128 call.  A live payload is flagged as freed only if all
        // three 32-bit slots simultaneously equal (offset XOR their respective magic) --
        // probability 1/2^96 per (slot, payload) pair, i.e. never in any realistic
        // workload.  Using offset XOR rather than fixed magics means a specific payload
        // pattern can at most false-positive at one slot (not every slot), further
        // limiting the worst case.
        [[nodiscard]] static bool valid(Trix *trx, vm_offset_t target) {
            if (!trx->valid_offset<WideValue>(target)) {
                return false;
            } else {
                auto wide = trx->offset_to_ptr<WideValue>(target);
                return (((wide->s.m_free_sentinel_1 ^ target) != sm_dead_sentinel_magic_1) ||
                        ((wide->s.m_free_sentinel_2 ^ target) != sm_dead_sentinel_magic_2) ||
                        ((wide->s.m_free_sentinel_3 ^ target) != sm_dead_sentinel_magic_3));
            }
        }

        // Stored as two uint64_t halves to keep alignof == 8 (matches VM heap alignment
        // without forcing alignas).  memcpy round-trip is a no-op at -O1+.
        void write_int128(int128_t value) { std::memcpy(&m_halves, &value, sizeof(int128_t)); }

        [[nodiscard]] int128_t read_int128() const {
            int128_t out;
            std::memcpy(&out, &m_halves, sizeof(int128_t));
            return out;
        }

        void write_uint128(uint128_t value) { std::memcpy(&m_halves, &value, sizeof(uint128_t)); }

        [[nodiscard]] uint128_t read_uint128() const {
            uint128_t out;
            std::memcpy(&out, &m_halves, sizeof(uint128_t));
            return out;
        }
    private:
        friend class Object;
        friend class Save;

        // alloc / alloc_local / free: parallel structure to ExtValue.  See the
        // documentation block on ExtValue::alloc above for the global-vs-local
        // semantics; everything stated there applies here for 128-bit values.
        [[nodiscard]] static std::pair<WideValue *, vm_offset_t> alloc(Trix *trx, ChunkKind kind) {
            if (trx->m_curr_alloc_global) {
                auto [wide, offset] = trx->gvm_alloc<WideValue>(sizeof(WideValue), kind);
                wide->s.m_free_sentinel_1 = 0;
                wide->s.m_free_sentinel_2 = 0;
                wide->s.m_free_sentinel_3 = 0;
                return std::pair{wide, offset};
            } else {
                return alloc_local(trx);
            }
        }

        [[nodiscard]] static std::pair<WideValue *, vm_offset_t> alloc_local(Trix *trx) {
            auto bucket = &trx->m_widevalue_free_list[trx->m_curr_save_level];
            auto offset = *bucket;
            if (offset != nulloffset) {
                auto wide = trx->offset_to_ptr<WideValue>(offset);
                *bucket = wide->s.m_next;
                // Clear all three sentinel slots.  Any value whose XOR with `offset` differs
                // from the expected magic reads as live; writing 0 works because valid VM
                // offsets (< vm_capacity, well under 2^32) cannot equal any of the magics.
                wide->s.m_free_sentinel_1 = 0;
                wide->s.m_free_sentinel_2 = 0;
                wide->s.m_free_sentinel_3 = 0;
                ++trx->m_widevalue_active_count;
                return std::pair{wide, offset};
            } else {
                ++trx->m_widevalue_active_count;
                return trx->vm_alloc<WideValue>();
            }
        }

        static void free(Trix *trx, vm_offset_t offset, save_level_t save_level) {
            // Global path: route through gvm_free (mirror of ExtValue::free).
            if (trx->is_global(offset)) {
                trx->gvm_free(offset);
            } else if (!valid(trx, offset)) {
                trx->error(Error::InternalError, "WideValue::free: double-free or invalid WideValue at offset {}", offset);
            } else if (save_level > trx->m_curr_save_level) {
                trx->error(Error::RangeCheck,
                           "WideValue::free: save_level {} exceeds current save level {}",
                           save_level,
                           trx->m_curr_save_level);
            } else {
                auto bucket = &trx->m_widevalue_free_list[save_level];
                auto wide = trx->offset_to_ptr<WideValue>(offset);
                wide->s.m_next = *bucket;
                // Store three independent offset-XOR'd sentinels across the three 4-byte
                // slots the free layout doesn't need for the free-list link.  With three
                // different magics and offset-XOR, a live 128-bit payload must match all
                // three slots simultaneously for valid() to misclassify it as freed -- a
                // 1-in-2^96 coincidence per (slot, payload) pair, for any fixed payload.
                wide->s.m_free_sentinel_1 = (offset ^ sm_dead_sentinel_magic_1);
                wide->s.m_free_sentinel_2 = (offset ^ sm_dead_sentinel_magic_2);
                wide->s.m_free_sentinel_3 = (offset ^ sm_dead_sentinel_magic_3);
                *bucket = offset;
                --trx->m_widevalue_active_count;
            }
        }

        // Relocate the `count` above-barrier local WideValue scalars (running +
        // suspended coroutine stacks) below the restore barrier; mirror of
        // ExtValue::restore (Trix::find_lowest_relocatable returns the global lowest
        // WideValue, ascending order).  Runs AFTER ExtValue::restore (save.inl).
        static void restore(Trix *trx, save_level_t restore_level, integer_t count) {
            for (auto i = (trx->m_curr_save_level + 1); i < trx->m_max_save_level; ++i) {
                trx->m_widevalue_free_list[i] = nulloffset;
            }

            auto barrier = static_cast<vm_offset_t>(trx->m_vm_ptr - trx->m_vm_base);
            for (save_level_t lvl = 0; lvl <= trx->m_curr_save_level; ++lvl) {
                auto offset = trx->m_widevalue_free_list[lvl];
                while (offset != nulloffset) {
                    if (offset >= barrier) {
                        trx->error(Error::InternalError,
                                   "WideValue::restore: free-list[{}] entry at offset {} is above restore barrier {}",
                                   lvl,
                                   offset,
                                   barrier);
                    }
                    offset = trx->offset_to_ptr<WideValue>(offset)->s.m_next;
                }
            }

            auto curr_save_level = trx->m_curr_save_level;
            for (auto remaining = count; remaining != 0; --remaining) {
                auto *object = trx->find_lowest_relocatable(restore_level, true);
                if (object == nullptr) {
                    trx->error(Error::InternalError, "WideValue::restore: no object found with {} pending", remaining);
                } else {
                    auto old_wide = object->widevalue(trx);
                    // alloc_local: stay below the restore barrier even when
                    // m_curr_alloc_global is set (mirror of ExtValue::restore).
                    auto [new_wide, new_offset] = alloc_local(trx);
                    // Copy through a stack temporary: the new cell is allocated at
                    // the restore barrier and the dying cell may sit only 8 bytes
                    // above it, so a direct 16-byte struct assignment is a memcpy
                    // over overlapping storage (ASan memcpy-param-overlap; found
                    // by fuzzing at the 256K harness VM).  ExtValue::restore is
                    // immune -- its payload is a single 8-byte scalar.
                    auto halves_obj = old_wide->m_halves;
                    new_wide->m_halves = halves_obj;

                    object->m_extvalue_save_level = curr_save_level;
                    object->m_offset = new_offset;
                }
            }
        }

        // Three independent constants, each XOR'd with the slot's own vm_offset to form
        // three expected "dead" marker words filling bytes 4..15 of the 16-byte slot when
        // free.  Three different magics (the classic debug sentinels 0xDEADBEEF /
        // 0xCAFEBABE / 0xFEEDFACE) are used so no single fixed 32-bit pattern acts as the
        // marker; a live payload would need all three 32-bit slots to simultaneously equal
        // (offset XOR the respective magic) to false-positive.
        static constexpr uint32_t sm_dead_sentinel_magic_1{0xDEADBEEF};
        static constexpr uint32_t sm_dead_sentinel_magic_2{0xCAFEBABE};
        static constexpr uint32_t sm_dead_sentinel_magic_3{0xFEEDFACE};

        // Layout: when free, bytes 0-3 hold the next-free-list link, bytes 4-7 / 8-11 /
        // 12-15 each hold an offset-XOR'd sentinel word.  When live, all 16 bytes form the
        // 128-bit value (stored in little-endian on x86/ARM LE), which means the three
        // sentinel words overlap the upper 32 bits of m_lo, the lower 32 bits of m_hi,
        // and the upper 32 bits of m_hi.
        //
        // Staying at 16 bytes is a cost-vs-false-positive tradeoff.  Bumping to 24 bytes
        // with an out-of-band sentinel would be watertight but adds 50% memory per 128-bit
        // value.  A fixed single sentinel (0xDEADBEEF at one fixed location) was
        // unacceptable because any payload whose top 32 bits matched false-positived at
        // every slot -- e.g. the hex literal 0xDEADBEEFCAFEBABE0123456789ABCDEF was
        // misidentified as freed.  Offset-XOR'ing each sentinel makes the expected "dead"
        // value unique per slot, and using three independent sentinel words demands a
        // simultaneous 96-bit coincidence for a false positive: at most ONE slot can
        // false-positive for any fixed payload pattern (the one whose offset XOR-matches
        // the payload's bytes 4..15), with overall probability 1/2^96 per (slot, payload)
        // pair.
        struct Halves {
            uint64_t m_lo;  // bytes 0-7
            uint64_t m_hi;  // bytes 8-15
        };
        union {
            struct {
                vm_offset_t m_next;          // bytes 0-3: free-list link when free
                uint32_t m_free_sentinel_1;  // bytes 4-7: (offset ^ magic_1) when free
                uint32_t m_free_sentinel_2;  // bytes 8-11: (offset ^ magic_2) when free
                uint32_t m_free_sentinel_3;  // bytes 12-15: (offset ^ magic_3) when free
            } s;
            Halves m_halves;
        };
    };
    static_assert(sizeof(WideValue) == 16);

    [[nodiscard]] WideValue *widevalue(Trix *trx) const {
        assert(uses_widevalue());

        if (WideValue::valid(trx, m_offset)) {
            return trx->offset_to_ptr<WideValue>(m_offset);
        } else {
            trx->error(Error::InternalError, "widevalue: accessing freed or invalid WideValue at offset {}", m_offset);
        }
    }

    void free_widevalue(Trix *trx) {
        if (uses_widevalue()) {
            // Object is now in a partially-invalid state, caller should immediately overwrite or free this Object
            WideValue::free(trx, m_offset, m_extvalue_save_level);
            m_offset = nulloffset;
        } else {
            trx->error(Error::TypeCheck, "free_widevalue: object does not use WideValue");
        }
    }

    void maybe_free_widevalue(Trix *trx) {
        if (uses_widevalue()) {
            // Object is now in a partially-invalid state, caller should immediately overwrite or free this Object
            WideValue::free(trx, m_offset, m_extvalue_save_level);
            m_offset = nulloffset;
        }
    }

    [[nodiscard]] ExtValue *extvalue(Trix *trx) const {
        assert(uses_extvalue());

        if (ExtValue::valid(trx, m_offset)) {
            return trx->offset_to_ptr<ExtValue>(m_offset);
        } else {
            trx->error(Error::InternalError, "extvalue: accessing freed or invalid ExtValue at offset {}", m_offset);
        }
    }

    void free_extvalue(Trix *trx) {
        if (uses_extvalue()) {
            // Object is now in a partially-invalid state, caller should immediately overwrite or free this Object
            ExtValue::free(trx, m_offset, m_extvalue_save_level);
            m_offset = nulloffset;
        } else {
            trx->error(Error::TypeCheck, "free_extvalue: object does not use ExtValue");
        }
    }

    void maybe_free_extvalue(Trix *trx) {
        if (uses_extvalue()) {
            // Object is now in a partially-invalid state, caller should immediately overwrite or free this Object
            ExtValue::free(trx, m_offset, m_extvalue_save_level);
            m_offset = nulloffset;
        } else if (uses_widevalue()) {
            // Same contract for WideValue-backed Objects (Int128/UInt128).
            WideValue::free(trx, m_offset, m_extvalue_save_level);
            m_offset = nulloffset;
        }
    }

    [[nodiscard]] hash_t hash(Trix *trx) const {
        switch (type()) {
        case Type::Null:
            return wyhash32_sv("Object::null");

        case Type::Byte:
            return mix32(m_byte);

        case Type::Integer:
            return mix32(static_cast<uint32_t>(m_integer));

        case Type::UInteger:
            return mix32(m_uinteger);

        case Type::Long:
            return mix64_to_32(static_cast<uint64_t>(long_value(trx)));

        case Type::ULong:
            return mix64_to_32(ulong_value(trx));

        case Type::Int128: {
            auto v = static_cast<uint128_t>(int128_value(trx));
            return mix64_to_32(static_cast<uint64_t>(v) ^ static_cast<uint64_t>(v >> 64));
        }

        case Type::UInt128: {
            auto v = uint128_value(trx);
            return mix64_to_32(static_cast<uint64_t>(v) ^ static_cast<uint64_t>(v >> 64));
        }

        case Type::Address:
            return mix64_to_32(reinterpret_cast<uintptr_t>(address_value(trx)));

        case Type::Real: {
            // NaN does not compare equal; VerifyKey rejects NaN keys before insertion
            real_t value = real_value();
            if (value == 0.0f) {
                // canonicalize -0.0f -> +0.0f
                value = 0.0f;
            }
            return mix32(std::bit_cast<uint32_t>(value));
        }

        case Type::Double: {
            // NaN does not compare equal; VerifyKey rejects NaN keys before insertion
            double_t value = double_value(trx);
            if (value == 0.0) {
                // canonicalize -0.0 -> +0.0
                value = 0.0;
            }
            return mix64_to_32(std::bit_cast<uint64_t>(value));
        }

        case Type::Boolean:
            return (m_boolean ? wyhash32_sv("Object::true") : wyhash32_sv("Object::false"));

        case Type::Operator:
            // 'OP' (0x4F50) in the high 16 bits keeps operator hashes in a disjoint
            // space from Integer hashes whose values stay in the low 16 bits.
            return mix32(static_cast<uint32_t>(m_operator) | 0x4F500000U);

        case Type::Mark:
            return wyhash32_sv("Object::mark");

        case Type::Name:
            // name and string values must hash the same
            return trx->offset_to_ptr<Name>(m_name)->hash();

        case Type::Array:   // m_array
        case Type::Packed:  // m_packed
            return mix64_to_32((static_cast<uint64_t>(m_offset) << 32) | m_arrays_length);

        case Type::String:
            // name and string values must hash the same
            return wyhash32_sv(sv_value(trx));

        case Type::Stream:        // m_stream
        case Type::Dict:          // m_dict
        case Type::Curry:         // m_curry
        case Type::Thunk:         // m_thunk
        case Type::Set:           // m_set
        case Type::Tagged:        // m_tagged
        case Type::Record:        // m_record
        case Type::Coroutine:     // m_coroutine
        case Type::PipeBuffer:    // m_pipe_buffer
        case Type::Cell:          // m_cell
        case Type::Continuation:  // m_continuation
        case Type::OpaqueHandle:  // m_handle
            return mix32(m_offset);

        case Type::SourceLoc:
            assert(false && "hash: SourceLoc cannot be a dict key");

            std::unreachable();
        }

        assert(false && "hash: unknown Object type");
        std::unreachable();
    }

    [[nodiscard]] bool equal(Trix *trx, Object other) const {
        auto this_type = type();
        auto other_type = other.type();
        // Fast path: Name-Name comparison (the hot case for dict-key compares, `eq`,
        // and user-level name equality).  Avoids the 28-way switch.
        if ((this_type == Type::Name) && (other_type == Type::Name)) [[likely]] {
            return (m_name == other.m_name);
        } else {
            if (this_type == other_type) {
                switch (this_type) {
                case Type::Null:
                    return true;

                case Type::Byte:
                    return (m_byte == other.m_byte);

                case Type::Integer:
                    return (m_integer == other.m_integer);

                case Type::UInteger:
                    return (m_uinteger == other.m_uinteger);

                case Type::Long:
                    return (long_value(trx) == other.long_value(trx));

                case Type::ULong:
                    return (ulong_value(trx) == other.ulong_value(trx));

                case Type::Int128:
                    return (int128_value(trx) == other.int128_value(trx));

                case Type::UInt128:
                    return (uint128_value(trx) == other.uint128_value(trx));

                case Type::Address:
                    return (address_value(trx) == other.address_value(trx));

                case Type::Real:
                    return (m_real == other.m_real);

                case Type::Double:
                    return (double_value(trx) == other.double_value(trx));

                case Type::Boolean:
                    return (m_boolean == other.m_boolean);

                case Type::Operator:
                    return (m_operator == other.m_operator);

                case Type::Mark:
                    return true;

                case Type::Name:
                    return (m_name == other.m_name);

                case Type::Array:
                    return ((m_array == other.m_array) && (m_arrays_length == other.m_arrays_length));

                case Type::Packed:
                    return ((m_packed == other.m_packed) && (m_arrays_length == other.m_arrays_length));

                case Type::String: {
                    auto this_length = m_string_length;
                    if (this_length == other.m_string_length) {
                        auto this_str = string_data_ptr(trx);
                        auto other_str = other.string_data_ptr(trx);
                        return (std::memcmp(this_str, other_str, this_length) == 0);
                    } else {
                        return false;
                    }
                }

                case Type::Stream:
                    return (m_stream == other.m_stream);

                case Type::Dict:
                    return (m_dict == other.m_dict);

                case Type::SourceLoc:
                    assert(false && "equal: SourceLoc cannot be a dict key");
                    std::unreachable();

                case Type::Curry:
                    return (m_curry == other.m_curry);

                case Type::Thunk:
                    return (m_thunk == other.m_thunk);

                case Type::Set:
                    return (m_set == other.m_set);

                case Type::Tagged:
                    return (m_tagged == other.m_tagged);

                case Type::Record:
                    return ((m_record == other.m_record) && (m_length == other.m_length));

                case Type::Coroutine:
                    return (m_coroutine == other.m_coroutine);

                case Type::PipeBuffer:
                    return (m_pipe_buffer == other.m_pipe_buffer);

                case Type::Cell:
                    return (m_cell == other.m_cell);

                case Type::Continuation:
                    return (m_continuation == other.m_continuation);

                case Type::OpaqueHandle:
                    // Same kind + same struct offset = equal handle.  m_length holds HandleKind.
                    return ((m_handle == other.m_handle) && (m_handle_kind == other.m_handle_kind));
                }

                assert(false && "equal: unknown Object type");
                std::unreachable();
            }
            // Name/String duality: binary-token-stream dicts insert keys as Strings;
            // user lookups pass Names.  Both directions compare content-equal.
            const Object *name_side = nullptr;
            const Object *str_side = nullptr;
            if ((this_type == Type::Name) && (other_type == Type::String)) {
                name_side = this;
                str_side = &other;
            } else if ((this_type == Type::String) && (other_type == Type::Name)) {
                name_side = &other;
                str_side = this;
            } else {
                return false;
            }
            auto name = trx->offset_to_ptr<Name>(name_side->m_name);
            auto str = trx->offset_to_ptr<char>(str_side->m_string);
            return name->equal(std::string_view{str, str_side->m_string_length});
        }
    }

    // deep copy
    //
    // The scalar fast path (no ExtValue/WideValue backing -- integers, bytes,
    // booleans, operators, names, container headers) is a struct copy plus a
    // save-level stamp and inlines at call sites; only heap-backed values pay
    // the make_clone_heap call.  The split measured -9.8% on bench_interpreter
    // and -7.0% on bench_journal_write2 (2026-06-06).
    [[nodiscard]] Object make_clone(Trix *trx) const {
        assert(!is_sourceloc() && "make_clone: SourceLoc must not be cloned");

        if (!uses_extvalue() && !uses_widevalue()) {
            auto object = *this;
            object.m_object_save_level = Save::BASE;
            return object;
        } else {
            return make_clone_heap(trx);
        }
    }

    [[nodiscard]] Object make_clone_heap(Trix *trx) const {
        // clone the Object
        auto object = *this;
        object.m_object_save_level = Save::BASE;
        if (uses_extvalue()) {
            // clone the ExtValue.  Kind is derived from this->type() so the
            // clone gets a matching ChunkKind tag when m_curr_alloc_global is
            // set (the global path consults kind; the local path ignores it).
            auto old_ext = extvalue(trx);
            auto [new_ext, new_offset] = ExtValue::alloc(trx, heap_kind_of(type()));
            new_ext->m_value = old_ext->m_value;

            // update Object.  When the clone landed in global VM, stamp its
            // ExtValue save_level as Save::BASE -- globals don't get
            // reclaimed by save/restore, so the per-save-level field is inert
            // for them; BASE is the right sentinel.
            object.m_extvalue_save_level = extvalue_save_level_for_alloc(trx, new_offset);
            object.m_offset = new_offset;
        } else if (uses_widevalue()) {
            // clone the WideValue (mirror of the ExtValue path above)
            auto old_wide = widevalue(trx);
            auto [new_wide, new_offset] = WideValue::alloc(trx, heap_kind_of(type()));
            new_wide->m_halves = old_wide->m_halves;

            // update Object
            object.m_extvalue_save_level = extvalue_save_level_for_alloc(trx, new_offset);
            object.m_offset = new_offset;
        }
        return object;
    }

    // Like make_clone, but forces the cloned ExtValue/WideValue into the LOCAL
    // region regardless of m_curr_alloc_global.  For SCRATCH clones that must not
    // escape -- e.g. membership-dict keys built inside an operator whose temp
    // structures are reclaimed by vm_temp_restore.  Under ${...}, plain make_clone
    // would allocate those keys GLOBALLY, putting a global block in a temp-region
    // dict whose save-level-naive teardown (create_temp_dict invariant #1) then
    // mis-handles it; forcing local restores the (working) top-level behaviour.
    [[nodiscard]] Object make_clone_local(Trix *trx) const {
        assert(!is_sourceloc() && "make_clone_local: SourceLoc must not be cloned");

        auto object = *this;
        object.m_object_save_level = Save::BASE;
        if (uses_extvalue()) {
            auto old_ext = extvalue(trx);
            auto [new_ext, new_offset] = ExtValue::alloc_local(trx);
            new_ext->m_value = old_ext->m_value;
            object.m_extvalue_save_level = extvalue_save_level_for_alloc(trx, new_offset);
            object.m_offset = new_offset;
        } else if (uses_widevalue()) {
            auto old_wide = widevalue(trx);
            auto [new_wide, new_offset] = WideValue::alloc_local(trx);
            new_wide->m_halves = old_wide->m_halves;
            object.m_extvalue_save_level = extvalue_save_level_for_alloc(trx, new_offset);
            object.m_offset = new_offset;
        }
        return object;
    }

    // shallow copy
    [[nodiscard]] Object make_copy(save_level_t save_level = Save::BASE) const {
        assert(!is_sourceloc() && "make_copy: SourceLoc must not be copied");

        // copy the Object
        auto object = *this;
        object.m_object_save_level = save_level;
        return object;
    }

    [[nodiscard]] static constexpr Object make_null(save_level_t save_level = Save::BASE) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Null);
        object.m_object_save_level = save_level;
        object.m_length = 0;
        object.m_null = 0;
        return object;
    }

    [[nodiscard]] bool is_null() const { return (type() == Type::Null); }

    [[nodiscard]] static constexpr Object make_byte(vm_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Byte);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_byte = value;
        return object;
    }

    [[nodiscard]] bool is_byte() const { return (type() == Type::Byte); }

    void update_byte(vm_t value) {
        assert(is_byte());

        m_byte = value;
    }

    [[nodiscard]] vm_t byte_value() const {
        assert(is_byte());

        return m_byte;
    }

    [[nodiscard]] static constexpr Object make_integer(integer_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Integer);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_integer = value;
        return object;
    }

    [[nodiscard]] bool is_integer() const { return (type() == Type::Integer); }

    void update_integer(integer_t value) {
        assert(is_integer());

        m_integer = value;
    }

    [[nodiscard]] integer_t integer_value() const {
        assert(is_integer());

        return m_integer;
    }

    [[nodiscard]] std::pair<bool, integer_t> integer_value(Trix *trx,
                                                           integer_t lower = std::numeric_limits<integer_t>::min(),
                                                           integer_t upper = std::numeric_limits<integer_t>::max()) const {
        auto integer_min = std::numeric_limits<integer_t>::min();
        auto integer_max = std::numeric_limits<integer_t>::max();
        auto value = integer_t{0};
        auto valid = false;

        switch (+type()) {
        case +Type::Byte: {
            auto b = byte_value();
            valid = true;
            value = static_cast<integer_t>(b);
            break;
        }

        case +Type::Integer:
            valid = true;
            value = integer_value();
            break;

        case +Type::UInteger: {
            auto ui = uinteger_value();
            valid = (ui <= static_cast<uinteger_t>(integer_max));
            value = valid ? static_cast<integer_t>(ui) : integer_max;
            break;
        }

        case +Type::Long: {
            auto l = long_value(trx);
            valid = ((l >= static_cast<long_t>(integer_min)) && (l <= static_cast<long_t>(integer_max)));
            if (valid) {
                value = static_cast<integer_t>(l);
            } else if (l < static_cast<long_t>(integer_min)) {
                value = integer_min;
            } else {
                value = integer_max;
            }
            break;
        }

        case +Type::ULong: {
            auto ul = ulong_value(trx);
            valid = (ul <= static_cast<ulong_t>(integer_max));
            value = valid ? static_cast<integer_t>(ul) : integer_max;
            break;
        }

        case +Type::Int128: {
            auto h = int128_value(trx);
            valid = ((h >= static_cast<int128_t>(integer_min)) && (h <= static_cast<int128_t>(integer_max)));
            if (valid) {
                value = static_cast<integer_t>(h);
            } else if (h < static_cast<int128_t>(integer_min)) {
                value = integer_min;
            } else {
                value = integer_max;
            }
            break;
        }

        case +Type::UInt128: {
            auto uh = uint128_value(trx);
            valid = (uh <= static_cast<uint128_t>(integer_max));
            value = valid ? static_cast<integer_t>(uh) : integer_max;
            break;
        }

        default:
            break;
        }

        return std::pair{valid && (value >= lower) && (value <= upper), value};
    }

    [[nodiscard]] static constexpr Object make_uinteger(uinteger_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::UInteger);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_uinteger = value;
        return object;
    }

    [[nodiscard]] bool is_uinteger() const { return (type() == Type::UInteger); }

    [[nodiscard]] bool is_integers() const {
        auto typ = type();
        return ((typ == Type::Integer) || (typ == Type::UInteger));
    }

    void update_uinteger(uinteger_t value) {
        assert(is_uinteger());

        m_uinteger = value;
    }

    [[nodiscard]] uinteger_t uinteger_value() const {
        assert(is_uinteger());

        return m_uinteger;
    }

    [[nodiscard]] std::pair<bool, uinteger_t> uinteger_value(Trix *trx,
                                                             uinteger_t lower = std::numeric_limits<uinteger_t>::min(),
                                                             uinteger_t upper = std::numeric_limits<uinteger_t>::max()) const {
        auto uinteger_max = std::numeric_limits<uinteger_t>::max();
        auto value = uinteger_t{0};
        auto valid = false;

        switch (+type()) {
        case +Type::Byte: {
            auto b = byte_value();
            valid = true;
            value = static_cast<uinteger_t>(b);
            break;
        }

        case +Type::Integer: {
            auto i = integer_value();
            valid = (i >= 0);
            value = valid ? static_cast<uinteger_t>(i) : 0;
            break;
        }

        case +Type::UInteger:
            valid = true;
            value = uinteger_value();
            break;

        case +Type::Long: {
            auto l = long_value(trx);
            valid = ((l >= 0) && (l <= static_cast<long_t>(uinteger_max)));
            if (valid) {
                value = static_cast<uinteger_t>(l);
            } else if (l < 0) {
                value = 0;
            } else {
                value = uinteger_max;
            }
            break;
        }

        case +Type::ULong: {
            auto ul = ulong_value(trx);
            valid = (ul <= static_cast<ulong_t>(uinteger_max));
            value = valid ? static_cast<uinteger_t>(ul) : uinteger_max;
            break;
        }

        case +Type::Int128: {
            auto h = int128_value(trx);
            valid = ((h >= 0) && (h <= static_cast<int128_t>(uinteger_max)));
            if (valid) {
                value = static_cast<uinteger_t>(h);
            } else if (h < 0) {
                value = 0;
            } else {
                value = uinteger_max;
            }
            break;
        }

        case +Type::UInt128: {
            auto uh = uint128_value(trx);
            valid = (uh <= static_cast<uint128_t>(uinteger_max));
            value = valid ? static_cast<uinteger_t>(uh) : uinteger_max;
            break;
        }

        default:
            break;
        }

        return std::pair{valid && (value >= lower) && (value <= upper), value};
    }

    // ChunkKind for a heap-cell-backed Type.  Used by make_clone and any future
    // path that needs to derive a kind tag from an existing Object's Type.  Covers
    // both ExtValue (Long/ULong/Address/Double) and WideValue (Int128/UInt128)
    // payloads in one table; non-heap-backed Types map to ChunkKind::Other.
    //
    // Indexed by `+type` (Trix idiom for unscoped enum->int conversion).  The
    // table is built once at constexpr-time via a lambda; ChunkKind::Other has
    // value 0, so zero-initialising the array gives the right default for every
    // Type that doesn't have a heap-cell payload.
    static constexpr auto sm_heap_kind = [] {
        std::array<ChunkKind, TypeCount> a{};  // all entries default to ChunkKind::Other (=0)
        a[+Type::Long] = ChunkKind::Long;
        a[+Type::ULong] = ChunkKind::ULong;
        a[+Type::Address] = ChunkKind::Address;
        a[+Type::Double] = ChunkKind::Double;
        a[+Type::Int128] = ChunkKind::Int128;
        a[+Type::UInt128] = ChunkKind::UInt128;
        return a;
    }();

    [[nodiscard]] static constexpr ChunkKind heap_kind_of(Type t) { return sm_heap_kind[+t]; }

    // m_extvalue_save_level for a freshly-allocated cell: BASE when the cell
    // landed in global VM (save/restore doesn't reclaim it; level is inert),
    // current save level otherwise.  Used by every make_long/make_ulong/etc.
    // factory below; centralises the global-vs-local stamp.
    [[nodiscard]] static save_level_t extvalue_save_level_for_alloc(Trix *trx, vm_offset_t offset) {
        return (trx->is_global(offset) ? Save::BASE : trx->m_curr_save_level);
    }

    [[nodiscard]] static Object make_long(Trix *trx, long_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Long);
        object.m_object_save_level = Save::BASE;
        object.m_long = ExtValue::make(trx, value);
        object.m_extvalue_save_level = extvalue_save_level_for_alloc(trx, object.m_long);
        return object;
    }

    [[nodiscard]] bool is_long() const { return (type() == Type::Long); }

    void update_long(Trix *trx, long_t value) {
        assert(is_long());

        auto ext_value = extvalue(trx);
        ext_value->m_long = value;
    }

    [[nodiscard]] long_t long_value(Trix *trx) const {
        assert(is_long());

        auto ext_value = extvalue(trx);
        return ext_value->m_long;
    }

    [[nodiscard]] static Object make_ulong(Trix *trx, ulong_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::ULong);
        object.m_object_save_level = Save::BASE;
        object.m_ulong = ExtValue::make(trx, value);
        object.m_extvalue_save_level = extvalue_save_level_for_alloc(trx, object.m_ulong);
        return object;
    }

    [[nodiscard]] bool is_ulong() const { return (type() == Type::ULong); }

    void update_ulong(Trix *trx, ulong_t value) {
        assert(is_ulong());

        auto ext_value = extvalue(trx);
        ext_value->m_ulong = value;
    }

    [[nodiscard]] ulong_t ulong_value(Trix *trx) const {
        assert(is_ulong());

        auto ext_value = extvalue(trx);
        return ext_value->m_ulong;
    }

    [[nodiscard]] static Object make_int128(Trix *trx, int128_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Int128);
        object.m_object_save_level = Save::BASE;
        object.m_int128 = WideValue::make(trx, value);
        object.m_extvalue_save_level = extvalue_save_level_for_alloc(trx, object.m_int128);
        return object;
    }

    [[nodiscard]] bool is_int128() const { return (type() == Type::Int128); }

    void update_int128(Trix *trx, int128_t value) {
        assert(is_int128());

        widevalue(trx)->write_int128(value);
    }

    [[nodiscard]] int128_t int128_value(Trix *trx) const {
        assert(is_int128());

        return widevalue(trx)->read_int128();
    }

    [[nodiscard]] static Object make_uint128(Trix *trx, uint128_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::UInt128);
        object.m_object_save_level = Save::BASE;
        object.m_uint128 = WideValue::make(trx, value);
        object.m_extvalue_save_level = extvalue_save_level_for_alloc(trx, object.m_uint128);
        return object;
    }

    [[nodiscard]] bool is_uint128() const { return (type() == Type::UInt128); }

    void update_uint128(Trix *trx, uint128_t value) {
        assert(is_uint128());

        widevalue(trx)->write_uint128(value);
    }

    [[nodiscard]] uint128_t uint128_value(Trix *trx) const {
        assert(is_uint128());

        return widevalue(trx)->read_uint128();
    }

    [[nodiscard]] static Object make_address(Trix *trx, address_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Address);
        object.m_object_save_level = Save::BASE;
        object.m_address = ExtValue::make(trx, value);
        object.m_extvalue_save_level = extvalue_save_level_for_alloc(trx, object.m_address);
        return object;
    }

    [[nodiscard]] bool is_address() const { return (type() == Type::Address); }

    void update_address(Trix *trx, address_t value) {
        assert(is_address());

        auto ext_value = extvalue(trx);
        ext_value->m_address = value;
        clear_address_cache();
    }

    [[nodiscard]] address_t address_value(Trix *trx) const {
        assert(is_address());

        auto ext_value = extvalue(trx);
        return ext_value->m_address;
    }

    // Address cache: F=0,X=0 uncached; F=0,X=1 invalid/null;
    //                F=1,X=0 read-only; F=1,X=1 read-write.
    [[nodiscard]] bool has_address_cache() const {
        assert(is_address());

        return ((m_aat & (SpecialFlag | AttribMask)) != 0);
    }

    [[nodiscard]] AddressState cached_address_state() const {
        assert(is_address() && has_address_cache());

        auto bits = m_aat & (SpecialFlag | AttribMask);
        if (bits == AttribMask) {
            return AddressState::Invalid;
        } else if (bits == SpecialFlag) {
            return AddressState::IsReadOnly;
        } else {
            return AddressState::IsReadWrite;
        }
    }

    void set_address_cache(AddressState state) {
        assert(is_address());

        // Clear both bits first, then set the appropriate pattern.
        m_aat &= ~(SpecialFlag | AttribMask);
        switch (state) {
        case AddressState::Invalid:
        case AddressState::IsNullPtr:
            m_aat |= AttribMask;  // F=0, X=1
            break;

        case AddressState::IsReadOnly:
            m_aat |= SpecialFlag;  // F=1, X=0
            break;

        case AddressState::IsReadWrite:
            m_aat |= (SpecialFlag | AttribMask);  // F=1, X=1
            break;
        }
    }

    void clear_address_cache() {
        assert(is_address());

        m_aat &= ~(SpecialFlag | AttribMask);
    }

    [[nodiscard]] static constexpr Object make_real(real_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Real);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_real = value;
        return object;
    }

    [[nodiscard]] bool is_real() const { return (type() == Type::Real); }

    void update_real(real_t value) {
        assert(is_real());

        m_real = value;
    }

    [[nodiscard]] real_t real_value() const {
        assert(is_real());

        return m_real;
    }

    [[nodiscard]] static Object make_double(Trix *trx, double_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Double);
        object.m_object_save_level = Save::BASE;
        object.m_double = ExtValue::make(trx, value);
        object.m_extvalue_save_level = extvalue_save_level_for_alloc(trx, object.m_double);
        return object;
    }

    [[nodiscard]] bool is_double() const { return (type() == Type::Double); }

    void update_double(Trix *trx, double_t value) {
        assert(is_double());

        auto ext_value = extvalue(trx);
        ext_value->m_double = value;
    }

    [[nodiscard]] double_t double_value(Trix *trx) const {
        assert(is_double());

        auto ext_value = extvalue(trx);
        return ext_value->m_double;
    }

    [[nodiscard]] bool is_number_zero(Trix *trx) const {
        assert(is_number());

        switch (+type()) {
        case +Type::Byte: {
            auto b = byte_value();
            return (b == 0);
        }

        case +Type::Integer: {
            auto i = integer_value();
            return (i == 0);
        }

        case +Type::UInteger: {
            auto ui = uinteger_value();
            return (ui == 0);
        }

        case +Type::Long: {
            auto l = long_value(trx);
            return (l == 0);
        }

        case +Type::ULong: {
            auto ul = ulong_value(trx);
            return (ul == 0);
        }

        case +Type::Int128: {
            auto i = int128_value(trx);
            return (i == 0);
        }

        case +Type::UInt128: {
            auto u = uint128_value(trx);
            return (u == 0);
        }

        case +Type::Real: {
            auto r = real_value();
            return (std::fpclassify(r) == FP_ZERO);
        }

        case +Type::Double: {
            auto d = double_value(trx);
            return (std::fpclassify(d) == FP_ZERO);
        }

        default:
            break;
        }

        std::unreachable();
    }

    [[nodiscard]] bool is_number_negative(Trix *trx) const {
        assert(is_signed_number());

        switch (+type()) {
        case +Type::Integer: {
            auto i = integer_value();
            return (i < 0);
        }

        case +Type::Long: {
            auto l = long_value(trx);
            return (l < 0);
        }

        case +Type::Int128: {
            auto i = int128_value(trx);
            return (i < 0);
        }

        case +Type::Real: {
            auto r = real_value();
            return (r < 0.0f);
        }

        case +Type::Double: {
            auto d = double_value(trx);
            return (d < 0.0);
        }

        default:
            break;
        }
        std::unreachable();
    }

    // Apply a predicate to a floating-point value (Real or Double).
    // Pred must be callable with both real_t and double_t arguments.
    template<typename Pred>
    [[nodiscard]] bool floating_point_test(Trix *trx, Pred pred) const {
        assert(is_floating_point());

        switch (+type()) {
        case +Type::Real:
            return pred(real_value());

        case +Type::Double:
            return pred(double_value(trx));

        default:
            break;
        }
        std::unreachable();
    }

    [[nodiscard]] bool is_floating_point_inf(Trix *trx) const {
        return floating_point_test(trx, [](auto v) { return std::isinf(v); });
    }

    // num is NaN
    [[nodiscard]] bool is_floating_point_nan(Trix *trx) const {
        return floating_point_test(trx, [](auto v) { return std::isnan(v); });
    }

    [[nodiscard]] bool is_floating_point_is_abs_lesseq_one(Trix *trx) const {
        return floating_point_test(trx, [](auto v) { return (std::abs(v) <= decltype(v)(1.0)); });
    }

    // |num| < 1
    [[nodiscard]] bool is_floating_point_is_abs_less_one(Trix *trx) const {
        return floating_point_test(trx, [](auto v) { return (std::abs(v) < decltype(v)(1.0)); });
    }

    // num >= 1
    [[nodiscard]] bool is_floating_point_greater_eq_one(Trix *trx) const {
        return floating_point_test(trx, [](auto v) { return (v >= decltype(v)(1.0)); });
    }

    // num > -1
    [[nodiscard]] bool is_floating_point_greater_than_neg_one(Trix *trx) const {
        return floating_point_test(trx, [](auto v) { return (v > decltype(v)(-1.0)); });
    }

    // num is 0 or a negative integer
    [[nodiscard]] bool is_floating_point_nonpositive_integer(Trix *trx) const {
        return floating_point_test(trx, [](auto v) { return (v <= decltype(v)(0.0)) && (v == std::trunc(v)); });
    }

    // num is normal, (not subnormal, zero, infinite, NaN)
    [[nodiscard]] bool is_floating_point_normal(Trix *trx) const {
        return floating_point_test(trx, [](auto v) { return std::isnormal(v); });
    }

    // num has finite value (normal, subnormal, zero), (not infinite, NaN)
    [[nodiscard]] bool is_floating_point_finite(Trix *trx) const {
        return floating_point_test(trx, [](auto v) { return std::isfinite(v); });
    }

    // True if this numeric Object has value zero.  Works for all numeric types
    // (Byte, Integer, UInteger, Long, ULong, Int128, UInt128, Real, Double, Address).
    // Returns false for non-numeric types.
    [[nodiscard]] bool is_numeric_zero(Trix *trx) const {
        switch (+type()) {
        case +Type::Byte:
            return (byte_value() == 0);

        case +Type::Integer:
            return (integer_value() == 0);

        case +Type::UInteger:
            return (uinteger_value() == 0);

        case +Type::Long:
            return (long_value(trx) == 0);

        case +Type::ULong:
            return (ulong_value(trx) == 0);

        case +Type::Int128:
            return (int128_value(trx) == 0);

        case +Type::UInt128:
            return (uint128_value(trx) == 0);

        case +Type::Real:
            return (real_value() == 0.0f);

        case +Type::Double:
            return (double_value(trx) == 0.0);

        case +Type::Address:
            return (address_value(trx) == nullptr);

        default:
            return false;
        }
    }

    [[nodiscard]] static constexpr Object make_boolean(boolean_t value) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Boolean);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_boolean = value;
        return object;
    }

    [[nodiscard]] bool is_boolean() const { return (type() == Type::Boolean); }

    void update_boolean(boolean_t value) {
        assert(is_boolean());

        m_boolean = value;
    }

    [[nodiscard]] boolean_t boolean_value() const {
        assert(is_boolean());
        assert((m_boolean == 0) || (m_boolean == 1));

        return m_boolean;
    }

    [[nodiscard]] static constexpr Object make_operator(SystemName sysname) {
        assert(((sysname >= SystemName::FIRST_STD_OP) && (sysname <= SystemName::LAST_STD_OP)) ||
               ((sysname >= SystemName::FIRST_PLACEHOLDER_OP) && (sysname <= SystemName::LAST_PLACEHOLDER_OP)));

        Object object;
        object.m_aat = (ExecutableAttrib | +Type::Operator);
        object.m_object_save_level = Save::BASE;
        object.m_operator_data.m_flags = StandardOp;
        object.m_operator_data.m_popcount = 0;
        object.m_operator = static_cast<operator_index_t>(+sysname);
        return object;
    }

    [[nodiscard]] bool is_operator() const { return (type() == Type::Operator); }

    [[nodiscard]] static Object make_control_operator(SystemName sysname) {
        assert((sysname >= SystemName::FIRST_CONTROL_OP) && (sysname <= SystemName::LAST_CONTROL_OP));

        auto [flags, popcount] = [&]() -> std::pair<operator_flags_t, operator_popcount_t> {
            switch (+sysname) {
            case +SystemName::atRun:
                // run: exec stack barrier for the `run` operator
                return std::pair{Object::RunOp, 0};

            case +SystemName::atStop:
                // stop/stopped: exec stack barrier for stop/stopped
                return std::pair{Object::StopOp, 0};

            case +SystemName::atLoop:
                // loop: [proc] [@loop]
            case +SystemName::atDoWhile:
                // do-while: [body] [@do-while]
                return std::pair{Object::ControlOp, 2};

            case +SystemName::atUIntegerRepeat:
                // repeat: [proc] [count] [@uinteger-repeat]
            case +SystemName::atULongRepeat:
                // repeat: [proc] [count] [@ulong-repeat]
            case +SystemName::atArrayAny:
                // any: [proc] [src-arr] [@array-any]
            case +SystemName::atArrayAll:
                // all: [proc] [src-arr] [@array-all]
            case +SystemName::atWhile:
                // while: [body] [cond] [@while]
                return std::pair{Object::ControlOp, 3};

            case +SystemName::atArrayForAll:
                // for-all: [proc] [array] [saved-depth] [@array-for-all]
            case +SystemName::atPackedForAll:
                // for-all: [proc] [packed] [saved-depth] [@packed-for-all]
            case +SystemName::atStringForAll:
                // for-all: [proc] [string] [saved-depth] [@string-for-all]
                return std::pair{Object::ControlOp, 4};

            case +SystemName::atArrayFind:
                // find: [proc] [src-array] [saved-element] [@array-find]
            case +SystemName::atArrayCount:
                // count: [proc] [src-array] [counter] [@array-count]
                return std::pair{Object::ControlOp, 4};

            case +SystemName::atDictForAll:
                // dict-for-all: [proc] [dict] [bucket-idx] [entry-offset] [saved-depth] [@dict-for-all]
                return std::pair{Object::DictForAllOp | Object::ControlOp, 6};

            case +SystemName::atSetForAll:
                // set-for-all: [proc] [set] [bucket-idx] [entry-offset] [saved-depth] [@set-for-all]
                return std::pair{Object::ControlOp, 6};

            case +SystemName::atDictMap:
                // dict-map: [proc] [result-dict] [src-dict] [entry-offset] [bucket-idx] [saved-key] [@dict-map]
                return std::pair{Object::ControlOp, 7};

            case +SystemName::atDictFilter:
                // dict-filter: [proc] [result-dict] [src-dict] [entry-offset] [bucket-idx] [saved-key] [saved-value]
                // [@dict-filter]
                return std::pair{Object::ControlOp, 8};

            case +SystemName::atSetFilter:
                // set-filter: [proc] [result-set] [src-set] [entry-offset] [bucket-idx] [saved-key] [@set-filter]
                return std::pair{Object::ControlOp, 7};

            case +SystemName::atByteFor:
            case +SystemName::atIntegerFor:
            case +SystemName::atUIntegerFor:
            case +SystemName::atLongFor:
            case +SystemName::atULongFor:
            case +SystemName::atAddressFor:
            case +SystemName::atRealFor:
            case +SystemName::atDoubleFor:
                // for: [proc] [limit] [increment] [control] [@*-for]
                return std::pair{Object::ControlOp, 5};

            case +SystemName::atArrayMap:
                // map: [proc] [src] [dst] [write-idx] [@array-map]
            case +SystemName::atArrayMapIndexed:
                // map-indexed: [proc] [src] [dst] [write-idx] [@array-map-indexed]
                return std::pair{Object::ControlOp, 5};

            case +SystemName::atArrayMinBy:
            case +SystemName::atArrayMaxBy:
                // min-by/max-by: [proc] [src] [best-elem] [best-key] [saved-elem] [@array-min/max-by]
                return std::pair{Object::ControlOp, 6};

            case +SystemName::atArraySortBy:
                // sort-by: [proc] [src] [keys-arr] [elems-arr] [index] [@array-sort-by]
                return std::pair{Object::ControlOp, 6};

            case +SystemName::atArrayScan:
                // scan: [proc] [src] [result-arr] [accumulator] [index] [@array-scan]
                return std::pair{Object::ControlOp, 6};

            case +SystemName::atArrayFilter:
                // filter: [proc] [src-array] [dst-array] [write-index] [saved-element] [@array-filter]
                return std::pair{Object::ControlOp, 6};

            case +SystemName::atArrayGroupBy:
                // group-by: [proc] [src] [keys-arr] [elems-arr] [index] [@array-group-by]
                return std::pair{Object::ControlOp, 6};

            case +SystemName::atArrayTakeWhile:
                // take-while: [proc] [src] [dst-array] [write-index] [saved-elem] [@array-take-while]
                return std::pair{Object::ControlOp, 6};

            case +SystemName::atArrayDropWhile:
                // drop-while: [proc] [src] [saved-elem] [@array-drop-while]
                return std::pair{Object::ControlOp, 4};

            case +SystemName::atArrayPartition:
                // partition: [proc] [src] [dst-pass] [pass-idx] [dst-fail] [fail-idx] [saved-elem] [@array-partition]
                return std::pair{Object::ControlOp, 8};

            case +SystemName::atTryBarrier:
                // try: exec stack barrier for error recovery (pushes /error-name)
            case +SystemName::atTryCatchBarrier:
                // try-catch: exec stack barrier with error handler dict (pushes /error-name)
            case +SystemName::atReplBarrier:
                // repl-barrier: top-level REPL error recovery (pushes /error-name)
                return std::pair{Object::BarrierOp | Object::ErrorPushOp, 0};

            case +SystemName::atFinallyBarrier:
                // finally: exec stack barrier with cleanup block on error stack
            case +SystemName::atWithStream:
                // with-stream: exec stack barrier; closes stream on error stack
            case +SystemName::atChoiceBarrier:
                // choice: exec stack barrier for backtracking
            case +SystemName::atCellEval:
                // cell-eval: exec stack barrier for computed cell re-evaluation
            case +SystemName::atBatchEnd:
                // batch-end: exec stack barrier for deferred watcher firing
            case +SystemName::atBatchFire:
                // batch-fire: exec stack barrier for post-recomputation watcher firing
            case +SystemName::atChoiceCountBarrier:
                // choice-count: exec stack barrier for success counting
            case +SystemName::atFindAllBarrier:
                // find-all: exec stack barrier for solution collection
            case +SystemName::atFindNBarrier:
                // find-n: exec stack barrier for bounded solution collection
            case +SystemName::atForEachSolutionBarrier:
                // for-each-solution: exec stack barrier for streaming iteration
            case +SystemName::atAggregateBarrier:
                // aggregate: exec stack barrier for fold-phase-1 (alt attempt)
            case +SystemName::atAggregateReduceBarrier:
                // aggregate: exec stack barrier for fold-phase-2 (reducer)
            case +SystemName::atOnceBarrier:
                // once: exec stack barrier for single-attempt choice
            case +SystemName::atNafBarrier:
                // naf: exec stack barrier for negation-as-failure
            case +SystemName::atUnifyMatchBarrier:
                // unify-match: exec stack barrier for pattern dispatch
            case +SystemName::atForceComplete:
                // force: exec stack barrier for thunk evaluation.  On error the
                // unwinder swaps to @force-fail, which resets the thunk state
                // from Evaluating back to Unevaluated so the caller can retry.
                // Companion below the barrier: thunk-literal (so the fail
                // handler can identify which thunk to reset).
                return std::pair{Object::BarrierOp, 0};

            case +SystemName::atEndLocals:
                // end-locals: pops frame dict from dict stack + recycles
            case +SystemName::atEndModule:
                // end-module: pops module dict from dict stack, makes ReadOnly, registers
            case +SystemName::atClosureEnd:
                // closure-end: pops closure dict from dict stack
                return std::pair{Object::DictPopOp, 0};

            case +SystemName::atCatchError:
                // catch-error: error handler proc pushed by try-catch
            case +SystemName::atFinallyReraise:
                // finally: re-raises original error after finally-block completes
            case +SystemName::atCall:
                // call: popped by at_call_op itself (pops name + SourceLoc from exec stack)
            case +SystemName::atDip:
                // dip/keep: hidden value restored to operand stack when quotation finishes
            case +SystemName::atUpdate:
                // update: [dict] [key] [@update] -- stores proc result back to dict
            case +SystemName::atUpdatePersist:
                // update-persist: [dict] [key] [@update-persist] -- non-journaled put_persist of proc result
            case +SystemName::atCheckModule:
                // check-module: verifies a module was registered after require completes
            case +SystemName::atTime:
                // time: [start-timestamp] [@time]
            case +SystemName::atHandlerDone:
                // handler-done: clears m_in_global_handler after error handler returns
            case +SystemName::atReplRecover:
                // repl-recover: replaces @repl-barrier after an error; prints and re-arms
                // StandardOp: exit/stop walk past without stopping (not a loop boundary)
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atCoroutineAwaitCheck:
                // coroutine-await: resume check -- target dead? push result / rethrow / re-sleep
            case +SystemName::atCoroutineComplete:
                // coroutine: exec stack sentinel -- coroutine completion handler
            case +SystemName::atCoroutineJoinCheck:
                // coroutine-join: resume check -- target dead? push result or re-sleep
            case +SystemName::atCoroutineWaitAllCheck:
                // coroutine-wait-all: resume check -- all done? or re-sleep
            case +SystemName::atTagUpdateComplete:
                // tag-update: re-tags proc result with saved tag name
                // exec stack layout: [tag-name] [@tag-update-complete]
            case +SystemName::atTryResultComplete:
                // try-result: wraps proc result in /ok or /err tagged value
                // exec stack layout: [saved-depth] [@try-result-complete]
            case +SystemName::atTryRollback:
                // try: on error path, rolls operand stack back to saved depth
                // and replants /error-name on top.  exec layout below
                // @try-barrier: [saved-depth] [@try-rollback].
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atRecordCtor:
                // @record-ctor: pops schema_offset from op stack, builds record
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atRecordForAll:
                // record-for-all: [proc] [record] [field-idx] [saved-depth] [@record-for-all]
                return std::pair{Object::ControlOp, 5};

            case +SystemName::atRecordMapFieldComplete:
                // record-map-field: [field-idx] [record] [@record-map-field-complete]
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atRecordMapStep:
                // record-map: [proc] [record] [new-offset] [field-idx] [@record-map-step]
                return std::pair{Object::ControlOp, 5};

                // --- Lazy sequence internal control operators ---

            case +SystemName::atLazyMapBuild:
                // lazy-map: [tail-thunk] [@lazy-map-build]
            case +SystemName::atLazyIterateBuild:
                // lazy-iterate: [seed-thunk] [@lazy-iterate-build]
            case +SystemName::atLazyUnfoldBuild:
                // lazy-unfold: [seed proc] [@lazy-unfold-build]
            case +SystemName::atLazyScanBuild:
                // lazy-scan: [tail-thunk init proc] [@lazy-scan-build]
            case +SystemName::atLazyMapIndexedBuild:
                // lazy-map-indexed: [tail-thunk index] [@lazy-map-indexed-build]
            case +SystemName::atLazyZipWithBuild:
                // lazy-zip-with: [tail1 tail2 proc] [@lazy-zip-with-build]
            case +SystemName::atLazyTakeWhileTest:
                // lazy-take-while: [head tail-thunk pred] [@lazy-take-while-test]
            case +SystemName::atLazyFlatMapBuild:
                // lazy-flat-map: [outer-tail proc] [@lazy-flat-map-build]
            case +SystemName::atLazyFlatMapResume:
                // lazy-flat-map: [outer-tail proc] [@lazy-flat-map-resume]
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atLazyFilterTest:
                // lazy-filter: [head tail-thunk pred] [@lazy-filter-test]
            case +SystemName::atLazyFilterResume:
                // lazy-filter: [tail-thunk pred] [@lazy-filter-resume]
            case +SystemName::atLazyFilterMapTest:
                // lazy-filter-map: [tail-thunk proc] [@lazy-filter-map-test]
            case +SystemName::atLazyFilterMapResume:
                // lazy-filter-map: [tail-thunk proc] [@lazy-filter-map-resume]
            case +SystemName::atLazyDropWhileTest:
                // lazy-drop-while: [head tail-thunk pred] [@lazy-drop-while-test]
            case +SystemName::atLazyDropWhileResume:
                // lazy-drop-while: [tail-thunk pred] [@lazy-drop-while-resume]
            case +SystemName::atLazyDropStep:
                // lazy-drop: [n] [@lazy-drop-step]
            case +SystemName::atLazyChunkedCollect:
                // lazy-chunked: [dst write-idx n tail-thunk] [@lazy-chunked-collect]
            case +SystemName::atLazyFoldStep:
                // lazy-fold: [acc proc] [@lazy-fold-step]
            case +SystemName::atLazyForEachStep:
                // lazy-for-each: [proc] [@lazy-for-each-step]
            case +SystemName::atLazyAnyStep:
                // lazy-any: [pred] [@lazy-any-step]
            case +SystemName::atLazyAllStep:
                // lazy-all: [pred] [@lazy-all-step]
            case +SystemName::atLazyFindStep:
                // lazy-find: [pred] [@lazy-find-step]
            case +SystemName::atLazyFindIndexStep:
                // lazy-find-index: [index pred] [@lazy-find-index-step]
            case +SystemName::atLazyToArrayStep:
                // lazy-to-array: [resume-curr] [@lazy-to-array-step]
            case +SystemName::atLazyCountStep:
                // lazy-count: [resume-curr count] [@lazy-count-step]
            case +SystemName::atLazyNthHead:
                // lazy-nth: [] [@lazy-nth-head]
            case +SystemName::atLazyMapImpl:
            case +SystemName::atLazyFilterImpl:
            case +SystemName::atLazyTakeWhileImpl:
            case +SystemName::atLazyUnfoldImpl:
            case +SystemName::atLazyFilterMapImpl:
            case +SystemName::atLazyFlatMapImpl:
            case +SystemName::atLazyZipWithImpl:
                // internal recursive entry points (no verify, just require_op_count)
                return std::pair{Object::StandardOp, 0};

                // --- Pipeline internal control operators ---

            case +SystemName::atPipePutRetry:
                // pipe-put retry: [buffer] [@pipe-put-retry]
            case +SystemName::atPipeGetRetry:
                // pipe-get retry: [buffer] [@pipe-get-retry]
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atPipeStage:
                // pipe-stage: mid-stage batch accumulation loop
            case +SystemName::atPipeCleanup:
                // pipe-cleanup: error propagation after try-barrier
                // exec stack: [output-buf] [@pipe-cleanup]
                return std::pair{Object::StandardOp, 0};

                // --- Actor internal control operators ---

            case +SystemName::atActorSendRetry:
                // actor-send retry: [target] [message] [@actor-send-retry]
            case +SystemName::atActorRecvCheck:
                // actor-recv check: [@actor-recv-check]
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atActorRecvTimeoutCheck:
                // actor-recv-timeout check: [deadline-obj] [@actor-recv-timeout-check]
            case +SystemName::atActorRecvMatchCheck:
                // actor-recv-match check: [pred] [@actor-recv-match-check]
            case +SystemName::atActorRecvMatchTimeoutCheck:
                // actor-recv-match-timeout check: [pred] [@actor-recv-match-timeout-check]
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atReadKeyRetry:
                // read-key-byte wakeup: scheduler resumed coroutine after stdin
                // became readable.  Retry the read; no companions on err stack.
            case +SystemName::atReadKeyTimeoutRetry:
                // read-key-byte-timeout wakeup: scheduler resumed coroutine
                // after stdin became readable OR the timeout fired.  Retry
                // the read; on EAGAIN the timer expired -> push false.
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atSupervisorCheck:
                // supervisor recv-match wakeup handler
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atSupervisorInit:
                // supervisor initialization: spawn children, enter recv loop
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atChoiceFail:
                // choice point: failure handler (restore, try next alternative)
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atCellEvalFail:
                // cell-get: computed cell evaluation failed -- restore m_current_cell
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atBatchFail:
                // batch: failure -- discard deferred watchers, re-raise
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atBatchFireFail:
                // batch-fire: failure during post-recomputation watcher firing
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atCellUpdateDone:
                // cell-update: continuation after proc completes -- 1 companion (cell_offset)
                return std::pair{Object::ControlOp, 1};

            case +SystemName::atCellValidateInit:
                // cell-validated: initial value validation -- 0 companions (value+proc on op stack)
                return std::pair{Object::ControlOp, 0};

            case +SystemName::atCellValidateSet:
                // cell-set: validator check -- 1 companion (cell_offset)
                return std::pair{Object::ControlOp, 1};

            case +SystemName::atChoiceCountFail:
                // choice-count: failure -- restore, advance
            case +SystemName::atFindAllFail:
                // find-all: failure -- restore, skip, try next
            case +SystemName::atFindNFail:
                // find-n: failure -- restore, skip, try next
            case +SystemName::atForEachSolutionFail:
                // for-each-solution: failure -- restore, advance
            case +SystemName::atAggregateFail:
                // aggregate: alt failure -- restore, advance
            case +SystemName::atAggregateReduceFail:
                // aggregate: reducer failure -- restore, clean up, re-raise
            case +SystemName::atOnceFail:
                // once: failure -- restore, re-raise
            case +SystemName::atNafFail:
                // naf: failure -- restore, push true
            case +SystemName::atUnifyMatchFail:
                // unify-match: failure -- restore, re-raise
            case +SystemName::atForceFail:
                // force: failure -- reset thunk state to Unevaluated, re-raise
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atMatchTest:
                // match/cond: test result dispatch -- [value] [pairs] [@match-test]
                return std::pair{Object::ControlOp, 2};

            case +SystemName::atMatchAllTest:
                // match-all: two-phase test/collect dispatch --
                // [value] [pairs] [state] [@match-all-test].  ExitFenceOp:
                // exit inside a test or body cannot reconcile the
                // accumulation Mark, so it raises /invalid-exit instead of
                // silently leaking it (use throw/try for early termination).
                return std::pair{static_cast<operator_flags_t>(Object::ControlOp | Object::ExitFenceOp), 3};

            case +SystemName::atEnsureCheck:
                // ensure: [check-proc] below control op.
                // StandardOp so exit_op does not treat it as a loop boundary
                // (postcondition body should be transparent to exit).
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atGenServerInit:
                // gen-server init: no frame yet (frame built by this op)
                return std::pair{Object::ControlOp, 0};

            case +SystemName::atGenServerRecv:
            case +SystemName::atGenServerCallDone:
            case +SystemName::atGenServerCastDone:
                // gen-server: [spec, state, ref, from] = 4-slot frame
                return std::pair{Object::ControlOp, 4};

            case +SystemName::atGenCallTimeoutDone:
                // gen-call-timeout: no companions (result is on op stack)
                return std::pair{Object::StandardOp, 0};

            case +SystemName::atDelimitBarrier:
                // delimit: barrier on exec stack, no companions
                return std::pair{Object::BarrierOp, 0};

            case +SystemName::atEffectBarrier:
                // effect: barrier on exec stack, handler-dict companion on error stack
                return std::pair{Object::BarrierOp, 0};

            case +SystemName::atHandlerScope:
                // handler-scope: barrier on exec stack marking the boundary
                // abort-exec trims to.  Two err companions: (K identity,
                // op-depth at handler-push).  Pushed by capture_op above the
                // handler frame; popped by at_handler_scope_op on normal
                // return, or by unwind_exec_to / abort-exec / error-unwind
                // when the scope is being discarded.
                return std::pair{Object::BarrierOp, 0};

            case +SystemName::atInGlobal:
                // in-global runtime: not user-pronounceable -- emitted by
                // the scanner as the runtime side of a ${...} block.
                // Pops the body proc from operand stack, saves the prior
                // m_curr_alloc_global on err stack, sets flag true, pushes
                // @end-in-global + body-proc on exec stack.  Standard
                // operator semantics (ControlOp flag for non-user-name).
                return std::pair{Object::ControlOp, 0};

            case +SystemName::atInLocal:
                // in-local runtime: symmetric inverse of @in-global,
                // emitted by the scanner as the runtime side of a
                // $${...} block.  Identical shape; sets the flag FALSE
                // instead of true.  Shares @end-in-global as closer.
                return std::pair{Object::ControlOp, 0};

            case +SystemName::atEndInGlobal:
                // in-global cleanup: not an error catcher.  On normal
                // completion, pops a saved Boolean from the err stack and
                // restores m_curr_alloc_global.  On error unwind, the
                // try-catch handler does the same restore + keep searching.
                return std::pair{Object::BarrierOp, 0};

#ifdef TRIX_DEBUGGER
            case +SystemName::atDebugErrorResume:
                // break-on-error session tail: restores the parked fatal
                // error and re-enters global_handler.  No companions; not
                // an error catcher (a session error that unwinds past it
                // just discards it -- global_handler's armed flag copes).
                return std::pair{Object::ControlOp, 0};
#endif

            default:
                assert(false && "make_control_operator: unknown sysname");
                std::unreachable();
            }
        }();

        Object object;
        object.m_aat = (ExecutableAttrib | +Type::Operator);
        object.m_object_save_level = Save::BASE;
        object.m_operator_data.m_flags = flags;
        object.m_operator_data.m_popcount = popcount;
        object.m_operator = static_cast<operator_index_t>(+sysname);
        return object;
    }

    [[nodiscard]] static constexpr Object make_user_operator(operator_index_t value) {
        assert(value < 0);

        Object object;
        object.m_aat = (ExecutableAttrib | +Type::Operator);
        object.m_object_save_level = Save::BASE;
        object.m_operator_data.m_flags = StandardOp;
        object.m_operator_data.m_popcount = 0;
        object.m_operator = value;
        return object;
    }

    [[nodiscard]] std::string_view operator_string(const Trix *trx) const {
        assert(is_operator());

        if (m_operator >= 0) {
            auto op = sysname_value(static_cast<name_index_t>(m_operator));
            return op.m_sv;
        } else {
            assert(trx->m_useroperators != nullptr);
            return trx->m_useroperators[(-m_operator) - 1].m_sv;
        }
    }

    void operator_execute(Trix *trx) const {
        assert(is_operator());
        assert((m_operator < 0) ? (trx->m_useroperators != nullptr) : true);

        *trx->m_last_operator_ptr = *this;
        Operator op;
        if (m_operator >= 0) [[likely]] {
            op = sysname_value(static_cast<name_index_t>(m_operator));
        } else {
            op = trx->m_useroperators[(-m_operator) - 1];
        }
        if (op.m_func == nullptr) [[unlikely]] {
            trx->error(Error::InternalError, "operator function is null for operator index {}", m_operator);
        } else {
            op.m_func(trx);
        }
    }

    [[nodiscard]] bool operator_is_std() const {
        assert(is_operator());

        return (m_operator_data.m_flags == StandardOp);
    }

    [[nodiscard]] bool operator_is_control() const {
        assert(is_operator());

        return ((m_operator_data.m_flags & ControlOp) != 0);
    }

    [[nodiscard]] bool operator_is_run() const {
        assert(is_operator());

        return ((m_operator_data.m_flags & RunOp) != 0);
    }

    [[nodiscard]] bool operator_is_stop() const {
        assert(is_operator());

        return ((m_operator_data.m_flags & StopOp) != 0);
    }

    [[nodiscard]] bool operator_is_dictforall() const {
        assert(is_operator());

        return ((m_operator_data.m_flags & DictForAllOp) != 0);
    }

    [[nodiscard]] bool operator_is_forall_family() const {
        // Six iteration-frame barriers that share the
        //   [proc][container][saved_depth][<barrier>]
        // exec layout (saved_depth at scan-1).  Used by the
        // ContinuationContext rewrite-table to spot captured iteration
        // frames whose saved_depth needs collapsing to the resumer's
        // segment-start floor.  Each variant runs the same
        // stack-effect check; on staleness the check would fire
        // /range-check on the wrong frame.
        assert(is_operator());

        return ((m_operator == +SystemName::atArrayForAll) || (m_operator == +SystemName::atPackedForAll) ||
                (m_operator == +SystemName::atStringForAll) || (m_operator == +SystemName::atDictForAll) ||
                (m_operator == +SystemName::atSetForAll) || (m_operator == +SystemName::atRecordForAll));
    }

    [[nodiscard]] bool operator_is_barrier() const {
        assert(is_operator());

        return ((m_operator_data.m_flags & BarrierOp) != 0);
    }

    [[nodiscard]] bool operator_is_errorpush() const {
        assert(is_operator());

        return ((m_operator_data.m_flags & ErrorPushOp) != 0);
    }

    [[nodiscard]] bool operator_is_dictpop() const {
        assert(is_operator());

        return ((m_operator_data.m_flags & DictPopOp) != 0);
    }

    [[nodiscard]] bool operator_is_trybarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atTryBarrier);
    }

    [[nodiscard]] bool operator_is_tryrollback() const {
        assert(is_operator());

        return (m_operator == +SystemName::atTryRollback);
    }

    [[nodiscard]] bool operator_is_tryresultcomplete() const {
        assert(is_operator());

        return (m_operator == +SystemName::atTryResultComplete);
    }

    [[nodiscard]] bool operator_is_trycatchbarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atTryCatchBarrier);
    }

    [[nodiscard]] bool operator_is_replbarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atReplBarrier);
    }

    [[nodiscard]] bool operator_is_finallybarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atFinallyBarrier);
    }

    [[nodiscard]] bool operator_is_withstream() const {
        assert(is_operator());

        return (m_operator == +SystemName::atWithStream);
    }

    [[nodiscard]] bool operator_is_endinglobal() const {
        assert(is_operator());

        return (m_operator == +SystemName::atEndInGlobal);
    }

    [[nodiscard]] bool operator_is_choicebarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atChoiceBarrier);
    }

    [[nodiscard]] bool operator_is_findallbarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atFindAllBarrier);
    }

    [[nodiscard]] bool operator_is_findnbarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atFindNBarrier);
    }

    [[nodiscard]] bool operator_is_oncebarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atOnceBarrier);
    }

    [[nodiscard]] bool operator_is_nafbarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atNafBarrier);
    }

    [[nodiscard]] bool operator_is_unifymatchbarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atUnifyMatchBarrier);
    }

    [[nodiscard]] bool operator_is_choicecountbarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atChoiceCountBarrier);
    }

    [[nodiscard]] bool operator_is_foreachsolutionbarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atForEachSolutionBarrier);
    }

    [[nodiscard]] bool operator_is_aggregatebarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atAggregateBarrier);
    }

    [[nodiscard]] bool operator_is_aggregatereducebarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atAggregateReduceBarrier);
    }

    [[nodiscard]] bool operator_is_celleval() const {
        assert(is_operator());

        return (m_operator == +SystemName::atCellEval);
    }

    [[nodiscard]] bool operator_is_forcecomplete() const {
        assert(is_operator());

        return (m_operator == +SystemName::atForceComplete);
    }

    [[nodiscard]] bool operator_is_batchend() const {
        assert(is_operator());

        return (m_operator == +SystemName::atBatchEnd);
    }

    [[nodiscard]] bool operator_is_batchfire() const {
        assert(is_operator());

        return (m_operator == +SystemName::atBatchFire);
    }

    [[nodiscard]] bool operator_is_delimitbarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atDelimitBarrier);
    }

    [[nodiscard]] bool operator_is_effectbarrier() const {
        assert(is_operator());

        return (m_operator == +SystemName::atEffectBarrier);
    }

    [[nodiscard]] bool operator_is_handlerscope() const {
        assert(is_operator());

        return (m_operator == +SystemName::atHandlerScope);
    }

    [[nodiscard]] bool operator_is_endlocals() const {
        assert(is_operator());

        return (m_operator == +SystemName::atEndLocals);
    }

    [[nodiscard]] bool operator_is_endmodule() const {
        assert(is_operator());

        return (m_operator == +SystemName::atEndModule);
    }

    [[nodiscard]] bool operator_is_closureend() const {
        assert(is_operator());

        return (m_operator == +SystemName::atClosureEnd);
    }

    [[nodiscard]] bool operator_is_loop() const {
        assert(is_operator());

        return (m_operator == +SystemName::atLoop);
    }

    [[nodiscard]] bool operator_is_dowhile() const {
        assert(is_operator());

        return (m_operator == +SystemName::atDoWhile);
    }

    [[nodiscard]] bool operator_is_repeat_family() const {
        assert(is_operator());

        return ((m_operator == +SystemName::atUIntegerRepeat) || (m_operator == +SystemName::atULongRepeat));
    }

    [[nodiscard]] bool operator_is_dip() const {
        assert(is_operator());

        return (m_operator == +SystemName::atDip);
    }

    // operator_is_stop already exists above (flag-based on StopOp).

    [[nodiscard]] bool operator_is_time() const {
        assert(is_operator());

        return (m_operator == +SystemName::atTime);
    }

    [[nodiscard]] bool operator_is_invalid_exit() const {
        assert(is_operator());

        return ((m_operator_data.m_flags & (RunOp | StopOp | ExitFenceOp)) != 0);
    }

    [[nodiscard]] bool operator_is_call() const {
        assert(is_operator());

        return (m_operator == +SystemName::atCall);
    }

    [[nodiscard]] operator_popcount_t operator_popcount() const {
        assert(is_operator());

        return (m_operator_data.m_popcount);
    }

    [[nodiscard]] static constexpr Object make_mark() {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Mark);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_mark = 0;
        return object;
    }

    [[nodiscard]] bool is_mark() const { return (type() == Type::Mark); }

    [[nodiscard]] static constexpr Object make_name(vm_offset_t offset, length_t length, attrib_t attrib = LiteralAttrib) {
        Object object;
        object.m_aat = (attrib | +Type::Name);
        object.m_object_save_level = Save::BASE;
        object.m_name_length = length;
        object.m_name = offset;
        return object;
    }

    [[nodiscard]] bool is_name() const { return (type() == Type::Name); }

    [[nodiscard]] vm_offset_t name_offset() const {
        assert(is_name());

        return m_name;
    }

    [[nodiscard]] Name *name_value(Trix *trx) const {
        assert(is_name());

        return trx->offset_to_ptr<Name>(m_name);
    }

    [[nodiscard]] std::string_view name_sv(Trix *trx) const {
        assert(is_name());

        auto name = trx->offset_to_ptr<Name>(m_name);
        return name->sv();
    }

    void set_name_binding(Trix *trx, const Object *value) const {
        assert(is_name());

        auto value_offset = trx->ptr_to_offset(value);
        if (trx->m_live_coroutine_count == 0) {
            // Single-coroutine fast path (covers VM initialization too --
            // Dict::init runs with count == 0 before any spawn).  Writing to
            // Name::m_binding directly requires no running-coroutine context.
            auto name = name_value(trx);
            name->set_binding(value_offset);
        } else {
            auto running = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
            trx->binding_table_put(&running->m_binding_table, m_name, value_offset);
        }
    }

    void clear_name_binding(Trix *trx) const {
        assert(is_name());

        if (trx->m_live_coroutine_count == 0) {
            // Single-coroutine fast path.
            auto name = name_value(trx);
            name->clear_binding();
        } else {
            // Multi-coroutine: walk every live coroutine's table.  A Name
            // undef'd/out-of-scope for the caller may still be cached by peers
            // sharing the same dict (userdict etc.); leaving stale entries in
            // peer caches would return pointers into an empty DictEntry slot.
            trx->binding_remove_all_coroutines(m_name);
        }
    }

    [[nodiscard]] static constexpr Object
    make_array(vm_offset_t offset, length_t length, attrib_t attrib = LiteralAttrib, access_t access = ReadWriteAccess) {
        Object object;
        object.m_aat = (attrib | access | +Type::Array);
        object.m_object_save_level = Save::BASE;
        object.m_arrays_length = length;
        object.m_array = offset;
        return object;
    }

    [[nodiscard]] bool is_array() const { return (type() == Type::Array); }

    // Eqref detection: SpecialFlag on Array/Packed/String/Dict/Set marks a reference to
    // a shared eq-storage buffer.  The value slot holds the creation-time generation
    // instead of a VM offset; accessors validate against the current per-kind counter
    // and raise Unsupported on a mismatch.  Address uses SpecialFlag for address-cache
    // state and is explicitly excluded.
    //
    // Per-kind disambiguation uses (type, attrib):
    //   executable Array or Packed + SpecialFlag  -> is_eqproc_ref     (}#=)
    //   literal    Array           + SpecialFlag  -> is_eqarray_ref    (]#=)
    //   String                     + SpecialFlag  -> is_eqstring_ref   ()#=)
    //   Dict                       + SpecialFlag  -> is_eqdict_ref     (<<>>#=)
    //   Set                        + SpecialFlag  -> is_eqset_ref      ({{}}#=)
    [[nodiscard]] bool is_eqproc_ref() const {
        return (((m_aat & SpecialFlag) != 0) && (is_array() || is_packed()) && is_executable());
    }

    [[nodiscard]] bool is_eqarray_ref() const { return (((m_aat & SpecialFlag) != 0) && is_array() && is_literal()); }

    [[nodiscard]] bool is_eqstring_ref() const { return (((m_aat & SpecialFlag) != 0) && is_string()); }

    [[nodiscard]] bool is_eqdict_ref() const { return (((m_aat & SpecialFlag) != 0) && is_dict()); }

    [[nodiscard]] bool is_eqset_ref() const { return (((m_aat & SpecialFlag) != 0) && is_set()); }

    // Predicate for ExtValue-ownership and save-policy branches: "does this Array refer
    // to the shared eq-array buffer?"  Two paths match:
    //   - user-facing eqarray ref: SpecialFlag set, value slot reads as m_generation.
    //   - internal root Object (RootObject::EqArray): SpecialFlag clear, m_array is the
    //     real VM offset of the shared buffer.  Internal paths (eqarray_op calling
    //     array_clear on the root to zero it before handing out the public ref) must
    //     also be treated as "the eq buffer" for ownership accounting, or array_clear
    //     will try to save the pre-clear contents and trip invalid-restore at BASE level.
    [[nodiscard]] bool is_eqarray(const Trix *trx) const {
        assert(is_array());

        if (is_eqarray_ref()) {
            return true;
        } else {
            return (m_array == trx->root_object(RootObject::EqArray).m_array);
        }
    }

    // Any-kind eqref predicate for reject-at-store sites that must block the full family.
    [[nodiscard]] bool is_eqref() const {
        return (is_eqproc_ref() || is_eqarray_ref() || is_eqstring_ref() || is_eqdict_ref() || is_eqset_ref());
    }

    // Eqref factories.  Each takes the generation counter (not a VM offset) and stamps
    // SpecialFlag so accessors route through the shared eq-storage buffer and validate
    // the stored generation against the current per-kind counter.  Callers bump and
    // limit-check the counter before calling; the factory only stamps the flag.
    //
    // The value slot is written via the m_generation union alias (not m_array/m_string/
    // etc.) so the construction reads as "stamp a creation-time counter", not "store an
    // offset that secretly isn't an offset".
    [[nodiscard]] static Object make_eqstring(uint32_t generation, length_t length, attrib_t attrib, access_t access) {
        Object obj;
        obj.m_aat = (attrib | access | SpecialFlag | +Type::String);
        obj.m_object_save_level = Save::BASE;
        obj.m_string_length = length;
        obj.m_generation = generation;
        return obj;
    }

    [[nodiscard]] static Object make_eqarray(uint32_t generation, length_t length, attrib_t attrib, access_t access) {
        Object obj;
        obj.m_aat = (attrib | access | SpecialFlag | +Type::Array);
        obj.m_object_save_level = Save::BASE;
        obj.m_arrays_length = length;
        obj.m_generation = generation;
        return obj;
    }

    [[nodiscard]] static Object make_eqdict(uint32_t generation) {
        Object obj;
        obj.m_aat = (LiteralAttrib | SpecialFlag | +Type::Dict);
        obj.m_object_save_level = Save::BASE;
        obj.m_length = 0;
        obj.m_generation = generation;
        return obj;
    }

    [[nodiscard]] static Object make_eqset(uint32_t generation) {
        Object obj;
        obj.m_aat = (LiteralAttrib | SpecialFlag | +Type::Set);
        obj.m_object_save_level = Save::BASE;
        obj.m_length = 0;
        obj.m_generation = generation;
        return obj;
    }

    // eqproc has two physical shapes sharing one generation counter: an Array body
    // (literal or executable) and a Packed body.  Two factories, one counter.
    [[nodiscard]] static Object make_eqproc_array(uint32_t generation, length_t length, access_t access) {
        Object obj;
        obj.m_aat = (ExecutableAttrib | access | SpecialFlag | +Type::Array);
        obj.m_object_save_level = Save::BASE;
        obj.m_arrays_length = length;
        obj.m_generation = generation;
        return obj;
    }

    [[nodiscard]] static Object make_eqproc_packed(uint32_t generation, length_t length) {
        Object obj;
        obj.m_aat = (ExecutableAttrib | ReadOnlyAccess | SpecialFlag | +Type::Packed);
        obj.m_object_save_level = Save::BASE;
        obj.m_arrays_length = length;
        obj.m_generation = generation;
        return obj;
    }

    // Copies length elements from above mark_ptr into dst, stamping each with the current save level.
    static void copy_mark_elements(Trix *trx, const Object *mark_ptr, Object *dst, length_t length) {
        auto curr_save_level = trx->m_curr_save_level;
        auto src = (mark_ptr + 1);
        std::transform(src, (src + length), dst, [curr_save_level](Object obj) {
            obj.set_save_level(curr_save_level);
            return obj;
        });
    }

    [[nodiscard]] static Object make_array_from_mark(Trix *trx,
                                                     attrib_t attrib = LiteralAttrib,
                                                     access_t access = ReadWriteAccess,
                                                     Object *mark_floor = nullptr) {
        // Mirror of make_tagged: when m_curr_alloc_global is set (typically
        // via the surrounding `${...}` block), route to the global-VM
        // allocator so per-iteration arrays land where GC can
        // reclaim them.  Without this, runtime `[ ... ]` literals with
        // executable names always leak into local VM at sl=0 -- the
        // mini-scheme CEK loop's `scheme-cons { [ car cdr ] /pair tag }`
        // hit this hard, growing m_vm_ptr ~1KB per 32 trampoline iters
        // and exhausting default 1MB VM around 16k iterations.
        if (trx->m_curr_alloc_global) {
            return make_global_array_from_mark(trx, attrib, access, mark_floor);
        } else {
            auto [mark_ptr, length] = trx->find_opstack_mark(mark_floor);
            if (trx->vm_remaining<Object>() < (static_cast<Trix::vm_size_t>(length) * Trix::vm_sizeof<Object>())) {
                cleanup_opstack_to_mark(trx, mark_ptr);
                trx->error(Error::VMFull, "while constructing an array");
            } else {
                auto [dst, offset] = trx->vm_alloc_n<Object>(length);
                copy_mark_elements(trx, mark_ptr, dst, length);
                trx->m_op_ptr = (mark_ptr - 1);
                return make_array(offset, length, attrib, access);
            }
        }
    }

    // Mirror of make_array_from_mark that allocates the array's backing
    // storage in the global VM region.  Backs the [...]#$ literal suffix.
    // Body Object values copy in unchanged -- a Name interned locally
    // stays local; #$ globalises only the array's storage cells.  Use
    // ${[...]} to globalise body Names too.
    [[nodiscard]] static Object make_global_array_from_mark(Trix *trx,
                                                            attrib_t attrib = LiteralAttrib,
                                                            access_t access = ReadWriteAccess,
                                                            Object *mark_floor = nullptr) {
        auto [mark_ptr, length] = trx->find_opstack_mark(mark_floor);

        // R6 pointer hygiene: the array storage is global; clone every fragile-local
        // SCALAR body element into global and reject a NON-scalar fragile-local (see
        // Dict::put / array-store).  Each element is on the op stack -- reject first
        // (no free), then clone + write the clone back to its slot, so it stays
        // GC-rooted across the gvm_alloc_n below (and the next element's clone-alloc)
        // and copy_mark_elements reads the cloned values.  At BASE both helpers
        // short-circuit, so file-load-time [...]#$ (the common case) pays a cheap
        // loop; runtime-scanned [...]#$ at sl>=1 (e.g. via `token` or `require`
        // inside a save scope) clones the fragile scalar elements.
        for (auto src = (mark_ptr + 1); src <= trx->m_op_ptr; ++src) {
            Save::reject_local_into_global(trx, true, *src, "[]#$");
            *src = Save::clone_fragile_scalar_into_global(trx, true, *src);
        }

        auto [dst, offset] = trx->gvm_alloc_n<Object>(length, Trix::ChunkKind::Array);
        // Stamp user-visible Object count for GC walker (eliminates
        // slack-walking on Array payloads).
        trx->gvm_set_obj_count(offset, length);
        copy_mark_elements(trx, mark_ptr, dst, length);
        trx->m_op_ptr = (mark_ptr - 1);
        return make_array(offset, length, attrib, access);
    }

    [[nodiscard]] static Object make_eqarray_from_mark(Trix *trx,
                                                       attrib_t attrib = LiteralAttrib,
                                                       access_t access = ReadWriteAccess,
                                                       Object *mark_floor = nullptr) {
        auto eqarray = trx->root_object(RootObject::EqArray);
        auto [mark_ptr, length] = trx->find_opstack_mark(mark_floor);
        auto dst = eqarray.array_objects_raw(trx);
        auto eqarray_length = eqarray.arrays_length();

        if (length > eqarray_length) {
            cleanup_opstack_to_mark(trx, mark_ptr);
            trx->error(Error::LimitCheck, "array length {} exceeds =array max length {}", length, eqarray_length);
        } else if (trx->m_eqarray_generation == std::numeric_limits<uint32_t>::max()) {
            // Wraparound would let a stale ref accidentally match a future generation.
            cleanup_opstack_to_mark(trx, mark_ptr);
            trx->error(Error::LimitCheck, "]#= generation counter exhausted (2^32 creations); cannot create more eqarray refs");
        } else {
            // Free old eqarray ExtValues before overwriting (eqarray storage is reused each call).
            for (length_t i = 0; i < eqarray_length; ++i) {
                dst[i].maybe_free_extvalue(trx);
            }

            copy_mark_elements(trx, mark_ptr, dst, length);

            trx->m_op_ptr = (mark_ptr - 1);
            // Bump generation; returned Array Object is an eqarray ref whose m_generation
            // holds the creation-time counter (not a VM offset).  Accessors resolve
            // through root_object(EqArray).
            ++trx->m_eqarray_generation;
            return make_eqarray(trx->m_eqarray_generation, length, attrib, access);
        }
    }

    // Routes through m_curr_alloc_global: when the global flag is set
    // (e.g. inside ${...} or after `true set-global`), the array's
    // backing storage allocates in global VM and survives save/restore;
    // otherwise local.
    [[nodiscard]] static Object
    make_empty_array(Trix *trx, length_t length, attrib_t attrib = LiteralAttrib, access_t access = ReadWriteAccess) {
        auto [ptr, offset] = trx->vm_alloc_dispatch_n<Object>(length, Trix::ChunkKind::Array);
        auto arr_obj = make_array(offset, length, attrib, access);
        auto null_obj = make_null(trx->m_curr_save_level);
        std::fill_n(ptr, length, null_obj);
        return arr_obj;
    }

    [[nodiscard]] std::pair<Object *, length_t> array_value(Trix *trx) const {
        assert(is_array());

        return std::pair{array_objects(trx), m_arrays_length};
    }

    [[nodiscard]] Object *array_objects(Trix *trx) const {
        assert(is_array());

        if (is_eqproc_ref()) {
            if (m_generation != trx->m_eqproc_generation) {
                trx->error(Error::Unsupported, "stale }}#= reference: storage reused by a subsequent }}#=");
            } else {
                return trx->m_eqproc_storage_ptr;
            }
        }
        if (is_eqarray_ref()) {
            if (m_generation != trx->m_eqarray_generation) {
                trx->error(Error::Unsupported, "stale ]#= reference: storage reused by a subsequent ]#=");
            } else {
                return trx->root_object(RootObject::EqArray).array_objects_raw(trx);
            }
        } else {
            return trx->offset_to_ptr<Object>(m_array);
        }
    }

    // Raw accessor that bypasses eqref validation, used by the eqarray resolver and the
    // root-object path.  Only call when the Object is known to be an ordinary Array or
    // the canonical eqarray root Object (which holds the real VM offset internally).
    [[nodiscard]] Object *array_objects_raw(Trix *trx) const {
        assert(is_array());

        return trx->offset_to_ptr<Object>(m_array);
    }

    // Eqref iteration/mutation strategy: mutating ops advance a cursor or shrink length,
    // but the eqref's value slot holds m_generation (not a VM offset), so we cannot
    // mutate it in-place.  Instead, materialize the eqref body into a fresh VM-heap
    // allocation the first time a mutation is about to happen -- the Object is converted
    // into an ordinary VM-heap array/packed/string in place, clearing SpecialFlag and
    // reusing the value slot's bytes as a real offset written via m_array / m_packed /
    // m_string.  Subsequent mutations proceed along the standard path.  The eq-storage
    // scratch buffer remains available for the next )#= / }#= to write into.
    // Hot-path wrapper: F=0 can never be an eqref (every is_eq*_ref predicate
    // requires SpecialFlag), so the overwhelmingly common non-eqref case is one
    // masked test that inlines at call sites -- packed_pop_head pays this per
    // element popped.  Measured -4.5% on bench_interpreter (2026-06-06).
    void materialize_eqref_if_needed(Trix *trx) {
        if ((m_aat & SpecialFlag) != 0) {
            materialize_eqref_slow(trx);
        }
    }

    void materialize_eqref_slow(Trix *trx) {
        if (is_eqproc_ref()) {
            if (is_array()) {
                if (m_generation != trx->m_eqproc_generation) {
                    trx->error(Error::Unsupported, "stale }}#= reference: storage reused by a subsequent }}#=");
                } else {
                    auto size = static_cast<vm_size_t>(m_arrays_length * sizeof(Object));
                    if (trx->vm_remaining<Object>() < size) {
                        trx->error(Error::VMFull, "while materializing }}#= for iteration");
                    } else {
                        auto [dst, offset] = trx->vm_alloc<Object>(size);
                        std::copy_n(trx->m_eqproc_storage_ptr, m_arrays_length, dst);
                        m_array = offset;
                        m_aat &= ~SpecialFlag;
                    }
                }
            } else {
                assert(is_packed());
                if (m_generation != trx->m_eqproc_generation) {
                    trx->error(Error::Unsupported, "stale }}#= reference: storage reused by a subsequent }}#=");
                } else {
                    // Walk the packed bytes to compute total size.
                    auto src = reinterpret_cast<const packed_data_t *>(trx->m_eqproc_storage_ptr);
                    auto scan = src;
                    scan = skip_packed(scan, m_arrays_length);
                    auto size = static_cast<vm_size_t>(scan - src);
                    if (trx->vm_remaining<packed_data_t>() < size) {
                        trx->error(Error::VMFull, "while materializing }}#= for iteration");
                    } else {
                        auto [dst, offset] = trx->vm_alloc<packed_data_t>(size);
                        std::copy_n(src, size, dst);
                        m_packed = offset;
                        m_aat &= ~SpecialFlag;
                    }
                }
            }
        } else if (is_eqarray_ref()) {
            if (m_generation != trx->m_eqarray_generation) {
                trx->error(Error::Unsupported, "stale ]#= reference: storage reused by a subsequent ]#=");
            } else {
                auto size = static_cast<vm_size_t>(m_arrays_length * sizeof(Object));
                if (trx->vm_remaining<Object>() < size) {
                    trx->error(Error::VMFull, "while materializing ]#= for mutation");
                } else {
                    auto src = trx->root_object(RootObject::EqArray).array_objects_raw(trx);
                    auto [dst, offset] = trx->vm_alloc<Object>(size);
                    std::copy_n(src, m_arrays_length, dst);
                    m_array = offset;
                    m_aat &= ~SpecialFlag;
                }
            }
        } else if (is_eqstring_ref()) {
            if (m_generation != trx->m_eqstring_generation) {
                trx->error(Error::Unsupported, "stale )#= reference: storage reused by a subsequent )#=");
            } else {
                // Allocate m_string_length + 1 (for the nul terminator).
                auto alloc_size = static_cast<vm_size_t>(m_string_length + 1);
                if (trx->vm_remaining<vm_t>() < alloc_size) {
                    trx->error(Error::VMFull, "while materializing )#= for mutation");
                } else {
                    auto src = trx->root_object(RootObject::EqString).string_vptr_raw(trx);
                    auto [dst, offset] = trx->vm_alloc<vm_t>(alloc_size);
                    std::copy_n(src, m_string_length, dst);
                    dst[m_string_length] = '\0';
                    m_string = offset;
                    m_aat &= ~SpecialFlag;
                }
            }
        }
    }

    [[nodiscard]] Object array_pop_head(Trix *trx) {
        assert(is_array());
        assert(m_arrays_length > 0);

        materialize_eqref_if_needed(trx);
        auto head = trx->offset_to_ptr<Object>(m_array);
        auto object = *head;
        --m_arrays_length;
        m_array = static_cast<vm_offset_t>(m_array + sizeof(Object));
        return object;
    }

    // Pop head and clone -- returns an owned copy with its own ExtValue.
    // Use this when the popped element will be placed on the operand stack
    // where arithmetic operators may free its ExtValue.  Without cloning,
    // the source array element and the operand stack entry share the same
    // ExtValue, creating a use-after-free when the operator consumes it.
    [[nodiscard]] Object array_pop_clone_head(Trix *trx) {
        assert(is_array());
        assert(m_arrays_length > 0);

        materialize_eqref_if_needed(trx);
        auto head = trx->offset_to_ptr<Object>(m_array);
        auto object = head->make_clone(trx);
        --m_arrays_length;
        m_array = static_cast<vm_offset_t>(m_array + sizeof(Object));
        return object;
    }

    [[nodiscard]] vm_offset_t arrays_offset() const {
        assert(is_array());

        return m_array;
    }

    [[nodiscard]] length_t arrays_length() const {
        assert(is_sequence());

        return m_arrays_length;
    }

    void set_array_length(length_t length) {
        assert(is_array());
        assert(length <= m_arrays_length);

        m_arrays_length = length;
    }

    void array_interval(Trix *trx, length_t index, length_t count) {
        assert(is_array());
        assert((count + index) <= m_arrays_length);

        materialize_eqref_if_needed(trx);
        m_arrays_length = count;
        if (index != 0) {
            m_array = static_cast<vm_offset_t>(m_array + (index * sizeof(Object)));
        }
    }

    void array_clear(Trix *trx) {
        assert(is_array());

        // Use is_eqarray (not is_eqarray_ref) so the internal root Object is also treated
        // as "the eq buffer": eqarray_op calls this on the root to zero it before handing
        // out the public ref, and the root is never journaled -- if we took the save_object
        // path we would trip invalid-restore at BASE level when called from a coroutine or
        // any other context that runs outside a save scope.
        auto is_eq = is_eqarray(trx);
        auto curr_save_level = trx->m_curr_save_level;
        auto null_obj = make_null(curr_save_level);
        auto objects = array_objects(trx);
        for (auto count = m_arrays_length; count != 0; --count) {
            if ((objects->save_level() == curr_save_level) || is_eq) {
                objects->maybe_free_extvalue(trx);
            } else {
                Save::save_object(trx, objects);
            }
            *objects++ = null_obj;
        }
    }

    [[nodiscard]] static constexpr Object make_packed(vm_offset_t offset, length_t length, attrib_t attrib = LiteralAttrib) {
        Object object;
        object.m_aat = (attrib | ReadOnlyAccess | +Type::Packed);
        object.m_object_save_level = Save::BASE;
        object.m_arrays_length = length;
        object.m_packed = offset;
        return object;
    }

    [[nodiscard]] bool is_packed() const { return (type() == Type::Packed); }

    // Resolves the raw byte pointer for a Packed.  For eqproc-ref packeds, the value slot
    // holds m_generation (the creation-time counter); validate it against the current
    // counter and dispatch to the shared eqproc storage.  For normal packeds, m_packed is
    // a VM offset.
    [[nodiscard]] packed_data_t *packed_data_ptr(Trix *trx) const {
        assert(is_packed());

        if (is_eqproc_ref()) {
            if (m_generation != trx->m_eqproc_generation) {
                trx->error(Error::Unsupported, "stale }}#= reference: storage reused by a subsequent }}#=");
            } else {
                return reinterpret_cast<packed_data_t *>(trx->m_eqproc_storage_ptr);
            }
        }
        return trx->offset_to_ptr<packed_data_t>(m_packed);
    }

    [[nodiscard]] std::pair<packed_data_t *, length_t> packed_value(Trix *trx) const {
        return std::pair{packed_data_ptr(trx), m_arrays_length};
    }

    [[nodiscard]] packed_data_span packed_span(Trix *trx) const { return packed_data_span{packed_data_ptr(trx), m_arrays_length}; }

    [[nodiscard]] cpacked_data_span const_packed_span(Trix *trx) const {
        return cpacked_data_span{packed_data_ptr(trx), m_arrays_length};
    }

    [[nodiscard]] Object packed_pop_head(Trix *trx) {
        assert(is_packed());
        assert(m_arrays_length > 0);

        materialize_eqref_if_needed(trx);
        auto packed_data = trx->offset_to_ptr<const packed_data_t>(m_packed);
        auto [next, object] = extract_next_packed(trx, packed_data);
        --m_arrays_length;
        m_packed = trx->ptr_to_offset(next);
        return object;
    }

    // Pop head and clone -- returns an owned copy with its own ExtValue.
    // Symmetric with array_pop_clone_head; use when the popped element
    // will be placed on the operand stack.
    [[nodiscard]] Object packed_pop_clone_head(Trix *trx) { return packed_pop_head(trx).make_clone(trx); }

    // Pop head and clone from either array or packed.
    // Dispatches to array_pop_clone_head or packed_pop_clone_head based on type.
    [[nodiscard]] Object arrays_pop_clone_head(Trix *trx) {
        return (is_array() ? array_pop_clone_head(trx) : packed_pop_clone_head(trx));
    }

    void packed_interval(Trix *trx, length_t index, length_t count) {
        assert(is_packed());
        assert((count + index) <= m_arrays_length);

        m_arrays_length = count;
        if (index != 0) {
            auto packed_data = trx->offset_to_ptr<const packed_data_t>(m_packed);
            packed_data = skip_packed(packed_data, index);
            m_packed = trx->ptr_to_offset(packed_data);
        }
    }

    [[nodiscard]] static constexpr Object
    make_string(vm_offset_t offset, length_t length, attrib_t attrib = LiteralAttrib, access_t access = ReadWriteAccess) {
        Object object;
        object.m_aat = (attrib | access | +Type::String);
        object.m_object_save_level = Save::BASE;
        object.m_string_length = length;
        object.m_string = offset;
        return object;
    }

    [[nodiscard]] bool is_string() const { return (type() == Type::String); }

    // Routes through m_curr_alloc_global: when the global flag is set,
    // the string's backing bytes allocate in global VM and survive
    // save/restore; otherwise local.
    [[nodiscard]] static Object
    make_empty_string(Trix *trx, length_t length, attrib_t attrib = LiteralAttrib, access_t access = ReadWriteAccess) {
        // length+1 (the nul slot) can reach 65536 at MaxStringLength, one past
        // length_t -- so route the byte buffer through the single byte-size
        // dispatch variant (vm_size_t = uint32_t), NOT the element-count _n
        // variant whose length_t assert/obj_count stamp are meant for Object[]
        // payloads.  String is a GC leaf (gc.inl walk_block_contents); its
        // m_obj_count is never read, so stamping it is pointless.  Matches
        // every other String alloc site (make_string_region, concat, etc.).
        auto [ptr, offset] = trx->vm_alloc_dispatch<vm_t>(length + 1, Trix::ChunkKind::String);
        std::fill_n(ptr, length, vm_t{0});
        ptr[length] = '\0';  // nul-terminate
        return make_string(offset, length, attrib, access);
    }

    [[nodiscard]] static Object make_string(Trix *trx, const char *ptr, length_t length) {
        auto [base, offset] = trx->vm_alloc<char>(length + 1);
        std::copy_n(ptr, length, base);
        base[length] = '\0';  // nul-terminate
        return make_string(offset, length, LiteralAttrib, ReadOnlyAccess);
    }

    [[nodiscard]] static Object make_string(Trix *trx, const vm_t *ptr, length_t length) {
        auto [base, offset] = trx->vm_alloc<vm_t>(length + 1);
        std::copy_n(ptr, length, base);
        base[length] = '\0';  // nul-terminate
        return make_string(offset, length, LiteralAttrib, ReadOnlyAccess);
    }

    // Region-aware copying string constructors: like make_string(trx, ptr, len)
    // but the destination honors m_curr_alloc_global (global inside a `${...}`
    // block, local otherwise).  Used by array-producing ops (split, regex-find-all,
    // regex-split) so that under ${...} the element strings land in the global
    // region and survive save/restore, instead of dangling as local copies/views
    // inside a global result array.  Callers that fill a result array with these
    // must root that array across the fill (each call may fire vm_global_gc).
    [[nodiscard]] static Object make_string_region(Trix *trx, const char *ptr, length_t length) {
        auto [base, offset] = trx->vm_alloc_dispatch<char>(length + 1, Trix::ChunkKind::String);
        std::copy_n(ptr, length, base);
        base[length] = '\0';  // nul-terminate
        return make_string(offset, length, LiteralAttrib, ReadOnlyAccess);
    }

    [[nodiscard]] static Object make_string_region(Trix *trx, const vm_t *ptr, length_t length) {
        auto [base, offset] = trx->vm_alloc_dispatch<vm_t>(length + 1, Trix::ChunkKind::String);
        std::copy_n(ptr, length, base);
        base[length] = '\0';  // nul-terminate
        return make_string(offset, length, LiteralAttrib, ReadOnlyAccess);
    }

    [[nodiscard]] static Object make_string(Trix *trx, std::string_view sv) {
        auto length = sv_length(sv);
        auto [base, offset] = trx->vm_alloc<char>(length + 1);
        std::copy_n(sv.data(), length, base);
        base[length] = '\0';  // nul-terminate
        return make_string(offset, length, LiteralAttrib, ReadOnlyAccess);
    }

    // Region-aware sibling of make_string(sv): when m_curr_alloc_global is set
    // the backing bytes allocate in global VM and survive save/restore (so a
    // global container holding the result does not dangle); otherwise local.
    [[nodiscard]] static Object make_string_dispatch(Trix *trx, std::string_view sv) {
        auto length = sv_length(sv);
        // Byte buffer, not an Object[] -- single byte-size dispatch (length+1
        // reaches 65536 at MaxStringLength, past length_t; String's obj_count
        // is a GC-leaf no-op).  See make_empty_string for the rationale.
        auto [ptr, offset] = trx->vm_alloc_dispatch<vm_t>(length + 1, Trix::ChunkKind::String);
        std::copy_n(sv.data(), length, reinterpret_cast<char *>(ptr));
        ptr[length] = '\0';  // nul-terminate
        return make_string(offset, length, LiteralAttrib, ReadOnlyAccess);
    }

    template<typename... T>
    [[nodiscard]] static Object make_error_string(Trix *trx, std::format_string<T...> format, T &&...args) {
        auto [out, _] = std::format_to_n(trx->m_error_string_base, MaxErrorLength, format, args...);
        auto length = static_cast<length_t>(out - trx->m_error_string_base);
        trx->m_error_string_base[length] = '\0';
        auto offset = trx->ptr_to_offset(trx->m_error_string_base);
        return make_string(offset, length, LiteralAttrib, ReadOnlyAccess);
    }

    [[nodiscard]] static Object make_error_string(Trix *trx, std::string_view sv) {
        auto length = std::min(sv_length(sv), MaxErrorLength);
        std::copy_n(sv.data(), length, trx->m_error_string_base);
        trx->m_error_string_base[length] = '\0';
        return make_string(trx->ptr_to_offset(trx->m_error_string_base), length, LiteralAttrib, ReadOnlyAccess);
    }

    [[nodiscard]] static Object append_error_string(Trix *trx, Object errstr, std::string_view sv) {
        assert(errstr.is_string());

        auto ptr = trx->offset_to_ptr<char>(errstr.m_string);
        auto length = errstr.m_string_length;
        auto available = std::min(sv_length(sv), static_cast<length_t>(MaxErrorLength - length));
        std::copy_n(sv.data(), available, ptr + length);
        auto new_length = static_cast<length_t>(length + available);
        ptr[new_length] = '\0';
        errstr.m_string_length = new_length;
        return errstr;
    }

    // Resolves the raw byte pointer for a String.  For eqstring refs, the value slot holds
    // m_generation (the creation-time counter); validate it against the current counter
    // and dispatch to the shared eqstring root-object's buffer.  For normal strings,
    // m_string is a VM offset.  The resolved pointer points to the first string byte (the
    // generation lives only in the Object's value slot, not in the buffer).
    [[nodiscard]] vm_t *string_data_ptr(Trix *trx) const {
        assert(is_string());

        if (is_eqstring_ref()) {
            if (m_generation != trx->m_eqstring_generation) {
                trx->error(Error::Unsupported, "stale )#= reference: storage reused by a subsequent )#=");
            } else {
                return trx->root_object(RootObject::EqString).string_vptr_raw(trx);
            }
        }
        return trx->offset_to_ptr<vm_t>(m_string);
    }

    // Raw accessor used only by eqstring resolvers and the root_object path itself.
    // Bypasses the eqref validation so the root_object's own buffer can be reached
    // without recursion.
    [[nodiscard]] vm_t *string_vptr_raw(Trix *trx) const {
        assert(is_string());

        return trx->offset_to_ptr<vm_t>(m_string);
    }

    [[nodiscard]] std::string_view sv_value(Trix *trx) const {
        return std::string_view{reinterpret_cast<const char *>(string_data_ptr(trx)), m_string_length};
    }

    [[nodiscard]] vm_t *string_vptr(Trix *trx) const { return string_data_ptr(trx); }

    // Returns a nul-terminated const char* for C API use.
    // If the string is already nul-terminated (freshly allocated), returns directly.
    // Otherwise, copies into a new VM allocation with a trailing \0.
    [[nodiscard]] const char *string_cstr(Trix *trx) const {
        assert(is_string());

        auto ptr = reinterpret_cast<const char *>(string_data_ptr(trx));
        if (ptr[m_string_length] == '\0') {
            return ptr;
        } else {
            // sub-string or shrunk string: copy into VM with nul-terminator
            auto [buf, offset] = trx->vm_alloc<char>(m_string_length + 1);
            std::copy_n(ptr, m_string_length, buf);
            buf[m_string_length] = '\0';
            return buf;
        }
    }

    void set_string_length(Trix *trx, length_t length) {
        assert(is_string());
        assert(length <= m_string_length);

        // Shrinks length and writes a terminating nul in the body.  For eqstring refs
        // materialize first so the nul write targets private heap storage and m_string
        // can safely be interpreted as a VM offset.
        materialize_eqref_if_needed(trx);
        m_string_length = length;
        trx->offset_to_ptr<vm_t>(m_string)[length] = '\0';
    }

    [[nodiscard]] length_t string_length() const {
        assert(is_string());

        return m_string_length;
    }

    [[nodiscard]] Object string_pop_head(Trix *trx) {
        assert(is_string());
        assert(m_string_length > 0);

        materialize_eqref_if_needed(trx);
        auto head = trx->offset_to_ptr<vm_t>(m_string);
        auto object = make_byte(*head);
        ++m_string;
        --m_string_length;
        return object;
    }

    void string_pop_count(Trix *trx, length_t count) {
        assert(is_string());
        assert(count <= m_string_length);

        materialize_eqref_if_needed(trx);
        m_string += count;
        m_string_length = static_cast<length_t>(m_string_length - count);
    }

    void string_interval(Trix *trx, length_t index, length_t count) {
        assert(is_string());
        assert((count + index) <= m_string_length);

        materialize_eqref_if_needed(trx);
        m_string_length = count;
        if (index != 0) {
            m_string = static_cast<vm_offset_t>(m_string + (index * sizeof(vm_t)));
        }
    }

    void string_clear(Trix *trx) {
        assert(is_string());

        std::memset(string_data_ptr(trx), 0, m_string_length);
    }

    //           X   Y
    // -1: ("this" < "other")
    //  0: ("this" == "other")
    // +1: ("this" > "other")
    [[nodiscard]] int string_compare(Trix *trx, const Object *other) const {
        assert(is_string());
        assert(other->is_string());

        auto x = sv_value(trx);
        auto x_begin = reinterpret_cast<const vm_t *>(x.data());
        auto y = other->sv_value(trx);
        auto y_begin = reinterpret_cast<const vm_t *>(y.data());
        auto cmp = std::lexicographical_compare_three_way(x_begin, (x_begin + sv_length(x)), y_begin, (y_begin + sv_length(y)));
        return (std::is_lt(cmp) ? -1 : (std::is_gt(cmp) ? +1 : 0));
    }

    [[nodiscard]] static constexpr Object
    make_stream(vm_offset_t offset, stream_id_t sid, attrib_t attrib = LiteralAttrib, access_t access = ReadWriteAccess) {
        Object object;
        object.m_aat = (attrib | access | +Type::Stream);
        object.m_object_save_level = Save::BASE;
        // m_sid is a uint8_t in a 16-bit union, set m_length to 0 to initialize entire union
        object.m_length = 0;
        object.m_sid = sid;
        object.m_stream = offset;
        return object;
    }

    [[nodiscard]] static Object make_stream(Trix *trx, Stream *stream) {
        Object object;
        auto offset = trx->ptr_to_offset(stream);
        auto sid = stream->sid();
        auto access = stream->is_writable(sid) ? ReadWriteAccess : ReadOnlyAccess;
        object.m_aat = (LiteralAttrib | access | +Type::Stream);
        object.m_object_save_level = Save::BASE;
        // m_sid is a uint8_t in a 16-bit union, set m_length to 0 to initialize entire union
        object.m_length = 0;
        object.m_sid = sid;
        object.m_stream = offset;
        return object;
    }

    [[nodiscard]] static constexpr Object make_invalid_stream() {
        Object object;
        object.m_aat = (LiteralAttrib | ReadOnlyAccess | +Type::Stream);
        object.m_object_save_level = Save::BASE;
        // m_sid is a uint8_t in a 16-bit union, set m_length to 0 to initialize entire union
        object.m_length = 0;
        object.m_sid = Stream::INVALID_SID;
        object.m_stream = nulloffset;
        return object;
    }

    [[nodiscard]] bool is_stream() const { return (type() == Type::Stream); }

    [[nodiscard]] std::pair<Stream *, stream_id_t> stream_value(Trix *trx) const {
        assert(is_stream());

        return std::pair{trx->offset_to_ptr<Stream>(m_stream), m_sid};
    }

    [[nodiscard]] stream_id_t stream_sid() const {
        assert(is_stream());

        return m_sid;
    }

    [[nodiscard]] static constexpr Object make_dict(vm_offset_t offset) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Dict);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_dict = offset;
        return object;
    }

    [[nodiscard]] bool is_dict() const { return (type() == Type::Dict); }

    // Set: unordered collection of unique names (Dict without values).
    // Uses m_dict field for vm_offset_t -- same Object layout as Dict.
    [[nodiscard]] static constexpr Object make_set(vm_offset_t offset) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Set);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_set = offset;
        return object;
    }

    [[nodiscard]] bool is_set() const { return (type() == Type::Set); }

    // set_value(): Set shares Dict's VM structure (with header bit).
    // Returns Dict* -- caller uses set-specific methods (set_put, set_remove, etc.)
    [[nodiscard]] Dict *set_value(Trix *trx) const {
        assert(is_set());

        if (is_eqset_ref()) {
            if (m_generation != trx->m_eqset_generation) {
                trx->error(Error::Unsupported, "stale {{}}#= reference: storage reused by a subsequent {{}}#=");
            } else {
                return trx->m_eqset;
            }
        }
        return trx->offset_to_ptr<Dict>(m_set);
    }

    // Build a SourceLoc from stream id, line, and column.
    // SourceLoc is Literal + IgnoresExecute: never user-visible, internal to the @call frame.
    [[nodiscard]] static Object make_sourceloc(const SourceLocation *source_loc) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::SourceLoc);
        object.m_source_sid = source_loc->m_sid;
        object.m_source_pos = source_loc->m_col;
        object.m_source_line = source_loc->m_line;
        return object;
    }

    [[nodiscard]] bool is_sourceloc() const { return (type() == Type::SourceLoc); }

    // Curry: partial application -- 2 Objects [value, callable] stored in VM.
    // Routes through m_curr_alloc_global: when set, the [value, callable] pair
    // lands in global VM (survives save/restore) and is tagged ChunkKind::Curry.
    [[nodiscard]] static Object make_curry(Trix *trx, Object value, Object callable) {
        auto [storage, offset] = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Curry);
        auto curr_save_level = trx->m_curr_save_level;

        value.set_save_level(curr_save_level);
        storage[0] = value;

        callable.set_save_level(curr_save_level);
        storage[1] = callable;

        Object object;
        object.m_aat = (ExecutableAttrib | +Type::Curry);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_curry = offset;
        return object;
    }

    // Build a curry pair [value, callable] in VM.  Value is stored as literal.
    [[nodiscard]] static Object make_curry_pair(Trix *trx, Object value, Object callable) {
        value.set_literal();
        return Object::make_curry(trx, value, callable);
    }

    // Build a compose pair [f_executable, g] in VM.  f is executed, then g.
    [[nodiscard]] static Object make_compose_pair(Trix *trx, Object f, Object g) {
        f.set_executable();
        return Object::make_curry(trx, f, g);
    }

    [[nodiscard]] bool is_curry() const { return (type() == Type::Curry); }

    [[nodiscard]] Object *curry_storage(Trix *trx) const {
        assert(is_curry());

        return trx->offset_to_ptr<Object>(m_curry);
    }

    // Thunk: lazy evaluation -- 3 Objects [state, proc, result] stored in VM
    // state Object uses m_integer: 0=Unevaluated, 1=Evaluating, 2=Evaluated
    static constexpr integer_t ThunkUnevaluated{0};
    static constexpr integer_t ThunkEvaluating{1};
    static constexpr integer_t ThunkEvaluated{2};

    static constexpr int ThunkStorageState{0};
    static constexpr int ThunkStorageProc{1};
    static constexpr int ThunkStorageResult{2};

    // Thunk: lazy evaluation -- 3 Objects [state, proc, result] stored in VM
    // state Object uses m_integer: 0=Unevaluated, 1=Evaluating, 2=Evaluated.
    // Routes through m_curr_alloc_global: when set, the 3-Object tuple lands
    // in global VM (survives save/restore) and is tagged ChunkKind::Thunk.
    [[nodiscard]] static Object make_thunk(Trix *trx, Object state, Object proc, Object result) {
        auto [storage, offset] = trx->vm_alloc_dispatch_n<Object>(3, Trix::ChunkKind::Thunk);
        auto curr_save_level = trx->m_curr_save_level;

        state.set_save_level(curr_save_level);
        storage[ThunkStorageState] = state;

        proc.set_save_level(curr_save_level);
        storage[ThunkStorageProc] = proc;

        result.set_save_level(curr_save_level);
        storage[ThunkStorageResult] = result;

        Object object;
        object.m_aat = (LiteralAttrib | +Type::Thunk);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_thunk = offset;
        return object;
    }

    // Build a thunk from a proc Object (callable).
    [[nodiscard]] static Object make_lazy_thunk(Trix *trx, Object proc) {
        auto state = Object::make_integer(Object::ThunkUnevaluated);
        auto result = Object::make_null(trx->m_curr_save_level);
        return Object::make_thunk(trx, state, proc, result);
    }

    // Build a pre-evaluated thunk wrapping a result value.
    // The thunk is marked Evaluated; forcing it returns result immediately.
    [[nodiscard]] static Object make_pre_evaluated_thunk(Trix *trx, Object result) {
        auto state = Object::make_integer(Object::ThunkEvaluated);
        auto proc = Object::make_null(trx->m_curr_save_level);
        return Object::make_thunk(trx, state, proc, result);
    }

    // Build a pre-evaluated thunk wrapping a null value.
    [[nodiscard]] static Object make_pre_evaluated_thunk(Trix *trx) {
        auto state = Object::make_integer(Object::ThunkEvaluated);
        auto proc = Object::make_null(trx->m_curr_save_level);
        auto result = Object::make_null(trx->m_curr_save_level);
        return Object::make_thunk(trx, state, proc, result);
    }

    [[nodiscard]] bool is_thunk() const { return (type() == Type::Thunk); }

    [[nodiscard]] Object *thunk_storage(Trix *trx) const {
        assert(is_thunk());

        return trx->offset_to_ptr<Object>(m_thunk);
    }

    static constexpr int LazyHeadIndex{0};
    static constexpr int LazyTailThunkIndex{1};
    static constexpr length_t LazyArrayLength{2};

    // Build a 2-element array [head, tail_thunk] in VM.  Routes through
    // m_curr_alloc_global (like make_curry / make_thunk / make_tagged) so a
    // lazy-seq built inside ${...} allocates its node in the same region as its
    // tail thunk (make_lazy_thunk -> make_thunk is also region-aware) and
    // survives save/restore instead of dangling when local VM rewinds.
    [[nodiscard]] static Object make_lazy(Trix *trx, Object head, Object tail_thunk) {
        auto [storage, offset] = trx->vm_alloc_dispatch_n<Object>(LazyArrayLength, Trix::ChunkKind::Array);
        auto curr_save_level = trx->m_curr_save_level;

        head.set_save_level(curr_save_level);
        storage[LazyHeadIndex] = head;

        tail_thunk.set_save_level(curr_save_level);
        storage[LazyTailThunkIndex] = tail_thunk;
        return Object::make_array(offset, LazyArrayLength);
    }

    // A lazy-seq is either null (empty) or a 2-element array [head, tail-thunk]
    // where tail-thunk is a Thunk that, when forced, yields the next lazy-seq or null.
    [[nodiscard]] bool is_lazy_seq(Trix *trx) const {
        return (is_null() ||
                (is_array() && (arrays_length() == LazyArrayLength) && array_objects(trx)[LazyTailThunkIndex].is_thunk()));
    }

    [[nodiscard]] bool is_lazy_seq_nonempty(Trix *trx) const {
        return (is_array() && (arrays_length() == LazyArrayLength) && array_objects(trx)[LazyTailThunkIndex].is_thunk());
    }

    static constexpr int TaggedNameIndex{0};
    static constexpr int TaggedValueIndex{1};

    // Tagged: always literal, discriminated value -- 2 Objects [tag-name, payload] stored in VM.
    // Routes through m_curr_alloc_global: when set, the [name, payload] pair lands
    // in global VM (survives save/restore) and is tagged ChunkKind::Tagged.
    [[nodiscard]] static Object make_tagged(Trix *trx, Object name, Object payload) {
        assert(name.is_name());

        auto [storage, offset] = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Tagged);
        auto curr_save_level = trx->m_curr_save_level;

        name.set_literal();
        name.set_save_level(curr_save_level);
        storage[Object::TaggedNameIndex] = name;

        payload.set_save_level(curr_save_level);
        storage[Object::TaggedValueIndex] = payload;

        Object object;
        object.m_aat = (LiteralAttrib | +Type::Tagged);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_tagged = offset;
        return object;
    }

    [[nodiscard]] bool is_tagged() const { return (type() == Type::Tagged); }

    [[nodiscard]] vm_offset_t tagged_offset() const {
        assert(is_tagged());

        return m_tagged;
    }

    [[nodiscard]] Object *tagged_storage(Trix *trx) const {
        assert(is_tagged());

        return trx->offset_to_ptr<Object>(m_tagged);
    }

    // Record: immutable named-field composite -- [schema_offset, Object[0..N-1]] stored in VM
    // Schema is separately allocated: [field_count, Name[0..N-1]]
    [[nodiscard]] static constexpr Object make_record(vm_offset_t offset, length_t field_count) {
        Object object;
        object.m_aat = (LiteralAttrib | ReadOnlyAccess | +Type::Record);
        object.m_object_save_level = Save::BASE;
        object.m_length = field_count;
        object.m_record = offset;
        return object;
    }

    [[nodiscard]] bool is_record() const { return (type() == Type::Record); }

    [[nodiscard]] RecordInstance *record_instance(Trix *trx) const {
        assert(is_record());

        return trx->offset_to_ptr<RecordInstance>(m_record);
    }

    // OpaqueHandle: opaque pool of sub-typed VM handles (Screen now; Tilemap,
    // Sample later).  HandleKind discriminates; the per-kind state struct
    // lives in the VM heap at m_handle.
    [[nodiscard]] static constexpr Object make_handle(vm_offset_t offset, HandleKind kind) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::OpaqueHandle);
        object.m_object_save_level = Save::BASE;
        object.m_handle_kind = kind;
        object.m_handle = offset;
        return object;
    }

    [[nodiscard]] bool is_handle() const { return (type() == Type::OpaqueHandle); }

    [[nodiscard]] HandleKind handle_kind() const {
        assert(is_handle());

        return m_handle_kind;
    }

    [[nodiscard]] vm_offset_t handle_offset() const {
        assert(is_handle());

        return m_handle;
    }

    [[nodiscard]] bool is_screen() const { return (is_handle() && (handle_kind() == HandleKind::Screen)); }

    // Coroutine: cooperative coroutine handle -- CoroutineContext stored in VM
    [[nodiscard]] static constexpr Object make_coroutine(vm_offset_t offset) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Coroutine);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_coroutine = offset;
        return object;
    }

    [[nodiscard]] bool is_coroutine() const { return (type() == Type::Coroutine); }

    [[nodiscard]] vm_offset_t coroutine_offset() const {
        assert(is_coroutine());

        return m_coroutine;
    }

    [[nodiscard]] CoroutineContext *coroutine_context(Trix *trx) const {
        assert(is_coroutine());

        return trx->offset_to_ptr<CoroutineContext>(m_coroutine);
    }

    // PipeBuffer: bounded buffer for inter-coroutine pipelines -- PipeBufferHeader stored in VM
    [[nodiscard]] static constexpr Object make_pipe_buffer(vm_offset_t offset) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::PipeBuffer);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_pipe_buffer = offset;
        return object;
    }

    [[nodiscard]] bool is_pipe_buffer() const { return (type() == Type::PipeBuffer); }

    [[nodiscard]] vm_offset_t pipe_buffer_offset() const {
        assert(is_pipe_buffer());

        return m_pipe_buffer;
    }

    [[nodiscard]] PipeBufferHeader *pipe_buffer_header(Trix *trx) const {
        assert(is_pipe_buffer());

        return trx->offset_to_ptr<PipeBufferHeader>(m_pipe_buffer);
    }

    // Cell: reactive cell -- CellHeader stored in VM
    [[nodiscard]] static constexpr Object make_cell(vm_offset_t offset) {
        Object object;
        object.m_aat = (LiteralAttrib | +Type::Cell);
        object.m_object_save_level = Save::BASE;
        object.m_length = 0;
        object.m_cell = offset;
        return object;
    }

    [[nodiscard]] bool is_cell() const { return (type() == Type::Cell); }

    [[nodiscard]] vm_offset_t cell_offset() const {
        assert(is_cell());

        return m_cell;
    }

    [[nodiscard]] CellHeader *cell_header(Trix *trx) const {
        assert(is_cell());

        return trx->offset_to_ptr<CellHeader>(m_cell);
    }

    // Continuation: one-shot delimited continuation -- ContinuationContext stored in VM
    [[nodiscard]] static constexpr Object make_continuation(vm_offset_t offset, save_level_t capture_save_level) {
        Object object;
        object.m_aat = (ExecutableAttrib | +Type::Continuation);
        object.m_object_save_level = Save::BASE;
        object.m_continuation_save_level = capture_save_level;
        object.m_continuation = offset;
        return object;
    }

    [[nodiscard]] bool is_continuation() const { return (type() == Type::Continuation); }

    [[nodiscard]] vm_offset_t continuation_offset() const {
        assert(is_continuation());

        return m_continuation;
    }

    [[nodiscard]] save_level_t continuation_save_level() const {
        assert(is_continuation());

        return m_continuation_save_level;
    }

    [[nodiscard]] ContinuationContext *continuation_context(Trix *trx) const {
        assert(is_continuation());

        return trx->offset_to_ptr<ContinuationContext>(m_continuation);
    }

    // Free any ExtValues left on the operand stack above mark_ptr and reset
    // m_op_ptr to just below the mark.  Shared cleanup before raising an error
    // out of a [...] / <<...>> / {{...}} literal builder.
    static void cleanup_opstack_to_mark(Trix *trx, Object *mark_ptr) {
        maybe_free_extvalue_opstack(trx, mark_ptr);
        trx->m_op_ptr = (mark_ptr - 1);
    }

    // Validate that every key in mark_ptr+1 .. top_ptr is usable as a dict key.
    // Rejects null (TypeCheck) and Inf/NaN floating-point values (RangeCheck).
    // Cleans up the operand stack (frees ExtValues, resets m_op_ptr) before erroring.
    static void validate_dict_keys(Trix *trx, Object *mark_ptr, const Object *top_ptr) {
        for (auto key = (mark_ptr + 1); key < top_ptr; key += 2) {
            if (key->is_null()) {
                cleanup_opstack_to_mark(trx, mark_ptr);
                trx->error(Error::TypeCheck, "null is not a valid dict key");
            } else if (key->is_floating_point()) {
                if (key->is_floating_point_inf(trx)) {
                    cleanup_opstack_to_mark(trx, mark_ptr);
                    trx->error(Error::NumericalINF, "Inf is not a valid dict key");
                } else if (key->is_floating_point_nan(trx)) {
                    cleanup_opstack_to_mark(trx, mark_ptr);
                    trx->error(Error::NumericalNaN, "NaN is not a valid dict key");
                }
            }
        }
    }

    [[nodiscard]] static Object
    make_dict_from_mark(Trix *trx, DictMode mode = DictMode::ReadWriteFixed, Object *mark_floor = nullptr) {
        // Mirror of make_tagged: when m_curr_alloc_global is set (typically
        // via the surrounding `${...}` block), route the dict's storage
        // into global VM so per-iteration runtime <<...>> literals land
        // where GC can reclaim them.  See make_array_from_mark
        // for the matching arc.
        if (trx->m_curr_alloc_global) {
            return make_global_dict_from_mark(trx, mode, mark_floor);
        } else {
            auto [mark_ptr, n] = trx->find_opstack_mark(mark_floor);
            if ((n % 2) != 0) {
                cleanup_opstack_to_mark(trx, mark_ptr);
                trx->error(Error::RangeCheck, "mismatched number of key/value pairs when creating a dict");
            } else {
                auto top_ptr = trx->m_op_ptr;
                validate_dict_keys(trx, mark_ptr, top_ptr);

                auto count = static_cast<length_t>(n / 2);
                // Minimum capacity 4 for empty dict literals so << >> produces a usable dict
                // with a proper hash bucket count rather than a zero-capacity fixed dict.
                auto capacity = std::max(count, static_cast<length_t>(4));
                auto [dict, offset] = Dict::create_dict(trx, capacity, mode);
                // Note: same mixed-ownership caveat as make_eqdict_from_mark -- see comment there.
                for (auto key = (mark_ptr + 1); key < top_ptr; key += 2) {
                    auto value = (key + 1);
                    dict->put(trx, *key, *value);
                }

                trx->m_op_ptr = (mark_ptr - 1);
                return make_dict(offset);
            }
        }
    }

    // Mirror of make_dict_from_mark that allocates the Dict struct in
    // the global VM region.  Backs the <<...>>#$ literal suffix.  Body
    // values copy in unchanged.
    [[nodiscard]] static Object
    make_global_dict_from_mark(Trix *trx, DictMode mode = DictMode::ReadWriteFixed, Object *mark_floor = nullptr) {
        auto [mark_ptr, n] = trx->find_opstack_mark(mark_floor);
        if ((n % 2) != 0) {
            cleanup_opstack_to_mark(trx, mark_ptr);
            trx->error(Error::RangeCheck, "mismatched number of key/value pairs when creating a dict");
        } else {
            auto top_ptr = trx->m_op_ptr;
            validate_dict_keys(trx, mark_ptr, top_ptr);

            auto count = static_cast<length_t>(n / 2);
            auto capacity = std::max(count, static_cast<length_t>(4));
            auto [dict, offset] = Dict::create_global_dict(trx, capacity, mode);
            for (auto key = (mark_ptr + 1); key < top_ptr; key += 2) {
                auto value = (key + 1);
                dict->put(trx, *key, *value);
            }

            trx->m_op_ptr = (mark_ptr - 1);
            return make_dict(offset);
        }
    }

    [[nodiscard]] static Object
    make_eqdict_from_mark(Trix *trx, DictMode mode = DictMode::ReadWriteFixed, Object *mark_floor = nullptr) {
        auto [mark_ptr, n] = trx->find_opstack_mark(mark_floor);
        if ((n % 2) != 0) {
            cleanup_opstack_to_mark(trx, mark_ptr);
            trx->error(Error::RangeCheck, "mismatched number of key/value pairs when creating a dict");
        } else {
            auto top_ptr = trx->m_op_ptr;
            validate_dict_keys(trx, mark_ptr, top_ptr);

            auto count = static_cast<length_t>(n / 2);
            if (trx->m_eqdict_maxlength < count) {
                cleanup_opstack_to_mark(trx, mark_ptr);
                trx->error(Error::LimitCheck, "dict length {} exceeds =dict max length {}", count, trx->m_eqdict_maxlength);
            } else if (trx->m_eqdict_generation == std::numeric_limits<uint32_t>::max()) {
                cleanup_opstack_to_mark(trx, mark_ptr);
                trx->error(Error::LimitCheck,
                           "<<>>#= generation counter exhausted (2^32 creations); cannot create more eqdict refs");
            } else {
                auto access = (mode == DictMode::ReadOnly) ? ReadOnlyAccess : ReadWriteAccess;
                auto dict = trx->m_eqdict;
                dict->reset_dict(trx, count, access);

                // Note: put() transfers each key/value's ExtValue into the dict entry.  If a
                // later put() throws VMFull (only possible for string keys, which are copied into
                // VM), the opstack is left in a mixed-ownership state: earlier pairs have dangling
                // ExtValue pointers (now owned by the dict), later pairs are still valid.  A
                // PS-level try/catch that calls clear/cleartomark after catching such an error
                // could double-free the already-transferred ExtValues.  This is a shared design
                // limitation of make_dict_from_mark as well and is not easily fixed without
                // restructuring put() to separate string copy from ExtValue transfer.
                for (auto key = (mark_ptr + 1); key < top_ptr; key += 2) {
                    auto value = (key + 1);
                    dict->put(trx, *key, *value);
                }

                trx->m_op_ptr = (mark_ptr - 1);
                // Bump generation; eqdict-ref's value slot holds m_generation (the creation-time
                // counter, not a VM offset).  Accessors resolve through trx->m_eqdict.
                ++trx->m_eqdict_generation;
                return make_eqdict(trx->m_eqdict_generation);
            }
        }
    }

    // Validate that every element between mark_ptr and top_ptr is non-null.
    // Cleans up the operand stack (frees ExtValues, resets m_op_ptr) before erroring.
    static void validate_set_elements(Trix *trx, Object *mark_ptr, const Object *top_ptr) {
        for (auto elem = (mark_ptr + 1); elem <= top_ptr; ++elem) {
            if (elem->is_null()) {
                cleanup_opstack_to_mark(trx, mark_ptr);
                trx->error(Error::TypeCheck, "null is not a valid set element");
            }
        }
    }

    // Constructs a set from all operand stack elements above the matching '{{' mark.
    // Elements may be any hashable type (same restrictions as dict keys: no Null).
    [[nodiscard]] static Object
    make_set_from_mark(Trix *trx, DictMode mode = DictMode::ReadWriteFixed, Object *mark_floor = nullptr) {
        // Mirror of make_tagged: when m_curr_alloc_global is set, route
        // the set's storage into global VM so per-iteration runtime
        // {{...}} literals land where GC can reclaim them.
        // See make_array_from_mark for the matching arc.
        if (trx->m_curr_alloc_global) {
            return make_global_set_from_mark(trx, mode, mark_floor);
        } else {
            auto [mark_ptr, n] = trx->find_opstack_mark(mark_floor);
            validate_set_elements(trx, mark_ptr, trx->m_op_ptr);

            auto count = static_cast<length_t>(n);
            auto capacity = std::max(count, static_cast<length_t>(4));
            auto [set, offset] = Dict::create_set(trx, capacity, mode);
            for (auto elem = (mark_ptr + 1); elem <= trx->m_op_ptr; ++elem) {
                set->set_put(trx, *elem);
            }

            trx->m_op_ptr = (mark_ptr - 1);
            return make_set(offset);
        }
    }

    // Mirror of make_set_from_mark that allocates the set struct in the
    // global VM region.  Backs the {{...}}#$ literal suffix.
    [[nodiscard]] static Object
    make_global_set_from_mark(Trix *trx, DictMode mode = DictMode::ReadWriteFixed, Object *mark_floor = nullptr) {
        auto [mark_ptr, n] = trx->find_opstack_mark(mark_floor);
        validate_set_elements(trx, mark_ptr, trx->m_op_ptr);

        auto count = static_cast<length_t>(n);
        auto capacity = std::max(count, static_cast<length_t>(4));
        auto [set, offset] = Dict::create_global_set(trx, capacity, mode);
        for (auto elem = (mark_ptr + 1); elem <= trx->m_op_ptr; ++elem) {
            set->set_put(trx, *elem);
        }

        trx->m_op_ptr = (mark_ptr - 1);
        return make_set(offset);
    }

    [[nodiscard]] static Object
    make_eqset_from_mark(Trix *trx, DictMode mode = DictMode::ReadWriteFixed, Object *mark_floor = nullptr) {
        auto [mark_ptr, n] = trx->find_opstack_mark(mark_floor);
        validate_set_elements(trx, mark_ptr, trx->m_op_ptr);

        auto count = static_cast<length_t>(n);
        if (trx->m_eqset_maxlength < count) {
            cleanup_opstack_to_mark(trx, mark_ptr);
            trx->error(Error::LimitCheck, "set length {} exceeds =set max length {}", count, trx->m_eqset_maxlength);
        } else if (trx->m_eqset_generation == std::numeric_limits<uint32_t>::max()) {
            cleanup_opstack_to_mark(trx, mark_ptr);
            trx->error(Error::LimitCheck, "{{}}#= generation counter exhausted (2^32 creations); cannot create more eqset refs");
        } else {
            auto access = (mode == DictMode::ReadOnly) ? ReadOnlyAccess : ReadWriteAccess;
            auto set = trx->m_eqset;
            // Resets to the full configured ceiling, not `count` like make_eqdict.
            // Both are safe for the one-shot literal build: count <= m_eqset_maxlength
            // was checked above, and no further puts follow.
            set->reset_set(trx, trx->m_eqset_maxlength, access);

            for (auto elem = (mark_ptr + 1); elem <= trx->m_op_ptr; ++elem) {
                set->set_put(trx, *elem);
            }

            trx->m_op_ptr = (mark_ptr - 1);
            // Bump generation; eqset-ref's value slot holds m_generation (the creation-time
            // counter, not a VM offset).  Accessors resolve through trx->m_eqset.
            ++trx->m_eqset_generation;
            return make_eqset(trx->m_eqset_generation);
        }
    }

    [[nodiscard]] Dict *dict_value(Trix *trx) const {
        assert(is_dict());

        if (is_eqdict_ref()) {
            if (m_generation != trx->m_eqdict_generation) {
                trx->error(Error::Unsupported, "stale <<>>#= reference: storage reused by a subsequent <<>>#=");
            } else {
                return trx->m_eqdict;
            }
        }
        return trx->offset_to_ptr<Dict>(m_dict);
    }

    void dict_clear(Trix *trx) {
        assert(is_dict());

        dict_value(trx)->clear(trx);
    }

    [[nodiscard]] uint16_t object_length() const {
        assert(has_object_length());

        return m_length;
    }

    [[nodiscard]] uint16_t value_length(Trix *trx) const {
        assert(has_value_length());

        return (is_set() ? set_value(trx)->length() : dict_value(trx)->length());
    }

    [[nodiscard]] uint16_t length(Trix *trx) const {
        assert(has_length());

        if (has_object_length()) {
            return m_length;
        } else if (is_set()) {
            return set_value(trx)->length();
        } else {
            return dict_value(trx)->length();
        }
    }

    [[nodiscard]] vm_offset_t offset() const {
        assert(uses_vm());

        return m_offset;
    }

    [[nodiscard]] vm_offset_t extvalue_offset() const {
        assert(uses_extvalue());

        return m_offset;
    }

    // Unified offset accessor for any Object whose backing storage lives
    // in VM memory -- covers both uses_vm() (Array, Dict, String, Name,
    // Curry, Thunk, Tagged, Record, Set, Packed, Continuation,
    // OpaqueHandle) and uses_heap_cell() (ExtValue scalars: Long, ULong,
    // Double, Address; WideValue scalars: Int128, UInt128).  Used by the
    // R6 pointer-hygiene check (Save::reject_local_into_global) which
    // needs a single offset to compare against m_vm_global regardless
    // of how the Object encodes its reference.
    [[nodiscard]] vm_offset_t storage_offset() const {
        assert(uses_vm() || uses_heap_cell());

        return m_offset;
    }

    // Write 1-4 value bytes in big-endian order.  Returns pointer past the last byte written.
    static packed_data_t *pack_value_bytes(packed_data_t *dst, vm_offset_t value, int value_size) {
        switch ((value_size - 1) & 3) {
        case 3:
            *dst++ = static_cast<packed_data_t>((value >> 24) & 0xFF);
            [[fallthrough]];
        case 2:
            *dst++ = static_cast<packed_data_t>((value >> 16) & 0xFF);
            [[fallthrough]];
        case 1:
            *dst++ = static_cast<packed_data_t>((value >> 8) & 0xFF);
            [[fallthrough]];
        case 0:
            *dst++ = static_cast<packed_data_t>(value & 0xFF);
            break;
        }
        return dst;
    }

    // Determine the minimum number of bytes to represent a packed value.
    //
    // Sign-aware byte stripping for Integer and Operator types:
    //
    // Signed values use two's complement, so the redundant leading
    // bytes depend on sign.  A leading byte is stripped only when
    // the remaining MSB preserves the correct sign bit:
    //
    //   Value         Raw bytes       Stored    MSB bit7   Extend   Result
    //   -1            FF FF FF FF     FF        1          0xFF     FF FF FF FF
    //   -128          FF FF FF 80     80        1          0xFF     FF FF FF 80
    //   -129          FF FF FF 7F     FF 7F     1          0xFF     FF FF FF 7F
    //    127          00 00 00 7F     7F        0          0x00     00 00 00 7F
    //    128          00 00 00 80     00 80     0          0x00     00 00 00 80
    //    255          00 00 00 FF     00 FF     0          0x00     00 00 00 FF
    //
    // Key: 128 and -128 both have 0x80 in the low byte, but 128
    // needs 2 bytes (00 80) -- stripping the 00 would leave bit 7
    // set and the unpacker would incorrectly sign-extend to -128.
    //
    // Unsigned types (UInteger, Long, ULong, Address, Real, Double,
    // Name) strip leading 0x00 bytes only.
    //
    // Name must use at least 2 bytes so bind_packed() can overwrite
    // it in-place with an Operator index.
    [[nodiscard]] static int packed_value_size(packed_data_t packed_type, vm_offset_t value) {
        if (packed_type == +PackedType::Integer) {
            // 32-bit signed: strip redundant 0x00 or 0xFF leading bytes
            auto sv = static_cast<int32_t>(static_cast<uint32_t>(value));
            if ((sv >= -128) && (sv <= 127)) {
                return 1;
            } else if ((sv >= -32768) && (sv <= 32767)) {
                return 2;
            } else if ((sv >= -8388608) && (sv <= 8388607)) {
                return 3;
            } else {
                return 4;
            }
        } else if (packed_type == +PackedType::Operator) {
            // 16-bit signed: strip redundant leading byte
            auto sv = static_cast<int16_t>(static_cast<uint16_t>(value));
            return (((sv >= -128) && (sv <= 127)) ? 1 : 2);
        } else if (packed_type == +PackedType::Name) {
            // Name must use at least 2 bytes (bind_packed compatibility)
            if (value > 0x00FFFFFF) {
                return 4;
            } else if (value > 0x0000FFFF) {
                return 3;
            } else {
                return 2;
            }
        } else {
            // Unsigned types: strip leading 0x00 bytes
            if (value > 0x00FFFFFF) {
                return 4;
            } else if (value > 0x0000FFFF) {
                return 3;
            } else if (value > 0x000000FF) {
                return 2;
            } else {
                return 1;
            }
        }
    }

    struct PackedEncoding {
        packed_data_t packed_type;
        packed_data_t attrib;
        packed_data_t SS_value;
        int length_size;
        length_t length;
        int value_size;
        vm_offset_t value;
    };

    [[nodiscard]] static PackedEncoding classify_for_packing(Trix *trx, const Object *object) {
        auto is_readonly = (object->has_object_access() && object->has_readonly_access());
        auto attrib = object->is_executable() ? PackedExecutableAttrib : PackedLiteralAttrib;
        auto SS_value = SS_Null;
        auto packed_type = PackedType::Simple;

        auto length_size = 0;
        auto length = length_t{0};
        auto value_size = -1;  // -2 = pre-computed SS, -1 = auto-calculate, 1-4 = explicit byte count
        auto value = vm_offset_t{0};

        // Eqref family routes to PackedExt: value carries m_generation (not an
        // offset), and for String/Array/ProcArray/ProcPacked the encoding also
        // carries the per-ref length (written after the subcode, before gen, in
        // 1 or 2 bytes per Short/Long subcode variant).  Dict/Set carry no length.
        // Checked before the type switch because eqrefs reuse the normal type tags.
        if (object->is_eqref()) {
            packed_type = PackedType::PackedExt;
            value = object->m_generation;
            value_size = packed_value_size(+packed_type, value);
            if (object->is_eqdict_ref() || object->is_eqset_ref()) {
                length = 0;
                length_size = 0;
            } else {
                length = object->is_string() ? object->m_string_length : object->m_arrays_length;
                length_size = (length < 256) ? 1 : 2;
            }
            return PackedEncoding{+packed_type, attrib, SS_value, length_size, length, value_size, value};
        } else {
            switch (object->type()) {
            case Type::Null:
                packed_type = PackedType::Simple;
                SS_value = SS_Null;
                value_size = -2;
                break;

            case Type::Byte:
                packed_type = PackedType::Byte;
                value = object->m_byte;
                value_size = 1;
                break;

            case Type::Integer:
                packed_type = PackedType::Integer;
                value = static_cast<vm_offset_t>(static_cast<uint32_t>(object->m_integer));
                break;

            case Type::UInteger:
                packed_type = PackedType::UInteger;
                value = object->m_uinteger;
                break;

            case Type::Long:
                packed_type = PackedType::Long;
                value = object->m_long;
                break;

            case Type::ULong:
                packed_type = PackedType::ULong;
                value = object->m_ulong;
                break;

            case Type::Address:
                packed_type = PackedType::Address;
                attrib = PackedLiteralAttrib;  // strip address cache bits
                value = object->m_address;
                break;

            case Type::Real:
                packed_type = PackedType::Real;
                value = object->m_offset;  // read m_real's bit pattern via union (m_offset overlays m_real)
                break;

            case Type::Double:
                packed_type = PackedType::Double;
                value = object->m_double;
                break;

            case Type::Boolean:
                packed_type = PackedType::Simple;
                value_size = -2;
                SS_value = object->m_boolean ? SS_True : SS_False;
                break;

            case Type::Operator: {
                // Check for the 8 most-frequent operators (post-bind procedures).
                // Executable common-op -> PackedCommonOp (1 byte).
                // Literal op or any other op -> PackedOperator (2+ bytes).
                auto op = object->m_operator;
                auto common_op_count = std::size(sm_common_op_index);
                auto slot = common_op_count;

                if (object->is_executable()) {
                    for (slot = 0; slot < common_op_count; ++slot) {
                        if (op == sm_common_op_index[slot]) {
                            break;
                        }
                    }
                }
                if (slot != common_op_count) {
                    // Encode slot (0-7) into X and SS header bits:
                    // X = slot bit 2, SS = slot bits 1..0.  Decode always forces Executable.
                    packed_type = PackedType::CommonOp;
                    attrib = (slot >= 4) ? PackedExecutableAttrib : PackedLiteralAttrib;
                    SS_value = (slot & 3);
                    value_size = -2;
                } else {
                    packed_type = PackedType::Operator;
                    value = static_cast<vm_offset_t>(static_cast<uint16_t>(op));
                }
                break;
            }

            case Type::Mark:
                packed_type = PackedType::Simple;
                SS_value = SS_Mark;
                value_size = -2;
                break;

            case Type::Name:
                packed_type = PackedType::Name;
                value = object->m_name;
                break;

            case Type::Array:
                length = object->m_arrays_length;
                if (length < 256) {
                    packed_type = is_readonly ? PackedType::ReadOnlyShortLengthArray : PackedType::ShortLengthArray;
                    length_size = 1;
                } else {
                    packed_type = is_readonly ? PackedType::ReadOnlyLongLengthArray : PackedType::LongLengthArray;
                    length_size = 2;
                }
                value = object->m_array;
                break;

            case Type::Packed:
                length = object->m_arrays_length;
                if (length < 256) {
                    packed_type = PackedType::ReadOnlyShortLengthPacked;
                    length_size = 1;
                } else {
                    packed_type = PackedType::ReadOnlyLongLengthPacked;
                    length_size = 2;
                }
                value = object->m_packed;
                break;

            case Type::String:
                length = object->m_string_length;
                if (length < 256) {
                    packed_type = is_readonly ? PackedType::ReadOnlyShortLengthString : PackedType::ShortLengthString;
                    length_size = 1;
                } else {
                    packed_type = is_readonly ? PackedType::ReadOnlyLongLengthString : PackedType::LongLengthString;
                    length_size = 2;
                }
                value = object->m_string;
                break;

            case Type::Stream:
                length = object->m_sid;
                if (length < 256) {
                    packed_type = is_readonly ? PackedType::ReadOnlyShortLengthStream : PackedType::ShortLengthStream;
                    length_size = 1;
                } else {
                    packed_type = is_readonly ? PackedType::ReadOnlyLongLengthStream : PackedType::LongLengthStream;
                    length_size = 2;
                }
                value = object->m_stream;
                break;

            case Type::Dict:
                packed_type = PackedType::Dict;
                value = object->m_dict;
                break;

            case Type::Set:
                packed_type = PackedType::Dict;  // Set shares Dict's packed slot
                value = object->m_set;
                break;

            case Type::SourceLoc:
                trx->error(Error::TypeCheck, "make_packed_data: SourceLoc must not be packed");

            case Type::Curry:
                packed_type = PackedType::Curry;
                value = object->m_curry;
                break;

            case Type::Tagged:
                packed_type = PackedType::Curry;  // Tagged shares Curry's packed slot
                value = object->m_tagged;
                break;

            case Type::Thunk:
                trx->error(Error::TypeCheck, "make_packed_data: Thunk cannot be packed");

            case Type::Coroutine:
                trx->error(Error::TypeCheck, "make_packed_data: Coroutine cannot be packed");

            case Type::PipeBuffer:
                trx->error(Error::TypeCheck, "make_packed_data: PipeBuffer cannot be packed");

            case Type::Cell:
                trx->error(Error::TypeCheck, "make_packed_data: Cell cannot be packed");

            case Type::Continuation:
                trx->error(Error::TypeCheck, "make_packed_data: Continuation cannot be packed");

            case Type::OpaqueHandle:
                // OpaqueHandle rides PackedExt (same slot as Int128/UInt128/eqrefs)
                // with a per-kind subcode.  HandleKind goes in the length bytes
                // (always 1 since kinds fit in a uint8_t today; m_length is
                // length_t/uint16_t and supports the 2-byte form via packed_ext_length_bytes
                // if the kind space ever grows).  value = m_handle = vm_offset_t.
                packed_type = PackedType::PackedExt;
                value = object->m_handle;
                length = +object->m_handle_kind;
                length_size = (length < 256) ? 1 : 2;
                break;

            case Type::Record:
                packed_type = PackedType::Record;
                value = object->m_record;
                break;

            case Type::Int128:
                // 128-bit values use the PackedExt slot with a subcode byte discriminator.
                // The actual emission is handled specially in emit_packed_element().
                packed_type = PackedType::PackedExt;
                value = object->m_int128;
                break;

            case Type::UInt128:
                packed_type = PackedType::PackedExt;
                value = object->m_uint128;
                break;
            }

            if (value_size == -1) {
                value_size = packed_value_size(+packed_type, value);
            }

            return PackedEncoding{+packed_type, attrib, SS_value, length_size, length, value_size, value};
        }
    }

    static int emit_packed_element(Trix *trx, packed_data_t *&ptr, const Object *object, const PackedEncoding &enc) {
        auto packed_type = static_cast<PackedType>(enc.packed_type);
        auto attrib = enc.attrib;
        auto SS_value = enc.SS_value;
        auto length_size = enc.length_size;
        auto length = enc.length;
        auto value_size = enc.value_size;
        auto value = enc.value;

        // PackedRecord: custom encoding -- X=fc_width, SS=offset_size-1, TTTTT=Record
        // [header] [field_count: 1 or 2 bytes] [offset: SS+1 bytes]
        if (packed_type == PackedType::Record) {
            auto fc = object->object_length();
            auto x_bit = static_cast<packed_data_t>((fc > 255) ? 0x80 : 0);
            auto ss = static_cast<packed_data_t>(((value_size - 1) & 3) << 5);
            *ptr++ = (x_bit | ss | +PackedType::Record);
            if (fc > 255) {
                *ptr++ = static_cast<packed_data_t>((fc >> 8) & 0xFF);
            }
            *ptr++ = static_cast<packed_data_t>(fc & 0xFF);
            ptr = pack_value_bytes(ptr, value, value_size);
            return (static_cast<int>(sizeof(packed_header_t)) + ((fc > 255) ? 2 : 1) + value_size);
        } else {
            // PackedExt: custom encoding -- X=0, SS=value_size-1, TTTTT=PackedExt
            // [header] [subcode: 1 byte] [length: 0/1/2 bytes per subcode] [gen: SS+1 bytes]
            // Int128/UInt128: no length; value is a VM-heap offset to a WideValue cell.
            // Eq String/Array/ProcArray/ProcPacked: 1 byte length if < 256 (Short variant),
            //   else 2 bytes (Long variant); value is m_generation.
            // Eq Dict/Set: no length (Dict/Set carry length on the Dict body); value is m_generation.
            if (packed_type == PackedType::PackedExt) {
                auto ss = static_cast<packed_data_t>(((value_size - 1) & 3) << 5);
                *ptr++ = (ss | +PackedType::PackedExt);
                auto is_short = (length_size == 1);
                packed_data_t subcode;
                if (object->is_int128()) {
                    subcode = PackedExtSubcode_Int128;
                } else if (object->is_uint128()) {
                    subcode = PackedExtSubcode_UInt128;
                } else if (object->is_eqstring_ref()) {
                    if (object->has_readonly_access()) {
                        subcode = is_short ? PackedExtSubcode_EqStringShortRO : PackedExtSubcode_EqStringLongRO;
                    } else {
                        subcode = is_short ? PackedExtSubcode_EqStringShortRW : PackedExtSubcode_EqStringLongRW;
                    }
                } else if (object->is_eqarray_ref()) {
                    if (object->has_readonly_access()) {
                        subcode = is_short ? PackedExtSubcode_EqArrayShortRO : PackedExtSubcode_EqArrayLongRO;
                    } else {
                        subcode = is_short ? PackedExtSubcode_EqArrayShortRW : PackedExtSubcode_EqArrayLongRW;
                    }
                } else if (object->is_eqproc_ref()) {
                    if (object->is_packed()) {
                        subcode = is_short ? PackedExtSubcode_EqProcPackedShort : PackedExtSubcode_EqProcPackedLong;
                    } else if (object->has_readonly_access()) {
                        subcode = is_short ? PackedExtSubcode_EqProcArrayShortRO : PackedExtSubcode_EqProcArrayLongRO;
                    } else {
                        subcode = is_short ? PackedExtSubcode_EqProcArrayShortRW : PackedExtSubcode_EqProcArrayLongRW;
                    }
                } else if (object->is_eqdict_ref()) {
                    subcode = PackedExtSubcode_EqDict;
                } else if (object->is_eqset_ref()) {
                    subcode = PackedExtSubcode_EqSet;
                } else if (object->is_handle()) {
                    subcode = is_short ? PackedExtSubcode_OpaqueHandleShort : PackedExtSubcode_OpaqueHandleLong;
                } else {
                    trx->error(Error::InternalError, "emit_packed_element: unclassified PackedExt source");
                }
                *ptr++ = subcode;
                if (length_size == 2) {
                    *ptr++ = static_cast<packed_data_t>((length >> 8) & 0xFF);
                }
                if (length_size >= 1) {
                    *ptr++ = static_cast<packed_data_t>(length & 0xFF);
                }
                ptr = pack_value_bytes(ptr, value, value_size);
                return (static_cast<int>(sizeof(packed_header_t)) + 1 + length_size + value_size);
            } else {
                // output header: |X|S|S|T|T|T|T|T|  X=Executable, SS=value_size-1, TTTTT=PackedType
                // PackedSimple repurposes SS; use pre-built header.
                // PackedCommonOp repurpose X and SS; use pre-built header.
                if (value_size == -2) {
                    if (SS_value > 3) {
                        trx->error(Error::InternalError, "make_packed_data: SS_value {} out of range", SS_value);
                    } else {
                        SS_value = static_cast<packed_data_t>((SS_value & 3) << 5);
                    }
                } else if ((value_size < 1) || (value_size > 4)) {
                    trx->error(Error::InternalError, "make_packed_data: value_size {} out of range", value_size);
                } else {
                    SS_value = static_cast<packed_data_t>(((value_size - 1) & 3) << 5);
                }
                *ptr++ = (attrib | SS_value | +packed_type);

                // output length
                if (length_size > 0) {
                    if (length_size == 2) {
                        *ptr++ = static_cast<packed_data_t>((length >> 8) & 0xFF);
                    }
                    *ptr++ = static_cast<packed_data_t>(length & 0xFF);
                }

                // output value
                if (value_size > 0) {
                    ptr = pack_value_bytes(ptr, value, value_size);
                }

                // value_size == -2 means a special single-byte header (Simple/CommonOp)
                // with no value bytes; use 0 for accounting to avoid unsigned promotion.
                auto actual_value_size = (value_size > 0) ? value_size : 0;
                return (static_cast<int>(sizeof(packed_header_t)) + length_size + actual_value_size);
            }
        }
    }

    // Encodes count Objects starting at object into a compact packed byte stream in VM memory.
    // is_eqproc: if true, appends to trx->m_eqproc_storage_ptr instead of the VM heap.
    // Returns {ptr, offset} pointing to the start of the packed data, or {nullptr, nulloffset}
    // if there is insufficient VM space for the worst-case encoding.
    //
    // Eq-storage refs encode through PackedExt: subcode byte identifies the eq flavor
    // (string/array/proc-array/proc-packed/dict/set) and attrib/access; value bytes
    // carry the creation-time m_generation.  Length and backing buffer are recovered
    // from Trix state on decode.  A ref embedded in a packed stream may go stale if
    // its eq-storage is reused before the stream is replayed, in which case access
    // raises the usual stale-ref Unsupported error.
    [[nodiscard]] static std::pair<packed_data_t *, vm_offset_t>
    make_packed_data(Trix *trx, const Object *object, length_t count, bool is_eqproc = false) {
        auto MAX_PACKED_SIZE = sizeof(packed_header_t) + sizeof(length_t) + sizeof(vm_offset_t);
        if (trx->vm_remaining<packed_data_t>() >= (MAX_PACKED_SIZE * count)) {
            auto packed_size = vm_size_t{0};
            auto [ptr, remaining] = trx->vm_start_alloc<packed_data_t>();
            for (auto remaining_count = count; remaining_count-- != 0; ++object) {
                auto enc = classify_for_packing(trx, object);
                auto n = static_cast<vm_size_t>(emit_packed_element(trx, ptr, object, enc));
                remaining -= n;
                packed_size += n;
            }

            if (is_eqproc) {
                auto eqproc_packed_size = (trx->m_eqproc_length * sizeof(Object));
                if (packed_size > eqproc_packed_size) {
                    trx->error(Error::LimitCheck,
                               "'proc' packed size {} exceeds =proc packed size {}",
                               packed_size,
                               eqproc_packed_size);
                } else {
                    // Free old eqproc Objects (if any) before overwriting with packed bytes.
                    for (length_t i = 0; i < trx->m_eqproc_stored_length; ++i) {
                        trx->m_eqproc_storage_ptr[i].maybe_free_extvalue(trx);
                    }
                    trx->m_eqproc_stored_length = 0;  // eqproc_storage now holds packed bytes, not Objects
                    // Packed data was built in VM scratch space (vm_start_alloc).
                    // Discard the VM allocation (size 0) and copy into the reusable eqproc buffer.
                    auto vm_ptr = trx->vm_end_alloc_ptr<packed_data_t>(0);
                    auto eqproc_ptr = reinterpret_cast<packed_data_t *>(trx->m_eqproc_storage_ptr);
                    std::copy_n(vm_ptr, packed_size, eqproc_ptr);
                    return std::pair{eqproc_ptr, trx->ptr_to_offset(eqproc_ptr)};
                }
            } else if (trx->m_curr_alloc_global) {
                // Routes through m_curr_alloc_global: cancel the temp allocation,
                // alloc a global block tagged ChunkKind::Packed, and copy the
                // packed bytes in.  Mirrors the eqproc copy-out pattern above.
                auto src = trx->vm_end_alloc_ptr<packed_data_t>(0);
                auto [dst, dst_offset] = trx->gvm_alloc<packed_data_t>(packed_size, Trix::ChunkKind::Packed);
                // Stamp the user-visible element count into the block
                // header's m_obj_count field.  GC's Packed walker uses
                // it as the decode bound -- the byte stream has no
                // self-terminator and trailing alignment/slack bytes
                // would otherwise mis-decode as malformed elements.
                trx->gvm_set_obj_count(dst_offset, count);
                std::copy_n(src, packed_size, dst);
                return std::pair{dst, dst_offset};
            } else {
                return trx->vm_end_alloc<packed_data_t>(packed_size);
            }
        } else {
            return std::pair{nullptr, nulloffset};
        }
    }

    static void sign_extend_packed(vm_offset_t &value, int value_size, int full_size) {
        if (value_size < full_size) {
            auto sign_bit = vm_offset_t{1} << ((value_size * 8) - 1);
            if (value & sign_bit) {
                value |= ~((sign_bit << 1) - 1);
            }
        }
    }

    // Pure value-in / value-out: takes the partially-decoded Object and
    // the current packed_data cursor by value, returns both updated.
    // Pair order matches extract_next_packed's return so the caller is
    // a direct `return fixup_decoded_object(...)`.
    //
    // always_inline: extract_next_packed is the ONLY caller, and at -O2 GCC
    // otherwise leaves this as a tail call whose by-value Object round trip
    // through the call ABI costs ~53 Ir per packed element decoded.  Forcing
    // the inline measured -3.5% on bench_interpreter (2026-06-06).
    [[nodiscard, gnu::always_inline]] static inline std::pair<const packed_data_t *, Object> fixup_decoded_object(
            Trix *trx, Object object, const packed_data_t *packed_data, packed_data_t header, aat_t attrib, int value_size) {
        auto packed_type = static_cast<PackedType>(header & PackedTypeMask);

        switch (packed_type) {
        case PackedType::Simple: {
            // SS bits (header bits 6-5) select sub-type; X preserves the executable attribute.
            //   00 = Null, 01 = Mark, 10 = Boolean false, 11 = Boolean true
            auto sub_type = (header >> 5) & 0x03;
            switch (sub_type) {
            case 0:
                object.m_aat = (attrib | ReadWriteAccess | +Type::Null);
                object.m_null = 0;
                break;

            case 1:
                object.m_aat = (attrib | ReadWriteAccess | +Type::Mark);
                object.m_mark = 0;
                break;

            case 2:
                object.m_aat = (attrib | ReadWriteAccess | +Type::Boolean);
                object.m_boolean = false;
                break;

            case 3:
                object.m_aat = (attrib | ReadWriteAccess | +Type::Boolean);
                object.m_boolean = true;
                break;
            }
            break;
        }

        case PackedType::CommonOp: {
            // Reconstruct 3-bit slot: bit 7 (X) is slot bit 2, bits 6-5 (SS) are slot bits 1-0.
            // Always decoded as Executable regardless of the original X bit's attrib meaning.
            auto slot = (((header >> 7) & 1) << 2) | ((header >> 5) & 3);
            object.m_aat = (ExecutableAttrib | ReadWriteAccess | +Type::Operator);
            object.m_operator = static_cast<operator_index_t>(sm_common_op_index[slot]);
            break;
        }

        case PackedType::Integer:
            // Sign-extend from value_size bytes to 32 bits.
            // See sign-aware byte stripping comment in make_packed_data.
            sign_extend_packed(object.m_offset, value_size, 4);
            break;

        case PackedType::Operator:
            // Sign-extend from value_size bytes to 16 bits, then assign.
            // See sign-aware byte stripping comment in make_packed_data.
            sign_extend_packed(object.m_offset, value_size, 2);
            object.m_operator = static_cast<operator_index_t>(object.m_offset);
            break;

        case PackedType::Name: {
            auto name = trx->offset_to_ptr<Name>(object.m_offset);
            object.m_length = name->length();
            break;
        }

        case PackedType::Record: {
            // Packed Record: [header] [field_count: 1 or 2 bytes] [offset: SS+1 bytes]
            // X bit selects field_count width: X=0 -> 1 byte, X=1 -> 2 bytes.
            // SS encodes offset byte count (value_size-1). Record is always Literal.
            auto x_bit = ((header >> 7) & 1);
            length_t fc;
            if (x_bit != 0) {
                fc = static_cast<length_t>((*packed_data++) << 8);
                fc |= *packed_data++;
            } else {
                fc = *packed_data++;
            }
            auto ss = (header >> 5) & 0x03;
            auto offset_size = static_cast<uint8_t>(ss + 1);
            vm_offset_t rec_offset = 0;
            for (uint8_t i = 0; i < offset_size; ++i) {
                rec_offset = ((rec_offset << 8) | *packed_data++);
            }
            object.m_aat = (LiteralAttrib | ReadOnlyAccess | +Type::Record);
            object.m_length = fc;
            object.m_record = rec_offset;
            break;
        }

        case PackedType::PackedExt: {
            // PackedExt: [header] [subcode] [length: 0/1/2 bytes per subcode] [gen: SS+1]
            // Header X=0; SS encodes value byte count (value_size-1).
            // See packed_ext_length_bytes() for subcode -> length-bytes mapping.
            auto subcode = *packed_data++;
            auto length_bytes = packed_ext_length_bytes(subcode);
            length_t decoded_length = 0;
            if (length_bytes == 2) {
                decoded_length = static_cast<length_t>((*packed_data++) << 8);
                decoded_length |= *packed_data++;
            } else if (length_bytes == 1) {
                decoded_length = *packed_data++;
            }
            auto ss = (header >> 5) & 0x03;
            auto value_bytes = static_cast<uint8_t>(ss + 1);
            vm_offset_t value = 0;
            for (uint8_t i = 0; i < value_bytes; ++i) {
                value = ((value << 8) | *packed_data++);
            }
            auto decoded_gen = static_cast<uint32_t>(value);
            switch (subcode) {
            case PackedExtSubcode_Int128:
                object.m_aat = (LiteralAttrib | ReadWriteAccess | +Type::Int128);
                object.m_length = 0;
                object.m_offset = value;
                break;

            case PackedExtSubcode_UInt128:
                object.m_aat = (LiteralAttrib | ReadWriteAccess | +Type::UInt128);
                object.m_length = 0;
                object.m_offset = value;
                break;

            case PackedExtSubcode_EqStringShortRW:
            case PackedExtSubcode_EqStringLongRW:
                object = make_eqstring(decoded_gen, decoded_length, LiteralAttrib, ReadWriteAccess);
                break;

            case PackedExtSubcode_EqStringShortRO:
            case PackedExtSubcode_EqStringLongRO:
                object = make_eqstring(decoded_gen, decoded_length, LiteralAttrib, ReadOnlyAccess);
                break;

            case PackedExtSubcode_EqArrayShortRW:
            case PackedExtSubcode_EqArrayLongRW:
                object = make_eqarray(decoded_gen, decoded_length, LiteralAttrib, ReadWriteAccess);
                break;

            case PackedExtSubcode_EqArrayShortRO:
            case PackedExtSubcode_EqArrayLongRO:
                object = make_eqarray(decoded_gen, decoded_length, LiteralAttrib, ReadOnlyAccess);
                break;

            case PackedExtSubcode_EqProcArrayShortRW:
            case PackedExtSubcode_EqProcArrayLongRW:
                object = make_eqproc_array(decoded_gen, decoded_length, ReadWriteAccess);
                break;

            case PackedExtSubcode_EqProcArrayShortRO:
            case PackedExtSubcode_EqProcArrayLongRO:
                object = make_eqproc_array(decoded_gen, decoded_length, ReadOnlyAccess);
                break;

            case PackedExtSubcode_EqProcPackedShort:
            case PackedExtSubcode_EqProcPackedLong:
                object = make_eqproc_packed(decoded_gen, decoded_length);
                break;

            case PackedExtSubcode_EqDict:
                object = make_eqdict(decoded_gen);
                break;

            case PackedExtSubcode_EqSet:
                object = make_eqset(decoded_gen);
                break;

            case PackedExtSubcode_OpaqueHandleShort:
            case PackedExtSubcode_OpaqueHandleLong:
                // length bytes were HandleKind, value bytes are handle offset.
                object = make_handle(value, static_cast<HandleKind>(decoded_length));
                break;

            default:
                trx->error(Error::InternalError, "extract_next_packed: unknown PackedExt subcode {}", subcode);
            }
            break;
        }

        case PackedType::Reserved2:
            trx->error(Error::InternalError, "extract_next_packed: reserved packed type {}", +packed_type);

        case PackedType::Curry:
            // Curry and Tagged share PackedType::Curry.
            // Curry is always Executable (X=1), Tagged is always Literal (X=0).
            if (attrib == LiteralAttrib) {
                object.m_aat = (object.m_aat & ~TypeMask) | +Type::Tagged;
            }
            break;

        case PackedType::Long:
        case PackedType::ULong:
        case PackedType::Address:
        case PackedType::Double:
        case PackedType::Byte:
        case PackedType::UInteger:
        case PackedType::Real:
        case PackedType::ShortLengthArray:
        case PackedType::LongLengthArray:
        case PackedType::ReadOnlyShortLengthArray:
        case PackedType::ReadOnlyLongLengthArray:
        case PackedType::ReadOnlyShortLengthPacked:
        case PackedType::ReadOnlyLongLengthPacked:
        case PackedType::ShortLengthString:
        case PackedType::LongLengthString:
        case PackedType::ReadOnlyShortLengthString:
        case PackedType::ReadOnlyLongLengthString:
        case PackedType::ShortLengthStream:
        case PackedType::LongLengthStream:
        case PackedType::ReadOnlyShortLengthStream:
        case PackedType::ReadOnlyLongLengthStream:
            break;

        case PackedType::Dict:
            // Dict and Set share PackedType::Dict.  Peek through the offset
            // to the Dict header's SetFlag bit to reconstruct the correct Object type.
            if (trx != nullptr) {
                auto dict = trx->offset_to_ptr<Dict>(object.m_offset);
                if (dict->is_set_data()) {
                    object.m_aat = ((object.m_aat & ~TypeMask) | +Type::Set);
                }
            }
            break;
        }
        return std::pair{packed_data, object};
    }

    // Decodes one element from a packed byte stream.
    // Returns a pointer advanced past the consumed bytes and the decoded Object.
    // packed_data must point to the header byte of a valid packed element.
    //
    // OWNERSHIP: the returned Object is a BORROWED reference.  For ExtValue
    // types (Long, ULong, Double, Address) the Object's m_offset points to
    // the original ExtValue in the packed data's source -- it is NOT a new
    // allocation.  Callers must NOT call maybe_free_extvalue on the result.
    // To obtain an owned copy, call make_clone(trx) on the returned Object.
    [[nodiscard]] static std::pair<const packed_data_t *, Object> extract_next_packed(Trix *trx, const packed_data_t *packed_data) {
        // Sized PackedTypeMask + 1 (not PackedTypeCount) so any 5-bit type field
        // (0..31) indexes in-bounds; the unused slot 31 is a reserved sentinel and
        // header type bits >= PackedTypeCount are rejected below before use.
        static constexpr Type sm_type[PackedTypeMask + 1] = {
                Type::Operator,  // PackedType::CommonOp   (type overridden in switch)
                Type::Byte,      // PackedType::Byte
                Type::Integer,   // PackedType::Integer
                Type::Null,      // PackedType::PackedExt (type overridden in switch via subcode)
                Type::UInteger,  // PackedType::UInteger
                Type::Long,      // PackedType::Long
                Type::ULong,     // PackedType::ULong
                Type::Address,   // PackedType::Address
                Type::Real,      // PackedType::Real
                Type::Double,    // PackedType::Double
                Type::Null,      // PackedType::Simple     (type overridden in switch)
                Type::Curry,     // PackedType::Curry  (or Tagged if X=0; resolved in switch)
                Type::Operator,  // PackedType::Operator
                Type::Null,      // PackedType::Reserved2
                Type::Record,    // PackedType::Record  (field_count set in switch)
                Type::Name,      // PackedType::Name
                Type::Array,     // PackedType::ShortLengthArray
                Type::Array,     // PackedType::LongLengthArray
                Type::Array,     // PackedType::ReadOnlyShortLengthArray
                Type::Array,     // PackedType::ReadOnlyLongLengthArray
                Type::Packed,    // PackedType::ReadOnlyShortLengthPacked
                Type::Packed,    // PackedType::ReadOnlyLongLengthPacked
                Type::String,    // PackedType::ShortLengthString
                Type::String,    // PackedType::LongLengthString
                Type::String,    // PackedType::ReadOnlyShortLengthString
                Type::String,    // PackedType::ReadOnlyLongLengthString
                Type::Stream,    // PackedType::ShortLengthStream
                Type::Stream,    // PackedType::LongLengthStream
                Type::Stream,    // PackedType::ReadOnlyShortLengthStream
                Type::Stream,    // PackedType::ReadOnlyLongLengthStream
                Type::Dict,      // PackedType::Dict
                Type::Null       // 31: reserved/invalid (header rejected before use)
        };

        // header: |X|S|S|T|T|T|T|T|  X=Executable, SS=value_size-1, TTTTT=PackedType
        // layout: [header][save_level if ExtValue][0..2 length bytes][0..4 value bytes]
        const packed_header_t header = *packed_data++;

        // Reject an out-of-range type field before indexing the tables.  Internally
        // produced streams never emit type bits >= PackedTypeCount, but a corrupt or
        // hostile snapshot blob can, and the packed bytes are not validated at thaw.
        if ((header & PackedTypeMask) >= PackedTypeCount) {
            trx->error(Error::InternalError,
                       "extract_next_packed: packed type {} out of range [0, {})",
                       static_cast<unsigned>(header & PackedTypeMask),
                       static_cast<unsigned>(PackedTypeCount));
        }

        auto [length_size, value_size, is_readonly] = packed_sizes(header);
        length_t length = 0;
        if (length_size > 0) {
            if (length_size == 2) {
                length = static_cast<length_t>((*packed_data++) << 8);
            }
            length |= *packed_data++;
        }

        auto attrib = ((header & PackedAttribMask) == PackedExecutableAttrib) ? ExecutableAttrib : LiteralAttrib;
        auto access = is_readonly ? ReadOnlyAccess : ReadWriteAccess;
        auto packed_type = static_cast<PackedType>(header & PackedTypeMask);

        Object object;
        object.m_aat = attrib | access | +sm_type[+packed_type];
        object.m_object_save_level = Save::BASE;
        object.m_length = length;
        object.m_offset = 0;

        if (value_size > 0) {
            switch ((value_size - 1) & 3) {
            case 3:
                object.m_offset = (static_cast<vm_offset_t>(*packed_data++) << 24);
                [[fallthrough]];
            case 2:
                object.m_offset |= (static_cast<vm_offset_t>(*packed_data++) << 16);
                [[fallthrough]];
            case 1:
                object.m_offset |= (static_cast<vm_offset_t>(*packed_data++) << 8);
                [[fallthrough]];
            case 0:
                object.m_offset |= static_cast<vm_offset_t>(*packed_data++);
                break;
            }
        }

        return fixup_decoded_object(trx, object, packed_data, header, attrib, value_size);
    }

    // Non-const overload for mutable packed data (e.g. bind_packed writes through the pointer).
    [[nodiscard]] static std::pair<packed_data_t *, Object> extract_next_packed(Trix *trx, packed_data_t *packed_data) {
        auto [next, obj] = extract_next_packed(trx, static_cast<const packed_data_t *>(packed_data));
        return std::pair{const_cast<packed_data_t *>(next), obj};
    }

    enum struct ExtractPackedDestination {
        Array,    // write into existing array slots: free/save old value, write clone, set save level
        EqArray,  // same as Array but skips per-element saves (dict's save mechanism covers them)
        Stack,    // write onto operand stack: clone only, no save/restore handling
        Temp,     // write into a temporary buffer: shallow copy, no ExtValue duplication
    };

    // Decodes count packed elements from packed_data into dst[], applying destination semantics.
    // Returns a pointer advanced past all consumed bytes.
    // Array/EqArray: fully self-contained -- callers must NOT pre-process dst[] before calling.
    static const packed_data_t *
    extract_packed(Trix *trx, const packed_data_t *packed_data, Object *dst, length_t count, ExtractPackedDestination destination) {
        auto curr_save_level = trx->m_curr_save_level;
        while (count-- != 0) {
            auto [next, object] = extract_next_packed(trx, packed_data);
            packed_data = next;

            switch (destination) {
            case ExtractPackedDestination::Array:
            case ExtractPackedDestination::EqArray:
                if ((dst->save_level() == curr_save_level) || (destination == ExtractPackedDestination::EqArray)) {
                    dst->maybe_free_extvalue(trx);
                } else {
                    Save::save_object(trx, dst);
                }
                *dst = object.make_clone(trx);
                dst->set_save_level(curr_save_level);
                break;

            case ExtractPackedDestination::Stack:
                *dst = object.make_clone(trx);
                break;

            case ExtractPackedDestination::Temp:
                *dst = object.make_copy();
                break;
            }

            ++dst;
        }
        return packed_data;
    }

    [[nodiscard]] static const packed_data_t *skip_packed(const packed_data_t *packed_data, length_t count) {
        while (count-- != 0) {
            auto header = *packed_data;
            auto packed_type = static_cast<PackedType>(header & PackedTypeMask);
            if (packed_type == PackedType::Record) {
                // Record: [header] [fc: 1 or 2 bytes] [offset: SS+1 bytes]
                auto x_bit = (header >> 7) & 1;
                auto ss = (header >> 5) & 0x03;
                packed_data += (sizeof(header) + static_cast<size_t>((x_bit != 0) ? 2 : 1) + static_cast<size_t>(ss + 1));
            } else if (packed_type == PackedType::PackedExt) {
                // PackedExt: [header] [subcode: 1 byte] [length: 0/1/2 bytes per subcode] [gen: SS+1]
                auto ss = (header >> 5) & 0x03;
                auto subcode = *(packed_data + 1);
                auto length_bytes = packed_ext_length_bytes(subcode);
                packed_data += (sizeof(header) + 1 + static_cast<size_t>(length_bytes) + static_cast<size_t>(ss + 1));
            } else {
                auto [length_size, value_size, _] = packed_sizes(header);
                packed_data += (sizeof(header) + length_size + value_size);
            }
        }

        return packed_data;
    }

    // Whether an array being bound is a normal array or the eqarray.
    // EqArray skips per-element saves (the dict's own save mechanism covers them).
    enum class ArrayKind { Normal, EqArray };

    // Whether packed-name entries should be saved before overwriting during bind.
    enum class SavePolicy { DoNotSave, Save };

    // Resolve executable name elements within src in-place to their bound operator values.
    // kind: Normal  = save each element to the undo log before overwriting (when at an older save level);
    //       EqArray = skip per-element saves (the enclosing dict's save mechanism covers rollback).
    // Contrast with copy_array (Trix member), which copies elements between two arrays.
    static void bind_array(Trix *trx, Object *src, length_t count, ArrayKind kind) {
        auto curr_save_level = trx->m_curr_save_level;
        for (auto end = src + count; src != end; ++src) {
            if (src->is_executable()) {
                auto save_policy = (src->save_level() != curr_save_level) ? SavePolicy::Save : SavePolicy::DoNotSave;
                if (src->is_name()) {
                    auto value = Name::name_search(trx, src);
                    if ((value != nullptr) && value->is_operator()) {
                        if ((save_policy == SavePolicy::Save) && (kind != ArrayKind::EqArray)) {
                            Save::save_object(trx, src);
                        }
                        *src = *value;
                    }
                } else if (src->is_array()) {
                    auto [head, length] = src->array_value(trx);
                    bind_array(trx, head, length, kind);
                } else if (src->is_packed()) {
                    bind_packed(trx, *src, save_policy);
                }
            }
        }
    }

    // Resolves executable Name elements within a packed array in-place to their bound Operator encoding.
    // save_policy: Save = preserve original bytes in the save log before overwriting (when the
    // packed array was created at an earlier save level than the current one).
    static void bind_packed(Trix *trx, Object packed_obj, SavePolicy save_policy) {
        // Bind only when the packed proc has data.  An empty packed proc (e.g.
        // the body of `{ }` or `{|x| }`) has no data offset -- m_packed is
        // nulloffset -- so there is nothing to bind, and packed_value()/
        // packed_data_ptr() would otherwise call offset_to_ptr(nulloffset) and
        // raise an internal error.  Reachable via #e/bind on a locals proc with
        // an empty body, whose transform nests an empty packed element
        // ([/x K N {} begin-locals]).
        if (packed_obj.m_arrays_length != 0) {
            auto [src, count] = packed_obj.packed_value(trx);
            while (count-- != 0) {
                auto [next, object] = extract_next_packed(trx, src);
                if (object.is_executable()) {
                    if (object.is_name()) {
                        auto name_value = Name::name_search(trx, &object);
                        if ((name_value != nullptr) && name_value->is_operator()) {
                            auto header = *src;
                            auto [length_size, value_size, _] = packed_sizes(header);
                            if ((length_size != 0) || (value_size < 2) || (value_size > 4)) {
                                trx->error(Error::InternalError,
                                           "bind_packed: packed Name has unexpected layout: "
                                           "length_size={} value_size={}",
                                           length_size,
                                           value_size);
                            } else {
                                if (save_policy == SavePolicy::Save) {
                                    // preserve header and value
                                    auto size = static_cast<vm_size_t>(1 + value_size);
                                    Save::save_packed_name(trx, src, size);
                                }

                                // update header
                                auto index = name_value->m_operator;
                                header &= ~PackedTypeMask;
                                header |= +PackedType::Operator;
                                *src++ = header;

                                // Store raw bit pattern (sign-aware encoding)
                                auto value = vm_offset_t{static_cast<uint16_t>(index)};

                                // update value (same encoding as make_packed_data)
                                switch ((value_size - 1) & 3) {
                                case 3:
                                    *src++ = static_cast<packed_data_t>((value >> 24) & 0xFF);
                                    [[fallthrough]];
                                case 2:
                                    *src++ = static_cast<packed_data_t>((value >> 16) & 0xFF);
                                    [[fallthrough]];
                                case 1:
                                    *src++ = static_cast<packed_data_t>((value >> 8) & 0xFF);
                                    [[fallthrough]];
                                case 0:
                                    *src++ = static_cast<packed_data_t>(value & 0xFF);
                                    break;
                                }
                            }
                        }
                    } else if (object.is_array()) {
                        auto array_objects = trx->offset_to_ptr<Object>(object.m_array);
                        bind_array(trx, array_objects, object.m_arrays_length, ArrayKind::Normal);
                    } else if (object.is_packed()) {
                        bind_packed(trx, object, save_policy);
                    }
                }
                src = next;
            }
        }
    }
private:
    friend class Trix;

    struct operator_t {
        operator_flags_t m_flags;
        operator_popcount_t m_popcount;
    };

    using object_attrib_t = uint16_t;
    // PushOpDirect: type is always pushed to the operand stack when the interpreter
    //   encounters it directly as a proc body element, regardless of executable attribute.
    //   Array/Packed use this so that { [ 1 2 ] } treats the inner array as data.
    // IgnoresExecute: type is always pushed to the operand stack even when encountered
    //   via executable name lookup, ignoring the executable attribute entirely.
    //   Numbers/Boolean/Mark etc. use this because they have no meaningful "call" behavior.
    // Key distinction: Array/Packed have PushOpDirect but NOT IgnoresExecute.
    //   Inside a proc body they push as data; reached via executable name lookup they
    //   execute as procedures (pushed onto exec stack by execute_value()).
    static constexpr object_attrib_t PushOpDirect{0x0001};        // pushed on operand stack when encountered directly
    static constexpr object_attrib_t IgnoresExecute{0x0002};      // pushed on operand stack even if executable
    static constexpr object_attrib_t HasObjectAccess{0x0004};     // Has Object level ReadOnly/ReadWrite access
    static constexpr object_attrib_t HasValueAccess{0x0008};      // Has Value level ReadOnly/ReadWrite access
    static constexpr object_attrib_t HasObjectLength{0x0010};     // Has Object level length
    static constexpr object_attrib_t HasValueLength{0x0020};      // Has Value level length
    static constexpr object_attrib_t UsesVM{0x0040};              // m_offset references additional storage in VM
    static constexpr object_attrib_t UsesExtValue{0x0080};        // m_offset references an ExtValue in VM
    static constexpr object_attrib_t IsSignedIntegral{0x0100};    // Integer, Long, Int128
    static constexpr object_attrib_t IsUnsignedIntegral{0x0200};  // Byte, UInteger, ULong, UInt128
    static constexpr object_attrib_t IsFloatingPoint{0x0400};     // Real, Double
    static constexpr object_attrib_t IsSequence{0x0800};          // Array, Packed
    static constexpr object_attrib_t IsCallable{0x1000};          // Array, Packed, Curry
    static constexpr object_attrib_t IsDeepeq{0x2000};            // deep vs shallow eq
    static constexpr object_attrib_t IsPersistable{
            0x4000};  // survives persistent scratch region (inline + ExtValue/WideValue types)
    static constexpr object_attrib_t NeedsExecSlot{
            0x8000};  // executable dispatch pushes self back on exec stack (Name/String/Stream)
    // Note: no UsesWideValue attribute bit -- all 16 attribute bits are used.
    // Int128/UInt128 are the only wide-value types, so uses_widevalue() is a direct
    // type check (see its definition above), not a table lookup.

    // Indexed [0..TypeCount-1] by Type, then [TypeCount..TypeCount+HandleKindCount-1]
    // by HandleKind for Type::OpaqueHandle.  See attrib_index() below: non-handle
    // types take the unchanged path; OpaqueHandle dispatches to its kind row so
    // each opaque handle kind (Screen, future Tilemap, future Sample, ...) can
    // declare its own callable/sequence/length/etc. profile without sharing one
    // generic OpaqueHandle row.  The OpaqueHandle slot at index +Type::OpaqueHandle
    // is a placeholder (zero attrs) and is never read at runtime.
    static constexpr object_attrib_t sm_object_attrib[TypeCount + HandleKindCount] = {
            IsPersistable,                                                                                     // Null
            (PushOpDirect | IgnoresExecute | IsUnsignedIntegral | IsPersistable),                              // Byte
            (PushOpDirect | IgnoresExecute | IsSignedIntegral | IsPersistable),                                // Integer
            (PushOpDirect | IgnoresExecute | IsUnsignedIntegral | IsPersistable),                              // UInteger
            (PushOpDirect | IgnoresExecute | UsesVM | UsesExtValue | IsSignedIntegral | IsPersistable),        // Long
            (PushOpDirect | IgnoresExecute | UsesVM | UsesExtValue | IsUnsignedIntegral | IsPersistable),      // ULong
            (PushOpDirect | IgnoresExecute | UsesVM | UsesExtValue | IsPersistable),                           // Address
            (PushOpDirect | IgnoresExecute | IsFloatingPoint | IsPersistable),                                 // Real
            (PushOpDirect | IgnoresExecute | UsesVM | UsesExtValue | IsFloatingPoint | IsPersistable),         // Double
            (PushOpDirect | IgnoresExecute | IsPersistable),                                                   // Boolean
            IsPersistable,                                                                                     // Operator
            (PushOpDirect | IgnoresExecute | IsPersistable),                                                   // Mark
            (HasObjectLength | UsesVM | IsPersistable | NeedsExecSlot),                                        // Name
            (PushOpDirect | HasObjectAccess | HasObjectLength | UsesVM | IsSequence | IsCallable | IsDeepeq),  // Array
            (PushOpDirect | HasObjectAccess | HasObjectLength | UsesVM | IsSequence | IsCallable | IsDeepeq),  // Packed
            (HasObjectAccess | HasObjectLength | UsesVM | NeedsExecSlot),                                      // String
            (HasObjectAccess | UsesVM | NeedsExecSlot),                                                        // Stream
            (PushOpDirect | IgnoresExecute | HasValueAccess | HasValueLength | UsesVM | IsDeepeq),             // Dict
            (IgnoresExecute | IsPersistable),                                                                  // SourceLoc
            (PushOpDirect | UsesVM | IsCallable | IsDeepeq),                                                   // Curry
            (PushOpDirect | UsesVM | IsDeepeq),                                                                // Thunk
            (PushOpDirect | IgnoresExecute | HasValueAccess | HasValueLength | UsesVM | IsDeepeq),             // Set
            (PushOpDirect | IgnoresExecute | UsesVM | IsDeepeq),                                               // Tagged
            (PushOpDirect | IgnoresExecute | HasObjectLength | UsesVM | IsDeepeq),                             // Record
            (PushOpDirect | IgnoresExecute | UsesVM),                                                          // Coroutine
            (PushOpDirect | IgnoresExecute | UsesVM),                                                          // PipeBuffer
            (PushOpDirect | IgnoresExecute | UsesVM),                                                          // Cell
            (PushOpDirect | UsesVM | IsCallable),                                                              // Continuation
            (PushOpDirect | IgnoresExecute | UsesVM | IsSignedIntegral | IsPersistable),                       // Int128
            (PushOpDirect | IgnoresExecute | UsesVM | IsUnsignedIntegral | IsPersistable),                     // UInt128
            0,                                                                                                 // OpaqueHandle
            (PushOpDirect | IgnoresExecute | UsesVM)                                                           // HandleKind::Screen
    };

    // attrib_index() lifts the lookup key for sm_object_attrib above its raw
    // type-bits form so OpaqueHandle Objects route to a per-kind row instead of
    // sharing one OpaqueHandle row.  Non-handle types take the unchanged path;
    // the branch is [[unlikely]] since OpaqueHandle is rare relative to Integer/
    // Name/Array/Dict in the hot operand-stack paths.
    [[nodiscard]] size_t attrib_index() const {
        auto type_idx = static_cast<size_t>(m_aat & TypeMask);
        if (type_idx == +Type::OpaqueHandle) [[unlikely]] {
            return (TypeCount + +m_handle_kind);
        } else {
            return type_idx;
        }
    }

    using packed_header_t = packed_data_t;
    using packed_type_t = packed_data_t;

    enum struct PackedType : packed_type_t {
        CommonOp,  // X|SS = 3-bit slot index for 8 hot operators
        Byte,
        Integer,
        PackedExt,  // custom encoding: [header][subcode][offset: SS+1]; subcode 0=Int128, 1=UInt128
        UInteger,
        Long,
        ULong,
        Address,
        Real,
        Double,
        Simple,  // SS = 2-bit sub-type (00=Null,01=Mark,10=False,11=True)
        Curry,
        Operator,
        Reserved2,
        Record,
        Name,
        ShortLengthArray,
        LongLengthArray,
        ReadOnlyShortLengthArray,
        ReadOnlyLongLengthArray,
        ReadOnlyShortLengthPacked,
        ReadOnlyLongLengthPacked,
        ShortLengthString,
        LongLengthString,
        ReadOnlyShortLengthString,
        ReadOnlyLongLengthString,
        ShortLengthStream,
        LongLengthStream,
        ReadOnlyShortLengthStream,
        ReadOnlyLongLengthStream,
        Dict
    };
    static constexpr packed_type_t PackedTypeCount{+PackedType::Dict + 1};
    static constexpr packed_type_t PackedTypeMask{0x1F};

    static constexpr packed_data_t SS_Null{0};
    static constexpr packed_data_t SS_Mark{1};
    static constexpr packed_data_t SS_False{2};
    static constexpr packed_data_t SS_True{3};

    // PackedExt subcode byte discriminators.  SS in the header encodes value_size
    // (offset or generation byte count, SS+1).  Int128/UInt128 subcodes carry a
    // VM-heap offset to a WideValue cell.  Eq* subcodes carry the creation-time
    // m_generation; String/Array/ProcArray/ProcPacked subcodes also carry the
    // ref's length (1 byte for Short when length < 256, 2 bytes for Long) ahead
    // of the generation so per-ref length (e.g. set_length-shrunk) survives the
    // round-trip.  Dict/Set subcodes carry no length (Dict/Set track length on
    // the Dict body).  Stale refs still raise on access; fresh refs round-trip
    // exactly.
    static constexpr packed_data_t PackedExtSubcode_Int128{0x00};
    static constexpr packed_data_t PackedExtSubcode_UInt128{0x01};
    static constexpr packed_data_t PackedExtSubcode_EqStringShortRW{0x02};
    static constexpr packed_data_t PackedExtSubcode_EqStringShortRO{0x03};
    static constexpr packed_data_t PackedExtSubcode_EqStringLongRW{0x04};
    static constexpr packed_data_t PackedExtSubcode_EqStringLongRO{0x05};
    static constexpr packed_data_t PackedExtSubcode_EqArrayShortRW{0x06};
    static constexpr packed_data_t PackedExtSubcode_EqArrayShortRO{0x07};
    static constexpr packed_data_t PackedExtSubcode_EqArrayLongRW{0x08};
    static constexpr packed_data_t PackedExtSubcode_EqArrayLongRO{0x09};
    static constexpr packed_data_t PackedExtSubcode_EqProcArrayShortRW{0x0A};
    static constexpr packed_data_t PackedExtSubcode_EqProcArrayShortRO{0x0B};
    static constexpr packed_data_t PackedExtSubcode_EqProcArrayLongRW{0x0C};
    static constexpr packed_data_t PackedExtSubcode_EqProcArrayLongRO{0x0D};
    static constexpr packed_data_t PackedExtSubcode_EqProcPackedShort{0x0E};
    static constexpr packed_data_t PackedExtSubcode_EqProcPackedLong{0x0F};
    static constexpr packed_data_t PackedExtSubcode_EqDict{0x10};
    static constexpr packed_data_t PackedExtSubcode_EqSet{0x11};
    // OpaqueHandle: length bytes carry HandleKind, value bytes carry handle offset.
    // One subcode per discriminator-width: short (1 byte kind) is the only form
    // needed today (kind space is small); reserved long form for future expansion.
    static constexpr packed_data_t PackedExtSubcode_OpaqueHandleShort{0x12};
    static constexpr packed_data_t PackedExtSubcode_OpaqueHandleLong{0x13};

    // Returns the number of length bytes (0, 1, or 2) that precede the generation
    // bytes for a given PackedExt subcode.  Used by emit_packed_element, the
    // PackedExt branch of fixup_decoded_object, and skip_packed.
    [[nodiscard]] static int packed_ext_length_bytes(packed_data_t subcode) {
        switch (subcode) {
        case PackedExtSubcode_EqStringShortRW:
        case PackedExtSubcode_EqStringShortRO:
        case PackedExtSubcode_EqArrayShortRW:
        case PackedExtSubcode_EqArrayShortRO:
        case PackedExtSubcode_EqProcArrayShortRW:
        case PackedExtSubcode_EqProcArrayShortRO:
        case PackedExtSubcode_EqProcPackedShort:
        case PackedExtSubcode_OpaqueHandleShort:
            return 1;

        case PackedExtSubcode_EqStringLongRW:
        case PackedExtSubcode_EqStringLongRO:
        case PackedExtSubcode_EqArrayLongRW:
        case PackedExtSubcode_EqArrayLongRO:
        case PackedExtSubcode_EqProcArrayLongRW:
        case PackedExtSubcode_EqProcArrayLongRO:
        case PackedExtSubcode_EqProcPackedLong:
        case PackedExtSubcode_OpaqueHandleLong:
            return 2;

        default:
            return 0;
        }
    }

    static constexpr name_index_t sm_common_op_index[8] = {
            +SystemName::Exch,
            +SystemName::Dup,
            +SystemName::Pop,
            +SystemName::Index,
            +SystemName::Roll,
            +SystemName::If,
            +SystemName::IfElse,
            +SystemName::Eq,
    };

    struct PackedSizes {
        uint8_t length_size;
        uint8_t value_size;
        bool is_readonly;
    };

    [[nodiscard]] static PackedSizes packed_sizes(packed_type_t header) {
        static constexpr uint8_t VSizeZero{0x80};
        static constexpr uint8_t ReadOnly{0x40};
        static constexpr uint8_t LSizeMask{0x03};
        static constexpr uint8_t LSizeZero{0x00};
        static constexpr uint8_t LSizeOne{0x01};
        static constexpr uint8_t LSizeTwo{0x02};

        // sm_sizes[] entry -- per-type static metadata byte
        //  7 6 5 4 3 2 1 0
        // +-+-+-+-+-+-+-+-+
        // |Z|O|-|-|-|-|L|L|
        // +-+-+-+-+-+-+-+-+
        //  | |         \ /
        //  | |          |
        //  | |          +-- LL: length field size in stream (0=none, 1=1 byte, 2=2 bytes)
        //  | +------------- O:  1 = object is ReadOnly
        //  +--------------- Z:  1 = no value bytes (SS in stream header is ignored)
        // Sized PackedTypeMask + 1 (see sm_type above): slot 31 is a reserved
        // sentinel so packed_sizes/skip_packed never index out of bounds.
        static constexpr uint8_t sm_sizes[PackedTypeMask + 1] = {
                VSizeZero,              // PackedType::CommonOp     X|SS=slot(0-7) Length Size: 0, Value Size:   0 bytes
                LSizeZero,              // PackedType::Byte                        Length Size: 0, Value Size:   1 byte
                LSizeZero,              // PackedType::Integer                     Length Size: 0, Value Size:1..4 bytes
                VSizeZero,              // PackedType::PackedExt X=0 SS=offsz-1    Length Size: 0, Value Size:   0 bytes (custom)
                LSizeZero,              // PackedType::UInteger                    Length Size: 0, Value Size:1..4 bytes
                LSizeZero,              // PackedType::Long                        Length Size: 0, Value Size:1..4 bytes
                LSizeZero,              // PackedType::ULong                       Length Size: 0, Value Size:1..4 bytes
                LSizeZero,              // PackedType::Address                     Length Size: 0, Value Size:1..4 bytes
                LSizeZero,              // PackedType::Real                        Length Size: 0, Value Size:1..4 bytes
                LSizeZero,              // PackedType::Double                      Length Size: 0, Value Size:1..4 bytes
                VSizeZero,              // PackedType::Simple X=attrib SS=sub-type Length Size: 0, Value Size:   0 bytes
                LSizeZero,              // PackedType::Curry                       Length Size: 0, Value Size:1..4 bytes
                LSizeZero,              // PackedType::Operator                    Length Size: 0, Value Size:1..2 bytes
                VSizeZero,              // PackedType::Reserved2                   Length Size: 0, Value Size:   0 bytes
                VSizeZero,              // PackedType::Record   X=fc width         Length Size: 0, Value Size:   0 bytes (custom)
                LSizeZero,              // PackedType::Name                        Length Size: 0, Value Size:2..4 bytes
                LSizeOne,               // PackedType::ShortLengthArray            Length Size: 1, Value Size:1..4 bytes
                LSizeTwo,               // PackedType::LongLengthArray             Length Size: 2, Value Size:1..4 bytes
                (ReadOnly | LSizeOne),  // PackedType::ReadOnlyShortLengthArray    Length Size: 1, Value Size:1..4 bytes
                (ReadOnly | LSizeTwo),  // PackedType::ReadOnlyLongLengthArray     Length Size: 2, Value Size:1..4 bytes
                (ReadOnly | LSizeOne),  // PackedType::ReadOnlyShortLengthPacked   Length Size: 1, Value Size:1..4 bytes
                (ReadOnly | LSizeTwo),  // PackedType::ReadOnlyLongLengthPacked    Length Size: 2, Value Size:1..4 bytes
                LSizeOne,               // PackedType::ShortLengthString           Length Size: 1, Value Size:1..4 bytes
                LSizeTwo,               // PackedType::LongLengthString            Length Size: 2, Value Size:1..4 bytes
                (ReadOnly | LSizeOne),  // PackedType::ReadOnlyShortLengthString   Length Size: 1, Value Size:1..4 bytes
                (ReadOnly | LSizeTwo),  // PackedType::ReadOnlyLongLengthString    Length Size: 2, Value Size:1..4 bytes
                LSizeOne,               // PackedType::ShortLengthStream           Length Size: 1, Value Size:1..4 bytes
                LSizeTwo,               // PackedType::LongLengthStream            Length Size: 2, Value Size:1..4 bytes
                (ReadOnly | LSizeOne),  // PackedType::ReadOnlyShortLengthStream   Length Size: 1, Value Size:1..4 bytes
                (ReadOnly | LSizeTwo),  // PackedType::ReadOnlyLongLengthStream    Length Size: 2, Value Size:1..4 bytes
                LSizeZero,              // PackedType::Dict                        Length Size: 0, Value Size:1..4 bytes
                VSizeZero               // 31: reserved/invalid (no value, no length)
        };

        // Two distinct byte formats are in play -- do not conflate them:
        //   sm_sizes[] entry (above): Z|O|-|-|-|-|L|L  (per-type static metadata)
        //     Z=1: no value bytes; O=1: object is ReadOnly; LL: length field byte count
        //   packed stream header byte (below): X|S|S|T|T|T|T|T
        //     X=1: Executable; SS: value_size-1; TTTTT: PackedType index into sm_sizes
        // packed_sizes() decodes both: reads T to index sm_sizes for Z/O/LL,
        // then reads SS for value_size (ignored when Z=1).
        //
        // packed stream header byte
        //  7 6 5 4 3 2 1 0
        // +-+-+-+-+-+-+-+-+
        // |X|S|S|T|T|T|T|T|
        // +-+-+-+-+-+-+-+-+
        // X: 0 = Literal, 1 = Executable
        // S: value_size - 1  (ignored when Z=1 in sm_sizes)
        // T: PackedType index into sm_sizes (valid types 0..30; 31 is the reserved sentinel)
        auto sizes = sm_sizes[header & PackedTypeMask];
        auto value_size = ((sizes & VSizeZero) != 0) ? static_cast<uint8_t>(0) : static_cast<uint8_t>(((header >> 5) & 0x03) + 1);
        auto is_readonly = ((sizes & ReadOnly) != 0);
        auto length_size = static_cast<uint8_t>(sizes & LSizeMask);
        return PackedSizes{length_size, value_size, is_readonly};
    }

    aat_t m_aat;                           // b7:attrib, b6:access, b5:special flag, b4..b0:type
    union {                                //
        save_level_t m_object_save_level;  // save level
        stream_id_t m_source_sid;          // SourceLoc: source sid
    };  // HEADER (top) 2 bytes
    union {                                      //
        operator_t m_operator_data;              // Operator: flags and popcount
        save_level_t m_extvalue_save_level;      // Long, ULong, Address, Double: ExtValue save level
        save_level_t m_continuation_save_level;  // Continuation: m_curr_save_level at capture time (restore-dangle detection)
        length_t m_name_length;                  // Name: name string length
        length_t m_arrays_length;                // Array, Packed: Object capacity
        length_t m_string_length;                // String: length
        stream_id_t m_sid;                       // Stream: id used to associate Object with Stream buffer
        uint16_t m_source_pos;                   // SourceLoc: source pos
        HandleKind m_handle_kind;                // OpaqueHandle: which HandleKind
        length_t m_length;                       // generic length
    };  // HEADER (bottom) 2 bytes
    union {                           //
        uint32_t m_null;              // unused, initialized to zero
        vm_t m_byte;                  // unsigned 8-bit value
        integer_t m_integer;          // signed 32-bit integer value
        uinteger_t m_uinteger;        // unsigned 32-bit integer value
        vm_offset_t m_long;           // ExtValue signed 64-bit integer value
        vm_offset_t m_ulong;          // ExtValue unsigned 64-bit integer value
        vm_offset_t m_address;        // ExtValue user space address
        real_t m_real;                // 32-bit floating point value
        vm_offset_t m_double;         // ExtValue 64-bit floating point value
        boolean_t m_boolean;          // boolean value
        operator_index_t m_operator;  // index into built-in and user-defined operator tables
        uint32_t m_mark;              // unused, initialized to zero
        vm_offset_t m_name;           // Name
        vm_offset_t m_array;          // Object array_storage
        vm_offset_t m_packed;         // byte-encoded-Object packed_storage
        vm_offset_t m_string;         // vm_t string_storage[m_string_length]
        vm_offset_t m_stream;         // Stream
        vm_offset_t m_dict;           // Dict
        vm_offset_t m_set;            // Set (shares Dict VM structure with header bit)
        uint32_t m_generation;        // eqref creation-time counter (when SpecialFlag + Array/Packed/String/Dict/Set)
        uint32_t m_source_line;       // SourceLoc source line
        vm_offset_t m_curry;          // Curry: offset to 2 Objects [value, callable]
        vm_offset_t m_thunk;          // Thunk: offset to 3 Objects [state, proc, result]
        vm_offset_t m_tagged;         // Tagged: offset to 2 Objects [tag-name, payload]
        vm_offset_t m_record;         // Record: offset to [schema_offset, Object[0..N-1]]
        vm_offset_t m_coroutine;      // Coroutine: offset to CoroutineContext in VM
        vm_offset_t m_pipe_buffer;    // PipeBuffer: offset to PipeBufferHeader in VM
        vm_offset_t m_cell;           // Cell: offset to CellHeader in VM
        vm_offset_t m_continuation;   // Continuation: offset to ContinuationContext in VM
        vm_offset_t m_int128;         // Int128: WideValue offset to 128-bit signed value
        vm_offset_t m_uint128;        // UInt128: WideValue offset to 128-bit unsigned value
        vm_offset_t m_handle;         // OpaqueHandle: offset to per-kind state struct (e.g. ScreenState)
        vm_offset_t m_offset;         // generic VM offset
    };  // VALUE 4 bytes
};  // OBJECT 8 bytes

static_assert(sizeof(Object) == 8);

// Root object accessors (heap-allocated Object array, snapshot/thaw'd with the blob).
// Indexed by the RootObject enum; storage is shared by eq temporaries, stdio name
// strings, and the pending-error-data slot (see src/enums.inl).
[[nodiscard]] Object root_object(RootObject idx) const {
    return m_root_objects_ptr[+idx];
}
void set_root_object(RootObject idx, Object val_obj) {
    m_root_objects_ptr[+idx] = val_obj;
}

//
// gc_walk_object_array(payload_offset, payload_size, work,
//                      work_count, marked_count):
// GC mark-phase walker for blocks whose payload is a flat Object[].
//
// Reads `m_obj_count` from the block header at
// `payload_offset - GvmHeaderSize` and walks exactly that many
// Object slots, calling gc_mark_object on each.  When m_obj_count is
// 0 (older alloc sites that haven't been updated to stamp it,
// snapshot-v162 blobs, kinds whose alloc-time count isn't tracked
// in the header), falls back to payload_size / sizeof(Object) --
// gc_mark_object's defensive checks cover any slack noise.
//
// Caller contract: payload_offset MUST be the block's payload
// start (i.e., what gvm_alloc returned).  Sub-payload offsets like
// `payload_offset + 8` would misalign the header lookup and read
// payload bytes as GvmBlock fields -- callers needing to walk a
// suffix of the payload must do their own loop.  The Record case
// in walk_block_contents is the one such caller and inlines its
// own loop (see gc.inl).
//
void gc_walk_object_array(vm_offset_t payload_offset, vm_size_t payload_size) {
    auto count = static_cast<vm_size_t>(gvm_get_obj_count(payload_offset));
    if (count == 0) {
        count = static_cast<vm_size_t>(payload_size / sizeof(Object));
    }
    auto *objects = offset_to_ptr<Object>(payload_offset);
    for (vm_size_t i = 0; i < count; ++i) {
        gc_mark_object(objects[i]);
    }
}

//===--- Object-Array and Packed-Temp Helpers ---===//
//
// Trix-level helpers for manipulating Object[] spines and Packed temporaries.
// They live here, not in the ops_*.inl files that call them, because their
// domain is Object/Packed storage rather than any single operator family.

// Applies string_interval, array_interval, or packed_interval to a
// container Object based on its type.
static void container_interval(Trix *trx, Object *container, length_t index, length_t count) {
    if (container->is_string()) {
        container->string_interval(trx, index, count);
    } else if (container->is_array()) {
        container->array_interval(trx, index, count);
    } else {
        container->packed_interval(trx, index, count);
    }
}

// Clone elements from src (Array or Packed) into dst_ptr.
// dst_ptr must be a fresh/uninitialized buffer.  Uses Stack mode for packed
// extraction (make_clone only) rather than Array mode -- Array mode reads the
// existing dst Objects to decide whether to save_object or free, which crashes
// on uninitialized memory containing garbage save_levels.  Stack mode is
// consistent with the array path (make_clone, no set_save_level).
static void clone_array_elements(Trix *trx, Object *src, Object *dst_ptr, length_t count) {
    if (src->is_array()) {
        auto src_ptr = src->array_objects(trx);
        for (length_t i = 0; i < count; ++i) {
            dst_ptr[i] = src_ptr[i].make_clone(trx);
        }
    } else {
        auto packed_data = src->const_packed_span(trx);
        Object::extract_packed(trx, packed_data.data(), dst_ptr, count, Object::ExtractPackedDestination::Stack);
    }
}

// Expand a packed array to a temp buffer for indexed access.
// If the source is packed, clones elements into a vm_temp_alloc buffer.
// If the source is a regular array, returns its element pointer directly.
// Returns {element_ptr, temp_buf_or_null}.  Caller must call release_packed_temp()
// when done to free any cloned ExtValues and restore the temp pointer.
struct ExpandedArray {
    Object *ptr{nullptr};
    Object *buf{nullptr};  // non-null if packed expansion was needed
};

[[nodiscard]] static ExpandedArray expand_packed_to_temp(Trix *trx, Object *obj, length_t count) {
    if (obj->is_packed()) {
        auto buf = trx->vm_temp_alloc<Object>(count);
        // The temp buffer sits directly below m_vm_global; a GLOBAL allocation
        // grows the global region DOWN over it (gvm_alloc bounds-checks only the
        // local heap top, never the temp region).  Under ${...} both the packed
        // decode (which materialises a Long/Double ExtValue) and the make_clone
        // would allocate globally and clobber the very buffer being filled.  Force
        // the expansion LOCAL: these are scratch clones released by
        // release_packed_temp, so local is correct and matches the top-level path.
        // (Restored on the error-unwind path by the enclosing ${...} barrier.)
        auto saved_global = trx->m_curr_alloc_global;
        trx->m_curr_alloc_global = false;
        clone_array_elements(trx, obj, buf, count);
        trx->m_curr_alloc_global = saved_global;
        return {buf, buf};
    } else {
        return {obj->array_objects(trx), nullptr};
    }
}

// Free a temporary packed expansion created by expand_packed_to_temp().
static void release_packed_temp(Trix *trx, Object *buf, length_t count) {
    if (buf != nullptr) {
        for (length_t i = 0; i < count; ++i) {
            buf[i].maybe_free_extvalue(trx);
        }
    }
}

// Recursively deep-clone a freshly-built LOCAL result array tree into the GLOBAL
// region.  The set-algebra / packed-expansion array ops (union/intersect/difference,
// zip/zip-longest/interpose/enumerate/chunk/sliding-window) build their result
// (vm_alloc_n, local heap) and any membership/seen dict (create_temp_dict, temp
// region) LOCAL, because a global allocation grows the global region DOWN over the
// live temp region and would clobber it.  Once the temp is released it is safe to
// allocate globally, but a plain make_clone of the result is SHALLOW for arrays --
// it would leave nested sub-arrays (zip pairs, chunks) LOCAL, a global->local
// dangling pointer.  This descends every array level, rebuilding it global; scalars,
// strings, etc. take the region-aware make_clone.  Each level is fully null-init'd
// before any further alloc and rooted across its fill (Cat-2 GC rules 1+2;
// require_gc_root_capacity_more composes with the caller's / outer levels' roots).
// MUST be called only when m_curr_alloc_global is set and AFTER vm_temp_restore:
// the local source lives in the local heap (not the temp region, never GC-swept),
// so it stays valid across the global clones' GC.  Result sub-arrays are always
// REGULAR arrays (vm_alloc_n), never packed, so only is_array recurses.
[[nodiscard]] static Object deep_promote_to_global(Trix *trx, Object src) {
    if (src.is_array()) {
        auto slen = src.arrays_length();
        auto [gdst, goff] = trx->vm_alloc_dispatch_n<Object>(slen, Trix::ChunkKind::Array);
        std::fill_n(gdst, slen, Object::make_null());
        trx->require_gc_root_capacity_more(1);
        *++trx->m_gc_roots_ptr = Object::make_array(goff, slen);
        for (length_t i = 0; i < slen; ++i) {
            auto clone = deep_promote_to_global(trx, src.array_objects(trx)[i]);
            trx->offset_to_ptr<Object>(goff)[i] = clone;
        }
        trx->gc_root_pop_n(1);
        return Object::make_array(goff, slen);
    } else if (src.is_set()) {
        auto ssrc = src.set_value(trx);
        auto mode = ssrc->is_dynamic()         ? Object::DictMode::ReadWriteDynamic
                    : ssrc->has_write_access() ? Object::DictMode::ReadWriteFixed
                                               : Object::DictMode::ReadOnly;
        auto [gset, goff] = Dict::create_global_set(trx, ssrc->maxlength(), mode);
        trx->require_gc_root_capacity_more(1);
        *++trx->m_gc_roots_ptr = Object::make_set(goff);
        auto entry_offset = nulloffset;
        auto bucket_idx = integer_t{-1};
        while (true) {
            auto [next, idx, key] = src.set_value(trx)->set_next(trx, entry_offset, bucket_idx);
            if (next == nulloffset) {
                break;
            } else {
                auto kc = deep_promote_to_global(trx, key);  // recursion roots its own dst
                trx->offset_to_ptr<Dict>(goff)->set_put(trx, kc);
                entry_offset = next;
                bucket_idx = idx;
            }
        }
        trx->gc_root_pop_n(1);
        return Object::make_set(goff);
    } else if (src.is_dict()) {
        auto dsrc = src.dict_value(trx);
        auto mode = dsrc->is_dynamic()         ? Object::DictMode::ReadWriteDynamic
                    : dsrc->has_write_access() ? Object::DictMode::ReadWriteFixed
                                               : Object::DictMode::ReadOnly;
        auto [gd, goff] = Dict::create_global_dict(trx, dsrc->maxlength(), mode);
        trx->require_gc_root_capacity_more(2);
        *++trx->m_gc_roots_ptr = Object::make_dict(goff);
        auto entry_offset = nulloffset;
        auto bucket_idx = integer_t{-1};
        while (true) {
            auto [next, idx, key, value] = src.dict_value(trx)->next(trx, entry_offset, bucket_idx);
            if (next == nulloffset) {
                break;
            } else {
                auto kc = deep_promote_to_global(trx, key);
                *++trx->m_gc_roots_ptr = kc;  // root key clone across the value's deep promote
                auto vc = deep_promote_to_global(trx, value);
                trx->offset_to_ptr<Dict>(goff)->put(trx, kc, vc);
                trx->gc_root_pop_n(1);
                entry_offset = next;
                bucket_idx = idx;
            }
        }
        trx->gc_root_pop_n(1);
        return Object::make_dict(goff);
    } else {
        return src.make_clone(trx);
    }
}

// Promote a LOCAL result array to the global region under ${...}, else return it
// unchanged.  Thin region guard over deep_promote_to_global; the array ops above
// call this on their result Object at the tail (after the temp region is restored)
// so the result survives save/restore instead of being R6-rejected as a
// restore-fragile local stored into a global container.
[[nodiscard]] static Object promote_result_if_global(Trix *trx, Object result) {
    return (trx->m_curr_alloc_global ? deep_promote_to_global(trx, result) : result);
}

// copy_array: copy elements from src into dst, cloning ExtValues and honoring save/restore.
// Contrast with bind_array, which RESOLVES executable name elements to their bound operator
// values in-place within a single array -- it does not copy between two arrays.
static void copy_array(Trix *trx, const Object *src, Object *dst) {
    auto src_length = src->arrays_length();
    auto src_is_array = src->is_array();
    auto [dst_ptr, dst_length] = dst->array_value(trx);
    if (dst_length >= src_length) {
        auto dst_is_eqarray = dst->is_eqarray(trx);
        if (src_is_array) {
            auto src_ptr = src->array_objects(trx);
            if (dst_ptr != src_ptr) {
                auto curr_save_level = trx->m_curr_save_level;
                for (auto remaining = src_length; remaining != 0; --remaining) {
                    if ((dst_ptr->save_level() == curr_save_level) || dst_is_eqarray) {
                        dst_ptr->maybe_free_extvalue(trx);
                    } else {
                        Save::save_object(trx, dst_ptr);
                    }
                    *dst_ptr = src_ptr->make_clone(trx);
                    dst_ptr->set_save_level(curr_save_level);

                    ++src_ptr;
                    ++dst_ptr;
                }
            }
        } else {
            auto packed_data = src->const_packed_span(trx);
            auto destination = dst_is_eqarray ? Object::ExtractPackedDestination::EqArray : Object::ExtractPackedDestination::Array;
            Object::extract_packed(trx, packed_data.data(), dst_ptr, src_length, destination);
        }
        dst->set_array_length(src_length);
    } else {
        auto desc = src_is_array ? "array" : "packed";
        trx->error(
                Error::RangeCheck, "destination 'array' length {} less than source '{}' length {}", dst_length, desc, src_length);
    }
}

// Free any ExtValues on the operand stack from m_op_ptr down to base_ptr
// (inclusive), wrapping the per-Object maybe_free_extvalue above.
static void maybe_free_extvalue_opstack(Trix *trx, Object *base_ptr) {
    for (auto curr_ptr = trx->m_op_ptr; curr_ptr >= base_ptr; --curr_ptr) {
        curr_ptr->maybe_free_extvalue(trx);
    }
}

// Free any ExtValues on the execution stack from m_exec_ptr down to base_ptr.
static void maybe_free_extvalue_execstack(Trix *trx, Object *base_ptr) {
    for (auto curr_ptr = trx->m_exec_ptr; curr_ptr >= base_ptr; --curr_ptr) {
        curr_ptr->maybe_free_extvalue(trx);
    }
}
