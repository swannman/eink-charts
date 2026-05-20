"""Render panel data as device-ready JSON.

The /frame path delivers a pre-dithered framebuffer; this path delivers
just the values + axis metadata so the device renders the chart natively
(no dithering, sharper output, smaller payload, perfect differential
refresh between panel transitions).
"""
from __future__ import annotations

import datetime
import hashlib
import json
import math
import time
from typing import Any

import httpx

from .config import PanelConfig


def parse_grafana_time(s: str, now: float) -> float:
    """Convert 'now', 'now-6h', 'now-30m', a unix timestamp string, or RFC3339
    to a unix timestamp. Falls back to `now` on unrecognized input."""
    s = s.strip()
    if s == "now":
        return now
    if s.startswith("now-"):
        rest = s[4:]
        if not rest:
            return now
        unit = rest[-1].lower()
        try:
            num = float(rest[:-1])
        except ValueError:
            return now
        mult = {"s": 1.0, "m": 60.0, "h": 3600.0, "d": 86400.0, "w": 604800.0}.get(unit)
        if mult is None:
            return now
        return now - num * mult
    try:
        return float(s)
    except ValueError:
        pass
    try:
        return datetime.datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return now


def _nice_number(x: float, round_: bool) -> float:
    if x <= 0:
        return 1.0
    exp = math.floor(math.log10(x))
    f = x / (10 ** exp)
    if round_:
        nf = 1 if f < 1.5 else 2 if f < 3 else 5 if f < 7 else 10
    else:
        nf = 1 if f <= 1 else 2 if f <= 2 else 5 if f <= 5 else 10
    return nf * (10 ** exp)


def nice_axis(lo: float, hi: float, ticks: int = 5) -> tuple[float, float, list[float]]:
    """Return (axis_min, axis_max, tick_values) using nice round numbers."""
    if not (math.isfinite(lo) and math.isfinite(hi)):
        lo, hi = 0.0, 1.0
    if hi <= lo:
        hi = lo + 1.0
    rng = _nice_number(hi - lo, round_=False)
    step = _nice_number(rng / max(1, ticks - 1), round_=True)
    axis_min = math.floor(lo / step) * step
    axis_max = math.ceil(hi / step) * step
    out: list[float] = []
    v = axis_min
    while v <= axis_max + 0.5 * step:
        out.append(round(v, 6))
        v += step
    return axis_min, axis_max, out


def _format_tick(v: float, decimals: int) -> str:
    if decimals <= 0:
        # Drop the trailing .0 when value is whole.
        if abs(v - round(v)) < 1e-6:
            return f"{int(round(v))}"
        return f"{v:.1f}"
    return f"{v:.{decimals}f}"


def _format_legend(template: str, metric: dict[str, str]) -> str:
    def fallback() -> str:
        for k, v in metric.items():
            if not k.startswith("__"):
                return str(v)
        return "series"
    if not template:
        return fallback()
    out = template
    for k, v in metric.items():
        out = out.replace("{{" + k + "}}", str(v)).replace("{{ " + k + " }}", str(v))
    # If the template referenced a label this series doesn't have, the leftover
    # "{{...}}" is useless on a tiny EPD. Drop the template and use a label.
    if "{{" in out:
        return fallback()
    return out


def _format_time_axis(start: float, end: float, tz_name: str, n: int = 5) -> list[str]:
    try:
        from zoneinfo import ZoneInfo

        tz = ZoneInfo(tz_name)
    except Exception:
        tz = None
    duration = end - start
    labels: list[str] = []
    for i in range(n):
        ts = start + duration * i / (n - 1)
        dt = datetime.datetime.fromtimestamp(ts, tz=tz) if tz else datetime.datetime.fromtimestamp(ts)
        if duration <= 6 * 3600:
            labels.append(dt.strftime("%H:%M"))
        elif duration <= 3 * 86400:
            labels.append(dt.strftime("%H:%M"))
        elif duration <= 14 * 86400:
            labels.append(dt.strftime("%a"))
        else:
            labels.append(dt.strftime("%m/%d"))
    # Last tick is always rendered as HH:MM of the bundle render time — gives
    # the user the actual "as-of" time instead of a vague "now".
    end_dt = datetime.datetime.fromtimestamp(end, tz=tz) if tz else datetime.datetime.fromtimestamp(end)
    labels[-1] = end_dt.strftime("%H:%M")
    return labels


