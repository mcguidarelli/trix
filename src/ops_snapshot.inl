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

//===--- Save, Restore, Snapshot, and Thaw Operators ---===//
//
// Implements the user-facing save/restore operators and the snap-shot/thaw
// subsystem for serializing the entire VM state to binary image files.
// Based on:
//
//   PostScript save/restore: user-level operators for creating and rolling
//   back VM checkpoints.  See save.inl for the journal mechanism.
//
//   Smalltalk image persistence: saving the complete runtime state to a file
//   and resuming from it later.  Trix's snap-shot/thaw is a simpler version:
//   the heap + header are written as a binary blob, with stream reconnection
//   for open files.
//
// --- Core concepts for maintainers ---
//
// SAVE / RESTORE OPERATORS
//   save     -- push a save checkpoint, return an Integer save token
//   restore  -- roll back to a save checkpoint (positive token: absolute;
//               negative -N: relative pop; replays journal, resets heap)
//   These are the user-visible interface to the Save class (save.inl).
//   restore validates that no objects created after the save are still
//   referenced on the operand stack (the "past save barrier" check).
//
// SNAP-SHOT (serialize)
//   snap-shot writes the entire VM state to a file:
//     1. Flush pending writes on seekable user-file streams and on
//        stdout/stderr; close non-seekable user streams.
//     2. Write the SnapShotHeader (root pointers, member variables).
//     3. Write memory stream blocks (IsMemory external buffers + the
//        startup-file tail).
//     4. For each open seekable user file stream: write stream offset,
//        file position, mode, flags, and the (re-derived) filename.
//     5. Write the local VM blob (m_vm_base, vm_used bytes).
//     6. Write the global VM blob (m_vm_global..m_vm_limit,
//        vm_global_used bytes) when present.
//   The result is a self-contained binary image.
//
// THAW (deserialize)
//   thaw loads a snap-shot image and resumes execution:
//     1. Read the SnapShotHeader.
//     2. Validate version, heap size, and structural integrity.
//     3. Read the VM heap into memory.
//     4. Restore all member variables from the header.
//     5. Reconnect file streams (reopen files, seek to saved positions).
//     6. Resume the interpreter (exec stack is intact from the snapshot).
//
// STARTUP IMAGE
//   startup_image loads an image file at VM construction time (--image
//   mode).  It uses the same thaw logic but runs during init rather than
//   as an operator.
//
// STREAM RECONNECTION
//   File streams open at snap-shot time have their filename and position
//   saved.  On thaw, each file is reopened and seeked to the saved
//   position.  Memory (IsMemory) streams from the invoke API are
//   preserved by serializing their external buffer directly into the
//   image; on thaw the buffer is malloc'd and read back (tracked via
//   RestoredMemStream, snapshot.inl), so no host involvement is needed.
//   m_invoke_length/m_invoke_data are reset to 0/nullptr on thaw.
//
// --- Operators ---
//
//   save         -- int           Create save checkpoint, push token
//   restore      int --           Roll back to checkpoint (-N: relative pop)
//   save-level?  int -- int       Decode a save token's level (0 = BASE)
//   recover-save int -- int       Recreate the token for an active level (1..curr)
//   snap-shot    filename --      Serialize VM to file
//   thaw         filename --      Deserialize VM from file
//

// restore: int :- --
// Restores the VM to a save point.  The integer is a save token:
//   positive: packed (level | gen ^ barrier_low23) returned by save / recover-save.
//             Validates the token's generation field against the slot;
//             stale tokens (slot recycled by a subsequent save+restore cycle)
//             raise /invalid-restore.
//   negative: relative pop -- `-N` pops |N| save levels.  Always valid by
//             construction (uses the current top of the save stack).
//   zero:    raises /invalid-restore.
// throws: vm-full, internal-error, invalid-restore, opstack-underflow, type-check
static void restore_op(Trix *trx) {
    trx->verify_operands(VerifyInteger);
    trx->require_save_count(2);

    auto signed_token = trx->m_op_ptr->integer_value();

    // Save::restore (called inside restore_signed_token) handles the op-stack pop
    // by walking the stack and reclaiming slots above the barrier.
    Save::restore_signed_token(trx, signed_token);
}

// save: :- int
// Creates a VM save point and pushes its packed save token (Integer).
// The token encodes (save level | gen ^ barrier_low23) and validates
// stale-token detection on restore.
// throws: vm-full, limit-check, opstack-overflow
static void save_op(Trix *trx) {
    trx->require_op_capacity(1);
    trx->require_save_capacity(1);

    Save::save(trx);
}

// save-level?: int -- int
// Consumes a save token and returns its save level (0 = BASE, negative input -> 0).
// Decodes from the packed token's low 8 bits; does not validate gen
// (a stale token reports its encoded level rather than erroring).
// throws: opstack-underflow, type-check
static void save_level_q_op(Trix *trx) {
    trx->verify_operands(VerifyInteger);

    auto signed_token = trx->m_op_ptr->integer_value();
    auto level = (signed_token > 0) ? Save::token_level(static_cast<vm_offset_t>(signed_token)) : Save::BASE;
    *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(level));
}

// recover-save: int -- int
// Recreates a save token (Integer) for any active save level (1..curr_save_level).
// The recovered token is identical to the one originally returned by save.
// throws: opstack-underflow, type-check, range-check
static void recover_save_op(Trix *trx) {
    trx->verify_operands(VerifyInteger);

    auto level = trx->m_op_ptr->integer_value();
    if ((level < 1) || (level > static_cast<integer_t>(trx->m_curr_save_level))) {
        trx->error(Error::RangeCheck, "recover-save: level {} out of range (active: 1..{})", level, trx->m_curr_save_level);
    } else {
        auto lvl = static_cast<save_level_t>(level);
        auto barrier = trx->m_save_stack[lvl];
        auto gen = trx->m_save_generation[lvl];
        auto token = Save::pack_token(lvl, gen, barrier);
        *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(token));
    }
}

// Write exactly n bytes to fd, retrying on short writes and EINTR.
// Returns true on success, false on error (errno is set by the failing write).
static bool write_all(int fd, const void *buf, size_t n) {
    auto p = static_cast<const char *>(buf);
    while (n > 0) {
        auto nw = ::write(fd, p, n);
        if (nw < 0) {
            // EINTR: interrupted before any byte -- retry; any other error is fatal
            if (errno != EINTR) {
                return false;
            }
        } else if (nw == 0) {
            return false;
        } else {
            p += nw;
            n -= static_cast<size_t>(nw);
        }
    }
    return true;
}

// Read exactly n bytes from fd, retrying on short reads and EINTR.
// Returns true on success, false on error or premature EOF.
static bool read_all(int fd, void *buf, size_t n) {
    auto p = static_cast<char *>(buf);
    while (n > 0) {
        auto nr = ::read(fd, p, n);
        if (nr < 0) {
            // EINTR: interrupted before any byte -- retry; any other error is fatal
            if (errno != EINTR) {
                return false;
            }
        } else if (nr == 0) {
            return false;
        } else {
            p += nr;
            n -= static_cast<size_t>(nr);
        }
    }
    return true;
}

// Copy a Trix string_view into a NUL-terminated C-string buffer for the POSIX
// path APIs (open64/rename/realpath), which require a terminated `const char*`.
// Raises `err` if the path plus its terminator would not fit in `cap` -- a path
// at/over PATH_MAX cannot be opened anyway, so rejecting it here fails earlier
// and more clearly than a downstream ENAMETOOLONG.  No heap, no VM allocation.
static void copy_sv_to_path(Trix *trx, std::string_view sv, char *out, size_t cap, Error err, const char *what) {
    auto length = sv_length(sv);
    if ((static_cast<size_t>(length) + 1) > cap) {
        trx->error(err, "{}: path too long ({} bytes, max {})", what, length, cap - 1);
    } else {
        std::memcpy(out, sv.data(), length);
        out[length] = '\0';
    }
}

// Resolve a seekable user-file stream's source name to an absolute path, writing the
// NUL-terminated result into `out` (capacity `cap`, >= PATH_MAX for the realpath case)
// and returning its length.  Absolute sources are copied verbatim; relative sources are
// resolved via realpath so thaw works even if the working directory changed since
// snap-shot; realpath failure falls back to the raw name.  The one transient realpath
// input buffer lives in this frame -- no heap, no VM allocation -- and resolution is
// deterministic across snap-shot's CRC and write passes (the file is held open).
//
// noinline (here and on the sibling snap-shot/thaw helpers below): each owns a
// PATH_MAX or 4 KB C-stack buffer, and these are cold paths.  Without noinline an
// optimized build inlines them into snapshot_op/thaw_op, merging every buffer into
// one frame and blowing the per-function stack-usage budget.  Keeping them out-of-line
// keeps each frame small (no perf cost on a cold path).
[[gnu::noinline]] static size_t resolve_stream_path(Trix *trx, Stream *s, char *out, size_t cap) {
    auto src = s->source(trx);
    auto length = static_cast<size_t>(sv_length(src));
    if ((length > 0) && (src.data()[0] == '/')) {
        if ((length + 1) > cap) {
            trx->error(Error::IOWriteError, "snap-shot: stream path too long ({} bytes)", length);
        } else {
            std::memcpy(out, src.data(), length);
            out[length] = '\0';
            return length;
        }
    }
    // Relative (or empty): NUL-terminate the source, then realpath into `out`.
    char rawbuf[PATH_MAX];
    if ((length + 1) > sizeof(rawbuf)) {
        trx->error(Error::IOWriteError, "snap-shot: stream path too long ({} bytes)", length);
    } else {
        std::memcpy(rawbuf, src.data(), length);
        rawbuf[length] = '\0';
        // realpath() writes up to PATH_MAX bytes into `out`, so only call it when `out`
        // is at least that large (callers pass PATH_MAX buffers); a smaller buffer would
        // be a buffer overflow.  This PATH_MAX requirement is why the snap-shot path
        // helpers carry PATH_MAX buffers and stay [[gnu::noinline]].
        if ((cap >= PATH_MAX) && (::realpath(rawbuf, out) != nullptr)) {
            return std::strlen(out);
        } else if ((length + 1) > cap) {
            trx->error(Error::IOWriteError, "snap-shot: stream path too long ({} bytes)", length);
        } else {
            std::memcpy(out, rawbuf, length);
            out[length] = '\0';
            return length;
        }
    }
}

// Feed a user-file stream's reconnection filename (uint16 length + name bytes) into
// `crc` and return the updated CRC.  The PATH_MAX name buffer lives in this frame --
// not the caller's -- so snapshot_op stays within the per-function C-stack budget.
[[gnu::noinline]] static crc32_t crc_stream_path(Trix *trx, Stream *s, crc32_t crc) {
    char namebuf[PATH_MAX];
    auto fnlen = static_cast<uint16_t>(resolve_stream_path(trx, s, namebuf, sizeof(namebuf)));
    crc = crc32_update(crc, &fnlen, sizeof(fnlen));
    crc = crc32_update(crc, namebuf, fnlen);
    return crc;
}

// Write a user-file stream's reconnection filename (uint16 length + name bytes) to
// `dst_fd`.  Name buffer lives in this frame (see crc_stream_path).
[[gnu::noinline]] static void write_stream_path(Trix *trx, Stream *s, int dst_fd) {
    char namebuf[PATH_MAX];
    auto fnlen = static_cast<uint16_t>(resolve_stream_path(trx, s, namebuf, sizeof(namebuf)));
    if (!write_all(dst_fd, &fnlen, sizeof(fnlen)) || !write_all(dst_fd, namebuf, fnlen)) {
        auto errno_str = errno_string();
        trx->error(Error::IOWriteError, "snap-shot: user file stream block write failed: {}/{}", errno, errno_str);
    }
}

// Feed `length` bytes starting at file offset `off` of `fd` into `crc` and return the
// updated CRC, restoring the fd to `off` afterward.  Lets the snap-shot startup-file
// tail be CRC'd without buffering it.  One transient 4 KB C-stack chunk -- no heap.
[[gnu::noinline]] static crc32_t crc_fd_range(Trix *trx, int fd, off64_t off, size_t length, crc32_t crc) {
    if (::lseek64(fd, off, SEEK_SET) == -1) {
        auto errno_str = errno_string();
        trx->error(Error::IOSeekError, "snap-shot: startup tail seek failed: {}/{}", errno, errno_str);
    } else {
        uint8_t chunk[SnapshotIoBufSize];
        auto remaining = length;
        while (remaining > 0) {
            auto n = std::min(remaining, sizeof(chunk));
            if (!read_all(fd, chunk, n)) {
                auto errno_str = errno_string();
                trx->error(Error::IOReadError, "snap-shot: startup tail read failed: {}/{}", errno, errno_str);
            } else {
                crc = crc32_update(crc, chunk, n);
                remaining -= n;
            }
        }
        if (::lseek64(fd, off, SEEK_SET) == -1) {
            auto errno_str = errno_string();
            trx->error(Error::IOSeekError, "snap-shot: startup tail seek-restore failed: {}/{}", errno, errno_str);
        } else {
            return crc;
        }
    }
}

