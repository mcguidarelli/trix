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

//===--- Memory, Binary Data, and Address Operators ---===//
//
// Implements low-level memory access, packed array compression, binary data
// encoding/decoding, and address arithmetic.  Based on:
//
//   PostScript packed arrays: compressed representation of procedure bodies
//   for space efficiency.  Trix extends this with 31 PackedType encodings
//   for operators, names, integers, and other common patterns.
//
//   C struct packing / Python struct module: binary encoding of typed values
//   into byte sequences for I/O and interprocess communication.
//
//   Forth PEEK/POKE: direct memory access primitives for reading and writing
//   raw bytes at addresses within the VM heap.
//
// --- Core concepts for maintainers ---
//
// PACKED ARRAYS
//   Packed arrays are a compressed representation of Array objects.  Each
//   element is encoded using a PackedType tag (1 byte) followed by a
//   variable-length payload (0-4 bytes).  Common patterns (small integers,
//   system operators, system names) encode in 1-2 bytes vs 8 bytes for a
//   regular Object.
//
//   The `packed` operator builds a Packed array from operands; `array-load`
//   pushes its elements back onto the stack.  Packed arrays are read-only and
//   sequential-access only (no O(1) indexing).  They are the default
//   representation for procedure bodies created by { } (via the scanner's
//   Object::make_packed_data).
//
// BINARY PACK/UNPACK
//   pack encodes Trix values into a byte string using a format descriptor
//   (similar to Python's struct.pack).
//   unpack decodes a byte string back into Trix values; pack-size returns the
//   byte count a format descriptor produces.
//   Supports: byte (b/B), 16/32/64/128-bit integers (h/H i/I l/L q/Q, big/little
//   endian via > < =), float (f), double (d), fixed-length string (Ns), and
//   padding (x).
//
// PEEK/POKE
//   peek reads raw bytes from a VM heap address.
//   poke writes raw bytes to a VM heap address.
//   Both validate that the address is within the active VM heap region
//   and that access permissions allow the operation.
//
// ADDRESS VALIDATION
//   address-state? (addr :- addr name) probes whether a VM heap address is
//   null/invalid/read-only/read-write and pushes the corresponding state name,
//   caching the result in the Address object's F/X bits.  Addresses are
//   vm_offset_t values (uint32_t byte offsets from m_vm_base).
//
// PACKED ARRAY ENCODING (PackedType enum + encoding logic in object.inl)
//
//   Each element is encoded as:
//     [header] [length?] [value?]
//
//   HEADER BYTE (always 1 byte):
//     7 6 5 4 3 2 1 0
//    +-+-+-+-+-+-+-+-+
//    |X|S S|T T T T T|
//    +-+-+-+-+-+-+-+-+
//     |  |  |_________|
//     |  |       |
//     |  |       +-- TTTTT: PackedType index (0-31)
//     |  +---------- SS: value_size - 1 (0..3 means 1..4 bytes)
//     |              ignored when sm_sizes[T] has VSizeZero set
//     +------------- X: 0 = Literal, 1 = Executable
//
//   LENGTH (0, 1, or 2 bytes, determined by sm_sizes[T].LL):
//     Present for container types that carry a count/id (Array, Packed,
//     String, Stream).  1 byte for Short variants, 2 bytes for Long.
//
//   VALUE (0 to 4 bytes, determined by SS bits or VSizeZero flag):
//     SS+1 bytes (1-4) for most types.  0 bytes when VSizeZero is set.
//     Variable-length: uses minimum bytes needed (strip leading 0x00 for
//     unsigned, strip redundant sign-extension bytes for signed).
//
//   ENCODING BY TYPE:
//
//   Type                       Header  Len   Value   Notes
//   -------------------------  ------  ----  ------  ----------------------------
//   CommonOp (0)               1       -     -       X|SS encode slot(0-7)
//                                                    exch dup pop index roll
//                                                    if if-else eq
//   Byte (1)                   1       -     1       unsigned byte value
//   Integer (2)                1       -     1-4     signed 32-bit, var-length
//   PackedExt (3)              1       0/1/2 1-4     CUSTOM: [subcode][len?]
//                                                    [value]; subcodes encode
//                                                    Int128/UInt128, Eq* refs,
//                                                    OpaqueHandle
//   UInteger (4)               1       -     1-4     unsigned 32-bit, var-length
//   Long (5)                   1       -     1-4     vm_offset_t to ExtValue
//   ULong (6)                  1       -     1-4     vm_offset_t to ExtValue
//   Address (7)                1       -     1-4     vm_offset_t to ExtValue
//   Real (8)                   1       -     1-4     32-bit float bit pattern
//   Double (9)                 1       -     1-4     vm_offset_t to ExtValue
//   Simple (10)                1       -     -       SS: 00=Null 01=Mark
//                                                          10=False 11=True
//   Curry (11)                 1       -     1-4     vm_offset_t to heap pair
//                                                    (Tagged shares: X=lit)
//   Operator (12)              1       -     1-2     signed 16-bit operator index
//                                                    (>=0: SystemName, <0: user op)
//   Reserved2 (13)             -       -     -       (unused)
//   Record (14)                1       fc    1-4     CUSTOM: X=fc width(1|2),
//                                                    SS=offset size, fc=field
//                                                    count, value=vm_offset_t
//   Name (15)                  1       -     2-4     vm_offset_t (min 2 bytes)
//   ShortLengthArray (16)      1       1     1-4     vm_offset_t to Object[]
//   LongLengthArray (17)       1       2     1-4     vm_offset_t to Object[]
//   ROShortLengthArray (18)    1       1     1-4     read-only
//   ROLongLengthArray (19)     1       2     1-4     read-only
//   ROShortLengthPacked (20)   1       1     1-4     vm_offset_t to packed data
//   ROLongLengthPacked (21)    1       2     1-4     vm_offset_t to packed data
//   ShortLengthString (22)     1       1     1-4     vm_offset_t to byte[]
//   LongLengthString (23)      1       2     1-4     vm_offset_t to byte[]
//   ROShortLengthString (24)   1       1     1-4     read-only
//   ROLongLengthString (25)    1       2     1-4     read-only
//   ShortLengthStream (26)     1       1     1-4     vm_offset_t to Stream
//   LongLengthStream (27)      1       2     1-4     vm_offset_t to Stream
//   ROShortLengthStream (28)   1       1     1-4     read-only
//   ROLongLengthStream (29)    1       2     1-4     read-only
//   Dict (30)                  1       -     1-4     vm_offset_t to Dict
//                                                    (Set shares this slot;
//                                                    decoder distinguishes via
//                                                    the Dict body's SetFlag,
//                                                    not the header)
//
//   TOTAL SIZE per element: header(1) + [PackedExt subcode(1)] + length(0|1|2)
//   + value(0-4)
//   Minimum: 1 byte (CommonOp, Simple)
//   Maximum: 8 bytes -- a PackedExt Long-variant eqref: header + subcode +
//   2-byte length + 4-byte generation (emit_packed_element, object.inl).
//   Non-PackedExt types top out at 7 (Long-length container: header + 2-byte
//   length + 4-byte offset).  The save level is not stored; on decode it is
//   reset to Save::BASE.
//   Typical: 1-3 bytes (most operators and small integers)
//
//   NOT PACKABLE: Thunk, Coroutine, PipeBuffer, Cell, Continuation, SourceLoc
//   (these types raise TypeCheck if pack is attempted)
//
// Low-level memory access (peek, poke), heap allocation (alloc, free),
// address validation (address-state?), binary pack/unpack (pack, unpack,
// pack-size) with format-string visitor, binary token encoding
// (to-binary-token operator, backed by the make_binary_token_string helper),
// and bit-position template (bit_position_op) backing the
// bit?/bit-set/bit-clear/bit-toggle operators.
//

// throws: opstack-underflow, type-check, unsupported
static void peek_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "peek: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyName, VerifyAddress);

        auto name_ptr = trx->m_op_ptr;
        auto addr_ptr = (name_ptr - 1);

        auto [is_name, type] = trx->is_type_name(name_ptr);
        if (!is_name) {
            trx->error(Error::TypeCheck, "peek: not a type name");
        } else if ((TypeToVerify(type) & VerifyNumbers) == 0) {
            trx->error(Error::TypeCheck, "peek: type name is not a numeric type");
        } else {
            auto address = addr_ptr->address_value(trx);

            // Determine probe size from target type.
            auto probe_size = size_t{0};
            switch (+type) {
            case +Object::Type::Byte:
                probe_size = sizeof(byte_t);
                break;

            case +Object::Type::Integer:
                probe_size = sizeof(integer_t);
                break;

            case +Object::Type::UInteger:
                probe_size = sizeof(uinteger_t);
                break;

            case +Object::Type::Long:
                probe_size = sizeof(long_t);
                break;

            case +Object::Type::ULong:
                probe_size = sizeof(ulong_t);
                break;

            case +Object::Type::Int128:
                probe_size = sizeof(int128_t);
                break;

            case +Object::Type::UInt128:
                probe_size = sizeof(uint128_t);
                break;

            case +Object::Type::Real:
                probe_size = sizeof(real_t);
                break;

            case +Object::Type::Double:
                probe_size = sizeof(double_t);
                break;

            default:
                std::unreachable();
            }

            // Use cached address state if available, otherwise probe and cache.
            if (!addr_ptr->has_address_cache()) {
                addr_ptr->set_address_cache(trx->address_state(address, probe_size));
            }
            if (addr_ptr->cached_address_state() <= AddressState::IsNullPtr) {
                trx->error(Error::RangeCheck, "peek: invalid address");
            } else {
                // Address is valid -- read the value.
                Object result;
                switch (+type) {
                case +Object::Type::Byte: {
                    byte_t value;
                    std::memcpy(&value, address, sizeof(value));
                    result = Object::make_byte(value);
                    break;
                }

                case +Object::Type::Integer: {
                    integer_t value;
                    std::memcpy(&value, address, sizeof(value));
                    result = Object::make_integer(value);
                    break;
                }

                case +Object::Type::UInteger: {
                    uinteger_t value;
                    std::memcpy(&value, address, sizeof(value));
                    result = Object::make_uinteger(value);
                    break;
                }

                case +Object::Type::Long: {
                    long_t value;
                    std::memcpy(&value, address, sizeof(value));
                    result = Object::make_long(trx, value);
                    break;
                }

                case +Object::Type::ULong: {
                    ulong_t value;
                    std::memcpy(&value, address, sizeof(value));
                    result = Object::make_ulong(trx, value);
                    break;
                }

                case +Object::Type::Real: {
                    real_t value;
                    std::memcpy(&value, address, sizeof(value));
                    result = Object::make_real(value);
                    break;
                }

                case +Object::Type::Int128: {
                    int128_t value;
                    std::memcpy(&value, address, sizeof(value));
                    result = Object::make_int128(trx, value);
                    break;
                }

                case +Object::Type::UInt128: {
                    uint128_t value;
                    std::memcpy(&value, address, sizeof(value));
                    result = Object::make_uint128(trx, value);
                    break;
                }

                case +Object::Type::Double: {
                    double_t value;
                    std::memcpy(&value, address, sizeof(value));
                    result = Object::make_double(trx, value);
                    break;
                }

                default:
                    std::unreachable();
                }

                addr_ptr->maybe_free_extvalue(trx);
                *addr_ptr = result;
                trx->m_op_ptr = addr_ptr;
            }
        }
    }
}