async def fetch_panel_data(
    client: httpx.AsyncClient,
    grafana_url: str,
    token: str,
    panel: PanelConfig,
) -> dict[str, Any]:
    """Execute all of a panel's queries and shape the result into rendering-ready
    JSON. Series points are normalized to [0,1] on both axes so the device can
    plot directly without rescaling."""
    now = time.time()
    start = parse_grafana_time(panel.from_, now)
    end = parse_grafana_time(panel.to, now)
    if end <= start:
        end = start + 1.0
    duration = end - start
    step = max(15, int(duration / 100))

    raw_series: list[tuple[str, list[tuple[float, float]]]] = []
    global_min = math.inf
    global_max = -math.inf

    for tgt in panel.targets:
        if not tgt.expr or not tgt.datasource_uid:
            continue
        url = f"{grafana_url}/api/datasources/proxy/uid/{tgt.datasource_uid}/api/v1/query_range"
        params = {"query": tgt.expr, "start": start, "end": end, "step": step}
        r = await client.get(
            url,
            params=params,
            headers={"Authorization": f"Bearer {token}"},
            timeout=30.0,
        )
        r.raise_for_status()
        body = r.json()
        for series in body.get("data", {}).get("result", []) or []:
            metric = series.get("metric", {}) or {}
            name = _format_legend(tgt.legend_format, metric)
            points: list[tuple[float, float]] = []
            for sample in series.get("values", []) or []:
                try:
                    ts = float(sample[0])
                    v = float(sample[1])
                except (TypeError, ValueError, IndexError):
                    continue
                if not math.isfinite(v):
                    continue
                points.append((ts, v))
                if v < global_min:
                    global_min = v
                if v > global_max:
                    global_max = v
            if points:
                raw_series.append((name, points))

    if not math.isfinite(global_min):
        global_min, global_max = 0.0, 1.0

    # Respect the panel's configured y-axis bounds (Grafana fieldConfig):
    #   soft_min/soft_max: preferred axis range; expands when data exceeds.
    #   hard_min/hard_max: absolute clip on the axis (and on the data).
    y_lo, y_hi = global_min, global_max
    if panel.soft_min is not None:
        y_lo = min(y_lo, panel.soft_min)
    if panel.soft_max is not None:
        y_hi = max(y_hi, panel.soft_max)
    if panel.hard_min is not None:
        y_lo = max(y_lo, panel.hard_min)
    if panel.hard_max is not None:
        y_hi = min(y_hi, panel.hard_max)
    axis_min, axis_max, ticks = nice_axis(y_lo, y_hi, ticks=5)
    y_labels = [_format_tick(v, panel.decimals) for v in ticks]
    x_labels = _format_time_axis(start, end, panel.tz, n=5)

    series_out: list[dict[str, Any]] = []
    rng_y = axis_max - axis_min if axis_max > axis_min else 1.0
    for name, points in raw_series:
        normalized: list[list[float]] = []
        for ts, v in points:
            nx = (ts - start) / duration
            ny = (v - axis_min) / rng_y
            if 0.0 <= nx <= 1.0:
                normalized.append([round(nx, 5), round(ny, 5)])
        series_out.append({"name": name, "points": normalized})

    return {
        "title": panel.name,
        "y_axis": {"min": axis_min, "max": axis_max, "labels": y_labels},
        "x_axis": {"labels": x_labels},
        "series": series_out,
    }


