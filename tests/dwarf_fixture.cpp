// tests/dwarf_fixture.cpp -- regression fixture for lib/dwarf.trx.
//
// Compiled per-run by tests/run_dwarf_tests.sh with debug info.  A single
// global struct of mixed scalar types gives the Trix DWARF reader a small,
// eyeball-able target for name->address and type->layout resolution.  Plain
// base types (no <cstdint> typedefs, no includes) keep every member a direct
// DW_TAG_base_type and the unit a single compilation unit -- matching the
// Chunk-A scope (v4, single CU, no typedef chasing).
//
// Member layout on the x86-64 / AArch64 SysV ABI (8-byte double alignment):
//   id     unsigned int   DW_ATE_unsigned (7)        4 bytes  offset 0
//   scale  double         DW_ATE_float (4)           8 bytes  offset 8
//   flags  int            DW_ATE_signed (5)          4 bytes  offset 16
//   kind   unsigned char  DW_ATE_unsigned_char (8)   1 byte   offset 20
//   sizeof(DwarfProbe) == 24

struct DwarfProbe {
    unsigned int id;
    double scale;
    int flags;
    unsigned char kind;
};

DwarfProbe g_probe = {0x1234u, 2.5, 7, 9};

// L-8: a uniquely-named defined function so dwarf-lookup-fn resolves a
// DW_TAG_subprogram by name to its DW_AT_low_pc (entry address), cross-checked
// against nm.  extern "C" keeps DW_AT_name and the nm symbol identical (no
// mangling); referenced from main so -O0 keeps a real body + low_pc.
extern "C" int dwarf_probe_fn(int x) {
    return (x * 3) + 1;
}

// L-4: a file-local static whose bare name `l4_dup` also exists in the other CU
// (dwarf_fixture_b.cpp, value 200).  Both have a static address, so the index
// must keep BOTH -- dwarf-lookup-all returns the pair, distinguished by /cu-name.
static int l4_dup __attribute__((used)) = 100;

int main() {
    return static_cast<int>(g_probe.id ^ static_cast<unsigned int>(g_probe.flags) ^ g_probe.kind) ^ dwarf_probe_fn(g_probe.flags) ^
           l4_dup;
}
