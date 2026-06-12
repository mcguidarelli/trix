#!/bin/bash
# build.sh -- Local builder for Trix (C++23, ASan/UBSan, -Werror).
#
# Usage:
#   ./build.sh               Debug build of ./trix, ./tetrix and ./chip8
#                            (silent on success, exit 0).  -O0, ASan + UBSan,
#                            all optional features enabled.
#   ./build.sh -v|--verbose  Echo the compile command before running.
#   ./build.sh --release     Build with optional features (vm-heap-tracking,
#                            debugger) disabled, to validate the compile-time
#                            gating and produce a slimmer binary.  Still uses
#                            -O0 and ASan -- it is a "feature-stripped debug"
#                            build, not a fully optimized release.
#   ./build.sh --optimized   Optimized release build, written to ./trix.opt,
#                            ./tetrix.opt and ./chip8.opt (the sanitized
#                            binaries from the default build are left in
#                            place).  -O3, no sanitizers, no debug symbols
#                            (stripped), -DNDEBUG, -fhardened
#                            (FORTIFY_SOURCE=3, _GLIBCXX_ASSERTIONS,
#                            stack-clash-protection, cf-protection, PIE,
#                            linker hardening).  Optional features
#                            (vm-heap-tracking, debugger) are disabled.
#                            Suitable for distribution; CMakeLists.txt with
#                            -DCMAKE_BUILD_TYPE=Release is the canonical
#                            path for packaged artifacts.
#   ./build.sh --both        Build BOTH the debug ./trix and the optimized
#                            ./trix.opt from the same source tree in one
#                            invocation, so the two never drift out of sync.
#                            Cannot be combined with --optimized / --release.
#   ./build.sh --no-zlib     Build without zlib: the deflate/inflate
#                            operators raise /unsupported and libz is not
#                            linked.  crc32/adler32 still work.  See
#                            src/build_config.inl (TRIX_NO_ZLIB).
#   ./build.sh --no-readline Build without GNU readline: the interactive
#                            REPL degrades to a plain prompt (no editing or
#                            history) and libreadline is not linked.  See
#                            src/build_config.inl (TRIX_NO_READLINE).
#   ./build.sh --signed-char Force `char` to signed (-fsigned-char) so this
#                            ARM box behaves like an x86 host at runtime
#                            (sign-extension, `c < 0`).  vm_t stays unsigned;
#                            only plain `char` flips.  Run the suite against
#                            the result to exercise the x86 char path.  For
#                            the COMPILE-time half (char<->vm_t warnings, which
#                            need clang) use tools/check_signed_char.py.
#                            Composes with --both / --optimized / etc.
#   ./build.sh -h|--help     Show this help and exit.
#
# Requires: gcc-15, libreadline-dev, zlib1g-dev (readline/zlib each optional
# via --no-readline / --no-zlib).
# Override the compiler with: CXX=g++-15 ./build.sh
#
# Flags here MUST stay in lockstep with CMakeLists.txt; CMake is the
# canonical build for release/packaged artifacts.

set -euo pipefail

# Run from the repo root regardless of the caller's CWD.
cd "$(dirname "${BASH_SOURCE[0]}")"

# ---- Argument parsing -------------------------------------------------

VERBOSE=0
RELEASE=0
OPTIMIZED=0
BOTH=0
NO_ZLIB=0
NO_READLINE=0
SIGNED_CHAR=0
for arg in "$@"; do
    case "${arg}" in
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^#\s\?//'
            exit 0
            ;;
        -v|--verbose)
            VERBOSE=1
            ;;
        --release)
            RELEASE=1
            ;;
        --optimized)
            OPTIMIZED=1
            ;;
        --both)
            BOTH=1
            ;;
        --no-zlib)
            NO_ZLIB=1
            ;;
        --no-readline)
            NO_READLINE=1
            ;;
        --signed-char)
            SIGNED_CHAR=1
            ;;
        *)
            echo "build.sh: unknown option '${arg}' (try --help)" >&2
            exit 2
            ;;
    esac
done

# --both already implies the debug + optimized pair; combining it with a
# single-variant selector is contradictory.
if [[ "${BOTH}" -eq 1 && "${OPTIMIZED}" -eq 1 ]]; then
    echo "build.sh: --both already builds the optimized binary; drop --optimized" >&2
    exit 2
fi
if [[ "${BOTH}" -eq 1 && "${RELEASE}" -eq 1 ]]; then
    echo "build.sh: --both builds the standard debug + optimized pair; --release (a feature-stripped debug) can't combine with it" >&2
    exit 2
