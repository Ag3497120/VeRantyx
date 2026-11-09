import Foundation
setvbuf(stdout, nil, _IONBF, 0)
import VideoToolbox
import CoreMedia
import Network
import Accelerate
import CoreAudio
import Metal
import CoreImage
import ImageIO
import CoreText
import SceneKit
import AppKit

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
    var submittedAt: TimeInterval = ProcessInfo.processInfo.systemUptime
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
// The longest time between two of the game's frames in each diagnostics period, and how many gaps passed 100 ms:
// a tester's "the picture freezes for a moment every few seconds" (issue #8) is then in hardware-encoder.log
// ("gap=" and "stalls=") whether the game stopped or the stream did.
var lastCapturedFrameAt: TimeInterval = 0
var longestCaptureGap: TimeInterval = 0
var captureStalls = 0
var submittedFrameCount = 0
var posePacketCount = 0
var lastPoseReceivedAt: TimeInterval = 0
var encoderDelayCount = 0
var encoderDelaySum: Double = 0
var encoderDelayMax: Double = 0
let encoderProbePath = ProcessInfo.processInfo.environment["VERANTYX_ENCODER_LATENCY_TEST"]
var encoderProbeDelays: [Double] = []

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

// VERANTYX_SNAPSHOT=<directory>: every 5 seconds one captured frame (what the headset is sent) as a PNG there,
// evidence of what a game draws when nobody wears the headset.
var snapshotDirectory: String? = ProcessInfo.processInfo.environment["VERANTYX_SNAPSHOT"]
var lastSnapshotAt: Double = 0
func writeSnapshotIfDue(_ pb: CVPixelBuffer, now: Double) {
    guard let dir = snapshotDirectory, now - lastSnapshotAt >= 5 else { return }
    lastSnapshotAt = now
    let image = CIImage(cvPixelBuffer: pb)
    guard let cg = CIContext().createCGImage(image, from: image.extent) else { return }
    let stamp = Int(Date().timeIntervalSince1970)
    let url = URL(fileURLWithPath: dir).appendingPathComponent("frame-\(stamp).png")
    guard let dest = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else { return }
    CGImageDestinationAddImage(dest, cg, nil)
    CGImageDestinationFinalize(dest)
}
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
// (up to 300: the app's Sharper preset for ALVR asks for 150, above the old ceiling of 80)
let videoBitrate = Int32(environmentDouble("VERANTYX_BITRATE_MBPS", default: 20, range: 2...300) * 1_000_000)
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
    let ipd: (@convention(c) () -> Float)?                              // (a library from before it had one: nil)
    let haptics: (@convention(c) (UInt32, Float, Float, Float) -> Void)?
    let linkTrouble: (@convention(c) () -> UInt32)?                    // (ALVRBridge's loss events for adaptiveBitrate)
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
    let bridge = ALVRBridge(start: start, input: input, connected: connected, takeIDR: takeIDR, setConfig: setConfig, send: send, restartNeeded: restartNeeded,
                            ipd: fn("vx_alvr_ipd", (@convention(c) () -> Float).self),
                            haptics: fn("vx_alvr_haptics", (@convention(c) (UInt32, Float, Float, Float) -> Void).self),
                            linkTrouble: fn("vx_alvr_link_trouble", (@convention(c) () -> UInt32).self))
    guard bridge.start(nil) else { print("ALVR: the streaming core did not start"); return nil }
    print("ALVR: streaming core started (\(path)); open an ALVR client on this network")
    return bridge
}
// Game audio for ALVR: ALVR records an output device it finds by name (on Windows, the speakers by loopback). macOS has
// no loopback, so this makes one: an aggregate device of the Mac's output (so that it counts as an output device) and
// a tap of the Mac's sound (its input), which ALVR, in this process, records. The tap mutes the speakers while it is
// read (VERANTYX_AUDIO_KEEP_MAC_SPEAKERS=1: not). VERANTYX_ALVR_AUDIO=0: no game audio over ALVR.
// A forced ALVR restart can leave Core Audio's old aggregate alive briefly.
// Both UID and display name must be unique: ALVR selects by name substring.
let alvrAudioInstance = "\(getpid())-\(UUID().uuidString)"
let alvrAudioDeviceName = "VeRantyx ALVR game audio \(getpid())-\(alvrAudioInstance.suffix(8))"
func makeALVRAudioDevice() -> Bool {
    guard ProcessInfo.processInfo.environment["VERANTYX_ALVR_AUDIO"] != "0" else { return false }
    guard #available(macOS 14.2, *) else { print("ALVR: game audio needs macOS 14.2 or later"); return false }
    var output = AudioObjectID(kAudioObjectUnknown)
    var size = UInt32(MemoryLayout<AudioObjectID>.size)
    var address = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
                                             mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &output) == noErr else {
        print("ALVR: no default output device for game audio"); return false
    }
    var uid: Unmanaged<CFString>?
    size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
    address.mSelector = kAudioDevicePropertyDeviceUID
    guard AudioObjectGetPropertyData(output, &address, 0, nil, &size, &uid) == noErr, let outputUID = uid?.takeRetainedValue() as String? else {
        print("ALVR: the output device has no UID"); return false
    }
    let description = CATapDescription(stereoGlobalTapButExcludeProcesses: [])
    description.name = "VeRantyx ALVR game audio tap"
    description.muteBehavior = ProcessInfo.processInfo.environment["VERANTYX_AUDIO_KEEP_MAC_SPEAKERS"] == "1" ? .unmuted : .mutedWhenTapped
    description.isPrivate = true
    var tap = AudioObjectID(kAudioObjectUnknown)
    var status = AudioHardwareCreateProcessTap(description, &tap)
    guard status == noErr else {
        print("ALVR: could not create the system audio tap (\(status)). Allow this terminal under System Settings > Privacy & Security > Screen & System Audio Recording, then relaunch.")
        return false
    }
    let aggregate: [String: Any] = [
        kAudioAggregateDeviceNameKey as String: alvrAudioDeviceName,
        kAudioAggregateDeviceUIDKey as String: "com.verantyx.alvr.game-audio.\(alvrAudioInstance)",
        kAudioAggregateDeviceIsPrivateKey as String: true,
        kAudioAggregateDeviceMainSubDeviceKey as String: outputUID,
        kAudioAggregateDeviceSubDeviceListKey as String: [[kAudioSubDeviceUIDKey as String: outputUID]],
        kAudioAggregateDeviceTapAutoStartKey as String: true,
        kAudioAggregateDeviceTapListKey as String: [[
            kAudioSubTapUIDKey as String: description.uuid.uuidString,
            kAudioSubTapDriftCompensationKey as String: true,
        ]],
    ]
    var device = AudioObjectID(kAudioObjectUnknown)
    status = AudioHardwareCreateAggregateDevice(aggregate as CFDictionary, &device)
    guard status == noErr else {
        AudioHardwareDestroyProcessTap(tap)
        print("ALVR: could not create the game audio device (\(status)); game audio is unavailable")
        return false
    }
    print("ALVR: game audio from \"\(alvrAudioDeviceName)\" (the Mac's sound; its speakers \(description.muteBehavior == .unmuted ? "stay on" : "muted while a headset listens"))")
    return true
}
if alvrMode {
    if makeALVRAudioDevice() { setenv("VERANTYX_ALVR_AUDIO_DEVICE", alvrAudioDeviceName, 1) }
    else { unsetenv("VERANTYX_ALVR_AUDIO_DEVICE") }
}
let alvr: ALVRBridge? = alvrMode ? loadALVRBridge() : nil
if alvrMode && alvr == nil {
    print("ALVR was requested but could not start; stopping the encoder")
    exit(1)
}
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
        let delay = (ProcessInfo.processInfo.systemUptime - refConPtr.pointee.submittedAt) * 1000
        encoderDiagnosticLock.lock()
        encoderDelayCount += 1; encoderDelaySum += delay; encoderDelayMax = max(encoderDelayMax, delay)
        if encoderProbePath != nil { encoderProbeDelays.append(delay) }
        encoderDiagnosticLock.unlock()
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[CFString: Any]]
        let isKeyFrame = (attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool) != true
        
        processSampleBuffer(sampleBuffer: sampleBuffer, isKeyFrame: isKeyFrame, frameSequence: frameSequence, visionTimestamp: visionTimestamp, transform: transform)

    },
                                            refcon: nil,
                                            compressionSessionOut: &compressionSession)
}

