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

//===--- Stream I/O Operators ---===//
//
// Implements file and stream I/O operations: open, close, read, write,
// seek, flush, and the with-stream scoping pattern.  Based on:
//
//   PostScript file I/O: file (stream), closefile (close-stream),
//   read, write, readstring (read-string), writestring (write-string),
//   bytesavailable (bytes-available), flush (flush-stream),
//   setfileposition (set-stream-position), fileposition (stream-position),
//   status, and the currentfile/run pattern.  Trix names in parentheses.
//
//   C stdio: fopen/fclose/fread/fwrite/fseek/ftell model, but using
//   Trix's Stream abstraction (stream.inl) rather than FILE*.
//
// --- Core concepts for maintainers ---
//
// FILE OPERATIONS
//   stream       -- open a file by name with mode (r/w/a/e/x, uppercase R/W/A/E for read+write)
//   close-stream -- close a stream (flushes buffer, releases fd)
//   with-stream  -- RAII pattern: open, execute proc, auto-close
//
// READ OPERATIONS
//   read              -- read one byte
//   read-string       -- read up to N bytes into a string
//   read-line         -- read until newline or EOF
//   bytes-available   -- bytes available without blocking
//
// WRITE OPERATIONS
//   write             -- write one byte
//   write-string      -- write a string
//   flush-stream      -- flush buffered output
//
// SEEK OPERATIONS
//   set-stream-position -- seek to absolute position
//   stream-position     -- query current position
//
// WITH-STREAM PATTERN
//   `filename mode { ... } with-stream` opens a file, pushes the
//   stream on the operand stack, executes the proc, and guarantees
//   the stream is closed on both normal and exceptional exit.  This
//   uses exec-stack barriers similar to try-catch/finally.
//

// bytesavailable: stream :- int
// Returns the number of bytes available to read.
// throws: invalid-stream-access, opstack-underflow, type-check
static void bytesavailable_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto [stream, sid] = trx->m_op_ptr->stream_value(trx);
    if (stream->is_readable(sid)) {
        *trx->m_op_ptr = Object::make_integer(stream->bytesavailable());
    } else {
        trx->error(Error::InvalidStreamAccess, "bytesavailable: stream is not readable");
    }
}

// closestream: stream :- --
// Closes the stream.  Idempotent: calling close-stream on a stream
// that is already closed (e.g., because read-all drained it and
// auto-closed on EOF) is a no-op, not an error.  This lets the
// POSIX-familiar "open / read / close" idiom work uniformly in
// Trix regardless of whether intermediate operators have already
// released the underlying fd.
// throws: opstack-underflow, type-check
static void closestream_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto [stream, sid] = trx->m_op_ptr->stream_value(trx);
    if (stream->is_open(sid)) {
        stream->close(trx);
    }
    --trx->m_op_ptr;
}

// currentstream: :- stream
// Pushes the current input stream from the exec stack.
// throws: opstack-overflow
static void currentstream_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto stream_obj = Object::make_invalid_stream();
    for (auto object = trx->m_exec_ptr; (object >= trx->m_exec_base); --object) {
        if (object->is_executable()) {
            if (object->is_stream()) {
                auto [stream, sid] = object->stream_value(trx);
                if (stream->is_readable(sid)) {
                    stream_obj = *object;
                    break;
                }
            } else if (object->is_string()) {
                stream_obj = *object;
                break;
            }
        }
    }
    stream_obj.set_literal();
    *++trx->m_op_ptr = stream_obj;
}

// raw-mode signal/atexit handler.
//
// Process-global state: sigaction() is process-wide, so the first Trix
// instance to call raw-mode wins ownership of the SIGINT/SIGTERM/SIGHUP/
// SIGQUIT dispositions.  The saved termios snapshot lives here (not on the
// instance) so the handler can restore it without a Trix * argument.
//
// The handler restores cooked mode synchronously, emits defensive terminal
// cleanup (show cursor + leave alt-screen + reset SGR), restores the
// previous signal disposition, and re-raises the signal so default
// behavior wins (process termination, core dump, etc.).  The cleanup
// emits are gated on sm_atexit_termios_valid (only when raw-mode was
// actually entered) and use write(2) which is async-signal-safe.  If the
// caller never entered alt-screen or hid the cursor, the sequences are
// visually harmless no-ops.  atexit hook is belt-and-suspenders for paths
// that bypass the signal handler (assert(), abort(), exit()).
//
// Without this cleanup, ^C from a TUI like tetrix leaves the terminal
// stuck in alt-screen (no scrollback) with the cursor hidden -- and
// `stty sane` doesn't help because that mode lives in the terminal
// emulator, not the kernel tty driver.
static void raw_mode_emit_terminal_cleanup() {
    static constexpr const char cleanup[] = "\033[?25h\033[?1049l\033[0m";
    // Async-signal-safe write -- we are mid-shutdown (signal handler or
    // atexit) and have no meaningful recovery for a short write or EBADF.
    // _FORTIFY_SOURCE=3 (prod build) ignores static_cast<void>() on the
    // warn_unused_result attribute, so capture the result and mark it
    // [[maybe_unused]] to document intent and silence the warning.
    [[maybe_unused]] const ssize_t n = ::write(STDOUT_FILENO, cleanup, sizeof(cleanup) - 1);
}

static void raw_mode_signal_handler(int sig) {
    if (sm_atexit_termios_valid != 0) {
        static_cast<void>(::tcsetattr(sm_atexit_termios_fd, TCSANOW, &sm_atexit_termios));
        raw_mode_emit_terminal_cleanup();
        sm_atexit_termios_valid = 0;
    }
    struct sigaction *prev = nullptr;
    switch (sig) {
    case SIGINT:
        prev = &sm_prev_sigint;
        break;

    case SIGTERM:
        prev = &sm_prev_sigterm;
        break;

    case SIGHUP:
        prev = &sm_prev_sighup;
        break;

    case SIGQUIT:
        prev = &sm_prev_sigquit;
        break;

    default:
        break;
    }
    if (prev != nullptr) {
        static_cast<void>(::sigaction(sig, prev, nullptr));
    }
    ::raise(sig);
}

