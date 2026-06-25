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

# Reactive Cells: Technical Reference

Layer 5 of the Trix durable process runtime.

---

## 1. Overview

Reactive cells provide **incremental computation** with automatic
dependency tracking and invalidation.  A cell is a named value that
knows which other cells depend on it.  When a cell changes, all
dependent cells are automatically marked dirty and recomputed on
demand.

Two kinds of cell:

| Kind              | Created by      | Value source                  | Updated by                   |
| ----------------- | --------------- | ----------------------------- | ---------------------------- |
| **Base cell**     | `cell`          | Explicit value                | `cell-set`                   |
| **Computed cell** | `cell-computed` | Proc (re-evaluated on demand) | Automatic (when deps change) |

The reactive model is **pull-based with push invalidation**:

- **Push**: when a base cell changes via `cell-set`, dirty flags propagate
  eagerly through all transitive dependents (DFS).
- **Pull**: dirty computed cells re-evaluate lazily -- only when read
  via `cell-get`.  Unread dirty cells never recompute.

Watchers bridge push and pull: a proc registered on a cell that fires
after `cell-set` completes, enabling push-based side effects on top of
the pull-based evaluation model.

### Why Reactive Cells in Trix

| Without cells                         | With cells                                 |
| ------------------------------------- | ------------------------------------------ |
| Manual dependency tracking            | Automatic dependency recording             |
| Explicit invalidation (`thunk-reset`) | Automatic transitive invalidation          |
| Easy to miss an edge                  | Impossible to miss (recorded at eval time) |
| No notification on change             | Watchers fire on state transitions         |
| No batching                           | `batch` coalesces multiple updates         |

Reactive cells complete the durable process runtime:

```
Snap-shot/Thaw -------- persistence
Reactive Cells -------- incremental computation
Logic/Backtracking ---- reasoning
Supervision ----------- fault tolerance
Actors ---------------- communication
Pipelines ------------- concurrent dataflow
Coroutines ------------ concurrency primitive
```

Every layer composes with every other.  A supervised actor can maintain
a reactive cell graph, use logic programming for constraint validation,
feed results into a pipeline, and survive a snap-shot/thaw cycle.

---

## 2. Architecture

### 2.1 Type::Cell

Cell is a dedicated Object type (slot 26 of 32).  The Object payload
is a `vm_offset_t` pointing to a `CellHeader` struct on the VM heap.

```
Object (8 bytes)
+-------+-------+--------+----------+
| aat   | save  | length | m_cell   |
| 1B    | 1B    | 2B     | 4B       |
+-------+-------+--------+----------+
  type=Cell (11010)        offset to CellHeader
  IgnoresExecute
  PushOpDirect
```

No PackedType -- cells are runtime-created mutable objects, never
literals in packed procedures or arrays.

### 2.2 CellHeader

Every cell, base or computed, has a CellHeader on the VM heap:

```
CellHeader (56 bytes)
+---------------------+---------------------+
| m_value (8B)        | m_proc (8B)         |  Objects
+---------------------+---------------------+
| m_deps (4B)         | m_rdeps (4B)        |  vm_offset_t
+---------------------+---------------------+
| m_watchers (4B)     | m_deps_len (2B)     |  vm_offset_t, length_t
+---------------------+---------------------+
| m_deps_cap (2B)     | m_rdeps_len (2B)    |  length_t
+---------------------+---------------------+
| m_rdeps_cap (2B)    | m_watchers_len (2B) |  length_t
+---------------------+---------------------+
| m_watchers_cap (2B) | m_debug_name (4B)   |  length_t, vm_offset_t
+---------------------+---------------------+
| m_validator (8B)    | m_dirty (1B)        |  Object, flag
+---------------------+---------------------+
| m_base (1B)         | (pad to 56)         |  flag
+---------------------+---------------------+
```

| Field        | Base cell                 | Computed cell                           |
| ------------ | ------------------------- | --------------------------------------- |
| `m_value`    | Current value             | Cached result (null if dirty)           |
| `m_proc`     | null                      | The compute proc                        |
| `m_deps`     | nulloffset                | Cells this cell reads (rebuilt on eval) |
| `m_rdeps`    | Cells that read this cell | Cells that read this cell               |
| `m_watchers` | Watcher procs             | Watcher procs                           |
| `m_dirty`    | Always 0                  | 1 if invalidated, 0 after eval          |
| `m_base`     | 1                         | 0                                       |

### 2.3 Dependency Graph

Dependencies are stored as **per-cell arrays** on the VM heap:

- **`m_deps`** (forward edges): cells this cell reads during evaluation.
  Only meaningful for computed cells.  Rebuilt from scratch on each
  re-evaluation.
- **`m_rdeps`** (reverse edges): cells that depend on this cell.
  Updated incrementally: `cell-get` adds, re-evaluation cleans up stale
  entries.

Both arrays grow dynamically (initial capacity 4, doubled on overflow).
The rdeps array uses a duplicate check on insertion.

```
Base cell A             Computed cell B           Computed cell C
+-----------+           +-----------+             +-----------+
| m_rdeps:  |           | m_rdeps:  |             | m_deps:   |
| [B, C]    |           | [C]       |             | [A, B]    |
+-----------+           +-----------+             +-----------+
      |                       |                         |
      +--------> B reads A    +--------> C reads B      |
      +--------> C reads A    <----------+              |
                              C reads B  <--------------+
```

### 2.4 Dependency Recording

A member variable `vm_offset_t m_current_cell` tracks which computed
cell is currently being evaluated.  Default: `nulloffset` (not
evaluating).

