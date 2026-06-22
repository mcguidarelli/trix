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

//===--- Global VM Allocator (dlmalloc-derived design) ---===//
//
// This file implements Trix's per-block-tagged allocator for the GLOBAL
// VM region.  It is the destination for every byte allocated above
// `m_vm_global` -- every $/foo / $\foo / ${...} / <lit>#$ Object, every
// Name interned with `set-global true`, every Dict / Set / Array /
// String created while `current-global` is true.
//
//===--- Attribution -------------------------------------------------------//
//
// The data-structure design here -- per-block boundary tags, segregated
// free lists, in-place coalescing of adjacent free blocks, top-chunk
// advance -- comes directly from Doug Lea's research and reference
// implementation, "dlmalloc."  Lea published the original in 1987 and
// refined it for nearly three decades; the design is documented in
// his canonical writeup:
//
//     Doug Lea, "A Memory Allocator", 1996.
//     <https://gee.cs.oswego.edu/dl/html/malloc.html>
//
// Every production allocator since -- jemalloc (Jason Evans, 2006),
// tcmalloc (Sanjay Ghemawat, 2007), ptmalloc (Wolfram Gloger,
// glibc's default), mimalloc (Daan Leijen, 2019), and many others --
// is a descendant of dlmalloc, refining its bin structure or adding
// per-thread caches over the same boundary-tag + segregated-list
// foundation.  We acknowledge this lineage explicitly: when this file
// uses the words "fastbin" or "boundary tag" or "top chunk", it is
// using terminology Lea coined.
//
//===--- This is a ground-up implementation, not a port -------------------//
//
// Lea's original dlmalloc is approximately 6,000 lines of C, much of it
// devoted to concerns Trix does not have:
//
//     * mmap / munmap fall-back for very large allocations
//     * sbrk-style heap growth via the Unix data-segment break
//     * multi-arena / per-thread heap support
//     * a real-malloc-replacement ABI (malloc, free, calloc, realloc,
//       memalign, valloc, mallinfo, mallopt, malloc_stats, ...)
//     * 32-bit / 64-bit dual support with byte-perfect on-disk layout
//     * Win32 build paths, OS-X paths, embedded paths
//     * roughly thirty configuration knobs (M_MXFAST, M_TRIM_THRESHOLD,
//       M_TOP_PAD, etc.) and the malloc_state apparatus they tune
//     * decades of debugging hooks, integrity checks, fuzz armor
//
// We need NONE of that.  Trix runs in a single thread on a fixed-size
// VM allocated once at startup, with one fixed integration point
// (`vm_alloc_global_ptr` in vm_heap.inl) and four internal callers
// (Name, Dict/Set, Object array, vm_t string).  The right-sized
// implementation is the data-structure essence of Lea's design --
// boundary tags, segregated bins, coalescing -- written from first
// principles against:
//
//     * a single contiguous zone whose fixed top is `m_vm_limit` and
//       whose lower bound `m_vm_global` grows DOWNWARD (toward
//       m_vm_base) as new allocations are made;
//     * Trix's `vm_offset_t` (uint32_t) addressing convention -- every
//       free-list link, every payload reference is relative to
//       `m_vm_base`, so snapshots serialize trivially with no pointer
//       relocation;
//     * idiomatic C++23 (constexpr, [[nodiscard]], structured bindings,
//       std::span), inline in a class-scope .inl file, no separate
//       compilation unit;
//     * the existing Trix allocator-side conventions (Object 8-byte
//       alignment, vm_size_t bound by 2^32-1, error reporting via
//       `error(Error::VMFull, ...)`, no exceptions).
//
// Most of the bulk here is comment + invariant documentation; the
// executable surface is a small fraction of Lea's original.  This is
// because we deliberately do not solve any of the problems listed above.
//
//===--- Licensing ---------------------------------------------------------//
//
// Doug Lea released dlmalloc into the public domain in May 2000:
//
//     "I hope this code is useful, but make no warranty about its
//      correctness or anything else.  Use it at your own risk.
//      To the extent possible under law, I have dedicated all
//      copyright and related and neighboring rights to this software
//      to the public domain worldwide."
//
// No legal attribution clause attaches.  This file is original code
// distributed under the Trix project's Apache 2.0 license (see the
// banner above).  The attribution above is moral, not legal: it is
// the right thing to do.
//
//===--- Scope --------------------------------------------------------------//
//
// Allocation reclaims freed blocks via a sorted free list plus segregated
// fastbins (see Free lists below), with O(1) boundary-tag coalescing and
// top-edge advance on free.  There is no user-facing `free-global` op --
// frees are C++-internal only: the stop-the-world mark-sweep GC (gc.inl,
// via vm_global_gc) reclaims unreachable global blocks (gvm_free per
// swept block), and ExtValue::free / WideValue::free (object.inl) free
// their backing blocks directly.
//
// All allocator logic lives here and only here; name.inl, dict.inl,
// object.inl, and scanner.inl reach it through gvm_alloc / gvm_alloc_n /
// vm_alloc_dispatch but hold no allocator state of their own (modulo the
// C++-internal free callsites in object.inl that wire ExtValue::free /
// WideValue::free through to gvm_free).  vm_alloc_global_ptr is
// vm_heap.inl's internal bump primitive that gvm_alloc_impl calls; the
// callers above do not invoke it directly.
//
//===--- Block layout ------------------------------------------------------//
//
// Every gvm_alloc-returned offset is the address of a PAYLOAD byte.
// SIXTEEN bytes ABOVE the payload sits the GvmBlock header; four bytes
// at the END of the block hold a tail tag mirroring `m_size`.  The
// tail tag is what makes O(1) backward coalesce work: given a freed
// block, its lower neighbour's size is at `(block_start - 4)`, so we
// can find that neighbour's header in constant time.
//
//                               (high addresses)
//                       +---------------------------+ <- m_vm_limit
//                       |  ...older blocks above... |
//                       +---------------------------+
//                       |  prev block tail tag (4)  |  <- prev block's m_size
//                       +---------------------------+
//   block start   --->  |  m_size                   |  <- GvmBlock header
//                       |  m_kind | m_mark_gen |    |     (16 bytes total,
//                       |  m_obj_count              |      8-byte aligned;
//                       |  m_next_in_work           |      see struct below)
//                       |  m_magic = GvmMagic       |
//                       +---------------------------+
//   payload start --->  |  payload byte 0           |  <- offset returned
//                       |  payload byte 1           |     by gvm_alloc
//                       |          ...              |
//                       |  payload byte M-1         |
//                       |  (slack/padding)          |
//                       +---------------------------+
//                       |  tail tag = m_size  (4)   |  <- mirrors header
//                       +---------------------------+      (4-byte aligned)
//                       |  next (newer) block       |
//                       +---------------------------+
//                       |          ...              |
//                       +---------------------------+ <- m_vm_global
//                               (low addresses)
//
// Field semantics (full per-field commentary lives on the GvmBlock
// struct; this is the layout summary):
//
//     m_size      -- total block size in bytes, INCLUDING header,
//                    payload, padding, AND tail tag.  Always a
//                    multiple of BlockAlignment (8).  Stepping field
//                    for gvm_for_each.
//     m_kind      -- ChunkKind enum byte (see ChunkKind below).
//                    Bit 7 (GvmFreeFlag) marks the block as free;
//                    bits 0-6 hold the original kind for forensic
//                    context across coalesce.
//     m_mark_gen  -- GC mark generation (see Trix::m_gc_current_gen).
//     m_obj_count -- length_t user-visible element count for the
//                    handful of kinds whose GC walker reads it
//                    (Array, Packed, Curry, Thunk, Tagged, Record);
//                    0 for kinds that ignore it.
//     m_next_in_work -- intrusive GC mark-phase BFS queue link.
//                       nulloffset between GC passes.
//     m_magic     -- GvmMagic sentinel; corruption + heap-walk guard.
//     tail        -- uint32_t at offset (block_start + m_size - 4),
//                    copy of m_size.  Used by free's backward-
//                    coalesce path.
//
// Free block layout (when m_kind == ChunkKind::Free):
//
//                       +---------------------------+ <- block start
//                       |  m_size + kind=Free       |  header
//                       +---------------------------+
//   payload area  --->  |  prev_free  (vm_offset_t) |  free-list link
//                       +---------------------------+
//                       |  next_free  (vm_offset_t) |  free-list link
//                       +---------------------------+
//                       |  ...unused payload...     |  was user data when live
//                       +---------------------------+
//                       |  tail tag = m_size  (4)   |  same location/value
//                       +---------------------------+
//
// Free-list links are vm_offset_t into the global region, so the
// list survives snapshot/thaw with zero rebasing.
//
// Min block size = HeaderSize (16) + MinPayloadSize (8 for free links)
//                + TailTagSize (4) = 28, rounded up to BlockAlignment
//                = 32.  Payload requests of < 8 bytes round up to
//                32-byte blocks with up to 7 bytes of slack.
//
// The choice of 8-byte block alignment satisfies every Trix payload
// type:
//
//     * Object (alignof=8) -- Array storage cells, packed-array
//       slots, eqarray cells.  Header is 8-byte aligned and 8 bytes
//       long, so the payload starts on an 8-byte boundary;
//     * Name (alignof=4) -- m_next, m_binding, m_hash, m_length,
//       m_data[].
//     * Dict (alignof=4) -- m_pool, m_length, m_maxlength,
//       m_bucket_count, m_access, m_dict_save_level, then the
//       buckets[] tail.
//     * vm_t (alignof=1) -- string bytes. (over-aligned, but the
//       8-byte rounding-up costs at most 7 bytes per global string,
//       in exchange for a cleaner block walk).
//
//===--- Boundary tags ----------------------------------------------------//
//
// The boundary-tag idea is the cornerstone trick of Lea's allocator:
// every block carries its size at BOTH ends, so coalescing a
// freshly-freed block with either neighbour is O(1).  We follow the
// classic design: a 4-byte tail tag mirroring `m_size` at the end of
// every block (live or free).  Given a block at offset O, its
// previous block's size is at offset (O - 4), so the previous
// header is at offset (O - prev_size) -- one read to find it, one
// flag-check to test whether to coalesce.
//
// Trix simplifies vs Lea's original by NOT encoding "previous is
// free" in the LSB of `prev_size`.  Instead we always read the
// previous header's `m_kind` to test against ChunkKind::Free.  The
// cost is one extra cache line per backward-coalesce vs Lea's bit
// trick; the simplicity is worth it given Trix's allocation rates.
//
//===--- Free lists -------------------------------------------------------//
//
// Two structures hold free blocks; routing is by size:
//
//   Fastbins (m_gvm_fastbins[GvmFastBinCount]):
//       4 doubly-linked LIFO stacks for sizes 32, 40, 48, 56.
//       Insert: O(1) push at head; unlink: O(1) via prev/next links.
//       Lookup: exclusive-match (size N goes ONLY to bin N; no
//       bin-skip search).  Threshold is GvmFastBinThreshold (= 64).
//
//   General free list (m_gvm_free_head):
//       Doubly-linked list sorted ASCENDING by block offset for
//       blocks of size >= GvmFastBinThreshold.  Insert: O(N) walk
//       to find the right slot; remove: O(1); first-fit search
//       during alloc: O(N).
//
// gvm_free_insert_anywhere / gvm_free_unlink_anywhere are the size-
// routed dispatchers; allocator + coalesce paths call those rather
// than touching the structure-specific functions directly.
//
// Free-list links live in PAYLOAD memory of free blocks (only valid
// when GvmFreeFlag is set on m_kind), not in the header.  Layout:
// prev_free at payload[0..3], next_free at payload[4..7].  Min payload
// area for a free block is 8 bytes; with the 16-byte header + 4-byte
// tail tag, min block size is 28 -> rounded to BlockAlignment = 32.
//
// Top-edge advance: when freeing produces a block whose header sits
// exactly at `m_vm_global`, we advance `m_vm_global` upward by the
// block's size and DON'T insert the block into the free list.  This
// reclaims contiguous tops back into the unallocated region (so
// `vm-global-used` shrinks).  Without this, a long-running program
// with churning globals would never see `m_vm_global` move back up
// even when the high end is entirely free.
//
//===--- Snapshot/thaw ----------------------------------------------------//
//
// Snapshot serialization (`ops_snapshot.inl`) treats the global
// region as an opaque byte blob and writes [m_vm_global, m_vm_limit)
// verbatim into the snapshot file, with `vm_global_used = m_vm_limit
// - m_vm_global` recorded in the SnapShotHeader.  Thaw reads the blob
// back into the same address range at the top of the destination
// VM.
//
// The allocator's headers and (eventually) free-list links are all
// expressed as `vm_offset_t` -- relative to `m_vm_base`.  The
// classical reason dlmalloc trips up on serialization (free-list
// links are absolute pointers that must be relocated on load)
// simply does not apply.  Snapshots remain a verbatim byte copy.
//
// Caveat: the per-block header layout is part of the snapshot wire
// format.  SNAPSHOT_VERSION gates it -- an older binary rejects a
// newer snapshot at load time, as it does across every version bump.
// See `types.inl` for the version constant.
//
//===--- Integration contract with vm_heap.inl ----------------------------//
//
// The split of responsibilities is:
//
//     vm_heap.inl owns:
//         m_vm_base, m_vm_ptr, m_vm_temp_ptr, m_vm_global, m_vm_limit
//         the bump primitive vm_alloc_global_ptr<T>(size)
//         the address-range predicate is_global(offset)
//         the local-region allocators (vm_alloc, vm_temp_alloc)
//
//     gvm_heap.inl owns:
//         the GvmBlock header layout
//         the ChunkKind classification
//         the gvm_alloc / gvm_alloc_n entry points
//         the gvm_for_each block walk
//         the gvm_kind_name string table for introspection
//
//     callers (name.inl, dict.inl, object.inl, scanner.inl):
//         pass an explicit ChunkKind at every gvm_alloc callsite
//         operate on the returned PAYLOAD offset -- the existence of
//         the header is invisible to them
//
// gvm_heap.inl never writes m_vm_ptr / m_vm_temp_ptr; it only READS
// m_vm_ptr to verify a downward global bump will not collide with the
// upward-growing local region, consumes m_vm_global / m_vm_limit for
// walks, and calls vm_alloc_global_ptr for forward growth.  This keeps
// the local-region allocator free to evolve independently (e.g. the
// future generational rewrite) without touching globals.
//
// Single-threaded.  Trix is one thread per Trix object; concurrent
// alloc/free from different Trix instances is safe because each owns
// its own m_vm_global.  The allocator does not lock or atomic.
//
// No exceptions.  Every error path goes through `error(...)`, which
// raises a Trix error (longjmp/setjmp under the hood) and never
// returns.
//
//===-------------------------------------------------------------------===//

