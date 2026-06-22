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

//===--- Operand Verification (verify_t) ---===//
//
// Implements compile-time-composable operand type checking for operators.
// Every operator that requires specific operand types calls verify_operands()
// with a list of verify_t descriptors, one per operand (top of stack first).
// Based on:
//
//   PostScript type checking: operators verify operand types before execution
//   and raise typecheck errors for invalid inputs.  In PostScript this is
//   ad-hoc (each operator checks individually); Trix systematizes it with
//   a bitmask-based verification system.
//
// --- Core concepts for maintainers ---
//
// VERIFY_T ENCODING
//   A verify_t is a uint64_t that packs type acceptance and constraint
//   flags into a single word:
//     Bits 0..30:  one bit per Object::Type (31 types defined).
//       A set bit means that type is acceptable.  Multiple bits can be
//       set to accept multiple types (e.g., VerifyString | VerifyArray
//       accepts either strings or arrays).
//     Bits 32..45: constraint flags applied after the type check.
//       VerifyRW           -- must be ReadWrite access
//       VerifyExe          -- must be Executable attribute
//       VerifyNotZero      -- numeric value must not be zero
//       VerifyNotNegative  -- numeric value must not be negative
//       VerifyIsFinite     -- float must not be infinite or NaN
//       etc.
//
// USAGE PATTERN
//   Operators declare their requirements as a verify_operands() call:
//     trx->verify_operands(VerifyString, VerifyInteger);
//   This checks that the top operand is a String and the second is an
//   Integer (stack order: the first formal is matched against the top of
//   stack, which is checked first).  On mismatch, a TypeCheck
//   error is raised with a descriptive message naming the expected and
//   actual types.
//
// PREDEFINED CONSTANTS
//   Common patterns are predefined for readability:
//     VerifyAny        -- any type (all bits set)
//     VerifyNumbers    -- any numeric type
//     VerifyString     -- String type only
//     VerifyArray      -- Array type only
//     VerifyProc       -- (Array | Packed) with the Executable attribute (VerifyExe)
//     VerifyRWString   -- String + ReadWrite constraint
//     VerifyRWStream   -- Stream + ReadWrite constraint
//     VerifyMark       -- Mark type (for mark-based variadic args)
//     etc.
//
// COMPOSABILITY
//   verify_t values are composable via bitwise OR:
//     VerifyString | VerifyArray  -- accepts String or Array
//     VerifyInteger | VerifyNotZero  -- Integer that is not zero
//   This gives operators fine-grained control over acceptable inputs
//   without per-operator boilerplate.
//
// verify_t packs type acceptance and constraint flags into a single 64-bit word:
//   Bits  0..30: one bit per Object::Type (Null=bit 0 .. OpaqueHandle=bit 30).
//                A set bit means that type is acceptable for this operand.
//                31 of 32 lower bits used; 1 remains for future types.
//   Bits 32..45: constraint flags (VerifyRW, VerifyExe, VerifyNotZero, etc.)
//                applied after the type check.
//                14 of 32 upper bits used by constraint flags; bits 46..53
//                (8 slots) are reserved for OpaqueHandle subkind discrimination
//                (see VerifyHandleKindShift); 10 bits (54..63) remain free.
// verify_operands() tests the type bit for each actual operand against the
// type-acceptance mask and checks the constraint flags in a single pass.
using verify_t = uint64_t;

[[nodiscard]] static constexpr verify_t TypeToVerify(Object::Type type) {
    return (1ull << +type);
}

static constexpr int VerifyFlagsShift{32};
static_assert(Object::TypeCount <= VerifyFlagsShift);
static constexpr verify_t VerifyTypesMask{(1ull << Object::TypeCount) - 1};

