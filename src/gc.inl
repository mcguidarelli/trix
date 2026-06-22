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

//===--- Global-VM Mark-Sweep Garbage Collector ---===//
//
// Composite Objects that could outlive `restore` are routed into the
// global VM region (above m_vm_global, managed by gvm_heap.inl) so they
// SURVIVE rollback.  That leaves a reclamation gap: composite blocks
// stored in a global container and then overwritten would leak their
// storage until snapshot+thaw, and circular structures would never
// reclaim at all.  This file closes that gap.
//
// It implements stop-the-world precise mark-sweep GC for the global
// region.  Liveness is tracked by a per-block mark GENERATION in
// `GvmBlock.m_mark_gen`: vm_global_gc flips Trix::m_gc_current_gen (a
// 1-bit flip-flop) each pass, and a block is alive iff
// `m_mark_gen == m_gc_current_gen` (see gc_advance_generation and
// gvm_heap.inl's GvmBlock comment).
//
// File ordering: gc.inl is included LATE in trix.h -- after every
// composite struct's owning .inl file -- because the per-kind walkers
// reach into Coroutine/Mailbox/Supervisor/PipeBuffer/Dict/etc.  Per
// the per-subsystem walker convention, those walkers live next to
// their struct definitions; gc.inl's `walk_block_contents` switch is
// the dispatch table.
//
// **Adding a new ChunkKind is a four-step procedure** -- enumerator,
// `gvm_kind_name` case, `walk_block_contents` case, and the per-
// subsystem walker.  See the comment block over `enum class ChunkKind`
// in src/gvm_heap.inl for the full procedure.  The hard-fail safety
// net is the missing-case compile error from `walk_block_contents`'s
// switch (no default: clause + -Wswitch-enum + -Werror).
//
// LEAF vs COMPOSITE classification:
//   Leaves (no Object references in payload, walker is a bare return):
//     Long, ULong, Address, Double, Int128, UInt128, String,
//     Name (m_data name bytes; m_next is a bucket chain reached via
//       the Name-table roots; m_binding is a non-rooting DictEntry
//       cache -- none followed),
//     HashEntry (standalone Dict/Set entry block; its m_key/m_value are
//       marked by the owning Dict/Set bucket walk).
//   Composites (walker enumerates Object references):
//     Dict (entries), Set (entries; key only),
//     Array (Object[N]), Curry, Thunk, Tagged, Record,
//     Packed (a readonly Object array encoded as a byte stream --
//       walker decodes via Object::extract_next_packed and emits one
//       gc_mark_object call per decoded element),
//     CoroutineContext, PipeBuffer, Mailbox, Monitor, BindingBucket,
//     BindingEntry, Supervisor,
//     Cell (reactive cell: value/proc/validator + deps/rdeps/watchers,
//       via cell_gc_walk).
//   No-op walkers (block is marked but its payload is not followed):
//     CoroutineStacks (live region walked from the context's tip
//       pointers, not this block as a whole),
//     GcScratch (GC-private raw-offset payload, kept alive as a root).
//   Non-walkers (retired or reserved):
//     Other, Stream, Continuation, Screen.
//   Sentinel:
//     Free -- gc_mark_object errors on any root that points at one
//     (catches latent dangling-payload bugs).
//

// gc_visited_capacity_from_scratch(): compute the visited-set
// capacity from the lazy GC scratch block's payload size.  Used at
// GC entry to derive the actual capacity, since the scratch block
// may have been grown by grow_gc_local_visited in a previous pass
// (and persisted between passes via the scratch lifecycle).  The
// constant GcScratchVisitedCapacity is only the INITIAL size; once
// grown, the scratch carries its larger size forward.
//
[[nodiscard]] vm_size_t gc_visited_capacity_from_scratch() {
    auto block_size = gvm_get_block_size(m_gc_scratch_offset);
    auto payload_size = gvm_get_payload_size(block_size);
    return (payload_size / static_cast<vm_size_t>(sizeof(vm_offset_t)));
}

// grow_gc_local_visited(): double the visited-set capacity by
// allocating a larger GcScratch block, copying entries, freeing the
// old block, and swapping pointers.  Called from gc_mark_object's
// auxiliary visited-set overflow path.  Mid-pass grow is safe:
// gvm_alloc with kind=GcScratch bypasses the lazy-scratch lifecycle
// hook (so we don't recurse into ensure_gc_scratch); m_in_gc gates
// the auto-on-VMFull retry (so we don't recursively GC).
//
// On VMFull (the global region truly can't fit the bigger scratch):
// gvm_alloc raises Error::VMFull, which propagates up through GC --
// the user sees a clean out-of-memory error rather than a buffer
// overflow.
//
void grow_gc_local_visited() {
    auto old_capacity = m_gc_local_visited_capacity;
    auto new_capacity = old_capacity * 2;

    auto old_scratch_off = m_gc_scratch_offset;
    // STEP 1 (allocation, can throw VMFull).  If this throws, no
    // pointer-swap has happened: old scratch + old m_gc_* state are
    // still consistent.  The vm_global_gc() catch handler propagates
    // VMFull cleanly to the caller.
    auto [new_payload, new_off] = gvm_alloc_n<vm_offset_t>(new_capacity, ChunkKind::GcScratch);

    // STEP 2 (memcpy, noexcept) + STEP 3 (pointer swap, noexcept).
    std::memcpy(new_payload, m_gc_local_visited, m_gc_local_visited_count * sizeof(vm_offset_t));

    m_gc_scratch_offset = new_off;
    m_gc_local_visited = new_payload;
    m_gc_local_visited_capacity = new_capacity;

    // STEP 4 (free old).  gvm_free_impl bypasses the lazy-scratch
    // lifecycle bookkeeping for ChunkKind::GcScratch (the public
    // gvm_free's user-block-count decrement is gated on kind!=GcScratch).
    //
    // gvm_free_impl can throw only on heap corruption -- double-free
    // (kind already has GvmFreeFlag), or magic-fail in the
    // forward/backward coalesce paths.  In either case the heap is
    // already broken; propagating cleanly is the right call.  The
    // secondary leak (old block stays as a stale GcScratch in the
    // global region) surfaces in vm-global-info as a kind=gcscratch
    // count > 1, which is a clear forensic signal.
    gvm_free_impl(old_scratch_off);
}

//
// gc_work_list_insert(payload_offset): head-insert a block onto the
// intrusive m_gc_work_head list, threaded through GvmBlock::m_next_in_work.
// Shared by gc_work_push (mark phase) and gc_doomed_push (sweep phase),
// which run in disjoint sub-phases of a single vm_global_gc pass and so
// reuse the same head + field without conflict.  Capacity is total_live
// by construction -- one slot per live block via the block's own header
// field -- so no overflow check is needed.
//
// Caller contract: the block's m_next_in_work MUST be nulloffset at
// entry (off-list).  Every alloc-success path in gvm_alloc_impl
// initialises m_next_in_work=nulloffset, so this is the steady state
// outside an active mark/sweep.  Caller is responsible for not double-
// inserting -- gc_mark_object's "already-marked" short-circuit is the
// gate that guarantees this for the mark phase.
//
void gc_work_list_insert(vm_offset_t payload_offset) {
    auto block_offset = static_cast<vm_offset_t>(payload_offset - GvmHeaderSize);
    auto header = offset_to_ptr<GvmBlock>(block_offset);
    assert(header->m_next_in_work == nulloffset);

    header->m_next_in_work = m_gc_work_head;
    m_gc_work_head = payload_offset;
}

// gc_work_push(payload_offset): enqueue a freshly-marked block onto the
// in-block GC mark queue and count it toward m_gc_marked_count (the
// "no garbage to sweep" early-exit gate).
//
void gc_work_push(vm_offset_t payload_offset) {
    gc_work_list_insert(payload_offset);
    ++m_gc_marked_count;
}

// gc_work_pop(): dequeue the head of the in-block GC mark queue.
// Returns nulloffset when the queue is empty.
//
// Sets the dequeued block's m_next_in_work back to nulloffset
// (defensive -- maintains the off-queue invariant for the next push).
//
[[nodiscard]] vm_offset_t gc_work_pop() {
    auto payload_offset = m_gc_work_head;
    if (payload_offset == nulloffset) {
        return nulloffset;
    } else {
        auto block_offset = static_cast<vm_offset_t>(payload_offset - GvmHeaderSize);
        auto header = offset_to_ptr<GvmBlock>(block_offset);
        m_gc_work_head = header->m_next_in_work;
        header->m_next_in_work = nulloffset;
        return payload_offset;
    }
}

// gc_doomed_push(payload_offset): enqueue an unmarked (doomed) block
// onto the sweep-phase doomed list.  Same intrusive head-insert as
// gc_work_push but does NOT increment m_gc_marked_count -- the doomed
// list is sweep-phase only and the count is never read again until the
// next pass's clear.  See gc_sweep_unmarked.
//
void gc_doomed_push(vm_offset_t payload_offset) {
    gc_work_list_insert(payload_offset);
}

// LOCAL-VM Dict/Set visit list end-of-list sentinel.  Distinguishes
// "tail of list" from "off list" (the latter is nulloffset).  Picked
// as offset 1 because it's:
//   * not nulloffset (which means "off list")
//   * not 8-byte aligned (no real GvmBlock or local payload can land
//     at offset 1; block alignment is 8 by construction)
//   * easily recognisable in a debugger
static constexpr vm_offset_t VisitListEnd = 1;

