"""push.py — build the bundle in-process, encrypt it for the X3, and PUT
it to the public Worker. Invoked by a systemd timer every 4 min; runs
once per invocation and exits.

Encryption is X25519 ECDH + AES-256-GCM with HKDF-SHA256 key derivation —
see seal() for the exact wire format. The Worker is outside the trust
boundary; only the X3 (which holds the private key) can decrypt.

There is no longer a separate long-running bridge process; this script
loads the config, queries Grafana, builds the bundle via the same
Scheduler.render_bundle_once() code path that the old FastAPI server used,
and pushes the sealed result.
"""

from __future__ import annotations

import asyncio
import base64
import logging
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import httpx
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey,
    X25519PublicKey,
)
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

from .config import load as load_config
from .scheduler import FrameStore, Scheduler

log = logging.getLogger("grafana-push")

# Bumped only when the wire format changes incompatibly. Both Pi and X3 must
# agree on this string — it's mixed into HKDF, so a mismatch fails decryption
# rather than silently producing garbage.
HKDF_INFO = b"EInkCharts seal v1"

# Reject implausibly large bundles before encrypting + uploading. The Worker
# also enforces MAX_BUNDLE_BYTES (65536); this one's just a sanity check.
MAX_PLAINTEXT_BYTES = 64 * 1024

# Cloudflare's WAF rejects the default "Python-urllib/x.y" UA with 403. Any
# non-default string passes; using a project-specific token keeps it
# identifiable in Worker logs.
USER_AGENT = "einkcharts-push/1"


def seal(recipient_pk_bytes: bytes, plaintext: bytes) -> bytes:
    """Sealed-box style encryption for one-shot, recipient-only delivery.

    Wire format (returned bytes): ``ephemeral_pk || nonce || ciphertext_and_tag``
      - ephemeral_pk: 32 bytes (X25519 public key)
      - nonce:        12 bytes (random)
      - ciphertext:   N bytes (same length as plaintext)
      - auth tag:     16 bytes (Poly1305)

    Total overhead: 60 bytes.
    """
    if len(recipient_pk_bytes) != 32:
        raise ValueError(f"recipient pk must be 32 bytes, got {len(recipient_pk_bytes)}")

    esk = X25519PrivateKey.generate()
    epk = esk.public_key().public_bytes_raw()
    recipient_pk = X25519PublicKey.from_public_bytes(recipient_pk_bytes)
    shared = esk.exchange(recipient_pk)
    # Mixing both pubkeys into the salt binds the derived key to this specific
    # sender/recipient pair, defending against unknown-key-share attacks.
    key = HKDF(
        algorithm=hashes.SHA256(),
        length=32,
        salt=epk + recipient_pk_bytes,
        info=HKDF_INFO,
    ).derive(shared)
    nonce = os.urandom(12)
    ct = AESGCM(key).encrypt(nonce, plaintext, associated_data=None)
    return epk + nonce + ct


def unseal(recipient_sk_bytes: bytes, sealed: bytes) -> bytes:
    """Inverse of seal(). Only used in tests on the Pi side; the X3 has its
    own implementation against mbedTLS."""
    if len(sealed) < 32 + 12 + 16:
        raise ValueError("sealed payload too short")
    epk_bytes = sealed[:32]
    nonce = sealed[32:44]
    ct = sealed[44:]
    sk = X25519PrivateKey.from_private_bytes(recipient_sk_bytes)
    epk = X25519PublicKey.from_public_bytes(epk_bytes)
    recipient_pk_bytes = sk.public_key().public_bytes_raw()
    shared = sk.exchange(epk)
    key = HKDF(
        algorithm=hashes.SHA256(),
        length=32,
        salt=epk_bytes + recipient_pk_bytes,
        info=HKDF_INFO,
    ).derive(shared)
    return AESGCM(key).decrypt(nonce, ct, associated_data=None)


def b64url_decode(s: str) -> bytes:
    s = s.strip()
    pad = "=" * (-len(s) % 4)
    return base64.urlsafe_b64decode(s + pad)


async def _build_bundle_async(config_path: Path, token: str) -> tuple[bytes, str]:
    """Run one bundle render against Grafana and return (body, etag).
    Mirrors what the legacy FastAPI bridge did on its background timer,
    but synchronous to the caller — no long-running process needed."""
    config = load_config(config_path)
    store = FrameStore()
    scheduler = Scheduler(config, token, store)
    async with httpx.AsyncClient() as client:
        # render_bundle_once() resolves panels then queries Grafana for the
        # 24h/2h/7d views of each. Without battery telemetry the synthetic
        # battery panel will be empty — that's expected post-listener.
        ok = await scheduler.render_bundle_once(client)
    if not ok or store.bundle_body is None:
        raise RuntimeError(
            f"bundle render failed (last_error={store.last_error or 'unknown'})"
        )
    return store.bundle_body, store.bundle_etag


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

    config_path = Path(os.environ.get("GRAFANA_BRIDGE_CONFIG", "/etc/grafana-bridge/config.yaml"))
    token = os.environ.get("GRAFANA_TOKEN", "").strip()
    worker_url = os.environ.get("WORKER_URL", "https://dashboard.contexa.net/bundle")
    bearer = os.environ.get("WORKER_BEARER_TOKEN", "").strip()
    pk_b64 = os.environ.get("X3_PUBKEY_B64", "").strip()

    if not token:
        log.error("GRAFANA_TOKEN not set")
        return 2
    if not bearer:
        log.error("WORKER_BEARER_TOKEN not set")
        return 2
    if not pk_b64:
        # First-boot: the X3 hasn't been provisioned yet. Don't fail loud —
        # systemd will keep retrying every 4 min, and once the operator pastes
        # the pubkey into the env file we'll start succeeding.
        log.warning("X3_PUBKEY_B64 not set; skipping push until X3 enrollment is complete")
        return 0
    if not config_path.exists():
        log.error("config file not found: %s", config_path)
        return 2

    try:
        recipient_pk = b64url_decode(pk_b64)
    except Exception as e:
        log.error("invalid X3_PUBKEY_B64: %s", e)
        return 2
    if len(recipient_pk) != 32:
        log.error("X3_PUBKEY_B64 must decode to 32 bytes (got %d)", len(recipient_pk))
        return 2

    t0 = time.monotonic()
    try:
        plaintext, bundle_etag = asyncio.run(_build_bundle_async(config_path, token))
    except Exception as e:
        log.error("bundle render failed: %s", e)
        return 1
    if not plaintext:
        log.error("bundle render returned empty body")
        return 1
    if len(plaintext) > MAX_PLAINTEXT_BYTES:
        log.error("bundle too large: %d > %d", len(plaintext), MAX_PLAINTEXT_BYTES)
        return 1
    build_ms = int((time.monotonic() - t0) * 1000)

    t1 = time.monotonic()
    sealed = seal(recipient_pk, plaintext)
    seal_ms = int((time.monotonic() - t1) * 1000)

    t2 = time.monotonic()
    try:
        _push_to_worker(worker_url, bearer, sealed)
    except urllib.error.HTTPError as e:
        log.error("worker push failed: HTTP %d %s", e.code, e.reason)
        return 1
    except (urllib.error.URLError, RuntimeError) as e:
        log.error("worker push failed: %s", e)
        return 1
    push_ms = int((time.monotonic() - t2) * 1000)

    log.info(
        "pushed: plaintext=%dB sealed=%dB etag=%s build=%dms seal=%dms push=%dms",
        len(plaintext),
        len(sealed),
        bundle_etag or "?",
        build_ms,
        seal_ms,
        push_ms,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
