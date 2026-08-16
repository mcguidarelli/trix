# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.12.0] - 2026-08-16

- **Release tarballs now come in a `-debugger` variant.** The default
  `trix-linux-<arch>.tar.gz` stays debugger-free -- the `debug-*` op family and
  breakpoint machinery are deliberately not in the shipping build -- so `--inspect` has
  never worked from a published binary. A second `trix-linux-<arch>-debugger.tar.gz` is now
  built per arch with `-DTRIX_DEBUGGER=ON` and ships `lib/*.trx` beside the binary, which
  the inspector needs at run time: its UI is written in Trix and loaded from
  `lib/debugger.trx` through the binary-relative arm of the module search path. The release
  job asserts all three properties -- the variant reports the `debugger` feature, accepts
  `--inspect`, and the default build does *not* -- so the two cannot silently converge.
  Checksums for both tarballs share the existing per-arch `.sha256` file.

- **Snapshot-thaw fuzz harness (`fuzz/fuzz_thaw.cpp`).** Targets `startup_image()` -- the
  `--image` / `-l` boot thaw path: header validation, section reads, CRC gates,
  `restore_from_header()`, `apply_fixup_streams()`, stdio reattach, and post-thaw execution
  over the restored heap. Thaw runs on untrusted input whenever a user loads an image they
  did not produce, across 185 revisions of conditional decode. The image is guarded by a
  whole-file CRC-32 that random mutation never reproduces, so a naive fuzzer stalls at the
  checksum arm; a custom mutator pins a valid header, fuzzes the VM blob, and re-stamps
  `vm_used`, the VM-base sentinel, and the checksum, so every generated image clears the
  gates and drives decode with root offsets pointing into corrupted heap -- the real threat
  model, since a CRC-32 is an integrity check, not a security boundary. The template header
  is minted at startup from the harness's own linked engine, so the snapshot version and
  operator-table signature always match the binary under test rather than rotting in-tree.
  Build with `fuzz/build.sh`, run with `fuzz/run_thaw.sh`.
- **`vm-heap-verify`: an all-builds heap-invariant walker.** Runs a mark-only pass with a
  verifier armed and returns a dict of violation counts (`/stale-refs`, `/kind-mismatch`,
  `/work-list-dirty`, `/dict-bucket-errors`, `/dict-length-mismatch`,
  `/dict-bad-bucket-count`), all zero on a consistent heap. Where `vm-global-gc-probe`
  answers "how much is garbage", this answers "is the heap self-consistent". Present in
  every build rather than behind `TRIX_DEBUGGER`, so it runs under CI's cmake configuration
  and a heap regression is caught by the standard gate set instead of only a developer's
  local debug binary. A reachable slot holding an unresolvable reference means a root was
  dropped and the block reused underneath it; the verifier records what `gc_mark_object` was
  already deciding, leaving marking behaviour unchanged.
- **The GC-stress suite is now a CI gate.** `tests/test_gc_stress_*.trx` drive
  `vm-gc-stress` / `vm-gc-poison`, which make `gvm_alloc` collect before every global
  allocation so a dropped GC root surfaces as a deterministic use-after-free at the exact
  site. Both ops compile out unless `TRIX_DEBUGGER` is defined, so these tests ran in no
  automated gate at all -- including through the GC perf campaign and the
  localdict/globaldict split, exactly the changes they exist to guard. A `TRIX_DEBUGGER`
  cmake option now builds a second tree in both `ci-local` and a new `ci.yml` job, leaving
  the primary build on the shipping configuration.
- **`tools/ci-local.sh`: mirror the full CI gate set before pushing.** `build.sh` compiles
  with `-DTRIX_DEBUGGER -DTRIX_HEAP_TRACKING`, so its binary carries ops CI's cmake build
  does not, and CI runs a standalone `clang-format --Werror` check that `runtests.sh` does
  not -- repeated "green locally, red in CI" came from that gap. `ci-local.sh` builds the CI
  way, stashes the dev binaries so the run matches CI, executes every `ci.yml` gate, and
  restores the dev binaries on exit. It also pins `gcc-15` to match CI's compiler leg, with
  a loud warning on fallback: a `-Wnrvo` change that was green locally turned CI red under
  its default gcc-13.
- **`tetrix` and `chip8` are built in CI and shipped in the release tarballs.** Both are the
  trix VM with a native kernel registered for their showcase, but cmake built `trix` alone,
  so CI skipped the cross-binary tests (snapshot user-operator mismatch, chip8 native
  lockstep) and the tarballs shipped `trix` only. All three host targets now share one
  compile/link configuration, run in CI, and are stripped, version-verified, and packaged
  for both x86_64 and arm64.
