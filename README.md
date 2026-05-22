# eink-charts

Custom firmware + push service that turns an Xteink X3 3.68" e-paper
display (ESP32-C3, 792×528, 1-bit, SSD1677) into a Grafana Cloud dashboard
viewer that works **anywhere you have your phone**.

The X3 wakes on a timer (or button press), fetches a pre-rendered bundle
of chart data either over Wi-Fi (HTTPS to a Cloudflare Worker) or BLE
(from an iOS companion app when no Wi-Fi is reachable), decrypts it
locally with its own X25519 private key, draws the charts natively on the
EPD, and goes back to deep sleep. Charts render in vector form on-device
— no PNG decoding, no dithering — so they're crisp, the payload is tiny,
and battery life is measured in weeks.

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
- A synthesized Battery (V) panel from BQ27220 readings the X3 posts back
  via a separate Worker endpoint
- A "Device Logs" screen (reachable via long-press list) showing the
  device's recent serial-style output across deep-sleep cycles

## Architecture

```
Grafana Cloud
   │ PromQL via Grafana API
   ▼
Raspberry Pi  (bridge-cloud/, systemd timer every 4 min)
   │ X25519-seal the bundle with the X3's public key
   ▼
Cloudflare Worker (worker/, R2-backed)  ── /bundle (encrypted)
   │                                       /battery (plaintext voltages)
   │
   ├─ X3 over Wi-Fi (HTTPS):  primary path
   │
   └─ iOS app over BLE:       fallback when no Wi-Fi reachable
       (ios-app/, fetches from Worker, relays bytes via BLE GATT)
   │
   ▼
Xteink X3  (firmware-cloud/)
   │ decrypt with X25519 private key (never leaves NVS)
   │ render chart natively
   ▼
e-paper display
```

The bundle stays end-to-end encrypted throughout — Cloudflare and the
iOS app both see only ciphertext. Only the X3 (which holds the private
key it generated on first boot) can decrypt.

## Subdirectories

- **[`firmware-cloud/`](firmware-cloud/README.md)** — ESP32-C3 firmware for
  the X3. Multi-WiFi credentials with per-network refresh floors, first-boot
  QR-code key enrollment, X25519+AES-256-GCM decrypt, BLE peripheral
  fallback when WiFi is unreachable.
- **[`bridge-cloud/`](bridge-cloud/README.md)** — push-only service that
  runs on a Raspberry Pi. Queries Grafana, builds the binary bundle, seals
  it for the X3, uploads to the Cloudflare Worker every 4 minutes via a
  systemd timer. No inbound ports.
- **[`worker/`](worker/README.md)** — Cloudflare Worker (JS, deployed via
  GitHub Actions). Two endpoints behind a bearer token: `/bundle` (sealed
  blob, R2-backed) and `/battery` (rolling 7-day voltage history).
- **[`ios-app/`](ios-app/README.md)** — Swift companion app, BLE central
  in background mode. Wakes on the X3's advertisement, fetches the sealed
  bundle from the Worker, forwards the bytes to the X3 over BLE GATT.

## References

Everything I read while designing this. None are git-vendored — they're
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

## License

MIT — see [LICENSE](LICENSE).
