# eink-charts

Custom firmware + bridge service that turns an Xteink X3 3.68" e-paper
display (ESP32-C3, 792×528, 1-bit, SSD1677) into a Grafana Cloud dashboard
viewer.

The X3 wakes on a timer (or button press), fetches a pre-rendered bundle
of chart data over Wi-Fi, draws the charts natively on the EPD, and goes
back to deep sleep. Charts render in vector form on-device — no PNG
decoding, no dithering — so they're crisp, the payload is tiny, and
battery life is measured in weeks.

## What it looks like

- Full-screen line charts with title, axes, gridlines, and dotted fill under
  the line
- Side-by-side stat panels with big numbers, units, and sparklines
  (auto-grouped from Grafana stat panels that share a row)
- Long-press the power button → 2-column scrollable panel list (rocker
  buttons move the cursor)
- Double-click → cycle time window (24h ↔ 2h ↔ 7d). Per-panel default view
  is read from Grafana's `timeFrom` override. The refresh cadence follows
  the time window: 2h view refreshes every 5 min, 24h every 15 min, 7d
  every hour — finer windows want fresher data.
- Quiet hours (22:00–06:00 local) suppress background Wi-Fi refreshes

## Two architectures

There are two complete implementations in this repo. They share the bundle
format and rendering, but the data path is different. Pick whichever fits
how you want to use the display.

| | **Legacy (local)** | **Cloud (anywhere)** |
|---|---|---|
| Data path | X3 ← home WiFi ← Pi bridge | X3 ← any WiFi ← Cloudflare Worker ← Pi push |
| Works away from home | No | Yes — including phone hotspot |
| Inbound exposure on home net | Port 8080 to the Pi | None (push-only outbound) |
| Encryption | None (LAN) | X25519 + AES-256-GCM end-to-end |
| Multi-WiFi support | No | Yes, with per-network refresh floors |
| Firmware | [`firmware/`](firmware/README.md) | [`firmware-cloud/`](firmware-cloud/README.md) |
| Server | [`bridge/`](bridge/README.md) | [`bridge-cloud/`](bridge-cloud/README.md) + [`worker/`](worker/README.md) |

Each subdirectory has its own README with build, deploy, and protocol
details. The two architectures are mutually exclusive on a given Pi
(both want to query Grafana on overlapping schedules); installing one
disables the other.

## Subdirectories

- **[`firmware/`](firmware/README.md)** — ESP32-C3 firmware for the X3 that
  fetches from a local Pi bridge. Pair with `bridge/`.
- **[`firmware-cloud/`](firmware-cloud/README.md)** — ESP32-C3 firmware that
  fetches encrypted bundles from a Cloudflare Worker. Multi-WiFi-aware,
  first-boot QR-code enrollment, public-key crypto. Pair with `bridge-cloud/`
  + `worker/`.
- **[`bridge/`](bridge/README.md)** — original FastAPI service. Runs on
  the Pi, serves the bundle at `GET /data/all` on port 8080.
- **[`bridge-cloud/`](bridge-cloud/README.md)** — push-only service. Builds
  the same bundle in-process, X25519-seals it for the X3, uploads to the
  Cloudflare Worker every 4 minutes via systemd timer.
- **[`worker/`](worker/README.md)** — Cloudflare Worker (TypeScript) that
  proxies the encrypted bundle between R2 storage and clients. Auth via
  bearer token; never sees plaintext.

## References

Everything I read while designing this. None are git-vendored — they're
research material only.

- **[bcrpntr/crosspet-x3](https://github.com/bcrpntr/crosspet-x3)** (MIT) —
  The most complete community firmware for the X3. The eink display driver
  in `firmware/lib/community-sdk` is bcrpntr's, included as a submodule.
  Source-of-truth for the SSD1677 init/LUTs, I²C pin map (BQ27220 + QMI8658
  on SCL=GPIO0/SDA=GPIO20), and how to keep the battery MOSFET held HIGH
  through deep sleep.
- **[CrazyCoder gist: X3 hardware analysis](https://gist.github.com/CrazyCoder/82fec0bbd0e515dcc237d3db7451ec6f)** —
  Reverse-engineered display command sequence + LUT details.
- **[CrazyCoder gist: X3 firmware analysis](https://gist.github.com/CrazyCoder/1c5f846adee18e21f91e264601a6ddce)** —
  Button GPIO mapping (resistor-ladder ADC on GPIO 1/2, power on GPIO 3),
  QMI8658 IMU registers, battery calculation, INT-pin speculation.
- **[bigbag/papyrix-reader](https://github.com/bigbag/papyrix-reader)** —
  Independent X3 spec doc and `InputManager` source confirming the ADC
  thresholds and that the IMU INT pin is not wired to a GPIO on this board
  (no hardware wake-on-motion path).
- **[usetrmnl/trmnl-firmware](https://github.com/usetrmnl/trmnl-firmware)** —
  TRMNL's X4 firmware. Same C3 silicon, same battery-MOSFET-hold pattern;
  borrowed the `gpio_hold_en` + `gpio_deep_sleep_hold_en` sequence that
  prevents the brownout-reboot loop after deep sleep.
- **QMI8658 datasheet** — for IMU register addresses (CTRL1=0x40 auto-
  increment, CTRL2 FS/ODR encoding, AX_L at 0x35).
- **Grafana HTTP API docs** — `/api/dashboards/uid/<uid>`,
  `/api/datasources/proxy/uid/<uid>/api/v1/query_range`, plus
  `fieldConfig.defaults` for units / min / max / timeFrom overrides.

## Hardware notes

- USB Serial JTAG (HWCDC) on the C3 won't enumerate while the chip is in
  deep sleep, so serial capture across a sleep cycle requires polling for
  the port to reappear after a wake.
- NVS partition is sized at 80 KB so the bundle (~15–25 KB depending on
  panel count) fits comfortably. The default ESP32 partition table is too
  small.
- The QMI8658 INT pin is not connected to a GPIO on this board (verified
  across three independent firmware codebases). Wake-on-motion via the
  IMU is not possible without a PCB mod; we use timer + power-button wake.
- Battery: BQ27220 fuel gauge over I²C. Voltage at register `0x08`. The
  local variant piggybacks the reading on every fetch via an
  `X-Battery-MV` header; the cloud variant uses a separate `/battery`
  Worker endpoint that the X3 PUTs to after each successful bundle fetch
  (Pi reads it back to synthesize the same panel). Both produce a
  synthetic Battery (V) panel.

## License

MIT — see [LICENSE](LICENSE).