//
// ChunkKind: classification tag for global-VM allocations.
//
// Stored as one byte in the GvmBlock header.  Set explicitly by the
// caller of gvm_alloc / gvm_alloc_n -- every callsite knows what kind
// of object it is constructing (the scanner knows its #$ target was a
// string; Name::add knows it is making a Name; create_global_set
// passes Set even though the C++ type is Dict).
//
// The values are STABLE.  Introspection ops, snapshot tooling, and
// future telemetry depend on these byte values, so DO NOT renumber
// existing entries.  Append-only.  When adding a new kind, also extend
// gvm_kind_name() and bump MaxChunkKind below.
//
// Adding a new ChunkKind is a four-step procedure:
//   1. Add the enumerator HERE and bump MaxChunkKind if it extends the range.
//   2. Add the case to gvm_kind_name() (human-readable label for
//      `vm-global-info`'s /by-kind histogram).
//   3. Add the case to gc.inl::walk_block_contents (dispatches to the
//      per-subsystem walker; the switch has NO default clause, so a
//      missing case is a -Wswitch-enum compile-time error).
//   4. Implement the per-subsystem walker next to the struct definition
//      (e.g., Dict::gc_walk_contents in dict.inl) -- enumerate every
//      Object cell in the payload and call gc_mark_object on each.  Leaves
//      with no Object cells declare `// leaf -- no walker needed` and
//      step 3 emits a bare `return;`.
// Skipping any of these is a runtime correctness bug.  The hard-fail
// safety net is the missing-case compile error from step 3.
//
// The enum covers the full Trix Object-type taxonomy that could
// plausibly land in the global region -- not just the kinds with a
// live caller today.  Reserving the slots up front keeps
// later additions cheap (no MaxChunkKind+ tooling churn) and gives
// users a stable per-kind /by-kind schema regardless of which
// kinds happen to be populated.  Kinds with no caller today report
// count=0; populating them is the caller's job when migration
// happens.
//
// Naming follows Trix's `type_sv()` shorthand (the "-type" suffix
// dropped), so `Long` is "long" (not "long-type"), `PipeBuffer` is
// "pipebuffer" (not "pipe-buffer", matching `pipebuffer-type`),
// etc.  Kept in sync with src/object.inl::type_sv().
//
enum class ChunkKind : uint8_t {
    // Active kinds -- every entry below has a live gvm_alloc caller and
    // reports > 0 in /by-kind whenever the corresponding type is
    // allocated inside a `${...}` / `set-global true` window.
    Other = 0,              // SENTINEL ONLY -- never a valid gvm_alloc tag (gvm_alloc asserts against it).
                            // Used as the "no heap kind" default in object.inl::expected_chunk_kind_for
                            // for non-heap-backed Types.  Every real allocation names a specific kind.
    Name = 1,               // Name struct from Name::add (interned global name)
    Dict = 2,               // Dict (header + buckets + entry pool) from Dict::create_global_dict
    Set = 3,                // Dict-with-SetFlag from Dict::create_global_set
    Array = 4,              // Object[] backing for global Trix Arrays
    String = 5,             // vm_t[] backing for global Trix Strings
    Long = 6,               // ExtValue backing for 64-bit signed integer
    ULong = 7,              // ExtValue backing for 64-bit unsigned integer
    Address = 8,            // ExtValue backing for raw memory address
    Double = 9,             // ExtValue backing for 64-bit IEEE-754
    Int128 = 10,            // WideValue backing for 128-bit signed integer
    UInt128 = 11,           // WideValue backing for 128-bit unsigned integer
    Packed = 12,            // vm_t[] packed-array body (compressed proc/array encoding)
    Curry = 14,             // Curry struct (partially-applied proc) -- skips 13 because Stream = 13 (RETIRED, see below)
    Thunk = 15,             // Thunk struct (deferred evaluation cell)
    Tagged = 16,            // Tagged struct (tag + payload pair)
    Record = 17,            // Record schema or instance backing (single bucket)
    CoroutineStacks = 18,   // per-coroutine contiguous stack block (op + exec + err + dict + scratch); >= 3328 bytes
    PipeBuffer = 19,        // PipeBufferHeader + Object data array for pipeline buffers
    Mailbox = 23,           // MailboxHeader for actor mailboxes
    Monitor = 24,           // MonitorEntry for actor monitor/link relationships
    BindingBucket = 25,     // BindingBucketCount-sized vm_offset_t array (per-coroutine Name binding cache)
    BindingEntry = 26,      // BindingEntry struct (single binding cache slot)
    Supervisor = 27,        // SupervisorState header + ChildEntry array + timestamps
    CoroutineContext = 28,  // CoroutineContext fixed-shape struct (~236 bytes)
    GcScratch = 29,         // GC's lazy scratch block: holds m_gc_local_visited (the local-VM cycle-break offset
                            // set, default 32 KiB).  Allocated on the first user gvm_alloc so it sits at the top
                            // of global VM, above all user blocks; freed when the last user block is freed and
                            // m_gvm_user_block_count returns to zero.  Walker is a no-op (payload is GC-private
                            // raw offsets, not user Objects).  Backs the temp-region squeeze fix on top of the
                            // alloc-honor audit.
    HashEntry = 30,         // standalone Dict/Set entry block from put_persist_or_create /
                            // set_add_persist_or_create.  The -persist family allocates ONE
                            // entry at a time outside the dict's own pool (so the binding
                            // survives restore, journal-skipped).  Leaf walker: the entry's
                            // m_key/m_value are marked by the owning Dict/Set's bucket walk
                            // (Dict::gc_walk_contents), the ONLY path that reaches it; this
                            // block just needs a real kind so the back-walk can keep it alive.

    // Retired kinds -- types whose architecture or design intent rules
    // out global VM allocation.  These slots stay count=0 forever; they
    // remain in the schema only for forensic visibility (so /by-kind
    // returns a complete histogram).
    //
    //   Stream       -- backing struct lives in a fixed-capacity init-time
    //                   pool (stream.inl); recycled via alloc_stream /
    //                   free_stream.  Already survives save/restore via
    //                   the pool architecture.
    //   Cell         -- ops_reactive.inl explicitly documents cells as
    //                   non-persistent; recommended pattern is
    //                   `put-persist` on a plain dict.  Migrating
    //                   contradicts the design.
    //   Continuation -- ContinuationContext is a self-contained value
    //                   copy (memcpy at capture time; no live stack
    //                   pointers).  Capture save level is stamped on
    //                   the Object handle (Object::m_continuation_save_level),
    //                   so execute_continuation (interpreter.inl:705)
    //                   already raises a clean InvalidAccess on stale
    //                   resume after restore.  Migrating adds nothing
    //                   user-observable; true cross-restore continuations
    //                   are a separate feature, not a routing migration.
    Stream = 13,        // RETIRED -- pool-allocated in stream.inl
    Cell = 20,          // reactive cell header; region-aware, walked by cell_gc_walk (ops_reactive.inl)
    Continuation = 21,  // RETIRED -- handle-side dangle check already correct