// Copy `length` bytes starting at file offset `off` of `src_fd` to `dst_fd`, restoring
// src_fd to `off` afterward.  Streams the snap-shot startup-file tail to the image
// without buffering it.  One transient 4 KB C-stack chunk -- no heap.
[[gnu::noinline]] static void write_fd_range(Trix *trx, int src_fd, off64_t off, size_t length, int dst_fd) {
    if (::lseek64(src_fd, off, SEEK_SET) == -1) {
        auto errno_str = errno_string();
        trx->error(Error::IOSeekError, "snap-shot: startup tail seek failed: {}/{}", errno, errno_str);
    } else {
        uint8_t chunk[SnapshotIoBufSize];
        auto remaining = length;
        while (remaining > 0) {
            auto n = std::min(remaining, sizeof(chunk));
            if (!read_all(src_fd, chunk, n)) {
                auto errno_str = errno_string();
                trx->error(Error::IOReadError, "snap-shot: startup tail read failed: {}/{}", errno, errno_str);
            } else if (!write_all(dst_fd, chunk, n)) {
                auto errno_str = errno_string();
                trx->error(Error::IOWriteError, "snap-shot: startup tail block write failed: {}/{}", errno, errno_str);
            } else {
                remaining -= n;
            }
        }
        if (::lseek64(src_fd, off, SEEK_SET) == -1) {
            auto errno_str = errno_string();
            trx->error(Error::IOSeekError, "snap-shot: startup tail seek-restore failed: {}/{}", errno, errno_str);
        }
    }
}

