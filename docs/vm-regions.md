<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# VM Memory Regions: Local, Global, Temp

This is a **maintainer's** guide to the three VM memory regions and the
invariants that govern how operators allocate across them. The regions
themselves are documented per-subsystem in `gvm-heap-gc.md` (the global
allocator + GC) and `vm-internals.md` (the heap layout); the user-facing
local/global model is in `save-restore.md` and `local-global-vm.md`. This
document collects the *cross-region* invariants that, until now, lived only
in commit messages and audit notes (gap-M1) -- chiefly the **temp-clobber
invariant**, the **#46 region-aware-results decision table**, and the
**GC-rooting discipline** every operator author must follow when a result or
a borrowed value crosses a region boundary.

If you are adding or modifying an operator that builds a container, clones a
value, or holds a heap object across an allocation, read sections
[2](#2-the-temp-clobber-invariant), [3](#3-region-aware-results-the-four-patterns),
and [5](#5-gc-rooting-discipline-for-operator-authors) before you write code.
The function names cited here are the contract; line numbers drift.

---

## Table of Contents

1. [The Three-Region Map](#1-the-three-region-map)
2. [The Temp-Clobber Invariant](#2-the-temp-clobber-invariant)
3. [Region-Aware Results: The Four Patterns](#3-region-aware-results-the-four-patterns)
4. [Restore-Fragile Semantics](#4-restore-fragile-semantics)
5. [GC-Rooting Discipline for Operator Authors](#5-gc-rooting-discipline-for-operator-authors)
6. [The Lvar Exception](#6-the-lvar-exception)
7. [Testing the Invariants](#7-testing-the-invariants)

---

## 1. The Three-Region Map

The VM heap is a **single** `malloc`'d block, `m_vm_base` to `m_vm_limit`
(default 1 MB, 256 KB minimum, `--vm-size`). Three regions share those bytes; the
authoritative diagram is the comment at the top of `src/vm_heap.inl`
(`HEAP LAYOUT`). The maintainer-essential shape:

```
  +------------------------------------------------------------------+
  | m_vm_base                                            m_vm_limit  |
  |  +--------+-----+--------+-----+---------+-----+----------+       |
  |  | names  | ... | allocs | ... |  temp   | ... |  global  |       |
  |  | extval |     |        |     | scratch |     |  region  |       |
  |  | stacks |     |        |     |         |     |          |       |
  |  +--------+-----+--------+-----+---------+-----+----------+       |
  |  ^                  ^             ^             ^                 |
  |  m_vm_base     m_vm_ptr     m_vm_temp_ptr   m_vm_global          |
  |                (grows ->)    (<- grows)     (<- grows)           |
  +------------------------------------------------------------------+
                          ^                   ^
                          |                   |
                  free budget shared    global grows DOWN
                  by local + global     through where temp lives
```

The pointer invariant is
`m_vm_base <= m_vm_ptr <= m_vm_temp_ptr <= m_vm_global <= m_vm_limit`
(`src/vm_heap.inl`, "Invariant:" comment). The three regions:

- **Local** (main heap, `m_vm_base .. m_vm_ptr`): the bump-allocated arena.
  `vm_alloc` / `vm_alloc_n` grow it **up** via `m_vm_ptr`. There is no
  `free` -- the only reclamation is `restore`, which rolls `m_vm_ptr` back to
  a save barrier in O(1) and replays the journal. Ordinary allocations land
  here. See `save-restore.md`.
- **Global** (`m_vm_global .. m_vm_limit`): journal-skipped, GC-managed
  storage that survives `restore`. `gvm_alloc` grows it **down** by lowering
  `m_vm_global` toward the local heap. Reclaimed by the mark-sweep collector,
  not by `restore`. This is where `${...}` blocks, `make-global-dict` (the `${ N dict }` idiom), and
  every other region-aware result allocate. See `gvm-heap-gc.md`.
- **Temp** (`m_vm_temp_ptr .. m_vm_global`): operator-scoped scratch. It sits
  **directly below the global region** and grows **down** via `m_vm_temp_ptr`,
  anchored at `m_vm_global`. `vm_temp_alloc_ptr` allocates it; `vm_temp_restore`
  reclaims it after the operator finishes. Used for membership dicts and
  expansion buffers in set-algebra, sort-by/group-by/partition, and the
  packed-expansion ops.

Free space is the gap `m_vm_ptr .. m_vm_temp_ptr`. Both the local bump and the
global/temp downward bumps draw from it; whichever side cannot fit a request
raises `Error::VMFull`.

The position-independence of the whole scheme rests on **offset-based
references**: every internal pointer is a `vm_offset_t` (a `uint32_t` byte
offset from `m_vm_base`), so the heap can be snapshotted and thawed at a
different base address. `is_global(offset)` is a pure address-range test --
"the offset's absolute address is at or above `m_vm_global`" -- with no header
bit consumed (`src/vm_heap.inl`, `is_global`).

---

## 2. The Temp-Clobber Invariant

> **The headline invariant.** `gvm_alloc` bounds-checks a new global block
> **only against the local-heap top** (`m_vm_ptr`), never against the temp
> region. So **any global allocation while a temp buffer or dict is live
> CLOBBERS it.**

The low-level bump is `vm_alloc_global_ptr<T>` in `src/vm_heap.inl`. Its
entire bounds check is:

```cpp
auto new_global = /* m_vm_global lowered by size, aligned */;
if (new_global < m_vm_ptr) {
    error(Error::VMFull, "global region exhausted ...");
} else {
    m_vm_global = new_global;
    ...
    if (m_vm_temp_ptr > new_global) {
        m_vm_temp_ptr = new_global;   // <-- the clobber clamp
    }
}
```

The global region grows down by lowering `m_vm_global`. The temp watermark
`m_vm_temp_ptr` is **clamped** to follow it. In the steady state -- no
operator mid-temp-use -- this clamp is benign and load-bearing (it lets the
scanner interleave `${...}` global allocations with the post-restore
baseline). But when temp data is **live** at `[m_vm_temp_ptr, m_vm_global)`
and a new global block lands **below** the old temp top, the new global block
`[new_global, old_global)` **overlaps the live temp slice**. The bump never
sees it: it only checked `new_global < m_vm_ptr`. The comment in
`vm_alloc_global_ptr` documents exactly this (`src/vm_heap.inl`, the
`m_vm_temp_ptr serves a dual role` block).

This is **production-facing**, not just a stress artifact: a plain
`${ ... }` program building a `zip` or `sort-by` result over `Long`/`Double`
elements would corrupt its own scratch, with no debug op enabled.

### The two defenses

An operator that keeps temp scratch live across an allocating step must use
**one** of these. Both reduce to the same principle -- *no global allocation
while temp scratch is live* -- so the global region never moves and the clamp
never fires.

1. **Everything LOCAL while temp scratch is live** -- `Object::make_clone_local`.
   `make_clone` consults `m_curr_alloc_global` and, under `${...}`, allocates
   the clone *globally* (`ExtValue::alloc` -> `gvm_alloc`). `make_clone_local`
   (`src/object.inl`) is the mirror that forces `ExtValue::alloc_local` /
   `WideValue::alloc_local` regardless of the flag. Use it for scratch keys
   and result elements while a membership dict is open. `expand_packed_to_temp`
   takes the same tack at the source: it **suppresses** `m_curr_alloc_global`
   across its decode+clone so the decoded elements land local, not in a global
   block that would clobber the temp buffer it is filling.

2. **Suppress the global flag for the WHOLE op** -- the saved-flag-on-the-frame
   pattern. For two-phase collect ops (sort-by, group-by, partition) the
   key/element scratch persists across *every* user-proc invocation, and the
   proc itself runs arbitrary allocating user code. There is no narrow window
   to wrap. Instead the op saves `m_curr_alloc_global`, sets it false for the
   whole operator, and rides the saved flag on the **bottom of the exec frame**
   (so existing index-relative offsets stay put), restoring it at finalization
   on every exit path. Error unwinds are covered by the enclosing `${}`
   barrier.

**Rooting is not sufficient here.** A GC root guards against *sweep*; it does
nothing against a temp *clobber*, which is a raw memory overwrite by the next
global bump. The temp-clobber defenses are about *where* you allocate, not
*whether the GC can see it*.

A consequence: ops that take either defense produce **LOCAL** results under
`${...}` during the build (the tracked region-inconsistency, same class as
`map-dict`). To still deliver a survives-restore result they promote at the
end -- see pattern (b) in the next section.

---

## 3. Region-Aware Results: The Four Patterns

The #46 pass made every container result region-aware: a container built
inside `${...}` lands in global VM so it can be stored into a global container
and survive `restore`. There are **four** patterns; pick by whether the op
holds live temp scratch and whether the result is built directly or in phases.

| # | Pattern | When to use | Helper / mechanism | Example op |
| --- | --- | --- | --- | --- |
| (a) | **Build direct into target region** | No temp scratch; result built directly over op-stack-rooted sources | `create_result_set` / `create_result_dict` (dispatch `create_global_*` vs `create_*` on `m_curr_alloc_global`); root across the fill | `set-union` (set-algebra) |
| (b) | **Promote at end** | Op holds a live temp dict/buffer during the build (so it must build LOCAL per §2) | `deep_promote_to_global` after `vm_temp_restore`, via the `promote_result_if_global` guard | `sort-by`, `group-by`, `zip`, `frequencies` |
| (c) | **`make_clone_local` during temp-live windows** | Cloning elements while a membership dict / expansion buffer is open | `Object::make_clone_local` (forces local alloc) | the packed-expansion ops (zip/interpose/chunk/...) |
| (d) | **Suppress `m_curr_alloc_global` for the whole op** | Two-phase collect: scratch live across every user-proc call | save flag on the exec frame, restore at finalization | `sort-by` / `group-by` / `partition` (the two-phase collect) |

Notes on each:

- **(a) build-direct.** When the result is built directly via `set_put` /
  `put` over sources that are already rooted on the op stack (no temp dict),
  route the *constructor* through the region dispatcher and root the result
  across the fill. `create_result_set` and `create_result_dict`
  (`src/ops_array_iteration.inl`) are exactly
  `m_curr_alloc_global ? create_global_* : create_*`. The set-algebra ops
  (`set-union` / `set-intersection` / `set-difference` /
  `symmetric-difference`) use this: they probe membership read-only and
  `set_put` cloned keys into a pre-sized result -- no scratch dict -- so they
  build direct, rooted, no promote.

- **(b) promote-at-end.** When the op *must* build local (it holds a live temp
  -- §2), it builds the result LOCAL and then, **after** `vm_temp_restore` (and
  after `m_curr_alloc_global` is restored for the flag-suppressing ops), calls
  `promote_result_if_global` on the result. That guard
  (`src/object.inl`) is `m_curr_alloc_global ? deep_promote_to_global : result`.
  `deep_promote_to_global` recursively rebuilds every array/set/dict level in
  the global region -- a *shallow* `make_clone` would leave nested sub-arrays
  (zip pairs, chunks) or dict-array values dangling local. Each level is fully
  null-init'd before any further alloc and rooted across its fill (the GC rules
  in §5). It is **only** safe after `vm_temp_restore`: the local source then
  lives in the local heap (never the temp region), so it survives the global
  clones' GC.

- **(c) `make_clone_local`.** The narrow-window twin of (d). When you clone
  individual elements while a temp buffer/dict is still open, clone them
  **local** so no global bump clobbers the temp. The packed-expansion ops use
  this for their result elements.

- **(d) suppress-for-whole-op.** The blunt instrument for when there is no
  narrow window because user procs allocate throughout the build (see §2,
  defense 2).

**Deferred-as-safe:** `supervisor-which-children` is region-inconsistent but
left as-is -- its result is ephemeral runtime handles (coroutine offsets /
interned Names), so restore-survival is not meaningful, and it is verified
crash-free under stress. The `gen_server` startup proc and
`protocol_make_dispatch_proc` are internal (consumed immediately by dispatch
machinery), shallow clones, also left.

### Worked example: a region-aware result survives restore

```trix
/d ${ << >> } def
save /sv exch def
% concat (string result) and set-union (set-algebra) both build GLOBAL under ${}
d ${ /s } ${ (foo) (bar) concat } put
d ${ /u } ${ [ 1l 2l 3l ] set-from-array
            [ 3l 4l 5l ] set-from-array set-union } put
sv restore
(concat result survived restore) d ${ /s } get (foobar) eq assert
(union result survived restore)  d ${ /u } get length 5 eq assert
(union has element 4l)           d ${ /u } get 4l set-member? assert
```

Without #46 the `put` would be R6-rejected (the result would be a
restore-fragile local stored into a global dict); with it, the global result
survives the `restore`.

---

## 4. Restore-Fragile Semantics

`restore` rolls the local heap back to the save barrier and replays the
journal; global mutations persist and are journal-skipped. That asymmetry is
the whole reason for the region rules: a global container must never hold a
reference to local storage that a future `restore` will reclaim.

### The save-barrier offset test

`Save::is_restore_fragile_local` (`src/save.inl`) decides whether a value
would dangle after `restore`. A value is **restore-fragile** iff it is a
heap/VM value allocated **above the first save barrier** and not already
global:

```cpp
if (m_curr_save_level == BASE)            return false;  // no save active
if (!uses_vm() && !uses_heap_cell())      return false;  // pure value, no offset
if (is_global(offset))                    return false;  // already global
return offset >= m_save_stack[BASE + 1];                 // above the BASE barrier
```

BASE-local allocations (offset below `m_save_stack[BASE + 1]`) are
restore-**immune**: `restore` rolls back to a barrier but never below BASE.

### Two outcomes: clone the value, reject the container

The split is enforced by **`make_clone`**, which **deep-copies** ExtValue
(Long/ULong/Double/Address) and WideValue (Int128/UInt128) but is **shallow**
for Name/String/composites (it shares the offset). So:

- **Values move regions.** `Save::clone_fragile_scalar_into_global`
  (`src/save.inl`) deep-clones a fragile scalar into the global region (sets
  `m_curr_alloc_global`, `make_clone`, frees the original local ExtValue) so a
  global container can hold it across `restore`. This is why storing a number
  into a global container "just works" at any save level.

- **Containers cannot relocate, so they are rejected.**
  `Save::reject_local_into_global` (`src/save.inl`) raises
  `Error::InvalidAccess` ("cannot store restore-fragile local-VM &lt;type&gt;
  into global container") for a fragile **non-scalar** -- a shallow `make_clone`
  would keep the same local offset, which would dangle. The fix is to build the
  container in `${...}` (global) or below the barrier.

```trix
/d ${ << >> } def
save /sv exch def
/a [ 1 2 3 ] def                  % local array, above the barrier
{ d ${ /k } a put } try           % store local array into global dict
(local container into global is rejected) exch /invalid-access eq assert
sv restore
```

```trix
/d ${ << >> } def
save /sv exch def
% a VALUE (Long) clones freely into the global dict
{ d ${ /k } 42l put } try
(value into global dict ok) exch /no-error eq assert
(value stored) d ${ /k } get 42l eq assert
sv restore
(value survived restore) d ${ /k } get 42l eq assert
```

Note that the **key** must survive too: `/k` is a scan-time local Name, so it
is itself restore-fragile -- hence `${ /k }` to intern it global. Values clone
freely; *containers and names* must be built in the region they are stored
into. This is the user-facing VALUE-vs-CONTAINER rule; see `local-global-vm.md`
and `save-restore.md`.

**Stricter siblings.** The `-persist` family (`put-persist`,
`set-add-persist`, `array-store-persist`, ...) rejects **any** above-barrier
operand -- even a number -- with `/above-barrier`, because it creates permanent
entries whose header mutations would dangle; it needs below-barrier or global
operands. Reactive cells take the *opposite* tack from `reject_local_into_global`:
`cell_set_core` **clones** a fragile value into global rather than rejecting,
since a long-lived cell should accept any value. `eqref` containers (root
EqArray/eqdict/eqset) reject above-barrier values because they bypass
journaling.

```trix
/d ${ << >> } def
save /sv exch def
% -persist rejects even a plain Long created above the barrier
{ d ${ /k } 42l put-persist } try
(persist rejects above-barrier) exch /above-barrier eq assert
sv restore
```

---

## 5. GC-Rooting Discipline for Operator Authors

The global region is mark-swept. Any global value an operator holds **off a
GC-scanned stack** -- in a bare C local, or in an under-filled / uninitialized
slot of a block already stamped with its element count -- is a sweep target the
moment the next global allocation fires the collector. The rooting helpers live
in `src/vm_heap.inl`.

### `gc_root_push_oneoff` vs `require_gc_root_capacity_more` + `gc_root_pop_n`

- **`gc_root_push_oneoff(obj)` / `gc_root_pop_oneoff()`** -- the common
  "keep one value alive across one allocation, then drop it" case.
  `gc_root_push_oneoff` **asserts the gc-root stack is empty first**
  (`require_gc_root_capacity`), so it **cannot nest**. Use it only at the top of
  an operator that does not already hold roots. For nested arrays (e.g. a
  regex-find-all groups array), do **not** try to push two oneoffs -- instead
  **link the sub-array into the already-rooted outer result before filling it**,
  so the outer root covers it.

- **`require_gc_root_capacity_more(n)` + raw push + `gc_root_pop_n(n)`** -- for a
  helper that runs **nested under an operator already holding roots** (a lazy-seq
  builder forcing a thunk, an exec mid-build). `require_gc_root_capacity_more`
  reserves capacity *without* the empty-stack assert; you push onto
  `m_gc_roots_ptr` directly; `gc_root_pop_n` decrements by exactly `n` **without**
  resetting the stack to empty (which `reset_gc_root` would do, clobbering the
  caller's roots). `deep_promote_to_global` (§3) is the canonical user.

### The borrow-vs-owned iterator rule

Load-bearing, and the source of several latent UAFs:

- **`Dict::next` / `set_next` return BORROWED keys/values** -- `entry->m_key` /
  `entry->m_value` **as-is**, an alias the container *owns*. An iterator callback
  must **never** `maybe_free_extvalue` a borrowed key/value, or it corrupts the
  source container's ExtValue/WideValue mid-iteration. (`@set-filter` once freed a
  borrowed source key -- the lone violator across the iterator family; `@dict-filter`
  already carried a comment warning of exactly this.)

- **`arrays_pop_clone_head` returns an OWNED clone** -- the array iteration
  callbacks (partition/take-while/scan) *must* free it to avoid a leak.

When in doubt: did the call *clone*? Then you own it and must free it. Did it hand
back an `m_key`/`m_value` field? Then you borrow it and must not.

### Clone while on the op stack, never to a bare C local

The recurring bug shape: an op pops a heap value to a bare C local, then
`make_clone`s -- the clone fires GC, which sweeps the now-unrooted source block,
leaving the C local dangling. **Clone the value while it is still on the op stack
(rooted), then pop.** When two clones are live at once (a dict's key + value into a
not-yet-chained entry), root the first across the second's allocation
(`Dict::put` does this). The same rule bit the execution paths of `curry` and
`thunk` (`execute_curry`, `execute_thunk`, `at_force_complete_op`): they popped to
a C local then cloned -- fixed by rooting the block across the clone via
`require_gc_root_capacity_more` / `gc_root_pop_n` (they run nested under the lazy
builders, so not oneoff).

### Two more rules for global blocks

- **A GLOBAL array/dict must be FULLY null-init'd BEFORE any subsequent alloc.**
  A local unrooted array is not walked, so partial init was tolerable; a global
  block is walked by `gvm_for_each` *by its stamped `obj_count`* on the very next
  GC -- an uninitialized slot is a garbage offset -> walk-crash. Null all slots and
  root, *then* clone into them.
- **Proc-invoking ops have a setup window.** map/filter/take-while pop the first
  element (a GC-firing clone) *before* parking the result on the exec stack, so a
  global result is briefly unreachable. Root it (`gc_root_push_oneoff`) across the
  pop, then hand off to the exec-stack park.

`error()` resets the gc-root stack at the earliest throw point
(`src/ops_system.inl`), so a root held across a fill loop with mid-loop error
paths needs no manual pop on the error path.

For the broader ExtValue-ownership and GC-root contracts see `dev-invariants.md`.

---

## 6. The Lvar Exception

There is exactly **one** place a global slot's mutation is *deliberately
journaled*: a logic-variable binding.

A logic variable (`lvar`) created inside `${...}` has a **global** inner
array, so it survives `restore` (intended -- the lvar persists). But a binding
made during backtracking (`find-all` / choice / `aggregate`) **must** be undone
by the per-alternative `restore`, or backtracking drops every alternative but
the first. Global mutations are normally journal-skipped, which would make that
impossible.

`Save::Flavor::LvarBinding` (`src/save.inl`) resolves it. `save_data`
short-circuits journaling for global offsets **except** when the flavor is
`LvarBinding` and a save is active -- then the binding write **is** journaled,
on a global slot, and replay reverts it exactly like a local Object. `lvar_bind`
(`src/ops_logic.inl`) routes a global binding slot through this flavor and the
ordinary path for a local one, keeping the two disjoint. To keep the slot valid
across any GC between the bind and the restore, `gc_walk_chain` **pins** the
owning block (`mark_global_offset` on the entry's global `m_ptr`) so it cannot
be freed or realloced out from under the journal entry before replay.

This is the single source of truth: the `LvarBinding` flavor is the only flavor
whose journaled slot is global; a plain `Object` entry never targets global
(`save_object` short-circuits), so the two never collide. See `logic.md` for the
logic layer and `save-restore.md` for the journal.

---

## 7. Testing the Invariants

The region invariants are GC-timing-dependent and **mask under immediate
payloads**: when a container holds Name/Integer/Byte elements, `make_clone` and
`maybe_free_extvalue` are no-ops, so a clone-across-GC bug never surfaces. The
methodology that found six latent UAFs in one session:

1. Run the op inside `${...}` under **`true vm-gc-stress`** (fire a GC before
   *every* global allocation) and **`true vm-gc-poison`** (scribble freed
   payloads with a poison byte). Both are `TRIX_DEBUGGER`-only; see
   trix-reference §3.43, [Family C](trix-reference.md#343-interactive-debugger-and-vm-introspection).
2. Use **heap-backed payloads** -- `ExtValue` (`Long` `Nl`, `ULong` `Nul`,
   `Address` `Na`, `Double` `Nd`) and `WideValue` (`Int128` `Nq`, `UInt128`
   `Nuq`) -- so `make_clone`/`maybe_free` actually allocate and free.
3. Assert with **exact equality** (exact sums / element-0 canaries), not
   inequalities: poison bytes form huge values that pass `ge`/`le`. Add a
   **source-intact** check (an exact sum over the *input* after the op) to catch
   the borrow-vs-owned class (a freed borrowed key).

A minimal smoke version:

<!-- doctest: skip (requires a TRIX_DEBUGGER build: vm-gc-stress is debug-only) -->
```trix
true vm-gc-stress
/d ${ << >> } def
% build a global array of Long (ExtValue) and Int128 (WideValue) under stress
d ${ /a } ${ [ 5000000001l 5000000002l 5000000003l ] } put
d ${ /b } ${ [ 100000000000000000001q 100000000000000000002q ] } put
false vm-gc-stress
(longs intact)  d ${ /a } get 0 get 5000000001l eq assert
(int128 intact) d ${ /b } get 1 get 100000000000000000002q eq assert
```

The standing regression cadre is
`tests/test_container_extvalue.trx` (functional + `${}` save/restore region
survival across array/packed/dict/set/record/tagged/curry/thunk/closure/lazy/
cell/string x Long+Int128, plus ULong/Double/Address/UInt128 spot checks) and
`tests/test_gc_stress_container_extvalue.trx` (the same matrix under
`vm-gc-stress` + `vm-gc-poison` with exact-sum / canary / source-intact
asserts). The stress file is opt-skipped (the optimized build lacks
`vm-gc-stress`). Build the **debug** `./trix` after any non-vacuity revert
before re-running the GC-stress suite -- the matrix's opt step does not rebuild
the debug binary.

For literal syntax and the per-op semantics see `trix-reference.md`; for the GC
itself see `gvm-heap-gc.md`; for the heap layout see `vm-internals.md`; for the
operator-author allocation contracts see `dev-invariants.md`.
