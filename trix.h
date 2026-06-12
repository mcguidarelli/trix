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

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cfenv>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iterator>
#include <numeric>
#include <print>
#include <source_location>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <locale.h>
#include <poll.h>
#ifndef TRIX_NO_READLINE
#include <readline/history.h>
#include <readline/readline.h>
#endif
#include <regex.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifndef TRIX_NO_ZLIB
#include <zlib.h>
#endif

// ============================================================================
// Class / Namespace Hierarchy
// ============================================================================
//
// All types live in namespace trix_details.
//
//  namespace trix_details
//  |-- operator+(T)          unary + extracts enum underlying value
//  |-- Trix                  *** (intentional and unapologetic) GOD CLASS ***
//  |   |-- PCG32             32-bit Permuted Congruential Generator (O'Neill)
//  |   |-- DebugState        interactive debugger state holder
//  |   |-- Object            8-byte tagged value; every Trix value is an Object
//  |   |-- Name              interned string in VM; literal or executable
//  |   |-- Number            numeric type dispatch helper
//  |   |-- Dict              hash-table mapping Name->Object (fixed or dynamic)
//  |   |-- Stream            I/O abstraction: file, memory, string, stdio
//  |   |-- Save              save/restore transaction checkpoint
//  |   |-- SnapShotHeader    .img file header: magic, version, counts, CRCs
//  |   |-- RestoredMemStream carries a restored memory-stream buffer into thaw
//  |   |-- ReconnectedStream carries a pre-opened fd for user file reconnect
//  |   |-- PrintFmt          format-string engine ({:d} {:f} {:O} ...)
//  |   `-- ScanFmt           format-string scanner (sscanf-style)
//  |
//  Convenience aliases for the Trix inner classes are re-exported at namespace
//  scope after class Trix closes:
//    trix_details::Object, Name, Number, Dict, Stream, Save, PrintFmt, ScanFmt
//
// Note on the God-class pattern
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Trix is deliberately monolithic.  The VM heap, all four stacks (operand,
// execution, dictionary, error), the name table, the stream list, the
// save/restore journal, and every operator implementation must share the same
// raw heap pointer and co-ordinate on allocation, save barriers, and error
// dispatch.  Decomposing them into independent objects would require either
// global state or a dense web of cross-references -- both worse than a single
// coherent class.
//
// The inner classes (Object, Dict, Stream, ...) provide logical grouping and
// encapsulation; they receive a Trix * on every call that touches the heap and
// have no independent lifetime.  The physical file split (trix/*.inl) is for
// developer ergonomics only -- all fragments are #include'd inside the single
// class body and compiled as one translation unit.
// ============================================================================

namespace trix_details {
    template<typename T>
    requires std::is_enum_v<T>
    constexpr auto operator+(T e) {
        return std::to_underlying(e);
    }

    // Compile-time feature flags.  Lives at namespace scope so the
    // `static constexpr bool` flags are accessible inside the class body
    // for `if constexpr (...)`, and the `TRIX_<NAME>(...)` macros are
    // available at all declaration sites.  See src/build_config.inl.
#include "src/build_config.inl"

    class Trix {
    public:
        // clang-format off

        //===--- Include Ordering ---===//
        //
        // All 63 .inl files are #include'd inside the class Trix {} body
        // and compiled as one translation unit.  The order is load-bearing:
        //
        // 1. TYPES (types.inl): type aliases, constants, Config.
        //    Must be first -- everything else uses these types.
        //
        // 2. FOUNDATION (pcg32, enums, scanner_tables, vm_heap):
        //    Enums (SystemName, Error, Lexeme), lookup tables, heap
        //    allocation primitives, hash functions.  No dependencies on
        //    inner classes.
        //
        // 3. INNER CLASSES (object, verify, name, number, dict, stream,
        //    save, snapshot, printfmt, scanfmt):
        //    The core data types.  Order matters: Object is used by all
        //    others; Name/Dict/Stream/Save reference each other but use
        //    forward declarations (class Dict; etc. at top of object.inl).
        //    PrintFmt/ScanFmt depend on Object and Stream.
        //
        // 4. RUNTIME + INTERPRETER (runtime, interpreter):
        //    The execution engine.  Depends on all inner classes.
        //
        // 5. SHIMS (shims.inl):
        //    Type-dispatch functions for binary operators.  Depends on
        //    Object type accessors and ExtValue management.
        //
        // 6. OPERATORS (ops_*.inl, 36 files):
        //    Static member functions implementing all 1015 operators
        //    (856 user-facing).
        //    Order within this group is flexible -- all inner classes and
        //    the interpreter are already defined.  Grouped by category.
        //
        // 7. DISPATCH (dispatch.inl):
        //    Consteval-built tables mapping SystemName/WellKnownName to
        //    Operator{function, name}.  Must follow all ops_*.inl files
        //    so all operator functions are declared.
        //
        // 8. PUBLIC API + INIT (api.inl, init.inl):
        //    Host-facing API (parse_args, error, raise_interrupt, invoke)
        //    and constructors/destructor.  Init depends on dispatch (for
        //    systemdict population) and all operators.
        //
        // 9. MEMBER VARIABLES (member_vars.inl):
        //    Must be last -- member variables are declared after all
        //    member functions, matching C++ class body conventions.
        //
        // DO NOT reorder without verifying compilation.  The deferred
        // parsing of member function bodies inside a class definition
        // (C++ standard) means function bodies can reference later-
        // declared members, but parameter types and base classes must
        // be resolvable at the point of declaration.

        // Universal types (must be first)
#include "src/types.inl"

        // Foundation
#include "src/pcg32.inl"
#include "src/enums.inl"
#include "src/op_descriptor.inl"
#include "src/scanner_tables.inl"
#include "src/hash.inl"
#include "src/vm_heap.inl"
#include "src/gvm_heap.inl"

        // Inner classes
#include "src/object.inl"
#include "src/verify.inl"
#include "src/name.inl"
#include "src/binding_table.inl"
#include "src/number.inl"
#include "src/dict.inl"
#include "src/stream.inl"
#include "src/save.inl"
#include "src/snapshot.inl"
#include "src/chrono.inl"
#include "src/printfmt.inl"
#include "src/scanfmt.inl"

        // Runtime and interpreter
#include "src/runtime.inl"
#include "src/interpreter.inl"

        // Operator infrastructure
#include "src/shims.inl"

        // Operators
#include "src/ops_stack.inl"
#include "src/ops_array.inl"
#include "src/ops_array_iteration.inl"
#include "src/ops_string.inl"
#include "src/ops_regex.inl"
#include "src/ops_flow.inl"
#include "src/ops_stream_io.inl"
#include "src/ops_screen.inl"
#include "src/ops_math.inl"
#include "src/ops_bitwise.inl"
#include "src/ops_set.inl"
#include "src/ops_tagged.inl"
#include "src/ops_record.inl"
#include "src/ops_lazy.inl"
#include "src/ops_dict.inl"
#include "src/ops_convert.inl"
#include "src/ops_format.inl"
#include "src/ops_memory.inl"
#include "src/ops_higher.inl"
#include "src/ops_system.inl"
#include "src/ops_chrono.inl"
#include "src/ops_coroutine.inl"
#include "src/ops_scratch.inl"
#include "src/ops_pipeline.inl"
#include "src/ops_actor.inl"
#include "src/ops_supervision.inl"
#include "src/ops_logic.inl"
#include "src/ops_reactive.inl"
#include "src/ops_snapshot.inl"
#include "src/ops_protocol.inl"
#include "src/ops_match.inl"
#include "src/ops_transducer.inl"
#include "src/ops_genserver.inl"
#include "src/ops_continuation.inl"
#include "src/ops_effect.inl"

        // Mark-sweep GC for the global VM.  Must come AFTER
        // every composite struct's owning .inl file (per-subsystem
        // walkers reach into them) and BEFORE dispatch.inl (the op
        // names point here).
#include "src/gc.inl"

        // Dispatch and initialization
#include "src/dispatch.inl"

        // Public API and initialization
#include "src/api.inl"
#include "src/init.inl"

        // Member variables (must be last)
#include "src/member_vars.inl"

        // clang-format on
    };

    // Scanner character-class tables are verified at COMPILE time: any drift
    // between the sm_chattr bit table and the expected-character lists in
    // scanner_tables.inl fails every build (was a debug-startup assert in
    // init.inl).  See verify_chattr() for the per-table bisection notes.
    static_assert(Trix::verify_chattr(), "scanner_tables.inl: sm_chattr bit table drifted from the expected character-class lists");

    // The Number scanner's per-character hint table is verified the same way:
    // every sm_hint_data entry must agree with the character-class predicates
    // (was the adjacent debug-startup assert in init.inl, which left release
    // builds unverified).
    static_assert(Trix::Number::verify_hint_table(), "number.inl: sm_hint_data drifted from the character-class predicates");

    // The three dispatch tables (sysvariable / wellknown / sysoperator) are
    // built consteval from the row lists in dispatch.inl; this proves every
    // enum index is covered exactly once in this build configuration -- a
    // missing, duplicated, or empty row fails every build here.
    static_assert(Trix::verify_dispatch_tables(),
                  "dispatch.inl: dispatch row lists drifted from the SystemName/WellKnownName enums");

    // Convenience aliases for inner classes
    using Object = Trix::Object;
    using Name = Trix::Name;
    using Number = Trix::Number;
    using Dict = Trix::Dict;
    using Stream = Trix::Stream;
    using Save = Trix::Save;
    using PrintFmt = Trix::PrintFmt;
    using ScanFmt = Trix::ScanFmt;

}  // namespace trix_details

using Trix = trix_details::Trix;
