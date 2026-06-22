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

//===--- Binary Token Decoders ---===//
//
// Implements decoders for binary-encoded tokens (bytes 0x80-0xFF).  Binary
// tokens provide a compact bytecode representation for Trix programs.  They
// are produced by the object-to-binary-token operator (ops_memory.inl) and
// consumed by the scanner, which dispatches bytes 0x80-0xFF here via
// scan_binary_token.  Based on:
//
//   PostScript binary object sequences (Type 1/Type 2 binary encoding):
//   compact representation of PostScript programs using byte-encoded tokens
//   instead of text.  Reduces program size and parse overhead.
//
//   Java class file bytecode / WASM binary format: the general concept of
//   a compact binary encoding for a high-level language, where each opcode
//   byte is followed by a type-specific payload.
//
// --- Core concepts for maintainers ---
//
// ENCODING SCHEME
//   Bytes 0x00-0x7F are normal ASCII characters (handled by the text
//   scanner in scanner.inl).  Bytes 0x80-0xFF are binary token opcodes.
//   Each opcode identifies a token type; the payload follows immediately
//   in the byte stream.  Most fixed-width payloads are 0-8 bytes; Int128/
//   UInt128 use a 16-byte payload, and String/NumberArray tokens carry
//   variable-length payloads.
//
//   The BinaryToken enum (enums.inl) maps opcode bytes to token types,
//   including: Byte, Integer_8/16/32, UInteger_8/16/32, Long_8/16/32/64,
//   ULong_8/16/32/64, Real, Double, Fixed, False/True, Null, Mark,
//   String_8/16, SystemLitName/SystemExecName (8/16), SystemLitValue/
//   SystemExecValue (8/16), WellKnownLitName/WellKnownExecName, NumberArray,
//   Int128, UInt128, Address_nullptr, a family of baked-in numeric constants
//   (e.g. Integer_0/1/Min/Max, Real_pi/e/INF), and Reserved_F2..FF.
//   (Multi-byte integer/real/double/String_16/System* types have paired _b
//   (big) and _l (little) opcodes plus an unsuffixed native-order variant.
//   Fixed/NumberArray encode endianness in a payload bit; Int128/UInt128 are
//   native-only on a little-endian host.)
//
// DECODER FUNCTIONS
//   Each bt_* function decodes one token type:
//     1. Read the payload bytes from the stream (byte order per the token;
//        unsuffixed opcodes use host-native order).
//     2. Construct the corresponding Object.
//     3. Return (Lexeme, Object) pair for the scanner.
//
// ENDIANNESS
//   Multi-byte payloads carry an explicit endianness per token: most
//   integer/real/double/String_16/System* types provide paired _b (big-endian)
//   and _l (little-endian) opcodes plus an unsuffixed variant decoded in
//   host-native order.  Decoders read the bytes via getn<T> and byteswap via
//   byteswap_helper (ops_bitwise.inl: std::byteswap for integers, bit_cast for
//   floating-point) when the stored endianness differs from the host (see
//   get_endian in stream.inl).  Fixed/NumberArray carry endianness in a payload
//   bit (no _b/_l opcodes); Int128/UInt128 are native-only and assume a
//   little-endian host (get_u128_native static_assert).
//
// ERROR HANDLING
//   If the stream ends mid-token (incomplete payload), the decoder
//   returns (EndOfStream, error-string).  Reserved opcodes (0xF2-0xFF) and
//   unsupported representations raise Unsupported; a binary-token byte with
//   no matching decoder raises InternalError.
//

// bt_byte: decode a BinaryToken::Byte token.
// Reads 1 byte payload (unsigned 0-255) and returns it as a literal Byte object.
// make_byte is constexpr inline -- no VM allocation, no VMFull risk.
[[nodiscard]] ScanToken bt_byte(Trix *trx) {
    return literal_pair(Object::make_byte(static_cast<vm_t>(getu8(trx))));
}

// Decodes a 1-byte signed integer binary token; sign-extends to integer_t.
[[nodiscard]] ScanToken bt_integer_8(Trix *trx) {
    return literal_pair(Object::make_integer(static_cast<integer_t>(geti8(trx))));
}

// Decodes a 2-byte signed integer binary token (given endianness); sign-extends to integer_t.
[[nodiscard]] ScanToken bt_integer_16(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_integer(static_cast<integer_t>(geti16(trx, encoding))));
}

// Decodes a 4-byte signed integer binary token (given endianness).
[[nodiscard]] ScanToken bt_integer_32(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_integer(static_cast<integer_t>(geti32(trx, encoding))));
}

// Decodes a 1-byte unsigned integer binary token; zero-extends to uinteger_t.
[[nodiscard]] ScanToken bt_uinteger_8(Trix *trx) {
    return literal_pair(Object::make_uinteger(static_cast<uinteger_t>(getu8(trx))));
}

// Decodes a 2-byte unsigned integer binary token (given endianness); zero-extends to uinteger_t.
[[nodiscard]] ScanToken bt_uinteger_16(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_uinteger(static_cast<uinteger_t>(getu16(trx, encoding))));
}

