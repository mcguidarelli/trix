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
public:
using int8_t = ::std::int8_t;
using int16_t = ::std::int16_t;
using int32_t = ::std::int32_t;
using int64_t = ::std::int64_t;
using uint8_t = ::std::uint8_t;
using uint16_t = ::std::uint16_t;
using uint32_t = ::std::uint32_t;
using uint64_t = ::std::uint64_t;
using uintptr_t = ::std::uintptr_t;

using vm_t = uint8_t;
using length_t = uint16_t;

using boolean_t = bool;
using byte_t = uint8_t;
using integer_t = int32_t;
using uinteger_t = uint32_t;
using long_t = long long;
using ulong_t = unsigned long long;
// Trix relies on the compiler's 128-bit integer extension (GCC/Clang).
// 32-bit targets and MSVC are not supported; this matches the codebase's
// other GCC/Clang assumptions (sanitizers, --gc-sections, -fno-rtti, etc.).
static_assert(__SIZEOF_INT128__ == 16,
              "Trix requires GCC or Clang with __int128 support; 32-bit targets and MSVC are unsupported.");
__extension__ using int128_t = __int128;
__extension__ using uint128_t = unsigned __int128;
using address_t = void *;
using real_t = float;
using double_t = double;

// DebugState lives in src/ops_debugger.inl (after object.inl is included)
// because it carries an Object value member.  Forward-declared here is
// not needed -- the only references in this file's downstream consumers
// are in ops_debugger.inl, init.inl, and member_vars.inl, all of which
// include after object.inl + ops_debugger.inl.

using operator_func_t = void (*)(Trix *trx);
struct Operator {
    operator_func_t m_func{nullptr};
    std::string_view m_sv{};
};

// primary template
[[nodiscard]] static length_t sv_length(std::string_view sv) {
    if (sv.length() > std::numeric_limits<length_t>::max()) {
        std::println(stderr, "Trix sv_length: length {} exceeds maximum {}", sv.length(), std::numeric_limits<length_t>::max());
        std::abort();
    } else {
        return static_cast<length_t>(sv.length());
    }
}

[[nodiscard]] static std::string_view make_sv(void *ptr, length_t length) {
    return std::string_view{reinterpret_cast<const char *>(ptr), static_cast<size_t>(length)};
}

[[nodiscard]] static std::string_view make_sv(const void *ptr, size_t length) {
    return std::string_view{reinterpret_cast<const char *>(ptr), length};
}

using stream_enable_t = uint8_t;
static constexpr stream_enable_t StdIODisabled{0x00};
static constexpr stream_enable_t StdInEnable{0x01};    // vm usage: (sizeof(Stream) + StdIoBufferSize)
static constexpr stream_enable_t StdEditEnable{0x02};  // vm usage: sizeof(Stream)
static constexpr stream_enable_t StdOutEnable{0x04};   // vm usage: (sizeof(Stream) + StdIoBufferSize)
static constexpr stream_enable_t StdErrEnable{0x08};   // vm usage: (sizeof(Stream) + StdIoBufferSize)
static constexpr stream_enable_t StdIOEnabled{StdInEnable | StdEditEnable | StdOutEnable | StdErrEnable};
static constexpr stream_enable_t DefaultStreamEnable{StdIOEnabled};

using stream_count_t = uint8_t;
static constexpr stream_count_t MinStreamCount{0};
static constexpr stream_count_t MaxStreamCount{255};
static constexpr stream_count_t DefaultStreamCount{4};  // vm usage: stream_count * (sizeof(Stream) + stream_buffer_size)

using stream_buffer_size_t = uint32_t;
static constexpr stream_buffer_size_t MinStreamBufferSize{128};
static constexpr stream_buffer_size_t MaxStreamBufferSize{256 * 1024};
static constexpr stream_buffer_size_t DefaultStreamBufferSize{4 * 1024};
static constexpr stream_buffer_size_t StdIoBufferSize{4 * 1024};  // stdout/stderr line-independent write buffer

static constexpr length_t MinEqStringLength{0};
static constexpr length_t MaxEqStringLength{256};
static constexpr length_t DefaultEqStringLength{128};

static constexpr length_t MinEqArrayLength{0};
static constexpr length_t MaxEqArrayLength{256};
static constexpr length_t DefaultEqArrayLength{32};

static constexpr length_t MinEqProcLength{0};
static constexpr length_t MaxEqProcLength{256};
static constexpr length_t DefaultEqProcLength{32};

static constexpr length_t MinEqDictMaxLength{0};
static constexpr length_t MaxEqDictMaxLength{256};
static constexpr length_t DefaultEqDictMaxLength{32};

static constexpr length_t MinEqSetMaxLength{0};
static constexpr length_t MaxEqSetMaxLength{256};
static constexpr length_t DefaultEqSetMaxLength{32};

using dict_bucket_count_t = uint16_t;
using name_bucket_count_t = uint32_t;
// Smallest prime >= 2^n + 1 for n in 7..16 -- same progression Dict::bucket_count_for_capacity uses.
// Doubling steps keep the choice space predictable (--name-buckets=N snaps to the next prime up).
static constexpr name_bucket_count_t sm_name_bucket_counts[] = {131, 257, 521, 1031, 2053, 4099, 8209, 16411, 32771, 65537};
static constexpr name_bucket_count_t DefaultNameBucketCount{2053};  // vm usage: name_bucket_count * sizeof(vm_offset_t)
// Config sentinel: resolve the bucket count from the VM size at init
// (scaled_name_bucket_count).  The bucket array never grows, so the right
// count is a function of how many names the VM can hold -- one bucket per
// NameBucketVmDivisor bytes of VM keeps the load factor flat as --vm-size
// grows.  The default 1 MB VM resolves to DefaultNameBucketCount, so
// default-configuration behavior is unchanged.
static constexpr name_bucket_count_t AutoNameBucketCount{0};
// uint32_t (not vm_size_t): vm_size_t is declared further down this file.
static constexpr uint32_t NameBucketVmDivisor{512};

using save_level_t = uint8_t;
static constexpr save_level_t MinSaveCount{4};
static constexpr save_level_t MaxSaveCount{255};
static constexpr save_level_t DefaultSaveCount{64};  // vm usage: save_count * sizeof(vm_offset_t *)

// Save-token generation counter type.  Per-slot in m_save_generation,
// bumped each time Save::save reuses a slot.  Only the low 23 bits are
// significant -- they pack into the save-token integer alongside an
// 8-bit save level (level:8 | (gen ^ barrier_low23):23 + sign bit).
// uint32_t is convenient for storage; SaveGenerationMask gates writes
// to keep the persisted value within the 23-bit encoding window.
using save_generation_t = uint32_t;
static constexpr save_generation_t SaveTokenLevelBits{8};
static constexpr save_generation_t SaveTokenLevelMask{(save_generation_t{1} << SaveTokenLevelBits) - 1};
static constexpr save_generation_t SaveGenerationBits{23};
static constexpr save_generation_t SaveGenerationMask{(save_generation_t{1} << SaveGenerationBits) - 1};

using stack_depth_t = length_t;
static constexpr stack_depth_t MinDictionaryDepth{16};
static constexpr stack_depth_t MaxDictionaryDepth{256};
static constexpr stack_depth_t DefaultDictionaryDepth{64};  // vm usage: dictionary_depth * sizeof(Object)
static constexpr stack_depth_t PermanentDictCount{4};       // systemdict, protocoldict, globaldict, localdict

