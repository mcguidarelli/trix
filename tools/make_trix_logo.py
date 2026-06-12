#!/usr/bin/env python3
"""Generate the Trix logo badge: a crisp PNG plus a matching SVG.

The ASCII logo is laid out glyph-by-glyph on a fixed cell grid so it aligns
exactly regardless of how a renderer measures a monospace advance.  The PNG is
rasterized with Pillow + DejaVu Sans Mono; the SVG embeds that same font as a
data-URI @font-face so every viewer renders identical glyphs (a plain
font-family reference renders "odd" wherever the font is substituted).

Usage:
    python3 tools/make_trix_logo.py --out assets/trix-logo      # light square (default)
    python3 tools/make_trix_logo.py --dark --landscape --no-caption
Run with -h for the full flag list.
"""
import argparse
import base64

from PIL import Image, ImageDraw, ImageFont

# ---- The logo (exact, including leading spaces; U+00B7 middle dot) ----
ART = [
    "  ______    _",
    " /_  __/___(_)_  __",
    "  / / / __/ /\\ \\/ /",
    " / / / / / /  > · <",
    "/_/ /_/ /_/  /_/\\_\\",
]
ROWS = len(ART)
COLS = max(len(line) for line in ART)
ACCENT_CHAR = "·"

FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
FONT_REG = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"

# Palettes -------------------------------------------------------------------
LIGHT = dict(bg_top=(255, 255, 255), bg_bot=(246, 248, 250), art=(23, 27, 38),
             caption=(107, 114, 128), border=(224, 228, 233))
DARK = dict(bg_top=(20, 24, 36), bg_bot=(12, 15, 23), art=(232, 237, 244),
            caption=(138, 148, 166), border=(45, 212, 191))


def _hex(c):
    return "#%02x%02x%02x" % tuple(c)


def _font_face(path, family):
    try:
        with open(path, "rb") as f:
            b64 = base64.b64encode(f.read()).decode("ascii")
        return (f"@font-face{{font-family:'{family}';font-weight:normal;"
                f"src:url(data:font/ttf;base64,{b64}) format('truetype');}}")
    except OSError:
        return ""


