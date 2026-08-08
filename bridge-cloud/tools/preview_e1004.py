#!/usr/bin/env python3
"""Render a reTerminal E1004 color dashboard bundle (0xCFB2 v3) to RGB PNGs.

This runs the SAME fit-to-screen grid layout the E1004 firmware uses, on the
panel's landscape 1600x1200 geometry, so we can eyeball layout + the
threshold->Spectra 6 color mapping before ever flashing hardware. Every
Grafana threshold zone renders (green included, matching Grafana's "area"
style); only white/transparent zones draw nothing.

The device dithers status colors with white to fake light tints (Spectra 6 has
six solid colors, no shades); the preview approximates those dithers as pale
RGB fills. Solid ink (text, lines, accent bars) uses approximate Spectra 6
ink colors.

Usage:
    python tools/preview_e1004.py BUNDLE.bin [OUTDIR]

where BUNDLE.bin is the unsealed bundle from:
    python -m grafana_push.push_e1004 --dry-run BUNDLE.bin

The layout math here is the reference for firmware-e1004/dashboard_renderer.cpp
— keep the two in sync (constants are called out in ALL_CAPS below).
"""
from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))
from grafana_push.data_trmnl import (  # noqa: E402
    PANEL_STAT,
    SPECTRA_BLACK,
    SPECTRA_BLUE,
    SPECTRA_GREEN,
    SPECTRA_RED,
    SPECTRA_WHITE,
    SPECTRA_YELLOW,
    decode_dashboard_bundle,
)

# --- Shared layout constants (mirror in firmware-e1004) ----------------------
SCREEN_W = 1600          # landscape: panel is 1200x1600 portrait, rotated 90°
SCREEN_H = 1200
GRID_COLS = 24
SAFE_TOP = 40
SAFE_BOTTOM = 24
SAFE_LEFT = 24
SAFE_RIGHT = 24
PANEL_GUTTER = 6
TITLE_FRAC = 0.16
# Per-series line colors (Spectra codes, darkest/primary first).
SERIES_COLORS = [SPECTRA_BLACK, SPECTRA_BLUE, SPECTRA_RED, SPECTRA_GREEN]

# Approximate ink colors of the Spectra 6 palette for the host preview.
INK = {
    SPECTRA_BLACK: (20, 20, 20),
    SPECTRA_WHITE: (255, 255, 255),
    SPECTRA_RED: (190, 35, 35),
    SPECTRA_YELLOW: (225, 180, 40),
    SPECTRA_GREEN: (55, 150, 70),
    SPECTRA_BLUE: (45, 90, 190),
}

GRIDLINE = (190, 190, 190)   # device: sparse black dither
LABEL = (90, 90, 90)         # device: solid black, small thin font


def tint(code: int, alpha: float) -> tuple[int, int, int]:
    """Pale blend of a Spectra ink over white — how a device-side dither of
    `alpha` coverage reads at arm's length."""
    r, g, b = INK.get(code, (255, 255, 255))
    return (int(255 + (r - 255) * alpha),
            int(255 + (g - 255) * alpha),
            int(255 + (b - 255) * alpha))


# Dither coverages (mirror firmware): threshold bands use the same tint as
# the stat tile backgrounds.
BAND_ALPHA = 0.25
TILE_ALPHA = 0.25


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
    px = int(start_px)
    while px > min_px:
        f = _font(px)
        if _text_wh(draw, text, f)[0] <= max_w:
            return f
        px -= 2
    return _font(min_px)


def _cell_xy(grid_rows: int) -> tuple[float, float]:
    area_w = SCREEN_W - SAFE_LEFT - SAFE_RIGHT
    area_h = SCREEN_H - SAFE_TOP - SAFE_BOTTOM
    return area_w / GRID_COLS, area_h / max(1, grid_rows)


