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

//===--- VM Heap and Offset Utility Functions ---===//
//
// Implements the VM heap: a single contiguous memory region from which all
// runtime data is allocated.  Also provides offset/pointer conversion,
// stack bounds checking, and allocation primitives.  (Hash functions
// live in hash.inl -- they have no allocator dependency.)
// Based on:
//
//   PostScript VM: a managed memory region where all composite objects
//   (strings, arrays, dictionaries, names) reside.  The VM is the single
//   source of truth for all runtime state.
//
//   Forth dictionary: a linear memory region with a growing allocation
//   pointer.  Allocation is bump-pointer (O(1)); there is no general
//   free/realloc -- reclamation happens via save/restore rollback.
//
// --- Core concepts for maintainers ---
//
// HEAP LAYOUT
//   The VM heap is a single malloc'd block: m_vm_base to m_vm_limit.
//   Default size: 1 MiB, minimum: 256 KiB, configurable via --vm-size.
//
//   +------------------------------------------------------------------+
//   | m_vm_base                                            m_vm_limit  |
//   |  |                                                          |    |
//   |  v                                                          v    |
//   |  +--------+-----+--------+-----+---------+-----+----------+     |
//   |  | names  | ... | allocs | ... | temp    | ... | global   |     |
//   |  | extval |     |        |     | scratch |     | region   |     |
//   |  | stacks |     |        |     |         |     |          |     |
//   |  +--------+-----+--------+-----+---------+-----+----------+     |
//   |  ^                  ^             ^             ^                |
//   |  m_vm_base     m_vm_ptr     m_vm_temp_ptr   m_vm_global          |
//   |                (grows ->)    (<- grows)     (<- grows)           |
//   +------------------------------------------------------------------+
//
//   Main region (m_vm_base to m_vm_ptr): permanent allocations.
//     Grows upward.  Reclaimed only by save/restore rollback.
//   Temp region (m_vm_temp_ptr to m_vm_global): scratch allocations.
//     Grows downward from the global region's lower edge.  Used for
//     temporary dicts/arrays during set operations (intersect,
//     difference, unique).  Reclaimed by vm_temp_restore() after the
//     operation completes.
//   Global region (m_vm_global to m_vm_limit): GC-managed allocations
//     that survive save/restore (make-global-dict, ${...} blocks).
//     Grows downward from m_vm_limit; m_vm_global == m_vm_limit when
//     no globals exist.
//   Free space: between m_vm_ptr and m_vm_temp_ptr.
//
//   Allocation grows upward from m_vm_base via m_vm_ptr (bump pointer).
//   Temporary allocations grow downward via m_vm_temp_ptr, anchored at
//   m_vm_global (used for scratch space during operations like
//   intersect/difference).
//
//   All composite objects live in this heap: strings, arrays, packed
//   arrays, dicts/sets, names, ExtValues, coroutine contexts/stacks,
//   mailboxes, pipe buffers, monitor entries, cell headers, record
//   schemas/instances, save journal entries -- everything.
//
// OFFSET-BASED REFERENCES
//   All internal references use vm_offset_t (uint32_t byte offset from
//   m_vm_base) rather than raw pointers.  This makes the heap position-
//   independent: the entire heap can be serialized (snap-shot) and
//   loaded at a different base address (thaw) with all references intact.
//
//   offset_to_ptr<T>(offset) converts offset to T*.
//   ptr_to_offset(ptr) converts T* to offset.
//   These are the only sanctioned ways to traverse the offset/pointer
//   boundary.
//
//   `nulloffset` is the sentinel value 0 -- used by nullptr_to_offset /
//   offset_to_nullptr and as the "no link" marker on every vm_offset_t
//   field in heap structs.  init.inl reserves the first 4 VM bytes to
//   hold the TRIX_SENTINEL magic so no real allocation can land at
//   offset 0.  See init.inl's "Reserve VM offset 0 as the null-sentinel
//   guard word" block for the on-thaw-corruption-detector backstory.
//
// ALLOCATION
//   vm_alloc<T>(size)      -- allocate from the main heap (bump pointer)
//   vm_alloc_n<T>(count)   -- allocate an array of count T's
//   vm_temp_alloc_ptr<T>() -- allocate from the temp region (top-down)
//   vm_end_alloc()         -- cancel an in-progress variable-size alloc
//   vm_trim_alloc()        -- shrink the most recent allocation
//
//   Allocation is O(1) (bump pointer).  There is no free() -- memory is
//   reclaimed only by save/restore rollback (which resets m_vm_ptr to
//   its saved position, discarding everything allocated since the save).
//
//   VMFull (Error::VMFull) is raised when allocation would exceed the
//   heap limit.  This is the primary resource exhaustion error in Trix.
//
// STACK BOUNDS CHECKING
//   require_op_capacity(), require_op_count(), require_exec_capacity(),
//   etc. validate that the operand/execution/error/dictionary stacks
//   have sufficient room or depth before pushing or popping.  These are
//   the primary safety checks that prevent stack overflow/underflow.
//
// Hash functions (mix32/mix64_to_32, wyhash32_sv, fastmod_*) live in
// hash.inl, included before this file in trix.h.
//

