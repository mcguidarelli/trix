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

//===--- Save: Transaction Save / Restore ---===//
//
// Implements transaction-like save/restore checkpoints for the VM.  This is
// the undo mechanism that underpins error recovery, logic backtracking, and
// scoping.  Based on:
//
//   PostScript save/restore: the original mechanism for capturing and rolling
//   back VM state.  `save` creates a checkpoint; `restore` rolls back
//   modifications since that checkpoint (array element changes, dict entries,
//   heap allocations).  Note: unlike PostScript, Trix does NOT journal string
//   byte writes -- they persist across restore by design.  String buffer
//   reallocations are likewise unjournaled (the new buffer's vm_offset_t
//   sits in a String's m_offset slot which IS journaled by save_object,
//   so the slot itself rolls back, but the bytes the buffer ever held
//   are not snapshotted).
//
//   Database write-ahead logging (WAL): the journal records the old value
//   before each mutation, enabling replay-in-reverse to undo changes.
//
// --- Core concepts for maintainers ---
//
// SAVE LEVELS
//   Each save() call creates a new save level (m_curr_save_level increments).
//   Each Object carries a save_level_t recording when it was last modified.
//   restore() rolls back to the target level by replaying the journal.
//   Save levels form a stack (m_save_stack); nested saves/restores are
//   supported up to the configured depth (--save-depth).
//
// JOURNAL (save journal)
//   The journal is a linked list of Entry records on the VM heap, one per
//   mutation.  Each Entry records:
//     - Flavor: what was mutated (Object, DictHeader, DictEntry,
//       DictEntryNext, PackedName2/3/4, SetEntry, SetEntryNext,
//       StreamInfixOffset, LvarBinding)
//     - m_next: link to previous entry (LIFO chain for reverse replay)
//     - m_data[]: snapshot of the old bytes (inline for small data,
//       extended allocation for larger data)
//
//   Journal entries are created by save_object(), save_dict_entry(),
//   save_dict_header(), etc.  These are called by Dict::put, Dict::undef,
//   array element writes, packed name binding, and other mutation sites
//   throughout the codebase.
//
// RESTORE PROCESS
//   restore() walks the journal chain in LIFO order, copying each entry's
//   saved bytes back to their original location (memcpy).  This undoes
//   mutations in reverse chronological order.  After replay, m_vm_ptr is
//   reset to its saved position, discarding all heap allocations since
//   the save (O(1) bulk deallocation).
//
// WHAT IS JOURNALED
//   - Array/Packed element writes (save_object)
//   - Dict/Set entry mutations (save_dict_entry, save_set_entry)
//   - Dict bucket chain heads (save_dict_header)
//   - Dict/Set entry predecessor links (save_dict_entry_next, save_set_entry_next)
//   - Packed array name binding bytes (save_packed_name)
//   - Stream infix offsets (save_stream_infix_offset)
//
// WHAT IS NOT JOURNALED
//   - String byte writes via `put` (persist across restore by design)
//   - Heap allocations (reclaimed by m_vm_ptr rollback, not journaled)
//   - ExtValue/WideValue mutations (heap-cell scalars tracked via
//     save_level on the Object; reclaimed/relocated on restore, not journaled)
//
// INTERACTION WITH OTHER SYSTEMS
//   - Logic backtracking: choice/fail use save/restore for automatic
//     variable unbinding (the journal IS the Prolog trail).
//   - Error recovery: try-catch uses save/restore to roll back state
//     after a failed operation.
//   - Coroutines: each coroutine shares the save stack (save levels are
//     global, not per-coroutine).  restore() scans suspended coroutines
//     for heap-referencing objects above the barrier (raises invalid-restore
//     to prevent dangling references).  Coroutines not active during the
//     save scope are skipped via m_activation_sl optimization.
//
class Save {
private:
    // Each Flavor records what kind of location was mutated so restore() knows
    // how many bytes to copy back and how to interpret the save record:
    //   Object:        a single Object field (e.g. an array element, dict variable)
    //   DictHeader:    the head-of-chain vm_offset_t in the bucket array of a Dict
    //                  (changed when a new entry is prepended to a collision chain)
    //   DictEntry:     the entire Dict::DictEntry struct (changed when a key's value is updated)
    //   DictEntryNext: only the m_next field of a Dict::DictEntry -- captures the forward-link
    //                  mutation when a new entry is inserted ahead of an existing one
    //   PackedName2/3/4: 2-4 bytes of inline-encoded name data within a packed array
    //   LvarBinding:   a single Object field (an lvar binding cell) that lives in
    //                  GLOBAL VM.  Same payload shape as Object, but flagged
    //                  distinctly because it is the ONLY flavor whose journaled
    //                  slot (m_ptr) is intentionally global: an lvar created inside
    //                  ${...} has a global inner array, yet a binding made during
    //                  backtracking (find-all / choice / aggregate) MUST be undone
    //                  by the per-alternative restore.  save_data journals it
    //                  despite the global slot; gc_walk_chain PINS the owning block
    //                  so it cannot be freed/realloced before replay; replay reverts
    //                  it exactly like an Object.  A plain Object entry never targets
    //                  global (save_object short-circuits), so the two are disjoint.
    enum struct Flavor : uint8_t {
        Object,
        DictHeader,
        DictEntry,
        DictEntryNext,
        PackedName2,
        PackedName3,
        PackedName4,
        SetEntry,
        SetEntryNext,
        StreamInfixOffset,
        LvarBinding
    };

    // packed name layout: 1-byte header + N-byte value
    static constexpr length_t PackedNameHeaderSize{1};

    // count_for(flavor, bucket_count) -- byte size of the journal data
    // payload for a given Flavor.  bucket_count is meaningful only for
    // DictHeader (whose payload size depends on the dict's hash-table
    // capacity); pass 0 for all other flavors.
    //
    // Single source of truth used by:
    //   * the save_* convenience wrappers (save_dict_entry, ...) to
    //     pass the right `size` argument to save_data;
    //   * restore()'s replay switch, to know how many bytes to copy
    //     back into the dst slot;
    //   * validate_journal, to bound-check the post-data range
    //     (entry->m_ptr + count) against active VM.
    //
    // Adding a new flavor: extend the enum above AND add a case here.
    // -Wswitch-enum + -Werror enforce coverage; an unreachable trailing
    // return guards against a corrupted byte cast to Flavor at runtime.
    [[nodiscard]] static vm_size_t count_for(Flavor flavor, dict_bucket_count_t bucket_count = 0) {
        switch (flavor) {
        case Flavor::Object:
            return vm_sizeof<Object>();

        case Flavor::DictHeader:
            return Dict::alloc_size(bucket_count);

        case Flavor::DictEntry:
            return vm_sizeof<Dict::DictEntry>();

        case Flavor::DictEntryNext:
            return sizeof(vm_offset_t);

        case Flavor::PackedName2:
            return (PackedNameHeaderSize + 2);

        case Flavor::PackedName3:
            return (PackedNameHeaderSize + 3);

        case Flavor::PackedName4:
            return (PackedNameHeaderSize + 4);

        case Flavor::SetEntry:
            return vm_sizeof<Dict::SetEntry>();

        case Flavor::SetEntryNext:
            return sizeof(vm_offset_t);

        case Flavor::StreamInfixOffset:
            return sizeof(vm_offset_t);

        case Flavor::LvarBinding:
            return vm_sizeof<Object>();  // same payload shape as Object
        }
        // Reachable only if m_flavor is a corrupted byte outside the
        // enum.  Returning 0 makes downstream consumers (replay copy_n,
        // validate_journal bound check) trivially safe; the caller that
        // matters (restore replay) hits its own default-error branch
        // before this returns.
        return 0;
    }
    static constexpr length_t MinPackedNameValueSize{2};

    struct Entry {                           // 12 bytes
        vm_offset_t m_next;                  // next Save in linked list
        vm_offset_t m_ptr;                   // location to restore data
        dict_bucket_count_t m_bucket_count;  // DictHeader bucket count
        Flavor m_flavor;                     // flavor of data saved
        vm_t m_data[1];                      // first byte of data to restore (variable-length tail; see save_data)

        [[nodiscard]] vm_t *data() { return m_data; }
        [[nodiscard]] const vm_t *data() const { return m_data; }
    };
    static_assert(sizeof(Entry) == 12);
    // m_data lives at byte offset 11 (vm_offset_t * 2 + dict_bucket_count_t + 1)
    // -- not aligned for any type larger than vm_t.  Reads through data()
    // that interpret the bytes as Object / Dict::DictEntry / Dict::SetEntry
    // MUST go through std::memcpy to a local rather than reinterpret_cast.
    // See gc_walk_chain (this file).
    // Reordering fields for nicer alignment would require a snapshot bump
    // (Entry layout is journaled byte-for-byte) and is not worth the churn.
    static_assert(offsetof(Entry, m_data) == 11);

