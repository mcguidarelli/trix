#!/usr/bin/env python3
# =============================================================================
# tools/gen_mask_svg.py -- rasterise an SVG into a Trix MASK SHAPE for the
# examples/amazing.trx masking feature (--mask logo / --mask-file).
#
# Trix has no SVG rasteriser (just as it has no FreeType), so an .svg cannot be
# turned into a mask inside the VM.  This host-side tool renders the SVG ONCE,
# thresholds it to 1-bit, and emits it as Trix data
# (examples/mask-shapes/<name>.trx).  Only the derived mask is ever committed --
# never the source .svg need be, and for your own shapes nothing is committed at
# all.  This mirrors the font tooling (tools/gen_mask_font.py): a host tool
# generates, the data is checked in.  The maze engine is source-agnostic -- a
# mask from an SVG carves exactly like one from a font or an analytic shape.
#
# HOST TOOLS REQUIRED (none of this is needed to RUN amazing.trx -- the bundled
# trix-logo mask is already committed; you only need these to regenerate it or
# to rasterise your OWN .svg):
#   * Python 3.8+
#   * CairoSVG, Pillow, NumPy:  sudo apt install python3-cairosvg python3-pil python3-numpy
#
# DEFAULT (no args): regenerates the BUNDLED logo from the in-repo SVG as the
# cutout used by `--mask logo`:
#   examples/mask-shapes/trix-logo.trx     (from assets/trix-logo.svg)
#
# BRING YOUR OWN SVG (for your own local mazes):
#   python3 tools/gen_mask_svg.py star.svg --name star
#   ./trix examples/amazing.trx --mask-file star --out star.png
#
# -----------------------------------------------------------------------------
# A NOTE ON SVG LICENSING  (informational -- not legal advice; verify yourself)
# -----------------------------------------------------------------------------
# A mask records only WHERE the maze goes -- a 1-bit silhouette, no colours,
# gradients, or paths from the source artwork.  Rasterising an SVG to a mask for
# YOUR OWN local output is producing pixels, not redistributing the artwork.
# The source SVG's license only matters if you intend to COMMIT / REDISTRIBUTE
# the generated mask.  amazing.trx bundles only the Trix logo (our own asset).
# =============================================================================

import argparse
import io
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(REPO, "examples", "mask-shapes")
LOGO_SVG = os.path.join(REPO, "assets", "trix-logo.svg")

# In-mask byte is a printable '#' (0x23); out-of-mask is a NUL (0x00).  Trix's
# mask-in? tests "byte != 0", so this round-trips and keeps the dense cutout
# rows compact (one char per in-mask cell instead of a four-char \x00 escape).
IN_CHAR = "#"
OUT_CHAR = "\\x00"


def _require_deps():
    try:
        import cairosvg  # noqa: F401
        import numpy  # noqa: F401
        from PIL import Image  # noqa: F401
    except ImportError as exc:
        sys.exit(
            "gen_mask_svg.py: missing host tool (%s).\n"
            "  install:  sudo apt install python3-cairosvg python3-pil python3-numpy"
            % exc.name
        )


def rasterize(svg_path, target_w, threshold, coverage, supersample):
    """Render svg_path to a [h][w] grid of 0/1; 1 = in-mask foreground ink.

    Ink is classified at HIGH resolution (luminance < threshold), then the binary
    ink map is downsampled by AREA COVERAGE -- a target cell is in-mask if at
    least `coverage` of its area is ink.  This preserves thin strokes (a 1px
    underscore) that an averaging/LANCZOS downsample would wash out below the
    threshold.  A sharp ink threshold also lets `threshold` select by colour
    (e.g. keep dark glyphs, drop a faint grey subtitle) without anti-alias
    fringe leaking in as coverage."""
    import cairosvg
    import numpy as np
    from PIL import Image

    png = cairosvg.svg2png(
        url=svg_path, output_width=supersample, background_color="white"
    )
    im = Image.open(io.BytesIO(png)).convert("L")
    a = np.asarray(im, dtype=np.float32) / 255.0
    h0, w0 = a.shape
    target_h = max(1, round(target_w * h0 / w0))
    ink = (a < threshold).astype(np.uint8) * 255           # sharp high-res ink
    cov = np.asarray(
        Image.fromarray(ink).resize((target_w, target_h), Image.BOX),
        dtype=np.float32,
    ) / 255.0                                              # per-cell ink coverage
    return [
        [1 if cov[y, x] >= coverage else 0 for x in range(target_w)]
        for y in range(target_h)
    ]


def crop(grid):
    """Crop to the in-mask bounding box (drop empty margins around the ink)."""
    h = len(grid)
    w = len(grid[0])
    ys = [y for y in range(h) if any(grid[y])]
    xs = [x for x in range(w) if any(grid[y][x] for y in range(h))]
    if not ys:
        sys.exit("gen_mask_svg.py: the SVG rasterised to an empty mask "
                 "(try a lower --threshold or check the artwork is dark-on-light)")
    y0, y1 = min(ys), max(ys)
    x0, x1 = min(xs), max(xs)
    return [row[x0 : x1 + 1] for row in grid[y0 : y1 + 1]]


