import Foundation
import Network
import Combine
import simd

struct VRCompressedFrame {
    let streamGeneration: UInt64
    let sequence: UInt32
    let timestamp: Double
    let headTransform: simd_float4x4
    let data: Data
}

/// Numbers for the in-game fps overlay and the diagnostics log. Written from
/// the network, audio and tracking threads; read once a second by the render
/// thread and by the main actor.
final class VRStreamStatsStore {
    static let shared = VRStreamStatsStore()

    struct Snapshot {
        /// From the Mac's "STAT" packet. `gameFPS` is the rate at which the
        /// game produced new frames (capture); -1 until the first packet.
        var gameFPS: Float = -1
        var encodedFPS: Float = -1
        var sentFPS: Float = -1
        var bitrateMbps: Float = 0
        var macAudioPacketsPerSecond: Float = 0
        var statsReceivedAt: TimeInterval = 0
        /// Complete video frames received per second (VRUDPReceiver).
        var receivedFPS = 0
        /// Pose-to-display latency from VRPoseSampler; -1 before video shows.
        var latencyMilliseconds = -1
        /// VRAudioPlayer counters.
        var audioReceived = 0
        var audioPlayed = 0
        var audioDropped = 0
        var audioConcealed = 0
        var audioUnderruns = 0
        var audioRestarts = 0
        var audioPacketsPerSecond = 0
        var audioEngineRunning = false
        var audioLastError = ""
    }

    private let lock = NSLock()
    private var current = Snapshot()

    func update(_ change: (inout Snapshot) -> Void) {
        lock.lock()
        change(&current)
        lock.unlock()
    }

    func snapshot() -> Snapshot {
        lock.lock()
        defer { lock.unlock() }
        return current
    }

    /// One line for CLIENT_STATS and the diagnostics log.
    var summary: String {
        let s = snapshot()
        let latency = s.latencyMilliseconds >= 0 ? "\(s.latencyMilliseconds)" : "-1"
        return String(format: "game_fps=%.1f encoded_fps=%.1f sent_fps=%.1f bitrate_mbps=%.1f receive_fps=%d latency_ms=%@ ",
                      s.gameFPS, s.encodedFPS, s.sentFPS, s.bitrateMbps, s.receivedFPS, latency) +
            "audio_rx=\(s.audioReceived) audio_rx_rate=\(s.audioPacketsPerSecond)/s mac_audio_rate=\(Int(s.macAudioPacketsPerSecond))/s " +
            "audio_played=\(s.audioPlayed) audio_dropped=\(s.audioDropped) audio_concealed=\(s.audioConcealed) " +
            "audio_underruns=\(s.audioUnderruns) audio_restarts=\(s.audioRestarts) audio_engine=\(s.audioEngineRunning ? "running" : "stopped")" +
            (s.audioLastError.isEmpty ? "" : " audio_error=\(s.audioLastError)")
    }
}

/// Discovers the Mac encoder and reassembles its 96-byte VRAN HEVC fragment packets.
final class VRUDPReceiver: ObservableObject {
    let framePublisher = PassthroughSubject<VRCompressedFrame, Never>()

    @Published private(set) var connectionStatus = String(localized: "Searching for the Mac encoder…")
    @Published private(set) var receivedVideoFPS = 0
    @Published private(set) var gameFrameStatus = String(localized: "waiting for the Mac's frame rate")
    @Published private(set) var trackingPacketsSent = 0
    @Published private(set) var receiveDiagnostics = String(localized: "Waiting for video packets.")
    @Published private(set) var diagnosticsSummary = "packets=0 incomplete=0"

    private var browser: NWBrowser?
    private var connection: NWConnection?
    private let networkQueue = DispatchQueue(label: "com.verantyx.vrudp-receiver", qos: .userInteractive)
    // Pose packets leave from the render thread. Sending them through
    // networkQueue made them wait behind keyframe reassembly, which arrived at
    // SteamVR as head-pose jitter. NWConnection.send is thread-safe.
    private let trackingLock = NSLock()
    private var trackingConnection: NWConnection?
    private var trackingSentCount = 0
    private var shouldRun = false
    private var didReceiveVideo = false
    private var videoWasStale = false
    private var lastVideoReceivedAt: TimeInterval?
    private var connectionReadyAt: TimeInterval?
    private var watchdogGeneration: UInt64 = 0
    private var fpsWindowStart = ProcessInfo.processInfo.systemUptime
    private var fpsWindowFrames = 0
    private var streamGeneration: UInt64 = 0
    private var receivedPacketCount = 0
    private var incompleteFrameCount = 0
    private var lastPacketReceivedAt: TimeInterval?
    private var lastKeyFrameRequestAt: TimeInterval = 0