template<typename T>
[[nodiscard]] static vm_size_t vm_sizeof() {
    return static_cast<vm_size_t>(sizeof(T));
}

[[nodiscard]] vm_offset_t ptr_to_offset(const void *ptr) {
    if ((ptr < m_vm_base) || (ptr >= m_vm_limit)) {
        error(Error::InternalError, "ptr_to_offset: address out of VM range");
    } else {
        const uintptr_t offset = static_cast<uintptr_t>(static_cast<const vm_t *>(ptr) - m_vm_base);
        return static_cast<vm_offset_t>(offset & std::numeric_limits<vm_offset_t>::max());
    }
}

[[nodiscard]] vm_offset_t nullptr_to_offset(const void *ptr) {
    if (ptr == nullptr) {
        return nulloffset;
    } else if ((ptr < m_vm_base) || (ptr >= m_vm_limit)) {
        // Boundary is half-open [m_vm_base, m_vm_limit), matching ptr_to_offset
        // and offset_to_ptr.  A one-past-end pointer is NOT in-range -- the
        // round-trip through offset_to_ptr would error on the resulting offset,
        // so we reject it here for symmetry.
        error(Error::InternalError, "nullptr_to_offset: address out of VM range");
    } else {
        const uintptr_t offset = static_cast<uintptr_t>(static_cast<const vm_t *>(ptr) - m_vm_base);
        return static_cast<vm_offset_t>(offset & std::numeric_limits<vm_offset_t>::max());
    }
}

template<typename T>
[[nodiscard]] T *offset_to_ptr(vm_offset_t offset) {
    if (offset == nulloffset) {
        error(Error::InternalError, "offset_to_ptr: null offset");
    } else if (offset >= static_cast<uintptr_t>(m_vm_limit - m_vm_base)) {
        error(Error::InternalError, "offset_to_ptr: offset {} out of VM range", offset);
    } else {
        return reinterpret_cast<T *>(m_vm_base + offset);
    }
}

template<typename T>
[[nodiscard]] T offset_to_value(vm_offset_t offset) {
    return *offset_to_ptr<T>(offset);
}

template<typename T>
[[nodiscard]] T *offset_to_nullptr(vm_offset_t offset) {
    if (offset == nulloffset) {
        return nullptr;
    } else if (offset >= static_cast<uintptr_t>(m_vm_limit - m_vm_base)) {
        // Half-open boundary, matching nullptr_to_offset / offset_to_ptr.
        // A one-past-end offset is NOT in-range.
        error(Error::RangeCheck, "offset_to_nullptr: offset {} out of VM range", offset);
    } else {
        return reinterpret_cast<T *>(m_vm_base + offset);
    }
}

// check if offset is valid within entire VM range, AND has room for a
// full sizeof(T) read at that offset.  The bound is `offset + sizeof(T)
// <= vm_size`, expressed in subtract-first form to avoid overflow on
// 32-bit vm_offset_t.
template<typename T>
[[nodiscard]] bool valid_offset(vm_offset_t offset) const {
    // Check alignment of the actual address (m_vm_base + offset), not just the offset,
    // to correctly handle any residual misalignment in m_vm_base.
    auto mask = uintptr_t{alignof(T) - 1};
    auto vm_size = static_cast<vm_offset_t>(m_vm_limit - m_vm_base);
    return ((offset != nulloffset) && (sizeof(T) <= vm_size) && (offset <= (vm_size - sizeof(T))) &&
            (((reinterpret_cast<uintptr_t>(m_vm_base) + offset) & mask) == 0));
}

// check if offset is valid within active allocated VM range -- LOCAL
// (below m_vm_ptr) OR GLOBAL (at-or-above m_vm_global, below m_vm_limit).
// Both halves represent live storage that the runtime currently owns;
// the LOCAL half is journal-tracked and reclaimable on restore, the
// GLOBAL half is journal-skipped and permanent.
// Bound is `offset + sizeof(T) <= region_end`, in subtract-first form.
template<typename T>
[[nodiscard]] bool valid_active_offset(vm_offset_t offset) const {
    auto mask = uintptr_t{alignof(T) - 1};
    auto local_end = static_cast<vm_offset_t>(m_vm_ptr - m_vm_base);
    auto global_start = static_cast<vm_offset_t>(m_vm_global - m_vm_base);
    auto global_end = static_cast<vm_offset_t>(m_vm_limit - m_vm_base);
    auto in_local = (sizeof(T) <= local_end) && (offset <= local_end - sizeof(T));
    auto in_global = (sizeof(T) <= global_end - global_start) && (offset >= global_start) && (offset <= global_end - sizeof(T));
    return ((offset != nulloffset) && (in_local || in_global) && (((reinterpret_cast<uintptr_t>(m_vm_base) + offset) & mask) == 0));
}

