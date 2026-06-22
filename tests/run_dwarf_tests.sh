#!/bin/bash
# ====================================================================
# tests/run_dwarf_tests.sh -- DWARF reader (lib/dwarf.trx) regression suite
# ====================================================================
# Compiles a two-TU fixture (-> two compilation units) with debug info and
# asserts that lib/dwarf.trx resolves both globals + struct layouts by name,
# cross-checking each parsed address against nm.  Runs a producer x version
# matrix: the primary compiler at DWARF v4 and v5, plus clang at v5 (which
# exercises the strx/addrx + .debug_str_offsets/.debug_addr indirection that
# GCC does not emit).
#
# Exit code: 0 if all checks pass, 1 otherwise.
# ====================================================================

set -uo pipefail
cd "$(dirname "$0")/.."  # run from repo root

PS=./trix
PASS=0
FAIL=0
TMPDIR=/tmp/trix_dwarf_test_$$
mkdir -p "$TMPDIR"
cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "$PS" ]; then
    echo "ERROR: $PS not found. Run ./build.sh first."
    exit 1
fi

echo "===================================="
echo "  DWARF reader (lib/dwarf.trx) tests"
echo "===================================="

# DWARF gate: dwarf-open stays registered but raises /unsupported under
# --no-dwarf.  Distinguish gated-off (/unsupported) from present
# (/file-open-error on a missing path) and skip cleanly when gated off.
dprobe=$(echo "<< /unsupported { (gated) = } /file-open-error { (present) = } >> { (/nonexistent-dwarf-probe) dwarf-open } try-catch" | "$PS" --stdin 2>/dev/null)
if [ "$dprobe" = "gated" ]; then
    echo "  SKIP: DWARF support disabled in this build (TRIX_NO_DWARF)"
    exit 0
fi

# Pick a C++ compiler.  Honor $CXX, else first available driver (the clang CI
# leg has no gcc-15).  The fixture is a standalone data carrier -- it does NOT
# include trix.h, so no -I / -pthread / sanitizers are needed.
FIX_CXX="${CXX:-}"
if [ -z "$FIX_CXX" ]; then
    for c in g++-15 clang++-20 g++ c++; do
        command -v "$c" >/dev/null 2>&1 && { FIX_CXX="$c"; break; }
    done
fi

# Build the two-TU fixture with one compiler + DWARF flag, run the assertions,
# and cross-check the parsed g_probe address against nm.  -fno-pie/-no-pie so
# DW_OP_addr (and clang's addrx target) is the absolute link-time address.
build_and_assert() {
    label="$1"
    cxx="$2"
    dwflag="$3"  # may be multiple flags (word-split intentionally), e.g. "-gdwarf-5 -gdwarf64"
    bin="$TMPDIR/fix_$label"
    if "$cxx" -std=c++23 -g $dwflag -O0 -fno-pie -c tests/dwarf_fixture.cpp -o "$TMPDIR/${label}_a.o" 2>"$TMPDIR/$label.err" &&
        "$cxx" -std=c++23 -g $dwflag -O0 -fno-pie -c tests/dwarf_fixture_b.cpp -o "$TMPDIR/${label}_b.o" 2>>"$TMPDIR/$label.err" &&
        "$cxx" -no-pie "$TMPDIR/${label}_a.o" "$TMPDIR/${label}_b.o" -o "$bin" 2>>"$TMPDIR/$label.err"; then
        pass "[$label] fixture built ($cxx $dwflag, 2 CUs)"
    else
        fail "[$label] fixture build failed"
        cat "$TMPDIR/$label.err" >&2
        return
    fi

    if "$PS" tests/dwarf_layout.trx "$bin" > "$TMPDIR/$label.out" 2>"$TMPDIR/$label.run.err" &&
        grep -q "__test_ok__" "$TMPDIR/$label.out" && ! grep -qE "^FAILED:" "$TMPDIR/$label.out"; then
        pass "[$label] all layout assertions passed"
    else
        fail "[$label] layout assertions failed"
        grep -E "^FAILED:" "$TMPDIR/$label.out" 2>/dev/null
        cat "$TMPDIR/$label.run.err" >&2
    fi

    parsed=$(grep -oE 'g_probe-address=[0-9]+' "$TMPDIR/$label.out" | head -1 | cut -d= -f2)
    nmhex=$(nm "$bin" 2>/dev/null | awk '$NF=="g_probe"{print $1; exit}')
    if [ -n "$nmhex" ]; then
        expect=$(printf '%d' "0x$nmhex")
        if [ -n "$parsed" ] && [ "$parsed" = "$expect" ]; then
            pass "[$label] parsed address matches nm ($expect)"
        else
            fail "[$label] address mismatch (parsed=$parsed nm=$expect)"
        fi
    fi

    # L-8: the function's DW_AT_low_pc must match nm's symbol address.
    fnparsed=$(grep -oE 'dwarf_probe_fn-address=[0-9]+' "$TMPDIR/$label.out" | head -1 | cut -d= -f2)
    fnnmhex=$(nm "$bin" 2>/dev/null | awk '$NF=="dwarf_probe_fn"{print $1; exit}')
    if [ -n "$fnnmhex" ]; then
        fnexpect=$(printf '%d' "0x$fnnmhex")
        if [ -n "$fnparsed" ] && [ "$fnparsed" = "$fnexpect" ]; then
            pass "[$label] function low_pc matches nm ($fnexpect)"
        else
            fail "[$label] function address mismatch (parsed=$fnparsed nm=$fnexpect)"
        fi
    fi

    # L-22: dwarf-line of the function's low_pc must agree with addr2line (the
    # authoritative oracle, same as nm for addresses).  Compare the line exactly
    # and the source file by basename (dwarf-line dir-joins; addr2line may differ
    # in absolute vs relative).  Skipped if addr2line is absent or can't resolve.
    lnparsed=$(grep -oE 'dwarf_probe_fn-line=[0-9]+' "$TMPDIR/$label.out" | head -1 | cut -d= -f2)
    flparsed=$(grep -E 'dwarf_probe_fn-file=' "$TMPDIR/$label.out" | head -1 | sed 's/^dwarf_probe_fn-file=//')
    if command -v addr2line >/dev/null 2>&1 && [ -n "$fnnmhex" ]; then
        a2l=$(addr2line -e "$bin" "0x$fnnmhex" 2>/dev/null | head -1)
        a2l_file=$(printf '%s' "$a2l" | sed 's/:[0-9].*$//')
        a2l_line=$(printf '%s' "$a2l" | grep -oE ':[0-9]+' | head -1 | tr -d ':')
        if printf '%s' "$a2l" | grep -q '??'; then
            echo "  NOTE: [$label] addr2line could not resolve dwarf_probe_fn; line cross-check skipped"
        elif [ -n "$lnparsed" ] && [ "$lnparsed" = "$a2l_line" ] && [ "$(basename "$flparsed")" = "$(basename "$a2l_file")" ]; then
            pass "[$label] dwarf-line matches addr2line ($(basename "$a2l_file"):$a2l_line)"
        else
            fail "[$label] line mismatch (dwarf-line=$(basename "$flparsed"):$lnparsed addr2line=$(basename "$a2l_file"):$a2l_line)"
        fi
    fi
}

