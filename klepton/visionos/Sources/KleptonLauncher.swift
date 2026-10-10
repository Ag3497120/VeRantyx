// Shared "configure, then Start" launcher used by the game-facing builds (hl1,
// hl2, portal). The common part is the game-files source: a user-picked folder
// (on-device or iCloud Drive) fed to the guest with KL_FILES_DIR, with offloaded
// iCloud items downloaded first. Per-title extras live with each title (hl1's
// Xash panel in KleptonHL1.swift; hl2/portal's below, which is thin).
import SwiftUI
import Foundation
import UniformTypeIdentifiers

/// The build's target name, known from the FIRST render — before kl_app_configure
/// sets g_target (which only happens at boot). Using the plain kl_app_target_name()
/// here returns "(unconfigured)" until a boot runs, which left the launcher unable
/// to identify itself (and BootView capturing a dead fallback files object, so its
/// Start button never re-enabled). Always use this for launcher identity.
func klTargetName() -> String { String(cString: kl_app_target_name_or_default()) }

/// The launcher-facing title for the current build, or nil for a target that
/// just auto-boots (the runtime-debugging builds).
///
/// The "configure, then Start" launcher is OPT-IN at COMPILE TIME: only a build
/// generated with KL_CUSTOM_LAUNCHER set carries the `KL_CUSTOM_LAUNCHER` Swift
/// active-compilation condition (see visionos/gen_xcodeproj.py), and only then does
/// this return a title. A shipping build baked that way gets the launcher with no
/// runtime env; every other build — hl1/hl2/portal included — behaves the ORIGINAL
/// way (autoboot honouring KL_AUTOBOOT, a plain "Boot" button, no folder-picker
/// panel, the "Klepton" title). This is the single gate the whole launcher hangs
/// off (isLauncher, the panel, the autoboot suppression, "Boot"), so the
/// default build and existing scripts (visionos/run.sh) are untouched.
func klLauncherTitle() -> String? {
    #if KL_CUSTOM_LAUNCHER
    switch klTargetName() {
    case "hl1":    return "HL1VR"
    case "hl2":    return "HL2VR"
    case "portal": return "P1VR"
    default:       return nil
    }
    #else
    return nil
    #endif
}
func klIsLauncher() -> Bool { klLauncherTitle() != nil }

/// The game-files source, shared by every launcher title. Picks a folder, holds
/// its security scope for the run, remembers it across launches, materialises any
/// offloaded iCloud files with progress, and points the guest at it via
/// KL_FILES_DIR (kl_userdata_dir honours that env — no copy of the data is made).
@MainActor
final class LauncherFiles: ObservableObject {
    @Published var url: URL? = nil
    @Published var status = "No folder chosen — using the app's bundled data."
    @Published var progress: Double? = nil     // non-nil while materialising iCloud files
    @Published var ready = true                // false while a folder is being prepared
    @Published var staged = false              // a game folder already sits in the app container
    @Published var copyProgress: Double? = nil // non-nil while installing offline (copying in)
    @Published var forceChooser = false        // show the picker even though files are in-app

    private let key: String
    /// The folder the user is meant to pick, by name (e.g. "xash" for hl1) — used
    /// in the picker's label and to confirm what they chose.
    let expected: String
    /// Fired on the main actor when a chosen folder finishes preparing (hl1 uses
    /// it to (re)load the folder's commandline.txt).
    var onReady: (() -> Void)?

    init(bookmarkKey: String, expected: String) {
        key = bookmarkKey; self.expected = expected
        staged = stagedExists()          // a manually-staged (or previously installed) folder wins
        if !staged { restore() }
    }

    /// The default in-app location for this title's game folder. If it exists (a
    /// developer staged it, or the user chose "keep offline"), the normal staging
    /// path already works — no picker and no base-dir override are needed.
    var stagedURL: URL {
        URL(fileURLWithPath: Paths.container).appendingPathComponent("android-files/\(expected)")
    }
    private func stagedExists() -> Bool {
        ((try? FileManager.default.contentsOfDirectory(atPath: stagedURL.path))?.isEmpty == false)
    }
    /// The game root the guest should actually use: a prepared picked folder wins,
    /// otherwise the in-app staged folder, otherwise bundled (nil).
    var effectiveRoot: URL? { (url != nil && ready) ? url : (staged ? stagedURL : nil) }
    /// The base-dir OVERRIDE a title should setenv — only when the root is NOT the
    /// default in-app staged path (which the guest already reads without any env).
    var overrideRoot: URL? { let r = effectiveRoot; return (r != nil && r != stagedURL) ? r : nil }
    /// Whether to show the folder chooser (hidden once files are in-app, unless the
    /// person explicitly asked to replace them).
    var showChooser: Bool { !staged || forceChooser }
    /// Whether "keep offline" can run (a prepared folder is chosen, not mid-copy).
    var canInstallOffline: Bool { url != nil && ready && copyProgress == nil }

