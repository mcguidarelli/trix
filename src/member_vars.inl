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

//===--- Instance Variables ---===//
//
// Almost all fields carry in-line default initializers.  The Trix instance is
// constructed on the stack or as a member, so without defaults member
// storage is uninitialized until init_and_interpret() assigns it -- any
// read before that assignment reads garbage (a silent bug if the garbage
// happens to look plausible).  Defaults are cheap (one-shot at startup)
// and localize the "what is this when unset?" invariant to the decl site.
// The lone exception is the trailing "cache-line cold data" block (m_mutex,
// m_cond, m_pipe_fds, m_pcg32, m_active): it is initialized in the
// constructor member-init list in init.inl instead -- mutex/condvar/PCG32
// self-init via their default ctors, while m_pipe_fds{-1, -1} and
// m_active{false} are set there explicitly.
//
// Cache-line 0 (64 bytes = 8 pointers): the interpreter inner loop's hot
// fields.  Every iteration touches m_op_ptr, m_exec_ptr, m_exec_base
// (loop condition), and m_op_push_limit (data push).  Name lookups touch
// m_dict_ptr.  Overflow paths touch m_op_base, m_op_limit, m_exec_limit.
// Grouping these in one cache line minimizes L1 misses in the inner loop.
//
public:
Object *m_op_ptr{nullptr};  // top of operand stack (one-past-empty = m_op_base - 1)
private:
Object *m_exec_ptr{nullptr};       // top of execution stack (one-past-empty = m_exec_base - 1)
Object *m_op_push_limit{nullptr};  // m_op_limit - 1: fast single-push capacity check
Object *m_exec_base{nullptr};      // bottom of execution stack
public:
Object *m_op_base{nullptr};  // bottom of operand stack
private:
Object *m_dict_ptr{nullptr};    // top of dictionary stack
Object *m_op_limit{nullptr};    // one-past-end of operand stack
Object *m_exec_limit{nullptr};  // one-past-end of execution stack
// --- end cache line 0 (64 bytes) ---

// VM heap pointers
vm_t *m_vm_ptr{nullptr};       // current watermark; HOT (advanced on every alloc); grows upward from m_vm_base
vm_t *m_vm_limit{nullptr};     // one-past-end; fixed after construction
vm_t *m_vm_temp_ptr{nullptr};  // temp region watermark; checked on every alloc; grows downward from m_vm_limit
vm_t *m_vm_base{nullptr};      // start of heap; cold after initialisation
// malloc'd VM buffer this Trix owns (allocating ctor only), freed in ~Trix;
// nullptr for the pre-allocated (host-owned) ctor so host memory is untouched.
vm_t *m_owned_vm_ptr{nullptr};
vm_t *m_vm_alloc_active{nullptr};         // open-ended streaming alloc pointer (nullptr when idle)
vm_t *m_vm_global{nullptr};               // global region watermark; grows downward from m_vm_limit (== m_vm_limit when no globals)
vm_offset_t m_gvm_free_head{nulloffset};  // head of sorted-by-offset free list inside [m_vm_global, m_vm_limit);
                                          // nulloffset when empty
vm_offset_t m_gvm_fastbins[GvmFastBinCount]{};  // segregated LIFO fastbins for small block sizes (32/40/48/56);
                                                // each cell defaults to nulloffset (== 0)
bool m_curr_alloc_global{false};                // global-allocation flag (set-global / current-global / ${...}):
                                                // when true, future scanner Name interns and runtime container
                                                // allocations (array/dict/set/string/dynamic-dict) land in global VM.
vm_t *m_token_base{nullptr};
vm_t *m_token_limit{nullptr};

// dictionary stack (m_dict_ptr is in cache line 0)
Object *m_dict_base{nullptr};
Object *m_dict_limit{nullptr};

// error stack
Object *m_err_base{nullptr};
Object *m_err_limit{nullptr};
Object *m_err_ptr{nullptr};

// Per-coroutine scratch arena live pointers (mirror
// the running coroutine's m_scratch_base/limit/ptr while it owns the CPU).
// scratch_push grows m_scratch_ptr upward from m_scratch_base toward
// m_scratch_limit; scratch_free / scratch_collect reset to base.  Saved
// into the running CoroutineContext on flush; reloaded on context switch.
Object *m_scratch_base{nullptr};
Object *m_scratch_limit{nullptr};
Object *m_scratch_ptr{nullptr};  // one-past-empty = m_scratch_base when idle

