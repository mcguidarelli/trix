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
private:
//===--- Snapshot / Thaw Data Structures ---===//
//
// Defines the data structures used by snap-shot (serialize) and thaw
// (deserialize) to capture and restore the entire VM state to/from a
// byte stream.  The actual snap-shot and thaw operators are in
// ops_snapshot.inl; this file contains only the struct definitions.
// Based on:
//
//   Erlang term_to_binary / binary_to_term: serializing arbitrary runtime
//   state to a portable binary format and reconstituting it later.
//
//   Smalltalk image files: saving the entire object memory as a snapshot
//   that can be resumed later, preserving all objects, threads, and state.
//
// --- Core concepts for maintainers ---
//
// WHAT IS CAPTURED
//   A snapshot captures the complete VM state:
//   - The local VM heap (m_vm_base to m_vm_ptr): all locally-allocated
//     objects, strings, arrays, dicts, coroutine contexts, mailboxes,
//     pipe buffers, etc.
//   - The global VM heap (m_vm_global to m_vm_limit): allocations made
//     under m_curr_alloc_global=true via ${...} / $/foo / <lit>#$ /
//     `set-global true` and the runtime allocator ops.  Survives every
//     save/restore in the source process and survives snap-shot/thaw.
//   - The SnapShotHeader: ~140 root pointers and scalar fields that
//     reference heap structures (stack bases/pointers/limits, dict
//     pointers, stream state, coroutine state, save state, etc.).
//   - Open file stream metadata (filename, position) for reconnection.
//
//   The snapshot is position-independent: all internal references use
//   vm_offset_t (relative to m_vm_base), so the heap can be loaded at
//   any base address.
//
// WHAT IS NOT CAPTURED
//   - External file descriptors (reopened on thaw via reconnection)
//   - The host-provided memory stream buffer (invoke API)
//   - OS-level state (threads, mutexes, timers)
//   - Readline history
//
// RECONNECTION
//   File streams that were open at snapshot time are reconnected on thaw:
//   the filename and file position are stored in the snapshot, and the
//   file is reopened and seeked to the saved position.  Memory streams
//   (from the invoke API) use RestoredMemStream to track the external
//   buffer provided by the host during thaw.
//
// SNAPSHOTHEADER
//   A large struct (~140 fields) that captures every Trix member variable
//   needed to resume execution.  Written as a binary blob at the start
//   of the snapshot, followed by the raw heap bytes.
//
// Tracks a restored IsMemory stream external buffer during thaw.
// Used by startup_image and thaw_op.
struct RestoredMemStream {
    vm_offset_t offset;
    vm_t *buf;
    size_t remaining;  // written/read as sizeof(size_t) bytes -- 64-bit only
};
// The file format stores 'remaining' with native sizeof(size_t) width.  Trix is
// 64-bit only (see the __int128 gate in types.inl); the static_assert below
// enforces the 8-byte word the format depends on.
static_assert(sizeof(size_t) == 8, "memory stream block file format assumes 8-byte size_t (64-bit build only)");

// Chunk size for snap-shot/thaw streaming reads (startup-tail CRC/copy, thaw CRC
// re-scan, user-file-name CRC).  Purely an I/O granularity knob -- not tied to the
// file format -- so it can be tuned freely.  NB: this is NOT the path-buffer size;
// those use PATH_MAX because realpath() requires a >= PATH_MAX output buffer.
static constexpr size_t SnapshotIoBufSize{1024};

// Carries a pre-opened, pre-seeked fd for a user file stream restored during thaw_op.
// Ownership of fd transfers to the Stream object inside apply_fixup_streams.
struct ReconnectedStream {
    vm_offset_t stream_offset;  // Stream's vm_offset_t in the VM heap
    int fd;                     // open64()+lseek64() already done; -1 not used
};