static void raw_mode_atexit_handler() {
    if (sm_atexit_termios_valid != 0) {
        static_cast<void>(::tcsetattr(sm_atexit_termios_fd, TCSANOW, &sm_atexit_termios));
        raw_mode_emit_terminal_cleanup();
        sm_atexit_termios_valid = 0;
    }
}

// raw-mode: -- :- --
// Switches the controlling tty to raw mode: ICANON and ECHO disabled, VMIN=1
// VTIME=0 (read returns as soon as one byte is available).  Idempotent: a
// second call while already in raw mode is a no-op.  Lazy-installs a
// SIGINT/SIGTERM/SIGHUP/SIGQUIT handler chained to any previous disposition,
// plus an atexit hook, so a crash leaves the user's tty in cooked mode.
// throws: io-read-error (stdin is not a tty), unsupported (sandbox)
static void rawmode_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "raw-mode: disabled in sandbox mode");
    } else if (trx->m_raw_mode) {
        return;
    } else if (::tcgetattr(STDIN_FILENO, &trx->m_saved_termios) != 0) {
        trx->error(Error::IOReadError, "raw-mode: stdin is not a tty");
    } else {
        auto raw = trx->m_saved_termios;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            trx->error(Error::IOReadError, "raw-mode: tcsetattr failed");
        } else {
            // Switch stdin to non-blocking so read-key-byte can probe without stalling
            // the scheduler.  Save the original flags so cooked-mode restores them.
            trx->m_stdin_orig_flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
            if (trx->m_stdin_orig_flags >= 0) {
                static_cast<void>(::fcntl(STDIN_FILENO, F_SETFL, trx->m_stdin_orig_flags | O_NONBLOCK));
            }

            sm_atexit_termios = trx->m_saved_termios;
            sm_atexit_termios_fd = STDIN_FILENO;
            sm_atexit_termios_valid = 1;

            if (!trx->m_raw_mode_signal_handler_installed) {
                struct sigaction sa{};
                sa.sa_handler = &raw_mode_signal_handler;
                ::sigemptyset(&sa.sa_mask);
                sa.sa_flags = 0;
                static_cast<void>(::sigaction(SIGINT, &sa, &sm_prev_sigint));
                static_cast<void>(::sigaction(SIGTERM, &sa, &sm_prev_sigterm));
                static_cast<void>(::sigaction(SIGHUP, &sa, &sm_prev_sighup));
                static_cast<void>(::sigaction(SIGQUIT, &sa, &sm_prev_sigquit));
                static_cast<void>(::atexit(&raw_mode_atexit_handler));
                trx->m_raw_mode_signal_handler_installed = true;
            }

            trx->m_raw_mode = true;
        }
    }
}

// cooked-mode: -- :- --
// Restores cooked (canonical) tty mode by writing back the termios snapshot
// taken at raw-mode entry.  Idempotent: no-op if not currently raw.  Leaves
// the signal handler installed (one-time per-process cost); the atexit
// invariant flag is cleared so subsequent normal exit doesn't double-restore.
// throws: unsupported (sandbox)
static void cookedmode_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "cooked-mode: disabled in sandbox mode");
    } else if (trx->m_raw_mode) {
        static_cast<void>(::tcsetattr(STDIN_FILENO, TCSANOW, &trx->m_saved_termios));
        if (trx->m_stdin_orig_flags >= 0) {
            static_cast<void>(::fcntl(STDIN_FILENO, F_SETFL, trx->m_stdin_orig_flags));
        }
        sm_atexit_termios_valid = 0;
        trx->m_raw_mode = false;
    }
}

// raw-mode?: -- :- bool
// Pushes true if currently in raw mode, false otherwise.  Allowed in sandbox
// mode (read-only state inspection is harmless).
// throws: opstack-overflow
static void israwmode_op(Trix *trx) {
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_boolean(trx->m_raw_mode);
}

// terminal-size: -- :- cols rows
// Pushes the controlling terminal's column and row count.  Implemented via
// ioctl(STDIN_FILENO, TIOCGWINSZ, ...).  Used by TUI programs to lay out
// their canvas; call once per frame (there is no SIGWINCH listener, so
// resizes are observed lazily).
// throws: io-read-error (stdin not a tty), opstack-overflow, unsupported (sandbox)
static void terminalsize_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "terminal-size: disabled in sandbox mode");
    } else {
        trx->require_op_capacity(2);
        winsize ws{};
        if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0) {
            trx->error(Error::IOReadError, "terminal-size: stdin is not a tty");
        } else {
            *++trx->m_op_ptr = Object::make_integer(ws.ws_col);
            *++trx->m_op_ptr = Object::make_integer(ws.ws_row);
        }
    }
}

// key-ready?: -- :- bool
// Non-blocking poll on stdin: pushes true if at least one byte is available
// for read-key-byte to consume immediately, false otherwise.  No coroutine
// trampolining; pure poll(2) with timeout 0.  Independent of raw mode --
// useful for "drain pending input" patterns.
// throws: opstack-overflow, unsupported (sandbox)
static void keyready_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "key-ready?: disabled in sandbox mode");
    } else {
        trx->require_op_capacity(1);
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        auto rc = ::poll(&pfd, 1, 0);
        auto ready = (rc > 0) && ((pfd.revents & (POLLIN | POLLHUP)) != 0);
        *++trx->m_op_ptr = Object::make_boolean(ready);
    }
}

// Outcome of a single non-blocking stdin read (read_key_byte_try).
enum class ReadKeyResult {
    Byte,        // a byte was read (out_byte set)
    Eof,         // clean close (read returned 0)
    WouldBlock,  // EAGAIN/EWOULDBLOCK -- no data ready
    Error,       // other read failure (errno set)
};

