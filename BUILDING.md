<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Building Trix

Trix is a single-header C++23 library (`trix.h`) with a thin `trix.cpp`
front-end.  There are two ways to build it:

* **`./build.sh`** — the local developer builder (fast to invoke, writes
  binaries to the repo root).  Best for day-to-day work.
* **CMake** — the canonical build for packaged/release artifacts.

Both paths stay in lockstep; CMake is authoritative for distribution.

Both build the **standalone interpreter** — the `trix` REPL and script runner.
If you are instead *embedding* Trix as a library in your own C++ program (often
the primary use), see [Embedding Trix](#embedding-trix); the compiler and
link-library requirements are identical to the standalone build.

## Dependencies

| Need       | Requirement                                             | Ubuntu/Debian package |
| ---------- | ------------------------------------------------------- | --------------------- |
| Compiler   | C++23: `gcc-15` (or `clang-20+`)                        | `g++-15` / `clang`    |
| Build tool | CMake 3.25+ (for the CMake path) or just `bash`         | `cmake`               |
| Readline   | GNU readline headers + lib (interactive REPL line edit) | `libreadline-dev`     |
| zlib       | zlib headers + lib (`deflate` / `inflate`)              | `zlib1g-dev`          |

Runtime dependencies are just the shared libraries: `libreadline` and `zlib`
(`sudo apt-get install libreadline8 zlib1g`).  There are no other external
dependencies.  Both readline and zlib are optional at build time — see
`--no-readline` / `--no-zlib` below and [Embedding Trix](#embedding-trix).

**Platforms:** Linux is the tested target (gcc-15 and clang-20).  macOS is
expected to work (POSIX-only, no platform-specific code) but is not regularly
tested.  Windows is not currently supported.

## Toolchain and portability

Trix targets **64-bit GCC or Clang only**.  Three assumptions about the host
toolchain are baked in; the first two are enforced at compile time, so a
non-conforming build fails to compile rather than miscompiling:

- **64-bit word.**  Trix is 64-bit only.  `src/types.inl` requires the
  `__int128` extension (which GCC and Clang provide only on 64-bit targets) and
  `src/snapshot.inl` asserts `sizeof(size_t) == 8`, because the snapshot file
  format serializes `size_t` at its native width.  A 32-bit build fails both
  `static_assert`s.  The in-memory `Object` is exactly 8 bytes and several
  structures pack against an 8-byte pointer (e.g. `Stream` is 96 bytes).
- **GCC or Clang.**  MSVC is unsupported — it provides neither `__int128` nor
  the `__builtin_*_overflow` family below.
- **C++23 standard library.**  Build with `-std=c++23`; a C++20 compiler is not
  enough.

### C++23 features required

| Feature                       | Header        | Used for                                |
| ----------------------------- | ------------- | --------------------------------------- |
| `std::print` / `std::println` | `<print>`     | All formatted console output            |
| `std::byteswap`               | `<bit>`       | Endian conversion in binary pack/unpack |
| `std::unreachable`            | `<utility>`   | Marking dispatch-table dead ends        |
| `[[assume(...)]]`             | *(attribute)* | Optimizer hints on hot invariants       |

These are what make the floor `gcc-15` / `clang-20+`: full library support for
`<print>`, `std::byteswap`, and `std::unreachable` lands in those releases.  The
codebase also leans on several features that are **C++20** (a C++23 compiler
already provides them) — `std::format` (which itself needs a C++23-era
libstdc++), `std::span`, `std::bit_cast`, `std::ranges`, the `<bit>` algorithms
(`std::popcount`, `std::countl_zero`, `std::countr_zero`), and the `[[likely]]`
/ `[[unlikely]]` attributes.

### Compiler builtins and extensions

All are common to both GCC and Clang:

| Extension | Used for |
| --- | --- |
| `__int128` / `unsigned __int128` (via `__extension__`) | The 128-bit integer type (`Int128` / `UInt128`); also the 64-bit gate |
| `__builtin_add_overflow` / `sub` / `mul` (plus the typed `__builtin_s*` / `u*` ones) | Checked integer arithmetic — overflow raises `/numerical-overflow` instead of wrapping |
| `__builtin_clzg` / `__builtin_ctzg` / `__builtin_popcountg` | Count-leading/trailing-zeros and population count |
| `[[gnu::noinline]]` | Keeping cold paths out of hot inner loops |

There is no inline assembly, no computed `goto`, and no other platform-specific
intrinsics.  Portability across both compilers — including signed and unsigned
`char` — is verified in CI.

## Quick build with `build.sh`

```bash
./build.sh               # debug build -> ./trix (+ ./tetrix, ./chip8)
./build.sh --optimized   # optimized build -> ./trix.opt (+ ./tetrix.opt, ./chip8.opt)
```

| Invocation | Output | Flags |
| --- | --- | --- |
| `./build.sh` | `./trix` | `-O0`, ASan + UBSan, `-Werror`, all optional features on (default) |
| `./build.sh --optimized` | `./trix.opt` | `-O3`, no sanitizers, stripped, `-DNDEBUG`, `-fhardened`, features off |
| `./build.sh --release` | `./trix` | Feature-stripped **debug** (features off, still `-O0` + ASan) — validates compile-time gating |
| `./build.sh --no-zlib` | (as above) | Drop zlib: `deflate`/`inflate` raise `/unsupported`, libz unlinked |
| `./build.sh --no-readline` | (as above) | Drop GNU readline: the REPL degrades to a plain prompt, libreadline unlinked |
| `./build.sh -v` | (as above) | Also echoes the compile command before running |

Override the compiler with `CXX=g++-15 ./build.sh`.  The debug binary is the
default target of the test suite (`./runtests.sh`).

## CMake build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/trix                 # interactive REPL
./build/trix script.trx      # run a script
```

Release (no sanitizers, optimized):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DTRIX_SANITIZERS=OFF
cmake --build build
```

### CMake options

| Option             | Default    | Effect                                              |
| ------------------ | ---------- | --------------------------------------------------- |
| `TRIX_SANITIZERS`  | ON (Debug) | AddressSanitizer + UndefinedBehaviorSanitizer       |
| `TRIX_WERROR`      | ON         | Treat warnings as errors (`-Werror`)                |
| `TRIX_FUZZ`        | OFF        | Build the libFuzzer harnesses only (needs Clang)    |
| `TRIX_NO_ZLIB`     | OFF        | Drop zlib: `deflate`/`inflate` raise `/unsupported` |
| `TRIX_NO_READLINE` | OFF        | Drop GNU readline: REPL degrades to a plain prompt  |

`TRIX_SANITIZERS` defaults ON for Debug and OFF otherwise.  Optional diagnostic
features (the in-language debugger, VM heap tracking) are compiled in for Debug
builds and gated out of Release / `--optimized` builds.  `TRIX_NO_ZLIB` /
`TRIX_NO_READLINE` are dependency-trim opt-outs: zlib and readline are required
by default, and these flags drop them (zlib stays `find_package(... REQUIRED)`
unless you opt out).

## Embedding Trix

Trix is a single-header library: drop `trix.h` into your project, `#include`
it, and instantiate a `Trix` VM.  The embedding API — constructing the VM,
registering host operators, driving execution, snapshots — is documented in
[`docs/host-integration.md`](docs/host-integration.md).

The build requirements match the standalone interpreter.  By default `trix.h`
includes `<readline/readline.h>` and `<zlib.h>`, so a default embed needs the
C++23 compiler, the readline and zlib development headers, and these flags:

| Need     | Flag                                              |
| -------- | ------------------------------------------------- |
| Compiler | `-std=c++23` (`gcc-15` / `clang-20+`)             |
| Threads  | `-pthread` (host IRQ delivery uses `std::thread`) |
| Readline | `-lreadline`                                      |
| zlib     | `-lz`                                             |
| libm     | `-lm`                                             |

A minimal embed compiles like any translation unit that includes the header:

```bash
g++ -std=c++23 -O2 -pthread -I/path/to/trix myapp.cpp -o myapp -lreadline -lz -lm
```

To shrink the dependency surface, define `TRIX_NO_READLINE` and/or
`TRIX_NO_ZLIB` (the gates `build.sh --no-readline` / `--no-zlib` pass): each
drops its header include and link library.  Without zlib the `deflate` /
`inflate` operators raise `/unsupported` (crc32 / adler32 keep working);
without readline the interactive REPL degrades to a plain prompt (no editing
or history).  A headless embed that wants neither:

```bash
g++ -std=c++23 -O2 -pthread -DTRIX_NO_READLINE -DTRIX_NO_ZLIB \
    -I/path/to/trix myapp.cpp -o myapp -lm
```

## Build directories

All build output directories are **gitignored** (`build/`, `build-fuzz/`):

| Directory     | Produced by       | Contents                            |
| ------------- | ----------------- | ----------------------------------- |
| `build/`      | `cmake -B build`  | The CMake build tree + `build/trix` |
| `build-fuzz/` | `./fuzz/build.sh` | The libFuzzer harness build tree    |

The repo-root binaries (`./trix`, `./trix.opt`) come from `build.sh` and are
likewise gitignored.

## Testing, fuzzing, benchmarking

```bash
./build.sh && ./runtests.sh   # build the debug binary, run the full test suite
```

* **Tests** — see [`tests/README.md`](tests/README.md) for the suite layout,
  the specialized runners, and the static-analysis gates.
* **Fuzzing** — see [`fuzz/README.md`](fuzz/README.md) (libFuzzer, requires
  `clang++-20`).
* **Benchmarks** — see [`benchmark/README.md`](benchmark/README.md).
