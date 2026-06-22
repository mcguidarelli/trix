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

//===--- Math, Comparison, and Numeric Operators ---===//
//
// Implements arithmetic, comparison, type promotion, numeric predicates,
// IEEE 754 operations, and C++ <cmath> wrappers.  Based on:
//
//   PostScript arithmetic: add, sub, mul, div, idiv, mod, neg, abs,
//   ceiling, floor, round, truncate, sqrt, exp, ln, log, sin, cos, atan.
//   Unlike PostScript, Trix is 100% type-safe: binary operators require
//   both operands to be the same type (no implicit promotion).  The explicit
//   `promote` operator is available for widening when needed.
//
//   IEEE 754-2019: full support for special values (NaN, Inf, -0),
//   classification (fpclassify, is-nan, is-inf, is-finite, is-normal),
//   rounding modes (fe-set-round, fe-get-round), NaN payloads, and the
//   complete C++23 <cmath> special functions (Bessel, elliptic integrals,
//   Legendre, Laguerre, Hermite, etc.).
//
//   Comparison: eq, ne, lt, le, gt, ge.  eq/ne accept any two types
//   (different types are simply not equal); lt/le/gt/ge require matched
//   types.  `deep-eq` for recursive structural comparison of containers
//   (arrays, records, sets, dicts, tagged values).
//
// --- Core concepts for maintainers ---
//
// TYPE-SAFE DISPATCH (shim system)
//   Binary operations (add, sub, mul, etc.) are dispatched through the
//   shim system (shims.inl).  Each operator has one shim function per
//   valid type.  Both operands must be the same type -- the shim calls
//   verify_matched_operands (MatchPolicy::AllMustMatch) which raises
//   TypeCheck if the types differ.  There is no implicit promotion.
//
//   This avoids runtime type-checking loops: the dispatch is a computed
//   function pointer call, O(1) per operation.
//
// EXTVALUE MANAGEMENT
//   Operations producing 64-bit results (Long, ULong, Double, Address)
//   allocate ExtValues.  Operand ExtValues are freed after use.  The shim
//   functions handle this consistently: read input ExtValues, compute
//   result, free input ExtValues, allocate output ExtValue if needed.
//
// DIVISION BY ZERO
//   Both integer and floating-point division by zero raise
//   Error::DivByZero (error name "div-by-zero").  Trix does not
//   return +/-Inf for x/0.  div rejects a zero divisor for every numeric
//   type; the integer-only mod and the float fmod/remainder reject one too.
//
// --- Categories ---
//
// Comparison:    eq, ne, lt, le, gt, ge, min, max, clamp
//                (deep-eq/deep-ne implemented in ops_convert.inl)
// Arithmetic:    add, sub, mul, div, mod, neg, abs, sign
// Rounding:      ceil, floor, round, trunc
// Powers/logs:   sqrt, cbrt, exp, exp2, expm1, log, log2, log10, log1p, pow, hypot
// Trigonometry:  sin, cos, tan, asin, acos, atan, atan2
// Hyperbolic:    sinh, cosh, tanh, asinh, acosh, atanh
// Special:       erf, erfc, tgamma, lgamma, beta, Bessel, Legendre,
//                Laguerre, Hermite, elliptic integrals, Riemann zeta
// IEEE 754:      is-nan, is-inf, is-finite, is-normal, fpclassify,
//                copy-sign, next-after, fma, remainder, fmod,
//                scalbn, logb, ilogb, ldexp, frexp, modf, nan-payload,
//                nan-with-payload, fe-set-round, fe-get-round,
//                fe-raise-except, fe-clear-except, fe-test-except,
//                round-even, nearby-int
// Bitwise:       (see ops_bitwise.inl)
// Random:        rand-seed, rand-uinteger, rand-bounded-uinteger, rand-ulong,
//                rand-bounded-ulong, rand-int128, rand-uint128,
//                rand-bounded-uint128, rand-real, rand-double
//                (PCG32 engine in pcg32.inl; ops in ops_system.inl)
// Numeric pred:  is-number, is-integer, dot-product, between?, prime?
//                (is-number/is-integer are type predicates; between?
//                and prime? are value predicates defined here)
//

// eq: any any :- bool
// Tests equality of the top two operands.
// throws: opstack-underflow
static void eq_op(Trix *trx) {
    compare_eq_impl(trx, false);
}

// lcm: int int :- int
// Computes the least common multiple.
// throws: numerical-overflow, opstack-underflow, type-check
static void lcm_op(Trix *trx) {
    shim_OIntegers_Integers__RIntegers(trx, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, int128_t> || std::is_same_v<T, uint128_t>) {
            if constexpr (std::is_same_v<T, int128_t>) {
                if ((x == std::numeric_limits<int128_t>::min()) || (y == std::numeric_limits<int128_t>::min())) {
                    p->error(Error::NumericalOverflow, "lcm: argument is the minimum int128 value");
                }
            }
            if ((x == 0) || (y == 0)) {
                return T{0};
            } else {
                // compute gcd on absolute magnitudes
                uint128_t ax;
                uint128_t ay;
                if constexpr (std::is_same_v<T, int128_t>) {
                    ax = (x < 0) ? static_cast<uint128_t>(-x) : static_cast<uint128_t>(x);
                    ay = (y < 0) ? static_cast<uint128_t>(-y) : static_cast<uint128_t>(y);
                } else {
                    ax = x;
                    ay = y;
                }
                uint128_t a = ax;
                uint128_t b = ay;
                while (b != 0) {
                    auto r = a % b;
                    a = b;
                    b = r;
                }
                auto g = a;
                auto quot = ax / g;
                uint128_t product;
                if (__builtin_mul_overflow(quot, ay, &product)) {
                    p->error(Error::NumericalOverflow, "lcm: result overflows");
                } else {
                    if constexpr (std::is_same_v<T, int128_t>) {
                        if (product > static_cast<uint128_t>(std::numeric_limits<int128_t>::max())) {
                            p->error(Error::NumericalOverflow, "lcm: result overflows int128");
                        }
                    }
                    return static_cast<T>(product);
                }
            }
        } else {
            if constexpr (std::is_signed_v<T>) {
                if ((x == std::numeric_limits<T>::min()) || (y == std::numeric_limits<T>::min())) {
                    p->error(Error::NumericalOverflow, "lcm: argument is the minimum integer value");
                }
            }
            if ((x == 0) || (y == 0)) {
                return T{0};
            } else {
                auto g = std::gcd(x, y);
                const T quot = static_cast<T>(x / g);
                T result;
                if (__builtin_mul_overflow(quot, y, &result)) {
                    p->error(Error::NumericalOverflow, "lcm: result overflows");
                } else {
                    if constexpr (std::is_signed_v<T>) {
                        if (result < 0) {
                            result = static_cast<T>(-result);
                        }
                    }
                    return result;
                }
            }
        }
    });
}

// midpoint: num num :- num
// Computes the midpoint of two values.
// throws: opstack-underflow, type-check
static void midpoint_op(Trix *trx) {
    shim_ONumbers_Numbers__RNumbers(trx, VerifyIsFinite, VerifyIsFinite, [](Trix *, auto x, auto y) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, uint128_t>) {
            // floor((x+y)/2) without overflow
            return static_cast<uint128_t>((x / 2) + (y / 2) + (x & y & uint128_t{1}));
        } else if constexpr (std::is_same_v<T, int128_t>) {
            // rounds toward x, matching std::midpoint for signed integers.
            // Use unsigned intermediates to avoid overflow when y - x would
            // exceed int128 range (e.g. INT128_MIN -> INT128_MAX).
            using U = uint128_t;
            if (x <= y) {
                return static_cast<int128_t>(static_cast<U>(x) + ((static_cast<U>(y) - static_cast<U>(x)) / 2));
            } else {
                return static_cast<int128_t>(static_cast<U>(x) - ((static_cast<U>(x) - static_cast<U>(y)) / 2));
            }
        } else {
            return std::midpoint(x, y);
        }
    });
}

// mod: int int :- int
// Computes integer modulo.
// throws: numerical-overflow, opstack-underflow, type-check, undefined-result
static void mod_op(Trix *trx) {
    shim_OIntegers_Integers__RIntegers(trx, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        static_assert(std::is_same_v<T, decltype(y)>);

        if (y == 0) {
            p->error(Error::DivByZero, "mod: division by zero");
        } else {
            if constexpr (std::is_same_v<T, int128_t>) {
                if ((y == int128_t{-1}) && (x == std::numeric_limits<int128_t>::min())) {
                    p->error(Error::NumericalOverflow, "mod: int128 modulo overflow");
                }
            } else if constexpr (std::is_signed_v<T>) {
                if ((y == T{-1}) && (x == std::numeric_limits<T>::min())) {
                    p->error(Error::NumericalOverflow, "mod: signed modulo overflow");
                }
            }
            return static_cast<T>(x % y);
        }
    });
}

// ne: any any :- bool
// Tests inequality of the top two operands.
// throws: opstack-underflow
static void ne_op(Trix *trx) {
    compare_eq_impl(trx, true);
}

// neg: num :- num
// Negates a signed integer or float.
// throws: numerical-overflow, opstack-underflow, type-check
static void neg_op(Trix *trx) {
    shim_OSignedIntegersFloats__RSignedIntegersFloats(trx, [](Trix *p, auto x) {
        using T = decltype(x);

        if constexpr (std::is_integral_v<T>) {
            if (x == std::numeric_limits<T>::min()) {
                p->error(Error::NumericalOverflow, "neg: cannot negate minimum integer value");
            } else {
                return -x;
            }
        } else if constexpr (std::is_same_v<T, int128_t>) {
            if (x == std::numeric_limits<int128_t>::min()) {
                p->error(Error::NumericalOverflow, "neg: cannot negate minimum int128 value");
            } else {
                return static_cast<int128_t>(-x);
            }
        } else {
            return -x;
        }
    });
}

// promote: num1 num2 promote -- num1' num2'
//          arr/packed promote -- arr'
// Pairwise: widens both operands to their common numeric type.
// Array: widens all numeric elements to the common type across the array.
// Errors on non-numeric elements in the array form.
// No-op when all operands are already the same type.
// throws: opstack-underflow, type-check, vm-full
static void promote_op(Trix *trx) {
    trx->require_op_count(1);

    auto top_ptr = trx->m_op_ptr;

    if (top_ptr->is_sequence()) {
        // array/packed form: promote all elements to common type
        auto container_ptr = top_ptr;
        auto count = container_ptr->arrays_length();

        if (count <= 1) {
            // 0 or 1 elements: nothing to promote, return copy
            if (count == 0) {
                auto [_, offset] = trx->vm_alloc_dispatch_n<Object>(0, ChunkKind::Array);
                *trx->m_op_ptr = Object::make_array(offset, 0);
            } else {
                // Null + root the result before the clone: under ${...} dst is a global
                // block (gvm_for_each walks it on the clone's GC -- so null first to avoid
                // a garbage-offset walk) and is not yet on any stack (so root it to avoid
                // being swept as unreachable).
                auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(1, ChunkKind::Array);
                dst_ptr[0] = Object::make_null();
                auto result_obj = Object::make_array(dst_offset, 1);
                trx->gc_root_push_oneoff(result_obj);
                clone_array_elements(trx, container_ptr, dst_ptr, 1);
                trx->gc_root_pop_oneoff();
                if (!dst_ptr[0].is_number()) {
                    dst_ptr[0].maybe_free_extvalue(trx);
                    trx->error(Error::TypeCheck, "promote: array contains non-numeric element");
                } else {
                    container_ptr->maybe_free_extvalue(trx);
                    *trx->m_op_ptr = result_obj;
                }
            }
            return;
        } else {
            // expand packed to temp buffer or get array pointer
            Object *src_ptr{nullptr};
            Object *src_buf{nullptr};
            if (container_ptr->is_packed()) {
                auto [buf, _] = trx->vm_alloc_n<Object>(count);
                // Expand LOCALLY: clone_array_elements would otherwise materialize global
                // ExtValues under ${...} into this unrooted local buffer, and a later GC
                // (e.g. from the global result alloc / fill below) would sweep them.  Local
                // scratch clones are safe (the global GC never sweeps local VM) and are freed
                // below; mirrors expand_packed_to_temp's suppress-global pattern.
                auto saved_global = trx->m_curr_alloc_global;
                trx->m_curr_alloc_global = false;
                clone_array_elements(trx, container_ptr, buf, count);
                trx->m_curr_alloc_global = saved_global;
                src_ptr = buf;
                src_buf = buf;
            } else {
                src_ptr = container_ptr->array_objects(trx);
            }

            // pass 1: find common type across all elements
            auto common = src_ptr[0].type();
            if (!src_ptr[0].is_number()) {
                if (src_buf != nullptr) {
                    for (length_t i = 0; i < count; ++i) {
                        src_buf[i].maybe_free_extvalue(trx);
                    }
                }
                trx->error(Error::TypeCheck, "promote: array contains non-numeric element");
            } else {
                for (length_t i = 1; i < count; ++i) {
                    if (!src_ptr[i].is_number()) {
                        if (src_buf != nullptr) {
                            for (length_t j = 0; j < count; ++j) {
                                src_buf[j].maybe_free_extvalue(trx);
                            }
                        }
                        trx->error(Error::TypeCheck, "promote: array contains non-numeric element");
                    } else {
                        common = promote_common_type(common, src_ptr[i].type());
                    }
                }

                // pass 2: allocate result and convert each element.  Null + root the result
                // across the per-element clones: under ${...} the result is a global block, so
                // null first (gvm_for_each walks it on each clone's GC) and root it (it is not
                // yet on any stack).  src_buf, if present, holds LOCAL scratch clones -- safe.
                auto [dst_ptr, dst_offset] = trx->vm_alloc_dispatch_n<Object>(count, ChunkKind::Array);
                std::fill_n(dst_ptr, count, Object::make_null());
                auto result_obj = Object::make_array(dst_offset, count);
                trx->gc_root_push_oneoff(result_obj);
                for (length_t i = 0; i < count; ++i) {
                    dst_ptr[i] = src_ptr[i].make_clone(trx);
                    if (dst_ptr[i].type() != common) {
                        promote_convert(trx, &dst_ptr[i], common);
                    }
                }
                trx->gc_root_pop_oneoff();

                // free temp buffer from packed expansion
                if (src_buf != nullptr) {
                    for (length_t i = 0; i < count; ++i) {
                        src_buf[i].maybe_free_extvalue(trx);
                    }
                }

                container_ptr->maybe_free_extvalue(trx);
                *trx->m_op_ptr = result_obj;
            }
        }
    } else {
        // pairwise form: num1 num2 promote -- num1' num2'
        trx->verify_operands(VerifyNumbers, VerifyNumbers);

        auto y_ptr = trx->m_op_ptr;
        auto x_ptr = (y_ptr - 1);

        auto x_type = x_ptr->type();
        auto y_type = y_ptr->type();

        if (x_type != y_type) {
            auto common = promote_common_type(x_type, y_type);

            if (x_type != common) {
                promote_convert(trx, x_ptr, common);
            }
            if (y_type != common) {
                promote_convert(trx, y_ptr, common);
            }
        }
    }
}