// poke: num addr :- --
// Writes a typed value to the given address.
// throws: opstack-underflow, range-check, read-only, type-check
static void poke_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "poke: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyAddress, VerifyNumbers);

        auto addr_ptr = trx->m_op_ptr;
        auto val_ptr = (addr_ptr - 1);

        auto address = addr_ptr->address_value(trx);

        // Check address writability using cache or probe.
        if (!addr_ptr->has_address_cache()) {
            auto probe_size = size_t{0};
            switch (+val_ptr->type()) {
            case +Object::Type::Byte:
                probe_size = sizeof(byte_t);
                break;

            case +Object::Type::Integer:
                probe_size = sizeof(integer_t);
                break;

            case +Object::Type::UInteger:
                probe_size = sizeof(uinteger_t);
                break;

            case +Object::Type::Long:
                probe_size = sizeof(long_t);
                break;

            case +Object::Type::ULong:
                probe_size = sizeof(ulong_t);
                break;

            case +Object::Type::Int128:
                probe_size = sizeof(int128_t);
                break;

            case +Object::Type::UInt128:
                probe_size = sizeof(uint128_t);
                break;

            case +Object::Type::Real:
                probe_size = sizeof(real_t);
                break;

            case +Object::Type::Double:
                probe_size = sizeof(double_t);
                break;

            default:
                std::unreachable();
            }
            addr_ptr->set_address_cache(trx->address_state(address, probe_size));
        }

        auto cached = addr_ptr->cached_address_state();
        if (cached <= AddressState::IsNullPtr) {
            trx->error(Error::RangeCheck, "poke: invalid address");
        } else if (cached == AddressState::IsReadOnly) {
            trx->error(Error::ReadOnly, "poke: address is read-only");
        } else {
            switch (+val_ptr->type()) {
            case +Object::Type::Byte: {
                auto value = val_ptr->byte_value();
                std::memcpy(address, &value, sizeof(value));
                break;
            }

            case +Object::Type::Integer: {
                auto value = val_ptr->integer_value();
                std::memcpy(address, &value, sizeof(value));
                break;
            }

            case +Object::Type::UInteger: {
                auto value = val_ptr->uinteger_value();
                std::memcpy(address, &value, sizeof(value));
                break;
            }

            case +Object::Type::Long: {
                auto value = val_ptr->long_value(trx);
                std::memcpy(address, &value, sizeof(value));
                break;
            }

            case +Object::Type::ULong: {
                auto value = val_ptr->ulong_value(trx);
                std::memcpy(address, &value, sizeof(value));
                break;
            }

            case +Object::Type::Int128: {
                auto value = val_ptr->int128_value(trx);
                std::memcpy(address, &value, sizeof(value));
                break;
            }

            case +Object::Type::UInt128: {
                auto value = val_ptr->uint128_value(trx);
                std::memcpy(address, &value, sizeof(value));
                break;
            }

            case +Object::Type::Real: {
                auto value = val_ptr->real_value();
                std::memcpy(address, &value, sizeof(value));
                break;
            }

            case +Object::Type::Double: {
                auto value = val_ptr->double_value(trx);
                std::memcpy(address, &value, sizeof(value));
                break;
            }

            default:
                std::unreachable();
            }

            addr_ptr->maybe_free_extvalue(trx);
            val_ptr->maybe_free_extvalue(trx);
            trx->m_op_ptr -= 2;
        }
    }
}

// ====================================================================
// Binary pack/unpack operators
// ====================================================================

// Shared format string parser for pack/unpack/pack-size.
// Calls visitor(specifier, repeat, endian, string_count) for each specifier.
// string_count is non-zero only for 's' specifier (the preceding repeat count).
// Returns total byte count.
template<typename Visitor>
static vm_size_t parse_pack_format(Trix *trx, std::string_view fmt, Visitor &&visit) {
    auto endian = std::endian::native;
    vm_size_t total_bytes = 0;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        auto ch = fmt[i];
        if (ch == '>') {
            endian = std::endian::big;
        } else if (ch == '<') {
            endian = std::endian::little;
        } else if (ch == '=') {
            endian = std::endian::native;
        } else {
            // Parse optional repeat/count prefix
            length_t repeat = 1;
            if ((ch >= '0') && (ch <= '9')) {
                vm_size_t acc = 0;
                while ((i < fmt.size()) && (fmt[i] >= '0') && (fmt[i] <= '9')) {
                    acc = (acc * 10) + static_cast<vm_size_t>(fmt[i] - '0');
                    if (acc > MaxStringLength) {
                        trx->error(Error::LimitCheck, "pack: repeat count exceeds maximum {}", MaxStringLength);
                    } else {
                        ++i;
                    }
                }
                if (i >= fmt.size()) {
                    trx->error(Error::InvalidFormatString, "pack: repeat count without specifier");
                } else {
                    ch = fmt[i];
                    repeat = static_cast<length_t>(acc);
                }
            }
            // Compute size per specifier
            length_t spec_size = 0;
            switch (ch) {
            case 'b':
            case 'B':
                spec_size = 1;
                break;

            case 'h':
            case 'H':
                spec_size = 2;
                break;

            case 'i':
            case 'I':
                spec_size = 4;
                break;

            case 'l':
            case 'L':
                spec_size = 8;
                break;

            case 'q':
            case 'Q':
                spec_size = 16;
                break;

            case 'f':
                spec_size = 4;
                break;

            case 'd':
                spec_size = 8;
                break;

            case 'x':
                spec_size = 1;
                break;

            case 's':
                spec_size = repeat;
                repeat = 1;  // 's' uses count as byte length, not repeat
                break;

            default:
                trx->error(Error::InvalidFormatString, "pack: unknown specifier '{:c}'", ch);
            }
            visit(ch, repeat, endian, spec_size);
            total_bytes += static_cast<vm_size_t>(spec_size) * repeat;
            if (total_bytes > MaxStringLength) {
                trx->error(Error::LimitCheck, "pack: format size {} exceeds maximum string length", total_bytes);
            }
        }
    }
    return total_bytes;
}

// Helper: conditionally byte-swap a value based on target endianness.
template<typename T>
static T pack_maybe_swap(T value, std::endian target) {
    if constexpr (sizeof(T) == 1) {
        return value;
    } else if (target != std::endian::native) {
        return byteswap_helper(value);
    } else {
        return value;
    }
}

// Extract a signed 64-bit integer from any integer-typed Object for pack.
static int64_t pack_extract_signed(Trix *trx, const Object *arg, char spec) {
    switch (arg->type()) {
    case Object::Type::Byte:
        return static_cast<int64_t>(arg->byte_value());

    case Object::Type::Integer:
        return static_cast<int64_t>(arg->integer_value());

    case Object::Type::UInteger:
        return static_cast<int64_t>(arg->uinteger_value());

    case Object::Type::Long:
        return static_cast<int64_t>(arg->long_value(trx));

    case Object::Type::ULong:
        return static_cast<int64_t>(arg->ulong_value(trx));

    case Object::Type::Null:
    case Object::Type::Address:
    case Object::Type::Real:
    case Object::Type::Double:
    case Object::Type::Boolean:
    case Object::Type::Operator:
    case Object::Type::Mark:
    case Object::Type::Name:
    case Object::Type::Array:
    case Object::Type::Packed:
    case Object::Type::String:
    case Object::Type::Stream:
    case Object::Type::Dict:
    case Object::Type::Set:
    case Object::Type::Curry:
    case Object::Type::Thunk:
    case Object::Type::Tagged:
    case Object::Type::Record:
    case Object::Type::Coroutine:
    case Object::Type::PipeBuffer:
    case Object::Type::Cell:
    case Object::Type::Continuation:
    case Object::Type::Int128:
    case Object::Type::UInt128:
    case Object::Type::SourceLoc:
    case Object::Type::OpaqueHandle:
        // 128-bit integers must use the 'q'/'Q' specifier.
        trx->error(Error::TypeCheck, "pack '{:c}': expected integer type, got {}", spec, Object::type_sv(arg->type()));
    }
    std::unreachable();
}

// Extract an unsigned 64-bit integer from any integer-typed Object for pack.
static uint64_t pack_extract_unsigned(Trix *trx, const Object *arg, char spec) {
    switch (arg->type()) {
    case Object::Type::Byte:
        return static_cast<uint64_t>(arg->byte_value());

    case Object::Type::Integer: {
        auto v = arg->integer_value();
        if (v < 0) {
            trx->error(Error::RangeCheck, "pack '{:c}': negative value {}", spec, v);
        } else {
            return static_cast<uint64_t>(v);
        }
    }

    case Object::Type::UInteger:
        return static_cast<uint64_t>(arg->uinteger_value());

    case Object::Type::Long: {
        auto v = arg->long_value(trx);
        if (v < 0) {
            trx->error(Error::RangeCheck, "pack '{:c}': negative value {}", spec, v);
        } else {
            return static_cast<uint64_t>(v);
        }
    }

    case Object::Type::ULong:
        return static_cast<uint64_t>(arg->ulong_value(trx));

    case Object::Type::Null:
    case Object::Type::Address:
    case Object::Type::Real:
    case Object::Type::Double:
    case Object::Type::Boolean:
    case Object::Type::Operator:
    case Object::Type::Mark:
    case Object::Type::Name:
    case Object::Type::Array:
    case Object::Type::Packed:
    case Object::Type::String:
    case Object::Type::Stream:
    case Object::Type::Dict:
    case Object::Type::Set:
    case Object::Type::Curry:
    case Object::Type::Thunk:
    case Object::Type::Tagged:
    case Object::Type::Record:
    case Object::Type::Coroutine:
    case Object::Type::PipeBuffer:
    case Object::Type::Cell:
    case Object::Type::Continuation:
    case Object::Type::Int128:
    case Object::Type::UInt128:
    case Object::Type::SourceLoc:
    case Object::Type::OpaqueHandle:
        // 128-bit integers must use the 'q'/'Q' specifier.
        trx->error(Error::TypeCheck, "pack '{:c}': expected integer type, got {}", spec, Object::type_sv(arg->type()));
    }
    std::unreachable();
}

// Extract a signed 128-bit integer from any integer-typed Object for pack.
// Widens narrower integer types into Int128.
static int128_t pack_extract_int128(Trix *trx, const Object *arg, char spec) {
    switch (+arg->type()) {
    case +Object::Type::Byte:
        return static_cast<int128_t>(arg->byte_value());

    case +Object::Type::Integer:
        return static_cast<int128_t>(arg->integer_value());

    case +Object::Type::UInteger:
        return static_cast<int128_t>(arg->uinteger_value());

    case +Object::Type::Long:
        return static_cast<int128_t>(arg->long_value(trx));

    case +Object::Type::ULong:
        return static_cast<int128_t>(arg->ulong_value(trx));

    case +Object::Type::Int128:
        return arg->int128_value(trx);

    case +Object::Type::UInt128: {
        auto u = arg->uint128_value(trx);
        if (u > static_cast<uint128_t>(std::numeric_limits<int128_t>::max())) {
            trx->error(Error::RangeCheck, "pack '{:c}': uint128 value out of int128 range", spec);
        } else {
            return static_cast<int128_t>(u);
        }
    }

    default:
        break;
    }
    trx->error(Error::TypeCheck, "pack '{:c}': expected integer type, got {}", spec, Object::type_sv(arg->type()));
}

