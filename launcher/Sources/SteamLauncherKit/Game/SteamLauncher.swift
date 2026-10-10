import Foundation

/// The launcher's whole Steam story in one place: sign in, prove ownership, install or update,
/// and pull cloud saves. The UI drives this; it holds no UI state itself.
public final class SteamLauncher: @unchecked Sendable {
    public let profile: GameProfile
    public let installRoot: URL
    public private(set) var cm = CMClient()
    public private(set) var logon: CMClient.LogonInfo?
    public var log: @Sendable (String) -> Void = { _ in }

    public init(profile: GameProfile, installRoot: URL) {
        self.profile = profile
        self.installRoot = installRoot
    }

    // MARK: Connection and sign-in

    /// Connects, retrying on another CM if one drops us (Steam does that under load).
    public func connect(avoiding: Set<String> = []) async throws {
        var lastError: Error = SteamError.noServers
        for attempt in 0..<3 {
            do {
                await cm.disconnect()
                cm = CMClient()
                let log = self.log
                await cm.setLogger(log)
                try await cm.connect(avoiding: avoiding)
                return
            } catch {
                lastError = error
                try await Task.sleep(nanoseconds: UInt64(attempt + 1) * 700_000_000)
            }
        }
        throw lastError
    }

    /// `deviceName` is what the Steam app shows on its Approve screen.
    public func beginQRSignIn(deviceName: String = SteamAuth.deviceName()) async throws -> SteamAuthSession {
        try await connect()
        return try await SteamAuth.beginQR(cm: cm, deviceName: deviceName)
    }

    /// `deviceName` is what the Steam app shows on its Approve screen.
    public func beginPasswordSignIn(account: String, password: String, guardData: String?,
                                    deviceName: String = SteamAuth.deviceName()) async throws -> SteamAuthSession {
        try await connect()
        return try await SteamAuth.beginCredentials(cm: cm, accountName: account, password: password, guardData: guardData,
                                                    deviceName: deviceName)
    }

    /// Logs on with saved credentials. Reconnects first if the socket went away.
    @discardableResult
    public func logOn(_ creds: SteamCredentials) async throws -> CMClient.LogonInfo {
        let info = try await withLogonRetry {
            try await self.cm.logon(accountName: creds.accountName, refreshToken: creds.refreshToken,
                                    machineName: SteamAuth.deviceName())
        }
        logon = info
        return info
    }

    /// For tests against free content only.
    public func logOnAnonymously() async throws {
        logon = try await withLogonRetry { try await self.cm.logonAnonymous() }
    }

    /// Runs a logon, moving to another CM when the socket drops or the CM says it's busy
    /// (TryAnotherCM, ServiceUnavailable). Any other answer from Steam is final.
    private func withLogonRetry<T>(_ body: () async throws -> T) async throws -> T {
        var lastError: Error = SteamError.notConnected
        var turnedAway: Set<String> = []
        for attempt in 0..<4 {
            do {
                if await cm.endpoint == nil { try await connect(avoiding: turnedAway) }
                return try await body()
            } catch SteamError.result(let r, let what) where r != .tryAnotherCM && r != .serviceUnavailable {
                throw SteamError.result(r, what)
            } catch {
                lastError = error
                if let e = await cm.endpoint { turnedAway.insert(e) }
                log("logon attempt \(attempt + 1) failed: \(error)")
                try? await Task.sleep(nanoseconds: UInt64(attempt + 1) * 500_000_000)
                try? await connect(avoiding: turnedAway)
            }
        }
        throw lastError
    }

    public func signOut() async {
        await cm.logOff()
        logon = nil
    }

    // MARK: Ownership

    public func checkOwnership() async throws -> SteamApps.Ownership {
        let licenses = try await cm.licenses()
        return try await SteamApps(cm: cm).ownership(of: profile.appID, licenses: licenses)
    }

    // MARK: Install

    public struct DepotPlan: Sendable {
        public let depotID: UInt32
        public let manifest: DepotManifest
        public let key: Data
        public let bytes: UInt64
    }

    public struct InstallPlan: Sendable {
        public let buildID: UInt64
        public let depots: [DepotPlan]
        public let servers: [ContentServer]
        public var totalBytes: UInt64 { depots.reduce(0) { $0 + $1.bytes } }
    }

    public struct InstalledState: Codable, Sendable {
        public var buildID: UInt64
        public var manifests: [String: UInt64]
        public var date: Date
    }

    private var stateURL: URL { installRoot.appendingPathComponent(".launcher-install.json") }

    public var installed: InstalledState? {
        (try? Data(contentsOf: stateURL)).flatMap { try? JSONDecoder().decode(InstalledState.self, from: $0) }
    }

