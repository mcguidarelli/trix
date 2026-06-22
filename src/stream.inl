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

//===--- Stream: I/O and Memory Buffer ---===//
//
// Implements the Stream class: a unified abstraction for file I/O, memory
// buffers, standard streams, and the interactive readline editor.  Streams
// are both data sources/sinks and executable objects (the interpreter scans
// tokens from executable streams).  Based on:
//
//   PostScript file objects: readable/writable/seekable file handles with
//   token scanning.  `currentfile` and `run` execute code from streams.
//   The scanner reads from the "current file" on the exec stack.
//
//   C stdio (FILE*): buffered I/O with read/write/seek operations, but
//   Trix streams use a fixed-size buffer on the VM heap rather than
//   libc-managed buffers.
//
// --- Core concepts for maintainers ---
//
// STREAM TYPES
//   File streams:   backed by a file descriptor (open/close/read/write/seek).
//   Memory streams: backed by a host-provided byte buffer (invoke API).
//     Read-only, no seek.  Used by the invoke() host API to inject code.
//   String-write:   writable in-memory string-stream of fixed capacity on the
//     VM heap (no fd, IsStringWrite).  Created by make-string-stream;
//     buffer-full at write raises /io-write-error; get-string-stream returns
//     the accumulated bytes.
//   Stdin:          block-buffered reading from fd 0 (no readline).
//   Stdedit:        interactive input via GNU readline (prompt, history,
//     line editing).  Used for the REPL.
//   Stdout/Stderr:  output-only streams for print operators.
//
// STREAM MANAGEMENT
//   All streams are managed through two linked lists: m_stream_inuse_list
//   (open streams) and m_stream_free_list (available for reuse).  Streams
//   are allocated on the VM heap.  On close, a stream moves from the inuse
//   list to the free list.  The maximum number of simultaneous open streams
//   is configurable (--stream-count).
//
// BUFFERING
//   Each stream has a fixed-size buffer on the VM heap (configurable via
//   --stream-buffer).  Reads fill the buffer in chunks; writes flush when
//   full or on explicit flush.  The buffer is a simple linear region with
//   read/write pointers, refilled by fill(), which dispatches on the stream
//   kind (memory chunk, stdin/stdedit, or file fd) when the read pointer
//   reaches the limit.
//
// SCANNER INTEGRATION
//   Streams serve as token sources for the interpreter.  Stream::scanner()
//   reads characters from the stream and produces (Lexeme, Object) pairs.
//   When an executable stream is on the exec stack, the interpreter calls
//   scanner() to get the next token, executes it, and pushes the stream
//   back for the next iteration.
//
//   The scanner handles: whitespace, comments (% and %{...%}), strings
//   ((..)), raw strings (<(...)>), hex strings (<..>), names (/name,
//   \name), dictionary paths (//:dict:key), numbers, procedures ({..}),
//   arrays ([..]), dicts (<<..>>), sets ({{..}}), infix expressions
//   ($(...) and $[...]), binary tokens (0x80-0xFF range), and string
//   interpolation (\{name}).
//
// STREAM STATUS
//   Streams track read/write direction, EOF state, error state, and
//   access permissions.  A stream opened for reading cannot be written
//   to (and vice versa) without an intervening seek on seekable streams.
//
// STARTUP MODES
//   Stream::init() configures the initial input source based on
//   StartupMode (from command-line parsing):
//     ScriptFile:        open file, push [Quit, @Run, file-stream]
//     StdIn:             raw stdin, push [Quit, @Run, stdin-stream]
//     Interactive:       readline stdedit, push [Quit, @Run, stdedit-stream]
//     FileAndInteractive: push stdedit then file (file runs first)
//     InspectFile:       (TRIX_DEBUGGER builds only) wrap the script in a
//       synthesized boot that loads lib/debugger.trx, installs the chosen
//       --inspect variant, then `run`s the user script from file; the
//       synthesized boot itself is executed as a memory stream
//
class Stream {
public:
    using status_t = uint16_t;

    // ReadData / WriteData track the direction of the most recent I/O operation.
    // Trix requires that setfileposition on a seekable read/write stream is only
    // legal at a read->write or write->read boundary -- not mid-sequence.  These flags
    // let the setfileposition implementation enforce that constraint without external state.
    static constexpr status_t IsClosed{0x0000};
    static constexpr status_t IsOpen{0x0001};
    static constexpr status_t IsReadable{0x0002};
    static constexpr status_t IsWritable{0x0004};
    static constexpr status_t IsStdIO{0x0008};
    static constexpr status_t IsFile{0x0010};
    static constexpr status_t IsStartupFile{0x0020};
    static constexpr status_t IsString{0x0040};
    static constexpr status_t IsMemory{0x0080};
    static constexpr status_t SupportsRandomAccess{0x0100};
    static constexpr status_t ReadData{0x0200};
    static constexpr status_t WriteData{0x0400};
    static constexpr status_t IsAppend{0x0800};       // stream was opened in append mode ('a'/'A')
    static constexpr status_t IsStringWrite{0x1000};  // writable in-memory string-stream (no fd, buffer-full -> /io-write-error)
    static constexpr status_t IsBorrowed{
            0x2000};  // m_ext_base points at VM-heap (or otherwise caller-owned) bytes -- do NOT std::free in close()

    // SID (stream ID) reservation prevents stale Stream Objects from silently aliasing
    // newly-opened streams.  is_open(sid) compares the caller's SID against m_sid: if
    // the stream slot was closed and reopened, m_sid will have changed and the old Object
    // returns false rather than accessing the wrong stream.
    // SIDs 0..6 are permanently assigned to well-known streams and never reused.
    // Dynamic streams are allocated from FIRST_SID upward.
    static constexpr stream_id_t INVALID_SID{0};
    static constexpr stream_id_t STDIN_SID{1};
    static constexpr stream_id_t STDEDIT_SID{2};
    static constexpr stream_id_t STDOUT_SID{3};
    static constexpr stream_id_t STDERR_SID{4};
    static constexpr stream_id_t STRING_SID{5};
    static constexpr stream_id_t STARTUP_SID{6};
    static constexpr stream_id_t FIRST_SID{7};

    [[nodiscard]] stream_id_t sid() const { return m_sid; }

    [[nodiscard]] bool is_open(stream_id_t sid) const { return ((sid == m_sid) && ((m_status & IsOpen) != 0)); }

    [[nodiscard]] bool is_readable(stream_id_t sid) const { return ((sid == m_sid) && ((m_status & IsReadable) != 0)); }

    [[nodiscard]] bool is_writable(stream_id_t sid) const { return ((sid == m_sid) && ((m_status & IsWritable) != 0)); }

    [[nodiscard]] bool is_stdio() const { return (((m_status & IsStdIO) != 0)); }

    [[nodiscard]] bool is_file() const { return (((m_status & IsFile) != 0)); }

    [[nodiscard]] bool is_startup_file() const { return (((m_status & IsStartupFile) != 0)); }

    [[nodiscard]] bool is_string() const { return (((m_status & IsString) != 0)); }

    [[nodiscard]] bool is_memory() const { return (((m_status & IsMemory) != 0)); }

    [[nodiscard]] bool is_string_write() const { return (((m_status & IsStringWrite) != 0)); }

    [[nodiscard]] bool supports_random_access() const { return (((m_status & SupportsRandomAccess) != 0)); }

    [[nodiscard]] std::string_view source(Trix *trx) const {
        assert(m_source.is_string());

        return m_source.sv_value(trx);
    }

    [[nodiscard]] bool last_op_read() const { return (((m_status & ReadData) != 0)); }

    [[nodiscard]] bool last_op_write() const { return (((m_status & WriteData) != 0)); }

    struct SourceLocation {
        std::string_view source;
        uint32_t line;
        uint16_t column;
    };

    [[nodiscard]] SourceLocation source_location(Trix *trx) const {
        assert(m_source.is_string());

        return SourceLocation{m_source.sv_value(trx), m_line_number, static_cast<uint16_t>(m_line_pos - 1)};
    }

#include "scanner.inl"