// Lowers the HEVC bitrate for packet loss or ALVR network/decoder pressure,
// and raises it back slowly once the stream has been healthy for a while. VERANTYX_BITRATE_MBPS is
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
        print(String(format: "HEVC bitrate %@ to %.1f Mbps (%d stream pressure events in the last %.0f s)",
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
    if let value = ProcessInfo.processInfo.environment["VERANTYX_ENCODER_MAX_DELAY"], let delay = Int(value), (0...3).contains(delay) {
        set(kVTCompressionPropertyKey_MaxFrameDelayCount, NSNumber(value: delay), "frame delay bound")
    }
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

// An ALVR headset's eye frustum, saved where the SteamVR driver reads it when SteamVR starts
// (C:\\vr_verantyx_tangents.txt beside the shared map, the format the driver writes): SteamVR asks for the frustum once,
// at its start, so a headset that connects before SteamVR does (the launch script waits for a headset it has not seen)
// gets its own shape from the first frame instead of a symmetric fallback that shows the eyes apart (issue #4).
var savedFrustum: [Float] = []
func saveHeadsetFrustum(_ tangents: UnsafeRawPointer) {
    let values = (0..<8).map { tangents.load(fromByteOffset: 4 * $0, as: Float.self) }
    guard values != savedFrustum, values.allSatisfy({ $0.isFinite }), values[0] < values[1], values[2] < values[3],
          values[4] < values[5], values[6] < values[7] else { return }
    savedFrustum = values
    let path = URL(fileURLWithPath: filePath).deletingLastPathComponent().appendingPathComponent("vr_verantyx_tangents.txt")
    let text = values.map { String(format: "%.6f", $0) }.joined(separator: " ") + "\n"
    if (try? String(contentsOf: path, encoding: .utf8)) == text { return }
    try? text.write(to: path, atomically: true, encoding: .utf8)
    print("ALVR headset eye frustum saved for SteamVR's next start: \(text.trimmingCharacters(in: .whitespacesAndNewlines))")
}

// SteamVR keeps the frustum it read at its start. A headset whose field of view changed since (ALVR's FoV setting,
// issue #10: double vision at 0.9, gone after a restart at 1.0) is told so in the log, which the app shows: the
// eyes do not line up until the game is started again.
var frustumMismatchChecks = 0
var frustumMismatchReported = false
func checkHeadsetFrustumAgainstSteamVR() {
    guard savedFrustum.count == 8, let used = readSourceProjection() else { frustumMismatchChecks = 0; return }
    let differs = zip(savedFrustum, used).contains { abs($0 - $1) > 0.03 }
    frustumMismatchChecks = differs ? frustumMismatchChecks + 1 : 0
    if differs && frustumMismatchChecks == 4 && !frustumMismatchReported {
        frustumMismatchReported = true
        print("ALVR headset field of view differs from the one SteamVR started with (headset \(savedFrustum.map { String(format: "%.3f", $0) }.joined(separator: " ")), SteamVR \(used.map { String(format: "%.3f", $0) }.joined(separator: " "))): stop and start the game again so both eyes line up")
    } else if !differs && frustumMismatchReported {
        frustumMismatchReported = false
        print("ALVR headset field of view matches SteamVR's again")
    }
}

// MARK: - A game's prompt over the picture

struct GameLoadingState: Decodable {
    let version: Int
    let phase: String
    let message: String?
}

struct ShaderCompilationStatus: Decodable {
    struct Counts: Decodable {
        let requested: UInt64
        let completed: UInt64
        let failed: UInt64
        let pending: UInt64
        var valid: Bool { completed <= requested && failed <= completed && pending == requested - completed }
    }
    let version: Int
    let pid: Int32
    let updated_at: Double
    let last_activity_at: Double
    let libraries: Counts
    let pipelines: Counts

    var pending: UInt64 { libraries.pending + pipelines.pending }
    func valid(now: TimeInterval) -> Bool {
        version == 1 && pid > 0 && updated_at.isFinite && last_activity_at.isFinite &&
        (0...3).contains(now - updated_at) && last_activity_at <= updated_at &&
        libraries.valid && pipelines.valid &&
        libraries.requested <= UInt64.max / 2 && pipelines.requested <= UInt64.max / 2 &&
        (kill(pid, 0) == 0 || errno == EPERM)
    }
    // An empty queue between two batches is not a reliable time to ask the player for input.
    func busy(now: TimeInterval) -> Bool { pending > 0 || now - last_activity_at < 1 }
}

struct LoadingFrameClock {
    private var lastOutput: TimeInterval = -.infinity
    private var lastHadOverlay = false
    private(set) var awaitingSource = false
    init(awaitingSource: Bool = false) {
        self.awaitingSource = awaitingSource
    }
    func needsFrame(hasSource: Bool, hasOverlay: Bool, now: TimeInterval, interval: TimeInterval = 0.1) -> Bool {
        hasSource || ((hasOverlay || awaitingSource) && now - lastOutput >= interval) || (!hasOverlay && lastHadOverlay)
    }
    mutating func sent(now: TimeInterval, hasOverlay: Bool, hasSource: Bool) {
        // Keep sending cleared frames until the game resumes: a lost clear packet must not leave a stale prompt.
        awaitingSource = !hasSource && (awaitingSource || (lastHadOverlay && !hasOverlay))
        lastOutput = now; lastHadOverlay = hasOverlay
    }
}

struct GameFrameActivity {
    let startedAt: TimeInterval
    private var lastGameFrame: TimeInterval?
    init(startedAt: TimeInterval) { self.startedAt = startedAt }
    func stalled(now: TimeInterval) -> Bool { now - (lastGameFrame ?? startedAt) >= 1.5 }
    mutating func noteFrame(sourceMode: UInt32, now: TimeInterval) -> Bool {
        // Mode 2 is the game's Submit, not SteamVR's compositor or our loading heartbeat.
        guard sourceMode == 2 else { return false }
        let resumed = stalled(now: now)
        lastGameFrame = now
        return resumed
    }
}

struct GamePromptContent: Equatable {
    let text: String
    var animated = false
}

// Half-Life: Alyx writes some things on its loading page, a SteamVR web UI that does not run on VXRuntime (the
// web UI stand-in answers for it: scripts/vxrt_helper.swift). After a level has loaded the game waits, paused, for a
// trigger pull and says so there ("Press Trigger To Start"); the player saw a black picture instead. The stand-in
// publishes its loading/waiting/running state. Background compiles must not cover a game that is still submitting
// frames. Outside an explicit load/input wait, preparation is shown only after game submissions stall. The small
// notice sits just below the view's centre, at the same 2 m depth in both eyes.
final class GamePromptOverlay {
    static let distance: Float = 2.0
    let path: String?
    let statePath: String?
    let shaderPath: String?
    let language: String
    private var content: GamePromptContent?
    var text: String? { content?.text }
    var readyForInput: Bool { content?.animated == false }
    private var activity: GameFrameActivity
    private var lastCheck: TimeInterval = -.infinity
    private var animationPhase: Int?
    private var image: CGImage?
    private var imageFor = (0, 0)
    private var startupImage: CGImage?
    private var startupImageFor = (0, 0, -1)

    init(path: String?, statePath: String? = nil, shaderPath: String? = nil,
         language: String = Locale.preferredLanguages.first ?? "en",
         startedAt: TimeInterval = ProcessInfo.processInfo.systemUptime) {
        self.path = path.flatMap { $0.isEmpty ? nil : $0 }
        self.statePath = statePath.flatMap { $0.isEmpty ? nil : $0 }
        self.shaderPath = shaderPath.flatMap { $0.isEmpty ? nil : $0 }
        self.language = language
        self.activity = GameFrameActivity(startedAt: startedAt)
    }

    func noteFrame(sourceMode: UInt32, now: TimeInterval) -> Bool { activity.noteFrame(sourceMode: sourceMode, now: now) }

    func words(_ english: String, _ russian: String, _ japanese: String) -> String {
        language.hasPrefix("ru") ? russian : language.hasPrefix("ja") ? japanese : english
    }

    func currentContent(now: TimeInterval, wallTime: TimeInterval) -> GamePromptContent? {
        let decoder = JSONDecoder()
        let state = statePath.flatMap { FileManager.default.contents(atPath: $0) }
            .flatMap { try? decoder.decode(GameLoadingState.self, from: $0) }
            .flatMap { $0.version == 1 ? $0 : nil }
        let progress = shaderPath.flatMap { FileManager.default.contents(atPath: $0) }
            .flatMap { try? decoder.decode(ShaderCompilationStatus.self, from: $0) }
            .flatMap { $0.valid(now: wallTime) ? $0 : nil }
        let loading = state?.phase == "loading" || state?.phase == "waiting"
        if let progress, (loading && progress.busy(now: wallTime)) ||
            (progress.pending > 0 && activity.stalled(now: now)) {
            let title = words("Preparing graphics", "Подготовка графики", "グラフィックスを準備中")
            return GamePromptContent(text: title + "\n\(progress.libraries.completed)/\(progress.libraries.requested)", animated: true)
        }
        if statePath != nil {
            guard let state else { return nil }
            let loading = GamePromptContent(text: words("Loading game\nPlease wait", "Загрузка игры\nПожалуйста, подождите", "ゲームを読み込み中\nしばらくお待ちください"), animated: true)
            switch state.phase {
            case "waiting":
                // Unknown/stale compilation state must not enable an input prompt.
                guard shaderPath == nil || progress != nil else { return loading }
                return GamePromptContent(text: words("Hold the trigger to start", "Нажмите и удерживайте триггер", "トリガーを長押しして開始"))
            case "loading": return state.message.map { GamePromptContent(text: $0, animated: true) } ?? loading
            default: return nil
            }
        }
        let legacy = path.flatMap { FileManager.default.contents(atPath: $0) }
            .map { String(decoding: $0, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines) }
            .flatMap { $0.isEmpty ? nil : $0 }
        return shaderPath == nil || progress != nil ? legacy.map { GamePromptContent(text: $0) } : nil
    }

    /// Reads the prompt file, four times a second at most; true when the text changed.
    @discardableResult
    func poll(now: TimeInterval, wallTime: TimeInterval = Date().timeIntervalSince1970, force: Bool = false) -> Bool {
        guard force || now - lastCheck >= 0.25 else { return false }
        lastCheck = now
        let current = currentContent(now: now, wallTime: wallTime)
        let phase = current?.animated == true ? Int(now * 4) % 8 : nil
        let changed = current != content
        guard changed || phase != animationPhase else { return false }
        content = current
        animationPhase = phase
        image = nil
        if changed {
            print(current.map { "Game prompt drawn into the picture: \($0.text.replacingOccurrences(of: "\n", with: " / "))" }
                  ?? "Game prompt taken away")
        }
        return true
    }

    /// The prompt's centre in each eye (pixels from the frame's top left; left eye in the left half).
    static func centers(width: Int, height: Int, tangents: [Float]?, ipd: Float, badgeSize: CGSize = .zero,
                        offsetX: Double = 0, offsetY: Double = -tan(8 * .pi / 180)) -> [(x: Double, y: Double)] {
        let eyeWidth = Double(width) / 2
        let valid = tangents?.count == 8 && (0..<2).allSatisfy { eye in
            guard let t = tangents else { return false }
            return t[(eye * 4)..<(eye * 4 + 4)].allSatisfy { $0.isFinite } && t[eye * 4] < t[eye * 4 + 1] && t[eye * 4 + 2] < t[eye * 4 + 3]
        }
        let t = valid ? tangents! : [-1, 1, -1, 1, -1, 1, -1, 1]
        // Clamp a single point in the overlap of both frusta, so a safe inset does not change stereo depth.
        let insetX = Double(badgeSize.width) / (2 * eyeWidth) + 0.1
        let insetY = Double(badgeSize.height) / (2 * Double(height)) + 0.1
        var minX = -Double.infinity, maxX = Double.infinity, minY = -Double.infinity, maxY = Double.infinity
        for eye in 0..<2 {
            let (l, r, b, top) = (Double(t[eye * 4]), Double(t[eye * 4 + 1]), Double(t[eye * 4 + 2]), Double(t[eye * 4 + 3]))
            let eyeOffset = Double((eye == 0 ? 1 : -1) * (ipd / 2) / distance)
            minX = max(minX, l + (r - l) * insetX - eyeOffset)
            maxX = min(maxX, r - (r - l) * insetX - eyeOffset)
            minY = max(minY, b + (top - b) * insetY)
            maxY = min(maxY, top - (top - b) * insetY)
        }
        let x = minX <= maxX ? min(maxX, max(minX, offsetX)) : (minX + maxX) / 2
        let y = minY <= maxY ? min(maxY, max(minY, offsetY)) : (minY + maxY) / 2
        return (0..<2).map { eye in
            let (l, r, bottom, top) = (Double(t[eye * 4]), Double(t[eye * 4 + 1]), Double(t[eye * 4 + 2]), Double(t[eye * 4 + 3]))
            let eyeOffset = Double((eye == 0 ? 1 : -1) * (ipd / 2) / distance)
            let u = (x + eyeOffset - l) / (r - l), v = (top - y) / (top - bottom)
            return (eyeWidth * (Double(eye) + u), Double(height) * v)
        }
    }

    /// White text on a dark plate, sized for an eye of eyeWidth x eyeHeight pixels.
    static func render(_ text: String, eyeWidth: Int, eyeHeight: Int, activityPhase: Int? = nil) -> CGImage? {
        guard !text.isEmpty else { return nil }
        let space = CGColorSpace(name: CGColorSpace.sRGB)!
        let size = CGFloat(eyeHeight) / 60
        let lineHeight = size * 1.25, pad = size * 0.5, leading = activityPhase == nil ? 0 : size * 1.75
        let font = CTFontCreateWithName("Helvetica Neue Medium" as CFString, size, nil)
        let attributes: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTForegroundColorAttributeName as String): CGColor(colorSpace: space, components: [1, 1, 1, 1])!]
        let limit = max(size, CGFloat(eyeWidth) * 0.24 - 2 * pad)
        // Wrap long prompts so input hints keep the loading notice's font size in every language.
        let ctLines: [CTLine] = text.split(separator: "\n").flatMap { paragraph in
            let attributed = NSAttributedString(string: String(paragraph), attributes: attributes)
            let typesetter = CTTypesetterCreateWithAttributedString(attributed)
            var lines: [CTLine] = [], start = 0
            while start < attributed.length {
                let count = max(1, CTTypesetterSuggestLineBreak(typesetter, start, Double(limit)))
                lines.append(CTTypesetterCreateLine(typesetter, CFRange(location: start, length: count)))
                start += count
            }
            return lines
        }
        guard !ctLines.isEmpty else { return nil }
        let textWidth = ctLines.map { CGFloat(CTLineGetTypographicBounds($0, nil, nil, nil) - CTLineGetTrailingWhitespaceWidth($0)) }.max() ?? 0
        let w = Int(ceil(textWidth + 2 * pad + leading)), h = Int(ceil(lineHeight * CGFloat(ctLines.count) + 2 * pad - (lineHeight - size)))
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        ctx.setFillColor(CGColor(colorSpace: space, components: [0.05, 0.05, 0.08, 0.8])!)
        ctx.addPath(CGPath(roundedRect: CGRect(x: 0, y: 0, width: w, height: h), cornerWidth: size * 0.6, cornerHeight: size * 0.6, transform: nil))
        ctx.fillPath()
        if let phase = activityPhase {
            let cx = pad + leading / 2, cy = CGFloat(h) / 2, radius = size * 0.65
            ctx.setLineWidth(max(1, size * 0.14))
            ctx.setLineCap(.round)
            for segment in 0..<8 {
                let angle = CGFloat(segment) * .pi / 4
                let alpha = 1 - CGFloat((segment - phase + 8) % 8) * 0.1
                ctx.setStrokeColor(CGColor(colorSpace: space, components: [1, 1, 1, alpha])!)
                ctx.move(to: CGPoint(x: cx + cos(angle) * radius * 0.55, y: cy + sin(angle) * radius * 0.55))
                ctx.addLine(to: CGPoint(x: cx + cos(angle) * radius, y: cy + sin(angle) * radius))
                ctx.strokePath()
            }
        }
        for (i, line) in ctLines.enumerated() {
            let lineWidth = CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil) - CTLineGetTrailingWhitespaceWidth(line))
            ctx.textPosition = CGPoint(x: pad + leading + (textWidth - lineWidth) / 2, y: CGFloat(h) - pad - size * 0.8 - lineHeight * CGFloat(i))
            CTLineDraw(line, ctx)
        }
        return ctx.makeImage()
    }

    /// Draws the prompt, if there is one, into a BGRA frame (both eyes side by side). True when it drew.
    @discardableResult
    func draw(into pb: CVPixelBuffer, tangents: [Float]?, ipd: Float) -> Bool {
        guard let text else { return false }
        let width = CVPixelBufferGetWidth(pb), height = CVPixelBufferGetHeight(pb)
        if image == nil || imageFor != (width, height) {
            image = GamePromptOverlay.render(text, eyeWidth: width / 2, eyeHeight: height, activityPhase: animationPhase)
            imageFor = (width, height)
        }
        guard let image else { return false }
        return Self.drawBadge(image, into: pb, tangents: tangents, ipd: ipd)
    }

    /// Draws the compact animated startup notice during initial boot while the 3D loading scene is intentionally hidden.
    @discardableResult
    func drawStartup(into pb: CVPixelBuffer, tangents: [Float]?, ipd: Float, now: TimeInterval = ProcessInfo.processInfo.systemUptime) -> Bool {
        let width = CVPixelBufferGetWidth(pb), height = CVPixelBufferGetHeight(pb), phase = Int(now * 4) % 8
        if startupImage == nil || startupImageFor != (width, height, phase) {
            let notice = words("Loading game\nPlease wait", "Загрузка игры\nПожалуйста, подождите", "ゲームを読み込み中\nしばらくお待ちください")
            startupImage = GamePromptOverlay.render(notice, eyeWidth: width / 2, eyeHeight: height, activityPhase: phase)
            startupImageFor = (width, height, phase)
        }
        guard let startupImage else { return false }
        return Self.drawBadge(startupImage, into: pb, tangents: tangents, ipd: ipd)
    }

    private static func drawBadge(_ image: CGImage, into pb: CVPixelBuffer, tangents: [Float]?, ipd: Float) -> Bool {
        let width = CVPixelBufferGetWidth(pb), height = CVPixelBufferGetHeight(pb)
        guard CVPixelBufferLockBaseAddress(pb, []) == kCVReturnSuccess else { return false }
        defer { CVPixelBufferUnlockBaseAddress(pb, []) }
        guard let base = CVPixelBufferGetBaseAddress(pb),
              let ctx = CGContext(data: base, width: width, height: height, bitsPerComponent: 8,
                                  bytesPerRow: CVPixelBufferGetBytesPerRow(pb), space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return false }
        let eyeWidth = Double(width) / 2
        for (eye, c) in GamePromptOverlay.centers(width: width, height: height, tangents: tangents, ipd: ipd,
                                                badgeSize: CGSize(width: image.width, height: image.height)).enumerated() {
            ctx.saveGState()
            ctx.clip(to: CGRect(x: eyeWidth * Double(eye), y: 0, width: eyeWidth, height: Double(height)))   // (each in its own eye)
            // (the context's y goes up from the bottom row)
            ctx.draw(image, in: CGRect(x: c.x - Double(image.width) / 2, y: Double(height) - c.y - Double(image.height) / 2,
                                       width: Double(image.width), height: Double(image.height)))
            ctx.restoreGState()
        }
        return true
    }
}
// Draw the shared interstitial from the owner's installed assets. The room is
// supplied by SetStageOverride, independently of the mailbox's map and text.
// Loading and SceneKit work stay on one worker; cached frames retain the pose
// they were rendered with, so ALVR can reproject them while the game is busy.
func loadingHeightOffset(_ history: UnsafeRawPointer) -> Float {
    // Match the driver's standing origin, as the Windows capture proxy does.
    // Native producers and older drivers leave this metadata absent.
    guard history.loadUnaligned(fromByteOffset: 4, as: UInt32.self) == 0x5348_5056,
          history.loadUnaligned(fromByteOffset: 8, as: UInt32.self) == 0x464f_4856 else { return 0 }
    let value = history.loadUnaligned(fromByteOffset: 12, as: Float.self)
    return value.isFinite ? value : 0
}

