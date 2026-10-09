import Foundation
setvbuf(stdout, nil, _IONBF, 0)
import VideoToolbox
import CoreMedia
import Network
import Accelerate

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
            // Limit keyframe bursts to 24 Mbps. In-flight completion alone only
            // bounds the userspace queue; it does not mean radio delivery.
            nextSendAt += Double(packet.count) / 3_000_000
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

func setupEncoder(width: Int32, height: Int32) {
    let status = VTCompressionSessionCreate(allocator: kCFAllocatorDefault,
                                            width: width,
                                            height: height,
                                            codecType: kCMVideoCodecType_HEVC,
                                            encoderSpecification: [kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: true] as CFDictionary,
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
    
    if status != noErr {
        print("Failed to create VTCompressionSession")
        exit(1)
    }
    
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_HEVC_Main_AutoLevel)
    
    // Critical settings for ultra-low latency VR streaming
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse) // Disable B-frames
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, value: kCFBooleanTrue)
    
    var bitrate: Int32 = 12_000_000 // Keep AWDL delivery smooth while the headset decodes both eyes.
    let bitrateNum = CFNumberCreate(kCFAllocatorDefault, .sInt32Type, &bitrate)
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_AverageBitRate, value: bitrateNum)
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_DataRateLimits,
        value: [1_500_000, 1] as CFArray)
    
    // This is an initial rate-control hint, not the display refresh rate. The
    // main loop updates it from observed capture cadence every five seconds.
    var expectedFPS: Int32 = 90
    let expectedFPSNum = CFNumberCreate(kCFAllocatorDefault, .sInt32Type, &expectedFPS)
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: expectedFPSNum)
    
    // Bound recovery in elapsed time even when the game only produces 4fps.
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: 45 as CFNumber)
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, value: 0.5 as CFNumber)
    
    // FORCE ZERO FRAME DELAY in the encoder queue
    var maxFrameDelayCount: Int32 = 1
    let maxFrameDelayCountNum = CFNumberCreate(kCFAllocatorDefault, .sInt32Type, &maxFrameDelayCount)
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_MaxFrameDelayCount, value: maxFrameDelayCountNum)

    
    VTCompressionSessionPrepareToEncodeFrames(compressionSession!)
    requestKeyFrame()
}

// MARK: - Main IPC Loop

let winePrefix = ProcessInfo.processInfo.environment["WINEPREFIX"]
    ?? (NSHomeDirectory() + "/Verantyx_VR_Drive/SteamVR_Prefix")
let filePath = URL(fileURLWithPath: winePrefix)
    .appendingPathComponent("drive_c/vr_shared_frame.dat")
    .path
let sharedHandsSize = 287 // Keep in sync with SharedHands in driver_verantyx.cpp.
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
let configuredOutputWidth = Int(ProcessInfo.processInfo.environment["VERANTYX_STREAM_WIDTH"] ?? "1440") ?? 1440
let configuredOutputHeight = Int(ProcessInfo.processInfo.environment["VERANTYX_STREAM_HEIGHT"] ?? "810") ?? 810
let hasExplicitOutputSize = ProcessInfo.processInfo.environment["VERANTYX_STREAM_WIDTH"] != nil ||
    ProcessInfo.processInfo.environment["VERANTYX_STREAM_HEIGHT"] != nil
var outputWidth = min(4096, max(2, configuredOutputWidth & ~1))
var outputHeight = min(4096, max(2, configuredOutputHeight & ~1))
var outputSourceAspect: Double?

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
    print(String(format: "Pipeline %.1fs source=%u capture=%.1ffps submit=%.1ffps encoded=%.1ffps sent=%.1ffps UDP=%.2fMbps packets=%d sendErrors=%d networkDrops=%d encoderDrops=%d pose=%.1fHz poseAge=%.2fs peer=%@",
        elapsed, dbHeaderPtr.pointee.captureSourceMode, captureFPS,
        Double(current.submit - lastDiagnostics.submit) / elapsed,
        Double(current.encode - lastDiagnostics.encode) / elapsed,
        Double(current.sent - lastDiagnostics.sent) / elapsed,
        Double(current.bytes - lastDiagnostics.bytes) * 8 / elapsed / 1_000_000,
        current.packets - lastDiagnostics.packets, current.errors - lastDiagnostics.errors,
        current.drops - lastDiagnostics.drops, droppedEncoderFrames,
        Double(current.poses - lastDiagnostics.poses) / elapsed, poseAge, endpoint))
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
    // budget as 1440x810. This avoids stretching when the headset supplies an
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

func copyFrameToBGRA(
    source: UnsafeMutableRawPointer,
    sourceFormat: UInt32,
    sourceWidth: Int,
    sourceHeight: Int,
    destination: UnsafeMutableRawPointer,
    destinationBytesPerRow: Int
) -> Bool {
    guard sourceWidth > 0, sourceHeight > 0,
          sourceWidth <= 4096, sourceHeight <= 4096,
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
    if isRGBA {
        for row in 0..<outputHeight {
            let channels = destination.advanced(by: row * destinationBytesPerRow)
                .assumingMemoryBound(to: UInt8.self)
            for offset in stride(from: 0, to: outputWidth * 4, by: 4) {
                let red = channels[offset]
                channels[offset] = channels[offset + 2]
                channels[offset + 2] = red
            }
        }
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
        if shouldLog, let message = String(data: packet.prefix(512), encoding: .utf8) {
            let singleLine = message.components(separatedBy: .controlCharacters).joined(separator: " ")
            print(String(format: "Headset diagnostics uptime=%.3f %@", now, singleLine))
        }
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

print("Starting native read loop and UDP transmission on port 9999...")

while isEncoding {
    autoreleasepool {
        reportDiagnosticsIfDue()
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

                guard width > 0, height > 0, width <= 4096, height <= 4096 else {
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
                guard CVPixelBufferLockBaseAddress(pb, []) == kCVReturnSuccess else {
                    encoderInFlightFrames.signal()
                    lastSeq = currentSeq
                    return
                }
                guard let destination = CVPixelBufferGetBaseAddress(pb) else {
                    CVPixelBufferUnlockBaseAddress(pb, [])
                    encoderInFlightFrames.signal()
                    lastSeq = currentSeq
                    return
                }

                let sourceFormat = frameHeader.format
                let didCopy = copyFrameToBGRA(
                    source: pixelDatas[Int(readyIndex)],
                    sourceFormat: sourceFormat,
                    sourceWidth: width,
                    sourceHeight: height,
                    destination: destination,
                    destinationBytesPerRow: CVPixelBufferGetBytesPerRow(pb)
                )
                CVPixelBufferUnlockBaseAddress(pb, [])
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
                if consumeKeyFrameRequest() || now - lastForcedKeyFrameAt >= 0.5 {
                    let dict: [CFString: Any] = [kVTEncodeFrameOptionKey_ForceKeyFrame: true]
                    frameProps = dict as CFDictionary
                    lastForcedKeyFrameAt = now
                }

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