// Extract an unsigned 128-bit integer from any integer-typed Object for pack.
// Widens narrower integer types into UInt128; rejects negative signed values.
static uint128_t pack_extract_uint128(Trix *trx, const Object *arg, char spec) {
    switch (+arg->type()) {
    case +Object::Type::Byte:
        return static_cast<uint128_t>(arg->byte_value());

    case +Object::Type::Integer: {
        auto v = arg->integer_value();
        if (v < 0) {
            trx->error(Error::RangeCheck, "pack '{:c}': negative value {}", spec, v);
        } else {
            return static_cast<uint128_t>(v);
        }
    }

    case +Object::Type::UInteger:
        return static_cast<uint128_t>(arg->uinteger_value());

    case +Object::Type::Long: {
        auto v = arg->long_value(trx);
        if (v < 0) {
            trx->error(Error::RangeCheck, "pack '{:c}': negative value {}", spec, v);
        } else {
            return static_cast<uint128_t>(v);
        }
    }

    case +Object::Type::ULong:
        return static_cast<uint128_t>(arg->ulong_value(trx));

    case +Object::Type::Int128: {
        auto v = arg->int128_value(trx);
        if (v < 0) {
            trx->error(Error::RangeCheck, "pack '{:c}': negative int128 value", spec);
        } else {
            return static_cast<uint128_t>(v);
        }
    }

    case +Object::Type::UInt128:
        return arg->uint128_value(trx);

    default:
        break;
    }
    trx->error(Error::TypeCheck, "pack '{:c}': expected integer type, got {}", spec, Object::type_sv(arg->type()));
}

// Extract a double from any numeric Object for pack.
static double pack_extract_double(Trix *trx, const Object *arg, char spec) {
    switch (arg->type()) {
    case Object::Type::Byte:
        return static_cast<double>(arg->byte_value());

    case Object::Type::Integer:
        return static_cast<double>(arg->integer_value());

    case Object::Type::UInteger:
        return static_cast<double>(arg->uinteger_value());

    case Object::Type::Long:
        return static_cast<double>(arg->long_value(trx));

    case Object::Type::ULong:
        return static_cast<double>(arg->ulong_value(trx));

    case Object::Type::Real:
        return static_cast<double>(arg->real_value());

    case Object::Type::Double:
        return arg->double_value(trx);

    case Object::Type::Null:
    case Object::Type::Address:
    case Object::Type::Boolean:
    case Object::Type::Operator:
    case Object::Type::Mark:
    case Object::Type::Name:
    case Object::Type::Array:
    case Object::Type::Packed:
    case Object::Type::String:
    case Object::Type::Stream:
    case Object::Type::Dict:
    case Object::Type::Set:
    case Object::Type::Curry:
    case Object::Type::Thunk:
    case Object::Type::Tagged:
    case Object::Type::Record:
    case Object::Type::Coroutine:
    case Object::Type::PipeBuffer:
    case Object::Type::Cell:
    case Object::Type::Continuation:
    case Object::Type::Int128:
    case Object::Type::UInt128:
    case Object::Type::SourceLoc:
    case Object::Type::OpaqueHandle:
        trx->error(Error::TypeCheck, "pack '{:c}': expected numeric type, got {}", spec, Object::type_sv(arg->type()));
    }
    std::unreachable();
}

// pack: mark v1 v2 ... vN fmt-str :- str
// Packs values into a binary string according to format specifiers.
// throws: invalid-format-string, opstack-underflow, range-check, type-check, unmatched-mark, vm-full
static void pack_op(Trix *trx) {
    // Stack: ... fmt-str mark arg1 arg2 ... argN
    // find_opstack_mark scans downward from m_op_ptr looking for the mark
    // But our layout is: mark arg1 ... argN fmt-str (fmt is on top)
    // So: fmt_ptr = m_op_ptr, mark is below args, args_count includes fmt
    trx->verify_operands(VerifyString);  // format string on top
    auto fmt_ptr = trx->m_op_ptr;
    auto fmt_sv = fmt_ptr->sv_value(trx);
    --trx->m_op_ptr;  // temporarily pop fmt to find mark
    auto [mark_ptr, args_count] = trx->find_opstack_mark();
    auto args_ptr = (mark_ptr + 1);

    // Pass 1: compute total size and count expected args
    length_t expected_args = 0;
    auto total_bytes = parse_pack_format(trx, fmt_sv, [&](char ch, length_t repeat, std::endian, length_t) {
        if ((ch != 'x') && (ch != 's')) {
            expected_args = static_cast<length_t>(expected_args + repeat);
        } else if (ch == 's') {
            expected_args = static_cast<length_t>(expected_args + 1);
        }
    });
    if (expected_args != args_count) {
        trx->error(Error::RangeCheck, "pack: format requires {} values, got {}", expected_args, args_count);
    } else {
        // Allocate result string
        auto [dst_ptr, dst_offset] = trx->vm_alloc<vm_t>(total_bytes + 1);
        auto out = reinterpret_cast<char *>(dst_ptr);

        // Pass 2: write values
        length_t arg_index = 0;
        parse_pack_format(trx, fmt_sv, [&](char ch, length_t repeat, std::endian endian, length_t spec_size) {
            for (length_t r = 0; r < repeat; ++r) {
                switch (ch) {
                case 'x': {
                    *out++ = '\0';
                    break;
                }

                case 'b': {
                    auto val = pack_extract_signed(trx, &args_ptr[arg_index++], 'b');
                    if ((val < -128) || (val > 127)) {
                        trx->error(Error::RangeCheck, "pack 'b': value {} out of int8 range", val);
                    } else {
                        auto v = static_cast<int8_t>(val);
                        std::memcpy(out, &v, 1);
                        out += 1;
                        break;
                    }
                }

                case 'B': {
                    auto val = pack_extract_unsigned(trx, &args_ptr[arg_index++], 'B');
                    if (val > 255) {
                        trx->error(Error::RangeCheck, "pack 'B': value {} out of uint8 range", val);
                    } else {
                        auto v = static_cast<uint8_t>(val);
                        std::memcpy(out, &v, 1);
                        out += 1;
                        break;
                    }
                }

                case 'h': {
                    auto val = pack_extract_signed(trx, &args_ptr[arg_index++], 'h');
                    if ((val < -32768) || (val > 32767)) {
                        trx->error(Error::RangeCheck, "pack 'h': value {} out of int16 range", val);
                    } else {
                        auto v = pack_maybe_swap(static_cast<int16_t>(val), endian);
                        std::memcpy(out, &v, 2);
                        out += 2;
                        break;
                    }
                }

                case 'H': {
                    auto val = pack_extract_unsigned(trx, &args_ptr[arg_index++], 'H');
                    if (val > 65535) {
                        trx->error(Error::RangeCheck, "pack 'H': value {} out of uint16 range", val);
                    } else {
                        auto v = pack_maybe_swap(static_cast<uint16_t>(val), endian);
                        std::memcpy(out, &v, 2);
                        out += 2;
                        break;
                    }
                }

                case 'i': {
                    auto val = pack_extract_signed(trx, &args_ptr[arg_index++], 'i');
                    auto v = pack_maybe_swap(static_cast<int32_t>(val), endian);
                    std::memcpy(out, &v, 4);
                    out += 4;
                    break;
                }

                case 'I': {
                    auto val = pack_extract_unsigned(trx, &args_ptr[arg_index++], 'I');
                    if (val > 0xFFFFFFFFu) {
                        trx->error(Error::RangeCheck, "pack 'I': value {} out of uint32 range", val);
                    } else {
                        auto v = pack_maybe_swap(static_cast<uint32_t>(val), endian);
                        std::memcpy(out, &v, 4);
                        out += 4;
                        break;
                    }
                }

                case 'l': {
                    auto val = pack_extract_signed(trx, &args_ptr[arg_index++], 'l');
                    auto v = pack_maybe_swap(static_cast<int64_t>(val), endian);
                    std::memcpy(out, &v, 8);
                    out += 8;
                    break;
                }

                case 'L': {
                    auto val = pack_extract_unsigned(trx, &args_ptr[arg_index++], 'L');
                    auto v = pack_maybe_swap(static_cast<uint64_t>(val), endian);
                    std::memcpy(out, &v, 8);
                    out += 8;
                    break;
                }

                case 'q': {
                    auto val = pack_extract_int128(trx, &args_ptr[arg_index++], 'q');
                    auto v = pack_maybe_swap(static_cast<uint128_t>(val), endian);
                    std::memcpy(out, &v, 16);
                    out += 16;
                    break;
                }

                case 'Q': {
                    auto val = pack_extract_uint128(trx, &args_ptr[arg_index++], 'Q');
                    auto v = pack_maybe_swap(val, endian);
                    std::memcpy(out, &v, 16);
                    out += 16;
                    break;
                }

                case 'f': {
                    auto val = static_cast<float>(pack_extract_double(trx, &args_ptr[arg_index++], 'f'));
                    auto v = pack_maybe_swap(val, endian);
                    std::memcpy(out, &v, 4);
                    out += 4;
                    break;
                }

                case 'd': {
                    auto val = pack_extract_double(trx, &args_ptr[arg_index++], 'd');
                    auto v = pack_maybe_swap(val, endian);
                    std::memcpy(out, &v, 8);
                    out += 8;
                    break;
                }

                case 's': {
                    auto arg = &args_ptr[arg_index++];
                    if (!arg->is_string()) {
                        trx->error(Error::TypeCheck, "pack 's': expected string");
                    } else {
                        auto sv = arg->sv_value(trx);
                        auto copy_len = std::min(static_cast<length_t>(sv.size()), spec_size);
                        std::memcpy(out, sv.data(), copy_len);
                        // Zero-pad if string is shorter than spec_size
                        if (copy_len < spec_size) {
                            std::memset(out + copy_len, 0, spec_size - copy_len);
                        }
                        out += spec_size;
                        break;
                    }
                }

                default:
                    std::unreachable();
                }
            }
        });

        // NUL-terminate the string
        dst_ptr[total_bytes] = '\0';

        // Clean up operand stack and push result.  m_op_ptr still points at the
        // topmost argument (the fmt string was popped above), so freeing
        // [mark_ptr, m_op_ptr] inclusive covers the mark plus every argument --
        // including the last one, whose ExtValue would otherwise leak.
        maybe_free_extvalue_opstack(trx, mark_ptr);
        auto result = Object::make_string(dst_offset, static_cast<length_t>(total_bytes));
        *mark_ptr = result;
        trx->m_op_ptr = mark_ptr;
    }
}

