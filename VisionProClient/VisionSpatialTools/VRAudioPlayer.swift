import AVFoundation
import Foundation

/// Plays the Mac's game audio ("AUDI" PCM datagrams) through the headset.
/// Packets are scheduled on an AVAudioPlayerNode as they arrive; a small
/// backlog limit keeps the sound close to the picture.
final class VRAudioPlayer {
    static let shared = VRAudioPlayer()

    private let queue = DispatchQueue(label: "com.verantyx.vr-audio", qos: .userInteractive)
    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private var format: AVAudioFormat?
    private var scheduledFrames = 0
    private var droppedPackets = 0
    private var receivedPackets = 0
    private var concealedPackets = 0
    private var underruns = 0
    private var started = false
    private var playing = false
    private var expectedSequence: UInt32?
    // Wi-Fi delivers the 5 ms packets in bursts around the video frames.
    // Hold this much before (re)starting playback so bursts do not empty the
    // player, and drop audio only when far more than that has piled up.
    // 150 ms: Wi-Fi gaps of 100 ms were common in play and emptied an 80 ms
    // buffer, which sounded choppy. Latency is not critical for game audio.
    private let prebufferSeconds = 0.15
    private let maximumBacklogSeconds = 0.5

    func enqueue(_ data: Data) {
        queue.async { [self] in
            guard data.count >= 20 else { return }
            let sampleRate = Double(readUInt32LE(data, at: 8))
            let channels = Int(readUInt32LE(data, at: 12))
            let frames = Int(readUInt32LE(data, at: 16))
            guard sampleRate > 0, channels == 1 || channels == 2, frames > 0,
                  data.count == 20 + frames * channels * 2 else { return }
            receivedPackets += 1
            let sequence = readUInt32LE(data, at: 4)
            ensureEngine(sampleRate: sampleRate, channels: channels)
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
            data.withUnsafeBytes { raw in
                let samples = raw.baseAddress!.advanced(by: 20).assumingMemoryBound(to: Int16.self)
                for channel in 0..<channels {
                    let out = floatData[channel]
                    for frame in 0..<frames {
                        out[frame] = Float(Int16(littleEndian: samples[frame * channels + channel])) / 32768
                    }
                }
            }
        } else {
            for channel in 0..<channels { floatData[channel].update(repeating: 0, count: frames) }
        }
        scheduledFrames += frames
        player.scheduleBuffer(buffer) { [weak self] in
            self?.queue.async { [weak self] in
                guard let self else { return }
                scheduledFrames -= frames
                if scheduledFrames <= 0, playing {
                    // Ran dry: pause and let the prebuffer fill again.
                    playing = false
                    underruns += 1
                    player.pause()
                }
            }
        }
        if !playing, scheduledFrames >= Int(sampleRate * prebufferSeconds) {
            playing = true
            player.play()
        }
    }

    func stop() {
        queue.async { [self] in
            player.stop()
            engine.stop()
            started = false
            playing = false
            format = nil
            scheduledFrames = 0
            expectedSequence = nil
        }
    }

    var status: String {
        queue.sync { started ? String(localized: "received \(receivedPackets) / concealed \(concealedPackets) / underruns \(underruns) / dropped \(droppedPackets)") : String(localized: "waiting for audio") }
    }

    private func ensureEngine(sampleRate: Double, channels: Int) {
        if let format, format.sampleRate == sampleRate, Int(format.channelCount) == channels, started { return }
        player.stop()
        engine.stop()
        engine.detach(player)
        guard let newFormat = AVAudioFormat(standardFormatWithSampleRate: sampleRate,
                                            channels: AVAudioChannelCount(channels)) else { return }
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            try session.setPreferredIOBufferDuration(0.005)
            try session.setActive(true)
            engine.attach(player)
            engine.connect(player, to: engine.mainMixerNode, format: newFormat)
            try engine.start()
            format = newFormat
            started = true
            playing = false
            scheduledFrames = 0
            expectedSequence = nil
            print("VR audio: playing \(Int(sampleRate)) Hz \(channels)ch from the Mac")
        } catch {
            print("VR audio: could not start playback: \(error)")
            started = false
        }
    }

    private func readUInt32LE(_ data: Data, at offset: Int) -> UInt32 {
        UInt32(data[offset]) | (UInt32(data[offset + 1]) << 8)
            | (UInt32(data[offset + 2]) << 16) | (UInt32(data[offset + 3]) << 24)
    }
}