    private struct Fragment {
        let offset: Int
        let data: Data
    }

    // Reference semantics avoid copying the entire fragment dictionary for
    // every incoming datagram (especially expensive for a large keyframe).
    private final class FrameAssembly {
        let expectedChunks: Int
        let createdAt: TimeInterval
        let timestamp: Double
        let headTransform: simd_float4x4
        var chunks: [UInt32: Fragment] = [:]
        var byteCount = 0

        init(expectedChunks: Int, createdAt: TimeInterval, timestamp: Double,
             headTransform: simd_float4x4) {
            self.expectedChunks = expectedChunks
            self.createdAt = createdAt
            self.timestamp = timestamp
            self.headTransform = headTransform
        }
    }

    private var frames: [UInt32: FrameAssembly] = [:]
    private var lastPlayedFrame: UInt32?

    private let headerSize = 96 // 24-byte VRAN header, 8-byte timestamp, 64-byte head transform
    private let maxFrameBytes = 32 * 1024 * 1024
    private let maxChunks = 32_768
    private let maxPendingFrames = 8
    // The sender allows up to 750 ms for a large first IDR. Its fragments
    // must survive that burst; the decode FIFO has a separate 100 ms bound.
    private let maxAssemblyAge: TimeInterval = 1.0

    func start() {
        networkQueue.async { [weak self] in
            guard let self else { return }
            guard !self.shouldRun else { return }
            self.shouldRun = true
            self.beginBrowsing()
            self.watchdogGeneration &+= 1
            self.scheduleVideoWatchdog(generation: self.watchdogGeneration)
        }
    }

    private func beginBrowsing() {
        guard shouldRun, browser == nil, connection == nil else { return }
        didReceiveVideo = false
        videoWasStale = false
        lastVideoReceivedAt = nil
        connectionReadyAt = nil
        fpsWindowStart = ProcessInfo.processInfo.systemUptime
        fpsWindowFrames = 0
        receivedPacketCount = 0
        incompleteFrameCount = 0
        lastPacketReceivedAt = nil
        lastKeyFrameRequestAt = 0
        DispatchQueue.main.async { [weak self] in
            self?.connectionStatus = String(localized: "Searching for the Mac encoder…")
            self?.receivedVideoFPS = 0
            self?.gameFrameStatus = String(localized: "waiting for the Mac's frame rate")
            self?.trackingPacketsSent = 0
            self?.receiveDiagnostics = String(localized: "Waiting for video packets.")
        }

        let descriptor = NWBrowser.Descriptor.bonjour(type: "_verantyxvr._udp", domain: "local.")
        let parameters = NWParameters.udp
        parameters.allowFastOpen = true
        parameters.includePeerToPeer = true

        let newBrowser = NWBrowser(for: descriptor, using: parameters)
        newBrowser.stateUpdateHandler = { [weak self] state in
            guard let self, self.browser === newBrowser else { return }
            print("VRUDPBrowser State: \(state)")
            if case .failed(let error) = state {
                self.browserFailed(error)
            }
        }
        newBrowser.browseResultsChangedHandler = { [weak self] results, _ in
            guard let self, self.shouldRun, self.browser === newBrowser,
                  self.connection == nil, let result = results.first else { return }
            print("Found Mac VR service: \(result.endpoint)")
            self.browser?.cancel()
            self.browser?.stateUpdateHandler = nil
            self.browser?.browseResultsChangedHandler = nil
            self.browser = nil
            self.connect(to: result.endpoint)
        }

        browser = newBrowser
        newBrowser.start(queue: networkQueue)
        print("Browsing for Mac HardwareEncoder (_verantyxvr._udp)...")
    }