build_and_assert v2 "$FIX_CXX" -gdwarf-2  # block-form location + DW_OP_plus_uconst member offsets
build_and_assert v3 "$FIX_CXX" -gdwarf-3
build_and_assert v4 "$FIX_CXX" -gdwarf-4
build_and_assert v5 "$FIX_CXX" -gdwarf-5
build_and_assert v5-dwarf64 "$FIX_CXX" "-gdwarf-5 -gdwarf64"  # 64-bit DWARF (8-byte offsets, ref8)

# clang v5 specifically exercises strx/addrx indirection (GCC v5 does not emit
# .debug_str_offsets/.debug_addr).  Run it when a distinct clang is available.
CLANG=""
for c in clang++-20 clang++-19 clang++; do
    command -v "$c" >/dev/null 2>&1 && { CLANG="$c"; break; }
done
if [ -n "$CLANG" ] && [ "$CLANG" != "$FIX_CXX" ]; then
    build_and_assert clang-v5 "$CLANG" -gdwarf-5
    build_and_assert clang-v5-dwarf64 "$CLANG" "-gdwarf-5 -gdwarf64"
else
    echo "  NOTE: no distinct clang found; strx/addrx covered only if the primary compiler emits it"
fi

# Big-endian (L-7): a hand-built BE ELF64 + DWARF v4, since no BE cross-toolchain
# is assumed.  Exercises every byte-order-sensitive read + typedef/pointer/enum.
if command -v python3 >/dev/null 2>&1; then
    BE_ELF="$TMPDIR/dwarf_be.elf"
    if python3 tests/gen_dwarf_be_fixture.py "$BE_ELF" 2>"$TMPDIR/be_gen.err"; then
        pass "[be] big-endian fixture generated"
        if "$PS" tests/dwarf_be.trx "$BE_ELF" > "$TMPDIR/be.out" 2>"$TMPDIR/be.run.err" &&
            grep -q "__test_ok__" "$TMPDIR/be.out" && ! grep -qE "^FAILED:" "$TMPDIR/be.out"; then
            pass "[be] big-endian layout assertions passed"
        else
            fail "[be] big-endian assertions failed"
            grep -E "^FAILED:" "$TMPDIR/be.out" 2>/dev/null
            cat "$TMPDIR/be.run.err" >&2
        fi
    else
        fail "[be] could not generate big-endian fixture"
        cat "$TMPDIR/be_gen.err" >&2
    fi
