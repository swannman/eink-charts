# firmware/ — legacy X3 firmware (Pi-bridge fetch)

ESP32-C3 / Arduino / PlatformIO firmware for the Xteink X3. Fetches
bundles from a local Raspberry Pi bridge over plain HTTP on Wi-Fi.

> Use this with [`../bridge/`](../bridge/README.md). For the work-anywhere
> variant with encryption + multi-WiFi, see
> [`../firmware-cloud/`](../firmware-cloud/README.md).

## When to use this

- You want the X3 on your home network and don't need to take it
  elsewhere.
- You're using `bridge/` (the local FastAPI listener) on a Pi at a
  known IP on your LAN.

If the X3 needs to work outside home Wi-Fi (phone hotspot, etc.), flash
`firmware-cloud/` instead — it pulls from a Cloudflare Worker over any
network and decrypts bundles end-to-end.

## Behavior

- **Timer wake** (every 5/15/60 min depending on view mode) → connect to
  Wi-Fi, fetch the bundle from `http://<bridge>:8080/data/all`, decode
  into NVS, render the current panel, deep sleep.
- **Button wake** → never touches Wi-Fi; serves from the NVS cache.
- **Total awake time** per cycle: ~3-7 s on a successful fetch, ~150 ms
  on a button-only redraw.

## Controls

- **Short press POWER** → next panel
- **Double-click POWER** → cycle view (24h → 2h → 7d → 24h)
- **Long press POWER** → 2-column scrollable panel list (rocker buttons
  move cursor; POWER selects, 30 s inactivity auto-selects)

## Configure + build

```sh
cp firmware/platformio.local.ini.example firmware/platformio.local.ini
# Optional: bake in build-time defaults via -DDEFAULT_WIFI_SSID=...
# (or set them at runtime via NVS keys wifi_ssid / wifi_pass / bridge_url)

# Create firmware/src/secrets.h (gitignored) — see source for what it expects.
cd firmware
pio run -t upload
```

The X3's USB Serial JTAG is finicky during deep sleep, so the easiest
flash flow is: hold the BOOT button while replugging USB, then run upload.

## Bundle handling

- Validates magic (`0xCFB1`) + version (currently `4`). Mismatch = bundle
  is discarded, prior cache kept, error logged on serial.
- Stored as a single `Preferences` blob in NVS namespace `x3-cache`.
- Decoded on every render via `loadCachedScreen()` — no in-RAM state
  between wakes beyond a handful of RTC variables.

## Battery telemetry

On each fetch, the firmware reads the BQ27220 fuel gauge and sends the
voltage as `X-Battery-MV` header. The bridge accumulates a 24 h history
and synthesizes a battery-voltage panel that shows up alongside the real
ones.

This is one of the things the cloud variant doesn't currently do — there's
no return channel from the X3 to the Pi when traffic flows
X3 ← Worker ← Pi.

## Memory + flash

- **Framebuffer**: 792 × 528 / 8 = 52,272 bytes in RAM (single buffer).
- **Bundle cache**: ~3-25 KB in NVS depending on panel count.
- **Total RAM at runtime**: ~85-95 KB on a fetch cycle (WiFi + HTTPClient
  + framebuffer + decoded bundle).
- **Flash**: ~1.1 MB of the 3 MB app partition.

## Submodule: community-sdk

`firmware/lib/community-sdk` is a Git submodule pointing at a personal
fork of `bcrpntr/crosspet-x3` with a small `markRedRamSynced()` patch
that lets us trust the controller's RED RAM contents across deep-sleep
cycles, enabling differential refreshes. The `EInkDisplay` library is
loaded from this submodule via `platformio.ini`'s `symlink://` directive.

`firmware-cloud/` reuses the same submodule via a relative path — there
is only one copy on disk.

## Source layout

```
src/
├── main.cpp                # boot flow, RTC vars, sleep + wake logic
├── config.h                # GPIO pins, button thresholds, constants
├── secrets.h               # gitignored: WIFI_SSID/PASSWORD + DEMO_MODE
├── panel_model.h           # PanelData / StatEntry struct definitions
├── panel_renderer.cpp      # drawPanel / drawStatScreen / drawListView
├── gfx_lite.{h,cpp}        # 1-bit framebuffer + 5x7 font + Adafruit GFX wrap
├── demo_panel_data.h       # built-in stub panels for DEMO_MODE
├── font5x7.h               # fixed-space bitmap font (BSD)
└── imu.{h,cpp}             # QMI8658 probe (currently log-only)
```

## Why two firmwares?

This one came first and is the simplest to understand: HTTP, no crypto,
single Wi-Fi credential. `firmware-cloud/` adds Cloudflare-relay
fetching, multi-WiFi with per-network refresh floors, X25519+AES-GCM
decryption, and a first-boot QR-code enrollment flow. They share the
SSD1677 driver via the submodule but diverge everywhere else.
