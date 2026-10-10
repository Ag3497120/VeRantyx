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
    /// The hand holds nothing: trigger and grip come from its pinch and
    /// finger-curl gestures while stick and buttons come from the controller
    /// held in the other hand (one-handed gamepad mode).
    var gestureTriggerGrip = false
    /// While the stick is deflected, the hand's pointing direction follows
    /// the stick: direction from the stick angle, distance (pitch) from how
    /// far it is pushed, so Alyx's teleport arc is aimed with the stick.
    var stickAimsTeleport = false
}

/// How a one-handed gamepad (Pro Controller or any non-Joy-Con pad) is held;
/// its stick axes are rotated to match. Stored in UserDefaults "gamepadHold".
enum VRGamepadHold: Int {
    case upright = 0
    case rotatedLeft = 1   // top edge toward the left: turned 90° counterclockwise
    case rotatedRight = 2  // top edge toward the right: turned 90° clockwise

    static var current: VRGamepadHold {
        guard UserDefaults.standard.object(forKey: "gamepadHold") != nil else { return .rotatedLeft }
        return VRGamepadHold(rawValue: UserDefaults.standard.integer(forKey: "gamepadHold")) ?? .rotatedLeft
    }

    /// Maps a stick read in the pad's own frame (x toward its right edge,
    /// y toward its top edge) to the player's frame (x right, y forward).
    func rotate(_ stick: SIMD2<Float>) -> SIMD2<Float> {
        switch self {
        case .upright: return stick
        case .rotatedLeft: return SIMD2<Float>(-stick.y, stick.x)   // top edge left: +x points up
        case .rotatedRight: return SIMD2<Float>(stick.y, -stick.x)  // top edge right: +y points right
        }
    }
}

