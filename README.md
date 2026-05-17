# eink-charts

Custom firmware + bridge service that turns a **Xteink X3** 3.68" e-paper
display (ESP32-C3, 792×528, 1-bit, SSD1677) into a battery-powered Grafana
Cloud dashboard viewer. The device shows one panel at a time and you step
through them with the physical buttons.

The X3 wakes on a timer (or button press), pulls a pre-rendered bundle of
chart data over Wi-Fi from a small bridge service on a Raspberry Pi, draws
the charts natively on the EPD, and goes back to deep sleep. Charts render
in vector form on-device — no PNG decoding, no dithering — so they're crisp,
the payload is tiny, and battery life is measured in weeks.

## What it looks like

- Full-screen line charts with title, axes, gridlines, and dotted fill under
  the line
- Side-by-side **stat panels** with big numbers, units, and sparklines
  (auto-grouped from Grafana stat panels that share a row)
- Long-press the power button → 2-column scrollable panel list (rocker
  buttons move the cursor)
- Double-click → cycle time window (24h ↔ 2h ↔ 7d). Per-panel default view
  is read from Grafana's `timeFrom` override. The refresh cadence follows
  the time window: 2h view refreshes every 5 min, 24h every 15 min, 7d
  every hour — finer windows want fresher data.
- Synthetic Battery (V) panel built from BQ27220 fuel-gauge readings the
  device piggy-backs on every fetch
- Quiet hours (22:00–06:00 local) suppress background Wi-Fi refreshes

## Architecture

```
Grafana Cloud  →  bridge service  →  X3 firmware  →  EPD
 (PromQL)        (binary bundle)    (deep sleep    (SSD1677)
                                     + button wake)
```

- **bridge/** — FastAPI service (Python 3.13) on the Raspberry Pi. Runs
  PromQL queries against Grafana Cloud for each panel × time window (24h,
  2h, 7d), validates them, packs them into a single compact binary "bundle"
  cached in memory, serves it at `GET /data/all`. Refreshes every 4 min.
- **firmware/** — PlatformIO/Arduino project for the ESP32-C3. Wakes,
  fetches the bundle into NVS, renders the current panel using a tiny
  in-house GFX layer, sleeps. Stays awake only for the long-press list UI.

Bundle format is documented inline in `bridge/src/grafana_bridge/data.py`
and parsed in `firmware/src/main.cpp` — versioned (currently v4) so the
firmware fails gracefully when the bridge ships a new schema.

## Building the firmware

```sh
cp firmware/platformio.local.ini.example firmware/platformio.local.ini
# fill in WiFi SSID/password and bridge URL
cd firmware
pio run -t upload
```

The X3's USB Serial JTAG is finicky during deep sleep, so the easiest flash
flow is: hold the BOOT button while replugging USB, then `pio run -t upload`.

## Deploying the bridge

```sh
cp bridge/deploy/config.yaml.example /etc/grafana-bridge/config.yaml
cp bridge/deploy/grafana-bridge.env.example /etc/default/grafana-bridge
# fill in GRAFANA_TOKEN, dashboard UIDs, panel IDs
sudo bridge/deploy/install.sh
```

Runs as a dedicated `grafana-bridge` systemd user.

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
- Battery: BQ27220 fuel gauge over I²C. Voltage at register `0x08`.

## License

MIT — see [LICENSE](LICENSE).
