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

//===--- Number: Numeric Literal Parser ---===//
//
// Implements the scanner's numeric literal recognizer and parser.  Converts
// character sequences into typed numeric Objects (Byte, Integer, UInteger,
// Long, ULong, Int128, UInt128, Address, Real, Double).  Based on:
//
//   PostScript numeric syntax: decimal integers, decimal reals with optional
//   exponent, and radix notation (base#digits#).  Trix extends this with
//   type suffixes (b, i, u, l, ul, q, uq, a, r, d), C-style prefixes (0x, 0o, 0b),
//   and C99 hex float notation (0x1.0p10).
//
// --- Core concepts for maintainers ---
//
// TWO-PHASE RECOGNITION
//   The scanner calls Number::scan() to determine if a token is numeric:
//
//   Phase 1 (fast reject): The scanner accumulates character attribute hints
//     during tokenization.  If the token contains characters that cannot
//     appear in any numeric literal, scan() returns false immediately
//     without examining the token.  This avoids per-character parsing for
//     the vast majority of non-numeric tokens (names, operators, etc.).
//
//   Phase 2 (parsing): If hints indicate a possible number, scan() parses
//     the token character by character, handling sign, radix prefix, digits,
//     decimal point, exponent, and type suffix.  On success, the Number
//     object holds the parsed value and type.
//
// NUMERIC FORMATS
//   Decimal:     123, -42, 3.14, 1.5e10, -2.5E-3
//   Radix:       16#FF#, 2#1010#, 8#377# (base 2-36, PostScript syntax)
//   C-prefix:    0xFF, 0o77, 0b1010 (hex, octal, binary)
//   Hex float:   0x1.0p10, 0x1.fp-3 (C99 hexadecimal floating-point)
//   Type suffix: b (byte), i (integer), u/ui (uinteger), l (long),
//                ul (ulong), q (int128), uq (uint128), a (address),
//                r (real), d (double)
//
// TYPE INFERENCE
//   Without an explicit suffix, an integer literal is parsed as Integer;
//   values outside [-2^31, 2^31-1] raise NumericalOverflow (there is no
//   automatic widening to UInteger/Long).  Use a suffix to select a wider
//   or unsigned type: 'u' -> UInteger [0, 2^32-1], 'l'/'ul' -> Long/ULong,
//   'q'/'uq' -> Int128/UInt128.  A decimal value with a fractional part or
//   exponent is parsed as Real.
//
// OVERFLOW HANDLING
//   Integer overflow during parsing is detected and reported as a
//   NumericalOverflow error.  The parser accumulates the literal's exact
//   magnitude into an unsigned 128-bit value using __builtin_mul_overflow/
//   __builtin_add_overflow (a sticky ACCUM_WRAPPED flag records any wrap
//   past 2^128); the final value is then range-checked against each target
//   type's bounds in determine_integer_type.
//
// --- Numeric literal types ---
//
//  representation    type      literal    radix example      prefix example
//  ----------------  --------  ---------  ----------------  ----------------
//  unsigned   8-bit  byte      1b         2#0111#b          0b0111#b
//    signed  32-bit  integer   1[i]       8#0377#[i]        0o377[#i]
//  unsigned  32-bit  uinteger  1u[i]      16#0123#u[i]      0x0123#u[i]
//    signed  64-bit  long      1l         16#0123#l         0x0123#l
//  unsigned  64-bit  ulong     1ul        16#0123#ul        0x0123#ul
//    signed 128-bit  int128    1q         16#0123#q         0x0123#q
//  unsigned 128-bit  uint128   1uq        16#0123#uq        0x0123#uq
//   address  64-bit  address   1a         16#0123#a         0x0123#a
//  IEEE-754  32-bit  real      1.0[r]     -- (no radix form) 0x1.0p0[#r]
//  IEEE-754  64-bit  double    1.0d       -- (no radix form) 0x1.0p0#d
//
//  Prefix forms: 0x (hex), 0o (octal), 0b (binary).  Case insensitive (0X, 0O, 0B).
//  Type suffix requires '#' separator: 0xFF#b (byte 255), 0o77#ul (ulong 63).
//  Without '#', suffix letters are digits: 0xFFb = hex FFB = 4091.
//  Special case: bare '0b' is byte 0 (existing suffix), not empty binary prefix.
//  Hex floats: 0x digits[.frac] p [+-] exp [#d|#r].  'p' exponent is mandatory.

//===--- Number: Numeric Literal Parser / Formatter ---===//
class Number {
public:
    Number(Trix *trx) : m_trx{trx} { reset(); }

    Number(Trix *trx, std::string_view buffer) : m_trx{trx} {
        reset();
        if (!scan(buffer)) {
            m_type = Object::Type::Null;
        }
    }

    Number(Trix *trx, std::string_view buffer, Object::Type specified_type) : m_trx{trx} {
        reset();
        if (!scan(buffer, specified_type)) {
            m_type = Object::Type::Null;
        }
    }

    Number() = delete;

    void reset() {
        m_ulong = 0;
        m_type = Object::Type::Null;
    }

    // Attempts to parse buffer as a numeric literal.
    // Returns true and sets m_type/value fields if buffer is a valid number; false if it
    //   is a name or contains characters that rule out all numeric forms.
    // Raises: NumericalOverflow via trx->error() if the literal is syntactically valid but
    //   its value is out of range for the target type (e.g. 2147483648 for Integer,
    //   3.5e38r for Real).  Does NOT return false for overflow -- it throws.
    [[nodiscard]] bool scan(std::string_view buffer) { return scan_impl(buffer, Object::Type::Null); }

    // Strict-typed scan: parses buffer with specified_type as a hint (equivalent to an
    // implicit type suffix when the input carries none), but performs NO coercion if the
    // natural parse produces a different type.
    // Returns true only when scan_impl succeeds AND the parsed type equals specified_type.
    // Intended for callers that must honor a declared target type without implicit
    // conversion (e.g. ScanFmt field specifiers under strict type safety).
    // Raises: NumericalOverflow for overflow (inherited from scan_impl).
    [[nodiscard]] bool scan_strict(std::string_view buffer, Object::Type specified_type) {
        assert(((VerifyNumbers | VerifyAddress) & (1ull << +specified_type)) != 0);

        return (scan_impl(buffer, specified_type) && (type() == specified_type));
    }

