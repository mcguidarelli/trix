#!/bin/bash
# ====================================================================
# examples/gallery.sh -- generate a gallery of amazing.trx output PNGs
# ====================================================================
# Drives examples/amazing.trx through every showcase combination and
# writes the PNGs into examples/maze-gallery/.  Designed to be run on
# demand -- the gallery itself is NOT committed to git (binary blobs).
#
# Usage:
#   examples/gallery.sh              Default: 86 PNGs
#   examples/gallery.sh --full       Adds the 200x200 --stress entry and the
#                                    full-res 1000x1000 --monster (~3 min)
#   examples/gallery.sh --quiet      Suppresses per-step echo
#
# All runs use a fixed seed (42) so the output is bit-exact reproducible
# across machines.  --vm-size is set high enough for the heaviest case
# (theta polar render); generally amazing.trx itself runs fine on 8M.
# ====================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# Prefer the optimized binary -- the gallery (especially --full's ~3-minute
# 1000x1000 monster) is far faster than the -O0 + ASan debug build.  Falls back
# to ./trix, and honors an explicit TRIX=... override.
TRIX="${TRIX:-$ROOT_DIR/trix.opt}"
[[ -x "$TRIX" ]] || TRIX="$ROOT_DIR/trix"
AMAZING="$SCRIPT_DIR/amazing.trx"
OUT_DIR="$SCRIPT_DIR/maze-gallery"

FULL=0
QUIET=0
for arg in "$@"; do
    case "$arg" in
        --full)  FULL=1 ;;
        --quiet) QUIET=1 ;;
        -h|--help)
            sed -n '3,18p' "$0"; exit 0 ;;
        *)
            echo "Unknown flag: $arg (try --help)" >&2; exit 1 ;;
    esac
done

if [[ ! -x "$TRIX" ]]; then
    echo "error: $TRIX not found or not executable" >&2
    echo "build it first with ./build.sh from $ROOT_DIR" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

# Wraps trix invocation with --vm-size=128M --quiet --seed 42 baseline.
# $1 is the output filename (relative to OUT_DIR); remaining args go
# straight through to amazing.trx.
run() {
    local outname="$1"; shift
    [[ "$QUIET" -eq 0 ]] && printf "  %-32s " "$outname"
    "$TRIX" --vm-size=128M "$AMAZING" --quiet --seed 42 "$@" --out "$OUT_DIR/$outname" >/dev/null
    [[ "$QUIET" -eq 0 ]] && echo "ok"
    # explicit success: under --quiet the [[ ]] && echo above returns 1,
    # and set -e treats a non-zero function return as fatal
    return 0
}

[[ "$QUIET" -eq 0 ]] && echo "Algorithms (mono, square 25x25, seed 42):"
for algo in backtrack kruskal wilson eller binary-tree sidewinder aldous-broder prim hunt-kill growing-tree division; do
    run "algo-${algo}.png" --algo "$algo" --size 25x25
done

# Curated colored showcase: recursive-division's nested-room structure is
# most legible as a distance heatmap (the bands trace the subdivision).
[[ "$QUIET" -eq 0 ]] && echo "Recursive-division showcase (magma + turbo heatmaps):"
run "algo-division-magma.png" --algo division --color magma --size 30x30  --cell-px 16
run "algo-division-turbo.png" --algo division --color turbo --size 100x100 --cell-px 6 --wall-px 1

[[ "$QUIET" -eq 0 ]] && echo "Colormaps (kruskal, square 30x30):"
for color in viridis magma inferno plasma cividis turbo rainbow cubehelix grayscale two-tone; do
    run "color-${color}.png" --algo kruskal --color "$color" --size 30x30
done

[[ "$QUIET" -eq 0 ]] && echo "Features:"
run "feature-solve.png"      --algo backtrack --color viridis --solve --size 25x25
run "feature-braid-0.5.png"  --algo backtrack --braid 0.5 --size 20x20
run "feature-braid-1.0.png"  --algo backtrack --braid 1.0 --size 20x20
run "feature-weave.png"      --algo backtrack --weave --size 25x25 --cell-px 18

[[ "$QUIET" -eq 0 ]] && echo "Grids:"
run "grid-square.png"     --algo backtrack --size 20x20
run "grid-hex.png"        --grid hex --size 16x16 --cell-px 14
run "grid-theta.png"      --grid theta --size 6x12 --cell-px 18

