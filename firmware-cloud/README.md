# firmware-cloud/ — X3 firmware for cloud-relay mode

Forked from `firmware/` to support fetching encrypted dashboard bundles
from a public Cloudflare Worker. Works from any Wi-Fi with internet
access — home, phone hotspot, friend's WiFi.

> Use this with [`../bridge-cloud/`](../bridge-cloud/README.md) and
> [`../worker/`](../worker/README.md). For the home-LAN-only variant
> (simpler, no encryption), see [`../firmware/`](../firmware/README.md).

## What's different from firmware/

- **HTTPS** fetch from Cloudflare with bearer-token auth.
- **End-to-end** X25519 + AES-256-GCM decryption of the bundle body.
- **Multi-WiFi** credentials, tried in priority order, each with its
  own minimum refresh interval (e.g. 5 min on home, 1 h on phone).
- **First-boot enrollment** screen rendering a QR code of the device's
  X25519 public key so you can paste it into the Pi push config.
- **Battery telemetry over a separate Worker endpoint**: after each
  successful fetch, the X3 PUTs the BQ27220 voltage to `/battery` so the
  Pi can read it back and synthesize the same battery panel the legacy
  bridge did. See [Battery telemetry](#battery-telemetry) below.
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

## Battery telemetry

After each successful bundle fetch (while WiFi is still up), the firmware
reads the BQ27220 voltage and PUTs `{"mv": <reading>}` to
`https://dashboard.contexa.net/battery` with the same bearer token. The
Worker appends to a rolling 7-day JSON history in R2; the Pi reads that
history before each push and seeds it into the bundle so the
synthetic **Battery (V)** panel renders identically to the legacy
firmware. The PUT is best-effort — if it fails (network blip, Worker
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
7. Render the current screen from the NVS cache (chart, stat group, or
   list view if long-press was held).
8. Deep sleep for `currentRefreshInterval()` (or until 06:00 if entering
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
├── log_buffer.{h,cpp}        # RTC ring buffer + MirrorPrint subclass
├── panel_model.h             # PanelData / StatEntry / drawLogsScreen
├── panel_renderer.cpp        # drawPanel / drawStatScreen / drawListView / drawLogsScreen
├── gfx_lite.{h,cpp}          # 1-bit framebuffer + 5x7 + Adafruit GFX
├── demo_panel_data.h         # DEMO_MODE stub panels
├── font5x7.h                 # bitmap font, used only for chart axis labels
└── imu.{h,cpp}               # QMI8658 probe (log-only)
```

## Submodule

`firmware-cloud` does **not** have its own `lib/community-sdk` directory.
Instead `platformio.ini` points at the sibling firmware's copy:

```ini
EInkDisplay=symlink://../firmware/lib/community-sdk/libs/display/EInkDisplay
```

This keeps one source of truth for the SSD1677 driver across both
firmwares. If you only want to clone this one variant, you'll still
need the submodule (`git submodule update --init firmware/lib/community-sdk`).

## Memory + flash

- **Framebuffer**: 792 × 528 / 8 = 52,272 bytes in RAM.
- **Flash**: ~1.2 MB / 3 MB. The crypto + multi-WiFi additions cost
  about 50 KB over `firmware/`.
- **RAM at runtime**: ~98 KB peak during a fetch (TLS + HTTPClient +
  sealed buf + plaintext buf + framebuffer). About 30 % of the C3's
  327 KB.
