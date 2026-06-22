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

//===--- Reactive Cell Operators ---===//
//
// Implements a reactive (dataflow) computation model, commonly known as
// "spreadsheet-style" or "incremental" computation.  Based on concepts from:
//
//   Umut A. Acar, "Self-Adjusting Computation", PhD thesis, Carnegie Mellon
//   University, 2005.  (Foundational work on incremental recomputation.)
//
//   The observer/observable pattern from reactive programming (Rx, MobX,
//   SolidJS signals, Excel cell formulas).  The specific push-invalidation /
//   pull-recomputation design is closest to SolidJS signals and MobX.
//
// --- Core concepts for maintainers ---
//
// CELLS
//   A cell is a named container for a value that participates in a
//   dependency graph.  There are two kinds:
//
//   Base cell: holds a mutable value set directly by the user.
//     Created with `cell` (e.g., `42 cell` creates a cell holding 42).
//     Updated with `cell-set` (e.g., `99 my-cell cell-set` sets it to 99).
//     When updated, all cells that depend on it are invalidated.
//
//   Computed cell: holds a cached value derived from other cells.
//     Created with `cell-computed` (e.g., `{ x cell-get 2 mul } cell-computed`).
//     The proc is evaluated lazily on first `cell-get`.  The result is
//     cached until a dependency changes, at which point the cell is marked
//     dirty and will recompute on the next `cell-get`.
//
// DEPENDENCY TRACKING
//   When a computed cell's proc runs, every `cell-get` it calls is
//   recorded.  This builds the dependency graph automatically -- the user
//   never declares dependencies explicitly.
//
//   Each cell maintains two lists:
//     m_deps  -- cells this cell reads (only for computed cells)
//     m_rdeps -- cells that read this cell (reverse dependencies)
//
//   When a base cell is updated via `cell-set`, invalidation propagates
//   through the reverse dependency graph (m_rdeps), marking each
//   downstream cell as dirty.  This is a depth-first walk, bounded
//   by MaxInvalidateDepth to guard against cycles or deep chains.
//
// PUSH-INVALIDATION / PULL-RECOMPUTATION
//   The system uses a two-phase strategy:
//     Phase 1 (push): When a cell changes, invalidation propagates eagerly
//       through m_rdeps, marking dirty cells.  This is O(affected cells).
//     Phase 2 (pull): Recomputation happens lazily on `cell-get`.  A dirty
//       computed cell re-runs its proc, caches the result, and clears the
//       dirty flag.  Clean cells return their cached value immediately.
//
//   This avoids redundant recomputation: if a cell is invalidated multiple
//   times before anyone reads it, it recomputes only once.
//
// WATCHERS
//   A watcher is a user proc attached to a cell that fires when the cell's
//   value changes.  Watchers are side-effecting callbacks (e.g., logging,
//   updating external state).
//
//   Watchers fire synchronously during `cell-set` on a base cell: the base
//   cell's own watchers fire, and any downstream cell that has watchers is
//   collected and fired at the same time (a computed cell's watcher sees its
//   old cached value as the 'new' value; it is not refired when `cell-get`
//   later recomputes).  During a `batch` block, watcher firing is deferred
//   until the batch ends, allowing multiple updates to coalesce.
//
// BATCHING
//   `batch { ... }` groups multiple cell updates into a single transaction.
//   Invalidation still propagates immediately (cells are marked dirty),
//   but watcher firing is deferred until the batch ends.  This prevents
//   watchers from seeing intermediate states during a multi-cell update.
//
// NOT IN THE -PERSIST FAMILY
//   Reactive cells are deliberately NOT members of the persist mutator
//   family (put-persist, def-persist, ...) for a fundamental reason:
//   watchers are journaled puts into ordinary dicts.  A non-journaling
//   cell-set-persist would commit the cell's value across save/restore
//   while every watcher's downstream effect (journaled put into the
//   watcher's target dict) would revert -- the cell ends up persistent
//   but its dependents are reverted to a stale state.  That kind of
//   inconsistency cannot be papered over.
//
//   For persistent state in a reactive context, use `put-persist` on
//   a plain dict instead of a cell.  A reactive cell is the wrong
//   primitive for "value that survives transaction rollback" -- it is
//   designed for "value that fires watchers on change".  See
//   docs/trix-reference.md s7.7.1 "Deliberate omissions".
//
// CELL REPRESENTATION
//   Cells are a dedicated VM type (Type::Cell, slot 26).  The Object holds
//   a vm_offset_t pointing to a CellHeader struct on the VM heap.
//   CellHeader contains the value, compute proc (null for base cells),
//   dependency arrays, watcher array, validator proc, debug name, and
//   dirty/base flags.
//
// VALIDATED CELLS
//   `cell-validated` creates a base cell with a validator proc.  The
//   validator is called with the proposed value on every `cell-set` and
//   `cell-update` (and on creation).  It must return a boolean; false
//   raises /require and leaves the cell unchanged.  Validation uses the
//   trampoline pattern: the value is cloned onto the op stack for the
//   validator, which runs via the exec stack.  Control operators
//   @cell-validate-init and @cell-validate-set handle the result.
//
// DISPOSE
//   `cell-dispose` disconnects a cell from the dependency graph, clears
//   its watchers, and marks it with CellDisposed.  Any subsequent
//   cell-get or cell-set on a disposed cell raises /invalid-access.
//
// DEBUG NAMING
//   `cell-set-name` / `cell-name` attach an optional Name to a cell for
//   diagnostic display.  Named cells show as --cell:name:value-- in ==
//   output; unnamed cells show as --cell:value--.  A dirty cell shows
//   --cell:dirty-- and a disposed cell --cell:disposed-- (with the name
//   spliced in as --cell:name:dirty-- / --cell:name:disposed-- when named)
//   instead of the value.
//
// --- Operators ---
//
//   cell            value -- cell          Create a base cell
//   cell-computed   proc -- cell           Create a computed cell (lazy)
//   cell-validated  value proc -- cell     Create a validated base cell
//   cell-get        cell -- value          Read value (recompute if dirty)
//   cell-set        value cell --          Update base cell, invalidate rdeps
//   cell-value      cell -- value          Read cached value (no dep tracking)
//   cell-update     proc cell --           Atomic read-modify-write
//   cell-map        cell proc -- cell'     Computed cell from single source
//   cell-transduce  cell xf -- cell'       Computed cell via transducer
//   cell-combine    [cells] proc -- cell'  Computed cell from multiple sources
//   is-cell         any -- bool            Test if value is a cell
//   cell-dirty?     cell -- bool           Test if cell needs recomputation
//   cell-dispose    cell --                Disconnect and mark disposed
//   cell-deps       cell -- array          Forward dependencies
//   cell-rdeps      cell -- array          Reverse dependencies
//   cell-watchers   cell -- array          Watcher procs
//   cell-set-name   name cell --           Set debug name
//   cell-name       cell -- name true | false  Get debug name
//   watch           cell proc --           Attach a watcher proc
//   unwatch         cell proc --           Remove a watcher proc
//   batch           proc --                Deferred watcher firing
//
// Control operators (internal, not user-visible):
//   @cell-eval           Recomputation succeeded: cache result, track deps
//   @cell-eval-fail      Recomputation failed: restore m_current_cell, rethrow
//   @batch-end           Batch completed: fire deferred watchers
//   @batch-fail          Batch failed: clear deferred state, rethrow
//   @batch-fire          Post-batch watcher firing (after dirty recomputation)
//   @batch-fire-fail     Post-batch watcher firing failed: cleanup, rethrow
//   @cell-update-done    cell-update proc completed: push cell, invoke cell-set
//   @cell-validate-init  cell-validated initial check: create cell or error
//   @cell-validate-set   cell-set validator check: proceed to set or error
//

//===--- CellHeader ---===//

struct CellHeader {
    Object m_value;            // current value (base) or cached result (computed)
    Object m_proc;             // null for base cells; the compute proc for computed
    vm_offset_t m_deps;        // array of cell offsets this cell reads (computed only)
    vm_offset_t m_rdeps;       // array of cell offsets that read this cell
    vm_offset_t m_watchers;    // array of watcher procs
    length_t m_deps_len;       // number of deps
    length_t m_deps_cap;       // allocated capacity of deps array
    length_t m_rdeps_len;      // number of rdeps
    length_t m_rdeps_cap;      // allocated capacity of rdeps array
    length_t m_watchers_len;   // number of watchers
    length_t m_watchers_cap;   // allocated capacity of watchers array
    vm_offset_t m_debug_name;  // offset to Name for debug display (nulloffset = unnamed)
    Object m_validator;        // validator proc (null = no validation); checked on cell-set/cell-update
    bool m_dirty;              // 1 if invalidated, not yet recomputed
    uint8_t m_base;            // CellComputed, CellBase, CellDisposed
};

