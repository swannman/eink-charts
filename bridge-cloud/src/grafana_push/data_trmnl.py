"""TRMNL X "dashboard bundle" encoder (magic 0xCFB2).

Where the X3 bundle (``data.py``, magic 0xCFB1) is a flat list of *screens*
(one panel each), the TRMNL X shows a whole Grafana dashboard per screen: the
device lays every panel out at its ``gridPos`` and renders it natively, scaling
the grid to fill the 1872x1404 panel. This module turns fetched panel data plus
Grafana threshold metadata into that binary, and — because the firmware renders
in genuine 16-level grayscale — pre-maps Grafana's threshold *colors* to gray
levels so the device only ever deals with 0..15 shades.

Gray convention matches FastEPD: 0 = black, 15 = white.

Binary layout (all little-endian, ``pstr`` = u8 length + UTF-8 bytes):

    Header:  u16 magic=0xCFB2, u8 version=1, u8 dashboard_count, u32 next_poll
    Offsets: u32 * dashboard_count   (byte offset of each dashboard block)
    Dashboard block:
      pstr title
      u8 grid_cols            (Grafana grid width, always 24)
      u8 grid_rows            (max gy+gh across the dashboard's panels)
      u8 panel_count
      per panel:
        u8 type               (0=timeseries, 1=stat)
        u8 gx, u8 gy, u8 gw, u8 gh
        pstr title
        pstr unit             (already shortened, e.g. "%", "F")
        u8 base_gray          (status shade from the active threshold; 15 if none)
        -- type 0 (timeseries) --
        u8 y_label_count, then count x pstr
        u8 x_label_count, then count x pstr
        u8 band_count, then count x { u16 y0n, u16 y1n, u8 gray }
        u8 series_count, then per series: u16 point_count, count x { u16 nx, u16 ny }
        -- type 1 (stat) --
        pstr value_str
        u8 spark_count, then count x { u16 nx, u16 ny }
"""
from __future__ import annotations

import hashlib
import math
import struct
from typing import Any

BUNDLE_MAGIC = 0xCFB2
BUNDLE_VERSION = 1

# Slideshow manifest: a tiny sidecar the device fetches first so it knows how
# many per-dashboard bundles exist and which changed since last time (so it only
# re-downloads what moved). Distinct magic from the bundle itself.
MANIFEST_MAGIC = 0xCFB3
MANIFEST_VERSION = 1

PANEL_TIMESERIES = 0
PANEL_STAT = 1

# White background — a panel/value with no matching threshold draws no tint.
GRAY_WHITE = 15
GRAY_BLACK = 0

# Grafana threshold colour -> gray level (0=black .. 15=white). These are the
# "status shade" for a value or band; the renderer lightens them for tile
# backgrounds so black text stays legible. Chosen so the common cold/warn/ok
# ramp (red < yellow < green) reads as darker -> lighter, i.e. worse is darker
# and grabs the eye on an e-ink panel.
COLOR_GRAY: dict[str, int] = {
    "red": 5,
    "dark-red": 4,
    "orange": 7,
    "dark-orange": 6,
    "yellow": 9,
    "gold": 9,
    "#eab839": 9,   # Grafana's classic gold hex, used on dashboard 2
    "green": 12,
    "dark-green": 11,
    "blue": 11,
    "dark-blue": 10,
    "light-blue": 13,
    "purple": 8,
    "text": GRAY_BLACK,
    "transparent": GRAY_WHITE,
    "": GRAY_WHITE,
    "none": GRAY_WHITE,
}


def _parse_rgb(color: str) -> tuple[int, int, int] | None:
    """Parse a ``#rgb``/``#rrggbb`` or ``rgb()``/``rgba()`` colour to (r,g,b)."""
    c = (color or "").strip().lower()
    if c.startswith("#"):
        h = c[1:]
        if len(h) == 3:
            h = "".join(ch * 2 for ch in h)
        if len(h) != 6:
            return None
        try:
            return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
        except ValueError:
            return None
    if c.startswith("rgb") and "(" in c and ")" in c:
        parts = c[c.find("(") + 1:c.find(")")].split(",")
        if len(parts) >= 3:
            try:
                return (int(float(parts[0])), int(float(parts[1])), int(float(parts[2])))
            except ValueError:
                return None
    return None