def _draw_timeseries(draw, x, y, w, h, panel):
    title = panel["title"]
    tb_h = int(min(max(h * TITLE_FRAC, 24), 48))

    yl = panel.get("y_labels") or []
    left = x + PANEL_GUTTER + (44 if yl else 10)
    right = x + w - PANEL_GUTTER - 6
    top = y + tb_h + 6
    bot = y + h - PANEL_GUTTER - 6

    tf = _fit_font(draw, title, (x + w - PANEL_GUTTER) - left, tb_h * 0.82)
    _, tth = _text_wh(draw, title, tf)
    draw.text((left, y + 6 + (tb_h - tth) // 2), title, fill=INK[SPECTRA_BLACK], font=tf)

    if right - left < 20 or bot - top < 20:
        return
    pw, ph = right - left, bot - top

    # Threshold fill bands: pale tints of the real hue (device dithers).
    for y0n, y1n, code in panel.get("bands") or []:
        by0 = bot - int(y1n * ph)
        by1 = bot - int(y0n * ph)
        draw.rectangle([left, by0, right, by1], fill=tint(code, BAND_ALPHA))

    lab_f = _font(min(max(11, h * 0.055), 17))
    n = len(yl)
    pos = panel.get("y_label_pos") or []
    for i, lab in enumerate(yl):
        p = pos[i] if i < len(pos) else (i / (n - 1) if n > 1 else 0.0)
        gy = bot - int(max(0.0, min(1.0, p)) * ph)
        for gx in range(left + 2, right, 6):
            draw.point((gx, gy), fill=GRIDLINE)
        tw, th = _text_wh(draw, lab, lab_f)
        draw.text((left - 6 - tw, gy - th // 2), lab, fill=LABEL, font=lab_f)

    for si, pts in enumerate(panel.get("series") or []):
        if len(pts) < 2:
            continue
        code = SERIES_COLORS[si % len(SERIES_COLORS)]
        xy = [(left + nx * pw, bot - ny * ph) for nx, ny in pts]
        draw.line(xy, fill=INK[code], width=3, joint="curve")


def _draw_stat(draw, x, y, w, h, panel):
    base = panel.get("base_gray", SPECTRA_WHITE)   # v3: Spectra code
    x0, y0 = x + PANEL_GUTTER, y + PANEL_GUTTER
    x1, y1 = x + w - PANEL_GUTTER, y + h - PANEL_GUTTER
    # Pale tinted card only — the tint alone carries the status; no border,
    # no accent bar.
    draw.rectangle([x0, y0, x1, y1], fill=tint(base, TILE_ALPHA))

    tf = _fit_font(draw, panel["title"], w - 28, min(max(h * 0.15, 18), 36))
    draw.text((x + 16, y + 18), panel["title"], fill=INK[SPECTRA_BLACK], font=tf)

    vu = (panel.get("value_str") or "-") + (panel.get("unit") or "")
    vf = _fit_font(draw, vu, w - 24, h * 0.42)
    vw, vh = _text_wh(draw, vu, vf)
    draw.text((x + w // 2 - vw // 2, y + h // 2 - vh // 2), vu,
              fill=INK[SPECTRA_BLACK], font=vf)

    spark = panel.get("sparkline") or []
    if len(spark) >= 2:
        sl, sr = x + 16, x + w - 16
        sb, st = y + h - 14, y + int(h * 0.72)
        sw, sh = sr - sl, sb - st
        if sw > 10 and sh > 6:
            xy = [(sl + nx * sw, sb - ny * sh) for nx, ny in spark]
            draw.line(xy, fill=(70, 70, 70), width=2, joint="curve")


def render_dashboard(dash: dict) -> Image.Image:
    img = Image.new("RGB", (SCREEN_W, SCREEN_H), INK[SPECTRA_WHITE])
    draw = ImageDraw.Draw(img)
    cell_x, cell_y = _cell_xy(dash.get("grid_rows", 1))
    for p in dash.get("panels") or []:
        x = SAFE_LEFT + int(p["gx"] * cell_x)
        y = SAFE_TOP + int(p["gy"] * cell_y)
        w = int(p["gw"] * cell_x)
        h = int(p["gh"] * cell_y)
        if p["type"] != PANEL_STAT:
            # No frame around timeseries panels (mirrors firmware).
            _draw_timeseries(draw, x, y, w, h, p)
        else:
            _draw_stat(draw, x, y, w, h, p)
    return img


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    bundle_path = Path(sys.argv[1])
    outdir = Path(sys.argv[2]) if len(sys.argv) > 2 else bundle_path.parent / "e1004_preview"
    outdir.mkdir(parents=True, exist_ok=True)

    decoded = decode_dashboard_bundle(bundle_path.read_bytes())
    print(f"bundle v{decoded['version']} next_poll={decoded['next_poll']}s "
          f"dashboards={len(decoded['dashboards'])}")
    if decoded["version"] != 3:
        print("warning: not a v3 color bundle — shade bytes will be misread as colors")
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