static constexpr uint8_t CellComputed = 0;
static constexpr uint8_t CellBase = 1;
static constexpr uint8_t CellDisposed = 2;

// Entry in the deferred watcher buffer used during batch execution.
// Records which cell changed and its value before the batch started.
struct BatchEntry {
    vm_offset_t m_cell_offset;
    Object m_old_value;
};

//===--- Helpers ---===//

// Add a cell offset to another cell's rdeps array.  Grows the array if needed.
static void cell_add_rdep(Trix *trx, vm_offset_t cell_offset, vm_offset_t dep_offset) {
    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
    // Check for duplicate
    if (header->m_rdeps != nulloffset) {
        auto rdeps = trx->offset_to_ptr<vm_offset_t>(header->m_rdeps);
        for (length_t i = 0; i < header->m_rdeps_len; ++i) {
            if (rdeps[i] == dep_offset) {
                return;  // already present
            }
        }
    }
    // Grow if needed
    if (header->m_rdeps_len == header->m_rdeps_cap) {
        auto new_cap = static_cast<length_t>((header->m_rdeps_cap == 0) ? 4 : (header->m_rdeps_cap * 2));
        auto [new_arr, new_offset] =
                trx->vm_alloc_dispatch_n<vm_offset_t>(static_cast<vm_size_t>(new_cap), Trix::ChunkKind::Address);
        if (header->m_rdeps != nulloffset) {
            auto old = trx->offset_to_ptr<vm_offset_t>(header->m_rdeps);
            std::memcpy(new_arr, old, header->m_rdeps_len * sizeof(vm_offset_t));
        }
        header->m_rdeps = new_offset;
        header->m_rdeps_cap = new_cap;
    }
    auto rdeps = trx->offset_to_ptr<vm_offset_t>(header->m_rdeps);
    rdeps[header->m_rdeps_len++] = dep_offset;
}

// Remove a cell offset from another cell's rdeps array.
static void cell_remove_rdep(Trix *trx, vm_offset_t cell_offset, vm_offset_t dep_offset) {
    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
    if (header->m_rdeps != nulloffset) {
        auto rdeps = trx->offset_to_ptr<vm_offset_t>(header->m_rdeps);
        for (length_t i = 0; i < header->m_rdeps_len; ++i) {
            if (rdeps[i] == dep_offset) {
                // Compact: shift trailing entries
                for (length_t j = i; j < (header->m_rdeps_len - 1); ++j) {
                    rdeps[j] = rdeps[j + 1];
                }
                --header->m_rdeps_len;
                break;
            }
        }
    }
}

// Add a cell offset to a cell's deps array (forward dependencies).  Grows if needed.
static void cell_add_dep(Trix *trx, vm_offset_t cell_offset, vm_offset_t dep_offset) {
    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
    // Check for duplicate
    if (header->m_deps != nulloffset) {
        auto deps = trx->offset_to_ptr<vm_offset_t>(header->m_deps);
        for (length_t i = 0; i < header->m_deps_len; ++i) {
            if (deps[i] == dep_offset) {
                return;
            }
        }
    }
    // Grow if needed (same pattern as rdeps)
    if (header->m_deps_len == header->m_deps_cap) {
        auto new_cap = static_cast<length_t>((header->m_deps_cap == 0) ? 4 : (header->m_deps_cap * 2));
        auto [new_arr, new_offset] =
                trx->vm_alloc_dispatch_n<vm_offset_t>(static_cast<vm_size_t>(new_cap), Trix::ChunkKind::Address);
        if (header->m_deps != nulloffset) {
            auto old = trx->offset_to_ptr<vm_offset_t>(header->m_deps);
            std::memcpy(new_arr, old, header->m_deps_len * sizeof(vm_offset_t));
        }
        header->m_deps = new_offset;
        header->m_deps_cap = new_cap;
    }
    auto deps = trx->offset_to_ptr<vm_offset_t>(header->m_deps);
    deps[header->m_deps_len++] = dep_offset;
}

// Remove a cell offset from another cell's deps array (forward deps).
static void cell_remove_dep(Trix *trx, vm_offset_t cell_offset, vm_offset_t dep_offset) {
    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
    if (header->m_deps != nulloffset) {
        auto deps = trx->offset_to_ptr<vm_offset_t>(header->m_deps);
        for (length_t i = 0; i < header->m_deps_len; ++i) {
            if (deps[i] == dep_offset) {
                for (length_t j = i; j < (header->m_deps_len - 1); ++j) {
                    deps[j] = deps[j + 1];
                }
                --header->m_deps_len;
                break;
            }
        }
    }
}

// Record a dependency: m_current_cell reads cell_offset.
// Adds to both the computing cell's deps (forward) and the read cell's rdeps (reverse).
static void cell_record_dep(Trix *trx, vm_offset_t cell_offset) {
    if (trx->m_current_cell != nulloffset) {
        cell_add_dep(trx, trx->m_current_cell, cell_offset);
        cell_add_rdep(trx, cell_offset, trx->m_current_cell);
    }
}

// Clear old deps: remove self from each dep's rdeps.
static void cell_clear_deps(Trix *trx, vm_offset_t self_offset) {
    auto header = trx->offset_to_ptr<CellHeader>(self_offset);
    if (header->m_deps != nulloffset) {
        auto deps = trx->offset_to_ptr<vm_offset_t>(header->m_deps);
        for (length_t i = 0; i < header->m_deps_len; ++i) {
            cell_remove_rdep(trx, deps[i], self_offset);
        }
        // Keep the allocation (m_deps, m_deps_cap) for reuse; just reset the count
        header->m_deps_len = 0;
    }
}

// Eager transitive invalidation via DFS.
// Marks all transitive dependents dirty and collects cells with watchers.
static void cell_invalidate(Trix *trx, vm_offset_t cell_offset, int depth) {
    if (depth > MaxInvalidateDepth) {
        trx->error(Error::LimitCheck, "cell invalidation: recursion depth exceeded (max {})", MaxInvalidateDepth);
    } else {
        auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
        if (header->m_rdeps != nulloffset) {
            auto rdeps = trx->offset_to_ptr<vm_offset_t>(header->m_rdeps);
            auto rdeps_len = header->m_rdeps_len;
            for (length_t i = 0; i < rdeps_len; ++i) {
                auto dep_offset = rdeps[i];
                auto dep = trx->offset_to_ptr<CellHeader>(dep_offset);
                if (!dep->m_dirty) {
                    dep->m_dirty = true;
                    cell_invalidate(trx, dep_offset, depth + 1);
                }
            }
        }
    }
}

// Add a watcher proc to a cell's watcher array.  Grows if needed.
static void cell_add_watcher(Trix *trx, vm_offset_t cell_offset, Object proc) {
    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
    if (header->m_watchers_len == header->m_watchers_cap) {
        auto new_cap = static_cast<length_t>((header->m_watchers_cap == 0) ? 4 : (header->m_watchers_cap * 2));
        auto [new_arr, new_offset] = trx->vm_alloc_dispatch_n<Object>(static_cast<vm_size_t>(new_cap), Trix::ChunkKind::Array);
        if (header->m_watchers != nulloffset) {
            auto old = trx->offset_to_ptr<Object>(header->m_watchers);
            std::memcpy(new_arr, old, header->m_watchers_len * sizeof(Object));
        }
        // Null-init the unused capacity slots [len, cap).  The block is tagged
        // ChunkKind::Array with obj_count == new_cap, so once cell_gc_walk marks it
        // reachable the generic block walker scans all new_cap slots -- gvm_alloc does
        // not zero, so an uninitialized tail slot would be read as a garbage Object and
        // trip the GC.  (deps/rdeps are ChunkKind::Address leaves, never walked as
        // Objects, so they don't need this.)  No GC can fire here: make_null does not
        // allocate, and new_arr is not linked into the header until below.
        std::fill_n(new_arr + header->m_watchers_len, static_cast<size_t>(new_cap - header->m_watchers_len), Object::make_null());
        header->m_watchers = new_offset;
        header->m_watchers_cap = new_cap;
    }
    auto watchers = trx->offset_to_ptr<Object>(header->m_watchers);
    watchers[header->m_watchers_len++] = proc;
}