When `cell-get` reads a cell:
1. If `m_current_cell != nulloffset`, add `m_current_cell` to the read
   cell's `m_rdeps` array (the evaluating cell depends on the read cell).
2. When the trampoline evaluation completes (`@cell-eval`), the control
   operator also records the dependency for the outer evaluating cell.

`cell-value` skips dependency recording entirely -- it reads without
creating an edge.

### 2.5 Member Variables

| Variable | Type | Default | Purpose |
| --- | --- | --- | --- |
| `m_current_cell` | `vm_offset_t` | `nulloffset` | Recording scope |
| `m_batch_depth` | `integer_t` | `0` | Nesting depth of `batch` |
| `m_deferred_watchers` | `vm_offset_t` | `nulloffset` | BatchEntry array for deferred watcher firing during batch |
| `m_deferred_watcher_count` | `length_t` | `0` | Number of valid entries in deferred watcher array |

All four are serialized in SnapShotHeader (SNAPSHOT_VERSION 178).

---

## 3. Operators

### 3.1 cell

```
value cell -- cell
```

Create a base cell with initial value.

**Implementation**: Allocates a CellHeader via `vm_alloc_ptr<CellHeader>`.
Sets `m_value = value.make_copy(csl)`, `m_base = 1`, `m_dirty = 0`.
All array fields initialized to `nulloffset` with zero lengths.

**Cost**: 56 bytes (CellHeader) + 8 bytes (Object on stack).

### 3.2 cell-computed

```
proc cell-computed -- cell
```

Create a computed (derived) cell.  Accepts `Array`, `Packed`, or `Curry`
as the proc argument.  Starts dirty -- first `cell-get` triggers evaluation.

**Implementation**: Same allocation as `cell`.  Sets `m_proc = proc`,
`m_base = 0`, `m_dirty = 1`, `m_value = null`.

**Why Curry is accepted**: Trix has no lexical closures.  A proc like
`{ x cell-get }` captures the *name* `x`, not the *value*.  Inside a
loop, `x` may be overwritten.  `curry` captures the actual cell object:
```
my-cell { cell-get } curry cell-computed
```
This creates a proc that pushes `my-cell` (the Object, not the name)
then runs `cell-get`.

### 3.3 cell-get

```
cell cell-get -- value
```

Read a cell's value.  If the cell is a dirty computed cell, triggers
re-evaluation via trampoline.  If inside a computed cell evaluation,
records a dependency edge.

**Clean path** (not dirty, or base cell): returns `m_value` directly.
O(1) plus one rdeps-append if recording.

**Dirty path** (computed cell, `m_dirty == 1`):

1. Clear old deps (remove self from each dep's rdeps).
2. Push trampoline on exec stack:
   ```
   Exec: ... | dep_flag | prev_current_cell | cell_offset | @cell-eval | proc
   ```
3. Set `m_current_cell = cell_offset`.
4. Pop cell from op stack, return to interpreter.
5. Interpreter evaluates proc; proc calls `cell-get` on dependencies,
   which records edges via `cell_record_dep`.
6. `@cell-eval` fires: caches result, clears dirty, restores
   `m_current_cell`, records dep for outer cell if nested.

**Nesting**: when computed cell C1 reads dirty computed cell C2,
`cell-get` enters a nested trampoline.  `m_current_cell` is saved on
the exec stack and restored by `@cell-eval`.  Arbitrary nesting depth
is supported, limited by exec stack capacity (~400 levels on default
2048-entry exec stack).

### 3.4 cell-set

```
value cell cell-set --
```

Update a base cell's value.  Errors on computed cells (`undefined-result`).

**Implementation**:

1. Journal old value via `Save::save_object` pattern (same as `put_op`
   and `lvar_bind`).
2. Write new value: `m_value = new_value.make_copy(csl)`.
3. Collect cells with watchers (pre-invalidation, to capture old values).
4. Invalidate dependents: eager transitive DFS via `cell_invalidate`.
5. Fire watchers (if `m_batch_depth == 0`):
   - For each cell with watchers, push `old_value`, `new_value` on
     operand stack, push watcher proc on exec stack.
   - Watchers execute sequentially on the current coroutine.

**Save/restore**: the value write is journaled.  `restore` reverts the
cell to its pre-save value.  Dirty flags on dependents are NOT journaled
(spurious dirty is harmless -- re-evaluation produces the restored value).

### 3.5 cell-value

```
cell cell-value -- value
```

Read a cell's value **without** recording a dependency.  If the cell is
a dirty computed cell, triggers re-evaluation (same trampoline as
`cell-get`), but passes a dep-flag of 0 so the `@cell-eval` control op
does not record the *caller's* reverse-dependency edge (`m_current_cell`
is still pointed at this cell so its own proc's reads rebuild its
deps/rdeps).

**Use case**: a computed cell that needs to read another cell for a
one-time check without re-triggering when that cell changes.

```
/config 42 cell def
/snapshot { config cell-value } cell-computed def
% snapshot evaluates once; changes to config do NOT invalidate snapshot
```

### 3.6 is-cell

```
any is-cell -- bool
```

Type predicate.  Returns true if the operand is a Cell.

### 3.7 cell-dirty?

```
cell cell-dirty? -- bool
```

Returns true if the cell has been invalidated but not yet recomputed.
Base cells are never dirty (always returns false for base cells).

### 3.8 watch

```
cell proc watch --
```

Register a change watcher on a cell.  The watcher proc fires after
`cell-set` completes (deferred -- after all invalidation has propagated).

When fired, the watcher receives two values on the operand stack:

```
... old-value new-value    (new-value on top)
```

The watcher proc must consume both values.

**Implementation**: appends `proc` to the cell's `m_watchers` array
(grows dynamically, initial capacity 4).

