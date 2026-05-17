from __future__ import annotations

from io import BytesIO

import httpx
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageOps

from .config import PanelConfig

# Top-right corner where Grafana stamps the "Powered by Grafana" watermark on
# rendered panels. Painted white before dithering so the X3 doesn't waste
# pixels on the badge.
WATERMARK_MASK_W = 184  # pixels from the right edge (byte-aligned for clean assertions)
WATERMARK_MASK_H = 120  # pixels from the top edge — Grafana renders the badge at y~69-109


async def fetch_panel_png(
    client: httpx.AsyncClient,
    grafana_url: str,
    token: str,
    panel: PanelConfig,
    width: int,
    height: int,
) -> bytes:
    url = f"{grafana_url}/render/d-solo/{panel.dashboard_uid}/{panel.dashboard_slug}"
    params: dict[str, str | int] = {
        "panelId": panel.panel_id,
        "width": width,
        "height": height,
        "from": panel.from_,
        "to": panel.to,
        "tz": panel.tz,
        "theme": panel.theme,
    }
    for k, v in panel.vars.items():
        params[f"var-{k}"] = v
    r = await client.get(
        url,
        params=params,
        headers={"Authorization": f"Bearer {token}"},
        timeout=30.0,
    )
    r.raise_for_status()
    return r.content


def png_to_framebuffer(
    png_bytes: bytes,
    width: int,
    height: int,
    *,
    contrast: float = 1.0,
    autocontrast: bool = False,
    dilate: int = 0,
) -> bytes:
    img = Image.open(BytesIO(png_bytes))
    if img.size[0] == width and img.size[1] > height:
        # Source is the right width but taller (overscan render to crop the
        # panel title bar). Keep the bottom `height` rows.
        img = img.crop((0, img.size[1] - height, width, img.size[1])).convert("L")
    elif img.size != (width, height):
        img.thumbnail((width, height), Image.Resampling.LANCZOS)
        canvas = Image.new("L", (width, height), 255)
        x = (width - img.size[0]) // 2
        y = (height - img.size[1]) // 2
        canvas.paste(img.convert("L"), (x, y))
        img = canvas
    else:
        img = img.convert("L")

    # Mask the Grafana watermark with a white rectangle. Done on grayscale
    # before any contrast / dither so it ends up as pure white (0xFF).
    # PIL's rectangle is inclusive on both endpoints, so subtract 1 to keep
    # WATERMARK_MASK_W/H meaning "pixels masked".
    ImageDraw.Draw(img).rectangle(
        [(width - WATERMARK_MASK_W, 0), (width - 1, WATERMARK_MASK_H - 1)],
        fill=255,
    )

    if autocontrast:
        # cutoff=1 ignores 1% of pixels at each end of the histogram before stretching,
        # which prevents a single stray outlier (e.g. anti-aliased corner pixel) from
        # blocking the stretch.
        img = ImageOps.autocontrast(img, cutoff=1)
    if contrast != 1.0:
        img = ImageEnhance.Contrast(img).enhance(contrast)
    for _ in range(dilate):
        # MinFilter(3) sets each pixel to the min of its 3x3 neighborhood — on a
        # grayscale image this is dilation of dark features by 1 pixel. Thickens
        # chart lines and text strokes.
        img = img.filter(ImageFilter.MinFilter(3))

    mono = img.convert("1", dither=Image.Dither.FLOYDSTEINBERG)
    raw = mono.tobytes()
    expected = (width // 8) * height
    if len(raw) != expected:
        raise ValueError(f"framebuffer size mismatch: got {len(raw)}, expected {expected}")
    return raw
