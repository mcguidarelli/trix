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

//===--- Host introspection primitives (Layer 1) ---===//
//
// Eight POSIX-only primitives that let a Trix program parse the host
// executable's own ELF/DWARF and read its live memory by name.  The ELF and
// DWARF parsing themselves live in Trix (a *.trx library); these ops supply
// only what a stack language cannot do cheaply or safely (dwarf-read-die and
// dwarf-line-lookup, like the rest of the library, are optimizations -- the
// hot opcode loops are fused into C++; see their banners below):
//
//   dwarf-open    path :- base size   mmap a file read-only; push its base
//                                     Address and byte size.
//   dwarf-munmap  base size :-        release a dwarf-open mapping (munmap).
//                                     The Trix dwarf-close wraps it with a ctx.
//   peek-bytes    addr n :- str       copy n (<= MaxStringLength) bytes from a
//                                     host address into a fresh VM string, for
//                                     `unpack` to decode.  The blob read that
//                                     `peek` (fixed scalars only) cannot do.
//   leb128-decode addr signed? :- value next-addr
//                                     decode one ULEB128 (signed? false) or
//                                     SLEB128 (signed? true) varint at addr;
//                                     push the integer and the address one past
//                                     its last byte.  The one varint loop too
//                                     hot to run byte-at-a-time in Trix.
//   module-load-bias -- bias        the value added to a link-time DWARF
//                                     address to get the main executable's
//                                     runtime address (0 for non-PIE, the ASLR
//                                     slide for a PIE).
//   module-load-bias-for path :- bias found?
//                                     the same load bias, but for the loaded
//                                     module whose basename matches `path` (the
//                                     main exe OR a shared object), so a global
//                                     in a .so relocates against ITS base, not
//                                     the exe's.  found? is false when no such
//                                     module is mapped in this process (L-21).
//   dwarf-read-die scope off :- die-dict next-off has-children
//                                     parse one DIE at section offset `off`
//                                     using a per-CU scope array; a fused C++
//                                     port of lib/dwarf.trx's per-DIE parse
//                                     loop, to cut interpreter overhead on the
//                                     millions of ops a real binary needs.
//   dwarf-line-lookup params addr :- row | null
//                                     run one CU's .debug_line program (a fused
//                                     state machine) and return the source row
//                                     (file/line/column) covering link-time PC
//                                     `addr`, or null.  The other hot opcode
//                                     loop, fused like dwarf-read-die (L-22).
//
// Like peek/poke/alloc these are raw-memory ops, so they are sandbox-gated at
// runtime (raise /unsupported under --sandbox).  They are ALSO compile-gated:
// -DTRIX_NO_DWARF drops the bodies (and <sys/mman.h>/<link.h>) to trim binary
// size for hosts that never introspect, leaving registered stubs that raise
// /unsupported -- the same dependency-trim posture as TRIX_NO_ZLIB.

#ifdef TRIX_NO_DWARF

static void dwarf_open_op(Trix *trx) {
    trx->error(Error::Unsupported, "dwarf-open: built without DWARF support (TRIX_NO_DWARF)");
}

static void dwarf_munmap_op(Trix *trx) {
    trx->error(Error::Unsupported, "dwarf-munmap: built without DWARF support (TRIX_NO_DWARF)");
}

static void peek_bytes_op(Trix *trx) {
    trx->error(Error::Unsupported, "peek-bytes: built without DWARF support (TRIX_NO_DWARF)");
}

static void leb128_decode_op(Trix *trx) {
    trx->error(Error::Unsupported, "leb128-decode: built without DWARF support (TRIX_NO_DWARF)");
}

static void module_load_bias_op(Trix *trx) {
    trx->error(Error::Unsupported, "module-load-bias: built without DWARF support (TRIX_NO_DWARF)");
}

static void module_load_bias_for_op(Trix *trx) {
    trx->error(Error::Unsupported, "module-load-bias-for: built without DWARF support (TRIX_NO_DWARF)");
}

static void dwarf_read_die_op(Trix *trx) {
    trx->error(Error::Unsupported, "dwarf-read-die: built without DWARF support (TRIX_NO_DWARF)");
}

static void dwarf_line_lookup_op(Trix *trx) {
    trx->error(Error::Unsupported, "dwarf-line-lookup: built without DWARF support (TRIX_NO_DWARF)");
}

#else

// dwarf-open: path :- base size
// mmap the named file read-only and push its base Address + byte size.
// throws: opstack-underflow, type-check, limit-check, file-open-error, unsupported
static void dwarf_open_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "dwarf-open: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        auto path_ptr = trx->m_op_ptr;
        auto path_cstr = path_ptr->string_cstr(trx);

        auto fd = ::open64(path_cstr, (O_RDONLY | O_LARGEFILE | O_CLOEXEC));
        if (fd < 0) {
            auto saved_errno = errno;
            auto errno_str = errno_string();
            trx->error(Error::FileOpenError, "dwarf-open: open '{}' failed, errno {}/{}", path_cstr, saved_errno, errno_str);
        } else {
            struct stat st{};
            if (::fstat(fd, &st) != 0) {
                auto saved_errno = errno;
                auto errno_str = errno_string();
                ::close(fd);
                trx->error(Error::FileOpenError, "dwarf-open: fstat '{}' failed, errno {}/{}", path_cstr, saved_errno, errno_str);
            } else if (st.st_size <= 0) {
                ::close(fd);
                trx->error(Error::FileOpenError, "dwarf-open: '{}' is empty", path_cstr);
            } else {
                auto size = static_cast<std::size_t>(st.st_size);
                auto base = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (base == MAP_FAILED) {
                    auto saved_errno = errno;
                    auto errno_str = errno_string();
                    ::close(fd);
                    trx->error(
                            Error::FileOpenError, "dwarf-open: mmap '{}' failed, errno {}/{}", path_cstr, saved_errno, errno_str);
                } else {
                    ::close(fd);
                    auto base_obj = Object::make_address(trx, base);
                    auto size_obj = Object::make_ulong(trx, static_cast<ulong_t>(size));
                    path_ptr->maybe_free_extvalue(trx);
                    *path_ptr = base_obj;
                    trx->require_op_capacity(1);
                    *++trx->m_op_ptr = size_obj;
                }
            }
        }
    }
}

// dwarf-munmap: base size :-
// Release an mmap created by dwarf-open (munmap base, size).  The Trix
// dwarf-close wraps this with a context's elf-base / elf-size; after it the
// mapping is gone, so the context must not be read again (reading the unmapped
// region is undefined, like use-after-free).
// throws: opstack-underflow, type-check, file-open-error, unsupported
static void dwarf_munmap_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "dwarf-munmap: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyULong, VerifyAddress);

        auto size_ptr = trx->m_op_ptr;
        auto base_ptr = (size_ptr - 1);

        auto address = base_ptr->address_value(trx);
        auto size = static_cast<std::size_t>(size_ptr->ulong_value(trx));

        if (::munmap(address, size) != 0) {
            auto saved_errno = errno;
            auto errno_str = errno_string();
            trx->error(Error::FileOpenError, "dwarf-munmap: munmap failed, errno {}/{}", saved_errno, errno_str);
        } else {
            base_ptr->maybe_free_extvalue(trx);
            size_ptr->maybe_free_extvalue(trx);
            trx->m_op_ptr -= 2;
        }
    }
}

