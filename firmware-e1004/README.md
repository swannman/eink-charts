# firmware-e1004/ — reTerminal E1004 firmware

Custom firmware for the **Seeed reTerminal E1004** — a 13.3" 1200×1600
**E Ink Spectra 6 full-color** panel driven by an **ESP32-S3** (32 MB flash /
8 MB OPI PSRAM) via Seeed_GFX's EPaper driver (T133A01, `BOARD_SCREEN_COMBO
523`).

Works like [`../firmware-trmnl/`](../firmware-trmnl/README.md) (the TRMNL X):
it cycles through whole Grafana **dashboards**, rendering every panel natively
at its `gridPos` — no screenshots. The difference is **color**: the bundle is
the version-3 "color bundle" whose shade bytes carry Spectra 6 color codes, so
threshold state shows as real **red / yellow / green**:

- **Timeseries threshold bands** fill in dithered light red/yellow (in-range /
  green zones stay clean white).
- **Stat tiles** tint by the active threshold color (a pale dithered wash) —
  green when healthy, yellow/red when not. No borders or accent bars; the
  tint alone carries the status.
- **Series lines** draw black first, then blue/red/green for extra series.

> Pairs with [`../bridge-cloud/`](../bridge-cloud/README.md) (`push_e1004`
> builds + seals the color bundle) and [`../worker/`](../worker/README.md)
> (`/bundle-e1004` endpoints). Same end-to-end X25519 + AES-256-GCM sealing —
> the device holds the private key; Cloudflare only ever sees ciphertext.

## Hardware

| | |
|---|---|
| MCU | ESP32-S3, 32 MB quad flash, 8 MB octal PSRAM |
| Panel | 13.3" 1200×1600 E Ink Spectra 6 (black/white/red/yellow/green/blue), SPI, dual-controller (T133A01), full refresh ~25 s |
| USB | CH340 UART bridge → `/dev/cu.wchusbserial*` on macOS (no native CDC) |
| Wi-Fi | S3 native 2.4 GHz |
| Battery | 5000 mAh LiPo, ADC voltage sense (no fuel gauge) |
| Buttons | front buttons; the green one advances the slideshow (ext0 deep-sleep wake) |

## Build + flash

```sh
cp platformio.local.ini.example platformio.local.ini   # pin the port
cp src/secrets.h.example src/secrets.h                  # Wi-Fi + Worker bearer
$EDITOR src/secrets.h

pio run -e e1004            # compile
pio run -e e1004 -t upload
pio device monitor -b 115200
```

## First-boot enrollment

1. Flash the firmware to a fresh device.
2. On first boot it generates an X25519 keypair (NVS namespace `e1004-crypto`),
   renders a full-screen **QR of the public key** plus the key as text, and
   sleeps.
3. Scan the QR (or copy the `crypto: E1004_PUBKEY_B64=…` line from serial) and
   paste it into the bridge:
   ```sh
   sudo sed -i 's/^E1004_PUBKEY_B64=.*/E1004_PUBKEY_B64=<paste>/' /etc/default/grafana-push
   sudo systemctl start grafana-push-e1004.service
   ```
4. Press the green button (or wait 24 h) to continue. Subsequent boots fetch
   and render decrypted color dashboards.

## Boot flow (`src/main.cpp`)

1. Allocate the landscape framebuffer (PSRAM) + bring up the EPaper sprite.
2. First boot → generate key, show enrollment QR, sleep.
3. Advance the dashboard index (every wake — dwell timer or green button).
4. Every ~`REFRESH_INTERVAL` (counted in wakes) → connect Wi-Fi, read
   `/manifest-e1004`, GET only the changed `/bundle-e1004?d=<i>` objects,
   decrypt, cache to LittleFS; best-effort PUT battery voltage to
   `/battery-e1004`.
5. Render the current dashboard from cache and full-refresh the panel (~25 s).
6. Deep sleep for `DWELL_SECONDS`, also waking on the green button.

## Source layout

```
src/
├── main.cpp                 # boot flow, fetch-vs-cache, sleep/wake
├── config.h                 # pins, URLs, dwell/refresh, Spectra palette codes
├── display_e1004.{h,cpp}    # Seeed_GFX EPaper lifecycle + fb->sprite packing
├── framebuffer.{h,cpp}      # landscape 1600x1200 indexed-color fb (PSRAM)
├── gfxc.{h,cpp}             # solid-color text (FreeSans GFX fonts) + fitting
├── dashboard_renderer.{h,cpp}  # fit-to-screen grid layout + color panels
├── bundle_parser.{h,cpp}    # 0xCFB2 v3 reader (mirrors data_trmnl.py)
├── bundle_seal.{h,cpp}      # X25519 + HKDF + AES-256-GCM decrypt (mbedTLS)
├── x25519_keystore.{h,cpp}  # keypair gen + NVS persistence + b64url
├── enroll_screen.{h,cpp}    # first-boot QR + visible pubkey
├── wifi_config.{h,cpp}      # multi-network loader
├── battery_e1004.{h,cpp}    # ADC pack voltage + curve-fit SOC
└── serial_console.{h,cpp}   # dev console: color fb dumps, next/prev, battery
```

## Previewing layout without hardware

```sh
cd ../bridge-cloud
GRAFANA_TOKEN=… .venv/bin/python -m grafana_push.push_e1004 --dry-run /tmp/b.bin
.venv/bin/python tools/preview_e1004.py /tmp/b.bin
```

## Notes / caveats

- **Full refresh only.** Spectra 6 color refreshes take ~25 s and flash through
  the color sequence — fine for a 45-minute slideshow cadence, not for
  interactivity. The green button advance therefore takes ~30 s to complete.
- **No grayscale.** The panel has 6 solid colors; "light tints" are ordered
  4×4 dithers (`fb::fillRectDither`), and text is solid ink.
- **Colors match the bridge**: the `SPECTRA_*` codes in
  `bridge-cloud/.../data_trmnl.py`, `config.h COL_*`, and the
  `display_e1004.cpp PAL2EPD` table must stay aligned.