static constexpr verify_t VerifyRW{0x001ull << VerifyFlagsShift};
static constexpr verify_t VerifyExe{0x002ull << VerifyFlagsShift};
static constexpr verify_t VerifyNotZero{0x004ull << VerifyFlagsShift};             // Number != 0
static constexpr verify_t VerifyNotNegative{0x008ull << VerifyFlagsShift};         // Number >= 0
static constexpr verify_t VerifyNotInf{0x010ull << VerifyFlagsShift};              // Real/Double != infinite
static constexpr verify_t VerifyNotNan{0x020ull << VerifyFlagsShift};              // Real/Double !=  NaN
static constexpr verify_t VerifyIsAbsLessEqOne{0x040ull << VerifyFlagsShift};      // |Number| <= 1
static constexpr verify_t VerifyIsNormal{0x080ull << VerifyFlagsShift};            // Real/Double not subnormal, zero, infinite, NaN
static constexpr verify_t VerifyIsFinite{0x100ull << VerifyFlagsShift};            // Real/Double not infinite, NaN
static constexpr verify_t VerifyLazy{0x200ull << VerifyFlagsShift};                // array length 2, data[1] is a thunk
static constexpr verify_t VerifyGreaterEqualOne{0x400ull << VerifyFlagsShift};     // Number >= 1
static constexpr verify_t VerifyIsAbsLessOne{0x800ull << VerifyFlagsShift};        // |Number| < 1
static constexpr verify_t VerifyGreaterThanNegOne{0x1000ull << VerifyFlagsShift};  // Number > -1
static constexpr verify_t VerifyNotNonPositiveInteger{0x2000ull << VerifyFlagsShift};  // Number not 0 or a negative integer

// Bits 46..53 (8 slots) discriminate OpaqueHandle subkinds.  When any such
// bit is set in `formal` and the actual operand is an OpaqueHandle, the
// operand's handle_kind() must match one of the requested kind slots.
// VerifyOpaqueHandle alone (no kind bits) still accepts any kind.
static constexpr int VerifyHandleKindShift{VerifyFlagsShift + 14};
static_assert(VerifyHandleKindShift + Object::HandleKindCount <= 64);
static constexpr verify_t VerifyHandleScreen{1ull << (VerifyHandleKindShift + +Object::HandleKind::Screen)};
static constexpr verify_t VerifyHandleKindMask{VerifyHandleScreen /* | future kinds: VerifyHandleTilemap, ... */};

static constexpr verify_t VerifyAttrsNone{0};  // no additional verification
static constexpr verify_t VerifyAttrs{VerifyNotZero | VerifyNotNegative | VerifyNotInf | VerifyNotNan | VerifyIsAbsLessEqOne |
                                      VerifyGreaterEqualOne | VerifyIsAbsLessOne | VerifyGreaterThanNegOne |
                                      VerifyNotNonPositiveInteger | VerifyIsNormal | VerifyIsFinite};

static constexpr verify_t VerifyFloatConstraints{VerifyNotInf | VerifyNotNan | VerifyIsAbsLessEqOne | VerifyGreaterEqualOne |
                                                 VerifyIsAbsLessOne | VerifyGreaterThanNegOne | VerifyNotNonPositiveInteger |
                                                 VerifyIsNormal | VerifyIsFinite};