def gray_for_color(color: str | None) -> int:
    """Map a Grafana colour token (name, hex, or rgb/rgba) to a 0..15 gray."""
    if not color:
        return GRAY_WHITE
    c = color.strip().lower()
    if c in COLOR_GRAY:
        return COLOR_GRAY[c]
    rgb = _parse_rgb(c)
    if rgb is not None:
        lum = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]  # 0..255
        return max(0, min(15, round(lum / 255.0 * 15)))
    # e.g. "semi-dark-red" -> strip prefixes down to a known base colour.
    for base in ("red", "orange", "yellow", "green", "blue", "purple"):
        if base in c:
            return COLOR_GRAY[base]
    return GRAY_WHITE


# Very light band shades for threshold zones (0=black .. 15=white, so higher =
# lighter). In-range (green) draws NO fill so the "good" region reads as blank;
# out-of-range zones get a faint tint — red slightly stronger than yellow — so
# it's clear when the line leaves the desired range without the band competing
# with the plotted line. Kept near-white on purpose (~1-2 levels of darkness).
BAND_GRAY_ALERT = 13   # red / danger  (was 11)
BAND_GRAY_WARN = 14    # yellow / orange (was 13)


def band_gray_for_color(color: str | None) -> int:
    """Gray for a threshold *band*: GRAY_WHITE (no fill) for in-range/green,
    a light shade for out-of-range warm colours."""
    rgb = _parse_rgb(color)
    if rgb is not None:
        r, g, b = rgb
        if g > r and g > b:
            return GRAY_WHITE                 # green / in-range -> clean
        if r >= 150 and g >= 120:
            return BAND_GRAY_WARN             # yellow / orange
        return BAND_GRAY_ALERT                # red / danger
    c = (color or "").strip().lower()
    if "green" in c or c in ("", "none", "transparent", "text"):
        return GRAY_WHITE
    if "yellow" in c or "gold" in c or "orange" in c:
        return BAND_GRAY_WARN
    if "red" in c:
        return BAND_GRAY_ALERT
    g = gray_for_color(color)
    return g if g < GRAY_WHITE else GRAY_WHITE


def _sorted_steps(
    steps: list[tuple[float | None, str]],
) -> list[tuple[float | None, str]]:
    """Grafana evaluates thresholds in ascending value order (null = -inf);
    sort defensively in case the panel JSON stores them out of order."""
    return sorted(steps, key=lambda s: (-math.inf if s[0] is None else s[0]))


def active_threshold_gray(steps: list[tuple[float | None, str]], value: float | None) -> int:
    """Gray of the threshold step a stat value falls in (Grafana's absolute
    thresholds: the last step whose ``value`` is <= the reading; the first
    step's value is null = -inf)."""
    if not steps or value is None:
        return GRAY_WHITE
    steps = _sorted_steps(steps)
    chosen = steps[0][1]
    for thr, color in steps:
        if thr is None:
            chosen = color
            continue
        if value >= thr:
            chosen = color
        else:
            break
    return gray_for_color(chosen)


def bands_from_steps(
    steps: list[tuple[float | None, str]],
    axis_min: float,
    axis_max: float,
) -> list[tuple[float, float, int]]:
    """Convert absolute threshold steps into normalized [0,1] fill bands for a
    timeseries panel (used by dashboards whose thresholdsStyle shows an area).
    Each band spans from one step's value up to the next, shaded by that step's
    colour via :func:`band_gray_for_color` (in-range/green draws nothing).
    Returns (y0_norm, y1_norm, gray) with y measured bottom-up like the device's
    chart y-axis."""
    if not steps or axis_max <= axis_min:
        return []
    rng = axis_max - axis_min
    # Lower edges. Grafana's LOWEST step is the base (covers from -inf), even
    # when its stored value isn't null — so anchor the first sorted step at
    # axis_min regardless of its value. (Anchoring it at its own value instead
    # drops the below-value band whenever that value sits above axis_min, e.g. a
    # freezer with a red base at 0 and an axis reaching below 0.) Later steps
    # anchor at their threshold value, clamped to the axis.
    edges: list[tuple[float, str]] = []
    for i, (thr, color) in enumerate(_sorted_steps(steps)):
        if i == 0 or thr is None:
            v = axis_min
        else:
            v = max(axis_min, min(axis_max, thr))
        edges.append((v, color))
    bands: list[tuple[float, float, int]] = []
    for i, (lo, color) in enumerate(edges):
        hi = edges[i + 1][0] if i + 1 < len(edges) else axis_max
        if hi <= lo:
            continue
        gray = band_gray_for_color(color)
        if gray >= GRAY_WHITE:
            continue  # in-range / white bands draw no fill
        y0n = (lo - axis_min) / rng
        y1n = (hi - axis_min) / rng
        bands.append((round(y0n, 5), round(y1n, 5), gray))
    return bands