else
    echo "  NOTE: python3 not found; big-endian fixture (L-7) not exercised"
fi

# Large-binary leg (L-1): a DIE-heavy STL TU parsed past the old 4096-element
# scratch ceiling.  The lazy reader's per-read churn wants a bigger heap and
# the walk is slow on the debug build, so run it on trix.opt (fast) when
# present; skip otherwise to keep the default debug suite quick.
if [ -x ./trix.opt ]; then
    BIGBIN="$TMPDIR/fix_big"
    if "$FIX_CXX" -std=c++23 -g -gdwarf-5 -O0 -fno-pie -no-pie tests/dwarf_fixture_big.cpp -o "$BIGBIN" 2>"$TMPDIR/big.err"; then
        ndies=$(readelf --debug-dump=info "$BIGBIN" 2>/dev/null | grep -cE '\(DW_TAG')
        if [ "${ndies:-0}" -gt 4096 ]; then
            pass "[big] DIE-heavy fixture built ($ndies DIEs > 4096 scratch ceiling)"
            if ./trix.opt --vm-size=134217728 tests/dwarf_big.trx "$BIGBIN" > "$TMPDIR/big.out" 2>"$TMPDIR/big.run.err" &&
                grep -q "__test_ok__" "$TMPDIR/big.out" && ! grep -qE "^FAILED:" "$TMPDIR/big.out"; then
                pass "[big] $ndies-DIE binary parsed + layout assertions passed"
            else
                fail "[big] DIE-heavy parse/assertions failed"
                grep -E "^FAILED:" "$TMPDIR/big.out" 2>/dev/null
                cat "$TMPDIR/big.run.err" >&2
            fi
            parsed=$(grep -oE 'g_big-address=[0-9]+' "$TMPDIR/big.out" | head -1 | cut -d= -f2)
            nmhex=$(nm "$BIGBIN" 2>/dev/null | awk '$NF=="g_big"{print $1; exit}')
            if [ -n "$nmhex" ]; then
                expect=$(printf '%d' "0x$nmhex")
                if [ -n "$parsed" ] && [ "$parsed" = "$expect" ]; then
                    pass "[big] parsed address matches nm ($expect)"
                else
                    fail "[big] address mismatch (parsed=$parsed nm=$expect)"
                fi
            fi
        else
            echo "  NOTE: big fixture only ${ndies:-0} DIEs (<=4096); L-1 ceiling not exercised here"
        fi
    else
        fail "[big] DIE-heavy fixture build failed"
        cat "$TMPDIR/big.err" >&2
    fi
else
    echo "  NOTE: ./trix.opt not present; large-binary leg (L-1) skipped (build with --optimized to run it)"
fi