static constexpr stack_depth_t MinExecutionDepth{128};
static constexpr stack_depth_t MaxExecutionDepth{8192};
static constexpr stack_depth_t DefaultExecutionDepth{2048};  // vm usage: execution_depth * sizeof(Object)

static constexpr stack_depth_t MinErrorDepth{8};
static constexpr stack_depth_t MaxErrorDepth{256};
static constexpr stack_depth_t DefaultErrorDepth{64};  // vm usage: error_depth * sizeof(Object)

static constexpr stack_depth_t MinOperandDepth{128};
static constexpr stack_depth_t MaxOperandDepth{8192};
static constexpr stack_depth_t DefaultOperandDepth{1024};  // vm usage: operand_depth * sizeof(Object)

// Fixed-depth GC-root stack backing the GcScope handle-scope (object.inl): a
// dedicated VM-heap stack of temporary GC roots used while building global VM
// objects across allocations that can fire vm_global_gc.  Not user-configurable
// (the bounded chain-builders root a small fixed count; unbounded-recursive
// operators stay on the operand stack).  vm usage: MaxGcRootDepth * sizeof(Object).
// Operator-scoped (empty at every operator boundary; operators do not nest with
// live roots -- see require_gc_root_capacity in vm_heap.inl), so this caps the
// single deepest operator's simultaneous roots, not any recursion depth.  Most
// operators sit well under 10 (lazy flat-map's 6 was the high-water before the
// reactive path).  cell_set_core now drives the cap: it roots up to
// MaxWatcherCollect (64) collected downstream watcher old-values + 1 for the cell's
// own old value + 1 for the cell itself (an undef'd inline cell is reachable only via
// the op-stack slot cell_set_core pops) = 66 across watcher collection / invalidation
// / firing (each fires vm_global_gc and the precise GC does not scan the C-stack
// buffers that hold those clones).  80 covers that 66 with headroom, under the prior 128.
static constexpr stack_depth_t MaxGcRootDepth{80};

static constexpr length_t MinLocalDictMaxLength{256};
static constexpr length_t MaxLocalDictMaxLength{50000};
// vm usage: 12 bytes + (vm_offset_t * bucket_count) + (2 * (sizeof(DictEntry) * localdict_maxlength))
static constexpr length_t DefaultLocalDictMaxLength{512};
// globaldict (local VM, save level 0): holds definitions made under set-global /
// current-global.  FIXED capacity -- it never regrows, so its bucket array is stable
// and a persistent global entry (allocated in global VM at save level > 0) can never
// be orphaned by a rolled-back regrow.  --globaldict-size sets the fixed capacity.
static constexpr length_t MinGlobalDictMaxLength{16};
static constexpr length_t MaxGlobalDictMaxLength{50000};
static constexpr length_t DefaultGlobalDictMaxLength{64};
static constexpr length_t MaxDictPoolSize{16};  // support maxlength 1..16 (locals + try-catch handlers)

using name_index_t = uint16_t;

static constexpr length_t DefaultInternalDictCapacity{32};  // require dict, modules dict; ReadWriteDynamic grows if needed

// Minimum initial entry-pool capacity for a ReadWriteDynamic dict or set.  A
// dynamic container created with capacity 0 (e.g. `0 dynamic-dict`, `0 set`, or an
// internally-built empty result like set-filter on an empty set) is clamped up to
// this at creation: expand_dict/expand_set grow by doubling the current capacity, so
// a 0 base can never grow.  Clamping at creation (Dict::create_dict / create_set and
// their global twins) -- rather than rejecting the later put -- makes a dynamic
// container always growable from empty.  ReadWriteFixed / ReadOnly 0-capacity dicts
// are left at 0 (an explicitly empty fixed dict is legitimate and must not grow).
static constexpr length_t MinDynamicDictCapacity{4};

static constexpr int MaxRecursionDepth{64};                  // shared depth limit for deep-eq, flatten, and similar recursive walks
static constexpr int MaxPrintDepth{16};                      // max recursion depth for == object-form printing
static constexpr length_t MaxStrftimeTemplateLength{64};     // max length for a strftime format-string
static constexpr length_t MaxStrftimeInputLength{256};       // max input length for :I/:D scan strptime stack buffer
static constexpr length_t MaxStrftimeOutputLength{256};      // max output length for :I/:D print std::format_to_n buffer
static constexpr int MaxProcNesting{64};                     // max { } nesting depth during scanning
static constexpr integer_t MaxNameBucketChainLength{10000};  // cycle/corruption guard for name bucket validation
static constexpr integer_t MaxSaveJournalChainLength{100000};  // cycle/corruption guard for save journal validation

// Coroutine stack defaults (smaller than main stacks; ~4KB per coroutine).
// Dict + exec + err track the main-stack defaults so any proc tree that
// runs in the main coroutine also runs inside an actor without surprise
// overflow (frame-dicts + handle-effect + nested try-catch consume slots
// quickly).
static constexpr stack_depth_t DefaultCoroutineOperandDepth{128};
static constexpr stack_depth_t DefaultCoroutineExecutionDepth{256};
static constexpr stack_depth_t DefaultCoroutineDictionaryDepth{32};
static constexpr stack_depth_t DefaultCoroutineErrorDepth{16};

// Per-coroutine scratch arena.  Bump-and-collect arena carved from the
// back of the contiguous stack block.  Used by find-all / find-n /
// aggregate to accumulate per-call results without touching the shared
// persist region.  Size is configurable via the
// --scratch-depth CLI flag; both main and spawned coroutines use the
// configured value.
static constexpr stack_depth_t MinCoroutineScratchDepth{16};
static constexpr stack_depth_t MaxCoroutineScratchDepth{4096};
static constexpr stack_depth_t DefaultCoroutineScratchDepth{128};  // ~1 KB at sizeof(Object) = 8

// Actor mailbox default capacity (messages, not bytes)
static constexpr length_t MaxActorMailboxCapacity{64};

// Pipe-buffer free-list size classes (messages).  Dead-pipe buffers (both
// producer and consumer have signaled done) return to a per-save-level pool
// bucketed by size class.  Same logical/physical split as mailbox: mbx_capacity
// is user-facing (used for bounds checks + modulus), physical allocation is
// rounded up to nearest class for bucketing efficiency.  Buffers with capacity
// above the largest class bypass the pool.
static constexpr length_t sm_pipe_pool_classes[]{4, 8, 16, 32, 64, 128, 256};
static constexpr size_t MaxPipePoolSize{std::size(sm_pipe_pool_classes)};

// Mailbox free-list size classes (messages).  The mailbox-pool design splits
// "physical" from "logical" capacity to get both reuse efficiency and
// user-faithful semantics:
//
//   - Physical capacity = the actual heap-allocated buffer size, rounded up
//     from the user's request to the nearest entry in sm_mailbox_pool_classes.
//     This is what determines which free-list bucket a dead mailbox returns to.
//
//   - Logical capacity = MailboxHeader::m_capacity = the user's exact request.
//     All bounds checks and circular-buffer moduli use this value, so
//     backpressure triggers exactly at the user's cap (not at the rounded-up
//     physical size).  The slots between [logical..physical-1] are dead
//     weight, never read or written.
//
// Because physical is always >= logical, a recycled mailbox in class-8 bucket
// can serve any future request for logical <= 8.  That lets us derive the
// bucket from the current m_capacity on recycle (mailbox_class_for_capacity)
// without storing physical separately -- keeps MailboxHeader at 16 bytes.
//
// Mailboxes with capacity > the largest class are allocated exact and
// abandoned on death (same rare-case handling as Dict for maxlength > MaxDictPoolSize).
static constexpr length_t sm_mailbox_pool_classes[]{8, 16, 32, 64, 128, 256};
static constexpr size_t MaxMailboxPoolSize{std::size(sm_mailbox_pool_classes)};

