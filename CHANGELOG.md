# Changelog

All notable changes to Trix are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The snapshot-image format version is tracked independently of the
public SemVer; see the README for the rationale and current value.

## [Unreleased]

(no changes yet)

## [0.9.0] - 2026-06-06

### Added

#### Release close-out (June)

- `match-all` operator: collect-everything pattern matching via
  mark accumulation, with a two-phase exit fence (`ExitFenceOp`) so
  `exit` inside a test or body raises `/invalid-exit` instead of
  corrupting the accumulation.
- `--resident` mode (`Config::m_resident`): explicit embedder/server
  pattern -- boot the VM, run the init script, and stay resident for
  host-driven interrupt dispatch instead of exiting at EOF.
  `ExitIRQ` stops a resident VM.
- `--sleep-budget=N`: cumulative wall-clock cap (ms) across all
  scheduler parks -- bounds total sleep for batch/CI runs.
- `--no-banner`: suppress the startup banner in script mode.
- Name hash table auto-scales with `--vm-size` (one bucket per 512 B
  of VM, snapped to the prime table; the default 1 MB VM keeps 2053
  buckets).  Name-heavy scans measure ~30% faster at 16 MB with no
  flag.  Explicit `--name-buckets` still wins.
- Z-Machine showcase: full unicode output (custom + default ZSCII
  translation tables, `print_unicode` UTF-8), a 2021-2025 award-
  winner catalog expansion, and `--self-test` growth to 288 asserts.

#### Compression

- Native `deflate`, `deflate-level`, and `inflate` operators (RFC 1951
  raw DEFLATE), backed by zlib.  Output is the bare bitstream -- callers
  add zlib (RFC 1950) or gzip (RFC 1952) framing themselves with `pack`
  plus the existing `crc32` / `adler32` ops.  `deflate-level` accepts
  level `0..9`; `deflate` defaults to level `6`.  `inflate` raises
  `range-check` on malformed / truncated / empty input and `vm-full`
  if expansion exceeds available VM scratch space (zip-bomb defence).
  Adds zlib to the required runtime dependencies (`-lz`, Ubuntu
  `zlib1g` / build-time `zlib1g-dev`).  Bumps SNAPSHOT 147 → 148.
- Streaming counterparts `deflate-stream`, `deflate-stream-level`, and
  `inflate-stream`.  Stack effects: `(in-stream out-stream -- )` plus a
  level argument for `deflate-stream-level`.  Read input until EOF and
  write raw RFC 1951 output to the destination stream incrementally via
  zlib's `Z_NO_FLUSH` / `Z_FINISH` state machine.  Lifts the 65 535-byte
  `MaxStringLength` cap that bounds the one-shot string variants -- any
  Stream-backed source / sink (file streams, stdio, string-streams up
  to their cap) works in either direction.  Internal scratch is two
  4 KiB buffers regardless of payload size.  Bumps SNAPSHOT 148 → 149.
- Streaming checksum ops `crc32-stream`, `fletcher32-stream`, and
  `adler32-stream` -- `(in-stream -- uint32)`.  Read the source stream
  to EOF and produce the same uint32 the one-shot variants would on
  identical input (adler32 preserves the NMAX=5552 mod-deferral;
  fletcher32 stashes a trailing odd byte across chunk boundaries).
  Useful for assembling a zlib (RFC 1950) or gzip (RFC 1952) wrapper
  around a `deflate-stream` payload that doesn't fit in one Trix string.
- `make-memory-stream (str -- in-stream)` -- wraps a Trix string as a
  read-only Stream by borrowing the string's VM-heap bytes (no
  host-side allocation, no copy).  A new `Stream::IsBorrowed` status
  flag suppresses the close-time `std::free` that the snapshot /
  invoke memory-stream paths rely on.  Lets streaming compression /
  checksum ops consume in-process Trix strings without round-tripping
  through a temp file.  Bumps SNAPSHOT 149 → 150.

#### Language

- Locals-capacity suffix `|names|#N` on procedure preambles.  By
  default the locals dictionary is sized exactly to the declared
  name count K, which leaves no room for working variables defined
  by `/tmp ... def` inside the body; appending `#N` (no whitespace
  between `|` and `#`) requests a dict of capacity `N >= K`,
  reserving `N - K` slots for body-local defs.  `N` range is
  `K..65535`; `N < K` is a `SyntaxError` and `N > 65535` is a
  `LimitCheck` at scan time.  Removes the need for
  `N dict begin ... end` boilerplate inside recursive evaluators,
  AST walkers, and recursive-descent parsers.
- Relative-capacity form `|names|#+N` -- requests `K + N` total
  slots regardless of how many names are declared.  Adding or
  removing an arg no longer cascades into a suffix bump.
- Empty header form `||#N` for procs that take no args but want a
  fresh frame dict for working variables.
- `local-def` operator -- explicit "write into the current frame
  dict" companion to `def` (which now skips frames; see Changed).
- `bind-locals` / `bind-into-dict` -- atomic batch dict binding.
  `bind-locals` writes N values to N names in the current frame;
  `bind-into-dict` writes them into an arbitrary dict.  Single op
  replaces N-deep `exch def` chains.
- Algebraic effects: `handle-effect` / `perform` with deep handler
  semantics built on delimited continuations.  Nested handler
  delegation, error-stack integration, per-coroutine isolation.
- Delimited continuations: `delimit` / `capture` / `is-continuation`
  with a new `Continuation` Object type.  One-shot enforcement;
  `capture` snapshots operand-stack slice, exec stack, save-level,
  and dict-stack and works across `|locals|` boundaries.
- `abort-exec` -- terminate a `delimit` body without running its
  continuation; clean unwind for one-armed `call/cc` patterns.
- Arbitrary user-defined error Names -- `throw` and `try-catch`
  accept any Name, not just the built-in error vocabulary.
- `range-from start stop -- arr` -- the explicit 2-arg range
  form.  `range stop -- arr` is now strictly 1-arg.
- Short-circuit `and?` / `or?` operators (proc-valued operands;
  evaluate lazily).
- Predicates: `interactive?` (is the stream a tty?),
  `coroutine-await` (Erlang-link rethrow), `coroutine-last-error`
  (per-coroutine cache of last joined/awaited/released exit reason).
- Time: `now -- ulong` returns monotonic milliseconds (companion
  to `clock` micros and wall-clock `epoch-time`).
- Numeric literals: `0xDE_AD`, `0o755_644`, `0b1010_1010`, hex
  floats `0x1.fp_3` accept underscore separators in the digit
  region (per-base predicate, not just decimal).
- 128-bit integer types `Int128` / `UInt128` with `q` / `uq`
  literal suffixes; `pack` / `unpack` `q` / `Q` format codes.

#### Persistence

- Persist family of mutation ops: `put-persist`, `def-persist`,
  `update-persist`, `undef-persist`, `array-store-persist`,
  `set-add-persist`, `set-remove-persist`.  These mutate without
  journaling, so the change survives `restore` -- enabling
  cross-iteration state in long-running save/restore loops.
  Validates `above-barrier` invariants at the call site.
- `crc32` / `fletcher32` checksum operators (zlib polynomial CRC,
  Wikipedia little-endian Fletcher-32).

#### Global VM allocation

PostScript-faithful global VM region: a journal-skipped area at the
top of the VM heap where allocations survive every `save`/`restore`
cycle.  Five scanner-aware surfaces opt allocations into global, plus
two runtime ops for direct flag control, plus full snapshot/thaw
round-trip.  Mirrors PLRM Level 2/3 §3.7.2 with Trix-native syntax
(per-form `$` prefix and `#$` suffix; no `make-global-XXX` family is
needed -- the runtime allocators consult the flag).

- `${ body }` -- scanner-aware scope block.  Body is scanned with
  the global-allocation flag set, so literal Names interned during
  scan land in global VM, AND runs the body with the runtime flag
  set so flag-honoring allocations also land in global.  Flag is
  saved/restored on entry/exit (incl. on error unwind).  Lexical
  scope; nests cleanly.
- `$/foo` and `$\foo` -- per-name prefixes that intern the literal /
  executable Name struct in global VM.  Pure scan-time effect; no
  runtime op.  `$//foo` and `$\\foo` (immediate-lookup forms) are
  rejected with `/syntax-error`.
- `(str)#$`, `[arr]#$`, `<<dict>>#$`, `{{set}}#$` -- per-literal
  suffixes that allocate the container's own storage in global VM
  (container-only; for full body propagation, write
  `${ <<...>> }`).  `#$` mutually exclusive with `#=`; combines with
  `#r` / `#w` access flags.
- `set-global` (`bool --`) and `current-global` (`-- bool`) -- direct
  control of the global-allocation flag for cases where `${...}`'s
  lexical scope doesn't fit (long initialisation sequences split
  across procedures, runtime-Boolean-driven allocation target).
- Runtime allocator ops `array`, `dict`, `dynamic-dict`, `set`,
  `string` (the `int -- container` family) consult the flag and
  route to global VM when set.  So `${ 100 dict }` produces a global
  dict, `${ 256 string }` a global string buffer, etc.
- `vm-global-used` status field reports bytes used in the global
  region.  Monotonic in the current bump-allocator implementation;
  a future free-list allocator (queued in
  `memory/plan_global_vm_dlmalloc.md`) will let it shrink.
- Snapshot / thaw round-trips the entire global region plus the
  `current-global` flag.  Capacity rule: when the snapshot contains
  a non-empty global region, the thaw target's VM capacity must
  match exactly (offsets in the global region are not rebased on
  thaw).  Bumps SNAPSHOT 150 → 151.