    private func connect(to endpoint: NWEndpoint) {
        let parameters = NWParameters.udp
        parameters.allowFastOpen = true
        parameters.includePeerToPeer = true

        let newConnection = NWConnection(to: endpoint, using: parameters)
        newConnection.stateUpdateHandler = { [weak self] state in
            guard let self, self.shouldRun, self.connection === newConnection else { return }
            print("VRUDPConnection State: \(state)")
            if state == .ready {
                self.streamGeneration &+= 1
                self.frames.removeAll()
                self.lastPlayedFrame = nil
                self.didReceiveVideo = false
                self.videoWasStale = false
                self.lastVideoReceivedAt = nil
                self.connectionReadyAt = ProcessInfo.processInfo.systemUptime
                self.fpsWindowStart = self.connectionReadyAt ?? ProcessInfo.processInfo.systemUptime
                self.fpsWindowFrames = 0
                // The Mac uses this datagram's source address as the video destination.
                newConnection.send(content: Data("HELLO_VR".utf8), completion: .idempotent)
                self.setTrackingConnection(newConnection)
                DispatchQueue.main.async { [weak self] in
                    self?.connectionStatus = String(localized: "Connected to the Mac. Waiting for video from the encoder.")
                }
            } else if case .failed(let error) = state {
                self.connectionFailed(newConnection, error: error)
            }
        }

        connection = newConnection
        newConnection.start(queue: networkQueue)
        receive(on: newConnection)
    }

    func stop() {
        networkQueue.async { [weak self] in
            guard let self else { return }
            self.shouldRun = false
            self.browser?.cancel()
            self.browser?.stateUpdateHandler = nil
            self.browser?.browseResultsChangedHandler = nil
            self.browser = nil
            self.connection?.cancel()
            self.connection?.stateUpdateHandler = nil
            self.connection = nil
            self.setTrackingConnection(nil)
            self.watchdogGeneration &+= 1
            self.frames.removeAll()
            self.lastPlayedFrame = nil
            self.didReceiveVideo = false
            self.videoWasStale = false
            self.lastVideoReceivedAt = nil
            self.connectionReadyAt = nil
            self.fpsWindowFrames = 0
            DispatchQueue.main.async {
                self.connectionStatus = String(localized: "Stopped")
            }
        }
    }

    /// A lost HEVC reference frame must be repaired with an IDR, rather than
    /// feeding damaged dependent frames into VideoToolbox indefinitely.
    func requestKeyFrame() {
        networkQueue.async { [weak self] in
            self?.requestKeyFrameOnQueue()
        }
    }

    /// The caller samples UI/render counters at most once every five seconds.
    func sendClientDiagnostics(_ message: String) {
        // Keep the whole datagram below the stream's 1196-byte payload budget,
        // including when the caller supplies multibyte or composed characters.
        var bounded = ""
        var byteCount = 0
        for character in message.prefix(1100) {
            let value = String(character)
            let size = value.utf8.count
            guard byteCount + size <= 1100 else { break }
            bounded.append(character)
            byteCount += size
        }
        let packet = Data(("CLIENT_STATS " + bounded).utf8)
        networkQueue.async { [weak self] in
            guard let self, self.shouldRun, let connection = self.connection,
                  connection.state == .ready else { return }
            connection.send(content: packet, completion: .idempotent)
        }
    }

    private func requestKeyFrameOnQueue() {
        let now = ProcessInfo.processInfo.systemUptime
        guard shouldRun, now - lastKeyFrameRequestAt >= 0.2,
              let connection, connection.state == .ready else { return }
        lastKeyFrameRequestAt = now
        connection.send(content: Data("REQUEST_KEYFRAME".utf8), completion: .idempotent)
    }

    private func setTrackingConnection(_ connection: NWConnection?) {
        trackingLock.lock()
        trackingConnection = connection
        if connection == nil { trackingSentCount = 0 }
        trackingLock.unlock()
    }

    func sendTrackingPacket(_ packet: Data) {
        trackingLock.lock()
        let connection = trackingConnection
        if connection != nil { trackingSentCount += 1 }
        let sent = trackingSentCount
        trackingLock.unlock()
        guard let connection else { return }
        // A late pose is useless; do not wait for or retry individual sends.
        connection.send(content: packet, completion: .idempotent)
        if sent % 30 == 0 {
            DispatchQueue.main.async { [weak self] in self?.trackingPacketsSent = sent }
        }
    }