// Fire watchers for a cell: push old_value, new_value, then execute each watcher proc.
// Called after invalidation completes (deferred execution model).
static void cell_fire_watchers(Trix *trx, vm_offset_t cell_offset, Object old_value, Object new_value) {
    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
    if (header->m_watchers_len != 0) {
        auto len = header->m_watchers_len;
        trx->require_op_capacity(static_cast<length_t>(len * 2));
        trx->require_exec_capacity(len);

        // old_value/new_value are unrooted C-local params (callers pass a fresh,
        // unrooted new_value.make_clone temporary).  For 2+ watchers the loop below
        // make_clones them per watcher, and under ${...} each clone's alloc can fire
        // vm_global_gc -- sweeping the still-borrowed source so a later clone reads
        // freed memory (watchers then see a poisoned/aliased value).  Root both sources
        // across the loop.  Nested-safe: this runs inside cell_set_core's gc-root scope,
        // so reserve incrementally and pop exactly our two, leaving caller roots intact.
        trx->require_gc_root_capacity_more(2);
        *++trx->m_gc_roots_ptr = old_value;
        *++trx->m_gc_roots_ptr = new_value;

        auto watchers = trx->offset_to_ptr<Object>(header->m_watchers);
        for (length_t i = 0; i < len; ++i) {
            if (i < (len - 1)) {
                // Clone for all but last watcher to avoid shared ExtValue
                *++trx->m_op_ptr = old_value.make_clone(trx);
                *++trx->m_op_ptr = new_value.make_clone(trx);
                watchers = trx->offset_to_ptr<Object>(trx->offset_to_ptr<CellHeader>(cell_offset)->m_watchers);
            } else {
                *++trx->m_op_ptr = old_value;
                *++trx->m_op_ptr = new_value;
            }
            *++trx->m_exec_ptr = watchers[i];
        }

        trx->gc_root_pop_n(2);
    }
}

// Record a cell for deferred watcher firing during a batch.
// Deduplicates: if the cell is already recorded, the new old_value is freed
// and the original (pre-batch) old_value is preserved.
static void cell_defer_watcher(Trix *trx, vm_offset_t cell_offset, Object old_value) {
    auto entries = trx->offset_to_ptr<BatchEntry>(trx->m_deferred_watchers);
    auto count = trx->m_deferred_watcher_count;

    // Deduplicate: keep original old_value (pre-batch snapshot)
    for (length_t i = 0; i < count; ++i) {
        if (entries[i].m_cell_offset == cell_offset) {
            old_value.maybe_free_extvalue(trx);
            return;
        }
    }

    // Add new entry if room
    if (count < MaxWatcherCollect) {
        entries[count].m_cell_offset = cell_offset;
        entries[count].m_old_value = old_value;
        trx->m_deferred_watcher_count = static_cast<length_t>(count + 1);
    } else {
        // Bounded buffer full; drop with ExtValue cleanup
        old_value.maybe_free_extvalue(trx);
    }
}

// Collect cells with watchers during invalidation, for deferred firing.
// Returns a list of {cell_offset, old_value} pairs.
// This walks the rdeps tree and collects any cell that has watchers and was just marked dirty.
static void cell_collect_watcher_cells(Trix *trx,
                                       vm_offset_t cell_offset,
                                       vm_offset_t *collect_buf,
                                       Object *old_values,
                                       length_t *collect_count,
                                       length_t collect_cap,
                                       int depth) {
    if (depth > MaxInvalidateDepth) {
        trx->error(Error::LimitCheck, "cell_collect_watcher_cells: recursion depth exceeded (max {})", MaxInvalidateDepth);
    } else {
        auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
        if (header->m_rdeps != nulloffset) {
            auto rdeps = trx->offset_to_ptr<vm_offset_t>(header->m_rdeps);
            auto rdeps_len = header->m_rdeps_len;
            for (length_t i = 0; i < rdeps_len; ++i) {
                auto dep_offset = rdeps[i];
                auto dep = trx->offset_to_ptr<CellHeader>(dep_offset);
                auto cc = *collect_count;
                if ((dep->m_watchers_len > 0) && (cc < collect_cap)) {
                    // Check if already collected (deduplicate)
                    bool already = false;
                    for (length_t j = 0; j < cc; ++j) {
                        if (collect_buf[j] == dep_offset) {
                            already = true;
                            break;
                        }
                    }
                    if (!already) {
                        // Defensive bound: ensure room for one more gc-root BEFORE cloning,
                        // so a push can never run past the gc-root stack even if a caller
                        // under-reserved -- raises a catchable /limit-check instead of a
                        // corrupting write, and erroring before the clone avoids leaking it.
                        // cell_set_core reserves MaxWatcherCollect + 1 up front and the
                        // cc < collect_cap guard caps adds at MaxWatcherCollect, so in
                        // practice this always passes.
                        trx->require_gc_root_capacity_more(1);
                        collect_buf[cc] = dep_offset;
                        old_values[cc] = dep->m_value.make_clone(trx);
                        // Root this clone so a later make_clone's vm_global_gc cannot sweep it
                        // before its watcher fires.  Popped by cell_set_core's reset_gc_root at
                        // its single return (counted into 1 + watcher_count - before_collect).
                        *++trx->m_gc_roots_ptr = old_values[cc];
                        *collect_count = static_cast<length_t>(cc + 1);
                    }
                }
                cell_collect_watcher_cells(trx, dep_offset, collect_buf, old_values, collect_count, collect_cap, depth + 1);
            }
        }
    }
}

static Object make_cell(Trix *trx, Object value, Object proc, Object validator) {
    // Region-aware: a cell created inside ${...} lands in the global GC heap so
    // its (possibly global) value/proc/deps survive; cell_gc_walk marks them.
    auto [header, offset] = trx->vm_alloc_dispatch<CellHeader>(vm_sizeof<CellHeader>(), Trix::ChunkKind::Cell);

    // m_value is save-level aware in cell_set_core and at_cell_eval_op
    value.set_save_level(trx->m_curr_save_level);
    auto proc_is_null = proc.is_null();

    header->m_value = value;
    header->m_proc = proc;
    header->m_deps = nulloffset;
    header->m_rdeps = nulloffset;
    header->m_watchers = nulloffset;
    header->m_deps_len = 0;
    header->m_deps_cap = 0;
    header->m_rdeps_len = 0;
    header->m_rdeps_cap = 0;
    header->m_watchers_len = 0;
    header->m_watchers_cap = 0;
    header->m_debug_name = nulloffset;
    header->m_validator = validator;
    header->m_dirty = !proc_is_null;
    header->m_base = proc_is_null ? CellBase : CellComputed;

    return Object::make_cell(offset);
}

// Mark every VM reference a CellHeader holds.  Called from walk_block_contents
// for a global cell block, and from gc_mark_object's local-VM descent for a
// LOCAL cell that holds global values -- without this a global object reachable
// only through a cell is swept by vm_global_gc (cells are not a GC root and the
// Cell chunk kind was a no-op leaf).  deps/rdeps hold cell offsets: a global one
// gets its block marked, a local one is silently skipped by mark_global_offset.
static void cell_gc_walk(Trix *trx, vm_offset_t cell_offset) {
    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
    trx->gc_mark_object(header->m_value);
    trx->gc_mark_object(header->m_proc);
    trx->gc_mark_object(header->m_validator);
    if (header->m_deps != nulloffset) {
        trx->mark_global_offset(header->m_deps);
        auto deps = trx->offset_to_ptr<vm_offset_t>(header->m_deps);
        for (length_t i = 0; i < header->m_deps_len; ++i) {
            trx->mark_global_offset(deps[i]);
        }
    }
    if (header->m_rdeps != nulloffset) {
        trx->mark_global_offset(header->m_rdeps);
        auto rdeps = trx->offset_to_ptr<vm_offset_t>(header->m_rdeps);
        for (length_t i = 0; i < header->m_rdeps_len; ++i) {
            trx->mark_global_offset(rdeps[i]);
        }
    }
    if (header->m_watchers != nulloffset) {
        trx->mark_global_offset(header->m_watchers);
        auto watchers = trx->offset_to_ptr<Object>(header->m_watchers);
        for (length_t i = 0; i < header->m_watchers_len; ++i) {
            trx->gc_mark_object(watchers[i]);
        }
    }
}

//===--- Standard Operators ---===//

// cell: value -- cell
// Creates a base cell with the given initial value.
// throws: opstack-underflow, vm-full
static void cell_op(Trix *trx) {
    trx->require_op_count(1);

    auto value = trx->m_op_ptr[0];
    auto proc = Object::make_null();
    auto validator = Object::make_null();

    *trx->m_op_ptr = make_cell(trx, value, proc, validator);
}