// single-bit?: uint :- bool
// Tests if the value is an exact power of two.
// throws: opstack-underflow, type-check
static void has_single_bit_op(Trix *trx) {
    shim_OUnsignedIntegers__RBoolean(trx, [](Trix *, auto x) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, uint128_t>) {
            return ((x != T{0}) && ((x & (x - T{1})) == T{0}));
        } else {
            return std::has_single_bit(x);
        }
    });
}

// abs: num :- num
// Absolute value of a signed integer or float.
// throws: numerical-overflow, opstack-underflow, type-check
static void abs_op(Trix *trx) {
    shim_OSignedIntegersFloats__RSignedIntegersFloats(trx, [](Trix *p, auto x) {
        using T = decltype(x);

        if constexpr (std::is_integral_v<T>) {
            if (x == std::numeric_limits<T>::min()) {
                p->error(Error::NumericalOverflow, "abs: cannot negate minimum integer value");
            } else {
                return std::abs(x);
            }
        } else if constexpr (std::is_same_v<T, int128_t>) {
            if (x == std::numeric_limits<int128_t>::min()) {
                p->error(Error::NumericalOverflow, "abs: cannot negate minimum int128 value");
            } else {
                return static_cast<int128_t>((x < 0) ? -x : x);
            }
        } else {
            return std::abs(x);
        }
    });
}

// sign: number :- int
// Returns -1, 0, or 1 indicating the sign of the operand.
// throws: opstack-underflow, type-check
static void sign_op(Trix *trx) {
    trx->verify_operands(VerifyNumbers);

    auto val_ptr = trx->m_op_ptr;
    auto result = integer_t{0};

    switch (+val_ptr->type()) {
    case +Object::Type::Byte:
        result = (val_ptr->byte_value() != 0) ? 1 : 0;
        break;

    case +Object::Type::Integer: {
        auto value = val_ptr->integer_value();
        result = (value > 0) ? 1 : (value < 0) ? -1 : 0;
        break;
    }

    case +Object::Type::UInteger:
        result = (val_ptr->uinteger_value() != 0) ? 1 : 0;
        break;

    case +Object::Type::Long: {
        auto value = val_ptr->long_value(trx);
        result = (value > 0) ? 1 : (value < 0) ? -1 : 0;
        break;
    }

    case +Object::Type::ULong:
        result = (val_ptr->ulong_value(trx) != 0) ? 1 : 0;
        break;

    case +Object::Type::Int128: {
        auto value = val_ptr->int128_value(trx);
        result = (value > 0) ? 1 : (value < 0) ? -1 : 0;
        break;
    }

    case +Object::Type::UInt128:
        result = (val_ptr->uint128_value(trx) != 0) ? 1 : 0;
        break;

    case +Object::Type::Real: {
        auto value = val_ptr->real_value();
        result = (value > 0) ? 1 : (value < 0) ? -1 : 0;
        break;
    }

    case +Object::Type::Double: {
        auto value = val_ptr->double_value(trx);
        result = (value > 0) ? 1 : (value < 0) ? -1 : 0;
        break;
    }

    default:
        std::unreachable();
    }

    val_ptr->maybe_free_extvalue(trx);
    *val_ptr = Object::make_integer(result);
}

// gcd: int int :- int
// Computes the greatest common divisor.
// throws: numerical-overflow, opstack-underflow, type-check
static void gcd_op(Trix *trx) {
    shim_OIntegers_Integers__RIntegers(trx, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        if constexpr (std::is_same_v<T, int128_t> || std::is_same_v<T, uint128_t>) {
            uint128_t a;
            uint128_t b;
            if constexpr (std::is_same_v<T, int128_t>) {
                if ((x == std::numeric_limits<int128_t>::min()) || (y == std::numeric_limits<int128_t>::min())) {
                    p->error(Error::NumericalOverflow, "gcd: argument is the minimum int128 value");
                } else {
                    a = (x < 0) ? static_cast<uint128_t>(-x) : static_cast<uint128_t>(x);
                    b = (y < 0) ? static_cast<uint128_t>(-y) : static_cast<uint128_t>(y);
                }
            } else {
                a = x;
                b = y;
            }
            // Euclidean on absolute values (std::gcd does not accept __int128)
            while (b != 0) {
                auto r = a % b;
                a = b;
                b = r;
            }
            return static_cast<T>(a);
        } else {
            if constexpr (std::is_signed_v<T>) {
                if ((x == std::numeric_limits<T>::min()) || (y == std::numeric_limits<T>::min())) {
                    p->error(Error::NumericalOverflow, "gcd: argument is the minimum integer value");
                }
            }
            return std::gcd(x, y);
        }
    });
}

// factorial: int -- int
// Computes N!.  N must be non-negative.
// Limits: 5! for Byte, 12! for Integer, 12! for UInteger, 20! for Long/ULong.
// throws: opstack-underflow, type-check, range-check, numerical-overflow
static void factorial_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers);

    auto val_ptr = trx->m_op_ptr;
    auto type = val_ptr->type();

    switch (+type) {
    case +Object::Type::Byte: {
        auto n = val_ptr->byte_value();
        if (n > 5) {
            trx->error(Error::NumericalOverflow, "factorial: {}! overflows byte", n);
        } else {
            vm_t result = 1;
            for (vm_t i = 2; i <= n; ++i) {
                result = static_cast<vm_t>(result * i);
            }
            val_ptr->update_byte(result);
            return;
        }
    }

    case +Object::Type::Integer: {
        auto n = val_ptr->integer_value();
        if (n < 0) {
            trx->error(Error::RangeCheck, "factorial: argument {} is negative", n);
        } else if (n > 12) {
            trx->error(Error::NumericalOverflow, "factorial: {}! overflows integer", n);
        } else {
            integer_t result = 1;
            for (integer_t i = 2; i <= n; ++i) {
                result *= i;
            }
            val_ptr->update_integer(result);
            return;
        }
    }

    case +Object::Type::UInteger: {
        auto n = val_ptr->uinteger_value();
        if (n > 12) {
            trx->error(Error::NumericalOverflow, "factorial: {}! overflows uinteger", n);
        } else {
            uinteger_t result = 1;
            for (uinteger_t i = 2; i <= n; ++i) {
                result *= i;
            }
            val_ptr->update_uinteger(result);
            return;
        }
    }

    case +Object::Type::Long: {
        auto n = val_ptr->long_value(trx);
        if (n < 0) {
            trx->error(Error::RangeCheck, "factorial: argument {} is negative", n);
        } else if (n > 20) {
            trx->error(Error::NumericalOverflow, "factorial: {}! overflows long", n);
        } else {
            long_t result = 1;
            for (long_t i = 2; i <= n; ++i) {
                result *= i;
            }
            val_ptr->update_long(trx, result);
            return;
        }
    }

    case +Object::Type::ULong: {
        auto n = val_ptr->ulong_value(trx);
        if (n > 20) {
            trx->error(Error::NumericalOverflow, "factorial: {}! overflows ulong", n);
        } else {
            ulong_t result = 1;
            for (ulong_t i = 2; i <= n; ++i) {
                result *= i;
            }
            val_ptr->update_ulong(trx, result);
            return;
        }
    }

    case +Object::Type::Int128:
    case +Object::Type::UInt128:
        trx->error(Error::TypeCheck, "factorial: 128-bit arguments not supported; cast to ULong first");
    default:
        break;
    }
    trx->error(Error::TypeCheck, "factorial: unsupported integer type");
}

// nCr: n r -- int
// Binomial coefficient "n choose r".  Both arguments must be non-negative, r <= n.
// Computes iteratively with alternating multiply/divide to minimize overflow.
// throws: opstack-underflow, type-check, range-check, numerical-overflow
static void ncr_op(Trix *trx) {
    shim_OIntegers_Integers__RIntegers(trx, [](Trix *p, auto n, auto r) {
        using T = decltype(n);

        if constexpr (std::is_signed_v<T>) {
            if ((n < 0) || (r < 0)) {
                p->error(Error::RangeCheck, "nCr: arguments must be non-negative");
            }
        }
        if (r > n) {
            p->error(Error::RangeCheck, "nCr: r ({}) must not exceed n ({})", r, n);
        } else {
            // symmetry: nCr(n, r) == nCr(n, n-r)
            if (r > (n - r)) {
                r = static_cast<T>(n - r);
            }

            // iterative: result = n * (n-1) * ... * (n-r+1) / (1 * 2 * ... * r)
            T result = 1;
            for (T i = 0; i < r; ++i) {
                // multiply first, then divide -- division is always exact
                if constexpr (std::is_same_v<T, int128_t> || std::is_same_v<T, uint128_t>) {
                    T product;
                    if (__builtin_mul_overflow(result, static_cast<T>(n - i), &product)) {
                        p->error(Error::NumericalOverflow, "nCr: result overflows 128-bit");
                    } else {
                        result = static_cast<T>(product / static_cast<T>(i + 1));
                    }
                } else if constexpr (std::is_same_v<T, vm_t>) {
                    auto product = static_cast<uinteger_t>(result) * static_cast<uinteger_t>(n - i);
                    if (product > std::numeric_limits<vm_t>::max()) {
                        p->error(Error::NumericalOverflow, "nCr: result overflows");
                    } else {
                        result = static_cast<vm_t>(product / static_cast<uinteger_t>(i + 1));
                    }
                } else if constexpr (std::is_same_v<T, integer_t>) {
                    integer_t product;
                    if (__builtin_smul_overflow(result, static_cast<integer_t>(n - i), &product)) {
                        p->error(Error::NumericalOverflow, "nCr: result overflows integer");
                    } else {
                        result = product / static_cast<integer_t>(i + 1);
                    }
                } else if constexpr (std::is_same_v<T, uinteger_t>) {
                    uinteger_t product;
                    if (__builtin_umul_overflow(result, static_cast<uinteger_t>(n - i), &product)) {
                        p->error(Error::NumericalOverflow, "nCr: result overflows uinteger");
                    } else {
                        result = product / static_cast<uinteger_t>(i + 1);
                    }
                } else if constexpr (std::is_same_v<T, long_t>) {
                    long_t product;
                    if (__builtin_smulll_overflow(result, static_cast<long_t>(n - i), &product)) {
                        p->error(Error::NumericalOverflow, "nCr: result overflows long");
                    } else {
                        result = product / static_cast<long_t>(i + 1);
                    }
                } else if constexpr (std::is_same_v<T, ulong_t>) {
                    ulong_t product;
                    if (__builtin_umulll_overflow(result, static_cast<ulong_t>(n - i), &product)) {
                        p->error(Error::NumericalOverflow, "nCr: result overflows ulong");
                    } else {
                        result = product / static_cast<ulong_t>(i + 1);
                    }
                }
            }
            return result;
        }
    });
}

