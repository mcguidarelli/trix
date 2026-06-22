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

//===--- BindingTable: Per-Coroutine Name Binding Cache ---===//
//
// Each coroutine owns a small chained hash table mapping Name offsets to
// the DictEntry::m_value slots they currently resolve to.  The table is a
// mini-version of the global Name table: fixed-count bucket array, chain
// per bucket, entries allocated on the VM heap.
//
// --- Why per-coroutine ---
//
// A lookup cache shared on the Name object itself (Name::m_binding) races
// under multiple coroutines: Names are interned globally, so two coroutines
// that `def` the same name would stomp on each other's binding across a
// yield -- coroutine A defs /x = 1, coroutine B defs /x = 2, coroutine A
// wakes up and reads /x, gets 2.  Silent, far-from-source, uncatchable by `try`.
//
// Moving the cache onto the running coroutine eliminates the race at the
// root: each coroutine resolves /x through its own bucket array, against
// its own dict stack.  Coroutines never share a binding table entry.
//
// --- Shape ---
//
//   CoroutineContext::m_binding_table -> vm_offset_t bucket array
//       [BindingBucketCount vm_offsets -> head of chain (or nulloffset)]
//
//   each chain node:
//       BindingEntry { m_next, m_name_offset, m_value_offset, m_save_level, m_pad }
//
// Lazy-allocated: nulloffset until the first `binding_table_put` call for
// this coroutine.  A coroutine that never defs a name pays zero heap cost.
//
// --- Free lists ---
//
// Two single-size-class free lists in Trix member vars:
//   m_binding_bucket_free : 64-byte bucket arrays
//   m_binding_entry_free  : 16-byte BindingEntry blocks
// On coroutine death, the bucket array and every live entry are returned
// to their respective pools.  Parked bucket arrays thread through their
// buckets[0] slot; parked entries thread through m_next.
//
// --- Hash ---
//
// Keys are vm_offset_t Name offsets (4 bytes, well-distributed because they
// are bump-allocated at init time).  mix32 from hash.inl is plenty of
// avalanche for 16 buckets; modulus is (hash & (BindingBucketCount - 1)).

static constexpr length_t BindingBucketCount = 16;  // must be a power of two
static_assert((BindingBucketCount & (BindingBucketCount - 1)) == 0, "BindingBucketCount must be a power of two");

struct BindingEntry {
    vm_offset_t m_next;          // chain link (or free-list link while parked)
    vm_offset_t m_name_offset;   // Name offset (key)
    vm_offset_t m_value_offset;  // DictEntry::m_value offset
    save_level_t m_save_level;   // save level when this binding was established
    uint8_t m_pad[3];            // 3 bytes padding to 16-byte alignment
};
static_assert(sizeof(BindingEntry) == 16, "BindingEntry must be 16 bytes");

// Byte size of a per-coroutine bucket array: BindingBucketCount * sizeof(vm_offset_t).
static constexpr vm_size_t BindingBucketArraySize = BindingBucketCount * static_cast<vm_size_t>(sizeof(vm_offset_t));

// --- Bucket array allocation ---

[[nodiscard]] std::pair<vm_offset_t *, vm_offset_t> binding_table_alloc_bucket_array() {
    if (m_binding_bucket_free != nulloffset) {
        auto offset = m_binding_bucket_free;
        auto buckets = offset_to_ptr<vm_offset_t>(offset);
        m_binding_bucket_free = buckets[0];  // free-list link lives in slot 0 while parked
        for (length_t i = 0; i < BindingBucketCount; ++i) {
            buckets[i] = nulloffset;
        }
        return {buckets, offset};
    } else {
        // Bucket arrays land in global VM so they survive
        // restore for coroutines spawned in a save scope.
        auto [buckets, offset] = gvm_alloc_n<vm_offset_t>(BindingBucketCount, ChunkKind::BindingBucket);
        for (length_t i = 0; i < BindingBucketCount; ++i) {
            buckets[i] = nulloffset;
        }
        return {buckets, offset};
    }
}

void binding_table_free_bucket_array(vm_offset_t bucket_array_offset) {
    auto buckets = offset_to_ptr<vm_offset_t>(bucket_array_offset);
    buckets[0] = m_binding_bucket_free;
    m_binding_bucket_free = bucket_array_offset;
}

// --- Entry allocation ---

[[nodiscard]] std::pair<BindingEntry *, vm_offset_t> binding_table_alloc_entry() {
    if (m_binding_entry_free != nulloffset) {
        auto offset = m_binding_entry_free;
        auto entry = offset_to_ptr<BindingEntry>(offset);
        m_binding_entry_free = entry->m_next;
        return {entry, offset};
    } else {
        // Binding entries land in global VM (see
        // binding_table_alloc_bucket_array comment).
        return gvm_alloc<BindingEntry>(sizeof(BindingEntry), ChunkKind::BindingEntry);
    }
}