    // Reserved kinds -- slots awaiting a revived-by-future-migration
    // caller.  CURRENTLY count=0, but unlike RETIRED the design
    // intent is that a caller WILL appear.
    //
    //   Screen -- cross-coroutine ownership of a TTY descriptor needs
    //             design work beyond allocator routing.  Probably defer
    //             indefinitely.
    Screen = 22,  // RESERVED -- probably defer

    // Sentinel kind -- not a user-allocable kind.  Returned by
    // gvm_get_kind for any block whose m_kind has GvmFreeFlag set.
    // Out of the iterated [0, MaxChunkKind) range so `vm-global-info`'s
    // `/by-kind` histogram excludes it (free blocks are reported via the
    // top-level `/free-blocks` and `/free-bytes` instead).
    //
    // Wire format: freed blocks carry their
    // ORIGINAL ChunkKind in the low 7 bits of m_kind, with the
    // GvmFreeFlag (0x80) bit set in bit 7 -- this preserves forensic
    // information about what kind of block was at that address before
    // it was freed (useful when stale Object refs point at a reissued
    // block and we want to know what was there).  gvm_get_kind() masks
    // the low 7 bits and reports Free for freed blocks (back-compat
    // with older callers that switch on it).  gvm_get_freed_kind()
    // returns the original kind regardless of free state.  Older
    // snapshots use 0xFF as the freed marker; they remain valid
    // because (0xFF & GvmFreeFlag) == GvmFreeFlag tests true.
    Free = 0xFF,
};
static constexpr uint8_t MaxChunkKind = 31;  // exclusive upper bound; size of gvm_kind_name table

// GvmFreeFlag: bit 7 of GvmBlock::m_kind, set on freed blocks while
// the original kind is preserved in bits 0-6.  See ChunkKind::Free
// comment above for the rationale + back-compat note.
static constexpr uint8_t GvmFreeFlag = 0x80;
static_assert(MaxChunkKind <= GvmFreeFlag, "ChunkKind values must fit in 7 bits so GvmFreeFlag (bit 7) is unambiguous");

// gvm_kind_is_free: predicate for testing freed-ness from a raw
// ChunkKind value (e.g., header field access without going through
// the offset-based gvm_block_is_free path).  Tests both the new
// "(orig | GvmFreeFlag)" encoding and the legacy ChunkKind::Free
// (0xFF) sentinel -- both have bit 7 set.
[[nodiscard]] static constexpr bool gvm_kind_is_free(ChunkKind kind) {
    return ((+kind & GvmFreeFlag) != 0);
}

//
// GvmBlock: 16-byte block header.
//
// Sits immediately before every payload returned by gvm_alloc.  The
// caller's offset arithmetic is unchanged (Trix Object refs continue
// to use the payload offset); only the introspection walk and the
// free-list machinery touch this struct.
//
// Layout (little-endian, but layout is independent of endianness
// because every field is read through its own type):
//
//     bytes  0-3  : m_size          (uint32_t, total block size, multiple of 8)
//     byte   4    : m_kind          (ChunkKind, classification tag)
//     byte   5    : m_mark_gen      (uint8_t, GC mark generation -- see below)
//     bytes  6-7  : m_obj_count     (length_t, user-visible Object count for Object[]-payload kinds)
//     bytes  8-11 : m_next_in_work  (vm_offset_t, intrusive GC mark/sweep queue link; nulloffset off-queue)
//     bytes 12-15 : m_magic         (uint32_t, GvmMagic sentinel; corruption + heap-walk validation)
//
// Total = 16 bytes.  Every block lands on an 8-byte boundary by
// construction, so loads of m_size are always aligned.
//
// m_next_in_work is the intrusive link used by gc.inl during the mark
// phase (BFS work queue) and the sweep phase (doomed list).  Both
// phases run at GC time, in disjoint sub-phases of the same pass; the
// field is nulloffset between passes.  Threading the queue through the
// blocks themselves guarantees enough storage for any total_live by
// construction (each live block contributes exactly one queue slot).
//
// m_magic is a fixed sentinel (GvmMagic) stamped on every alloc and
// preserved across free/coalesce.  Two uses: gvm_for_each forward-walk
// validation (catches corruption that overwrites the header with
// non-header bytes), and gvm_find_owning_payload backward walks
// (validates "this candidate is actually a header" alongside the
// existing size + tail-tag check, making spurious owner identification
// effectively impossible).
//
// m_mark_gen is the per-block GC mark generation.  gc.inl::vm_global_gc
// flips Trix::m_gc_current_gen (a 1-bit flip-flop) each pass; a block is
// "alive in this pass" iff `header->m_mark_gen == m_gc_current_gen`.
// Flipping the bit makes every old mark stale by definition, so there is
// no per-pass clear walk -- ever.  See member_vars.inl::m_gc_current_gen
// and gc.inl::gc_advance_generation.
//
struct GvmBlock {
    uint32_t m_size;             // total bytes including header + payload + padding + tail tag (multiple of BlockAlignment)
    ChunkKind m_kind;            // classification (see ChunkKind enum above); ChunkKind::Free indicates a free block
    uint8_t m_mark_gen;          // GC mark generation (see Trix::m_gc_current_gen); fresh-alloc value: gc_init_mark_for_alloc()
    length_t m_obj_count;        // kind-specific user-visible-element count; zeroed by gvm_alloc on every allocation.
                                 //   ChunkKind::Packed: user-visible element count (the GC Packed
                                 //     walker uses it as the decode bound, since the byte stream
                                 //     has no self-terminator and trailing slack would otherwise
                                 //     mis-decode as malformed elements).  Stamped after gvm_alloc
                                 //     by make_packed_data in object.inl.
                                 //   ChunkKind::Array: user-visible Object count.
                                 //     Stamped after gvm_alloc_n<Object> by every Array alloc site.
                                 //     Eliminates GC slack-walking on Object[] payloads.
                                 //   ChunkKind::Record (RecordInstance): user-visible field count.
                                 //     RecordSchema does NOT stamp -- its field count lives in the
                                 //     first 2 bytes of payload; walker disambiguates by checking
                                 //     whether first 4 bytes look like a global-VM offset.
                                 //   All other kinds: unused; remains 0.
    vm_offset_t m_next_in_work;  // intrusive GC queue link (mark-phase BFS queue / sweep-phase doomed list).
                                 //   nulloffset between GC passes and on every off-queue block.
                                 //   See gc.inl::gc_work_push / gc_work_pop.
    uint32_t m_magic;            // GvmMagic sentinel; stamped on every alloc, preserved across free/coalesce.
                                 //   Used by gvm_for_each (forward-walk corruption check) and
                                 //   gvm_find_owning_payload (backward-walk header validation).
};
static_assert(sizeof(GvmBlock) == 16, "GvmBlock header must be exactly 16 bytes");
static_assert(alignof(GvmBlock) <= 8, "GvmBlock alignment must not exceed BlockAlignment");

// Magic sentinel stamped in every GvmBlock::m_magic.  Properties:
//   * Every byte >= 0x80, so it can't collide with ASCII payload runs
//     during gvm_find_owning_payload's backward header scan.
//   * Far above the upper bound of legitimate m_size (a few MB at most),
//     so it can't be mistaken for a size field.
//   * Not 8-byte aligned (low byte is 0xDE), so it can't be mistaken
//     for a vm_offset_t pointer.
//   * Instantly recognisable in a hex dump.
//
// Note on the 0x80-0xFF range: those bytes are ALSO Trix's binary-token
// vocabulary (see enums.inl::BinaryToken).  In particular 0xAD = Null,
// 0xC0 = SystemExecValue_16b, and 0xDE = Real_1 are valid tokens that
// can appear inside packed proc bodies, hex strings, byte arrays, etc.
// User-controlled data can therefore contain the bytes 0xDEADC0DE in
// arbitrary positions.  This is harmless because m_magic is only ONE
// of four independent validation fields checked by
// gvm_find_owning_payload:
//
//     1. m_kind  matches the expected_kind passed by the caller.
//        Caller-supplied; a Packed back-scan won't be fooled by a
//        String/Dict header pattern, and vice versa.
//     2. m_size  is positive, 8-aligned, and within the heap.  A
//        fake header would need a coincidentally-valid size.
//     3. m_magic equals GvmMagic.  This is the cheap early-out:
//        most random 4-byte windows fail here without paying for
//        the tail-tag dereference below.
//     4. The 4 bytes at (candidate + m_size - 4) equal m_size
//        (the tail-tag invariant -- see gvm_set_tail_tag).
//
// Combined false-positive odds:
//     (1/256) * (1/2^32) * (1/2^32) ~= 5e-23.
// Even with adversarial user-controlled bytes in the payload, the
// walker cannot be fooled by chance.  Test regression coverage for
// this claim lives in tests/test_gvm_heap.trx section 9.
//
// Do NOT weaken any of the four fields without re-deriving this
// probability calculation.  Removing the tail-tag check "as
// redundant" or relaxing the kind match "for flexibility" would
// reduce the four-way independence and risk real false positives.
// If a future change wants to reduce header bytes, the right move
// is to merge fields (e.g. pack size + kind) while preserving the
// independence of each validation step, NOT to drop steps.
static constexpr uint32_t GvmMagic = 0xDEADC0DEu;

//
// gc_init_mark_for_alloc(): the m_mark_gen value to stamp on every
// freshly-allocated LIVE block.  Returns the CURRENT m_gc_current_gen
// (the pre-flip value).
//
// m_gc_current_gen is a 1-bit flip-flop.  The next GC/probe pass flips
// it before marking (gc_advance_generation), so a block stamped with
// the current value looks "old" the moment that pass starts -- eligible
// for sweep unless reached by walk_all_roots / walk_block_contents.
// The block lifecycle is uniform: every block, fresh or surviving, is
// "old" at the start of every pass; the mark phase promotes reached
// blocks to the new current_gen.  At boot (m_gc_current_gen == 0) no GC
// has run; fresh blocks stamp 0 and the first pass flips to 1, so they
// look old -- no boot special-case needed.
//
[[nodiscard]] uint8_t gc_init_mark_for_alloc() const {
    return m_gc_current_gen;
}