    // Parses buffer as hexadecimal digits with an optional "0x"/"0X" prefix.
    // type_hint acts as an implicit suffix when the input carries none (mirroring
    // determine_integer_type's hint-as-suffix behavior).
    // Returns true on success and sets m_type/value fields; false if the payload
    // is empty or contains a non-hex-digit/non-suffix character.
    // Intended for ScanFmt :x type specifier.
    [[nodiscard]] bool scan_hex(std::string_view buffer, Object::Type type_hint = Object::Type::Null) {
        reset();
        auto payload = buffer;
        if ((sv_length(payload) >= 2) && (payload[0] == '0') && ((payload[1] == 'x') || (payload[1] == 'X'))) {
            payload.remove_prefix(2);
        }
        if (sv_length(payload) == 0) {
            return false;
        } else {
            return scan_integer_value_and_suffix(payload, 16, type_hint);
        }
    }

    // Strict variant of scan_hex: succeeds only when the parsed type exactly matches
    // specified_type.  No coercion is performed.
    [[nodiscard]] bool scan_hex_strict(std::string_view buffer, Object::Type specified_type) {
        assert(((VerifyIntegers | VerifyAddress) & (1ull << +specified_type)) != 0);

        return (scan_hex(buffer, specified_type) && (type() == specified_type));
    }

    // Attempts to parse buffer as a numeric literal of the given type.
    // Parses as scan(buffer), then coerces the result to specified_type via cast_to_type.
    // specified_type must be in VerifyNull | VerifyNumbers | VerifyAddress.
    // Returns true on success; false if the token is not a valid number.
    // Raises: NumericalOverflow for overflow; TypeCheck if the parsed type cannot be
    //   coerced to specified_type (e.g. Address coercion from a non-Address parse).
    [[nodiscard]] bool scan(std::string_view buffer, Object::Type specified_type) {
        assert(((VerifyNull | VerifyNumbers | VerifyAddress) & (1ull << +specified_type)) != 0);

        auto result = scan_impl(buffer, specified_type);
        if (result && (specified_type != Object::Type::Null)) {
            auto actual_type = type();
            if (specified_type != actual_type) {
                switch (+specified_type) {
                case +Object::Type::Byte: {
                    auto b = cast_to_type<vm_t>();
                    m_byte = b;
                    m_type = specified_type;
                    break;
                }

                case +Object::Type::Integer: {
                    auto i = cast_to_type<integer_t>();
                    m_integer = i;
                    m_type = specified_type;
                    break;
                }

                case +Object::Type::UInteger: {
                    auto ui = cast_to_type<uinteger_t>();
                    m_uinteger = ui;
                    m_type = specified_type;
                    break;
                }

                case +Object::Type::Long: {
                    auto l = cast_to_type<long_t>();
                    m_long = l;
                    m_type = specified_type;
                    break;
                }

                case +Object::Type::ULong: {
                    auto ul = cast_to_type<ulong_t>();
                    m_ulong = ul;
                    m_type = specified_type;
                    break;
                }

                case +Object::Type::Int128: {
                    auto h = cast_to_type<int128_t>();
                    m_int128 = h;
                    m_type = specified_type;
                    break;
                }

                case +Object::Type::UInt128: {
                    auto uh = cast_to_type<uint128_t>();
                    m_uint128 = uh;
                    m_type = specified_type;
                    break;
                }

                case +Object::Type::Real: {
                    auto r = cast_to_type<real_t>();
                    m_real = r;
                    m_type = specified_type;
                    break;
                }

                case +Object::Type::Double: {
                    auto d = cast_to_type<double_t>();
                    m_double = d;
                    m_type = specified_type;
                    break;
                }

                default:
                    m_trx->error(Error::TypeCheck, "Number::scan: cannot coerce to non-numeric type");
                }
            }
        }
        return result;
    }

    [[nodiscard]] bool is_valid() const { return (m_type != Object::Type::Null); }

    [[nodiscard]] Object::Type type() const {
        assert(m_type != Object::Type::Null);

        [[assume(m_type != Object::Type::Null)]];

        return m_type;
    }

    [[nodiscard]] vm_t byte_value() const {
        assert(m_type == Object::Type::Byte);

        return m_byte;
    }

    [[nodiscard]] integer_t integer_value() const {
        assert(m_type == Object::Type::Integer);

        return m_integer;
    }

    [[nodiscard]] uinteger_t uinteger_value() const {
        assert(m_type == Object::Type::UInteger);

        return m_uinteger;
    }

    [[nodiscard]] long_t long_value() const {
        assert(m_type == Object::Type::Long);

        return m_long;
    }

    [[nodiscard]] ulong_t ulong_value() const {
        assert(m_type == Object::Type::ULong);

        return m_ulong;
    }

    [[nodiscard]] address_t address_value() const {
        assert(m_type == Object::Type::Address);

        return m_address;
    }

    [[nodiscard]] real_t real_value() const {
        assert(m_type == Object::Type::Real);

        return m_real;
    }

    [[nodiscard]] double_t double_value() const {
        assert(m_type == Object::Type::Double);

        return m_double;
    }

    [[nodiscard]] int128_t int128_value() const {
        assert(m_type == Object::Type::Int128);

        return m_int128;
    }

    [[nodiscard]] uint128_t uint128_value() const {
        assert(m_type == Object::Type::UInt128);

        return m_uint128;
    }

    [[nodiscard]] std::pair<Error, integer_t> integer_value(integer_t lower, integer_t upper) {
        auto integer_min = std::numeric_limits<integer_t>::min();
        auto integer_max = std::numeric_limits<integer_t>::max();
        auto value = integer_t{0};
        auto err = Error::NoError;

        switch (+m_type) {
        case +Object::Type::Byte:
            value = static_cast<integer_t>(m_byte);
            break;

        case +Object::Type::Integer:
            value = m_integer;
            break;

        case +Object::Type::UInteger:
            if (m_uinteger <= static_cast<uinteger_t>(integer_max)) {
                value = static_cast<integer_t>(m_uinteger);
            } else {
                err = Error::RangeCheck;
            }
            break;

        case +Object::Type::Long:
            if ((m_long >= static_cast<long_t>(integer_min)) && (m_long <= static_cast<long_t>(integer_max))) {
                value = static_cast<integer_t>(m_long);
            } else {
                err = Error::RangeCheck;
            }
            break;

        case +Object::Type::ULong:
            if (m_ulong <= static_cast<ulong_t>(integer_max)) {
                value = static_cast<integer_t>(m_ulong);
            } else {
                err = Error::RangeCheck;
            }
            break;

        case +Object::Type::Int128:
            if ((m_int128 >= static_cast<int128_t>(integer_min)) && (m_int128 <= static_cast<int128_t>(integer_max))) {
                value = static_cast<integer_t>(m_int128);
            } else {
                err = Error::RangeCheck;
            }
            break;

        case +Object::Type::UInt128:
            if (m_uint128 <= static_cast<uint128_t>(integer_max)) {
                value = static_cast<integer_t>(m_uint128);
            } else {
                err = Error::RangeCheck;
            }
            break;

        case +Object::Type::Address:
        case +Object::Type::Real:
        case +Object::Type::Double:
            err = Error::TypeCheck;
            break;

        case +Object::Type::Null:
            err = Error::Undefined;
            break;

        default:
            assert(false && "integer_value(lower, upper) unknown Number type");
            std::unreachable();
            break;
        }

        if ((err == Error::NoError) && ((value < lower) || (value > upper))) {
            err = Error::RangeCheck;
        }

        return std::pair{err, value};
    }