// Helper: try a single non-blocking read from stdin.
static ReadKeyResult read_key_byte_try(uint8_t *out_byte) {
    auto rc = ssize_t{0};
    do {
        // Retry on EINTR like every other raw-syscall site (raw mode installs signal
        // handlers without SA_RESTART, so a delivered signal would otherwise surface as
        // a hard /io-read-error instead of a transparent retry).
        rc = ::read(STDIN_FILENO, out_byte, 1);
    } while ((rc < 0) && (errno == EINTR));
    if (rc == 1) {
        return ReadKeyResult::Byte;
    } else if (rc == 0) {
        return ReadKeyResult::Eof;
    } else if (errno == EAGAIN) {
        // POSIX 2008+ guarantees EAGAIN == EWOULDBLOCK on Linux/glibc; older
        // platforms make them distinct.  Comparing to one covers both on Linux
        // (and -Wlogical-op flags the OR-of-equals).  If we ever port to a
        // libc where they diverge, switch to the OR with -Wno-logical-op-local.
        return ReadKeyResult::WouldBlock;
    } else {
        return ReadKeyResult::Error;
    }
}

// Helper: push the common byte-or-EOF outcome of read_key_byte_try onto the
// operand stack (Byte -> `byte true`, Eof -> `false`).  Returns true when it
// handled the result; false for WouldBlock/Error, which each caller resolves
// itself (block, return false, or raise).  Caller must require_op_capacity(2).
[[nodiscard]] static bool push_read_key_result(Trix *trx, ReadKeyResult result, uint8_t byte) {
    if (result == ReadKeyResult::Byte) {
        *++trx->m_op_ptr = Object::make_byte(byte);
        *++trx->m_op_ptr = Object::make_boolean(true);
        return true;
    } else if (result == ReadKeyResult::Eof) {
        *++trx->m_op_ptr = Object::make_boolean(false);
        return true;
    } else {
        return false;
    }
}

// Helper: register the running coroutine as the single stdin reader, push the
// given continuation control op on the exec stack, and yield.  Wakeup happens
// from the scheduler's idle-fallback poll() when stdin becomes readable.
void coroutine_block_on_stdin(SystemName continuation_op, uint64_t wake_time) {
    if (m_stdin_blocked_reader != nulloffset) {
        error(Error::InvalidAccess, "read-key-byte: another coroutine is already reading stdin");
    } else {
        require_exec_capacity(1);
        *++m_exec_ptr = Object::make_control_operator(continuation_op);
        m_stdin_blocked_reader = m_running_coroutine;
        coroutine_flush_running();
        auto ctx = offset_to_ptr<CoroutineContext>(m_running_coroutine);
        ctx->m_status = CoroutineContext::Sleeping;
        ctx->m_wake_time_ns = wake_time;
        if (wake_time == std::numeric_limits<uint64_t>::max()) {
            ctx->m_flags |= CoroutineContext::FlagBlocked;
        } else {
            timer_list_insert(ctx);
        }
        coroutine_schedule();
    }
}

// read-key-byte: -- :- byte true | false
// Read one byte from stdin.  If no byte is available, the running coroutine
// blocks (yields to the scheduler) until stdin becomes readable.  Returns
// `byte true` on success or `false` on EOF (clean close).  Single-reader
// discipline: only one coroutine may be in read-key-byte at a time.
// throws: invalid-access (concurrent reader), io-read-error (non-EAGAIN read
// failure), opstack-overflow, unsupported (sandbox)
static void readkeybyte_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "read-key-byte: disabled in sandbox mode");
    } else {
        trx->require_op_capacity(2);

        uint8_t byte = 0;
        auto rc = read_key_byte_try(&byte);
        if (!push_read_key_result(trx, rc, byte)) {
            if (rc == ReadKeyResult::WouldBlock) {
                // EAGAIN: block until stdin is readable.
                trx->coroutine_block_on_stdin(SystemName::atReadKeyRetry, std::numeric_limits<uint64_t>::max());
            } else {
                trx->error(Error::IOReadError, "read-key-byte: read failed (errno {})", errno);
            }
        }
    }
}

// @read-key-retry: continuation for read-key-byte after a poll-driven wake.
// The coroutine is resumed with no exec-stack companion; just retry the read.
// On EAGAIN (rare, since the poll() said data was ready), re-sleep.
static void at_read_key_retry_op(Trix *trx) {
    trx->require_op_capacity(2);

    uint8_t byte = 0;
    auto rc = read_key_byte_try(&byte);
    if (!push_read_key_result(trx, rc, byte)) {
        if (rc == ReadKeyResult::WouldBlock) {
            trx->coroutine_block_on_stdin(SystemName::atReadKeyRetry, std::numeric_limits<uint64_t>::max());
        } else {
            trx->error(Error::IOReadError, "read-key-byte: read failed (errno {})", errno);
        }
    }
}

