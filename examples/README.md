# Trix Examples

Realistic showcase programs that exercise Trix's unique capabilities.
Each example is self-contained (no external resources), runs to
completion with visible output, and is small enough to read in one
sitting.

Run any example directly:

```bash
./trix examples/sudoku.trx
```

For meaningful timing figures, run the optimized binary:

```bash
benchmark/release.sh                          # one-time build
benchmark/trix.rel examples/sudoku.trx
```

## Examples

### [sudoku.trx](sudoku.trx) — Logic / backtracking

Classic 9x9 Sudoku solver built from `logic-var`, `unify`, `guard`, and `choice`.  Recursive search across empty cells; `choice` picks values 1..9; peer-check guards prune conflicting placements; Trix's save/restore handles rollback automatically.  After each solve the bound grid escapes the per-puzzle rollback through a `${...}` global-VM snapshot bound via `def-persist`, and is independently re-verified after the restore (27-unit permutation bitmask check); run totals (puzzles solved, cumulative solve time) accumulate across the rollbacks the same way.  Long solves print a live "marching ants" display -- a one-line search status every ~2 s (attempts, rate, deepest cell) and the current SPECULATIVE grid every ~10 s, hints solid and in-trial candidates ANSI-dim, so you literally watch the backtracker think (the counters are `def-persist` writes: the only kind that survives every backtracking rollback).  Interactive per-puzzle prompt + solve-time measurement.  Puzzles loaded from an external catalog (see below).

### [genealogy.trx](genealogy.trx) — Prolog-style logic / rule-based reasoning

A four-generation, 14-person family-tree knowledge engine that exercises the rest of Trix's logic surface beyond what sudoku covers (`find-all`, `find-n`, `for-each-solution`, `aggregate`, `unify-match`, `naf`, `copy-term`, `once`) plus the supporting language features (` | args | #+N` locals frames + `local-def`, `def-persist` for scalar state that escapes a `save`/`restore` rollback, and `[...]#=` eq-arrays for one-shot temporary scratch).  Two relations -- `parent` and `spouse` -- back twelve query predicates: single-solution checks (`is-parent?`, `is-spouse?`), multi-solution finders (`parents-of`, `children-of`, `spouses-of`), compound queries built by composing `parent-alts` with `flat-map` (`grandparents-of`, `siblings-of`, `cousins-of`), recursive transitive closure (`ancestors-of`, `descendants-of`), step-relation derivations from spouse-of-parent (`step-parents-of`, `step-children-of`, plus cycle-aware `step-ancestors-of` / `step-descendants-of`), and negation-as-failure (`childless?`, `only-child?`, `unmarried?`).  Section G uses `unify-match` as a relationship classifier: a 10-flag `/relation` tag (parent / child / spouse / sibling / ancestor / descendant / cousin / shared-ancestor / step-ancestor / step-descendant) is dispatched against pattern arms whose wildcards are fresh `logic-var`s -- so each `_` matches anything but each pattern arm is independent.  Pass `--grandpa` to swap in the cast of Lonzo & Oscar's 1947 song "I'm My Own Grandpa" (narrator marries widow, narrator's father subsequently marries widow's daughter); the step-relation chain closes into a self-loop and the classifier announces the punchline via a pattern arm whose two name slots share a single `logic-var`, forcing `a == b`.  A scalar `calls-made` counter is bumped via `def-persist` inside section G's per-pair `save`/`restore` wrapper -- without `-persist`, every `restore` would zero it.  The "predicate as alt-array" idiom is documented at length in the file header: each fact-base relation compiles to an array of `curry`-bound alts (one per fact, with the per-fact context baked in via the curried value to sidestep Trix's loop-variable closure trap), then `find-all` / `choice` / `aggregate` / `for-each-solution` drive the alts as the query operator dictates.  Run with `--vm-size=2M`.

### [heist.trx](heist.trx) — Set-cover optimization + bitmask enumeration + persist family

A heist crew planner over three jobs (the Aurora Casino Vault by default; `--job gallery`, `--job crown`; `--all-jobs` side by side): every suspect covers a skill subset and charges a fee, and the script compares a greedy cover against exhaustive bitmask-enumerated optima by size and by fee, printing the symmetric difference between greedy and optimal picks.  Constraint flags: `--budget N` (optimal under a fee cap, or an honest "no team can cover"), `--no-pair A,B` (antipathy), `--analyze NAME` (per-suspect impact), `--extra-roster` (12th suspect).  The optimizer's carry-over bests live in userdict via `def-persist` so they survive each enumeration iteration's `save`/`restore` reclaim -- the canonical bounded-heap search-loop idiom.  Solver outputs brute-force-verified.  Run with `--vm-size=4M`.

### [supervised-word-count.trx](supervised-word-count.trx) — Pipelines / actors / supervision

Three worker actors each tokenize a text file through a four-stage pipeline (`regex-split` / `pipe-map` / `pipe-filter` / `pipe-tap`+`precondition`) and send per-file counts to a reducer actor.  One corpus file (`poison.txt`) carries a sentinel token that trips the pipeline's `precondition`; the worker dies and the `/one-for-one /transient` supervisor restarts it.  The run completes with no operator input and prints the top-10 merged words.  See [word-count-corpus/](word-count-corpus/).

### [reactive-financial-model.trx](reactive-financial-model.trx) — Reactive cells + save/restore

Four-quarter income statement: 13 base cells (revenue / COGS / opex per quarter plus a shared tax rate) feed 22 computed cells (per-quarter gross-margin, operating-income, tax, net-income; annual rollups for every line + annual margin %).  Zero-crossing watchers register on every quarterly and annual net-income.  Three what-if scenarios (Q2 revenue uplift, recession, cost cutting) mutate the graph inside a `batch`; each scenario is bracketed by `save` / `restore` so the baseline is rolled back untouched between runs.  Reactive + transactional rollback is the composition few other reactive systems offer.

### [village.trx](village.trx) — Actors + supervision + reactive cells + snap-shot/thaw

