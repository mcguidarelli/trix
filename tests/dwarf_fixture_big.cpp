// tests/dwarf_fixture_big.cpp -- a deliberately DIE-heavy translation unit.
//
// Instantiating a few STL templates pulls thousands of DIEs into .debug_info
// (typically ~7-8K), well past the old reader's 4096-element scratch ceiling.
// The lazy reader never materializes the whole DIE tree, so it parses this;
// the old scratch-based reader could not (Wave 0, limit L-1).  g_big is a
// plain struct so its layout is still cheaply assertable after the heavy walk.
//
//   BigProbe { unsigned int id; double scale; int flags; }
//     id     DW_ATE_unsigned (7)  4 bytes  offset 0
//     scale  DW_ATE_float (4)     8 bytes  offset 8
//     flags  DW_ATE_signed (5)    4 bytes  offset 16
//     sizeof(BigProbe) == 24 (8-aligned by the double)

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct BigProbe {
    unsigned int id;
    double scale;
    int flags;
};

// Force the heavy templates to be emitted with debug info.
std::map<std::string, std::vector<int>> g_m1;
std::unordered_map<std::string, std::shared_ptr<int>> g_m2;
std::vector<std::map<int, std::string>> g_v1;

__attribute__((used)) BigProbe g_big = {0xCAFE, 2.5, 7};

int main() {
    return static_cast<int>(g_m1.size() + g_m2.size() + g_v1.size()) + g_big.flags;
}