// prime?: int -- bool
// Deterministic Miller-Rabin primality test.  Correct for all values up to 2^64.
// Input must be a non-negative integer.  0 and 1 are not prime.
// throws: opstack-underflow, type-check, range-check
static void prime_pred_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers);

    auto val_ptr = trx->m_op_ptr;
    auto type = val_ptr->type();

    // extract as ulong_t regardless of input type
    ulong_t n = 0;
    switch (+type) {
    case +Object::Type::Byte:
        n = val_ptr->byte_value();
        break;

    case +Object::Type::Integer: {
        auto v = val_ptr->integer_value();
        if (v < 0) {
            trx->error(Error::RangeCheck, "prime?: argument {} is negative", v);
        } else {
            n = static_cast<ulong_t>(v);
            break;
        }
    }

    case +Object::Type::UInteger:
        n = val_ptr->uinteger_value();
        break;

    case +Object::Type::Long: {
        auto v = val_ptr->long_value(trx);
        if (v < 0) {
            trx->error(Error::RangeCheck, "prime?: argument {} is negative", v);
        } else {
            val_ptr->maybe_free_extvalue(trx);
            n = static_cast<ulong_t>(v);
            break;
        }
    }

    case +Object::Type::ULong:
        n = val_ptr->ulong_value(trx);
        val_ptr->maybe_free_extvalue(trx);
        break;

    case +Object::Type::Int128:
    case +Object::Type::UInt128:
        trx->error(Error::TypeCheck,
                   "prime?: 128-bit arguments not supported (Miller-Rabin requires 256-bit intermediates); "
                   "cast to ULong first");
    default:
        trx->error(Error::TypeCheck, "prime?: unsupported integer type");
    }

    // small cases
    if (n < 2) {
        *val_ptr = Object::make_boolean(false);
    } else if (n < 4) {
        *val_ptr = Object::make_boolean(true);
    } else if (((n % 2) == 0) || ((n % 3) == 0)) {
        *val_ptr = Object::make_boolean(false);
    } else {
        // trial division for small factors up to 37
        for (ulong_t d = 5; ((d * d) <= n) && (d <= 37); d += 6) {
            if (((n % d) == 0) || ((n % (d + 2)) == 0)) {
                *val_ptr = Object::make_boolean(false);
                return;
            }
        }

        // for values <= 1000, trial division above is sufficient
        if (n <= 1000) {
            *val_ptr = Object::make_boolean(true);
        } else {
            // modular exponentiation: base^exp mod m using __uint128_t
            auto mod_pow = [](ulong_t base, ulong_t exp, ulong_t mod) -> ulong_t {
                ulong_t result = 1;
                base %= mod;
                while (exp > 0) {
                    if ((exp & 1) != 0) {
                        result = static_cast<ulong_t>((__uint128_t(result) * base) % mod);
                    }
                    exp >>= 1;
                    base = static_cast<ulong_t>((__uint128_t(base) * base) % mod);
                }
                return result;
            };

            // Miller-Rabin: write n-1 = d * 2^r
            auto d = n - 1;
            int r = 0;
            while ((d & 1) == 0) {
                d >>= 1;
                ++r;
            }

            // deterministic witnesses: correct for all n < 2^64
            // (Sorenson & Webster, 2015)
            static constexpr ulong_t witnesses[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};

            auto is_prime = true;
            for (auto a : witnesses) {
                if (a < n) {
                    auto x = mod_pow(a, d, n);
                    if ((x != 1) && (x != (n - 1))) {
                        auto found_minus_one = false;
                        for (int i = 0; i < (r - 1); ++i) {
                            x = static_cast<ulong_t>((__uint128_t(x) * x) % n);
                            if (x == (n - 1)) {
                                found_minus_one = true;
                                break;
                            }
                        }
                        if (!found_minus_one) {
                            is_prime = false;
                            break;
                        }
                    }
                    // else x == 1 or x == n-1: this witness attests probable primality
                }
                // else witness a >= n contributes nothing for this n
            }

            *val_ptr = Object::make_boolean(is_prime);
        }
    }
}

// pow-mod: base exp mod -- int
// Modular exponentiation: base^exp mod m.  All three must be non-negative integers.
// mod must be > 0.  Uses __uint128_t for intermediate products.
// throws: opstack-underflow, type-check, range-check
static void powmod_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers, VerifyIntegers, VerifyIntegers);

    auto mod_ptr = trx->m_op_ptr;
    auto exp_ptr = (mod_ptr - 1);
    auto base_ptr = (exp_ptr - 1);

    auto mod_type = mod_ptr->type();
    auto exp_type = exp_ptr->type();
    auto base_type = base_ptr->type();

    // extract all three as ulong_t, validate non-negative
    auto extract = [trx](const Object *obj, Object::Type t, const char *name) -> ulong_t {
        switch (+t) {
        case +Object::Type::Byte:
            return obj->byte_value();

        case +Object::Type::Integer: {
            auto v = obj->integer_value();
            if (v < 0) {
                trx->error(Error::RangeCheck, "pow-mod: {} must be non-negative", name);
            } else {
                return static_cast<ulong_t>(v);
            }
        }

        case +Object::Type::UInteger:
            return obj->uinteger_value();

        case +Object::Type::Long: {
            auto v = obj->long_value(trx);
            if (v < 0) {
                trx->error(Error::RangeCheck, "pow-mod: {} must be non-negative", name);
            } else {
                return static_cast<ulong_t>(v);
            }
        }

        case +Object::Type::ULong:
            return obj->ulong_value(trx);

        case +Object::Type::Int128:
        case +Object::Type::UInt128:
            trx->error(Error::TypeCheck,
                       "pow-mod: 128-bit {} not supported (modular exponentiation requires 256-bit intermediates); "
                       "cast to ULong first",
                       name);
        default:
            trx->error(Error::TypeCheck, "pow-mod: unsupported integer type for {}", name);
        }
    };

    auto base_val = extract(base_ptr, base_type, "base");
    auto exp_val = extract(exp_ptr, exp_type, "exponent");
    auto mod_val = extract(mod_ptr, mod_type, "modulus");

    if (mod_val == 0) {
        trx->error(Error::RangeCheck, "pow-mod: modulus must be > 0");
    } else {
        // modular exponentiation using __uint128_t
        ulong_t result = 1;
        base_val %= mod_val;
        while (exp_val > 0) {
            if ((exp_val & 1) != 0) {
                result = static_cast<ulong_t>((__uint128_t(result) * base_val) % mod_val);
            }
            exp_val >>= 1;
            base_val = static_cast<ulong_t>((__uint128_t(base_val) * base_val) % mod_val);
        }

        // Validate the result against the base operand's type and construct the
        // result object BEFORE freeing the operands' ExtValues: an error raised
        // after the frees (result overflow here, or vm-full from the Long/ULong
        // ExtValue allocation) would leave freed ExtValue objects on the operand
        // stack for @try-rollback to free a second time.
        Object result_obj{};
        switch (+base_type) {
        case +Object::Type::Byte:
            if (result > std::numeric_limits<vm_t>::max()) {
                trx->error(Error::NumericalOverflow, "pow-mod: result {} overflows byte", result);
            } else {
                result_obj = Object::make_byte(static_cast<vm_t>(result));
                break;
            }

        case +Object::Type::Integer:
            if (result > static_cast<ulong_t>(std::numeric_limits<integer_t>::max())) {
                trx->error(Error::NumericalOverflow, "pow-mod: result {} overflows integer", result);
            } else {
                result_obj = Object::make_integer(static_cast<integer_t>(result));
                break;
            }

        case +Object::Type::UInteger:
            if (result > std::numeric_limits<uinteger_t>::max()) {
                trx->error(Error::NumericalOverflow, "pow-mod: result {} overflows uinteger", result);
            } else {
                result_obj = Object::make_uinteger(static_cast<uinteger_t>(result));
                break;
            }

        case +Object::Type::Long:
            if (result > static_cast<ulong_t>(std::numeric_limits<long_t>::max())) {
                trx->error(Error::NumericalOverflow, "pow-mod: result {} overflows long", result);
            } else {
                result_obj = Object::make_long(trx, static_cast<long_t>(result));
                break;
            }

        case +Object::Type::ULong:
            result_obj = Object::make_ulong(trx, result);
            break;

        default:
            trx->error(Error::TypeCheck, "pow-mod: unsupported result type");
        }

        // free ExtValues and replace the three operands with the result
        mod_ptr->maybe_free_extvalue(trx);
        exp_ptr->maybe_free_extvalue(trx);
        base_ptr->maybe_free_extvalue(trx);
        trx->m_op_ptr -= 2;
        *trx->m_op_ptr = result_obj;
    }
}

// isqrt: int -- int
// Integer square root (floor of sqrt(n)).  Uses Newton's method in integer arithmetic.
// Input must be non-negative.
// throws: opstack-underflow, type-check, range-check
static void isqrt_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers);

    auto val_ptr = trx->m_op_ptr;
    auto type = val_ptr->type();

    switch (+type) {
    case +Object::Type::Byte: {
        auto n = val_ptr->byte_value();
        // byte: max 255, sqrt(255) = 15
        vm_t x = static_cast<vm_t>(std::sqrt(static_cast<double>(n)));
        // correct for floating-point imprecision
        while ((x * x) > n) {
            --x;
        }
        while (((x + 1) * (x + 1)) <= n) {
            ++x;
        }
        val_ptr->update_byte(x);
        return;
    }

    case +Object::Type::Integer: {
        auto n = val_ptr->integer_value();
        if (n < 0) {
            trx->error(Error::RangeCheck, "isqrt: argument {} is negative", n);
        } else {
            // int32: max sqrt is 46340
            auto x = static_cast<integer_t>(std::sqrt(static_cast<double>(n)));
            // use long_t to avoid overflow in x*x near int32 max
            auto n64 = long_t{n};
            while ((static_cast<long_t>(x) * x) > n64) {
                --x;
            }
            while ((static_cast<long_t>(x + 1) * (x + 1)) <= n64) {
                ++x;
            }
            val_ptr->update_integer(x);
            return;
        }
    }

    case +Object::Type::UInteger: {
        auto n = val_ptr->uinteger_value();
        auto x = static_cast<uinteger_t>(std::sqrt(static_cast<double>(n)));
        // use ulong_t to avoid overflow in (x+1)*(x+1) near uint32 max
        auto n64 = ulong_t{n};
        while ((static_cast<ulong_t>(x) * x) > n64) {
            --x;
        }
        while ((static_cast<ulong_t>(x + 1) * (x + 1)) <= n64) {
            ++x;
        }
        val_ptr->update_uinteger(x);
        return;
    }

    case +Object::Type::Long: {
        auto n = val_ptr->long_value(trx);
        if (n < 0) {
            trx->error(Error::RangeCheck, "isqrt: argument {} is negative", n);
        } else {
            // Newton's method for int64 (double lacks precision above 2^53)
            if (n == 0) {
                val_ptr->update_long(trx, 0);
                return;
            } else {
                auto un = static_cast<ulong_t>(n);
                auto x = static_cast<ulong_t>(std::sqrt(static_cast<double>(un)));
                // Newton refinement -- converges in 1-2 iterations
                while (true) {
                    auto x1 = (x + un / x) / 2;
                    if (x1 >= x) {
                        break;
                    } else {
                        x = x1;
                    }
                }
                // use __uint128_t to avoid overflow in x*x near uint64 max
                while ((__uint128_t(x) * x) > un) {
                    --x;
                }
                val_ptr->update_long(trx, static_cast<long_t>(x));
                return;
            }
        }
    }

    case +Object::Type::ULong: {
        auto n = val_ptr->ulong_value(trx);
        if (n == 0) {
            val_ptr->update_ulong(trx, 0);
            return;
        } else {
            auto x = static_cast<ulong_t>(std::sqrt(static_cast<double>(n)));
            while (true) {
                auto x1 = (x + n / x) / 2;
                if (x1 >= x) {
                    break;
                } else {
                    x = x1;
                }
            }
            // use __uint128_t to avoid overflow in x*x near uint64 max
            while ((__uint128_t(x) * x) > n) {
                --x;
            }
            val_ptr->update_ulong(trx, x);
            return;
        }
    }

    case +Object::Type::Int128:
    case +Object::Type::UInt128:
        trx->error(Error::TypeCheck, "isqrt: 128-bit arguments not supported; cast to ULong first");
    default:
        break;
    }
    trx->error(Error::TypeCheck, "isqrt: unsupported integer type");
}

