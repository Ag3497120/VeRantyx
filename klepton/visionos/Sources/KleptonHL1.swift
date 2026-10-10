// Half-Life VR (hl1) launcher panel — the Xash/Lambda1VR-specific parts. The
// shared game-files source (folder picker + iCloud) lives in KleptonLauncher.swift
// (LauncherFiles); this adds hl1's own knobs: an editable commandline.txt and the
// Xash VR settings Klepton reads from the environment (KL_XASH_*).
import SwiftUI
import Foundation

/// True only for the Half-Life build.
func klIsHL1() -> Bool { klTargetName() == "hl1" }

@MainActor
final class HL1Settings: ObservableObject {
    static let shared = HL1Settings()

    /// Shared folder/iCloud source. The user points this at their `xash` folder
    /// (valve/, HL_Gold_HD/, commandline.txt); hl1 also reads its commandline.txt.
    let files = LauncherFiles(bookmarkKey: "hl1.filesBookmark", expected: "xash")

    // ---- commandline.txt (editable override; see apply()) ----
    @Published var commandline = ""

    // ---- VR comfort (-> KL_XASH_CVARS / userconfig) ----
    @Published var smoothTurn   = true
    @Published var turnSpeed    = 5.0     // vr_turn_angle when smooth (< 10)
    @Published var snapAngle    = 45.0    // vr_turn_angle when snap (>= 10)
    @Published var heightAdjust = -1.5    // vr_height_adjust, metres
    @Published var vignette     = false   // vr_comfort_mask

    // ---- controls ----
    @Published var gripEnabled = true              // right Options button
    @Published var gripCmd     = "changelevel c1a0"
    @Published var menuEnabled = true              // left Create -> game menu

    // ---- performance (-> commandline.txt via KL_XASH_SUPERSAMPLING / _MSAA) ----
    @Published var supersampling = 1.0
    @Published var msaa = 2

    private init() {
        guard klIsHL1() else { return }
        files.onReady = { [weak self] in self?.seedCommandline() }
        load()                       // restores a previously edited commandline
        seedCommandline()            // only fills a blank one (from the folder / default)
    }

    // A writable in-app file the guest is pointed at (KL_XASH_COMMANDLINE_FILE), so the
    // edited commandline works even when the picked game folder is read-only.
    private var commandlineFile: URL {
        URL(fileURLWithPath: Paths.container).appendingPathComponent("klepton-hl1-commandline.txt")
    }
    // The game root's own commandline.txt, used only to pre-fill the editor first time.
    private var commandlineURL: URL? { files.effectiveRoot?.appendingPathComponent("commandline.txt") }

    /// Fill the editor only when it is still empty — never clobber the user's edits.
    private func seedCommandline() {
        guard commandline.isEmpty else { return }
        if let u = commandlineURL, let s = try? String(contentsOf: u, encoding: .utf8),
           !s.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            commandline = s.trimmingCharacters(in: .whitespacesAndNewlines)
        } else {
            commandline = "xash3d -log -game HL_Gold_HD"
        }
    }

    /// Fold every setting into where the guest reads it, then report readiness.
    @discardableResult
    func apply() -> Bool {
        guard files.canLaunch else { return false }
        // Override the Xash base only for an external folder — the in-app staged
        // path is the default and needs no override.
        if let r = files.overrideRoot { setenv("KL_XASH_BASEDIR", r.path, 1) }

        // Write the (possibly edited) commandline to a writable in-app file and point
        // the guest at it — works even if the game folder itself is read-only.
        let cl = commandline.trimmingCharacters(in: .whitespacesAndNewlines)
        try? (cl + "\n").write(to: commandlineFile, atomically: true, encoding: .utf8)
        setenv("KL_XASH_COMMANDLINE_FILE", commandlineFile.path, 1)

        let angle = smoothTurn ? turnSpeed : snapAngle
        let cvars = "vr_smoothturn \(smoothTurn ? 1 : 0); vr_turn_angle \(fmt(angle)); "
                  + "vr_height_adjust \(fmt(heightAdjust)); vr_comfort_mask \(vignette ? 1 : 0)"
        setenv("KL_XASH_CVARS", cvars, 1)

        setenv("KL_XASH_GRIP_CMD", gripEnabled ? gripCmd : "", 1)   // empty disables (no-op cmd)
        setenv("KL_XASH_MENU_CMD", menuEnabled ? "escape" : "", 1)

        setenv("KL_XASH_SUPERSAMPLING", fmt(supersampling), 1)
        setenv("KL_XASH_MSAA", String(msaa), 1)

        save()
        return true
    }

    private func fmt(_ v: Double) -> String { String(format: "%g", v) }

    private func save() {
        let d = UserDefaults.standard
        d.set(smoothTurn, forKey: "hl1.smoothTurn"); d.set(turnSpeed, forKey: "hl1.turnSpeed")
        d.set(snapAngle, forKey: "hl1.snapAngle");   d.set(heightAdjust, forKey: "hl1.heightAdjust")
        d.set(vignette, forKey: "hl1.vignette");     d.set(gripEnabled, forKey: "hl1.gripEnabled")
        d.set(gripCmd, forKey: "hl1.gripCmd");        d.set(menuEnabled, forKey: "hl1.menuEnabled")
        d.set(supersampling, forKey: "hl1.supersampling"); d.set(msaa, forKey: "hl1.msaa")
        d.set(commandline, forKey: "hl1.commandline")
    }
    private func load() {
        let d = UserDefaults.standard
        commandline = d.string(forKey: "hl1.commandline") ?? ""
        guard d.object(forKey: "hl1.smoothTurn") != nil else { return }   // first run: defaults
        smoothTurn = d.bool(forKey: "hl1.smoothTurn"); turnSpeed = d.double(forKey: "hl1.turnSpeed")
        snapAngle = d.double(forKey: "hl1.snapAngle"); heightAdjust = d.double(forKey: "hl1.heightAdjust")
        vignette = d.bool(forKey: "hl1.vignette");     gripEnabled = d.bool(forKey: "hl1.gripEnabled")
        gripCmd = d.string(forKey: "hl1.gripCmd") ?? gripCmd; menuEnabled = d.bool(forKey: "hl1.menuEnabled")
        supersampling = d.double(forKey: "hl1.supersampling"); msaa = d.integer(forKey: "hl1.msaa")
        if msaa == 0 { msaa = 1 }   // migrate the old "Off" (0) to 1x (1 sample)
    }
}

