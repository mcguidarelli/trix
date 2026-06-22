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
//===--- Constructors and Destructor ---===//
//
// Implements VM construction, initialization, and teardown.  The constructor
// allocates the VM heap, initializes all subsystems, and optionally loads
// a startup script or image.  Based on:
//
//   PostScript interpreter startup: allocate VM, create systemdict with all
//   operators, create userdict, set up the dictionary stack, open the
//   startup file, and begin the interpreter loop.
//
// --- Core concepts for maintainers ---
//
// CONSTRUCTION SEQUENCE
//   1. Allocate VM heap (single malloc, default 1 MiB; minimum 256 KiB).
//   2. Initialize member variables to defaults.
//   3. Allocate the four stacks (operand, execution, error, dictionary)
//      from the VM heap.
//   4. Allocate error string buffer.
//   5. Allocate eq-buffers (=string, =array, =proc, =dict, =set).
//   6. Allocate name hash table buckets.
//   7. Initialize systemname/typename/errorname offset tables and the
//      well-known name offset table (WellKnownName enum).
//   8. Initialize streams (Stream::init: stdin, stdout, stderr, stdedit).
//   9. Call Dict::init() to create systemdict, userdict, errordict,
//      handlersdict, numbersdict -- populating ~830 user-facing
//      operators (829 user-facing / 987 total; see
//      user_facing_operator_count()).
//  10. Push startup file/stream/image onto the execution stack.
//  11. Call interpreter() -- the main loop.
//
// DESTRUCTOR
//   Sends ExitIRQ to stop the interpreter and waits (up to 5s on a
//   condition variable) for the interpreter loop to clear m_active, then
//   closes the address-validation pipe and frees the VM heap (only if this
//   Trix owns it).  The interpreter runs synchronously, not on a separate
//   OS thread.
//
// STARTUP MODES
//   Determined by Config::m_mode (set by parse_args in api.inl):
//     ScriptFile:        open and execute a .trx file
//     ImageFile:         load a snap-shot image (--image flag)
//     StdIn:             read from stdin (--stdin flag)
//     Interactive:       readline REPL (default when no filename)
//     FileAndInteractive: run file then drop to REPL (-i file.trx)
//
// PRE-ALLOCATED CONSTRUCTOR
//   An overload accepts a pre-allocated vm_t* buffer instead of
//   malloc'ing one.  Used for embedded scenarios where the host
//   controls memory allocation.
//
// Default constructor: DefaultVmSize, all Config fields default.
Trix() : Trix(DefaultVmSize, Config{}) {
}

// Allocating constructor with default vm_size.
explicit Trix(Config config) : Trix(DefaultVmSize, config) {
}

// Allocating constructor with explicit vm_size.
explicit Trix(vm_size_t vm_size, Config config) : m_mutex{}, m_cond{}, m_pipe_fds{-1, -1}, m_pcg32{}, m_active{false} {
    if (vm_size < MinVmSize) {
        throw std::invalid_argument("vm_size too small");
    } else {
        auto vm = std::unique_ptr<vm_t, decltype(&std::free)>(static_cast<vm_t *>(std::malloc(vm_size)), std::free);
        if (vm == nullptr) {
            throw std::bad_alloc();
        } else {
            m_vm_base = vm.get();
            // Round vm_size down to a multiple of GvmBlockAlignment (8) so
            // m_vm_limit is 8-byte aligned -- the allocator's per-block walk
            // assumes consecutive blocks abut m_vm_limit cleanly with no
            // trailing slack.  At most 7 bytes are wasted at the very top
            // of heap.  MinVmSize (256 KiB) is already aligned, so the
            // common path is a no-op.
            m_vm_limit = (m_vm_base + (vm_size & ~vm_size_t{GvmBlockAlignment - 1}));

            init_and_interpret(config);

            // Construction succeeded: take ownership of the malloc'd buffer
            // (freed in ~Trix) so m_vm_base stays valid for the object's whole
            // lifetime.  Had init_and_interpret thrown, the still-owning vm
            // unique_ptr would free it during stack unwinding.
            m_owned_vm_ptr = vm.release();
        }
    }
}

// Pre-allocated constructor: uses caller-supplied memory for the VM heap.
explicit Trix(void *mem, vm_size_t mem_size, Config config) : m_mutex{}, m_cond{}, m_pipe_fds{-1, -1}, m_pcg32{}, m_active{false} {
    if (mem == nullptr) {
        throw std::invalid_argument("mem is nullptr");
    } else if (mem_size < MinVmSize) {
        throw std::invalid_argument("mem_size too small");
    } else {
        m_vm_base = static_cast<vm_t *>(mem);
        // See comment in the allocating constructor: round to a multiple
        // of GvmBlockAlignment so the allocator's block-walk invariant holds.
        m_vm_limit = (m_vm_base + (mem_size & ~vm_size_t{GvmBlockAlignment - 1}));

        init_and_interpret(config);
    }
}