def encode_panel_payload(
    panel_index: int, panel_total: int, data: dict[str, Any], next_poll: int
) -> tuple[bytes, str]:
    """Wrap fetched data into the device-facing envelope and compute the etag.

    Etag is computed over the payload sans-etag, so a stable payload yields a
    stable etag round-trip."""
    base = dict(data)
    base["panel_index"] = panel_index
    base["panel_total"] = panel_total
    base["next_poll"] = next_poll
    canonical = json.dumps(base, separators=(",", ":"), sort_keys=True).encode("utf-8")
    etag = hashlib.sha256(canonical).hexdigest()[:16]
    base["etag"] = etag
    body = json.dumps(base, separators=(",", ":")).encode("utf-8")
    return body, etag


def encode_bundle_payload(
    default_panels: list[dict[str, Any]],
    zoom_panels: list[dict[str, Any]],
    next_poll: int,
) -> tuple[bytes, str]:
    """All-panels bundle: default-range + 2h-zoom for every panel, in one
    response, so the device can pre-cache everything in a single WiFi cycle
    and serve button presses purely from local NVS."""
    payload: dict[str, Any] = {
        "default": default_panels,
        "zoom_2h": zoom_panels,
        "panel_total": len(default_panels),
        "next_poll": next_poll,
    }
    canonical = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    etag = hashlib.sha256(canonical).hexdigest()[:16]
    payload["etag"] = etag
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    return body, etag


# -----------------------------------------------------------------------------
# Binary bundle encoder. Compact format used by /data/all so the device can
# cache the whole bundle as a single NVS blob and parse it directly with no
# JSON overhead. All little-endian; matches ESP32-C3 native.
#
# Header (8 bytes):
#   u16 magic = 0xCFB1
#   u8  version = 1
#   u8  panel_count
#   u32 next_poll_seconds
# Offset table (8 * panel_count bytes):
#   u32 default_offset, u32 zoom_offset (both from start of bundle)
# Panel block (one per entry):
#   pstr title              (u8 length + bytes)
#   pstr series_name
#   u8 y_label_count, then count × pstr
#   u8 x_label_count, then count × pstr
#   u16 point_count, then count × {u16 nx, u16 ny}   (each 0..65535)
# -----------------------------------------------------------------------------

import struct

BUNDLE_MAGIC = 0xCFB1
# v4 = "screens" (composite). Each screen contains 1..3 entries of mixed type
#      (chart full-screen, or 1-3 stat panels side by side). v3 had one panel
#      per screen.
# v3 = adds a per-panel default-view byte (after the offset table).
# v2 = three time windows per panel (24h, 2h, 7d). v1 had two (default + 2h).
BUNDLE_VERSION = 4

# Screen type codes — must match firmware ScreenType enum.
SCREEN_CHART = 0
SCREEN_STAT_GROUP = 1

# Per-entry type codes inside a screen — must match firmware EntryType enum.
ENTRY_CHART = 0
ENTRY_STAT = 1


# Translation of common Grafana unit codes to short display strings that fit
# on a small EPD. Unknown codes pass through as-is.
UNIT_DISPLAY = {
    "": "",
    "none": "",
    "short": "",
    "fahrenheit": "F",
    "celsius": "C",
    "percent": "%",
    "percentunit": "%",
    "humidity": "%",
    "MBs": "MB/s",
    "KBs": "KB/s",
    "Bps": "B/s",
    "binBps": "B/s",
    "Mbits": "Mb/s",
    "watt": "W",
    "kwatt": "kW",
    "watth": "Wh",
    "kwatth": "kWh",
    "volt": "V",
    "millivolt": "mV",
    "amp": "A",
    "milliamp": "mA",
    "pressurehpa": "hPa",
    "pressurembar": "mbar",
    "pressurepsi": "PSI",
    "lengthm": "m",
    "lengthcm": "cm",
    "lengthmm": "mm",
    "lengthft": "ft",
    "lengthkm": "km",
    "velocitymps": "m/s",
    "velocitymph": "mph",
    "gallon": "gal",
}


