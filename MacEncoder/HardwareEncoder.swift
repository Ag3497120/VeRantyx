import Foundation
setvbuf(stdout, nil, _IONBF, 0)
import VideoToolbox
import CoreMedia
import Network
import Accelerate
import CoreAudio
import Metal

import simd

// MARK: - IPC and UDP Structures

struct SharedFrame {
    var sequenceNumber: UInt32
    var width: UInt32
    var height: UInt32
    var format: UInt32
    var renderHeadTransform: (Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float)
    var visionTimestamp: Double
}

struct DoubleBufferHeader {
    var latestReadyIndex: UInt32
    var pendingTextureHandle: UInt32
    var captureRequestSequence: UInt32
    var captureAckSequence: UInt32
    var captureSourceMode: UInt32
    var poseSequence: UInt32
    var padding: (UInt32, UInt32, UInt32, UInt32, UInt32, UInt32, UInt32, UInt32, UInt32, UInt32)
}

struct FrameContext {
    var sequence: UInt32
    var visionTimestamp: Double
    var transform: (Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float)
    // Keep the source image alive until VideoToolbox finishes reading it.
    var pixelBuffer: CVPixelBuffer
}
struct UDPHeader {
    var magic: UInt32 // 0x5652414E ("VRAN")
    var frameSequence: UInt32
    var chunkIndex: UInt32
    var totalChunks: UInt32
    var chunkOffset: UInt32
    var payloadSize: UInt32
    var timestamp: Double
    var transform: (Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float)
}

public struct JCrossNode_VR {
    public var nodeId: UInt16
    public var flags: UInt16
    public var position: simd_float3
    public var rotation: simd_quatf
    public var velocity: simd_float3
    public var angularVelocity: simd_float3
}

public struct JCrossZone_Input {
    public var parentNodeId: UInt16
    public var buttonMask: UInt32
    public var triggerValue: Float
    public var gripValue: Float
    public var thumbstick: simd_float2
}

public struct JCrossPayload {
    public var frameId: UInt32
    public var timestamp: Double
    public var headNode: JCrossNode_VR
    public var leftHandNode: JCrossNode_VR
    public var rightHandNode: JCrossNode_VR
    public var leftInput: JCrossZone_Input
    public var rightInput: JCrossZone_Input
}

struct JoyconPacket {
    var magic: UInt32 // 0x4A4F5943 ("JOYC")
    var rightButtons: UInt32
    var leftButtons: UInt32
    var rightStickX: Float
    var rightStickY: Float
    var leftStickX: Float
    var leftStickY: Float
    var rightVelocityX: Float
    var rightVelocityY: Float
    var rightVelocityZ: Float
    var leftVelocityX: Float
    var leftVelocityY: Float
    var leftVelocityZ: Float
}

// MARK: - Hand Tracking & Video Connection (AWDL Bonjour)

let transportQueue = DispatchQueue(label: "com.verantyx.encoder.transport", qos: .userInteractive)
let peerLock = NSLock()
var videoPeer: NWConnection?
let retiredVideoPeers = NSHashTable<NWConnection>(options: [.weakMemory, .objectPointerPersonality])
var peerNeedsKeyFrame = true
var keyFrameRequested = true
var peerGeneration: UInt64 = 0
var lastClientDiagnosticsAt: TimeInterval = -.infinity
let videoDatagramsInFlight = DispatchSemaphore(value: 8)
let outputLock = NSLock()

func requestKeyFrame() {
    peerLock.lock()
    keyFrameRequested = true
    peerLock.unlock()
}

func consumeKeyFrameRequest() -> Bool {
    peerLock.lock()
    defer { peerLock.unlock() }
    let requested = keyFrameRequested
    keyFrameRequested = false
    return requested
}

func registerVideoPeer(_ connection: NWConnection, handshake: Bool) {
    peerLock.lock()
    guard !retiredVideoPeers.contains(connection) else {
        peerLock.unlock()
        return
    }
    let changed = videoPeer !== connection
    let oldPeer = changed ? videoPeer : nil
    if let oldPeer { retiredVideoPeers.add(oldPeer) }
    if changed { lastClientDiagnosticsAt = -.infinity }
    videoPeer = connection
    if changed || handshake {
        peerGeneration &+= 1
        peerNeedsKeyFrame = true
        keyFrameRequested = true
    }
    peerLock.unlock()
    oldPeer?.cancel()
    if changed || handshake {
        print("Vision Pro endpoint \(connection.endpoint); keyframe requested, path=\(String(describing: connection.currentPath))")
    }
}

// MARK: - VideoToolbox Callback

var cachedVPS: Data?
var cachedSPS: Data?
var cachedPPS: Data?
let encoderDiagnosticLock = NSLock()
var compressedFrameCount = 0
var sentVideoPacketCount = 0
var videoPacketSendErrorCount = 0
var videoBytesSent = 0
var transmittedFrameCount = 0
var droppedTransportFrames = 0
var capturedFrameCount = 0
var submittedFrameCount = 0
var posePacketCount = 0
var lastPoseReceivedAt: TimeInterval = 0

final class DatagramBatch {
    let completion = DispatchGroup()
    private let lock = NSLock()
    private var failed = false

    func markFailed() {
        lock.lock()
        failed = true
        lock.unlock()
    }

    var hasFailed: Bool {
        lock.lock()
        defer { lock.unlock() }
        return failed
    }
}

// All chunks of one frame use the same Network.framework flow. This retains the
// interface and local address chosen by Bonjour, including link-local AWDL.
func sendVideoPacket(_ packet: Data, to peer: NWConnection, batch: DatagramBatch,
                     deadline: DispatchTime) -> Bool {
    guard videoDatagramsInFlight.wait(timeout: deadline) == .success else {
        batch.markFailed()
        return false
    }
    batch.completion.enter()
    peer.send(content: packet, completion: .contentProcessed { error in
        encoderDiagnosticLock.lock()
        if let error {
            videoPacketSendErrorCount += 1
            if videoPacketSendErrorCount <= 3 || videoPacketSendErrorCount % 100 == 0 {
                print("UDP video send failed: \(error)")
            }
            batch.markFailed()
        } else {
            sentVideoPacketCount += 1
            videoBytesSent += packet.count
        }
        encoderDiagnosticLock.unlock()
        videoDatagramsInFlight.signal()
        batch.completion.leave()
    })
    return true
}

func processSampleBuffer(sampleBuffer: CMSampleBuffer, isKeyFrame: Bool, frameSequence: UInt32, visionTimestamp: Double, transform: (Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float)) {
    // Keep parameter sets, packet ordering and the bounded network backpressure
    // in one place even if VideoToolbox invokes output on different threads.
    outputLock.lock()
    defer { outputLock.unlock() }
    if let alvr { alvrSendFrame(alvr, sampleBuffer, isKeyFrame: isKeyFrame, timestamp: visionTimestamp) }
    encoderDiagnosticLock.lock()
    compressedFrameCount += 1
    let outputFrameNumber = compressedFrameCount
    encoderDiagnosticLock.unlock()
    peerLock.lock()
    let peer = videoPeer
    let waitingForKeyFrame = peerNeedsKeyFrame
    let generation = peerGeneration
    peerLock.unlock()
    guard let peer, !waitingForKeyFrame || isKeyFrame else { return }
    guard let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }
    var length = 0
    var dataPointer: UnsafeMutablePointer<Int8>? = nil
    guard CMBlockBufferGetDataPointer(dataBuffer, atOffset: 0, lengthAtOffsetOut: nil,
        totalLengthOut: &length, dataPointerOut: &dataPointer) == noErr else { return }
    
    guard let data = dataPointer else { return }
    
    var frameData = Data()
    
    // 1. Extract and Cache SPS and PPS
    if let formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer) {
        var vpsSize: Int = 0
        var vpsCount: Int = 0
        var vpsPtr: UnsafePointer<UInt8>? = nil
        if CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(formatDesc, parameterSetIndex: 0, parameterSetPointerOut: &vpsPtr, parameterSetSizeOut: &vpsSize, parameterSetCountOut: &vpsCount, nalUnitHeaderLengthOut: nil) == noErr, let vps = vpsPtr {
            cachedVPS = Data(bytes: vps, count: vpsSize)
        }
        
        var spsSize: Int = 0
        var spsCount: Int = 0
        var spsPtr: UnsafePointer<UInt8>? = nil
        if CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(formatDesc, parameterSetIndex: 1, parameterSetPointerOut: &spsPtr, parameterSetSizeOut: &spsSize, parameterSetCountOut: &spsCount, nalUnitHeaderLengthOut: nil) == noErr, let sps = spsPtr {
            cachedSPS = Data(bytes: sps, count: spsSize)
        }
        
        var ppsSize: Int = 0
        var ppsCount: Int = 0
        var ppsPtr: UnsafePointer<UInt8>? = nil
        if CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(formatDesc, parameterSetIndex: 2, parameterSetPointerOut: &ppsPtr, parameterSetSizeOut: &ppsSize, parameterSetCountOut: &ppsCount, nalUnitHeaderLengthOut: nil) == noErr, let pps = ppsPtr {
            cachedPPS = Data(bytes: pps, count: ppsSize)
        }
    }
    
    if isKeyFrame {
        let startCode: [UInt8] = [0, 0, 0, 1]
        if let vps = cachedVPS {
            frameData.append(contentsOf: startCode)
            frameData.append(vps)
        }
        if let sps = cachedSPS {
            frameData.append(contentsOf: startCode)
            frameData.append(sps)
        }
        if let pps = cachedPPS {
            frameData.append(contentsOf: startCode)
            frameData.append(pps)
        }
    }
    
    // 2. AVCC to Annex-B Conversion
    var offset = 0
    while offset < length - 4 {
        var nalLength: UInt32 = 0
        memcpy(&nalLength, data + offset, 4)
        nalLength = UInt32(bigEndian: nalLength)
        guard nalLength > 0, Int(nalLength) <= length - offset - 4 else {
            print("Discarding invalid HEVC NAL length \(nalLength) in \(length)-byte frame")
            requestKeyFrame()
            return
        }
        
        let startCode: [UInt8] = [0, 0, 0, 1]
        frameData.append(contentsOf: startCode)
        frameData.append(data.advanced(by: offset + 4).withMemoryRebound(to: UInt8.self, capacity: Int(nalLength)) { $0 }, count: Int(nalLength))
        
        offset += 4 + Int(nalLength)
    }
    
    // 3. UDP Fragmentation and Transmission
    // 1100 + 96-byte VR header + 48-byte IPv6/UDP header fits even a 1280 MTU.
    let mtu = 1100
    let totalBytes = frameData.count
    let numFragments = (totalBytes + mtu - 1) / mtu

    if outputFrameNumber <= 3 || waitingForKeyFrame {
        print("Encoded HEVC frame seq=\(frameSequence), bytes=\(totalBytes), keyFrame=\(isKeyFrame)")
    }
    
    let batch = DatagramBatch()
    // A first IDR can be much larger than a predicted frame. Allow its bounded
    // burst to finish inside the receiver's one-second assembly lifetime.
    let deadline = DispatchTime.now() + .milliseconds(isKeyFrame ? 750 : 250)
    var nextSendAt = ProcessInfo.processInfo.systemUptime
    frameData.withUnsafeBytes { rawBuffer in
        guard let baseAddress = rawBuffer.bindMemory(to: UInt8.self).baseAddress else { return }
        
        for i in 0..<numFragments {
            guard DispatchTime.now() < deadline, !batch.hasFailed else {
                batch.markFailed()
                break
            }
            let chunkOffset = i * mtu
            let fragSize = min(mtu, totalBytes - chunkOffset)
            
            var header = UDPHeader(
                magic: UInt32(0x5652414E).littleEndian, // "VRAN"
                frameSequence: frameSequence.littleEndian,
                chunkIndex: UInt32(i).littleEndian,
                totalChunks: UInt32(numFragments).littleEndian,
                chunkOffset: UInt32(chunkOffset).littleEndian,
                payloadSize: UInt32(fragSize).littleEndian,
                timestamp: visionTimestamp,
                transform: transform
            )
            
            var packet = Data()
            withUnsafeBytes(of: &header) { pktHeader in
                packet.append(contentsOf: pktHeader)
            }
            packet.append(baseAddress + chunkOffset, count: fragSize)
            
            guard sendVideoPacket(packet, to: peer, batch: batch, deadline: deadline) else { break }
            // Pace bursts (60 Mbps by default). In-flight completion alone only
            // bounds the userspace queue; it does not mean radio delivery.
            nextSendAt += Double(packet.count) / sendPacingBytesPerSecond
            let pause = nextSendAt - ProcessInfo.processInfo.systemUptime
            if pause > 0 { usleep(useconds_t(min(pause, 0.01) * 1_000_000)) }
        }
    }
    let finished = batch.completion.wait(timeout: deadline) == .success
    let succeeded = finished && !batch.hasFailed
    peerLock.lock()
    if videoPeer === peer && peerGeneration == generation {
        if succeeded && isKeyFrame { peerNeedsKeyFrame = false }
        if !succeeded {
            peerNeedsKeyFrame = true
            keyFrameRequested = true
        }
    }
    peerLock.unlock()
    encoderDiagnosticLock.lock()
    if succeeded { transmittedFrameCount += 1 } else { droppedTransportFrames += 1 }
    encoderDiagnosticLock.unlock()
}