static constexpr verify_t VerifyNull{1ull << +Object::Type::Null};
static constexpr verify_t VerifyByte{1ull << +Object::Type::Byte};
static constexpr verify_t VerifyInteger{1ull << +Object::Type::Integer};
static constexpr verify_t VerifyUInteger{1ull << +Object::Type::UInteger};
static constexpr verify_t VerifyLong{1ull << +Object::Type::Long};
static constexpr verify_t VerifyULong{1ull << +Object::Type::ULong};
static constexpr verify_t VerifyAddress{1ull << +Object::Type::Address};
static constexpr verify_t VerifyReal{1ull << +Object::Type::Real};
static constexpr verify_t VerifyDouble{1ull << +Object::Type::Double};
static constexpr verify_t VerifyBoolean{1ull << +Object::Type::Boolean};
static constexpr verify_t VerifyOperator{1ull << +Object::Type::Operator};
static constexpr verify_t VerifyMark{1ull << +Object::Type::Mark};
static constexpr verify_t VerifyName{1ull << +Object::Type::Name};
static constexpr verify_t VerifyArray{1ull << +Object::Type::Array};
static constexpr verify_t VerifyPacked{1ull << +Object::Type::Packed};
static constexpr verify_t VerifyString{1ull << +Object::Type::String};
static constexpr verify_t VerifyStream{1ull << +Object::Type::Stream};
static constexpr verify_t VerifyDict{1ull << +Object::Type::Dict};
static constexpr verify_t VerifySourceLoc{1ull << +Object::Type::SourceLoc};  // internal exec stack use only
static constexpr verify_t VerifyCurry{1ull << +Object::Type::Curry};
static constexpr verify_t VerifyThunk{1ull << +Object::Type::Thunk};
static constexpr verify_t VerifySet{1ull << +Object::Type::Set};
static constexpr verify_t VerifyTagged{1ull << +Object::Type::Tagged};
static constexpr verify_t VerifyRecord{1ull << +Object::Type::Record};
static constexpr verify_t VerifyCoroutine{1ull << +Object::Type::Coroutine};
static constexpr verify_t VerifyPipeBuffer{1ull << +Object::Type::PipeBuffer};
static constexpr verify_t VerifyCell{1ull << +Object::Type::Cell};
static constexpr verify_t VerifyContinuation{1ull << +Object::Type::Continuation};
static constexpr verify_t VerifyInt128{1ull << +Object::Type::Int128};
static constexpr verify_t VerifyUInt128{1ull << +Object::Type::UInt128};
static constexpr verify_t VerifyOpaqueHandle{1ull << +Object::Type::OpaqueHandle};

// Convenience composites for specific opaque-handle kinds.  An operator
// that takes a Screen handle should declare VerifyScreen rather than
// VerifyOpaqueHandle so the verify pass rejects wrong-kind handles up
// front instead of leaving the kind probe to the operator body.
static constexpr verify_t VerifyScreen{VerifyOpaqueHandle | VerifyHandleScreen};

static constexpr verify_t VerifyRWArray{VerifyArray | VerifyRW};
static constexpr verify_t VerifyRWString{VerifyString | VerifyRW};
static constexpr verify_t VerifyRWStream{VerifyStream | VerifyRW};
static constexpr verify_t VerifyRWDict{VerifyDict | VerifyRW};
static constexpr verify_t VerifyRWSet{VerifySet | VerifyRW};

static constexpr verify_t VerifySignedIntegers{VerifyInteger | VerifyLong | VerifyInt128};
static constexpr verify_t VerifyUnsignedIntegers{VerifyByte | VerifyUInteger | VerifyULong | VerifyUInt128};
static constexpr verify_t VerifyIntegers{VerifySignedIntegers | VerifyUnsignedIntegers};
// Integer types that fit in 32 bits.  Use this for operators consuming
// counts/indices bounded by uinteger range -- coords, dimensions,
// codepoints, palette indices.  Avoids accepting Long/ULong (which can
// be ExtValues for values > UInteger range and add heap-allocation
// pressure on hot paths).
static constexpr verify_t VerifyIntegers32{VerifyByte | VerifyInteger | VerifyUInteger};

// Integer types that fit in 64 bits.  Use this for operators that consume
// integer counts/indices bounded by ulong range and don't support 128-bit
// iteration (e.g. repeat, for -- matches for_op's explicit Int128/UInt128
// rejection).  Operators that need full 128-bit reach should use VerifyIntegers.
static constexpr verify_t VerifyIntegers64{VerifyIntegers32 | VerifyLong | VerifyULong};

static constexpr verify_t VerifyFloats{VerifyReal | VerifyDouble};

static constexpr verify_t VerifyNumbers{VerifyIntegers | VerifyFloats};

static constexpr verify_t VerifyArrays{VerifyArray | VerifyPacked};
static constexpr verify_t VerifyProc{VerifyArrays | VerifyExe};
static constexpr verify_t VerifyCallable{VerifyProc | VerifyNull | VerifyName | VerifyOperator | VerifyCurry | VerifyThunk |
                                         VerifyContinuation};