final class NativeLoadingMap {
    struct State: Decodable, Equatable {
        let type: String
        let mapName: String?
        let chapter: String?
        let description: String?
        let loadingText: String?
        let facingAngle: Float?
        var messageText: String?
        let visible: Bool?
        let waitingForInput: Bool?
    }
    struct Stage: Decodable, Equatable {
        let model: String
        let transform: [Float]
        let settings: [Float]
        let cull: Int
        let greyscale: Int
        let wireframe: Int
        let token: UInt64
    }
    struct View {
        let image: CGImage
        let head: simd_float4x4
        let timestamp: Double
        let mapName: String
        let stageModel: String?
    }
    struct MapInfo: Decodable {
        struct Position: Decodable { let x: Float?; let y: Float?; let z: Float? }
        let pos: Position
        let cables: Int
        let hidden: Bool?
        let drawVault: Bool?
    }
    let path: String?
    let game: URL?
    let stagePath: String?
    fileprivate let queue = DispatchQueue(label: "verantyx.loading-scene", qos: .userInitiated, autoreleaseFrequency: .workItem)
    private let lock = NSLock()
    private var generation = 0
    private var image: View?
    private var pending = false
    private var lastCheck = -Double.infinity
    private(set) var state: State?
    private(set) var stage: Stage?
    var active: Bool { state != nil }
    var visible: Bool { state != nil && state?.visible != false }
    fileprivate var renderer: Scene?

    init(path: String?, game: String?, stagePath: String? = nil) {
        self.path = path; self.stagePath = stagePath
        self.game = game.map { URL(fileURLWithPath: $0).resolvingSymlinksInPath() }
    }
    static func validName(_ name: String) -> Bool {
        !name.isEmpty && name.utf8.count <= 96 && name.utf8.allSatisfy {
            (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || $0 == 95 || $0 == 45
        }
    }
    static func read<T: Decodable>(_ path: String?, as: T.Type) -> T? {
        guard let path, let size = (try? FileManager.default.attributesOfItem(atPath: path)[.size]) as? NSNumber,
              size.intValue <= 16384, let data = FileManager.default.contents(atPath: path) else { return nil }
        return try? JSONDecoder().decode(T.self, from: data)
    }
    func poll(now: Double, inputReady: Bool = true) {
        guard now - lastCheck >= 0.1 else { return }; lastCheck = now
        var next = Self.read(path, as: State.self)
        if next?.type != "begin_loading" || !Self.validName(next?.mapName ?? "") || game == nil { next = nil }
        // Windows keeps its existing shader-idle policy for the trigger hint.
        if next?.waitingForInput == true && !inputReady { next?.messageText = "" }
        var nextStage = Self.read(stagePath, as: Stage.self)
        if let value = nextStage, value.transform.count != 12 || value.settings.count != 11 ||
            !(value.transform + value.settings).allSatisfy(\.isFinite) { nextStage = nil }
        guard next != state || nextStage != stage else { return }
        state = next; stage = nextStage
        lock.lock()
        generation += 1
        // Mailbox text and stage requests can change while assets are uploading.
        // Keep the last complete stereo pair until its replacement is ready.
        if !visible { image = nil }
        lock.unlock()
        // A game may wait for StageOverrideReady before beginning its load.
        if let game {
            let stage = nextStage, epoch = generation
            queue.async { [self] in
                if renderer == nil { renderer = Scene(game: game) }
                if renderer?.setStage(stage) == true, let stage, let stagePath {
                    lock.lock(); defer { lock.unlock() }
                    if generation == epoch {
                        try? Data(String(stage.token).utf8).write(to: URL(fileURLWithPath: stagePath + ".ready"), options: .atomic)
                    }
                }
            }
        }
    }
    @discardableResult func draw(into buffer: CVPixelBuffer, tangents: [Float]?, ipd: Double,
                                 head: simd_float4x4 = simd_float4x4(diagonal: SIMD4(1,1,1,1)), timestamp: Double = 0,
                                 now: Double = ProcessInfo.processInfo.systemUptime) -> View? {
        guard visible, let state, let game else { return nil }
        let width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer)
        lock.lock()
        let current = image
        if !pending {
            pending = true
            let epoch = generation
            queue.async { [self] in
                if renderer == nil { renderer = Scene(game: game) }
                let rendered = renderer?.render(state, width: width, height: height, tangents: tangents,
                                                ipd: Float(ipd), head: head, time: now)
                lock.lock(); defer { lock.unlock() }
                if epoch == generation, let rendered {
                    image = View(image: rendered, head: head, timestamp: timestamp, mapName: state.mapName ?? "",
                                 stageModel: renderer?.stageNode == nil ? nil : renderer?.stage?.model)
                }
                pending = false
            }
        }
        lock.unlock()
        guard let current, current.image.width == width, current.image.height == height,
              CVPixelBufferLockBaseAddress(buffer, []) == kCVReturnSuccess else { return nil }
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: width, height: height, bitsPerComponent: 8,
            bytesPerRow: CVPixelBufferGetBytesPerRow(buffer), space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        context.draw(current.image, in: CGRect(x: 0, y: 0, width: width, height: height))
        return current
    }