void binding_table_free_entry(BindingEntry *entry, vm_offset_t entry_offset) {
    entry->m_next = m_binding_entry_free;
    m_binding_entry_free = entry_offset;
}

// --- Operations ---

// Hash a Name offset to a bucket index in [0, BindingBucketCount).
[[nodiscard]] static length_t binding_bucket_index(vm_offset_t name_offset) {
    return static_cast<length_t>(mix32(name_offset) & (BindingBucketCount - 1));
}

// Look up a Name offset in the table.  Returns the stored value offset
// (a DictEntry::m_value offset) on hit, or nulloffset on miss.  Caller
// falls back to the dict-stack walk and may populate via binding_table_put.
[[nodiscard]] vm_offset_t binding_table_lookup(vm_offset_t bucket_array_offset, vm_offset_t name_offset) {
    if (bucket_array_offset == nulloffset) {
        return nulloffset;
    } else {
        auto buckets = offset_to_ptr<vm_offset_t>(bucket_array_offset);
        auto curr = buckets[binding_bucket_index(name_offset)];
        while (curr != nulloffset) {
            auto entry = offset_to_ptr<BindingEntry>(curr);
            if (entry->m_name_offset == name_offset) {
                return entry->m_value_offset;
            } else {
                curr = entry->m_next;
            }
        }
        return nulloffset;
    }
}

// Insert or update an entry.  Takes the owning coroutine's binding-table
// slot BY POINTER (not by value with a returned offset) so the lazily
// allocated bucket array is PUBLISHED into *table_slot before the entry
// allocation below: binding_table_alloc_entry's gvm_alloc can fire
// vm_global_gc, and gc_walk_coroutine_context reaches the bucket array only
// through this slot.  A bucket array held merely in a C local across that
// GC would be unmarked and swept (the global free lists are drained at GC
// entry), leaving the stored offset dangling -- a corruption that surfaces
// only on the first put for a coroutine, since later puts pass a non-null,
// already-rooted slot.  Publishing here roots it for both allocations.
void binding_table_put(vm_offset_t *table_slot, vm_offset_t name_offset, vm_offset_t value_offset) {
    if (*table_slot == nulloffset) {
        auto [_, new_offset] = binding_table_alloc_bucket_array();
        *table_slot = new_offset;  // root via gc_walk_coroutine_context before the entry alloc
    }
    auto bucket_array_offset = *table_slot;
    auto buckets = offset_to_ptr<vm_offset_t>(bucket_array_offset);
    auto bucket_idx = binding_bucket_index(name_offset);
    auto curr = buckets[bucket_idx];
    while (curr != nulloffset) {
        auto entry = offset_to_ptr<BindingEntry>(curr);
        if (entry->m_name_offset == name_offset) {
            entry->m_value_offset = value_offset;
            entry->m_save_level = m_curr_save_level;  // re-stamp: the rebind lives at the current level
            return;
        } else {
            curr = entry->m_next;
        }
    }
    auto [entry, entry_offset] = binding_table_alloc_entry();
    // Refetch buckets: binding_table_alloc_entry may bump m_vm_ptr on the fresh-alloc path.
    buckets = offset_to_ptr<vm_offset_t>(bucket_array_offset);
    entry->m_next = buckets[bucket_idx];
    entry->m_name_offset = name_offset;
    entry->m_value_offset = value_offset;
    entry->m_save_level = m_curr_save_level;
    buckets[bucket_idx] = entry_offset;
}

// Remove an entry for the given Name offset, if present.  No-op otherwise.
void binding_table_remove(vm_offset_t bucket_array_offset, vm_offset_t name_offset) {
    if (bucket_array_offset != nulloffset) {
        auto buckets = offset_to_ptr<vm_offset_t>(bucket_array_offset);
        auto bucket_idx = binding_bucket_index(name_offset);
        auto prev_link = &buckets[bucket_idx];
        auto curr = *prev_link;
        while (curr != nulloffset) {
            auto entry = offset_to_ptr<BindingEntry>(curr);
            auto next = entry->m_next;
            if (entry->m_name_offset == name_offset) {
                *prev_link = next;
                binding_table_free_entry(entry, curr);
                return;
            } else {
                prev_link = &entry->m_next;
                curr = next;
            }
        }
    }
}