static constexpr verify_t VerifyLazySeq{VerifyNull | VerifyArray | VerifyLazy};
static constexpr verify_t VerifyLazySeqNonempty{VerifyArray | VerifyLazy};

static constexpr verify_t VerifyTransducer{VerifyArray | VerifyTagged};

static constexpr verify_t VerifyIndexable{VerifyString | VerifyArray | VerifyPacked};

static constexpr verify_t VerifyKey{VerifyByte | VerifyInteger | VerifyUInteger | VerifyLong | VerifyULong | VerifyAddress |
                                    VerifyReal | VerifyDouble | VerifyBoolean | VerifyOperator | VerifyMark | VerifyName |
                                    VerifyArray | VerifyPacked | VerifyString | VerifyStream | VerifyDict | VerifyCurry |
                                    VerifyThunk | VerifySet | VerifyTagged | VerifyRecord | VerifyCoroutine | VerifyPipeBuffer |
                                    VerifyCell | VerifyContinuation | VerifyInt128 | VerifyUInt128 | VerifyOpaqueHandle};

static constexpr verify_t VerifyAny{
        VerifyNull | VerifyByte | VerifyInteger | VerifyUInteger | VerifyLong | VerifyULong | VerifyAddress | VerifyReal |
        VerifyDouble | VerifyBoolean | VerifyOperator | VerifyMark | VerifyName | VerifyArray | VerifyPacked | VerifyString |
        VerifyStream | VerifyDict | VerifyCurry | VerifyThunk | VerifySet | VerifyTagged | VerifyRecord | VerifyCoroutine |
        VerifyPipeBuffer | VerifyCell | VerifyContinuation | VerifyInt128 | VerifyUInt128 | VerifyOpaqueHandle};

static constexpr verify_t VerifyLiteral{VerifyAny & ~(VerifyOperator | VerifyCurry | VerifyAddress)};
static constexpr verify_t VerifyExecutable{VerifyAny &
                                           ~(VerifyTagged | VerifyRecord | VerifyCoroutine | VerifyAddress | VerifyOpaqueHandle)};

static constexpr verify_t VerifyHasAccess{VerifyStream | VerifyArray | VerifyPacked | VerifyString | VerifyDict | VerifySet};

static constexpr verify_t VerifyHasLength{VerifyName | VerifyArray | VerifyPacked | VerifyString | VerifyDict | VerifySet |
                                          VerifyRecord};

// Buffer sizes for verify_description() output.
// Formal (all types): "RW|X|Null|Byte|...|Cell|Continuation" ~ 205 chars + NUL + margin.
// Single type: longest name "UnsignedIntegers" with "RW|X|" prefix <= 24 chars; 32 gives headroom.
static constexpr size_t VERIFY_FORMAL_BUFFER_SIZE{210};
static constexpr size_t VERIFY_TYPE_BUFFER_SIZE{32};

// Controls whether all operands passed to verify_operands() must share the same type.
enum class MatchPolicy { AnyType, AllMustMatch };