/// The hl1 launcher sections (shared files section + Xash knobs).
struct HL1Panel: View {
    @ObservedObject var s = HL1Settings.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            LauncherFilesSection(files: s.files)

            DisclosureGroup {
                VStack(alignment: .leading, spacing: 6) {
                    Text("The engine launch line. Overrides the game folder's own "
                         + "commandline.txt (the folder can stay read-only) and picks "
                         + "the mod via -game.")
                        .font(.caption).foregroundStyle(.secondary)
                    TextEditor(text: $s.commandline)
                        .font(.system(.caption, design: .monospaced))
                        .frame(minHeight: 60, maxHeight: 110)
                        .overlay(RoundedRectangle(cornerRadius: 6).stroke(.secondary.opacity(0.3)))
                }.font(.callout).padding()
            } label: { Label("commandline.txt", systemImage: "terminal") }.font(.callout)

            DisclosureGroup {
                VStack(alignment: .leading, spacing: 6) {
                    Toggle("Smooth turning (off = snap)", isOn: $s.smoothTurn)
                    if s.smoothTurn { slider("Turn speed", $s.turnSpeed, 1...9, "%g") }
                    else            { slider("Snap angle", $s.snapAngle, 15...90, "%.0f°") }
                    slider("Height offset (m)", $s.heightAdjust, -2.5...0.5, "%.2f")
                    Toggle("Comfort vignette", isOn: $s.vignette)
                }.font(.callout).padding()
            } label: { Label("VR comfort", systemImage: "figure.stand") }.font(.callout)

            DisclosureGroup {
                VStack(alignment: .leading, spacing: 6) {
                    Toggle("Right Options button", isOn: $s.gripEnabled)
                    if s.gripEnabled {
                        TextField("Command (e.g. changelevel c1a0)", text: $s.gripCmd)
                            .font(.system(.caption, design: .monospaced)).textFieldStyle(.roundedBorder)
                    }
                    Toggle("Left Create button opens the game menu", isOn: $s.menuEnabled)
                }.font(.callout).padding()
            } label: { Label("Controls", systemImage: "gamecontroller") }.font(.callout)

            DisclosureGroup {
                VStack(alignment: .leading, spacing: 6) {
                    slider("Supersampling", $s.supersampling, 0.7...1.5, "%.2fx")
                    Picker("MSAA", selection: $s.msaa) {
                        Text("1x").tag(1); Text("2x").tag(2); Text("4x").tag(4)
                    }.pickerStyle(.segmented)
                    Text("Multisample anti-aliasing — smooths jagged edges at a GPU cost. "
                         + "1x is off; 4x is the smoothest and heaviest.")
                        .font(.caption).foregroundStyle(.secondary)
                }.font(.callout).padding()
            } label: { Label("Performance", systemImage: "speedometer") }.font(.callout)
        }
    }

    @ViewBuilder private func slider(_ title: String, _ v: Binding<Double>,
                                     _ range: ClosedRange<Double>, _ fmt: String) -> some View {
        HStack {
            Text(title).frame(width: 130, alignment: .leading)
            Slider(value: v, in: range)
            Text(String(format: fmt, v.wrappedValue)).frame(width: 52, alignment: .trailing)
                .font(.caption.monospacedDigit())
        }
    }
}