- VM Redux Phase 5+8 follow-up: `PipeBuffer` migrated to global VM.
  Mirror of the Phase 3 mailbox migration; closes the symmetric
  story for inter-coroutine pipes.

  `pipe_alloc_buffer` (ops_pipeline.inl) routes through
  `gvm_alloc<PipeBufferHeader>(.., ChunkKind::PipeBuffer)`.
  `ChunkKind::PipeBuffer = 19` flips from RESERVED to Active in
  `src/gvm_heap.inl` (the slot was reserved during Phase 3 prep
  for exactly this migration).

  Pool collapsed from `[save_count][MaxPipePoolSize]` to
  `[MaxPipePoolSize]` (drops the `m_curr_save_level *` factor in 3
  call sites).  Pipe buffers live in global VM, so the per-save-
  level dimension that prevented above-barrier-fragile reuse is no
  longer needed.

  Per-save-level pool reset in `save.inl` (was lines 740-743) deleted
  -- pipe pool entries don't dangle on restore now.

  No scrub function added (unlike Phase 3's `mailbox_scrub_above_-
  barrier`).  Phase 5 made coroutines survive restore, so there are
  no above-barrier coroutines whose blocked-writer/reader references
  in surviving pipe buffers would dangle.  Pipes are also SPSC
  (single-slot `m_blocked_writer` / `m_blocked_reader` rather than
  the multi-sender chain that mailboxes maintain).

  Bumps SNAPSHOT 159 -> 160 (pool array size formula changed; old
  snapshot pipe-pool blobs are a different size).  PATCH 54 -> 55.
  New test `tests/test_pipe_global.trx` (3 sections, 3 expects):
  /pipebuffer count grows on alloc; recycled buffer survives an
  outer restore (pool reuses post-restore); 3 nested alloc/restore
  cycles show bounded growth.  run_all.sh 176/0/8.  Snapshot 44/44.

  Side-track queue now: only the Force-local Name sigil remains,
  behind its 3 documented trigger conditions.

- VM Redux Phase 5 follow-up: `SupervisorState` and the supervisor
  start-proc array migrated to global VM.  Phase 4 deferred this with
  rationale "natural-owner stays local"; once Phase 5 made
  `CoroutineContext` + stacks restore-immune, that rationale dissolved
  -- a supervisor coroutine spawned at sl>0 would survive an outer
  restore but its `SupervisorState` (allocated at sl>0 in local VM)
  would be reclaimed, leaving the supervisor's exec stack offset
  dangling and the cross-coroutine validator rejecting the restore
  with /invalid-restore.

  Two allocator routings:
    `ops_supervision.inl::supervisor_op` -- `vm_alloc<SupervisorState>`
      becomes `gvm_alloc<SupervisorState>(.., ChunkKind::Supervisor)`.
    Same op's start-proc array (1-element Object array hosting
      `@supervisor-init`) -- `vm_alloc_n_ptr<Object>` becomes
      `gvm_alloc_n<Object>(.., ChunkKind::Array)`; save_level stamped
      `Save::BASE` instead of current.

  New `ChunkKind::Supervisor = 27` in `src/gvm_heap.inl`; `MaxChunkKind`
  bumped 27 -> 28; `gvm_kind_name` returns "supervisor".  By-kind
  introspection now reports a `/supervisor` field.

  Caller pattern unchanged from other Phase 5+ cross-restore guidance:
  pre-intern Name keys at BASE (or use `$/foo`), and `${ ... }` the
  child `/start` proc.  With those in place, supervisors spawned in a
  save scope survive arbitrary restores.

  Bumps SNAPSHOT 158 -> 159 (new ChunkKind byte appears in dlm chunk
  headers; old binaries can't decode it).  PATCH 53 -> 54.  New test
  `tests/test_supervisor_cross_restore.trx` (2 sections, 4 expects):
  spawn-at-sl=1 + restore round-trip (no /invalid-restore); scheduler
  tick after restore; `/supervisor` present in `vm-global-info /by-kind`
  with non-zero count.  Existing `tests/test_gvm_heap.trx` updated
  for the by-kind length 27 -> 28.  run_all.sh 175/0/8.  Snapshot 44/44.

- VM Redux R6 pointer-hygiene check landed.  Reject attempts to store
  a restore-fragile local-VM value into a container that lives in
  global VM.  PostScript-faithful rule: a global object must never
  reference a local object whose storage will be reclaimed by some
  future restore, leaving the global container holding a dangling
  offset.  Was deferred from VM Redux Phase 2 originally as "low-
  priority for now (most patterns route through ${...} which
  globalises descendants), but should land before serious adoption".
  Lands now ahead of any user adoption.

  New `Object::storage_offset()` -- unified accessor returning
  `m_offset` for both `uses_vm()` and `uses_heap_cell()` Objects.
  New `Save::reject_local_into_global(trx, container_is_global, val,
  op_name)` helper raising /invalid-access when the value's offset
  is at-or-above `m_save_stack[BASE+1]` (allocated at sl >= 1) and
  the container is global.  At-BASE local refs are immune (BASE
  storage survives every restore).  Pure value types (Integer,
  Boolean, Byte, Real, Mark) are immune.  Save-not-active short-
  circuits (everything live is at BASE).

  Wired into the C++ container methods so every Trix put goes
  through one centralized check:
    Dict::put, put_persist, put_persist_or_create -- both key + value
    Dict::set_put, set_add_persist_or_create -- key
    ops_array.inl::put_op (array branch), put_persist_op (array)
    ops_stack.inl::array_store_op, array_store_persist_op (batch
      pre-check; matches array-store-persist's all-or-nothing rule)

  New `Dict::is_global(trx)` predicate (`reinterpret_cast<vm_t *>(this)
  >= trx->m_vm_global`) -- O(1) pointer comparison gating the check.

  Interaction with the existing /above-barrier rule (-persist family):
  /above-barrier fires FIRST at the op layer for current-sl-fragile
  values, so the persist family still raises /above-barrier (not
  /invalid-access) for keys/values interned at the current save level.
  R6 catches the cases the existing rule misses: non-persist puts of
  current-sl values into global containers (where /above-barrier
  doesn't apply because the value would normally journal cleanly into
  a local container).  Net: persist-family error names unchanged;
  non-persist puts gain /invalid-access for the global-container case.

  test_dollar_block.trx, test_dollar_name.trx, test_global_integration.trx
  updated -- the prior "demonstrates the silent-corruption footgun"
  patterns now expect R6 to reject at write time.  New
  tests/test_pointer_hygiene.trx (7 sections, 21 expects) covers
  dict put / put-persist / set set-add / set-add-persist / array
  put / put-persist / array-store / array-store-persist; plus R6's
  no-op short-circuits at BASE and for local containers.

  Bumps PATCH 52 -> 53.  SNAPSHOT unchanged.  run_all.sh 174/0/8.
  Snapshot 44/44.

- CoroutineContext::m_creation_sl retired.  PF4 audit (during VM
  Redux Phase 5 prep) confirmed zero readers anywhere -- pure write-
  only state with three init sites (init.inl main coroutine, ops_-
  coroutine.inl coroutine_launch_common, ops_pipeline.inl pipeline-
  stage spawn).  Removing the field shifts m_activation_sl forward in
  the struct byte layout, breaking snapshot byte-stream compatibility
  for thawed CoroutineContexts (Phase 5 stores them in global VM as
  raw bytes).  Bumps SNAPSHOT 157 -> 158, PATCH 51 -> 52.  No test or
  documentation changes -- the field had no observable behaviour.

- VM Redux Phase 8 SHIPPED -- def-persist + put-persist (dict path) +
  set-add-persist accept missing keys at any save level by allocating
  the new entry in global VM.  Lifts the long-standing /above-barrier
  "key not found; new-entry insertion at sl > 0 would dangle on
  restore" rejection from def-persist, the dict path of put-persist,
  and set-add-persist.  The new DictEntry / SetEntry survives restore
  (global VM is journal-skipped); the chain-head write into
  m_buckets[bucket_index] and the m_length bump are non-journaled,
  mirroring the existing put_persist overwrite path.

  New methods in src/dict.inl:
    `Dict::put_persist_or_create(key, val)` -- like put_persist, but
      allocates a fresh DictEntry via gvm_alloc<DictEntry>(...) tagged
      ChunkKind::Other when the key is missing.  String keys: the key
      bytes are also copied into global VM via vm_alloc_global_ptr<vm_t>
      so they survive restore.
    `Dict::set_add_persist_or_create(key)` -- parallel for sets.
      Allocates a fresh SetEntry via gvm_alloc<SetEntry>(...) tagged
      ChunkKind::Other; already-a-member is a silent no-op.

  The original Dict::put_persist (overwrite-only, returns nullptr on
  missing key) stays in place for update-persist's @update-persist
  continuation, which keeps the existing semantics: if the proc
  removed the key concurrently, raise /above-barrier rather than
  silently re-creating the entry.

  Caller pattern: the value's above-barrier rule is unchanged.  The
  KEY's above-barrier rule is also unchanged -- a Name first interned
  inside a save scope still has its Name struct in local VM at sl>0
  and is rejected.  Two patterns avoid this:
    (a) Pre-intern the Name at BASE (e.g. `/foo pop` once at startup).
    (b) Use a global Name via $/foo (Phase 2 syntax).
  The Phase 8 test sections in tests/test_persist.trx exercise both.

  Bumps PATCH 50 -> 51.  SNAPSHOT format unchanged (no header fields
  changed; the new ChunkKind::Other allocations look like any other
  global block to the heap walker).  test_persist.trx sections 6, 9,
  13 updated to assert the new "creates and persists" semantics; +6
  expects across these sections (15 -> 21).  run_all.sh 173/0/8.
  Snapshot 44/44.

- VM Redux Phase 7 SHIPPED -- find-all and find-n migrated to the
  per-coroutine scratch arena.  Closes the original VM Redux motivation:
  yielding inside a find-all body no longer corrupts cross-coroutine
  state because each coroutine's accumulator is private.  Retires the
  process-wide PERSIST region entirely; heap layout simplifies from
  `| LOCAL | free | TEMP | PERSIST | GLOBAL |` to
  `| LOCAL | free | TEMP | GLOBAL |`.

  `ops_logic.inl::find_all_op` + `find_n_op` (10 call sites total)
  switch from `Trix::persist_*` to `Trix::scratch_*`.  Idle check is
  now `m_scratch_ptr == m_scratch_base` (running coroutine).  Result-
  Array order is unchanged (persist reversed last-pushed-first; scratch
  preserves chronological order; both end up first-found..last-found).
  The post-restore `m_vm_temp_ptr` clamp inside `at_find_all_fail_op` /
  `at_find_n_fail_op` is dropped -- `Save::restore` already clamps to
  `m_vm_global`, which is now the authoritative ceiling.

  Error cleanup at `ops_system.inl:2267` migrates from `persist_free`
  to `scratch_free` on the running coroutine.  Non-main coroutines
  errored out via the early `coroutine_kill` path (which now also
  walks scratch in `coroutine_cleanup_stacks`); main coroutine lands
  here with `m_scratch_ptr != m_scratch_base` only if find-all was in
  flight.

  `coroutine_cleanup_stacks` gains a 5th walk for scratch slots --
  ExtValue refs in cloned find-all results need `maybe_free_extvalue`
  before the stack block returns to `m_coroutine_stack_free`,
  otherwise the next coroutine to claim the slab inherits dangling
  refs.

  Removed: `persist_push<T>`, `persist_free<T>`, `persist_collect<T>`
  (vm_heap.inl); `m_vm_persist`, `m_persist_count` (member_vars.inl);
  `vm-persist-count` and `vm-persist-used` status keys (ops_system.inl
  + init.inl); the `m_persist_count != 0` guard in
  `vm_alloc_global_ptr`; the persist-region idle check in
  `snap-shot`; the persist-init in `init.inl`; the persist-tracking
  bookkeeping in `gvm_heap.inl`'s top-edge-advance.  `vm-temp-used`
  now uses `m_vm_global - m_vm_temp_ptr` (was `m_vm_persist`); semantic
  unchanged when persist was idle (which was the steady state).

  Per-coroutine scratch is fixed-size (default 128 Objects).  find-all
  collecting more than the configured depth raises /vm-full; tune via
  `--scratch-depth=N` (max 4096).  `tests/test_find_all_persist.trx`
  has a 1001-result stress section so its run-config gains
  `--scratch-depth=2048` in `tests/run_all.sh`.  Three obsolete
  sections (62-64) of that test referenced the dropped
  `vm-persist-count` / `vm-persist-used` status keys; removed.

  Bumps PATCH 49 -> 50.  SNAPSHOT format unchanged (no header fields
  changed; the persist region was per-process transient state, not
  serialised).  New test `tests/test_phase7_find_all_yield.trx` (5
  sections, 22 expects): yield mid-find-all preserves results; peer
  scratch-push during a find-all yield does not leak into the find-
  all's accumulator (the original VM Redux motivation); find-all
  under outer save/restore; non-Fail error inside find-all body
  cleans the scratch arena; collect order matches push order.

- VM Redux Phase 6 SHIPPED -- per-coroutine scratch arena.  Bump-and-
  collect arena carved from the back of every coroutine's contiguous
  stack block (5th partition after operand / execution / error /
  dictionary); main coroutine allocates a parallel slab via
  `vm_alloc_n_ptr` at init.  Sized by the new `--scratch-depth=N`
  CLI flag (Min 16, Default 128, Max 4096 Objects = 1 KB at default).

  C++ API in `vm_heap.inl` mirrors the existing `persist_*` family
  but stores in the running coroutine's slab:
    `scratch_push<T>(val)`     bump-push, raises /vm-full on overflow
    `scratch_free<T>()`        reset to base, walking elements for
                               ExtValue cleanup
    `scratch_collect<T>()`     copy contents (push order preserved)
                               into a fresh `vm_alloc_n` array,
                               reset arena.  Returns the array Object.
  Push order preserved by `scratch_collect` (find-all wants
  chronological), in contrast to `persist_collect` which reverses
  (persist grows downward).

  Trix surface in new `ops_scratch.inl`: `scratch-push (val --)`,
  `scratch-collect (-- array)`, `scratch-clear (--)`.  Public ops;
  primary planned client is Phase 7's find-all migration.

  Layout impact: `CoroutineContext` grew 216 -> 232 bytes (3 new
  vm_offset_t fields: `m_scratch_base`, `m_scratch_ptr`,
  `m_scratch_limit`).  Trix member vars added the matching live
  pointers (`m_scratch_base / limit / ptr` as `Object *`) plus
  `m_scratch_depth` (stack_depth_t, copied from
  `Config::m_scratch_depth` at init).
  `coroutine_flush_running` saves `m_scratch_ptr`; `coroutine_load`
  reloads all three live pointers from the new context.

  Snapshot/thaw: SnapShotHeader gains 3 vm_offset_t fields
  (`scratch_base_offset`, `scratch_ptr_offset`, `scratch_limit_offset`)
  for main's scratch.  Header size 544 -> 552.  Thaw recovers
  `m_scratch_depth` from `(limit - base)` so a thawed image preserves
  its original sizing even when the new Trix instance was constructed
  with a different `--scratch-depth`.

  Bumps SNAPSHOT 156 -> 157, PATCH 48 -> 49.  New test
  `tests/test_phase6_scratch.trx` (5 sections, 17 expects): push/
  collect order; empty drain; mixed-type round-trip (Integer / Real /
  String / Name / Long-via-ExtValue); clear semantics; two concurrent
  coroutines have independent scratch arenas with yields between
  pushes (no cross-leak).

- VM Redux Phase 5 SHIPPED -- coroutine context, stack block, and
  per-coroutine binding cache globalised.  Four allocator routings:
  `coroutine_alloc_context` and `coroutine_alloc_stacks` (in
  `ops_coroutine.inl`) move from `vm_alloc<...>` to
  `gvm_alloc<...>(.., ChunkKind::Coroutine)`; `binding_table_alloc_bucket_array`
  and `binding_table_alloc_entry` (in `binding_table.inl`) move to
  `gvm_alloc<...>(.., ChunkKind::BindingBucket)` and
  `gvm_alloc<...>(.., ChunkKind::BindingEntry)`.

  Behavioral change (deliberate): a coroutine spawned at save level N
  now SURVIVES an outer restore past N.  Pre-Phase-5 the
  `m_context_offset >= barrier` kill walk in `Save::restore`
  slaughtered every such spawn; post-Phase-5 every CoroutineContext
  lives in global VM and the gate is trivially-true for every
  coroutine, so the walk had to go.  Mailboxes + monitor relationships
  persist across the restore (already global since Phases 3+4); user
  code can join the survivor as usual.  The cross-coroutine validator
  still rejects above-barrier local-VM composites in surviving stacks
  -- if the proc body or any held composite was scanned in the save
  scope, restore raises `/invalid-restore`.  Workaround: define proc
  bodies before save, or use `${...}` to globalise.

  Restore-side dead-code deletes: the running-coroutine destroy guard,
  the above-barrier kill walk, the dead-list unlink walk, and the four
  free-list above-barrier scrubs (m_coroutine_ctx_free,
  m_coroutine_stack_free, m_binding_bucket_free, m_binding_entry_free)
  all became unreachable after the migration; removed.  The
  scheduler-rebuild pass is kept (defensive, idempotent under the new
  state model where neither heads nor per-coroutine list fields are
  journaled).  The cross-coroutine stack-content scan is also kept;
  its `m_activation_sl >= restore_level` guard remains a valid perf
  optimisation.

  `binding_table_prune_above_level` patched: pre-Phase-5 the chain
  walk skipped the free-list push for entries `>= barrier` (assuming
  rollback would reclaim them), but global-VM entries are always
  above local barriers AND never roll back -- without the push every
  restore would leak BindingEntry slots permanently.  Push condition
  relaxed to `(curr < barrier || is_global(curr))`.  Sympathetic
  fix in `binding_restore_all_coroutines`: don't null out a
  survivor's bucket array if it sits in global VM.

  `ChunkKind::Coroutine` (= 18) flips RESERVED -> Active.
  `ChunkKind::BindingBucket` (= 25) and `ChunkKind::BindingEntry`
  (= 26) are added.  `MaxChunkKind` 25 -> 27.  `gvm_kind_name` gains
  `coroutine`, `bindingbucket`, `bindingentry` entries.

  Bumps SNAPSHOT 155 -> 156 (slot-count change in /by-kind schema)
  and PATCH 46 -> 48 (one bump per of two commits in the phase).
  New test `tests/test_phase5_coroutine_global.trx` covers all four
  Active-slot bumps + the post-restore survival semantics + monitor
  /down delivery across a Phase 5 context boundary.

  Test infrastructure: 8 existing tests adjusted for the shifted
  baselines.  `test_status`, `test_temp_alloc`, `test_extvalue_torture`,
  `test_name_torture` extend their `vm-used + vm-temp-used + vm-free
  = vm-total` invariant to include `vm-global-used` (main coroutine
  resident in global VM at startup).  `test_gvm_heap` section 1
  baseline shifts from "empty global region" to "main resident";
  section 2 captures pre/post deltas instead of absolute counts.
  `test_global_composite` section 6 adds /coroutine and
  /bindingbucket Active assertions.  `test_frame_dict_coroutine`
  sections 3 and 4 documented the killed-by-restore -> /invalid-restore
  + kill + retry pattern (post-save spawns now poison the save scope
  unless their proc body is below the barrier).  `tests/run_all.sh`
  joins `test_supervisor.trx` to the 2 MB list -- post-Phase-5 each
  spawn consumes ~5 KB in the global region, and the supervisor
  stress sections need ~1.3 MB total.

  PF4 audit lived up: `m_activation_sl` write at `coroutine_load`
  is load-bearing (encodes scheduling invariant); the read-side
  guard at `save.inl:626` STAYS as a perf optimisation (the plan's
  claim it would be removed was over-optimistic -- the surrounding
  cross-coroutine scan didn't disappear, only the kill walk did).
  `m_creation_sl` has zero readers anywhere (writes-only in init,
  ops_coroutine, ops_pipeline) -- candidate for retirement, not
  urgent.

- VM Redux Phase 4 SHIPPED -- actor monitor / link entries globalised.
  `supervision_create_monitor` (`ops_supervision.inl`) routes through
  `gvm_alloc<MonitorEntry>(sizeof, ChunkKind::Monitor)` instead of
  `vm_alloc`; monitor entries now live in global VM, BASE-immune to
  save/restore.  `m_monitor_pool` collapses from a per-save-level
  array (`vm_offset_t *m_monitor_pool` indexed by `m_curr_save_level`)
  to a single bare-offset head (`vm_offset_t m_monitor_pool`); the
  per-save-level pool reset in `save.inl` is removed; `stream.inl`
  init no longer needs a runtime allocation for the pool.

  Hazard-resolution turned out to be free: MonitorEntry is
  symmetrically referenced (source's `m_monitoring` list AND
  target's `m_monitors` list), and `supervision_notify_death` (called
  from `coroutine_kill` in `save.inl`'s above-barrier coroutine kill
  walk) explicitly unlinks BOTH sides + frees the entry.  Differs
  from Phase 3 mailbox migration (which needed a Hazard #1 scrub
  because `m_blocked_sender` / `m_blocked_reader` were referenced
  only from the mailbox side, asymmetrically).

  `ChunkKind::Monitor` (= 24) added to the dlmalloc enum (Active);
  `MaxChunkKind` 24 -> 25.  `gvm_kind_name` gains the `monitor`
  string.  Plan items "Supervisor tree nodes" deferred -- `SupervisorState`
  is naturally tied to its supervisor coroutine's lifetime and
  globalising it would just delay reclaim without enabling new
  user-visible behavior until VM Redux Phase 5 ships.  "Named-actor
  registry" plan item is N/A: no such feature exists -- the
  `actor-set-name` / `actor-name` pair stores a debug-name `vm_offset_t`
  on the `CoroutineContext` directly (already global since Phase 2),
  not in a registry dict.

  `gvm_for_each` block-walk caller in `vm_global_info_op` updated
  for the Phase 3 visitor signature change (already in place; no
  further change needed).

  Snapshot field `monitor_pool_offset` re-interpreted: was a VM-
  pointer offset for the per-level array; now a bare free-list head
  offset.  Wire format unchanged (one `vm_offset_t` either way).

  New regression test `tests/test_phase4_monitor_global.trx`
  (4 sections, 4 expects) covering: alloc bumps `/monitor`;
  above-barrier monitor entries don't leak across restore;
  link/unlink + monitor leaves bounded growth; nested save/monitor/
  restore cycles bounded.  Test wraps body in a tester-actor (the
  `monitor` op requires the caller to be an actor); main joins via
  `coroutine-status /dead eq` polling.

  All existing supervision tests stay green
  (test_supervision.trx, test_supervisor.trx, test_genserver.trx,
  test_supervision_snapshot.trx).

  Bumps SNAPSHOT 154 -> 155.  PATCH 45 -> 46.  run_all.sh 169 ->
  170 / 0 fails.  Snapshot 44/44.

- VM Redux Phase 3 SHIPPED -- actor mailboxes globalised.
  `actor_alloc_mailbox` (`ops_actor.inl`) routes through
  `gvm_alloc<MailboxHeader>(total, ChunkKind::Mailbox)` instead of
  `vm_alloc<MailboxHeader>`; mailboxes now live in global VM,
  BASE-immune to save/restore.  `m_mailbox_pool` collapses from
  `[MaxSaveLevel][MaxMailboxPoolSize]` to `[MaxMailboxPoolSize]`
  (1D, class-indexed only); the per-save-level pool reset in
  `save.inl` is removed.  Retires the `ops_actor.inl:166`
  workaround that motivated the entire VM Redux arc.

  Two restore-time hazards handled:
    1. Above-barrier actor's mailbox no longer leaks: the existing
       `coroutine_kill -> coroutine_cleanup_mailbox -> mailbox_recycle`
       chain (already invoked by `save.inl`'s above-barrier
       coroutine kill walk) parks the global mailbox back in the
       class-indexed pool, where the next same-class spawn reuses
       the offset.
    2. Surviving actor's blocked-sender / blocked-reader lists
       referencing about-to-die coroutines no longer dangle: new
       `mailbox_scrub_above_barrier(barrier)` (called from
       `save.inl::restore` after the kill walk, before the heap
       rollback) walks every live `Mailbox` block and unlinks
       entries whose `CoroutineContext` offset is above the barrier.

  `gvm_for_each` visitor signature gains a leading `vm_offset_t
  payload_offset` argument so scrubbers can recover a typed pointer
  via `offset_to_ptr<T>(payload_offset)` -- one existing caller
  (`vm-global-info`'s histogram walker) updated to ignore it.

  `ChunkKind::Mailbox` (= 23) is now Active (was RESERVED ahead of
  this commit).  `MaxChunkKind` is unchanged at 24 (the slot was
  added in the previous prep commit).  No SNAPSHOT bump (block
  format unchanged from the slot-add commit).

  New regression test `tests/test_phase3_mailbox_global.trx`
  (4 sections, 4 expects) verifies: (i) mailbox allocation bumps
  `/by-kind/mailbox`; (ii) above-barrier actor's mailbox doesn't
  leak across restore; (iii) surviving actor's blocked-sender list
  scrubbed of dying senders; (iv) deep nested save/restore cycles
  show bounded growth (pool recycle works).  All 53+ existing
  actor / supervisor tests stay green.

  PATCH 44 -> 45.  run_all.sh 168 -> 169 / 0 fails.  Snapshot 44/44.

- `ChunkKind::Mailbox` (= 23) added to the dlmalloc enum, RESERVED
  ahead of VM Redux Phase 3 (in-flight).  No caller yet -- the slot
  exists so the introspection schema is ready when
  `actor_alloc_mailbox` migrates to `gvm_alloc<MailboxHeader>`.
  `MaxChunkKind` bumps from 23 to 24; `gvm_kind_name` gains the
  `mailbox` string.  Test additions: `test_gvm_heap.trx` Section 1
  expects `length 24`; `test_global_composite.trx` Section 6 asserts
  `/mailbox` count stays 0 pre-Phase-3.  Bumps SNAPSHOT 153 → 154
  (introspection schema change; old snapshots can't have Mailbox-
  tagged blocks but a snapshot from a Mailbox-aware build could).
- `ChunkKind` enum re-categorises `Coroutine` and `PipeBuffer`
  from RETIRED to RESERVED (no code).  The original Migration 4
  framing was premature: VM Redux (`plan_vm_redux.md`) explicitly
  plans to globalise both -- Phase 3 migrates actor mailboxes
  (sibling type to `PipeBufferHeader`), Phase 5 migrates
  `CoroutineContext`.  Once those phases ship, the symmetry
  argument forces PipeBuffer to follow.  The RESERVED group now
  holds Coroutine + PipeBuffer + Screen; RETIRED holds Stream +
  Cell + Continuation.  Integer values unchanged so the
  introspection schema stays stable.  `tests/test_global_composite.trx`
  Section 6 message strings updated to match.  No SNAPSHOT bump.
- `ChunkKind::Continuation` tagged RETIRED (dlmalloc Migration 5 --
  decision, no code).  `ContinuationContext` is a self-contained
  value copy of the captured exec / err / dict / op slices
  (`memcpy` at capture time, no live stack pointers), and the
  capture save level is stamped on the **Object handle**, not in
  the ctx (`Object::m_continuation_save_level`).
  `execute_continuation` already raises a clean `InvalidAccess`
  on a stale resume after restore (`interpreter.inl:705`) without
  dereferencing the ctx.  Migrating the ctx to global VM would not
  enable any new user-observable behaviour -- a captured
  continuation in a global Dict is already correctly rejected
  post-restore.  True cross-restore continuations would require
  deep globalisation of every captured Object plus save-level
  rewrite semantics, which is a separate feature, not a
  routing-through-a-flag migration.  Test
  `tests/test_global_composite.trx` Section 6 now asserts
  `/continuation` count stays 0 alongside the other four retired
  slots.  Only `Screen` remains RESERVED (Migration 6).  No
  SNAPSHOT bump.
- `ChunkKind` enum tags `Stream`, `Coroutine`, `PipeBuffer`, and
  `Cell` as RETIRED (dlmalloc Migration 4 -- decision, no code).
  These four types are subsystem-managed -- pool-allocated by
  `stream.inl` / `ops_coroutine.inl` / `ops_pipeline.inl`, or
  explicitly designed as non-persistent in `ops_reactive.inl` --
  and will never route through `gvm_alloc`.  Their introspection
  slots stay in `vm-global-info /by-kind` for forensic visibility
  (count=0 by design); the comment block in `src/gvm_heap.inl`
  spells out the per-type rationale.  `Continuation` and `Screen`
  remain RESERVED -- Migrations 5 and 6 are pending design passes
  on capture-time stack-copy semantics and cross-coroutine handle
  ownership respectively.  No SNAPSHOT bump.
- Composite types route through `m_curr_alloc_global` (dlmalloc
  Migration 3).  `Curry`, `Thunk`, `Tagged`, `Record` (schema +
  instance + record-clone-replacing-field), and `Packed` (via
  `make_packed_data`) factories consult the global flag the same way
  the existing Dict / Array / String / numeric paths do.  When the
  flag is set, the backing storage is allocated via
  `gvm_alloc_n<Object>(N, ChunkKind::X)` and survives every
  save/restore cycle, so a global Dict holding a curried proc, a
  thunk, a tagged value, a record instance, or a packed array no
  longer dangles after `restore`.  `record-type`'s 2-Object
  constructor proc body migrates as `ChunkKind::Array` (not
  `Record`), keeping the schema-or-instance Record bucket
  semantically clean.  Packed migration uses the existing
  scratch-space-then-copy pattern (mirroring the eqproc copy path)
  rather than streaming directly into global VM, since `vm_start_alloc`
  has no global counterpart.  `Stream` is intentionally not migrated:
  its backing struct is allocated from a fixed-capacity init-time
  pool (`stream.inl`) and recycled via `alloc_stream` /
  `free_stream`; pool entries are BASE-stamped, never move, and
  Stream Objects already survive save/restore via the pool
  architecture.  `ChunkKind::Stream` stays count=0 by design.
  Coroutine, PipeBuffer, Cell, Continuation, and Screen remain
  queued (Migration 4-6) -- their lifetimes are subsystem-managed,
  needing design decisions before allocator routing.  No SNAPSHOT
  bump (Migration 3 reuses the existing block format).
- Collapse the dlmalloc `IsFree` flag bit into a new
  `ChunkKind::Free` sentinel kind (value `0xFF`, outside the
  `[0, MaxChunkKind)` iteration range).  Free blocks now encode as
  `(m_kind = Free, m_flags = 0)` instead of
  `(m_kind = Other, m_flags = GvmFlagFree)`; `m_kind` becomes the
  single source of truth for "this block is free".  `m_flags` is
  retained as a 1-byte hole reserved for future flag bits.  All
  coalesce / double-free / fastbin-pop / `gvm_for_each` checks read
  `m_kind == ChunkKind::Free`; the `GvmFlagFree` constant is gone.
  `vm-global-info`'s `/by-kind` histogram is unchanged (Free is a
  sentinel, not iterated; free blocks remain at the top-level
  `/free-blocks` and `/free-bytes` fields).  Bumps SNAPSHOT
  161 -> 162 -- 161 free-block bytes are encoded with the old kind
  + flag pair, so 161 snapshots cannot be thawed by 162+ binaries.
- Segregated fastbins (dlmalloc Phase 3c).  The smallest free-block
  sizes now live in per-size LIFO stacks instead of the sorted-by-
  address general list: bin[0]=24-byte (`ExtValue` / `SetEntry`
  payloads), bin[1]=32-byte (`WideValue` / `DictEntry` payloads),
  bin[2]=40-byte, bin[3]=48-byte; 56-byte and larger blocks remain
  on the general list.  `gvm_alloc` exact-matches the bin first;
  `gvm_free` routes its forward / backward coalesce neighbour-unlink
  and final-insert through size-dispatched helpers
  (`gvm_free_{insert,unlink}_anywhere`) so a single free block lives
  in exactly one structure regardless of which path put it there.
  Top-edge advance still reclaims sub-threshold blocks at
  `m_vm_global` directly (no bin insertion).  Bin head pointers
  serialize in `SnapShotHeader` (4 × `vm_offset_t` = 16 bytes) and
  thaw verbatim alongside `gvm_free_head`; bin contents (the
  `GvmFreeLinks` payload overlay) survive byte-for-byte inside the
  global-region heap blob.  Replaces O(n) first-fit list walks with
  O(1) bin pops on the realistic `${...}` ExtValue-churn workload.
  Bumps SNAPSHOT 160 -> 161 (header 552 -> 568, checksum offset
  544 -> 560).
- Free + boundary-tag coalesce (dlmalloc Phase 3b).  Every global
  block now carries a 4-byte tail tag mirroring `m_size` at its
  trailing edge; freeing routes through `gvm_free`, which performs
  O(1) forward coalesce (via the next block's header) and O(1)
  backward coalesce (via the prev block's tail tag), then either
  inserts the merged block into a sorted-by-address doubly-linked
  free list OR -- if the merged block sits at `m_vm_global` --
  advances `m_vm_global` upward to reclaim the memory back into
  the unallocated region.  `gvm_alloc` now first-fit-searches the
  free list before bumping; same-size allocations reuse vacated
  slots without growing the heap.  `ExtValue::free` and
  `WideValue::free` route global frees through `gvm_free`, so the
  Migration 1+2 leak (overwriting a global Long / Double / etc. in
  a Dict no longer permanently strands its backing cell) is now
  closed.  No user-facing `free-global` op in 3b -- C++-internal
  frees only.  `vm-global-info` gains `/live-blocks`, `/live-bytes`,
  `/free-blocks`, `/free-bytes` top-level fields; `/by-kind` now
  counts LIVE blocks only.  Block layout change: minimum block
  size grows from 16 to 24 bytes (8 header + 8 free-list-link area
  + 4 tail tag, padded to 8-byte alignment) -- empty arrays /
  zero-byte payloads pay 8 extra bytes each.  Bumps SNAPSHOT
  152 -> 153.
- ExtValue / WideValue allocators route through `m_curr_alloc_global`
  (dlmalloc Migration 1+2).  When the runtime global flag is set
  (`${...}` block, `set-global true`, etc.), `Long`, `ULong`,
  `Address`, `Double`, `Int128`, and `UInt128` allocations land in
  global VM tagged with the matching `ChunkKind`, so numeric values
  stored in global Dicts and Arrays survive `restore` instead of
  dangling.  `ExtValue::alloc` and `WideValue::alloc` accept a
  `ChunkKind` parameter; on the global path they bypass the
  per-save-level free list entirely (globals are journal-skipped --
  Phase 3b's coalescing closes the resulting overwrite-leak).
  Companion `alloc_local` variants stay below the restore barrier
  even when the flag is set, so a `set-global true; save; ...;
  restore` pattern doesn't silently relocate a recovered ExtValue
  into global VM.  `find_lowest_offset` and `Save::check_stack`
  exclude global offsets from relocation counts (the latter was
  already correct in Phase 1).  `m_extvalue_save_level` is stamped
  `Save::BASE` for global cells (the field is inert there but BASE
  is the right sentinel).  `vm-global-info` `/by-kind` populates
  `long` / `ulong` / `address` / `double` / `int128` / `uint128`
  for the first time.
- Per-block headers + introspection (Phase 3a of the dlmalloc plan).
  Every global allocation is now wrapped in an 8-byte `GvmBlock`
  header tagged with a `ChunkKind`.  Six kinds active in Phase 3a
  (`name`, `dict`, `set`, `array`, `string`, `other`) plus 17
  reserved slots for the rest of the Trix heap-backed Object-type
  taxonomy (`long`, `ulong`, `address`, `double`, `int128`,
  `uint128`, `packed`, `stream`, `curry`, `thunk`, `tagged`,
  `record`, `coroutine`, `pipebuffer`, `cell`, `continuation`,
  `screen`) -- 23 kinds total.  Reserved slots report count=0 until
  per-type allocation paths migrate to `gvm_alloc<T>(size, kind)`;
  reservation up-front makes those migrations cheap and gives users
  a stable per-kind schema today.  New op `vm-global-info (-- dict)`
  walks the header chain and returns a per-kind histogram with
  `/total-blocks`, `/total-bytes`, `/payload-bytes`, `/header-bytes`,
  and a `/by-kind` sub-dict.  No behaviour change for existing
  global-VM users: payload offsets are unchanged; allocators add 8
  bytes of overhead (16 bytes minimum block including padding).
  Lives in `src/gvm_heap.inl` -- ground-up implementation, not a
  port, with full attribution to Doug Lea's "A Memory Allocator"
  (1996); Phase 3b will add `free-global` + boundary-tag coalescing
  on top of the same headers.  Bumps SNAPSHOT 151 → 152.

#### Terminal I/O

- `OpaqueHandle` Object type -- sub-typed by `HandleKind`
  (currently `Screen`; future Tilemap/Sample/...).  Single 5-bit
  type slot covers an extensible kind taxonomy via the Object's
  `length` field as discriminator.
- Screen subsystem: virtual screen buffers with diff-based render
  to ANSI streams.  Operators `screen`, `screen-render`,
  `screen-render-to`, `screen-resize`, etc.; companion
  `examples/lib/screen.trx` wrapper and `screen-demo.trx`.
- Raw-mode keyboard: `raw-mode`, `cooked-mode`, `raw-mode?`,
  `read-key-byte`, `read-key-byte-timeout`, `key-ready?`,
  `terminal-size`, plus `examples/lib/keys.trx` decoder and
  `with-raw-mode` wrapper.  Crash-safe restore via signal handler.
- ANSI escape library `examples/lib/ansi.trx` plus
  `make-string-stream` / `get-string-stream` for in-memory
  rendering.

#### Tooling and observability

- Interactive Trix-script debugger gated by `--inspect FILE`,
  `--inspect-on-error FILE`, and `--inspect-at /NAME FILE` plus
  `--no-color`.  Split-pane TUI implemented as a Trix module
  (`lib/debugger.trx`, ~880 LOC) on top of `TRIX_DEBUGGER`-gated
  C++ hooks (release builds compile it out).  Five panes (source +
  output + op/exec/dict/err stacks) plus a `:` command line and a
  status bar.  Source-line stepping by default (`s`/`n`/`o`/`c`/`q`
  + F-key aliases) with named-resolution + in-source `breakpoint`
  ops.  Color theme with a `/mono` variant; `Tab` cycles pane
  focus.  `:` commands include `b /name`, `clear /name`,
  `clear-all`, `bp`, `p EXPR`, `help`.  Multi-file source caching
  via `stream-name sid -- str`.  User-script stdout is captured
  into a 32 KiB writable string-stream while the script runs and
  surfaced in the output pane (cb-entry drain) or dumped to the
  real terminal after `alt-screen-leave` (uninstall path).  C++ ops
  added: 11 debugger ops (`debug-step`, `debug-step-over`,
  `debug-step-out`, `debug-continue`, `debug-break`,
  `debug-unbreak`, `debug-break-on-error`, `debug-call-depth`,
  `debug-pc`, `debug-pc-source`, `debug-on-event`) + 4
  introspection ops (`*-stack-snapshot` ×4, `proc-disasm`,
  `format-object`) + `debug-breakpoints`, `stream-name`, plus 2
  stream ops shared with general use (`set-stdout`,
  `clear-string-stream`).
- **User manual for the interactive debugger**: `docs/debugger.md`
  is a comprehensive ~950-line reference covering invocation, TUI
  layout, key bindings, every `:` command verb (with grammar and
  examples), a step-by-step tutorial, feature deep-dives (conditional
  breakpoints, one-shot breakpoints, watches, modal pane expand,
  sandboxed `:p` eval, multi-file source pane, output capture, frame
  navigation, command history), conditional compilation
  (`TRIX_DEBUGGER` macro + release-build behavior), a "How it works"
  showcase section (architecture, the C-side intrinsic surface, the
  STL carve-out, design highlights), known limitations, and a
  troubleshooting guide.  Linked from a new **Tooling** section in
  `docs/index.md`.
- `examples/debugger_tour.trx` -- intentionally-buggy 30-line
  recursive `find-min` script that the manual's tutorial walks
  through with step / break / conditional bp / watch / frame nav.
  Reproduces three wrong outputs (`0 0 0` instead of `2 42 25`) and
  guides the reader to the one-line fix.
- Command history in the debugger's `:` command line: pressing the
  up arrow walks backward through past submitted commands (newest
  first); the down arrow walks forward.  On the first up from a
  live (in-progress) buffer, the buffer is stashed; if the user
  steps back past the newest history entry with the down arrow,
  the stashed buffer is restored verbatim so edits aren't lost.
  Adjacent-duplicate pushes are deduped (repeating the same
  command back-to-back counts once); empty submissions are
  skipped.  Any non-nav key (printable / backspace / Enter / Esc)
  resets the navigation cursor, decoupling subsequent edits from
  history nav.  History persists across halt events within one
  debug session (32-entry cap, oldest entries fall off).  Pure
  Trix in `lib/debugger.trx`; no new C++ surface.
- Breakpoint hit-count display in the debugger.  Each registered
  bp now tracks how many times it has fired since being set; the
  `:bp` listing renders `/foo [3 hits]` (and combines with the
  existing `(once)` / `when {pred}` markers).  Counts include
  predicate-filtered silent-skips on conditional bps -- the count
  reflects "how often this name was dispatched while the bp was
  registered", not "how often the user actually saw a halt".
  Re-setting an existing bp via `b /name` preserves the count;
  `clear /name` and `clear-all` drop it (with the bp entry).  Bp
  registry migrated from Set<Name> to Dict<Name, Integer>
  internally; also moved from local-VM (which the
  `debug_breakpoints_set` helper claimed was global but wasn't --
  `create_set` ignores `m_curr_alloc_global`) to actual global
  VM via `create_global_dict`, fixing a pre-existing
  inconsistency.  New C++ op `debug-bp-hits /name -- int`.
- Width-adaptive pretty-print in the debugger.  Stack-pane
  previews now compute per-item width from `screen-cols`
  (formerly hardcoded 14): `clamp((cols - 9) / 4, 10, 30)`.
  At 80 cols the per-item width is 17; at 120 cols it's 27;
  at narrow 60-col terminals it falls back to 12.  The modal
  pane's `format-object` width also adapts to `screen-cols`
  when a screen is allocated (former hardcoded 200 stays as
  the test-context fallback, since `dbg-modal-open` can be
  called without a screen in headless renders).  Reduces
  wasted right-side whitespace on wide terminals and lets
  users actually read the items being previewed.
- `proc-disasm` auto-unwraps `|locals|` procs: when the input
  matches the canonical `[/name1 ... K N inner-packed begin-locals]`
  outer shape, disasm returns rows for the inner user body
  directly.  Single-level unwrap; the user can recurse manually
  for nested `|locals|`.  Removes the friction of having to
  index into the synthesized outer body via `4 get` (or
  whatever) before disassembling.
- Frame navigation in the `:` command line.  Four new verbs:
  `up` moves to an older (caller) @call frame, `down` moves
  back toward the deepest halt, `frame N` jumps to a specific
  frame, `frames` lists them inline with a `*` marker on the
  selected one.  The source pane re-renders at the selected
  frame's source location.  New C++ op `frame-source-locs --
  array` (TRIX_DEBUGGER-gated) walks the exec stack for @call
  markers and emits `[sid line col name]` records in top-frame-
  first order.  Frame state refreshed once per cb fire and
  reset to the deepest frame (index 0) at each halt.  Limitation:
  per-frame line numbers reflect call-time `m_last_scan_location`,
  which doesn't update during proc-body dispatch -- frames called
  from inside an already-scanned proc body all report the same
  line as the outermost dispatch.  Frame NAMES are always
  distinct, so the navigation still gives useful "I'm in X
  called from Y called from Z" context.
- Per-op source-line annotation in `proc-disasm`: each row now
  reports the source line where the corresponding body element was
  scanned, replacing the v1 placeholder of `0`.  Populated by the
  scanner at proc-finalize time into a host-side
  `std::unordered_map<vm_offset_t, std::vector<int32_t>>` on the
  Trix struct (TRIX_DEBUGGER-gated; compiled out of release
  builds via the new STL carve-out for debugger-only state -- see
  `feedback_no_stl_heap_containers.md`).  Pruned in `Save::restore`
  for entries above the rewind barrier so reused offsets don't
  inherit stale lines.  Works for both plain procs AND `|locals|`
  procs -- the latter's annotation lands on the inner packed-body's
  storage offset (the outer 2-element synthesized body shows lines
  as 0).  Line numbers reflect the post-scanner `m_line_number`,
  which approximates the token's containing line for single-line
  tokens (exact) and reports the END line for multi-line tokens
  (cross-line string literals).
- One-shot breakpoints in the `:` command line: `b1 /name` (and
  `b1 /name when {predicate}`) registers a breakpoint that
  auto-clears the first time it actually halts.  Predicate-
  filtered silent-skips do NOT consume the one-shot -- the bp
  keeps firing until the predicate passes.  The auto-clear
  happens after the predicate filter and before the halt is
  rendered, so by the time the user sees the breakpoint the
  registry entry is already gone (re-arm via `b1 /name`).
  Combines naturally with conditional bps: the condition is
  cleared alongside the bp.  `clear /name` and `clear-all`
  drop the one-shot tag alongside the bp + condition.  `bp`
  listing renders `(once)` after the name.  Implementation is
  pure Trix -- new `_dbg-bp-oneshot` Dict tracking which names
  are tagged; `_dbg-handle-oneshot` does the cleanup at halt
  time.  No new C++ surface.
- Persistent sid -> path cache in the debugger: `stream-name SID`
  now resolves the path of a file stream EVEN AFTER it has been
  closed (typically by `require`).  Populated at file-open time
  into a dynamic-mode Dict in the global VM region; survives any
  user-script save/restore and remains valid for the lifetime of
  the process.  Compiled out in release builds (TRIX_DEBUGGER
  gated).  Enables `proc-disasm` to render meaningful file names
  for procs whose source stream is no longer open.
- `Dict::expand_dict` and `Dict::expand_set` now allocate the
  auxiliary pool block in the same region as the parent (global-VM
  when `is_global(trx)`, local-VM otherwise).  The previous
  unconditional `vm_alloc_ptr` placed pool blocks for global
  Dicts in local VM; after a save/restore the local-VM block
  was reclaimed while the global Dict header survived, leading
  to dangling cross-region pointers and SIGTRAP on the next
  `put`.  Surfaced during the sid-path cache work (initial
  design used a dynamic-mode global Dict; test_streams.trx
  tripped after opening 17 distinct files = initial cap 16 + 1
  grow).  Caveat: `Dict::gc_walk_contents` doesn't yet mark the
  auxiliary pool blocks themselves, so `vm-global-gc` after
  expansion would reclaim them.  Bounded today because
  `vm-global-gc` is a manual op.
- Modal pane expand in the debugger: pressing Enter while a stack
  pane (op / exec / dict / err) is focused switches to a full-
  screen pretty-print of that pane's contents.  Esc returns to
  the normal multi-pane layout.  Up/Down arrows scroll one line
  at a time; PageUp/PageDown move by one screen height.  Each
  item renders as `[I] format-object(item, 200-wide)` so big
  composites get a wider preview than the in-line stack rows
  allow.  The status bar swaps to a modal-specific hint set.
  Modal state resets at every halt event so each cb fire starts
  in the normal layout regardless of how the previous halt was
  left.  Implementation is pure Trix (no new C++); the snapshot
  is taken FIRST inside `dbg-modal-open` before any helper
  literal name lands on the operand stack -- `op-stack-snapshot`
  and the sister ops capture the entire coroutine operand stack
  (from `m_op_base` to `m_op_ptr`), including any working
  values the helper pushed.
- Conditional breakpoints in the `:` command line:
  `b /name when {predicate}`.  The predicate is sandbox-eval'd in
  the user's dict context whenever name resolution would fire the
  bp; falsy results (false / null / no-value / error / blocklisted
  op) cause the debugger to silent-skip without rendering, resuming
  in the user's most recent mode (step / step-over / step-out /
  continue).  Truthy results halt as a normal bp would.  Plain
  `b /name` (no `when` clause) clears any previous condition; the
  bp becomes unconditional again.  `clear /name` and `clear-all`
  drop both the bp and its condition.  `bp` lists every breakpoint
  with its predicate inline: `/foo when {i 10 gt} /bar`.  Predicate
  strings are copied to global VM at store time so they survive
  any user-script save/restore that the cb is invoked inside of.
  Implementation is pure Trix in lib/debugger.trx (no new C++
  surface); reuses the `:p` blocklist + save/restore harness plus
  a Boolean-extracting variant that drops local-VM residue BEFORE
  the restore so only an immediate Boolean crosses the boundary.
- Watch expressions in the `:` command line: `w EXPR` adds a watch
  (max 8), `clear-watch N` drops a single watch by index,
  `clear-watches` empties the list, `watches` prints the list.
  Watch expressions re-evaluate at every halt event (and once at
  add-time so the pane shows a result immediately).  Each eval
  reuses the `:p` sandbox harness -- save/restore-bracketed,
  blocklisted ops rejected pre-eval, result allocated in global VM
  so it survives the wrapping restore.  Side-effects from the
  watch (def / store / put / VM allocations) roll back between
  fires.  The pane sits between the output pane and the stack-
  pane header; height = 1 header + N rows, sizes the source pane
  down by the same amount.  When no watches are set the pane
  disappears entirely.
- `set-stdout stream -- stream` swaps `trx->m_stdout` to a writable
  stream and returns the previous one.  Systemdict `/stdout`
  binding is unchanged -- `stdout` always resolves to the original
  real stream object, so swap-back is `stdout set-stdout pop`.
  Used by the debugger to route user-script output into a capture
  buffer; also useful for tests that need to redirect print output
  to a string-stream.
- `clear-string-stream stream --` resets the write pointer of a
  writable in-memory string-stream so subsequent writes overwrite
  earlier bytes.  Pairs with `make-string-stream` /
  `get-string-stream` for reusable capture buffers across many
  drain cycles without sid churn.
- `format-object` and `concat` now honor `m_curr_alloc_global`
  (the global-alloc flag set by `${...}` blocks and `set-global`).
  Inside `${ ... format-object ... }` or `${ ... concat ... }`,
  the result String is allocated in global VM and survives a
  wrapping `save`/`restore` -- previously the result was always
  local VM and would trip `/invalid-restore` if left on the
  operand stack across a restore.  Used by the debugger's
  sandboxed `:p EXPR` to extract the eval result from inside a
  save scope.
- `command-line-args -- array` -- script-side argv access.
- VM heap tracking (dev-build only, gated by `build_config.inl`):
  per-source-line allocation counters, per-block side-table,
  `vm-heap-snapshot` / `vm-heap-diff` ops.  Compiled out of
  release builds.
- `:status:` keys: `frame-dict-pool-evictions`,
  `save-journal-bytes`, `vm-peak-used`, `vm-peak-temp-used`.
- `:status:` keys `vm-global-num-alloc` and `vm-global-num-free` for
  O(1) introspection of the global VM allocator's live-block and
  free-list counts.  Previously available only via `vm-global-info`'s
  O(N) heap walk.  Useful for leak detection, free-list health checks,
  and regression tests.  Bumps SNAPSHOT 167 → 168 (new
  `gvm_free_block_count` field consumes existing pad in
  `SnapShotHeader`; header size unchanged).
- Build script: `./build.sh --optimized` produces a stripped
  `-O3` release binary.
- Native-kernel pattern: a dedicated user-op binary (`./tetrix`)
  ships C++ hot-path operators alongside `./trix` without
  polluting the language surface.  Documented in
  `docs/reference_native_kernels.md`.
- Journal-write microbenchmarks `benchmark/bench_journal_write.trx`
  (1 M `save / put / restore` cycles) and
  `benchmark/bench_journal_write2.trx` (4 M journal writes inside a
  single save scope).  Used to validate the `is_global()` short-
  circuit added to `Save::save_data` for the Global VM region: ~0.3 %
  overhead vs the same code with the check stubbed out, well within
  run-to-run noise.  Methodology + numbers in
  `benchmark/PF2_RESULTS.md`.  Filed as PF2 in the VM-redux plan;
  closes with "no optimisation needed".

#### Showcases

- `examples/tetrix.trx` -- playable Tetris with AI search modes
  (`--ai-peek`, `--ai-oracle`), pack/unpack save files with
  Fletcher-32 checksums, animated splash + line-clear flashes.
- `examples/raycaster.trx` -- Wolfenstein-3D ASCII dungeon with
  three tile sets (`/ascii`, `/blocks`, `/halfblock`) and
  optional distance-based wall shading.
- `examples/chip8.trx` -- CHIP-8 + Super-CHIP emulator with a
  round-trip mnemonic assembler (`examples/chip8-asm.trx`),
  manual (`examples/chip8-manual.md`), and 10 bundled ROMs.
- `examples/symdiff.trx` -- direct-tagged AST symbolic
  differentiator with rewrite-to-fixpoint and precedence pretty-
  printer.
- `examples/regex.trx` -- naive backtracker vs Thompson NFA + Pike
  VM, with a ReDoS demo showing the pathological vs linear paths.
- `examples/mini-scheme.trx` -- metacircular Scheme interpreter
  with a CEK trampoline rewrite for stack-safety.
- `examples/sudoku.trx` -- logic-programming solver with a
  20-puzzle catalog and an opt-in stress-test file.
- `examples/minidb.trx` -- in-memory SQL-ish database with a
  hand-rolled tokenizer/parser/executor, primary-key indexed
  lookup, and `BEGIN` / `COMMIT` / `ROLLBACK` mapped onto
  `save` / `restore`.
- `examples/markov.trx` -- direct-coroutine bigram text generator.
- `examples/village.trx` -- save-anywhere "fork the timeline"
  via `snap-shot` + `thaw`.
- `examples/supervised-wordcount.trx` -- pipelines + actors +
  supervision composed.
- `examples/reactive-finance.trx` -- reactive cells with batch
  updates, computed cells, watchers, and save/restore what-ifs.

### Changed

#### Release close-out (June)

- Interpreter performance pass: the three dispatch lookup switches
  became consteval-built tables, and the per-op decode path gained
  fast-path splits (scalar `make_clone`, eqref pre-test, inlined
  packed fixup).  Headline dispatch throughput ~45M -> ~47M ops/sec;
  the measurement chain lives in benchmark/ and
  release-audit/phase5/perf-analysis.md.
- Startup failures now exit with their `Error`-enum code instead of
  0, and stdio-disabled modes no longer hang parked on an interrupt
  that can never arrive -- one shared quit floor in `Stream::init`.
- Heap-tracking tables scale with the VM size (minimum VM boots
  again; full-size tables at or above the default VM so exhaustion
  landscapes are unchanged).
- `@pipe-source-feed` removed (dead dispatch slot); the gen-state
  guidance stub removed in favor of the `/query` handler-call arm.

#### Language

- **Unified `$`-suffix family.**  `$` means "global VM" in both
  prefix (`$/foo`, `$\foo`, `${...}`) and suffix (`(str)#$`,
  `[arr]#$`, `<<dict>>#$`, `{{set}}#$`, `{proc}#$`) positions.
  Adds the symmetric inverse `#$$` -- a per-literal force-local
  suffix that overrides an enclosing `${...}` or `set-global true`,
  mirroring `$$/foo` / `$${...}`.  `#=`, `#$`, and `#$$` are
  mutually exclusive in the first slot.  Internally, three
  mutex bool fields collapse into a single `AllocClass` enum
  (Default / LocalVm / GlobalVm / EqTmp); mutex enforced by the
  type.
