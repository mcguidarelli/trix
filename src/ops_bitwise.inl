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

// shift-left: uint int :- uint
// Shifts unsigned integer left by count bits.
// throws: opstack-underflow, type-check
static void shift_left_op(Trix *trx) {
    shim_OUnsignedIntegers_Integer__RUnsignedIntegers(trx, [](Trix *, auto x, integer_t y) {
        using T = decltype(x);

        const auto digits = std::numeric_limits<T>::digits;
        if (y > 0) {
            return static_cast<T>(x << (y % digits));
        } else if (y < 0) {
            // Cast to long_t before negating to avoid UB when y == INT_MIN
            const auto shift = static_cast<int>(static_cast<uinteger_t>(-static_cast<long_t>(y)) % static_cast<unsigned>(digits));
            return static_cast<T>(x >> shift);
        } else {
            return x;
        }
    });
}

// shift-right: uint int :- uint
// Shifts unsigned integer right by count bits.
// throws: opstack-underflow, type-check
static void shift_right_op(Trix *trx) {
    shim_OUnsignedIntegers_Integer__RUnsignedIntegers(trx, [](Trix *, auto x, integer_t y) {
        using T = decltype(x);

        const auto digits = std::numeric_limits<T>::digits;
        if (y > 0) {
            return static_cast<T>(x >> (y % digits));
        } else if (y < 0) {
            // Cast to long_t before negating to avoid UB when y == INT_MIN
            const auto shift = static_cast<int>(static_cast<uinteger_t>(-static_cast<long_t>(y)) % static_cast<unsigned>(digits));
            return static_cast<T>(x << shift);
        } else {
            return x;
        }
    });
}

// rotate-left: uint int :- uint
// Rotates unsigned integer left by count bits.
// throws: opstack-underflow, type-check
static void rotate_left_op(Trix *trx) {
    shim_OUnsignedIntegers_Integer__RUnsignedIntegers(trx, [](Trix *, auto x, integer_t y) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, uint128_t>) {
            constexpr auto N = int{128};
            const auto r = y % N;
            if (r == 0) {
                return x;
            } else if (r > 0) {
                return static_cast<T>((x << r) | (x >> (N - r)));
            } else {
                const auto rr = -r;
                return static_cast<T>((x >> rr) | (x << (N - rr)));
            }
        } else {
            return std::rotl(x, y);
        }
    });
}

// rotate-right: uint int :- uint
// Rotates unsigned integer right by count bits.
// throws: opstack-underflow, type-check
static void rotate_right_op(Trix *trx) {
    shim_OUnsignedIntegers_Integer__RUnsignedIntegers(trx, [](Trix *, auto x, integer_t y) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, uint128_t>) {
            constexpr auto N = int{128};
            const auto r = y % N;
            if (r == 0) {
                return x;
            } else if (r > 0) {
                return static_cast<T>((x >> r) | (x << (N - r)));
            } else {
                const auto rr = -r;
                return static_cast<T>((x << rr) | (x >> (N - rr)));
            }
        } else {
            return std::rotr(x, y);
        }
    });
}

template<typename T>
requires std::is_integral_v<T> || std::is_floating_point_v<T> || std::is_same_v<T, int128_t> || std::is_same_v<T, uint128_t>
static T byteswap_helper(T x) {
    if constexpr (std::is_same_v<T, int128_t> || std::is_same_v<T, uint128_t>) {
        const auto u = static_cast<uint128_t>(x);
        const auto hi = static_cast<uint64_t>(u >> 64);
        const auto lo = static_cast<uint64_t>(u);
        const auto result = (static_cast<uint128_t>(std::byteswap(lo)) << 64) | static_cast<uint128_t>(std::byteswap(hi));
        return static_cast<T>(result);
    } else if constexpr (std::is_integral_v<T>) {
        return std::byteswap(x);
    } else if constexpr (std::is_same_v<T, real_t>) {
        return std::bit_cast<real_t>(std::byteswap(std::bit_cast<uint32_t>(x)));
    } else if constexpr (std::is_same_v<T, double_t>) {
        return std::bit_cast<double_t>(std::byteswap(std::bit_cast<uint64_t>(x)));
    } else {
        std::unreachable();
    }
}

// byteswap: num :- num
// Reverses the byte order.
// throws: opstack-underflow, type-check
static void byteswap_op(Trix *trx) {
    shim_ONumbers__RNumbers(trx, [](Trix *, auto x) { return byteswap_helper(x); });
}