// gc_visit_mark(dict_off): mark a LOCAL-VM Dict/Set as visited
// during the global GC walk.  Called from gc_mark_object's local-VM
// branch the first time a Dict/Set is seen.
//
// The visit list serves as a "visited set" with O(1) membership
// check via dict->next_in_visit() != nulloffset.  Items are NEVER
// removed during the mark phase -- the link stays set as the
// "already-visited" marker, providing cycle break.  At end of mark
// phase, gc_visit_clear_all walks the chain and clears every link.
//
// Caller should check `dict->next_in_visit() == nulloffset` before
// calling to avoid double-add (cycle break).
//
void gc_visit_mark(vm_offset_t dict_off) {
    auto dict = offset_to_ptr<Dict>(dict_off);
    assert(dict->next_in_visit() == nulloffset);

    dict->set_next_in_visit_no_save((m_gc_visit_head == nulloffset) ? VisitListEnd : m_gc_visit_head);
    m_gc_visit_head = dict_off;
}

// gc_visit_clear_all(): walk the visit list and clear every dict's
// m_next_in_visit back to nulloffset.  Called at end of mark phase
// to restore the off-list invariant for the next GC pass.
//
void gc_visit_clear_all() {
    auto curr = m_gc_visit_head;
    while ((curr != nulloffset) && (curr != VisitListEnd)) {
        auto dict = offset_to_ptr<Dict>(curr);
        auto next = dict->next_in_visit();
        dict->set_next_in_visit_no_save(nulloffset);
        curr = next;
    }
    m_gc_visit_head = nulloffset;
}

//
// gc_count_live_blocks(): walk every live block in the global region
// and return the count.  Used to pre-size the mark-phase work queue
// (which has an exact upper bound: each live block can be enqueued
// at most once).
//
// This routine does not clear marks per pass -- gc_advance_generation
// flips m_gc_current_gen, making old marks stale by definition.  Only
// the count is computed.
//
// Return type vm_size_t to match vm-global-info's aggregations;
// block counts can vastly exceed length_t's uint16_t range on a
// large global heap.
//
vm_size_t gc_count_live_blocks() {
    vm_size_t total_live = 0;
    gvm_for_each([&total_live](vm_offset_t /*payload_offset*/,
                               ChunkKind kind,
                               vm_size_t /*payload_size*/,
                               vm_size_t /*block_size*/,
                               bool is_free) {
        // Skip the lazy GC scratch block: it's not a user-reachable
        // payload, and the work-queue capacity check downstream uses
        // total_live as the upper bound on enqueued blocks.  Counting
        // scratch would inflate total_live by 1 with no offsetting
        // mark/enqueue, breaking the marked_count == total_live early
        // exit.
        if (!is_free && (kind != ChunkKind::GcScratch)) {
            total_live++;
        }
    });
    return total_live;
}

//
// gc_advance_generation(): flip m_gc_current_gen for the next pass.
// m_gc_current_gen is a 1-bit flip-flop holding {0,1}; XOR-ing it
// makes every existing block's m_mark_gen point to the "other"
// (now-stale) generation by definition -- no per-block clear walk,
// ever.  A block is "alive in this pass" iff m_mark_gen == the new
// m_gc_current_gen; the mark phase promotes reached blocks to it.
//
// This works because of two invariants: every cycle sweeps the WHOLE
// region (gc_sweep_unmarked, gc.inl:342), so after any sweep all
// survivors share one value and nothing from an older gen lingers to
// alias; and fresh allocs are stamped with the current (pre-flip)
// value (gc_init_mark_for_alloc), so the next flip makes them look
// "old" and the mark phase traces them.  The probe marks without
// sweeping, which would break the first invariant, so it restores the
// heap to its pre-probe state itself (see vm_global_gc_probe_impl).
//
void gc_advance_generation() {
    m_gc_current_gen ^= 1;
}

//
// gc_sweep_unmarked(expected_doomed): Phase C of the GC pipeline.
//
// Walks every live block via gvm_for_each.  Unmarked blocks are
// "doomed" -- their payload offsets get collected into a temp-region
// buffer along with their total block sizes accumulated for return.
// Marked blocks (m_mark_gen == current generation) survive untouched;
// the next gc_advance_generation makes the current generation stale, so
// no per-block clear pass is needed.
//
// After the walk completes (and only after -- gvm_for_each's cursor
// would be invalidated by inline gvm_free coalesce / top-edge-advance
// mutation), each doomed offset is released via gvm_free.
//
// Returns the total number of bytes reclaimed (sum of block sizes,
// not just payload bytes -- header + tail tag count).  The reclaimed
// bytes are returned to the global free list / fastbins / top-edge
// region as gvm_free decides.
//
// Caller contracts:
//
//   * `expected_doomed` MUST equal the exact count of unmarked live
//     blocks the upcoming walk will encounter.  vm_global_gc_impl
//     computes this as `total_live - marked_count` after the mark
//     phase finishes; the doomed buffer is pre-sized to the exact
//     count, so passing a smaller value is undefined-behaviour buffer
//     overflow and a larger value wastes temp-region space.
//
//   * Caller MUST NOT invoke this when there is no garbage --
//     vm_global_gc_impl's `if (marked_count == total_live) return 0`
//     early-exit is the contract gate.  Calling here with
//     expected_doomed == 0 is a contract violation; we error.
//
//   * Caller MUST snapshot m_vm_temp_ptr before invoking and restore
//     it after.  vm_global_gc() is the wrapper that handles this;
//     without it, the doomed buffer leaks until the next caller bumps
//     the temp watermark.
//
vm_size_t gc_sweep_unmarked(vm_size_t expected_doomed) {
    if (expected_doomed == 0) {
        error(Error::InternalError,
              "gc_sweep_unmarked: called with expected_doomed == 0 "
              "(caller should have early-exited via marked_count == total_live)");
    } else {
        // Sweep invariant: this function MUST NOT trigger any global-VM
        // allocation between gvm_for_each entry and the final gvm_free
        // drain loop.  An allocation here could ensure_gc_scratch into
        // a region we're about to walk, change m_gc_local_visited under
        // us, or grow the scratch out from under the visited cleanup
        // (gc_visit_clear_all already ran in the mark phase, but a fresh
        // alloc could create new Dict/Set blocks the visited set isn't
        // tracking).  Future patches that need allocation here must move
        // it to before mark or after the sweep loop completes.
        //
        // Doomed list reuses the mark-phase work-queue mechanism: same
        // intrusive linked-list head (m_gc_work_head) threading through
        // every doomed block's GvmBlock::m_next_in_work field.  Mark phase
        // drained the queue to nulloffset before sweep starts (assert-
        // gated below); doomed blocks are about to be freed so clobbering
        // their headers is harmless.  Capacity is total_live by construction
        // -- one slot per live block -- so no overflow check is needed.
        assert(m_gc_work_head == nulloffset);
        vm_size_t reclaimed_bytes = 0;
        auto current_gen = m_gc_current_gen;
        gvm_for_each([this, current_gen, &reclaimed_bytes](vm_offset_t payload_offset,
                                                           ChunkKind kind,
                                                           vm_size_t /*payload_size*/,
                                                           vm_size_t block_size,
                                                           bool is_free) {
            // The lazy GC scratch block survives every sweep -- it's holding
            // the visited set we just used.  Skipping by kind (rather than
            // by m_gc_scratch_offset comparison) means the scratch's
            // mark_gen never has to track current_gen, freeing the mark
            // generation logic from a one-block special case.
            if (!is_free && (kind != ChunkKind::GcScratch) && (gvm_get_mark_gen(payload_offset) != current_gen)) {
                gc_doomed_push(payload_offset);
                reclaimed_bytes += block_size;
            }
            // Survivors (m_mark_gen == current_gen) keep their mark.  The
            // next gc_advance_generation will flip the bit, making this
            // generation "stale" by definition without any per-block
            // clear walk.
        });
        while (m_gc_work_head != nulloffset) {
            gvm_free(gc_work_pop());
        }
        return reclaimed_bytes;
    }
}