- **Unified `$` prefix family** -- `$$` consistently means
  "force-local" across name + block forms; promoted (auto-widening)
  infix is `$[ ... ]`.  New forms: `$$/foo` / `$$\foo` (force-local
  literal/executable name) and `$${ body }` (force-local allocation
  scope block).  Useful inside a `${ ... }` block (or under
  `set-global true`) when one specific Name or sub-region should
  stay in local VM.
- **`{proc}#$` and `{proc}#$$` procedure literals.**  Extends the
  `#$` / `#$$` family to procedures: `{ body }#$` allocates the
  proc's packed bytes (default) or Array storage (`#$a`) in global
  VM; `#$$` force-allocates in local VM.  Mutually exclusive with
  `#=` (eqproc scratch).  Full suffix grammar is now
  `#[=|$|$$][a|p][e|l][r|w]`.  Closes a consistency gap left over
  from the Phase 2 container-suffix work (strings, arrays, dicts,
  and sets already had `#$`).
- **Save tokens are inline `Integer` values** -- `save` returns an
  `Integer` packing `level | (gen ^ barrier_low23) << 8`; `restore`
  takes the same `Integer`.  Negative tokens (`-N`) pop `|N|` save
  levels relatively.  Zero is rejected with `/invalid-restore`.
  `Object::Type::Save` and `PackedType::Save` slots both freed.
  Stale-token detection via per-slot generation counter XOR'd with
  the barrier offset's low 23 bits -- gen wrap is genuinely safe.
  Snapshot bumps 143 → 146 across phases.