    final class Scene {
        let game: URL
        let device: MTLDevice
        let renderer: SCNRenderer
        let scene = SCNScene()
        let camera = SCNNode()
        var geometry: [String: SCNGeometry] = [:]
        var textures: [String: MTLTexture] = [:]
        var stage: Stage?
        var stageNode: SCNNode?
        var mapNode: SCNNode?
        var lastState: State?
        var spinners: [SCNNode] = []
        var loadingLabels: [SCNNode] = []
        var info: [String: MapInfo] = [:]
        var order: [String] = []
        var tertiary: [String] = []
        var cables: [String] = []
        var fonts: [String] = []
        init?(game: URL) {
            guard let device = MTLCreateSystemDefaultDevice() else { return nil }
            self.game = game; self.device = device
            renderer = SCNRenderer(device: device, options: nil)
            renderer.scene = scene; renderer.pointOfView = camera
            camera.camera = SCNCamera(); camera.camera?.automaticallyAdjustsZRange = false
            scene.rootNode.addChildNode(camera)
            scene.background.contents = NSColor.black
            // Read only the authored data tables, without executing the game's JS.
            if let script = asset("webui/interstitials.js", limit: 2 * 1024 * 1024),
               let text = try? String(contentsOf: script, encoding: .utf8) {
                func table(_ field: String, object: Bool = false) -> Data? {
                    let pattern = "\\." + field + "=(" + (object ? "\\{.*?\\}),[A-Za-z_$][A-Za-z_0-9$]*\\.k_" : "\\[[^\\]]*\\]),")
                    guard let re = try? NSRegularExpression(pattern: pattern),
                          let match = re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
                          let range = Range(match.range(at: 1), in: text) else { return nil }
                    var value = String(text[range])
                    for (pattern, replacement) in [("([,{])([A-Za-z_][A-Za-z_0-9]*):", "$1\"$2\":"),
                        ("!0", "true"), ("!1", "false")] {
                        value = value.replacingOccurrences(of: pattern, with: replacement, options: .regularExpression)
                    }
                    value = value.replacingOccurrences(of: ":.", with: ":0.").replacingOccurrences(of: ":-.", with: ":-0.")
                    return value.data(using: .utf8)
                }
                if let data = table("k_MapInfo", object: true) { info = (try? JSONDecoder().decode([String: MapInfo].self, from: data)) ?? [:] }
                for field in ["k_sGameMaps", "k_sTertiaryMaps", "k_sVaultCables"] {
                    let names = table(field).flatMap { try? JSONDecoder().decode([String].self, from: $0) } ?? []
                    if field == "k_sGameMaps" { order = names }
                    else if field == "k_sTertiaryMaps" { tertiary = names }
                    else { cables = names }
                }
            }
            if let font = asset("webui/fonts/raju-light.otf", limit: 2 * 1024 * 1024) {
                CTFontManagerRegisterFontsForURL(font as CFURL, .process, nil)
                if let descriptors = CTFontManagerCreateFontDescriptorsFromURL(font as CFURL) as? [CTFontDescriptor], let first = descriptors.first,
                   let name = CTFontDescriptorCopyAttribute(first, kCTFontNameAttribute) as? String { fonts = [name] }
            }
        }
        func asset(_ relative: String, limit: Int = 64 * 1024 * 1024) -> URL? {
            guard !relative.contains("\\"), !relative.split(separator: "/").contains("..") else { return nil }
            let url = game.appendingPathComponent(relative).resolvingSymlinksInPath()
            guard url.path.hasPrefix(game.path + "/"),
                  let n = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size]) as? NSNumber,
                  (1...limit).contains(n.intValue) else { return nil }
            return url
        }
        func mesh(_ relative: String) -> SCNGeometry? {
            if let known = geometry[relative] { return known }
            guard let url = asset(relative, limit: 8 * 1024 * 1024), let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
            var v: [SCNVector3] = [], n: [SCNVector3] = [], uv: [CGPoint] = []
            var positions: [SCNVector3] = [], normals: [SCNVector3] = [], coords: [CGPoint] = []
            func index(_ value: Substring, _ count: Int) -> Int? {
                guard let i = Int(value), i != 0 else { return nil }
                let at = i > 0 ? i - 1 : count + i
                return (0..<count).contains(at) ? at : nil
            }
            for line in text.split(whereSeparator: \.isNewline) {
                let p = line.split(whereSeparator: \.isWhitespace)
                if (p.first == "v" || p.first == "vn"), p.count >= 4,
                   let x = Float(p[1]), let y = Float(p[2]), let z = Float(p[3]),
                   [x,y,z].allSatisfy({ $0.isFinite && abs($0) < 10000 }) {
                    if p[0] == "v" { v.append(SCNVector3(x,y,z)) } else { n.append(SCNVector3(x,y,z)) }
                } else if p.first == "vt", p.count >= 3, let x = Double(p[1]), let y = Double(p[2]), x.isFinite, y.isFinite {
                    uv.append(CGPoint(x: x, y: y))
                } else if p.first == "f", (4...17).contains(p.count), positions.count < 600000 {
                    let face = p.dropFirst().map { $0.split(separator: "/", omittingEmptySubsequences: false) }
                    for i in 1..<(face.count - 1) {
                        let triangle = [face[0], face[i], face[i+1]]
                        let vi = triangle.compactMap { $0.first.flatMap { index($0, v.count) } }
                        guard vi.count == 3 else { continue }
                        let a = SIMD3<Float>(Float(v[vi[0]].x),Float(v[vi[0]].y),Float(v[vi[0]].z))
                        let b = SIMD3<Float>(Float(v[vi[1]].x),Float(v[vi[1]].y),Float(v[vi[1]].z))
                        let c = SIMD3<Float>(Float(v[vi[2]].x),Float(v[vi[2]].y),Float(v[vi[2]].z))
                        let cross = simd_cross(b-a,c-a), normal = simd_length(cross) > 1e-8 ? simd_normalize(cross) : SIMD3<Float>(0,1,0)
                        for j in 0..<3 {
                            positions.append(v[vi[j]])
                            let f = triangle[j]
                            normals.append(f.count > 2 ? index(f[2], n.count).map { n[$0] } ?? SCNVector3(normal.x,normal.y,normal.z) : SCNVector3(normal.x,normal.y,normal.z))
                            coords.append(f.count > 1 ? index(f[1], uv.count).map { uv[$0] } ?? .zero : .zero)
                        }
                    }
                }
            }
            guard !positions.isEmpty else { return nil }
            let indices = Array(0..<UInt32(positions.count))
            let result = SCNGeometry(sources: [SCNGeometrySource(vertices: positions), SCNGeometrySource(normals: normals), SCNGeometrySource(textureCoordinates: coords)],
                elements: [SCNGeometryElement(indices: indices, primitiveType: .triangles)])
            geometry[relative] = result; return result
        }
        func texture(_ relative: String) -> MTLTexture? {
            if let known = textures[relative] { return known }
            // Shipped dioramas contain 8192-square DDS files (~85 MiB). Map the
            // container and upload a bounded mip, rather than rejecting the room.
            guard let url = asset(relative, limit: 512 * 1024 * 1024),
                  let data = try? Data(contentsOf: url, options: .mappedIfSafe), data.count >= 128 else { return nil }
            func word(_ at: Int) -> UInt32 { data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: at, as: UInt32.self) } }
            guard word(0) == 0x20534444 else { return nil }
            var width = Int(word(16)), height = Int(word(12)), offset = 128, block = 16
            let format: MTLPixelFormat
            switch word(84) {
            case 0x55344342: format = .bc4_rUnorm; block = 8 // BC4U
            case 0x30315844:
                guard data.count >= 148 else { return nil }; offset = 148
                switch word(128) {
                case 71: format = .bc1_rgba; block = 8
                case 72: format = .bc1_rgba_srgb; block = 8
                case 77: format = .bc3_rgba
                case 78: format = .bc3_rgba_srgb
                case 80: format = .bc4_rUnorm; block = 8
                case 98: format = .bc7_rgbaUnorm
                case 99: format = .bc7_rgbaUnorm_srgb
                default: return nil
                }
            default: return nil
            }
            guard (1...16384).contains(width), (1...16384).contains(height) else { return nil }
            var levels = max(1, Int(word(28)))
            guard levels <= 1 + Int(log2(Double(max(width,height)))) else { return nil }
            while max(width,height) > 4096 && levels > 1 {
                offset += ((width+3)/4) * ((height+3)/4) * block
                width = max(1,width/2); height = max(1,height/2); levels -= 1
            }
            var mips: [(offset: Int, width: Int, height: Int, row: Int)] = []
            var w = width, h = height
            for _ in 0..<levels {
                let row = ((w+3)/4) * block, size = row * ((h+3)/4)
                guard offset + size <= data.count else { return nil }
                mips.append((offset,w,h,row)); offset += size
                w = max(1,w/2); h = max(1,h/2)
            }
            let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: width, height: height, mipmapped: levels > 1)
            desc.mipmapLevelCount = levels
            desc.usage = .shaderRead; desc.storageMode = .shared
            guard let result = device.makeTexture(descriptor: desc) else { return nil }
            data.withUnsafeBytes { raw in
                for (level, mip) in mips.enumerated() {
                    result.replace(region: MTLRegionMake2D(0,0,mip.width,mip.height), mipmapLevel: level,
                        withBytes: raw.baseAddress!.advanced(by: mip.offset), bytesPerRow: mip.row)
                }
            }
            textures[relative] = result; return result
        }
        func mapTexture(_ name: String, kind: String) -> MTLTexture? {
            let folder = "models/ui/map_hologram_steamvr/"
            // Several cable models share one material. OBJ/MTL references name
            // PNGs, while the shipped images are DDS files with the same stem.
            if let url = asset(folder + name + ".mtl", limit: 16384), let text = try? String(contentsOf: url, encoding: .utf8) {
                for line in text.split(whereSeparator: \.isNewline) {
                    let p = line.split(whereSeparator: \.isWhitespace)
                    guard p.first == "map_Kd", p.count == 2 else { continue }
                    let file = String(p[1]), stem = (file as NSString).deletingPathExtension
                    guard NativeLoadingMap.validName(stem) else { continue }
                    let target = kind == "ao" ? stem.replacingOccurrences(of: "_wire", with: "_ao") : stem
                    if let result = texture(folder + target + ".dds") { return result }
                }
            }
            return texture(folder + name + "_" + kind + ".dds")
        }
        func material(_ colour: UInt32, opacity: CGFloat = 1, texture: MTLTexture? = nil, additive: Bool = false) -> SCNMaterial {
            let m = SCNMaterial(); m.lightingModel = .constant; m.isDoubleSided = true
            let tint = NSColor(srgbRed: CGFloat((colour >> 16) & 255)/255, green: CGFloat((colour >> 8) & 255)/255,
                               blue: CGFloat(colour & 255)/255, alpha: 1)
            m.diffuse.contents = texture ?? tint as Any
            if texture != nil {
                // DDS rows run downward; OBJ V runs upward. Without this the
                // texture atlas lands on unrelated triangles of the room/map.
                m.diffuse.contentsTransform = SCNMatrix4Scale(SCNMatrix4MakeTranslation(0, 1, 0), 1, -1, 1)
                m.diffuse.mipFilter = .linear
            }
            m.multiply.contents = texture == nil ? NSColor.white : tint
            m.transparency = opacity
            m.blendMode = additive ? .add : .alpha
            m.writesToDepthBuffer = !additive
            return m
        }
        func node(_ geometry: SCNGeometry, material: SCNMaterial, parent: SCNNode) -> SCNNode {
            let copy = geometry.copy() as! SCNGeometry; copy.materials = [material]
            let node = SCNNode(geometry: copy); parent.addChildNode(node); return node
        }
        @discardableResult func setStage(_ next: Stage?) -> Bool {
            if next == stage { return stageNode != nil }
            stageNode?.removeFromParentNode(); stageNode = nil
            geometry = geometry.filter { !$0.key.hasPrefix("models/ui/dioramas/") }
            textures = textures.filter { !$0.key.hasPrefix("models/ui/dioramas/") }
            stage = next
            guard let next else { return false }
            // Native uses an absolute path; the Windows bridge supplies an
            // install-relative model. Both stay confined to this game's assets.
            let model = next.model.replacingOccurrences(of: "\\", with: "/")
            let url = (model.hasPrefix("models/ui/dioramas/") ? game.appendingPathComponent(model)
                : URL(fileURLWithPath: model)).resolvingSymlinksInPath()
            let root = game.appendingPathComponent("models/ui/dioramas").path + "/"
            guard url.path.hasPrefix(root), url.pathExtension == "obj", Self.validStage(next),
                  let mesh = mesh("models/ui/dioramas/" + url.lastPathComponent) else { return false }
            let name = url.deletingPathExtension().lastPathComponent
            let tex = texture("models/ui/dioramas/" + name + ".dds")
            guard tex != nil else { return false }
            let m = material(0xffffff, texture: tex)
            let s = next.settings
            let grey = next.greyscale != 0 || tex?.pixelFormat == .bc4_rUnorm
            m.shaderModifiers = [.surface: """
                float3 colour = _surface.diffuse.rgb;
                \(grey ? (tex?.pixelFormat == .bc4_rUnorm ? "colour = float3(colour.r);" : "colour = float3(dot(colour, float3(0.2126, 0.7152, 0.0722)));") : "")
                float3 primary = float3(\(s[0]), \(s[1]), \(s[2]));
                float3 secondary = float3(\(s[4]), \(s[5]), \(s[6]));
                float fade = 0.0;
                \(s[9] > s[8] ? "float3 world = (scn_frame.inverseViewTransform * float4(_surface.position, 1.0)).xyz; fade = smoothstep(\(s[8]), \(s[9]), length(world));" : "")
                \(s[10] > 0 ? "fade = pow(1.0 - abs(dot(normalize(_surface.normal), normalize(_surface.view))), \(s[10]));" : "")
                _surface.diffuse.rgb = mix(colour * primary, secondary, fade);
                _surface.diffuse.a *= mix(\(s[3]), \(s[7]), fade);
                """]
            m.isDoubleSided = next.cull == 0; m.fillMode = next.wireframe == 0 ? .fill : .lines
            let room = node(mesh, material: m, parent: scene.rootNode)
            let t = next.transform
            room.simdTransform = simd_float4x4(SIMD4(t[0],t[4],t[8],0), SIMD4(t[1],t[5],t[9],0),
                SIMD4(t[2],t[6],t[10],0), SIMD4(t[3],t[7],t[11],1))
            stageNode = room
            guard renderer.prepare(room, shouldAbortBlock: nil) else { room.removeFromParentNode(); stageNode = nil; return false }
            return true
        }
        static func validStage(_ stage: Stage) -> Bool { stage.transform.count == 12 && stage.settings.count == 11 }
        func text(_ value: String, height: CGFloat, parent: SCNNode, position: SCNVector3, left: Bool = false) -> SCNNode {
            let font = CTFontCreateWithName((fonts.first ?? "HelveticaNeue-Light") as CFString, 64, nil)
            let line = CTLineCreateWithAttributedString(NSAttributedString(string: String(value.prefix(256)), attributes: [
                kCTFontAttributeName as NSAttributedString.Key: font,
                kCTForegroundColorAttributeName as NSAttributedString.Key: CGColor(gray: 1, alpha: 1)]))
            let bounds = CTLineGetBoundsWithOptions(line, [.useOpticalBounds])
            let w = max(1, Int(ceil(bounds.width + 8))), h = 88
            let context = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w*4,
                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            context.textPosition = CGPoint(x: 4 - bounds.minX, y: 18); CTLineDraw(line, context)
            let plane = SCNPlane(width: height * CGFloat(w)/CGFloat(h), height: height)
            let m = material(0xffffff); m.diffuse.contents = context.makeImage(); m.writesToDepthBuffer = false
            let result = node(plane, material: m, parent: parent); result.position = position; result.renderingOrder = 10
            if left { result.position.x += plane.width/2 }
            return result
        }
        func buildMap(_ state: State) {
            mapNode?.removeFromParentNode(); spinners = []; loadingLabels = []
            let root = SCNNode(); scene.rootNode.addChildNode(root); mapNode = root
            root.eulerAngles.y = CGFloat(state.facingAngle ?? 0) * .pi / 180
            let name = state.mapName ?? "", current = info[name]
            let visible = current != nil && current?.hidden != true
            var prompt = state.messageText?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            if ["Press Right Hand To Start", "Press Left Hand To Start", "Press Trigger To Start"].contains(prompt) { prompt = "Pull Trigger To Start" }
            if !prompt.isEmpty {
                _ = text(prompt, height: 0.11, parent: root, position: SCNVector3(0,1.5,-1.5))
            } else if !visible {
                _ = text(state.loadingText ?? "LOADING", height: 0.12, parent: root, position: SCNVector3(0,1.8,-2))
            }
            guard visible else { return }
            let map = SCNNode(); root.addChildNode(map)
            map.position = SCNVector3(0,1.1,-1.5); map.eulerAngles.x = .pi / 6; map.scale = SCNVector3(0.8,0.8,0.8)
            let models = SCNNode(); map.addChildNode(models); models.scale = SCNVector3(0.3,0.3,0.3)
            let at = order.firstIndex(of: name) ?? max(0,order.count-1)
            let selectedName = order.indices.contains(at) ? order[at] : name
            let played = Array(order.prefix(at + 1))
            let cableNames = Array(cables.dropFirst(max(0,current?.cables ?? cables.count)))
            var names = tertiary + played + cableNames + ["combine_wall", "cables_citadel"]
            if current?.drawVault == true { names.append("a5_vault") }
            if at < order.count-1 { names.append("a4_c17_parking_garage_precrash") }
            for name in names where NativeLoadingMap.validName(name) {
                guard let g = mesh("models/ui/map_hologram_steamvr/" + name + ".obj") else { continue }
                let selected = name == selectedName, visited = played.contains(name)
                let cable = cableNames.contains(name) || name == "cables_citadel"
                let vault = name == "a5_vault", precrash = name == "a4_c17_parking_garage_precrash"
                let ao = mapTexture(name, kind: "ao")
                let fill = material(selected ? 0xffbe55 : visited || cableNames.contains(name) ? 0x805f2a : 0x553c18,
                    opacity: selected || visited || vault ? 1 : 0.75, texture: ao, additive: true)
                let rimStrength = cable ? 0.1 : selected || visited || vault ? 0.5 : 0.75
                fill.shaderModifiers = [.surface: """
                    \(ao != nil ? "_surface.diffuse.rgb = float3(_surface.diffuse.r);" : "")
                    float rim = pow(clamp(1.0 - abs(dot(normalize(_surface.normal), normalize(_surface.view))), 0.0, 1.0), \(rimStrength));
                    _surface.diffuse.a = clamp(_surface.diffuse.a * (1.0 \(cable ? "+" : "-") rim), 0.0, 1.0);
                    """]
                let mask = material(0); mask.colorBufferWriteMask = []
                // The colour and wire shaders round coincident depths
                // differently. Put only the occlusion shell slightly behind
                // them, preserving the visible mesh and its stereo position.
                mask.shaderModifiers = [.geometry: """
                    float4 viewPosition = scn_node.modelViewTransform * _geometry.position;
                    viewPosition.xyz *= 1.00002;
                    _geometry.position = scn_node.inverseModelViewTransform * viewPosition;
                    """]
                let depth = node(g, material: mask, parent: models); depth.renderingOrder = -2
                let surface = node(g, material: fill, parent: models); surface.renderingOrder = 2
                let wire = material(selected ? 0xffd155 : visited ? 0xcc953d : vault ? 0x805924 : 0x66471d,
                    opacity: selected ? 1 : visited || tertiary.contains(name) || vault || precrash ? 0.75 : name == "cables_citadel" ? 0.15 : 0.25,
                    texture: mapTexture(name, kind: "wire"), additive: true)
                if wire.diffuse.contents is MTLTexture {
                    wire.shaderModifiers = [.surface: "_surface.transparent.a = _surface.diffuse.r; _surface.diffuse.rgb = float3(1.0);"]
                } else { wire.fillMode = .lines }
                let lines = node(g, material: wire, parent: models); lines.renderingOrder = 3
            }
            if let streets = mesh("models/ui/map_hologram_steamvr/map_streets.obj") {
                let m = material(0x664c22, opacity: 0.1, texture: texture("models/ui/map_hologram_steamvr/map_streets_col.dds"), additive: true)
                _ = node(streets, material: m, parent: models)
            }
            // The faint points outside the city are authored quads, not a
            // screen-space pattern: they must keep the map's stereo depth.
            if let grid = mesh("models/ui/map_hologram_steamvr/voxel_cubes.obj") {
                let dots = material(0xffe2a8, opacity: 0.35, texture: mapTexture("voxel_cubes", kind: "ao"), additive: true)
                // The authored lower mips fade these subpixel marks away.
                // Keep their base image; the city retains filtered mip chains.
                dots.diffuse.mipFilter = .none
                dots.shaderModifiers = [.surface: """
                    _surface.diffuse.rgb = float3(_surface.diffuse.r);
                    """]
                let points = node(grid, material: dots, parent: models); points.renderingOrder = 4
                points.name = "loading-map-points"
                let wire = material(0xffbe55, opacity: 0.1, texture: mapTexture("voxel_cubes", kind: "wire"), additive: true)
                wire.diffuse.mipFilter = .none
                if wire.diffuse.contents is MTLTexture {
                    wire.shaderModifiers = [.surface: "_surface.transparent.a = _surface.diffuse.r; _surface.diffuse.rgb = float3(1.0);"]
                } else { wire.fillMode = .lines }
                let lines = node(grid, material: wire, parent: models); lines.renderingOrder = 4
                lines.name = "loading-map-point-wire"
            }
            // The amber panel lies beneath the city, with its far edge elevated
            // in the viewer's field of view just as in the authored interstitial.
            let panel = SCNNode(); map.addChildNode(panel); panel.eulerAngles.x = -.pi/2
            panel.position = SCNVector3(0,0.01,0.25)
            func backdrop(_ colour: UInt32, height: CGFloat, opacity: CGFloat) -> SCNNode {
                let m = material(colour, opacity: opacity, additive: true)
                m.readsFromDepthBuffer = false
                let context = CGContext(data: nil, width: 256, height: 2, bitsPerComponent: 8, bytesPerRow: 1024,
                    space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
                let gradient = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                    colors: [CGColor(gray: 1, alpha: 0), CGColor(gray: 1, alpha: 1), CGColor(gray: 1, alpha: 1), CGColor(gray: 1, alpha: 0)] as CFArray,
                    locations: [0,0.4,0.6,1])!
                context.drawLinearGradient(gradient, start: .zero, end: CGPoint(x: 256,y: 0), options: [])
                m.diffuse.contents = context.makeImage()
                m.multiply.contents = material(colour).diffuse.contents
                let result = node(SCNPlane(width: 8, height: height), material: m, parent: panel)
                result.renderingOrder = 1
                return result
            }
            _ = backdrop(0x553c18, height: 1.8, opacity: 0.2)
            let edge = backdrop(0xffbe55, height: 0.1, opacity: 0.2)
            edge.position.y = 0.9
            if prompt.isEmpty, let p = current?.pos {
                let anchor = SCNNode(); map.addChildNode(anchor); anchor.position = SCNVector3(p.x ?? 0,p.y ?? 0,p.z ?? 0)
                let stem = node(SCNCylinder(radius: 0.0015, height: 0.1), material: material(0xffffff), parent: anchor)
                stem.position.y = 0.05
                let elbow = node(SCNCylinder(radius: 0.0015, height: 0.01), material: material(0xffffff), parent: anchor)
                elbow.position = SCNVector3(0.005,0.1,0); elbow.eulerAngles.z = -.pi/2
                let callout = SCNNode(); anchor.addChildNode(callout)
                callout.position = SCNVector3(0.02,0.1,0); callout.eulerAngles.x = -.pi/3
                for count in 0...3 {
                    let label = text((state.loadingText ?? "LOADING") + String(repeating: ".", count: count),
                        height: 0.08, parent: callout, position: SCNVector3(0.035,0,0), left: true)
                    loadingLabels.append(label)
                }
                for index in 0..<6 {
                    let ink = material(0xffffff); ink.writesToDepthBuffer = false
                    if let url = asset(String(format: "webui/images/arc_%02d.svg", index+1), limit: 16384),
                       let image = NSImage(contentsOf: url) {
                        ink.diffuse.contents = image
                    } else {
                        let context = CGContext(data: nil, width: 96, height: 96, bitsPerComponent: 8, bytesPerRow: 384,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
                        context.setStrokeColor(CGColor(gray: 1, alpha: 1)); context.setLineWidth(4)
                        let start = Double(index % 3) * 2 * .pi / 3
                        context.addArc(center: CGPoint(x: 48,y: 48), radius: index < 3 ? 27 : 39,
                                       startAngle: start, endAngle: start + .pi/2, clockwise: false)
                        context.strokePath(); ink.diffuse.contents = context.makeImage()
                    }
                    let arc = node(SCNPlane(width: 0.075,height: 0.075), material: ink, parent: callout)
                    arc.renderingOrder = 10; spinners.append(arc)
                }
            }
        }
        func render(_ state: State, width: Int, height: Int, tangents: [Float]?, ipd: Float,
                    head: simd_float4x4, time: Double) -> CGImage? {
            guard width > 0, height > 0, width <= 8192, height <= 4096,
                  head.columns.0.x.isFinite, time.isFinite else { return nil }
            if state != lastState { buildMap(state); lastState = state }
            let phase = max(0,time.truncatingRemainder(dividingBy: 2))
            for (index, arc) in spinners.enumerated() {
                let progress = min(1,max(0,(phase - Double(index)*0.1)/0.9))
                arc.eulerAngles.z = CGFloat(-2 * .pi * (1 - pow(1-progress,3)))
            }
            for (index, label) in loadingLabels.enumerated() { label.isHidden = index != Int(phase*2) }
            guard let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                bytesPerRow: width*4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
            for eye in 0..<2 {
                let t = tangents?.count == 8 ? Array(tangents!.dropFirst(eye*4).prefix(4)) : [-1,1,-1,1]
                let l = t[0], r = t[1], b = t[2], u = t[3], near: Float = 0.05, far: Float = 100
                guard t.allSatisfy(\.isFinite), r-l > 0.01, u-b > 0.01 else { return nil }
                camera.camera?.projectionTransform = SCNMatrix4(simd_float4x4(
                    SIMD4(2/(r-l),0,0,0), SIMD4(0,2/(u-b),0,0),
                    SIMD4((r+l)/(r-l),(u+b)/(u-b),-(far+near)/(far-near),-1), SIMD4(0,0,-2*far*near/(far-near),0)))
                var offset = matrix_identity_float4x4; offset.columns.3.x = (eye == 0 ? -0.5 : 0.5)*ipd
                camera.simdTransform = head * offset
                let shot = renderer.snapshot(atTime: time, with: CGSize(width: width/2,height: height), antialiasingMode: .multisampling4X)
                guard let image = shot.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return nil }
                context.draw(image, in: CGRect(x: eye*width/2,y: 0,width: width/2,height: height))
            }
            return context.makeImage()
        }
    }
}