    [[nodiscard]] Object make_object() const {
        assert(m_type != Object::Type::Null);

        switch (+m_type) {
        case +Object::Type::Byte:
            return Object::make_byte(m_byte);

        case +Object::Type::Integer:
            return Object::make_integer(m_integer);

        case +Object::Type::UInteger:
            return Object::make_uinteger(m_uinteger);

        case +Object::Type::Long:
            return Object::make_long(m_trx, m_long);

        case +Object::Type::ULong:
            return Object::make_ulong(m_trx, m_ulong);

        case +Object::Type::Int128:
            return Object::make_int128(m_trx, m_int128);

        case +Object::Type::UInt128:
            return Object::make_uint128(m_trx, m_uint128);

        case +Object::Type::Address:
            return Object::make_address(m_trx, m_address);

        case +Object::Type::Real:
            return Object::make_real(m_real);

        case +Object::Type::Double:
            return Object::make_double(m_trx, m_double);

        case +Object::Type::Null:
        default:
            assert(false && "Number::make_object: unknown Number type");
            std::unreachable();
            break;
        }
    }

    [[nodiscard]] static consteval bool verify_hint_table() {
        for (int ch = 0; ch < 128; ++ch) {
            auto expected = TERMINATOR;
            if (Trix::is_integer(ch)) {
                expected = MAYBE_INTEGER;
            } else if (ch == '.') {
                expected = (CANTBE_INTEGER | CANTBE_RADIX | MAYBE_REAL);
            } else if (ch == '#') {
                expected = (CANTBE_INTEGER | MAYBE_RADIX | CANTBE_REAL);
            } else if (Trix::is_alpha(ch)) {
                const auto lc = Trix::to_lowercase(ch);
                if (Trix::is_integral_suffix(lc)) {
                    expected = CANTBE_REAL;
                } else if (Trix::is_floatingpoint_suffix(lc)) {
                    expected = (CANTBE_INTEGER | MAYBE_REAL);
                } else {
                    expected = (CANTBE_INTEGER | CANTBE_REAL);
                }
            } else if (Trix::is_regular(ch)) {
                expected = MUSTBE_NAME;
            } else {
                expected = TERMINATOR;
            }
            if (get_hint(ch) != expected) {
                return false;
            }
        }
        return (get_hint(EOFc) == TERMINATOR);
    }
private:
    [[nodiscard]] bool scan_impl(std::string_view buffer, Object::Type type_hint) {
        auto buffer_data = reinterpret_cast<const vm_t *>(buffer.data());
        auto length = sv_length(buffer);
        auto maybe_prefix = ((length >= 3) && (buffer_data[0] == '0'));

        m_type = Object::Type::Null;
        m_hints = 0;
        for (length_t i = 0; i < length; ++i) {
            auto hint = get_hint(buffer_data[i]);
            if (hint == MUSTBE_NAME) {
                return false;
            } else if (hint == TERMINATOR) {
                if (m_hints == 0) {
                    return false;
                } else {
                    length = i;
                    if (length < 3) {
                        maybe_prefix = false;
                    }
                    break;
                }
            } else {
                m_hints |= hint;
                if (!maybe_prefix && ((m_hints & CANTBE_ANY) == CANTBE_ANY)) {
                    // Every hint-driven path is permanently dead and no prefix fallback is possible.
                    return false;
                }
            }
        }

        // MUSTBE_NAME cannot be set here: the scan loop returns false immediately on
        // any MUSTBE_NAME character (line above), so it never accumulates into m_hints.
        if ((m_hints & (MAYBE_INTEGER | MAYBE_RADIX | MAYBE_REAL)) != 0) {
            if (((m_hints & (MAYBE_INTEGER | CANTBE_INTEGER)) == MAYBE_INTEGER) && scan_integer(buffer, type_hint)) {
                // found a byte, integer, uinteger, long, ulong, or address
                return true;
            } else if (((m_hints & (MAYBE_REAL | CANTBE_REAL)) == MAYBE_REAL) && scan_real(buffer, type_hint)) {
                // found a real, or double
                return true;
            } else if (((m_hints & (MAYBE_RADIX | CANTBE_RADIX)) == MAYBE_RADIX) && scan_radix(buffer)) {
                // found a byte, integer, uinteger, long, ulong, or address
                return true;
            }
        }
        return (maybe_prefix && scan_prefix(buffer, type_hint));
    }

    template<typename T>
    [[nodiscard]] T cast_to_type() {
        static_assert(!std::is_same_v<T, address_t>, "T can not be cast to/from Address");
        assert((m_type != Object::Type::Null) && (m_type != Object::Type::Address));

        switch (+m_type) {
        case +Object::Type::Byte:
            return static_cast<T>(m_byte);

        case +Object::Type::Integer:
            return static_cast<T>(m_integer);

        case +Object::Type::UInteger:
            return static_cast<T>(m_uinteger);

        case +Object::Type::Long:
            return static_cast<T>(m_long);

        case +Object::Type::ULong:
            return static_cast<T>(m_ulong);

        case +Object::Type::Int128:
            return static_cast<T>(m_int128);

        case +Object::Type::UInt128:
            return static_cast<T>(m_uint128);

        case +Object::Type::Real:
            if constexpr (std::is_integral_v<T>) {
                // Casting a float to an integer is UB when the value (after truncation toward
                // zero) is not representable in T.  Validate range first using double precision
                // to avoid precision loss in the boundary comparisons.
                const auto dval = static_cast<double>(m_real);
                constexpr double lo = static_cast<double>(std::numeric_limits<T>::min());
                constexpr double hi = static_cast<double>(std::numeric_limits<T>::max()) + 1.0;
                if (!((dval >= lo) && (dval < hi))) {
                    m_trx->error(Error::RangeCheck, "Number::cast_to_type: real {} out of range for target integer type", m_real);
                }
            }
            return static_cast<T>(m_real);

        case +Object::Type::Double:
            if constexpr (std::is_integral_v<T>) {
                constexpr double lo = static_cast<double>(std::numeric_limits<T>::min());
                constexpr double hi = static_cast<double>(std::numeric_limits<T>::max()) + 1.0;
                if (!((m_double >= lo) && (m_double < hi))) {
                    m_trx->error(
                            Error::RangeCheck, "Number::cast_to_type: double {} out of range for target integer type", m_double);
                }
            }
            return static_cast<T>(m_double);

        default:
            assert(false && "Number::cast_to_type: unknown Number type");
            std::unreachable();
            break;
        }
    }