// lerp: num num num :- num
// Linear interpolation: lerp(a, b, t).
// throws: opstack-underflow, type-check
static void lerp_op(Trix *trx) {
    shim_OFloats_Floats_Floats__RFloats(trx, [](Trix *, auto x, auto y, auto t) { return std::lerp(x, y, t); });
}

// fma: num num num :- num
// Fused multiply-add: (x * y) + z.
// throws: opstack-underflow, type-check
static void fma_op(Trix *trx) {
    shim_OFloats_Floats_Floats__RFloats(trx, [](Trix *, auto x, auto y, auto z) { return std::fma(x, y, z); });
}

// fdim: num num :- num
// Positive difference: max(x - y, 0).
// throws: opstack-underflow, type-check
static void fdim_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::fdim(x, y); });
}

// fmin: num num :- num
// Returns the smaller of two floats (NaN-safe).
// throws: opstack-underflow, type-check
static void fmin_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::fmin(x, y); });
}

// fmax: num num :- num
// Returns the larger of two floats (NaN-safe).
// throws: opstack-underflow, type-check
static void fmax_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::fmax(x, y); });
}

// fmod: num num :- num
// Floating-point remainder of x / y.
// throws: opstack-underflow, type-check, div-by-zero, undefined-result
static void fmod_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *p, auto x, auto y) {
        if (std::fpclassify(y) == FP_ZERO) {
            p->error(Error::DivByZero, "fmod: division by zero");
        } else if (std::fpclassify(x) == FP_INFINITE) {
            p->error(Error::UndefinedResult, "fmod: infinity mod");
        } else {
            return std::fmod(x, y);
        }
    });
}

// remainder: num num :- num
// IEEE remainder of x / y.
// throws: opstack-underflow, type-check, div-by-zero, undefined-result
static void remainder_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *p, auto x, auto y) {
        // Report modulus-by-zero as /div-by-zero (matching fmod), distinct from
        // the /undefined-result raised when the IEEE remainder is NaN for other
        // domain reasons (e.g. remainder(inf, y)).
        if (std::fpclassify(y) == FP_ZERO) {
            p->error(Error::DivByZero, "remainder: division by zero");
        } else {
            auto result = std::remainder(x, y);
            if (std::fpclassify(result) == FP_NAN) {
                p->error(Error::UndefinedResult, "remainder: result is NaN");
            } else {
                return result;
            }
        }
    });
}

// copysign: num num :- num
// Returns x with the sign of y.
// throws: opstack-underflow, type-check
static void copysign_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::copysign(x, y); });
}

// nextafter: num num :- num
// Next representable value after x toward y.
// throws: opstack-underflow, type-check
static void nextafter_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::nextafter(x, y); });
}

// nexttoward: num num :- num
// Next representable value after x toward y (long double).
// throws: opstack-underflow, type-check
static void nexttoward_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::nexttoward(x, static_cast<long double>(y)); });
}

// pow: num num :- num
// Raises x to the power y.  Rejects out-of-domain combinations: a negative base
// with a non-integer exponent, and a zero base with a negative exponent.
// throws: opstack-underflow, type-check, range-check
static void pow_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *p, auto x, auto y) {
        if ((x < 0) && std::isfinite(y) && (y != std::trunc(y))) {
            p->error(Error::RangeCheck, "pow: negative base {} with non-integer exponent {}", x, y);
        } else if ((x == 0) && (y < 0)) {
            p->error(Error::RangeCheck, "pow: zero base with negative exponent {}", y);
        } else {
            return std::pow(x, y);
        }
    });
}

// hypot: num num :- num
// Computes sqrt(x^2 + y^2).
// throws: opstack-underflow, type-check
static void hypot_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::hypot(x, y); });
}

// beta: num num :- num
// Computes the beta function B(x, y).
// throws: opstack-underflow, type-check
static void beta_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::beta(x, y); });
}

// cyl-bessel-i: num num :- num
// Regular modified cylindrical Bessel function I.
// throws: opstack-underflow, type-check
static void cyl_bessel_i_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::cyl_bessel_i(x, y); });
}

// cyl-bessel-j: num num :- num
// Cylindrical Bessel function of the first kind J.
// throws: opstack-underflow, type-check
static void cyl_bessel_j_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::cyl_bessel_j(x, y); });
}

// cyl-bessel-k: num num :- num
// Irregular modified cylindrical Bessel function K.
// throws: opstack-underflow, type-check
static void cyl_bessel_k_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::cyl_bessel_k(x, y); });
}

// cyl-neumann: num num :- num
// Cylindrical Neumann function (Bessel Y).
// throws: opstack-underflow, type-check
static void cyl_neumann_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::cyl_neumann(x, y); });
}

// signbit: num :- bool
// Tests if the sign bit is set.
// throws: opstack-underflow, type-check
static void signbit_op(Trix *trx) {
    shim_OFloats__RBoolean(trx, [](Trix *, auto x) { return std::signbit(x); });
}

// isnormal: num :- bool
// Tests if the value is normal (not zero, subnormal, inf, or NaN).
// throws: opstack-underflow, type-check
static void isnormal_op(Trix *trx) {
    shim_OFloats__RBoolean(trx, [](Trix *, auto x) { return std::isnormal(x); });
}

// isnan: num :- bool
// Tests if the value is NaN.
// throws: opstack-underflow, type-check
static void isnan_op(Trix *trx) {
    shim_OFloats__RBoolean(trx, [](Trix *, auto x) { return std::isnan(x); });
}

// isinf: num :- bool
// Tests if the value is infinite.
// throws: opstack-underflow, type-check
static void isinf_op(Trix *trx) {
    shim_OFloats__RBoolean(trx, [](Trix *, auto x) { return std::isinf(x); });
}

// isfinite: num :- bool
// Tests if the value is finite.
// throws: opstack-underflow, type-check
static void isfinite_op(Trix *trx) {
    shim_OFloats__RBoolean(trx, [](Trix *, auto x) { return std::isfinite(x); });
}

// isgreater: num num :- bool
// Tests x > y (NaN-safe).
// throws: opstack-underflow, type-check
static void isgreater_op(Trix *trx) {
    shim_OFloats_Floats__RBoolean(trx, [](Trix *, auto x, auto y) { return std::isgreater(x, y); });
}

// isgreaterequal: num num :- bool
// Tests x >= y (NaN-safe).
// throws: opstack-underflow, type-check
static void isgreaterequal_op(Trix *trx) {
    shim_OFloats_Floats__RBoolean(trx, [](Trix *, auto x, auto y) { return std::isgreaterequal(x, y); });
}

// isless: num num :- bool
// Tests x < y (NaN-safe).
// throws: opstack-underflow, type-check
static void isless_op(Trix *trx) {
    shim_OFloats_Floats__RBoolean(trx, [](Trix *, auto x, auto y) { return std::isless(x, y); });
}

// islessequal: num num :- bool
// Tests x <= y (NaN-safe).
// throws: opstack-underflow, type-check
static void islessequal_op(Trix *trx) {
    shim_OFloats_Floats__RBoolean(trx, [](Trix *, auto x, auto y) { return std::islessequal(x, y); });
}

// islessgreater: num num :- bool
// Tests x < y or x > y (NaN-safe).
// throws: opstack-underflow, type-check
static void islessgreater_op(Trix *trx) {
    shim_OFloats_Floats__RBoolean(trx, [](Trix *, auto x, auto y) { return std::islessgreater(x, y); });
}

// isunordered: num num :- bool
// Tests if either operand is NaN.
// throws: opstack-underflow, type-check
static void isunordered_op(Trix *trx) {
    shim_OFloats_Floats__RBoolean(trx, [](Trix *, auto x, auto y) { return std::isunordered(x, y); });
}

// scalbn: num int :- num
// Multiplies float by 2^n using scalbn.
// throws: opstack-underflow, type-check
static void scalbn_op(Trix *trx) {
    shim_OFloats_Integer__RFloats(trx, [](Trix *, auto x, auto y) { return std::scalbn(x, y); });
}

// ldexp: num int :- num
// Multiplies float by 2^n using ldexp.
// throws: opstack-underflow, type-check
static void ldexp_op(Trix *trx) {
    shim_OFloats_Integer__RFloats(trx, [](Trix *, auto x, auto y) { return std::ldexp(x, y); });
}

// fpclassify: num :- name
// Returns the floating-point classification as a name.
// throws: opstack-underflow, type-check
static void fpclassify_op(Trix *trx) {
    constexpr auto X{VerifyFloats};
    trx->verify_operands(X);

    auto x_ptr = trx->m_op_ptr;
    int classification = -1;
    if (x_ptr->is_real()) {
        auto x_value = x_ptr->real_value();
        classification = std::fpclassify(x_value);
    } else {
        auto x_value = x_ptr->double_value(trx);
        x_ptr->free_extvalue(trx);
        classification = std::fpclassify(x_value);
    }
    auto result = Name::make_number_classification(trx, classification);
    *x_ptr = result;
}

// add: num num :- num
// Addition with overflow check.  Also supports Address + Integer -> Address.
// throws: numerical-inf, numerical-nan, numerical-overflow, opstack-underflow, type-check
static void add_op(Trix *trx) {
    // Address + Integer or Integer + Address -> Address
    trx->require_op_count(2);
    auto y_ptr = trx->m_op_ptr;
    auto x_ptr = (y_ptr - 1);
    if (x_ptr->is_address() || y_ptr->is_address()) {
        auto addr_ptr = x_ptr->is_address() ? x_ptr : y_ptr;
        auto int_ptr = x_ptr->is_address() ? y_ptr : x_ptr;
        if (int_ptr->is_address()) {
            trx->error(Error::TypeCheck, "add: cannot add two Address values");
        } else {
            auto addr = reinterpret_cast<uintptr_t>(addr_ptr->address_value(trx));
            auto offset = address_offset(trx, int_ptr);
            auto result = reinterpret_cast<address_t>(addr + static_cast<uintptr_t>(offset));
            // Construct the result BEFORE freeing the operands' ExtValues: a
            // vm-full raised by the allocation after the frees would leave freed
            // ExtValue objects on the operand stack for @try-rollback to free a
            // second time.
            auto result_obj = Object::make_address(trx, result);
            int_ptr->maybe_free_extvalue(trx);
            addr_ptr->maybe_free_extvalue(trx);
            *x_ptr = result_obj;
            trx->m_op_ptr = x_ptr;
            return;
        }
    }

    shim_ONumbers_Numbers__RNumbers(trx, VerifyIsFinite, VerifyIsFinite, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        static_assert(std::is_same_v<T, decltype(y)>);

        if constexpr (std::is_same_v<T, vm_t>) {
            auto result = (static_cast<uinteger_t>(x) + static_cast<uinteger_t>(y));
            auto overflow = (result > 255);
            if (overflow) {
                p->error(Error::NumericalOverflow, "byte addition");
            } else {
                return static_cast<vm_t>(result);
            }
        } else if constexpr (std::is_same_v<T, integer_t>) {
            integer_t result;
            auto overflow = __builtin_sadd_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "integer addition");
            } else {
                return result;
            }
        } else if constexpr (std::is_same_v<T, uinteger_t>) {
            uinteger_t result;
            auto overflow = __builtin_uadd_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "uinteger addition");
            } else {
                return result;
            }
        } else if constexpr (std::is_same_v<T, long_t>) {
            long_t result;
            auto overflow = __builtin_saddll_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "long addition");
            } else {
                return result;
            }
        } else if constexpr (std::is_same_v<T, ulong_t>) {
            ulong_t result;
            auto overflow = __builtin_uaddll_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "ulong addition");
            } else {
                return result;
            }
        } else if constexpr (std::is_same_v<T, int128_t>) {
            int128_t result;
            auto overflow = __builtin_add_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "int128 addition");
            } else {
                return result;
            }
        } else if constexpr (std::is_same_v<T, uint128_t>) {
            uint128_t result;
            auto overflow = __builtin_add_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "uint128 addition");
            } else {
                return result;
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            return p->verify_floating_point_result(x + y);
        }
    });
}