/// Reads Joy-Con (or any other extended gamepad) and splits them into the
/// left and right hand. Hand tracking still supplies where each hand is; the
/// controller supplies buttons, sticks, triggers and inertial sensors, which
/// keep working when a hand leaves the cameras' view.
///
/// Two ways to connect Joy-Con:
/// - Separately (recommended): each Joy-Con is its own controller with its
///   own gyro and accelerometer, so both hands get inertial sensing. visionOS
///   presents a single Joy-Con as if it were held sideways, so its stick and
///   sensor axes are rotated back to the upright hold here.
/// - Merged (press L and R on both): one controller, one set of sensors,
///   applied to the right hand.
///
/// Joy-Con layout, held upright one per hand:
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

    private enum Role { case merged, left, right, oneHanded }

    private struct Source {
        let controller: GCController
        let role: Role
    }

    private let lock = NSLock()
    private var state = Snapshot(left: nil, right: nil, status: String(localized: "Not connected (hand gestures control the game)"))
    private let queue = DispatchQueue(label: "com.verantyx.game-controller", qos: .userInteractive)
    private var observers: [NSObjectProtocol] = []
    private var sources: [Source] = []
    private var motionDescription = String(localized: "none")
    private var mergedMotion: VRControllerMotion?
    private var leftMotion: VRControllerMotion?
    private var rightMotion: VRControllerMotion?
    private var oneHandedMotion: VRControllerMotion?

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
    private static func joyConRole(_ controller: GCController) -> Role? {
        let name = name(of: controller) + " " + controller.productCategory
        guard name.contains("Joy-Con") else { return nil }
        if name.contains("L/R") || name.contains("L / R") { return .merged }
        if name.contains("(L)") || name.hasSuffix(" L") || name.contains("Left") { return .left }
        if name.contains("(R)") || name.hasSuffix(" R") || name.contains("Right") { return .right }
        return nil
    }

    private static func motionSample(from motion: GCMotion, role: Role) -> VRControllerMotion {
        // A single Joy-Con is reported as if held sideways with its rail on
        // top: the left one is the upright Joy-Con turned counterclockwise,
        // the right one turned clockwise. Rotate the axes (about the face
        // normal, z) back to the upright hold.
        func upright(_ v: SIMD3<Double>) -> SIMD3<Float> {
            switch role {
            case .merged: return SIMD3<Float>(Float(v.x), Float(v.y), Float(v.z))
            case .left: return SIMD3<Float>(Float(v.y), Float(-v.x), Float(v.z))
            case .right: return SIMD3<Float>(Float(-v.y), Float(v.x), Float(v.z))
            case .oneHanded:
                // Same rotation as the sticks, about the pad's face normal.
                let rotated = VRGamepadHold.current.rotate(SIMD2<Float>(Float(v.x), Float(v.y)))
                return SIMD3<Float>(rotated.x, rotated.y, Float(v.z))
            }
        }
        var sample = VRControllerMotion()
        sample.acceleration = upright(SIMD3<Double>(motion.acceleration.x, motion.acceleration.y, motion.acceleration.z))
        if motion.hasGravityAndUserAcceleration {
            sample.userAcceleration = upright(SIMD3<Double>(motion.userAcceleration.x, motion.userAcceleration.y, motion.userAcceleration.z))
            sample.gravity = upright(SIMD3<Double>(motion.gravity.x, motion.gravity.y, motion.gravity.z))
        }
        if motion.hasRotationRate {
            sample.rotationRate = upright(SIMD3<Double>(motion.rotationRate.x, motion.rotationRate.y, motion.rotationRate.z))
        }
        sample.timestamp = ProcessInfo.processInfo.systemUptime
        return sample
    }

    private func updateMotion(from motion: GCMotion, role: Role) {
        let sample = Self.motionSample(from: motion, role: role)
        lock.lock()
        switch role {
        case .merged:
            mergedMotion = sample
            if Self.mergedPairOneHanded { state.left?.motion = sample } else { state.right?.motion = sample }
        case .left:
            leftMotion = sample
            state.left?.motion = sample
        case .right:
            rightMotion = sample
            state.right?.motion = sample
        case .oneHanded:
            oneHandedMotion = sample
            state.left?.motion = sample
        }
        lock.unlock()
    }

    @MainActor
    private func refresh() {
        let controllers = GCController.controllers().filter { $0.extendedGamepad != nil }
        let singles = controllers.compactMap { controller -> Source? in
            guard let role = Self.joyConRole(controller), role != .merged else { return nil }
            return Source(controller: controller, role: role)
        }
        var chosen: [Source] = []
        if singles.contains(where: { $0.role == .left }) || singles.contains(where: { $0.role == .right }) {
            // One of each at most; a second controller of the same side is ignored.
            if let left = singles.first(where: { $0.role == .left }) { chosen.append(left) }
            if let right = singles.first(where: { $0.role == .right }) { chosen.append(right) }
        } else if let merged = controllers.first(where: { Self.joyConRole($0) == .merged }) {
            chosen = [Source(controller: merged, role: .merged)]
        } else if let pad = controllers.first {
            // A Pro Controller or any other gamepad: held in the left hand, it
            // supplies both sticks and all buttons; the right hand stays free
            // and uses its gestures for trigger and grip.
            chosen = [Source(controller: pad, role: .oneHanded)]
        }

        let previous = sources.map { ObjectIdentifier($0.controller) }
        let next = chosen.map { ObjectIdentifier($0.controller) }
        if previous != next {
            for source in sources {
                source.controller.extendedGamepad?.valueChangedHandler = nil
                source.controller.motion?.valueChangedHandler = nil
            }
            sources = chosen
            var descriptions: [String] = []
            for source in chosen {
                let controller = source.controller
                let role = source.role
                controller.handlerQueue = queue
                controller.extendedGamepad?.valueChangedHandler = { [weak self] gamepad, _ in
                    self?.update(from: gamepad, role: role)
                }
                if let motion = controller.motion {
                    if motion.sensorsRequireManualActivation { motion.sensorsActive = true }
                    descriptions.append(String(localized: "gyro \(motion.hasRotationRate ? "yes" : "no") / accelerometer \(motion.hasGravityAndUserAcceleration ? "separated" : "combined")"))
                    motion.valueChangedHandler = { [weak self] motion in
                        self?.updateMotion(from: motion, role: role)
                    }
                } else {
                    descriptions.append(String(localized: "none"))
                }
                let buttons = controller.extendedGamepad.map { pad in
                    pad.allButtons.map { $0.localizedName ?? "?" }.sorted().joined(separator: ", ")
                } ?? ""
                print("Game controller in use: \(Self.name(of: controller)) (\(controller.productCategory)) as \(role); buttons: \(buttons)")
            }
            motionDescription = descriptions.joined(separator: " + ")
            lock.lock()
            mergedMotion = nil
            leftMotion = nil
            rightMotion = nil
            oneHandedMotion = nil
            state = Snapshot(left: nil, right: nil, status: state.status)
            lock.unlock()
        }

        if chosen.isEmpty {
            lock.lock()
            state = Snapshot(left: nil, right: nil, status: String(localized: "Not connected (hand gestures control the game)"))
            lock.unlock()
            return
        }
        for source in chosen {
            if let gamepad = source.controller.extendedGamepad { update(from: gamepad, role: source.role) }
        }
        let names = chosen.map { Self.name(of: $0.controller) }.joined(separator: " + ")
        let mode: String
        switch chosen.first?.role {
        case .merged where Self.mergedPairOneHanded:
            mode = String(localized: "merged pair held in the left hand (ZR/R, both sticks, arrows, A/B/X/Y); the right hand uses gestures")
        case .merged: mode = String(localized: "merged pair, sensors on the right hand")
        case .oneHanded: mode = String(localized: "one-handed in the left hand; the right hand uses gestures for trigger and grip")
        default: mode = String(localized: "separate Joy-Con, sensors per hand")
        }
        setStatus(String(localized: "Using \(names) (\(mode); position from hand tracking, motion sensors: \(motionDescription))"))
    }

    private func setStatus(_ status: String) {
        lock.lock()
        state.status = status
        lock.unlock()
    }

    private static func stick(_ pad: GCControllerDirectionPad) -> SIMD2<Float> {
        let value = SIMD2<Float>(pad.xAxis.value, pad.yAxis.value)
        return simd_length(value) < 0.08 ? .zero : simd_clamp(value, SIMD2<Float>(-1, -1), SIMD2<Float>(1, 1))
    }

    /// Reads one single Joy-Con, which visionOS reports as a sideways
    /// controller: the stick axes are rotated back to the upright hold and
    /// every button that can mean "trigger" or "grip" is accepted, since the
    /// sideways profile names the physical buttons differently.
    private static func singleJoyCon(_ gamepad: GCExtendedGamepad, role: Role) -> VRControllerHand {
        var hand = VRControllerHand()
        func pressedValue(matching names: [String]) -> Float {
            var value: Float = 0
            for button in gamepad.allButtons {
                let name = (button.localizedName ?? "").uppercased()
                if names.contains(where: { name == $0 || name.hasPrefix($0 + " ") || name.contains("(" + $0 + ")") }) {
                    value = max(value, button.value)
                }
            }
            return value
        }
        hand.trigger = max(gamepad.leftTrigger.value, gamepad.rightTrigger.value, pressedValue(matching: ["ZL", "ZR"]))
        hand.grip = max(gamepad.leftShoulder.value, gamepad.rightShoulder.value, pressedValue(matching: ["L", "R", "SL", "SR"]))
        let sideways = stick(gamepad.leftThumbstick)
        hand.stick = role == .left ? SIMD2<Float>(sideways.y, -sideways.x) : SIMD2<Float>(-sideways.y, sideways.x)
        hand.stickClick = gamepad.leftThumbstickButton?.isPressed == true || gamepad.rightThumbstickButton?.isPressed == true
        // Sideways, the four face buttons (or the left Joy-Con's arrows) are
        // reported as A/B/X/Y by position: A/B sit toward the right/bottom.
        hand.primary = gamepad.buttonA.isPressed || gamepad.buttonB.isPressed || gamepad.dpad.down.isPressed || gamepad.dpad.left.isPressed
        hand.secondary = gamepad.buttonX.isPressed || gamepad.buttonY.isPressed || gamepad.dpad.up.isPressed || gamepad.dpad.right.isPressed
        hand.menu = gamepad.buttonMenu.isPressed || gamepad.buttonOptions?.isPressed == true
        return hand
    }

    /// One gamepad in the left hand. Its left half (ZL, L, left stick,
    /// d-pad, −) is the left Touch controller; its right stick and A/B/X/Y
    /// drive the free right hand, whose trigger and grip are gestures.
    private func updateOneHanded(from gamepad: GCExtendedGamepad) {
        let hold = VRGamepadHold.current
        var left = VRControllerHand()
        left.trigger = gamepad.leftTrigger.value
        left.grip = gamepad.leftShoulder.value
        left.stick = hold.rotate(Self.stick(gamepad.leftThumbstick))
        left.stickClick = gamepad.leftThumbstickButton?.isPressed == true
        left.primary = gamepad.dpad.down.isPressed || gamepad.dpad.left.isPressed
        left.secondary = gamepad.dpad.up.isPressed || gamepad.dpad.right.isPressed
        left.menu = gamepad.buttonMenu.isPressed || gamepad.buttonOptions?.isPressed == true
        left.stickAimsTeleport = true

        var right = VRControllerHand()
        right.stick = hold.rotate(Self.stick(gamepad.rightThumbstick))
        right.stickClick = gamepad.rightThumbstickButton?.isPressed == true
        right.primary = gamepad.buttonA.isPressed || gamepad.buttonB.isPressed
        right.secondary = gamepad.buttonX.isPressed || gamepad.buttonY.isPressed
        right.gestureTriggerGrip = true

        lock.lock()
        left.motion = oneHandedMotion
        state.left = left
        state.right = right
        lock.unlock()
    }

    /// The merged Joy-Con pair held in the left hand (UserDefaults
    /// "mergedJoyConOneHanded", default on): the right Joy-Con's ZR and R are
    /// the trigger and grip (the left one's ZL and L are ignored), the right
    /// Joy-Con's stick moves (teleport: the left Touch stick) and the left
    /// Joy-Con's stick turns (the right Touch stick). The arrows are X/Y and
    /// A/B/X/Y are the right Touch buttons; the right hand stays free and its
    /// trigger and grip are gestures.
    private func updateOneHandedMergedPair(from gamepad: GCExtendedGamepad) {
        let hold = VRGamepadHold.current
        var left = VRControllerHand()
        left.trigger = gamepad.rightTrigger.value
        left.grip = gamepad.rightShoulder.value
        left.stick = hold.rotate(Self.stick(gamepad.rightThumbstick))
        left.stickClick = gamepad.rightThumbstickButton?.isPressed == true
        left.primary = gamepad.dpad.down.isPressed || gamepad.dpad.left.isPressed
        left.secondary = gamepad.dpad.up.isPressed || gamepad.dpad.right.isPressed
        left.menu = gamepad.buttonMenu.isPressed || gamepad.buttonOptions?.isPressed == true
        left.stickAimsTeleport = true

        var right = VRControllerHand()
        right.stick = hold.rotate(Self.stick(gamepad.leftThumbstick))
        right.stickClick = gamepad.leftThumbstickButton?.isPressed == true
        right.primary = gamepad.buttonA.isPressed || gamepad.buttonB.isPressed
        right.secondary = gamepad.buttonX.isPressed || gamepad.buttonY.isPressed
        right.gestureTriggerGrip = true

        lock.lock()
        left.motion = mergedMotion
        state.left = left
        state.right = right
        lock.unlock()
    }

    static var mergedPairOneHanded: Bool {
        UserDefaults.standard.object(forKey: "mergedJoyConOneHanded") == nil || UserDefaults.standard.bool(forKey: "mergedJoyConOneHanded")
    }

    private func update(from gamepad: GCExtendedGamepad, role: Role) {
        switch role {
        case .merged where Self.mergedPairOneHanded:
            updateOneHandedMergedPair(from: gamepad)
        case .oneHanded:
            updateOneHanded(from: gamepad)
        case .left:
            var hand = Self.singleJoyCon(gamepad, role: .left)
            lock.lock()
            hand.motion = leftMotion
            state.left = hand
            lock.unlock()
        case .right:
            var hand = Self.singleJoyCon(gamepad, role: .right)
            lock.lock()
            hand.motion = rightMotion
            state.right = hand
            lock.unlock()
        case .merged:
            let menu = gamepad.buttonMenu.isPressed || gamepad.buttonOptions?.isPressed == true
            var left = VRControllerHand()
            left.trigger = gamepad.leftTrigger.value
            left.grip = gamepad.leftShoulder.value
            left.stick = Self.stick(gamepad.leftThumbstick)
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
            right.stick = Self.stick(gamepad.rightThumbstick)
            right.stickClick = gamepad.rightThumbstickButton?.isPressed == true
            right.primary = gamepad.buttonA.isPressed || gamepad.buttonB.isPressed
            right.secondary = gamepad.buttonX.isPressed || gamepad.buttonY.isPressed

            lock.lock()
            right.motion = mergedMotion
            state.left = left
            state.right = right
            lock.unlock()
        }
    }
}