Small NPC village: three actors (baker, guard, farmer) run under a `/one-for-one /transient` supervisor; shared `bread-count` / `flour-count` cells track inventory with a low-stock / sellout watcher.  Simulation runs 6 AM - 8 AM and then `snap-shot`s the entire VM to a byte image -- mailboxes, coroutine stacks, reactive graph, and all.  The master then forks two child `trix` processes that `thaw` the SAME image and run two divergent 8 AM - 10 AM futures (festival day vs. rainstorm).  Identical starting state, different stimuli, different two-hour traces.  This composition -- paused cooperative coroutines serialized to bytes and resumed in a fresh process -- is specifically what Lua's snapshot story cannot replicate.

### [effects_mini_scheme.trx](effects_mini_scheme.trx) — Algebraic effects + tagged AST + handler-stacked dispatch

A ~260-line miniature Lisp-like evaluator where the meaning of **variable lookup**, **primitive dispatch**, and **stuck terms** is supplied by an algebraic-effects handler rather than baked into the evaluator.  Three effects -- `/env-lookup`, `/apply-prim`, `/stuck` -- and a 12-line `tag-match` evaluator over four AST kinds (`/lit`, `/var`, `/app`, `/if`).  Three handlers each install a different semantics for the SAME ASTs: **real-eval** (production: dict-backed env, real add/sub/mul/eq, `/failure throw` on stuck), **tracing+real** (delegates outward via re-`perform`; observes every effect; caps to 4 trace lines per program), and **synthetic** (permissive: unknown var = 0, unknown op = 0, stuck = 42 -- never fails).  Demonstrates the Eff/Koka deep-handler stacking idiom plus `[/env] closure-capture` for handler-dict procs that close over a per-instance environment.  Companion to [`docs/effects.md`](../docs/effects.md); compare with the full closure / call/cc / CEK interpreter in `mini-scheme.trx`.

### [mini-scheme.trx](mini-scheme.trx) — Tagged values + records + CEK trampoline + first-class continuations