// MARK: - VideoToolbox Setup

var compressionSession: VTCompressionSession?
var pixelBufferPool: CVPixelBufferPool?
let encoderInFlightFrames = DispatchSemaphore(value: 3)
var isEncoding = true
var totalBytesEncoded = 0

func makePixelBufferPool(width: Int, height: Int) -> CVPixelBufferPool? {
    let poolAttributes: [CFString: Any] = [
        kCVPixelBufferPoolMinimumBufferCountKey: 3
    ]
    let pixelBufferAttributes: [CFString: Any] = [
        kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
        kCVPixelBufferWidthKey: width,
        kCVPixelBufferHeightKey: height,
        kCVPixelBufferIOSurfacePropertiesKey: [:] as [String: Any],
        kCVPixelBufferMetalCompatibilityKey: true
    ]
    var pool: CVPixelBufferPool?
    let status = CVPixelBufferPoolCreate(
        kCFAllocatorDefault,
        poolAttributes as CFDictionary,
        pixelBufferAttributes as CFDictionary,
        &pool
    )
    guard status == kCVReturnSuccess else {
        print("CVPixelBufferPoolCreate failed: \(status)")
        return nil
    }
    return pool
}

func environmentDouble(_ name: String, default fallback: Double, range: ClosedRange<Double>) -> Double {
    guard let text = ProcessInfo.processInfo.environment[name], let value = Double(text),
          range.contains(value) else { return fallback }
    return value
}

// IDRs are several times larger than predicted frames. Sending one every 0.5 s
// made a visible hitch twice a second; lost frames already request an IDR.
let keyFrameIntervalSeconds = environmentDouble("VERANTYX_KEYFRAME_INTERVAL", default: 3.0, range: 0.5...30)
let videoBitrate = Int32(environmentDouble("VERANTYX_BITRATE_MBPS", default: 20, range: 2...80) * 1_000_000)
// ALVR (VERANTYX_ALVR=1): ALVR's streaming core (ALVRBridge/, its library loaded at run time from VERANTYX_ALVR_LIB)
// takes the frames as well, for the ALVR client apps (Quest, Pico, the Vision Pro's "ALVR"); their poses and buttons
// go into SharedHands as the Vision Pro app's do, so the driver sees no difference. Each frame goes with the time of
// the tracking sample it was rendered for (SharedHands.visionTimestamp, in ALVR's clock).
struct ALVRBridge {
    let start: @convention(c) (UnsafePointer<CChar>?) -> Bool
    let input: @convention(c) (UnsafeMutableRawPointer, UInt64) -> UInt64
    let connected: @convention(c) () -> Bool
    let takeIDR: @convention(c) () -> Bool
    let setConfig: @convention(c) (UnsafePointer<UInt8>, Int) -> Void
    let send: @convention(c) (Double, UnsafePointer<UInt8>, Int, Bool) -> Void
    let restartNeeded: @convention(c) () -> Bool
}
let alvrMode = ProcessInfo.processInfo.environment["VERANTYX_ALVR"] == "1"
func loadALVRBridge() -> ALVRBridge? {
    let path = ProcessInfo.processInfo.environment["VERANTYX_ALVR_LIB"] ?? "ALVRBridge/target/release/libverantyx_alvr_bridge.dylib"
    guard let lib = dlopen(path, RTLD_NOW) else { print("ALVR: cannot load \(path): \(String(cString: dlerror()))"); return nil }
    func fn<T>(_ name: String, _ type: T.Type) -> T? { dlsym(lib, name).map { unsafeBitCast($0, to: type) } }
    guard let start = fn("vx_alvr_start", (@convention(c) (UnsafePointer<CChar>?) -> Bool).self),
          let input = fn("vx_alvr_input", (@convention(c) (UnsafeMutableRawPointer, UInt64) -> UInt64).self),
          let connected = fn("vx_alvr_connected", (@convention(c) () -> Bool).self),
          let takeIDR = fn("vx_alvr_take_idr_request", (@convention(c) () -> Bool).self),
          let setConfig = fn("vx_alvr_set_config", (@convention(c) (UnsafePointer<UInt8>, Int) -> Void).self),
          let send = fn("vx_alvr_send", (@convention(c) (Double, UnsafePointer<UInt8>, Int, Bool) -> Void).self),
          let restartNeeded = fn("vx_alvr_restart_needed", (@convention(c) () -> Bool).self)
    else { print("ALVR: \(path) lacks the bridge's functions"); return nil }
    let bridge = ALVRBridge(start: start, input: input, connected: connected, takeIDR: takeIDR, setConfig: setConfig, send: send, restartNeeded: restartNeeded)
    guard bridge.start(nil) else { print("ALVR: the streaming core did not start"); return nil }
    print("ALVR: streaming core started (\(path)); open an ALVR client on this network")
    return bridge
}
let alvr: ALVRBridge? = alvrMode ? loadALVRBridge() : nil
var alvrConfig = Data()

// The encoded frame to ALVR: Annex-B, the parameter sets before an IDR (and given to the core when they change).
func alvrSendFrame(_ alvr: ALVRBridge, _ sampleBuffer: CMSampleBuffer, isKeyFrame: Bool, timestamp: Double) {
    var config = Data()
    let startCode: [UInt8] = [0, 0, 0, 1]
    if isKeyFrame, let fd = CMSampleBufferGetFormatDescription(sampleBuffer) {
        for i in 0..<3 {
            var p: UnsafePointer<UInt8>? = nil; var n = 0
            if CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: &p,
                                                                  parameterSetSizeOut: &n, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr, let p {
                config.append(contentsOf: startCode); config.append(p, count: n)
            }
        }
        if !config.isEmpty && config != alvrConfig {
            alvrConfig = config
            config.withUnsafeBytes { alvr.setConfig($0.bindMemory(to: UInt8.self).baseAddress!, config.count) }
        }
    }
    guard alvr.connected(), let block = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }
    var length = 0; var ptr: UnsafeMutablePointer<Int8>? = nil
    guard CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &length, dataPointerOut: &ptr) == noErr, let base = ptr else { return }
    var au = isKeyFrame ? config : Data()
    var offset = 0
    while offset + 4 <= length {
        let n = Int(UInt32(bigEndian: UnsafeRawPointer(base + offset).loadUnaligned(as: UInt32.self)))
        guard n > 0, n <= length - offset - 4 else { requestKeyFrame(); return }
        au.append(contentsOf: startCode)
        au.append(UnsafeRawPointer(base + offset + 4).assumingMemoryBound(to: UInt8.self), count: n)
        offset += 4 + n
    }
    au.withUnsafeBytes { alvr.send(timestamp, $0.bindMemory(to: UInt8.self).baseAddress!, au.count, isKeyFrame) }
}

let hevcTenBit = ProcessInfo.processInfo.environment["VERANTYX_HEVC_10BIT"].map { $0 != "0" } ?? !alvrMode   // (ALVR: Main, which every ALVR client decodes)
// Pace UDP bursts so AWDL does not drop them, but fast enough that a frame
// does not spend most of its interval queued behind the previous one.
let sendPacingBytesPerSecond = environmentDouble("VERANTYX_SEND_PACING_MBPS", default: 60, range: 8...400) * 1_000_000 / 8