// Decodes a 4-byte unsigned integer binary token (given endianness).
[[nodiscard]] ScanToken bt_uinteger_32(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_uinteger(static_cast<uinteger_t>(getu32(trx, encoding))));
}

// Decodes a 1-byte signed long binary token; sign-extends to long_t.
// Raises: EndOfStream; VMFull if the ExtValue allocation for the Long Object fails.
[[nodiscard]] ScanToken bt_long_8(Trix *trx) {
    return literal_pair(Object::make_long(trx, static_cast<long_t>(geti8(trx))));
}

// Decodes a 2-byte signed long binary token (given endianness); sign-extends to long_t.
// Raises: EndOfStream; VMFull if the ExtValue allocation for the Long Object fails.
[[nodiscard]] ScanToken bt_long_16(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_long(trx, static_cast<long_t>(geti16(trx, encoding))));
}

// Decodes a 4-byte signed long binary token (given endianness); sign-extends to long_t.
// Raises: EndOfStream; VMFull if the ExtValue allocation for the Long Object fails.
[[nodiscard]] ScanToken bt_long_32(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_long(trx, static_cast<long_t>(geti32(trx, encoding))));
}

// Decodes an 8-byte signed long binary token (given endianness).
// Raises: EndOfStream; VMFull if the ExtValue allocation for the Long Object fails.
[[nodiscard]] ScanToken bt_long_64(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_long(trx, static_cast<long_t>(geti64(trx, encoding))));
}

// Decodes a 1-byte unsigned long binary token; zero-extends to ulong_t.
// Raises: EndOfStream; VMFull if the ExtValue allocation for the ULong Object fails.
[[nodiscard]] ScanToken bt_ulong_8(Trix *trx) {
    return literal_pair(Object::make_ulong(trx, static_cast<ulong_t>(getu8(trx))));
}

// Decodes a 2-byte unsigned long binary token (given endianness); zero-extends to ulong_t.
// Raises: EndOfStream; VMFull if the ExtValue allocation for the ULong Object fails.
[[nodiscard]] ScanToken bt_ulong_16(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_ulong(trx, static_cast<ulong_t>(getu16(trx, encoding))));
}

// Decodes a 4-byte unsigned long binary token (given endianness); zero-extends to ulong_t.
// Raises: EndOfStream; VMFull if the ExtValue allocation for the ULong Object fails.
[[nodiscard]] ScanToken bt_ulong_32(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_ulong(trx, static_cast<ulong_t>(getu32(trx, encoding))));
}

// Decodes an 8-byte unsigned long binary token (given endianness).
// Raises: EndOfStream; VMFull if the ExtValue allocation for the ULong Object fails.
[[nodiscard]] ScanToken bt_ulong_64(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_ulong(trx, static_cast<ulong_t>(getu64(trx, encoding))));
}

// Decodes a 4-byte IEEE 754 single-precision real binary token (given endianness).
// make_real() is inline; no VM allocation, no VMFull risk.
// Raises: EndOfStream if the stream ends mid-payload.
[[nodiscard]] ScanToken bt_real(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_real(getreal(trx, encoding)));
}

// Decodes an 8-byte IEEE 754 double-precision double binary token (given endianness).
// Raises: EndOfStream if the stream ends mid-payload;
//         VMFull if the ExtValue allocation for the Double Object fails.
[[nodiscard]] ScanToken bt_double(Trix *trx, std::endian encoding) {
    return literal_pair(Object::make_double(trx, getdouble(trx, encoding)));
}

// Decodes a 16-byte native-endian 128-bit unsigned payload by reading two uint64
// halves (low half first, matching little-endian layout on our target toolchains).
// Used by bt_int128 and bt_uint128 to avoid depending on std::byteswap<__uint128>.
[[nodiscard]] uint128_t get_u128_native(Trix *trx) {
    static_assert(std::endian::native == std::endian::little,
                  "Int128 binary-token encoding assumes a little-endian host (GCC/Clang target baseline)");
    auto lo = getu64(trx, std::endian::native);
    auto hi = getu64(trx, std::endian::native);
    return ((uint128_t{hi} << 64) | uint128_t{lo});
}

// Decodes a BinaryToken::Int128 token (1-byte tag + 16-byte native-endian payload).
// Raises: EndOfStream if the stream ends mid-payload;
//         VMFull if the WideValue allocation for the Int128 Object fails.
[[nodiscard]] ScanToken bt_int128(Trix *trx) {
    return literal_pair(Object::make_int128(trx, static_cast<int128_t>(get_u128_native(trx))));
}

// Decodes a BinaryToken::UInt128 token (1-byte tag + 16-byte native-endian payload).
// Raises: EndOfStream if the stream ends mid-payload;
//         VMFull if the WideValue allocation for the UInt128 Object fails.
[[nodiscard]] ScanToken bt_uint128(Trix *trx) {
    return literal_pair(Object::make_uint128(trx, get_u128_native(trx)));
}