// Binary header written at the start of every snap-shot file.
// All multi-byte integers use native endianness; the endian_le field allows thaw to
// detect and reject files written on a machine with different byte order.
// KNOWN LIMITATIONS:
//   1. Address objects store raw host pointers; they will be garbage after thaw.
//   2. Native endianness only -- no cross-endian portability.
//   3. Open user file streams (SID >= FIRST_SID): the image records each
//      stream's filename, file offset, and flags.  thaw_op reopens the file
//      and seeks to the saved offset (or EOF for append streams), handing the
//      fresh fd to the restored Stream; startup-image thaw cannot reopen and
//      leaves them closed.  IsMemory streams are preserved by serializing
//      their external buffer.
//   4. The useroperator table pointer itself is not serialized; thaw validates
//      that the live table has the same count and names as the frozen image and
//      then continues using it.  The caller must have the same table installed.
//   5. The thaw target VM must have capacity >= the frozen vm_used bytes.
struct SnapShotHeader {
    // Type aliases for Trix-nested types used as field types.
    using crc32_t = Trix::crc32_t;

    // identity
    uint8_t magic[4];           // {'T', 'R', 'I', 'X'}
    uint32_t snapshot_version;  // monotonic format-revision counter (see SNAPSHOT_VERSION in types.inl)
    uint8_t endian_le;          // 1 = little-endian, 0 = big-endian

    // VM extents and base address.  Stored for diagnostics and covered by the overall CRC;
    // all Stream pointer fields use vm_offset_t so no address rebasing is needed on thaw.
    uint64_t vm_base_addr;      // reinterpret_cast<uint64_t>(m_vm_base) at snap-shot time
    uint32_t vm_used;           // m_vm_ptr - m_vm_base  (local VM bytes written after header)
    uint32_t vm_global_used;    // m_vm_limit - m_vm_global (global VM bytes written after the local block)
    uint32_t vm_capacity;       // m_vm_limit - m_vm_base (thaw needs >= this; must equal exactly when vm_global_used > 0)
    vm_offset_t gvm_free_head;  // head of sorted-by-offset global free list (nulloffset when empty)
    vm_offset_t gvm_fastbins[GvmFastBinCount];  // per-size LIFO fastbin head pointers (32/40/48/56-byte blocks);
                                                // each nulloffset when empty
    vm_offset_t gc_scratch_offset;              // lazy GC scratch block (ChunkKind::GcScratch); nulloffset when no user blocks live
    uint32_t gvm_user_block_count;              // count of LIVE user (non-GcScratch) blocks; gates scratch lifecycle
    uint32_t gvm_free_block_count;              // count of blocks currently on the global VM free list (main + fastbins combined)

    // scalar instance variables (size-descending order to avoid manual padding)
    name_bucket_count_t name_bucket_count;
    stream_buffer_size_t stream_buffer_size;
    length_t eqproc_length;
    length_t eqproc_stored_length;
    length_t eqdict_maxlength;
    length_t eqset_maxlength;
    // eqref generation counters: monotonically bumped on every #= creation; preserved
    // across snap-shot/thaw so eqref Objects in the saved image remain valid.
    uint32_t eqstring_generation;
    uint32_t eqarray_generation;
    uint32_t eqproc_generation;
    uint32_t eqdict_generation;
    uint32_t eqset_generation;
    length_t deferred_watcher_count;
    stream_id_t next_sid;
    save_level_t max_save_level;
    save_level_t curr_save_level;
    bool sid_wrapped;
    bool curr_alloc_global;  // m_curr_alloc_global (the set-global / current-global flag)
    uint8_t gc_current_gen;  // m_gc_current_gen at save time (a 1-bit flip-flop, {0,1}).  Restoring
                             // this is MANDATORY: m_gc_current_gen must never reset to a default on
                             // thaw.  Without it the post-thaw GC's first flip would collide with the
                             // stale m_mark_gen values stamped at save time -- a 1-in-2 false-mark
                             // (premature collection).  Saving the bit drives that probability to 0.
    Error last_error;
    Error external_error;
    interrupt_t interrupt;
    interrupt_t interrupt_mask;

    // Root Objects live in a heap-allocated Object[] array; only the pointer is in the header.
    vm_offset_t root_objects_offset;

    // PCG32 RNG state
    uint64_t pcg32_state;
    uint64_t pcg32_inc;

