#!/bin/bash
# Release-only build for benchmarking (no ASAN/UBSAN).  Produces benchmark/trix.rel.
# Run from anywhere; script anchors itself to the repo root.
set -e
cd "$(dirname "$0")/.."

CC=gcc-15
VER=c++23
LIB="-lreadline -lm -lpthread -lrt -lgcc -lc -lstdc++ -lstdc++exp -lz"
WARNS="-w"
FLAGS="-fno-rtti -fexceptions -fvisibility=hidden -ffunction-sections -fdata-sections -pthread"

rm -f benchmark/trix.rel
$CC -O2 -DNDEBUG -std=$VER $WARNS $FLAGS -o benchmark/trix.rel trix.cpp -Wl,--gc-sections $LIB
