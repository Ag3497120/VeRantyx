import SwiftUI
import SteamLauncherKit
import GloveboxReports

/// Tells the player when a newer Glovebox is out. The headset app is built on their Mac, so the update comes from
/// there: the Mac app updates itself, then installs the new build on the headset (its Home page shows Install
/// Update). Checked when the app starts or comes back to the front, at most every few hours; the last answer is
/// kept, so the notice shows offline too and goes away by itself once the new build is installed.
enum GloveboxUpdateCheck {
    /// The release this app was built from (alyx-demo/glovebox.json, written by the Mac's build).
    static let installed: String? = GloveboxBuildInfo.load().glovebox
    static let interval: TimeInterval = 6 * 3600
    private static let latestKey = "gloveboxLatestRelease"
    private static let checkedKey = "gloveboxLatestReleaseCheckedAt"

    /// The newer release, from the last answer kept; nil when this app is the latest.
    static var known: String? {
        GloveboxRelease.newer(UserDefaults.standard.string(forKey: latestKey), than: installed)
    }

    /// Asks GitHub again when the last answer is older than `interval`. A failed ask keeps the last answer and is
    /// tried again next time.
    static func refresh() async -> String? {
        guard let installed, GloveboxRelease.isRelease(installed) else { return nil }
        let d = UserDefaults.standard
        if Date().timeIntervalSince1970 - d.double(forKey: checkedKey) >= interval,
           let latest = await GloveboxRelease.fetchLatest() {
            d.set(latest, forKey: latestKey)
            d.set(Date().timeIntervalSince1970, forKey: checkedKey)
        }
        return known
    }
}

/// "Update Available" on glass beside the Glovebox chip. It never stands between the player and Play: what to
/// do is one look away, in a popover.
struct GloveboxUpdateButton: View {
    let version: String
    @State private var showSteps = false

    var body: some View {
        Button { showSteps = true } label: {
            Label {
                Text("Update Available")
            } icon: {
                Image(systemName: "arrow.down.circle.fill")
                    .symbolRenderingMode(.palette)
                    .foregroundStyle(.black, Brand.orange)
            }
            .font(.title3.weight(.semibold))
            .padding(.leading, 12).padding(.trailing, 18)
            .frame(height: 44)
            .contentShape(.hoverEffect, .capsule)
            .hoverEffect(.highlight)
        }
        .buttonStyle(.plain)
        .glassBackgroundEffect(in: .capsule)
        .accessibilityLabel("Glovebox \(version) is available")
        .accessibilityHint("Shows how to update")
        .popover(isPresented: $showSteps, arrowEdge: .top) { GloveboxUpdateSteps(version: version) }
        .task {
            // The design-review screen with the popover open (DemoState).
            guard DemoState.current == .appupdatesteps else { return }
            try? await Task.sleep(nanoseconds: 1_000_000_000)
            showSteps = true
        }
    }
}

/// What to do, in the Mac app's own words: its menu item, then its button.
struct GloveboxUpdateSteps: View {
    let version: String

    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            HStack(spacing: 16) {
                Image(systemName: "laptopcomputer.and.arrow.down")
                    .font(.system(size: 34, weight: .medium))
                    .foregroundStyle(Brand.orange)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Glovebox \(version) is available").font(.title3.weight(.bold))
                    Text("Update from your Mac.").foregroundStyle(.secondary)
                }
            }
            VStack(alignment: .leading, spacing: 16) {
                step(1, "Open Glovebox and choose **Glovebox\u{00A0}›\u{00A0}Check\u{00A0}for\u{00A0}Updates**.")
                step(2, "Click **Install Update**.")
            }
            Label("Your game and saves stay.", systemImage: "checkmark.circle.fill")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
        .padding(32)
        .frame(width: 460, alignment: .leading)
    }

    private func step(_ n: Int, _ text: LocalizedStringKey) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 14) {
            Text("\(n)")
                .font(.subheadline.weight(.bold))
                .foregroundStyle(.black)
                .frame(width: 28, height: 28)
                .background(Brand.orange, in: .circle)
            Text(text).fixedSize(horizontal: false, vertical: true)
        }
        .font(.body)
    }
}