### 3.9 unwatch

```
cell proc unwatch --
```

Remove a watcher.  Matches by `Object::equal`.  Idempotent -- no error
if the proc is not found.

### 3.10 batch

```
proc batch --
```

Execute `proc` with watcher firing deferred.  Multiple `cell-set` calls
within the batch do not fire watchers until the batch proc completes.
At batch end, each cell that had a net value change fires its watchers
exactly once with (pre-batch old value, final new value).  Cells whose
value returned to its pre-batch state (set then unset) do not fire.

**Implementation**: For the outermost `batch`, allocates a `BatchEntry[]`
array (`MaxWatcherCollect` entries) in the top-end temp region
(`vm_temp_alloc`) to avoid fragmenting the main heap.  The saved temp
pointer is pushed on the exec stack as a UInteger below `@batch-end`.
Exec layout: `[saved_temp_offset] [@batch-end] [proc]` (outermost) or
`[@batch-end] [proc]` (nested).  During the batch, `cell_set_op` calls
`cell_defer_watcher` to record `{cell_offset, old_value}` pairs with
deduplication (keeps first old_value per cell).  Nested batches share
the outer batch's deferred array; watchers fire only when `m_batch_depth`
returns to zero.  At batch end, `vm_temp_restore` reclaims the array.
On error, `@batch-fail` frees deferred ExtValues, restores the temp
region, and re-raises.

### 3.11 cell-update

```
proc cell cell-update --
```

Atomic read-modify-write on a base cell.  Reads the current value, runs
`proc` (`value -- value'`) on it, and stores the result through the same
journaled, watcher-firing, validator-checked path as `cell-set`.  Errors
on computed cells (`undefined-result`); on a validated cell a rejected
result raises `require`.

```
/counter 10 cell def
{ 5 add } counter cell-update
counter cell-get =                 % -> 15
```

### 3.12 Derived Cells: cell-map, cell-transduce, cell-combine

Three constructors build a **computed** cell from one or more source
cells, wiring the dependency edges automatically:

```
cell    proc  cell-map        -- cell'    % apply proc to the source value
cell    xf    cell-transduce  -- cell'    % apply a transducer to the source array
[cells] proc  cell-combine    -- cell'    % apply proc to all source values
```

- **`cell-map`** derives `proc(source-value)` and re-evaluates whenever
  the source changes.
- **`cell-transduce`** applies a transducer `xf` to the source cell's
  array value (see `transducers.md`).
- **`cell-combine`** reads every cell in the array, pushes their values
  in order, then runs `proc` over them.  An empty array raises
  `range-check`.

```
/src 7 cell def
/doubled src { 2 mul } cell-map def
doubled cell-get =                 % -> 14
21 src cell-set
doubled cell-get =                 % -> 42  (auto-recomputed)

/a 3 cell def  /b 4 cell def
/total [ a b ] { add } cell-combine def
total cell-get =                   % -> 7

/nums [ 1 2 3 4 5 6 ] cell def
/evens nums { 2 mod 0 eq } xf-filter cell-transduce def
evens cell-get ==                  % -> [2i 4i 6i]
```

Each derived cell is an ordinary computed cell: `cell-set` / `cell-update`
on it raise `undefined-result`.

### 3.13 cell-validated

```
value proc cell-validated -- cell
```

Create a **base** cell whose value is guarded by a validator proc
(`value -- bool`).  The validator runs on creation and on every
`cell-set` / `cell-update`.  A rejected value raises `require`, and the
cell keeps its previous value.

```
/age 5 { 0 ge } cell-validated def
age cell-get =                     % -> 5
{ -1 age cell-set } try            % -> /require   (rejected; age stays 5)
```

### 3.14 Graph Introspection: cell-deps, cell-rdeps, cell-watchers

```
cell cell-deps     -- array     % forward edges: cells this cell reads
cell cell-rdeps    -- array     % reverse edges: cells that read this cell
cell cell-watchers -- array     % watcher procs attached to this cell
```

Each returns a fresh array snapshot of the corresponding per-cell list.
`cell-deps` is meaningful only for computed cells (a base cell reads
nothing).  Useful for inspecting dependency graphs.

```
/src 1 cell def
/derived src { 1 add } cell-map def
derived cell-get pop               % force evaluation so the edge exists
derived cell-deps length =         % -> 1   (reads src)
src cell-rdeps length =            % -> 1   (read by derived)
```

### 3.15 Debug Names: cell-set-name, cell-name

```
name cell cell-set-name --          % attach a diagnostic name
cell cell-name -- name true | false % read it (false if unnamed)
```

Attach a human-readable name to a cell for diagnostics and `print`
output.  `cell-name` pushes the name then `true`, or just `false` if the
cell was never named.

```
/temperature 20 cell def
/temp-celsius temperature cell-set-name
temperature cell-name =            % -> true   (name /temp-celsius is below it)
```

### 3.16 cell-dispose

```
cell cell-dispose --
```

Disconnect a cell from the dependency graph: remove it from every
dependency's rdeps, clear its own deps / rdeps / watcher arrays, and
release its value.  The handle remains a Cell Object but is no longer
wired into the graph.  After disposal the cell is marked disposed; any
subsequent `cell-get`, `cell-set`, `cell-value`, `cell-update`, or
`watch` on it raises `/invalid-access`.  `cell-dispose` itself is
idempotent.  **Irreversible across `restore`** -- a disposed cell is not
resurrected by rolling back to a save level taken before the dispose.

```
/scratch 99 cell def
scratch cell-dispose               % detach + free
```

### 3.17 Control Operators

The reactive subsystem registers 9 control operators:

| Operator | Fires when | Action |
| --- | --- | --- |
| `@cell-eval` | Computed proc completes | Cache result, clear dirty, restore `m_current_cell`, record dep for outer cell |
| `@cell-eval-fail` | Computed proc errors | Restore `m_current_cell`, re-raise via `trx->error()` (cell stays dirty) |
| `@batch-end` | Batch proc completes | Decrement `m_batch_depth`; if zero: fast path fires watchers directly when no dirty computed cells remain; otherwise pushes a recompute trampoline + `@batch-fire` which fires them after recomputation |
| `@batch-fail` | Batch proc errors | Decrement `m_batch_depth`; if zero, free deferred ExtValues and clear array; re-raise via `trx->error()` |
| `@batch-fire` | Dirty computed cells recomputed (post-`@batch-end`) | Sweep deferred array, fire watchers for cells with net change, reclaim the temp region |
| `@batch-fire-fail` | Error during post-batch recompute/fire | Free deferred ExtValues, restore temp region, re-raise |
| `@cell-update-done` | `cell-update` proc completes | Store the proc's result through the journaled, watcher-firing, validator-checked path |
| `@cell-validate-init` | `cell-validated` validator returns | If accepted, create the validated cell; else raise `require` |
| `@cell-validate-set` | A validated cell's `cell-set` validator returns | If accepted, write the value; else raise `require` (cell keeps prior value) |

**Barrier integration**: `@cell-eval`, `@batch-end`, and `@batch-fire`
are recognized by `try_catch_handler` as exec stack barriers.  On error,
they are replaced with their fail variants, and the exec stack is cut
back.

**Exec stack layout** (cell-get trampoline):

```
... | dep_flag_int | prev_current_cell_int | cell_offset_int | @cell-eval | proc
```

`@cell-eval` pops `cell_offset`, `prev_current_cell`, and `dep_flag`
(three integers below the barrier on exec stack).  `@cell-eval-fail`
does the same before re-raising.

---

## 4. Invalidation

### 4.1 Eager Transitive DFS

When `cell-set` updates a base cell, invalidation walks the `m_rdeps`
array recursively:

```
cell_invalidate(cell_offset, depth):
  if depth > MaxInvalidateDepth (64): error(LimitCheck)
  for each dep_offset in cell.m_rdeps:
    dep = header(dep_offset)
    if dep.m_dirty == 0:
      dep.m_dirty = 1
      cell_invalidate(dep_offset, depth + 1)
```

**Key properties**:

- Already-dirty cells are skipped (no redundant work).
- Diamond dependencies (C depends on both A and B) cause C to be
  marked dirty once from the first path; the second path skips it.
- Depth limit prevents infinite loops from cyclic dependency graphs.

### 4.2 Cycle Detection

Cyclic dependencies (A reads B reads A) are not prevented at
construction time.  They are caught at invalidation time by the depth
limit (`MaxInvalidateDepth = 64`).  A cyclic graph that exceeds this
limit raises `Error::LimitCheck`.

Cyclic evaluation is also bounded: if `cell-get` on A triggers
evaluation of A's proc, which calls `cell-get` on B, which triggers
B's proc, which calls `cell-get` on A -- A is still dirty (its proc
is still running), so `cell-get` enters a second trampoline for A.
This exhausts the exec stack, producing `execstack-overflow`.

**Recommendation**: avoid cyclic dependencies.  They are a design
error, not a supported pattern.

### 4.3 Double Invalidation

If cell C depends on both A and B, and both A and B are updated
(e.g., in a batch), C is marked dirty by the first invalidation and
skipped by the second.  C re-evaluates once on the next `cell-get`.

### 4.4 Watcher Collection

Before invalidation, `cell-set` collects all downstream cells with
watchers and their current (pre-invalidation) values.  After
invalidation completes, watchers fire with `old-value new-value`
on the operand stack.