// snap-shot: str :- --
// Writes a VM snapshot to the named file.
// throws: vm-full, file-open-error, io-seek-error, io-write-error, opstack-underflow, type-check
static void snapshot_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "snap-shot: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        // Copy the target filename into a C-stack buffer: NUL-terminated for open64/
        // rename, and so we can build "<fn>.tmp" below.  No heap allocation.
        char target[PATH_MAX];
        copy_sv_to_path(trx, trx->m_op_ptr->sv_value(trx), target, sizeof(target), Error::FileOpenError, "snap-shot");

        // Pop the filename object before any writes so the operand stack is in its
        // final state when the VM image is captured.
        --trx->m_op_ptr;

        // Temps are transient within single operators -- none should survive to snapshot.
        // The idle position for m_vm_temp_ptr is m_vm_global (which equals m_vm_limit when
        // no globals are present); a non-idle temp watermark drops it below m_vm_global.
        if (trx->m_vm_temp_ptr != trx->m_vm_global) {
            trx->error(Error::LimitCheck, "snap-shot: temporary allocations are active");
        } else {
            // Pre-process seekable user file streams: normalize buffer state, record info.
            // These streams are NOT closed; they stay in the inuse list for the VM image.
            // Non-seekable user file streams are still closed below (existing behavior).
            // Per seekable user-file stream: the scalar reconnection record.  The filename
            // is NOT stored here (inline storage would be up to MaxStreamCount * PATH_MAX of
            // stack); it is re-derived from the still-open stream via resolve_stream_path()
            // in the CRC and write passes below.  Bounded by MaxStreamCount -- stdio streams
            // are separate allocations, so the inuse list holds at most m_stream_count
            // (<= MaxStreamCount) user streams.
            struct UserFileInfo {
                int64_t file_offset;  // first so the struct packs to 16 bytes, not 24
                vm_offset_t stream_offset;
                uint8_t open_mode;  // 0=RDONLY, 1=WRONLY, 2=RDWR
                uint8_t flags;      // bit 0: IsAppend -- on reconnect seek to EOF instead of file_offset
            };
            UserFileInfo user_file_infos[MaxStreamCount];
            uint16_t user_file_count = 0;

            for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                if (!s->is_stdio() && !s->is_memory() && !s->is_startup_file() && s->supports_random_access() && (s->m_fd >= 0)) {
                    // 1. Seek fd back to logical read position (past buffered-but-unread bytes).
                    if ((s->m_status & Stream::ReadData) != 0) {
                        // Use signed arithmetic: if the buffer invariant is violated (rptr > rlimit),
                        // unread is negative, the guard below rejects it, and the fd position is
                        // captured unchanged -- the subsequent lseek64(SEEK_CUR) reports where it is.
                        auto unread = static_cast<off64_t>(s->m_rlimit_offset) - static_cast<off64_t>(s->m_rptr_offset);
                        if (unread > 0) {
                            if (::lseek64(s->m_fd, -unread, SEEK_CUR) == -1) {
                                auto errno_str = errno_string();
                                trx->error(Error::IOSeekError,
                                           "snap-shot: seek-back of unread buffer failed: {}/{}",
                                           errno,
                                           errno_str);
                            }
                        }
                    }
                    // 2. Flush any pending write data (only if WriteData is set; IsWritable alone
                    //    means the stream can write, not that there is buffered write data pending).
                    //    Calling flush() on a stream in read mode (ReadData set) would error.
                    if ((s->m_status & Stream::WriteData) != 0) {
                        s->flush(trx);
                    }
                    // 3. Reset buffer (empty read buf; write ptr back to wlimit; clear RW flags).
                    s->reset();
                    // 4. Capture logical position (fd is now at the right place).
                    auto pos = ::lseek64(s->m_fd, 0, SEEK_CUR);
                    if (pos == -1) {
                        auto errno_str = errno_string();
                        trx->error(Error::IOSeekError, "snap-shot: cannot get file position: {}/{}", errno, errno_str);
                    } else {
                        // 5. Derive open mode and flags from status.
                        const bool rd = (s->m_status & Stream::IsReadable) != 0;
                        const bool wr = (s->m_status & Stream::IsWritable) != 0;
                        const bool app = (s->m_status & Stream::IsAppend) != 0;
                        const uint8_t open_mode = (rd && wr) ? uint8_t{2} : wr ? uint8_t{1} : uint8_t{0};
                        const uint8_t flags = app ? uint8_t{1} : uint8_t{0};
                        // 6. Record the scalar reconnection info.  The absolute filename (so
                        //    thaw works even if the working directory changed) is re-derived
                        //    from this still-open stream via with_resolved_stream_path() in the
                        //    CRC and write passes below -- not stored, to keep the array small.
                        if (user_file_count >= MaxStreamCount) {
                            trx->error(Error::InternalError, "snap-shot: user-file stream count exceeds MaxStreamCount");
                        } else {
                            user_file_infos[user_file_count++] = {pos, trx->ptr_to_offset(s), open_mode, flags};
                        }
                    }
                }
            }

            // Capture remaining startup file content beyond the buffer so that thaw can
            // restore it as the stream's ext buffer (making the .img self-contained).
            // The startup file is a regular IsFile stream -- its fd is replaced with /dev/null
            // on thaw, so without this step any content past the buffer would be lost.
            //
            // The fd is positioned just past the last byte that was read into the VM buffer
            // (the scanner has consumed some of those bytes but hasn't advanced the fd).
            // We read from the fd's current position to EOF -- this is the content that
            // fill() would read on subsequent buffer refills during normal execution.
            Stream *startup_stream = nullptr;
            off64_t startup_tail_off = 0;
            size_t startup_tail_size = 0;
            for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                if (s->is_startup_file() && (s->m_fd >= 0)) {
                    startup_stream = s;
                    // Determine remaining bytes from fd position to EOF.
                    auto cur = ::lseek64(s->m_fd, 0, SEEK_CUR);
                    auto end = ::lseek64(s->m_fd, 0, SEEK_END);
                    if ((cur == -1) || (end == -1)) {
                        auto errno_str = errno_string();
                        trx->error(Error::IOSeekError, "snap-shot: startup stream position query failed: {}/{}", errno, errno_str);
                    } else {
                        // Record the tail's position + size only; the bytes are streamed
                        // straight from this fd in the CRC and write passes (scan_fd_range)
                        // rather than buffered.  Seek the fd back to the logical read position
                        // so post-snap-shot execution and every streaming pass start there.
                        startup_tail_off = cur;
                        startup_tail_size = static_cast<size_t>(end - cur);
                        if (::lseek64(s->m_fd, cur, SEEK_SET) == -1) {
                            auto errno_str = errno_string();
                            trx->error(
                                    Error::IOSeekError, "snap-shot: startup stream seek-restore failed: {}/{}", errno, errno_str);
                        } else {
                            // Do NOT modify the stream's ext fields -- the VM blob must be serialized
                            // with the stream struct unchanged.  The startup tail is written as a
                            // separate memory stream block keyed by stream offset.
                            break;  // at most one startup file stream
                        }
                    }
                }
            }

            // Close all open non-stdio, non-memory, non-startup, non-seekable streams before snap-shot.
            // IsMemory streams are preserved: their external buffer is serialized below.
            // IsStartupFile is preserved open so apply_fixup_streams can replace its fd with
            // /dev/null and restore the tail ext buffer after thaw.
            // Seekable user file streams are normalized above and kept open in the VM image.
            // Warn for non-seekable user file streams that will be silently closed and lost.
            for (auto s = trx->m_stream_inuse_list; s != nullptr; /* empty */) {
                auto next = s->next_stream(trx);
                if (!s->is_stdio() && !s->is_memory() && !s->is_startup_file() && !s->supports_random_access()) {
                    if (s->m_fd >= 0) {
                        auto src = s->source(trx);
                        trx->diag_println("snap-shot: warning: non-seekable stream '{}' will be closed and "
                                          "cannot be reconnected after thaw",
                                          std::string_view(src.data(), sv_length(src)));
                    }
                    s->close(trx);
                }
                s = next;
            }

            // Flush pending output before capturing the VM image so that no buffered
            // bytes are silently discarded when thaw resets the write-buffer state.
            if (trx->m_stdout != nullptr) {
                trx->m_stdout->flush(trx);
            }
            if (trx->m_stderr != nullptr) {
                trx->m_stderr->flush(trx);
            }

            // Write to a temporary file so that a partial or failed write never
            // leaves the target .img in a corrupted state.  Rename atomically on success.
            // Build "<target>.tmp" in a C-stack buffer (+5 covers ".tmp" plus its NUL).
            char tmp[PATH_MAX + 5];
            auto target_len = std::strlen(target);
            std::memcpy(tmp, target, target_len);
            std::memcpy(tmp + target_len, ".tmp", 5);  // includes the trailing NUL
            auto fd = ::open64(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0666);
            if (fd == -1) {
                auto errno_str = errno_string();
                trx->error(Error::FileOpenError, "snap-shot: cannot open '{}': {}/{}", tmp, errno, errno_str);
            } else {
                // RAII guard: on any trx->error() throw, closes fd (unless already -1) and
                // unlinks the partial temporary file.
                struct Guard {
                    Guard(int *f, const char *t) : m_fd(f), m_tmp(t), m_dismissed(false) {}

                    ~Guard() {
                        if (!m_dismissed) {
                            if (*m_fd >= 0) {
                                ::close(*m_fd);
                            }
                            ::unlink(m_tmp);
                        }
                    }

                    void dismiss() { m_dismissed = true; }

                    int *m_fd;
                    const char *m_tmp;
                    bool m_dismissed;
                } guard(&fd, tmp);

                // Build the snap-shot header.
                SnapShotHeader h{};
                h.magic[0] = 'T';
                h.magic[1] = 'R';
                h.magic[2] = 'I';
                h.magic[3] = 'X';
                h.snapshot_version = SNAPSHOT_VERSION;
                h.operator_table_signature = compute_operator_table_signature();
                h.operator_count = static_cast<uint32_t>(SYSOPERATOR_COUNT);
                h.endian_le = (std::endian::native == std::endian::little) ? uint8_t{1} : uint8_t{0};

                h.vm_base_addr = reinterpret_cast<uint64_t>(trx->m_vm_base);
                h.vm_used = static_cast<uint32_t>(trx->m_vm_ptr - trx->m_vm_base);
                h.vm_global_used = static_cast<uint32_t>(trx->m_vm_limit - trx->m_vm_global);
                h.vm_capacity = static_cast<uint32_t>(trx->m_vm_limit - trx->m_vm_base);
                h.curr_alloc_global = trx->m_curr_alloc_global;
                h.gc_current_gen = trx->m_gc_current_gen;
                h.gvm_free_head = trx->m_gvm_free_head;
                for (length_t i = 0; i < GvmFastBinCount; ++i) {
                    h.gvm_fastbins[i] = trx->m_gvm_fastbins[i];
                }
                h.gc_scratch_offset = trx->m_gc_scratch_offset;
                h.gvm_user_block_count = static_cast<uint32_t>(trx->m_gvm_user_block_count);
                h.gvm_free_block_count = static_cast<uint32_t>(trx->m_gvm_free_block_count);

                h.max_save_level = trx->m_max_save_level;
                h.curr_save_level = trx->m_curr_save_level;
                h.name_bucket_count = trx->m_name_bucket_count;
                h.next_sid = trx->m_next_sid;
                h.sid_wrapped = trx->m_sid_wrapped;
                h.stream_buffer_size = trx->m_stream_buffer_size;
                h.eqproc_length = trx->m_eqproc_length;
                h.eqproc_stored_length = trx->m_eqproc_stored_length;
                h.eqdict_maxlength = trx->m_eqdict_maxlength;
                h.eqset_maxlength = trx->m_eqset_maxlength;
                h.eqstring_generation = trx->m_eqstring_generation;
                h.eqarray_generation = trx->m_eqarray_generation;
                h.eqproc_generation = trx->m_eqproc_generation;
                h.eqdict_generation = trx->m_eqdict_generation;
                h.eqset_generation = trx->m_eqset_generation;
                h.last_error = trx->m_last_error;
                h.external_error = trx->m_external_error;
                h.interrupt = trx->m_interrupt;
                h.interrupt_mask = trx->m_interrupt_mask;

                h.root_objects_offset = trx->nullptr_to_offset(trx->m_root_objects_ptr);

                std::tie(h.pcg32_state, h.pcg32_inc) = trx->m_pcg32.snapshot_state();

                h.vm_ptr_offset = trx->nullptr_to_offset(trx->m_vm_ptr);
                h.vm_alloc_active_offset = trx->nullptr_to_offset(trx->m_vm_alloc_active);
                h.save_stack_offset = trx->nullptr_to_offset(trx->m_save_stack);
                h.save_generation_offset = trx->nullptr_to_offset(trx->m_save_generation);
                h.extvalue_free_list_offset = trx->nullptr_to_offset(trx->m_extvalue_free_list);
                h.widevalue_free_list_offset = trx->nullptr_to_offset(trx->m_widevalue_free_list);
                h.vm_temp_save_offset = trx->nullptr_to_offset(static_cast<const void *>(trx->m_vm_temp_save));
                h.extvalue_active_save_offset = trx->nullptr_to_offset(trx->m_extvalue_active_save);
                h.widevalue_active_save_offset = trx->nullptr_to_offset(trx->m_widevalue_active_save);
                h.save_journal_count_offset = trx->nullptr_to_offset(trx->m_save_journal_count);
                h.save_journal_bytes_offset = trx->nullptr_to_offset(trx->m_save_journal_bytes);
                h.dict_pool_offset = trx->nullptr_to_offset(trx->m_dict_pool);
                h.frame_dict_pool_offset = trx->nullptr_to_offset(trx->m_frame_dict_pool);
                h.mailbox_pool_offset = trx->nullptr_to_offset(trx->m_mailbox_pool);
                h.monitor_pool_offset = trx->m_monitor_pool;  // bare offset, not a pointer to an array
                h.pipe_pool_offset = trx->nullptr_to_offset(trx->m_pipe_pool);
                h.op_base_offset = trx->nullptr_to_offset(trx->m_op_base);
                h.op_ptr_offset = trx->nullptr_to_offset(trx->m_op_ptr);
                h.op_limit_offset = trx->nullptr_to_offset(trx->m_op_limit);
                h.dict_base_offset = trx->nullptr_to_offset(trx->m_dict_base);
                h.dict_ptr_offset = trx->nullptr_to_offset(trx->m_dict_ptr);
                h.dict_limit_offset = trx->nullptr_to_offset(trx->m_dict_limit);
                h.exec_base_offset = trx->nullptr_to_offset(trx->m_exec_base);
                h.exec_ptr_offset = trx->nullptr_to_offset(trx->m_exec_ptr);
                h.exec_limit_offset = trx->nullptr_to_offset(trx->m_exec_limit);
                h.err_base_offset = trx->nullptr_to_offset(trx->m_err_base);
                h.err_ptr_offset = trx->nullptr_to_offset(trx->m_err_ptr);
                h.err_limit_offset = trx->nullptr_to_offset(trx->m_err_limit);
                h.scratch_base_offset = trx->nullptr_to_offset(trx->m_scratch_base);
                h.scratch_ptr_offset = trx->nullptr_to_offset(trx->m_scratch_ptr);
                h.scratch_limit_offset = trx->nullptr_to_offset(trx->m_scratch_limit);
                h.gc_roots_base_offset = trx->nullptr_to_offset(trx->m_gc_roots_base);
                h.error_string_base_offset = trx->nullptr_to_offset(trx->m_error_string_base);
                h.error_string_limit_offset = trx->nullptr_to_offset(trx->m_error_string_limit);
                h.token_base_offset = trx->nullptr_to_offset(trx->m_token_base);
                h.token_limit_offset = trx->nullptr_to_offset(trx->m_token_limit);
                h.name_buckets_offset = trx->nullptr_to_offset(trx->m_name_buckets);
                h.systemname_offsets_offset = trx->nullptr_to_offset(trx->m_systemname_offsets);
                h.typename_offsets_offset = trx->nullptr_to_offset(trx->m_typename_offsets);
                h.errorname_offsets_offset = trx->nullptr_to_offset(trx->m_errorname_offsets);
                h.wellknown_offsets_offset = trx->nullptr_to_offset(trx->m_wellknown_offsets);
                h.systemdict_offset = trx->nullptr_to_offset(trx->m_systemdict);
                h.protocoldict_offset = trx->nullptr_to_offset(trx->m_protocoldict);
                h.userdict_offset = trx->nullptr_to_offset(trx->m_userdict);
                h.errordict_offset = trx->nullptr_to_offset(trx->m_errordict);
                h.handlersdict_offset = trx->nullptr_to_offset(trx->m_handlersdict);
                h.eqproc_storage_offset = trx->nullptr_to_offset(trx->m_eqproc_storage_ptr);
                h.eqdict_offset = trx->nullptr_to_offset(trx->m_eqdict);
                h.eqset_offset = trx->nullptr_to_offset(trx->m_eqset);
                h.stream_inuse_list_offset = trx->nullptr_to_offset(trx->m_stream_inuse_list);
                h.stream_free_list_offset = trx->nullptr_to_offset(trx->m_stream_free_list);
                h.stdin_offset = trx->nullptr_to_offset(trx->m_stdin);
                h.stdedit_offset = trx->nullptr_to_offset(trx->m_stdedit);
                h.stdout_offset = trx->nullptr_to_offset(trx->m_stdout);
                h.stderr_offset = trx->nullptr_to_offset(trx->m_stderr);
                h.last_error_name_offset = trx->nullptr_to_offset(trx->m_last_error_name_ptr);
                h.last_operator_offset = trx->nullptr_to_offset(trx->m_last_operator_ptr);
                h.last_error_msg_offset = trx->nullptr_to_offset(trx->m_last_error_msg_ptr);
                h.last_error_data_offset = trx->nullptr_to_offset(trx->m_last_error_data_ptr);
                h.ostack_offset = trx->nullptr_to_offset(trx->m_ostack_ptr);
                h.dstack_offset = trx->nullptr_to_offset(trx->m_dstack_ptr);
                h.estack_offset = trx->nullptr_to_offset(trx->m_estack_ptr);
                h.require_dict_offset = trx->m_require_dict_offset;
                h.modules_dict_offset = trx->m_modules_dict_offset;
                h.protocol_registry_offset = trx->m_protocol_registry_offset;

                // Coroutine scheduler state -- flush running context before saving
                trx->coroutine_flush_running();
                h.coroutine_head = trx->m_coroutine_head;
                h.running_coroutine = trx->m_running_coroutine;
                h.main_context = trx->m_main_context;
                h.live_coroutine_count = trx->m_live_coroutine_count;
                h.ready_head = trx->m_ready_head;
                h.ready_tail = trx->m_ready_tail;
                h.ready_high_head = trx->m_ready_high_head;
                h.ready_high_tail = trx->m_ready_high_tail;
                h.timer_head = trx->m_timer_head;
                h.next_coroutine_id = trx->m_next_coroutine_id;
                h.coroutine_ops_remaining = trx->m_coroutine_ops_remaining;
                h.default_coroutine_quantum = trx->m_default_coroutine_quantum;
                h.sched_count = trx->m_sched_count;
                h.sched_ready_pops = trx->m_sched_ready_pops;
                h.sched_timer_wakes = trx->m_sched_timer_wakes;
                h.sched_real_sleeps = trx->m_sched_real_sleeps;
                h.ready_queue_depth = trx->m_ready_queue_depth;
                h.ready_high_queue_depth = trx->m_ready_high_queue_depth;
                h.timer_list_depth = trx->m_timer_list_depth;
                h.starvation_threshold_ns = trx->m_starvation_threshold_ns;
                h.coroutine_stack_free = trx->m_coroutine_stack_free;
                h.coroutine_ctx_free = trx->m_coroutine_ctx_free;
                h.binding_bucket_free = trx->m_binding_bucket_free;
                h.binding_entry_free = trx->m_binding_entry_free;
                h.frame_dict_overflow = trx->m_frame_dict_overflow;
                h.next_monitor_ref = trx->m_next_monitor_ref;
                h.current_cell = trx->m_current_cell;
                h.batch_depth = trx->m_batch_depth;
                h.deferred_watchers = trx->m_deferred_watchers;
                h.deferred_watcher_count = trx->m_deferred_watcher_count;
                h.gen_ref_counter = trx->m_gen_ref_counter;

                // Compute CRC-32 over user operator names piecewise: for each entry feed the
                // native-endian uint16_t length then the name bytes into the running CRC.
                // No buffer allocation needed.  thaw recomputes identically from the live table.
                // m_invoke_length/m_invoke_data are not serialized; thaw resets them.
                uint32_t userop_count = 0;
                crc32_t names_crc = 0;
                if (trx->m_useroperators != nullptr) {
                    for (auto p = trx->m_useroperators; p->m_func != nullptr; ++p, ++userop_count) {
                        auto sv = p->m_sv;
                        auto len = static_cast<uint16_t>(sv.size());
                        names_crc = crc32_update(names_crc, &len, sizeof(len));
                        names_crc = crc32_update(names_crc, sv.data(), sv.size());
                    }
                }
                h.useroperator_count = userop_count;
                h.useroperator_names_crc = names_crc;

                // Collect IsMemory ext buffers and the startup file tail; compute their section CRC.
                // File layout: [header] [memory stream blocks] [user file stream blocks] [VM blob]
                {
                    uint32_t mem_count = 0;
                    crc32_t mem_crc = 0;
                    for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                        if (s->is_memory() && (s->m_ext_remaining > 0)) {
                            ++mem_count;
                            auto off = trx->ptr_to_offset(s);
                            mem_crc = crc32_update(mem_crc, &off, sizeof(off));
                            mem_crc = crc32_update(mem_crc, &s->m_ext_remaining, sizeof(s->m_ext_remaining));
                            mem_crc = crc32_update(mem_crc, s->m_ext_ptr, s->m_ext_remaining);
                        }
                    }
                    // Include the startup file tail as an additional memory stream block.  Its
                    // bytes are streamed straight from the startup fd rather than buffered.
                    if ((startup_stream != nullptr) && (startup_tail_size > 0)) {
                        ++mem_count;
                        auto off = trx->ptr_to_offset(startup_stream);
                        size_t rem = startup_tail_size;
                        mem_crc = crc32_update(mem_crc, &off, sizeof(off));
                        mem_crc = crc32_update(mem_crc, &rem, sizeof(rem));
                        mem_crc = crc_fd_range(trx, startup_stream->m_fd, startup_tail_off, rem, mem_crc);
                    }
                    h.memory_stream_count = mem_count;
                    h.memory_stream_crc = (mem_count > 0) ? mem_crc : 0;
                }

                // Compute user file stream section CRC.  Each (absolute) filename is re-derived
                // from its still-open stream via with_resolved_stream_path() -- it is not stored
                // in the record array.
                {
                    crc32_t ufs_crc = 0;
                    for (uint16_t i = 0; i < user_file_count; ++i) {
                        auto *info = &user_file_infos[i];
                        auto *s = trx->offset_to_ptr<Stream>(info->stream_offset);
                        ufs_crc = crc32_update(ufs_crc, &info->stream_offset, sizeof(info->stream_offset));
                        ufs_crc = crc32_update(ufs_crc, &info->file_offset, sizeof(info->file_offset));
                        ufs_crc = crc32_update(ufs_crc, &info->open_mode, sizeof(info->open_mode));
                        ufs_crc = crc32_update(ufs_crc, &info->flags, sizeof(info->flags));
                        ufs_crc = crc_stream_path(trx, s, ufs_crc);
                    }
                    h.user_file_stream_count = user_file_count;
                    h.user_file_stream_crc = (user_file_count > 0) ? ufs_crc : 0;
                }

                // Compute overall CRC-32 over: header (checksum zeroed) + memory blocks
                // + user file stream blocks + VM blob.
                h.checksum = 0;
                {
                    auto crc = crc32_update(0, &h, sizeof(h));
                    for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                        if (s->is_memory() && (s->m_ext_remaining > 0)) {
                            auto off = trx->ptr_to_offset(s);
                            crc = crc32_update(crc, &off, sizeof(off));
                            crc = crc32_update(crc, &s->m_ext_remaining, sizeof(s->m_ext_remaining));
                            crc = crc32_update(crc, s->m_ext_ptr, s->m_ext_remaining);
                        }
                    }
                    if ((startup_stream != nullptr) && (startup_tail_size > 0)) {
                        auto off = trx->ptr_to_offset(startup_stream);
                        size_t rem = startup_tail_size;
                        crc = crc32_update(crc, &off, sizeof(off));
                        crc = crc32_update(crc, &rem, sizeof(rem));
                        crc = crc_fd_range(trx, startup_stream->m_fd, startup_tail_off, rem, crc);
                    }
                    for (uint16_t i = 0; i < user_file_count; ++i) {
                        auto *info = &user_file_infos[i];
                        auto *s = trx->offset_to_ptr<Stream>(info->stream_offset);
                        crc = crc32_update(crc, &info->stream_offset, sizeof(info->stream_offset));
                        crc = crc32_update(crc, &info->file_offset, sizeof(info->file_offset));
                        crc = crc32_update(crc, &info->open_mode, sizeof(info->open_mode));
                        crc = crc32_update(crc, &info->flags, sizeof(info->flags));
                        crc = crc_stream_path(trx, s, crc);
                    }
                    crc = crc32_update(crc, trx->m_vm_base, h.vm_used);
                    // Global region (if any): bytes from m_vm_global to m_vm_limit.
                    if (h.vm_global_used > 0) {
                        crc = crc32_update(crc, trx->m_vm_global, h.vm_global_used);
                    }
                    h.checksum = crc;
                }

                // Write: 1. Header
                if (!write_all(fd, &h, sizeof(h))) {
                    auto errno_str = errno_string();
                    trx->error(Error::IOWriteError, "snap-shot: header write failed: {}/{}", errno, errno_str);
                } else {
                    // Write: 2. Memory stream blocks (IsMemory ext buffers + startup file tail)
                    for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                        if (s->is_memory() && (s->m_ext_remaining > 0)) {
                            auto off = trx->ptr_to_offset(s);
                            if (!write_all(fd, &off, sizeof(off)) ||
                                !write_all(fd, &s->m_ext_remaining, sizeof(s->m_ext_remaining)) ||
                                !write_all(fd, s->m_ext_ptr, s->m_ext_remaining)) {
                                auto errno_str = errno_string();
                                trx->error(Error::IOWriteError,
                                           "snap-shot: memory stream block write failed: {}/{}",
                                           errno,
                                           errno_str);
                            }
                        }
                    }
                    if ((startup_stream != nullptr) && (startup_tail_size > 0)) {
                        auto off = trx->ptr_to_offset(startup_stream);
                        size_t rem = startup_tail_size;
                        if (!write_all(fd, &off, sizeof(off)) || !write_all(fd, &rem, sizeof(rem))) {
                            auto errno_str = errno_string();
                            trx->error(Error::IOWriteError, "snap-shot: startup tail block write failed: {}/{}", errno, errno_str);
                        } else {
                            // Stream the tail bytes straight from the startup fd (never buffered).
                            write_fd_range(trx, startup_stream->m_fd, startup_tail_off, rem, fd);
                        }
                    }

                    // Write: 3. User file stream blocks (re-derive each filename from its stream)
                    for (uint16_t i = 0; i < user_file_count; ++i) {
                        auto *info = &user_file_infos[i];
                        auto *s = trx->offset_to_ptr<Stream>(info->stream_offset);
                        if (!write_all(fd, &info->stream_offset, sizeof(info->stream_offset)) ||
                            !write_all(fd, &info->file_offset, sizeof(info->file_offset)) ||
                            !write_all(fd, &info->open_mode, sizeof(info->open_mode)) ||
                            !write_all(fd, &info->flags, sizeof(info->flags))) {
                            auto errno_str = errno_string();
                            trx->error(
                                    Error::IOWriteError, "snap-shot: user file stream block write failed: {}/{}", errno, errno_str);
                        }
                        write_stream_path(trx, s, fd);
                    }

                    // Write: 4. Local VM blob
                    if (!write_all(fd, trx->m_vm_base, h.vm_used)) {
                        auto errno_str = errno_string();
                        trx->error(Error::IOWriteError, "snap-shot: VM data write failed: {}/{}", errno, errno_str);
                    } else {
                        // Write: 5. Global VM blob (placed at offset h.vm_capacity - h.vm_global_used in
                        // the original layout; thaw requires capacity match when this is non-zero).
                        if (h.vm_global_used > 0) {
                            if (!write_all(fd, trx->m_vm_global, h.vm_global_used)) {
                                auto errno_str = errno_string();
                                trx->error(Error::IOWriteError, "snap-shot: global VM data write failed: {}/{}", errno, errno_str);
                            }
                        }

                        ::close(fd);
                        fd = -1;  // prevent guard from double-closing on rename failure

                        // Atomically replace the target file with the completed image.
                        if (::rename(tmp, target) == -1) {
                            auto errno_str = errno_string();
                            trx->error(Error::IOWriteError,
                                       "snap-shot: cannot rename '{}' to '{}': {}/{}",
                                       tmp,
                                       target,
                                       errno,
                                       errno_str);
                        } else {
                            guard.dismiss();
                        }
                    }
                }
            }
        }
    }
}