// Returns m_vm_ptr advanced forward to the next alignment boundary for T.
template<typename T>
[[nodiscard]] vm_t *align_vm_ptr() const {
    auto mask = uintptr_t{alignof(T) - 1};
    return reinterpret_cast<vm_t *>((reinterpret_cast<uintptr_t>(m_vm_ptr) + mask) & ~mask);
}

template<typename T>
bool vm_size_available(vm_size_t size) {
    assert(m_vm_alloc_active == nullptr);

    auto aligned_ptr = align_vm_ptr<T>();
    auto vm_ptr = (aligned_ptr + size);
    return (vm_ptr <= m_vm_temp_ptr);
}

// vm-heap-tracking: record an allocation.  Two layers:
//   the alloc-stats hash table  -- aggregated per (sid, line).
//   the heap-track side-table   -- per-block, indexed by offset.
// Hot-path cost: one read of m_last_scan_location, hash + probe in the
// alloc-stats table, append in the side-table.  Skipped when
// m_alloc_tracking_paused is set (the alloc-stats / vm-heap-snapshot
// ops suppress self-measurement during result-array build).
//
// The function definition is gated on TRIX_HEAP_TRACKING so release
// builds drop it entirely; its call sites are gated on the same macro
// so the lookup of this name never fires in release.
#ifdef TRIX_HEAP_TRACKING
void record_alloc_stats(vm_size_t size, vm_offset_t offset) {
    // Peak trackers update unconditionally (independent of paused flag),
    // so they catch transient spikes even during op result-array builds.
    if (m_vm_ptr > m_vm_peak_ptr) {
        m_vm_peak_ptr = m_vm_ptr;
    }
    auto temp_used = static_cast<vm_size_t>(m_vm_limit - m_vm_temp_ptr);
    if (temp_used > m_vm_peak_temp_used) {
        m_vm_peak_temp_used = temp_used;
    }

    if (!m_alloc_tracking_paused) [[likely]] {
        auto key = (static_cast<uint64_t>(m_last_scan_location.m_sid) << 32) | static_cast<uint64_t>(m_last_scan_location.m_line);
        if (key == 0) [[unlikely]] {
            key = 1;  // collide INVALID_SID/line-0 onto a real slot; m_key == 0 is reserved as "empty"
        }

        // Aggregate into the per-(sid,line) alloc-stats hash table.
        auto h = static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 32) & (m_alloc_stats_capacity - 1);
        for (auto probe = size_t{0}; probe < m_alloc_stats_capacity; ++probe) {
            auto idx = (h + probe) & (m_alloc_stats_capacity - 1);
            auto &entry = m_alloc_stats[idx];
            if (entry.m_key == key) {
                ++entry.m_count;
                entry.m_bytes += size;
                break;
            } else if (entry.m_key == 0) {
                entry.m_key = key;
                entry.m_count = 1;
                entry.m_bytes = size;
                ++m_alloc_stats_count;
                break;
            } else if ((probe + 1) == m_alloc_stats_capacity) {
                // Saturation = no empty slot found AND no existing slot
                // matched.  A long-but-not-full probe chain that ends in
                // either a match or an empty slot exits via the inner
                // breaks above; only a fully-walked table reaches here.
                m_alloc_stats_saturated = true;
            }
        }

        // Append a per-block entry to the heap-track side-table.  Bump
        // allocator semantics mean entries are always in increasing offset
        // order (modulo restore-prune from the tail).
        if (m_heap_track_count < m_heap_track_capacity) [[likely]] {
            m_heap_track[m_heap_track_count++] = HeapTrackEntry{offset, key};
        } else {
            m_heap_track_saturated = true;
        }
    }
}
#endif

template<typename T>
[[nodiscard]] T *vm_alloc_ptr(vm_size_t size = vm_sizeof<T>()) {
    // Invariant: no dynamic allocation in progress (set by vm_start_alloc, cleared by vm_end_alloc variants)
    assert(m_vm_alloc_active == nullptr);

    auto aligned_ptr = align_vm_ptr<T>();
    auto vm_ptr = (aligned_ptr + size);
    if (vm_ptr <= m_vm_temp_ptr) {
        m_vm_ptr = vm_ptr;
#ifdef TRIX_HEAP_TRACKING
        record_alloc_stats(size, static_cast<vm_offset_t>(aligned_ptr - m_vm_base));
#endif
        return reinterpret_cast<T *>(aligned_ptr);
    } else {
#ifdef TRIX_HEAP_TRACKING
        std::fprintf(stderr,
                     "VM-FULL: req=%zu  used=%zu  peak-used=%zu  temp=%zu  peak-temp=%zu  "
                     "global=%zu  free=%zd  sl=%u  paused=%d  alloc_active=%p\n",
                     static_cast<size_t>(size),
                     static_cast<size_t>(m_vm_ptr - m_vm_base),
                     static_cast<size_t>(m_vm_peak_ptr - m_vm_base),
                     static_cast<size_t>(m_vm_global - m_vm_temp_ptr),
                     static_cast<size_t>(m_vm_peak_temp_used),
                     static_cast<size_t>(m_vm_limit - m_vm_global),
                     static_cast<ssize_t>(m_vm_temp_ptr - m_vm_ptr),
                     static_cast<unsigned>(m_curr_save_level),
                     static_cast<int>(m_alloc_tracking_paused),
                     static_cast<void *>(m_vm_alloc_active));
#endif
        error(Error::VMFull, "cannot allocate {} bytes", size);
    }
}

