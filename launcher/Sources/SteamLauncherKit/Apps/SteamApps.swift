import Foundation

public struct AppDepot: Sendable, Hashable {
    public let id: UInt32
    public let name: String
    public let maxSize: UInt64
    public let publicManifest: UInt64?
    public let oslist: String
    public let osarch: String
    public let lowViolence: Bool
    /// Some depots are shared from another app (e.g. Steamworks redistributables).
    public let fromApp: UInt32?
}

public struct AppInfo: Sendable {
    public let appID: UInt32
    public let name: String
    public let buildID: UInt64
    public let depots: [AppDepot]
    public let changeNumber: UInt32
    public let missingToken: Bool
    /// Steam Cloud Auto-Cloud rules (`ufs/savefiles`).
    public let cloudRules: [CloudSaveRule]
}

/// Product info (PICS), licenses, depot keys and content servers.
public struct SteamApps {
    public let cm: CMClient
    public init(cm: CMClient) { self.cm = cm }

    public func accessTokens(apps: [UInt32] = [], packages: [UInt32] = []) async throws -> (apps: [UInt32: UInt64], packages: [UInt32: UInt64]) {
        let p = try await cm.request(.clientPICSAccessTokenRequest, body: CMsgClientPICSAccessTokenRequest(packageIDs: packages, appIDs: apps))
        let r = try CMsgClientPICSAccessTokenResponse(proto: p.body)
        return (r.appTokens, r.packageTokens)
    }

    func productInfo(apps: [(UInt32, UInt64)] = [], packages: [(UInt32, UInt64)] = []) async throws -> CMsgClientPICSProductInfoResponse {
        let req = CMsgClientPICSProductInfoRequest(packages: packages.map { .init(id: $0.0, accessToken: $0.1) },
                                                   apps: apps.map { .init(id: $0.0, accessToken: $0.1) })
        let p = try await cm.request(.clientPICSProductInfoRequest, body: req, timeout: 60)
        return try CMsgClientPICSProductInfoResponse(proto: p.body)
    }

    /// The raw app info KeyValues (e.g. for the `ufs` Steam Cloud rules).
    public func appInfoKV(_ appID: UInt32) async throws -> KV {
        let token = (try? await accessTokens(apps: [appID]).apps[appID]) ?? 0
        let r = try await productInfo(apps: [(appID, token)])
        guard let item = r.apps.first(where: { $0.id == appID }) else { throw SteamError.badData("Steam doesn't know app \(appID)") }
        let kv = try KV.parseText(item.buffer)
        return kv["appinfo"] ?? kv
    }

    public func appInfo(_ appID: UInt32) async throws -> AppInfo {
        let token = (try? await accessTokens(apps: [appID]).apps[appID]) ?? 0
        let r = try await productInfo(apps: [(appID, token)])
        guard let item = r.apps.first(where: { $0.id == appID }) else {
            throw SteamError.badData("Steam doesn't know app \(appID)")
        }
        let kv = try KV.parseText(item.buffer)
        let root = kv["appinfo"] ?? kv
        var depots: [AppDepot] = []
        for (key, v) in root["depots"]?.children ?? [] {
            guard let id = UInt32(key) else { continue }
            let manifests = v["manifests"]
            let pub = manifests?["public"]
            let gid = pub?["gid"]?.uint64 ?? pub?.uint64
            depots.append(AppDepot(
                id: id, name: v["name"]?.string ?? "", maxSize: v["maxsize"]?.uint64 ?? 0, publicManifest: gid,
                oslist: v["config"]?["oslist"]?.string ?? "", osarch: v["config"]?["osarch"]?.string ?? "",
                lowViolence: v["config"]?["lowviolence"]?.string == "1",
                fromApp: v["depotfromapp"]?.int.map { UInt32($0) }))
        }
        let buildID = root["depots"]?["branches"]?["public"]?["buildid"]?.uint64 ?? 0
        return AppInfo(appID: appID, name: root["common"]?["name"]?.string ?? "App \(appID)", buildID: buildID,
                       depots: depots, changeNumber: item.changeNumber, missingToken: item.missingToken,
                       cloudRules: CloudSaveRule.parse(ufs: root["ufs"]))
    }

    public struct Ownership: Sendable {
        public let owned: Bool
        public let packageID: UInt32?
        public let depotIDs: Set<UInt32>
    }

    /// Checks the account's licenses: does any package grant this app? Steam enforces the same
    /// thing server-side, since only owners get depot keys.
    public func ownership(of appID: UInt32, licenses: [License]) async throws -> Ownership {
        let ids = licenses.map(\.packageID)
        guard !ids.isEmpty else { return Ownership(owned: false, packageID: nil, depotIDs: []) }
        var tokens: [UInt32: UInt64] = [:]
        for l in licenses where l.accessToken != 0 { tokens[l.packageID] = l.accessToken }
        var owner: UInt32?
        var depots = Set<UInt32>()
        // Big accounts own thousands of packages; ask in batches.
        for batch in stride(from: 0, to: ids.count, by: 400).map({ Array(ids[$0..<min($0 + 400, ids.count)]) }) {
            let r = try await productInfo(packages: batch.map { ($0, tokens[$0] ?? 0) })
            for p in r.packages where p.buffer.count > 4 {
                let kv = try KV.parseBinary(p.buffer.dropFirst(4))
                let pkg = kv.children.first?.1 ?? kv
                let apps = pkg["appids"]?.children.compactMap { $0.1.int.map { UInt32($0) } } ?? []
                if apps.contains(appID) {
                    owner = owner ?? p.id
                    for (_, d) in pkg["depotids"]?.children ?? [] { if let v = d.int { depots.insert(UInt32(v)) } }
                }
            }
        }
        return Ownership(owned: owner != nil, packageID: owner, depotIDs: depots)
    }

    /// The AES key for a depot. Steam only hands it to accounts that own the app.
    public func depotKey(depotID: UInt32, appID: UInt32) async throws -> Data {
        let p = try await cm.request(.clientGetDepotDecryptionKey, body: CMsgClientGetDepotDecryptionKey(depotID: depotID, appID: appID))
        let r = try CMsgClientGetDepotDecryptionKeyResponse(proto: p.body)
        guard r.eresult == .ok, r.key.count == 32 else { throw SteamError.result(r.eresult, "Depot key for \(depotID)") }
        return r.key
    }

    /// Manifests are gated by a short-lived request code since 2022.
    public func manifestRequestCode(appID: UInt32, depotID: UInt32, manifestID: UInt64, branch: String = "public") async throws -> UInt64 {
        let data = try await cm.call("ContentServerDirectory.GetManifestRequestCode#1",
                                     GetManifestRequestCodeRequest(appID: appID, depotID: depotID, manifestID: manifestID, branch: branch))
        return try GetManifestRequestCodeResponse(proto: data).code
    }

    public func contentServers(cellID: UInt32) async throws -> [ContentServer] {
        let data = try await cm.call("ContentServerDirectory.GetServersForSteamPipe#1", GetServersForSteamPipeRequest(cellID: cellID))
        return try GetServersForSteamPipeResponse(proto: data).servers
            .filter { $0.type == "SteamCache" || $0.type == "CDN" }
            .filter { !$0.useAsProxy }
            .sorted { $0.weightedLoad < $1.weightedLoad }
    }
}