[[ "$QUIET" -eq 0 ]] && echo "Hex / theta color (Group 8):"
run "grid-hex-viridis.png"    --grid hex --color viridis --size 14x14 --cell-px 14
run "grid-hex-magma.png"      --grid hex --color magma --size 14x14 --cell-px 14
run "grid-theta-viridis.png"  --grid theta --color viridis --size 6x12 --cell-px 18
run "grid-theta-inferno.png"  --grid theta --color inferno --size 6x12 --cell-px 18

[[ "$QUIET" -eq 0 ]] && echo "Hex / theta solve (Group 9):"
run "grid-hex-solve.png"      --grid hex --color viridis --solve --size 14x14 --cell-px 14
run "grid-theta-solve.png"    --grid theta --color magma --solve --size 6x12 --cell-px 18

[[ "$QUIET" -eq 0 ]] && echo "Hex / theta braid (Group 10):"
run "grid-hex-braid.png"      --grid hex --braid 1.0 --size 14x14 --cell-px 14
run "grid-theta-braid.png"    --grid theta --braid 0.5 --size 6x12 --cell-px 18

[[ "$QUIET" -eq 0 ]] && echo "Triangle grid (Group 11):"
run "grid-triangle.png"        --grid triangle --size 16x12 --cell-px 24

[[ "$QUIET" -eq 0 ]] && echo "Triangle parity (color / solve / braid):"
run "grid-triangle-viridis.png" --grid triangle --color viridis --size 16x12 --cell-px 24
run "grid-triangle-inferno.png" --grid triangle --color inferno --size 16x12 --cell-px 24
run "grid-triangle-solve.png"   --grid triangle --color magma --solve --size 14x10 --cell-px 28
run "grid-triangle-braid.png"   --grid triangle --braid 0.5 --size 16x12 --cell-px 24

[[ "$QUIET" -eq 0 ]] && echo "Upsilon grid (Group 12 -- octagon+square):"
run "grid-upsilon.png"          --grid upsilon --size 12x10 --cell-px 24
run "grid-upsilon-viridis.png"  --grid upsilon --color viridis --size 12x10 --cell-px 24
run "grid-upsilon-magma.png"    --grid upsilon --color magma --size 12x10 --cell-px 24
run "grid-upsilon-solve.png"    --grid upsilon --color viridis --solve --size 10x8 --cell-px 24

# The capstone: every portable algorithm carving every non-square grid,
# driven by the shared topology descriptor (Section 7D-ter).  4 grids x 7
# portable algos = 28 colored tiles.
[[ "$QUIET" -eq 0 ]] && echo "Algorithm x topology matrix (portable algos, viridis):"
for grid in hex theta triangle upsilon; do
    for algo in backtrack kruskal wilson aldous-broder prim hunt-kill growing-tree; do
        run "matrix-${grid}-${algo}.png" --grid "$grid" --algo "$algo" \
            --color viridis --size 16x16 --cell-px 14
    done
done

# Masking: carve the maze into a word / logo / analytic figure (square grid).
# Disconnected shapes (letters) become one perfect maze per connected piece.
[[ "$QUIET" -eq 0 ]] && echo "Masking (shape-carved mazes):"
run "mask-logo.png"          --mask logo
run "mask-text-amazing.png"  --mask-text AMAZING --color viridis
run "mask-disc.png"          --mask disc  --size 28x28 --color inferno
run "mask-ring.png"          --mask ring  --size 28x28 --color magma
run "mask-frame.png"         --mask frame --size 28x28
run "mask-trix-invert.png"   --mask-text TRIX --mask-invert --color magma

[[ "$QUIET" -eq 0 ]] && echo "Compare:"
run "compare-4-algos.png" --compare backtrack,kruskal,wilson,eller --size 16x16

[[ "$QUIET" -eq 0 ]] && echo "Monster (colored heatmap, ~15s):"
run "monster-magma.png" --algo backtrack --color magma --size 400x400 --cell-px 3 --wall-px 1
# Recursive division at monster scale: the rainbow distance field paints the
# nested-room hierarchy as broad color blocks -- the algorithm's structure at a glance.
run "monster-division-rainbow.png" --algo division --color rainbow --size 400x400 --cell-px 3 --wall-px 1

if [[ "$FULL" -eq 1 ]]; then
    [[ "$QUIET" -eq 0 ]] && echo "Stress (slow):"
    run "stress.png" --stress
    [[ "$QUIET" -eq 0 ]] && echo "Monster (full-res mono, ~3 min):"
    run "monster.png" --monster
fi

count=$(find "$OUT_DIR" -maxdepth 1 -name '*.png' -type f | wc -l)
total_size=$(du -sh "$OUT_DIR" | cut -f1)
echo
echo "wrote $count PNGs ($total_size) to $OUT_DIR/"
