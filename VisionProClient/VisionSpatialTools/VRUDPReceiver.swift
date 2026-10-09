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

/// Discovers the Mac encoder and reassembles its 96-byte VRAN HEVC fragment packets.
final class VRUDPReceiver: ObservableObject {
    let framePublisher = PassthroughSubject<VRCompressedFrame, Never>()

    @Published private(set) var connectionStatus = "Macのエンコーダーを検索しています…"
    @Published private(set) var receivedVideoFPS = 0
    @Published private(set) var trackingPacketsSent = 0
    @Published private(set) var receiveDiagnostics = "映像パケットを待っています。"

    private var browser: NWBrowser?
    private var connection: NWConnection?
    private let networkQueue = DispatchQueue(label: "com.verantyx.vrudp-receiver", qos: .userInitiated)
    private var shouldRun = false
    private var didReceiveVideo = false
    private var videoWasStale = false
    private var lastVideoReceivedAt: TimeInterval?
    private var connectionReadyAt: TimeInterval?
    private var watchdogGeneration: UInt64 = 0
    private var fpsWindowStart = ProcessInfo.processInfo.systemUptime
    private var fpsWindowFrames = 0
    private var trackingPacketCount = 0
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
        trackingPacketCount = 0
        receivedPacketCount = 0
        incompleteFrameCount = 0
        lastPacketReceivedAt = nil
        lastKeyFrameRequestAt = 0
        DispatchQueue.main.async { [weak self] in
            self?.connectionStatus = "Macのエンコーダーを検索しています…"
            self?.receivedVideoFPS = 0
            self?.trackingPacketsSent = 0
            self?.receiveDiagnostics = "映像パケットを待っています。"
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
                DispatchQueue.main.async { [weak self] in
                    self?.connectionStatus = "Macに接続済み。エンコーダーから映像を待っています。"
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
            self.watchdogGeneration &+= 1
            self.frames.removeAll()
            self.lastPlayedFrame = nil
            self.didReceiveVideo = false
            self.videoWasStale = false
            self.lastVideoReceivedAt = nil
            self.connectionReadyAt = nil
            self.fpsWindowFrames = 0
            DispatchQueue.main.async {
                self.connectionStatus = "停止中"
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
        for character in message.prefix(512) {
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
        guard shouldRun, now - lastKeyFrameRequestAt >= 0.75,
              let connection, connection.state == .ready else { return }
        lastKeyFrameRequestAt = now
        connection.send(content: Data("REQUEST_KEYFRAME".utf8), completion: .idempotent)
    }

    func sendTrackingPacket(_ packet: Data) {
        networkQueue.async { [weak self] in
            guard let self, let connection = self.connection, connection.state == .ready else { return }
            connection.send(content: packet, completion: .contentProcessed { [weak self] error in
                if let error {
                    print("Tracking UDP send failed: \(error)")
                    return
                }
                self?.networkQueue.async { [weak self] in
                    guard let self, self.shouldRun else { return }
                    self.trackingPacketCount += 1
                    if self.trackingPacketCount % 30 == 0 {
                        let sent = self.trackingPacketCount
                        DispatchQueue.main.async { self.trackingPacketsSent = sent }
                    }
                }
            })
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
        frames.removeAll()
        lastPlayedFrame = nil
        didReceiveVideo = false
        videoWasStale = false
        lastVideoReceivedAt = nil
        connectionReadyAt = nil
        fpsWindowFrames = 0
        DispatchQueue.main.async { [weak self] in
            self?.connectionStatus = "Macへの接続に失敗しました: \(error)"
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
            self?.connectionStatus = "Macの検索に失敗しました: \(error)"
        }
        networkQueue.asyncAfter(deadline: .now() + 1) { [weak self] in
            self?.beginBrowsing()
        }
    }

    private func processPacket(_ data: Data) {
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
                self?.connectionStatus = "Macから映像を受信中です。"
            }
        }
    }

    private func readUInt32LE(_ data: Data, at offset: Int) -> UInt32 {
        UInt32(data[offset])
            | (UInt32(data[offset + 1]) << 8)
            | (UInt32(data[offset + 2]) << 16)
            | (UInt32(data[offset + 3]) << 24)
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
            DispatchQueue.main.async { [weak self] in
                self?.receivedVideoFPS = fps
                self?.receiveDiagnostics = "受信 \(packets) パケット／欠落 \(lost) フレーム"
            }
            let sinceVideo = now - (self.lastVideoReceivedAt ?? self.connectionReadyAt ?? now)
            if sinceVideo > 0.75 { self.requestKeyFrameOnQueue() }
            let videoTimedOut = self.lastVideoReceivedAt.map { now - $0 > 3 } ??
                self.connectionReadyAt.map { now - $0 > 5 } ?? false
            if videoTimedOut {
                if !self.videoWasStale {
                    self.videoWasStale = true
                    DispatchQueue.main.async {
                        self.connectionStatus = "映像受信が途切れています。Mac側のSteamVRとキャプチャを確認してください。"
                    }
                }

                // NWConnection can remain .ready after its Bonjour service has
                // disappeared. Recreate both sides so an encoder restart is found.
                if let connection = self.connection {
                    print("VR video timed out; reconnecting to the Mac encoder service.")
                    self.connection = nil
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
