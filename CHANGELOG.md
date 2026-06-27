# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **`examples/amazing.trx`: masking -- carve mazes into words, the Trix logo, any
  SVG, and shapes.** `--mask disc|ring|frame|logo`, `--mask-text WORD`, and
  `--mask-file NAME` restrict the square-grid maze to an arbitrary in-mask cell set;
  disconnected shapes (separate letters) become one perfect maze per connected
  component, and `--mask-invert` (with `--mask-margin`) punches the shape out of a
  full maze instead. Text renders through a selectable `--font` (`--font-dir` to
  locate the data): a built-in 5x7 block font, hi-res **Roboto** bitmap atlases
  (Apache-2.0), and **Hershey** stroke fonts (public domain) rendered in pure Trix
  by stroking centerlines. Fonts are generated from real faces by host tools
  (`tools/gen_mask_font.py` / `gen_hershey_font.py`); only the derived glyph data is
  committed (`examples/mask-fonts/`). `--mask logo` carves the **real Trix logo**,
  cut out of a maze, and `--mask-file` carves any other SVG -- both via
  `tools/gen_mask_svg.py`, which rasterises an SVG to a 1-bit mask
  (`examples/mask-shapes/`, `--mask-dir` to locate it); only the logo's derived mask
  is committed. Masking composes with any `--algo` and `--color`.
- **`examples/amazing.trx`: `--wall-color` / `--bg-color`.** The maze line color and
  the passage/background color are now configurable as `RRGGBB` hex (default black
  lines on a white background), honored across every grid topology and both mono and
  distance-heatmap renders.
- **`globaldict` -- a second user dictionary implementing a PostScript-style
  local/global definition split.** A fixed-capacity dictionary pre-allocated in local
  VM and placed on the dict stack directly below `localdict`, pushed by the new
  `globaldict` operator. `--globaldict-size=N` sets its capacity (default 64; range
  16..50000), `:status:globaldict-length` / `:status:globaldict-maxlength` report it,
  and the `:globaldict:` name path resolves into it. `def` / `override` /
  `def-persist` / `store` route by **sticky home**: a base-dict name keeps the
  dictionary it was first defined in and is updated there regardless of allocation
  mode (so `set-global; /existing def` updates the existing binding in place); only a
  genuinely-new name is placed by `set-global` -- globaldict when active, else
  localdict. A global definition made above save level 0 persists across `restore`,
  and globaldict accepts only global values there. A name present in BOTH base
  dictionaries -- reachable only by a direct `put` that bypassed routing -- raises the
  new `/dict-conflict` error (exit code 61). The permanent dict-stack count rises from
  3 to 4, and the snapshot format is bumped to **v183** (new `globaldict_offset`
  header field and the `GlobalDict` name ordinal).
- **`vm-gc-profile` / `vm-gc-profile-report` -- per-section global-GC timing
  (debug builds only).** A `TRIX_DEBUGGER`-gated stopwatch that attributes
  stop-the-world GC time across the root-walk sections -- stacks, coroutines,
  global names, object tables, named dictionaries, and the isolated `localdict`
  scan. `vm-gc-profile` toggles collection; `vm-gc-profile-report` prints
  cumulative nanoseconds and pass counts per section. Both compile out under
  `-DNDEBUG`, so the user-facing operator count is unchanged; they exist to
  measure the root-walk fast paths below.

### Changed
- **BREAKING: the user dictionary `userdict` is renamed `localdict`.** The operator
  `userdict`, the name-path prefix `:userdict:`, the status variable
  `:status:userdict-maxlength`, and the CLI flag `--userdict-size` become
  `localdict`, `:localdict:`, `:status:localdict-maxlength`, and `--localdict-size`
  respectively. This is groundwork for a PostScript-style split in which a
  forthcoming `globaldict` (global VM) sits alongside `localdict` (local VM); the
  new name makes the local-VM role explicit. No snapshot-format change — the
  snapshot's user-dict offset field was renamed in place.