let loadingEnvironment = ProcessInfo.processInfo.environment
let nativeLoadingMap = NativeLoadingMap(path: loadingEnvironment["VERANTYX_LOADING_FILE"] ?? loadingEnvironment["VERANTYX_NATIVE_LOADING_FILE"],
                                       game: loadingEnvironment["VERANTYX_LOADING_GAME_DIR"] ?? loadingEnvironment["VERANTYX_NATIVE_GAME_DIR"],
                                       stagePath: loadingEnvironment["VERANTYX_LOADING_STAGE_FILE"] ?? loadingEnvironment["VERANTYX_NATIVE_STAGE_FILE"])

if let output = ProcessInfo.processInfo.environment["VERANTYX_NATIVE_MAP_SELFTEST"],
   let game = ProcessInfo.processInfo.environment["VERANTYX_NATIVE_GAME_DIR"] {
    let name = ProcessInfo.processInfo.environment["VERANTYX_NATIVE_MAP_NAME"] ?? "a2_pistol"
    precondition(NativeLoadingMap.validName(name) && !NativeLoadingMap.validName("../outside"))
    var history = Data(count: 16)
    history.withUnsafeMutableBytes { bytes in
        precondition(loadingHeightOffset(bytes.baseAddress!) == 0)
        bytes.storeBytes(of: UInt32(0x5348_5056), toByteOffset: 4, as: UInt32.self)
        bytes.storeBytes(of: UInt32(0x464f_4856), toByteOffset: 8, as: UInt32.self)
        bytes.storeBytes(of: Float(-0.74), toByteOffset: 12, as: Float.self)
        precondition(abs(loadingHeightOffset(bytes.baseAddress!) + 0.74) < 0.0001)
        bytes.storeBytes(of: Float.nan, toByteOffset: 12, as: Float.self)
        precondition(loadingHeightOffset(bytes.baseAddress!) == 0)
    }
    let json = try! JSONSerialization.data(withJSONObject: ["type": "begin_loading", "mapName": name, "chapter": "HALF-LIFE: ALYX", "loadingText": "LOADING"])
    let stateFile = output + ".state.json", stageFile = output + ".stage.json"
    let stageName = ProcessInfo.processInfo.environment["VERANTYX_NATIVE_STAGE_MODEL"] ?? "synthetic"
    let stage: [String: Any] = ["model": URL(fileURLWithPath: game).appendingPathComponent("models/ui/dioramas/" + stageName + ".obj").path,
        "transform": [1,0,0,0,0,1,0,0,0,0,1,0], "settings": [0.22,0.22,0.22,1,0,0,0,1,0,0,0],
        "cull": 0, "greyscale": 1, "wireframe": 0, "token": 1]
    try! JSONSerialization.data(withJSONObject: stage).write(to: URL(fileURLWithPath: stageFile), options: .atomic)
    try! json.write(to: URL(fileURLWithPath: stateFile), options: .atomic)
    let probe = NativeLoadingMap(path: stateFile, game: game, stagePath: stageFile)
    probe.poll(now: 0); precondition(probe.active)
    var pixel: CVPixelBuffer?
    precondition(CVPixelBufferCreate(nil, 1536, 864, kCVPixelFormatType_32BGRA, nil, &pixel) == kCVReturnSuccess)
    // Sparse synthetic DDS: the real train room exceeds the former 64 MiB cap.
    let largeRoot = URL(fileURLWithPath: output + ".large")
    try! FileManager.default.createDirectory(at: largeRoot, withIntermediateDirectories: true)
    let largeFile = largeRoot.appendingPathComponent("room.dds")
    var header = Data(count: 148)
    for (offset, value) in [(0, 0x20534444), (4, 124), (12, 8192), (16, 8192), (28, 14), (84, 0x30315844), (128, 99)] {
        var word = UInt32(value).littleEndian
        withUnsafeBytes(of: &word) { header.replaceSubrange(offset..<(offset+4), with: $0) }
    }
    try! header.write(to: largeFile)
    let largeHandle = try! FileHandle(forWritingTo: largeFile)
    try! largeHandle.truncate(atOffset: 89_478_660); try! largeHandle.close()
    let largeScene = NativeLoadingMap.Scene(game: largeRoot)!
    let roomTexture = largeScene.texture("room.dds")
    precondition(roomTexture?.width == 4096 && roomTexture?.mipmapLevelCount == 13,
                 "Large diorama must retain the bounded mip and its smaller levels")
    try! header.write(to: largeRoot.appendingPathComponent("truncated.dds"))
    precondition(largeScene.texture("truncated.dds") == nil, "Truncated mip chains must be rejected")
    try! FileManager.default.removeItem(at: largeRoot)
    var head = matrix_identity_float4x4; head.columns.3.y = 1.7
    let tilt = Float(-25) * .pi / 180
    head = head * simd_float4x4(simd_quatf(angle: tilt, axis: SIMD3(1,0,0)))
    func draw(_ transform: simd_float4x4, timestamp: Double) -> NativeLoadingMap.View {
        for _ in 0..<3000 {
            if let view = probe.draw(into: pixel!, tangents: [-1.1,1.1,-1,1,-1.1,1.1,-1,1], ipd: 0.064,
                head: transform, timestamp: timestamp, now: 10), view.timestamp == timestamp { return view }
            usleep(10_000)
        }
        fatalError("Loading renderer did not produce the requested pose")
    }
    func save(_ image: CGImage, _ path: String) {
        let destination = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil)!
        CGImageDestinationAddImage(destination, image, nil); precondition(CGImageDestinationFinalize(destination))
    }
    let loading = draw(head, timestamp: 1)
    precondition(probe.renderer?.info[name] != nil, "No authored map metadata")
    precondition(probe.renderer?.stageNode != nil, "The loading room was discarded")
    precondition((probe.renderer?.geometry.count ?? 0) >= 3, "The city geometry was not loaded")
    precondition(probe.renderer?.geometry["models/ui/map_hologram_steamvr/voxel_cubes.obj"] != nil,
                 "The point field around the city was not loaded")
    precondition((try? String(contentsOfFile: stageFile + ".ready", encoding: .utf8)) == "1")
    save(loading.image, output)
    // Windows publishes install-relative models; their room must be identical
    // to the native absolute path, including its transform and material.
    var relativeStage = stage
    relativeStage["model"] = "models/ui/dioramas/" + stageName + ".obj"
    try! JSONSerialization.data(withJSONObject: relativeStage).write(to: URL(fileURLWithPath: stageFile), options: .atomic)
    probe.poll(now: 0.2); probe.queue.sync {}
    let relativeView = draw(head, timestamp: 0.2)
    precondition(relativeView.stageModel == relativeStage["model"] as? String &&
                 relativeView.image.dataProvider!.data! as Data == loading.image.dataProvider!.data! as Data,
                 "Windows and native room paths must produce the same stereo image")
    // A fixed view missed the reported flicker. Exercise coincident depth and
    // colour passes with subpixel head motion on wholly synthetic buildings.
    let stability = NativeLoadingMap.Scene(game: largeRoot)!
    stability.info[name] = NativeLoadingMap.MapInfo(pos: .init(x: 0,y: 0,z: 0), cables: 99, hidden: nil, drawVault: nil)
    stability.order = [name]
    var vertices: [SCNVector3] = [], normals: [SCNVector3] = []
    let faces = [[0,1,2,3],[4,7,6,5],[0,4,5,1],[1,5,6,2],[2,6,7,3],[3,7,4,0]]
    for row in 0..<12 { for column in 0..<12 {
        let x = Float(column)*0.45-2.7, z = Float(row)*0.4-1.8
        let top = Float((row*7+column*11)%5)*0.06+0.06
        let corners: [SIMD3<Float>] = [SIMD3(x,0,z),SIMD3(x+0.32,0,z),SIMD3(x+0.32,0,z+0.28),SIMD3(x,0,z+0.28),
            SIMD3(x,top,z),SIMD3(x+0.32,top,z),SIMD3(x+0.32,top,z+0.28),SIMD3(x,top,z+0.28)]
        for face in faces {
            let normal = simd_normalize(simd_cross(corners[face[1]]-corners[face[0]],corners[face[2]]-corners[face[0]]))
            for index in [face[0],face[1],face[2],face[0],face[2],face[3]] {
                let p = corners[index]; vertices.append(SCNVector3(p.x,p.y,p.z))
                normals.append(SCNVector3(normal.x,normal.y,normal.z))
            }
        }
    }}
    stability.geometry["models/ui/map_hologram_steamvr/" + name + ".obj"] = SCNGeometry(
        sources: [SCNGeometrySource(vertices: vertices), SCNGeometrySource(normals: normals)],
        elements: [SCNGeometryElement(indices: Array(0..<UInt32(vertices.count)), primitiveType: .triangles)])
    let stabilityState = try! JSONDecoder().decode(NativeLoadingMap.State.self, from: json)
    let jitter = head * simd_float4x4(simd_quatf(angle: 0.0001, axis: SIMD3(0,1,0)))
    let first = stability.render(stabilityState, width: 1536,height: 864,tangents: nil,ipd: 0.064,head: head,time: 10)!
    let second = stability.render(stabilityState, width: 1536,height: 864,tangents: nil,ipd: 0.064,head: jitter,time: 10)!
    let a = [UInt8](first.dataProvider!.data! as Data), b = [UInt8](second.dataProvider!.data! as Data)
    var changed = [Bool](repeating: false,count: 1536*864)
    for y in 0..<864 { for x in 0..<1536 {
        let at = y*first.bytesPerRow+x*4
        if Int(b[at+2]) > Int(b[at+1])+8 && Int(b[at+1]) > Int(b[at])+4 && abs(Int(a[at+2])-Int(b[at+2])) > 15 {
            changed[y*1536+x] = true
        }
    }}
    var largest = 0
    for start in changed.indices where changed[start] {
        var pending = [start], count = 0; changed[start] = false
        while let at = pending.popLast() {
            count += 1
            for dy in -1...1 { for dx in -1...1 {
                let x = at%1536+dx, y = at/1536+dy
                guard (0..<1536).contains(x), (0..<864).contains(y) else { continue }
                let next = y*1536+x
                if changed[next] { changed[next] = false; pending.append(next) }
            }}
        }
        largest = max(largest,count)
    }
    print("Loading depth jitter: largest changing patch \(largest) pixels")
    precondition(largest <= 16, "Subpixel head motion must not flash building faces")
    // A loaded mesh alone does not prove the side points survived filtering.
    // Model a sparse point atlas whose smaller authored mips lose its ink.
    let pointScene = NativeLoadingMap.Scene(game: largeRoot)!
    pointScene.info = stability.info; pointScene.order = [name]
    var pointVertices: [SCNVector3] = [], pointUV: [CGPoint] = []
    let corners: [CGPoint] = [.init(x: 0,y: 0),.init(x: 1,y: 0),.init(x: 1,y: 1),.init(x: 0,y: 1)]
    for row in 0..<8 { for column in 0..<32 {
        for index in [0,1,2,0,2,3] {
            let uv = corners[index]
            pointVertices.append(SCNVector3(Float(column)*0.5-8+Float(uv.x)*0.01,0.05,Float(row)*0.5-1.5+Float(uv.y)*0.01))
            pointUV.append(uv)
        }
    }}
    let pointFolder = "models/ui/map_hologram_steamvr/"
    pointScene.geometry[pointFolder + "voxel_cubes.obj"] = SCNGeometry(
        sources: [SCNGeometrySource(vertices: pointVertices), SCNGeometrySource(textureCoordinates: pointUV)],
        elements: [SCNGeometryElement(indices: Array(0..<UInt32(pointVertices.count)), primitiveType: .triangles)])
    for kind in ["ao","wire"] {
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: 32,height: 32,mipmapped: true)
        desc.storageMode = .shared; desc.usage = .shaderRead
        let texture = pointScene.device.makeTexture(descriptor: desc)!
        for level in 0..<texture.mipmapLevelCount {
            let size = max(1,32 >> level)
            let bytes = [UInt8](repeating: level == 0 ? 255 : 0,count: size*size)
            bytes.withUnsafeBytes { texture.replace(region: MTLRegionMake2D(0,0,size,size),mipmapLevel: level,withBytes: $0.baseAddress!,bytesPerRow: size) }
        }
        pointScene.textures[pointFolder + "voxel_cubes_" + kind + ".dds"] = texture
    }
    pointScene.buildMap(stabilityState); pointScene.lastState = stabilityState
    var pointNodes: [SCNNode] = []
    pointScene.mapNode!.enumerateChildNodes { node,_ in
        if ["loading-map-points","loading-map-point-wire"].contains(node.name ?? "") { pointNodes.append(node) }
    }
    precondition(pointNodes.count == 2)
    for (width,height) in [(1536,864),(2880,1620)] {
        for pose in [head,jitter] {
            func pointImage(_ hidden: Bool) -> CGImage {
                for node in pointNodes { node.isHidden = hidden }
                return pointScene.render(stabilityState,width: width,height: height,tangents: nil,ipd: 0.064,head: pose,time: 10)!
            }
            let background = pointImage(true), visible = pointImage(false)
            let back = [UInt8](background.dataProvider!.data! as Data), ink = [UInt8](visible.dataProvider!.data! as Data)
            var counts = [Int](repeating: 0,count: 4)
            for y in 0..<height { for x in 0..<width {
                let at = y*visible.bytesPerRow+x*4, eyeWidth = width/2, local = x%eyeWidth
                guard Int(ink[at+2])-Int(back[at+2]) > 6 else { continue }
                if local < eyeWidth/4 { counts[(x/eyeWidth)*2] += 1 }
                if local >= eyeWidth*3/4 { counts[(x/eyeWidth)*2+1] += 1 }
            }}
            print("Loading side points \(width)x\(height): \(counts)")
            precondition(counts.allSatisfy { $0 >= 10 }, "Side points must remain visible on both sides of both eyes")
        }
    }
    for timestamp in 2...5 {
        let repeated = draw(head, timestamp: Double(timestamp))
        precondition(repeated.image.dataProvider!.data! as Data == loading.image.dataProvider!.data! as Data,
                     "A stationary loading map must not flicker between identical renders")
    }
    var ready = try! JSONSerialization.jsonObject(with: json) as! [String: Any]
    ready["messageText"] = "Press Trigger To Start"
    ready["waitingForInput"] = true
    try! JSONSerialization.data(withJSONObject: ready).write(to: URL(fileURLWithPath: stateFile), options: .atomic)
    probe.poll(now: 0.4, inputReady: false)
    precondition(probe.state?.messageText == "", "Busy/unknown Windows shader state must withhold the trigger hint")
    let stillLoading = draw(head, timestamp: 0.4)
    precondition(stillLoading.image.dataProvider!.data! as Data == loading.image.dataProvider!.data! as Data)
    _ = draw(head, timestamp: 5)
    // Hold the scene worker across a mailbox update, as during a slow asset upload.
    probe.queue.sync {}
    let blocked = DispatchSemaphore(value: 0), release = DispatchSemaphore(value: 0)
    probe.queue.async { blocked.signal(); release.wait() }
    blocked.wait()
    probe.poll(now: 1)
    let movedWhileBusy = head * simd_float4x4(simd_quatf(angle: 0.1, axis: SIMD3(0,1,0)))
    let retained = probe.draw(into: pixel!, tangents: nil, ipd: 0.064, head: movedWhileBusy, timestamp: 6)
    guard retained?.timestamp == 5 && retained?.head == head else {
        print("Loading cache regression: mailbox update discarded the complete stereo frame")
        exit(1)
    }
    release.signal()
    let waiting = draw(head, timestamp: 2); save(waiting.image, output + ".ready.png")
    precondition(waiting.image.dataProvider!.data! as Data != loading.image.dataProvider!.data! as Data)
    // A rejected render must not replace the cached picture with a blank frame.
    probe.queue.sync {}
    _ = probe.draw(into: pixel!, tangents: Array(repeating: 0, count: 8), ipd: 0.064, head: head, timestamp: 99)
    probe.queue.sync {}
    precondition(probe.draw(into: pixel!, tangents: nil, ipd: 0.064, head: head, timestamp: 2)?.timestamp == 2,
                 "A failed render discarded the previous stereo frame")
    let turn = head * simd_float4x4(simd_quatf(angle: 0.25, axis: SIMD3(0,1,0)))
    let turned = draw(turn, timestamp: 3); save(turned.image, output + ".turned.png")
    precondition(turned.head == turn && turned.image.dataProvider!.data! as Data != waiting.image.dataProvider!.data! as Data)
    var hidden = ready; hidden["visible"] = false
    try! JSONSerialization.data(withJSONObject: hidden).write(to: URL(fileURLWithPath: stateFile), options: .atomic)
    probe.poll(now: 1.5); precondition(probe.active && !probe.visible)
    precondition(probe.draw(into: pixel!, tangents: nil, ipd: 0.064, head: head, timestamp: 4) == nil,
                 "Initial boot must not display the room, city or trigger hint")
    try! FileManager.default.removeItem(atPath: stateFile); probe.poll(now: 2); precondition(!probe.active)
    try! Data("{\"type\":\"begin_loading\",\"mapName\":\"../outside\"}".utf8).write(to: URL(fileURLWithPath: stateFile))
    probe.poll(now: 3); precondition(!probe.active)
    for file in [stateFile, stageFile, stageFile + ".ready"] { try? FileManager.default.removeItem(atPath: file) }
    print("Native loading scene: room, city and point field, textures, world-space prompt, head motion, stable stereo cache across updates/failures, load/end lifecycle and bounded paths passed")
    exit(0)
}

