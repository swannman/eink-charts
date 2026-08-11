# eink-charts

Custom firmware + a push service that turn battery-powered e-paper displays
into Grafana Cloud dashboard viewers. Three displays are supported today: a
3.68" 1-bit mono panel, a 10.3" 16-level grayscale panel, and a 13.3" full-color
E Ink Spectra 6 panel.

Each device wakes on a timer (or a button / touch), fetches a pre-rendered
bundle of chart data over HTTPS from a Cloudflare Worker — or, on the X3, over
BLE from an iOS companion app when no Wi-Fi is reachable — decrypts it locally
with its own X25519 private key, draws the charts natively on the EPD, and goes
back to deep sleep. Charts render in vector form on-device: no PNG decoding, no
screenshots, no dithering. So they're crisp, the payload is tiny, and battery
life is measured in weeks.

## Supported displays

|  | **Xteink X3** | **TRMNL X** | **Seeed reTerminal E1004** |
|---|---|---|---|
| Panel | 3.68" 792×528 | 10.3" 1872×1404 parallel | 13.3" 1200×1600 |
| Color | 1-bit mono | 16-level (4-bit) grayscale | Spectra 6 full color |
| MCU | ESP32-C3 | ESP32-S3 (16 MB flash / 8 MB PSRAM) | ESP32-S3 (32 MB flash / 8 MB OPI PSRAM) |
| Display driver | SSD1677 over SPI | FastEPD | Seeed_GFX / T133A01 (combo 523) |
| Shows | one panel per screen | whole-dashboard slideshow | whole-dashboard slideshow |
| Input | power button + rocker | touch to advance | button to advance |
| Transport | Wi-Fi + BLE fallback | Wi-Fi | Wi-Fi |
| Fuel gauge | BQ27220 | BQ27427 | — |
| Quiet hours | yes | — | yes |
| Firmware | [`firmware-cloud/`](firmware-cloud/README.md) | [`firmware-trmnl/`](firmware-trmnl/README.md) | [`firmware-e1004/`](firmware-e1004/README.md) |
| Worker objects | `/bundle`, `/battery` | `/bundle-trmnl`, `/battery-trmnl` | `/bundle-e1004`, `/battery-e1004` |
| Push service | `grafana-push` (4 min) | `grafana-push-trmnl` (5 min) | `grafana-push-e1004` (5 min) |

All three share the same bridge, the same sealed-bundle crypto, and the same
Worker. Each has its own device key and its own bundle object, so they run side
by side without interfering.

## Two rendering models

**Panel at a time — the X3.** One Grafana *panel* fills the screen: a
full-screen line chart, or a row of stat tiles. You navigate panels with the
buttons, and cycle the time window per panel.

**Whole dashboard — the TRMNL X and reTerminal E1004.** Instead of one panel per
screen, these cycle through the *dashboards* in a Grafana folder (default
`f8jmvq`), rendering every panel of a dashboard at its `gridPos`, scaled to fill
the screen. Grafana's threshold colors are carried through so status reads at a
glance — mapped to gray shades on the TRMNL X, and to real red / yellow / green
on the E1004.

The bundle formats follow that split. The X3 has its own panel bundle (currently
version 4). The two dashboard devices share one builder that emits version 2
(shade bytes are gray levels) or version 3 (shade bytes are Spectra 6 color
codes); the wire layout is otherwise identical, so a device keys its
interpretation off the header version alone.

## What it looks like

Shared across all three: charts drawn natively with title, axes, gridlines and
dotted fill under the line; stat panels with big numbers, units and sparklines;
a synthesized Battery (V) panel on the devices that have a fuel gauge, built
from readings the device POSTs back to the Worker after each fetch.

**On the X3**

- Long-press the power button → 2-column scrollable panel list (rocker buttons
  move the cursor)
- Double-click → cycle time window (24h ↔ 2h ↔ 7d). The per-panel default is
  read from Grafana's `timeFrom` override. Refresh cadence follows the window:
  2h refreshes every 5 min, 24h every 15 min, 7d hourly — finer windows want
  fresher data.
- A "Device Logs" screen (via the long-press list) showing recent serial-style
  output across deep-sleep cycles
- Quiet hours (22:00–06:00 local) suppress background Wi-Fi refreshes

**On the TRMNL X and reTerminal E1004**

- Auto-rotating slideshow through the folder's dashboards; touch (TRMNL X) or a
  button press (E1004) advances immediately
