# einkcharts-dashboard Worker

Cloudflare Worker that ferries encrypted dashboard bundles from the Pi
(push) to the X3 (pull). Bound to R2 bucket `einkcharts-bundle`.

## Endpoints

- `PUT /bundle` — Pi pushes a sealed bundle. Bearer-token-gated.
- `GET /bundle` — X3 fetches the latest bundle. Supports `If-None-Match`
  for 304s. Bearer-token-gated.

Payload is X25519+ChaCha20-Poly1305 sealed by the Pi before upload; the
Worker only sees ciphertext.

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
| `CLOUDFLARE_API_TOKEN` | Token with Workers Scripts:Edit + DNS:Edit + Workers Routes:Edit |
| `CLOUDFLARE_ACCOUNT_ID` | `e586791371224fd8a71725fce7f6f1a6` |

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
```
