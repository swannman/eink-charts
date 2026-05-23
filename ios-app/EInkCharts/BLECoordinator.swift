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

import Combine
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
    // Ring buffer of recent activity lines for the in-app log view. Same
    // strings that get printed to the Xcode debug console — having them
    // visible in-app means you don't need to be tethered to debug a
    // relay cycle. Bounded so we don't grow unbounded across a long-
    // running background session.
    @Published var log: [String] = []
    private let logCapacity = 200

    // The enclosing class is @MainActor, which would otherwise isolate
    // this static; mark it nonisolated so appendLog can format
    // timestamps from the BLE queue. DateFormatter is Sendable in
    // recent SDKs, so no `(unsafe)` needed.
    nonisolated private static let timestampFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    // CB types are documented as thread-safe — Apple says you can call any
    // method on them from any context. Marking them nonisolated(unsafe)
    // lets the CBPeripheralDelegate methods (which run on the BLE queue,
    // not MainActor) cancel/connect without bouncing through Task.
    nonisolated(unsafe) private var central: CBCentralManager!
    nonisolated(unsafe) private var pendingPeripheral: CBPeripheral?
    private var bundleData: Data?
    // State for the chunked write protocol. didWriteValueFor uses these
    // to advance to the next chunk until everything has been sent.
    nonisolated(unsafe) private var writeQueue: Data = Data()
    nonisolated(unsafe) private var writeChar: CBCharacteristic?
    nonisolated(unsafe) private var writeTotalBytes: Int = 0
    nonisolated(unsafe) private var writeBatteryChar: CBCharacteristic?

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

    // Helpers callable from CB delegate methods (nonisolated context).
    // Each hops to the MainActor via Task to set the @Published state.
    // appendLog drops the same line into the in-app log view and the
    // Xcode debug console, so the relay is observable either way.
    nonisolated private func appendLog(_ s: String) {
        print("[BLE] \(s)")
        let stamp = BLECoordinator.timestampFormatter.string(from: Date())
        let line = "\(stamp)  \(s)"
        Task { @MainActor in
            self.log.append(line)
            if self.log.count > self.logCapacity {
                self.log.removeFirst(self.log.count - self.logCapacity)
            }
        }
    }

    nonisolated private func setStatus(_ s: String) {
        appendLog(s)
        Task { @MainActor in self.status = s }
    }

    nonisolated private func setError(_ s: String?) {
        if let s = s { appendLog("ERROR: \(s)") }
        Task { @MainActor in self.lastError = s }
    }

    nonisolated private func setBluetoothEnabled(_ b: Bool) {
        Task { @MainActor in self.bluetoothEnabled = b }
    }

    nonisolated private func markSynced() {
        Task { @MainActor in self.lastSync = Date() }
    }

    nonisolated private func stateString(_ s: CBManagerState) -> String {
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
        writeQueue = Data()
        writeChar = nil
        writeTotalBytes = 0
        writeBatteryChar = nil
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
        peripheral.discoverCharacteristics(
            [Config.bleCharacteristicUUID, Config.bleBatteryCharacteristicUUID],
            for: service)
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral,
                                didDiscoverCharacteristicsFor service: CBService,
                                error: Error?) {
        let discovered = service.characteristics?.map { $0.uuid.uuidString } ?? []
        appendLog("discovered characteristics: \(discovered)")
        guard let char = service.characteristics?.first(where: { $0.uuid == Config.bleCharacteristicUUID }) else {
            setError("Bundle characteristic missing")
            central.cancelPeripheralConnection(peripheral)
            return
        }
        // Stash the battery characteristic (may be absent on old firmware
        // — non-fatal). We'll read it after the bundle drains so we don't
        // race the write protocol.
        writeBatteryChar = service.characteristics?.first(where: { $0.uuid == Config.bleBatteryCharacteristicUUID })
        appendLog("battery characteristic present? \(writeBatteryChar != nil)")
        // GATT caps a single write at 512 bytes (BLE_ATT_ATTR_MAX_LEN). The
        // X3 expects: first chunk = 4-byte LE u32 total length followed by
        // up to 508 bytes of bundle; subsequent chunks = up to 512 bytes
        // each. Stage the full byte stream (prefix + bundle) and let
        // didWriteValueFor drain it one write at a time.
        Task { @MainActor in
            guard let data = self.bundleData else {
                self.setError("Bundle missing at write time")
                self.central.cancelPeripheralConnection(peripheral)
                return
            }
            let total = UInt32(data.count).littleEndian
            var queue = Data(capacity: 4 + data.count)
            withUnsafeBytes(of: total) { queue.append(contentsOf: $0) }
            queue.append(data)
            self.writeQueue = queue
            self.writeChar = char
            self.writeTotalBytes = data.count
            self.setStatus("Writing \(data.count) bytes to X3 (chunked)…")
            self.writeNextChunk(peripheral: peripheral)
        }
    }

    nonisolated private func writeNextChunk(peripheral: CBPeripheral) {
        // Run on the BLE queue (nonisolated context). We only touch
        // writeQueue/writeChar from here and didWriteValueFor, both of
        // which run on that same queue — so the nonisolated(unsafe) is OK.
        guard let char = writeChar else { return }
        if writeQueue.isEmpty {
            markSynced()
            // Hand-off succeeded. If the firmware exposes the battery
            // characteristic, read it now; the response lands in
            // didUpdateValueFor, which forwards to /battery and then
            // disconnects. Older firmware without the characteristic
            // skips straight to disconnect.
            if let batteryChar = writeBatteryChar {
                setStatus("Delivered. Reading battery…")
                peripheral.readValue(for: batteryChar)
            } else {
                setStatus("Delivered. Disconnecting.")
                central.cancelPeripheralConnection(peripheral)
            }
            return
        }
        let chunkSize = min(writeQueue.count, 512)
        let chunk = writeQueue.prefix(chunkSize)
        writeQueue.removeFirst(chunkSize)
        peripheral.writeValue(Data(chunk), for: char, type: .withResponse)
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral,
                                didUpdateValueFor characteristic: CBCharacteristic,
                                error: Error?) {
        defer { central.cancelPeripheralConnection(peripheral) }
        guard characteristic.uuid == Config.bleBatteryCharacteristicUUID else {
            return
        }
        if let error = error {
            setError("Battery read failed: \(error.localizedDescription)")
            return
        }
        guard let bytes = characteristic.value, bytes.count >= 2 else {
            setStatus("Battery: no data")
            return
        }
        // u16 little-endian millivolts. 0 means "no reading" on the X3.
        let mv = UInt16(bytes[0]) | (UInt16(bytes[1]) << 8)
        guard mv > 0 else {
            setStatus("Delivered. (No battery data.) Disconnecting.")
            return
        }
        setStatus("Battery: \(mv) mV. Forwarding to Worker…")
        Task {
            do {
                try await BatteryReporter.report(mv: mv)
                self.setStatus("Battery \(mv) mV forwarded. Done.")
            } catch {
                self.setError("Battery forward failed: \(error)")
            }
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral,
                                didWriteValueFor characteristic: CBCharacteristic,
                                error: Error?) {
        if let error = error {
            setError("Write failed: \(error.localizedDescription)")
            setStatus("Write failed")
            central.cancelPeripheralConnection(peripheral)
            return
        }
        let remaining = writeQueue.count
        let sent = writeTotalBytes + 4 - remaining
        setStatus("Sent \(sent) / \(writeTotalBytes + 4) bytes…")
        writeNextChunk(peripheral: peripheral)
    }
}