let gamePrompt = GamePromptOverlay(path: ProcessInfo.processInfo.environment["VERANTYX_GAME_PROMPT_FILE"],
                                  statePath: ProcessInfo.processInfo.environment["VERANTYX_GAME_STATE_FILE"],
                                  shaderPath: ProcessInfo.processInfo.environment["VERANTYX_SHADER_PROGRESS_FILE"])

// VERANTYX_PROMPT_SELFTEST=<png> [VERANTYX_PROMPT_SELFTEST_TEXT, _SIZE=WxH, _TANGENTS="8 numbers"]: draws the prompt
// into a grey frame with a cross at each eye's straight-ahead point, writes the PNG and exits (nothing else starts).
if let out = ProcessInfo.processInfo.environment["VERANTYX_PROMPT_SELFTEST"] ?? ProcessInfo.processInfo.environment["VERANTYX_LOADING_SELFTEST"] {
    let env = ProcessInfo.processInfo.environment
    let size = (env["VERANTYX_PROMPT_SELFTEST_SIZE"] ?? "2880x1620").split(separator: "x").compactMap { Int($0) }
    let tangents = env["VERANTYX_PROMPT_SELFTEST_TANGENTS"].map { $0.split(separator: " ").compactMap { Float($0) } }
    let (w, h) = (size.count == 2 ? size[0] : 2880, size.count == 2 ? size[1] : 1620)
    var pb: CVPixelBuffer?
    CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_32BGRA, [kCVPixelBufferIOSurfacePropertiesKey: [:]] as CFDictionary, &pb)
    guard let pb else { print("no pixel buffer"); exit(1) }
    CVPixelBufferLockBaseAddress(pb, [])
    let base = CVPixelBufferGetBaseAddress(pb)!, rowBytes = CVPixelBufferGetBytesPerRow(pb)
    for y in 0..<h { for x in 0..<w { base.storeBytes(of: UInt32(0xFF30_3030), toByteOffset: y * rowBytes + x * 4, as: UInt32.self) } }
    for (cx, cy) in GamePromptOverlay.centers(width: w, height: h, tangents: tangents, ipd: 0, offsetX: 0, offsetY: 0) {   // (crosses: at infinity)
        for d in -40...40 {
            for (x, y) in [(Int(cx) + d, Int(cy)), (Int(cx), Int(cy) + d)] where x >= 0 && x < w && y >= 0 && y < h {
                base.storeBytes(of: UInt32(0xFFFF2020), toByteOffset: y * rowBytes + x * 4, as: UInt32.self)
            }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, [])
    let fromFile: GamePromptOverlay
    if env["VERANTYX_LOADING_SELFTEST"] != nil {
        fromFile = GamePromptOverlay(path: env["VERANTYX_GAME_PROMPT_FILE"], statePath: env["VERANTYX_GAME_STATE_FILE"],
                                    shaderPath: env["VERANTYX_SHADER_PROGRESS_FILE"], language: env["VERANTYX_LOADING_SELFTEST_LANGUAGE"] ?? "en", startedAt: 0)
        let now = Double(env["VERANTYX_LOADING_SELFTEST_UPTIME"] ?? "") ?? 2
        let wall = Double(env["VERANTYX_LOADING_SELFTEST_NOW"] ?? "") ?? Date().timeIntervalSince1970
        if let age = Double(env["VERANTYX_LOADING_SELFTEST_GAME_FRAME_AGE"] ?? "") {
            _ = fromFile.noteFrame(sourceMode: 2, now: now - age)
        }
        fromFile.poll(now: now, wallTime: wall)
        if env["VERANTYX_LOADING_SELFTEST_GAMEPLAY"] == "1" {
            precondition(fromFile.text != nil)
            _ = fromFile.noteFrame(sourceMode: 0, now: 2.1)
            fromFile.poll(now: 2.1, wallTime: wall, force: true)
            precondition(fromFile.text != nil) // Compositor frames do not prove that the game resumed.
            for step in 0..<20 {
                let time = 2.2 + Double(step) * 0.1
                let resumed = fromFile.noteFrame(sourceMode: 2, now: time)
                fromFile.poll(now: time, wallTime: wall, force: resumed)
                precondition(fromFile.text == nil)
            }
            fromFile.poll(now: 5.4, wallTime: wall)
            precondition(fromFile.text == nil) // A short hitch stays unobstructed.
            fromFile.poll(now: 5.7, wallTime: wall)
            precondition(fromFile.text != nil)
            let resumed = fromFile.noteFrame(sourceMode: 2, now: 5.71)
            precondition(resumed)
            fromFile.poll(now: 5.71, wallTime: wall, force: resumed)
            precondition(fromFile.text == nil)
            print("loading selftest: gameplay, short hitch, stall and immediate resume -> ok")
            exit(0)
        }
        if let expected = env["VERANTYX_LOADING_SELFTEST_EXPECT"], (fromFile.text ?? "(none)") != expected {
            print("loading selftest: expected \(expected), got \(fromFile.text ?? "(none)")"); exit(1)
        }
        var pregameClock = LoadingFrameClock(awaitingSource: true)
        precondition(pregameClock.needsFrame(hasSource: false, hasOverlay: false, now: 0))
        pregameClock.sent(now: 0, hasOverlay: false, hasSource: false)
        precondition(!pregameClock.needsFrame(hasSource: false, hasOverlay: false, now: 0.05))
        precondition(pregameClock.needsFrame(hasSource: false, hasOverlay: false, now: 0.11))
        pregameClock.sent(now: 0.2, hasOverlay: false, hasSource: true)
        precondition(!pregameClock.needsFrame(hasSource: false, hasOverlay: false, now: 0.31))
        var clock = LoadingFrameClock()
        precondition(clock.needsFrame(hasSource: false, hasOverlay: true, now: 0))
        clock.sent(now: 0, hasOverlay: true, hasSource: false)
        precondition(!clock.needsFrame(hasSource: false, hasOverlay: true, now: 0.05))
        precondition(clock.needsFrame(hasSource: false, hasOverlay: true, now: 0.11))
        precondition(clock.needsFrame(hasSource: false, hasOverlay: false, now: 1))
        clock.sent(now: 1, hasOverlay: false, hasSource: false)
        precondition(clock.needsFrame(hasSource: false, hasOverlay: false, now: 1.11))
        clock.sent(now: 1.2, hasOverlay: false, hasSource: true)
        precondition(!clock.needsFrame(hasSource: false, hasOverlay: false, now: 1.31))
        precondition(clock.needsFrame(hasSource: true, hasOverlay: false, now: 0.05))
        if fromFile.text == nil { print("loading selftest: no prompt -> ok"); exit(0) }
        if let badge = GamePromptOverlay.render(fromFile.text!, eyeWidth: w / 2, eyeHeight: h, activityPhase: 0) {
            precondition(Double(badge.width) <= Double(w / 2) * 0.28 + 1)
            precondition(Double(badge.height) <= Double(h) * 0.09)
            let size = CGSize(width: badge.width, height: badge.height)
            let points = GamePromptOverlay.centers(width: w, height: h, tangents: tangents, ipd: 0.063, badgeSize: size)
            for (eye, p) in points.enumerated() {
                let rect = CGRect(x: p.x - size.width / 2, y: p.y - size.height / 2, width: size.width, height: size.height)
                precondition(rect.minX >= Double(eye * (w / 2)) + Double(w / 2) * 0.1 - 1)
                precondition(rect.maxX <= Double((eye + 1) * (w / 2)) - Double(w / 2) * 0.1 + 1)
                precondition(rect.minY >= Double(h) * 0.1 - 1 && rect.maxY <= Double(h) * 0.9 + 1)
            }
            print("loading selftest: compact badge and binocular safe inset -> ok")
        }
    } else {
        let text = env["VERANTYX_PROMPT_SELFTEST_TEXT"] ?? "Hold the trigger to start"
        let file = NSTemporaryDirectory() + "verantyx-prompt-selftest-\(getpid()).txt"
        try? Data(text.utf8).write(to: URL(fileURLWithPath: file))
        fromFile = GamePromptOverlay(path: file)
        fromFile.poll(now: 0)
        try? FileManager.default.removeItem(atPath: file)
    }
    let drew = fromFile.draw(into: pb, tangents: tangents, ipd: 0.063)
    let image = CIImage(cvPixelBuffer: pb)
    guard drew, let cg = CIContext().createCGImage(image, from: image.extent),
          let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: out) as CFURL, "public.png" as CFString, 1, nil) else {
        print("prompt selftest: nothing drawn"); exit(1)
    }
    CGImageDestinationAddImage(dest, cg, nil)
    CGImageDestinationFinalize(dest)
    let c = GamePromptOverlay.centers(width: w, height: h, tangents: tangents, ipd: 0.063)
    print(String(format: "prompt selftest: %dx%d, centres (%.1f, %.1f) and (%.1f, %.1f) -> %@", w, h, c[0].x, c[0].y, c[1].x, c[1].y, out))
    exit(0)
}

