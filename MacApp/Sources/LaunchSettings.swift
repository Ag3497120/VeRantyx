// One application preference; each title resolves it against its declared capabilities.
import Foundation

struct LaunchPreferences {
    var backend: GameBackend
    var quality: Quality

    static func load(_ values: [String: Any]) -> LaunchPreferences {
        let saved = (values["preferredBackend"] as? String).flatMap(GameBackend.init(rawValue:))
        let legacy = (values["gameBackends"] as? [String: String])?[NativeGameSupport.alyx.appID].flatMap(GameBackend.init(rawValue:))
        let backend = saved ?? legacy ?? .vxrt
        let qualityKey = saved == nil && backend == .nativeARM64 ? "nativeAlyxQuality" : "quality"
        let quality = (values[qualityKey] as? String).flatMap(Quality.init(rawValue:)) ?? .standard
        return LaunchPreferences(backend: backend, quality: backend == .nativeARM64 && ![.standard, .sharp].contains(quality) ? .standard : quality)
    }
}

struct GameInstallRequest: Equatable {
    var id: String
    var backend: GameBackend
}

extension AppModel {
    var effectivePreference: GameBackend { ProcessInfo.processInfo.environment["VERANTYX_BACKEND"].flatMap(GameBackend.init(rawValue:)) ?? preferredBackend }
    var configurationLocked: Bool { play?.running == true || job != nil || flow != nil || setupDownloading }
    func chooseBackend(_ backend: GameBackend) {
        guard !configurationLocked else { return }
        preferredBackend = backend
        if !sessionQualityChoices.contains(quality) { quality = .standard }
    }
    func backend(forID id: String) -> GameBackend { Title.named(id)?.backend(preferred: effectivePreference) ?? .vxrt }
    var sessionQualityChoices: [Quality] { effectivePreference == .nativeARM64 ? [.standard, .sharp] : Quality.choices(for: headset) }
    var sessionQualityDetail: String {
        if effectivePreference == .nativeARM64 {
            return quality == .sharp
                ? T("Native ARM64: about 2.2 million rendered pixels per eye; 2880×1620 stream, 150 Mbps. Windows games use their Windows preset.", "ネイティブ ARM64：片目約 220 万画素、配信 2880×1620・150 Mbps。Windows ゲームは Windows 用の設定です。")
                : T("Native ARM64: about 1 million rendered pixels per eye; 2880×1040 stream, 40 Mbps. Windows games use their Windows preset.", "ネイティブ ARM64：片目約 100 万画素、配信 2880×1040・40 Mbps。Windows ゲームは Windows 用の設定です。")
        }
        return quality.detail
    }
    func summary(for title: Title) -> String { backend(forID: title.id) == .nativeARM64 ? title.nativeSupport?.summary ?? title.summary : title.summary }
    func nativeReason(for title: Title) -> String? {
        guard let support = title.nativeSupport else { return nil }
        if !support.layout(drive: drive, runtime: Paths.runtime).bundled { return T("Rebuild VeRantyx with its native components included.", "ネイティブ部品を含めて VeRantyx を再ビルドしてください。") }
        if !isInstalled(title.id, backend: .nativeARM64) { return nativeInstallProblems[title.id] ?? T("Download the native version in this app first.", "先にこのアプリでネイティブ版をダウンロードしてください。") }
        if route(forID: title.id) != .alvr { return T("Native ARM64 uses ALVR. Choose ALVR on the Headset page or this game's route.", "ネイティブ ARM64 は ALVR を使います。ヘッドセットページかゲームの経路で ALVR を選んでください。") }
        return nil
    }
    var setupTitles: [Title] { Title.all.filter { $0.id != Title.steamVR } }
    var primarySetupTitles: [Title] { setupTitles.filter { effectivePreference == .vxrt || $0.nativeSupport != nil } }
    var setupNeedsWindows: Bool {
        effectivePreference == .vxrt || setupTitles.contains { (wanted.contains($0.id) || setupFocusID == $0.id) && backend(forID: $0.id) == .vxrt }
    }
    var setupGamesDone: Bool {
        primarySetupTitles.contains { isInstalled($0.id) } && (effectivePreference != .vxrt || steamVR)
    }
    var setupComponentsReady: Bool {
        if effectivePreference == .vxrt { return redistCount > 0 }
        return !primarySetupTitles.isEmpty && primarySetupTitles.allSatisfy { $0.nativeSupport?.layout(drive: drive, runtime: Paths.runtime).bundled == true }
            && primarySetupTitles.contains { route(forID: $0.id) == .alvr }
    }
    var launchModeReady: Bool { effectivePreference != .vxrt || rosetta == true }
    var setupChecks: [Bool] { [appleSilicon && macOSOK, launchModeReady, steamInstalled && steamRunning, freeBytes != nil, setupGamesDone, setupComponentsReady] }
    var playSetupReady: Bool { effectivePreference == .vxrt ? windowsRuntimeReady : appleSilicon && macOSOK && steamInstalled && setupComponentsReady }
    func showInstallation(_ title: Title) {
        setupFocusID = title.id
        if title.source == .steamcmd || backend(forID: title.id) == .nativeARM64 { wanted.insert(title.id) }
        page = .setup
    }
    func estimatedBytes(for title: Title) -> Int64 {
        guard backend(forID: title.id) == .nativeARM64 else { return title.bytes }
        let cached = title.nativeSupport?.cachedContentAvailable(drive: drive, runtime: Paths.runtime) == true
        let copies: Int64 = nativeCanClone ? (isInstalled(title.id, backend: .vxrt) || cached ? 0 : 1) : 2
        return copies * title.bytes + (2 << 30)
    }
    func installationPlan(for ids: Set<String>) -> [GameInstallRequest] {
        let selected = setupTitles.filter { ids.contains($0.id) }
        var plan: [GameInstallRequest] = []
        if selected.contains(where: { backend(forID: $0.id) == .vxrt }), !steamVR { plan.append(GameInstallRequest(id: Title.steamVR, backend: .vxrt)) }
        for title in selected where title.source != .windowsSteam || backend(forID: title.id) == .nativeARM64 {
            let backend = backend(forID: title.id)
            if !isInstalled(title.id, backend: backend) { plan.append(GameInstallRequest(id: title.id, backend: backend)) }
        }
        return plan
    }
}