// Converts a signed 32-bit fixed-point integer i with 'scale' fractional bits
// to a real_t.  Equivalent to i * 2^(-scale), computed via double for precision.
// scale must be in [1, 31]; callers must handle scale == 0 as Integer before calling.
[[nodiscard]] static real_t fixed_scale(Trix *trx, int32_t i, uint8_t scale) {
    if ((scale == 0) || (scale >= 32)) {
        trx->error(Error::InternalError, "fixed_scale: scale {} out of range [1, 31]", scale);
    } else {
        // 0.5 / (1 << (scale-1)) == 2^(-scale), avoids UB when scale == 31 (1u << 31 overflows)
        return static_cast<real_t>(static_cast<double_t>(i) * (0.5 / static_cast<double_t>(1u << (scale - 1))));
    }
}

// Decodes one signed fixed-point value: rep 0..31 -> 32-bit, rep 32..47 -> 16-bit;
// scale = fractional bits, so scale == 0 yields an Integer and otherwise a Real via
// fixed_scale.  Shared by bt_fixed and the bt_numberarray element loop; the caller
// guarantees rep < 48.
[[nodiscard]] Object decode_fixed_object(Trix *trx, uint8_t rep, std::endian encoding) {
    auto value = int32_t{0};
    auto scale = rep;
    if (rep < 32) {
        // 32-bit fixed point number
        value = geti32(trx, encoding);
    } else {
        // 16-bit fixed point number
        scale = static_cast<uint8_t>(rep - 32);
        value = static_cast<int32_t>(geti16(trx, encoding));
    }
    if (scale == 0) {
        return Object::make_integer(static_cast<integer_t>(value));
    } else {
        return Object::make_real(fixed_scale(trx, value, scale));
    }
}

// Decodes a BinaryToken::Fixed numeric literal from the stream.
// The byte immediately following the token header encodes endianness and representation:
//   Bit 7: endianness -- 0 = big-endian, 1 = little-endian (rep -= 128).
//   Bits 0..6 (rep):
//     rep  0..31: 32-bit signed fixed-point; scale = rep fractional bits.
//                 rep == 0 -> integer; returned as Integer Object.
//     rep 32..47: 16-bit signed fixed-point; scale = (rep-32) fractional bits.
//                 rep == 32 -> integer; returned as Integer Object.
//     rep 48+:    unsupported; raises Unsupported error.
// Returns {LiteralValue, Integer} for scale==0, {LiteralValue, Real} otherwise.
// Raises: EndOfStream; Unsupported for rep >= 48.
[[nodiscard]] ScanToken bt_fixed(Trix *trx) {
    auto encoding = std::endian::big;
    auto rep = getu8(trx);
    if (rep >= 128) {
        encoding = std::endian::little;
        rep = static_cast<uint8_t>(rep - 128);
    }

    if (rep < 48) {
        return literal_pair(decode_fixed_object(trx, rep, encoding));
    } else {
        trx->error(Error::Unsupported, "BinaryToken::Fixed: unsupported fixed-point representation {:#x}", rep);
    }
}

// Decodes a BinaryToken::True or BinaryToken::False literal.
// value is supplied by the caller based on which token header byte was read.
[[nodiscard]] ScanToken bt_boolean(bool value) {
    return literal_pair(Object::make_boolean(value));
}

// Decodes a BinaryToken::Mark literal.  No payload bytes; takes no parameters.
[[nodiscard]] ScanToken bt_mark() {
    return literal_pair(Object::make_mark());
}

// Decodes a BinaryToken::Null literal.  No payload bytes; takes no parameters.
[[nodiscard]] ScanToken bt_null() {
    return literal_pair(Object::make_null());
}

// Allocates a binary token string of the given length from the stream into VM memory.
// Returns a read-write String Object of that length.
// Raises: EndOfStream (via getn) if the stream ends mid-payload; VMFull if the VM
// heap cannot accommodate length+1 bytes (payload + nul terminator).
[[nodiscard]] ScanToken bt_string(Trix *trx, length_t length) {
    auto [ptr, remaining] = trx->vm_start_alloc<vm_t>();
    if (remaining >= (static_cast<vm_size_t>(length) + 1)) {
        getn(trx, ptr, length);
        ptr[length] = '\0';
        auto [_, offset] = trx->vm_end_alloc<vm_t>(length + 1);
        return literal_pair(Object::make_string(offset, length));
    } else {
        trx->vm_end_alloc();
        trx->error(Error::VMFull, "BinaryToken string cannot allocate {} bytes", length);
    }
}

[[nodiscard]] ScanToken bt_string_8(Trix *trx) {
    return bt_string(trx, getu8(trx));
}

[[nodiscard]] ScanToken bt_string_16(Trix *trx, std::endian encoding) {
    return bt_string(trx, getu16(trx, encoding));
}

// Decodes a BinaryToken::SystemLitName: validates the pre-read index into the
// system name table and returns a literal Name Object.
// Raises: IndexCheck if index >= SYSTEMNAME_COUNT.
[[nodiscard]] ScanToken bt_literal_systemname(Trix *trx, uint16_t index) {
    if (index < SYSTEMNAME_COUNT) {
        auto name_obj = Name::make_system(trx, static_cast<SystemName>(index), Object::LiteralAttrib);
        return std::pair{Lexeme::LiteralValue, name_obj};
    } else {
        trx->error(Error::IndexCheck, "BinaryToken::SystemName invalid index {}", index);
    }
}

