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

//===--- Hash Functions ---===//
//
// Three families cover every hashing need in the interpreter:
//
//   - wyhash32_sv: variable-length hash for string/name content (dict keys).
//     Wang Yi's wyhash short-key variant; SMHasher-clean distribution; all
//     primitives are constexpr so dict-key hashes can be computed at scan
//     time when the input is a string literal.
//
//   - mix32 / mix64_to_32: fixed-size integer bit mixers (numeric & offset
//     keys).  Two-multiply finalizers (~5 cycles) with good avalanche:
//     mix32 is a Murmur3/Stafford-style 32-bit finalizer (0x7feb352d /
//     0x846ca68b); mix64_to_32 is the MurmurHash3 fmix64 finalizer
//     (Appleby) folded to 32 bits.
//
//   - fastmod_u32 / fastmod_magic_u32: D. Lemire's "fast remainder by
//     arbitrary divisor" (2019).  Replaces hardware integer division
//     (~20 cycles on x86) with a mul+mul+shift (~3-4 cycles) once a
//     per-divisor magic has been precomputed.  Used by dict's bucket
//     lookup, where the bucket count changes only on rehash.
//
// All routines are pure (no instance state), constexpr-eligible, and
// rely on hash_t / uint64_t / __uint128_t.  They live here rather than
// in vm_heap.inl because they have zero allocator dependency.
//
// CRC-32 (IEEE 802.3 / ISO HDLC) lives in snapshot.inl, not here -- its
// only callers are the snapshot/thaw machinery and the user-op table
// integrity check, both already inside that file.
//
//===----------------------------------------------------------------------===//

// Integer bit mixers.
[[nodiscard]] static constexpr hash_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

[[nodiscard]] static constexpr hash_t mix64_to_32(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return static_cast<hash_t>(x);
}

// wyhash (Wang Yi, public domain) -- short-key variable-length hash for dict keys.
// Processes 8 bytes per 64x64->128 multiply-and-fold, SMHasher-clean distribution.
//
// INVARIANT: Name and String objects with identical content must hash identically
// (Object::equal treats them interchangeably as dict keys).  Both use this function.
[[nodiscard]] static constexpr uint64_t wy_read8(const char *p) {
    if consteval {
        uint64_t v = 0;
        for (auto i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (i * 8);
        }
        return v;
    } else {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }
}

[[nodiscard]] static constexpr uint64_t wy_read4(const char *p) {
    if consteval {
        uint64_t v = 0;
        for (auto i = 0; i < 4; ++i) {
            v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (i * 8);
        }
        return v;
    } else {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return static_cast<uint64_t>(v);
    }
}

// 64x64 -> 128 multiply, folded back to 64 bits (wyhash core primitive).
[[nodiscard]] static constexpr uint64_t wy_mum(uint64_t a, uint64_t b) {
    const __uint128_t r = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
    return (static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64));
}

[[nodiscard]] static constexpr hash_t wyhash32_sv(std::string_view sv) {
    constexpr uint64_t S0 = 0xa0761d6478bd642fULL;
    constexpr uint64_t S1 = 0xe7037ed1a0b428dbULL;

    auto p = sv.data();
    auto n = sv.size();
    uint64_t seed = S0;
    uint64_t a;
    uint64_t b;

    if (n <= 16) {
        if (n >= 4) {
            // read first-4, mid-4, last-4, pre-last-4 (mid slot scrambles for uniqueness)
            const size_t mid = (n >> 3) << 2;
            a = (wy_read4(p) << 32) | wy_read4(p + mid);
            b = (wy_read4(p + n - 4) << 32) | wy_read4(p + n - 4 - mid);
        } else if (n > 0) {
            // 1-3 bytes: pack first/mid/last into a, b=0
            a = (static_cast<uint64_t>(static_cast<uint8_t>(p[0])) << 16) |
                (static_cast<uint64_t>(static_cast<uint8_t>(p[n >> 1])) << 8) |
                static_cast<uint64_t>(static_cast<uint8_t>(p[n - 1]));
            b = 0;
        } else {
            a = 0;
            b = 0;
        }
    } else {
        auto i = n;
        while (i > 16) {
            seed = wy_mum(wy_read8(p) ^ S1, wy_read8(p + 8) ^ seed);
            p += 16;
            i -= 16;
        }
        // read final 16 bytes (may overlap the last block -- ok)
        a = wy_read8(p + i - 16);
        b = wy_read8(p + i - 8);
    }

    const uint64_t mixed = wy_mum(S1 ^ n, wy_mum(a ^ S1, b ^ seed));
    return static_cast<hash_t>(mixed ^ (mixed >> 32));
}

// Lemire's "fast remainder by arbitrary divisor" (2019).  Replaces a hardware
// integer division (~20 cycles on x86) with a mul+mul+shift (~3-4 cycles) once
// a per-divisor magic has been precomputed.  Valid for 0 < d <= 2^32 and
// n < 2^32.  The magic depends only on the divisor, so callers cache it next
// to the bucket count and recompute only when the bucket count changes.
[[nodiscard]] static constexpr uint64_t fastmod_magic_u32(uint32_t d) {
    return (((~uint64_t{0}) / d) + 1);
}

[[nodiscard]] static constexpr uint32_t fastmod_u32(uint32_t n, uint64_t magic, uint32_t d) {
    const uint64_t lowbits = magic * n;
    return static_cast<uint32_t>((static_cast<__uint128_t>(lowbits) * d) >> 64);
}
