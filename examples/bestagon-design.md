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

# Bestagon Wars — Game Design Document

`examples/bestagon.trx` — a hex-grid tactical wargame in 16-color ANSI
terminal. Advance-Wars-shaped mechanics on procedurally-generated
continents. Empire flavor. Trix logic-programming drives the tactical AI.

This document is the **source of truth** for Bestagon Wars. Every system,
table, formula, and shape it describes is locked. Implementation phases
read from this document; deviations require a new commit updating this
document first.

The design history — what was debated, what was considered and rejected,
why decisions landed where they did — lives in
`memory/plan_showcase_bestagon.md` (planner's working notes; not the spec).

---

## Table of contents

1. [Premise and locked decisions](#1-premise-and-locked-decisions)
2. [Game loop and turn structure](#2-game-loop-and-turn-structure)
3. [Map topology and hex coordinates](#3-map-topology-and-hex-coordinates)
4. [Terrain](#4-terrain)
5. [Buildings](#5-buildings)
6. [Units](#6-units)
7. [Combat](#7-combat)
8. [Commanding Officers](#8-commanding-officers)
9. [Power meter](#9-power-meter)
10. [Capture mechanic](#10-capture-mechanic)
11. [Fog of war](#11-fog-of-war)
12. [Economy](#12-economy)
13. [AI v1 (greedy heuristic)](#13-ai-v1-greedy-heuristic)
14. [AI v2 (logic-programmed)](#14-ai-v2-logic-programmed) — incl. §14.10 AI v3 (optional)
15. [Save and load](#15-save-and-load)
16. [Scenario file format](#16-scenario-file-format)
17. [Game state shape](#17-game-state-shape)
18. [UI, rendering, and controls](#18-ui-rendering-and-controls)
19. [Self-test invariants](#19-self-test-invariants)
20. [Scenario-overridable surface](#20-scenario-overridable-surface)
21. [Out of scope](#21-out-of-scope)
22. [Phase plan](#22-phase-plan)

---

## 1. Premise and locked decisions

### 1.1 Pitch

Bestagon Wars is a turn-based tactical wargame on procgen hex
continents. Two to four factions take turns moving units, attacking,
capturing buildings, and managing funds. Win conditions are
scenario-defined; the default win is to capture the enemy HQ or
eliminate all enemy units and buildings.

The game's signature design choices:

- **Hex topology** because hexagons are the bestagons.
- **AW-faithful mechanics** with three unit mergers for a leaner
  17-unit roster.
- **Trix-as-game-engine showcase**: the AI v2 tactical evaluator uses
  Trix's logic-programming primitives (`find-all`, `find-n`,
  `aggregate`, `once`) to enumerate and score candidate moves — the
  headline demonstration of what the language can do.
- **16-color ANSI terminal output** via Trix's screen ops; no PNG, no
  pixel-mode, no GUI.
- **Multi-line flat-top hex rendering** for visual clarity, paired
  with a minimap for whole-map situational awareness.

### 1.2 Genre positioning

Bestagon Wars sits at a specific intersection of turn-based-strategy
subgenres. This subsection names the games that define the design
space and locks which axes Bestagon aligns with vs. deliberately
departs from. Provenance for v1 decisions; provenance for v2
candidates lives in `memory/plan_showcase_bestagon.md`.

#### Primary heritage

- **Advance Wars** (AW1 / AW2 / AWBW). The foundational reference for
  combat math, economy, capture, fog, and CO archetypes. Bestagon §7
  is AW2/AWBW-canon-faithful as of the 2026-05-15 math audit (§7.1
  formula, §7.3 luck, §12.4 fuel rates, §12.5 stationed-only healing).
  Departures are labeled where they occur (§6 unit mergers, §7.4
  AA-vs-BCop merger consequence, §8 CO AW-analog notes).

#### Aligned peers (mine for v2)

- **Wargroove / Wargroove 2** (Chucklefish). Closest modern
  AW-spiritual successor. Two v2-worthy mechanics that Bestagon
  doesn't have: **per-unit Groove abilities** (every unit charges its
  own ability bar; tactical hooks finer than once-per-game CO Power)
  and **commanders-on-field** (CO is a unit on the board, not a
  passive force buff). Neither in v1.
- **Battle for Wesnoth** (open-source, ongoing). Hex-grid tactical
  with mature **veterancy** and **time-of-day alignment** systems.
  Veterancy is already in the v2 menu; time-of-day is the cleaner
  shape of the "weather" v2 candidate (20 years of balance precedent
  in Wesnoth's lawful/neutral/chaotic alignment system).
- **Tiny Metal** (Area 35). AW-homage that introduced **Focus Fire**
  (multiple attackers stacking on one target with a damage bonus).
  Slots into Bestagon's §7.1 formula trivially as a `focus_count`
  multiplier in v2 — meaningful positional depth at near-zero
  implementation cost.

#### Deliberate antithesis

- **Into the Breach** (Subset Games). Fully deterministic, enemy
  moves **telegraphed one turn ahead**, no luck, no fog. Built for
  chess-puzzle-feel; closer to a tactics puzzle than a wargame.
  Bestagon's `/luck-mode /on` opt-in and AW-style fog (locked decision
  15) explicitly reject this paradigm. If a v2 "puzzle mode" is ever
  considered, ITB is the reference for what determinism + telegraph
  actually buy in playtest.

#### Adjacent but out of scope (do not mine)

- **Fire Emblem** (Intelligent Systems). Character-driven tactical
  with permadeath. Bestagon units are **interchangeable
  type-instances**; permadeath here is structural (any unit can die
  any turn), not narrative. Different audience; informs no v2
  features.
- **XCOM** (Firaxis). Cover-based percentage combat with hit-chance /
  body-part UI. Bestagon's §18.6 damage-preview deliberately borrows
  XCOM's "predicted damage shown pre-confirm" UX, but the underlying
  combat paradigm (terrain stars + HP scaling) is AW-faithful, not
  cover-based. Surface UX reference only.
- **Polytopia / Civilization series** (Midjiwan, Firaxis). Empire-scale
  4X with economy + tech trees + era progression. Bestagon is
  **tactical** — single scenario per game, no tech, no era
  progression. The "campaign mode" v2 candidate references state
  carryover, not 4X structure.
- **Panzer General / classic hex wargames** (SSI lineage). Historical
  predecessor; informed AW's design. Worth knowing as the ancestor of
  the genre, but already digested via AW.

#### Positioning summary

| Axis | Bestagon position | Closest peer | Closest antithesis |
| --- | --- | --- | --- |
| Scale | Tactical (single scenario) | Wargroove | Civilization |
| Grid | Hex (flat-top + odd-q) | Wesnoth | XCOM (square) |
| Determinism | Luck-off default; luck-on opt-in | AW2/AWBW | Into the Breach |
| Information | Fog of war (AW model, locked 15) | AW | Into the Breach |
| Unit identity | Interchangeable type-instances | AW | Fire Emblem |
| Per-unit abilities (v1) | None — CO Powers only | AW | Wargroove |
| Economy | Funds + cities (AW canon) | AW | Polytopia |
| Win conditions | HQ capture OR elimination (scenario override) | AW | — |
| Audience | AW veterans + tactical-game players | Wargroove | Polytopia |

### 1.3 Locked decisions index

The full 39-row decision matrix. Each row is load-bearing; deviation
requires updating this document first.

| # | Decision | Choice |
| --- | --- | --- |
| 1 | Topology | Hex, **flat-top + odd-q offset coords**, 0-indexed top-left origin |
| 2 | Heritage anchor | Advance Wars mechanics, Empire flavor |
| 3 | Display medium | 16-color ANSI terminal via screen ops (no PNG, no pixel mode v1) |
| 4 | Hex render | Multi-line flat-top hex, ~5 lines × 7 chars per hex, odd-q brick-offset; minimap at 1 char/hex |
| 5 | Viewport | Scrolling — cursor-near-edge pans (non-optional given hex size) |
| 6 | Map sizes | Small 25×18, Standard 50×36, Epic 75×54 |
| 7 | Capture model | AW HP-capture (2 turns at full HP, scales with damage) |
| 8 | Indirect fire | Keep — core to AW tactical depth; can't move + fire same turn |
| 9 | Building types | 4: HQ, City, Factory, Port |
| 10 | Economy | Funds; 1000/turn per City + HQ |
| 11 | Production | Buy-each-turn from menu (AW model), no queued timers |
| 12 | Combat model | AW damage table × HP × terrain × CO × luck |
| 13 | Stacking | One unit per hex |
| 14 | ZOC | Light — moving into adjacent enemy halts further movement that turn |
| 15 | Persistent fog reveal | NO — AW model (only currently-in-vision; recon matters) |
| 16 | Win conditions | Capture HQ or eliminate all enemies (default); per-scenario overrides |
| 17 | Save/load | Custom `.bws` binary (game-state Dict via `to-binary-token`), auto-save end-of-turn; impl in TDD §10 (locked 2026-05-16) |
| 18 | AI v1 strategy | Greedy heuristic |
| 19 | AI v2 strategy | Logic-programmed tactical eval |
| 20 | Filename | `examples/bestagon.trx` (single file, scenarios in `examples/bestagon-scenarios/`) |
| 21 | CO Power meter | v1 — passives + Power meter + regular Power + Super Power |
| 22 | Unit cost canonical reference | AW1/AW2 (= AWBW community standard) |
| 23 | Unit mergers vs canonical AW roster | Anti-Air absorbs Missiles; Destroyer absorbs Cruiser; Battle Copter absorbs Transport Copter |
| 24 | Starting funds | 0 default; scenario can declare `/starting-funds N` per faction |
| 25 | Cost-table representation | Dict in global game-state (scenario-overridable) |
| 26 | CO roster (v1) | 6 archetypes: Bishop / Hammer / Hawk / Boots / Veil / Talon |
| 27 | Power-meter charge formula | `star_gain = (dealt_funds + received_funds × 0.5) / 5000 × (charge_rate / 100)`, cap 12 |
| 28 | Damage table | 17×17 sparse dict-of-dicts, AW2/AWBW canon + 3 merger adjustments |
| 29 | Scenario file format | Trix-source `.bw` files; everything-is-a-proc win conditions |
| 30 | Tutorial scenario content | `tutorial.bw`: 25×18 Bishop-vs-Hammer-AI-easy |
| 31 | Commit-group milestone sequencing | 14-17 commits across 9 milestones (revised by the SS22 phase plan from the original 9-12/8 estimate); M1 = Groups 1+2 combined |
| 32 | Game state shape | Logical schema in GDD §17; physical layout in TDD §3 (SoA `Packed<Byte>` cell grids, Dict-by-id units, SoA factions). Locked sub-decisions: **32-A2** = constants as Records (§17.5); **32-C** = per-cell occupant grid for O(1) cell→unit lookup (§14.9); **32-D** = no fog cache — recomputed on demand from `/units` + per-faction vision (§11.1). |
| 33 | Combat formula free variables | AW-canon HP-coupled terrain defense; additive integer `luck_pp` (0–9 pp) with `/off` default |
| 34 | AI v2 performance budget | Player-selectable tier: `/standard` (5s/10s/30s) or `/patient` (10s/20s/60s) |
| 35 | Hex-render Phase-0 readability gate | 162-combination validation matrix; iterate-until-pass (no fallback renderer per 2026-05-15) |
| 36 | Weather (v1) | Scenario-opt-in `/weather-cycle [...]`; advances once per game-day (not per faction turn); resolves through §8.4 mods |
| 37 | Unit veterancy (v1) | Per-unit `/kills` + `/vet-tier` 0-3; thresholds 3/5/8 kills; +5/+10/+15% atk |
| 38 | Coordinate type | `[q r]` 2-element UInteger arrays (q, r non-negative; bitwise-friendly for parity tests) |
| 39 | Minimum supported terminal | 120×40 (locked 2026-05-15 Phase 0.6C; §18.1 layout refined 2026-05-16: floating top/keybind + tall minimap column) |
| 40 | Unit count cap | Max 254 active units across all factions (byte unit-id + 255 sentinel per TDD §3.1); reject scenarios past this in `validate-scenario`; expansion path documented (locked 2026-05-16) |

---

## 2. Game loop and turn structure

### Per-turn flow (for the active faction)

1. **Start of turn.** Active-faction increments funds by
   `1000 × (HQ count + City count)`. Unit fuel decremented for air/sea
   units; ground unaffected. Per-unit `/has-moved`, `/has-fired`,
   `/has-captured` flags reset to false. Healing and refueling applied
   (see §12.5). Captured-buildings rolled in (already done at capture
   time; this step is a no-op).
2. **Player action loop.** Player issues actions one at a time. The
   **canonical unit action is composite move-then-act** (AW selection
   model): selecting a unit highlights its reachable-set; choosing a
   destination opens a per-destination submenu (Attack-from-here /
   Capture / Wait / Load / Unload / etc.); the entire compound
   resolves as a single committed action that sets `/has-moved` and
   (where applicable) `/has-fired` / `/has-captured` together. The
   bullets below describe the *resolution* of each composite outcome,
   not separate menu items.

   - **Move-only**: unit moves to a reachable hex (respects terrain,
     fuel, ZOC, occupant array); player chooses "Wait" at destination.
     Sets `/has-moved = true`.
   - **Move-then-attack**: unit moves to a reachable hex AND attacks
     an in-range target from that hex. Indirect-fire units cannot
     move + fire same turn (locked decision 8) — for them, the submenu
     omits Attack when the chosen destination differs from the start
     hex. Sets `/has-moved = true` AND `/has-fired = true`.
   - **Move-then-capture**: Inf/Mech moves onto an enemy or neutral
     building and chooses "Capture" at destination. Adds capture HP per
     turn equal to unit's current HP. Sets `/has-moved = true` AND
     `/has-captured = true`.
   - **Move-and-load**: moving an Inf/Mech onto a friendly transport
     (APC, Lander) hex loads automatically — no submenu.
   - **Unload**: a transport submenu offers "Unload" with adjacent
     legal hex as target; unloaded unit ends its turn (its `/has-moved`
     and `/has-fired` both true).
   - **Build a unit** at a Factory or Port (or HQ for ground+air) when
     funds permit. New unit appears on the building hex with
     `/has-moved = true` AND `/has-fired = true` (can't act on the turn
     it was built).
   - **Activate CO Power** when `floor(power-meter-stars) >= power-cost`.
     Sets faction `/power-active = true` for the remainder of the turn;
     clears the meter. Power activation is faction-scoped, not
     unit-scoped — issuable any time during the action loop.
3. **End of turn.** Player ends turn via UI. Engine:
   - Decrements capture progress on any building that had a capturing
     unit *not* end on it this turn.
   - Fuel-tick on air/sea units; units that hit 0 fuel crash (air) or
     sink (sea). Submerged subs consume 5 fuel/turn (§11.2).
   - Power-active AND super-active flags clear.
   - `/built-this-turn` flag clears on every unit owned by the active
     faction.
   - Submarine state-machine: subs that didn't attack this turn
     transition to `/submerged` (§11.2).
   - Weather cycle advancement (§11.5): if `state.turn.day` advanced
     this turn (i.e., the active faction was the last one in the
     day cycle), `/weather-cycle-idx` increments mod cycle length and
     `/weather-current` updates.
   - Win-conditions evaluated (each entry resolved to a proc, called
     with state+faction; first true wins).
   - Active-faction advances to next faction.
   - At start of new `/day`, increment `/turn.day` counter.
   - **Hotseat pass-the-keyboard** (if next faction is `/human` AND
     previous was `/human`): engine clears the main viewport and
     prompts `"<NextCO>'s turn — press Enter when ready"`. Prevents
     accidental info-leak between hot-seat humans (the previous
     player's view is hidden until the next player acknowledges).
     Single-player + AI turns skip this screen.

**Action atomicity** (locked): every player-issued action (move,
attack, capture, load, unload, build, power-activate) is **atomic
at commit-time**:

- Action preconditions (target alive, in range, fuel/ammo
  available, building hex empty, etc.) are validated at the moment
  the player confirms the action.
- If a precondition fails (e.g., target destroyed by an unrelated
  prior action this turn — not possible in v1 since one action at
  a time, but the rule generalizes), the action is rejected: no
  fuel/ammo consumed, no `/has-moved` set, no state change.
- An indirect-fire attack whose target was destroyed mid-turn by
  another own unit is a special case: the second attack's target
  validation fails → action cancelled cleanly. No "wasted ammo"
  side effect.
- Power activation is similarly atomic: if for some reason the
  meter dropped below cost between menu-open and confirm (e.g., a
  Super was activated by some other event — not possible in v1),
  the activation is rejected.

### Game loop pseudocode

```
loop:
    while game-status is /running:
        for faction in /factions:
            advance state to /turn.current-faction = faction.id
            start-of-turn(state, faction)
            run-faction-turn(state, faction)        % player or AI loop
            end-of-turn(state, faction)
            evaluate-win-conditions(state)
            if state.game-status is not /running: break
```

---

## 3. Map topology and hex coordinates

### Hex grid (flat-top, odd-q offset)

Flat-top hexes (top and bottom edges horizontal; points on left and
right). Odd-q offset coordinate system: each column is staggered
vertically relative to its neighbors; odd-q columns sit 2 lines lower
than even-q columns.

Coordinates are `[q r]` 2-element UInteger arrays, 0-indexed (locked
decision 38). UInteger components are non-negative by construction
and bitwise-friendly for parity tests (`$[ q & 1u ]` is the canonical
odd-q check); cube coordinates from `pos-to-cube` are signed Integers
(can be negative for hexes northwest of `[0 0]`).

- Top-left of a `W × H` map is `[0 0]`.
- Bottom-right is `[W-1, H-1]`.
- Even-q columns sit at "high" vertical position; odd-q columns sit at
  "low" position (2 lines lower in the render).
- The brick-stagger is **render-time only** as far as scenario files
  are concerned; the scenario map array is a rectangle of
  `H` rows × `W` columns of terrain characters.

### Adjacency (offset coords)

Adjacency depends on column parity:

| Direction  | Even-q neighbor offset | Odd-q neighbor offset |
| ---------- | ---------------------- | --------------------- |
| North      | `[q, r-1]`             | `[q, r-1]`            |
| South      | `[q, r+1]`             | `[q, r+1]`            |
| North-West | `[q-1, r-1]`           | `[q-1, r]`            |
| North-East | `[q+1, r-1]`           | `[q+1, r]`            |
| South-West | `[q-1, r]`             | `[q-1, r+1]`          |
| South-East | `[q+1, r]`             | `[q+1, r+1]`          |

Edge hexes (top row, bottom row, leftmost/rightmost column) have fewer
neighbors; out-of-bounds neighbors are filtered.

### Hex distance

Convert offset `[q r]` to cube `[x y z]` where `x + y + z = 0`. Distance
between cube `[x1 y1 z1]` and `[x2 y2 z2]`:

```
distance = (abs(x1-x2) + abs(y1-y2) + abs(z1-z2)) / 2
```

Cube-from-offset conversion for flat-top odd-q:

```
x = q
z = r - (q - (q & 1)) / 2
y = -x - z
```

### Map sizes

| Size     | Dimensions | Total hexes | Use case                          |
| -------- | ---------- | ----------- | --------------------------------- |
| Small    | 25 × 18    | 450         | Tutorial, quick skirmish          |
| Standard | 50 × 36    | 1800        | Default competitive match         |
| Epic     | 75 × 54    | 4050        | Long-form scenarios, 3-4 factions |

Map size is declared by the scenario via `/map-size [W H]`. The engine
imposes a sanity bound: `W, H ≤ 100`.

### Unit count cap (locked decision 40, 2026-05-16)

**Maximum active units across all factions = 254** at any one time.
Unit-ids are bytes (0-254); the value `255` is reserved as the "no
unit on this cell" sentinel in the per-cell `/unit-id` grid (TDD
§3.1).  The cap applies to live units only — units that die free
their id for re-use by the next spawn.

**Why 254 is enough for v1:**

| Map              | Hexes | Typical dense unit count | 4-faction worst case |
| ---------------- | ----- | ------------------------ | -------------------- |
| Small (Tutorial) | 450   | ~10-20                   | ~80                  |
| Standard         | 1800  | ~30-50 per faction       | ~200                 |
| Epic             | 4050  | ~40-60 per faction       | ~240                 |

The cap accommodates 4-faction Epic at canonical AW density with
headroom; `validate-scenario` rejects scenarios whose
`/starting-units` count exceeds the cap, and `unit-spawn` returns
`/unit-cap-exceeded` (TDD §3.2) when the build menu hits the
ceiling at runtime.

**Path to widening (v2 if measured):** moving from 254 to 512 (or
any value up to 65 535) requires touching exactly these sites; **no
gameplay code reads or writes unit-ids directly**, so the change is
small and localized:

1. **TDD §3.1 cell grid type**: change `/unit-id` from
   `Packed<Byte>` (`255b` sentinel) to `Packed<UShort>`
   (`65535us` sentinel) — or `Packed<UInteger>` if a wider type
   is preferred.  Other five 1-byte cell grids unchanged.
2. **TDD §3.2 `unit-spawn`**: bump the `/unit-cap-exceeded` check
   constant; rest of the chokepoint code is unchanged because it
   accesses unit-ids via the cell accessor procs (`cell-unit-id` /
   `cell-set-unit-id`).
3. **TDD §3.5 `/next-unit-id` slot**: widen from Byte to UShort.
4. **GDD §3 + decision 40 + §16 `validate-scenario`**: update the
   `254` literal and the capacity table.
5. **Save body format-version bump** (TDD §10.1 header byte): the
   widened `/unit-id` grid changes the on-disk shape of `/map`, so
   `BWS-FORMAT-VERSION` increments and saves from the old format
   become incompatible (orphan-rename UX per §15.2).

**Sites NOT affected** (verified 2026-05-16):
- Gameplay code never types unit-ids — they're opaque values
  passed through accessor procs (TDD §3.1, §3.2).
- `/units` Dict already keys by unit-id; Dict supports any
  hashable Integer key without code change.
- `/faction-units` Arrays store unit-id values; Array element type
  is dynamic, no code change.
- `/transport-cargo` Arrays: same as `/faction-units`.
- Combat / fog / pathfind / AI / render reads unit-ids via
  `cell-unit-id` and `/units uid get`; both work without
  modification.
- Cheats / debug output: format strings render unit-ids with
  `{:d}` which works for any integer width.

If a real scenario in M3-M9 development demands more than 254 live
units, the bump is a single-commit change.  Until then, the Byte
cap stays — YAGNI applied to capacity headroom (the other five
cell grids' value sets don't benefit from a wider type either).

### 3.4 Procedural map generation

Engine ships a procedural map generator for skirmish play, de-risked
by Phase 0.7 prototypes (commits `cf72a97` terrain-only +
`a0acb98` building scatter). Eight-phase pipeline:

1. **Random-fill** at `/sea-pct` probability (white noise sea/land).
2. **Cellular-automata smoothing** — 5 iterations of "land if ≥ 4 of 6 neighbors are land" carves organic continent shapes.
3. **Mountain cluster-spawn** — N seeds × random-walk produce ranges, not scattered peaks.
4. **Forest cluster-spawn** — M seeds × random-walk produce woods, skipping mountain cells.
5. **Beach auto-placement** — any land cell adjacent to sea (NOT mountain) becomes beach.
6. **Symmetry transform** — `/mirror-h` reflects across the vertical midline; `/rotational-180` rotates 180° about the center; `/none` is asymmetric.
7. **Connectedness flood-fill** — every land cell must be mutually reachable; small islands pruned to sea (Phase 1 invariant `procgen-connected`).
8. **Empire-style building placement** — Inspired by *Empire: Wargame of the Century*: 2 HQs at max mutual hex-distance + ≥3 from edges, then neutral cities + factories + ports scattered at the density-preset rate with 1-hex spacing between buildings. Symmetric maps halve placement targets and mirror for fairness.

**Knobs** (engine reads from `/procgen-spec` block in `/scenario` or CLI flag overrides):

| Knob                 | Type  | Default        | Meaning                                                 |
| -------------------- | ----- | -------------- | ------------------------------------------------------- |
| `/seed`              | ulong | (current-time) | RNG seed for deterministic regeneration                 |
| `/sea-pct`           | int   | 30             | Target sea coverage % (post-CA stabilises near initial) |
| `/mountain-clusters` | int   | 3              | Number of mountain ranges                               |
| `/forest-clusters`   | int   | 5              | Number of forest woods                                  |
| `/cluster-size`      | int   | 8              | Random-walk steps per cluster                           |
| `/ca-iters`          | int   | 5              | Cellular-automata smoothing passes                      |
| `/symmetry`          | name  | /none          | `/none` \| `/mirror-h` \| `/rotational-180`             |
| `/density-preset`    | name  | /default       | `/sparse` \| `/default` \| `/dense` (per table below)   |
| `/hq-edge-buffer`    | int   | 3              | Minimum hexes between HQ and map edge                   |
| `/n-factions`        | int   | 2              | Number of HQs to place (one per faction)                |

**Density presets** (per 1000 land hexes, post-prune):

| Preset     | Cities | Factories | Ports |                        |
| ---------- | ------ | --------- | ----- | ---------------------- |
| `/sparse`  | 15     | 5         | 3     |                        |
| `/default` | 30     | 10        | 5     | (Empire-canon density) |
| `/dense`   | 50     | 20        | 10    |                        |

**Perf budget** (prod build, default 1 MB VM, mean over 10 seeds):

| Map size       | Mean gen time |
| -------------- | ------------- |
| Tutorial 25×18 | ~50 ms        |
| Standard 50×36 | ~190 ms       |
| Epic 75×54     | ~440 ms       |

Well under "1 s for user to cycle through seeds" UX budget.

**Reference implementation**: `examples/bestagon-proto/proto-mapgen.trx`. Phase 1's `bestagon.trx` graduates this in-place.

**Procgen + section 7.3 luck**: procgen seed (RNG state for map gen) is independent from `/rng-state` (combat-roll RNG state). Both share the `/seed` if `/seed` is the only knob set; scenarios may declare both via separate `/procgen-seed` and `/rng-seed` if reproducibility of either independently matters.

---

## 4. Terrain

Nine terrain types. Each has a movement-cost row keyed by unit class,
a defense star rating, fog modifiers, and render attributes.

### Terrain table

| Type | Defense ★ | Move cost (foot/wheel/tread/air/sea) | Fog effect | Render fg/bg |
| --- | --- | --- | --- | --- |
| Plain | 1 (2 inf) | 1/2/1/1/— | none | green `.` on green-bg |
| Road | 0 | 1/1/1/1/— | none | yellow `=` on tan-bg |
| Forest | 2 (3 inf) | 1/3/2/1/— | hides ground in fog | bright-green `*` on green-bg |
| Mountain | 4 inf only | 2/—/—/1/— | +3 vision to infantry | white `^` on dark-gray-bg |
| River | 0 | 2 inf-only / — / — / 1 / — | none | cyan `~` on blue-bg |
| Bridge | 0 | 1/1/1/1/— | none | yellow `=` on cyan-bg |
| Sea | 0 | — / — / — / 1 / 1 | none | blue `~` on dark-blue-bg |
| Reef | 1 | — / — / — / 1 / 2 | hides naval in fog | cyan `:` on dark-blue-bg |
| Beach | 0 | 1/2/1/1/1 | none | tan `_` on tan/blue boundary |

### Movement classes

| Class | Member units                                      |
| ----- | ------------------------------------------------- |
| foot  | Infantry, Mech                                    |
| wheel | Recon, APC, Anti-Air, Artillery, Rocket           |
| tread | Tank, Heavy Tank                                  |
| air   | Fighter, Bomber, Battle Copter                    |
| sea   | Lander, Destroyer, Submarine, Battleship, Carrier |

`—` = impassable for that class.

### Defense-stars interaction

The `def_terrain_stars × def_HP` term of `def_total` in the combat
formula (§7.1) consumes these stars. A 4-star Mountain at full
defender HP reduces incoming damage by 40 percentage points; at 1 HP
it reduces by 4 percentage points (AW-canon HP scaling). The inf-only
column applies when an Infantry or Mech is the defender on terrain
where their class-specific value applies (Plain 2★, Forest 3★,
Mountain 4★ infantry-only).

### Fog modifiers

- **Forest**: ground units inside forest are invisible to enemy units
  more than 1 hex away. Adjacent enemies see the unit.
- **Mountain**: infantry stationed on mountain has vision +3 (AW canon).
  Mech identical to Inf.
- **Reef**: naval units inside reef are invisible to enemy units more
  than 1 hex away.
- **Submarine**: submerged subs are invisible to all enemies; only
  Destroyers within range 1 can detect them.

### Render

Terrain renders as a background color fill + a center icon within the
multi-line hex outline (§18). Faction units overlay terrain at render
time.

---

## 5. Buildings

Four building types. Each is owned by a faction (or `-1` for neutral), occupies
a single hex, generates income or services, and is capturable by enemy
Infantry/Mech.

### Building table

| Glyph | Building | Income/turn | Heals at start-of-turn | Builds units | Capturable | Special |
| --- | --- | --- | --- | --- | --- | --- |
| `H` | HQ | 1000 | Stationed ground OR air +2 HP | Ground + air (locked v1) | Yes | Loss = game over for owner; air-heal mirrors AW Airport role (v1 has no separate Airport, locked decision 9) |
| `c` | City | 1000 | Stationed ground +2 HP | — | Yes |  |
| `F` | Factory | 0 | Stationed ground +2 HP | Ground units only | Yes |  |
| `P` | Port | 0 | Stationed naval +2 HP / refuel | Naval units only | Yes |  |

Notes:

- HQ doubles as a ground-unit producer in v1 (locked decision 9 with the
  v1 simplification that Air units build at Factories AND HQs; no
  separate Airport).
- Income from HQ + City summed at start-of-turn (locked decision 10).
- "Heals stationed" applies to a unit ending its turn ON the building
  hex (AW-canon healing model; see §12.5). Adjacent units do not heal.
  Carrier is the one exception — adjacent Fighters, Bombers, AND
  Battle Copters heal +2 because refueling/repair is the Carrier's
  core role.
- **HQ doubles as air-heal** in v1 (no separate Airport per locked
  decision 9): stationed Fighter/Bomber/BCop heals +2 HP at SOT,
  same as ground units stationed on HQ.  City and Factory still heal
  ground only.

### Faction-tint render

Buildings render with a fg+bg color combination that encodes both type
(letter glyph) and owner (color tint). Neutral buildings use gray.

| Owner state | Glyph color    | Background      |
| ----------- | -------------- | --------------- |
| Owned       | bright faction | faction-bg      |
| Neutral     | gray           | dark-gray       |
| Enemy       | faction-fg     | dark-faction-bg |

---

## 6. Units

Seventeen units, organized by domain. Each has a glyph, cost, HP, move
range, fuel, ammo, vision, and combat class.

### Ground units (9)

| Glyph | Unit | Cost | HP | Move | Fuel | Ammo | Vision | Class | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `i` | Infantry | 1000 | 10 | 3 | 99 | — | 2 | direct | Captures; river-crossing |
| `m` | Mech | 3000 | 10 | 2 | 70 | 3 | 2 | direct | Captures; anti-vehicle bazooka (see note) |
| `r` | Recon | 4000 | 10 | 8 | 80 | — | 5 | direct | Fog scout |
| `u` | APC | 5000 | 10 | 6 | 70 | — | 1 | — | Transports Inf/Mech; resupplies adjacent (§12.6) |
| `t` | Tank | 7000 | 10 | 6 | 70 | 9 | 3 | direct | Main MBT |
| `T` | Heavy Tank | 16000 | 10 | 5 | 50 | 8 | 1 | direct | Best direct-fire ground unit |
| `α` | Anti-Air | 8000 | 10 | 6 | 60 | 9 | 2 | direct | Hits air + soft ground |
| `a` | Artillery | 6000 | 10 | 5 | 50 | 9 | 1 | indirect | Range 2-3; can't move + fire same turn |
| `R` | Rocket | 15000 | 10 | 5 | 50 | 6 | 1 | indirect | Range 3-5 |

**Mech ammo** (locked simplification, fallback % retuned 2026-05-16):
the `Ammo 3` column above is the bazooka count (vs vehicles). When
bazooka ammo is exhausted, the Mech falls back to an unlimited
machine-gun at **15% of bazooka-row damage** from §7.4 — applies to
anti-vehicle and anti-air entries (line: `/mech /tank 55` becomes
effective 8 with bazooka empty; on Plain that's `8 × 1.0 × 0.90 ×
1.0 ≈ 7%`, ~0-1 HP — closer to AW canon's ~5%).  Anti-infantry
damage (Mech vs Inf 65, vs Mech 55) is machine-gun based and
unaffected by bazooka depletion. Stored as a single integer in
`/ammo`; the fallback math is hard-coded in the damage resolver.

**Ammo `—` notation** (locked): an `Ammo —` entry (Infantry,
Recon, APC, Lander, Carrier) means the unit has **no ammo
bookkeeping**.  Inf and Recon attack with an unlimited machine-gun
(never decrements; the §12.4 "0 ammo = can't attack" rule does not
fire for them).  APC, Lander, and Carrier don't attack at all.
Stored as `/ammo null` in the unit state (§17.4).

**Anti-Air glyph fallback** (locked 2026-05-16): `α` is the
preferred glyph. ASCII-strict fallback is **`A`** (uppercase). The
fallback was previously documented as `^` but conflicts with
Mountain's `^` terrain glyph (§4); `A` was chosen as it's
unambiguously a unit (no terrain or building uses uppercase A).

### Air units (3)

| Glyph | Unit | Cost | HP | Move | Fuel | Ammo | Vision | Class | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `f` | Fighter | 20000 | 10 | 9 | 99 | 9 | 5 | direct | Air-superiority |
| `b` | Bomber | 22000 | 10 | 7 | 99 | 9 | 3 | direct | Anti-ground |
| `h` | Battle Copter | 9000 | 10 | 6 | 99 | 6 | 3 | direct | Carries 1 Inf/Mech; effective move 3 when carrying (§12.7) |

### Sea units (5)

| Glyph | Unit | Cost | HP | Move | Fuel | Ammo | Vision | Class | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `L` | Lander | 12000 | 10 | 6 | 99 | — | 1 | — | Transports 2 ground units |
| `d` | Destroyer | 18000 | 10 | 8 | 60 | 8 | 5 | direct | Anti-sub + anti-air (Cruiser merge) |
| `s` | Submarine | 20000 | 10 | 5 | 60 | 6 | 5 | direct | Hidden when submerged |
| `B` | Battleship | 28000 | 10 | 5 | 99 | 9 | 2 | indirect | Range 2-6 |
| `C` | Carrier | 30000 | 10 | 5 | 99 | — | 4 | — | Refuels/repairs adjacent air (no attack v1) |

**17 units total.** Costs anchored to AW1/AW2 canon (= AWBW community
standard); locked decision 22.

### Unit mergers vs canonical AW

Three AW units merged into our 17-unit roster (locked decision 23):

| AW unit omitted  | Absorbed into | De-merger cost (if scenario wants restored) |
| ---------------- | ------------- | ------------------------------------------- |
| Missiles         | Anti-Air      | 12000                                       |
| Cruiser          | Destroyer     | AW1/2: 18000; AWDoR: 16000                  |
| Transport Copter | Battle Copter | 5000                                        |

### Cost dict literal (Phase 3-ready)

```trix
% Unit costs — AW1/AW2 canon.  Scenarios MAY override via /unit-cost-overrides.
/unit-costs <<
    /infantry      1000    /mech         3000    /recon        4000
    /apc           5000    /tank         7000    /heavy-tank  16000
    /anti-air      8000    /artillery    6000    /rocket      15000
    /fighter      20000    /bomber      22000    /battle-copter 9000
    /lander       12000    /destroyer   18000    /submarine   20000
    /battleship   28000    /carrier     30000
>>#r def
```

DoR/DS variants kept as reference for scenario authors:

```
Infantry:  AWDoR 1500.   Mech:       AWDoR 2500.   Tank:        AWDoR 6000.
Anti-Air:  AWDoR 7000.   Lander:     AWDoR 10000.  Destroyer:   AWDoR 16000.
Battleship: AWDoR 25000. Carrier:    AWDoR 28000.  Fighter:     AWDoR 18000.
```

---

## 7. Combat

### 7.1 Combat formula

AW-faithful (matches AW2 / AWBW canon up to notation):

```
actual = ((base + luck_pp) × atk_CO_mult) × def_factor × (atk_HP / 10)

  luck_pp     = 0                          if /luck-mode /off
              = uniform_int(0, 9)          if /luck-mode /on
  atk_CO_mult = 1 + sum_atk_pct / 100
  def_factor  = 1 − def_total / 100
  def_total   = sum_def_pct + def_terrain_stars × def_HP   % percentage points
```

**Component derivations:**

- **`base`** — looked up from the damage table (§7.4) on attacker and
  defender unit types. Missing entry = attack illegal.
- **`luck_pp`** — additive **integer percentage points** rolled in
  `[0, 9]` per attack when `/luck-mode /on` (§7.3); 0 when `/off`.
  Added to base damage BEFORE multiplications, so weak attacks see
  proportionally larger variance — chip-damage tension matches AW.
- **`atk_CO_mult`** — `(1 + sum_atk_pct / 100)`, where `sum_atk_pct` is
  the additive sum of every applicable attacker-side `/all-atk`,
  `/direct-atk`, `/indirect-atk`, `/foot-atk`, `/vehicle-atk`,
  `/air-atk`, `/ground-atk`, `/vs-revealed-atk` (CO passives +
  Power-if-active + Super-Power-if-active) PLUS the attacker's
  **veterancy bonus** `attacker.vet-tier × 5` (§7.8). Inapplicable
  keys contribute 0.
- **`def_factor`** — `(1 − def_total / 100)`, where `def_total` is in
  **percentage points** and combines two sources:
  - **`sum_def_pct`** — additive sum of every applicable defender-side
    `/all-def` (passives + Power-if-active). Inapplicable keys
    contribute 0. **Does not scale with defender HP** (a CO def bonus
    is bodyguard / training, not terrain).
  - **`def_terrain_stars × def_HP`** — terrain stars (§4 base + any
    `/forest-def` / `/reef-def` matches at the defender's tile),
    multiplied by the defender's current display HP (1-10).
    **This is the canonical AW wounded-defender effect**: a 1-HP
    defender retains only 1/10 of their terrain bonus, so wounded
    units in cover are progressively easier to dislodge.
- **`atk_HP / 10`** — attacker's current display HP as a fraction of
  full HP. Wounded attackers deal proportionally less damage.

**Aggregation rule** (locked):

- Within a category (atk pct, def pct), modifier percents are
  **additive**.
- Between categories (`(base + luck) × atk_CO_mult × def_factor ×
  atk_HP/10`), factors are **multiplicative**.
- Inapplicable modifier keys contribute 0 to their category sum.

**Rounding rule** (locked): `actual` is truncated to integer HP damage
via **floor** (e.g., 6.6 → 6, 6.9 → 6). Defender HP floored at 0 (unit
destroyed at 0 HP).

### 7.2 Wounded-defender behavior (HP-scaled terrain)

The terrain term `def_terrain_stars × def_HP` is the only HP-dependent
term in the defense formula. The table below shows the effective
defense reduction on a 4-star Forest at various defender HP levels:

| def_HP | def_terrain_stars × def_HP (pp) | def_factor | base damage taken at 55% |
| ------ | ------------------------------- | ---------- | ------------------------ |
| 10     | 40                              | 0.60       | 33.0 → 3 HP              |
| 8      | 32                              | 0.68       | 37.4 → 3 HP              |
| 5      | 20                              | 0.80       | 44.0 → 4 HP              |
| 3      | 12                              | 0.88       | 48.4 → 4 HP              |
| 1      | 4                               | 0.96       | 52.8 → 5 HP              |

A full-HP 4-star Forest defender takes 33% of base; a 1-HP defender
on the same tile takes 53%. That ~20-pp swing is the "focus-fire
wounded units" tactical pressure. Compare to plain terrain (§4:
1 star for non-inf, 2 for inf): a full-HP non-inf defender contributes
10 pp; a 1-HP defender contributes 1 pp — only a ~9-pp swing. Open
ground is fragile by design; wounded units in cover (Forest, Mountain,
Reef) gain real survivability that wounded units in the open do not.

`atk_HP / 10` continues to penalize wounded *attackers* symmetrically;
a wounded attacker hitting a wounded defender on terrain remains
roughly symmetric overall.

### 7.3 `luck_pp` (two-mode system, locked decision 33)

| Mode   | Formula                       | Default for                                |
| ------ | ----------------------------- | ------------------------------------------ |
| `/off` | `luck_pp = 0` (deterministic) | tutorial.bw + scenarios that don't declare |
| `/on`  | `luck_pp = uniform_int(0, 9)` | Scenarios that declare `/luck-mode /on`    |

`luck_pp` is an integer in `[0, 9]` rolled fresh per attack, added to
base damage in percentage points BEFORE multiplications. This matches
AW2 / AWBW canon "good luck" exactly.

When `/luck-mode /on`:

- Game-state gains an Integer `/rng-state` field, seeded at game start
  from `current-time` or scenario-declared `/rng-seed`.
- Each combat roll advances RNG state.
- Save/load preserves RNG state via `def-persist`.

When `/luck-mode /off`, `/rng-state` is absent from game-state. Combat
is a pure function of state.

**Replay determinism**: replay log records actual damage values
(`/dealt-hp`, `/received-hp` per combat event), not formula inputs.
Replay re-applies recorded damage; doesn't recompute the formula.

> **PENDING Phase 5.5A + 5.5E re-measurement** (filed 2026-05-15
> from Phase 0.5D luck-variance sim): 11 cells in the §7.4 damage
> matrix produce ~30% P(one-shot at full HP) under `/luck-mode
> /on` — the top hits are Bomber vs Inf, HTank vs Inf/Mech, AA vs
> BCop. The stub sim measured pin-hot favorable conditions
> (attacker can-see, full HP, Plain terrain). Real-game one-shot
> frequency depends on (a) how often the attacker gets unobstructed
> shots at full-HP soft targets in fog/open ground, (b) AA presence
> as the AW-canon counter to Bomber/HTank. Phase 5.5A (CO-matchup
> sim, 36 pairings on real Standard map) + Phase 5.5E (air-
> superiority sim) will re-measure; decide one of: **accept
> current ranges** / **clamp luck_pp to [0, 4]** (halving max
> roll-up) / **drop the 11 base-105+ cells to 100**.

### 7.4 Damage table (17×17 sparse, locked decision 28)

Rows are attackers, columns are defenders. Values are base damage %.
`—` = attack illegal.

```
                 INF MEC REC APC TNK HTK AA  ART ROC FGT BMB BCP LND DST SUB BSP CAR
Infantry          55  45  12  14   5   1   5  15  25  —   —    7  14   5   1   1   1
Mech              65  55  85  75  55  15  65  70  85  —   —    9  75  55  15  15  15
Recon             70  65  35  45   8   1   4  45  55  —   —   10  55   8   1   1   1
APC               —   —   —   —   —   —   —   —   —   —   —    —   —   —   —   —   —
Tank              75  70  85  75  55  15  65  70  85  —   —   10  75   —   —   —   —
HeavyTank        105  95 105 105  85  55 105 105 105  —   —   12 105   —   —   —   —
Anti-Air         105 105  60  50  25  10  45  50  55  65  75  120  —   —   —   —   —
Artillery         90  85  80  70  70  45  75  75  80  —   —    —  55  65  60  40  45
Rocket            95  90  90  80  80  55  85  85  85  —   —    —  60  75  85  55  60
Fighter           —   —   —   —   —   —   —   —   —   55 100 100  —   —   —   —   —
Bomber           110 110 105 105 105  95  95 105 105  —   —    —  95  95  95  75  75
BattleCopter      75  75  55  60  55  25  25  65  65  —   —   65  25  25  25  25  25
Lander            —   —   —   —   —   —   —   —   —   —   —    —   —   —   —   —   —
Destroyer         30  30  28  50  18  10  18  55  55 100 100 100  95  25  55  10  50
Submarine         —   —   —   —   —   —   —   —   —   —   —    —  95  55  55  55  75
Battleship        95  90  90  80  80  55  85  80  80  —   —    —  95  95  95  50  60
Carrier           —   —   —   —   —   —   —   —   —   —   —    —   —   —   —   —   —
```

Three rows are entirely illegal-attack: APC, Lander, Carrier.

**Submarine engagement scope**: Submarines engage **naval targets
only** — Lander, Destroyer, Submarine, Battleship, Carrier. They
cannot attack ground, air, or amphibious-loaded cargo even when
adjacent. This is intentional (AW canon); document it here because
the sparse table can read as an oversight at first glance.

**Anti-Air vs Battle Copter = 120 (intentional merger consequence)**:
In canon AW, Anti-Air-vs-BCop deals 75% base. Bestagon's Anti-Air
absorbs the canonical Missiles unit (locked decision 23), and
Missiles-vs-BCop is 120% in canon. Bestagon keeps the 120 to preserve
the Missiles-style anti-air bite that would otherwise vanish with the
merger. Tactical effect: a full-HP Anti-Air can one-shot a full-HP
Battle Copter on plain terrain (120% × 1.0 × 0.90 × 1.0 = 108% damage
→ defender takes 10 HP, full destruction, floored at 0 HP remaining
— Plain's 1 star isn't enough cover to break the 100% threshold).
BCops in Bestagon must screen for AA much more carefully than in canon
AW; this is by design, not an oversight.

### 7.5 Damage table dict literal (Phase 6-ready)

```trix
% Damage table — AW2/AWBW canon with 3 unit-merger adjustments.
% Sparse: illegal attacks are missing keys.
/damage-table <<
    /infantry <<
        /infantry 55  /mech 45  /recon 12  /apc 14  /tank 5  /heavy-tank 1
        /anti-air 5   /artillery 15  /rocket 25   /battle-copter 7
        /lander 14    /destroyer 5   /submarine 1 /battleship 1   /carrier 1
    >>
    /mech <<
        /infantry 65  /mech 55  /recon 85  /apc 75  /tank 55 /heavy-tank 15
        /anti-air 65  /artillery 70  /rocket 85   /battle-copter 9
        /lander 75    /destroyer 55  /submarine 15 /battleship 15 /carrier 15
    >>
    /recon <<
        /infantry 70  /mech 65  /recon 35  /apc 45  /tank 8  /heavy-tank 1
        /anti-air 4   /artillery 45  /rocket 55   /battle-copter 10
        /lander 55    /destroyer 8   /submarine 1 /battleship 1   /carrier 1
    >>
    % /apc has no attack
    /tank <<
        /infantry 75  /mech 70  /recon 85  /apc 75  /tank 55  /heavy-tank 15
        /anti-air 65  /artillery 70  /rocket 85   /battle-copter 10
        /lander 75
    >>
    /heavy-tank <<
        /infantry 105 /mech 95  /recon 105 /apc 105 /tank 85  /heavy-tank 55
        /anti-air 105 /artillery 105 /rocket 105  /battle-copter 12
        /lander 105
    >>
    /anti-air <<
        /infantry 105 /mech 105 /recon 60  /apc 50  /tank 25  /heavy-tank 10
        /anti-air 45  /artillery 50  /rocket 55
        /fighter 65   /bomber 75    /battle-copter 120
    >>
    /artillery <<
        /infantry 90  /mech 85  /recon 80  /apc 70  /tank 70  /heavy-tank 45
        /anti-air 75  /artillery 75  /rocket 80
        /lander 55    /destroyer 65 /submarine 60 /battleship 40 /carrier 45
    >>
    /rocket <<
        /infantry 95  /mech 90  /recon 90  /apc 80  /tank 80  /heavy-tank 55
        /anti-air 85  /artillery 85 /rocket 85
        /lander 60    /destroyer 75 /submarine 85 /battleship 55 /carrier 60
    >>
    /fighter <<
        /fighter 55   /bomber 100  /battle-copter 100
    >>
    /bomber <<
        /infantry 110 /mech 110 /recon 105 /apc 105 /tank 105 /heavy-tank 95
        /anti-air 95  /artillery 105 /rocket 105
        /lander 95    /destroyer 95 /submarine 95 /battleship 75 /carrier 75
    >>
    /battle-copter <<
        /infantry 75  /mech 75  /recon 55  /apc 60  /tank 55  /heavy-tank 25
        /anti-air 25  /artillery 65 /rocket 65   /battle-copter 65
        /lander 25    /destroyer 25 /submarine 25 /battleship 25 /carrier 25
    >>
    % /lander has no attack
    /destroyer <<
        /infantry 30  /mech 30  /recon 28  /apc 50  /tank 18  /heavy-tank 10
        /anti-air 18  /artillery 55 /rocket 55
        /fighter 100  /bomber 100 /battle-copter 100   % Cruiser-canon AA absorbed
        /lander 95    /destroyer 25 /submarine 55 /battleship 10 /carrier 50
    >>
    /submarine <<
        /lander 95    /destroyer 55 /submarine 55 /battleship 55 /carrier 75
    >>
    /battleship <<
        /infantry 95  /mech 90  /recon 90  /apc 80  /tank 80  /heavy-tank 55
        /anti-air 85  /artillery 80 /rocket 80
        /lander 95    /destroyer 95 /submarine 95 /battleship 50 /carrier 60
    >>
    % /carrier has no attack
>>#r def
```

### 7.6 Counter-attack semantics

Combat resolution sequence:

1. Look up `damage-table[attacker][defender]`. Missing → attack illegal,
   abort.
2. Compute attacker's actual damage via the formula (§7.1).
3. Apply damage to defender's HP.
4. If attacker is indirect-fire (Artillery / Rocket / Battleship): no
   counter-attack. End.
5. If defender survived AND `damage-table[defender][attacker]` exists:
   compute counter-attack damage using defender's POST-damage HP. Apply
   to attacker.

Counter-attack happens iff: attacker is direct-fire AND defender
survives AND `damage-table[defender][attacker]` is defined.

### 7.7 Indirect-fire range

| Unit       | Min range | Max range |
| ---------- | --------- | --------- |
| Artillery  | 2         | 3         |
| Rocket     | 3         | 5         |
| Battleship | 2         | 6         |

Indirect attackers cannot move + fire same turn (locked decision 8).
`/has-moved` must be false at fire time.

### 7.8 Veterancy (locked decision 37)

Units track their kill count and gain attack bonuses at thresholds.
Per-unit, not per-CO. Folds into `sum_atk_pct` (§7.1) — veterancy is
just another attack-side modifier.

> **PENDING Phase 5.5A re-measurement** (filed 2026-05-15 from Phase
> 0.5E vet-distribution sim): vet-3 threshold of 8 kills produced 0%
> incidence across the 30-turn stub-arena. Real AI v1 on real maps
> (capture mechanics + healing + retreats) will yield different
> distributions. Phase 5.5A will re-measure and decide one of:
> **keep 8** / **drop to 7** / **drop to 6** / **add XP-from-assists
> (kill-assist gives 0.5 kill-credit)**. The 3/5/8 threshold table
> below is the current locked value; the row marked `*` is the open
> question.

#### Tiers

| Vet tier | Kills to reach     | Atk bonus | Render pip |
| -------- | ------------------ | --------- | ---------- |
| 0        | 0                  | +0%       | none       |
| 1        | 3                  | +5%       | `★`        |
| 2        | 5                  | +10%      | `★★`       |
| 3        | 8 (*PENDING 5.5A*) | +15%      | `★★★`      |

#### Tracking

A "kill" is credited to attacker whenever the attacker's damage reduces
defender HP to 0. Counter-attack kills count for the counter-attacker.
Indirect attacks that kill count normally; range-2-6 Battleship kills
are not special-cased.

Per-unit Dict (§17.4) gains:

```trix
/kills      0          % integer, accumulator
/vet-tier   0          % integer 0-3; recomputed on kill
```

On kill: `/kills += 1`; recompute `/vet-tier` via the threshold table
above. Tier never decreases; transported units in an APC/Lander carry
their veterancy. New units built at Factory/Port/HQ start at vet 0.

#### Formula integration

`/vet-atk +N` becomes one of the keys aggregated into `sum_atk_pct` in
the §7.1 formula. The aggregator gates by attacker-unit-id, reading
`/vet-tier` and multiplying by 5:

```
sum_atk_pct += unit.vet-tier × 5
```

A vet-3 attacker thus contributes +15 percentage points to the base-
damage multiplier, on top of any CO bonuses. Cap: tier 3 is the
maximum; no vet-4 in v1.

#### Edge cases

- **Friendly-fire kills** (not possible in v1 — no friendly fire op).
- **HP-rounded-to-0 kills** count; the unit is destroyed.
- **Capture-kills** (HQ-flip eliminating a faction's units at end-of-turn)
  do NOT credit any unit. They're game-state cleanup, not combat.
- **Self-destruction** (sea-sink, air-crash on 0 fuel) does not credit
  the most-recent-attacker. Game-state cleanup, not a combat resolution.

### 7.9 Zone of Control (locked decision 14)

ZOC defines how enemy unit adjacency restricts movement. The v1
model is **light ZOC**: a moving unit that enters a hex adjacent to
any enemy unit halts further movement that turn (i.e., the entering
hex becomes the destination; no further hexes can be traversed even
if move-budget remains).

#### Scope

| Aspect                               | ZOC behavior                                                     |
| ------------------------------------ | ---------------------------------------------------------------- |
| Triggers on entering adjacency       | Yes — entering a hex adjacent to ≥ 1 enemy unit halts movement   |
| Applies to ground (foot/wheel/tread) | Yes                                                              |
| Applies to air                       | **No** — air units ignore ZOC (move freely over/around enemy)    |
| Applies to sea                       | Yes — sea units halt on adjacency to enemy naval units only      |
| Stealth-Sub adjacency                | **No** — a submerged Sub does not project ZOC (it's invisible)   |
| Adjacency to own units               | **No** — only enemy unit adjacency projects ZOC                  |
| Starting hex already in ZOC          | No effect — ZOC triggers on ENTERING a new adjacent hex          |
| Indirect-fire's target hex           | No special interaction — indirect range and ZOC are independent  |
| Capturing-on-own-building start      | No effect — Inf already on enemy building when capturing is fine |

#### Detection rule

```
on attempted move from src to dst (one hex at a time):
    if dst is adjacent to any enemy unit:
        commit move to dst
        halt — no further hexes this turn
    else:
        commit move to dst
        continue if budget remains
```

The check uses §3 hex adjacency (offset-coord table). Submerged subs (per §11.2) are
excluded from "enemy unit" for ZOC purposes — they're invisible, so
they don't project visible ZOC either.

#### Interaction with terrain

ZOC takes precedence over move-cost. A unit with 5 move that enters
a ZOC hex on its 2nd hex burns 2 move (per terrain costs) and halts
— remaining 3 move is forfeited.

#### Interaction with CO Powers

`/foot-move`, `/all-move`, `/direct-move`, `/air-move` modifiers
increase the move budget but do not change ZOC rules. A
Forced-March Boots Inf with `/foot-move +3` (Inf base 3 + Boots
passive `/foot-move +1` + Forced March `/foot-move +3` = budget 7)
still halts on entering an enemy-adjacent hex on hex 2.

#### Phase 5 invariant

The Phase 5 self-test (§19) asserts: "Recon moves into adjacent
enemy halts after 1 hex" — Recon has 8 move; entering an
adjacent-to-enemy hex on hex 1 should result in `/has-moved = true`
and the unit at that hex, not deeper.

---

## 8. Commanding Officers

Six archetypes. Each has a callsign, color, AW analog, signature
passive bonuses, a regular Power activation, and a Super Power
activation at a higher star cost (locked decision 21).

### 8.1 Roster overview

| # | Callsign | Color | Archetype | AW analog | Bio |
| --- | --- | --- | --- | --- | --- |
| 1 | Bishop | white | Balanced | Jess (closest) | "Steady hand. No specialty, no weakness." |
| 2 | Hammer | red | Direct brawler | Max | "If it's in range, hit it harder." |
| 3 | Hawk | yellow | Indirect / artillery | Grit | "Range is depth. Depth is victory." |
| 4 | Boots | green | Infantry / capture | Sami | "Cities are the prize. Take more, take them faster." |
| 5 | Veil | purple | Intel / fog-of-war | original | "What they can't see, they can't fight." |
| 6 | Talon | cyan | Air specialist | Eagle | "Sky's the high ground." |

AW-analog notes:

- **Bishop** is closer to AW2 Jess (small vehicle/atk bonus) than to
  Andy. Andy is the no-passive baseline CO; Bishop's always-on +5/+5
  makes him strictly stronger than Andy. If you want a closer
  Andy-analog, drop Bishop's `/all-atk +5` `/all-def +5` passives.
- **Hammer** matches Max in full now (including the AW-canonical −1
  indirect range penalty added 2026-05-15).
- **Hawk** matches Grit one-for-one.
- **Boots** matches Sami except for the foot-move +1 passive (Sami's
  movement perk in AW2 applies to transports, not foot directly). The
  Bestagon variant is slightly more permissive — keep as-is for clarity.
- **Veil** is **not** Sonja. Sonja's signature kit is counter-first
  combat AND wounded-defender luck inversion; Veil's `/vs-revealed-atk`
  and Forest/Reef def bonuses are original. Treat Veil as a new
  archetype rather than an AW import.
- **Talon** matches Eagle (AW2 air specialist): all air units gain
  firepower + movement; ground units take a small penalty as the
  trade. Lightning Drive Super Power (air units act twice) is the
  signature shared move (scoped to air-only on 2026-05-16 — see §8.2
  Super table note). Added 2026-05-15 as part of the §1.3 row 26
  roster expansion + row 21 Super Power promotion.
  **Departure from AW Eagle (intentional, locked 2026-05-16):**
  Eagle has +20% air-atk AND +20% air-def; Talon's passive is
  `/air-atk +15 /air-move +1 /ground-atk -10` — no defensive bonus.
  Reduces Talon's "air swarms are uncatchable" feedback loop in v1
  (Air units are already expensive at 9-22k; the +1 air-move passive
  gives the offense bite without making defenders feel helpless).

### 8.2 Per-CO spec

#### Regular Power

| CO | Passive (always-on) | Power name | Star cost | Power effect (one turn) | Charge rate |
| --- | --- | --- | --- | --- | --- |
| Bishop | All units +5% atk, +5% def | Air Support | 5 | All own units +2 HP healed; **+15% atk** this turn (bumped from +10 on 2026-05-16 for per-star parity with Hammer's +20 direct-ground / Boots' 4★ Forced March) | 1.0× |
| Hammer | Direct **ground** units +20% atk; indirect units −20% atk AND −1 max range | Max Force | 6 | Direct **ground** units +30% atk AND +1 move this turn | 1.0× |
| Hawk | Indirect units +1 max range AND +20% atk; direct **ground** units −20% atk | Long Watch | 5 | Indirect +1 range +30% atk; all units +1 vision this turn | 1.0× |
| Boots | Inf/Mech +30% atk AND +1 move; capture speed +50%; vehicles −10% atk | Forced March | 4 | Inf/Mech +3 move; in-progress capture finishes if attacker stays on tile; **+50% capture-speed this turn** (stacks with the +50% passive — Inf at full HP captures a city in one turn; added 2026-05-16 to keep the Power useful on activation turns with no in-progress capture, which is the common AI-v1-greedy case) | 1.2× |
| Veil | All units +1 vision; +15% atk vs revealed enemies; +1 def on Forest or Reef | Eclipse | 5 | Reveal enemies within 3 hexes of own; revealed enemies adjacent at EOT take −1 move next turn | 0.9× |
| Talon | Air units +15% atk AND +1 move; ground units −10% atk | Air Superiority | 6 | Air units +30% atk; all air units gain 1 extra action this turn | 1.0× |

#### Super Power

**Super-cost rule (locked v1):** `super-cost = power-cost + 4`.
This is a v1 design constraint applied to every Super in the canonical
6-CO roster below AND required of every CO added via
`/co-roster-extras` (§16.4): scenario authors who declare a custom
CO with `/power-cost N` must declare `/super-cost N + 4`.  Rationale:
keeps Power-vs-Super pacing comparable across COs (≈ 4-star
"upgrade premium" regardless of base Power cost), simplifies the
Phase 16 invariant, and gives the meter-charge analysis (§9.5) a
clean closed form.

Activation conditions: §9. Each Super is a strict superset of the
regular Power's effect — same effect category, larger numbers, plus
one additional kicker.

| CO | Super name | Star cost | Super effect (one turn) |
| --- | --- | --- | --- |
| Bishop | Air Crusade | 9 | All own units +4 HP healed; **+35% atk** this turn; +1 move all units (atk bumped from +25 on 2026-05-16 for balance) |
| Hammer | Maximum Force | 10 | Direct ground units +50% atk, +1 move; +1 max range for indirect (negates the −1 passive this turn) |
| Hawk | Total Watch | 9 | Indirect +1 range, +50% atk; all units +2 vision; indirect units gain 1 extra action this turn |
| Boots | Victory March | 8 | Inf/Mech +5 move; all in-progress captures auto-complete; +5 HP healed to all foot; **own foot on enemy/neutral buildings auto-begin capture at start-of-turn** (kicker added 2026-05-16 to differentiate from regular Forced March) |
| Veil | Total Eclipse | 9 | Reveal enemies within 5 hexes; revealed enemies adjacent at EOT take −2 move next turn; all own units +2 def this turn |
| Talon | Lightning Drive | 10 | **All own AIR units: +50% atk, 1 extra action, fuel + ammo restocked to max** this turn (scoped to air-only on 2026-05-16; previously all-units, which dwarfed Hammer's Maximum Force at the same 10★ cost). Differentiates from the regular Power (which is +30% atk + extra action) by the +20% atk delta + the restock-to-max kicker, which removes the Carrier-adjacency dependence for one turn — Talon's operational-tempo signature. The all-units version is filed as a v2 candidate if data shows Talon undertuned. |

### 8.3 CO roster dict literal (Phase 16-ready)

```trix
/co-roster <<
    /bishop <<
        /callsign    (Bishop)
        /color       /white
        /bio         (Steady hand. No specialty, no weakness.)
        /passives    << /all-atk +5  /all-def +5 >>
        /power-name  (Air Support)
        /power-cost  5
        /power       << /all-hp +2  /all-atk +15 >>   % +15 atk (bumped from +10 on 2026-05-16 for per-star parity)
        /super-name  (Air Crusade)
        /super-cost  9
        /super       << /all-hp +4  /all-atk +35  /all-move +1 >>   % +35 atk (bumped from +25 on 2026-05-16 to compete with Lightning Drive)
        /charge-rate 100
    >>
    /hammer <<
        /callsign    (Hammer)
        /color       /red
        /bio         (If it's in range, hit it harder.)
        /passives    << /direct-atk +20  /indirect-atk -20  /indirect-range -1 >>
        /power-name  (Max Force)
        /power-cost  6
        /power       << /direct-atk +30  /direct-move +1 >>
        /super-name  (Maximum Force)
        /super-cost  10
        /super       << /direct-atk +50  /direct-move +1  /indirect-range +1 >>
        /charge-rate 100
    >>
    /hawk <<
        /callsign    (Hawk)
        /color       /yellow
        /bio         (Range is depth. Depth is victory.)
        /passives    << /indirect-range +1  /indirect-atk +20  /direct-atk -20 >>
        /power-name  (Long Watch)
        /power-cost  5
        /power       << /indirect-range +1  /indirect-atk +30  /all-vision +1 >>
        /super-name  (Total Watch)
        /super-cost  9
        /super       << /indirect-range +1  /indirect-atk +50  /all-vision +2  /indirect-extra-action true >>
        /charge-rate 100
    >>
    /boots <<
        /callsign    (Boots)
        /color       /green
        /bio         (Cities are the prize. Take more, take them faster.)
        /passives    << /foot-atk +30  /foot-move +1  /capture-speed +50  /vehicle-atk -10 >>
        /power-name  (Forced March)
        /power-cost  4
        /power       << /foot-move +3  /capture-finish-now true  /capture-speed +50 >>   % +50 capture-speed this turn stacks with passive (added 2026-05-16)
        /super-name  (Victory March)
        /super-cost  8
        /super       << /foot-move +5  /capture-finish-now true  /foot-hp +5  /auto-start-captures true >>   % auto-start kicker added 2026-05-16
        /charge-rate 120
    >>
    /veil <<
        /callsign    (Veil)
        /color       /purple
        /bio         (What they can't see, they can't fight.)
        /passives    <<
            /all-vision        +1
            /vs-revealed-atk  +15
            /forest-def        +1
            /reef-def          +1
        >>
        /power-name  (Eclipse)
        /power-cost  5
        /power       <<
            /reveal-radius-around-own       3
            /adjacent-revealed-move-penalty 1
        >>
        /super-name  (Total Eclipse)
        /super-cost  9
        /super       <<
            /reveal-radius-around-own       5
            /adjacent-revealed-move-penalty 2
            /all-def                        +2
        >>
        /charge-rate 90
    >>
    /talon <<
        /callsign    (Talon)
        /color       /cyan
        /bio         (Sky's the high ground.)
        /passives    << /air-atk +15  /air-move +1  /ground-atk -10 >>
        /power-name  (Air Superiority)
        /power-cost  6
        /power       << /air-atk +30  /air-extra-action true >>
        /super-name  (Lightning Drive)
        /super-cost  10
        /super       << /air-atk +50  /air-extra-action true  /air-restock true >>   % air-only since 2026-05-16 (was /all-extra-action); restock-to-max removes Carrier dependence for the turn
        /charge-rate 100
    >>
>>#r def
```

### 8.4 Combat-resolver modifier keys

The combat resolver iterates `co.passives` and `co.power` (when active)
and applies whichever keys match the situation.

| Key | Reader | Applies to |
| --- | --- | --- |
| `/all-atk` | every attack roll | percent additive into `sum_atk_pct` |
| `/all-def` | every defense roll | percent additive into `sum_def_pct` |
| `/all-hp` | end-of-turn heal step | additive HP |
| `/all-vision` | vision computation | additive hex radius |
| `/all-move` | every move-action | additive hex; uniform (Bishop's Air Crusade Super) |
| `/direct-atk` / `/direct-move` | **ground** direct-fire only | Inf/Mech/Recon/Tank/HTank/AA — does NOT include Fighter/Bomber/BCop/Destroyer/Sub |
| `/air-direct-atk` | air direct-fire only | Fighter/Bomber/Battle Copter (reserved key; no v1 CO uses it) |
| `/sea-direct-atk` | sea direct-fire only | Destroyer/Submarine (reserved key; no v1 CO uses it) |
| `/indirect-atk` / `/indirect-range` | indirect-fire only | Artillery/Rocket/Battleship |
| `/foot-atk` / `/foot-move` / `/foot-hp` | foot class only | Inf/Mech |
| `/vehicle-atk` | wheel + tread classes | Recon/APC/AA/Arty/Rocket/Tank/HTank |
| `/air-atk` / `/air-move` | air class only | Fighter/Bomber/Battle Copter |
| `/ground-atk` | foot + wheel + tread classes | every non-air, non-sea unit |
| `/tread-move` | tread class only | Tank/HTank |
| `/wheel-move` | wheel class only | Recon/APC/AA/Arty/Rocket |
| `/capture-speed` | capture state machine | percent (+50 = `floor(unit.HP × 1.5)` per turn, per §10.3) |
| `/capture-finish-now` | capture state machine | boolean — completes any in-progress capture this turn |
| `/auto-start-captures` | capture state machine (Super) | boolean — own foot units on enemy/neutral buildings auto-begin capture at SOT (Boots Victory March exclusive) |
| `/vs-revealed-atk` | every attack roll | additive percent, gated on `attacker.can-see(target)` |
| `/forest-def` / `/reef-def` | defense on terrain | additive stars folded into `def_terrain_stars` when defender on matching terrain |
| `/reveal-radius-around-own` | Eclipse activation | radius-N reveal burst around every own unit this turn |
| `/adjacent-revealed-move-penalty` | Eclipse activation | enemy −N move next turn if adjacent + revealed at EOT. **Non-stacking across sources AND turns:** a unit penalised this turn (by any source — same Veil casting consecutively, multiple Veils in a free-for-all, or scenario-extra COs that share the key) takes max −N total, not −2N or worse.  Implementation: clamp the per-unit move-mod after applying all `/adjacent-revealed-move-penalty` contributions for the turn. |
| `/air-extra-action` | Talon Power AND Super (boolean) | every air unit gets a second `/has-moved`+`/has-fired` reset this turn |
| `/air-restock` | Talon Super (Lightning Drive) | all own air units' `/ammo` and `/fuel` restocked to max at activation (one-shot, single turn) |
| `/indirect-extra-action` | Hawk Super (boolean) | every indirect unit gets a second reset this turn |
| `/all-extra-action` | (reserved — not used in v1; was Talon Super pre-2026-05-16, now `/air-extra-action`) | every own unit (excluding `/built-this-turn` and capturing units) gets a second reset; kept in the modifier dict so a v2 / scenario-extra CO can opt in without re-introducing the key |

**Resolver order** (locked — Super replaces Power, NOT augments):

1. Veterancy bonus (`vet-tier × 5` added to `sum_atk_pct`, per §7.8)
2. CO passives (always-on bonuses)
3. **EITHER** Power-if-active **OR** Super-if-active (mutually
   exclusive — picking one closes the other for the turn, per §9.1).
   Never both stacked.
4. **Weather modifiers** (`/weather-current` modifier Dict from
   §11.5; applies to all sides equally — weather is a world effect,
   not faction-specific)
5. Terrain stars (HP-scaled per §7.1)
6. HP scaling (`atk_HP / 10` and `def_HP / 10`)
7. Luck roll (`luck_pp` added to base before multiplications, per §7.3)

The "EITHER/OR" at step 3 is the key correctness rule: §9.1's
"Super is replacement, not augmentation" applies here. The resolver
checks `faction.super-active` first; if true, applies Super
modifiers and skips Power modifiers entirely. Otherwise, if
`faction.power-active`, applies Power. Otherwise, neither.

**Indirect-range stacking cap** (locked): The `/indirect-range` key
is additive across (passive + Power-OR-Super), like other modifiers.
Per the resolver, the maximum stack is `passive ± Super` OR
`passive ± Power`, never `passive + Power + Super` together. For
Hawk this caps the bonus at `+1` (passive) `+ +1` (Power or Super)
`= +2`. Applied to base indirect ranges:

| Unit       | Base | Max stack (Hawk passive + Power/Super) |
| ---------- | ---- | -------------------------------------- |
| Artillery  | 2-3  | 2-5                                    |
| Rocket     | 3-5  | 3-7                                    |
| Battleship | 2-6  | 2-8                                    |

These are the **firm upper bounds**; no further `/indirect-range`
stacking from veterancy or scenario overrides applies. (Scenario
files MAY use `/co-roster-extras` to introduce new COs with their
own indirect-range modifiers, but the per-attack effective range
must satisfy `effective ≤ base + 2` after all modifier resolution.)

**Cap enforcement**: applied at attack-target-validation time. A
target outside the effective range is illegal — the action fails
silently in UI (target hex not highlighted as legal).

---

## 9. Power meter

### 9.1 Charge formula

```
star_gain = (dealt_funds + received_funds × 0.5) / 5000 × (charge_rate / 100)

  dealt_funds    = dealt_HP    × target_unit_cost / 10
  received_funds = received_HP × own_unit_cost    / 10
  charge_rate    = CO modifier (90 / 100 / 120 per locked roster)
  cap            = 12 stars (excess discarded)
  reset          = activating Power OR Super sets stars to 0
```

Each resolved combat exchange contributes a `star_gain` to BOTH
attacker and defender factions.

**Activation flow** (regular Power vs Super):

When `floor(stars) >= power-cost`, the action-loop UI offers
**Power / Super / Hold**:

- **Power** — activates the regular Power (requires `stars >=
  power-cost`); meter resets to 0; `/power-active = true` for the
  remainder of turn.
- **Super** — activates the Super Power (requires `stars >=
  super-cost`); meter resets to 0; `/power-active = true` AND
  `/super-active = true` for the remainder of turn. Super's effect
  is applied *instead of* the regular Power's effect (not in addition
  to — the Super is a strict replacement).
- **Hold** — no activation; meter continues accumulating up to
  `cap-stars`. Useful when the player is saving up for Super.

The meter cap (12 stars) is set high enough to hold a Super charge for
every CO (max super-cost is Hammer's 10 / Talon's 10, leaving 2 stars
of headroom above the highest super-cost).

### 9.2 Pacing target

| CO     | Power cost | Super cost | Charge rate | Exchanges to first Power | Exchanges to first Super |
| ------ | ---------- | ---------- | ----------- | ------------------------ | ------------------------ |
| Boots  | 4          | 8          | 1.2×        | ~5                       | ~10                      |
| Veil   | 5          | 9          | 0.9×        | ~7                       | ~12                      |
| Bishop | 5          | 9          | 1.0×        | ~6                       | ~11                      |
| Hammer | 6          | 10         | 1.0×        | ~7                       | ~12                      |
| Hawk   | 5          | 9          | 1.0×        | ~8                       | ~14                      |
| Talon  | 6          | 10         | 1.0×        | ~8                       | ~14                      |

**Calibration source** (post-Phase-0.5C, 2026-05-15): Monte-Carlo sim
at 1000 trials per (atk_co, def_co) Tank-vs-Tank Plain matchup × 36
pairs = 36,000 trials per CO.  Sim means landed at 6.29 / 7.01 / 7.60
/ 4.99 / 6.74 / 8.35 exchanges for Bishop / Hammer / Hawk / Boots /
Veil / Talon respectively (std-dev 0.07-0.49).  The table values are
rounded to integer with all six COs within ±0.40 of sim mean — well
inside the §19 v1 self-test ±1-exchange tolerance.  Super-cost
exchanges scaled by super-cost / power-cost (linear under constant
charge-rate accumulation between Power and Super activation).  Hawk
and Talon's slower pacing is by design — their specialty kits don't
apply to a generic Tank-vs-Tank attack (Hawk's /direct-atk -20 + 
Talon's /ground-atk -10 actually penalize this matchup); the trade is
that their Power / Super impact is correspondingly larger when
activated.  See `examples/bestagon-sim/pacing-sim.trx` and
`memory/plan_bestagon_balance_prototyping.md` § Findings.

**Pacing math.** Assume a "typical" Tank-vs-Tank exchange on plain
terrain (Plain = 1 star per §4 → def_factor 0.90 at full HP), both at
full HP, **zero CO passives** (the math below; in practice a
Bishop-vs-Bishop matchup happens to net to nearly the same numbers
because his +5/+5 atk/def passives cancel symmetrically), luck off:

```
attacker deals ((55 + 0) × 1.0) × 0.90 × 10/10 = 49.5% → 4 HP → 2800 funds
defender counter ((55 + 0) × 1.0) × 0.90 ×  6/10 = 29.7% → 2 HP → 1400 funds
attacker star_gain = (2800 + 1400 × 0.5) / 5000 × 1.0
                   = (2800 + 700) / 5000
                   = 0.70 stars
defender star_gain = (1400 + 2800 × 0.5) / 5000 × 1.0
                   = (1400 + 1400) / 5000
                   = 0.56 stars
```

So in **rough** terms, an attacking faction averages ~0.6-0.85 stars
per exchange (closer to 0.85 with strong attacker-side passives like
Hammer's +20 direct-atk; closer to 0.4 in poor matchups), giving
Bishop's 5-star Power around 6 meaningful exchanges to charge.  Boots'
1.2× charge rate AND lower 4-star cost compound to ~5 exchanges.
Hammer's 6 stars at 1.0× rate with his +20 atk passive lands at ~7.
Hawk and Talon take ~8 because their kits *penalize* the generic
Tank-vs-Tank matchup (negative passives apply); their advantage shows
up in matchups their specialty targets, AND in Power / Super impact.
The pacing table above was calibrated by Phase 0.5C Monte-Carlo
simulation (36,000 trials at 1000 trials per CO pair); the v1
self-test (§19 Phase 16) asserts the table's prediction holds within
±1 exchange under the same scripted scenario.

### 9.3 Worked example: Tank-vs-Tank, plain terrain, both full HP

Hammer attacks Boots; both Tanks at full HP, `/luck-mode /off`.
Hammer's passives: `/direct-atk +20` (Tank is direct-ground; this
applies). Hammer's `/indirect-atk -20` and `/indirect-range -1`
passives are present but **inapplicable** (Tank is direct-fire,
not indirect). Boots' passives: `/vehicle-atk -10` (Tank is
wheel/tread, so vehicle), `/foot-atk +30` (Tank is not foot,
inapplicable). Neither has `/all-def`. No Power active; no Super
active; no veterancy. Plain = 1 star (§4) → def_terrain_stars = 1.

| Step | Computation | Result |
| --- | --- | --- |
| Hammer's attack | `((55 + 0) × 1.20) × (1 − (0 + 1×10)/100) × 10/10` = 59.4 → floor | **5 HP** to Boots' Tank (now 5 HP) |
| Boots' counter (defender post-damage HP) | `((55 + 0) × 0.90) × (1 − (0 + 1×10)/100) × 5/10`  = 22.275 → floor | **2 HP** to Hammer's Tank |
| Funds dealt by Hammer | `5 × 7000 / 10` | 3500 |
| Funds dealt by Boots | `2 × 7000 / 10` | 1400 |
| Hammer star_gain | `(3500 + 1400×0.5) / 5000 × 1.0` | **0.84 stars** |
| Boots star_gain | `(1400 + 3500×0.5) / 5000 × 1.2` | **0.756 stars** |

Note how Boots' `/vehicle-atk -10` passive applies to the counter
(Boots' Tank is the *attacker* of the counter step), shifting the raw
counter float from `24.75` (no-passive: `55 × 1.0 × 0.90 × 0.5`) to
`22.275` — both floor to 2 HP, so the integer output is the same in
this specific matchup. The passive *does* affect integer output at
other HP/matchup combinations: e.g., Boots Tank attacking Hammer Recon
on Plain at full HP, no vet, no Power, deals 6 HP with `/vehicle-atk -10`
applied vs 7 HP without (`85 × 0.90 × 0.90 = 68.85` → 6 vs
`85 × 1.0 × 0.90 = 76.5` → 7). This worked example is the canonical
regression for Phase 6 (§19): commit a self-test that asserts these
exact outputs.

### 9.4 Meter config dict literal

```trix
/co-meter-config <<
    /threshold-funds-per-star  5000
    /received-weight           50         % percent (0.5)
    /cap-stars                 12         % bumped from 10 on 2026-05-15 to hold Super charges
    /reset-on-power-activation true       % applies to BOTH regular Power and Super
>>#r def
```

Scenario-overridable via `/co-meter-overrides`.

### 9.5 Edge cases

- **Power-boosted damage counts at actual delivered amount.** Small
  feedback loop; bounded by cap + reset.
- **Healing contributes nothing** to either side's meter.
- **Capture contributes nothing** (no HP exchange).
- **First-turn meters start at 0**.
- **Per-faction state**; preserved via `def-persist`.
- **UI displays `floor(stars)`**; float accumulates internally.
- **Stockpile headroom (locked 2026-05-15):** `cap-stars = 12` and
  max `super-cost = 10` leaves **2★ of headroom**. Intentional: the
  cap was bumped from 10 to 12 specifically to enable
  Super-saving — a faction holding 10★ can keep earning up to 12★
  before the meter clamps, so an immediately-pending Super doesn't
  block subsequent meter gain mid-exchange. Larger headroom isn't
  needed in v1 (charge rates 0.9× to 1.2× × ~7-12 turns to refill
  means ≤ 1 stockpiled exchange is the realistic upper bound).
- **Super > Power on the same turn**: the player can only choose ONE
  activation per turn. Picking Power closes Super for that turn (meter
  resets). Picking Super skips the regular Power's effect entirely
  (replacement, not augmentation).
- **Super-stars-insufficient → Power still available**: if the meter
  is between `power-cost` and `super-cost`, only Power is offered;
  Super is greyed out.
- **`/air-extra-action` (Lightning Drive) edge**: air units that
  already acted this turn get their `/has-moved` and `/has-fired`
  flags reset to false. Build-on-turn-of-construction air units are
  NOT re-enabled (they're flagged `built-this-turn` separately to
  prevent factory rushes). Ground units are unaffected by Lightning
  Drive as of the 2026-05-16 air-only scoping.
- **Super-Power's `/foot-hp +5`** (Boots Victory March): heal applied
  AFTER the in-progress-capture-finish check, so a capturing Inf
  reduced below 1 HP can't lose vet credit from a kill it just made.

---

## 10. Capture mechanic

### 10.1 Capture state machine

Per building:

```
/capture-progress     integer   % 0-20 accumulator; building captured at 20
/capturer-unit-id     int | null
```

Per turn that an Inf/Mech unit ends its turn on an enemy or neutral
building:

1. If `/capture-progress = 0`, set `/capturer-unit-id` to the unit's id.
2. If `/capturer-unit-id` matches the unit on the tile, add the unit's
   current HP to `/capture-progress`.
3. If `/capture-progress >= 20`, building flips to capturer's faction.
   Reset progress to 0 and capturer to null. Faction increments
   `/buildings-captured`.

**Dual-field invariant** (locked 2026-05-15 Phase 0.6H): the pair
`(capture-progress, capturer-unit-id)` is always either
`(0, null)` or `(N>0, valid-unit-id)` — never `(N>0, null)`. The
state machine maintains this by construction: interrupt clears
both fields together; step 1 sets capturer whenever progress
transitions from 0. Phase 9's `validate-state` SHOULD flag any
building with `progress>0 AND capturer=null` as a save-file
corruption / invalid-scenario error.

### 10.2 Capture interruption

If, at end-of-turn, the building's `/capturer-unit-id` is NOT on the
building hex, reset `/capture-progress` to 0 and capturer to null.

### 10.3 CO modifiers

- **Boots' `/capture-speed +50` passive**: capture-HP contribution per
  turn multiplied by 1.5, then **floored** to integer. Concretely:
  per-turn progress added = `floor(unit.hp × 1.5)`.

  | unit HP | default progress/turn | Boots progress/turn | Default turns to capture | Boots turns to capture |
  | --- | --- | --- | --- | --- |
  | 10 | 10 | 15 | 2 | 2 |
  | 8 | 8 | 12 | 3 | 2 |
  | 5 | 5 | 7 | 4 | 3 |
  | 3 | 3 | 4 | 7 | 5 |

  The passive does **not** speed up full-HP capture (both finish in 2
  turns); its real value is preserving capture pace when the capturer
  is wounded.
- **Boots' Forced March Power's `/capture-finish-now true`**: any
  in-progress capture (capturer-unit-id remained on tile this turn)
  completes immediately at end-of-turn, regardless of accumulated
  `/capture-progress`.

### 10.4 Game-over capture

HQ is special: if `/owner` changes, the previous owner is **eliminated**
at end-of-turn:

1. The faction's `/defeated?` field (§17.2) flips to `true`.
2. All of the faction's units are removed from `/units` /
   `/faction-units` (and the cell `/unit-id` grid is cleared at each
   freed cell per TDD §3.2 `unit-kill`).
3. Any remaining buildings owned by the faction flip to neutral
   (`/owner = -1`, `/capture-progress = 0`, `/capturer-unit-id = null`).
4. The turn loop's win-condition check (§16.3) runs.  If exactly one
   non-defeated faction remains, `/game-status.state` flips to
   `/victory` with `/winner-id` set to that faction.  If zero
   non-defeated factions remain (simultaneous eliminations on the
   same turn — rare but possible), `/game-status.state` flips to
   `/draw`.

The `/game-status.state` enum (§17.1) is global to the game and
remains `/running` while any active faction is still in play — it
never holds a per-faction value.  Per-faction elimination is
tracked exclusively via `/defeated?`.

---

## 11. Fog of war

### 11.1 Visibility computation

`compute-fog faction-id state -- visible-set` returns the Set of
`[col row]` hexes visible to `faction-id`. Recomputed on demand; no
cache (locked decision 32-D).

Algorithm:

1. Start with empty visible-set.
2. For each unit owned by faction:
   - Compute effective vision: `unit.vision + CO modifiers + terrain (Mountain +3 for Inf/Mech)`.
   - For each hex within hex distance (per §3 "Hex distance" cube formula) `≤ vision` of unit's pos:
     - If Forest AND target hex contains an enemy **ground** unit
       AND hex distance > 1: skip (Forest hides ground units only;
       air units flying over Forest remain visible at full range).
     - If Reef AND target hex contains an enemy **naval** unit AND
       hex distance > 1: skip (Reef hides naval units only).
     - Otherwise add to visible-set.
3. For each building owned by faction: add its pos.
4. Return visible-set.

Unit-on-unit visibility computed at render time via the same logic.

**Internal representation** (locked 2026-05-15 Phase 0.6E): the
spec's `visible-set` is a conceptual Set of `[col row]` hexes — the
*function signature* — but the *internal implementation* is free to
choose Set vs `Array<Byte>` based on call-site needs. Phase 0.6E
measured both: bool-array (W*H bytes, 1=visible 0=not) was 3-10%
faster than Set on a 30-unit Standard/Epic fixture because the full-
array zero-fill at start of each call costs less than incremental
Set populate on the ~20-44% fog coverage typical of mid-game.
Engine convention: render code uses bool-array for O(1) per-cell
lookup; AI v2's `find-all /visible-hex` predicate uses whichever
representation the engine produces. **NEVER use fresh `[q r]` Array
tuples as Set keys** — Trix Sets dedupe by identity for compound
keys, so two unit-vision discs overlapping at the same hex would
double-count (see [[feedback_trix_idioms_for_sim_code]] §"Trix Set
uses IDENTITY equality"). Use Integer linear-idx `q*h+r` as the
key, or pre-allocated per-cell shared tuples looked up by idx.

### 11.2 Submarine hiding (locked state machine)

Submarines have two states: **submerged** (hidden) and **surfaced**
(visible). State stored as `/sub-state /submerged | /surfaced` on
the unit Dict (extension to §17.4 for submarine type only; absent
for other unit types).

#### State transitions

| Trigger                                                     | Resulting state                      |
| ----------------------------------------------------------- | ------------------------------------ |
| Unit built at Port                                          | `/surfaced` (newly-built; visible)   |
| End-of-turn AND unit didn't attack this turn                | `/submerged`                         |
| Unit performs an attack action                              | `/surfaced` (remains until next EOT) |
| Unit's faction's turn ends with sub `/surfaced` + no attack | `/submerged`                         |
| Save/restore                                                | preserved verbatim                   |

#### Visibility

- Submerged sub is **invisible to all enemy units** except enemy
  Destroyers within hex distance 1 (range-1 detection per
  AW canon).
- Surfaced sub is visible like any other unit (subject to Forest/Reef
  hide only if on those terrains — but subs can't be on
  Forest/Mountain; they're sea-class only).

#### Fuel consumption

Submerged subs consume **5 fuel/turn** end-of-turn (vs 1/turn for
sea units generally, per §12.4). Surfaced subs consume the
standard sea rate (1/turn). The 5x consumption forces strategic
submerge-timing.

#### Implementation note

The state machine is small enough that no full §10-style state
diagram is needed. The single field `/sub-state` plus the
end-of-turn transition rule above is the full spec.

### 11.3 `fog-of-war: false`

Scenario can disable fog. With fog off, engine renders all units
to all factions. Used in tutorial.bw.

### 11.4 Veil's vs-revealed bonus

Veil's `/vs-revealed-atk +15` passive applies on every attack against
a target currently in Veil's `visible-set`. Combat resolver computes
visible-set at attack-evaluation time and gates the bonus.

### 11.5 Weather (locked decision 36)

Optional scenario feature. Adds per-turn environmental modifiers that
flow through the §8.4 modifier resolver alongside CO passives. Models
the AW weather-day mechanic at minimum complexity (no per-unit
alignment field, no time-of-day phases).

#### Weather table

```trix
/weather-table <<
    /clear     << >>                                                       % no modifiers
    /rain      << /all-vision -1   /all-atk -5 >>
    /snow      << /tread-move -1   /wheel-move -1   /foot-move -1 >>
    /sandstorm << /all-vision -2   /indirect-atk -10 >>
>>#r def
```

The empty `<<>>` for `/clear` is intentional: makes the resolver's
"merge modifiers from current weather" path uniform across weather
types. Engine default weather is `/clear` (no modifiers).

#### Cycle declaration

Scenarios opt in via `/weather-cycle` (top-level optional, §16.1).
The cycle is a finite array; engine rotates per turn round-robin:

```trix
/weather-cycle [/clear /clear /rain /clear /snow /clear] def
```

**Cycle advancement rule** (locked): the cycle index advances **once
per game-day** (i.e., once after every faction has taken its turn),
NOT per individual faction turn. With 2 factions this means weather
updates every 2 turns; with 4 factions every 4 turns. AW-canon
parallel — weather is a per-day phenomenon, not a per-faction-turn
toggle.

Concretely: at end-of-turn, the engine checks `state.turn.day` (§17.1)
against `state.weather-day-last-cycled`. If `day != last-cycled`,
increment `/weather-cycle-idx`. `/weather-current` is the active
weather at the start of every faction's turn within a single day.

Absence of `/weather-cycle` in a scenario means perpetual `/clear`
(no weather system runs).

#### Resolver integration

At combat resolution and movement resolution, the weather modifier
dict is folded into the active CO modifier sum **before** atk/def
aggregation (§7.1). Inapplicable keys contribute 0 (e.g., `/tread-move`
during Rain doesn't affect a Fighter). Weather can be cancelled or
mitigated by Powers — a Veil Eclipse with `/all-vision +1` partially
offsets Rain's `/all-vision -1`.

#### State

Game-state (§17.1) gains:

```trix
/weather-current     /clear | /rain | /snow | /sandstorm | (custom)
/weather-cycle-idx   integer    % index into /weather-cycle, 0-based
```

Both absent when scenario has no `/weather-cycle` declaration.

#### Custom weather types

Scenarios may extend `/weather-table` with their own entries via
`/weather-table-extras` (shallow-merged at load):

```trix
${ <<
    /heat-haze << /all-vision -1  /air-move -1 >>
>> } /weather-table-extras def
```

Custom names then become legal in `/weather-cycle`.

---

## 12. Economy

### 12.1 Income

At start of each faction's turn:

```
income                        = 1000 × (count of HQs owned + count of Cities owned)
faction.funds                += income
faction.cumulative-funds-earned += income
```

The `/cumulative-funds-earned` field (§17.2) increments here. It's
the load-bearing input for the `/turn-limit-funds` win condition
(§16.3) — `/funds` alone is insufficient because spent funds vanish.

### 12.2 Production

Build action at Factory, Port, or HQ:

- Factory: ground units only (Inf, Mech, Recon, APC, Tank, HTank, AA,
  Artillery, Rocket).
- Port: sea units only (Lander, Destroyer, Submarine, Battleship,
  Carrier).
- HQ: ground + air units (Fighter, Bomber, BCop). No separate Airport in v1.

**Building-hex preconditions** (locked):

- The build action requires the building hex to have **no unit** at
  the moment of build commit. The "stationed on building" state
  (§A.5.8) describes a unit occupying the building hex; this blocks
  production from that building until the unit moves.
- If `faction.funds >= unit.cost`: funds deducted; new unit appears
  on the building hex with `/has-moved = true`, `/has-fired = true`,
  AND `/built-this-turn = true` (the last flag prevents Lightning
  Drive factory-rush — §8.4).
- `faction.units-built` incremented.

`/built-this-turn` clears at the next start-of-turn for the unit's
faction (one turn of "freshly built" status; cannot act this turn,
can act normally next turn).

### 12.3 Starting funds

Default 0 per faction; scenario can declare `/starting-funds N`.
Tutorial.bw declares 7000 for the player.

### 12.4 Fuel and ammo (locked model)

**Fuel model: per-turn end-of-turn tick** (not per-hex-moved). This
diverges from AW canon (which is per-hex) and is locked for v1
simplicity — fuel becomes a turn-budget constraint, not a
movement-budget constraint.

- Air units decrement fuel by **2** at end-of-turn.
- Sea units decrement fuel by **1** at end-of-turn.
- Submerged Submarines decrement fuel by **5** at end-of-turn (AW
  canon; faster drain forces strategic submerge timing).
- Ground units have no fuel tick (per §6 unit roster — ground fuel
  is "soak", consumed only by special-action ops if any).
- 0 fuel at start-of-turn: air crashes (unit destroyed), sea sinks
  (unit destroyed). Submerged sub at 0 fuel surfaces, then sinks.
- Ammo decrements by 1 per direct attack (any attack consuming
  primary weapon; indirect counts).
- 0 ammo = unit can't attack until resupply (APC, Carrier, or
  stationed-on-building restock).

Mech's secondary bazooka ammo (3 shots vs vehicles): the damage
table row uses bazooka damage when applicable AND the unit has
bazooka ammo remaining; otherwise the row falls through to a
machine-gun damage scaling. **v1 simplification**: store `/ammo` as
a single integer representing bazooka ammo; machine-gun is
unlimited and provides a fixed 30% of bazooka-row damage when
bazooka is exhausted. (See §6 Mech entry.)

### 12.5 Healing and refueling

AW-canon model: heal **only** for units **stationed on (occupying)** a
friendly building hex at start of their faction's turn. Adjacent-only
units do NOT heal. This prevents "city-cluster bunker" turtle
strategies.

At start of each faction's turn:

- Each **HQ** heals the **stationed-on-it** ground OR air unit (if
  any, owned by the active faction) **+2 HP** (cap 10) AND restocks
  ammo to max AND restocks fuel to max.  Air-heal mirrors the AW
  Airport role (v1 has no separate Airport, locked decision 9).
- Each **City / Factory** heals the **stationed-on-it** ground unit
  only (per AW canon — Cities don't repair air, Factories ground-
  only build site).
- Each **Port** heals the **stationed-on-it** naval unit (if any)
  **+2 HP** AND restocks ammo to max AND refuels to max.
- Each **Carrier** refuels + repairs **adjacent** Fighters, Bombers,
  AND Battle Copters **+2 HP** (refuels to max, restocks ammo to
  max). Carrier exception: refueling is its core role; the
  adjacent-not-stationed scope is intentional.

Healing applies in this order: building heal → Carrier heal →
Power-Power heal (`/all-hp`, `/foot-hp`) → CO Power heal (e.g.,
Bishop's Air Support `/all-hp +2`). All heal steps cap at HP=10.

**Heal-step locked at start-of-turn** (not end-of-turn): §8.4
modifier-key descriptions previously had `/all-hp` as "end-of-turn"
— corrected here. Single heal-pass per turn, executed before player
action loop.

Automatic and free before player input.

### 12.6 APC resupply

The APC has a unique passive: at start-of-turn, refuel + restock
ammo on every adjacent own unit (up to 6 hexes — one per hex
direction). Heals nothing; only fuel + ammo. APC itself doesn't
need to be stationed on a building; it's a "mobile supply depot."

The APC's `/has-fired = false` precondition does NOT apply — APC
resupply happens passively at start-of-turn alongside building
heals. APC can still move + load/unload normally on the same turn.

### 12.7 Battle Copter carrying

When a Battle Copter has cargo (`/transport-cargo` non-empty), its
effective `/move` is **`floor(base_move / 2) = 3`** (vs base 6).
Penalty applies for the **whole turn**, not just the load-turn.
Once cargo is unloaded, the BCop's move reverts to base on the next
turn.

The BCop carries at most **1 ground unit** (Inf or Mech) — Tank,
Recon, etc. cannot be carried by BCop. Load-action: ground unit
moves onto BCop's hex and is consumed into `/transport-cargo`.
Unload-action: cargo unit moves from BCop's hex to an adjacent legal
ground hex; cargo's `/has-moved` becomes `true` (cannot act further
this turn).

---

## 13. AI v1 (greedy heuristic)

Phase 15. Ships in M5.

### 13.1 Strategy

Greedy per-unit scoring:

1. For each unit, enumerate possible actions (capture, attack, move
   toward objective, defend).
2. Score each action heuristically (capture > attack > advance > defend
   with damage-delta and progress bonuses).
3. Pick highest-scoring action; tie-break deterministically.
4. Execute.

### 13.2 Build decisions

**Phase order (locked 2026-05-16):** the AI's build phase runs
**after** the AI's unit-action phase within the same faction turn.
This means a unit that vacates its build-source hex (HQ / Factory
/ Port) during the action phase frees the hex for a new build in
the same turn — so a Mech built on turn N and moved off-HQ on
turn N+1 doesn't block turn N+1's build at HQ.  Surfaced by the
2026-05-16 tutorial play-by-play where the alternate ordering
(build-then-act) would have produced a multi-turn HQ-stall.  The
2026-05-16 10-round follow-up sim confirmed this is the **common
path**, not an edge case: the HQ-resident unit vacates most turns
once the AI starts pushing forward, so the same-turn-build pattern
fires repeatedly across a match.

After all unit actions, AI v1 spends funds in priority order (Tank > Inf >
Mech > Recon > AA), cycling based on faction's current force gaps.

**Unaffordable-top-tier policy (locked default 2026-05-16):
downgrade-to-next-affordable.**  When the top priority isn't
affordable (e.g., Tank 7k vs current funds 3k on B-min-base 1k/turn
through turn 4), the AI walks the priority list and builds the
highest-priority unit it CAN afford this turn.  Concretely on a
1k/turn map: turn 1 Inf (1k); turn 2 Mech (3k) if cash ≥ 3k else
Inf; turn 3 Mech; turn 4 Recon (4k); turn 5+ Tank.  This keeps the
AI presence-competitive from turn 1 rather than producing a 7-turn
power vacuum.

The save-up policy and per-difficulty selection were considered and
rejected:

| Policy | Why rejected |
| --- | --- |
| Save-up | AI has zero army for ≥ 6 turns on B-min-base; player rolls over AI's HQ; uninteresting matchups |
| Per-difficulty (/easy downgrades, /hard saves up) | /hard should be *stronger* not weaker; save-up makes /hard worse in practice; difficulty knobs already exist (damage cheats §13.4) |

**Phase 5.5A** (post-M5) will tune the priority weights via
self-play and may add per-difficulty cycle ordering (not save-vs-
downgrade selection); the downgrade default stays.

### 13.3 Power activation

Activates at start-of-turn when `floor(power-meter-stars) >= power-cost`.

### 13.4 Difficulty levels

| Level | Effect |
| --- | --- |
| /easy | AI v1 with reduced unit-building budget (skips 1 of every 3 turns) |
| /normal | AI v1 with full budget |
| /hard | AI v1 with full budget + **1.1× damage cheat-modifier on AI's attacks** (this is a cheat-difficulty — not strategic improvement) |

Difficulty set per faction by scenario's `/ai-difficulty`.

`/hard` is **cheat-difficulty**: the +10% damage is a numerical buff,
not improved decision-making. Player-visible note: the side panel
displays "(hard-cheat)" next to the AI faction name when this
difficulty is active, to set expectations. For "actually smarter
AI", use `/ai-v2` or `/ai-v3`.

---

## 14. AI v2 (logic-programmed)

Phase 17. The headline showcase. M7.

### 14.1 Strategy

Logic-programmed tactical evaluation. AI enumerates candidate moves via
`find-all`, scores via Trix predicates, selects via `once`/`aggregate`.

**Engine helper procs used by the AI** (signatures + module per TDD §5):

| Op | Stack signature | Module | Notes |
| --- | --- | --- | --- |
| `move-reachable-hexes` | `unit-id -- Set<[q r]>` | `movement.trx` | All hexes the unit can move to this turn, respecting `/has-moved`, move-range, terrain move-cost (§4), ZOC (§5.14), and transport-cargo penalty (§12.6, §12.7). Includes the unit's current hex iff `/has-moved = false` (i.e., "stay put" is a legal move). Excludes hexes occupied by non-loadable units. |
| `unit-faction-flat-map` | `faction-id { \|unit-id\| -- Array } -- Array` | `unit.trx` | Iterates `/faction-units faction-id get`, applies the proc to each `unit-id`, concatenates results. Empty Arrays are skipped. Used by the AI to enumerate (unit, candidate-move) pairs across the whole faction. |
| `combat-threat-map` | `state -- Dict<[q r] integer>` | `combat.trx` | Per-turn threat scoring (§14.2); shared by AI v2 + AI v3 (§14.10). |
| `ai-score-move` | `[unit-id dest-hex] state -- Real` | `ai_v2.trx` | Heuristic move score (§14.3). |
| `reduce` | `arr init proc -- result` | Trix built-in | Array fold (proc: `acc elem -- acc'`). The AI seeds with element 0 and keeps the higher-scoring of (best, candidate) to select the best entry. |

```trix
% Per AI v2 turn:
threat-map state combat-threat-map per-turn-scratch put

/candidate-moves
    ai-faction
    { |unit-id|
        unit-id move-reachable-hexes
            { |dest-hex| [unit-id dest-hex] }
            map
    } unit-faction-flat-map
local-def

/scored-moves
    candidate-moves
    { |move| move state ai-score-move }
    map
local-def

/best-move
    scored-moves dup 0 get
    { |best cand| cand /score get best /score get gt
      { cand } { best } if-else }
    reduce
local-def

best-move execute-move
```

### 14.2 Threat map

Per-turn transient (cleared on turn-end). Maps each hex to a threat
score:

```
threat-at(hex) = Σ over enemies of {
    damage-they-would-deal-to-typical-defender-at(hex)
    × probability-they-can-reach-attack-position
    × distance-decay-factor
}
```

Computed once at start of AI v2's turn; cached in per-turn scratch dict.

### 14.3 Scoring predicates

The `ai-score-move` predicate is a linear combination of four
components.  Component weights and sub-component values are **locked
in v1** below; M7 commits these as constants AND a self-test
invariant on a fixture scenario (§19 Phase 17).  Treat the values
as a starting point that moves only via spec-amendment commit, not
via in-code tuning.

```trix
% ai-score-move: move state -- score
/ai-score-move {|move state|#+10
    /unit-id    move /unit-id  get   local-def
    /dest-hex   move /dest-hex get   local-def
    /action     move /action   get   local-def

    /value-of-action  action  /ai-action-value get          local-def
    /threat-at-dest   threat-map dest-hex get               local-def
    /damage-potential unit-id dest-hex action
                      predict-damage-potential              local-def
    /counter-cost     unit-id dest-hex action
                      predict-counter-cost                  local-def
    /defensive-bonus  dest-hex state def-stars-at           local-def

    value-of-action 100 mul
    damage-potential 50 mul +
    defensive-bonus 10 mul +
    threat-at-dest 30 mul -
    counter-cost 50 mul -
} def
```

**Component weights (locked v1):**

| Component | Weight | Meaning |
| --- | --- | --- |
| `value-of-action` | 100 | Headline driver. Action-class ranking. |
| `damage-potential` | 50 | Expected enemy HP destroyed (uses §18.6 predict-damage). |
| `counter-cost` | 50 | **Subtracted** — own HP expected lost to the defender's counter-attack (mirror weight of damage-potential, so net-trade-HP is the figure of merit). `0` for indirect attacks (no counter, §7.5). Locked 2026-05-16 after the tutorial-sim play-by-play surfaced the HP-2 chip-suicide pathology — `/attack-chip` with predict-damage 1 and predict-counter 7 (Inf-vs-Tank) used to score positive on `value-of-action × 100` alone; the counter-cost term makes the AI honest about both sides of the trade. |
| `defensive-bonus` | 10 | Terrain stars at destination. |
| `threat-at-dest` | 30 (direct) / 50 (indirect) | Subtracted — incoming **ambient** threat at dest (not from the specific defender of this attack; `counter-cost` covers that).  **Asymmetric weight (added 2026-05-16):** weight bumps to 50 when the attacker is indirect (Artillery / Rocket) since `counter-cost = 0` for indirect attacks (§7.5 no-counter rule); without the bump, indirect units would over-commit into ambient threat zones because the counter-cost term that restrains direct attackers doesn't apply.  At weight 50, the indirect-attacker's threat term has the same relative weight as direct attackers' damage+counter combined. |

**`/ai-action-value` lookup (locked v1):**

```trix
/ai-action-value <<
    /capture-hq        20   % highest -- game-ending
    /capture           15   % city/factory/port capture
    /attack-kill       12   % attack that destroys defender
    /attack-trade      6    % attack that nets favorable HP exchange
    /defend-building   4    % stand fast on faction-owned or contested building (added 2026-05-16)
    /attack-chip       3    % attack that lands but trades poorly
    /advance           2    % move toward objective without acting
    /defend            1    % wait on a strong-defense tile
    /idle              0
>>#r def
```

**Action-class classifier (locked 2026-05-16):** an attack candidate
is classified by predicted-damage and predicted-counter:

| Condition | Class |
| --- | --- |
| `predict-damage == 0` | `/advance` (the 0-damage guard, §14.3 above) |
| `predict-damage ≥ defender's current HP` | `/attack-kill` |
| `predict-damage > predict-counter` (favorable net-HP trade) | `/attack-trade` |
| `predict-damage > 0` AND `predict-damage ≤ predict-counter` | `/attack-chip` (kept around the counter-cost term will still tank the score for genuinely-bad chip attacks) |

A move whose `dest-hex` is a faction-owned or contested (own
capture-in-progress) building classifies as `/defend-building`
instead of `/defend` when EITHER (a) there is no in-range attack
target, OR (b) every in-range attack target has `predict-damage =
0` (i.e., all chip-attack candidates would be reclassified to
`/advance` under the 0-damage guard).  This stops the AI abandoning
captured cities just to `/advance` toward the enemy.  Surfaced
2026-05-16 by the 20-turn tutorial sim: a wounded Hammer Inf on
(12,5) correctly declined a bad attack but then `/advance`d off
the city, letting Bishop re-flip it next turn.

The clause-(b) extension was added 2026-05-16 after a follow-up
Hammer-vs-Boots sim showed an enemy Tank pushing adjacent to a
Boots Inf on (12,5): the Inf's `/attack-chip` reclassified to
`/advance` (predict-damage 0), and `/advance = 2` outranked
`/defend = 1`, walking the Inf off the city it had held for 4
turns.  With clause (b), the Inf stays on the building because
`/defend-building = 4` beats `/advance = 2` even when an enemy is
adjacent — provided the chip-attack is genuinely 0-damage.

`damage-potential` is `predict-damage(...) / 10` (HP scale, integer
0-10), so its 50× weight makes a clean-kill attack worth +60 points
on damage alone — comparable to `value-of-action = /attack-kill × 100
= 1200`, which is the dominant signal. Capture moves at +1500 still
beat a clean kill, so the AI prefers captures when both are available.

**Action-classification guard (locked 2026-05-16):** an attack move
is classified as `/attack-chip` ONLY when `predict-damage > 0`.
A would-be `/attack-chip` with zero predicted damage is reclassified
as `/advance` for scoring (action-value 2 instead of 3) — the move
accomplishes positioning without any attack effect, and treating it
as an attack would feed units into 0-damage chip attacks just
because `/attack-chip = 3` outranks `/advance = 2` in the headline
table.  Surfaced by the 2026-05-16 tutorial-sim play-by-play where
HP-4 Hammer Infs walked into Tank counter-fire for 0 dealt + 6
taken.  Same guard applies to the AI v1 greedy heuristic (§13.1).

### 14.4 Player-selectable performance budget (locked decision 34)

**Two tiers selectable by player:**

| Tier      | Selector                            | Target | Soft cap | Hard cap |
| --------- | ----------------------------------- | ------ | -------- | -------- |
| /standard | `/ai-perf-tier /standard` (default) | 5s     | 10s      | 30s      |
| /patient  | `/ai-perf-tier /patient`            | 10s    | 20s      | 60s      |

Player picks at game-start menu. Scenarios may override default via
`/ai-perf-tier` in `/scenario`. CLI flag `--ai-perf-tier=TIER` overrides
both. Setting persisted in `/game-state.settings.ai-perf-tier`; survives
save/load.

### 14.5 Cap behavior

| Cap      | Wall-clock      | Behavior                                                               |
| -------- | --------------- | ---------------------------------------------------------------------- |
| Target   | (tier-specific) | Full-eval mode: every candidate evaluated via find-all + scoring       |
| Soft cap | (tier-specific) | Top-K prefilter mode: heuristic pre-score; only top K=20 get full eval |
| Hard cap | (tier-specific) | Abort with current-best, log warning, surface UI indicator             |

**Implementation note:** the cap thresholds and mode switches are
implemented inside `ai-think` itself (TDD §4.4 affordance 8).
`ai-think` is **budget-aware and cooperatively yielding** — it tracks
elapsed wall-clock at each candidate-evaluation iteration, flips into
Top-K mode at soft-cap, and returns the current best-scored plan
immediately at hard-cap.  The interruptible-budget contract is
realised inline in game-actor (TDD §4 locks the 3-actor topology
without a separate ai-worker), with periodic `/ai-progress`
actor-sends keeping render-actor responsive — see §14.8.

### 14.6 Techniques permitted in v1

- Top-K prefilter.
- Threat-map memoization within turn.
- `find-n` (N = K) instead of `find-all`.
- `once` for "first acceptable" decisions.

### 14.7 Deferred to AI v2's v2

- Minimax depth-search.
- Coroutine multithreading.
- ML-based eval.

### 14.8 Player-visible UI during AI turn

- "Thinking..." indicator with elapsed-seconds counter shown when AI
  turn exceeds 2s. Spinner animates.
- "Prefilter active" indicator at soft cap.
- "AI move stalled" indicator at hard cap.

These indicators are driven by `/ai-progress` messages that
`ai-think` emits to render-actor every ~100 ms during AI turns (TDD
§4.2 message catalog).  Each message carries elapsed-ms, current
mode (`/full-eval` | `/top-k` | `/stalled`), candidates-evaluated,
and current-best-score; render-actor formats the panel from these
fields without needing to inspect AI internals.  Because the
`actor-send` is itself a yield point in Trix's coroutine scheduler,
input-actor keystrokes (e.g. `Esc` to surface an AI-stall dialog)
are also processed between progress emissions even though the AI
itself runs inline.

### 14.9 Headroom from occupant array

Locked decision 32-C's per-cell occupant array lifts pos-query
operations (~24,000 per AI turn at 75×54) from O(N=80) linear-scan to
O(1). Reclaims ~2-3 seconds of headroom — load-bearing for the
`/standard` tier; lets `/patient` breathe further.

**Phase 0.6D stub-to-real-impl multiplier guidance** (recorded
2026-05-15 for Phase 17 implementers): the stub prototype at
`examples/bestagon-proto/proto-ai-v2-perf.trx` ran a 30-unit
fixture end-to-end at 591 ms on Standard / 864 ms on Epic
(prod build, default 1 MB VM), comfortably within both tier
targets. Real Phase 17 implementation adds ~5-10× the stub's
per-turn cost from these factors:

| Source                                   | Stub-to-real multiplier |
| ---------------------------------------- | ----------------------- |
| §7 damage matrix lookup in ai-score-move | 1.5-2×                  |
| §8 ZOC + §4 terrain-cost in reachable    | 1.2-1.5×                |
| Phase 0.6G A* (vs cube-distance disk)    | 1.2-2×                  |
| Multi-target attack enumeration per move | 1.5-2×                  |
| Fuel/ammo + capture-state guards         | 1.05-1.1×               |
| §11 fog projection for AI                | 1.1-1.3×                |

Implementers seeing Standard at 3-5 s or Epic at 5-8 s during
Phase 17 development should treat that as expected, NOT as a
budget miss. Budget audit happens at M7 ship-gate via the
`proto-ai-v2-perf.trx` graduated bench harness (§19 Phase 17
invariants); /patient tier soft-cap is 20s and hard-cap is 60s
per §14.4.

### 14.10 AI v3 — MCTS + hierarchical planner (optional enemy profile)

Phase 18. Ships in M9. **Opt-in third controller profile** alongside
AI v1 (§13, greedy) and AI v2 (§14.1–9, logic-programmed scoring).
Scenarios opt in per-faction via `/controller /ai-v3` in the faction
declaration. Scenarios that don't declare it never instantiate v3 —
its cost (compute, complexity, casual-player crush risk) is paid only
when a scenario explicitly invokes it.

Default mapping for casual play: `tutorial.bw` and the first 3 demo
scenarios use AI v2; "challenge" scenarios opt into AI v3. The
`--ai-perf-tier=patient` hard-cap (60 s, §14.4) is the load-bearing
perf budget when v3 is active.

#### 14.10.1 Three-layer architecture

Hierarchical planner, replanned every faction turn:

```
┌────────────────────────────────────────────────────────────┐
│ Strategic layer  (game-horizon, ~50 ms / turn)             │
│   Pick active goal: capture-hq / defend / eliminate / etc. │
│   Goal = proc (state faction -- score); find-all + best    │
└──────────────────────────┬─────────────────────────────────┘
                           │ active goal shapes weights
                           ▼
┌────────────────────────────────────────────────────────────┐
│ Operational layer  (multi-turn, ~200 ms / turn)            │
│   Expand active goal into 1–3 sub-goals with deadlines     │
│   Sub-goal = Tagged value w/ success predicate + deadline  │
└──────────────────────────┬─────────────────────────────────┘
                           │ sub-goals contribute scoring bias
                           ▼
┌────────────────────────────────────────────────────────────┐
│ Tactical layer  (per-turn, MCTS — the bulk of the budget)  │
│   Action = full per-turn bundle (NOT per-unit move)        │
│   UCB1 selection; coroutine-parallel rollouts              │
│   Rollout policy = §14.3 `ai-score-move` (reused)               │
└────────────────────────────────────────────────────────────┘
```

#### 14.10.2 Strategic layer

Each turn, AI v3 enumerates the strategic-goal registry and picks the
highest-scoring active goal. Single active goal per faction per turn.

| Goal name                   | Predicate sketch                                             |
| --------------------------- | ------------------------------------------------------------ |
| `/win-by-capture-hq`        | enemy HQ reachable within `min_capture_distance` × 2 turns   |
| `/win-by-elimination`       | enemy unit-cost-sum < own × 0.5 AND no enemy production      |
| `/win-by-objective-control` | scenario declared `/objectives` AND ≥ 1 unowned by AI        |
| `/turn-limit-funds`         | turn limit declared AND own funds + projected income > rival |
| `/defend-hq`                | own HQ capture-progress > 5 OR enemy within 5 hex            |
| `/economy-grow`             | own city count < 3 OR own funds rate < 3000/turn             |

Each goal is a proc with signature `state faction -- score`.
Strategic-layer driver:

```trix
/strategic-goal-registry [
    /win-by-capture-hq /win-by-elimination
    /win-by-objective-control /turn-limit-funds
    /defend-hq /economy-grow
] def

/pick-strategic-goal {|state faction|#+2
    /scored
        strategic-goal-registry
        { |goal-name| [goal-name (goal-name state faction call)] }
        map
    local-def

    scored dup 0 get
    { |best cand| cand 1 get best 1 get gt { cand } { best } if-else }
    reduce
    0 get      % return goal name only
} def
```

Pure §14.1-style `find-all` + `aggregate` idiom. Reuses the §14.3
logic-programmed evaluator pattern.

#### 14.10.3 Operational layer

Given the active strategic goal, expand into 1–3 operational
sub-goals carrying:

- **success predicate** — proc `state -- bool` evaluated at
  start-of-turn
- **deadline turn** — integer; sub-goal expires (and counts as
  failed) past this
- **priority** — `/high | /medium | /low`
- **scoring shaping** — weight multipliers applied to tactical scoring

Example expansion (active goal = `/win-by-capture-hq`):

```trix
/expand-capture-hq {|state faction|
    /enemy-hq-pos    state faction find-enemy-hq-pos      local-def
    /chokepoint-pos  state enemy-hq-pos bridge-on-path-to local-def
    /current-turn    state /turn /turn-number get-path    local-def

    [
        ${ << /tag /establish-front
              /at chokepoint-pos
              /deadline (current-turn 3 +)
              /priority /high
              /shaping << /move-toward-bonus 50 /distance-from-target 1 >> >> } /sub-goal Tagged

        ${ << /tag /build-armor
              /unit-type /tank
              /by-deadline (current-turn 4 +)
              /priority /medium
              /shaping << /build-tank-bonus 30 >> >> } /sub-goal Tagged

        ${ << /tag /protect-vulnerable-cities
              /priority /medium
              /shaping << /defend-city-bonus 20 >> >> } /sub-goal Tagged
    ]
} def
```

Sub-goals live in global VM via `${...}` so MCTS rollouts can
reference them without per-rollout reallocation. At-deadline
expiration triggers strategic-layer replan if no progress.

#### 14.10.4 Tactical layer — MCTS

**Key design decision**: MCTS action = **entire per-turn move bundle**,
not single unit move.

A per-turn bundle is the ordered sequence of (unit moves, build
actions, power activation) that the AI commits at end-of-turn.
Per-unit branching at 75×54/80-units is ~24,000 — tree explodes.
Per-bundle branching is huge in theory but **prunable to top-K** via
heuristic pre-scoring using §14.3 `ai-score-move`.

Tree structure:

```
Node {
    parent-bundle         : bundle | null       % action that produced this node from its parent
    visits                : integer
    total-reward          : real
    children              : [Node ...]          % top-K candidate bundles
}
```

**State representation (locked):** nodes do NOT store a full
game-state copy.  The root's state is the live game-state at AI
turn-start; any descendant's state is **derived on demand** by
applying the path of `parent-bundle` deltas from root down to that
node.  At ~1.4 KB per game-state and thousands of MCTS nodes per
search, per-node state copies would consume tens of MB — the
rollout path instead replays bundles into a single scratch state
held in local VM (sl ≥ 1) and discards it after backprop.

This also makes the parent-bundle field load-bearing: it isn't
decorative metadata, it's the only persistent record of how the
node differs from its parent.  Rollouts mutate a scratch state at
sl ≥ 1; on `restore` the scratch is reclaimed, leaving only the
node's visit count / reward / children for backprop accounting.

Search loop:

```
while time-remaining AND rollout-budget-remaining:
    leaf  = traverse from root via UCB1 until unexpanded
    if leaf.visits = 0: rollout-and-expand
    else:               expand leaf to top-K children; pick one; rollout
    backpropagate reward up to root
return root.best-visited-child
```

**Bundle generation** (top-K children of a node):

1. For each own unit, top-N candidate moves via §14.3 `ai-score-move`
   (N=3 default).
2. For each Factory/Port/HQ with funds, top-3 build candidates
   (or skip-build).
3. Power activation: activate-now / hold (binary).
4. Greedy bundle assembly: respect ZOC, occupant array, no two units
   same destination hex.
5. Score K=12 default bundles by summed `ai-score-move`; UCB1 picks
   among them.

**UCB1 selection**:
`argmax_a (Q(s,a) / N(s,a) + c × sqrt(ln N(s) / N(s,a)))`, **c = 1.4**.

**Rollout policy** (from leaf state, play forward H=4 turns):

- Both factions play §14.3 greedy `ai-score-move` selection.
- Score terminal state via state-evaluator:
  ```
  V(s) = own_unit_cost_sum + own_building_count × 5000
       − enemy_unit_cost_sum − enemy_building_count × 5000
       + own_HP_total × 100 − enemy_HP_total × 100
  ```
- Reward = `clamp((V(leaf) − V(root)) / 50000, −1, +1)`.

**Stop conditions**:

- Wall-clock cap hit
- Rollout-count cap hit
- Best action visited > 50% of total rollouts (convergence)

#### 14.10.5 Coroutine parallelism

Master/worker pattern using Trix coroutines:

```
+--------+       leaf-states       +-----------+
| Master |  ───────────────────▶   | Worker 1  |
| (tree) |  ◀───────────────────   |  rollout  |
+--------+      reward + path      +-----------+
     │  ◀───────────────────────▶  | Worker 2  |
     │                             +-----------+
     │                             | Worker 3  |
     │                             +-----------+
     │  ... N workers (default 4)
```

- **1 master coroutine** owns the MCTS tree (in global VM,
  shared-by-reference).
- **N worker coroutines** receive `(leaf-state, depth-budget)` via
  mailbox; return `(reward, terminal-V)` via reply mailbox.
- Master selects next leaf, dispatches to free worker, awaits any
  worker via `find-n` barrier (N=1 means "first to return").
- Backpropagation single-threaded on master; tree mutation never
  concurrent.

Showcase angle: **the actor-pattern + coroutine + global-VM +
`find-n`-barrier combo** doing real MCTS work-stealing — no other
Trix example exercises all four together.

#### 14.10.6 Difficulty tiers

The §13.4 `/ai-difficulty` selector gains tiers under AI v3:

| Tier      | Layers active               | MCTS depth | Rollouts | Workers | Typical wall-clock (Epic) |
| --------- | --------------------------- | ---------- | -------- | ------- | ------------------------- |
| `/easy`   | tactical only (no MCTS)     | 0          | 0        | 0       | ~2 s                      |
| `/normal` | strategic + tactical (MCTS) | 1          | 200      | 2       | ~10 s                     |
| `/hard`   | all 3 layers                | 2          | 800      | 4       | ~25 s (`/standard`)       |
| `/brutal` | all 3 layers, max budget    | 2          | 2500     | 6       | ~55 s (`/patient`)        |

`/easy` AI v3 is intentionally similar in cost to AI v2 (~2 s) — the
difference is that strategic-goal shaping reweights `ai-score-move`,
making decisions slightly more coherent across turns. Casual-friendly
entry point. `/brutal` requires `--ai-perf-tier=patient`.

#### 14.10.7 Scenario opt-in

```trix
/factions [
    << /id 0 /controller /human /co /bishop ... >>
    << /id 1 /controller /ai-v3 /ai-difficulty /hard /co /hammer ... >>
] def
```

Scenarios that don't declare `/ai-v3` default to `/ai-v2`. AI v3 is
**opt-in per-faction**, not global — scenarios can mix profiles
(one v3 enemy + one v1 ally, etc.).

**Mixed-profile validator warning**: when a scenario declares
`/ai-v3` alongside any `/ai-v1` OR `/ai-v2` controller in the same
`/factions` array, the loader emits a `/scenario-warning`
(non-fatal) noting that the profiles have markedly different
strength tiers and the match may be unbalanced.  Players see this
in the briefing screen.  The warning does NOT block load.

The v3+v2 case still warns because §19 Phase 18 invariant locks
"AI v3 hard wins ≥ 70 % of 25×18 standard-CO matches vs AI v2";
a 30 % opposition win-rate gap is enough to call the match
unbalanced.  The warning does NOT fire for v1+v2 mixes (those tiers
are close enough that scenario authors might intentionally pair
them).

Similarly, `/ai-difficulty /brutal` is only legal with `/ai-v3`
controllers; the validator rejects `/brutal` for v1 / v2.

#### 14.10.8 Showcase angles unique to AI v3

| Layer / mechanic         | Trix idiom showcased                                               |
| ------------------------ | ------------------------------------------------------------------ |
| Strategic goal selection | `find-all` over goal-procs, `reduce` for best                      |
| Operational sub-goals    | Tagged values + global-VM persistence + deadline integer compare   |
| MCTS tree state          | Dict of Tagged nodes; visit/reward as Records                      |
| UCB1 selection           | `find-n` + scoring lambda; coroutine-yielding loop                 |
| Rollout policy           | Reuses §14.3 `ai-score-move` (clean v2/v3 factoring)               |
| Worker pool              | Coroutine actor pattern; mailbox dispatch; `find-n` result barrier |
| Tree-state in global VM  | `${...}` for the tree root; survives turn-end, GC at faction swap  |

Seven distinct Trix subsystems exercised, vs AI v2's four. AI v3 is
the single best Trix-subsystem-integration showcase in the entire
example library if shipped.

#### 14.10.9 Deferred to AI v3's v2

- **Opponent modeling**: track human/AI move tendencies across turns;
  bias rollout policy.
- **Theory-of-mind rollouts**: predict opponent's move via their
  inferred eval, not greedy `ai-score-move`.
- **Asymmetric rollout horizon**: deeper own-side rollouts, shallower
  opponent-side.
- **Neural value head** for terminal-state `V(s)` — explicitly
  rejected (single-file `.trx` constraint).
- **Cross-game tactic library**: persist won-position patterns across
  saves (the AlphaZero-lite move).
- **Information-set MCTS** for fog-of-war perfect-info → imperfect-info
  transition.

---

## 15. Save and load

### 15.1 Save format

Phase 14. **Custom `.bws` binary file containing ONLY the game-state
Dict, serialised via Trix `to-binary-token`. NOT a whole-VM
`snap-shot`.** Save files live at `./saves/` by default (gitignored;
`--save-dir=path` overrides; auto-created if absent).

Why custom instead of `snap-shot`: smaller files (~60 KB vs
300–500 KB), survives Bestagon source patches (data only, no
bytecode), survives Trix engine bumps (depends on `to-binary-token`
stability, not `SNAPSHOT_VERSION`), and is auditable (versioned
header + named-field schema vs opaque binary blob). Full byte-layout
and load/save mechanics are pinned in **TDD §10** (the authoritative
implementation spec); this GDD section documents player- and
scenario-author-facing behaviour.

The persisted body is a Dict with named keys: `/version`,
`/scenario-name`, `/master-seed`, `/turn-number`, `/current-faction`,
`/weather`, `/map`, `/units`, `/faction-units`, `/factions`, plus
per-system RNG slots (`/combat-rng-state`, `/mapgen-rng-state`,
`/ai-rng-state`, `/cosmetic-rng-state`, `/event-rng-state`). See
TDD §10.2 for types and provenance.

Excluded (re-derived or transient): loaded modules (`require`
rebuilds), coroutine stacks (actors re-spawn from `main.trx`),
exec/dict stacks (empty at sl=0 save points), `/config` Dict
(re-parsed from CLI), all render/input/combat/AI/pathfind scratch
(local-VM, doesn't exist between turn brackets).

### 15.2 Save version stamp (locked)

The save file carries two version markers:

1. **Header byte** at offset 4: `format-version` (uint8). The
   on-disk format-version of the BWS container itself. Currently
   `0x01`.
2. **Body field** `/version` (UInteger): cross-checked against the
   header byte on load. Bumped whenever the persisted-state schema
   changes incompatibly — a renamed key, removed key, changed
   value-type for an existing key, or a changed game rule whose
   semantics are embedded in persisted data (e.g., capture threshold
   goes from 20 to 25).

Loader behavior:

1. Read 4-byte magic `"BWS\0"`. Mismatch → `/bws-magic-mismatch` error.
2. Read header `format-version` byte.
3. Match against engine's compiled-in `BWS-FORMAT-VERSION` constant.
4. Deserialise body Dict via `token` (the binary-token wire decoder).
5. Cross-check body `/version` against header byte; mismatch →
   `/bws-version-cross-check-failed` error.
6. Match → load proceeds (bind state slots, §15.3).
7. Mismatch → reject with error: `(Save file version VS is incompatible
   with engine version VE; cannot load.)`. No silent partial-load, no
   auto-migration in v1.

Engine constant `BWS-FORMAT-VERSION` lives near the top of
`save.trx` as `1` for the v1 release; bump on every persisted-shape
change. CHANGELOG entry required at the same commit.

**Mismatch UX path** (locked):

1. Loader reads version, detects mismatch.
2. Loader prints the error message (above) to stderr AND to the
   player-visible side panel for ≥ 5 seconds.
3. Engine renames the incompatible save file from `X.bws` to
   `X.bws.orphan-vN` (where N is the file's body `/version`).
4. Auto-save in particular: `autosave.bws` → `autosave.bws.orphan-vN`.
5. Engine returns to the main menu / scenario selector.

The orphan files are preserved (not deleted) so the player can
manually inspect or send to support, but the engine will not attempt
to load them. Scenario authors who need to test cross-version
loading can use the `--ignore-save-version` CLI flag (developer-only;
no support guarantee).

### 15.3 Load

Loading is a **fresh process invocation** of `main.trx` with
`--load=PATH` (TDD §10.4):

```
./trix examples/bestagon/main.trx -- --load=saves/quicksave.bws
```

`main.trx` parses CLI, `require`s every module (rebuilds engine
state from current source), then branches on `--load` vs
`--scenario`. On the `--load` branch the loader reads the `.bws`
file via `bws-load`, validates the header + body cross-check
(§15.2), and binds each state slot (`/scenario-name`,
`/master-seed`, `/turn-number`, `/current-faction`, `/weather`,
`/map`, `/units`, `/faction-units`, `/factions`, all RNG slots) from
the loaded Dict. Three actors spawn; game-actor enters the main
loop at the saved turn boundary.

**Win-conditions on load** (locked): the loader does **not** rely on
the win-conditions array that was persisted in the save file (it
isn't persisted; the body schema in §15.1 explicitly excludes
`/win-conditions`). Instead, the loader re-runs the source scenario
file resolved from the persisted `/scenario-name`, which rebinds the
scenario's userdict-level win-condition declarations. The
`/win-conditions` array is then re-resolved from the freshly-rerun
userdict; Tagged inline procs come from the scenario re-run, not
from the save.

This avoids the double-bookkeeping risk of "persisted procs vs. fresh
scenario-file procs"; the scenario file is authoritative for
named-predicate bodies, the save file is authoritative for runtime
state. Save files store `/scenario-name` (resolved to a `.bw` path
at load time), NOT the proc bodies; rename/move a scenario file and
old saves cannot load (clean failure, not silent stale-behavior).

### 15.4 Auto-save

At end of every player faction's turn (after win-condition check,
between turn brackets at sl=0), engine writes current state to
`./saves/autosave.bws`. Overwrites previous. Game-actor calls
`save-game` synchronously between brackets; target write latency
≤ 100 ms for the ~60 KB body. `--no-autosave` flag disables.

### 15.5 Player-triggered save/load

| Action               | Keybind      | Filename                                |
| -------------------- | ------------ | --------------------------------------- |
| Quick save           | F5           | `quicksave.bws` (overwrite)             |
| Quick load           | F9           | `quicksave.bws` (re-exec via `--load=`) |
| Save to slot N (1-9) | F2-F10 menu  | `saveN.bws`                             |
| Named save (cmd)     | `:save NAME` | `NAME.bws`                              |
| Named load (cmd)     | `:load NAME` | `NAME.bws`                              |

All filenames resolved against `./saves/`. Quick-load and named-load
operate by re-exec'ing the engine with `--load=<resolved-path>` — the
running process does not attempt to thaw in place (consistent with
the fresh-process load model, §15.3).

Additional reserved filename: `crash.bws` (uncaught error from sl=0
frame, §17.7 of TDD).

### 15.6 Constraints

- The save file is a custom binary container; **not** a Trix VM
  snapshot. Bytecode, stacks, and engine internals are NOT persisted.
- Constants in global VM are re-built by `require`-ing modules; saves
  carry only mutable game state.
- Replay log: not in v1 (deferred to v2 as `.bwr` variant of the same
  schema; TDD §10).
- Fog cache is recomputed on demand from `/map` + `/units` + per-faction
  vision; no fog state persisted.
- Occupant grid (TDD §3.2) is rebuilt from `/units` on load;
  `validate-state` confirms post-load consistency.

---

## 16. Scenario file format

### 16.1 Top-level names expected in userdict after `run`

| Name | Required? | Type | Purpose |
| --- | --- | --- | --- |
| `/scenario` | required | readonly Dict | Main metadata + map + references |
| `/factions` | required | Array of Dicts | One entry per faction |
| `/buildings` | required | Array of Dicts | One entry per building |
| `/starting-units` | required | Array of Dicts | One entry per pre-placed unit |
| `/terrain-key` | required | Dict | 1-char String → terrain-type Name (see §16.7 note on key form) |
| `/damage-overrides` | optional | Dict-of-Dicts | Per-cell damage table overrides |
| `/unit-cost-overrides` | optional | Dict | Per-unit cost overrides |
| `/co-meter-overrides` | optional | Dict | Meter-config overrides |
| `/co-roster-extras` | optional | Dict-of-Dicts | Additional COs beyond default 6 |
| `/weather-cycle` | optional | Array of Names | Per-turn weather rotation (see §11.5); absent = perpetual `/clear` |
| `/weather-table-extras` | optional | Dict | Custom weather types beyond the built-in 4 (§11.5) |

Loader runs `validate-scenario` after `run`. Missing required name or
shape mismatch raises `/scenario-error`.

### 16.2 `/scenario` Dict schema

```trix
/scenario <<
    /name           (string)              % required
    /description    (string)              % optional
    /author         (string)              % optional
    /map-size       [W H]                 % required
    /map            [string ...]          % required; H strings, each W chars
    /terrain-key    terrain-key           % required
    /factions       factions              % required
    /buildings      buildings             % required
    /starting-units starting-units        % required
    /fog-of-war     true | false          % optional; default true (AW skirmish model, locked decision 15)
    /turn-limit     null | integer        % optional; default null
    /win-conditions [/cond1 ...]          % required
    /briefing       (multi-line string)   % optional; `\n` inside the string is interpreted as a newline by `scen-load` (post-parse expansion, not Trix-native string-escape) — author with `\n` between paragraphs
    /objectives     [[col row] ...]       % optional; required iff /objective-locations in /win-conditions
    /luck-mode      /off | /on            % optional; default /off
    /rng-seed       integer               % optional; default current-time
    /ai-perf-tier   /standard | /patient  % optional; default /standard
>>#r def
```

**`/weather-cycle` and override Dicts** (`/damage-overrides`,
`/unit-cost-overrides`, `/co-meter-overrides`, `/co-roster-extras`,
`/weather-table-extras`) live at **top-level userdict** (§16.1), NOT
inside `/scenario`. The `/scenario` Dict carries only required
metadata + reference-by-Name to entity arrays (`/factions`,
`/buildings`, `/starting-units`, `/terrain-key`).

### 16.3 Win conditions (everything-is-a-proc model)

Each entry in `/win-conditions` is either a bare Name (resolved
userdict-first then built-in-registry) OR a Tagged predicate
(`proc /name Tagged`).

Built-in registry:

| Name | Behavior |
| --- | --- |
| `/capture-hq` | True when any enemy HQ is owned by `faction` |
| `/eliminate-all-enemies` | True when no enemy units AND no enemy buildings remain |
| `/turn-limit-funds` | At turn limit: true for faction with greatest `/cumulative-funds-earned` (§17.2). **Tie-break**: current `/funds`, then lowest `/id` (deterministic). If still tied (rare; identical economies), `/game-status.state` flips to `/draw` rather than picking arbitrarily. |
| `/turn-limit-territory` | At turn limit: true for faction with most cities. **Tie-break**: total building count (cities + factories + ports + HQ), then lowest `/id`, then `/draw`. |
| `/objective-locations` | True when `faction` "controls" every hex in `/objectives` list — see definition below |

**"Controls" definition** (locked, for `/objective-locations`):
faction controls a hex iff there is currently an **own unit standing
on it** (cargo inside a transport does not count — the transport
must be the unit on the hex). Control is checked at win-condition
evaluation time (end-of-turn). Hex doesn't need to have been
captured or owned via building rules — pure "currently occupied by
my unit" semantics.

Each is a proc with signature `state faction -- bool`. Scenarios may:

1. Reference a built-in by Name.
2. Override a built-in by `def`'ing the same Name.
3. Define a new proc with a new Name; reference by that Name.
4. Inline via Tagged: `proc /name Tagged`.

Default when omitted: `[/capture-hq /eliminate-all-enemies]`.

### 16.4 Scenario overrides

Optional top-level userdict names; loader detects via `userdict known?`
and shallow-merges onto canonical defaults. All scenario overrides
listed in §16.1 are constant scenario data — wrap each in `${...}` so the override Dict
lives in the global VM region (journal-skipped, survives save/restore
without re-allocation, reclaimed by `vm-global-gc` when no longer
referenced).

```trix
${ <<
    /tank << /tank 60 >>
>> } /damage-overrides def

${ <<
    /infantry 1500
>> } /unit-cost-overrides def

${ <<
    /threshold-funds-per-star 3000
>> } /co-meter-overrides def

${ <<
    /talon <<
        /callsign (Talon)
        /color    /cyan
        ...
    >>
>> } /co-roster-extras def
```

### 16.5 Coordinate convention

`[col row]` 2-element UInteger array (locked decision 38), 0-indexed,
**flat-top + odd-q offset**. Top-left = `[0 0]`; bottom-right of a
25×18 map = `[24 17]`.

Map ASCII strings read top-to-bottom matching `row 0..H-1`; within each
row, characters read left-to-right matching `col 0..W-1`. Even and odd
columns have the SAME row count — the brick-stagger by column parity is
render-time only.

### 16.6 Loader and validation

```trix
% load-scenario: path -- game-state
/load-scenario {|path|
    path run                      % executes file; top-level names land in userdict
    validate-scenario
    build-initial-game-state
} def
```

The loader uses `run` (single-shot execute) rather than `require`
(idempotent run-once) so a scenario can be re-loaded after `clear-state`
or for save/load round-trips without `require`'s realpath-based dedupe
suppressing the re-load.

`validate-scenario` checks:

- `/scenario` is a Dict, contains every required key.
- `/map-size` is `[W H]` with W, H positive integers ≤ 100.
- `/map` array length = H; each string length = W.
- Every character in `/map` appears as a key in `/terrain-key`.
- Every building/unit `/pos` is in map bounds.
- Every building/unit on legal terrain (no ground unit on Sea; etc.).
- **Building-terrain placement rules** (locked):
  - HQ, City, Factory: must be on a **land** terrain (Plain, Road,
    Bridge, Beach, Forest, Mountain — NOT Sea/Reef/River).
  - Port: must be on **Sea** or **Beach** terrain (NOT inland).
- Every faction has at least one HQ.
- Every `/owner` references a valid faction id (integer ≥ 0) or `-1` (neutral). Scenario files MAY use the syntactic sugar `/neutral` which the loader converts to `-1`.
- Every CO Name in `/factions` exists in `/co-roster` or `/co-roster-extras`.
- No duplicate `/pos` across buildings + units.
- `/starting-units` count ≤ 254 (the system-wide unit-id cap; §3 unit-count cap).
- Every CO defined in `/co-roster-extras` (§16.4) satisfies `super-cost = power-cost + 4` (the §8.2 Super-cost rule).
- Every `/win-conditions` entry resolves to a callable proc.
- If `/objective-locations` is in `/win-conditions`, `/objectives` declared.
- `/ai-difficulty /brutal` is only legal when `/controller /ai-v3`.

**Error format** (locked): errors are reported as a single
text block in the form:

```
/scenario-error in <scenario-path>:
  [<error-index>] <severity> <message-class>: <message>
                  at <location-hint>
  ...
  <N> error(s) total. Loading aborted.
```

Severity is `/error` (always — there are no warnings in v1
validation). Message-class is one of:
`/missing-key`, `/wrong-type`, `/out-of-bounds`, `/duplicate`,
`/illegal-placement`, `/unknown-name`, `/missing-dependency`,
`/precondition`. Location hint is the offending `[col row]` or
`/faction-id N` or similar.

Errors are collected across the entire scenario file in one
validation pass; the message lists ALL detected errors, not just
the first.

### 16.7 Tutorial scenario file

**Note on `/terrain-key` keys** (locked 2026-05-15 Phase 0.6F): keys
are **1-char Strings**, not Names. The earlier `/. /plain` form is
invalid in Trix: the 2026-04 field-access sugar (`.field` →
`/field get`) makes `/.` an invalid Name literal — the scanner errors
`expected digit or field name after '.'`. String-keyed Dicts work
identically to Name-keyed Dicts for lookup (`d (.) get` returns the
value); the loader's `check-terrain-coverage` reads each key as a
1-char String and indexes byte 0 to obtain the map glyph.

```trix
% examples/bestagon-scenarios/tutorial.bw -- Bestagon Wars introductory scenario.

/terrain-key <<
    (.) /plain   (^) /mountain  (*) /forest  (~) /sea  (_) /beach
    (B) /bridge  (R) /road      (|) /river   (;) /reef
>> def

/factions [
    << /id 0 /color /white /co /bishop /starting-funds 7000 /controller /human                          >>
    << /id 1 /color /red   /co /hammer /starting-funds 0    /controller /ai-v1  /ai-difficulty /easy >>
] def

/buildings [
    << /pos [12  1] /type /hq   /owner 1        >>
    << /pos [13  1] /type /city /owner 1        >>
    << /pos [12  5] /type /city /owner -1 >>
    << /pos [ 8 13] /type /city /owner -1 >>
    << /pos [16 13] /type /city /owner -1 >>
    << /pos [12 16] /type /hq   /owner 0        >>
    << /pos [11 16] /type /city /owner 0        >>
] def

/starting-units [
    << /pos [10  3] /type /infantry /owner 1 >>
    << /pos [14  3] /type /infantry /owner 1 >>
    << /pos [ 9 13] /type /infantry /owner 0 >>
    << /pos [15 13] /type /infantry /owner 0 >>
] def

/scenario <<
    /name        (Tutorial Beach)
    /description (Capture neutral cities, build a tank, push to enemy HQ.)
    /author      (Bestagon Wars Team)
    /map-size    [25 18]
    /map [
        (.........................)
        (.........................)
        (.........................)
        (.........................)
        (.........................)
        (.........................)
        (......^^^.......^^^......)
        (.....^^*^^.....^^*^^.....)
        (......^^^.......^^^......)
        (.........................)
        (~~~~~~~~~~~~B~~~~~~~~~~~~)
        (.........................)
        (.........................)
        (.........................)
        (.....***.......***.......)
        (....*****.....*****......)
        (.........................)
        (.........................)
    ]
    /terrain-key    terrain-key
    /factions       factions
    /buildings      buildings
    /starting-units starting-units
    /fog-of-war     false
    /turn-limit     null
    /win-conditions [/capture-hq /eliminate-all-enemies]
    /briefing       (Welcome to Bestagon Wars.  You command Bishop's balanced army (white).  Capture the neutral cities for +1000 funds/turn each, then push to Hammer's HQ in the north.\n\nTwo things the game does NOT spell out on-screen:\n  * Captures take TWO turns at full HP -- city income flips in the day AFTER you arrive on the building, not the same day.  Don't panic if turn-2 funds look low; turn-3 is where the economy opens up.\n  * Moving next to an enemy unit halts the rest of your move that turn (zone-of-control).  When pushing across the central bridge, screen your tank with an infantry FIRST so the ZOC zone is yours, not theirs.)
>>#r def
```

### 16.8 `/procgen-spec` scenario shorthand (locked Phase 0.7)

Scenarios may omit explicit `/map` / `/buildings` / `/starting-units`
in favor of a `/procgen-spec` block inside `/scenario`. The loader
runs the §3.4 procgen pipeline with the spec's knobs to generate
terrain + buildings; the resulting state is structurally identical
to a hand-authored scenario.

```trix
/factions [
    << /id 0 /color /white /co /bishop /starting-funds 7000 /controller /human >>
    << /id 1 /color /red   /co /hammer /starting-funds 0    /controller /ai-v1 /ai-difficulty /normal >>
] def

/scenario <<
    /name        (Random skirmish #42)
    /description (Procgen-default density, mirror-h symmetry)
    /map-size    [50 36]
    /procgen-spec <<
        /seed             42ul
        /sea-pct          30
        /density-preset   /default
        /symmetry         /mirror-h
        /mountain-clusters 3
        /forest-clusters   5
        % Knobs not listed inherit §3.4 defaults
    >>
    /factions       factions
    /fog-of-war     true
    /turn-limit     null
    /win-conditions [/capture-hq /eliminate-all-enemies]
>>#r def
```

`validate-scenario` checks:

- Exactly one of `/map` (hand-authored) OR `/procgen-spec` (generated) is present.
- `/procgen-spec /size` (or `/map-size`) gives `[W H]` per §3 bounds.
- `/procgen-spec /symmetry` is in `{/none, /mirror-h, /rotational-180}`.
- `/procgen-spec /density-preset` is in `{/sparse, /default, /dense}`.
- All other §3.4 knobs are Integer or ULong.
- `/factions` length matches `/procgen-spec /n-factions` (default 2).

After loading: the engine populates `/scenario` with synthetic `/map` /
`/buildings` / `/starting-units` arrays as if hand-authored. The
original `/procgen-spec` is preserved for save/load reproducibility
(re-running procgen with the same spec yields a byte-identical map).

**Replay determinism**: a procgen scenario + replay log together fully
reproduce the game (procgen seed → map; combat seed → rolls; action log
→ moves).

**`--mapgen` CLI mode** (Phase 1 entry point): `bestagon --mapgen
--seed 42 --size 50x36 --preset default --symmetry mirror-h` produces
a procgen scenario file to stdout (or `-o output.bw`). Useful for
"seed cycling" UX where the player browses generated maps before
committing to one.

---

## 17. Game state shape

This section specifies the **logical schema** of game state — what
data exists and how subsystems refer to it (locked decision 32).
The **physical storage layout** (SoA `Packed<Byte>` cell grids,
Dict-by-unit-id for units, parallel SoA arrays for factions,
separate global slots for turn-state / events / scenario overrides)
is pinned in **TDD §3** as the authoritative implementation spec.

Treat the field listings below as a single logical view.  The impl
may split fields across multiple global slots; accessor procs
(`cell-terrain`, `unit-q`, `faction-funds`, etc., per TDD §3) hide
the storage detail from gameplay code.

### 17.1 Top-level (logical view)

```
game state =

    /scenario-name            string                % matches save body /scenario-name (§15)
    /scenario-author          string
    /briefing                 string

    /map                                            % per-cell grids; storage TDD §3.1
        /width                integer
        /height               integer
        /terrain              per-cell terrain-type Name (§A.2)
        /occupant             per-cell unit-id or "none" sentinel
        /owner                per-cell faction-id (building cells only) or -1
        /capture-progress     per-cell 0–20 (building cells only)
        /capturer-id          per-cell unit-id or "none"
        /fog                  per-cell vision state (§11.3)
        /terrain-key          Dict char → terrain Name; used at scenario load only

    /factions                 per-faction state (§17.2); storage TDD §3.3
    /units                    per-unit state    (§17.4); storage TDD §3.2
    /buildings                logical view of building cells (§17.3); see TDD §3.4 index

    /turn
        /day                  integer (1-indexed)
        /current-faction      integer (faction id of active player)
        /turn-number          integer (cumulative, 1-indexed)
        /phase                /move | /build | /end-of-turn

    /win-conditions           [proc proc Tagged ...]  % rebuilt from scenario re-run on load (§15.3)

    /game-status
        /state                /running | /victory | /draw | /quit-requested
        /winner               integer | null

    /events
        /transient            ring buffer, last ~20 events
        /replay-log           v2 only — `.bwr` format (§15.1)

    /scenario-overrides
        /damage-overrides
        /unit-cost-overrides
        /co-meter-overrides
        /co-roster-extras
        /weather-cycle        [/clear /rain ...]     % see §11.5
        /weather-table-extras                        % see §11.5

    /settings
        /ai-perf-tier         /standard | /patient

    /next-unit-id             integer                % monotonic 0-254; reused after a unit-id is freed by death (per TDD §3.2)
                                                    % (no /next-building-id — buildings have no separate id; they are cells per §17.3)

    /combat-rng-state         ULong                  % §9.3; in save body (§15)
    /mapgen-rng-state         ULong                  % §9.3
    /ai-rng-state             ULong                  % §9.3
    /cosmetic-rng-state       ULong                  % reserved
    /event-rng-state          ULong                  % reserved

    /weather-current          Name | absent          % present iff scenario declares /weather-cycle
    /weather-cycle-idx        integer | absent       % index into /weather-cycle
    /weather-day-last-cycled  integer | absent       % day value when cycle last advanced (§11.5)

    /version                  integer                % save body version; see §15.2
```

### 17.1.1 Coordinate convention (locked)

Hex coordinates are `(q, r)` per §3 (flat-top + odd-q offset).  The
brick-stagger by column parity is a **render-time** transformation
only (§3, §18); the underlying cell space is a rectangle of `H`
rows × `W` columns regardless of parity.

The physical map storage (TDD §3.1) flattens cell `(q, r)` to a
single index via row-major mapping:

```
idx        = r × W + q
q          = idx mod W
r          = idx div W
in-bounds  = (0 ≤ q < W) AND (0 ≤ r < H)
```

`cell-idx` (TDD §3.1) is the canonical converter; gameplay code
goes through named accessors (`cell-terrain`, `cell-set-terrain`,
etc.) and never indexes the grids directly.

### 17.2 Faction state (logical fields)

Each faction has these logical fields (storage as parallel SoA
arrays per TDD §3.3):

| Field | Type | Notes |  |  |  |  |
| --- | --- | --- | --- | --- | --- | --- |
| `/id` | integer | 0-indexed |  |  |  |  |
| `/color` | Name | per §A.4 palette |  |  |  |  |
| `/co` | Name | one of §8.1 roster, or scenario-extra |  |  |  |  |
| `/controller` | Name | `/human | /ai-v1 | /ai-v2 | /ai-v3` |  |
| `/ai-difficulty` | Name or null | `/easy | /normal | /hard | /brutal | null`; `/brutal` valid only with `/ai-v3` |
| `/funds` | integer | current balance |  |  |  |  |
| `/cumulative-funds-earned` | integer | lifetime total (§16.3 `/turn-limit-funds` win cond) |  |  |  |  |
| `/power-meter-stars` | real | 0.0 .. 12.0 (§9.1) |  |  |  |  |
| `/power-active` | boolean | regular Power active this turn |  |  |  |  |
| `/super-active` | boolean | Super Power active this turn (mutually exclusive with `/power-active` per §9.1) |  |  |  |  |
| `/power-used` | boolean | at-least-one Power has fired this game |  |  |  |  |
| `/units-built` | integer | running totals; surfaced in status / replay export |  |  |  |  |
| `/units-killed` | integer |  |  |  |  |  |
| `/units-lost` | integer |  |  |  |  |  |
| `/buildings-captured` | integer |  |  |  |  |  |
| `/buildings-lost` | integer |  |  |  |  |  |
| `/total-damage-dealt` | integer |  |  |  |  |  |
| `/total-damage-taken` | integer |  |  |  |  |  |
| `/defeated?` | boolean | flips true when faction is eliminated (§10.4) |  |  |  |  |

**`/power-active` vs `/super-active`** (locked): at most one of these
is `true` at any time, per §9.1. The resolver order (§8.4) checks
`/super-active` first; if `true`, applies Super-modifier set and
skips Power. Both clear at end-of-turn.

No fog cache (recomputed on demand per locked decision 32-D).

### 17.3 Building state (logical fields)

Buildings are not separate entities — they are **cells whose terrain
type is a building type** (`/hq | /city | /factory | /port`).  The
per-cell state below lives in the cell grids (TDD §3.1); a
per-faction index (TDD §3.4) gives O(1) iteration of "all buildings
of faction X" for end-of-turn income, fog reveal, etc.

| Field | Type | Notes |  |  |  |
| --- | --- | --- | --- | --- | --- |
| `/type` | Name | one of `/hq | /city | /factory | /port`; same value as the cell's terrain |
| `/pos` | (q, r) | the cell's coordinate; implicit in the grid index |  |  |  |
| `/owner` | integer | -1 = neutral; ≥ 0 = faction id (locked: integer, NEVER Name) |  |  |  |
| `/capture-progress` | integer | 0-20 (§10.3); 0 = not being captured |  |  |  |
| `/capturer-unit-id` | integer or null | unit currently capturing, else null |  |  |  |

`/owner = -1` is the sentinel for neutral (was `/neutral` Name in
earlier drafts; normalized 2026-05-16 to integer-only for
implementation simplicity). All faction-id comparisons use integer
equality.

### 17.4 Unit state (logical fields)

Per-unit fields (storage as Dict-by-id with separate `/q`/`/r`
fields per TDD §3.2; a `/faction-units` index gives faction →
unit-id list):

| Field | Type | Notes |  |
| --- | --- | --- | --- |
| `/id` | integer | 0-254 byte (system-wide cap, §3 "Unit count cap"); 255 reserved as "none" sentinel in cell grids |  |
| `/pos` | (q, r) | current hex; when `/transport-of != null`, mirrors transport's `/pos` |  |
| `/type` | Name | from §6 unit roster |  |
| `/owner` | integer | faction id |  |
| `/hp` | integer | 1-10 |  |
| `/fuel` | integer | every unit tracks fuel per the §6 unit roster; the per-turn tick rate (§12.4) is 0 for ground (fuel is "soak"), 2 for air, 1 for sea, 5 for submerged Submarines |  |
| `/ammo` | integer or null | null for units that don't attack (APC, Lander, Carrier); also `null` (i.e., infinite) for machine-gun-only Inf/Recon per §6 |  |
| `/has-moved` | boolean |  |  |
| `/has-fired` | boolean |  |  |
| `/has-captured` | boolean |  |  |
| `/transport-of` | integer or null | id of carrying transport, else null |  |
| `/transport-cargo` | [unit-id ...] | unit ids carried (empty for non-transports) |  |
| `/built-this-turn` | boolean | set by build action; reset at next start-of-turn; prevents Lightning Drive factory-rush |  |
| `/kills` | integer | §7.8 veterancy; default 0; increments on kill |  |
| `/vet-tier` | integer | 0-3; recomputed from `/kills` on each kill |  |
| `/sub-state` | Name | `/submerged | /surfaced`; Submarine ONLY (absent for other unit types per §11.2) |

**`/pos` of a carried unit** (locked): when `/transport-of != null`,
the carried unit's `/pos` mirrors the transport's current `/pos`.
This means the carried unit moves with the transport without
requiring a separate update path. The unit is **not** in the
`/occupant` cell-grid slot (§17.6) — the transport occupies the
hex; the cargo is "inside" the transport.

**`/built-this-turn`** (locked 2026-05-16): set to `true` when the
build action creates the unit; reset to `false` at the next
start-of-turn for that unit's faction. Talon's Lightning Drive
(`/air-extra-action`, air-only since 2026-05-16) excludes air
units with `/built-this-turn = true` — prevents the factory-rush
exploit where Talon builds an air unit and immediately gets two
actions out of it.

### 17.5 Constants as Records (locked decision 32-A2)

Declared at engine init via `record-type names -- proc`. Each constant
table is a global-VM Dict whose VALUES are Records:

| Record | Fields |
| --- | --- |
| `unit-stat` | `[/cost /hp /move /fuel /ammo /vision /class /direct-or-indirect /indirect-min /indirect-max ...]` |
| `co-spec` | `[/callsign /color /bio /passives /power-name /power-cost /power /super-name /super-cost /super /charge-rate]` |
| `terrain-spec` | `[/move-cost-foot /move-cost-wheel /move-cost-tread /move-cost-air /move-cost-sea /defense-stars /defense-stars-inf /fog-hide-ground /fog-hide-naval /render-fg /render-bg /render-char /scenario-char]` |

Scenario files author with Dict literals; `build-initial-game-state`
converts to Records via `record-from-dict` during load.

The damage table stays as Dict-of-Dicts (sparse 17×17).

### 17.6 Mutation discipline (contract)

Any state mutation that moves, spawns, or kills a unit MUST
atomically update **all three** indexes:

1. The unit's own `/q` / `/r` (and other affected fields) — see §17.4.
2. The cell-grid `/unit-id` slot at the old position (cleared to the
   "none" sentinel) and at the new position (set to the unit's id) — see §17.3.
3. The `/faction-units` index (append on spawn; remove on kill;
   unchanged on move) — see TDD §3.2.

A single chokepoint op owns each of these multi-write transactions:
`unit-spawn` / `unit-move` / `unit-kill` (TDD §3.2, `unit.trx`).  No
gameplay caller touches the indexes directly.

`validate-state` runs at scenario load, at load-game time (after
state-slot binding, §15.3), and optionally at end-of-turn under
`--self-test`; it confirms that the cell `/unit-id` grid, `/units`
Dict keyset, and `/faction-units` arrays are in lockstep.

Building mutations (capture progress tick, ownership flip on
capture, owner-clear on demolition v2) follow the same discipline
against the `/owner` / `/capture-progress` / `/capturer-id` cell
grids plus the `/faction-buildings` index (TDD §3.4).

---

## 18. UI, rendering, and controls

### 18.1 Screen layout

**Layout rule (locked 2026-05-16):** the minimap section is fixed
at **36 cols wide** regardless of terminal width.  The side-panel +
combat-log left column absorbs all extra width as the terminal
grows.  Formally: `c-mid = w - 36`, so at 120×40 minimum the left
column gets 84 cols and the minimap gets 36; at 200×60 the left
column gets 164 cols and the minimap stays at 36.



```
   Day 7 · Turn 4 · Bishop's turn (funds 12500 · power 3.2)             <- top bar (floating)
+--------------------------------------------------------------------+
|                                                                    |
|         _____         _____         _____                          |
|        / cHQ \       / ... \       / ... \                         |
|  _____/      \_____/  T h  \_____/   .   \_____                    |
| /     \=8/9 //     \  ...  /     \  ...  /     \                   |
|/  ...  \____/ -1,-1 \_____/  1,-1 \_____/  ...  \                  |    <- main
|\       /     \       /     \       /     \       /                 |       viewport
| \_____/ -2,0  \_____/  0,0  \_____/  2,0  \_____/                  |
|                                                                    |
+----------------------------------+--minimap-----------------------+
| Selected: Tank (HP 10/10, ...)   | . . . ^ ^ ^ . . . .            | <- side panel
| Adjacent enemies: 2 Inf (E, NE)  | . ^ * ^ . . . . . .            |    (left col)
+----------------------------------+ . . . . . T . . . .            | <- minimap
| Combat: Bishop's Tank hit Hammer | . . . . . . . . . .            |    (right col,
| for 7 HP (counter 1 HP)          | . . . . . . . . . .            |     full height)
+----------------------------------+--------------------------------+
   WASD/HJKL move · Enter select · Space menu · Shift-E end · ? help  <- keybind (floating)
```

### 18.2 Hex render strategy (locked decisions 4 + 35)

Each hex is **5 lines tall × 9 chars wide**, flat-top with odd-q
brick-offset interlocking. Adjacent columns share diagonals (one column's
`\` is the next column's `/`); adjacent rows in the same column share
the top/bottom edge. The middle two lines (3 and 4) are at the
**widest** position (7-char content area); lines 2 and 4-edges step
inward by one column on each side.

The **canonical rendered shape** is Appendix A's source of truth.
The template below is illustrative; if it conflicts with Appendix A,
Appendix A wins.

**Per-hex render template:**

```
  _____           line 1: top edge (5 underscores, cols 2-6; shared with row above)
 /     \          line 2: top diagonals (col 1, col 7) + 5-char content (cols 2-6)
/       \         line 3: widest line — diagonals (col 0, col 8) + 7-char content (cols 1-7)
\       /         line 4: widest line — diagonals (col 0, col 8) + 7-char content (cols 1-7)
 \_____/          line 5: bottom edge (5 underscores, cols 2-6; shared with row below)
```

**Per-hex content canvas**:

| Line | Width   | Content                                                                                  |
| ---- | ------- | ---------------------------------------------------------------------------------------- |
| 2    | 5 chars | Top half — terrain icon + building marker; faction-color border tint                     |
| 3    | 7 chars | Middle (widest) — unit glyph centered; carried-unit marker if applicable                 |
| 4    | 7 chars | Bottom half (widest) — HP bar (`=N/M`) + fuel/ammo indicator + vet-tier pip (`★`, right) |

The vet-tier pip (§7.8) is rendered in the bottom-right of line 4
(cols 5-7), with 1, 2, or 3 `★` glyphs in bright-yellow corresponding
to `/vet-tier`. Vet-0 units show no pip. Line 4's 7-char budget
fits `=N/M` (5 chars) + 1 space + 1-pip comfortably; with 3 pips it
overrides the leftmost fuel/ammo digits — vet is
high-signal-low-frequency vs fuel which is queryable via the side
panel (§18.1).

**Sample multi-hex grid** (illustrates the interlocking; canonical
empty hexes, no content):

```
         _____
        /     \
  _____/       \_____
 /     \       /     \
/       \_____/       \
\       /     \       /
 \_____/       \_____/
 /     \       /     \
/       \_____/       \
\       /     \       /
 \_____/       \_____/
       \       /
        \_____/
```

Even-q columns sit at "high" vertical position; odd-q columns sit 2
lines lower (the brick offset). Vertical step between same-column
hexes is 4 screen lines (line 5 of upper hex shares its screen row
with line 1 of the next hex below). Adjacent columns share one
diagonal char per line — the `\` of column N's line 3 sits at the
same screen position as the `/` of column N+1's line 3.

### 18.3 Viewport math

| Map size | Whole-map screen size (chars × lines) |
| -------- | ------------------------------------- |
| Tutorial | 177 × 73                              |
| Standard | 352 × 145                             |
| Epic     | 527 × 217                             |

(Whole-map width = `q × 7 + 9` for the rightmost column's bbox;
height = `r × 4 + 5` for the bottom row's bbox.  Per §18.4.)

No terminal shows whole maps; scrolling viewport (locked decision 5) is
non-optional.

**Minimum supported terminal: 120 × 40** (locked decision 39, added
2026-05-15 Phase 0.6C; layout refined 2026-05-16 — see below).  Below
that, the §18.1 layout cannot fit the floating top-bar + viewport +
side-panel/combat-log/minimap section + floating keybind without
overflow.  Visible hexes inside the viewport panel (after the layout
eats 10 rows and 4 cols of overhead):

| Terminal | Viewport interior | Visible hex columns × rows | Total |
| -------- | ----------------- | -------------------------- | ----- |
| 120 × 40 | 116 × 30          | 16 × 7                     | 112   |
| 160 × 50 | 156 × 40          | 22 × 9                     | 198   |
| 200 × 60 | 196 × 50          | 27 × 12                    | 324   |

At 120×40 (minimum), tactical theater: about a third of a Standard
map.  At 200×60 (large), tutorial map fits in one view; Standard
map needs ~6 viewport shifts.

### 18.4 Per-row screen mapping

```
hex-to-screen: pos -- [screen_col screen_row]   (top-left corner of hex)

  hex_width  = 9       % bounding box width per hex column
  hex_height = 4       % vertical step between same-column hexes (5-line hex - 1 shared)

  screen_col = q × 7                                   % adjacent columns share 2-char diagonal overlap
  screen_row = r × 4 + (q is odd ? 2 : 0)              % brick-stagger by column parity
```

### 18.5 Minimap (NEW UI element)

A 1-char-per-hex condensed view of a **slice of the map centered on
the cursor**, living in the tall right column of the §18.1 lower
section (spanning the side-panel and combat-log rows). Shows:

- Terrain colors (per the terrain table render `bg` column).
- Faction-owned hexes (faction-color tint on cities/HQs).
- Cursor position (always at the geometric center of the slice unless
  edge-clamped — see below; rendered inverse `bg=15 fg=0`).
- Current viewport rectangle (outlined so player sees where they are in
  the whole map).

**Slice dimensions:** the slice is sized to the right-column content
area.  Per §18.1's fixed-width minimap rule, the slice is **always
~32 cells wide × 5 cells tall** regardless of terminal width (right
column locked at 36 cols total; content area = 36 minus borders and
2-cell padding = ~32 wide).  For the §A.8 canonical 76-wide render
the slice illustration is narrower (19×5) to fit the small example;
production renders at 120+ always show the full 32×5 slice.

**Centering and edge clamping:** the slice's center hex always
matches the cursor's `[q r]` position UNLESS doing so would expose
out-of-bounds map cells. When the cursor is within `slice_w/2` of the
left/right edge (or `slice_h/2` of the top/bottom edge), the slice
clamps to the edge — i.e., the cursor moves off-center toward that
edge while the slice stops scrolling, so every rendered cell stays
in-bounds and the cursor remains visible in the slice. Formally:

```
slice_origin_q = clamp(cursor_q - slice_w/2, 0, map_w - slice_w)
slice_origin_r = clamp(cursor_r - slice_h/2, 0, map_h - slice_h)
```

This matches how the main viewport pans (§18.8 cursor-near-edge
behavior) — the minimap is a zoomed-out preview of the same
camera-following idea, not a fixed whole-map view.

Reuses the 1-char-per-hex render math. Math layer (coord conversion,
adjacency, distance) shared between main viewport and minimap; only
the per-hex render proc differs.

### 18.6 Damage-preview overlay

When the player has selected an attacker and the cursor is over a
valid target hex (in range, legal damage-table entry), the side panel
(§18.1) replaces "Adjacent enemies" with a **damage preview** block:

```
  Target: Hammer's Tank (HP 7/10, plain)
  Predicted damage: 4 HP  (counter: 1 HP at 3 HP post-damage)
```

The preview reuses the §7 combat resolver in a side-effect-free mode:
no damage applied, no RNG state advanced.

| Mode              | Preview shown                                                     |
| ----------------- | ----------------------------------------------------------------- |
| `/luck-mode /off` | Exact integer damage + exact counter (deterministic).             |
| `/luck-mode /on`  | Damage range `min-max` from `luck_pp ∈ [0, 9]`; counter likewise. |

The preview refreshes on cursor move within target-selection mode.
Damage prediction is one of the two single most-missed features when
absent (the other is undo-before-commit, deferred to v2 — §21).

The same predicate (`predict-damage attacker defender state -- min max`)
is the building block AI v2 (§14.3) uses for `damage-potential` in
scoring; defining it once for both is the natural Trix factoring.

### 18.7 Controls

Player-facing v1 keybinds.  Per-mode dispatch detail (cursor /
unit-move / unit-attack / menu-action / menu-build / menu-co-power
/ menu-system / animating) lives in **TDD §7.5**.

**Cursor + view:**

| Key                 | Action                                         |
| ------------------- | ---------------------------------------------- |
| W / H / up          | Move cursor up (north)                         |
| S / L / down        | Move cursor down (south)                       |
| A / J / left        | Move cursor left (2-hex strafe — see TDD §7.6) |
| D / K / right       | Move cursor right (2-hex strafe)               |
| Y or Q              | Move cursor NW (hex diagonal)                  |
| U or E              | Move cursor NE (hex diagonal)                  |
| B or Z              | Move cursor SW (hex diagonal)                  |
| N or C              | Move cursor SE (hex diagonal)                  |
| Page-Up / Page-Down | Pan camera independent of cursor               |
| Ctrl+Arrow          | Free-scroll camera (cursor stays put)          |
| Home                | Re-center camera on cursor                     |

**Selection + action:**

| Key   | Action                                                                                  |
| ----- | --------------------------------------------------------------------------------------- |
| Enter | Select unit / confirm destination / confirm action                                      |
| Space | Open action menu at cursor (move / attack / capture / wait / build)                     |
| Tab   | Cycle to next own unit (in cursor mode) / next valid destination (in move/attack modes) |
| Esc   | Cancel current selection or open system menu (save/load/quit/help)                      |

**Direct shortcuts** (mnemonic; equivalent to the menu path):

| Key     | Action                                                                                 |
| ------- | -------------------------------------------------------------------------------------- |
| Shift-B | Build (cursor on owned Factory/Port/HQ with funds)                                     |
| P       | Open CO Power menu (offers regular Power if meter ≥ power-cost, Super if ≥ super-cost) |
| Shift-E | End turn                                                                               |

`Shift-B` is a shortcut; the canonical entry is Space → action menu
→ Build.  The action menu only offers Build when the cursor is on an
owned Factory/Port/HQ AND the faction has funds for at least one
unit; otherwise Build is omitted from the menu (avoid dead options).

**Save / load / help / quit:**

| Key       | Action                                        |
| --------- | --------------------------------------------- |
| F1 or `?` | Show in-game help overlay                     |
| F5        | Quick save → `saves/quicksave.bws`            |
| F9        | Quick load (re-exec via `--load=`; see §15.3) |
| Shift-Q   | Quit (confirmation prompt)                    |

The floating keybind row (§18.1) shows a compact subset of the most
common actions: `WASD/HJKL move · Enter select · Space menu · E end
turn · ?`.  Full help is one keypress away via `F1` or `?`.

Controls finalize in Phase 12-13 (Group 5). The list above is the v1
commit target.  Mouse / pointing is **out of scope** for v1 (TDD
§7.7; GDD §21).  Cheats (e.g., reveal map) require `--cheats` CLI
flag and add Ctrl-prefixed bindings; see TDD §13.6.

### 18.8 Viewport scrolling

Locked decision 5. The viewport pans when the cursor approaches the
viewport edge. Cursor stays centered when possible; map edges block
scrolling.

### 18.9 Color theme

Six faction colors, one per CO in the v1 roster (§8.1):

| Faction | Color  | ANSI bright fg | ANSI dim fg |
| ------- | ------ | -------------- | ----------- |
| Bishop  | white  | 15             | 7           |
| Hammer  | red    | 9              | 1           |
| Hawk    | yellow | 11             | 3           |
| Boots   | green  | 10             | 2           |
| Veil    | purple | 13             | 5           |
| Talon   | cyan   | 14             | 6           |

Neutral buildings render gray (`fg=7` on `bg=8`).

The 16-color ANSI palette is the locked render medium (locked
decision 3). Scenarios CANNOT introduce additional faction colors
in v1; if two factions need to share a color (rare in v1 because
roster is exactly 6 COs), scenarios use distinct CO callsigns to
disambiguate.

Terrain colors per terrain table (§4) and Appendix A.0 + A.2.

### 18.10 Phase 0 readability gate

The 5-line × 9-char-wide flat-top hex render (§18.2, Appendix A) must
be validated BEFORE Phase 0 commits to main. Validation matrix:

| Dimension          | Values                                             |
| ------------------ | -------------------------------------------------- |
| Terminal size      | 120×40 (minimum), 160×50 (typical), 200×60 (large) |
| Map size           | 25×18 (tutorial), 50×36 (standard), 75×54 (epic)   |
| Terrain mix        | mostly-plain, dense-mountains-forests, naval-heavy |
| Color-mix scenario | 6 faction-color combinations                       |

Total: 162 combinations.

**Acceptance criteria (must pass ALL):**

- Unit glyph centered in hex distinguishable from terrain background
  and from other unit glyphs.
- Hex-grid topology reads as a hex grid (interlocking flat-top hexagons),
  not as misaligned chars.
- All 9 terrain types identifiable from their render combination
  (background color + center icon).
- 6 faction colors + white + neutral-gray distinguishable in the hex
  border tint.

**Pass path**: commit Phase 0 as drafted; render strategy locks.

**Fail path**: iterate on the renderer until acceptance criteria pass.
The 5-line × 9-char-wide hex is **the** renderer — there is no smaller
fallback shape. (Locked decision 35, updated 2026-05-15: previously
allowed a 3-line × 7-char compact fallback; dropped on the grounds
that two divergent renderers would split self-test infrastructure
and Appendix A would need to enumerate both forms. Single source of
truth is cheaper to maintain than two.)

Gate runs in Phase 0 and iteratively in Phase 12 (Group 5) commits
until acceptance is reached. Subsequent phases inherit the
acceptance-locked render; no re-evaluation.

---

## 19. Self-test invariants

Run via `./trix.opt examples/bestagon/main.trx --tests` (one-shot,
exit-code-driven; CI gate) or via `--invariants=LIST` for per-turn
checks during interactive play.  Harness contract: TDD §11.
Cumulative across phases.  Every commit extends this set.

### Phase 0 invariants
- [ ] Hex coord round-trip: `(q,r) -> cube -> (q,r)` identity for all 25×18 cells
- [ ] Adjacency function returns 6 neighbors for interior hex, ≤ 5 for edge hex
- [ ] `pos->idx` and `idx->pos` are inverses
- [ ] Phase 0 readability gate passes (iterate-until-pass per §18.10; no fallback renderer)
- [ ] Minimap renders within its 36-col-wide right-column allocation (§18.1) as a cursor-centered slice of the configured slice dimensions (§18.5, ~32×5 at 120×40 minimum)

### Phase 1 invariants
- [ ] Procgen continent: every land cell reachable from every other land cell after flood-fill (validated by Phase 0.7 proto)
- [ ] Seed-determinism: same seed → byte-identical map (validated by Phase 0.7 proto)
- [ ] Procgen 2-faction buildings: exactly 2 HQs, one per faction, with hex-distance ≥ map-diagonal/3 between them (validated by Phase 0.7b proto)
- [ ] Procgen building legality: HQ/City/Factory only on land terrain; Port only on Beach (§16.6 + Phase 0.7b)
- [ ] Procgen symmetric maps (`/symmetry /mirror-h` or `/rotational-180`): building parity across axis -- mirroring left half yields identical right half (Phase 0.7b)
- [ ] `/procgen-spec` scenario round-trip: save + reload yields byte-identical state (§16.8)

### Phase 2 invariants
- [ ] tutorial.bw loads without `/scenario-error`
- [ ] Every unit and building from tutorial.bw instantiates with correct owner/HP/pos

### Phase 3 invariants
- [ ] Unit roster (§6): every entry's cost, HP, move, fuel, ammo, vision matches the §6 tables byte-for-byte
- [ ] Damage table (§7.4 + §7.5): each non-`—` entry in the 17×17 ASCII table has a matching key in the dict literal
- [ ] Anti-Air vs Battle Copter = 120 (intentional merger consequence per §7.4)
- [ ] Mech bazooka exhaustion falls through to 30% of bazooka-row damage (§6 + §12.4)

### Phase 4 invariants
- [ ] Reachable-set: every unit type's move-set on every terrain type matches expected count
- [ ] Occupant array stays in sync with units array after move (validate-state passes)
- [ ] BCop carrying: cargo on board reduces effective move from 6 to 3 (§12.7)
- [ ] APC resupply: at start-of-turn, adjacent own units have fuel + ammo restocked to max (§12.6)

### Phase 5 invariants
- [ ] ZOC: moving Recon into adjacent enemy halts after 1 hex (§7.9)
- [ ] ZOC: air units ignore ZOC (Fighter flies through adjacency without halting)
- [ ] ZOC: submerged sub does NOT project ZOC
- [ ] Move execution: per-turn fuel tick (NOT per-hex consumption per §12.4 locked model)

### Phase 6 invariants
- [ ] Damage symmetry: Tank vs Tank at full HP, zero-passive CO, on Plain produces identical damage either direction
- [ ] Counter-attack uses defender's POST-damage HP
- [ ] Indirect-fire: no counter-attack
- [ ] Floor rounding applied; HP bar of result floors to 0 (not below)

### Phase 7 invariants
- [ ] Artillery range exactly 2-3 hex (no CO bonus)
- [ ] Rocket range exactly 3-5 hex (no CO bonus)
- [ ] Battleship range exactly 2-6 hex (no CO bonus)
- [ ] Hawk indirect-range cap: Battleship max effective range = 2-8 (passive +1 + Power-OR-Super +1; per §8.4 cap)
- [ ] Indirect units can't fire after moving (locked decision 8)

### Phase 8 invariants
- [ ] Combined move-then-attack action: composite action sets both `/has-moved` and `/has-fired` atomically
- [ ] Combined move-then-capture: composite action sets `/has-moved` and `/has-captured`; advances capture-progress on building
- [ ] Action atomicity: if target destroyed mid-action, action rejected with no state change (§2 action-atomicity rule)
- [ ] Load action: Inf moving onto own APC consumes the move + adds Inf id to `/transport-cargo`; APC's `/pos` unchanged

### Phase 9 invariants
- [ ] Capture: 10-HP infantry captures HQ in exactly 2 turns
- [ ] Capture: 5-HP infantry takes 4 turns (default capture-speed); shorter with Boots passive
- [ ] Veterancy: kill increments `/kills`; tier advances at 3 / 5 / 8 thresholds
- [ ] Veterancy: on a Tank-vs-Tank matchup with zero-passive CO (custom fixture, not standard Bishop), vet-3 attacker damage = vet-0 attacker damage × 1.15 (rounded floor)
- [ ] Veterancy: capture-kill (HQ-flip cleanup) does NOT increment any unit's `/kills`
- [ ] Veterancy: self-destruction (0 fuel) does NOT credit the most-recent attacker
- [ ] Veterancy survives save/load: a vet-2 unit reloaded has /vet-tier = 2 + /kills ≥ 5

### Phase 10 invariants
- [ ] Fog: enemy ground unit in forest invisible from non-adjacent hex; visible from adjacent
- [ ] Sub: visible only to adjacent Destroyer when submerged
- [ ] Weather: scenario with `/weather-cycle [/clear /rain]` flips current weather at EOT
- [ ] Weather: Rain `/all-vision -1` reduces effective vision of every unit by 1 hex
- [ ] Weather: Snow `/tread-move -1` reduces Tank/HTank effective move by 1 hex
- [ ] Weather: scenario without `/weather-cycle` has `/weather-current` absent from game-state
- [ ] Weather rotation: 4-entry cycle returns to entry 0 at the start of **day 5** (per §11.5 cycle-advances-once-per-game-day rule). For 2-faction scenarios this is turn 9 (1-indexed turn-number); for N-faction scenarios it's turn `(4 × N) + 1`

### Phase 11 invariants
- [ ] Economy: 5 cities + HQ = 6000/turn income
- [ ] Economy: `/cumulative-funds-earned` increments by income on each turn-start (used by `/turn-limit-funds` win-cond)
- [ ] Production: spending funds creates unit; remaining funds correct; new unit gets `/built-this-turn = true`
- [ ] Healing: stationed Inf at 5 HP on owned City → 7 HP after start-of-turn
- [ ] Healing: adjacent-but-not-stationed Inf at 5 HP next to owned City → still 5 HP (AW-canon stationed-only)
- [ ] Healing: ammo + fuel restocked alongside HP at stationed buildings
- [ ] Carrier heal: adjacent BCop heals + refuels (BCop included in Carrier scope, §12.5)
- [ ] Fuel: full-fuel Bomber decrements 2 per turn; full-fuel Destroyer decrements 1 per turn; submerged sub decrements 5 per turn

### Phase 12 invariants (render gallery)
- [ ] `render-hex-gallery-test`: every §A.2 terrain exemplar renders byte-for-byte per Appendix A
- [ ] `render-unit-faction-test`: every (17 units × 6 factions × 3 owner-states) combination renders per §A.4.4 matrix
- [ ] `render-grid-gallery-test`: §A.7.4 mixed scene renders byte-for-byte
- [ ] `render-screen-gallery-test`: §A.8.2 full screen renders 76 chars × 30 lines byte-for-byte
- [ ] Anti-Air ASCII fallback glyph is `A` (not `^`)
- [ ] Vet-3 + wounded render: HP bar at cols 1-5 + pip cluster at cols 6-7 (loses 3rd pip per §A.5.4)

### Phase 13 invariants
- [ ] Turn structure: end-of-turn advances current-faction correctly
- [ ] Hotseat: 2 humans alternate turns
- [ ] Hotseat pass-the-keyboard screen appears between two consecutive `/human` faction turns (§2)
- [ ] Action-atomicity test: indirect attack with mid-turn-destroyed target produces no fuel/ammo consumption and no `/has-moved` set

### Phase 14 invariants
- [ ] Save/load round-trip: full game state byte-identical after save+load

### Phase 15 invariants
- [ ] AI v1: 100-turn skirmish never soft-locks
- [ ] AI v1: captures at least 1 city in a standard match
- [ ] AI v1: 0-damage-attack guard — fixture `tests/test_ai_v1_zero_damage_guard.bw` places HP-1 Hammer Inf adjacent to Bishop Tank; assert AI v1 picks `/advance` or `/defend` (per §13.1 + §14.3 action-classification guard), NOT the suicidal 0-damage chip attack. Sim verification 2026-05-16 showed the guard is correct but rarely triggered in organic play, so an explicit fixture is required.
- [ ] AI v1: build-phase ordering — fixture asserts that on turn N+1, after the HQ-resident unit vacates during action phase, the build phase places a new unit at HQ in the same turn (per §13.2 phase-order lock).
- [ ] AI v1: `/defend-building` clause (b) — fixture `tests/test_defend_building_clause_b.bw` places HP-1 Hammer Inf on Hammer-owned city with Bishop Tank adjacent; assert AI v1 picks `/defend-building` (value 4) rather than `/advance` off the city (value 2) or `/defend` (value 1). Clause (b) fires because the Inf's would-be chip attack has predict-damage 0 (Inf-vs-Tank at HP-1 floors to 0%), reclassifies to `/advance` under the 0-damage guard, and the city's defensive bonus + counter-cost 0 makes `/defend-building` the winning score. 3rd-sim verification 2026-05-16 showed organic play rarely produces an HP-1 Inf with a Boots-turn to decide — explicit fixture needed.

### Phase 16 invariants
- [ ] Each CO's passive applies correctly (damage modifier observed in combat)
- [ ] Power meter charges at the predicted rate
- [ ] Power activation applies one-turn buff; resets at end-of-turn
- [ ] Talon (6th CO) loads and renders with cyan color tint
- [ ] Talon's `/air-extra-action` Power gives every air unit a second `/has-moved` reset
- [ ] Super Power activation: when stars ≥ super-cost, action-loop UI offers Power / Super / Hold
- [ ] Super activation resets meter to 0 (same as regular Power)
- [ ] Super-cost = power-cost + 4 holds for all 6 COs
- [ ] Meter cap is 12 (bumped from 10); accumulating to 13 caps at 12
- [ ] Talon's Lightning Drive Super: `/air-extra-action` + `/air-restock` resets every own AIR unit + restocks all air-unit ammo/fuel (per 2026-05-16 air-only scoping; ground units NOT affected)
- [ ] Picking Power closes Super for the turn (replacement, not augmentation)
- [ ] Boots' Forced March kicker — fixture `tests/test_forced_march_kicker.bw` pre-positions a full-HP Boots Inf 4 hexes from an enemy-owned city, Boots meter at 4.0★; assert Forced March activates SOT, the Inf moves 4 hexes onto the city (foot-mv 4+3=7 budget reached), and the fresh capture completes in 1 turn (progress = `floor(10 × passive 1.5 × kicker 1.5) = floor(10 × 2.0) = 20`, flips this turn). 3rd-sim verification 2026-05-16 showed organic play geometry-gates the kicker (Boots foot rarely reaches an enemy building during the FM activation turn on a 25×18 map with 12-15-row separation) — explicit fixture needed.

### Phase 17 invariants
- [ ] AI v2 tactical eval picks +HP-trade move over −HP-trade move when both are scoreable
- [ ] AI v2 turn at 75×54 with 80 units completes within hard cap for both tiers
- [ ] AI v2 prefilter mode triggers when soft cap crossed
- [ ] `/ai-perf-tier` setting honored at game start and after save/load
- [ ] AI v2 counter-cost term: HP-2 Inf-vs-Tank attack scores below `/advance` (predict-damage 1 × 50 = 50 vs counter-cost 7 × 50 = 350; `/attack-chip` × 100 + 50 − 350 = −200 vs `/advance` × 100 = 200). Fixture `tests/test_ai_v2_counter_cost.bw` asserts the AI picks advance, not chip.
- [ ] AI v2 weights match §14.3 locked values; `/ai-action-value` dict equals the locked table key-for-key
- [ ] AI v2 fixture scenario `fixture-ai-v2-known-good.bw`: best-move output is the documented expected move, byte-for-byte
- [ ] AI v2 prefers capture-hq over clean-kill when both are legal (1500 > 1200 weight test)
- [ ] Fog perf: at 75×54 with 80 units, `compute-fog` invocations per AI v2 turn ≤ 200 (memoize hint: cache per faction per turn)

### Phase 18 invariants (AI v3, optional)
- [ ] AI v3 MCTS converges to expected best move within 500 rollouts on `fixture-ai-v3-known-good.bw` (scripted 2-faction position with forced optimal move)
- [ ] AI v3 worker pool: 4 coroutine workers complete a 100-rollout batch within hard-cap budget
- [ ] AI v3 strategic layer: active-goal flips from `/economy-grow` to `/win-by-capture-hq` when enemy HQ becomes reachable within 2 turns
- [ ] AI v3 hierarchical ordering: tactical layer never executes a move that violates an active operational sub-goal's `/shaping` weights
- [ ] AI v3 vs AI v2 self-play: AI v3 (/hard) wins ≥ 70% of 25×18 standard-CO matches (statistical, ±5%)
- [ ] AI v3 (/easy) without MCTS matches AI v2 wall-clock at /standard tier (no regression)
- [ ] Tree global-VM persistence: AI v3 mid-search snapshot/thaw resumes search without restart

---

## 20. Scenario-overridable surface

Every constant table is overridable per-scenario.

| Override name | Affects |
| --- | --- |
| `/damage-overrides` | Per-cell `damage-table[atk][def]` values |
| `/unit-cost-overrides` | Per-unit `unit-costs[unit]` |
| `/co-meter-overrides` | `/threshold-funds-per-star`, `/received-weight`, `/cap-stars`, `/reset-on-power-activation` |
| `/co-roster-extras` | Additional COs beyond the 6-roster |
| `/luck-mode` | `/off` or `/on` |
| `/rng-seed` | Seeds RNG when `/luck-mode /on` |
| `/ai-perf-tier` | `/standard` or `/patient` (default tier for this scenario) |
| `/weather-cycle` | Per-turn weather rotation array (see §11.5) |
| `/weather-table-extras` | Custom weather types beyond the 4 built-ins |

Scenario declares any subset; loader shallow-merges onto canonical
defaults at game-state-build time.

---

## 21. Out of scope

Out for v1; not even a stretch goal:

- 3D / isometric perspective.
- Sprite graphics / sub-cell detail.
- Real-time / pause-able mode (turn-based only).
- Network multiplayer (hotseat only).
- Audio / music (terminal program).
- Map editor GUI (CLI map editing only, if any).
- Custom DSL for COs / units (data is Dict literals).
- Mod loader (scenarios are full Trix-source).
- Stealth aircraft, Neotank, Pipe Runner, Black Boat.
- Lab / Tower / Silo buildings.
- AI v2 minimax depth-search / coroutine multithreading / ML eval
  (subsumed by AI v3 §14.10, optional M9).
- Persistent fog reveal (locked decision 15: AW model only).
- Per-unit Groove abilities (Wargroove-style; see §1.2 v2 candidates).
- Commanders-on-field (Wargroove-style; v2 candidate).
- Focus Fire (Tiny Metal-style multi-attacker stacking; v2 candidate).
- Wesnoth-style time-of-day unit alignment (v1 ships simple weather
  cycle per §11.5; deeper alignment system is v2).

---

## 22. Phase plan

10 implementation Groups, 9 milestones, 14-17 commits total.
M9 (AI v3) is an optional milestone — v1 is fully playable on M8.

| Milestone | Commits | LOC | What's demoable | Naming |
| --- | --- | --- | --- | --- |
| M1 | Groups 1+2 (1-2 commits) | 1400 | Load tutorial.bw, render map + minimap, move infantry — no combat | `bestagon: foundations + units + movement` |
| M2 | Group 3 (1-2 commits) | 600 | Combat works: direct, counter, indirect; combat log shows damage | `bestagon: combat resolution (direct + counter + indirect)` |
| M3 | Groups 4+5 (2 commits) | 1300 | Capture + fog + economy + UI panels + hotseat — game loop complete | `bestagon: game loop (capture, fog, economy, UI)` |
| M4 | Group 6 (1 commit) | 300 | Save/load works; scenario library has 3-5 maps | `bestagon: persistence + scenario library` |
| M5 | Group 7 (1 commit) | 400 | AI v1 plays a game vs human — playable solo | `bestagon: AI v1 (greedy)` |
| M6 | Group 8 (1 commit) | 450 | 5 COs work with full passives + Powers + meters | `bestagon: COs + Power meter` |
| M7 | Group 9 (1 commit) | 500 | AI v2 logic-programmed eval (player-selectable tier) — the headline showcase | `bestagon: AI v2 (logic-programmed tactical eval)` |
| M8 | Group 10 (1 commit) | 300 | Mini-map polish, replay log, README, gallery | `bestagon: polish + docs` |
| M9 | Group 11 (5 commits) | 1100 | AI v3 hierarchical + MCTS + coroutine-parallel rollouts — optional hard mode | `bestagon: AI v3 (MCTS + hierarchical, optional)` |

### M9 commit breakdown

| Commit | Scope                                                                         | LOC est. |
| ------ | ----------------------------------------------------------------------------- | -------- |
| 9A     | Strategic + operational layers; no MCTS; integrates with §14.3 scoring        | 250      |
| 9B     | Tactical MCTS with single-coroutine rollout (no parallelism)                  | 400      |
| 9C     | Coroutine worker pool + parallel rollouts via mailbox + `find-n` barrier      | 200      |
| 9D     | Difficulty-tier scaffolding + scenario opt-in + `fixture-ai-v3-known-good.bw` | 100      |
| 9E     | Self-test invariants (Phase 18) + AI v3 vs AI v2 self-play scripted match     | 150      |

### Commit gates

Every commit must:

- Add at least one `--self-test` invariant. Cumulative count grows
  monotonically.
- Render cleanly via `./trix.opt examples/bestagon.trx --self-test`. Exit 0.
- Exercise its documented Trix-showcase angle.

### Per-phase Trix-showcase angles

| Phase | Trix idiom showcased |
| --- | --- |
| 0-1 | Hex coords + cellular automata (flat-top + odd-q fresh math) |
| 2 | Scenario as Trix-source: dict + arrays + Tagged terrain enums |
| 3 | Unit roster as dict-of-records (heist.trx muscle) |
| 4 | A* via priority queue; reachable-set as `find-all` |
| 6 | Damage table as nested dict literal |
| 9 | Capture state as record with `def-persist` HP slot |
| 10 | Fog reveal via set ops: `visible-now = union of vision-sets` |
| 12 | screen ops: blit panels, multi-line hex render, minimap |
| 14 | `to-binary-token` round-trip of the whole game-state Dict — the headline persistence showcase (custom `.bws` format, not VM snapshot) |
| 15 | Greedy AI: per-unit scoring loop |
| 16 | COs as data: dict literal per CO + Power meter as record field |
| 17 | **Logic-programmed AI**: find-all + once + aggregate — the headline AI showcase |
| 18 | **MCTS + hierarchical + coroutine actor pool**: 7-subsystem integration showcase (optional) |

---

## Appendix A — Hex tile render gallery (canonical source of truth)

This appendix is the **rendered source of truth** for §18. Every
exemplar is byte-for-byte canonical: Phase 12 commits a self-test
(`render-hex-gallery-test`) that asserts the renderer produces these
exact characters for the named state. Color is annotated by ANSI
index in a per-section key table; the ASCII shapes are monochrome
because the markdown medium can't carry ANSI escapes.

**If §18 prose ever disagrees with this appendix, this appendix wins.**

### A.0 ANSI color reference

Bestagon uses the standard 16-color ANSI palette. `fg` = foreground
(text), `bg` = background. SGR codes per the standard.

| Idx | Name           | SGR fg / bg | Bestagon role(s)                                     |
| --- | -------------- | ----------- | ---------------------------------------------------- |
| 0   | Black          | 30 / 40     | (unused as primary; cursor underlay only)            |
| 1   | Red            | 31 / 41     | Hammer (enemy / dim variant)                         |
| 2   | Green          | 32 / 42     | Plain terrain base; Forest base; Boots (enemy/dim)   |
| 3   | Yellow         | 33 / 43     | Road glyph; Bridge accent; tan base; Hawk (e/dim)    |
| 4   | Blue           | 34 / 44     | Sea / River base (no faction in v1 roster uses blue) |
| 5   | Magenta        | 35 / 45     | Veil (enemy/dim)                                     |
| 6   | Cyan           | 36 / 46     | Reef; River accent; Talon (enemy/dim)                |
| 7   | White          | 37 / 47     | gray (neutral building text; dim Bishop)             |
| 8   | Bright Black   | 90 / 100    | dark gray (Mountain bg; neutral building bg)         |
| 9   | Bright Red     | 91 / 101    | Hammer (own / bright variant)                        |
| 10  | Bright Green   | 92 / 102    | Forest icon `*`; Boots (own / bright)                |
| 11  | Bright Yellow  | 93 / 103    | Hawk (own / bright); **vet pip `★`**                 |
| 12  | Bright Blue    | 94 / 104    | reserved faction (own / bright)                      |
| 13  | Bright Magenta | 95 / 105    | Veil (own / bright); "purple"                        |
| 14  | Bright Cyan    | 96 / 106    | Talon (own / bright)                                 |
| 15  | Bright White   | 97 / 107    | Bishop (own); selected-cursor highlight              |

Notation throughout: a **color-key mini-table** after each exemplar
lists which colors paint which positions, of the form `bg=2 lines
2-4; fg=10 line-3 glyph; border fg=10`. Positions reference the
line-and-column scheme defined in A.1.

### A.1 Anatomy of a hex (canonical shape)

The canonical glyph base. Empty hex on Plain terrain — no building,
no unit, no overlays.

```
  _____           line 1 — top edge (5 underscores; shared with row above)
 /     \          line 2 — top diagonals (cols 1, 7) + 5-char content (cols 2-6)
/       \         line 3 — widest line; diagonals (cols 0, 8) + 7-char content (cols 1-7)
\       /         line 4 — widest line; diagonals (cols 0, 8) + 7-char content (cols 1-7)
 \_____/          line 5 — bottom edge (5 underscores; shared with row below)
```

**Char-position addressing** (used by `render-hex-gallery-test`):

| Line | Char positions (0-indexed within the 9-wide × 5-tall bounding box)           |
| ---- | ---------------------------------------------------------------------------- |
| 1    | cols 2-6 = 5 underscores (`_____`)                                           |
| 2    | col 1 = `/`; cols 2-6 = top-half content area (5 chars); col 7 = `\`         |
| 3    | col 0 = `/`; cols 1-7 = middle content area (7 chars wide); col 8 = `\`      |
| 4    | col 0 = `\`; cols 1-7 = bottom-half content area (7 chars wide); col 8 = `/` |
| 5    | col 1 = `\`; cols 2-6 = 5 underscores (`_____`); col 7 = `/`                 |

Lines 3 and 4 are **both** at the widest position — the hex has a
two-line flat middle, characteristic of flat-top hex geometry.
Vertical step between same-column hexes is **4 lines** (line 5 of one
hex shares its screen row with line 1 of the hex below). Adjacent
columns share one diagonal char per line — the `\` of column N's
line 3 sits at the same screen position as the `/` of column N+1's
line 3.

Color key (empty Plain — see A.2.1 for the canonical Plain exemplar):

| Position                       | Color  |
| ------------------------------ | ------ |
| Lines 2-4 background fill      | `bg=2` |
| Edges (lines 1, 5 underscores) | `fg=2` |
| Diagonals (`/` `\` lines 2-5)  | `fg=2` |

### A.2 Terrain gallery

Empty hex per terrain type — no unit, no building. Glyph + colors
locked to §4. The first three exemplars below establish the pattern;
A.2.4 through A.2.9 (Mountain, River, Bridge, Sea, Reef, Beach) follow
in subsequent commits.

#### A.2.1 Plain

```
  _____  
 /     \ 
/   .   \
\       /
 \_____/ 
```

Center glyph `.` at line 3 col 4 (mid of the 7-char content area).
The `.` reads as "open ground" — barely visible against green bg,
intentional.

| Position          | Color  |
| ----------------- | ------ |
| Lines 2-4 fill    | `bg=2` |
| Line 3 col 4: `.` | `fg=2` |
| Edges + diagonals | `fg=2` |

#### A.2.2 Road

```
  _____  
 /     \ 
/   =   \
\       /
 \_____/ 
```

Center glyph `=` at line 3 col 4 — reads as paved-road stripes.

| Position          | Color  |
| ----------------- | ------ |
| Lines 2-4 fill    | `bg=3` |
| Line 3 col 4: `=` | `fg=3` |
| Edges + diagonals | `fg=3` |

Road defense stars = 0 (§4); the border is unaccented — Road is
"neutral travel surface", visually low-emphasis.

#### A.2.3 Forest

```
  _____  
 /     \ 
/   *   \
\       /
 \_____/ 
```

Center glyph `*` at line 3 col 4 — tree canopy. Brighter green
border distinguishes Forest from Plain at a glance.

| Position          | Color   |
| ----------------- | ------- |
| Lines 2-4 fill    | `bg=2`  |
| Line 3 col 4: `*` | `fg=10` |
| Edges + diagonals | `fg=10` |

Forest hides ground units in fog (§4); the bright-green border
doubles as a "this terrain is mechanically special" visual flag.

#### A.2.4 Mountain

```
  _____  
 /     \ 
/   ^   \
\       /
 \_____/ 
```

Center glyph `^` at line 3 col 4 — mountain peak. Bright-white-on-
dark-gray maximizes silhouette against the green/blue map.

| Position          | Color   |
| ----------------- | ------- |
| Lines 2-4 fill    | `bg=8`  |
| Line 3 col 4: `^` | `fg=15` |
| Edges + diagonals | `fg=15` |

4-star defense (infantry-only per §4); the highest passive bonus in
the terrain table. Inf/Mech stationed here gain +3 vision (locked
2026-05-15 to match AW canon).

#### A.2.5 River

```
  _____  
 /     \ 
/   ~   \
\       /
 \_____/ 
```

Center glyph `~` at line 3 col 4 — water wave. Bright-cyan-on-blue
reads as "shallow water" (vs Sea's deeper render at A.2.7).

| Position          | Color   |
| ----------------- | ------- |
| Lines 2-4 fill    | `bg=4`  |
| Line 3 col 4: `~` | `fg=14` |
| Edges + diagonals | `fg=14` |

Infantry can ford with 2× move cost; vehicles + tread are blocked.
Air passes overhead without penalty.

#### A.2.6 Bridge

```
  _____  
 /     \ 
/   =   \
\       /
 \_____/ 
```

Center glyph `=` at line 3 col 4 — bridge planking. Bright-yellow-on-
cyan: the only terrain that combines two non-adjacent palette colors,
making bridges visually pop as the "crossing point."

| Position          | Color   |
| ----------------- | ------- |
| Lines 2-4 fill    | `bg=6`  |
| Line 3 col 4: `=` | `fg=11` |
| Edges + diagonals | `fg=11` |

All ground classes cross at Road move cost (1 per hex). The bg cyan
hint indicates "this is over water" — see §A.7 for grid samples
showing how Bridge integrates between two River/Sea hexes.

#### A.2.7 Sea

```
  _____  
 /     \ 
/   ~   \
\       /
 \_____/ 
```

Center glyph `~` at line 3 col 4 — same wave glyph as River, but
**fg=12 (bright blue)** on the same `bg=4`. Subtler contrast reads as
"deep water" vs River's bright-cyan "shallow water."

| Position          | Color   |
| ----------------- | ------- |
| Lines 2-4 fill    | `bg=4`  |
| Line 3 col 4: `~` | `fg=12` |
| Edges + diagonals | `fg=12` |

Naval-only base terrain. Ground/wheel/tread impassable; air 1-cost.

#### A.2.8 Reef

```
  _____  
 /     \ 
/   :   \
\       /
 \_____/ 
```

Center glyph `:` at line 3 col 4 — reef texture (dotted). Bright cyan
fg distinguishes Reef from neighboring Sea hexes when they tile.

| Position          | Color   |
| ----------------- | ------- |
| Lines 2-4 fill    | `bg=4`  |
| Line 3 col 4: `:` | `fg=14` |
| Edges + diagonals | `fg=14` |

1-star defense; hides naval units in fog from non-adjacent enemies
(§11.1). Sea move-cost 2 for sea units (vs 1 on open Sea) — slows
naval through reef zones.

#### A.2.9 Beach

```
  _____  
 /     \ 
/   _   \
\       /
 \_____/ 
```

Center glyph `_` at line 3 col 4 — sand line. Bright-yellow-on-tan-bg
is intentionally low-contrast; Beach reads as a "transitional"
terrain rather than commanding attention. The glyph also doubles the
edge underscores' visual weight, suggesting the sea-land boundary.

| Position          | Color   |
| ----------------- | ------- |
| Lines 2-4 fill    | `bg=3`  |
| Line 3 col 4: `_` | `fg=11` |
| Edges + diagonals | `fg=11` |

Movement: foot 1, wheel 2, tread 1, air 1, sea 1 — all-class
crossable. Beach is the canonical Lander unload tile; ports adjacent
to Beach get bonus tactical relevance.

### A.3 Buildings (4 × 3 owner states = 12 exemplars)

Each building has a single glyph at line 3 col 4 and a faction-tinted
rendering across 3 owner states (Owned / Neutral / Enemy). The hex
shape is identical to A.1; only the glyph and colors differ.

For the canonical gallery, **Own** = Bishop (white faction) and
**Enemy** = Hammer (red faction). The same color scheme generalizes to
the other 5 factions by swapping the bright/dark palette index per
§A.0 (Hawk yellow → `bg=11`/`fg=3`; Boots green → `bg=10`/`fg=2`;
Veil magenta → `bg=13`/`fg=5`; Talon cyan → `bg=14`/`fg=6`;
reserved blue → `bg=12`/`fg=4`).

**Shared owner-state color scheme** (applies to all 4 buildings):

| Owner state    | Lines 2-4 bg | Line 3 glyph fg | Edges + diagonals fg | Reading                        |
| -------------- | ------------ | --------------- | -------------------- | ------------------------------ |
| Owned (Bishop) | `bg=15`      | `fg=0`          | `fg=15`              | bright white bg, black glyph   |
| Neutral        | `bg=8`       | `fg=7`          | `fg=7`               | dark gray bg, light gray glyph |
| Enemy (Hammer) | `bg=1`       | `fg=9`          | `fg=9`               | dark red bg, bright red glyph  |

Phase 12 self-test parameterizes (4 glyphs × 3 owner states × 6
factions) = 72 building exemplars. The 12 listed here cover the
"Bishop-own / neutral / Hammer-enemy" canonical slice.

#### A.3.1 HQ (`H`)

Income 1000/turn (when owned). Builds ground + air (locked decision 9
v1 simplification). **Capture = game over** for previous owner (§10.4).

**Owned** (Bishop):
```
  _____  
 /     \ 
/   H   \
\       /
 \_____/ 
```

**Neutral**:
```
  _____  
 /     \ 
/   H   \
\       /
 \_____/ 
```

**Enemy** (Hammer):
```
  _____  
 /     \ 
/   H   \
\       /
 \_____/ 
```

(Neutral HQ is rare in practice — scenarios usually pre-assign every
HQ to a faction — but the render is canonical for completeness.)

#### A.3.2 City (`c`)

Income 1000/turn. Heals stationed ground +2 HP at start-of-turn
(§12.5). Captures grant +1000/turn income to the capturing faction.

**Owned** (Bishop):
```
  _____  
 /     \ 
/   c   \
\       /
 \_____/ 
```

**Neutral**:
```
  _____  
 /     \ 
/   c   \
\       /
 \_____/ 
```

**Enemy** (Hammer):
```
  _____  
 /     \ 
/   c   \
\       /
 \_____/ 
```

#### A.3.3 Factory (`F`)

No income. Heals stationed ground +2 HP + restocks ammo. **Builds
ground units only** (§12.2).

**Owned** (Bishop):
```
  _____  
 /     \ 
/   F   \
\       /
 \_____/ 
```

**Neutral**:
```
  _____  
 /     \ 
/   F   \
\       /
 \_____/ 
```

**Enemy** (Hammer):
```
  _____  
 /     \ 
/   F   \
\       /
 \_____/ 
```

#### A.3.4 Port (`P`)

No income. Heals stationed naval +2 HP + refuels to max. **Builds
naval units only** (§12.2). Sits on Sea/Beach in-game; the faction-
tinted hex bg fully overrides the underlying terrain bg (the building
"paints over" the water).

**Owned** (Bishop):
```
  _____  
 /     \ 
/   P   \
\       /
 \_____/ 
```

**Neutral**:
```
  _____  
 /     \ 
/   P   \
\       /
 \_____/ 
```

**Enemy** (Hammer):
```
  _____  
 /     \ 
/   P   \
\       /
 \_____/ 
```

### A.4 Unit roster (canonical fresh-built, Bishop-owned)

All 17 units rendered in their canonical "fresh-built" state:

- **Full HP** (10/10) — line 4 intentionally empty; the HP bar
  `=N/10` appears only when wounded (HP < 10).
- **Full fuel + ammo** — no warning indicators.
- **Vet 0** — no `★` pip.
- **`/has-moved`** and **`/has-fired`** both `false` — unit reads as
  "ready" (full-brightness faction tint, not dimmed-out).
- **Owned by Bishop** (white) — glyph `fg=15`.

Unit glyph sits at **line 3, col 4** (center of the 7-char content
area). The glyph is the only difference between any two unit
exemplars in this section; the surrounding hex shape is identical
per §A.1.

Ground + air units render on Plain (`bg=2`); sea units render on Sea
(`bg=4`) — see §A.4.3 color override.

**Shared color rules for §A.4.1 (Ground) + §A.4.2 (Air):**

| Position                  | Color   |
| ------------------------- | ------- |
| Lines 2-4 background fill | `bg=2`  |
| Line 3 col 4: unit glyph  | `fg=15` |
| Edges + diagonals         | `fg=2`  |

**Shared color rules for §A.4.3 (Sea):**

| Position                  | Color   |
| ------------------------- | ------- |
| Lines 2-4 background fill | `bg=4`  |
| Line 3 col 4: unit glyph  | `fg=15` |
| Edges + diagonals         | `fg=4`  |

#### A.4.1 Ground units (9)

**Infantry** — `i` — cost 1000, move 3 (foot), captures.

```
  _____  
 /     \ 
/   i   \
\       /
 \_____/ 
```

**Mech** — `m` — cost 3000, move 2 (foot), captures + anti-vehicle bazooka.

```
  _____  
 /     \ 
/   m   \
\       /
 \_____/ 
```

**Recon** — `r` — cost 4000, move 8 (wheel), vision 5, fog scout.

```
  _____  
 /     \ 
/   r   \
\       /
 \_____/ 
```

**APC** — `u` — cost 5000, move 6 (wheel), transports Inf/Mech.

```
  _____  
 /     \ 
/   u   \
\       /
 \_____/ 
```

**Tank** — `t` — cost 7000, move 6 (tread), main battle tank.

```
  _____  
 /     \ 
/   t   \
\       /
 \_____/ 
```

**Heavy Tank** — `T` — cost 16000, move 5 (tread), best direct-fire ground.

```
  _____  
 /     \ 
/   T   \
\       /
 \_____/ 
```

**Anti-Air** — `α` — cost 8000, move 6 (wheel), hits air + soft ground. ASCII-strict fallback glyph: `A` (uppercase; avoids the conflict with Mountain's `^`).

```
  _____  
 /     \ 
/   α   \
\       /
 \_____/ 
```

**Artillery** — `a` — cost 6000, move 5 (wheel), indirect range 2-3.

```
  _____  
 /     \ 
/   a   \
\       /
 \_____/ 
```

**Rocket** — `R` — cost 15000, move 5 (wheel), indirect range 3-5.

```
  _____  
 /     \ 
/   R   \
\       /
 \_____/ 
```

#### A.4.2 Air units (3)

**Fighter** — `f` — cost 20000, move 9, air-superiority.

```
  _____  
 /     \ 
/   f   \
\       /
 \_____/ 
```

**Bomber** — `b` — cost 22000, move 7, anti-ground.

```
  _____  
 /     \ 
/   b   \
\       /
 \_____/ 
```

**Battle Copter** — `h` — cost 9000, move 6, carries 1 ground unit at half move.

```
  _____  
 /     \ 
/   h   \
\       /
 \_____/ 
```

#### A.4.3 Sea units (5)

Color override per section header: `bg=4` (Sea), edges `fg=4`.

**Lander** — `L` — cost 12000, move 6, transports 2 ground units.

```
  _____  
 /     \ 
/   L   \
\       /
 \_____/ 
```

**Destroyer** — `d` — cost 18000, move 8, anti-sub + anti-air (Cruiser merge).

```
  _____  
 /     \ 
/   d   \
\       /
 \_____/ 
```

**Submarine** — `s` — cost 20000, move 5, hidden when submerged.

```
  _____  
 /     \ 
/   s   \
\       /
 \_____/ 
```

**Battleship** — `B` — cost 28000, move 5, indirect range 2-6.

```
  _____  
 /     \ 
/   B   \
\       /
 \_____/ 
```

**Carrier** — `C` — cost 30000, move 5, refuels adjacent air; no attack v1.

```
  _____  
 /     \ 
/   C   \
\       /
 \_____/ 
```

#### A.4.4 Faction-tint variants (color parameterization)

§A.4.1-3 renders all units with Bishop's color set (`fg=15` for
ready/own, `fg=7` for moved/dim). Every other faction follows the
**identical hex shape, glyph, and content canvas** — only the
line-3 glyph `fg` substitutes per the matrix below. No further ASCII
exemplars are needed; what changes is one color channel.

**Faction-color matrix** (locked; applies to all 17 units × all 3
owner-states):

| Faction        | Ready (own) fg | Moved (own / dim) fg | Enemy fg |
| -------------- | -------------- | -------------------- | -------- |
| Bishop (white) | 15             | 7                    | 7        |
| Hammer (red)   | 9              | 1                    | 1        |
| Hawk (yellow)  | 11             | 3                    | 3        |
| Boots (green)  | 10             | 2                    | 2        |
| Veil (purple)  | 13             | 5                    | 5        |
| Talon (cyan)   | 14             | 6                    | 6        |

Column semantics:

- **Ready (own)** — unit owned by viewing faction, `/has-moved =
  false` AND `/has-fired = false`. Bright variant of the faction
  color.
- **Moved (own / dim)** — unit owned by viewing faction, has acted
  this turn. Dim (non-bright) variant.
- **Enemy** — unit owned by another faction. Dim variant of the
  owning faction's color, viewed from outside.

Bishop's "Moved" and "Enemy" both resolve to `fg=7` because there's
no "darker white" in the 16-color palette — Bishop relies on
action-state context (own units always actionable on Bishop's turn;
enemy units never) rather than fg differentiation.

**Special case — Bishop unit moved-on-own-City** (low-contrast risk):
a moved Bishop unit stationed on a Bishop-owned City would render
as `fg=7` (gray) on `bg=15` (bright white) — barely visible. To
preserve readability, when a unit is **stationed on its own
faction's building AND in moved state**, the glyph fg overrides to
`fg=8` (dark gray) instead of the standard moved-dim. Universal
across all 6 factions, but only Bishop visually needs it (other
factions' bright/dim bg-fg pairs already have sufficient contrast).

**Worked example** — Tank, fresh-built, owned-and-ready, on Plain.
The ASCII below is byte-for-byte identical regardless of which
faction owns it; only the `t` glyph fg color changes:

```
  _____
 /     \
/   t   \
\       /
 \_____/
```

Per-faction render — same ASCII, fg variants:

| Faction | Tank glyph fg (own ready) | Reads as                                               |
| ------- | ------------------------- | ------------------------------------------------------ |
| Bishop  | 15                        | bright white `t` on green plain                        |
| Hammer  | 9                         | bright red `t` on green plain                          |
| Hawk    | 11                        | bright yellow `t` on green plain                       |
| Boots   | 10                        | bright green `t` on green plain (low contrast — known) |
| Veil    | 13                        | bright magenta `t` on green plain                      |
| Talon   | 14                        | bright cyan `t` on green plain                         |

Boots' low-contrast-on-Plain case is acknowledged in the §18.10
Phase 0 readability gate — if it fails acceptance, Boots' glyph
shifts to a darker green or the unit shows a small color-coded
border accent. Iterate until pass per the locked decision 35 rule.

**Phase 12 self-test surface**: `render-unit-faction-test`
parameterizes `(17 units × 6 factions × 3 owner-states)` = **306
testable exemplars**. Compact spec → exhaustive test coverage.

Sea units (§A.4.3) use the Sea bg `bg=4` instead of Plain `bg=2`;
the faction-fg matrix above applies unchanged. Mountain / Forest /
Reef / Beach / Bridge / Road backgrounds (§A.2) compose with the
same matrix when units occupy non-Plain terrain.

### A.5 Content-canvas state reference

The hex content canvas encodes runtime state — HP, fuel/ammo,
veterancy, capture progress, transport cargo, and building overlay —
in two slots:

- **Line 2** (5-char content area, cols 2-6): top-half overlay.
  Carries the **building marker** (small letter when a unit stands on
  a building hex), **capture-progress overlay** (when capturing), or
  **carried-unit marker(s)** (when a transport has cargo). Empty
  when on terrain with no building and no transport cargo.
- **Line 4** (7-char content area, cols 1-7): bottom-half overlay.
  Carries **HP bar**, **fuel-low / ammo-empty indicators**, and
  **veterancy pips**. Empty for canonical "fresh, full-HP, vet 0,
  fully-supplied" state (see §A.4).

This section is the reference for every non-trivial state — implementers
should copy these patterns verbatim, and Phase 12 self-tests assert
byte-for-byte equality.

#### A.5.0 Line-4 slot priority (locked)

Line 4 is 7 chars wide (cols 1-7). Slot priority left-to-right:

| Cols | Slot | Content |
| --- | --- | --- |
| 1-5 | HP bar | `=N/10` when N < 10; otherwise empty (5 spaces). |
| 6 | Padding | Always space. |
| 7 | Status / vet tier | Single char: `★` (vet pip, highest priority) > `a` (ammo-empty) > `f` (fuel-low) > empty. **Note**: at vet ≥ 1 the pip masks any ammo/fuel warning at col 7; players who need combat-readiness for a veteran unit query the side panel (§18.1 "Selected:") which shows full HP/fuel/ammo. Vet is high-signal-low-frequency; ammo/fuel is queryable, so vet wins the single-char slot. |

**Veterancy expansion** (locked 2026-05-16):

- Vet 1: pip at col 7
- Vet 2: pips at cols 6-7 (overrides the padding space)
- Vet 3: when full HP — pips at cols 5-7 (3 pips visible). When
  wounded — pips clamp to cols 6-7 (max 2 pips visible, identical
  to vet 2 rendering). HP bar always wins cols 1-5.

The "clamp to 2 when wounded" rule (previously: "override HP bar's
trailing `0`") was changed 2026-05-16 to avoid visually ambiguous
`=N/1★★★` reading. Vet-3 wounded units rely on the side panel
(§18.1) for exact tier display; the hex shows the wounded HP
correctly, which is the higher-priority signal.

**Fuel-low / ammo-empty thresholds**:

- `fuel-low`: fuel ≤ 25% of max (e.g., Bomber max 99, threshold ≤ 24)
- `ammo-empty`: ammo = 0
- When both fuel-low AND ammo-empty: ammo-empty wins (combat-blocking
  > movement-degrading).
- When a status indicator AND vet pip both want col 7: vet pip wins.
  Status indicators surface in the side panel even when hidden by
  vet pips on the hex.

#### A.5.1 HP variations

Tank on Plain at 5 HP levels. Glyph `t` at line 3 col 4 throughout;
only line 4 changes.

**HP 10/10** (full — canonical, line 4 empty):
```
  _____  
 /     \ 
/   t   \
\       /
 \_____/ 
```

**HP 8/10**:
```
  _____  
 /     \ 
/   t   \
\=8/10  /
 \_____/ 
```

**HP 5/10**:
```
  _____  
 /     \ 
/   t   \
\=5/10  /
 \_____/ 
```

**HP 3/10**:
```
  _____  
 /     \ 
/   t   \
\=3/10  /
 \_____/ 
```

**HP 1/10** (one hit from destruction):
```
  _____  
 /     \ 
/   t   \
\=1/10  /
 \_____/ 
```

#### A.5.2 Supply indicators

Tank on Plain, full HP, vet 0. Line 4 col 7 carries the status char.

**Fuel-low** (`fuel ≤ 25%` of max):
```
  _____  
 /     \ 
/   t   \
\      f/
 \_____/ 
```

**Ammo-empty** (ammo = 0):
```
  _____  
 /     \ 
/   t   \
\      a/
 \_____/ 
```

**Wounded + ammo-empty** (HP 5, ammo 0):
```
  _____  
 /     \ 
/   t   \
\=5/10 a/
 \_____/ 
```

#### A.5.3 Veterancy pips

Tank on Plain, full HP, full supply. Pip count = `/vet-tier`
(see §7.8).

**Vet 0** (canonical — no pip, identical to A.4 Tank entry):
```
  _____  
 /     \ 
/   t   \
\       /
 \_____/ 
```

**Vet 1** (`/kills` ≥ 3):
```
  _____  
 /     \ 
/   t   \
\      ★/
 \_____/ 
```

**Vet 2** (`/kills` ≥ 5):
```
  _____  
 /     \ 
/   t   \
\     ★★/
 \_____/ 
```

**Vet 3** (`/kills` ≥ 8):
```
  _____  
 /     \ 
/   t   \
\    ★★★/
 \_____/ 
```

#### A.5.4 Combined state: wounded + veteran

Tank on Plain, HP 5, vet 2 — both HP bar (cols 1-5) and 2 pips
(cols 6-7) fit on line 4:

```
  _____  
 /     \ 
/   t   \
\=5/10★★/
 \_____/ 
```

Tank on Plain, HP 5, vet 3 — **HP bar takes precedence; vet display
clamps to 2 pips when wounded**:

```
  _____  
 /     \ 
/   t   \
\=5/10★★/
 \_____/ 
```

When wounded (HP < 10), vet pips can occupy at most cols 6-7
(maximum 2 visible). Vet-3 wounded units render identically to
vet-2 wounded — the side panel (§18.1) is the authoritative source
for exact tier. Trade-off chosen 2026-05-16 over the previous
"override HP bar's trailing `0`" rule, which was visually ambiguous
(line 4 reading `=5/1★★★` could be parsed as HP 5/1 instead of
HP 5/10).

At **full HP** (no HP bar), all 3 vet pips display cleanly at cols
5-7 — see §A.5.3 Vet 3 example.

#### A.5.5 Ready vs moved tinting

Glyph-color encodes action state — no line-4 difference. Bishop Tank
on Plain.

**Ready** (`/has-moved = false` AND `/has-fired = false`):
```
  _____  
 /     \ 
/   t   \
\       /
 \_____/ 
```

Glyph fg = `15` (bright white, Bishop owned).

**Moved** (`/has-moved = true` OR `/has-fired = true` — no further
action possible this turn):
```
  _____  
 /     \ 
/   t   \
\       /
 \_____/ 
```

Glyph fg = `7` (dim white / gray — the non-bright variant of Bishop's
color, per A.0). Reads as "this unit is done for the turn."

For enemy factions, ready = bright variant (`fg=9` for Hammer), moved
= dim variant (`fg=1`). The bright/dim distinction is universal
across all 6 factions and is the primary "who's still actionable
this turn" signal at a glance.

#### A.5.6 Capturing marker

Infantry on enemy-owned City, capture-progress 5 of 20. Line 2
carries `c=N` overlay (cols 2-5: 4 chars `c=5 ` or `c=10`).

**Bishop Inf capturing Hammer City** (capture-progress 5):
```
  _____  
 /c=5  \ 
/   i   \
\       /
 \_____/ 
```

- Line 2 cols 2-5: `c=5 ` (4 chars + 1 trailing space within the 5-char content area)
- Line 3 col 4: `i` (Bishop Inf, `fg=15`)
- Lines 2-4 bg: `bg=1` (Hammer-enemy city bg per §A.3) — but the
  glyph and `c=N` overlay are in Bishop's bright-white `fg=15` to
  signal "Bishop is currently capturing this Hammer building."

The `c=N` overlay updates each turn the capture continues; clears
when the Inf moves off the building or the capture completes.

#### A.5.7 Carried-unit markers (transport cargo)

Transports (APC, Lander, Battle Copter) carry ground units. Cargo
type indicated on line 2.

**APC carrying 1 Infantry** (Bishop-owned, on Plain):
```
  _____  
 / i   \ 
/   u   \
\       /
 \_____/ 
```

- Line 2 col 3: `i` (cargo marker — Inf inside)
- Line 3 col 4: `u` (APC)

**Lander carrying 1 Tank + 1 Mech** (Bishop-owned, on Sea):
```
  _____  
 / tm  \ 
/   L   \
\       /
 \_____/ 
```

- Line 2 cols 3-4: `tm` (cargo markers — Tank + Mech inside)
- Line 3 col 4: `L` (Lander)
- Lines 2-4 bg: `bg=4` (Sea)

**Battle Copter carrying 1 Infantry** (Bishop-owned, on Plain):
```
  _____  
 / i   \ 
/   h   \
\       /
 \_____/ 
```

#### A.5.8 Unit on a building

Unit glyph (line 3) sits on top of building-tinted hex. Line 2 col 4
shows the building marker (small `H` / `c` / `F` / `P`).

**Bishop Inf stationed on Bishop-owned City** (start-of-turn heal applies):
```
  _____  
 /  c  \ 
/   i   \
\       /
 \_____/ 
```

- Lines 2-4 bg: `bg=15` (Bishop-owned City bg per §A.3)
- Line 2 col 4: `c` (city marker, `fg=0` for contrast against bright white bg)
- Line 3 col 4: `i` (Bishop Inf, `fg=0`)

Note: when on an owned building, the Inf glyph uses **black `fg=0`**
instead of bright white `fg=15`, because bright white on bright white
bg would be invisible. The convention reverses with the building's
own glyph-color rule: the dominant color of the hex is the building's
bg, and the unit glyph adapts to maintain contrast.

For Bishop Inf stationed on enemy Hammer City (mid-capture, see A.5.6
for combined render): bg=1 (Hammer enemy), glyph stays fg=15 (Bishop's
bright color reads cleanly against dark-red).

### A.6 Weather render

Weather (§11.5) is rendered as a top-bar indicator on the screen
(§18.1), not as per-hex tinting. Per-hex effects (e.g., reduced
vision under Rain, slower tread move under Snow) are not visually
distinguished on the hex itself — players read weather state from
the top-bar.

**Top-bar weather color table** (locked):

| Weather | Glyph in top-bar | Fg | Notes |
| --- | --- | --- | --- |
| `/clear` | `/clear` | `fg=7` (dim) | Default; no mechanical effect — render dim to deprioritise visual attention |
| `/rain` | `/rain` | `fg=12` (bright blue) | Reduced vision, ground move penalty |
| `/snow` | `/snow` | `fg=15` (bright white) | Tread-move penalty; foot units unaffected |
| `/sandstorm` | `/sandstorm` | `fg=11` (bright yellow) | Indirect range penalty |
| (custom) | scenario-supplied | `fg=7` (default chrome) | `/weather-table-extras` may pin a color via `/render-fg` |

The full A.8.2 canonical render shows `/clear` in `fg=7`; switching
weather flips the glyph and color per the table above.  Per-hex
glyphs / backgrounds remain unchanged across weathers.

### A.7 Interlocking grid samples (with content)

This section establishes the canonical multi-hex interlocking pattern
and demonstrates how content (terrain glyphs, unit glyphs, building
markers, line-4 overlays) sits within a real grid. The 7-hex diamond
layout below is the canonical test pattern — Phase 12's
`render-grid-gallery-test` asserts byte-for-byte equality against
the four exemplars in §A.7.1-4.

#### A.7.0 Layout reference

The canonical interlocking pattern uses a **7-hex diamond** spanning
23 chars wide × 13 rows tall:

```
Hex layout (q,r odd-q offset, 0-indexed at center):
                                    
              A (0,0)               <- even-q "high"; lines 1-5 at rows 0-4
                                    
       B (-1,0)        C (1,0)      <- odd-q "low";  lines 1-5 at rows 2-6
                                    
              D (0,1)               <- even-q "high"; lines 1-5 at rows 4-8
                                    
       E (-1,1)        F (1,1)      <- odd-q "low";  lines 1-5 at rows 6-10
                                    
              G (0,2)               <- even-q "high"; lines 1-5 at rows 8-12
```

**Per-hex content-cell coordinates** (where to write a glyph):

| Hex | Line 3 row | Line 3 content cols | Line 3 center col | Line 4 row | Line 4 content cols |
| --- | ---------- | ------------------- | ----------------- | ---------- | ------------------- |
| A   | 2          | 8-14                | 11                | 3          | 8-14                |
| B   | 4          | 1-7                 | 4                 | 5          | 1-7                 |
| C   | 4          | 15-21               | 18                | 5          | 15-21               |
| D   | 6          | 8-14                | 11                | 7          | 8-14                |
| E   | 8          | 1-7                 | 4                 | 9          | 1-7                 |
| F   | 8          | 15-21               | 18                | 9          | 15-21               |
| G   | 10         | 8-14                | 11                | 11         | 8-14                |

#### A.7.1 Empty 7-hex diamond (canonical interlocking baseline)

All 7 hexes on Plain, no units, no buildings.

```
         _____
        /     \
  _____/       \_____
 /     \       /     \
/       \_____/       \
\       /     \       /
 \_____/       \_____/
 /     \       /     \
/       \_____/       \
\       /     \       /
 \_____/       \_____/
       \       /
        \_____/
```

This grid demonstrates the brick-offset interlocking — even-q cols
(0) sit "high", odd-q cols (-1, +1) sit 2 rows lower. Adjacent
columns share diagonals (e.g., the `\` of B's line 3 is the same
character as the `/` of A's line 4-to-5 transition at row 4 col 8).

Color key: every hex's lines 2-4 fill `bg=2`; edges + diagonals `fg=2`.

#### A.7.2 Terrain mix

Same 7-hex diamond, each hex carrying a different terrain glyph at
line 3 center (per §A.2 glyph table).

| Hex | Terrain  | Glyph |
| --- | -------- | ----- |
| A   | Plain    | `.`   |
| B   | Forest   | `*`   |
| C   | Mountain | `^`   |
| D   | Road     | `=`   |
| E   | River    | `~`   |
| F   | Sea      | `~`   |
| G   | Beach    | `_`   |

```
         _____
        /     \
  _____/   .   \_____
 /     \       /     \
/   *   \_____/   ^   \
\       /     \       /
 \_____/   =   \_____/
 /     \       /     \
/   ~   \_____/   ~   \
\       /     \       /
 \_____/   _   \_____/
       \       /
        \_____/
```

Per-hex bg + fg per §A.2 (Plain `bg=2`; Forest `bg=2 fg=10`; Mountain
`bg=8 fg=15`; Road `bg=3 fg=3`; River `bg=4 fg=14`; Sea `bg=4 fg=12`;
Beach `bg=3 fg=11`). Note River and Sea share the wave glyph `~` but
differ in `fg` (bright-cyan vs bright-blue) — the only visual cue
distinguishing them in monochrome ASCII.

#### A.7.3 Unit formation (all Bishop, all on Plain, all canonical fresh-built)

7 different unit types from §A.4, fresh-built state (full HP, full
supply, vet 0, ready). All Plain bg (`bg=2`), all Bishop fg (`fg=15`).

| Hex | Unit       | Glyph |
| --- | ---------- | ----- |
| A   | Infantry   | `i`   |
| B   | Tank       | `t`   |
| C   | Artillery  | `a`   |
| D   | Recon      | `r`   |
| E   | Mech       | `m`   |
| F   | Battle Cop | `h`   |
| G   | APC        | `u`   |

```
         _____
        /     \
  _____/   i   \_____
 /     \       /     \
/   t   \_____/   a   \
\       /     \       /
 \_____/   r   \_____/
 /     \       /     \
/   m   \_____/   h   \
\       /     \       /
 \_____/   u   \_____/
       \       /
        \_____/
```

Demonstrates that all 7 unit glyphs are mutually distinguishable at
the canonical render width. Line 4 is uniformly empty (canonical
fresh-built state).

#### A.7.4 Mixed scene (units + buildings + state overlays)

A combat-relevant snapshot showing how the canvas combines glyphs
from §A.2 / §A.3 / §A.4 / §A.5 in a single grid. Bishop = own
(white); Hammer = enemy (red).

| Hex | Contents                            | Line 3 col | Line 4 overlay       |
| --- | ----------------------------------- | ---------- | -------------------- |
| A   | Bishop Inf, full HP, vet 0          | `i` col 11 | empty                |
| B   | Bishop Tank, **HP 5**, vet 0        | `t` col 4  | `=5/10  ` cols 1-7   |
| C   | Hammer Recon, full HP, **vet 1**    | `r` col 18 | `      ★` cols 15-21 |
| D   | Bishop **City** (owned), no unit    | `c` col 11 | empty                |
| E   | Hammer Mech, full HP, vet 0         | `m` col 4  | empty                |
| F   | Bishop Bomber, **moved** (dim fg=7) | `b` col 18 | empty                |
| G   | Hammer **HQ** (enemy), no unit      | `H` col 11 | empty                |

```
         _____
        /     \
  _____/   i   \_____
 /     \       /     \
/   t   \_____/   r   \
\=5/10  /     \      ★/
 \_____/   c   \_____/
 /     \       /     \
/   m   \_____/   b   \
\       /     \       /
 \_____/   H   \_____/
       \       /
        \_____/
```

**Color key**:

| Hex | Lines 2-4 bg | Line 3 glyph fg           | Notes                                           |
| --- | ------------ | ------------------------- | ----------------------------------------------- |
| A   | `bg=2`       | `fg=15` (Bishop bright)   | Plain                                           |
| B   | `bg=2`       | `fg=15`                   | Plain; HP bar at line 4 cols 1-5 also `fg=15`   |
| C   | `bg=2`       | `fg=1` (Hammer dim/enemy) | Plain; `★` at line 4 col 21 also `fg=1`         |
| D   | `bg=15`      | `fg=0` (black-on-white)   | Bishop-owned City — building bg overrides Plain |
| E   | `bg=2`       | `fg=1` (Hammer dim/enemy) | Plain                                           |
| F   | `bg=2`       | `fg=7` (Bishop dim/moved) | Plain; dim variant signals "acted this turn"    |
| G   | `bg=1`       | `fg=9` (Hammer bright)    | Hammer-enemy HQ — building bg `bg=1`            |

Demonstrates simultaneous rendering of:
- Terrain bg (Plain, hexes A/B/C/E/F)
- Building bg overrides (City `bg=15` in D; HQ `bg=1` in G)
- Faction-color glyphs (Bishop bright `fg=15`, Hammer bright `fg=9`,
  Hammer dim `fg=1`)
- Action-state dim (Bomber in F at `fg=7`)
- HP bar overlay (B's `=5/10  ` at line 4 cols 1-7)
- Vet pip overlay (C's `★` at line 4 col 21)
- Mixed faction proximity (Bishop-own and Hammer-enemy hexes
  adjacent in same grid)

This is the canonical regression for Phase 12's grid renderer —
correctness of this single exemplar implies correctness of the entire
§A.2-§A.5 specification surface.

### A.8 Full screen render (canonical 76×25)

Composes every preceding gallery section into a single byte-for-byte
canonical screen render at the **76-char × 25-line reference size**.
The §A.8.2 render below fits comfortably inside the 120×40 minimum
terminal (§18.3) with substantial side margins and headroom.
Phase 12 ships `render-screen-gallery-test` asserting the renderer
produces this exact output for the named game state.

#### A.8.1 Canonical game state

- **Day 3, Turn 6, Bishop's turn**
- Bishop funds 8500; power meter 3.2 stars; weather `/clear` (default)
- Active scenario: tutorial.bw 25×18; viewport scrolled to mid-map
- Cursor on Bishop's Tank at hex (q, r) = (-1, 0) — the `t` in the
  §A.7.4 diamond
- Selected unit: Bishop Tank, HP 5/10 (wounded from previous combat),
  fuel 65/70, ammo 9/9, ready, vet 0
- Adjacent enemies: Hammer Recon to NE (vet 1), Hammer Mech to SW
- Last combat event (Bishop's previous turn): Bishop Tank attacked
  Hammer Recon, dealt 4 HP, took counter 3 HP
- Minimap slice centered on cursor; 19 wide × 5 tall

#### A.8.2 Canonical render

```
   Day 3 · Turn 6 · Bishop (funds 8500 · ★3.2 · /clear)
+--------------------------------------------------------------------------+
|                                                                          |
|              _____                                                       |
|             /     \                                                      |
|       _____/   i   \_____                                                |
|      /     \       /     \                                               |
|     /   t   \_____/   r   \                                              |
|     \=5/10  /     \      ★/                                              |
|      \_____/   c   \_____/                                               |
|      /     \       /     \                                               |
|     /   m   \_____/   b   \                                              |
|     \       /     \       /                                              |
|      \_____/   H   \_____/                                               |
|            \       /                                                     |
|             \_____/                                                      |
|                                                                          |
+-------------------------------------------------+--minimap---------------+
| Selected: Bishop Tank ★0 (HP 5/10)              |   ..^^...~~..........  |
| Adjacent: Hammer Recon ★1, Hammer Mech          |   ..tir...~..........  |
+-------------------------------------------------+   ...c...............  |
| Combat: Bishop Tank → Hammer Recon              |   ...mb..............  |
|   dealt 4 HP · counter 3 HP                     |   ....H..............  |
+-------------------------------------------------+------------------------+
   WASD/HJKL move · Enter select · Space menu · Shift-E end · ? help
```

#### A.8.3 Region-by-region color key

**Top bar** (row 0, floating — no enclosing borders):
- Bg: `bg=0` (black)
- Text fg: `fg=15` (bright white)
- `Bishop` callsign tinted `fg=15` (Bishop's own bright); `★3.2` in
  `fg=11` (bright yellow — universal Power-meter color, not
  faction-specific)
- `/clear` weather indicator in `fg=7` (dim — weather effects on
  combat are visualized by hex modifiers, not the top-bar text)

**Main viewport** (the 7-hex diamond, per §A.7.4):
- Per §A.7.4's color key — terrain bg per §A.2; building bg per §A.3
  for hexes D and G; faction fg per §A.4.4 for unit glyphs; line-4
  overlays per §A.5

**Side panel** (left column, top half, 2 content lines):
- Bg: `bg=0` (black)
- Text fg: `fg=15` (bright white) for headings; `fg=11` for `★1`
  vet-pip indicator inline
- Hammer faction names tinted `fg=9` (Hammer bright) when mentioning
  enemy units; Bishop's "Tank" tinted `fg=15`

**Combat log section** (left column, bottom half, 2 content lines):
- Bg `bg=0`; text `fg=7` (dim — past event); attacker / defender
  faction names colored per §A.4.4 inline
- `Bishop Tank` → `fg=15`; `Hammer Recon` → `fg=9`

**Minimap section** (right column, full section height, 5 content lines):
- Bg `bg=0`; per-hex chars colored per terrain/unit ownership
- Cursor position (the `t` in the centered slice) rendered inverse:
  `bg=15 fg=0` instead of the usual `bg=0 fg=15`
- Buildings (`c`, `H`) inverse-tinted with their owner's bg
- Hammer enemy units (`r`, `m`, `H`) in `fg=9` (Hammer bright)
- Bishop own units (`t`, `i`, `b`) in `fg=15` (Bishop bright)
- Terrain `.` `^` `~` glyphs in their terrain fg per §A.2
- The slice is centered on the cursor with edge clamping (§18.5)

**Borders** (`+`, `-`, `|`):
- All `fg=7` (dim gray) `bg=0` (black) — borders are chrome, not
  primary content
- The `--minimap` label inline in the section top divider is rendered
  in `fg=7` (chrome color, same as surrounding `-`)
- The floating top-bar and keybind rows have NO surrounding borders
  (no leading `|`, no top/bottom `+---+` chrome)

#### A.8.4 Phase 12 self-test contract

`render-screen-gallery-test` asserts:

1. Total render = exactly **25 lines tall × 76 chars wide** (chrome
   rows shown above; the floating top-bar at row 0 and floating
   keybind at row 24 are rstripped of trailing whitespace).
2. Top bar text matches `Day 3 · Turn 6 · Bishop (funds 8500 · ★3.2
   · /clear)` byte-for-byte, indented 3 cols (no leading `|`).
3. Main viewport hex-grid bytes match the §A.7.4 mixed scene exactly,
   positioned in rows 2–16 inside the viewport panel.
4. Side panel text matches the two lines specified in §A.8.1 (left
   column, rows 18–19).
5. Combat log lines match (left column, rows 21–22).
6. Minimap rows match (19 chars each × 5 rows; right column, rows
   18–22 — minimap row 3 sits beside the mid-divider).
7. The section top divider (row 17) embeds the `--minimap` label
   inline starting at col 51 (2 dashes after the central `+`).
8. All ANSI color codes per §A.8.3 are emitted in the correct
   sequence (test parses SGR codes from the output stream).

#### A.8.5 Out-of-scope variations

The canonical render fixes the game state and faction perspective.
Variations that should NOT be rendered as separate A.8.* subsections
because they're covered by the parameter matrices above:

- Other CO turns (Hammer / Hawk / Boots / Veil / Talon active) —
  same layout, top-bar text substitutes CO name + faction fg
- Other map states (different unit positions, different terrain mix)
  — covered by §A.7 layout reference + §A.2-A.5 content rules
- Weather active (`/rain`, `/snow`, `/sandstorm`) — top-bar `/clear`
  substitutes; per-hex visuals unchanged (weather affects mechanics,
  not rendering per §11.5)
- Power-active state (`/power-active = true`) — top-bar `★3.2`
  changes to `★ACTIVE` or similar in `fg=11`; main viewport hex
  glyphs flash to bright variant during the active-turn duration

A.8 is intentionally a single frozen frame; animation/state-transition
specs live in §18 prose, not the gallery.

---

## End

This is the source of truth. Phases read from this document; deviations
require updating this document first.

Living memory of design history — what was debated, what was considered
and rejected, why decisions landed where they did — lives in
`memory/plan_showcase_bestagon.md` (planner's working notes; not the spec).