~Trix() {
    if (m_active) {
        static_cast<void>(raise_interrupt(ExitIRQ));
        std::unique_lock<std::mutex> lk(m_mutex);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (m_active.load(std::memory_order_acquire)) {
            if (m_cond.wait_until(lk, deadline) == std::cv_status::timeout) {
                diag_println("~Trix: interpreter did not respond to ExitIRQ within 5s -- destructing...");
                break;
            }
        }
    }

    // Close address-validation pipe. Normally closed at interpreter() exit,
    // but may still be open if init_and_interpret threw before interpreter()
    // ran, or if interpreter() itself exited via an uncaught exception. Idempotent:
    // the fd values are sentinelled to -1 once closed.
    for (int i = 0; i < 2; ++i) {
        if (m_pipe_fds[i] != -1) {
            ::close(m_pipe_fds[i]);
            m_pipe_fds[i] = -1;
        }
    }

    // Free the VM heap if this Trix owns it (allocating constructor).  Done
    // last, after the interpreter thread has stopped, so nothing touches VM
    // memory during the free.  The pre-allocated constructor leaves
    // m_owned_vm_ptr null, so caller-supplied memory is never freed here.
    if (m_owned_vm_ptr != nullptr) {
        std::free(m_owned_vm_ptr);
        m_owned_vm_ptr = nullptr;
    }
}