func createCompressionSession(width: Int32, height: Int32, lowLatency: Bool) -> OSStatus {
    var specification: [CFString: Any] = [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: true]
    if lowLatency {
        // Low-latency rate control keeps frame sizes even, but on an M-series
        // Mac it took 12 ms per 1442x810 frame versus 8 ms for the standard
        // real-time encoder, so it is opt-in (VERANTYX_LOW_LATENCY_ENCODER=1).
        specification[kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = true
    }
    return VTCompressionSessionCreate(allocator: kCFAllocatorDefault,
                                            width: width,
                                            height: height,
                                            codecType: kCMVideoCodecType_HEVC,
                                            encoderSpecification: specification as CFDictionary,
                                            imageBufferAttributes: nil,
                                            compressedDataAllocator: nil,
                                            outputCallback: { (outputCallbackRefCon, sourceFrameRefCon, status, infoFlags, sampleBuffer) in
        guard let sourceFrameRefCon else {
            encoderInFlightFrames.signal()
            return
        }
        let refConPtr = sourceFrameRefCon.assumingMemoryBound(to: FrameContext.self)
        let frameSequence = refConPtr.pointee.sequence
        let visionTimestamp = refConPtr.pointee.visionTimestamp
        let transform = refConPtr.pointee.transform
        defer {
            refConPtr.deinitialize(count: 1)
            refConPtr.deallocate()
            encoderInFlightFrames.signal()
        }
        
        if status != noErr {
            print("Encoding error: \(status)")
            requestKeyFrame()
            return
        }
        guard !infoFlags.contains(.frameDropped), let sampleBuffer else {
            requestKeyFrame()
            return
        }
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[CFString: Any]]
        let isKeyFrame = (attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool) != true
        
        processSampleBuffer(sampleBuffer: sampleBuffer, isKeyFrame: isKeyFrame, frameSequence: frameSequence, visionTimestamp: visionTimestamp, transform: transform)

    },
                                            refcon: nil,
                                            compressionSessionOut: &compressionSession)
}

// Lowers the HEVC bitrate while the link loses packets (the headset's
// incomplete-frame counter, or local send errors) and raises it back
// slowly once the link has been clean for a while. VERANTYX_BITRATE_MBPS is
// the ceiling, VERANTYX_BITRATE_MIN_MBPS the floor; VERANTYX_ADAPTIVE_BITRATE=0
// keeps the ceiling fixed.
final class AdaptiveBitrate {
    let maximum: Int32
    let minimum: Int32
    private(set) var current: Int32
    private let enabled = ProcessInfo.processInfo.environment["VERANTYX_ADAPTIVE_BITRATE"] != "0"
    private let lock = NSLock()
    private var headsetLossTotal = 0      // incomplete frames reported by the headset
    private var headsetLossAtLastCheck = 0
    private var cleanSeconds: TimeInterval = 0

    init(maximum: Int32) {
        self.maximum = maximum
        let floor = environmentDouble("VERANTYX_BITRATE_MIN_MBPS", default: max(4, Double(maximum) / 1_000_000 * 0.4), range: 1...80)
        self.minimum = min(maximum, Int32(floor * 1_000_000))
        self.current = maximum
    }

    func noteHeadsetStatistics(_ message: String) {
        // Only frames that arrived with packets missing count as link loss.
        // The headset's gap counter also grows when the encoder itself skips
        // a frame, which lowered the bitrate for no reason (8 Mbps in play).
        var total = 0
        if let range = message.range(of: "incomplete=") {
            let digits = message[range.upperBound...].prefix { $0.isNumber }
            total = Int(digits) ?? 0
        }
        lock.lock()
        headsetLossTotal = total
        lock.unlock()
    }

    /// Called every diagnostics period with the local loss counters' deltas.
    func update(localLossEvents: Int, elapsed: TimeInterval, session: VTCompressionSession?) {
        guard enabled, let session else { return }
        lock.lock()
        let headsetDelta = max(0, headsetLossTotal - headsetLossAtLastCheck)
        headsetLossAtLastCheck = headsetLossTotal
        lock.unlock()
        let lossEvents = localLossEvents + headsetDelta
        var target = current
        if lossEvents >= 2 {
            cleanSeconds = 0
            target = max(minimum, Int32(Double(current) * 0.8))
        } else {
            cleanSeconds += elapsed
            if cleanSeconds >= 30, current < maximum {
                cleanSeconds = 0
                target = min(maximum, Int32(Double(current) * 1.1))
            }
        }
        guard target != current else { return }
        current = target
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: current))
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_DataRateLimits,
            value: [NSNumber(value: Double(current) / 8), NSNumber(value: 1)] as CFArray)
        print(String(format: "HEVC bitrate %@ to %.1f Mbps (%d loss events in the last %.0f s)",
            lossEvents >= 2 ? "lowered" : "raised", Double(current) / 1_000_000, lossEvents, elapsed))
    }
}
let adaptiveBitrate = AdaptiveBitrate(maximum: videoBitrate)

func setupEncoder(width: Int32, height: Int32) {
    var lowLatency = ProcessInfo.processInfo.environment["VERANTYX_LOW_LATENCY_ENCODER"] == "1"
    var status = createCompressionSession(width: width, height: height, lowLatency: lowLatency)
    if status != noErr && lowLatency {
        print("Low-latency HEVC rate control is unavailable (\(status)); using the standard real-time encoder")
        lowLatency = false
        status = createCompressionSession(width: width, height: height, lowLatency: false)
    }
    guard status == noErr, let session = compressionSession else {
        print("Failed to create VTCompressionSession: \(status)")
        exit(1)
    }

    func set(_ key: CFString, _ value: CFTypeRef, _ label: String) {
        let result = VTSessionSetProperty(session, key: key, value: value)
        if result != noErr { print("HEVC encoder ignored \(label): \(result)") }
    }

    set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue, "real-time mode")
    // Colour: BT.709 primaries and matrix in the stream (the headset's decoder otherwise guesses, and a guess of
    // BT.601 shifts hues: reds and greens).
    set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2, "colour primaries")
    set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_sRGB, "transfer function")   // as the frames: no conversion
    set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2, "YCbCr matrix")
    // HEVC Main10 (VERANTYX_HEVC_10BIT=0: Main): even from 8-bit frames, the 10-bit YCbCr and quantization keep
    // dark gradients smooth (4x the steps of Main on a dark ramp, measured on an M1 Max) at fewer bits. The
    // headset app decodes it to half floats.
    set(kVTCompressionPropertyKey_ProfileLevel, hevcTenBit ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel, "profile")
    // Critical settings for ultra-low latency VR streaming
    // A quality target as well as the bitrate: rate control alone gave dark frames a fraction of their share
    // (Half-Life: Alyx's Valve logo, a dark red scene: a 22 KB keyframe where 40 Mbps allows 80; the shadows
    // came out in blocks). The data-rate limit below still caps each second. VERANTYX_HEVC_QUALITY: 0-1.
    let quality = Double(ProcessInfo.processInfo.environment["VERANTYX_HEVC_QUALITY"] ?? "0.9") ?? 0.9
    if quality > 0 { set(kVTCompressionPropertyKey_Quality, NSNumber(value: min(quality, 1)), "quality") }
    set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse, "no B-frames")
    set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue, "speed priority")
    set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: adaptiveBitrate.current), "average bitrate")
    // Hard cap each second at the average: the game runs 15-25 fps and the
    // encoder overshot to 35 Mbps at a 20 Mbps average, which lost packets.
    set(kVTCompressionPropertyKey_DataRateLimits,
        [NSNumber(value: Double(adaptiveBitrate.current) / 8), NSNumber(value: 1)] as CFArray, "data-rate limit")

    // This is an initial rate-control hint, not the display refresh rate. The
    // main loop updates it from observed capture cadence every five seconds.
    set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: 30), "expected frame rate")

    // Bound recovery in elapsed time even when the game only produces 4fps.
    set(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: Int(keyFrameIntervalSeconds * 90)), "keyframe interval")
    set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: keyFrameIntervalSeconds), "keyframe duration")

    VTCompressionSessionPrepareToEncodeFrames(session)
    print(String(format: "HEVC encoder ready: %dx%d %@ %.1f Mbps keyframe=%.1fs pacing=%.0f Mbps lowLatency=%@",
        width, height, hevcTenBit ? "Main10" : "Main", Double(videoBitrate) / 1_000_000, keyFrameIntervalSeconds,
        sendPacingBytesPerSecond * 8 / 1_000_000, lowLatency ? "yes" : "no"))
    requestKeyFrame()
}

// MARK: - Main IPC Loop

let winePrefix = ProcessInfo.processInfo.environment["WINEPREFIX"]
    ?? (NSHomeDirectory() + "/Verantyx_VR_Drive/SteamVR_Prefix")
// VERANTYX_SHARED_FRAME names the file directly (the native ARM build keeps
// it beside the game, not in a Wine prefix).
let filePath = ProcessInfo.processInfo.environment["VERANTYX_SHARED_FRAME"].flatMap { $0.isEmpty ? nil : $0 }
    ?? URL(fileURLWithPath: winePrefix)
        .appendingPathComponent("drive_c/vr_shared_frame.dat")
        .path
let sharedHandsSize = 312 // Keep in sync with SharedHands in driver_verantyx.cpp.
let mapSize = MemoryLayout<DoubleBufferHeader>.size + (Int(MemoryLayout<SharedFrame>.size) + 4096 * 4096 * 8) * 2 + sharedHandsSize + 1024

print("Waiting for vr_shared_frame.dat...")
while true {
    if FileManager.default.fileExists(atPath: filePath) {
        do {
            let attr = try FileManager.default.attributesOfItem(atPath: filePath)
            if let fileSize = attr[FileAttributeKey.size] as? UInt64 {
                if fileSize >= UInt64(mapSize) {
                    break
                }
            }
        } catch {}
    }
    Thread.sleep(forTimeInterval: 0.5)
}

let fd = open(filePath, O_RDWR)
if fd < 0 {
    print("Failed to open file")
    exit(1)
}

