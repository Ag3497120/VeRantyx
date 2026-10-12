import AVFoundation
import Foundation

/// Plays the Mac's game audio ("AUDI" PCM datagrams) through the headset.
/// Packets are scheduled on an AVAudioPlayerNode as they arrive; a small
/// backlog limit keeps the sound close to the picture.
final class VRAudioPlayer {
    static let shared = VRAudioPlayer()

    /// Receives one-line events for immersive-diagnostics.log (set by VRStreamerModel).
    var log: ((String) -> Void)?

    private let queue = DispatchQueue(label: "com.verantyx.vr-audio", qos: .userInteractive)
    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private var format: AVAudioFormat?
    private var scheduledFrames = 0
    private var droppedPackets = 0
    private var receivedPackets = 0
    private var playedPackets = 0
    private var concealedPackets = 0
    private var underruns = 0
    private var restarts = 0
    private var lastError = ""
    private var started = false
    private var playing = false
    private var expectedSequence: UInt32?
    // Buffers scheduled before a stop/restart still report completion
    // afterwards; their frames must not be subtracted from the new session.
    private var sessionGeneration = 0
    private var rateWindowStart = ProcessInfo.processInfo.systemUptime
    private var rateWindowPackets = 0
    private var lastFailureLogAt: TimeInterval = -.infinity
    private var observers: [NSObjectProtocol] = []
    // Wi-Fi delivers the 5 ms packets in bursts around the video frames.
    // Hold this much before (re)starting playback so bursts do not empty the
    // player, and drop audio only when far more than that has piled up.
    // 150 ms: Wi-Fi gaps of 100 ms were common in play and emptied an 80 ms
    // buffer, which sounded choppy. Latency is not critical for game audio.
    private let prebufferSeconds = 0.15
    private let maximumBacklogSeconds = 0.5

