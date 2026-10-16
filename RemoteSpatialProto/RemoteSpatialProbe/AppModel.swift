import Foundation
import QuartzCore
import SwiftUI

/// App-wide UI state (main actor), as in Apple's Spatial Rendering App template.
@MainActor
@Observable
final class AppModel {
    let immersiveSpaceID = "RemoteSpatialProbe"

    enum ImmersiveSpaceState: String {
        case closed, inTransition, open
    }
    var immersiveSpaceState = ImmersiveSpaceState.closed
    var lastOpenResult = "—"

    /// Latest numbers from the render thread (pushed a few times a second).
    var stats = ProbeStats()
    /// What the layer reported when it was created.
    var capabilities: [String] = []

    let control = ProbeControl()
}

/// A snapshot of the render loop's counters, sent to the UI.
struct ProbeStats: Sendable {
    var framesRendered = 0
    var framesLate = 0               // GPU finished after renderingDeadline
    var framesSkipped = 0            // gaps in drawable.presentationFrameIndex
    var framesWithoutDrawable = 0    // queryDrawables() returned none
    var inputToPresentMs = 0.0       // t_input -> predicted presentation, median of recent frames
    var queryToPresentMs = 0.0       // t_query -> predicted presentation, median
    var gpuMs = 0.0                  // GPU time, median
    var frameIntervalMs = 0.0        // presentation-time step, median
    var anchorFraction = 0.0         // frames with a device anchor / frames
    var anchorTracked = false
    var head = SIMD3<Float>(0, 0, 0)
    var spatialEvents = 0
    var lastSpatialEvent = "none"
    var lastFlash = "none"
    var layerState = "—"
    var arState = "—"
}

/// Settings and requests the UI hands to the render thread. Lock-protected:
/// the render loop runs on its own thread.
final class ProbeControl: @unchecked Sendable {
    private let lock = NSLock()
    private var pendingFlashPress: CFTimeInterval?
    private var _autoFlash = false
    private var _poseDelayMs = 0.0
    private var lastAutoFlash: CFTimeInterval = 0

    /// Space bar / Flash button: the next frame that starts encoding shows the flash.
    func requestFlash() {
        let now = CACurrentMediaTime()
        lock.withLock { pendingFlashPress = now }
        ProbeLog.shared.note("flash pressed t_press=\(now.s6)")
    }

    /// Called once per frame by the renderer. Returns the press time if this frame flashes.
    func takeFlash(now: CFTimeInterval) -> CFTimeInterval? {
        lock.withLock {
            if let press = pendingFlashPress {
                pendingFlashPress = nil
                return press
            }
            if _autoFlash, now - lastAutoFlash >= 1.0 {
                lastAutoFlash = now
                return now
            }
            return nil
        }
    }

    var autoFlash: Bool {
        get { lock.withLock { _autoFlash } }
        set { lock.withLock { _autoFlash = newValue } }
    }

    // Pinch input from LayerRenderer.onSpatialEvent. On macOS this is the only
    // hand data the SDK offers (HandTrackingProvider is unavailable there):
    // the pinching hand's pose while a pinch is active.
    private var _pinches: [SIMD3<Float>] = []
    var pinches: [SIMD3<Float>] {
        get { lock.withLock { _pinches } }
        set { lock.withLock { _pinches = newValue } }
    }

    /// Render with a head pose this much older than the predicted display time.
    /// If the compositor reprojects with drawable.deviceAnchor, the world stays
    /// still anyway; if it does not, the world swims when you turn your head.
    var poseDelayMs: Double {
        get { lock.withLock { _poseDelayMs } }
        set { lock.withLock { _poseDelayMs = newValue } }
    }
}