//
// gc_mark_object(o): Phase B mark
// primitive.  Per-Object marking helper called from every walker
// (and from the root walker).
//
// Call effect (when not skipped):
//   * stamps the referenced block's m_mark_gen to the current pass,
//   * increments marked_count,
//   * pushes the referenced payload_offset onto the work queue so
//     its contents will be walked later.
//
// Skip (no-op return) cases, in order:
//
//   1. Inline scalar (Object::uses_vm() == false): no payload, no
//      reference to follow.
//   2. Eqref handle (Object::is_eqref()): the value slot stores a
//      generation counter, not a payload offset.  Eqref-backed
//      storage is reachable through the eqref tables (m_eqdict /
//      m_eqset / m_eqproc_storage), which are walked as roots in
//      step 7.
//   3. nulloffset payload (e.g., the empty packed body):
//      Object::make_packed(nulloffset, 0, ExecutableAttrib).
//   4. Local-VM payload (offset < m_vm_global): GC only sweeps the
//      global region; local-VM blocks are managed by save/restore
//      and never appear in the gvm_for_each walk anyway.
//   5. Out-of-range / corrupt offset: the bytes don't form a
//      plausible block header within the global region.  See
//      "Slack noise tolerance" below.
//   6. Free block: the offset lands on a freed block's header.  See
//      "Slack noise tolerance" below.
//   7. Already-marked block (mark bit already set): cycle break.
//      Without this short-circuit, mutual references between two
//      composites would loop forever.
//
// CRITICAL invariant: when we mark a block, we set the mark bit
// BEFORE pushing the work item.  This means any cyclic reference
// encountered before the queue is drained will short-circuit on the
// "already-marked" check above.  The mark bit IS the "in-progress"
// signal -- it doubles as both "this block survives sweep" and
// "this block is on the work queue or has been processed".  If we
// pushed first and marked later, a cycle of length 2 would push the
// same block twice and process it twice (stack-blow potential on
// long cycles).
//
// Slack noise tolerance: gvm_alloc CAN return a block whose payload
// region is bigger than the user's request (the "use whole block"
// path when split would leave < MinBlockSize).  The slack bytes
// hold whatever the prior occupant wrote (free-list links + the
// occupant's user data).  When the prior occupant was an Object[]
// composite, those bytes look like Object cells -- m_aat type tags,
// uses_vm()-eligible kinds, and 4-byte offset fields that randomly
// land within m_vm_global..m_vm_limit range.  A naive walker would
// follow those slack-noise "references" to whatever block they
// happen to point at.  Three things can happen:
//
//   * the slack-noise offset points at a real LIVE block: we mark
//     that block.  False positive; one cycle of false retention.
//     Acceptable.
//   * the slack-noise offset points at a real FREE block: erroring
//     here would be a false-positive runtime fault.  We silently
//     skip (case 6).
//   * the slack-noise offset is corrupt (out of range, lands at
//     unaligned bytes that don't form a valid header): we silently
//     skip (case 5).
//
// The cost of defensive-skip in case 5 is that we lose the
// dangling-payload diagnostic that an idealised gc_mark_object would
// surface for misaligned/out-of-range/header-corrupt offsets.
// Acceptable -- those are slack-noise (e.g., DictEntry mid-block
// pointers cached in Name::m_binding) that legitimately fail header
// sanity.
//
// Case 6 (Free-block hit): error().  gc_mark_object is only called
// from typed-region walks that have semantic guarantees about their
// slots -- nothing slack-walks Object[] payloads -- so a Free-block
// hit is a real use-after-free in the calling subsystem, matching
// mark_global_offset's typed-pointer counterpart.
//
//
// mark_global_offset(payload_offset):
// like gc_mark_object, but takes a raw payload offset instead of an
// Object.  Used by walkers that have a vm_offset_t stored as a raw
// field (e.g., RecordInstance::m_schema, BindingBucket entries) and
// by the root walker for member-var pointers.  Every caller hands a
// typed-pointer guarantee; the Free-block check is an error(), not
// a silent-skip (see gc_mark_object above for the asymmetry rationale).
//
// Same defensive checks as gc_mark_object, minus the Object-shape gates
// (uses_vm, is_eqref) -- callers know they have a raw offset.
//
void mark_global_offset(vm_offset_t payload_offset) {
    if (payload_offset != nulloffset) {
        auto vm_global_off = static_cast<vm_offset_t>(m_vm_global - m_vm_base);
        auto vm_limit_off = static_cast<vm_offset_t>(m_vm_limit - m_vm_base);
        if ((payload_offset >= (vm_global_off + GvmHeaderSize)) && (payload_offset < vm_limit_off)) {
            // Alignment check.  Misaligned offsets are slack-noise garbage,
            // not real payload pointers; reading the would-be block header
            // at a misaligned address is undefined behaviour.  Must come
            // BEFORE any gvm_get_* call (each dereferences the header).
            if ((payload_offset & (GvmBlockAlignment - 1)) == 0) {
                auto block_size = gvm_get_block_size(payload_offset);
                if ((block_size != 0) && ((block_size & (GvmBlockAlignment - 1)) == 0) &&
                    (gvm_block_extends_to(payload_offset) <= vm_limit_off)) {
                    // Free-block diagnostic.  Every caller of mark_global_offset hands a
                    // typed pointer field with a specific subsystem role (CoroutineContext's
                    // m_stacks_offset / m_mailbox / m_monitors / m_binding_table /
                    // m_debug_name; RecordInstance schema offset; supervisor child entries;
                    // BindingBucket entries; Name table buckets and SystemName / TypeName /
                    // ErrorName / WellKnown root offsets).  A hit on a freed block IS a
                    // real UAF in the owning subsystem -- error loudly so the bug surfaces.
                    // (Object[]-payload walks via gc_mark_object retain the silent-skip; see
                    // gc_mark_object below for the stacks-walk rationale.)
                    if (gvm_block_is_free(payload_offset)) {
                        error(Error::InternalError,
                              "GC: typed pointer {} points to a freed block (use-after-free?)",
                              static_cast<integer_t>(payload_offset));
                    } else if (gvm_get_mark_gen(payload_offset) != m_gc_current_gen) {
                        // Mark-generation bit: "alive in this pass" iff
                        // m_mark_gen == m_gc_current_gen.  Already-marked short-circuit.
                        gvm_set_mark_gen(payload_offset, m_gc_current_gen);
                        gc_work_push(payload_offset);
                    }
                }
            }
        }
    }
}

// expected_chunk_kind_for(type):  the global-VM ChunkKind tag stamped
// on blocks that back a given Trix Type with offset_can_advance().
// Used by gc_mark_object to constrain gvm_find_owning_payload's backward
// walk: matching kind is the third independent check (size + tail-tag
// + kind) that makes false-positive owner identification essentially
// impossible.  Only the three offset-advancing types are covered;
// callers must guard with o.offset_can_advance() first.
[[nodiscard]] static constexpr ChunkKind expected_chunk_kind_for(Object::Type t) {
    if (t == Object::Type::Packed) {
        return ChunkKind::Packed;
    } else if (t == Object::Type::Array) {
        return ChunkKind::Array;
    } else if (t == Object::Type::String) {
        return ChunkKind::String;
    } else {
        std::unreachable();
    }
}

// Mark global refs reachable from a LOCAL-VM container payload (offset
// below vm_global_off + GvmHeaderSize).  A local payload is never itself a
// GC-managed global block, but it can hold Object refs INTO global VM that
// must stay marked.  Descends by type; EVERY path returns, so the caller
// treats a local payload as fully handled here.
//
//   * Dict + Set: intrusive m_next_in_visit link.  Capacity is
//     "number of unique local Dicts/Sets visited" -- no cap.
//     Walked lazily by the gc_visit_pop drain in the mark loop.
//   * Tagged / Curry / Thunk / Array / Record / Packed: the
//     auxiliary m_gc_local_visited array, grown on overflow by
//     grow_gc_local_visited (initial size GcScratchVisitedCapacity).
//     These types are typically much smaller in count than Dict/Set.
//
// Frame dicts (|args|#N / |locals|#N) allocate via vm_alloc, never
// gvm_alloc, and are the most common case; local Tagged / Array values
// (e.g. def-prim's primitives-dict entries) can hold global refs too.
// Local Packed matters because a proc body defined outside ${...} lands in
// local VM, yet a nested `${ ... }` inside it allocates ITS body proc (and
// literals) in global VM -- without walking local Packeds that nested body
// would be unreachable from any root and get reclaimed.
// Record a first visit to a LOCAL container at `offset` in the auxiliary
// m_gc_local_visited dedup array (used by Cell / Continuation / Tagged /
// Curry / Thunk / Array / Record / Packed -- the non-Dict/Set locals; Dict
// and Set cycle-break via their intrusive m_next_in_visit link instead).
// Returns false if `offset` was already visited (caller must return without
// re-walking, breaking dep cycles); true after recording a fresh visit.
bool gc_try_record_local_visit(vm_offset_t offset) {
    for (vm_size_t i = 0; i < m_gc_local_visited_count; ++i) {
        if (m_gc_local_visited[i] == offset) {
            return false;
        }
    }
    if (m_gc_local_visited_count >= m_gc_local_visited_capacity) {
        grow_gc_local_visited();
    }
    m_gc_local_visited[m_gc_local_visited_count++] = offset;
    return true;
}