    // C++ pointers serialized as (ptr - m_vm_base); nulloffset means nullptr
    vm_offset_t vm_ptr_offset;
    vm_offset_t vm_alloc_active_offset;
    vm_offset_t save_stack_offset;
    vm_offset_t save_generation_offset;
    vm_offset_t extvalue_free_list_offset;
    vm_offset_t widevalue_free_list_offset;
    vm_offset_t vm_temp_save_offset;
    vm_offset_t extvalue_active_save_offset;
    vm_offset_t widevalue_active_save_offset;
    vm_offset_t save_journal_count_offset;
    vm_offset_t save_journal_bytes_offset;
    vm_offset_t dict_pool_offset;
    vm_offset_t frame_dict_pool_offset;
    vm_offset_t mailbox_pool_offset;
    vm_offset_t monitor_pool_offset;
    vm_offset_t pipe_pool_offset;
    vm_offset_t op_base_offset;
    vm_offset_t op_ptr_offset;
    vm_offset_t op_limit_offset;
    vm_offset_t dict_base_offset;
    vm_offset_t dict_ptr_offset;
    vm_offset_t dict_limit_offset;
    vm_offset_t exec_base_offset;
    vm_offset_t exec_ptr_offset;
    vm_offset_t exec_limit_offset;
    vm_offset_t err_base_offset;
    vm_offset_t err_ptr_offset;
    vm_offset_t err_limit_offset;
    vm_offset_t scratch_base_offset;  // main coroutine's scratch arena
    vm_offset_t scratch_ptr_offset;
    vm_offset_t scratch_limit_offset;
    vm_offset_t gc_roots_base_offset;  // GcScope root stack (always empty at a snapshot boundary)
    vm_offset_t error_string_base_offset;
    vm_offset_t error_string_limit_offset;
    vm_offset_t token_base_offset;
    vm_offset_t token_limit_offset;
    vm_offset_t name_buckets_offset;
    vm_offset_t systemname_offsets_offset;
    vm_offset_t typename_offsets_offset;
    vm_offset_t errorname_offsets_offset;
    vm_offset_t wellknown_offsets_offset;
    vm_offset_t systemdict_offset;
    vm_offset_t protocoldict_offset;
    vm_offset_t userdict_offset;
    vm_offset_t errordict_offset;
    vm_offset_t handlersdict_offset;
    vm_offset_t eqproc_storage_offset;
    vm_offset_t eqdict_offset;
    vm_offset_t eqset_offset;
    vm_offset_t stream_inuse_list_offset;
    vm_offset_t stream_free_list_offset;
    vm_offset_t stdin_offset;
    vm_offset_t stdedit_offset;
    vm_offset_t stdout_offset;
    vm_offset_t stderr_offset;
    vm_offset_t last_error_name_offset;
    vm_offset_t last_operator_offset;
    vm_offset_t last_error_msg_offset;
    vm_offset_t last_error_data_offset;
    vm_offset_t ostack_offset;
    vm_offset_t dstack_offset;
    vm_offset_t estack_offset;
    vm_offset_t require_dict_offset;
    vm_offset_t modules_dict_offset;
    vm_offset_t protocol_registry_offset;

    // Coroutine scheduler state
    vm_offset_t coroutine_head;
    vm_offset_t running_coroutine;
    vm_offset_t main_context;
    length_t live_coroutine_count;
    vm_offset_t ready_head;
    vm_offset_t ready_tail;
    vm_offset_t ready_high_head;
    vm_offset_t ready_high_tail;
    vm_offset_t timer_head;
    uint32_t next_coroutine_id;
    uint32_t coroutine_ops_remaining;
    uint32_t default_coroutine_quantum;
    uint64_t sched_count;
    uint64_t sched_ready_pops;
    uint64_t sched_timer_wakes;
    uint64_t sched_real_sleeps;
    uint32_t ready_queue_depth;
    uint32_t ready_high_queue_depth;
    uint32_t timer_list_depth;
    uint64_t starvation_threshold_ns;
    vm_offset_t coroutine_stack_free;
    vm_offset_t coroutine_ctx_free;
    vm_offset_t binding_bucket_free;
    vm_offset_t binding_entry_free;
    vm_offset_t frame_dict_overflow;

    // Supervision
    uint32_t next_monitor_ref;

    // Reactive cells
    vm_offset_t current_cell;
    int32_t batch_depth;
    vm_offset_t deferred_watchers;

    // GenServer
    uint64_t gen_ref_counter;

    // User-defined operator table identity.  snap-shot stores the count of entries in
    // m_useroperators (null-func-terminated) and a CRC-32 over their encoded names
    // (each as uint16_t length + name bytes, native endian).  thaw recomputes the same
    // CRC from the live table and rejects the image if count or CRC do not match.
    uint32_t useroperator_count;     // 0 if no user operators were installed
    crc32_t useroperator_names_crc;  // CRC-32 over encoded name bytes (0 if count == 0)