let mapPtr = mmap(nil, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
if mapPtr == MAP_FAILED {
    print("mmap failed")
    exit(1)
}

let basePtr = mapPtr! + MemoryLayout<DoubleBufferHeader>.size
let frameBlockSize = Int(MemoryLayout<SharedFrame>.size) + 4096 * 4096 * 8

let headers: [UnsafeMutablePointer<SharedFrame>] = [
    basePtr.bindMemory(to: SharedFrame.self, capacity: 1),
    (basePtr + frameBlockSize).bindMemory(to: SharedFrame.self, capacity: 1)
]

let pixelDatas: [UnsafeMutableRawPointer] = [
    basePtr + MemoryLayout<SharedFrame>.size,
    (basePtr + frameBlockSize) + MemoryLayout<SharedFrame>.size
]

let handsMapPtr = basePtr + (frameBlockSize * 2)
let dbHeaderPtr = mapPtr!.bindMemory(to: DoubleBufferHeader.self, capacity: 1)
let sharedHandsWriteLock = NSLock()

func withSharedHandsWrite(_ write: () -> Void) {
    sharedHandsWriteLock.lock()
    defer { sharedHandsWriteLock.unlock() }
    let current = dbHeaderPtr.pointee.poseSequence
    let odd = (current & 1) == 0 ? current &+ 1 : current &+ 2
    dbHeaderPtr.pointee.poseSequence = odd
    OSMemoryBarrier()
    write()
    OSMemoryBarrier()
    dbHeaderPtr.pointee.poseSequence = odd &+ 1
}

// ALVR's input into SharedHands (offsets as the Vision Pro's POSE packet writes them; ALVRBridge's VxAlvrInput layout,
// which its Rust side checks at compile time), and its clients' IDR requests to the encoder.
if let alvr {
    Thread {
        let input = UnsafeMutableRawPointer.allocate(byteCount: 312, alignment: 8)
        var seq: UInt64 = 0
        var checked = Date()
        while isEncoding {
            if alvr.takeIDR() { requestKeyFrame() }
            if Date().timeIntervalSince(checked) > 0.5 {                // (a headset with other display settings:
                checked = Date()                                        //  scripts/launch_vxrt_steamvr.sh starts the
                if alvr.restartNeeded() {                               //  encoder again, which connects with them)
                    print("ALVR: the headset brought other display settings (stored): the encoder starts again")
                    fflush(stdout)
                    exit(75)
                }
            }
            let next = alvr.input(input, seq)
            if next != seq {
                seq = next
                withSharedHandsWrite {
                    memcpy(handsMapPtr, input + 8, 64)                 // head, left, right (column-major)
                    memcpy(handsMapPtr + 64, input + 72, 64)
                    memcpy(handsMapPtr + 128, input + 136, 64)
                    memcpy(handsMapPtr + 192, input + 305, 4)          // leftTracked rightTracked leftTrigger rightTrigger
                    memcpy(handsMapPtr + 196, input + 296, 8)          // rightButtons leftButtons
                    memcpy(handsMapPtr + 204, input + 256, 8)          // right stick
                    memcpy(handsMapPtr + 212, input + 248, 8)          // left stick
                    memcpy(handsMapPtr + 220, input + 212, 12)         // right velocity
                    memcpy(handsMapPtr + 232, input + 200, 12)         // left velocity
                    memcpy(handsMapPtr + 244, input, 8)                // the sample's time (the frame's pose id)
                    if input.load(fromByteOffset: 311, as: UInt8.self) != 0 {
                        memcpy(handsMapPtr + 252, input + 264, 32)     // eye tangents, left then right
                    }
                    memcpy(handsMapPtr + 284, input + 309, 2)          // grips
                    memcpy(handsMapPtr + 286, input + 304, 1)          // headTracked
                    memcpy(handsMapPtr + 287, input + 224, 24)         // angular velocities, left then right
                    handsMapPtr.advanced(by: 311).storeBytes(of: UInt8(1), as: UInt8.self)
                }
            }
            usleep(500)
        }
    }.start()
}

var lastSeq: UInt32 = 0
let initialSequence0 = headers[0].pointee.sequenceNumber
let initialSequence1 = headers[1].pointee.sequenceNumber
var streamSequence = Int32(bitPattern: initialSequence0 &- initialSequence1) > 0
    ? initialSequence0 : initialSequence1
var framesEncoded = 0
var droppedEncoderFrames = 0
var lastCaptureSourceMode = dbHeaderPtr.pointee.captureSourceMode
let presentationEpoch = ProcessInfo.processInfo.systemUptime
var lastPresentationSeconds: TimeInterval = -1
var lastForcedKeyFrameAt: TimeInterval = -.infinity
var lastDiagnosticsAt = ProcessInfo.processInfo.systemUptime
var lastDiagnostics = (capture: 0, submit: 0, encode: 0, sent: 0, packets: 0, bytes: 0, errors: 0, drops: 0, poses: 0)
// Default pixel budget for both eyes (~2 MP, 2276x910 for the Vision Pro's
// frustum). 1440x810 left ~8 pixels per degree and aliased text; ~3 MP at
// 30 Mbps lost packets on Wi-Fi often enough to stall decoding.
let configuredOutputWidth = Int(ProcessInfo.processInfo.environment["VERANTYX_STREAM_WIDTH"] ?? "1920") ?? 1920
let configuredOutputHeight = Int(ProcessInfo.processInfo.environment["VERANTYX_STREAM_HEIGHT"] ?? "1080") ?? 1080
// VERANTYX_STREAM_WIDTH/HEIGHT set the pixel budget; the stream keeps the eyes' aspect within it (a 2.5:1 frame
// squeezed into 16:9 lost twice the detail across as down). VERANTYX_STREAM_FIXED_SIZE=1: exactly that size.
let hasExplicitOutputSize = ProcessInfo.processInfo.environment["VERANTYX_STREAM_FIXED_SIZE"] == "1"
var outputWidth = min(4096, max(2, configuredOutputWidth & ~1))
var outputHeight = min(4096, max(2, configuredOutputHeight & ~1))
var outputSourceAspect: Double?

// The driver publishes the eye frustum SteamVR renders with in the shared
// header's padding (8 tangents + "VTAN"). The headset needs it to draw each
// eye at the angles the game actually used.
let sourceProjectionMagic: UInt32 = 0x4E415456 // "VTAN"
var lastSourceProjectionSentAt: TimeInterval = -.infinity
var lastSourceProjection: [Float] = []

func readSourceProjection() -> [Float]? {
    var header = dbHeaderPtr.pointee
    return withUnsafeBytes(of: &header.padding) { raw -> [Float]? in
        guard raw.load(fromByteOffset: 32, as: UInt32.self) == sourceProjectionMagic else { return nil }
        let values = (0..<8).map { raw.load(fromByteOffset: $0 * 4, as: Float.self) }
        guard values.allSatisfy({ $0.isFinite }), values[0] < 0, values[1] > 0, values[2] < 0, values[3] > 0,
              values[4] < 0, values[5] > 0, values[6] < 0, values[7] > 0 else { return nil }
        return values
    }
}

func sendSourceProjectionIfDue() {
    let now = ProcessInfo.processInfo.systemUptime
    guard now - lastSourceProjectionSentAt >= 1 else { return }
    lastSourceProjectionSentAt = now
    guard let tangents = readSourceProjection() else { return }
    peerLock.lock()
    let peer = videoPeer
    peerLock.unlock()
    guard let peer else { return }
    if tangents != lastSourceProjection {
        lastSourceProjection = tangents
        print("SteamVR eye frustum sent to Vision Pro: \(tangents.map { String(format: "%.3f", $0) }.joined(separator: " "))")
    }
    var packet = Data(capacity: 36)
    withUnsafeBytes(of: sourceProjectionMagic.littleEndian) { packet.append(contentsOf: $0) }
    for value in tangents {
        withUnsafeBytes(of: value.bitPattern.littleEndian) { packet.append(contentsOf: $0) }
    }
    peer.send(content: packet, completion: .idempotent)
}

func reportDiagnosticsIfDue() {
    let now = ProcessInfo.processInfo.systemUptime
    let elapsed = now - lastDiagnosticsAt
    guard elapsed >= 5 else { return }
    encoderDiagnosticLock.lock()
    let current = (capture: capturedFrameCount, submit: submittedFrameCount,
        encode: compressedFrameCount, sent: transmittedFrameCount,
        packets: sentVideoPacketCount, bytes: videoBytesSent,
        errors: videoPacketSendErrorCount, drops: droppedTransportFrames, poses: posePacketCount)
    let poseAge = lastPoseReceivedAt > 0 ? now - lastPoseReceivedAt : -1
    encoderDiagnosticLock.unlock()
    peerLock.lock()
    let endpoint = videoPeer.map { String(describing: $0.endpoint) } ?? "none"
    peerLock.unlock()
    let captureFPS = Double(current.capture - lastDiagnostics.capture) / elapsed
    print(String(format: "Pipeline %.1fs source=%u capture=%.1ffps submit=%.1ffps encoded=%.1ffps sent=%.1ffps UDP=%.2fMbps bitrate=%.1fMbps packets=%d sendErrors=%d networkDrops=%d encoderDrops=%d pose=%.1fHz poseAge=%.2fs peer=%@",
        elapsed, dbHeaderPtr.pointee.captureSourceMode, captureFPS,
        Double(current.submit - lastDiagnostics.submit) / elapsed,
        Double(current.encode - lastDiagnostics.encode) / elapsed,
        Double(current.sent - lastDiagnostics.sent) / elapsed,
        Double(current.bytes - lastDiagnostics.bytes) * 8 / elapsed / 1_000_000,
        Double(adaptiveBitrate.current) / 1_000_000,
        current.packets - lastDiagnostics.packets, current.errors - lastDiagnostics.errors,
        current.drops - lastDiagnostics.drops, droppedEncoderFrames,
        Double(current.poses - lastDiagnostics.poses) / elapsed, poseAge, endpoint))
    if videoPeer != nil {
        adaptiveBitrate.update(
            localLossEvents: (current.errors - lastDiagnostics.errors) + (current.drops - lastDiagnostics.drops),
            elapsed: elapsed, session: compressionSession)
    }
    if let compressionSession, captureFPS > 0 {
        VTSessionSetProperty(compressionSession, key: kVTCompressionPropertyKey_ExpectedFrameRate,
            value: NSNumber(value: min(90, max(1, captureFPS.rounded()))))
    }
    lastDiagnostics = current
    lastDiagnosticsAt = now
}

@discardableResult
func resolveOutputSize(sourceWidth: Int, sourceHeight: Int) -> Bool {
    guard !hasExplicitOutputSize, sourceWidth > 0, sourceHeight > 0 else {
        return false
    }

    // Preserve the eye frustum's aspect ratio while keeping the same pixel
    // budget as the configured size. This avoids stretching when the headset supplies an
    // asymmetric per-eye projection.
    let sourceAspect = min(2.5, max(0.5, Double(sourceWidth) / Double(sourceHeight)))
    if let previousAspect = outputSourceAspect,
       abs(sourceAspect - previousAspect) / previousAspect < 0.01 {
        return false
    }
    outputSourceAspect = sourceAspect
    let pixelBudget = Double(configuredOutputWidth * configuredOutputHeight)
    let evenDimension: (Double) -> Int = { value in
        min(4096, max(2, (Int(value.rounded()) + 1) & ~1))
    }
    let previousWidth = outputWidth
    let previousHeight = outputHeight
    outputWidth = evenDimension(sqrt(pixelBudget * sourceAspect))
    outputHeight = evenDimension(sqrt(pixelBudget / sourceAspect))
    return previousWidth != outputWidth || previousHeight != outputHeight
}

// GPU path: a Metal compute kernel scales the RGBA/BGRA capture into the
// encoder's BGRA buffer (bilinear) and handles the channel order through the
// texture formats. The CPU path below (vImage) stays as the fallback; it cost
// 3-5 ms of CPU per frame.
final class GPUFrameConverter {
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLComputePipelineState
    private let textureCache: CVMetalTextureCache
    private var source: MTLTexture?
    private var sourceFormat: MTLPixelFormat = .invalid
    private(set) var convertedFrames = 0

    init?() {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else { return nil }
        let shader = """
        #include <metal_stdlib>
        using namespace metal;
        // Each output pixel averages its whole footprint in the source (up to 4x4 bilinear taps), in linear
        // light (the source is read through an sRGB view), then is sRGB-encoded with a little ordered dither
        // before the 8-bit store: one tap per pixel aliased thin edges (which 4:2:0 chroma turned magenta and
        // green in dark areas) and dark gradients banded.
        kernel void scaleFrame(texture2d<float, access::sample> src [[texture(0)]],
                               texture2d<float, access::write> dst [[texture(1)]],
                               uint2 gid [[thread_position_in_grid]]) {
            if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
            constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);
            float2 dsize = float2(dst.get_width(), dst.get_height()), ssize = float2(src.get_width(), src.get_height());
            float2 scale = ssize / dsize;                        // source pixels per output pixel
            int2 taps = clamp(int2(ceil(scale * 0.5)), 1, 4);    // bilinear taps cover 2 source pixels each
            float4 sum = 0;
            for (int y = 0; y < taps.y; y++)
                for (int x = 0; x < taps.x; x++) {
                    float2 at = (float2(gid) + (float2(x, y) + 0.5) / float2(taps)) / dsize;
                    sum += src.sample(s, at);
                }
            float4 c = sum / float(taps.x * taps.y);
            float3 e = select(1.055 * pow(max(c.rgb, 0.0), 1.0 / 2.4) - 0.055, c.rgb * 12.92, c.rgb <= 0.0031308);
            const float bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
            float d = (bayer[(gid.y & 3) * 4 + (gid.x & 3)] + 0.5) / 16.0 - 0.5;
            dst.write(float4(saturate(e + d / 255.0), c.a), gid);
        }
        """
        var cache: CVMetalTextureCache?
        guard let library = try? device.makeLibrary(source: shader, options: nil),
              let function = library.makeFunction(name: "scaleFrame"),
              let pipeline = try? device.makeComputePipelineState(function: function),
              CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &cache) == kCVReturnSuccess,
              let cache else { return nil }
        self.device = device
        self.queue = queue
        self.pipeline = pipeline
        self.textureCache = cache
    }

    func convert(source bytes: UnsafeMutableRawPointer, dxgiFormat: UInt32, width: Int, height: Int,
                 destination: CVPixelBuffer) -> Bool {
        // (sRGB views: the kernel averages in linear light and encodes the result itself)
        let format: MTLPixelFormat = (dxgiFormat == 28 || dxgiFormat == 29) ? .rgba8Unorm_srgb : .bgra8Unorm_srgb
        if source == nil || source?.width != width || source?.height != height || sourceFormat != format {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: width, height: height, mipmapped: false)
            descriptor.usage = .shaderRead
            descriptor.storageMode = .shared
            guard let texture = device.makeTexture(descriptor: descriptor) else { return false }
            source = texture
            sourceFormat = format
        }
        guard let source else { return false }
        source.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: bytes, bytesPerRow: width * 4)

        var cvTexture: CVMetalTexture?
        let outWidth = CVPixelBufferGetWidth(destination), outHeight = CVPixelBufferGetHeight(destination)
        guard CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, textureCache, destination, nil,
                                                        .bgra8Unorm, outWidth, outHeight, 0, &cvTexture) == kCVReturnSuccess,
              let cvTexture, let target = CVMetalTextureGetTexture(cvTexture),
              let commandBuffer = queue.makeCommandBuffer(),
              let encoder = commandBuffer.makeComputeCommandEncoder() else { return false }
        encoder.setComputePipelineState(pipeline)
        encoder.setTexture(source, index: 0)
        encoder.setTexture(target, index: 1)
        let threads = MTLSize(width: 16, height: 16, depth: 1)
        let groups = MTLSize(width: (outWidth + 15) / 16, height: (outHeight + 15) / 16, depth: 1)
        encoder.dispatchThreadgroups(groups, threadsPerThreadgroup: threads)
        encoder.endEncoding()
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()
        guard commandBuffer.status == .completed else { return false }
        convertedFrames += 1
        return true
    }
}

