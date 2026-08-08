"""push_e1004.py — build the reTerminal E1004 COLOR dashboard bundle, seal it
for the device, and PUT it to the Worker's /bundle-e1004 endpoint. Sibling of
push_trmnl.py (the TRMNL X path); invoked by its own systemd timer.

Same crypto and same wire structure as the TRMNL X, but the bundle is stamped
version 3: every shade byte carries an E Ink Spectra 6 color code instead of a
0..15 gray, so threshold state renders as real red/yellow/green on the panel.
Sealed against the E1004's own X25519 key (E1004_PUBKEY_B64).

Env:
  GRAFANA_TOKEN            Grafana API token (read).
  GRAFANA_BRIDGE_CONFIG    config.yaml path (only grafana_url is used here).
  E1004_PUBKEY_B64         E1004 device X25519 public key (base64url).
  E1004_WORKER_URL         default https://dashboard.contexa.net/bundle-e1004
  WORKER_BEARER_TOKEN      bearer for the Worker.
  E1004_FOLDER_UID         Grafana folder to cycle (default: TRMNL's f8jmvq,
                           so both displays show the same dashboards).
  E1004_FROM               fallback window when a dashboard pins none (now-7d).
  E1004_TZ                 timezone (default America/Los_Angeles).
  E1004_REFRESH_SECONDS    next_poll hint in the bundle (default 600).

Flags:
  --dry-run [PATH]   build + write the UNSEALED bundle to PATH (default
                     e1004_bundle.bin) and skip Grafana pubkey/push. Handy with
                     tools/preview_e1004.py.
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
    BUNDLE_VERSION_COLOR,
    decode_dashboard_bundle,
    encode_dashboard_bundle,
    encode_dashboard_bundle_fit,
    encode_manifest,
)
from .push import USER_AGENT, b64url_decode, seal
from .push_trmnl import (
    MAX_PLAINTEXT_BYTES,
    _dash_url,
    _push_to_worker,
)


# Local URL helpers — push_trmnl's equivalents hardcode the -trmnl paths.
def _manifest_url(worker_url: str) -> str:
    return worker_url.rsplit("/", 1)[0] + "/manifest-e1004"


def _fetch_device_capacity(worker_url: str, bearer: str) -> int | None:
    """Ask the Worker what cache capacity the E1004 last advertised. Returns
    the plaintext byte budget, or None if unknown / unreachable."""
    cap_url = worker_url.rsplit("/", 1)[0] + "/capacity-e1004"
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

log = logging.getLogger("grafana-push-e1004")

# Conservative plaintext budget used until the device reports its real
# capacity. The E1004 caches to LittleFS on its 32 MB flash, so first-push
# headroom mirrors the TRMNL default. Overridable via E1004_MAX_BUNDLE_BYTES.
DEFAULT_BUNDLE_BUDGET = 46 * 1024

# Points/series requested for the E1004's 1600px-wide landscape layout.
E1004_TARGET_POINTS = 3200


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
            from_default=from_default, tz=tz, palette="spectra",
        )
    if not dashboards:
        raise RuntimeError("no dashboards resolved from folder")
    return dashboards


def main() -> int:
    logging.basicConfig(
        level=os.environ.get("LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(message)s",
    )
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", nargs="?", const="e1004_bundle.bin", default=None,
                    metavar="PATH",
                    help="write the unsealed bundle to PATH and skip pushing")
    args = ap.parse_args()

    config_path = Path(os.environ.get("GRAFANA_BRIDGE_CONFIG", "/etc/grafana-bridge/config.yaml"))
    token = os.environ.get("GRAFANA_TOKEN", "").strip()
    worker_url = os.environ.get("E1004_WORKER_URL", "https://dashboard.contexa.net/bundle-e1004")
    bearer = os.environ.get("WORKER_BEARER_TOKEN", "").strip()
    pk_b64 = os.environ.get("E1004_PUBKEY_B64", "").strip()
    folder_uid = os.environ.get("E1004_FOLDER_UID", "f8jmvq").strip()
    from_default = os.environ.get("E1004_FROM", "now-7d").strip()
    tz = os.environ.get("E1004_TZ", "America/Los_Angeles").strip()
    refresh_seconds = int(os.environ.get("E1004_REFRESH_SECONDS", "600"))
    default_budget = int(os.environ.get("E1004_MAX_BUNDLE_BYTES", str(DEFAULT_BUNDLE_BUDGET)))

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
            log.warning("E1004_PUBKEY_B64 not set; skipping push until E1004 enrollment is complete")
            return 0
        try:
            recipient_pk = b64url_decode(pk_b64)
        except Exception as e:
            log.error("invalid E1004_PUBKEY_B64: %s", e)
            return 2
        if len(recipient_pk) != 32:
            log.error("E1004_PUBKEY_B64 must decode to 32 bytes (got %d)", len(recipient_pk))
            return 2

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

    encoded: list[tuple[bytes, str]] = []
    for i, d in enumerate(dashboards):
        body, etag, fit = encode_dashboard_bundle_fit(
            [d], next_poll=refresh_seconds, budget=budget,
            version=BUNDLE_VERSION_COLOR,
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
        combined, _ = encode_dashboard_bundle(
            dashboards, next_poll=refresh_seconds, version=BUNDLE_VERSION_COLOR
        )
        out = Path(args.dry_run)
        out.write_bytes(combined)
        decoded = decode_dashboard_bundle(combined)  # fail loud on a broken encode
        log.info(
            "dry-run: wrote combined %s (%dB, %d dashboards, build=%dms); decode OK -> %s",
            out, len(combined), n_dash, build_ms,
            [d["title"] for d in decoded["dashboards"]],
        )
        return 0

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