template<typename T>
[[nodiscard]] T *vm_alloc_n_ptr(vm_size_t n) {
    if (n > (std::numeric_limits<vm_size_t>::max() / vm_sizeof<T>())) {
        error(Error::LimitCheck, "vm_alloc_n_ptr: allocation count {} overflows vm_size_t", n);
    } else {
        return vm_alloc_ptr<T>(n * vm_sizeof<T>());
    }
}

template<typename T>
[[nodiscard]] std::pair<T *, vm_offset_t> vm_alloc(vm_size_t size = vm_sizeof<T>()) {
    auto ptr = vm_alloc_ptr<T>(size);
    return std::pair{ptr, static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(ptr) - m_vm_base)};
}

template<typename T>
[[nodiscard]] std::pair<T *, vm_offset_t> vm_alloc_n(vm_size_t n) {
    if (n > (std::numeric_limits<vm_size_t>::max() / vm_sizeof<T>())) {
        error(Error::LimitCheck, "vm_alloc_n: allocation count {} overflows vm_size_t", n);
    } else {
        return vm_alloc<T>(n * vm_sizeof<T>());
    }
}

template<typename T>
[[nodiscard]] vm_size_t vm_remaining() {
    // Invariant: no dynamic allocation in progress (set by vm_start_alloc, cleared by vm_end_alloc variants)
    assert(m_vm_alloc_active == nullptr);

    auto aligned_ptr = align_vm_ptr<T>();
    return ((aligned_ptr < m_vm_temp_ptr) ? static_cast<vm_size_t>(m_vm_temp_ptr - aligned_ptr) : 0);
}

template<typename T>
[[nodiscard]] std::pair<T *, vm_size_t> vm_start_alloc() {
    // Only one dynamic (open-ended) allocation may be in progress at a time.
    // vm_start_alloc/vm_end_alloc bracket a single caller that is streaming bytes
    // into VM memory (e.g. the string scanner).  A second caller interleaving its
    // own vm_alloc would corrupt the in-progress allocation.
    // Invariant: no dynamic allocation in progress (set by vm_start_alloc, cleared by vm_end_alloc variants)
    assert(m_vm_alloc_active == nullptr);

    auto vm_alloc_active = align_vm_ptr<T>();
    if (vm_alloc_active < m_vm_temp_ptr) {
        m_vm_alloc_active = vm_alloc_active;
        return std::pair{reinterpret_cast<T *>(m_vm_alloc_active), static_cast<vm_size_t>(m_vm_temp_ptr - m_vm_alloc_active)};
    } else {
        error(Error::VMFull, "cannot start dynamic allocation");
    }
}

template<typename T>
[[nodiscard]] T *vm_end_alloc_ptr(vm_size_t size) {
    // Invariant: no dynamic allocation in progress (set by vm_start_alloc, cleared by vm_end_alloc variants)
    assert(m_vm_alloc_active != nullptr);

    if ((m_vm_alloc_active + size) > m_vm_temp_ptr) {
        error(Error::VMFull, "cannot end dynamic allocation {} of bytes", size);
    } else {
        // When size == 0, the allocation is cancelled: m_vm_ptr is not advanced, so the
        // in-progress data is not committed to the VM heap.  The caller uses the returned
        // pointer only transiently (e.g. to copy a byte or snapshot string content) and
        // must not retain it across any subsequent vm_alloc call.
        if (size != 0) {
            m_vm_ptr = (m_vm_alloc_active + size);
#ifdef TRIX_HEAP_TRACKING
            record_alloc_stats(size, static_cast<vm_offset_t>(m_vm_alloc_active - m_vm_base));
#endif
        }
        auto ptr = reinterpret_cast<T *>(m_vm_alloc_active);
        m_vm_alloc_active = nullptr;
        return ptr;
    }
}

template<typename T>
[[nodiscard]] std::pair<T *, vm_offset_t> vm_end_alloc(vm_size_t size) {
    auto ptr = vm_end_alloc_ptr<T>(size);
    return std::pair{ptr, static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(ptr) - m_vm_base)};
}

void vm_end_alloc() {
    // Invariant: no dynamic allocation in progress (set by vm_start_alloc, cleared by vm_end_alloc variants)
    assert(m_vm_alloc_active != nullptr);

    m_vm_alloc_active = nullptr;
}

void vm_trim_alloc(const vm_t *ptr, vm_size_t size) {
    if ((size > 0) && (ptr == m_vm_ptr)) {
        m_vm_ptr -= size;
    }
}