//
// GvmFreeLinks: free-list links overlaid on a free block's payload area.
//
// Only valid when the block's header has m_kind == ChunkKind::Free.
// Stored as the FIRST 8 bytes of the payload region (offset HeaderSize
// from the block start).  When the block is allocated (live), these
// bytes are user-payload data.
//
struct GvmFreeLinks {
    vm_offset_t prev_free;  // ascending-order prev neighbour in free list, or nulloffset for head
    vm_offset_t next_free;  // ascending-order next neighbour in free list, or nulloffset for tail
};
static_assert(sizeof(GvmFreeLinks) == 8, "GvmFreeLinks must be exactly 8 bytes (vm_offset_t pair)");

//
// Allocator constants.
//
// BlockAlignment is the granularity at which the global region's
// `m_vm_global` watermark moves.  Every block size is rounded UP to
// this alignment, so consecutive blocks abut without gaps and the
// gvm_for_each walk can step header-by-header without padding probes.
//
// HeaderSize is sizeof(GvmBlock).  TailTagSize is sizeof(uint32_t).
// MinPayloadSize is the minimum bytes a payload area must reserve so
// that a freed block can host its prev_free / next_free links.
// MinBlockSize is the smallest block we'll ever allocate -- payload
// requests below MinPayloadSize round up to it.
//
static constexpr vm_size_t GvmBlockAlignment = 8;
static constexpr vm_size_t GvmHeaderSize = static_cast<vm_size_t>(sizeof(GvmBlock));
static constexpr vm_size_t GvmTailTagSize = static_cast<vm_size_t>(sizeof(uint32_t));
static constexpr vm_size_t GvmMinPayloadSize = static_cast<vm_size_t>(sizeof(GvmFreeLinks));
static constexpr vm_size_t GvmMinBlockSize =
        ((GvmHeaderSize + GvmMinPayloadSize + GvmTailTagSize) + (GvmBlockAlignment - 1)) & ~(GvmBlockAlignment - 1);
static_assert(GvmMinBlockSize == 32, "Min block size = 32 bytes (16 header + 8 links + 4 tail tag, padded to 8-byte alignment)");

//
// Segregated fastbins for small block sizes.
//
// Bins partition the smallest free-block sizes into per-size LIFO
// stacks for O(1) alloc/free.  Block sizes >= GvmFastBinThreshold
// continue to use the sorted-by-address general free list.
//
// bin[0] = 32-byte blocks   (Long / SetEntry payloads after 16B header growth)
// bin[1] = 40-byte blocks   (Int128 / WideValue / DictEntry / ExtValue payloads)
// bin[2] = 48-byte blocks
// bin[3] = 56-byte blocks
//
// 64-byte and larger blocks live in the general list.  64 is exclusive:
// a 64-byte block does NOT live in a bin.
//
// Bin head pointers live in m_gvm_fastbins[]; each bin is a doubly-
// linked LIFO using the existing GvmFreeLinks payload overlay (insert
// at head, unlink in O(1) for coalesce).
//
static constexpr length_t GvmFastBinCount = 4;
static constexpr vm_size_t GvmFastBinThreshold = GvmMinBlockSize + GvmFastBinCount * GvmBlockAlignment;
static_assert(GvmFastBinThreshold == 64, "GvmFastBinThreshold should be 64 (32 min + 4*8)");

// (m_kind == ChunkKind::Free is the single source of truth for the
// free/live state of a block.  When kind == Free, the block's payload
// area is reinterpreted as GvmFreeLinks; otherwise the payload is user
// data tagged by the live ChunkKind.)

//
// gvm_alloc<T>(payload_size, kind):  allocate `payload_size` bytes
// of payload from the global region, with the block tagged `kind`.
//
// The returned vm_offset_t points at the FIRST PAYLOAD byte (NOT the
// header) -- callers store this offset in Object cells exactly as they
// would for any other allocation; the header is invisible to them.
//
// Guarantees:
//   * payload is aligned to GvmBlockAlignment (8 bytes);
//   * total block size (header + payload + padding) is a multiple of
//     GvmBlockAlignment; consecutive blocks abut without gaps;
//   * the block is "live" -- m_kind is the caller's tag; future free will set m_kind = ChunkKind::Free;
//   * raises Error::VMFull if the global region cannot grow downward
//     by the rounded-up total size; raises Error::LimitCheck if the
//     payload size would overflow vm_size_t after header rounding.
//
// This is the workhorse routine: every global Trix allocation goes
// through here.  Callers that allocate `count` elements of T should
// use gvm_alloc_n<T>(count, kind), which also overflow-checks the
// multiplication.
//
// Lazy GC scratch sizing.  Initial visited-set capacity for the
// auxiliary cycle-break table (Tagged/Curry/Thunk/Array/Record/Packed
// local containers; Dict/Set use the in-Dict/Set link instead).  Sized
// small (1024 entries = 4 KiB) because typical visited-counts for these
// 6 types are well under that.  On overflow, grow_gc_local_visited
// doubles the scratch block in place (gc.inl), so this is a starting
// size, not a hard ceiling.
//
// Layout in scratch payload (vm_offset_t indices):
//   [0 .. capacity)  -- visited set
static constexpr vm_size_t GcScratchVisitedCapacity = 1024;  // entries (4 KiB initial)
static constexpr vm_size_t GcScratchTotalEntries = GcScratchVisitedCapacity;

// Lazy-allocate the GC scratch block on the first user gvm_alloc.  Sits at
// the top of global VM (highest address near m_vm_limit) because it's the
// FIRST block allocated in the global region; every subsequent user block
// lands below it.  When the last user block is freed (m_gvm_user_block_count
// returns to 0), the scratch block is freed too -- m_vm_global then advances
// all the way back to m_vm_limit, restoring the idle state.
//
// Recursion: the inner gvm_alloc_n call uses kind=GcScratch, which the
// public gvm_alloc wrapper detects and skips this hook for, so there's no
// infinite recursion.
void ensure_gc_scratch() {
    if (m_gc_scratch_offset == nulloffset) {
        auto [_, off] = gvm_alloc_n<vm_offset_t>(GcScratchTotalEntries, ChunkKind::GcScratch);
        m_gc_scratch_offset = off;
    }
}

// Public gvm_alloc.  Wraps gvm_alloc_impl with the lazy-scratch lifecycle
// hook for user kinds; passes through verbatim for ChunkKind::GcScratch
// itself (the recursion break).
template<typename T>
[[nodiscard]] std::pair<T *, vm_offset_t> gvm_alloc(vm_size_t payload_size, ChunkKind kind) {
    // Every global allocation must name a SPECIFIC kind so the GC mark-phase walker
    // can dispatch to a walker that marks its Objects (or treat it as a documented
    // leaf).  ChunkKind::Other is a sentinel/default only (non-heap-backed types in
    // expected_chunk_kind_for); a block tagged Other is invisible to the kind-matched
    // back-walk and silently swept.  Pick the kind that names what you are building.
    assert(kind != ChunkKind::Other && "gvm_alloc: ChunkKind::Other is not a valid allocation kind -- name a specific kind");

    if (kind != ChunkKind::GcScratch) {
        ensure_gc_scratch();
// GC-stress (debug only, vm-gc-stress): fire a full sweep before every
// global user alloc so a dropped GC root is collected right before the
// allocation that needs it.  m_in_gc gates re-entry (the GC's own allocs
// must not recurse); GcScratch is excluded above.  m_vm_alloc_active
// gates a GC mid open-ended STREAMING alloc (scanner string/array
// literals), where a sweep would corrupt the half-built block -- several
// VM ops assert(m_vm_alloc_active == nullptr) for the same reason.  The
// fixed-size dispatch allocs the lazy/record/etc. builders use are never
// streaming, so their in-flight-root windows are still fully exercised.
#ifdef TRIX_DEBUGGER
        if (m_gc_stress && !m_in_gc && (m_vm_alloc_active == nullptr)) {
            vm_global_gc();
        }
#endif
    }
    auto result = gvm_alloc_impl<T>(payload_size, kind);
    if (kind != ChunkKind::GcScratch) {
        ++m_gvm_user_block_count;
    }
    return result;
}

// gvm_reissue_free_block<T>(block_off, kind): claim a free block (already
// unlinked from its fastbin or the free list) for a fresh allocation of
// `kind`.  Restamps the header live and returns the payload pointer +
// offset.  The block keeps its m_size -- and therefore its tail tag -- so
// a caller that took a larger-than-requested block hands the user the
// extra slack invisibly; on the next free the block routes back to its
// true size class via gvm_bin_class_for_size(m_size).
template<typename T>
[[nodiscard]] std::pair<T *, vm_offset_t> gvm_reissue_free_block(vm_offset_t block_off, ChunkKind kind) {
    auto header = offset_to_ptr<GvmBlock>(block_off);
    assert(gvm_kind_is_free(header->m_kind));

    header->m_kind = kind;
    header->m_mark_gen = gc_init_mark_for_alloc();  // live; "previous gen"
    header->m_obj_count = 0;
    header->m_next_in_work = nulloffset;  // off-queue at fresh alloc
    header->m_magic = GvmMagic;           // sentinel preserved across reuse, restamp defensive
    // Tail tag value is stable (size unchanged); no rewrite needed.
    auto payload_offset = static_cast<vm_offset_t>(block_off + GvmHeaderSize);
    auto payload_ptr = offset_to_ptr<T>(payload_offset);
    return std::pair{payload_ptr, payload_offset};
}

