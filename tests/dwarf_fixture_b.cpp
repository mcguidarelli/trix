// tests/dwarf_fixture_b.cpp -- second compilation unit for multi-CU coverage.
//
// Linked with dwarf_fixture.cpp (which owns main).  Its global and struct
// form a distinct CU, so the reader must iterate compilation units AND add
// each unit's base when resolving a CU-relative DW_FORM_ref4 type reference.
// The member types are deliberately disjoint from DwarfProbe's (long / float /
// unsigned long vs unsigned int / double / int / unsigned char) so a cross-CU
// misresolution surfaces as a wrong peek-type rather than passing by accident.
//
// Member layout (SysV ABI):
//   counter  long           DW_ATE_signed (5)     8 bytes  offset 0
//   ratio    float          DW_ATE_float (4)      4 bytes  offset 8
//   mask     unsigned long  DW_ATE_unsigned (7)   8 bytes  offset 16
//   sizeof(OtherProbe) == 24

struct OtherProbe {
    long counter;
    float ratio;
    unsigned long mask;
};

__attribute__((used)) OtherProbe g_other = {123456789L, 1.5f, 0xABCDEF0123456789UL};

// TypedProbe exercises typedef + qualifier + pointer + enum resolution (Chunk
// D).  Members must resolve THROUGH the typedef/const wrappers to the
// underlying scalar:
//   tid     u32_t (typedef unsigned int)  -> /uinteger-type   off 0
//   factor  const double                  -> /double-type     off 8
//   link    int *                         -> /ulong-type (8b) off 16
//   hue     enum Color (unsigned, 4 bytes)-> /uinteger-type   off 24
//   sizeof(TypedProbe) == 32
typedef unsigned int u32_t;
enum Color { RED, GREEN, BLUE };

struct TypedProbe {
    u32_t tid;
    const double factor;
    int *link;
    Color hue;
};

__attribute__((used)) TypedProbe g_typed = {7u, 3.5, nullptr, GREEN};

// AnonHost exercises the L-2 anonymous-member guard.  GCC/clang emit the
// anonymous union as a NAMELESS DW_TAG_member, which used to crash
// dwarf-layout (mem /name md /name get put on a member with no DW_AT_name):
//   tag      int    DW_ATE_signed   off 0
//   <anon>   union                  off 8   (nameless member -> kind /aggregate)
//   trailer  int    DW_ATE_signed   off 16
//   sizeof(AnonHost) == 24
struct AnonHost {
    int tag;
    union {
        long u_long;
        double u_dbl;
    };
    int trailer;
};

__attribute__((used)) AnonHost g_anon = {1, {0}, 9};

// NestHost exercises L-9: a by-value nested struct.  dwarf-layout must expand
// the inner member's type recursively (member carries /layout), and dwarf-peek
// must decode it into a nested Record.
//   tag    unsigned int   off 0
//   inner  Inner          off 8   (double in Inner forces 8-byte alignment)
//   tail   long           off 24
//   sizeof(NestHost) == 32; Inner{ int a off 0; double b off 8 } sizeof 16
struct Inner {
    int a;
    double b;
};

struct NestHost {
    unsigned int tag;
    Inner inner;
    long tail;
};

__attribute__((used)) NestHost g_nested = {
        0xFEEDu, {42, 1.25},
         999L
};

// L-10 arrays: a 1D array global, a struct with an array member, and a 2D array
// (row-major, which the reader flattens to a single Trix array of the product).
//   g_ints  int[5]                                -> [10 20 30 40 50]
//   g_arr   ArrHost{ unsigned int n; double vals[3] } -> Record{ n, vals[] }
//   g_grid  int[2][3]                              -> counts [2 3], flat 6 elems
__attribute__((used)) int g_ints[5] = {10, 20, 30, 40, 50};

struct ArrHost {
    unsigned int n;
    double vals[3];
};

__attribute__((used)) ArrHost g_arr = {
        3u, {1.5, 2.5, 3.5}
};

__attribute__((used)) int g_grid[2][3] = {
        {1, 2, 3},
        {4, 5, 6}
};

// L-6 bitfields: mixed signed/unsigned widths packed into one 32-bit unit.
//   a unsigned int : 3   bits 0..2    -> 5
//   b unsigned int : 5   bits 3..7    -> 17
//   c int          : 10  bits 8..17   -> -100  (signed -> sign-extended on peek)
//   d unsigned int : 14  bits 18..31  -> 9000
struct Flags {
    unsigned int a : 3;
    unsigned int b : 5;
    int c          : 10;
    unsigned int d : 14;
};

