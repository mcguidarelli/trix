// tests/dwarf_l21_probe.cpp -- L-21 shared-object live-peek fixture.
//
// Built into a small shared library (libdwarf_l21_probe.so, -g -fPIC -shared)
// that tests/run_dwarf_tests.sh LD_PRELOADs into the live ./trix process.  The
// trix script tests/dwarf_so_peek.trx then loads the .so's OWN DWARF by path
// and reads these globals from live process memory via dwarf-peek-live, proving
// that the link-time DWARF address is relocated by THIS .so's load bias (not
// the main executable's) -- the L-21 fix for module-load-bias-for.
//
// Globals carry known initializers so the live read can be cross-checked
// against the static (file-image) read of the same symbol.  [[gnu::used,
// gnu::retain]] keeps them past the optimizer and a --gc-sections link, and
// extern "C" keeps the names unmangled so dwarf-lookup finds them by source name.

struct L21Probe {
    unsigned int magic;
    double scale;
    int count;
    unsigned char tag;
};

extern "C" {
[[gnu::used, gnu::retain]] L21Probe g_l21_probe = {0xBADF00Du, 2.5, -7, 9};
[[gnu::used, gnu::retain]] int g_l21_scalar = 314159;
}