// Coroutine time quantum (operations per scheduling slice; 0 = unlimited)
static constexpr uint32_t DefaultCoroutineQuantum{0};                      // unlimited by default (backward compatible)
static constexpr uint64_t DefaultStarvationThresholdNs{1'000'000'000ULL};  // 1 second

// Logic/backtracking recursion depth limits
static constexpr int MaxUnifyDepth{64};
static constexpr int MaxDerefDepth{64};
static constexpr int MaxCopyTermDepth{64};

// Pipeline stage limit
static constexpr int32_t MaxPipeStages{32};

// Reactive cell limits
static constexpr int MaxInvalidateDepth{64};
static constexpr length_t MaxWatcherCollect{64};

// Interpreter inner loop: number of operations between IRQ checks.
// Higher = better throughput, lower = better IRQ responsiveness.
// At ~100M ops/sec, 1024 ops approximately 10 microseconds of IRQ latency.
static constexpr int InterpreterIRQInterval{1024};

using vm_size_t = uint32_t;
static constexpr vm_size_t MinVmSize{256 * 1024};
static constexpr vm_size_t MaxVmSize{std::numeric_limits<vm_size_t>::max()};
static constexpr vm_size_t DefaultVmSize{1024 * 1024};

enum struct StartupMode {
    ScriptFile,          // interpret filename as a Trix source file
    Eval,                // execute an inline source string supplied via -e/--eval (no filename)
    ImageFile,           // restore VM state from a snap-shot image written by the snap-shot operator
    StdIn,               // read from stdin (no readline, no prompt; for piped input)
    Interactive,         // readline REPL via stdedit (default when no filename)
    FileAndInteractive,  // run script file, then drop to readline REPL
    EvalAndInteractive,  // run the -e/--eval inline source, then drop to readline REPL
    InspectFile,         // load lib/debugger.trx, install the debugger UI, then run script (--inspect).
                         // Enum value is always present (keeps switches/comparisons stable), but the
                         // --inspect flags that select it are gated out of non-debugger builds, so it
                         // is unreachable in release.
};

struct Config {
    const char *m_filename = nullptr;
    // -e/--eval: inline source string to execute instead of a file (StartupMode::Eval).
    // Points at process-lifetime storage (a parse_args static buffer); nullptr otherwise.
    const char *m_eval_source = nullptr;
    StartupMode m_mode = StartupMode::ScriptFile;
    stream_enable_t m_stream_enable = DefaultStreamEnable;
    stream_count_t m_stream_count = DefaultStreamCount;
    stream_buffer_size_t m_stream_buffer_size = DefaultStreamBufferSize;
    length_t m_eqstring_length = DefaultEqStringLength;
    length_t m_eqarray_length = DefaultEqArrayLength;
    length_t m_eqproc_length = DefaultEqProcLength;
    length_t m_eqdict_maxlength = DefaultEqDictMaxLength;
    length_t m_eqset_maxlength = DefaultEqSetMaxLength;
    name_bucket_count_t m_name_bucket_count =
            AutoNameBucketCount;  // 0 = scale with VM size; explicit values snap to sm_name_bucket_counts
    save_level_t m_save_count = DefaultSaveCount;
    stack_depth_t m_dictionary_depth = DefaultDictionaryDepth;
    stack_depth_t m_execution_depth = DefaultExecutionDepth;
    stack_depth_t m_error_depth = DefaultErrorDepth;
    stack_depth_t m_operand_depth = DefaultOperandDepth;
    stack_depth_t m_scratch_depth = DefaultCoroutineScratchDepth;  // per-coroutine scratch arena
    length_t m_localdict_maxlength = DefaultLocalDictMaxLength;
    length_t m_globaldict_maxlength = DefaultGlobalDictMaxLength;
    const Operator *m_useroperators = nullptr;
    // -d / --debug: arm the step-in debugger at startup.  Debugger-only --
    // set in api.inl (gated) and read only by the TRIX_DBG-gated DebugState
    // init in init.inl, so the field + its CLI flag compile out of release.
#ifdef TRIX_DEBUGGER
    bool m_debug = false;
#endif
    bool m_quiet = false;       // suppress startup banner AND all diagnostic stderr (backtraces, error messages, internal warnings)
    bool m_no_banner = false;   // suppress ONLY the interactive startup banner; diagnostics unaffected
    bool m_sandbox = false;     // disable filesystem, system, and raw memory operators
    bool m_check_only = false;  // -c/--check: scan the source for lexical/structural errors, do not execute
    bool m_stack_check = true;  // --no-stack-check sets false: skip the scan-time |...| -- out stack-effect check
    bool m_resident = false;    // skip the exec-stack Quit floor: when startup work drains, park on
                                // the IRQ wait and serve invoke()/raise_interrupt() work items
                                // instead of exiting (stopped by a delivered quit or ExitIRQ)
    uint32_t m_coroutine_quantum = DefaultCoroutineQuantum;  // default quantum for new coroutines
    uint64_t m_max_ops = 0;                                  // max operations before halt (0 = unlimited)
    uint64_t m_sleep_budget_ms{0};                           // cumulative sleep/timeout grant, ms (0 = unlimited)
    uint64_t m_timeout_ms{0};                                // --timeout: wall-clock deadline, ms (0 = unlimited)
    uint64_t m_seed{0};                                      // --seed: PCG32 seed for a fresh run (used only when m_seed_set)
    bool m_seed_set = false;                                 // whether --seed was given; thaw/-l restore saved RNG state regardless

    // Module search path: colon-separated list of directories searched by
    // require / require-module when the requested filename is relative and
    // cwd-resolution fails.  User entries searched first, then binary-
    // relative `lib/` (m_bin_dir + "/lib").  Both pointers reference long-
    // lived storage (argv backing store + parse_args static buffer) and
    // remain valid for the process lifetime.
    const char *m_module_path = nullptr;
    const char *m_bin_dir = nullptr;
    // Full resolved path to the running binary (realpath of argv[0]); surfaced
    // to scripts via executable-path so a snapshot/fork example can re-exec the
    // SAME build instead of a hard-coded path.  Process-lifetime storage.
    const char *m_exe_path = nullptr;

    // --inspect variants.  m_inspect_on_error: --inspect-on-error
    // (debugger arms break-on-error rather than StepIn).  m_inspect_at_name:
    // --inspect-at /name (pre-sets a breakpoint).  m_no_color: --no-color
    // (debugger UI switches to the mono theme at install time).  All three
    // are honoured only when m_mode == InspectFile.  Debugger-only: the
    // --inspect flag surface is gated out of non-debugger builds (the only
    // readers live in Stream::init's TRIX_DEBUGGER-gated push_inspect_boot),
    // so these fields compile out with their setters in api.inl.
#ifdef TRIX_DEBUGGER
    bool m_inspect_on_error = false;
    const char *m_inspect_at_name = nullptr;
    bool m_no_color = false;
#endif

    // Script-side argv: everything after argv[optind] (the script filename).
    // Pointers reference the original argv backing store; valid for the
    // process lifetime.  Surfaced to scripts via the command-line-args op.
    int m_script_argc = 0;
    const char *const *m_script_argv = nullptr;

    // Test hook: preload all five eq-storage generation counters to this value at init.
    // Used only by tests/test_eqref_overflow.trx to exercise the 2^32-wrap LimitCheck path
    // without actually performing 2^32 creations.  Default 0 = normal init.
    uint32_t m_test_eqgen_preload = 0;
};

//===--- Command-Line Argument Parsing ---===//

struct ParseResult {
    Config config;
    vm_size_t vm_size = DefaultVmSize;
    bool should_exit = false;
    int exit_code = 0;
};

//===--- Interrupt System and Error Codes ---===//
using interrupt_t = uint8_t;
static constexpr interrupt_t NoIRQ{0x00};
static constexpr interrupt_t Level0IRQ{0x01};  // highest priority
static constexpr interrupt_t ErrorIRQ{0x02};
static constexpr interrupt_t Level1IRQ{0x04};
static constexpr interrupt_t SuspendIRQ{0x08};
static constexpr interrupt_t ResumeIRQ{0x10};
static constexpr interrupt_t InvokeIRQ{0x20};
static constexpr interrupt_t Level2IRQ{0x40};
static constexpr interrupt_t ExitIRQ{0x80};  // lowest priority

using Error_t = uint8_t;
enum struct Error : Error_t {
    NoError,
    AssertFailed,
    DictFull,
    DictStackOverflow,
    DictStackUnderflow,
    DivByZero,
    ErrStackOverflow,
    ExecStackOverflow,
    FileOpenError,
    FilenameExists,
    FilenameNotFound,
    IOReadError,
    IOSeekError,
    IOWriteError,
    IndexCheck,
    InternalError,
    InvalidAccess,
    InvalidExit,
    InvalidFormatString,
    InvalidName,
    InvalidRestore,
    InvalidStop,
    InvalidStream,
    InvalidStreamAccess,
    InvalidThrow,
    LimitCheck,
    NumericalINF,
    NumericalNaN,
    NumericalOverflow,
    OpStackOverflow,
    OpStackUnderflow,
    RangeCheck,
    ReadOnly,
    ScanDuplicateArgId,
    ScanInputFail,
    ScanMatchFail,
    ScanTypeFail,
    ScanTypeMismatch,
    SetFilePositionRequired,
    SyntaxError,
    TypeCheck,
    Undefined,
    UndefinedCase,
    UndefinedResult,
    UnmatchedMark,
    Unsupported,
    VMFull,
    InvalidImageFile,
    SnapShotError,
    Fail,
    Protocol,
    Match,
    Require,
    Ensure,
    ExecutionLimit,
    UnhandledCapture,
    EffectNotHandled,
    AboveBarrier,
    UserError,
    TimeLimit,
    StackEffect,
    DictConflict,
};
static constexpr auto ErrorCount{+Error::DictConflict + 1};
// The Error enum doubles as the process exit code on uncaught error: the
// runtime exits with status `+error`.  Reserve 125+ for shell/POSIX-defined
// codes (125 = uncaught C++ exception, 126/127 = shell-reserved, 128+N =
// killed by signal N).  See docs/trix-reference.md "Process exit codes" --
// keep both in sync if a new Error name is added.
static_assert(ErrorCount <= 125,
              "Error count exceeds the 0..124 range reserved for Trix exit codes "
              "(125 is uncaught C++ exception, 128+N is killed by signal N); "
              "see docs/trix-reference.md \"Process exit codes\".");

//===--- Public API ---===//
[[nodiscard]] static constexpr std::string_view error_sv(Error err) {
    using namespace std::literals::string_view_literals;

    switch (err) {
    case Error::NoError:
        return "no-error"sv;

    case Error::AssertFailed:
        return "assert-failed"sv;

    case Error::DictFull:
        return "dict-full"sv;

    case Error::DictStackOverflow:
        return "dictstack-overflow"sv;

    case Error::DictStackUnderflow:
        return "dictstack-underflow"sv;

    case Error::DivByZero:
        return "div-by-zero"sv;

    case Error::ErrStackOverflow:
        return "errstack-overflow"sv;

    case Error::ExecStackOverflow:
        return "execstack-overflow"sv;

    case Error::FileOpenError:
        return "file-open-error"sv;

    case Error::FilenameExists:
        return "filename-exists"sv;

    case Error::FilenameNotFound:
        return "filename-not-found"sv;

    case Error::IOReadError:
        return "io-read-error"sv;

    case Error::IOSeekError:
        return "io-seek-error"sv;

    case Error::IOWriteError:
        return "io-write-error"sv;

    case Error::IndexCheck:
        return "index-check"sv;

    case Error::InternalError:
        return "internal-error"sv;

    case Error::InvalidAccess:
        return "invalid-access"sv;

    case Error::InvalidExit:
        return "invalid-exit"sv;

    case Error::InvalidFormatString:
        return "invalid-format-string"sv;

    case Error::InvalidName:
        return "invalid-name"sv;

    case Error::InvalidRestore:
        return "invalid-restore"sv;

    case Error::InvalidStop:
        return "invalid-stop"sv;

    case Error::InvalidStream:
        return "invalid-stream"sv;

    case Error::InvalidStreamAccess:
        return "invalid-stream-access"sv;

    case Error::InvalidThrow:
        return "invalid-throw"sv;

    case Error::LimitCheck:
        return "limit-check"sv;

    case Error::NumericalINF:
        return "numerical-inf"sv;

    case Error::NumericalNaN:
        return "numerical-nan"sv;

    case Error::NumericalOverflow:
        return "numerical-overflow"sv;

    case Error::OpStackOverflow:
        return "opstack-overflow"sv;

    case Error::OpStackUnderflow:
        return "opstack-underflow"sv;

    case Error::RangeCheck:
        return "range-check"sv;

    case Error::ReadOnly:
        return "read-only"sv;

    case Error::ScanDuplicateArgId:
        return "scan-duplicate-arg-id"sv;

    case Error::ScanInputFail:
        return "scan-input-fail"sv;

    case Error::ScanMatchFail:
        return "scan-match-fail"sv;

    case Error::ScanTypeFail:
        return "scan-type-fail"sv;

    case Error::ScanTypeMismatch:
        return "scan-type-mismatch"sv;

    case Error::SetFilePositionRequired:
        return "set-file-position-required"sv;

    case Error::SyntaxError:
        return "syntax-error"sv;

    case Error::TypeCheck:
        return "type-check"sv;

    case Error::Undefined:
        return "undefined"sv;

    case Error::UndefinedCase:
        return "undefined-case"sv;

    case Error::UndefinedResult:
        return "undefined-result"sv;

    case Error::UnmatchedMark:
        return "unmatched-mark"sv;

    case Error::Unsupported:
        return "unsupported"sv;

    case Error::VMFull:
        return "vm-full"sv;

    case Error::InvalidImageFile:
        return "invalid-image-file"sv;

    case Error::SnapShotError:
        return "snap-shot-error"sv;

    case Error::Fail:
        return "fail"sv;

    case Error::Protocol:
        return "protocol"sv;

    case Error::Match:
        return "match"sv;

    case Error::Require:
        return "require"sv;

    case Error::Ensure:
        return "ensure"sv;

    case Error::ExecutionLimit:
        return "execution-limit"sv;

    case Error::UnhandledCapture:
        return "unhandled-capture"sv;

    case Error::EffectNotHandled:
        return "effect-not-handled"sv;

    case Error::AboveBarrier:
        return "above-barrier"sv;

    case Error::UserError:
        return "user-error"sv;

    case Error::TimeLimit:
        return "time-limit"sv;

    case Error::StackEffect:
        return "stack-effect"sv;

    case Error::DictConflict:
        return "dict-conflict"sv;

    default:
        assert(false && "error_sv: unknown Error");
        std::unreachable();
    }
}
private:
//===--- Private Types and Constants ---===//
// Public SemVer: printed by --version, --about, and the REPL banner.  Bump
// MINOR for new features, PATCH for bug fixes, MAJOR for breaking changes.
//
// PRERELEASE is the pre-release identifier appended to the version string (e.g.
// "0.11.0-dev").  master carries "-dev" so a build off an untagged commit is
// honestly distinguishable from the corresponding tagged release: a release is
// cut by setting PRERELEASE to "" in the `release: vX.Y.Z` commit (alongside the
// CHANGELOG [Unreleased] promotion + README/docs version refresh) and tagging it;
// the next commit re-sets PRERELEASE to "-dev" (and bumps MINOR/PATCH as needed)
// to re-open the dev cycle.  Empty PRERELEASE formats to nothing.
static constexpr uint8_t MAJOR{0};
static constexpr uint8_t MINOR{12};
static constexpr uint8_t PATCH{0};
static constexpr std::string_view PRERELEASE{"-dev"};

// Snapshot-image format revision: stored in the snapshot header and
// strict-compared on thaw.  Increment any time the on-disk layout,
// object representation, or save/restore semantics change.  Independent
// of the SemVer above so public releases don't silently invalidate
// existing snapshots.  Monotonic uint32_t counter -- 115 was the last
// value written under the previous major/minor/patch triple encoding;
// 116 acknowledges the header layout change itself.  117 reflects the
// supervisor-synchronous-spawn change: at_supervisor_init_op no longer
// spawns children, so old images would replay with the wrong semantics
// if loaded by a new binary (no spawn on first tick).  118 adds the
// per-coroutine BindingTable: new CoroutineContext::m_binding_table
// field, new SnapShotHeader free-list heads (binding_bucket_free,
// binding_entry_free), and a new m_binding_table bucket-array offset
// per CoroutineContext on the VM heap.  119 re-adds Name::m_binding
// (Name 12 -> 16 bytes) as a single-coroutine fast-path cache gated on
// m_live_coroutine_count == 0; layout change requires bump.  120 adds
// PackedExt subcodes 0x02-0x11 for the eqref family (EqString/EqArray/
// EqProcArray/EqProcPacked in Short+Long x RW+RO variants, plus EqDict
// and EqSet).  Eqrefs can now ride inside packed procedures; the old
// reject-at-pack guard is gone.  Per-ref length (1 or 2 bytes per
// subcode variant) is encoded after the subcode so set_length-shrunk
// refs round-trip exactly.  121 changes the begin-locals opcode
// encoding: the scanner now emits TWO integers before the body (K =
// declared name count, N = dict capacity) to support the |a b|#N
// locals-capacity suffix.  Old snapshots with a single-integer preamble
// would mis-decode.  122 extends ContinuationContext with m_dict_count
// and the Continuation Object with m_continuation_save_level so capture
// / perform can cross |locals| boundaries (storing the matching locals
// dicts in the context) and resume can reject post-restore invalidation
// without dereferencing the freed ctx.  123 adds op-stack capture for
// `capture`: @delimit-barrier gets a new integer op-depth companion on
// err stack (pushed by delimit_op, consumed by at_delimit_barrier_op or
// capture_op), ContinuationContext gains m_op_count plus a trailing
// op-slice region, and execute_continuation splices the captured op
// slice below the resume value so restored bodies see their original
// op layout.  124 adds the @handler-scope barrier and its two err
// companions (Continuation identity + op-depth at handler-push),
// installed by capture_op above the trimmed delimit range.  Enables
// the abort-exec operator (still to land) to identify which handler's
// frames to discard when multiple captures are nested.  No other
// operator emits @handler-scope yet, but serialized snapshots written
// after a capture inside a running handler would include the barrier
// and its companions; SystemName indices shifted by one so old
// snapshots mis-decode.  125 appends the `interactive?` stream-tty
// predicate as a new SystemName::IsInteractive entry (new
// LAST_STD_OP).  No on-disk layout changed, but the SystemName count
// grew by one and any snapshot that serializes a reference to
// SystemName by numeric index now has one more valid slot.  126
// appends the short-circuit `and?` / `or?` pair as
// SystemName::ShortAnd / SystemName::ShortOr (new LAST_STD_OP) for
// the same reason.  128 adds the frame-dict pool -- a save-level-free
// free list for dicts tagged with Dict::IsFrame.  New field
// SnapShotHeader::frame_dict_pool_offset records where the pool lives
// in the VM heap so thaw can rehydrate it; old snapshots wouldn't carry
// the pool and would deref garbage.
// 129 reserves VM offset 0 as a null-sentinel guard word holding the
// magic cookie 'TRIX' (little-endian).  Prior to 129 the first real
// allocation (m_save_stack) landed at offset 0, which collided with
// nulloffset == 0 and caused m_save_stack to restore as nullptr after
// thaw -- any subsequent save/restore then SEGV'd.  Old images are
// rejected on thaw because their first four bytes are vm_offset_t data
// (not 'TRIX') and because save_stack_offset is 0 there.
// 130 appends SystemName::MakeStringStream / GetStringStream (new
// LAST_STD_OP) for in-memory writable string-streams.  Adds the
// IsStringWrite status bit on Stream::m_status; old snapshots never set
// the bit and old binaries reading new snapshots that include a
// string-stream would mishandle the buffer-full guard in putc/putn.
// 131 adds Object::Type::OpaqueHandle (second-to-last 5-bit type slot,
// 0x1E; 0x1F remains free),
// Object::HandleKind sub-type discriminator (Screen for now), and 8
// new SystemName entries for the screen-* op family (make-screen,
// screen-cols/rows/clear/resize/put-cell/put-string/fill-rect).  Object
// representation reuses existing m_length / m_offset slots so the 8-byte
// Object size is unchanged; sm_object_attrib is expanded to TypeCount +
// HandleKindCount and indexed via attrib_index() so each handle kind has
// its own attribute row.  PackedExt subcodes 0x12/0x13 added for
// OpaqueHandle (short/long kind variants).
// 132 appends SystemName::ScreenRender / ScreenRenderTo (new LAST_STD_OP)
// for the diff-rendering ops that emit minimal-byte CSI/SGR/UTF-8 from
// the cell buffer to stdout (sandbox-gated) or to any writable stream.
// SystemName count grew by two; any snapshot that serializes a reference
// to SystemName by numeric index now has two more valid slots.  No
// per-Object layout changes -- ScreenState was sized in version 131
// already with m_last_attrs/fg/bg/state_valid for the SGR cache.
// 134 adds m_frame_dict_overflow (single vm_offset_t scalar) for parking
// frame dicts whose maxlength exceeds MaxDictPoolSize.  Pre-fix, oversized
// frame dicts (e.g. tetrix's |fld pc futures|#25) were dropped on recycle,
// permanently leaking ~584 bytes per call.  The new chain pays-as-you-go:
// only sizes actually freed appear, scanned linearly on alloc.  The thaw
// path nulls it (mirroring the m_frame_dict_pool reset in the thaw path) so
// the field is informational across thaw, not load-bearing.
//
// 135 inserts SystemName::Crc32 ("crc32" between "countr-zero" and
// "cyl-bessel-i") and SystemName::Fletcher32 ("fletcher32" between
// "flatten" and "finally") for the C-side checksum operators.  Like
// 133's CommandLineArgs insertion, this shifts subsequent SystemName
// indices and invalidates any prior snapshot that serialized a name
// by numeric index.
//
// 136 inserts SystemName::LocalDef ("local-def" between "load" and
// "lgamma") -- the def-skips-frame fix.  Additive
// only (no behavior change to def); shifts subsequent SystemName
// indices, invalidating prior snapshots that serialized a name by
// numeric index.  Counterpart to def for explicit |...| frame writes.
//
// 137 inserts SystemName::Now ("now" between "not" and "opstack") --
// monotonic-clock milliseconds for elapsed-time deltas.  Companion to
// the existing "epoch-time" (wall clock) and "time { proc }" (proc
// elapsed micros).  Additive; shifts subsequent SystemName indices.
//
// 138 inserts SystemName::atTryRollback -- internal control
// op that rolls the operand stack back to the depth it had when `try`
// was called when the wrapped proc errors.  exec layout becomes
// [saved-depth] [@try-rollback] [@try-barrier] [proc].  Pre-fix `try`
// left proc residue below /error-name; the new layer drops the residue
// and replants /error-name on a clean stack.  Additive; shifts
// subsequent SystemName indices.
//
// 139 inserts SystemName::RangeFrom -- splits the
// arity-overloaded `range` op into a strict 1-arg `range stop -- arr`
// and a new 2-arg `range-from start stop -- arr`.  Pre-fix the 1-or-2
// arg detection silently grabbed an integer below the top whenever
// one happened to be there, producing wrong-shape arrays.  Additive;
// shifts subsequent SystemName indices.
//
// 141 inserts SystemName::CoroutineLastError and WellKnownName::NoError.
// Side-channel diagnostic accessor:
// `coroutine-last-error coroutine -- /name` returns the error name that
// killed the target, or /no-error if it exited normally / is still alive.
// Reads m_exit_reason which coroutine_kill already records; pre-fix the
// only way to recover this was to wrap every coroutine body in try.  Pure
// addition; shifts subsequent SystemName + WellKnownName indices.
//
// 142 inserts SystemName::CoroutineAwait + SystemName::atCoroutineAwaitCheck.
// `coroutine-await coroutine -- value` is the
// auto-rethrow join: blocks until target dies; on normal exit pushes the
// return value, on abnormal exit rethrows the same error name in the
// awaiter's context so a wrapping try/try-catch catches it as if local.
// Mirrors coroutine-join's trampoline (atCoroutineAwaitCheck).  Pure
// addition; coroutine-join keeps its existing 2-value contract for
// observe-without-inherit.  Shifts subsequent SystemName indices.
//
// 143 renames the IEEE-754 Double special-value names "infd" -> "inf#d"
// and "nand" -> "nan#d" (matching the existing #X type-qualifier suffix
// family on literals: 0xFF#b, 16#FF#u, etc.; "nand" also visually
// collided with the bitwise NOT-AND op in any C/Verilog reader's eye)
// and adds the explicit-suffix aliases "inf#r" / "nan#r" for the bare
// "inf" / "nan" Real defaults (parallel to how 42 and 42i both name
// Integer).  Two new SystemName entries (InfRSuffix / NaNRSuffix)
// inserted between NaNR and InfD; subsequent indices shift by 2.
// Source-incompatible: ROMs / scripts using bare "infd" or "nand"
// must rename to "inf#d" / "nan#d" (no deprecated aliases).
//
// 147 appends SystemName::Adler32 ("adler32") at the end of the enum
// (after HandleKind) -- third native checksum op alongside crc32 and
// fletcher32, matching the same str -- uint32 signature.  RFC 1950
// Adler-32 (zlib trailer; init s1=1 s2=0; defers mod every NMAX=5552
// bytes).  Used by examples/amazing.trx for PNG IDAT trailers without
// paying the per-byte Trix-side adler accumulation cost.
//
// 148 appends SystemName::Deflate, SystemName::DeflateLevel, and
// SystemName::Inflate (in that order, after Adler32) -- native zlib-
// backed RFC 1951 raw DEFLATE / inflate primitives.  Stack effects:
// `deflate (str -- str)`, `deflate-level (str int -- str)` (level in
// 0..9), `inflate (str -- str)`.  Output is the raw bitstream; callers
// adding a zlib (RFC 1950) or gzip (RFC 1952) wrapper compose the 2-
// byte / 10-byte header + adler32/crc32 trailer themselves with the
// existing checksum ops.  Inflate output is bounded by available VM
// scratch space (effectively `--vm-size`); zip-bomb inputs raise
// vm-full once they exceed that.  Adds a hard zlib link dependency
// (`-lz`).  Pure addition; shifts subsequent SystemName indices.
//
// 149 appends SystemName::DeflateStream, SystemName::DeflateStreamLevel,
// and SystemName::InflateStream (after Inflate) -- streaming variants
// of the deflate / inflate ops.  Stack effects:
// `deflate-stream (in-stream out-stream -- )` default level 6,
// `deflate-stream-level (in-stream out-stream int -- )` level 0..9,
// `inflate-stream (in-stream out-stream -- )`.  Reads input until EOF,
// writes raw RFC 1951 output to the destination stream incrementally
// via zlib's Z_NO_FLUSH / Z_FINISH state machine.  Lifts the 65 535-
// byte MaxStringLength cap that bounds the one-shot string variants;
// works on inputs of any size (file streams, stdio, or any combination
// of stream backends).  Pure addition; shifts subsequent SystemName
// indices.
//
// 150 appends SystemName::Adler32Stream, SystemName::Crc32Stream,
// SystemName::Fletcher32Stream, and SystemName::MakeMemoryStream
// (after InflateStream) -- streaming checksum companions plus a
// readable Stream wrapping a Trix string.  Stack effects:
// `<checksum>-stream (in-stream -- uint32)` for each of the three
// checksum streams; `make-memory-stream (str -- in-stream)` returns a
// read-only Stream over a malloc'd copy of the source bytes (the
// Stream takes ownership and `free()`s on close).  The streaming
// checksums match the one-shot variants byte-for-byte on identical
// input -- adler32 defers the mod every NMAX=5552 bytes, crc32 routes
// through `crc32_update`, fletcher32 stashes the trailing odd byte
// across chunk boundaries.  make-memory-stream lifts the
// MaxStringLength cap on streaming compress / checksum tests by
// removing the temp-file requirement for in-memory data.  Pure
// addition; shifts subsequent SystemName indices.
// 158 retires CoroutineContext::m_creation_sl (save_level_t, 2 bytes).
// A later audit confirmed it had zero readers anywhere in the tree;
// pure write-only state.  Removing the field shifts m_activation_sl forward in
// the byte layout, breaking the snapshot-byte-stream compatibility
// for thawed CoroutineContexts (stored in global VM as
// raw bytes).  Pure removal; no replacement field.
//
// 159 adds ChunkKind::Supervisor (= 27).  SupervisorState now allocates
// via gvm_alloc<SupervisorState>(.., ChunkKind::Supervisor); the new
// ChunkKind byte appears in dlm chunk headers in the global region.
// Old binaries reading new snapshots would not recognize the tag in
// gvm_kind_name's by-kind introspection.
//
// 160 collapses m_pipe_pool from a 2D [save_count][MaxPipePoolSize]
// array to a 1D [MaxPipePoolSize] array (mirror of the mailbox
// pool collapse).  Pipe buffers also flip from local VM (vm_alloc) to
// global VM (gvm_alloc with newly-Active ChunkKind::PipeBuffer = 19).
// The pool array's serialized size shrinks by a factor of m_max_save_-
// level, breaking layout compatibility with older snapshots.
//
// 161 adds 4 segregated fastbin head pointers to SnapShotHeader for
// the segregated fastbins.  Bins partition free blocks of size 24/32/40/48
// bytes into per-size LIFO stacks; alloc/free of small blocks (the
// realistic ${...} ExtValue-churn workload) becomes O(1).  Header
// grows by 16 bytes (552 -> 568); checksum offset shifts (544 -> 560).
// Old binaries cannot read 161 snapshots (header size mismatch); new
// binaries cannot read 160 snapshots (no fastbin state in old image).
//
// 162 collapses the GvmBlock IsFree bit (m_flags & GvmFlagFree) into
// a new ChunkKind::Free sentinel.  Free blocks now encode as
// (m_kind=Free, m_flags=0); pre-162 free blocks encode as
// (m_kind=Other, m_flags=0x01).  Heap-blob bytes shift, so 161
// snapshots cannot be read by 162+ binaries.  Header layout
// unchanged.
//
// 163 splits ChunkKind::Coroutine (= 18) into two: CoroutineStacks
// (= 18, the per-coroutine ~3328-byte stacks block) and CoroutineContext
// (= 28, the ~236-byte fixed-shape struct).  Walker dispatch now keys
// on kind instead of payload size.  Pre-163 snapshots have m_kind=18
// for both blocks; new binaries cannot disambiguate them retroactively.
// Older binaries cannot read 163 snapshots (CoroutineContext = 28 is
// outside their MaxChunkKind = 28).
//
// 164 adds ChunkKind::GcScratch (= 29) for the lazy GC scratch block
// that holds the local-visited set + work queue.  Adds two header
// fields: gc_scratch_offset (vm_offset_t) and gvm_user_block_count
// (uint32_t).  Header grows 568 -> 576; checksum offset shifts
// 560 -> 568.  Old snapshots are unreadable.  Closes the temp-region
// squeeze that stalled vm-global-gc when global pressure clamped
// m_vm_temp_ptr below the visited-set's 32 KiB.
//
// 165: GvmBlock header grows 8 -> 16 bytes (m_next_in_work intrusive GC
// queue link + m_magic sentinel).  Every block in the global region
// shifts payload offset by +8 vs v164; pre-v165 snapshots are
// unreadable as a verbatim byte-blob.  Closes the fixed-cap GC work-
// queue / doomed-list landmines.
//
// 166: Dict + Set struct prefix grows 16 -> 20 bytes (m_next_in_visit
// intrusive GC visit-list link).  Used by the global GC mark phase to
// track local-VM Dict/Set containers visited from global walks --
// removes the 8192-entry visited-set fixed cap for the dominant case.
// Bucket array offset shifts +4 in every Dict/Set block; bytes within
// pre-v166 snapshots are unreadable.
//
// 168 adds gvm_free_block_count (uint32_t) to SnapShotHeader for the
// live count of blocks on the global VM allocator's free list (main +
// fastbins combined).  Backs //:status:vm-global-num-free (O(1) read)
// alongside the existing //:status:vm-global-num-alloc (the existing
// m_gvm_user_block_count).  The new uint32_t consumed an existing
// padding slot after gvm_user_block_count, so the header size and
// checksum offset are unchanged from v167 -- but the new field is
// non-zero in fresh snapshots and a v167 binary reading a v168
// snapshot would interpret that byte range as padding.  Pre-v168
// snapshots are unreadable by v168+ binaries (and vice versa) via
// the version-mismatch reject in thaw.
//
// 169 adds m_op_rewrite_count to ContinuationContext (a length_t field
// after m_op_count) and a tail array of err-segment offsets after the
// existing exec / err / dicts / op-slice payload.  Closes the silent
// operand-stack-corruption bug where a perform-captured K rides an
// inner @delimit-barrier along; on resume the inner barrier's
// capturer-time op-depth was stale, and a subsequent capture in the
// resumed body would trim the live op stack to the wrong floor.  The
// rewrite-table collapses every captured inner op-depth slot to the
// resumer's segment-start floor at restore time.  ContinuationContext
// layout changes; snapshots that hold mid-capture continuations from
// a pre-v169 binary won't thaw.  Acceptable -- continuations are
// short-lived runtime state.
//
// 170 extends the rewrite-table mechanism with a second array for the
// captured EXEC segment (m_exec_rewrite_count + trailing offsets after
// the err-rewrite array).  Captured @try-rollback barriers carry a
// saved-depth Integer one slot below themselves on the exec stack
// (set by try when it records the saved operand depth).  Without this
// fix a try { ... perform ... }
// captured K, resumed in a frame with operands below the K-invocation
// site, would trim the live op stack to the stale capturer-frame
// saved-depth on the @try-rollback error path -- same silent-
// corruption shape as v169 closed for err-side op-depths.
// ContinuationContext layout grows by one length_t; pre-v170 mid-
// capture snapshots won't thaw.
// v171: SnapshotHeader gains gc_roots_base_offset (the GcScope root stack).
// v172: dead SystemName::atPipeSourceFeed removed (source-feeder logic
// lives in synthesized Trix procs; nothing ever pushed the op).  Every
// operator index after it renumbers, so pre-v172 images would dispatch
// wrong operators -- the version gate is what makes the removal safe.
// v173: SystemName::GenState removed (state queries belong in a /query
// handle-call arm).  Same renumbering rationale as v172.
// v174: two changes.  (1) SystemName::ExecutablePath added (the executable-
// path operator); inserting an enum entry renumbers every operator after it,
// so pre-v174 images would dispatch the wrong operator (same rationale as
// v172/v173).  (2) SnapShotHeader gains operator_table_signature +
// operator_count.  The version gate only separates SOURCE revisions, but a
// debug and an optimized build of the SAME source share a version yet differ
// in operator set (#ifdef'd debug-only ops); the signature is what makes a
// cross-build thaw fail loudly instead of silently mis-dispatching.
// v175: Error::InvalidName moved from the appended tail into its alphabetical
// slot (=19), renumbering every Error from InvalidRestore onward.  The header
// persists last_error/external_error as raw Error byte values, so a pre-v175
// image would restore the wrong error identity; the version gate rejects it.
// v176: GC mark-generation became a 1-bit flip-flop (was a 0..255 counter).
// gc_current_gen is restored from the header and only ever holds {0,1}; a fresh
// alloc is stamped with the current (pre-flip) value, and each pass flips it via
// XOR.  A pre-v176 image stamped fresh allocs at gen-1 and survivors at gen, so
// a v176 build restoring gen then flipping it would collide (~50%, parity-
// dependent) with those gen-1 fresh-alloc marks -- false pre-mark, skip-trace,
// premature collection.  The version gate rejects pre-v176 images.
// v178: inserting `override` into the SystemName enum shifts the binary-token /
// snapshot system-name indices of every later operator, so a pre-v178 image
// would resolve those indices to the wrong operator -- the version gate rejects
// pre-v178 images.
// v179 changes the begin-locals preamble encoding for named-preamble locals
// (the `/`-prefix |a b /t /acc| grammar).  The scanner now emits THREE integers
// before the body (P = param count, M = declared-local count, N = dict capacity)
// and the M declared-local names sit as leading literals after the P params, so
// the layout is [/p1../pP /loc1../locM P M N {body} begin-locals].  begin-locals
// binds the P params and discards the M loc names (they are metadata for #e and
// future slot-indexing -- declared, not bound, so an unassigned /local reads
// /undefined).  Old v178 snapshots used a two-integer (K N) preamble and would
// mis-decode.
// v180 adds slot-indexing: a locals proc's own top-level body refs to
// its params are rewritten at scan time from executable Names to PackedType::SlotRef
// elements (an inline frame-slot index), resolved by direct frame indexing at run
// time.  A pre-v180 image never contains SlotRef body elements; a v180 build is
// fine reading its own images, but a pre-v180 build replaying a v180 packed stream
// would hit a SlotRef element it cannot resolve -- the version gate rejects the
// mismatch.
// v181 inserts the string-from-bytes operator into the SystemName enum next to
// chars, which shifts the ordinals of every operator after it.  Operator Objects
// persist their ordinal in the heap (and thus in snapshots), so a pre-v181 image's
// operators would resolve to the wrong functions under a v181 build -- the version
// gate rejects the mismatch.
// v182 adds name_global_mask_offset (vm_offset_t) to SnapShotHeader: the offset of
// the per-bucket "holds a global Name" GC-walk mask, pre-allocated in local VM
// parallel to the name buckets.  The mask block itself rides in the serialized VM
// blob; only its offset needs the header slot.  A pre-v182 image lacks the field,
// so the version gate rejects the mismatch.
// v183 adds the globaldict (a global-VM dictionary, sibling to localdict, holding
// definitions made under set-global / current-global).  This (a) inserts the
// GlobalDict SystemName next to LocalDict, shifting the ordinals of every operator
// after it, and (b) adds globaldict_offset (vm_offset_t) to SnapShotHeader so thaw
// can re-point m_globaldict.  A pre-v183 image mismatches on both counts; the
// version gate rejects it.
// v184 adds localdict_maybe_global (bool) to SnapShotHeader: m_localdict_maybe_global,
// the flag gating the GC's localdict skip (true => localdict may transitively own
// global VM and must be marked; false => the global sweep skips it).  Its EXACT value
// is saved/restored -- never re-derived on thaw -- so a restored image keeps walking
// or skipping localdict precisely as the saved run did.  A pre-v184 image lacks the
// field; the version gate rejects the mismatch.
static constexpr uint32_t SNAPSHOT_VERSION{184};
public:
using vm_offset_t = vm_size_t;
static constexpr vm_offset_t nulloffset{0};

// Magic cookie written at VM offset 0 by init.inl.  Spells 'TRIX' as
// four ASCII bytes in memory order (T=0x54, R=0x52, I=0x49, X=0x58);
// as a little-endian uint32_t that's 0x58495254.  Checked by thaw to
// reject stale or mis-sized images before any pointer rehydration.
static constexpr uint32_t TRIX_SENTINEL{0x58495254};

using hash_t = uint32_t;
using crc32_t = uint32_t;

using stream_id_t = uint8_t;
using operator_index_t = int16_t;  // >= 0:built-in operator, < 0:user-defined operator
private:
using packed_data_t = uint8_t;
static constexpr packed_data_t PackedLiteralAttrib{0x00};
static constexpr packed_data_t PackedExecutableAttrib{0x80};
static constexpr packed_data_t PackedAttribMask{0x80};

// span type with length_t Extent
using packed_data_span = std::span<packed_data_t>;
using cpacked_data_span = std::span<const packed_data_t>;

static constexpr length_t MaxLength{std::numeric_limits<length_t>::max()};
static constexpr length_t MaxStringLength{MaxLength};
static constexpr length_t MaxArrayLength{MaxLength};
static constexpr length_t MaxDictLength{MaxLength};
static constexpr length_t MaxSetLength{MaxLength};
static constexpr length_t MaxErrorLength{256};
static constexpr length_t MaxNameLength{127};
static constexpr length_t MaxTokenLength{128};
static constexpr length_t MaxInfixTokens{128};

static constexpr int MaxInfixNesting{64};  // max recursion depth for infix_expr / infix_primary
// 6 bytes: header + optional subcode + value (PackedExt is the worst case: 128-bit via subcode)
static constexpr vm_size_t MaxInfixPackedSize{sizeof(packed_data_t) + sizeof(packed_data_t) + sizeof(vm_offset_t)};
static constexpr vm_size_t MaxInfixPackedBufferSize{MaxInfixTokens *
                                                    MaxInfixPackedSize};  // 768 bytes: provable worst case for 128 tokens
static_assert(MaxTokenLength > MaxNameLength);

enum struct AddressState { Invalid = -2, IsNullPtr = -1, IsReadOnly = 0, IsReadWrite = 1 };

//===--- Scanner end of file, source location tracking ---===//
static constexpr int EOFc{-1};

struct SourceLocation {
    uint32_t m_line{0};
    uint16_t m_col{0};
    stream_id_t m_sid{0};
};

//===--- VM heap allocation tracking (per-source-line counters) ---===//
// Fixed-size open-addressed hash table embedded in the Trix struct.  Tracks
// (sid, line) -> (count, bytes) for every vm_alloc.  No C++ heap allocation,
// no VM allocation: the table is part of the Trix instance.
//   key encoding: (static_cast<uint64_t>(sid) << 32) | static_cast<uint64_t>(line).
//   key == 0 marks an empty slot, so a synthesized real-key collision with 0
//   is mapped to 1 (sid 0 = INVALID_SID, line 0 = pre-scan; aliasing them is
//   acceptable for diagnostic purposes).
//   If every slot fills, m_alloc_stats_saturated is set and further
//   not-yet-tracked sites are dropped (counted-existing sites still update).
struct AllocStatsEntry {
    uint64_t m_key{0};
    uint64_t m_count{0};
    uint64_t m_bytes{0};
};
static constexpr size_t AllocStatsTableSize{1024};  // power of two; allocated from VM heap at init
static_assert((AllocStatsTableSize & (AllocStatsTableSize - 1)) == 0, "AllocStatsTableSize must be a power of two");

// vm-heap-tracking side-table: per-block, indexed by VM offset.
// One HeapTrackEntry per live VM-heap block, appended at alloc time and
// pruned from the back at restore time (entries with offset >= new
// m_vm_ptr are dropped, since the bump allocator means side-table
// entries are stored in offset order).  m_key encodes (sid << 32) | line
// the same way as AllocStatsEntry::m_key.
struct HeapTrackEntry {
    vm_offset_t m_offset{nulloffset};
    uint64_t m_key{0};
};
static constexpr size_t HeapTrackTableSize{16384};  // ~256 KB side-table; gated out in release builds