    /// Fetches the current build's manifests (or the pinned ones). Only owners get past the depot key and
    /// manifest request code, so this is also Steam's own ownership check.
    public func plan() async throws -> InstallPlan {
        let apps = SteamApps(cm: cm)
        let info = try await apps.appInfo(profile.appID)
        let servers = try await apps.contentServers(cellID: logon?.cellID ?? 0)
        let cdn = CDNClient(servers: servers)
        var depots: [DepotPlan] = []
        for id in profile.depots {
            guard let gid = profile.pinnedManifests[id] ?? info.depots.first(where: { $0.id == id })?.publicManifest else {
                throw SteamError.badData("depot \(id) has no public build")
            }
            let key = try await apps.depotKey(depotID: id, appID: profile.appID)
            let code = try await apps.manifestRequestCode(appID: profile.appID, depotID: id, manifestID: gid)
            let manifest = try await cdn.manifest(depotID: id, manifestID: gid, requestCode: code, depotKey: key)
            let dl = downloader(cdn: cdn, depotID: id, key: key, manifest: manifest)
            depots.append(DepotPlan(depotID: id, manifest: manifest, key: key, bytes: dl.plannedBytes))
        }
        return InstallPlan(buildID: info.buildID, depots: depots, servers: servers)
    }

    private func downloader(cdn: CDNClient, depotID: UInt32, key: Data, manifest: DepotManifest) -> DepotDownloader {
        let dl = DepotDownloader(cdn: cdn, depotID: depotID, key: key, manifest: manifest, root: installRoot)
        let profile = self.profile
        dl.policy = profile.policy
        dl.localPath = profile.localPath
        return dl
    }

    public var freeSpace: UInt64? {
        #if os(Linux)
        let attrs = try? FileManager.default.attributesOfFileSystem(forPath: installRoot.deletingLastPathComponent().path)
        return (attrs?[.systemFreeSize] as? NSNumber)?.uint64Value
        #else
        let v = try? installRoot.deletingLastPathComponent()
            .resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return v?.volumeAvailableCapacityForImportantUsage.map { UInt64(max(0, $0)) }
        #endif
    }

    /// Where the system's background downloader leaves chunks for the next install run.
    public var staged: StagedChunks { StagedChunks(installRoot: installRoot) }

    /// Every chunk the plan still needs, in download order, minus what is already staged.
    public func remainingChunks(_ plan: InstallPlan) -> [ChunkRequest] {
        let cdn = CDNClient(servers: plan.servers)
        return plan.depots.flatMap { d in
            downloader(cdn: cdn, depotID: d.depotID, key: d.key, manifest: d.manifest).remaining(staged: staged)
        }
    }

    /// Downloads (or resumes) every depot of the plan. Progress covers the whole game. Chunks the
    /// system fetched in the background are unpacked first; chunks named in `skipping` are still
    /// on their way, so the install is only marked complete when nothing was skipped.
    public func install(_ plan: InstallPlan, skipping: Set<String> = [],
                        progress: @escaping @Sendable (DownloadProgress) -> Void) async throws {
        try FileManager.default.createDirectory(at: installRoot, withIntermediateDirectories: true)
        let cdn = CDNClient(servers: plan.servers)
        let total = plan.totalBytes
        let staged = self.staged
        var before: UInt64 = 0
        for d in plan.depots {
            let dl = downloader(cdn: cdn, depotID: d.depotID, key: d.key, manifest: d.manifest)
            let offset = before
            try await dl.run(staged: staged, skipping: skipping) { p in
                var q = p
                q.totalBytes = total
                q.doneBytes = offset + p.doneBytes
                progress(q)
            }
            before += d.bytes
        }
        guard skipping.isEmpty else { return }
        staged.clear()
        let state = InstalledState(buildID: plan.buildID,
                                   manifests: Dictionary(uniqueKeysWithValues: plan.depots.map { (String($0.depotID), $0.manifest.manifestID) }),
                                   date: Date())
        try JSONEncoder().encode(state).write(to: stateURL, options: .atomic)
    }

    public func needsUpdate(_ plan: InstallPlan) -> Bool {
        guard let s = installed else { return true }
        return plan.depots.contains { s.manifests[String($0.depotID)] != $0.manifest.manifestID }
    }

    // MARK: Cloud

    /// Two-way Steam Cloud sync, the way the Steam client does it around a game session.
    /// Call before the game starts and again after it exits (or at the next launch). With `uploads`
    /// off it only downloads, and the report lists what would have gone up.
    public func syncSaves(uploads: Bool) async throws -> CloudSync.Report {
        let apps = SteamApps(cm: cm)
        let info = try await apps.appInfo(profile.appID)
        guard !info.cloudRules.isEmpty else { return CloudSync.Report() }
        let root = installRoot, profile = self.profile
        var sync = CloudSync(remote: SteamCloud(cm: cm, appID: profile.appID, buildID: info.buildID),
                             rules: info.cloudRules,
                             rootFolder: { token in profile.cloudRoots[token].map { root.appendingPathComponent($0) } },
                             localPath: profile.localPath,
                             stateURL: root.appendingPathComponent(".launcher-cloud.json"),
                             backupRoot: root.appendingPathComponent(".cloud-backups"))
        sync.uploads = uploads
        sync.neverUpload = profile.cloudNeverUpload
        return try await sync.run()
    }
}