void gc_mark_local_container(Object o, vm_offset_t offset) {
    switch (+o.type()) {
    case +Object::Type::Dict:
    case +Object::Type::Set: {
        auto *dict = offset_to_ptr<Dict>(offset);
        if (dict->next_in_visit() == nulloffset) {  // not yet visited (cycle break)
            gc_visit_mark(offset);
            Dict::gc_walk_contents(this, offset);
        }
        break;
    }

    case +Object::Type::Cell:
        // A LOCAL cell can hold global values (m_value/m_proc/watchers) and
        // deps/rdeps pointing at global cells; descend so they stay marked.
        // Computed cells can form dep cycles, so cycle-break via the visited set.
        if (gc_try_record_local_visit(offset)) {
            cell_gc_walk(this, offset);
        }
        break;

    case +Object::Type::Continuation:
        // A LOCAL ContinuationContext (capture / perform allocate it via
        // vm_alloc) holds the captured exec / err / dict / op-slice Object
        // segments.  A body or effect handler-dict captured inside ${...} lands
        // in GLOBAL VM, and perform LIFTS its handler-dict off the err stack
        // INTO the ctx -- so this descent is the only root that reaches those
        // global refs.  Mark them so a stress GC does not sweep the dict/body
        // out from under a still-resumable continuation.  Cycle-break via the
        // visited set (a stored continuation may be reachable from >1 root).
        if (gc_try_record_local_visit(offset)) {
            gc_walk_continuation(this, offset);
        }
        break;

    case +Object::Type::Tagged:
    case +Object::Type::Curry:
    case +Object::Type::Thunk:
    case +Object::Type::Array:
    case +Object::Type::Packed:
    case +Object::Type::Record: {
        // A fully-consumed local Array/Packed advances its m_offset to one
        // past its last element (array_pop_clone_head bumps m_array by
        // sizeof(Object); packed_pop_head bumps m_packed past the byte
        // stream) -- which is exactly the START offset of whatever block was
        // bump-allocated immediately above it (e.g. map/filter allocate their
        // dst right after consuming the source).  Such an empty view has no
        // elements to descend into, so walking it is a no-op; but recording
        // its offset in the visited set below would FALSELY shadow that
        // adjacent, live, non-empty container when it is reached later in the
        // same root walk (offset-keyed dedup) -- its global element refs would
        // then go unmarked and be swept mid-operation.  Skip empties from the
        // visited bookkeeping entirely; they alias and have nothing to mark.
        // The && short-circuit keeps an empty/aliasing offset OUT of the
        // visited set: gc_try_record_local_visit runs only when not empty.
        // (empty_alias can only be true for Array/Packed; for the other four
        // types it is false and the gate reduces to the visited-set check.)
        const bool empty_alias = (o.is_array() || o.is_packed()) && (o.arrays_length() == 0);
        if (!empty_alias && gc_try_record_local_visit(offset)) {
            switch (+o.type()) {
            case +Object::Type::Tagged: {
                // Tagged payload is exactly 2 Objects at fixed offsets
                // (TaggedNameIndex=0, TaggedValueIndex=1).  No header
                // lookup -- read both slots directly via offset_to_ptr.
                auto *slots = offset_to_ptr<Object>(offset);
                gc_mark_object(slots[Object::TaggedNameIndex]);
                gc_mark_object(slots[Object::TaggedValueIndex]);
                break;
            }

            case +Object::Type::Curry: {
                // Curry payload is exactly 2 Objects [value, callable]
                // at offsets 0 and 1.  Same shape as Tagged.
                auto *slots = offset_to_ptr<Object>(offset);
                gc_mark_object(slots[0]);
                gc_mark_object(slots[1]);
                break;
            }

            case +Object::Type::Thunk: {
                // Thunk payload is exactly 3 Objects [state, proc, result]
                // at offsets ThunkStorageState=0, ThunkStorageProc=1,
                // ThunkStorageResult=2.  state is an Integer (no payload
                // to chase) but walking it is harmless and keeps the loop
                // structurally identical to Tagged/Curry.
                auto *slots = offset_to_ptr<Object>(offset);
                gc_mark_object(slots[Object::ThunkStorageState]);
                gc_mark_object(slots[Object::ThunkStorageProc]);
                gc_mark_object(slots[Object::ThunkStorageResult]);
                break;
            }

            case +Object::Type::Array: {
                // Local-VM Array: length lives in the Object handle
                // (m_arrays_length), not in any GvmBlock header.  Walk
                // exactly that many slots starting at the payload offset.
                auto count = static_cast<vm_size_t>(o.arrays_length());
                auto *slots = offset_to_ptr<Object>(offset);
                for (vm_size_t i = 0; i < count; ++i) {
                    gc_mark_object(slots[i]);
                }
                break;
            }

            case +Object::Type::Packed: {
                // Local-VM Packed: same encoding as global-VM Packed (a
                // byte stream decoded by extract_next_packed) but the
                // element count lives in the Object handle (m_arrays_length)
                // rather than the GvmBlock header's m_obj_count.  For a
                // partially-consumed Packed (rare in local VM, but possible
                // if a local proc is mid-execution at GC time), m_offset
                // has advanced past consumed bytes and m_arrays_length has
                // been decremented to match -- iterating from the current
                // cursor for the current count walks exactly the remaining
                // elements, which is correct.
                auto count = static_cast<vm_size_t>(o.arrays_length());
                auto *cursor = offset_to_ptr<const packed_data_t>(offset);
                for (vm_size_t i = 0; i < count; ++i) {
                    auto [next, obj] = Object::extract_next_packed(this, cursor);
                    gc_mark_object(obj);
                    cursor = next;
                }
                break;
            }

            case +Object::Type::Record: {
                // Local-VM Record instance: layout is [m_schema (4 bytes),
                // Object[field_count]] starting at offsetof(m_fields) --
                // alignof(Object) is 4, so m_fields packs immediately
                // after m_schema with no padding.  Field count lives in
                // the Object handle's m_length.  The schema referenced
                // via m_schema may be local too -- gc_mark_object would
                // silent-skip a local schema but the schema is typically
                // held via userdict (the top-level def-record's returned
                // proc holds the schema offset as a literal).  We don't
                // walk the schema here; only the instance's field values.
                // RecordSchema Objects don't appear as standalone Objects
                // on Trix stacks (only their Array proc wrapper does), so
                // this branch handles only instances.
                auto count = static_cast<vm_size_t>(o.object_length());
                auto fields_offset = static_cast<vm_offset_t>(offset + offsetof(RecordInstance, m_fields));
                auto *slots = offset_to_ptr<Object>(fields_offset);
                for (vm_size_t i = 0; i < count; ++i) {
                    gc_mark_object(slots[i]);
                }
                break;
            }

            default:
                break;  // unreachable: outer group is exactly these six types
            }
        }
        break;
    }

    default:
        // Other local types (String / Stream / WideValue / Coroutine /
        // PipeBuffer / OpaqueHandle / ...) hold no global Object refs to
        // mark -- no-op, matching the original chain's implicit fall-through.
        break;
    }
}

void gc_mark_object(Object o) {
    if (o.uses_vm() && !o.is_eqref() && (o.m_offset != nulloffset)) {
        auto offset = o.m_offset;
        auto vm_global_off = static_cast<vm_offset_t>(m_vm_global - m_vm_base);
        auto vm_limit_off = static_cast<vm_offset_t>(m_vm_limit - m_vm_base);

        // Range check.  A valid global-VM payload offset must leave at
        // least GvmHeaderSize of room before m_vm_global (so its block
        // header sits in the global region) and lie strictly below
        // m_vm_limit.  Anything else is local-VM (descend into Dict / Set
        // contents to mark any global refs they hold; skip otherwise) or
        // out-of-bounds slack noise (skip).
        if (offset < (vm_global_off + GvmHeaderSize)) {
            gc_mark_local_container(o, offset);
        } else if (offset < vm_limit_off) {
            // Resolve to the owning block payload.  Three Trix types advance
            // m_offset past the original block-payload start as the container
            // is consumed:
            //
            //   * Array   advances m_array  by sizeof(Object) per array_pop_head
            //             (stays 8-aligned, but lands inside the array's payload
            //             instead of at its start);
            //   * Packed  advances m_packed by 1-8 bytes per packed_pop_head
            //             (variable element widths in the byte stream often
            //             leave m_packed misaligned);
            //   * String  advances m_string by 1+ bytes per string_pop_count /
            //             string_interval (similar misalignment risk).
            //
            // For these three types a partially-consumed container on the exec
            // stack at GC time has m_offset != payload_start.  Without a
            // backward resolve, gvm_get_block_size(offset) would read random
            // payload bytes as a header -- either silently rejecting the mark
            // (and letting the container's block die) or false-marking some
            // unrelated block.  Both are use-after-free.  gvm_find_owning_payload
            // walks backward in 8-byte steps until it finds a header whose
            // (size + tail-tag + kind) match, identifying the owning block
            // unambiguously.
            //
            // For all other Object types m_offset always equals payload_start;
            // a misaligned offset there is genuine garbage (stale reference,
            // bug-induced corruption) and skipping silently is correct.
            bool can_mark = true;
            if (o.offset_can_advance()) {
                auto owning_payload = gvm_find_owning_payload(offset, expected_chunk_kind_for(o.type()));
                if (owning_payload != nulloffset) {
                    offset = owning_payload;
                } else {
                    can_mark = false;
                }
            } else if ((offset & (GvmBlockAlignment - 1)) != 0) {
                can_mark = false;
            }

            // Header sanity + mark.  Runs for both an advanceable type whose
            // owning payload resolved and an aligned non-advanceable type: the
            // block size must be aligned and the block must stay within the VM.
            if (can_mark) {
                auto block_size = gvm_get_block_size(offset);
                if ((block_size != 0) && ((block_size & (GvmBlockAlignment - 1)) == 0) &&
                    (gvm_block_extends_to(offset) <= vm_limit_off)) {
                    // Free-block diagnostic.  Every gc_mark_object caller traverses a
                    // typed/bounded region with a specific semantic guarantee about
                    // its slots: Array / RecordInstance / RecordSchema use m_obj_count
                    // for an exact count, Curry/Tagged/Thunk pass hard-coded byte
                    // counts, and active stack regions are walked through tip pointers.
                    // No slack-walking remains, so a Free-block hit here is a real
                    // use-after-free in the slot's owner; surface it loudly.
                    if (gvm_block_is_free(offset)) {
                        error(Error::InternalError,
                              "GC: Object slot {} references a freed block (use-after-free?)",
                              static_cast<integer_t>(offset));
                    } else if (gvm_get_mark_gen(offset) != m_gc_current_gen) {
                        // Cycle break: block already marked at the current generation,
                        // contents enqueued / processed.  Mark FIRST so cyclic refs
                        // short-circuit.
                        gvm_set_mark_gen(offset, m_gc_current_gen);
                        gc_work_push(offset);
                    }
                }
            }
        }
    }
}