// Per-Trix scratch depth -- copied from Config::m_scratch_depth at init.
// Both main and spawned coroutines use this size for their scratch arena.
stack_depth_t m_scratch_depth{DefaultCoroutineScratchDepth};

// GC-root stack: a fixed-depth VM-heap stack of temporary GC roots, scanned by
// walk_all_roots like the other stacks.  Holds operator-scoped temps: a using
// operator clears it at its tail, and error() clears it on the throw path, so it
// is empty at every operator boundary (see require_gc_root_capacity in
// vm_heap.inl).  m_gc_roots_ptr points at the top (one-past-empty =
// m_gc_roots_base - 1), matching m_op_ptr.
Object *m_gc_roots_base{nullptr};
Object *m_gc_roots_ptr{nullptr};
Object *m_gc_roots_limit{nullptr};

// save stack
vm_offset_t *m_save_stack{nullptr};
save_generation_t *m_save_generation{nullptr};  // per-slot generation counter; bumped on each save reuse so a stale token (level,
                                                // gen ^ barrier_low23) detects mismatch
save_level_t m_max_save_level{0};
save_level_t m_curr_save_level{0};
vm_offset_t *m_extvalue_free_list{nullptr};
vm_offset_t *m_widevalue_free_list{nullptr};  // per-save-level free list for 16-byte WideValue cells (Int128/UInt128)
vm_t **m_vm_temp_save{nullptr};               // per-save-level snapshot of m_vm_temp_ptr
integer_t *m_extvalue_active_save{nullptr};   // per-save-level snapshot of m_extvalue_active_count
integer_t *m_widevalue_active_save{nullptr};  // per-save-level snapshot of m_widevalue_active_count
length_t *m_save_journal_count{nullptr};      // per-save-level journal entry count (O(1) query)
vm_size_t *m_save_journal_bytes{nullptr};     // per-save-level journal byte size (O(1) query, sum of vm_alloc'd Entry sizes)

// Name
vm_offset_t *m_name_buckets{nullptr};
vm_offset_t *m_systemname_offsets{nullptr};
vm_offset_t *m_typename_offsets{nullptr};
vm_offset_t *m_errorname_offsets{nullptr};
vm_offset_t *m_wellknown_offsets{nullptr};  // WellKnownName-indexed table of pre-interned name offsets
// Deliberately a fixed C++ array, NOT a vm_offset_t/Object* into VM like the
// m_wellknown_offsets sibling above: this is DERIVED data, rebuilt from
// m_wellknown_offsets by populate_wellknown_cache() on init + thaw, so it rides
// ZERO snapshot bytes (pattern: reconstruct-on-thaw, not ride-the-heap). Making
// it VM-resident would require a SnapShotHeader field + a SNAPSHOT_VERSION bump
// to persist reconstructible data, for ~0.7 KB of class space and no access-speed
// gain (a cached base pointer would just match m_root_objects_ptr). Keep as-is
// unless that trade ever becomes worthwhile.
Object m_wellknown_cache[WELLKNOWN_COUNT]{};  // prebaked Literal Name Objects; caller OR's in attrib
Object *m_root_objects_ptr{nullptr};          // RootObject-indexed array of heap-resident Objects
name_bucket_count_t m_name_bucket_count{0};
uint64_t m_name_bucket_magic{0};  // Lemire fastmod magic for m_name_bucket_count (recomputed on init/thaw)

// Stream
stream_id_t m_next_sid{0};
bool m_sid_wrapped{false};
stream_buffer_size_t m_stream_buffer_size{0};
Stream *m_stream_inuse_list{nullptr};
Stream *m_stream_free_list{nullptr};
Stream *m_stdin{nullptr};
Stream *m_stdedit{nullptr};
Stream *m_stdout{nullptr};
Stream *m_stderr{nullptr};

// Dicts
Dict *m_systemdict{nullptr};
Dict *m_protocoldict{nullptr};
Dict *m_userdict{nullptr};
Dict *m_errordict{nullptr};
Dict *m_handlersdict{nullptr};