// computed: proc -- cell
// Creates a computed (derived) cell.  Starts dirty (will be evaluated on first cell-get).
// throws: opstack-underflow, type-check, vm-full
static void computed_op(Trix *trx) {
    trx->verify_operands(VerifyCallable);

    auto value = Object::make_null();
    auto proc = trx->m_op_ptr[0];
    auto validator = Object::make_null();

    *trx->m_op_ptr = make_cell(trx, value, proc, validator);
}

// cell-get: cell -- value
// Reads cell value.  If the cell is a dirty computed cell, triggers re-evaluation
// via trampoline.  Records dependency if inside a computed cell evaluation.
// throws: opstack-underflow, type-check
static void cell_get_op(Trix *trx) {
    trx->verify_operands(VerifyCell);

    auto cell_obj = *trx->m_op_ptr;
    auto cell_offset = cell_obj.cell_offset();
    auto header = cell_obj.cell_header(trx);

    if (header->m_base == CellDisposed) {
        trx->error(Error::InvalidAccess, "cell-get: cell is disposed");
    } else if (header->m_dirty && (header->m_base == CellComputed)) {
        trx->require_exec_capacity(5);

        // Computed cell needs re-evaluation.
        // Record dependency before trampoline (dirty path must also track deps)
        cell_record_dep(trx, cell_offset);
        // Clear old deps (remove self from old deps' rdeps)
        cell_clear_deps(trx, cell_offset);

        // Save m_current_cell on exec stack via control operator, then set to this cell
        // Exec stack layout: [dep_flag] [prev_current_cell] [cell_offset] [@cell-eval] [proc]
        *++trx->m_exec_ptr = Object::make_integer(1);  // dep_flag: record reverse dep in @cell-eval
        *++trx->m_exec_ptr = Object::make_uinteger(static_cast<uinteger_t>(trx->m_current_cell));
        *++trx->m_exec_ptr = Object::make_uinteger(static_cast<uinteger_t>(cell_offset));
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atCellEval);
        *++trx->m_exec_ptr = header->m_proc;

        trx->m_current_cell = cell_offset;

        // Pop cell from op stack -- proc result will be pushed by @cell-eval
        --trx->m_op_ptr;
    } else {
        // Not dirty -- return cached value (clone for op stack ownership)
        *trx->m_op_ptr = header->m_value.make_clone(trx);

        // Record dependency if inside computed cell evaluation
        cell_record_dep(trx, cell_offset);
    }
}

// Core cell-set logic: journal old value, write new value, invalidate rdeps, fire watchers.
// Expects op stack: [... new_value cell].  Pops both.
// Called by cell_set_op (non-validated path) and at_cell_validate_set_op (after validation).
static void cell_set_core(Trix *trx, vm_offset_t cell_offset) {
    auto new_value = trx->m_op_ptr[-1];
    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);

    // R6 pointer hygiene: a global cell outlives save/restore (its block is
    // journal-skipped, so the old m_value is not even restorable), so it must not
    // hold a restore-fragile LOCAL value -- storage above the BASE barrier that a
    // future restore would reclaim, leaving m_value dangling.  Unlike Dict::put
    // (which REJECTS such a store), a reactive cell is long-lived and should accept
    // any value, so deep-clone the value into the global region instead.  Write the
    // clone back to the op-stack slot so it stays GC-rooted across the allocations
    // below, and free the original local ExtValue (its storage is replaced, not
    // moved into m_value).  BASE-local / global / pure values pass through untouched.
    if (trx->is_global(cell_offset) && Save::is_restore_fragile_local(trx, new_value)) {
        auto saved_global = trx->m_curr_alloc_global;
        trx->m_curr_alloc_global = true;
        auto global_value = new_value.make_clone(trx);
        trx->m_curr_alloc_global = saved_global;
        new_value.maybe_free_extvalue(trx);
        trx->m_op_ptr[-1] = global_value;
        new_value = global_value;
    }

    // Old-values held from here until their watcher fires (or is deferred) are GLOBAL
    // ExtValue/WideValue clones under ${...}; they live only in C-stack storage, and the
    // make_clone calls below (Save::save_object journaling, cell_collect_watcher_cells,
    // the per-dep new-value clones in the fire loop) can fire vm_global_gc.  The precise
    // GC does not scan C locals, so it would sweep an earlier clone before its watcher
    // consumes it (the freed cell then reused by the next clone -> a corrupted old value).
    // Root them on the (pre-allocated) gc-root stack: the cell's own old_value here, and
    // each collected downstream old-value as cell_collect_watcher_cells clones it.  Up to
    // MaxWatcherCollect + 1 roots -- MaxGcRootDepth is sized for this.  No VM allocation
    // (cell-set must not gain a /vm-full failure mode on a full heap).  Reentrancy-safe:
    // the fire loop only stages watcher args/procs onto the op/exec stacks (where they
    // become rooted), so cell_set_core does not re-enter while these roots are live.  The
    // exact push count (cell + old_value + the clones collect adds) is reset_gc_root'd
    // at the single return below, validating the push/pop balance.
    trx->require_gc_root_capacity(MaxWatcherCollect + 2);

    // Root the cell itself across the rest of the operation.  cell_set_op leaves the
    // cell on the op stack, but the `m_op_ptr -= 2` below pops it -- and the cell may
    // be reachable ONLY via that slot (an undef'd inline cell, e.g.
    // `0l cell dup { .. } watch v exch cell-set`).  The collect / invalidate / fire
    // steps below all allocate and can fire vm_global_gc, which does not scan C locals:
    // an unrooted cell would be swept and its header (m_value / m_watchers /
    // m_watchers_len) read back as poison -- the own-watcher fire then reads a garbage
    // watcher count.  make_cell(offset) only wraps the offset (no alloc); rooting it
    // makes the GC mark the cell and descend its fields (m_value's ExtValue included,
    // which is what new_value.make_clone at the fire site reads).
    *++trx->m_gc_roots_ptr = Object::make_cell(cell_offset);

    // Clone old value: save/journal pattern below may free its ExtValue
    auto old_value = header->m_value.make_clone(trx);
    *++trx->m_gc_roots_ptr = old_value;

    // Journal old value for save/restore
    auto curr_save_level = trx->m_curr_save_level;
    if (header->m_value.save_level() != curr_save_level) {
        Save::save_object(trx, &header->m_value);
    } else {
        header->m_value.maybe_free_extvalue(trx);
    }
    header->m_value = new_value.make_copy(curr_save_level);

    trx->m_op_ptr -= 2;

    // Collect cells with watchers before invalidation changes their state
    vm_offset_t watcher_cells[MaxWatcherCollect];
    Object watcher_old_values[MaxWatcherCollect];
    length_t watcher_count = 0;

    // Check if this cell itself has watchers
    if (header->m_watchers_len > 0) {
        watcher_cells[0] = cell_offset;
        watcher_old_values[0] = old_value;
        watcher_count = 1;
    }

    // Collect downstream watcher cells and their old values before invalidation.  The
    // own-watcher slot (if any) reuses old_value's already-pushed root, so collect's
    // gc-root pushes are exactly the entries it adds: watcher_count - count_before_collect.
    auto watcher_count_before_collect = watcher_count;
    cell_collect_watcher_cells(trx, cell_offset, watcher_cells, watcher_old_values, &watcher_count, MaxWatcherCollect, 0);

    // Invalidate dependents (eager transitive DFS)
    cell_invalidate(trx, cell_offset, 0);

    // Fire watchers (deferred to after invalidation completes)
    if (trx->m_batch_depth == 0) {
        // Outside batch: fire immediately
        // For this cell's own watchers: old_value and new_value are known
        if (header->m_watchers_len > 0) {
            cell_fire_watchers(trx, cell_offset, old_value, new_value.make_clone(trx));
        }
        // For downstream cells: their watchers get old cached value and will get new on next cell-get
        // We fire watchers for cells that have them -- they receive old and new cached values
        for (length_t i = 0; i < watcher_count; ++i) {
            if (watcher_cells[i] != cell_offset) {
                auto dep = trx->offset_to_ptr<CellHeader>(watcher_cells[i]);
                auto dep_new = dep->m_value.make_clone(trx);
                cell_fire_watchers(trx, watcher_cells[i], watcher_old_values[i], dep_new);
            }
        }
    } else {
        // Inside batch: defer watchers for firing at @batch-end
        for (length_t i = 0; i < watcher_count; ++i) {
            cell_defer_watcher(trx, watcher_cells[i], watcher_old_values[i]);
        }
    }

    // If old_value was not consumed by any watcher path, free its ExtValue.
    // This happens when the cell has no watchers and no downstream cells have watchers.
    if (watcher_count == 0) {
        old_value.maybe_free_extvalue(trx);
    }

    // Drop the roots: cell (1) + old_value (1) + the downstream clones collect pushed
    // (watcher_count - watcher_count_before_collect).  reset_gc_root asserts (debug) that
    // this matches the actual push count, catching any miscount loudly here.
    trx->reset_gc_root(static_cast<integer_t>(2 + watcher_count - watcher_count_before_collect));
}

