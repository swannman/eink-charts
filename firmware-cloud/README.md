# firmware-cloud/ — X3 firmware

ESP32-C3 firmware for the Xteink X3 e-paper display. Fetches encrypted
dashboard bundles from a public Cloudflare Worker over Wi-Fi when any
configured network is in range; falls back to BLE (to the iOS companion
app) when no Wi-Fi is reachable.

> Pairs with [`../bridge-cloud/`](../bridge-cloud/README.md) (Pi push
> service) + [`../worker/`](../worker/README.md) (Cloudflare Worker) +
> [`../ios-app/`](../ios-app/README.md) (BLE fallback transport).

## Highlights

- **HTTPS** fetch from Cloudflare with bearer-token auth.
- **End-to-end** X25519 + AES-256-GCM decryption of the bundle body.
- **Multi-WiFi** credentials, tried in priority order, each with its
  own minimum refresh interval (e.g. 5 min on home, 1 h on phone).
- **First-boot enrollment** screen rendering a QR code of the device's
  X25519 public key so you can paste it into the Pi push config.
- **BLE peripheral fallback** when no configured WiFi is in range: the
  X3 advertises a custom GATT service; the iOS companion app fetches the
  sealed bundle from the Worker and pushes the bytes over BLE. The X3
  decrypts the same way regardless of which transport delivered the
  ciphertext. See [BLE fallback](#ble-fallback) below.
- **Battery telemetry over a separate Worker endpoint**: after each
  successful fetch, the X3 PUTs the BQ27220 voltage to `/battery` so the
  Pi reads it back to synthesize a battery panel. See
  [Battery telemetry](#battery-telemetry) below.
- **Post-flash QR re-display**: on first boot after every firmware flash,
  the device renders the enrollment QR for 30 s (skippable by pressing
  power) so you can re-scan or re-copy the pubkey without wiping the
  keystore.
- **Device Logs panel** (long-press list only): an RTC-backed ring buffer
  mirrors every `Log.printf` output across deep-sleep cycles, rendered as
  a scrollable on-screen log. See [Device Logs](#device-logs) below.
- **Proportional fonts** (Adafruit GFX FreeSans / FreeSansBold) for the
  connecting splash, enrollment QR screen, panel titles, and logs. The
  fixed-width 5x7 face is reserved for chart axis labels.
- **Removed**: the local `/advance` POST and the `bridge_url` NVS key —
  neither applies to the cloud flow.

## Controls

- **Short-press POWER** → next panel (forward through the bundle's panels,
  wrapping at the end; never lands on the logs screen).
- **Double-click POWER** (≤250 ms between releases) → cycle the time
  window: 24h ↔ 2h ↔ 7d.
- **Long-press POWER** (≥800 ms) → enter the 2-column panel list. Use
  the rocker buttons to move the cursor, POWER to select, 30 s of
  inactivity auto-selects. The list ends with a synthesized **Device
  Logs** entry.
- **On the Device Logs screen**: lower rocker (BACK or OK) pages back to
  older logs; upper rocker (UP or DOWN) pages forward to newer; each
  press scrolls ~1/3 of the visible window so 2/3 of the prior content
  stays on screen. POWER exits to deep sleep (position preserved). 30 s
  of inactivity also exits.

## First-boot enrollment flow

1. Flash firmware-cloud to a fresh X3.
2. On first boot the firmware:
   - Generates an X25519 keypair via mbedTLS.
   - Persists it in NVS namespace `x3-crypto` (separate from the bundle
     cache, so firmware-update cache wipes don't kill the keypair).
   - Renders a full-screen QR code containing the base64url public key,
     plus the same key as visible text (a fallback for typing manually).
   - Sleeps for 24 h (or until a power-button wake).
3. Scan the QR with the iOS Camera. Tap the yellow banner → **Copy**.
4. SSH to the Pi and paste:
   ```sh
   sudo sed -i 's/^X3_PUBKEY_B64=.*/X3_PUBKEY_B64=<paste>/' /etc/default/grafana-push
   sudo systemctl start grafana-push.service  # to skip the timer wait
   ```
5. Press the X3's power button. Subsequent boots load the keypair from
   NVS and fetch decrypted bundles from the Worker.

The base64url pubkey is also printed to the serial console on every
boot as a fallback — `pio device monitor` and look for the line
`crypto: X3_PUBKEY_B64=...`.

## Multi-WiFi with per-network refresh floors

Up to **4 networks** in priority order. Each carries a
`min_refresh_sec` floor: the minimum time between fetches on that
network. Defaults:

- **Home WiFi**: floor 300 s (5 min) — matches the Pi's push cadence.
- **Phone hotspot**: floor 3600 s (1 h) — protects cellular data.

Configure via either:

- **Compile time** in `src/secrets.h`:
  ```c
  #define DEFAULT_WIFI_SSID_0     "HomeWiFi"
  #define DEFAULT_WIFI_PASSWORD_0 "..."
  #define DEFAULT_WIFI_MIN_0      300

  #define DEFAULT_WIFI_SSID_1     "MyHotspot"
  #define DEFAULT_WIFI_PASSWORD_1 "..."
  #define DEFAULT_WIFI_MIN_1      3600
  ```
- **Runtime** via NVS keys `wifi_ssid_<i>`, `wifi_pass_<i>`,
  `wifi_min_<i>` (use the `Preferences` API or a tiny provisioning sketch).

### Scheduling semantics

Wake cadence follows the current view mode: 5 min for 2h zoom, 15 min for
24h, 60 min for 7d. The per-network floor only decides whether to
**fetch** this cycle:

| You're on… | View | Wake every | Fetch? |
|---|---|---|---|
| Home WiFi (300 s floor) | 2h zoom (5 min interval) | 5 min | Every wake |
| Phone hotspot (1 h floor) | 2h zoom | 5 min | Once an hour (other wakes connect → check floor → skip) |
| Phone hotspot, then home reappears | 2h zoom | 5 min | Immediately on the next wake after home is back |

The "wake even on phone to check for home" behavior costs a few seconds
of radio per cycle but means you don't have to wait a stale 1 h sleep
to detect home WiFi coming back.

## Device Logs

A 4 KB ring buffer in RTC slow memory captures every `Log.printf` output
(the same lines that go out the USB serial port). Survives deep sleep
between wakes; lost on a full power-cycle — exactly the scope you want
for "what's happened since the device last cold-booted."

Reached via long-press → bottom of the list → "Device Logs". Interactive:
- Lower rocker → page back (older).
- Upper rocker → page forward (newer).
- POWER → exit, deep sleep, scroll position preserved.

Useful for diagnosing issues away from a serial monitor — e.g., did the
X3 actually fall back to phone hotspot, did the fetch decrypt cleanly,
how long did the cycle take.

## BLE fallback

When `wifi_config::connect_any()` returns no association (away from any
configured network, or all configured APs unreachable), the firmware
tears down WiFi and brings up a BLE peripheral for up to 30 s. The X3
never initiates network IO over BLE — it's strictly a transport that
the iOS companion app drives.

GATT layout:
- Service `0e1c0a9c-1bb1-4f1e-8e26-1c3c5a3e9c7f`
- Bundle characteristic `…9c80` (write-with-response, max 512 bytes)
- Battery characteristic `…9c81` (read, 2-byte LE u16 millivolts)

The battery characteristic carries the current BQ27220 reading, snapshotted
before advertising starts. Value of 0 means "no reading available"
(USB-powered, missing battery, dead gauge). See the description of the
post-bundle grace window above for how the iOS app reads + forwards it.

Wire format on the characteristic:
- **First write**: 4-byte little-endian u32 total length, then up to 508
  bytes of the sealed bundle.
- **Subsequent writes**: up to 512 bytes of bundle bytes each, in order.

The 512-byte ceiling is the GATT spec maximum attribute length
(`BLE_ATT_ATTR_MAX_LEN`). For an 18 KB bundle, that's ~37 round trips;
even on a 7.5 ms connection interval with one ack per write, the whole
hand-off finishes inside a couple of seconds.

The X3 accumulates into a single 32 KB heap buffer and treats the
result identically to a Wi-Fi response — same decrypt path, same NVS
cache, same render.

Once the bundle is complete the X3 holds BLE alive for up to 5 s so
the iOS central can read the battery characteristic (`…9c81`, u16 LE
mV) and forward it to the Worker's `/battery` endpoint. The X3 has no
internet during a BLE cycle, so without this hop the synthetic battery
panel would gap. The grace window exits early as soon as the central
disconnects.

NimBLE notes for future debugging:
- Uses `h2zero/NimBLE-Arduino@^2.3.0`. NimBLE-Arduino 1.x cannot be used
  with Arduino-ESP32 3.x / IDF 5.x — the IDF already ships a NimBLE host,
  and 1.x double-links its own, leaving NULL function pointers in the
  controller→host vtable (manifests as a guru meditation at PC=0 inside
  `btdm_controller_init`).
- `NimBLEDevice::deinit(/*clearAll=*/false)` is intentional: 2.5.0's
  `deinit(true)` triggers a `heap_caps_free` assert on a static buffer.
  The chip enters deep sleep right after, which wipes RAM, so any leak
  is academic.

## Battery telemetry

After each successful bundle fetch (while WiFi is still up), the firmware
reads the BQ27220 voltage and PUTs `{"mv": <reading>}` to
`https://dashboard.contexa.net/battery` with the same bearer token. The
Worker appends to a rolling 7-day JSON history in R2; the Pi reads that
history before each push and seeds it into the bundle so the
synthetic **Battery (V)** panel renders. The PUT is best-effort — if it fails (network blip, Worker
hiccup) the bundle cycle still succeeds; we just miss one data point.

## Decryption (`src/bundle_seal.cpp`)

Mirrors the Pi-side `bridge_cloud/.../push.py:seal()`:

1. Parse `[epk:32][nonce:12][ciphertext][tag:16]` from the HTTPS response
   body (60-byte overhead total).
2. `shared = X25519(x3_sk, epk)` via `mbedtls_ecp_mul`.
3. `key = HKDF-SHA256(shared, salt=epk||x3_pk, info="EInkCharts seal v1")`
   — 32-byte symmetric key.
4. `plaintext = AES-256-GCM-decrypt(key, nonce, ciphertext, tag)`.
   ESP32-C3 has hardware AES, so this runs in single-digit milliseconds.

Tag mismatch fails closed. The most common cause is a wrong
`X3_PUBKEY_B64` on the Pi (it's sealing for someone else); look for
`bundle_seal: gcm_decrypt err=-0x...` in the serial log.

## Configure + build

```sh
cp firmware-cloud/platformio.local.ini.example firmware-cloud/platformio.local.ini

# Create firmware-cloud/src/secrets.h (gitignored):
cat > firmware-cloud/src/secrets.h <<'EOF'
#pragma once
#define DEFAULT_WIFI_SSID_0     "HomeWiFi"
#define DEFAULT_WIFI_PASSWORD_0 "..."
#define DEFAULT_WIFI_MIN_0      300

#define DEFAULT_WIFI_SSID_1     "MyHotspot"
#define DEFAULT_WIFI_PASSWORD_1 "..."
#define DEFAULT_WIFI_MIN_1      3600

#define DEFAULT_WORKER_BEARER   "<your-worker-bearer-token>"
#define DEMO_MODE 0
EOF

cd firmware-cloud
pio run -t upload
```

Same BOOT-button-while-plugging-in trick applies for flashing during
deep sleep cycles.

## Boot flow

1. Restore battery-rail MOSFET (else the chip browns out after sleep).
2. Read wake cause + button press pattern (forward / zoom / list / none).
3. Display init. If the controller's RED RAM was populated on the prior
   cycle, trust it and request a differential refresh.
4. **Enrollment check**: if `x3-crypto` NVS namespace doesn't have a
   keypair, generate one, render the enrollment QR + visible-text
   screen, and deep-sleep. Otherwise load `(x3_sk, x3_pk)` for later
   use and print the pubkey to serial.
5. Decide whether to fetch this cycle (`elapsed >= currentRefreshInterval()`).
6. If fetching: try each WiFi network in priority order until one
   connects. If the connected network's floor is met, HTTPS GET the
   Worker, decrypt, validate header, store plaintext in NVS cache.
7. If WiFi found no networks, fall back to the BLE peripheral and wait
   up to 30 s for the iOS app to push a chunked bundle. Same decrypt +
   cache path on success.
8. Render the current screen from the NVS cache (chart, stat group, or
   list view if long-press was held).
9. Deep sleep for `currentRefreshInterval()` (or until 06:00 if entering
   quiet hours).

## Source layout

```
src/
├── main.cpp                  # boot flow, sleep + wake, fetch orchestration
├── config.h                  # GPIO pins, default URLs, button thresholds
├── secrets.h                 # gitignored: WIFI_*, WORKER_BEARER, DEMO_MODE
├── bundle_seal.{h,cpp}       # X25519 ECDH + HKDF + AES-256-GCM decrypt
├── x25519_keystore.{h,cpp}   # mbedTLS keygen + NVS persistence + b64url
├── enroll_screen.{h,cpp}     # first-boot QR + visible-text screen
├── wifi_config.{h,cpp}       # multi-network loader + connect_any()
├── ble_bundle_receiver.{h,cpp}  # NimBLE peripheral, chunked write accumulator
├── log_buffer.{h,cpp}        # RTC ring buffer + MirrorPrint subclass
├── panel_model.h             # PanelData / StatEntry / drawLogsScreen
├── panel_renderer.cpp        # drawPanel / drawStatScreen / drawListView / drawLogsScreen
├── gfx_lite.{h,cpp}          # 1-bit framebuffer + 5x7 + Adafruit GFX
├── demo_panel_data.h         # DEMO_MODE stub panels
├── font5x7.h                 # bitmap font, used only for chart axis labels
└── imu.{h,cpp}               # QMI8658 probe (log-only)
```

## Submodule

`firmware-cloud/lib/community-sdk` is a git submodule pointing at a
personal fork of `bcrpntr/crosspet-x3` with a small `markRedRamSynced()`
patch that lets us trust the controller's RED RAM contents across
deep-sleep cycles. `platformio.ini` references it via:

```ini
EInkDisplay=symlink://lib/community-sdk/libs/display/EInkDisplay
```

After cloning the repo, run
`git submodule update --init firmware-cloud/lib/community-sdk` before
building.

## Memory + flash

- **Framebuffer**: 792 × 528 / 8 = 52,272 bytes in RAM.
- **Flash**: ~1.4 MB / 3 MB. NimBLE is the biggest single contributor
  (~220 KB) on top of the crypto + multi-WiFi additions.
- **RAM at runtime**: ~98 KB peak during a fetch (TLS + HTTPClient +
  sealed buf + plaintext buf + framebuffer). About 30 % of the C3's
  327 KB.
