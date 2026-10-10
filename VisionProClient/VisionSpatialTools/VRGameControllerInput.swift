import Foundation
import GameController
import simd

/// One hand's Oculus Touch inputs read from a game controller.
/// Joy-Con inertial sensors, in the controller's own axes (g and rad/s).
struct VRControllerMotion {
    var acceleration = SIMD3<Float>.zero   // total acceleration (gravity + motion)
    var userAcceleration: SIMD3<Float>?     // motion only, when the controller separates it
    var gravity: SIMD3<Float>?
    var rotationRate: SIMD3<Float>?
    var timestamp: TimeInterval = 0
}

struct VRControllerHand {
    var trigger: Float = 0
    var grip: Float = 0
    var stick = SIMD2<Float>.zero
    var stickClick = false
    var primary = false   // Touch A / X
    var secondary = false // Touch B / Y
    var menu = false
    var motion: VRControllerMotion?
}

/// Reads a pair of Joy-Con combined into one controller (or any other extended
/// gamepad) and splits it into the left and right hand. Hand tracking still
/// supplies where each hand is; the controller supplies buttons, sticks and
/// triggers, which keep working when a hand leaves the cameras' view.
///
/// Joy-Con layout, held one per hand:
/// - ZL / ZR: trigger, L / R: grip, sticks: joysticks (press: stick click)
/// - right A / B: Touch A, right X / Y: Touch B
/// - left ↓ / ←: Touch X, left ↑ / →: Touch Y
/// - − / +: menu (left system button)
final class VRGameControllerInput: @unchecked Sendable {
    struct Snapshot {
        var left: VRControllerHand?
        var right: VRControllerHand?
        var status: String
    }

    private let lock = NSLock()
    private var state = Snapshot(left: nil, right: nil, status: String(localized: "Not connected (hand gestures control the game)"))
    private let queue = DispatchQueue(label: "com.verantyx.game-controller", qos: .userInteractive)
    private var observers: [NSObjectProtocol] = []
    private weak var activeController: GCController?
    private var motionDescription = String(localized: "none")
    private var latestMotion: VRControllerMotion?

    /// A merged Joy-Con pair exposes one motion source. It is applied to the
    /// right hand, which holds the gun in Alyx.
    private func updateMotion(from motion: GCMotion) {
        var sample = VRControllerMotion()
        sample.acceleration = SIMD3<Float>(Float(motion.acceleration.x), Float(motion.acceleration.y), Float(motion.acceleration.z))
        if motion.hasGravityAndUserAcceleration {
            sample.userAcceleration = SIMD3<Float>(Float(motion.userAcceleration.x), Float(motion.userAcceleration.y), Float(motion.userAcceleration.z))
            sample.gravity = SIMD3<Float>(Float(motion.gravity.x), Float(motion.gravity.y), Float(motion.gravity.z))
        }
        if motion.hasRotationRate {
            sample.rotationRate = SIMD3<Float>(Float(motion.rotationRate.x), Float(motion.rotationRate.y), Float(motion.rotationRate.z))
        }
        sample.timestamp = ProcessInfo.processInfo.systemUptime
        lock.lock()
        latestMotion = sample
        state.right?.motion = sample
        lock.unlock()
    }

    init() {
        let center = NotificationCenter.default
        for name in [Notification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.refresh() }
            })
        }
        Task { @MainActor [weak self] in self?.refresh() }
    }

    deinit {
        observers.forEach(NotificationCenter.default.removeObserver)
    }

    func snapshot() -> Snapshot {
        lock.lock()
        defer { lock.unlock() }
        return state
    }

    @MainActor
    private static func name(of controller: GCController) -> String {
        controller.vendorName ?? controller.productCategory
    }

    @MainActor
    private static func isSingleJoyCon(_ controller: GCController) -> Bool {
        let name = name(of: controller) + " " + controller.productCategory
        return name.contains("Joy-Con") && !name.contains("L/R") && !name.contains("L / R")
    }

    @MainActor
    private func refresh() {
        let controllers = GCController.controllers()
        let gamepads = controllers.filter { $0.extendedGamepad != nil && !Self.isSingleJoyCon($0) }
        let chosen = gamepads.first { Self.name(of: $0).contains("Joy-Con") } ?? gamepads.first

        if chosen !== activeController {
            activeController?.extendedGamepad?.valueChangedHandler = nil
            activeController = chosen
            if let chosen, let gamepad = chosen.extendedGamepad {
                chosen.handlerQueue = queue
                gamepad.valueChangedHandler = { [weak self] gamepad, _ in
                    self?.update(from: gamepad)
                }
                if let motion = chosen.motion {
                    if motion.sensorsRequireManualActivation { motion.sensorsActive = true }
                    motionDescription = String(localized: "gyro \(motion.hasRotationRate ? "yes" : "no") / accelerometer \(motion.hasGravityAndUserAcceleration ? "separated" : "combined")")
                    motion.valueChangedHandler = { [weak self] motion in
                        self?.updateMotion(from: motion)
                    }
                } else {
                    motionDescription = String(localized: "none")
                }
                print("Game controller in use: \(Self.name(of: chosen)) (\(chosen.productCategory)), motion: \(motionDescription)")
            }
        }

        if let chosen, let gamepad = chosen.extendedGamepad {
            update(from: gamepad)
            setStatus(String(localized: "Using \(Self.name(of: chosen)) (position from hand tracking, motion sensors: \(motionDescription))"))
        } else {
            let singles = controllers.filter { Self.isSingleJoyCon($0) }
            let status = singles.isEmpty
                ? String(localized: "Not connected (hand gestures control the game)")
                : String(localized: "Joy-Con are connected separately. Press L and R on both at the same time to merge them into one controller.")
            lock.lock()
            state = Snapshot(left: nil, right: nil, status: status)
            lock.unlock()
        }
    }

    private func setStatus(_ status: String) {
        lock.lock()
        state.status = status
        lock.unlock()
    }

    private func update(from gamepad: GCExtendedGamepad) {
        func stick(_ pad: GCControllerDirectionPad) -> SIMD2<Float> {
            let value = SIMD2<Float>(pad.xAxis.value, pad.yAxis.value)
            return simd_length(value) < 0.08 ? .zero : simd_clamp(value, SIMD2<Float>(-1, -1), SIMD2<Float>(1, 1))
        }
        let menu = gamepad.buttonMenu.isPressed || gamepad.buttonOptions?.isPressed == true

        var left = VRControllerHand()
        left.trigger = gamepad.leftTrigger.value
        left.grip = gamepad.leftShoulder.value
        left.stick = stick(gamepad.leftThumbstick)
        left.stickClick = gamepad.leftThumbstickButton?.isPressed == true
        left.primary = gamepad.dpad.down.isPressed || gamepad.dpad.left.isPressed
        left.secondary = gamepad.dpad.up.isPressed || gamepad.dpad.right.isPressed
        left.menu = menu

        // GameController names face buttons by position or by label depending
        // on the controller; either way A/B are the right and bottom buttons
        // and X/Y the top and left ones.
        var right = VRControllerHand()
        right.trigger = gamepad.rightTrigger.value
        right.grip = gamepad.rightShoulder.value
        right.stick = stick(gamepad.rightThumbstick)
        right.stickClick = gamepad.rightThumbstickButton?.isPressed == true
        right.primary = gamepad.buttonA.isPressed || gamepad.buttonB.isPressed
        right.secondary = gamepad.buttonX.isPressed || gamepad.buttonY.isPressed

        right.motion = latestMotion
        lock.lock()
        state.left = left
        state.right = right
        lock.unlock()
    }
}