// cell-set: value cell --
// Updates a base cell's value; invalidates dependents; fires watchers.
// If the cell has a validator proc (from cell-validated), the validator is run
// via trampoline before the write.  On failure, raises Error::Require.
// throws: opstack-underflow, type-check, undefined-result, require
static void cell_set_op(Trix *trx) {
    trx->verify_operands(VerifyCell, VerifyAny);

    auto cell_obj = *trx->m_op_ptr;
    auto cell_offset = cell_obj.cell_offset();
    auto header = cell_obj.cell_header(trx);

    if (header->m_base == CellDisposed) {
        trx->error(Error::InvalidAccess, "cell-set: cell is disposed");
    } else if (header->m_base == CellComputed) {
        trx->error(Error::UndefinedResult, "cell-set: cannot set a computed cell");
    } else {
        auto new_value = trx->m_op_ptr[-1];

        // Equality-based skip: if new value equals old, no-op (no invalidation,
        // no watcher firing, no journal entry).  Standard in MobX/SolidJS.
        if (header->m_value.equal(trx, new_value)) {
            new_value.maybe_free_extvalue(trx);
            trx->m_op_ptr -= 2;
        } else if (header->m_validator.is_null()) {
            cell_set_core(trx, cell_offset);
        } else {
            trx->require_op_capacity(1);
            trx->require_exec_capacity(3);

            // Validated cell: trampoline to run validator before writing.
            // Op stack: [... value cell] -> push value copy -> [... value cell value_copy]
            // Validator consumes value_copy, pushes bool -> [... value cell bool]
            // @cell-validate-set pops bool, if true calls cell_set_core on [... value cell].
            auto validator_obj = header->m_validator;
            auto value_copy = new_value.make_clone(trx);

            *++trx->m_op_ptr = value_copy;

            *++trx->m_exec_ptr = Object::make_uinteger(static_cast<uinteger_t>(cell_offset));
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atCellValidateSet);
            *++trx->m_exec_ptr = validator_obj;
        }
    }
}

// cell-value: cell -- value
// Reads cell value without recording a dependency.
// If dirty computed cell, triggers re-evaluation (same as cell-get minus dep recording).
// throws: opstack-underflow, type-check
static void cell_value_op(Trix *trx) {
    trx->verify_operands(VerifyCell);

    auto cell_obj = *trx->m_op_ptr;
    auto cell_offset = cell_obj.cell_offset();
    auto header = cell_obj.cell_header(trx);

    if (header->m_base == CellDisposed) {
        trx->error(Error::InvalidAccess, "cell-value: cell is disposed");
    } else if (header->m_dirty && (header->m_base == CellComputed)) {
        trx->require_exec_capacity(5);

        // Same trampoline as cell-get but with dep_flag=0 to skip dep recording.
        cell_clear_deps(trx, cell_offset);

        // Exec stack layout: [dep_flag=0] [prev_current_cell] [cell_offset] [@cell-eval] [proc]
        *++trx->m_exec_ptr = Object::make_integer(0);  // dep_flag: no reverse dep in @cell-eval
        *++trx->m_exec_ptr = Object::make_uinteger(static_cast<uinteger_t>(trx->m_current_cell));
        *++trx->m_exec_ptr = Object::make_uinteger(static_cast<uinteger_t>(cell_offset));
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atCellEval);
        *++trx->m_exec_ptr = header->m_proc;

        // Point m_current_cell at this cell so its proc's reads rebuild its own
        // deps/rdeps; dep_flag=0 keeps the *caller* from being recorded as a dep.
        trx->m_current_cell = cell_offset;

        --trx->m_op_ptr;
    } else {
        // Not dirty -- return cached value (clone for op stack ownership, no dep recording)
        *trx->m_op_ptr = header->m_value.make_clone(trx);
    }
}

// is-cell: any -- bool
// Type predicate.
// throws: opstack-underflow
static void is_cell_op(Trix *trx) {
    trx->require_op_count(1);

    *trx->m_op_ptr = Object::make_boolean(trx->m_op_ptr->is_cell());
}

// cell-dirty?: cell -- bool
// True if the cell has been invalidated but not yet recomputed.
// throws: opstack-underflow, type-check
static void cell_dirty_op(Trix *trx) {
    trx->verify_operands(VerifyCell);

    auto header = trx->m_op_ptr->cell_header(trx);
    *trx->m_op_ptr = Object::make_boolean(header->m_dirty);
}

// cell-deps: cell -- array
// Returns the cells this cell reads (dependencies).  Empty for base cells.
// throws: opstack-underflow, type-check, vm-full
static void cell_deps_op(Trix *trx) {
    trx->verify_operands(VerifyCell);

    auto header = trx->m_op_ptr[0].cell_header(trx);
    auto len = header->m_deps_len;
    if (len == 0) {
        *trx->m_op_ptr = Object::make_empty_array(trx, 0);
    } else {
        auto deps = trx->offset_to_ptr<vm_offset_t>(header->m_deps);
        auto [arr, arr_offset] = trx->vm_alloc_dispatch_n<Object>(len, Trix::ChunkKind::Array);
        for (length_t i = 0; i < len; ++i) {
            arr[i] = Object::make_cell(deps[i]);
        }
        *trx->m_op_ptr = Object::make_array(arr_offset, len);
    }
}

// cell-rdeps: cell -- array
// Returns the cells that read this cell (reverse dependencies / dependents).
// throws: opstack-underflow, type-check, vm-full
static void cell_rdeps_op(Trix *trx) {
    trx->verify_operands(VerifyCell);

    auto header = trx->m_op_ptr[0].cell_header(trx);
    auto len = header->m_rdeps_len;
    if (len == 0) {
        *trx->m_op_ptr = Object::make_empty_array(trx, 0);
    } else {
        auto rdeps = trx->offset_to_ptr<vm_offset_t>(header->m_rdeps);
        auto [arr, arr_offset] = trx->vm_alloc_dispatch_n<Object>(len, Trix::ChunkKind::Array);
        for (length_t i = 0; i < len; ++i) {
            arr[i] = Object::make_cell(rdeps[i]);
        }
        *trx->m_op_ptr = Object::make_array(arr_offset, len);
    }
}

// cell-watchers: cell -- array
// Returns the watcher procs attached to this cell.
// throws: opstack-underflow, type-check, vm-full
static void cell_watchers_op(Trix *trx) {
    trx->verify_operands(VerifyCell);

    auto header = trx->m_op_ptr->cell_header(trx);
    auto len = header->m_watchers_len;
    if (len == 0) {
        *trx->m_op_ptr = Object::make_empty_array(trx, 0);
    } else {
        auto watchers = trx->offset_to_ptr<Object>(header->m_watchers);
        auto [arr, arr_offset] = trx->vm_alloc_dispatch_n<Object>(len, Trix::ChunkKind::Array);
        for (length_t i = 0; i < len; ++i) {
            arr[i] = watchers[i].make_clone(trx);
        }
        *trx->m_op_ptr = Object::make_array(arr_offset, len);
    }
}

// watch: cell proc --
// Register a change watcher on a cell.
// throws: opstack-underflow, type-check, invalid-access, vm-full
static void watch_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyCell);

    auto proc_obj = *trx->m_op_ptr--;
    auto cell_obj = *trx->m_op_ptr--;

    auto header = cell_obj.cell_header(trx);
    if (header->m_base == CellDisposed) {
        trx->error(Error::InvalidAccess, "watch: cell is disposed");
    } else {
        cell_add_watcher(trx, cell_obj.cell_offset(), proc_obj);
    }
}