// sub: num num :- num
// Subtraction with overflow check.  Also supports Address - Integer -> Address
// and Address - Address -> Long.
// throws: numerical-inf, numerical-nan, numerical-overflow, opstack-underflow, type-check
static void sub_op(Trix *trx) {
    // Address - Integer -> Address, Address - Address -> Long
    trx->require_op_count(2);
    auto y_ptr = trx->m_op_ptr;
    auto x_ptr = (y_ptr - 1);
    if (x_ptr->is_address()) {
        auto x_addr = reinterpret_cast<uintptr_t>(x_ptr->address_value(trx));
        if (y_ptr->is_address()) {
            // Address - Address -> Long.  Construct the result BEFORE freeing
            // the operands' ExtValues: a vm-full raised by the allocation after
            // the frees would leave freed ExtValue objects on the operand stack
            // for @try-rollback to free a second time.
            auto y_addr = reinterpret_cast<uintptr_t>(y_ptr->address_value(trx));
            auto result = static_cast<long_t>(x_addr - y_addr);
            auto result_obj = Object::make_long(trx, result);
            y_ptr->free_extvalue(trx);
            x_ptr->free_extvalue(trx);
            *x_ptr = result_obj;
        } else {
            // Address - Integer -> Address (same construct-before-free order)
            auto offset = address_offset(trx, y_ptr);
            auto result = reinterpret_cast<address_t>(x_addr - static_cast<uintptr_t>(offset));
            auto result_obj = Object::make_address(trx, result);
            y_ptr->maybe_free_extvalue(trx);
            x_ptr->free_extvalue(trx);
            *x_ptr = result_obj;
        }
        trx->m_op_ptr = x_ptr;
    } else if (y_ptr->is_address()) {
        trx->error(Error::TypeCheck, "sub: cannot subtract Address from non-Address");
    } else {
        shim_ONumbers_Numbers__RNumbers(trx, VerifyIsFinite, VerifyIsFinite, [](Trix *p, auto x, auto y) {
            using T = decltype(x);
            static_assert(std::is_same_v<T, decltype(y)>);

            if constexpr (std::is_same_v<T, vm_t>) {
                auto overflow = (y > x);
                if (overflow) {
                    p->error(Error::NumericalOverflow, "byte subtraction");
                } else {
                    return static_cast<vm_t>(x - y);
                }
            } else if constexpr (std::is_same_v<T, integer_t>) {
                integer_t result;
                auto overflow = __builtin_ssub_overflow(x, y, &result);
                if (overflow) {
                    p->error(Error::NumericalOverflow, "integer subtraction");
                } else {
                    return result;
                }
            } else if constexpr (std::is_same_v<T, uinteger_t>) {
                uinteger_t result;
                auto overflow = __builtin_usub_overflow(x, y, &result);
                if (overflow) {
                    p->error(Error::NumericalOverflow, "uinteger subtraction");
                } else {
                    return result;
                }
            } else if constexpr (std::is_same_v<T, long_t>) {
                long_t result;
                auto overflow = __builtin_ssubll_overflow(x, y, &result);
                if (overflow) {
                    p->error(Error::NumericalOverflow, "long subtraction");
                } else {
                    return result;
                }
            } else if constexpr (std::is_same_v<T, ulong_t>) {
                ulong_t result;
                auto overflow = __builtin_usubll_overflow(x, y, &result);
                if (overflow) {
                    p->error(Error::NumericalOverflow, "ulong subtraction");
                } else {
                    return result;
                }
            } else if constexpr (std::is_same_v<T, int128_t>) {
                int128_t result;
                auto overflow = __builtin_sub_overflow(x, y, &result);
                if (overflow) {
                    p->error(Error::NumericalOverflow, "int128 subtraction");
                } else {
                    return result;
                }
            } else if constexpr (std::is_same_v<T, uint128_t>) {
                uint128_t result;
                auto overflow = __builtin_sub_overflow(x, y, &result);
                if (overflow) {
                    p->error(Error::NumericalOverflow, "uint128 subtraction");
                } else {
                    return result;
                }
            } else if constexpr (std::is_floating_point_v<T>) {
                return p->verify_floating_point_result(x - y);
            }
        });
    }
}

// mul: num num :- num
// Multiplication with overflow check.
// throws: numerical-inf, numerical-nan, numerical-overflow, opstack-underflow, type-check
static void mul_op(Trix *trx) {
    shim_ONumbers_Numbers__RNumbers(trx, VerifyIsFinite, VerifyIsFinite, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        static_assert(std::is_same_v<T, decltype(y)>);

        if constexpr (std::is_same_v<T, vm_t>) {
            auto result = (static_cast<uinteger_t>(x) * static_cast<uinteger_t>(y));
            auto overflow = (result > 255);
            if (overflow) {
                p->error(Error::NumericalOverflow, "byte multiplication");
            } else {
                return static_cast<vm_t>(result);
            }
        } else if constexpr (std::is_same_v<T, integer_t>) {
            integer_t result;
            auto overflow = __builtin_smul_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "integer multiplication");
            } else {
                return result;
            }
        } else if constexpr (std::is_same_v<T, uinteger_t>) {
            uinteger_t result;
            auto overflow = __builtin_umul_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "uinteger multiplication");
            } else {
                return result;
            }
        } else if constexpr (std::is_same_v<T, long_t>) {
            long_t result;
            auto overflow = __builtin_smulll_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "long multiplication");
            } else {
                return result;
            }
        } else if constexpr (std::is_same_v<T, ulong_t>) {
            ulong_t result;
            auto overflow = __builtin_umulll_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "ulong multiplication");
            } else {
                return result;
            }
        } else if constexpr (std::is_same_v<T, int128_t>) {
            int128_t result;
            auto overflow = __builtin_mul_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "int128 multiplication");
            } else {
                return result;
            }
        } else if constexpr (std::is_same_v<T, uint128_t>) {
            uint128_t result;
            auto overflow = __builtin_mul_overflow(x, y, &result);
            if (overflow) {
                p->error(Error::NumericalOverflow, "uint128 multiplication");
            } else {
                return result;
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            return p->verify_floating_point_result(x * y);
        }
    });
}

// div: num num :- num
// Division with zero and overflow check.
// throws: numerical-inf, numerical-nan, numerical-overflow, opstack-underflow, type-check
static void div_op(Trix *trx) {
    shim_ONumbers_Numbers__RNumbers(trx, VerifyIsFinite, VerifyIsFinite, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        static_assert(std::is_same_v<T, decltype(y)>);

        if constexpr (std::is_same_v<T, vm_t>) {
            if (y == 0) {
                p->error(Error::DivByZero, "div: division by zero");
            } else {
                return static_cast<vm_t>(x / y);
            }
        } else if constexpr (std::is_integral_v<T>) {
            if (y == 0) {
                p->error(Error::DivByZero, "div: division by zero");
            } else {
                if constexpr (std::is_signed_v<T>) {
                    if ((y == T{-1}) && (x == std::numeric_limits<T>::min())) {
                        p->error(Error::NumericalOverflow, "div: signed division overflow");
                    }
                }
                return (x / y);
            }
        } else if constexpr (std::is_same_v<T, int128_t>) {
            if (y == 0) {
                p->error(Error::DivByZero, "div: division by zero");
            } else if ((y == int128_t{-1}) && (x == std::numeric_limits<int128_t>::min())) {
                p->error(Error::NumericalOverflow, "div: int128 division overflow");
            } else {
                return (x / y);
            }
        } else if constexpr (std::is_same_v<T, uint128_t>) {
            if (y == 0) {
                p->error(Error::DivByZero, "div: division by zero");
            } else {
                return (x / y);
            }
        } else if constexpr (std::is_floating_point_v<T>) {
            if (y == T{0}) {
                p->error(Error::DivByZero, "div: division by zero");
            } else {
                return p->verify_floating_point_result(x / y);
            }
        }
    });
}

// min: num num :- num
// Returns the smaller of two values.
// throws: opstack-underflow, type-check
static void min_op(Trix *trx) {
    shim_ONumbersAddressString_NumbersAddressString__RNumbersAddressString(trx, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        static_assert(std::is_same_v<T, decltype(y)>);

        if constexpr (std::is_same_v<T, Object *>) {
            return ((x->string_compare(p, y) < 0) ? x : y);
        } else {
            return std::min(x, y);
        }
    });
}

// max: num num :- num
// Returns the larger of two values.
// throws: opstack-underflow, type-check
static void max_op(Trix *trx) {
    shim_ONumbersAddressString_NumbersAddressString__RNumbersAddressString(trx, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        static_assert(std::is_same_v<T, decltype(y)>);

        if constexpr (std::is_same_v<T, Object *>) {
            return ((x->string_compare(p, y) > 0) ? x : y);
        } else {
            return std::max(x, y);
        }
    });
}

// ge: num num :- bool
// Tests x >= y.
// throws: opstack-underflow, type-check
static void ge_op(Trix *trx) {
    shim_ONumbersAddressString_NumbersAddressString__RBoolean(trx, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        static_assert(std::is_same_v<T, decltype(y)>);

        if constexpr (std::is_same_v<T, Object *>) {
            return (x->string_compare(p, y) >= 0);
        } else {
            return (x >= y);
        }
    });
}

// gt: num num :- bool
// Tests x > y.
// throws: opstack-underflow, type-check
static void gt_op(Trix *trx) {
    shim_ONumbersAddressString_NumbersAddressString__RBoolean(trx, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        static_assert(std::is_same_v<T, decltype(y)>);

        if constexpr (std::is_same_v<T, Object *>) {
            return (x->string_compare(p, y) > 0);
        } else {
            return (x > y);
        }
    });
}

// le: num num :- bool
// Tests x <= y.
// throws: opstack-underflow, type-check
static void le_op(Trix *trx) {
    shim_ONumbersAddressString_NumbersAddressString__RBoolean(trx, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        static_assert(std::is_same_v<T, decltype(y)>);

        if constexpr (std::is_same_v<T, Object *>) {
            return (x->string_compare(p, y) <= 0);
        } else {
            return (x <= y);
        }
    });
}

// lt: num num :- bool
// Tests x < y.
// throws: opstack-underflow, type-check
static void lt_op(Trix *trx) {
    shim_ONumbersAddressString_NumbersAddressString__RBoolean(trx, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        static_assert(std::is_same_v<T, decltype(y)>);

        if constexpr (std::is_same_v<T, Object *>) {
            return (x->string_compare(p, y) < 0);
        } else {
            return (x < y);
        }
    });
}

// sin: num :- num
// Sine.
// throws: opstack-underflow, type-check
static void sin_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotInf | VerifyNotNan), [](Trix *, auto x) { return std::sin(x); });
}

// cos: num :- num
// Cosine.
// throws: opstack-underflow, type-check
static void cos_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotInf | VerifyNotNan), [](Trix *, auto x) { return std::cos(x); });
}

// tan: num :- num
// Tangent.
// throws: opstack-underflow, type-check
static void tan_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotInf | VerifyNotNan), [](Trix *, auto x) { return std::tan(x); });
}

// asin: num :- num
// Arcsine.
// throws: opstack-underflow, type-check
static void asin_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyIsAbsLessEqOne | VerifyNotNan), [](Trix *, auto x) { return std::asin(x); });
}

// acos: num :- num
// Arccosine.
// throws: opstack-underflow, type-check
static void acos_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyIsAbsLessEqOne | VerifyNotNan), [](Trix *, auto x) { return std::acos(x); });
}

// atan: num :- num
// Arctangent.
// throws: opstack-underflow, type-check
static void atan_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::atan(x); });
}

// atan2: num num :- num
// Two-argument arctangent.
// throws: opstack-underflow, type-check
static void atan2_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, VerifyNotNan, [](Trix *, auto y, auto x) { return std::atan2(y, x); });
}

// sinh: num :- num
// Hyperbolic sine.
// throws: opstack-underflow, type-check
static void sinh_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::sinh(x); });
}

// cosh: num :- num
// Hyperbolic cosine.
// throws: opstack-underflow, type-check
static void cosh_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::cosh(x); });
}

// tanh: num :- num
// Hyperbolic tangent.
// throws: opstack-underflow, type-check
static void tanh_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::tanh(x); });
}

// asinh: num :- num
// Inverse hyperbolic sine.
// throws: opstack-underflow, type-check
static void asinh_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::asinh(x); });
}

// acosh: num :- num
// Inverse hyperbolic cosine.  Domain: x >= 1.
// throws: opstack-underflow, type-check, range-check
static void acosh_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotNan | VerifyGreaterEqualOne), [](Trix *, auto x) { return std::acosh(x); });
}

// atanh: num :- num
// Inverse hyperbolic tangent.  Domain: the open interval (-1, 1).
// throws: opstack-underflow, type-check, range-check
static void atanh_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotNan | VerifyIsAbsLessOne), [](Trix *, auto x) { return std::atanh(x); });
}

// floor: num :- num
// Rounds toward negative infinity.
// throws: opstack-underflow, type-check
static void floor_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::floor(x); });
}

// trunc: num :- num
// Rounds toward zero.
// throws: opstack-underflow, type-check
static void trunc_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::trunc(x); });
}

// round: num :- num
// Rounds to nearest, halfway away from zero.
// throws: opstack-underflow, type-check
static void round_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::round(x); });
}

// nearby-int: num :- num
// Rounds using the current rounding mode.
// throws: opstack-underflow, type-check
static void nearbyint_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::nearbyint(x); });
}