A ~1600-line metacircular Scheme interpreter: tokenizer, recursive-descent parser, and a Felleisen-style **CEK machine** -- single-step transitions over `(expr, env, kont)` triples driven by a top-level loop -- in place of the traditional direct-style eval/apply recursion.  Tail calls reuse the current kont (unbounded by Trix's 64-deep dict stack); non-tail calls extend kont as a Scheme list of frame Tagged values, bounded only by VM heap.  call/cc and coroutine-call/cc capture the driver's kont as a `/continuation` Scheme value -- no Trix-level `delimit` / `capture` involved.  Every Scheme value is a Trix Tagged; every environment frame is a Trix Record.  Nine demos: factorial, fibonacci, map, let, counter-via-set!, classic call/cc early-exit (`(+ 10 (call/cc (lambda (k) (k 42))))` => 52), two-coroutine ping-pong via `coroutine-call/cc`, and two deep-recursion stress demos (`count-down 100000`, `sum 500` -- impossible under the old direct-style evaluator, which died around call depth 14).  Every demo's result is machine-checked: a mismatch prints a diagnostic and the run exits nonzero, and the `== mini-scheme done ==` marker prints only on a fully verified run.  Runs at the default `--vm-size`; bigger recursion targets (`sum 100000`) need `--vm-size=8M`.

### [markov.trx](markov.trx) — Direct coroutine + nested dict + record

A bigram Markov-chain text generator over the opening chapters of Pride and Prejudice.  The training stage walks a 1,795-token corpus into a `prefix-word -> Record{total-count, successors}` dict, where the inner `successors` dict is mutated in place and the outer record's `total-count` field is bumped via `record-update`.  The generation stage is a plain `coroutine-launch`ed generator -- no actor wrapper, no pipeline DSL -- that hands tokens through a capacity-1 `pipe-buffer` to the main consumer as a synchronous producer/consumer pair.  Weighted sampling uses `rand-bounded-uinteger` with a fixed `rand-seed` for bit-exact reproducibility.  See [markov-corpus/](markov-corpus/).

### [minidb.trx](minidb.trx) — Records as schemas + tagged AST + save/restore as user-facing transactions

An in-memory SQL-ish database with a hand-rolled tokenizer / recursive-descent parser / executor.  Every parsed statement is a Trix Tagged; every WHERE clause is a Tagged tree (`/where-eq` / `/where-lt` / `/where-and` / ...).  `CREATE TABLE` builds a Record schema on the fly and rows are instances of that per-table record-type.  The table's primary-key dict IS the index, so `WHERE id=X` takes a one-hop `known-get` path (executor prints `PLAN: indexed lookup` vs. `PLAN: full table scan`, so a reader sees which path ran).  `BEGIN` / `COMMIT` / `ROLLBACK` drop straight onto Trix's `save` / `restore`: the demo inserts a row inside a transaction, rolls back, and SELECTs zero rows.  The script runs a built-in 12-row demo then consumes any SQL piped on stdin.

### [regex.trx](regex.trx) — Tagged AST + two engines + Russ-Cox-style ReDoS demo

A regex matcher that ships **two** engines side by side over the same parsed AST: a 50-line **naive backtracker** that lists all match positions (the cautionary tale -- exponential on nested quantifiers), and a textbook **Thompson NFA + Pike VM** (the hero -- linear in input length, regardless of pattern shape).  AST nodes are Trix Tagged (`/lit`, `/dot`, `/class`, `/anchor`, `/cat`, `/alt`, `/opt`, `/star`, `/plus`).  NFA states are 4-element arrays `[kind payload out1 out2]` constructed via standard Thompson fragment-and-patch; epsilon-closure folds anchors and split-states into the addstate worker.  A 25-case test corpus runs both engines and asserts agreement.  The headline is the **head-to-head ReDoS table** at the bottom: pattern `a?{n}a{n}` against `a^n` (Cox's canonical exponential pathology), n from 2 to 10.  Naive doubles each step; Pike scales linearly -- the gap is ~10x at n=10 and would be 1000x at n=20 but for the naive matcher's heap cost.  Run with `--vm-size=64M` for the full demo.  Reference: Russ Cox, [*Regular Expression Matching Can Be Simple And Fast*](https://swtch.com/~rsc/regexp/regexp1.html) (2007).

### [symdiff.trx](symdiff.trx) — Tagged AST + tag-pattern rewrite to fixpoint

Symbolic differentiator: parse infix algebra (`x^2 + 3*x*sin(x) - log(x+1)`) into a Tagged AST, differentiate via tag-dispatched rules, simplify via algebraic-identity rewrite to a fixpoint, pretty-print back to infix.  This is a **transformer** showcase -- mini-scheme interprets, minidb executes, regex matches; symdiff *rewrites trees*.  AST nodes are direct-tagged (`/num`, `/var`, `/add`, `/sub`, `/mul`, `/div`, `/pow`, `/neg`, `/sin`, `/cos`, `/tan`, `/exp`, `/log`) so simplifier rules dispatch on a single `tag-name` instead of an outer-tag-plus-op-tag wrapper.  The differentiator is one handler per tag (sum / product / quotient / power / chain rule); the simplifier is a list of small tag-pattern checks (`0+e`, `e*1`, `e^0`, double-neg, constant folding, neg-pulling, `e+e -> 2*e`, associative `c1*(c2*e) -> (c1*c2)*e`) that run bottom-up and re-invoke until the tree is unchanged.  Pretty-printer tracks outer-op precedence to decide where to parenthesize and treats `^` as right-associative.  Twelve textbook expressions show the raw vs.\ simplified derivative side by side; e.g. `3*x^2 + 2*x + 1` -> raw `0*x^2 + 3*(2*x^1*1) + (0*x + 2*1) + 0` -> simplified `6*x + 2`.

### [tetrix.trx](tetrix.trx) — Coroutines + actors + raw-mode keyboard + virtual-screen render + AI search + pack/unpack persistence

A real, playable terminal falling-block game (~3400 lines): seven-bag PRNG, full SRS rotation with wall kicks, T-spin detection (3-corner rule), guideline scoring with B2B + combo + level multiplier, NES-table gravity.  Three play modes selectable via CLI flag: **`--human`** (you play), **`--ai-peek`** (2-piece-lookahead AI plays under the same info constraints a human has), **`--ai-oracle`** (perfect-information AI sees the whole bag schedule and runs a depth-N deterministic search).  Architecture: input / gravity / AI / AI-tick coroutines spawned via `actor-spawn` send messages to a central game-actor that owns the play state; the AI uses Dellacherie-style heuristic weights (`w_height`, `w_holes`, `w_bumpiness`, `w_well`, `w_lines`) over a per-recursion-level scratch field, allocation-free in the inner loop.  Rendering goes through `screen-render`'s diff so only changed cells reach the terminal; `with-fullscreen-screen` provides a crash-safe alt-screen + cursor-hide + raw-mode wrapper.  Persistence uses **`pack` / `unpack`** to write a 298-byte binary save (294-byte payload + 4-byte trailing Fletcher-32 checksum); load-time validation catches one-byte corruption.  Phase 9 polish: animated TETRIX title splash with all 7 tetrominoes, line-clear flash, game-over flash.  Bare `./trix examples/tetrix.trx` launches the game in human mode (this is a showcase -- the player should land in the action); `--ai-peek` and `--ai-oracle` switch modes; `--self-test` runs the 259-assertion in-source test suite (pure logic + AI + save/load + flash no-op contracts) instead.

### [chip8.trx](chip8.trx) — VM emulator + dispatch dicts + coroutines + virtual-screen

A working CHIP-8 + Super-CHIP emulator (~1700 lines) running the same 1977-vintage instruction set the original COSMAC VIP did.  4KB byte memory with the canonical low-res font sprites at 0x000 and the SCHIP hi-res font at 0x050; ROMs load at 0x200.  CPU step is a four-level **dispatch tree** of module-level dicts (`exec-by-n0` outer, `exec-0xxx` / `exec-8xyN` / `exec-EXNN` / `exec-FXNN` inner) -- each opcode-class lookup is one O(1) `case` call, way ahead of the chained-`if-else` ladder both in lines-per-arm and lookup cost.  Three cooperating coroutines drive the emulator: a 60Hz **timer-actor** decrements DT/ST, a configurable-rate **cpu-actor** ticks the CPU (default `--ips=500` = 500 instructions/sec), and a 60Hz **render-actor** polls a `/display-dirty` flag and pushes a **half-block render** (U+2580/U+2584/U+2588 glyphs pack two vertical CHIP-8 pixels per terminal cell) through `screen-render`'s diff.  `--ascii` swaps in a one-cell-per-pixel `#` / ` ` renderer for non-Unicode terminals.  A fourth **key-actor** maps the canonical `1234 / qwer / asdf / zxcv` keypad layout into a 16-bit `/keys` bitmap with a 150ms decay (terminals don't generate key-up events).  ESC quits.  `--bell` rings the terminal once per ST > 0 transition.  ROMs are not bundled -- run [chip8-roms/fetch-ch8.py](chip8-roms/fetch-ch8.py) to pull CC0 public-domain ROMs from the chip8Archive; `--disasm` runs the disassembler without entering raw-mode; `--self-test` runs 113 in-source assertions (every opcode shape, scheduler smoke, off-screen render verification, keymap, SKP/SKNP, LD K) without touching a tty.  The dedicated `./chip8` host binary (chip8.cpp, the tetrix.cpp pattern) registers a native batch CPU kernel -- `--cpu-kernel=native | trix` selects it; rare instructions defer to the Trix dispatch, which stays the reference -- and under that binary the self-test adds lockstep scenarios requiring byte-identical machine state between both implementations (same engine PCG seed, synthetic inline programs).  Native batching makes `--ips` rates far past 1000 achievable.  `./chip8 examples/chip8-roms/snek.ch8` runs a ROM directly -- bare non-`.trx` arguments pass through to the script (a `.trx` argument overrides the script itself).

### [chip8-asm.trx](chip8-asm.trx) — Two-pass assembler + binary emission

The inverse of chip8.trx's disassembler: reads assembly source (one instruction per line, the same mnemonic format the disassembler emits, plus a `.BYTE` directive for data) and writes ROM bytes loadable via `load-rom`.  Round-trip contract: byte-perfect on every ROM you load -- `chip8.trx --disasm rom | chip8-asm.trx` reassembles to the identical binary, including odd-length data tails.  `--self-test` runs 33 assertions; `--help`/no-args print usage.

### [raycaster.trx](raycaster.trx) — Raw-mode keyboard + virtual-screen + DDA grid traversal

A first-person 3D dungeon (~700 lines): 32x32 tile map, per-column DDA ray cast, perspective-correct vertical strip rendering, mini-map overlay.  Walk forward/back/strafe with **WASD** or arrow keys; turn with **Q/E**; **T** toggles distance-based shading; **Y** cycles tile sets (ASCII -> Unicode shade blocks -> Unicode half-block + truecolor, 2x vertical resolution); **ESC** to quit.  Wall glyphs are taken from the tile byte itself (`#` stone, `H` brick, `=` panel, `+` pillar); rays that crossed a Y-grid line ("N/S walls") render with a one-step-dimmer glyph (`#`->`=`, `H`->`+`, ...) to suggest a fixed light direction.  Optional `--distance-shading` (or runtime `T`) further dims far walls along a 5-glyph ramp.  `--flythrough` runs a non-interactive scripted demo (~95 inputs over ~7 seconds) that does not need a tty -- pipe it to `asciinema rec` to record.  Reference: Lode Vandevenne's [*Raycasting*](https://lodev.org/cgtutor/raycasting.html) tutorial.

### [zmachine.trx](zmachine.trx) — VM emulator + opcode dispatch + ZSCII codec + dyn-mem snapshot

A full Infocom Z-machine interpreter (~7900 lines) covering versions V1-V5, V7, V8 (no V6 graphical).  Companion programming + implementation guide: [zmachine-manual.md](zmachine-manual.md).  Plays the entire Infocom catalogue plus modern Inform-V8 titles -- 99 / 99 across the test set (full list in [zmachine/CATALOG.md](zmachine/CATALOG.md)).  Architecture matches the spec topology: header parser, big-endian byte/word memory primitives with 16-bit address wrap (§1.1.3), a three-alphabet ZSCII decoder, V3 + V4+ object trees, a singleton-mutating instruction decoder + 5-form dispatcher, full eval/frame stack with var-0 peek-replace semantics, V3 sread + V5+ aread + tokenizer, V4+ window split + ANSI SGR text styling, and the EXT-opcode family (save / restore / save_undo / restore_undo / log_shift / art_shift / set_font / print_unicode / check_unicode).  Save / restore / restart all wired to Trix's `save` / `restore` plus a manual dyn-mem byte snapshot (Trix's heap rollback doesn't journal string-byte writes).  Story files are loaded into a multi-bank Trix-array of 32 KB strings so files past the 65535-byte string cap (e.g.\ Anchorhead, 520 KB) work uniformly.  Its interactive surface -- a game-fingerprint splash, showcase slash-commands (`/inspect`, `/dict`, `/map`, `/hint`, ...), CRT `--theme`s, InvisiClues `--hints`, and `--auto-map` room recording -- is documented in the companion manual's §12 (CLI flags + slash-commands).  Story files are not bundled: run [`zmachine/fetch-stories.py`](zmachine/fetch-stories.py) to fetch the freely-distributable titles, or see [zmachine/CATALOG.md](zmachine/CATALOG.md).

### [log-timestamp.trx](log-timestamp.trx) — Chrono Tier 1 surface

Compact ~50-line showcase for `epoch-time`, the `:I` / `:Il` PrintFmt format-spec letters, the UTC vs local-zone instant accessors, and the `/chrono` convenience dict.  Generates five synthetic log events spaced one second apart, prints each with side-by-side `[UTC: %Y-%m-%dT%H:%M:%SZ]` and `(local: %H:%M:%S %Z)` columns, then walks the events through a `begin`/`end` scope of `/chrono` to tally events by UTC weekday using the dict's short-name `weekday` (= `instant-weekday`).  Run under any `TZ` (e.g. `TZ=Asia/Tokyo ./trix examples/log-timestamp.trx`) to see the local column shift; UTC output stays stable.

### [birthdays.trx](birthdays.trx) — Chrono Tier 2 (udate) surface

Compact ~80-line age + days-until-birthday calculator for a fixed roster of historical figures (Ada Lovelace, Grace Hopper, Donald Knuth, Alan Turing, Margaret Hamilton).  Demonstrates `make-date`, `date-weekday`, `day-of-year`, `date-add-years` (with Feb 29 -> Feb 28 clamp), `date-diff-days` for both age computation and countdown, and `instant-to-date` to derive today's udate from the wall clock.  Closes with calendar facts (current year leap status, Feb day count) accessed through `//:systemdict:chrono begin/end` short names (`leap-year?`, `days-in-month`).  Output is reproducible day-to-day -- only the "Until birthday" / age columns advance with the calendar.

### [amazing.trx](amazing.trx) — 10-algo maze zoo + 5-grid topology + PNG output + bitmap font

A self-contained maze generator (~5200 lines), a homage to Steve Capps' "Amazing" on the 128K Mac.  Ships **ten** maze algorithms (recursive-backtracker, Kruskal, Wilson, Eller, binary-tree, sidewinder, Aldous-Broder, Prim, Hunt-and-Kill, Growing Tree) across **five** grid topologies -- square (the full zoo), plus hex, theta polar, triangle, and upsilon octagon (backtracker-only) -- with BFS distance-field heatmaps in six colormaps and `--solve` / `--braid` / `--weave` overlays.  Output is a real PNG whose format is assembled in Trix (no libpng) over the engine's native `deflate` (zlib) and hand-rolled `crc32`/`adler32` ops.  Companion teaching + implementation guide: [amazing-manual.md](amazing-manual.md).  `--self-test` runs 95 assertions; [`gallery.sh`](gallery.sh) renders a ~41-PNG showcase into `examples/maze-gallery/` (gitignored).  Needs `--vm-size=8M`+ (more for `--stress`); see the manual for sizing.

### [schedule.trx](schedule.trx) — Chrono recurring-event generator

A CLI-driven schedule generator that exercises the full calendar-app-style recurrence matrix: daily (every weekday / every weekend / every N days), weekly (every N weeks on a selected weekday subset), monthly (day-of-month with clamp / Nth-day-type with offset / "Friday the 13th" weekday-on-day), yearly (fixed Month/Day with Feb 29 clamp / Nth-day-type-of-Month with offset).  Range controls: `--start`, `--until`, `--after N`, `--limit N`.  Output: a default table of `# / date / weekday / +days`, a `--csv` mode, and a `--calendar` mode that renders only the months containing occurrences as 7-column day grids with hits highlighted via ANSI reverse-video.  Examples: `--kind monthly --ordinal last --day-type fri --every-n-months 1` (last Friday of the month), `--kind yearly --ordinal fourth --day-type thu --month nov` (Thanksgiving), `--kind monthly --weekday fri --day 13` (every Friday the 13th).  Demonstrates `make-date`, all `date-*` accessors, `date-add-{days,months,years}`, `date-diff-days`, `days-in-month`, `leap-year?`, `instant-to-date`, the `:D` PrintFmt/ScanFmt format-spec, and `match` for rule-kind dispatch.

### [debugger_tour.trx](debugger_tour.trx) — TRIX_DEBUGGER walkthrough companion

Companion script for [docs/debugger.md](../docs/debugger.md): a recursive `find-min` with a documented one-line bug (returns 0 for every driver).  Run `./trix --inspect examples/debugger_tour.trx` and follow the doc's walkthrough -- stepping, plain and conditional breakpoints, watches, frame navigation -- to find and fix the bug.  Outside the debugger it runs normally and prints the buggy three-zero output.

### [keytest.trx](keytest.trx) — Raw-mode keyboard smoke test

Manual tty-only check for the raw-mode keyboard surface: each keystroke prints the decoded Name produced by `lib/keys.trx` (arrows, function keys, modifiers); `q` quits, and Ctrl-C demonstrates the crash-safe restore handler returning the terminal to cooked mode.  On a non-tty it fails fast with `/io-read-error` rather than hanging.

### [screen-demo.trx](screen-demo.trx) — Screen subsystem guided tour

Four-page interactive tour of the virtual-screen layer: (1) type/introspection plus 256-color palette stripe and 6x6 cube, (2) SGR attribute gallery, (3) UTF-8 gallery (box-drawing, multi-script, truncation), (4) blit popup via save-region/restore-region.  Needs a real tty (raw-mode) and the repo root as cwd (relative `lib/` requires); non-tty invocation fails fast with `/io-read-error`.

## Sudoku specifics

### Puzzle catalog

Puzzles are stored in [sudoku-puzzles.txt](sudoku-puzzles.txt), one
block per puzzle:

```
## Display name
<row 1>
<row 2>
...
<row 9>
```

where each row is exactly nine characters: digits 1-9 for hints and
`.` for empty cells.  Comments with a single `#` and blank lines are
ignored.  Add your own puzzles by appending another block.

The shipped catalog contains **31 curated puzzles** spanning easy
(~6 ms) to extreme (~9 s).  Sources:

- 27 grids from [Project Euler problem 96](https://projecteuler.net/project/resources/p096_sudoku.txt)
  (chosen across the difficulty spectrum, grid IDs preserved)
- Wikipedia canonical Sudoku
- Qassim Hamza's oft-cited medium puzzle
- Arto Inkala's "AI Escargot" (2006) and his 2012 "world's
  hardest" -- both brutal for HUMAN solvers (forcing chains), yet
  merely slow for a backtracker.  The machine-hard mirror image, a
  17-clue grid constructed specifically to defeat brute-force
  scanners, lives in the stress file below: human-hard and
  machine-hard are nearly disjoint properties.

Complete run of all 31 takes ~29 seconds on the release binary.
For larger catalogs, point the script at your own file by copying
it over `sudoku-puzzles.txt`.

### Marching ants -- watching a long solve think

Any solve that runs past ~2 seconds starts printing a one-line
search status (`[thinking] 12.9 s   38912 tried (3006/s)   deepest
cell 71/81`), and past ~10 seconds the current **speculative grid**
every ~10 s: puzzle hints render solid, the candidate digits
currently under trial render ANSI-dim, unbound cells as `.`.  The
counters survive every backtracking rollback because they are
written with `def-persist` (and the clock cells are built in
`${...}` global VM) -- the same escape hatches the solution
extraction uses, doing double duty as live observability inside a
transactional search.  Short solves never reach the first interval,
so easy puzzles print exactly what they always did.

### Stress tests -- opt-in

Six grids live in
[`sudoku-stress-tests.txt`](sudoku-stress-tests.txt) rather than
the default catalog: three PE96 grids that exceeded the original
15-second budget (09, 13, 48), two more in the 13-14 s band (06,
14), and the **anti-brute-force 17-clue grid** from Wikipedia's
"Sudoku solving algorithms" article -- constructed so that an
ascending-value, left-to-right backtracker (exactly this solver)
must enumerate an enormous prefix space before the constraints
bite.  It did not finish in 2 minutes on the release binary;
expect a marathon, and watch it think the whole way via the
marching-ants display.  Naive backtracking hits its practical
ceiling on these puzzles; adding constraint propagation and
most-constrained-variable ordering would crack them in
milliseconds, but the example deliberately keeps the solver
mechanical to spotlight the logic primitives.

To run any stress test, copy its nine rows into
`examples/sudoku-input.txt` and run the solver normally -- the
file-override mechanism solves just that one puzzle.  Expect runs
of 30 seconds to hours; ctrl-C is always safe.  The 17-clue grid
needs `--save-depth=200` (see below); the others fit the default.

### Custom puzzle override

If a file `examples/sudoku-input.txt` exists, the solver replaces
its built-in catalog with just that one puzzle.  Format is
identical (9 lines of 9 chars, `.` for empty).

### Batch / non-interactive runs

The script prompts before each solve so you can try solving the
puzzle first.  To skip all prompts in one go, pipe blank lines:

```bash
yes '' | ./trix examples/sudoku.trx
```

### `--save-depth`

The solver nests one `choice` save level per empty cell, so a
puzzle needs about (empty cells + 2) save levels.  Every catalog
puzzle has 21+ clues and fits the default `--save-depth=64`
(verified: the full catalog completes bare-flag).  Only 17-clue
puzzles -- 64 empties, like the anti-brute-force stress grid --
exceed the default; run those with `--save-depth=200`.

## Supervised word-count specifics

### Corpus

Four text files in [word-count-corpus/](word-count-corpus/):

- `sonnet-18.txt`, `sonnet-29.txt`, `sonnet-73.txt` -- Shakespeare sonnets (public domain)
- `poison.txt` -- normal English text that embeds the single-word sentinel `POISONWORD`

The tokenizer lower-cases, drops empty tokens, and `precondition`-asserts that no token equals the sentinel.  The sentinel in `poison.txt` fires the assertion, which the pipeline turns into a `/require` error that kills the worker.

### Expected output

Something like:

```
  [worker] started
  [worker] started
  [worker] started
  [worker] started        <-- the restarted worker, after the supervisor observes /down
  [worker] processed examples/word-count-corpus/sonnet-18.txt
  [worker] processed examples/word-count-corpus/sonnet-29.txt
  [worker] processed examples/word-count-corpus/sonnet-73.txt

=== Supervised Word Count ===

Corpus       : 4 files
Workers      : 3
Unique words : 204
Elapsed time : ~15 ms

Top 10 words:
     12  and
     10  the
     ...
```

The poisoned file contributes no words to the merged result -- the pipeline crashes before `pipe-collect` returns, so no `/merge` message for `poison.txt` ever reaches the reducer.

## Reactive financial model specifics

### Graph shape

35 cells total: 13 base + 22 computed.  Four quarterly templates (gross-margin, operating-income, tax, net-income) are instantiated per quarter via `curry` -- a single proc template captures the quarter index.  Annual rollups sum the quarterly arrays on the operand stack (no loop-index `def`); annual gross-margin and operating-income derive from the annual line items rather than summing per-quarter totals directly, but the numbers come out identical because all the operators are linear.

### What-if via save / restore

Each scenario is bracketed by `save` / `restore`.  The scenario proc mutates any combination of base cells; `cell-set` journals each write; on `restore`, every journaled cell value reverts to its pre-save state.  Computed cells re-evaluate lazily on the next `cell-get` and produce the restored values.  The final baseline print matches the initial baseline character-for-character -- three back-to-back what-ifs leave no residue.

This composition is the piece few other reactive systems offer: you can script multiple exploratory mutations, inspect each result, and roll the whole graph back without bookkeeping or defensive copying.

### Why `batch`

Each scenario wraps its `cell-set` calls in `batch { ... }`.  Outside a batch, when a base cell changes, downstream watchers fire *before* the dependent computed cells recompute, so the watcher sees `(old, stale-cached-old)` instead of `(old, new)` and the zero-crossing check can't detect the transition.  Inside a batch, `@batch-end` forces re-evaluation of every dirty watched cell first, then `@batch-fire` compares pre-batch old against true post-batch new and fires watchers correctly.

### Watcher semantics

The zero-crossing watcher is a single proc curried with a per-cell label, registered on every quarterly and annual net-income.  It receives `old-value new-value` on the operand stack; the test is `(old >= 0) xor (new >= 0)`.  Because Trix's `cell-set` skips the write entirely when new equals old, repeated identical writes do not trigger spurious watcher calls.

## Village save-anywhere specifics

### Run from the project root

The master process uses `shell` to invoke `./trix --image /tmp/village-snapshot.img` for each timeline, so the example assumes the working directory is the project root (same assumption the other examples make for their `examples/...` asset paths).

### How "fork the timeline" works

1. Master boots the village, simulates 6 AM - 8 AM, and calls `snap-shot` to `/tmp/village-snapshot.img`.
2. After snap-shot, the master-or-child branch check looks for `/tmp/trix-scenario.txt`.  Master just wrote *nothing* there yet -- master branch taken.
3. Master writes `festival` to the scenario file, then `shell`s out to `./trix --image /tmp/village-snapshot.img`.  The child process thaws the image; its execution resumes at exactly the post-snap-shot branch check; the scenario file NOW exists, so the child takes the child branch, reads the scenario name, sets the global `/scenario`, and runs the clock from 8 AM - 10 AM.  Each NPC's body has festival-timeline events guarded by `scenario (festival) eq`.
4. Master prints the captured stdout, overwrites the scenario file with `rainstorm`, and shells again.  Second child thaws the same image (untouched by first child -- snap-shot files are write-once), runs its scenario, prints its trace.
5. Master deletes both temp files and stops its own NPCs.

### What survives snap-shot

All of it.  The NPCs are parked in `actor-recv` when snap-shot fires, with pending mailbox state and active dictionary stacks.  The `bread-count` cell's watcher list is in VM heap.  The supervisor's link to its children is captured.  The RNG state is captured.  The scanner's position inside `examples/village.trx` is captured (that's why the child's execution resumes at the branch check rather than at the top of the script).

### Why this is hard in Lua

Lua coroutines can yield but cannot be serialized.  A Lua save-game has to hand-reconstruct every coroutine's position after load, which in practice means designing every NPC AI as a state machine the engine can step explicitly.  Trix gives you the same mid-coroutine resumption as a primitive.

## Markov generator specifics

### Corpus

The corpus is the opening two chapters of Pride and Prejudice by Jane Austen, taken from [Project Gutenberg eBook #1342](https://www.gutenberg.org/ebooks/1342) (public domain in the United States and most other jurisdictions).  The file `markov-corpus/pride-ch1-2.txt` is a lightly cleaned plain-text slice: smart quotes and em-dashes normalized to ASCII, illustration blocks and italic underscores stripped, everything else preserved verbatim.  ~8.8 KB yielding ~1,795 tokens after tokenization (words plus `.`, `?`, `!` as pseudo-tokens).

### Why a bare `pipe-buffer` (no pipe-map/run)

The [supervised-word-count](supervised-word-count.trx) showcase demonstrates the `pipe-map` / `pipe-filter` / `pipe-collect` pipeline DSL, which composes stages into a coroutine tree and drives them with a terminal operator.  Markov deliberately uses only the raw `pipe-buffer` primitive plus a plain `coroutine-launch` on the generator side -- no stage DSL, no terminal operator -- to make clear that a direct coroutine yielding via a capacity-1 buffer is a perfectly ordinary use of Trix concurrency, not a thing that requires the pipeline library.  The buffer is the only synchronization between generator and consumer.

### Determinism

`rng-seed rand-seed` is set once before the generator coroutine launches, so the same corpus and seed produce bit-exact identical output across runs.  Change `rng-seed` (top of the script) to get a different sample.  Stopping is governed by two conditions: a hard ceiling at `sample-tokens` tokens, and a preferred stopping point at the first sentence-final punctuation after `sentence-stop-min` tokens.  The shipped defaults produce output that ends cleanly on a `.` or `?`.

### Abbreviation quirk

"Mr.", "Mrs.", "Mr. Bingley" etc. tokenize as `mr`, `.`, `bingley` because the preprocessor treats every `.` as a sentence-final pseudo-token.  The generator therefore occasionally emits phrases like "mrs. bennet" where the "." is actually an abbreviation rather than a sentence end.  This is authentic Markov-chain behavior; fixing it would require a heuristic like "trailing period keeps attached when followed by a capitalized word", which is out of scope for the showcase.

## Minidb specifics

### SQL surface (small on purpose)

```
CREATE TABLE <name> (<col> INT|TEXT|BOOL, ...)
INSERT INTO <name> VALUES (<lit>, ...)
SELECT *|<col>,... FROM <name>
    [WHERE <cond> [AND <cond>]] [ORDER BY <col>] [LIMIT <n>]
BEGIN / COMMIT / ROLLBACK
.schema [<name>]    .dump <name>    .quit
```

Types are `INT`, `TEXT`, `BOOL`.  WHERE supports `=` / `<` / `>` and `AND`.  No NULL, no joins, no GROUP BY.  The first column of every table is the primary key.

### Transactions: why ROLLBACK is the only interesting half

Trix `save` captures every mutation made afterward (dict `put`, record field bumps, array append) until the matching `restore` reverts them.  There is no separate "commit" primitive -- committing just means dropping the save token; the save level stays open but inert until something below it does `restore`.  So:

- `BEGIN` runs `save` and pushes the save token (an inline Integer) on an internal `tx-stack`.  Tx depth prints so you can see nesting.
- `ROLLBACK` pops the top token and calls `restore`.  Every `CREATE TABLE`, `INSERT`, or row mutation performed since the `BEGIN` is rolled back atomically -- the table registry, the per-table rows dict, and the primary-key index are all journaled.
- `COMMIT` pops the top token and discards it.  The mutations stay; the save level remains open (no token, no rollback, no journal compaction) until process exit.  The demo prints `[Trix note: no-op; see comment.]` on COMMIT to make this explicit.

The demo includes a BEGIN / INSERT / SELECT-inside-tx / ROLLBACK / SELECT-after-rollback round-trip.  The post-ROLLBACK select prints `(0 rows)` -- the inserted row is genuinely gone, not merely hidden.

### Indexed vs. scanned plan

The executor looks for exactly one pattern -- `WHERE <pk-col> = <literal>` -- and short-circuits to a single `known-get` on the rows dict when it matches.  All other WHERE shapes (range comparisons, AND combinations, predicates on non-PK columns) walk the rows dict via `for-all`.  The chosen plan prints as `PLAN: indexed lookup on <col>=<val>` or `PLAN: full table scan` before the result, so the demo can showcase both paths side-by-side.

### Stdin REPL

After the built-in demo finishes, the script keeps reading statements from stdin via `read-line` until EOF; a REPL banner prints only when stdin is an interactive tty (redirected input is consumed silently).  Redirect a file in (`./trix examples/minidb.trx < my-sql.txt`) or type Ctrl-D at an interactive prompt to exit.  `.quit` also ends the session early.

## Regex specifics

### Supported regex surface (small on purpose)

```
literal char     a, b, c, ...
.                any byte
\d \w \s         digit, word, whitespace
\D \W \S         negated versions
\\ \. \* ...     escaped metacharacters as literals
[...] [^...]     character class with ranges
?  *  +          zero-or-one, zero-or-more, one-or-more
|                alternation
(...)            grouping
^  $             anchors (start, end of input)
```

No backreferences, no lookaround, no lazy quantifiers, no Unicode beyond ASCII.  Equivalent power to a regular language -- which is precisely what makes the Thompson construction applicable.

### Why the naive matcher returns position lists

Most introductory backtracking-regex tutorials use return-first-match recursion (with continuation-passing for the cat-then-rest plumbing).  The list-of-positions formulation in this showcase trades a tiny constant factor for a much shorter implementation: `/cat l r` is `flatmap (match-at l) (match-at r)`, and `/alt l r` is `(match-at l) ++ (match-at r)`.  The exponential blow-up on nested quantifiers shows up *naturally* as the position list doubling at every `?` over an unsplittable input.

### Thompson construction notes

The classic fragment-and-patch builder.  Each `compile-X` returns a fragment record `[start dlist]` where `dlist` is an array of `[state-id slot]` dangling out-edges that the next layer patches.  /cat patches l's dangling to r's start; /alt builds a split state above two sub-fragments; /star patches its inner fragment back to its own split.  The full algorithm is on one page of Cox's article and translates almost line-for-line to Trix.

### Why the Pike VM uses Sets

A Trix Set gives `set-member?` and `set-add` in average-O(1), which is exactly what Pike's `addstate` needs to deduplicate epsilon-closures.  Without dedup, a pathological NFA can re-add the same state O(2^N) times per input position, defeating the whole point.  Each step builds a fresh `next` set; iterating the previous `cur` set walks every member exactly once.  Iteration order is deterministic enough for our purposes (we don't depend on it).

### Headline ReDoS pattern

`a?{n}a{n}` against `a^n`.  This is the canonical exponential pathology: the backtracker greedily matches every `a?` to an `a`, leaving none for the required `a{n}`, then backtracks one optional at a time.  At every layer of backtracking, the unmatched-required cascade re-explores deeper layers' choices -- 2^n total combinations.  Both engines find the (single) successful match: optionals all empty, requireds eat all n characters.  Pike finds it in O(n*m); naive does O(n*2^n).

### Running the demo

```
benchmark/release.sh                                         # one-time
benchmark/trix.rel --vm-size=64M examples/regex.trx
```

The VM bump is needed because the naive matcher's intermediate position arrays consume a few MB of VM heap at n=10 (default VM is 1 MB).  The cat-tree of `a?{10}a{10}` is 19 levels deep, comfortably inside the default 64-slot dict stack.  The Pike side comfortably fits in defaults at any n.

## Symbolic differentiation specifics

### What the simplifier knows

Each rule is a tag-pattern check at the root of the (already bottom-up-simplified) subtree:

```
0 + e   ->  e             e + 0   ->  e
e - 0   ->  e             0 - e   ->  -e
e + e   ->  2*e           e - e   ->  0
e + (-f) -> e - f         e - (-f) -> e + f

0 * e   ->  0             1 * e   ->  e             e * 1   ->  e
(-a)*(-b) ->  a*b         a*(-b)  ->  -(a*b)        (-a)*b  ->  -(a*b)
c1 * (c2 * e)  ->  (c1*c2) * e         (c1 * e) * c2  ->  (c1*c2) * e

e / 1   ->  e             0 / e   ->  0
e ^ 0   ->  1             e ^ 1   ->  e             0 ^ e   ->  0    1 ^ e   ->  1

--e     ->  e             -0      ->  0             -num    ->  fold
constant fold of any same-type (int/int or float/float) /add /sub /mul
```

The walker is bottom-up + apply-rules-at-root, then the outer loop calls itself again until the AST stops changing (structural equality via `ast-eq?`).  Most expressions converge in 2-4 passes.

### Why direct tagging instead of `/binop [op l r]`

The plan briefly considered `/binop` wrapping each operator under one outer tag.  Rejected after sketching the simplifier: every rule would become `tag-name == /binop && payload[0] == /+` instead of a single `tag-name == /add`, which doubles the dispatch noise *and* makes the rules-table harder to read at a glance.  Direct tagging is the showcase point.

### Sample run

```
$ ./trix examples/symdiff.trx

  f(x) = x ^ 2
    raw  : 2*x^1*1
    simp : 2*x

  f(x) = 3 * x ^ 2 + 2 * x + 1
    raw  : 0*x^2 + 3*(2*x^1*1) + (0*x + 2*1) + 0
    simp : 6*x + 2

  f(x) = exp(-x ^ 2)
    raw  : exp(-x^2)*-(2*x^1*1)
    simp : -(exp(-x^2)*(2*x))

  f(x) = log(1 + x ^ 2)
    raw  : (0 + 2*x^1*1)/(1 + x^2)
    simp : 2*x/(1 + x^2)
```

The "raw" derivative is what falls out of the differentiation rules with no cleanup.  The "simp" form is after the simplifier reaches a fixpoint -- exactly the shape a textbook would write.

## Raycaster specifics

### Map format

The 32x32 grid is defined inline as 32 strings, each 32 bytes wide, at the top of the file.  ` ` (space) is empty floor; anything else is a wall whose tile byte is the glyph rendered for E/W faces (`#` stone, `H` brick, `=` panel, `+` pillar).  Edit the literal to reshape the dungeon -- the renderer reads the byte directly, so any printable ASCII works as a new wall character.

### Player state

Six fields in a mutable dict: `(x, y)` Real-typed position, `(dx, dy)` facing-direction unit vector, `(px, py)` camera-plane vector perpendicular to dir with length `tan(fov/2)` (~0.66, the classic 66-degree FOV).  This is the Lode Vandevenne representation; storing as a dict (not a Record) keeps per-frame updates as in-place puts.

### DDA single-ray cost

Each ray walks at most 64 grid cells before hitting a wall (the borders are walls).  At 80 columns x 30 fps the inner loop runs ~150 K iterations/sec.  No floating-point divisions inside the loop; one division per ray for the perpendicular-distance fish-eye correction.

### Three tile sets

`Y` cycles, or pick at startup with `--tiles=ascii|blocks|halfblock`:

- **`ascii`** (default): pure printable ASCII glyph ramps per tile, e.g. stone walls render along `#` -> `=` -> `-` -> `.` -> ` `.  Texture identity is encoded in the glyph itself.  Works in any terminal regardless of font or UTF-8 support; this is the showcase aesthetic that pairs with the rest of the examples.
- **`blocks`**: Unicode shade blocks `█ ▓ ▒ ░` (U+2588..U+2591) replace the per-texture glyph ramp.  Texture identity moves to FG colour (gray stone, red brick, cyan panel, yellow pillar).  Walls now look like solid surfaces instead of tile-character art.  Drop-in -- same renderer, same per-column DDA cost.
- **`halfblock`**: each terminal cell uses U+2580 ▀ with FG = upper sub-pixel colour and BG = lower sub-pixel colour, for an effective 2x vertical resolution.  `screen-rows * 2` virtual rows are computed per column; wall edges land at sub-cell precision, so the transitions between wall and ceiling/floor are visibly smoother.  Per-texture 5-step colour ramps replace the glyph ramps; the same offset = `side + dist-bucket` indexing applies.  Highest visual fidelity at the cost of needing a 256-colour-capable terminal.

### Two shading modes

Each tile has a 5-glyph brightness ramp (`#` -> `=` -> `-` -> `.` -> ` `, similar for `H` `=` `+`).  N/S walls (rays that crossed a Y-grid line) shift one step lighter, suggesting a fixed light direction.

- **Default** (`shading-mode = /none`): the side shift is the entire offset.  All walls render at full brightness regardless of distance; perspective alone communicates depth.  This is the classic ray-cast aesthetic and gives the cleanest ASCII output.
- **Distance shading** (`--distance-shading` or runtime `T`): perp-distance is bucketed (3 / 6 / 12 / >12) and added to the side shift, so far walls dim toward the floor and finally fade out.  This trades crispness for a stronger sense of depth.

The toggle is a single global (`/shading-mode`); `T` mid-game flips between the two modes for instant A/B comparison.

### Flythrough vs interactive

`--flythrough` runs a fixed keystroke sequence (~95 inputs over ~7 seconds) and exits.  It enters alt-screen and hides the cursor but does NOT call `raw-mode` -- so it works on non-tty stdouts.  Pipe it to a recorder:

```bash
asciinema rec rc.cast -c './trix examples/raycaster.trx --flythrough'
```

Interactive mode (no flag) needs a real tty for `raw-mode`; running it from a pipe fails fast with `/io-read-error` rather than hanging.

More examples land here as the project matures.