// Process-exit code from the most recent init_and_interpret() run.
// 0 on clean exit (quit_op or normal end-of-script).  Non-zero when an
// uncaught error reached default_handler_op (returns the underlying
// Error_t value of m_last_error) or when init_and_interpret's outer
// catch arms fired.  All non-zero values fit in 1..125 to avoid colliding
// with shell conventions for exec failures (126/127) and signals (128+N).
[[nodiscard]] int exit_code() const {
    return m_exit_code;
}
private:
//===--- Initialisation ---===//
void init_and_interpret(Config config) {
    const char *filename = config.m_filename;
    const StartupMode mode = config.m_mode;
    const bool resident = config.m_resident;
    const stream_enable_t stream_enable = config.m_stream_enable;
    const stream_count_t stream_count = config.m_stream_count;
    const stream_buffer_size_t stream_buffer_size = config.m_stream_buffer_size;
    const length_t eqstring_length = config.m_eqstring_length;
    const length_t eqarray_length = config.m_eqarray_length;
    const length_t eqproc_length = config.m_eqproc_length;
    const length_t eqdict_maxlength = config.m_eqdict_maxlength;
    const length_t eqset_maxlength = config.m_eqset_maxlength;
    name_bucket_count_t name_bucket_count = config.m_name_bucket_count;
    if (name_bucket_count == AutoNameBucketCount) {
        // m_vm_base/m_vm_limit are set by every constructor before this runs.
        name_bucket_count = scaled_name_bucket_count(static_cast<vm_size_t>(m_vm_limit - m_vm_base));
    }
    const save_level_t save_count = config.m_save_count;
    const stack_depth_t dictionary_depth = config.m_dictionary_depth;
    const stack_depth_t execution_depth = config.m_execution_depth;
    const stack_depth_t error_depth = config.m_error_depth;
    const stack_depth_t operand_depth = config.m_operand_depth;
    const stack_depth_t scratch_depth = config.m_scratch_depth;
    const length_t userdict_maxlength = config.m_userdict_maxlength;
    const Operator *useroperators = config.m_useroperators;

    assert((stream_count >= MinStreamCount) && (stream_count <= MaxStreamCount));
    assert((stream_buffer_size >= MinStreamBufferSize) && (stream_buffer_size <= MaxStreamBufferSize));
    assert((eqstring_length >= MinEqStringLength) && (eqstring_length <= MaxEqStringLength));
    assert((eqarray_length >= MinEqArrayLength) && (eqarray_length <= MaxEqArrayLength));
    assert((eqproc_length >= MinEqProcLength) && (eqproc_length <= MaxEqProcLength));
    assert((eqdict_maxlength >= MinEqDictMaxLength) && (eqdict_maxlength <= MaxEqDictMaxLength));
    assert(name_bucket_count_valid(name_bucket_count));
    assert((save_count >= MinSaveCount) && (save_count <= MaxSaveCount));
    assert((dictionary_depth >= MinDictionaryDepth) && (dictionary_depth <= MaxDictionaryDepth));
    assert((execution_depth >= MinExecutionDepth) && (execution_depth <= MaxExecutionDepth));
    assert((error_depth >= MinErrorDepth) && (error_depth <= MaxErrorDepth));
    assert((operand_depth >= MinOperandDepth) && (operand_depth <= MaxOperandDepth));
    assert((scratch_depth >= MinCoroutineScratchDepth) && (scratch_depth <= MaxCoroutineScratchDepth));
    assert((userdict_maxlength >= MinUserDictMaxLength) && (userdict_maxlength <= MaxUserDictMaxLength));

    m_error_init_complete = false;
// Interactive debugger init -- whole block folds out when Debugger is
// false (TRIX_DEBUGGER undefined).  In that case config.m_debug (set
// by the -d startup flag in api.inl) is silently ignored.
#ifdef TRIX_DEBUGGER
    m_debug = DebugState{};
    m_debug.m_mode = config.m_debug ? DebugState::Mode::StepIn : DebugState::Mode::Off;
    m_debug_active = (m_debug.m_mode != DebugState::Mode::Off);
#endif
    m_sandbox = config.m_sandbox;
    m_quiet = config.m_quiet;
    m_script_argc = config.m_script_argc;
    m_script_argv = config.m_script_argv;
    m_module_path = config.m_module_path;
    m_bin_dir = config.m_bin_dir;
    m_exe_path = config.m_exe_path;
// --inspect variants: members + Config fields exist only in debugger
// builds (see member_vars.inl / types.inl), so the copy folds out too.
#ifdef TRIX_DEBUGGER
    m_inspect_on_error = config.m_inspect_on_error;
    m_inspect_at_name = config.m_inspect_at_name;
    m_no_color = config.m_no_color;
#endif

    auto trx = this;
    try {
        // Address validation pipe
        if (::pipe2(m_pipe_fds, O_NONBLOCK) == -1) {
            diag_println("Trix pipe2(pipe_fds, O_NONBLOCK) failed, errno:{}/{}", errno, errno_string());
        } else {
            // m_useroperators must be set before startup_image so frozen names
            // can be validated against the live table.
            trx->m_useroperators = useroperators;
            trx->m_max_ops = config.m_max_ops;
            trx->m_sleep_budget_ms = config.m_sleep_budget_ms;

            if (mode == StartupMode::ImageFile) {
                // Restore VM state from a snap-shot image and resume execution.
                // parse_args guarantees the filename; a failed thaw has already
                // printed its diagnostic, so just exit non-zero.
                if ((filename != nullptr) && startup_image(trx, filename)) {
                    interpreter();
                } else {
                    m_exit_code = +Error::InvalidImageFile;
                }
            } else {
                // set VM bounds
                m_vm_ptr = m_vm_base;
                m_vm_temp_ptr = m_vm_limit;
                m_vm_alloc_active = nullptr;
                m_vm_global = m_vm_limit;
                m_curr_alloc_global = false;
                m_gvm_free_head = nulloffset;  // empty global free list at boot

                // Reserve VM offset 0 as the null-sentinel guard word.
                // nullptr_to_offset / offset_to_nullptr use 0 (== nulloffset)
                // as the null marker, so user data at VM base would survive a
                // snap-shot round trip as nullptr and crash on first deref
                // (see save/restore post-thaw).  The guard also doubles as a
                // magic cookie on thaw: the first four bytes of any live VM
                // heap must spell 'TRIX'.
                auto sentinel_ptr = vm_alloc_ptr<uint32_t>();
                *sentinel_ptr = TRIX_SENTINEL;

                // allocate save stack
                m_save_stack = vm_alloc_n_ptr<vm_offset_t>(save_count);
                m_max_save_level = save_count;
                m_curr_save_level = Save::BASE;

                // allocate base save barrier and push on save stack
                auto barrier_ptr = vm_alloc_ptr<vm_offset_t>();
                *barrier_ptr = nulloffset;
                m_save_stack[m_curr_save_level] = ptr_to_offset(barrier_ptr);

                // allocate per-slot save generation counters
                m_save_generation = vm_alloc_n_ptr<save_generation_t>(save_count);
                std::fill_n(m_save_generation, save_count, save_generation_t{0});

                // allocate ExtValue free list
                m_extvalue_free_list = vm_alloc_n_ptr<vm_offset_t>(save_count);
                std::fill_n(m_extvalue_free_list, save_count, nulloffset);

                // allocate WideValue free list (parallel to ExtValue for Int128/UInt128)
                m_widevalue_free_list = vm_alloc_n_ptr<vm_offset_t>(save_count);
                std::fill_n(m_widevalue_free_list, save_count, nulloffset);

                // allocate per-save-level temp pointer save array
                m_vm_temp_save = vm_alloc_n_ptr<vm_t *>(save_count);
                std::fill_n(m_vm_temp_save, save_count, m_vm_limit);
                m_extvalue_active_save = vm_alloc_n_ptr<integer_t>(save_count);
                std::fill_n(m_extvalue_active_save, save_count, integer_t{0});
                m_widevalue_active_save = vm_alloc_n_ptr<integer_t>(save_count);
                std::fill_n(m_widevalue_active_save, save_count, integer_t{0});
                m_save_journal_count = vm_alloc_n_ptr<length_t>(save_count);
                std::fill_n(m_save_journal_count, save_count, length_t{0});
                m_save_journal_bytes = vm_alloc_n_ptr<vm_size_t>(save_count);
                std::fill_n(m_save_journal_bytes, save_count, vm_size_t{0});

                // operand stack
                m_op_base = vm_alloc_n_ptr<Object>(operand_depth);
                m_op_limit = (m_op_base + operand_depth);
                m_op_push_limit = (m_op_limit - 1);
                m_op_ptr = (m_op_base - 1);

                // dictionary stack
                m_dict_base = vm_alloc_n_ptr<Object>(dictionary_depth);
                m_dict_limit = (m_dict_base + dictionary_depth);
                m_dict_ptr = (m_dict_base - 1);

                // execution stack
                m_exec_base = vm_alloc_n_ptr<Object>(execution_depth);
                m_exec_limit = (m_exec_base + execution_depth);
                m_exec_ptr = (m_exec_base - 1);

                // error stack
                m_err_base = vm_alloc_n_ptr<Object>(error_depth);
                m_err_limit = (m_err_base + error_depth);
                m_err_ptr = (m_err_base - 1);

                // Per-coroutine scratch arena.  Main
                // allocates its slab into local VM at init time (parallel
                // to op/exec/err/dict).  Spawned coroutines carve their
                // scratch from the contiguous global stack block.
                m_scratch_depth = scratch_depth;
                m_scratch_base = vm_alloc_n_ptr<Object>(scratch_depth);
                m_scratch_limit = (m_scratch_base + scratch_depth);
                m_scratch_ptr = m_scratch_base;

                // GC-root stack (GcScope): fixed depth, like the other stacks.
                // m_gc_roots_ptr = base - 1 (empty; top-points convention, as m_op_ptr).
                m_gc_roots_base = vm_alloc_n_ptr<Object>(MaxGcRootDepth);
                m_gc_roots_limit = (m_gc_roots_base + MaxGcRootDepth);
                m_gc_roots_ptr = (m_gc_roots_base - 1);

                // error string buffer (+1 for nul terminator)
                m_error_string_base = vm_alloc_n_ptr<vm_t>(MaxErrorLength + 1);
                m_error_string_limit = (m_error_string_base + MaxErrorLength);

                // scanner source Stream, for error reporting
                m_scanner_stream = nullptr;

                // scanner token buffer
                m_token_base = vm_alloc_n_ptr<vm_t>(MaxTokenLength);
                m_token_limit = (m_token_base + MaxTokenLength);

                // allocate the root objects array (RootObject-indexed, snapshot/thaw'd with heap)
                m_root_objects_ptr = vm_alloc_n_ptr<Object>(ROOT_OBJECT_COUNT);
                for (uint8_t i = 0; i < ROOT_OBJECT_COUNT; ++i) {
                    m_root_objects_ptr[i] = Object::make_null();
                }

                // ( )#=
                set_root_object(RootObject::EqString, Object::make_empty_string(trx, eqstring_length));
                m_eqstring_generation = 0;

                // [ ]#=
                set_root_object(RootObject::EqArray, Object::make_empty_array(trx, eqarray_length));
                m_eqarray_generation = 0;

                // { }#=
                m_eqproc_length = eqproc_length;
                m_eqproc_storage_ptr = vm_alloc_n_ptr<Object>(m_eqproc_length);
                m_eqproc_stored_length = 0;
                m_eqproc_generation = 0;

                // << >>#=
                {
                    m_eqdict_maxlength = eqdict_maxlength;
                    auto [dict, offset] = Dict::create_dict(trx, m_eqdict_maxlength);
                    m_eqdict = dict;
                    set_root_object(RootObject::EqDictObject, Object::make_dict(offset));
                    m_eqdict_generation = 0;
                }

                // {{ }}#=
                {
                    m_eqset_maxlength = eqset_maxlength;
                    auto [set, offset] = Dict::create_set(trx, m_eqset_maxlength);
                    m_eqset = set;
                    set_root_object(RootObject::EqSetObject, Object::make_set(offset));
                    m_eqset_generation = 0;
                }

                if (config.m_test_eqgen_preload != 0) {
                    m_eqstring_generation = config.m_test_eqgen_preload;
                    m_eqarray_generation = config.m_test_eqgen_preload;
                    m_eqproc_generation = config.m_test_eqgen_preload;
                    m_eqdict_generation = config.m_test_eqgen_preload;
                    m_eqset_generation = config.m_test_eqgen_preload;
                }

                // require dict: tracks idempotent file loading
                {
                    auto [_, offset] = Dict::create_dict(trx, DefaultInternalDictCapacity, Object::DictMode::ReadWriteDynamic);
                    m_require_dict_offset = offset;
                }

                // modules dict: registered module namespaces (ReadWriteDynamic grows if needed)
                {
                    auto [_, offset] = Dict::create_dict(trx, DefaultInternalDictCapacity, Object::DictMode::ReadWriteDynamic);
                    m_modules_dict_offset = offset;
                }

                // protocol registry: maps protocol-name -> dict of {method-name -> dispatch-dict}
                {
                    auto [_, offset] = Dict::create_dict(trx, DefaultInternalDictCapacity, Object::DictMode::ReadWriteDynamic);
                    m_protocol_registry_offset = offset;
                }

                // interpreter variables
                m_invoke_data = nullptr;
                m_invoke_length = 0;
                m_last_error = Error::NoError;
                m_external_error = Error::NoError;
                set_root_object(RootObject::PendingErrorData, Object::make_null());
                m_in_global_handler = false;
                m_op_count = 0;
                m_tco_count = 0;
                m_op_high_water_ptr = (m_op_base - 1);
                m_exec_high_water_ptr = (m_exec_base - 1);
                m_dict_high_water_ptr = (m_dict_base - 1);
                m_err_high_water_ptr = (m_err_base - 1);
                m_extvalue_active_count = 0;
                m_widevalue_active_count = 0;
                m_interrupt = NoIRQ;
                m_pending_irq.store(NoIRQ, std::memory_order_relaxed);
                m_interrupt_mask = NoIRQ;

                // initialize the name table, bucket_count should be a prime number
                m_name_buckets = vm_alloc_n_ptr<vm_offset_t>(name_bucket_count);
                m_name_bucket_count = name_bucket_count;
                m_name_bucket_magic = fastmod_magic_u32(name_bucket_count);
                do {
                    m_name_buckets[--name_bucket_count] = nulloffset;
                } while (name_bucket_count > 0);

                // initialize the systemname table
                // Control and placeholder operators are internal -- skip name-table
                // registration so they are invisible to user-level name lookup.
                {
                    auto systemname_offsets = vm_alloc_n_ptr<vm_offset_t>(SYSTEMNAME_COUNT);
                    for (name_index_t i = 0; i < SYSTEMNAME_COUNT; ++i) {
                        auto sysname = static_cast<SystemName>(i);
                        if (((sysname >= SystemName::FIRST_CONTROL_OP) && (sysname <= SystemName::LAST_CONTROL_OP)) ||
                            ((sysname >= SystemName::FIRST_PLACEHOLDER_OP) && (sysname <= SystemName::LAST_PLACEHOLDER_OP))) {
                            systemname_offsets[i] = nulloffset;
                        } else {
                            auto op = sysname_value(i);
                            systemname_offsets[i] = Name::add(trx, op.m_sv);
                        }
                    }
                    m_systemname_offsets = systemname_offsets;
                }

                // initialize the type names
                m_typename_offsets = vm_alloc_n_ptr<vm_offset_t>(Object::TypeCount);
                for (int i = 0; i < Object::TypeCount; ++i) {
                    m_typename_offsets[i] = Name::add(trx, Object::type_sv(static_cast<Object::Type>(i)));
                }

                // initialize the error names
                m_errorname_offsets = vm_alloc_n_ptr<vm_offset_t>(ErrorCount);
                for (int i = 0; i < ErrorCount; ++i) {
                    m_errorname_offsets[i] = Name::add(trx, error_sv(static_cast<Error>(i)));
                }

                // initialize the well-known name offset table (enum-indexed, snapshot-friendly)
                {
                    m_wellknown_offsets = vm_alloc_n_ptr<vm_offset_t>(WELLKNOWN_COUNT);
                    for (name_index_t i = 0; i < WELLKNOWN_COUNT; ++i) {
                        // Completeness (every index filled, non-empty) is proven at COMPILE
                        // time by the verify_dispatch_tables() static_assert in trix.h; this
                        // assert is a defensive guard on the loop plumbing only.
                        auto sv = wellknown_sv(i);
                        assert(!sv.empty() && "wellknown_sv: missing or empty string for a WellKnownName index");
                        m_wellknown_offsets[i] = Name::add(trx, sv);
                    }
                    populate_wellknown_cache();
                }

                // pre-intern well-known names used by query-status and stack-probe
                // so they are allocated before any save barrier (immune to restore).
                // IMPORTANT: keep in sync with status_lookup() and stack_probe_op().
                {
                    using namespace std::literals::string_view_literals;
                    static constexpr std::string_view status_names[] = {
                            // VM memory
                            "vm-used"sv,
                            "vm-temp-used"sv,
                            "vm-free"sv,
                            "vm-total"sv,
                            "vm-alloc-since-save"sv,
                            "vm-alloc-active"sv,
                            "vm-global-used"sv,
                            "vm-global-num-alloc"sv,
                            "vm-global-num-free"sv,
                            // GC introspection
                            "gc-runs"sv,
                            "gc-last-reclaimed"sv,
                            "gc-in-progress"sv,
                            "gc-current-gen"sv,
                            // stack depths
                            "op-depth"sv,
                            "exec-depth"sv,
                            "dict-depth"sv,
                            "error-depth"sv,
                            // stack limits
                            "max-op-depth"sv,
                            "max-exec-depth"sv,
                            "max-dict-depth"sv,
                            "max-error-depth"sv,
                            "max-save-depth"sv,
                            // high-water marks
                            "op-high-water"sv,
                            "exec-high-water"sv,
                            "dict-high-water"sv,
                            "error-high-water"sv,
                            // save system
                            "save-level"sv,
                            "save-journal-entries"sv,
                            "save-journal-bytes"sv,
                            "frame-dict-pool-evictions"sv,
                            "save-journal-validate"sv,
                            "save-vm-barrier"sv,
                            "save-name-barrier"sv,
                            // ExtValue pool
                            "extvalue-free"sv,
                            "extvalue-active"sv,
                            "extvalue-validate"sv,
                            // WideValue pool
                            "widevalue-free"sv,
                            "widevalue-active"sv,
                            "widevalue-validate"sv,
                            // validation
                            "vm-validate"sv,
                            "exec-validate"sv,
                            // counters
                            "op-count"sv,
                            "tco-count"sv,
                            // exec inspection
                            "exec-top-type"sv,
                            "exec-top-is-call"sv,
                            "exec-top-is-barrier"sv,
                            "call-depth"sv,
                            // error state
                            "last-error"sv,
                            // name table
                            "name-count"sv,
                            "name-vm-used"sv,
                            "name-max-chain"sv,
                            "name-avg-chain"sv,
                            "name-bucket-count"sv,
                            // dictionary
                            "userdict-length"sv,
                            "userdict-maxlength"sv,
                            // configuration
                            "eqstring-length"sv,
                            "eqarray-length"sv,
                            "eqproc-length"sv,
                            "stream-buffer-size"sv,
                            "interrupt-level"sv,
                            "interrupt-mask"sv,
                            "last-scan-line"sv,
                            "last-scan-col"sv,
                            "last-scan-sid"sv,
                            "locals-pool-count"sv,
                            "locals-overflow-count"sv,
                            // eqref generations
                            "eqstring-generation"sv,
                            "eqarray-generation"sv,
                            "eqproc-generation"sv,
                            "eqdict-generation"sv,
                            "eqset-generation"sv,
// vm-heap-tracking (alloc-stats + heap-track)
#ifdef TRIX_HEAP_TRACKING
                            "alloc-sites"sv,
                            "alloc-saturated"sv,
                            "alloc-tracking"sv,
                            "heap-track-count"sv,
                            "heap-track-capacity"sv,
                            "heap-track-saturated"sv,
                            "heap-track-generation"sv,
                            "vm-peak-used"sv,
                            "vm-peak-temp-used"sv,
#endif
                            // streams
                            "stream-inuse"sv,
                            "stream-free"sv,
                            "stream-count"sv,
                            // coroutines
                            "coroutine-count"sv,
                            "coroutine-total"sv,
                            "coroutine-stack-free"sv,
                            "coroutine-ctx-free"sv,
                            "coroutine-is-main"sv,
                            "coroutine-running"sv,
                            // scheduler
                            "coroutine-ready"sv,
                            "coroutine-sleeping"sv,
                            "coroutine-blocked"sv,
                            "coroutine-suspended"sv,
                            "coroutine-starved"sv,
                            "ready-queue-depth"sv,
                            "ready-high-queue-depth"sv,
                            "timer-list-depth"sv,
                            "sched-count"sv,
                            "sched-ready-pops"sv,
                            "sched-timer-wakes"sv,
                            "sched-real-sleeps"sv,
                            "starvation-threshold"sv,
                            // composability subsystem
                            "protocol-count"sv,
                            "protocol-method-count"sv,
                            "gen-ref-counter"sv,
                            // stack-probe selectors
                            "op-stack"sv,
                            "exec-stack"sv,
                            "dict-stack"sv,
                            "error-stack"sv,
                    };
                    for (auto sv : status_names) {
                        static_cast<void>(Name::add(trx, sv));
                    }
                }

                // standard Streams and stream pool
                Stream::init(trx, stream_enable, stream_count, stream_buffer_size, filename, mode, resident);

                // create and populate standard Dicts
                Dict::init(trx, Dict::InitConfig{userdict_maxlength, useroperators});
                m_error_init_complete = true;

                // gen-server ref counter
                m_gen_ref_counter = 0;

                // supervision: monitor ref counter
                m_next_monitor_ref = 0;

                // reactive cells
                m_current_cell = nulloffset;
                m_batch_depth = 0;
                m_deferred_watchers = nulloffset;
                m_deferred_watcher_count = 0;

                // initialize coroutine scheduler: main is coroutine #0
                {
                    auto [ctx, ctx_offset] = coroutine_alloc_context();
                    ctx->m_status = CoroutineContext::Running;
                    ctx->m_has_return = false;
                    ctx->m_activation_sl = Save::BASE;
                    ctx->m_trap_exit = false;
                    ctx->m_flags = 0;
                    ctx->m_context_offset = ctx_offset;
                    ctx->m_wake_time_ns = 0;
                    ctx->m_suspend_remaining_ns = 0;
                    ctx->m_next = ctx_offset;  // circular: points to self
                    ctx->m_ready_next = nulloffset;
                    ctx->m_ready_prev = nulloffset;
                    ctx->m_timer_next = nulloffset;
                    ctx->m_id = 0;       // main coroutine is always ID 0
                    ctx->m_quantum = 0;  // main never auto-yields
                    ctx->m_ops_remaining = 0;
                    ctx->m_priority = CoroutineContext::PriorityNormal;
                    ctx->m_blocked_sender_next = nulloffset;
                    ctx->m_last_run_time_ns = 0;
                    ctx->m_joiner = nulloffset;
                    ctx->m_return_value = Object::make_null();
                    ctx->m_stacks_offset = nulloffset;  // main uses Trix member vars directly
                    ctx->m_scanner_stream = nullptr;
                    ctx->m_mailbox = nulloffset;
                    ctx->m_monitors = nulloffset;
                    ctx->m_monitoring = nulloffset;
                    ctx->m_binding_table = nulloffset;
                    ctx->m_exit_reason = wellknown_name(WellKnownName::Normal);
                    ctx->m_last_joined_exit_reason = wellknown_name(WellKnownName::NoError);
                    ctx->m_curr_alloc_global =
                            false;  // process-wide flag starts false; per-coroutine save/restore takes over thereafter
                    // Store current stack pointers into main context
                    ctx->m_op_base = ptr_to_offset(m_op_base);
                    ctx->m_op_ptr = ptr_to_offset(m_op_ptr + 1);
                    ctx->m_op_limit = ptr_to_offset(m_op_limit);
                    ctx->m_exec_base = ptr_to_offset(m_exec_base);
                    ctx->m_exec_ptr = ptr_to_offset(m_exec_ptr + 1);
                    ctx->m_exec_limit = ptr_to_offset(m_exec_limit);
                    ctx->m_err_base = ptr_to_offset(m_err_base);
                    ctx->m_err_ptr = ptr_to_offset(m_err_ptr + 1);
                    ctx->m_err_limit = ptr_to_offset(m_err_limit);
                    ctx->m_dict_base = ptr_to_offset(m_dict_base);
                    ctx->m_dict_ptr = ptr_to_offset(m_dict_ptr + 1);
                    ctx->m_dict_limit = ptr_to_offset(m_dict_limit);
                    ctx->m_scratch_base = ptr_to_offset(m_scratch_base);
                    ctx->m_scratch_ptr = ptr_to_offset(m_scratch_ptr);
                    ctx->m_scratch_limit = ptr_to_offset(m_scratch_limit);
                    m_coroutine_head = ctx_offset;
                    m_running_coroutine = ctx_offset;
                    m_main_context = ctx_offset;
                    m_live_coroutine_count = 0;
                    m_ready_head = nulloffset;
                    m_ready_tail = nulloffset;
                    m_ready_high_head = nulloffset;
                    m_ready_high_tail = nulloffset;
                    m_timer_head = nulloffset;
                    m_next_coroutine_id = 1;        // main got ID 0, next coroutine gets 1
                    m_coroutine_ops_remaining = 0;  // main has unlimited quantum
                    m_default_coroutine_quantum = config.m_coroutine_quantum;
                    m_sched_count = 0;
                    m_sched_ready_pops = 0;
                    m_sched_timer_wakes = 0;
                    m_sched_real_sleeps = 0;
                    m_ready_queue_depth = 0;
                    m_ready_high_queue_depth = 0;
                    m_timer_list_depth = 0;
                    m_starvation_threshold_ns = DefaultStarvationThresholdNs;
                    m_coroutine_stack_free = nulloffset;
                    m_coroutine_ctx_free = nulloffset;
                    m_binding_bucket_free = nulloffset;
                    m_binding_entry_free = nulloffset;
                }

                // set high-water marks to post-initialization state
                m_op_high_water_ptr = m_op_ptr;
                m_exec_high_water_ptr = m_exec_ptr;
                m_dict_high_water_ptr = m_dict_ptr;
                m_err_high_water_ptr = m_err_ptr;

                // print startup banner for interactive modes
                if (!config.m_quiet && !config.m_no_banner &&
                    ((mode == StartupMode::Interactive) || (mode == StartupMode::FileAndInteractive))) {
                    auto vm_size = static_cast<uint64_t>(m_vm_limit - m_vm_base);
                    constexpr uint64_t G = 1024ULL * 1024 * 1024;
                    constexpr uint64_t M = 1024ULL * 1024;
                    constexpr uint64_t K = 1024;
                    auto vm_val = vm_size;
                    const char *vm_unit = "";
                    if ((vm_size >= G) && ((vm_size % G) == 0)) {
                        vm_val = vm_size / G;
                        vm_unit = "G";
                    } else if ((vm_size >= M) && ((vm_size % M) == 0)) {
                        vm_val = vm_size / M;
                        vm_unit = "M";
                    } else if ((vm_size >= K) && ((vm_size % K) == 0)) {
                        vm_val = vm_size / K;
                        vm_unit = "K";
                    }

                    // Count installed user operators (null-func-terminated array);
                    // 3 for the reference trix binary (its example ops), more for
                    // tetrix / chip8 / embeds, 0 if the host installs none.
                    auto user_op_count = name_index_t{0};
                    if (m_useroperators != nullptr) {
                        for (auto ptr = m_useroperators; ptr->m_func != nullptr; ++ptr) {
                            ++user_op_count;
                        }
                    }

                    // Build-identity line: user-facing op count (full table size in
                    // parens), any installed user operators, and the snap-shot image
                    // format version -- so a cross-build thaw mismatch is visible at
                    // a glance (the "total" is what the image's signature pins).
                    if (user_op_count > 0) {
                        std::println("Trix {}.{}.{} -- {}{} VM -- {} ops ({} total) +{} user, image v{}",
                                     MAJOR,
                                     MINOR,
                                     PATCH,
                                     vm_val,
                                     vm_unit,
                                     user_facing_operator_count(),
                                     SYSOPERATOR_COUNT,
                                     user_op_count,
                                     SNAPSHOT_VERSION);
                    } else {
                        std::println("Trix {}.{}.{} -- {}{} VM -- {} ops ({} total), image v{}",
                                     MAJOR,
                                     MINOR,
                                     PATCH,
                                     vm_val,
                                     vm_unit,
                                     user_facing_operator_count(),
                                     SYSOPERATOR_COUNT,
                                     SNAPSHOT_VERSION);
                    }
                    std::println("(ctrl-D or 'quit' to exit)");
                }

                // start processing
                interpreter();
            }  // end fresh-boot (non-image) startup
        }
    }

    // catch by value as e is a trivial type and this is slightly more efficient
    catch (Exception e) {
        switch (e) {
        case Exception::EndOfStream:
            diag_println("Trix EndOfStream while initializing 'interpreter'");
            m_exit_code = static_cast<int>(Error::IOReadError);
            break;

        case Exception::Error: {
            if (!m_error_init_complete) {
                diag_println("Trix {} while initializing 'interpreter'", error_sv(m_last_error));
            } else {
                auto errname_obj = *m_last_error_name_ptr;
                auto errstr = errname_obj.name_value(trx)->sv();
                auto opstr = m_last_operator_ptr->operator_string(trx);
                diag_println("Trix {} '{}' while initializing 'interpreter'", errstr, opstr);
            }
            m_exit_code = static_cast<int>(m_last_error);
            break;
        }

        case Exception::Exit:
            // Exit reaching the init catch implies the throw happened before
            // interpreter() was set up to swallow it -- i.e., an init-time abort.
            // default_handler_op's Exit on the abnormal-error path is caught by
            // interpreter() instead and sets m_exit_code there.  quit_op leaves
            // m_exit_code at 0 (clean exit).
            diag_println("Trix Exit while initializing 'interpreter'");
            break;
        }
    }

    catch (const std::exception &e) {
        // terminate on uncaught standard exception, this is a design choice
        diag_println("Trix {} unhandled exception while initializing 'interpreter'", e.what());
        m_exit_code = 125;
    }

    catch (...) {
        // terminate on uncaught unknownexception, this is a design choice
        diag_println("Trix unhandled unknown exception while initializing 'interpreter'");
        m_exit_code = 125;
    }
}