// unwatch: cell proc --
// Unregister a change watcher.  No error if proc is not found (idempotent).
// throws: opstack-underflow, type-check
static void unwatch_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyCell);

    auto proc_obj = *trx->m_op_ptr--;
    auto cell_obj = *trx->m_op_ptr--;

    auto cell_offset = cell_obj.cell_offset();
    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
    if (header->m_watchers != nulloffset) {
        auto watchers = trx->offset_to_ptr<Object>(header->m_watchers);
        for (length_t i = 0; i < header->m_watchers_len; ++i) {
            if (watchers[i].equal(trx, proc_obj)) {
                for (length_t j = i; j < (header->m_watchers_len - 1); ++j) {
                    watchers[j] = watchers[j + 1];
                }
                --header->m_watchers_len;
                break;
            }
        }
    }
}

// cell-dispose: cell --
// Disconnect cell from the dependency graph, clear watchers, free value.
// Idempotent (no error on double-dispose).
// After disposal, cell-get, cell-set, cell-update, and watch raise invalid-access.
// throws: opstack-underflow, type-check
static void cell_dispose_op(Trix *trx) {
    trx->verify_operands(VerifyCell);

    auto cell_obj = *trx->m_op_ptr--;
    auto cell_offset = cell_obj.cell_offset();
    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);

    // Idempotent: already disposed
    if (header->m_base != CellDisposed) {
        // 1. Remove self from each dep's rdeps (forward deps -> deps' reverse deps)
        cell_clear_deps(trx, cell_offset);

        // 2. Remove self from each rdep's deps (reverse deps -> rdeps' forward deps)
        header = trx->offset_to_ptr<CellHeader>(cell_offset);
        if (header->m_rdeps != nulloffset) {
            auto rdeps = trx->offset_to_ptr<vm_offset_t>(header->m_rdeps);
            auto rdeps_len = header->m_rdeps_len;
            for (length_t i = 0; i < rdeps_len; ++i) {
                cell_remove_dep(trx, rdeps[i], cell_offset);
            }
            header->m_rdeps_len = 0;
        }

        // 3. Clear watchers
        header->m_watchers_len = 0;

        // 4. Free value's ExtValue
        header->m_value.maybe_free_extvalue(trx);
        header->m_value = Object::make_null();

        // 5. Mark as disposed
        header->m_base = CellDisposed;
    }
}

// cell-set-name: name cell --
// Sets a debug name on a cell for diagnostic display.
// throws: opstack-underflow, type-check
static void cell_set_name_op(Trix *trx) {
    trx->verify_operands(VerifyCell, VerifyName);

    auto cell_obj = *trx->m_op_ptr--;
    auto name_obj = *trx->m_op_ptr--;

    auto header = cell_obj.cell_header(trx);
    header->m_debug_name = name_obj.name_offset();
}

// cell-name: cell -- name true | false
// Returns the debug name of a cell, or false if unnamed.
// throws: opstack-underflow, type-check
static void cell_name_op(Trix *trx) {
    trx->verify_operands(VerifyCell);

    auto cell_obj = *trx->m_op_ptr;
    auto header = cell_obj.cell_header(trx);

    if (header->m_debug_name != nulloffset) {
        trx->require_op_capacity(1);

        auto name = trx->offset_to_ptr<Name>(header->m_debug_name);
        *trx->m_op_ptr = Object::make_name(header->m_debug_name, name->length());
        *++trx->m_op_ptr = Object::make_boolean(true);
    } else {
        *trx->m_op_ptr = Object::make_boolean(false);
    }
}

// cell-validated: value proc -- cell
// Creates a base cell with a validator proc.  The validator is called with the
// proposed value and must return a boolean.  If false, Error::Require is raised.
// The initial value is validated via trampoline before the cell is created.
//
// Op stack flow:
//   [... value proc] -> push value_copy -> [... value proc value_copy]
//   Validator consumes value_copy, pushes bool -> [... value proc bool]
//   @cell-validate-init pops bool; if true creates cell from value+proc.
//
// throws: opstack-underflow, type-check, require
static void cell_validated_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyAny);
    trx->require_op_capacity(1);
    trx->require_exec_capacity(2);

    auto proc_obj = trx->m_op_ptr[0];
    auto value_obj = trx->m_op_ptr[-1];

    // Push clone of value for validator to consume
    *++trx->m_op_ptr = value_obj.make_clone(trx);

    // Exec stack: [@cell-validate-init] [validator_proc]
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atCellValidateInit);
    *++trx->m_exec_ptr = proc_obj;
}

// @cell-validate-init: (control operator)
// Fires after cell-validated's validator proc returns.
// Op stack: [... value proc bool]
// If bool is true: pop bool+proc+value, create cell with value+validator.
// If bool is false: pop bool+proc+value, raise Precondition.
static void at_cell_validate_init_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean, VerifyCallable, VerifyAny);

    auto result_ptr = trx->m_op_ptr;
    if (result_ptr->boolean_value()) {
        // Create validated base cell
        auto value = trx->m_op_ptr[-2];
        auto proc = Object::make_null();
        auto validator = trx->m_op_ptr[-1];

        trx->m_op_ptr -= 2;
        *trx->m_op_ptr = make_cell(trx, value, proc, validator);
    } else {
        trx->error(Error::Require, "cell-validated: initial value rejected by validator");
    }
}

// @cell-validate-set: (control operator)
// Fires after cell-set's validator proc returns for a validated cell.
// Exec stack on entry: [... cell_offset_int | @cell-validate-set]
// Op stack on entry:   [... value cell bool]
// If bool is true: pop bool, call cell_set_core on [... value cell].
// If bool is false: pop bool+cell+value, raise Precondition.
static void at_cell_validate_set_op(Trix *trx) {
    trx->require_op_count(3);

    auto result_obj = trx->m_op_ptr[0];
    if (result_obj.is_boolean()) {
        if (result_obj.boolean_value()) {
            auto cell_offset = vm_offset_t{trx->m_exec_ptr->uinteger_value()};

            // Validation passed: pop bool, leave [... value cell] for cell_set_core
            --trx->m_exec_ptr;
            --trx->m_op_ptr;
            cell_set_core(trx, cell_offset);
        } else {
            trx->error(Error::Require, "cell-set: value rejected by validator");
        }
    } else {
        trx->error(Error::TypeCheck, "cell-set: validator must return a boolean");
    }
}

// cell-update: proc cell --
// Atomic read-modify-write for base cells.  Reads the cell's current value,
// applies proc (which transforms the value on the operand stack), then sets
// the result back.  Equivalent to `dup cell-get 3 -1 roll exec exch cell-set`
// but guaranteed atomic with respect to dependency tracking.
//
// Uses trampoline: pushes cell value on op stack, proc on exec stack, and
// @cell-update-done below.  When proc completes, @cell-update-done pushes
// the cell and invokes cell-set.
// throws: opstack-underflow, type-check, undefined-result
static void cell_update_op(Trix *trx) {
    trx->verify_operands(VerifyCell, VerifyCallable);

    auto cell_obj = trx->m_op_ptr[0];
    auto cell_offset = cell_obj.cell_offset();
    auto header = cell_obj.cell_header(trx);

    if (header->m_base == CellDisposed) {
        trx->error(Error::InvalidAccess, "cell-update: cell is disposed");
    } else if (header->m_base == CellComputed) {
        trx->error(Error::UndefinedResult, "cell-update: cannot update a computed cell");
    } else {
        trx->require_exec_capacity(3);
        --trx->m_op_ptr;

        // Trampoline: [cell_offset] [@cell-update-done] [proc]
        *++trx->m_exec_ptr = Object::make_uinteger(static_cast<uinteger_t>(cell_offset));
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atCellUpdateDone);
        *++trx->m_exec_ptr = trx->m_op_ptr[0];

        // Replace cell+proc on op stack with current cell value
        *trx->m_op_ptr = header->m_value.make_clone(trx);
    }
}

// cell-map: cell proc -- cell'
// Creates a computed cell derived from a single source cell.
// The proc transforms the source cell's value: when the source changes,
// the derived cell recomputes by reading the source and applying proc.
// Sugar for: { source-cell cell-get proc exec } cell-computed
// throws: opstack-underflow, type-check, vm-full
static void cell_map_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyCell);

    // Build computed proc: [source_cell cell-get mapping_proc exec]
    // source_cell is literal -- the interpreter pushes it to op stack.
    // mapping_proc is an executable proc/curry -- pushed as data (nested in body),
    // then exec runs it.
    // Region-aware so the proc shares the cell's region under ${...} (see cell-combine).
    auto [arr, arr_offset] = trx->vm_alloc_dispatch_n<Object>(4, Trix::ChunkKind::Array);
    arr[0] = trx->m_op_ptr[-1];  // literal cell
    arr[1] = Object::make_operator(SystemName::CellGet);
    arr[2] = trx->m_op_ptr[0];  // proc
    arr[3] = Object::make_operator(SystemName::Exec);

    auto value = Object::make_null();
    auto proc = Object::make_array(arr_offset, 4, Object::ExecutableAttrib, Object::ReadOnlyAccess);
    auto validator = Object::make_null();

    --trx->m_op_ptr;
    trx->gc_root_push_oneoff(proc);
    *trx->m_op_ptr = make_cell(trx, value, proc, validator);
    trx->gc_root_pop_oneoff();
}