// VERANTYX_GPU_CONVERT: 1 = Metal, 0 = vImage on the CPU, auto (default) =
// time the first frames on each and keep the faster one for this Mac. The
// game and SteamVR leave different headroom on different chips: an M1 Max is
// CPU-bound with an idle GPU, a base M1 has neither to spare.
let gpuConvertSetting = ProcessInfo.processInfo.environment["VERANTYX_GPU_CONVERT"] ?? "auto"
var gpuConverter: GPUFrameConverter? = gpuConvertSetting == "0" ? nil : GPUFrameConverter()
var gpuConverterAnnounced = false

final class FrameConversionSelector {
    private let probeFrames = 24
    private var gpuSamples: [Double] = []
    private var cpuSamples: [Double] = []
    private(set) var decided = gpuConvertSetting != "auto"

    /// Whether the next frame should use the GPU path.
    func preferGPU() -> Bool {
        if decided { return gpuConverter != nil }
        return gpuSamples.count < probeFrames
    }

    func record(gpu: Bool, milliseconds: Double) {
        guard !decided else { return }
        if gpu { gpuSamples.append(milliseconds) } else { cpuSamples.append(milliseconds) }
        guard gpuSamples.count >= probeFrames, cpuSamples.count >= probeFrames else { return }
        let gpuMedian = gpuSamples.sorted()[probeFrames / 2]
        let cpuMedian = cpuSamples.sorted()[probeFrames / 2]
        decided = true
        // The GPU path also frees the encoder thread; prefer it unless the CPU
        // path is clearly faster (a small chip with a busy GPU).
        let useGPU = gpuConverter != nil && gpuMedian <= cpuMedian * 1.25
        if !useGPU { gpuConverter = nil }
        print(String(format: "Frame conversion: GPU %.2f ms vs CPU %.2f ms per frame; using the %@",
            gpuMedian, cpuMedian, useGPU ? "GPU (Metal)" : "CPU (vImage)"))
    }
}
let conversionSelector = FrameConversionSelector()

func transferFrameOnGPU(
    source: UnsafeMutableRawPointer, sourceFormat: UInt32, sourceWidth: Int, sourceHeight: Int,
    destination: CVPixelBuffer
) -> Bool {
    guard let converter = gpuConverter else { return false }
    guard converter.convert(source: source, dxgiFormat: sourceFormat, width: sourceWidth, height: sourceHeight,
                            destination: destination) else {
        print("GPU frame conversion failed; converting frames on the CPU from now on")
        gpuConverter = nil
        return false
    }
    if !gpuConverterAnnounced {
        gpuConverterAnnounced = true
        print("Frames are scaled and converted on the GPU (Metal)")
    }
    return true
}

func copyFrameToBGRA(
    source: UnsafeMutableRawPointer,
    sourceFormat: UInt32,
    sourceWidth: Int,
    sourceHeight: Int,
    destination: UnsafeMutableRawPointer,
    destinationBytesPerRow: Int
) -> Bool {
    guard sourceWidth > 0, sourceHeight > 0,
          sourceWidth <= 8192, sourceHeight <= 4096,
          sourceFormat == 87 || sourceFormat == 91 || sourceFormat == 28 || sourceFormat == 29
    else { return false }

    var destinationBuffer = vImage_Buffer(
        data: destination,
        height: vImagePixelCount(outputHeight),
        width: vImagePixelCount(outputWidth),
        rowBytes: destinationBytesPerRow
    )

    let isRGBA = sourceFormat == 28 || sourceFormat == 29
    if sourceWidth == outputWidth && sourceHeight == outputHeight {
        for row in 0..<outputHeight {
            memcpy(destination.advanced(by: row * destinationBytesPerRow),
                   source.advanced(by: row * sourceWidth * 4),
                   outputWidth * 4)
        }
    } else {
        var sourceBuffer = vImage_Buffer(
            data: source,
            height: vImagePixelCount(sourceHeight),
            width: vImagePixelCount(sourceWidth),
            rowBytes: sourceWidth * 4
        )
        guard vImageScale_ARGB8888(
            &sourceBuffer,
            &destinationBuffer,
            nil,
            vImage_Flags(kvImageNoFlags)
        ) == kvImageNoError else { return false }
    }

    // DXGI format 28/29 is RGBA, while VideoToolbox expects BGRA. Scale first,
    // then swap R/B in the smaller output buffer to avoid a full-size temporary.
    // vImage does this with SIMD; the old per-byte loop cost several ms a frame.
    if isRGBA {
        let swapRedBlue: [UInt8] = [2, 1, 0, 3]
        guard vImagePermuteChannels_ARGB8888(
            &destinationBuffer, &destinationBuffer, swapRedBlue, vImage_Flags(kvImageNoFlags)
        ) == kvImageNoError else { return false }
    }
    return true
}