//
// walk_block_contents(payload_offset):
// Phase B inner-loop dispatch.
//
// Called from the mark-phase work-queue drain loop once per popped
// block.  Looks at the block's ChunkKind tag and either:
//
//   * returns immediately (leaf or non-walker kind), or
//   * delegates to the per-subsystem walker (defined next to its
//     owning struct) that enumerates the block's Object references
//     and calls gc_mark_object on each one.
//
// The CRITICAL invariant: this switch MUST have NO `default:` clause.
// Combined with -Wswitch-enum + -Werror (already enabled), a missing
// case for a newly-added ChunkKind becomes a compile-time error.
// That's the hard-fail safety net for the four-step procedure
// documented over `enum class ChunkKind` in gvm_heap.inl.
//
void walk_block_contents(vm_offset_t payload_offset) {
    auto payload_size = gvm_get_payload_size(gvm_get_block_size(payload_offset));
    switch (gvm_get_kind(payload_offset)) {
    // Leaves -- no Object references in payload.
    case ChunkKind::Long:
    case ChunkKind::ULong:
    case ChunkKind::Address:
    case ChunkKind::Double:
    case ChunkKind::Int128:
    case ChunkKind::UInt128:
    case ChunkKind::String:
        break;

    // Name is a leaf for GC purposes:
    //   * m_data[]: name string bytes (no Object cells).
    //   * m_next:   chains to next Name in this Name's bucket.  The
    //               Name table buckets are walked as roots, so every
    //               interned Name in the bucket is reached
    //               that way -- no need to follow m_next from here.
    //   * m_binding: single-coroutine cache pointing into a
    //                DictEntry::m_value slot, NOT to a block payload.
    //                Following it would hit a non-aligned mid-block
    //                offset that mark_global_offset rejects anyway.
    //                Non-rooting cache; the actual Dict is reached
    //                via systemdict / userdict / frame-dict roots.
    case ChunkKind::Name:
        break;

    // HashEntry: a standalone Dict/Set entry block (put_persist_or_create /
    // set_add_persist_or_create).  Documented LEAF: the entry's m_key/m_value are
    // marked by the owning Dict/Set's bucket walk (Dict::gc_walk_contents), which is
    // the ONLY path that reaches the entry -- and that same walk back-marks this
    // block so it survives the sweep.  Walking it here as anything (e.g. a Dict
    // header) would mis-read; a leaf is correct precisely because the contents are
    // covered by the owner.
    case ChunkKind::HashEntry:
        break;

    // Retired or reserved kinds -- no live allocator today, so no
    // global block of these kinds can exist.  Bare return; if a new
    // caller starts allocating one, the four-step procedure applies.
    // (Other is now also rejected at the gvm_alloc guard -- every
    // allocation must name a specific kind.)
    case ChunkKind::Other:
    case ChunkKind::Stream:
    case ChunkKind::Continuation:
    case ChunkKind::Screen:
        break;

    // Reactive cell: marks its value/proc/validator + deps/rdeps/watchers.
    case ChunkKind::Cell:
        cell_gc_walk(this, payload_offset);
        break;

    // Composite walkers: flat Object arrays.
    case ChunkKind::Array:
        gc_walk_object_array(payload_offset, payload_size);
        break;

    case ChunkKind::Dict:
    case ChunkKind::Set:
        Dict::gc_walk_contents(this, payload_offset);
        break;

    // Composite walkers: fixed-shape Object tuples.
    //
    // Curry and Tagged share the same payload shape: 2 Objects in a
    // flat array.  Thunk has 3 Objects [state, proc, result].  Both
    // are reached via gc_walk_object_array with a hard-coded byte
    // count -- block payload may contain trailing slack (whole-block
    // reuse + alignment pad), but the walker bounds itself to the
    // user-visible region.
    case ChunkKind::Curry:
    case ChunkKind::Tagged:
        gc_walk_object_array(payload_offset, 2 * sizeof(Object));
        break;

    case ChunkKind::Thunk:
        gc_walk_object_array(payload_offset, 3 * sizeof(Object));
        break;

    // Record covers both RecordSchema and RecordInstance.  Layout is
    // identical from the GC's perspective: 4-byte header (schema
    // offset for instances, field_count + pad for schemas), then
    // Object[] starting at offsetof(m_fields)/offsetof(m_names).
    // alignof(Object) is 4 (NOT 8 -- sizeof(Object) is 8 but its
    // largest member is uint32_t-aligned), so the compiler packs
    // m_fields[] / m_names[] right after the 4-byte header with no
    // padding.  Both stamp m_obj_count at alloc time, so we walk
    // Object[m_obj_count] uniformly.
    //
    // Disambiguating instance vs schema (so we know whether to chase
    // first_four as a schema offset) used to use a "first_four looks
    // like a valid global offset" range test.  That fails when an
    // instance is global but its schema is local (mini-scheme's env
    // record-type defined at top level outside ${...}, instances
    // created via env-extend inside ${...}): first_four is a small
    // local offset, the test classifies the block as a schema, and
    // the walker reads count from the low bits of the local offset
    // -- corruption.  Fix: detect schemas by their distinctive
    // 4-byte-header layout (low 16 = field_count, high 16 = 0 pad)
    // -- a valid global schema offset never has its high 16 bits
    // zero, because a global VM offset is greater than 65535 in
    // practice (the global region sits at the top of the heap).
    case ChunkKind::Record: {
        auto first_four = *offset_to_ptr<uint32_t>(payload_offset);
        const bool is_schema = ((first_four & 0xFFFF0000u) == 0);

        if (!is_schema) {
            // Instance: first_four = schema offset.  mark_global_offset
            // silent-skips a local offset; a local schema is reached
            // instead via the dict-stack / userdict registry roots.
            mark_global_offset(first_four);
        }

        // Both RecordInstance::m_fields and RecordSchema::m_names
        // sit immediately after a 4-byte header.  alignof(Object) is
        // 4 (sizeof(Object) is 8 but its largest member is uint32_t-
        // aligned), so the compiler packs the Object[] right after
        // the 4-byte header with no padding.  Use offsetof so the
        // offset stays in sync if the structs ever change.
        static_assert(offsetof(RecordInstance, m_fields) == offsetof(RecordSchema, m_names),
                      "RecordInstance and RecordSchema must share the Object[] start offset");
        constexpr auto fields_start = offsetof(RecordInstance, m_fields);
        auto count = gvm_get_obj_count(payload_offset);
        if ((count > 0) && (payload_size >= (fields_start + (count * sizeof(Object))))) {
            auto fields_offset = static_cast<vm_offset_t>(payload_offset + fields_start);
            auto *fields = offset_to_ptr<Object>(fields_offset);
            for (length_t i = 0; i < count; ++i) {
                gc_mark_object(fields[i]);
            }
        }
        break;
    }

    // Packed is a readonly Object array encoded as a byte stream.
    // The user-visible element count is stored in m_obj_count at
    // alloc time (see make_packed_data in object.inl).  Iterate via
    // Object::extract_next_packed -- the same decoder used by every
    // other Packed-traversal site -- and call gc_mark_object on each
    // produced Object.
    //
    // Why m_obj_count and not "decode until end of payload": the
    // Packed byte stream has no terminator and gvm_alloc may add
    // trailing alignment / slack bytes whose values would mis-decode
    // as malformed packed elements (potentially raising
    // Error::InternalError mid-walk).  Stamping the count at alloc
    // time is the cleanest bound.
    case ChunkKind::Packed: {
        auto element_count = gvm_get_obj_count(payload_offset);
        auto *cursor = offset_to_ptr<const packed_data_t>(payload_offset);
        for (length_t i = 0; i < element_count; ++i) {
            auto [next, obj] = Object::extract_next_packed(this, cursor);
            gc_mark_object(obj);
            cursor = next;
        }
        break;
    }

    // Composite walkers: coroutine + actor surface.
    //
    // CoroutineContext and CoroutineStacks are separate kinds.
    // CoroutineContext walks via gc_walk_coroutine_context (which marks
    // the stacks block by chasing ctx->m_stacks_offset); CoroutineStacks
    // is a no-op walker -- the block is enqueued + marked but its slots
    // are not followed, because only the user-active region between
    // base/tip pointers is live, and those are walked inline from the
    // context's region pointers.
    case ChunkKind::CoroutineContext:
        gc_walk_coroutine_context(payload_offset);
        break;

    case ChunkKind::CoroutineStacks:
        // No-op: the block is marked by the enqueue (via the context
        // walker's mark_global_offset on ctx->m_stacks_offset); its
        // slot contents are walked from the context's tip pointers,
        // not from this block's payload as a whole (slots past the
        // tips hold stale bytes from prior suspends).
        break;

    case ChunkKind::GcScratch:
        // No-op: the GC's own lazy scratch block (holds m_gc_local_visited).
        // Payload is GC-private raw offsets, not user Objects.  Marked as a
        // root so sweep never frees it; this case just consumes the queue
        // entry without further walking.
        break;

    case ChunkKind::PipeBuffer:
        gc_walk_pipebuffer(payload_offset);
        break;

    case ChunkKind::Mailbox:
        gc_walk_mailbox(payload_offset);
        break;

    case ChunkKind::Monitor:
        gc_walk_monitor(payload_offset);
        break;

    case ChunkKind::BindingBucket:
        gc_walk_binding_bucket(payload_offset);
        break;

    case ChunkKind::BindingEntry:
        gc_walk_binding_entry(payload_offset);
        break;

    case ChunkKind::Supervisor:
        gc_walk_supervisor(payload_offset);
        break;

    // Sentinel -- a free block must never reach the work queue.
    case ChunkKind::Free:
        error(Error::InternalError,
              "gc walk_block_contents: walked a free block at offset {}",
              static_cast<integer_t>(payload_offset));
    }
}

//
// walk_object_range(base, ptr, ...): mark every Object in the
// inclusive range [base..ptr].  Used by walk_all_roots for the
// running coroutine's hot stacks (op / exec / err / dict / scratch).
//
// Convention follows the Trix stack pointer convention: ptr points
// at the TOP of stack (last live element), one past the empty mark.
// When base > ptr, the stack is empty -- iteration skips.
//
void walk_object_range(Object *base, Object *ptr) {
    if ((base != nullptr) && (ptr != nullptr) && (ptr >= base)) {
        for (auto *p = base; p <= ptr; ++p) {
            gc_mark_object(*p);
        }
    }
}