// Per-save-level dict free list for recycling small dicts (locals, try-catch handlers).
// 2D array [save_count][MaxDictPoolSize] flattened into m_dict_pool, indexed by
// save_level * MaxDictPoolSize + (maxlength - 1).  Follows ExtValue's per-save-level
// bucketing: dicts freed at level N are only reused at level N.
vm_offset_t *m_dict_pool{nullptr};

// Frame-dict free list: separate from m_dict_pool, indexed by capacity only
// (no save-level dimension).  Holds dicts with Dict::IsFrame set -- i.e. the
// dicts pushed by |locals|#N begin_locals_op.  The save/restore machinery
// treats frame dicts as transparent (see save.inl check_and_compact_dict_stack),
// so the pool is level-free: frame dicts recycled at level 5 are available
// for reuse at level 5, level 3, level 0, whatever current_save_level is at
// the next begin-locals.  On restore, entries whose offset is above the
// rollback barrier are pruned (their storage is about to be reclaimed).
vm_offset_t *m_frame_dict_pool{nullptr};

// Overflow chain for frame dicts whose maxlength exceeds MaxDictPoolSize
// (the indexed pool covers maxlength 1..MaxDictPoolSize only).  Single
// linked list threaded via Dict::m_pool, scanned linearly on alloc to
// find a parked dict with matching maxlength.  Pay-as-you-go: only sizes
// actually freed appear here, so a program that never recycles an
// oversized frame dict pays zero overhead.  Same restore-prune treatment
// as m_frame_dict_pool: above-barrier entries are dropped.
vm_offset_t m_frame_dict_overflow{nulloffset};

// Pool-dynamics visibility: counts every frame-dict pool entry dropped by
// Save::restore's prune walk (both the indexed buckets and the overflow
// chain).  Each eviction means a recycled-but-above-barrier dict whose
// storage is about to be reclaimed by the VM rollback -- the next request
// of that capacity will allocate fresh instead of reusing.  Cheap (one
// uint64_t bump per drop) and ALWAYS-ON because it's load-bearing for
// understanding cross-actor save/restore dynamics, not just heap-tracking.
uint64_t m_frame_dict_pool_evictions{0};

// Class-indexed mailbox free list for recycling dead-actor mailboxes.
// 1D array [MaxMailboxPoolSize], indexed by class_index.  Size classes
// in sm_mailbox_pool_classes[].  Mailboxes live in global VM (BASE-immune
// to save/restore), so the free list needs no per-save-level dimension.
vm_offset_t *m_mailbox_pool{nullptr};

// Single-head MonitorEntry free list (fixed 24-byte struct).  Free-list
// link in m_next_target.  MonitorEntries live in global VM (BASE-immune
// to save/restore), so the free list needs no per-save-level dimension.
vm_offset_t m_monitor_pool{nulloffset};

// Per-save-level pipe-buffer free list.  2D array [save_count][MaxPipePoolSize]
// flattened, indexed by save_level * MaxPipePoolSize + class_index.  Size
// classes in sm_pipe_pool_classes[].  Buffers freed at save level N are only
// reused at level N.  Freed when BOTH producer and consumer have signaled
// done (pipe-close + pipe-get-returns-/empty).
vm_offset_t *m_pipe_pool{nullptr};

// scanner
Stream *m_scanner_stream{nullptr};
SourceLocation m_last_scan_location{};
// Interactive debugger state.  Whole pair is gated out in release builds
// (TRIX_DEBUGGER undefined) -- see src/build_config.inl.  When gated out
// the dispatch_loop hot-path check (m_debug_active) folds away too, and
// debug_op / debug_hook / debug_describe are not compiled.
#ifdef TRIX_DEBUGGER
DebugState m_debug{};
bool m_debug_active{false};
// Per-proc body source-line annotation -- populated by the scanner at
// proc-finalize time, queried by `proc-disasm` to fill each row's line
// column.  Key: body offset (Array storage offset or Packed data
// offset).  Value: int32 per body element.  Pruned in Save::restore
// for entries above the rewind barrier so reused offsets don't carry
// stale lines from prior allocations.
//
// STL container is permitted here under the TRIX_DEBUGGER carve-out
// (see [[feedback_no_stl_heap_containers]]) -- the natural VM-resident
// design would be Dict<Integer, Packed of Int16> plus a GC-walker hook
// for the packed bodies, 3-4x the code with no debugger-UX benefit.
// Whole map compiles out in release builds.
std::unordered_map<vm_offset_t, std::vector<int32_t>> m_debug_proc_lines{};
#endif
bool m_sandbox{false};
bool m_quiet{false};  // suppress all diagnostic stderr output (banners, backtraces, error messages)
// Set TRIX_BT_VERBOSE=1 (env var) to opt format_backtrace into uncapped
// output: all operands shown (not just top 8), preview rendered for every
// composite operand (not just the first found), uncapped preview lengths.
// Read once at construction time -- env-var changes during a run don't
// take effect.  Quiet mode wins: TRIX_BT_VERBOSE has no effect when -q is set.
bool m_bt_verbose{std::getenv("TRIX_BT_VERBOSE") != nullptr};