- **`def` skips frame dicts** -- bare `def` (and `store` fallback,
  `current-dict`, `import`) now walks past `|locals|` frames to
  the first non-frame dict.  `local-def` is the explicit op when
  you want to write into the current frame.  Eliminates the
  "helper without `|...|` leaks bindings into the caller" trap
  (W02) and the "bare `def` inside a `|...|` proc never reaches
  module scope" trap.  Existing call sites in tests/showcases
  migrated; transition-error guidance in the body.
- **`try` scrubs operand-stack residue on failure** -- when a
  proc errors mid-execution, `try` now leaves only the error name
  on top, freeing any ExtValue/WideValue residue from below.
  `try-catch` and `try-result` are unchanged (handlers want the
  residue).  Snapshot bump 138 → 139.
- **`for-all` body must return operand stack to its pre-iter
  depth** -- saved-depth slot per `@*-forall` exec frame; mismatch
  raises `/range-check`.  Catches three families of latent bugs
  (empty body / residue / over-consume).  Applies to array,
  packed, string, dict, set, record `for-all`.
- **`range` is strictly 1-arg** (`range stop -- arr`); the 2-arg
  form is now `range-from start stop -- arr`.  Closes Wart 01
  ("range silently grabs an integer off the stack").
- "locals dict" renamed to **"frame dict"** across docs, source
  comments, and tests.  The frame-dict allocator is decoupled from
  save level, transparent to save/restore (above-barrier frame
  dicts get compacted out instead of erroring), and pooled by
  capacity bucket.  Adds an `IsFrame` bit to the Dict header.
