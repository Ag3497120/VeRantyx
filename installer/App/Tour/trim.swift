import AVFoundation

// trim <in.mov> <out.mp4> <seconds>: the recording's first <seconds> as H.264 MP4, for sharing.
let args = CommandLine.arguments
guard args.count == 4, let seconds = Double(args[3]) else {
    print("usage: trim <in.mov> <out.mp4> <seconds>")
    exit(2)
}
let asset = AVURLAsset(url: URL(fileURLWithPath: args[1]))
let out = URL(fileURLWithPath: args[2])
try? FileManager.default.removeItem(at: out)
guard let export = AVAssetExportSession(asset: asset, presetName: AVAssetExportPreset1920x1080) else {
    print("trim: can't export")
    exit(1)
}
export.outputURL = out
export.outputFileType = .mp4
export.timeRange = CMTimeRange(start: .zero, duration: CMTime(seconds: seconds, preferredTimescale: 600))
let done = DispatchSemaphore(value: 0)
export.exportAsynchronously { done.signal() }
done.wait()
print("trim: status \(export.status.rawValue) \(export.error.map { "\($0)" } ?? "")")
exit(export.status == .completed ? 0 : 1)