[[nodiscard]] const char *verify_description(char *buffer, size_t size, verify_t x) {
    if (x == VerifyAny) {
        return "Any";
    } else if (x == VerifyKey) {
        return "Key";
    } else if (x == VerifyProc) {
        return "Proc";
    } else if (x == VerifyArrays) {
        return "Arrays";
    } else if (x == VerifyIndexable) {
        return "Indexable";
    } else if (x == VerifyNumbers) {
        return "Numbers";
    } else if (x == VerifyFloats) {
        return "Floats";
    } else if (x == VerifyIntegers) {
        return "Integers";
    } else if (x == VerifyIntegers64) {
        return "Integers64";
    } else if (x == VerifyUnsignedIntegers) {
        return "UnsignedIntegers";
    } else if (x == VerifySignedIntegers) {
        return "SignedIntegers";
    } else if (x == VerifyLazySeq) {
        return "LazySeq";
    } else if (x == VerifyLazySeqNonempty) {
        return "LazySeqNonempty";
    } else if (x == VerifyTransducer) {
        return "Transducer";
    } else if ((buffer != nullptr) && (size > 0)) {
        using namespace std::literals::string_view_literals;

        // "RW|X|Null|Byte|Integer|UInteger|Long|ULong|Address|Real|Double|" 62 chars
        // "Boolean|Operator|Mark|Name|Array|Packed|String|Stream|Dict|..." through OpaqueHandle
        static constexpr struct {
            verify_t m_bit;
            std::string_view m_sv;
        } verify_sv[] = {
                {          VerifyRW,           "RW|"sv},
                {         VerifyExe,            "X|"sv},
                {        VerifyNull,         "Null|"sv},
                {        VerifyByte,         "Byte|"sv},
                {     VerifyInteger,      "Integer|"sv},
                {    VerifyUInteger,     "UInteger|"sv},
                {        VerifyLong,         "Long|"sv},
                {       VerifyULong,        "ULong|"sv},
                {     VerifyAddress,      "Address|"sv},
                {        VerifyReal,         "Real|"sv},
                {      VerifyDouble,       "Double|"sv},
                {     VerifyBoolean,      "Boolean|"sv},
                {    VerifyOperator,     "Operator|"sv},
                {        VerifyMark,         "Mark|"sv},
                {        VerifyName,         "Name|"sv},
                {       VerifyArray,        "Array|"sv},
                {      VerifyPacked,       "Packed|"sv},
                {      VerifyString,       "String|"sv},
                {      VerifyStream,       "Stream|"sv},
                {        VerifyDict,         "Dict|"sv},
                {   VerifySourceLoc,    "SourceLoc|"sv},
                {       VerifyCurry,        "Curry|"sv},
                {       VerifyThunk,        "Thunk|"sv},
                {         VerifySet,          "Set|"sv},
                {      VerifyTagged,       "Tagged|"sv},
                {      VerifyRecord,       "Record|"sv},
                {   VerifyCoroutine,    "Coroutine|"sv},
                {  VerifyPipeBuffer,   "PipeBuffer|"sv},
                {        VerifyCell,         "Cell|"sv},
                {VerifyContinuation, "Continuation|"sv},
                {      VerifyInt128,       "Int128|"sv},
                {     VerifyUInt128,      "UInt128|"sv},
                {VerifyOpaqueHandle, "OpaqueHandle|"sv}
        };

        auto ptr = buffer;
        for (size_t i = 0; i < std::size(verify_sv); ++i) {
            auto v = &verify_sv[i];
            if ((x & v->m_bit) != 0) {
                auto length = v->m_sv.length();
                if (size > length) {
                    std::copy_n(v->m_sv.data(), length, ptr);
                    ptr += length;
                    size -= length;
                } else {
                    break;
                }
            }
        }
        if (ptr != buffer) {
            ptr[-1] = '\0';
            return buffer;
        }
    }

    return "";
}

