# Bestagon Wars — Technical Design Document

**Status:** SKELETON (2026-05-15).  Sections marked LOCKED have decisions
the GDD or de-risk prototypes already pinned.  PROPOSED sections carry a
recommendation but no commitment.  OPEN sections need the user's call
before Phase 1 main coding can finalize.

**Relationship to the GDD:** the GDD (`bestagon-design.md`) is the source
of truth for **what** the game does.  This TDD is the source of truth for
**how** the code realizes it: module boundaries, memory model, save
discipline, actor topology, render pipeline, CLI surface, perf budgets,
build & test harness.

Cross-references use GDD section numbers (`§N`) and prototype file names
(`proto-*.trx`).  Anything specified in the GDD is not restated here.

---

## Table of contents

| Tier | Section | Topic | Status |
| --- | --- | --- | --- |
| 1 | 1 | Save/restore discipline | **LOCKED** (B', 2026-05-15) |
| 1 | 2 | Memory model & VM-size budgets | **LOCKED** (mostly global + soft warnings on size caps, 2026-05-16) |
| 1 | 3 | State representation (cells, units, factions) | **LOCKED** (A-SoA + /units Dict, 2026-05-15) |
| 1 | 4 | Actor / coroutine topology | **LOCKED** (3 actors, ai-worker reserved, 2026-05-16) |
| 1 | 5 | Module layout & load order | **LOCKED** (flat dir, require, module-prefix, 2026-05-16) |
| 2 | 6 | Tick & frame model | **LOCKED** (adaptive: idle 2 Hz, animating 30 Hz, 2026-05-16) |
| 2 | 7 | Input pipeline | **LOCKED** (input-actor parses, game-actor mode-dispatches, 10 modes, 2026-05-16) |
| 2 | 8 | Render pipeline | **LOCKED** (full back-buffer rebuild per frame, screen-render diff, 2026-05-16) |
| 2 | 9 | RNG threading & determinism | **LOCKED** (per-system slots, bit-exact replay, 2026-05-16) |
| 3 | 10 | Save-game format | **LOCKED** (custom .bws — game-state Dict via to-binary-token, 2026-05-16) |
| 3 | 11 | Self-test & invariant harness | **LOCKED** (CLI-controlled, 3 run modes, scope tiers, 2026-05-16) |
| 3 | 12 | Performance budgets | **LOCKED** (CI gates with 10% tolerance, prod-build only, 2026-05-16) |
| 3 | 13 | CLI & debug surface | **LOCKED** (post-filename flags + /config Dict, 2026-05-16) |
| 3 | 14 | Modding hooks | **LOCKED** (scenarios only; GDD §21 dispositive, 2026-05-16) |
| 4 | 15 | Cross-platform terminal support | **LOCKED** (Linux+macOS primary; Windows best-effort, 2026-05-16) |
| 4 | 16 | Long-session memory & GC cadence | **LOCKED** (GC at scenario boundaries + /VMFull recovery only, 2026-05-16) |
| 4 | 17 | Crash recovery & autosave | **LOCKED** (write crash.bws + exit; no auto-resume, 2026-05-16) |
| -- | A | Prototype-derived perf numbers (frozen) | LOCKED |
| -- | B | Open decisions log | OPEN |

---

## Tier 1 — Load-bearing architectural locks

### 1. Save/restore discipline — **LOCKED (B'), 2026-05-15**

**Decision:** **Option B' — global-VM state with mutate-at-sl=0 and
scratch-only save brackets.**  Reachable because GDD §18.6 defers
undo-before-commit to v2.

**The model:**

```trix
% init (sl=0): build persistent state in global VM
${ ...allocate map, unit roster, faction state... } /game-state def

% main loop (sl=0):
{
    save                                  % sl 0 -> 1, barrier rises
        render-frame
        read-input            -> command
        validate command
        compute reachable / preview / animations    % all scratch in local VM
        if ai-turn? { ai-think }                    % scratch in local VM
    restore                               % sl 1 -> 0, scratch reclaimed

    % apply mutations at sl=0; in-place put on global Dicts/Arrays/Records
    % does NOT journal at sl=0, so changes are committed immediately.
    apply-action
    advance-turn
    check-end-condition
} loop
```

**Why B' over A/B/C:**

|  | A: no brackets | B: brackets + persist | C: nested | **B'** |
| --- | --- | --- | --- | --- |
| Scratch reclamation | ❌ leaks indefinitely | ✅ on `restore` | ✅ on `restore` | ✅ on `restore` |
| Persist-family ops needed | n/a | ❌ every state write | ❌ every state write | ✅ **none** |
| Per-turn undo | ❌ needs explicit copy | ✅ free | ✅ free | ❌ deferred to v2 |
| Save-level book-keeping | none | heavy | heaviest | **trivial** |

GDD §18.6 says undo is a v2 feature, so option B/C's main advantage
isn't actually a Phase-1 requirement.  B' captures everything else
B/C offer (scratch reclamation, clean separation of computed vs.
committed) without the persist-family op discipline tax.

**The four-quadrant test, resolved:**

| Container | Mutates? | Survives restore needed? | Allocation |
| --- | --- | --- | --- |
| Map terrain | no (write-once) | yes | global VM `${...}` |
| Unit roster Dict | yes | yes | global VM, mutated at sl=0 |
| Per-unit Dicts | yes (hp, vet, pos...) | yes | global VM, mutated at sl=0 |
| Faction state | yes (cash, power, score) | yes | global VM, mutated at sl=0 |
| Fog visibility | yes (each turn) | yes | global VM, mutated at sl=0 |
| Capture state | yes | yes | embedded in cell or unit, sl=0 |
| Reachable-hex sets | yes (per call) | no | local VM, dies on `restore` |
| AI search scratch | yes | no | local VM, dies on `restore` |
| Pathfinding queues | yes | no | local VM, dies on `restore` |
| Render command list | yes | no | local VM, dies on `restore` |
| Damage-preview state | yes | no | local VM, dies on `restore` |
| Input parse buffers | yes | no | local VM, dies on `restore` |
| Animation timeline | yes | no | local VM, dies on `restore` |

**Invariants this locks:**

1. **No `def-persist` / `put-persist` / `array-store-persist` /
   `set-add-persist` / `set-remove-persist` / `update-persist` in
   gameplay code.**  If you find yourself reaching for one, you're
   mutating from inside the save bracket, which is the wrong place.
   Bracket-internal computations must be pure (or write only to
   local-VM scratch).
2. **All persistent containers allocated via `${...}` / `#$`.**  Map
   terrain, unit Dicts, faction state, fog grid: every one is global-VM
   at scenario init.
3. **Mutations happen between brackets, at sl=0.**  The bracket body
   is read-mostly; commit happens after `restore`.
4. **`set-global` flag is OFF during the bracket body.**  Scratch
   allocations must land in local VM so they die on `restore`.  Turn
   it on (or use explicit `#$` literals) only for the rare case of
   computing a survivor inside the bracket.
5. **One bracket per turn.**  No nested save brackets in gameplay code
   (animation, AI, etc. all share the turn-wide scratch).

**Coroutine bootstrap consequence:** `docs/trix-reference.md` §7.12's
"yield-once-per-child" pattern applies before the FIRST entry to the
loop body.  Spawn order: input-actor, render-actor, game-actor, then
N yields where N = actor count.  Without this, the first save bracket
hits `/invalid-restore` because child frame dicts are at sl=1.

**Re-opening route (v2 undo):** add an outer save bracket at turn
granularity and convert the gameplay mutations to persist-family ops.
Architecturally compatible — no restructure required, just a discipline
shift on N call sites (estimate: ~20-30 mutation points).

**Cross-refs:** GDD §15 (snapshot save-game), §18.6 (undo deferred),
§21 (out-of-scope); `docs/trix-reference.md` §7.12 (bootstrap),
§ Global VM Allocation; `proto-fog-perf.trx` (fog recompute cheap);
`feedback_def_shadows_systemdict.md`,
`feedback_trix_idioms_for_sim_code.md` (mutable specs > new specs).

---

### 2. Memory model & VM-size budgets

**What needs locking:**
- `--vm-size=N` per scenario size (tutorial / standard / epic).
- `--scratch-depth=N` if the default 128 is insufficient.
- Allocation policy: which containers go in global VM (`#$`), which
  stay local.

**Prototype data:**
- Each prototype ran on default 1 MB.
- `proto-mapgen.trx` Epic = 8.5 s on debug, 435 ms prod, 75×54×~32 bytes
  per cell ≈ 130 KiB just for terrain bytes.
- `proto-ai-v2-perf.trx` Standard = 591 ms with scratch arenas.
- Combined runtime (map + units + fog + AI scratch + render + actor
  mailboxes + save journal) is the open question.

**LOCKED (downstream of §1 B', 2026-05-15):** global-VM strategy is
**"mostly global, persistent state only."**  All long-lived state
(map, units, factions, fog) lives in global VM; all per-turn scratch
lives in local VM and dies on `restore`.

---

#### 2.1 Recommended sizes — **LOCKED 2026-05-16: soft warnings, not hard caps**

| Scenario class | Cells (w×h) | Units (max) | `--vm-size` | `--scratch-depth` |
| -------------- | ----------- | ----------- | ----------- | ----------------- |
| Tutorial       | ≤ 450       | ~20         | **2 MB**    | **128**           |
| Standard       | 451 – 2700  | ~80         | **4 MB**    | **256**           |
| Epic           | ≥ 2701      | ~160        | **8 MB**    | **512**           |

Recommendations, not hard caps.  The launcher does not refuse to
start with `--vm-size` below the recommended value.

**Behaviour when `--vm-size` is below recommended:**

1. `scenario.trx` computes scenario size class from `w × h` after
   parsing the file.
2. `config.trx` compares `--vm-size` (Trix's `vm-size` status field)
   to the class's recommended value.
3. If under-sized: emit a one-line stderr warning at scenario load:

   ```
   Bestagon: scenario is Epic class (4050 cells); recommended --vm-size=8M, current is 1M.
             Continuing — if the game runs out of memory, restart with: --vm-size=8M
   ```

4. Continue execution.  Trix will raise `/VMFull` if the global VM
   actually fills up; `main.trx`'s top-level error handler converts
   that into a clear "out of memory" message with the recovery
   `--vm-size=` value (§17 crash recovery).

**Why soft warnings instead of hard caps:**

- Players running prototypes / small scenarios on tight VMs (e.g.
  testing on a Raspberry Pi) should be able to try.
- `--vm-size` is a Trix CLI flag consumed BEFORE Bestagon code runs;
  Bestagon can't bump it at runtime, only warn.
- Hard error-on-load forces the user to relaunch — annoying when
  most of the time their cushion is fine.
- CI hardness lives in §12.4's memory gates, not in user enforcement.

**No upper cap.**  `--vm-size=64M` for Tutorial is wasteful but
harmless; no warning.

---

#### 2.2 `--scratch-depth` recommendations

Same shape: soft warning if below recommended; no upper cap.

`--scratch-depth` is per-coroutine.  With 3 actors plus the main
thread, total scratch memory ≈ 4 × scratch-depth × 8 bytes — under
20 KiB even at 512.  The default 128 will likely be enough for
Tutorial; Standard / Epic AI scratch loops are the cost-drivers
(per `proto-ai-v2-perf.trx`).

---

#### 2.3 Prototype data (record of derivation)

- Each Phase 0.6/0.7 prototype ran on default 1 MB.
- `proto-mapgen.trx` Epic = 8.5 s debug / 435 ms prod;
  75 × 54 × ~32 B per cell ≈ 130 KiB for terrain bytes alone.
- `proto-ai-v2-perf.trx` Standard = 591 ms with scratch arenas.
- Combined runtime memory peak (map + units + fog + AI scratch +
  render + actor mailboxes + save journal) was not measured by any
  single prototype.  §12.4 gates assert the combined ceiling
  empirically once Phase 1 integration runs the actual game.

If Phase 1 measurement reveals the recommended sizes are wrong:
either tighten the gates (§12.4) or raise the recommendations
(this table).  Both require a same-PR doc update.

---

### 3. State representation — **LOCKED, 2026-05-15**

**Allocation site (downstream of §1 B'):** everything in this section
is allocated in **global VM** at scenario init (`${...}` / `#$`) and
mutated in place at sl=0.  No persist-family ops.

---

#### 3.1 Map cells — **LOCKED: A-SoA (struct-of-arrays of `Packed<Byte>`)**

Per-cell fields (GDD §17): terrain-kind (1B), owner (1B), unit-id (1B
sentinel 255=none, max 254 units system-wide per GDD §3 "Unit count
cap" + locked decision 40), fog-state (1B), capture-progress (1B),
capturer-id (1B, 255=none).

**Representation:** one `Packed<Byte>` per field, each of length `w·h`.
Cell index is `r * w + q` (row-major, flat-top odd-q).

**Why A-SoA over A-AoS (one flat byte array) or C (Packed<Record>):**

|  | A-AoS (`Packed<Byte>` of `w·h·K`) | **A-SoA (K × `Packed<Byte>`)** | C (`Packed<Record>`) |
| --- | --- | --- | --- |
| Epic memory | ~32 KB | ~32 KB | ~190 KB |
| Hot-loop scan touching 1-2 fields | strides over irrelevant bytes | **cache-optimal — only relevant grids loaded** | pointer-chase per cell |
| Named field access | manual index math everywhere | one named grid per field | `.field` sugar |
| Schema flexibility | edit index math across modules | add a new grid | add a Record field |
| Per-block GC walks | 1 block | K blocks (still tiny) | w·h blocks |
| Prototype precedent | n/a | **`proto-mapgen.trx` already uses this** | n/a |

Bestagon's hot loops touch 1-2 fields, not all of them:
- Fog compute: terrain + unit-id only (proto: 9.8 ms Epic).
- Pathfind BFS: terrain (+ occasional owner).
- Capture tick: capture-progress + capturer-id.
- Render: full set (but amortized over 33 ms frame).

CS-first-principles call per
`feedback_dont_defer_predictable_optimization.md` — locality win is
predictable; don't gate on "benchmark first."

**Allocation shape:**

```trix
${
    << /w 75
       /h 54
       /terrain          75 54 mul 0b  make-packed-array
       /owner            75 54 mul 255b make-packed-array
       /unit-id          75 54 mul 255b make-packed-array
       /fog              75 54 mul 0b  make-packed-array
       /capture-progress 75 54 mul 0b  make-packed-array
       /capturer-id      75 54 mul 255b make-packed-array
    >>
} /map def
```

**Sentinel values:** owner / unit-id / capturer-id use `255b` for
"none" / "neutral" / "no-capturer."  This keeps the field a single
unsigned byte and reserves the high value as a non-id sentinel.  All
other fields use `0b` as their natural zero.

**Wrap behind named accessors** so SoA-ness doesn't leak:

```trix
/cell-idx { |q r| $[ r * map.w + q ] } def
/cell-terrain        { |q r| map.terrain  q r cell-idx get } def
/cell-set-terrain    { |q r v| map.terrain  q r cell-idx v put } def
/cell-unit-id        { |q r| map.unit-id  q r cell-idx get } def
/cell-set-unit-id    { |q r v| map.unit-id  q r cell-idx v put } def
% ... one get/set pair per field, in map.trx
```

Field accessors live in `map.trx`; no other module touches grids
directly.

**Future extension:** if a new field exceeds 1 byte (e.g. unit-id
needs >254 ids, or capture-progress needs a wider range), promote
that one grid to `Packed<UShort>` / `Packed<UInteger>` / `Packed<Long>`
independently of the others.  AoS would force the whole array to
widen.  For the unit-id case specifically, the full widening
checklist (5 sites total — none in gameplay code) is in **GDD §3
"Path to widening"** under the Unit count cap subsection.

---

#### 3.2 Units — **LOCKED: global `/units` Dict + `/faction-units` index**

Per-unit fields: kind, owner, pos=[q r] (or `/q` and `/r` separately
to avoid a sub-Array alloc), hp, vet, fuel, ammo, has-moved?,
has-attacked?, transport-cargo.

**Storage:**

```trix
${
    16 dict                                          % /units Dict (unit-id -> unit Dict)
} /units def

${
    4 dict                                           % /faction-units Dict (faction-id -> Array<unit-id>)
        0b ${ 64 array }#$ put
        1b ${ 64 array }#$ put
        2b ${ 64 array }#$ put
        3b ${ 64 array }#$ put
} /faction-units def
```

Each unit Dict is allocated in global VM at spawn time, keyed by
`unit-id` (Byte, 0-254; 255 reserved as "none" sentinel in cell grids).

**Indexes maintained in lockstep at sl=0:**

| Query                | Path                         | Cost             |
| -------------------- | ---------------------------- | ---------------- |
| cell → unit-id       | `/unit-id` grid (§3.1)       | O(1)             |
| unit-id → unit Dict  | `/units uid get`             | O(1) Dict lookup |
| unit → cell          | `unit /q get`, `unit /r get` | O(1)             |
| faction → unit list  | `/faction-units fid get`     | O(1) → Array     |
| all units of faction | iterate the Array            | O(N_faction)     |

**Mutation discipline.**  All four indexes update together at sl=0
between save brackets:

- **Spawn:** allocate a fresh unit-id from `/next-unit-id` (incremented
  monotonically; reused after a unit-id is freed by kill).  If no
  free id is available because `/units` already contains 254 live
  entries, raise `/unit-cap-exceeded` (system-wide cap, GDD §3 +
  decision 40); allocate unit Dict in global VM; `put` into
  `/units`; push unit-id onto `/faction-units fid get`; write
  unit-id into `/unit-id` grid at the unit's cell.
- **Move:** clear the old cell's `/unit-id` grid slot (write `255b`),
  write new cell's slot, update `unit /q /r`.
- **Kill:** clear the unit-id grid slot; remove unit-id from
  `/faction-units` Array; `remove` from `/units` Dict (freeing the
  id for re-use by a future spawn); GC reclaims the unit Dict on
  next `vm-global-gc`.

A `unit-spawn` / `unit-move` / `unit-kill` op in `unit.trx`
encapsulates the four-index update.  No caller touches the indexes
directly.

**Cap-exceeded UX:** when the player issues a Build command and the
faction has funds but the cap is full, the build menu surfaces a
non-modal "unit cap reached (254)" indicator; the build doesn't
fire and funds aren't spent.  Practical scenarios reach this only
on pathological maps; canonical Epic 4-faction games top out
around 240 live units (GDD §3 capacity table).

**Why not a faction-keyed Array of unit Dicts (option originally
sketched in the TDD draft)?** Three reasons:
1. Cell-grid lookup needs a global unit-id space; two-level lookup
   (cell → faction-id + slot-index → unit) is slower.
2. Transport-cargo and "killed by faction X" mechanics want a flat
   unit-id namespace.
3. Faction reassignment (capture flips owner of a building, not of a
   unit, but the principle generalises) is cheap in the flat scheme.

---

#### 3.3 Factions — **LOCKED: flat global Dict (SoA), 2026-05-15; field set expanded 2026-05-16**

```trix
${
    << /count        2b                                                       % active faction count

       % Identity + control
       /color        ${ 4 array  /none /none /none /none arrayify }           % per-faction §A.4 palette name
       /co           ${ 4 array  /none /none /none /none arrayify }           % CO name from §8.1 roster
       /controller   ${ 4 array  /none /none /none /none arrayify }           % /human|/ai-v1|/ai-v2|/ai-v3
       /ai-difficulty ${ 4 array /none /none /none /none arrayify }           % /easy|/normal|/hard|/brutal|/none

       % Economy
       /funds                  ${ 4 array  0 0 0 0 arrayify }                 % current balance
       /cumulative-funds-earned ${ 4 array 0 0 0 0 arrayify }                 % lifetime; §16.3 turn-limit-funds

       % CO Power meter (§9.1)
       /power-meter-stars  ${ 4 array  0.0 0.0 0.0 0.0 arrayify }             % Real, 0.0..12.0
       /power-active       ${ 4 array  false false false false arrayify }
       /super-active       ${ 4 array  false false false false arrayify }
       /power-used         ${ 4 array  false false false false arrayify }

       % Running totals (display + replay export + win-cond inputs)
       /units-built        ${ 4 array  0 0 0 0 arrayify }
       /units-killed       ${ 4 array  0 0 0 0 arrayify }
       /units-lost         ${ 4 array  0 0 0 0 arrayify }
       /buildings-captured ${ 4 array  0 0 0 0 arrayify }
       /buildings-lost     ${ 4 array  0 0 0 0 arrayify }
       /total-damage-dealt ${ 4 array  0 0 0 0 arrayify }
       /total-damage-taken ${ 4 array  0 0 0 0 arrayify }

       % Elimination flag
       /defeated?          ${ 4 array  false false false false arrayify }     % §10.4
    >>
} /factions def
```

Per-field global arrays of length `MaxFactions` (4 to start; bumpable
without ABI change since it's data not Record schema).  `factions.count`
holds the active count.  Cleaner than a Dict-per-faction for the same
SoA reasons as cells: most fields are scalar, no per-faction Dict
allocation, and field-by-field iteration is cache-friendly.

**Field name alignment with GDD §17.2:** all field names match the
GDD logical schema verbatim.  Earlier drafts used `/cash`, `/co-kind`,
`/co-power` — renamed 2026-05-16 to `/funds`, `/co`,
`/power-meter-stars` to eliminate two specs for one concept.

**Accessor convention:**

```trix
/faction-funds          { |fid|   factions /funds get fid get } def
/faction-set-funds      { |fid v| factions /funds get fid v put } def
/faction-power-active?  { |fid|   factions /power-active get fid get } def
% ... one get/set pair per field, in faction.trx
```

Field accessors live in `faction.trx`; no other module touches the
SoA arrays directly.

**Memory cost** (4 factions): ~18 fields × 4 Objects × 8 bytes
= ~580 bytes for the entire `/factions` substate, plus header.

---

#### 3.4 Buildings — **LOCKED: cell-grid storage + per-faction index, 2026-05-16**

Buildings are not separate entities.  GDD §17.3 defines them
logically; the implementation stores per-building state in the
**cell grids** of §3.1 and maintains a per-faction index for
gameplay iteration.

**Per-building state — already in §3.1 cell grids:**

| Logical field (GDD §17.3) | Physical storage (§3.1 grid)                                         |
| ------------------------- | -------------------------------------------------------------------- |
| `/type`                   | `/terrain` (one of the building-type names)                          |
| `/pos`                    | implicit in the grid index                                           |
| `/owner`                  | `/owner` (-1 = neutral, 255b sentinel reserved as cross-grid "none") |
| `/capture-progress`       | `/capture-progress` (0-20; 0 = not being captured)                   |
| `/capturer-unit-id`       | `/capturer-id` (255b = none)                                         |

**Faction-buildings index** — O(1) "which buildings does faction F
own?" lookup for end-of-turn income, fog reveal, capture-victory
check, and AI scoring:

```trix
${
    4 dict
        0b ${ 32 array }#$ put
        1b ${ 32 array }#$ put
        2b ${ 32 array }#$ put
        3b ${ 32 array }#$ put
} /faction-buildings def
```

Each Array holds cell-indexes (`r * w + q`) of building cells the
faction currently owns.  Index size capped at 32 per faction (well
above the dense-Epic preset's ~24 buildings per faction).

**Mutation discipline.**  Building state changes go through
`building.trx` chokepoint ops:

- **`building-capture`** (faction flips on `/capture-progress`
  reaching 20): write new `/owner` into cell grid; append cell-idx
  to new owner's `/faction-buildings` Array; remove cell-idx from
  old owner's Array (no-op if neutral); zero `/capture-progress`
  + `/capturer-id`.
- **`building-tick-capture`**: update cell grids; no index update.

`validate-state` confirms that for each owned cell `c`, `c`'s
`/owner` value `f` satisfies `c ∈ /faction-buildings[f]`.

**Why not an Array of Building Dicts?** Three reasons (parallel to
§3.2's reasoning for units):
1. Capture mechanics already touch per-cell state (range checks,
   adjacency).  Folding the building state into cell grids removes a
   second representation.
2. The `/faction-buildings` index makes faction → buildings O(1).
3. No allocation churn on capture — flip a byte in the `/owner` grid
   and update the two index Arrays.

---

#### 3.5 Top-level state slots — **LOCKED, 2026-05-16**

GDD §17.1's logical "game state" Dict is implemented as a small set
of **separate global slots**, not one big Dict.  This keeps
`/map`, `/units`, `/factions` independently addressable (cleaner
accessor layering, smaller save-Dict construction in §10.3) and
matches the locked decision 32-A2 ("constants as Records, mutable
state in global VM at sl=0").

| Logical (GDD §17.1) | Slot | Type | Allocation |  |
| --- | --- | --- | --- | --- |
| `/turn` | `/turn-number` | UInteger | scalar |  |
|  | `/day` | UInteger | scalar |  |
|  | `/current-faction` | Byte | scalar |  |
|  | `/turn-phase` | Name | scalar |  |
| `/game-status` | `/game-status` | Name | scalar |  |
|  | `/winner-id` | Byte or 255b sentinel | scalar |  |
| `/win-conditions` | `/win-conditions` | Array of (Name | Tagged) | rebuilt on load via scenario re-run (§10.4) |
| `/events.transient` | `/event-ring` | Array (ring of 20) | global VM |  |
| `/scenario-overrides` | `/scenario-overrides` | Dict | global; loaded once at scenario init |  |
| `/settings` | `/settings` | Dict | from CLI parse (§13) |  |
| `/next-unit-id` | `/next-unit-id` | Byte | scalar |  |
| `/next-building-id` | `/next-building-id` | (unused in cell-grid model — buildings have no separate id) |  |  |
| `/weather-*` | `/weather`, `/weather-cycle-idx`, `/weather-day-last-cycled` | Name + UInteger × 2 | scalars (§11.5) |  |
| `/scenario-name` | `/scenario-name` | String | from scenario header (§16) |  |
| RNG slots | `/combat-rng-state`, `/mapgen-rng-state`, `/ai-rng-state`, `/cosmetic-rng-state`, `/event-rng-state` | ULong × 5 | scalars (§9.3) |  |

Each slot is bound via `def-persist` (or `def` for write-once
scenario constants) at scenario init.  The save body (§10.2)
serialises only the persistent subset; transient slots (event ring,
local-VM scratch) are excluded.

`/event-ring` is the only non-scalar persistent state outside the
big four (`/map`, `/units`, `/faction-units`, `/factions`,
`/faction-buildings`).  v1 deferred decision (2026-05-16): event
ring is **not** persisted in the save body — events are transient
display state.  Replay export (`.bwr`, v2) will add a persistent
event log.

---

**Cross-refs:** GDD §17 (logical schema; this section is the
authoritative impl); §10 (save body schema lists which slots
serialise; §10.3 save-game proc builds the body Dict from these
slots); §11 (fog touches `/terrain` + `/unit-id` grids); §9.3 (RNG
slot lifecycle);
`feedback_dict_value_auto_exec.md` (Dict gotcha — values stored as
`{ a b c }` auto-exec on lookup; use `[...]` for data tables);
`feedback_trix_idioms_for_sim_code.md` (mutable specs > new specs);
`feedback_dont_defer_predictable_optimization.md` (CS-first-principles
locality win, no benchmark gate).

---

### 4. Actor / coroutine topology — **LOCKED, 2026-05-16**

**Decision:** **3 actors — `input-actor`, `render-actor`, `game-actor`.**
The ai-worker case is deferred but explicitly NOT designed out: §4.4
enumerates the v1 affordances that keep promotion to a 4-actor
topology a config change, not a rewrite.

---

#### 4.1 The three actors

| Actor | Body | Mailbox cap | Why an actor (vs. inline proc) |
| --- | --- | --- | --- |
| **input-actor** | `read-key-byte` loop → parse key → `actor-send` command to game-actor | 4 | `read-key-byte` blocks; needs its own yield point |
| **render-actor** | `actor-recv` loop → diff/redraw via screen ops | 4 | Decouples paint cadence from game-tick; game-actor signals "redraw" and continues |
| **game-actor** | main loop: `actor-recv` → bounded compute → mutate state at sl=0 → signal render | 16 | State owner; runs GDD §2 turn loop; drives AI inline on AI turns via **budget-aware cooperative `ai-think`** (§4.4 affordance 8) — periodic `/ai-progress` actor-sends keep input + render responsive |

**Deferred but not designed out** (v2 / "if measured"):

| Actor | Why deferred | Promotion cost |
| --- | --- | --- |
| `ai-worker` | UX-acceptable to block on AI turn (Advance Wars / Wargroove model); but §4.4 keeps the upgrade cheap | spawn + 1 yield + message-dispatch case |
| `fog-worker` | Fog ≈ 9.8 ms (`proto-fog-perf.trx`) — inline at turn boundary | unlikely to ever need |
| `autosave-worker` | Snapshot must run at sl=0 between brackets; game-actor is already there | unlikely to ever need |
| `animation-worker` | Animations are scratch (§1 B'); inside game-actor's bracket | unlikely to ever need |
| Network / sound | Out of scope per GDD §21 | N/A |

---

#### 4.2 Message flow (v1)

Pure one-way fan-in:

```
   ┌──────────────┐    /key ...        ┌──────────────────┐
   │ input-actor  │ ────────────────▶  │                  │
   └──────────────┘                    │   game-actor     │
                                       │  (main loop,     │
   ┌──────────────┐    /redraw ...     │   state owner)   │
   │ render-actor │ ◀────────────────  │                  │
   └──────────────┘                    └──────────────────┘
```

No back-edges to input or render in v1.  Quit propagates game → render
→ input on shutdown (§4.5).

**Message catalog** (extensible — case dispatch in game-actor):

| Tag | Sender | Receiver | Payload |
| --- | --- | --- | --- |
| `/key` | input-actor | game-actor | `{ /char Byte /mods Byte }`#$ |
| `/redraw-all` | game-actor | render-actor | (none — pull state from globals) |
| `/redraw-region` | game-actor | render-actor | `{ /panel Name /q /r /w /h }`#$ |
| `/ai-progress` | game-actor | render-actor | `{ /faction /elapsed-ms /mode /candidates-evaluated /current-best-score }`#$ — emitted every ~100 ms by budget-aware `ai-think` (§4.4 affordance 8); drives the GDD §14.8 "Thinking..." spinner + soft/hard-cap indicators |
| `/quit` | game-actor | render-actor | (none) |
| `/quit` | input-actor | game-actor | (none) |
| `/ai-result` | (reserved for v2 ai-worker) | game-actor | `{ /faction /action-plan }`#$ |
| `/ai-request` | (reserved for v2 game-actor → ai-worker) | ai-worker | `{ /faction /snapshot-ref }`#$ |

---

#### 4.3 Cross-actor message discipline

**Rule:** every `actor-send` payload must be allocated in global VM
(`[...]#$` / `${...}` / `#$` literals).

When game-actor is inside its `save` bracket and yields (`actor-recv`),
other actors run at sl=1 globally.  Local-VM allocations they `actor-send`
become dangling references on game-actor's `restore`.
`Save::reject_local_into_global` catches the obvious cases but
defensive discipline beats a runtime check.

Established Trix idiom — MEMORY.md "Phase 3 mailbox migration to global
VM plus `[...]#$ actor-send` pattern."  Locking it explicit for Bestagon.

---

#### 4.4 ai-worker promotion affordances (v1 design constraints)

The v1 inline-AI design **must** preserve these affordances so that
v2 promotion is a localised change, not a refactor:

1. **`ai-think` is a pure function with a clean signature.**

   ```trix
   /ai-think { |faction-id tier| ...; produces action-plan ... } def
   ```

   Signature: `faction-id tier -- action-plan`.  Reads from global-VM
   state (`/map`, `/units`, `/factions`) only.  No closure over
   game-actor's local frame.  No screen / input access (except via
   the `/ai-progress` actor-send pattern in affordance 8).  Lives in
   `ai_v1.trx` (Phase 2) / `ai_v2.trx` (later); identical signature
   across AI versions.

2. **`action-plan` is a global-VM data structure.**  An ordered Array
   of action Records (move / attack / capture / build / end-turn).
   Allocated via `${...}` so it survives both inline scratch reclaim
   and (in v2) cross-actor message transport without re-allocation.

3. **Game-actor's AI dispatch is one call site.**  Whether v1 inline
   or v2 actor-forwarded, there's exactly one spot in game-actor's
   loop that says "it's an AI faction's turn; produce a plan."  v1:

   ```trix
   /plan faction-id tier ai-think def
   plan apply-action-plan
   ```

   v2 promotion:

   ```trix
   ${ << /faction faction-id /tier tier >> } ai-worker actor-send
   { actor-recv /msg local-def
     msg tag-value /ai-result eq? ... }
     until-true
   plan apply-action-plan
   ```

   Same `apply-action-plan` step; only the plan acquisition changes.

4. **Message dispatcher is tag-based with `case`.**  Adding `/ai-result`
   in v2 is one new `case` branch — not a new dispatcher.  v1
   dispatcher must be coded in this shape from day one (no
   if/else cascade on tag).

5. **AI RNG seed is per-system** (locks ahead into §9 RNG).  AI seed is
   derived from `hash(master-seed, /ai, turn-number)` so AI is
   deterministic regardless of inline-vs-actor execution.  Without
   this, v2 ai-worker introduces scheduling nondeterminism and breaks
   replay.

6. **Mailbox cap of 16 on game-actor** already accommodates the v2
   case (occasional `/ai-result` interleaved with player keys).

7. **AI must not depend on game-actor-local scratch.**  All inputs are
   read fresh from global-VM state at the start of `ai-think`.  In v1
   game-actor's save bracket reclaims AI scratch on restore; in v2
   ai-worker's own save bracket does the same.  `ai-think` itself
   does not manage save/restore — the caller does.

8. **`ai-think` is budget-aware and cooperatively yielding.**  This is
   what makes the inline v1 model work without breaking input + render
   responsiveness, and what implements the GDD §14.4 / §14.5 cap
   semantics:

   - `ai-think` takes the player-selected tier (`/standard` or
     `/patient`) and reads the per-tier `target` / `soft-cap` /
     `hard-cap` wall-clock thresholds from the §14.4 locked table.
   - At each candidate-evaluation iteration the loop checks elapsed
     wall-clock.  When elapsed exceeds **soft-cap** the loop flips
     into Top-K prefilter mode (heuristic pre-score; only top
     K = 20 candidates get full eval).  When elapsed exceeds
     **hard-cap** the loop returns the current best-scored plan
     immediately and emits an `/ai-stalled` flag in the next
     `/ai-progress` message.
   - Every ~100 ms (or every `N` candidates, whichever fires first)
     the loop emits an `/ai-progress` message to render-actor.  This
     `actor-send` is itself a yield point in Trix's coroutine
     scheduler, so input-actor and render-actor both get scheduled
     between progress emissions.  Net effect: GDD §14.8's
     "Thinking…" spinner and elapsed counter animate during AI
     turns even though the AI is running synchronously inline.
   - `coroutine-sleep 0` is permitted as an additional explicit yield
     between work batches if the actor-send cadence is too coarse
     for the UI.

   This affordance is what makes the GDD §14.5 cap-behavior table
   (Target → Soft cap → Hard cap with mode switches and abort
   semantics) implementable inline.  Without it, the GDD's
   interruptible-budget contract would force the v2 ai-worker
   topology immediately.

**Inline path (v1):**

```trix
% inside game-actor loop, at sl=0
config /ai-perf-tier get /tier local-def      % /standard or /patient
save
    /plan faction-id tier ai-think def        % AI scratch in local VM at sl=1
                                              % ai-think internally:
                                              %  - tracks elapsed wall-clock
                                              %  - emits /ai-progress every ~100 ms
                                              %  - switches to Top-K at soft-cap
                                              %  - returns current-best at hard-cap
restore                                       % AI scratch reclaimed
plan apply-action-plan                        % plan was in global VM, survives
```

`ai-think` signature evolves to take the tier:

```trix
/ai-think { |faction-id tier|
    ${ 0 array } /candidates local-def
    ${ << /score -inf-real /plan null >> } /best local-def
    now /t-start local-def
    tier-thresholds tier /target /soft /hard get-path  % three Reals: target soft hard (seconds)
    ...
    % loop over candidate plans, scoring + updating /best;
    % at each step check elapsed, switch mode, maybe emit /ai-progress
    ...
    best /plan get                            % current-best at any exit
} def
```

**Forwarded path (v2):** lifts the `ai-think` call into `ai-worker`'s
own body without changing the function, the tier-aware contract, the
`/ai-progress` emission cadence, or the apply step.  The only
difference is that in v2 the `actor-send` of `/ai-progress` happens
from the ai-worker coroutine rather than from game-actor.

---

#### 4.5 Bootstrap (LOCKED via §1 B')

```trix
% main thread (sl=0)
${ scr cfg }                                                 % shared init already done

[ actor-self ]#$         { input-body }   actor-spawn  /input-actor   def
[ actor-self ]#$         { render-body }  actor-spawn  /render-actor  def
[ actor-self scr ]#$     { game-body }    actor-spawn  /game-actor    def

0 coroutine-sleep    % yield #1 — input-actor parks on read-key-byte
0 coroutine-sleep    % yield #2 — render-actor parks on actor-recv
0 coroutine-sleep    % yield #3 — game-actor parks on actor-recv

game-actor coroutine-join pop pop      % main thread waits for game-actor exit
```

3 actors = 3 post-spawn yields.  When v2 promotes ai-worker, this
becomes 4 spawns + 4 yields (one extra line each).

---

#### 4.6 Termination flow

1. User presses `Q` (or `Ctrl-C` is trapped to same path).
2. input-actor sends `/quit` to game-actor.
3. game-actor finishes the current turn-bracket cleanly (no half-mutated
   state), sends `/quit` to render-actor.
4. render-actor drops alt-screen, exits raw mode, dies.
5. game-actor dies.
6. main thread wakes from `coroutine-join`, prints exit message, exits.
7. input-actor is killed by main thread on the way out (or detects
   render-actor's death via mailbox-closed signal and self-exits).

v2 ai-worker: game-actor sends `/quit` to ai-worker between steps 3
and 4; ai-worker drops any in-flight think and dies.

---

**Cross-refs:** `examples/tetrix.trx` (5-actor reference: main / input
/ gravity / ai-actor / ai-tick — Tetrix's ai-actor body is the model
for our v2 ai-worker promotion); GDD §13/§14 (AI v1/v2 design);
`feedback_trix_idioms_for_sim_code.md` (yield-once-per-child,
mailboxes in global VM); `docs/trix-reference.md` §7.12 (bootstrap).

---

### 5. Module layout & load order — **LOCKED, 2026-05-16**

#### 5.1 Directory structure

Flat layout under `examples/bestagon/` (matches existing
`examples/bestagon-proto/` / `-sim/` / `-scenarios/` companion-dir
convention; no shared code with other examples).

```
examples/bestagon/
    main.trx          # entry: CLI parse, init, spawn, bootstrap, join
    config.trx        # CLI flags, defaults, /config Dict
    rng.trx           # per-system RNG slots + begin/end (§9)
    coord.trx         # hex math (from proto-hex-math)
    map.trx           # SoA grids + cell accessors (§3.1)
    unit.trx          # /units + /faction-units + lifecycle (§3.2)
    faction.trx       # SoA faction state (§3.3)
    movement.trx      # reachable-hexes, ZOC, indirect (from proto-pathfind)
    combat.trx        # damage matrix + luck + vet + CO (combat-core.trx is reference)
    capture.trx       # capture state machine (from proto-capture)
    fog.trx           # bbox fog (from proto-fog-perf)
    scenario.trx      # scenario parser + validator (from proto-loader)
    mapgen.trx        # procedural maps (from proto-mapgen)
    render.trx        # screen ops, viewport, minimap, panels (from proto-screen-wireframe)
    input.trx         # raw-mode key reader + command parse
    save.trx          # snapshot save/load (depends on §10)
    ai_v1.trx         # greedy heuristic (Phase 2)
    ai_v2.trx         # logic-programmed (Phase 3+, optional)
    selftest.trx      # §19 invariants harness + --replay-determinism-check (§9.6)
    debug.trx         # --reveal-map, set-hp, etc. (gated)
```

20 files.  Entry point: `examples/bestagon/main.trx` (users invoke
`./trix examples/bestagon/main.trx -- --scenario=<name>` directly;
no top-level wrapper).

Actor bodies (`game-actor-body`, `input-actor-body`,
`render-actor-body`) live in `main.trx` — they are short, only
referenced once each at spawn time, and share local context with
the bootstrap code.  Following Tetrix precedent.

#### 5.2 Load mechanism — `require` only

Every cross-module load uses `(examples/bestagon/<module>.trx)
require` (idempotent; canonical-path cached).  No `run`.  Conditional
modules (`selftest.trx`, `debug.trx`, `ai_v2.trx`) are required
behind a `--flag` check in `config.trx` — `require` is still safe
because the path-cache key is the canonical path.

Path convention is **always full from project root**:

```trix
(examples/bestagon/coord.trx) require
```

Reasons: `require`'s realpath canonicalization picks up the cwd at
invocation; the explicit-full-path form is robust to running from any
cwd, and grep-able.

#### 5.3 Naming convention — module-prefix on every name

All module-defined ops, slots, and Records named
`<module>-<verb>-<noun>` (or `<module>-<noun>-<verb>?` for predicates):

| Module | Prefix | Examples |
| --- | --- | --- |
| `map.trx` | `map-` | `map-cell-terrain`, `map-cell-set-terrain`, `map-in-bounds?` |
| `unit.trx` | `unit-` | `unit-spawn`, `unit-move`, `unit-kill`, `unit-at-cell`, `unit-alive?`, `unit-faction-flat-map` (per GDD §14.1) |
| `faction.trx` | `faction-` | `faction-funds`, `faction-power-meter-stars`, `faction-defeated?` |
| `movement.trx` | `move-` | `move-reachable-hexes` (signature in GDD §14.1), `move-cost-onto`, `move-zoc?` |
| `combat.trx` | `combat-` | `combat-resolve-attack`, `combat-predict-damage`, `combat-threat-map` (GDD §14.1/§14.2; shared with AI v3) |
| `capture.trx` | `capture-` | `capture-tick`, `capture-interrupt`, `capture-progress` |
| `fog.trx` | `fog-` | `fog-recompute`, `fog-visible?` |
| `mapgen.trx` | `mapgen-` | `mapgen-generate-map`, `mapgen-default-options` |
| `scenario.trx` | `scen-` | `scen-load`, `scen-validate`, `scen-parse` |
| `render.trx` | `render-` | `render-frame`, `render-side-panel`, `render-combat-log`, `render-minimap` |
| `input.trx` | `input-` | `input-parse-key`, `input-dispatch` |
| `save.trx` | `save-` | `save-snapshot`, `save-load` |
| `ai_v1.trx` / `ai_v2.trx` | `ai-` | `ai-think` (same signature, see §4.4) |
| `rng.trx` | `<system>-rng-` | `combat-rng-begin`, `combat-rng-end`, `ai-rng-begin`, ... |
| `coord.trx` | `hex-` | `hex-distance`, `hex-neighbours`, `hex-to-cube` |
| `selftest.trx` | `selftest-` | `selftest-run`, `selftest-check-capture-invariant` |
| `debug.trx` | `debug-` | `debug-reveal-map`, `debug-set-hp` |
| `config.trx` | `config-` | `config-vm-size`, `config-headless?` |

**Why prefix:** at 20 modules and several hundred ops, a single global
namespace without discipline creates collision risk and obscures
ownership.  Prefixes also keep grep-from-call-site cheap (`grep -n
map-cell-` finds every map-cell operation).  Standard Trix
hyphenation already in the codebase.

**Persistent state slots** follow the same convention: `/map`,
`/units`, `/faction-units`, `/factions`, `/config`,
`/combat-rng-state`, `/mapgen-rng-state`, `/ai-rng-state`,
`/master-seed`.  Top-level (un-prefixed) names are reserved for
singletons of these kinds.

**Exception:** `hex-*` for coord ops rather than `coord-*`, matching
the codebase's existing `hex-to-int`, `mask-to-team`,
`value-to-string` `-to-` idiom.

#### 5.4 Dependency graph and load order

```
config  rng  coord
  │      │     │
  └──┬───┘     │
     │         ├──────────────┐
     │   ┌─────┤              │
     │   │     │              │
     │   │     ▼              │
     │   │    map ◀─────┐     │
     │   │     │        │     │
     │   │     ▼        │     │
     │   │   unit       │     │
     │   │     │        │     │
     │   │     ▼        │     │
     │   │  faction     │     │
     │   │     │        │     │
     │   │     │        │     │
     │   ├──┬──┴────┬───┤     │
     │   │  │       │   │     │
     │   ▼  ▼       ▼   ▼     ▼
     │  movement   capture   fog
     │   │            │       │
     │   ▼            │       │
     │ combat ◀───────┘       │
     │   │                    │
     │   ▼                    │
     │ scenario ◀────────────┐│
     │   │                   ││
     │   ▼                   ▼▼
     │ mapgen ◀──── render  ◀── input
     │                       │
     ▼                       │
   save ◀───────────────────┘

   (then conditionally:  ai_v1 → ai_v2 → selftest → debug)
```

**Load order in `main.trx`:**

```trix
(examples/bestagon/config.trx)    require    % CLI flags first
(examples/bestagon/rng.trx)       require    % RNG slots before mapgen
(examples/bestagon/coord.trx)     require    % pure math, no deps
(examples/bestagon/map.trx)       require
(examples/bestagon/unit.trx)      require
(examples/bestagon/faction.trx)   require
(examples/bestagon/movement.trx)  require
(examples/bestagon/capture.trx)   require
(examples/bestagon/fog.trx)       require
(examples/bestagon/combat.trx)    require
(examples/bestagon/scenario.trx)  require
(examples/bestagon/mapgen.trx)    require
(examples/bestagon/render.trx)    require
(examples/bestagon/input.trx)     require
(examples/bestagon/save.trx)      require
(examples/bestagon/ai_v1.trx)     require    % Phase 2; require unconditionally once it exists

% Conditional loads
config-flag-ai-v2?  { (examples/bestagon/ai_v2.trx) require } if
config-flag-debug?  { (examples/bestagon/debug.trx)  require } if
config-flag-tests?  { (examples/bestagon/selftest.trx) require } if

% Then: parse scenario, allocate state, spawn 3 actors, yield 3 times, join.
```

#### 5.5 Module contract surface

Each module declares its **public surface** in a comment header at the
top of the file: every name a caller may use.  Anything else is
implementation detail and may be renamed without bumping a "contract
version."  Sketch shape:

```trix
% ====================================================================
% map.trx -- map grids (SoA) and cell accessors
%
% Public surface:
%   /map                              -- global Dict (alloc'd in main)
%   /map-cell-terrain      q r -- b
%   /map-cell-set-terrain  q r b --
%   /map-cell-owner        q r -- b
%   /map-cell-set-owner    q r b --
%   /map-cell-unit-id      q r -- b
%   /map-cell-set-unit-id  q r b --
%   /map-cell-fog          q r -- b
%   /map-cell-set-fog      q r b --
%   /map-in-bounds?        q r -- bool
%   /map-w                                 -- u
%   /map-h                                 -- u
%
% Depends on:  coord
% ====================================================================
```

Header is documentation only — not enforced by Trix.  Discipline rule:
new public names must be added to the header in the same commit; new
private helpers don't need to be.

---

**Cross-refs:** `examples/tetrix.trx` (single-file precedent + `require`
of `lib/screen.trx`); `docs/trix-reference.md` § `require` /
`require-module` (load semantics); §3 (data shape per module);
§4 (actor bodies live in main.trx); §9 (rng.trx surface);
`feedback_def_shadows_systemdict.md` (name-collision risk — prefixes
mitigate but still audit via `userdict ... systemdict known?` sweep
periodically).

---

## Tier 2 — Game-loop mechanics

### 6. Tick & frame model — **LOCKED, 2026-05-16**

**Decision:** **Adaptive cooperative — event-driven at idle, 30 Hz
during animation.**  Bestagon's screen is static between actions and
busy during them; the model matches that profile rather than paying
constant CPU.

---

#### 6.1 The model

render-actor's body has three states; transitions are message-driven.

```
            ┌─────────────────────────────────────┐
            │              IDLE                    │
            │  actor-recv-with-timeout(500ms)      │
            │  on timeout: cursor-blink-tick       │
            │  on msg: process; if anim queued     │
            │          → ANIMATING                 │
            └────────┬──────────────────────▲──────┘
                     │ anim queued           │ queue drained
                     ▼                       │
            ┌─────────────────────────────────────┐
            │            ANIMATING                 │
            │  drain mailbox (non-blocking)        │
            │  advance one animation tick          │
            │  render frame                        │
            │  coroutine-sleep 33                  │
            │  if /skip-animations:                │
            │      → FAST-FORWARD                  │
            └────────┬────────────────────────────┘
                     │ /skip-animations
                     ▼
            ┌─────────────────────────────────────┐
            │          FAST-FORWARD                │
            │  drain entire animation queue        │
            │  render final state                  │
            │  → IDLE                              │
            └─────────────────────────────────────┘
```

**Concretely:**

```trix
% render.trx — render-actor body sketch
/render-body { |main|
    /anim-queue 64 array  local-def           % allocated in main pre-spawn; passed in
    /cursor-phase 0u      local-def
    {
        anim-queue empty? {
            % IDLE: poll mailbox with timeout
            { actor-self actor-mailbox-empty? not }
              { actor-recv handle-msg }
              while
            cursor-phase 1u add 2u mod /cursor-phase store
            cursor-blink-tick
            500 coroutine-sleep
        } {
            % ANIMATING: 30 Hz frame loop
            { actor-self actor-mailbox-empty? not }
              { actor-recv handle-msg }
              while                            % drain any pending messages
            anim-queue advance-one-tick
            render-frame
            33 coroutine-sleep
        } if-else
    } loop
} def
```

Idle CPU: roughly 2 wakeups per second + a single-cell update.
Animation CPU: 30 frames per second of dirty-rect render (per §8).

---

#### 6.2 Frame timing

| Mode         | Sleep  | Effective FPS         | Notes                                      |
| ------------ | ------ | --------------------- | ------------------------------------------ |
| IDLE         | 500 ms | 2 (cursor blink only) | Single cell redraw per wake; near-zero CPU |
| ANIMATING    | 33 ms  | ~30                   | Full-frame dirty-rect render               |
| FAST-FORWARD | 0 ms   | one-shot              | Drain queue + final render                 |

**Frame budget while ANIMATING: 33 ms total per tick.**  Render must
complete inside ~16 ms to leave headroom for animation advance and
message handling.  Render strategy (full-redraw vs dirty-rect) is
§8's call but the dirty-rect proposal there fits inside this budget
on Epic-size maps.

**No 60 Hz mode.**  The game is turn-based with brief animations;
30 Hz is indistinguishable from 60 Hz to the eye for slides /
flashes, and halves the CPU.

---

#### 6.3 Animation catalog (v1)

| Animation | Duration | Ticks (30 Hz) | Triggered by |
| --- | --- | --- | --- |
| Cursor blink | 500 ms cycle | n/a (IDLE timer) | Idle render-actor |
| Unit move along path | 150 ms / hex | 4-5 / hex | `/animate-move` from game-actor |
| Attack flash + damage popup | 500 ms | 15 | `/animate-attack` |
| Death (fade / shrink) | 300 ms | 9 | `/animate-death` |
| Capture progress increment | 100 ms flash | 3 | `/animate-capture` |
| CO power activation (screen flash + banner) | 1000 ms | 30 | `/animate-co-power` |
| Turn-start banner slide | 750 ms | 22 | `/animate-turn-start` |
| Menu open / close | instant | 0 | (no animation in v1) |

All animations linear-interpolate; no easing curves (deferred).
Animation parameters tunable per-scenario (GDD §16.6
`/animation-speed` multiplier).

---

#### 6.4 Message protocol with game-actor

game-actor produces animations as side-effects of state mutations,
sending one or more `/animate-X` messages per tick to render-actor:

| Message | Payload | Source |
| --- | --- | --- |
| `/animate-move` | `{ /unit-id /from-q /from-r /to-q /to-r /path }` | apply-move |
| `/animate-attack` | `{ /atkr-id /defr-id /damage }` | apply-attack |
| `/animate-death` | `{ /unit-id /at-q /at-r }` | unit-kill |
| `/animate-capture` | `{ /unit-id /at-q /at-r /progress }` | capture-tick |
| `/animate-co-power` | `{ /faction-id /co-name }` | apply-co-power |
| `/animate-turn-start` | `{ /faction-id }` | advance-turn |
| `/redraw-region` | `{ /q /r /w /h }` | post-mutation hint |
| `/skip-animations` | (none) | game-actor on `/key` during animation |
| `/sync-animations` | (none — synchronous handshake) | game-actor pre-block-on-input |

**Game-actor does not block on animation completion** during its
mutation loop.  It fires the animation messages and continues to
the next mutation or end-of-turn.  Animations queue up in
render-actor; render plays them out at 30 Hz while game-actor
prepares the next turn / waits for input.

**Sync handshake:** before game-actor enters `actor-recv` for the
next player input, it sends `/sync-animations` and waits for
`/animations-done` from render-actor.  This ensures the player sees
the previous turn's outcome before being prompted.

**Skip:** if game-actor receives a `/key` while a `/sync-animations`
is pending, it forwards `/skip-animations` to render-actor.  Render
fast-forwards to final state.  Player input snaps to "now."

---

#### 6.5 Why not AI-think on a worker actor (re-affirms §4)

§4.4 reserved the ai-worker affordance but kept AI inline.  The
frame model confirms that decision:

- AI on AI's turn: 591 ms (Standard) / 864 ms (Epic) per
  `proto-ai-v2-perf.trx`.  During AI think, render-actor remains in
  IDLE mode (no animations queued by game-actor yet — they queue
  after `ai-think` returns).  Player sees a frozen frame for ~1
  second while AI thinks, matching every turn-based wargame's UX.
- If post-launch metrics show player-side "AI is too slow," promote
  per §4.4.  Frame model unchanged — render-actor still 30 Hz when
  animations queue; AI runs in parallel on its own actor.

---

#### 6.6 Sleep / timing primitives

| Op                       | Use                                          |
| ------------------------ | -------------------------------------------- |
| `coroutine-sleep N` (ms) | Frame pacing                                 |
| `clock` (microsec)       | Frame-time measurement (logging, perf gates) |
| `now` (ms)               | Wall-clock for animation timeline            |

No real-time-clock dependencies.  Animations are framecount-driven,
not wall-clock-driven, so a slow machine plays animations at fewer
frames-per-second rather than skipping frames — visually slower
but never desynced.  Acceptable for v1.

---

**Cross-refs:** GDD §2 (turn structure), §18 (animation, controls);
§4.1 (render-actor mailbox cap 4 — fine since game-actor sends at
most ~10 animations per turn before sync); §4.4 (ai-worker promotion
path); §8 (render strategy — dirty-rect proposal fits inside the
33ms budget); §11 (replay-determinism-check runs `--headless` with
no render-actor, so frame model doesn't affect replay).

---

### 7. Input pipeline — **LOCKED, 2026-05-16**

GDD §18.7 fixes the player-facing key list (WASD / HJKL / arrows for
cursor; Enter / Esc / Tab / Space / B / P).  This TDD section locks
the **implementation surface**: where parsing happens, what symbolic
keys flow between actors, what modes exist, and how mode dispatch
is wired.

---

#### 7.1 Two-layer pipeline

```
read-key-byte (raw)
       │
       ▼
   input-actor          parses CSI/SS3 escape sequences + Ctrl/Alt
       │                normalizes to symbolic Name keys
       │                emits one /key message per logical keystroke
       ▼
   game-actor           dispatches on (mode, key) → action proc
       │                mutates state at sl=0
       ▼
   render-actor         renders the new state
```

**input-actor stays dumb.**  It does NOT know modes, doesn't track
selection, doesn't validate.  Its only intelligence is escape-sequence
parsing.  All gameplay logic — including "this key isn't valid in
this mode" — lives in game-actor.

**game-actor owns `/mode`** as a global slot.  Mode-keyed dispatch
tables (§7.4) map (mode, key) → handler proc.  Handlers mutate state
at sl=0 between brackets (§1 B') and produce 0+ animation messages
to render-actor.

---

#### 7.2 Input-actor output: symbolic keys

input-actor emits messages with this shape:

```
/key { /sym Name /mods Byte } #$
```

Where `/sym` is a normalised Name and `/mods` is a bitmask:
`0x01 = Ctrl`, `0x02 = Alt`, `0x04 = Shift` (rarely needed since
shift produces a different `/sym`).

**Symbolic key table** (normalisation done in input-actor):

| Raw input | `/sym` | Notes |
| --- | --- | --- |
| Printable ASCII byte | `/a`..`/z`, `/A`..`/Z`, `/0`..`/9`, `/-`, `/=`, ... | `Name::make` of the 1-char string |
| CSI `[A`..`[D` | `/up` / `/down` / `/right` / `/left` | Arrow keys |
| CSI `[1~` ... `[24~` | `/home` / `/insert` / `/delete` / `/end` / `/page-up` / `/page-down` / `/f1`..`/f12` | Function and navigation |
| 0x0A / 0x0D | `/enter` | LF and CR both map here |
| 0x1B (alone) | `/esc` | After CSI-timeout (200 ms) |
| 0x20 | `/space` |  |
| 0x09 | `/tab` |  |
| 0x7F / 0x08 | `/backspace` |  |
| 0x01..0x1A | `/ctrl-a`..`/ctrl-z` | Ctrl-letter |
| 0x1B `<letter>` | `/alt-<letter>` | Alt prefix (Esc-then-letter pattern) |

**ESC ambiguity:** a lone 0x1B might be `/esc` OR the start of a
CSI / SS3 / Alt sequence.  input-actor waits up to 200 ms for follow-on
bytes; if none, emits `/esc`.  Tetrix uses the same pattern.

---

#### 7.3 Mode list

Locked set of 10 modes; `/mode` global slot holds one Name from this
list at all times.

| Mode              | Meaning                                                                |
| ----------------- | ---------------------------------------------------------------------- |
| `/cursor`         | Default — cursor on map, no unit selected, no menu                     |
| `/unit-move`      | A unit is selected; cursor constrained to move-range; pick destination |
| `/unit-attack`    | After move (or in-place): pick target from attack-range                |
| `/menu-action`    | Action menu open at cursor (move / attack / capture / wait / build)    |
| `/menu-build`     | At own production building with funds: pick unit type                  |
| `/menu-co-power`  | P pressed with full meter: confirm CO power                            |
| `/menu-system`    | Esc from `/cursor`: system menu (save / load / quit / options)         |
| `/menu-save-slot` | System → save: pick slot 1-9 (or quick)                                |
| `/menu-load-slot` | System → load: pick slot 1-9 (or quick)                                |
| `/animating`      | Animation in progress — most keys forward as `/skip-animations`        |

`/animating` is set whenever game-actor sends a `/sync-animations`
to render-actor and is awaiting `/animations-done`.  Other modes are
manipulated by handlers.

---

#### 7.4 Mode dispatch

game-actor's `handle-key` op (in `input.trx`):

```trix
% input.trx — handle-key key --
/handle-key { |key|
    /mode load /mode-dispatch-table load known? {
        key /mode load /mode-dispatch-table get
            key /sym get  swap  get-or-null
            dup null eq?
              { pop                        % no handler for this key in this mode
                /key-unbound-flash render-actor actor-send }
              { exec }
            if-else
    } { /unknown-mode error } if-else
} def

% mode-dispatch-table layout
/mode-dispatch-table
    << /cursor         <<...>>   % keyed by /sym -> handler
       /unit-move      <<...>>
       ...
    >>#$
def
```

Each mode-table is a Dict keyed by `/sym` Name → handler proc.
Missing key in a mode = no-op + UI flash (no error to player).
Unknown mode = code bug = error.

---

#### 7.5 Full keybind tables

**`/cursor` mode** (the most-used):

| Key | Action |
| --- | --- |
| `/up` / `/k` / `/w` | Move cursor up (r--) |
| `/down` / `/j` / `/s` | Move cursor down (r++) |
| `/left` / `/h` / `/a` | Move cursor left (q--) |
| `/right` / `/l` / `/d` | Move cursor right (q++) |
| `/y` / `/q` | Move cursor NW (hex diagonal — q-- r-±) |
| `/u` / `/e` | Move cursor NE (hex diagonal — q++ r-±) |
| `/b` / `/z` | Move cursor SW (hex diagonal — q-- r+±) |
| `/n` / `/c` | Move cursor SE (hex diagonal — q++ r+±) |
| `/enter` | If cursor on own unit: select → `/unit-move` |
| `/space` | Open `/menu-action` |
| `/tab` | Cycle to next own unit (cursor moves to it) |
| `/B` (shift-b) | If on own production with funds: → `/menu-build` |
| `/p` / `/P` | If CO meter ≥ cost: → `/menu-co-power` (menu chooses Power vs Super based on meter level) |
| `/E` (shift-e) | End current faction's turn (no confirm — high-frequency action; per GDD §18.7) |
| `/esc` | → `/menu-system` |
| `/page-up` / `/page-down` | Pan camera independent of cursor |
| `/ctrl-up` / `/ctrl-down` / `/ctrl-left` / `/ctrl-right` | Free-scroll camera |
| `/home` | Center camera on cursor |
| `/f1` / `/?` | In-game help overlay (per GDD §18.7; `/?` is the mnemonic alias) |
| `/f5` | Quick save → `quicksave.bws` |
| `/f9` | Quick load (re-exec) |
| `/Q` (shift-q) | Quit (confirmation prompt) |

**`/unit-move` mode:**

| Key | Action |
| --- | --- |
| `/up`/`/k`/`/w` ... `/n`/`/c` | Move cursor within move-range only (clamped) |
| `/enter` | Confirm destination — execute move; if attack possible from dest → `/unit-attack`, else → `/cursor` |
| `/esc` | Cancel — unit unselected, → `/cursor` |
| `/tab` | Skip to next valid destination |

**`/unit-attack` mode:**

| Key                           | Action                                        |
| ----------------------------- | --------------------------------------------- |
| `/up`/`/k`/`/w` ... `/n`/`/c` | Move cursor within attack-range only          |
| `/enter`                      | Confirm target — resolve attack, → `/cursor`  |
| `/esc`                        | Skip attack (move-only finalised) → `/cursor` |
| `/tab`                        | Skip to next valid target                     |

**`/menu-action` mode:**

| Key                              | Action                                                      |
| -------------------------------- | ----------------------------------------------------------- |
| `/up`/`/k`                       | Highlight previous menu item                                |
| `/down`/`/j`                     | Highlight next menu item                                    |
| `/enter`                         | Execute highlighted item                                    |
| `/esc`                           | Close menu, → `/cursor`                                     |
| `/m` / `/a` / `/c` / `/w` / `/b` | Mnemonic shortcuts (move / attack / capture / wait / build) |

**`/menu-build` / `/menu-co-power` / `/menu-system` / `/menu-save-slot` / `/menu-load-slot`:** same shape as `/menu-action` (up/down highlight, enter confirm, esc cancel).  Per-menu item lists per GDD §18 / §10.

**`/animating` mode:**

| Key    | Action                                                              |
| ------ | ------------------------------------------------------------------- |
| any    | Send `/skip-animations` to render-actor                             |
| `/esc` | Send `/skip-animations` AND queue an `/esc` to deliver in next mode |

**`/cheats` extension** (when `--cheats` is on, per §13.6): adds
`/ctrl-r`, `/ctrl-h`, `/ctrl-m`, `/ctrl-p`, `/ctrl-k`, `/ctrl-n`,
`/ctrl-s` to the `/cursor` mode dispatch table.

---

#### 7.6 Hex-direction mapping for diagonals

Bestagon is flat-top odd-q.  Hex 6-way movement from `(q, r)`:

| Direction        | Δq  | Δr (q even) | Δr (q odd) |
| ---------------- | --- | ----------- | ---------- |
| N (`/up`)        | 0   | -1          | -1         |
| S (`/down`)      | 0   | +1          | +1         |
| NW (`/y` / `/q`) | -1  | -1          | 0          |
| NE (`/u` / `/e`) | +1  | -1          | 0          |
| SW (`/b` / `/z`) | -1  | 0           | +1         |
| SE (`/n` / `/c`) | +1  | 0           | +1         |

`/left` and `/right` are simulated 2-hex moves on flat-top odd-q:
"left" steps NW or SW alternately based on context (default: same as
NW unless cursor is on bottom edge of map).  This is a tradeoff —
true hex has no "due west" neighbour.  Document for users; vim-style
`/y/u/b/n` is the recommended power-user binding.

---

#### 7.7 Mouse / pointing — explicit NO

GDD §21 lists mouse as out of scope.  This TDD confirms:

- No `--mouse` flag.
- No SGR mouse-mode escape sequences read.
- Terminals that send SGR mouse sequences when clicked will emit
  bytes input-actor doesn't recognise; input-actor's escape-parser
  drops unknown CSI sequences silently (or emits `/unknown-csi` Name,
  which game-actor's dispatcher ignores in every mode).

v2 candidate (not in scope): terminal mouse for click-to-move.

---

#### 7.8 Repeat / key-held behaviour

Terminal key-repeat is OS-level (autorepeat sends repeated bytes
after a holddown).  input-actor processes each as a separate `/key`
message.

game-actor's `/cursor` mode applies cursor-move handlers with no
debouncing — autorepeat just produces smooth cursor motion.

For `/unit-move` / `/unit-attack` modes, autorepeat is helpful
(cursor strafes through valid hexes).  No special handling.

For `/menu-*` modes, autorepeat scrolls highlight.  No special handling.

---

#### 7.9 Input buffering during animation

When game-actor is in `/animating` mode and the player pre-types:

- input-actor keeps reading and `actor-send`ing `/key` messages.
- game-actor's mailbox (cap 16 per §4.1) buffers them.
- First key sends `/skip-animations`; on `/animations-done`, the
  remaining queued keys process in order in the post-animation mode.

**Mailbox overflow** (16 unread keys): input-actor's `actor-send`
blocks until space frees.  Player's typing temporarily stalls.
Acceptable — turn-based; no rapid input demands.

---

#### 7.10 Input-actor body (sketch)

```trix
% input.trx — input-body
/input-body { |main|
    cooked-mode-leave
    raw-mode-enter
    {
        read-key-byte                     % blocks until a key
        % Parse escape sequences, build symbolic key Name
        parse-key-byte                    % stack: /sym mods (or null if mid-CSI)
        dup null ne {
            % Build /key message in global VM (cross-actor rule §4.3)
            [ /key 2 dict 3 -1 roll /sym put dup /mods 3 -1 roll put ]#$
            main actor-send
        } {
            pop pop
        } if-else
    } loop
} def
```

**Termination:** input-actor receives `/quit` on its own mailbox
(via game-actor → input-actor) during shutdown; drops raw mode,
returns.  In practice game-actor's quit-flow tears down render
first, so input-actor is killed by the main thread on the way out
per §4.6.

---

#### 7.11 Headless mode

`--headless` (implied by `--benchmark` / `--tests`): input-actor not
spawned; no `read-key-byte`; no raw-mode entry.

Benchmarks and tests don't need input.  Replay-determinism-check
(§9.6) runs the scenario with all-AI factions, no human input.

---

**Cross-refs:** GDD §18.7 (player-facing key list — the keys above
are a superset that includes diagonals and convenience bindings);
§4.2 (`/key` message in actor catalog); §4.3 (cross-actor messages
in global VM); §6.4 (`/skip-animations` flow); §13.6 (cheat keys
when `--cheats`); `examples/tetrix.trx` `input-body` (reference
implementation of read-key + raw-mode);
`docs/trix-reference.md` (raw-mode-enter / read-key-byte ops).

---

### 8. Render pipeline — **LOCKED, 2026-05-16**

**Decision:** **Full back-buffer rebuild per frame; `screen-render`
handles tty-write minimisation.**  The "game-actor emits dirty cells"
dirty-rect proposal in the draft was over-engineered for our scale —
math shows full-rebuild fits well inside the 16 ms render budget
(§6.2), and `screen-render` already does cell-level diff at the tty
boundary, giving most of the dirty-rect benefit for free.

---

#### 8.1 Why full-rebuild

Cost analysis (Epic, 75×54 map, 120×40 minimum terminal per GDD
locked decision 39).

Each hex glyph occupies a 9×5 bounding box (GDD Appendix A.1), but
adjacent hexes share diagonal columns and edge rows.  Effective
contribution to the back-buffer ≈ 8 cols × 5 rows = 40 cells per
hex.  Visible viewport at 120×40 minimum ≈ 16×7 hexes = 112 hexes ×
40 cells ≈ **~4500 cell-writes per frame** for the map layer (per
GDD §18.3).

| Layer | Operations | Estimated cost |
| --- | --- | --- |
| Clear back-buffer (or skip; overwrite all) | 1× memset of `screen-buf` | <1 ms |
| Viewport hex-by-hex | ~4500 `screen-put-cell` calls | ~5 ms |
| Unit sprites in viewport | ~30 units × 5 cells | <1 ms |
| Fog overlay | ~4500 cell mutations | ~5 ms |
| Cursor + mode overlays | ~50 cells | negligible |
| Side panel + combat log (left col, 4 content rows × ~57 chars) | ~230 chars | <1 ms |
| Minimap (right col, 5 rows × ~57 chars) | ~285 writes | <1 ms |
| Top-bar + keybind (floating, ~80 chars each) | ~160 chars | negligible |
| `screen-render` (diff + tty flush) | depends on dirty fraction | ~1-3 ms |

**Total ~15 ms worst-case** on Epic with a moving animation.
Fits inside the 16 ms render budget (§6.2's 33 ms frame minus
animation-advance + message-handling overhead).

Game-actor pulling state from global VM and the renderer reading it
back is one-way data flow — no `/redraw-region` accounting, no
dirty-bit invariants to maintain.  `screen-render`'s tty diff handles
"unchanged this frame, skip the write" automatically.

If Phase-1 perf measurement shows we miss the budget on Epic + heavy
animation, the fallback is per-layer dirty tracking (e.g.,
"minimap doesn't need re-render unless map state changed") — additive,
not a rewrite.

---

#### 8.2 Frame composition order

```trix
% render.trx — render-frame: composes one full frame to the back-buffer
/render-frame {
    render-clear              % overwrite all cells (or skip — relies on layer coverage)
    render-viewport           % map terrain + buildings within camera window
    render-units-in-view      % unit sprites overlaid on viewport
    render-fog-overlay        % apply fog state per cell
    render-cursor             % cursor highlight on its cell
    render-mode-overlay       % attack-mode targeting / move-range / preview-damage
    render-top-bar            % floating row 0: day/turn/funds/power
    render-side-panel         % left col, upper: Selected / Adjacent
    render-combat-log         % left col, lower: last damage event
    render-minimap            % right col (tall), cursor-centered slice
    render-keybind            % floating last row: hotkeys
    render-co-power-banner    % overlay if CO power active this frame
    scr screen-render         % flush diff to tty
    scr pop                   % discard screen handle per feedback_screen_ops_leave_screen
} def
```

Layer order matters: later layers overdraw earlier ones (side panel
and combat log sit on top of map area in case of a UI overlay; CO
banner sits on top of everything).

Each `render-*` op reads from global VM (`/map`, `/units`, `/factions`,
`/fog-grid`, `/cursor-q`, `/cursor-r`, `/selection`, `/mode`) and
writes to the screen handle.  None of the `render-*` ops mutate
game state.

---

#### 8.3 Viewport / camera

**Glyph dimensions** (from GDD Appendix A.1): hex glyph is 9 cols × 5
rows in its bounding box, but adjacent hexes share one diagonal
column (horizontal step 8) and adjacent rows share one underscore
row (vertical step 4 within an odd-q column, 2 between offset rows).

**Panel dimensions** (minimum 120×40 terminal, GDD §18.1 + §18.3).
Minimap section is fixed at 36 cols wide; left col absorbs extra
width as terminal grows (`c-mid = w - 36`):

| Panel | Cells | Notes |  |
| --- | --- | --- | --- |
| Top-bar (floating, row 0) | 120 × 1 | Day · Turn · CO · funds · power |  |
| **Map viewport** (rows 2-31, full width inside ` | ` bars) | 116 × 30 | ~16 hexes wide × 7 hexes tall = ~112 hexes visible |
| Side panel (left col upper, rows 33-34) | ~82 × 2 | Selected / Adjacent (or damage preview) |  |
| Combat log (left col lower, rows 36-37) | ~82 × 2 | Last damage event |  |
| Minimap (right col, rows 33-37) | 32 × 5 | Cursor-centered slice (§18.5); fixed |  |
| Keybind (floating, row 39) | 120 × 1 | Hotkey reference |  |

Bigger terminals enlarge the map viewport AND the side/combat
content area; the minimap slice stays at 32×5 since the right
column is fixed at 36 cols.  The section is fixed at 7 rows tall;
only viewport `h-vp = h - 10` scales with terminal height.

**Camera math:**

```trix
% Camera position = (camera-q, camera-r) = top-left hex visible
% cursor at (cursor-q, cursor-r)
% scroll margin = 2 hexes from any edge

/camera-update {
    % Pull cursor toward camera with 2-hex margin
    cursor-q  $[ camera-q + margin ] lt
      { /camera-q $[ cursor-q - margin ] store } if
    cursor-q  $[ camera-q + viewport-w - margin ] ge
      { /camera-q $[ cursor-q - viewport-w + margin + 1 ] store } if
    % ... same for r ...

    % Clamp camera to map bounds
    camera-q  0  lt  { /camera-q 0  store } if
    camera-q  $[ map-w - viewport-w ] gt
      { /camera-q $[ map-w - viewport-w ] store } if
    % ... same for r ...
} def
```

**Free-scroll mode** (PageUp/PageDown/Ctrl-Arrows): independent of
cursor; cursor stays put, camera moves.  Pan animates over 100ms
(3 ticks at 30 Hz).

**All map sizes need scrolling** — Tutorial 25×18 hexes = ~200×72
terminal cells, well past 120×40.  No "fit-to-screen" mode for v1.

---

#### 8.4 Status panel layout

GDD §18.1 specifies layout; this TDD pins the implementation shape.
The new layout (refined 2026-05-16) is a compact 4-zone design with
floating top/bottom rows and a 2-column lower section:

```
   Day N · Turn M · Hammer (funds 4500 · ★3.2 · /clear)              <- floating top-bar
+--------------------------------------------------------+
|                                                        |
|              ... viewport ...                          |   <- map viewport (rows 2..h-9)
|                                                        |
+----------------------------+--minimap------------------+   <- section top divider w/ inline label
| Selected: M1 Tank ★2 (HP 7/10, fuel 45/70, ammo 3/6) | .....##.....   |
| Adjacent: Boots Inf HP 6 · Talon Bomber HP 8         | ..#####.....   |
+----------------------------+ ...####.....              |   <- mid-divider (left col only)
| Combat: M1 Tank → Boots Inf · dealt 6 HP             | ...####.....   |
|   counter 1 HP                                       | .....##.....   |
+----------------------------+--------------------------+
   SPACE menu · Q quit · E end turn · ? help                          <- floating keybind
```

Sub-panels (`render-side-panel`, `render-combat-log`, `render-minimap`
plus `render-top-bar`, `render-keybind`) live in `render.trx`.  The
compact 2-line side-panel inlines unit details that earlier mocks
spread across multiple lines (HP / fuel / ammo all on the "Selected:"
line; vet tier shown via `★N`).  Cursor terrain / owner is implied
by hex glyph rendering in the viewport and not duplicated in text.

When `/mode` = `/attack`, the side panel's "Adjacent:" line is
replaced by damage-preview (§8.5 + GDD §18.6).

---

#### 8.5 Combat preview overlay

When `/mode` = `/attack` and cursor is over a valid target:

- Adjacent-enemies sub-panel (§8.4) replaces with damage-preview
  (GDD §18.6).
- Map viewport overlays: thin red outline on target hex, faint red
  trail line on attacker→target axis (1 char per hex).
- `combat-predict-damage atkr defr -- min max` op in `combat.trx`
  computes the preview (side-effect-free; does NOT draw from
  combat-rng-state — same op used by AI v2 §14.3 for scoring).

Per GDD §7.1 luck-mode dispatch: `/off` shows exact integer; `/on`
shows `min–max` range.

---

#### 8.6 Palette fallback ladder

**Mode detection:**

```trix
% config.trx — config-detect-colors
/config-detect-colors {
    (TERM) getenv /term local-def
    config-flag /colors get /auto eq? {
        % Heuristic:
        % - contains "256color" -> /256
        % - "linux", "xterm", "vt*" without 256 -> /16
        % - "dumb", unset, or no-tty -> /mono
        term ... detect-color-class
    } { config-flag /colors get } if-else
} def
```

**Palette tables** in `render.trx`:

| Table | 256-color | 16-color | mono |
| --- | --- | --- | --- |
| Terrain bg | full 256-palette per GDD A.2 | mapped to nearest ANSI 16 (per GDD A.0) | spaces only |
| Faction owner | hue per faction | ANSI 16 (Hammer=red, Boots=green, etc.) | brackets `[ ]` indicate ownership |
| Vet pip `★` | bright yellow + saturation | ANSI 11 bright yellow | `*` no color |
| Cursor highlight | bright white | ANSI 15 | reverse video (SGR 7) |

256→16 mapping is a fixed table (no runtime nearest-color match);
GDD A.0 already provides the 16-color anchors.

mono mode is for `--colors=mono` testing and dumb-terminal users —
gameplay is still readable (terrain disambiguated by glyph; units
disambiguated by letter and bracket).  Not a recommended play mode.

`--colors=auto` is the default and standard path.  `256` / `16` /
`mono` force a specific palette regardless of `$TERM`.

---

#### 8.7 Animator (owned by render-actor)

```trix
% render.trx — animation state inside render-actor's local frame

/anim-queue   64 array  local-def    % FIFO of pending animations
/anim-active  null      local-def    % currently-playing animation or null
/anim-tick    0u        local-def    % tick counter for active animation
```

Each animation is a Dict with:
- `/kind` Name (one of §6.3 catalog)
- `/payload` Dict (kind-specific fields)
- `/total-ticks` UInteger (duration in 30 Hz frames)
- `/render-fn` Name (the renderer to call per tick)

Per-frame in ANIMATING state:

```trix
% At each 30 Hz tick:
anim-active null eq?
  { anim-queue dequeue /anim-active store  0u /anim-tick store } if
anim-active null ne {
    anim-active /payload get  anim-tick  anim-active /total-ticks get
        anim-active /render-fn get  apply-renderer
    /anim-tick anim-tick 1u add store
    anim-tick anim-active /total-ticks get ge
      { null /anim-active store } if
}
render-frame    % full-rebuild including the animator's interpolated unit positions
```

**Interpolation:** linear only; no easing.  The renderer queries
`anim-active` when drawing unit sprites — if a unit is mid-animation,
the renderer interpolates its position between source and destination
hex.

**Skip / fast-forward:** on `/skip-animations` from game-actor,
render-actor drains both `anim-queue` and `anim-active`, applies all
their final-state effects (e.g. unit ends at destination), renders
one final frame, returns to IDLE per §6.1.

---

#### 8.8 Screen handle ownership and lifecycle

- One screen handle (back-buffer + cached front-buffer state) lives
  in `/scr` global slot, allocated at scenario init in `render.trx`.
- render-actor reads/writes via `scr screen-put-cell ...` etc.
- **Discipline:** each `screen-*` op in a render-step ends with
  `pop` because the screen ops return the handle for chaining.  Per
  `feedback_screen_ops_leave_screen.md`, forgetting `pop` after the
  last `screen-render` in a frame leaks a handle per frame.
- On quit (game-actor → render-actor `/quit`): render-actor drops
  alt-screen, exits raw-mode, closes scr.

---

#### 8.9 Headless mode (`--headless` / `--benchmark` / `--tests`)

render-actor not spawned; `/scr` not allocated; no `screen-*` calls;
no alt-screen / raw-mode setup.

Benchmark mode calls perf-target functions directly (no main loop,
no actors).  Test mode runs `selftest-run` and exits.

---

**Cross-refs:** GDD §18 (UI layout authoritative), Appendix A
(glyph dimensions + palette tables), locked decision 39 (min
terminal 120×40); `proto-screen-wireframe.trx` (panel layout
prototype); `feedback_screen_ops_leave_screen.md` (`pop` discipline);
§4.1 (render-actor mailbox); §6 (frame model — full-rebuild fits in
the 16 ms render-slice of the 33 ms budget); §13 (`--colors=`,
`--headless`).

---

### 9. RNG threading & determinism — **LOCKED, 2026-05-16**

**Decision:** **per-system RNG state in global VM, swap-in/swap-out
around each system's drawing burst.**  Replay contract: **bit-exact at
turn boundary.**  Compatible with v2 ai-worker promotion (§4.4).

---

#### 9.1 Constraints

Trix has a **single global RNG** (`rand-seed ulong --` / `rand-ulong --
ulong`; see `docs/trix-reference.md` §3.21).  No per-stream RNG state.
Per-system isolation is implemented in Trix-userland: each system owns
a ULong slot, swaps its state into the global RNG before drawing,
captures the resulting state back after.

---

#### 9.2 Master seed

- **Type:** ULong (matches `rand-seed`).
- **Source:** scenario file `/seed` field (GDD §16).  Default `0ul` =
  reserved sentinel meaning "use `clock` at scenario load."  Production
  gameplay sets an explicit seed for determinism.
- **CLI override:** `--seed=N` overrides the scenario's `/seed`.

---

#### 9.3 Per-system RNG slots

Each drawing system owns a ULong slot in global VM, initialized at
scenario load via a deterministic mix function:

```trix
% At scenario load (sl=0, in global VM)
master-seed                                        /master-seed       def
master-seed /mapgen   seed-mix                     /mapgen-rng-state  def
master-seed /combat   seed-mix                     /combat-rng-state  def
master-seed /ai       seed-mix                     /ai-rng-state      def
```

**Systems with their own slot:**

| System | Slot                | When drawn                 |
| ------ | ------------------- | -------------------------- |
| mapgen | `/mapgen-rng-state` | Scenario load, one-shot    |
| combat | `/combat-rng-state` | Per attack (luck-mode /on) |
| AI     | `/ai-rng-state`     | Per AI-faction turn        |

**Reserved for future use** (allocate the slot now if added later, to
preserve replay determinism on existing saves):

| System           | Slot                  | Notes                                         |
| ---------------- | --------------------- | --------------------------------------------- |
| Cosmetic effects | `/cosmetic-rng-state` | If we add CO-power random flashes etc.        |
| Map events       | `/event-rng-state`    | If GDD adds random weather draws beyond §11.5 |

---

#### 9.4 `seed-mix` contract

```
seed-mix    ulong name -- ulong
```

Deterministic function: same inputs always yield same output.
Well-distributed: distinct `name` inputs produce uncorrelated outputs.
Implementation lives in `rng.trx`; suffices to do
`master XOR (name-hash * MAGIC_PRIME)` where `name-hash` folds the
name's bytes.  Exact mix function is implementation detail and can be
revised pre-1.0 without breaking the §9 contract — but any change
invalidates existing replay seeds, so freeze it at first ship.

---

#### 9.5 Swap-in / swap-out idiom

Each system wraps its drawing burst:

```trix
% rng.trx — one pair per system
/combat-rng-begin { combat-rng-state rand-seed } def
/combat-rng-end   { rand-ulong /combat-rng-state store } def

/ai-rng-begin     { ai-rng-state rand-seed } def
/ai-rng-end       { rand-ulong /ai-rng-state store } def

/mapgen-rng-begin { mapgen-rng-state rand-seed } def
/mapgen-rng-end   { rand-ulong /mapgen-rng-state store } def

% Usage in combat.trx
/resolve-attack { |atkr defr|
    combat-rng-begin
        atkr defr luck-roll       % rand-bounded-uinteger draws
        damage-table-lookup
    combat-rng-end
    % apply-damage at sl=0 from here
} def
```

**Invariant (must hold):** systems must NOT yield to other coroutines
between `begin` and `end`.  Yielding lets another system reseed the
global RNG and clobber the stream.

- **v1 (inline AI in game-actor):** no yields happen during
  combat-resolution / mapgen / `ai-think`; the invariant holds
  automatically.
- **v2 ai-worker:** if `ai-think` runs to completion without yields,
  wrap the whole thing in begin/end.  If it must yield (long search):
  `ai-rng-end` before yielding, `ai-rng-begin` on resume.

---

#### 9.6 Replay contract — **bit-exact at turn boundary**

State hash of the global VM at sl=0 between turns must match across
replays from the same `master-seed + command-stream`.

**Achievable because:**
- Persistent state lives in global VM (§1 B'); scratch dies on
  `restore`.
- All RNG draws come from per-system deterministic streams (§9.5).
- Action-application order is deterministic (§9.7).
- Snapshot save/load (§10) is trivially bit-exact.
- Replay-from-commands is bit-exact because seeds are deterministic
  and action order is deterministic.

**Testable** via §11 self-test (in the Tier-3 §11 section): optional
flag `--replay-determinism-check` re-runs the same command stream
twice from the same master seed and compares state hashes at each
turn boundary.

---

#### 9.7 Action-order determinism rules

Six rules; violation breaks bit-exact replay.

1. **Faction iteration:** by `/factions` array index (0, 1, 2, 3).
   Never Dict order.
2. **Unit iteration within a faction:** by `/faction-units` Array
   order.  Append on spawn; swap-remove on kill (or compact — either,
   but pick one and apply identically everywhere).
3. **Action application within a turn:** by action-plan Array order.
   AI emits a deterministic ordering; player produces actions
   sequentially in input order.
4. **Whole-map scan:** by linear index `r * w + q`.  Never Dict
   iteration.
5. **Mailbox processing:** by `actor-recv` FIFO arrival order.
6. **`for-all` over Dicts is forbidden where order matters.**  If a
   Dict-of-X must be iterated in some order, store an auxiliary
   Array of keys whose order is canonical.

**Specific applications:**

- `/units` Dict (unit-id → unit Dict) — never iterate directly; use
  `/faction-units` Arrays.
- `/factions` is already an SoA of Arrays (§3.3) — iterate by index.
- Map grids (§3.1) are `Packed<Byte>` — linear-index iteration is
  natural.

---

#### 9.8 Out of scope

- **Non-seeded `clock`-based randomness in gameplay code** — forbidden.
  Allowed only in `--benchmark` and dev-only paths.
- **Cryptographically secure RNG** — not needed (no security context).
- **Cosmetic RNG sharing the gameplay streams** — forbidden.  Add a
  separate `/cosmetic-rng-state` if cosmetic randomness is wanted.

---

**Cross-refs:** GDD §16 (`/seed` scenario field); GDD locked decision
33 (2-mode luck — /off draws no RNG; /on draws per attack);
`docs/trix-reference.md` §3.21 (RNG API);
`feedback_trix_idioms_for_sim_code.md` (`rand-seed` wants ULong,
`rand-bounded-uinteger` wants UInteger); §4.4 (ai-worker promotion —
this section satisfies the per-system-seed affordance);
§10 (save format — bit-exact contract makes both snapshot and
command-log formats viable); §11 (self-test framework — adds the
`--replay-determinism-check` flag).

---

## Tier 3 — Operations

### 10. Save-game format — **LOCKED, 2026-05-16**

**Decision:** **Custom `.bws` format — Bestagon-specific binary file
containing ONLY the game-state Dict from §3, serialised via Trix
`to-binary-token`.**  Not a whole-VM `snap-shot`.  Single built-in
save mechanism for v1; replay export (`.bwr`) deferred to v2 as an
additive variant of the same schema.

**Why custom instead of `snap-shot`:**

|  | Whole-VM `snap-shot` | **Custom `.bws`** |
| --- | --- | --- |
| File size (estimated) | 300–500 KB | ~60 KB |
| Survives Bestagon source patch | risky (stale bytecode in file) | yes (data only) |
| Survives Trix engine bump | no (SNAPSHOT_VERSION coupling) | yes if `to-binary-token` is stable |
| What's saved | code + stacks + dicts + scratch + state | state only |
| Auditable | binary blob, opaque | versioned header + named-field schema |
| Write/read latency | ~500 ms target | ~50–100 ms target |

`snap-shot` was the lazy answer.  We control the data schema and the
file shape; the format below codifies it.

---

#### 10.1 File layout

```
Offset  Size  Field           Contents
------  ----  --------------  -------------------------------------
  0     4     magic           "BWS\0" (0x42 0x57 0x53 0x00)
  4     1     format-version  uint8, currently 0x01
  5     1     flags           reserved (0)
  6    10     reserved        zero-padded
 16     N     body            to-binary-token serialised Dict

Total: 16-byte fixed header + variable body.
```

Header is hand-written by `save.trx` (raw `write-bytes`).  Body is
the Trix binary-token form of the game-state Dict (below) produced
by `to-binary-token`.

---

#### 10.2 Body schema (format-version 1)

A Dict with the following keys.  Order is unspecified (Dict is
deserialised by name lookup, not by position); all keys required
unless noted.

| Key | Type | From | Notes |  |  |  |
| --- | --- | --- | --- | --- | --- | --- |
| `/version` | UInteger | hard-coded `1u` | Matches header byte-4; loader cross-checks |  |  |  |
| `/scenario-name` | String | `/scenario-name` slot | Resolved to `.bw` path on load to re-bind win-conditions (§10.4 step 3); also used for status panel + replay export |  |  |  |
| `/master-seed` | ULong | `/master-seed` slot (§9.2) |  |  |  |  |
| `/turn-number` | UInteger | `/turn-number` slot (§3.5) | 1-indexed cumulative |  |  |  |
| `/day` | UInteger | `/day` slot (§3.5) | 1-indexed game day; rolls forward when last faction ends turn |  |  |  |
| `/current-faction` | Byte | `/current-faction` slot (§3.5) | Index into `/factions` |  |  |  |
| `/turn-phase` | Name | `/turn-phase` slot (§3.5) | `/move | /build | /end-of-turn` |  |
| `/game-status` | Name | `/game-status` slot (§3.5) | `/running | /victory | /draw | /quit-requested` |
| `/winner-id` | Byte | `/winner-id` slot (§3.5) | Faction id, or 255b sentinel for "no winner yet" |  |  |  |
| `/weather` | Name | `/weather` slot | `/clear`, `/rain`, ... |  |  |  |
| `/weather-cycle-idx` | UInteger | `/weather-cycle-idx` slot | Position in `/weather-cycle` (§11.5); 0 if scenario has no cycle |  |  |  |
| `/weather-day-last-cycled` | UInteger | `/weather-day-last-cycled` slot | Day value at last cycle advance (§11.5) |  |  |  |
| `/map` | Dict | `/map` (§3.1) | Composite: `/w`, `/h`, 6 × `Packed<Byte>` |  |  |  |
| `/units` | Dict | `/units` (§3.2) | unit-id → per-unit Dict |  |  |  |
| `/faction-units` | Dict | `/faction-units` (§3.2) | faction-id → Array<unit-id> |  |  |  |
| `/faction-buildings` | Dict | `/faction-buildings` (§3.4) | faction-id → Array<cell-idx>; could rebuild from `/owner` grid on load but persisted for self-validation |  |  |  |
| `/factions` | Dict | `/factions` (§3.3) | SoA per-faction state — all fields per GDD §17.2 |  |  |  |
| `/next-unit-id` | Byte | `/next-unit-id` slot (§3.5) | Next id to allocate on spawn |  |  |  |
| `/combat-rng-state` | ULong | §9.3 slot |  |  |  |  |
| `/mapgen-rng-state` | ULong | §9.3 slot |  |  |  |  |
| `/ai-rng-state` | ULong | §9.3 slot |  |  |  |  |
| `/cosmetic-rng-state` | ULong | §9.3 reserved slot | Saved as `0ul` if unused — preserve for future |  |  |  |
| `/event-rng-state` | ULong | §9.3 reserved slot | Saved as `0ul` if unused — preserve for future |  |  |  |

**Excluded** (re-derived from scenario re-run, transient, or
recomputable):
- `/win-conditions` — rebuilt by scenario re-run (§10.4 step 3).
- `/scenario-overrides`, `/terrain-key` — re-bound by scenario re-run.
- `/event-ring` — transient display state (v2 `.bwr` adds persistent
  event log).
- `/settings` (`/ai-perf-tier` etc.) — re-parsed from CLI on load.
- Loaded modules — `require` rebuilds them.
- Coroutine stacks — actors re-spawn from `main.trx`.
- Exec / dict stacks — at sl=0 these are empty.
- `/config` Dict — re-parsed from CLI on load.
- Render / input / animation scratch — local-VM, doesn't exist between
  brackets.
- Combat / AI / pathfind scratch — same.
- Debug overlay state — transient UI.

---

#### 10.3 Save mechanics

```trix
% save.trx — save-game path --
/save-game { |path|
    % Caller guarantees sl=0 between turn brackets (§1 B').

    % Build the state Dict (fresh Dict, all entries already in global VM)
    24 dict
        dup /version                   1u                         put
        dup /scenario-name             scenario-name              put
        dup /master-seed               master-seed                put
        dup /turn-number               turn-number                put
        dup /day                       day                        put
        dup /current-faction           current-faction            put
        dup /turn-phase                turn-phase                 put
        dup /game-status               game-status                put
        dup /winner-id                 winner-id                  put
        dup /weather                   weather                    put
        dup /weather-cycle-idx         weather-cycle-idx          put
        dup /weather-day-last-cycled   weather-day-last-cycled    put
        dup /map                       map                        put
        dup /units                     units                      put
        dup /faction-units             faction-units              put
        dup /faction-buildings         faction-buildings          put
        dup /factions                  factions                   put
        dup /next-unit-id              next-unit-id               put
        dup /combat-rng-state          combat-rng-state           put
        dup /mapgen-rng-state          mapgen-rng-state           put
        dup /ai-rng-state              ai-rng-state               put
        dup /cosmetic-rng-state        cosmetic-rng-state         put
        dup /event-rng-state           event-rng-state            put
    /state local-def

    % Open file
    path /write-binary open-file /f local-def

    % Header (16 bytes)
    f (BWS\0) write-bytes              % magic
    f 1b write-byte                    % format-version
    f 0b write-byte                    % flags
    f 10 write-zero-bytes              % reserved padding

    % Body: binary-token serialisation of state Dict
    state to-binary-token              % str (binary bytes)
    f exch write-bytes

    f close-file
} def
```

(Exact stream / open-file ops follow Trix conventions; the shape is
shown for design clarity.)

The state Dict is allocated **inside the proc's local-VM scratch** —
it doesn't need to survive; once `to-binary-token` walks it, the
serialised bytes are what's persisted.  Allocation cost: one fresh
Dict (16 slots) + 14 `put` calls.  No deep copy needed — fields
already reference global-VM containers.

---

#### 10.4 Load mechanics

Loading is a **fresh process invocation**:

```bash
./trix examples/bestagon/main.trx -- --load=saves/quicksave.bws
```

`main.trx` flow with `--load=PATH`:

```trix
% 1. Parse CLI
parse-cli /config def

% 2. Always require every module (rebuild engine state from current source)
(examples/bestagon/config.trx)  require
(examples/bestagon/rng.trx)     require
... all 20 modules ...

% 3. Branch on --load vs --scenario
config /load-path get null ne {
    % Load path
    config /load-path get bws-load        % reads file, validates, deserialises Dict
    /loaded-state def

    % Bind state slots from loaded-state.  All slots from §10.2 body
    % schema; %3.5 lists their global slot names.
    loaded-state /scenario-name             get  /scenario-name             def
    loaded-state /master-seed               get  /master-seed               def
    loaded-state /turn-number               get  /turn-number               def
    loaded-state /day                       get  /day                       def
    loaded-state /current-faction           get  /current-faction           def
    loaded-state /turn-phase                get  /turn-phase                def
    loaded-state /game-status               get  /game-status               def
    loaded-state /winner-id                 get  /winner-id                 def
    loaded-state /weather                   get  /weather                   def
    loaded-state /weather-cycle-idx         get  /weather-cycle-idx         def
    loaded-state /weather-day-last-cycled   get  /weather-day-last-cycled   def
    loaded-state /map                       get  /map                       def
    loaded-state /units                     get  /units                     def
    loaded-state /faction-units             get  /faction-units             def
    loaded-state /faction-buildings         get  /faction-buildings         def
    loaded-state /factions                  get  /factions                  def
    loaded-state /next-unit-id              get  /next-unit-id              def
    loaded-state /combat-rng-state          get  /combat-rng-state          def
    loaded-state /mapgen-rng-state          get  /mapgen-rng-state          def
    loaded-state /ai-rng-state              get  /ai-rng-state              def
    loaded-state /cosmetic-rng-state        get  /cosmetic-rng-state        def
    loaded-state /event-rng-state           get  /event-rng-state           def

    % Re-run the scenario file to rebind win-condition procs into
    % userdict (GDD §15.3).  Win-conditions are NOT persisted in the
    % save body; the scenario file is authoritative for predicate
    % bodies.  `scen-load` resolves /scenario-name to a .bw path,
    % runs it, validates the userdict shape (GDD §16.1), and binds
    % /scenario.  Mapgen is skipped on the load path -- /map already
    % came from the save body above.
    scenario-name scen-load /scenario def
} {
    % Fresh-scenario path: parse scenario file, run mapgen
    config /scenario-path get scen-load /scenario def
    scenario mapgen-from-scenario
} if-else

% 4. Spawn 3 actors, yield 3 times (§4.5)
% 5. Game-actor enters main loop at start-of-current-faction's-turn
```

`bws-load` (in `save.trx`):

```trix
% path -- state-dict
/bws-load { |path|#+4
    /f   path (r)#b stream  local-def
    /raw f read-all         local-def
    f close-stream

    % Header validation (bytes 0-3 magic, 4 format-version,
    % 5-15 flags + reserved)
    raw 0 4 get-interval (BWS\0) eq
      { } { /bws-magic-mismatch throw } if-else

    /file-version raw 4 get local-def
    file-version 1b eq
      { } { /bws-version-unsupported throw } if-else
                                              % v2+ migrator hook here

    % Body: one binary-token value (the state Dict)
    raw 16 raw length 16 sub get-interval
    token                                     % post any true | false
      { exch pop }                            % drop the post-string
      { /bws-corrupt throw }
    if-else
    dup /version get file-version /integer-type cast eq
      { } { /bws-version-cross-check-failed throw } if-else
} def
```

---

#### 10.5 File naming, slots, autosave

**Extension:** `.bws` (Bestagon Wars Save).  Distinct from `.bw`
(scenario, GDD §16).  Reserved: `.bwr` (replay; v2).

| Slot        | Filename                   | Trigger                              | Behaviour             |
| ----------- | -------------------------- | ------------------------------------ | --------------------- |
| Quick save  | `quicksave.bws`            | F5                                   | Overwrite             |
| Quick load  | `quicksave.bws`            | F9                                   | Re-exec via `--load=` |
| Autosave    | `autosave.bws`             | End of player faction's turn         | Overwrite             |
| Named slots | `save1.bws` .. `save9.bws` | Menu / `F2`..`F10`                   | User-named per slot   |
| Crash dump  | `crash.bws`                | Uncaught error from sl=0 frame (§17) | Overwrite             |

**Save dir:** `./saves/` default; `--save-dir=path` overrides; auto-
created if absent.

**Autosave cadence:** end of every player faction's turn (matches AW
/ Wargroove).  Game-actor between brackets calls `save-game`
synchronously.  Target under 100 ms (vs. the 500 ms originally
budgeted for whole-VM snapshot — actual `to-binary-token` of ~60 KB
on the local SSD).  `--no-autosave` flag disables.

---

#### 10.6 Save flow

```
% F5 pressed -> input-actor parses to /save-quick command
input-actor: -- /save-quick path --> game-actor

game-actor:
    actor-recv -> /save-quick path
    % We may be mid-bracket (player just hit F5 in selection mode).
    % If so, finish the bracket cleanly and discard selection state.
    in-bracket? { restore } if          % land at sl=0
    path save-game                      % serialise + write
    [/save-flash path]#$ render-actor actor-send
    actor-recv ...                      % continue main loop
```

**Critical invariants:**
1. `save-game` called at sl=0 (not inside a bracket).  Avoids
   serialising scratch state.
2. State Dict construction does not modify the persistent slots.
3. Failure mid-write leaves the file in an inconsistent state — we
   write to `<path>.tmp` then atomic-rename to `<path>` on success.
   (`save.trx` implements this.)

---

#### 10.7 Load flow

Fresh process via `--load=PATH`:

1. CLI parse populates `/config` with `/load-path`.
2. `require` every module (rebuilds engine).
3. `bws-load` reads header, validates magic + version, parses body
   via `token` (the binary-token wire decoder), returns the state Dict.
4. Bind 14 slots from state Dict into global VM.
5. Spawn 3 actors, yield 3 times.
6. game-actor enters main loop at start-of-`current-faction`'s turn.

**Error paths:**

| Error | Cause | Handling |
| --- | --- | --- |
| `/bws-magic-mismatch` | Wrong magic bytes | top-level handler in `main.trx`: print "not a Bestagon save file", exit non-zero |
| `/bws-version-unsupported` | format-version > 1 (or older than migrator can handle) | print "save version N not supported by this build", exit |
| `/bws-version-cross-check-failed` | Header version vs body `/version` mismatch | print "save file is corrupt", exit |
| `/bws-corrupt` | Body bytes don't form a valid binary-token stream (`token` returns false, or raises one of its decode errors) | print "save file is corrupt or truncated", exit |
| `/filename-not-found` | Path doesn't exist (raised by `stream`) | print "save file <path> not found", exit |

No auto-recovery from corrupt save — user retries with another slot.
Top-level handler in `main.trx` is the only error catcher; no `try`
wrapping the load path (errors should be terminal at this layer).

---

#### 10.8 Format-version migration

Each format-version bump documents a migrator from N → N+1.
Bestagon-source-only changes (rule tweaks, new units, balance) do
NOT bump format-version; only schema changes (new field, removed
field, semantics shift on existing field) do.

**Adding a field (v1 → v2):**
- Reader: if field present, use it; if absent, supply a default.
  Bump format-version when readers REQUIRE it.
- Writer: always write the new field.

**Removing / renaming a field:**
- Bump format-version.
- Migrator function `bws-migrate-v<N>-to-v<N+1> dict -- dict` runs in
  `bws-load` between parse and slot-bind.

**Reading a file from a newer build:**
- `format-version` byte exceeds the build's known max — error out with
  `/bws-version-unsupported`.  No partial-load attempts.

---

#### 10.9 Version coupling — clarified

This format is robust to:

- **Bestagon source patches** (rule tweaks, new content) — data shape
  unchanged, saves load.
- **Trix engine bumps that preserve `to-binary-token` format** —
  historically stable; saves load.
- **Format-version bumps within Bestagon** — migrators handle it.

This format is NOT robust to:

- **Trix `to-binary-token` format change** — would require a one-shot
  conversion utility.  Has not happened in the codebase to date.
- **Endianness changes** — all targets little-endian for v1.

---

#### 10.10 v2 replay-export affordance (`.bwr`)

A `.bwr` is a `.bws` variant with two additions and one allowable
omission:

- ADD `/command-log` — global Array of action tags + payloads logged
  by game-actor at each apply-action call.  (Hook in game-actor's
  dispatcher; one `array-append` per accepted action.)
- ADD `/initial-turn-number` — turn number at which logging started
  (usually 1).
- MAY OMIT `/map` / `/units` / `/faction-units` / `/factions` —
  recoverable from `scenario-name` + `master-seed` + replay of
  `command-log` (the §9 bit-exact contract guarantees this).

`.bwr` files are smaller than `.bws` (no map/unit blobs) and human-
diffable in their command-log section.

v2 export:

```trix
% bws-to-bwr in-path out-path --
% Reads .bws, drops map/units, copies command-log + headers, writes .bwr.
```

v1 does not implement `.bwr` but **must** allocate `/command-log` as
an empty global Array at scenario init so that v2's loggers can
`array-append` without restructuring.  (Adding it later is fine
since v1's `.bws` will simply lack the field on load — handled by
the v1→v2 migrator.)

---

**Cross-refs:** GDD §15 (save/load semantics);
`docs/trix-reference.md` §3.21 (binary tokens, `to-binary-token`);
§1 B' (sl=0 save point); §3 (game-state schema — every field in §10.2
maps to a §3 slot); §4.2 (command messages); §9 (bit-exact contract
makes `.bwr` viable); §13 (CLI: `--load`, `--save-dir`,
`--no-autosave`); §17 (crash dumps use the same `save-game` op).

---

### 11. Self-test & invariant harness — **LOCKED, 2026-05-16**

**Decision:** CLI-controlled, not build-time.  Two distinct run modes
share the same registered-invariants table: **(a) `--tests` one-shot
mode** runs everything and exits with a status code (CI / dev gate);
**(b) `--invariants=LIST` per-turn mode** runs a chosen subset at
each turn boundary (development play / bug repro).  Plus
**`--replay-determinism-check`** as a third specialised mode.

GDD §19 (99 invariants across 18 phase-grouped subsections)
enumerates the catalog; this section locks the harness.

---

#### 11.1 Registration

Every invariant lives in `selftest.trx` and registers itself into
a global Dict at load time:

```trix
% selftest.trx — invariant registration pattern
/_invariants 256 dict def              % name -> { /tag /phase /check /scope }

/register-invariant { |tag phase scope check|
    _invariants tag
        4 dict
            dup /tag   tag   put
            dup /phase phase put
            dup /scope scope put
            dup /check check put
        put
} def

% Each invariant follows this shape:
/selftest-hex-roundtrip
    /coord-roundtrip                              % tag (Name)
    0u                                            % phase (per GDD §19)
    /test-only                                    % scope (see §11.2)
    { coord-roundtrip-test }                      % check proc
    register-invariant
```

**Invariant naming:** module-prefixed (`selftest-` or `<module>-`)
per §5.3.  Tags are flat Names registered into `/_invariants` so
they can be selected by `--invariants=` lists.

**Phase number** from GDD §19 grouping — drives which invariants
run during Phase-N development before later phases are implemented.

---

#### 11.2 Scopes

| Scope | Meaning | When valid to run |
| --- | --- | --- |
| `/test-only` | Pure unit test — doesn't touch live state | Anytime; cheap |
| `/per-turn` | Reads live game state; checks an ongoing invariant | At turn boundary in `--invariants=` mode |
| `/full-state` | Walks all units / all cells | `--tests` mode only — too expensive for per-turn |
| `/setup` | Constructs a synthetic fixture, runs assertion, tears down | `--tests` mode only |

`--invariants=` per-turn checks only run `/per-turn` and `/test-only`
scopes.  `/full-state` and `/setup` are gated to `--tests` to
preserve the §6 frame budget.

---

#### 11.3 Run modes

**Mode A: `--tests` (one-shot, exit-code-driven)**

```
./trix examples/bestagon/main.trx --tests
```

`main.trx` skips actor-spawn; calls `selftest-run-all`:

1. Iterate `/_invariants` by tag (deterministic order: GDD §19
   phase order, then registration order within phase).
2. For each invariant whose phase ≤ current build phase: invoke
   its `/check` proc within a `try` block.
3. Track pass / fail / skip counts; on fail, capture invariant tag
   + error message + status dump.
4. Print summary line `PASS: 87  FAIL: 0  SKIP: 12  (out of 99)`.
5. Exit code: `0` if all PASS, `1` if any FAIL, `2` on harness error.

**Output format** mirrors `tests/run_all.sh` style for CI
consumption:

```
phase 0 (5 invariants):
  PASS coord-roundtrip
  PASS adjacency-fan-out
  PASS pos-idx-bijection
  PASS phase0-readability
  PASS minimap-25x18

phase 1 (7 invariants):
  PASS procgen-connectedness
  FAIL procgen-seed-determinism
    Two runs from seed 42 produced different /terrain bytes at idx 1247
    Expected: 4b (Forest)
    Actual:   2b (Plain)

PASS: 87  FAIL: 1  SKIP: 11  (out of 99)
```

**Mode B: `--invariants=LIST` (per-turn in interactive play)**

```
./trix examples/bestagon/main.trx --scenario=tutorial --invariants=capture,fog
```

After each turn-bracket close (in game-actor's main loop):

```trix
config-flag /invariants get empty? not {
    config-flag /invariants get
        { |tag|
            _invariants tag get
                dup /scope get  /per-turn /test-only  contains? {
                    /check get exec
                } { pop } if-else
        } for-all
} if
```

`--invariants=all` expands to the full set of `/per-turn` +
`/test-only`-scope tags.  Comma-separated tag names select specific
ones (`capture` matches all tags starting with `capture-`; case-
insensitive substring match).

**Failure mode in interactive play:**

- Print to a dedicated message-log line:
  `INVARIANT FAIL: capture-progress-without-capturer at turn 5`
- DO NOT abort the running game (player may have hours of work).
- Crash dump (`crash.bws`) opt-in via `--invariants-fatal` (post-1.0).

**Mode C: `--replay-determinism-check`**

```
./trix examples/bestagon/main.trx --scenario=tutorial --replay-determinism-check
```

`main.trx` runs the scenario twice with all-AI factions, identical
master seed, and captures state hash at each turn boundary.  Hashes
are compared.  Exit code: `0` if matching, `1` if any divergence.

`selftest.trx` op:

```
% selftest-state-hash -- ulong
% Returns a 64-bit hash of the persistent game state at sl=0.
% Walks /map grids + /units (sorted by unit-id) + /factions + RNG slots.
% Excludes transient state (cursor pos, /mode, screen, mailbox contents).
```

The state-hash op is also the bedrock of §9.6's bit-exact replay
contract.

---

#### 11.4 Standard invariant patterns

Locked patterns; new invariants pick the closest match.

**Pattern A — Mathematical identity (Phase 0).**

```trix
/coord-roundtrip-test {
    % All 25x18 cells round-trip pos -> cube -> pos
    25 0 do { /q local-def
        18 0 do { /r local-def
            q r pos-to-cube cube-to-pos
            /q1 local-def /r1 local-def
            q q1 eq? { } { (coord-roundtrip failed) error } if-else
            r r1 eq? { } { (coord-roundtrip failed) error } if-else
        } for
    } for
} def
```

**Pattern B — Dual-invariant on state field (proto-capture.trx
precedent).**

```trix
/capture-progress-without-capturer-test {
    % progress > 0 iff capturer-id != 255 (the sentinel)
    map-w 0 do { /q local-def
        map-h 0 do { /r local-def
            q r map-cell-capture-progress  /p local-def
            q r map-cell-capturer-id       /cid local-def
            p 0b ne   cid 255b ne   eq? {
                % both zero / both nonzero — OK
            } {
                (capture: progress=) p {0:d} format-fmt
                  ( but capturer=) cid {0:d} format-fmt
                concat error
            } if-else
        } for
    } for
} def
```

**Pattern C — Fixture-based with synthetic setup.**

```trix
/capture-2-turn-test {
    % Build a fixture in scratch arena, run capture-tick twice,
    % assert HQ ownership flipped, tear down.
    save                          % isolate fixture
        synthetic-hq-fixture      % allocates in local VM
        capture-tick capture-tick % per-turn ticks
        check-hq-owner-equals-2
    restore                       % discard fixture
} def
```

Note Pattern C uses an *inner* save bracket — allowed per §1 B'
invariant 5 only inside selftest code, not gameplay code.

---

#### 11.5 Per-turn perf gate

`--invariants=` per-turn checks run inside the gap between turn
brackets at sl=0.  They contribute to the per-turn wall-clock
budget (§12).

**Budget contribution:**

| Scope                                            | Budget allocation                                 |
| ------------------------------------------------ | ------------------------------------------------- |
| `/per-turn` (cheap, O(units) or O(active-zones)) | up to 5 ms per turn total when `--invariants=all` |
| `/test-only`                                     | < 1 ms each                                       |

Invariant authors must annotate the cost class.  Anything over 5 ms
single-check goes to `/full-state` scope (out of `--invariants=`'s
per-turn path).

A profiling pass at Phase 1 sanity-checks this: `--invariants=all`
on Epic should not double the turn time.

---

#### 11.6 Harness output and integration

**`--tests` exit-code matrix** (for CI / shell):

| Code | Meaning                                                     |
| ---- | ----------------------------------------------------------- |
| 0    | All requested invariants PASS                               |
| 1    | At least one FAIL                                           |
| 2    | Harness error (invariant raised non-`/fail` error in setup) |
| 64   | Argument error (unknown invariant tag, etc.)                |

stderr captures harness errors; stdout captures pass/fail report.

**Optional flags** (Phase 1):

| Flag                   | Meaning                                               |
| ---------------------- | ----------------------------------------------------- |
| `--tests-phase=N`      | Run only invariants with `phase=N`                    |
| `--tests-tag=PREFIX`   | Run only invariants whose tag starts with PREFIX      |
| `--tests-stop-on-fail` | Halt at first FAIL (default: run all, report summary) |

These extend `--tests` (which itself implies `--headless`).

---

#### 11.7 Coverage discipline

**Rule:** new mechanic implies new invariant.  PR checklist (in
contributor docs, not enforced by harness): "did you add at least
one invariant in `selftest.trx`?"

Specifically for the patterns prototyped in Phase 0.6:

| Mechanic       | Required invariant                                     | Pattern            |
| -------------- | ------------------------------------------------------ | ------------------ |
| Hex math       | round-trip identity                                    | Pattern A          |
| Pathfinding    | reachable-set size matches manual count for fixture    | Pattern C          |
| Combat         | symmetry + counter rules                               | Pattern A + C      |
| Capture        | progress ↔ capturer dual-invariant                     | Pattern B          |
| Fog            | visible cells subset of seen cells                     | Pattern B          |
| State mutation | unit-id grid ↔ /units Dict ↔ /faction-units cross-sync | Pattern B (triple) |

The "state-cross-sync" invariant is the most important — it catches
the §3.2 four-index discipline violations that are easy to introduce
in `unit-spawn` / `unit-move` / `unit-kill`.

---

#### 11.8 GDD §19 reference

This TDD is the harness spec; GDD §19 is the catalog (99 invariants
across 18 phase-grouped subsections, with a phase-marker on each so
this harness can phase-gate execution).

GDD §19 prose currently says "Run via
`./trix.opt examples/bestagon.trx --self-test`."  As part of this
TDD lock, GDD §19 will be updated to match TDD-locked paths and
flags: `./trix.opt examples/bestagon/main.trx --tests`.

---

**Cross-refs:** GDD §19 (invariant catalog); §1 B' (per-turn checks
run between brackets at sl=0); §5 (`selftest.trx` module + naming);
§9.6 (bit-exact replay test uses state-hash op); §12 (per-turn
checks contribute to the perf budget); §13 (`--tests`,
`--invariants=`, `--replay-determinism-check`); `proto-capture.trx`
(dual-invariant Pattern B precedent); `tests/run_all.sh` (output
format model).

---

### 12. Performance budgets — **LOCKED, 2026-05-16**

Each gate is a **CI assertion** — if the prod-build measurement
exceeds the target on the reference hardware, CI fails.  Numbers
without a measured prototype are **proposed gates** flagged
"verify in Phase 1" — first measurement seeds the prototype baseline.

---

#### 12.1 Reference hardware and harness

**Hardware:** modern x86_64 / Apple Silicon laptop running the
project's CI machine class (Parallels VM on M-series host as of
the 0.6/0.7 prototype runs).  Specific CPU/RAM stamp pinned in
`tests/run_bestagon_perf.sh` when written.

**Build:** all gates apply to **prod build only** (`./trix.opt`,
built via `./build.sh --optimized`).  Debug build is 10-30×
slower per `feedback_debug_build_slow.md`; debug-build measurements
are not gate-relevant.

**Measurement op:** `time { ... } -- ulong` (microseconds elapsed).
Each gate runs 5+ iterations and reports `min / median / max`.
The **median** is the gate value; spikes don't fail CI.

**Harness invocation:** `./trix examples/bestagon/main.trx
--benchmark=NAME --headless` runs the named module's perf loop
and prints structured output:

```
benchmark: mapgen
  scenario: tutorial      iters: 10  min: 41 ms  med: 48 ms  max: 56 ms  gate: 100 ms  PASS
  scenario: standard      iters: 10  min: 172 ms  med: 188 ms  max: 215 ms  gate: 400 ms  PASS
  scenario: epic          iters: 10  min: 402 ms  med: 435 ms  max: 478 ms  gate: 1000 ms  PASS
benchmark: ai-v2
  ...
```

Exit code: 0 = all PASS, 1 = at least one gate fail.

---

#### 12.2 Per-module gates

| Module | Scope | Measured (Phase 0.6/0.7) | Gate | Headroom | Source |
| --- | --- | --- | --- | --- | --- |
| mapgen | Tutorial | 48 ms | **100 ms** | 2.1× | `proto-mapgen.trx` 0.7b |
| mapgen | Standard | 188 ms | **400 ms** | 2.1× | `proto-mapgen.trx` |
| mapgen | Epic | 435 ms | **1000 ms** | 2.3× | `proto-mapgen.trx` |
| ai-v2 (per faction per turn) | Standard | 591 ms | **1500 ms** | 2.5× | `proto-ai-v2-perf.trx` 0.6D |
| ai-v2 (per faction per turn) | Epic | 864 ms | **2000 ms** | 2.3× | `proto-ai-v2-perf.trx` |
| fog (bbox, full recompute) | Standard | 9.8 ms | **25 ms** | 2.5× | `proto-fog-perf.trx` 0.6E |
| fog (bbox, full recompute) | Epic | 8.7 ms | **25 ms** | 2.9× | `proto-fog-perf.trx` |
| pathfind (reachable-hexes per unit) | Epic | TBD | **5 ms** | TBD | `proto-pathfind.trx` 0.6G — measure Phase 1 |
| capture-tick (per buildings) | Epic | TBD | **1 ms** | TBD | `proto-capture.trx` 0.6H — measure Phase 1 |
| scenario load (parse + validate) | any | TBD | **50 ms** | TBD | `proto-loader.trx` 0.6F — measure Phase 1 |
| bws-save (autosave, ~60 KB) | any | TBD | **100 ms** | TBD | §10 estimated — measure Phase 1 |
| bws-load (startup deserialize + slot-bind) | any | TBD | **200 ms** | TBD | §10 estimated — measure Phase 1 |
| combat-resolve-attack (one attack) | any | TBD | **2 ms** | TBD | derived from `bestagon-sim/` — measure Phase 1 |
| render-frame (full back-buffer rebuild) | Epic | TBD | **16 ms** | TBD | §8.1 estimated 15 ms — measure Phase 1 |

**Phase 1 measurement plan:** every TBD row gets a benchmark
harness in `tests/run_bestagon_perf.sh` during the first end-to-end
integration milestone.  Gates marked TBD are proposed targets; if
Phase 1 measurement comes in over target, either tune the
implementation or raise the gate (with rationale committed).

---

#### 12.3 Composite gates

Whole-system end-to-end measurements that compose the per-module
gates above:

| Composite | Target | Composition |
| --- | --- | --- |
| Scenario startup (cold) | **1500 ms** Epic | `require` graph (~300 ms est.) + mapgen Epic (1000 ms) + actor spawn + first render (200 ms est.) |
| Cold load from `.bws` | **500 ms** Epic | `require` graph (~300 ms) + bws-load (200 ms) — no mapgen on load path |
| Player faction turn wall-clock | **200 ms** Standard | fog (25 ms) + render-frame (16 ms) + autosave (100 ms) + invariant per-turn (5 ms) + slack (54 ms) |
| Player faction turn wall-clock | **400 ms** Epic | same composition; doubled budget for larger map |
| AI faction turn wall-clock | **2000 ms** Epic | ai-v2 think (2000 ms) — animation runs in parallel on render-actor |
| Render frame (animating, full rebuild) | **33 ms** Epic | §6.2 frame budget; §8.1 actual ~15 ms |
| `--invariants=all` overhead per turn | **5 ms** Epic | §11.5 budget |

**Critical UX guarantee:** the player NEVER waits more than
**400 ms** for their own input to resolve (Standard), or 800 ms
on Epic (player turn × 2 buffer).  AI thinking time is acceptable
to player (frozen frame, matches AW / Wargroove convention per
§6.5).

---

#### 12.4 Memory gates

| Container / scenario | Gate | Notes |
| --- | --- | --- |
| Global VM peak (Tutorial post-mapgen, mid-game) | **< 1.5 MB** | with `--vm-size=2M` per §13.2 |
| Global VM peak (Standard) | **< 3 MB** | `--vm-size=4M` |
| Global VM peak (Epic) | **< 6 MB** | `--vm-size=8M` |
| Local VM peak per turn (during AI think) | **< 500 KB** | reclaimed by save brackets per §1 B' |
| `.bws` file size (autosave Epic) | **< 100 KB** | per §10 estimate of ~60 KB |
| Scratch arena depth (peak) | **< 256** | within `--scratch-depth=256` Standard / `512` Epic |

**Measurement:** `vm-global-info` op for global VM; `vm-size` op
for individual containers; file-stat for `.bws`.

**Runtime warning** (per §13.2): scenario load emits warning if
global VM exceeds 70% of `--vm-size`.  This is a SOFT warning
for player; CI hardness is the gate above.

---

#### 12.5 Enforcement mechanism

**CI gate:** `tests/run_bestagon_perf.sh` invoked from
`tests/run_all.sh` (or a separate `make perf` target).  Failure of
any gate → CI red.  Run on every PR that touches `examples/bestagon/`,
`src/`, or `lib/`.

**Tolerance:** measured median may exceed gate by up to **10%**
without CI failure (single-run noise floor).  Repeat-runs above the
gate are real regressions.

**Developer workflow:** before pushing a perf-sensitive change, run
`./trix examples/bestagon/main.trx --benchmark=NAME --headless`
locally and confirm gates still pass.  Three TDD-wide perf-sensitive
areas: combat-resolve / pathfind / ai-think.

**Historical tracking:** `tests/perf-history.json` (one record per
commit) appended by `run_bestagon_perf.sh` on green builds.  Enables
"when did X get slower" diagnosis.

---

#### 12.6 Hardware scaling

Gates calibrated for dev/CI hardware (modern laptop class).  Players
on slower hardware (5-year-old laptop ≈ 2× slower) experience:

- Mapgen Epic: ~870 ms (still under 1 s perceptual threshold).
- AI turn Epic: ~1700 ms (still under the 2 s gate).
- Player turn Epic: ~800 ms — still snappy enough for turn-based.
- Scenario startup Epic: ~3 s — borderline but acceptable for a
  one-time load.

**Minimum spec policy:** "any Linux/macOS laptop made in the last
5 years."  If a gate measurement on dev hardware uses more than
50% of the player-turn UX budget, escalate — slower hardware will
miss the UX guarantee.

No `--low-perf-mode` flag for v1.  Players who need it run
smaller scenarios.

---

#### 12.7 Regression detection

**Per-commit:** CI runs `--benchmark=all`; appends to
`tests/perf-history.json`:

```json
{
  "commit": "abc1234",
  "date": "2026-05-16T12:34:56Z",
  "host": "ci-runner-1",
  "build": "trix.opt",
  "measurements": {
    "mapgen.tutorial.median_ms": 48,
    "mapgen.standard.median_ms": 188,
    "mapgen.epic.median_ms": 435,
    "ai-v2.standard.median_ms": 591,
    ...
  },
  "gates_failed": []
}
```

**Diagnosis** when a regression appears:

1. `tests/perf-bisect.sh GATE` runs git-bisect over the last N green
   commits to find the regressor.
2. Compare profile output (`time` proc) between the bad commit and
   parent.

**Gate-raise policy** (i.e., "the gate was too tight"):

- Raising a gate requires: (a) a measurement showing the new
  baseline is stable across 3 runs, (b) a commit note explaining
  why the regression is acceptable (algorithmic change, new feature,
  trade-off), (c) update to the gate table in this section.
- Gates **cannot** be raised silently as part of an unrelated PR.

---

#### 12.8 Out of scope (v1)

- **GPU-bound rendering** — terminal output isn't GPU-bound.
- **Network latency** — no multiplayer per GDD §21.
- **Cache miss / branch prediction tuning** — premature for
  Trix-level optimization; if needed, address in Trix engine,
  not gameplay code.
- **Profiling overlay in-game** — would skew the measurement; use
  `--benchmark=` modes.

---

**Cross-refs:** Appendix A (raw prototype numbers — gates here
match those plus headroom); §6.2 (frame budget 33 ms);
§8.1 (render-frame ~15 ms estimate); §10.5 (bws-save 100 ms
target); §11.5 (invariant overhead 5 ms); §13 (`--benchmark=`,
`--headless`); `feedback_debug_build_slow.md` (10-30× debug
penalty — gates apply to prod build only);
`feedback_dont_defer_predictable_optimization.md` (CS-first-
principles gates beat "measure-first" timidity).

---

### 13. CLI & debug surface — **LOCKED, 2026-05-16**

#### 13.1 Invocation

```
./trix [TRIX-ENGINE-FLAGS] examples/bestagon/main.trx [BESTAGON-FLAGS]
```

Trix consumes flags BEFORE the filename; everything after is delivered
to `command-line-args` (docs/trix-reference.md §3.22) for Bestagon to
parse.  The `--` separator is **not required** — `command-line-args`
returns the post-filename tail directly.  We use `--` in
documentation only, as a visual cue.

---

#### 13.2 Trix engine flags Bestagon depends on

These are consumed by Trix, not Bestagon, but the documentation /
launcher scripts must surface them:

| Flag                | Recommended value                         | Notes                                   |
| ------------------- | ----------------------------------------- | --------------------------------------- |
| `--vm-size=BYTES`   | `2M` Tutorial / `4M` Standard / `8M` Epic | Default `1M` insufficient for Standard+ |
| `--scratch-depth=N` | `128` / `256` / `512`                     | Default `128` ok for Tutorial           |

Other Trix flags (`--operand-depth=`, `--exec-depth=`, etc.) use
defaults.  `--save-depth=64` is plenty since we keep save brackets
shallow (§1 B').

A startup check in `scenario.trx` reads `vm-global-info` and warns
if global-VM usage after scenario load exceeds 70% of the configured
`--vm-size` — common cause of confusing late-game failures.

---

#### 13.3 Bestagon flags

Required (exactly one of):

| Flag | Type | Purpose |
| --- | --- | --- |
| `--scenario=NAME-OR-PATH` | str | Start new game.  NAME resolves under search dirs (`./examples/bestagon-scenarios/`, `./scenarios/`, `$BESTAGON_SCENARIOS_DIR`); PATH used as-is if it contains `/` |
| `--load=PATH` | str | Load a `.bws` save file (§10) |

Game options:

| Flag              | Default          | Purpose                                                      |
| ----------------- | ---------------- | ------------------------------------------------------------ |
| `--seed=N`        | scenario `/seed` | Override master seed.  Ignored (with warning) when `--load=` |
| `--save-dir=PATH` | `./saves/`       | Save directory; auto-created                                 |
| `--no-autosave`   | autosave on      | Disable end-of-turn autosave (§10.5)                         |
| `--colors=MODE`   | `auto`           | `auto` / `256` / `16` / `mono`                               |

Dev / debug:

| Flag | Default | Purpose |
| --- | --- | --- |
| `--cheats` | off | Load `debug.trx`; enable in-game cheat keys (reveal-map, set-hp, give-cash, ...) |
| `--ai-v2` | use v1 | Load `ai_v2.trx`; AI factions use logic-programmed v2 |
| `--invariants=LIST` | `none` | Run §19 invariants per turn.  `all` / `none` / comma list (`capture,fog,unit-position`) |
| `--replay-determinism-check` | off | Run scenario twice with all-AI factions; compare turn-boundary state hashes (§9.6) |

Headless modes (no tty; one-shot; exit after):

| Flag | Default | Purpose |
| --- | --- | --- |
| `--benchmark=NAME` | off | Run perf harness, exit.  `mapgen` / `ai` / `fog` / `path` / `capture` / `all` |
| `--tests` | off | Run `selftest.trx` (§11); exit with 0 on PASS, non-zero on FAIL |

`--benchmark` and `--tests` imply no actors, no screen, no input
reading — they call into the relevant module directly and print
results.

Standard:

| Flag        | Purpose                              |
| ----------- | ------------------------------------ |
| `--help`    | Print usage and exit                 |
| `--version` | Print Bestagon Wars version and exit |

Reserved (not implemented in v1):

| Flag                   | Purpose                           | Reason reserved |
| ---------------------- | --------------------------------- | --------------- |
| `--export-replay=PATH` | Read `--load=` save, write `.bwr` | v2 (§10.10)     |
| `--load-replay=PATH`   | Replay a `.bwr` file              | v2              |
| `--ai-think-budget=MS` | Override AI per-turn budget       | post-1.0 polish |

---

#### 13.4 Flag conflicts and validation

Resolved at CLI-parse time in `config.trx`; fail-fast with a clear
error before any module beyond `config` runs:

| Combination | Resolution |
| --- | --- |
| Neither `--scenario=` nor `--load=` | Error: "one of `--scenario=` or `--load=` is required" |
| Both `--scenario=` and `--load=` | Error: "specify exactly one of `--scenario=` or `--load=`" |
| `--seed=` with `--load=` | Warning: "`--seed=` ignored when loading a save"; continue |
| `--benchmark=` with `--scenario=` / `--load=` | Warning: "gameplay flags ignored in `--benchmark` mode"; continue |
| `--tests` with anything else | Warning: "gameplay flags ignored in `--tests` mode"; continue |
| `--invariants=` with unknown invariant name | Error: "unknown invariant '<name>' — known: capture, fog, …" |
| `--colors=foo` (unknown mode) | Error: "unknown `--colors` mode '<foo>' — try auto / 256 / 16 / mono" |
| Unknown flag | Error: "unknown flag '<flag>'"; print `--help`; exit non-zero |

Validation lives in `config-validate` (in `config.trx`); called once
right after CLI parse.

---

#### 13.5 Naming conflicts with Trix flags

Trix consumes a `--debug` flag (interactive debugger).  Bestagon
deliberately uses `--cheats`, not `--debug`, to avoid confusion if
a user types `./trix examples/bestagon/main.trx --debug` and is
surprised when the debugger doesn't open (Bestagon would parse it
as a script-side flag — and reject it as unknown unless we also
adopted `--debug`).

The other Trix flag namespace (`--vm-size`, `--scratch-depth`, etc.)
is positional — they only work BEFORE the filename, so Bestagon
can use any post-filename flag name without ambiguity.

---

#### 13.6 In-game debug overlay (when `--cheats` is on)

`debug.trx` registers extra input-actor key bindings.  Locked
subset for v1:

| Key      | Action                                                                      |
| -------- | --------------------------------------------------------------------------- |
| `Ctrl-R` | Toggle reveal-map (disable fog locally)                                     |
| `Ctrl-H` | Open "set HP" prompt on cursor unit                                         |
| `Ctrl-M` | Add 10000 cash to current faction                                           |
| `Ctrl-P` | Fill CO power to max for current faction                                    |
| `Ctrl-K` | Kill unit under cursor                                                      |
| `Ctrl-N` | Advance turn (skip current faction)                                         |
| `Ctrl-S` | Set state-hash overlay on (display turn-boundary hash for replay debugging) |

All bind to `debug-*` ops in `debug.trx`.  None of these are
available without `--cheats`.

---

#### 13.7 `--help` text (canonical)

```
Bestagon Wars — terminal hex wargame
Usage: ./trix [TRIX-FLAGS] examples/bestagon/main.trx [BESTAGON-FLAGS]

Trix engine flags (recommended; consumed BEFORE the filename):
  --vm-size=2M|4M|8M       Tutorial / Standard / Epic scenarios
  --scratch-depth=128|256|512

Required (one of):
  --scenario=NAME-OR-PATH       Start a new game from scenario
  --load=PATH                   Load a saved game (.bws)

Game options:
  --seed=N                      Override scenario master seed
  --save-dir=PATH               Save directory (default: ./saves/)
  --no-autosave                 Disable end-of-turn autosave
  --colors=auto|256|16|mono     Colour mode (default: auto)

Dev / debug:
  --cheats                      Enable cheat keys (Ctrl-R reveal map, etc.)
  --ai-v2                       Use AI v2 instead of v1
  --invariants=LIST             Run self-test invariants per turn
                                  (all|none|<comma-separated list>)
  --replay-determinism-check    Verify deterministic replay; exit

Headless / one-shot (no tty):
  --benchmark=mapgen|ai|fog|path|capture|all   Run perf harness; exit
  --tests                       Run selftest.trx; exit with PASS/FAIL

Standard:
  --help                        This text
  --version                     Show Bestagon Wars version

Examples:
  ./trix --vm-size=2M examples/bestagon/main.trx --scenario=tutorial
  ./trix --vm-size=4M examples/bestagon/main.trx --load=saves/quicksave.bws
  ./trix --vm-size=8M examples/bestagon/main.trx --scenario=epic-archipelago --seed=42
  ./trix examples/bestagon/main.trx --benchmark=all --vm-size=8M
```

---

#### 13.8 `config.trx` surface

The parser produces a `/config` Dict in global VM at startup, with
these keys (all present, default-filled):

| Key                     | Type        | Default                                                            |
| ----------------------- | ----------- | ------------------------------------------------------------------ |
| `/config-mode`          | Name        | `/play` / `/load` / `/benchmark` / `/tests` / `/help` / `/version` |
| `/config-scenario-path` | String?     | null unless mode=`/play`                                           |
| `/config-load-path`     | String?     | null unless mode=`/load`                                           |
| `/config-seed-override` | ULong?      | null = use scenario seed                                           |
| `/config-save-dir`      | String      | `./saves/`                                                         |
| `/config-autosave?`     | Boolean     | true                                                               |
| `/config-colors`        | Name        | `/auto`                                                            |
| `/config-cheats?`       | Boolean     | false                                                              |
| `/config-ai-v2?`        | Boolean     | false                                                              |
| `/config-invariants`    | Array<Name> | `[]`                                                               |
| `/config-replay-check?` | Boolean     | false                                                              |
| `/config-benchmark`     | Name?       | null                                                               |
| `/config-headless?`     | Boolean     | derived (true if mode=`/benchmark` or `/tests`)                    |

Module-prefixed accessor ops per §5.3 (e.g.,
`config-autosave?`, `config-cheats?`, `config-vm-size`).

---

**Cross-refs:** `docs/trix-reference.md` §3.22 (`command-line-args`);
`./trix --help` (engine flags); §1 (no `--save` flag-equivalent —
saves go via in-game `F5`, not CLI); §4 (no actors in headless modes);
§9 (`--seed=`, `--replay-determinism-check`); §10 (`--load=`,
`--save-dir=`, `--no-autosave`, `.bws` / `.bwr` extensions);
§11 (`--tests`, `--invariants=`); §12 (`--benchmark=` perf
gates).

---

### 14. Modding hooks — **LOCKED, 2026-05-16**

**Decision:** **v1 modding surface = scenario files only.**  Players
author `.bw` files per GDD §16; scenarios can override the rules
defined in GDD §20 ("Scenario-overridable surface").  Everything
else is out of scope per GDD §21 (mod loader, custom DSL for units
/ COs, campaign support).

**In-scope v1:**

| Hook | Mechanism | Reference |
| --- | --- | --- |
| Custom maps | `.bw` scenario file with explicit `/terrain` grid OR `/procgen-spec` | GDD §16, §16.8 |
| Starting roster | `/units` array in scenario | GDD §16 |
| Building placement | `/buildings` array in scenario | GDD §16, §16.6 |
| Faction count and COs | `/factions` array (selecting from built-in COs) | GDD §16, §8 |
| Initial cash / income / weather rules | per-scenario fields | GDD §16, §20 |
| Capture speed multipliers | scenario can scale GDD §10 rates | GDD §20 |
| Animation speed multiplier | `/animation-speed` field | GDD §16.6, §6.3 |
| Self-test invariants for scenario validity | §11 harness via scenario.trx's `scen-validate` | TDD §11, GDD §16.6 |

**Explicitly OUT of scope for v1** (GDD §21):

- Custom unit types beyond the §6 roster.
- Custom CO classes / new CO power abilities.
- Lua / Python / external-DSL scripting embedded in scenarios.
- Campaign chains (scenario → next-scenario branching).
- Persistent stats across scenarios (XP carry-over, fleet wagering).
- Map editor GUI.
- Mod loader / `.mod` archive format.

**v2 candidates** noted for future TDD revision; no architecture
hooks reserved beyond what's already in `.bws` (§10.10 has `.bwr`
reserved for replay, not modding).

**Security note** — scenario files ARE full Trix source (GDD §16
intro: "scenarios are full Trix-source").  Loading a hostile `.bw`
file gives the author full Trix capability.  v1 mitigation:
**don't load untrusted scenarios.**  No sandbox, no syscall
filtering.  If post-v1 demand emerges, Trix's `--sandbox` flag is
the lever (disables filesystem / raw-memory ops) — could ship a
`--sandbox-scenarios` Bestagon flag that internally requires
sandbox mode.

---

## Tier 4 — Risks (resolved 2026-05-16)

### 15. Cross-platform terminal support — **LOCKED 2026-05-16**

**Decision:** **Linux + macOS primary for v1; Windows is best-effort,
not tested.**

| Platform | v1 status |
| --- | --- |
| Linux (xterm / kitty / alacritty / urxvt / Linux console) | Primary; CI runs here |
| macOS (Terminal.app / iTerm2 / Ghostty) | Primary; manual testing |
| Windows Terminal (modern ConPTY) | Best-effort; runs if Trix builds + screen ops behave; no test coverage |
| Windows legacy console (cmd.exe / pre-ConPTY) | Unsupported |
| SSH / tmux sessions on any of the above | Supported; common dev environment |

**What "best-effort" means for Windows v1:**

- No advertisement in README.
- No `windows-` branch in build scripts.
- If players run on Windows Terminal and it works: great; we accept
  bug reports but don't gate fixes on Windows-specific debugging.
- Trix engine portability concerns (raw-mode, ANSI escapes, ConPTY)
  fall to Trix maintainers; Bestagon doesn't work around them.

**v2 candidate:** explicit Windows Terminal support after Trix's
own Windows story matures.

---

### 16. Long-session memory & GC cadence — **LOCKED 2026-05-16**

**Decision:** **`vm-global-gc` runs at scenario boundaries only.**
No per-turn or threshold-triggered GC for v1.

**Rationale:**

- Persistent state mutates in place (§1 B'); no per-turn churn.
- Dominant garbage source: killed-unit Dicts.  ~1-5 deaths per turn
  in heavy battle × ~150 B per unit Dict = under 1 KB/turn garbage.
- Over a 50-turn game: ~50 KB of dead unit Dicts.  Three orders of
  magnitude below the §12.4 memory gates (1.5–6 MB).
- `vm-global-gc` cost is non-trivial (one mark+sweep of the global
  region); running it every turn is overhead-without-benefit.

**Where GC actually runs:**

| Trigger                           | Site                     | Why                                      |
| --------------------------------- | ------------------------ | ---------------------------------------- |
| Scenario load completes           | `scenario.trx` post-load | Reclaim any setup scratch                |
| Game-actor receives `/quit`       | game-actor shutdown      | Clean exit measurement                   |
| `--benchmark=` or `--tests` start | harness setup            | Stable baseline                          |
| `/VMFull` raised during play      | top-level error handler  | Best-effort recovery before erroring out |

The `/VMFull` recovery path: top-level handler in `main.trx` catches
`/VMFull`, runs one `vm-global-gc`, retries the failing op.  If still
`/VMFull` after GC: print "out of memory; restart with larger
--vm-size=NM" + crash-dump per §17, exit non-zero.  This makes GC a
**recovery mechanism**, not a routine sweep.

**If Phase 1 reveals pathological accumulation:** add
`--gc-cadence=NUMBER-OF-TURNS` flag.  Out of scope for v1 unless
measured to be needed.

---

### 17. Crash recovery & autosave — **LOCKED 2026-05-16**

**Decision:** **On uncaught error, write `crash.bws` via the
already-locked §10 save format; print recovery hint; exit non-zero.
No auto-resume.**

#### 17.1 Crash flow

```trix
% main.trx — top-level error handler
{ run-game } {
    % An uncaught error reached the top of the stack.
    /err-msg status local-def

    % Try to save a crash dump.  Best-effort; failures here go to stderr.
    {
        config-save-dir /crash.bws path-join /crash-path local-def
        crash-path save-game                     % §10.3 save op
        (Bestagon crashed.) stderr write-line
        (Error: ) err-msg concat stderr write-line
        (Crash dump saved to: ) crash-path concat stderr write-line
        (Resume with: ./trix examples/bestagon/main.trx --load=) crash-path concat
                                                 stderr write-line
    } {
        (Bestagon crashed AND crash-save failed.) stderr write-line
        (Error: ) err-msg concat stderr write-line
        (Game state could not be preserved.) stderr write-line
    } try

    1 exit-code
} try
```

#### 17.2 State at the moment of error

Because §1 B' wraps each turn's compute in `save { ... } restore`,
any error inside the bracket triggers Trix's normal restore-on-error
behaviour — local-VM scratch is reclaimed.  After `restore`, global
VM reflects the **last successful between-brackets state**, which is
exactly what `save-game` serialises.

**Net result:** player loses at most one mid-turn's worth of cursor
movements / selection actions.  No half-applied mutations.  No
inconsistent state.

#### 17.3 What gets dumped

Exactly the §10.2 schema — same `.bws` format, same loader path.
No special "crash format."  This means `crash.bws` is loadable by
the player and by Phase 1's developers identically: it's just another
save file.

#### 17.4 No auto-resume

After a crash, Bestagon exits.  The player decides:

- "I'll never play this build again" — they delete `crash.bws`.
- "I want my game back" — they relaunch with
  `--load=saves/crash.bws`.

Auto-resume risks an infinite loop if the same error fires
immediately on the next turn.  Player-decides is safer and
respects the player's agency.

#### 17.5 Errors during crash handling

If `save-game` itself errors during crash dump (disk full, etc.),
print "crash AND crash-save failed" to stderr.  Exit non-zero.
No further recovery — at this point the only safe action is to
abort cleanly.

#### 17.6 Out of scope for v1

- **Telemetry / crash reporting upload** — no network calls.
- **Multiple crash dumps with timestamped filenames** — single
  `crash.bws` overwritten.
- **Auto-bisect last-known-good** — manual save-slot management
  is the recovery story.

---

**Cross-refs:** GDD §16 (scenario format), §20 (overridable
surface), §21 (out of scope); §10 (save format used for crash
dumps); §12.4 (memory gates that pre-empt GC need); §13
(`--sandbox-scenarios` v2 flag — reserved); `feedback_no_stl_heap_containers.md`
(memory discipline supports the no-GC-routine choice).

---

## Appendix A — Prototype-derived perf numbers (frozen)

These are the prod-build (`./trix.opt`) numbers from the Phase 0.6/0.7
de-risk prototypes.  They are ship gates; regressions block release.

| Prototype                    | Tutorial | Standard | Epic   | Source                 |
| ---------------------------- | -------- | -------- | ------ | ---------------------- |
| mapgen (terrain + buildings) | 48 ms    | 188 ms   | 435 ms | `proto-mapgen.trx`     |
| AI v2 (per faction)          | n/a      | 591 ms   | 864 ms | `proto-ai-v2-perf.trx` |
| Fog (bbox method)            | n/a      | 9.8 ms   | 8.7 ms | `proto-fog-perf.trx`   |
| Loader (scenario load)       | TBD      | TBD      | TBD    | `proto-loader.trx`     |
| Pathfind (per unit)          | TBD      | TBD      | TBD    | `proto-pathfind.trx`   |
| Capture tick                 | TBD      | TBD      | TBD    | `proto-capture.trx`    |

Numbers marked TBD will be measured during Phase 1 integration.

---

## Appendix B — Open decisions log

Track each OPEN item by section + brief.  Resolve in this document
before Phase 1 main coding starts on the affected module.

| # | Section | Decision | Owner | Status |
| --- | --- | --- | --- | --- |
| 1 | §1 | Save bracket policy (A/B/C/B') | user | **RESOLVED 2026-05-15: B'** |
| 2 | §2 | Global-VM strategy | user | **RESOLVED 2026-05-15 (downstream of #1): mostly global** |
| 2a | §2.1 | VM-size defaults vs. hard caps | user | **RESOLVED 2026-05-16: soft warnings (one-line stderr at scenario load if under-sized), no hard launcher refusal, no upper cap; CI memory gates enforce hardness; bin thresholds locked Tutorial≤450 / Standard 451-2700 / Epic ≥2701 cells** |
| 3 | §3.1 | Cell representation | user | **RESOLVED 2026-05-15: A-SoA** |
| 4 | §3.2 | Unit indexing scheme | user | **RESOLVED 2026-05-15: /units Dict + /faction-units index** |
| 4a | §3.3 | Faction state shape | user | **RESOLVED 2026-05-15: flat SoA arrays** |
| 5 | §4 | Actor count (3 vs. more) | user | **RESOLVED 2026-05-16: 3 actors + ai-worker affordances reserved** |
| 6 | §5 | Module layout flat vs. proposed | user | **RESOLVED 2026-05-16: flat dir, require-only, module-prefix names, public-surface header** |
| 7 | §6 | Frame model (1/2/3) | user | **RESOLVED 2026-05-16: adaptive — IDLE state polls at 500 ms for cursor blink, ANIMATING state runs 30 Hz; /skip-animations on player keypress; game-actor async-queues animations, blocks via /sync-animations before next input** |
| 8 | §7 | Keybindings | user | **RESOLVED 2026-05-16: input-actor emits symbolic /key Name + mods (no mode awareness); game-actor mode-dispatches via per-mode key→handler table; 10 modes; GDD §18.7 4-way keys + Y/U/B/N (vim) and Q/E/Z/C (numpad) for 6-way diagonals** |
| 9 | §8 | Full-redraw vs dirty-rect | user | **RESOLVED 2026-05-16: full back-buffer rebuild per frame; screen-render handles tty-write minimization; layered composition; cursor-follow viewport with 2-hex scroll margin; animator owned by render-actor; palette ladder 256→16→mono via $TERM auto-detect** |
| 10 | §9 | Seed strategy + replay contract | user | **RESOLVED 2026-05-16: per-system slots, bit-exact at turn boundary** |
| 11 | §10 | Save format (A/B/C) | user | **RESOLVED 2026-05-16: custom .bws — 16-byte header + to-binary-token of game-state Dict; ~60 KB target; format-versioned with migrators; .bwr reserved for v2 (omits map/units, adds /command-log)** |
| 12 | §11 | Invariants CLI vs build-time | user | **RESOLVED 2026-05-16: CLI-controlled with 3 modes — `--tests` one-shot CI, `--invariants=LIST` per-turn during play, `--replay-determinism-check` for §9.6; 4 scope tiers; 3 invariant patterns** |
| 13 | §12 | Ship-gate perf numbers | user | **RESOLVED 2026-05-16: 13 per-module gates (7 measured + 6 TBD for Phase 1) + 6 composite gates + 6 memory gates; 5-iter median; 10% tolerance; CI hardness; tests/perf-history.json tracking; gate-raise policy** |
| 14 | §13 | CLI flag scope | user | **RESOLVED 2026-05-16: Trix-vs-Bestagon split at filename; required scenario-or-load; --cheats not --debug; --benchmark/--tests imply headless; full /config Dict shape** |
| 15 | §14 | Modding scope for v1 | user | **RESOLVED 2026-05-16: scenarios only (full Trix-source); 8 in-scope hooks via GDD §20 overrides; 7 explicit out-of-scope (GDD §21); security via don't-load-untrusted; v2 `--sandbox-scenarios` reserved** |
| 16 | §15 | Windows support | user | **RESOLVED 2026-05-16: Linux + macOS primary; Windows Terminal best-effort (no CI, no advertising), legacy console unsupported, SSH/tmux supported** |
| 17 | §16 | Long-session GC cadence | user | **RESOLVED 2026-05-16: scenario-boundary only (load + quit) + /VMFull recovery path; no per-turn or threshold trigger; --gc-cadence flag reserved if Phase 1 reveals need** |
| 18 | §17 | Crash-recovery policy | user | **RESOLVED 2026-05-16: write crash.bws via §10 save op + print recovery hint + exit non-zero; no auto-resume; single overwriteable slot; no telemetry** |

## End