### Performance
- **Global-GC root-walk fast paths.** The stop-the-world global mark-sweep's
  root walk gained four short-circuits that cut its fixed per-pass cost: a
  maintained live-block counter (`m_gvm_user_block_count`) replaces a full
  count-walk; leaf and no-op object kinds skip the mark work-queue entirely
  (`gc_kind_has_no_children`); the global-`Name` root walk is skipped wholesale
  when no global names exist; and a per-bucket global-name mask
  (`m_name_global_mask`) restricts that walk to the buckets that actually hold a
  global binding. The mask rides the snapshot, bumping the format to **v182**.
- **The global mark-sweep now skips the local user dictionary (`localdict`).**
  `localdict` is a program's largest mutable GC root -- every plain `def` lands
  there -- yet code that keeps its globals in `globaldict` stores no reference
  into global VM through it at all. A write-barrier flag,
  `m_localdict_maybe_global`, is set whenever a global-VM value is stored into a
  local container; while it is clear the collector skips the entire `localdict`
  subtree, including its descent during the dict-stack walk. A `TRIX_DEBUGGER`
  oracle marks `localdict` anyway on every clear-flag pass and asserts it reached
  zero global blocks, so a barrier gap is a test failure rather than silent
  use-after-free; global `Name` references are excluded from the barrier (they
  are section-3 roots, not reached through `localdict`). The flag is part of the
  snapshot, bumping the format to **v184**. For the bundled Z-machine interpreter
  -- whose 311-proc `localdict` walk dominated GC -- routing its global-owning
  `z-run` definition into `globaldict` (a `true set-global ... false set-global`
  wrapper) cuts the measured per-pass GC cost roughly **240x** (~392 us to
  ~1.6 us).
- **Precise re-skip via store-time deep scans.** The skip flag now clears as
  soon as `localdict` provably reaches no global block again -- even while other
  globals remain live -- instead of only when the global heap empties entirely.
  Soundness is preserved by an iterative, allocation-free closure walk
  (`value_reaches_global`, traversing a pre-allocated local-VM path stack -- no
  recursion, no heap container) run at the barrier when a local composite is
  stored: a value that buries a global re-arms the flag immediately, closing the
  hole in which a later `def` could otherwise hide a global behind an
  already-clear flag. The path-stack workspace offset is added to the snapshot
  header, bumping the format to **v185** (the current snapshot format).