// Allocate count elements of type T from the top-end temporary region.
// Grows downward from m_vm_limit.  Caller must save m_vm_temp_ptr before
// the first temp allocation and restore it (via vm_temp_restore) when
// temps are no longer needed.  Multiple temp allocations nest naturally
// (stack-based LIFO) -- no "in use" flag is needed.
template<typename T>
[[nodiscard]] T *vm_temp_alloc(vm_size_t count) {
    // no streaming allocation in progress (temp alloc would invalidate the
    // remaining-space value already returned by vm_start_alloc)
    if (m_vm_alloc_active != nullptr) {
        error(Error::InternalError, "vm_temp_alloc: dynamic allocation in progress");
    } else if (count > (std::numeric_limits<vm_size_t>::max() / vm_sizeof<T>())) {
        // overflow check (same pattern as vm_alloc_n_ptr)
        error(Error::LimitCheck, "vm_temp_alloc: count {} overflows vm_size_t", count);
    } else {
        auto size = count * vm_sizeof<T>();
        auto raw = reinterpret_cast<uintptr_t>(m_vm_temp_ptr) - size;
        auto aligned = raw & ~uintptr_t{alignof(T) - 1};
        auto new_temp_ptr = reinterpret_cast<vm_t *>(aligned);
        // collision with permanent region
        if (new_temp_ptr < m_vm_ptr) {
            error(Error::VMFull, "cannot allocate {} bytes of temporary space", size);
        } else {
            m_vm_temp_ptr = new_temp_ptr;
#ifdef TRIX_HEAP_TRACKING
            auto temp_used = static_cast<vm_size_t>(m_vm_limit - m_vm_temp_ptr);
            if (temp_used > m_vm_peak_temp_used) {
                m_vm_peak_temp_used = temp_used;
            }
#endif
            return reinterpret_cast<T *>(new_temp_ptr);
        }
    }
}

// Allocate size bytes from the top-end temporary region, aligned for type T.
// Like vm_alloc_ptr but grows downward from m_vm_limit.
template<typename T>
[[nodiscard]] T *vm_temp_alloc_ptr(vm_size_t size) {
    if (m_vm_alloc_active != nullptr) {
        error(Error::InternalError, "vm_temp_alloc_ptr: dynamic allocation in progress");
    } else {
        auto raw = reinterpret_cast<uintptr_t>(m_vm_temp_ptr) - size;
        auto aligned = raw & ~uintptr_t{alignof(T) - 1};
        auto new_temp_ptr = reinterpret_cast<vm_t *>(aligned);
        if (new_temp_ptr < m_vm_ptr) {
            error(Error::VMFull, "cannot allocate {} bytes of temporary space", size);
        } else {
            m_vm_temp_ptr = new_temp_ptr;
#ifdef TRIX_HEAP_TRACKING
            auto temp_used = static_cast<vm_size_t>(m_vm_limit - m_vm_temp_ptr);
            if (temp_used > m_vm_peak_temp_used) {
                m_vm_peak_temp_used = temp_used;
            }
#endif
            return reinterpret_cast<T *>(new_temp_ptr);
        }
    }
}

// Restore m_vm_temp_ptr to a previously saved value, freeing all temps
// allocated since that point.
void vm_temp_restore(vm_t *saved) {
    // can only free (move upward toward m_vm_limit), never allocate more
    if (saved < m_vm_temp_ptr) {
        error(Error::InternalError,
              "vm_temp_restore: saved pointer {} below current temp pointer {}",
              static_cast<const void *>(saved),
              static_cast<const void *>(m_vm_temp_ptr));
    } else if (saved > m_vm_limit) {
        // cannot exceed heap end
        error(Error::InternalError,
              "vm_temp_restore: saved pointer {} exceeds heap limit {}",
              static_cast<const void *>(saved),
              static_cast<const void *>(m_vm_limit));
    } else {
        m_vm_temp_ptr = saved;
    }
}

//===--- Global Region ---===//
//
// One top-of-heap region growing downward from m_vm_limit:
//
//   GLOBAL: journal-skipped, cross-coroutine-scan-skipped permanent storage.
//           Address-range encoded via is_global() (no header bit consumed).
//
// Layout:
//   | permanent heap |  free  |  temp scratch  |  global  |
//   ^                ^        ^                ^          ^
//   m_vm_base    m_vm_ptr  m_vm_temp_ptr   m_vm_global  m_vm_limit
//
// Invariant: m_vm_ptr <= m_vm_temp_ptr <= m_vm_global <= m_vm_limit.
// When GLOBAL idle: m_vm_global == m_vm_limit.
//
// Per-coroutine scratch lives inside each coroutine's
// stack block in global VM, not in this region.  See scratch_* below.
//
// All functions are templates to defer instantiation until Object is defined.

//===--- Per-Coroutine Scratch Arena ---===//
//
// scratch_push<T> / scratch_free<T> / scratch_collect<T> use the running
// coroutine's private slab carved from the back of its stack block.
// Storage lives in the coroutine's scratch region (Object slots between
// m_scratch_base and m_scratch_limit, sized by --scratch-depth).  Bump
// allocator growing upward; resets to base on free / collect.
//
// Used by find-all / find-n so per-call scratch state lives
// with the coroutine that produced it.  Yielding mid-find-all does not
// corrupt cross-coroutine state.
//
// Save/restore: scratch contents live in global VM (coroutine stacks)
// and don't journal-rollback.  scratch_free runs from the error()
// cleanup on non-Fail errors that occur while scratch is dirty.

