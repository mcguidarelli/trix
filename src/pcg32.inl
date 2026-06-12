//===----------------------------------------------------------------------===//
//                                                                            //
// 32-bit Permuted Congruential Generator                                     //
//                                                                            //
// Author: Melissa O'Neill, <oneill@pcg-random.org>                           //
// Homepage: https://www.pcg-random.org/                                      //
// Paper: "PCG: A Family of Simple Fast Space-Efficient Statistically Good    //
//         Algorithms for Random Number Generation" (2014)                    //
//                                                                            //
// Copyright 2014 Melissa O'Neill <oneill@pcg-random.org>                     //
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

class PCG32 {
    static constexpr uint64_t DEFAULT_STATE{0x853C49E6748FEA9Bull};
    static constexpr uint64_t DEFAULT_INC{0xDA3E39CB94B95BDBull};
public:
    PCG32() : m_state{DEFAULT_STATE}, m_inc{DEFAULT_INC} {
        // uses known seed values if open fails or upon a partial read from the "/dev/urandom" device
        auto fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            // use system random device to generate initial state and inc
            auto state = m_state;
            auto n = ::read(fd, &state, sizeof(state));
            if (n == static_cast<::ssize_t>(sizeof(state))) {
                auto inc = m_inc;
                n = ::read(fd, &inc, sizeof(inc));
                if (n == static_cast<::ssize_t>(sizeof(inc))) {
                    seed(state, inc);
                }
            }
            ::close(fd);
        }
    }

    void seed(uint64_t state, uint64_t inc = 1) {
        m_state = 0;

        // LCG increment must be odd for maximum 2^64 period
        m_inc = ((inc << 1) | 1);

        // O'Neill's init sequence: advance once from the degenerate all-zeros state,
        // then inject the desired state, then advance again to reach a well-mixed position.
        static_cast<void>(next_uint32());
        m_state += state;
        static_cast<void>(next_uint32());
    }

    // generate a uniformly distributed unsigned 32-bit random number
    [[nodiscard]] uint32_t next_uint32() {
        constexpr uint64_t MULTIPLIER{0x5851F42D4C957F2Dull};

        auto old_state = m_state;
        m_state = ((old_state * MULTIPLIER) + m_inc);
        auto xor_shifted = static_cast<uint32_t>(((old_state >> 18u) ^ old_state) >> 27u);
        auto rot = static_cast<uint32_t>(old_state >> 59u);
        return std::rotr(xor_shifted, static_cast<int>(rot));
    }

    // generate a uniformly distributed number: 0 <= r < bound
    [[nodiscard]] uint32_t next_uint32(uint32_t bound) {
        assert(bound != 0 && "next_uint32: bound is 0");

        [[assume(bound != 0)]];

        // Rejection threshold: (2^32 - bound) % bound, via unsigned wraparound.
        const auto threshold = (uint32_t{} - bound) % bound;
        while (true) {
            auto r = next_uint32();
            // drop values less than threshold to avoid bias
            if (r >= threshold) {
                return (r % bound);
            }
        }
    }

    // generate a uniformly distributed unsigned 64-bit random number by
    // concatenating two independent 32-bit draws from the PCG32 stream
    [[nodiscard]] uint64_t next_uint64() {
        auto hi = uint64_t{next_uint32()};
        auto lo = uint64_t{next_uint32()};
        return ((hi << 32) | lo);
    }

    // generate a uniformly distributed number: 0 <= r < bound
    [[nodiscard]] uint64_t next_uint64(uint64_t bound) {
        assert(bound != 0 && "next_uint64: bound is 0");

        [[assume(bound != 0)]];

        // Rejection threshold: (2^64 - bound) % bound, via unsigned wraparound.
        const auto threshold = (uint64_t{} - bound) % bound;
        while (true) {
            auto r = next_uint64();
            // drop values less than threshold to avoid bias
            if (r >= threshold) {
                return (r % bound);
            }
        }
    }

    // generate a uniformly distributed unsigned 128-bit random number by
    // concatenating two independent 64-bit draws from the PCG32 stream
    [[nodiscard]] uint128_t next_uint128() {
        auto hi = uint128_t{next_uint64()};
        auto lo = uint128_t{next_uint64()};
        return ((hi << 64) | lo);
    }

    // generate a uniformly distributed number: 0 <= r < bound
    [[nodiscard]] uint128_t next_uint128(uint128_t bound) {
        assert(bound != 0 && "next_uint128: bound is 0");

        [[assume(bound != 0)]];

        // Rejection threshold: (2^128 - bound) % bound, via unsigned wraparound.
        const auto threshold = (uint128_t{} - bound) % bound;
        while (true) {
            auto r = next_uint128();
            // drop values less than threshold to avoid bias
            if (r >= threshold) {
                return (r % bound);
            }
        }
    }

    // generate a float on the interval [0, 1)
    [[nodiscard]] float next_float() {
        // IEEE 754 single: >> 9 produces 23 random mantissa bits, OR with 1.0f
        // exponent (0x3F800000) gives a uniform value in [1.0, 2.0), subtract 1.
        constexpr uint32_t FLOAT_ONE_BITS{0x3F800000u};

        return (std::bit_cast<float>((next_uint32() >> 9) | FLOAT_ONE_BITS) - 1.0f);
    }

    // generate a double on the interval [0, 1)
    [[nodiscard]] double next_double() {
        // IEEE 754 double: >> 12 produces 52 random mantissa bits, OR with 1.0
        // exponent (0x3FF0...) gives a uniform value in [1.0, 2.0), subtract 1.
        constexpr uint64_t DOUBLE_ONE_BITS{0x3FF0000000000000ull};

        return (std::bit_cast<double>((next_uint64() >> 12) | DOUBLE_ONE_BITS) - 1.0);
    }

    [[nodiscard]] std::pair<uint64_t, uint64_t> snapshot_state() const { return std::pair{m_state, m_inc}; }

    void thaw_state(uint64_t state, uint64_t inc) {
        m_state = state;
        m_inc = inc;
    }
private:
    uint64_t m_state;
    uint64_t m_inc;
};
