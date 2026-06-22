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

//===--- Public Host Program API ---===//
//
// Additional host program API defined in their cohesive units:
//   verify.inl    -- verify_operands() and Verify* constants
//   object.inl    -- Object::make_*() factories and value accessors
//   init.inl      -- Constructors and destructor
//   types.inl     -- Config, ParseResult, Operator, Error, interrupt constants
//
public:
//--- Pre-construction: command-line parsing ---//
// Parse argc/argv into a Config and vm_size via getopt_long.
// Handles --help, --version, --about (sets should_exit).
// Validates numeric arguments against per-option bounds: most use named Min/Max
// constants; quantum/max-ops/sleep-budget use literal limits, and name-buckets
// clamps to its prime-table ends then rounds up to the next prime in that table.
// Numeric values > uint8_t accept K, M, G suffixes (x1024, x1048576, x1073741824).
static ParseResult parse_args(int argc, char *argv[]) {
    ParseResult result;
    Config &cfg = result.config;

    // Long-only option IDs (above ASCII range to avoid short-option collisions).
    enum LongOpt : int {
        OptAbout = 256,
        OptStdIn,
        OptVmSize,
        OptOperandDepth,
        OptExecDepth,
        OptDictDepth,
        OptErrorDepth,
        OptScratchDepth,
        OptSaveDepth,
        OptStreamCount,
        OptStreamBuffer,
        OptStreamIO,
        OptNameBuckets,
        OptUserdictSize,
        OptEqString,
        OptEqArray,
        OptEqProc,
        OptEqDict,
        OptEqSet,
        OptSandbox,
        OptNoBanner,
        OptResident,
        OptQuantum,
        OptMaxOps,
        OptSleepBudget,
        OptModulePath,
        OptErrorCodes,
#ifdef TRIX_DEBUGGER
        OptInspect,
        OptInspectOnError,
        OptInspectAt,
        OptNoColor,
#endif
        OptTestEqgenPreload,
    };

    static const option long_options[] = {
            {              "help",       no_argument, nullptr,                 'h'},
            {           "version",       no_argument, nullptr,                 'v'},
#ifdef TRIX_DEBUGGER
            {             "debug",       no_argument, nullptr,                 'd'},
#endif
            {           "stdedit",       no_argument, nullptr,                 'i'},
            {       "interactive",       no_argument, nullptr,                 'i'},
            {             "image",       no_argument, nullptr,                 'l'},
            {             "stdin",       no_argument, nullptr,            OptStdIn},
            {             "quiet",       no_argument, nullptr,                 'q'},
            {         "no-banner",       no_argument, nullptr,         OptNoBanner},
            {           "sandbox",       no_argument, nullptr,          OptSandbox},
            {          "resident",       no_argument, nullptr,         OptResident},
            {             "about",       no_argument, nullptr,            OptAbout},
            {       "error-codes",       no_argument, nullptr,       OptErrorCodes},
            {           "vm-size", required_argument, nullptr,           OptVmSize},
            {     "operand-depth", required_argument, nullptr,     OptOperandDepth},
            {        "exec-depth", required_argument, nullptr,        OptExecDepth},
            {        "dict-depth", required_argument, nullptr,        OptDictDepth},
            {       "error-depth", required_argument, nullptr,       OptErrorDepth},
            {     "scratch-depth", required_argument, nullptr,     OptScratchDepth},
            {        "save-depth", required_argument, nullptr,        OptSaveDepth},
            {      "stream-count", required_argument, nullptr,      OptStreamCount},
            {     "stream-buffer", required_argument, nullptr,     OptStreamBuffer},
            {         "stream-io", required_argument, nullptr,         OptStreamIO},
            {      "name-buckets", required_argument, nullptr,      OptNameBuckets},
            {     "userdict-size", required_argument, nullptr,     OptUserdictSize},
            {         "eq-string", required_argument, nullptr,         OptEqString},
            {          "eq-array", required_argument, nullptr,          OptEqArray},
            {           "eq-proc", required_argument, nullptr,           OptEqProc},
            {           "eq-dict", required_argument, nullptr,           OptEqDict},
            {            "eq-set", required_argument, nullptr,            OptEqSet},
            {           "quantum", required_argument, nullptr,          OptQuantum},
            {           "max-ops", required_argument, nullptr,           OptMaxOps},
            {      "sleep-budget", required_argument, nullptr,      OptSleepBudget},
            {       "module-path", required_argument, nullptr,       OptModulePath},
#ifdef TRIX_DEBUGGER
            {           "inspect",       no_argument, nullptr,          OptInspect},
            {  "inspect-on-error",       no_argument, nullptr,   OptInspectOnError},
            {        "inspect-at", required_argument, nullptr,        OptInspectAt},
            {          "no-color",       no_argument, nullptr,          OptNoColor},
#endif
            {"test-eqgen-preload", required_argument, nullptr, OptTestEqgenPreload},
            {             nullptr,                 0, nullptr,                   0},
    };

    // Parse a numeric string with optional K/M/G suffix into uint64_t.
    // Returns false on parse error.
    auto parse_scaled = [](const char *str, uint64_t &out) -> bool {
        char *end = nullptr;
        errno = 0;
        uint64_t val = std::strtoull(str, &end, 10);
        if ((end == str) || (errno == ERANGE)) {
            return false;
        } else if ((*end == 'K') || (*end == 'k')) {
            if (val > (std::numeric_limits<uint64_t>::max() / 1024)) {
                return false;
            } else {
                val *= 1024;
                ++end;
            }
        } else if ((*end == 'M') || (*end == 'm')) {
            if (val > (std::numeric_limits<uint64_t>::max() / (1024ULL * 1024))) {
                return false;
            } else {
                val *= (1024ULL * 1024);
                ++end;
            }
        } else if ((*end == 'G') || (*end == 'g')) {
            if (val > (std::numeric_limits<uint64_t>::max() / (1024ULL * 1024 * 1024))) {
                return false;
            } else {
                val *= (1024ULL * 1024 * 1024);
                ++end;
            }
        }
        if (*end != '\0') {
            return false;
        } else {
            out = val;
            return true;
        }
    };

    // Parse and clamp a scaled numeric argument to [min, max].
    // Prints error and sets exit on failure; returns clamped value on success.
    auto parse_clamped = [&](const char *name, const char *str, uint64_t min_val, uint64_t max_val) -> uint64_t {
        uint64_t val = 0;
        if (!parse_scaled(str, val)) {
            std::println(stderr, "trix: --{}: invalid value '{}'", name, str);
            result.should_exit = true;
            result.exit_code = 1;
            return min_val;
        } else if (val < min_val) {
            std::println(stderr, "trix: --{}: {} is below minimum {}", name, val, min_val);
            val = min_val;
        }
        if (val > max_val) {
            std::println(stderr, "trix: --{}: {} exceeds maximum {}", name, val, max_val);
            val = max_val;
        }
        return val;
    };

    // Parse a plain integer without K/M/G (for uint8_t fields).
    auto parse_plain = [&](const char *name, const char *str, unsigned min_val, unsigned max_val) -> unsigned {
        char *end = nullptr;
        errno = 0;
        auto val = std::strtoul(str, &end, 10);
        if ((end == str) || (*end != '\0') || (errno == ERANGE)) {
            std::println(stderr, "trix: --{}: invalid value '{}'", name, str);
            result.should_exit = true;
            result.exit_code = 1;
            return min_val;
        } else if (val < min_val) {
            std::println(stderr, "trix: --{}: {} is below minimum {}", name, val, min_val);
            val = min_val;
        }
        if (val > max_val) {
            std::println(stderr, "trix: --{}: {} exceeds maximum {}", name, val, max_val);
            val = max_val;
        }
        return static_cast<unsigned>(val);
    };

    // Format a byte size with K/M/G suffix for display.
    // Values within 1K of a G/M/K boundary are rounded (e.g., 4G-1 -> "4G").
    // Caller-supplied stack buffer keeps this heap-free; returns a view
    // into the buffer.  Buffer must outlive the view.
    auto format_size = [](uint64_t val, std::span<char> buf) -> std::string_view {
        constexpr uint64_t G = 1024ULL * 1024 * 1024;
        constexpr uint64_t M = 1024ULL * 1024;
        constexpr uint64_t K = 1024;
        // Pick suffix and divisor up front so a single format_to_n call
        // covers all cases; passing std::format_string through a generic
        // helper trips C++23's immediate-function escalation rules.
        char suffix = '\0';
        uint64_t scaled = val;
        if ((val >= G) && ((((val + K - 1) / G * G) - val) < K)) {
            suffix = 'G';
            scaled = (val + K - 1) / G;
        } else if ((val >= M) && ((val % M) == 0)) {
            suffix = 'M';
            scaled = val / M;
        } else if ((val >= K) && ((val % K) == 0)) {
            suffix = 'K';
            scaled = val / K;
        }
        auto n = static_cast<std::ptrdiff_t>(buf.size());
        auto r = (suffix == '\0') ? std::format_to_n(buf.data(), n, "{}", scaled)
                                  : std::format_to_n(buf.data(), n, "{}{:c}", scaled, suffix);
        return std::string_view{buf.data(), static_cast<size_t>(r.out - buf.data())};
    };

    // Build provenance: distinguishes the slow default debug build (-O0,
    // sanitized, all features) from the optimized release build (-O3,
    // NDEBUG, no sanitizers).  Printed by both --version and --about, and
    // parsed by tests/run_all.sh to skip perf tests on a debug build.
    auto print_build_info = []() {
#if defined(NDEBUG)
        const char *mode = "optimized";
#else
        const char *mode = "debug";
#endif
#if defined(__SANITIZE_ADDRESS__)
        const char *sanitizers = "address";  // gcc
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
        const char *sanitizers = "address";  // clang
#else
        const char *sanitizers = "none";
#endif
#else
        const char *sanitizers = "none";
#endif
        std::print("build: {}; sanitizers: {}; features:", mode, sanitizers);
        if (HeapTracking) {
            std::print(" heap-tracking");
        }
        if (Debugger) {
            std::print(" debugger");
        }
        if (BacktraceEnabled) {
            std::print(" backtrace");
        }
        if (ZlibEnabled) {
            std::print(" zlib");
        }
        if (ReadlineEnabled) {
            std::print(" readline");
        }
        if (!HeapTracking && !Debugger && !BacktraceEnabled && !ZlibEnabled && !ReadlineEnabled) {
            std::print(" none");
        }
        std::print("\n");
    };

    auto print_version = [&]() {
        // The tagline is the name's origin story: Trixie is the cat in
        // concatenative (see README "Why Trix?").
        std::println("trix {}.{}.{} — the cat in concatenative", MAJOR, MINOR, PATCH);
        print_build_info();
    };

    auto print_about = [&]() {
        std::println("  ______    _\n"
                     " /_  __/___(_)_  __\n"
                     "  / / / __/ /\\ \\/ /       Stack-Based Interpreter & VM\n"
                     " / / / / / /  > \u00b7 <      C++23 \u00b7 Single-Header Library\n"
                     "/_/ /_/ /_/  /_/\\_\\     Copyright 2026 Mark Guidarelli\n"
                     "\n"
                     "Version {}.{}.{}  (snapshot format {})\n"
                     "Licensed under the Apache License, Version 2.0\n",
                     MAJOR,
                     MINOR,
                     PATCH,
                     SNAPSHOT_VERSION);
#if defined(__GNUC__) && !defined(__clang__)
        std::println("Compiler: GCC {}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(__clang__)
        std::println("Compiler: Clang {}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#endif
        std::println("Standard: C++{}", (__cplusplus / 100) % 100);
        print_build_info();
        std::print("\n");
        char vm_buf[16];
        char sb_buf[16];
        std::println("Default configuration:\n"
                     "  VM size:          {}\n"
                     "  Operand depth:    {}\n"
                     "  Execution depth:  {}\n"
                     "  Dictionary depth: {}\n"
                     "  Error depth:      {}\n"
                     "  Scratch depth:    {}\n"
                     "  Save depth:       {}\n"
                     "  Stream count:     {}\n"
                     "  Stream buffer:    {}\n"
                     "  Name buckets:     {}\n"
                     "  Userdict size:    {}\n"
                     "  Eq string:        {}\n"
                     "  Eq array:         {}\n"
                     "  Eq proc:          {}\n"
                     "  Eq dict:          {}\n"
                     "  Eq set:           {}",
                     format_size(DefaultVmSize, vm_buf),
                     DefaultOperandDepth,
                     DefaultExecutionDepth,
                     DefaultDictionaryDepth,
                     DefaultErrorDepth,
                     DefaultCoroutineScratchDepth,
                     DefaultSaveCount,
                     DefaultStreamCount,
                     format_size(DefaultStreamBufferSize, sb_buf),
                     DefaultNameBucketCount,
                     DefaultUserDictMaxLength,
                     DefaultEqStringLength,
                     DefaultEqArrayLength,
                     DefaultEqProcLength,
                     DefaultEqDictMaxLength,
                     DefaultEqSetMaxLength);
    };

    // Dump every Error enum entry as `code<TAB>name` (declaration order ==
    // process exit code; name from error_sv()).  This is the runtime single
    // source for the exit-code contract: tests/error_codes.sh consumes it so
    // tests resolve symbolic Error names instead of hardcoding the numbers,
    // and tests/check_error_codes.py asserts the doc catalog matches.
    auto print_error_codes = []() {
        for (int code = 0; code < ErrorCount; ++code) {
            std::println("{}\t{}", code, error_sv(static_cast<Error>(code)));
        }
    };

    auto print_usage = [&]() {
        char vm_def[16];
        char vm_min[16];
        char vm_max[16];
        char sb_def[16];
        char sb_min[16];
        char sb_max[16];
        std::println("Usage: trix [options] [filename] [script-args...]\n"
                     "\n"
                     "Tokens after [filename] are passed to the script via the\n"
                     "command-line-args operator and are NOT interpreted by trix.\n"
                     "\n"
                     "Options:\n"
                     "  -h, --help               Show this help message and exit\n"
                     "  -v, --version            Show version + build info and exit\n"
#ifdef TRIX_DEBUGGER
                     "  -d, --debug              Enable interactive debugger\n"
#endif
                     "  -i, --stdedit            Interactive REPL (default when no filename)\n"
                     "      --stdin              Read from standard input (for piped commands)\n"
                     "  -l, --image              Load snap-shot image instead of script\n"
                     "  -q, --quiet              Suppress the startup banner and diagnostic output\n"
                     "      --no-banner          Suppress only the startup banner (diagnostics kept;\n"
                     "                           for scripted interactive sessions)\n"
                     "      --sandbox            Disable filesystem, system, and raw memory ops\n"
                     "      --resident           Stay resident after the script or input ends:\n"
                     "                           wait for host-delivered work instead of exiting\n"
                     "      --about              Show extended version/build/config info\n"
                     "      --error-codes        List Error names with their exit codes and exit\n"
                     "\n"
#ifdef TRIX_DEBUGGER
                     "Interactive script debugger (lib/debugger.trx):\n"
                     "      --inspect            Step into script under interactive control from line 1\n"
                     "      --inspect-on-error   Run normally; drop into the inspector on first error\n"
                     "      --inspect-at=/NAME   Run normally; halt when /NAME is invoked\n"
                     "      --no-color           Disable ANSI colors in the inspector UI\n"
                     "\n"
#endif
                     "VM tuning (values accept K, M, G suffixes where noted):\n"
                     "      --vm-size=BYTES      VM heap size [{}] ({}..{})\n"
                     "      --operand-depth=N    Operand stack depth [{}] ({}..{})\n"
                     "      --exec-depth=N       Execution stack depth [{}] ({}..{})\n"
                     "      --dict-depth=N       Dictionary stack depth [{}] ({}..{})\n"
                     "      --error-depth=N      Error stack depth [{}] ({}..{})\n"
                     "      --scratch-depth=N    Per-coroutine scratch arena depth [{}] ({}..{})\n"
                     "      --save-depth=N       Save/restore depth [{}] ({}..{})\n"
                     "      --stream-count=N     Max open streams [{}] ({}..{})\n"
                     "      --stream-buffer=BYTES  Stream buffer size [{}] ({}..{})\n"
                     "      --stream-io=MODE     Stream I/O: none, all, or comma-separated\n"
                     "                             stdin,stdout,stderr,stdedit [all]\n"
                     "      --name-buckets=N     Name hash buckets [auto: {} at default VM size]\n"
                     "      --userdict-size=N    Userdict capacity [{}] ({}..{})\n"
                     "      --eq-string=N        Eq string buffer [{}] ({}..{})\n"
                     "      --eq-array=N         Eq array buffer [{}] ({}..{})\n"
                     "      --eq-proc=N          Eq proc buffer [{}] ({}..{})\n"
                     "      --eq-dict=N          Eq dict buffer [{}] ({}..{})\n"
                     "      --eq-set=N           Eq set buffer [{}] ({}..{})\n"
                     "      --quantum=N          Default coroutine quantum (0 = unlimited) [{}] (0..{})\n"
                     "      --max-ops=N          Execution-limit cap (0 = unlimited) [0] (0..{})\n"
                     "      --sleep-budget=N     Cumulative sleep/timeout grant, ms (0 = unlimited) [0] (0..{})\n"
                     "      --module-path=PATH   Colon-separated search path for require/require-module.\n"
                     "                             Binary-relative `lib/` is searched last.",
                     format_size(DefaultVmSize, vm_def),
                     format_size(MinVmSize, vm_min),
                     format_size(static_cast<uint64_t>(std::numeric_limits<vm_size_t>::max()), vm_max),
                     DefaultOperandDepth,
                     MinOperandDepth,
                     MaxOperandDepth,
                     DefaultExecutionDepth,
                     MinExecutionDepth,
                     MaxExecutionDepth,
                     DefaultDictionaryDepth,
                     MinDictionaryDepth,
                     MaxDictionaryDepth,
                     DefaultErrorDepth,
                     MinErrorDepth,
                     MaxErrorDepth,
                     DefaultCoroutineScratchDepth,
                     MinCoroutineScratchDepth,
                     MaxCoroutineScratchDepth,
                     DefaultSaveCount,
                     MinSaveCount,
                     MaxSaveCount,
                     DefaultStreamCount,
                     MinStreamCount,
                     MaxStreamCount,
                     format_size(DefaultStreamBufferSize, sb_def),
                     format_size(MinStreamBufferSize, sb_min),
                     format_size(MaxStreamBufferSize, sb_max),
                     DefaultNameBucketCount,
                     DefaultUserDictMaxLength,
                     MinUserDictMaxLength,
                     MaxUserDictMaxLength,
                     DefaultEqStringLength,
                     MinEqStringLength,
                     MaxEqStringLength,
                     DefaultEqArrayLength,
                     MinEqArrayLength,
                     MaxEqArrayLength,
                     DefaultEqProcLength,
                     MinEqProcLength,
                     MaxEqProcLength,
                     DefaultEqDictMaxLength,
                     MinEqDictMaxLength,
                     MaxEqDictMaxLength,
                     DefaultEqSetMaxLength,
                     MinEqSetMaxLength,
                     MaxEqSetMaxLength,
                     DefaultCoroutineQuantum,
                     1'000'000'000u,
                     std::numeric_limits<uint64_t>::max(),
                     std::numeric_limits<uint64_t>::max());
    };

    // Resolve binary directory (parent of argv[0]) into a static buffer so it
    // is stable across the process lifetime.  Used as the implicit `lib/`
    // fallback root for the module search path.  Lazy: resolved on first
    // call; left empty if resolution fails (caller treats nullptr as "no
    // binary-relative search").
    static char bin_dir_buf[PATH_MAX] = "";
    static char exe_path_buf[PATH_MAX] = "";
    if ((argc > 0) && (bin_dir_buf[0] == '\0')) {
        char resolved_argv0[PATH_MAX];
        if (::realpath(argv[0], resolved_argv0) != nullptr) {
            // Keep the FULL resolved binary path (executable-path surfaces it
            // so a snapshot/fork example can re-exec the SAME build); then
            // derive the parent directory for the lib/ search fallback.
            auto path_len = std::strlen(resolved_argv0);
            if (path_len < sizeof(exe_path_buf)) {
                std::memcpy(exe_path_buf, resolved_argv0, path_len + 1);
            }
            char *last_slash = std::strrchr(resolved_argv0, '/');
            if (last_slash != nullptr) {
                auto dir_len = static_cast<size_t>(last_slash - resolved_argv0);
                if (dir_len < (sizeof(bin_dir_buf) - 1)) {
                    std::memcpy(bin_dir_buf, resolved_argv0, dir_len);
                    bin_dir_buf[dir_len] = '\0';
                }
            }
        }
    }
    if (bin_dir_buf[0] != '\0') {
        cfg.m_bin_dir = bin_dir_buf;
    }
    if (exe_path_buf[0] != '\0') {
        cfg.m_exe_path = exe_path_buf;
    }

    optind = 1;  // reset getopt state for re-entrant safety
    int opt;
    auto interactive_requested = false;
    // Leading "+" disables option permutation: getopt stops at the first
    // non-option argument (the script filename), leaving any following
    // tokens in argv for command-line-args to surface to the script.
    // The 'd' short option (--debug) is spliced in only for debugger builds;
    // the long_options "debug" row is likewise gated, so a debugger-off build
    // rejects -d / --debug as an invalid option.
    while ((opt = getopt_long(argc,
                              argv,
#ifdef TRIX_DEBUGGER
                              "+hvdilq"
#else
                              "+hvilq"
#endif
                              ,
                              long_options,
                              nullptr)) != -1) {
        switch (opt) {
        case 'h':
            print_usage();
            result.should_exit = true;
            return result;

        case 'v':
            print_version();
            result.should_exit = true;
            return result;

        case OptAbout:
            print_about();
            result.should_exit = true;
            return result;

        case OptErrorCodes:
            print_error_codes();
            result.should_exit = true;
            return result;

#ifdef TRIX_DEBUGGER
        case 'd':
            cfg.m_debug = true;
            break;
#endif

        case 'i':
            interactive_requested = true;
            break;

        case 'l':
            cfg.m_mode = StartupMode::ImageFile;
            break;

        case 'q':
            cfg.m_quiet = true;
            break;

        case OptSandbox:
            cfg.m_sandbox = true;
            break;

        case OptNoBanner:
            cfg.m_no_banner = true;
            break;

        case OptResident:
            cfg.m_resident = true;
            break;

        case OptStdIn:
            cfg.m_mode = StartupMode::StdIn;
            break;

        case OptVmSize:
            result.vm_size = static_cast<vm_size_t>(parse_clamped("vm-size", optarg, MinVmSize, MaxVmSize));
            break;

        case OptOperandDepth:
            cfg.m_operand_depth =
                    static_cast<stack_depth_t>(parse_clamped("operand-depth", optarg, MinOperandDepth, MaxOperandDepth));
            break;

        case OptExecDepth:
            cfg.m_execution_depth =
                    static_cast<stack_depth_t>(parse_clamped("exec-depth", optarg, MinExecutionDepth, MaxExecutionDepth));
            break;

        case OptDictDepth:
            cfg.m_dictionary_depth =
                    static_cast<stack_depth_t>(parse_clamped("dict-depth", optarg, MinDictionaryDepth, MaxDictionaryDepth));
            break;

        case OptErrorDepth:
            cfg.m_error_depth = static_cast<stack_depth_t>(parse_clamped("error-depth", optarg, MinErrorDepth, MaxErrorDepth));
            break;

        case OptScratchDepth:
            cfg.m_scratch_depth = static_cast<stack_depth_t>(
                    parse_clamped("scratch-depth", optarg, MinCoroutineScratchDepth, MaxCoroutineScratchDepth));
            break;

        case OptSaveDepth:
            cfg.m_save_count = static_cast<save_level_t>(parse_plain("save-depth", optarg, MinSaveCount, MaxSaveCount));
            break;

        case OptStreamCount:
            cfg.m_stream_count = static_cast<stream_count_t>(parse_plain("stream-count", optarg, MinStreamCount, MaxStreamCount));
            break;

        case OptStreamBuffer:
            cfg.m_stream_buffer_size = static_cast<stream_buffer_size_t>(
                    parse_clamped("stream-buffer", optarg, MinStreamBufferSize, MaxStreamBufferSize));
            break;

        case OptStreamIO: {
            // Accepts: none, all, or comma-separated list of stdin,stdout,stderr,stdedit
            std::string_view arg{optarg};
            if (arg == "none") {
                cfg.m_stream_enable = StdIODisabled;
            } else if (arg == "all") {
                cfg.m_stream_enable = StdIOEnabled;
            } else {
                stream_enable_t bits = StdIODisabled;
                bool valid = true;
                while (!arg.empty() && valid) {
                    auto comma = arg.find(',');
                    auto token = arg.substr(0, comma);
                    arg = (comma == std::string_view::npos) ? std::string_view{} : arg.substr(comma + 1);
                    if (token == "stdin") {
                        bits |= StdInEnable;
                    } else if (token == "stdout") {
                        bits |= StdOutEnable;
                    } else if (token == "stderr") {
                        bits |= StdErrEnable;
                    } else if (token == "stdedit") {
                        bits |= StdEditEnable;
                    } else {
                        std::println(stderr,
                                     "trix: --stream-io: unknown '{}'"
                                     " (use none, all, or comma-separated: stdin,stdout,stderr,stdedit)",
                                     token);
                        result.should_exit = true;
                        result.exit_code = 1;
                        valid = false;
                    }
                }
                if (valid) {
                    cfg.m_stream_enable = bits;
                }
            }
            break;
        }

        case OptNameBuckets: {
            const uint64_t raw = parse_clamped(
                    "name-buckets", optarg, sm_name_bucket_counts[0], sm_name_bucket_counts[std::size(sm_name_bucket_counts) - 1]);
            // Round up to the smallest prime >= raw in the bucket table.
            name_bucket_count_t best = sm_name_bucket_counts[0];
            for (auto b : sm_name_bucket_counts) {
                if (b >= raw) {
                    best = b;
                    break;
                } else {
                    best = b;
                }
            }
            cfg.m_name_bucket_count = best;
            break;
        }

        case OptUserdictSize:
            cfg.m_userdict_maxlength =
                    static_cast<length_t>(parse_clamped("userdict-size", optarg, MinUserDictMaxLength, MaxUserDictMaxLength));
            break;

        case OptEqString:
            cfg.m_eqstring_length = static_cast<length_t>(parse_clamped("eq-string", optarg, MinEqStringLength, MaxEqStringLength));
            break;

        case OptEqArray:
            cfg.m_eqarray_length = static_cast<length_t>(parse_clamped("eq-array", optarg, MinEqArrayLength, MaxEqArrayLength));
            break;

        case OptEqProc:
            cfg.m_eqproc_length = static_cast<length_t>(parse_clamped("eq-proc", optarg, MinEqProcLength, MaxEqProcLength));
            break;

        case OptEqDict:
            cfg.m_eqdict_maxlength =
                    static_cast<length_t>(parse_clamped("eq-dict", optarg, MinEqDictMaxLength, MaxEqDictMaxLength));
            break;

        case OptEqSet:
            cfg.m_eqset_maxlength = static_cast<length_t>(parse_clamped("eq-set", optarg, MinEqSetMaxLength, MaxEqSetMaxLength));
            break;

        case OptQuantum:
            cfg.m_coroutine_quantum = static_cast<uint32_t>(parse_clamped("quantum", optarg, 0, 1'000'000'000));
            break;

        case OptMaxOps:
            cfg.m_max_ops = parse_clamped("max-ops", optarg, 0, std::numeric_limits<uint64_t>::max());
            break;

        case OptSleepBudget:
            cfg.m_sleep_budget_ms = parse_clamped("sleep-budget", optarg, 0, std::numeric_limits<uint64_t>::max());
            break;

        case OptModulePath:
            cfg.m_module_path = optarg;
            break;

#ifdef TRIX_DEBUGGER
        case OptInspect:
            cfg.m_mode = StartupMode::InspectFile;
            break;

        case OptInspectOnError:
            cfg.m_mode = StartupMode::InspectFile;
            cfg.m_inspect_on_error = true;
            break;

        case OptInspectAt:
            cfg.m_mode = StartupMode::InspectFile;
            cfg.m_inspect_at_name = optarg;
            break;

        case OptNoColor:
            cfg.m_no_color = true;
            break;
#endif

        case OptTestEqgenPreload:
            // Internal test hook: preload all five eq-storage generation counters so the
            // 2^32-wrap LimitCheck path can be exercised without performing 2^32 creations.
            // Intentionally absent from --help.
            cfg.m_test_eqgen_preload =
                    static_cast<uint32_t>(parse_clamped("test-eqgen-preload", optarg, 0, std::numeric_limits<uint32_t>::max()));
            break;

        default:
            // getopt_long already printed an error message.
            result.should_exit = true;
            result.exit_code = 1;
            return result;
        }

        if (result.should_exit) {
            return result;
        }
    }

    // Remaining non-option argument is the filename.
    if (optind < argc) {
        cfg.m_filename = argv[optind];

        // Anything after the filename becomes the script's argv tail,
        // surfaced via the command-line-args operator.  parse_args does NOT
        // try to interpret these flags -- the .trx script does.
        if ((optind + 1) < argc) {
            cfg.m_script_argv = &argv[optind + 1];
            cfg.m_script_argc = argc - (optind + 1);
        }
    }

    // Resolve startup mode from flags and filename.
    // --stdin is mutually exclusive with -i and with a filename.
    if (cfg.m_mode == StartupMode::StdIn) {
        if (interactive_requested) {
            std::println(stderr, "trix: --stdin and -i/--stdedit are mutually exclusive");
            result.should_exit = true;
            result.exit_code = 1;
        } else if (cfg.m_filename != nullptr) {
            std::println(stderr, "trix: --stdin and filename are mutually exclusive");
            result.should_exit = true;
            result.exit_code = 1;
        }
    }
#ifdef TRIX_DEBUGGER
    else if (cfg.m_mode == StartupMode::InspectFile) {
        // --inspect requires a script filename and is mutually exclusive
        // with -i / --image.  Debugger-only: the --inspect flags that set
        // InspectFile mode are gated out of non-debugger builds (api.inl),
        // so this arm is unreachable -- and compiled out -- in release.
        if (cfg.m_filename == nullptr) {
            std::println(stderr, "trix: --inspect / --inspect-on-error / --inspect-at requires a script filename");
            result.should_exit = true;
            result.exit_code = 1;
        } else if (interactive_requested) {
            std::println(stderr, "trix: --inspect and -i/--stdedit are mutually exclusive");
            result.should_exit = true;
            result.exit_code = 1;
        }
    }
#endif
    else if (cfg.m_mode == StartupMode::ImageFile) {
        // -l/--image thaws a frozen VM image and resumes its frozen exec
        // stack, so it needs the image filename and cannot combine with
        // an interactive session.
        if (cfg.m_filename == nullptr) {
            std::println(stderr, "trix: -l/--image requires an image filename");
            result.should_exit = true;
            result.exit_code = 1;
        } else if (interactive_requested) {
            std::println(stderr, "trix: --image and -i/--stdedit are mutually exclusive");
            result.should_exit = true;
            result.exit_code = 1;
        }
    } else {
        // Not --stdin, not --image: resolve ScriptFile vs Interactive vs FileAndInteractive
        if (interactive_requested && (cfg.m_filename != nullptr)) {
            cfg.m_mode = StartupMode::FileAndInteractive;
        } else if (interactive_requested) {
            cfg.m_mode = StartupMode::Interactive;
        } else if (cfg.m_filename == nullptr) {
            // No filename, no flags -> default to interactive REPL
            cfg.m_mode = StartupMode::Interactive;
        }
        // else: filename without -i -> ScriptFile (default)
    }

    return result;
}

//--- Diagnostic output helpers ---//
// Short-circuit wrappers around std::println(stderr, ...) / std::print(stderr, ...).
// When m_quiet is set (e.g. fuzz harness, --quiet host), these no-op before
// building format arguments -- saves the formatting work, not just the write.
// The caller is still responsible for guarding expensive argument assembly
// (e.g. format_backtrace walks the exec stack -- guard with `if (!m_quiet)`).
template<typename... T>
void diag_println(std::format_string<T...> format, T &&...args) const {
    if (!m_quiet) {
        std::println(stderr, format, std::forward<T>(args)...);
    }
}

template<typename... T>
void diag_print(std::format_string<T...> format, T &&...args) const {
    if (!m_quiet) {
        std::print(stderr, format, std::forward<T>(args)...);
    }
}

//--- User operator API: error reporting ---//
// Raise an error with a formatted message.
//
// With scanner context (m_scanner_stream != nullptr):
//   Input:   << null 2084 >>
//   Message: Trix type-check 'scanner': test_file.trx:8:18: 'null' is not a valid 'dict' key
//
// Without scanner context:
//   Input:   null (LilTrix) def
//   Message: Trix type-check 'def': operand #2 'null' is not a valid 'dict' key
template<typename... T>
[[noreturn]] void error(Error err, std::format_string<T...> format, T &&...args) {
    // During VM initialization, m_error_string_base may not be allocated yet.
    // Bypass the format path and delegate to the guarded error(Error, Object).
    if (!m_error_init_complete) {
        m_last_error = err;
        diag_println("Trix {}: error during VM initialization (VM heap too small?)", error_sv(err));
        throw Exception::Error;
    }

    auto trx = this;
    length_t error_length = 0;

    if (m_scanner_stream != nullptr) {
        auto [source, line_number, line_pos] = m_scanner_stream->source_location(trx);

        auto [prefix_ptr, prefix_length] =
                std::format_to_n(m_error_string_base, MaxErrorLength, "{}:{}:{}: ", source, line_number, line_pos);
        // clamp: format_to_n returns the would-be total size, which may exceed MaxErrorLength
        auto clamped_prefix = std::min(prefix_length, static_cast<decltype(prefix_length)>(MaxErrorLength));
        auto [_, message_length] = std::format_to_n(m_error_string_base + clamped_prefix,
                                                    static_cast<std::ptrdiff_t>(MaxErrorLength - clamped_prefix),
                                                    format,
                                                    std::forward<T>(args)...);
        error_length = static_cast<length_t>(
                std::min(clamped_prefix + message_length, static_cast<decltype(message_length)>(MaxErrorLength)));
    } else {
        auto [_, message_length] = std::format_to_n(m_error_string_base, MaxErrorLength, format, std::forward<T>(args)...);
        error_length = static_cast<length_t>(std::min(message_length, static_cast<decltype(message_length)>(MaxErrorLength)));
    }

    m_error_string_base[error_length] = '\0';
    auto offset = ptr_to_offset(m_error_string_base);
    auto error_string = Object::make_string(offset, error_length, Object::LiteralAttrib, Object::ReadOnlyAccess);
    error(err, error_string);
}

//--- Thread-safe interaction with a running Trix instance ---//
[[nodiscard]] bool raise_interrupt(interrupt_t irq) {
    if (std::popcount(irq) == 1) {
        const std::unique_lock<std::mutex> lk(m_mutex);
        // ErrorIRQ and InvokeIRQ are excluded: they carry additional protocol state
        // (m_external_error and m_invoke_data/length) that must be set atomically
        // alongside the IRQ bit; use raise_error() and invoke() for those.
        auto mask = (Level0IRQ | Level1IRQ | Level2IRQ | SuspendIRQ | ResumeIRQ | ExitIRQ);
        if ((irq & mask) != 0) {
            // Always record the interrupt, even when masked.  Masking defers
            // delivery (in process_interrupt), not raising.  This ensures
            // interrupts raised while disabled are delivered after re-enabling,
            // and ExitIRQ from the destructor is never lost.
            m_interrupt |= irq;
            m_pending_irq.store(actionable_interrupts(), std::memory_order_relaxed);
            m_cond.notify_one();
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool raise_error(Error error) {
    const std::unique_lock<std::mutex> lk(m_mutex);
    // Always record, even when masked.  The single-slot guard
    // (m_external_error == NoError) prevents overwriting a pending error.
    auto valid = ((error != Error::NoError) && (m_external_error == Error::NoError));
    if (valid) {
        m_interrupt |= ErrorIRQ;
        m_external_error = error;
        m_pending_irq.store(actionable_interrupts(), std::memory_order_relaxed);
        m_cond.notify_one();
    }
    return valid;
}

[[nodiscard]] bool invoke(const void *data, size_t length) {
    // Probe one element to verify the pointer is valid and readable.
    // Checking sizeof(vm_t) bytes is sufficient -- the caller is responsible
    // for keeping the full [data, data+length) buffer alive and accessible
    // until the invocation completes (i.e. until the memory stream is fully
    // consumed by the interpreter, which may be on a subsequent iteration).
    auto state = address_state(data, sizeof(vm_t));
    if ((state == AddressState::IsReadOnly) || (state == AddressState::IsReadWrite)) {
        const std::unique_lock<std::mutex> lk(m_mutex);
        // Always record, even when masked.  The single-slot guard
        // (InvokeIRQ not already pending) prevents overwriting the payload.
        if ((m_interrupt & InvokeIRQ) == 0) {
            m_invoke_data = static_cast<const vm_t *>(data);
            m_invoke_length = length;
            m_interrupt |= InvokeIRQ;
            m_pending_irq.store(actionable_interrupts(), std::memory_order_relaxed);
            m_cond.notify_one();
            return true;
        }
    }
    return false;
}