def unit_display_str(unit: str) -> str:
    """Translate a Grafana unit code to a 1-4 char EPD-friendly suffix."""
    if not unit:
        return ""
    s = UNIT_DISPLAY.get(unit)
    if s is not None:
        return s
    # Pass through unknown codes verbatim (truncated if huge).
    return unit[:8]


def build_stat_panel(panel_data: dict[str, Any], unit: str, decimals: int) -> dict[str, Any]:
    """Reduce a time-series panel_data to a stat representation: title + unit +
    formatted last-value string + sparkline points. Matches Grafana stat
    panel's default reducer ('last value, non-null') and 'area' graph mode."""
    title = panel_data.get("title", "")
    series = panel_data.get("series") or []
    value_str = "—"
    spark: list[list[float]] = []
    if series and series[0].get("points"):
        pts = series[0]["points"]
        last_ny = float(pts[-1][1])
        y_axis = panel_data.get("y_axis") or {}
        y_min = float(y_axis.get("min", 0.0))
        y_max = float(y_axis.get("max", 1.0))
        rng = y_max - y_min if y_max > y_min else 1.0
        raw = y_min + last_ny * rng
        value_str = _format_tick(raw, max(0, min(decimals, 4)))
        # Sparkline reuses the same normalized [0,1] points. Subsample to ~60
        # for stat columns — denser than that adds bytes without visible gain
        # at the narrow column widths.
        if len(pts) > 60:
            stride = len(pts) / 60.0
            spark = [pts[int(i * stride)] for i in range(60)]
            if spark[-1] != pts[-1]:
                spark.append(pts[-1])
        else:
            spark = pts
    return {
        "type": "stat",
        "title": title,
        "unit": unit_display_str(unit),
        "value_str": value_str,
        "sparkline": spark,
    }


def _encode_chart_entry(buf: bytearray, panel: dict[str, Any]) -> None:
    buf.append(ENTRY_CHART)
    _encode_panel(buf, panel)


def _encode_stat_entry(buf: bytearray, stat: dict[str, Any]) -> None:
    buf.append(ENTRY_STAT)
    _encode_pstr(buf, stat.get("title", ""))
    _encode_pstr(buf, stat.get("unit", ""))
    _encode_pstr(buf, stat.get("value_str", ""))
    spark = stat.get("sparkline") or []
    pcount = min(len(spark), 255)
    buf.append(pcount)
    for nx, ny in spark[:pcount]:
        ux = max(0, min(65535, int(round(float(nx) * 65535))))
        uy = max(0, min(65535, int(round(float(ny) * 65535))))
        buf += struct.pack("<HH", ux, uy)


def _encode_screen(buf: bytearray, screen: dict[str, Any]) -> None:
    """A screen is {type: int, entries: [chart-dict or stat-dict, ...]}."""
    stype = screen.get("type", SCREEN_CHART)
    entries = screen.get("entries") or []
    buf.append(stype & 0xFF)
    buf.append(min(len(entries), 3))
    for e in entries[:3]:
        if e.get("type") == "stat":
            _encode_stat_entry(buf, e)
        else:
            _encode_chart_entry(buf, e)


