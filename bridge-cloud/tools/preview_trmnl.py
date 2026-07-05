#!/usr/bin/env python3
"""Render a TRMNL X dashboard bundle (0xCFB2) to grayscale PNGs on the host.

This runs the SAME fit-to-screen grid layout the firmware uses (square grid
cells, letterboxed) so we can eyeball layout + the threshold->gray mapping
before ever flashing hardware. The output is a 1872x1404 8-bit grayscale image
per dashboard, quantized to the panel's 16 real gray levels.

Usage:
    python tools/preview_trmnl.py BUNDLE.bin [OUTDIR]

where BUNDLE.bin is the unsealed bundle from:
    python -m grafana_push.push_trmnl --dry-run BUNDLE.bin

The layout math here is the reference for firmware-trmnl/dashboard_renderer.cpp
— keep the two in sync (constants are called out in ALL_CAPS below).
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# Make `grafana_push` importable when run straight from the repo.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))
from grafana_push.data_trmnl import (  # noqa: E402
    PANEL_STAT,
    decode_dashboard_bundle,
)

# --- Shared layout constants (mirror in firmware) ---------------------------
SCREEN_W = 1872
SCREEN_H = 1404
GRID_COLS = 24
# Bezel safe-area insets (mirror firmware config.h) — keep content off the frame.
SAFE_TOP = 48
SAFE_BOTTOM = 28
SAFE_LEFT = 28
SAFE_RIGHT = 28
PANEL_GUTTER = 6        # px inset inside each panel cell rect
TITLE_FRAC = 0.16       # title band height as fraction of panel height
SERIES_GRAYS = [0, 6, 10, 3]   # per-series line shades (0=black, darkest first)


def g2l(g: int) -> int:
    """Gray level 0..15 -> 8-bit luminance, quantized to the 16 real levels."""
    return int(round(max(0, min(15, g)) * 255 / 15))


def tile_bg(g: int) -> int:
    """Very light tint of a status gray for a stat tile background (keeps black
    text legible)."""
    if g >= 15:
        return g2l(15)
    light = g + (15 - g) * 3 // 4
    return g2l(light)


def _font(size: int) -> ImageFont.FreeTypeFont:
    size = max(8, int(size))
    for path in (
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def _text_wh(draw: ImageDraw.ImageDraw, s: str, font) -> tuple[int, int]:
    l, t, r, b = draw.textbbox((0, 0), s, font=font)
    return r - l, b - t


def _fit_font(draw, text, max_w, start_px, min_px=10):
    """Largest font (<= start_px) whose text fits in max_w, down to min_px."""
    px = int(start_px)
    while px > min_px:
        f = _font(px)
        if _text_wh(draw, text, f)[0] <= max_w:
            return f
        px -= 2
    return _font(min_px)


def _cell_xy(grid_rows: int) -> tuple[float, float]:
    # Stretch to fill the safe area (screen minus bezel inset), independent x/y.
    area_w = SCREEN_W - SAFE_LEFT - SAFE_RIGHT
    area_h = SCREEN_H - SAFE_TOP - SAFE_BOTTOM
    return area_w / GRID_COLS, area_h / max(1, grid_rows)


def _draw_timeseries(draw, x, y, w, h, panel):
    title = panel["title"]
    tb_h = int(min(max(h * TITLE_FRAC, 26), 54))
    tf = _fit_font(draw, title, w - 20, tb_h * 0.82)
    _, tth = _text_wh(draw, title, tf)
    draw.text((x + 10, y + 6 + (tb_h - tth) // 2), title, fill=0, font=tf)

    # Plot area with room for y-labels on the left (x-labels dropped).
    lab_f = _font(min(max(11, h * 0.06), 22))
    yl = panel.get("y_labels") or []
    left = x + PANEL_GUTTER + (48 if yl else 10)
    right = x + w - PANEL_GUTTER - 6
    top = y + tb_h + 6
    bot = y + h - PANEL_GUTTER - 6      # no x-axis labels: reclaim the bottom
    if right - left < 20 or bot - top < 20:
        return
    pw, ph = right - left, bot - top

    # Threshold fill bands (y measured bottom-up, normalized 0..1). Grays are
    # already the light shades chosen on the bridge — paint them directly.
    for y0n, y1n, gray in panel.get("bands") or []:
        by0 = bot - int(y1n * ph)
        by1 = bot - int(y0n * ph)
        draw.rectangle([left, by0, right, by1], fill=g2l(gray))

    # Axis + dotted gridlines + y labels.
    draw.line([left, bot, right, bot], fill=0, width=2)
    n = len(yl)
    for i, lab in enumerate(yl):
        gy = bot - int(ph * i / max(1, n - 1))
        for gx in range(left + 2, right, 6):
            draw.point((gx, gy), fill=g2l(10))
        tw, th = _text_wh(draw, lab, lab_f)
        draw.text((left - 6 - tw, gy - th // 2), lab, fill=0, font=lab_f)

    # Series lines.
    for si, pts in enumerate(panel.get("series") or []):
        if len(pts) < 2:
            continue
        shade = g2l(SERIES_GRAYS[si % len(SERIES_GRAYS)])
        xy = [(left + nx * pw, bot - ny * ph) for nx, ny in pts]
        draw.line(xy, fill=shade, width=3, joint="curve")


def _draw_stat(draw, x, y, w, h, panel):
    base = panel.get("base_gray", 15)
    # Light tinted tile + a status accent bar across the top.
    draw.rectangle([x + PANEL_GUTTER, y + PANEL_GUTTER,
                    x + w - PANEL_GUTTER, y + h - PANEL_GUTTER],
                   fill=tile_bg(base), outline=g2l(9), width=1)
    if base < 15:
        draw.rectangle([x + PANEL_GUTTER, y + PANEL_GUTTER,
                        x + w - PANEL_GUTTER, y + PANEL_GUTTER + max(6, h // 18)],
                       fill=g2l(base))

    tf = _fit_font(draw, panel["title"], w - 28, min(max(h * 0.15, 20), 40))
    draw.text((x + 16, y + 14), panel["title"], fill=0, font=tf)

    value = panel.get("value_str") or "-"
    unit = panel.get("unit") or ""
    vf = _fit_font(draw, value + unit, w - 40, h * 0.42)
    uf = _font(_text_wh(draw, "0", vf)[1] * 0.55)
    vw, vh = _text_wh(draw, value, vf)
    uw, uh = _text_wh(draw, unit, uf) if unit else (0, 0)
    total = vw + (uw + 6 if unit else 0)
    cx = x + w // 2 - total // 2
    cy = y + h // 2 - vh // 2
    draw.text((cx, cy), value, fill=0, font=vf)
    if unit:
        draw.text((cx + vw + 6, cy + (vh - uh)), unit, fill=g2l(4), font=uf)

    # Sparkline in the bottom band.
    spark = panel.get("sparkline") or []
    if len(spark) >= 2:
        sl = x + 16
        sr = x + w - 16
        sb = y + h - 14
        st = y + int(h * 0.72)
        sw, sh = sr - sl, sb - st
        if sw > 10 and sh > 6:
            xy = [(sl + nx * sw, sb - ny * sh) for nx, ny in spark]
            draw.line(xy, fill=g2l(3), width=2, joint="curve")


def render_dashboard(dash: dict) -> Image.Image:
    img = Image.new("L", (SCREEN_W, SCREEN_H), g2l(15))
    draw = ImageDraw.Draw(img)
    grid_rows = dash.get("grid_rows", 1)
    cell_x, cell_y = _cell_xy(grid_rows)

    for p in dash.get("panels") or []:
        x = SAFE_LEFT + int(p["gx"] * cell_x)
        y = SAFE_TOP + int(p["gy"] * cell_y)
        w = int(p["gw"] * cell_x)
        h = int(p["gh"] * cell_y)
        # Light panel frame for visual separation (timeseries only; stat tiles
        # draw their own tinted card).
        if p["type"] != PANEL_STAT:
            draw.rectangle([x + PANEL_GUTTER, y + PANEL_GUTTER,
                            x + w - PANEL_GUTTER, y + h - PANEL_GUTTER],
                           outline=g2l(11), width=1)
            _draw_timeseries(draw, x, y, w, h, p)
        else:
            _draw_stat(draw, x, y, w, h, p)
    return img


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    bundle_path = Path(sys.argv[1])
    outdir = Path(sys.argv[2]) if len(sys.argv) > 2 else bundle_path.parent / "trmnl_preview"
    outdir.mkdir(parents=True, exist_ok=True)

    decoded = decode_dashboard_bundle(bundle_path.read_bytes())
    print(f"bundle v{decoded['version']} next_poll={decoded['next_poll']}s "
          f"dashboards={len(decoded['dashboards'])}")
    written = []
    for i, dash in enumerate(decoded["dashboards"]):
        img = render_dashboard(dash)
        safe = "".join(c if c.isalnum() else "_" for c in dash.get("title", str(i)))
        out = outdir / f"dashboard_{i}_{safe}.png"
        img.save(out)
        written.append(out)
        print(f"  [{i}] '{dash['title']}' {len(dash['panels'])} panels -> {out}")
    print(f"wrote {len(written)} PNGs to {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