    // Result of check_stack: per-stack counts of relocatable heap cells above the barrier.
    struct RelocateCounts {
        stack_depth_t extvalue;
        stack_depth_t widevalue;
    };

    // Error if any non-ExtValue/WideValue composite Object (array, string, dict, etc.) lives
    // beyond the restore barrier.  Count and return the number of relocatable Objects beyond
    // the barrier -- these are moved below the barrier by the caller (ExtValue::restore /
    // WideValue::restore).
    static RelocateCounts check_stack(Trix *trx, vm_offset_t barrier, Object *stack_base, Object *stack_ptr, const char *desc) {
        auto counts = RelocateCounts{0, 0};
        for (auto curr_ptr = stack_base; curr_ptr <= stack_ptr; ++curr_ptr) {
            // Global-region offsets are above the barrier numerically but
            // never roll back, so they never dangle on restore.  Exclude
            // them from both the ExtValue/WideValue relocation counts and
            // the composite-Object error path.
            if (curr_ptr->uses_vm() && (curr_ptr->offset() >= barrier) && !trx->is_global(curr_ptr->offset())) {
                if (curr_ptr->uses_extvalue()) {
                    ++counts.extvalue;
                } else if (curr_ptr->uses_widevalue()) {
                    ++counts.widevalue;
                } else {
                    char buffer[ObjectNameBufferSize];

                    static_cast<void>(trx->object_name(curr_ptr, buffer));
                    trx->error(Error::InvalidRestore, "{} past 'save' barrier present in {} Stack during restore", buffer, desc);
                }
            }
        }
        return counts;
    }

    [[nodiscard]] static bool any_relocatable(RelocateCounts counts) { return ((counts.extvalue | counts.widevalue) != 0); }

    // Dict-stack handling at restore time.
    //
    // Frame dicts (dicts tagged with Dict::IsFrame, pushed by |locals|#N) are
    // excluded from the standard "no above-barrier objects" rule: the dict
    // stack may carry freshly-allocated frame dicts whose offsets sit above
    // the barrier without tripping /invalid-restore.  The VM rollback is
    // about to discard their storage, so we compact them out of the dict
    // stack in place; any below-barrier dicts above them slide down to
    // preserve their relative order.  Non-frame dicts above the barrier
    // remain illegal and trigger /invalid-restore exactly as before.
    //
    // Called in place of the dict-stack check_stack invocation.  Above-
    // barrier non-frame dicts trigger /invalid-restore (matching the
    // check_stack contract); above-barrier frame dicts on
    // the *running* coroutine's dict stack are compacted out in place
    // (their locals revert by design -- that's what save/restore is).
    //
    // Suspended coroutines need a different policy and use
    // check_suspended_dict_stack: their above-barrier frame dicts hold
    // locals the coroutine's proc body expects to find on resume, so
    // we cannot drop them silently.  Save level is global across
    // coroutines, so a coroutine whose first activation lands inside
    // an outer save scope lazy-allocates its locals frame at sl > 0;
    // letting that restore proceed would either reclaim the frame's
    // storage (offset becomes dangling) or, with this validator,
    // surface the situation as /invalid-restore so the user can move
    // the spawn-and-bootstrap before the save (yield once at sl=0
    // after each actor-spawn / coroutine-launch).  Symptom this
    // catches: tetrix's ai-tick-actor first activated after
    // game-actor's loop-body save, then on resume hit
    // Dict::bucket_magic_for(count=0) walking the dict stack.
    static void check_and_compact_dict_stack(Trix *trx, vm_offset_t barrier) {
        // Pass 1: validate.  Any above-barrier Object that is not a frame
        // dict is an error, matching the check_stack contract.
        // Global-region dicts pass through (numerically above-barrier but
        // never reclaimed by restore).
        for (auto curr_ptr = trx->m_dict_base; curr_ptr <= trx->m_dict_ptr; ++curr_ptr) {
            if (curr_ptr->uses_vm() && (curr_ptr->offset() >= barrier) && !trx->is_global(curr_ptr->offset())) {
                auto dict = curr_ptr->dict_value(trx);
                if (!dict->is_frame()) {
                    char buffer[ObjectNameBufferSize];

                    static_cast<void>(trx->object_name(curr_ptr, buffer));
                    trx->error(Error::InvalidRestore, "{} past 'save' barrier present in Dictionary Stack during restore", buffer);
                }
            }
        }

        // Pass 2: compact in place.  Drop above-barrier frame dicts; keep
        // everything else in original order (including global dicts -- they
        // survive restore).  VM rollback will reclaim the dropped dicts'
        // memory, so no recycle call is required (and would be unsafe --
        // Dict::recycle writes the soon-to-be-reclaimed cell).
        auto write_ptr = trx->m_dict_base;
        for (auto read_ptr = trx->m_dict_base; read_ptr <= trx->m_dict_ptr; ++read_ptr) {
            auto is_frame_above = read_ptr->uses_vm() && (read_ptr->offset() >= barrier) && !trx->is_global(read_ptr->offset());
            if (!is_frame_above) {
                if (write_ptr != read_ptr) {
                    *write_ptr = *read_ptr;
                }
                ++write_ptr;
            }
        }
        trx->m_dict_ptr = write_ptr - 1;
    }

    // Validate-only counterpart for suspended coroutines: any above-
    // barrier dict (frame or non-frame) is an error.  See the comment
    // above for why we cannot compact frame dicts here -- the
    // suspended coroutine's proc body expects the locals to be there
    // when it resumes.
    static void check_suspended_dict_stack(Trix *trx, vm_offset_t barrier, Object *dict_base, Object *dict_ptr) {
        for (auto curr_ptr = dict_base; curr_ptr <= dict_ptr; ++curr_ptr) {
            // Global-region dicts pass through; they never roll back so
            // a suspended coroutine holding one is not in danger.
            if (curr_ptr->uses_vm() && (curr_ptr->offset() >= barrier) && !trx->is_global(curr_ptr->offset())) {
                char buffer[ObjectNameBufferSize];

                static_cast<void>(trx->object_name(curr_ptr, buffer));
                trx->error(Error::InvalidRestore,
                           "{} past 'save' barrier present in suspended coroutine's Dictionary Stack during restore",
                           buffer);
            }
        }
    }

    static Entry *save_data(Trix *trx, const void *data, Flavor flavor, vm_size_t size) {
        auto data_offset = trx->ptr_to_offset(data);
        // Global region is journal-skipped by default: mutations to global
        // storage never roll back, so there is nothing to record.  Short-circuit
        // before the BASE check so global mutations at BASE level (which are also
        // legal -- globals don't depend on save scope) don't raise InvalidRestore.
        //
        // EXCEPTION (Flavor::LvarBinding): a logic-variable binding cell is
        // transient even when the lvar lives in global VM -- backtracking
        // (find-all / choice / aggregate) MUST undo it across the per-alternative
        // restore.  lvar_bind routes through save_object_journal_global, which
        // tags the entry LvarBinding so the binding write IS journaled even on a
        // global slot; restore then reverts it like a local one.  gc_walk_chain
        // pins the owning block (mark_global_offset on the entry's global m_ptr)
        // so the slot stays valid across any GC between the bind and the restore
        // -- the block cannot be freed/realloced out from under the entry.  A bind
        // at BASE level is permanent (no active save to roll back), so it is still
        // skipped.  The flavor is the single source of truth: no other flavor ever
        // targets a global slot.
        if (trx->is_global(data_offset)) {
            if ((flavor != Flavor::LvarBinding) || (trx->m_curr_save_level == BASE)) {
                return nullptr;
            }
        } else if (trx->m_curr_save_level == BASE) {
            trx->error(Error::InvalidRestore, "save_data: called at BASE level with no active save");
        }

        // Dedup: skip if the chain HEAD already has an entry for this exact location and
        // flavor.  Only the most recent entry is checked -- a full chain walk would be
        // O(n) per save; the head check catches the common case of repeated writes.
        auto barrier_offset = trx->m_save_stack[trx->m_curr_save_level];
        auto chain_head_offset = trx->offset_to_value<vm_offset_t>(barrier_offset);
        if (chain_head_offset != nulloffset) {
            auto head = trx->offset_to_ptr<Entry>(chain_head_offset);
            if ((head->m_ptr == data_offset) && (head->m_flavor == flavor)) {
                return head;
            }
        }

        static constexpr vm_size_t embedded_size = sizeof(Entry::m_data);
        auto extra_size = (size > embedded_size) ? (size - embedded_size) : 0;
        auto entry_alloc_size = vm_sizeof<Entry>() + extra_size;
        auto [entry_ptr, entry_offset] = trx->vm_alloc<Entry>(entry_alloc_size);

        entry_ptr->m_ptr = data_offset;
        entry_ptr->m_bucket_count = 0;  // only used by DictHeader; zero others for clean memory
        entry_ptr->m_flavor = flavor;
        std::copy_n(static_cast<const vm_t *>(data), size, entry_ptr->data());

        // add to head of restore chain.  Note: must re-resolve the
        // chain-head pointer here -- the vm_alloc<Entry> above may
        // have reallocated/moved the heap base, invalidating any
        // pointer captured before the alloc.  Hoisting this fetch
        // earlier looks like a benign refactor but breaks under heap
        // growth; the captured pointer would dangle.
        auto chain_head = trx->offset_to_ptr<vm_offset_t>(barrier_offset);
        entry_ptr->m_next = *chain_head;
        *chain_head = entry_offset;
        ++trx->m_save_journal_count[trx->m_curr_save_level];
        trx->m_save_journal_bytes[trx->m_curr_save_level] += entry_alloc_size;
        return entry_ptr;
    }
public:
    static constexpr save_level_t BASE{0};