// Decodes a BinaryToken::SystemExecName: validates the pre-read index into the
// system name table and returns an executable Name Object.
// Raises: IndexCheck if index >= SYSTEMNAME_COUNT.
[[nodiscard]] ScanToken bt_executable_systemname(Trix *trx, uint16_t index) {
    if (index < SYSTEMNAME_COUNT) {
        auto name_obj = Name::make_system(trx, static_cast<SystemName>(index), Object::ExecutableAttrib);
        return std::pair{Lexeme::ExecutableValue, name_obj};
    } else {
        trx->error(Error::IndexCheck, "BinaryToken::SystemName invalid index {}", index);
    }
}

// Decodes a BinaryToken::WellKnownLitName: validates the pre-read index into the
// well-known name table and returns a literal Name Object.
// Raises: IndexCheck if index >= WELLKNOWN_COUNT.
[[nodiscard]] ScanToken bt_literal_wellknown(Trix *trx, uint8_t index) {
    if (index < WELLKNOWN_COUNT) {
        auto name_obj = trx->wellknown_name(static_cast<WellKnownName>(index));
        return std::pair{Lexeme::LiteralValue, name_obj};
    } else {
        trx->error(Error::IndexCheck, "BinaryToken::WellKnownLitName invalid index {}", index);
    }
}

// Decodes a BinaryToken::WellKnownExecName: validates the pre-read index into the
// well-known name table and returns an executable Name Object.
// Raises: IndexCheck if index >= WELLKNOWN_COUNT.
[[nodiscard]] ScanToken bt_executable_wellknown(Trix *trx, uint8_t index) {
    if (index < WELLKNOWN_COUNT) {
        auto name_obj = trx->wellknown_name(static_cast<WellKnownName>(index));
        name_obj.set_executable();
        return std::pair{Lexeme::ExecutableValue, name_obj};
    } else {
        trx->error(Error::IndexCheck, "BinaryToken::WellKnownExecName invalid index {}", index);
    }
}

// Looks up the value bound to name in the dictionary stack and returns it as a clone
// with the given lexeme token.  name must be a Name Object.
// token selects LiteralValue vs. ExecutableValue for the returned object.
// The returned object's attributes come from the dictionary (not from name's attrib).
// Raises: InternalError if name is not a Name Object; Undefined if name has no binding.
[[nodiscard]] ScanToken bt_name_to_value(Trix *trx, const Object *name_ptr, Lexeme token) {
    if (name_ptr->is_name()) {
        auto value_ptr = Name::name_search(trx, name_ptr);
        if (value_ptr != nullptr) {
            return std::pair{token, value_ptr->make_clone(trx)};
        } else {
            trx->error(Error::Undefined, "BinaryToken: name {} has no bound value", name_ptr->name_sv(trx));
        }
    } else {
        trx->error(Error::InternalError, "bt_name_to_value: name is not a Name object");
    }
}

// Decodes a BinaryToken::SystemLitValue: validates the pre-read index, looks up
// the value bound to the corresponding system name in the dictionary stack, and returns
// a literal clone of that value.
// Raises: IndexCheck if index >= SYSTEMNAME_COUNT; Undefined if unbound.
[[nodiscard]] ScanToken bt_literal_systemvalue(Trix *trx, uint16_t index) {
    if (index < SYSTEMNAME_COUNT) {
        auto name_obj = Name::make_system(trx, static_cast<SystemName>(index));
        return bt_name_to_value(trx, &name_obj, Lexeme::LiteralValue);
    } else {
        trx->error(Error::IndexCheck, "BinaryToken::SystemValue invalid index {}", index);
    }
}

// Decodes a BinaryToken::SystemExecValue: validates the pre-read index, looks up
// the value bound to the corresponding system name in the dictionary stack, and returns
// an executable clone of that value.
// Raises: IndexCheck if index >= SYSTEMNAME_COUNT; Undefined if unbound.
[[nodiscard]] ScanToken bt_executable_systemvalue(Trix *trx, uint16_t index) {
    if (index < SYSTEMNAME_COUNT) {
        auto name_obj = Name::make_system(trx, static_cast<SystemName>(index));
        return bt_name_to_value(trx, &name_obj, Lexeme::ExecutableValue);
    } else {
        trx->error(Error::IndexCheck, "BinaryToken::SystemValue invalid index {}", index);
    }
}

