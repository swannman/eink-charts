// Debug-y status panel. The real work happens in the background; the
// UI is only here so you can see what state the coordinator is in and
// force a scan during development.

import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var coordinator: BLECoordinator

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Image(systemName: coordinator.bluetoothEnabled ? "bolt.horizontal.circle.fill" : "bolt.slash.circle")
                    .foregroundStyle(coordinator.bluetoothEnabled ? .green : .secondary)
                Text("EInkCharts Companion")
                    .font(.title2.bold())
                Spacer()
            }

            GroupBox(label: Text("Status").font(.headline)) {
                Text(coordinator.status)
                    .font(.body.monospaced())
                    .frame(maxWidth: .infinity, alignment: .leading)
                if let last = coordinator.lastSync {
                    Text("Last sync: \(last, style: .relative) ago")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            if let err = coordinator.lastError {
                GroupBox(label: Text("Last error").font(.headline)) {
                    Text(err)
                        .font(.caption.monospaced())
                        .foregroundStyle(.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }

            Button {
                coordinator.startScan()
            } label: {
                Label("Sync now", systemImage: "arrow.triangle.2.circlepath")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)

            Spacer()

            Text("This app forwards encrypted dashboard updates from Cloudflare to the X3 over Bluetooth when no Wi-Fi network is reachable. It runs in the background; you can normally close it.")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
        .padding()
    }
}

#Preview {
    ContentView()
        .environmentObject(BLECoordinator.shared)
}