    static void init(Trix *trx,
                     stream_enable_t stream_enable,
                     stream_count_t stream_count,
                     stream_buffer_size_t stream_buffer_size,
                     const char *startup_filename,
                     StartupMode mode,
                     bool resident) {
        // create the memory pool of Stream and i/o buffers
        auto embedded_size = static_cast<vm_offset_t>(sizeof(Stream::m_buffer));
        auto extra_size = (stream_buffer_size <= embedded_size) ? 0 : (stream_buffer_size - embedded_size);
        auto alloc_size = vm_sizeof<Stream>() + extra_size;
        trx->m_next_sid = FIRST_SID;
        trx->m_sid_wrapped = false;
        {
            using namespace std::literals::string_view_literals;

            trx->set_root_object(RootObject::StdInString, Object::make_string(trx, "--stdin--"sv));
            trx->set_root_object(RootObject::StdEditString, Object::make_string(trx, "--stdedit--"sv));
            trx->set_root_object(RootObject::StdOutString, Object::make_string(trx, "--stdout--"sv));
            trx->set_root_object(RootObject::StdErrString, Object::make_string(trx, "--stderr--"sv));
            trx->set_root_object(RootObject::StringString, Object::make_string(trx, "--string--"sv));
            trx->set_root_object(RootObject::MemoryString, Object::make_string(trx, "--memory--"sv));
        }
        trx->m_stream_buffer_size = stream_buffer_size;
        trx->m_stream_inuse_list = nullptr;
        trx->m_stream_free_list = nullptr;
        // allocate per-save-level dict pool: [max_save_level][MaxDictPoolSize]
        auto dict_pool_size = static_cast<vm_size_t>(trx->m_max_save_level * MaxDictPoolSize);
        trx->m_dict_pool = trx->vm_alloc_n_ptr<vm_offset_t>(dict_pool_size);
        std::fill_n(trx->m_dict_pool, dict_pool_size, nulloffset);
        // allocate frame-dict pool: capacity-indexed only, no save-level dimension
        trx->m_frame_dict_pool = trx->vm_alloc_n_ptr<vm_offset_t>(MaxDictPoolSize);
        std::fill_n(trx->m_frame_dict_pool, MaxDictPoolSize, nulloffset);
        // Mailbox pool is class-indexed only ([MaxMailboxPoolSize]).
        // Mailboxes live in global VM (BASE-immune to save/restore), so it
        // needs no per-save-level dimension.
        auto mailbox_pool_size = static_cast<vm_size_t>(MaxMailboxPoolSize);
        trx->m_mailbox_pool = trx->vm_alloc_n_ptr<vm_offset_t>(mailbox_pool_size);
        std::fill_n(trx->m_mailbox_pool, mailbox_pool_size, nulloffset);
        // m_monitor_pool is a single bare offset (no per-save-level array).
        // Default-init by member-var initialiser (nulloffset); no
        // allocation needed at runtime init.
        // Pipe pool is class-indexed only ([MaxPipePoolSize]).  Pipe
        // buffers live in global VM (BASE-immune to save/restore), so it
        // needs no per-save-level dimension -- mirror of the mailbox pool.
        auto pipe_pool_size = static_cast<vm_size_t>(MaxPipePoolSize);
        trx->m_pipe_pool = trx->vm_alloc_n_ptr<vm_offset_t>(pipe_pool_size);
        std::fill_n(trx->m_pipe_pool, pipe_pool_size, nulloffset);
// allocate the vm-heap-tracking alloc-stats hash table and the
// per-block side-table.  Tracking is paused during these
// allocations (m_alloc_tracking_paused defaults to true) and
// turned on after the tables are in place; init's earlier
// vm_alloc calls are not counted, which is the correct semantics
// (init builds the runtime, not the user program).
#ifdef TRIX_HEAP_TRACKING
        // Scale the instrumentation tables to the heap: at full size the
        // alloc-stats hash (24 KB) plus the per-block side-table (256 KB)
        // outweigh MinVmSize itself, so a tracking build could never boot
        // at the advertised minimum.  At DefaultVmSize and above both
        // tables stay full-size (existing configurations are bit-for-bit
        // unchanged, including vm-full exhaustion landscapes tests pin);
        // smaller heaps get proportional tables (alloc-stats 1/32 of the
        // VM, bit_floor keeps the hash mask a power of two; side-table
        // 1/8) and rely on the saturation flags.
        auto vm_bytes = static_cast<size_t>(trx->m_vm_limit - trx->m_vm_base);
        auto full_tables = (vm_bytes >= static_cast<size_t>(DefaultVmSize));
        auto alloc_stats_capacity =
                full_tables ? AllocStatsTableSize
                            : std::min(AllocStatsTableSize, std::bit_floor((vm_bytes / 32) / sizeof(AllocStatsEntry)));
        trx->m_alloc_stats_capacity = alloc_stats_capacity;
        trx->m_alloc_stats = trx->vm_alloc_n_ptr<AllocStatsEntry>(static_cast<vm_size_t>(alloc_stats_capacity));
        std::fill_n(trx->m_alloc_stats, alloc_stats_capacity, AllocStatsEntry{});
        auto heap_track_capacity =
                full_tables ? HeapTrackTableSize : std::min(HeapTrackTableSize, (vm_bytes / 8) / sizeof(HeapTrackEntry));
        trx->m_heap_track_capacity = heap_track_capacity;
        trx->m_heap_track = trx->vm_alloc_n_ptr<HeapTrackEntry>(static_cast<vm_size_t>(heap_track_capacity));
        // explicit zero-init not strictly needed (m_heap_track_count == 0 means
        // "no live entries"), but cheap insurance against reading uninitialized
        // memory if a future bug walks past the live range.
        std::fill_n(trx->m_heap_track, heap_track_capacity, HeapTrackEntry{});
        // peak trackers start at the post-init m_vm_ptr / current temp usage.
        trx->m_vm_peak_ptr = trx->m_vm_ptr;
        trx->m_vm_peak_temp_used = static_cast<vm_size_t>(trx->m_vm_limit - trx->m_vm_temp_ptr);
        trx->m_alloc_tracking_paused = false;
#endif
        while (stream_count-- != 0) {
            auto stream = trx->vm_alloc_ptr<Stream>(alloc_size);
            stream->m_next_offset = trx->nullptr_to_offset(trx->m_stream_free_list);
            stream->m_fd = -1;
            stream->m_sid = INVALID_SID;
            stream->m_status = IsClosed;
            trx->m_stream_free_list = stream;
        }

        // Create a standard I/O stream with the embedded buffer size.
        auto create_stdio = [&](stream_enable_t flag, Object source_obj, int fd, stream_id_t sid, status_t status) -> Stream * {
            if ((stream_enable & flag) != 0) {
                auto s = trx->vm_alloc_ptr<Stream>();
                if (s != nullptr) {
                    const stream_buffer_size_t buffer_size = sizeof(Stream::m_buffer);
                    s->config_buffered(trx, source_obj, fd, sid, Save::BASE, (IsOpen | IsStdIO | status), buffer_size);
                }
                return s;
            } else {
                return nullptr;
            }
        };

        // Create a standard I/O stream with a custom (larger) buffer.
        auto create_stdio_buffered = [&](stream_enable_t flag,
                                         Object source_obj,
                                         int fd,
                                         stream_id_t sid,
                                         status_t status,
                                         stream_buffer_size_t buf_size) -> Stream * {
            if ((stream_enable & flag) != 0) {
                auto embedded = static_cast<vm_offset_t>(sizeof(Stream::m_buffer));
                auto extra = (buf_size <= embedded) ? vm_offset_t{0} : static_cast<vm_offset_t>(buf_size - embedded);
                auto s = trx->vm_alloc_ptr<Stream>(vm_sizeof<Stream>() + extra);
                if (s != nullptr) {
                    s->config_buffered(trx, source_obj, fd, sid, Save::BASE, (IsOpen | IsStdIO | status), buf_size);
                }
                return s;
            } else {
                return nullptr;
            }
        };

        trx->m_stdin = create_stdio_buffered(
                StdInEnable, trx->root_object(RootObject::StdInString), STDIN_FILENO, STDIN_SID, IsReadable, StdIoBufferSize);

        trx->m_stdedit =
                create_stdio(StdEditEnable, trx->root_object(RootObject::StdEditString), STDIN_FILENO, STDEDIT_SID, IsReadable);
#ifndef TRIX_NO_READLINE
        if (trx->m_stdedit != nullptr) {
            ::rl_catch_signals = 0;
            ::rl_catch_sigwinch = 0;
            ::rl_initialize();
        }
#endif

        trx->m_stdout = create_stdio_buffered(
                StdOutEnable, trx->root_object(RootObject::StdOutString), STDOUT_FILENO, STDOUT_SID, IsWritable, StdIoBufferSize);
        trx->m_stderr = create_stdio_buffered(
                StdErrEnable, trx->root_object(RootObject::StdErrString), STDERR_FILENO, STDERR_SID, IsWritable, StdIoBufferSize);

        // Helper: push an executable stream onto the exec stack with @Run
        // below (the Quit floor is pushed once by the mode dispatch).
        auto push_exec_stream = [&](Stream *stream, stream_id_t sid, Object::access_t access) {
            auto offset = trx->ptr_to_offset(stream);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRun);
            *++trx->m_exec_ptr = Object::make_stream(offset, sid, Object::ExecutableAttrib, access);
        };

