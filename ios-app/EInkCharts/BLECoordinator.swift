// Owns the CoreBluetooth central. Lifecycle:
//
//   1. App launches (foreground OR iOS-restored). init() creates the
//      CBCentralManager with a restore identifier so iOS can relaunch us
//      on the X3's advertisement.
//   2. centralManagerDidUpdateState fires when Bluetooth is ready.
//      We start scanning for our service UUID — scan-with-service-UUID
//      is the iOS-blessed background-friendly form.
//   3. didDiscover fires when the X3 is in range. We fetch the sealed
//      bundle from the Worker, connect to the X3, write the bundle to
//      the bundle characteristic, disconnect, and resume scanning.
//   4. iOS may suspend the app between events. State restoration plus
//      the scan-with-service form lets it wake us when the X3 advertises
//      again on its next deep-sleep cycle.
//
// The whole thing is a singleton because there's only one BLE central
// and iOS expects state restoration to find the same delegate object.

import CoreBluetooth
import Foundation
import SwiftUI

@MainActor
final class BLECoordinator: NSObject, ObservableObject {
    static let shared = BLECoordinator()

    @Published var status: String = "Initializing…"
    @Published var lastSync: Date?
    @Published var lastError: String?
    @Published var bluetoothEnabled = false

    private var central: CBCentralManager!
    private var pendingPeripheral: CBPeripheral?
    private var bundleData: Data?

    private override init() {
        super.init()
        let queue = DispatchQueue(label: "BLECoordinator", qos: .userInitiated)
        central = CBCentralManager(
            delegate: self,
            queue: queue,
            options: [
                CBCentralManagerOptionRestoreIdentifierKey: Config.bleRestoreIdentifier,
                CBCentralManagerOptionShowPowerAlertKey: true,
            ]
        )
    }

    /// Tap target for the "Sync now" button. Real syncs happen automatically
    /// via the scan/restore path; this just forces a fresh scan.
    func startScan() {
        guard central.state == .poweredOn else {
            setStatus("Bluetooth off (\(stateString(central.state)))")
            return
        }
        central.stopScan()
        central.scanForPeripherals(withServices: [Config.bleServiceUUID], options: nil)
        setStatus("Scanning for X3…")
    }

    private func setStatus(_ s: String) {
        Task { @MainActor in self.status = s }
    }

    private func setError(_ s: String?) {
        Task { @MainActor in self.lastError = s }
    }

    private func setBluetoothEnabled(_ b: Bool) {
        Task { @MainActor in self.bluetoothEnabled = b }
    }

    private func markSynced() {
        Task { @MainActor in self.lastSync = Date() }
    }

    private func stateString(_ s: CBManagerState) -> String {
        switch s {
        case .poweredOn:    return "on"
        case .poweredOff:   return "off"
        case .unauthorized: return "unauthorized (grant Bluetooth permission)"
        case .unsupported:  return "unsupported"
        case .resetting:    return "resetting"
        default:            return "unknown"
        }
    }
}

extension BLECoordinator: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let on = (central.state == .poweredOn)
        Task { @MainActor in
            self.bluetoothEnabled = on
        }
        if on {
            central.scanForPeripherals(withServices: [Config.bleServiceUUID], options: nil)
            setStatus("Scanning for X3…")
        } else {
            setStatus("Bluetooth \(stateString(central.state))")
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        // iOS handed us back our state. Anything we were connected to is
        // listed here; we adopt the delegate so callbacks keep flowing.
        if let peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral] {
            for p in peripherals {
                p.delegate = self
                Task { @MainActor in self.pendingPeripheral = p }
            }
        }
        setStatus("Restored by iOS — \(peripherals(dict).count) pending peripherals")
    }

    private nonisolated func peripherals(_ dict: [String: Any]) -> [CBPeripheral] {
        (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral]) ?? []
    }

    nonisolated func centralManager(_ central: CBCentralManager,
                                    didDiscover peripheral: CBPeripheral,
                                    advertisementData: [String: Any],
                                    rssi RSSI: NSNumber) {
        // Only one X3 in this house; first-seen wins. Stop scanning while we
        // service it; we'll resume after disconnect.
        central.stopScan()
        peripheral.delegate = self
        Task { @MainActor in self.pendingPeripheral = peripheral }
        setStatus("Found X3 (rssi=\(RSSI)). Fetching bundle from Worker…")

        Task {
            do {
                let data = try await BundleFetcher.fetch()
                Task { @MainActor in self.bundleData = data }
                self.setStatus("Got \(data.count)-byte bundle. Connecting to X3…")
                central.connect(peripheral, options: nil)
            } catch {
                self.setError("Worker fetch failed: \(error)")
                self.setStatus("Worker fetch failed — rescanning")
                central.scanForPeripherals(withServices: [Config.bleServiceUUID], options: nil)
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        setStatus("Connected. Discovering bundle service…")
        peripheral.discoverServices([Config.bleServiceUUID])
    }

    nonisolated func centralManager(_ central: CBCentralManager,
                                    didFailToConnect peripheral: CBPeripheral,
                                    error: Error?) {
        setError("Failed to connect: \(error?.localizedDescription ?? "unknown")")
        setStatus("Connect failed — rescanning")
        central.scanForPeripherals(withServices: [Config.bleServiceUUID], options: nil)
    }

    nonisolated func centralManager(_ central: CBCentralManager,
                                    didDisconnectPeripheral peripheral: CBPeripheral,
                                    error: Error?) {
        if let error = error {
            setStatus("Disconnected: \(error.localizedDescription)")
        } else {
            setStatus("Disconnected — scanning again")
        }
        Task { @MainActor in
            self.pendingPeripheral = nil
            self.bundleData = nil
        }
        central.scanForPeripherals(withServices: [Config.bleServiceUUID], options: nil)
    }
}

extension BLECoordinator: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == Config.bleServiceUUID }) else {
            setError("X3 didn't expose the expected service")
            central.cancelPeripheralConnection(peripheral)
            return
        }
        peripheral.discoverCharacteristics([Config.bleCharacteristicUUID], for: service)
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral,
                                didDiscoverCharacteristicsFor service: CBService,
                                error: Error?) {
        guard let char = service.characteristics?.first(where: { $0.uuid == Config.bleCharacteristicUUID }) else {
            setError("Bundle characteristic missing")
            central.cancelPeripheralConnection(peripheral)
            return
        }
        // Snapshot the buffer off the main actor before crossing nonisolated.
        Task { @MainActor in
            guard let data = self.bundleData else {
                self.setError("Bundle missing at write time")
                self.central.cancelPeripheralConnection(peripheral)
                return
            }
            self.setStatus("Writing \(data.count) bytes to X3…")
            peripheral.writeValue(data, for: char, type: .withResponse)
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral,
                                didWriteValueFor characteristic: CBCharacteristic,
                                error: Error?) {
        if let error = error {
            setError("Write failed: \(error.localizedDescription)")
            setStatus("Write failed")
        } else {
            markSynced()
            setStatus("Delivered. Disconnecting.")
        }
        central.cancelPeripheralConnection(peripheral)
    }
}