// unpack: str fmt-str :- v1 v2 ... vN int
// Unpacks a binary string into values according to format specifiers.
// Pushes all unpacked values followed by an integer count.
// throws: invalid-format-string, opstack-underflow, range-check, type-check
static void unpack_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto fmt_ptr = trx->m_op_ptr;
    auto str_ptr = (fmt_ptr - 1);
    auto fmt_sv = fmt_ptr->sv_value(trx);
    auto str_sv = str_ptr->sv_value(trx);

    // Pass 1: compute total bytes and count result values
    length_t result_count = 0;
    auto total_bytes = parse_pack_format(trx, fmt_sv, [&](char ch, length_t repeat, std::endian, length_t) {
        if ((ch != 'x') && (ch != 's')) {
            result_count = static_cast<length_t>(result_count + repeat);
        } else if (ch == 's') {
            result_count = static_cast<length_t>(result_count + 1);
        }
    });
    if (static_cast<length_t>(str_sv.size()) < total_bytes) {
        trx->error(Error::RangeCheck, "unpack: string length {} too short for format (needs {})", str_sv.size(), total_bytes);
    } else {
        // Ensure stack capacity: result_count values + 1 count integer, minus the 2 inputs we consume
        if (result_count > 0) {
            trx->require_op_capacity(result_count);
        }

        // Pass 2: read values from string
        auto in = str_sv.data();
        auto result_ptr = str_ptr;  // Overwrite inputs with results
        parse_pack_format(trx, fmt_sv, [&](char ch, length_t repeat, std::endian endian, length_t spec_size) {
            for (length_t r = 0; r < repeat; ++r) {
                switch (ch) {
                case 'x': {
                    in += 1;
                    break;
                }

                case 'b': {
                    int8_t v;
                    std::memcpy(&v, in, 1);
                    *result_ptr++ = Object::make_integer(static_cast<integer_t>(v));
                    in += 1;
                    break;
                }

                case 'B': {
                    uint8_t v;
                    std::memcpy(&v, in, 1);
                    *result_ptr++ = Object::make_byte(static_cast<vm_t>(v));
                    in += 1;
                    break;
                }

                case 'h': {
                    int16_t v;
                    std::memcpy(&v, in, 2);
                    v = pack_maybe_swap(v, endian);
                    *result_ptr++ = Object::make_integer(static_cast<integer_t>(v));
                    in += 2;
                    break;
                }

                case 'H': {
                    uint16_t v;
                    std::memcpy(&v, in, 2);
                    v = pack_maybe_swap(v, endian);
                    *result_ptr++ = Object::make_uinteger(static_cast<uinteger_t>(v));
                    in += 2;
                    break;
                }

                case 'i': {
                    int32_t v;
                    std::memcpy(&v, in, 4);
                    v = pack_maybe_swap(v, endian);
                    *result_ptr++ = Object::make_integer(static_cast<integer_t>(v));
                    in += 4;
                    break;
                }

                case 'I': {
                    uint32_t v;
                    std::memcpy(&v, in, 4);
                    v = pack_maybe_swap(v, endian);
                    *result_ptr++ = Object::make_uinteger(static_cast<uinteger_t>(v));
                    in += 4;
                    break;
                }

                case 'l': {
                    int64_t v;
                    std::memcpy(&v, in, 8);
                    v = pack_maybe_swap(v, endian);
                    *result_ptr++ = Object::make_long(trx, static_cast<long_t>(v));
                    in += 8;
                    break;
                }

                case 'L': {
                    uint64_t v;
                    std::memcpy(&v, in, 8);
                    v = pack_maybe_swap(v, endian);
                    *result_ptr++ = Object::make_ulong(trx, static_cast<ulong_t>(v));
                    in += 8;
                    break;
                }

                case 'q': {
                    uint128_t v;
                    std::memcpy(&v, in, 16);
                    v = pack_maybe_swap(v, endian);
                    *result_ptr++ = Object::make_int128(trx, static_cast<int128_t>(v));
                    in += 16;
                    break;
                }

                case 'Q': {
                    uint128_t v;
                    std::memcpy(&v, in, 16);
                    v = pack_maybe_swap(v, endian);
                    *result_ptr++ = Object::make_uint128(trx, v);
                    in += 16;
                    break;
                }

                case 'f': {
                    real_t v;
                    std::memcpy(&v, in, 4);
                    v = pack_maybe_swap(v, endian);
                    *result_ptr++ = Object::make_real(v);
                    in += 4;
                    break;
                }

                case 'd': {
                    double_t v;
                    std::memcpy(&v, in, 8);
                    v = pack_maybe_swap(v, endian);
                    *result_ptr++ = Object::make_double(trx, v);
                    in += 8;
                    break;
                }

                case 's': {
                    // Allocate a new string in VM for the unpacked bytes
                    auto [s_ptr, s_offset] = trx->vm_alloc<vm_t>(static_cast<vm_size_t>(spec_size) + 1);
                    std::memcpy(s_ptr, in, spec_size);
                    s_ptr[spec_size] = '\0';
                    *result_ptr++ = Object::make_string(s_offset, spec_size);
                    in += spec_size;
                    break;
                }

                default:
                    std::unreachable();
                }
            }
        });

        // Push count
        *result_ptr++ = Object::make_integer(static_cast<integer_t>(result_count));
        trx->m_op_ptr = (result_ptr - 1);
    }
}

// pack-size: fmt-str :- int
// Returns the number of bytes a format string would produce.
// throws: invalid-format-string, opstack-underflow, type-check
static void packsize_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto fmt_ptr = trx->m_op_ptr;
    auto fmt_sv = fmt_ptr->sv_value(trx);
    auto total = parse_pack_format(trx, fmt_sv, [](char, length_t, std::endian, length_t) {});
    *fmt_ptr = Object::make_integer(static_cast<integer_t>(total));
}

// crc32: str :- uint32
// Reflected-polynomial CRC-32 (Ethernet/zlib/PNG/gzip/POSIX).  Matches
// `cksum -a crc` output and Python `binascii.crc32`.  Pure function on
// string bytes; empty string -> 0.  Test vectors:
//   ()          -> 0
//   (a)         -> 0xE8B7BE43
//   (123456789) -> 0xCBF43926 (canonical zlib)
// Reuses the snapshot-side crc32_table + crc32_update helpers (defined
// in src/snapshot.inl) so there is exactly one CRC implementation.
// throws: opstack-underflow, type-check
static void crc32_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto str_ptr = trx->m_op_ptr;
    auto sv = str_ptr->sv_value(trx);
    auto crc = crc32_update(0, sv.data(), sv.size());
    *str_ptr = Object::make_uinteger(static_cast<uinteger_t>(crc));
}

// fletcher32: str :- uint32
// Fletcher-32 over 16-bit words, little-endian byte pairing per
// Wikipedia's worked example.  Odd trailing byte is zero-padded as
// the high byte of the final word.  Modulus 65535.  Test vectors:
//   ()         -> 0
//   (abcde)    -> 0xF04FC729
//   (abcdef)   -> 0x56502D2A
//   (abcdefgh) -> 0xEBE19591
// throws: opstack-underflow, type-check
static void fletcher32_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto str_ptr = trx->m_op_ptr;
    auto sv = str_ptr->sv_value(trx);
    uint32_t sum1 = 0;
    uint32_t sum2 = 0;
    auto n = sv.size();
    for (size_t i = 0; i < n; i += 2) {
        auto lo = static_cast<uint8_t>(sv[i]);
        auto hi = ((i + 1) < n) ? static_cast<uint8_t>(sv[i + 1]) : uint8_t{0};
        auto word = static_cast<uint32_t>((hi << 8) | lo);
        sum1 = (sum1 + word) % 65535u;
        sum2 = (sum2 + sum1) % 65535u;
    }
    auto result = (sum2 << 16) | sum1;
    *str_ptr = Object::make_uinteger(static_cast<uinteger_t>(result));
}

// adler32: str :- uint32
// RFC 1950 Adler-32 (zlib trailer).  s1 starts at 1, s2 at 0; for each
// byte b: s1 = (s1 + b) mod 65521; s2 = (s2 + s1) mod 65521.  Result is
// (s2 << 16) | s1.  Defers the mod for blocks of NMAX=5552 bytes -- the
// largest run for which no per-step mod is needed without uint32
// overflow.  Test vectors:
//   ()          -> 0x00000001
//   (a)         -> 0x00620062
//   (abc)       -> 0x024D0127
//   (Wikipedia) -> 0x11E60398
// throws: opstack-underflow, type-check
static void adler32_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto str_ptr = trx->m_op_ptr;
    auto sv = str_ptr->sv_value(trx);
    constexpr uint32_t Mod = 65521;
    constexpr size_t NMax = 5552;
    uint32_t s1 = 1;
    uint32_t s2 = 0;
    auto p = sv.data();
    auto n = sv.size();
    while (n > 0) {
        auto k = std::min(n, NMax);
        for (size_t i = 0; i < k; ++i) {
            s1 += static_cast<uint8_t>(p[i]);
            s2 += s1;
        }
        s1 %= Mod;
        s2 %= Mod;
        p += k;
        n -= k;
    }
    auto result = (s2 << 16) | s1;
    *str_ptr = Object::make_uinteger(static_cast<uinteger_t>(result));
}

#ifndef TRIX_NO_ZLIB

// Point a z_stream's output at the entire remaining VM scratch region (one
// byte reserved for the nul terminator, capped at MaxStringLength) and
// return its base.  Shared by deflate_impl / inflate_op; pair with
// zlib_vm_output_finish once (de|in)flate reaches Z_STREAM_END.
[[nodiscard]] static vm_t *zlib_vm_output_start(Trix *trx, z_stream *stream) {
    auto [dst_base, dst_remaining] = trx->vm_start_alloc<vm_t>();
    auto dst_limit = std::min(dst_remaining - 1, static_cast<vm_size_t>(MaxStringLength));
    stream->next_out = reinterpret_cast<Bytef *>(dst_base);
    stream->avail_out = static_cast<uInt>(std::min<vm_size_t>(dst_limit, std::numeric_limits<uInt>::max()));
    return dst_base;
}

// Commit a VM scratch buffer holding `length` produced bytes into a String at
// result_ptr: nul-terminate, finalise the allocation with vm_end_alloc, and
// overwrite the result slot.  Caller has already ended the z_stream.
static void zlib_vm_output_finish(Trix *trx, vm_t *dst_base, length_t length, Object *result_ptr) {
    dst_base[length] = '\0';
    auto [str_ptr, str_offset] = trx->vm_end_alloc<vm_t>(static_cast<vm_size_t>(length + 1));
    static_cast<void>(str_ptr);
    *result_ptr = Object::make_string(str_offset, length);
}

// Shared core for deflate / deflate-level.  Compresses the bytes of `src_sv`
// into the VM heap as a raw RFC 1951 deflate stream (no zlib header/trailer
// -- callers that need the zlib (RFC 1950) or gzip (RFC 1952) wrappers add
// the ~6 bytes of framing themselves with `crc32` / `adler32`) and replaces
// `result_ptr` (already pointing at the source-string slot or its level
// neighbour) with the resulting String.  Uses zlib's deflate API directly
// rather than `compress2` so we can pass `windowBits = -MAX_WBITS` for the
// raw stream.  Output buffer is the entire remaining VM scratch region;
// for typical inputs that's vastly larger than `compressBound(srcLen)` so
// only catastrophic compressor expansion (or VM near-exhaustion) raises
// vm-full.  `level` must already be in 0..9 or Z_DEFAULT_COMPRESSION.
static void deflate_impl(Trix *trx, Object *result_ptr, std::string_view src_sv, int level) {
    z_stream stream{};
    auto src_size = static_cast<uLong>(src_sv.size());
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(src_sv.data()));
    stream.avail_in = static_cast<uInt>(src_size);

    if (deflateInit2(&stream, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        trx->error(Error::InternalError, "deflate: deflateInit2 failed: {}", stream.msg ? stream.msg : "unknown");
    } else {
        auto dst_base = zlib_vm_output_start(trx, &stream);

        auto rc = ::deflate(&stream, Z_FINISH);
        if (rc != Z_STREAM_END) {
            trx->vm_end_alloc();
            const char *saved_msg = stream.msg ? stream.msg : "unknown";
            deflateEnd(&stream);
            if ((rc == Z_OK) || (rc == Z_BUF_ERROR)) {
                // Output buffer exhausted before deflate could finish.
                trx->error(Error::VMFull, "deflate: output exceeds available VM space");
            } else {
                trx->error(Error::InternalError, "deflate: failed (rc={}): {}", rc, saved_msg);
            }
        }
        auto length = static_cast<length_t>(stream.total_out);
        deflateEnd(&stream);
        zlib_vm_output_finish(trx, dst_base, length, result_ptr);
    }
}