// Fix up one Stream's raw pointer fields after thaw: null the stale ext pointers,
// then restore IsMemory / startup-file ext buffers from `restored`, dup /dev/null for
// the startup stream, or reconnect a user-file stream from `reconnected` (marking its
// slot in `matched_reconnected`).  A named pointer-parameter function rather than a
// [&]-capturing lambda inside apply_fixup_streams.
static void fixup_one_stream(Trix *trx,
                             Stream *s,
                             int nulldev_fd,
                             std::span<const RestoredMemStream> restored,
                             std::span<const ReconnectedStream> reconnected,
                             bool *matched_reconnected) {
    // m_source is an Object (vm_offset_t) -- correct after thaw.
    // m_next_offset, m_base_offset, m_rlimit_offset, m_rptr_offset,
    // m_wlimit_offset, m_wptr_offset are all vm_offset_t -- no rebasing needed.
    //
    // m_ext_base/m_ext_ptr are raw pointers to a malloc'd buffer from the
    // snap-shot process. That allocation is gone; null them out unconditionally.
    // IsMemory streams are restored from the serialized blocks rather than closed.
    s->m_ext_base = nullptr;
    s->m_ext_ptr = nullptr;
    s->m_ext_remaining = 0;
    if ((s->m_status & Stream::IsMemory) != 0) {
        // Restore ext fields from the serialized block if m_ext_remaining > 0
        // at snap-shot time; otherwise leave nulled (stream drains VM buffer).
        auto stream_off = trx->ptr_to_offset(s);
        for (size_t k = 0; k < restored.size(); ++k) {
            auto *r = &restored[k];
            if (r->offset == stream_off) {
                s->m_ext_base = r->buf;
                s->m_ext_ptr = r->buf;
                s->m_ext_remaining = r->remaining;
                // The restored block is freshly malloc'd by the thaw path
                // (regardless of whether the source stream borrowed its
                // pre-snapshot pointer or owned it).  Clear IsBorrowed so
                // close() / free_ext_base() correctly std::free's the
                // restored buffer instead of leaking it.
                s->m_status = static_cast<Stream::status_t>(s->m_status & ~Stream::IsBorrowed);
                break;
            }
        }
        // Leave stream open -- reads from VM buffer and/or m_ext_base.
    } else if (!s->is_stdio()) {
        if ((s->m_status & Stream::IsStartupFile) != 0) {
            // Startup file stream: its buffer may still hold unexecuted
            // script content (the exec stack resumes from it after thaw).
            // Replace the stale fd with a dup of /dev/null so the scanner
            // gets clean EOF once the ext buffer and VM buffer are drained.
            s->m_fd = (nulldev_fd != -1) ? ::dup(nulldev_fd) : -1;
            if (s->m_fd == -1) {
                s->m_status = Stream::IsClosed;  // dup failed; fallback
            }
            // Restore the startup file tail (content beyond the 4KB buffer)
            // from the serialized memory stream block, if present.  After the
            // VM buffer is exhausted, fill() drains this ext buffer, then reads
            // EOF from the /dev/null fd.
            auto stream_off = trx->ptr_to_offset(s);
            for (size_t k = 0; k < restored.size(); ++k) {
                auto *r = &restored[k];
                if (r->offset == stream_off) {
                    s->m_ext_base = r->buf;
                    s->m_ext_ptr = r->buf;
                    s->m_ext_remaining = r->remaining;
                    break;
                }
            }
        } else {
            // User file stream: reconnect if a pre-opened fd was provided (thaw_op),
            // otherwise mark closed (startup_image, or no reconnect info available).
            // Linear scan over reconnected is O(M-N) where N = reconnected count.
            // N is always tiny (number of open user file streams at snap-shot time),
            // so no hash lookup structure is needed here.
            auto stream_off = trx->ptr_to_offset(s);
            bool found = false;
            for (size_t j = 0; j < reconnected.size(); ++j) {
                if (reconnected[j].stream_offset == stream_off) {
                    s->m_fd = reconnected[j].fd;  // buffer already reset by snapshot_op; stream stays open
                    matched_reconnected[j] = true;
                    found = true;
                    break;
                }
            }
            if (!found) {
                s->m_fd = -1;
                s->m_status = Stream::IsClosed;
            }
        }
    }
}

static void
apply_fixup_streams(Trix *trx, std::span<const RestoredMemStream> restored, std::span<const ReconnectedStream> reconnected) {
    int nulldev_fd = ::open("/dev/null", O_RDONLY);

    // Track which reconnected entries were matched to a live stream, so unmatched
    // fds can be closed as a defense-in-depth measure.  Indexed by position in
    // `reconnected`, which is bounded by MaxStreamCount.
    bool matched_reconnected[MaxStreamCount] = {};

    // StdIO streams (stdin/stdedit/stdout/stderr) are standalone vm_alloc_ptr
    // allocations -- they are NOT in the inuse or free lists.  Fixup them first
    // so that reattach_stdio finds a correctly set m_trx.
    if (trx->m_stdin != nullptr) {
        fixup_one_stream(trx, trx->m_stdin, nulldev_fd, restored, reconnected, matched_reconnected);
    }
    if (trx->m_stdedit != nullptr) {
        fixup_one_stream(trx, trx->m_stdedit, nulldev_fd, restored, reconnected, matched_reconnected);
    }
    if (trx->m_stdout != nullptr) {
        fixup_one_stream(trx, trx->m_stdout, nulldev_fd, restored, reconnected, matched_reconnected);
    }
    if (trx->m_stderr != nullptr) {
        fixup_one_stream(trx, trx->m_stderr, nulldev_fd, restored, reconnected, matched_reconnected);
    }

    for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
        fixup_one_stream(trx, s, nulldev_fd, restored, reconnected, matched_reconnected);
    }
    for (auto s = trx->m_stream_free_list; s != nullptr; s = s->next_stream(trx)) {
        fixup_one_stream(trx, s, nulldev_fd, restored, reconnected, matched_reconnected);
    }

    // Defense-in-depth: close any reconnected fds that were not matched to a live
    // stream. Should be unreachable if the CRC check passed, but prevents fd leaks
    // on logic errors or corrupt-but-CRC-passing images.
    for (size_t j = 0; j < reconnected.size(); ++j) {
        if (!matched_reconnected[j]) {
            ::close(reconnected[j].fd);
        }
    }

    if (nulldev_fd != -1) {
        ::close(nulldev_fd);
    }
}