// vm-heap-tracking: per-source-line allocation counters and
// per-block side-table.  Tables live in the VM heap (allocated once at
// init); only pointers + counters sit in the Trix struct.  Whole block
// is gated out in release builds (TRIX_HEAP_TRACKING undefined) -- see
// src/build_config.inl.
//
// m_alloc_tracking_paused starts true so init's own vm_alloc calls
// (including the one that allocates m_alloc_stats itself) do not
// recurse into a nullptr table.  init.inl flips it false after the
// tables are in place.  The alloc-stats / vm-heap-diff ops also
// re-pause while building their results so the measurement does not
// measure itself.
#ifdef TRIX_HEAP_TRACKING
AllocStatsEntry *m_alloc_stats{nullptr};
size_t m_alloc_stats_capacity{0};  // power of two (hash mask); scaled to VM size at init
size_t m_alloc_stats_count{0};
bool m_alloc_stats_saturated{false};
bool m_alloc_tracking_paused{true};
HeapTrackEntry *m_heap_track{nullptr};
size_t m_heap_track_count{0};
size_t m_heap_track_capacity{0};
uint32_t m_heap_track_generation{0};
bool m_heap_track_saturated{false};
// Peak m_vm_ptr and (m_vm_limit - m_vm_temp_ptr) seen since boot or last
// clear-alloc-stats; reveals transient spikes the periodic trace can't see.
vm_t *m_vm_peak_ptr{nullptr};
vm_size_t m_vm_peak_temp_used{0};
#endif

// raw-mode terminal state.  Per-instance
// fields track whether THIS Trix is in raw mode and what termios to restore.
bool m_raw_mode{false};
bool m_raw_mode_signal_handler_installed{false};
termios m_saved_termios{};
int m_stdin_orig_flags{0};                       // saved fcntl flags so cooked-mode restores O_NONBLOCK cleanly
vm_offset_t m_stdin_blocked_reader{nulloffset};  // single coroutine blocked in read-key-byte (SPSC)

// Process-global raw-mode handler state.  sigaction() and atexit() are
// process-wide, so the first Trix instance to call raw-mode wins ownership of
// the SIGINT/SIGTERM/SIGHUP/SIGQUIT dispositions.  The handler reads
// sm_atexit_termios directly (no Trix * available in a signal handler) to
// restore cooked mode on abnormal termination.
static inline termios sm_atexit_termios{};
static inline int sm_atexit_termios_fd{-1};
static inline volatile sig_atomic_t sm_atexit_termios_valid{0};
static inline struct sigaction sm_prev_sigint{};
static inline struct sigaction sm_prev_sigterm{};
static inline struct sigaction sm_prev_sighup{};
static inline struct sigaction sm_prev_sigquit{};

// error handling
bool m_error_init_complete{false};  // true after Dict::init sets error pointers; guards error() during init
vm_t *m_error_string_base{nullptr};
vm_t *m_error_string_limit{nullptr};
Object *m_last_error_name_ptr{nullptr};
Object *m_last_operator_ptr{nullptr};
Object *m_last_error_msg_ptr{nullptr};
Object *m_last_error_data_ptr{nullptr};
Object *m_ostack_ptr{nullptr};
Object *m_dstack_ptr{nullptr};
Object *m_estack_ptr{nullptr};