    // Status flags for integer value parsing and overflow detection.
    using status_mask_t = uint32_t;
    static constexpr status_mask_t NONE{0x00000000};
    static constexpr status_mask_t IS_NEGATIVE{0x00000001};
    static constexpr status_mask_t SUFFIX_U{0x00000002};
    static constexpr status_mask_t SUFFIX_B{0x00000004};
    static constexpr status_mask_t SUFFIX_I{0x00000008};
    static constexpr status_mask_t SUFFIX_L{0x00000010};
    static constexpr status_mask_t SUFFIX_A{0x00000020};
    static constexpr status_mask_t SUFFIX_Q{0x00000040};
    static constexpr status_mask_t ACCUM_WRAPPED{0x00000100};  // u128 accumulator lost bits past 2^128
    static constexpr status_mask_t SUFFIX_MASK{SUFFIX_U | SUFFIX_B | SUFFIX_I | SUFFIX_L | SUFFIX_A | SUFFIX_Q};
    static constexpr ulong_t BYTE_MASK{0xFF};
    static constexpr ulong_t INTEGER_MASK{0xFFFFFFFF};
    static constexpr ulong_t INTEGER_SIGN_BIT{1ull << 31};
    static constexpr ulong_t LONG_SIGN_BIT{1ull << 63};
    static constexpr uint128_t INT128_SIGN_BIT{uint128_t{1} << 127};

    // Given the accumulated value and status flags after parsing all digits and suffixes,
    // determine the final integer type, apply type hints, and check for overflow.
    void determine_integer_type(uint128_t u128, status_mask_t status_mask, Object::Type type_hint) {
        // When no explicit suffix is present and a type hint is provided
        // (e.g. from ScanFmt target type), apply the hint to guide type
        // deduction and overflow checking.
        if (((status_mask & SUFFIX_MASK) == 0) && (type_hint != Object::Type::Null)) {
            switch (+type_hint) {
            case +Object::Type::Byte:
                status_mask |= SUFFIX_B;
                break;

            case +Object::Type::UInteger:
                status_mask |= SUFFIX_U;
                break;

            case +Object::Type::Long:
                status_mask |= SUFFIX_L;
                break;

            case +Object::Type::ULong:
                status_mask |= (SUFFIX_U | SUFFIX_L);
                break;

            case +Object::Type::Address:
                status_mask |= SUFFIX_A;
                break;

            case +Object::Type::Int128:
                status_mask |= SUFFIX_Q;
                break;

            case +Object::Type::UInt128:
                status_mask |= (SUFFIX_U | SUFFIX_Q);
                break;

            default:
                break;
            }
        }

        auto is_byte = ((status_mask & SUFFIX_B) != 0);
        auto is_long = ((status_mask & SUFFIX_L) != 0);
        auto is_int128 = ((status_mask & SUFFIX_Q) != 0);
        auto is_address = ((status_mask & SUFFIX_A) != 0);
        auto is_unsigned = ((status_mask & SUFFIX_U) != 0);
        auto is_negative = ((status_mask & IS_NEGATIVE) != 0);
        // The accumulator holds the literal's exact magnitude unless it
        // wrapped past 2^128 (ACCUM_WRAPPED), so every type's overflow test
        // is a direct value comparison against that type's bound.  Negative
        // literals reach only the signed arms (negative + unsigned/byte/
        // address suffixes fall back to names upstream); each signed arm
        // admits its MIN's magnitude (sign-bit pattern already correct,
        // negating it would be signed-overflow UB) and negates the rest.
        auto wrapped = ((status_mask & ACCUM_WRAPPED) != 0);
        auto overflow = wrapped;
        auto ul = static_cast<ulong_t>(u128);
        if (is_byte) {
            overflow = overflow || (u128 > BYTE_MASK);
            m_type = Object::Type::Byte;
            m_byte = static_cast<vm_t>(ul & BYTE_MASK);
        } else if (is_int128) {
            if (is_unsigned) {
                m_type = Object::Type::UInt128;
                m_uint128 = u128;
            } else {
                m_type = Object::Type::Int128;
                m_int128 = static_cast<int128_t>(u128);
                if (is_negative) {
                    overflow = overflow || (u128 > INT128_SIGN_BIT);
                    if (u128 != INT128_SIGN_BIT) {
                        m_int128 = -m_int128;
                    }
                } else {
                    overflow = overflow || (u128 >= INT128_SIGN_BIT);
                }
            }
        } else if (is_long) {
            if (is_unsigned) {
                overflow = overflow || (u128 > uint128_t{~ulong_t{0}});
                m_type = Object::Type::ULong;
                m_ulong = ul;
            } else {
                m_type = Object::Type::Long;
                m_long = static_cast<long_t>(ul);
                if (is_negative) {
                    overflow = overflow || (u128 > uint128_t{LONG_SIGN_BIT});
                    if (ul != LONG_SIGN_BIT) {
                        m_long = -m_long;
                    }
                } else {
                    overflow = overflow || (u128 >= uint128_t{LONG_SIGN_BIT});
                }
            }
        } else if (is_address) {
            overflow = overflow || (u128 > uint128_t{~ulong_t{0}});
            m_type = Object::Type::Address;
            m_address = reinterpret_cast<address_t>(ul);
        } else if (is_unsigned) {
            overflow = overflow || (u128 > uint128_t{INTEGER_MASK});
            m_type = Object::Type::UInteger;
            m_uinteger = static_cast<uinteger_t>(ul & INTEGER_MASK);
        } else {
            m_type = Object::Type::Integer;
            m_integer = static_cast<integer_t>(ul & INTEGER_MASK);
            if (is_negative) {
                overflow = overflow || (u128 > uint128_t{INTEGER_SIGN_BIT});
                if (u128 != uint128_t{INTEGER_SIGN_BIT}) {
                    m_integer = -m_integer;
                }
            } else {
                overflow = overflow || (u128 >= uint128_t{INTEGER_SIGN_BIT});
            }
        }

        if (overflow) {
            auto sv = Object::type_sv(m_type);
            m_trx->error(Error::NumericalOverflow, "numerical overflow encountered while scanning a {}", sv);
        }
    }

