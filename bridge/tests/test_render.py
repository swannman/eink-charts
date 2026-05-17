from __future__ import annotations

from io import BytesIO

from PIL import Image, ImageDraw

from grafana_bridge.render import WATERMARK_MASK_H, WATERMARK_MASK_W, png_to_framebuffer

WIDTH = 792
HEIGHT = 528
EXPECTED_BYTES = (WIDTH // 8) * HEIGHT  # 52,272


def _png(size: tuple[int, int] = (WIDTH, HEIGHT), color: int = 128) -> bytes:
    img = Image.new("L", size, color)
    buf = BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def test_framebuffer_size_native() -> None:
    fb = png_to_framebuffer(_png(), WIDTH, HEIGHT)
    assert len(fb) == EXPECTED_BYTES


def test_framebuffer_size_when_resized() -> None:
    fb = png_to_framebuffer(_png(size=(1024, 600)), WIDTH, HEIGHT)
    assert len(fb) == EXPECTED_BYTES


def test_all_white_image() -> None:
    fb = png_to_framebuffer(_png(color=255), WIDTH, HEIGHT)
    # PIL mode "1": 1 = white. tobytes() packs 8 px/byte, MSB = leftmost.
    assert fb == b"\xff" * EXPECTED_BYTES


def test_all_black_image_except_watermark_mask() -> None:
    # Floyd-Steinberg diffuses error across the mask boundary, so we don't
    # assert byte-perfect rows. Instead: rows outside the mask region are
    # exactly all-black, and the masked corner contains white pixels.
    fb = png_to_framebuffer(_png(color=0), WIDTH, HEIGHT)
    bytes_per_row = WIDTH // 8
    mask_bytes = WATERMARK_MASK_W // 8

    # Rows fully below the mask region: untouched, all-black.
    for y in range(WATERMARK_MASK_H, HEIGHT):
        row = fb[y * bytes_per_row : (y + 1) * bytes_per_row]
        assert row == b"\x00" * bytes_per_row, f"row {y} not all black"

    # Mask region itself: the far-right `mask_bytes` of each row are pure white.
    for y in range(WATERMARK_MASK_H):
        row = fb[y * bytes_per_row : (y + 1) * bytes_per_row]
        assert row[-mask_bytes:] == b"\xff" * mask_bytes, f"row {y} mask not white"


def test_dither_produces_mixed_bits() -> None:
    # Mid-gray gets dithered, so output must not be all-black or all-white.
    # (A flat gray dithers to a checkerboard, which is just {0x55, 0xAA} — that's still dithering.)
    fb = png_to_framebuffer(_png(color=128), WIDTH, HEIGHT)
    unique = set(fb)
    assert unique != {0xFF}, "expected dithered output, got all-white"
    assert unique != {0x00}, "expected dithered output, got all-black"
    assert len(unique) >= 2


def test_smaller_source_centered_on_white_canvas() -> None:
    # A small black square padded onto a white canvas should leave the edges white.
    img = Image.new("L", (400, 300), 255)
    ImageDraw.Draw(img).rectangle([100, 100, 200, 200], fill=0)
    buf = BytesIO()
    img.save(buf, format="PNG")
    fb = png_to_framebuffer(buf.getvalue(), WIDTH, HEIGHT)
    assert len(fb) == EXPECTED_BYTES
    # First row should be all-white (the source was padded).
    bytes_per_row = WIDTH // 8
    assert fb[0:bytes_per_row] == b"\xff" * bytes_per_row
    assert fb[-bytes_per_row:] == b"\xff" * bytes_per_row