# L-19 guard: split DWARF (-gsplit-dwarf) must be rejected with a clear
# /unsupported error -- its real DIEs live in a separate .dwo that is typically
# absent at runtime, so silently mis-parsing the skeleton would yield empty
# lookups.  Cover both dialects: the DWARF5 skeleton (DW_UT_skeleton, caught at
# the CU header) and pre-v5 GNU split (DW_AT_GNU_dwo_name on the root).
cat > "$TMPDIR/split_probe.trx" <<'PROBE'
(lib/dwarf.trx) run
command-line-args 0 get /bin exch def
<< /unsupported { (split-rejected) = } >> { bin dwarf-load pop (split-NOT-rejected) = } try-catch
PROBE
for spec in "gdwarf-5:v5-skeleton" "gdwarf-4:v4-gnu"; do
    sfl=${spec%:*}
    stag=${spec##*:}
    if "$FIX_CXX" -std=c++23 -g "-$sfl" -gsplit-dwarf -O0 -fno-pie -c tests/dwarf_fixture.cpp -o "$TMPDIR/sp_$stag.o" 2>"$TMPDIR/sp_$stag.err" &&
        "$FIX_CXX" -no-pie "$TMPDIR/sp_$stag.o" -o "$TMPDIR/split_$stag" 2>>"$TMPDIR/sp_$stag.err"; then
        sres=$("$PS" "$TMPDIR/split_probe.trx" "$TMPDIR/split_$stag" 2>/dev/null | grep -oE 'split-(rejected|NOT-rejected)' | head -1)
        if [ "$sres" = "split-rejected" ]; then
            pass "[split:$stag] split DWARF rejected with /unsupported"
        else
            fail "[split:$stag] split DWARF not rejected (got '${sres:-<no output>}')"
        fi
    else
        echo "  NOTE: [split:$stag] could not build a -gsplit-dwarf fixture; guard test skipped"
    fi
done

# Live self-introspection leg: load THIS debug binary's own DWARF (~140K DIEs,
# ~5s) and decode a known debug-only anchor global from live process memory via
# dwarf-peek-live, cross-checked against the static (file-image) read, plus a
# function lookup.  Only the debug build ($PS) carries -g + the TRX_DEBUG anchor
# g_trix_dwarf_self_probe -- cheap nm symbol probe (no DWARF load) skips cleanly
# if absent (e.g. an -DNDEBUG build).  Capture rather than `| grep -q`: with
# `set -o pipefail`, grep -q closing the pipe early SIGPIPEs nm (exit 141) and
# pipefail would report the whole probe as failed even on a match.
anchor_sym=$(nm "$PS" 2>/dev/null | grep g_trix_dwarf_self_probe)
if [ -n "$anchor_sym" ]; then
    if "$PS" --vm-size=268435456 tests/dwarf_self_peek.trx > "$TMPDIR/self.out" 2>"$TMPDIR/self.err" &&
        grep -q "__test_ok__" "$TMPDIR/self.out" && ! grep -qE "^FAILED:" "$TMPDIR/self.out"; then
        pass "[self] live self-peek + static cross-check + fn lookup on $PS"

        # L-22: cross-check dwarf-line(main) against addr2line on the real binary.
        mln=$(grep -oE 'main-line=[0-9]+' "$TMPDIR/self.out" | head -1 | cut -d= -f2)
        mfl=$(grep -E 'main-file=' "$TMPDIR/self.out" | head -1 | sed 's/^main-file=//')
        mainhex=$(nm "$PS" 2>/dev/null | awk '$NF=="main"{print $1; exit}')
        if command -v addr2line >/dev/null 2>&1 && [ -n "$mainhex" ]; then
            ma2l=$(addr2line -e "$PS" "0x$mainhex" 2>/dev/null | head -1)
            ma2l_file=$(printf '%s' "$ma2l" | sed 's/:[0-9].*$//')
            ma2l_line=$(printf '%s' "$ma2l" | grep -oE ':[0-9]+' | head -1 | tr -d ':')
            if printf '%s' "$ma2l" | grep -q '??'; then
                echo "  NOTE: [self] addr2line could not resolve main; line cross-check skipped"
            elif [ -n "$mln" ] && [ "$mln" = "$ma2l_line" ] && [ "$(basename "$mfl")" = "$(basename "$ma2l_file")" ]; then
                pass "[self] dwarf-line(main) matches addr2line ($(basename "$ma2l_file"):$ma2l_line)"
            else
                fail "[self] line mismatch (dwarf-line=$(basename "$mfl"):$mln addr2line=$(basename "$ma2l_file"):$ma2l_line)"
            fi
        fi
    else
        fail "[self] self-introspection assertions failed"
        grep -E "^FAILED:" "$TMPDIR/self.out" 2>/dev/null
        cat "$TMPDIR/self.err" >&2
    fi
else
    echo "  NOTE: $PS has no TRX_DEBUG self-probe anchor; live self-peek leg skipped"
fi

# Shared-object live-peek leg (L-21): build a tiny .so carrying known globals,
# LD_PRELOAD it into THIS trix process, then load the .so's OWN DWARF by path and
# read those globals from live memory.  Proves module-load-bias-for relocates by
# the .so's load base (not the main exe's).  Run on the debug $PS (always built);
# the host needs no debug info of its own.  verify_asan_link_order=0: LD_PRELOAD
# puts the (non-ASan) probe ahead of libasan in the initial list, which the debug
# build's link-order check would otherwise reject -- the probe is pure data with
# no pre-init allocations, so the ordering is harmless here.
SO="$TMPDIR/libdwarf_l21_probe.so"
if "$FIX_CXX" -std=c++23 -g -gdwarf-4 -O0 -fPIC -shared tests/dwarf_l21_probe.cpp -o "$SO" 2>"$TMPDIR/so.err"; then
    pass "[so] L-21 shared-object fixture built ($FIX_CXX)"
    if ASAN_OPTIONS="verify_asan_link_order=0" LD_PRELOAD="$SO" \
        "$PS" tests/dwarf_so_peek.trx "$SO" > "$TMPDIR/so.out" 2>"$TMPDIR/so.run.err" &&
        grep -q "__test_ok__" "$TMPDIR/so.out" && ! grep -qE "^FAILED:" "$TMPDIR/so.out"; then
        pass "[so] live .so peek + static cross-check (module-load-bias-for relocation)"
    else
        fail "[so] shared-object live-peek assertions failed"
        grep -E "^FAILED:" "$TMPDIR/so.out" 2>/dev/null
        cat "$TMPDIR/so.run.err" >&2
    fi
else
    fail "[so] L-21 shared-object fixture build failed"
    cat "$TMPDIR/so.err" >&2
fi

echo ""
echo "DWARF tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