    // Parses an integer value and its optional type suffix from buffer.
    // radix: 0 means base-10 (decimal) with a digits[suffix] form; non-zero means the
    //   caller has already parsed "base#" and is passing only the digits[#suffix] tail.
    // Returns true on success; false if buffer is not a valid integer literal.
    // Raises: NumericalOverflow via trx->error() if the value is out of range for the
    //   suffix-determined type.
    // integral number suffix
    //
    // number form: digits[suffix]
    //  radix form: base#digits[#suffix]
    //
    // suffix  type
    // ------  ----
    //      b  byte
    //    [i]  integer (default)
    //   u[i]  uinteger
    //      l  long
    //     ul  ulong
    //      a  address
    [[nodiscard]] bool
    scan_integer_value_and_suffix(std::string_view buffer, uinteger_t radix = 0, Object::Type type_hint = Object::Type::Null) {
        auto length = sv_length(buffer);
        assert(length != 0);

        auto valid = false;
        auto ptr = reinterpret_cast<const vm_t *>(buffer.data());
        auto limit = ptr + length;
        auto ch = *ptr;
        auto is_negative = (ch == '-');
        auto status_mask = status_mask_t{is_negative ? IS_NEGATIVE : NONE};

        // process leading sign, if present
        if (is_negative || (ch == '+')) {
            ++ptr;
        }

        auto is_radix_form = true;
        auto process_suffix = false;
        if (radix == 0) {
            // scanning a base-10 number[suffix]
            radix = 10;
            is_radix_form = false;
            process_suffix = true;
        }

        auto u128 = uint128_t{0};
        while (ptr < limit) {
            ch = *ptr++;
            if (Trix::is_digit_or_alpha(ch)) {
                if (Trix::is_digit(ch) && ((status_mask & SUFFIX_MASK) != 0)) {
                    // digit after suffix: is a name
                    return false;
                } else if (Trix::is_alpha(ch) && ((is_radix_form && process_suffix) || !is_radix_form)) {
                    // process suffix
                    ch = Trix::to_lowercase(ch);
                    if ((ch == 'b') || (ch == 'u') || (ch == 'a')) {
                        if (((status_mask & SUFFIX_MASK) == 0) && ((status_mask & IS_NEGATIVE) == 0)) {
                            if (ch == 'b') {
                                status_mask |= SUFFIX_B;
                            } else if (ch == 'u') {
                                status_mask |= SUFFIX_U;
                            } else {
                                assert(ch == 'a');

                                status_mask |= SUFFIX_A;
                            }
                        } else {
                            // suffix already present or has leading minus sign: is a name
                            return false;
                        }
                    } else if ((ch == 'i') || (ch == 'l') || (ch == 'q')) {
                        if ((status_mask & (SUFFIX_B | SUFFIX_I | SUFFIX_L | SUFFIX_A | SUFFIX_Q)) == 0) {
                            if (ch == 'i') {
                                status_mask |= SUFFIX_I;
                            } else if (ch == 'l') {
                                status_mask |= SUFFIX_L;
                            } else {
                                status_mask |= SUFFIX_Q;
                            }
                        } else {
                            // suffix already present: is a name
                            return false;
                        }
                    } else {
                        return false;
                    }
                } else {
                    uinteger_t value = ch;
                    if (Trix::is_digit(ch)) {
                        value = static_cast<uinteger_t>(ch - '0');
                    } else if (Trix::is_uppercase(ch)) {
                        value = static_cast<uinteger_t>(ch - 'A' + 10);
                    } else {
                        value = static_cast<uinteger_t>(ch - 'a' + 10);
                    }

                    if (value < radix) {
                        // Exact 128-bit accumulation: u128 = u128*radix + value with a
                        // sticky lost-bits flag.  The accumulator is the single source
                        // of truth -- determine_integer_type compares the final value
                        // against each type's exact bounds, so the only failure mode
                        // left to detect here is the accumulator itself wrapping past
                        // 2^128.  (The previous per-width prior>=current wrap heuristic
                        // missed any radix>=16 step whose masked value still increased
                        // -- 0x1FF#b, 0x1FFFFFFFF#u, 33-hex-digit #uq literals -- and
                        // silently truncated unsigned suffixed literals.)
                        uint128_t scaled{0};
                        if (__builtin_mul_overflow(u128, static_cast<uint128_t>(radix), &scaled) ||
                            __builtin_add_overflow(scaled, static_cast<uint128_t>(value), &u128)) {
                            status_mask |= ACCUM_WRAPPED;
                        }
                        valid = true;
                    } else {
                        // digit/letter out of radix range: is a name
                        return false;
                    }
                }
            } else if (ch == '#') {
                if (is_radix_form && !process_suffix) {
                    // radix numbers are base#digits#suffix
                    process_suffix = true;
                } else {
                    // could be a radix or a name
                    return false;
                }
            } else if (Trix::is_terminator(ch)) {
                // white space, delimiter, binary-token, syntax-error ends integer sequence
                break;
            } else {
                // could be a radix, a real, or a name
                return false;
            }
        }

        if (valid) {
            determine_integer_type(u128, status_mask, type_hint);
        }
        return valid;
    }

    // Delegates to scan_integer_value_and_suffix with default base-10 radix.
    // Returns true on success; false if buffer is not a valid integer literal.
    // Raises: NumericalOverflow if the value is out of range for the target type.
    [[nodiscard]] bool scan_integer(std::string_view buffer, Object::Type type_hint = Object::Type::Null) {
        return scan_integer_value_and_suffix(buffer, 0, type_hint);
    }

    // Parses a radix literal of the form base#digits[#suffix] (e.g. 16#FF, 2#1010#ul).
    // Returns true on success; false if buffer is not a valid radix literal.
    // Raises: NumericalOverflow if the value is out of range for the target type.
    [[nodiscard]] bool scan_radix(std::string_view buffer) {
        auto length = sv_length(buffer);
        assert(length != 0);

        auto valid = false;
        auto ptr = reinterpret_cast<const vm_t *>(buffer.data());
        auto limit = ptr + length;
        auto radix = uinteger_t{0};
        while (ptr < limit) {
            auto ch = *ptr++;
            if (Trix::is_digit(ch)) {
                // base-10 radix
                radix = (radix * 10) + static_cast<uinteger_t>(ch - '0');
                if (radix > 36) {
                    break;
                }
            } else {
                if ((ch == '#') && (radix >= 2) && (radix <= 36)) {
                    length = static_cast<length_t>(limit - ptr);
                    if (length != 0) {
                        valid = scan_integer_value_and_suffix(make_sv(ptr, length), radix);
                    }
                }
                break;
            }
        }
        return valid;
    }