    private init() {
        // AVAudioEngine stops itself when the output route or format changes
        // (for example when the immersive space opens or the wearer takes the
        // headset off and on). Without a restart, play() is then silent and
        // every packet ends up dropped as backlog.
        let center = NotificationCenter.default
        observers.append(center.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine,
                                            queue: nil) { [weak self] _ in
            self?.queue.async { self?.handleEngineConfigurationChange() }
        })
        observers.append(center.addObserver(forName: AVAudioSession.interruptionNotification, object: nil,
                                            queue: nil) { [weak self] notification in
            let type = (notification.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt)
                .flatMap(AVAudioSession.InterruptionType.init(rawValue:))
            self?.queue.async { self?.handleInterruption(type) }
        })
    }

    func enqueue(_ data: Data) {
        queue.async { [self] in
            guard data.count >= 20 else { return }
            let sampleRate = Double(readUInt32LE(data, at: 8))
            let channels = Int(readUInt32LE(data, at: 12))
            let frames = Int(readUInt32LE(data, at: 16))
            guard sampleRate > 0, channels == 1 || channels == 2, frames > 0,
                  data.count == 20 + frames * channels * 2 else { return }
            receivedPackets += 1
            rateWindowPackets += 1
            let sequence = readUInt32LE(data, at: 4)
            ensureEngine(sampleRate: sampleRate, channels: channels)
            publishCountersIfDue()
            guard let format, started else { return }
            if scheduledFrames > Int(sampleRate * maximumBacklogSeconds) {
                droppedPackets += 1
                expectedSequence = sequence &+ 1
                return
            }
            // A lost packet leaves a hole; fill it with silence so the rest
            // keeps its timing instead of pulling the sound earlier.
            if let expected = expectedSequence {
                let missing = Int(Int32(bitPattern: sequence &- expected))
                if missing > 0 && missing <= 20 {
                    for _ in 0..<missing { schedule(frames: frames, format: format, fill: nil) }
                    concealedPackets += missing
                } else if missing < 0 {
                    return // Late duplicate or reordered packet; already covered.
                }
            }
            expectedSequence = sequence &+ 1
            schedule(frames: frames, format: format, fill: data)
        }
    }

    private func schedule(frames: Int, format: AVAudioFormat, fill data: Data?) {
        let channels = Int(format.channelCount)
        let sampleRate = format.sampleRate
        guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(frames)),
              let floatData = buffer.floatChannelData else { return }
        buffer.frameLength = AVAudioFrameCount(frames)
        if let data {
            // enqueue() checked data.count == 20 + frames * channels * 2.
            // Unaligned loads: the samples start 20 bytes into a buffer whose
            // own alignment is not guaranteed.
            data.withUnsafeBytes { raw in
                guard raw.count >= 20 + frames * channels * 2 else { return }
                for channel in 0..<channels {
                    let out = floatData[channel]
                    for frame in 0..<frames {
                        let offset = 20 + (frame * channels + channel) * 2
                        let sample = Int16(littleEndian: raw.loadUnaligned(fromByteOffset: offset, as: Int16.self))
                        out[frame] = Float(sample) / 32768
                    }
                }
            }
        } else {
            for channel in 0..<channels { floatData[channel].update(repeating: 0, count: frames) }
        }
        scheduledFrames += frames
        let generation = sessionGeneration
        let carriesSound = data != nil
        player.scheduleBuffer(buffer) { [weak self] in
            self?.queue.async { [weak self] in
                guard let self, generation == sessionGeneration else { return }
                scheduledFrames -= frames
                if carriesSound { playedPackets += 1 }
                if scheduledFrames <= 0, playing {
                    // Ran dry: pause and let the prebuffer fill again.
                    playing = false
                    underruns += 1
                    player.pause()
                }
            }
        }
        if !playing, scheduledFrames >= Int(sampleRate * prebufferSeconds) {
            guard engine.isRunning else {
                // The engine stopped underneath the player (route change);
                // restart it before the backlog overflows into drops.
                restartEngine(reason: "engine not running at play()")
                return
            }
            playing = true
            player.play()
        }
    }

    func stop() {
        queue.async { [self] in
            player.stop()
            engine.stop()
            sessionGeneration += 1
            started = false
            playing = false
            format = nil
            scheduledFrames = 0
            expectedSequence = nil
            publishCounters()
        }
    }

    var status: String {
        queue.sync {
            started
                ? String(localized: "received \(receivedPackets) / played \(playedPackets) / concealed \(concealedPackets) / underruns \(underruns) / dropped \(droppedPackets) / restarts \(restarts)")
                : (lastError.isEmpty ? String(localized: "waiting for audio") : String(localized: "audio failed: \(lastError)"))
        }
    }

    private func ensureEngine(sampleRate: Double, channels: Int) {
        if let format, format.sampleRate == sampleRate, Int(format.channelCount) == channels, started {
            if !engine.isRunning { restartEngine(reason: "engine stopped between packets") }
            return
        }
        startEngine(sampleRate: sampleRate, channels: channels, reason: "first packet")
    }

    private func restartEngine(reason: String) {
        guard let format else { return }
        // Bound the retry rate: a persistent failure must not spin on every packet.
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastFailureLogAt >= 1 else { return }
        restarts += 1
        startEngine(sampleRate: format.sampleRate, channels: Int(format.channelCount), reason: reason)
    }

    private func startEngine(sampleRate: Double, channels: Int, reason: String) {
        player.stop()
        engine.stop()
        sessionGeneration += 1
        playing = false
        scheduledFrames = 0
        expectedSequence = nil
        if engine.attachedNodes.contains(player) { engine.detach(player) }
        guard let newFormat = AVAudioFormat(standardFormatWithSampleRate: sampleRate,
                                            channels: AVAudioChannelCount(channels)) else { return }
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            // A short IO buffer is a preference only; not being granted it
            // must not keep the engine from starting.
            try? session.setPreferredIOBufferDuration(0.005)
            try session.setActive(true)
            engine.attach(player)
            engine.connect(player, to: engine.mainMixerNode, format: newFormat)
            try engine.start()
            format = newFormat
            started = true
            lastError = ""
            let output = engine.outputNode.outputFormat(forBus: 0)
            let message = String(format: "VR audio: engine started (%@): %d Hz %dch from the Mac, output %.0f Hz %dch, route %@, volume %.2f",
                                 reason, Int(sampleRate), channels, output.sampleRate, Int(output.channelCount),
                                 session.currentRoute.outputs.map { $0.portType.rawValue }.joined(separator: "+"),
                                 session.outputVolume)
            print(message)
            log?(message)
        } catch {
            started = false
            lastError = error.localizedDescription
            let now = ProcessInfo.processInfo.systemUptime
            if now - lastFailureLogAt >= 5 {
                lastFailureLogAt = now
                let message = "VR audio: could not start playback (\(reason)): \(error)"
                print(message)
                log?(message)
            }
        }
        publishCounters()
    }

    private func handleEngineConfigurationChange() {
        guard started else { return }
        let message = "VR audio: engine configuration changed; running=\(engine.isRunning); restarting."
        print(message)
        log?(message)
        restartEngine(reason: "configuration change")
    }

    private func handleInterruption(_ type: AVAudioSession.InterruptionType?) {
        guard let type else { return }
        switch type {
        case .began:
            log?("VR audio: session interrupted (began); playback paused.")
            playing = false
        case .ended:
            log?("VR audio: interruption ended; restarting the engine.")
            if started { restartEngine(reason: "interruption ended") }
        @unknown default:
            break
        }
    }

    private func publishCountersIfDue() {
        let now = ProcessInfo.processInfo.systemUptime
        let elapsed = now - rateWindowStart
        guard elapsed >= 1 else { return }
        let rate = Int((Double(rateWindowPackets) / elapsed).rounded())
        rateWindowPackets = 0
        rateWindowStart = now
        publishCounters(packetsPerSecond: rate)
    }

    private func publishCounters(packetsPerSecond: Int? = nil) {
        let running = started && engine.isRunning
        VRStreamStatsStore.shared.update { [self] stats in
            stats.audioReceived = receivedPackets
            stats.audioPlayed = playedPackets
            stats.audioDropped = droppedPackets
            stats.audioConcealed = concealedPackets
            stats.audioUnderruns = underruns
            stats.audioRestarts = restarts
            stats.audioEngineRunning = running
            stats.audioLastError = lastError
            if let packetsPerSecond { stats.audioPacketsPerSecond = packetsPerSecond }
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
}