    private func receive(on connection: NWConnection) {
        connection.receiveMessage { [weak self] content, _, _, error in
            guard let self, self.shouldRun, self.connection === connection else { return }

            // Arm the next receive before parsing this fragment. Replies and
            // tracking continue to share this exact Network.framework flow.
            if error == nil { self.receive(on: connection) }

            if let content {
                self.processPacket(content)
            }

            if let error {
                self.connectionFailed(connection, error: error)
            }
        }
    }

    private func connectionFailed(_ failedConnection: NWConnection, error: NWError) {
        guard connection === failedConnection else { return }
        print("UDP connection error: \(error)")
        failedConnection.stateUpdateHandler = nil
        failedConnection.cancel()
        connection = nil
        setTrackingConnection(nil)
        frames.removeAll()
        lastPlayedFrame = nil
        didReceiveVideo = false
        videoWasStale = false
        lastVideoReceivedAt = nil
        connectionReadyAt = nil
        fpsWindowFrames = 0
        DispatchQueue.main.async { [weak self] in
            self?.connectionStatus = String(localized: "Connection to the Mac failed: \(String(describing: error))")
        }
        networkQueue.asyncAfter(deadline: .now() + 1) { [weak self] in
            self?.beginBrowsing()
        }
    }

    private func browserFailed(_ error: NWError) {
        guard browser != nil else { return }
        print("VRUDPBrowser failed: \(error)")
        browser?.stateUpdateHandler = nil
        browser?.browseResultsChangedHandler = nil
        browser?.cancel()
        browser = nil
        DispatchQueue.main.async { [weak self] in
            self?.connectionStatus = String(localized: "Search for the Mac failed: \(String(describing: error))")
        }
        networkQueue.asyncAfter(deadline: .now() + 1) { [weak self] in
            self?.beginBrowsing()
        }
    }