    // Public accessors for user-operator code that needs to journal
    // mutations correctly outside class Trix scope.  current_level()
    // returns the topmost active save level (BASE when none active);
    // is_active() is the common-case shorthand "are we inside a save?".
    [[nodiscard]] static save_level_t current_level(const Trix *trx) { return trx->m_curr_save_level; }
    [[nodiscard]] static bool is_active(const Trix *trx) { return (trx->m_curr_save_level != BASE); }

    //
    // GC: walk one per-save-level journal chain and mark
    // every Object cell captured in its entries.  Encapsulates the
    // private Flavor + Entry types so gc.inl doesn't need access to
    // them.
    //
    // Three flavors carry Object cells in their saved bytes:
    //
    //   * Flavor::Object    -- 1 Object payload
    //   * Flavor::DictEntry -- 2 Objects (key + value) preceded by m_next
    //   * Flavor::SetEntry  -- 1 Object (key)         preceded by m_next
    //
    // When such a journaled Object holds a global-VM payload offset
    // (the slot used to reference a global Dict / Array / etc. before
    // being overwritten), the GC mark phase MUST follow it -- the
    // referenced block has to survive until the save scope is
    // committed/restored or the restore would dangle.
    //
    // Other flavors (DictHeader / DictEntryNext / SetEntryNext /
    // PackedName2/3/4 / StreamInfixOffset) have no Object cells in
    // their saved bytes; this walker skips them.
    //
    static void gc_walk_chain(Trix *trx, vm_offset_t chain_head_offset) {
        if (chain_head_offset != nulloffset) {
            auto *chain_head = trx->offset_to_ptr<vm_offset_t>(chain_head_offset);
            auto entry_off = *chain_head;
            while (entry_off != nulloffset) {
                auto *entry = trx->offset_to_ptr<Entry>(entry_off);
                switch (entry->m_flavor) {
                case Flavor::LvarBinding:
                    // The journaled slot (m_ptr) is an lvar's GLOBAL inner array
                    // (payload base).  PIN that block so it -- and its current
                    // binding value -- stay alive across this GC, keeping the slot
                    // valid until restore replays the entry into it (the block cannot
                    // be freed/realloced out from under the entry).  Then fall through
                    // to mark the saved OLD value exactly like a plain Object entry.
                    assert(trx->is_global(entry->m_ptr));
                    trx->mark_global_offset(entry->m_ptr);
                    [[fallthrough]];
                case Flavor::Object: {
                    // Plain Object entries journal LOCAL slots only -- save_data
                    // short-circuits a global slot for every flavor except
                    // LvarBinding, so a global m_ptr here would be a journaling bug.
                    assert((entry->m_flavor == Flavor::LvarBinding) || !trx->is_global(entry->m_ptr));
                    // Entry::m_data lives at byte offset 11 (after m_next/m_ptr/
                    // m_bucket_count/m_flavor), which is not aligned for Object
                    // (alignof == 8) or Dict::DictEntry/SetEntry (alignof == 4).
                    // memcpy a local copy to avoid UB; access happens only at GC
                    // time so the extra byte copy is negligible.
                    Object obj;
                    std::memcpy(&obj, entry->data(), sizeof(obj));
                    trx->gc_mark_object(obj);
                    break;
                }

                case Flavor::DictEntry: {
                    Dict::DictEntry de;
                    std::memcpy(&de, entry->data(), sizeof(de));
                    trx->gc_mark_object(de.m_key);
                    trx->gc_mark_object(de.m_value);
                    break;
                }

                case Flavor::SetEntry: {
                    Dict::SetEntry se;
                    std::memcpy(&se, entry->data(), sizeof(se));
                    trx->gc_mark_object(se.m_key);
                    break;
                }

                case Flavor::DictHeader:
                case Flavor::DictEntryNext:
                case Flavor::SetEntryNext:
                case Flavor::PackedName2:
                case Flavor::PackedName3:
                case Flavor::PackedName4:
                case Flavor::StreamInfixOffset:
                    break;
                }
                entry_off = entry->m_next;
            }
        }
    }

    // Returns true iff the value's backing storage would be reclaimed by a
    // restore from the current save level back to current-1.  Used by the
    // -persist operator family (put-persist, etc.) to reject above-barrier
    // values at write time -- storing one into a below-barrier container
    // would leave a dangling reference after the next restore.  Caller is
    // responsible for short-circuiting when !is_active(trx); the function
    // is otherwise safe to call (returns false at BASE).
    [[nodiscard]] static bool is_above_barrier(const Trix *trx, Object obj) {
        if (obj.uses_vm()) {
            // Global-region storage never rolls back, so it is never
            // "above the barrier" in the rollback sense.
            return ((obj.offset() >= trx->m_save_stack[trx->m_curr_save_level]) && !trx->is_global(obj.offset()));
        } else if (obj.uses_heap_cell()) {
            // ExtValue/WideValue cells: m_extvalue_save_level (aliased through
            // m_object_save_level via the Object header union) tracks the cell's
            // allocation level.  At-or-above the current level means the next
            // restore frees the cell.
            return (obj.save_level() >= trx->m_curr_save_level);
        } else {
            // Pure value types (Integer, Boolean, Byte, Real, Name, Mark):
            // data lives in the 8-byte Object slot itself.  Never above-barrier.
            // (Save tokens are inline Integers; covered by the Integer case.)
            return false;
        }
    }

    // Validate all journal chains for all active save levels.
    // Checks: every entry offset is within [0, vm_ptr), every entry's restore
    // target (m_ptr) is within [0, vm_ptr), every flavor is a valid enum value,
    // every entry's flavor-implied data length fits within active VM beyond
    // m_ptr, and chain lengths are bounded (no cycles).
    [[nodiscard]] static bool validate_journal(Trix *trx) {
        for (save_level_t lvl = 1; lvl <= trx->m_curr_save_level; ++lvl) {
            auto barrier_offset = trx->m_save_stack[lvl];
            if (!trx->valid_active_offset<vm_t>(barrier_offset)) {
                return false;
            } else {
                auto offset = trx->offset_to_value<vm_offset_t>(barrier_offset);
                integer_t chain_len = 0;

                while (offset != nulloffset) {
                    // entry itself must be within active heap
                    if (!trx->valid_active_offset<Entry>(offset)) {
                        return false;
                    } else {
                        auto entry = trx->offset_to_ptr<Entry>(offset);

                        // restore target must be within active heap
                        if (!trx->valid_active_offset<vm_t>(entry->m_ptr)) {
                            return false;
                        } else {
                            // flavor must be a valid enum value (0..9)
                            if (static_cast<uint8_t>(entry->m_flavor) > static_cast<uint8_t>(Flavor::LvarBinding)) {
                                return false;
                            } else {
                                // flavor-implied data range [m_ptr, m_ptr + count) must
                                // be entirely within active VM.  Catches the case where
                                // a corrupted m_flavor byte (e.g. Object flipped to
                                // DictEntry) passes the enum-bound check but tries to
                                // restore 20 bytes of data into an 8-byte slot during
                                // replay -- silent overrun.  count_for returns 0 for
                                // a Flavor outside the enum (defensive); the bound
                                // check below treats 0 as "no extra payload" and
                                // approves -- the enum-bound check above already
                                // catches the Flavor itself.
                                auto count = count_for(entry->m_flavor, entry->m_bucket_count);
                                if (count > 0) {
                                    // Compute the last byte offset in 64-bit: a corrupted m_ptr
                                    // near the top of the offset space plus count could wrap a
                                    // vm_offset_t to a small in-range value and slip past this
                                    // overrun guard.  Reject if it does not fit in vm_offset_t.
                                    const uint64_t last_offset = static_cast<uint64_t>(entry->m_ptr) + (count - 1);
                                    auto last_truncated = static_cast<vm_offset_t>(last_offset);
                                    if ((last_offset != last_truncated) || !trx->valid_active_offset<vm_t>(last_truncated)) {
                                        return false;
                                    }
                                }

                                offset = entry->m_next;
                                if (++chain_len > MaxSaveJournalChainLength) {
                                    return false;  // probable cycle or corruption
                                }
                            }
                        }
                    }
                }
            }
        }
        return true;
    }

