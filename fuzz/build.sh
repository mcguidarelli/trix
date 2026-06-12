#!/bin/bash
# build.sh -- Build the Trix fuzz harness with clang and libFuzzer.
#
# Requires: clang++-20 (or adjust CXX below), libreadline-dev
#
# Usage:
#   ./fuzz/build.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

CXX="${CXX:-clang++-20}"
CXXFLAGS="-std=c++23 -fno-rtti -fexceptions -O1 -g"
SANITIZERS="-fsanitize=fuzzer,address,undefined"
INCLUDES="-I${PROJECT_DIR}"
LIBS="-lreadline -lz -lpthread -lrt"
OUTDIR="${SCRIPT_DIR}"

# Clang-specific warnings that complement the gcc set used in build.sh.  These
# catch things the gcc build does not: unused set-variables / lambda captures,
# sign-shift overflow, loop-analysis bugs, self-assign / self-move, comma
# operator misuse, using-namespace in headers, pessimizing-moves, and more.
# -Wcovered-switch-default is deliberately NOT enabled -- Trix prefers
# defensive default: labels for missing-case-safety when enums grow.
WARNINGS="-Werror -Wall -Wextra \
  -Wunused-but-set-variable \
  -Wunused-lambda-capture \
  -Wconditional-uninitialized \
  -Wshift-sign-overflow \
  -Wloop-analysis \
  -Wrange-loop-analysis \
  -Wbitfield-enum-conversion \
  -Wself-assign \
  -Wself-move \
  -Wcomma \
  -Wheader-hygiene \
  -Wpessimizing-move \
  -Widiomatic-parentheses \
  -Wextra-semi \
  -Wnewline-eof \
  -Wunreachable-code-aggressive \
  -Wno-implicit-fallthrough"

echo "Building fuzz_trix..."
${CXX} ${CXXFLAGS} ${SANITIZERS} ${WARNINGS} ${INCLUDES} \
    "${SCRIPT_DIR}/fuzz_trix.cpp" \
    ${LIBS} -o "${OUTDIR}/fuzz_trix"
echo "  -> ${OUTDIR}/fuzz_trix"

echo "Done."
