import CoreMedia
import CoreVideo
import Combine
import Foundation
import VideoToolbox
import simd

struct VRDecodedFrame {
    let sequence: UInt32
    let timestamp: Double
    let headTransform: simd_float4x4
    let pixelBuffer: CVPixelBuffer
}

/// Keeps only the newest decoded frame so the immersive renderer never builds latency.
final class VRVideoFrameStore {
    static let shared = VRVideoFrameStore()

    private let lock = NSLock()
    private var latest: VRDecodedFrame?
    private var generation: UInt64 = 0

    func beginGeneration() -> UInt64 {
        lock.lock()
        generation &+= 1
        latest = nil
        let result = generation
        lock.unlock()
        return result
    }

    func publish(_ frame: VRDecodedFrame, generation candidateGeneration: UInt64) {
        lock.lock()
        guard candidateGeneration == generation else {
            lock.unlock()
            return
        }
        if let latest, frame.sequence != latest.sequence,
           !isNewer(frame.sequence, than: latest.sequence) {
            lock.unlock()
            return
        }
        latest = frame
        lock.unlock()
    }

    func snapshot() -> VRDecodedFrame? {
        lock.lock()
        defer { lock.unlock() }
        return latest
    }

    private func isNewer(_ candidate: UInt32, than current: UInt32) -> Bool {
        Int32(bitPattern: candidate &- current) > 0
    }
}

private final class VRDecompressionFrameContext {
    let sequence: UInt32
    let generation: UInt64
    let timestamp: Double
    let headTransform: simd_float4x4
    var outputStatus: OSStatus = noErr
    var didDecode = false

    init(sequence: UInt32, generation: UInt64, timestamp: Double, headTransform: simd_float4x4) {
        self.sequence = sequence
        self.generation = generation
        self.timestamp = timestamp
        self.headTransform = headTransform
    }
}

private let vrDecompressionOutputCallback: VTDecompressionOutputCallback = {
    _, sourceFrameRefCon, status, _, imageBuffer, _, _ in
    guard let sourceFrameRefCon else { return }
    let context = Unmanaged<VRDecompressionFrameContext>.fromOpaque(sourceFrameRefCon).takeUnretainedValue()
    context.outputStatus = status
    guard status == noErr, let imageBuffer else { return }
    context.didDecode = true
    VRVideoFrameStore.shared.publish(VRDecodedFrame(
        sequence: context.sequence,
        timestamp: context.timestamp,
        headTransform: context.headTransform,
        pixelBuffer: imageBuffer
    ), generation: context.generation)
}

/// Rebuilds Annex-B HEVC packets as CMSampleBuffers and decodes directly to Metal-compatible pixels.
final class VRVideoDecoder: ObservableObject {
    @Published private(set) var status = String(localized: "Waiting for HEVC video from the Mac.")
    @Published private(set) var decodedVideoFPS = 0
    @Published private(set) var decodeDiagnostics = String(localized: "0 frames decoded")
    // ASCII counters for the diagnostics sent to the Mac.
    @Published private(set) var diagnosticsSummary = "decoded=0 recoveries=0 gaps=0 late_drops=0"

    // Configure before starting the receiver; invoked from the decode queue.
    var requestKeyFrame: (() -> Void)?

    private let decodeQueue = DispatchQueue(label: "com.verantyx.vrvideo-decoder", qos: .userInitiated)
    private let pendingFrameLock = NSLock()
    private struct PendingFrame {
        let frame: VRCompressedFrame
        let receivedAt: TimeInterval
    }
    private var pendingFrames: [PendingFrame] = []
    private var pendingDiscontinuity = false
    private var droppedPendingFrames = 0
    private var hasDecodeWorker = false
    private let maxPendingFrames = 8
    private let maxPendingAge: TimeInterval = 0.1
    private var generation: UInt64
    private var uiGeneration: UInt64
    private var isActive = true
    private var streamGeneration: UInt64?
    private var lastSequence: UInt32?
    private var needsKeyFrame = true
    private var lastKeyFrameRequestAt: TimeInterval = 0
    private var decodedFrameCount = 0
    private var recoveryCount = 0
    private var gapCount = 0
    private var fpsWindowFrames = 0
    private var fpsWindowStart = ProcessInfo.processInfo.systemUptime
    private var lastDecodedAt: TimeInterval?
    private var diagnosticsTimer: DispatchSourceTimer?
    private var lastStatus = ""
    private var formatDescription: CMVideoFormatDescription?
    private var decompressionSession: VTDecompressionSession?
    private var vpsData: [UInt8]?
    private var spsData: [UInt8]?
    private var ppsData: [UInt8]?
    private var frameCounter: Int64 = 0
    private let timeScale: CMTimeScale = 90_000
    private let frameDuration = CMTime(value: 1_000, timescale: 90_000)