// read-key-byte-timeout: ms -- :- byte true | false
// Same as read-key-byte but waits at most `ms` milliseconds for input.  On
// timeout returns just `false` (no byte).  ms < 0 is treated as infinite
// (equivalent to read-key-byte).  ms == 0 polls without blocking.
// throws: invalid-access (concurrent reader), io-read-error, opstack-overflow,
// type-check, unsupported (sandbox)
static void readkeybyte_timeout_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "read-key-byte-timeout: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyIntegers);
        auto [ms_valid, ms] =
                trx->m_op_ptr->integer_value(trx, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
        if (!ms_valid) {
            trx->error(Error::TypeCheck, "read-key-byte-timeout: ms out of int32 range");
        } else {
            --trx->m_op_ptr;
            trx->require_op_capacity(2);

            // ms == 0: pure poll, no trampoline
            if (ms == 0) {
                uint8_t byte = 0;
                auto rc = read_key_byte_try(&byte);
                if (!push_read_key_result(trx, rc, byte)) {
                    if (rc == ReadKeyResult::WouldBlock) {
                        // No data ready and no time to wait: report no key.
                        *++trx->m_op_ptr = Object::make_boolean(false);
                    } else {
                        trx->error(Error::IOReadError, "read-key-byte-timeout: read failed (errno {})", errno);
                    }
                }
            } else {
                // First attempt: non-blocking probe before committing to a sleep.
                uint8_t byte = 0;
                auto rc = read_key_byte_try(&byte);
                if (!push_read_key_result(trx, rc, byte)) {
                    if (rc == ReadKeyResult::Error) {
                        trx->error(Error::IOReadError, "read-key-byte-timeout: read failed (errno {})", errno);
                    } else {
                        // WouldBlock: sleep with timer.  ms < 0 == infinite (equivalent to read-key-byte).
                        // Non-negative timeouts are budgeted (see sleep_budget_grant); the infinite
                        // form is an event park on stdin, not a wall-clock sleep, and stays exempt.
                        auto wake =
                                (ms < 0)
                                        ? std::numeric_limits<uint64_t>::max()
                                        : (trx->monotonic_ns() + trx->sleep_budget_grant(static_cast<uint64_t>(ms)) * 1'000'000ULL);
                        trx->coroutine_block_on_stdin((ms < 0) ? SystemName::atReadKeyRetry : SystemName::atReadKeyTimeoutRetry,
                                                      wake);
                    }
                }
            }
        }
    }
}

// @read-key-timeout-retry: continuation for read-key-byte-timeout after a
// wake (either poll-driven or timer-driven).  On wake, retry the read once.
// EAGAIN means the timer fired with no input: return `false`.
static void at_read_key_timeout_retry_op(Trix *trx) {
    trx->require_op_capacity(2);

    uint8_t byte = 0;
    auto rc = read_key_byte_try(&byte);
    if (!push_read_key_result(trx, rc, byte)) {
        if (rc == ReadKeyResult::WouldBlock) {
            // Timer expired with no data.
            *++trx->m_op_ptr = Object::make_boolean(false);
        } else {
            trx->error(Error::IOReadError, "read-key-byte-timeout: read failed (errno {})", errno);
        }
    }
}

// delete-file: str :- --
// Deletes the file named by str.
// throws: filename-not-found, opstack-underflow, type-check, unsupported
static void deletefile_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "delete-file: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        auto cstr = trx->m_op_ptr->string_cstr(trx);
        if (std::remove(cstr) != 0) {
            trx->error(Error::FilenameNotFound, "delete-file: cannot delete '{}'", cstr);
        } else {
            --trx->m_op_ptr;
        }
    }
}

// stream: str int :- stream
// Opens a file stream with the given access mode.
// throws: execstack-overflow, file-open-error, filename-exists, filename-not-found, invalid-stream-access,
// io-seek-error, limit-check, opstack-underflow, type-check
static void stream_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "stream: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyByte, VerifyString);

        auto access_ptr = trx->m_op_ptr;
        auto ch = access_ptr->byte_value();
        auto lower_ch = Trix::to_lowercase(ch);
        if ((lower_ch == 'r') || (lower_ch == 'w') || (lower_ch == 'a') || (lower_ch == 'e') || (lower_ch == 'x')) {
            auto filename_ptr = (access_ptr - 1);

            if (lower_ch == 'x') {
                trx->require_exec_capacity(1);

                auto [stream_offset, sid] = Stream::open_file(trx, *filename_ptr, 'r');
                *++trx->m_exec_ptr = Object::make_stream(stream_offset, sid, Object::ExecutableAttrib, Object::ReadOnlyAccess);
                trx->m_op_ptr -= 2;
            } else {
                auto [stream_offset, sid] = Stream::open_file(trx, *filename_ptr, ch);
                auto access = (lower_ch == 'r') ? Object::ReadOnlyAccess : Object::ReadWriteAccess;
                *--trx->m_op_ptr = Object::make_stream(stream_offset, sid, Object::LiteralAttrib, access);
            }
        } else {
            trx->error(Error::InvalidStreamAccess, "stream: invalid access mode '{:c}'", ch);
        }
    }
}

// make-string-stream: int :- stream
// Allocates a writable in-memory string-stream of the given byte capacity.
// Writes accumulate into a VM-heap byte buffer; get-string-stream returns a
// fresh Trix string copy of bytes written so far.  Hitting the buffer limit
// raises /io-write-error.  No host effect, so allowed under --sandbox.
// throws: limit-check, opstack-underflow, range-check, type-check, vm-full
static void make_string_stream_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers64 | VerifyNotNegative);
    auto cap_ptr = trx->m_op_ptr;
    auto [valid, capacity] = cap_ptr->uinteger_value(trx);
    if (!valid || (capacity == 0)) {
        trx->error(Error::RangeCheck, "make-string-stream: capacity must be positive and fit in uinteger");
    } else if (capacity > MaxStringLength) {
        trx->error(Error::LimitCheck, "make-string-stream: capacity {} exceeds maximum {}", capacity, MaxStringLength);
    } else {
        cap_ptr->maybe_free_extvalue(trx);
        auto [stream_offset, sid] = Stream::open_string_write(trx, static_cast<vm_size_t>(capacity));
        *cap_ptr = Object::make_stream(stream_offset, sid, Object::LiteralAttrib, Object::ReadWriteAccess);
    }
}

// get-string-stream: stream :- str
// Returns a fresh Trix string holding the bytes written to a string-stream
// (created by make-string-stream) so far.  Independent of open/closed state:
// callers may peek mid-stream, then continue writing, or close-and-extract.
// Raises /invalid-stream-access if the stream is not a string-stream.
// throws: invalid-stream-access, opstack-underflow, type-check, vm-full
static void get_string_stream_op(Trix *trx) {
    trx->verify_operands(VerifyStream);
    auto stream_ptr = trx->m_op_ptr;
    auto [stream, sid] = stream_ptr->stream_value(trx);
    static_cast<void>(sid);
    if (!stream->is_string_write()) {
        trx->error(Error::InvalidStreamAccess, "get-string-stream: not a string-stream");
    } else {
        auto length = static_cast<length_t>(stream->m_wptr_offset - stream->m_base_offset);
        auto src_ptr = trx->offset_to_ptr<vm_t>(stream->m_base_offset);
        *stream_ptr = Object::make_string(trx, src_ptr, length);
    }
}

