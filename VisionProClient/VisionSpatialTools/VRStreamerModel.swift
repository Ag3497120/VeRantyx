import Combine
import Foundation

@MainActor
final class VRStreamerModel: ObservableObject {
    @Published private(set) var immersiveDiagnostic = String(localized: "Compositor not initialized yet.")
    @Published private(set) var isImmersiveSpacePresented = false
    @Published private(set) var rendererStatus = String(localized: "waiting for video")
    @Published var showsRealHands = false

    let receiver = VRUDPReceiver()
    let decoder = VRVideoDecoder()
    let tracking = VisionTrackingManager()

    private var frameSubscription: AnyCancellable?
    private var isPrepared = false
    private var isStreaming = false

    init() {
        decoder.requestKeyFrame = { [weak receiver] in
            receiver?.requestKeyFrame()
        }
        frameSubscription = receiver.framePublisher.sink { [weak decoder] frame in
            decoder?.decodeFrame(frame)
        }
        VRAudioPlayer.shared.log = { [weak self] message in
            Task { @MainActor in self?.appendDiagnosticsLog(message) }
        }
    }

    func prepare() {
        guard !isPrepared else { return }
        isPrepared = true
        recordImmersiveEvent("Main app ready; waiting for the immersive start action.")
        receiver.start()
    }

    func recordRenderStatistics(_ statistics: VRRenderStatistics) {
        rendererStatus = String(localized: "display \(statistics.renderFPS) fps / new frames \(statistics.newFrameFPS) fps / audio \(VRAudioPlayer.shared.status)")
        // The same numbers the in-game overlay shows (game, received, decode,
        // display, latency, audio), every 5 s, for the next session's log.
        let streamStats = VRStreamStatsStore.shared.summary
        let line = "decode_fps=\(decoder.decodedVideoFPS) " +
            "render_fps=\(statistics.renderFPS) new_frame_fps=\(statistics.newFrameFPS) " +
            "pose_age_ms=\(statistics.frameAgeMilliseconds) anchor=\(statistics.anchored) gpu_errors=\(statistics.gpuErrors) " +
            "\(statistics.eyeGeometry) \(receiver.diagnosticsSummary) \(decoder.diagnosticsSummary) " +
            "\(streamStats) \(tracking.latencyStatus)"
        appendDiagnosticsLog("Stream stats: \(line)")
        receiver.sendClientDiagnostics(line)
    }

    func recordImmersiveEvent(_ message: String) {
        immersiveDiagnostic = message
        appendDiagnosticsLog(message)
    }

    /// Appends one line to Documents/immersive-diagnostics.log without
    /// changing the Diagnostics row.
    func appendDiagnosticsLog(_ message: String) {
        let timestamp = Self.logTimestamp.string(from: Date())
        let line = "\(timestamp) \(message)\n"
        guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            return
        }
        let logURL = documents.appendingPathComponent("immersive-diagnostics.log")
        // A stats line every 5 s would otherwise grow without end: past 4 MB
        // the log moves to immersive-diagnostics.previous.log (one kept).
        if let size = (try? logURL.resourceValues(forKeys: [.fileSizeKey]))?.fileSize,
           size > Self.maxLogBytes {
            let previous = documents.appendingPathComponent("immersive-diagnostics.previous.log")
            try? FileManager.default.removeItem(at: previous)
            try? FileManager.default.moveItem(at: logURL, to: previous)
        }
        let data = Data(line.utf8)
        if FileManager.default.fileExists(atPath: logURL.path),
           let handle = try? FileHandle(forWritingTo: logURL) {
            defer { try? handle.close() }
            _ = try? handle.seekToEnd()
            try? handle.write(contentsOf: data)
        } else {
            try? data.write(to: logURL, options: .atomic)
        }
    }

    private static let logTimestamp = ISO8601DateFormatter()
    private static let maxLogBytes = 4 * 1024 * 1024

    func immersiveSpaceDidAppear() {
        isImmersiveSpacePresented = true
        recordImmersiveEvent("ImmersiveSpace appeared.")
    }

    func immersiveSpaceDidDisappear() {
        isImmersiveSpacePresented = false
        recordImmersiveEvent("ImmersiveSpace disappeared.")
        stop()
    }

    func start() async -> Bool {
        guard !isStreaming else { return true }
        prepare()
        isStreaming = true
        decoder.start()
        let trackingStarted = await tracking.start { [weak receiver] packet in
            receiver?.sendTrackingPacket(packet)
        }
        guard trackingStarted else {
            stop()
            return false
        }
        return true
    }

    func stop() {
        guard isPrepared || isStreaming else { return }
        isPrepared = false
        isStreaming = false
        tracking.stop()
        receiver.stop()
        decoder.stop()
        VRAudioPlayer.shared.stop()
    }
}