// deflate: str :- str
// Compresses `str` with raw RFC 1951 DEFLATE at the default level (6).
// The result is the bare deflate bitstream -- no zlib (RFC 1950) or gzip
// (RFC 1952) header/trailer.  Callers that need those formats wrap the
// output with the appropriate framing themselves (zlib: 2-byte header +
// adler32 trailer; gzip: 10-byte header + crc32 trailer + size trailer).
// Empty string compresses to a non-empty 2-byte "empty stored block".
// throws: opstack-underflow, type-check, vm-full, internal-error
static void deflate_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto str_ptr = trx->m_op_ptr;
    auto sv = str_ptr->sv_value(trx);
    deflate_impl(trx, str_ptr, sv, Z_DEFAULT_COMPRESSION);
}

// deflate-level: str int :- str
// Like `deflate`, but with an explicit compression level: 0 = no compression
// (stored blocks only -- output is slightly larger than input but minimal CPU),
// 1 = fastest, 9 = best compression / slowest.  Level 6 matches `deflate`.
// throws: opstack-underflow, range-check, type-check, vm-full, internal-error
static void deflate_level_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers32 | VerifyNotNegative, VerifyString);

    auto level_ptr = trx->m_op_ptr;
    auto str_ptr = (level_ptr - 1);
    auto [valid, level] = level_ptr->integer_value(trx, 0, 9);
    if (!valid) {
        trx->error(Error::RangeCheck, "deflate-level: level must be in 0..9");
    } else {
        auto sv = str_ptr->sv_value(trx);
        deflate_impl(trx, str_ptr, sv, static_cast<int>(level));
        --trx->m_op_ptr;
    }
}

// inflate: str :- str
// Decompresses `str`, which must be a raw RFC 1951 DEFLATE stream produced
// by `deflate` / `deflate-level` (or stripped of its zlib/gzip wrapper).
// Output size is bounded by available VM space (effectively `--vm-size`)
// rather than a fixed cap -- pathological "zip bomb" inputs simply raise
// vm-full once the decompressor would overrun the scratch region.
// Malformed inputs raise range-check.  Empty input raises range-check
// (deflate streams always have at least one block header).
// throws: opstack-underflow, range-check, type-check, vm-full, internal-error
static void inflate_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto str_ptr = trx->m_op_ptr;
    auto sv = str_ptr->sv_value(trx);

    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(sv.data()));
    stream.avail_in = static_cast<uInt>(sv.size());

    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        trx->error(Error::InternalError, "inflate: inflateInit2 failed: {}", stream.msg ? stream.msg : "unknown");
    } else {
        auto dst_base = zlib_vm_output_start(trx, &stream);

        auto rc = ::inflate(&stream, Z_FINISH);
        if (rc != Z_STREAM_END) {
            trx->vm_end_alloc();
            const char *saved_msg = stream.msg ? stream.msg : "unknown";
            auto out_was_full = (stream.avail_out == 0);
            inflateEnd(&stream);
            if ((rc == Z_BUF_ERROR) && out_was_full) {
                // Output didn't fit -- VM exhausted (genuine zip bomb or `--vm-size`
                // too small for the legitimate payload).
                trx->error(Error::VMFull, "inflate: output exceeds available VM space");
            } else {
                // Z_DATA_ERROR, Z_NEED_DICT (preset dictionary -- not supported),
                // Z_STREAM_ERROR, or Z_BUF_ERROR with avail_out > 0 (truncated input).
                trx->error(Error::RangeCheck, "inflate: malformed deflate stream: {}", saved_msg);
            }
        }
        auto length = static_cast<length_t>(stream.total_out);
        inflateEnd(&stream);
        zlib_vm_output_finish(trx, dst_base, length, str_ptr);
    }
}

#else  // TRIX_NO_ZLIB: deflate/inflate need zlib -- stub them to /unsupported.

// deflate (TRIX_NO_ZLIB stub): the operator stays registered but raises
// /unsupported in a build without zlib.  See build_config.inl ZlibEnabled.
static void deflate_op(Trix *trx) {
    trx->error(Error::Unsupported, "deflate: built without zlib support (TRIX_NO_ZLIB)");
}

// deflate-level (TRIX_NO_ZLIB stub)
static void deflate_level_op(Trix *trx) {
    trx->error(Error::Unsupported, "deflate-level: built without zlib support (TRIX_NO_ZLIB)");
}

// inflate (TRIX_NO_ZLIB stub)
static void inflate_op(Trix *trx) {
    trx->error(Error::Unsupported, "inflate: built without zlib support (TRIX_NO_ZLIB)");
}

#endif  // TRIX_NO_ZLIB

// Internal scratch-buffer size for the streaming compress ops.  Two
// buffers of this size (input + output) live on the C++ stack across a
// single deflate-stream / inflate-stream call.  4 KiB each keeps the
// pair (~8 KiB) under the per-function -Wstack-usage=16000 budget while
// still feeding zlib enough bytes per call to amortise its per-call
// overhead (zlib itself batches further internally).
static constexpr size_t StreamingCompressBufSize{4 * 1024};

// Drain up to `max` bytes from `stream` into `dst`, stopping at EOF.
// Returns the actual count read (0 == EOF reached with no fresh bytes).
// Uses the per-character getc fast path; EOFc terminates the loop
// without raising (unlike Stream::getn, which throws EndOfStream).
[[nodiscard]] static size_t stream_read_some(Trix *trx, Stream *stream, vm_t *dst, size_t max) {
    size_t got = 0;
    while (got < max) {
        auto ch = stream->getc(trx);
        if (ch == EOFc) {
            break;
        } else {
            dst[got++] = static_cast<vm_t>(ch & 0xFF);
        }
    }
    return got;
}

#ifndef TRIX_NO_ZLIB

// Shared core for deflate-stream / deflate-stream-level.  Reads `in_stream`
// to EOF and writes the raw RFC 1951 deflate output to `out_stream`.  Uses
// zlib's incremental API: Z_NO_FLUSH while input is available, Z_FINISH
// after the source EOF.  Both streams must already be open and oriented
// the right way (input readable, output writable); errors raised by the
// underlying Stream operations (io-read-error / io-write-error / etc)
// propagate through unchanged.
//
// On any zlib failure the matching deflateEnd is called before raising
// the Trix error so the z_stream's internal allocations are freed.
static void deflate_stream_impl(Trix *trx, Stream *in_stream, Stream *out_stream, int level) {
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        trx->error(Error::InternalError, "deflate-stream: deflateInit2 failed: {}", zs.msg ? zs.msg : "unknown");
    } else {
        vm_t in_buf[StreamingCompressBufSize];
        vm_t out_buf[StreamingCompressBufSize];

        auto pump_output = [&](int flag) -> int {
            do {
                zs.next_out = reinterpret_cast<Bytef *>(out_buf);
                zs.avail_out = static_cast<uInt>(StreamingCompressBufSize);
                auto rc = ::deflate(&zs, flag);
                auto produced = static_cast<size_t>(StreamingCompressBufSize - zs.avail_out);
                if (produced != 0) {
                    out_stream->putn(trx, out_buf, produced);
                }
                if (rc == Z_STREAM_END) {
                    return rc;
                } else if ((rc != Z_OK) && (rc != Z_BUF_ERROR)) {
                    const char *saved_msg = zs.msg ? zs.msg : "unknown";
                    deflateEnd(&zs);
                    trx->error(Error::InternalError, "deflate-stream: failed (rc={}): {}", rc, saved_msg);
                }
            } while (zs.avail_out == 0);
            return Z_OK;
        };

        while (true) {
            auto in_count = stream_read_some(trx, in_stream, in_buf, StreamingCompressBufSize);
            if (in_count == 0) {
                break;
            } else {
                zs.next_in = reinterpret_cast<Bytef *>(in_buf);
                zs.avail_in = static_cast<uInt>(in_count);
                while (zs.avail_in != 0) {
                    pump_output(Z_NO_FLUSH);
                }
            }
        }
        pump_output(Z_FINISH);
        deflateEnd(&zs);
    }
}

// deflate-stream: in-stream out-stream :- --
// Reads `in-stream` until EOF and writes the raw RFC 1951 DEFLATE
// output (default level 6) to `out-stream`.  Both streams stay open;
// the caller is responsible for closing them.  Output is the bare
// bitstream -- no zlib (RFC 1950) or gzip (RFC 1952) framing -- so
// callers add the wrapping bytes themselves on the output side if
// needed.  Suitable for inputs larger than MaxStringLength: the entire
// payload never has to fit in a single Trix string.
// throws: invalid-stream, invalid-stream-access, io-read-error,
//         io-write-error, opstack-underflow, type-check, internal-error
static void deflate_stream_op(Trix *trx) {
    trx->verify_operands(VerifyStream, VerifyStream);

    auto out_ptr = trx->m_op_ptr;
    auto in_ptr = (out_ptr - 1);
    auto [in_stream, in_sid] = in_ptr->stream_value(trx);
    auto [out_stream, out_sid] = out_ptr->stream_value(trx);
    if (!in_stream->is_readable(in_sid)) {
        trx->error(Error::InvalidStreamAccess, "deflate-stream: input stream is not readable");
    } else if (!out_stream->is_writable(out_sid)) {
        trx->error(Error::InvalidStreamAccess, "deflate-stream: output stream is not writable");
    } else {
        deflate_stream_impl(trx, in_stream, out_stream, Z_DEFAULT_COMPRESSION);
        trx->m_op_ptr -= 2;
    }
}

// deflate-stream-level: in-stream out-stream int :- --
// Like `deflate-stream`, but with an explicit compression level (0..9).
// Level 0 = stored blocks only (uncompressed); 9 = best compression.
// throws: invalid-stream, invalid-stream-access, io-read-error,
//         io-write-error, opstack-underflow, range-check, type-check,
//         internal-error
static void deflate_stream_level_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers32 | VerifyNotNegative, VerifyStream, VerifyStream);

    auto level_ptr = trx->m_op_ptr;
    auto out_ptr = (level_ptr - 1);
    auto in_ptr = (out_ptr - 1);
    auto [valid, level] = level_ptr->integer_value(trx, 0, 9);
    if (!valid) {
        trx->error(Error::RangeCheck, "deflate-stream-level: level must be in 0..9");
    } else {
        auto [in_stream, in_sid] = in_ptr->stream_value(trx);
        auto [out_stream, out_sid] = out_ptr->stream_value(trx);
        if (!in_stream->is_readable(in_sid)) {
            trx->error(Error::InvalidStreamAccess, "deflate-stream-level: input stream is not readable");
        } else if (!out_stream->is_writable(out_sid)) {
            trx->error(Error::InvalidStreamAccess, "deflate-stream-level: output stream is not writable");
        } else {
            deflate_stream_impl(trx, in_stream, out_stream, static_cast<int>(level));
            trx->m_op_ptr -= 3;
        }
    }
}

