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

# Trix Invariants and Contracts

Critical rules that must be maintained across all code changes.  Violating
these invariants causes memory corruption, resource leaks, or undefined
behavior.  Each rule includes the rationale and where to look for examples.

## ExtValue / WideValue Lifecycle

ExtValues are 8-byte slots on the VM heap for 64-bit values (Long, ULong,
Double, Address).  **WideValue is the 16-byte variant of the same
mechanism, backing Int128 and UInt128** -- a parallel heap cell with its
own per-save-level free list, its own active counter, and the same
sentinel-based validity checking.  Every ownership rule below applies to
both kinds identically; `Object::uses_heap_cell()` /
`heap_cell_valid()` are the unified predicates when a call site should
handle either.  Forgetting the WideValue branch in code that handles
ExtValues is a live source of regressions (object.inl says so in the
helper's own comment) -- if you touch one, ask where the other is.

**Rule: Every ExtValue/WideValue must have exactly one owner.**
- Creating an Object via `make_long()`, `make_ulong()`, `make_double()`,
  `make_address()` allocates a new ExtValue; `make_int128()` /
  `make_uint128()` allocate a new WideValue.  The returned Object owns it.
- Copying an Object (bitwise copy, assignment) copies the offset, creating
  a shared reference.  This is ONLY safe for temporary reads — the copy
  must not outlive the original unless one of them is freed.
- `make_clone(trx)` creates an independent copy (allocates a new cell of
  the right kind).  Use this when copying between containers or across
  ownership boundaries.
- `maybe_free_extvalue(trx)` returns the cell to its free list — despite
  the name it handles BOTH kinds (the WideValue branch is built in).
  Call this when discarding an Object that may hold either.
- `make_copy(save_level)` is a bitwise copy that transfers ownership —
  the source must not be freed afterward.

**Consequences of violation:**
- Leak: cell not freed -> `m_extvalue_active_count` /
  `m_widevalue_active_count` drifts positive.
- Double-free: cell freed twice -> free list corruption -> silent
  memory aliasing -> hard-to-debug data corruption.

**Where to check:** `m_extvalue_active_count` and
`m_widevalue_active_count` carry a fixed nonzero baseline (currently 28
ExtValue / 4 WideValue) from the systemdict `numbers` constants
(`init_numbers` / `init_numbers_float_type` in `dict.inl` populate
Double/Long/ULong/Int128/UInt128 min/max/lowest/epsilon/infinity/NaN/pi/e
constants via `make_numeric_value`).  These are permanent LOCAL heap cells
living in a ReadOnly dict for the whole VM lifetime, not a leak.  A leak is
therefore a count that DRIFTS above this idle baseline (or fails to return
to it after an operator), not a nonzero count per se -- a leak check should
snapshot the baseline at startup and compare deltas.  Both counts are
exposed to gray-box tests as `:status:extvalue-active` /
`:status:widevalue-active` (with `extvalue-free` / `widevalue-free` and
`-validate` companions); `:status:extvalue-validate` /
`:status:widevalue-validate` are the free-list-corruption checks.
`ExtValue::valid()` / `WideValue::valid()` check the dead sentinel for
freed-value access.

**Rule: heap-cell scalars have VALUE semantics across save/restore.**
From the program's point of view a Long/ULong/Double/Address/Int128/
UInt128 is an immediate value like Integer -- the heap cell is an
implementation detail.  The engine upholds this in `Save::restore`
(step 12): above-barrier cells still owned by live stack slots are
**relocated** below the barrier via `ExtValue::restore` /
`WideValue::restore` (ascending offset order, so a relocation's freed
slot can be reused by a later one; completeness is post-asserted),
while local composites in the same position raise `/invalid-restore`.
Two consequences for engine code:
- A cell's `m_offset` is NOT stable across restore -- never cache one
  across a possible restore boundary.
- Stacks may legitimately hold these types above the barrier; code
  that walks stacks for barrier violations must exempt them (see
  `Save::check_stack`'s RelocateCounts).

**Common patterns:**
- Stack pop that discards: `obj.maybe_free_extvalue(trx); --m_op_ptr;`
- Copy into container: `dst[i] = src[i].make_clone(trx);`
- Replace in-place: `old.maybe_free_extvalue(trx); *slot = new_value;`

**Rule: borrowed iterator results must not be freed; owned ones must.**
- `Dict::next` / `Dict::set_next` return the entry's `m_key` / `m_value`
  **as-is** -- a BORROWED alias the container still owns.  An iterator callback
  must NEVER `maybe_free_extvalue` a borrowed key/value, or it corrupts the
  source container's ExtValue/WideValue mid-iteration.
- `Object::arrays_pop_clone_head` returns an OWNED clone -- the array-iteration
  callbacks (partition / take-while / scan) MUST free it to avoid a leak.
- The test: did the call *clone*?  Then you own it and must free it.  Did it
  hand back an `m_key` / `m_value` field?  Then you borrow it and must not.
  (Borrowed references from `extract_next_packed` follow the same rule -- see
  `vm-internals.md` "The Borrowed Reference Problem".)  The full region-aware
  contract -- borrow-vs-owned, the temp-clobber invariant, and the four
  region-aware-result patterns -- lives in `vm-regions.md`.

## GC-Root Stack: Rooting Temporaries Across Allocations

`vm_global_gc` is a stop-the-world, precise, **non-compacting** mark-sweep
(see `gc.inl`).  It can fire on any global VM allocation — `make_long`,
`make_clone` of an ExtValue, `make_curry_pair`, `make_lazy`, every
`gvm_alloc`.  A live object that is not reachable from a GC root when the
sweep runs is freed out from under the code still building with it.

**Rule: while an operator builds a global composite across more than one
allocation, every in-flight Object it still needs must be reachable from a
GC root before the next allocation fires.**

The operand, execution, dict, and error stacks are GC roots; so is the
dedicated **gc-root stack** (`m_gc_roots_base/ptr/limit`, carved in
`init.inl`, walked by `walk_all_roots` in `gc.inl`).  The gc-root stack is
the sanctioned place to park operator-scoped temporaries that must survive
an allocation but do not belong on the operand stack.

**The contract** (enforced by `require_gc_root_capacity` / `reset_gc_root`
in `vm_heap.inl`):
- The stack holds **operator-scoped** Objects only.  It must be **empty at
  every operator boundary** — never spanning a `restore`, a coroutine
  switch, or a snapshot point.  That invariant is what lets a later restore
  and its allocations run without walking a dangling root.
- Guard capacity ONCE with `require_gc_root_capacity(n)`, then raw-push
  `*++m_gc_roots_ptr = obj`.  Capture `auto *r = m_gc_roots_ptr` for a
  rolling accumulator — loops re-stamp one slot, not one root per iteration.
- Clear at the operator's tail with `reset_gc_root(n)` on **every path that
  pushed**; paths that never pushed leave it alone.  The dispatch loop does
  NOT reset per iteration — only operators that use the stack pay to clear
  it.
- The throw path is covered centrally: `error()` resets the stack before it
  runs `try_catch_handler` / `global_handler` (which may restore the VM and
  allocate), so an unwound operator's roots never survive.

**Initial vs. incremental guard:**
- `require_gc_root_capacity(n)` is the INITIAL guard.  It debug-asserts the
  stack is empty before reserving — catching the one leak `reset_gc_root`'s
  exact-count check cannot: an operator that pushed roots but never reset,
  whose stale roots would otherwise be inherited silently by the next
  operator to call the initial guard.
- `require_gc_root_capacity_more(n)` is the PHASED guard: reserve more roots
  mid-rooting (a common N up front, then M more inside a branch) WITHOUT the
  empty-stack assert, since the stack legitimately holds this operator's
  earlier roots.  `reset_gc_root(total)` still validates the combined count
  at the tail.

**The clone-into-slot rule.** `make_curry` / `make_thunk` allocate first and
store their Object arguments only after.  A freshly-made object passed inline
as an argument — `make_curry_pair(trx, foo.make_clone(trx), X)` — is UNROOTED
during make_curry's own allocation, so an ExtValue clone there can be swept.
Land the clone in its gc-root slot in the same statement that creates it,
then re-stamp the slot:
```cpp
*++trx->m_gc_roots_ptr = foo.make_clone(trx);  auto *c = trx->m_gc_roots_ptr;
*c = Object::make_curry_pair(trx, *c, X);       // *c stays rooted during the alloc
```
An object already living in an op/exec slot is already rooted there — do not
duplicate it onto the gc-root stack.

**What stays off the gc-root stack.** The stack is bounded
(`MaxGcRootDepth = 80`); overflow is a catchable `/limit-check`, never a
corrupting write.  Bounded chain-builders use it; **unbounded-recursive**
rooting (copy-term, deep structural walks) can exceed 80 on deep user data
and STAYS on the operand stack.  The stack roots Objects only — it cannot
root a non-Object VM block (e.g. a BindingBucket); publish those into a slot
that is already a root before the next allocation.

**Consequences of violation:** a mid-build GC sweeps the half-built
composite or a dropped temporary, producing a use-after-free that surfaces
only under GC pressure.  The debug `vm-gc-stress` hook (`TRIX_DEBUGGER`,
fires `vm_global_gc` before every global alloc) and the
`tests/test_gc_stress_*.trx` oracles exist to flush these out.

**Where to check:** `walk_all_roots` (`gc.inl`) walks `[m_gc_roots_base,
m_gc_roots_ptr]`.  The empty-at-boundary invariant is asserted at both ends
— `require_gc_root_capacity` (operator entry) and `reset_gc_root` (tail).

## Save/Restore Journaling Contract

The save/restore system captures old values before mutation so `restore()`
can undo changes.  See `save.inl` for the journal implementation.

**Rule: All mutations to heap-resident data must be journaled.**

Specifically:
- Array element writes: call `Save::save_object(trx, &element)` before
  modifying an array element that may predate the current save level.
- Dict entry value updates: call `Save::save_dict_entry(trx, entry)`
  before overwriting an entry's key or value.
- Dict bucket chain changes: call `Save::save_dict_header(trx, dict)`
  before modifying bucket heads or pool/length/maxlength.
- Dict predecessor link changes: call `Save::save_dict_entry_next()`
  before unlinking an entry from a bucket chain.
- Set entry mutations: same pattern with `Save::save_set_entry()`.
- Packed name binding: call `Save::save_packed_name()` before modifying
  inline name bytes in a packed array.

**Exception: String byte writes via `put` are NOT journaled.**
This is by design — string mutations persist across restore.  This is a
documented language behavior, not a bug.

**Exception: Heap allocations are NOT journaled.**
They are reclaimed in bulk by resetting m_vm_ptr to its saved position.

**Exception: global mutations are journal-skipped -- except one.**
A write to a global offset normally short-circuits journaling (it survives
`restore` by design).  The lone exception is `Save::Flavor::LvarBinding`: a
logic-variable binding made during backtracking IS journaled even on a global
slot, so per-alternative `restore` can undo it.  See `vm-regions.md` "The Lvar
Exception" and `logic.md`.

**How to check:** Compare `obj.save_level()` with `trx->m_curr_save_level`.
If they match, the object was created/modified at the current level and
its current state is already the "saved" state — no journal entry needed.
If they differ, journal before mutating.

## DictEntry Never Relocates

**Rule: DictEntry and SetEntry structs are never moved after allocation.**

The Name binding cache (`Name::m_binding`) stores a raw vm_offset_t
pointing directly into `DictEntry::m_value`.  If entries were relocated
(e.g., by a rehash), every cached binding across every Name in the VM
would silently point to stale data.

This is why Dict uses pool-based allocation (entries taken from a
pre-allocated pool) and why `expand_dict()` appends new pool blocks rather
than rehashing.  Bucket count is fixed at creation time.

## VM Heap Never Relocates

**Rule: The VM heap base address (m_vm_base) never changes.**

All vm_offset_t values are byte offsets from m_vm_base.  Raw pointers
derived from offset_to_ptr() remain valid as long as the pointed-to
data has not been reclaimed by save/restore.  New allocations (vm_alloc)
advance m_vm_ptr but never move existing data.

This means:
- Pointers into the heap remain valid after subsequent allocations.
- "Re-read after alloc" patterns in the code are unnecessary (but some
  remain from earlier defensive coding — safe to remove).
- Snap-shot/thaw works because offsets are position-independent.

## Coroutine Stack Isolation

**Rule: Each coroutine has isolated stacks; the heap is shared.**

A coroutine can only access its own operand, exec, error, and
dict stacks.  The VM heap is shared across all coroutines, but
since only one coroutine runs at a time (single-threaded cooperative
scheduling), there are no data races.

**Corollary:** Passing data between coroutines requires heap-resident
objects (arrays, dicts, mailbox messages, pipe buffer items).  Objects
on the operand stack are private.

## Save Level on Objects

**Rule: Every Object placed on a stack or stored in a container must have
its save_level set to m_curr_save_level.**

The save/restore system uses save_level to determine which objects need
journaling.  An object with a stale save_level may not be journaled when
it should be, causing restore to produce incorrect state.

Pattern: `obj.set_save_level(trx->m_curr_save_level);` after creating or
cloning an object, before storing it.

## Control Operator Companion Contract

**Rule: A control operator must consume all its companion Objects from the
exec stack.**

When an operator uses the trampoline pattern (see interpreter.inl), it
pushes companion data (saved operands, indices, state) below a control
operator on the exec stack.  The control operator MUST pop these
companions when it fires — whether on success, failure, or error unwind.

Failure to consume companions corrupts the exec stack: the interpreter
would dispatch companion data as if it were code.

Pattern:
```
// Push:  [companion1] [companion2] [@control-op] [user-proc]
// Pop:   @control-op reads companions, adjusts m_exec_ptr -= N
```

## Operator Must Not Access Member Vars After Schedule

**Rule: After calling `coroutine_schedule()`, the calling operator must
not access any Trix member variables (m_op_ptr, m_exec_ptr, etc.).**

`coroutine_schedule()` may context-switch to a different coroutine.  The
member variables now belong to that other coroutine.  The original
coroutine's state is saved in its CoroutineContext and will be restored
when it is rescheduled.

This applies to: `coroutine_schedule()`, `coroutine_sleep_and_schedule()`,
and any function that calls them.

## Dict::put Ownership Transfer

**Rule: `Dict::put()` takes ownership of both key and value Objects.**

The caller must not use the Objects after passing them to put().  If the
caller needs to retain a value, it must `make_clone()` before calling put().

For existing keys, put() frees the caller's key (via maybe_free_extvalue)
and moves the value into the entry.  For new keys, both key and value are
moved into the new DictEntry.  String keys are copied to a new VM
allocation (the caller's original string data is not referenced).