    // IsMemory stream serialization.  Open memory streams with m_ext_remaining > 0 at
    // snap-shot time are serialized as variable-length blocks between the header and the
    // VM blob.  Each block: [vm_offset_t stream_offset][size_t remaining][uint8_t data[]].
    // Streams with m_ext_remaining == 0 are left open; their VM buffer is in the blob.
    uint32_t memory_stream_count;  // number of IsMemory stream blocks serialized (0 if none)
    crc32_t memory_stream_crc;     // CRC-32 over all memory stream blocks (0 if count == 0)

    // User file stream reconnect section.  Seekable (SupportsRandomAccess) non-stdio,
    // non-memory, non-startup-file streams that were open at snap-shot time are serialized
    // here so runtime thaw can re-open them.
    // Each block: [vm_offset_t stream_offset][int64_t file_offset]
    //             [uint8_t open_mode][uint8_t flags][uint16_t filename_length][char filename[]]
    // open_mode: 0 = O_RDONLY, 1 = O_WRONLY, 2 = O_RDWR
    // flags: bit 0 = IsAppend (reconnect at SEEK_END rather than saved file_offset)
    uint32_t user_file_stream_count;  // 0 if none
    crc32_t user_file_stream_crc;     // CRC-32 over all blocks (0 if count == 0)

    // Build-compatibility guard, placed just before the trailing checksum so it
    // shifts no other field's offset.  snapshot_version pins the FORMAT, but a
    // debug and an optimized binary of the SAME source share a format yet have
    // different operator tables (debug-only ops are #ifdef'd out), so an
    // operator's stored enum identity would mis-resolve on a cross-build thaw.
    // operator_table_signature (FNV-1a over every operator name; see
    // compute_operator_table_signature) diverges between such builds; thaw
    // rejects a mismatch instead of silently dispatching the wrong operator.
    // operator_count is carried only to make the rejection message legible.
    uint32_t operator_table_signature;  // Trix::compute_operator_table_signature() at save time
    uint32_t operator_count;            // Trix::SYSOPERATOR_COUNT at save time (diagnostic only)

    // CRC-32 (IEEE 802.3) over the entire header (this field zeroed) followed by
    // memory stream blocks (if any) followed by user file stream blocks (if any)
    // followed by vm_used bytes of VM data.
    // Verified by thaw before restoring any state.
    crc32_t checksum;
};
static_assert(std::is_trivially_copyable_v<SnapShotHeader>);
static_assert(sizeof(SnapShotHeader) == 592);              // guard against silent layout changes (v171 added gc_roots_base_offset;
                                                           // lazy GC scratch added gc_scratch_offset + gvm_user_block_count;
                                                           // live free-block counter consumed existing pad in v168;
                                                           // v174 added operator_table_signature + operator_count)
static_assert(offsetof(SnapShotHeader, checksum) == 584);  // checksum must be the last field

//===--- CRC-32 /ISO-HDLC (IEEE 802.3) reflected polynomial Checksum ---===//
constexpr static std::array<crc32_t, 256> crc32_table = []() {
    constexpr crc32_t CRC32_POLY{0xEDB88320};

    std::array<crc32_t, 256> table{};
    for (int i = 0; i < 256; ++i) {
        auto crc = static_cast<crc32_t>(i);
        for (int j = 0; j < 8; ++j) {
            // Branchless: -1 (all bits set) when LSB is 1 masks in the polynomial; 0 masks it out.
            crc = (crc >> 1) ^ (static_cast<crc32_t>(-static_cast<int>(crc & 1)) & CRC32_POLY);
        }
        table[static_cast<size_t>(i)] = crc;
    }
    return table;
}();

// CRC-32, IEEE 802.3 polynomial, over a byte buffer, suitable for chaining
[[nodiscard]] static crc32_t crc32_update(crc32_t crc, const void *buffer, size_t size) {
    auto ptr = static_cast<const uint8_t *>(buffer);
    crc = ~crc;
    while (size-- > 0) {
        crc = (crc >> 8) ^ crc32_table[(crc & 0xFF) ^ *ptr++];
    }
    return ~crc;
}
