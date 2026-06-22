cmake_minimum_required(VERSION 3.25)
project(trix
    VERSION 0.0.74
    DESCRIPTION "Embeddable stack-based scripting VM with Erlang-style concurrency"
    LANGUAGES CXX
)

# ── C++23 ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ──────────────────────────────────────────────────────────
# Default build type: Debug (sanitized dev build).  Use -DCMAKE_BUILD_TYPE=Release
# (or RelWithDebInfo) for a hardened production build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# Sanitizers default ON for Debug, OFF for optimized builds.  Override with
# -DTRIX_SANITIZERS=ON/OFF on the cmake command line.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(TRIX_SANITIZERS_DEFAULT ON)
else()
    set(TRIX_SANITIZERS_DEFAULT OFF)
endif()
option(TRIX_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer" ${TRIX_SANITIZERS_DEFAULT})
option(TRIX_WERROR     "Treat warnings as errors (-Werror)" ON)
option(TRIX_FUZZ       "Build libFuzzer harnesses only (requires Clang)" OFF)
option(TRIX_NO_ZLIB    "Build without zlib (deflate/inflate raise /unsupported)" OFF)
option(TRIX_NO_READLINE "Build without GNU readline (REPL degrades to a plain prompt)" OFF)

# ── Dependencies ─────────────────────────────────────────────────────
find_package(Threads REQUIRED)
if(NOT TRIX_NO_ZLIB)
    find_package(ZLIB REQUIRED)
endif()

# readline: no standard FindReadline module; locate directly
if(NOT TRIX_NO_READLINE)
    find_path(READLINE_INCLUDE_DIR readline/readline.h)
    find_library(READLINE_LIBRARY readline)
    if(NOT READLINE_INCLUDE_DIR OR NOT READLINE_LIBRARY)
        message(FATAL_ERROR "GNU readline not found (install libreadline-dev)")
    endif()
endif()

# ── Main target (skipped when TRIX_FUZZ is ON) ──────────────────────
if(NOT TRIX_FUZZ)
add_executable(trix trix.cpp)

target_link_libraries(trix PRIVATE
    Threads::Threads
)

# zlib is optional: -DTRIX_NO_ZLIB=ON drops the dependency and the
# deflate/inflate operators raise /unsupported (see src/build_config.inl).
if(TRIX_NO_ZLIB)
    target_compile_definitions(trix PRIVATE TRIX_NO_ZLIB)
else()
    target_link_libraries(trix PRIVATE ZLIB::ZLIB)
endif()

# readline is optional: -DTRIX_NO_READLINE=ON drops the dependency and the
# interactive REPL degrades to a plain prompt (see src/build_config.inl).
if(TRIX_NO_READLINE)
    target_compile_definitions(trix PRIVATE TRIX_NO_READLINE)
else()
    target_link_libraries(trix PRIVATE ${READLINE_LIBRARY})
    target_include_directories(trix PRIVATE ${READLINE_INCLUDE_DIR})
endif()

# ── Compiler flags ───────────────────────────────────────────────────
target_compile_options(trix PRIVATE
    -fno-rtti
    -fexceptions
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
    -fstack-protector-strong
    # Emit per-function stack-usage (.su) under Debug; matches build.sh
    $<$<CONFIG:Debug>:-fstack-usage>

    # Warnings (portable subset)
    -Wall -Wextra -Wpedantic
    -Wformat=2 -Wformat-security
    -Wnull-dereference -Wstack-protector -Walloca -Wvla
    -Wcast-qual -Wconversion
    -Wshadow -Wundef -Wswitch-enum
    -Wold-style-cast -Wzero-as-null-pointer-constant
    -Wextra-semi -Wsuggest-override -Wnon-virtual-dtor
    -Wmismatched-tags -Wdate-time -Winvalid-utf8
    -Wrange-loop-construct
    -Wdisabled-optimization -Wdouble-promotion
)

# GCC-specific warnings (not supported by Clang)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(trix PRIVATE
        -Wformat-overflow=2 -Wformat-truncation=2
        -Wtrampolines
        -Warray-bounds=2 -Wimplicit-fallthrough=3 -Wshift-overflow=2
        -Wstringop-overflow=4 -Warith-conversion
        -Wlogical-op -Wduplicated-cond -Wduplicated-branches
        -Wformat-signedness -Wstack-usage=16000
        -Wno-psabi -Wnrvo
        -Wcatch-value=2 -Wredundant-tags
        -Wdangling-reference -Wplacement-new=2
        -Walloc-zero -Wsign-promo -Wredundant-decls
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # The anonymous-union layout in object.inl is an intentional GCC extension
    # that clang flags under -Wpedantic; suppress just that one warning.
    target_compile_options(trix PRIVATE -Wno-nested-anon-types)
endif()

if(TRIX_WERROR)
    target_compile_options(trix PRIVATE -Werror)
endif()

if(TRIX_SANITIZERS)
    target_compile_options(trix PRIVATE
        -fsanitize=address,undefined
        -fsanitize-address-use-after-scope
        -fno-sanitize-recover=undefined
    )
    target_link_options(trix PRIVATE -fsanitize=address,undefined)
endif()

# Hardening for optimized builds: -fhardened (GCC 14+) bundles _FORTIFY_SOURCE=3,
# _GLIBCXX_ASSERTIONS, -fstack-clash-protection, -fcf-protection=full, PIE, and
# linker hardening (-z,now -z,relro -z,noexecstack).  Skipped for Debug because
# _FORTIFY_SOURCE requires -O1+ and emits a no-op warning otherwise.
# -Wno-hardened: Ubuntu's gcc-15 pre-defines _FORTIFY_SOURCE=3 at -O2+, which
# -fhardened would set to the same value; suppress the "already-defined" warning.
target_compile_options(trix PRIVATE
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:-fhardened;-Wno-hardened>
)
target_link_options(trix PRIVATE
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:-fhardened>
)

target_link_options(trix PRIVATE -Wl,--gc-sections)

# Strip Release at link time so an optimized binary is ALWAYS stripped, even from
# a raw `cmake --build` (the build.sh / Dockerfile / release.yml strip steps then
# become a harmless no-op).  Only Release: RelWithDebInfo intentionally keeps -g.
target_link_options(trix PRIVATE $<$<CONFIG:Release>:-s>)

target_compile_definitions(trix PRIVATE
    $<$<CONFIG:Debug>:TRX_DEBUG>
)
endif() # NOT TRIX_FUZZ

# ── Fuzz targets (clang only, opt-in) ────────────────────────────────

if(TRIX_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "TRIX_FUZZ requires Clang (use -DCMAKE_CXX_COMPILER=clang++-20)")
    endif()

    # Common properties for fuzz targets
    set(FUZZ_SANITIZERS "-fsanitize=fuzzer,address,undefined")
    set(FUZZ_LIBS ${READLINE_LIBRARY} Threads::Threads ZLIB::ZLIB)

    # Scanner-only fuzzer
    add_executable(fuzz_scanner fuzz/fuzz_scanner.cpp)
    target_include_directories(fuzz_scanner PRIVATE ${READLINE_INCLUDE_DIR})
    target_link_libraries(fuzz_scanner PRIVATE ${FUZZ_LIBS})
    target_compile_options(fuzz_scanner PRIVATE
        -fno-rtti -fexceptions -O1 -g
        ${FUZZ_SANITIZERS}
        -Wno-parentheses-equality
    )
    target_link_options(fuzz_scanner PRIVATE ${FUZZ_SANITIZERS})

    # Full interpreter fuzzer
    add_executable(fuzz_interpreter fuzz/fuzz_interpreter.cpp)
    target_include_directories(fuzz_interpreter PRIVATE ${READLINE_INCLUDE_DIR})
    target_link_libraries(fuzz_interpreter PRIVATE ${FUZZ_LIBS})
    target_compile_options(fuzz_interpreter PRIVATE
        -fno-rtti -fexceptions -O1 -g
        ${FUZZ_SANITIZERS}
        -Wno-parentheses-equality
    )
    target_link_options(fuzz_interpreter PRIVATE ${FUZZ_SANITIZERS})
endif()

# ── Install ──────────────────────────────────────────────────────────
if(NOT TRIX_FUZZ)
    install(TARGETS trix RUNTIME DESTINATION bin)
endif()
install(FILES trix.h DESTINATION include)
install(DIRECTORY src/ DESTINATION include/trix FILES_MATCHING PATTERN "*.inl")