Collection is deduplicated (each cell's watchers fire at most once)
and bounded (`MaxWatcherCollect = 64`).

---

## 5. Programming Patterns

### 5.1 Configuration with Derived Values

Base cells for raw config, computed cells for derived settings.

```
% Raw configuration
/port        (8080) cell def
/host        (localhost) cell def
/debug       false cell def

% Derived configuration
/base-url {
    (http://) host cell-get concat
    (:) concat
    port cell-get concat
} cell-computed def

/log-level {
    debug cell-get { 0 } { 2 } if-else
} cell-computed def

% Read derived values
base-url cell-get =          % -> http://localhost:8080
log-level cell-get =         % -> 2

% Update raw config -- derived values auto-invalidate
(9090) port cell-set
base-url cell-get =          % -> http://localhost:9090 (recomputed)

true debug cell-set
log-level cell-get =         % -> 0 (recomputed)
```

**Cost**: 56 bytes per cell + arrays for deps/rdeps.  For 5 config
cells with 3 computed derivatives (8 cells), total ~500 bytes.

### 5.2 Spreadsheet-Style Computation

The classic reactive pattern: cells that depend on other cells,
forming a computation graph.

```
% Input cells
/price    100.0d cell def
/quantity 5 cell def
/tax-rate 0.08d cell def

% Computed cells
/subtotal {
    price cell-get quantity cell-get /double-type cast mul
} cell-computed def

/tax {
    subtotal cell-get tax-rate cell-get mul
} cell-computed def

/total {
    subtotal cell-get tax cell-get add
} cell-computed def

% Read
total cell-get =             % -> 540

% Update price -- subtotal, tax, total all recompute
150.0d price cell-set
total cell-get =             % -> 810

% Batch update: change price and quantity together
{
    200.0d price cell-set
    10 quantity cell-set
} batch
total cell-get =             % -> 2160
```

**Dependency graph**:
```
price -----> subtotal -----> tax -----> total
quantity -->            tax-rate -->       ^
                                          |
                        subtotal ---------+
```

### 5.3 Form Validation

Input cells for form fields, computed cells for validation rules,
watchers for feedback.

```
/username (  ) cell def
/password (  ) cell def

/username-valid {
    username cell-get length 3 ge
} cell-computed def

/password-valid {
    password cell-get length 8 ge
} cell-computed def

/form-valid {
    username-valid cell-get
    password-valid cell-get
    and
} cell-computed def

% Watcher for validation feedback
form-valid {
    pop  % old value
    { (Form is valid) = } { (Form has errors) = } if-else
} watch

% Simulate user input
(alice) username cell-set       % triggers validation
(secret123) password cell-set   % see note below (computed-cell watcher)
```

### 5.4 Self-Invalidating Cache

A computed cell is a cache that automatically invalidates when its
inputs change.

```
% Expensive computation (simulated)
/input-data [ 1 2 3 4 5 6 7 8 9 10 ] cell def

/processed {
    input-data cell-get
    { 2 mul } map
    { 10 gt } filter
    sum
} cell-computed def

% First read: computes
processed cell-get =         % -> 80 (12+14+16+18+20)

% Second read: cached, no recomputation
processed cell-get =         % -> 80 (instant)

% Update input: cache auto-invalidates
[ 5 10 15 20 ] input-data cell-set
processed cell-get =         % -> 90 (recomputed: 20+30+40, since 10 is filtered out by 10 gt)
```

**Key advantage over manual `thunk-reset`**: you never forget to
invalidate.  The dependency is recorded automatically during evaluation.

### 5.5 Monitoring and Alerting

Compose reactive cells with watchers for monitoring.  In a supervised
actor system, watchers can send messages to alert handlers.

```
% Metrics (updated by sensor actors)
/cpu-load    0.0d cell def
/memory-used 0 cell def
/request-rate 0 cell def

% Derived health indicators
/system-healthy {
    cpu-load cell-get 0.9d lt
    memory-used cell-get 900000 lt and
    request-rate cell-get 10000 lt and
} cell-computed def

% Alert watcher
system-healthy {
    /new-val exch def
    pop  % old value
    new-val not {
        (ALERT: system unhealthy!) =
    } if
} watch

% When metrics update, health auto-recomputes
0.95d cpu-load cell-set         % see note below (computed-cell watcher)
0.5d cpu-load cell-set          % (no alert)
```

> **Note -- watching a computed cell.** A `watch` on a *computed* cell only
> fires with a fresh value if (a) the computed cell has been read at least once
> (`cell-get`) before the watch, so its dependency edges exist, **and** (b) the
> triggering `cell-set` runs inside a `batch` (the batch end recomputes dirty
> computed cells before firing watchers). Outside a batch, a computed-cell
> watcher receives the pre-recompute (stale) cached value. For immediate
> notification, `watch` a **base** cell (as in 5.7) instead.

### 5.6 Data Transformation Chain

A pipeline of computed cells, each transforming the output of the
previous.  Unlike a concurrent pipeline (Layer 1), this is single-
threaded and demand-driven.

```
/raw-data (  alice:30,bob:25,charlie:35  ) cell def

% Stage 1: parse
/parsed {
    raw-data cell-get trim
    (,) split
} cell-computed def

% Stage 2: transform -- split each "name:age" into a [name age] pair
/transformed {
    parsed cell-get
    { (:) split } map
} cell-computed def

% Stage 3: filter -- keep ages >= 30
/filtered {
    transformed cell-get
    { 1 get to-number 30 ge } filter
} cell-computed def

% Read final result
filtered cell-get =
% Update source -- entire chain recomputes on next read
(dave:40,eve:22) raw-data cell-set
filtered cell-get =
```

**When to use this vs pipelines (Layer 1)**: reactive chains are
synchronous and single-threaded.  Use them when the computation is
fast and you want automatic invalidation.  Use Layer 1 pipelines
when stages are slow and benefit from concurrent execution with
backpressure.

### 5.7 State Machine with Derived Properties

Base cell for state, computed cells for derived properties.

```
/state /idle cell def

/is-running {
    state cell-get /running eq
} cell-computed def

/is-idle {
    state cell-get /idle eq
} cell-computed def

/status-message {
    state cell-get
    /idle eq { (System idle) }
    { state cell-get /running eq
      { (Processing...) }
      { (Error state) }
      if-else
    } if-else
} cell-computed def

% Watcher logs transitions
state {
    /new exch def /old exch def
    (State: ) print old = ( -> ) print new =
} watch

/running state cell-set   % State: idle -> running
status-message cell-get = % Processing...
```

### 5.8 Sensor Gateway (Full-Stack Demo)

This pattern composes all 5 layers: reactive cells for thresholds,
actors for processing, supervision for fault tolerance, pipelines for
data flow, and logic for anomaly diagnosis.

```
% Layer 5: Reactive threshold (adjustable at runtime)
/threshold 100.0d cell def
/alert-count 0 cell def
/alert-active {
    alert-count cell-get 0 gt
} cell-computed def

% Alert watcher
alert-active {
    /new exch def pop
    new { (ALERT: threshold breached!) = } if
} watch

% Layer 4: Logic-based anomaly classification
/classify-anomaly {
    % reading on stack
    /reading exch def
    /cause logic-var def
    [
        { reading threshold cell-get 2.0d mul gt
          guard  /critical cause unify guard }
        { reading threshold cell-get gt
          guard  /warning cause unify guard }
    ] choice
    cause deref
} def

% Update threshold at runtime -- all dependent logic re-evaluates
50.0d threshold cell-set
```

---

## 6. Cost Model

### 6.1 Memory

| Item                                       | Cost                          |
| ------------------------------------------ | ----------------------------- |
| CellHeader                                 | 56 bytes                      |
| Cell Object                                | 8 bytes                       |
| rdeps array (per entry)                    | 4 bytes                       |
| deps array (per entry)                     | 4 bytes                       |
| watcher array (per entry)                  | 8 bytes                       |
| Minimum cell (no deps, no watchers)        | 64 bytes                      |
| Typical computed cell (3 deps, 0 watchers) | 64 + 12 + 12 = 88 bytes       |
| Cell with 1 watcher                        | 64 + 8 + overhead = ~96 bytes |

**Array overhead**: arrays allocate with initial capacity 4 and double
on overflow.  A cell with 3 rdeps uses a 4-slot array (16 bytes) with
1 slot unused.

**Graph of 10 cells, 15 edges**: ~10 * 64 + 15 * 4 * 2 = ~760 bytes
for the cells, plus ~120 bytes for dep/rdep array overhead.  Under 1 KB
total.

### 6.2 CPU

| Operation                | Cost                                           |
| ------------------------ | ---------------------------------------------- |
| `cell` / `cell-computed` | O(1) -- single allocation                      |
| `cell-get` (clean)       | O(1) -- read + optional rdeps append           |
| `cell-get` (dirty)       | O(proc) -- re-evaluation of compute proc       |
| `cell-set`               | O(E) -- DFS over edges in rdeps graph          |
| `cell-value`             | Same as `cell-get` minus rdeps append          |
| `watch` / `unwatch`      | O(W) -- append or scan watcher array           |
| `batch`                  | O(1) -- increment counter + exec stack push    |
| Invalidation             | O(V + E) -- vertices + edges in dirty subgraph |

### 6.3 When NOT to Use Cells

- **High-frequency updates**: if a base cell is updated thousands of
  times per second and has many dependents, the invalidation DFS runs
  on every update.  Use `batch` to coalesce, or consider a different
  architecture.
- **Large dependency graphs**: graphs with hundreds of cells and complex
  dependency patterns consume significant heap space and invalidation
  time.  For graphs > 50 cells, consider topological ordering
  (future optimization).
- **Simple one-shot computation**: if a value is computed once and
  never changes, use a thunk, not a cell.  Thunks have zero
  dependency overhead.
- **Concurrent data processing**: for streaming data through stages
  with backpressure, use Layer 1 pipelines.  Cells are synchronous
  and single-threaded.

---

## 7. Design Decisions

### D1: New Type vs Thunk Extension

**Decision**: dedicated `Type::Cell` (slot 26).

**Rationale**: thunks are "compute once, cache forever until explicit
reset."  Base cells are "mutable value that notifies dependents."
Overloading thunks would require special-casing base cells (no proc),
attaching metadata meaningless for non-cell thunks, and conflating
`thunk-reset` with reactive propagation.  A new type gives clean
dispatch, clean `verify_operands`, and no semantic leakage.

**Cost**: 1 type slot.  31 of the 32 possible type values are now
assigned; 1 slot remains.

### D2: Per-Cell Arrays vs Centralized Graph

**Decision**: per-cell `m_deps` and `m_rdeps` arrays.

**Rationale**: a centralized adjacency structure fights the VM heap
model and adds complexity for no clear benefit.  Per-cell arrays are
local, simple, and support incremental updates.  The rdeps-only
alternative (Option C) saves one array per cell but creates a cleanup
problem: without a deps list, stale rdeps entries accumulate as
memory leaks.

### D3: Member Variable for Recording

**Decision**: `vm_offset_t m_current_cell` member variable.

**Rationale**: exec stack marker (Option B) requires O(exec depth)
scan on every `cell-get`.  The member variable is O(1) -- one null
check per `cell-get`.  Nesting is handled by saving/restoring
`m_current_cell` via the `@cell-eval` control operator.

**SnapShotHeader cost**: 4 bytes.

### D4: Eager Transitive Invalidation

**Decision**: DFS from changed cell, marking all transitive dependents
dirty immediately.

**Rationale**: lazy marking (Option B) adds overhead to every `cell-get`
(must check if any dep is dirty).  Topological propagation (Option C)
requires depth tracking and re-sorting on graph changes.  For Trix's
scale (tens of cells, not thousands), eager DFS is simple, correct, and
the "double invalidation" case is cheap (skip already-dirty cells).

### D5: Deferred Watcher Execution

**Decision**: watchers fire after invalidation completes, not during.

**Rationale**: inline watcher firing (Option A) allows reentrancy --
a watcher calling `cell-set` would corrupt the invalidation traversal.
Coroutine-based watchers (Option B) add allocation pressure and
non-determinism.  Deferred execution ensures propagation is fully
complete before any watcher runs.  `batch` extends the deferral window.

### D6: A Complete First-Release Operator Set

**Decision**: the first release shipped 10 orthogonal operators --
`cell`, `cell-computed`, `cell-get`, `cell-set`, `cell-value`, `is-cell`,
`cell-dirty?`, `watch`, `unwatch`, `batch`.

**Rationale**: `cell-value` (read without dep) is essential -- without
it, there's no way to read a cell without creating a dependency edge.
All 10 were small, orthogonal, and served distinct use cases.  The set
has since grown to 21 standard operators -- derived cells, validation,
graph introspection, naming, and disposal (see sections 3.11-3.16).

---

## 8. Layer Interactions

### 8.1 Save/Restore

| Operation              | Journaled? | Mechanism                        |
| ---------------------- | ---------- | -------------------------------- |
| `cell-set` value write | Yes        | `Save::save_object` on `m_value` |
| Dirty flag changes     | No         | Spurious dirty is harmless       |
| Deps/rdeps arrays      | No         | Rebuilt on re-evaluation         |
| Watcher arrays         | No         | Structural metadata              |

**Behavior after restore**: cell value reverts.  Dependent cells may
be spuriously dirty -- they re-evaluate and produce the correct
(restored) result.  One wasted re-evaluation, then clean.

### 8.2 Snap-shot/Thaw

CellHeaders, deps/rdeps/watcher arrays, and Cell Objects on stacks
are captured as part of the VM heap blob.  Four member variables
(`m_current_cell`, `m_batch_depth`, `m_deferred_watchers`,
`m_deferred_watcher_count`) are serialized in SnapShotHeader (+14 bytes).

After thaw, the dependency graph is intact.  Dirty cells re-evaluate
on next `cell-get`.  Watchers fire normally on subsequent `cell-set` calls.

### 8.3 Actors and Supervision

Cells are shared-heap objects -- all actors in the same VM can read
and write the same cells.  This enables patterns like:

- **Shared configuration**: base cells for config, updated by a config
  actor, read by worker actors via `cell-get`.
- **Health monitoring**: metric cells updated by sensor actors, health
  computed cells read by supervisor actors.
- **State synchronization**: a cell that aggregates state from multiple
  actors, with watchers that notify coordinators.

**Caution**: cells are not thread-safe across VMs.  Within a single
VM's cooperative scheduler, only one coroutine runs at a time, so
no data races occur.

### 8.4 Logic/Backtracking

Cells and logic variables are orthogonal.  A cell can hold a logic
variable as its value.  `cell-set` inside a `choice` alternative is
journaled -- backtracking reverts the cell value (but dirty flags
on dependents persist as spurious, harmless).

### 8.5 Pipelines

Cells and pipelines serve different models:

|           | Cells                                    | Pipelines                               |
| --------- | ---------------------------------------- | --------------------------------------- |
| Execution | Synchronous, demand-driven               | Concurrent, backpressure-driven         |
| Updates   | Pull (recompute on read)                 | Push (data flows through stages)        |
| State     | Persistent (cells live until GC/restore) | Transient (pipeline runs to completion) |
| Use case  | Derived values, config, caching          | Streaming data processing               |

A cell can feed a pipeline (a watcher that writes to a pipe buffer),
and a pipeline can update cells (a terminal stage that calls `cell-set`).

---

## 9. Limitations and Constraints

### 9.1 No Topological Ordering

Invalidation uses simple DFS without topological sorting.  In a
diamond dependency (C depends on A and B, both updated), C may be
marked dirty twice (the second is a no-op skip).  For large graphs,
topological ordering would eliminate redundant marking, but the
implementation complexity is not justified at Trix's typical scale.

### 9.2 Heap Fragmentation from Array Churn

When a computed cell re-evaluates and its dependency set changes,
old deps/rdeps arrays are abandoned on the VM heap (no GC).  For
long-running programs with frequently changing dependency sets, this
causes slow heap fragmentation.

**Mitigation**: most computed cells have stable dependency sets (they
read the same cells every evaluation).  The stable-deps case requires
no array reallocation.  Save/restore reclaims abandoned arrays within
a save scope.

### 9.3 Cycle Detection is Depth-Limited

Cyclic dependencies are detected by `MaxInvalidateDepth` (64) during
invalidation and by exec stack exhaustion during evaluation.  There is
no static analysis to prevent cycles at construction time.

### 9.4 Batch Watcher Deferral

`batch` defers watcher firing until the outermost batch completes.
During the batch, `cell_set_op` records each cell with watchers into
a `BatchEntry[]` array (`m_deferred_watchers`) with its pre-batch
value.  Deduplication keeps only the first old_value per cell.

At `@batch-end`, the deferred array is swept: for each entry, the
cell's current `m_value` is compared with the stored old_value.
Cells with a net change fire their watchers once with `(old, new)`.
Cells whose value returned to its pre-batch state are skipped.

The deferred array is bounded by `MaxWatcherCollect` (64 entries).
Excess entries are silently dropped.  On error (`@batch-fail`),
deferred ExtValues are freed and the array is discarded without
firing watchers.

### 9.5 Watcher Execution Order

Watchers on the same cell fire in registration order.  Watchers on
different cells fire in DFS traversal order (determined by rdeps array
order).  This order is deterministic but not specified -- do not depend
on it.

### 9.6 Equality-Based Skip

`cell-set` short-circuits on equality: if the new value equals the cached
value (via `Object::equal`), the set is a **no-op** -- no invalidation, no
watcher firing, no journal entry (the standard MobX/SolidJS behavior).  Only
a set to a genuinely different value propagates to dependents and watchers.

### 9.7 Exec Stack Depth for Deep Chains

Each level of computed cell nesting during evaluation consumes 5 exec
stack entries (dep_flag, prev_current_cell, cell_offset, `@cell-eval`,
proc).  A chain of 20 computed cells requires ~100 entries.  The default
exec stack (2048 entries) supports chains of ~400 cells.  Deeper chains
require a larger exec stack via construction-time Config (`--exec-depth`).

---

## 10. The Big Picture: Layers 1-5

Reactive cells are the final layer of the durable process runtime.
Each layer adds a capability that compounds with every layer below it.

```
+-----------------------------------------------+
|         Snap-shot / Thaw                       |
|  Entire VM serialized/restored to bytes        |
+-----------------------------------------------+
|                                                |
|  Layer 5: Reactive Cells         (21 ops)      |
|  cell, cell-computed, cell-get, cell-set, watch, batch |
|  Incremental computation, dep tracking         |
|                                                |
|  Layer 4: Logic / Backtracking   (19 ops)      |
|  logic-var, unify, deref, choice, guard, fail  |
|  Constraint solving, search                    |
|                                                |
|  Layer 3: Supervision            (18 ops)      |
|  monitor, link, supervisor                     |
|  Fault isolation, restart policies             |
|                                                |
|  Layer 2: Actors                 (18 ops)      |
|  actor-spawn, actor-send, actor-recv           |
|  Message passing, selective receive            |
|                                                |
|  Layer 1: Pipelines              (22 ops)      |
|  pipe-buffer, pipe-put, pipe-get, pipe-run     |
|  Bounded channels, backpressure                |
|                                                |
+-----------------------------------------------+
|  Existing Primitives                           |
|  Coroutines, lazy-seq, records, tagged,        |
|  curry/thunk, TCO, save/restore, try-catch     |
+-----------------------------------------------+
```

### 10.1 Layer Composition Table

Every pair of layers composes meaningfully:

| Composition | What it enables |
| --- | --- |
| Cells + Actors | Shared reactive state across communicating actors |
| Cells + Supervision | Self-monitoring systems (health cells, alert watchers, auto-restart) |
| Cells + Logic | Rule engine that reacts to state changes (cell-set triggers choice evaluation) |
| Cells + Pipelines | Reactive cells feeding data into concurrent pipelines via watchers |
| Cells + Save/Restore | Transactional cell updates (cell-set reverts on restore) |
| Cells + Snap-shot/Thaw | Dependency graph persists across crashes and restarts |
| Actors + Supervision | Self-healing concurrent systems (Erlang/OTP pattern) |
| Actors + Pipelines | Actor-based data processing with backpressure |
| Logic + Actors | Agents that reason about messages |
| Logic + Save/Restore | Backtracking search with automatic state rollback |
| All 5 + Snap-shot | **Durable process runtime**: fault-tolerant, reactive, intelligent, crash-recoverable |

### 10.2 Full-Stack Architecture

A production system using all 5 layers:

```
                    +-------------------+
                    | Snap-shot/Thaw    |  periodic checkpoint
                    +-------------------+
                            |
              +-------------+-------------+
              |                           |
     +-----------------+         +-----------------+
     | Supervisor      |         | Reactive Cells  |
     | (Layer 3)       |         | (Layer 5)       |
     |                 |         |                 |
     | /one-for-one    |         | threshold cell  |
     | restart policy  |         | health cell     |
     +-----------------+         | alert watcher   |
              |                  +-----------------+
     +--------+--------+                |
     |                 |                |
+----------+    +----------+    +----------+
| Sensor   |    | Processor|    | Alert    |
| Actor    |    | Actor    |    | Actor    |
| (Layer 2)|    | (Layer 2)|    | (Layer 2)|
|          |    |          |    |          |
| reads HW |    | logic    |    | logs     |
| updates  |    | analysis |    | alerts   |
| metrics  |    | (Layer 4)|    |          |
+----------+    +----------+    +----------+
     |                |
     v                v
+----------+    +----------+
| Pipeline |    | Pipeline |
| (Layer 1)|    | (Layer 1)|
| raw data |    | filtered |
| ingestion|    | output   |
+----------+    +----------+
```

**Data flow**:
1. Sensor actor reads hardware, updates metric base cells via `cell-set`.
2. Health computed cells auto-invalidate and recompute.
3. Alert watcher fires when health status changes, sends message to alert actor.
4. Processor actor uses logic programming to classify anomalies.
5. Supervisor monitors all actors; restarts crashed ones.
6. Periodic snap-shot serializes the entire system for crash recovery.

**Total VM footprint**: ~5 actors (~26 KB) + ~10 cells (~1 KB) +
pipeline buffers (~2 KB) + overhead = ~30 KB of the 256 KB VM.

### 10.3 The 839-Operator Runtime

| Category                                              | Operators |
| ----------------------------------------------------- | --------- |
| Core language (stack, math, string, flow, etc.)       | ~590      |
| Collections (array, set, dict, record, lazy)          | ~88       |
| Pipelines                                             | 22        |
| Actors                                                | 18        |
| Supervision                                           | 18        |
| Logic / Backtracking                                  | 19        |
| Reactive Cells                                        | 21        |
| Composability (protocols, matching, transducers, ...) | ~53       |
| **Total**                                             | **839**   |

All in a single `#include "trix.h"`.  No external dependencies beyond
readline and zlib.  Mark-sweep GC for the global VM region; otherwise
pre-allocated memory and deterministic execution.

Nothing else in the embeddable tier can say that.

---

## 11. Implementation Files

| File | Content |
| --- | --- |
| `src/ops_reactive.inl` | CellHeader, helpers, 21 standard + 9 control operators |
| `src/object.inl` | Type::Cell, m_cell union member, accessors, sm_object_attrib, barrier predicates |
| `src/enums.inl` | 30 SystemName entries (21 standard + 9 control) |
| `src/verify.inl` | VerifyCell mask |
| `src/member_vars.inl` | m_current_cell, m_batch_depth, m_deferred_watchers |
| `src/init.inl` | Initialization |
| `src/snapshot.inl` | 4 SnapShotHeader fields |
| `src/ops_snapshot.inl` | Freeze/thaw |
| `src/ops_system.inl` | try_catch_handler barrier integration |
| `src/dispatch.inl` | 30 dispatch entries (21 standard + 9 control) |
| `src/printfmt.inl` | print_cell method |
| `src/types.inl` | PATCH version bump |
| `tests/test_reactive.trx` | 293 assertions, 169 sections |