// rint: num :- num
// Rounds using the current rounding mode (may raise FE_INEXACT).
// throws: opstack-underflow, type-check
static void rint_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::rint(x); });
}

// ceil: num :- num
// Rounds toward positive infinity.
// throws: opstack-underflow, type-check
static void ceil_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::ceil(x); });
}

// degrees-to-radians: num :- num
// Converts degrees to radians.  Real inputs compute via double intermediates
// to avoid precision loss from the float-rounded pi/180 constant.  Double inputs
// use a two-part constant with FMA: pi/180 = deg2rad_hi + deg2rad_lo, where
// deg2rad_hi has limited trailing bits so that x * deg2rad_hi is exact for
// small integer degree values.  Result = fma(x, hi, x * lo).
// throws: opstack-underflow, type-check
static void degrees_to_radians_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) {
        using T = decltype(x);

        if constexpr (std::is_same_v<T, real_t>) {
            // compute in double, narrow to float -- eliminates float-precision constant error
            return static_cast<real_t>(static_cast<double_t>(x) * (std::numbers::pi_v<double_t> / double_t{180}));
        } else {
            // two-part constant: pi/180 = hi + lo
            // hi has 26 significant bits so that (small integer) * hi is exact
            static constexpr double_t deg2rad_hi = 0x1.1df46a0000000p-6;
            static constexpr double_t deg2rad_lo = std::numbers::pi_v<double_t> / double_t{180} - deg2rad_hi;
            return std::fma(x, deg2rad_hi, x * deg2rad_lo);
        }
    });
}

// radians-to-degrees: num :- num
// Converts radians to degrees.  Real inputs compute via double intermediates.
// Double inputs use a two-part constant with FMA: 180/pi = rad2deg_hi + rad2deg_lo.
// throws: opstack-underflow, type-check
static void radians_to_degrees_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) {
        using T = decltype(x);

        if constexpr (std::is_same_v<T, real_t>) {
            return static_cast<real_t>(static_cast<double_t>(x) * (double_t{180} / std::numbers::pi_v<double_t>));
        } else {
            // two-part constant: 180/pi = hi + lo
            static constexpr double_t rad2deg_hi = 0x1.ca5dc1a000000p+5;
            static constexpr double_t rad2deg_lo = double_t{180} / std::numbers::pi_v<double_t> - rad2deg_hi;
            return std::fma(x, rad2deg_hi, x * rad2deg_lo);
        }
    });
}

// exp: num :- num
// Natural exponential e^x.
// throws: opstack-underflow, type-check
static void exp_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::exp(x); });
}

// exp2: num :- num
// Base-2 exponential 2^x.
// throws: opstack-underflow, type-check
static void exp2_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::exp2(x); });
}

// expm1: num :- num
// e^x - 1, accurate for small x.
// throws: opstack-underflow, type-check
static void expm1_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::expm1(x); });
}

// expint: num :- num
// Exponential integral Ei(x).
// throws: opstack-underflow, type-check
static void expint_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::expint(x); });
}

// log: num :- num
// Natural logarithm.
// throws: opstack-underflow, type-check
static void log_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotZero | VerifyNotNegative | VerifyNotNan), [](Trix *, auto x) { return std::log(x); });
}

// log2: num :- num
// Base-2 logarithm.
// throws: opstack-underflow, type-check
static void log2_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotZero | VerifyNotNegative | VerifyNotNan), [](Trix *, auto x) { return std::log2(x); });
}

// log10: num :- num
// Base-10 logarithm.
// throws: opstack-underflow, type-check
static void log10_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotZero | VerifyNotNegative | VerifyNotNan), [](Trix *, auto x) { return std::log10(x); });
}

// log1p: num :- num
// ln(1 + x), accurate for small x.  Domain: x > -1.
// throws: opstack-underflow, type-check, range-check
static void log1p_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotNan | VerifyGreaterThanNegOne), [](Trix *, auto x) { return std::log1p(x); });
}

// erf: num :- num
// Error function.
// throws: opstack-underflow, type-check
static void erf_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::erf(x); });
}

// erfc: num :- num
// Complementary error function.
// throws: opstack-underflow, type-check
static void erfc_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::erfc(x); });
}

// tgamma: num :- num
// Gamma function.  Domain: all reals except 0 and the negative integers (poles).
// throws: opstack-underflow, type-check, range-check
static void tgamma_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotNan | VerifyNotNonPositiveInteger), [](Trix *, auto x) { return std::tgamma(x); });
}

// lgamma: num :- num
// Natural log of the absolute value of gamma.  Domain: all reals except 0 and
// the negative integers (poles).
// throws: opstack-underflow, type-check, range-check
static void lgamma_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotNan | VerifyNotNonPositiveInteger), [](Trix *, auto x) { return std::lgamma(x); });
}

// riemann-zeta: num :- num
// Riemann zeta function.
// throws: opstack-underflow, type-check
static void riemann_zeta_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::riemann_zeta(x); });
}

// hermite: uint num :- num
// Hermite polynomial H_n(x); requires order n < 128 and finite x.
// throws: opstack-underflow, type-check, range-check
static void hermite_op(Trix *trx) {
    shim_OUInteger_Floats__RFloats(trx, VerifyNotNan | VerifyNotInf, [](Trix *, auto n, auto x) { return std::hermite(n, x); });
}

// laguerre: uint num :- num
// Laguerre polynomial L_n(x); requires x >= 0 and order n < 128.
// throws: opstack-underflow, type-check, range-check
static void laguerre_op(Trix *trx) {
    shim_OUInteger_Floats__RFloats(
            trx, VerifyNotNan | VerifyNotInf | VerifyNotNegative, [](Trix *, auto n, auto x) { return std::laguerre(n, x); });
}

// legendre: uint num :- num
// Legendre polynomial P_n(x); requires |x| <= 1 and order n < 128.
// throws: opstack-underflow, type-check, range-check
static void legendre_op(Trix *trx) {
    shim_OUInteger_Floats__RFloats(
            trx, VerifyNotNan | VerifyIsAbsLessEqOne, [](Trix *, auto n, auto x) { return std::legendre(n, x); });
}

// sph-bessel: uint num :- num
// Spherical Bessel function of the first kind j_n(x); requires x >= 0 and order n < 128.
// throws: opstack-underflow, type-check, range-check
static void sph_bessel_op(Trix *trx) {
    shim_OUInteger_Floats__RFloats(
            trx, VerifyNotNan | VerifyNotInf | VerifyNotNegative, [](Trix *, auto n, auto x) { return std::sph_bessel(n, x); });
}

// sph-neumann: uint num :- num
// Spherical Neumann function (Bessel second kind) y_n(x); requires x > 0
// (pole at 0) and order n < 128.
// throws: opstack-underflow, type-check, range-check
static void sph_neumann_op(Trix *trx) {
    shim_OUInteger_Floats__RFloats(trx,
                                   VerifyNotNan | VerifyNotInf | VerifyNotZero | VerifyNotNegative,
                                   [](Trix *, auto n, auto x) { return std::sph_neumann(n, x); });
}

// assoc-laguerre: uint uint num :- num
// Associated Laguerre polynomial L_n^m(x); requires x >= 0 and orders n, m < 128.
// throws: opstack-underflow, type-check, range-check
static void assoc_laguerre_op(Trix *trx) {
    shim_OUInteger_UInteger_Floats__RFloats(trx,
                                            VerifyNotNan | VerifyNotInf | VerifyNotNegative,
                                            [](Trix *, auto n, auto m, auto x) { return std::assoc_laguerre(n, m, x); });
}

// assoc-legendre: uint uint num :- num
// Associated Legendre polynomial P_n^m(x); requires |x| <= 1 and orders n, m < 128.
// throws: opstack-underflow, type-check, range-check
static void assoc_legendre_op(Trix *trx) {
    shim_OUInteger_UInteger_Floats__RFloats(
            trx, VerifyNotNan | VerifyIsAbsLessEqOne, [](Trix *, auto n, auto m, auto x) { return std::assoc_legendre(n, m, x); });
}

// sph-legendre: uint uint num :- num
// Spherical harmonic Y_l^m(theta) (real part); requires orders l, m < 128 and finite theta.
// throws: opstack-underflow, type-check, range-check
static void sph_legendre_op(Trix *trx) {
    shim_OUInteger_UInteger_Floats__RFloats(
            trx, VerifyNotNan | VerifyNotInf, [](Trix *, auto l, auto m, auto theta) { return std::sph_legendre(l, m, theta); });
}

// sqrt: num :- num
// Square root.
// throws: opstack-underflow, type-check
static void sqrt_op(Trix *trx) {
    shim_OFloats__RFloats(trx, (VerifyNotNan | VerifyNotNegative), [](Trix *, auto x) { return std::sqrt(x); });
}

// cbrt: num :- num
// Cube root.
// throws: opstack-underflow, type-check
static void cbrt_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::cbrt(x); });
}

// logb: num :- num
// Extracts the exponent as a float.
// throws: opstack-underflow, type-check
static void logb_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::logb(x); });
}

// ilogb: num :- int
// Extracts the exponent as an integer.
// throws: opstack-underflow, type-check
static void ilogb_op(Trix *trx) {
    shim_OFloats__RInteger(trx, [](Trix *, auto x) { return std::ilogb(x); });
}

// modf: num :- num num
// Splits into fractional and integral parts.
// throws: vm-full, opstack-overflow, opstack-underflow, type-check
static void modf_op(Trix *trx) {
    shim_OFloats__RFloats_Floats(trx, [](Trix *, auto x) {
        using T = decltype(x);

        T integral_part;
        auto fractional_part = std::modf(x, &integral_part);
        return std::pair{fractional_part, integral_part};
    });
}

// frexp: num :- num int
// Splits into normalized fraction and exponent.
// throws: opstack-overflow, opstack-underflow, type-check
static void frexp_op(Trix *trx) {
    shim_OFloats__RFloats_Integer(trx, [](Trix *, auto x) {
        int exp;
        auto fraction = std::frexp(x, &exp);
        return std::pair{fraction, static_cast<integer_t>(exp)};
    });
}

// quot-rem: int int :- int int
// Signed integer quotient and remainder.
// throws: numerical-overflow, opstack-underflow, type-check, undefined-result
static void quotrem_op(Trix *trx) {
    shim_OSignedIntegers_SignedIntegers__RSignedIntegers_SignedIntegers(trx, [](Trix *p, auto x, auto y) {
        using T = decltype(x);
        if (y == 0) {
            p->error(Error::DivByZero, "quot-rem: division by zero");
        } else if ((x == std::numeric_limits<T>::min()) && (y == T{-1})) {
            p->error(Error::NumericalOverflow, "signed quot-rem overflow");
        } else if constexpr (std::is_same_v<T, int128_t>) {
            return std::pair{static_cast<T>(x / y), static_cast<T>(x % y)};
        } else {
            auto result = std::div(x, y);
            return std::pair{static_cast<T>(result.quot), static_cast<T>(result.rem)};
        }
    });
}

// fe-clear-except: uint :- bool
// Clears the specified floating-point exception flags.
// throws: opstack-underflow, range-check, type-check
static void feclearexcept_op(Trix *trx) {
    fe_except_bool_impl(trx, std::feclearexcept, "fe-clear-except");
}

// fe-raise-except: uint :- bool
// Raises the specified floating-point exception flags.
// throws: opstack-underflow, range-check, type-check
static void feraiseexcept_op(Trix *trx) {
    fe_except_bool_impl(trx, std::feraiseexcept, "fe-raise-except");
}

// fe-test-except: uint :- uint
// Tests the specified floating-point exception flags.
// throws: opstack-underflow, range-check, type-check
static void fetestexcept_op(Trix *trx) {
    trx->verify_operands(VerifyUInteger);

    auto val_ptr = trx->m_op_ptr;
    auto excepts = val_ptr->uinteger_value();

    if ((excepts & ~static_cast<uinteger_t>(FE_ALL_EXCEPT)) == 0) {
        auto result = std::fetestexcept(static_cast<int>(excepts));
        val_ptr->update_uinteger(static_cast<uinteger_t>(result));
    } else {
        trx->error(Error::RangeCheck, "fe-test-except: invalid exception flags");
    }
}

// fe-set-round: name :- bool
// Sets the floating-point rounding mode.
// throws: opstack-underflow, range-check, type-check
static void fesetround_op(Trix *trx) {
    trx->verify_operands(VerifyName);

    auto val_ptr = trx->m_op_ptr;
    auto name = val_ptr->name_value(trx);

    int rounding;
    if (name->is_systemname(trx, SystemName::FE_Downward)) {
        rounding = FE_DOWNWARD;
    } else if (name->is_systemname(trx, SystemName::FE_ToNearest)) {
        rounding = FE_TONEAREST;
    } else if (name->is_systemname(trx, SystemName::FE_TowardZero)) {
        rounding = FE_TOWARDZERO;
    } else if (name->is_systemname(trx, SystemName::FE_Upward)) {
        rounding = FE_UPWARD;
    } else {
        trx->error(Error::RangeCheck, "fe-set-round: unknown rounding mode");
    }

    auto result = std::fesetround(rounding);
    *val_ptr = Object::make_boolean(result == 0);
}