    private func processPacket(_ data: Data) {
        // "AUDI": game audio from the Mac.
        if data.count >= 20, readUInt32LE(data, at: 0) == 0x49445541 {
            VRAudioPlayer.shared.enqueue(data)
            return
        }
        // "STAT" + 5 floats, once a second: capture, encoded and sent fps,
        // the HEVC bitrate and the Mac's audio packet rate.
        if data.count == 24, readUInt32LE(data, at: 0) == 0x54415453 {
            let values = (0..<5).map { Float(bitPattern: readUInt32LE(data, at: 4 + $0 * 4)) }
            guard values.allSatisfy({ $0.isFinite && $0 >= 0 }) else { return }
            VRStreamStatsStore.shared.update { stats in
                stats.gameFPS = values[0]
                stats.encodedFPS = values[1]
                stats.sentFPS = values[2]
                stats.bitrateMbps = values[3]
                stats.macAudioPacketsPerSecond = values[4]
                stats.statsReceivedAt = ProcessInfo.processInfo.systemUptime
            }
            let status = String(format: String(localized: "game %d fps / encoded %d fps / sent %d fps / %.1f Mbps / Mac audio %d pkt/s"),
                                Int(values[0].rounded()), Int(values[1].rounded()), Int(values[2].rounded()),
                                values[3], Int(values[4].rounded()))
            DispatchQueue.main.async { [weak self] in self?.gameFrameStatus = status }
            return
        }
        // "SCRN" + 4 floats, once a second: a flat game made stereo on the Mac (VXRT_STEREO): show it on a
        // world-locked virtual screen (one eye's aspect, the screen's width and distance in metres, version).
        if data.count == 20, readUInt32LE(data, at: 0) == 0x4E524353 {
            let values = (0..<4).map { Float(bitPattern: readUInt32LE(data, at: 4 + $0 * 4)) }
            VRScreenModeStore.shared.update(eyeAspect: values[0], width: values[1], distance: values[2])
            return
        }
        // "VTAN" + 8 floats: the eye frustum SteamVR renders with.
        if data.count == 36, readUInt32LE(data, at: 0) == 0x4E415456 {
            let values = (0..<8).map { Float(bitPattern: readUInt32LE(data, at: 4 + $0 * 4)) }
            VRSourceProjectionStore.shared.update(
                left: SIMD4<Float>(values[0], values[1], values[2], values[3]),
                right: SIMD4<Float>(values[4], values[5], values[6], values[7])
            )
            return
        }
        guard data.count >= headerSize else { return }

        let magic = readUInt32LE(data, at: 0)
        let frameSequence = readUInt32LE(data, at: 4)
        let chunkIndex = readUInt32LE(data, at: 8)
        let totalChunks = readUInt32LE(data, at: 12)
        let chunkOffset = readUInt32LE(data, at: 16)
        let payloadSize = readUInt32LE(data, at: 20)

        // HardwareEncoder.swift writes this value in little-endian order.
        guard magic == 0x5652414E,
              totalChunks > 0,
              totalChunks <= UInt32(maxChunks),
              chunkIndex < totalChunks,
              payloadSize > 0,
              payloadSize <= UInt32(maxFrameBytes),
              Int(payloadSize) == data.count - headerSize,
              chunkOffset <= UInt32(maxFrameBytes),
              payloadSize <= UInt32(maxFrameBytes) - chunkOffset
        else { return }

        let now = ProcessInfo.processInfo.systemUptime
        receivedPacketCount += 1
        lastPacketReceivedAt = now

        if let lastPlayedFrame, !isNewer(frameSequence, than: lastPlayedFrame) {
            return
        }

        discardStaleFrames(now: now)
        let timestamp = readDoubleLE(data, at: 24)
        let headTransform = readMatrixLE(data, at: 32)

        let expectedChunks = Int(totalChunks)
        if let existing = frames[frameSequence], existing.expectedChunks != expectedChunks {
            frames.removeValue(forKey: frameSequence)
            return
        }

        let assembly = frames[frameSequence] ?? FrameAssembly(
            expectedChunks: expectedChunks,
            createdAt: now,
            timestamp: timestamp,
            headTransform: headTransform
        )
        guard assembly.chunks[chunkIndex] == nil else { return }

        let start = headerSize
        let end = start + Int(payloadSize)
        let fragment = Fragment(
            offset: Int(chunkOffset),
            data: data.subdata(in: start..<end)
        )
        guard assembly.byteCount <= maxFrameBytes - fragment.data.count else {
            frames.removeValue(forKey: frameSequence)
            return
        }

        assembly.chunks[chunkIndex] = fragment
        assembly.byteCount += fragment.data.count
        frames[frameSequence] = assembly
        discardExcessFrames()

        guard let completed = frames[frameSequence],
              completed.chunks.count == completed.expectedChunks
        else { return }

        var frame = Data()
        frame.reserveCapacity(completed.byteCount)
        for index in 0..<completed.expectedChunks {
            guard let part = completed.chunks[UInt32(index)], part.offset == frame.count else {
                frames.removeValue(forKey: frameSequence)
                return
            }
            frame.append(part.data)
        }

        frames.removeValue(forKey: frameSequence)
        if let lastPlayedFrame, !isNewer(frameSequence, than: lastPlayedFrame) { return }
        self.lastPlayedFrame = frameSequence
        let superseded = frames.keys.filter { !isNewer($0, than: frameSequence) }
        incompleteFrameCount += superseded.count
        for sequence in superseded { frames.removeValue(forKey: sequence) }
        framePublisher.send(VRCompressedFrame(
            streamGeneration: streamGeneration,
            sequence: frameSequence,
            timestamp: completed.timestamp,
            headTransform: completed.headTransform,
            data: frame
        ))
        lastVideoReceivedAt = now
        fpsWindowFrames += 1
        if !didReceiveVideo || videoWasStale {
            didReceiveVideo = true
            videoWasStale = false
            DispatchQueue.main.async { [weak self] in
                self?.connectionStatus = String(localized: "Receiving video from the Mac.")
            }
        }
    }

    /// Little-endian read relative to the packet's own start (a Data slice
    /// does not start at index 0), and 0 for a read past its end rather than
    /// a crash on a short or malformed packet.
    private func readUInt32LE(_ data: Data, at offset: Int) -> UInt32 {
        guard offset >= 0, offset + 4 <= data.count else { return 0 }
        let i = data.startIndex + offset
        return UInt32(data[i]) | (UInt32(data[i + 1]) << 8)
            | (UInt32(data[i + 2]) << 16) | (UInt32(data[i + 3]) << 24)
    }