// //:status: scratch Object for on-demand introspection results
Object m_status_scratch_obj{};

// Infix expression scanner: shared scratch for Pratt parser output.
// Only one scan_infix_expression() is active at a time (no recursion, cooperative coroutines).
Object m_infix_scratch_objs[MaxInfixTokens]{};

// Current high-water mark in m_infix_scratch_objs during a $() parse.
// infix_emit bumps this on every successful emission; the
// InfixScratchGuard destructor reads it to reclaim ExtValues from
// slots [0..m_infix_scratch_count) on error unwind.  Reset to 0 by
// the guard's constructor and destructor.  Live only during a single
// scan_infix_expression call; not snapshot-serialised.
length_t m_infix_scratch_count{0};

// temporary use container Objects
Object *m_eqproc_storage_ptr{nullptr};
length_t m_eqproc_length{0};
length_t m_eqproc_stored_length{0};  // number of valid Objects currently in m_eqproc_storage_ptr (0 if packed bytes)
// Generation counters for eq-storage references.  Each eq-kind has its own counter so
// writes to one buffer don't invalidate refs of another kind.  Bumped on every new eqref
// creation.  Eqref Objects (Array/Packed/String/Dict/Set with SpecialFlag set) store their
// creation-time generation in m_generation (a union alias over the value slot) instead of
// a VM offset.  Access validates the stored generation against the current counter; a
// mismatch means the storage was reused and the reference is stale, so access raises
// Unsupported rather than returning corrupted data.
// Monotonic -- never rolled back on restore (stale refs stay stale after restore).
uint32_t m_eqproc_generation{0};
uint32_t m_eqstring_generation{0};
uint32_t m_eqarray_generation{0};
uint32_t m_eqdict_generation{0};
uint32_t m_eqset_generation{0};
Dict *m_eqdict{nullptr};
length_t m_eqdict_maxlength{0};

Dict *m_eqset{nullptr};
length_t m_eqset_maxlength{0};

// require operator: tracks loaded file paths
vm_offset_t m_require_dict_offset{nulloffset};

// module system: registered module dicts
vm_offset_t m_modules_dict_offset{nulloffset};

// Module search path: colon-separated entries searched by require /
// require-module when cwd-resolution fails on a relative path.  User
// entries searched first, then binary-relative `lib/` (m_bin_dir + "/lib").
// Pointers reference long-lived storage (argv backing store + parse_args
// static buffer); valid for process lifetime.
const char *m_module_path{nullptr};
const char *m_bin_dir{nullptr};
// Full resolved path to the running binary (realpath of argv[0]); surfaced by
// the executable-path operator.  Long-lived (parse_args static buffer).
const char *m_exe_path{nullptr};

// --inspect variants (consumed by Stream::init when m_mode ==
// InspectFile to synthesize the boot script that loads lib/debugger.trx
// and installs the right install-debugger variant before running the
// user script).  m_inspect_at_name references the argv backing store.
// Debugger-only: the only readers are inside push_inspect_boot, which is
// itself TRIX_DEBUGGER-gated, so these compile out of non-debugger builds
// along with the --inspect CLI flags in api.inl.
#ifdef TRIX_DEBUGGER
const char *m_inspect_at_name{nullptr};
bool m_inspect_on_error{false};
bool m_no_color{false};
#endif

// protocol registry: maps protocol-name -> dict of method dispatch dicts
vm_offset_t m_protocol_registry_offset{nulloffset};

// gen-server
uint64_t m_gen_ref_counter{0};

// interpreter
const Operator *m_useroperators{nullptr};

