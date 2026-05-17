from __future__ import annotations

import asyncio
import dataclasses
import hashlib
import logging
import os
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import AsyncIterator

import httpx
from fastapi import FastAPI, Header, Query, Response

from .config import load
from .data import encode_panel_payload, fetch_panel_data
from .scheduler import FrameStore, Scheduler

log = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    config_path = Path(os.environ.get("GRAFANA_BRIDGE_CONFIG", "/etc/grafana-bridge/config.yaml"))
    token = os.environ.get("GRAFANA_TOKEN")
    if not token:
        raise RuntimeError("GRAFANA_TOKEN env var is required")

    config = load(config_path)
    store = FrameStore()
    cache_path = config.cache_dir / "frame.bin"
    scheduler = Scheduler(config, token, store, cache_path=cache_path)
    task = asyncio.create_task(scheduler.run(), name="scheduler")
    # Bundle refresher runs alongside the PNG scheduler so /data/all is always
    # warm. 5-minute refresh keeps the cached data comfortably fresher than
    # the device's 15-min-on-zoom poll cadence.
    bundle_task = asyncio.create_task(
        scheduler.run_bundle_refresher(refresh_interval_sec=240),
        name="bundle-refresher",
    )

    app.state.config = config
    app.state.store = store
    app.state.scheduler = scheduler
    log.info(
        "grafana-bridge ready: %d panels, %dx%d, listening on %s:%d",
        len(config.panels),
        config.width,
        config.height,
        config.listen_host,
        config.listen_port,
    )

    try:
        yield
    finally:
        scheduler.stop()
        task.cancel()
        bundle_task.cancel()
        for t in (task, bundle_task):
            try:
                await t
            except (asyncio.CancelledError, Exception):
                pass


app = FastAPI(lifespan=lifespan, title="grafana-bridge")


@app.get("/healthz")
def healthz() -> dict[str, object]:
    s: FrameStore = app.state.store
    return {
        "ok": s.buffer is not None,
        "last_render": s.last_render,
        "last_error": s.last_error,
        "frame_index": s.index,
        "frame_total": s.total,
        "panel_name": s.panel_name,
        "etag": s.etag,
    }


@app.get("/frame")
def frame(if_none_match: str | None = Header(default=None, alias="If-None-Match")) -> Response:
    s: FrameStore = app.state.store
    if s.buffer is None:
        return Response(status_code=503, content=b"no frame rendered yet")
    if if_none_match and if_none_match.strip('"') == s.etag:
        return Response(status_code=304)
    headers = {
        "X-Frame-Index": str(s.index),
        "X-Frame-Total": str(s.total),
        "X-Frame-Etag": s.etag,
        "X-Panel-Name": s.panel_name,
        "X-Next-Poll-Seconds": str(s.next_poll_seconds),
        "ETag": f'"{s.etag}"',
        "Cache-Control": "no-store",
    }
    return Response(content=s.buffer, media_type="application/octet-stream", headers=headers)


@app.get("/data")
async def data(
    if_none_match: str | None = Header(default=None, alias="If-None-Match"),
    from_: str | None = Query(default=None, alias="from"),
    to: str | None = Query(default=None),
) -> Response:
    """JSON payload for native on-device chart rendering. See data.py for
    schema; the device parses with ArduinoJson and feeds the values into
    its own drawTitle/drawAxes/plotLine pipeline.

    `from` / `to` override the panel's default time range for a one-shot
    fresh render — bypasses the cache. Used by the device's long-press zoom."""
    s: FrameStore = app.state.store
    if from_ or to:
        scheduler: Scheduler = app.state.scheduler
        panel = scheduler.current_panel
        if panel is None:
            return Response(status_code=503, content=b"no panel rendered yet")
        override = dataclasses.replace(panel, from_=from_ or panel.from_, to=to or panel.to)
        try:
            async with httpx.AsyncClient() as client:
                data = await fetch_panel_data(client, app.state.config.grafana_url, scheduler.token, override)
        except Exception as e:
            log.exception("on-demand /data fetch failed")
            return Response(status_code=502, content=f"upstream error: {e}".encode())
        body, etag = encode_panel_payload(s.index, s.total, data, s.next_poll_seconds)
        if if_none_match and if_none_match.strip('"') == etag:
            return Response(status_code=304)
        headers = {
            "X-Frame-Index": str(s.index),
            "X-Frame-Total": str(s.total),
            "X-Frame-Etag": etag,
            "X-Panel-Name": panel.name,
            "X-Next-Poll-Seconds": str(s.next_poll_seconds),
            "ETag": f'"{etag}"',
            "Cache-Control": "no-store",
        }
        return Response(content=body, media_type="application/json", headers=headers)

    if s.data_body is None:
        return Response(status_code=503, content=b"no data rendered yet")
    if if_none_match and if_none_match.strip('"') == s.data_etag:
        return Response(status_code=304)
    headers = {
        "X-Frame-Index": str(s.index),
        "X-Frame-Total": str(s.total),
        "X-Frame-Etag": s.data_etag,
        "X-Panel-Name": s.panel_name,
        "X-Next-Poll-Seconds": str(s.next_poll_seconds),
        "ETag": f'"{s.data_etag}"',
        "Cache-Control": "no-store",
    }
    return Response(content=s.data_body, media_type="application/json", headers=headers)


@app.get("/data/all")
async def data_all(
    if_none_match: str | None = Header(default=None, alias="If-None-Match"),
    battery_mv: int | None = Header(default=None, alias="X-Battery-MV"),
) -> Response:
    """Bundle of every panel's data in two time windows (default + 2h zoom).
    Served from a background-refreshed in-memory cache so the device's HTTP
    request is instant — no waiting on Grafana queries. Also records the
    device's battery reading from X-Battery-MV for the synthetic battery
    panel."""
    scheduler: Scheduler = app.state.scheduler
    if battery_mv is not None:
        scheduler.record_battery_reading(int(battery_mv))
        log.info("battery reading: %d mV", battery_mv)
    s: FrameStore = app.state.store
    if s.bundle_body is None:
        return Response(status_code=503, content=b"bundle not rendered yet")
    if if_none_match and if_none_match.strip('"') == s.bundle_etag:
        return Response(status_code=304)
    headers = {
        "X-Frame-Total": str(len(scheduler.all_panels)),
        "X-Frame-Etag": s.bundle_etag,
        "X-Bundle-Age-Sec": str(int(time.time() - s.bundle_last_render)),
        "ETag": f'"{s.bundle_etag}"',
        "Cache-Control": "no-store",
    }
    return Response(content=s.bundle_body, media_type="application/octet-stream", headers=headers)


@app.post("/advance")
async def advance() -> dict[str, object]:
    store: FrameStore = app.state.store
    app.state.scheduler.advance()
    # Block until the next post-advance render completes so callers can
    # immediately GET /frame and see the new panel.
    completed = await app.state.scheduler.wait_advance_completed(timeout=10.0)
    return {
        "ok": completed,
        "etag": store.etag,
        "panel_name": store.panel_name,
        "frame_index": store.index,
    }