    init() {
        let generation = VRVideoFrameStore.shared.beginGeneration()
        self.generation = generation
        self.uiGeneration = generation
        let timer = DispatchSource.makeTimerSource(queue: decodeQueue)
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in self?.publishDiagnostics() }
        diagnosticsTimer = timer
        timer.resume()
    }

    func decodeFrame(_ frame: VRCompressedFrame) {
        pendingFrameLock.lock()
        let now = ProcessInfo.processInfo.systemUptime
        if pendingFrames.count >= maxPendingFrames ||
           pendingFrames.first.map({ now - $0.receivedAt > maxPendingAge }) == true {
            droppedPendingFrames += pendingFrames.count
            pendingFrames.removeAll(keepingCapacity: true)
            pendingDiscontinuity = true
        }
        pendingFrames.append(PendingFrame(frame: frame, receivedAt: now))
        let shouldStartWorker = !hasDecodeWorker
        if shouldStartWorker { hasDecodeWorker = true }
        pendingFrameLock.unlock()

        if shouldStartWorker {
            decodeQueue.async { [weak self] in
                self?.decodeNextPendingFrame()
            }
        }
    }

    private func decodeNextPendingFrame() {
        pendingFrameLock.lock()
        let now = ProcessInfo.processInfo.systemUptime
        if pendingFrames.first.map({ now - $0.receivedAt > maxPendingAge }) == true {
            droppedPendingFrames += pendingFrames.count
            pendingFrames.removeAll(keepingCapacity: true)
            pendingDiscontinuity = true
        }
        let pending = pendingFrames.isEmpty ? nil : pendingFrames.removeFirst()
        let discontinuity = pendingDiscontinuity
        pendingDiscontinuity = false
        pendingFrameLock.unlock()

        if isActive {
            if discontinuity { continueAfterGap(String(localized: "Decoding fell behind; old frames dropped. Requesting a keyframe.")) }
            if let pending { decodeFrameOnQueue(pending.frame) }
        }

        pendingFrameLock.lock()
        let hasAnotherFrame = !pendingFrames.isEmpty
        if !hasAnotherFrame { hasDecodeWorker = false }
        pendingFrameLock.unlock()

        if hasAnotherFrame {
            decodeQueue.async { [weak self] in
                self?.decodeNextPendingFrame()
            }
        }
    }

    private func discardPendingFrames() {
        pendingFrameLock.lock()
        pendingFrames.removeAll(keepingCapacity: true)
        pendingDiscontinuity = false
        pendingFrameLock.unlock()
    }

    func start() {
        let currentGeneration = decodeQueue.sync {
            discardPendingFrames()
            invalidateDecompressionSession()
            formatDescription = nil
            vpsData = nil
            spsData = nil
            ppsData = nil
            frameCounter = 0
            streamGeneration = nil
            lastSequence = nil
            needsKeyFrame = true
            decodedFrameCount = 0
            recoveryCount = 0
            gapCount = 0
            fpsWindowFrames = 0
            fpsWindowStart = ProcessInfo.processInfo.systemUptime
            lastDecodedAt = nil
            lastKeyFrameRequestAt = 0
            lastStatus = ""
            pendingFrameLock.lock()
            droppedPendingFrames = 0
            pendingFrameLock.unlock()
            isActive = true
            generation = VRVideoFrameStore.shared.beginGeneration()
            return generation
        }
        DispatchQueue.main.async { [weak self] in
            guard let self, currentGeneration >= self.uiGeneration else { return }
            self.uiGeneration = currentGeneration
            self.status = String(localized: "Waiting for HEVC video from the Mac.")
            self.decodedVideoFPS = 0
            self.decodeDiagnostics = String(localized: "0 frames decoded")
        }
    }

    private func decodeFrameOnQueue(_ frame: VRCompressedFrame) {
        if streamGeneration != frame.streamGeneration {
            // An encoder restart can reset its sequence and parameter sets.
            // Reconnects must also reset the renderer's ordering generation.
            streamGeneration = frame.streamGeneration
            lastSequence = nil
            invalidateDecompressionSession()
            formatDescription = nil
            vpsData = nil
            spsData = nil
            ppsData = nil
            needsKeyFrame = true
            generation = VRVideoFrameStore.shared.beginGeneration()
            let currentGeneration = generation
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.uiGeneration = max(self.uiGeneration, currentGeneration)
            }
            updateStatus(String(localized: "Reconnected to the Mac. Waiting for an HEVC keyframe."))
        }
        if let lastSequence {
            guard Int32(bitPattern: frame.sequence &- lastSequence) > 0 else { return }
            if frame.sequence != lastSequence &+ 1 {
                continueAfterGap(String(localized: "Lost video packets. Requesting a keyframe."))
            }
        }
        lastSequence = frame.sequence
        let nals = extractNALUs(from: frame.data)
        var vclNALUs: [Data] = []
        var parameterSetsUpdated = false
        var isRandomAccess = false
        var hasPicture = false

        for nal in nals where nal.count >= 2 {
            let nalType = (nal[0] >> 1) & 0x3F
            switch nalType {
            case 32:
                if vpsData == nil || Array(nal) != vpsData! {
                    vpsData = Array(nal)
                    parameterSetsUpdated = true
                }
            case 33:
                if spsData == nil || Array(nal) != spsData! {
                    spsData = Array(nal)
                    parameterSetsUpdated = true
                }
            case 34:
                if ppsData == nil || Array(nal) != ppsData! {
                    ppsData = Array(nal)
                    parameterSetsUpdated = true
                }
            default:
                // VPS/SPS/PPS configure the session; all picture slices and
                // auxiliary SEI NALs remain in the same access unit.
                if nalType <= 31 {
                    hasPicture = true
                    isRandomAccess = isRandomAccess || (16...21).contains(nalType)
                }
                if nalType <= 31 || nalType == 39 || nalType == 40 { vclNALUs.append(nal) }
            }
        }

        if parameterSetsUpdated {
            invalidateDecompressionSession()
            formatDescription = nil
            needsKeyFrame = true
        }
        guard hasPicture else { return }
        guard !needsKeyFrame || isRandomAccess else {
            requestRecoveryFrame()
            return
        }
        guard vpsData != nil, spsData != nil, ppsData != nil else {
            updateStatus(String(localized: "Waiting for the HEVC keyframe and parameter sets."))
            requestRecoveryFrame()
            return
        }
        if decompressionSession == nil {
            createFormatDescription()
            createDecompressionSession()
        }
        guard let decompressionSession else {
            updateStatus(String(localized: "Could not initialize the HEVC decoder."))
            requestRecoveryFrame()
            return
        }
        guard !vclNALUs.isEmpty,
              let sampleBuffer = createSampleBuffer(from: vclNALUs)
        else { return }

        let contextGeneration = generation
        let context = VRDecompressionFrameContext(
            sequence: frame.sequence,
            generation: contextGeneration,
            timestamp: frame.timestamp,
            headTransform: frame.headTransform
        )
        let frameRefCon = Unmanaged.passUnretained(context).toOpaque()
        var infoFlags = VTDecodeInfoFlags()
        let decodeStatus = VTDecompressionSessionDecodeFrame(
            decompressionSession,
            sampleBuffer: sampleBuffer,
            // Synchronous on this dedicated queue bounds VideoToolbox's work
            // as well as our FIFO. Do not discard HEVC reference frames merely
            // because a newer compressed frame arrived during decoding.
            flags: [],
            frameRefcon: frameRefCon,
            infoFlagsOut: &infoFlags
        )
        if decodeStatus != noErr || context.outputStatus != noErr || !context.didDecode {
            let failure = decodeStatus != noErr ? decodeStatus : context.outputStatus
            print("[VRVideoDecoder] Decode failed/dropped: status=\(failure), seq=\(frame.sequence)")
            requireKeyFrame(String(localized: "Restarting video decoding (\(Int(failure)))."))
        } else {
            needsKeyFrame = false
            decodedFrameCount += 1
            fpsWindowFrames += 1
            lastDecodedAt = ProcessInfo.processInfo.systemUptime
            updateStatus(String(localized: "Decoding HEVC video."))
        }
    }

    /// A lost frame only damages the frames that refer to it. Keep decoding
    /// (brief artifacts) and ask for a keyframe at once. Stopping until the
    /// next keyframe froze the picture every time Wi-Fi dropped one packet.
    private func continueAfterGap(_ message: String) {
        gapCount += 1
        updateStatus(message)
        requestRecoveryFrame()
    }

    private func requireKeyFrame(_ message: String) {
        if !needsKeyFrame { recoveryCount += 1 }
        needsKeyFrame = true
        invalidateDecompressionSession()
        updateStatus(message)
        requestRecoveryFrame()
    }

    private func requestRecoveryFrame() {
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastKeyFrameRequestAt >= 0.2 else { return }
        lastKeyFrameRequestAt = now
        requestKeyFrame?()
    }

    private func publishDiagnostics() {
        guard isActive else { return }
        let now = ProcessInfo.processInfo.systemUptime
        let fps = Int((Double(fpsWindowFrames) / max(0.001, now - fpsWindowStart)).rounded())
        fpsWindowFrames = 0
        fpsWindowStart = now
        pendingFrameLock.lock()
        let dropped = droppedPendingFrames
        let queued = pendingFrames.count
        pendingFrameLock.unlock()
        let text = String(localized: "\(decodedFrameCount) decoded / \(gapCount) gaps / \(recoveryCount) recoveries / \(queued) queued / \(dropped) dropped late")
        let summary = "decoded=\(decodedFrameCount) gaps=\(gapCount) recoveries=\(recoveryCount) late_drops=\(dropped)"
        let currentGeneration = generation
        DispatchQueue.main.async { [weak self] in
            guard let self, self.uiGeneration == currentGeneration else { return }
            self.decodedVideoFPS = fps
            self.decodeDiagnostics = text
            self.diagnosticsSummary = summary
        }
        if needsKeyFrame { requestRecoveryFrame() }
        if let lastDecodedAt, now - lastDecodedAt > 3 {
            updateStatus(String(localized: "Video decoding stalled. Requesting a keyframe."))
            requestRecoveryFrame()
        }
    }

    func stop() {
        let currentGeneration = decodeQueue.sync {
            discardPendingFrames()
            isActive = false
            invalidateDecompressionSession()
            formatDescription = nil
            vpsData = nil
            spsData = nil
            ppsData = nil
            frameCounter = 0
            streamGeneration = nil
            lastSequence = nil
            needsKeyFrame = true
            generation = VRVideoFrameStore.shared.beginGeneration()
            return generation
        }
        DispatchQueue.main.async { [weak self] in
            guard let self, currentGeneration >= self.uiGeneration else { return }
            self.uiGeneration = currentGeneration
            self.status = String(localized: "Stopped")
        }
    }

    deinit {
        diagnosticsTimer?.cancel()
        invalidateDecompressionSession()
    }

    private func invalidateDecompressionSession() {
        guard let decompressionSession else { return }
        VTDecompressionSessionInvalidate(decompressionSession)
        self.decompressionSession = nil
    }

    private func updateStatus(_ value: String, generation requestedGeneration: UInt64? = nil) {
        guard value != lastStatus else { return }
        lastStatus = value
        let currentGeneration = requestedGeneration ?? generation
        DispatchQueue.main.async { [weak self] in
            guard let self, self.uiGeneration == currentGeneration else { return }
            self.status = value
        }
    }

    private func extractNALUs(from data: Data) -> [Data] {
        var nals: [Data] = []
        let bytes = [UInt8](data)
        var starts: [Int] = []
        var index = 0
        while index + 3 < bytes.count {
            if bytes[index] == 0, bytes[index + 1] == 0,
               bytes[index + 2] == 0, bytes[index + 3] == 1 {
                starts.append(index)
                index += 4
            } else if bytes[index] == 0, bytes[index + 1] == 0,
                      bytes[index + 2] == 1 {
                starts.append(index)
                index += 3
            } else {
                index += 1
            }
        }

        for startIndex in starts.indices {
            let start = starts[startIndex]
            let prefixLength = bytes[start + 2] == 1 ? 3 : 4
            let payloadStart = start + prefixLength
            let end = startIndex + 1 < starts.count ? starts[startIndex + 1] : bytes.count
            if payloadStart < end {
                nals.append(Data(bytes[payloadStart..<end]))
            }
        }
        return nals
    }

    private func createFormatDescription() {
        guard let vps = vpsData, let sps = spsData, let pps = ppsData else { return }
        formatDescription = nil

        vps.withUnsafeBufferPointer { vpsBuffer in
            sps.withUnsafeBufferPointer { spsBuffer in
                pps.withUnsafeBufferPointer { ppsBuffer in
                    let pointers: [UnsafePointer<UInt8>] = [
                        vpsBuffer.baseAddress!, spsBuffer.baseAddress!, ppsBuffer.baseAddress!
                    ]
                    let sizes = [vps.count, sps.count, pps.count]
                    let status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: 3,
                        parameterSetPointers: pointers,
                        parameterSetSizes: sizes,
                        nalUnitHeaderLength: 4,
                        extensions: nil,
                        formatDescriptionOut: &formatDescription
                    )
                    if status != noErr {
                        print("[VRVideoDecoder] CMVideoFormatDescriptionCreateFromHEVCParameterSets failed: \(status)")
                        formatDescription = nil
                    }
                }
            }
        }
    }

    private func createDecompressionSession() {
        guard let formatDescription else { return }
        let attributes: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferMetalCompatibilityKey: true,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary
        ]
        var callback = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: vrDecompressionOutputCallback,
            decompressionOutputRefCon: nil
        )
        let status = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            formatDescription: formatDescription,
            decoderSpecification: nil,
            imageBufferAttributes: attributes as CFDictionary,
            outputCallback: &callback,
            decompressionSessionOut: &decompressionSession
        )
        if status != noErr {
            print("[VRVideoDecoder] VTDecompressionSessionCreate failed: \(status)")
            decompressionSession = nil
        } else if let decompressionSession {
            // Ask VideoToolbox to schedule decoding ahead of background work.
            VTSessionSetProperty(decompressionSession, key: kVTDecompressionPropertyKey_RealTime,
                                 value: kCFBooleanTrue)
        }
    }

    private func createSampleBuffer(from nals: [Data]) -> CMSampleBuffer? {
        guard let formatDescription else { return nil }
        var avccData = Data()
        for nal in nals {
            var length = UInt32(nal.count).bigEndian
            withUnsafeBytes(of: &length) { avccData.append(contentsOf: $0) }
            avccData.append(nal)
        }

        var blockBuffer: CMBlockBuffer?
        let createStatus = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: avccData.count,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: avccData.count,
            flags: 0,
            blockBufferOut: &blockBuffer
        )
        guard createStatus == noErr, let blockBuffer else {
            print("[VRVideoDecoder] CMBlockBufferCreateWithMemoryBlock failed: \(createStatus)")
            return nil
        }

        let copyStatus = avccData.withUnsafeBytes { bytes -> OSStatus in
            guard let baseAddress = bytes.baseAddress else { return kCMBlockBufferBadCustomBlockSourceErr }
            return CMBlockBufferReplaceDataBytes(
                with: baseAddress,
                blockBuffer: blockBuffer,
                offsetIntoDestination: 0,
                dataLength: avccData.count
            )
        }
        guard copyStatus == noErr else {
            print("[VRVideoDecoder] CMBlockBufferReplaceDataBytes failed: \(copyStatus)")
            return nil
        }

        frameCounter += 1
        var timing = CMSampleTimingInfo(
            duration: frameDuration,
            presentationTimeStamp: CMTime(value: frameCounter * 1_000, timescale: timeScale),
            decodeTimeStamp: .invalid
        )
        var sampleSize = avccData.count
        var sampleBuffer: CMSampleBuffer?
        let sampleStatus = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: blockBuffer,
            formatDescription: formatDescription,
            sampleCount: 1,
            sampleTimingEntryCount: 1,
            sampleTimingArray: &timing,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer
        )
        guard sampleStatus == noErr else {
            print("[VRVideoDecoder] CMSampleBufferCreateReady failed: \(sampleStatus)")
            return nil
        }
        return sampleBuffer
    }
}
