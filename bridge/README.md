# bridge/ — local FastAPI listener (legacy / LAN-only)

Original implementation. A FastAPI service that runs on a Raspberry Pi
inside your home network. The X3 (`firmware/`) connects directly to it
over Wi-Fi at port 8080.

> Use this with [`../firmware/`](../firmware/README.md). For the
> work-anywhere variant, see [`../bridge-cloud/`](../bridge-cloud/README.md)
> + [`../firmware-cloud/`](../firmware-cloud/README.md).

## When to use this

- You want the X3 to display Grafana dashboards while staying on your
  home LAN.
- You don't want any external dependency (no Cloudflare account, no
  outbound encryption setup).
- You're fine with the display going dark when away from home Wi-Fi.

If you want the display to work over phone hotspot or any other network,
use `bridge-cloud/` instead — same panel config, just a different data
path.

## Endpoints

- `GET /data/all` — versioned binary bundle of every panel × 3 view
  modes (24h / 2h / 7d). This is what `firmware/` fetches on each wake.
- `GET /frame` — legacy: dithered 792×528 framebuffer (single-panel-at-a-
  time). Predates the bundle path; kept for reference.
- `GET /data` — JSON variant for a single panel, supports `?from=…&to=…`
  override (used by long-press zoom in older firmware).
- `POST /advance` — manually advance the auto-rotating panel pointer
  (legacy; new firmware navigates client-side).
- `GET /healthz` — liveness probe, returns the last render's status.

## How it builds bundles

A background `Scheduler` runs `render_bundle_once()` every 4 minutes:

1. Resolves panels from `config.yaml` — static entries plus any panels
   discovered by traversing the configured dashboard UIDs via Grafana's
   `/api/dashboards/uid/X` endpoint.
2. For each panel × each time window (24h, 2h, 7d), runs the PromQL
   query through Grafana's datasource proxy.
3. Validates the results (data points present, axis bounds sane).
4. Groups stat panels that share `gridPos.y` into side-by-side screens
   (up to 3 across).
5. Encodes the whole thing into a compact binary bundle (~3-20 KB
   depending on panel count) with magic `0xCFB1`, version 4.
6. Caches in memory; served instantly on each X3 request.

A synthetic Battery (V) panel is appended, built from BQ27220 voltage
readings the X3 piggy-backs on each fetch via the `X-Battery-MV` header.

## Config

| Path | Purpose |
|---|---|
| `/etc/grafana-bridge/config.yaml` | Panel list, dashboard UIDs, defaults — see `deploy/config.yaml.example`. |
| `/etc/default/grafana-bridge` | Environment file: `GRAFANA_TOKEN`, `LOG_LEVEL`, etc. See `deploy/grafana-bridge.env.example`. |

## Deploy

```sh
# From the dev box:
rsync -a --delete --exclude '.venv' --exclude '__pycache__' \
    bridge/ pibridge:/tmp/grafana-bridge-staging/
ssh pibridge 'sudo bash /tmp/grafana-bridge-staging/deploy/install.sh /tmp/grafana-bridge-staging'

# On the Pi: edit config + env, then restart:
sudo systemctl restart grafana-bridge
curl -s http://localhost:8080/healthz | jq
```

Runs as the dedicated `grafana-bridge` systemd user. The service is
enabled by default after install.

## Testing

```sh
cd bridge
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"
.venv/bin/pytest tests/
```

Tests cover the PNG-to-framebuffer dithering pipeline; bundle encoding
is exercised end-to-end by the running service against real Grafana.

## Bundle format

Documented inline in `src/grafana_bridge/data.py`. Header is 8 bytes:

```
[2B magic 0xCFB1][1B version][1B screen_count][4B next_poll_secs]
[N × 12B screen offsets: u32 24h, u32 2h, u32 7d]
[N × screen blocks]
```

Each screen block starts with `[type, entry_count]` and contains either a
chart entry or 1-3 stat entries. The format is versioned so the firmware
fails gracefully on schema mismatch — bump `BUNDLE_VERSION` if you change
the layout.

## Why two architectures?

This implementation predates the Cloudflare-relay setup. It's simpler and
needs zero external services, but it ties the display to your LAN.
`bridge-cloud/` adds end-to-end encryption and outbound-only push so the
display works on any network — at the cost of more moving pieces. Both
share the panel config schema and the bundle format.