// Tombstone every entry whose value offset falls in [range_start, range_end).
// Used when a Dict leaves the running coroutine's dict stack: its DictEntry
// memory may be reused or overwritten, so cached pointers into it must go.
void binding_table_clear_pointing_into(vm_offset_t bucket_array_offset, vm_offset_t range_start, vm_offset_t range_end) {
    if (bucket_array_offset != nulloffset) {
        auto buckets = offset_to_ptr<vm_offset_t>(bucket_array_offset);
        for (length_t i = 0; i < BindingBucketCount; ++i) {
            auto prev_link = &buckets[i];
            auto curr = *prev_link;
            while (curr != nulloffset) {
                auto entry = offset_to_ptr<BindingEntry>(curr);
                auto next = entry->m_next;
                if ((entry->m_value_offset >= range_start) && (entry->m_value_offset < range_end)) {
                    *prev_link = next;
                    binding_table_free_entry(entry, curr);
                } else {
                    prev_link = &entry->m_next;
                }
                curr = next;
            }
        }
    }
}

// Null every Name::m_binding field.  Called on the 0 -> 1 gate transition
// (first spawn flips the system from single-coroutine mode to multi-
// coroutine mode) and on restore in single-coroutine mode.  After this,
// every name lookup misses the global cache and walks the dict stack;
// the per-coroutine tables become the authoritative cache from here on.
// Cost: O(total interned names) -- once per gate transition, not per
// lookup.  Typically ~700 system names plus user names = sub-millisecond.
void flush_all_name_bindings() {
    auto bucket_count = m_name_bucket_count;
    for (name_bucket_count_t i = 0; i < bucket_count; ++i) {
        auto offset = m_name_buckets[i];
        while (offset != nulloffset) {
            auto name = offset_to_ptr<Name>(offset);
            name->clear_binding();
            offset = name->next();
        }
    }
}

// Called when m_live_coroutine_count is about to rise above 0 for the
// first time (or after any 1 -> 0 transition has been reversed).  Flushes
// the single-coroutine cache so the per-coroutine tables that are about
// to populate aren't shadowed by stale Name::m_binding pointers from the
// pre-spawn era.
void on_first_spawn_transition() {
    flush_all_name_bindings();
}

// Called when m_live_coroutine_count drops back to 0 (the last spawned
// coroutine died).  Main's per-coroutine table may have entries from the
// multi-coroutine period that are now stale; drop the whole table so
// subsequent lookups use the global Name::m_binding fast path (which is
// currently empty from the 0 -> 1 flush, and will repopulate lazily).
void on_last_death_transition() {
    if (m_main_context != nulloffset) {
        auto main_ctx = offset_to_ptr<CoroutineContext>(m_main_context);
        binding_table_free_all(main_ctx->m_binding_table);
        main_ctx->m_binding_table = nulloffset;
    }
}

// Walk every live coroutine's binding table and remove any entry keyed
// on `name_offset`.  Called when a Name is undef'd in a dict that could
// be reachable from other coroutines (userdict, systemdict, etc.) --
// the running coroutine would otherwise be the only one whose cached
// pointer gets cleared, and peers would keep returning pointers into
// the now-nulled DictEntry slot.  Cheap: O(live coroutines) per undef,
// and undef is not on any hot path.
void binding_remove_all_coroutines(vm_offset_t name_offset) {
    if (m_coroutine_head != nulloffset) {
        auto curr_offset = m_coroutine_head;
        do {
            auto ctx = offset_to_ptr<CoroutineContext>(curr_offset);
            if (ctx->m_status != CoroutineContext::Dead) {
                binding_table_remove(ctx->m_binding_table, name_offset);
            }
            curr_offset = ctx->m_next;
        } while (curr_offset != m_coroutine_head);
    }
}

// Walk every live coroutine's binding table and tombstone any entry
// whose value_offset falls in [range_start, range_end).  Called when a
// Dict is leaving the dict stack (pop on `end`, @end-locals, etc.).
// Conservative: hits every coroutine, not just the running one, in case
// the popped dict was reachable from a peer's stack too.
void binding_clear_all_coroutines_pointing_into(vm_offset_t range_start, vm_offset_t range_end) {
    if (m_coroutine_head != nulloffset) {
        auto curr_offset = m_coroutine_head;
        do {
            auto ctx = offset_to_ptr<CoroutineContext>(curr_offset);
            if (ctx->m_status != CoroutineContext::Dead) {
                binding_table_clear_pointing_into(ctx->m_binding_table, range_start, range_end);
            }
            curr_offset = ctx->m_next;
        } while (curr_offset != m_coroutine_head);
    }
}