def render(out_stem, *, caption="STACK-BASED SCRIPTING LANGUAGE", palette=LIGHT,
           accent=(124, 58, 237), round_dot=True, square=True, transparent=False,
           fs_art=100, pad_x=64, ss=2):
    bg_top, bg_bot = palette["bg_top"], palette["bg_bot"]
    art_color, caption_color, border = palette["art"], palette["caption"], palette["border"]

    # ---- Grid geometry (final-resolution units, supersampled by ss) ----
    cell_w = round(fs_art * 0.602)          # DejaVu Sans Mono advance
    cell_h = round(fs_art * 1.16)
    gap, cap_h, pad_v = 34, 58, 86
    fs_cap = round(fs_art * 0.33)

    art_w, art_h = COLS * cell_w, ROWS * cell_h
    content_h = art_h + (gap + cap_h if caption else 0)
    W = art_w + 2 * pad_x
    H = W if square else content_h + 2 * pad_v
    art_x0 = (W - art_w) // 2
    art_y0 = (H - content_h) // 2           # vertically center the content block
    dot_r = round(cell_w * 0.24)

    # Caption layout, shared by PNG + SVG: letter-track, then shrink the font
    # until it fits inside the art width so it never clips the rounded frame.
    cap_tracked, cap_fs = None, fs_cap
    if caption:
        cap_tracked = "   ".join(" ".join(w) for w in caption.split(" "))
        cw = ImageFont.truetype(FONT_REG, fs_cap).getlength(cap_tracked)
        target = art_w * 0.94
        if cw > target:
            cap_fs = max(16, int(fs_cap * target / cw))

    # ---- PNG ----
    Wq, Hq = W * ss, H * ss
    img = Image.new("RGBA", (Wq, Hq), (0, 0, 0, 0))
    panel = Image.new("RGB", (Wq, Hq), bg_bot)
    pd = ImageDraw.Draw(panel)
    for y in range(Hq):
        t = y / max(1, Hq - 1)
        pd.line([(0, y), (Wq, y)],
                fill=tuple(round(bg_top[i] + (bg_bot[i] - bg_top[i]) * t) for i in range(3)))
    radius = 30 * ss
    mask = Image.new("L", (Wq, Hq), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, Wq - 1, Hq - 1], radius=radius, fill=255)
    if not transparent:
        img.paste(panel, (0, 0), mask)

    draw = ImageDraw.Draw(img)
    inset = 3 * ss
    draw.rounded_rectangle([inset, inset, Wq - 1 - inset, Hq - 1 - inset],
                           radius=radius - inset, outline=tuple(border) + (170,), width=max(2, ss))

    font_art = ImageFont.truetype(FONT_BOLD, fs_art * ss)
    for r, line in enumerate(ART):
        cy = (art_y0 + r * cell_h + cell_h // 2) * ss
        for c, ch in enumerate(line):
            if ch == " ":
                continue
            cx = (art_x0 + c * cell_w + cell_w // 2) * ss
            if ch == ACCENT_CHAR and round_dot:
                rr = dot_r * ss
                draw.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=accent)
            else:
                draw.text((cx, cy), ch, font=font_art, anchor="mm",
                          fill=(accent if ch == ACCENT_CHAR else art_color))

    if caption:
        font_cap = ImageFont.truetype(FONT_REG, cap_fs * ss)
        rule_w = art_w * ss // 3
        ry = (art_y0 + art_h + gap - 16) * ss
        draw.line([(W // 2 * ss - rule_w // 2, ry), (W // 2 * ss + rule_w // 2, ry)],
                  fill=tuple(accent) + (140,), width=max(1, ss))
        draw.text((W // 2 * ss, (art_y0 + art_h + gap + cap_h // 2) * ss),
                  cap_tracked, font=font_cap, anchor="mm", fill=caption_color)

    img.resize((W, H), Image.LANCZOS).save(out_stem + ".png")
    print(f"wrote {out_stem}.png  ({W}x{H})")

    # ---- SVG (same grid; embedded font so glyphs match the PNG everywhere) ----
    esc = {"<": "&lt;", ">": "&gt;", "&": "&amp;"}
    faces = _font_face(FONT_BOLD, "TrixMono") + _font_face(FONT_REG, "TrixMonoReg")
    art_family = "'TrixMono','DejaVu Sans Mono',monospace"
    cap_family = "'TrixMonoReg','DejaVu Sans Mono',monospace"
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">']
    if faces:
        s.append(f"  <style>{faces}</style>")
    s.append('  <defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
             f'<stop offset="0" stop-color="{_hex(bg_top)}"/>'
             f'<stop offset="1" stop-color="{_hex(bg_bot)}"/></linearGradient></defs>')
    if not transparent:
        s.append(f'  <rect width="{W}" height="{H}" rx="30" fill="url(#bg)"/>')
    s.append(f'  <rect x="3" y="3" width="{W-6}" height="{H-6}" rx="27" fill="none" '
             f'stroke="{_hex(border)}" stroke-opacity="0.67" stroke-width="2"/>')
    s.append(f'  <g font-family="{art_family}" font-size="{fs_art}" text-anchor="middle" '
             f'dominant-baseline="central">')
    for r, line in enumerate(ART):
        cy = art_y0 + r * cell_h + cell_h // 2
        for c, ch in enumerate(line):
            if ch == " ":
                continue
            cx = art_x0 + c * cell_w + cell_w // 2
            if ch == ACCENT_CHAR and round_dot:
                s.append(f'    <circle cx="{cx}" cy="{cy}" r="{dot_r}" fill="{_hex(accent)}"/>')
            else:
                fill = _hex(accent) if ch == ACCENT_CHAR else _hex(art_color)
                s.append(f'    <text x="{cx}" y="{cy}" fill="{fill}">{esc.get(ch, ch)}</text>')
    s.append("  </g>")
    if caption:
        ry = art_y0 + art_h + gap - 16
        rule_w = art_w // 3
        s.append(f'  <line x1="{W//2-rule_w//2}" y1="{ry}" x2="{W//2+rule_w//2}" y2="{ry}" '
                 f'stroke="{_hex(accent)}" stroke-opacity="0.55" stroke-width="1"/>')
        cap_cy = art_y0 + art_h + gap + cap_h // 2
        s.append(f'  <text x="{W//2}" y="{cap_cy}" text-anchor="middle" dominant-baseline="central" '
                 f'font-family="{cap_family}" font-size="{cap_fs}" '
                 f'fill="{_hex(caption_color)}">'
                 f'{"".join(esc.get(ch, ch) for ch in cap_tracked)}</text>')
    s.append("</svg>")
    with open(out_stem + ".svg", "w") as f:
        f.write("\n".join(s) + "\n")
    print(f"wrote {out_stem}.svg")


def main():
    ap = argparse.ArgumentParser(description="Generate the Trix logo badge (PNG + SVG).")
    ap.add_argument("--out", default="trix-logo", help="output path stem (no extension)")
    ap.add_argument("--dark", action="store_true", help="dark theme instead of light")
    ap.add_argument("--landscape", action="store_true", help="landscape instead of square")
    ap.add_argument("--no-caption", action="store_true", help="omit the subtitle")
    ap.add_argument("--accent", default="#7c3aed", help="accent hex for the dot (default violet)")
    ap.add_argument("--caption", default="STACK-BASED SCRIPTING LANGUAGE")
    args = ap.parse_args()
    h = args.accent.lstrip("#")
    accent = tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
    render(args.out, caption=None if args.no_caption else args.caption,
           palette=DARK if args.dark else LIGHT, accent=accent, square=not args.landscape)


if __name__ == "__main__":
    main()
