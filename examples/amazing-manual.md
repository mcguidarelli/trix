<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Maze Generation Manual

A teaching reference for the maze zoo built by
[`examples/amazing.trx`](amazing.trx) -- eleven classic maze-generation
algorithms, five grid topologies, distance-field heatmaps, and a PNG encoder
whose file format is assembled in Trix.

This manual is self-contained: read it end to end and you will come away
understanding how each maze algorithm works, what kind of maze it produces and
why, how those algorithms generalize off the square grid, and how ~6,600 lines
of Trix turn the result into a real `.png` on disk. No prior background assumed;
the only Trix-specific idea you need is the [save/restore arena and global
VM](../docs/local-global-vm.md), which shows up where the hot loops touch a
million cells.

`amazing.trx` is a homage to Steve Capps' *Amazing*, the maze toy that shipped
on the 128K Macintosh in 1984.

## Table of Contents

1. [Mazes in 60 Seconds](#1-mazes-in-60-seconds)
2. [The Cell, the Grid, and the Million-Cell Problem](#2-the-cell-the-grid-and-the-million-cell-problem)
3. [The Algorithm Zoo](#3-the-algorithm-zoo)
   - [3.1 Carving family (DFS)](#31-carving-family-dfs)
   - [3.2 Spanning-tree family](#32-spanning-tree-family)
   - [3.3 Row-wise family](#33-row-wise-family)
   - [3.4 Wall-adding family](#34-wall-adding-family)
   - [3.5 Bias at a glance](#35-bias-at-a-glance)
4. [Grid Topologies](#4-grid-topologies)
5. [Distance Fields and Colormaps](#5-distance-fields-and-colormaps)
6. [Overlays: Solve, Braid, Weave](#6-overlays-solve-braid-weave)
7. [Masking: Shape-Carved Mazes](#7-masking-shape-carved-mazes)
8. [The PNG Encoder](#8-the-png-encoder)
9. [Command-Line Reference](#9-command-line-reference)
10. [Implementation Tour](#10-implementation-tour)
11. [Further Reading](#11-further-reading)

---

## 1. Mazes in 60 Seconds

A **perfect maze** is a grid of cells where exactly one path connects any two
cells -- no loops, no isolated regions. In graph terms it is a **spanning tree**
of the grid: every cell reachable, no cycles. All eleven algorithms here produce
perfect mazes; they differ only in *which* spanning tree they pick, and that
choice is what gives each its visual character ("texture" or "bias").

There are two ways to build one:

- **Carving (passage carving).** Start with every wall up and knock walls down
  as you visit cells. The maze grows as a tree of carved passages. Most
  algorithms here carve.
- **Wall adding / joining.** Start with every cell isolated and join cells until
  the grid is one connected tree. Kruskal's is the clean example.

Either way the invariant is the same: add a passage only when it connects two
*not-yet-connected* regions, and you can never form a loop. Break that rule on
purpose and you get a **braided** maze with loops -- see [§6](#6-overlays-solve-braid-weave).

A useful mental split for the zoo:

- **Uniform-spanning-tree (UST)** algorithms (Wilson's, Aldous-Broder) pick each
  possible maze with *equal* probability -- the "fairest" mazes, at a cost in
  speed.
- **Biased** algorithms (everything else) are faster but favor certain shapes --
  long corridors, diagonal drift, river-like winding, and so on.

---

## 2. The Cell, the Grid, and the Million-Cell Problem

Before the algorithms, three implementation facts that recur throughout.

### 2.1 One byte per cell

Each cell is a single byte (Section 5). The low nibble holds the
four walls; the high nibble holds transient flags:

| Bit | Mask   | Meaning                               |
| --- | ------ | ------------------------------------- |
| 0   | `0x01` | wall N                                |
| 1   | `0x02` | wall E                                |
| 2   | `0x04` | wall S                                |
| 3   | `0x08` | wall W                                |
| 4   | `0x10` | weave-under, horizontal (render hint) |
| 5   | `0x20` | weave-under, vertical (render hint)   |
| 6   | `0x40` | VISITED (transient, algorithm-local)  |
| 7   | `0x80` | IN-SOLUTION (set by `--solve`)        |

A fresh square cell is `0x0F` (all four walls up). `carve-wall` clears the wall
bit on *both* the cell and its neighbor, so a passage is always symmetric. Other
topologies reuse the same byte with more wall bits (hex needs six, `0x3F`); the
upsilon octagon needs all eight bits for walls, so it spends a **second byte**
per cell for the VISITED / IN-SOLUTION flags (see [§4](#4-grid-topologies)).

### 2.2 The grid container and the 65,535 cap

A grid is a packed 3-array `[width height rows]` (`grid-w` / `grid-h` / `grid-cells`), where `rows` is
an array of byte-strings, one per row. Trix's `length_t` is a `uint16_t`, so a
single string maxes out at 65,535 bytes -- a flat one-string grid would cap you
at a ~255×255 maze. Storing one string *per row* sidesteps that: a `--monster`
1000×1000 maze is 1,000 row-strings of 1,000 bytes each.

Auxiliary per-cell storage that an algorithm needs (union-find parents, BFS
queues, frontier lists) faces the same cap, so the file provides a
**chunked-array** (Section 4B): a logical array split into chunks
of `CHUNK = 32768` elements, indexed by `(i / CHUNK, i mod CHUNK)`. This is what
lets the spanning-tree algorithms scale past 65,535 cells.

### 2.3 Journaled vs. transient writes

Trix's local arena journals in-place mutations so `restore` can roll them back.
For a maze that writes millions of cells, journaling every write would balloon
the save journal. `amazing.trx` is deliberate about this:

- **`put-persist` / `chunked-put-persist`** (journaled-cold: the write is *not*
  added to the rollback journal) is used only where the data is long-lived and
  hot -- Kruskal's union-find and wall list, Wilson's `in-maze` / `walk-dir`
  arrays, and the BFS distance field.
- **Plain `chunked-put`** (ordinary transient churn) is used for the
  backtracker's stack, Prim's frontier, and Growing-Tree's active set, which are
  scratch structures consumed within the run.

This distinction is a good worked example of the [persist family](../docs/local-global-vm.md#the--persist-family);
watch for it in the per-algorithm notes.

---

## 3. The Algorithm Zoo

All eleven algorithms below run on the **square** grid; seven of them
(recursive-backtracker, Kruskal, Wilson, Aldous-Broder, Prim, Hunt-and-Kill,
Growing Tree) also run on every non-square grid through a shared topology
descriptor (see [§4](#4-grid-topologies)). Eller, binary-tree, sidewinder, and
recursive-division stay square-only. Select one with `--algo NAME`; the default
is `backtrack`. Dispatch is a string-keyed table (`algo-dispatch` /
`dispatch-algo` on square, `td-algos` / `dispatch-algo-td` on the others). On
every grid the seven portable algorithms route through one shared generic
engine (`square-desc` + the `g-*` procs, [§4](#4-grid-topologies)); `algo-dispatch`
keeps bespoke square entries only for `backtrack` (which carries the weave
logic), eller, binary-tree, sidewinder, and division.

Five of the eleven ignore the start cell entirely (`kruskal`, `eller`,
`binary-tree`, `sidewinder`, `division`); the other six begin carving from
`--start`.

### 3.1 Carving family (DFS)

These grow a single tree by walking and backtracking. They tend to produce
**long, winding corridors with few short dead-ends** -- the "twisty little
passages" look.

**recursive-backtracker** (`--algo backtrack`)
The default, and the prettiest general-purpose maze. Depth-first search: from
the current cell, step to a random unvisited neighbor (carving the wall), and
when you hit a dead-end, backtrack to the last cell that still has an unvisited
neighbor. *Implementation:* an explicit iterative DFS -- a `w*h` chunked-array
used as a stack of `[x y dirs dir-idx]` frames, where `dirs` is a freshly
shuffled permutation of the four directions, so there is no recursion-depth
limit. *Texture:* long corridors, low branching, every cell on one snaking
spine. This is also the only algorithm that supports `--weave` ([§6](#6-overlays-solve-braid-weave)).

**hunt-and-kill** (`--algo hunt-kill`)
Same walk as the backtracker, but with no stack. When the walk gets stuck, it
**hunts**: scan the grid row by row for the first unvisited cell that has at
least one visited neighbor, carve into it, and resume the walk there. *Texture:*
similar long corridors to the backtracker but with longer "rivers", because the
hunt restarts deterministically from the top-left rather than backtracking.
*Cost:* the repeated scans make it slower; the payoff is O(1) extra memory.

**growing-tree** (`--algo growing-tree`)
A generalization that subsumes two others. Keep an **active set** of cells; each
step, *pick* one active cell, carve to a random unvisited neighbor (adding it to
the set), and drop a cell from the set when it has no unvisited neighbors left.
The *pick rule* is everything: always-newest behaves exactly like the recursive
backtracker; always-random behaves like Prim's. `amazing.trx` uses a deliberate
**50/50 mix** of newest and random, giving a texture halfway between the two.

### 3.2 Spanning-tree family

These think of the maze as a graph and build a spanning tree directly. Two of
them (Wilson's, Aldous-Broder) are *uniform* -- every possible maze equally
likely.

**Kruskal's** (`--algo kruskal`)
Classic minimum-spanning-tree shape: list every interior wall, shuffle the list,
and remove each wall **only if** the two cells it separates are not already
connected. *Implementation:* a **path-compressed union-find** (`-uf-find`) tracks connectivity; the wall list is a chunked-array of UIntegers,
each wall packed as `(y*w + x)*2 + (east|south)`, shuffled in place with
Fisher-Yates. All union-find and wall writes go through `chunked-put-persist`
(journal-cold) because there are millions of them. *Texture:* lots of short
dead-ends, a "spiky", uniform-looking maze with no global grain.

**Prim's (randomized)** (`--algo prim`)
Grow a tree from a seed by repeatedly connecting a random **frontier** cell (a
not-yet-in cell adjacent to the tree) to a random in-tree neighbor.
*Implementation:* the frontier is a chunked-array used as a stack with O(1)
removal (swap-with-last), backed by a parallel byte-bitmap (`in-frontier`) for
O(1) membership tests so cells are never double-added. This is *random* Prim's
(uniform edge weights), not weighted. *Texture:* very short dead-ends and a
strong "radial" branching look around the seed.

**Wilson's** (`--algo wilson`)
A **uniform** spanning tree via loop-erased random walks. Pick a cell not yet in
the maze, random-walk until you hit the maze, then add the walk's *loop-erased*
path. *Implementation:* two parallel byte arrays -- `in-maze` and `walk-dir`
(the last direction walked out of each cell). Loop erasure is free: revisiting a
cell simply overwrites its `walk-dir`, so when you finally retrace, you follow
the most recent direction out of each cell and the loops vanish. A monotonic
scan cursor finds the next start cell and never backs up. *Texture:* unbiased --
the fairest maze in the zoo. *Cost:* the early walks wander a long time before
the maze is big enough to hit; slow to start, fast to finish.

**Aldous-Broder** (`--algo aldous-broder`)
The other **uniform** algorithm, and the simplest to state: random-walk the
whole grid; whenever you step into an unvisited cell, carve the wall you crossed.
Stop when every cell is visited. *Implementation:* no auxiliary structure at all
-- visited-ness is read from the cell's VISITED bit, and out-of-bounds steps are
re-rolled to keep the neighbor choice uniform (the property that makes it a
*uniform* spanning tree). *Texture:* unbiased, like Wilson's. *Cost:* expected
~`n log n` steps -- the slowest in the zoo, included precisely as the cautionary
"correct but slow" counterpoint to Wilson's.

### 3.3 Row-wise family

These sweep the grid once, top to bottom, with O(width) or O(1) memory. They are
the fastest, at the price of an obvious directional grain.

**binary-tree** (`--algo binary-tree`)
The simplest possible maze: for every cell, carve **either north or east** (coin
flip; the only available option at the edges). One pass, no state. *Texture:* a
hard diagonal bias -- the entire top row is one open corridor, the entire right
column is another, and every cell has an unobstructed path to the top-right.
Great for teaching, ugly as a maze.

**sidewinder** (`--algo sidewinder`)
binary-tree's smarter cousin. Sweep each row left to right building a horizontal
"run"; at each cell, either extend the run east or **close** it -- and when you
close a run, carve north from one *random* cell in the run. *Texture:* removes
the vertical-corridor artifact (only the top row is one long hall), leaving a
characteristic "rising" look. O(1) memory.

**Eller's** (`--algo eller`)
The cleverest of the row algorithms and the one that makes `--monster` possible.
It produces a perfect maze while holding **only two rows in memory at a time**.
Each row, randomly join horizontally-adjacent cells into sets, then for every
set carve at least one passage south into the next row; cells that carved south
inherit their set, the rest start fresh. The last row joins everything to close
the tree. *Implementation:* set membership is two `width`-length id arrays plus a
small dict -- **no union-find**, just an O(width) renumber per row. *Texture:*
similar to a relaxed binary-tree, but its O(1)-in-height memory is the whole
point: this is the algorithm behind `--stress` (200×200) and `--monster`
(1000×1000).

### 3.4 Wall-adding family

The lone generator here that **adds** walls rather than carving them.

**recursive-division** (`--algo division`)
The inverse of every other algorithm. Start from an open chamber (knock down
every interior wall, leaving only the border), then **recursively partition**:
lay one straight wall across the region -- horizontal or vertical, whichever
splits the longer axis -- pierce it with a single random gap, and recurse on the
two halves. Recursion stops once a region is one cell wide in either dimension,
since such a corridor is already acyclic; assembling acyclic regions joined by one
passage each yields a perfect (tree) maze. *Implementation:* an open-the-interior
pass followed by an explicit `w*h` region stack of `[x0 y0 x1 y1]` bounds
(coexisting regions stay pairwise disjoint, so the stack can never overflow), plus
an `-add-wall` helper that is the exact inverse of `carve-wall`. *Texture:* long
straight walls and nested rectangular rooms -- a crisp, architectural look unlike
any of the carving algorithms' organic grain. *Cost:* O(n), no recursion-depth
limit.

### 3.5 Bias at a glance

| Algorithm | Family | Bias / texture | Uniform? | Extra memory |
| --- | --- | --- | --- | --- |
| recursive-backtracker | carving | long winding corridors, few dead-ends | no | O(n) stack |
| hunt-and-kill | carving | long rivers, no stack | no | O(1) |
| growing-tree (50/50) | carving | tunable; here, backtracker∼Prim blend | no | O(n) active set |
| Kruskal's | spanning-tree | spiky, many short dead-ends | no | O(n) union-find |
| Prim's (random) | spanning-tree | short dead-ends, radial branching | no | O(n) frontier |
| Wilson's | spanning-tree | unbiased (UST) | **yes** | O(n) walk arrays |
| Aldous-Broder | spanning-tree | unbiased (UST), slow | **yes** | O(1) |
| binary-tree | row-wise | hard NE diagonal bias | no | O(1) |
| sidewinder | row-wise | "rising" look, one top corridor | no | O(1) |
| Eller's | row-wise | relaxed grain, O(height)=O(1) memory | no | O(width) |
| recursive-division | wall-adding | straight walls, nested rectangular rooms | no | O(n) region stack |

---

## 4. Grid Topologies

`--grid TYPE` selects the topology (default `square`). The headline fact:

> **The square grid runs the full eleven-algorithm zoo; the four non-square
> grids (`hex`, `theta`, `triangle`, `upsilon`) run seven of them.** A shared
> topology descriptor lets backtrack, Kruskal, Wilson, Aldous-Broder, Prim,
> Hunt-and-Kill, and Growing Tree carve any grid; Eller, binary-tree,
> sidewinder, and recursive-division stay square-only.

Generalizing maze carving off the square is the genuinely interesting part --
a generator doesn't care about geometry as long as you can answer "what are
this cell's neighbors?" and "which wall separates these two?". Each grid below
answers those two questions differently. The seven portable algorithms reach
that answer through a per-topology **descriptor** (`Section 7D-ter`) -- a small
vtable of `neighbors` / `link` / `visited?` / `mark` procs -- so a single
implementation of each algorithm drives all five grids. The availability
matrix:

| Algorithm     | square | hex | theta | triangle | upsilon |
| ------------- | ------ | --- | ----- | -------- | ------- |
| backtrack     | +      | +   | +     | +        | +       |
| kruskal       | +      | +   | +     | +        | +       |
| wilson        | +      | +   | +     | +        | +       |
| aldous-broder | +      | +   | +     | +        | +       |
| prim          | +      | +   | +     | +        | +       |
| hunt-kill     | +      | +   | +     | +        | +       |
| growing-tree  | +      | +   | +     | +        | +       |
| eller         | +      |     |       |          |         |
| binary-tree   | +      |     |       |          |         |
| sidewinder    | +      |     |       |          |         |
| division      | +      |     |       |          |         |

Slanted and curved walls are drawn with an integer **Bresenham line** primitive
(`-draw-line`); only the square grid gets away with
axis-aligned rectangle fills.

### 4.1 square (default)
Four neighbors (N/E/S/W), one byte per cell, axis-aligned walls. `--size WxH` is
columns × rows. The reference topology; everything else is measured against it.

### 4.2 hex (`--grid hex`)
Pointy-top hexagons in an **odd-r offset** layout (Section 5B):
odd rows are shifted half a hex to the right. Six neighbors; the neighbor offsets
depend on row parity (`row mod 2`), so the direction table stores even-row and
odd-row deltas side by side. Cell byte init is `0x3F` (six walls). Walls are
drawn as Bresenham lines along the closed hex edges; the color path scanline-fills
each hexagon with its distance color, then stamps walls on top.

### 4.3 theta (`--grid theta`) -- polar
Concentric rings of cells around a central hole (Section 5C).
Here `--size WxH` reads as **rings × sectors**. Four neighbors: inward, outward,
clockwise, counter-clockwise -- and CW/CCW **wrap** around the ring modularly.
Rendering is the most unusual in the file: a **per-pixel polar scan** (no
polygons). For every pixel it computes `rad = sqrt(dx² + dy²)` and
`angle = atan2(dy, dx)`, derives the (ring, sector), and decides whether the
pixel falls on a wall. It is slow -- one `sqrt` and one `atan2` per pixel -- but
produces a true round maze with no polygon approximation. (Because each pixel is
independent, theta is the *lightest* grid on VM memory; it runs in the default
1 MB VM.)

### 4.4 triangle (`--grid triangle`)
Alternating up- and down-pointing equilateral triangles (Section 5D). A cell
points up when `(col + row)` is even, down otherwise. Three
neighbors: left, right, and a horizontal edge whose direction flips with the
cell's orientation (an up-triangle's horizontal neighbor is below it; a
down-triangle's is above). Walls are Bresenham lines along the three closed
edges.

### 4.5 upsilon (`--grid upsilon`) -- octagons + squares
A 4.8.8 truncated-square tiling: octagons and squares alternate (Section 5E). A cell is an **octagon** when `(col + row)` is even (eight
neighbors: four axis squares + four diagonal octagons) and a **square**
otherwise (four axis neighbors, all octagons). The octagon needs all eight wall
bits, so upsilon is the one grid that uses **two bytes per cell** -- a walls byte
and a separate flags byte for VISITED / IN-SOLUTION. The geometry is sized so the
octagon's diagonal edges land exactly on the square's corners, tiling without
gaps. (`--braid` is silently ignored on upsilon; color heatmaps *are* supported.)

### 4.6 What works where

| Feature            | square | hex | theta | triangle | upsilon |
| ------------------ | :----: | --- | :---: | :------: | :-----: |
| Full 10-algo zoo   |   ✓    | ·   |   ·   |    ·     |    ·    |
| Backtracker        |   ✓    | ✓   |   ✓   |    ✓     |    ✓    |
| Color heatmaps (6) |   ✓    | ✓   |   ✓   |    ✓     |    ✓    |
| `--solve` ribbon   |   ✓    | ✓   |   ✓   |    ✓     |    ✓    |
| `--braid`          |   ✓    | ✓   |   ✓   |    ✓     |    ·    |
| `--weave`          |   ✓    | ·   |   ·   |    ·     |    ·    |

---

## 5. Distance Fields and Colormaps

The heatmap coloring answers "how far is each cell from the start?" and paints
the answer.

**The distance field** is a plain breadth-first search from the start cell
(`bfs-distances`), implemented as a ring-buffer FIFO over two
chunked UInteger arrays. Because a perfect maze is a tree, BFS distance is just
the unique path length; the field also records `max-d`, the eccentricity used to
normalize colors. Every topology has its own BFS variant with the same shape but
the right neighbor set.

**The colormaps** (`--color NAME`, Section 9B). Ten are
available; the default is `mono` (plain black/white, no distance field computed):

| Name        | Stops | Source                                    |
| ----------- | ----- | ----------------------------------------- |
| `viridis`   | 32    | matplotlib viridis, sampled to 8-bit      |
| `magma`     | 32    | matplotlib magma                          |
| `inferno`   | 32    | matplotlib inferno                        |
| `plasma`    | 32    | matplotlib plasma                         |
| `cividis`   | 32    | matplotlib cividis (colorblind-optimized) |
| `turbo`     | 32    | matplotlib turbo (perceptual rainbow)     |
| `rainbow`   | 32    | computed HSV cycle (hue = i·360/32)       |
| `cubehelix` | 32    | matplotlib cubehelix (grayscale-safe)     |
| `grayscale` | 2     | black → white luminance ramp              |
| `two-tone`  | 2     | blue → orange ramp                        |

A cell's color is found by normalizing `t = dist / max-d` into `[0,1]`, scaling
to the stop range, and **linearly interpolating** each RGB channel between the
two bracketing stops (`cmap-color` / `-lerp-byte`). The polar
and octagon renderers precompute one color per cell so the per-pixel inner loop
stays a pure byte lookup.

---

## 6. Overlays: Solve, Braid, Weave

Three optional transforms, applied in this order: **braid** (mutates the maze),
then **solve** and **weave** (render-time overlays).

### 6.1 `--solve` -- the shortest path

Draws the start→end geodesic as a red ribbon. The path is recovered by running
the BFS distance field from the start and then walking *backward* from the end,
always stepping to a neighbor whose distance is exactly one less (`bfs-solve`) -- correct because BFS on a tree gives true geodesic distances.
The end defaults to the far corner; override with `--start X,Y` / `--end X,Y`.
The ribbon is stamped over the finished image (it does not alter cell bytes), and
on the non-square grids it is drawn as a real Bresenham polyline between cell
centers. If `--braid` has disconnected the end from the start, the solver returns
an empty path.

### 6.2 `--braid P` -- knocking out dead-ends

A perfect maze is all dead-ends; a **braided** maze removes some, creating loops.
`--braid P` walks the grid and, for each dead-end, with probability `P` knocks
down one of its walls (preferring an in-bounds neighbor). `P = 1.0` removes every
dead-end (fully braided, very loopy); `P = 0.3` thins them out. A "dead-end" is
defined per grid by wall count -- square = 3 of 4 walls present, hex = 5 of 6,
theta = 3 of 4, triangle = 2 of 3. Braiding **breaks the spanning-tree property**
on purpose, which is why a braided maze can have more than one solution (and why
the solver can come up empty if a region is cut off). Not supported on upsilon.

### 6.3 `--weave` -- bridges and underpasses

The flashiest overlay, and **square + backtracker only**. A *weave* maze lets one
passage cross *over* another. During the backtracker carve, when the next cell
`B` is already visited but the cell two steps away `C` is free *and* `B` has a
straight perpendicular passage, `amazing.trx` carves a bridge in one move:
`A → B → C`, making `B` a four-way crossing in the connectivity graph, and tags
`B` with a WEAVE-UNDER-H or WEAVE-UNDER-V hint bit. Connectivity stays correct --
BFS, solve, and braid are all weave-agnostic; the tag bits are purely a render
hint. The renderer stamps the under-cell with two black "bridge bands", a
shoulder tint, and a drop shadow, producing the illusion of one corridor passing
over the other.

---

## 7. Masking: Shape-Carved Mazes

Masking restricts the maze to a chosen subset of the square grid, so the
corridors trace a **word, a logo, an SVG, or an analytic figure** instead of
filling a plain rectangle. It is the same idea as Walter Pullen's Daedalus mask
images: masks come from text rendered through a selectable font, from analytic
geometry, or from an SVG rasterised to a 1-bit silhouette by a host tool (only
the Trix logo's derived mask is bundled -- see [§7.4](#74-svg-masks)).

```bash
./trix --vm-size=64M examples/amazing.trx --mask-text 'Amazing!' --out word.png
./trix --vm-size=128M examples/amazing.trx --mask logo --color turbo --out logo.png
./trix --vm-size=128M examples/amazing.trx --mask-file star --color viridis --out star.png
./trix --vm-size=64M examples/amazing.trx --mask-text Trix --font hershey-serif --out hershey.png
./trix --vm-size=64M examples/amazing.trx --mask disc --size 28x28 --color inferno --out disc.png
./trix --vm-size=64M examples/amazing.trx --mask-text Trix --mask-invert --color magma --out punch.png
```

### 7.1 What a mask is

A mask mirrors the grid: a `[w h rows]` triple whose `rows` is an array of
`w`-byte strings, a byte `!= 0` meaning **in-mask**. The sources:

| Source | Flag | How it is built |
| --- | --- | --- |
| **Text** | `--mask-text WORD` | the word rendered through `--font` (see [§7.3](#73-fonts)); each font pixel becomes a `--mask-scale` × `--mask-scale` block of cells |
| **Logo** | `--mask logo` | the real Trix logo, cut out of a maze -- a mask rasterised from `assets/trix-logo.svg` (see [§7.4](#74-svg-masks)) |
| **SVG** | `--mask-file NAME` | any SVG you rasterised to `examples/mask-shapes/<NAME>.trx` (see [§7.4](#74-svg-masks)) |
| **Analytic** | `--mask disc` / `ring` / `frame` | a circle / annulus / border band inscribed in the `--size` grid |
| **Inverse** | add `--mask-invert` | the shape is padded into a larger canvas (`--mask-margin`) and complemented, so the maze fills *around* it and the shape becomes holes |

So you get the figure two ways: by default the **corridors are the shape**; with
`--mask-invert` the **shape is punched out** of a full maze.

### 7.2 Carving only the in-mask cells

The carve reuses the generic engine (§[10.1](#101-section-map), Section 7D-ter)
unchanged. A wrapper descriptor, `masked-square-desc`, delegates everything to
the square descriptor except `neighbors`: `masked-neighbors` calls the base
enumerator and then **drops every neighbour that is out-of-mask**. Because a wall
is only ever carved between two in-mask cells, the boundary walls are never
touched -- they stay closed and the renderer draws them as the crisp silhouette
edge for free.

A word or logo is usually **disconnected** (separate letters, separate strokes),
so the carve runs per connected component:

- **Kruskal** lists every in-mask wall in a single pass; its union-find naturally
  yields one spanning tree per component (a spanning *forest*).
- The other six generators are **seeded once per component** over an isolated
  grid -- the cells of every other component are temporarily marked visited, so a
  seeded backtracker / Wilson / Aldous-Broder / Prim / Hunt-and-Kill /
  Growing-Tree fills exactly its own piece. (Wilson and Aldous-Broder terminate
  when the region is fully visited, so they count the *unvisited* cells through
  the descriptor first -- on a full grid that is every cell, under a mask it is
  one component.)

The result is a perfect maze in every connected piece: the self-test asserts
`open-edges == in-mask-cells - components` for all seven algorithms, which also
certifies that no cell was left stranded. Masking composes with every `--color`
(each component is shaded by its own internal BFS distance) and any `--algo`;
`--solve`, `--braid`, and `--weave` are square-rectangle overlays and are skipped
under a mask.

### 7.3 Fonts

`--mask-text` renders through a selectable `--font`. Trix has no
FreeType, so a font cannot be rasterised inside the VM; instead each font is
pre-rendered by a host tool into a Trix data file under `examples/mask-fonts/`,
which `amazing.trx` loads on first use (lazily; non-masking runs never touch
them). `--font-dir` overrides where they are found.

| `--font` | Kind | Source |
| --- | --- | --- |
| `5x7` | bitmap (built-in) | the inline 5×7 block font ([§10.2](#102-the---compare-font)); no file load |
| `roboto-bold` *(default)* | bitmap atlas | Roboto Bold, **Apache-2.0** |
| `roboto-mono-bold` | bitmap atlas | Roboto Mono Bold, **Apache-2.0** |
| `hershey-sans` | stroke | Hershey Roman Simplex, **public domain** |
| `hershey-serif` | stroke | Hershey Roman Triplex, **public domain** |
| *(your atlas)* | bitmap / stroke | any `examples/mask-fonts/<name>.trx` |

Two **kinds** of font, both producing the same in/out mask:

- **Bitmap atlas** (`roboto-*`): a per-glyph packed bitmap on a common baseline.
  Glyphs are laid out by proportional advance width (correct spacing; pair
  kerning is omitted -- sub-cell here), each font pixel a `--mask-scale` block,
  and the finished word is trimmed to its ink (so `Trix` loses the empty
  descender band but `Amazing!` keeps the `g`). Filled letterforms.
- **Stroke font** (`hershey-*`): polyline centerlines rendered in **pure Trix** by
  stroking each segment with a square pen -- vector, so crisp at any size, with
  an engraved/wireframe look. No host renderer needed at run time.

**Regenerating / adding fonts.** Two host tools (needed only to regenerate, not
to run): `tools/gen_mask_font.py` (Pillow + NumPy) downloads Roboto and emits the
bitmap atlases; `tools/gen_hershey_font.py` (standard library) re-encodes the
public-domain Hershey coordinates. To use *any* font for your own local mazes:

```bash
tools/gen_mask_font.py --ttf /path/to/Some-Bold.ttf --name some-bold
./trix examples/amazing.trx --mask-text Hello --font some-bold --out hi.png
```

A note on licensing (informational, not legal advice): in the US a typeface
*design* -- and the pixels it renders -- is not copyrightable; only the font
*program* is, and the font *name* may be a trademark. Rendering glyphs for your
own output is unencumbered regardless of the source font's license. The license
only matters if you intend to *commit / redistribute* the generated atlas. This
project therefore bundles only **Apache-2.0** (Roboto) and **public-domain**
(Hershey) glyph data; see `tools/gen_mask_font.py` for a license-by-font table
and the root `NOTICE.md` for attribution.

### 7.4 SVG masks

`--mask logo` carves the **real Trix logo**, and `--mask-file NAME` carves any
SVG you choose. Trix has no SVG rasteriser, so -- exactly as with fonts -- a host
tool renders the SVG **once** into a `[w h rows]` mask under
`examples/mask-shapes/`, which `amazing.trx` loads on first use. Only the derived
1-bit mask is committed, never the source `.svg`; the maze engine is
source-agnostic, so an SVG mask carves like any other.

The tool is [`tools/gen_mask_svg.py`](../tools/gen_mask_svg.py). Install the
renderer (Debian/Ubuntu) with `sudo apt install python3-cairosvg python3-pil
python3-numpy`, then:

```bash
# regenerate the bundled logo from assets/trix-logo.svg
python3 tools/gen_mask_svg.py

# rasterise any SVG for your own local mazes (not committed)
python3 tools/gen_mask_svg.py star.svg --name star
./trix --vm-size=128M examples/amazing.trx --mask-file star --color turbo --out star.png
```

It classifies ink at high resolution (luminance `< --threshold`, so a faint
subtitle is dropped while dark artwork is kept), then downsamples by **area
coverage** so thin strokes survive instead of washing out. `--dilate` thickens
strokes; `--invert` ships the **cutout** (the maze fills *around* the shape) and
`--margin` adds a surrounding band.

The bundled `trix-logo` is line-art, so it ships pre-inverted: its in-mask region
is the maze *surrounding* the slash-art, and the logo reads as channels cut out
of the maze. `--mask logo` auto-sizes `--cell-px` so the image lands near
~3000 px (override with your own `--cell-px`); `--mask logo --mask-invert`
recovers the (thin) filled strokes. A mask is a 1-bit silhouette with no colours
or paths from the artwork, so rasterising an SVG for your own output is
unencumbered; the source license only matters if you *commit / redistribute* the
generated mask. See [`examples/mask-shapes/README.md`](mask-shapes/).

---

## 8. The PNG Encoder

`amazing.trx` writes real `.png` files with **no libpng**: the PNG file *format*
-- the chunk structure, headers, and scanline framing -- is assembled in Trix.
The heavy lifting underneath uses the Trix engine's native ops:

- **`deflate`** -- the IDAT compression. This is the engine's zlib-backed op
  (raw RFC-1951 stream, `Z_DEFAULT_COMPRESSION`), so the compression itself *is*
  zlib; what's hand-written is everything around it.
- **`crc32`** -- the per-chunk CRC seal. Hand-rolled in the engine (reflected
  CRC-32, polynomial `0xEDB88320`), *not* from zlib.
- **`adler32`** -- the zlib-stream trailer checksum. Also hand-rolled in the
  engine, not from zlib.

So the honest one-liner is: **the PNG format is built in Trix over the engine's
native `deflate` (zlib) and hand-rolled `crc32`/`adler32` ops.** No libpng, and
zlib only for the DEFLATE step.

### 8.1 File structure

The encoder (Sections 2-4) emits, in order:

1. **Signature** -- the 8 bytes `89 50 4E 47 0D 0A 1A 0A`.
2. **IHDR** -- width and height (big-endian u32), **bit depth 8**, **color type
   2** (truecolor RGB), compression 0, filter 0, interlace 0.
3. **IDAT** -- the pixel data as a zlib stream: the 2-byte header `78 9C`, the
   raw DEFLATE body from the `deflate` op, and a 4-byte big-endian `adler32` of
   the *uncompressed* filtered scanlines. Each scanline is prefixed with a `0x00`
   (None) filter byte. Large images split the payload across multiple IDAT
   chunks (capped at 65,000 bytes each).
4. **IEND** -- the empty terminator chunk.

Every chunk is framed as `length (BE u32) ‖ type ‖ data ‖ crc32(type ‖ data)`.

### 8.2 Two IDAT backends

The 65,535-byte string cap bites again, this time on the *compressed* buffer.
`amazing.trx` picks a backend by size (`write-idat`):

- **In-memory** (`-write-idat-deflated`): when the filtered data fits in a Trix
  string (`height·(1 + 3·width) ≤ 65535`), `deflate` the whole buffer at once.
- **Streaming** (`-write-idat-streaming`): for larger images, tee the scanlines
  to a temp file, `deflate-stream` it file-to-file, and read the compressed
  result back in chunks -- folding the per-row Adler-32 fragments with a small
  RFC-1950 `adler32_combine` helper so the input never has to be re-read.

This is why `--monster` (a 1001×1001 PNG) works at all.

---

## 9. Command-Line Reference

Flags are parsed in `/parse-args` against a string-keyed `arg-dispatch` table. A bare non-flag argument is taken as the output filename.

| Flag | Argument | Effect | Default |
| --- | --- | --- | --- |
| `--help`, `-h` | -- | Print usage and exit | -- |
| `--self-test` | -- | Run the internal regression suite (needs a big VM; see below) | off |
| `--size` | `WxH` | Maze size in cells (rings × sectors for `theta`) | `20x20` |
| `--cell-px` | int | Pixels per cell | `16` |
| `--wall-px` | int | Wall thickness in pixels | `2` |
| `--wall-color` | `RRGGBB` | Maze line color (hex; `#` optional) | `000000` |
| `--bg-color` | `RRGGBB` | Passage / background color (hex) | `FFFFFF` |
| `--seed` | uint | RNG seed; `0` seeds from the clock | `0` |
| `--out` | file | Output PNG path (or pass it positionally) | `maze.png` |
| `--algo` | name | Generation algorithm (7 portable to all grids; eller/binary-tree/sidewinder/division square-only) | `backtrack` |
| `--grid` | type | `square` / `hex` / `theta` / `triangle` / `upsilon` | `square` |
| `--color` | name | `mono` or a colormap from [§5](#5-distance-fields-and-colormaps) | `mono` |
| `--start` | `X,Y` | Path/heatmap start cell | `0,0` |
| `--end` | `X,Y` | Path end cell; `-1,-1` = far corner | `-1,-1` |
| `--solve` | -- | Overlay the shortest-path ribbon in red | off |
| `--braid` | float `0..1` | Fraction of dead-ends to remove | `0.0` |
| `--weave` | -- | Buck-style overpasses (square + backtrack only) | off |
| `--compare` | `A,B,C` | Side-by-side mono panels of several algorithms | -- |
| `--mask` | name | Carve the maze into a shape: `disc` / `ring` / `frame` / `logo` (the real Trix logo, [§7.4](#74-svg-masks); square grid) | -- |
| `--mask-text` | `WORD` | Carve the maze into a word, rendered through `--font` | -- |
| `--mask-file` | `NAME` | Carve from `mask-shapes/<NAME>.trx`, an SVG rasterised by `tools/gen_mask_svg.py` ([§7.4](#74-svg-masks)) | -- |
| `--mask-dir` | dir | Where to find `mask-shapes/*.trx` | auto |
| `--font` | name | Font for `--mask-text` ([§7.3](#73-fonts)): `5x7` / `roboto-bold` / `roboto-mono-bold` / `hershey-sans` / `hershey-serif` / custom | `roboto-bold` |
| `--font-dir` | dir | Where to find `mask-fonts/*.trx` | auto |
| `--mask-scale` | int | Cells per font pixel | `1` hi-res, `4` for `5x7` |
| `--mask-invert` | -- | Punch the shape out of a full maze (the inverse) | off |
| `--mask-margin` | int | With `--mask-invert`, surrounding-maze thickness | auto |
| `--stress` | -- | Preset: 200×200 Eller's | -- |
| `--monster` | -- | Preset: 1000×1000 Eller's (needs a large VM) | -- |
| `--bench` | -- | Time all eleven algorithms; write no PNG | off |
| `--metrics` | -- | Print a quality report (degree histogram, loops, solution length) on any grid; twistiness is square-only; no PNG | off |
| `--quiet` | -- | Suppress stderr progress and phase timings | off |

**VM size.** `--vm-size` is a *Trix interpreter* flag and goes **before** the
script name (`trix --vm-size=8M examples/amazing.trx ...`). Rough guidance:

- `theta` runs in the default 1 MB VM.
- `square` and `hex` at typical sizes want **≥ 2 MB** (`--vm-size=8M` is a safe
  default for showcase sizes).
- `--stress` wants **≥ 8 MB**; `--monster` wants **~64 MB**.
- `--self-test` needs a large VM (**`--vm-size=64M`**); it allocates a
  32,768-element chunk and aborts at the default 1 MB.

A few examples:

```sh
# Default backtracker maze
./trix --vm-size=8M examples/amazing.trx --out maze.png

# Hex maze, magma heatmap, solved
./trix --vm-size=8M examples/amazing.trx --grid hex --color magma --solve --out hex.png

# Polar maze in the default VM
./trix examples/amazing.trx --grid theta --size 8x24 --color viridis --out theta.png

# A weave maze
./trix --vm-size=8M examples/amazing.trx --weave --color inferno --out weave.png

# Four algorithms side by side
./trix --vm-size=16M examples/amazing.trx --compare backtrack,kruskal,wilson,eller --out compare.png
```

### Going big: `--stress` and `--monster`

Two presets push the size envelope, both via Eller's (its O(width) memory is what
makes them practical). The figures below are from the **optimized** binary
(`trix.opt`):

| Preset      | Cells     | Image (px) | Build time | Peak RSS | File size | `--vm-size` |
| ----------- | --------- | ---------- | ---------- | -------- | --------- | ----------- |
| `--stress`  | 200×200   | 1001×1001  | ~3.5 s     | small    | ~60 KB    | `16M`       |
| `--monster` | 1000×1000 | 3001×3001  | **~3 min** | ~54 MB   | ~0.9 MB   | `64M`       |

`--monster` is the practical ceiling preset -- a million cells, three thousand
pixels on a side, in about three minutes. The file stays under a megabyte because
a mono maze is mostly long runs of black and white that DEFLATE crushes. You
*can* go bigger by hand (`--algo eller --size NxN`, since Eller's is O(1) in
height), but the work grows with the cell count: doubling each side is ~4× the
cells and roughly ~4× the time, so a 2000×2000 maze is ten-plus minutes.

**Two cautions for the big sizes:**

- **Build on `trix.opt`, not the debug binary.** The debug build is `-O0` with
  AddressSanitizer/UBSan; a `--monster` render that takes ~3 minutes optimized
  will crawl -- and use far more memory -- under `./trix`.
- **A full-resolution monster is for panning and zooming, not thumbnails.**
  Scaled down to fit a page it collapses into gray noise; open it in an image
  viewer and explore it at 100%. For something that reads at small size, render a
  *colored* heatmap instead -- the BFS gradient gives it large-scale structure
  that survives shrinking.

---

## 10. Implementation Tour

### 10.1 Section map

The file is organized into numbered `Section N` headers (grep `^%  Section`):

| Section | Contents                                                                         |
| ------- | -------------------------------------------------------------------------------- |
| 2-4     | PNG container, IDAT framing, `write-png`                                         |
| 5 / 4B  | Cell encoding, grid, chunked-array                                               |
| 5B-5E   | Hex / theta / triangle / upsilon grids                                           |
| 6-7     | Direction shuffle; recursive backtracker                                         |
| 7B      | Union-find helper (`-uf-find`, backs generic Kruskal)                            |
| 7C-*    | Per-topology backtrackers (test oracles)                                         |
| 7D-ter  | Topology descriptor vtable + generic algorithm engine                            |
| 7D-7G   | Eller; binary-tree; sidewinder; recursive division; braid                        |
| 7D / 7F | BFS distance field; path solver; hardest pair                                    |
| 8-10E   | Pixel buffer; mono and color renderers per grid                                  |
| 11-13   | Solve overlay; 5×7 font; compare mode                                            |
| 14      | Masking: mask builders, masked descriptor, per-component carve, masked renderers |
| 99      | `--self-test` suite                                                              |
| 100     | CLI parsing, main dispatch, `--bench` / `--metrics`                              |

### 10.2 The `--compare` font

`--compare` labels each panel using a tiny built-in **5×7 bitmap font** (Section
12): 26 glyphs (A-Z), 7 bytes each, **182 bytes** total, with
lowercase folded to uppercase and unknown characters rendered blank.

### 10.3 Self-test

`--self-test` runs **179 assertions** across 29 test procedures (the `opt-self-test` block):
Adler-32 vectors, chunked-array primitives, cell bit-encoding, a PNG checkerboard
round-trip, a per-algorithm connectivity invariant (every algorithm must yield a
fully connected spanning tree), the recursive-division perfect-maze check (connected
*and* exactly `w*h-1` passages -- the failure modes a wall-adder has that carvers
don't), colormap endpoints, end-to-end color renders,
BFS-solve correctness on all five grids, braid-to-zero-dead-ends at `P=1.0`,
weave under-cell presence with intact connectivity, font glyph bits, compare
geometry, and the masking builders + per-component perfect-forest invariant for all
seven portable algorithms. Run it with a large VM:

```sh
./trix --vm-size=64M examples/amazing.trx --self-test
```

### 10.4 The gallery

[`gallery.sh`](gallery.sh) renders a curated showcase -- by default **41 PNGs**
(every algorithm, every colormap, plus solve/braid/weave feature shots) into
`examples/maze-gallery/` at a fixed seed (42) for bit-exact reproducibility. The
gallery directory is git-ignored except for a handful of curated images tracked
for the project README; see [`maze-gallery/README.md`](maze-gallery/README.md).

```sh
examples/gallery.sh          # 41 PNGs
examples/gallery.sh --full   # adds the slow 200×200 stress render
```

---

## 11. Further Reading

- **Jamis Buck, *Mazes for Programmers* (Pragmatic Bookshelf, 2015).** The
  definitive practical tour of these algorithms, the grid abstractions (square /
  hex / polar / triangle), braiding, and weaving. `amazing.trx` follows its
  taxonomy closely.
- **Jamis Buck's blog series**, "Maze Generation" (2010-2011) -- the original
  write-ups of each algorithm with animations.
- **Wilson's algorithm**: David Bruce Wilson, "Generating random spanning trees
  more quickly than the cover time" (STOC 1996) -- the uniform-spanning-tree
  result.
- **Aldous-Broder**: the random-walk UST result, independently David Aldous and
  Andrei Broder (1989-1990).
- **PNG**: the [PNG (Portable Network Graphics) Specification](https://www.w3.org/TR/png/);
  **DEFLATE** is [RFC 1951](https://www.rfc-editor.org/rfc/rfc1951), the **zlib**
  container is [RFC 1950](https://www.rfc-editor.org/rfc/rfc1950).
- For the Trix mechanics the hot loops lean on, see
  [local-global-vm.md](../docs/local-global-vm.md) (the arena / persist family)
  and the `deflate` / `crc32` / `adler32` ops in
  [trix-reference.md](../docs/trix-reference.md) §3.9.