// inflate-stream: in-stream out-stream :- --
// Reads a raw RFC 1951 DEFLATE bitstream from `in-stream` until EOF and
// writes the inflated output to `out-stream`.  Both streams stay open;
// caller closes them.  No fixed output cap -- output size is bounded
// only by what `out-stream` can absorb (file streams are effectively
// unbounded; string-streams cap at MaxStringLength and raise io-write-
// error if exceeded).  Malformed deflate input raises range-check.
// throws: invalid-stream, invalid-stream-access, io-read-error,
//         io-write-error, opstack-underflow, range-check, type-check,
//         internal-error
static void inflate_stream_op(Trix *trx) {
    trx->verify_operands(VerifyStream, VerifyStream);

    auto out_ptr = trx->m_op_ptr;
    auto in_ptr = (out_ptr - 1);
    auto [in_stream, in_sid] = in_ptr->stream_value(trx);
    auto [out_stream, out_sid] = out_ptr->stream_value(trx);
    if (!in_stream->is_readable(in_sid)) {
        trx->error(Error::InvalidStreamAccess, "inflate-stream: input stream is not readable");
    } else if (!out_stream->is_writable(out_sid)) {
        trx->error(Error::InvalidStreamAccess, "inflate-stream: output stream is not writable");
    } else {
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            trx->error(Error::InternalError, "inflate-stream: inflateInit2 failed: {}", zs.msg ? zs.msg : "unknown");
        } else {
            vm_t in_buf[StreamingCompressBufSize];
            vm_t out_buf[StreamingCompressBufSize];
            auto saw_eof = false;
            auto stream_done = false;

            while (!stream_done) {
                if ((zs.avail_in == 0) && !saw_eof) {
                    auto in_count = stream_read_some(trx, in_stream, in_buf, StreamingCompressBufSize);
                    if (in_count == 0) {
                        saw_eof = true;
                    }
                    zs.next_in = reinterpret_cast<Bytef *>(in_buf);
                    zs.avail_in = static_cast<uInt>(in_count);
                }
                zs.next_out = reinterpret_cast<Bytef *>(out_buf);
                zs.avail_out = static_cast<uInt>(StreamingCompressBufSize);
                auto rc = ::inflate(&zs, saw_eof ? Z_FINISH : Z_NO_FLUSH);
                auto produced = static_cast<size_t>(StreamingCompressBufSize - zs.avail_out);
                if (produced != 0) {
                    out_stream->putn(trx, out_buf, produced);
                }
                if (rc == Z_STREAM_END) {
                    stream_done = true;
                } else if (rc == Z_BUF_ERROR) {
                    // Need more output space (loop again) -- or, if we've already seen
                    // EOF without producing more output, the input is truncated.
                    if (saw_eof && (produced == 0)) {
                        inflateEnd(&zs);
                        trx->error(Error::RangeCheck, "inflate-stream: truncated deflate stream");
                    }
                } else if (rc != Z_OK) {
                    const char *saved_msg = zs.msg ? zs.msg : "unknown";
                    inflateEnd(&zs);
                    trx->error(Error::RangeCheck, "inflate-stream: malformed deflate stream: {}", saved_msg);
                }
            }
            inflateEnd(&zs);
            trx->m_op_ptr -= 2;
        }
    }
}

#else  // TRIX_NO_ZLIB: deflate-stream/inflate-stream need zlib -- stub them.

// deflate-stream (TRIX_NO_ZLIB stub)
static void deflate_stream_op(Trix *trx) {
    trx->error(Error::Unsupported, "deflate-stream: built without zlib support (TRIX_NO_ZLIB)");
}

// deflate-stream-level (TRIX_NO_ZLIB stub)
static void deflate_stream_level_op(Trix *trx) {
    trx->error(Error::Unsupported, "deflate-stream-level: built without zlib support (TRIX_NO_ZLIB)");
}

// inflate-stream (TRIX_NO_ZLIB stub)
static void inflate_stream_op(Trix *trx) {
    trx->error(Error::Unsupported, "inflate-stream: built without zlib support (TRIX_NO_ZLIB)");
}

#endif  // TRIX_NO_ZLIB

// adler32-stream: in-stream :- uint32
// Streaming counterpart of `adler32`.  Reads `in-stream` to EOF and
// returns RFC 1950 Adler-32 of all bytes read.  Useful for computing
// the zlib trailer when the source bytes don't fit in a single Trix
// string (paired with `deflate-stream`).  Empty stream produces the
// initial state 0x00000001.
// throws: invalid-stream, invalid-stream-access, io-read-error,
//         opstack-underflow, type-check
static void adler32_stream_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto str_ptr = trx->m_op_ptr;
    auto [stream, sid] = str_ptr->stream_value(trx);
    if (!stream->is_readable(sid)) {
        trx->error(Error::InvalidStreamAccess, "adler32-stream: stream is not readable");
    } else {
        constexpr uint32_t Mod = 65521;
        static_assert(StreamingCompressBufSize <= 5552,
                      "adler32 mod-deferral requires per-iteration accumulation <= NMAX=5552 to avoid uint32 overflow");
        uint32_t s1 = 1;
        uint32_t s2 = 0;

        vm_t buf[StreamingCompressBufSize];
        while (true) {
            auto count = stream_read_some(trx, stream, buf, StreamingCompressBufSize);
            if (count == 0) {
                break;
            } else {
                // The buffer (4 KiB) is below the 5552-byte NMAX bound (see static_assert
                // above), so each chunk is one accumulate-then-mod pass with no risk of
                // uint32 overflow.  Defer the mod across chunks for symmetry with the
                // one-shot op (and to avoid per-byte mod).
                for (size_t i = 0; i < count; ++i) {
                    s1 += static_cast<uint8_t>(buf[i]);
                    s2 += s1;
                }
                s1 %= Mod;
                s2 %= Mod;
            }
        }
        auto result = (s2 << 16) | s1;
        *str_ptr = Object::make_uinteger(static_cast<uinteger_t>(result));
    }
}

// crc32-stream: in-stream :- uint32
// Streaming counterpart of `crc32`.  Reads `in-stream` to EOF and
// returns the reflected-polynomial CRC-32 of all bytes read (matches
// `crc32` byte-for-byte on identical input).  Reuses the snapshot-side
// `crc32_update` helper so there is exactly one CRC implementation.
// Empty stream returns 0.
// throws: invalid-stream, invalid-stream-access, io-read-error,
//         opstack-underflow, type-check
static void crc32_stream_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto str_ptr = trx->m_op_ptr;
    auto [stream, sid] = str_ptr->stream_value(trx);
    if (!stream->is_readable(sid)) {
        trx->error(Error::InvalidStreamAccess, "crc32-stream: stream is not readable");
    } else {
        crc32_t crc = 0;
        vm_t buf[StreamingCompressBufSize];
        while (true) {
            auto count = stream_read_some(trx, stream, buf, StreamingCompressBufSize);
            if (count == 0) {
                break;
            } else {
                crc = crc32_update(crc, buf, count);
            }
        }
        *str_ptr = Object::make_uinteger(static_cast<uinteger_t>(crc));
    }
}

// fletcher32-stream: in-stream :- uint32
// Streaming counterpart of `fletcher32`.  Reads `in-stream` to EOF and
// returns Fletcher-32 over 16-bit little-endian words formed from the
// byte stream (matches `fletcher32` byte-for-byte on identical input,
// including the odd-length zero-pad rule).  A single trailing byte at a
// chunk boundary is stashed and paired with the first byte of the next
// chunk; at EOF an unpaired stashed byte is paired with 0.
// throws: invalid-stream, invalid-stream-access, io-read-error,
//         opstack-underflow, type-check
static void fletcher32_stream_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto str_ptr = trx->m_op_ptr;
    auto [stream, sid] = str_ptr->stream_value(trx);
    if (!stream->is_readable(sid)) {
        trx->error(Error::InvalidStreamAccess, "fletcher32-stream: stream is not readable");
    } else {
        uint32_t sum1 = 0;
        uint32_t sum2 = 0;
        bool has_pending = false;
        uint8_t pending = 0;

        vm_t buf[StreamingCompressBufSize];
        while (true) {
            auto count = stream_read_some(trx, stream, buf, StreamingCompressBufSize);
            if (count == 0) {
                break;
            } else {
                size_t i = 0;
                if (has_pending) {
                    // Pair the stashed byte (low) with this chunk's first byte (high).
                    auto lo = pending;
                    auto hi = static_cast<uint8_t>(buf[0]);
                    auto word = static_cast<uint32_t>((hi << 8) | lo);
                    sum1 = (sum1 + word) % 65535u;
                    sum2 = (sum2 + sum1) % 65535u;
                    has_pending = false;
                    i = 1;
                }
                while ((i + 1) < count) {
                    auto lo = static_cast<uint8_t>(buf[i]);
                    auto hi = static_cast<uint8_t>(buf[i + 1]);
                    auto word = static_cast<uint32_t>((hi << 8) | lo);
                    sum1 = (sum1 + word) % 65535u;
                    sum2 = (sum2 + sum1) % 65535u;
                    i += 2;
                }
                if (i < count) {
                    // Odd byte at chunk end -- stash for the next chunk.
                    pending = static_cast<uint8_t>(buf[i]);
                    has_pending = true;
                }
            }
        }
        if (has_pending) {
            // Total length was odd: pair the stashed byte with a 0 high byte.
            auto word = static_cast<uint32_t>(pending);
            sum1 = (sum1 + word) % 65535u;
            sum2 = (sum2 + sum1) % 65535u;
        }
        auto result = (sum2 << 16) | sum1;
        *str_ptr = Object::make_uinteger(static_cast<uinteger_t>(result));
    }
}

// make-memory-stream: str :- in-stream
// Wraps a Trix string as a read-only Stream by borrowing the string's
// VM-heap bytes -- no host-side allocation, no copy.  The Stream's
// IsBorrowed flag suppresses the close-time `std::free` that the
// snapshot / invoke memory-stream paths rely on, so the underlying
// VM-heap memory is left to the Save system to manage.  Useful for
// feeding in-memory bytes to ops like `deflate-stream`,
// `adler32-stream`, or any other Stream-consuming operator without
// round-tripping through a temp file.  Output stream supports the
// standard read ops (read-all, read-string, read-line, getc, ...).
//
// Lifetime: the source string and the returned Stream are both
// allocated at the current save level.  A `restore` past that level
// invalidates both together, so the borrowed pointer never outlives
// its backing storage.
//
// throws: limit-check (all stream slots in use), opstack-underflow,
//         type-check
static void make_memory_stream_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto str_ptr = trx->m_op_ptr;
    auto sv = str_ptr->sv_value(trx);
    auto length = sv.size();

    auto result = Stream::open_memory(trx, reinterpret_cast<const vm_t *>(sv.data()), length, /*owns_data=*/false);
    if (!result.success) {
        trx->error(Error::LimitCheck, "make-memory-stream: all stream slots in use");
    } else {
        str_ptr->maybe_free_extvalue(trx);
        *str_ptr = Object::make_stream(result.offset, result.sid, Object::LiteralAttrib, Object::ReadOnlyAccess);
    }
}