static void restore_from_header(Trix *trx, const SnapShotHeader *h) {
    // Restore scalar instance variables.
    trx->m_max_save_level = h->max_save_level;
    trx->m_curr_save_level = h->curr_save_level;
    trx->m_name_bucket_count = h->name_bucket_count;
    trx->m_name_bucket_magic = fastmod_magic_u32(h->name_bucket_count);
    trx->m_next_sid = h->next_sid;
    trx->m_sid_wrapped = h->sid_wrapped;
    trx->m_stream_buffer_size = h->stream_buffer_size;
    trx->m_eqproc_length = h->eqproc_length;
    trx->m_eqproc_stored_length = h->eqproc_stored_length;
    trx->m_eqdict_maxlength = h->eqdict_maxlength;
    trx->m_eqset_maxlength = h->eqset_maxlength;
    trx->m_eqstring_generation = h->eqstring_generation;
    trx->m_eqarray_generation = h->eqarray_generation;
    trx->m_eqproc_generation = h->eqproc_generation;
    trx->m_eqdict_generation = h->eqdict_generation;
    trx->m_eqset_generation = h->eqset_generation;
    trx->m_last_error = h->last_error;
    trx->m_external_error = h->external_error;
    trx->m_interrupt = h->interrupt;
    trx->m_interrupt_mask = h->interrupt_mask;

    trx->m_root_objects_ptr = trx->offset_to_nullptr<Object>(h->root_objects_offset);

    trx->m_pcg32.thaw_state(h->pcg32_state, h->pcg32_inc);

    // Restore all C++ pointer fields from header offsets.
    trx->m_vm_ptr = trx->offset_to_nullptr<vm_t>(h->vm_ptr_offset);
    trx->m_vm_temp_ptr = trx->m_vm_limit;                    // no temps active after thaw
    trx->m_vm_global = trx->m_vm_limit - h->vm_global_used;  // restore global watermark from snapshot
    trx->m_curr_alloc_global = h->curr_alloc_global;
    trx->m_gc_current_gen = h->gc_current_gen;
    trx->m_gvm_free_head = h->gvm_free_head;  // free-list head survives thaw
    for (length_t i = 0; i < GvmFastBinCount; ++i) {
        trx->m_gvm_fastbins[i] = h->gvm_fastbins[i];  // fastbin heads survive thaw
    }
    trx->m_gc_scratch_offset = h->gc_scratch_offset;  // lazy GC scratch survives thaw
    trx->m_gvm_user_block_count = h->gvm_user_block_count;
    trx->m_gvm_free_block_count = h->gvm_free_block_count;
    // Local-visited pointer is derived from m_gc_scratch_offset on every GC
    // entry; reset to nullptr here.  Thaw doesn't run GC, so an idle pointer
    // is correct.
    trx->m_gc_local_visited = nullptr;
    trx->m_gc_local_visited_count = 0;
    trx->m_gc_local_visited_capacity = 0;
    trx->m_vm_alloc_active = trx->offset_to_nullptr<vm_t>(h->vm_alloc_active_offset);
    trx->m_save_stack = trx->offset_to_nullptr<vm_offset_t>(h->save_stack_offset);
    trx->m_save_generation = trx->offset_to_nullptr<save_generation_t>(h->save_generation_offset);
    trx->m_extvalue_free_list = trx->offset_to_nullptr<vm_offset_t>(h->extvalue_free_list_offset);
    trx->m_widevalue_free_list = trx->offset_to_nullptr<vm_offset_t>(h->widevalue_free_list_offset);
    trx->m_vm_temp_save = trx->offset_to_nullptr<vm_t *>(h->vm_temp_save_offset);
    // temp save array contains raw pointers from the snapshotted process -- stale after thaw;
    // since no temps can be active at snapshot time, reinitialize all entries to m_vm_limit
    std::fill_n(trx->m_vm_temp_save, trx->m_max_save_level, trx->m_vm_limit);
    trx->m_extvalue_active_save = trx->offset_to_nullptr<integer_t>(h->extvalue_active_save_offset);
    trx->m_widevalue_active_save = trx->offset_to_nullptr<integer_t>(h->widevalue_active_save_offset);
    // reset per-level snapshots; m_extvalue_active_count / m_widevalue_active_count are reset below
    std::fill_n(trx->m_extvalue_active_save, trx->m_max_save_level, integer_t{0});
    std::fill_n(trx->m_widevalue_active_save, trx->m_max_save_level, integer_t{0});
    trx->m_save_journal_count = trx->offset_to_nullptr<length_t>(h->save_journal_count_offset);
    std::fill_n(trx->m_save_journal_count, trx->m_max_save_level, length_t{0});
    trx->m_save_journal_bytes = trx->offset_to_nullptr<vm_size_t>(h->save_journal_bytes_offset);
    std::fill_n(trx->m_save_journal_bytes, trx->m_max_save_level, vm_size_t{0});
    trx->m_dict_pool = trx->offset_to_nullptr<vm_offset_t>(h->dict_pool_offset);
    trx->m_frame_dict_pool = trx->offset_to_nullptr<vm_offset_t>(h->frame_dict_pool_offset);
    trx->m_mailbox_pool = trx->offset_to_nullptr<vm_offset_t>(h->mailbox_pool_offset);
    trx->m_monitor_pool = h->monitor_pool_offset;  // bare offset, not a pointer to an array
    trx->m_pipe_pool = trx->offset_to_nullptr<vm_offset_t>(h->pipe_pool_offset);
    trx->m_op_base = trx->offset_to_nullptr<Object>(h->op_base_offset);
    trx->m_op_ptr = trx->offset_to_nullptr<Object>(h->op_ptr_offset);
    trx->m_op_limit = trx->offset_to_nullptr<Object>(h->op_limit_offset);
    trx->m_op_push_limit = (trx->m_op_limit - 1);
    trx->m_dict_base = trx->offset_to_nullptr<Object>(h->dict_base_offset);
    trx->m_dict_ptr = trx->offset_to_nullptr<Object>(h->dict_ptr_offset);
    trx->m_dict_limit = trx->offset_to_nullptr<Object>(h->dict_limit_offset);
    trx->m_exec_base = trx->offset_to_nullptr<Object>(h->exec_base_offset);
    trx->m_exec_ptr = trx->offset_to_nullptr<Object>(h->exec_ptr_offset);
    trx->m_exec_limit = trx->offset_to_nullptr<Object>(h->exec_limit_offset);
    trx->m_err_base = trx->offset_to_nullptr<Object>(h->err_base_offset);
    trx->m_err_ptr = trx->offset_to_nullptr<Object>(h->err_ptr_offset);
    trx->m_err_limit = trx->offset_to_nullptr<Object>(h->err_limit_offset);
    // Scratch arena pointers.  m_scratch_depth is recovered
    // from (limit - base) so a thawed image preserves its original
    // sizing even if the new Trix instance was constructed with a
    // different --scratch-depth value.
    trx->m_scratch_base = trx->offset_to_nullptr<Object>(h->scratch_base_offset);
    trx->m_scratch_ptr = trx->offset_to_nullptr<Object>(h->scratch_ptr_offset);
    trx->m_scratch_limit = trx->offset_to_nullptr<Object>(h->scratch_limit_offset);
    trx->m_scratch_depth = static_cast<stack_depth_t>(trx->m_scratch_limit - trx->m_scratch_base);
    // GcScope root stack: always empty at a snapshot boundary, so only the base
    // offset is saved; reconstruct limit (fixed depth) + ptr (empty = base - 1).
    trx->m_gc_roots_base = trx->offset_to_nullptr<Object>(h->gc_roots_base_offset);
    trx->m_gc_roots_limit = (trx->m_gc_roots_base + MaxGcRootDepth);
    trx->m_gc_roots_ptr = (trx->m_gc_roots_base - 1);
    trx->m_error_string_base = trx->offset_to_nullptr<vm_t>(h->error_string_base_offset);
    trx->m_error_string_limit = trx->offset_to_nullptr<vm_t>(h->error_string_limit_offset);
    trx->m_token_base = trx->offset_to_nullptr<vm_t>(h->token_base_offset);
    trx->m_token_limit = trx->offset_to_nullptr<vm_t>(h->token_limit_offset);
    trx->m_name_buckets = trx->offset_to_nullptr<vm_offset_t>(h->name_buckets_offset);
    trx->m_systemname_offsets = trx->offset_to_nullptr<vm_offset_t>(h->systemname_offsets_offset);
    trx->m_typename_offsets = trx->offset_to_nullptr<vm_offset_t>(h->typename_offsets_offset);
    trx->m_errorname_offsets = trx->offset_to_nullptr<vm_offset_t>(h->errorname_offsets_offset);
    trx->m_wellknown_offsets = trx->offset_to_nullptr<vm_offset_t>(h->wellknown_offsets_offset);
    trx->populate_wellknown_cache();
    trx->m_systemdict = trx->offset_to_nullptr<Dict>(h->systemdict_offset);
    trx->m_protocoldict = trx->offset_to_nullptr<Dict>(h->protocoldict_offset);
    trx->m_userdict = trx->offset_to_nullptr<Dict>(h->userdict_offset);
    trx->m_errordict = trx->offset_to_nullptr<Dict>(h->errordict_offset);
    trx->m_handlersdict = trx->offset_to_nullptr<Dict>(h->handlersdict_offset);
    trx->m_eqproc_storage_ptr = trx->offset_to_nullptr<Object>(h->eqproc_storage_offset);
    trx->m_eqdict = trx->offset_to_nullptr<Dict>(h->eqdict_offset);
    trx->m_eqset = trx->offset_to_nullptr<Dict>(h->eqset_offset);
    trx->m_stream_inuse_list = trx->offset_to_nullptr<Stream>(h->stream_inuse_list_offset);
    trx->m_stream_free_list = trx->offset_to_nullptr<Stream>(h->stream_free_list_offset);
    trx->m_stdin = trx->offset_to_nullptr<Stream>(h->stdin_offset);
    trx->m_stdedit = trx->offset_to_nullptr<Stream>(h->stdedit_offset);
    trx->m_stdout = trx->offset_to_nullptr<Stream>(h->stdout_offset);
    trx->m_stderr = trx->offset_to_nullptr<Stream>(h->stderr_offset);
    trx->m_last_error_name_ptr = trx->offset_to_nullptr<Object>(h->last_error_name_offset);
    trx->m_last_operator_ptr = trx->offset_to_nullptr<Object>(h->last_operator_offset);
    trx->m_last_error_msg_ptr = trx->offset_to_nullptr<Object>(h->last_error_msg_offset);
    trx->m_last_error_data_ptr = trx->offset_to_nullptr<Object>(h->last_error_data_offset);
    trx->m_ostack_ptr = trx->offset_to_nullptr<Object>(h->ostack_offset);
    trx->m_dstack_ptr = trx->offset_to_nullptr<Object>(h->dstack_offset);
    trx->m_estack_ptr = trx->offset_to_nullptr<Object>(h->estack_offset);
    trx->m_require_dict_offset = h->require_dict_offset;
    trx->m_modules_dict_offset = h->modules_dict_offset;
    trx->m_protocol_registry_offset = h->protocol_registry_offset;

    // Coroutine scheduler state
    trx->m_coroutine_head = h->coroutine_head;
    trx->m_running_coroutine = h->running_coroutine;
    trx->m_main_context = h->main_context;
    trx->m_live_coroutine_count = h->live_coroutine_count;
    trx->m_ready_head = h->ready_head;
    trx->m_ready_tail = h->ready_tail;
    trx->m_ready_high_head = h->ready_high_head;
    trx->m_ready_high_tail = h->ready_high_tail;
    trx->m_timer_head = h->timer_head;
    trx->m_next_coroutine_id = h->next_coroutine_id;
    trx->m_coroutine_ops_remaining = h->coroutine_ops_remaining;
    trx->m_default_coroutine_quantum = h->default_coroutine_quantum;
    trx->m_sched_count = h->sched_count;
    trx->m_sched_ready_pops = h->sched_ready_pops;
    trx->m_sched_timer_wakes = h->sched_timer_wakes;
    trx->m_sched_real_sleeps = h->sched_real_sleeps;
    trx->m_ready_queue_depth = h->ready_queue_depth;
    trx->m_ready_high_queue_depth = h->ready_high_queue_depth;
    trx->m_timer_list_depth = h->timer_list_depth;
    trx->m_starvation_threshold_ns = h->starvation_threshold_ns;
    trx->m_coroutine_stack_free = h->coroutine_stack_free;
    trx->m_coroutine_ctx_free = h->coroutine_ctx_free;
    trx->m_binding_bucket_free = h->binding_bucket_free;
    trx->m_binding_entry_free = h->binding_entry_free;
    trx->m_frame_dict_overflow = h->frame_dict_overflow;
    trx->m_next_monitor_ref = h->next_monitor_ref;
    trx->m_current_cell = h->current_cell;
    trx->m_batch_depth = h->batch_depth;
    trx->m_deferred_watchers = h->deferred_watchers;
    trx->m_deferred_watcher_count = h->deferred_watcher_count;
    trx->m_gen_ref_counter = h->gen_ref_counter;

    // Reset transient interpreter state that does not belong in the snap-shot image.
    trx->m_scanner_stream = nullptr;
    trx->m_invoke_data = nullptr;
    trx->m_invoke_length = 0;
    trx->m_in_global_handler = false;
    trx->m_op_count = 0;
    trx->m_tco_count = 0;
    trx->m_op_high_water_ptr = trx->m_op_ptr;
    trx->m_exec_high_water_ptr = trx->m_exec_ptr;
    trx->m_dict_high_water_ptr = trx->m_dict_ptr;
    trx->m_err_high_water_ptr = trx->m_err_ptr;
    trx->m_extvalue_active_count = 0;   // reset; tracks allocations since thaw
    trx->m_widevalue_active_count = 0;  // same policy as extvalue
// Debugger state reset -- gated out in release builds (TRIX_DEBUGGER
// undefined).  When the debugger isn't compiled in there's no debug
// state to reset; the whole block folds away.
#ifdef TRIX_DEBUGGER
    trx->m_debug = DebugState{};
    trx->m_debug_active = false;
#endif
    // sync fast-path atomic with the restored m_interrupt so pending IRQs are not lost
    trx->m_pending_irq.store(trx->m_interrupt, std::memory_order_relaxed);

    // invalidate entire dict pool -- pooled dicts from the pre-thaw session are stale
    std::fill_n(trx->m_dict_pool, trx->m_max_save_level * MaxDictPoolSize, nulloffset);
    std::fill_n(trx->m_frame_dict_pool, MaxDictPoolSize, nulloffset);
    trx->m_frame_dict_overflow = nulloffset;

    // Null out m_scanner_stream in all coroutine contexts.  These are raw Stream*
    // pointers from the original process address space and are dangling after thaw.
    // Safe: the scanner will be re-entered from the execution stack on next dispatch.
    if (trx->m_coroutine_head != nulloffset) {
        auto cursor = trx->m_coroutine_head;
        do {
            auto ctx = trx->offset_to_ptr<CoroutineContext>(cursor);
            ctx->m_scanner_stream = nullptr;
            cursor = ctx->m_next;
        } while (cursor != trx->m_coroutine_head);
    }
}