// bt_numberarray: decode a BinaryToken::NumberArray token.
// Wire format: 1-byte rep field (bit 7: 0=big-endian, 1=little-endian; bits 0-6:
//   element type), then a 2-byte element count (same endianness), then 'length'
//   encoded elements whose size and type depend on rep (see table below).
// Raises: Unsupported (rep > 56, checked before reading length to avoid consuming
//   stream bytes on error), LimitCheck (length * sizeof(Object) overflows),
//   VMFull (no space for the Object array; Long/ULong/Double elements may also
//   VMFull mid-loop if ExtValue space runs out after the initial Object-space check),
//   EndOfStream (stream ends before all element bytes are read).
// Returns a literal ReadWrite Array Object.
[[nodiscard]] ScanToken bt_numberarray(Trix *trx) {
    auto encoding = std::endian::big;
    auto rep = getu8(trx);
    if (rep >= 128) {
        encoding = std::endian::little;
        rep = static_cast<uint8_t>(rep - 128);
    }
    // Validate rep before reading length to avoid consuming stream bytes on error.
    //    0..31 32-bit fixed-point number (scale = rep fractional bits; rep=0 -> Integer)
    //   32..47 16-bit fixed-point number (scale = rep-32 fractional bits; rep=32 -> Integer)
    //       48 Byte (1 byte)
    //       49 Integer 16-bit (2 bytes, sign-extended)
    //       50 UInteger 16-bit (2 bytes, zero-extended)
    //       51 Integer 32-bit (4 bytes)
    //       52 UInteger 32-bit (4 bytes)
    //       53 Long 64-bit (8 bytes; ExtValue)
    //       54 ULong 64-bit (8 bytes; ExtValue)
    //       55 Real 32-bit (4 bytes)
    //       56 Double 64-bit (8 bytes; ExtValue)
    // 128..184 same as rep-128 but little-endian
    if (rep > 56) {
        trx->error(Error::Unsupported, "BinaryToken::NumberArray unsupported representation {:#x}", rep);
    } else {
        auto length = getu16(trx, encoding);

        if (trx->m_curr_alloc_global || (trx->vm_remaining<Object>() >= (static_cast<vm_size_t>(length) * vm_sizeof<Object>()))) {
            auto [elem_ptr, offset] = trx->vm_alloc_dispatch_n<Object>(length, Trix::ChunkKind::Array);
            auto base_ptr = elem_ptr;
            try {
                auto n = length;
                while (n-- != 0) {
                    if (rep < 48) {
                        *elem_ptr++ = decode_fixed_object(trx, rep, encoding);
                    } else {
                        auto sub_rep = (rep - 48);
                        switch (sub_rep) {
                        case 0:  // 48 byte
                            *elem_ptr++ = Object::make_byte(static_cast<vm_t>(getu8(trx)));
                            break;

                        case 1:  // 49 (16-bit) integer
                            *elem_ptr++ = Object::make_integer(static_cast<integer_t>(geti16(trx, encoding)));
                            break;

                        case 2:  // 50 (16-bit) uinteger
                            *elem_ptr++ = Object::make_uinteger(static_cast<uinteger_t>(getu16(trx, encoding)));
                            break;

                        case 3:  // 51 integer
                            *elem_ptr++ = Object::make_integer(static_cast<integer_t>(geti32(trx, encoding)));
                            break;

                        case 4:  // 52 uinteger
                            *elem_ptr++ = Object::make_uinteger(static_cast<uinteger_t>(getu32(trx, encoding)));
                            break;

                        case 5:  // 53 long
                            *elem_ptr++ = Object::make_long(trx, static_cast<long_t>(geti64(trx, encoding)));
                            break;

                        case 6:  // 54 ulong
                            *elem_ptr++ = Object::make_ulong(trx, static_cast<ulong_t>(getu64(trx, encoding)));
                            break;

                        case 7:  // 55 real
                            *elem_ptr++ = Object::make_real(getreal(trx, encoding));
                            break;

                        case 8:  // 56 double
                            *elem_ptr++ = Object::make_double(trx, getdouble(trx, encoding));
                            break;

                        default:
                            trx->error(Error::InternalError, "bt_numberarray: unhandled sub_rep {}", sub_rep);
                        }
                    }
                }
                return literal_pair(Object::make_array(offset, length));
            }
            catch (...) {
                // Free ExtValues for already-written elements on mid-loop failure
                for (auto curr_ptr = base_ptr; curr_ptr < elem_ptr; ++curr_ptr) {
                    curr_ptr->maybe_free_extvalue(trx);
                }
                throw;
            }
        } else {
            trx->error(Error::VMFull, "while scanning a BinaryToken::NumberArray");
        }
    }
}

// bt_constant_byte: return a literal Byte for a no-payload constant token.
// ch is the constant value baked into the token header (no stream reads).
// make_byte is constexpr inline -- no VM allocation, no errors possible.
[[nodiscard]] ScanToken bt_constant_byte(vm_t ch) {
    return literal_pair(Object::make_byte(ch));
}

// bt_constant_integer: return a literal Integer for a no-payload constant token.
// i is the constant value baked into the token header (no stream reads).
// make_integer is constexpr inline -- no VM allocation, no errors possible.
[[nodiscard]] ScanToken bt_constant_integer(integer_t i) {
    return literal_pair(Object::make_integer(i));
}

