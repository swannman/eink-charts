// Template — copy to Config.swift and fill in the bearer token. The real
// Config.swift is gitignored.

import CoreBluetooth
import Foundation

enum Config {
    static let workerBaseURL = URL(string: "https://dashboard.contexa.net")!
    static let workerBearer = "PUT-YOUR-BEARER-TOKEN-HERE"
    static let bleServiceUUID        = CBUUID(string: "0e1c0a9c-1bb1-4f1e-8e26-1c3c5a3e9c7f")
    static let bleCharacteristicUUID = CBUUID(string: "0e1c0a9c-1bb1-4f1e-8e26-1c3c5a3e9c80")
    static let bleRestoreIdentifier = "net.contexa.einkcharts.central"
}