// Check floating-point range constraints for a single operand.
// Called only when (formal & VerifyFloatConstraints) != 0 and actual is a floating-point Object.
void check_float_range(verify_t formal, const Object *actual_ptr, int param_num) {
    auto trx = this;
    if (((formal & VerifyNotInf) != 0) && actual_ptr->is_floating_point_inf(trx)) {
        error(Error::NumericalINF, "operand #{} is INF", param_num);
    } else if (((formal & VerifyNotNan) != 0) && actual_ptr->is_floating_point_nan(trx)) {
        error(Error::NumericalNaN, "operand #{} is NaN", param_num);
    } else if (((formal & VerifyIsAbsLessEqOne) != 0) && !actual_ptr->is_floating_point_is_abs_lesseq_one(trx)) {
        error(Error::RangeCheck, "|operand #{}| > 1", param_num);
    } else if (((formal & VerifyGreaterEqualOne) != 0) && !actual_ptr->is_floating_point_greater_eq_one(trx)) {
        error(Error::RangeCheck, "operand #{} is < 1", param_num);
    } else if (((formal & VerifyIsAbsLessOne) != 0) && !actual_ptr->is_floating_point_is_abs_less_one(trx)) {
        error(Error::RangeCheck, "|operand #{}| >= 1", param_num);
    } else if (((formal & VerifyGreaterThanNegOne) != 0) && !actual_ptr->is_floating_point_greater_than_neg_one(trx)) {
        error(Error::RangeCheck, "operand #{} is <= -1", param_num);
    } else if (((formal & VerifyNotNonPositiveInteger) != 0) && actual_ptr->is_floating_point_nonpositive_integer(trx)) {
        error(Error::RangeCheck, "operand #{} is a non-positive integer (pole)", param_num);
    } else if (((formal & VerifyIsNormal) != 0) && !actual_ptr->is_floating_point_normal(trx)) {
        error(Error::NumericalINF, "operand #{} is not normal", param_num);
    } else if (((formal & VerifyIsFinite) != 0) && !actual_ptr->is_floating_point_finite(trx)) {
        error(Error::NumericalINF, "operand #{} is not finite", param_num);
    }
}

// Verify that actual operand's type matches the formal type mask and executable attribute.
void verify_type_check(verify_t formal, int actual_type, bool actual_is_literal, int param_num) {
    auto formal_is_executable = ((formal & VerifyExe) != 0);
    auto lit_exe_mismatch = (formal_is_executable && actual_is_literal) && ((1ull << actual_type) != VerifyCurry);

    // (1ull << actual_type) is always a single bit in [0, TypeCount) which is entirely
    // within VerifyTypesMask, so the extra VerifyTypesMask AND is redundant.
    if (((formal & (1ull << actual_type)) == 0) || lit_exe_mismatch) {
        if ((formal == VerifyKey) && ((1ull << actual_type) == VerifyNull)) {
            error(Error::TypeCheck, "operand #{}: null is not a valid dict key", param_num);
        } else if ((formal == VerifyLazySeqNonempty) && ((1ull << actual_type) == VerifyNull)) {
            error(Error::RangeCheck, "lazy-seq is empty");
        } else {
            char formal_buf[VERIFY_FORMAL_BUFFER_SIZE];
            auto formal_desc = verify_description(formal_buf, sizeof(formal_buf), formal);

            char actual_buf[VERIFY_TYPE_BUFFER_SIZE];
            auto actual_attr = (formal_is_executable ? (actual_is_literal ? "L|" : "X|") : "");
            auto actual_desc = verify_description(actual_buf, sizeof(actual_buf), (1ull << actual_type));
            error(Error::TypeCheck,
                  "operand #{} type mismatch: expected {}, actual {}{}",
                  param_num,
                  formal_desc,
                  actual_attr,
                  actual_desc);
        }
    }
}

// Verify that an OpaqueHandle operand has the requested kind.  No-op unless
// formal carries a VerifyHandle<Kind> bit.  Only inspected when the type
// check passed and the actual operand is an OpaqueHandle.
void verify_handle_kind(verify_t formal, const Object *actual_ptr, int actual_type, int param_num) {
    auto kind_constraints = (formal & VerifyHandleKindMask);
    if ((kind_constraints != 0) && (actual_type == +Object::Type::OpaqueHandle)) {
        auto kind = +actual_ptr->handle_kind();
        auto kind_bit = (1ull << (VerifyHandleKindShift + kind));
        if ((kind_constraints & kind_bit) == 0) {
            error(Error::TypeCheck, "operand #{}: opaque-handle kind mismatch", param_num);
        }
    }
}

