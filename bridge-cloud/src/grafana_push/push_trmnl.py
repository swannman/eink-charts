"""push_trmnl.py — build the TRMNL X dashboard bundle, seal it for the device,
and PUT it to the Worker's /bundle-trmnl endpoint. Sibling of push.py (the X3
path); invoked by its own systemd timer.

Same crypto as the X3 (X25519 + AES-256-GCM via push.seal), a different payload
(the 0xCFB2 dashboard bundle) sealed against a different device key
(TRMNL_PUBKEY_B64) and delivered to a different Worker object.

Env:
  GRAFANA_TOKEN            Grafana API token (read).
  GRAFANA_BRIDGE_CONFIG    config.yaml path (only grafana_url is used here).
  TRMNL_PUBKEY_B64         TRMNL X device X25519 public key (base64url).
  TRMNL_WORKER_URL         default https://dashboard.contexa.net/bundle-trmnl
  WORKER_BEARER_TOKEN      bearer for the Worker.
  TRMNL_FOLDER_UID         Grafana folder to cycle (default f8jmvq).
  TRMNL_FROM               fallback window when a dashboard pins none (now-7d).
  TRMNL_TZ                 timezone (default America/Los_Angeles).
  TRMNL_REFRESH_SECONDS    next_poll hint in the bundle (default 600).

Flags:
  --dry-run [PATH]   build + write the UNSEALED bundle to PATH (default
                     trmnl_bundle.bin) and skip Grafana pubkey/push. Handy with
                     tools/preview_trmnl.py.
"""
from __future__ import annotations

import argparse
import asyncio
import logging
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import httpx

from .config import load as load_config
from .dashboards_trmnl import build_dashboards
from .data_trmnl import (
    decode_dashboard_bundle,
    encode_dashboard_bundle,
    encode_dashboard_bundle_fit,
    encode_manifest,
)
from .push import USER_AGENT, b64url_decode, seal

log = logging.getLogger("grafana-push-trmnl")

# Hard ceiling on the plaintext bundle regardless of what the device advertises
# — a bridge-side sanity backstop. Sized to comfortably hold a full-resolution
# (~4x, 3200 pts/series) multi-dashboard bundle now that the device caches to
# its 12 MB storage partition instead of the tiny NVS.
MAX_PLAINTEXT_BYTES = 256 * 1024

# Conservative plaintext budget used until the device reports its real capacity
# (first push, or if the Worker has no capacity on file). Kept under the
# device's ~48 KB sealed-download ceiling minus seal overhead so the very first
# bundle is guaranteed cacheable. Overridable via TRMNL_MAX_BUNDLE_BYTES.
DEFAULT_BUNDLE_BUDGET = 46 * 1024


async def _fetch_dashboards_async(
    config_path: Path,
    token: str,
    folder_uid: str,
    from_default: str,
    tz: str,
) -> list[dict]:
    config = load_config(config_path)
    async with httpx.AsyncClient() as client:
        dashboards = await build_dashboards(
            client, config, token, folder_uid,
            from_default=from_default, tz=tz,
        )
    if not dashboards:
        raise RuntimeError("no dashboards resolved from folder")
    return dashboards


def _dash_url(worker_url: str, index: int) -> str:
    """Per-dashboard object URL: .../bundle-trmnl?d=<index>."""
    sep = "&" if "?" in worker_url else "?"
    return f"{worker_url}{sep}d={index}"


def _manifest_url(worker_url: str) -> str:
    return worker_url.rsplit("/", 1)[0] + "/manifest-trmnl"