//
// walk_all_roots: enumerate every member-var field with global-VM
// reach and route it through gc_mark_object / mark_global_offset.
//
// Source of truth is src/member_vars.inl -- this function audits
// it field-by-field, in roughly the same order.  When a new
// global-VM-reachable field is added there, this walker MUST be
// extended; missing roots are a use-after-free risk.
//
// (Do not use SnapShotHeader as a checklist.  That header is a
// derived projection of these fields for save/restore; using it
// as a cross-check would propagate any drift between member vars
// and the header.)
//
// Sections, in declaration order:
//   1. Running coroutine hot stacks (Object pointer ranges).
//   2. All coroutines via the m_coroutine_head circular registry.
//   3. Name table (m_name_buckets) and pre-interned Name offsets
//      (system / type / error / well-known names + the well-known
//      Object cache).
//   4. Root-objects array (m_root_objects_ptr -- RootObject-indexed).
//   5. Named registry dicts (system / user / error / handlers /
//      protocol) and their offset companions (require / modules /
//      protocol-registry).
//   6. Eqref tables (eqdict, eqset, eqproc storage).
//   7. Save journal (per-save-level chains; flavor-aware Object
//      decoding for Object / DictEntry / SetEntry entries).
//   8. Last-error fields (Object pointers stored on the Trix
//      instance).
//   9. Misc Object cells (status scratch, infix scratch).
//  10. Reactive cell deferred-watcher buffer.
//
// Offsets that are NOT roots (and why):
//   * m_gvm_free_head, m_gvm_fastbins[]: free-list heads inside
//     the global region.  Walking them would mark blocks we
//     specifically want to leave dead.
//   * m_extvalue_free_list, m_widevalue_free_list: per-save-level
//     free cells in LOCAL VM (not global).
//   * m_coroutine_stack_free, m_coroutine_ctx_free,
//     m_binding_bucket_free, m_binding_entry_free, m_monitor_pool:
//     scalar pool free lists of recyclable blocks; same rationale
//     as the dlm free list.  All five are DRAINED at GC entry
//     (see vm_global_gc_impl) so the runtime allocs fresh after
//     a GC pass instead of popping a since-reclaimed offset.
//   * m_dict_pool, m_frame_dict_pool: pool free entries; in-use
//     ones are reachable through the running coroutine's dict
//     stack or the named registry dicts.
//   * m_mailbox_pool, m_pipe_pool, m_monitor_pool: free-list pools.
//   * m_ready_head/tail, m_ready_high_*, m_timer_head: scheduler
//     queues threading through coroutine fields; the coroutines
//     themselves are rooted via m_coroutine_head.
//   * m_main_context: the main coroutine's own m_coroutine_head
//     cycle includes it.
//   * Stream pointers (m_stream_inuse_list, m_stdin/out/err,
//     m_stream_free_list): Stream is a retired ChunkKind today
//     (no global allocator); the C++-side Stream objects live
//     outside global VM.
//
void walk_all_roots() {
    // 1. Running coroutine hot stacks
    walk_object_range(m_op_base, m_op_ptr);
    walk_object_range(m_exec_base, m_exec_ptr);
    walk_object_range(m_err_base, m_err_ptr);
    walk_object_range(m_dict_base, m_dict_ptr);
    walk_object_range(m_scratch_base, m_scratch_ptr);
    // GcScope temporary roots (object.inl): in-flight global nodes held across a
    // build's allocations.  Empty except mid-operator; scanned identically.
    walk_object_range(m_gc_roots_base, m_gc_roots_ptr);

    // 2. All coroutines via the registry list (circular).  The
    // running coroutine's context block is included; gc_walk_
    // coroutine_context only marks subsystem block offsets, not
    // the stale stack ranges, so there's no double-walk against
    // section 1 above.
    if (m_coroutine_head != nulloffset) {
        auto off = m_coroutine_head;
        do {
            mark_global_offset(off);
            off = offset_to_ptr<CoroutineContext>(off)->m_next;
        } while (off != m_coroutine_head);
    }

    // 2b. Main coroutine context, when it's local-VM allocated (the
    // common case since coroutine_alloc_context's "first call"
    // optimisation; see ops_coroutine.inl).  The registry walk above
    // calls mark_global_offset on every context offset, which silently
    // skips offsets in local VM -- so main's bindings / mailbox /
    // monitor list / supervisor ref / etc. would never get walked
    // without this branch.  Call gc_walk_coroutine_context directly:
    // it reads main's content fields and marks each reachable global
    // block (no-op for local fields, identical behaviour to walking a
    // global context).
    auto vm_global_off = static_cast<vm_offset_t>(m_vm_global - m_vm_base);
    if ((m_main_context != nulloffset) && (m_main_context < (vm_global_off + GvmHeaderSize))) {
        gc_walk_coroutine_context(m_main_context);
    }

    // 3. Name table buckets + pre-interned Name offsets.
    if (m_name_buckets != nullptr) {
        for (name_bucket_count_t i = 0; i < m_name_bucket_count; ++i) {
            for (auto curr = m_name_buckets[i]; curr != nulloffset; curr = offset_to_ptr<Name>(curr)->next()) {
                mark_global_offset(curr);
            }
        }
    }
    // 3b. Pre-interned Name offset tables (vm_offset_t arrays, null until
    // built): walk each entry by raw offset via mark_global_offset.
    struct OffsetTable {
        const vm_offset_t *base_ptr;
        size_t count;
    };
    for (auto table : {
                 OffsetTable{m_systemname_offsets,  static_cast<size_t>(SYSTEMNAME_COUNT)},
                 OffsetTable{  m_typename_offsets, static_cast<size_t>(Object::TypeCount)},
                 OffsetTable{ m_errorname_offsets,        static_cast<size_t>(ErrorCount)},
                 OffsetTable{ m_wellknown_offsets,   static_cast<size_t>(WELLKNOWN_COUNT)}
    }) {
        if (table.base_ptr != nullptr) {
            for (size_t i = 0; i < table.count; ++i) {
                mark_global_offset(table.base_ptr[i]);
            }
        }
    }

    // 4. Object arrays: the WellKnownName cache (fixed member array -- base is
    // never null) and the RootObject array (heap pointer, null until built).
    // Mark each Object directly via gc_mark_object.
    struct ObjectTable {
        const Object *base_ptr;
        size_t count;
    };
    for (auto table : {
                 ObjectTable{ m_wellknown_cache,   static_cast<size_t>(WELLKNOWN_COUNT)},
                 ObjectTable{m_root_objects_ptr, static_cast<size_t>(ROOT_OBJECT_COUNT)}
    }) {
        if (table.base_ptr != nullptr) {
            for (size_t i = 0; i < table.count; ++i) {
                gc_mark_object(table.base_ptr[i]);
            }
        }
    }

    // 5. Named registry dicts + offset companions.  These Dict
    // headers may live in LOCAL VM (allocated at init via vm_alloc,
    // below m_vm_global) but their entries' Object values can
    // reference global blocks -- a /a-keep -> Array binding routes
    // through a DictEntry whose m_value is an Array Object whose
    // m_offset is a global-VM payload offset.  Walk their contents
    // DIRECTLY (not via mark_global_offset, which would silently
    // skip local-VM offsets); Dict::gc_walk_contents internally
    // calls gc_mark_object on each key/value, and gc_mark_object's range
    // check screens local-VM payloads while marking global ones.
    for (auto *dict_ptr : {m_systemdict, m_protocoldict, m_userdict, m_errordict, m_handlersdict}) {
        if (dict_ptr != nullptr) {
            Dict::gc_walk_contents(this, ptr_to_offset(dict_ptr));
        }
    }
    // The offset-form companions may be in either region; mark_global_offset
    // handles global cases (enqueues for kind dispatch).  If they happen to
    // be local, we'd still need walk-contents -- but today these three are
    // populated via global VM allocation (require, modules, protocol-registry),
    // so mark_global_offset reaches them correctly.
    for (auto off : {m_require_dict_offset, m_modules_dict_offset, m_protocol_registry_offset}) {
        mark_global_offset(off);
    }

    // 6. Eqref tables: same local-vs-global concern as named dicts.
    // Walk their entries directly so global-VM Object values stay reachable.
    for (auto *dict_ptr : {m_eqdict, m_eqset}) {
        if (dict_ptr != nullptr) {
            Dict::gc_walk_contents(this, ptr_to_offset(dict_ptr));
        }
    }
    if (m_eqproc_storage_ptr != nullptr) {
        // Storage holds Object cells when m_eqproc_stored_length > 0;
        // when 0, the buffer holds packed bytes (unaddressable as Objects).
        for (length_t i = 0; i < m_eqproc_stored_length; ++i) {
            gc_mark_object(m_eqproc_storage_ptr[i]);
        }
    }

    // 7. Save journal: per-save-level chains, each entry's flavor
    // determines whether it carries Object cells worth marking.
    walk_save_journal();

    // 8. Last-error fields (pointers into local VM scratch areas)
    for (auto *obj_ptr : {m_last_error_name_ptr,
                          m_last_operator_ptr,
                          m_last_error_msg_ptr,
                          m_last_error_data_ptr,
                          m_ostack_ptr,
                          m_dstack_ptr,
                          m_estack_ptr}) {
        if (obj_ptr != nullptr) {
            gc_mark_object(*obj_ptr);
        }
    }

    // 9. Misc Object cells stored directly on the Trix instance.
    gc_mark_object(m_status_scratch_obj);
    for (size_t i = 0; i < MaxInfixTokens; ++i) {
        gc_mark_object(m_infix_scratch_objs[i]);
    }

    // 10. Reactive cell deferred-watcher buffer (BatchEntry pairs).  Mark both the
    // pre-batch old value AND the cell itself: @batch-end / @batch-fire read the cell's
    // current m_value (header->m_value.make_clone) as the watcher's NEW value when the
    // batch closes, and the slow path re-reads the header for dirty/computed checks.  A
    // cell reachable only via this buffer (an undef'd inline cell set inside a batch)
    // would otherwise be swept mid-batch and its header read back as poison.
    if ((m_deferred_watchers != nulloffset) && (m_deferred_watcher_count > 0)) {
        auto *entries = offset_to_ptr<BatchEntry>(m_deferred_watchers);
        for (length_t i = 0; i < m_deferred_watcher_count; ++i) {
            gc_mark_object(Object::make_cell(entries[i].m_cell_offset));
            gc_mark_object(entries[i].m_old_value);
        }
    }

// 11. Debugger roots: the breakpoints Dict, sid->path cache Dict, and the
// event-callback proc all live in global VM but are referenced only from
// m_debug; without these, vm-global-gc sweeps them out from under the hook.
// Compiled out with the debugger.
#ifdef TRIX_DEBUGGER
    mark_global_offset(m_debug.m_breakpoints_offset);
    mark_global_offset(m_debug.m_sid_path_cache_offset);
    gc_mark_object(m_debug.m_event_callback);
    // Break-on-error stash: live across the inspector session, which can
    // run a global GC (watch evals); see DebugState in ops_debugger.inl.
    gc_mark_object(m_debug.m_pending_error_name);
    gc_mark_object(m_debug.m_pending_error_msg);
    gc_mark_object(m_debug.m_pending_error_data);
    gc_mark_object(m_debug.m_pending_operator);
#endif
}