// fe-get-round: :- name
// Returns the current floating-point rounding mode.
// throws: opstack-overflow
static void fegetround_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto rounding = std::fegetround();
    *++trx->m_op_ptr = Name::make_number_rounding(trx, rounding);
}

// rem-quo: num num :- num int
// IEEE remainder and partial quotient.
// throws: opstack-underflow, type-check
static void remquo_op(Trix *trx) {
    shim_OFloats_Floats__RFloats_Integer(trx, [](Trix *, auto x, auto y) {
        int quo;
        auto result = std::remquo(x, y, &quo);
        return std::pair{result, quo};
    });
}

// fe-get-env: :- ulong
// Returns the current floating-point environment as a packed ULong.
// Bits 0-31: exception flags from fetestexcept(FE_ALL_EXCEPT).
// Bits 32-63: rounding mode from fegetround().
// throws: opstack-overflow
static void fegetenv_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto excepts = static_cast<ulong_t>(std::fetestexcept(FE_ALL_EXCEPT));
    auto rounding = static_cast<ulong_t>(std::fegetround());
    auto packed = (rounding << 32) | (excepts & 0xFFFFFFFFull);
    *++trx->m_op_ptr = Object::make_ulong(trx, packed);
}

// fe-set-env: ulong :-
// Restores the floating-point environment from a packed ULong.
// throws: opstack-underflow, range-check, type-check
static void fesetenv_op(Trix *trx) {
    trx->verify_operands(VerifyULong);

    auto val_ptr = trx->m_op_ptr;
    auto packed = val_ptr->ulong_value(trx);

    auto excepts = static_cast<int>(packed & 0xFFFFFFFFull);
    auto rounding = static_cast<int>(packed >> 32);

    if ((excepts & ~FE_ALL_EXCEPT) != 0) {
        trx->error(Error::RangeCheck, "fe-set-env: invalid exception flags in packed environment");
    } else {
        // Validate (and apply) the rounding mode BEFORE touching the exception
        // flags: fesetround mutates only the rounding mode and is a no-op on a bad
        // mode, so an invalid rounding field leaves the whole FP environment
        // untouched (all-or-nothing) instead of clobbering the flags then erroring.
        if (std::fesetround(rounding) != 0) {
            trx->error(Error::RangeCheck, "fe-set-env: invalid rounding mode in packed environment");
        } else {
            val_ptr->free_extvalue(trx);

            std::feclearexcept(FE_ALL_EXCEPT);
            if (excepts != 0) {
                std::feraiseexcept(excepts);
            }

            --trx->m_op_ptr;
        }
    }
}

// fe-hold-except: :- ulong
// Saves the current floating-point environment as a packed ULong,
// then clears all exception flags (non-stop mode).
// throws: opstack-overflow
static void feholdexcept_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto excepts = static_cast<ulong_t>(std::fetestexcept(FE_ALL_EXCEPT));
    auto rounding = static_cast<ulong_t>(std::fegetround());
    auto packed = (rounding << 32) | (excepts & 0xFFFFFFFFull);

    std::feclearexcept(FE_ALL_EXCEPT);

    *++trx->m_op_ptr = Object::make_ulong(trx, packed);
}

// fe-update-env: ulong :-
// Saves current exception flags, restores the packed environment,
// then raises the previously-saved exception flags (merging them).
// throws: opstack-underflow, range-check, type-check
static void feupdateenv_op(Trix *trx) {
    trx->verify_operands(VerifyULong);

    auto val_ptr = trx->m_op_ptr;
    auto packed = val_ptr->ulong_value(trx);

    auto saved_excepts = static_cast<int>(packed & 0xFFFFFFFFull);
    auto saved_rounding = static_cast<int>(packed >> 32);

    if ((saved_excepts & ~FE_ALL_EXCEPT) != 0) {
        trx->error(Error::RangeCheck, "fe-update-env: invalid exception flags in packed environment");
    } else {
        // Capture current exceptions before any FP-state mutation (for the merge
        // below); fetestexcept is read-only so this is safe ahead of validation.
        auto current_excepts = std::fetestexcept(FE_ALL_EXCEPT);

        // Validate (and apply) the rounding mode FIRST: fesetround touches only the
        // rounding mode and is a no-op on a bad mode, so an invalid rounding field
        // leaves the FP environment untouched (all-or-nothing) rather than
        // clobbering the flags then erroring.
        if (std::fesetround(saved_rounding) != 0) {
            trx->error(Error::RangeCheck, "fe-update-env: invalid rounding mode in packed environment");
        } else {
            val_ptr->free_extvalue(trx);

            // restore the saved exception flags
            std::feclearexcept(FE_ALL_EXCEPT);
            if (saved_excepts != 0) {
                std::feraiseexcept(saved_excepts);
            }

            // merge: raise any exceptions that occurred since feholdexcept
            if (current_excepts != 0) {
                std::feraiseexcept(current_excepts);
            }

            --trx->m_op_ptr;
        }
    }
}

// round-even: num :- num
// Rounds to nearest, ties to even (IEEE 754-2019 roundTiesToEven).
// throws: opstack-underflow, type-check
static void roundeven_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyAttrsNone, [](Trix *, auto x) {
        using T = decltype(x);
        auto saved = std::fegetround();
        std::fesetround(FE_TONEAREST);
        auto result = std::nearbyint(x);
        std::fesetround(saved);
        return static_cast<T>(result);
    });
}

// ulp: num :- num
// Returns the unit in the last place (ULP) of a floating-point number.
// For finite x: the magnitude of the least significant bit.
// For inf/NaN: returns NaN.
// throws: opstack-underflow, type-check
static void ulp_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyAttrsNone, [](Trix *, auto x) {
        using T = decltype(x);
        if (!std::isfinite(x)) {
            return std::numeric_limits<T>::quiet_NaN();
        } else {
            auto ax = std::abs(x);
            auto next = std::nextafter(ax, std::numeric_limits<T>::infinity());
            if (std::isinf(next)) {
                // x is max representable value
                return (ax - std::nextafter(ax, T{0}));
            } else {
                return (next - ax);
            }
        }
    });
}

// total-order?: num num :- bool
// IEEE 754-2019 totalOrder predicate.
// Returns true if x <= y in the total ordering:
// -NaN < -Inf < -finite < -0 < +0 < +finite < +Inf < +NaN
// throws: opstack-underflow, type-check
static void totalorder_op(Trix *trx) {
    shim_OFloats_Floats__RBoolean(trx, [](Trix *, auto x, auto y) { return std::is_lteq(std::strong_order(x, y)); });
}

// total-order-mag?: num num :- bool
// IEEE 754-2019 totalOrderMag predicate.
// Returns true if |x| <= |y| in the total ordering.
// throws: opstack-underflow, type-check
static void totalordermag_op(Trix *trx) {
    shim_OFloats_Floats__RBoolean(trx,
                                  [](Trix *, auto x, auto y) { return std::is_lteq(std::strong_order(std::abs(x), std::abs(y))); });
}

// fmax-mag: num num :- num
// Returns the operand with the larger absolute value (NaN-aware).
// If |x| > |y|, returns x. If |y| > |x|, returns y.
// If |x| == |y|, returns fmax(x, y).
// throws: opstack-underflow, type-check
static void fmaxmag_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) {
        auto ax = std::abs(x);
        auto ay = std::abs(y);
        if (ax > ay) {
            return x;
        } else if (ay > ax) {
            return y;
        } else {
            return std::fmax(x, y);
        }
    });
}

// fmin-mag: num num :- num
// Returns the operand with the smaller absolute value (NaN-aware).
// If |x| < |y|, returns x. If |y| < |x|, returns y.
// If |x| == |y|, returns fmin(x, y).
// throws: opstack-underflow, type-check
static void fminmag_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) {
        auto ax = std::abs(x);
        auto ay = std::abs(y);
        if (ax < ay) {
            return x;
        } else if (ay < ax) {
            return y;
        } else {
            return std::fmin(x, y);
        }
    });
}

// ulp-equal?: num num int :- bool
// Returns true if two floating-point numbers are within N ULPs of each other.
// Handles edge cases: NaN (always false), infinities (equal only if same sign),
// zero vs zero (always true), subnormals.
// throws: opstack-underflow, type-check
static void ulpequal_op(Trix *trx) {
    trx->verify_operands(VerifyInteger | VerifyNotNegative, VerifyFloats, VerifyFloats);

    auto n_ptr = trx->m_op_ptr;
    auto y_ptr = (n_ptr - 1);
    auto x_ptr = (n_ptr - 2);

    auto n = n_ptr->integer_value();

    if (x_ptr->is_real() && y_ptr->is_real()) {
        auto x = x_ptr->real_value();
        auto y = y_ptr->real_value();
        bool result;

        if (std::isnan(x) || std::isnan(y)) {
            result = false;
        } else if (x == y) {
            result = true;
        } else {
            // convert sign-magnitude IEEE 754 to linear integer ordering
            auto xi_bits = std::bit_cast<uint32_t>(x);
            auto yi_bits = std::bit_cast<uint32_t>(y);
            // negative floats: flip all bits; positive: offset by 0x80000000
            auto xi = (xi_bits & 0x80000000u) ? ~xi_bits : (xi_bits + 0x80000000u);
            auto yi = (yi_bits & 0x80000000u) ? ~yi_bits : (yi_bits + 0x80000000u);
            auto diff = (xi > yi) ? (xi - yi) : (yi - xi);
            result = (diff <= static_cast<uint32_t>(n));
        }

        trx->m_op_ptr -= 2;
        *x_ptr = Object::make_boolean(result);
    } else if (x_ptr->is_double() && y_ptr->is_double()) {
        auto x = x_ptr->double_value(trx);
        auto y = y_ptr->double_value(trx);
        bool result;

        if (std::isnan(x) || std::isnan(y)) {
            result = false;
        } else if (x == y) {
            result = true;
        } else {
            // convert sign-magnitude IEEE 754 to linear integer ordering
            auto xi_bits = std::bit_cast<uint64_t>(x);
            auto yi_bits = std::bit_cast<uint64_t>(y);
            // negative floats: flip all bits; positive: offset by 0x8000000000000000
            auto xi = (xi_bits & 0x8000000000000000ull) ? ~xi_bits : (xi_bits + 0x8000000000000000ull);
            auto yi = (yi_bits & 0x8000000000000000ull) ? ~yi_bits : (yi_bits + 0x8000000000000000ull);
            auto diff = (xi > yi) ? (xi - yi) : (yi - xi);
            result = (diff <= static_cast<uint64_t>(n));
        }

        x_ptr->free_extvalue(trx);
        y_ptr->free_extvalue(trx);
        trx->m_op_ptr -= 2;
        *x_ptr = Object::make_boolean(result);
    } else {
        trx->error(Error::TypeCheck, "ulp-equal: both operands must be the same float type");
    }
}

// Promote a numeric array/packed element to double for the precision-summation
// ops, raising /type-check on a non-numeric element instead of
// object_to_double's silent 0.0 default arm.  kahan-sum / dot-product both
// document `throws: type-check` and must reject non-numerics like product does;
// is_number() selects exactly object_to_double's explicit (non-default) arms.
[[nodiscard]] static double numeric_element_to_double(Trix *trx, Object elem, const char *op_name) {
    if (!elem.is_number()) {
        trx->error(Error::TypeCheck, "{}: array elements must be numeric", op_name);
    } else {
        return object_to_double(trx, elem, elem.type());
    }
}