# -----------------------------------------------------------------------------
# Encoder
# -----------------------------------------------------------------------------

def _u16n(x: float) -> int:
    return max(0, min(65535, int(round(float(x) * 65535))))


def _encode_pstr(buf: bytearray, s: str) -> None:
    b = (s or "").encode("utf-8")
    if len(b) > 255:
        b = b[:255]
    buf.append(len(b))
    buf += b


def _encode_points(buf: bytearray, points: list, cap: int = 65535) -> None:
    n = min(len(points), cap)
    if cap <= 255:
        buf.append(n)
    else:
        buf += struct.pack("<H", n)
    for nx, ny in points[:n]:
        buf += struct.pack("<HH", _u16n(nx), _u16n(ny))


def _encode_panel(buf: bytearray, p: dict[str, Any]) -> None:
    ptype = int(p.get("type", PANEL_TIMESERIES))
    buf.append(ptype & 0xFF)
    buf += bytes(
        (
            int(p.get("gx", 0)) & 0xFF,
            int(p.get("gy", 0)) & 0xFF,
            int(p.get("gw", 24)) & 0xFF,
            int(p.get("gh", 1)) & 0xFF,
        )
    )
    _encode_pstr(buf, p.get("title", ""))
    _encode_pstr(buf, p.get("unit", ""))
    buf.append(int(p.get("base_gray", GRAY_WHITE)) & 0xFF)

    if ptype == PANEL_STAT:
        _encode_pstr(buf, p.get("value_str", ""))
        _encode_points(buf, p.get("sparkline") or [], cap=255)
        return

    # timeseries
    y_labels = [str(x) for x in (p.get("y_labels") or [])][:255]
    buf.append(len(y_labels))
    for lab in y_labels:
        _encode_pstr(buf, lab)
    x_labels = [str(x) for x in (p.get("x_labels") or [])][:255]
    buf.append(len(x_labels))
    for lab in x_labels:
        _encode_pstr(buf, lab)
    bands = (p.get("bands") or [])[:255]
    buf.append(len(bands))
    for y0n, y1n, gray in bands:
        buf += struct.pack("<HHB", _u16n(y0n), _u16n(y1n), int(gray) & 0xFF)
    series = (p.get("series") or [])[:255]
    buf.append(len(series))
    for pts in series:
        _encode_points(buf, pts or [], cap=65535)


def _decimate(points: list, keep: int) -> list:
    """Evenly subsample ``points`` to at most ``keep`` samples, always retaining
    the first and last so the line's endpoints stay put. Reducing point counts
    is how we shrink the bundle to fit the device (each point is 4 bytes)."""
    n = len(points)
    if keep >= n or n <= 2:
        return points
    if keep < 2:
        return [points[0], points[-1]] if n >= 2 else list(points)
    stride = (n - 1) / (keep - 1)
    out = [points[min(n - 1, int(round(i * stride)))] for i in range(keep)]
    out[-1] = points[-1]
    return out


def _cap_series_points(dashboards: list[dict[str, Any]], keep: int) -> list[dict[str, Any]]:
    """Return a shallow copy of ``dashboards`` with every timeseries series (and
    stat sparkline) decimated to at most ``keep`` points. Only the point lists
    are rebuilt; all other panel metadata is shared."""
    out: list[dict[str, Any]] = []
    for d in dashboards:
        panels_out: list[dict[str, Any]] = []
        for p in d.get("panels") or []:
            q = dict(p)
            if int(p.get("type", PANEL_TIMESERIES)) == PANEL_STAT:
                spark = p.get("sparkline") or []
                if len(spark) > keep:
                    q["sparkline"] = _decimate(spark, keep)
            else:
                q["series"] = [_decimate(s or [], keep) for s in (p.get("series") or [])]
            panels_out.append(q)
        dd = dict(d)
        dd["panels"] = panels_out
        out.append(dd)
    return out