// cell-transduce: cell xf -- cell'
// Creates a computed cell derived by applying transducer xf to the source cell's value.
// The source cell must hold an array (or packed array).  When the source changes, the
// derived cell recomputes by reading the source and applying the transducer.
// Sugar for: { source-cell cell-get xf into } cell-computed
// throws: opstack-underflow, type-check, vm-full
static void cell_transduce_op(Trix *trx) {
    trx->verify_operands(VerifyTransducer, VerifyCell);

    // Build computed proc: [source_cell cell-get xf into]
    // Region-aware so the proc shares the cell's region under ${...} (see cell-combine).
    auto [arr, arr_offset] = trx->vm_alloc_dispatch_n<Object>(4, Trix::ChunkKind::Array);
    arr[0] = trx->m_op_ptr[-1];  // literal cell
    arr[1] = Object::make_operator(SystemName::CellGet);
    arr[2] = trx->m_op_ptr[0];  // literal transducer
    arr[3] = Object::make_operator(SystemName::Into);

    // Create computed cell (same as cell_map_op / computed_op)
    auto proc = Object::make_array(arr_offset, 4, Object::ExecutableAttrib, Object::ReadOnlyAccess);
    auto value = Object::make_null();
    auto validator = Object::make_null();

    --trx->m_op_ptr;
    trx->gc_root_push_oneoff(proc);
    *trx->m_op_ptr = make_cell(trx, value, proc, validator);
    trx->gc_root_pop_oneoff();
}

// cell-combine: [cells] proc -- cell'
// Creates a computed cell derived from multiple source cells.
// The proc receives the values of all source cells on the operand stack
// (leftmost cell's value deepest) and produces a single result.
// Sugar for: { cell1 cell-get cell2 cell-get ... cellN cell-get proc exec } cell-computed
// throws: opstack-underflow, type-check, vm-full
static void cell_combine_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyArray);

    auto proc_obj = trx->m_op_ptr[0];
    auto arr_obj = trx->m_op_ptr[-1];
    auto [src, arr_len] = arr_obj.array_value(trx);

    if (arr_len == 0) {
        trx->error(Error::RangeCheck, "cell-combine: empty cell array");
    } else {
        // Validate all elements are cells
        for (length_t i = 0; i < arr_len; ++i) {
            if (!src[i].is_cell()) {
                trx->error(Error::TypeCheck, "cell-combine: element {} is not a cell", i);
            }
        }

        // Build computed proc: [cell1 cell-get cell2 cell-get ... cellN cell-get proc exec].
        // Compute the proc length wide and bound it BEFORE narrowing: 2*arr_len+2 can
        // exceed length_t for arr_len >= 32767, and a truncated length would under-allocate.
        auto proc_count = (static_cast<uinteger_t>(arr_len) * 2u) + 2u;
        if (proc_count > MaxArrayLength) {
            trx->error(Error::LimitCheck,
                       "cell-combine: source cell count {} too large; combined proc would exceed maximum {}",
                       arr_len,
                       MaxArrayLength);
        }
        auto proc_len = static_cast<length_t>(proc_count);
        // Region-aware: under ${...} the proc array must land in the global heap to
        // match the region-aware cell -- a global cell whose m_proc points into the
        // local frame dangles once that frame is reclaimed (the cell survives but
        // recomputes from freed memory).
        auto [arr2, arr2_offset] = trx->vm_alloc_dispatch_n<Object>(proc_len, Trix::ChunkKind::Array);

        for (length_t i = 0; i < arr_len; ++i) {
            arr2[static_cast<size_t>(i) * 2] = src[i];  // literal cell
            arr2[(static_cast<size_t>(i) * 2) + 1] = Object::make_operator(SystemName::CellGet);
        }
        arr2[proc_len - 2] = proc_obj;
        arr2[proc_len - 1] = Object::make_operator(SystemName::Exec);

        // Create computed cell
        auto value = Object::make_null();
        auto proc = Object::make_array(arr2_offset, proc_len, Object::ExecutableAttrib, Object::ReadOnlyAccess);
        auto validator = Object::make_null();

        --trx->m_op_ptr;
        // Root the proc across make_cell's global CellHeader alloc, which can fire
        // vm_global_gc and would otherwise sweep the not-yet-referenced global proc.
        trx->gc_root_push_oneoff(proc);
        *trx->m_op_ptr = make_cell(trx, value, proc, validator);
        trx->gc_root_pop_oneoff();
    }
}

// batch: proc --
// Defers watcher firing until the proc completes.
// throws: opstack-underflow, type-check
static void batch_op(Trix *trx) {
    trx->verify_operands(VerifyCallable);

    auto proc_obj = *trx->m_op_ptr--;

    // Allocate deferred watcher buffer for outermost batch.
    // Uses top-end temp region (grows downward from m_vm_limit) to avoid
    // fragmenting the main heap.  The saved temp pointer is pushed on the
    // exec stack so @batch-end / @batch-fail can free it.
    // Exec stack layout (outermost): [saved_temp_offset] [@batch-end] [proc]
    // Exec stack layout (nested):    [@batch-end] [proc]
    if (trx->m_batch_depth == 0) {
        trx->require_exec_capacity(3);

        auto saved_temp_offset = static_cast<vm_offset_t>(trx->m_vm_temp_ptr - trx->m_vm_base);
        auto buf = trx->vm_temp_alloc<BatchEntry>(MaxWatcherCollect);
        trx->m_deferred_watchers = static_cast<vm_offset_t>(reinterpret_cast<vm_t *>(buf) - trx->m_vm_base);
        trx->m_deferred_watcher_count = 0;
        *++trx->m_exec_ptr = Object::make_uinteger(saved_temp_offset);
    } else {
        trx->require_exec_capacity(2);
    }

    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atBatchEnd);
    *++trx->m_exec_ptr = proc_obj;

    ++trx->m_batch_depth;
}

//===--- Control Operators ---===//

// @cell-eval: (control operator -- success path)
// Fires when a computed cell's proc completes successfully.
// Caches the result, clears dirty flag, restores m_current_cell.
// Exec stack on entry: ... | prev_current_cell | cell_offset | @cell-eval
// Op stack on entry: ... | result
static void at_cell_eval_op(Trix *trx) {
    // Pop cell_offset, prev_current_cell, and dep_flag from exec stack
    auto cell_offset = vm_offset_t{trx->m_exec_ptr->uinteger_value()};
    --trx->m_exec_ptr;
    auto prev_cell = vm_offset_t{trx->m_exec_ptr->uinteger_value()};
    --trx->m_exec_ptr;
    auto dep_flag = trx->m_exec_ptr->integer_value();
    --trx->m_exec_ptr;

    // Read result from op stack
    trx->require_op_count(1);

    auto result = *trx->m_op_ptr;

    // Clone result for independent cell cache (op stack keeps original)
    auto curr_save_level = trx->m_curr_save_level;
    auto cached = result.make_clone(trx);
    cached.set_save_level(curr_save_level);

    auto header = trx->offset_to_ptr<CellHeader>(cell_offset);

    // Journal old cached value for save/restore
    if (header->m_value.save_level() != curr_save_level) {
        Save::save_object(trx, &header->m_value);
    } else {
        header->m_value.maybe_free_extvalue(trx);
    }
    header->m_value = cached;
    header->m_dirty = false;

    // Deps were recorded during evaluation by cell_record_dep/cell_add_dep (with
    // m_current_cell pointing at this cell) and are reset by cell_clear_deps at
    // the start of each re-evaluation; no separate tracking list is needed.
    trx->m_current_cell = prev_cell;

    // Record reverse dependency only for cell-get (dep_flag=1), not cell-value (dep_flag=0).
    // The cell that was being evaluated when cell-get was called depends on cell_offset.
    if (dep_flag && (prev_cell != nulloffset)) {
        cell_add_rdep(trx, cell_offset, prev_cell);
    }

    // Result stays on op stack
}

