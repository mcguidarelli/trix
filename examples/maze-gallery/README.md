# Maze gallery

Sample output from [`examples/amazing.trx`](../amazing.trx) — a ~6,600-line
pure-Trix maze generator (homage to Steve Capps' *Amazing* on the 128K Mac).
Every image here is a real PNG whose format `amazing.trx` assembles itself in
Trix -- no libpng, over the engine's native `deflate`/`crc32`/`adler32` ops --
holding eleven maze algorithms across five grid topologies, distance-colored with
ten colormaps.

This directory is otherwise generated output and git-ignored; the handful below
is curated and tracked for the project README and docs. To render the full set
(grids × algorithms × colormaps, plus solve/braid/weave feature shots), run:

```sh
examples/gallery.sh
```

| Image | Shows |
| --- | --- |
| [`grid-upsilon-viridis.png`](grid-upsilon-viridis.png) | **Upsilon** grid (octagons + squares), viridis shading — the README hero |
| [`grid-theta-viridis.png`](grid-theta-viridis.png) | Concentric **polar** grid, viridis distance shading |
| [`grid-hex-magma.png`](grid-hex-magma.png) | **Hex** grid (pointy-top, odd-r offset), magma colormap |
| [`grid-triangle-inferno.png`](grid-triangle-inferno.png) | **Triangle** grid (alternating up/down), inferno colormap |
| [`compare-4-algos.png`](compare-4-algos.png) | Four algorithms side by side (backtracker, Kruskal, Wilson, Eller) |
| [`algo-division-magma.png`](algo-division-magma.png) | **Recursive division** (wall-adding), magma heatmap — the color bands trace its nested-room subdivision |
| [`algo-division-turbo.png`](algo-division-turbo.png) | **Recursive division** at 100×100, turbo heatmap — the README's third hero shot |
| [`grid-hex-solve.png`](grid-hex-solve.png) | Hex maze with the solution path overlaid in red |
| [`monster-magma.png`](monster-magma.png) | **Monster** heatmap — a 400×400 maze (160k cells) in magma distance shading |
| [`monster-division-rainbow.png`](monster-division-rainbow.png) | **Monster recursive-division** — 400×400 (160k cells), rainbow heatmap; the color blocks expose the recursive partition hierarchy |
| [`color-cividis.png`](color-cividis.png) | **cividis** colormap (colorblind-friendly perceptual) on a Kruskal maze |
| [`color-turbo.png`](color-turbo.png) | **turbo** colormap (perceptually-ordered rainbow) on a Kruskal maze |
| [`color-cubehelix.png`](color-cubehelix.png) | **cubehelix** colormap (grayscale-safe monotone luminance) on a Kruskal maze |
| [`color-grayscale.png`](color-grayscale.png) | **grayscale** colormap (smooth black→white ramp) on a Kruskal maze |
| [`mask-logo.png`](mask-logo.png) | **Masking** — the Trix wordmark (`--mask logo`) carved as maze corridors |
| [`mask-text-amazing.png`](mask-text-amazing.png) | **Text mask** — `--mask-text AMAZING`, each letter its own perfect maze (built-in 5×7 font), viridis |
| [`mask-disc.png`](mask-disc.png) | **Analytic mask** — `--mask disc`, a circular maze with an inferno distance heatmap |
| [`mask-trix-invert.png`](mask-trix-invert.png) | **Inverse mask** — `--mask-text TRIX --mask-invert`: the letters are punched out of a full magma maze |

Algorithms: recursive-backtracker, Kruskal, Wilson, Eller, binary-tree,
sidewinder, Aldous-Broder, Prim, Hunt-and-Kill, Growing Tree,
recursive-division (the one wall-adding generator).
Grids: square, hex, theta (polar), triangle, upsilon (octagon).
Colormaps: viridis, magma, inferno, plasma, cividis, turbo, rainbow, cubehelix,
grayscale, two-tone (plus the `mono` outline render).

The full-resolution **monster** — a 1000×1000-cell maze (3001×3001 px, ~0.9 MB)
for panning and zooming — isn't tracked here; run `examples/gallery.sh --full`
(or `trix.opt examples/amazing.trx --monster`, ~3 min) to generate `monster.png`
in this directory.