    func chose(_ u: URL) {
        _ = u.startAccessingSecurityScopedResource()
        url = u
        if let data = try? u.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
            UserDefaults.standard.set(data, forKey: key)
        }
        prepare()
    }

    func forget() {
        url?.stopAccessingSecurityScopedResource()
        url = nil; forceChooser = false
        UserDefaults.standard.removeObject(forKey: key)
        status = staged ? "Game files are installed in the app."
                        : "No folder chosen — using the app's bundled data."
        ready = true; progress = nil
        onReady?()
    }

    private func restore() {
        guard let data = UserDefaults.standard.data(forKey: key) else { return }
        var stale = false
        guard let u = try? URL(resolvingBookmarkData: data, options: [], relativeTo: nil,
                               bookmarkDataIsStale: &stale) else { return }
        _ = u.startAccessingSecurityScopedResource()
        url = u
        prepare()
    }

    /// Materialise the folder from iCloud (if offloaded) and load-check it. An
    /// offloaded folder is itself a placeholder whose children don't even list until
    /// it downloads — and sub-folders inside it are the same — so we kick a download
    /// on the folder AND on every not-downloaded item found, and re-scan as the tree
    /// fills in. Counts files rather than bytes: robust and enough for a progress bar.
    func prepare() {
        guard let root = url else { ready = true; return }
        ready = false; progress = 0; status = "Checking files…"
        let expected = self.expected      // captured by value for the detached task
        Task.detached { [weak self] in
            let fm = FileManager.default
            let name = root.lastPathComponent
            // Kick the folder placeholder itself — required, or its contents never list.
            try? fm.startDownloadingUbiquitousItem(at: root)

            // One pass over the (currently visible) tree: request a download on every
            // still-offloaded item (files AND sub-folders), and report file counts.
            func scan() -> (total: Int, done: Int, pending: Int) {
                var total = 0, done = 0, pending = 0
                guard let en = fm.enumerator(at: root,
                        includingPropertiesForKeys: [.isRegularFileKey, .ubiquitousItemDownloadingStatusKey]) else {
                    return (0, 0, 0)
                }
                for case let u as URL in en {
                    let rv = try? u.resourceValues(forKeys: [.isRegularFileKey, .ubiquitousItemDownloadingStatusKey])
                    let st = rv?.ubiquitousItemDownloadingStatus
                    let here = (st == nil || st == .current || st == .downloaded)
                    if !here { try? fm.startDownloadingUbiquitousItem(at: u); pending += 1 }
                    if rv?.isRegularFile == true { total += 1; if here { done += 1 } }
                }
                return (total, done, pending)
            }

            let mismatch = name.lowercased() != expected.lowercased()
            var emptyTries = 0
            for _ in 0..<600 {                         // up to ~10 min of downloading
                let (total, done, pending) = scan()
                let complete = total > 0 && pending == 0 && done == total
                await MainActor.run {
                    if complete {
                        self?.progress = nil
                        self?.status = mismatch
                            ? "Using \"\(name)\" (\(total) files) — expected a folder named `\(expected)`; use it only if this is your game root."
                            : "Ready — \(name), \(total) files."
                    } else if total == 0 && pending == 0 {
                        self?.progress = nil; self?.status = "Checking files…"
                    } else {
                        let of = max(total, done + pending)
                        self?.progress = of > 0 ? Double(done) / Double(of) : 0
                        self?.status = "Downloading from iCloud… \(done)/\(of)"
                    }
                }
                if complete { await MainActor.run { self?.ready = true; self?.onReady?() }; return }
                if total == 0 && pending == 0 {
                    emptyTries += 1
                    if emptyTries >= 3 {               // genuinely empty, not just still-listing
                        await MainActor.run {
                            self?.status = "\"\(name)\" is empty or unreadable. Pick your `\(expected)` folder."
                            self?.ready = false; self?.progress = nil
                        }
                        return
                    }
                } else { emptyTries = 0 }
                try? await Task.sleep(for: .seconds(1))
            }
            await MainActor.run {                       // ran out the clock, still downloading
                self?.status = "Still downloading from iCloud — press Boot again once it finishes."
                self?.ready = false; self?.progress = nil
            }
        }
    }

    /// Copy the chosen folder into the app container ("keep offline"), so the game
    /// runs with no dependency on the external/iCloud folder. Multi-GB, so file by
    /// file with count progress; on success `staged` flips true and the chooser hides.
    func installOffline() {
        guard let src = url, ready, !staged else { return }
        copyProgress = 0; status = "Installing into app…"
        let dst = stagedURL
        Task.detached { [weak self] in
            let fm = FileManager.default
            var files: [URL] = []
            if let en = fm.enumerator(at: src, includingPropertiesForKeys: [.isRegularFileKey]) {
                for case let u as URL in en {
                    if (try? u.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true { files.append(u) }
                }
            }
            let total = max(files.count, 1)
            try? fm.removeItem(at: dst)
            try? fm.createDirectory(at: dst, withIntermediateDirectories: true)
            let srcPath = src.path
            var done = 0, failed = false
            for f in files {
                let rel = String(f.path.dropFirst(srcPath.count).drop(while: { $0 == "/" }))
                let out = dst.appendingPathComponent(rel)
                try? fm.createDirectory(at: out.deletingLastPathComponent(), withIntermediateDirectories: true)
                do { try fm.copyItem(at: f, to: out) } catch { failed = true }
                done += 1
                if done % 8 == 0 || done == total {
                    let d = done
                    await MainActor.run { self?.copyProgress = Double(d) / Double(total); self?.status = "Installing into app… \(d)/\(total)" }
                }
            }
            await MainActor.run {
                self?.copyProgress = nil
                if failed { self?.status = "Some files could not be copied — try again, or run from the folder directly." }
                else { self?.staged = true; self?.status = "Installed in the app — \(total) files."; self?.onReady?() }
            }
        }
    }

    /// Drop the in-app copy and go back to choosing a folder.
    func removeStaged() {
        try? FileManager.default.removeItem(at: stagedURL)
        staged = false
        status = "Removed the in-app copy. Choose a folder to run from."
    }

    /// Ready to launch: game files are actually available — either installed in the
    /// app (staged), or a picked folder that has finished preparing. A launcher has no
    /// bundled data of its own, so "no folder chosen" is NOT launchable.
    var canLaunch: Bool { staged || (url != nil && ready) }
}

