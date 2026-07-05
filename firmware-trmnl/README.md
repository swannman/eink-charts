# firmware-trmnl/ — TRMNL X firmware

Custom firmware for the **TRMNL X** (usetrmnl) — a 10.3" 1872×1404 parallel
e-paper display with genuine **16-level (4-bit) grayscale**, driven by an
**ESP32-S3** (16 MB flash / 8 MB PSRAM) via the **FastEPD** library.

Where the X3 firmware ([`../firmware-cloud/`](../firmware-cloud/README.md))
shows one Grafana *panel* per screen on a 1-bit SPI panel, the TRMNL X cycles
through whole **dashboards**: it renders every panel of a dashboard at its
`gridPos`, scaled to fill the screen, as a native grayscale drawing — no
screenshots, no dithering. It rotates through the dashboards in the Grafana
folder the bridge is pointed at (default `f8jmvq` / "trmnl").

> Pairs with [`../bridge-cloud/`](../bridge-cloud/README.md) (the `push_trmnl`
> service builds + seals the dashboard bundle) and
> [`../worker/`](../worker/README.md) (`/bundle-trmnl` endpoint). Same
> end-to-end X25519 + AES-256-GCM sealing as the X3 — the device holds the
> private key; Cloudflare only ever sees ciphertext.

## What it shows

- **A dashboard per screen**, laid out on the 24-column Grafana grid scaled to
  1872×1404 (square cells, letterboxed). Timeseries panels render as mini line
  charts with axes, gridlines, threshold-shaded bands, and multiple series in
  distinct gray shades; stat panels render as big-number tiles.
- **Grayscale carries status.** Grafana threshold colours are pre-mapped (on the
  bridge) to gray levels: a stat tile shades by the band its value falls in and
  gets a status accent bar; timeseries area thresholds become shaded regions.
- **Auto-rotating slideshow.** The device caches the whole multi-dashboard
  bundle in one Wi-Fi fetch and advances locally every `DWELL_SECONDS`
  (default 45 s), re-fetching over Wi-Fi only every `REFRESH_INTERVAL`
  (default 10 min). A **tap on the touch bar** advances immediately.

## Hardware (from usetrmnl/trmnl-firmware, BOARD_TRMNL_X)

| | |
|---|---|
| MCU | ESP32-S3, 16 MB flash, 8 MB octal PSRAM, native USB-CDC |
| Panel | 10.3" 1872×1404, parallel (i80), 16-level grayscale, driven by FastEPD `BB_PANEL_TRMNL_X` in `BB_MODE_4BPP` |
| Grayscale | vendor 16×9 gray matrix (`src/gray_table.h`) applied via `setCustomMatrix()` — the calibrated waveform for this panel |
| Wi-Fi | S3 native 2.4 GHz (`WiFi.h`); the on-board ESP32-C5 5 GHz modem is **not** used |
| Touch | Azoteq IQS323 @ I²C 0x44, RDY/INT on GPIO3 (deep-sleep wake) |
| Battery | TI BQ27427 fuel gauge @ I²C 0x55 (SDA=39/SCL=40) |

## Build + flash

```sh
cp platformio.local.ini.example platformio.local.ini   # optional: pin the port
cp src/secrets.h.example src/secrets.h                  # Wi-Fi + Worker bearer
$EDITOR src/secrets.h

pio run -e trmnl_x          # compile
pio run -e trmnl_x -t upload
pio device monitor -b 115200
```

The TRMNL X enumerates as `/dev/cu.usbmodem*` on macOS (native USB-CDC — no
UART bridge). If it doesn't appear, wake/dock it first; hold BOOT while plugging
in to force the bootloader.

## First-boot enrollment

1. Flash the firmware to a fresh device.
2. On first boot it generates an X25519 keypair (NVS namespace `trmnlx-crypto`),
   renders a full-screen **QR of the public key** plus the key as text, and
   sleeps.
3. Scan the QR (or copy the `crypto: TRMNL_PUBKEY_B64=…` line from serial) and
   paste it into the bridge:
   ```sh
   sudo sed -i 's/^TRMNL_PUBKEY_B64=.*/TRMNL_PUBKEY_B64=<paste>/' /etc/default/grafana-push
   sudo systemctl start grafana-push-trmnl.service
   ```
4. Tap the touch bar (or wait 24 h) to continue. Subsequent boots fetch and
   render decrypted dashboards.

## Boot flow (`src/main.cpp`)

1. Bring up the sensor I²C bus + FastEPD panel (4bpp, vendor gray matrix).
2. First boot → generate key, show enrollment QR, sleep.
3. Advance the dashboard index (every wake — dwell timer or touch tap).
4. Every ~`REFRESH_INTERVAL` (counted in wakes) → connect Wi-Fi, GET
   `/bundle-trmnl`, decrypt, validate the `0xCFB2` header, cache the plaintext in
   NVS; best-effort PUT battery voltage to `/battery-trmnl`.
5. Render the current dashboard from cache and full-refresh the panel.
6. Deep sleep for `DWELL_SECONDS`, also waking on the touch RDY line.

## Source layout

```
src/
├── main.cpp                 # boot flow, fetch-vs-cache, sleep/wake
├── config.h                 # pins, URLs, dwell/refresh, gray convention
├── display_trmnl.{h,cpp}    # FastEPD lifecycle + vendor gray matrix
├── gray_table.h             # the 144-byte vendor 16-gray waveform
├── gfx4.{h,cpp}             # grayscale text (anti-aliased) + line helpers
├── bundle_parser.{h,cpp}    # 0xCFB2 reader (mirrors data_trmnl.py)
├── dashboard_renderer.{h,cpp}  # fit-to-screen grid layout + panel drawing
├── bundle_seal.{h,cpp}      # X25519 + HKDF + AES-256-GCM decrypt (mbedTLS)
├── x25519_keystore.{h,cpp}  # keypair gen + NVS persistence + b64url
├── enroll_screen.{h,cpp}    # first-boot QR + visible pubkey
├── wifi_config.{h,cpp}      # multi-network loader (native S3 Wi-Fi)
├── touch_iqs323.{h,cpp}     # IQS323 presence probe (tap = ext0 wake)
├── battery_bq27427.{h,cpp}  # fuel-gauge voltage / SoC
└── fonts/                   # vendored Roboto Black BB_FONTs (Apache-2.0)
```

## Previewing layout without hardware

The bridge ships a host renderer that runs the *same* fit-to-screen layout math
and produces 1872×1404 grayscale PNGs, so you can review layout + threshold
grays before flashing:

```sh
cd ../bridge-cloud
GRAFANA_TOKEN=… .venv/bin/python -m grafana_push.push_trmnl --dry-run /tmp/b.bin
.venv/bin/python tools/preview_trmnl.py /tmp/b.bin
```

## Notes / caveats

- **Grayscale is full-refresh only** in 4bpp (FastEPD `partialUpdate` is 1/2bpp
  only). Each dashboard change is a full flashing refresh — fine for a slideshow.
- **Touch** currently wakes on *any* IQS323 RDY assertion and advances. If the
  stock IQS323 mode wakes too often on real hardware, set `ENABLE_TOUCH 0` in
  `secrets.h` for pure timed rotation, or load an event-only config in
  `touch_iqs323.cpp` (see `touch_iqs323.h`).
- **Licensing:** this project is MIT. FastEPD (bitbank2) and the Roboto fonts are
  permissive. The touch/battery drivers here are written clean-room from the
  chips' register maps — not copied from the GPL-3.0 usetrmnl firmware. The
  vendor gray-matrix *values* in `gray_table.h` are calibration data.
```
