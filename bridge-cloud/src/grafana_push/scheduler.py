from __future__ import annotations

import asyncio
import hashlib
import logging
import time
from dataclasses import dataclass, field
from pathlib import Path

import httpx

from collections import deque

from .config import AppConfig, DashboardConfig, PanelConfig, TargetConfig
from .data import (
    build_battery_panel,
    build_stat_panel,
    encode_bundle_screens,
    encode_panel_payload,
    fetch_panel_data,
    map_time_from_to_view,
    validate_panel_data,
    SCREEN_CHART,
    SCREEN_STAT_GROUP,
    VIEW_24H,
)
from .render import fetch_panel_png, png_to_framebuffer
import dataclasses

log = logging.getLogger(__name__)


@dataclass
class FrameStore:
    buffer: bytes | None = None
    etag: str = ""
    index: int = 0
    total: int = 0
    panel_name: str = ""
    next_poll_seconds: int = 300
    last_render: float = 0.0
    last_error: str | None = None
    # JSON payload for the /data path (device-side native rendering). Separate
    # etag because the data payload changes more often than the dithered image
    # (anti-aliasing noise can stabilize at the same buffer hash).
    data_body: bytes | None = None
    data_etag: str = ""
    # Binary bundle for /data/all. Pre-rendered by a background task so the
    # device's HTTP request returns instantly from cache.
    bundle_body: bytes | None = None
    bundle_etag: str = ""
    bundle_last_render: float = 0.0


