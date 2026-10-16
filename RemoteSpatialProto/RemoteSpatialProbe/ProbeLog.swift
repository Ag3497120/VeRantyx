import Foundation
import QuartzCore

/// Writes the probe's logs to ~/Library/Logs/RemoteSpatialProbe/:
/// - frames-<date>.csv   one row per rendered frame (written when the GPU finishes it)
/// - session-<date>.txt  capabilities, ARKit events, flashes, spatial events
/// All writes go through one serial queue, so any thread may call in.
final class ProbeLog: @unchecked Sendable {
    static let shared = ProbeLog()

    let directory: URL
    let framesURL: URL
    let sessionURL: URL
    private let queue = DispatchQueue(label: "ProbeLog", qos: .utility)
    private var frames: FileHandle?
    private var session: FileHandle?

    static let csvHeader = [
        "frame_index",               // LayerRenderer.Frame.frameIndex
        "t_query",                   // CACurrentMediaTime() right after queryNextFrame() returned
        "t_input",                   // CACurrentMediaTime() after waiting until optimalInputTime
        "optimal_input_time",        // predictTiming().optimalInputTime (s, mach clock)
        "rendering_deadline",        // predictTiming().renderingDeadline
        "predicted_presentation",    // predictTiming().presentationTime
        "trackable_anchor_time",     // predictTiming().trackableAnchorTime
        "drawable_presentation",     // drawable.frameTiming.presentationTime (after queryDrawables)
        "presentation_frame_index",  // drawable.presentationFrameIndex (compositor frame index)
        "drawables",                 // queryDrawables().count
        "pose_query_time",           // timestamp passed to queryDeviceAnchor
        "pose_delay_ms",             // artificial pose age (reprojection test), 0 normally
        "anchor_present",
        "anchor_tracked",
        "anchor_timestamp",          // DeviceAnchor.timestamp
        "head_x", "head_y", "head_z",
        "t_commit",                  // CACurrentMediaTime() just before commandBuffer.commit()
        "t_gpu_done",                // CACurrentMediaTime() in the command buffer's completed handler
        "gpu_ms",                    // commandBuffer.gpuEndTime - gpuStartTime
        "late",                      // t_gpu_done > rendering_deadline
        "flash",                     // this frame draws the white flash square
        "frame_repeat_count",        // layerRenderer.minimumFrameRepeatCount
    ].joined(separator: ",")

    private init() {
        let fm = FileManager.default
        directory = fm.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Logs/RemoteSpatialProbe", isDirectory: true)
        try? fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let stamp = Self.fileStamp()
        framesURL = directory.appendingPathComponent("frames-\(stamp).csv")
        sessionURL = directory.appendingPathComponent("session-\(stamp).txt")
    }

    private static func fileStamp() -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd_HH-mm-ss"
        return f.string(from: Date())
    }

    private static func open(_ url: URL, header: String?) -> FileHandle? {
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: header.map { Data(($0 + "\n").utf8) })
        }
        let handle = try? FileHandle(forWritingTo: url)
        _ = try? handle?.seekToEnd()
        return handle
    }

    /// A line in the session log, prefixed with wall-clock and CACurrentMediaTime.
    func note(_ text: String) {
        let now = CACurrentMediaTime()
        let wall = ISO8601DateFormatter.string(from: Date(), timeZone: .current,
                                               formatOptions: [.withInternetDateTime, .withFractionalSeconds])
        let line = "\(wall) t=\(String(format: "%.6f", now)) \(text)\n"
        queue.async {
            if self.session == nil { self.session = Self.open(self.sessionURL, header: nil) }
            self.session?.write(Data(line.utf8))
        }
        print("[probe] \(text)")
    }

    func frameRow(_ fields: [String]) {
        let line = fields.joined(separator: ",") + "\n"
        queue.async {
            if self.frames == nil { self.frames = Self.open(self.framesURL, header: Self.csvHeader) }
            self.frames?.write(Data(line.utf8))
        }
    }

    func flush() {
        queue.sync {
            try? frames?.synchronize()
            try? session?.synchronize()
        }
    }
}

extension Double {
    /// Fixed 6-decimal seconds for the CSV (microsecond resolution).
    var s6: String { String(format: "%.6f", self) }
    var ms3: String { String(format: "%.3f", self) }
}