def dilate(grid, r):
    """Grow the in-mask region by r cells (8-connected) -- thickens thin strokes
    so a line-art cutout reads as visible channels rather than hairlines."""
    if r <= 0:
        return grid
    h = len(grid)
    w = len(grid[0])
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if grid[y][x]:
                for dy in range(-r, r + 1):
                    ny = y + dy
                    if 0 <= ny < h:
                        row = out[ny]
                        for dx in range(-r, r + 1):
                            nx = x + dx
                            if 0 <= nx < w:
                                row[nx] = 1
    return out


def pad(grid, m):
    """Centre the mask in a canvas with m extra out-of-mask cells on every side."""
    if m <= 0:
        return grid
    h = len(grid)
    w = len(grid[0])
    out = [[0] * (w + 2 * m) for _ in range(h + 2 * m)]
    for y in range(h):
        for x in range(w):
            out[y + m][x + m] = grid[y][x]
    return out


def invert(grid):
    """Complement: in-mask <-> out-of-mask.  Turns a silhouette into a cutout
    (the maze fills AROUND the shape and the shape becomes holes)."""
    return [[0 if c else 1 for c in row] for row in grid]


def emit(grid, name):
    """Render the grid as a Trix [w h [rows]] mask literal."""
    h = len(grid)
    w = len(grid[0])
    lines = [
        "% Auto-generated by tools/gen_mask_svg.py -- do not edit by hand.",
        f"% Mask shape registered as '{name}'; format is [w h [rows]] where each row",
        "% is a w-byte string and a non-zero byte ('#') marks an in-mask cell.",
        f"MASK-SHAPES ({name}) [ {w} {h} [",
    ]
    for row in grid:
        lines.append("  (" + "".join(IN_CHAR if c else OUT_CHAR for c in row) + ")")
    lines.append("] ] put")
    return "\n".join(lines) + "\n"


def build(svg_path, target_w, threshold, coverage, supersample,
          do_dilate, do_invert, margin):
    grid = rasterize(svg_path, target_w, threshold, coverage, supersample)
    grid = crop(grid)
    grid = dilate(grid, do_dilate)
    if do_invert:
        grid = invert(pad(grid, margin))
    elif margin > 0:
        grid = pad(grid, margin)
    return grid


def main():
    ap = argparse.ArgumentParser(
        description="Rasterise an SVG into a Trix mask shape for amazing.trx.")
    ap.add_argument("svg", nargs="?", help="source .svg (default: the bundled logo)")
    ap.add_argument("--name", help="mask name / output stem (default: from the file)")
    ap.add_argument("--width", type=int, default=150,
                    help="mask width in cells (height follows the aspect; default 150)")
    ap.add_argument("--threshold", type=float, default=0.5,
                    help="luminance below this counts as ink (0..1; default 0.5). "
                         "Lower it to drop faint/grey elements, e.g. a subtitle.")
    ap.add_argument("--coverage", type=float, default=0.06,
                    help="fraction of a cell that must be ink to mark it in-mask "
                         "(0..1; default 0.06 -- small, so thin strokes survive)")
    ap.add_argument("--dilate", type=int, default=0,
                    help="grow the ink by N cells before masking (thickens strokes)")
    ap.add_argument("--invert", action="store_true",
                    help="ship the cutout: maze fills AROUND the shape (line-art)")
    ap.add_argument("--margin", type=int, default=0,
                    help="out-of-mask border cells on every side (with --invert: the "
                         "surrounding maze thickness)")
    ap.add_argument("--supersample", type=int, default=0,
                    help="render width before downsampling (default: auto)")
    ap.add_argument("--out-dir", default=DEFAULT_OUT,
                    help="where to write <name>.trx (default examples/mask-shapes)")
    args = ap.parse_args()

    _require_deps()

    if args.svg is None:
        # Default: regenerate the bundled logo cutout exactly as `--mask logo`
        # consumes it (a large maze with the slash-art logo carved out).
        args.svg = LOGO_SVG
        if args.name is None:
            args.name = "trix-logo"
        # 2x: smooth (un-jaggy) logo edges while a plain `--mask logo` still
        # renders to a sane ~3000px image in reasonable time (the dispatch
        # auto-sizes --cell-px for this mask).  dilate/margin scale with it.
        args.width = 300
        args.dilate = max(args.dilate, 2)
        args.invert = True
        if args.margin == 0:
            args.margin = 32
        # Keep the dark slash-art (+ purple accent dot ~0.33) but drop the faint
        # grey subtitle (~0.45) and the divider rule -- they are illegible noise
        # as maze holes.
        args.threshold = 0.4

    name = args.name or os.path.splitext(os.path.basename(args.svg))[0]
    ss = args.supersample or min(2400, max(1200, args.width * 8))

    grid = build(args.svg, args.width, args.threshold, args.coverage, ss,
                 args.dilate, args.invert, args.margin)

    os.makedirs(args.out_dir, exist_ok=True)
    out_path = os.path.join(args.out_dir, name + ".trx")
    with open(out_path, "w") as fh:
        fh.write(emit(grid, name))

    incount = sum(sum(r) for r in grid)
    print("%s: %dx%d mask, %d in-mask cells (%.1f%%)%s"
          % (os.path.relpath(out_path, REPO), len(grid[0]), len(grid),
             incount, 100.0 * incount / (len(grid) * len(grid[0])),
             "  [cutout]" if args.invert else ""))


if __name__ == "__main__":
    main()