class Scheduler:
    def __init__(self, config: AppConfig, token: str, store: FrameStore, cache_path: Path | None = None) -> None:
        self.config = config
        self.token = token
        self.store = store
        self.cache_path = cache_path
        # Tracks the panel that was rendered most recently so /data?from=…
        # can do an on-demand override-fetch against the same target.
        self.current_panel: PanelConfig | None = None
        # Full list resolved this rotation — used by /data/all so the device
        # can pull every panel in one round trip and serve them from cache.
        self.all_panels: list[PanelConfig] = []
        # In-memory rolling battery history (epoch_secs, millivolts). One entry
        # per device WiFi cycle, ~24 entries/day at default cadence. Memory-
        # only is fine — we accept loss across bridge restarts.
        self.battery_history: deque[tuple[float, int]] = deque(maxlen=2000)
        self._advance_event = asyncio.Event()
        self._advance_completed = asyncio.Event()
        self._stop = asyncio.Event()
        self._load_cache()

    def _load_cache(self) -> None:
        if self.cache_path and self.cache_path.exists():
            try:
                data = self.cache_path.read_bytes()
                expected = (self.config.width // 8) * self.config.height
                if len(data) == expected:
                    self.store.buffer = data
                    self.store.etag = hashlib.sha256(data).hexdigest()[:16]
                    self.store.total = len(self.config.panels)
                    log.info("loaded cached frame from %s", self.cache_path)
            except OSError as e:
                log.warning("failed to load cache: %s", e)

    def _save_cache(self) -> None:
        if self.cache_path and self.store.buffer is not None:
            try:
                self.cache_path.parent.mkdir(parents=True, exist_ok=True)
                self.cache_path.write_bytes(self.store.buffer)
            except OSError as e:
                log.warning("failed to save cache: %s", e)

    def advance(self) -> None:
        self._advance_completed.clear()
        self._advance_event.set()

    def record_battery_reading(self, mv: int) -> None:
        if mv < 2500 or mv > 5000:
            return  # implausible — drop
        self.battery_history.append((time.time(), int(mv)))

    async def render_bundle_once(self, client: httpx.AsyncClient) -> bool:
        """Fetch data for every resolved panel (default + 2h zoom) and update
        the cached bundle. Called by run_bundle_refresher on a fixed cadence
        so /data/all can return from cache instantly.

        Re-resolves the dashboard each cycle so newly-added Grafana panels
        appear within one refresh interval instead of waiting for the slow
        per-rotation re-resolve in run()."""
        try:
            resolved = await self._resolve_panels(client)
            if resolved:
                self.all_panels = resolved
        except Exception as e:
            log.warning("bundle refresh: re-resolve failed (%s) — using last list", e)

        panels = list(self.all_panels)
        if not panels:
            return False

        async def fetch_views(p: PanelConfig):
            d24 = await fetch_panel_data(client, self.config.grafana_url, self.token, p)
            p2h = dataclasses.replace(p, from_="now-2h", to="now")
            d2 = await fetch_panel_data(client, self.config.grafana_url, self.token, p2h)
            p7d = dataclasses.replace(p, from_="now-7d", to="now")
            d7 = await fetch_panel_data(client, self.config.grafana_url, self.token, p7d)
            return d24, d2, d7

        try:
            results = await asyncio.gather(*[fetch_views(p) for p in panels])
        except Exception as e:
            log.exception("bundle render failed")
            self.store.last_error = repr(e)
            return False

        # Drop panels that won't render acceptably on the device. We check the
        # 24h view because it's the most-data-rich; a panel dropped here will
        # also have unreliable 2h/7d views.
        keep: list[tuple[PanelConfig, tuple[dict, dict, dict]]] = []
        for p, r in zip(panels, results):
            ok, reason = validate_panel_data(r[0])
            if not ok:
                log.warning("bundle: dropping panel '%s' — %s", p.name, reason)
                continue
            keep.append((p, r))
        if not keep:
            log.warning("bundle: every panel dropped — keeping prior cache")
            return False
        panels = [k[0] for k in keep]
        results = [k[1] for k in keep]

        # Group panels into screens. Stat panels at the same gridPos.y are
        # combined into a single side-by-side screen (up to 3 across).
        # Charts always get their own full-width screen.
        screens_24h: list[dict] = []
        screens_2h: list[dict] = []
        screens_7d: list[dict] = []
        default_views: list[int] = []

        i = 0
        while i < len(panels):
            p = panels[i]
            if p.panel_type == "stat":
                # Greedily collect consecutive stat panels sharing this y.
                group_end = i + 1
                while (group_end < len(panels)
                       and panels[group_end].panel_type == "stat"
                       and panels[group_end].grid_y == p.grid_y
                       and (group_end - i) < 3):
                    group_end += 1
                group = list(zip(panels[i:group_end], results[i:group_end]))
                stats_24h = [build_stat_panel(r[0], gp.unit, gp.decimals) for gp, r in group]
                stats_2h = [build_stat_panel(r[1], gp.unit, gp.decimals) for gp, r in group]
                stats_7d = [build_stat_panel(r[2], gp.unit, gp.decimals) for gp, r in group]
                screens_24h.append({"type": SCREEN_STAT_GROUP, "entries": stats_24h})
                screens_2h.append({"type": SCREEN_STAT_GROUP, "entries": stats_2h})
                screens_7d.append({"type": SCREEN_STAT_GROUP, "entries": stats_7d})
                # Stat screens don't respond to view-mode cycling — pick the
                # default of the first panel for the default-view byte.
                default_views.append(map_time_from_to_view(p.time_from_override))
                i = group_end
            else:
                r = results[i]
                screens_24h.append({"type": SCREEN_CHART, "entries": [r[0]]})
                screens_2h.append({"type": SCREEN_CHART, "entries": [r[1]]})
                screens_7d.append({"type": SCREEN_CHART, "entries": [r[2]]})
                default_views.append(map_time_from_to_view(p.time_from_override))
                i += 1

        # Append the synthetic battery panel last so it shares the same
        # cycling/zoom behavior as the real ones.
        now = time.time()
        tz = panels[0].tz if panels else "America/Los_Angeles"
        snap = list(self.battery_history)
        screens_24h.append({"type": SCREEN_CHART, "entries": [build_battery_panel(snap, now - 24 * 3600, now, tz)]})
        screens_2h.append({"type": SCREEN_CHART, "entries": [build_battery_panel(snap, now - 2 * 3600, now, tz)]})
        screens_7d.append({"type": SCREEN_CHART, "entries": [build_battery_panel(snap, now - 7 * 86400, now, tz)]})
        default_views.append(VIEW_24H)

        dwell = max((p.dwell_seconds for p in panels), default=900)
        body, etag = encode_bundle_screens(
            screens_24h, screens_2h, screens_7d, default_views, max(dwell, 900),
        )
        self.store.bundle_body = body
        self.store.bundle_etag = etag
        self.store.bundle_last_render = time.time()
        log.info("bundle rendered: %d panels, %d bytes, etag=%s", len(panels), len(body), etag)
        return True

    async def run_bundle_refresher(self, refresh_interval_sec: int = 300) -> None:
        """Periodically refresh the /data/all cache so the device's HTTP
        request is a memory read, not a 12-Grafana-query wait."""
        async with httpx.AsyncClient() as client:
            # Wait for the scheduler to resolve panels at startup, then render
            # the bundle immediately so the first device request post-restart
            # doesn't race the cold start and 503 out.
            while not self._stop.is_set() and not self.all_panels:
                try:
                    await asyncio.wait_for(self._stop.wait(), timeout=0.5)
                except asyncio.TimeoutError:
                    pass
            while not self._stop.is_set():
                await self.render_bundle_once(client)
                try:
                    await asyncio.wait_for(self._stop.wait(), timeout=refresh_interval_sec)
                except asyncio.TimeoutError:
                    pass

    async def wait_advance_completed(self, timeout: float) -> bool:
        try:
            await asyncio.wait_for(self._advance_completed.wait(), timeout)
            return True
        except asyncio.TimeoutError:
            return False

    def stop(self) -> None:
        self._stop.set()
        self._advance_event.set()

    async def _fetch_dashboard_panels(
        self, client: httpx.AsyncClient, dashboard: DashboardConfig
    ) -> list[PanelConfig]:
        url = f"{self.config.grafana_url}/api/dashboards/uid/{dashboard.uid}"
        r = await client.get(
            url,
            headers={"Authorization": f"Bearer {self.token}"},
            timeout=15.0,
        )
        r.raise_for_status()
        data = r.json()
        slug = data.get("meta", {}).get("slug", dashboard.uid)
        raw_panels = data.get("dashboard", {}).get("panels", []) or []
        # Sort by grid position (top-to-bottom, left-to-right) — that's
        # the order panels are visually arranged in the Grafana UI.
        raw_panels.sort(
            key=lambda p: (
                p.get("gridPos", {}).get("y", 0),
                p.get("gridPos", {}).get("x", 0),
            )
        )
        out: list[PanelConfig] = []
        for p in raw_panels:
            ptype = p.get("type", "")
            if ptype in ("row", "text"):
                continue
            targets = self._extract_targets(p)
            fc = (p.get("fieldConfig") or {}).get("defaults") or {}
            decimals = int(fc.get("decimals") or 0)
            unit = fc.get("unit") or ""
            custom = fc.get("custom") or {}
            def _f(v) -> float | None:
                if v is None:
                    return None
                try:
                    return float(v)
                except (TypeError, ValueError):
                    return None
            hard_min = _f(fc.get("min"))
            hard_max = _f(fc.get("max"))
            soft_min = _f(custom.get("axisSoftMin"))
            soft_max = _f(custom.get("axisSoftMax"))
            time_from_override = p.get("timeFrom")
            gp = p.get("gridPos") or {}
            out.append(
                PanelConfig(
                    name=p.get("title") or f"panel_{p.get('id', '?')}",
                    dashboard_uid=dashboard.uid,
                    dashboard_slug=slug,
                    panel_id=int(p["id"]),
                    from_=dashboard.from_,
                    to=dashboard.to,
                    tz=dashboard.tz,
                    theme=dashboard.theme,
                    contrast=dashboard.contrast,
                    autocontrast=dashboard.autocontrast,
                    dilate=dashboard.dilate,
                    crop_top=dashboard.crop_top,
                    dwell_seconds=dashboard.dwell_seconds,
                    targets=targets,
                    panel_type=ptype,
                    decimals=decimals,
                    hard_min=hard_min,
                    hard_max=hard_max,
                    soft_min=soft_min,
                    soft_max=soft_max,
                    time_from_override=time_from_override,
                    unit=unit,
                    grid_y=int(gp.get("y", 0)),
                    grid_x=int(gp.get("x", 0)),
                    grid_w=int(gp.get("w", 24)),
                )
            )
        return out

    @staticmethod
    def _extract_targets(panel_json: dict) -> list[TargetConfig]:
        """Pull the panel's queries out of the dashboard JSON, keeping enough
        metadata for the /data path to execute them against the right
        datasource. Panel-level datasource is the fallback when individual
        targets don't override it."""
        panel_ds = (panel_json.get("datasource") or {})
        default_ds_uid = panel_ds.get("uid") or ""
        out: list[TargetConfig] = []
        for t in panel_json.get("targets") or []:
            expr = t.get("expr") or t.get("query") or ""
            if not expr:
                continue
            tgt_ds = t.get("datasource") or {}
            ds_uid = tgt_ds.get("uid") or default_ds_uid
            out.append(
                TargetConfig(
                    expr=expr,
                    legend_format=t.get("legendFormat") or "",
                    datasource_uid=ds_uid,
                    ref_id=t.get("refId") or "",
                )
            )
        return out

    async def _resolve_panels(self, client: httpx.AsyncClient) -> list[PanelConfig]:
        """Combine static panels with dashboards expanded via the Grafana API."""
        resolved = list(self.config.panels)
        for dashboard in self.config.dashboards:
            try:
                resolved.extend(await self._fetch_dashboard_panels(client, dashboard))
            except Exception as e:
                log.warning("dashboard %s expansion failed: %s", dashboard.uid, e)
        return resolved

    async def run(self) -> None:
        async with httpx.AsyncClient() as client:
            i = 0
            panels: list[PanelConfig] = []
            # True iff the upcoming render came from an explicit /advance call,
            # so we can signal _advance_completed after the render lands.
            from_advance = False
            while not self._stop.is_set():
                # Re-resolve at the start of each full rotation so newly-added
                # panels in a referenced dashboard get picked up.
                if not panels or i % max(len(panels), 1) == 0:
                    panels = await self._resolve_panels(client)
                    if panels:
                        log.info("resolved %d panels: %s", len(panels), [p.name for p in panels])
                        self.all_panels = panels

                if not panels:
                    log.warning("no panels resolved; retrying in 30s")
                    await asyncio.sleep(30)
                    continue

                idx = i % len(panels)
                panel = panels[idx]
                self.current_panel = panel
                try:
                    png = await fetch_panel_png(
                        client,
                        self.config.grafana_url,
                        self.token,
                        panel,
                        self.config.width,
                        self.config.height + panel.crop_top,
                    )
                    buf = png_to_framebuffer(
                        png,
                        self.config.width,
                        self.config.height,
                        contrast=panel.contrast,
                        autocontrast=panel.autocontrast,
                        dilate=panel.dilate,
                    )
                    etag = hashlib.sha256(buf).hexdigest()[:16]
                    self.store.buffer = buf
                    self.store.etag = etag
                    self.store.index = idx
                    self.store.total = len(panels)
                    self.store.panel_name = panel.name
                    self.store.next_poll_seconds = panel.dwell_seconds
                    self.store.last_render = time.time()
                    self.store.last_error = None
                    self._save_cache()
                    log.info(
                        "rendered panel %s (%d/%d) etag=%s",
                        panel.name,
                        idx + 1,
                        len(panels),
                        etag,
                    )
                except Exception as e:
                    self.store.last_error = repr(e)
                    log.exception("failed to render panel %s", panel.name)

                # Data path: parallel to the PNG path so a failure here doesn't
                # break the /frame output, and vice-versa. Static panels (no
                # introspected targets) skip this — they have no PromQL to run.
                if panel.targets:
                    try:
                        data = await fetch_panel_data(
                            client, self.config.grafana_url, self.token, panel
                        )
                        body, data_etag = encode_panel_payload(
                            idx, len(panels), data, panel.dwell_seconds
                        )
                        self.store.data_body = body
                        self.store.data_etag = data_etag
                        log.info(
                            "data panel %s (%d/%d) etag=%s bytes=%d series=%d",
                            panel.name,
                            idx + 1,
                            len(panels),
                            data_etag,
                            len(body),
                            len(data.get("series", [])),
                        )
                    except Exception as e:
                        self.store.last_error = repr(e)
                        log.exception("failed to fetch data for panel %s", panel.name)

                # Signal advance-completed AFTER the render that came from an
                # explicit /advance call lands, so blocking /advance callers
                # see the new panel rather than racing the pre-existing render.
                if from_advance:
                    self._advance_completed.set()
                    from_advance = False

                try:
                    await asyncio.wait_for(self._advance_event.wait(), timeout=panel.dwell_seconds)
                    self._advance_event.clear()
                    from_advance = True
                except asyncio.TimeoutError:
                    pass
                i += 1