const vm_t *m_invoke_data{nullptr};
size_t m_invoke_length{0};
Error m_last_error{Error::NoError};
Error m_external_error{Error::NoError};
bool m_in_global_handler{false};
// Number of objects try_catch_handler planted on the OPERAND stack for the
// error currently unwinding (1 = /error-name for try / try-catch barriers;
// 2 = failing-op + /error-name for the repl barrier; 0 otherwise).  The C++
// throw in error() unwinds THROUGH scan_procedure's catch(...) cleanup,
// which resets m_op_ptr to its scan-entry depth -- the cleanup consults this
// count to preserve the planted objects across that reset (they belong to
// the barrier's recovery atom, e.g. @try-rollback / @catch-error /
// @repl-recover, which pops them when it runs).  Reset by error() at entry
// and by the interpreter's Exception::Error catch once the unwind completes.
int8_t m_planted_error_objects{0};
// At-error err-stack-top snapshot, captured in error() before
// try_catch_handler unwinds.  Pops decrement m_err_ptr but do NOT erase
// the bytes -- they remain readable below the live ptr.  format_backtrace
// walks downward from this snapshot via the same err_scan pattern used in
// perform_op / capture_op, reading barrier companions for error enrichment.
// It also lets the debugger inspect pre-unwind state cheaply.
Object *m_error_err_ptr_snapshot{nullptr};
// Process-exit code: 0 on clean exit (quit_op or normal end-of-script),
// non-zero when an uncaught error reaches default_handler_op or when
// init_and_interpret's outer catch arms fire.  Read by main() after the
// Trix constructor returns; not part of save/restore.
int m_exit_code{0};
uint64_t m_op_count{0};
uint64_t m_max_ops{0};  // 0 = unlimited; halt with limit-check when reached

// Cumulative wall-clock park budget.  m_max_ops cannot tick while a
// coroutine is parked, so a bounded run is still stallable for arbitrary
// wall time by one huge sleep/timeout operand; when the budget is set,
// sleep_budget_grant caps the TOTAL granted park time and exhausted
// grants degrade to immediate wakes.  Not part of save/restore.
uint64_t m_sleep_budget_ms{0};   // 0 = unlimited; mirror of Config::m_sleep_budget_ms
uint64_t m_sleep_granted_ms{0};  // ms granted so far against the budget

// Script-side argv: snapshot of Config::m_script_argv/argc captured at init.
// Pointers reference the original process argv (lifetime = process); not
// part of save/restore or snap-shot.  command-line-args reads these.
int m_script_argc{0};
const char *const *m_script_argv{nullptr};
uint64_t m_tco_count{0};
Object *m_op_high_water_ptr{nullptr};
Object *m_exec_high_water_ptr{nullptr};
Object *m_dict_high_water_ptr{nullptr};
Object *m_err_high_water_ptr{nullptr};
integer_t m_extvalue_active_count{0};
integer_t m_widevalue_active_count{0};  // live WideValue count (Int128/UInt128)

interrupt_t m_interrupt{NoIRQ};
std::atomic<interrupt_t> m_pending_irq{NoIRQ};
interrupt_t m_interrupt_mask{NoIRQ};

// coroutine scheduler
vm_offset_t m_coroutine_head{nulloffset};     // head of registry list (circular, all coroutines)
vm_offset_t m_running_coroutine{nulloffset};  // currently running CoroutineContext offset (nulloffset before coroutine init)
vm_offset_t m_main_context{nulloffset};       // coroutine #0 (main program)
length_t m_live_coroutine_count{0};           // number of non-dead coroutines (excluding main)
vm_offset_t m_ready_head{nulloffset};         // head of normal-priority ready FIFO queue (nulloffset = empty)
vm_offset_t m_ready_tail{nulloffset};         // tail of normal-priority ready FIFO queue (nulloffset = empty)
vm_offset_t m_ready_high_head{nulloffset};    // head of high-priority ready FIFO queue (nulloffset = empty)
vm_offset_t m_ready_high_tail{nulloffset};    // tail of high-priority ready FIFO queue (nulloffset = empty)
vm_offset_t m_timer_head{nulloffset};         // head of sorted timer list (nulloffset = empty)
uint32_t m_next_coroutine_id{0};              // monotonic ID counter (never reused)
uint32_t m_coroutine_ops_remaining{0};        // live quantum counter for running coroutine (0 = unlimited)
uint32_t m_default_coroutine_quantum{0};      // default quantum for newly launched coroutines

// scheduler observability counters
uint64_t m_sched_count{0};              // total coroutine_schedule() calls
uint64_t m_sched_ready_pops{0};         // times a coroutine was popped from ready queue
uint64_t m_sched_timer_wakes{0};        // times timer_sweep_expired moved coroutines to ready
uint64_t m_sched_real_sleeps{0};        // times real thread slept waiting for timers
uint32_t m_ready_queue_depth{0};        // current normal-priority ready queue length
uint32_t m_ready_high_queue_depth{0};   // current high-priority ready queue length
uint32_t m_timer_list_depth{0};         // current timer list length
uint64_t m_starvation_threshold_ns{0};  // starvation detection threshold (ns, 0 = disabled)