template<typename T>
[[nodiscard]] std::pair<T *, vm_offset_t> gvm_alloc_impl(vm_size_t payload_size, ChunkKind kind) {
    // Round up to a block that can host free-list links + tail tag if it's
    // ever freed.  payload_size is the user-requested size; the block
    // reserves at least MinPayloadSize bytes of payload area regardless.
    constexpr vm_size_t kSlack = GvmHeaderSize + GvmTailTagSize + (GvmBlockAlignment - 1);
    if (payload_size > (std::numeric_limits<vm_size_t>::max() - kSlack)) {
        error(Error::LimitCheck, "gvm_alloc: payload size {} too large to fit a block header", payload_size);
    } else {
        auto effective_payload = (payload_size > GvmMinPayloadSize) ? payload_size : GvmMinPayloadSize;
        auto raw_total = static_cast<vm_size_t>(GvmHeaderSize + effective_payload + GvmTailTagSize);
        auto total_size = (raw_total + (GvmBlockAlignment - 1)) & ~vm_size_t{GvmBlockAlignment - 1};
        if (total_size < GvmMinBlockSize) {
            total_size = GvmMinBlockSize;
        }

        // Auto-on-VMFull retry.  When fastbin / free list / bump
        // all come up empty, run one mark-sweep GC pass and try the whole
        // alloc sequence again -- GC may have moved blocks back to the
        // fastbins or free list, or advanced m_vm_global up if the freed
        // block was at the top edge.  Retry at most ONCE per alloc; if the
        // post-GC attempt also fails, fall through to vm_alloc_global_ptr's
        // VMFull error.  m_in_gc gates the retry so a VMFull raised from
        // INSIDE the GC's own allocation path can't recursively trigger
        // another pass.
        //
        // The for loop bound IS the retry-once invariant: attempt 0 =
        // pre-GC alloc, attempt 1 = post-GC alloc.  Successful attempts
        // return directly from inside the loop.
        constexpr int kMaxAllocAttempts = 2;
        for (int attempt = 0; attempt < kMaxAllocAttempts; ++attempt) {
            // Fastbin exact-match lookup.  Bin-resident blocks are
            // always exactly the bin's size, so no split happens here.  Skip
            // the bin walk if the request is too large for any bin.
            if (total_size < GvmFastBinThreshold) {
                auto bin_class = gvm_bin_class_for_size(total_size);
                auto head_off = m_gvm_fastbins[bin_class];
                if (head_off != nulloffset) {
                    gvm_fastbin_unlink(head_off, bin_class);
                    // Bin-resident blocks are always exactly the bin's size.
                    assert(offset_to_ptr<GvmBlock>(head_off)->m_size == total_size);
                    return gvm_reissue_free_block<T>(head_off, kind);
                } else {
                    // Exact-match miss: try ONE bin level up before falling through
                    // to the general free list.  Limited to one level because beyond
                    // that the slack (GvmBlockAlignment per step = 8 bytes) approaches
                    // a general-list split's no-slack outcome -- AND splits refill
                    // lower bins (a 64->32+32 split puts a fresh 32-byte block back
                    // in bin 0).  Going +1 saves the O(N) general-list walk when the
                    // next-up bin happens to have an entry.
                    //
                    // The reused block keeps its m_size (next-up bin's size); the
                    // user gets payload + (8 bytes slack) and never sees it.  When
                    // the block eventually frees, gvm_bin_class_for_size routes it
                    // back to its original (next-up) bin via m_size, so there's no
                    // accumulating "wrong-bin" drift.
                    auto next_bin = static_cast<length_t>(bin_class + 1);
                    if (next_bin < GvmFastBinCount) {
                        auto next_head_off = m_gvm_fastbins[next_bin];
                        if (next_head_off != nulloffset) {
                            gvm_fastbin_unlink(next_head_off, next_bin);
                            // Next-up bin's block is strictly larger; user gets the slack.
                            assert(offset_to_ptr<GvmBlock>(next_head_off)->m_size > total_size);
                            return gvm_reissue_free_block<T>(next_head_off, kind);
                        }
                    }
                }
            }

            // First-fit search of the sorted free list.  Walk ascending until we
            // find a block big enough; pop it, optionally split, return.  If no
            // free block fits, fall through to the bump path.
            auto curr = m_gvm_free_head;
            while (curr != nulloffset) {
                auto curr_header = offset_to_ptr<GvmBlock>(curr);
                auto curr_size = curr_header->m_size;
                if (curr_size >= total_size) {
                    gvm_free_list_remove(curr);
                    // If the found block is large enough to split into a usable
                    // remainder (>= MinBlockSize), do so; the upper part is
                    // returned to the free list.  Otherwise consume the whole
                    // block (slack becomes overpayment but no fragmentation hole).
                    if (curr_size >= (total_size + GvmMinBlockSize)) {
                        auto remainder_offset = static_cast<vm_offset_t>(curr + total_size);
                        auto remainder_size = static_cast<vm_size_t>(curr_size - total_size);

                        curr_header->m_size = total_size;
                        curr_header->m_kind = kind;
                        curr_header->m_mark_gen = gc_init_mark_for_alloc();  // live; "previous gen"
                        curr_header->m_obj_count = 0;
                        curr_header->m_next_in_work = nulloffset;
                        curr_header->m_magic = GvmMagic;
                        gvm_write_tail_tag(curr, total_size);

                        auto rem_header = offset_to_ptr<GvmBlock>(remainder_offset);
                        rem_header->m_size = remainder_size;
                        rem_header->m_kind = ChunkKind::Free;
                        rem_header->m_mark_gen = 0;
                        rem_header->m_obj_count = 0;
                        rem_header->m_next_in_work = nulloffset;
                        rem_header->m_magic = GvmMagic;
                        gvm_write_tail_tag(remainder_offset, remainder_size);
                        // Remainder may be bin-eligible (size < threshold)
                        // or list-eligible (>= threshold); route via dispatcher.
                        gvm_free_insert_anywhere(remainder_offset, remainder_size);
                    } else {
                        // Use the whole block; size and tail tag stay as they were.
                        curr_header->m_kind = kind;
                        curr_header->m_mark_gen = gc_init_mark_for_alloc();  // live; "previous gen"
                        curr_header->m_obj_count = 0;
                        curr_header->m_next_in_work = nulloffset;
                        curr_header->m_magic = GvmMagic;
                    }

                    auto payload_raw = reinterpret_cast<vm_t *>(curr_header) + GvmHeaderSize;
                    auto payload_ptr = reinterpret_cast<T *>(payload_raw);
                    auto payload_offset = static_cast<vm_offset_t>(curr + GvmHeaderSize);
                    return std::pair{payload_ptr, payload_offset};
                } else {
                    curr = offset_to_ptr<GvmFreeLinks>(curr + GvmHeaderSize)->next_free;
                }
            }

            // Bump capacity check.  When the bump would fit, fall out of the
            // loop and run vm_alloc_global_ptr below.  When it would not,
            // run a GC pass on attempt 0 (if not already inside one) and
            // try again; on attempt 1, fall through to vm_alloc_global_ptr
            // and let it raise the VMFull error.
            auto raw = reinterpret_cast<uintptr_t>(m_vm_global) - total_size;
            auto aligned = raw & ~uintptr_t{alignof(GvmBlock) - 1};
            auto new_global = reinterpret_cast<vm_t *>(aligned);
            if (new_global >= m_vm_ptr) {
                break;  // bump would fit; exit loop, do the bump below
            } else if ((attempt == 0) && !m_in_gc) {
                vm_global_gc();  // retry the alloc body once on the next loop iteration
            } else {
                break;  // out of options; let vm_alloc_global_ptr raise VMFull
            }
        }

        // No free block fit -- bump m_vm_global down via the existing primitive.
        auto header_ptr = vm_alloc_global_ptr<GvmBlock>(total_size);

        header_ptr->m_size = total_size;
        header_ptr->m_kind = kind;
        header_ptr->m_mark_gen = gc_init_mark_for_alloc();  // live; "previous gen"
        header_ptr->m_obj_count = 0;
        header_ptr->m_next_in_work = nulloffset;
        header_ptr->m_magic = GvmMagic;

        auto block_offset = static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(header_ptr) - m_vm_base);
        gvm_write_tail_tag(block_offset, total_size);

        auto payload_raw = reinterpret_cast<vm_t *>(header_ptr) + GvmHeaderSize;
        auto payload_ptr = reinterpret_cast<T *>(payload_raw);
        auto payload_offset = static_cast<vm_offset_t>(payload_raw - m_vm_base);
        return std::pair{payload_ptr, payload_offset};
    }
}

//
// gvm_free(payload_offset):  free a block previously returned by
// gvm_alloc.  Coalesces with adjacent free blocks (forward in O(1)
// via the next block's header, backward in O(1) via the previous
// block's tail tag), then either inserts the merged block into the
// sorted free list OR -- if the merged block sits at the top edge,
// abutting m_vm_global -- advances m_vm_global to reclaim the
// memory back into the unallocated region.
//
// Caller's contract: payload_offset MUST be a previously-returned
// gvm_alloc offset, and the block MUST currently be LIVE.  Calling
// gvm_free twice on the same offset (without an intervening
// gvm_alloc reuse) is undefined behaviour -- the kind == Free check
// catches some of these cases but not all.
//
// Public gvm_free.  Wraps gvm_free_impl with the lazy-scratch lifecycle
// hook: when freeing the last user block (m_gvm_user_block_count returns
// to 0), the scratch block is freed too.  The scratch block itself bypasses
// the bookkeeping (its kind is GcScratch).
void gvm_free(vm_offset_t payload_offset) {
    auto block_offset = static_cast<vm_offset_t>(payload_offset - GvmHeaderSize);
    auto kind = offset_to_ptr<GvmBlock>(block_offset)->m_kind;
    const bool is_user = (kind != ChunkKind::GcScratch);
#ifdef TRIX_DEBUGGER
    // GC-poison (debug-only, active only while vm-gc-poison is on): scribble the payload
    // of a block being freed so a stray use-after-free READ returns an obviously wrong
    // value DETERMINISTICALLY, instead of stale-but-valid bits that only corrupt when the
    // slot is later reused.  Paired with vm-gc-stress (which fires the freeing GCs), this
    // catches dropped-GC-root bugs reuse-independently (the VM heap is not ASan-poisoned
    // and frees are not otherwise scribbled).  Kept a separate toggle from m_gc_stress so
    // it can be enabled per-test without forcing every existing gc-stress test to first
    // be cleared of latent UAFs.  Payload only: the header (read just above for
    // kind/m_size) and the free-list links + tail tag that gvm_free_impl is about to write
    // are left untouched by poisoning strictly within [payload_offset, block_end - tail
    // tag); gvm_free_impl then overwrites the link region at the payload start.
    if (m_gc_poison) {
        auto block_size = offset_to_ptr<GvmBlock>(block_offset)->m_size;
        auto payload_bytes = gvm_get_payload_size(block_size);
        std::fill_n(offset_to_ptr<vm_t>(payload_offset), payload_bytes, static_cast<vm_t>(0xA5));
    }
#endif
    gvm_free_impl(payload_offset);
    if (is_user) {
        --m_gvm_user_block_count;
        if ((m_gvm_user_block_count == 0) && (m_gc_scratch_offset != nulloffset)) {
            auto scratch_off = m_gc_scratch_offset;
            m_gc_scratch_offset = nulloffset;
            // Reset the m_gc_local_visited pointer too -- it points into the
            // scratch payload, which is about to disappear.  GC entry
            // re-derives the pointer from m_gc_scratch_offset.
            m_gc_local_visited = nullptr;
            m_gc_local_visited_count = 0;
            m_gc_local_visited_capacity = 0;
            gvm_free_impl(scratch_off);
        }
    }
}