- **`examples/zmachine`: 20 more Z-code stories in the catalog.** Added to the fetch
  manifest, the in-interpreter recognition table, and `CATALOG.md`, all verified to boot and
  self-identify. Nine were already documented and recognized but absent from the manifest,
  so `fetch-stories.py` could not retrieve them. Also corrects a pre-existing mislabel --
  IFhd `9:010613` was recorded as *All Things Devours* but is *Dangerous Curves* (confirmed
  by the story's own banner) -- and adds an "Out of scope -> Glulx" section for requested
  titles that target a different VM. Story files remain unbundled and fetched on demand.
- **The interactive inspector can halt anywhere — four fixes from
  [#14](https://github.com/mcguidarelli/trix/issues/14).** Reported as "the debugger TUI
  doesn't come up", which turned out to be four independent defects, each on its own
  sufficient to kill a session before it drew a frame.
  - **`let` / `destructure` scopes are growable.** The scope dict was created at exactly
    its bound-name count and fixed there, so it was full the instant it was pushed and the
    body's first `def` raised `/dict-full`. That also made the scopes undebuggable: the
    inspector halts by running Trix in the interrupted program's dict context, so no halt
    inside any `let` could survive. `def` inside a scope now binds in the scope and is
    reclaimed by the matching `end`.
  - **`stream-name` honours its documented contract.** It returned the synthetic source
    name (`--memory--`, `--stdin--`, `--string--`) for non-file-backed streams instead of
    `null`, so the debugger's null-guarded source lookup sailed past the guard and tried to
    open `--memory--` as a file. `--inspect` died with `/filename-not-found` on any
    top-level-only script.
  - **`--inspect` halts at a fatal error** instead of tearing the session down at the one
    moment you most want to look, matching `--inspect-on-error`. The variants now differ
    only in where they *first* stop.
  - **`--inspect` works with the CWD anywhere.** The session bootstrap required its own
    implementation as `lib/debugger.trx`, a prefixed relative name only ever resolved
    against the CWD, so the inspector ran solely from a source checkout root. It now
    requires by bare name through the module search path, which gained a second
    binary-relative arm (`<bin>/../lib/trix`) for installed layouts — and `lib/*.trx` is
    now installed, which it previously never was.
- **`lib/debugger.trx` keeps its scratch out of the inspected program.** Every proc on the
  halt path declares its own frame capacity (`|args|#+N`) and binds with `local-def`.
  Previously the callback `def`'d ~66 working variables into whatever scope the interrupted
  program left topmost, polluting it, and dying outright whenever that scope was a full
  fixed dict.
- **`-d` / `--debug` is described accurately** in `--help`: it arms the debugger *substrate*
  (the `debug-*` ops and breakpoint machinery a script drives itself), which is not the
  full-screen inspector. Promising "interactive debugger" is what sent #14's reporter
  looking for a TUI that flag never starts.

- **`examples/amazing.trx`: crack grid (`--grid crack`) -- a seventh topology, irregular Voronoi
  cells.** A "crack" maze tessellates the plane into irregular polygons (the cracked-mud look) and
  carves a maze over the cell-adjacency graph. The Voronoi geometry is generated **entirely in
  Trix** (no host tool, no bundled data) using the engine's IEEE-754 ops: each cell is built by
  half-plane clipping (Sutherland-Hodgman) of the bounding box against every other site's
  perpendicular bisector, with `--crack-relax` Lloyd passes to even the cells out, and the per-edge
  clip owner yields the adjacency directly. Generation is **~O(N)** (not O(N^2)) via a uniform
  bucket grid -- Voronoi neighbours are spatially local, so each cell only clips against sites in
  expanding rings of nearby buckets until the prune radius is exceeded; cell count is `w*h`, so a
  40x40 (1600 cells) generates in a couple of seconds. The irregular graph rides the **same**
  topology descriptor as the lattice grids by a one-row-of-N embed (cell id = `cy*w+cx`, `w*h = N`),
  so all eight portable generators, the generic solvers, `--metrics`, `--target-*` braiding and
  `--color` work unchanged (56 portable combos). The colour path adds a convex-polygon **scanline
  fill** -- each cell is filled by its BFS distance, walls stroked on top -- and the `--solve` ribbon
  runs between cell centroids. Self-test +4 (268): backtracker/Kruskal/Wilson each carve a perfect
  maze (connected spanning tree) on the irregular graph, plus a graph-connectivity check. Masking and
  `--weave` stay square-only.
- **`examples/amazing.trx`: Trémaux solver (`--solver tremaux`).** A fourth method in the solver
  zoo: Trémaux's algorithm, the depth-first passage-marking walk a person can run with chalk and no
  map (mark a passage once when you take it, twice when its far side is exhausted, and never re-enter
  a twice-marked passage). Implemented as an explicit-stack DFS over the topology descriptor
  (`neighbors` / `open?`), so it runs on **every** grid; it wanders into dead ends and backs out,
  visiting more cells than A\*'s focused frontier but fewer than BFS's full flood, and stops the
  instant it reaches the exit. The visited cells are tinted green on the square render and the
  visited **count** reports on every grid (the solution ribbon itself is the canonical BFS path, as
  with the other solvers). Self-test +5 (264): reaches the goal while visiting between the path
  length and every cell, on the square grid and on a non-square (hex) grid.
- **`examples/amazing.trx`: zeta grid (`--grid zeta`) -- a sixth topology, square cells with
  diagonal passages.** A cell may connect to its four diagonal neighbours as well as the four
  orthogonal ones, subject to the rule that the two diagonals of any 2x2 quad never cross. That is
  guaranteed by construction: at grid creation each quad is pre-assigned one permitted diagonal
  orientation (randomised by `--seed`), so `neighbors` is stateless with respect to carving and
  every portable generator works unchanged -- including the pre-collecting Kruskal and walk-then-carve
  Wilson that an algorithm-chooses-the-diagonal constraint would have broken. Reuses upsilon's
  two-byte cell layout and eight-direction table; a new renderer draws diagonal corridors as
  centre-to-centre bands through the quad corner. All eight portable algos work (48 combos), with
  `--color`, `--solve` (diagonal ribbons), and `--target-*` braiding. Self-test +16 (259): a perfect
  connected crossing-free maze for backtracker/Kruskal/Wilson plus the full 8-algo connectivity row.
- **`examples/amazing.trx`: the grid generate/render/solve pipeline is now table-driven.** The
  per-topology `--grid` dispatch was a six-deep `opt-grid (X) eq { ... }` if-else ladder (and a
  parallel one for the solve ribbon). The topology descriptor gained `make-grid` / `bfs-distances` /
  `mono-render` / `color-render` / `braid` / `draw-overlay` (it already had `neighbors` / `link` /
  `visited?` / `mark` / `open?` / `bfs-solve`), so both ladders collapse to one descriptor-driven
  block each (the solve overlay keeps a lone theta special, whose polar ribbon needs the grid).
  Adding a grid is now one descriptor plus one registry row -- no new branch. Output is byte-identical.
- **`examples/amazing.trx`: metric-targeted braiding (`--target-dead-ends` / `--target-loops`).**
  The `--metrics` numbers become targets: `--target-dead-ends P` braids the maze toward a dead-end
  percentage, `--target-loops N` toward an exact loop count. Braiding is monotone -- from the
  generated perfect maze, each step (knock one dead-end's closed wall through to an in-bounds
  neighbour) removes 1-2 dead-ends, adds exactly one independent loop, and never disconnects -- so
  this needs no annealer: enumerate the dead-ends once, shuffle, and knock them until the metric is
  hit, tracked incrementally. Loop count is exact (one loop per braid); a dead-end percentage lands
  within one step; an already-satisfied target is a no-op. Reports achieved-vs-requested to stderr,
  and `--metrics` reflects the result. The whole thing rides the topology descriptor
  (`neighbors`/`open?`/`link`), so it works on **every** grid including upsilon (whose probabilistic
  `--braid` is still deferred). This is v1 -- the targets braiding can reach monotonically;
  non-monotone targets (solution length, twistiness) that need wall additions + a real annealer are
  left for a later revision. Self-test +11 (243): the loop/dead-end accounting is cross-checked
  against a full degree tally, connectivity is verified, and upsilon is exercised through the shared
  descriptor.
- **`examples/amazing.trx`: four new colormaps + ramp shaping for large mazes.** The palette
  set grows from ten to fourteen: `fire-ice` (a vivid hand-built diverging map, ice navy ->
  white -> fire red), and matplotlib's `spectral`, `coolwarm`, and the cyclic `twilight`. Two
  new knobs reshape the distance ramp, which a single linear sweep washes out past ~100x100:
  `--color-curve G` applies a gamma (`t' = t^G`, `G>1` spreads near cells, `G<1` the far tail),
  and `--color-cycles N` is a seamless sine-wave (raised-cosine) modulation that repeats the
  palette as `N` contour bands -- "Sine-Wave-Modulation" -- pairing naturally with the cyclic
  `twilight`. Both default to identity, so existing renders are byte-identical; the transforms
  live in one `-color-transfer` helper ahead of `cmap-color`. Self-test +14 (232): both
  endpoints of each new palette, plus the gamma and sine transforms. New gallery shots:
  `color-{fire-ice,spectral,coolwarm,twilight}.png`, `color-curve-fire-ice.png`,
  `color-cycles-twilight.png`.
- **`examples/amazing.trx`: `--unicursal` single-path labyrinth.** A classical labyrinth -- one
  non-branching path that visits every cell, no junctions. Built by the textbook passage-doubling
  transform: generate a perfect maze, then weave its spanning tree into a single Hamiltonian path on
  the `2w × 2h` doubled grid (each cell becomes a 2×2 block the path snakes through, every tree edge
  crossed twice). The output is square-only and twice the `--size`; with `--color` the distance
  heatmap is position-along-the-path, flowing continuously end to end. Self-test +3 (218): the weave
  is verified to be a single path (degree ≤ 2, two ends, connected).
- **`examples/amazing.trx`: `--solver` zoo (dead-end-fill, A\*, wall-follower).** `--solver NAME`
  (implies `--solve`) picks how the maze is solved and visualises the method. On a perfect maze all
  recover the same solution ribbon; what differs is the work: `dead-end-fill` iteratively fills every
  dead-end until only the solution corridor stands (tinted grey on square -- the "drained" maze),
  `astar` expands a focused frontier toward the goal over a binary-heap A\* (tinted blue, far fewer
  cells than BFS's flood), and `wall-follower` traces the left-hand-rule walk (square only; warns and
  falls back to BFS elsewhere). dead-end-fill and A\* ride the descriptor, so their counts report on
  every grid; each method prints a one-line stat. Self-test +7 (215).
- **`examples/amazing.trx`: `--algo origin-shift` (Origin Shift, CaptainLuma 2023).** A
  twelfth maze algorithm, and the only one that doesn't carve or add walls: the maze is
  kept as a directed spanning tree of parent pointers, and each step re-roots it at a
  random neighbor of the origin (one edge removed, one added), so *every intermediate
  state is a valid perfect maze*. Seeds a tree by BFS, mixes for `20x` the component size,
  then carves one wall per pointer. Portable across all five grids (8th portable algo, 40
  combos) and composes with masking (one origin shift per component). Mixes toward a
  random-looking maze but is not a proven uniform spanning tree. Self-test +8 (208).

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
- **`examples/amazing.trx`: `--flow` flow-field mazes.** `--flow radial|linear|spiral|sine`
  turns Kruskal into a minimum spanning tree over a scalar field -- walls are weighted by
  the field at their grid midpoint and carved low-weight-first (O(n) counting sort), so the
  corridors flow along it while staying a perfect maze. Works on every grid and composes
  with `--color`; `--flow-jitter N` dials the field bias from strict "flow art" (`0`) to a
  twisty ordinary Kruskal maze.
- **`examples/amazing.trx`: `--flow-image NAME` image-steered flow mazes.** The same
  weighted Kruskal driven by an *image* field instead of a formula: the corridors carve the
  picture's dark regions first, so the maze's flow aligns to the image at a macro scale
  while still punching a perfect maze. Trix has no image decoder, so the new
  `tools/gen_flow_field.py` samples a PNG/SVG once into a small `flow-fields/<NAME>.trx`
  grayscale grid (loaded via a new `FLOW-FIELDS` registry). Two fields are bundled: `logo`
  (from `assets/trix-logo.svg`, traces the wordmark) and `cat` (a bold silhouette the tool
  draws itself -- broad masses steer the flow more legibly). Only the derived fields are
  committed -- bring your own image for local mazes.
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

[Unreleased]: https://github.com/mcguidarelli/trix/compare/v0.12.0...HEAD
[0.12.0]: https://github.com/mcguidarelli/trix/compare/v0.11.0...v0.12.0
[0.11.0]: https://github.com/mcguidarelli/trix/compare/v0.10.1...v0.11.0
[0.10.1]: https://github.com/mcguidarelli/trix/compare/v0.10.0...v0.10.1
[0.10.0]: https://github.com/mcguidarelli/trix/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/mcguidarelli/trix/releases/tag/v0.9.0