// Push a T into the running coroutine's scratch arena.  Caller must
// have already cloned any ExtValue contents (clone goes in local VM
// at the post-restore save level).
template<typename T>
void scratch_push(T val_obj) {
    if (m_scratch_ptr >= m_scratch_limit) {
        error(Error::VMFull, "scratch arena exhausted (size {} Objects)", m_scratch_depth);
    } else {
        *m_scratch_ptr = val_obj;
        ++m_scratch_ptr;
    }
}

// Free all T objects in the running coroutine's scratch arena,
// releasing their ExtValues.  Resets m_scratch_ptr to base.
template<typename T>
void scratch_free() {
    auto base = static_cast<T *>(m_scratch_base);
    auto count = static_cast<length_t>(m_scratch_ptr - m_scratch_base);
    // Reset first so a maybe_free_extvalue error sees an idle arena.
    m_scratch_ptr = m_scratch_base;
    for (length_t i = 0; i < count; ++i) {
        base[i].maybe_free_extvalue(this);
    }
}

// Collect all T objects in the running coroutine's scratch into a
// fresh heap-allocated array (allocated in local VM via vm_alloc_n,
// at the caller's save level).  Order is preserved (push order =
// result order, opposite of persist which reverses).  After the
// copy, scratch is reset to idle and the elements are nulled so a
// subsequent scratch_free does not double-free their ExtValues.
template<typename T>
[[nodiscard]] T scratch_collect() {
    auto count = static_cast<length_t>(m_scratch_ptr - m_scratch_base);
    auto [elem_ptr, arr_offset] = vm_alloc_n<T>(count);
    auto src_ptr = static_cast<T *>(m_scratch_base);
    for (length_t i = 0; i < count; ++i) {
        elem_ptr[i] = src_ptr[i];
    }
    for (length_t i = 0; i < count; ++i) {
        src_ptr[i] = T::make_null();
    }
    m_scratch_ptr = m_scratch_base;
    return T::make_array(arr_offset, count);
}

//===--- Global Region (low-level bump primitive) ---===//
//
// vm_alloc_global_ptr<T>(size) is the low-level bump allocator that
// grows the global region downward.  The global VM allocator's header subsystem
// (see gvm_heap.inl) layers on top of this primitive: every external
// caller goes through `gvm_alloc<T>(size, kind)`, which prepends a
// ChunkKind-tagged GvmBlock header before invoking this routine.
//
// This function is INTERNAL to the allocator stack -- direct callers
// outside gvm_heap.inl get an untagged block (kind = Other in the
// best case; garbage in introspection in the worst).  Use gvm_alloc
// instead.
template<typename T>
[[nodiscard]] T *vm_alloc_global_ptr(vm_size_t size = vm_sizeof<T>()) {
    if (m_vm_alloc_active != nullptr) {
        error(Error::InternalError, "vm_alloc_global: dynamic allocation in progress");
    } else {
        auto raw = reinterpret_cast<uintptr_t>(m_vm_global) - size;
        auto aligned = raw & ~uintptr_t{alignof(T) - 1};
        auto new_global = reinterpret_cast<vm_t *>(aligned);
        if (new_global < m_vm_ptr) {
            error(Error::VMFull, "global region exhausted (cannot allocate {} bytes)", size);
        } else {
            m_vm_global = new_global;
            // Clamp temp watermark so future temp allocations start below
            // the new global region.  m_vm_temp_ptr serves a dual role:
            //
            //   (a) the lowest occupied byte of temp DATA when an op is
            //       actively using temp scratch (sort-by, group-by,
            //       set-difference, etc.), AND
            //   (b) a "headroom limit" pinned to m_vm_global by
            //       save.inl::restore() so future temp allocations stay
            //       below globals.
            //
            // Under (b) -- the steady state when no op is mid-temp-use
            // -- this clamp is benign and load-bearing (the scanner
            // depends on it to interleave global ${...} allocations
            // with the post-restore baseline).  Under (a) the clamp
            // CAN silently corrupt: if in-use temp data sits at
            // [old_temp_ptr, m_vm_limit) AND new_global lands below
            // old_temp_ptr, the global block at [new_global, old_global)
            // overlaps the temp slice from new_global to old_global.
            //
            // The callers that DO keep temp live across user code or across their
            // own clones -- sort-by, group-by, partition (key/element scratch
            // persists across every proc invocation), and the set-algebra /
            // packed-expansion / frequencies ops (temp dicts and packed-expansion
            // buffers held across element clones) -- defuse this by SUPPRESSING
            // global allocation (m_curr_alloc_global = false) for the duration:
            // with no global bump, m_vm_global never moves, so the clamp above never
            // fires and cannot corrupt the live temp slice.  Those ops therefore run
            // fully-local under ${...} (producing local results, the tracked region-
            // inconsistency) -- see ops_array_iteration.inl / ops_array.inl and
            // object.inl::expand_packed_to_temp.  A future vm_temp_alloc caller that
            // keeps temp live across user-driven global allocation WITHOUT this
            // suppression would hit the hazard; gate it the same way (or convert this
            // clamp to an error guarded by an "active temp" flag).
            if (m_vm_temp_ptr > new_global) {
                m_vm_temp_ptr = new_global;
            }
            return reinterpret_cast<T *>(new_global);
        }
    }
}