void gvm_free_impl(vm_offset_t payload_offset) {
    auto block_offset = static_cast<vm_offset_t>(payload_offset - GvmHeaderSize);
    auto header = offset_to_ptr<GvmBlock>(block_offset);
    auto block_size = header->m_size;

    if (gvm_kind_is_free(header->m_kind)) {
        error(Error::InternalError,
              "gvm_free: double-free at offset {} (block already marked free)",
              static_cast<integer_t>(payload_offset));
    }

    // Preserve the original kind in the low 7 bits before we coalesce
    // with neighbours; the merged block's m_kind reflects whichever
    // kind happened to occupy the lowest address before the merge,
    // which is purely a forensic hint -- the merged region is free.
    auto orig_kind = static_cast<uint8_t>(header->m_kind);

    auto vm_global_off = static_cast<vm_offset_t>(m_vm_global - m_vm_base);
    auto vm_limit_off = static_cast<vm_offset_t>(m_vm_limit - m_vm_base);

    // Forward coalesce: next block (if it exists and is free) merges into us.
    // Neighbour may live in a fastbin OR the general list -- route
    // the unlink by neighbour size via gvm_free_unlink_anywhere.
    auto next_offset = static_cast<vm_offset_t>(block_offset + block_size);
    if (next_offset < vm_limit_off) {
        auto next_header = offset_to_ptr<GvmBlock>(next_offset);
        if (gvm_kind_is_free(next_header->m_kind)) {
            gvm_free_unlink_anywhere(next_offset, next_header->m_size);
            block_size = static_cast<vm_size_t>(block_size + next_header->m_size);
        }
    }

    // Backward coalesce: read prev block's tail tag, find its header,
    // and merge if free.  Tail tag at offset (block_offset - 4).
    // Prev neighbour may live in a fastbin OR the general list --
    // route the unlink by prev_size via gvm_free_unlink_anywhere.
    if (block_offset > vm_global_off) {
        auto prev_size = *offset_to_ptr<uint32_t>(static_cast<vm_offset_t>(block_offset - GvmTailTagSize));
        if ((prev_size != 0) && ((prev_size & (GvmBlockAlignment - 1)) == 0) && (prev_size <= (block_offset - vm_global_off))) {
            auto prev_offset = static_cast<vm_offset_t>(block_offset - prev_size);
            auto prev_header = offset_to_ptr<GvmBlock>(prev_offset);
            // Validate magic before trusting prev_header->m_kind.  A
            // corrupted tail tag could land us inside a payload whose
            // bytes happen to satisfy the alignment+bounds gate above
            // but are not a real header.  m_magic == GvmMagic is the
            // last-mile guard.
            if ((prev_header->m_magic == GvmMagic) && gvm_kind_is_free(prev_header->m_kind)) {
                gvm_free_unlink_anywhere(prev_offset, prev_size);
                block_offset = prev_offset;
                block_size = static_cast<vm_size_t>(block_size + prev_size);
                header = prev_header;
                // Adopt prev block's original kind for the merged
                // header (its low 7 bits already reflect what was
                // there before).  Keeps the forensic hint stable
                // across coalesce.
                orig_kind = static_cast<uint8_t>(prev_header->m_kind);
            }
        }
    }

    // Top-edge advance: if the (possibly coalesced) block sits at the
    // very top of the global region (i.e. its start == m_vm_global),
    // advance m_vm_global up by the block size.  The block disappears
    // entirely; it is NOT inserted into the free list.
    //
    // This reclaims contiguous tops back into the unallocated region.
    if (block_offset == vm_global_off) {
        m_vm_global += block_size;
    } else {
        // Otherwise: write the merged-block header + tail tag, mark Free,
        // and insert into the matching structure (fastbin if size < threshold,
        // sorted general list otherwise).  Preserve the original kind in
        // the low 7 bits of m_kind for forensic logging (gvm_get_freed_kind);
        // bit 7 (GvmFreeFlag) marks the block as free for the test paths
        // (gvm_block_is_free, gvm_kind_is_free).
        header->m_size = block_size;
        header->m_kind = static_cast<ChunkKind>(orig_kind | GvmFreeFlag);
        header->m_mark_gen = 0;
        header->m_obj_count = 0;
        header->m_next_in_work = nulloffset;  // off-queue; free blocks never participate in the GC mark/sweep queue
        header->m_magic = GvmMagic;           // defensive restamp (preserve invariant across coalesce)
        gvm_write_tail_tag(block_offset, block_size);
        gvm_free_insert_anywhere(block_offset, block_size);
    }
}

//
// gvm_write_tail_tag:  the boundary tag is a uint32_t at offset
// (block_start + size - 4) mirroring m_size.  It is written by every
// gvm_alloc / gvm_free path, and read inline (no paired helper) by
// gvm_free's backward-coalesce branch.
//
void gvm_write_tail_tag(vm_offset_t block_offset, vm_size_t block_size) {
    auto tail_offset = static_cast<vm_offset_t>(block_offset + block_size - GvmTailTagSize);
    *offset_to_ptr<uint32_t>(tail_offset) = block_size;
}

//
// gvm_free_list_insert / gvm_free_list_remove:  doubly-linked list
// sorted ASCENDING by offset.  Insert is O(N) walk to find the
// right slot; remove is O(1) via the doubly-linked links.
//
// The list head (m_gvm_free_head) is the lowest-offset free block,
// or nulloffset when empty.  Node links live in the free block's
// payload area -- valid only while m_kind == ChunkKind::Free.
//
void gvm_free_list_insert(vm_offset_t block_offset) {
    ++m_gvm_free_block_count;
    // Magic-validate the inserted block AND every neighbour the walk
    // visits.  A corrupted m_next/m_prev link would otherwise traverse
    // into garbage, silently appending the garbage region into the free
    // list.  The walk's per-step assert catches the corruption at the
    // first non-GvmBlock landing site.
    assert(offset_to_ptr<GvmBlock>(block_offset)->m_magic == GvmMagic);
    auto self = offset_to_ptr<GvmFreeLinks>(block_offset + GvmHeaderSize);

    // Walk from head looking for the first node whose offset > block_offset.
    // Insert before that node.
    auto curr = m_gvm_free_head;
    vm_offset_t prev_off = nulloffset;
    while ((curr != nulloffset) && (curr < block_offset)) {
        assert(offset_to_ptr<GvmBlock>(curr)->m_magic == GvmMagic);
        prev_off = curr;
        curr = offset_to_ptr<GvmFreeLinks>(curr + GvmHeaderSize)->next_free;
    }

    self->prev_free = prev_off;
    self->next_free = curr;

    if (prev_off == nulloffset) {
        m_gvm_free_head = block_offset;
    } else {
        auto item = offset_to_ptr<GvmFreeLinks>(prev_off + GvmHeaderSize);
        item->next_free = block_offset;
    }
    if (curr != nulloffset) {
        auto item = offset_to_ptr<GvmFreeLinks>(curr + GvmHeaderSize);
        item->prev_free = block_offset;
    }
}

void gvm_free_list_remove(vm_offset_t block_offset) {
    --m_gvm_free_block_count;
    // Magic-validate the block being unlinked.  See gvm_free_list_insert
    // for the corruption-detection rationale.
    assert(offset_to_ptr<GvmBlock>(block_offset)->m_magic == GvmMagic);
    auto self = offset_to_ptr<GvmFreeLinks>(block_offset + GvmHeaderSize);
    auto prev_off = self->prev_free;
    auto next_off = self->next_free;

    if (prev_off == nulloffset) {
        m_gvm_free_head = next_off;
    } else {
        auto item = offset_to_ptr<GvmFreeLinks>(prev_off + GvmHeaderSize);
        item->next_free = next_off;
    }
    if (next_off != nulloffset) {
        auto item = offset_to_ptr<GvmFreeLinks>(next_off + GvmHeaderSize);
        item->prev_free = prev_off;
    }

    self->prev_free = nulloffset;
    self->next_free = nulloffset;
}

//
// gvm_bin_class_for_size(size):  map a block size in [GvmMinBlockSize,
// GvmFastBinThreshold) to its bin index in [0, GvmFastBinCount).
//
// 32 -> 0, 40 -> 1, 48 -> 2, 56 -> 3.  Asserts the size is bin-eligible.
//
[[nodiscard]] static constexpr length_t gvm_bin_class_for_size(vm_size_t size) {
    assert(size >= GvmMinBlockSize);
    assert(size < GvmFastBinThreshold);
    assert((size & (GvmBlockAlignment - 1)) == 0);

    return static_cast<length_t>((size - GvmMinBlockSize) / GvmBlockAlignment);
}

//
// gvm_fastbin_insert / gvm_fastbin_unlink:  doubly-linked LIFO stack
// at m_gvm_fastbins[bin_class].  Insert at head; unlink in O(1) via
// the doubly-linked GvmFreeLinks pointers.
//
// Each block in a bin has m_kind == ChunkKind::Free and m_size in the bin's class
// range.  prev_free/next_free overlay the free block's payload area
// exactly as in the general list -- a block is in EXACTLY one
// structure (general list OR one bin) at a time.
//
void gvm_fastbin_insert(vm_offset_t block_offset, length_t bin_class) {
    ++m_gvm_free_block_count;
    assert(bin_class < GvmFastBinCount);
    // Magic-validate the inserted block AND the existing head, which we
    // back-link through.  Mirrors gvm_free_list_insert's defence.
    assert(offset_to_ptr<GvmBlock>(block_offset)->m_magic == GvmMagic);
    auto self = offset_to_ptr<GvmFreeLinks>(block_offset + GvmHeaderSize);
    auto head_off = m_gvm_fastbins[bin_class];

    self->prev_free = nulloffset;
    self->next_free = head_off;

    if (head_off != nulloffset) {
        assert(offset_to_ptr<GvmBlock>(head_off)->m_magic == GvmMagic);
        auto head_links = offset_to_ptr<GvmFreeLinks>(head_off + GvmHeaderSize);
        head_links->prev_free = block_offset;
    }
    m_gvm_fastbins[bin_class] = block_offset;
}

void gvm_fastbin_unlink(vm_offset_t block_offset, length_t bin_class) {
    --m_gvm_free_block_count;
    assert(bin_class < GvmFastBinCount);
    // Magic-validate the block being unlinked.  See gvm_fastbin_insert.
    assert(offset_to_ptr<GvmBlock>(block_offset)->m_magic == GvmMagic);
    auto self = offset_to_ptr<GvmFreeLinks>(block_offset + GvmHeaderSize);
    auto prev_off = self->prev_free;
    auto next_off = self->next_free;

    if (prev_off == nulloffset) {
        assert(m_gvm_fastbins[bin_class] == block_offset);
        m_gvm_fastbins[bin_class] = next_off;
    } else {
        auto item = offset_to_ptr<GvmFreeLinks>(prev_off + GvmHeaderSize);
        item->next_free = next_off;
    }
    if (next_off != nulloffset) {
        auto item = offset_to_ptr<GvmFreeLinks>(next_off + GvmHeaderSize);
        item->prev_free = prev_off;
    }

    self->prev_free = nulloffset;
    self->next_free = nulloffset;
}