def _fetch_device_capacity(worker_url: str, bearer: str) -> int | None:
    """Ask the Worker what cache capacity the TRMNL X last advertised. Returns
    the plaintext byte budget, or None if unknown / unreachable (caller falls
    back to DEFAULT_BUNDLE_BUDGET)."""
    cap_url = worker_url.rsplit("/", 1)[0] + "/capacity-trmnl"
    req = urllib.request.Request(
        cap_url,
        headers={"Authorization": f"Bearer {bearer}", "User-Agent": USER_AGENT},
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            import json
            data = json.loads(resp.read().decode("utf-8") or "{}")
    except (urllib.error.URLError, ValueError, TimeoutError) as e:
        log.warning("capacity fetch failed (%s); using default budget", e)
        return None
    cap = data.get("cap")
    if isinstance(cap, int) and cap > 0:
        log.info("device capacity: %d bytes (reported %s)", cap, data.get("reportedAt", "?"))
        return cap
    return None


def _push_to_worker(worker_url: str, bearer: str, sealed: bytes) -> None:
    req = urllib.request.Request(
        worker_url,
        method="PUT",
        data=sealed,
        headers={
            "Authorization": f"Bearer {bearer}",
            "Content-Type": "application/octet-stream",
            "User-Agent": USER_AGENT,
        },
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        if resp.status != 200:
            raise RuntimeError(f"worker returned HTTP {resp.status}")


def main() -> int:
    logging.basicConfig(
        level=os.environ.get("LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(message)s",
    )
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", nargs="?", const="trmnl_bundle.bin", default=None,
                    metavar="PATH",
                    help="write the unsealed bundle to PATH and skip pushing")
    args = ap.parse_args()

    config_path = Path(os.environ.get("GRAFANA_BRIDGE_CONFIG", "/etc/grafana-bridge/config.yaml"))
    token = os.environ.get("GRAFANA_TOKEN", "").strip()
    worker_url = os.environ.get("TRMNL_WORKER_URL", "https://dashboard.contexa.net/bundle-trmnl")
    bearer = os.environ.get("WORKER_BEARER_TOKEN", "").strip()
    pk_b64 = os.environ.get("TRMNL_PUBKEY_B64", "").strip()
    folder_uid = os.environ.get("TRMNL_FOLDER_UID", "f8jmvq").strip()
    from_default = os.environ.get("TRMNL_FROM", "now-7d").strip()
    tz = os.environ.get("TRMNL_TZ", "America/Los_Angeles").strip()
    refresh_seconds = int(os.environ.get("TRMNL_REFRESH_SECONDS", "600"))
    default_budget = int(os.environ.get("TRMNL_MAX_BUNDLE_BYTES", str(DEFAULT_BUNDLE_BUDGET)))

    if not token:
        log.error("GRAFANA_TOKEN not set")
        return 2
    if not config_path.exists():
        log.error("config file not found: %s", config_path)
        return 2

    recipient_pk = b""
    if not args.dry_run:
        if not bearer:
            log.error("WORKER_BEARER_TOKEN not set")
            return 2
        if not pk_b64:
            log.warning("TRMNL_PUBKEY_B64 not set; skipping push until TRMNL enrollment is complete")
            return 0
        try:
            recipient_pk = b64url_decode(pk_b64)
        except Exception as e:
            log.error("invalid TRMNL_PUBKEY_B64: %s", e)
            return 2
        if len(recipient_pk) != 32:
            log.error("TRMNL_PUBKEY_B64 must decode to 32 bytes (got %d)", len(recipient_pk))
            return 2

    # Each dashboard is fetched, then fit to whatever the device says it can
    # hold *per dashboard* (falling back to a conservative default until it
    # reports). The device prefetches every dashboard and serves the slideshow
    # from cache, so each one gets the full capacity rather than a shared slice.
    budget = default_budget
    if not args.dry_run:
        reported = _fetch_device_capacity(worker_url, bearer)
        if reported is not None:
            budget = reported
    budget = min(budget, MAX_PLAINTEXT_BYTES)

    t0 = time.monotonic()
    try:
        dashboards = asyncio.run(
            _fetch_dashboards_async(config_path, token, folder_uid, from_default, tz)
        )
    except Exception as e:
        log.error("dashboard build failed: %s", e)
        return 1
    n_dash = len(dashboards)

    # Encode + fit each dashboard to its own single-dashboard bundle.
    encoded: list[tuple[bytes, str]] = []
    for i, d in enumerate(dashboards):
        body, etag, fit = encode_dashboard_bundle_fit(
            [d], next_poll=refresh_seconds, budget=budget
        )
        if len(body) > MAX_PLAINTEXT_BYTES:
            log.error("dashboard %d '%s' too large: %d > %d",
                      i, d.get("title", "?"), len(body), MAX_PLAINTEXT_BYTES)
            return 1
        note = ("downsampled to %d pts/series" % fit["kept_points"]
                if fit.get("downsampled") else "full res, %d pts/series" % fit["kept_points"])
        log.info("dashboard %d '%s': %dB (%s, budget %dB)",
                 i, d.get("title", "?"), len(body), note, fit["budget"])
        encoded.append((body, etag))
    build_ms = int((time.monotonic() - t0) * 1000)

    if args.dry_run:
        # Emit a combined bundle for tools/preview_trmnl.py (layout is identical
        # to what's pushed; only the transport is split per-dashboard).
        combined, _ = encode_dashboard_bundle(dashboards, next_poll=refresh_seconds)
        out = Path(args.dry_run)
        out.write_bytes(combined)
        decoded = decode_dashboard_bundle(combined)  # fail loud on a broken encode
        log.info(
            "dry-run: wrote combined %s (%dB, %d dashboards, build=%dms); decode OK -> %s",
            out, len(combined), n_dash, build_ms,
            [d["title"] for d in decoded["dashboards"]],
        )
        return 0

    # Push each dashboard object, then the manifest last so the device never
    # sees a manifest referencing an object that isn't up yet.
    etags = [etag for _, etag in encoded]
    total_sealed = 0
    try:
        for i, (body, _etag) in enumerate(encoded):
            sealed = seal(recipient_pk, body)
            total_sealed += len(sealed)
            _push_to_worker(_dash_url(worker_url, i), bearer, sealed)
        manifest = encode_manifest(etags, refresh_seconds)
        _push_to_worker(_manifest_url(worker_url), bearer, manifest)
    except urllib.error.HTTPError as e:
        log.error("worker push failed: HTTP %d %s", e.code, e.reason)
        return 1
    except (urllib.error.URLError, RuntimeError) as e:
        log.error("worker push failed: %s", e)
        return 1

    log.info(
        "pushed: %d dashboards, sealed=%dB total, manifest etags=%s build=%dms",
        n_dash, total_sealed, etags, build_ms,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