- Default coroutine dict-stack depth bumped from **8 to 24** --
  systemdict + protocoldict + userdict eat 3 slots; nested helper
  procs were tripping `/dictstack-overflow` inside actors.

#### Concurrency

- Per-coroutine name binding cache replaces the previous global
  cache.  Cross-actor `=string` rebinds no longer race; fast path
  is ~2 cycles when only one coroutine is active.
- `supervisor_op` spawns children synchronously inside the
  operator (rather than via deferred messages).  Eliminates the
  "child not registered yet" race at startup.
- `restore` rebuilds scheduler lists from surviving coroutines --
  previously `restore` could amputate sleeping coroutines below
  the barrier.

#### Build / packaging

- Renamed `doit.sh` → `build.sh`; CMake wired in lockstep.
- Renamed `doc/` → `docs/` for GitHub Pages support.
- Added `NOTICE.md`, `CITATION.cff`, `CONTRIBUTING.md`, GitHub
  Actions CI (build, README example verification, CodeQL,
  clang-tidy), release workflow with prebuilt-binary install path.
- `begin-locals` opcode encoding: the scanner now emits two
  integers before the body (`K` = declared name count, `N` = dict
  capacity) instead of one; when `|names|` is used without the
  `#N` suffix, `N == K` so the runtime semantics are unchanged.
