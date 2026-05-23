# einkcharts-dashboard Worker

Cloudflare Worker that ferries encrypted dashboard bundles from the Pi
(push) to the X3 (pull), plus a tiny battery-telemetry side channel back
from the X3. Bound to R2 bucket `einkcharts-bundle`.

## Endpoints

**`/bundle`** — the main payload.

- `PUT /bundle` — Pi pushes a sealed bundle (binary).
- `GET /bundle` — X3 fetches the latest bundle directly when on Wi-Fi;
  the iOS companion app fetches it on the X3's behalf during the BLE
  fallback path. Supports `If-None-Match` for 304s.

The bundle body is X25519 + AES-256-GCM sealed by the Pi before upload;
the Worker only sees ciphertext, never holds the X3's private key, and
can't decrypt.

**`/battery`** — small JSON telemetry channel.

- `PUT /battery` — X3 posts `{"mv": <int>}` after each successful Wi-Fi
  fetch; the iOS companion app posts on the X3's behalf after each
  successful BLE relay. The Worker appends `{ts, mv}` to a JSON array
  stored at R2 key `battery_history` and prunes entries older than 7 days.
- `GET /battery` — Pi reads the array before each bundle build and seeds
  it into the synthetic battery panel. Returns `[]` if empty.

Battery readings are plaintext voltages (no privacy concern) but still
bearer-token-gated to keep randos out.

All endpoints require `Authorization: Bearer <BEARER_TOKEN>`.

## Deploy

CI/CD runs automatically on push to `main` when `worker/**` changes.
See `.github/workflows/deploy-worker.yml`.

Manual deploy:

```sh
cd worker
npx wrangler deploy
```

## Required GitHub repo secrets

| Secret | Value |
|---|---|
| `CLOUDFLARE_API_TOKEN` | Token with Workers Scripts:Edit + Workers R2 Storage:Edit + DNS:Edit + Workers Routes:Edit |
| `CLOUDFLARE_ACCOUNT_ID` | Cloudflare Dashboard → right sidebar → Account ID |

The `BEARER_TOKEN` Worker secret is provisioned separately (see below)
and is intentionally NOT in CI/CD — that way GitHub never sees it. CI
deploys preserve existing Worker secrets.

## Provisioning the bearer token (one-time)

```sh
BEARER=$(python3 -c "import secrets, base64; print(base64.urlsafe_b64encode(secrets.token_bytes(32)).rstrip(b'=').decode())")
npx wrangler secret put BEARER_TOKEN
# paste $BEARER when prompted
```

Then copy that same `$BEARER` into the Pi push config and the X3 firmware
secrets header.

## Verifying

```sh
# 401 — no auth
curl -i https://dashboard.contexa.net/bundle

# 404 — no bundle uploaded yet
curl -i -H "Authorization: Bearer $BEARER" https://dashboard.contexa.net/bundle

# upload + read back
echo -n "test" | curl -X PUT -H "Authorization: Bearer $BEARER" --data-binary @- https://dashboard.contexa.net/bundle
curl -i -H "Authorization: Bearer $BEARER" https://dashboard.contexa.net/bundle

# battery telemetry
curl -X PUT -H "Authorization: Bearer $BEARER" -H 'Content-Type: application/json' \
     -d '{"mv":4123}' https://dashboard.contexa.net/battery
curl -i -H "Authorization: Bearer $BEARER" https://dashboard.contexa.net/battery
```