def _max_series_points(dashboards: list[dict[str, Any]]) -> int:
    m = 0
    for d in dashboards:
        for p in d.get("panels") or []:
            if int(p.get("type", PANEL_TIMESERIES)) == PANEL_STAT:
                m = max(m, len(p.get("sparkline") or []))
            else:
                for s in p.get("series") or []:
                    m = max(m, len(s or []))
    return m


def encode_dashboard_bundle_fit(
    dashboards: list[dict[str, Any]], next_poll: int, budget: int
) -> tuple[bytes, str, dict[str, Any]]:
    """Encode the bundle, scaling chart resolution down so the result fits within
    ``budget`` plaintext bytes (the capacity the device advertised). This is the
    server half of the overflow defense: rather than emit a bundle the device
    can't cache, we drop points per series until it fits.

    Returns ``(body, etag, info)`` where ``info`` describes what happened
    (``downsampled``, ``kept_points``, ``size``, ``budget``)."""
    body, etag = encode_dashboard_bundle(dashboards, next_poll)
    if budget <= 0 or len(body) <= budget:
        return body, etag, {
            "downsampled": False, "size": len(body), "budget": budget,
            "kept_points": _max_series_points(dashboards),
        }

    # Bundle size is monotonic in the per-series point cap, so binary-search the
    # largest cap whose encoding still fits. Re-encoding is cheap (a handful of
    # dashboards), and we only fetched Grafana once.
    lo, hi = 2, _max_series_points(dashboards)
    best_body, best_etag, best_keep = None, None, None
    while lo <= hi:
        mid = (lo + hi) // 2
        cand_body, cand_etag = encode_dashboard_bundle(_cap_series_points(dashboards, mid), next_poll)
        if len(cand_body) <= budget:
            best_body, best_etag, best_keep = cand_body, cand_etag, mid
            lo = mid + 1
        else:
            hi = mid - 1

    if best_body is None:
        # Even 2 points/series overflows (many panels, tiny budget). Emit the
        # floor; the device's own MAX_SEALED check is the last backstop.
        best_body, best_etag = encode_dashboard_bundle(_cap_series_points(dashboards, 2), next_poll)
        best_keep = 2
    return best_body, best_etag, {
        "downsampled": True, "size": len(best_body), "budget": budget,
        "kept_points": best_keep,
    }


def encode_dashboard_bundle(
    dashboards: list[dict[str, Any]], next_poll: int
) -> tuple[bytes, str]:
    """Encode the 0xCFB2 dashboard bundle. ``dashboards`` is a list of
    ``{title, grid_cols, grid_rows, panels: [...]}`` dicts (see module docstring
    for the per-panel fields). Returns (body, etag)."""
    n = len(dashboards)
    buf = bytearray()
    buf += struct.pack("<HBBI", BUNDLE_MAGIC, BUNDLE_VERSION, n, next_poll)
    table_pos = len(buf)
    buf += b"\x00" * (4 * n)
    offsets: list[int] = []
    for d in dashboards:
        offsets.append(len(buf))
        _encode_pstr(buf, d.get("title", ""))
        buf.append(int(d.get("grid_cols", 24)) & 0xFF)
        buf.append(int(d.get("grid_rows", 1)) & 0xFF)
        panels = d.get("panels") or []
        buf.append(min(len(panels), 255))
        for p in panels[:255]:
            _encode_panel(buf, p)
    table = bytearray()
    for ofs in offsets:
        table += struct.pack("<I", ofs)
    buf[table_pos:table_pos + 4 * n] = table
    etag = hashlib.sha256(bytes(buf)).hexdigest()[:16]
    return bytes(buf), etag


def etag32(etag_hex: str) -> int:
    """Compress a hex bundle etag to a u32 for the compact binary manifest. The
    device compares these to decide which dashboards to re-download."""
    try:
        return int((etag_hex or "")[:8], 16) & 0xFFFFFFFF
    except (ValueError, TypeError):
        return 0


def encode_manifest(etags: list[str], next_poll: int) -> bytes:
    """Encode the slideshow manifest (magic 0xCFB3):

        u16 magic, u8 version, u8 count, u32 next_poll
        count x u32 etag32   (one per dashboard, index order)
    """
    n = len(etags)
    buf = bytearray()
    buf += struct.pack("<HBBI", MANIFEST_MAGIC, MANIFEST_VERSION, n & 0xFF, next_poll)
    for e in etags:
        buf += struct.pack("<I", etag32(e))
    return bytes(buf)