// clear-string-stream: stream :- --
// Resets the write pointer of a writable in-memory string-stream so
// subsequent writes start at offset 0.  Buffer is reused; any string
// previously returned by get-string-stream remains valid (it's a fresh
// copy).  Raises /invalid-stream-access for non-string-streams (file or
// fd-backed) -- those have no rewind semantics here.
// throws: invalid-stream-access, opstack-underflow, type-check
static void clear_string_stream_op(Trix *trx) {
    trx->verify_operands(VerifyStream);
    auto stream_ptr = trx->m_op_ptr;
    auto [stream, sid] = stream_ptr->stream_value(trx);
    static_cast<void>(sid);
    if (!stream->is_string_write()) {
        trx->error(Error::InvalidStreamAccess, "clear-string-stream: not a string-stream");
    } else {
        stream->m_wptr_offset = stream->m_base_offset;
        --trx->m_op_ptr;
    }
}

// set-stdout: stream :- stream
// Swaps trx->m_stdout to the given writable stream and pushes the
// previous stdout back as a Stream Object.  Used by the debugger to
// route user-script output into a capture buffer (a writable string-
// stream produced by make-string-stream) and to restore real stdout
// at callback entry.  Pending bytes in the previous stdout are NOT
// flushed -- callers wanting to drain must flush themselves before
// the swap.  Set-stdout does not update systemdict's `/stdout`
// binding; user code reading `/stdout` still sees the original
// stream object.
// throws: invalid-stream-access, opstack-underflow, type-check
static void set_stdout_op(Trix *trx) {
    trx->verify_operands(VerifyRWStream);
    auto stream_ptr = trx->m_op_ptr;
    auto [stream, sid] = stream_ptr->stream_value(trx);
    if (!stream->is_writable(sid)) {
        trx->error(Error::InvalidStreamAccess, "set-stdout: stream is not writable");
    } else {
        auto prev = trx->m_stdout;
        trx->m_stdout = stream;
        if (prev != nullptr) {
            *stream_ptr = Object::make_stream(trx, prev);
        } else {
            *stream_ptr = Object::make_invalid_stream();
        }
    }
}

// filename-for-all: str proc :- --
// Executes proc for each file matching the glob pattern.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void filenameforall_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyString);

    auto proc = trx->m_op_ptr;
    auto pattern_ptr = (proc - 1);
    auto pattern_cstr = pattern_ptr->string_cstr(trx);

    glob_t g;
    auto ret = ::glob(pattern_cstr, GLOB_NOSORT, nullptr, &g);

    if ((ret != 0) || (g.gl_pathc == 0)) {
        if (ret == 0) {
            ::globfree(&g);
        }
        trx->m_op_ptr -= 2;
    } else {
        // collect matches into array
        auto count = static_cast<length_t>(g.gl_pathc);
        auto [dst_ptr, dst_offset] = trx->vm_alloc_n<Object>(count);
        for (length_t i = 0; i < count; ++i) {
            auto sv = std::string_view{g.gl_pathv[i]};
            dst_ptr[i] = Object::make_string(trx, sv);
        }
        ::globfree(&g);
        auto names_array = Object::make_array(dst_offset, count);

        // set up for-all (include saved-depth slot)
        auto saved_proc = *proc;
        trx->m_op_ptr -= 2;
        trx->require_exec_capacity(4);
        *++trx->m_exec_ptr = saved_proc;
        *++trx->m_exec_ptr = names_array;
        auto saved_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);
        *++trx->m_exec_ptr = Object::make_integer(saved_depth);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayForAll);
    }
}

// stream-position: stream :- int
// Returns the current byte position in the stream.
// throws: vm-full, io-seek-error, opstack-underflow, type-check
static void streamposition_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto [stream, sid] = trx->m_op_ptr->stream_value(trx);
    auto pos = stream->get_filepos(trx);
    *trx->m_op_ptr = Object::make_long(trx, static_cast<long_t>(pos));
}

// flush: :- --
// Flushes stdout.
// throws: io-write-error, unsupported
static void flush_op(Trix *trx) {
    auto out = trx->m_stdout;
    if (out != nullptr) {
        out->flush(trx);
    } else {
        trx->error(Error::Unsupported, "stdout support not enabled");
    }
}

// flushstream: stream :- --
// Flushes a writable stream or consumes all input of a readable stream.
// throws: invalid-stream, io-read-error, io-write-error, opstack-underflow, set-file-position-required, syntax-error,
// type-check
static void flushstream_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto [stream, sid] = trx->m_op_ptr->stream_value(trx);
    if (stream->is_open(sid)) {
        if (stream->is_writable(sid)) {
            stream->flush(trx);
        }
        if (stream->is_readable(sid)) {
            // consume all pending input
            auto ch = stream->peekc(trx);
            while (ch != EOFc) {
                stream->consume(trx);
                ch = stream->peekc(trx);
            }
            stream->reset();
        }
        --trx->m_op_ptr;
    } else {
        trx->error(Error::InvalidStream, "flushstream: stream is closed");
    }
}

// read: stream :- byte true | false
// Reads one byte from stream; pushes the Byte plus true, or false on EOF.
// Returns a Byte (not an Integer): Trix has a full numeric type tree, and
// EOF is signalled by the false branch, so C's int-from-getc sentinel
// rationale does not apply.  Matches read-key-byte, string get, unpack 'B'.
// throws: invalid-stream-access, io-read-error, opstack-overflow, opstack-underflow, set-file-position-required,
// syntax-error, type-check
static void read_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto [stream, sid] = trx->m_op_ptr->stream_value(trx);
    if (stream->is_readable(sid)) {
        auto ch = stream->peekc(trx);
        const bool noteof = (ch != EOFc);
        if (noteof) {
            trx->require_op_capacity(1);

            stream->consume(trx);
            *trx->m_op_ptr++ = Object::make_byte(static_cast<vm_t>(ch & 0xFF));
        }
        *trx->m_op_ptr = Object::make_boolean(noteof);
    } else {
        trx->error(Error::InvalidStreamAccess, "attempting to read from a write-only stream");
    }
}