__attribute__((used)) Flags g_flags = {5u, 17u, -100, 9000u};

// L-5: an out-of-line static data member definition.  Its variable DIE has a
// location + DW_AT_specification but NO name/type -- both live on the in-class
// declaration, so indexing must follow the specification to resolve it.
struct SpecHost {
    static int s_counter;
};

__attribute__((used)) int SpecHost::s_counter = 12345;

// L-14 namespace/qualified lookup.  GCC/clang split a namespace-scoped global
// into a NAMED declaration inside the namespace (no location) and a LOCATED
// definition at file scope that points back via DW_AT_specification, so the
// reader recovers the qualified name through the spec target.  alpha::ns_dup and
// alpha::beta::ns_dup share the bare name `ns_dup` -- only the qualified form
// disambiguates them; bare `ns_dup` resolves to one (last indexed wins).
namespace alpha {
    __attribute__((used)) int ns_dup = 111;
    __attribute__((used)) long ns_only = 7777L;
    int ns_func(int x);  // declaration in-namespace; defined out-of-line below
    namespace beta {
        __attribute__((used)) int ns_dup = 222;
    }  // namespace beta
}  // namespace alpha

// Out-of-line definition at file scope: DW_TAG_subprogram with DW_AT_low_pc and
// DW_AT_specification -> alpha::ns_func's in-namespace declaration.  Exercises
// the qualified FUNCTION path (dwarf-lookup-fn (alpha::ns_func)).
int alpha::ns_func(int x) {
    return x + 1;
}

// A class member function defined out-of-line: the in-class declaration carries
// the scope, the definition (file scope) points back via DW_AT_specification, so
// dwarf-lookup-fn (QualHost::compute) resolves to the qualified name.
struct QualHost {
    int val;
    long compute(int k);
};

long QualHost::compute(int k) {
    return static_cast<long>(val) + k;
}

__attribute__((used)) QualHost g_qual = {50};
// Reference the member function so its out-of-line definition is emitted/kept.
// Also exercises L-13 pointer-to-member kind (DW_TAG_ptr_to_member_type).
__attribute__((used)) long (QualHost::*g_qual_mp)(int) = &QualHost::compute;

// L-13 C++ reference / rvalue-reference / function-pointer kinds.  A reference is
// a pointer-width address at the machine level, so g_lref stores the address of
// l13_target and peeks as a ULong equal to l13_target's address.
__attribute__((used)) int l13_target = 4242;
__attribute__((used)) int &g_lref = l13_target;
__attribute__((used)) int &&g_rref = 7;
// A function pointer is an ordinary pointer (kind /pointer); its value is the
// callee's entry address, i.e. alpha::ns_func's DW_AT_low_pc.
__attribute__((used)) int (*g_fnptr)(int) = &alpha::ns_func;

// L-4: the other half of the same-name static pair (the first is in
// dwarf_fixture.cpp, value 100).  Distinct CU, distinct address, distinct value.
static int l4_dup __attribute__((used)) = 200;

// L-11 enumerators: dwarf-layout of an enum type carries /enumerators (name ->
// value).  Color (RED/GREEN/BLUE = 0/1/2, unsigned underlying type) is the
// default sequence; Status exercises explicit values incl. a negative one, which
// makes the underlying type signed and emits the const_value as signed sdata.
__attribute__((used)) Color g_color = GREEN;
enum Status { ST_OK = 0, ST_WARN = 5, ST_ERR = -1 };
__attribute__((used)) Status g_status = ST_WARN;

// L-15 scalar widths: a 2-byte short / unsigned short widens to a Trix Integer /
// UInteger (no native 16-bit scalar); __int128 / unsigned __int128 use the 16-byte
// Trix wide types; an 80-bit/128-bit long double has no Trix type (-> /unknown).
// g_i128 sets bit 64 so the decode must read the high half of the 16 bytes.
__attribute__((used)) short g_s16 = -1234;
__attribute__((used)) unsigned short g_u16 = 50000;
__attribute__((used)) __int128 g_i128 = static_cast<__int128>(1) << 64;
__attribute__((used)) unsigned __int128 g_u128 = static_cast<unsigned __int128>(1) << 64;
__attribute__((used)) long double g_ld = 3.5L;