### Fixed
- **`print-fmt` / `sprint-fmt`: the local-zone instant spec (`:Il…`) now
  supports the `%Z` and `%z` conversions.** A template such as
  `{0:Il%H:%M:%S %Z}` previously raised `/invalid-format-string` ("format
  argument does not contain the information required by the chrono-specs"):
  the local path formatted a bare `std::chrono::local_time`, which carries no
  zone, so libstdc++ rejected `%Z` / `%z`. It now formats the `zoned_time`
  directly, so `%Z` prints the zone abbreviation (`EST` / `EDT`) and `%z` the
  offset (`-0500`); every other conversion is byte-identical. Surfaced by
  `examples/log-timestamp.trx`.

## [0.11.0] - 2026-06-24

### Added
- **`string-from-bytes` operator + byte arrays accepted by output sinks.** New
  `string-from-bytes` (`array -- string`) builds a string from an array whose
  elements are all bytes — the runtime inverse of `chars` and the `(...)#a`
  literal — raising `/type-check` on the first non-byte element. Its motivation is
  `save`/`restore` journaling: an array of `Byte` is fully journaled (element
  writes roll back on `restore`) whereas string byte writes persist by design, so
  a byte array is the representation of choice for undoable text. To render such
  text without an explicit conversion, the output sinks `print`, `write-string`,
  `screen-put-string`, and `screen-put-utf8-string` now accept a byte array in
  place of a string (coerced internally; the array left on the stack is
  unchanged). Operator count is now 839. The snapshot format is bumped to **v181**
  (the new operator shifts the SystemName ordinals that operators persist in a
  snapshot).
- **Scan-time stack-effect checking.** A procedure may declare its stack effect by
  extending the `|...|` preamble with a `-- outputs` tail —
  `{ |price qty -- total| price qty mul }` is `( 2 -- 1 )`. At scan time (zero
  run-time cost) the body is abstractly interpreted and verified to leave the
  declared number of values and consume no more than its declared inputs; a
  mismatch raises the new `/stack-effect` error (exit 60) before the program runs.
  The check is best-effort: it reports only provable violations and silently
  accepts anything it cannot fully analyze (variadic operators, dynamic lookup,
  procs not yet defined), understanding straight-line bodies plus the
  `if` / `if-else` / `repeat` combinators, and tracking `local-def` / `store`
  frame locals. It is inter-procedural: a call to an already-defined procedure has
  that procedure's own (inferred or declared) effect applied in place, so a checked
  proc is verified through the procs it calls, with effects read from the bindings
  live at scan time. It is sound for first-order code (parameters and locals
  holding data values); because a bare reference to a frame binding that holds a
  procedure auto-executes it, a higher-order procedure that bare-references a
  proc-valued parameter should be left un-annotated (see the best-practices note in
  `docs/trix-reference.md` § 3.15). A bare `|...|` with no `--` is unchecked (opt-in
  per procedure); `--no-stack-check` disables the gate process-wide. The arity table
  (`src/op_effects.inl`) is generated from `dispatch.inl` + the reference docs by
  `tools/gen_op_effects.py` and pinned by its `--check` CI gate.
- **`-e` / `--eval EXPR` runs inline source.** Executes `EXPR` as a Trix program
  instead of reading a file (the `perl -e` / `python -c` equivalent). No filename
  is consumed: tokens after `EXPR` become the script's args (`command-line-args`).
  May be given once; mutually exclusive with a filename, `--stdin`, and
  `-l`/`--image`. Combined with `-i` it runs the source then drops into the REPL.
- **`-c` / `--check` validates without executing.** Scans a script file, `--stdin`,
  or `-e` source for lexical/structural errors (unbalanced `{}`/`[]`, unterminated
  strings, malformed numbers) and exits `0` if clean or with the scanner's error
  code otherwise — the lexical half of `perl -c`. Nothing runs, so it does not
  recurse into `require`d files. Cannot combine with `-l`, `-i`, or `--resident`.
- **`--timeout=MS` wall-clock deadline.** Raises the new `/time-limit` error
  (exit 59) once `MS` milliseconds of wall-clock elapse during a run — a real-time
  companion to `--max-ops` (which bounds op count). Like `--max-ops`, it only fires
  while ops execute, not while parked/blocked (use `--sleep-budget` for parks).
- **Named declared locals in the `|...|` preamble.** A `/`-prefixed name is a
  declared frame local — `{ |a b /t /acc| ... }` takes `a`, `b` as parameters
  (popped) and declares `t`, `acc` as locals. Declared locals are *not* bound at
  entry: they read `/undefined` until assigned with `local-def` (or `store`),
  exactly like a `local-def` working variable, but being named at scan time they
  are visible to `#e` early binding and tooling. The named-scratch form
  `{ | /t /acc| ... }` (zero params) is also allowed. Capacity (`#N` / `#+N`)
  now counts against `P + M` (params + declared locals).
- **Frame-local slot-indexing.** A locals proc's references to its own frame
  locals — parameters *and* declared `/locals` — where they appear directly in the
  proc's top-level body, are compiled at scan time into direct frame-slot
  references, resolved by positional frame indexing (`O(1)`, no hash, no
  dictionary-stack walk) at run time. Always on (no suffix). This is the same
  hot-loop speedup as `#e` for reading a frame local, without `#e`'s binding-cache
  sensitivity to recursion / `save`, and makes an own-frame local reference
  inherently immune to the `#e` operator-shadow hazard (the frame local always
  wins). A slot-ref read of a declared-but-unassigned `/local` raises `/undefined`
  (it is pinned to the slot, not falling through to an enclosing binding; a dynamic
  name lookup of it still falls through). Depth-0 only: a frame local referenced
  inside a nested proc keeps a dynamic name (Trix frame scoping is dynamic). Frame
  locals remain reachable by name (`/p load`, reflection). Tail Call Optimization
  is preserved for a frame-local-bound proc invoked in tail position.
- **`--seed N` for reproducible RNG runs.** Seeds the PCG32 generator from a
  fixed value instead of `/dev/urandom`, so a run's random-dependent behavior --
  and any snapshot it writes -- is repeatable. Combined with the snapshot image
  normalization below, two runs with the same `--seed` produce byte-identical
  `.img` files.

### Changed
- **`examples/amazing.trx`: first-order helper procedures now declare stack effects.** 32 cell/grid accessors, index-math, and pure-transform procs carry a `|… -- …|` effect and are scan-time verified (output byte-identical); the iterative algorithm and PNG-encoding procs stay unannotated.
- **`#e` early binding no longer freezes a frame-local name that shadows a
  built-in operator.** A `|...|` parameter or declared local whose name collides
  with an operator (e.g. `sum`, `count`, `max`) used to be frozen to the
  *operator* under `#e` (the frame local does not exist at scan time), diverging
  from the late-bound proc. The early binder now skips frame-local names — the
  proc's own and every lexically enclosing locals proc's — at all nesting depths.
  (A name installed only via `local-def` / `bind-locals`, not declared in the
  preamble, remains invisible at scan time and is still frozen; declare it in the
  preamble to make it `#e`-safe.)
- **BREAKING (syntax):** a duplicate name in a `|...|` preamble is now a
  `/syntax-error`. This includes `|a a|` (two same-named params), which was
  previously accepted silently (last write won). Param/local and local/local
  duplicates are likewise rejected, and all parameters must precede any `/local`.

### Internal
- Code style: enforce the house rule of a blank line between every adjacent
  function definition (including single-line accessors) across the source.
  Whitespace-only -- no behavior change.
- README metrics table refreshed: source ~87,500 lines C++23 / 69 `.inl` files;
  20,700+ test assertions across 263 test files (operators unchanged at 838).
- Pass `Object` by value instead of `const Object *` for single-object,
  read-only parameters -- `Object` is a POD 8-byte handle (`sizeof(Object) == 8`,
  no owning members), so a by-value copy is the same size as a pointer with no
  indirection or spurious nullability. Applied across ~55 functions and their
  call sites; `Dict::get`'s redundant `const Object *` overload is folded into
  the existing by-value `get(Object)`. Parameters whose address is taken
  (save/restore journaling), or that are walked as an array or a range, keep
  their pointer by design. Behavior-preserving -- no snapshot-format change.
- Return named POD structs instead of `std::pair` from 11 functions whose pair
  elements are same-typed (a silent field-order footgun) or carry a non-obvious
  (flag, payload) meaning -- e.g. the scanner's `ScanToken`, `is_type_name` /
  `is_error_name`, `Name::find`, and `Object`'s `(valid, value)` integer
  accessors. The structs follow the existing `PackedEncoding` aggregate idiom
  (returned positionally as `TypeName{...}`); distinct-typed, locally-
  destructured pairs (allocator ptr+offset, `scan_proc_suffix`, etc.) keep
  `std::pair`. Behavior-preserving.
- Replace the three designated-initializer `ScreenCell` aggregates (the only
  designated-init in the tree) with positional init per house style, and hoist
  the duplicated render sentinel into a named `SentinelScreenCell` constant.
  Behavior-preserving.
- Table-drive `verify_description`: the 14-arm if/else composite-mask chain
  becomes a `verify_composites` lookup table, and the bit-emit loop a range-for
  over `verify_sv`. Behavior-preserving.
- Rename `ChildEntry::padding` to `restart_marked`: the field documented as
  unused padding actually carries the transient OneForAll/RestForOne per-wave
  "terminated" marker. Still `uint32_t`; 32-byte layout unchanged; stale comments
  corrected.
- Table-drive screen-render SGR attribute emission: the seven copy-paste
  attribute-bit blocks in the render diff become a `{mask, code}` table walked in
  one loop, removing per-bit transcription risk (the codes are non-contiguous --
  reverse=7, strike=9). Behavior-preserving.
- Table-drive transducer step dispatch: the 7-way if/equal cascade in
  `xf_push_steps_for_target` (each repeating the same Array/Lazy/Pipe target
  if/else) becomes an `xf_step_dispatch` table keyed by step tag; pipe-unsupported
  errors reuse the tag's own name, so messages are byte-identical.
  Behavior-preserving.
- Unify the numeric-cast switches behind `cast_object()`: `promote_convert`,
  `cast_op`, and `coerce_element` each repeated the same per-`Object::Type` switch
  building `make_<type>(cast_to_type<T>(...))`. Factor it into `cast_object()` plus
  an `is_numeric_or_boolean_type()` predicate (also replacing `coerce_op`'s inline
  10-term target check); `cast_op` gates on the predicate so its "cannot cast to
  non-numeric type" error is preserved exactly. Behavior-preserving.
- Single-source the snapshot stream-block serialization: the memory-stream,
  startup-tail, and user-file-stream block fields were emitted three times in the
  same order (section CRC, overall CRC, write pass) -- a silent format-divergence
  hazard if a field were added to one pass but not another. Factor the field order
  into two walkers (`walk_memory_blocks` / `walk_user_file_blocks`) driven by
  per-pass callbacks, so the on-disk order lives in one place. Byte-identical
  output (no `SNAPSHOT_VERSION` bump; all 75 snapshot tests incl. the adversarial
  format-drift calibration pass).
- Snapshot images are now byte-reproducible across runs: the ASLR-varying
  absolute addresses that thaw discards and re-derives are normalized out of the
  image. The diagnostic `vm_base_addr` header field is written as 0; the
  `m_vm_temp_save` per-save-level watermark array and every inuse stream's raw
  `m_ext_base`/`m_ext_ptr` pair (non-null for a partially-read memory stream, or
  the startup-file tail) are zeroed in both the CRC pass and the write pass via a
  single ascending-offset region walk. All of these are don't-care values --
  thaw CRC-checks them and then re-derives them -- so the on-disk layout is
  unchanged: no `SNAPSHOT_VERSION` bump, and the adversarial exact-offset
  calibration suite still passes.
- Route the four pipe-put / pipe-get block-and-reschedule paths
  (`ops_pipeline.inl`) through the existing `coroutine_sleep_and_schedule`
  helper instead of hand-rolling the flush -> `Sleeping` + `FlagBlocked` +
  wake=never -> schedule dance at each site -- the same helper already used by
  coroutine join / await / wait-all. Behavior-preserving (-27 lines).
- Single-source the freshly-spawned `CoroutineContext` field initialization: the
  ~27-field bookkeeping block (status/flags/scheduler metadata) that was
  duplicated verbatim in `coroutine_launch_common` (`ops_coroutine.inl`, used by
  coroutine-launch and actor-spawn) and `pipe_alloc_stage_context`
  (`ops_pipeline.inl`) now lives in one helper, `coroutine_init_spawned_fields`.
  Each spawn site's per-site logic (stack-block partitioning, registry link,
  GC-rooting strategy, scanner stream) stays put; the main coroutine (#0) keeps
  its own init in `init.inl` since it carries genuinely different values
  (Running/BASE/id 0/unlimited quantum) and is never recycled. The helper also
  zeroes `m_last_mailbox_capacity` on the launch path (previously only the
  pipeline path did) -- a provably-dead store, since that field is read only for
  `FlagWasActor` contexts, which always recapture it at mailbox recycle.
  Behavior-preserving (-14 lines).
- Hoist the triplicated "wake the head blocked sender after freeing a mailbox
  slot" block (`ops_actor.inl`, the recv / recv-match / recv-match-timeout
  paths) into one `mailbox_wake_head_sender` helper, counterpart to the existing
  `mailbox_append_blocked_sender`. A future fix to the wake protocol now touches
  one site, not three. Behavior-preserving.
