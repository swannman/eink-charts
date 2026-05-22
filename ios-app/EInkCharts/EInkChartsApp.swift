// Entry point. The app's *real* job is running BLECoordinator in the
// background — the UI is just a status panel for debugging.
//
// Background lifecycle: BLECoordinator owns a CBCentralManager with a
// fixed restore identifier. When iOS notices our X3's advertisement,
// it relaunches the app (even after a phone reboot, as long as the
// user has opened the app once since the reboot) and replays the
// peripheral-discovery callback so we can deliver a fresh bundle and
// go back to sleep.

import SwiftUI

@main
struct EInkChartsApp: App {
    @StateObject private var coordinator = BLECoordinator.shared

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(coordinator)
        }
    }
}
