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

//===--- Shim Dispatch Functions ---===//
//
// Implements the type-dispatch mechanism for binary arithmetic and comparison
// operators.  Each shim is a dispatch function that switches at runtime on the
// operand Object::Type, handling a whole family of same-typed operands; the
// per-operation computation is passed in as a generic lambda.  Binary operators
// require both operands to share a type, so there is no mixed-type combination
// like Integer + Real.
//
// Based on:
//
//   Trix type-safe dispatch: binary operators require both operands to be the
//   same type (verify_matched_operands / MatchPolicy::AllMustMatch).  There is
//   no implicit type promotion -- `1 2.0 add` is a TypeCheck error, not an
//   automatic integer-to-real conversion.  The explicit `promote` operator
//   (ops_convert.inl) is available when the user wants to widen types.
//
//   Lambda-parameterized dispatch: each shim takes the per-operation
//   computation as a generic lambda, then switches at runtime on the operand
//   Object::Type to compute and write the result.  Operator name resolution
//   (dispatch.inl) selects the operator function (e.g., add_op), stored
//   one-per-operator in the SystemName-indexed sysoperator table; there is
//   no 2D type-indexed table of shim pointers.
//
// --- Core concepts for maintainers ---
//
// SHIM NAMING CONVENTION
//   shim_O{InputTypes}__R{ResultType}
//   Example: shim_OUnsignedIntegers__RInteger handles Byte/UInteger/ULong/UInt128
//   inputs producing an Integer result.
//
// DISPATCH FLOW
//   1. The operator function (e.g., add_op) is called.
//   2. It calls the shim function directly, passing a generic lambda that
//      performs the type-agnostic computation.
//   3. The shim runs a runtime switch on the operand Object::Type to select
//      the per-type branch.
//   4. That branch reads the operand(s), invokes the lambda, writes the
//      result, and frees any ExtValue/WideValue backing.
//
// WHY SHIMS (not a virtual-dispatch hierarchy)
//   Each shim is a single dispatch function, not a per-type template;
//   dispatch is an O(1) runtime switch on Object::Type with no virtual
//   call overhead.  Each per-type branch is small (5-15 lines) and fully
//   inlined by the compiler.  The total number of shims is bounded (27
//   shims, each shared across every operator with a matching type
//   signature via the passed lambda).
//
// EXTVALUE / WIDEVALUE HANDLING
//   Shims that consume a heap-backed number -- Long / ULong / Double /
//   Address via ExtValue (8 bytes), Int128 / UInt128 via WideValue (16
//   bytes) -- read the value into a local first, compute the result,
//   then free the old ExtValue/WideValue before overwriting the slot
//   (a fresh one is allocated only if the result type needs it).
//   Reading into a local before the free is what prevents a leak or a
//   use-after-free: the operand's heap bytes are dead by the time the
//   slot is reused.
//
// DO NOT REFACTOR
//   The shims are intentionally NOT consolidated into a single generic
//   dispatch helper.  Each shim's explicit per-type switch is a
//   deliberate design choice for clarity and debuggability.
//

//        X    R
// Byte     :- Integer
// UInteger :- Integer
// ULong    :- Integer
// UInt128  :- Integer
template<typename F>
static void shim_OUnsignedIntegers__RInteger(Trix *trx, F &&func) {
    constexpr auto X{VerifyUnsignedIntegers};
    trx->verify_operands(X);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto result = func(trx, x_value);
        *x_ptr = Object::make_integer(result);
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto result = func(trx, x_value);
        *x_ptr = Object::make_integer(result);
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto result = func(trx, x_value);
        x_ptr->free_extvalue(trx);
        *x_ptr = Object::make_integer(result);
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto result = func(trx, x_value);
        x_ptr->free_widevalue(trx);
        *x_ptr = Object::make_integer(result);
        return;
    }

    default:
        assert(false && "shim_OUnsignedIntegers__RInteger: logic error");
        std::unreachable();
    }
}

//        X    R
// Byte     :- Byte
// UInteger :- UInteger
// ULong    :- ULong
// UInt128  :- UInt128
template<typename F>
static void shim_OUnsignedIntegers__RUnsignedIntegers(Trix *trx, F &&func) {
    constexpr auto X{VerifyUnsignedIntegers};
    trx->verify_operands(X);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto result = func(trx, x_value);
        x_ptr->update_byte(result);
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto result = func(trx, x_value);
        x_ptr->update_uinteger(result);
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_ulong(trx, result);
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_uint128(trx, result);
        return;
    }

    default:
        assert(false && "shim_OUnsignedIntegers__RUnsignedIntegers: logic error");
        std::unreachable();
    }
}