def encode_bundle_screens(
    screens_24h: list[dict[str, Any]],
    screens_2h: list[dict[str, Any]],
    screens_7d: list[dict[str, Any]],
    default_views: list[int],
    next_poll: int,
) -> tuple[bytes, str]:
    """v4 binary bundle: list of screens (each holding 1-3 entries) for each of
    three time windows.

    Layout:
      Header (8):  u16 magic, u8 version=4, u8 screen_count, u32 next_poll
      Offset table: 3 × u32 per screen (24h_ofs, 2h_ofs, 7d_ofs)
      Default-view: 1 byte per screen
      Screen blocks: written in 24h-then-2h-then-7d order so offsets resolve.
    """
    n = len(screens_24h)
    assert n == len(screens_2h) == len(screens_7d) == len(default_views), \
        "screen counts must match across views"
    buf = bytearray()
    buf += struct.pack("<HBBI", BUNDLE_MAGIC, BUNDLE_VERSION, n, next_poll)
    table_pos = len(buf)
    buf += b"\x00" * (12 * n)
    buf += bytes(default_views[:n])
    offsets_24h: list[int] = []
    offsets_2h: list[int] = []
    offsets_7d: list[int] = []
    for s in screens_24h:
        offsets_24h.append(len(buf))
        _encode_screen(buf, s)
    for s in screens_2h:
        offsets_2h.append(len(buf))
        _encode_screen(buf, s)
    for s in screens_7d:
        offsets_7d.append(len(buf))
        _encode_screen(buf, s)
    table = bytearray()
    for i in range(n):
        table += struct.pack("<III", offsets_24h[i], offsets_2h[i], offsets_7d[i])
    buf[table_pos:table_pos + 12 * n] = table
    etag = hashlib.sha256(bytes(buf)).hexdigest()[:16]
    return bytes(buf), etag




# View mode codes used in the default-view byte. Must mirror firmware
# ViewMode enum.
VIEW_24H = 0
VIEW_2H = 1
VIEW_7D = 2


def _encode_pstr(buf: bytearray, s: str) -> None:
    b = (s or "").encode("utf-8")
    if len(b) > 255:
        b = b[:255]
    buf.append(len(b))
    buf += b


def _encode_panel(buf: bytearray, panel: dict[str, Any]) -> None:
    _encode_pstr(buf, panel.get("title", ""))
    series = panel.get("series") or []
    s0 = series[0] if series else {}
    _encode_pstr(buf, s0.get("name", ""))
    y_labels = (panel.get("y_axis") or {}).get("labels") or []
    buf.append(min(len(y_labels), 255))
    for lab in y_labels[:255]:
        _encode_pstr(buf, str(lab))
    x_labels = (panel.get("x_axis") or {}).get("labels") or []
    buf.append(min(len(x_labels), 255))
    for lab in x_labels[:255]:
        _encode_pstr(buf, str(lab))
    points = s0.get("points") or []
    pcount = min(len(points), 65535)
    buf += struct.pack("<H", pcount)
    for nx, ny in points[:pcount]:
        ux = max(0, min(65535, int(round(float(nx) * 65535))))
        uy = max(0, min(65535, int(round(float(ny) * 65535))))
        buf += struct.pack("<HH", ux, uy)


def build_battery_panel(
    history: list[tuple[float, int]],
    from_epoch: float,
    to_epoch: float,
    tz_name: str = "America/Los_Angeles",
) -> dict[str, Any]:
    """Synthesize a battery-voltage panel from the bridge's in-memory history.

    Same shape as a Grafana panel so it slots into the bundle next to the real
    ones with no special-casing on the device. Empty history → empty series;
    the device will just draw axes without a line."""
    if to_epoch <= from_epoch:
        to_epoch = from_epoch + 1.0
    duration = to_epoch - from_epoch
    points_in_window = [(t, mv) for t, mv in history if from_epoch <= t <= to_epoch]

    # Convert mV → V on the bridge so the axis labels (e.g. "3.3", "4.2") stay
    # narrow on the small EPD.
    points_v = [(t, mv / 1000.0) for t, mv in points_in_window]
    if points_v:
        vs = [v for _, v in points_v]
        y_lo = min(min(vs), 3.3)
        y_hi = max(max(vs), 4.2)
    else:
        y_lo, y_hi = 3.3, 4.2
    axis_min, axis_max, ticks = nice_axis(y_lo, y_hi, ticks=5)
    rng_y = axis_max - axis_min if axis_max > axis_min else 1.0

    series_points: list[list[float]] = []
    for t, v in points_v:
        nx = (t - from_epoch) / duration
        ny = (v - axis_min) / rng_y
        if 0.0 <= nx <= 1.0:
            series_points.append([round(nx, 5), round(ny, 5)])

    return {
        "title": "Battery (V)",
        "y_axis": {
            "min": axis_min,
            "max": axis_max,
            "labels": [_format_tick(v, 1) for v in ticks],
        },
        "x_axis": {"labels": _format_time_axis(from_epoch, to_epoch, tz_name, n=5)},
        "series": [{"name": "battery", "points": series_points}] if series_points else [],
    }