// write: stream int :- --
// Writes a single byte to stream.
// throws: invalid-stream-access, io-write-error, opstack-underflow, type-check
static void write_op(Trix *trx) {
    trx->verify_operands(VerifyByte | VerifyIntegers, VerifyStream);

    auto byte_ptr = trx->m_op_ptr;
    auto stream_ptr = (byte_ptr - 1);
    auto [stream, sid] = stream_ptr->stream_value(trx);
    if (stream->is_writable(sid)) {
        auto [valid, byte_val] = byte_ptr->integer_value(trx, 0, 255);
        if (!valid) {
            trx->error(Error::RangeCheck, "write: byte value must be in 0..255");
        } else {
            stream->putc(trx, static_cast<vm_t>(byte_val));
            byte_ptr->maybe_free_extvalue(trx);
            trx->m_op_ptr -= 2;
        }
    } else {
        trx->error(Error::InvalidStreamAccess, "write: stream is not writable");
    }
}

// writestring: stream str :- --
// Writes all bytes of str to stream.
// throws: invalid-stream-access, io-write-error, opstack-underflow, type-check
static void writestring_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyStream);

    auto str_ptr = trx->m_op_ptr;
    auto stream_ptr = (str_ptr - 1);
    auto [stream, sid] = stream_ptr->stream_value(trx);
    if (stream->is_writable(sid)) {
        auto str_data = str_ptr->string_vptr(trx);
        auto len = str_ptr->string_length();
        stream->putn(trx, str_data, len);
        trx->m_op_ptr -= 2;
    } else {
        trx->error(Error::InvalidStreamAccess, "write-string: stream is not writable");
    }
}

// readhexstring: stream str :- str bool
// Reads hex-encoded data from stream into str.
// throws: invalid-stream-access, io-read-error, opstack-underflow, set-file-position-required, type-check
static void readhexstring_op(Trix *trx) {
    trx->verify_operands(VerifyRWString, VerifyStream);

    auto fp = trx->m_op_ptr - 1;
    auto [stream, sid] = fp->stream_value(trx);
    if (stream->is_readable(sid)) {
        auto str_ptr = trx->m_op_ptr;
        auto str_data = str_ptr->string_vptr(trx);
        auto length = str_ptr->string_length();
        auto limit = str_data + length;
        auto hi_nibble = true;
        auto found_eof = false;
        auto new_length = length_t{0};

        while (str_data < limit) {
            auto ch = stream->getc(trx);
            if (ch == EOFc) {
                found_eof = true;
                break;
            } else if (Trix::is_hexdigit(ch)) {
                ch = Trix::hexdigit_to_value(ch);
                if (hi_nibble) {
                    *str_data = static_cast<vm_t>(ch * 16);
                } else {
                    *str_data++ |= static_cast<vm_t>(ch);
                    ++new_length;
                }
                hi_nibble = !hi_nibble;
            }
        }
        if (!hi_nibble) {
            ++str_data;
            ++new_length;
        }
        trx->m_op_ptr->set_string_length(trx, new_length);
        trx->m_op_ptr[-1] = *trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_boolean(!found_eof);
    } else {
        trx->error(Error::InvalidStreamAccess, "read-hex-string: stream is not readable");
    }
}

// readline: stream str :- str bool
// Reads one line from stream into str.
// throws: invalid-stream-access, io-read-error, opstack-underflow, range-check, set-file-position-required, type-check
static void readline_op(Trix *trx) {
    trx->verify_operands(VerifyRWString, VerifyStream);

    auto fp = trx->m_op_ptr - 1;
    auto [stream, sid] = fp->stream_value(trx);
    if (stream->is_readable(sid)) {
        auto str_ptr = trx->m_op_ptr;
        auto str_data = str_ptr->string_vptr(trx);
        auto length = str_ptr->string_length();
        auto limit = str_data + length;
        auto new_length = length_t{0};

        auto ch = EOFc;
        while (str_data < limit) {
            ch = stream->getc(trx);
            if ((ch == EOFc) || (ch == ASCII_CR) || (ch == ASCII_LF)) {
                break;
            } else {
                *str_data++ = static_cast<vm_t>(ch);
                ++new_length;
            }
        }
        if (str_data == limit) {
            trx->error(Error::RangeCheck, "read-line: line exceeds string capacity");
        } else {
            if (ch == ASCII_CR) {
                // CRLF are always processed as a pair
                stream->matchlf(trx);
            }
            trx->m_op_ptr->set_string_length(trx, new_length);
            trx->m_op_ptr[-1] = *trx->m_op_ptr;
            *trx->m_op_ptr = Object::make_boolean(ch != EOFc);
        }
    } else {
        trx->error(Error::InvalidStreamAccess, "read-line: stream is not readable");
    }
}