vm_offset_t m_coroutine_stack_free{nulloffset};  // free list for coroutine stack blocks
vm_offset_t m_coroutine_ctx_free{nulloffset};    // free list for CoroutineContext blocks
vm_offset_t m_binding_bucket_free{nulloffset};   // free list for per-coroutine bucket arrays (binding_table.inl)
vm_offset_t m_binding_entry_free{nulloffset};    // free list for BindingEntry blocks (binding_table.inl)

// supervision
uint32_t m_next_monitor_ref{0};  // monotonic counter for monitor ref-ids

// GC re-entrancy guard.  Set true while a vm_global_gc()
// pass is in flight; cleared on exit.  Auto-on-VMFull retry hook in
// gvm_alloc consults this -- a VMFull raised from inside the GC's
// own vm_temp_alloc / gvm_free path must NOT recursively trigger
// another GC pass.
bool m_in_gc{false};

// Debug-only GC-stress toggle (vm-gc-stress operator).  When true, gvm_alloc
// fires a full vm_global_gc before every global user allocation so GC-rooting
// bugs surface deterministically -- a root dropped from a C local is swept right
// before the allocation that depends on it.  O(live-heap) per alloc: a test
// tool, never enabled in production.  Absent in release builds.
#ifdef TRIX_DEBUGGER
bool m_gc_stress{false};
#endif

// Debug-only GC-poison toggle (vm-gc-poison operator).  When true, gvm_free
// scribbles a freed block's payload with a poison byte pattern so a stray
// use-after-free READ returns an obviously-wrong value deterministically instead
// of stale-but-valid bits (which only corrupt when the slot is later reused).
// Orthogonal to m_gc_stress, but pairs with it: gc-stress fires the GCs that do
// the freeing, poison makes the resulting dangling reads deterministic.  A test
// tool, never enabled in production.  Absent in release builds.
#ifdef TRIX_DEBUGGER
bool m_gc_poison{false};
#endif

// Current GC mark-generation: a 1-bit flip-flop holding {0,1}.
// GvmBlock::m_mark_gen is the matching per-block tag.  A block is
// "alive in this pass" iff `header->m_mark_gen == m_gc_current_gen`.
//
// Every gc / probe pass flips m_gc_current_gen (XOR 1) before marking,
// which makes every old mark stale by definition -- no per-pass clear
// walk, ever.  This is sound because every cycle sweeps the whole
// region, so survivors always share one value and no older generation
// lingers to alias.  See gc.inl::gc_advance_generation.
//
// Initial value is 0; fresh blocks are stamped with the current value
// (gc_init_mark_for_alloc), and the first pass's flip to 1 makes them
// look "old", so no boot special-case is needed.  The field stays
// uint8_t for snapshot-layout stability but only ever uses {0,1}.
uint8_t m_gc_current_gen{0};

// GC introspection counters.  Bumped at end of every
// completed vm_global_gc pass; surfaced via //:status:gc-runs and
// //:status:gc-last-reclaimed.  Not snapshot/thaw'd: counters reset
// to 0 on thaw (a different process can't inherit the previous
// process's GC history meaningfully).
uint64_t m_gc_run_count{0};
vm_size_t m_gc_last_reclaimed_bytes{0};

// GC visited-offset set used to break cycles when gc_mark_object
// descends into a LOCAL-VM Dict / Set whose payload happens to hold
// global-VM refs (frame dicts in particular).  Local-VM blocks have no
// GvmBlock header to record m_mark_gen on, so the per-block already-
// marked short-circuit doesn't apply; this auxiliary set fills the
// gap.  Backed by the lazy GC scratch block (m_gc_scratch_offset)
// rather than vm_temp_alloc, so global pressure can't clamp
// m_vm_temp_ptr below the visited-set's 32 KiB and starve it.
// Empty / null between GC passes; m_gc_local_visited_count resets to
// 0 on each pass entry.  Not snapshot/thaw'd.
vm_offset_t *m_gc_local_visited{nullptr};
vm_size_t m_gc_local_visited_count{0};
vm_size_t m_gc_local_visited_capacity{0};