[[nodiscard]] static bool startup_image(Trix *trx, const char *image_filename) {
    auto fd = ::open64(image_filename, O_RDONLY);
    if (fd == -1) {
        trx->diag_println("thaw: cannot open '{}': {}/{}", image_filename, errno, errno_string());
        return false;
    } else {
        // Fixed-capacity record array.  Each .buf is still std::malloc'd -- ownership
        // transfers to a Stream in apply_fixup_streams; on failure the Guard frees them.
        // memory_stream_count counts the startup-file tail among the inuse streams, so it
        // is bounded by MaxStreamCount (no +1 needed).
        RestoredMemStream restored_mem_streams[MaxStreamCount];
        uint16_t restored_count = 0;

        // RAII guard: on any early return, closes fd (unless already -1) and frees
        // any malloc'd memory-stream buffers accumulated so far.
        struct Guard {
            Guard(int *f, RestoredMemStream *m, uint16_t *n) : m_fd(f), m_mem(m), m_count(n), m_dismissed(false) {}

            ~Guard() {
                if (!m_dismissed) {
                    if (*m_fd >= 0) {
                        ::close(*m_fd);
                    }
                    for (uint16_t i = 0; i < *m_count; ++i) {
                        std::free(m_mem[i].buf);
                    }
                }
            }

            void dismiss() { m_dismissed = true; }

            int *m_fd;
            RestoredMemStream *m_mem;
            uint16_t *m_count;
            bool m_dismissed;
        } guard(&fd, restored_mem_streams, &restored_count);

        SnapShotHeader h{};
        if (!read_all(fd, &h, sizeof(h))) {
            trx->diag_println("thaw: header read failed: {}/{}", errno, errno_string());
            return false;
        } else {
            // Save the stored checksum and zero the field so the header bytes fed into the
            // CRC computation below match what snapshot_op hashed (header with checksum == 0).
            auto saved_crc = h.checksum;
            h.checksum = 0;

            // Validate magic
            constexpr uint8_t MAGIC[4] = {'T', 'R', 'I', 'X'};
            if ((std::memcmp(h.magic, MAGIC, 4) != 0) || (h.snapshot_version != SNAPSHOT_VERSION)) {
                trx->diag_println("thaw: invalid snap-shot file (bad magic or version)");
                return false;
            } else if (h.operator_table_signature != compute_operator_table_signature()) {
                trx->diag_println("thaw: image built by an incompatible Trix (image has {} operators, this binary has {})",
                                  static_cast<integer_t>(h.operator_count),
                                  static_cast<integer_t>(SYSOPERATOR_COUNT));
                return false;
            } else {
                // Validate endianness
                auto expected_le = (std::endian::native == std::endian::little) ? uint8_t{1} : uint8_t{0};
                if (h.endian_le != expected_le) {
                    trx->diag_println("thaw: endian mismatch (file={}, host={})", h.endian_le, expected_le);
                    return false;
                } else {
                    // Validate VM capacity.  Local data must fit; if there's a global region in the
                    // image, capacity must match exactly so the global block lands at its original
                    // offset (offsets stored in the global region are not rebased on thaw).
                    auto vm_capacity = static_cast<uint32_t>(trx->m_vm_limit - trx->m_vm_base);
                    if (h.vm_used > vm_capacity) {
                        trx->diag_println("thaw: VM too small (need {} bytes, have {})", h.vm_used, vm_capacity);
                        return false;
                    } else if ((h.vm_global_used > 0) && (h.vm_capacity != vm_capacity)) {
                        trx->diag_println("thaw: VM capacity mismatch with global region present (file capacity={}, host "
                                          "capacity={}); global "
                                          "offsets are not rebased",
                                          h.vm_capacity,
                                          vm_capacity);
                        return false;
                    } else if (h.vm_global_used > (vm_capacity - h.vm_used)) {
                        trx->diag_println(
                                "thaw: local + global ({} + {}) exceeds VM capacity {}", h.vm_used, h.vm_global_used, vm_capacity);
                        return false;
                    } else {
                        // Load the frozen VM image.  No pre-thaw stream close needed -- the VM is
                        // uninitialized at startup so there are no open user file streams to release.
                        // File layout: [header] [memory stream blocks] [user file stream blocks] [VM blob]

                        // Bound the count to the fixed array (a corrupt image could claim a huge count).
                        if (h.memory_stream_count > static_cast<uint32_t>(MaxStreamCount)) {
                            trx->diag_println(
                                    "thaw: memory stream count {} exceeds maximum {}", h.memory_stream_count, MaxStreamCount);
                            return false;
                        } else {
                            // Compute the overall CRC incrementally as each section is read (header, then
                            // memory blocks, user-file blocks, VM blob -- the on-disk order), so the
                            // user-file blocks need not be retained after the VM blob is loaded.
                            auto crc = crc32_update(0, &h, sizeof(h));

                            // Read memory stream blocks (between header and user file stream blocks).
                            crc32_t computed_mem_crc = 0;
                            for (uint32_t i = 0; i < h.memory_stream_count; ++i) {
                                vm_offset_t off = 0;
                                size_t rem = 0;
                                if (!read_all(fd, &off, sizeof(off)) || !read_all(fd, &rem, sizeof(rem))) {
                                    trx->diag_println("thaw: memory stream block {} header read failed", i);
                                    return false;
                                } else if (rem == 0) {
                                    trx->diag_println("thaw: memory stream block {} has zero size (corrupt file)", i);
                                    return false;
                                } else {
                                    auto buf = static_cast<vm_t *>(std::malloc(rem));
                                    if (buf == nullptr) {
                                        trx->diag_println("thaw: malloc failed for memory stream block {} ({} bytes)", i, rem);
                                        return false;
                                    } else if (!read_all(fd, buf, rem)) {
                                        trx->diag_println("thaw: memory stream block {} data read failed", i);
                                        std::free(buf);
                                        return false;
                                    } else {
                                        computed_mem_crc = crc32_update(computed_mem_crc, &off, sizeof(off));
                                        computed_mem_crc = crc32_update(computed_mem_crc, &rem, sizeof(rem));
                                        computed_mem_crc = crc32_update(computed_mem_crc, buf, rem);
                                        crc = crc32_update(crc, &off, sizeof(off));
                                        crc = crc32_update(crc, &rem, sizeof(rem));
                                        crc = crc32_update(crc, buf, rem);
                                        restored_mem_streams[restored_count++] = {off, buf, rem};
                                    }
                                }
                            }

                            // Read user file stream blocks for CRC verification only -- constructor thaw
                            // does not reopen them.  Stream the filename bytes through the CRCs without
                            // retaining them.
                            crc32_t computed_ufs_crc = 0;
                            for (uint32_t i = 0; i < h.user_file_stream_count; ++i) {
                                vm_offset_t stream_offset = 0;
                                int64_t file_offset = 0;
                                uint8_t open_mode = 0;
                                uint8_t flags = 0;
                                uint16_t fnlen = 0;
                                if (!read_all(fd, &stream_offset, sizeof(stream_offset)) ||
                                    !read_all(fd, &file_offset, sizeof(file_offset)) ||
                                    !read_all(fd, &open_mode, sizeof(open_mode)) || !read_all(fd, &flags, sizeof(flags)) ||
                                    !read_all(fd, &fnlen, sizeof(fnlen))) {
                                    trx->diag_println("thaw: user file stream block {} header read failed", i);
                                    return false;
                                }
                                computed_ufs_crc = crc32_update(computed_ufs_crc, &stream_offset, sizeof(stream_offset));
                                computed_ufs_crc = crc32_update(computed_ufs_crc, &file_offset, sizeof(file_offset));
                                computed_ufs_crc = crc32_update(computed_ufs_crc, &open_mode, sizeof(open_mode));
                                computed_ufs_crc = crc32_update(computed_ufs_crc, &flags, sizeof(flags));
                                computed_ufs_crc = crc32_update(computed_ufs_crc, &fnlen, sizeof(fnlen));
                                crc = crc32_update(crc, &stream_offset, sizeof(stream_offset));
                                crc = crc32_update(crc, &file_offset, sizeof(file_offset));
                                crc = crc32_update(crc, &open_mode, sizeof(open_mode));
                                crc = crc32_update(crc, &flags, sizeof(flags));
                                crc = crc32_update(crc, &fnlen, sizeof(fnlen));
                                // Stream the filename bytes through both CRCs in fixed chunks (no retention).
                                uint16_t fn_remaining = fnlen;
                                while (fn_remaining > 0) {
                                    uint8_t chunk[SnapshotIoBufSize];
                                    auto n = std::min<uint16_t>(fn_remaining, static_cast<uint16_t>(sizeof(chunk)));
                                    if (!read_all(fd, chunk, n)) {
                                        trx->diag_println("thaw: user file stream block {} filename read failed", i);
                                        return false;
                                    } else {
                                        computed_ufs_crc = crc32_update(computed_ufs_crc, chunk, n);
                                        crc = crc32_update(crc, chunk, n);
                                        fn_remaining = static_cast<uint16_t>(fn_remaining - n);
                                    }
                                }
                            }

                            if (!read_all(fd, trx->m_vm_base, h.vm_used)) {
                                trx->diag_println("thaw: VM data read failed: {}/{}", errno, errno_string());
                                return false;
                            } else {
                                crc = crc32_update(crc, trx->m_vm_base, h.vm_used);

                                // Read the global VM blob (if any) into its original position [m_vm_limit - vm_global_used,
                                // m_vm_limit).
                                if (h.vm_global_used > 0) {
                                    auto global_dst = trx->m_vm_limit - h.vm_global_used;
                                    if (!read_all(fd, global_dst, h.vm_global_used)) {
                                        trx->diag_println("thaw: global VM data read failed: {}/{}", errno, errno_string());
                                        return false;
                                    } else {
                                        crc = crc32_update(crc, global_dst, h.vm_global_used);
                                    }
                                }

                                ::close(fd);
                                fd = -1;  // prevent guard from double-closing

                                // Verify integrity before restoring any state.  `crc` was accumulated in file
                                // order across the reads above.
                                // m_useroperators is NOT restored from the snap-shot file; the live table
                                // (set by init_and_interpret before calling startup_image) is validated
                                // against h.useroperator_names_crc and then continues to be used after thaw.
                                // m_invoke_length/m_invoke_data are reset to 0/nullptr regardless of snap-shot state.
                                if (crc != saved_crc) {
                                    trx->diag_println("thaw: checksum mismatch (file={:#010x}, computed={:#010x})", saved_crc, crc);
                                    return false;
                                } else {
                                    // Defense-in-depth: verify memory stream CRC separately.
                                    if ((h.memory_stream_count > 0) && (computed_mem_crc != h.memory_stream_crc)) {
                                        trx->diag_println("thaw: memory stream checksum mismatch");
                                        return false;
                                    } else {
                                        // Defense-in-depth: verify user file stream CRC separately.
                                        if ((h.user_file_stream_count > 0) && (computed_ufs_crc != h.user_file_stream_crc)) {
                                            trx->diag_println("thaw: user file stream checksum mismatch");
                                            return false;
                                        } else {
                                            // Verify the VM-base sentinel ('TRIX' at offset 0).  Every live VM
                                            // heap produced by init.inl writes this cookie as a null-sentinel
                                            // guard.  A mismatch means the image is stale (pre-129) or was
                                            // trimmed/replaced in a way CRC somehow missed.
                                            {
                                                uint32_t sentinel = 0;
                                                std::memcpy(&sentinel, trx->m_vm_base, sizeof(sentinel));
                                                if (sentinel != TRIX_SENTINEL) {
                                                    trx->diag_println("thaw: VM-base sentinel mismatch (expected 'TRIX')");
                                                    return false;
                                                }
                                            }

                                            // Validate that the live useroperator table matches what was frozen.
                                            // Feed each name's uint16_t length then its bytes into a running CRC, identical
                                            // to snapshot_op -- no buffer allocation needed.  A single diagnostic covers
                                            // both count and name mismatches.
                                            {
                                                uint32_t live_count = 0;
                                                crc32_t live_crc = 0;
                                                if (trx->m_useroperators != nullptr) {
                                                    for (auto p = trx->m_useroperators; p->m_func != nullptr; ++p, ++live_count) {
                                                        auto sv = p->m_sv;
                                                        auto len = static_cast<uint16_t>(sv.size());
                                                        live_crc = crc32_update(live_crc, &len, sizeof(len));
                                                        live_crc = crc32_update(live_crc, sv.data(), sv.size());
                                                    }
                                                }
                                                if ((live_count != h.useroperator_count) ||
                                                    (live_crc != h.useroperator_names_crc)) {
                                                    trx->diag_println("thaw: incompatible useroperator table; re-create snap-shot");
                                                    return false;
                                                }
                                            }

                                            // Restore all scalar/pointer fields and reset transient state.
                                            restore_from_header(trx, &h);

                                            // Fix up raw pointer fields in each Stream struct after thaw.
                                            // Constructor thaw does not reopen user file streams; pass an empty reconnected
                                            // span.
                                            apply_fixup_streams(
                                                    trx,
                                                    std::span<const RestoredMemStream>(restored_mem_streams, restored_count),
                                                    {});

                                            // Re-initialise the stdio Stream objects so they point at the correct OS fds.
                                            Stream::reattach_stdio(trx);

                                            // Error infrastructure is now valid (restored from image).
                                            trx->m_error_init_complete = true;

                                            // Thaw complete -- caller calls interpreter() to resume from restored exec
                                            // stack.
                                            guard.dismiss();
                                            return true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// Re-read the user-file-stream section + VM blob from `fd` to finish the overall thaw
// CRC and capture the VM-base sentinel.  `crc` is the running CRC after the header and
// memory blocks; returns the completed CRC and sets *sentinel_ok.  The 4 KB re-read
// scratch lives in this frame, keeping thaw_op's frame (which holds the fixed record
// arrays) within the per-function C-stack-usage budget.  Throws on I/O error.
[[gnu::noinline]] static crc32_t thaw_scan_tail_crc(Trix *trx,
                                                    int fd,
                                                    int64_t ufs_section_start,
                                                    int64_t vm_blob_offset,
                                                    const SnapShotHeader *h,
                                                    crc32_t crc,
                                                    bool *sentinel_ok) {
    // Seek to ufs_section_start so the loop covers UFS section + VM blob contiguously.
    if (::lseek64(fd, ufs_section_start, SEEK_SET) == -1) {
        auto errno_str = errno_string();
        trx->error(Error::InvalidImageFile, "thaw: cannot seek to UFS section for CRC check: {}/{}", errno, errno_str);
    } else {
        uint8_t scratch[SnapshotIoBufSize];
        const uint64_t ufs_bytes = static_cast<uint64_t>(vm_blob_offset - ufs_section_start);
        uint64_t remaining = ufs_bytes + h->vm_used + h->vm_global_used;
        uint64_t bytes_scanned = 0;
        uint32_t vm_sentinel = 0;
        bool captured_sentinel = false;
        uint8_t sentinel_buf[sizeof(vm_sentinel)];
        uint32_t sentinel_have = 0;
        while (remaining > 0) {
            auto chunk = static_cast<uint32_t>(std::min(remaining, uint64_t{sizeof(scratch)}));
            if (!read_all(fd, scratch, chunk)) {
                auto errno_str = errno_string();
                trx->error(Error::InvalidImageFile, "thaw: VM blob read failed during CRC check: {}/{}", errno, errno_str);
            } else {
                crc = crc32_update(crc, scratch, chunk);
                // Capture the VM blob's first four bytes as they stream past.  The blob start
                // can land anywhere in a chunk -- including its last 1-3 bytes -- so accumulate
                // byte-by-byte across chunk boundaries rather than requiring all four in one chunk.
                if (!captured_sentinel && ((bytes_scanned + chunk) > ufs_bytes)) {
                    auto blob_pos = (bytes_scanned < ufs_bytes) ? static_cast<uint32_t>(ufs_bytes - bytes_scanned) : 0u;
                    while ((blob_pos < chunk) && (sentinel_have < sizeof(sentinel_buf))) {
                        sentinel_buf[sentinel_have] = scratch[blob_pos];
                        ++sentinel_have;
                        ++blob_pos;
                    }
                    if (sentinel_have == sizeof(sentinel_buf)) {
                        std::memcpy(&vm_sentinel, sentinel_buf, sizeof(vm_sentinel));
                        captured_sentinel = true;
                    }
                }
                bytes_scanned += chunk;
                remaining -= chunk;
            }
        }
        *sentinel_ok = captured_sentinel && (vm_sentinel == TRIX_SENTINEL);
        return crc;
    }
}

// thaw: str :- --
// Restores VM state from a snapshot file.
// throws: vm-full, file-open-error, invalid-image-file, io-write-error, opstack-underflow, type-check
static void thaw_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "thaw: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        // Copy filename before popping -- used for open64() and error messages.  This
        // PATH_MAX C-stack buffer is reused below for each reconnect filename, so the
        // image filename is only needed up through the open64() that follows.
        char path_scratch[PATH_MAX];
        copy_sv_to_path(trx, trx->m_op_ptr->sv_value(trx), path_scratch, sizeof(path_scratch), Error::FileOpenError, "thaw");

        // -- PHASE 1: VALIDATE ----------------------------------------------------
        // VM is untouched throughout; trx->error() is safe on any failure.

        // 1. Pop operand (stack in final state before error paths).
        --trx->m_op_ptr;

        // 2. Open file.  Fixed-capacity record arrays (no heap): each restored .buf is
        //    std::malloc'd and ownership transfers to a Stream in apply_fixup_streams;
        //    each reconnected .fd is an open fd transferred the same way.
        RestoredMemStream
                restored_mem_streams[MaxStreamCount];  // memory_stream_count <= MaxStreamCount (startup tail counts within it)
        uint16_t restored_count = 0;
        ReconnectedStream reconnected_streams[MaxStreamCount];
        uint16_t reconnected_count = 0;

        auto fd = ::open64(path_scratch, O_RDONLY);
        if (fd == -1) {
            auto errno_str = errno_string();
            trx->error(Error::FileOpenError, "thaw: cannot open '{}': {}/{}", path_scratch, errno, errno_str);
        } else {
            // RAII guard: on any trx->error() throw in Phase 1, closes fd and frees/closes
            // all resources accumulated in restored_mem_streams and reconnected_streams.
            struct Guard {
                Guard(int *f, RestoredMemStream *m, uint16_t *mn, ReconnectedStream *u, uint16_t *un) :
                        m_fd(f),
                        m_mem(m),
                        m_mem_count(mn),
                        m_ufs(u),
                        m_ufs_count(un),
                        m_dismissed(false) {}

                ~Guard() {
                    if (!m_dismissed) {
                        if (*m_fd >= 0) {
                            ::close(*m_fd);
                        }
                        for (uint16_t i = 0; i < *m_mem_count; ++i) {
                            std::free(m_mem[i].buf);
                        }
                        for (uint16_t i = 0; i < *m_ufs_count; ++i) {
                            ::close(m_ufs[i].fd);
                        }
                    }
                }

                void dismiss() { m_dismissed = true; }

                int *m_fd;
                RestoredMemStream *m_mem;
                uint16_t *m_mem_count;
                ReconnectedStream *m_ufs;
                uint16_t *m_ufs_count;
                bool m_dismissed;
            } guard(&fd, restored_mem_streams, &restored_count, reconnected_streams, &reconnected_count);

            // 3. Read header; save/zero checksum.
            SnapShotHeader h{};
            if (!read_all(fd, &h, sizeof(h))) {
                auto errno_str = errno_string();
                trx->error(Error::InvalidImageFile, "thaw: header read failed: {}/{}", errno, errno_str);
            } else {
                auto saved_crc = h.checksum;
                h.checksum = 0;

                // 4. Validate magic, version, endianness.
                constexpr uint8_t MAGIC[4] = {'T', 'R', 'I', 'X'};
                if ((std::memcmp(h.magic, MAGIC, 4) != 0) || (h.snapshot_version != SNAPSHOT_VERSION)) {
                    trx->error(Error::InvalidImageFile, "thaw: invalid snap-shot file (bad magic or version)");
                } else if (h.operator_table_signature != compute_operator_table_signature()) {
                    trx->error(Error::InvalidImageFile,
                               "thaw: image built by an incompatible Trix (image has {} operators, this binary has {})",
                               static_cast<integer_t>(h.operator_count),
                               static_cast<integer_t>(SYSOPERATOR_COUNT));
                } else {
                    {
                        auto expected_le = (std::endian::native == std::endian::little) ? uint8_t{1} : uint8_t{0};
                        if (h.endian_le != expected_le) {
                            trx->error(
                                    Error::InvalidImageFile, "thaw: endian mismatch (file={}, host={})", h.endian_le, expected_le);
                        }
                    }

                    // 5. Check VM capacity.  Local data must fit; if there's a global region in the
                    // image, capacity must match exactly so the global block lands at its original
                    // offset (offsets stored in the global region are not rebased on thaw).
                    auto vm_capacity = static_cast<uint32_t>(trx->m_vm_limit - trx->m_vm_base);
                    if (h.vm_used > vm_capacity) {
                        trx->error(Error::InvalidImageFile, "thaw: VM too small (need {} bytes, have {})", h.vm_used, vm_capacity);
                    } else {
                        if ((h.vm_global_used > 0) && (h.vm_capacity != vm_capacity)) {
                            trx->error(Error::InvalidImageFile,
                                       "thaw: VM capacity mismatch with global region present (file capacity={}, host "
                                       "capacity={}); global offsets are "
                                       "not rebased",
                                       h.vm_capacity,
                                       vm_capacity);
                        }
                        if (h.vm_global_used > (vm_capacity - h.vm_used)) {
                            trx->error(Error::InvalidImageFile,
                                       "thaw: local + global ({} + {}) exceeds VM capacity {}",
                                       h.vm_used,
                                       h.vm_global_used,
                                       vm_capacity);
                        }

                        // 6. Read memory stream blocks into malloc'd bufs; compute mem CRC.  Bound the
                        //    count to the fixed array (a corrupt image could claim a huge count).
                        if (h.memory_stream_count > static_cast<uint32_t>(MaxStreamCount)) {
                            trx->error(Error::InvalidImageFile,
                                       "thaw: memory stream count {} exceeds maximum {}",
                                       h.memory_stream_count,
                                       MaxStreamCount);
                        }
                        crc32_t computed_mem_crc = 0;

                        for (uint32_t i = 0; i < h.memory_stream_count; ++i) {
                            vm_offset_t off = 0;
                            size_t rem = 0;
                            if (!read_all(fd, &off, sizeof(off)) || !read_all(fd, &rem, sizeof(rem))) {
                                trx->error(Error::InvalidImageFile, "thaw: memory stream block {} header read failed", i);
                            } else if (rem == 0) {
                                trx->error(Error::InvalidImageFile, "thaw: memory stream block {} has zero size (corrupt file)", i);
                            } else {
                                auto buf = static_cast<vm_t *>(std::malloc(rem));
                                if (buf == nullptr) {
                                    trx->error(Error::VMFull, "thaw: malloc failed for memory stream block {} ({} bytes)", i, rem);
                                } else if (!read_all(fd, buf, rem)) {
                                    std::free(buf);
                                    trx->error(Error::InvalidImageFile, "thaw: memory stream block {} data read failed", i);
                                } else {
                                    computed_mem_crc = crc32_update(computed_mem_crc, &off, sizeof(off));
                                    computed_mem_crc = crc32_update(computed_mem_crc, &rem, sizeof(rem));
                                    computed_mem_crc = crc32_update(computed_mem_crc, buf, rem);
                                    restored_mem_streams[restored_count++] = {off, buf, rem};
                                }
                            }
                        }

                        // 7. Record the file position at the start of the user file stream section.
                        auto ufs_section_start = ::lseek64(fd, 0, SEEK_CUR);
                        if (ufs_section_start == -1) {
                            auto errno_str = errno_string();
                            trx->error(
                                    Error::InvalidImageFile, "thaw: cannot determine UFS section offset: {}/{}", errno, errno_str);
                        } else {
                            // 8. Read user file stream blocks; test-open and pre-seek each file.  Bound the
                            //    count to the fixed array.
                            if (h.user_file_stream_count > static_cast<uint32_t>(MaxStreamCount)) {
                                trx->error(Error::InvalidImageFile,
                                           "thaw: user file stream count {} exceeds maximum {}",
                                           h.user_file_stream_count,
                                           MaxStreamCount);
                            }
                            crc32_t computed_ufs_crc = 0;

                            for (uint32_t i = 0; i < h.user_file_stream_count; ++i) {
                                vm_offset_t stream_offset = 0;
                                int64_t file_offset = 0;
                                uint8_t open_mode = 0;
                                uint8_t flags = 0;
                                uint16_t fnlen = 0;
                                if (!read_all(fd, &stream_offset, sizeof(stream_offset)) ||
                                    !read_all(fd, &file_offset, sizeof(file_offset)) ||
                                    !read_all(fd, &open_mode, sizeof(open_mode)) || !read_all(fd, &flags, sizeof(flags)) ||
                                    !read_all(fd, &fnlen, sizeof(fnlen))) {
                                    trx->error(Error::InvalidImageFile, "thaw: user file stream block {} header read failed", i);
                                }
                                // Read the filename into the reused path buffer (it must fit, to reopen).
                                if (fnlen >= sizeof(path_scratch)) {
                                    trx->error(Error::InvalidImageFile,
                                               "thaw: user file stream block {} filename too long ({} bytes)",
                                               i,
                                               fnlen);
                                } else if ((fnlen > 0) && !read_all(fd, path_scratch, fnlen)) {
                                    trx->error(Error::InvalidImageFile, "thaw: user file stream block {} filename read failed", i);
                                } else {
                                    path_scratch[fnlen] = '\0';
                                    computed_ufs_crc = crc32_update(computed_ufs_crc, &stream_offset, sizeof(stream_offset));
                                    computed_ufs_crc = crc32_update(computed_ufs_crc, &file_offset, sizeof(file_offset));
                                    computed_ufs_crc = crc32_update(computed_ufs_crc, &open_mode, sizeof(open_mode));
                                    computed_ufs_crc = crc32_update(computed_ufs_crc, &flags, sizeof(flags));
                                    computed_ufs_crc = crc32_update(computed_ufs_crc, &fnlen, sizeof(fnlen));
                                    computed_ufs_crc = crc32_update(computed_ufs_crc, path_scratch, fnlen);

                                    const int oflags = O_LARGEFILE | ((open_mode == 2)   ? O_RDWR
                                                                      : (open_mode == 1) ? O_WRONLY
                                                                                         : O_RDONLY);
                                    const int ufd = ::open64(path_scratch, oflags);
                                    if (ufd == -1) {
                                        auto errno_str = errno_string();
                                        trx->error(Error::InvalidImageFile,
                                                   "thaw: cannot reopen '{}': {}/{}",
                                                   path_scratch,
                                                   errno,
                                                   errno_str);
                                    } else {
                                        const bool is_append = (flags & 0x01) != 0;
                                        if (is_append) {
                                            // Append-mode: seek to current EOF so writes always go to the end.
                                            if (::lseek64(ufd, 0, SEEK_END) == -1) {
                                                auto errno_str = errno_string();
                                                ::close(ufd);
                                                trx->error(Error::InvalidImageFile,
                                                           "thaw: cannot seek '{}' to EOF: {}/{}",
                                                           path_scratch,
                                                           errno,
                                                           errno_str);
                                            }
                                        } else {
                                            // Non-append: verify file is large enough, then seek to saved position.
                                            struct stat st{};
                                            if (::fstat(ufd, &st) == -1) {
                                                auto errno_str = errno_string();
                                                ::close(ufd);
                                                trx->error(Error::InvalidImageFile,
                                                           "thaw: cannot stat '{}': {}/{}",
                                                           path_scratch,
                                                           errno,
                                                           errno_str);
                                            } else {
                                                int64_t file_size = static_cast<int64_t>(st.st_size);
                                                if (file_size < file_offset) {
                                                    ::close(ufd);
                                                    trx->error(Error::InvalidImageFile,
                                                               "thaw: '{}' truncated (size={}, need {})",
                                                               path_scratch,
                                                               file_size,
                                                               file_offset);
                                                }
                                                if (::lseek64(ufd, file_offset, SEEK_SET) == -1) {
                                                    auto errno_str = errno_string();
                                                    ::close(ufd);
                                                    trx->error(Error::InvalidImageFile,
                                                               "thaw: cannot seek '{}' to {}: {}/{}",
                                                               path_scratch,
                                                               file_offset,
                                                               errno,
                                                               errno_str);
                                                }
                                            }
                                        }
                                        reconnected_streams[reconnected_count++] = {stream_offset, ufd};
                                    }
                                }
                            }

                            // 9. Record the file position at the start of the VM blob (after UFS blocks).
                            auto vm_blob_offset = ::lseek64(fd, 0, SEEK_CUR);
                            if (vm_blob_offset == -1) {
                                auto errno_str = errno_string();
                                trx->error(
                                        Error::InvalidImageFile, "thaw: cannot determine VM blob offset: {}/{}", errno, errno_str);
                            } else {
                                // 10. Compute overall CRC over UFS blocks + VM blob via 4KB scratch reads.
                                // CRC order: header + memory blocks + UFS section bytes + VM blob bytes.
                                // Memory blocks are already loaded in restored_mem_streams; feed them first.
                                // UFS section: re-read by seeking back to ufs_section_start then to vm_blob_offset.
                                {
                                    auto crc = crc32_update(0, &h, sizeof(h));
                                    for (uint16_t i = 0; i < restored_count; ++i) {
                                        auto *r = &restored_mem_streams[i];
                                        crc = crc32_update(crc, &r->offset, sizeof(r->offset));
                                        crc = crc32_update(crc, &r->remaining, sizeof(r->remaining));
                                        crc = crc32_update(crc, r->buf, r->remaining);
                                    }

                                    // Feed UFS-section + VM-blob bytes via a helper that owns the 4 KB re-read
                                    // buffer (keeping it out of thaw_op's frame, which holds the record arrays).
                                    bool sentinel_ok = false;
                                    crc = thaw_scan_tail_crc(trx, fd, ufs_section_start, vm_blob_offset, &h, crc, &sentinel_ok);

                                    // 11. Verify overall CRC.
                                    if (crc != saved_crc) {
                                        trx->error(Error::InvalidImageFile,
                                                   "thaw: checksum mismatch (file={:#010x}, computed={:#010x})",
                                                   saved_crc,
                                                   crc);
                                    } else {
                                        // 11a. Verify VM-base sentinel.  See startup_image for rationale.
                                        if (!sentinel_ok) {
                                            trx->error(Error::InvalidImageFile,
                                                       "thaw: VM-base sentinel mismatch (expected 'TRIX')");
                                        }
                                    }
                                }

                                // 12. Defense-in-depth: verify memory stream CRC separately.
                                if ((h.memory_stream_count > 0) && (computed_mem_crc != h.memory_stream_crc)) {
                                    trx->error(Error::InvalidImageFile, "thaw: memory stream checksum mismatch");
                                } else {
                                    // 13. Defense-in-depth: verify user file stream CRC separately.
                                    // Runs after the overall CRC so the overall "checksum mismatch" fires first
                                    // for broad corruption; this check then disambiguates UFS-specific damage.
                                    if ((h.user_file_stream_count > 0) && (computed_ufs_crc != h.user_file_stream_crc)) {
                                        trx->error(Error::InvalidImageFile, "thaw: user file stream checksum mismatch");
                                    } else {
                                        // 14. Validate live useroperator table against frozen CRC.
                                        {
                                            uint32_t live_count = 0;
                                            crc32_t live_crc = 0;
                                            if (trx->m_useroperators != nullptr) {
                                                for (auto p = trx->m_useroperators; p->m_func != nullptr; ++p, ++live_count) {
                                                    auto sv = p->m_sv;
                                                    auto len = static_cast<uint16_t>(sv.size());
                                                    live_crc = crc32_update(live_crc, &len, sizeof(len));
                                                    live_crc = crc32_update(live_crc, sv.data(), sv.size());
                                                }
                                            }
                                            if ((live_count != h.useroperator_count) || (live_crc != h.useroperator_names_crc)) {
                                                trx->error(Error::InvalidImageFile,
                                                           "thaw: incompatible useroperator table; re-create snap-shot");
                                            }
                                        }

                                        // 15. Flush stdout/stderr while still in Phase 1 so the guard covers cleanup
                                        //     if flush() calls trx->error() (e.g. on write failure or wrong stream mode).
                                        if (trx->m_stdout != nullptr) {
                                            trx->m_stdout->flush(trx);
                                        }
                                        if (trx->m_stderr != nullptr) {
                                            trx->m_stderr->flush(trx);
                                        }

                                        // All Phase 1 validation passed -- dismiss the guard so it won't close fd or
                                        // free resources that are about to be transferred to the restored VM state.
                                        guard.dismiss();

                                        // -- PHASE 2: COMMIT ------------------------------------------------------
                                        // Past this point trx->error() must NOT be called (live state is being mutated).

                                        // 16. Free IsMemory ext buffers in the current inuse list.
                                        for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                                            if (s->is_memory() && (s->m_ext_base != nullptr)) {
                                                std::free(s->m_ext_base);
                                                s->m_ext_base = nullptr;
                                            }
                                        }

                                        // 17. Close non-stdio, non-memory fds (raw close only --
                                        //     no free_stream() / list mutation; stream objects are about to be overwritten).
                                        //     The startup-file stream is included: its fd is about to be lost when the VM
                                        //     blob overwrites the stream struct, and apply_fixup_streams will assign a fresh
                                        //     dup(/dev/null) to the restored startup stream.
                                        //     Skip any fd that belongs to a reconnected stream -- those are live fds in
                                        //     the current process that must survive into apply_fixup_streams.  Compare by
                                        //     fd value, not by VM offset: UFS block offsets are from the snap-shot VM and
                                        //     do not correspond to current-VM stream positions.
                                        for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                                            if (!s->is_stdio() && !s->is_memory() && (s->m_fd >= 0)) {
                                                // Linear scan over reconnected fds (bounded by MaxStreamCount, typically
                                                // a handful) -- no hash structure needed.
                                                bool is_reconnected = false;
                                                for (uint16_t j = 0; j < reconnected_count; ++j) {
                                                    if (reconnected_streams[j].fd == s->m_fd) {
                                                        is_reconnected = true;
                                                        break;
                                                    }
                                                }
                                                if (!is_reconnected) {
                                                    ::close(s->m_fd);
                                                }
                                            }
                                        }

                                        // 18. Seek back to VM blob and read it directly into the VM heap.
                                        // Past the commit point trx->error() cannot be called; abort on I/O failure
                                        // to avoid continuing with a partially-loaded VM.
                                        if ((::lseek64(fd, vm_blob_offset, SEEK_SET) == -1) ||
                                            !read_all(fd, trx->m_vm_base, h.vm_used)) {
                                            trx->diag_println(
                                                    "thaw: fatal I/O error reading VM data: {}/{}", errno, errno_string());
                                            std::abort();
                                        }
                                        // 18a. Read the global VM blob (if any) into [m_vm_limit - vm_global_used, m_vm_limit).
                                        if (h.vm_global_used > 0) {
                                            if (!read_all(fd, trx->m_vm_limit - h.vm_global_used, h.vm_global_used)) {
                                                trx->diag_println("thaw: fatal I/O error reading global VM data: {}/{}",
                                                                  errno,
                                                                  errno_string());
                                                std::abort();
                                            }
                                        }

                                        // 19. Close the image file.
                                        ::close(fd);

                                        // -- PHASE 3: RESTORE -----------------------------------------------------

                                        // 20. Restore all scalar/pointer fields and reset transient state.
                                        restore_from_header(trx, &h);

                                        // 21. Fix up Stream raw pointer fields; ownership of restored_mem_streams bufs and
                                        //     reconnected_streams fds transfers to Stream objects here -- do NOT free/close them.
                                        apply_fixup_streams(
                                                trx,
                                                std::span<const RestoredMemStream>(restored_mem_streams, restored_count),
                                                std::span<const ReconnectedStream>(reconnected_streams, reconnected_count));

                                        // 22. Re-initialise stdio streams to point at the correct OS fds.
                                        Stream::reattach_stdio(trx);

                                        // Caller (interpreter loop) continues from restored m_exec_ptr.
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