// Shared helper for bit?, bit-set, bit-clear, bit-toggle.
// Extracts value as uint64, validates bit position against type width,
// calls op_fn(raw_value, bit_mask) to produce the result.
// For bit?: op_fn returns bool-like (0 or non-zero); result is Boolean.
// For set/clear/toggle: op_fn returns modified uint64; result preserves input type.
enum class BitOpKind { Test, Modify };

// bit?: int n :- bool -- test if bit n is set
// throws: opstack-underflow, range-check, type-check
static void bittest_op(Trix *trx) {
    bit_position_op<BitOpKind::Test>(trx, "bit?", [](uint128_t val, uint128_t mask) { return (val & mask); });
}

// bit-set: int n :- int -- set bit n
// throws: opstack-underflow, range-check, type-check
static void bitset_op(Trix *trx) {
    bit_position_op<BitOpKind::Modify>(trx, "bit-set", [](uint128_t val, uint128_t mask) { return (val | mask); });
}

// bit-clear: int n :- int -- clear bit n
// throws: opstack-underflow, range-check, type-check
static void bitclear_op(Trix *trx) {
    bit_position_op<BitOpKind::Modify>(trx, "bit-clear", [](uint128_t val, uint128_t mask) { return (val & ~mask); });
}

// bit-toggle: int n :- int -- toggle bit n
// throws: opstack-underflow, range-check, type-check
static void bittoggle_op(Trix *trx) {
    bit_position_op<BitOpKind::Modify>(trx, "bit-toggle", [](uint128_t val, uint128_t mask) { return (val ^ mask); });
}

// Returns the signed byte offset from an integer-typed Object.
// Accepts Byte, Integer, UInteger, Long, ULong.
[[nodiscard]] static long_t address_offset(Trix *trx, const Object *val_ptr) {
    switch (+val_ptr->type()) {
    case +Object::Type::Byte:
        return val_ptr->byte_value();

    case +Object::Type::Integer:
        return val_ptr->integer_value();

    case +Object::Type::UInteger:
        return val_ptr->uinteger_value();

    case +Object::Type::Long:
        return val_ptr->long_value(trx);

    case +Object::Type::ULong: {
        auto ul = val_ptr->ulong_value(trx);
        return static_cast<long_t>(ul);
    }

    default:
        trx->error(Error::TypeCheck, "address arithmetic requires an integer offset");
    }
}

// bit-ceil: uint :- uint
// Smallest power of two not less than the value.
// throws: numerical-overflow, opstack-underflow, type-check
static void bit_ceil_op(Trix *trx) {
    shim_OUnsignedIntegers__RUnsignedIntegers(trx, [](Trix *p, auto x) {
        using T = decltype(x);
        if (x > (T{1} << (std::numeric_limits<T>::digits - 1))) {
            p->error(Error::NumericalOverflow, "bit-ceil: result would overflow");
        } else if constexpr (std::is_same_v<T, uint128_t>) {
            if (x <= T{1}) {
                return T{1};
            } else {
                auto w = 128 - __builtin_clzg(static_cast<T>(x - T{1}));
                return static_cast<T>(T{1} << w);
            }
        } else {
            return std::bit_ceil(x);
        }
    });
}

// bit-floor: uint :- uint
// Largest power of two not greater than the value.
// throws: opstack-underflow, type-check
static void bit_floor_op(Trix *trx) {
    shim_OUnsignedIntegers__RUnsignedIntegers(trx, [](Trix *, auto x) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, uint128_t>) {
            if (x == T{0}) {
                return T{0};
            } else {
                auto w = 128 - __builtin_clzg(x);
                return static_cast<T>(T{1} << (w - 1));
            }
        } else {
            return std::bit_floor(x);
        }
    });
}

// bit-width: uint :- int
// Smallest number of bits needed to represent the value.
// throws: opstack-underflow, type-check
static void bit_width_op(Trix *trx) {
    shim_OUnsignedIntegers__RInteger(trx, [](Trix *, auto x) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, uint128_t>) {
            return integer_t{128 - __builtin_clzg(x, 128)};
        } else {
            return integer_t{std::bit_width(x)};
        }
    });
}

// countl-zero: uint :- int
// Counts consecutive zero bits from the most significant bit.
// throws: opstack-underflow, type-check
static void countl_zero_op(Trix *trx) {
    shim_OUnsignedIntegers__RInteger(trx, [](Trix *, auto x) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, uint128_t>) {
            return integer_t{__builtin_clzg(x, 128)};
        } else {
            return integer_t{std::countl_zero(x)};
        }
    });
}

