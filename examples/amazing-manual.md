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
[`examples/amazing.trx`](amazing.trx) -- ten classic maze-generation
algorithms, five grid topologies, distance-field heatmaps, and a PNG encoder
whose file format is assembled in Trix.

This manual is self-contained: read it end to end and you will come away
understanding how each maze algorithm works, what kind of maze it produces and
why, how those algorithms generalize off the square grid, and how ~5,200 lines
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
   - [3.4 Bias at a glance](#34-bias-at-a-glance)
4. [Grid Topologies](#4-grid-topologies)
5. [Distance Fields and Colormaps](#5-distance-fields-and-colormaps)
6. [Overlays: Solve, Braid, Weave](#6-overlays-solve-braid-weave)
7. [The PNG Encoder](#7-the-png-encoder)
8. [Command-Line Reference](#8-command-line-reference)
9. [Implementation Tour](#9-implementation-tour)
10. [Further Reading](#10-further-reading)

---

## 1. Mazes in 60 Seconds

A **perfect maze** is a grid of cells where exactly one path connects any two
cells -- no loops, no isolated regions. In graph terms it is a **spanning tree**
of the grid: every cell reachable, no cycles. All ten algorithms here produce
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

Each cell is a single byte (Section 5, lines 309-353). The low nibble holds the
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

A grid is a packed 3-array `[width height rows]` (lines 445-456), where `rows` is
an array of byte-strings, one per row. Trix's `length_t` is a `uint16_t`, so a
single string maxes out at 65,535 bytes -- a flat one-string grid would cap you
at a ~255×255 maze. Storing one string *per row* sidesteps that: a `--monster`
1000×1000 maze is 1,000 row-strings of 1,000 bytes each.

Auxiliary per-cell storage that an algorithm needs (union-find parents, BFS
queues, frontier lists) faces the same cap, so the file provides a
**chunked-array** (Section 4B, lines 356-427): a logical array split into chunks
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

All ten algorithms below run on the **square** grid. (The four non-square grids
are backtracker-only -- see [§4](#4-grid-topologies).) Select one with
`--algo NAME`; the default is `backtrack`. The dispatch table is at lines
4948-4964.

Four of the ten ignore the start cell entirely (`kruskal`, `eller`,
`binary-tree`, `sidewinder`); the other six begin carving from `--start`.

### 3.1 Carving family (DFS)

These grow a single tree by walking and backtracking. They tend to produce
**long, winding corridors with few short dead-ends** -- the "twisty little
passages" look.

**recursive-backtracker** (`--algo backtrack`, lines 592-675)
The default, and the prettiest general-purpose maze. Depth-first search: from
the current cell, step to a random unvisited neighbor (carving the wall), and
when you hit a dead-end, backtrack to the last cell that still has an unvisited
neighbor. *Implementation:* an explicit iterative DFS -- a `w*h` chunked-array
used as a stack of `[x y dirs dir-idx]` frames, where `dirs` is a freshly
shuffled permutation of the four directions, so there is no recursion-depth
limit. *Texture:* long corridors, low branching, every cell on one snaking
spine. This is also the only algorithm that supports `--weave` ([§6](#6-overlays-solve-braid-weave)).

**hunt-and-kill** (`--algo hunt-kill`, lines 2027-2097)
Same walk as the backtracker, but with no stack. When the walk gets stuck, it
**hunts**: scan the grid row by row for the first unvisited cell that has at
least one visited neighbor, carve into it, and resume the walk there. *Texture:*
similar long corridors to the backtracker but with longer "rivers", because the
hunt restarts deterministically from the top-left rather than backtracking.
*Cost:* the repeated scans make it slower; the payoff is O(1) extra memory.

**growing-tree** (`--algo growing-tree`, lines 2107-2158)
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

**Kruskal's** (`--algo kruskal`, lines 711-775)
Classic minimum-spanning-tree shape: list every interior wall, shuffle the list,
and remove each wall **only if** the two cells it separates are not already
connected. *Implementation:* a **path-compressed union-find** (`-uf-find`, lines
699-709) tracks connectivity; the wall list is a chunked-array of UIntegers,
each wall packed as `(y*w + x)*2 + (east|south)`, shuffled in place with
Fisher-Yates. All union-find and wall writes go through `chunked-put-persist`
(journal-cold) because there are millions of them. *Texture:* lots of short
dead-ends, a "spiky", uniform-looking maze with no global grain.

**Prim's (randomized)** (`--algo prim`, lines 1948-2021)
Grow a tree from a seed by repeatedly connecting a random **frontier** cell (a
not-yet-in cell adjacent to the tree) to a random in-tree neighbor.
*Implementation:* the frontier is a chunked-array used as a stack with O(1)
removal (swap-with-last), backed by a parallel byte-bitmap (`in-frontier`) for
O(1) membership tests so cells are never double-added. This is *random* Prim's
(uniform edge weights), not weighted. *Texture:* very short dead-ends and a
strong "radial" branching look around the seed.

**Wilson's** (`--algo wilson`, lines 800-855)
A **uniform** spanning tree via loop-erased random walks. Pick a cell not yet in
the maze, random-walk until you hit the maze, then add the walk's *loop-erased*
path. *Implementation:* two parallel byte arrays -- `in-maze` and `walk-dir`
(the last direction walked out of each cell). Loop erasure is free: revisiting a
cell simply overwrites its `walk-dir`, so when you finally retrace, you follow
the most recent direction out of each cell and the loops vanish. A monotonic
scan cursor finds the next start cell and never backs up. *Texture:* unbiased --
the fairest maze in the zoo. *Cost:* the early walks wander a long time before
the maze is big enough to hit; slow to start, fast to finish.

**Aldous-Broder** (`--algo aldous-broder`, lines 1913-1938)
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

**binary-tree** (`--algo binary-tree`, lines 1855-1874)
The simplest possible maze: for every cell, carve **either north or east** (coin
flip; the only available option at the edges). One pass, no state. *Texture:* a
hard diagonal bias -- the entire top row is one open corridor, the entire right
column is another, and every cell has an unobstructed path to the top-right.
Great for teaching, ugly as a maze.

**sidewinder** (`--algo sidewinder`, lines 1881-1905)
binary-tree's smarter cousin. Sweep each row left to right building a horizontal
"run"; at each cell, either extend the run east or **close** it -- and when you
close a run, carve north from one *random* cell in the run. *Texture:* removes
the vertical-corridor artifact (only the top row is one long hall), leaving a
characteristic "rising" look. O(1) memory.

**Eller's** (`--algo eller`, lines 1749-1833)
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

### 3.4 Bias at a glance

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

---

## 4. Grid Topologies

`--grid TYPE` selects the topology (default `square`). The headline fact:

> **Only the square grid runs the full ten-algorithm zoo.** The four non-square
> grids (`hex`, `theta`, `triangle`, `upsilon`) are carved by the recursive
> backtracker only; `--algo` is ignored on them.

Generalizing maze carving off the square is the genuinely interesting part --
the backtracker doesn't care about geometry as long as you can answer "what are
this cell's neighbors?" and "which wall separates these two?". Each grid below
answers those two questions differently.

Slanted and curved walls are drawn with an integer **Bresenham line** primitive
(`-draw-line`, lines 2483-2506); only the square grid gets away with
axis-aligned rectangle fills.

### 4.1 square (default)
Four neighbors (N/E/S/W), one byte per cell, axis-aligned walls. `--size WxH` is
columns × rows. The reference topology; everything else is measured against it.

### 4.2 hex (`--grid hex`)
Pointy-top hexagons in an **odd-r offset** layout (Section 5B, lines 509-554):
odd rows are shifted half a hex to the right. Six neighbors; the neighbor offsets
depend on row parity (`row mod 2`), so the direction table stores even-row and
odd-row deltas side by side. Cell byte init is `0x3F` (six walls). Walls are
drawn as Bresenham lines along the closed hex edges; the color path scanline-fills
each hexagon with its distance color, then stamps walls on top.

### 4.3 theta (`--grid theta`) -- polar
Concentric rings of cells around a central hole (Section 5C, lines 1019-1068).
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
Alternating up- and down-pointing equilateral triangles (Section 5D, lines
1218-1265). A cell points up when `(col + row)` is even, down otherwise. Three
neighbors: left, right, and a horizontal edge whose direction flips with the
cell's orientation (an up-triangle's horizontal neighbor is below it; a
down-triangle's is above). Walls are Bresenham lines along the three closed
edges.

### 4.5 upsilon (`--grid upsilon`) -- octagons + squares
A 4.8.8 truncated-square tiling: octagons and squares alternate (Section 5E,
lines 1424-1539). A cell is an **octagon** when `(col + row)` is even (eight
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
(`bfs-distances`, lines 2328-2367), implemented as a ring-buffer FIFO over two
chunked UInteger arrays. Because a perfect maze is a tree, BFS distance is just
the unique path length; the field also records `max-d`, the eccentricity used to
normalize colors. Every topology has its own BFS variant with the same shape but
the right neighbor set.

**The colormaps** (`--color NAME`, Section 9B, lines 2671-2777). Six are
available; the default is `mono` (plain black/white, no distance field computed):

| Name       | Stops | Source                               |
| ---------- | ----- | ------------------------------------ |
| `viridis`  | 32    | matplotlib viridis, sampled to 8-bit |
| `magma`    | 32    | matplotlib magma                     |
| `inferno`  | 32    | matplotlib inferno                   |
| `plasma`   | 32    | matplotlib plasma                    |
| `rainbow`  | 32    | computed HSV cycle (hue = i·360/32)  |
| `two-tone` | 2     | blue → orange ramp                   |

A cell's color is found by normalizing `t = dist / max-d` into `[0,1]`, scaling
to the stop range, and **linearly interpolating** each RGB channel between the
two bracketing stops (`cmap-color` / `-lerp-byte`, lines 2737-2763). The polar
and octagon renderers precompute one color per cell so the per-pixel inner loop
stays a pure byte lookup.

---

## 6. Overlays: Solve, Braid, Weave

Three optional transforms, applied in this order: **braid** (mutates the maze),
then **solve** and **weave** (render-time overlays).

### 6.1 `--solve` -- the shortest path

Draws the start→end geodesic as a red ribbon. The path is recovered by running
the BFS distance field from the start and then walking *backward* from the end,
always stepping to a neighbor whose distance is exactly one less (`bfs-solve`,
lines 2383-2424) -- correct because BFS on a tree gives true geodesic distances.
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

## 7. The PNG Encoder

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

### 7.1 File structure

The encoder (Sections 2-4, lines 79-306) emits, in order:

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

### 7.2 Two IDAT backends

The 65,535-byte string cap bites again, this time on the *compressed* buffer.
`amazing.trx` picks a backend by size (`write-idat`, lines 285-292):

- **In-memory** (`-write-idat-deflated`): when the filtered data fits in a Trix
  string (`height·(1 + 3·width) ≤ 65535`), `deflate` the whole buffer at once.
- **Streaming** (`-write-idat-streaming`): for larger images, tee the scanlines
  to a temp file, `deflate-stream` it file-to-file, and read the compressed
  result back in chunks -- folding the per-row Adler-32 fragments with a small
  RFC-1950 `adler32_combine` helper so the input never has to be re-read.

This is why `--monster` (a 1001×1001 PNG) works at all.

---

## 8. Command-Line Reference

Flags are parsed in `/parse-args` (lines 4818-4905). A bare non-flag argument is
taken as the output filename.

| Flag | Argument | Effect | Default |
| --- | --- | --- | --- |
| `--help`, `-h` | -- | Print usage and exit | -- |
| `--self-test` | -- | Run the internal regression suite (needs a big VM; see below) | off |
| `--size` | `WxH` | Maze size in cells (rings × sectors for `theta`) | `20x20` |
| `--cell-px` | int | Pixels per cell | `16` |
| `--wall-px` | int | Wall thickness in pixels | `2` |
| `--seed` | uint | RNG seed; `0` seeds from the clock | `0` |
| `--out` | file | Output PNG path (or pass it positionally) | `maze.png` |
| `--algo` | name | Generation algorithm (square only) | `backtrack` |
| `--grid` | type | `square` / `hex` / `theta` / `triangle` / `upsilon` | `square` |
| `--color` | name | `mono` or a colormap from [§5](#5-distance-fields-and-colormaps) | `mono` |
| `--start` | `X,Y` | Path/heatmap start cell | `0,0` |
| `--end` | `X,Y` | Path end cell; `-1,-1` = far corner | `-1,-1` |
| `--solve` | -- | Overlay the shortest-path ribbon in red | off |
| `--braid` | float `0..1` | Fraction of dead-ends to remove | `0.0` |
| `--weave` | -- | Buck-style overpasses (square + backtrack only) | off |
| `--compare` | `A,B,C` | Side-by-side mono panels of several algorithms | -- |
| `--stress` | -- | Preset: 200×200 Eller's | -- |
| `--monster` | -- | Preset: 1000×1000 Eller's (needs a large VM) | -- |
| `--bench` | -- | Time all ten algorithms; write no PNG | off |
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

## 9. Implementation Tour

### 9.1 Section map

The file is organized into numbered `Section N` headers (grep `^%  Section`):

| Section | Lines | Contents                                        |
| ------- | ----- | ----------------------------------------------- |
| 2-4     | 79    | PNG container, IDAT framing, `write-png`        |
| 5 / 4B  | 309   | Cell encoding, grid, chunked-array              |
| 5B-5E   | 509+  | Hex / theta / triangle / upsilon grids          |
| 6-7     | 557   | Direction shuffle; recursive backtracker        |
| 7B-7C   | 678   | Kruskal, Wilson                                 |
| 7C-*    | 858+  | Per-topology backtrackers                       |
| 7D-7G   | 1726+ | Eller; long-tail algorithms; braid              |
| 7D / 7F | 2315  | BFS distance field; path solver                 |
| 8-10E   | 2427+ | Pixel buffer; mono and color renderers per grid |
| 11-13   | 3560+ | Solve overlay; 5×7 font; compare mode           |
| 99      | 3926  | `--self-test` suite                             |
| 100     | 4730  | CLI parsing and main dispatch                   |

### 9.2 The `--compare` font

`--compare` labels each panel using a tiny built-in **5×7 bitmap font** (Section
12, lines 3815-3838): 26 glyphs (A-Z), 7 bytes each, **182 bytes** total, with
lowercase folded to uppercase and unknown characters rendered blank.

### 9.3 Self-test

`--self-test` runs **95 assertions** across 26 test procedures (lines 4973-4998):
Adler-32 vectors, chunked-array primitives, cell bit-encoding, a PNG checkerboard
round-trip, a per-algorithm connectivity invariant (every algorithm must yield a
fully connected spanning tree), colormap endpoints, end-to-end color renders,
BFS-solve correctness on all five grids, braid-to-zero-dead-ends at `P=1.0`,
weave under-cell presence with intact connectivity, font glyph bits, and compare
geometry. Run it with a large VM:

```sh
./trix --vm-size=64M examples/amazing.trx --self-test
```

### 9.4 The gallery

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

## 10. Further Reading

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