- Hoist the modular-exponentiation loop (binary exponentiation over `__uint128_t`
  intermediates) shared by `prime?`'s Miller-Rabin witness test and `pow-mod`
  into one `mod_pow(base, exp, mod)` helper. Behavior-preserving.
- Collapse the 15 `instant-FIELD` / `instant-FIELD-local` accessor ops
  (`ops_chrono.inl`) -- whose bodies only differed by UTC-vs-local and which
  `CalendarParts` field -- onto two shared helpers (`instant_int_accessor` taking
  a `CalendarParts` member pointer, `instant_weekday_accessor`). The 15 named
  `_op` functions and their per-op docs stay as one-line dispatch wrappers, so
  the operator table is unchanged. Behavior-preserving.
- Add `Dict::set_for_each` / `Dict::set_all_of` adapters (set counterparts to
  `for_each`, the latter short-circuiting like `std::ranges::all_of`) and route
  the nine hand-rolled `SetEntry` cursors in `ops_set.inl` through them:
  set-union / set-intersection / set-difference / symmetric-difference / members
  via `set_for_each`, and `subset?` / `disjoint?` via `set_all_of` (preserving
  their early-out). The re-entrant `@set-map` / `@set-filter` / `@set-for-all`
  scheduler trampolines keep `set_next`, since their cursor state lives on the
  exec stack across ticks. Behavior-preserving.