    static void save(Trix *trx) {
        // allocate new save barrier and push on save stack
        if ((trx->m_curr_save_level + 1) >= trx->m_max_save_level) {
            trx->error(Error::LimitCheck, "save stack overflow: maximum {} levels exceeded", trx->m_max_save_level);
        } else {
            trx->require_op_capacity(1);

            auto [barrier_ptr, barrier_offset] = trx->vm_alloc<vm_offset_t>();
            // vm_alloc does NOT zero returned memory; the slot may hold
            // stale bytes from a prior occupant.  This init is the
            // chain-head-empty marker that save_data, restore, and
            // gc_walk_chain rely on -- load-bearing, do not remove.
            *barrier_ptr = nulloffset;
            trx->m_save_stack[++trx->m_curr_save_level] = barrier_offset;
            trx->m_vm_temp_save[trx->m_curr_save_level] = trx->m_vm_temp_ptr;
            trx->m_extvalue_active_save[trx->m_curr_save_level] = trx->m_extvalue_active_count;
            trx->m_widevalue_active_save[trx->m_curr_save_level] = trx->m_widevalue_active_count;
            trx->m_save_journal_count[trx->m_curr_save_level] = 0;
            trx->m_save_journal_bytes[trx->m_curr_save_level] = 0;
            // Bump the per-slot generation counter so a stale token
            // (level, gen ^ barrier_low23) from a prior occupant of this
            // slot detects mismatch on restore.  The 23-bit mask matches
            // the encoding window; the XOR-with-barrier in pack_token
            // makes wrap genuinely safe.
            trx->m_save_generation[trx->m_curr_save_level] =
                    (trx->m_save_generation[trx->m_curr_save_level] + 1) & SaveGenerationMask;

            // Pack (level, gen, barrier_low23) into the save token
            // and push it as an Integer Object.  Save tokens are inline
            // (no ExtValue allocation) and validated by Save::restore_token.
            auto token = pack_token(trx->m_curr_save_level, trx->m_save_generation[trx->m_curr_save_level], barrier_offset);

            // push save token (Integer) on the operand stack
            *++trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(token));
        }
    }

    // Save/restore protocol (executed in this order):
    //
    //   1. Assert no streaming allocation is in flight (m_vm_alloc_active).
    //   2. Find the matching save level by scanning m_save_stack for barrier.
    //   3. Rebuild scheduler ready/timer queues from surviving coroutines
    //      (restore_scheduler_lists).
    //   4. Drain the global-VM pool free lists (coroutine ctx/stack,
    //      binding bucket/entry, monitor) -- offsets above barrier dangle.
    //   5. Restore name bindings (single-coroutine fast path: full flush;
    //      multi-coroutine: per-entry prune above barrier).
    //   6. Walk operand/execution/error stacks to count ExtValue/WideValue
    //      Objects above the barrier (composite Objects above the barrier
    //      are illegal and trigger /invalid-restore).
    //   7. Compact the dict stack: above-barrier frame dicts are
    //      compacted in-place; non-frame above-barrier dicts error.
    //   8. Pop the save token from the operand stack (asserted to still
    //      be on top per the H1 token-skip invariant).
    //   9. Replay every journal chain newest-to-oldest via
    //      replay_journals.
    //  10. Roll back save level (m_curr_save_level = restore_level - 1)
    //      and VM heap pointer (m_vm_ptr = barrier).
    //  11. Restore m_vm_temp_ptr (clamped to m_vm_global if globals were
    //      interned during the rolled-back scope).
    //  12. Restore ExtValue/WideValue active-cell counts; relocate
    //      above-barrier cells via Object::ExtValue::restore /
    //      WideValue::restore.
    //  13. Reset dict pool entries above restore_level; prune frame-dict
    //      pool + frame-dict-overflow chain via prune_dict_chain.
    //  14. Trim heap-tracking side table (TRIX_HEAP_TRACK only).
    //  15. Stream::restore (close streams that opened above barrier);
    //      Name::restore (full-walk selective unlink).
    //  16. (No mailbox/supervision scrub -- registrations are global-VM
    //      offsets and survive restore; see the body comment.)
    //  17. Eq workspace flushes.
    //  18. Post-assertions on relocation completeness.
    static void restore(Trix *trx, vm_offset_t barrier) {
        // No streaming allocation may be in flight at restore entry.
        // m_vm_alloc_active points at memory that the VM rollback below
        // (m_vm_ptr = barrier) would reclaim out from under the
        // outstanding vm_start_alloc / vm_end_alloc / vm_trim_alloc
        // sequence.  No current path triggers a restore mid-streaming
        // allocation, but the assert guards against future changes
        // (e.g. an error during scanner streaming that crosses save
        // scopes).
        assert(trx->m_vm_alloc_active == nullptr);

        auto restore_level = find_save_level(trx, barrier);
        {
            // Every CoroutineContext lives in global VM (allocated via
            // gvm_alloc<CoroutineContext>(.., ChunkKind::CoroutineContext)),
            // so a per-barrier `m_context_offset >= barrier` test is
            // trivially true for every coroutine.  There is therefore no
            // above-barrier kill walk or dead-list unlink on restore: a
            // coroutine spawned at save level N survives an outer restore
            // past N, and its mailbox and monitor relationships persist
            // (also global).  Dead coroutines are unlinked when
            // joined/awaited, as usual.

            restore_scheduler_lists(trx);
            // The four free lists for coroutine context, stack, binding
            // bucket array, and binding entry all hold offsets into global
            // VM (the corresponding allocations land there via gvm_alloc).
            // Global offsets are above every local barrier by construction,
            // so there is no "drop entries above the barrier" walk for
            // them.  The m_frame_dict_pool walk further down is unaffected
            // (frame dicts still live in local VM).

            // Single-coroutine mode: the global Name::m_binding fast-path
            // cache has no per-name save-level tracking (the gate-transition
            // flush is the coarser mechanism).  Conservatively null every
            // m_binding on restore -- the next lookup walks the dict stack
            // and repopulates.  O(total interned names) per restore.
            //
            // Multi-coroutine mode: fine-grained per-entry pruning preserves
            // bindings at or below the restored level.
            if (trx->m_live_coroutine_count == 0) {
                trx->flush_all_name_bindings();
            } else {
                trx->binding_restore_all_coroutines(restore_level, barrier);
            }

            // The save token sits on top of the operand stack -- the caller
            // pushed it via `save` and we're about to consume it via the
            // `--trx->m_op_ptr` below.  check_stack would otherwise classify
            // it as an above-barrier Object and raise /invalid-restore.
            // Invariant for the gap between this snapshot and the pop below:
            // no helper between here and line 641's --m_op_ptr may push to
            // the operand stack, or that pushed value would be silently
            // popped instead of the token.
            [[maybe_unused]] auto saved_op_ptr_top = trx->m_op_ptr;
            auto op_ptr = (trx->m_op_ptr - 1);  // skip the save token at the top
            auto op_counts = check_stack(trx, barrier, trx->m_op_base, op_ptr, "Operand");
            auto exec_counts = check_stack(trx, barrier, trx->m_exec_base, trx->m_exec_ptr, "Execution");
            // Tally above-barrier relocatable scalars (ExtValue/WideValue) to move below
            // the barrier: the running coroutine's op/exec here, plus each suspended
            // coroutine's op/exec/err in the scan below.  ExtValue/WideValue::restore
            // relocate exactly this many (find_lowest_relocatable scans the same stacks).
            auto ext_relocate = static_cast<integer_t>(op_counts.extvalue) + static_cast<integer_t>(exec_counts.extvalue);
            auto wide_relocate = static_cast<integer_t>(op_counts.widevalue) + static_cast<integer_t>(exec_counts.widevalue);
            // Dict stack: frame dicts above the barrier are compacted out in
            // place; non-frame above-barrier dicts still trigger /invalid-restore.
            check_and_compact_dict_stack(trx, barrier);
            if (any_relocatable(check_stack(trx, barrier, trx->m_err_base, trx->m_err_ptr, "Error"))) {
                trx->error(Error::InternalError, "restore: error stack contains ExtValue/WideValue objects");
            }

            // Check suspended coroutine stacks for composite objects above the barrier.
            // Only scan coroutines that were active (ran) since the save was created --
            // coroutines that never ran during the save scope can't have acquired post-save
            // heap references.  The running coroutine is already checked above.
            if (trx->m_live_coroutine_count > 0) {
                auto curr_offset = trx->m_coroutine_head;
                do {
                    auto curr = trx->offset_to_ptr<CoroutineContext>(curr_offset);
                    if ((curr_offset != trx->m_running_coroutine) && (curr->m_status != CoroutineContext::Dead) &&
                        (curr->m_activation_sl >= restore_level)) {
                        auto co_op_base = trx->offset_to_ptr<Object>(curr->m_op_base);
                        auto co_op_ptr = trx->offset_to_ptr<Object>(curr->m_op_ptr) - 1;
                        auto co_exec_base = trx->offset_to_ptr<Object>(curr->m_exec_base);
                        auto co_exec_ptr = trx->offset_to_ptr<Object>(curr->m_exec_ptr) - 1;
                        auto co_err_base = trx->offset_to_ptr<Object>(curr->m_err_base);
                        auto co_err_ptr = trx->offset_to_ptr<Object>(curr->m_err_ptr) - 1;
                        // check_stack rejects this suspended coroutine's above-barrier
                        // COMPOSITES; accumulate its relocatable SCALARS so they are moved
                        // below the barrier (rather than left to dangle) by the relocation
                        // pass below -- the running coroutine's scalars are relocated, so a
                        // suspended coroutine's must be too.
                        auto co_op_c = check_stack(trx, barrier, co_op_base, co_op_ptr, "Coroutine Operand");
                        auto co_exec_c = check_stack(trx, barrier, co_exec_base, co_exec_ptr, "Coroutine Execution");
                        auto co_err_c = check_stack(trx, barrier, co_err_base, co_err_ptr, "Coroutine Error");
                        ext_relocate += static_cast<integer_t>(co_op_c.extvalue) + static_cast<integer_t>(co_exec_c.extvalue) +
                                        static_cast<integer_t>(co_err_c.extvalue);
                        wide_relocate += static_cast<integer_t>(co_op_c.widevalue) + static_cast<integer_t>(co_exec_c.widevalue) +
                                         static_cast<integer_t>(co_err_c.widevalue);
                        // Dict stack: validate-only.  Cannot compact frame
                        // dicts here -- the suspended coroutine's proc body
                        // needs them on resume.  CoroutineContext stores
                        // m_dict_ptr as "one past top" per coroutine_save.
                        auto co_dict_base = trx->offset_to_ptr<Object>(curr->m_dict_base);
                        auto co_dict_ptr = trx->offset_to_ptr<Object>(curr->m_dict_ptr) - 1;
                        check_suspended_dict_stack(trx, barrier, co_dict_base, co_dict_ptr);
                    }
                    curr_offset = curr->m_next;
                } while (curr_offset != trx->m_coroutine_head);
            }

            // pop save token from operand stack.  The assert pairs with the
            // saved_op_ptr_top snapshot ~40 lines above: every helper between
            // there and here MUST be net-zero on the operand stack, or the
            // pop here would discard a stranger's pushed Object instead of
            // the token.
            assert(trx->m_op_ptr == saved_op_ptr_top);
            --trx->m_op_ptr;

            // Guard before the loop: save_level_t is uint8_t; if restore_level == BASE (0),
            // the loop's --level would wrap 0->255 and spin forever.
            if (restore_level == BASE) {
                trx->error(Error::InvalidRestore, "restore at BASE level");
            }

            replay_journals(trx, restore_level);

            // restore save level
            trx->m_curr_save_level = static_cast<save_level_t>(restore_level - 1);

            // restore VM ptr
            trx->m_vm_ptr = trx->offset_to_ptr<vm_t>(barrier);

// TRIX_DEBUGGER: prune the per-op source-line annotation map for
// proc bodies whose storage offset now lies above the rewound
// m_vm_ptr.  New allocations after the rewind will reuse those
// offsets and would otherwise inherit stale line arrays from
// procs that no longer exist.  No-op in release builds.
#ifdef TRIX_DEBUGGER
            for (auto it = trx->m_debug_proc_lines.begin(); it != trx->m_debug_proc_lines.end();) {
                if (it->first >= barrier) {
                    it = trx->m_debug_proc_lines.erase(it);
                } else {
                    ++it;
                }
            }
#endif

            // restore temp region -- reclaim any temps orphaned by error
            // unwinding.  Clamp to m_vm_global: globals interned during the
            // rolled-back scope (Name allocations) shrink m_vm_global below
            // the saved temp_ptr; using the raw saved value would leave
            // m_vm_temp_ptr above m_vm_global, allowing future temp_alloc
            // to overwrite global storage.  Globals don't roll back, so
            // m_vm_global is authoritative for the post-restore ceiling.
            auto saved_temp_ptr = trx->m_vm_temp_save[restore_level];
            trx->m_vm_temp_ptr = (saved_temp_ptr > trx->m_vm_global) ? trx->m_vm_global : saved_temp_ptr;

            // restore ExtValue and WideValue active counts to their values at save time
            trx->m_extvalue_active_count = trx->m_extvalue_active_save[restore_level];
            trx->m_widevalue_active_count = trx->m_widevalue_active_save[restore_level];

            // clear dict pool for rolled-back levels -- dicts at the restored
            // level and below survive (per-save-level safety, same as ExtValue)
            for (auto level = static_cast<save_level_t>(trx->m_curr_save_level + 1); level < trx->m_max_save_level; ++level) {
                std::fill_n(&trx->m_dict_pool[static_cast<size_t>(level) * MaxDictPoolSize], MaxDictPoolSize, nulloffset);
            }
            // Frame-dict pool: not bucketed by save level, so we cannot wipe
            // wholesale on restore.  Walk each chain and drop entries whose
            // offset sits above the barrier -- those dicts are about to be
            // reclaimed by the VM rollback below and would dangle if left
            // linked.  Entries below the barrier are retained for reuse.
            for (length_t bucket = 0; bucket < MaxDictPoolSize; ++bucket) {
                prune_dict_chain(trx, &trx->m_frame_dict_pool[bucket], barrier);
            }
            // Same prune for the oversized-frame-dict overflow chain.
            prune_dict_chain(trx, &trx->m_frame_dict_overflow, barrier);
// vm-heap-tracking: side-table entries are appended in
// offset order, so all entries to drop sit at the tail.  Walk
// back from m_heap_track_count and discard any entry whose
// offset is at or above the barrier.  Bump the generation if
// anything was pruned so prior vm-heap-snapshot cursors can
// detect that they straddle a restore boundary.
#ifdef TRIX_HEAP_TRACKING
            {
                auto pruned = false;
                while ((trx->m_heap_track_count > 0) && (trx->m_heap_track[trx->m_heap_track_count - 1].m_offset >= barrier)) {
                    --trx->m_heap_track_count;
                    pruned = true;
                }
                if (pruned) {
                    ++trx->m_heap_track_generation;
                }
            }
#endif
            // (The mailbox pool is not per-save-level: mailboxes live in
            // global VM, BASE-immune to restore, and the pool is
            // class-indexed only.  No per-level reset needed.)

            // No mailbox scrub: m_blocked_reader / m_blocked_sender hold
            // CoroutineContext offsets, and contexts live in global VM --
            // they survive every restore (no restore-time kill walk; see
            // the preamble above), so parked registrations stay valid
            // across the heap rollback.  A historical scrub nulled any
            // registration at or above the barrier; once contexts moved
            // to global VM that test matched EVERY parked actor, and any
            // restore between an actor's recv-park and the next send lost
            // the wakeup forever (ENGINE BUG #12, the tetrix --ai-peek
            // wedge).  Death-driven cleanup is coroutine_kill's job.

            // (The monitor pool is not per-save-level: monitors live in
            // global VM, BASE-immune to restore, and the pool is a single
            // bare-offset head.  No per-level reset needed.
            // supervision_notify_death runs from coroutine_kill on
            // explicit death, never from restore.)
            // (The pipe pool is not per-save-level: pipe buffers live in
            // global VM, BASE-immune to restore, and the pool is
            // class-indexed only.  No per-level reset needed -- same shape
            // as the mailbox pool above.)

            // Clean up obsolete above-barrier streams + name-table entries BEFORE the
            // ExtValue/WideValue relocation below.  Relocation's alloc_local reuses
            // above-barrier space (it bumps m_vm_ptr up from the barrier), so any names /
            // streams still living there must be unlinked/closed first -- otherwise the
            // relocation clobbers them and Name::restore then walks a corrupted name-table
            // bucket chain (misaligned Name read).  m_vm_ptr is still == barrier here (the
            // relocation has not bumped it yet), so Name::restore's barrier is unchanged.
            // close obsolete stream
            Stream::restore(trx);
            // remove obsolete name entries (pass the restore barrier)
            Name::restore(trx, barrier);

            // Relocate obsolete ExtValue/WideValue scalars below the barrier, across the
            // running coroutine's op/exec stacks AND every suspended coroutine's saved
            // op/exec/err stacks (ext_relocate / wide_relocate tallied both above).
            Object::ExtValue::restore(trx, restore_level, ext_relocate);
            Object::WideValue::restore(trx, restore_level, wide_relocate);
            // Verify: after relocation, no ExtValue/WideValue Objects should reference offsets
            // above the CURRENT m_vm_ptr.  Use m_vm_ptr (not the original barrier) because
            // ExtValue::restore / WideValue::restore allocate new cells at/above the old barrier
            // -- those relocated values are legitimate and live below the new m_vm_ptr.
            [[maybe_unused]] auto post_barrier = static_cast<vm_offset_t>(trx->m_vm_ptr - trx->m_vm_base);
            assert(!any_relocatable(check_stack(trx, post_barrier, trx->m_op_base, trx->m_op_ptr, "Operand")));
            assert(!any_relocatable(check_stack(trx, post_barrier, trx->m_exec_base, trx->m_exec_ptr, "Execution")));

            // m_eqdict and m_eqset are excluded from save/restore journaling by design
            // (they are temporary scratch workspaces).  But any entries populated during
            // the rolled-back scope carry m_key/m_value VM offsets into heap that has
            // just been reclaimed.  A subsequent =dict / <<>>#= (or =set / {{}}#=) calls
            // reset_dict / reset_set which dereferences those dangling offsets via
            // clear_name_binding / maybe_free_extvalue, corrupting live memory at the
            // reused offset (or double-freeing a freelist).  Wipe both dicts here so the
            // next reset_dict sees no entries to clean up.
            trx->m_eqdict->quiet_flush(trx);
            trx->m_eqset->quiet_flush_set(trx);

            // reset error handling stacks, if storage is no longer in scope
            auto [dstack_head, dstack_length] = trx->m_dstack_ptr->array_value(trx);
            if ((dstack_length != 0) && (dstack_head->save_level() > trx->m_curr_save_level)) {
                *trx->m_ostack_ptr = Object::make_empty_array(trx, 0, Object::LiteralAttrib, Object::ReadOnlyAccess);
                *trx->m_dstack_ptr = Object::make_empty_array(trx, 0, Object::LiteralAttrib, Object::ReadOnlyAccess);
                *trx->m_estack_ptr = Object::make_empty_array(trx, 0, Object::LiteralAttrib, Object::ReadOnlyAccess);
            }
        }
    }

    static void save_object(Trix *trx, const Object *val_ptr) {
        save_data(trx, val_ptr, Flavor::Object, count_for(Flavor::Object));
    }

    // Journal an Object slot that may live in GLOBAL VM -- used ONLY for a
    // logic-variable binding cell (ops_logic.inl lvar_bind).  Tags the entry
    // Flavor::LvarBinding so save_data journals it even on a global slot (unlike
    // save_object / Flavor::Object, which short-circuits the global region): an
    // lvar binding is transient (backtracking undoes it) even when the lvar was
    // created inside ${...}, so the per-alternative restore must revert it.  For
    // a local slot it behaves exactly like save_object (same payload shape).
    // gc_walk_chain pins the owning block so a global slot stays valid between
    // bind and restore (the block cannot be freed/realloced out from under it).
    static void save_object_journal_global(Trix *trx, const Object *val_ptr) {
        save_data(trx, val_ptr, Flavor::LvarBinding, count_for(Flavor::LvarBinding));
    }

    // Reject a put/def into an eqref container (root EqArray, m_eqdict,
    // m_eqset) when the value's VM storage sits above the current save
    // barrier.  Eqref containers bypass save/restore journaling (the put
    // site's is_eqarray / is_eqdict / is_eqset branches skip save_object /
    // save_dict_entry / save_set_entry), so a post-barrier reference
    // stored in one would dangle after restore -- the stored Object
    // outlives the storage it references.  Accessors don't uniformly
    // catch this (Continuation checks save_level, Array / Dict etc.
    // do not).  See feedback_no_silent_footguns.md: reject at the write
    // site instead of hoping every deref path has a defensive check.
    //
    // No-op when: no save active, val has no VM offset, or val is itself
    // an eqref (its offset slot holds a generation counter, not a real
    // VM offset -- the referenced storage lives at BASE and cannot go
    // stale via barrier rollback; separate generation-staleness is a
    // different mechanism enforced at accessor time).
    // Is val_obj a "restore-fragile" local-VM value -- one whose storage was
    // allocated at save level >= 1 (above the BASE barrier) and would be reclaimed
    // by a future restore?  Restores can roll back to BASE but never below it, so
    // BASE allocations (offset < m_save_stack[1]) are immune.  Storing a fragile
    // value into a permanent-lifetime container (a global-VM block, or an eqref
    // container that bypasses journaling) leaves a dangling offset after restore.
    //
    // Returns false -- restore-immune -- when: no save is active (nothing above BASE
    // exists yet, and the m_save_stack[BASE + 1] read below assumes save level >= 1);
    // val is a pure value type (Integer/Boolean/Byte/Real/Mark -- no offset to
    // dangle); val is already in global VM; or val is a BASE-local allocation.
    [[nodiscard]] static bool is_restore_fragile_local(Trix *trx, Object val_obj) {
        if (trx->m_curr_save_level == BASE) {
            return false;
        } else if (!val_obj.uses_vm() && !val_obj.uses_heap_cell()) {
            return false;
        } else {
            auto offset = val_obj.storage_offset();
            if (trx->is_global(offset)) {
                return false;
            } else {
                return (offset >= trx->m_save_stack[BASE + 1]);
            }
        }
    }

    // Pointer-hygiene check: reject attempts to store a restore-fragile local-VM
    // value into a container that lives in global VM.  A global container has
    // permanent lifetime; storing a reference to local-VM storage that a future
    // restore will reclaim leaves the global container holding a dangling offset.
    // (Reactive cells take the opposite tack -- cell_set_core CLONES a fragile value
    // into the global region rather than rejecting, since a long-lived cell should
    // accept any value; see ops_reactive.inl.)
    // Reject a restore-fragile local value stored into a global container --
    // but ONLY a NON-SCALAR one.  A scalar (ExtValue/WideValue) fragile-local is
    // deep-cloned into global at the store (clone_fragile_scalar_into_global,
    // mirroring cell_set_core).  make_clone is SHALLOW for Name/String/composites
    // (it keeps the same local offset), so it CANNOT move them out of the
    // restore-fragile region -- storing one into a global container would dangle
    // after restore.  Reject those (the honest fail-safe): the caller must build
    // them in ${...} (global) or below the save barrier.  Call this UPFRONT (the
    // reject must precede any clone/free, so a throw leaves the caller's operands
    // intact).
    static void reject_local_into_global(Trix *trx, bool container_is_global, Object val_obj, const char *op_name) {
        if (container_is_global && is_restore_fragile_local(trx, val_obj) && !val_obj.uses_extvalue() &&
            !val_obj.uses_widevalue()) {
            trx->error(Error::InvalidAccess,
                       "{}: cannot store restore-fragile local-VM {} into global container",
                       op_name,
                       Object::type_sv(val_obj.type()));
        }
    }

    // Deep-clone a SCALAR (ExtValue/WideValue) restore-fragile local value into
    // the global region so a global container can hold it across restore (mirrors
    // cell_set_core's Option B), freeing the original local ExtValue.  Returns the
    // global clone, or val_obj unchanged when the container is local, val is
    // non-fragile, or val is a NON-scalar.  Non-scalar fragile-locals are rejected
    // UPFRONT by reject_local_into_global (make_clone is shallow for them and
    // cannot move them out of the fragile region); this function never errors, so
    // callers may invoke it AFTER freeing other operands without a throw-ownership
    // hazard.
    //
    // CALLER CONTRACT (GC-safety): the returned global clone lives only in the
    // caller's storage until moved into the container.  Either write it back to a
    // GC-scanned slot (an op-stack slot -- the array-store ops do this), or call
    // it immediately before the move-store with NO intervening allocation
    // (Dict::put / set_put, after expand / key-copy / journaling), so no
    // vm_global_gc sweeps the unrooted clone.  When two clones are live at once
    // (a dict's key+value into a not-yet-chained entry) the caller roots the first
    // across the second's alloc -- see Dict::put.
    [[nodiscard]] static Object clone_fragile_scalar_into_global(Trix *trx, bool container_is_global, Object val_obj) {
        if (!container_is_global || !is_restore_fragile_local(trx, val_obj)) {
            return val_obj;
        } else if (!val_obj.uses_extvalue() && !val_obj.uses_widevalue()) {
            return val_obj;  // non-scalar: rejected upfront; nothing make_clone can move
        } else {
            auto saved_global = trx->m_curr_alloc_global;
            trx->m_curr_alloc_global = true;
            auto global_value = val_obj.make_clone(trx);
            trx->m_curr_alloc_global = saved_global;
            val_obj.maybe_free_extvalue(trx);
            return global_value;
        }
    }

    static void reject_stale_value_into_eqref(Trix *trx, Object val_obj, const char *op_name) {
        if (!((trx->m_curr_save_level == BASE) || !val_obj.uses_vm() || val_obj.is_eqref())) {
            auto barrier = trx->m_save_stack[trx->m_curr_save_level];
            if (val_obj.offset() >= barrier) {
                trx->error(Error::InvalidRestore,
                           "{}: value allocated above the save barrier cannot be stored in an eqref container (would go stale on "
                           "restore)",
                           op_name);
            }
        }
    }

    static void save_dict_header(Trix *trx, const Dict *dictheader, dict_bucket_count_t bucket_count) {
        auto entry_ptr = save_data(trx, dictheader, Flavor::DictHeader, count_for(Flavor::DictHeader, bucket_count));
        // entry_ptr == nullptr when the dict lives in the global region
        // (save_data short-circuit -- nothing to journal).
        //
        // Post-dedup write: when save_data returned an existing head
        // entry (same m_ptr + m_flavor), we stomp its m_bucket_count
        // with the new value.  Safe because dict re-saves at the same
        // level always observe the same bucket_count -- Dict::expand
        // (rehash) ALWAYS allocates a fresh dict at a new offset
        // rather than rehashing in place, so a re-save targeting the
        // same dictheader pointer cannot have grown.  If that
        // invariant ever changes, this write must be guarded by a
        // bucket_count-comparison branch.
        if (entry_ptr != nullptr) {
            entry_ptr->m_bucket_count = bucket_count;
        }
    }

    static void save_dict_entry(Trix *trx, const Dict::DictEntry *dictentry) {
        save_data(trx, dictentry, Flavor::DictEntry, count_for(Flavor::DictEntry));
    }

    static void save_dict_entry_next(Trix *trx, const Dict::DictEntry *prev_entry) {
        save_data(trx, &prev_entry->m_next, Flavor::DictEntryNext, count_for(Flavor::DictEntryNext));
    }

    static void save_set_entry(Trix *trx, const Dict::SetEntry *setentry) {
        save_data(trx, setentry, Flavor::SetEntry, count_for(Flavor::SetEntry));
    }

    static void save_set_entry_next(Trix *trx, const Dict::SetEntry *prev_entry) {
        save_data(trx, &prev_entry->m_next, Flavor::SetEntryNext, count_for(Flavor::SetEntryNext));
    }

    static void save_stream_infix_offset(Trix *trx, const vm_offset_t *offset) {
        save_data(trx, offset, Flavor::StreamInfixOffset, count_for(Flavor::StreamInfixOffset));
    }

    static void save_packed_name(Trix *trx, const void *src, vm_size_t size) {
        constexpr auto min_size = (PackedNameHeaderSize + MinPackedNameValueSize);
        constexpr auto max_size = (PackedNameHeaderSize + 4);
        if ((size < min_size) || (size > max_size)) {
            trx->error(Error::InternalError, "save_packed_name: invalid size {}", size);
        } else {
            static constexpr Flavor sm_flavor[3] = {Flavor::PackedName2, Flavor::PackedName3, Flavor::PackedName4};
            auto flavor = sm_flavor[size - min_size];
            assert(size == count_for(flavor));
            save_data(trx, src, flavor, size);
        }
    }

    // Replay every journal entry from m_curr_save_level down to
    // restore_level, newest-to-oldest within each level (chains are
    // head-prepended at save time so head-to-tail walk IS reverse-
    // chronological).  Each entry's data slice is copied from the
    // entry's tail back into the destination slot at entry->m_ptr;
    // ExtValue/WideValue cells stored in payload Object slots are
    // freed BEFORE the overwrite via maybe_free_extvalue side-effect.
    //
    // count_for is the single source of truth for journal-data byte
    // length per flavor (also used by save_data + validate_journal).
    static void replay_journals(Trix *trx, save_level_t restore_level) {
        for (auto level = trx->m_curr_save_level; level >= restore_level; --level) {
            auto chain_head = trx->offset_to_ptr<vm_offset_t>(trx->m_save_stack[level]);
            vm_offset_t offset = *chain_head;
            while (offset != nulloffset) {
                auto save_entry = trx->offset_to_ptr<Entry>(offset);
                auto dst = trx->offset_to_ptr<void>(save_entry->m_ptr);

                // Side-effect step: free any ExtValues stored in
                // payload Object cells that are about to be
                // overwritten with their pre-mutation values.
                // Other flavors carry no Object cells and need no
                // pre-restore cleanup.
                switch (save_entry->m_flavor) {
                case Flavor::Object:
                case Flavor::LvarBinding: {
                    // LvarBinding shares Object's payload shape (1 Object); free
                    // the current binding's ExtValue (it may be a global Long,
                    // freed via gvm_free) before restoring the saved old value.
                    auto obj_ptr = static_cast<Object *>(dst);
                    obj_ptr->maybe_free_extvalue(trx);
                    break;
                }

                case Flavor::DictEntry: {
                    auto dict_entry = static_cast<Dict::DictEntry *>(dst);
                    dict_entry->m_key.maybe_free_extvalue(trx);
                    dict_entry->m_value.maybe_free_extvalue(trx);
                    break;
                }

                case Flavor::SetEntry: {
                    auto set_entry = static_cast<Dict::SetEntry *>(dst);
                    set_entry->m_key.maybe_free_extvalue(trx);
                    break;
                }

                case Flavor::DictHeader:
                case Flavor::DictEntryNext:
                case Flavor::SetEntryNext:
                case Flavor::PackedName2:
                case Flavor::PackedName3:
                case Flavor::PackedName4:
                case Flavor::StreamInfixOffset:
                    break;

                default:
                    // Fail closed on a corrupted m_flavor byte.
                    // validate_journal already bound-checks the enum;
                    // reaching here means either the journal was
                    // corrupted between save and restore, or a new
                    // flavor was added without a case here.
                    trx->error(Error::InternalError,
                               "restore: unknown save flavor {} at entry offset {}",
                               static_cast<integer_t>(static_cast<uint8_t>(save_entry->m_flavor)),
                               static_cast<integer_t>(offset));
                }

                auto count = count_for(save_entry->m_flavor, save_entry->m_bucket_count);
                std::copy_n(save_entry->data(), count, static_cast<vm_t *>(dst));
                offset = save_entry->m_next;
            }
        }
    }

    // Rebuild the scheduler ready/timer queues from surviving
    // coroutines after a restore has rolled back per-CoroutineContext
    // fields (m_status, m_timer_next, m_ready_next/prev,
    // m_wake_time_ns) to their save-time values.
    //
    // The queue HEADS (m_timer_head, m_ready_*_head/tail) and depth
    // counters are member vars, not VM-heap state, so they aren't
    // rolled back by the journal.  Pre-fix this path nulled the
    // heads, which silently amputated below-barrier sleeping
    // coroutines from the timer wheel (the gravity-actor freeze in
    // tetrix --ai-oracle mode).  Fix: clear stale per-survivor list
    // pointers, reset heads + depths, re-insert each survivor based
    // on its (rolled-back) m_status.
    //
    // Pass 1 clears every survivor's list-link slots (in case a
    // save-time link pointed at a since-killed neighbour).  Pass 2
    // re-inserts each survivor into the appropriate queue, skipping
    // the running coroutine (owns the CPU, not on any list),
    // Blocked (waits on a producer-side queue, journaled separately),
    // and Suspended (off scheduling by user request).
    static void restore_scheduler_lists(Trix *trx) {
        trx->m_ready_head = nulloffset;
        trx->m_ready_tail = nulloffset;
        trx->m_ready_high_head = nulloffset;
        trx->m_ready_high_tail = nulloffset;
        trx->m_timer_head = nulloffset;
        trx->m_ready_queue_depth = 0;
        trx->m_ready_high_queue_depth = 0;
        trx->m_timer_list_depth = 0;
        if (trx->m_live_coroutine_count != 0) {
            // Pass 1: clear stale list pointers on every survivor.
            {
                auto curr_offset = trx->m_coroutine_head;
                do {
                    auto curr = trx->offset_to_ptr<CoroutineContext>(curr_offset);
                    curr->m_ready_next = nulloffset;
                    curr->m_ready_prev = nulloffset;
                    curr->m_timer_next = nulloffset;
                    curr_offset = curr->m_next;
                } while (curr_offset != trx->m_coroutine_head);
            }
            // Pass 2: re-insert into the appropriate scheduler list.
            {
                auto curr_offset = trx->m_coroutine_head;
                do {
                    auto curr = trx->offset_to_ptr<CoroutineContext>(curr_offset);
                    const bool excluded = (curr_offset == trx->m_running_coroutine) ||
                                          ((curr->m_flags & CoroutineContext::FlagSuspended) != 0) ||
                                          ((curr->m_flags & CoroutineContext::FlagBlocked) != 0);
                    if (!excluded) {
                        if (curr->m_status == CoroutineContext::Sleeping) {
                            trx->timer_list_insert(curr);
                        } else if ((curr->m_status == CoroutineContext::Ready) || (curr->m_status == CoroutineContext::Created)) {
                            trx->ready_queue_push(curr);
                        }
                    }
                    curr_offset = curr->m_next;
                } while (curr_offset != trx->m_coroutine_head);
            }
        }
    }

    // Walk a frame-dict pool chain rooted at *chain_head and prune
    // entries whose offset is >= barrier (about to be reclaimed by
    // the VM rollback).  Below-barrier entries are retained for reuse
    // and re-linked through their Dict::m_pool slot.
    //
    // Used by restore() for both m_frame_dict_pool[bucket] and
    // m_frame_dict_overflow chains -- same shape, different roots.
    static void prune_dict_chain(Trix *trx, vm_offset_t *chain_head, vm_offset_t barrier) {
        auto *prev_link = chain_head;
        while (*prev_link != nulloffset) {
            auto offset = *prev_link;
            if (offset >= barrier) {
                auto dead_dict = trx->offset_to_ptr<Dict>(offset);
                *prev_link = dead_dict->m_pool;  // unlink (dead_dict memory is about to vanish)
                ++trx->m_frame_dict_pool_evictions;
            } else {
                auto live_dict = trx->offset_to_ptr<Dict>(offset);
                prev_link = &live_dict->m_pool;
            }
        }
    }

    // Finds the save level for a barrier offset on the save stack.
    // Returns the level, or errors if not found.
    [[nodiscard]] static save_level_t find_save_level(Trix *trx, vm_offset_t barrier) {
        for (save_level_t level = BASE; level < trx->m_max_save_level; ++level) {
            if (trx->m_save_stack[level] == barrier) {
                return level;
            }
        }
        trx->error(Error::InvalidRestore, "save barrier not found on save stack");
        std::unreachable();
    }

    [[nodiscard]] static save_level_t save_level(const Object *save_ptr) {
        return token_level(static_cast<vm_offset_t>(save_ptr->integer_value()));
    }

    //===--- Save-token packing ---===//
    //
    // Save tokens are inline 32-bit signed Integer Objects:
    //
    //   bits  0..7   : save level
    //   bits  8..30  : gen ^ barrier_low23  (XOR-folded validation field)
    //   bit  31      : sign  (0 = absolute token, 1 = -N relative pop)
    //
    // The XOR with the low 23 bits of the barrier offset means a stale
    // token (slot recycled with a different gen AND a different barrier)
    // is rejected even after the 23-bit gen field wraps.  False positive
    // requires both gen and barrier_low23 to coincide -- two uncorrelated
    // events in real workloads.
    //
    // For a valid token, m_save_stack[level] still holds the original
    // barrier (no recycling), so gen recovers cleanly via:
    //   gen = (token >> 8) ^ (m_save_stack[level] & 0x7FFFFF)
    //
    [[nodiscard]] static vm_offset_t pack_token(save_level_t level, save_generation_t gen, vm_offset_t barrier) {
        auto barrier_low = static_cast<save_generation_t>(barrier) & SaveGenerationMask;
        auto xor_field = (gen ^ barrier_low) & SaveGenerationMask;
        return (static_cast<vm_offset_t>(level) | (static_cast<vm_offset_t>(xor_field) << SaveTokenLevelBits));
    }

    [[nodiscard]] static save_level_t token_level(vm_offset_t token) {
        return static_cast<save_level_t>(token & SaveTokenLevelMask);
    }

    [[nodiscard]] static save_generation_t token_xor_field(vm_offset_t token) {
        return (static_cast<save_generation_t>((token >> SaveTokenLevelBits)) & SaveGenerationMask);
    }

    // Validate a save token: level in range, gen matches via XOR-with-barrier check.
    // Returns the level on success; raises /invalid-restore on stale or out-of-range.
    [[nodiscard]] static save_level_t validate_token(Trix *trx, vm_offset_t token) {
        auto level = token_level(token);
        if ((level == BASE) || (level > trx->m_curr_save_level)) {
            trx->error(Error::InvalidRestore,
                       "stale save token: level {} out of active range (1..{})",
                       static_cast<integer_t>(level),
                       static_cast<integer_t>(trx->m_curr_save_level));
        }
        auto current_barrier_low = static_cast<save_generation_t>(trx->m_save_stack[level]) & SaveGenerationMask;
        auto current_gen = trx->m_save_generation[level] & SaveGenerationMask;
        auto expected_xor = (current_gen ^ current_barrier_low) & SaveGenerationMask;
        if (token_xor_field(token) != expected_xor) {
            trx->error(Error::InvalidRestore, "stale save token: generation mismatch at level {}", static_cast<integer_t>(level));
        } else {
            return level;
        }
    }

    // Public restore entry: validate the token, look up the barrier, dispatch to restore.
    // Accepts the unsigned (positive-token) form -- callers branch on sign upstream
    // when they need to support the relative-pop encoding.
    static void restore_token(Trix *trx, vm_offset_t token) {
        auto level = validate_token(trx, token);
        restore(trx, trx->m_save_stack[level]);
    }

    // Restore using a signed save token:
    //   value > 0: positive packed token -- validate gen, dispatch
    //   value < 0: relative pop |N| save levels (no gen check; current by construction)
    //   value = 0: error /invalid-restore
    static void restore_signed_token(Trix *trx, integer_t signed_token) {
        if (signed_token > 0) {
            restore_token(trx, static_cast<vm_offset_t>(signed_token));
        } else if (signed_token < 0) {
            auto pop_count = -signed_token;
            if (pop_count > static_cast<integer_t>(trx->m_curr_save_level)) {
                trx->error(Error::InvalidRestore,
                           "relative restore: cannot pop {} levels, only {} active",
                           pop_count,
                           static_cast<integer_t>(trx->m_curr_save_level));
            }
            auto target_level = static_cast<save_level_t>(static_cast<integer_t>(trx->m_curr_save_level) - pop_count + 1);
            restore(trx, trx->m_save_stack[target_level]);
        } else {
            trx->error(Error::InvalidRestore, "save token of 0 is invalid (use -N for relative pop, or a positive token)");
        }
    }
};