// read-all: stream :- str
// Reads all remaining bytes from stream into a new string.
// throws: vm-full, invalid-stream-access, io-read-error, limit-check, opstack-underflow, set-file-position-required,
// type-check
static void readall_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto [stream, sid] = trx->m_op_ptr->stream_value(trx);
    if (!stream->is_readable(sid)) {
        trx->error(Error::InvalidStreamAccess, "read-all: stream is not readable");
    } else {
        // use dynamic allocation interface: vm_start_alloc / vm_end_alloc
        // reserve 1 byte for nul terminator
        auto [dst_base, vm_remaining] = trx->vm_start_alloc<vm_t>();
        auto dst_ptr = dst_base;
        auto dst_limit = dst_base + std::min(vm_remaining - 1, static_cast<vm_size_t>(MaxStringLength));
        while (dst_ptr < dst_limit) {
            auto ch = stream->getc(trx);
            if (ch == EOFc) {
                break;
            } else {
                *dst_ptr++ = static_cast<vm_t>(ch & 0xFF);
            }
        }
        if ((dst_ptr >= dst_limit) && (stream->peekc(trx) != EOFc)) {
            trx->vm_end_alloc();
            if (vm_remaining > static_cast<vm_size_t>(MaxStringLength)) {
                trx->error(Error::LimitCheck, "read-all: string length exceeds maximum {}", MaxStringLength);
            } else {
                trx->error(Error::VMFull, "read-all: file contents exceed available VM space");
            }
        }
        auto length = static_cast<length_t>(dst_ptr - dst_base);
        dst_base[length] = '\0';
        auto [str_ptr, str_offset] = trx->vm_end_alloc<vm_t>(static_cast<vm_size_t>(length + 1));
        *trx->m_op_ptr = Object::make_string(str_offset, length);
    }
}

// readstring: stream str :- str bool
// Reads bytes from stream to fill str.
// throws: invalid-stream-access, io-read-error, opstack-underflow, set-file-position-required, type-check
static void readstring_op(Trix *trx) {
    trx->verify_operands(VerifyRWString, VerifyStream);

    auto fp = trx->m_op_ptr - 1;
    auto [stream, sid] = fp->stream_value(trx);
    if (stream->is_readable(sid)) {
        auto str_ptr = trx->m_op_ptr;
        auto str_data = str_ptr->string_vptr(trx);
        auto length = str_ptr->string_length();
        auto limit = str_data + length;
        auto ptr = str_data;
        auto found_eof = false;
        while (ptr < limit) {
            auto ch = stream->getc(trx);
            if (ch != EOFc) {
                *ptr++ = static_cast<vm_t>(ch & 0xFF);
            } else {
                found_eof = true;
                break;
            }
        }

        trx->m_op_ptr->set_string_length(trx, static_cast<length_t>(ptr - str_data));
        trx->m_op_ptr[-1] = *trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_boolean(!found_eof);

    } else {
        trx->error(Error::InvalidStreamAccess, "read-string: stream is not readable");
    }
}

// rename-file: str str :- --
// Renames a file from oldname to newname.
// throws: filename-not-found, opstack-underflow, type-check, unsupported
static void renamefile_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "rename-file: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString, VerifyString);

        auto newname_cstr = trx->m_op_ptr->string_cstr(trx);
        auto oldname_cstr = (trx->m_op_ptr - 1)->string_cstr(trx);
        if (std::rename(oldname_cstr, newname_cstr) != 0) {
            trx->error(Error::FilenameNotFound, "rename-file: cannot rename '{}' to '{}'", oldname_cstr, newname_cstr);
        } else {
            trx->m_op_ptr -= 2;
        }
    }
}

// resetstream: stream :- --
// Resets the stream buffer.
// throws: invalid-stream, opstack-underflow, type-check
static void resetstream_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto [stream, sid] = trx->m_op_ptr->stream_value(trx);
    if (stream->is_open(sid)) {
        stream->reset();
        --trx->m_op_ptr;
    } else {
        trx->error(Error::InvalidStream, "resetstream: stream is closed");
    }
}

// set-stream-position: stream int :- --
// Seeks the stream to the given byte position.
// throws: io-seek-error, opstack-underflow, range-check, type-check
static void setstreamposition_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers, VerifyStream);

    auto pos_ptr = trx->m_op_ptr;
    auto stream_ptr = (pos_ptr - 1);
    auto [stream, sid] = stream_ptr->stream_value(trx);
    auto [valid, pos] = pos_ptr->integer_value(trx, 0);
    if (!valid) {
        trx->error(Error::RangeCheck, "set-stream-position: position must be non-negative");
    } else {
        stream->set_filepos(trx, static_cast<off64_t>(pos));
        pos_ptr->maybe_free_extvalue(trx);
        trx->m_op_ptr -= 2;
    }
}

// @with-stream: (normal-path barrier) closes the stream stored on the error stack.
// Fires when the proc body completes without error.
static void at_withstream_op(Trix *trx) {
    if (trx->m_err_ptr < trx->m_err_base) {
        trx->error(Error::InternalError, "error stack underflow in @with-stream");
    } else {
        auto stream_obj = *trx->m_err_ptr--;
        auto [stream, sid] = stream_obj.stream_value(trx);
        if (stream->is_open(sid)) {
            stream->close(trx);
        }
    }
}

// with-stream: str byte proc :- --
// Opens a file stream, pushes it on the operand stack, executes proc,
// and guarantees the stream is closed whether proc succeeds or fails.
// The stream is available inside proc on the operand stack.
// throws: errstack-overflow, execstack-overflow, file-open-error, filename-exists,
// filename-not-found, invalid-stream-access, io-seek-error, limit-check, opstack-overflow,
// opstack-underflow, type-check
static void withstream_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "with-stream: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyProc, VerifyByte, VerifyString);
        trx->require_exec_capacity(2);
        trx->require_error_capacity(1);
        trx->require_op_capacity(1);

        auto proc = *trx->m_op_ptr--;
        auto ch = trx->m_op_ptr->byte_value();
        auto lower_ch = Trix::to_lowercase(ch);
        if ((lower_ch != 'r') && (lower_ch != 'w') && (lower_ch != 'a') && (lower_ch != 'e')) {
            trx->error(Error::InvalidStreamAccess, "with-stream: invalid access mode '{:c}'", ch);
        } else {
            auto filename_obj = *(trx->m_op_ptr - 1);
            auto [stream_offset, sid] = Stream::open_file(trx, filename_obj, ch);
            auto access = (lower_ch == 'r') ? Object::ReadOnlyAccess : Object::ReadWriteAccess;
            auto stream_obj = Object::make_stream(stream_offset, sid, Object::LiteralAttrib, access);

            trx->m_op_ptr -= 2;

            // exec stack: @with-stream barrier below proc
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atWithStream);
            *++trx->m_exec_ptr = proc;

            // error stack: stream object (for cleanup on error or stop)
            *++trx->m_err_ptr = stream_obj;

            // push stream on operand stack so proc can use it
            *++trx->m_op_ptr = stream_obj;
        }
    }
}