// @cell-eval-fail: (control operator -- failure path)
// Fires when a computed cell's proc throws an error.
// Restores m_current_cell, leaves cell dirty, re-raises the error.
static void at_cell_eval_fail_op(Trix *trx) {
    // Pop cell_offset, prev_current_cell, and dep_flag from exec stack
    --trx->m_exec_ptr;  // cell_offset (unused)
    auto prev_cell = vm_offset_t{trx->m_exec_ptr->uinteger_value()};
    --trx->m_exec_ptr;
    --trx->m_exec_ptr;  // dep_flag (unused)

    // Restore m_current_cell
    trx->m_current_cell = prev_cell;

    // Re-raise the original error
    trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
}

// @batch-end: (control operator -- success path)
// Fires when a batch proc completes successfully.
// Decrements batch depth; if zero, fires deferred watchers.
//
// Dirty computed cells need recomputation before their watchers can fire
// (otherwise the cached value is stale and the old==new check suppresses
// the watcher).  When dirty computed cells exist among the deferred list,
// @batch-end pushes a trampoline sequence:
//   [saved_temp] [@batch-fire] [pop] [cell-get] [cellN] ... [pop] [cell-get] [cell1]
// The cell-get ops force recomputation via the normal trampoline, and
// @batch-fire fires all watchers once every cell is up-to-date.
//
// When no dirty computed cells exist, watchers fire directly here (fast path).
static void at_batch_end_op(Trix *trx) {
    --trx->m_batch_depth;
    if ((trx->m_batch_depth == 0) && (trx->m_deferred_watchers != nulloffset)) {
        auto saved_temp_offset = trx->m_exec_ptr->uinteger_value();
        --trx->m_exec_ptr;

        auto count = trx->m_deferred_watcher_count;

        // Check for dirty computed cells among deferred watchers
        length_t dirty_count = 0;
        {
            auto entries = trx->offset_to_ptr<BatchEntry>(trx->m_deferred_watchers);
            for (length_t i = 0; i < count; ++i) {
                auto header = trx->offset_to_ptr<CellHeader>(entries[i].m_cell_offset);
                if (header->m_dirty && (header->m_base == CellComputed)) {
                    ++dirty_count;
                }
            }
        }

        if (dirty_count == 0) {
            // Fast path: no dirty computed cells -- fire watchers directly
            for (length_t i = 0; i < count; ++i) {
                auto entries = trx->offset_to_ptr<BatchEntry>(trx->m_deferred_watchers);
                auto cell_offset = entries[i].m_cell_offset;
                auto old_value = entries[i].m_old_value;
                auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
                auto new_value = header->m_value.make_clone(trx);

                if (!old_value.equal(trx, new_value)) {
                    cell_fire_watchers(trx, cell_offset, old_value, new_value);
                } else {
                    old_value.maybe_free_extvalue(trx);
                    new_value.maybe_free_extvalue(trx);
                }
            }
            trx->m_deferred_watcher_count = 0;
            trx->m_deferred_watchers = nulloffset;
            trx->vm_temp_restore(trx->m_vm_base + saved_temp_offset);
        } else {
            // Slow path: dirty computed cells need recomputation first.
            // Build exec stack: [@batch-fire] then [pop cell-get cell_obj]* for each dirty cell.
            // @batch-fire will fire all watchers after recomputations complete.
            // Exec stack layout (top to bottom):
            //   [cell1_obj] [CellGet] [Pop] ... [cellN_obj] [CellGet] [Pop] [@batch-fire] [saved_temp]
            trx->require_exec_capacity(static_cast<length_t>(2 + dirty_count * 3));

            *++trx->m_exec_ptr = Object::make_uinteger(saved_temp_offset);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atBatchFire);

            // Push recomputation sequence for dirty cells (reverse order so first executes first)
            auto entries = trx->offset_to_ptr<BatchEntry>(trx->m_deferred_watchers);
            for (length_t i = count; i-- > 0;) {
                auto header = trx->offset_to_ptr<CellHeader>(entries[i].m_cell_offset);
                if (header->m_dirty && (header->m_base == CellComputed)) {
                    *++trx->m_exec_ptr = Object::make_operator(SystemName::Pop);
                    *++trx->m_exec_ptr = Object::make_operator(SystemName::CellGet);
                    *++trx->m_exec_ptr = Object::make_cell(entries[i].m_cell_offset);
                }
            }
            // Deferred state stays alive -- @batch-fire will clean up
        }
    }
}

// @batch-fire: (control operator -- success path)
// Fires after dirty computed cells have been recomputed by the trampoline
// sequence pushed by @batch-end.  All cells are now up-to-date, so watchers
// can compare pre-batch old values against final values.
// Exec stack on entry: ... | saved_temp_offset | @batch-fire
static void at_batch_fire_op(Trix *trx) {
    auto saved_temp_offset = trx->m_exec_ptr->uinteger_value();
    --trx->m_exec_ptr;

    auto count = trx->m_deferred_watcher_count;
    for (length_t i = 0; i < count; ++i) {
        auto entries = trx->offset_to_ptr<BatchEntry>(trx->m_deferred_watchers);
        auto cell_offset = entries[i].m_cell_offset;
        auto old_value = entries[i].m_old_value;
        auto header = trx->offset_to_ptr<CellHeader>(cell_offset);
        auto new_value = header->m_value.make_clone(trx);

        if (!old_value.equal(trx, new_value)) {
            cell_fire_watchers(trx, cell_offset, old_value, new_value);
        } else {
            old_value.maybe_free_extvalue(trx);
            new_value.maybe_free_extvalue(trx);
        }
    }
    trx->m_deferred_watcher_count = 0;
    trx->m_deferred_watchers = nulloffset;
    trx->vm_temp_restore(trx->m_vm_base + saved_temp_offset);
}

// @batch-fire-fail: (control operator -- failure path)
// Fires when an error occurs during post-batch recomputation or watcher firing.
// Cleans up deferred watchers and temp region, re-raises the error.
static void at_batch_fire_fail_op(Trix *trx) {
    auto saved_temp_offset = trx->m_exec_ptr->uinteger_value();
    --trx->m_exec_ptr;

    auto entries = trx->offset_to_ptr<BatchEntry>(trx->m_deferred_watchers);
    auto count = trx->m_deferred_watcher_count;
    for (length_t i = 0; i < count; ++i) {
        entries[i].m_old_value.maybe_free_extvalue(trx);
    }
    trx->m_deferred_watcher_count = 0;
    trx->m_deferred_watchers = nulloffset;
    trx->vm_temp_restore(trx->m_vm_base + saved_temp_offset);

    trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
}

// @cell-update-done: (control operator -- success path)
// Fires after cell-update's proc completes.  The transformed value is on top
// of the operand stack.  Pushes the cell object and invokes cell-set.
// Exec stack on entry: ... | cell_offset_int | @cell-update-done
// Op stack on entry: ... | new_value
static void at_cell_update_done_op(Trix *trx) {
    // ensure proc left a result
    trx->require_op_count(1);
    trx->require_op_capacity(1);

    // Push cell object on op stack so cell-set sees: value cell
    auto cell_offset = vm_offset_t{trx->m_exec_ptr->uinteger_value()};
    *++trx->m_op_ptr = Object::make_cell(cell_offset);

    // Push cell-set on exec stack
    *trx->m_exec_ptr = Object::make_operator(SystemName::CellSet);
}

// @batch-fail: (control operator -- failure path)
// Fires when a batch proc throws an error.
// Decrements batch depth, discards pending watchers, re-raises.
static void at_batch_fail_op(Trix *trx) {
    --trx->m_batch_depth;
    if ((trx->m_batch_depth == 0) && (trx->m_deferred_watchers != nulloffset)) {
        // Pop saved_temp_offset FIRST
        auto saved_temp_offset = trx->m_exec_ptr->uinteger_value();
        --trx->m_exec_ptr;

        // Discard deferred watchers: free ExtValues in old_values
        auto entries = trx->offset_to_ptr<BatchEntry>(trx->m_deferred_watchers);
        auto count = trx->m_deferred_watcher_count;
        for (length_t i = 0; i < count; ++i) {
            entries[i].m_old_value.maybe_free_extvalue(trx);
        }
        trx->m_deferred_watcher_count = 0;
        trx->m_deferred_watchers = nulloffset;

        // Restore temp region (frees the BatchEntry array)
        trx->vm_temp_restore(trx->m_vm_base + saved_temp_offset);
    }

    // Re-raise the original error
    trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
}