// dot-product: arr arr :- double
// Kahan-compensated dot product of two numeric arrays.
// Both arrays must have the same length. All elements promoted to double.
// throws: opstack-underflow, range-check, type-check
static void dotproduct_op(Trix *trx) {
    trx->verify_operands(VerifyArrays, VerifyArrays);

    auto b_ptr = trx->m_op_ptr;
    auto a_ptr = (b_ptr - 1);
    auto a_count = a_ptr->arrays_length();
    auto b_count = b_ptr->arrays_length();

    if (a_count != b_count) {
        trx->error(Error::RangeCheck, "dot-product: arrays must have the same length");
    } else {
        double sum = 0.0;
        double compensation = 0.0;

        if (a_ptr->is_array() && b_ptr->is_array()) {
            auto a_elem_ptr = a_ptr->array_objects(trx);
            auto b_elem_ptr = b_ptr->array_objects(trx);
            for (length_t i = 0; i < a_count; ++i) {
                auto ai = numeric_element_to_double(trx, a_elem_ptr[i], "dot-product");
                auto bi = numeric_element_to_double(trx, b_elem_ptr[i], "dot-product");
                auto y = (ai * bi) - compensation;
                auto t = sum + y;
                compensation = (t - sum) - y;
                sum = t;
            }
        } else {
            // handle mixed array/packed or packed/packed
            auto a_is_array = a_ptr->is_array();
            auto b_is_array = b_ptr->is_array();

            const Object *a_arr = a_is_array ? a_ptr->array_objects(trx) : nullptr;
            const Object *b_arr = b_is_array ? b_ptr->array_objects(trx) : nullptr;

            auto a_packed = a_is_array ? cpacked_data_span{} : a_ptr->const_packed_span(trx);
            auto b_packed = b_is_array ? cpacked_data_span{} : b_ptr->const_packed_span(trx);

            auto a_ptr_p = a_packed.data();
            auto b_ptr_p = b_packed.data();

            for (length_t i = 0; i < a_count; ++i) {
                double ai, bi;
                if (a_is_array) {
                    ai = numeric_element_to_double(trx, a_arr[i], "dot-product");
                } else {
                    auto [next, elem] = Object::extract_next_packed(trx, a_ptr_p);
                    ai = numeric_element_to_double(trx, elem, "dot-product");
                    a_ptr_p = next;
                }
                if (b_is_array) {
                    bi = numeric_element_to_double(trx, b_arr[i], "dot-product");
                } else {
                    auto [next, elem] = Object::extract_next_packed(trx, b_ptr_p);
                    bi = numeric_element_to_double(trx, elem, "dot-product");
                    b_ptr_p = next;
                }
                auto y = (ai * bi) - compensation;
                auto t = sum + y;
                compensation = (t - sum) - y;
                sum = t;
            }
        }

        b_ptr->maybe_free_extvalue(trx);
        a_ptr->maybe_free_extvalue(trx);
        --trx->m_op_ptr;
        *a_ptr = Object::make_double(trx, sum);
    }
}

// product: arr :- number
// Returns the product of all elements in the array.
// Empty array returns integer 1. Element types must be uniform.
// throws: numerical-overflow, opstack-underflow, type-check
static void product_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);

    auto container_ptr = trx->m_op_ptr;
    auto count = container_ptr->arrays_length();

    if (count == 0) {
        *container_ptr = Object::make_integer(1);
    } else {
        // peek at first element type to determine the identity value
        Object identity;
        if (container_ptr->is_array()) {
            identity = container_ptr->array_objects(trx)[0];
        } else {
            auto packed_data = container_ptr->const_packed_span(trx);
            auto [next, element] = Object::extract_next_packed(trx, packed_data.data());
            identity = element;
        }

        // rewrite stack: container -> container identity { mul }
        trx->require_op_capacity(2);
        trx->require_exec_capacity(4);

        switch (+identity.type()) {
        case +Object::Type::Byte:
            identity = Object::make_byte(1);
            break;

        case +Object::Type::Integer:
            identity = Object::make_integer(1);
            break;

        case +Object::Type::UInteger:
            identity = Object::make_uinteger(1);
            break;

        case +Object::Type::Long:
            identity = Object::make_long(trx, 1);
            break;

        case +Object::Type::ULong:
            identity = Object::make_ulong(trx, 1);
            break;

        case +Object::Type::Int128:
            identity = Object::make_int128(trx, 1);
            break;

        case +Object::Type::UInt128:
            identity = Object::make_uint128(trx, 1);
            break;

        case +Object::Type::Real:
            identity = Object::make_real(1.0f);
            break;

        case +Object::Type::Double:
            identity = Object::make_double(trx, 1.0);
            break;

        default:
            trx->error(Error::TypeCheck, "product: array elements must be numeric");
        }

        push_forall_frame(trx, Object::make_operator(SystemName::Mul), *container_ptr);

        // operand stack: replace container with identity (accumulator)
        *container_ptr = identity;
    }
}

// kahan-sum: arr|packed :- double
// Kahan compensated summation of a numeric array.
// All elements are promoted to double for maximum precision.
// Returns the sum as a Double.
// throws: opstack-underflow, type-check
static void kahansum_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);

    auto container_ptr = trx->m_op_ptr;
    auto count = container_ptr->arrays_length();

    if (count == 0) {
        *container_ptr = Object::make_double(trx, 0.0);
    } else {
        double sum = 0.0;
        double compensation = 0.0;

        if (container_ptr->is_array()) {
            auto arr_ptr = container_ptr->array_objects(trx);
            for (length_t i = 0; i < count; ++i) {
                auto y = numeric_element_to_double(trx, arr_ptr[i], "kahan-sum") - compensation;
                auto t = sum + y;
                compensation = (t - sum) - y;
                sum = t;
            }
        } else {
            auto packed_data = container_ptr->const_packed_span(trx);
            auto ptr = packed_data.data();
            for (length_t i = 0; i < count; ++i) {
                auto [next, element] = Object::extract_next_packed(trx, ptr);
                auto y = numeric_element_to_double(trx, element, "kahan-sum") - compensation;
                auto t = sum + y;
                compensation = (t - sum) - y;
                sum = t;
                ptr = next;
            }
        }

        container_ptr->maybe_free_extvalue(trx);
        *container_ptr = Object::make_double(trx, sum);
    }
}

// nan-payload: nan :- uint|ulong
// Extracts the payload from a NaN value.
// Real NaN returns UInteger payload, Double NaN returns ULong payload.
// throws: opstack-underflow, type-check, undefined-result
static void nanpayload_op(Trix *trx) {
    trx->verify_operands(VerifyFloats);

    auto val_ptr = trx->m_op_ptr;

    if (val_ptr->is_real()) {
        auto x = val_ptr->real_value();
        if (!std::isnan(x)) {
            trx->error(Error::UndefinedResult, "nan-payload: operand is not NaN");
        } else {
            auto bits = std::bit_cast<uint32_t>(x);
            // float: sign(1) + exponent(8) + mantissa(23); quiet bit is bit 22
            auto payload = static_cast<uinteger_t>(bits & 0x003FFFFFu);
            *val_ptr = Object::make_uinteger(payload);
        }
    } else {
        auto x = val_ptr->double_value(trx);
        if (!std::isnan(x)) {
            trx->error(Error::UndefinedResult, "nan-payload: operand is not NaN");
        } else {
            auto bits = std::bit_cast<uint64_t>(x);
            // double: sign(1) + exponent(11) + mantissa(52); quiet bit is bit 51
            auto payload = ulong_t{bits & 0x0007FFFFFFFFFFFFull};
            val_ptr->free_extvalue(trx);
            *val_ptr = Object::make_ulong(trx, payload);
        }
    }
}

// nan-with-payload: uint|ulong :- nan
// Creates a quiet NaN with the specified payload.
// UInteger input creates a Real NaN, ULong input creates a Double NaN.
// throws: opstack-underflow, range-check, type-check
static void nanwithpayload_op(Trix *trx) {
    trx->verify_operands(VerifyUInteger | VerifyULong);

    auto val_ptr = trx->m_op_ptr;

    if (val_ptr->is_uinteger()) {
        auto payload = val_ptr->uinteger_value();
        // float quiet NaN: exponent all 1s, quiet bit set, payload in bits 0-21
        if (payload > 0x003FFFFFu) {
            trx->error(Error::RangeCheck, "nan-with-payload: payload exceeds 22-bit float NaN payload capacity");
        } else {
            const uint32_t bits = 0x7FC00000u | payload;
            *val_ptr = Object::make_real(std::bit_cast<float>(bits));
        }
    } else {
        auto payload = val_ptr->ulong_value(trx);
        // double quiet NaN: exponent all 1s, quiet bit set, payload in bits 0-50
        if (payload > 0x0007FFFFFFFFFFFFull) {
            trx->error(Error::RangeCheck, "nan-with-payload: payload exceeds 51-bit double NaN payload capacity");
        } else {
            const uint64_t bits = 0x7FF8000000000000ull | payload;
            val_ptr->free_extvalue(trx);
            *val_ptr = Object::make_double(trx, std::bit_cast<double>(bits));
        }
    }
}

// clamp: val lo hi :- val
// Clamps val to the range [lo, hi].  Works with numbers, addresses, and strings.
// throws: opstack-underflow, type-check
static void clamp_op(Trix *trx) {
    shim_ONumbersAddressString_NumbersAddressString_NumbersAddressString__RNumbersAddressString(
            trx, [](Trix *p, auto v, auto lo, auto hi) {
                using T = decltype(v);
                static_assert(std::is_same_v<T, decltype(lo)>);
                static_assert(std::is_same_v<T, decltype(hi)>);

                if constexpr (std::is_same_v<T, Object *>) {
                    // v = min(v, hi);
                    if (v->string_compare(p, hi) > 0) {
                        *v = *hi;
                    }

                    // v = max(v, lo);
                    if (v->string_compare(p, lo) < 0) {
                        *v = *lo;
                    }
                    return v;
                } else {
                    return std::clamp(v, lo, hi);
                }
            });
}

// between?: val lo hi -- bool
// Tests if lo <= val <= hi.  Works with numbers, addresses, and strings.
// throws: opstack-underflow, type-check
static void between_pred_op(Trix *trx) {
    constexpr auto V{VerifyNumbers | VerifyAddress | VerifyString};
    trx->verify_matched_operands(V, V, V);

    auto hi = trx->m_op_ptr;
    auto lo = (hi - 1);
    auto val = (lo - 1);

    // lo <= val && val <= hi  ==  !(val < lo) && !(hi < val)
    auto result = !object_less_than(trx, *val, *lo) && !object_less_than(trx, *hi, *val);

    hi->maybe_free_extvalue(trx);
    lo->maybe_free_extvalue(trx);
    val->maybe_free_extvalue(trx);
    trx->m_op_ptr -= 2;
    *trx->m_op_ptr = Object::make_boolean(result);
}

// comp-ellint-1: num :- num
// Complete elliptic integral of the first kind.
// throws: opstack-underflow, type-check
static void comp_ellint_1_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::comp_ellint_1(x); });
}

// comp-ellint-2: num :- num
// Complete elliptic integral of the second kind.
// throws: opstack-underflow, type-check
static void comp_ellint_2_op(Trix *trx) {
    shim_OFloats__RFloats(trx, VerifyNotNan, [](Trix *, auto x) { return std::comp_ellint_2(x); });
}

// comp-ellint-3: num num :- num
// Complete elliptic integral of the third kind.
// throws: opstack-underflow, type-check
static void comp_ellint_3_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::comp_ellint_3(x, y); });
}

// ellint-1: num num :- num
// Incomplete elliptic integral of the first kind.
// throws: opstack-underflow, type-check
static void ellint_1_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::ellint_1(x, y); });
}

// ellint-2: num num :- num
// Incomplete elliptic integral of the second kind.
// throws: opstack-underflow, type-check
static void ellint_2_op(Trix *trx) {
    shim_OFloats_Floats__RFloats(trx, [](Trix *, auto x, auto y) { return std::ellint_2(x, y); });
}

// ellint-3: num num num :- num
// Incomplete elliptic integral of the third kind.
// throws: opstack-underflow, type-check
static void ellint_3_op(Trix *trx) {
    shim_OFloats_Floats_Floats__RFloats(trx, [](Trix *, auto k, auto nu, auto phi) { return std::ellint_3(k, nu, phi); });
}

// homogeneous?: arr :- bool
// Tests whether all elements of the array have the same type.
// throws: opstack-underflow, type-check
static void ishomogeneous_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);

    auto top_ptr = trx->m_op_ptr;
    auto length = top_ptr->arrays_length();

    if (length == 0) {
        *top_ptr = Object::make_boolean(false);
    } else {
        Object::Type common_type;

        if (top_ptr->is_array()) {
            auto [ptr, count] = top_ptr->array_value(trx);
            common_type = ptr[0].type();
            for (length_t i = 1; i < count; ++i) {
                if (ptr[i].type() != common_type) {
                    *top_ptr = Object::make_boolean(false);
                    return;
                }
            }
        } else {
            auto packed_data = top_ptr->const_packed_span(trx).data();
            auto [next, first] = Object::extract_next_packed(trx, packed_data);
            common_type = first.type();
            for (length_t i = 1; i < length; ++i) {
                auto [next2, element] = Object::extract_next_packed(trx, next);
                auto element_type = element.type();
                if (element_type != common_type) {
                    *top_ptr = Object::make_boolean(false);
                    return;
                } else {
                    next = next2;
                }
            }
        }

        trx->require_op_capacity(1);
        *top_ptr = trx->type_name(common_type);
        *++trx->m_op_ptr = Object::make_boolean(true);
    }
}
