import Foundation
import GloveboxReports

/// Sends stutter reports (diagnostics/) when the player said yes. The answer is StutterSharing's: the newest of the
/// Mac installer's (built into alyx-env.txt) and the headset's (Documents/klfz/report-consent.json), which
/// ReportUploader reads the same way. One report per app launch, with what the last sessions recorded: the endpoint
/// keeps only a few a day (reports/ runs on a small free storage plan), so sending more would only be dropped. What
/// doesn't fit in one report goes at the next launch, and nothing goes when nothing is new or the answer isn't yes.
enum StutterReports {
    private static var started = false

    /// Sends this launch's report, once per app run.
    @MainActor static func start() {
        guard Brand.upstreamServices, !started else { return }
        started = true
        let docs = StutterSharing.consentFile.deletingLastPathComponent().deletingLastPathComponent()
        // off the main thread: a pass reads the pipeline recording and hashes its shaders
        Task.detached(priority: .utility) {
            if await ReportUploader(documents: docs).uploadIfAllowed(maxBatches: 1) { NSLog("[reports] sent what's new") }
        }
    }
}