- Threshold zones render as banded backgrounds and stat tiles tint by status —
  gray shades on the TRMNL X, a pale dithered color wash on the E1004
- Quiet hours (22:00–06:00 local) on the E1004

## Architecture

```
Grafana Cloud
   │ PromQL + dashboard JSON via Grafana API
   ▼
Raspberry Pi  (bridge-cloud/, one systemd timer per device)
   │ build bundle → X25519-seal it with that device's public key
   ▼
Cloudflare Worker (worker/, R2-backed)
   │   /bundle        /bundle-trmnl        /bundle-e1004      (encrypted)
   │   /battery       /battery-trmnl       /battery-e1004     (plaintext)
   │
   ├─ Xteink X3 ──── Wi-Fi (HTTPS), primary path
   │     └─ iOS app over BLE GATT: fallback when no Wi-Fi is reachable
   │        (ios-app/, fetches from the Worker and relays the bytes)
   ├─ TRMNL X ────── Wi-Fi
   └─ reTerminal ─── Wi-Fi
         │ decrypt with X25519 private key (never leaves NVS)
         │ render natively
         ▼
      e-paper display
```

The bundle stays end-to-end encrypted throughout — Cloudflare and the iOS app
both see only ciphertext. Only the target device, which holds the private key it
generated on first boot, can decrypt it.

## Subdirectories

- **[`firmware-cloud/`](firmware-cloud/README.md)** — ESP32-C3 firmware for
  the X3. Multi-WiFi credentials with per-network refresh floors, first-boot
  QR-code key enrollment, X25519+AES-256-GCM decrypt, BLE peripheral
  fallback when WiFi is unreachable.
- **[`firmware-trmnl/`](firmware-trmnl/README.md)** — ESP32-S3 firmware for the
  TRMNL X. FastEPD 4-bit grayscale, whole-dashboard grid rendering scaled to
  fill the screen, threshold-to-gray status shading, auto-rotating slideshow
  with touch-to-advance. Wi-Fi only; same X25519+AES-256-GCM decrypt as the X3.
- **[`firmware-e1004/`](firmware-e1004/README.md)** — ESP32-S3 firmware for the
  reTerminal E1004. Seeed_GFX/T133A01 Spectra 6 color rendering (dithered
  threshold bands, status-tinted stat tiles), button-to-advance slideshow,
  quiet hours, same sealed-bundle pipeline with the v3 color wire format.
- **[`bridge-cloud/`](bridge-cloud/README.md)** — push-only service that runs on
  a Raspberry Pi. Queries Grafana, builds each device's binary bundle, seals it
  for that device, and uploads it to the Cloudflare Worker on a systemd timer
  (X3 every 4 minutes; TRMNL X and E1004 every 5). No inbound ports.
- **[`worker/`](worker/README.md)** — Cloudflare Worker (JS, deployed via
  GitHub Actions). Per-device endpoints behind a bearer token: `/bundle*`
  (sealed blob, R2-backed) and `/battery*` (rolling 7-day voltage history).
- **[`ios-app/`](ios-app/README.md)** — Swift companion app, BLE central
  in background mode. Wakes on the X3's advertisement, fetches the sealed
  bundle from the Worker, forwards the bytes to the X3 over BLE GATT.
  X3 only — the other two displays are Wi-Fi only.

## References

Everything we read while designing this. None are git-vendored — they're
research material only.

- **[bcrpntr/crosspet-x3](https://github.com/bcrpntr/crosspet-x3)** (MIT) —
  The most complete community firmware for the X3. The eink display driver
  in `firmware-cloud/lib/community-sdk` is bcrpntr's, included as a submodule.
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

### Xteink X3 (ESP32-C3)

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
  X3 PUTs the reading to the Worker's `/battery` endpoint after each
  successful bundle fetch; the Pi reads the rolling history back when
  building the next bundle, producing a synthetic Battery (V) panel.

### TRMNL X / reTerminal E1004 (ESP32-S3)

- Both are large panels driven from PSRAM, so the whole-dashboard bundle is
  held and rendered in RAM rather than staged through NVS like the X3.
- The TRMNL X reports battery via a BQ27427 (note: a different part from the
  X3's BQ27220 — the register map is not the same). The E1004 has no fuel
  gauge wired up, so it has no Battery (V) panel.
- Spectra 6 refreshes are slow relative to mono/grayscale; the E1004's
  slideshow cadence and quiet hours are set with that in mind.

## License

MIT — see [LICENSE](LICENSE).
