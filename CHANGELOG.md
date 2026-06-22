# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Named declared locals in the `|...|` preamble.** A `/`-prefixed name is a
  declared frame local — `{ |a b /t /acc| ... }` takes `a`, `b` as parameters
  (popped) and declares `t`, `acc` as locals. Declared locals are *not* bound at
  entry: they read `/undefined` until assigned with `local-def` (or `store`),
  exactly like a `local-def` working variable, but being named at scan time they
  are visible to `#e` early binding and tooling. The named-scratch form
  `{ | /t /acc| ... }` (zero params) is also allowed. Capacity (`#N` / `#+N`)
  now counts against `P + M` (params + declared locals).
- **Parameter slot-indexing.** A locals proc's references to its own parameters,
  where they appear directly in the proc's top-level body, are compiled at scan
  time into direct frame-slot references — resolved by positional frame indexing
  (`O(1)`, no hash, no dictionary-stack walk) at run time. Always on (no suffix).
  This is the same hot-loop speedup as `#e` for reading a parameter, without
  `#e`'s binding-cache sensitivity to recursion / `save`, and makes an own-frame
  parameter reference inherently immune to the `#e` operator-shadow hazard (the
  parameter always wins). Depth-0 only: a parameter referenced inside a nested
  proc keeps a dynamic name (Trix frame scoping is dynamic). Parameters remain
  reachable by name (`/p load`, reflection). Tail Call Optimization is preserved
  for a parameter-bound proc invoked in tail position. (Declared `/locals` are
  not yet slot-indexed.)

### Changed
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

[Unreleased]: https://github.com/mcguidarelli/trix/compare/v0.10.1...HEAD
[0.10.1]: https://github.com/mcguidarelli/trix/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/mcguidarelli/trix/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/mcguidarelli/trix/releases/tag/v0.9.0
