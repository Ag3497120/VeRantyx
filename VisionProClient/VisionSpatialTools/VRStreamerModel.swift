import Combine
import Foundation

@MainActor
final class VRStreamerModel: ObservableObject {
    @Published private(set) var immersiveDiagnostic = "Compositorはまだ初期化されていません。"
    @Published private(set) var isImmersiveSpacePresented = false
    @Published private(set) var rendererStatus = "映像待ち"

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
    }

    func prepare() {
        guard !isPrepared else { return }
        isPrepared = true
        recordImmersiveEvent("Main app ready; waiting for the immersive start action.")
        receiver.start()
    }

    func recordRenderStatistics(_ statistics: VRRenderStatistics) {
        rendererStatus = "表示 \(statistics.renderFPS) fps／新しい映像 \(statistics.newFrameFPS) fps"
        receiver.sendClientDiagnostics(
            "receive_fps=\(receiver.receivedVideoFPS) decode_fps=\(decoder.decodedVideoFPS) " +
            "render_fps=\(statistics.renderFPS) new_frame_fps=\(statistics.newFrameFPS) " +
            "pose_age_ms=\(statistics.frameAgeMilliseconds) anchor=\(statistics.anchored) gpu_errors=\(statistics.gpuErrors)"
        )
    }

    func recordImmersiveEvent(_ message: String) {
        immersiveDiagnostic = message

        let timestamp = ISO8601DateFormatter().string(from: Date())
        let line = "\(timestamp) \(message)\n"
        guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            return
        }
        let logURL = documents.appendingPathComponent("immersive-diagnostics.log")
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
    }
}