// In-block GC mark/sweep queue.  m_gc_work_head is the head of an
// intrusive linked list threaded through every queued block's
// GvmBlock::m_next_in_work field.  Used by mark-phase BFS (gc_work_push
// enqueues, gc_work_pop dequeues) and by the sweep-phase doomed list
// (the same field, in disjoint sub-phases of the same GC pass).
// Capacity is total_live by construction -- no fixed cap.
//
// nulloffset between GC passes; non-null only inside vm_global_gc.
// Not snapshot/thaw'd.
vm_offset_t m_gc_work_head{nulloffset};

// Incremented during the GC mark phase by gc_work_push.
// vm_global_gc_impl uses (marked_count == total_live) as the
// "no garbage to sweep" early-exit gate.  Reset to 0 at the start
// of every pass.  Not snapshot/thaw'd.
vm_size_t m_gc_marked_count{0};

// LOCAL-VM Dict/Set visit list head.  Intrusive linked list threaded
// through Dict::m_next_in_visit on every local Dict/Set visited from
// a global walk.  Dict + Set (the most common local-VM containers
// reached during global GC) use this list; Tagged/Curry/Thunk/Array/
// Record/Packed use the m_gc_local_visited set instead, since their
// counts are typically much smaller.
//
// Non-null only during the global GC mark phase; nulloffset between
// passes.  Not snapshot/thaw'd.
//
// Sentinel value VisitListEnd marks end-of-list (not nulloffset, so
// the on-list / off-list test for a Dict is `m_next_in_visit !=
// nulloffset`).  See Trix::gc_visit_push / gc_visit_pop.
vm_offset_t m_gc_visit_head{nulloffset};

// Lazy GC scratch block (ChunkKind::GcScratch).  Allocated on the
// first user gvm_alloc so it sits at the top of global VM, above
// every user block.  Holds the GC's local-visited set + the mark
// work queue (in that order in the payload).  Freed when the last
// user block is released and m_gvm_user_block_count returns to 0,
// so the global zone can advance back to m_vm_limit when idle.
// Not in temp space, so global pressure can't squeeze it out --
// it lives in the same region as the user blocks it serves.
//
// Snapshot/thaw: serializes with the global region (it's a regular
// gvm_alloc'd block); m_gc_scratch_offset is restored from the
// snapshot header.  m_gvm_user_block_count is recomputed from the
// heap walk on thaw.
//
// nulloffset between "no user globals" idle states.
vm_offset_t m_gc_scratch_offset{nulloffset};

// Count of LIVE user (non-GcScratch) blocks in the global heap.
// Maintained by gvm_alloc / gvm_free; the lazy scratch block is
// allocated when this transitions 0 -> 1, freed when it returns
// to 0.  Reduced to a counter rather than a heap walk to keep
// gvm_alloc / gvm_free O(1).  Recomputed on snapshot thaw.
vm_size_t m_gvm_user_block_count{0};

// Count of blocks currently on the global VM free list (main list +
// fastbins combined).  Maintained by the four lower-level free-list
// mutators in gvm_heap.inl: gvm_free_list_insert / gvm_free_list_remove
// / gvm_fastbin_insert / gvm_fastbin_unlink (the dispatchers
// gvm_free_*_anywhere forward to these, and the alloc path calls them
// directly).  Exposed via //:status:vm-global-num-free for O(1)
// monitoring vs the O(N) vm-global-info heap walk.  Snapshot-serialized.
vm_size_t m_gvm_free_block_count{0};

// reactive cells
vm_offset_t m_current_cell{nulloffset};       // cell being evaluated (for dependency recording); nulloffset when idle
integer_t m_batch_depth{0};                   // nesting depth of batch calls; 0 when not in batch
vm_offset_t m_deferred_watchers{nulloffset};  // array of BatchEntry pairs pending watcher fire; nulloffset when empty
length_t m_deferred_watcher_count{0};         // number of valid entries in m_deferred_watchers

// cache-line cold data
std::mutex m_mutex;
std::condition_variable m_cond;
int m_pipe_fds[2];
PCG32 m_pcg32;
std::atomic_bool m_active;