        // Helper: push a stdedit REPL frame above the Quit floor:
        // [@repl-barrier, @Run, stdedit].  The barrier keeps uncaught errors
        // from unwinding past the top level -- try_catch_handler rewrites it
        // to @repl-recover, which prints and re-arms.
        auto push_repl_stream = [&](Stream *stream, stream_id_t sid) {
            auto offset = trx->ptr_to_offset(stream);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atReplBarrier);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRun);
            *++trx->m_exec_ptr = Object::make_stream(offset, sid, Object::ExecutableAttrib, Object::ReadOnlyAccess);
        };

        // Helper: open and push a startup file stream.  Returns true on success.
        auto push_startup_file = [&]() -> bool {
            if (startup_filename != nullptr) {
                auto startup = alloc_stream(trx);
                if (startup != nullptr) {
                    auto fd = ::open64(startup_filename, (O_RDONLY | O_LARGEFILE | O_CLOEXEC));
                    if (fd >= 0) {
                        ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
                        auto len = static_cast<length_t>(std::strlen(startup_filename));
                        auto fn_obj = Object::make_string(trx, startup_filename, len);
                        auto sid = STARTUP_SID;
                        startup->config_buffered(trx,
                                                 fn_obj,
                                                 fd,
                                                 sid,
                                                 trx->m_curr_save_level,
                                                 (IsOpen | IsReadable | IsFile | IsStartupFile),
                                                 trx->m_stream_buffer_size);
                        auto offset = trx->ptr_to_offset(startup);
                        // push file stream (no Quit/@Run -- caller arranges the full stack)
                        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRun);
                        *++trx->m_exec_ptr = Object::make_stream(offset, sid, Object::ExecutableAttrib, Object::ReadOnlyAccess);
                        return true;
                    } else {
                        trx->diag_println("Trix could not open startup file {}", startup_filename);
                        startup->free_stream(trx);
                        trx->m_exit_code = +Error::FileOpenError;
                    }
                } else {
                    trx->diag_println("Trix could not allocate a 'stream' for startup file {}", startup_filename);
                    trx->m_exit_code = +Error::LimitCheck;
                }
            }
            return false;
        };

        // Helper: synthesize a Trix boot string that loads lib/debugger.trx,
        // installs the chosen variant of the debugger, and runs the user
        // script.  Allocates the source bytes in VM, opens them as a
        // memory stream, and pushes [@Run, exec-stream] above the Quit floor.
        // Returns false on any failure (caller falls back to stderr message).
        //
        // Debugger-only: the synthesized boot script invokes install-debugger /
        // debug-break / ... which are TRIX_DBG-gated ops, and the std::string here is
        // the STL carve-out for that debug-only data.  Compiled out (with its only
        // caller) in non-debugger builds.
#ifdef TRIX_DEBUGGER
        auto push_inspect_boot = [&]() -> bool {
            if (startup_filename == nullptr) {
                return false;
            } else {
                // Build the boot source.  We escape \, (, ) in the embedded
                // paths/names because Trix `(...)` string literals treat
                // unbalanced parens and backslashes specially.
                auto append_escaped = [](std::string &out, std::string_view sv) {
                    for (auto ch : sv) {
                        if ((ch == '\\') || (ch == '(') || (ch == ')')) {
                            out.push_back('\\');
                        }
                        out.push_back(ch);
                    }
                };
                std::string boot;
                boot.reserve(256);
                boot.append("(lib/debugger.trx) require\n");
                if (trx->m_no_color) {
                    boot.append("dbg-set-mono-theme\n");
                }
                if (trx->m_inspect_at_name != nullptr) {
                    // Accept --inspect-at /foo OR --inspect-at foo; strip any
                    // leading "/" so we always emit a single-slash literal Name.
                    auto at_name = std::string_view{trx->m_inspect_at_name};
                    while (!at_name.empty() && (at_name.front() == '/')) {
                        at_name.remove_prefix(1);
                    }
                    boot.append("/");
                    append_escaped(boot, at_name);
                    boot.append(" (");
                    append_escaped(boot, startup_filename);
                    boot.append(") install-debugger-at\n");
                } else if (trx->m_inspect_on_error) {
                    boot.append("(");
                    append_escaped(boot, startup_filename);
                    boot.append(") install-debugger-on-error\n");
                } else {
                    boot.append("(");
                    append_escaped(boot, startup_filename);
                    boot.append(") install-debugger\n");
                }
                boot.append("(");
                append_escaped(boot, startup_filename);
                boot.append(") run\n");
                // Normal-exit path: when the user script returns control rather
                // than calling `quit`, the boot stream continues here.  Drain
                // the output capture (stdout swap + tail print) and tear down
                // the alt-screen/raw-mode so the host shell sees a clean
                // terminal.  The `quit` action inside the cb calls
                // uninstall-debugger directly, so this line is a no-op for the
                // quit path; uninstall-debugger is idempotent.
                boot.append("uninstall-debugger\n");

                // Copy into VM-owned bytes so the memory stream can hold a
                // borrowed pointer for the process lifetime.
                auto length = static_cast<length_t>(boot.size());
                auto [bytes, _] = trx->vm_alloc<vm_t>(static_cast<vm_size_t>(length) + 1);
                std::copy_n(reinterpret_cast<const vm_t *>(boot.data()), length, bytes);
                bytes[length] = '\0';

                auto result = Stream::open_memory(trx, bytes, length, /*owns_data=*/false);
                if (!result.success) {
                    trx->diag_println("Trix could not open inspect boot stream");
                    return false;
                } else {
                    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRun);
                    *++trx->m_exec_ptr =
                            Object::make_stream(result.offset, result.sid, Object::ExecutableAttrib, Object::ReadOnlyAccess);
                    return true;
                }
            }
        };
#endif  // TRIX_DEBUGGER

        // Exec-stack floor shared by every startup mode: Quit runs when the
        // streams a mode pushes above it are exhausted.  A mode arm that
        // fails (and pushes nothing) falls straight through to it, so the
        // interpreter exits with the m_exit_code the arm recorded instead
        // of parking forever on the IRQ wait.
        //
        // Resident instances (--resident / Config::m_resident) skip the
        // floor: when their startup work drains, the empty exec stack parks
        // the interpreter on the IRQ wait (see interpreter.inl IDLE WAIT)
        // and it serves invoke()/raise_interrupt() work items until a
        // delivered quit or ExitIRQ stops it.
        if (!resident) {
            *++trx->m_exec_ptr = Object::make_operator(SystemName::Quit);
        }

        if (mode == StartupMode::ScriptFile) {
            // Mode 1: run file, exit
            push_startup_file();
        }
#ifdef TRIX_DEBUGGER
        else if (mode == StartupMode::InspectFile) {
            // InspectFile mode: wrap the user script in a debugger boot.
            // Synthetic boot source loads lib/debugger.trx, installs the
            // chosen variant of the inspector, and runs the user script.
            // Debugger-only: the --inspect flags that select InspectFile
            // mode are gated out of non-debugger builds (api.inl), so this
            // arm -- and the whole boot path -- compiles out in release.
            if (!push_inspect_boot()) {
                trx->diag_println("Trix --inspect could not start");
                trx->m_exit_code = +Error::InternalError;
            }
        }
