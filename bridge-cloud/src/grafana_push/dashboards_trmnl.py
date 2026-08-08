"""Resolve a Grafana folder into TRMNL X dashboard-bundle input.

Enumerates every dashboard in a folder, pulls each one's panels (reusing the
X3 scheduler's ``_fetch_dashboard_panels`` so gridPos / thresholds / units come
from one place), executes each panel's queries via ``data.fetch_panel_data``,
and shapes the result into the ``dashboards`` list that
``data_trmnl.encode_dashboard_bundle`` consumes.

The device renders these locally, so all Grafana-specific work — normalizing
series to [0,1], picking nice axis labels, mapping threshold colours to gray —
happens here, not on the ESP32.
"""
from __future__ import annotations

import dataclasses
import logging
from typing import Any

import httpx

from .config import AppConfig, DashboardConfig
from .data import build_stat_panel, fetch_panel_data, unit_display_str
from .data_trmnl import (
    GRAY_WHITE,
    PANEL_STAT,
    PANEL_TIMESERIES,
    SPECTRA_WHITE,
    active_threshold_gray,
    active_threshold_spectra,
    band_spectra_for_color,
    bands_from_steps,
)
from .scheduler import FrameStore, Scheduler

log = logging.getLogger(__name__)

# Panel types we can render natively. Anything else (table, text, news, ...)
# is skipped rather than drawn as a broken box.
STAT_TYPES = {"stat", "gauge", "bargauge"}
TIMESERIES_TYPES = {"timeseries", "graph", "state-timeline"}

# Points/series requested for the TRMNL X's 1872px panel (~one per pixel, with
# headroom). The bridge downsamples per the device's advertised per-dashboard
# capacity, so this is an upper bound, not the final resolution.
TRMNL_TARGET_POINTS = 3200


async def list_folder_dashboards(
    client: httpx.AsyncClient, grafana_url: str, token: str, folder_uid: str
) -> list[dict[str, str]]:
    """Return [{uid, title, slug}] for every dashboard in the folder."""
    r = await client.get(
        f"{grafana_url}/api/search",
        params={"folderUIDs": folder_uid, "type": "dash-db"},
        headers={"Authorization": f"Bearer {token}"},
        timeout=15.0,
    )
    r.raise_for_status()
    out: list[dict[str, str]] = []
    for item in r.json() or []:
        uid = item.get("uid")
        if not uid:
            continue
        out.append({
            "uid": uid,
            "title": item.get("title") or uid,
            "slug": (item.get("url") or "").rstrip("/").rsplit("/", 1)[-1] or uid,
        })
    # Stable order so the slideshow cycles the same way every push. The folder
    # dashboards are named "1"/"2"/"3"; sort by title so they cycle in order.
    out.sort(key=lambda d: d["title"])
    return out


def _time_from(from_default: str, time_from_override: str | None) -> str:
    """Honour a panel's Grafana `timeFrom` override for its data window (e.g.
    dashboard 2's stat tiles all read a 7d window). Falls back to the
    dashboard default."""
    if not time_from_override:
        return from_default
    s = time_from_override.strip()
    return s if s.startswith("now-") else f"now-{s}"


def _last_value(data: dict[str, Any]) -> float | None:
    """Recover the numeric last value of a panel's primary series from the
    normalized data (inverse of the [0,1] normalization) so we can pick the
    active threshold band."""
    series = data.get("series") or []
    if not series or not series[0].get("points"):
        return None
    y_axis = data.get("y_axis") or {}
    y_min = float(y_axis.get("min", 0.0))
    y_max = float(y_axis.get("max", 1.0))
    rng = y_max - y_min if y_max > y_min else 1.0
    last_ny = float(series[0]["points"][-1][1])
    return y_min + last_ny * rng