//
// walk_save_journal: iterate every per-save-level journal chain and
// delegate to Save::gc_walk_chain for the actual flavor-aware Object
// decoding.  The Save class encapsulates its own private Flavor /
// Entry types -- this helper just supplies the chain heads.
//
void walk_save_journal() {
    if (m_save_stack != nullptr) {
        for (save_level_t level = 1; level <= m_curr_save_level; ++level) {
            Save::gc_walk_chain(this, m_save_stack[level]);
        }
    }
}

//
// vm_global_gc(): top-level mark-sweep GC entry point.
//
// Runs the three-phase pipeline: clear marks + count live, mark
// from roots + drain work queue, sweep unmarked.  Returns the byte
// count reclaimed by the sweep phase (0 when there's no garbage).
//
// Re-entrancy guard: m_in_gc is set across the body so the auto-on-
// VMFull retry hook in gvm_alloc skips a recursive GC if its own
// allocation path raises VMFull.  m_vm_temp_ptr is snapshot/restored
// so the work-queue and doomed-list temp allocations are released
// cleanly even if vm_temp_alloc itself raises VMFull mid-pass.
//
// Re-entry test surface: the if (m_in_gc) early-return at function
// entry is unreachable from Trix-level user code today -- the GC
// body doesn't call gvm_alloc (only vm_temp_alloc, which doesn't
// re-enter gvm_alloc), and the auto-on-VMFull retry hook explicitly
// gates on `!m_in_gc`.  The guard is defensive against future bugs.
// Indirect evidence that m_in_gc is reset correctly:
//   - test_gc.trx section 8 (gc-in-progress is false at rest);
//   - test_gc_vmfull.trx section 1 (gc-in-progress cleared even
//     when GC throws mid-impl, exercising the catch/restore path).
//
// Two count-driven early-exits:
//   * total_live == 0      -- empty global heap, nothing to do.
//   * marked_count == total_live -- everything is reachable, no
//     garbage exists; skip the sweep walk entirely.  Marks on
//     survivors stay set; the next pass's clear-phase removes them.
//
// Cycles short-circuit naturally via gc_mark_object's already-marked
// check.  Mark BEFORE pushing the work item -- see gc_mark_object's
// "CRITICAL invariant" comment for the cycle-correctness rationale.
//
vm_size_t vm_global_gc() {
    if (m_in_gc) {
        return 0;
    } else {
        auto temp_save = m_vm_temp_ptr;
        m_in_gc = true;
        auto reclaimed = vm_size_t{0};
        try {
            reclaimed = vm_global_gc_impl();
        }
        catch (...) {
            // Mid-pass throw can leave intrusive queue + visit-list state
            // partially populated.  Drain both so the next pass's
            // entry-time invariants (gc_work_push / gc_visit_mark assert
            // the off-list shape; vm_global_gc_impl asserts both heads
            // are nulloffset) hold.
            //
            // Order matters: drain m_gc_work_head FIRST so each block's
            // m_next_in_work returns to nulloffset (gc_work_pop's side
            // effect).  Then clear the Dict/Set visit list.  Mark
            // generation stays untouched -- the next pass advances it,
            // making any partial m_mark_gen stamps stale and harmless.
            while (m_gc_work_head != nulloffset) {
                static_cast<void>(gc_work_pop());
            }
            gc_visit_clear_all();
            m_gc_marked_count = 0;
            m_vm_temp_ptr = temp_save;
            m_in_gc = false;
            throw;
        }
        m_vm_temp_ptr = temp_save;
        m_in_gc = false;
        // Introspection: bump counters AFTER restoring guard
        // state so a peek via //:status:gc-runs from inside the same
        // call chain sees consistent values (m_in_gc==false here).
        ++m_gc_run_count;
        m_gc_last_reclaimed_bytes = reclaimed;
        return reclaimed;
    }
}

vm_size_t vm_global_gc_impl() {
    // Drain the runtime pool free lists before mark+sweep.  These
    // chains hold recyclable blocks the runtime hasn't returned to
    // the global allocator yet (fast-reuse for hot alloc paths).  None of them
    // are GC roots (per the design intent that pool free lists hold
    // recyclable blocks GC may reclaim).  Without draining, the
    // chain heads would dangle at reclaimed blocks after sweep, and
    // the next pool-alloc would pop a freed offset.  Drop the
    // heads: GC reclaims the chains naturally, and the runtime
    // allocs fresh from gvm_alloc on the next request.
    //
    // Pre-narrowing the stacks-walk, slack-walking through the
    // chains' next-pointer slots accidentally kept some of these
    // blocks alive across passes.  Post-narrowing (see
    // gc_walk_coroutine_context in ops_coroutine.inl), that
    // accidental save is gone -- so the drain is now mandatory or
    // the runtime crashes on first reuse.
    //
    // The five scalar pool free lists hold globally-allocated blocks
    // (mailbox, monitor, supervisor, coroutine, binding tables -- all
    // in global VM and BASE-immune to save/restore).  Drop them
    // unconditionally.
    m_coroutine_stack_free = nulloffset;
    m_coroutine_ctx_free = nulloffset;
    m_binding_bucket_free = nulloffset;
    m_binding_entry_free = nulloffset;
    m_monitor_pool = nulloffset;

    // Array-shaped pool free lists.  Each slot may hold a local-VM
    // offset (Dict pools accept both local and global recycled dicts;
    // GC never touches local VM and those slots stay valid) or a
    // global-VM offset (would dangle after sweep).  Iterate and zero
    // only the global slots.
    auto drain_global_slots = [this](vm_offset_t *array, vm_size_t count) {
        if (array != nullptr) {
            for (vm_size_t i = 0; i < count; ++i) {
                if ((array[i] != nulloffset) && is_global(array[i])) {
                    array[i] = nulloffset;
                }
            }
        }
    };
    drain_global_slots(m_dict_pool, static_cast<vm_size_t>(m_max_save_level) * MaxDictPoolSize);
    drain_global_slots(m_frame_dict_pool, MaxDictPoolSize);
    // Mailbox + pipe pools hold only globally-allocated blocks, so the
    // conditional is_global check is a no-op for those slots but keeps
    // the helper uniform.
    drain_global_slots(m_mailbox_pool, MaxMailboxPoolSize);
    drain_global_slots(m_pipe_pool, MaxPipePoolSize);

    // m_frame_dict_overflow is a singly-linked chain (next pointer in
    // Dict::m_pool) of oversized frame dicts.  Excise global-VM nodes;
    // local-VM nodes stay in the chain (GC doesn't touch local).
    Dict::gc_drain_global_from_overflow(this, &m_frame_dict_overflow);

    // Phase A: advance the generation (flip the mark bit, making all
    // old marks stale by definition -- no clear walk) and count live
    // blocks for the work-queue upper bound.
    gc_advance_generation();
    auto total_live = gc_count_live_blocks();
    if (total_live == 0) {
        return 0;
    } else {
        // Phase B: mark.  Three cycle-break / work-queue mechanisms:
        //   * m_gc_work_head: GLOBAL block work queue (intrusive in
        //     GvmBlock::m_next_in_work).  Capacity = total_live, no cap.
        //   * m_gc_visit_head: LOCAL Dict/Set visit set (intrusive in
        //     Dict::m_next_in_visit).  Used as a visit-bit + post-pass
        //     cleanup chain; the link stays set during mark for cycle
        //     break, cleared en masse via gc_visit_clear_all at the end.
        //     Walking is recursive within gc_mark_object, not queued.
        //   * m_gc_local_visited: auxiliary visited-offset array for
        //     Tagged/Curry/Thunk/Array/Record/Packed (no struct prefix
        //     to absorb a link).  Starts at GcScratchVisitedCapacity and is
        //     grown on overflow by grow_gc_local_visited.
        if (m_gc_scratch_offset == nulloffset) {
            error(Error::InternalError, "gc: scratch block missing -- did gvm_alloc skip ensure_gc_scratch?");
        } else {
            auto *scratch = offset_to_ptr<vm_offset_t>(m_gc_scratch_offset);
            m_gc_local_visited = scratch;
            m_gc_local_visited_capacity = gc_visited_capacity_from_scratch();
            m_gc_local_visited_count = 0;
            assert(m_gc_work_head == nulloffset);
            assert(m_gc_visit_head == nulloffset);
            m_gc_marked_count = 0;

            walk_all_roots();

            while (m_gc_work_head != nulloffset) {
                auto payload_offset = gc_work_pop();
                walk_block_contents(payload_offset);
            }

            // Restore the off-list invariant on every visited Dict/Set so the
            // next GC pass starts clean.  Position relative to sweep does not
            // affect correctness -- sweep only frees global blocks, never the
            // local containers tracked here -- but clearing at the mark/sweep
            // boundary keeps the two phases cleanly separated.
            gc_visit_clear_all();

            if (m_gc_marked_count == total_live) {
                return 0;  // no garbage; skip sweep
            } else {
                // Phase C: sweep.
                return gc_sweep_unmarked(total_live - m_gc_marked_count);
            }
        }
    }
}

