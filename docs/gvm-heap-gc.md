# Global VM Allocator + Garbage Collection

This document covers the **global VM region** -- a second heap that
lives alongside the local bump arena and is managed by a dlmalloc-style
allocator with a stop-the-world precise mark-sweep garbage collector.

It is intended for two audiences:

1. **Trix users** who want to allocate persistent data that survives
   `save` / `restore` -- see [User Guide](#user-guide).
2. **Maintainers** working on the allocator, the GC, or any code that
   touches the global heap -- see [Maintainer Reference](#maintainer-reference).

---

## Why two heaps?

The original Trix VM was a single bump-allocated arena.  `save` /
`restore` worked by rolling the pointer back to a barrier and undoing
journal entries in reverse.  That model is wonderful for short-lived
allocations and disastrous for anything that needs to outlive a
`restore` -- e.g. an actor's mailbox, a persistent dict accumulating
log statistics, a logic-program `find-all` result set built across
many backtracks.

The **global VM** is the answer.  It is a second region above the local
arena, allocated from the top of the VM downward.  Three properties
distinguish it from the local heap:

| Property              | Local VM                | Global VM                        |
| --------------------- | ----------------------- | -------------------------------- |
| Allocator             | bump (O(1))             | dlmalloc segregated free list    |
| Free-block reuse      | no (rollback only)      | yes (coalesce + segregated bins) |
| `save` / `restore`    | journaled; rolled back  | journal-skipped; survives        |
| Reclamation           | rollback at `restore`   | mark-sweep GC                    |
| Per-block header      | none                    | 16 bytes (`GvmBlock`)            |
| Heap growth direction | upward from `m_vm_base` | downward from `m_vm_limit`       |

The two regions share the same contiguous VM bytes.  Local grows up,
global grows down, and the gap between them is the available free
budget for either side.  When `vm_alloc` (local) or `gvm_alloc`
(global) cannot satisfy a request from its own region, the allocator
raises `Error::VMFull`.

---

## User Guide

### Opting into global allocation

There are two scanner-aware surfaces (per-form directives and the
`${...}` scope block) and two runtime ops (`set-global` /
`current-global`).  Pick the smallest scope that fits.

#### Per-form directives (cheapest)

```trix
$/foo             % literal name interned in global VM
$\foo             % executable name interned in global VM
(str)#$           % string bytes in global VM
[arr]#$           % array storage in global VM
<<dict>>#$        % dict struct in global VM
{{set}}#$         % set struct in global VM
{proc}#$          % procedure storage in global VM

% Force-local inverses (override an enclosing ${...} for one literal):
$$/foo                 % literal name force-interned in local VM
$$\foo                 % executable name force-interned in local VM
(str)#$$ ... {proc}#$$ % force-local container/proc storage
```

`#$` is **container-only**: `<</a 1>>#$` places the dict struct in
global VM but the literal name `/a` is interned wherever it would
have been (local by default).  Use a scope block to globalise both.

`#=`, `#$`, and `#$$` are **mutually exclusive** in the first slot;
any pair raises `/syntax-error`.

#### Scope blocks

```trix
${ body }
```

Scans `body` with the scanner's global-alloc flag set, then runs `body`
with the runtime flag set.  The flag is saved on entry and restored on
exit -- including on error unwind.

```trix
${
    /populate {
        g /alpha 1 put
        g /beta  2 put
        g /gamma 3 put
    } def
}
```

#### Direct flag control

When `${...}` is not a fit -- e.g. the flag must span a procedure call,
or be driven by a Boolean computed at runtime:

```trix
true set-global             % bool --
% allocations here land in global VM
false set-global

current-global              % -- bool : peek the flag
```

Concrete cases:

<!-- doctest: skip (synopsis: load-config-from-disk/parse-feed-file etc. are stand-in helpers) -->
```trix
% (1) Long initialisation split across procs.  init-app enforces the
% policy in one place; helpers stay agnostic.
/init-app {
    true set-global
    load-config-from-disk
    build-name-index
    register-handlers
    false set-global
} def

% (2) Caller-driven persistence flag; can't be a literal scope.
/load-feed {
    % path bool -- result-dict
    set-global
    parse-feed-file
    false set-global
} def

(catalog.json) true  load-feed /catalog exch def     % persistent
(query.json)   false load-feed /one-shot exch def    % local
```

For everything else -- literals, single-proc scoped scans -- prefer
`${...}`, `$/foo`, or `<lit>#$`.

### Runtime ops honour the flag

The `int -- container` ops -- `array`, `dict`, `dynamic-dict`, `set`,
`string` -- consult `m_curr_alloc_global` and route to global VM when
set.  So `${ 100 dict }` produces a global dict, `${ 256 string }` a
global string buffer.  No separate `make-global-XXX` family exists or
is needed; the flag is the single uniform mechanism.

### Per-coroutine state

`m_curr_alloc_global` is **per-coroutine**.  Each coroutine carries
its own flag through `flush_running` / `coroutine_load`, so a child
coroutine that did `true set-global` does not bleed its policy into a
sibling on resume.  Spawning a coroutine inherits the parent's flag
value at spawn time.

### Restore semantics

Names interned in global survive `restore`; names interned in local at
`save_level > 0` are unlinked when their save level rolls back.  A
Dict allocated globally keeps its Name-keyed entries valid **only if
the Names themselves are also global** -- otherwise `restore` strands
the entries.  The `${...}` block solves this by globalising both the
Names and the container; `<<>>#$` alone does not.

The contrast with the older `-persist` family is in
[`save-restore.md`](save-restore.md) § The Persist Family.  Short
version: `-persist` ops mutate without journaling but still allocate
in the current region; `${...}` allocates upfront in global.  Prefer
`${...}` for long-lived containers, `-persist` for ad-hoc per-mutation
persistence.

### Garbage collection

GC runs **automatically on `Error::VMFull`** at the global heap.  The
allocator tries fastbin, then general free list, then bump.  If all
fail and `m_in_gc` is not set, the allocator triggers one mark-sweep
pass, then retries the allocation sequence once.  If it still fails,
it raises `Error::VMFull` and the user sees a clean out-of-memory
error.

You can also trigger GC explicitly:

```trix
vm-global-gc                % --
```

This is useful in test suites that want to assert specific reclamation
behaviour, or in long-running services that want to amortise GC cost
during idle periods rather than letting it fire at allocation time.

### Introspection

```trix
vm-global-info              % -- dict
```

Walks the per-block header chain and returns a per-`ChunkKind`
histogram:

```
<<
  /total-blocks  int        % all global blocks across all kinds
  /total-bytes   int        % including headers + padding
  /payload-bytes int        % payloads only
  /header-bytes  int        % total - payload (includes any pad)
  /by-kind       <<
    /other        <</count int /total-bytes int /payload-bytes int>>
    /name         <<...>>
    /dict         <<...>>
    /set          <<...>>
    /array        <<...>>
    /string       <<...>>
    ...
  >>
>>
```

The schema enumerates **every** `ChunkKind` -- 31 kinds spanning the
full taxonomy of heap-backed Trix Object types.  Reserved-for-future
kinds report `count: 0` until a per-type allocation path migrates to
`gvm_alloc<T>(size, kind)`.  Enumerating them upfront keeps the wire
format stable: no enum-byte churn, no consumer breakage when new
kinds activate.

### Status keys

`/foo query-status` or `//:status:foo` reads the following counters
without allocating:

| Key                   | Type    | Returns                                                               |
| --------------------- | ------- | --------------------------------------------------------------------- |
| `vm-global-used`      | Integer | Bytes between `m_vm_global` and `m_vm_limit` (occupied region size).  |
| `vm-global-num-alloc` | Integer | Count of live user blocks in the global region (gc-scratch excluded). |
| `vm-global-num-free`  | Integer | Count of blocks currently on the global free list (main + fastbins).  |
| `gc-runs`             | Long    | Total GC passes since boot.                                           |
| `gc-last-reclaimed`   | Integer | Bytes reclaimed by the most recent pass.                              |
| `gc-in-progress`      | Boolean | `m_in_gc` -- true while a pass is running.                            |
| `gc-current-gen`      | Integer | Current mark-generation bit (0 or 1).                                 |

`gc-current-gen` is the test-suite handle: it flips between 0 and 1 on
every GC pass, so a scope's pass count parity is observable, but the
canonical "exactly N passes occurred" check is `gc-runs` (a monotonic
counter), snapshotted before / after.

`vm-global-num-alloc` and `vm-global-num-free` are live counters --
O(1) reads vs `vm-global-info`'s O(N) heap walk.  Useful for leak
detection (alloc grows without free shrinking), free-list health
checks (free-block count climbing under churn signals fragmentation),
and regression tests that need to assert allocator state without
the overhead of a full walk.

### Examples

The maintained showcases that exercise global VM + GC:

* **`examples/genealogy.trx`** -- Prolog-style logic over a family
  tree; alternatives return global `${[pa ch]}` pairs that survive
  the find-all accumulator's rollback boundary.
* **`examples/heist.trx`** -- set-cover heist planner; uses the
  persist family for analyst notes and global containers for the
  suspect catalog.
* **`examples/amazing.trx`** -- maze generator with multiple
  topologies, colormaps, and a Buck-style weave renderer; allocates
  large per-cell byte arrays that need to persist across phase
  boundaries.

See `examples/README.md` for the full catalog.

---

## Maintainer Reference

This half of the document is the source of truth for the internals.
Source files: `src/gvm_heap.inl`, `src/gc.inl`, `src/vm_heap.inl`,
`src/hash.inl`.  Cross-cutting state lives in `src/member_vars.inl`;
the serialization fields are in `src/snapshot.inl`.

### Lineage

The data-structure design -- per-block boundary tags, segregated free
lists, in-place coalescing of adjacent free blocks, top-chunk advance
-- comes directly from Doug Lea's `dlmalloc` reference implementation
([writeup](https://gee.cs.oswego.edu/dl/html/malloc.html), 1996).
Every production allocator since (jemalloc, tcmalloc, ptmalloc,
mimalloc) refines the same foundation.

Trix's implementation is a **ground-up rewrite**, not a port.  Lea's
original is ~6000 lines of C; Trix's `gvm_heap.inl` is ~1700 lines of
C++23, most of it comment and invariant documentation.  We deliberately
drop everything Trix does not need: mmap fall-back, sbrk growth,
multi-arena, the full malloc ABI, 32/64-bit dual layout, Windows /
macOS paths, the M_MXFAST / M_TRIM_THRESHOLD knob apparatus, and
decades of debugging hooks.  When in doubt about a design call, the
default is "what would Lea do, assuming a single thread and a fixed
heap" -- prefer clarity over the last 5% of throughput.

### GvmBlock header

Every global allocation is prefixed by a 16-byte header.  Definition
in `src/gvm_heap.inl`:

```cpp
struct GvmBlock {
    uint32_t     m_size;            // bytes 0-3:  total block size, multiple of 8
    ChunkKind    m_kind;            // byte 4:     classification tag
    uint8_t      m_mark_gen;        // byte 5:     GC mark generation
    length_t     m_obj_count;       // bytes 6-7:  user-visible element count
    vm_offset_t  m_next_in_work;    // bytes 8-11: GC work-queue link
    uint32_t     m_magic;           // bytes 12-15: 0xDEADC0DE sentinel
};
static_assert(sizeof(GvmBlock) == 16);
```

Key constants (`src/gvm_heap.inl`):

```cpp
static constexpr vm_size_t GvmBlockAlignment = 8;
static constexpr vm_size_t GvmHeaderSize     = sizeof(GvmBlock);  // 16
static constexpr vm_size_t GvmTailTagSize    = sizeof(uint32_t);  //  4
static constexpr vm_size_t GvmMinPayloadSize = 8;
static constexpr vm_size_t GvmMinBlockSize   = 32;                // hdr + min + tail + pad
```

A 4-byte **tail tag** at `block_start + m_size - 4` mirrors `m_size`.
The tail tag enables O(1) backward coalescing: from `block_start`, read
the 4 bytes at `block_start - 4` to learn the previous block's size,
then read its header to check `m_kind == Free`.  Forward coalescing
is trivial: from `block_start`, jump `m_size` bytes forward to land on
the next header.

### Magic constant

`0xDEADC0DE`.  Properties (`src/gvm_heap.inl`):

* every byte `>= 0x80` -- no ASCII collision during backward scans
  through payload bytes;
* far above legitimate `m_size` values -- can't be mistaken for one;
* not 8-byte aligned (low byte `0xDE`) -- can't be mistaken for a
  valid offset;
* instantly recognisable in hex dumps.

The all-bytes-`>=0x80` property is **load-bearing**.  Backward header
scans walk through arbitrary payload data; a magic with ASCII bytes
would false-match string payloads.  The earlier candidate `0xDEADDA7A`
was rejected because `0x7A = 'z'` breaks this invariant.

The magic is checked at every free-list traversal hop and at every
backward-scan boundary.  Corruption of the magic is a hard error,
caught by `assert` in debug builds and `error(Error::Internal, ...)`
in release.

### ChunkKind taxonomy

Every block carries a `ChunkKind` tag.  As of the 2026-05-09 audit,
**Phase 3 of the migration is complete**: every composite Object type
that can route through `m_curr_alloc_global` has a `gvm_alloc<T>(size,
kind)` caller.  The full enumeration in `src/gvm_heap.inl`:

* **Leaves** (no Object references in payload; walker is a bare
  `return`): `Long`, `ULong`, `Address`, `Double`, `Int128`,
  `UInt128`, `String`.
* **Composites** (walker enumerates Object references): `Name`,
  `Dict`, `Set`, `Array`, `Curry`, `Thunk`, `Tagged`, `Record`,
  `Packed`, `CoroutineContext`, `PipeBuffer`, `Mailbox`, `Monitor`,
  `BindingBucket`, `BindingEntry`, `Supervisor`.
* **No-op walkers** (block is reachable through another walker that
  marks it directly; the block's own payload doesn't need scanning):
  `CoroutineStacks` (marked from `CoroutineContext::m_stacks_offset`;
  slot contents walked from the context's tip pointers because slots
  past the tips hold stale bytes from prior suspends), `GcScratch`
  (the GC's own lazy scratch block; not a user-visible payload),
  `HashEntry` (the standalone Dict/Set entry block allocated one-at-a-time
  by the `-persist` family; its `m_key`/`m_value` are marked by the owning
  Dict/Set's bucket walk, the only path that reaches it).
* **Retired or reserved** (`Other` kept for callers without a more
  specific tag; the rest are RETIRED stream/reactive ChunkKinds or
  RESERVED for future migrations): `Other`, `Stream`, `Cell`,
  `Continuation`, `Screen`.
* **Sentinel**: `Free` -- `gc_mark_object` errors on any root that
  points at one (catches latent dangling-payload bugs).

Note: the original `Coroutine` ChunkKind was split into
`CoroutineContext` (the fixed-shape control block) and
`CoroutineStacks` (the per-coroutine stack region) in commit
`9b8628c`.  The single enumerator no longer exists.

### Allocation algorithm

`gvm_alloc_impl(size, kind)` in `src/gvm_heap.inl`:

1. **Round up** `size` to the smallest multiple of 8 that fits the
   request plus header and tail tag.
2. **Fastbin lookup** (sizes `<= GvmFastBinThreshold = 64`):
   exclusive-match into one of four LIFO bins keyed by block size
   (32 / 40 / 48 / 56 bytes).  A request of size 32 goes only to
   bin[0]; no bin-skip search.  Phase 3c's H8 enhancement also tries
   **one bin up** before falling through to the general list, to
   amortise the "perfectly-sized block was just freed but it's in
   the next-size bin up" case.
3. **General free list**: doubly-linked, ordered by ascending block
   address.  First-fit walk; remainder is split if `>= GvmMinBlockSize`
   and routed back to fastbin (if newly bin-eligible) or general list.
4. **Bump from `m_vm_global`**: if free lists are empty, decrement
   `m_vm_global` by the rounded size and carve a fresh block out of
   the top of the global region.  Validates against `m_vm_temp_ptr +
   safety_slack`; raises `Error::VMFull` on collision with local.
5. **GC retry**: if all of the above fail and `m_in_gc` is not set,
   trigger one mark-sweep pass and retry from step 2.  At most one
   retry; double-failure raises `Error::VMFull`.

The allocator stamps `m_magic`, `m_kind`, `m_obj_count`, and
`m_mark_gen` on every freshly-handed block.  `m_mark_gen` is stamped
to the current `m_gc_current_gen`; the next pass flips that bit before
marking, so the fresh block reads as **unmarked** the moment the pass
starts and gets traced if reachable.

### Free-block layout

When a block is freed, its payload is reinterpreted as a `GvmFreeLinks`
overlay:

```cpp
struct GvmFreeLinks {
    vm_offset_t prev_free;  // nulloffset for head
    vm_offset_t next_free;  // nulloffset for tail
};
static_assert(sizeof(GvmFreeLinks) == 8);
```

This works because `GvmMinPayloadSize` is 8 -- every free block is
guaranteed to fit the overlay.  Insertion and removal route by size
through the `gvm_free_{insert,unlink}_anywhere` dispatchers, which
pick fastbin (size in range) or general list (size above threshold).

### Coalescing

After `gvm_free`:

1. **Forward**: check the next block's header at `block_start +
   m_size`.  If `m_kind == Free`, unlink it from its bin / list,
   absorb its size, rewrite the tail tag.
2. **Backward**: read the tail tag at `block_start - 4` to learn the
   previous block's size.  Validate (multiple of 8, `>= GvmMinBlockSize`,
   doesn't underflow into local region).  Read its header, check
   `m_kind == Free`, then absorb.
3. **Top-edge reclaim**: if the merged block's start equals
   `m_vm_global`, advance `m_vm_global` upward by the block size --
   reclaiming contiguous tops back into the bump frontier.  Repeats
   until the next block is non-Free.

### Garbage collection algorithm

`vm_global_gc()` in `src/gc.inl`.  Stop-the-world, precise, mark-sweep,
single generation.

#### Mark phase

1. **Advance generation**: `gc_advance_generation()` flips
   `m_gc_current_gen` (`^= 1`) -- a 1-bit flip-flop between 0 and 1.
   Flipping the bit makes every existing block's mark stale by
   definition, so there is no per-pass clear walk.  This is sound
   because every cycle sweeps the whole region (survivors all share one
   value, nothing older lingers) and fresh allocs are stamped with the
   pre-flip value so the flip makes them look unmarked.
2. **Root scan**: `walk_all_roots()` walks every root set the
   interpreter exposes -- operand / exec / error / dict
   stacks; system dicts (`systemdict`, `userdict`, `errordict`,
   `handlersdict`); the root-objects array; and the per-save-level
   suspension journal chains via `Save::gc_walk_chain`.
3. **Object marking**: `gc_mark_object(Object o)` resolves `o` to a
   global block (no-op for local-VM Objects and for ExtValue
   payloads), then stamps `m_mark_gen = m_gc_current_gen` and pushes
   the block onto the **work queue** via `m_next_in_work`.
4. **Work-queue drain**: pop blocks off `m_gc_work_head`, dispatch by
   `m_kind` to the appropriate per-kind walker, which calls
   `gc_mark_object` on every reachable Object.  BFS traversal; the
   intrusive `m_next_in_work` link guarantees enough storage for any
   live set by construction (one link per block, no fixed-size
   scratch).

The work queue is an **intrusive doubly-linked list** through
`GvmBlock::m_next_in_work`.  Push is O(1) at the head; pop is O(1)
from the head; the queue is exhausted when `m_gc_work_head ==
nulloffset`.  A block's `m_next_in_work == nulloffset` means
"not currently queued".

#### Sweep phase

`gvm_for_each` walks every block in address order from
`m_vm_limit` down to `m_vm_global`.  For each block:

* If `m_kind == Free`, skip (already on free list).
* If `m_mark_gen == m_gc_current_gen`, the block is live; skip.
* Otherwise, the block is unreachable; call `gvm_free_internal` to
  put it on the appropriate free list and coalesce.

The sweep maintains the **top-edge reclaim** invariant: contiguous
free blocks at the top of the global region merge into the bump
frontier, raising `m_vm_global` and making the bytes available to
both local (via the shared limit) and global (via fresh bump).

#### Mark generation bit

`m_gc_current_gen` is a **1-bit flip-flop** (stored in a `uint8_t`
field but only ever 0 or 1).  Each pass flips it (`^= 1`) before
marking, which makes every existing block's mark stale by definition
-- so there is **no per-pass clear walk, ever**.

Two invariants make a single bit sufficient:

1. **Full sweep every cycle.**  The sweep walks the whole region and
   frees every block whose mark isn't the current value, so after any
   sweep all survivors share one value -- nothing from the other
   generation lingers to alias on the next flip.
2. **Fresh allocs look old.**  A new block is stamped with the current
   (pre-flip) value; the next pass's flip makes it `!=` the new
   current, so the mark phase traces it if reachable and the sweep
   frees it if not.

The one wrinkle is the **probe** (`vm-global-gc-probe`): it marks the
reachable set but does *not* sweep, which would leave the heap
non-uniform (reachable blocks flipped, garbage not) and alias on the
next real GC.  So the probe runs as a **dry run** -- it restores
`m_gc_current_gen` and every block's `m_mark_gen` to their pre-probe
values before returning, making it a true no-op on heap state.  (The
old 0..255 counter needed no such restore: it advanced *past* the
probe's marks, leaving generations of slack.)

`gc-current-gen` exposes this bit to user code -- see [Status
keys](#status-keys) above.

### GC scratch buffer

`m_gc_scratch_offset` (`src/gc.inl`).  A lazily-allocated global
block of `ChunkKind::GcScratch` holds two GC-internal data
structures:

1. **Visited-set** for local-VM Objects encountered during root scan.
   Local Objects have no `m_mark_gen` of their own, so they can't be
   marked in place.  The visited set is a hash table; on overflow,
   `grow_gc_local_visited` doubles the scratch block size in place
   (mid-pass safe because `gvm_alloc` with `kind == GcScratch`
   bypasses the lazy-scratch lifecycle hook and `m_in_gc` gates the
   auto-on-VMFull retry).
2. **Initial work-queue seed** -- not strictly needed (the intrusive
   queue replaces it), but the scratch persists between passes once
   grown, so the visited-set capacity carries forward.

If the scratch can't grow because the global region is genuinely out
of memory, `gvm_alloc` raises `Error::VMFull` and the user sees a
clean OOM error rather than a buffer overflow.

### Snapshot / thaw

The global region is serialized as part of the snapshot.  Every
allocation's offset is relative to `m_vm_base`, so the heap is
position-independent and snapshots round-trip without pointer
relocation.

Two fields in `SnapShotHeader` (`src/snapshot.inl`) capture GC state:

* `uint8_t gc_current_gen` -- saved value of `m_gc_current_gen` at
  snapshot time.  Restoring it is **mandatory**: the bit must never
  reset to a default on thaw, or the post-thaw GC's first flip would
  collide with the `m_mark_gen` values stamped at save time -- a
  1-in-2 false-mark.  Saving the bit drives that probability to 0.
* `uint8_t curr_alloc_global` -- per-coroutine flag for the main
  coroutine.  Per-coroutine context flags for other coroutines are
  serialized inside each `CoroutineContext` block.

Current `SNAPSHOT_VERSION` is **176** (`src/types.inl`).

### Source file map

| File | Role |
| --- | --- |
| `src/vm_heap.inl` | Top-level dispatch: `vm_alloc_dispatch<T>` chooses local vs global based on `m_curr_alloc_global`.  Offset validation helpers. |
| `src/gvm_heap.inl` | Allocator: fastbins, free list, coalesce, top-edge reclaim.  `GvmBlock`, `ChunkKind` enum, magic constant, `gvm_for_each`. |
| `src/gc.inl` | Mark-sweep: root walk, `gc_mark_object`, per-kind walkers' dispatch table, sweep, generation flip-flop. |
| `src/hash.inl` | Hash primitives extracted from `vm_heap.inl` (used by the visited-set and elsewhere). |
| `src/save.inl` | `is_global()` / `is_above_barrier()` short-circuit global offsets in the save journal. |
| `src/snapshot.inl` | `SnapShotHeader` layout including `gc_current_gen` and `curr_alloc_global`. |
| `src/ops_memory.inl` | `vm-global-info` op (histogram). |
| `src/gc.inl` | `vm-global-gc` op. |
| `src/ops_flow.inl` | `set-global` / `current-global` ops. |
| `src/ops_system.inl` | `gc-runs` / `gc-last-reclaimed` / `gc-in-progress` / `gc-current-gen` / `vm-global-used` status keys. |
| `src/scanner.inl` | `${...}` scope block (`scan_global_block`), per-form `$/` `$\` `#$` parsing. |
| `src/init.inl` | Status-name interning; main `CoroutineContext` initialisation including `m_curr_alloc_global = false`. |
| `src/ops_coroutine.inl` | `CoroutineContext::m_curr_alloc_global` save / restore in `flush_running` and `coroutine_load`. |

### Adding a new ChunkKind

Four steps, in order (`src/gvm_heap.inl` enum block has the
canonical procedure):

1. **Add the enumerator** to `enum class ChunkKind`, choosing the
   next free byte value.  Document whether it is a leaf or composite.
2. **Add a case to `gvm_kind_name`** in `src/gvm_heap.inl` so
   `vm-global-info` reports it.
3. **Add a case to `walk_block_contents`** in `src/gc.inl` -- the
   dispatch table that the GC consults during work-queue drain.
4. **Add the per-subsystem walker** next to the struct definition.
   Leaves get a bare `return`; composites call `gc_mark_object` on
   every reachable Object reference in the payload.

The compile-time safety net is the missing-case error from
`walk_block_contents`: no `default:` clause, `-Wswitch-enum`, plus
`-Werror`.  Forgetting step 3 or 4 fails the build.

### Invariants

* Every live block has `m_magic == 0xDEADC0DE`.  Corruption is a
  hard error.
* Every block's size is a multiple of `GvmBlockAlignment = 8`.
* Every block's tail tag at `block_start + m_size - 4` mirrors
  `m_size`.
* Adjacent free blocks never exist -- coalesce maintains this.
* `m_next_in_work == nulloffset` iff the block is not currently on
  the GC work queue.
* A block with `m_kind == Free` is always on exactly one free list
  (fastbin or general).
* `m_curr_alloc_global == true` ⇒ allocations route to `gvm_alloc`;
  false ⇒ `vm_alloc`.  The flag is per-coroutine, saved and restored
  through context switches.
* Global-region offsets satisfy `offset >= (m_vm_global - m_vm_base)`;
  this is the `is_global(offset)` predicate.  The save journal's
  `is_above_barrier()` excludes global offsets unconditionally --
  they never roll back.

### Testing

* `tests/test_gvm_heap_fastbins.trx` -- exercises the four fastbins
  end to end, including bin-skip-up and split-into-bin paths.
* `tests/test_global_dict.trx` -- the global-dict path (the
  `<<>>#$` literal and `${ N dict }` op route).
* `tests/test_gc_vmfull.trx` -- VMFull-triggered GC and the
  post-throw queue-drain path.
* `tests/test_dollar_block.trx` -- `${...}` scope-block save /
  restore of the flag, including coroutine isolation.
* `tests/test_persist.trx` -- persist-family interaction with global
  containers; covers the BASE-write-after-persist regression
  (commit `d3f2e3a`).
* `tests/test_logic_global_results.trx` -- `find-all` accumulator
  routing through global VM, GC after accumulation.

Snapshot tests (`run_snapshot_tests`) round-trip the global region
including the `gc_current_gen` and `curr_alloc_global` fields; the suite
passes against the current `SNAPSHOT_VERSION` (176).

---

## See also

* [`save-restore.md`](save-restore.md) -- The save journal and how it
  skips the global region.
* [`snapshot-thaw.md`](snapshot-thaw.md) -- Snapshot header layout.
* [`vm-internals.md`](vm-internals.md) -- The local-VM bump allocator
  and shared offset machinery.
* [`scanner.md`](scanner.md) -- The `${...}` block and per-form
  `$/` / `$\` / `#$` directives.
* [`trix-reference.md`](trix-reference.md) § Global VM Allocation --
  User-facing op-by-op reference.