//
// gvm_free_insert_anywhere / gvm_free_unlink_anywhere:  size-routed
// dispatchers.  A block of size < GvmFastBinThreshold lives in the
// matching fastbin; size >= threshold lives in the sorted general list.
//
// Single point of truth -- callers don't need to know which structure
// a block is in: they pass `size` (recoverable from the header), and
// the dispatcher routes.
//
void gvm_free_insert_anywhere(vm_offset_t block_offset, vm_size_t block_size) {
    if (block_size < GvmFastBinThreshold) {
        gvm_fastbin_insert(block_offset, gvm_bin_class_for_size(block_size));
    } else {
        gvm_free_list_insert(block_offset);
    }
}

void gvm_free_unlink_anywhere(vm_offset_t block_offset, vm_size_t block_size) {
    if (block_size < GvmFastBinThreshold) {
        gvm_fastbin_unlink(block_offset, gvm_bin_class_for_size(block_size));
    } else {
        gvm_free_list_remove(block_offset);
    }
}

//
// gvm_alloc_n<T>(count, kind):  allocate `count` contiguous T-typed
// payload elements, returning the typed pointer and payload offset.
//
// Mirrors vm_alloc_n<T> for the global path.  The multiplication
// is overflow-checked; payload bytes are NOT zero-initialised
// (callers initialise via std::fill_n / make_null / etc., as they
// did pre-3a).
//
template<typename T>
[[nodiscard]] std::pair<T *, vm_offset_t> gvm_alloc_n(vm_size_t count, ChunkKind kind) {
    if (count > (std::numeric_limits<vm_size_t>::max() / vm_sizeof<T>())) {
        error(Error::LimitCheck, "gvm_alloc_n: count {} of {}-byte elements overflows vm_size_t", count, vm_sizeof<T>());
    } else {
        return gvm_alloc<T>(count * vm_sizeof<T>(), kind);
    }
}

//
// vm_alloc_dispatch / vm_alloc_dispatch_n:  honor m_curr_alloc_global.
//
// Replaces the boilerplate ternary that appeared at every alloc site
// that wanted to participate in `${...}` / `true set-global` blocks:
//
//     auto [p, o] = m_curr_alloc_global ? gvm_alloc<T>(size, kind)
//                                       : vm_alloc<T>(size);
//     if (m_curr_alloc_global) { gvm_set_obj_count(o, count); }
//
// becomes:
//
//     auto [p, o] = vm_alloc_dispatch<T>(size, kind);            // single
//     auto [p, o] = vm_alloc_dispatch_n<T>(count, kind);         // array (also stamps obj_count)
//
// ChunkKind is REQUIRED -- callers must pick the right tag for their
// payload (Array, Record, Tagged, Curry, Thunk, Packed, etc.) so that
// (1) the GC walker dispatches correctly, (2) /status:vm-by-kind tells
// the truth about block populations, and (3) UAF forensics can name
// the kind of the freed block.  An unsuited default (e.g. ChunkKind::Other)
// would silently mistag globals -- hence no default.
//
// vm_alloc_dispatch_n auto-stamps gvm_set_obj_count on the global
// path: every kind whose walker reads m_obj_count (Array, Packed,
// Curry, Thunk, Tagged, Record) wants the count stamped, and the
// few that don't pay only one store of overhead next to the global-allocation
// path itself.  Callers that want a different count semantics (e.g.
// stamping field_count instead of byte_count) should call gvm_alloc_n
// + gvm_set_obj_count by hand.
//
// Sites that are deliberately ALWAYS-LOCAL (save/restore journal, infix
// scratch, vm_temp_alloc) keep calling vm_alloc / vm_alloc_n directly --
// the dispatcher is opt-in by name.  Sites that are deliberately
// ALWAYS-GLOBAL (Dict::create_global_dict, make_global_array_from_mark,
// the <<...>>#$ literal scanner paths, etc.) keep calling gvm_alloc /
// gvm_alloc_n directly.
//
// Family parallels the vm_alloc / vm_alloc_ptr / vm_alloc_n / vm_alloc_n_ptr
// variants in vm_heap.inl:
//
//   vm_alloc_dispatch<T>(size, kind)         -- single, returns (T*, offset)
//   vm_alloc_dispatch_ptr<T>(size, kind)     -- single, returns T*
//   vm_alloc_dispatch_n<T>(count, kind)      -- array, returns (T*, offset)
//   vm_alloc_dispatch_n_ptr<T>(count, kind)  -- array, returns T*
//
// The _n variants auto-stamp gvm_set_obj_count on the global path; the
// non-_n variants do not (single allocations may be header+payload
// composites whose stamp count is field_count, not size -- caller decides).
//
template<typename T>
[[nodiscard]] std::pair<T *, vm_offset_t> vm_alloc_dispatch(vm_size_t size, ChunkKind kind) {
    return (m_curr_alloc_global ? gvm_alloc<T>(size, kind) : vm_alloc<T>(size));
}

template<typename T>
[[nodiscard]] T *vm_alloc_dispatch_ptr(vm_size_t size, ChunkKind kind) {
    return (m_curr_alloc_global ? gvm_alloc<T>(size, kind).first : vm_alloc_ptr<T>(size));
}

template<typename T>
[[nodiscard]] std::pair<T *, vm_offset_t> vm_alloc_dispatch_n(vm_size_t count, ChunkKind kind) {
    // Containers are language-level capped at length_t (the user-visible
    // size_t in Trix); the cast below is safe for in-spec callers.  An
    // out-of-spec caller would silently truncate without this assert.
    assert(count <= std::numeric_limits<length_t>::max());

    if (m_curr_alloc_global) {
        auto result = gvm_alloc_n<T>(count, kind);
        gvm_set_obj_count(result.second, static_cast<length_t>(count));
        return result;
    } else {
        return vm_alloc_n<T>(count);
    }
}

template<typename T>
[[nodiscard]] T *vm_alloc_dispatch_n_ptr(vm_size_t count, ChunkKind kind) {
    assert(count <= std::numeric_limits<length_t>::max());

    if (m_curr_alloc_global) {
        auto result = gvm_alloc_n<T>(count, kind);
        gvm_set_obj_count(result.second, static_cast<length_t>(count));
        return result.first;
    } else {
        return vm_alloc_n_ptr<T>(count);
    }
}

//
// gvm_block_at(payload_offset):  given a payload offset returned by
// gvm_alloc, return the address of the block's header.
//
// Caller's contract: payload_offset MUST be the offset returned by
// gvm_alloc -- i.e., the offset must point into a global block's
// payload region.  Passing an arbitrary offset (from local VM, from
// the temp region, from somewhere mid-payload) yields a meaningless
// header read and is undefined behaviour from the allocator's
// perspective.  This accessor does not validate the free bit -- callers
// must not dereference a block that has been freed.
//
[[nodiscard]] GvmBlock *gvm_block_at(vm_offset_t payload_offset) {
    return offset_to_ptr<GvmBlock>(payload_offset - GvmHeaderSize);
}

//
// gvm_set_obj_count(payload_offset, count):  stamp the user-visible
// element count into the block header's m_obj_count field for the
// block at `payload_offset`.
//
// This is the canonical entry point for callers that want to record
// "how many user-visible Objects (or Packed elements) live in the
// payload" -- the GC walkers read m_obj_count to bound their walk
// at the user-visible region, eliminating slack-walking on Object[]
// payloads.  See the GvmBlock header comment for the kinds that
// participate.
//
// Caller contract: same as gvm_block_at -- payload_offset must be
// the offset returned by a recent gvm_alloc / gvm_alloc_n call.
// Local-VM (vm_alloc) blocks have no GvmBlock header and must not
// be passed here; gate on m_curr_alloc_global at sites where the
// alloc routes conditionally.
//
void gvm_set_obj_count(vm_offset_t payload_offset, length_t count) {
    gvm_block_at(payload_offset)->m_obj_count = count;
}

//
// gvm_get_obj_count(payload_offset):  read the user-visible
// element count stamped at alloc time.  Returns 0 for blocks that
// don't participate (kinds whose alloc-time count isn't tracked, or
// snapshot-thawed blobs that predate the m_obj_count field).  GC
// walkers fall back to payload-size computation in that case.
//
[[nodiscard]] length_t gvm_get_obj_count(vm_offset_t payload_offset) {
    return gvm_block_at(payload_offset)->m_obj_count;
}

//
// gvm_get_mark_gen / gvm_set_mark_gen(payload_offset [, gen]):
// read or write the per-block GC mark generation in the block
// header.  See `gc.inl::vm_global_gc` and the GvmBlock m_mark_gen
// field comment for the semantics.  These helpers exist so GC
// code can treat the mark generation as an opaque per-block tag
// without reaching into block layout.
//
[[nodiscard]] uint8_t gvm_get_mark_gen(vm_offset_t payload_offset) {
    return gvm_block_at(payload_offset)->m_mark_gen;
}

void gvm_set_mark_gen(vm_offset_t payload_offset, uint8_t gen) {
    gvm_block_at(payload_offset)->m_mark_gen = gen;
}

//
// gvm_get_kind(payload_offset):  classification tag of the block.
// Returns ChunkKind::Free if the block has been freed (GvmFreeFlag
// bit set), otherwise the original kind.  Used by GC dispatch and
// free-block checks; the Free sentinel keeps the older switch-on-
// kind callers working unchanged.
//
[[nodiscard]] ChunkKind gvm_get_kind(vm_offset_t payload_offset) {
    auto raw = static_cast<uint8_t>(gvm_block_at(payload_offset)->m_kind);
    if ((raw & GvmFreeFlag) != 0) {
        return ChunkKind::Free;
    } else {
        return static_cast<ChunkKind>(raw);
    }
}

//
// gvm_get_freed_kind(payload_offset):  diagnostic accessor that
// returns the ORIGINAL ChunkKind regardless of whether the block
// is free.  Live blocks return their alloc-time kind; freed blocks
// return the kind they had at the moment they were freed (low 7
// bits of m_kind).  Useful for forensic logging when a stale ref
// points at a reissued / freed block and we want to know what was
// there.  Older snapshots store 0xFF for freed blocks; this returns
// 0x7F (= 127, no live kind) for those, which is a clear "unknown"
// signal.
//
[[nodiscard]] ChunkKind gvm_get_freed_kind(vm_offset_t payload_offset) {
    auto raw = static_cast<uint8_t>(gvm_block_at(payload_offset)->m_kind);
    return static_cast<ChunkKind>(raw & ~GvmFreeFlag);
}