//
// vm-global-gc:  -- int
//
// Run a global-VM mark-sweep GC pass.  Returns the number of bytes
// reclaimed by the sweep phase.
//
// throws: opstack-overflow
static void vm_global_gc_op(Trix *trx) {
    auto reclaimed = trx->vm_global_gc();
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(reclaimed));
}

//
// vm-gc-stress:  bool --   (debug-only)
//
// Toggle GC-stress mode: when true, gvm_alloc fires a full vm_global_gc before
// every global user allocation (see m_gc_stress).  A test tool for making
// GC-rooting bugs deterministic -- a root held only in a C local across an
// allocation is swept at the next alloc, surfacing as an ASan use-after-free at
// the exact site rather than a timing-dependent latent bug.  O(live-heap) per
// alloc; never for production use.  Present only in TRIX_DEBUGGER builds.
//
#ifdef TRIX_DEBUGGER
static void vm_gc_stress_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);
    trx->m_gc_stress = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;
}
#endif

// vm-gc-poison:  bool --   (debug-only)
//
// Toggle GC-poison mode: when true, gvm_free scribbles a freed block's payload with
// a poison byte pattern (see m_gc_poison), so a stray use-after-free READ returns an
// obviously-wrong value DETERMINISTICALLY rather than stale-but-valid bits that only
// corrupt when the slot is later reused.  Pairs with vm-gc-stress (which fires the
// GCs that free blocks) to make dropped-GC-root bugs surface reuse-independently.
// Present only in TRIX_DEBUGGER builds.
//
#ifdef TRIX_DEBUGGER
static void vm_gc_poison_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);
    trx->m_gc_poison = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;
}
#endif

//
// vm_global_gc_probe(): mark-only diagnostic pass.
//
// Runs the same Phase A (clear) + Phase B (mark) pipeline as
// vm_global_gc, but skips the sweep -- instead, walks every live
// block and counts how many are reachable (marked) vs unreachable
// (unmarked).  Marks on survivors are cleared in the tabulation
// pass so the next GC starts fresh.
//
// Use cases:
//   * "Did my refactor introduce a leak?"  Run the probe before and
//     after a workload; if dead-block counts grow without an
//     intervening GC, something is leaking.
//   * "How much would a real GC reclaim?"  The probe answers without
//     side-effects; user can decide whether the cost of the sweep is
//     warranted.
//   * Test assertions: tests/test_gc.trx uses the probe to verify
//     reachability without disturbing the heap.
//
// Cost: visits every live block THREE times (clear, walk-via-queue
// during mark, tabulate).  A real vm_global_gc visits twice (clear,
// sweep).  Probe is therefore SLOWER than a real GC, not faster.
//
struct GcProbeResult {
    vm_size_t live_count;
    vm_size_t dead_count;
    vm_size_t live_bytes;
    vm_size_t dead_bytes;
};

GcProbeResult vm_global_gc_probe() {
    GcProbeResult result{};
    if (m_in_gc) {
        return result;
    } else {
        auto temp_save = m_vm_temp_ptr;
        m_in_gc = true;
        try {
            result = vm_global_gc_probe_impl();
        }
        catch (...) {
            m_vm_temp_ptr = temp_save;
            m_in_gc = false;
            throw;
        }
        m_vm_temp_ptr = temp_save;
        m_in_gc = false;
        return result;
    }
}

GcProbeResult vm_global_gc_probe_impl() {
    GcProbeResult result{};

    // A probe is a DRY-RUN GC: it flips the mark bit, marks the
    // reachable set, tabulates live vs dead, then RESTORES the heap to
    // its pre-probe state (mark generation + every block's m_mark_gen).
    //
    // The restore is MANDATORY for the 1-bit mark-generation flip-flop.
    // A probe marks but does NOT sweep, so without restoring it leaves
    // the heap non-uniform: reachable blocks at the flipped gen, garbage
    // and any fresh allocs at the original gen.  The next real GC's flip
    // would then alias -- it would skip-tracing a live child of a
    // probe-marked parent (premature collection of reachable data) or
    // fail to reclaim garbage.  Resetting every block's mark back to
    // saved_gen makes the probe a true no-op on heap state.  (The old
    // 0..255 counter needed no restore: each GC advanced PAST the
    // probe's marks, leaving 254 generations of slack so they never
    // aliased.  The bit has no slack, hence the explicit restore.)
    auto saved_gen = m_gc_current_gen;
    gc_advance_generation();
    auto total_live = gc_count_live_blocks();
    if (total_live == 0) {
        m_gc_current_gen = saved_gen;
        return result;
    } else {
        // Phase B: mark from roots, drain queue.  Mirrors vm_global_gc_impl --
        // intrusive in-block work queue (m_gc_work_head + GvmBlock::m_next_in_work);
        // local-visited set lives in the lazy GC scratch block.  See
        // vm_global_gc_impl for the layout rationale.
        if (m_gc_scratch_offset == nulloffset) {
            error(Error::InternalError, "gc-probe: scratch block missing -- did gvm_alloc skip ensure_gc_scratch?");
        } else {
            auto *scratch = offset_to_ptr<vm_offset_t>(m_gc_scratch_offset);
            m_gc_local_visited = scratch;
            m_gc_local_visited_capacity = gc_visited_capacity_from_scratch();
            m_gc_local_visited_count = 0;
            assert(m_gc_work_head == nulloffset);
            assert(m_gc_visit_head == nulloffset);
            m_gc_marked_count = 0;

            walk_all_roots();
            while (m_gc_work_head != nulloffset) {
                auto payload_offset = gc_work_pop();
                walk_block_contents(payload_offset);
            }

            // Restore off-list invariant on every visited Dict/Set; same as in
            // vm_global_gc_impl.  Must run before the next GC pass.
            gc_visit_clear_all();

            // Tabulation + restore pass: split live blocks into reachable
            // (m_mark_gen == current gen) vs unreachable, AND reset every
            // block's mark back to saved_gen so the probe leaves the heap
            // exactly as it found it (see the dry-run rationale above).
            auto current_gen = m_gc_current_gen;
            gvm_for_each([&result, current_gen, saved_gen, this](vm_offset_t payload_offset,
                                                                 ChunkKind kind,
                                                                 vm_size_t /*payload_size*/,
                                                                 vm_size_t block_size,
                                                                 bool is_free) {
                if (!is_free) {
                    // Skip the lazy GC scratch block from the live/dead tabulation.
                    // It survives every sweep by kind (gc_sweep_unmarked also
                    // skips it), so reporting it as "dead" because it has no
                    // mark would be misleading.  Its mark never tracks the
                    // generation, so it needs no restore either.
                    if (kind != ChunkKind::GcScratch) {
                        if (gvm_get_mark_gen(payload_offset) == current_gen) {
                            result.live_count++;
                            result.live_bytes += block_size;
                        } else {
                            result.dead_count++;
                            result.dead_bytes += block_size;
                        }
                        // Restore the pre-probe mark so the heap stays uniform.
                        gvm_set_mark_gen(payload_offset, saved_gen);
                    }
                }
            });
            m_gc_current_gen = saved_gen;
            return result;
        }
    }
}

//
// vm-global-gc-probe:  -- dict
//
// Returns a dict with /live, /dead, /live-bytes, /dead-bytes.  See
// vm_global_gc_probe() for semantics.
//
// throws: opstack-overflow, vm-full
static void vm_global_gc_probe_op(Trix *trx) {
    auto probe = trx->vm_global_gc_probe();
    auto [dict, dict_offset] = Dict::create_dict(trx, 4);
    using namespace std::literals::string_view_literals;
    dict->put(trx, Name::make(trx, "live"sv), Object::make_integer(static_cast<integer_t>(probe.live_count)));
    dict->put(trx, Name::make(trx, "dead"sv), Object::make_integer(static_cast<integer_t>(probe.dead_count)));
    dict->put(trx, Name::make(trx, "live-bytes"sv), Object::make_integer(static_cast<integer_t>(probe.live_bytes)));
    dict->put(trx, Name::make(trx, "dead-bytes"sv), Object::make_integer(static_cast<integer_t>(probe.dead_bytes)));
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_dict(dict_offset);
}
