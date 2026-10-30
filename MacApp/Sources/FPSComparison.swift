// User-started, equal-length captures. Never changes the game scene or claims equivalent render settings.
import Foundation
import SwiftUI

struct FPSCapture: Codable, Identifiable {
    var id = UUID().uuidString
    var date = Date()
    var scene: String
    var backend: GameBackend
    var quality: String
    var headset: String
    var wired: Bool
    var macStage: String
    var macFPS: [Double]
    var headsetFPS: [Double]
    var latencyMS: [Double]
    var duration: Double
    var appBuild: String
    static func median(_ values: [Double]) -> Double? {
        let valid = values.filter { $0.isFinite && $0 >= 0 }.sorted()
        guard !valid.isEmpty else { return nil }
        return valid.count % 2 == 0 ? (valid[valid.count / 2 - 1] + valid[valid.count / 2]) / 2 : valid[valid.count / 2]
    }
    static var directory: URL { Paths.logs.appendingPathComponent("fps-comparisons") }
    static func load() -> [FPSCapture] {
        ((try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "json" }.compactMap { file in
                guard let data = try? Data(contentsOf: file) else { return nil }
                return try? JSONDecoder().decode(FPSCapture.self, from: data)
            }.sorted { $0.date > $1.date }
    }
}

extension Play {
    func recordFPS(scene: String) {
        let scene = String(scene.trimmingCharacters(in: .whitespacesAndNewlines).prefix(120))
        guard appID == "546560", phase == .playing, captureSeconds == nil, !scene.isEmpty else { return }
        captureMessage = nil; captureSeconds = 60
        let macStart = fpsSamples.count, headsetStart = headsetSamples.count, start = Date()
        Task { @MainActor [weak self] in
            guard let self else { return }
            for remaining in stride(from: 59, through: 0, by: -1) {
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard phase == .playing, running else { captureSeconds = nil; captureMessage = T("FPS capture stopped with the game. Start another capture in the scene you want to compare.", "ゲームとともに FPS 記録を停止しました。比較する場面で再度記録してください。"); return }
                captureSeconds = remaining
            }
            defer { captureSeconds = nil }
            let mac = Array(fpsSamples.dropFirst(macStart)), shown = Array(headsetSamples.dropFirst(headsetStart))
            guard mac.count >= 30 else { captureMessage = T("Not enough frame samples yet. Try again once the scene is running.", "フレームデータが足りません。場面が動作してから再度記録してください。"); return }
            let capture = FPSCapture(scene: scene, backend: backend, quality: quality.rawValue, headset: headset.rawValue, wired: captureWired,
                macStage: backend == .nativeARM64 ? "published stereo pairs" : "encoder captured stereo pairs",
                macFPS: mac, headsetFPS: shown.compactMap(\.fps), latencyMS: shown.compactMap(\.latency), duration: Date().timeIntervalSince(start), appBuild: AppBuild.description)
            do {
                try FileManager.default.createDirectory(at: FPSCapture.directory, withIntermediateDirectories: true)
                try JSONEncoder().encode(capture).write(to: FPSCapture.directory.appendingPathComponent(capture.id + ".json"), options: .atomic)
                captures = FPSCapture.load()
                captureMessage = T("FPS capture saved with the logs.", "FPS 記録をログとともに保存しました。")
            } catch { captureMessage = error.localizedDescription }
        }
    }
}

struct FPSComparisonView: View {
    var play: Play
    @State private var scene = ""
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(T("Compare Alyx FPS", "Alyx の FPS 比較")).font(.headline)
            Text(T("Use the same save, view, render resolution and stream settings in both modes. Record 60 seconds after loading and shader compilation. Mac counters measure different stages; headset FPS is the common output.", "両モードで同じセーブ・視点・描画解像度・配信設定を使い、読み込みとシェーダコンパイル後に 60 秒記録してください。Mac の計測段階は異なります。ヘッドセット FPS が共通の出力です。"))
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            HStack {
                TextField(T("Scene / save name", "場面・セーブ名"), text: $scene).textFieldStyle(.roundedBorder)
                if let seconds = play.captureSeconds { Text("\(seconds) s").monospacedDigit() }
                Button(T("Record 60 seconds", "60 秒記録")) { play.recordFPS(scene: scene) }
                    .disabled(scene.trimmingCharacters(in: .whitespaces).isEmpty || play.phase != .playing || play.captureSeconds != nil)
            }
            let matches = play.captures.filter { $0.scene == scene }
            let rows = GameBackend.allCases.compactMap { backend in matches.first { $0.backend == backend } }
            if !rows.isEmpty {
                Grid(alignment: .leading, horizontalSpacing: 16) {
                    GridRow { Text(T("Game launch mode", "ゲームの起動モード")); Text(T("fps on the Mac", "fps（Mac）")); Text(T("fps in the headset", "fps（ヘッドセット）")); Text(T("Picture", "画質")) }
                    ForEach(rows) { row in
                        GridRow {
                            Text(row.backend.name)
                            Text(FPSCapture.median(row.macFPS).map { String(format: "%.1f", $0) } ?? "—").help(row.macStage)
                            Text(FPSCapture.median(row.headsetFPS).map { String(format: "%.1f", $0) } ?? "—")
                            Text(row.quality + " / " + row.headset + (row.wired ? " / USB" : " / Wi-Fi"))
                        }
                    }
                }.font(.caption)
            }
            if let message = play.captureMessage { Text(message).font(.caption).foregroundStyle(.secondary) }
        }
        .onAppear { if scene.isEmpty { scene = play.captures.first?.scene ?? "" } }
    }
}
