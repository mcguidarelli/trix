# Mask shapes

Mask data for [`examples/amazing.trx`](../amazing.trx)'s masking feature
(`--mask logo` / `--mask-file`). Each `*.trx` file registers one `[w h rows]`
mask into the `MASK-SHAPES` registry and is loaded lazily on first use.

Trix has no SVG rasteriser, so an `.svg` cannot be turned into a mask inside the
VM. Instead a host tool renders the SVG **once** into Trix data here — **only the
derived 1-bit mask is committed, never the source `.svg`.** A mask records only
*where the maze goes*, so the maze engine is source-agnostic: a mask from an SVG
carves exactly like one from a font (`--mask-text`) or an analytic shape
(`--mask disc`).

| File            | Used by       | Source                                            |
| --------------- | ------------- | ------------------------------------------------- |
| `trix-logo.trx` | `--mask logo` | the in-repo `assets/trix-logo.svg` (our own logo) |

`trix-logo` ships **pre-inverted**: its in-mask region is the maze that
*surrounds* the slash-art, so the logo reads as channels cut out of a maze (the
thin strokes are unusable as positive corridors). `--mask logo --mask-invert`
recovers the filled strokes.

## Regenerate / add a shape

The host tool is [`tools/gen_mask_svg.py`](../../tools/gen_mask_svg.py). Install
the renderer (Debian/Ubuntu):

```bash
sudo apt install python3-cairosvg python3-pil python3-numpy
```

```bash
# regenerate the bundled logo from assets/trix-logo.svg
python3 tools/gen_mask_svg.py

# rasterise any SVG you have, for your own local mazes (not committed)
python3 tools/gen_mask_svg.py star.svg --name star
./trix --vm-size=128M examples/amazing.trx --mask-file star --color turbo --out star.png
```

It renders the SVG, classifies ink at high resolution (luminance `< --threshold`,
so a faint subtitle can be dropped while dark artwork is kept), then downsamples
by **area coverage** (`--coverage`) so thin strokes survive instead of washing
out. `--dilate` thickens strokes, `--invert` ships the cutout (maze around the
shape), and `--margin` adds a surrounding band. See `--help` for the full set.

## Licensing

A mask is a 1-bit silhouette — no colours, gradients, or paths from the source
artwork — and rasterising an SVG for **your own** local output is producing
pixels, not redistributing the artwork. The source SVG's license only matters if
you intend to **commit / redistribute** the generated mask. This project bundles
only the Trix logo (our own asset, Apache-2.0); see the root
[`NOTICE.md`](../../NOTICE.md).