# -----------------------------------------------------------------------------
# Decoder — used by the host-side preview (tools/preview_trmnl.py) and tests to
# verify the firmware-facing format round-trips. Mirrors the C++ BinReader the
# firmware will use.
# -----------------------------------------------------------------------------

class _Reader:
    def __init__(self, data: bytes) -> None:
        self.d = data
        self.p = 0

    def u8(self) -> int:
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self) -> int:
        v = struct.unpack_from("<H", self.d, self.p)[0]
        self.p += 2
        return v

    def u32(self) -> int:
        v = struct.unpack_from("<I", self.d, self.p)[0]
        self.p += 4
        return v

    def pstr(self) -> str:
        n = self.u8()
        s = self.d[self.p:self.p + n].decode("utf-8", "replace")
        self.p += n
        return s


def _decode_panel(r: _Reader) -> dict[str, Any]:
    ptype = r.u8()
    gx, gy, gw, gh = r.u8(), r.u8(), r.u8(), r.u8()
    title = r.pstr()
    unit = r.pstr()
    base_gray = r.u8()
    panel: dict[str, Any] = {
        "type": ptype, "gx": gx, "gy": gy, "gw": gw, "gh": gh,
        "title": title, "unit": unit, "base_gray": base_gray,
    }
    if ptype == PANEL_STAT:
        panel["value_str"] = r.pstr()
        spark_n = r.u8()
        panel["sparkline"] = [
            (r.u16() / 65535.0, r.u16() / 65535.0) for _ in range(spark_n)
        ]
        return panel
    yn = r.u8()
    panel["y_labels"] = [r.pstr() for _ in range(yn)]
    xn = r.u8()
    panel["x_labels"] = [r.pstr() for _ in range(xn)]
    bn = r.u8()
    bands = []
    for _ in range(bn):
        y0n = r.u16() / 65535.0
        y1n = r.u16() / 65535.0
        gray = r.u8()
        bands.append((y0n, y1n, gray))
    panel["bands"] = bands
    series_count = r.u8()
    series = []
    for _ in range(series_count):
        pn = r.u16()
        series.append([(r.u16() / 65535.0, r.u16() / 65535.0) for _ in range(pn)])
    panel["series"] = series
    return panel


def decode_dashboard_bundle(body: bytes) -> dict[str, Any]:
    """Inverse of encode_dashboard_bundle. Returns
    ``{magic, version, next_poll, dashboards: [...]}``."""
    r = _Reader(body)
    magic = r.u16()
    version = r.u8()
    count = r.u8()
    next_poll = r.u32()
    if magic != BUNDLE_MAGIC:
        raise ValueError(f"bad magic 0x{magic:04x}")
    if version != BUNDLE_VERSION:
        raise ValueError(f"unsupported version {version}")
    offsets = [r.u32() for _ in range(count)]
    dashboards = []
    for ofs in offsets:
        dr = _Reader(body)
        dr.p = ofs
        title = dr.pstr()
        grid_cols = dr.u8()
        grid_rows = dr.u8()
        panel_count = dr.u8()
        panels = [_decode_panel(dr) for _ in range(panel_count)]
        dashboards.append({
            "title": title, "grid_cols": grid_cols,
            "grid_rows": grid_rows, "panels": panels,
        })
    return {
        "magic": magic, "version": version,
        "next_poll": next_poll, "dashboards": dashboards,
    }


def decode_manifest(body: bytes) -> dict[str, Any]:
    """Inverse of encode_manifest. Returns
    ``{magic, version, count, next_poll, etags: [u32, ...]}``."""
    r = _Reader(body)
    magic = r.u16()
    version = r.u8()
    count = r.u8()
    next_poll = r.u32()
    if magic != MANIFEST_MAGIC:
        raise ValueError(f"bad manifest magic 0x{magic:04x}")
    if version != MANIFEST_VERSION:
        raise ValueError(f"unsupported manifest version {version}")
    etags = [r.u32() for _ in range(count)]
    return {
        "magic": magic, "version": version, "count": count,
        "next_poll": next_poll, "etags": etags,
    }