## [0.10.1] - 2026-06-21

### Fixed
- Build portability: include `<limits.h>` for `PATH_MAX` so the header compiles
  on toolchains that no longer pull it in transitively (e.g. Fedora 44 /
  GCC 16.1). Thanks to Gene Hightower ([#11](https://github.com/mcguidarelli/trix/pull/11)).

## [0.10.0] - 2026-06-21

### Added
- **DWARF host introspection** — a new operator family for reading DWARF debug
  information from a host binary at runtime: `dwarf-open`, `dwarf-munmap`,
  `dwarf-read-die`, `dwarf-line-lookup`, `module-load-bias`,
  `module-load-bias-for`, `leb128-decode`, and `peek-bytes` (8 C++ operators),
  plus a higher-level Trix reader (`lib/dwarf.trx`) and a manual (`docs/dwarf.md`).
  DIE walking is lazy/paged; PC → file:line resolves through `.debug_line`.
- New operator **`override`** (`/name any --`): the explicit, sanctioned way to
  shadow a built-in operator. It binds exactly like `def` (into the first
  non-frame dict, past `|...|` frames) but *requires* the name to be a built-in
  operator — otherwise it raises `/undefined` (use `def` for a non-operator
  name). `def` and `override` thus partition every bindable name: exactly one
  accepts any given name.

### Changed
- **BREAKING:** a global `def` / `def-persist` whose key names a built-in
  operator now raises `/invalid-name` instead of silently shadowing it. Silent
  operator-shadowing was a foot-gun — late binding resolved to the user value
  while an early-bound (`#e`) or already-cached reference still reached the
  operator (a silent split). Shadow a built-in deliberately with the new
  `override` operator. Frame-local binders (`local-def`, `bind-locals`, and
  `|...|` locals preambles) are unaffected, since operator names overlap heavily
  with good local-variable names (`sum`, `count`, `max`). This is exactly the
  sort of language-level break that keeps Trix pre-1.0.
  - Interrupt handlers, installed by redefining `l0-interrupt` / `l1-interrupt`
    / `l2-interrupt` in `userdict`, now require `override`.
  - `lib/ansi.trx`: the SGR reverse-video helper and its `with-attrs` attribute
    key were renamed `reverse` → `inverse`; the old name shadowed the
    array/string `reverse` operator for every program that loaded the library.
- Snapshot image format version bumped to **178** — the `override` operator
  shifts the `SystemName` enum, so pre-v178 snapshots are rejected.
- Operator count is now **838** user-facing (**996** total), up from 837.

### Fixed
- Binding-cache coherence: `def`, `override`, `def-persist` (at save level 0),
  `import`, and `bind-into-dict` write a non-topmost dictionary, but were
  repointing the per-name fast-path binding cache at that lower-priority entry —
  so a frameless helper's `def` of a name could hijack a caller's `|...|`
  frame-local of the same name (and an off-stack `bind-into-dict` could corrupt
  the cache). They now clear the cache instead, so a bare-name lookup always
  reflects true dict-stack precedence (frame → userdict → systemdict).

## [0.9.0] - 2026-06-15

First public release. Trix is an embeddable, stack-based (concatenative) scripting VM
that ships as a single C++23 header you `#include` into a host program. The language and
runtime are feature-complete; this release follows a full release-readiness pass
(documentation audit, sad-path test expansion, fuzzing campaigns, example hardening, and
a performance pass).

### Added

**Language and runtime**
- Concatenative core with 829 operators: stack manipulation, arithmetic, strings,
  arrays/dicts/sets/records, control flow, error handling, formatting, and I/O.
- Tagged-union value model: 31 types in an 8-byte `Object`, with 64-bit values
  (Long/ULong/Double/Address) in journaled heap extension slots and 128-bit values
  (Int128/UInt128) in 16-byte wide-value slots.
- Optional infix expressions, scoped modules (`require`/`use`/`import`), and
  precondition/postcondition contracts.

**Concurrency (cooperative, single-threaded)**
- Coroutines with sleep/yield/join and a two-tier priority scheduler.
- Bounded-buffer pipelines with automatic backpressure.
- Actors: isolated processes with mailboxes, send/recv, and selective receive.
- Erlang/OTP-style supervision: monitors, links, and restart strategies/intensity.

**Computation**
- Logic programming: Prolog-style unification, backtracking, and choice points,
  built on the save/restore journal.
- Reactive cells: spreadsheet-style incremental recomputation with watchers.
- Lazy sequences: infinite streams with deferred evaluation and transducers.
- Algebraic effects and delimited continuations; pattern matching, protocols
  (open type dispatch), and a GenServer abstraction.

**Durability and memory**
- Transactional local arena: `save`/`restore` checkpoints reclaim allocations and
  revert in-place mutations through a journal (no GC on the local arena).
- Precise mark-sweep garbage collector for the durable global region.
- Whole-VM snapshot/thaw: serialize the entire interpreter (stacks, heap, in-flight
  coroutines, mailboxes, supervision trees, reactive graph) to disk and resume later.

**Tooling and embedding**
- Single-header embedding with a constexpr user-operator table and a resident/server
  mode (`invoke()` / `raise_interrupt()`) for long-lived hosts.
- Source-level interactive debugger (`--inspect`): TUI single-stepping, conditional and
  one-shot breakpoints, watch expressions, and a sandboxed eval prompt — its UI is
  written in Trix itself.
- 24 example programs, including a full Infocom Z-machine, a metacircular Scheme with
  `call/cc`, a CHIP-8 emulator, a regex engine, and an in-memory SQL with transactions.
- 61 reference documents covering every subsystem.

### Quality

- ASan/UBSan clean; compiles `-Werror` under GCC 15 and Clang 20 with an extensive
  warning set.
- 20,200+ test assertions across 274 test files; a libFuzzer harness over the full
  interpreter (coverage-guided).
- Dependencies are readline and zlib, both opt-out (`--no-readline` / `--no-zlib`).
- Apache 2.0 licensed.

[Unreleased]: https://github.com/mcguidarelli/trix/compare/v0.11.0...HEAD
[0.11.0]: https://github.com/mcguidarelli/trix/compare/v0.10.1...v0.11.0
[0.10.1]: https://github.com/mcguidarelli/trix/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/mcguidarelli/trix/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/mcguidarelli/trix/releases/tag/v0.9.0
