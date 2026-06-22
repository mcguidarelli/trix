// fuzz_trix.cpp -- libFuzzer harness for Trix interpreter.
//
// Feeds fuzz data as a Trix script via stdin with sandbox mode enabled.
// Exercises the full pipeline: lexer, scanner, number/string/name parsing,
// binary token decoding, operator dispatch, stack operations, array/dict/
// string/set allocation, name lookup, save/restore, error handling,
// coroutine scheduling, and packed array encoding/decoding.
//
// Build:  see build.sh (requires clang with -fsanitize=fuzzer)
// Run:    ./fuzz/run.sh                          # default: max_len=4096
//         ./fuzz/run.sh -max_len=65536           # larger inputs

#include <cstdlib>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../trix.h"

static constexpr size_t kMaxInput = 64 * 1024;

// VM heap size, configurable via TRIX_FUZZ_VM_SIZE environment variable.
// Accepts plain bytes or suffixes: K (1024), M (1048576).
// Default: MinVmSize (256KB).  Clamped to [MinVmSize, MaxVmSize].
// Examples: 256K, 1M, 2M, 10M.
static Trix::vm_size_t parse_vm_size() {
    const char *env = std::getenv("TRIX_FUZZ_VM_SIZE");
    if (!env) {
        return Trix::MinVmSize;
    } else {
        char *end = nullptr;
        size_t val = std::strtoull(env, &end, 10);
        // strtoull always writes through a non-null endptr (to the first
        // unconverted char, or to env itself), so `end` is never null here.
        if (*end == 'K' || *end == 'k') {
            val *= 1024;
        } else if (*end == 'M' || *end == 'm') {
            val *= 1024 * 1024;
        }
        // Clamp to BOTH bounds before the uint32 narrowing, so an env value
        // >= 4G (or a wrapped multiply) cannot truncate to a tiny/zero size and
        // trip the constructor's vm_size precondition (which throws, uncaught,
        // aborting the campaign).  Mirrors api.inl's parse_clamped contract.
        if (val > Trix::MaxVmSize) {
            val = Trix::MaxVmSize;
        }
        if (val < Trix::MinVmSize) {
            val = Trix::MinVmSize;
        }
        return static_cast<Trix::vm_size_t>(val);
    }
}

static const Trix::vm_size_t kVmSize = parse_vm_size();

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if ((size == 0) || (size > kMaxInput)) {
        return 0;
    } else {
        // Create a memfd with the fuzz data and redirect stdin.
        auto saved_stdin = ::dup(STDIN_FILENO);
        auto fd = ::memfd_create("fuzz_trix", 0);
        if (fd < 0) {
            ::close(saved_stdin);
            return 0;
        } else {
            static_cast<void>(::write(fd, data, size));
            ::lseek(fd, 0, SEEK_SET);
            ::dup2(fd, STDIN_FILENO);
            ::close(fd);

            // Suppress Trix stdout and stderr (error backtraces + program output).
            auto saved_stdout = ::dup(STDOUT_FILENO);
            auto saved_stderr = ::dup(STDERR_FILENO);
            auto devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }

            // Construct Trix instance: sandbox mode, stdin input, bounded execution.
            Trix::Config cfg{};
            cfg.m_mode = Trix::StartupMode::StdIn;
            cfg.m_sandbox = true;
            cfg.m_quiet = true;
            cfg.m_stream_count = 2;
            cfg.m_max_ops = 1'000'000;    // prevent infinite loops from dominating fuzzer time
            cfg.m_sleep_budget_ms = 500;  // max_ops cannot tick while parked: bound wall-clock
                                          // sleeps too, or one huge sleep operand stalls a unit
                                          // until libFuzzer's -timeout flags a false positive

            // Trix constructor runs interpreter() synchronously and catches all exceptions.
            Trix trx(kVmSize, cfg);

            // Restore stdin, stdout, and stderr for the next iteration.
            ::dup2(saved_stdout, STDOUT_FILENO);
            ::close(saved_stdout);
            ::dup2(saved_stderr, STDERR_FILENO);
            ::close(saved_stderr);
            ::dup2(saved_stdin, STDIN_FILENO);
            ::close(saved_stdin);
        }
    }

    return 0;
}