// MARK: - Joy-Con UDP Server
DispatchQueue.global(qos: .userInteractive).async {
    let jcSock = socket(AF_INET, SOCK_DGRAM, 0)
    var opt: Int32 = 1
    setsockopt(jcSock, SOL_SOCKET, SO_REUSEADDR, &opt, socklen_t(MemoryLayout<Int32>.size))
    
    var bindAddr = sockaddr_in()
    bindAddr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
    bindAddr.sin_family = sa_family_t(AF_INET)
    bindAddr.sin_port = in_port_t(11002).bigEndian
    bindAddr.sin_addr.s_addr = INADDR_ANY
    
    bind(jcSock, withUnsafePointer(to: &bindAddr) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { $0 }
    }, socklen_t(MemoryLayout<sockaddr_in>.size))
    
    var buffer = [UInt8](repeating: 0, count: 2048)
    while true {
        var senderAddr = sockaddr()
        var senderLen = socklen_t(MemoryLayout<sockaddr>.size)
        let bytesRead = recvfrom(jcSock, &buffer, buffer.count, 0, &senderAddr, &senderLen)
        
        var ipStr = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
        let senderAddrIn = withUnsafePointer(to: &senderAddr) {
            $0.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee }
        }
        var sin_addr = senderAddrIn.sin_addr
        inet_ntop(AF_INET, &sin_addr, &ipStr, socklen_t(INET_ADDRSTRLEN))
        let senderIP = String(cString: ipStr)
        
        if senderIP == "127.0.0.1" && bytesRead == MemoryLayout<JoyconPacket>.size {
            buffer.withUnsafeBytes { rawBuffer in
                let jc = rawBuffer.load(as: JoyconPacket.self)
                if jc.magic == UInt32(0x4A4F5943) { // "JOYC"
                    // SharedHands offsets:
                    // 0-191: Matrices; 192-195: hand tracking and trigger state
                    // 196: rightButtons, 200: leftButtons
                    // 204: rightStickX, 208: rightStickY, 212: leftStickX, 216: leftStickY
                    // 220: rightVel, 232: leftVel
                    withSharedHandsWrite {
                        handsMapPtr.advanced(by: 196).storeBytes(of: jc.rightButtons, as: UInt32.self)
                        handsMapPtr.advanced(by: 200).storeBytes(of: jc.leftButtons, as: UInt32.self)
                        handsMapPtr.advanced(by: 204).storeBytes(of: jc.rightStickX, as: Float.self)
                        handsMapPtr.advanced(by: 208).storeBytes(of: jc.rightStickY, as: Float.self)
                        handsMapPtr.advanced(by: 212).storeBytes(of: jc.leftStickX, as: Float.self)
                        handsMapPtr.advanced(by: 216).storeBytes(of: jc.leftStickY, as: Float.self)
                        handsMapPtr.advanced(by: 220).storeBytes(of: jc.rightVelocityX, as: Float.self)
                        handsMapPtr.advanced(by: 224).storeBytes(of: jc.rightVelocityY, as: Float.self)
                        handsMapPtr.advanced(by: 228).storeBytes(of: jc.rightVelocityZ, as: Float.self)
                        handsMapPtr.advanced(by: 232).storeBytes(of: jc.leftVelocityX, as: Float.self)
                        handsMapPtr.advanced(by: 236).storeBytes(of: jc.leftVelocityY, as: Float.self)
                        handsMapPtr.advanced(by: 240).storeBytes(of: jc.leftVelocityZ, as: Float.self)
                    }
                }
            }
        }
    }
}


struct VRPosePacket {
    var magic: UInt32 // 0x45534F50 ("POSE" little endian)
    var padding: UInt32
    var timestamp: Double
    var headTransform: (Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float)
    var leftHandTransform: (Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float)
    var rightHandTransform: (Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float)
    var leftPinch: UInt8
    var rightPinch: UInt8
}

func receiveVideoPeerPacket(_ packet: Data, from connection: NWConnection) {
    peerLock.lock()
    let retired = retiredVideoPeers.contains(connection)
    peerLock.unlock()
    guard !retired else { return }
    if packet.starts(with: Data("CLIENT_STATS ".utf8)) {
        // Only the active headset can report diagnostics. Bound both the log
        // frequency and payload, and prevent remote newlines from forging rows.
        let now = ProcessInfo.processInfo.systemUptime
        peerLock.lock()
        let shouldLog = videoPeer === connection && now - lastClientDiagnosticsAt >= 4
        if shouldLog { lastClientDiagnosticsAt = now }
        peerLock.unlock()
        // Lossy decoding keeps a message whose multibyte text was cut off.
        let message = String(decoding: packet.prefix(1200), as: UTF8.self)
        if shouldLog {
            let singleLine = message.components(separatedBy: .controlCharacters).joined(separator: " ")
            print(String(format: "Headset diagnostics uptime=%.3f %@", now, singleLine))
        }
        adaptiveBitrate.noteHeadsetStatistics(message)
        return
    }
    if packet == Data("HELLO_VR".utf8) {
        registerVideoPeer(connection, handshake: true)
        return
    }
    if packet == Data("REQUEST_KEYFRAME".utf8) {
        registerVideoPeer(connection, handshake: true)
        return
    }
    if packet.count >= 271 {
        packet.withUnsafeBytes { rawBuffer in
            guard let baseAddr = rawBuffer.baseAddress else { return }
            var magic: UInt32 = 0
            memcpy(&magic, baseAddr, 4)

            if magic == 0x45534F50 || magic == 0x504F5345 {
                encoderDiagnosticLock.lock()
                posePacketCount += 1
                let posePacketsReceived = posePacketCount
                lastPoseReceivedAt = ProcessInfo.processInfo.systemUptime
                encoderDiagnosticLock.unlock()
                if posePacketsReceived == 1 || posePacketsReceived % 900 == 0 {
                    print("Received Vision Pro pose packet #\(posePacketsReceived) (\(packet.count) bytes)")
                }
                let visionTs = rawBuffer.loadUnaligned(fromByteOffset: 8, as: Double.self)

                // Vision Pro payload: magic(4) + pad(4) + ts(8) = 16 bytes offset
                let leftTracked = rawBuffer.loadUnaligned(fromByteOffset: 208, as: UInt8.self)
                let rightTracked = rawBuffer.loadUnaligned(fromByteOffset: 209, as: UInt8.self)
                let leftTrigger = rawBuffer.loadUnaligned(fromByteOffset: 210, as: UInt8.self)
                let rightTrigger = rawBuffer.loadUnaligned(fromByteOffset: 211, as: UInt8.self)
                let rightButtons = rawBuffer.loadUnaligned(fromByteOffset: 212, as: UInt32.self)
                let leftButtons = rawBuffer.loadUnaligned(fromByteOffset: 216, as: UInt32.self)
                let rightStickX = rawBuffer.loadUnaligned(fromByteOffset: 220, as: Float.self)
                let rightStickY = rawBuffer.loadUnaligned(fromByteOffset: 224, as: Float.self)
                let leftStickX = rawBuffer.loadUnaligned(fromByteOffset: 228, as: Float.self)
                let leftStickY = rawBuffer.loadUnaligned(fromByteOffset: 232, as: Float.self)
                let leftGrip = rawBuffer.loadUnaligned(fromByteOffset: 268, as: UInt8.self)
                let rightGrip = rawBuffer.loadUnaligned(fromByteOffset: 269, as: UInt8.self)
                let headTracked = rawBuffer.loadUnaligned(fromByteOffset: 270, as: UInt8.self)
                withSharedHandsWrite {
                    memcpy(handsMapPtr, baseAddr + 16, 64)       // Head (offset 0)
                    memcpy(handsMapPtr + 64, baseAddr + 80, 64)  // Left (offset 64)
                    memcpy(handsMapPtr + 128, baseAddr + 144, 64)// Right (offset 128)
                    handsMapPtr.advanced(by: 192).storeBytes(of: leftTracked, as: UInt8.self)
                    handsMapPtr.advanced(by: 193).storeBytes(of: rightTracked, as: UInt8.self)
                    handsMapPtr.advanced(by: 194).storeBytes(of: leftTrigger, as: UInt8.self)
                    handsMapPtr.advanced(by: 195).storeBytes(of: rightTrigger, as: UInt8.self)
                    handsMapPtr.advanced(by: 196).storeBytes(of: rightButtons, as: UInt32.self)
                    handsMapPtr.advanced(by: 200).storeBytes(of: leftButtons, as: UInt32.self)
                    handsMapPtr.advanced(by: 204).storeBytes(of: rightStickX, as: Float.self)
                    handsMapPtr.advanced(by: 208).storeBytes(of: rightStickY, as: Float.self)
                    handsMapPtr.advanced(by: 212).storeBytes(of: leftStickX, as: Float.self)
                    handsMapPtr.advanced(by: 216).storeBytes(of: leftStickY, as: Float.self)
                    handsMapPtr.advanced(by: 244).storeBytes(of: visionTs, as: Double.self)
                    handsMapPtr.advanced(by: 284).storeBytes(of: leftGrip, as: UInt8.self)
                    handsMapPtr.advanced(by: 285).storeBytes(of: rightGrip, as: UInt8.self)
                    handsMapPtr.advanced(by: 286).storeBytes(of: headTracked, as: UInt8.self)
                    if packet.count >= 268 {
                        memcpy(handsMapPtr + 252, baseAddr + 236, 16)
                        memcpy(handsMapPtr + 268, baseAddr + 252, 16)
                    }
                    // Optional velocities: left/right linear, left/right angular.
                    if packet.count >= 319 {
                        memcpy(handsMapPtr + 232, baseAddr + 271, 12) // leftVelocity
                        memcpy(handsMapPtr + 220, baseAddr + 283, 12) // rightVelocity
                        memcpy(handsMapPtr + 287, baseAddr + 295, 24) // angular left, right
                        handsMapPtr.advanced(by: 311).storeBytes(of: UInt8(1), as: UInt8.self)
                    } else {
                        handsMapPtr.advanced(by: 311).storeBytes(of: UInt8(0), as: UInt8.self)
                    }
                }

                registerVideoPeer(connection, handshake: false)
            }
        }
    }
}

func receiveFromVideoPeer(_ connection: NWConnection) {
    connection.receiveMessage { content, _, _, error in
        if let content { receiveVideoPeerPacket(content, from: connection) }
        if let error {
            print("Vision Pro receive failed: \(error)")
            connection.cancel()
        } else {
            receiveFromVideoPeer(connection)
        }
    }
}

