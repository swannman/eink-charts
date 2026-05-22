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