fi

# ---- Toolchain check --------------------------------------------------

CXX="${CXX:-gcc-15}"

if ! command -v "${CXX}" >/dev/null 2>&1; then
    echo "build.sh: compiler '${CXX}' not found on PATH" >&2
    echo "build.sh: install gcc-15 or override via 'CXX=<compiler> ./build.sh'" >&2
    exit 1
fi

# ---- Compile / link flags --------------------------------------------

STD="-std=c++23"

# Warnings.  Mirror the GCC warning block in CMakeLists.txt.
# Used by both debug and optimized builds.
WARNS=(
    -Werror
    -Wall -Wextra -Wpedantic
    -Wformat=2 -Wformat-overflow=2 -Wformat-truncation=2 -Wformat-security
    -Wnull-dereference -Wstack-protector
    -Wtrampolines -Walloca -Wvla
    -Warray-bounds=2 -Wimplicit-fallthrough=3 -Wshift-overflow=2
    -Wcast-qual -Wstringop-overflow=4
    -Wconversion -Warith-conversion
    -Wlogical-op -Wduplicated-cond -Wduplicated-branches
    -Wformat-signedness -Wshadow -Wundef
    -Wstack-usage=16000 -Wswitch-enum
    -Wno-psabi -Wnrvo
    -Wcatch-value=2 -Wold-style-cast -Wzero-as-null-pointer-constant
    -Wextra-semi -Wsuggest-override -Wnon-virtual-dtor
    -Wredundant-tags -Wmismatched-tags
    -Wdate-time -Winvalid-utf8
    -Wrange-loop-construct -Wdangling-reference
    -Wdisabled-optimization -Wplacement-new=2 -Wdouble-promotion
)

LIBS=(-lm -lstdc++)
[[ "${NO_READLINE}" -eq 1 ]] || LIBS+=(-lreadline)
[[ "${NO_ZLIB}" -eq 1 ]] || LIBS+=(-lz)

