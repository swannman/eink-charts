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
    ├── BLECoordinator.swift        # CBCentralManager + state restoration + chunked GATT writes
    ├── BundleFetcher.swift         # async GET /bundle from the Worker
    ├── ContentView.swift           # minimal status panel
    ├── Config.swift                # gitignored — bearer token + URLs (see template)
    └── Config.template.swift       # committed template — copy to Config.swift
```

There is no `Info.plist` checked in. Xcode 26 generates one at build
time from build settings — see step 4 below.

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
4. **Info.plist (Xcode 26 auto-generated).** Leave
   `GENERATE_INFOPLIST_FILE = YES` (the default). In Build Settings, set:
   - `INFOPLIST_KEY_NSBluetoothAlwaysUsageDescription` = `EInkCharts uses
     Bluetooth to deliver dashboard updates to the e-paper display when
     Wi-Fi isn't reachable.`
   - `INFOPLIST_KEY_UIBackgroundModes` = `bluetooth-central` (multi-value
     list, single entry).

   Do NOT commit a hand-written `Info.plist` and do NOT add one to the
   Copy Bundle Resources phase — Xcode 26 will complain about
   "Multiple commands produce Info.plist".
5. **Signing & Capabilities → Capabilities → Background Modes →** check
   **Uses Bluetooth LE accessories** (this is the same setting as the
   `UIBackgroundModes = [bluetooth-central]` build setting above; Xcode
   keeps them in sync).
6. **Target Membership for `Config.template.swift`**: in the File
   Inspector for that file, **uncheck** the EInkCharts target — otherwise
   you'll get `Invalid redeclaration of 'Config'` once you add the real
   `Config.swift`.
7. **Copy `Config.template.swift` to `Config.swift`** and paste the same
   bearer token used by the X3 firmware (`DEFAULT_WORKER_BEARER` in
   `firmware-cloud/src/secrets.h`) and the Pi push service
   (`WORKER_BEARER_TOKEN` in `/etc/default/grafana-push`).
8. **Build + run on a physical iPhone.** The simulator doesn't have a
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
5. **Chunked write**: GATT caps a single attribute value at 512 bytes,
   so the app stages a byte queue of `[4-byte LE total length][bundle…]`,
   then drains it 512 bytes per write-with-response, waiting on
   `didWriteValueFor` between writes. The X3 reads the length prefix
   on the first write and accumulates until it has the declared total.
6. Disconnect on queue drain.
7. Resume scanning.

For an 18 KB bundle that's ~37 round trips, finishing in a couple of
seconds at typical iOS BLE connection intervals.

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