// alloc: int :- addr
// Allocates size bytes and pushes the address.
// throws: vm-full, limit-check, opstack-underflow, range-check, type-check, unsupported
static void alloc_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "alloc: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyIntegers);

        auto size_ptr = trx->m_op_ptr;
        auto [valid, size] = size_ptr->uinteger_value(trx, 0);
        if (!valid) {
            // uinteger_value clamps too-large Long/ULong to UINT32_MAX (size != 0);
            // negative values produce 0.
            if (size != 0) {
                trx->error(Error::RangeCheck, "alloc: size exceeds maximum");
            } else {
                trx->error(Error::RangeCheck, "alloc: size must be positive");
            }
        } else if (size == 0) {
            trx->error(Error::RangeCheck, "alloc: size must be positive");
        }

        // Reserve the ExtValue slot before malloc so VM-OOM errors before
        // committing to any allocation, not after (which would leak ptr).
        auto result = Object::make_address(trx, nullptr);
        auto ptr = std::malloc(size);
        if (ptr == nullptr) {
            result.free_extvalue(trx);
            trx->error(Error::LimitCheck, "alloc: allocation of {} bytes failed", size);
        } else {
            result.update_address(trx, ptr);

            size_ptr->maybe_free_extvalue(trx);
            *size_ptr = result;
        }
    }
}

// free: addr :- --
// Frees memory at the given address.
// throws: opstack-underflow, type-check, unsupported
static void free_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "free: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyAddress);

        auto addr_ptr = trx->m_op_ptr;
        auto ptr = addr_ptr->address_value(trx);
        // Only free if the pointer is valid (not null and addressable).
        if (trx->address_state(ptr, sizeof(vm_t)) >= AddressState::IsReadOnly) {
            std::free(ptr);
        }

        addr_ptr->free_extvalue(trx);
        --trx->m_op_ptr;
    }
}

// address-state?: addr :- addr name
// Probes the address and pushes one of: /addr-invalid /addr-null
// /addr-read-only /addr-read-write.  Caches the result in the
// Address object's F/X bits so subsequent peek/poke skip the probe.
// throws: opstack-overflow, opstack-underflow, type-check
static void address_state_pred_op(Trix *trx) {
    trx->verify_operands(VerifyAddress);
    trx->require_op_capacity(1);

    auto addr_ptr = trx->m_op_ptr;
    auto address = addr_ptr->address_value(trx);

    // Null pointer is detected directly -- no probe or cache needed.
    SystemName name;
    if (address == nullptr) {
        name = SystemName::Addr_NullPtr;
    } else {
        if (!addr_ptr->has_address_cache()) {
            addr_ptr->set_address_cache(trx->address_state(address, sizeof(vm_t)));
        }
        auto state = addr_ptr->cached_address_state();
        switch (state) {
        case AddressState::Invalid:
        case AddressState::IsNullPtr:
            name = SystemName::Addr_Invalid;
            break;

        case AddressState::IsReadOnly:
            name = SystemName::Addr_ReadOnly;
            break;

        case AddressState::IsReadWrite:
            name = SystemName::Addr_ReadWrite;
            break;
        }
    }

    *++trx->m_op_ptr = Name::make_system(trx, name);
}

// object-to-binary-token: any :- str
// Encodes an object as a binary token string.
// throws: opstack-underflow, type-check
static void object_to_binary_token_op(Trix *trx) {
    trx->verify_operands(VerifyNull | VerifyNumbers | VerifyBoolean | VerifyMark | VerifyName | VerifyString);

    auto val_ptr = trx->m_op_ptr;
    auto result = make_binary_token_string(trx, *val_ptr);
    val_ptr->maybe_free_extvalue(trx);
    *val_ptr = result;
}
template<BitOpKind Kind, typename OpFn>
static void bit_position_op(Trix *trx, const char *name, OpFn &&op_fn) {
    trx->verify_operands(VerifyIntegers, VerifyIntegers);

    auto pos_ptr = trx->m_op_ptr;
    auto val_ptr = (pos_ptr - 1);

    // Extract bit position into a wide signed value.  Narrowing to int BEFORE
    // the range check (below) let a 64/128-bit position such as 2^32 truncate
    // down to a valid small index and silently address the wrong bit; capture
    // the full value so genuinely out-of-range positions are rejected.
    auto pos_raw = int128_t{0};
    switch (+pos_ptr->type()) {
    case +Object::Type::Byte:
        pos_raw = static_cast<int128_t>(pos_ptr->byte_value());
        break;

    case +Object::Type::Integer:
        pos_raw = static_cast<int128_t>(pos_ptr->integer_value());
        break;

    case +Object::Type::UInteger:
        pos_raw = static_cast<int128_t>(pos_ptr->uinteger_value());
        break;

    case +Object::Type::Long:
        pos_raw = static_cast<int128_t>(pos_ptr->long_value(trx));
        break;

    case +Object::Type::ULong:
        pos_raw = static_cast<int128_t>(pos_ptr->ulong_value(trx));
        break;

    case +Object::Type::Int128:
        pos_raw = pos_ptr->int128_value(trx);
        break;

    case +Object::Type::UInt128:
        pos_raw = static_cast<int128_t>(pos_ptr->uint128_value(trx));
        break;

    default:
        trx->error(Error::TypeCheck, "{}: bit position must be integer", name);
    }

    // Determine max bit and extract value (widened to 128-bit so Int128/UInt128 fit)
    auto max_bit = int{0};
    auto raw = uint128_t{0};
    switch (+val_ptr->type()) {
    case +Object::Type::Byte:
        max_bit = 7;
        raw = val_ptr->byte_value();
        break;

    case +Object::Type::Integer:
        max_bit = 31;
        raw = static_cast<uint128_t>(static_cast<uint32_t>(val_ptr->integer_value()));
        break;

    case +Object::Type::UInteger:
        max_bit = 31;
        raw = val_ptr->uinteger_value();
        break;

    case +Object::Type::Long:
        max_bit = 63;
        raw = static_cast<uint128_t>(static_cast<uint64_t>(val_ptr->long_value(trx)));
        break;

    case +Object::Type::ULong:
        max_bit = 63;
        raw = val_ptr->ulong_value(trx);
        break;

    case +Object::Type::Int128:
        max_bit = 127;
        raw = static_cast<uint128_t>(val_ptr->int128_value(trx));
        break;

    case +Object::Type::UInt128:
        max_bit = 127;
        raw = val_ptr->uint128_value(trx);
        break;

    default:
        trx->error(Error::TypeCheck, "{}: value must be integer", name);
    }

    if ((pos_raw < 0) || (pos_raw > max_bit)) {
        trx->error(Error::RangeCheck, "{}: bit position {} out of range 0..{}", name, static_cast<long_t>(pos_raw), max_bit);
    } else {
        auto mask = uint128_t{1} << static_cast<unsigned>(pos_raw);
        auto result_raw = op_fn(raw, mask);

        auto val_type = val_ptr->type();
        // Free ExtValues/WideValues before overwriting
        pos_ptr->maybe_free_extvalue(trx);
        val_ptr->maybe_free_extvalue(trx);

        if constexpr (Kind == BitOpKind::Test) {
            *val_ptr = Object::make_boolean(result_raw != 0);
        } else {
            // Reconstruct same type as input
            auto result = result_raw;
            switch (+val_type) {
            case +Object::Type::Byte:
                *val_ptr = Object::make_byte(static_cast<vm_t>(static_cast<uint64_t>(result) & 0xFF));
                break;

            case +Object::Type::Integer:
                *val_ptr = Object::make_integer(static_cast<integer_t>(static_cast<int32_t>(static_cast<uint32_t>(result))));
                break;

            case +Object::Type::UInteger:
                *val_ptr = Object::make_uinteger(static_cast<uinteger_t>(static_cast<uint32_t>(result)));
                break;

            case +Object::Type::Long:
                *val_ptr = Object::make_long(trx, static_cast<long_t>(static_cast<uint64_t>(result)));
                break;

            case +Object::Type::ULong:
                *val_ptr = Object::make_ulong(trx, static_cast<ulong_t>(static_cast<uint64_t>(result)));
                break;

            case +Object::Type::Int128:
                *val_ptr = Object::make_int128(trx, static_cast<int128_t>(result));
                break;

            case +Object::Type::UInt128:
                *val_ptr = Object::make_uint128(trx, result);
                break;

            default:
                std::unreachable();
            }
        }
        trx->m_op_ptr = val_ptr;
    }
}

//===--- Binary Token Utilities ---===//