    // Parses a prefix literal: 0x (hex), 0o (octal), 0b (binary).
    // Format: 0x digits [# suffix], 0o digits [# suffix], 0b digits [# suffix].
    // Does not support leading sign (use 'neg' operator for negative values).
    // The '#' separator is required before type suffixes to avoid ambiguity with
    // hex digits (e.g. 0xFFb is hex FFB, not hex FF as byte -- use 0xFF#b).
    // Special case: bare '0b' is byte 0 (suffix), not binary prefix; disambiguation
    // requires a third character that is a valid binary digit (0 or 1).
    // Returns true on success; false if buffer is not a valid prefix literal.
    // Raises: NumericalOverflow if the value is out of range for the target type.
    [[nodiscard]] bool scan_prefix(std::string_view buffer, Object::Type type_hint = Object::Type::Null) {
        auto length = sv_length(buffer);
        if ((length < 3) || (buffer[0] != '0')) {
            return false;
        } else {
            auto prefix_char = Trix::to_lowercase(static_cast<vm_t>(buffer[1]));
            auto radix = uinteger_t{0};

            if (prefix_char == 'x') {
                // Check for hex float: digits[.frac]p[+-]exp.  Scan ahead for
                // 'p'/'P'; if found after only hex digits and at most one '.',
                // route to the hex float parser instead of the integer path.
                auto remaining = buffer.substr(2);
                auto rdata = reinterpret_cast<const vm_t *>(remaining.data());
                auto rlen = sv_length(remaining);
                for (length_t i = 0; i < rlen; ++i) {
                    auto ch = rdata[i];
                    if (Trix::to_lowercase(ch) == 'p') {
                        return scan_hex_float(remaining, type_hint);
                    } else if (!Trix::is_hexdigit(ch) && (ch != '.')) {
                        break;
                    }
                }
                radix = 16;
            } else if (prefix_char == 'o') {
                radix = 8;
            } else if (prefix_char == 'b') {
                // Disambiguate from '0b' (byte suffix for zero):
                // only enter binary-prefix path if third character is a binary digit.
                auto third = static_cast<vm_t>(buffer[2]);
                if ((third != '0') && (third != '1')) {
                    return false;
                } else {
                    radix = 2;
                }
            } else {
                return false;
            }

            // Delegate digits[#suffix] to existing radix-form scanner.
            return scan_integer_value_and_suffix(buffer.substr(2), radix, type_hint);
        }
    }

    // Finalize a floating-point parse: check for overflow/underflow, store the result.
    // Shared by scan_hex_float and scan_real after std::from_chars completes.
    [[nodiscard]] bool finalize_float_parse(double_t d, std::errc ec, bool is_double) {
        if (ec == std::errc::result_out_of_range) {
            auto type_name = is_double ? "'double'" : "'real'";
            m_trx->error(Error::NumericalOverflow, "numerical overflow encountered while scanning a {}", type_name);
        } else {
            if (is_double) {
                m_type = Object::Type::Double;
                m_double = d;
            } else {
                m_type = Object::Type::Real;
                m_real = static_cast<real_t>(d);
                if (std::isinf(m_real) || ((m_real == 0.0f) && (d != 0.0))) {
                    m_trx->error(Error::NumericalOverflow, "numerical overflow encountered while scanning a 'real'");
                }
            }
            return true;
        }
    }

    // Parses a hex float literal: digits[.frac] p [+-] exp [# suffix].
    // Input is the buffer after the '0x' prefix has been stripped.
    // The 'p'/'P' exponent is mandatory (caller verifies presence before calling).
    // Default type is Real; '#d' selects Double, '#r' is explicit Real.  With no
    // explicit suffix, a Double type_hint acts as an implicit '#d' (mirroring scan_real).
    // Returns true on success; false if buffer is not a valid hex float literal.
    // Raises: NumericalOverflow if the value overflows to +/-Inf.
    [[nodiscard]] bool scan_hex_float(std::string_view digits, Object::Type type_hint = Object::Type::Null) {
        auto first = digits.data();
        auto last = first + sv_length(digits);

        double_t d{};
        auto [ptr, ec] = std::from_chars(first, last, d, std::chars_format::hex);
        if (ptr == first) {
            return false;
        } else {
            // Check for optional '#' + type suffix (d = double, r = real).  With no
            // explicit suffix, a Double type_hint acts as an implicit '#d' (see scan_real).
            auto is_double{false};
            auto has_explicit_suffix{false};
            if (ptr < last) {
                if (*ptr == '#') {
                    ++ptr;
                    if (ptr < last) {
                        auto ch = Trix::to_lowercase(static_cast<vm_t>(*ptr));
                        if (ch == 'd') {
                            is_double = true;
                            has_explicit_suffix = true;
                            ++ptr;
                        } else if (ch == 'r') {
                            has_explicit_suffix = true;
                            ++ptr;
                        } else {
                            return false;
                        }
                    } else {
                        return false;
                    }
                } else if (!Trix::is_terminator(*ptr)) {
                    return false;
                }
            }

            if (!has_explicit_suffix && (type_hint == Object::Type::Double)) {
                is_double = true;
            }

            return finalize_float_parse(d, ec, is_double);
        }
    }