- Const-correctness sweep: read-only leaf-pointer parameters take
  `const T *` and pure member functions that never mutate `*this` are
  marked `const` (const locals were already swept and enforced in CI via
  `misc-const-correctness`).  Handle-over-VM mutators that change VM-resident
  state through a `Trix*` while leaving the descriptor untouched stay
  non-`const` by design; STYLE.md documents the carve-out.  No behavioural
  change.
- Snapshot format version bumped from 121 to **146** across this
  release.  Old snapshots will not thaw cleanly.

### Removed

- `is-save` user operator -- save tokens are inline `Integer`s, so
  `is-integer` already covers the introspection.
- `Object::Type::Save` and `PackedType::Save` slots -- both freed
  by the save-token migration; reclaimable for future types.

### Fixed

#### Release close-out (June)

- Scheduler: an early timer wake could fall through and reload the
  main coroutine while it was still timer-linked, self-cycling the
  timer list into an infinite walk (the "dead keyboard" 99% spin);
  timer wakes also leaked the stdin blocked-reader slot, killing the
  next input actor with `/invalid-access`.
- Scheduler: the timer sweep only ran when the ready queue was empty,
  so a hot coroutine pair starved every sleeper; an O(1) head-peek
  sweep now runs on every scheduler entry.
- Restore-time mailbox scrub deleted (a fossil from before global-VM
  contexts): it wiped every parked actor's registration on each
  restore, losing wakeups.