// is_global: address-range encoding of global-region membership.  An offset
// is "global" iff its absolute address is at-or-above m_vm_global.  No header
// bit is consumed; the predicate evaluates in two loads + a compare.
[[nodiscard]] bool is_global(vm_offset_t off) const {
    return (off >= static_cast<vm_offset_t>(m_vm_global - m_vm_base));
}
public:
// --- Stack bounds-checking (part of the user-operator API) ---
// The sanctioned safety checks for a host user-op that leaves results on, or
// reaches into, a stack: validate room/depth before a push/pop.  Operators are
// dispatched with no reserved headroom (object.inl operator_execute), so an
// unchecked net push can overrun the operand stack.
[[nodiscard]] bool has_op_capacity(stack_depth_t count) {
    return ((m_op_ptr + count) < m_op_limit);
}

void require_op_capacity(integer_t count) {
    auto available = static_cast<integer_t>(m_op_limit - m_op_ptr - 1);
    if (count > available) {
        const char *plural = (count == 1) ? "" : "s";
        error(Error::OpStackOverflow, "Operand Stack does not have capacity for {} Object{}", count, plural);
    } else {
        auto post_push = (m_op_ptr + count);
        if (post_push > m_op_high_water_ptr) {
            m_op_high_water_ptr = post_push;
        }
    }
}

// GC-root stack capacity guard, mirroring require_op_capacity.  The GC-root stack
// (m_gc_roots_*, carved in init.inl, scanned by walk_all_roots) holds temporary GC
// roots while an operator builds a global VM object across allocations that can
// fire vm_global_gc (make_curry_pair / make_lazy / make_clone of an ExtValue, ...).
// Usage mirrors the operand stack: guard once with require_gc_root_capacity(n),
// then raw-push `*++m_gc_roots_ptr = obj` (capture `auto *r = m_gc_roots_ptr` for a
// rolling accumulator).  The roots are operator-scoped temporaries: the operator
// resets the stack to empty (`m_gc_roots_ptr = m_gc_roots_base - 1`) at its tail,
// on every path that pushed -- early-exit paths that never pushed leave it alone.
// The stack must be empty at every operator boundary; that invariant is what lets
// a later restore + allocation run without walking a dangling root.  The throw
// path is covered centrally: error() (ops_system.inl) resets the stack before it
// runs try_catch_handler / global_handler, so an unwound operator's roots never
// survive.  The interpreter dispatch loop does NOT reset per iteration -- only
// operators that use the stack pay for clearing it.  Bounded chain-builders use
// this; loops re-stamp one rolling slot rather than rooting per iteration;
// unbounded-recursive operators (copy-term) stay on the operand stack.  Overflow
// is a catchable /limit-check, never a corrupting write.
// Initial guard: called ONCE at the start of an operator's rooting, when the
// gc-root stack is empty.  Asserts empty first -- this catches the one leak
// reset_gc_root()'s exact-== cannot: an operator that pushed roots but never
// reset, whose stale roots would otherwise be inherited silently here.  An
// operator that guards in phases (a common N up front, then M more in a branch)
// uses require_gc_root_capacity_more() for the second+ guard, which skips the
// empty assert (the stack legitimately holds this operator's earlier roots).
// assert() compiles out under NDEBUG, so the release body is just the cap check.
void require_gc_root_capacity(integer_t count) {
    assert((m_gc_roots_ptr == (m_gc_roots_base - 1)) &&
           "gc-root stack not empty at operator rooting start (a prior operator leaked roots)");

    require_gc_root_capacity_more(count);
}

// Incremental guard: reserve `count` ADDITIONAL roots mid-rooting, without the
// empty-stack assert.  reset_gc_root(total) still validates the full push count
// at the tail, so a miscount across the combined guards is still caught.
void require_gc_root_capacity_more(integer_t count) {
    auto available = static_cast<integer_t>(m_gc_roots_limit - m_gc_roots_ptr - 1);
    if (count > available) {
        error(Error::LimitCheck, "GC root stack does not have capacity for {} root(s) (max {})", count, MaxGcRootDepth);
    }
}

// Clear the GC-root stack at an operator's tail, validating (debug only) that
// exactly `count` roots were pushed since the stack was last empty.  Bookends
// require_gc_root_capacity(count) and turns a push/reset miscount -- the easy
// mistake when hand-rooting a chain -- into a loud assert at the offending
// operator instead of a silent leak or a stale root carried into the next
// dispatch.  Exact-== is correct because using operators always begin with an
// empty stack (they do not nest with live roots; an operator that ever needs to
// would save/restore m_gc_roots_ptr by hand rather than call this).  assert()
// compiles out under NDEBUG, so the release body is just the pointer reset.
void reset_gc_root([[maybe_unused]] integer_t count) {
    assert((m_gc_roots_ptr - (m_gc_roots_base - 1)) == count && "gc-root reset count does not match the number pushed");

    m_gc_roots_ptr = m_gc_roots_base - 1;
}

