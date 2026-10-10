import Foundation
import GloveboxReports

// reportctl <recorded.foz> <slow.txt> <out dir> [--upload]: build the reports a headset would send from these files
// (state kept in <out dir>), write each as <out dir>/report-<seq>.json, and with --upload also send them.
let a = CommandLine.arguments
guard a.count >= 4 else { print("usage: reportctl <recorded.foz> <slow.txt> <out dir> [--upload]"); exit(2) }
let out = URL(fileURLWithPath: a[3])
let stateURL = out.appendingPathComponent("state.json")
var state = ReportState.load(stateURL)
while true {
    // prewarm.log beside the hitch log, as on the headset
    let prewarm = URL(fileURLWithPath: a[2]).deletingLastPathComponent().appendingPathComponent("prewarm.log")
    let (r, next) = try ReportBuilder.next(recording: URL(fileURLWithPath: a[1]), slowLog: URL(fileURLWithPath: a[2]),
                                           prewarmLog: prewarm, state: state, app: .init(glovebox: "reportctl", kit: "dev"),
                                           device: .init(model: ReportUploader.model, os: ReportUploader.osVersion))
    if r.isEmpty { break }
    let body = try ReportBuilder.body(r)
    let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys]
    try enc.encode(r).write(to: out.appendingPathComponent("report-\(r.seq).json"))
    print("report \(r.seq): \(r.entries.count) entries, \(r.modules.count) modules, \(r.hitches.count) hitches, \(r.prep.count) prep, \(body.count) bytes")
    if a.contains("--upload") {
        var req = URLRequest(url: URL(string: "https://glovebox-reports.vercel.app/api/report")!)
        req.httpMethod = "POST"; req.httpBody = body
        req.setValue("application/x-glovebox-report", forHTTPHeaderField: "Content-Type")
        let (_, resp) = try await URLSession.shared.data(for: req)
        print("  upload: \((resp as? HTTPURLResponse)?.statusCode ?? -1)")
    }
    state = next
    try state.save(stateURL)
}
