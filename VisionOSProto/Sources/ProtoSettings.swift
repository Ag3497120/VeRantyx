import Foundation
import os

/// How the low, fixed game rate is produced.
enum PacingMode: String, CaseIterable, Identifiable, Sendable {
    /// The heavy scene renders straight into the compositor drawable and the
    /// layer's `minimumFrameRepeatCount` makes visionOS show each frame
    /// (repeat + 1) times, reprojecting every repeat with the frame's depth.
    /// The rate is the display rate divided by an integer (90 Hz: 45, 30, 22.5, 18).
    case frameRepeat = "repeat"
    /// The layout the standalone game app will use: the "game" renders at its
    /// own exact rate into offscreen textures (MetalFX spatial upscale), and a
    /// cheap present pass copies the newest finished frame, with its depth and
    /// the head pose it was rendered from, into every 90 Hz drawable.
    case hold = "hold"

    var id: String { rawValue }
    var label: String { self == .frameRepeat ? "Frame repeat (system)" : "Hold (game-style, exact rate)" }
}

/// Settings shared by the window and the render thread. Launch arguments
/// (`-fps 22.5 -mode hold -load 48 -depth NO -foveation NO -scale 0.6 -autoImmersive YES`)
/// land in UserDefaults' argument domain; `VXPROTO_FPS` / `VXPROTO_MODE` in the
/// environment override them.
struct ProtoSettings: Sendable, Equatable {
    var fps: Double = 22.5
    var mode: PacingMode = .frameRepeat
    /// Fragment shader iterations: the GPU-load knob for the synthetic scene.
    var load: Int = 24
    /// When false the scene writes far depth everywhere, so the compositor can
    /// only correct rotation (what an app without depth gets).
    var providesDepth: Bool = true
    /// Read when the immersive space opens (a layer configuration setting).
    var foveation: Bool = true
    /// Offscreen render scale for hold mode (MetalFX spatial upscales to 1.0).
    var renderScale: Double = 0.7
    /// Objects orbiting the viewer: their motion shows the game rate.
    var animate: Bool = true

    static func fromLaunch() -> ProtoSettings {
        var s = ProtoSettings()
        let d = UserDefaults.standard
        let env = ProcessInfo.processInfo.environment
        if let v = env["VXPROTO_FPS"].flatMap(Double.init) { s.fps = v }
        else if d.object(forKey: "fps") != nil { s.fps = d.double(forKey: "fps") }
        if let m = (env["VXPROTO_MODE"] ?? d.string(forKey: "mode")).flatMap(PacingMode.init(rawValue:)) { s.mode = m }
        if d.object(forKey: "load") != nil { s.load = max(1, d.integer(forKey: "load")) }
        if d.object(forKey: "depth") != nil { s.providesDepth = d.bool(forKey: "depth") }
        if d.object(forKey: "foveation") != nil { s.foveation = d.bool(forKey: "foveation") }
        if d.object(forKey: "scale") != nil { s.renderScale = min(1, max(0.34, d.double(forKey: "scale"))) }
        if d.object(forKey: "animate") != nil { s.animate = d.bool(forKey: "animate") }
        if ![18.0, 20.0, 22.5, 30.0, 45.0, 90.0].contains(s.fps) { s.fps = min(90, max(10, s.fps)) }
        return s
    }
}

/// Lock-protected box the render thread reads every frame.
final class SettingsBox: @unchecked Sendable {
    private let lock = OSAllocatedUnfairLock(initialState: ProtoSettings())
    var value: ProtoSettings {
        get { lock.withLock { $0 } }
        set { lock.withLock { $0 = newValue } }
    }
}

/// What the render thread measured over the last report interval.
struct RenderStats: Sendable, Codable {
    var mode = ""
    var targetFps = 0.0
    var displayHz = 0.0          // compositor frames per second (presentation-time spacing)
    var appFrames = 0.0          // drawables this app submitted per second
    var gameFps = 0.0            // new heavy-scene frames per second
    var heavyGpuMs = 0.0         // GPU time of one heavy-scene frame
    var presentGpuMs = 0.0       // GPU time of the hold-mode present pass
    var lateFrames = 0           // drawables whose GPU work ended after the rendering deadline
    var repeatCount = 0
    var foveated = false
    var depth = true
    var logicalSize = ""         // per-eye logical size (rate map screen size)
    var physicalSize = ""        // per-eye texture size
    var availableMemoryMB = 0.0  // os_proc_available_memory
    var footprintMB = 0.0        // task phys_footprint
}