#endif
        else if (mode == StartupMode::StdIn) {
            // Mode 2: read from stdin
            if (trx->m_stdin != nullptr) {
                push_exec_stream(trx->m_stdin, STDIN_SID, Object::ReadOnlyAccess);
            } else {
                trx->diag_println("Trix stdin is not available (disabled via --stream-io)");
                trx->m_exit_code = +Error::InvalidStreamAccess;
            }
        } else if (mode == StartupMode::Interactive) {
            // Mode 3: readline REPL with error-recovery barrier
            if (trx->m_stdedit != nullptr) {
                push_repl_stream(trx->m_stdedit, STDEDIT_SID);
            } else {
                trx->diag_println("Trix stdedit is not available (disabled via --stream-io)");
                trx->m_exit_code = +Error::InvalidStreamAccess;
            }
        } else if (mode == StartupMode::FileAndInteractive) {
            // Mode 4: run file, then drop to REPL
            // Push stdedit at bottom (runs after file), file on top (runs first)
            if (trx->m_stdedit != nullptr) {
                push_repl_stream(trx->m_stdedit, STDEDIT_SID);
                push_startup_file();
            } else {
                trx->diag_println("Trix stdedit is not available (disabled via --stream-io)");
                trx->m_exit_code = +Error::InvalidStreamAccess;
            }
        }
    }

    static void stop(Trix *trx) {
        for (auto s = trx->m_stream_inuse_list; s != nullptr; /* empty */) {
            auto next = s->next_stream(trx);
            s->close(trx);
            s = next;
        }

        // Flush stdio streams.  They do not live on m_stream_inuse_list and
        // close() refuses to operate on them, so their pending write buffers
        // would otherwise be dropped on shutdown -- a script that only calls
        // `print` without explicit `flush` would produce zero visible output.
        // flush() is a no-op when there's no pending data.
        if ((trx->m_stdout != nullptr) && ((trx->m_stdout->m_status & IsWritable) != 0)) {
            trx->m_stdout->flush(trx);
        }
        if ((trx->m_stderr != nullptr) && ((trx->m_stderr->m_status & IsWritable) != 0)) {
            trx->m_stderr->flush(trx);
        }

#ifndef TRIX_NO_READLINE
        if (trx->m_stdedit != nullptr) {
            // shutdown readline: restore terminal mode.  We do NOT call
            // rl_cleanup_after_signal here -- it is for signal-interrupted
            // readlines and emits cursor-right escape sequences
            // ("\x1b[C" once per prompt column) to position the cursor after
            // the prompt, which leaks those 6 invisible escapes after the
            // exit newline and indents the host shell's next prompt.
            ::rl_deprep_terminal();
        }
#endif
    }

    [[nodiscard]] static stream_id_t next_sid(Trix *trx) {
        auto sid = INVALID_SID;
        auto start = trx->m_next_sid;
        do {
            sid = trx->m_next_sid;

            if (++trx->m_next_sid < FIRST_SID) {
                trx->m_next_sid = FIRST_SID;
                trx->m_sid_wrapped = true;
            }

            if (trx->m_sid_wrapped) {
                for (auto x = trx->m_stream_inuse_list; x != nullptr; x = x->next_stream(trx)) {
                    if (sid == x->m_sid) {
                        // sid is associated with an inuse stream
                        sid = INVALID_SID;
                        break;
                    }
                }
            }

            if ((sid == INVALID_SID) && (trx->m_next_sid == start)) {
                trx->error(Error::LimitCheck, "stream: all stream IDs exhausted");
            }
        } while (sid == INVALID_SID);

        return sid;
    }

    [[nodiscard]] static bool available(Trix *trx) { return (trx->m_stream_free_list != nullptr); }

    [[nodiscard]] static Stream *alloc_stream(Trix *trx) {
        auto stream = trx->m_stream_free_list;
        if (stream != nullptr) {
            trx->m_stream_free_list = stream->next_stream(trx);
            stream->m_next_offset = trx->nullptr_to_offset(trx->m_stream_inuse_list);
            trx->m_stream_inuse_list = stream;
        }
        return stream;
    }

    void free_stream(Trix *trx) {
        auto stream = this;
        m_fd = -1;
        m_sid = INVALID_SID;
        m_status = IsClosed;

        if (trx->m_stream_inuse_list == stream) {
            trx->m_stream_inuse_list = next_stream(trx);
        } else {
            for (auto prev = trx->m_stream_inuse_list; prev != nullptr; prev = prev->next_stream(trx)) {
                if (prev->next_stream(trx) == stream) {
                    prev->m_next_offset = m_next_offset;
                    break;
                }
            }
        }
        m_next_offset = trx->nullptr_to_offset(trx->m_stream_free_list);
        trx->m_stream_free_list = stream;
    }

    static void restore(Trix *trx) {
        auto curr_save_level = trx->m_curr_save_level;
        for (auto stream = trx->m_stream_inuse_list; stream != nullptr; /* empty */) {
            auto next = stream->next_stream(trx);
            if (stream->m_stream_save_level > curr_save_level) {
                stream->close(trx);
            }
            stream = next;
        }
    }

    // 'r' open for read only, error if file does not exist
    // 'R' open for read/write, error if file does not exist
    // 'w' open for write only, create if file does not exist, truncate if file does exist
    // 'W' open for read/write, create if file does not exist, truncate if file does exist
    // 'a' open for write only, create if file does not exist, append if file does exist
    // 'A' open for read/write, create if file does not exist, append if file does exist
    // "e" open for write only, create if file does not exist, fail if file does exist
    // "E" open for read/write, create if file does not exist, fail if file does exist
    // Resolve a file open mode character (r/w/a/e/R/W/A/E) to stream status flags and POSIX open flags.
    struct FileMode {
        status_t status;
        int oflags;
        bool append;
    };

    [[nodiscard]] static FileMode resolve_file_mode(int mode) {
        switch (mode) {
        case 'r':
            return {IsReadable, (O_LARGEFILE | O_CLOEXEC | O_RDONLY), false};

        case 'w':
            return {IsWritable, (O_LARGEFILE | O_CLOEXEC | O_CREAT | O_WRONLY | O_TRUNC), false};

        case 'a':
            return {(IsWritable | IsAppend), (O_LARGEFILE | O_CLOEXEC | O_CREAT | O_WRONLY), true};

        case 'e':
            return {IsWritable, (O_LARGEFILE | O_CLOEXEC | O_CREAT | O_EXCL | O_WRONLY), false};

        case 'R':
            return {(IsReadable | IsWritable), (O_LARGEFILE | O_CLOEXEC | O_RDWR), false};

        case 'W':
            return {(IsReadable | IsWritable), (O_LARGEFILE | O_CLOEXEC | O_CREAT | O_RDWR | O_TRUNC), false};

        case 'A':
            return {(IsReadable | IsWritable | IsAppend), (O_LARGEFILE | O_CLOEXEC | O_CREAT | O_RDWR), true};

        case 'E':
            return {(IsReadable | IsWritable), (O_LARGEFILE | O_CLOEXEC | O_CREAT | O_EXCL | O_RDWR), false};

        default:
            assert(false && "resolve_file_mode: invalid mode");
            std::unreachable();
        }
    }

    [[nodiscard]] static std::pair<vm_offset_t, stream_id_t> open_file(Trix *trx, Object filename_obj, int mode) {
        Stream *stream = alloc_stream(trx);
        if (stream == nullptr) {
            trx->error(Error::LimitCheck, "cannot open file based stream, all are in use");
        } else {
            auto [mode_status, oflags, append] = resolve_file_mode(mode);
            const status_t status = (IsOpen | IsFile | SupportsRandomAccess | mode_status);

            // get nul terminated filename
            auto fn = filename_obj.string_cstr(trx);
            const ::mode_t create_mode = (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
            auto fd = ((oflags & O_CREAT) != 0) ? ::open64(fn, oflags, create_mode) : ::open64(fn, oflags);

            auto error = Error::NoError;
            int saved_errno = 0;
            if (fd == -1) {
                if (errno == ENOENT) {
                    error = Error::FilenameNotFound;
                } else if (errno == EEXIST) {
                    error = Error::FilenameExists;
                } else {
                    error = Error::FileOpenError;
                }
            } else {
                // Hint sequential readahead for readable files.  Non-binding; advisory.
                if ((mode_status & IsReadable) != 0) {
                    ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
                }
                if (append && (::lseek64(fd, 0, SEEK_END) == -1)) {
                    saved_errno = errno;
                    ::close(fd);
                    error = Error::IOSeekError;
                }
            }

            if (error == Error::NoError) {
                auto sid = next_sid(trx);
                auto save_level = trx->m_curr_save_level;
                stream->config_buffered(trx, filename_obj, fd, sid, save_level, status, trx->m_stream_buffer_size);
// Debug-only: record sid -> path so `stream-name` can resolve
// it after the stream is closed (e.g. for procs that ran via
// `require` and then closed their source stream).  Compiled
// out in release builds.
#ifdef TRIX_DEBUGGER
                trx->debug_cache_sid_path(sid, filename_obj);
#endif
                return std::pair{trx->ptr_to_offset(stream), sid};
            } else {
                stream->free_stream(trx);
                if (saved_errno == 0) {
                    saved_errno = errno;
                }
                errno = saved_errno;
                auto errno_str = errno_string();
                trx->error(error, "stream open failed with errno = {}/{}", saved_errno, errno_str);
            }
        }
    }

    struct OpenMemoryResult {
        bool success;
        vm_offset_t offset;
        stream_id_t sid;
    };

    // owns_data == true (default) means the Stream takes ownership of the
    // buffer and `std::free`s it on close (used by snapshot restore + invoke
    // IRQ paths that hand off a malloc'd buffer).  owns_data == false marks
    // the buffer as borrowed -- typically a VM-heap Trix string -- and the
    // Stream just nulls its pointers on close without freeing.
    [[nodiscard]] static OpenMemoryResult open_memory(Trix *trx, const vm_t *data, size_t length, bool owns_data = true) {
        auto stream = alloc_stream(trx);
        if (stream == nullptr) {
            return OpenMemoryResult{false, nulloffset, INVALID_SID};
        } else {
            auto sid = next_sid(trx);
            stream->config_memory(trx, data, length, sid, trx->m_curr_save_level, owns_data);
            return OpenMemoryResult{true, trx->ptr_to_offset(stream), sid};
        }
    }

    // Allocate a writable in-memory string-stream of the given byte capacity.
    // The buffer lives in the VM heap; m_fd stays -1.  Buffer-full at write
    // raises /io-write-error (see putc/putn).
    [[nodiscard]] static std::pair<vm_offset_t, stream_id_t> open_string_write(Trix *trx, vm_size_t capacity) {
        auto stream = alloc_stream(trx);
        if (stream == nullptr) {
            trx->error(Error::LimitCheck, "make-string-stream: all stream slots in use");
        } else {
            auto sid = next_sid(trx);
            stream->config_string_write(trx, capacity, sid, trx->m_curr_save_level);
            return std::pair{trx->ptr_to_offset(stream), sid};
        }
    }

    void close(Trix *trx) {
        if ((m_status & (IsOpen | IsStdIO)) == IsOpen) {
            if (m_fd != -1) {
                // Only flush if there is actual pending write data.  Checking IsWritable
                // alone would incorrectly trigger flush() on a stream currently in read
                // mode (ReadData set), where the write-pointer sentinel looks like pending
                // data but isn't.
                if ((m_status & WriteData) != 0) {
                    flush(trx);
                }
                if ((m_status & IsFile) != 0) {
                    ::close(m_fd);
                }
            }
            if (m_ext_base != nullptr) {
                free_ext_base();
                m_ext_remaining = 0;
            }
            if ((m_status & IsString) == 0) {
                free_stream(trx);
            }
        }
    }

    void set_filepos(Trix *trx, off64_t pos) {
        if (m_fd >= 0) {
            // Only flush if there is actual pending write data.  IsWritable alone means
            // the stream can write, not that bytes are buffered.  flush() on a stream in
            // read mode (ReadData set) treats the write-pointer sentinel as pending data
            // and errors with "read then write without set-file-position".
            if ((m_status & WriteData) != 0) {
                flush(trx);
            }
            reset();
            auto ret = ::lseek64(m_fd, pos, SEEK_SET);
            if (ret == -1) {
                auto saved_errno = errno;
                auto errno_str = errno_string();
                trx->error(
                        Error::IOSeekError, "'stream' lseek to position {} failed with errno = {}/{}", pos, saved_errno, errno_str);
            }
        }
    }

    [[nodiscard]] off64_t get_filepos(Trix *trx) const {
        off64_t pos = 0;
        if (m_fd >= 0) {
            pos = ::lseek64(m_fd, 0, SEEK_CUR);
            if (pos == -1) {
                auto saved_errno = errno;
                auto errno_str = errno_string();
                trx->error(
                        Error::IOSeekError, "'stream' lseek to current position failed with errno = {}/{}", saved_errno, errno_str);
            }
            if ((m_status & ReadData) != 0) {
                // OS position is ahead by read-ahead bytes not yet consumed
                pos -= static_cast<off64_t>(m_rlimit_offset - m_rptr_offset);
            } else if ((m_status & WriteData) != 0) {
                // OS position is behind by buffered write bytes not yet flushed
                pos += static_cast<off64_t>(m_wptr_offset - m_base_offset);
            }
        }
        return pos;
    }

    // Called by fill() only when m_ext_base == nullptr; pending input chunks
    // are handled directly by fill()'s ext_base block.
    [[nodiscard]] bool fill_stdedit(Trix *trx) {
#ifdef TRIX_NO_READLINE
        // No libreadline: emit a bare prompt and read one line from stdin via
        // getline (no editing, no history).  getline returns a malloc'd buffer
        // and KEEPS the trailing newline, so the m_ext_base handoff here (and
        // the std::free in free_ext_base) is reused exactly as for readline.
        // The terminal stays in cooked mode, so the OS line discipline still
        // gives basic backspace editing -- just no history or cursor motion.
        std::fputs("Trix> ", stdout);
        std::fflush(stdout);
        char *line = nullptr;
        size_t cap = 0;
        auto count = ::getline(&line, &cap, stdin);
        if (count < 0) {
            // EOF / ^D at the prompt.  readline never owned the tty, so a bare
            // newline lands the host shell's next prompt at column 0.
            std::free(line);
            std::fputs("\n", stdout);
            std::fflush(stdout);
            return false;
        } else {
            m_ext_base = reinterpret_cast<vm_t *>(line);
            m_ext_ptr = reinterpret_cast<vm_t *>(line);
            m_ext_remaining = static_cast<size_t>(count);
            copy_ext_chunk(trx);
            return true;
        }
#else
        auto line = ::readline("Trix> ");
        if (line == nullptr) {
            // EOF / ^D -- readline left the cursor on the prompt line without
            // emitting a newline.  Restore the terminal to cooked mode first
            // (ONLCR is off while readline owns the tty, so a bare '\n' moves
            // to the next line at the same column), then emit CRLF so the
            // host shell's next prompt lands at column 0.
            ::rl_deprep_terminal();
            std::fputs("\r\n", stdout);
            std::fflush(stdout);
            return false;
        } else {
            auto line_length = std::strlen(line);
            if (line_length > 0) {
                ::add_history(line);
                // readline strips the newline; overwrite the NUL terminator with LF
                // so the scanner sees a proper line ending for comment termination
                line[line_length] = ASCII_LF;
                m_ext_base = reinterpret_cast<vm_t *>(line);
                m_ext_ptr = reinterpret_cast<vm_t *>(line);
                m_ext_remaining = line_length + 1;

                // Copy first chunk; fill()'s ext_base block handles any subsequent chunks.
                copy_ext_chunk(trx);
            } else {
                // treat an empty line as \n; free the empty readline buffer
                std::free(line);
                m_buffer[0] = ASCII_LF;
                m_rlimit_offset = (m_base_offset + 1);
                m_rptr_offset = m_base_offset;
                m_status |= ReadData;
            }
            return true;
        }
#endif
    }

    [[nodiscard]] bool fill_stdin(Trix *trx) {
        auto buf = trx->offset_to_ptr<vm_t>(m_base_offset);
        auto count = ::read(m_fd, buf, m_buffer_size);
        if (count > 0) {
            // Honor in-band ^D/EOT as end-of-stream: truncate the buffer at the first
            // EOT.  Kernel canonical-mode stdin converts terminal ^D to read()==0, so
            // this branch fires only for raw ^D bytes in a piped stream.
            auto eot = static_cast<vm_t *>(std::memchr(buf, ASCII_EOT, static_cast<size_t>(count)));
            auto effective = (eot != nullptr) ? static_cast<ssize_t>(eot - buf) : count;
            if (effective > 0) {
                m_rlimit_offset = m_base_offset + static_cast<vm_offset_t>(effective);
                m_rptr_offset = m_base_offset;
                m_status |= ReadData;
                return true;
            } else {
                m_rlimit_offset = m_base_offset;
                m_rptr_offset = m_base_offset;
                return false;
            }
        } else if (count < 0) {
            // i/o error
            auto saved_errno = errno;
            auto errno_str = errno_string();
            trx->error(Error::IOReadError, "STDIN_FILENO read failed with errno {}/{}", saved_errno, errno_str);
        } else {
            // EOF (count == 0)
            m_rlimit_offset = m_base_offset;
            m_rptr_offset = m_base_offset;
            return false;
        }
    }

    // Release the external buffer reference (readline or memory stream source).
    // For caller-owned (IsBorrowed) buffers -- e.g. a Trix string already in the
    // VM heap, used by make-memory-stream -- we just null the pointers; std::free
    // would corrupt the VM heap.  For everything else (snapshot restore, invoke
    // IRQ, readline) the buffer was malloc'd specifically to be handed off here
    // and must be freed.
    void free_ext_base() {
        if ((m_status & IsBorrowed) == 0) {
            std::free(m_ext_base);
        }
        m_ext_base = nullptr;
        m_ext_ptr = nullptr;
    }

    // Fill the read buffer from a file descriptor.
    // Returns true if data was read, false on EOF.
    // Raises: SetFilePositionRequired if last op was a write; IOReadError on I/O failure.
    [[nodiscard]] bool fill_file(Trix *trx) {
        if ((m_status & WriteData) != 0) {
            trx->error(Error::SetFilePositionRequired, "stream: write then read without set-file-position");
        } else {
            auto count = ::read(m_fd, trx->offset_to_ptr<vm_t>(m_base_offset), m_buffer_size);
            if (count > 0) {
                m_rlimit_offset = m_base_offset + static_cast<vm_offset_t>(count);
                m_rptr_offset = m_base_offset;
                m_status |= ReadData;
                return true;
            } else if (count < 0) {
                auto saved_errno = errno;
                auto errno_str = errno_string();
                trx->error(Error::IOReadError, "stream read failed with errno {}/{}", saved_errno, errno_str);
            } else {
                if (!is_writable(m_sid)) {
                    close(trx);
                }
                return false;
            }
        }
    }

    // Copy the next chunk from the external buffer (m_ext_ptr) into the VM read buffer.
    // Frees the external buffer when fully consumed.
    void copy_ext_chunk(Trix *trx) {
        auto n = std::min(static_cast<size_t>(m_buffer_size), m_ext_remaining);
        std::memcpy(trx->offset_to_ptr<vm_t>(m_base_offset), m_ext_ptr, n);
        m_ext_ptr += n;
        m_ext_remaining -= n;
        m_rlimit_offset = (m_base_offset + static_cast<vm_offset_t>(n));
        m_rptr_offset = m_base_offset;
        m_status |= ReadData;
        if (m_ext_remaining == 0) {
            free_ext_base();
        }
    }

    [[nodiscard]] bool fill(Trix *trx) {
        if (m_ext_base != nullptr) {
            if (m_ext_remaining != 0) {
                copy_ext_chunk(trx);
                return true;
            } else {
                free_ext_base();
                return false;
            }
        } else if (m_fd == STDIN_FILENO) {
            return ((m_sid == STDEDIT_SID) ? fill_stdedit(trx) : fill_stdin(trx));
        } else if (m_fd >= 0) {
            return fill_file(trx);
        } else {
            return false;
        }
    }

    // Update line number and column position after consuming a character.
    void track_line_pos(int ch) {
        if (ch == ASCII_LF) {
            ++m_line_number;
            m_line_pos = 1;
        } else {
            ++m_line_pos;
        }
    }

    [[nodiscard]] int getc(Trix *trx) {
        if ((m_rptr_offset != m_rlimit_offset) || fill(trx)) {
            auto ch = trx->offset_to_value<vm_t>(m_rptr_offset);
            ++m_rptr_offset;
            track_line_pos(ch);
            return ch;
        } else {
            return EOFc;
        }
    }

    void getn(Trix *trx, vm_t *ptr, size_t count) {
        while (count != 0) {
            if ((m_rptr_offset == m_rlimit_offset) && !fill(trx)) {
                throw Exception::EndOfStream;
            } else {
                auto rptr = trx->offset_to_ptr<vm_t>(m_rptr_offset);
                auto rlimit = trx->offset_to_ptr<vm_t>(m_rlimit_offset);
                auto n = std::min(count, static_cast<size_t>(rlimit - rptr));
                std::copy_n(rptr, n, ptr);
                ptr += n;
                m_rptr_offset += static_cast<vm_offset_t>(n);
                count -= n;
            }
        }
    }

    template<typename T>
    requires std::is_trivially_copyable_v<T>
    T getn(Trix *trx) {
        T value{};
        getn(trx, reinterpret_cast<vm_t *>(&value), sizeof(T));
        return value;
    }

    [[nodiscard]] int8_t geti8(Trix *trx) {
        if ((m_rptr_offset == m_rlimit_offset) && !fill(trx)) {
            throw Exception::EndOfStream;
        } else {
            return static_cast<int8_t>(trx->offset_to_value<vm_t>(m_rptr_offset++));
        }
    }

    [[nodiscard]] uint8_t getu8(Trix *trx) {
        if ((m_rptr_offset == m_rlimit_offset) && !fill(trx)) {
            throw Exception::EndOfStream;
        } else {
            return static_cast<uint8_t>(trx->offset_to_value<vm_t>(m_rptr_offset++));
        }
    }

    // Read a multi-byte value from the stream and convert from the given endianness
    // to native byte order.  Uses byteswap_helper (ops_bitwise.inl) which handles
    // both integral types (via std::byteswap) and floating-point types (via bit_cast).
    template<typename T>
    requires(std::is_integral_v<T> || std::is_floating_point_v<T>)
    [[nodiscard]] T get_endian(Trix *trx, std::endian encoding) {
        auto value = getn<T>(trx);
        if constexpr (std::endian::native == std::endian::big) {
            return ((encoding == std::endian::little) ? byteswap_helper(value) : value);
        } else if constexpr (std::endian::native == std::endian::little) {
            return ((encoding == std::endian::big) ? byteswap_helper(value) : value);
        } else {
            std::unreachable();
        }
    }

    [[nodiscard]] int16_t geti16(Trix *trx, std::endian encoding) { return get_endian<int16_t>(trx, encoding); }

    [[nodiscard]] uint16_t getu16(Trix *trx, std::endian encoding) { return get_endian<uint16_t>(trx, encoding); }

    [[nodiscard]] int32_t geti32(Trix *trx, std::endian encoding) { return get_endian<integer_t>(trx, encoding); }

    [[nodiscard]] uint32_t getu32(Trix *trx, std::endian encoding) { return get_endian<uinteger_t>(trx, encoding); }

    [[nodiscard]] int64_t geti64(Trix *trx, std::endian encoding) { return get_endian<long_t>(trx, encoding); }

    [[nodiscard]] uint64_t getu64(Trix *trx, std::endian encoding) { return get_endian<ulong_t>(trx, encoding); }

    [[nodiscard]] real_t getreal(Trix *trx, std::endian encoding) { return get_endian<real_t>(trx, encoding); }

    [[nodiscard]] double_t getdouble(Trix *trx, std::endian encoding) { return get_endian<double_t>(trx, encoding); }

    [[nodiscard]] int peekc(Trix *trx) {
        return (((m_rptr_offset != m_rlimit_offset) || fill(trx)) ? trx->offset_to_value<vm_t>(m_rptr_offset) : EOFc);
    }

    // Push back one byte into the unget slot (m_unget, immediately before m_buffer).
    // Precondition: m_rptr_offset >= m_base_offset (one unget per fill cycle).
    // Used by the infix parser to peek past buffer boundaries without losing data.
    void ungetc(Trix *trx, vm_t ch) {
        assert(m_rptr_offset >= m_base_offset);

        --m_rptr_offset;
        *trx->offset_to_ptr<vm_t>(m_rptr_offset) = ch;
    }

    [[nodiscard]] bool matchc(Trix *trx, vm_t ch) {
        auto matched = ((m_rptr_offset != m_rlimit_offset) || fill(trx)) && (trx->offset_to_value<vm_t>(m_rptr_offset) == ch);
        if (matched) {
            track_line_pos(ch);
            ++m_rptr_offset;
        }
        return matched;
    }

    void matchlf(Trix *trx) {
        if (((m_rptr_offset != m_rlimit_offset) || fill(trx)) && (trx->offset_to_value<vm_t>(m_rptr_offset) == ASCII_LF)) {
            track_line_pos(ASCII_LF);
            ++m_rptr_offset;
        }
    }

    void consume(Trix *trx) {
        if (m_rptr_offset != m_rlimit_offset) {
            auto ch = trx->offset_to_value<vm_t>(m_rptr_offset++);
            track_line_pos(ch);
        } else {
            trx->error(Error::InternalError, "stream consume: read buffer is empty");
        }
    }

    // Bulk-advance the read pointer past N already-validated bytes from the
    // unread window returned by space().  No line tracking (matches getn);
    // callers parsing content with semantic newlines should use getc/consume
    // instead.
    void advance_read(length_t n) {
        assert(static_cast<vm_offset_t>(n) <= (m_rlimit_offset - m_rptr_offset));

        m_rptr_offset += static_cast<vm_offset_t>(n);
    }

    void flush(Trix *trx) {
        if (m_fd >= 0) {
            auto pending = static_cast<ssize_t>(m_wptr_offset - m_base_offset);
            if (pending != 0) {
                if ((m_status & ReadData) != 0) {
                    trx->error(Error::SetFilePositionRequired, "stream: read then write without set-file-position");
                } else if ((m_status & WriteData) == 0) {
                    // First flush after buffer conversion: the data was written
                    // by the application into what was a read buffer, so the
                    // content is already where it needs to be.  Subsequent
                    // flushes (WriteData set) perform the actual ::write() to
                    // the fd via the loop below.
                    m_wptr_offset = m_base_offset;
                    m_status |= WriteData;
                } else {
                    // Drain `pending` bytes to the fd, retrying on EINTR/EAGAIN.
                    // EAGAIN can occur when the fd is non-blocking (e.g. raw-mode
                    // sets STDIN to O_NONBLOCK, which on a typical interactive
                    // shell makes STDOUT non-blocking too via shared open file
                    // description).  poll() until the fd is writable, then retry.
                    // Short writes (rare on file fds, common on pipes/sockets)
                    // are stitched together by accumulating into `done`.
                    auto base_ptr = trx->offset_to_ptr<vm_t>(m_base_offset);
                    ssize_t done = 0;
                    while (done < pending) {
                        auto n = ::write(m_fd, base_ptr + done, static_cast<size_t>(pending - done));
                        if (n > 0) {
                            done += n;
                        } else if (n == 0) {
                            // No bytes accepted and no error -- treat as device full.
                            break;
                        } else if (errno == EINTR) {
                            // interrupted before any byte was written -- retry
                        } else if (errno == EAGAIN) {
                            // fd not yet writable -- block until it is, then retry
                            pollfd pfd{m_fd, POLLOUT, 0};
                            static_cast<void>(::poll(&pfd, 1, -1));
                        } else {
                            auto saved_errno = errno;
                            auto errno_str = errno_string();
                            trx->error(Error::IOWriteError, "stream write failed with errno {}/{}", saved_errno, errno_str);
                        }
                    }

                    if (done == pending) {
                        // full write succeeded
                        m_wptr_offset = m_base_offset;
                    } else {
                        // partial-write fall-through (n==0 device-full path).
                        if ((m_status & IsStdIO) == 0) {
                            // clear IsWritable and WriteData to prevent close() from calling flush() again
                            m_status &= ~(IsWritable | WriteData);
                            close(trx);
                        } else {
                            // stdio short write: discard buffered data to prevent buffer overrun on next put
                            m_wptr_offset = m_base_offset;
                        }
                    }
                }
            }
        }
    }

    void putc(Trix *trx, vm_t c) {
        if (m_wptr_offset == m_wlimit_offset) {
            if (is_string_write()) {
                trx->error(Error::IOWriteError, "string-stream: buffer full");
            } else {
                flush(trx);
            }
        }
        if ((m_status & IsWritable) != 0) {
            *trx->offset_to_ptr<vm_t>(m_wptr_offset++) = c;
        }
    }

    void putn(Trix *trx, const vm_t *ptr, size_t count) {
        while (count != 0) {
            if (m_wptr_offset == m_wlimit_offset) {
                if (is_string_write()) {
                    trx->error(Error::IOWriteError, "string-stream: buffer full");
                } else {
                    flush(trx);
                }
            }
            if ((m_status & IsWritable) == 0) {
                break;
            } else {
                const size_t space = static_cast<size_t>(m_wlimit_offset - m_wptr_offset);
                auto n = std::min(count, space);
                std::copy_n(ptr, n, trx->offset_to_ptr<vm_t>(m_wptr_offset));
                m_wptr_offset += static_cast<vm_offset_t>(n);
                ptr += n;
                count -= n;
            }
        }
    }

    [[nodiscard]] std::pair<vm_t *, stream_buffer_size_t> space(Trix *trx) const {
        return std::pair{trx->offset_to_ptr<vm_t>(m_rptr_offset),
                         static_cast<stream_buffer_size_t>(m_rlimit_offset - m_rptr_offset)};
    }

    [[nodiscard]] integer_t bytesavailable() const {
        return ((m_rptr_offset == m_rlimit_offset) ? -1 : static_cast<integer_t>(m_rlimit_offset - m_rptr_offset));
    }

    void reset() {
        m_rlimit_offset = m_base_offset;
        m_rptr_offset = m_base_offset;
        m_wptr_offset = m_wlimit_offset;
        m_status &= ~(ReadData | WriteData);
    }
private:
    friend class Trix;

    Stream() = default;

    Stream(Trix *trx, const char *data, length_t length, stream_id_t sid, save_level_t save_level) :
            m_base_offset{trx->nullptr_to_offset(data)},
            m_rlimit_offset{m_base_offset + static_cast<vm_offset_t>(length)},
            m_rptr_offset{m_base_offset},
            m_wlimit_offset{nulloffset},
            m_wptr_offset{nulloffset},
            m_next_offset{nulloffset},
            m_ext_base{nullptr},
            m_source{trx->root_object(RootObject::StringString)},
            m_fd{-1},
            m_buffer_size{0},
            m_status{IsOpen | IsReadable | IsString},
            m_line_pos{1},
            m_line_number{1},
            m_sid{sid},
            m_stream_save_level{save_level},
            m_infix_offset{nulloffset},
            m_infix_packed_size{0},
            m_infix_packed_read{0} {
        // empty
    }

    void config_common(Trix *trx,
                       Object source_obj,
                       int fd,
                       stream_id_t sid,
                       save_level_t save_level,
                       status_t status,
                       stream_buffer_size_t buffer_size) {
        static_cast<void>(trx);
        m_source = source_obj;
        m_fd = fd;
        m_sid = sid;
        m_stream_save_level = save_level;
        m_status = status;
        m_buffer_size = buffer_size;
        m_line_number = 1;
        m_line_pos = 1;
        m_ext_ptr = nullptr;
        m_ext_remaining = 0;
        m_infix_offset = nulloffset;
        m_infix_packed_size = 0;
        m_infix_packed_read = 0;
    }

    // Single entry point for buffered (fd-backed) streams.
    // Buffer pointer setup is derived from the IsReadable / IsWritable bits already
    // present in `status`, eliminating the need for three near-identical overloads.
    void config_buffered(Trix *trx,
                         Object source_obj,
                         int fd,
                         stream_id_t sid,
                         save_level_t save_level,
                         status_t status,
                         stream_buffer_size_t buffer_size) {
        config_common(trx, source_obj, fd, sid, save_level, status, buffer_size);

        m_ext_base = nullptr;                      // buffered streams use VM-heap buffer only
        auto base = trx->ptr_to_offset(m_buffer);  // offset of m_buffer[0] within VM heap
        m_base_offset = base;
        if ((status & IsReadable) != 0) {
            m_rlimit_offset = base;
            m_rptr_offset = base;
        } else {
            m_rlimit_offset = nulloffset;
            m_rptr_offset = nulloffset;
        }
        if ((status & IsWritable) != 0) {
            m_wlimit_offset = base + m_buffer_size;
            // Initialize write pointer at the limit (buffer appears full) so that the
            // first write call immediately flushes to the underlying fd, cleanly
            // entering the normal write path without a separate "not yet initialized"
            // state.
            m_wptr_offset = m_wlimit_offset;
        } else {
            m_wlimit_offset = nulloffset;
            m_wptr_offset = nulloffset;
        }
    }

    // After thaw: re-link the three stdio streams (already at correct VM heap offsets)
    // to their OS file descriptors and reset stale I/O buffer state.
    static void reattach_stdio(Trix *trx) {
        auto reattach_reader = [](Stream *s, int fd) {
            if (s != nullptr) {
                s->m_fd = fd;
                s->m_status &= ~static_cast<status_t>(ReadData | WriteData);
                s->m_rlimit_offset = s->m_base_offset;  // empty read buffer
                s->m_rptr_offset = s->m_base_offset;
            }
        };
        auto reattach_writer = [](Stream *s, int fd) {
            if (s != nullptr) {
                s->m_fd = fd;
                s->m_status &= ~static_cast<status_t>(ReadData | WriteData);
                s->m_wlimit_offset = s->m_base_offset + s->m_buffer_size;
                s->m_wptr_offset = s->m_wlimit_offset;
            }
        };
        reattach_reader(trx->m_stdin, STDIN_FILENO);
        reattach_reader(trx->m_stdedit, STDIN_FILENO);
        reattach_writer(trx->m_stdout, STDOUT_FILENO);
        reattach_writer(trx->m_stderr, STDERR_FILENO);
    }

    void
    config_memory(Trix *trx, const vm_t *data, size_t length, stream_id_t sid, save_level_t save_level, bool owns_data = true) {
        // Allocate a real VM-heap buffer so fill() can copy ext data into it.
        // All read methods then work through the normal VM-heap buffer path.
        auto status = static_cast<status_t>(IsOpen | IsReadable | IsMemory);
        if (!owns_data) {
            status = static_cast<status_t>(status | IsBorrowed);
        }
        config_common(trx, trx->root_object(RootObject::MemoryString), -1, sid, save_level, status, trx->m_stream_buffer_size);

        m_ext_base = const_cast<vm_t *>(data);  // freed by close() unless IsBorrowed
        m_ext_ptr = const_cast<vm_t *>(data);   // current read position
        m_ext_remaining = length;

        auto base = trx->ptr_to_offset(m_buffer);
        m_base_offset = base;
        m_rlimit_offset = base;  // empty buffer -> first read triggers fill()
        m_rptr_offset = base;
        m_wlimit_offset = nulloffset;
        m_wptr_offset = nulloffset;
    }

    // Configure a writable in-memory string-stream.  Allocates a separate
    // VM-heap byte buffer of the given capacity (independent of the embedded
    // m_buffer, which is unused here).  Reads are not supported; writes
    // accumulate until the buffer is full, at which point putc/putn raise
    // /io-write-error.  get-string-stream returns a fresh Trix string copy
    // of bytes [m_base_offset, m_wptr_offset).
    void config_string_write(Trix *trx, vm_size_t capacity, stream_id_t sid, save_level_t save_level) {
        config_common(
                trx, trx->root_object(RootObject::StringString), -1, sid, save_level, (IsOpen | IsWritable | IsStringWrite), 0);

        auto [buf_ptr, buf_offset] = trx->vm_alloc_n<vm_t>(capacity);
        static_cast<void>(buf_ptr);
        m_ext_base = nullptr;
        m_ext_ptr = nullptr;
        m_ext_remaining = 0;
        m_base_offset = buf_offset;
        m_rlimit_offset = nulloffset;
        m_rptr_offset = nulloffset;
        m_wlimit_offset = buf_offset + static_cast<vm_offset_t>(capacity);
        m_wptr_offset = buf_offset;
    }

    // Resolve m_next_offset to the next Stream in the linked list.
    Stream *next_stream(Trix *trx) const {
        return ((m_next_offset == nulloffset) ? nullptr : trx->offset_to_ptr<Stream>(m_next_offset));
    }

    ~Stream() = default;

    // m_buffer[9], together with the immediately-preceding m_unget byte, begins a
    // variable-size buffer whose actual capacity is m_buffer_size.  Stream objects are
    // allocated in VM memory as sizeof(Stream) + (stream_buffer_size - sizeof(m_buffer))
    // bytes, so the full buffer extends into the extra allocated space.  The inline bytes
    // are not semantically special; they give the struct a concrete fixed-size member that
    // the static_assert below can validate against.

    vm_offset_t m_base_offset;           // beginning of buffer (nulloffset = external/closed)
    vm_offset_t m_rlimit_offset;         // end of read buffer
    vm_offset_t m_rptr_offset;           // next data to read
    vm_offset_t m_wlimit_offset;         // end of write buffer
    vm_offset_t m_wptr_offset;           // next data to write
    vm_offset_t m_next_offset;           // next stream in list (nulloffset = end of list)
    vm_t *m_ext_base;                    // external malloc (IsMemory: script data; StdEdit: readline data, Else: nullptr);
    vm_t *m_ext_ptr;                     // current read position in m_ext_base; null otherwise
    size_t m_ext_remaining;              // bytes remaining in m_ext_base; 0 otherwise
    Object m_source;                     // filename or source description
    int m_fd;                            // open() file descriptor
    stream_buffer_size_t m_buffer_size;  // buffer size
    status_t m_status;                   // status flags
    uint16_t m_line_pos;                 // char position within current line
    uint32_t m_line_number;              // line number based on scanned LF
    stream_id_t m_sid;                   // Stream to Object association
    save_level_t m_stream_save_level;    // save level

    // Infix expression scanner: packed postfix tokens from $( ) / $[ ] parsing.
    // scan_infix_expression() parses the full expression, packs the postfix output
    // into a compact byte stream in VM memory.  scanner() extracts them one at a time.
    vm_offset_t m_infix_offset;    // VM offset to packed byte buffer (nulloffset = inactive)
    length_t m_infix_packed_size;  // total packed bytes written
    length_t m_infix_packed_read;  // byte offset of next token to extract

    vm_t m_unget;      // 1-byte unget slot (immediately before m_buffer in memory)
    vm_t m_buffer[9];  // first bytes of buffer (contiguous with m_unget)
};
// Trix is 64-bit only (see the __int128 gate in types.inl); under an 8-byte
// pointer the vm_offset_t/vm_t* members pack Stream to exactly 96 bytes.
static_assert((sizeof(void *) == 8) && (sizeof(Stream) == 96), "Stream must be 96 bytes on a 64-bit target");

// Thread-safe view of the current errno's textual description.  Returns a
// view into a thread_local stack buffer; the view is valid until the next
// errno_string() call on the same thread.  Single-call-then-format usage
// at all current sites makes this safe in practice.
//
// Uses GNU strerror_r (returns char* that may or may not equal buf) so we
// route the result through the returned pointer.  No heap allocation.
[[nodiscard]] static std::string_view errno_string() {
    static thread_local char buf[256];
    // strerror_r may itself set errno (EINVAL for an unknown code, ERANGE for
    // a short buffer), which would corrupt the value a caller prints right
    // after this returns.  Save and restore it so a "{}/{}" message with errno
    // and this string always refer to the same error.
    auto saved_errno = errno;
    auto *msg = ::strerror_r(errno, buf, sizeof(buf));
    errno = saved_errno;
    return std::string_view{msg};
}
