// Micro-benchmark: FNV-1a vs wyhash for Trix string/name hashing.
// Build: g++-15 -O2 -std=c++23 bench_hash.cpp -o bench_hash
// Run:   ./bench_hash

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using hash_t = uint32_t;

// ---- FNV-1a (current) ------------------------------------------------------
static hash_t fnv1a32_void(const void *data, size_t size) {
    hash_t value = 0x811C9DC5U;
    auto ptr = static_cast<const uint8_t *>(data);
    while (size != 0) {
        value = (value ^ static_cast<hash_t>(*ptr)) * 0x01000193U;
        ++ptr;
        --size;
    }
    return value;
}
static hash_t fnv1a32_sv(std::string_view sv) {
    return fnv1a32_void(sv.data(), sv.size());
}

// ---- wyhash (proposed) -----------------------------------------------------
static uint64_t wy_read8(const char *p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
static uint64_t wy_read4(const char *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return static_cast<uint64_t>(v);
}
static uint64_t wy_mum(uint64_t a, uint64_t b) {
    __uint128_t r = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
    return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
}
static hash_t wyhash32_sv(std::string_view sv) {
    constexpr uint64_t S0 = 0xa0761d6478bd642fULL;
    constexpr uint64_t S1 = 0xe7037ed1a0b428dbULL;
    auto p = sv.data();
    auto n = sv.size();
    uint64_t seed = S0;
    uint64_t a;
    uint64_t b;
    if (n <= 16) {
        if (n >= 4) {
            size_t mid = (n >> 3) << 2;
            a = (wy_read4(p) << 32) | wy_read4(p + mid);
            b = (wy_read4(p + n - 4) << 32) | wy_read4(p + n - 4 - mid);
        } else if (n > 0) {
            a = (static_cast<uint64_t>(static_cast<uint8_t>(p[0])) << 16) |
                (static_cast<uint64_t>(static_cast<uint8_t>(p[n >> 1])) << 8) |
                static_cast<uint64_t>(static_cast<uint8_t>(p[n - 1]));
            b = 0;
        } else {
            a = 0;
            b = 0;
        }
    } else {
        size_t i = n;
        while (i > 16) {
            seed = wy_mum(wy_read8(p) ^ S1, wy_read8(p + 8) ^ seed);
            p += 16;
            i -= 16;
        }
        a = wy_read8(p + i - 16);
        b = wy_read8(p + i - 8);
    }
    uint64_t mixed = wy_mum(S1 ^ n, wy_mum(a ^ S1, b ^ seed));
    return static_cast<hash_t>(mixed ^ (mixed >> 32));
}

// ---- bench harness ---------------------------------------------------------
struct Result {
    double ns_total;
    double ns_per_hash;
    double mb_per_sec;
    uint64_t sink;
};

template<hash_t (*Fn)(std::string_view)>
Result time_fn(const std::vector<std::string> &pool, size_t passes, size_t total_bytes) {
    uint64_t sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t p = 0; p < passes; ++p) {
        for (const auto &s : pool) {
            sink += Fn(s);
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    size_t hashes = pool.size() * passes;
    size_t bytes = total_bytes * passes;
    return Result{ns, ns / hashes, (bytes / 1e6) / (ns / 1e9), sink};
}

void run_suite(const char *label, int len_min, int len_max, size_t n_strings, size_t passes) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> len_dist(len_min, len_max);
    std::uniform_int_distribution<int> ch_dist('a', 'z');

    std::vector<std::string> pool;
    pool.reserve(n_strings);
    size_t total_bytes = 0;
    for (size_t i = 0; i < n_strings; ++i) {
        int len = len_dist(rng);
        std::string s;
        s.reserve(len);
        for (int j = 0; j < len; ++j) {
            s.push_back(static_cast<char>(ch_dist(rng)));
        }
        total_bytes += len;
        pool.push_back(std::move(s));
    }

    // warm-up
    uint64_t warm = 0;
    for (const auto &s : pool) {
        warm += fnv1a32_sv(s);
        warm += wyhash32_sv(s);
    }

    // best of 3 to tame noise
    Result fnv_best{1e18, 0, 0, 0};
    Result wy_best{1e18, 0, 0, 0};
    for (int run = 0; run < 3; ++run) {
        auto r1 = time_fn<fnv1a32_sv>(pool, passes, total_bytes);
        auto r2 = time_fn<wyhash32_sv>(pool, passes, total_bytes);
        if (r1.ns_total < fnv_best.ns_total) {
            fnv_best = r1;
        }
        if (r2.ns_total < wy_best.ns_total) {
            wy_best = r2;
        }
    }

    double avg_len = static_cast<double>(total_bytes) / n_strings;
    std::printf(
            "\n== %s  (len %d-%d, avg %.1f, %zu strings x %zu passes) ==\n", label, len_min, len_max, avg_len, n_strings, passes);
    std::printf("  FNV-1a:  %.1f ns/hash   %.2f MB/s   (sink=%lx)\n", fnv_best.ns_per_hash, fnv_best.mb_per_sec, fnv_best.sink);
    std::printf("  wyhash:  %.1f ns/hash   %.2f MB/s   (sink=%lx)\n", wy_best.ns_per_hash, wy_best.mb_per_sec, wy_best.sink);
    std::printf("  speedup: %.2fx\n", fnv_best.ns_per_hash / wy_best.ns_per_hash);
    (void)warm;
}

int main() {
    std::printf("Hash micro-benchmark (lower ns/hash = better)\n");
    run_suite("tiny keys   (operator names)", 3, 8, 10000, 2000);
    run_suite("short keys  (typical names)", 3, 16, 10000, 2000);
    run_suite("medium keys (dict keys)", 8, 32, 10000, 1000);
    run_suite("long keys   (strings)", 32, 128, 10000, 500);
    return 0;
}