// bt_constant_uinteger: return a literal UInteger for a no-payload constant token.
// i is the constant value baked into the token header (no stream reads).
// make_uinteger is constexpr inline -- no VM allocation, no errors possible.
[[nodiscard]] ScanToken bt_constant_uinteger(uinteger_t i) {
    return literal_pair(Object::make_uinteger(i));
}

// bt_constant_long: return a literal Long for a no-payload constant token.
// l is the constant value baked into the token header (no stream reads).
// Raises VMFull if the ExtValue allocation for the Long Object fails.
[[nodiscard]] ScanToken bt_constant_long(Trix *trx, long_t l) {
    return literal_pair(Object::make_long(trx, l));
}

// bt_constant_ulong: return a literal ULong for a no-payload constant token.
// l is the constant value baked into the token header (no stream reads).
// Raises VMFull if the ExtValue allocation for the ULong Object fails.
[[nodiscard]] ScanToken bt_constant_ulong(Trix *trx, ulong_t l) {
    return literal_pair(Object::make_ulong(trx, l));
}

// bt_constant_real: return a literal Real for a no-payload constant token.
// r is the constant value baked into the token header (no stream reads).
// make_real is constexpr inline -- no VM allocation, no errors possible.
[[nodiscard]] ScanToken bt_constant_real(real_t r) {
    return literal_pair(Object::make_real(r));
}

// bt_constant_double: return a literal Double for a no-payload constant token.
// d is the constant value baked into the token header (no stream reads).
// Raises VMFull if the ExtValue allocation for the Double Object fails.
[[nodiscard]] ScanToken bt_constant_double(Trix *trx, double_t d) {
    return literal_pair(Object::make_double(trx, d));
}

// bt_constant_address_nullptr: return a literal null Address for a no-payload constant token.
// Address is a 64-bit ExtValue type -- raises VMFull if the ExtValue allocation fails.
[[nodiscard]] ScanToken bt_constant_address_nullptr(Trix *trx) {
    return literal_pair(Object::make_address(trx, nullptr));
}