// Verify that the actual operand has ReadWrite access when formal requires it.
void verify_rw_access(verify_t formal, const Object *actual_ptr, int actual_type, int param_num) {
    auto trx = this;
    if (((formal & VerifyRW) != 0) && (actual_ptr->has_object_access() || actual_ptr->is_dict() || actual_ptr->is_set())) {
        auto actual_access = actual_ptr->has_object_access() ? actual_ptr->access()
                             : actual_ptr->is_dict()         ? actual_ptr->dict_value(trx)->access()
                                                             : actual_ptr->set_value(trx)->access();
        if (actual_access == Object::ReadOnlyAccess) {
            char actual_buf[VERIFY_TYPE_BUFFER_SIZE];
            auto actual_desc = verify_description(actual_buf, sizeof(actual_buf), (1ull << actual_type));
            error(Error::ReadOnly, "operand #{} is ReadOnly {} while ReadWrite access is required", param_num, actual_desc);
        }
    }
}

// Verify that all operands share the same type (MatchPolicy::AllMustMatch).
void verify_type_match(int first_type, int actual_type, int param_num) {
    if (actual_type != first_type) {
        char firstbuf[VERIFY_TYPE_BUFFER_SIZE];
        auto firststr = verify_description(firstbuf, sizeof(firstbuf), (1ull << first_type));

        char actualbuf[VERIFY_TYPE_BUFFER_SIZE];
        auto actualstr = verify_description(actualbuf, sizeof(actualbuf), (1ull << actual_type));

        error(Error::TypeCheck, "operand #1:{} and Operand #{}:{} types mismatch", firststr, param_num, actualstr);
    }
}

// Verify numeric constraints (NotZero, NotNegative) and key/float range constraints.
void verify_value_constraints(verify_t formal, const Object *actual_ptr, int param_num) {
    auto trx = this;
    if (((formal & VerifyNotZero) != 0) && actual_ptr->is_number() && actual_ptr->is_number_zero(trx)) {
        error(Error::RangeCheck, "operand #{} is zero", param_num);
    } else if (((formal & VerifyNotNegative) != 0) && actual_ptr->is_signed_number() && actual_ptr->is_number_negative(trx)) {
        error(Error::RangeCheck, "operand #{} is less than zero", param_num);
    } else if ((formal == VerifyKey) && actual_ptr->is_floating_point()) {
        if (actual_ptr->is_floating_point_inf(trx)) {
            error(Error::NumericalINF, "operand #{} is INF and cannot be used as a dict key", param_num);
        } else if (actual_ptr->is_floating_point_nan(trx)) {
            error(Error::NumericalNaN, "operand #{} is NaN and cannot be used as a dict key", param_num);
        }
    }

    if (((formal & VerifyFloatConstraints) != 0) && actual_ptr->is_floating_point()) {
        check_float_range(formal, actual_ptr, param_num);
    }

    if (((formal & VerifyLazy) != 0) && actual_ptr->is_array()) {
        if (actual_ptr->arrays_length() != 2) {
            error(Error::TypeCheck, "operand #{} expected lazy-seq (2-element array)", param_num);
        } else if (!actual_ptr->array_objects(trx)[1].is_thunk()) {
            error(Error::TypeCheck, "operand #{} lazy-seq tail must be a thunk", param_num);
        }
    }
}