// Save/restore: prune cache entries whose m_save_level is at or above the
// restore_level being undone.  After the restore the current save level will
// be restore_level - 1, so entries stamped at restore_level or deeper cache
// pointers established while the save journal was recording -- the journal has
// now rolled their underlying slots back, making the cached pointers stale.
// Entries at strictly lower levels are preserved (the journal reverted any
// value mutations at their slots, so their cached pointers stay correct).
//
// The save-level check alone unlinks every entry whose memory VM rollback will
// reclaim: binding_table_put always stamps m_save_level = m_curr_save_level, so
// any above-barrier entry is caught by the level test.  The whole chain is
// walked (no early-out on the first above-barrier entry) so below-barrier
// trailing entries reachable via m_next are not missed.
//
// Called BEFORE VM rollback, so all entry memory is still readable.  An
// unlinked entry is pushed onto m_binding_entry_free for reuse only when its
// slot survives rollback: below-barrier entries (recycled slots re-stamped at
// the higher level) and global-VM entries (rollback never reclaims globals);
// without the push those slots would leak.  Above-barrier local-VM entries are
// skipped -- rollback reclaims their memory.
void binding_table_prune_above_level(vm_offset_t bucket_array_offset, save_level_t restore_level, vm_offset_t barrier) {
    if (bucket_array_offset != nulloffset) {
        auto buckets = offset_to_ptr<vm_offset_t>(bucket_array_offset);
        for (length_t i = 0; i < BindingBucketCount; ++i) {
            auto prev_link = &buckets[i];
            auto curr = *prev_link;
            while (curr != nulloffset) {
                auto entry = offset_to_ptr<BindingEntry>(curr);
                auto next = entry->m_next;
                if (entry->m_save_level >= restore_level) {
                    *prev_link = next;
                    if ((curr < barrier) || is_global(curr)) {
                        binding_table_free_entry(entry, curr);
                    }
                } else {
                    prev_link = &entry->m_next;
                }
                curr = next;
            }
        }
    }
}

// Walk every live coroutine's binding table and prune entries established
// at or above `restore_level`.  Preserves the save-level-aware invalidation
// the old Name::m_binding_level mechanism provided: pre-save bindings stay
// cached, only bindings at the undone level (and any deeper nesting) are
// dropped.
void binding_restore_all_coroutines(save_level_t restore_level, vm_offset_t barrier) {
    if (m_coroutine_head != nulloffset) {
        auto curr_offset = m_coroutine_head;
        do {
            auto ctx = offset_to_ptr<CoroutineContext>(curr_offset);
            if (ctx->m_binding_table != nulloffset) {
                if (!is_global(ctx->m_binding_table) && (ctx->m_binding_table >= barrier)) {
                    // Bucket array itself was allocated post-save in local VM --
                    // rollback reclaims it entirely, nothing to prune.  Newly-
                    // allocated bucket arrays land in global VM, so this branch
                    // is effectively dead in normal operation.
                    ctx->m_binding_table = nulloffset;
                } else {
                    // Bucket array survives (below barrier or in global VM).
                    // Prune above-level entries; binding_table_prune_above_level
                    // recycles global entries that would otherwise leak.
                    binding_table_prune_above_level(ctx->m_binding_table, restore_level, barrier);
                }
            }
            curr_offset = ctx->m_next;
        } while (curr_offset != m_coroutine_head);
    }
}

// Drop every entry and recycle the bucket array itself.  Called on
// coroutine death.  After this call the caller's m_binding_table slot
// should be set to nulloffset.
void binding_table_free_all(vm_offset_t bucket_array_offset) {
    if (bucket_array_offset != nulloffset) {
        auto buckets = offset_to_ptr<vm_offset_t>(bucket_array_offset);
        for (length_t i = 0; i < BindingBucketCount; ++i) {
            auto curr = buckets[i];
            while (curr != nulloffset) {
                auto entry = offset_to_ptr<BindingEntry>(curr);
                auto next = entry->m_next;
                binding_table_free_entry(entry, curr);
                curr = next;
            }
            buckets[i] = nulloffset;
        }
        binding_table_free_bucket_array(bucket_array_offset);
    }
}

//===--- GC walkers ---===//
//
// gc_walk_binding_bucket: a BindingBucket block is a flat array of
// BindingBucketCount vm_offset_t.  Each slot is either nulloffset
// or the head of a chain of BindingEntry blocks.  Mark each non-null
// chain head; the BindingEntry walker chains m_next from there.
//
void gc_walk_binding_bucket(vm_offset_t payload_offset) {
    auto *buckets = offset_to_ptr<vm_offset_t>(payload_offset);
    for (length_t i = 0; i < BindingBucketCount; ++i) {
        mark_global_offset(buckets[i]);
    }
}

//
// gc_walk_binding_entry: chain link to the next entry, plus the
// Name offset (the binding's key).  m_value_offset points INTO a
// DictEntry::m_value slot (mid-block, not a payload offset); the
// owning Dict is reached via dict-stack roots, so we don't follow
// m_value_offset from here.  m_save_level is metadata.
//
void gc_walk_binding_entry(vm_offset_t payload_offset) {
    auto *entry = offset_to_ptr<BindingEntry>(payload_offset);
    mark_global_offset(entry->m_next);
    mark_global_offset(entry->m_name_offset);
}