    private func readDoubleLE(_ data: Data, at offset: Int) -> Double {
        let low = UInt64(readUInt32LE(data, at: offset))
        let high = UInt64(readUInt32LE(data, at: offset + 4)) << 32
        return Double(bitPattern: high | low)
    }

    private func readMatrixLE(_ data: Data, at offset: Int) -> simd_float4x4 {
        var values = [Float](repeating: 0, count: 16)
        for index in values.indices {
            values[index] = Float(bitPattern: readUInt32LE(data, at: offset + index * 4))
        }
        return simd_float4x4(
            SIMD4<Float>(values[0], values[1], values[2], values[3]),
            SIMD4<Float>(values[4], values[5], values[6], values[7]),
            SIMD4<Float>(values[8], values[9], values[10], values[11]),
            SIMD4<Float>(values[12], values[13], values[14], values[15])
        )
    }

    private func isNewer(_ sequence: UInt32, than previous: UInt32) -> Bool {
        Int32(bitPattern: sequence &- previous) > 0
    }

    private func discardStaleFrames(now: TimeInterval) {
        let stale = frames.keys.filter { now - frames[$0]!.createdAt > maxAssemblyAge }
        incompleteFrameCount += stale.count
        for sequence in stale { frames.removeValue(forKey: sequence) }
        if !stale.isEmpty { requestKeyFrameOnQueue() }
    }

    private func discardExcessFrames() {
        while frames.count > maxPendingFrames,
              let oldest = frames.min(by: { $0.value.createdAt < $1.value.createdAt })?.key {
            frames.removeValue(forKey: oldest)
            incompleteFrameCount += 1
            requestKeyFrameOnQueue()
        }
    }

    private func scheduleVideoWatchdog(generation: UInt64) {
        networkQueue.asyncAfter(deadline: .now() + 1) { [weak self] in
            guard let self, self.shouldRun, self.watchdogGeneration == generation else { return }
            let now = ProcessInfo.processInfo.systemUptime
            self.discardStaleFrames(now: now)
            let interval = now - self.fpsWindowStart
            let fps = Int((Double(self.fpsWindowFrames) / max(interval, 0.001)).rounded())
            let packets = self.receivedPacketCount
            let lost = self.incompleteFrameCount
            self.fpsWindowFrames = 0
            self.fpsWindowStart = now
            VRStreamStatsStore.shared.update { $0.receivedFPS = fps }
            DispatchQueue.main.async { [weak self] in
                self?.receivedVideoFPS = fps
                self?.receiveDiagnostics = String(localized: "\(packets) packets received / \(lost) frames lost")
                self?.diagnosticsSummary = "packets=\(packets) incomplete=\(lost)"
            }
            let sinceVideo = now - (self.lastVideoReceivedAt ?? self.connectionReadyAt ?? now)
            if sinceVideo > 0.75 { self.requestKeyFrameOnQueue() }
            // Alyx sends nothing while it loads a level (30 s or more). Only
            // reconnect after a long silence; a 3 s limit caused a reconnect
            // storm (new flow every few seconds) during every load.
            let videoTimedOut = self.lastVideoReceivedAt.map { now - $0 > 20 } ??
                self.connectionReadyAt.map { now - $0 > 5 } ?? false
            if videoTimedOut {
                if !self.videoWasStale {
                    self.videoWasStale = true
                    DispatchQueue.main.async {
                        self.connectionStatus = String(localized: "Video stopped arriving. Check SteamVR and the capture on the Mac.")
                    }
                }

                // NWConnection can remain .ready after its Bonjour service has
                // disappeared. Recreate both sides so an encoder restart is found.
                if let connection = self.connection {
                    print("VR video timed out; reconnecting to the Mac encoder service.")
                    self.connection = nil
                    self.setTrackingConnection(nil)
                    self.connectionReadyAt = nil
                    self.frames.removeAll()
                    self.lastPlayedFrame = nil
                    connection.stateUpdateHandler = nil
                    connection.cancel()
                    self.networkQueue.asyncAfter(deadline: .now() + 1) { [weak self] in
                        self?.beginBrowsing()
                    }
                }
            }
            self.scheduleVideoWatchdog(generation: generation)
        }
    }
}
