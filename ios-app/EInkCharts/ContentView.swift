// Status panel + scrolling activity log. The app does all real work in
// the background; this view is so you can see what state the
// coordinator is in and tail recent BLE events without attaching Xcode.

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

            GroupBox(label: Text("Activity").font(.headline)) {
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 2) {
                            ForEach(Array(coordinator.log.enumerated()), id: \.offset) { idx, line in
                                Text(line)
                                    .font(.caption2.monospaced())
                                    .frame(maxWidth: .infinity, alignment: .leading)
                                    .id(idx)
                            }
                        }
                        .padding(.vertical, 4)
                    }
                    .frame(maxWidth: .infinity)
                    .onChange(of: coordinator.log.count) { _, _ in
                        if let last = coordinator.log.indices.last {
                            withAnimation { proxy.scrollTo(last, anchor: .bottom) }
                        }
                    }
                }
            }
            .frame(maxHeight: .infinity)

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