// Encodes object as a read-only binary token string.
// Raises TypeCheck for unencodable types: Address, Operator, Array, Packed, Stream, Dict, SourceLoc.
// Raises Undefined if object is a Name not registered in the system name table.
// Raises LimitCheck if the resulting binary token string would exceed MaxStringLength.
[[nodiscard]] static Object make_binary_token_string(Trix *trx, Object object) {
    auto header_only = [trx](BinaryToken token) -> Object {
        const vm_t buf[1] = {static_cast<vm_t>(token)};
        return Object::make_string(trx, buf, length_t{1});
    };

    auto from_buf = [trx](const vm_t *buf, length_t n) -> Object { return Object::make_string(trx, buf, n); };

    switch (object.type()) {
    case Object::Type::Address:
    case Object::Type::Operator:
    case Object::Type::Array:
    case Object::Type::Packed:
    case Object::Type::Stream:
    case Object::Type::Dict:
    case Object::Type::Set:
    case Object::Type::SourceLoc:
    case Object::Type::Curry:
    case Object::Type::Thunk:
    case Object::Type::Tagged:
    case Object::Type::Record:
    case Object::Type::Coroutine:
    case Object::Type::PipeBuffer:
    case Object::Type::Cell:
    case Object::Type::Continuation:
    case Object::Type::OpaqueHandle:
        trx->error(Error::TypeCheck, "make_binary_token_string: cannot encode {}", Object::type_sv(object.type()));

    case Object::Type::Int128: {
        vm_t buf[17];
        buf[0] = static_cast<vm_t>(BinaryToken::Int128);
        auto val = object.int128_value(trx);
        std::memcpy(buf + 1, &val, sizeof(val));
        return from_buf(buf, 17);
    }

    case Object::Type::UInt128: {
        vm_t buf[17];
        buf[0] = static_cast<vm_t>(BinaryToken::UInt128);
        auto val = object.uint128_value(trx);
        std::memcpy(buf + 1, &val, sizeof(val));
        return from_buf(buf, 17);
    }

    case Object::Type::Null:
        return header_only(BinaryToken::Null);

    case Object::Type::Mark:
        return header_only(BinaryToken::Mark);

    case Object::Type::Boolean:
        return header_only(object.boolean_value() ? BinaryToken::True : BinaryToken::False);

    case Object::Type::Byte: {
        auto val = object.byte_value();
        switch (val) {
        case 0:
            return header_only(BinaryToken::Byte_0);

        case 1:
            return header_only(BinaryToken::Byte_1);

        case 2:
            return header_only(BinaryToken::Byte_2);

        case 127:
            return header_only(BinaryToken::Byte_127);

        case 128:
            return header_only(BinaryToken::Byte_128);

        case 255:
            return header_only(BinaryToken::Byte_255);

        default: {
            vm_t buf[2] = {static_cast<vm_t>(BinaryToken::Byte), val};
            return from_buf(buf, 2);
        }
        }
    }

    case Object::Type::Integer: {
        auto val = object.integer_value();
        if (val == std::numeric_limits<integer_t>::min()) {
            return header_only(BinaryToken::Integer_Min);
        } else if (val == std::numeric_limits<integer_t>::max()) {
            return header_only(BinaryToken::Integer_Max);
        } else if (val == -2) {
            return header_only(BinaryToken::Integer_Neg2);
        } else if (val == -1) {
            return header_only(BinaryToken::Integer_Neg1);
        } else if (val == 0) {
            return header_only(BinaryToken::Integer_0);
        } else if (val == 1) {
            return header_only(BinaryToken::Integer_1);
        } else if (val == 2) {
            return header_only(BinaryToken::Integer_2);
        } else if (val == 10) {
            return header_only(BinaryToken::Integer_10);
        } else if (val == 100) {
            return header_only(BinaryToken::Integer_100);
        } else {
            vm_t buf[5];
            length_t len;
            if ((val >= INT8_MIN) && (val <= INT8_MAX)) {
                buf[0] = static_cast<vm_t>(BinaryToken::Integer_8);
                buf[1] = static_cast<vm_t>(static_cast<int8_t>(val));
                len = 2;
            } else if ((val >= INT16_MIN) && (val <= INT16_MAX)) {
                buf[0] = static_cast<vm_t>(BinaryToken::Integer_16);
                auto v16 = static_cast<int16_t>(val);
                std::memcpy(buf + 1, &v16, sizeof(v16));
                len = 3;
            } else {
                buf[0] = static_cast<vm_t>(BinaryToken::Integer_32);
                std::memcpy(buf + 1, &val, sizeof(val));
                len = 5;
            }
            return from_buf(buf, len);
        }
    }

    case Object::Type::UInteger: {
        auto val = object.uinteger_value();
        if (val == std::numeric_limits<uinteger_t>::min()) {
            return header_only(BinaryToken::UInteger_Min);
        } else if (val == std::numeric_limits<uinteger_t>::max()) {
            return header_only(BinaryToken::UInteger_Max);
        } else if (val == 1) {
            return header_only(BinaryToken::UInteger_1);
        } else {
            vm_t buf[5];
            length_t len;
            if (val <= UINT8_MAX) {
                buf[0] = static_cast<vm_t>(BinaryToken::UInteger_8);
                buf[1] = static_cast<vm_t>(val);
                len = 2;
            } else if (val <= UINT16_MAX) {
                buf[0] = static_cast<vm_t>(BinaryToken::UInteger_16);
                auto v16 = static_cast<uint16_t>(val);
                std::memcpy(buf + 1, &v16, sizeof(v16));
                len = 3;
            } else {
                buf[0] = static_cast<vm_t>(BinaryToken::UInteger_32);
                std::memcpy(buf + 1, &val, sizeof(val));
                len = 5;
            }
            return from_buf(buf, len);
        }
    }

    case Object::Type::Long: {
        auto val = object.long_value(trx);
        if (val == std::numeric_limits<long_t>::min()) {
            return header_only(BinaryToken::Long_Min);
        } else if (val == std::numeric_limits<long_t>::max()) {
            return header_only(BinaryToken::Long_Max);
        } else if (val == -1) {
            return header_only(BinaryToken::Long_Neg1);
        } else if (val == 0) {
            return header_only(BinaryToken::Long_0);
        } else if (val == 1) {
            return header_only(BinaryToken::Long_1);
        } else {
            vm_t buf[9];
            length_t len;
            if ((val >= INT8_MIN) && (val <= INT8_MAX)) {
                buf[0] = static_cast<vm_t>(BinaryToken::Long_8);
                buf[1] = static_cast<vm_t>(static_cast<int8_t>(val));
                len = 2;
            } else if ((val >= INT16_MIN) && (val <= INT16_MAX)) {
                buf[0] = static_cast<vm_t>(BinaryToken::Long_16);
                auto v16 = static_cast<int16_t>(val);
                std::memcpy(buf + 1, &v16, sizeof(v16));
                len = 3;
            } else if ((val >= INT32_MIN) && (val <= INT32_MAX)) {
                buf[0] = static_cast<vm_t>(BinaryToken::Long_32);
                auto v32 = static_cast<int32_t>(val);
                std::memcpy(buf + 1, &v32, sizeof(v32));
                len = 5;
            } else {
                buf[0] = static_cast<vm_t>(BinaryToken::Long_64);
                std::memcpy(buf + 1, &val, sizeof(val));
                len = 9;
            }
            return from_buf(buf, len);
        }
    }

    case Object::Type::ULong: {
        auto val = object.ulong_value(trx);
        if (val == std::numeric_limits<ulong_t>::min()) {
            return header_only(BinaryToken::ULong_Min);
        } else if (val == std::numeric_limits<ulong_t>::max()) {
            return header_only(BinaryToken::ULong_Max);
        } else if (val == 1) {
            return header_only(BinaryToken::ULong_1);
        } else {
            vm_t buf[9];
            length_t len;
            if (val <= UINT8_MAX) {
                buf[0] = static_cast<vm_t>(BinaryToken::ULong_8);
                buf[1] = static_cast<vm_t>(val);
                len = 2;
            } else if (val <= UINT16_MAX) {
                buf[0] = static_cast<vm_t>(BinaryToken::ULong_16);
                auto v16 = static_cast<uint16_t>(val);
                std::memcpy(buf + 1, &v16, sizeof(v16));
                len = 3;
            } else if (val <= UINT32_MAX) {
                buf[0] = static_cast<vm_t>(BinaryToken::ULong_32);
                auto v32 = static_cast<uint32_t>(val);
                std::memcpy(buf + 1, &v32, sizeof(v32));
                len = 5;
            } else {
                buf[0] = static_cast<vm_t>(BinaryToken::ULong_64);
                std::memcpy(buf + 1, &val, sizeof(val));
                len = 9;
            }
            return from_buf(buf, len);
        }
    }

    case Object::Type::Real: {
        auto val = object.real_value();
        if (std::isnan(val)) {
            return header_only(BinaryToken::Real_qNAN);
        } else if (val == -1.0f) {
            return header_only(BinaryToken::Real_Neg1);
        } else {
            // Positive zero only; negative zero gets the full encoding to preserve IEEE 754 semantics.
            if ((val == 0.0f) && !std::signbit(val)) {
                return header_only(BinaryToken::Real_0);
            } else if (val == 1.0f) {
                return header_only(BinaryToken::Real_1);
            } else if (val == 2.0f) {
                return header_only(BinaryToken::Real_2);
            } else if (val == std::numbers::e_v<real_t>) {
                return header_only(BinaryToken::Real_e);
            } else if (val == std::numbers::pi_v<real_t>) {
                return header_only(BinaryToken::Real_pi);
            } else if (std::isinf(val) && (val > 0)) {
                return header_only(BinaryToken::Real_INF);
            } else {
                vm_t buf[5];
                buf[0] = static_cast<vm_t>(BinaryToken::Real);
                std::memcpy(buf + 1, &val, sizeof(val));
                return from_buf(buf, 5);
            }
        }
    }

    case Object::Type::Double: {
        auto val = object.double_value(trx);
        if (std::isnan(val)) {
            return header_only(BinaryToken::Double_qNAN);
        } else if (val == -1.0) {
            return header_only(BinaryToken::Double_Neg1);
        } else if ((val == 0.0) && !std::signbit(val)) {
            return header_only(BinaryToken::Double_0);
        } else if (val == 1.0) {
            return header_only(BinaryToken::Double_1);
        } else if (val == 2.0) {
            return header_only(BinaryToken::Double_2);
        } else if (val == std::numbers::e_v<double_t>) {
            return header_only(BinaryToken::Double_e);
        } else if (val == std::numbers::pi_v<double_t>) {
            return header_only(BinaryToken::Double_pi);
        } else if (std::isinf(val) && (val > 0)) {
            return header_only(BinaryToken::Double_INF);
        } else {
            vm_t buf[9];
            buf[0] = static_cast<vm_t>(BinaryToken::Double);
            std::memcpy(buf + 1, &val, sizeof(val));
            return from_buf(buf, 9);
        }
    }

    case Object::Type::Name: {
        auto name_off = object.name_offset();
        vm_t buf[3];
        // Linear scan over name tables.  Tables are small and this is not a hot path.
        length_t len;
        // Check well-known names first (compact 2-byte encoding)
        for (name_index_t i = 0; i < WELLKNOWN_COUNT; ++i) {
            if (trx->m_wellknown_offsets[i] == name_off) {
                buf[0] = static_cast<vm_t>(object.is_executable() ? BinaryToken::WellKnownExecName : BinaryToken::WellKnownLitName);
                buf[1] = static_cast<vm_t>(i);
                return from_buf(buf, 2);
            }
        }
        for (name_index_t i = 0; i < SYSTEMNAME_COUNT; ++i) {
            if (trx->m_systemname_offsets[i] == name_off) {
                if (i < 256) {
                    buf[0] = static_cast<vm_t>(object.is_executable() ? BinaryToken::SystemExecName_8
                                                                      : BinaryToken::SystemLitName_8);
                    buf[1] = static_cast<vm_t>(i);
                    len = 2;
                } else {
                    buf[0] = static_cast<vm_t>(object.is_executable() ? BinaryToken::SystemExecName_16
                                                                      : BinaryToken::SystemLitName_16);
                    auto idx = static_cast<uint16_t>(i);
                    std::memcpy(buf + 1, &idx, sizeof(idx));
                    len = 3;
                }
                return from_buf(buf, len);
            }
        }
        trx->error(Error::Undefined, "make_binary_token_string: name not in system name table");
    }

    case Object::Type::String: {
        auto content_len = object.string_length();
        const auto *content = object.string_vptr(trx);
        if (content_len <= 255) {
            // Layout: [token_byte] [length_byte] [content...] [nul]
            auto total = static_cast<length_t>(1 + 1 + content_len);
            auto [ptr, offset] = trx->vm_alloc<vm_t>(total + 1);
            ptr[0] = static_cast<vm_t>(BinaryToken::String_8);
            ptr[1] = static_cast<vm_t>(content_len);
            std::copy_n(content, content_len, ptr + 2);
            ptr[total] = '\0';
            return Object::make_string(offset, total, Object::LiteralAttrib, Object::ReadOnlyAccess);
        } else {
            auto total_size = static_cast<uint32_t>(1) + 2 + content_len;
            if (total_size > MaxStringLength) {
                trx->error(
                        Error::LimitCheck, "make_binary_token_string: binary token string length {} exceeds maximum", total_size);
            }
            auto total = static_cast<length_t>(total_size);
            auto [ptr, offset] = trx->vm_alloc<vm_t>(total + 1);
            ptr[0] = static_cast<vm_t>(BinaryToken::String_16);
            auto len16 = static_cast<uint16_t>(content_len);
            std::memcpy(ptr + 1, &len16, sizeof(len16));
            std::copy_n(content, content_len, ptr + 3);
            ptr[total] = '\0';
            return Object::make_string(offset, total, Object::LiteralAttrib, Object::ReadOnlyAccess);
        }
    }
    }

    std::unreachable();
}
