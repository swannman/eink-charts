from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

import yaml


@dataclass
class TargetConfig:
    expr: str
    legend_format: str = ""
    datasource_uid: str = ""
    ref_id: str = ""


@dataclass
class PanelConfig:
    name: str
    dashboard_uid: str
    dashboard_slug: str
    panel_id: int
    from_: str = "now-6h"
    to: str = "now"
    tz: str = "America/Los_Angeles"
    theme: str = "light"
    contrast: float = 1.0
    autocontrast: bool = False
    dilate: int = 0  # MinFilter passes; thickens dark features by N pixels.
    crop_top: int = 0  # Pixels to render above target, then crop off — kills panel title bar.
    vars: dict[str, str] = field(default_factory=dict)
    dwell_seconds: int = 300
    # Filled when the panel was resolved from a dashboard rather than a static
    # entry. Used by the /data path (native chart rendering on the device).
    targets: list[TargetConfig] = field(default_factory=list)
    panel_type: str = ""
    decimals: int = 0
    # Y-axis bounds from the panel's fieldConfig. Hard bounds clip the axis;
    # soft bounds are the preferred axis range that expands if data exceeds.
    hard_min: float | None = None
    hard_max: float | None = None
    soft_min: float | None = None
    soft_max: float | None = None
    # Grafana panel `timeFrom` override, e.g. "7d". When present, the device
    # uses the matching view mode as this panel's default on navigation.
    time_from_override: str | None = None
    # Unit string from fieldConfig.defaults.unit — Grafana code like
    # "fahrenheit", "percent", "MBs". Translated to a display suffix by the
    # bridge before sending to the device.
    unit: str = ""
    # gridPos from the dashboard (x, y, w) so stat panels at the same y can
    # be grouped into one screen on the device.
    grid_y: int = 0
    grid_x: int = 0
    grid_w: int = 24


@dataclass
class DashboardConfig:
    """A whole-dashboard reference. The bridge fetches the panel list from
    Grafana and rotates through every (non-row, non-text) panel in gridPos
    order."""
    uid: str
    dwell_seconds: int = 300
    from_: str = "now-24h"
    to: str = "now"
    tz: str = "America/Los_Angeles"
    theme: str = "light"
    contrast: float = 1.0
    autocontrast: bool = False
    dilate: int = 0
    crop_top: int = 100


@dataclass
class AppConfig:
    grafana_url: str
    panels: list[PanelConfig]
    dashboards: list[DashboardConfig]
    width: int = 792
    height: int = 528
    cache_dir: Path = Path("/var/lib/grafana-bridge")
    listen_host: str = "0.0.0.0"
    listen_port: int = 8080


def load(path: Path) -> AppConfig:
    raw = yaml.safe_load(path.read_text())
    panels = [
        PanelConfig(
            name=p["name"],
            dashboard_uid=p["dashboard_uid"],
            dashboard_slug=p["dashboard_slug"],
            panel_id=int(p["panel_id"]),
            from_=p.get("from", "now-6h"),
            to=p.get("to", "now"),
            tz=p.get("tz", "America/Los_Angeles"),
            theme=p.get("theme", "light"),
            contrast=float(p.get("contrast", 1.0)),
            autocontrast=bool(p.get("autocontrast", False)),
            dilate=int(p.get("dilate", 0)),
            crop_top=int(p.get("crop_top", 0)),
            vars=p.get("vars") or {},
            dwell_seconds=int(p.get("dwell_seconds", 300)),
        )
        for p in (raw.get("panels") or [])
    ]
    dashboards = [
        DashboardConfig(
            uid=d["uid"],
            dwell_seconds=int(d.get("dwell_seconds", 300)),
            from_=d.get("from", "now-24h"),
            to=d.get("to", "now"),
            tz=d.get("tz", "America/Los_Angeles"),
            theme=d.get("theme", "light"),
            contrast=float(d.get("contrast", 1.0)),
            autocontrast=bool(d.get("autocontrast", False)),
            dilate=int(d.get("dilate", 0)),
            crop_top=int(d.get("crop_top", 100)),
        )
        for d in (raw.get("dashboards") or [])
    ]
    return AppConfig(
        grafana_url=raw["grafana_url"].rstrip("/"),
        panels=panels,
        dashboards=dashboards,
        width=int(raw.get("width", 792)),
        height=int(raw.get("height", 528)),
        cache_dir=Path(raw.get("cache_dir", "/var/lib/grafana-bridge")),
        listen_host=raw.get("listen_host", "0.0.0.0"),
        listen_port=int(raw.get("listen_port", 8080)),
    )