- Coroutine death now scrubs pipe/sender/joiner registrations
  (orphan-rule validation found the gap; `scheduler_validate` runs
  under `vm-validate`).
- Debugger `break-on-error` is actually wired: the hook fires and the
  breakpoint halts inside the erroring call.
- Scanner: unsigned radix>=16 literals near the 64-bit boundary were
  silently truncated by a masked-wrap heuristic; an exact u128
  accumulator now rejects precisely.
- Screen: codepoint output used CESU-8 for astral-plane characters;
  now real UTF-8.
- `pow-mod`: ExtValue freed before a raise could double-free under
  `@try-rollback`; `address-for` had a UB shift and offset wrap;
  `Quit` below the save floor could hang; scanner error objects are
  planted, not leaked.

- `Dict::gc_walk_contents` now marks the auxiliary expansion-pool
  blocks of dynamic global Dicts/Sets that have outgrown their
  initial pool.  For each entry whose offset lies outside the
  Dict's own block payload (bucket-chain head OR `m_pool` free-list
  entry), the walker calls `gvm_find_owning_payload` to back-walk
  to the owning expansion block and `mark_global_offset()` on the
  result.  Entries inside the Dict's own block are skipped via a
  payload-range check (already covered by the Dict's mark).  The
  mark-gen short-circuit ensures each expansion block is back-walked
  at most once per GC pass.  The free-list walk handles the all-free-
  expansion-block edge case: after a put-then-undef cycle drains
  an expansion block back to `m_pool` with no bucket-chain reference,
  the free-list traversal still discovers the block.  Pre-fix: a
  `vm-global-gc` after expansion reclaimed the block as an orphan,
  leaving the Dict's `m_pool` and bucket-chain heads dangling into
  freed memory; the next `put` SIGTRAPped or silently corrupted.
  Bounded today because `vm-global-gc` is a manual op; now fixed
  structurally.  Regression guard: `tests/test_global_dict_gc_expand.trx`.
- Pre-existing `-Werror=unused-variable` in `src/scanner.inl:2271`
  on release / `--optimized` builds.  `auto committed = ...` from
  the per-op source-line annotation work (commit `adfc757`) was
  consulted only inside `TRIX_DBG(...)`; with `TRIX_DEBUGGER` off
  the variable was unused.  Tagged `[[maybe_unused]]`.  Surfaced
  during the GC-walker work above when running `./build.sh --optimized`.
- **unify / unify-match: theoretical partial-bind-then-restore UAF
  closed**.  The 2026-05-04 audit theorized a use-after-free path
  through `lvar_bind`'s shallow-share semantics during a failed
  per-attempt save/restore in `unify_match_op`: the restore handler
  freed the lvar's binding slot, but the pattern array still
  referenced the same `m_offset`, leaving a dangling read on the
  next iteration.  Option B fixes this by making `lvar_bind`
  context-aware: top-level binds (consumable source from the op
  stack) still transfer via `make_copy`, while recursive sub-binds
  and deref-from-bound binds now go through `make_clone`, allocating
  a fresh ExtValue so the binding owns independent storage.
  `UnifyResult` is now `{matched, a_transferred, b_transferred}` so
  `unify_op` and `unify_match_op` free each top-level operand
  independently based on whether ownership moved.  Real-workload
  A/B (mini-scheme, genealogy, heist) regression under 1.5% on the
  optimized binary; per-leaf ExtValue clone cost is paid only on
  scalar Long/Double/Address/ULong binds done through deep
  recursion.  Plan: memory/plan_lvar_bind_context_aware.md.
- `benchmark/release.sh` link line was missing `-lz` -- broken since
  the native `deflate` / `inflate` ops landed.  Release-build path
  now links zlib correctly.
- AI search leak in tetrix's `--ai-peek` (3531 → 5 B/call) -- the
  best-placement scratch field was missing a save/restore wrap.
- Frame-dict pool: oversized `|locals|#N` dicts no longer escape
  recycling; pooled via an overflow chain.
- `save` free-list reconstruction on `restore` -- ExtValue free
  slots that survived the rollback are correctly re-linked.
- Mini-scheme: stack-safe via CEK trampoline rewrite (was hitting
  exec-stack overflow at modest depths).
- Closure tail-call frame preservation for non-`|locals|` tail
  callees.
- `capture_op` op-stack depth clamping when the body consumed
  below the snapshot.
- Silent operand-stack corruption when a perform-captured K rode an
  inner `@delimit-barrier` or `@handler-scope` and the resumed body
  did a subsequent `capture`.  The inner barrier's op-depth stayed
  at its capturer-time absolute value; on resume in a frame with
  operands below the K-invocation site, the second capture trimmed
  the live op stack to that stale floor and silently ate below-
  frame items.  Fix: `ContinuationContext` gains an
  `m_op_rewrite_count` field plus a trailing array of err-segment
  offsets; at restore time `execute_continuation` overwrites every
  recorded slot with the resumer's segment-start floor before the
  err `memcpy`.  Bumps SNAPSHOT 168 → 169.
- Same silent-corruption class on the exec side: a `try { ... perform
  ... }` inside `handle-effect` rides `@try-rollback` along in the
  captured exec segment.  `@try-rollback`'s saved-depth Integer is
  pushed on exec one slot below the barrier (`ops_flow.inl:try_op`);
  on K resume + error inside the try, the rollback path trimmed the
  live op stack to the stale capturer-frame depth and silently ate
  below-frame items.  Fix: a parallel `m_exec_rewrite_count` table
  records captured-exec offsets to saved-depth slots, rewritten to
  the resumer's segment-start floor before the exec `memcpy`.  Closes
  the matching half of the rewrite-table mechanism.  Bumps SNAPSHOT
  169 → 170.
- `try-result` (`ops_higher.inl:try_result_op`) had the same silent-
  corruption shape via its `@try-result-complete` barrier — same
  `[saved-depth][barrier][@try-barrier][proc]` exec layout as `try`.
  Extended the exec-rewrite table to recognise both barriers.  Used
  by `examples/mini-scheme.trx`; no snapshot bump (rewrite-table
  contents change, struct layout doesn't).
- Logic-programming barriers `@naf-barrier`, `@choice-count-barrier`,
  `@for-each-solution-barrier`, `@aggregate-barrier`,
  `@aggregate-reduce-barrier` (`ops_logic.inl`) had the same shape
  with `[op_depth][save_obj][<barrier>]` on exec.  Probed via `naf`
  with a non-empty capturer-frame (op_depth captured ≥ 0); the trim
  on barrier-fire after K resume silently ate below-frame operands.
  Extended the exec-rewrite table to recognise all five logic
  barriers (op_depth slot at scan-2, vs scan-1 for the try family).
  No snapshot bump.
- `@naf-barrier` was reading its saved_op_depth via
  `static_cast<length_t>(integer_value())` — an unsound cast: a
  negative depth (m_op_ptr below m_op_base when the op stack was
  empty at naf entry, conventionally -1) wrapped to 65535, the trim
  target overflowed above m_op_ptr, and the trim silently no-op'd.
  The other four logic barriers in the family already used
  `integer_t` directly; `@naf-barrier` was the outlier.  Fixed to
  match.  Latent before the rewrite-table work above; that fix would
  have left the cast benign in normal use (fresh_op_depth could land
  at -1 on a resume into an empty resumer frame and the wrap would
  have masked the rewrite).  Both changes ship together.
- For-all family iteration barriers `@array-for-all`,
  `@packed-for-all`, `@string-for-all`, `@dict-for-all`,
  `@set-for-all`, `@record-for-all` (`ops_array_iteration.inl`,
  `ops_set.inl`, `ops_record.inl`, plus pushers in `ops_flow.inl`,
  `ops_stream_io.inl`, `runtime.inl`) share the
  `[proc][container][saved-depth][<barrier>]` exec layout.  Each
  one's W07 stack-effect check reads saved_depth and compares
  against current_depth on every iteration; a captured saved_depth
  inside a perform K becomes stale on resume, raising `/range-check`
  on the wrong frame.  Failure mode is visible (not silent) but
  still wrong.  Extended the exec-rewrite walker to recognise the
  six for-all variants via a new `operator_is_forall_family()`
  predicate; same scan-1 offset as the try family.  No snapshot
  bump.
- `make_control_operator`: missing `atReadKeyRetry` /
  `atReadKeyTimeoutRetry` cases.
- Three `VerifyIntegers` callsites that read with bare
  `integer_value()` after multi-type verify.
- `close-stream` is now idempotent on already-closed streams.
- Stale-value puts into `eqarray` rejected at the write site
  rather than producing a silent dangling reference.
- `stty sane` does not recover alt-screen / hidden cursor; raw-
  mode signal handler now emits the matching escape sequences on
  SIGINT/SIGTERM/SIGHUP/SIGQUIT before re-raising.

## [0.5.0] - 2026-04-22

First tagged public release.  The language and runtime are
feature-complete; all concurrency layers (coroutines, pipelines,
actors, supervision) and computation features (logic, reactive
cells, lazy sequences) are implemented and tested.

### Added

- Embeddable stack-based VM (single-header C++23 library) with 700
  operators, 28 types in an 8-byte tagged-union `Object`
  representation, and a deterministic fixed-size VM heap.
- **Cooperative concurrency:** coroutines with `sleep`/`yield`/`join`;
  bounded-buffer pipelines with automatic backpressure; mailboxed
  actors with selective receive; Erlang/OTP-style supervision trees
  with `one-for-one`, `one-for-all`, and `rest-for-one` restart
  strategies.
- **Computation layers:** Prolog-style unification with choice
  points and cut; reactive cells with automatic dependency tracking
  and incremental recomputation; lazy sequences over infinite
  streams with `lazy-map`, `lazy-filter`, `lazy-take`, etc.
- **Durability:** transactional `save`/`restore` and whole-VM
  `snap-shot`/`thaw` serialization.
- **Composability:** tagged values (ADTs), records, modules with
  `require`/`use`/`import`, protocols for open type dispatch,
  transducers, curry/thunk, contracts.
- libFuzzer harness covering the full interpreter, with a seed
  corpus derived from the language reference.
- 13,800+ test assertions across 144 test files.
- GitHub Actions CI: build matrix of `{gcc-15, clang-18} x
  {Debug, Release}` on Ubuntu 24.04, plus a `clang-format --Werror`
  check on every PR.
- Prebuilt Linux x86_64 binary attached to tagged releases.
- Snapshot format version 116 (stored in the image header and
  strict-compared on thaw).

[Unreleased]: https://github.com/mcguidarelli/trix/compare/v0.9.0...HEAD
[0.9.0]: https://github.com/mcguidarelli/trix/compare/v0.5.0...v0.9.0
[0.5.0]: https://github.com/mcguidarelli/trix/releases/tag/v0.5.0