async def build_dashboards(
    client: httpx.AsyncClient,
    config: AppConfig,
    token: str,
    folder_uid: str,
    *,
    from_default: str = "now-7d",
    to: str = "now",
    tz: str = "America/Los_Angeles",
    palette: str = "gray",
) -> list[dict[str, Any]]:
    """Resolve the folder into the list of dashboard dicts the encoder wants.

    Each dashboard's own saved Grafana time range wins over ``from_default``, so
    you can set a dashboard's history window in Grafana (e.g. 30d for a slow
    trend, 6h for a live one) with no redeploy; per-panel ``timeFrom`` overrides
    still win within a dashboard.

    ``palette`` selects how threshold colours land in the shade bytes:
    ``"gray"`` (TRMNL X, v2 bundle) maps them to 0..15 grays; ``"spectra"``
    (reTerminal E1004, v3 bundle) maps them to Spectra 6 color codes."""
    spectra = palette == "spectra"
    shade_none = SPECTRA_WHITE if spectra else GRAY_WHITE
    stat_shade = active_threshold_spectra if spectra else active_threshold_gray
    sched = Scheduler(config, token, FrameStore())
    entries = await list_folder_dashboards(client, config.grafana_url, token, folder_uid)
    if not entries:
        log.warning("trmnl: folder %s has no dashboards", folder_uid)
        return []

    dashboards: list[dict[str, Any]] = []
    for entry in entries:
        # The dashboard's own time range is the per-dashboard default window.
        dfrom, dto = await sched._fetch_dashboard_time(client, entry["uid"])
        eff_from = dfrom or from_default
        eff_to = dto or to
        dcfg = DashboardConfig(uid=entry["uid"], from_=eff_from, to=eff_to, tz=tz)
        try:
            panels = await sched._fetch_dashboard_panels(client, dcfg)
        except Exception as e:
            log.warning("trmnl: dashboard %s expansion failed: %s", entry["uid"], e)
            continue

        panel_dicts: list[dict[str, Any]] = []
        grid_rows = 1
        for pc in panels:
            ptype = pc.panel_type
            if ptype not in STAT_TYPES and ptype not in TIMESERIES_TYPES:
                continue
            # Fetch this panel's data over its effective window (dashboard range,
            # then any per-panel timeFrom override).
            window = _time_from(eff_from, pc.time_from_override)
            pc_win = dataclasses.replace(pc, from_=window, to=eff_to)
            try:
                data = await fetch_panel_data(
                    client, config.grafana_url, token, pc_win,
                    target_points=TRMNL_TARGET_POINTS,
                    tight_axis=True,
                )
            except Exception as e:
                log.warning("trmnl: panel '%s' fetch failed: %s", pc.name, e)
                continue

            grid_rows = max(grid_rows, pc.grid_y + pc.grid_h)
            common = {
                "gx": pc.grid_x, "gy": pc.grid_y,
                "gw": pc.grid_w, "gh": pc.grid_h,
                "title": pc.name,
                "unit": unit_display_str(pc.unit),
            }

            if ptype in STAT_TYPES:
                stat = build_stat_panel(data, pc.unit, pc.decimals)
                value = _last_value(data)
                panel_dicts.append({
                    **common,
                    "type": PANEL_STAT,
                    "base_gray": stat_shade(pc.threshold_steps, value),
                    "value_str": stat["value_str"],
                    "sparkline": stat["sparkline"],
                })
            else:
                y_axis = data.get("y_axis") or {}
                axis_min = float(y_axis.get("min", 0.0))
                axis_max = float(y_axis.get("max", 1.0))
                # Shade threshold zones whenever the panel enables an area style
                # ("area", "line+area", "dashed+area", "fillbands"); line-only or
                # off styles stay clean so the chart reads well on e-ink.
                style = pc.thresholds_style or ""
                if "area" in style or style == "fillbands":
                    bands = bands_from_steps(
                        pc.threshold_steps, axis_min, axis_max,
                        **({"shade": band_spectra_for_color,
                            "none_value": SPECTRA_WHITE} if spectra else {}),
                    )
                else:
                    bands = []
                series_pts = [s.get("points") or [] for s in (data.get("series") or [])]
                panel_dicts.append({
                    **common,
                    "type": PANEL_TIMESERIES,
                    "base_gray": shade_none,
                    "y_labels": y_axis.get("labels") or [],
                    # Normalized (0=bottom..1=top) position of each y label, so
                    # the device draws them where Grafana does instead of evenly.
                    "y_label_pos": y_axis.get("positions") or [],
                    # X axis (time) labels dropped — they crowd the small charts
                    # and the "as-of" time is already on the dashboard cadence.
                    "x_labels": [],
                    "bands": bands,
                    "series": series_pts,
                })

        if not panel_dicts:
            log.warning("trmnl: dashboard '%s' produced no renderable panels", entry["title"])
            continue

        dashboards.append({
            "title": entry["title"],
            "grid_cols": 24,
            "grid_rows": grid_rows,
            "panels": panel_dicts,
        })
        log.info("trmnl: dashboard '%s' -> %d panels, %d grid rows",
                 entry["title"], len(panel_dicts), grid_rows)

    return dashboards