//        X    R
// Byte     :- Boolean
// UInteger :- Boolean
// ULong    :- Boolean
// UInt128  :- Boolean
template<typename F>
static void shim_OUnsignedIntegers__RBoolean(Trix *trx, F &&func) {
    constexpr auto X{VerifyUnsignedIntegers};
    trx->verify_operands(X);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto result = func(trx, x_value);
        *x_ptr = Object::make_boolean(result);
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto result = func(trx, x_value);
        *x_ptr = Object::make_boolean(result);
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto result = func(trx, x_value);
        x_ptr->free_extvalue(trx);
        *x_ptr = Object::make_boolean(result);
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto result = func(trx, x_value);
        x_ptr->free_widevalue(trx);
        *x_ptr = Object::make_boolean(result);
        return;
    }

    default:
        assert(false && "shim_OUnsignedIntegers__RBoolean: logic error");
        std::unreachable();
    }
}

//        X       Y    R
// Byte     Integer :- Byte
// UInteger Integer :- UInteger
// ULong    Integer :- ULong
// UInt128  Integer :- UInt128
template<typename F>
static void shim_OUnsignedIntegers_Integer__RUnsignedIntegers(Trix *trx, F &&func) {
    constexpr auto Y{VerifyInteger};
    constexpr auto X{VerifyUnsignedIntegers};
    trx->verify_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto y_value = y_ptr->integer_value();
    auto x_ptr = (y_ptr - 1);
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_byte(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_uinteger(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto result = func(trx, x_value, y_value);
        x_ptr->update_ulong(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto result = func(trx, x_value, y_value);
        x_ptr->update_uint128(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_OUnsignedIntegers_Integer__RUnsignedIntegers: logic error");
        std::unreachable();
    }
}

//       X    R
// Integer :- Integer
// Long    :- Long
// Int128  :- Int128
// Real    :- Real
// Double  :- Double
template<typename F>
static void shim_OSignedIntegersFloats__RSignedIntegersFloats(Trix *trx, F &&func) {
    constexpr auto X{VerifySignedIntegers | VerifyFloats};
    trx->verify_operands(X);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Integer: {
        auto x_value = x_ptr->integer_value();
        auto result = func(trx, x_value);
        x_ptr->update_integer(result);
        return;
    }

    case +Object::Type::Long: {
        auto x_value = x_ptr->long_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_long(trx, result);
        return;
    }

    case +Object::Type::Int128: {
        auto x_value = x_ptr->int128_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_int128(trx, result);
        return;
    }

    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto result = func(trx, x_value);
        x_ptr->update_real(result);
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_double(trx, result);
        return;
    }

    default:
        assert(false && "shim_OSignedIntegersFloats__RSignedIntegersFloats: logic error");
        std::unreachable();
    }
}

//        X        Y    R
// Byte     Byte     :- Byte
// Integer  Integer  :- Integer
// UInteger UInteger :- UInteger
// Long     Long     :- Long
// ULong    ULong    :- ULong
// Int128   Int128   :- Int128
// UInt128  UInt128  :- UInt128
template<typename F>
static void shim_OIntegers_Integers__RIntegers(Trix *trx, F &&func) {
    constexpr auto Y{VerifyIntegers};
    constexpr auto X{VerifyIntegers};
    trx->verify_matched_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto y_type = y_ptr->type();
    auto x_ptr = (y_ptr - 1);

    switch (+y_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto y_value = y_ptr->byte_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_byte(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Integer: {
        auto x_value = x_ptr->integer_value();
        auto y_value = y_ptr->integer_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_integer(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto y_value = y_ptr->uinteger_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_uinteger(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Long: {
        auto x_value = x_ptr->long_value(trx);
        auto y_value = y_ptr->long_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_long(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto y_value = y_ptr->ulong_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_ulong(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Int128: {
        auto x_value = x_ptr->int128_value(trx);
        auto y_value = y_ptr->int128_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_widevalue(trx);
        x_ptr->update_int128(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto y_value = y_ptr->uint128_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_widevalue(trx);
        x_ptr->update_uint128(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_OIntegers_Integers__RIntegers: logic error");
        std::unreachable();
    }
}

//        X    R
// Byte     :- Byte
// Integer  :- Integer
// UInteger :- UInteger
// Long     :- Long
// ULong    :- ULong
// Int128   :- Int128
// UInt128  :- UInt128
// Real     :- Real
// Double   :- Double
template<typename F>
static void shim_ONumbers__RNumbers(Trix *trx, F &&func) {
    constexpr auto X{VerifyNumbers};
    trx->verify_operands(X);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto result = func(trx, x_value);
        x_ptr->update_byte(result);
        return;
    }

    case +Object::Type::Integer: {
        auto x_value = x_ptr->integer_value();
        auto result = func(trx, x_value);
        x_ptr->update_integer(result);
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto result = func(trx, x_value);
        x_ptr->update_uinteger(result);
        return;
    }

    case +Object::Type::Long: {
        auto x_value = x_ptr->long_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_long(trx, result);
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_ulong(trx, result);
        return;
    }

    case +Object::Type::Int128: {
        auto x_value = x_ptr->int128_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_int128(trx, result);
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_uint128(trx, result);
        return;
    }

    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto result = func(trx, x_value);
        x_ptr->update_real(result);
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_double(trx, result);
        return;
    }

    default:
        assert(false && "shim_ONumbers__RNumbers: logic error");
        std::unreachable();
    }
}

//        X        Y    R
// Byte     Byte     :- Byte
// Integer  Integer  :- Integer
// UInteger UInteger :- UInteger
// Long     Long     :- Long
// ULong    ULong    :- ULong
// Int128   Int128   :- Int128
// UInt128  UInt128  :- UInt128
// Real     Real     :- Real
// Double   Double   :- Double
template<typename F>
static void shim_ONumbers_Numbers__RNumbers(Trix *trx, verify_t Yattr, verify_t Xattr, F &&func) {
    assert((Yattr & ~VerifyAttrs) == 0);
    assert((Xattr & ~VerifyAttrs) == 0);

    auto Y{VerifyNumbers | Yattr};
    auto X{VerifyNumbers | Xattr};
    trx->verify_matched_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto y_type = y_ptr->type();
    auto x_ptr = (y_ptr - 1);

    switch (+y_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto y_value = y_ptr->byte_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_byte(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Integer: {
        auto x_value = x_ptr->integer_value();
        auto y_value = y_ptr->integer_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_integer(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto y_value = y_ptr->uinteger_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_uinteger(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Long: {
        auto x_value = x_ptr->long_value(trx);
        auto y_value = y_ptr->long_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_long(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto y_value = y_ptr->ulong_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_ulong(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Int128: {
        auto x_value = x_ptr->int128_value(trx);
        auto y_value = y_ptr->int128_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_widevalue(trx);
        x_ptr->update_int128(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto y_value = y_ptr->uint128_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_widevalue(trx);
        x_ptr->update_uint128(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto y_value = y_ptr->real_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_real(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto y_value = y_ptr->double_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_double(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_ONumbers_Numbers__RNumbers: logic error");
        std::unreachable();
    }
}

//        X        Y    R
// Byte     Byte     :- Byte
// Integer  Integer  :- Integer
// UInteger UInteger :- UInteger
// Long     Long     :- Long
// ULong    ULong    :- ULong
// Int128   Int128   :- Int128
// UInt128  UInt128  :- UInt128
// Address  Address  :- Address
// Real     Real     :- Real
// Double   Double   :- Double
// String   String   :- String
template<typename F>
static void shim_ONumbersAddressString_NumbersAddressString__RNumbersAddressString(Trix *trx, F &&func) {
    constexpr auto Y{VerifyNumbers | VerifyAddress | VerifyString};
    constexpr auto X{VerifyNumbers | VerifyAddress | VerifyString};
    trx->verify_matched_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto y_type = y_ptr->type();
    auto x_ptr = (y_ptr - 1);

    switch (+y_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto y_value = y_ptr->byte_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_byte(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Integer: {
        auto x_value = x_ptr->integer_value();
        auto y_value = y_ptr->integer_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_integer(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto y_value = y_ptr->uinteger_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_uinteger(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Long: {
        auto x_value = x_ptr->long_value(trx);
        auto y_value = y_ptr->long_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_long(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto y_value = y_ptr->ulong_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_ulong(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Int128: {
        auto x_value = x_ptr->int128_value(trx);
        auto y_value = y_ptr->int128_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_widevalue(trx);
        x_ptr->update_int128(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto y_value = y_ptr->uint128_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_widevalue(trx);
        x_ptr->update_uint128(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Address: {
        auto x_value = x_ptr->address_value(trx);
        auto y_value = y_ptr->address_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_address(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto y_value = y_ptr->real_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_real(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto y_value = y_ptr->double_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_double(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::String: {
        auto result_ptr = func(trx, x_ptr, y_ptr);
        *x_ptr = *result_ptr;
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_ONumbersAddressString_NumbersAddressString__RNumbersAddressString: logic error");
        std::unreachable();
    }
}

//        X        Y        Z    R
// Byte     Byte     Byte     :- Byte
// Integer  Integer  Integer  :- Integer
// UInteger UInteger UInteger :- UInteger
// Long     Long     Long     :- Long
// ULong    ULong    ULong    :- ULong
// Int128   Int128   Int128   :- Int128
// UInt128  UInt128  UInt128  :- UInt128
// Address  Address  Address  :- Address
// Real     Real     Real     :- Real
// Double   Double   Double   :- Double
// String   String   String   :- String
template<typename F>
static void shim_ONumbersAddressString_NumbersAddressString_NumbersAddressString__RNumbersAddressString(Trix *trx, F &&func) {
    constexpr auto Z{VerifyNumbers | VerifyAddress | VerifyString};
    constexpr auto Y{VerifyNumbers | VerifyAddress | VerifyString};
    constexpr auto X{VerifyNumbers | VerifyAddress | VerifyString};
    trx->verify_matched_operands(Z, Y, X);

    auto z_ptr = trx->m_op_ptr;
    auto z_type = z_ptr->type();
    auto y_ptr = (z_ptr - 1);
    auto x_ptr = (y_ptr - 1);

    switch (+z_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto y_value = y_ptr->byte_value();
        auto z_value = z_ptr->byte_value();
        auto result = func(trx, x_value, y_value, z_value);
        x_ptr->update_byte(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Integer: {
        auto x_value = x_ptr->integer_value();
        auto y_value = y_ptr->integer_value();
        auto z_value = z_ptr->integer_value();
        auto result = func(trx, x_value, y_value, z_value);
        x_ptr->update_integer(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto y_value = y_ptr->uinteger_value();
        auto z_value = z_ptr->uinteger_value();
        auto result = func(trx, x_value, y_value, z_value);
        x_ptr->update_uinteger(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Long: {
        auto x_value = x_ptr->long_value(trx);
        auto y_value = y_ptr->long_value(trx);
        auto z_value = z_ptr->long_value(trx);
        auto result = func(trx, x_value, y_value, z_value);
        z_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        x_ptr->update_long(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto y_value = y_ptr->ulong_value(trx);
        auto z_value = z_ptr->ulong_value(trx);
        auto result = func(trx, x_value, y_value, z_value);
        z_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        x_ptr->update_ulong(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Int128: {
        auto x_value = x_ptr->int128_value(trx);
        auto y_value = y_ptr->int128_value(trx);
        auto z_value = z_ptr->int128_value(trx);
        auto result = func(trx, x_value, y_value, z_value);
        z_ptr->free_widevalue(trx);
        y_ptr->free_widevalue(trx);
        x_ptr->update_int128(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto y_value = y_ptr->uint128_value(trx);
        auto z_value = z_ptr->uint128_value(trx);
        auto result = func(trx, x_value, y_value, z_value);
        z_ptr->free_widevalue(trx);
        y_ptr->free_widevalue(trx);
        x_ptr->update_uint128(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Address: {
        auto x_value = x_ptr->address_value(trx);
        auto y_value = y_ptr->address_value(trx);
        auto z_value = z_ptr->address_value(trx);
        auto result = func(trx, x_value, y_value, z_value);
        z_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        x_ptr->update_address(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto y_value = y_ptr->real_value();
        auto z_value = z_ptr->real_value();
        auto result = func(trx, x_value, y_value, z_value);
        x_ptr->update_real(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto y_value = y_ptr->double_value(trx);
        auto z_value = z_ptr->double_value(trx);
        auto result = func(trx, x_value, y_value, z_value);
        z_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        x_ptr->update_double(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::String: {
        auto result_ptr = func(trx, x_ptr, y_ptr, z_ptr);
        *x_ptr = *result_ptr;
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_ONumbersAddressString_NumbersAddressString_NumbersAddressString__RNumbersAddressString: "
                        "logic error");
        std::unreachable();
    }
}

//        X        Y    R
// Byte     Byte     :- Boolean
// Integer  Integer  :- Boolean
// UInteger UInteger :- Boolean
// Long     Long     :- Boolean
// ULong    ULong    :- Boolean
// Int128   Int128   :- Boolean
// UInt128  UInt128  :- Boolean
// Address  Address  :- Boolean
// Real     Real     :- Boolean
// Double   Double   :- Boolean
// String   String   :- Boolean
template<typename F>
static void shim_ONumbersAddressString_NumbersAddressString__RBoolean(Trix *trx, F &&func) {
    constexpr auto Y{VerifyNumbers | VerifyAddress | VerifyString};
    constexpr auto X{VerifyNumbers | VerifyAddress | VerifyString};
    trx->verify_matched_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto y_type = y_ptr->type();
    auto x_ptr = (y_ptr - 1);

    switch (+y_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto y_value = y_ptr->byte_value();
        auto result = func(trx, x_value, y_value);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Integer: {
        auto x_value = x_ptr->integer_value();
        auto y_value = y_ptr->integer_value();
        auto result = func(trx, x_value, y_value);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto y_value = y_ptr->uinteger_value();
        auto result = func(trx, x_value, y_value);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Long: {
        auto x_value = x_ptr->long_value(trx);
        auto y_value = y_ptr->long_value(trx);
        auto result = func(trx, x_value, y_value);
        x_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto y_value = y_ptr->ulong_value(trx);
        auto result = func(trx, x_value, y_value);
        x_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Int128: {
        auto x_value = x_ptr->int128_value(trx);
        auto y_value = y_ptr->int128_value(trx);
        auto result = func(trx, x_value, y_value);
        x_ptr->free_widevalue(trx);
        y_ptr->free_widevalue(trx);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto y_value = y_ptr->uint128_value(trx);
        auto result = func(trx, x_value, y_value);
        x_ptr->free_widevalue(trx);
        y_ptr->free_widevalue(trx);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Address: {
        auto x_value = x_ptr->address_value(trx);
        auto y_value = y_ptr->address_value(trx);
        auto result = func(trx, x_value, y_value);
        x_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto y_value = y_ptr->real_value();
        auto result = func(trx, x_value, y_value);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto y_value = y_ptr->double_value(trx);
        auto result = func(trx, x_value, y_value);
        x_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::String: {
        auto result = func(trx, x_ptr, y_ptr);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_ONumbersAddressString_NumbersAddressString__RBoolean: logic error");
        std::unreachable();
    }
}

//      X    R
// Real   :- Real
// Double :- Double
template<typename F>
static void shim_OFloats__RFloats(Trix *trx, verify_t Xattr, F &&func) {
    assert((Xattr & ~VerifyAttrs) == 0);

    auto X{VerifyFloats | Xattr};
    trx->verify_operands(X);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto result = func(trx, x_value);
        x_ptr->update_real(result);
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_double(trx, result);
        return;
    }

    default:
        assert(false && "shim_OFloats__RFloats: logic error");
        std::unreachable();
    }
}

//      X    R
// Real   :- Real   Real
// Double :- Double Double
template<typename F>
static void shim_OFloats__RFloats_Floats(Trix *trx, F &&func) {
    constexpr auto X{VerifyFloats};
    trx->verify_operands(X);
    trx->require_op_capacity(1);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto [first, second] = func(trx, x_value);
        x_ptr->update_real(first);
        *++trx->m_op_ptr = Object::make_real(second);
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto [first, second] = func(trx, x_value);
        x_ptr->update_double(trx, first);
        *++trx->m_op_ptr = Object::make_double(trx, second);
        return;
    }

    default:
        assert(false && "shim_OFloats__RFloats_Floats: logic error");
        std::unreachable();
    }
}

//      X    R
// Real   :- Real   Integer
// Double :- Double Integer
template<typename F>
static void shim_OFloats__RFloats_Integer(Trix *trx, F &&func) {
    constexpr auto X{VerifyFloats};
    trx->verify_operands(X);
    trx->require_op_capacity(1);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto [first, second] = func(trx, x_value);
        x_ptr->update_real(first);
        *++trx->m_op_ptr = Object::make_integer(second);
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto [first, second] = func(trx, x_value);
        x_ptr->update_double(trx, first);
        *++trx->m_op_ptr = Object::make_integer(second);
        return;
    }

    default:
        assert(false && "shim_OFloats__RFloats_Integer: logic error");
        std::unreachable();
    }
}

//      X      Y    R
// Real   Real   :- Real   Integer
// Double Double :- Double Integer
template<typename F>
static void shim_OFloats_Floats__RFloats_Integer(Trix *trx, F &&func) {
    constexpr auto Y{VerifyFloats};
    constexpr auto X{VerifyFloats};
    trx->verify_matched_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto y_type = y_ptr->type();
    auto x_ptr = (y_ptr - 1);

    switch (+y_type) {
    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto y_value = y_ptr->real_value();
        auto [first, second] = func(trx, x_value, y_value);
        x_ptr->update_real(first);
        *y_ptr = Object::make_integer(second);
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto y_value = y_ptr->double_value(trx);
        auto [first, second] = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_double(trx, first);
        *y_ptr = Object::make_integer(second);
        return;
    }

    default:
        assert(false && "shim_OFloats_Floats__RFloats_Integer: logic error");
        std::unreachable();
    }
}

//       X       Y    R
// Integer Integer :- Integer Integer
// Long    Long    :- Long    Long
// Int128  Int128  :- Int128  Int128
template<typename F>
static void shim_OSignedIntegers_SignedIntegers__RSignedIntegers_SignedIntegers(Trix *trx, F &&func) {
    constexpr auto Y{VerifySignedIntegers};
    constexpr auto X{VerifySignedIntegers};
    trx->verify_matched_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto y_type = y_ptr->type();
    auto x_ptr = (y_ptr - 1);

    switch (+y_type) {
    case +Object::Type::Integer: {
        auto x_value = x_ptr->integer_value();
        auto y_value = y_ptr->integer_value();
        auto [first, second] = func(trx, x_value, y_value);
        x_ptr->update_integer(first);
        y_ptr->update_integer(second);
        return;
    }

    case +Object::Type::Long: {
        auto x_value = x_ptr->long_value(trx);
        auto y_value = y_ptr->long_value(trx);
        auto [first, second] = func(trx, x_value, y_value);
        x_ptr->update_long(trx, first);
        y_ptr->update_long(trx, second);
        return;
    }

    case +Object::Type::Int128: {
        auto x_value = x_ptr->int128_value(trx);
        auto y_value = y_ptr->int128_value(trx);
        auto [first, second] = func(trx, x_value, y_value);
        x_ptr->update_int128(trx, first);
        y_ptr->update_int128(trx, second);
        return;
    }

    default:
        assert(false && "shim_OSignedIntegers_SignedIntegers__RSignedIntegers_SignedIntegers: logic error");
        std::unreachable();
    }
}

//      X    R
// Real   :- Integer
// Double :- Integer
template<typename F>
static void shim_OFloats__RInteger(Trix *trx, F &&func) {
    constexpr auto X{VerifyFloats};
    trx->verify_operands(X);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto result = func(trx, x_value);
        *x_ptr = Object::make_integer(result);
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto result = func(trx, x_value);
        x_ptr->free_extvalue(trx);
        *x_ptr = Object::make_integer(result);
        return;
    }

    default:
        assert(false && "shim_OFloats__RInteger: logic error");
        std::unreachable();
    }
}

//      X    R
// Real   :- Boolean
// Double :- Boolean
template<typename F>
static void shim_OFloats__RBoolean(Trix *trx, F &&func) {
    constexpr auto X{VerifyFloats};
    trx->verify_operands(X);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto result = func(trx, x_value);
        *x_ptr = Object::make_boolean(result);
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto result = func(trx, x_value);
        x_ptr->free_extvalue(trx);
        *x_ptr = Object::make_boolean(result);
        return;
    }

    default:
        assert(false && "shim_OFloats__RBoolean: logic error");
        std::unreachable();
    }
}

//      X      Y    R
// Real   Real   :- Boolean
// Double Double :- Boolean
template<typename F>
static void shim_OFloats_Floats__RBoolean(Trix *trx, F &&func) {
    constexpr auto Y{VerifyFloats};
    constexpr auto X{VerifyFloats};
    trx->verify_matched_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto y_type = y_ptr->type();
    auto x_ptr = (y_ptr - 1);

    switch (+y_type) {
    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto y_value = y_ptr->real_value();
        auto result = func(trx, x_value, y_value);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto y_value = y_ptr->double_value(trx);
        auto result = func(trx, x_value, y_value);
        x_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        *x_ptr = Object::make_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_OFloats_Floats__RBoolean: logic error");
        std::unreachable();
    }
}

//      X      Y      Z    R
// Real   Real   Real   :- Real
// Double Double Double :- Double
template<typename F>
static void shim_OFloats_Floats_Floats__RFloats(Trix *trx, F &&func) {
    constexpr auto Z{VerifyFloats};
    constexpr auto Y{VerifyFloats};
    constexpr auto X{VerifyFloats};
    trx->verify_matched_operands(Z, Y, X);

    auto z_ptr = trx->m_op_ptr;
    auto z_type = z_ptr->type();
    auto y_ptr = (z_ptr - 1);
    auto x_ptr = (y_ptr - 1);

    switch (+z_type) {
    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto y_value = y_ptr->real_value();
        auto z_value = z_ptr->real_value();
        auto result = func(trx, x_value, y_value, z_value);
        x_ptr->update_real(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto y_value = y_ptr->double_value(trx);
        auto z_value = z_ptr->double_value(trx);
        auto result = func(trx, x_value, y_value, z_value);
        // Free discarded operands top-down (z then y), matching the generic
        // 3-operand shim; x_ptr is kept and updated in place.
        z_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        x_ptr->update_double(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_OFloats_Floats_Floats__RFloats: logic error");
        std::unreachable();
    }
}

//      X      Y    R
// Real   Real   :- Real
// Double Double :- Double
template<typename F>
static void shim_OFloats_Floats__RFloats(Trix *trx, verify_t Xattr, F &&func) {
    assert((Xattr & ~VerifyAttrs) == 0);

    auto Y{VerifyFloats | Xattr};
    auto X{VerifyFloats | Xattr};
    trx->verify_matched_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto y_type = y_ptr->type();
    auto x_ptr = (y_ptr - 1);

    switch (+y_type) {
    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto y_value = y_ptr->real_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_real(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto y_value = y_ptr->double_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_double(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_OFloats_Floats__RFloats: logic error");
        std::unreachable();
    }
}

template<typename F>
static void shim_OFloats_Floats__RFloats(Trix *trx, F &&func) {
    shim_OFloats_Floats__RFloats(trx, VerifyAttrsNone, std::forward<F>(func));
}

//      X       Y    R
// Real   Integer :- Real
// Double Integer :- Double
template<typename F>
static void shim_OFloats_Integer__RFloats(Trix *trx, F &&func) {
    constexpr auto Y{VerifyInteger};
    constexpr auto X{VerifyFloats};
    trx->verify_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto x_ptr = (y_ptr - 1);

    switch (+x_ptr->type()) {
    case +Object::Type::Real: {
        auto x_value = x_ptr->real_value();
        auto y_value = y_ptr->integer_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_real(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Double: {
        auto x_value = x_ptr->double_value(trx);
        auto y_value = y_ptr->integer_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_double(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_OFloats_Integer__RFloats: logic error");
        std::unreachable();
    }
}

//        X          Y    R
// UInteger   Real     :- Real
// UInteger   Double   :- Double
//
// The C++ standard ([sf.cmath]) requires special-function support only for
// orders below 128; orders >= 128 give implementation-defined results, so both
// order-shims reject them.  The attr parameter constrains the float operand.
static constexpr uinteger_t SpecialFunctionOrderLimit{128};

template<typename F>
static void shim_OUInteger_Floats__RFloats(Trix *trx, verify_t Yattr, F &&func) {
    assert((Yattr & ~VerifyAttrs) == 0);

    auto Y{VerifyFloats | Yattr};
    constexpr auto X{VerifyUInteger};
    trx->verify_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto x_ptr = (y_ptr - 1);
    auto x_value = x_ptr->uinteger_value();
    if (x_value >= SpecialFunctionOrderLimit) {
        trx->error(Error::RangeCheck,
                   "operand order {} is too large; special functions support orders 0..{}",
                   x_value,
                   SpecialFunctionOrderLimit - 1);
    }

    switch (+y_ptr->type()) {
    case +Object::Type::Real: {
        auto y_value = y_ptr->real_value();
        auto result = func(trx, x_value, y_value);
        *x_ptr = Object::make_real(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Double: {
        auto y_value = y_ptr->double_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        *x_ptr = Object::make_double(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_OUInteger_Floats__RFloats: logic error");
        std::unreachable();
    }
}

//        X          Y          Z    R
// UInteger   UInteger   Real     :- Real
// UInteger   UInteger   Double   :- Double
template<typename F>
static void shim_OUInteger_UInteger_Floats__RFloats(Trix *trx, verify_t Zattr, F &&func) {
    assert((Zattr & ~VerifyAttrs) == 0);

    auto Z{VerifyFloats | Zattr};
    constexpr auto Y{VerifyUInteger};
    constexpr auto X{VerifyUInteger};
    trx->verify_operands(Z, Y, X);

    auto z_ptr = trx->m_op_ptr;
    auto y_ptr = (z_ptr - 1);
    auto x_ptr = (y_ptr - 1);
    auto x_value = x_ptr->uinteger_value();
    auto y_value = y_ptr->uinteger_value();
    if ((x_value >= SpecialFunctionOrderLimit) || (y_value >= SpecialFunctionOrderLimit)) {
        trx->error(Error::RangeCheck,
                   "operand orders ({}, {}) are too large; special functions support orders 0..{}",
                   x_value,
                   y_value,
                   SpecialFunctionOrderLimit - 1);
    }

    switch (+z_ptr->type()) {
    case +Object::Type::Real: {
        auto z_value = z_ptr->real_value();
        auto result = func(trx, x_value, y_value, z_value);
        *x_ptr = Object::make_real(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Double: {
        auto z_value = z_ptr->double_value(trx);
        auto result = func(trx, x_value, y_value, z_value);
        z_ptr->free_extvalue(trx);
        *x_ptr = Object::make_double(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_OUInteger_UInteger_Floats__RFloats: logic error");
        std::unreachable();
    }
}

//        X        Y    R
// Byte     Byte     :- Byte
// UInteger UInteger :- UInteger
// ULong    ULong    :- ULong
// UInt128  UInt128  :- UInt128
// Address  Address  :- Address
// Boolean  Boolean  :- Boolean
template<typename F>
static void shim_OUnsignedIntegersAddressBoolean_UnsignedIntegersAddressBoolean__RUnsignedIntegersAddressBoolean(Trix *trx,
                                                                                                                 F &&func) {
    constexpr auto Y{VerifyUnsignedIntegers | VerifyAddress | VerifyBoolean};
    constexpr auto X{VerifyUnsignedIntegers | VerifyAddress | VerifyBoolean};
    trx->verify_matched_operands(Y, X);

    auto y_ptr = trx->m_op_ptr;
    auto y_type = y_ptr->type();
    auto x_ptr = (y_ptr - 1);

    switch (+y_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto y_value = y_ptr->byte_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_byte(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto y_value = y_ptr->uinteger_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_uinteger(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto y_value = y_ptr->ulong_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_ulong(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto y_value = y_ptr->uint128_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_widevalue(trx);
        x_ptr->update_uint128(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Address: {
        auto x_value = x_ptr->address_value(trx);
        auto y_value = y_ptr->address_value(trx);
        auto result = func(trx, x_value, y_value);
        y_ptr->free_extvalue(trx);
        x_ptr->update_address(trx, result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    case +Object::Type::Boolean: {
        auto x_value = x_ptr->boolean_value();
        auto y_value = y_ptr->boolean_value();
        auto result = func(trx, x_value, y_value);
        x_ptr->update_boolean(result);
        trx->m_op_ptr = x_ptr;
        return;
    }

    default:
        assert(false && "shim_OUnsignedIntegersAddressBoolean_UnsignedIntegersAddressBoolean__"
                        "RUnsignedIntegersAddressBoolean: logic error");
        std::unreachable();
    }
}

//        X    R
// Byte     :- Byte
// UInteger :- UInteger
// ULong    :- ULong
// UInt128  :- UInt128
// Address  :- Address
// Boolean  :- Boolean
template<typename F>
static void shim_OUnsignedIntegersAddressBoolean__RUnsignedIntegersAddressBoolean(Trix *trx, F &&func) {
    constexpr auto X{VerifyUnsignedIntegers | VerifyAddress | VerifyBoolean};
    trx->verify_operands(X);

    auto x_ptr = trx->m_op_ptr;
    auto x_type = x_ptr->type();

    switch (+x_type) {
    case +Object::Type::Byte: {
        auto x_value = x_ptr->byte_value();
        auto result = func(trx, x_value);
        x_ptr->update_byte(result);
        return;
    }

    case +Object::Type::UInteger: {
        auto x_value = x_ptr->uinteger_value();
        auto result = func(trx, x_value);
        x_ptr->update_uinteger(result);
        return;
    }

    case +Object::Type::ULong: {
        auto x_value = x_ptr->ulong_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_ulong(trx, result);
        return;
    }

    case +Object::Type::UInt128: {
        auto x_value = x_ptr->uint128_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_uint128(trx, result);
        return;
    }

    case +Object::Type::Address: {
        auto x_value = x_ptr->address_value(trx);
        auto result = func(trx, x_value);
        x_ptr->update_address(trx, result);
        return;
    }

    case +Object::Type::Boolean: {
        auto x_value = x_ptr->boolean_value();
        auto result = func(trx, x_value);
        x_ptr->update_boolean(result);
        return;
    }

    default:
        assert(false && "shim_OUnsignedIntegersAddressBoolean__RUnsignedIntegersAddressBoolean: logic error");
        std::unreachable();
    }
}