// Decodes one binary token whose header byte ch (0x80-0xFF) has already been consumed.
// Dispatches to a bt_xxx sub-function based on the BinaryToken encoding; catches
// Exception::EndOfStream from sub-functions (which read additional payload bytes)
// and converts it to {Lexeme::EndOfStream, errstr} so the caller can handle truncation
// gracefully.  Exception::Error and Exception::Exit propagate to the interpreter.
[[nodiscard]] ScanToken scan_binary_token(Trix *trx, int ch) {
    if (!is_binary_token(ch)) {
        trx->error(Error::InternalError, "scan_binary_token: ch {:#x} is not a binary token", ch);
    } else {
        try {
            switch (static_cast<BinaryToken>(ch)) {
            case BinaryToken::Byte:
                return bt_byte(trx);

            case BinaryToken::Integer_8:
                return bt_integer_8(trx);

            case BinaryToken::Integer_16:
                return bt_integer_16(trx, std::endian::native);

            case BinaryToken::Integer_16b:
                return bt_integer_16(trx, std::endian::big);

            case BinaryToken::Integer_16l:
                return bt_integer_16(trx, std::endian::little);

            case BinaryToken::Integer_32:
                return bt_integer_32(trx, std::endian::native);

            case BinaryToken::Integer_32b:
                return bt_integer_32(trx, std::endian::big);

            case BinaryToken::Integer_32l:
                return bt_integer_32(trx, std::endian::little);

            case BinaryToken::UInteger_8:
                return bt_uinteger_8(trx);

            case BinaryToken::UInteger_16:
                return bt_uinteger_16(trx, std::endian::native);

            case BinaryToken::UInteger_16b:
                return bt_uinteger_16(trx, std::endian::big);

            case BinaryToken::UInteger_16l:
                return bt_uinteger_16(trx, std::endian::little);

            case BinaryToken::UInteger_32:
                return bt_uinteger_32(trx, std::endian::native);

            case BinaryToken::UInteger_32b:
                return bt_uinteger_32(trx, std::endian::big);

            case BinaryToken::UInteger_32l:
                return bt_uinteger_32(trx, std::endian::little);

            case BinaryToken::Long_8:
                return bt_long_8(trx);

            case BinaryToken::Long_16:
                return bt_long_16(trx, std::endian::native);

            case BinaryToken::Long_16b:
                return bt_long_16(trx, std::endian::big);

            case BinaryToken::Long_16l:
                return bt_long_16(trx, std::endian::little);

            case BinaryToken::Long_32:
                return bt_long_32(trx, std::endian::native);

            case BinaryToken::Long_32b:
                return bt_long_32(trx, std::endian::big);

            case BinaryToken::Long_32l:
                return bt_long_32(trx, std::endian::little);

            case BinaryToken::Long_64:
                return bt_long_64(trx, std::endian::native);

            case BinaryToken::Long_64b:
                return bt_long_64(trx, std::endian::big);

            case BinaryToken::Long_64l:
                return bt_long_64(trx, std::endian::little);

            case BinaryToken::ULong_8:
                return bt_ulong_8(trx);

            case BinaryToken::ULong_16:
                return bt_ulong_16(trx, std::endian::native);

            case BinaryToken::ULong_16b:
                return bt_ulong_16(trx, std::endian::big);

            case BinaryToken::ULong_16l:
                return bt_ulong_16(trx, std::endian::little);

            case BinaryToken::ULong_32:
                return bt_ulong_32(trx, std::endian::native);

            case BinaryToken::ULong_32b:
                return bt_ulong_32(trx, std::endian::big);

            case BinaryToken::ULong_32l:
                return bt_ulong_32(trx, std::endian::little);

            case BinaryToken::ULong_64:
                return bt_ulong_64(trx, std::endian::native);

            case BinaryToken::ULong_64b:
                return bt_ulong_64(trx, std::endian::big);

            case BinaryToken::ULong_64l:
                return bt_ulong_64(trx, std::endian::little);

            case BinaryToken::Real:
                return bt_real(trx, std::endian::native);

            case BinaryToken::Real_b:
                return bt_real(trx, std::endian::big);

            case BinaryToken::Real_l:
                return bt_real(trx, std::endian::little);

            case BinaryToken::Double:
                return bt_double(trx, std::endian::native);

            case BinaryToken::Double_b:
                return bt_double(trx, std::endian::big);

            case BinaryToken::Double_l:
                return bt_double(trx, std::endian::little);

            case BinaryToken::Fixed:
                return bt_fixed(trx);

            case BinaryToken::False:
                return bt_boolean(false);

            case BinaryToken::True:
                return bt_boolean(true);

            case BinaryToken::Mark:
                return bt_mark();

            case BinaryToken::Null:
                return bt_null();

            case BinaryToken::String_8:
                return bt_string_8(trx);

            case BinaryToken::String_16:
                return bt_string_16(trx, std::endian::native);

            case BinaryToken::String_16b:
                return bt_string_16(trx, std::endian::big);

            case BinaryToken::String_16l:
                return bt_string_16(trx, std::endian::little);

            case BinaryToken::SystemLitName_8:
                return bt_literal_systemname(trx, getu8(trx));

            case BinaryToken::SystemExecName_8:
                return bt_executable_systemname(trx, getu8(trx));

            case BinaryToken::SystemLitName_16:
                return bt_literal_systemname(trx, getu16(trx, std::endian::native));

            case BinaryToken::SystemLitName_16b:
                return bt_literal_systemname(trx, getu16(trx, std::endian::big));

            case BinaryToken::SystemLitName_16l:
                return bt_literal_systemname(trx, getu16(trx, std::endian::little));

            case BinaryToken::SystemExecName_16:
                return bt_executable_systemname(trx, getu16(trx, std::endian::native));

            case BinaryToken::SystemExecName_16b:
                return bt_executable_systemname(trx, getu16(trx, std::endian::big));

            case BinaryToken::SystemExecName_16l:
                return bt_executable_systemname(trx, getu16(trx, std::endian::little));

            case BinaryToken::SystemLitValue_8:
                return bt_literal_systemvalue(trx, getu8(trx));

            case BinaryToken::SystemExecValue_8:
                return bt_executable_systemvalue(trx, getu8(trx));

            case BinaryToken::SystemLitValue_16:
                return bt_literal_systemvalue(trx, getu16(trx, std::endian::native));

            case BinaryToken::SystemLitValue_16b:
                return bt_literal_systemvalue(trx, getu16(trx, std::endian::big));

            case BinaryToken::SystemLitValue_16l:
                return bt_literal_systemvalue(trx, getu16(trx, std::endian::little));

            case BinaryToken::SystemExecValue_16:
                return bt_executable_systemvalue(trx, getu16(trx, std::endian::native));

            case BinaryToken::SystemExecValue_16b:
                return bt_executable_systemvalue(trx, getu16(trx, std::endian::big));

            case BinaryToken::SystemExecValue_16l:
                return bt_executable_systemvalue(trx, getu16(trx, std::endian::little));

            case BinaryToken::NumberArray:
                return bt_numberarray(trx);

            case BinaryToken::Byte_0:
                return bt_constant_byte(0);

            case BinaryToken::Byte_1:
                return bt_constant_byte(1);

            case BinaryToken::Byte_127:
                return bt_constant_byte(127);

            case BinaryToken::Byte_128:
                return bt_constant_byte(128);

            case BinaryToken::Byte_255:
                return bt_constant_byte(255);

            case BinaryToken::Byte_2:
                return bt_constant_byte(2);

            case BinaryToken::Integer_Min:
                return bt_constant_integer(std::numeric_limits<integer_t>::min());

            case BinaryToken::Integer_Max:
                return bt_constant_integer(std::numeric_limits<integer_t>::max());

            case BinaryToken::Integer_Neg1:
                return bt_constant_integer(-1);

            case BinaryToken::Integer_0:
                return bt_constant_integer(0);

            case BinaryToken::Integer_1:
                return bt_constant_integer(1);

            case BinaryToken::Integer_2:
                return bt_constant_integer(2);

            case BinaryToken::Integer_Neg2:
                return bt_constant_integer(-2);

            case BinaryToken::Integer_10:
                return bt_constant_integer(10);

            case BinaryToken::Integer_100:
                return bt_constant_integer(100);

            case BinaryToken::UInteger_Min:
                return bt_constant_uinteger(std::numeric_limits<uinteger_t>::min());

            case BinaryToken::UInteger_Max:
                return bt_constant_uinteger(std::numeric_limits<uinteger_t>::max());

            case BinaryToken::UInteger_1:
                return bt_constant_uinteger(1);

            case BinaryToken::Long_Min:
                return bt_constant_long(trx, std::numeric_limits<long_t>::min());

            case BinaryToken::Long_Max:
                return bt_constant_long(trx, std::numeric_limits<long_t>::max());

            case BinaryToken::Long_Neg1:
                return bt_constant_long(trx, -1);

            case BinaryToken::Long_0:
                return bt_constant_long(trx, 0);

            case BinaryToken::Long_1:
                return bt_constant_long(trx, 1);

            case BinaryToken::ULong_Min:
                return bt_constant_ulong(trx, std::numeric_limits<ulong_t>::min());

            case BinaryToken::ULong_Max:
                return bt_constant_ulong(trx, std::numeric_limits<ulong_t>::max());

            case BinaryToken::ULong_1:
                return bt_constant_ulong(trx, 1);

            case BinaryToken::Real_Neg1:
                return bt_constant_real(-1.0f);

            case BinaryToken::Real_0:
                return bt_constant_real(0.0f);

            case BinaryToken::Real_1:
                return bt_constant_real(1.0f);

            case BinaryToken::Real_e:
                return bt_constant_real(std::numbers::e_v<real_t>);

            case BinaryToken::Real_pi:
                return bt_constant_real(std::numbers::pi_v<real_t>);

            case BinaryToken::Real_INF:
                return bt_constant_real(std::numeric_limits<real_t>::infinity());

            case BinaryToken::Real_qNAN:
                return bt_constant_real(std::numeric_limits<real_t>::quiet_NaN());

            case BinaryToken::Real_2:
                return bt_constant_real(2.0f);

            case BinaryToken::Double_Neg1:
                return bt_constant_double(trx, -1.0);

            case BinaryToken::Double_0:
                return bt_constant_double(trx, 0.0);

            case BinaryToken::Double_1:
                return bt_constant_double(trx, 1.0);

            case BinaryToken::Double_e:
                return bt_constant_double(trx, std::numbers::e_v<double_t>);

            case BinaryToken::Double_pi:
                return bt_constant_double(trx, std::numbers::pi_v<double_t>);

            case BinaryToken::Double_INF:
                return bt_constant_double(trx, std::numeric_limits<double_t>::infinity());

            case BinaryToken::Double_qNAN:
                return bt_constant_double(trx, std::numeric_limits<double_t>::quiet_NaN());

            case BinaryToken::Double_2:
                return bt_constant_double(trx, 2.0);

            case BinaryToken::Address_nullptr:
                return bt_constant_address_nullptr(trx);

            case BinaryToken::WellKnownLitName:
                return bt_literal_wellknown(trx, getu8(trx));

            case BinaryToken::WellKnownExecName:
                return bt_executable_wellknown(trx, getu8(trx));

            case BinaryToken::Int128:
                return bt_int128(trx);

            case BinaryToken::UInt128:
                return bt_uint128(trx);

            case BinaryToken::Reserved_F2:
            case BinaryToken::Reserved_F3:
            case BinaryToken::Reserved_F4:
            case BinaryToken::Reserved_F5:
            case BinaryToken::Reserved_F6:
            case BinaryToken::Reserved_F7:
            case BinaryToken::Reserved_F8:
            case BinaryToken::Reserved_F9:
            case BinaryToken::Reserved_FA:
            case BinaryToken::Reserved_FB:
            case BinaryToken::Reserved_FC:
            case BinaryToken::Reserved_FD:
            case BinaryToken::Reserved_FE:
            case BinaryToken::Reserved_FF:
                trx->error(Error::Unsupported, "reserved binary token 0x{:02X}", ch);

            default:
                trx->error(Error::InternalError, "scan_binary_token: unknown BinaryToken {:#x}", ch);
            }
        }
        // catch by value as e is a trivial type and this is slightly more efficient
        catch (Exception e) {
            switch (e) {
            case Exception::EndOfStream: {
                if (trx->m_vm_alloc_active != nullptr) {
                    // set by vm_start_alloc() and cleared by vm_end_alloc_ptr(), vm_end_alloc(), error(),
                    // init_and_interpret() stop any active string/array allocation
                    trx->vm_end_alloc();
                }
                return std::pair{Lexeme::EndOfStream,
                                 Object::make_error_string(trx, "EndOfStream encountered within a binary-token")};
            }

            case Exception::Error:
            case Exception::Exit:
            default:
                // re-throw to interpreter; covers Error, Exit, and any future Exception values
                throw;
            }
        }

        trx->error(Error::InternalError, "scan_binary_token: unreachable code path reached");
    }
}
