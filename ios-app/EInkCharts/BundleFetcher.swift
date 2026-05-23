// Pulls the sealed bundle from the Cloudflare Worker. The app never sees
// plaintext — these are just opaque encrypted bytes that we relay to the X3.

import Foundation

enum BundleFetcherError: Error {
    case badStatus(Int)
    case emptyBody
}

struct BundleFetcher {
    /// GET /bundle from the Worker. Returns the sealed bytes verbatim.
    static func fetch() async throws -> Data {
        let url = Config.workerBaseURL.appendingPathComponent("bundle")
        var req = URLRequest(url: url)
        req.setValue("Bearer \(Config.workerBearer)", forHTTPHeaderField: "Authorization")
        req.setValue("application/octet-stream", forHTTPHeaderField: "Accept")
        req.setValue("einkcharts-ios/1", forHTTPHeaderField: "User-Agent")
        req.timeoutInterval = 15

        let (data, response) = try await URLSession.shared.data(for: req)
        guard let http = response as? HTTPURLResponse else {
            throw BundleFetcherError.badStatus(-1)
        }
        guard http.statusCode == 200 else {
            throw BundleFetcherError.badStatus(http.statusCode)
        }
        guard !data.isEmpty else {
            throw BundleFetcherError.emptyBody
        }
        return data
    }
}

struct BatteryReporter {
    /// PUT /battery with the X3's BQ27220 reading. Same wire format the
    /// X3 uses when it posts directly over Wi-Fi: `{"mv": <int>}`. We
    /// only call this when the BLE fallback successfully delivered a
    /// bundle, since otherwise the data point is misleading.
    static func report(mv: UInt16) async throws {
        guard mv > 0 else { return }
        let url = Config.workerBaseURL.appendingPathComponent("battery")
        var req = URLRequest(url: url)
        req.httpMethod = "PUT"
        req.setValue("Bearer \(Config.workerBearer)", forHTTPHeaderField: "Authorization")
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.setValue("einkcharts-ios/1", forHTTPHeaderField: "User-Agent")
        req.httpBody = #"{"mv":\#(mv)}"#.data(using: .utf8)
        req.timeoutInterval = 10

        let (_, response) = try await URLSession.shared.data(for: req)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else {
            throw BundleFetcherError.badStatus((response as? HTTPURLResponse)?.statusCode ?? -1)
        }
    }
}