let networkParameters = NWParameters.udp
networkParameters.includePeerToPeer = true
let videoListener: NWListener
do {
    videoListener = try NWListener(using: networkParameters, on: 9999)
} catch {
    print("Could not listen for Vision Pro on port 9999: \(error)")
    exit(1)
}
videoListener.service = NWListener.Service(name: "VerantyxVR", type: "_verantyxvr._udp")
videoListener.stateUpdateHandler = { state in
    if case .ready = state {
        print("Published Bonjour _verantyxvr._udp on 9999 using peer-to-peer Network.framework")
    } else if case .failed(let error) = state {
        print("Vision Pro listener failed: \(error)")
        exit(1)
    }
}
videoListener.newConnectionHandler = { connection in
    connection.stateUpdateHandler = { state in
        if case .ready = state {
            print("Vision Pro flow ready: \(connection.endpoint), path=\(String(describing: connection.currentPath))")
        }
        if case .failed(let error) = state {
            print("Vision Pro flow failed: \(error)")
            connection.cancel()
        }
        if case .cancelled = state {
            peerLock.lock()
            retiredVideoPeers.add(connection)
            if videoPeer === connection { videoPeer = nil; peerNeedsKeyFrame = true }
            peerLock.unlock()
            connection.stateUpdateHandler = nil
        }
    }
    connection.start(queue: transportQueue)
    receiveFromVideoPeer(connection)
}
videoListener.start(queue: transportQueue)

// MARK: - Game audio to the headset

/// Captures the Mac's audio output with a Core Audio process tap and sends it
/// as 16-bit PCM "AUDI" datagrams: magic, sequence, sample rate, channels,
/// frame count, then interleaved samples. The Mac's own speakers are muted
/// while the tap runs so the game is heard only in the headset.
final class GameAudioStreamer {
    private var tapID = AudioObjectID(kAudioObjectUnknown)
    private var aggregateID = AudioObjectID(kAudioObjectUnknown)
    private var procID: AudioDeviceIOProcID?
    private var sequence: UInt32 = 0
    private var pending: [Int16] = []
    private var sampleRate: UInt32 = 48_000
    private var channels: UInt32 = 2
    private var sentPackets = 0
    private var sentPacketsAtLastLog = 0
    private var lastLogAt: TimeInterval = 0
    // Loudest sample since the last log line. A global tap delivers silence
    // at the full packet rate when nothing on the Mac plays sound, so the
    // packet count alone says nothing about whether the game is audible.
    private var peakSinceLog = 0
    private let countersLock = NSLock()
    private let framesPerPacket = 480 // 10 ms at 48 kHz; 1920 bytes of stereo PCM (half the packets of 5 ms)

    /// Packets sent so far (any thread).
    var packetsSent: Int {
        countersLock.lock()
        defer { countersLock.unlock() }
        return sentPackets
    }

    func start() {
        guard ProcessInfo.processInfo.environment["VERANTYX_AUDIO"] != "0" else {
            print("Game audio streaming disabled (VERANTYX_AUDIO=0)")
            return
        }
        guard #available(macOS 14.2, *) else {
            print("Game audio streaming needs macOS 14.2 or later")
            return
        }
        let description = CATapDescription(stereoGlobalTapButExcludeProcesses: [])
        description.name = "Verantyx VR headset audio"
        description.muteBehavior = ProcessInfo.processInfo.environment["VERANTYX_AUDIO_KEEP_MAC_SPEAKERS"] == "1"
            ? .unmuted : .mutedWhenTapped
        description.isPrivate = true
        var status = AudioHardwareCreateProcessTap(description, &tapID)
        guard status == noErr else {
            print("Game audio: could not create the system audio tap (\(status)). Allow this terminal under System Settings > Privacy & Security > Screen & System Audio Recording, then relaunch.")
            return
        }

        var format = AudioStreamBasicDescription()
        var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        var address = AudioObjectPropertyAddress(mSelector: kAudioTapPropertyFormat,
                                                 mScope: kAudioObjectPropertyScopeGlobal,
                                                 mElement: kAudioObjectPropertyElementMain)
        status = AudioObjectGetPropertyData(tapID, &address, 0, nil, &size, &format)
        guard status == noErr, format.mSampleRate > 0, format.mChannelsPerFrame > 0 else {
            print("Game audio: could not read the tap format (\(status))")
            return
        }
        sampleRate = UInt32(format.mSampleRate)
        channels = min(2, format.mChannelsPerFrame)

        let aggregate: [String: Any] = [
            kAudioAggregateDeviceNameKey as String: "Verantyx VR tap",
            kAudioAggregateDeviceUIDKey as String: "com.verantyx.vr.audio-tap",
            kAudioAggregateDeviceIsPrivateKey as String: true,
            kAudioAggregateDeviceTapAutoStartKey as String: true,
            kAudioAggregateDeviceTapListKey as String: [[
                kAudioSubTapUIDKey as String: description.uuid.uuidString,
                kAudioSubTapDriftCompensationKey as String: true,
            ]],
        ]
        status = AudioHardwareCreateAggregateDevice(aggregate as CFDictionary, &aggregateID)
        guard status == noErr else {
            print("Game audio: could not create the tap device (\(status))")
            return
        }
        let isFloat = format.mFormatFlags & kAudioFormatFlagIsFloat != 0
        let isInterleaved = format.mFormatFlags & kAudioFormatFlagIsNonInterleaved == 0
        let sourceChannels = Int(format.mChannelsPerFrame)
        status = AudioDeviceCreateIOProcIDWithBlock(&procID, aggregateID, transportQueue) { [self] _, input, _, _, _ in
            handle(input, isFloat: isFloat, interleaved: isInterleaved, sourceChannels: sourceChannels)
        }
        guard status == noErr, let procID else {
            print("Game audio: could not attach to the tap device (\(status))")
            return
        }
        status = AudioDeviceStart(aggregateID, procID)
        guard status == noErr else {
            print("Game audio: could not start the tap device (\(status))")
            return
        }
        print("Game audio: streaming \(sampleRate) Hz \(channels)ch PCM to the headset; Mac speakers \(description.muteBehavior == .unmuted ? "stay on" : "muted while streaming")")
    }

    private func handle(_ input: UnsafePointer<AudioBufferList>, isFloat: Bool, interleaved: Bool, sourceChannels: Int) {
        let buffers = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: input))
        guard let first = buffers.first, let data = first.mData else { return }
        let outChannels = Int(channels)
        if interleaved {
            let frames = Int(first.mDataByteSize) / (sourceChannels * (isFloat ? 4 : 2))
            pending.reserveCapacity(pending.count + frames * outChannels)
            if isFloat {
                let samples = data.assumingMemoryBound(to: Float.self)
                for frame in 0..<frames {
                    for channel in 0..<outChannels {
                        pending.append(Self.int16(samples[frame * sourceChannels + min(channel, sourceChannels - 1)]))
                    }
                }
            } else {
                let samples = data.assumingMemoryBound(to: Int16.self)
                for frame in 0..<frames {
                    for channel in 0..<outChannels {
                        pending.append(samples[frame * sourceChannels + min(channel, sourceChannels - 1)])
                    }
                }
            }
        } else {
            let frames = Int(first.mDataByteSize) / (isFloat ? 4 : 2)
            pending.reserveCapacity(pending.count + frames * outChannels)
            for frame in 0..<frames {
                for channel in 0..<outChannels {
                    let buffer = buffers[min(channel, buffers.count - 1)]
                    guard let channelData = buffer.mData else { continue }
                    if isFloat {
                        pending.append(Self.int16(channelData.assumingMemoryBound(to: Float.self)[frame]))
                    } else {
                        pending.append(channelData.assumingMemoryBound(to: Int16.self)[frame])
                    }
                }
            }
        }
        flush()
    }

    private static func int16(_ value: Float) -> Int16 {
        Int16(max(-32768, min(32767, (value * 32767).rounded())))
    }

    private func flush() {
        let samplesPerPacket = framesPerPacket * Int(channels)
        guard pending.count >= samplesPerPacket else { return }
        peerLock.lock()
        let peer = videoPeer
        peerLock.unlock()
        guard let peer else {
            // Nobody listening: keep only the newest packet's worth.
            pending.removeFirst(pending.count - pending.count % samplesPerPacket)
            return
        }
        while pending.count >= samplesPerPacket {
            var packet = Data(capacity: 20 + samplesPerPacket * 2)
            for value in [UInt32(0x49445541), sequence, sampleRate, channels, UInt32(framesPerPacket)] {
                withUnsafeBytes(of: value.littleEndian) { packet.append(contentsOf: $0) }
            }
            pending[0..<samplesPerPacket].withUnsafeBytes { packet.append(contentsOf: $0) }
            var peak = 0
            for sample in pending[0..<samplesPerPacket] { peak = max(peak, abs(Int(sample))) }
            peakSinceLog = max(peakSinceLog, peak)
            pending.removeFirst(samplesPerPacket)
            sequence &+= 1
            peer.send(content: packet, completion: .idempotent)
            countersLock.lock()
            sentPackets += 1
            countersLock.unlock()
        }
        let now = ProcessInfo.processInfo.systemUptime
        if now - lastLogAt >= 30 {
            let elapsed = lastLogAt > 0 ? now - lastLogAt : 0
            lastLogAt = now
            let total = packetsSent
            let recent = total - sentPacketsAtLastLog
            sentPacketsAtLastLog = total
            let peakDescription = peakSinceLog > 0
                ? String(format: "peak %.1f dBFS", 20 * log10(Double(peakSinceLog) / 32768))
                : "SILENT: nothing on the Mac played sound (check the game's audio device and volume)"
            peakSinceLog = 0
            print(String(format: "Game audio: %d packets sent (%d in the last %.0f s, %@)", total, recent, elapsed, peakDescription))
        }
    }
}

let gameAudio = GameAudioStreamer()
gameAudio.start()

// MARK: - Frame-rate statistics for the headset's overlay

// "STAT" once a second: the game's capture rate, the encoded and sent rates,
// the current HEVC bitrate and the audio packet rate, as little-endian
// Float32 values after the magic (24 bytes; VRUDPReceiver.swift reads it).
let streamStatsMagic: UInt32 = 0x54415453 // "STAT"
var lastStreamStatsAt: TimeInterval = -.infinity
var lastStreamStatsCounts = (capture: 0, encode: 0, sent: 0, audio: 0)