def validate_panel_data(data: dict[str, Any]) -> tuple[bool, str]:
    """Decide whether a fetched panel will render acceptably on the device.

    Returns (ok, reason_if_dropped). The bridge drops panels failing this check
    from the bundle so the device never has to render a blank/broken chart
    (e.g., wrong panel id after a Grafana edit, query typo, dead datasource)."""
    title = (data.get("title") or "").strip()
    if not title:
        return False, "empty title"
    series = data.get("series") or []
    if not series:
        return False, "no series returned"
    points = series[0].get("points") or []
    if len(points) < 5:
        return False, f"only {len(points)} datapoints"
    y_axis = data.get("y_axis") or {}
    y_min = y_axis.get("min")
    y_max = y_axis.get("max")
    if y_min is None or y_max is None or y_max <= y_min:
        return False, f"collapsed y range ({y_min}..{y_max})"
    x_labels = (data.get("x_axis") or {}).get("labels") or []
    if len(x_labels) < 2:
        return False, "no x labels"
    return True, ""


def map_time_from_to_view(time_from: str | None) -> int:
    """Translate a Grafana panel `timeFrom` override (e.g. '7d', 'now-2h') to
    one of our three view-mode codes. Anything we don't recognize maps to 24h."""
    if not time_from:
        return VIEW_24H
    s = time_from.strip().lower()
    if "7d" in s or "1w" in s or "168h" in s:
        return VIEW_7D
    if s.endswith("2h") or "now-2h" in s:
        return VIEW_2H
    return VIEW_24H


def encode_bundle_binary(
    view_24h: list[dict[str, Any]],
    view_2h: list[dict[str, Any]],
    view_7d: list[dict[str, Any]],
    default_views: list[int],
    next_poll: int,
) -> tuple[bytes, str]:
    """Three time windows (24h, 2h, 7d) per panel plus a per-panel default
    view code. v3 layout: header → offset table (3 u32/panel) → default-view
    table (1 byte/panel) → panel blocks."""
    n = len(view_24h)
    assert n == len(view_2h) == len(view_7d) == len(default_views), "view counts must match"
    buf = bytearray()
    buf += struct.pack("<HBBI", BUNDLE_MAGIC, BUNDLE_VERSION, n, next_poll)
    table_pos = len(buf)
    buf += b"\x00" * (12 * n)  # three u32 offsets per panel
    defaults_pos = len(buf)
    buf += bytes(default_views[:n])
    offsets_24h: list[int] = []
    offsets_2h: list[int] = []
    offsets_7d: list[int] = []
    for p in view_24h:
        offsets_24h.append(len(buf))
        _encode_panel(buf, p)
    for p in view_2h:
        offsets_2h.append(len(buf))
        _encode_panel(buf, p)
    for p in view_7d:
        offsets_7d.append(len(buf))
        _encode_panel(buf, p)
    table = bytearray()
    for i in range(n):
        table += struct.pack("<III", offsets_24h[i], offsets_2h[i], offsets_7d[i])
    buf[table_pos:table_pos + 12 * n] = table
    etag = hashlib.sha256(bytes(buf)).hexdigest()[:16]
    return bytes(buf), etag
