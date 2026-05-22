# bridge-cloud/ — Pi push service

Self-contained Python package that builds Grafana dashboard bundles,
seals them with the X3's public key, and pushes them to a public
Cloudflare Worker every 4 minutes. No inbound port on the Pi; the X3
fetches the encrypted bundle from the Worker over whatever network it's
on (Wi-Fi or, as a fallback, BLE via the iOS companion app).

> Pairs with [`../firmware-cloud/`](../firmware-cloud/README.md) (X3),
> [`../worker/`](../worker/README.md) (Cloudflare Worker), and
> [`../ios-app/`](../ios-app/README.md) (BLE fallback transport).

## When to use this

- You want the display to work from any network — home WiFi, phone
  hotspot, friend's WiFi.
- You don't want to expose any port on your home network to the internet.
- You're willing to set up a Cloudflare account (free tier is plenty) and
  trust their infrastructure not to drop bundles — but not to read them.
  Bundles are end-to-end encrypted; Cloudflare sees only ciphertext.

## How it works

A systemd timer (`grafana-push.timer`) fires the service every 4 minutes:

1. **Pull battery history** from the Worker's `/battery` endpoint — the
   X3 PUTs its BQ27220 voltage there after every fetch, and the Worker
   keeps a rolling 7 days. Empty list on the first run is fine.
2. **Build the bundle in-process** via `Scheduler.render_bundle_once()`
   — queries Grafana for each panel × view window, validates, encodes.
   Seeds `scheduler.battery_history` with the list from step 1 so the
   synthetic Battery (V) panel renders.
3. **Seal** the bundle for the X3 using a fresh ephemeral X25519 keypair
   per push (forward secrecy):
   - `shared = X25519(ephemeral_sk, x3_pk)`
   - `key = HKDF-SHA256(shared, salt=epk||x3_pk, info="EInkCharts seal v1")`
   - `ciphertext, tag = AES-256-GCM(key, nonce, plaintext)`
4. **PUT** the sealed blob (`epk || nonce || ciphertext || tag`, 60-byte
   overhead) to `https://dashboard.contexa.net/bundle` with the bearer
   token from `/etc/default/grafana-push`.

One-shot per invocation; the process exits between fires. No long-running
state. Total wall time per push: ~1-3 s (Grafana queries dominate).

## Encryption design

Public-key. The X3 holds the private key; the Pi only ever has the
public key, the Worker only ever sees ciphertext.

| Step | Where the key lives |
|---|---|
| X3 generates X25519 keypair on first boot | NVS namespace `x3-crypto` on the device only |
| X3 displays pubkey as a QR + visible text | On-screen, one-time |
| Operator copies pubkey into `/etc/default/grafana-push` | Pi (`X3_PUBKEY_B64=`) |
| Pi seals each push against that pubkey | Pi |
| Cloudflare stores the sealed blob | Worker + R2 (ciphertext only) |
| X3 fetches + decrypts with its private key | Device |

A new ephemeral X25519 keypair is generated on the Pi for every push, so
even if the Pi's saved pubkey somehow leaked, past bundles intercepted
from Cloudflare can't be retroactively decrypted — they're each tied to
a one-shot ephemeral that's destroyed after the push completes.

If the Pi's input copy of `X3_PUBKEY_B64` is wrong (typo, wrong device),
the X3 will fail to decrypt — Poly1305 tag mismatch — and the bundle
cache won't update. Watch the X3's serial log for `bundle_seal:
gcm_decrypt err=-0x...` to catch this.

## Config

| Path | Purpose |
|---|---|
| `/etc/grafana-bridge/config.yaml` | Panel list, dashboard UIDs (see `deploy/config.yaml.example`) |
| `/etc/default/grafana-bridge` | `GRAFANA_TOKEN`, `GRAFANA_BRIDGE_CONFIG`, etc. |
| `/etc/default/grafana-push` | Push-specific: `WORKER_BEARER_TOKEN`, `X3_PUBKEY_B64`, optional `WORKER_URL` override |

The `/etc/grafana-bridge/` path is kept (rather than renamed to
`grafana-push/`) because it was the install location of an earlier
LAN-only FastAPI bridge that this service replaces; on existing
deployments the panel config is already there and worth preserving.

## Deploy

```sh
# From the dev box:
rsync -a --delete --exclude '.venv' --exclude '__pycache__' \
    bridge-cloud/ pibridge:/tmp/grafana-push-staging/
ssh pibridge 'sudo bash /tmp/grafana-push-staging/deploy/install.sh /tmp/grafana-push-staging'
```

The install script:
- Creates a `grafana-push` system user (read access to `/etc/grafana-bridge/`
  via supplementary group membership on `grafana-bridge`).
- Installs the code to `/opt/grafana-push/` with its own venv.
- Installs `grafana-push.service` + `grafana-push.timer`, enables the timer.
- If an older `grafana-bridge.service` is present on disk, stops + removes
  it (cleanup of a previous LAN-only deployment).

After install, set the secrets:

```sh
sudo sed -i 's/^WORKER_BEARER_TOKEN=.*/WORKER_BEARER_TOKEN=<your-token>/' /etc/default/grafana-push
# X3_PUBKEY_B64 stays blank until the X3 is provisioned (see firmware-cloud/README.md).
# The timer will fire every 4 min and warn-and-skip until you paste it in.

sudo systemctl start grafana-push.service
journalctl -u grafana-push.service -n 20
```

A successful push logs:

```
pushed: plaintext=4096B sealed=4156B etag=... build=2500ms seal=8ms push=300ms
```

## Testing

```sh
cd bridge-cloud
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"
.venv/bin/pytest tests/
```

Tests cover seal/unseal round-trip plus tamper-detection (Poly1305 fail-closed).

## Notes

`config.py`, `data.py`, `scheduler.py`, and `render.py` are a trimmed
copy of a defunct LAN-only FastAPI bridge that this service replaces.
`push.py` is the entry point that's actually invoked. There's no
long-running process and no HTTP listener; the scheduler module is used
purely as a library for its `render_bundle_once()` code path.