func sendStreamStatsIfDue() {
    let now = ProcessInfo.processInfo.systemUptime
    let elapsed = now - lastStreamStatsAt
    guard elapsed >= 1 else { return }
    encoderDiagnosticLock.lock()
    let counts = (capture: capturedFrameCount, encode: compressedFrameCount,
                  sent: transmittedFrameCount, audio: gameAudio.packetsSent)
    encoderDiagnosticLock.unlock()
    defer {
        lastStreamStatsCounts = counts
        lastStreamStatsAt = now
    }
    // The first call only seeds the counters.
    guard lastStreamStatsAt.isFinite else { return }
    peerLock.lock()
    let peer = videoPeer
    peerLock.unlock()
    guard let peer else { return }
    let rates: [Float] = [
        Float(Double(counts.capture - lastStreamStatsCounts.capture) / elapsed),
        Float(Double(counts.encode - lastStreamStatsCounts.encode) / elapsed),
        Float(Double(counts.sent - lastStreamStatsCounts.sent) / elapsed),
        Float(Double(adaptiveBitrate.current) / 1_000_000),
        Float(Double(counts.audio - lastStreamStatsCounts.audio) / elapsed),
    ]
    var packet = Data(capacity: 24)
    withUnsafeBytes(of: streamStatsMagic.littleEndian) { packet.append(contentsOf: $0) }
    for value in rates {
        withUnsafeBytes(of: value.bitPattern.littleEndian) { packet.append(contentsOf: $0) }
    }
    peer.send(content: packet, completion: .idempotent)
}

print("Starting native read loop and UDP transmission on port 9999...")

while isEncoding {
    autoreleasepool {
        reportDiagnosticsIfDue()
        sendSourceProjectionIfDue()
        sendStreamStatsIfDue()
        let sourceMode = dbHeaderPtr.pointee.captureSourceMode
        if sourceMode != lastCaptureSourceMode {
            print("Capture source changed \(lastCaptureSourceMode)->\(sourceMode); continuing stream sequence \(streamSequence)")
            lastCaptureSourceMode = sourceMode
            lastSeq = 0
            requestKeyFrame()
        }
        let readyIndex = dbHeaderPtr.pointee.latestReadyIndex
        OSMemoryBarrier()
        if readyIndex < 2 {
            let headerPtr = headers[Int(readyIndex)]
            let currentSeq = headerPtr.pointee.sequenceNumber
            
            if currentSeq != lastSeq && currentSeq > 0 {
                let frameHeader = headerPtr.pointee
                let width = Int(frameHeader.width)
                let height = Int(frameHeader.height)
                encoderDiagnosticLock.lock()
                capturedFrameCount += 1
                encoderDiagnosticLock.unlock()

                // (Wider than 4096 for 2.5K per eye side by side; a frame block holds 4096 x 4096 x 8 bytes.)
                guard width > 0, height > 0, width <= 8192, height <= 4096, width * height * 4 <= 4096 * 4096 * 8 else {
                    lastSeq = currentSeq
                    return
                }

                let outputSizeChanged = resolveOutputSize(sourceWidth: width, sourceHeight: height)
                if outputSizeChanged, let oldSession = compressionSession {
                    // SteamVR can switch to the headset's measured frustum after
                    // the first Vision Pro pose arrives. Drain the old session
                    // before changing pixel-buffer dimensions and its HEVC SPS.
                    // Complete first: an encoder may hold its last output until
                    // this call, so waiting for all callbacks first can deadlock.
                    VTCompressionSessionCompleteFrames(oldSession, untilPresentationTimeStamp: .invalid)
                    for _ in 0..<3 { encoderInFlightFrames.wait() }
                    VTCompressionSessionInvalidate(oldSession)
                    compressionSession = nil
                    pixelBufferPool = nil
                    for _ in 0..<3 { encoderInFlightFrames.signal() }
                    print("Reconfiguring HEVC encoder for source aspect \(String(format: "%.3f", Double(width) / Double(height)))")
                }

                if compressionSession == nil {
                    resolveOutputSize(sourceWidth: width, sourceHeight: height)
                    print("Initializing fixed-size VideoToolbox encoder for \(outputWidth)x\(outputHeight)...")
                    fflush(stdout)
                    setupEncoder(width: Int32(outputWidth), height: Int32(outputHeight))
                }

                if pixelBufferPool == nil {
                    pixelBufferPool = makePixelBufferPool(width: outputWidth, height: outputHeight)
                }

                guard encoderInFlightFrames.wait(timeout: .now()) == .success else {
                    droppedEncoderFrames += 1
                    lastSeq = currentSeq
                    if droppedEncoderFrames == 1 || droppedEncoderFrames % 90 == 0 {
                        print("Dropped capture frame: VideoToolbox has 3 frames in flight (total dropped=\(droppedEncoderFrames))")
                    }
                    return
                }

                guard let pool = pixelBufferPool else {
                    encoderInFlightFrames.signal()
                    print("Could not create VideoToolbox input pixel buffer pool")
                    lastSeq = currentSeq
                    return
                }
                var pixelBuffer: CVPixelBuffer?
                let poolStatus = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixelBuffer)
                guard poolStatus == kCVReturnSuccess, let pb = pixelBuffer else {
                    encoderInFlightFrames.signal()
                    droppedEncoderFrames += 1
                    lastSeq = currentSeq
                    if droppedEncoderFrames == 1 || droppedEncoderFrames % 90 == 0 {
                        print("Could not acquire VideoToolbox pixel buffer: \(poolStatus) (total dropped=\(droppedEncoderFrames))")
                    }
                    return
                }
                let sourceFormat = frameHeader.format
                let conversionStart = ProcessInfo.processInfo.systemUptime
                let tryGPU = conversionSelector.preferGPU()
                var didCopy = tryGPU && transferFrameOnGPU(
                    source: pixelDatas[Int(readyIndex)], sourceFormat: sourceFormat,
                    sourceWidth: width, sourceHeight: height, destination: pb)
                if didCopy {
                    conversionSelector.record(gpu: true, milliseconds: (ProcessInfo.processInfo.systemUptime - conversionStart) * 1000)
                }
                if !didCopy {
                    guard CVPixelBufferLockBaseAddress(pb, []) == kCVReturnSuccess else {
                        encoderInFlightFrames.signal()
                        lastSeq = currentSeq
                        return
                    }
                    if let destination = CVPixelBufferGetBaseAddress(pb) {
                        didCopy = copyFrameToBGRA(
                            source: pixelDatas[Int(readyIndex)],
                            sourceFormat: sourceFormat,
                            sourceWidth: width,
                            sourceHeight: height,
                            destination: destination,
                            destinationBytesPerRow: CVPixelBufferGetBytesPerRow(pb)
                        )
                    }
                    CVPixelBufferUnlockBaseAddress(pb, [])
                    if didCopy {
                        conversionSelector.record(gpu: false, milliseconds: (ProcessInfo.processInfo.systemUptime - conversionStart) * 1000)
                    }
                }
                guard didCopy else {
                    encoderInFlightFrames.signal()
                    print("Could not convert SteamVR DXGI frame \(width)x\(height), format \(sourceFormat)")
                    lastSeq = currentSeq
                    return
                }

                // The two shared buffers can be reused if encoding falls behind. Drop
                // a frame if its producer overwrote this slot while we copied it.
                OSMemoryBarrier()
                guard headerPtr.pointee.sequenceNumber == currentSeq,
                      dbHeaderPtr.pointee.captureSourceMode == sourceMode else {
                    encoderInFlightFrames.signal()
                    lastSeq = currentSeq
                    return
                }

                streamSequence &+= 1
                let outputSequence = streamSequence
                let now = ProcessInfo.processInfo.systemUptime
                // Capture is irregular under translation; numbering frames at
                // 90fps would misstate bitrate and keyframe timing by >20x at4fps.
                let presentationSeconds = max(now - presentationEpoch, lastPresentationSeconds + 0.000001)
                let presentationTime = CMTime(seconds: presentationSeconds, preferredTimescale: 1_000_000)
                lastPresentationSeconds = presentationSeconds
                let currentVisionTimestamp = frameHeader.visionTimestamp
                let currentTransform = frameHeader.renderHeadTransform

                let refConPtr = UnsafeMutablePointer<FrameContext>.allocate(capacity: 1)
                refConPtr.initialize(to: FrameContext(
                    sequence: outputSequence,
                    visionTimestamp: currentVisionTimestamp,
                    transform: currentTransform,
                    pixelBuffer: pb
                ))

                var frameProps: CFDictionary? = nil
                // Keyframes only on request (new peer, loss, decoder recovery);
                // VideoToolbox adds the periodic safety IDR on its own.
                if now - lastForcedKeyFrameAt >= 0.1 && consumeKeyFrameRequest() {
                    let dict: [CFString: Any] = [kVTEncodeFrameOptionKey_ForceKeyFrame: true]
                    frameProps = dict as CFDictionary
                    lastForcedKeyFrameAt = now
                }

                // The frames are sRGB-encoded RGB (BT.709 primaries): say so, so VideoToolbox converts them to
                // YCbCr with the same matrix the headset's decoder uses to convert back.
                CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, .shouldPropagate)
                CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_sRGB, .shouldPropagate)
                CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, .shouldPropagate)
                let status = VTCompressionSessionEncodeFrame(compressionSession!,
                                                             imageBuffer: pb,
                                                             presentationTimeStamp: presentationTime,
                                                             duration: CMTime.invalid,
                                                             frameProperties: frameProps,
                                                             sourceFrameRefcon: UnsafeMutableRawPointer(refConPtr),
                                                             infoFlagsOut: nil)

                if framesEncoded < 3 || framesEncoded % 90 == 0 {
                    print("Queued capture frame seq=\(currentSeq), source=\(width)x\(height) format=\(sourceFormat), status=\(status)")
                }

                if status != noErr {
                    print("VTCompressionSessionEncodeFrame failed: \(status)")
                    refConPtr.deinitialize(count: 1)
                    refConPtr.deallocate()
                    encoderInFlightFrames.signal()
                    requestKeyFrame()
                } else {
                    encoderDiagnosticLock.lock()
                    submittedFrameCount += 1
                    encoderDiagnosticLock.unlock()
                }

                framesEncoded += 1
                lastSeq = currentSeq
            } else {
                // Weak memory ordering on Apple Silicon: latestReadyIndex was updated, 
                // but sequenceNumber isn't visible yet. Do NOT update lastReadIndex, 
                // yield briefly and try again.
                usleep(100)
            }
        } else {
            usleep(1000)
        }
    }
}
