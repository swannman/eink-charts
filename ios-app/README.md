# ios-app/ — EInkCharts iOS companion

Swift / SwiftUI app that runs in the background as a BLE central. When
the X3 fails all configured Wi-Fi networks and falls back to advertising
its bundle service, iOS wakes this app (via Core Bluetooth state
restoration), it fetches the sealed bundle from the Cloudflare Worker,
forwards the bytes to the X3 over BLE GATT, and goes back to sleep.

The phone never sees plaintext — it's a transport, not a decryption peer.

## What's in the repo

```
ios-app/
├── README.md                       # this file
└── EInkCharts/
    ├── EInkChartsApp.swift         # @main app entry, hosts BLECoordinator
    ├── BLECoordinator.swift        # CBCentralManager + state restoration + GATT write
    ├── BundleFetcher.swift         # async GET /bundle from the Worker
    ├── ContentView.swift           # minimal status panel
    ├── Config.swift                # gitignored — bearer token + URLs (see template)
    ├── Config.template.swift       # committed template — copy to Config.swift
    └── Info.plist                  # bluetooth-central background mode + usage description
```

There's no Xcode project committed (the .pbxproj is a moving target that
diff-merges badly). Create the project once locally; see "First-time
Xcode setup" below.

## First-time Xcode setup

1. **Xcode → File → New → Project → iOS → App.**
   - Product Name: `EInkCharts`
   - Team: pick your Apple Developer Personal Team (or paid team)
   - Organization Identifier: `net.contexa` (the Bundle Identifier
     becomes `net.contexa.einkcharts`)
   - Interface: SwiftUI
   - Language: Swift
   - Storage: None / No
2. **Save the project at `ios-app/`.** Let Xcode create the
   `EInkCharts.xcodeproj` next to the `EInkCharts/` directory we provide.
3. **Replace the default source files.** Xcode auto-creates
   `ContentView.swift` and `EInkChartsApp.swift`; delete those and add
   the ones from this repo (drag them in from Finder, keep "Copy items
   if needed" *unchecked* so they remain in the repo path).
4. **Wire up the Info.plist.** Easiest path: open the target → Info tab
   → set `Custom iOS Target Properties` to match `Info.plist`. Or set
   `INFOPLIST_FILE = ios-app/EInkCharts/Info.plist` in Build Settings and
   disable Xcode's auto-generated Info.plist.
5. **Signing & Capabilities → Capabilities → Background Modes →** check
   **Uses Bluetooth LE accessories**.
6. **Copy `Config.template.swift` to `Config.swift`** and paste the same
   bearer token used by the X3 firmware (`DEFAULT_WORKER_BEARER` in
   `firmware-cloud/src/secrets.h`) and the Pi push service
   (`WORKER_BEARER_TOKEN` in `/etc/default/grafana-push`).
7. **Build + run on a physical iPhone.** The simulator doesn't have a
   BLE radio. First launch will prompt for Bluetooth permission.

## How it works

`BLECoordinator` (singleton, marked `@MainActor`) creates a
`CBCentralManager` with a fixed `CBCentralManagerOptionRestoreIdentifierKey`
so iOS can relaunch the app on Bluetooth events. It scans for
`Config.bleServiceUUID` (must match the firmware) — scan-with-service
form is the iOS-blessed background-friendly pattern.

On `didDiscover`:
1. Stop scanning so we don't get a flood of duplicate ads.
2. Fetch the sealed bundle from the Worker via `BundleFetcher`.
3. Connect to the peripheral.
4. Discover the bundle service + characteristic.
5. Write the sealed bytes (`type: .withResponse` so iOS handles long-write
   chunking automatically when the bundle exceeds MTU).
6. Disconnect on write completion.
7. Resume scanning.

On `willRestoreState`: iOS hands back any peripherals we were tracking
when we got suspended/killed. We re-adopt the delegate so the rest of
the lifecycle keeps working.

## iOS background quirks (none are dealbreakers)

- **First install + every phone reboot**: user must open the app once.
  After that, state restoration handles relaunch.
- **User force-quits**: iOS treats force-quit as "the user explicitly
  doesn't want this running" and won't relaunch. Reopen the app once.
- **Memory pressure** can kill background apps. iOS relaunches them
  on relevant events; brief silent windows possible.

In practice this works fine for the Tile / AirPods / `nRF` class of
background-helper apps. Reliability is "fine 95% of the time, plus
occasionally tap the app to wake it back up."

## Trust boundary

Identical to the Wi-Fi path:
- Bundle is X25519-sealed by the Pi *before* it reaches the Worker.
- The Worker only ever sees ciphertext (gated by bearer token).
- This iOS app fetches the same ciphertext and forwards it unchanged
  to the X3.
- Only the X3's X25519 private key (generated on its first boot, never
  leaves NVS) can decrypt.

The bearer token IS visible to the iOS app — it has to use it to read
from the Worker. That's why it lives in the gitignored `Config.swift`
and not in committed source.

## See also

- [`../firmware-cloud/`](../firmware-cloud/README.md) — the X3 firmware,
  including the BLE peripheral side of this protocol.
- [`../worker/`](../worker/README.md) — the Cloudflare Worker the iOS app
  fetches from.
- [`../bridge-cloud/`](../bridge-cloud/README.md) — the Pi push service
  that originally produces the sealed bundles.