// countl-one: uint :- int
// Counts consecutive one bits from the most significant bit.
// throws: opstack-underflow, type-check
static void countl_one_op(Trix *trx) {
    shim_OUnsignedIntegers__RInteger(trx, [](Trix *, auto x) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, uint128_t>) {
            return integer_t{__builtin_clzg(static_cast<T>(~x), 128)};
        } else {
            return integer_t{std::countl_one(x)};
        }
    });
}

// countr-zero: uint :- int
// Counts consecutive zero bits from the least significant bit.
// throws: opstack-underflow, type-check
static void countr_zero_op(Trix *trx) {
    shim_OUnsignedIntegers__RInteger(trx, [](Trix *, auto x) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, uint128_t>) {
            return integer_t{__builtin_ctzg(x, 128)};
        } else {
            return integer_t{std::countr_zero(x)};
        }
    });
}

// countr-one: uint :- int
// Counts consecutive one bits from the least significant bit.
// throws: opstack-underflow, type-check
static void countr_one_op(Trix *trx) {
    shim_OUnsignedIntegers__RInteger(trx, [](Trix *, auto x) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, uint128_t>) {
            return integer_t{__builtin_ctzg(static_cast<T>(~x), 128)};
        } else {
            return integer_t{std::countr_one(x)};
        }
    });
}

// popcount: uint :- int
// Counts the number of 1 bits.
// throws: opstack-underflow, type-check
static void popcount_op(Trix *trx) {
    shim_OUnsignedIntegers__RInteger(trx, [](Trix *, auto x) { return integer_t{__builtin_popcountg(x)}; });
}

// and: any any :- any
// Bitwise AND (unsigned integers, addresses) or logical AND (booleans).
// throws: opstack-underflow, type-check
static void and_op(Trix *trx) {
    shim_OUnsignedIntegersAddressBoolean_UnsignedIntegersAddressBoolean__RUnsignedIntegersAddressBoolean(
            trx, [](Trix *, auto x, auto y) {
                using T = decltype(x);
                static_assert(std::is_same_v<T, decltype(y)>);

                if constexpr (std::is_same_v<T, boolean_t>) {
                    return (x && y);
                } else if constexpr (std::is_same_v<T, address_t>) {
                    return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(x) & reinterpret_cast<uintptr_t>(y));
                } else {
                    return static_cast<T>(x & y);
                }
            });
}

// or: any any :- any
// Bitwise OR (unsigned integers, addresses) or logical OR (booleans).
// throws: opstack-underflow, type-check
static void or_op(Trix *trx) {
    shim_OUnsignedIntegersAddressBoolean_UnsignedIntegersAddressBoolean__RUnsignedIntegersAddressBoolean(
            trx, [](Trix *, auto x, auto y) {
                using T = decltype(x);
                static_assert(std::is_same_v<T, decltype(y)>);

                if constexpr (std::is_same_v<T, boolean_t>) {
                    return (x || y);
                } else if constexpr (std::is_same_v<T, address_t>) {
                    return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(x) | reinterpret_cast<uintptr_t>(y));
                } else {
                    return static_cast<T>(x | y);
                }
            });
}

// xor: any any :- any
// Bitwise XOR (unsigned integers, addresses) or logical XOR (booleans).
// throws: opstack-underflow, type-check
static void xor_op(Trix *trx) {
    shim_OUnsignedIntegersAddressBoolean_UnsignedIntegersAddressBoolean__RUnsignedIntegersAddressBoolean(
            trx, [](Trix *, auto x, auto y) {
                using T = decltype(x);
                static_assert(std::is_same_v<T, decltype(y)>);

                if constexpr (std::is_same_v<T, boolean_t>) {
                    return (x != y);
                } else if constexpr (std::is_same_v<T, address_t>) {
                    return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(x) ^ reinterpret_cast<uintptr_t>(y));
                } else {
                    return static_cast<T>(x ^ y);
                }
            });
}

// not: any :- any
// Bitwise NOT (unsigned integers, addresses) or logical NOT (booleans).
// throws: opstack-underflow, type-check
static void not_op(Trix *trx) {
    shim_OUnsignedIntegersAddressBoolean__RUnsignedIntegersAddressBoolean(trx, [](Trix *, auto x) {
        using T = decltype(x);

        if constexpr (std::is_same_v<T, boolean_t>) {
            return !x;
        } else if constexpr (std::is_same_v<T, address_t>) {
            return reinterpret_cast<T>(~reinterpret_cast<uintptr_t>(x));
        } else {
            return static_cast<T>(~x);
        }
    });
}