template<MatchPolicy Policy, typename... Args>
void verify_operands(Object *top_ptr, Args... args) {
    auto actual_ptr = top_ptr;
    auto actual_count = static_cast<int>(actual_ptr - m_op_base + 1);
    auto formal_count = static_cast<int>(sizeof...(Args));
    if (actual_count < formal_count) {
        error(Error::OpStackUnderflow,
              "insufficient number of Objects on Operand Stack: {} required and {} available",
              formal_count,
              actual_count);
    } else {
        const verify_t formal_args[]{args...};
        auto first_type = +actual_ptr->type();
        auto param_num = 1;
        for (auto formal : formal_args) {
            // Single attribute-byte read; extract type and literal bit locally.
            auto aat = actual_ptr->aat_raw();
            auto actual_type = static_cast<int>(aat & Object::TypeMask);
            auto actual_is_literal = ((aat & Object::AttribMask) == Object::LiteralAttrib);

            // Fast path: the vast majority of operators have no constraint bits set
            // (no VerifyRW/VerifyExe/VerifyNotZero/... all live in bits >= VerifyFlagsShift)
            // and are not the VerifyKey INF/NaN-rejection special case.  Under AnyType
            // policy, a single type-bit test subsumes all the helper calls below.
            auto type_matches = ((formal & (1ull << actual_type)) != 0);
            auto has_extra_work = ((formal >> VerifyFlagsShift) != 0) || (formal == VerifyKey);
            auto fast_path = false;
            if constexpr (Policy == MatchPolicy::AnyType) {
                // The common case: type matches and no constraint bits are set, so
                // none of the verify_* helpers below apply.
                fast_path = type_matches && !has_extra_work;
            }

            if (!fast_path) [[unlikely]] {
                verify_type_check(formal, actual_type, actual_is_literal, param_num);
                verify_handle_kind(formal, actual_ptr, actual_type, param_num);
                verify_rw_access(formal, actual_ptr, actual_type, param_num);
                if constexpr (Policy == MatchPolicy::AllMustMatch) {
                    verify_type_match(first_type, actual_type, param_num);
                }
                verify_value_constraints(formal, actual_ptr, param_num);
            }

            ++param_num;
            --actual_ptr;
        }
    }
}

template<typename... Args>
void verify_operands(Args... args) {
    verify_operands<MatchPolicy::AnyType>(m_op_ptr, args...);
}

template<typename... Args>
void verify_matched_operands(Args... args) {
    verify_operands<MatchPolicy::AllMustMatch>(m_op_ptr, args...);
}

template<typename T>
requires std::is_floating_point_v<T>
[[nodiscard]] T verify_floating_point_result(T result) {
    if (std::isfinite(result)) {
        return result;
    } else {
        auto err = std::isnan(result) ? Error::NumericalNaN : Error::NumericalINF;
        error(err, "result is not Finite");
    }
}

[[nodiscard]] std::pair<Object *, length_t> find_opstack_mark(Object *floor_ptr = nullptr) {
    auto base_ptr = (floor_ptr != nullptr) ? floor_ptr : m_op_base;
    for (auto curr_ptr = m_op_ptr; curr_ptr >= base_ptr; --curr_ptr) {
        if (curr_ptr->is_mark()) {
            auto count = static_cast<length_t>(m_op_ptr - curr_ptr);
            return std::pair{curr_ptr, count};
        }
    }
    error(Error::UnmatchedMark, "mark Object not present in Operand Stack");
}

// Result of find_invalid_key_index: index of the first Object that cannot be
// used as a dict key, along with the error code to raise and a short tag for
// the reason (empty if all entries are valid keys).
struct InvalidKeyInfo {
    length_t index;
    Error err;
    std::string_view reason;
};

// Scan [data, data+count) for the first Object that cannot be used as a dict
// key.  The invalid cases are null, NaN, and Inf -- matching VerifyKey and
// Object::validate_dict_keys.  NaN is especially insidious: hash() returns a
// stable value but equal() treats NaN != NaN, so NaN keys can be inserted but
// never retrieved, which silently produces duplicate entries and breaks any
// later get()-based pass (classic symptom: dict->get() returns nullptr for a
// key that was just put, then the caller dereferences and crashes).  Any
// operator that funnels array elements into a dict as keys must pre-validate
// them with this helper.  Caller handles temp-allocation cleanup before
// raising the error.
[[nodiscard]] InvalidKeyInfo find_invalid_key_index(const Object *data, length_t count) {
    auto trx = this;
    for (length_t i = 0; i < count; ++i) {
        if (data[i].is_null()) {
            return {i, Error::TypeCheck, "null"};
        } else if (data[i].is_floating_point()) {
            if (data[i].is_floating_point_nan(trx)) {
                return {i, Error::NumericalNaN, "NaN"};
            } else if (data[i].is_floating_point_inf(trx)) {
                return {i, Error::NumericalINF, "Inf"};
            }
        }
    }
    return {count, Error::NoError, {}};
}