    // floating point number suffix
    //
    // number form: digits[#suffix]
    //  radix form: n/a
    //
    // suffix  type              examples
    // ------  ----              --------
    //    [r]    real (default)  -.002  34.5  -3.62  123.6e10  1.0E-5  1E6  -1.  +1.
    //      d  double            -.002d 34.5D -3.62d 123.6e10D 1.0E-5d 1E6D -1.d +1.d
    // Parses a floating-point literal (e.g. 1.5, -3.2e10, 1.0d).
    // Returns true on success; false if buffer is not a valid real or double literal.
    // Raises: NumericalOverflow if the value overflows to +/-Inf (for both Real and Double).
    [[nodiscard]] bool scan_real(std::string_view buffer, Object::Type type_hint = Object::Type::Null) {
        auto first = buffer.data();
        auto last = (first + sv_length(buffer));

        {
            auto ptr = first;
            if (*ptr == '+') {
                // skip a leading '+' since from_chars does not accept it
                ++first;
                ++ptr;
            } else if (*ptr == '-') {
                ++ptr;
            }

            auto valid_fp = false;
            while (ptr < last) {
                if (Trix::is_frac_or_exp(*ptr)) {
                    // found . or e or E before suffix or terminator
                    valid_fp = true;
                    break;
                } else if (Trix::is_alpha(*ptr) || Trix::is_terminator(*ptr)) {
                    break;
                } else {
                    ++ptr;
                }
            }
            if (!valid_fp) {
                return false;
            }
        }

        double_t d{};
        auto [ptr, ec] = std::from_chars(first, last, d);
        if (ptr == first) {
            return false;
        } else {
            // check for optional type suffix
            auto is_double = false;
            auto has_explicit_suffix = false;
            if (ptr < last) {
                auto ch = Trix::to_lowercase(static_cast<unsigned char>(*ptr));
                if (ch == 'd') {
                    is_double = true;
                    has_explicit_suffix = true;
                    ++ptr;
                } else if (ch == 'r') {
                    has_explicit_suffix = true;
                    ++ptr;
                } else if (!Trix::is_terminator(*ptr)) {
                    // white space, delimiter, binary-token, syntax-error ends real sequence
                    return false;
                }
                if ((ptr < last) && !Trix::is_terminator(*ptr)) {
                    // could be a radix, or a name
                    return false;
                }
            }

            // When the input lacks an explicit d/r suffix, a Double type hint acts as an
            // implicit suffix (mirroring determine_integer_type's hint-as-suffix behavior).
            if (!has_explicit_suffix && (type_hint == Object::Type::Double)) {
                is_double = true;
            }

            return finalize_float_parse(d, ec, is_double);
        }
    }

    using hint_t = uint8_t;

    static constexpr hint_t TERMINATOR{0x00};
    static constexpr hint_t MAYBE_INTEGER{0x01};
    static constexpr hint_t MAYBE_RADIX{0x02};
    static constexpr hint_t MAYBE_REAL{0x04};
    static constexpr hint_t CANTBE_INTEGER{0x08};
    static constexpr hint_t CANTBE_RADIX{0x10};
    static constexpr hint_t CANTBE_REAL{0x20};
    static constexpr hint_t MUSTBE_NAME{0x40};
    static constexpr hint_t CANTBE_ANY{CANTBE_INTEGER | CANTBE_RADIX | CANTBE_REAL};