// Exercise the real encoder settings and callback without a game, audio,
// shared map, network listener or headset. JSON only; no per-frame logging.
if let path = encoderProbePath {
    precondition(!alvrMode)
    let width = 3056, height = 1528, count = 180, hz = 72.0
    setupEncoder(width: Int32(width), height: Int32(height))
    VTSessionSetProperty(compressionSession!, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: NSNumber(value: hz))
    let pool = makePixelBufferPool(width: width, height: height)!
    var buffers: [CVPixelBuffer] = []
    for phase in 0..<8 {
        var buffer: CVPixelBuffer?
        precondition(CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &buffer) == kCVReturnSuccess)
        let pb = buffer!; CVPixelBufferLockBaseAddress(pb, [])
        let base = CVPixelBufferGetBaseAddress(pb)!, stride = CVPixelBufferGetBytesPerRow(pb)
        for y in 0..<height {
            let row = base.advanced(by: y*stride).assumingMemoryBound(to: UInt32.self)
            for x in 0..<width {
                let v = UInt32(((x+phase*3)/8*13 + y/8*7) & 255)
                row[x] = 0xff000000 | (v << 16) | (((v+85)&255) << 8) | ((v+170)&255)
            }
        }
        CVPixelBufferUnlockBaseAddress(pb, []); buffers.append(pb)
    }
    let began = ProcessInfo.processInfo.systemUptime
    var calls: [Double] = []
    for index in 0..<count {
        let target = began + Double(index)/hz
        let wait = target - ProcessInfo.processInfo.systemUptime
        if wait > 0 { usleep(useconds_t(wait*1_000_000)) }
        precondition(encoderInFlightFrames.wait(timeout: .now()+2) == .success)
        let submitted = ProcessInfo.processInfo.systemUptime
        let context = UnsafeMutablePointer<FrameContext>.allocate(capacity: 1)
        context.initialize(to: FrameContext(sequence: UInt32(index), visionTimestamp: Double(index)/hz,
            transform: (1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1), pixelBuffer: buffers[index%buffers.count],
            submittedAt: submitted))
        let status = VTCompressionSessionEncodeFrame(compressionSession!, imageBuffer: context.pointee.pixelBuffer,
            presentationTimeStamp: CMTime(value: Int64(index), timescale: 72), duration: .invalid,
            frameProperties: nil, sourceFrameRefcon: UnsafeMutableRawPointer(context), infoFlagsOut: nil)
        precondition(status == noErr)
        calls.append((ProcessInfo.processInfo.systemUptime-submitted)*1000)
    }
    let inputDuration = ProcessInfo.processInfo.systemUptime-began
    VTCompressionSessionCompleteFrames(compressionSession!, untilPresentationTimeStamp: .invalid)
    for _ in 0..<3 { encoderInFlightFrames.wait() }
    VTCompressionSessionInvalidate(compressionSession!)
    encoderDiagnosticLock.lock(); let delays = Array(encoderProbeDelays.dropFirst(16)).sorted(); encoderDiagnosticLock.unlock()
    func percentile(_ values: [Double], _ fraction: Double) -> Double {
        let ordered = values.sorted(); return ordered.isEmpty ? 0 : ordered[Int(Double(ordered.count-1)*fraction)]
    }
    let report: [String: Any] = ["width": width, "height": height, "frames": count, "callbacks": encoderDelayCount,
        "input_seconds": inputDuration, "delay_median_ms": percentile(delays,0.5), "delay_p95_ms": percentile(delays,0.95),
        "encode_call_p95_ms": percentile(calls,0.95), "max_delay": ProcessInfo.processInfo.environment["VERANTYX_ENCODER_MAX_DELAY"] ?? "default"]
    try! JSONSerialization.data(withJSONObject: report, options: [.sortedKeys]).write(to: URL(fileURLWithPath: path))
    precondition(encoderDelayCount == count)
    print(String(format: "Encoder test: median %.1f ms, p95 %.1f ms, input %.2f s", percentile(delays,0.5),percentile(delays,0.95),inputDuration))
    exit(0)
}

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

func loadingFrame() -> SharedFrame {
    sharedHandsWriteLock.lock()
    defer { sharedHandsWriteLock.unlock() }
    var transform: (Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float)
        = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
    let timestamp = handsMapPtr.loadUnaligned(fromByteOffset: 244, as: Double.self)
    if timestamp.isFinite && timestamp > 0 {
        _ = withUnsafeMutableBytes(of: &transform) { memcpy($0.baseAddress!, handsMapPtr, 64) }
        transform.13 += loadingHeightOffset(handsMapPtr.advanced(by: 512))
    }
    return SharedFrame(sequenceNumber: 0, width: UInt32(outputWidth), height: UInt32(outputHeight), format: 87,
                       renderHeadTransform: transform, visionTimestamp: timestamp.isFinite ? timestamp : 0)
}