// peek-bytes: addr n :- str
// Copy n raw bytes from a host address into a fresh read-only VM string.
// throws: opstack-underflow, type-check, range-check, limit-check, unsupported
static void peek_bytes_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "peek-bytes: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyIntegers, VerifyAddress);

        auto n_ptr = trx->m_op_ptr;
        auto addr_ptr = (n_ptr - 1);

        auto [valid, n] = n_ptr->uinteger_value(trx, 0);
        if (!valid) {
            trx->error(Error::RangeCheck, "peek-bytes: byte count must be non-negative");
        } else if (n > MaxStringLength) {
            trx->error(Error::LimitCheck, "peek-bytes: byte count {} exceeds maximum {}", n, +MaxStringLength);
        } else {
            auto address = addr_ptr->address_value(trx);
            // Probe at least one byte even for a zero-length read, so a bad
            // pointer is rejected rather than silently yielding "".
            if (trx->address_state(address, (n == 0) ? 1 : n) <= AddressState::IsNullPtr) {
                trx->error(Error::RangeCheck, "peek-bytes: invalid address");
            } else {
                auto result = Object::make_string(trx, static_cast<const char *>(address), static_cast<length_t>(n));
                n_ptr->maybe_free_extvalue(trx);
                addr_ptr->maybe_free_extvalue(trx);
                *addr_ptr = result;
                trx->m_op_ptr = addr_ptr;
            }
        }
    }
}

// leb128-decode: addr signed? :- value next-addr
// Decode one LEB128 varint at addr.  Reads at most 10 bytes (the ceiling for a
// 64-bit value); callers pass addresses inside a page-aligned mmap, so a short
// over-read past a section end stays within the mapping.
// throws: opstack-underflow, type-check, range-check, unsupported
static void leb128_decode_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "leb128-decode: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyBoolean, VerifyAddress);

        auto flag_ptr = trx->m_op_ptr;
        auto addr_ptr = (flag_ptr - 1);

        auto is_signed = flag_ptr->boolean_value();
        auto address = addr_ptr->address_value(trx);

        if (trx->address_state(address, 1) <= AddressState::IsNullPtr) {
            trx->error(Error::RangeCheck, "leb128-decode: invalid address");
        } else {
            auto bytes = static_cast<const unsigned char *>(address);
            constexpr int MaxLebBytes = 10;
            auto result = uint64_t{0};
            int shift = 0;
            int i = 0;
            auto b = static_cast<unsigned char>(0);
            do {
                b = bytes[i];
                result |= static_cast<uint64_t>(b & 0x7fU) << shift;
                shift += 7;
                ++i;
            } while (((b & 0x80U) != 0) && (i < MaxLebBytes));

            auto value_obj = Object{};
            if (is_signed) {
                auto sval = static_cast<int64_t>(result);
                // Sign-extend when the final byte's sign bit (0x40) is set and
                // bits remain above the value we accumulated.
                if ((shift < 64) && ((b & 0x40U) != 0)) {
                    sval |= -(static_cast<int64_t>(1) << shift);
                }
                value_obj = Object::make_long(trx, static_cast<long_t>(sval));
            } else {
                value_obj = Object::make_ulong(trx, static_cast<ulong_t>(result));
            }
            auto next_obj = Object::make_address(trx, static_cast<void *>(static_cast<char *>(address) + i));

            flag_ptr->maybe_free_extvalue(trx);
            addr_ptr->maybe_free_extvalue(trx);
            *addr_ptr = value_obj;
            *flag_ptr = next_obj;
        }
    }
}

// module-load-bias: -- bias
// Push the main executable's load bias (added to a link-time DWARF address to
// get its runtime address): 0 for a non-PIE ET_EXEC, the ASLR slide for a PIE.
// throws: unsupported
static void module_load_bias_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "module-load-bias: disabled in sandbox mode");
    } else {
        unsigned long bias = 0;
        ::dl_iterate_phdr(
                [](dl_phdr_info *info, std::size_t, void *data) -> int {
                    *static_cast<unsigned long *>(data) = static_cast<unsigned long>(info->dlpi_addr);
                    return 1;
                },
                &bias);
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_ulong(trx, static_cast<ulong_t>(bias));
    }
}

// Basename of a NUL-terminated path: the segment after the last '/', or the
// whole string when it has no slash.  Returns a pointer into `p`.
[[nodiscard]] static const char *dwarf_basename(const char *p) {
    auto base{p};
    for (auto q{p}; (*q != '\0'); ++q) {
        if (*q == '/') {
            base = (q + 1);
        }
    }
    return base;
}

// Search state threaded through dl_iterate_phdr to match a loaded module by
// basename.  The main executable reports an empty dlpi_name, so it is matched
// against exe_base (the basename of /proc/self/exe, precomputed by the caller).
struct DwarfBiasSearch {
    const char *want_base{nullptr};
    const char *exe_base{nullptr};
    unsigned long bias{0};
    bool found{false};
};

// module-load-bias-for: path :- bias found?
// Find the in-process load bias of the module whose basename matches `path`'s
// basename -- the main executable (matched via /proc/self/exe) or any loaded
// shared object.  Pushes the bias (ULong; 0 for a non-PIE main exe) and a found
// flag; found? is false (bias 0) when no loaded module matches, so a caller can
// decline a live read rather than relocate against the wrong base.  This is the
// shared-object generalization of module-load-bias (L-21).
// throws: opstack-underflow, type-check, unsupported
static void module_load_bias_for_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "module-load-bias-for: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        auto path_ptr = trx->m_op_ptr;
        auto path_cstr = path_ptr->string_cstr(trx);

        // The dynamic linker reports the main program with an empty name, so
        // resolve its real path once and match the requested basename against it.
        char exe_path[PATH_MAX];
        auto exe_base = "";
        auto exe_len = ::readlink("/proc/self/exe", exe_path, (sizeof(exe_path) - 1));
        if (exe_len > 0) {
            exe_path[exe_len] = '\0';
            exe_base = dwarf_basename(exe_path);
        }

        DwarfBiasSearch search{};
        search.want_base = dwarf_basename(path_cstr);
        search.exe_base = exe_base;
        ::dl_iterate_phdr(
                [](dl_phdr_info *info, std::size_t, void *data) -> int {
                    auto state = static_cast<DwarfBiasSearch *>(data);
                    auto name = info->dlpi_name;
                    auto entry_base = (((name != nullptr) && (name[0] != '\0')) ? dwarf_basename(name) : state->exe_base);
                    if ((std::strcmp(entry_base, state->want_base) == 0)) {
                        state->bias = static_cast<unsigned long>(info->dlpi_addr);
                        state->found = true;
                        return 1;
                    } else {
                        return 0;
                    }
                },
                &search);

        auto bias_obj = Object::make_ulong(trx, static_cast<ulong_t>(search.bias));
        path_ptr->maybe_free_extvalue(trx);
        *path_ptr = bias_obj;
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_boolean(search.found);
    }
}

// === dwarf-read-die: a fused, in-C++ port of lib/dwarf.trx's per-DIE parser ===
//
// Profiling the Trix reader showed ~64% of a load is interpreter overhead
// (proc-token walking, dispatch, name lookup) from running ~15-20 interpreted
// ops per attribute, millions of times; heap alloc/free is only ~1.4%.  So the
// win is collapsing the per-DIE inner loop -- abbrev lookup + the form-reader
// table + the fixed-width readers + the attr loop -- into one C++ op.  Each
// form's result TYPE mirrors the Trix readers exactly (data1-4/udata/sdata/ref*
// -> Integer, addr/data8/addrx -> ULong, strp/strx/line_strp/string -> String,
// ref_addr/sec_offset -> ULong in 64-bit DWARF else Integer) so the layout /
// lookup logic above it is unchanged.  Everything else (CU iteration, type
// layout, the public API) stays in Trix.

// Per-CU parse scope, read once per op from the caller's scope array (built by
// lib/dwarf.trx dwarf--scope-for).  Index order is part of that contract.
struct DwarfRdScope {
    const unsigned char *di_base{nullptr};      // .debug_info base address
    const unsigned char *ds_base{nullptr};      // .debug_str base (or null)
    const unsigned char *ls_base{nullptr};      // .debug_line_str base (or null)
    const unsigned char *abbrev_base{nullptr};  // this CU's abbrev table
    const unsigned char *so_base{nullptr};      // str-offsets-base, absolute (or null)
    const unsigned char *ad_base{nullptr};      // addr-base, absolute (or null)
    int64_t cu_base{0};                         // CU start section offset (for CU-relative refs)
    int off_size{4};                            // 4 or 8 (32- vs 64-bit DWARF)
    bool big{false};                            // big-endian
};

[[nodiscard]] static uint64_t dwarf_rd_uint_at(const unsigned char *p, int n, bool big) {
    auto v = uint64_t{0};
    if (big) {
        for (int i = 0; i < n; ++i) {
            v = ((v << 8U) | static_cast<uint64_t>(p[i]));
        }
    } else {
        for (int i = 0; i < n; ++i) {
            v |= (static_cast<uint64_t>(p[i]) << static_cast<unsigned>(8 * i));
        }
    }
    return v;
}

[[nodiscard]] static uint64_t dwarf_rd_uleb_at(const unsigned char *&p) {
    auto r = uint64_t{0};
    auto shift{0U};
    auto b{static_cast<unsigned char>(0)};
    do {
        b = *p++;
        r |= (static_cast<uint64_t>(b & 0x7FU) << shift);
        shift += 7U;
    } while (((b & 0x80U) != 0) && (shift < 64U));
    return r;
}

[[nodiscard]] static int64_t dwarf_rd_sleb_at(const unsigned char *&p) {
    auto r = int64_t{0};
    auto shift{0U};
    auto b{static_cast<unsigned char>(0)};
    do {
        b = *p++;
        r |= (static_cast<int64_t>(b & 0x7FU) << shift);
        shift += 7U;
    } while ((b & 0x80U) != 0);
    if ((shift < 64U) && ((b & 0x40U) != 0)) {
        r |= -(static_cast<int64_t>(1) << shift);
    }
    return r;
}

// A NUL-terminated string within the mmap, as a fresh VM String (capped at
// MaxStringLength like any Trix string).
[[nodiscard]] static Object dwarf_cstr_obj(Trix *trx, const unsigned char *p) {
    auto e{p};
    auto limit{p + MaxStringLength};
    while ((e < limit) && (*e != 0)) {
        ++e;
    }
    return Object::make_string(trx, reinterpret_cast<const char *>(p), static_cast<length_t>(e - p));
}

// Decode the leading op of a location/member expression (DW_OP_addr / addrx /
// plus_uconst), mirroring lib/dwarf.trx dwarf--decode-loc and its result types.
[[nodiscard]] static Object dwarf_decode_loc_at(Trix *trx, const DwarfRdScope &s, const unsigned char *d) {
    auto op{*d};
    if (op == 0x03U) {  // DW_OP_addr
        return Object::make_ulong(trx, static_cast<ulong_t>(dwarf_rd_uint_at(d + 1, 8, s.big)));
    } else if (op == 0xA1U) {  // DW_OP_addrx
        auto q{d + 1};
        auto idx{dwarf_rd_uleb_at(q)};
        if (s.ad_base == nullptr) {
            return Object::make_ulong(trx, 0);
        } else {
            return Object::make_ulong(trx, static_cast<ulong_t>(dwarf_rd_uint_at(s.ad_base + (idx * 8U), 8, s.big)));
        }
    } else if (op == 0x23U) {  // DW_OP_plus_uconst
        auto q{d + 1};
        auto v{dwarf_rd_uleb_at(q)};
        return Object::make_integer(static_cast<integer_t>(v));
    } else {
        return Object::make_ulong(trx, 0);
    }
}

// DW_FORM_strx*: index into .debug_str_offsets -> offset into .debug_str.
[[nodiscard]] static Object dwarf_strx_obj(Trix *trx, const DwarfRdScope &s, uint64_t idx) {
    if ((s.so_base == nullptr) || (s.ds_base == nullptr)) {
        return Object::make_string(trx, "", 0);
    } else {
        auto soff{dwarf_rd_uint_at(s.so_base + (idx * static_cast<uint64_t>(s.off_size)), s.off_size, s.big)};
        return dwarf_cstr_obj(trx, s.ds_base + soff);
    }
}

// The set of forms dwarf_read_form handles -- pre-scanned before any allocation
// so an unsupported form errors cleanly (never mid-dict-construction).
[[nodiscard]] static bool dwarf_form_known(uint64_t fm) {
    switch (fm) {
    case 1:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
    case 20:
    case 21:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
    case 30:
    case 31:
    case 33:
    case 34:
    case 35:
    case 37:
    case 38:
    case 39:
    case 40:
    case 41:
    case 42:
    case 43:
    case 44:
        return true;

    default:
        return false;
    }
}

// Read one form at cursor, advance cursor, return the value Object.  Decimal
// form codes match lib/dwarf.trx's dwarf--form-readers table 1:1.  (Form 33,
// implicit_const, is handled by the caller -- its value lives in the abbrev.)
[[nodiscard]] static Object dwarf_read_form(Trix *trx, const DwarfRdScope &s, uint64_t fm, const unsigned char *&p) {
    switch (fm) {
    case 1: {  // addr
        auto v{dwarf_rd_uint_at(p, 8, s.big)};
        p += 8;
        return Object::make_ulong(trx, static_cast<ulong_t>(v));
    }

    case 11: {  // data1
        auto v{dwarf_rd_uint_at(p, 1, s.big)};
        p += 1;
        return Object::make_integer(static_cast<integer_t>(v));
    }

    case 5: {  // data2
        auto v{dwarf_rd_uint_at(p, 2, s.big)};
        p += 2;
        return Object::make_integer(static_cast<integer_t>(v));
    }

    case 6: {  // data4
        auto v{dwarf_rd_uint_at(p, 4, s.big)};
        p += 4;
        return Object::make_integer(static_cast<integer_t>(v));
    }

    case 7: {  // data8
        auto v{dwarf_rd_uint_at(p, 8, s.big)};
        p += 8;
        return Object::make_ulong(trx, static_cast<ulong_t>(v));
    }

    case 14: {  // strp
        auto so{dwarf_rd_uint_at(p, s.off_size, s.big)};
        p += s.off_size;
        return dwarf_cstr_obj(trx, s.ds_base + so);
    }

    case 8: {  // string (inline)
        auto o{dwarf_cstr_obj(trx, p)};
        while (*p != 0) {
            ++p;
        }
        ++p;
        return o;
    }

    case 15: {  // udata
        auto v{dwarf_rd_uleb_at(p)};
        return Object::make_integer(static_cast<integer_t>(v));
    }

    case 13: {  // sdata
        auto v{dwarf_rd_sleb_at(p)};
        return Object::make_integer(static_cast<integer_t>(v));
    }

    case 17: {  // ref1 (CU-relative)
        auto v{dwarf_rd_uint_at(p, 1, s.big)};
        p += 1;
        return Object::make_integer(static_cast<integer_t>(s.cu_base + static_cast<int64_t>(v)));
    }

    case 18: {  // ref2
        auto v{dwarf_rd_uint_at(p, 2, s.big)};
        p += 2;
        return Object::make_integer(static_cast<integer_t>(s.cu_base + static_cast<int64_t>(v)));
    }

    case 19: {  // ref4
        auto v{dwarf_rd_uint_at(p, 4, s.big)};
        p += 4;
        return Object::make_integer(static_cast<integer_t>(s.cu_base + static_cast<int64_t>(v)));
    }

    case 20: {  // ref8
        auto v{dwarf_rd_uint_at(p, 8, s.big)};
        p += 8;
        return Object::make_integer(static_cast<integer_t>(s.cu_base + static_cast<int64_t>(v)));
    }

    case 21: {  // ref_udata
        auto v{dwarf_rd_uleb_at(p)};
        return Object::make_integer(static_cast<integer_t>(s.cu_base + static_cast<int64_t>(v)));
    }

    case 16:    // ref_addr (offset-sized, section-relative)
    case 23: {  // sec_offset
        auto v{dwarf_rd_uint_at(p, s.off_size, s.big)};
        p += s.off_size;
        return ((s.off_size == 8) ? Object::make_ulong(trx, static_cast<ulong_t>(v))
                                  : Object::make_integer(static_cast<integer_t>(v)));
    }

    case 25:  // flag_present (consumes no bytes)
        return Object::make_integer(1);

    case 12: {  // flag
        auto v{dwarf_rd_uint_at(p, 1, s.big)};
        p += 1;
        return Object::make_integer(static_cast<integer_t>(v));
    }

    case 24: {  // exprloc
        auto len{dwarf_rd_uleb_at(p)};
        auto o{dwarf_decode_loc_at(trx, s, p)};
        p += len;
        return o;
    }

    case 10: {  // block1
        auto bl{dwarf_rd_uint_at(p, 1, s.big)};
        auto o{dwarf_decode_loc_at(trx, s, p + 1)};
        p += (1 + bl);
        return o;
    }

    case 3: {  // block2
        auto bl{dwarf_rd_uint_at(p, 2, s.big)};
        auto o{dwarf_decode_loc_at(trx, s, p + 2)};
        p += (2 + bl);
        return o;
    }

    case 4: {  // block4
        auto bl{dwarf_rd_uint_at(p, 4, s.big)};
        auto o{dwarf_decode_loc_at(trx, s, p + 4)};
        p += (4 + bl);
        return o;
    }

    case 9: {  // block (ULEB length)
        auto bl{dwarf_rd_uleb_at(p)};
        auto o{dwarf_decode_loc_at(trx, s, p)};
        p += bl;
        return o;
    }

    case 31: {  // line_strp
        auto o{dwarf_rd_uint_at(p, s.off_size, s.big)};
        p += s.off_size;
        return dwarf_cstr_obj(trx, s.ls_base + o);
    }

    case 30:  // data16 (skipped; mirror Trix's 0 placeholder)
        p += 16;
        return Object::make_integer(0);

    case 26: {  // strx
        auto n{dwarf_rd_uleb_at(p)};
        return dwarf_strx_obj(trx, s, n);
    }

    case 37: {  // strx1
        auto n{dwarf_rd_uint_at(p, 1, s.big)};
        p += 1;
        return dwarf_strx_obj(trx, s, n);
    }

    case 38: {  // strx2
        auto n{dwarf_rd_uint_at(p, 2, s.big)};
        p += 2;
        return dwarf_strx_obj(trx, s, n);
    }

    case 39: {  // strx3
        auto n{dwarf_rd_uint_at(p, 3, s.big)};
        p += 3;
        return dwarf_strx_obj(trx, s, n);
    }

    case 40: {  // strx4
        auto n{dwarf_rd_uint_at(p, 4, s.big)};
        p += 4;
        return dwarf_strx_obj(trx, s, n);
    }

    case 27: {  // addrx
        auto n{dwarf_rd_uleb_at(p)};
        if (s.ad_base == nullptr) {
            return Object::make_ulong(trx, 0);
        } else {
            return Object::make_ulong(trx, static_cast<ulong_t>(dwarf_rd_uint_at(s.ad_base + (n * 8U), 8, s.big)));
        }
    }

    case 41:    // addrx1
    case 42:    // addrx2
    case 43:    // addrx3
    case 44: {  // addrx4
        auto w{(fm == 41) ? 1 : ((fm == 42) ? 2 : ((fm == 43) ? 3 : 4))};
        auto n{dwarf_rd_uint_at(p, w, s.big)};
        p += w;
        if (s.ad_base == nullptr) {
            return Object::make_ulong(trx, 0);
        } else {
            return Object::make_ulong(trx, static_cast<ulong_t>(dwarf_rd_uint_at(s.ad_base + (n * 8U), 8, s.big)));
        }
    }

    case 34:    // loclistx
    case 35: {  // rnglistx
        auto v{dwarf_rd_uleb_at(p)};
        return Object::make_integer(static_cast<integer_t>(v));
    }

    default:  // unreachable: dwarf_form_known() pre-scan rejects unknown forms
        return Object::make_ulong(trx, 0);
    }
}

// Walk a CU's abbrev table for `want`; on a match, capture tag, has-children,
// and a pointer to the (attr,form) list.   returns false if `want` is absent.
[[nodiscard]] static bool dwarf_abbrev_find_cpp(
        const unsigned char *abase, uint64_t want, uint64_t &tag, bool &has_children, const unsigned char *&attr_ptr) {
    auto a{abase};
    while (true) {
        auto code{dwarf_rd_uleb_at(a)};
        if (code == 0) {
            return false;
        } else {
            auto tg{dwarf_rd_uleb_at(a)};
            auto hc{*a++};
            if (code == want) {
                tag = tg;
                has_children = (hc != 0);
                attr_ptr = a;
                return true;
            } else {
                while (true) {  // skip this entry's (attr,form[,implicit_const]) list
                    auto at{dwarf_rd_uleb_at(a)};
                    auto fm{dwarf_rd_uleb_at(a)};
                    if ((at == 0) && (fm == 0)) {
                        break;
                    } else if (fm == 33) {
                        static_cast<void>(dwarf_rd_sleb_at(a));
                    }
                }
            }
        }
    }
}

// The dict key for a captured DW_AT_*; nullptr for attributes read only to
// advance the cursor.  The keys here are exactly the ones lib/dwarf.trx consumes
// (dwarf--index-cu, dwarf--describe-type, dwarf-layout, dwarf-lookup-fn).
[[nodiscard]] static const char *dwarf_attr_key(uint64_t at) {
    switch (at) {
    case 3:
        return "name";

    case 73:
        return "type";

    case 2:
        return "location";

    case 16:
        return "stmt-list";  // DW_AT_stmt_list -- offset of the CU's .debug_line program (L-22)

    case 17:
        return "low-pc";  // DW_AT_low_pc -- a subprogram's entry address (L-8)

    case 11:
        return "byte-size";

    case 56:
        return "member-offset";

    case 47:
        return "upper-bound";  // DW_AT_upper_bound -- array dim (count-1) (L-10)

    case 55:
        return "count";  // DW_AT_count -- array dim, when given directly (L-10)

    case 13:
        return "bit-size";  // DW_AT_bit_size -- bitfield width in bits (L-6)

    case 12:
        return "bit-offset";  // DW_AT_bit_offset -- legacy (DWARF<=3/v4) bitfield (L-6)

    case 107:
        return "data-bit-offset";  // DW_AT_data_bit_offset -- modern bitfield (L-6)

    case 71:
        return "specification";  // DW_AT_specification -- out-of-line def -> decl (L-5)

    case 49:
        return "abstract-origin";  // DW_AT_abstract_origin -- inlined instance -> abstract (L-5)

    case 62:
        return "encoding";

    case 28:
        return "const-value";  // DW_AT_const_value -- an enumerator's value (L-11)

    case 60:
        return "declaration";  // DW_AT_declaration -- a non-defining decl (e.g. a
                               // static data member emitted as DW_TAG_member; L-14)

    case 118:
        return "dwo-name";  // DW_AT_dwo_name -- a DWARF5 split-DWARF skeleton (L-19 guard)

    case 8496:
        return "dwo-name";  // DW_AT_GNU_dwo_name (0x2130) -- pre-v5 GNU split DWARF (L-19 guard)

    case 114:
        return "str-offsets-base-attr";

    case 115:
        return "addr-base-attr";

    default:
        return nullptr;
    }
}

// dwarf-read-die: scope-array off :- die-dict next-off has-children
// Parse one DIE at section offset `off` using the per-CU `scope-array`.  For a
// tree-terminator (abbrev code 0) the die-dict is null.  next-off and
// has-children let the Trix caller drive the document-order walk.
// throws: opstack-underflow, type-check, range-check, invalid-image-file,
//         unsupported
static void dwarf_read_die_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "dwarf-read-die: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyInteger, VerifyArray);

        auto off_ptr{trx->m_op_ptr};
        auto scope_ptr{off_ptr - 1};

        auto [valid, off] = off_ptr->integer_value(trx, 0);
        if (!valid) {
            trx->error(Error::RangeCheck, "dwarf-read-die: offset must be non-negative");
        } else if (scope_ptr->arrays_length() < 9) {
            trx->error(Error::TypeCheck, "dwarf-read-die: scope array must have 9 elements");
        } else {
            auto el{scope_ptr->array_objects(trx)};
            auto s = DwarfRdScope{};
            s.di_base = static_cast<const unsigned char *>(el[0].address_value(trx));
            s.ds_base = el[1].is_null() ? nullptr : static_cast<const unsigned char *>(el[1].address_value(trx));
            s.ls_base = el[2].is_null() ? nullptr : static_cast<const unsigned char *>(el[2].address_value(trx));
            s.big = el[3].boolean_value();
            s.cu_base = static_cast<int64_t>(el[4].integer_value());
            s.off_size = static_cast<int>(el[5].integer_value());
            s.abbrev_base = static_cast<const unsigned char *>(el[6].address_value(trx));
            s.so_base = el[7].is_null() ? nullptr : static_cast<const unsigned char *>(el[7].address_value(trx));
            s.ad_base = el[8].is_null() ? nullptr : static_cast<const unsigned char *>(el[8].address_value(trx));

            auto cur{s.di_base + off};
            auto code{dwarf_rd_uleb_at(cur)};
            if (code == 0) {
                // Tree terminator: null die-dict, cursor advanced past the code byte.
                auto next_off{static_cast<integer_t>(cur - s.di_base)};
                *scope_ptr = Object::make_null();
                *off_ptr = Object::make_integer(next_off);
                trx->require_op_capacity(1);
                *++trx->m_op_ptr = Object::make_integer(0);
            } else {
                auto tag = uint64_t{0};
                auto has_children{false};
                const unsigned char *attr_ptr{nullptr};
                if (!dwarf_abbrev_find_cpp(s.abbrev_base, code, tag, has_children, attr_ptr)) {
                    trx->error(Error::InvalidImageFile, "dwarf-read-die: abbrev code {} not found", code);
                } else {
                    // Pre-scan the form list so an unsupported form errors here,
                    // before the result dict is allocated/rooted.
                    auto bad_form{false};
                    auto bad_fm = uint64_t{0};
                    {
                        auto ap{attr_ptr};
                        while (true) {
                            auto at{dwarf_rd_uleb_at(ap)};
                            auto fm{dwarf_rd_uleb_at(ap)};
                            if ((at == 0) && (fm == 0)) {
                                break;
                            } else if (fm == 33) {
                                static_cast<void>(dwarf_rd_sleb_at(ap));
                            } else if (!dwarf_form_known(fm)) {
                                bad_form = true;
                                bad_fm = fm;
                                break;
                            }
                        }
                    }
                    if (bad_form) {
                        trx->error(Error::Unsupported, "dwarf-read-die: unsupported form {}", bad_fm);
                    } else {
                        auto [die, die_off] = create_result_dict(trx, 12, Object::DictMode::ReadWriteDynamic);
                        trx->gc_root_push_oneoff(Object::make_dict(die_off));
                        die->put(trx, Name::make(trx, std::string_view{"tag"}), Object::make_integer(static_cast<integer_t>(tag)));
                        die->put(trx, Name::make(trx, std::string_view{"offset"}), Object::make_integer(off));
                        auto ap{attr_ptr};
                        while (true) {
                            auto at{dwarf_rd_uleb_at(ap)};
                            auto fm{dwarf_rd_uleb_at(ap)};
                            if ((at == 0) && (fm == 0)) {
                                break;
                            } else {
                                auto val{(fm == 33) ? Object::make_integer(static_cast<integer_t>(dwarf_rd_sleb_at(ap)))
                                                    : dwarf_read_form(trx, s, fm, cur)};
                                auto key{dwarf_attr_key(at)};
                                if (key != nullptr) {
                                    die->put(trx, Name::make(trx, std::string_view{key}), val);
                                } else {
                                    val.maybe_free_extvalue(trx);
                                }
                            }
                        }
                        trx->gc_root_pop_oneoff();
                        auto next_off{static_cast<integer_t>(cur - s.di_base)};
                        *scope_ptr = Object::make_dict(die_off);
                        *off_ptr = Object::make_integer(next_off);
                        trx->require_op_capacity(1);
                        *++trx->m_op_ptr = Object::make_integer(has_children ? 1 : 0);
                    }
                }
            }
        }
    }
}

// === dwarf-line-lookup: a fused .debug_line state machine (PC -> source row) ===
//
// Mirrors dwarf-read-die's posture: the line-number program is an opcode-dense
// loop with LEB128 operands, far too hot to interpret in Trix, so the whole
// header parse + state machine + file-name resolution lives in one C++ op.
// Supports DWARF v2-v5 (including v5's directory/file entry-format descriptors)
// and honors the section byte order.  Given one CU's program (its DW_AT_stmt_list
// offset) and a target link-time address, returns the row whose [address, next)
// range covers the target, or null when no row in this unit does.

// A (content-type, form) pair from a DWARF v5 entry-format descriptor.
struct DwarfLnctPair {
    uint64_t content{0};
    uint64_t form{0};
};

struct DwarfLineScope {
    const unsigned char *dl_base{nullptr};  // .debug_line section base
    uint64_t stmt_off{0};                   // this CU's program offset (DW_AT_stmt_list)
    const unsigned char *ls_base{nullptr};  // .debug_line_str (or null)
    const unsigned char *ds_base{nullptr};  // .debug_str (or null)
    bool big{false};
};

struct DwarfLineHeader {
    int version{0};
    int offset_size{4};
    int address_size{8};
    uint64_t min_inst_length{1};
    uint64_t max_ops{1};
    bool default_is_stmt{true};
    int line_base{0};
    uint64_t line_range{1};
    uint64_t opcode_base{1};
    const unsigned char *std_lengths{nullptr};  // opcode_base-1 bytes
    const unsigned char *program_start{nullptr};
    const unsigned char *unit_end{nullptr};
    const unsigned char *dir_table{nullptr};   // v<=4: cstrings; v5: per dir_format
    const unsigned char *file_table{nullptr};  // v<=4: cstr+3 uleb; v5: per file_format
    uint64_t dir_count{0};                     // v5 only
    uint64_t file_count{0};                    // v5 only
    DwarfLnctPair dir_format[8]{};
    int dir_format_count{0};
    DwarfLnctPair file_format[8]{};
    int file_format_count{0};
};

struct DwarfLineEntry {
    const char *path{nullptr};
    uint64_t dir{0};
};

struct DwarfLineRow {
    uint64_t address{0};
    uint64_t file{1};
    int64_t line{1};
    uint64_t column{0};
    bool is_stmt{false};
    bool found{false};
};

// Advance p past one value of `form` (the forms a line-table entry can carry).
// Returns false on an unsupported form, so the caller bails rather than desync.
[[nodiscard]] static bool dwarf_line_skip_form(int offset_size, uint64_t form, const unsigned char *&p) {
    switch (form) {
    case 0x08:  // DW_FORM_string (inline)
        while (*p != 0) {
            ++p;
        }
        ++p;
        return true;

    case 0x1f:  // DW_FORM_line_strp
    case 0x0e:  // DW_FORM_strp
        p += offset_size;
        return true;

    case 0x0b:  // DW_FORM_data1
        p += 1;
        return true;

    case 0x05:  // DW_FORM_data2
        p += 2;
        return true;

    case 0x06:  // DW_FORM_data4
        p += 4;
        return true;

    case 0x07:  // DW_FORM_data8
        p += 8;
        return true;

    case 0x1e:  // DW_FORM_data16 (MD5)
        p += 16;
        return true;

    case 0x0f:  // DW_FORM_udata
        static_cast<void>(dwarf_rd_uleb_at(p));
        return true;

    default:
        return false;
    }
}

// Read a DW_LNCT_path value as a pointer to its NUL-terminated string within the
// mmap; nullptr for a non-string form.  Advances p past the value.
[[nodiscard]] static const char *
dwarf_line_path_ptr(const DwarfLineScope &s, int offset_size, uint64_t form, const unsigned char *&p) {
    if (form == 0x08) {  // DW_FORM_string (inline)
        auto str{reinterpret_cast<const char *>(p)};
        while (*p != 0) {
            ++p;
        }
        ++p;
        return str;
    } else if (form == 0x1f) {  // DW_FORM_line_strp
        auto off{dwarf_rd_uint_at(p, offset_size, s.big)};
        p += offset_size;
        return ((s.ls_base != nullptr) ? reinterpret_cast<const char *>(s.ls_base + off) : nullptr);
    } else if (form == 0x0e) {  // DW_FORM_strp
        auto off{dwarf_rd_uint_at(p, offset_size, s.big)};
        p += offset_size;
        return ((s.ds_base != nullptr) ? reinterpret_cast<const char *>(s.ds_base + off) : nullptr);
    } else {
        static_cast<void>(dwarf_line_skip_form(offset_size, form, p));
        return nullptr;
    }
}

// Read an integer-valued form (a DW_LNCT_directory_index), advancing p.
[[nodiscard]] static uint64_t dwarf_line_read_uint_form(uint64_t form, const unsigned char *&p, bool big) {
    switch (form) {
    case 0x0b: {  // DW_FORM_data1
        auto v = uint64_t{*p};
        p += 1;
        return v;
    }

    case 0x05: {  // DW_FORM_data2
        auto v{dwarf_rd_uint_at(p, 2, big)};
        p += 2;
        return v;
    }

    case 0x06: {  // DW_FORM_data4
        auto v{dwarf_rd_uint_at(p, 4, big)};
        p += 4;
        return v;
    }

    case 0x07: {  // DW_FORM_data8
        auto v{dwarf_rd_uint_at(p, 8, big)};
        p += 8;
        return v;
    }

    case 0x0f:  // DW_FORM_udata
        return dwarf_rd_uleb_at(p);

    default:
        return 0;
    }
}

// Read one v5 entry (a directory or file) per its format descriptor, capturing
// the path and directory index and skipping everything else.  Advances p.
[[nodiscard]] static DwarfLineEntry dwarf_line_read_v5_entry(
        const DwarfLineScope &s, int offset_size, const DwarfLnctPair *fmt, int fmt_count, const unsigned char *&p) {
    auto e = DwarfLineEntry{};
    for (int i = 0; i < fmt_count; ++i) {
        auto content{fmt[i].content};
        auto form{fmt[i].form};
        if (content == 1) {  // DW_LNCT_path
            e.path = dwarf_line_path_ptr(s, offset_size, form, p);
        } else if (content == 2) {  // DW_LNCT_directory_index
            e.dir = dwarf_line_read_uint_form(form, p, s.big);
        } else {
            static_cast<void>(dwarf_line_skip_form(offset_size, form, p));
        }
    }
    return e;
}

// Parse the line-program unit header (v2-v5).  Returns false on a malformed or
// unsupported header.
[[nodiscard]] static bool dwarf_line_parse_header(const DwarfLineScope &s, DwarfLineHeader *h) {
    auto p{s.dl_base + s.stmt_off};
    auto init{dwarf_rd_uint_at(p, 4, s.big)};
    auto unit_len = uint64_t{0};
    if (init == 0xFFFFFFFFULL) {
        h->offset_size = 8;
        unit_len = dwarf_rd_uint_at(p + 4, 8, s.big);
        p += 12;
    } else {
        h->offset_size = 4;
        unit_len = init;
        p += 4;
    }
    h->unit_end = (p + unit_len);
    h->version = static_cast<int>(dwarf_rd_uint_at(p, 2, s.big));
    p += 2;

    // Single validity flag (no flat early-return guards): a malformed field
    // clears `ok` and the rest of the parse is gated on it.
    auto ok{(h->version >= 2) && (h->version <= 5)};
    if (ok) {
        if (h->version >= 5) {
            h->address_size = static_cast<int>(*p);
            p += 2;  // address_size + segment_selector_size
        }
        auto header_len{dwarf_rd_uint_at(p, h->offset_size, s.big)};
        p += h->offset_size;
        h->program_start = (p + header_len);
        h->min_inst_length = *p;
        p += 1;
        if (h->version >= 4) {
            h->max_ops = *p;
            p += 1;
        }
        if (h->max_ops == 0) {
            h->max_ops = 1;
        }
        h->default_is_stmt = (*p != 0);
        p += 1;
        h->line_base = static_cast<int>(static_cast<signed char>(*p));
        p += 1;
        h->line_range = *p;
        p += 1;
        h->opcode_base = *p;
        p += 1;
        h->std_lengths = p;
        p += ((h->opcode_base > 0) ? (h->opcode_base - 1) : 0);

        if (h->line_range == 0) {
            ok = false;
        } else if (h->version <= 4) {
            h->dir_table = p;
            while (*p != 0) {  // skip include_directories (cstrings, empty-string terminated)
                while (*p != 0) {
                    ++p;
                }
                ++p;
            }
            ++p;  // the terminating empty directory string
            h->file_table = p;
        } else {
            h->dir_format_count = static_cast<int>(*p);
            p += 1;
            if (h->dir_format_count > 8) {
                ok = false;  // more format pairs than the fixed table holds
            } else {
                for (int i = 0; i < h->dir_format_count; ++i) {
                    h->dir_format[i].content = dwarf_rd_uleb_at(p);
                    h->dir_format[i].form = dwarf_rd_uleb_at(p);
                }
                h->dir_count = dwarf_rd_uleb_at(p);
                h->dir_table = p;
                for (uint64_t d = 0; ((d < h->dir_count) && ok); ++d) {
                    for (int i = 0; ((i < h->dir_format_count) && ok); ++i) {
                        if (!dwarf_line_skip_form(h->offset_size, h->dir_format[i].form, p)) {
                            ok = false;
                        }
                    }
                }
                if (ok) {
                    h->file_format_count = static_cast<int>(*p);
                    p += 1;
                    if (h->file_format_count > 8) {
                        ok = false;
                    } else {
                        for (int i = 0; i < h->file_format_count; ++i) {
                            h->file_format[i].content = dwarf_rd_uleb_at(p);
                            h->file_format[i].form = dwarf_rd_uleb_at(p);
                        }
                        h->file_count = dwarf_rd_uleb_at(p);
                        h->file_table = p;
                    }
                }
            }
        }
    }
    return ok;
}

// Join a directory and file name into a VM string (dir + "/" + name), or the
// name alone when it is absolute or no directory is known.
[[nodiscard]] static Object dwarf_line_make_path(Trix *trx, const char *dir, const char *name) {
    if (name == nullptr) {
        return Object::make_null();
    } else {
        auto nlen = std::size_t{0};
        while ((name[nlen] != '\0') && (nlen < MaxStringLength)) {
            ++nlen;
        }
        auto absolute{(nlen > 0) && (name[0] == '/')};
        if (absolute || (dir == nullptr) || (dir[0] == '\0')) {
            return Object::make_string(trx, name, static_cast<length_t>(nlen));
        } else {
            auto dlen = std::size_t{0};
            while ((dir[dlen] != '\0') && (dlen < MaxStringLength)) {
                ++dlen;
            }
            char buf[2 * PATH_MAX];
            auto cap{sizeof(buf)};
            auto pos = std::size_t{0};
            for (std::size_t i = 0; ((i < dlen) && (pos < cap)); ++i) {
                buf[pos++] = dir[i];
            }
            if (pos < cap) {
                buf[pos++] = '/';
            }
            for (std::size_t i = 0; ((i < nlen) && (pos < cap)); ++i) {
                buf[pos++] = name[i];
            }
            if (pos > MaxStringLength) {
                pos = MaxStringLength;
            }
            return Object::make_string(trx, buf, static_cast<length_t>(pos));
        }
    }
}

// Resolve a line-program file index to its (directory-joined) path string.
[[nodiscard]] static Object
dwarf_line_resolve_file(Trix *trx, const DwarfLineScope &s, const DwarfLineHeader *h, uint64_t file_idx) {
    if (h->version <= 4) {
        // file_names is 1-indexed (entry 1 first); index 0 is the primary source.
        if (file_idx == 0) {
            return Object::make_null();
        } else {
            auto p{h->file_table};
            const char *name{nullptr};
            auto dir = uint64_t{0};
            auto found{false};
            for (uint64_t i = 1; !found; ++i) {
                if (*p == 0) {
                    return Object::make_null();  // table terminator before the index
                } else {
                    auto this_name{reinterpret_cast<const char *>(p)};
                    while (*p != 0) {
                        ++p;
                    }
                    ++p;
                    auto d{dwarf_rd_uleb_at(p)};
                    static_cast<void>(dwarf_rd_uleb_at(p));  // mtime
                    static_cast<void>(dwarf_rd_uleb_at(p));  // size
                    if (i == file_idx) {
                        name = this_name;
                        dir = d;
                        found = true;
                    }
                }
            }
            const char *dirstr{nullptr};
            if (dir >= 1) {  // dir 0 == comp_dir (not in the line header); dirs are 1-indexed
                auto dp{h->dir_table};
                auto idx = uint64_t{1};
                auto dfound{false};
                while ((*dp != 0) && !dfound) {
                    if (idx == dir) {
                        dirstr = reinterpret_cast<const char *>(dp);
                        dfound = true;
                    } else {
                        while (*dp != 0) {
                            ++dp;
                        }
                        ++dp;
                        ++idx;
                    }
                }
            }
            return dwarf_line_make_path(trx, dirstr, name);
        }
    } else {
        // v5: file_count entries, 0-indexed, each per file_format.
        if (file_idx >= h->file_count) {
            return Object::make_null();
        } else {
            auto p{h->file_table};
            auto entry = DwarfLineEntry{};
            for (uint64_t i = 0; i <= file_idx; ++i) {
                entry = dwarf_line_read_v5_entry(s, h->offset_size, h->file_format, h->file_format_count, p);
            }
            const char *dirstr{nullptr};
            if (entry.dir < h->dir_count) {
                auto dp{h->dir_table};
                auto dentry = DwarfLineEntry{};
                for (uint64_t i = 0; i <= entry.dir; ++i) {
                    dentry = dwarf_line_read_v5_entry(s, h->offset_size, h->dir_format, h->dir_format_count, dp);
                }
                dirstr = dentry.path;
            }
            return dwarf_line_make_path(trx, dirstr, entry.path);
        }
    }
}

// Run the line-number program, returning the row whose [address, next) range
// covers `target` (the row with the largest address <= target within a
// sequence whose following row exceeds target).  O(program), O(1) memory.
[[nodiscard]] static DwarfLineRow dwarf_line_run(const DwarfLineScope &s, const DwarfLineHeader *h, uint64_t target) {
    auto p{h->program_start};
    auto end{h->unit_end};
    auto address = uint64_t{0};
    auto op_index = uint64_t{0};
    auto file = uint64_t{1};
    auto line = int64_t{1};
    auto column = uint64_t{0};
    auto is_stmt{h->default_is_stmt};

    auto have_prev{false};
    auto prev = DwarfLineRow{};
    auto result = DwarfLineRow{};

    // Emit the current row: if the previous row's [prev.address, address) range
    // covers target, that prev row is the answer (return true to stop).  An
    // end_sequence row bounds the prior row, then ends the sequence.
    auto emit = [&](bool end_seq) -> bool {
        auto hit{have_prev && (prev.address <= target) && (target < address)};
        if (hit) {
            result.address = prev.address;
            result.file = prev.file;
            result.line = prev.line;
            result.column = prev.column;
            result.is_stmt = prev.is_stmt;
            result.found = true;
            return true;
        } else {
            if (end_seq) {
                have_prev = false;
            } else {
                prev.address = address;
                prev.file = file;
                prev.line = line;
                prev.column = column;
                prev.is_stmt = is_stmt;
                have_prev = true;
            }
            return false;
        }
    };

    auto stop{false};
    while ((p < end) && !stop) {
        auto opcode = uint64_t{*p};
        ++p;
        if (opcode >= h->opcode_base) {
            // Special opcode.
            auto adjusted{opcode - h->opcode_base};
            auto op_advance{adjusted / h->line_range};
            address += (h->min_inst_length * ((op_index + op_advance) / h->max_ops));
            op_index = ((op_index + op_advance) % h->max_ops);
            line += (h->line_base + static_cast<int64_t>(adjusted % h->line_range));
            stop = emit(false);
        } else if (opcode == 0) {
            // Extended opcode: ULEB length, then a sub-opcode + operands.
            auto inst_len{dwarf_rd_uleb_at(p)};
            auto ext_start{p};
            auto sub = uint64_t{0};
            if (inst_len > 0) {
                sub = *p;
            }
            if (sub == 1) {  // DW_LNE_end_sequence
                stop = emit(true);
                address = 0;
                op_index = 0;
                file = 1;
                line = 1;
                column = 0;
                is_stmt = h->default_is_stmt;
            } else if (sub == 2) {  // DW_LNE_set_address
                address = dwarf_rd_uint_at(p + 1, h->address_size, s.big);
                op_index = 0;
            }
            p = (ext_start + inst_len);
        } else {
            // Standard opcode.
            switch (opcode) {
            case 1:  // DW_LNS_copy
                stop = emit(false);
                break;

            case 2: {  // DW_LNS_advance_pc
                auto adv{dwarf_rd_uleb_at(p)};
                address += (h->min_inst_length * ((op_index + adv) / h->max_ops));
                op_index = ((op_index + adv) % h->max_ops);
                break;
            }

            case 3:  // DW_LNS_advance_line
                line += dwarf_rd_sleb_at(p);
                break;

            case 4:  // DW_LNS_set_file
                file = dwarf_rd_uleb_at(p);
                break;

            case 5:  // DW_LNS_set_column
                column = dwarf_rd_uleb_at(p);
                break;

            case 6:  // DW_LNS_negate_stmt
                is_stmt = !is_stmt;
                break;

            case 7:  // DW_LNS_set_basic_block
                break;

            case 8: {  // DW_LNS_const_add_pc (== special opcode 255's advance)
                auto adjusted{255 - h->opcode_base};
                auto adv{adjusted / h->line_range};
                address += (h->min_inst_length * ((op_index + adv) / h->max_ops));
                op_index = ((op_index + adv) % h->max_ops);
                break;
            }

            case 9:  // DW_LNS_fixed_advance_pc (fixed uhalf operand)
                address += dwarf_rd_uint_at(p, 2, s.big);
                p += 2;
                op_index = 0;
                break;

            case 10:  // DW_LNS_set_prologue_end
            case 11:  // DW_LNS_set_epilogue_begin
                break;

            case 12:  // DW_LNS_set_isa
                static_cast<void>(dwarf_rd_uleb_at(p));
                break;

            default: {
                // Unknown standard opcode: consume its declared ULEB operands.
                auto n{static_cast<unsigned>(h->std_lengths[opcode - 1])};
                for (unsigned k = 0; k < n; ++k) {
                    static_cast<void>(dwarf_rd_uleb_at(p));
                }
                break;
            }
            }
        }
    }
    return result;
}

// dwarf-line-lookup: params target-addr :- row-dict | null
// params is [ dl-base stmt-off ls-base ds-base big? ]; target-addr is the
// link-time PC.  Returns << /address /file /line /column /is-stmt >> for the row
// covering target in this CU's line program, or null.
// throws: opstack-underflow, type-check, unsupported
static void dwarf_line_lookup_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "dwarf-line-lookup: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyULong, VerifyArray);

        auto target_ptr{trx->m_op_ptr};
        auto params_ptr{target_ptr - 1};
        auto target{target_ptr->ulong_value(trx)};

        if (params_ptr->arrays_length() < 5) {
            trx->error(Error::TypeCheck, "dwarf-line-lookup: params array must have 5 elements");
        } else {
            auto el{params_ptr->array_objects(trx)};
            auto s = DwarfLineScope{};
            s.dl_base = static_cast<const unsigned char *>(el[0].address_value(trx));
            s.stmt_off =
                    static_cast<uint64_t>(el[1].is_ulong() ? el[1].ulong_value(trx) : static_cast<ulong_t>(el[1].integer_value()));
            s.ls_base = el[2].is_null() ? nullptr : static_cast<const unsigned char *>(el[2].address_value(trx));
            s.ds_base = el[3].is_null() ? nullptr : static_cast<const unsigned char *>(el[3].address_value(trx));
            s.big = el[4].boolean_value();

            auto h = DwarfLineHeader{};
            auto result = Object::make_null();
            if (dwarf_line_parse_header(s, &h)) {
                auto row{dwarf_line_run(s, &h, target)};
                if (row.found) {
                    auto path{dwarf_line_resolve_file(trx, s, &h, row.file)};
                    auto [d, d_off] = create_result_dict(trx, 6, Object::DictMode::ReadWriteDynamic);
                    trx->gc_root_push_oneoff(Object::make_dict(d_off));
                    d->put(trx,
                           Name::make(trx, std::string_view{"address"}),
                           Object::make_ulong(trx, static_cast<ulong_t>(row.address)));
                    d->put(trx, Name::make(trx, std::string_view{"file"}), path);
                    d->put(trx, Name::make(trx, std::string_view{"line"}), Object::make_integer(static_cast<integer_t>(row.line)));
                    d->put(trx,
                           Name::make(trx, std::string_view{"column"}),
                           Object::make_integer(static_cast<integer_t>(row.column)));
                    d->put(trx, Name::make(trx, std::string_view{"is-stmt"}), Object::make_boolean(row.is_stmt));
                    trx->gc_root_pop_oneoff();
                    result = Object::make_dict(d_off);
                }
            }

            target_ptr->maybe_free_extvalue(trx);
            params_ptr->maybe_free_extvalue(trx);
            *params_ptr = result;
            --trx->m_op_ptr;
        }
    }
}

#endif  // TRIX_NO_DWARF