// getcwd: :- str
// Pushes the current working directory as a string.
// throws: vm-full, opstack-overflow
static void getcwd_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "getcwd: disabled in sandbox mode");
    } else {
        trx->require_op_capacity(1);

        char buf[PATH_MAX];
        auto cwd = ::getcwd(buf, sizeof(buf));
        if (cwd == nullptr) {
            auto errno_str = errno_string();
            trx->error(Error::IOReadError, "getcwd: {}/{}", errno, errno_str);
        } else {
            *++trx->m_op_ptr = Object::make_string(trx, std::string_view{cwd});
        }
    }
}

// file-exists?: str :- bool
// Tests whether the named file exists and is accessible.
// throws: opstack-underflow, type-check
static void fileexists_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto cstr = trx->m_op_ptr->string_cstr(trx);
    *trx->m_op_ptr = Object::make_boolean(::access(cstr, F_OK) == 0);
}

// file-size: str :- long
// Pushes the size in bytes of the named file.
// throws: filename-not-found, opstack-underflow, type-check, vm-full
static void filesize_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto cstr = trx->m_op_ptr->string_cstr(trx);
    struct stat st{};
    if (::stat(cstr, &st) != 0) {
        trx->error(Error::FilenameNotFound, "file-size: cannot stat '{}'", cstr);
    } else {
        *trx->m_op_ptr = Object::make_long(trx, static_cast<long_t>(st.st_size));
    }
}

// mkdir: str :- --
// Creates a directory with default permissions (0777, modified by umask).
// throws: filename-not-found, opstack-underflow, type-check
static void mkdir_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "mkdir: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        auto cstr = trx->m_op_ptr->string_cstr(trx);
        if (::mkdir(cstr, 0777) != 0) {
            auto errno_str = errno_string();
            trx->error(Error::FilenameNotFound, "mkdir: cannot create '{}': {}/{}", cstr, errno, errno_str);
        } else {
            --trx->m_op_ptr;
        }
    }
}

// rmdir: str :- --
// Removes an empty directory.
// throws: filename-not-found, opstack-underflow, type-check, unsupported
static void rmdir_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "rmdir: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        auto cstr = trx->m_op_ptr->string_cstr(trx);
        if (::rmdir(cstr) != 0) {
            auto errno_str = errno_string();
            trx->error(Error::FilenameNotFound, "rmdir: cannot remove '{}': {}/{}", cstr, errno, errno_str);
        } else {
            --trx->m_op_ptr;
        }
    }
}

// chdir: str :- --
// Changes the current working directory.
// throws: filename-not-found, opstack-underflow, type-check, unsupported
static void chdir_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "chdir: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        auto cstr = trx->m_op_ptr->string_cstr(trx);
        if (::chdir(cstr) != 0) {
            auto errno_str = errno_string();
            trx->error(Error::FilenameNotFound, "chdir: cannot change to '{}': {}/{}", cstr, errno, errno_str);
        } else {
            --trx->m_op_ptr;
        }
    }
}

// chmod: str int :- --
// Changes the file mode bits (permissions) of the named file.
// throws: filename-not-found, opstack-underflow, type-check, unsupported
static void chmod_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "chmod: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyIntegers, VerifyString);

        auto mode_ptr = trx->m_op_ptr;
        auto [valid, mode_val] = mode_ptr->integer_value(trx, 0, 07777);
        if (!valid) {
            trx->error(Error::RangeCheck, "chmod: mode bits must be in 0..07777");
        } else {
            auto cstr = (mode_ptr - 1)->string_cstr(trx);
            if (::chmod(cstr, static_cast<mode_t>(mode_val)) != 0) {
                auto errno_str = errno_string();
                trx->error(Error::FilenameNotFound, "chmod: cannot chmod '{}': {}/{}", cstr, errno, errno_str);
            } else {
                mode_ptr->maybe_free_extvalue(trx);
                trx->m_op_ptr -= 2;
            }
        }
    }
}

// file-stat: str :- dict
// Pushes a dict with file metadata: /size (long), /mtime (long), /mode (integer),
// /type (name: /file, /directory, /symlink, /other), /nlink (integer).
// throws: filename-not-found, opstack-underflow, type-check, unsupported, vm-full
static void filestat_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "file-stat: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        auto cstr = trx->m_op_ptr->string_cstr(trx);
        struct stat st{};
        if (::stat(cstr, &st) != 0) {
            trx->error(Error::FilenameNotFound, "file-stat: cannot stat '{}'", cstr);
        } else {
            auto [dict, dict_offset] = Dict::create_dict(trx, 5);
            static_cast<void>(dict->put(trx, Name::make(trx, "size"), Object::make_long(trx, static_cast<long_t>(st.st_size))));
            static_cast<void>(dict->put(trx, Name::make(trx, "mtime"), Object::make_long(trx, static_cast<long_t>(st.st_mtime))));
            static_cast<void>(
                    dict->put(trx, Name::make(trx, "mode"), Object::make_integer(static_cast<integer_t>(st.st_mode & 07777))));
            static_cast<void>(dict->put(trx, Name::make(trx, "nlink"), Object::make_integer(static_cast<integer_t>(st.st_nlink))));

            Object type_name;
            if (S_ISREG(st.st_mode)) {
                type_name = Name::make(trx, "file");
            } else if (S_ISDIR(st.st_mode)) {
                type_name = Name::make(trx, "directory");
            } else if (S_ISLNK(st.st_mode)) {
                type_name = Name::make(trx, "symlink");
            } else {
                type_name = Name::make(trx, "other");
            }
            static_cast<void>(dict->put(trx, Name::make(trx, "type"), type_name));

            *trx->m_op_ptr = Object::make_dict(dict_offset);
        }
    }
}