// ALVR's input into SharedHands (offsets as the Vision Pro's POSE packet writes them; ALVRBridge's VxAlvrInput layout,
// which its Rust side checks at compile time), and its clients' IDR requests to the encoder.
if let alvr {
    Thread {
        let input = UnsafeMutableRawPointer.allocate(byteCount: 312, alignment: 8)
        var seq: UInt64 = 0
        var previousControls: UInt64?
        var previousTriggers: UInt8 = 0
        var triggerPresses = [UInt64(0), UInt64(0)]
        var controlsChanged = false
        var lastInputLog: TimeInterval = -.infinity
        // The game's vibrations, which the driver writes per hand at 336 in the map's tail (SharedHaptic: seq, then
        // duration, frequency, amplitude), to the headset (the seqs already there are old ones: skipped)
        var hapticSeq = [handsMapPtr.advanced(by: 336).load(as: UInt32.self), handsMapPtr.advanced(by: 352).load(as: UInt32.self)]
        var checked = Date()
        while isEncoding {
            if alvr.takeIDR() { requestKeyFrame() }
            if Date().timeIntervalSince(checked) > 0.5 {                // (a headset with other display settings:
                checked = Date()                                        //  scripts/launch_vxrt_steamvr.sh starts the
                // The headset's eye distance, for the driver (Prop_UserIpdMeters): in the map's tail after SharedHands,
                // at 320 ("VIPD", then the metres; driver_verantyx.cpp's SharedExtras)
                if let ipd = alvr.ipd?(), ipd > 0 {
                    handsMapPtr.advanced(by: 324).storeBytes(of: ipd, as: Float.self)
                    OSMemoryBarrier()
                    handsMapPtr.advanced(by: 320).storeBytes(of: UInt32(0x4450_4956), as: UInt32.self)
                }
                if alvr.connected() { checkHeadsetFrustumAgainstSteamVR() }
                // ALVR sometimes cannot list the game audio device this process made, and then refuses every handshake
                // for as long as this process lives ("Cannot find audio device"): a new encoder makes a new device.
                if !alvr.connected(), let audioName = ProcessInfo.processInfo.environment["VERANTYX_ALVR_AUDIO_DEVICE"],
                   let log = FileHandle(forReadingAtPath: FileManager.default.homeDirectoryForCurrentUser
                       .appendingPathComponent("Library/Application Support/VeRantyx/ALVR/session_log.txt").path) {
                    defer { try? log.close() }
                    let end = log.seekToEndOfFile()
                    log.seek(toFileOffset: end > 4096 ? end - 4096 : 0)
                    if String(decoding: log.readDataToEndOfFile(), as: UTF8.self).contains("Cannot find audio device which name contains \"\(audioName)\"") {
                        print("ALVR: the game audio device cannot be found: the encoder starts again")
                        fflush(stdout)
                        _exit(75)
                    }
                }
                if alvr.restartNeeded() {                               //  encoder again, which connects with them;
                                                                        //  also when ALVR's handshake hung: lib.rs)
                    print("ALVR: the headset brought other display settings (stored), or its handshake hung: the encoder starts again")
                    fflush(stdout)
                    _exit(75)                                           // (not exit: its handlers ran into ALVR's and
                                                                        //  the audio tap's threads, and it died of it)
                }
            }
            if let haptics = alvr.haptics {
                for hand in 0..<2 {
                    let at = handsMapPtr.advanced(by: 336 + 16 * hand)
                    let s = at.load(as: UInt32.self)
                    if s != hapticSeq[hand] {
                        hapticSeq[hand] = s
                        OSMemoryBarrier()
                        haptics(UInt32(hand), at.load(fromByteOffset: 4, as: Float.self), at.load(fromByteOffset: 8, as: Float.self),
                                at.load(fromByteOffset: 12, as: Float.self))
                    }
                }
            }
            let next = alvr.input(input, seq)
            if next != seq {
                seq = next
                let leftTrigger = input.load(fromByteOffset: 307, as: UInt8.self)
                let rightTrigger = input.load(fromByteOffset: 308, as: UInt8.self)
                let rightButtons = input.load(fromByteOffset: 296, as: UInt32.self)
                let leftButtons = input.load(fromByteOffset: 300, as: UInt32.self)
                let triggers: UInt8 = (leftTrigger >= 192 ? 1 : 0) | (rightTrigger >= 192 ? 2 : 0)
                for hand in 0..<2 where triggers & (1 << hand) != 0 && previousTriggers & (1 << hand) == 0 { triggerPresses[hand] &+= 1 }
                let controls = UInt64(leftButtons) << 32 | UInt64(rightButtons)
                controlsChanged = controlsChanged || previousControls != controls || previousTriggers != triggers
                previousTriggers = triggers
                previousControls = controls
                let now = ProcessInfo.processInfo.systemUptime
                if controlsChanged, now - lastInputLog >= 1 {
                    lastInputLog = now; controlsChanged = false
                    print("ALVR input: seq=\(seq) trigger=\(leftTrigger)/\(rightTrigger) presses=\(triggerPresses[0])/\(triggerPresses[1]) buttons=\(leftButtons)/\(rightButtons) hands=\(input.load(fromByteOffset: 305, as: UInt8.self))/\(input.load(fromByteOffset: 306, as: UInt8.self))")
                }
                encoderDiagnosticLock.lock()
                posePacketCount += 1
                lastPoseReceivedAt = ProcessInfo.processInfo.systemUptime
                encoderDiagnosticLock.unlock()
                let hasTangents = input.load(fromByteOffset: 311, as: UInt8.self) != 0
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
                    if hasTangents {
                        memcpy(handsMapPtr + 252, input + 264, 32)     // eye tangents, left then right
                    }
                    memcpy(handsMapPtr + 284, input + 309, 2)          // grips
                    memcpy(handsMapPtr + 286, input + 304, 1)          // headTracked
                    memcpy(handsMapPtr + 287, input + 224, 24)         // angular velocities, left then right
                    handsMapPtr.advanced(by: 311).storeBytes(of: UInt8(1), as: UInt8.self)
                }
                // Formatting and disk I/O must not leave the pose sequence odd.
                if hasTangents { saveHeadsetFrustum(input + 264) }
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
    let gap = longestCaptureGap, stalls = captureStalls
    let encodeMean = encoderDelayCount > 0 ? encoderDelaySum/Double(encoderDelayCount) : 0
    let encodeMax = encoderDelayMax
    encoderDelayCount = 0; encoderDelaySum = 0; encoderDelayMax = 0
    longestCaptureGap = 0; captureStalls = 0
    encoderDiagnosticLock.unlock()
    peerLock.lock()
    let endpoint = videoPeer.map { String(describing: $0.endpoint) } ?? "none"
    peerLock.unlock()
    let captureFPS = Double(current.capture - lastDiagnostics.capture) / elapsed
    print(String(format: "Pipeline %.1fs source=%u capture=%.1ffps submit=%.1ffps encoded=%.1ffps sent=%.1ffps UDP=%.2fMbps bitrate=%.1fMbps packets=%d sendErrors=%d networkDrops=%d encoderDrops=%d pose=%.1fHz poseAge=%.2fs gap=%.0fms stalls=%d encode=%.1f/%.1fms peer=%@",
        elapsed, dbHeaderPtr.pointee.captureSourceMode, captureFPS,
        Double(current.submit - lastDiagnostics.submit) / elapsed,
        Double(current.encode - lastDiagnostics.encode) / elapsed,
        Double(current.sent - lastDiagnostics.sent) / elapsed,
        Double(current.bytes - lastDiagnostics.bytes) * 8 / elapsed / 1_000_000,
        Double(adaptiveBitrate.current) / 1_000_000,
        current.packets - lastDiagnostics.packets, current.errors - lastDiagnostics.errors,
        current.drops - lastDiagnostics.drops, droppedEncoderFrames,
        Double(current.poses - lastDiagnostics.poses) / elapsed, poseAge, gap * 1000, stalls, encodeMean, encodeMax, endpoint))
    if videoPeer != nil {
        adaptiveBitrate.update(
            localLossEvents: (current.errors - lastDiagnostics.errors) + (current.drops - lastDiagnostics.drops),
            elapsed: elapsed, session: compressionSession)
    } else if let alvr, alvr.connected(), let linkTrouble = alvr.linkTrouble {
        // (an ALVR headset: what its statistics say of the link, ALVRBridge's vx_alvr_link_trouble)
        adaptiveBitrate.update(localLossEvents: Int(linkTrouble()), elapsed: elapsed, session: compressionSession)
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
    // (a flat game in stereo, captureSourceMode 3: two whole 16:10-ish pictures side by side, ~3.1:1)
    let maximumAspect = dbHeaderPtr.pointee.captureSourceMode == flatStereoSourceMode ? 4.0 : 2.5
    let sourceAspect = min(maximumAspect, max(0.5, Double(sourceWidth) / Double(sourceHeight)))
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
    guard !alvrMode else { return } // ALVR owns the controller inputs in this mode.
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
    guard !alvrMode else { return }
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
let videoListener: NWListener?
if alvrMode {
    videoListener = nil
} else {
    do {
        videoListener = try NWListener(using: networkParameters, on: 9999)
    } catch {
        print("Could not listen for Vision Pro on port 9999: \(error)")
        exit(1)
    }
}
if let videoListener {
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
}

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
        guard !alvrMode else {
            print("Game audio: ALVR records its dedicated tap; legacy UDP audio capture is off")
            return
        }
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
            kAudioAggregateDeviceUIDKey as String: "com.verantyx.vr.audio-tap.\(getpid()).\(UUID().uuidString)",
            kAudioAggregateDeviceIsPrivateKey as String: true,
            kAudioAggregateDeviceTapAutoStartKey as String: true,
            kAudioAggregateDeviceTapListKey as String: [[
                kAudioSubTapUIDKey as String: description.uuid.uuidString,
                kAudioSubTapDriftCompensationKey as String: true,
            ]],
        ]
        status = AudioHardwareCreateAggregateDevice(aggregate as CFDictionary, &aggregateID)
        guard status == noErr else {
            AudioHardwareDestroyProcessTap(tapID)
            tapID = AudioObjectID(kAudioObjectUnknown)
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

// A flat game made stereo by VXRuntime (VXRT_STEREO_STREAM): captureSourceMode 3, both eyes side by side with no
// head pose. "SCRN" + 4 floats once a second tells the headset to show it on a world-locked virtual screen: the
// picture's aspect (one eye), the screen's width and distance in metres (VERANTYX_SCREEN_WIDTH/_DISTANCE, default
// 3 m at 3.5 m: 2 % disparity at infinity is then 6 cm, about the IPD), and the packet's version.
let flatStereoSourceMode: UInt32 = 3
let screenModeMagic: UInt32 = 0x4E524353 // "SCRN"
let screenWidthMeters = Float(ProcessInfo.processInfo.environment["VERANTYX_SCREEN_WIDTH"] ?? "") ?? 3.0
let screenDistanceMeters = Float(ProcessInfo.processInfo.environment["VERANTYX_SCREEN_DISTANCE"] ?? "") ?? 3.5
var lastScreenModeSentAt: TimeInterval = -.infinity
var lastSourceFrameSize = (width: 0, height: 0)

func sendScreenModeIfDue() {
    let now = ProcessInfo.processInfo.systemUptime
    guard now - lastScreenModeSentAt >= 1, dbHeaderPtr.pointee.captureSourceMode == flatStereoSourceMode,
          lastSourceFrameSize.width > 0, lastSourceFrameSize.height > 0 else { return }
    lastScreenModeSentAt = now
    peerLock.lock()
    let peer = videoPeer
    peerLock.unlock()
    guard let peer else { return }
    let values: [Float] = [Float(lastSourceFrameSize.width) / 2 / Float(lastSourceFrameSize.height),
                           screenWidthMeters, screenDistanceMeters, 1]
    var packet = Data(capacity: 20)
    withUnsafeBytes(of: screenModeMagic.littleEndian) { packet.append(contentsOf: $0) }
    for value in values {
        withUnsafeBytes(of: value.bitPattern.littleEndian) { packet.append(contentsOf: $0) }
    }
    peer.send(content: packet, completion: .idempotent)
}

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
var loadingFrameClock = LoadingFrameClock(awaitingSource: alvrMode)
var loadingFrames = 0
var lastEncodedLoadingMap: String?
while isEncoding {
    autoreleasepool {
        reportDiagnosticsIfDue()
        sendSourceProjectionIfDue()
        sendStreamStatsIfDue()
        sendScreenModeIfDue()
        let pollTime = ProcessInfo.processInfo.systemUptime
        gamePrompt.poll(now: pollTime)
        nativeLoadingMap.poll(now: pollTime, inputReady: gamePrompt.readyForInput)
        if !nativeLoadingMap.active { lastEncodedLoadingMap = nil }
        let sourceMode = dbHeaderPtr.pointee.captureSourceMode
        if sourceMode != lastCaptureSourceMode {
            print("Capture source changed \(lastCaptureSourceMode)->\(sourceMode); continuing stream sequence \(streamSequence)")
            lastCaptureSourceMode = sourceMode
            lastSeq = 0
            requestKeyFrame()
        }
        let readyIndex = dbHeaderPtr.pointee.latestReadyIndex
        OSMemoryBarrier()
        let headerPtr = headers[Int(readyIndex < 2 ? readyIndex : 0)]
        let sourceSeq = headerPtr.pointee.sequenceNumber
        let hasGameSource = readyIndex < 2 && sourceSeq != lastSeq && sourceSeq > 0
        let hasSource = hasGameSource && !nativeLoadingMap.active
        let currentSeq = hasGameSource ? sourceSeq : lastSeq
        let validSize = headerPtr.pointee.width > 0 && headerPtr.pointee.height > 0 &&
            headerPtr.pointee.width <= 8192 && headerPtr.pointee.height <= 4096
        let resumed = hasSource && validSize && gamePrompt.noteFrame(sourceMode: sourceMode, now: pollTime)
        gamePrompt.poll(now: pollTime, force: resumed)
        let hasLoadingOverlay = nativeLoadingMap.active || gamePrompt.text != nil
        let loadingInterval = nativeLoadingMap.visible ? 1.0 / 60 : 0.1
        if readyIndex < 2 || loadingFrameClock.needsFrame(hasSource: false, hasOverlay: hasLoadingOverlay, now: pollTime, interval: loadingInterval) {
            if loadingFrameClock.needsFrame(hasSource: hasSource, hasOverlay: hasLoadingOverlay, now: pollTime, interval: loadingInterval) {
                var frameHeader = hasSource ? headerPtr.pointee : loadingFrame()
                let width = Int(frameHeader.width)
                let height = Int(frameHeader.height)
                if hasSource {
                    encoderDiagnosticLock.lock()
                    capturedFrameCount += 1
                    if lastCapturedFrameAt > 0 {
                        let gap = pollTime - lastCapturedFrameAt
                        longestCaptureGap = max(longestCaptureGap, gap)
                        if gap > 0.1 { captureStalls += 1 }
                    }
                    lastCapturedFrameAt = pollTime
                    encoderDiagnosticLock.unlock()
                }

                // (Wider than 4096 for 2.5K per eye side by side; a frame block holds 4096 x 4096 x 8 bytes.)
                guard width > 0, height > 0, width <= 8192, height <= 4096, width * height * 4 <= 4096 * 4096 * 8 else {
                    lastSeq = currentSeq
                    return
                }

                if hasSource { lastSourceFrameSize = (width, height) }   // (the game's frame, not the loading picture)
                let outputSizeChanged = hasSource && resolveOutputSize(sourceWidth: width, sourceHeight: height)
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
                    if hasSource { resolveOutputSize(sourceWidth: width, sourceHeight: height) }
                    print("Initializing fixed-size VideoToolbox encoder for \(outputWidth)x\(outputHeight)...")
                    fflush(stdout)
                    setupEncoder(width: Int32(outputWidth), height: Int32(outputHeight))
                }

                if pixelBufferPool == nil {
                    pixelBufferPool = makePixelBufferPool(width: outputWidth, height: outputHeight)
                }

                // A loading heartbeat has no captured frame to discard. Yield
                // briefly under backpressure instead of retrying it in a spin.
                let capacityDeadline: DispatchTime = hasSource ? .now() : .now() + .milliseconds(5)
                guard encoderInFlightFrames.wait(timeout: capacityDeadline) == .success else {
                    lastSeq = currentSeq
                    if hasSource {
                        droppedEncoderFrames += 1
                        if droppedEncoderFrames == 1 || droppedEncoderFrames % 90 == 0 {
                            print("Dropped capture frame: VideoToolbox has 3 frames in flight (total dropped=\(droppedEncoderFrames))")
                        }
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
                let tryGPU = hasSource && conversionSelector.preferGPU()
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
                        if hasSource {
                            didCopy = copyFrameToBGRA(
                                source: pixelDatas[Int(readyIndex)],
                                sourceFormat: sourceFormat,
                                sourceWidth: width,
                                sourceHeight: height,
                                destination: destination,
                                destinationBytesPerRow: CVPixelBufferGetBytesPerRow(pb)
                            )
                        } else {
                            memset(destination, 0, CVPixelBufferGetBytesPerRow(pb) * CVPixelBufferGetHeight(pb))
                            didCopy = true
                        }
                    }
                    CVPixelBufferUnlockBaseAddress(pb, [])
                    if didCopy && hasSource {
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
                guard !hasSource || (headerPtr.pointee.sequenceNumber == currentSeq &&
                      dbHeaderPtr.pointee.captureSourceMode == sourceMode) else {
                    encoderInFlightFrames.signal()
                    lastSeq = currentSeq
                    return
                }

                var loadingMapView: NativeLoadingMap.View?
                // (a game's prompt from its loading page, over both eyes: GamePromptOverlay)
                if nativeLoadingMap.active || gamePrompt.text != nil {
                    let ipd = handsMapPtr.load(fromByteOffset: 320, as: UInt32.self) == 0x4450_4956
                        ? handsMapPtr.load(fromByteOffset: 324, as: Float.self) : 0.063
                    let safeIPD = (0.04...0.09).contains(ipd) ? ipd : 0.063
                    if nativeLoadingMap.active {
                        let head = withUnsafeBytes(of: frameHeader.renderHeadTransform) { $0.loadUnaligned(as: simd_float4x4.self) }
                        if let view = nativeLoadingMap.draw(into: pb, tangents: readSourceProjection(), ipd: Double(safeIPD),
                            head: head, timestamp: frameHeader.visionTimestamp) {
                            loadingMapView = view
                            withUnsafeBytes(of: view.head) { bytes in
                                withUnsafeMutableBytes(of: &frameHeader.renderHeadTransform) { $0.copyMemory(from: bytes) }
                            }
                            frameHeader.visionTimestamp = view.timestamp
                        } else if nativeLoadingMap.visible {
                            // Asset preparation is asynchronous. Sending the
                            // cleared input here flashes black in the headset.
                            encoderInFlightFrames.signal()
                            return
                        } else if !hasSource {
                            gamePrompt.drawStartup(into: pb, tangents: readSourceProjection(), ipd: safeIPD)
                        }
                    }
                    else { gamePrompt.draw(into: pb, tangents: readSourceProjection(), ipd: safeIPD) }
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
                    pixelBuffer: pb, submittedAt: now
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

                if hasSource && (framesEncoded < 3 || framesEncoded % 90 == 0) {
                    print("Queued capture frame seq=\(currentSeq), source=\(width)x\(height) format=\(sourceFormat), status=\(status)")
                } else if !hasSource {
                    loadingFrames += 1
                    if !hasLoadingOverlay && !loadingFrameClock.awaitingSource { print("Queued loading clear frame, status=\(status)") }
                    else if loadingFrames == 1 || loadingFrames % 100 == 0 { print("Queued loading frame \(loadingFrames), status=\(status)") }
                }
                writeSnapshotIfDue(pb, now: now)

                if status != noErr {
                    print("VTCompressionSessionEncodeFrame failed: \(status)")
                    refConPtr.deinitialize(count: 1)
                    refConPtr.deallocate()
                    encoderInFlightFrames.signal()
                    requestKeyFrame()
                } else {
                    loadingFrameClock.sent(now: now, hasOverlay: hasLoadingOverlay, hasSource: hasSource)
                    if let view = loadingMapView {
                        let key = "map=\(view.mapName) stage=\(view.stageModel ?? "(none)")"
                        if key != lastEncodedLoadingMap {
                            print("Loading map encoded \(key)")
                            lastEncodedLoadingMap = key
                        }
                    }
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