//
// gvm_block_is_free(payload_offset):  convenience predicate that
// tests the GvmFreeFlag bit directly.  Equivalent to
// `gvm_get_kind(payload_offset) == ChunkKind::Free` but doesn't
// strip the original-kind bits.
//
[[nodiscard]] bool gvm_block_is_free(vm_offset_t payload_offset) {
    return ((static_cast<uint8_t>(gvm_block_at(payload_offset)->m_kind) & GvmFreeFlag) != 0);
}

//
// gvm_get_block_size(payload_offset):  total block size including
// the header, payload, padding, and tail tag.  Used by GC for
// header sanity checks, block-extent range checks, and sweep
// reclaimed-bytes accumulation.
//
[[nodiscard]] vm_size_t gvm_get_block_size(vm_offset_t payload_offset) {
    return gvm_block_at(payload_offset)->m_size;
}

// gvm_get_payload_size(block_size):  usable bytes between the header
// and the tail tag for a block of total size `block_size` (= payload
// plus any alignment padding).  The complement of the header/tail
// overhead that gvm_get_block_size includes; centralizes the
// `block_size - GvmHeaderSize - GvmTailTagSize` arithmetic shared by
// GC sweep accounting, poison fills, and gvm_for_each visiting.
//
[[nodiscard]] vm_size_t gvm_get_payload_size(vm_size_t block_size) {
    assert(block_size >= (GvmHeaderSize + GvmTailTagSize));

    return static_cast<vm_size_t>(block_size - GvmHeaderSize - GvmTailTagSize);
}

//
// gvm_block_extends_to(payload_offset):  the largest VM offset
// that lies INSIDE this block (i.e., the address one past the end
// of its tail tag, expressed as an offset).  Used by GC range
// checks: a candidate offset is "inside the global region" iff
// `block_extends_to(payload_offset) <= vm_limit_off`.  Hides the
// block-start arithmetic (= payload_offset - GvmHeaderSize).
//
[[nodiscard]] vm_offset_t gvm_block_extends_to(vm_offset_t payload_offset) {
    return static_cast<vm_offset_t>((payload_offset - GvmHeaderSize) + gvm_get_block_size(payload_offset));
}

// gvm_find_owning_payload(offset, expected_kind):  given an arbitrary
// offset that lies somewhere inside the payload region of an active
// gvm_alloc'd block -- e.g. a Packed Object's m_packed after
// packed_pop_head has advanced it past the block's payload start --
// return that block's payload offset.  Returns nulloffset if `offset`
// is outside the global region or no containing block matching
// `expected_kind` can be located (corrupt heap, slack noise, or the
// owner block has already been freed).
//
// Walks backward from offset in GvmBlockAlignment-sized steps, checking
// at each candidate that (candidate - GvmHeaderSize) carries a valid
// GvmBlock header with: size > 0 and 8-aligned, block within VM,
// payload range containing `offset`, kind matching `expected_kind`,
// magic == GvmMagic, and tail-tag matching m_size.  The kind+size+
// magic+tail-tag quadruple makes false positives essentially
// impossible: a fake header pattern inside a payload would need to
// coincide on four independent fields.
//
// Worst-case O(block_size / GvmBlockAlignment); typical Packed proc
// bodies are well under 1 KB so the walk is short.  When `offset` is
// already at a block's payload start (m_packed never advanced or
// happened to align), the FIRST iteration matches.
[[nodiscard]] vm_offset_t gvm_find_owning_payload(vm_offset_t offset, ChunkKind expected_kind) {
    auto vm_global_off = static_cast<vm_offset_t>(m_vm_global - m_vm_base);
    auto vm_limit_off = static_cast<vm_offset_t>(m_vm_limit - m_vm_base);
    if ((offset < (vm_global_off + GvmHeaderSize)) || (offset >= vm_limit_off)) {
        return nulloffset;
    } else {
        auto candidate = offset & ~static_cast<vm_offset_t>(GvmBlockAlignment - 1);
        while (candidate >= (vm_global_off + GvmHeaderSize)) {
            auto block_offset = static_cast<vm_offset_t>(candidate - GvmHeaderSize);
            auto header = offset_to_ptr<GvmBlock>(block_offset);
            auto block_size = header->m_size;
            if ((block_size > 0) && ((block_size & (GvmBlockAlignment - 1)) == 0) &&
                ((block_offset + block_size) <= vm_limit_off) && (offset < (block_offset + block_size)) &&
                (header->m_kind == expected_kind) && (header->m_magic == GvmMagic)) {
                auto tail_tag_offset = static_cast<vm_offset_t>(block_offset + block_size - GvmTailTagSize);
                auto tail_tag = *offset_to_ptr<uint32_t>(tail_tag_offset);
                if (tail_tag == block_size) {
                    return candidate;
                }
            }
            if (candidate < GvmBlockAlignment) {
                break;
            } else {
                candidate -= GvmBlockAlignment;
            }
        }
        return nulloffset;
    }
}

//
// gvm_for_each(visit):  walk every block in the global region from
// newest (lowest address, just above m_vm_global) to oldest (just
// below m_vm_limit), invoking
//
//     visit(vm_offset_t payload_offset, ChunkKind kind, vm_size_t payload_size, vm_size_t total_size, bool is_free)
//
// once per block.  No callback fires when the global region is empty
// (m_vm_global == m_vm_limit).
//
// `payload_offset` is the live-block payload offset (the same offset
// that gvm_alloc returned to the original caller).  Lets the visitor
// recover a typed pointer with `offset_to_ptr<T>(payload_offset)` --
// useful for restore-time scrubbers (e.g. unlinking dying senders
// from surviving Mailbox blocked-lists).
//
// `payload_size` is the USABLE payload region (block - HeaderSize -
// TailTagSize), the maximum bytes a live block could have used as
// user payload.  When `is_free` is true, the kind is ChunkKind::Free
// and the payload bytes are reused for free-list links + slack.
//
// Walks live AND free blocks; the visitor decides what to count.
// `is_free` reflects (GvmBlock.m_kind == ChunkKind::Free).
//
// Sanity guard: if a block header records a size that is zero, not
// a multiple of BlockAlignment, or extends past m_vm_limit, the walk
// raises Error::InternalError.  Such corruption can only arise from
// a bug in gvm_alloc / gvm_free, a buffer overflow into the global
// region, or a misaligned thaw -- in all three cases continuing the
// walk would compound the damage.
//
template<typename Visitor>
void gvm_for_each(Visitor &&visit) {
    auto cursor = m_vm_global;
    while (cursor < m_vm_limit) {
        auto header = reinterpret_cast<const GvmBlock *>(cursor);
        auto block_size = header->m_size;
        if ((block_size == 0) || ((block_size & (GvmBlockAlignment - 1)) != 0) || ((cursor + block_size) > m_vm_limit)) {
            error(Error::InternalError,
                  "gvm_for_each: corrupt block header at offset {} (size={}, kind={})",
                  static_cast<integer_t>(cursor - m_vm_base),
                  static_cast<integer_t>(block_size),
                  static_cast<integer_t>(static_cast<uint8_t>(header->m_kind)));
        } else if (header->m_magic != GvmMagic) {
            error(Error::InternalError,
                  "gvm_for_each: header magic mismatch at offset {} (got 0x{:x}, expected 0x{:x}); heap corruption",
                  static_cast<integer_t>(cursor - m_vm_base),
                  static_cast<integer_t>(header->m_magic),
                  static_cast<integer_t>(GvmMagic));
        } else {
            auto payload_size = gvm_get_payload_size(block_size);
            auto is_free = gvm_kind_is_free(header->m_kind);
            auto payload_offset = static_cast<vm_offset_t>((cursor - m_vm_base) + GvmHeaderSize);
            visit(payload_offset, header->m_kind, payload_size, static_cast<vm_size_t>(block_size), is_free);
            cursor += block_size;
        }
    }
}

//
// gvm_kind_name(kind):  human-readable label for a ChunkKind, used
// by the `vm-global-info` introspection op as a Dict key.
//
// Returns a stable, kebab-case string view.  The switch covers every
// enumerator (-Wswitch-enum + -Werror enforce that), so the trailing
// `return "other"sv` after the switch is reachable only if a corrupted
// byte is cast to ChunkKind from outside the enum's value range -- a
// defensive fallback for forensic diagnostics, not a code path normal
// callers ever reach.
//
// ChunkKind::Free returns "free" but vm-global-info excludes it from
// the per-kind histogram (see the Free comment in the enum); free
// blocks are reported via the top-level /free-blocks + /free-bytes
// fields instead.  The "free" label is reachable from internal
// callers like gvm_get_freed_kind for forensic logging.
//
[[nodiscard]] static constexpr std::string_view gvm_kind_name(ChunkKind kind) {
    using namespace std::literals::string_view_literals;
    switch (kind) {
    case ChunkKind::Other:
        return "other"sv;

    case ChunkKind::Name:
        return "name"sv;

    case ChunkKind::Dict:
        return "dict"sv;

    case ChunkKind::Set:
        return "set"sv;

    case ChunkKind::Array:
        return "array"sv;

    case ChunkKind::String:
        return "string"sv;

    case ChunkKind::Long:
        return "long"sv;

    case ChunkKind::ULong:
        return "ulong"sv;

    case ChunkKind::Address:
        return "address"sv;

    case ChunkKind::Double:
        return "double"sv;

    case ChunkKind::Int128:
        return "int128"sv;

    case ChunkKind::UInt128:
        return "uint128"sv;

    case ChunkKind::Packed:
        return "packed"sv;

    case ChunkKind::Stream:
        return "stream"sv;

    case ChunkKind::Curry:
        return "curry"sv;

    case ChunkKind::Thunk:
        return "thunk"sv;

    case ChunkKind::Tagged:
        return "tagged"sv;

    case ChunkKind::Record:
        return "record"sv;

    case ChunkKind::CoroutineStacks:
        return "coroutinestacks"sv;

    case ChunkKind::CoroutineContext:
        return "coroutinecontext"sv;

    case ChunkKind::GcScratch:
        return "gcscratch"sv;

    case ChunkKind::HashEntry:
        return "hashentry"sv;

    case ChunkKind::PipeBuffer:
        return "pipebuffer"sv;

    case ChunkKind::Cell:
        return "cell"sv;

    case ChunkKind::Continuation:
        return "continuation"sv;

    case ChunkKind::Screen:
        return "screen"sv;

    case ChunkKind::Mailbox:
        return "mailbox"sv;

    case ChunkKind::Monitor:
        return "monitor"sv;

    case ChunkKind::BindingBucket:
        return "bindingbucket"sv;

    case ChunkKind::BindingEntry:
        return "bindingentry"sv;

    case ChunkKind::Supervisor:
        return "supervisor"sv;

    case ChunkKind::Free:
        return "free"sv;
    }

    return "other"sv;
}
