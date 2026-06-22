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

//===--- Compile-time Feature Flags ---===//
//
// Single source of truth for which optional features are compiled into the
// build.  Each feature provides a `static constexpr bool` flag for use in
// `if constexpr (...)` body code -- the preferred form, since it compiles in
// both builds with no preprocessor.  A feature that must also gate
// declaration-level sites `if constexpr` cannot reach (member fields, enum
// entries, dispatch switch cases, init-list strings, bodies referencing a
// symbol absent from the gated-off build) uses plain `#ifdef TRIX_<NAME>`
// blocks; a feature consulted only from `if constexpr` (e.g. BacktraceEnabled)
// has just the flag.  There are NO function-like gate macros: the former
// `TRIX_DBG(...)` / `TRIX_HEAP_TRACK(...)` wrappers were retired because a
// macro argument cannot be clang-formatted (it mangled switch-case labels and
// forced variadic-comma workarounds) -- use `#ifdef` directly instead.
//
// The preprocessor is allowed in Trix ONLY for these feature gates
// (`TRIX_DEBUGGER`, `TRIX_HEAP_TRACKING`, `TRIX_NO_ZLIB`, `TRIX_NO_READLINE`,
// `TRIX_NO_BACKTRACE`) plus `#include` / `#pragma`.  See STYLE.md
// ("Language Restrictions") for the full rule and rationale.
//
// To toggle a flag for a build, define or undefine the corresponding
// `TRIX_<NAME>` preprocessor symbol when invoking ./build.sh:
//   ./build.sh                  -- default: TRIX_HEAP_TRACKING defined (dev build)
//   ./build.sh --release        -- TRIX_HEAP_TRACKING undefined (release build)
//
// Adding a new feature flag here is the entry point for any new compile-
// time configuration; resist sprinkling fresh `#ifdef`s elsewhere.
// ============================================================================

// ---- vm-heap-tracking (alloc-stats + per-block side-table) ----
// Diagnostic-only: tracks every VM allocation and exposes the alloc-stats /
// clear-alloc-stats / vm-heap-snapshot / vm-heap-diff ops + heap-track /
// alloc status keys.
// Costs ~256 KB of VM heap (the side-table) plus a hot-path branch per
// vm_alloc, so release builds disable it entirely.
#ifdef TRIX_HEAP_TRACKING
static constexpr bool HeapTracking = true;
#else
static constexpr bool HeapTracking = false;
#endif

// ---- interactive debugger (debug op + step/continue REPL) ----
// Diagnostic-only: the `debug` operator drops into a readline-driven step/
// continue prompt that single-steps the dispatch loop.  Costs one branch
// per dispatch iteration (m_debug_active fast-path guard, marked
// [[unlikely]]) plus the DebugState struct (Mode + small ints/offsets,
// several GC-root Objects for the event callback and break-on-error
// stash, and an inline MaxErrorLength+1 byte buffer) and the
// m_debug_proc_lines per-op source-line map (the sole std::unordered_map/
// std::vector heap-STL carve-out)
// in every Trix instance, plus ~200 LOC of debug_op / debug_hook /
// debug_describe.  Release builds disable it entirely: the debugger ops,
// member vars, GC hooks, and CLI flags (`-d`/`--debug`, `--inspect`/
// `--inspect-on-error`/`--inspect-at`, `--no-color`) all compile out, so a
// debugger-off build rejects those flags as unknown options rather than
// accepting them as silent no-ops.
#ifdef TRIX_DEBUGGER
static constexpr bool Debugger = true;
#else
static constexpr bool Debugger = false;
#endif

// ---- fatal-error backtrace dump (format_backtrace + state header) ----
// Default ON.  Set -DTRIX_NO_BACKTRACE to compile out the format_backtrace
// body for minimum-footprint embedded hosts that accept "no diagnostic on
// fatal" as a tradeoff.  Cost when ON: ~600 LOC of operand/exec-stack
// rendering + the type-by-type switch in format_backtrace itself.  Runtime
// cost in normal (non-fatal) execution is zero -- format_backtrace only
// runs from global_handler.  Runtime cost in fatal+quiet mode: one branch
// (the existing if (m_quiet) early-out is preserved either way).
#ifdef TRIX_NO_BACKTRACE
static constexpr bool BacktraceEnabled = false;
#else
static constexpr bool BacktraceEnabled = true;
#endif

// ---- zlib deflate/inflate compression ----
// Default ON.  Set -DTRIX_NO_ZLIB to drop the zlib dependency for a smaller
// embed: the six deflate/inflate operators (deflate, deflate-level,
// deflate-stream, deflate-stream-level, inflate, inflate-stream) stay
// registered but raise /unsupported, and <zlib.h> is not included.  crc32 /
// adler32 (+ their -stream variants) are hand-rolled and keep working;
// snapshots do not use zlib and are unaffected.  This is a dependency-trim
// gate, not a diagnostic one -- the operator surface is preserved (callers
// catch /unsupported) rather than compiled away.
#ifdef TRIX_NO_ZLIB
static constexpr bool ZlibEnabled = false;
#else
static constexpr bool ZlibEnabled = true;
#endif

// ---- GNU readline interactive line editor (stdedit REPL) ----
// Default ON.  Set -DTRIX_NO_READLINE to drop the libreadline dependency:
// the interactive `Trix>` REPL degrades to a plain prompt (a stdin getline
// loop -- no line editing, no history) and <readline/*.h> is not included.
// Script execution and the `--stdin` piped path are unaffected, and the
// `read-line` operator (which reads a line from any stream) is unrelated to
// libreadline.  Like ZlibEnabled this is a dependency-trim gate, not a
// diagnostic one -- the interactive REPL still works, just unadorned.
#ifdef TRIX_NO_READLINE
static constexpr bool ReadlineEnabled = false;
#else
static constexpr bool ReadlineEnabled = true;
#endif

// ---- ELF/DWARF host-introspection primitives ----
// Default ON.  Set -DTRIX_NO_DWARF to drop the three introspection operators
// (dwarf-open, peek-bytes, leb128-decode) and the <sys/mman.h> include: the ops
// stay registered but raise /unsupported, trimming their parser bodies from the
// binary for hosts that never inspect their own memory.  Like ZlibEnabled this
// is a dependency/footprint-trim gate, not a diagnostic one -- the operator
// surface is preserved (callers catch /unsupported) rather than compiled away.
// The ops are ALSO sandbox-gated at runtime (the peek/poke posture), which is
// independent of this compile flag.
#ifdef TRIX_NO_DWARF
static constexpr bool DwarfEnabled = false;
#else
static constexpr bool DwarfEnabled = true;
#endif