/// The "Game files" GroupBox, shared by all launcher titles.
struct LauncherFilesSection: View {
    @ObservedObject var files: LauncherFiles
    @State private var picking = false

    var body: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                if !files.showChooser {
                    // Files already live in the app (a developer staged them, or a
                    // previous "keep offline") — nothing to pick.
                    HStack {
                        Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                        Text("Game files are installed in the app.")
                        Spacer()
                        Button { files.forceChooser = true } label: {
                            Label("Replace…", systemImage: "arrow.triangle.2.circlepath")
                        }.font(.caption)
                    }
                    Text(files.status).font(.caption).foregroundStyle(.secondary)
                } else {
                    HStack {
                        Button { picking = true } label: {
                            Label(files.url == nil ? "Choose your `\(files.expected)` folder…" : "Change folder…",
                                  systemImage: "folder")
                        }
                        // Only offered while replacing already-installed files — a
                        // launcher has no bundled data to fall back to, so there is no
                        // "use bundled"; to drop a wrong pick, just Change folder.
                        if files.url != nil && files.staged {
                            Button(role: .destructive) { files.forget() } label: {
                                Label("Cancel", systemImage: "xmark")
                            }
                        }
                        Spacer()
                        if let p = files.progress { ProgressView(value: p).frame(width: 120) }
                        else if files.ready && files.url != nil {
                            Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                        }
                    }
                    Text(files.status).font(.caption).foregroundStyle(.secondary)
                    if let cp = files.copyProgress {
                        HStack {
                            ProgressView(value: cp).frame(width: 160)
                            Text("Keeping offline…").font(.caption).foregroundStyle(.secondary)
                        }
                    } else if files.canInstallOffline {
                        Button { files.installOffline() } label: {
                            Label("Keep offline (copy into the app)", systemImage: "square.and.arrow.down")
                        }.font(.caption)
                    }
                }
            }
        } label: { Label("Game files", systemImage: "externaldrive") }
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { r in
            if case .success(let u) = r { files.chose(u) }
        }
    }
}

/// hl2 / portal launcher: just the game-files source. Their whole rendering
/// config (hl2's multiview/two-pass/lowmem set, portal's) is the validated default
/// and is deliberately NOT exposed — a chosen folder is all these need.
@MainActor
final class SourceLauncher: ObservableObject {
    static let shared = SourceLauncher()
    let files: LauncherFiles

    private init() {
        let t = klTargetName()
        // The Source content root is named per game: Portal ships "Source", HL2 "srceng".
        files = LauncherFiles(bookmarkKey: "\(t).filesBookmark",
                              expected: t == "portal" ? "Source" : "srceng")
    }

    @discardableResult
    func apply() -> Bool {
        guard files.canLaunch else { return false }
        // Override the Source content root only for an external folder — the in-app
        // staged path is the default and needs no override.
        if let r = files.overrideRoot { setenv("KL_SOURCE_DATA_PATH", r.path, 1) }
        return true
    }
}

struct SourcePanel: View {
    @ObservedObject var s = SourceLauncher.shared
    var body: some View { LauncherFilesSection(files: s.files) }
}

// ---- dispatch: BootView talks to these, not to a specific title's type --------

/// The files object of whichever launcher this build is — touching only the one
/// singleton that target needs (never both, so a bookmark is restored once).
@MainActor func klActiveFiles() -> LauncherFiles? {
    switch klTargetName() {
    case "hl1":           return HL1Settings.shared.files
    case "hl2", "portal": return SourceLauncher.shared.files
    default:              return nil
    }
}

/// Fold this build's launcher settings into env/files; false if not launch-ready.
@MainActor func klLauncherApply() -> Bool {
    switch klTargetName() {
    case "hl1":           return HL1Settings.shared.apply()
    case "hl2", "portal": return SourceLauncher.shared.apply()
    default:              return true
    }
}

/// The right settings panel for this build.
struct LauncherPanel: View {
    var body: some View {
        switch klTargetName() {
        case "hl1": HL1Panel()
        default:    SourcePanel()     // hl2 / portal
        }
    }
}