// One-off single-Object root for the common "keep this Object alive across one
// allocation, then drop it" case (e.g. cloning a borrowed value while its source is
// off-stack).  Sugar over the gc-root stack's n==1 path: gc_root_push_oneoff asserts
// the stack is empty first (so no prior leak is silently inherited), gc_root_pop_oneoff
// asserts exactly one was pushed -- the two checks bookend a single quick use.  Reach
// for require_gc_root_capacity/reset_gc_root directly when rooting more than one Object
// or holding roots across a multi-step build.
//
// Templated on the argument like vm_heap.inl's scratch_* helpers: this file is
// included before object.inl, so a non-template `Object` parameter would not yet have
// a complete type.  The template defers instantiation to the call sites (in ops_*.inl,
// where Object is complete).  T is always Object in practice.
template<typename T>
void gc_root_push_oneoff(T obj) {
    require_gc_root_capacity(1);
    *++m_gc_roots_ptr = obj;
}
void gc_root_pop_oneoff() {
    reset_gc_root(1);
}

// Pop the top `n` roots WITHOUT resetting the whole stack to empty.  For a nested
// helper that reserved roots with require_gc_root_capacity_more and must leave the
// caller's roots in place (reset_gc_root, which validates a from-empty count and
// resets to empty, would clobber them).  Debug-asserts the pop does not underflow
// past the stack base.
void gc_root_pop_n([[maybe_unused]] integer_t n) {
    assert((m_gc_roots_ptr - (m_gc_roots_base - 1)) >= n && "gc-root pop underflow");

    m_gc_roots_ptr -= n;
}

// Parameter type is integer_t (int32_t), not stack_depth_t (uint16_t): callers
// frequently pass expressions derived from user-supplied integer_t counts
// (e.g. `n + 1` where n can be 65535) and narrowing to uint16_t before the
// comparison silently truncates the value, letting overflowing counts pass
// the check.  Evaluate in int32_t throughout; no caller passes a count that
// could legitimately exceed int32_t_max.
void require_op_count(integer_t count) {
    auto actual_count = static_cast<integer_t>(m_op_ptr - m_op_base + 1);
    if (actual_count < count) {
        error(Error::OpStackUnderflow,
              "insufficient number of Objects on Operand Stack: {} expected, {} available",
              count,
              actual_count);
    }
}

[[nodiscard]] stack_depth_t op_count() const {
    return static_cast<stack_depth_t>(m_op_ptr - m_op_base + 1);
}

void require_dict_capacity(integer_t count) {
    auto available = static_cast<integer_t>(m_dict_limit - m_dict_ptr - 1);
    if (count > available) {
        const char *plural = (count == 1) ? "" : "s";
        error(Error::DictStackOverflow, "Dictionary Stack does not have capacity for {} Object{}", count, plural);
    } else {
        auto post_push = (m_dict_ptr + count);
        if (post_push > m_dict_high_water_ptr) {
            m_dict_high_water_ptr = post_push;
        }
    }
}

[[nodiscard]] bool has_exec_capacity(stack_depth_t count) {
    return ((m_exec_ptr + count) < m_exec_limit);
}

void require_exec_capacity(integer_t count) {
    auto available = static_cast<integer_t>(m_exec_limit - m_exec_ptr - 1);
    if (count > available) {
        const char *plural = (count == 1) ? "" : "s";
        error(Error::ExecStackOverflow, "Execution Stack does not have capacity for {} Object{}", count, plural);
    } else {
        auto post_push = (m_exec_ptr + count);
        if (post_push > m_exec_high_water_ptr) {
            m_exec_high_water_ptr = post_push;
        }
    }
}

[[nodiscard]] stack_depth_t exec_count() const {
    return static_cast<stack_depth_t>(m_exec_ptr - m_exec_base + 1);
}

void require_error_capacity(integer_t count) {
    auto available = static_cast<integer_t>(m_err_limit - m_err_ptr - 1);
    if (count > available) {
        const char *plural = (count == 1) ? "" : "s";
        error(Error::ErrStackOverflow, "Error Stack does not have capacity for {} Object{}", count, plural);
    } else {
        auto post_push = (m_err_ptr + count);
        if (post_push > m_err_high_water_ptr) {
            m_err_high_water_ptr = post_push;
        }
    }
}

void require_save_capacity(stack_depth_t count) {
    if ((m_curr_save_level + count) >= m_max_save_level) {
        error(Error::LimitCheck, "maximum save depth ({}) has been exceeded", m_max_save_level);
    }
}

void require_save_count(stack_depth_t count) {
    auto actual_count = static_cast<stack_depth_t>(m_curr_save_level + 1);
    if (actual_count < count) {
        error(Error::InvalidRestore, "insufficient active save levels: {} expected, {} available", count, actual_count);
    }
}
private:
