#!/usr/bin/env python3
# gen_dwarf_be_fixture.py -- emit a BIG-ENDIAN ELF64 with a hand-built DWARF v4
# compilation unit, as a committed regression fixture for lib/dwarf.trx's
# byte-order handling (limit L-7).  No big-endian cross-toolchain is assumed;
# this generator IS the fixture (run by tests/run_dwarf_tests.sh).
#
# It deliberately exercises every byte-order-sensitive path in the reader:
#   - ELF64 header + Elf64_Shdr 8-byte fields (e_shoff, sh_offset, sh_size)
#   - CU header (unit_length u32, version u16, abbrev_offset u32)
#   - forms: strp (4-byte offset), ref4 (4-byte), data1, data4 (struct
#     byte_size -> u32 read), flag_present, exprloc with DW_OP_addr (8-byte u64)
#   - type kinds: base, typedef (-> chase), pointer, enum, struct + members
#
# Layout described (asserted by tests/dwarf_be.trx):
#   struct BeProbe {              // byte_size 40
#     my_u32 uid;   // typedef -> unsigned int   off 0  -> /uinteger-type
#     double amount;//                           off 8  -> /double-type
#     int    count; //                           off 16 -> /integer-type
#     <ptr>  ptr;   // int*                       off 24 -> /ulong-type (pointer)
#     Mode   mode;  // enum (unsigned, 4 bytes)   off 32 -> /uinteger-type
#   };
#   BeProbe g_be;   // DW_OP_addr 0x400140
#
# L-23 big-endian breadth -- the BE decode paths the little-endian matrix covers,
# now exercised big-endian (the bitfield path is the only one that was genuinely
# BE-broken; arrays/nested were BE-correct-but-untested):
#   struct BeBitsM/BeBitsL { unsigned hi:4; int mid:6; unsigned lo:6; } // 2 bytes
#       BeBitsM uses modern DW_AT_data_bit_offset; BeBitsL the legacy MSB-based
#       DW_AT_bit_offset (+ DW_AT_byte_size).  Image 0xDD 0xAA decodes BE as
#       hi=13, mid=-10 (signed), lo=42 for BOTH encodings.
#   int     g_be_arr[3]   = {10, -20, 30}           // array element decode
#   struct  BeOuter { int tag; BeInner{int x; unsigned y;} inner; } g_be_nest
#       = { tag=7, inner={ x=-1, y=255 } }          // recursive nested decode

import struct
import sys

BE = ">"


def uleb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def u8(n):
    return struct.pack(BE + "B", n)


def u16(n):
    return struct.pack(BE + "H", n)


def u32(n):
    return struct.pack(BE + "I", n)


def u64(n):
    return struct.pack(BE + "Q", n)


# DWARF constants
TAG_compile_unit, TAG_base_type, TAG_typedef, TAG_pointer_type = 0x11, 0x24, 0x16, 0x0F
TAG_enumeration_type, TAG_structure_type, TAG_member, TAG_variable = 0x04, 0x13, 0x0D, 0x34
TAG_array_type, TAG_subrange_type, TAG_subprogram = 0x01, 0x21, 0x2E
AT_name, AT_byte_size, AT_encoding, AT_type = 0x03, 0x0B, 0x3E, 0x49
AT_data_member_location, AT_external, AT_location = 0x38, 0x3F, 0x02
AT_bit_size, AT_data_bit_offset, AT_bit_offset, AT_upper_bound = 0x0D, 0x6B, 0x0C, 0x2F
AT_low_pc, AT_specification, AT_declaration = 0x11, 0x47, 0x3C
FORM_data1, FORM_data4, FORM_strp, FORM_ref4 = 0x0B, 0x06, 0x0E, 0x13
FORM_flag_present, FORM_exprloc, FORM_addr = 0x19, 0x18, 0x01
ATE_signed, ATE_float, ATE_unsigned = 0x05, 0x04, 0x07
DW_OP_addr = 0x03
G_ADDR = 0x400140

# g_be's static initializer, laid out big-endian per the BeProbe ABI (doubles
# 8-byte aligned, so members sit at 0/8/16/24/32 with tail/inter padding to 40).
# Lets dwarf-peek decode an end-to-end big-endian typed value (the one decode
# path the little-endian matrix cannot exercise).
G_DATA = (
    u32(0x11223344)               # uid    unsigned int   off 0
    + b"\0\0\0\0"                 #        padding         off 4
    + struct.pack(BE + "d", 6.25)  # amount double          off 8
    + struct.pack(BE + "i", -123456)  # count  int          off 16
    + b"\0\0\0\0"                 #        padding         off 20
    + u64(0)                      # ptr    int*  (null)    off 24
    + u32(2)                      # mode   enum Mode = 2   off 32
    + b"\0\0\0\0"                 #        padding         off 36
)
assert len(G_DATA) == 40

# L-23 globals appended after g_be; each gets VA = G_ADDR + its .data offset.
OFF_BITS_M, OFF_BITS_L, OFF_ARR, OFF_NEST = 40, 42, 44, 56
# 0xDD 0xAA packs big-endian (MSB-first) as hi(4)=1101, mid(6)=110110, lo(6)=101010
# -> hi=13, mid=54 sign-extended over 6 bits = -10, lo=42.  Same image, two encodings.
G_DATA += bytes([0xDD, 0xAA])                   # g_be_bits_m  off 40
G_DATA += bytes([0xDD, 0xAA])                   # g_be_bits_l  off 42
G_DATA += struct.pack(BE + "iii", 10, -20, 30)  # g_be_arr[3]  off 44
G_DATA += struct.pack(BE + "iiI", 7, -1, 255)   # g_be_nest    off 56 (tag, inner.x, inner.y)
OFF_SPEC = 68
G_DATA += struct.pack(BE + "i", 0x12345678)     # g_be_spec    off 68 (out-of-line def)
assert len(G_DATA) == 72

# --- .debug_str ---
dstr = bytearray(b"\0")
_stroff = {}


def s(name):
    if name in _stroff:
        return _stroff[name]
    off = len(dstr)
    _stroff[name] = off
    dstr.extend(name.encode() + b"\0")
    return off


# --- .debug_abbrev ---
abbrev = bytearray()


def abbr(code, tag, children, attrs):
    abbrev.extend(uleb(code) + uleb(tag) + u8(1 if children else 0))
    for at, fm in attrs:
        abbrev.extend(uleb(at) + uleb(fm))
    abbrev.extend(uleb(0) + uleb(0))


abbr(1, TAG_compile_unit, True, [(AT_name, FORM_strp)])
abbr(2, TAG_base_type, False, [(AT_name, FORM_strp), (AT_byte_size, FORM_data1), (AT_encoding, FORM_data1)])
abbr(3, TAG_typedef, False, [(AT_name, FORM_strp), (AT_type, FORM_ref4)])
abbr(4, TAG_pointer_type, False, [(AT_byte_size, FORM_data1), (AT_type, FORM_ref4)])
abbr(5, TAG_enumeration_type, False, [(AT_name, FORM_strp), (AT_byte_size, FORM_data1), (AT_encoding, FORM_data1)])
abbr(6, TAG_structure_type, True, [(AT_name, FORM_strp), (AT_byte_size, FORM_data4)])
abbr(7, TAG_member, False, [(AT_name, FORM_strp), (AT_type, FORM_ref4), (AT_data_member_location, FORM_data1)])
abbr(8, TAG_variable, False,
     [(AT_name, FORM_strp), (AT_type, FORM_ref4), (AT_external, FORM_flag_present), (AT_location, FORM_exprloc)])
# L-23: bitfield members (modern data_bit_offset + legacy bit_offset), array + subrange.
abbr(9, TAG_member, False,
     [(AT_name, FORM_strp), (AT_type, FORM_ref4), (AT_data_bit_offset, FORM_data1), (AT_bit_size, FORM_data1)])
abbr(10, TAG_member, False,
     [(AT_name, FORM_strp), (AT_type, FORM_ref4), (AT_data_member_location, FORM_data1),
      (AT_byte_size, FORM_data1), (AT_bit_offset, FORM_data1), (AT_bit_size, FORM_data1)])
abbr(11, TAG_array_type, True, [(AT_type, FORM_ref4)])
abbr(12, TAG_subrange_type, False, [(AT_upper_bound, FORM_data1)])
# L-23: subprogram (fn lookup, low_pc via 8-byte BE FORM_addr) + split decl/def
# (out-of-line definition reached through DW_AT_specification).
abbr(13, TAG_subprogram, False, [(AT_name, FORM_strp), (AT_low_pc, FORM_addr)])
abbr(14, TAG_variable, False, [(AT_name, FORM_strp), (AT_type, FORM_ref4), (AT_declaration, FORM_flag_present)])
abbr(15, TAG_variable, False, [(AT_specification, FORM_ref4), (AT_location, FORM_exprloc)])
abbrev.extend(uleb(0))  # end of this abbrev table

# --- .debug_info DIEs (32-bit DWARF v4: 11-byte header, DIEs start at +11) ---
HEADER_LEN = 11
dies = bytearray()
off = {}


def at():  # current DIE offset within .debug_info
    return HEADER_LEN + len(dies)


# Emit the compile_unit DIE first so every later DIE's recorded offset is final
# (ref4 references are all backward, so the target offset is known when used).
dies += uleb(1) + u32(s("be.c"))
off["uint"] = at(); dies += uleb(2) + u32(s("unsigned int")) + u8(4) + u8(ATE_unsigned)
off["double"] = at(); dies += uleb(2) + u32(s("double")) + u8(8) + u8(ATE_float)
off["int"] = at(); dies += uleb(2) + u32(s("int")) + u8(4) + u8(ATE_signed)
off["my_u32"] = at(); dies += uleb(3) + u32(s("my_u32")) + u32(off["uint"])
off["ptr"] = at(); dies += uleb(4) + u8(8) + u32(off["int"])
off["enum"] = at(); dies += uleb(5) + u32(s("Mode")) + u8(4) + u8(ATE_unsigned)
off["struct"] = at(); dies += uleb(6) + u32(s("BeProbe")) + u32(40)
off["uid"] = at(); dies += uleb(7) + u32(s("uid")) + u32(off["my_u32"]) + u8(0)
off["amount"] = at(); dies += uleb(7) + u32(s("amount")) + u32(off["double"]) + u8(8)
off["count"] = at(); dies += uleb(7) + u32(s("count")) + u32(off["int"]) + u8(16)
off["mptr"] = at(); dies += uleb(7) + u32(s("ptr")) + u32(off["ptr"]) + u8(24)
off["mode"] = at(); dies += uleb(7) + u32(s("mode")) + u32(off["enum"]) + u8(32)
dies += uleb(0)  # null: end of struct members (depth 2 -> 1)

off["g_be"] = at()
expr = u8(DW_OP_addr) + u64(G_ADDR)
dies += uleb(8) + u32(s("g_be")) + u32(off["struct"]) + uleb(len(expr)) + expr

# --- L-23 bitfield structs (byte_size 2): { unsigned hi:4; int mid:6; unsigned
# --- lo:6 } described twice -- modern DW_AT_data_bit_offset, then legacy
# --- DW_AT_bit_offset (MSB-based, + DW_AT_byte_size).  Same 0xDD 0xAA image. ---
off["bits_m"] = at(); dies += uleb(6) + u32(s("BeBitsM")) + u32(2)
dies += uleb(9) + u32(s("hi"))  + u32(off["uint"]) + u8(0)  + u8(4)   # data_bit_offset 0,  size 4
dies += uleb(9) + u32(s("mid")) + u32(off["int"])  + u8(4)  + u8(6)   # data_bit_offset 4,  size 6 (signed)
dies += uleb(9) + u32(s("lo"))  + u32(off["uint"]) + u8(10) + u8(6)   # data_bit_offset 10, size 6
dies += uleb(0)  # end BeBitsM members

off["bits_l"] = at(); dies += uleb(6) + u32(s("BeBitsL")) + u32(2)
# legacy member: member_offset, byte_size(2), bit_offset (bits left of the field MSB), bit_size
dies += uleb(10) + u32(s("hi"))  + u32(off["uint"]) + u8(0) + u8(2) + u8(0)  + u8(4)
dies += uleb(10) + u32(s("mid")) + u32(off["int"])  + u8(0) + u8(2) + u8(4)  + u8(6)
dies += uleb(10) + u32(s("lo"))  + u32(off["uint"]) + u8(0) + u8(2) + u8(10) + u8(6)
dies += uleb(0)  # end BeBitsL members

# --- L-23 array (int[3]) + nested struct (BeOuter { int tag; BeInner inner; }). ---
off["arr"] = at(); dies += uleb(11) + u32(off["int"])
dies += uleb(12) + u8(2)  # subrange upper_bound 2 -> count 3
dies += uleb(0)  # end array children

off["inner"] = at(); dies += uleb(6) + u32(s("BeInner")) + u32(8)
dies += uleb(7) + u32(s("x")) + u32(off["int"])  + u8(0)
dies += uleb(7) + u32(s("y")) + u32(off["uint"]) + u8(4)
dies += uleb(0)  # end BeInner members

off["outer"] = at(); dies += uleb(6) + u32(s("BeOuter")) + u32(12)
dies += uleb(7) + u32(s("tag"))   + u32(off["int"])   + u8(0)
dies += uleb(7) + u32(s("inner")) + u32(off["inner"]) + u8(4)
dies += uleb(0)  # end BeOuter members

# --- L-8: a subprogram, so dwarf-lookup-fn must read its low_pc (8-byte BE
# --- FORM_addr) correctly. ---
BE_FN_PC = 0x401000
off["fn"] = at(); dies += uleb(13) + u32(s("be_fn")) + u64(BE_FN_PC)

# --- L-5: an out-of-line definition reached via DW_AT_specification (a BE ref4)
# --- back to a non-defining declaration; dwarf-lookup must resolve g_be_spec
# --- (name+type from the decl) to the def's address. ---
off["spec_decl"] = at(); dies += uleb(14) + u32(s("g_be_spec")) + u32(off["int"])  # declaration flag has no data
off["spec_def"] = at()
expr = u8(DW_OP_addr) + u64(G_ADDR + OFF_SPEC)
dies += uleb(15) + u32(off["spec_decl"]) + uleb(len(expr)) + expr


def gvar(name, type_off, addr):  # a DW_TAG_variable with a DW_OP_addr location
    e = u8(DW_OP_addr) + u64(addr)
    return uleb(8) + u32(s(name)) + u32(type_off) + uleb(len(e)) + e


dies += gvar("g_be_bits_m", off["bits_m"], G_ADDR + OFF_BITS_M)
dies += gvar("g_be_bits_l", off["bits_l"], G_ADDR + OFF_BITS_L)
dies += gvar("g_be_arr", off["arr"], G_ADDR + OFF_ARR)
dies += gvar("g_be_nest", off["outer"], G_ADDR + OFF_NEST)
dies += uleb(0)  # null: end of CU children (depth 1 -> 0)

version, abbrev_off, addr_size = 4, 0, 8
body = u16(version) + u32(abbrev_off) + u8(addr_size) + bytes(dies)
debug_info = u32(len(body)) + body  # unit_length precedes the body

# --- .shstrtab ---
shstrtab = bytearray(b"\0")
_shoff = {}


def sh(name):
    o = len(shstrtab)
    _shoff[name] = o
    shstrtab.extend(name.encode() + b"\0")
    return o


for nm in (".debug_info", ".debug_abbrev", ".debug_str", ".shstrtab", ".data"):
    sh(nm)

# --- file layout: ehdr | .debug_info | .debug_abbrev | .debug_str | .shstrtab | .data | shdrs ---
o = 64
di_off = o; o += len(debug_info)
ab_off = o; o += len(abbrev)
ds_off = o; o += len(dstr)
ss_off = o; o += len(shstrtab)
data_off = o; o += len(G_DATA)
sh_off = o

SHT_PROGBITS, SHT_STRTAB = 1, 3
SHF_ALLOC = 2


def shdr(name, typ, offset, size, flags=0, addr=0):
    return (u32(_shoff[name]) + u32(typ) + u64(flags) + u64(addr) + u64(offset) + u64(size)
            + u32(0) + u32(0) + u64(1) + u64(0))


# .data is appended last (section index 5) so .shstrtab stays index 4
# (e_shstrndx).  It carries a real sh_addr/SHF_ALLOC so dwarf-peek's VA ->
# file-image mapping finds g_be's bytes there.
shdrs = b"".join([
    b"\0" * 64,  # SHT_NULL
    shdr(".debug_info", SHT_PROGBITS, di_off, len(debug_info)),
    shdr(".debug_abbrev", SHT_PROGBITS, ab_off, len(abbrev)),
    shdr(".debug_str", SHT_PROGBITS, ds_off, len(dstr)),
    shdr(".shstrtab", SHT_STRTAB, ss_off, len(shstrtab)),
    shdr(".data", SHT_PROGBITS, data_off, len(G_DATA), SHF_ALLOC, G_ADDR),
])

e_ident = bytes([0x7F, 0x45, 0x4C, 0x46, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0])  # ELFCLASS64, ELFDATA2MSB
ehdr = (e_ident + u16(2) + u16(22) + u32(1) + u64(0) + u64(0) + u64(sh_off)
        + u32(0) + u16(64) + u16(0) + u16(0) + u16(64) + u16(6) + u16(4))

blob = ehdr + debug_info + bytes(abbrev) + bytes(dstr) + bytes(shstrtab) + bytes(G_DATA) + shdrs
with open(sys.argv[1], "wb") as f:
    f.write(blob)