# ---- Per-variant build function ---------------------------------------
#
# build_variant <debug|optimized> -- select the flag set for one variant
# and compile trix / tetrix / chip8 for it.  --both calls this twice from
# the same source tree (debug then optimized) so the two binaries can
# never drift out of sync.  The shared WARNS / LIBS arrays above are read
# but never mutated here -- each call takes a local WARNS copy (VWARNS) --
# so the two invocations don't contaminate each other's flags.
build_variant() {
    local mode="$1"

    local OPT DBG
    local -a DEFS CODEGEN VWARNS LINK_FLAGS
    VWARNS=("${WARNS[@]}")

    if [[ "${mode}" == "optimized" ]]; then
        # Optimized release build: -O3, hardened, no sanitizers, no debug info.
        # NDEBUG strips assert(); TRX_DEBUG-gated paths are excluded.  Optional
        # features (vm-heap-tracking, debugger) are disabled regardless of
        # --release.
        OPT="-O3"
        DBG=""
        DEFS=(-DNDEBUG)

        # Code-generation flags for optimized builds.  -fhardened (GCC 14+)
        # bundles _FORTIFY_SOURCE=3, _GLIBCXX_ASSERTIONS, -fstack-clash-protection,
        # -fcf-protection=full, PIE, and linker hardening (-z,now -z,relro
        # -z,noexecstack).  -Wno-hardened suppresses the "_FORTIFY_SOURCE
        # already defined" warning Ubuntu's gcc-15 emits at -O2+.
        CODEGEN=(
            -fno-rtti
            -fexceptions
            -fhardened
            -fvisibility=hidden
            -ffunction-sections
            -fdata-sections
            -pthread
        )
        VWARNS+=(-Wno-hardened)
        LINK_FLAGS=(-fhardened -Wl,--gc-sections)
    else
        # Debug build: -O0, sanitized, all features.  --release drops
        # vm-heap-tracking + debugger to validate compile-time feature gating.
        OPT="-O0"
        DBG="-g"
        DEFS=(-DTRX_DEBUG)
        if [[ "${RELEASE}" -eq 0 ]]; then
            DEFS+=(-DTRIX_HEAP_TRACKING -DTRIX_DEBUGGER)
        fi

        # Code-generation flags.  Mirror target_compile_options(trix ...) in CMakeLists.txt.
        CODEGEN=(
            -fstack-usage                        # emit per-function .su files (stack-depth audit)
            -fno-rtti
            -fexceptions
            -fsanitize=address
            -fsanitize=undefined
            -fsanitize-address-use-after-scope
            -fno-sanitize-recover=undefined      # UBSan findings abort the run
            -fstack-protector-strong
            -fvisibility=hidden
            -ffunction-sections
            -fdata-sections
            -pthread                             # required by std::thread (host IRQ delivery)
        )
        LINK_FLAGS=(-Wl,--gc-sections)
    fi

    # Optional dependency-trim gates (see src/build_config.inl).  Applied
    # to every variant so a --both pair is consistent across both binaries.
    if [[ "${NO_ZLIB}" -eq 1 ]]; then
        DEFS+=(-DTRIX_NO_ZLIB)
    fi
    if [[ "${NO_READLINE}" -eq 1 ]]; then
        DEFS+=(-DTRIX_NO_READLINE)
    fi

    # --signed-char: force `char` to signed so this ARM box behaves like an
    # x86 host at RUNTIME (sign-extension, `c < 0`), exercising the same code
    # path x86 CI runs.  vm_t stays unsigned; only plain `char` flips.  Pairs
    # with tools/check_signed_char.py, which covers the COMPILE-time half
    # (clang -Wsign-conversion -- gcc here cannot see char<->vm_t).
    if [[ "${SIGNED_CHAR}" -eq 1 ]]; then
        CODEGEN+=(-fsigned-char)
    fi

    # Optimized builds write to <name>.opt so the sanitized dev binaries
    # (./trix, ./tetrix) stay in place and tooling that prefers one over
    # the other can pick deterministically.
    local TRIX_BIN TETRIX_BIN CHIP8_BIN
    if [[ "${mode}" == "optimized" ]]; then
        TRIX_BIN="trix.opt"
        TETRIX_BIN="tetrix.opt"
        CHIP8_BIN="chip8.opt"
    else
        TRIX_BIN="trix"
        TETRIX_BIN="tetrix"
        CHIP8_BIN="chip8"
    fi

    # In a --both run, name the variant so a failed compile is unambiguous.
    if [[ "${BOTH}" -eq 1 ]]; then
        echo "build.sh: building ${mode} (${TRIX_BIN})..."
    fi

    rm -f "${TRIX_BIN}" "${TETRIX_BIN}" "${CHIP8_BIN}"

    if [[ "${VERBOSE}" -eq 1 ]]; then
        set -x
    fi

    # trix: main interpreter binary.
    "${CXX}" \
        "${STD}" "${OPT}" ${DBG:+"${DBG}"} "${DEFS[@]}" \
        "${CODEGEN[@]}" \
        "${VWARNS[@]}" \
        -o "${TRIX_BIN}" trix.cpp \
        "${LINK_FLAGS[@]}" \
        "${LIBS[@]}"

    # tetrix: dedicated binary for examples/tetrix.trx with native AI kernels.
    "${CXX}" \
        "${STD}" "${OPT}" ${DBG:+"${DBG}"} "${DEFS[@]}" \
        "${CODEGEN[@]}" \
        "${VWARNS[@]}" \
        -o "${TETRIX_BIN}" tetrix.cpp \
        "${LINK_FLAGS[@]}" \
        "${LIBS[@]}"

    # chip8: dedicated binary for examples/chip8.trx with a native CPU kernel.
    "${CXX}" \
        "${STD}" "${OPT}" ${DBG:+"${DBG}"} "${DEFS[@]}" \
        "${CODEGEN[@]}" \
        "${VWARNS[@]}" \
        -o "${CHIP8_BIN}" chip8.cpp \
        "${LINK_FLAGS[@]}" \
        "${LIBS[@]}"

    # Strip symbols from optimized binaries (no debug symbols, no symbol table).
    if [[ "${mode}" == "optimized" ]]; then
        strip --strip-all "${TRIX_BIN}" "${TETRIX_BIN}" "${CHIP8_BIN}"
    fi

    if [[ "${VERBOSE}" -eq 1 ]]; then
        set +x
    fi
}

# ---- Build ------------------------------------------------------------

# --both builds debug first (the fast-failing ASan/UBSan variant catches
# most errors), then optimized -- both from this one source tree.
if [[ "${BOTH}" -eq 1 ]]; then
    build_variant debug
    build_variant optimized
elif [[ "${OPTIMIZED}" -eq 1 ]]; then
    build_variant optimized
else
    build_variant debug
fi