    static constexpr hint_t sm_hint_data[1 + 128] = {
            TERMINATOR,                                    // -1     EOF End of File
            TERMINATOR,                                    // 00  ^@ NUL Null
            TERMINATOR,                                    // 01  ^A SOH Start of Heading
            TERMINATOR,                                    // 02  ^B STX Start of Text
            TERMINATOR,                                    // 03  ^C ETX End of Text
            TERMINATOR,                                    // 04  ^D EOT End of Transmission
            TERMINATOR,                                    // 05  ^E ENQ Enquiry
            TERMINATOR,                                    // 06  ^F ACK Acknowledgement
            TERMINATOR,                                    // 07  ^G BEL Bell
            TERMINATOR,                                    // 08  ^H BS  Backspace
            TERMINATOR,                                    // 09  ^I HT  Horizontal Tab
            TERMINATOR,                                    // 0A  ^J LF  Line Feed
            TERMINATOR,                                    // 0B  ^K VT  Vertical Tab
            TERMINATOR,                                    // 0C  ^L FF  Form Feed
            TERMINATOR,                                    // 0D  ^M CR  Carriage Return
            TERMINATOR,                                    // 0E  ^N SO  Shift Out
            TERMINATOR,                                    // 0F  ^O SI  Shift In
            TERMINATOR,                                    // 10  ^P DLE Data Link Escape
            TERMINATOR,                                    // 11  ^Q DC1 Device Control 1/XON
            TERMINATOR,                                    // 12  ^R DC2 Device Control 2
            TERMINATOR,                                    // 13  ^S DC3 Device Control 3/XOFF
            TERMINATOR,                                    // 14  ^T DC4 Device Control 4
            TERMINATOR,                                    // 15  ^U NAK Negative Acknowledgement
            TERMINATOR,                                    // 16  ^V SYN Synchronous Idle
            TERMINATOR,                                    // 17  ^W ETB End of Transmission Block
            TERMINATOR,                                    // 18  ^X CAN Cancel
            TERMINATOR,                                    // 19  ^Y EM  End of Medium
            TERMINATOR,                                    // 1A  ^Z SUB Substitute
            TERMINATOR,                                    // 1B  ^[ ESC Escape
            TERMINATOR,                                    // 1C  ^\ FS  File Separator
            TERMINATOR,                                    // 1D  ^] GS  Group Separator
            TERMINATOR,                                    // 1E  ^^ RS  Record Separator
            TERMINATOR,                                    // 1F  ^_ US  Unit Separator
            TERMINATOR,                                    // 20 " " SP  Space
            MUSTBE_NAME,                                   // 21 "!"     Base-64 --string--
            MUSTBE_NAME,                                   // 22 """
            (CANTBE_INTEGER | MAYBE_RADIX | CANTBE_REAL),  // 23 "#"     radix number, suffix initiator
            MUSTBE_NAME,                                   // 24 "$"
            TERMINATOR,                                    // 25 "%"     comment initiator
            MUSTBE_NAME,                                   // 26 "&"
            MUSTBE_NAME,                                   // 27 "'"
            TERMINATOR,                                    // 28 "("     start --string--
            TERMINATOR,                                    // 29 ")"     end --string--
            MUSTBE_NAME,                                   // 2A "*"
            MAYBE_INTEGER,                                 // 2B "+"     positive number
            MUSTBE_NAME,                                   // 2C ","
            MAYBE_INTEGER,                                 // 2D "-"     negative number
            (CANTBE_INTEGER | CANTBE_RADIX | MAYBE_REAL),  // 2E "."
            TERMINATOR,                                    // 2F "/"     literal --name--
            MAYBE_INTEGER,                                 // 30 "0"
            MAYBE_INTEGER,                                 // 31 "1"
            MAYBE_INTEGER,                                 // 32 "2"
            MAYBE_INTEGER,                                 // 33 "3"
            MAYBE_INTEGER,                                 // 34 "4"
            MAYBE_INTEGER,                                 // 35 "5"
            MAYBE_INTEGER,                                 // 36 "6"
            MAYBE_INTEGER,                                 // 37 "7"
            MAYBE_INTEGER,                                 // 38 "8"
            MAYBE_INTEGER,                                 // 39 "9"
            MUSTBE_NAME,                                   // 3A ":"
            MUSTBE_NAME,                                   // 3B ";"
            TERMINATOR,                                    // 3C "<"     start hex --string--, --dict--
            MUSTBE_NAME,                                   // 3D "="     =string, =array, =dict, =proc,  suffix
            TERMINATOR,                                    // 3E ">"     end hex --string--, --dict--
            MUSTBE_NAME,                                   // 3F "?"
            MUSTBE_NAME,                                   // 40 "@"     name prefix for hidden system --operator--
            CANTBE_REAL,                                   // 41 "A"
            CANTBE_REAL,                                   // 42 "B"
            (CANTBE_INTEGER | CANTBE_REAL),                // 43 "C"
            (CANTBE_INTEGER | MAYBE_REAL),                 // 44 "D"
            (CANTBE_INTEGER | MAYBE_REAL),                 // 45 "E"
            (CANTBE_INTEGER | CANTBE_REAL),                // 46 "F"
            (CANTBE_INTEGER | CANTBE_REAL),                // 47 "G"
            (CANTBE_INTEGER | CANTBE_REAL),                // 48 "H"
            CANTBE_REAL,                                   // 49 "I"
            (CANTBE_INTEGER | CANTBE_REAL),                // 4A "J"
            (CANTBE_INTEGER | CANTBE_REAL),                // 4B "K"
            CANTBE_REAL,                                   // 4C "L"
            (CANTBE_INTEGER | CANTBE_REAL),                // 4D "M"
            (CANTBE_INTEGER | CANTBE_REAL),                // 4E "N"
            (CANTBE_INTEGER | CANTBE_REAL),                // 4F "O"
            (CANTBE_INTEGER | CANTBE_REAL),                // 50 "P"
            CANTBE_REAL,                                   // 51 "Q"     --int128-- suffix (quad)
            (CANTBE_INTEGER | MAYBE_REAL),                 // 52 "R"
            (CANTBE_INTEGER | CANTBE_REAL),                // 53 "S"
            (CANTBE_INTEGER | CANTBE_REAL),                // 54 "T"
            CANTBE_REAL,                                   // 55 "U"
            (CANTBE_INTEGER | CANTBE_REAL),                // 56 "V"
            (CANTBE_INTEGER | CANTBE_REAL),                // 57 "W"
            (CANTBE_INTEGER | CANTBE_REAL),                // 58 "X"
            (CANTBE_INTEGER | CANTBE_REAL),                // 59 "Y"
            (CANTBE_INTEGER | CANTBE_REAL),                // 5A "Z"
            TERMINATOR,                                    // 5B "["     start --array--
            TERMINATOR,                                    // 5C "\"     executable --name--
            TERMINATOR,                                    // 5D "]"     end --array--
            MUSTBE_NAME,                                   // 5E "^"
            MUSTBE_NAME,                                   // 5F "_"
            MUSTBE_NAME,                                   // 60 "`"
            CANTBE_REAL,                                   // 61 "a"     --address--, --array-- suffix
            CANTBE_REAL,                                   // 62 "b"     --byte-- suffix
            (CANTBE_INTEGER | CANTBE_REAL),                // 63 "c"
            (CANTBE_INTEGER | MAYBE_REAL),                 // 64 "d"     --double-- suffix
            (CANTBE_INTEGER | MAYBE_REAL),                 // 65 "e"     exponent, early binding suffix
            (CANTBE_INTEGER | CANTBE_REAL),                // 66 "f"
            (CANTBE_INTEGER | CANTBE_REAL),                // 67 "g"
            (CANTBE_INTEGER | CANTBE_REAL),                // 68 "h"
            CANTBE_REAL,                                   // 69 "i"     --integer-- suffix
            (CANTBE_INTEGER | CANTBE_REAL),                // 6A "j"
            (CANTBE_INTEGER | CANTBE_REAL),                // 6B "k"
            CANTBE_REAL,                                   // 6C "l"     --long-- suffix, literal suffix, late binding suffix
            (CANTBE_INTEGER | CANTBE_REAL),                // 6D "m"
            (CANTBE_INTEGER | CANTBE_REAL),                // 6E "n"
            (CANTBE_INTEGER | CANTBE_REAL),                // 6F "o"
            (CANTBE_INTEGER | CANTBE_REAL),                // 70 "p"     --packed-- suffix
            CANTBE_REAL,                                   // 71 "q"     --int128-- suffix (quad)
            (CANTBE_INTEGER | MAYBE_REAL),                 // 72 "r"     --real-- suffix, readonly suffix
            (CANTBE_INTEGER | CANTBE_REAL),                // 73 "s"
            (CANTBE_INTEGER | CANTBE_REAL),                // 74 "t"
            CANTBE_REAL,                                   // 75 "u"     unsigned suffix
            (CANTBE_INTEGER | CANTBE_REAL),                // 76 "v"
            (CANTBE_INTEGER | CANTBE_REAL),                // 77 "w"     writable suffix
            (CANTBE_INTEGER | CANTBE_REAL),                // 78 "x"     executable suffix
            (CANTBE_INTEGER | CANTBE_REAL),                // 79 "y"
            (CANTBE_INTEGER | CANTBE_REAL),                // 7A "z"
            TERMINATOR,                                    // 7B "{"     start --proc--
            MUSTBE_NAME,                                   // 7C "|"     local variable binding delimiter
            TERMINATOR,                                    // 7D "}"     end --proc--
            MUSTBE_NAME,                                   // 7E "~"
            TERMINATOR                                     // 7F  ^? DEL Delete
    };
    static constexpr const hint_t *sm_hint_table = &sm_hint_data[1];

    [[nodiscard]] static constexpr hint_t get_hint(int ch) { return ((ch < 128) ? sm_hint_table[ch] : TERMINATOR); }

    union {
        vm_t m_byte;            //   8-bit
        integer_t m_integer;    //  32-bit
        uinteger_t m_uinteger;  //  32-bit
        real_t m_real;          //  32-bit
        long_t m_long;          //  64-bit
        ulong_t m_ulong;        //  64-bit
        address_t m_address;    //  64-bit
        double_t m_double;      //  64-bit
        int128_t m_int128;      // 128-bit
        uint128_t m_uint128;    // 128-bit
    };
    Trix *m_trx;
    hint_t m_hints;
    Object::Type m_type;
};
