import ARKit
import Combine
import Foundation
import QuartzCore
import simd

final class VisionProjectionTangents {
    private let lock = NSLock()
    // A 8:9 per-eye image is the safe startup profile until Compositor Services
    // supplies the exact Vision Pro frustum.
    private var left = SIMD4<Float>(-8.0 / 9.0, 8.0 / 9.0, -1, 1)
    private var right = SIMD4<Float>(-8.0 / 9.0, 8.0 / 9.0, -1, 1)

    func update(left: SIMD4<Float>, right: SIMD4<Float>) {
        lock.lock()
        self.left = left
        self.right = right
        lock.unlock()
    }

    func snapshot() -> (left: SIMD4<Float>, right: SIMD4<Float>) {
        lock.lock()
        defer { lock.unlock() }
        return (left, right)
    }
}

/// Device anchors sent to the Mac, keyed by the pose ID that travels with the
/// pose through SteamVR and back with the rendered frame. The renderer gives the
/// compositor exactly this anchor, so late reprojection starts from the pose
/// the game actually rendered.
final class DeviceAnchorHistory {
    struct Sample {
        let id: CFTimeInterval
        let sampledAt: CFTimeInterval
        let anchor: DeviceAnchor
    }

    private let lock = NSLock()
    private var samples: [Sample] = []
    // A slow game can show a frame long after its pose was sampled.
    private let maximumSamples = 400

    func append(_ anchor: DeviceAnchor, id: CFTimeInterval, sampledAt: CFTimeInterval) {
        lock.lock()
        samples.append(Sample(id: id, sampledAt: sampledAt, anchor: anchor))
        if samples.count > maximumSamples {
            samples.removeFirst(samples.count - maximumSamples)
        }
        lock.unlock()
    }

    func sample(for id: CFTimeInterval) -> Sample? {
        lock.lock()
        defer { lock.unlock() }
        // IDs are bit-exact through the Mac; recent frames sit near the end.
        for sample in samples.reversed() where abs(sample.id - id) < 0.0005 {
            return sample
        }
        return nil
    }

    func removeAll() {
        lock.lock()
        samples.removeAll(keepingCapacity: true)
        lock.unlock()
    }
}

struct VRTrackingSnapshot {
    let controllerStatus: String
    let status: String
    let inputStatus: String
    let handPositionStatus: String
    let latencyStatus: String
}

/// Runs on the immersive render thread once per display frame. It samples the
/// head and hands, turns hand gestures into Oculus Touch input and sends one
/// pose packet to the Mac. A display-locked cadence keeps SteamVR's head pose
/// stream even; the old main-actor timer jittered with UI work.
final class VRPoseSampler: @unchecked Sendable {
    let history: DeviceAnchorHistory
    let projectionTangents: VisionProjectionTangents
    let gameController = VRGameControllerInput()

    private let lock = NSLock()
    private var worldTracking: WorldTrackingProvider?
    private var handTracking: HandTrackingProvider?
    private var send: ((Data) -> Void)?
    private var onSnapshot: ((VRTrackingSnapshot) -> Void)?
    private var worldToSteamVR: simd_float4x4?
    private var leftHand = HandState(isLeft: true)
    private var rightHand = HandState(isLeft: false)
    private var headTransform = VRPoseSampler.untrackedTransform
    private var eyeCenterOffset = SIMD3<Float>.zero
    private var recenterHeldSince: CFTimeInterval?
    private var recenterFired = false
    private var originCandidateSince: CFTimeInterval?
    private var implausibleHeightSince: CFTimeInterval?
    private var lastOriginWarningAt: CFTimeInterval = 0
    private var headIsTracked = false
    private var lastPoseID: CFTimeInterval = 0
    // How far ahead of the display the Mac's frame arrives, learned from
    // displayed frames. Predicting the head that far makes reprojection small.
    private var predictionHorizon: Double = 0.03
    private var measuredLatency: Double = 0
    private var lastSnapshotAt: CFTimeInterval = 0

    // The headset measured ~170 ms from pose to display with Alyx at ~20 fps.
    // The compositor corrects any prediction error from the exact anchor, so
    // predicting further mostly shrinks the black edge seen while turning;
    // longer horizons made ARKit's prediction overshoot.
    private static let maximumHeadPrediction: Double = 0.06
    // Game hands are drawn ~150 ms after sampling; predict a little further
    // so they trail the real hands less, without amplifying tracking noise.
    private static let maximumHandPrediction: Double = 0.045

    init(history: DeviceAnchorHistory, projectionTangents: VisionProjectionTangents) {
        self.history = history
        self.projectionTangents = projectionTangents
    }

    func activate(
        worldTracking: WorldTrackingProvider,
        handTracking: HandTrackingProvider,
        send: @escaping (Data) -> Void,
        onSnapshot: @escaping (VRTrackingSnapshot) -> Void
    ) {
        lock.lock()
        self.worldTracking = worldTracking
        self.handTracking = handTracking
        self.send = send
        self.onSnapshot = onSnapshot
        worldToSteamVR = nil
        leftHand = HandState(isLeft: true)
        rightHand = HandState(isLeft: false)
        headTransform = Self.untrackedTransform
        headIsTracked = false
        lastPoseID = 0
        predictionHorizon = 0.03
        measuredLatency = 0
        lock.unlock()
        history.removeAll()
    }

    func deactivate() {
        lock.lock()
        worldTracking = nil
        handTracking = nil
        send = nil
        onSnapshot = nil
        lock.unlock()
        history.removeAll()
    }

    /// Midpoint of the two eye views in the device anchor's frame.
    func updateEyeCenter(_ offset: SIMD3<Float>) {
        guard offset.x.isFinite, offset.y.isFinite, offset.z.isFinite, simd_length(offset) < 0.2 else { return }
        lock.lock()
        eyeCenterOffset = offset
        lock.unlock()
    }

    func queryDeviceAnchor(at timestamp: CFTimeInterval) -> DeviceAnchor? {
        lock.lock()
        let provider = worldTracking
        lock.unlock()
        guard let anchor = provider?.queryDeviceAnchor(atTimestamp: timestamp), anchor.isTracked else { return nil }
        return anchor
    }

    /// The renderer calls this when a streamed frame is first shown.
    func reportDisplayed(poseID: CFTimeInterval, sampledAt: CFTimeInterval, presentationTime: CFTimeInterval) {
        lock.lock()
        let error = presentationTime - poseID
        predictionHorizon = min(Self.maximumHeadPrediction, max(0, predictionHorizon + error * 0.05))
        let latency = presentationTime - sampledAt
        measuredLatency = measuredLatency == 0 ? latency : measuredLatency + (latency - measuredLatency) * 0.1
        lock.unlock()
    }

    func sample(presentationTime: CFTimeInterval, now: CFTimeInterval) {
        lock.lock()
        guard let worldTracking, let handTracking, let send else {
            lock.unlock()
            return
        }

        var poseID = presentationTime + predictionHorizon
        // Pose IDs must keep increasing for SteamVR's driver and the history.
        if poseID <= lastPoseID { poseID = lastPoseID + 0.000_1 }
        var deviceAnchor = worldTracking.queryDeviceAnchor(atTimestamp: poseID)
        if deviceAnchor?.isTracked != true {
            poseID = max(presentationTime, lastPoseID + 0.000_1)
            deviceAnchor = worldTracking.queryDeviceAnchor(atTimestamp: poseID)
        }
        if let deviceAnchor, deviceAnchor.isTracked {
            history.append(deviceAnchor, id: poseID, sampledAt: now)
            let worldHead = deviceAnchor.originFromAnchorTransform
            let worldY = worldHead.columns.3.y
            if worldToSteamVR == nil {
                // Take the origin only from a settled, plausible head pose:
                // right after the space opens, or while ARKit relocalizes,
                // the anchor can sit at a wrong height for a moment, and an
                // origin taken then put the player in the ceiling or floor.
                if worldY > 0.6 && worldY < 2.4 {
                    if originCandidateSince == nil { originCandidateSince = now }
                    if now - (originCandidateSince ?? now) >= 0.5 {
                        worldToSteamVR = Self.makeWorldToSteamVR(from: worldHead)
                        originCandidateSince = nil
                    }
                } else {
                    originCandidateSince = nil
                    if now - lastOriginWarningAt > 5 {
                        lastOriginWarningAt = now
                        print(String(format: "Waiting for a plausible head height before placing the SteamVR origin (ARKit y = %.2f m)", worldY))
                    }
                }
            }
            var eyeCenter = matrix_identity_float4x4
            eyeCenter.columns.3 = SIMD4<Float>(eyeCenterOffset, 1)
            if let worldToSteamVR {
                headTransform = worldToSteamVR * worldHead * eyeCenter
                headIsTracked = true
                // ARKit relocalized under us (the floor moved): the head is
                // suddenly far from any real eye height. Re-place the origin.
                let steamY = headTransform.columns.3.y
                if steamY < 0.3 || steamY > 2.5 {
                    if implausibleHeightSince == nil { implausibleHeightSince = now }
                    if now - (implausibleHeightSince ?? now) >= 0.5 {
                        print(String(format: "Head height %.2f m in SteamVR space is implausible; re-placing the origin", steamY))
                        self.worldToSteamVR = nil
                        implausibleHeightSince = nil
                    }
                } else {
                    implausibleHeightSince = nil
                }
            } else {
                headIsTracked = false
            }
        } else {
            headIsTracked = false
            originCandidateSince = nil
        }
        lastPoseID = poseID

        let handTime = now + min(max(poseID - now, 0), Self.maximumHandPrediction)
        let anchors = handTracking.handAnchors(at: handTime)
        let pads = gameController.snapshot()
        // Recenter: hold both Joy-Con stick clicks for one second. Resets the
        // horizontal position and yaw to the current head pose (height stays
        // floor-based).
        // Without Joy-Con: both fists held above head height for 1.5 s.
        let headY = headTransform.columns.3.y
        let fistsRaised = headIsTracked && leftHand.isFistRaised(above: headY) && rightHand.isFistRaised(above: headY)
        let bothStickClicks = pads.left?.stickClick == true && pads.right?.stickClick == true
        if bothStickClicks || fistsRaised {
            if recenterHeldSince == nil { recenterHeldSince = now }
            if let since = recenterHeldSince, now - since >= (bothStickClicks ? 1.0 : 1.5), !recenterFired {
                recenterFired = true
                worldToSteamVR = nil
                print("Recentered SteamVR space on request")
            }
        } else {
            recenterHeldSince = nil
            recenterFired = false
        }
        if let worldToSteamVR {
            leftHand.update(anchor: anchors.leftHand, worldToSteamVR: worldToSteamVR,
                            head: headTransform, headTracked: headIsTracked, now: now, controller: pads.left)
            rightHand.update(anchor: anchors.rightHand, worldToSteamVR: worldToSteamVR,
                             head: headTransform, headTracked: headIsTracked, now: now, controller: pads.right)
        }

        let packet = makePosePacket(poseID: poseID)
        let snapshot = now - lastSnapshotAt >= 0.2 ? makeSnapshot() : nil
        if snapshot != nil { lastSnapshotAt = now }
        let onSnapshot = self.onSnapshot
        lock.unlock()

        send(packet)
        if let snapshot, let onSnapshot { onSnapshot(snapshot) }
    }

    private func makeSnapshot() -> VRTrackingSnapshot {
        let head = headIsTracked ? String(localized: "tracked") : String(localized: "not tracked")
        let status = String(localized: "Head \(head) / left hand \(leftHand.trackingLabel) / right hand \(rightHand.trackingLabel)")
        let input = String(localized: "Left \(leftHand.inputLabel(primary: "X", secondary: "Y")) / right \(rightHand.inputLabel(primary: "A", secondary: "B"))")
        let positions = String(localized: "Left \(leftHand.positionLabel) / right \(rightHand.positionLabel)")
        let latency = measuredLatency > 0
            ? String(format: String(localized: "pose→display %.0f ms / head prediction %.0f ms"), measuredLatency * 1000, predictionHorizon * 1000)
            : String(format: String(localized: "waiting for video / head prediction %.0f ms"), predictionHorizon * 1000)
        return VRTrackingSnapshot(controllerStatus: gameController.snapshot().status,
                                  status: status, inputStatus: input,
                                  handPositionStatus: positions, latencyStatus: latency)
    }

    private func makePosePacket(poseID: CFTimeInterval) -> Data {
        var packet = Data(capacity: 319)
        appendRaw(UInt32(0x45534F50), to: &packet) // "POSE" in little-endian bytes.
        appendRaw(UInt32(0), to: &packet)
        // The pose ID is also the timestamp the head was predicted to.
        appendRaw(Double(poseID), to: &packet)
        appendRaw(headTransform, to: &packet)
        appendRaw(leftHand.transform, to: &packet)
        appendRaw(rightHand.transform, to: &packet)
        appendRaw(leftHand.trackingState, to: &packet)
        appendRaw(rightHand.trackingState, to: &packet)
        appendRaw(leftHand.triggerValue, to: &packet)
        appendRaw(rightHand.triggerValue, to: &packet)
        appendRaw(rightHand.buttonBits, to: &packet)
        appendRaw(leftHand.buttonBits << 8, to: &packet)
        appendRaw(rightHand.stick.x, to: &packet)
        appendRaw(rightHand.stick.y, to: &packet)
        appendRaw(leftHand.stick.x, to: &packet)
        appendRaw(leftHand.stick.y, to: &packet)
        // OpenVR orders its vertical clipping edges in reverse: pfTop is the
        // bottom (-Y) edge, and pfBottom is the top (+Y) edge.
        let tangents = projectionTangents.snapshot()
        appendRaw(tangents.left, to: &packet)
        appendRaw(tangents.right, to: &packet)
        appendRaw(leftHand.gripValue, to: &packet)
        appendRaw(rightHand.gripValue, to: &packet)
        appendRaw(UInt8(headIsTracked ? 1 : 0), to: &packet)
        // Velocities (m/s, rad/s in SteamVR space) for SteamVR's pose
        // prediction and Alyx's gravity-glove flick detection.
        appendRaw(leftHand.velocity, to: &packet)
        appendRaw(rightHand.velocity, to: &packet)
        appendRaw(leftHand.angularVelocity, to: &packet)
        appendRaw(rightHand.angularVelocity, to: &packet)
        assert(packet.count == 319, "Vision Pro pose packet must match the Mac receiver layout")
        return packet
    }

    private func appendRaw<T>(_ value: T, to data: inout Data) {
        var value = value
        withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
    }

    private func appendRaw(_ value: SIMD3<Float>, to data: inout Data) {
        appendRaw(value.x, to: &data)
        appendRaw(value.y, to: &data)
        appendRaw(value.z, to: &data)
    }

    /// visionOS puts the ARKit world origin on the floor beneath the wearer,
    /// so the head's y is the real eye height above the floor and maps to
    /// SteamVR's floor-level origin directly. Only x/z and yaw are recentered.
    /// (A fixed 1.65 m eye height put the floor wrong for anyone who was not
    /// that tall, or who started seated.)
    private static func makeWorldToSteamVR(from initialHead: simd_float4x4) -> simd_float4x4 {
        let position = SIMD3<Float>(initialHead.columns.3.x, initialHead.columns.3.y, initialHead.columns.3.z)
        let forward = -SIMD3<Float>(initialHead.columns.2.x, 0, initialHead.columns.2.z)
        let yaw = simd_length_squared(forward) > 0.0001
            ? atan2(forward.x, -forward.z)
            : 0
        let recenterYaw = simd_float4x4(simd_quatf(angle: yaw, axis: SIMD3<Float>(0, 1, 0)))
        var removeInitialPosition = matrix_identity_float4x4
        // Keep the floor-relative height unless it is implausible (origin not
        // on the floor), in which case assume a standing 1.65 m eye height.
        let floorIsValid = position.y > 0.6 && position.y < 2.4
        removeInitialPosition.columns.3 = SIMD4<Float>(-position.x, floorIsValid ? 0 : 1.65 - position.y, -position.z, 1)
        print("Vision Pro world → SteamVR: eye height \(position.y) m \(floorIsValid ? "from the ARKit floor" : "(implausible, using 1.65 m)")")
        return recenterYaw * removeInitialPosition
    }

    static var untrackedTransform: simd_float4x4 {
        simd_float4x4(columns: (
            SIMD4<Float>.zero,
            SIMD4<Float>.zero,
            SIMD4<Float>.zero,
            SIMD4<Float>.zero
        ))
    }
}

/// One hand's emulated Oculus Touch controller.
///
/// Gestures (both hands):
/// - thumb + index pinch: trigger
/// - closing middle, ring and little fingers: grip
/// - thumb + middle pinch held: joystick. Move the hand to push the stick.
///   Left hand pushes forward by default (teleport); right hand held still
///   for 0.45 s clicks the stick (weapon select).
/// - thumb + ring pinch: A / X
/// - thumb + little pinch: B / Y
///
/// With Joy-Con connected, the controller replaces all gestures for that hand
/// and hand tracking only supplies the pose.
private struct HandState {
    enum Finger { case middle, ring, little }
    enum StickPhase { case idle, pending, deflect, forward, click }

    let isLeft: Bool
    var trackingState: UInt8 = 0 // 0 untracked, 1 measured, 2 held after loss
    var transform = VRPoseSampler.untrackedTransform
    var triggerValue: UInt8 = 0
    var gripValue: UInt8 = 0
    var stick = SIMD2<Float>.zero
    var buttonBits: UInt32 = 0
    // In SteamVR space. Angular velocity comes from the Joy-Con gyro when
    // available (sharp and immediate), otherwise from successive poses.
    var velocity = SIMD3<Float>.zero
    var angularVelocity = SIMD3<Float>.zero
    private var lastWorldPosition: SIMD3<Float>?
    private var lastWorldRotation: simd_quatf?
    private var lastWorldTime: CFTimeInterval = 0

    private var triggerEngaged = false
    private var gripEngaged = false
    private var pinchedFinger: Finger?
    private var stickPhase = StickPhase.idle
    private var stickOrigin = SIMD3<Float>.zero
    private var stickStartedAt: CFTimeInterval = 0
    private var stickRight = SIMD3<Float>(1, 0, 0)
    private var stickForward = SIMD3<Float>(0, 0, -1)
    // Last measured pose relative to the head, and its head-relative velocity.
    private var lastMeasuredLocal: simd_float4x4?
    private var lastLocalPosition: SIMD3<Float>?
    private var lastLocalTime: CFTimeInterval = 0
    private var localVelocity = SIMD3<Float>.zero
    // Dead reckoning from the Joy-Con's sensors while the cameras cannot see
    // the hand: rotation from the gyro, position from acceleration with heavy
    // damping (it drifts within a second, so it is bounded and decays).
    private var inertialVelocity = SIMD3<Float>.zero
    private var inertialOffset = SIMD3<Float>.zero
    private var inertialLastTime: TimeInterval?
    private var inertialRotation = simd_quatf(angle: 0, axis: SIMD3<Float>(0, 1, 0))
    private var lostAt: CFTimeInterval?
    private(set) var usesController = false
    // Orientation fused from the Joy-Con gyro (immediate, drift-free over a
    // few seconds) and hand tracking (slow but anchored): the gyro turns it
    // between camera samples, the camera pulls it back over ~0.1 s.
    private var fusedRotation: simd_quatf?
    private var fusedAt: CFTimeInterval = 0
    private var fusedGyroTimestamp: TimeInterval = 0
    // A sharp wrist rotation marks a gravity-glove flick: for the next 100 ms
    // the velocity toward the head is kept at the speed Alyx looks for.
    private var flickBoostUntil: CFTimeInterval = 0
    private static let flickRotationRate: Float = 6     // rad/s
    private static let flickSpeed: Float = 2.5          // m/s toward the player

    // Keep a hand that leaves the cameras' view (lowered gun, object carried
    // to the side) where it was relative to the head instead of dropping it.
    // With Joy-Con the buttons keep working, so hold the hand longer.
    private static let holdDuration: CFTimeInterval = 0.8

    init(isLeft: Bool) {
        self.isLeft = isLeft
    }

    var trackingLabel: String {
        switch trackingState {
        case 1: return String(localized: "tracked")
        case 2: return String(localized: "held")
        default: return String(localized: "not visible")
        }
    }

    var positionLabel: String {
        guard trackingState != 0 else { return String(localized: "not visible") }
        let position = transform.columns.3
        return String(format: "(%.2f, %.2f, %.2f)m", position.x, position.y, position.z)
    }

    func inputLabel(primary: String, secondary: String) -> String {
        let stickLabel: String
        if buttonBits & 4 != 0 {
            stickLabel = String(localized: "click")
        } else if buttonBits & 8 == 0 {
            stickLabel = "–"
        } else {
            stickLabel = String(format: "(%+.2f,%+.2f)", stick.x, stick.y)
        }
        return String(
            format: "trigger %d%% grip %d%% stick %@ %@%@",
            Int(triggerValue) * 100 / 255, Int(gripValue) * 100 / 255, stickLabel,
            buttonBits & 1 != 0 ? primary : "–", buttonBits & 2 != 0 ? secondary : "–"
        )
    }

    mutating func update(anchor: HandAnchor?, worldToSteamVR: simd_float4x4,
                         head: simd_float4x4, headTracked: Bool, now: CFTimeInterval,
                         controller: VRControllerHand?) {
        usesController = controller != nil
        guard let anchor, anchor.isTracked, let skeleton = anchor.handSkeleton,
              let joints = HandJoints(anchor: anchor, skeleton: skeleton),
              let worldRaw = Self.touchRawPose(joints: joints, isLeft: isLeft) else {
            handleLoss(head: head, headTracked: headTracked, now: now, controller: controller)
            return
        }

        var steamRaw = worldToSteamVR * worldRaw
        inertialVelocity = .zero
        inertialOffset = .zero
        inertialLastTime = nil
        inertialRotation = simd_quatf(angle: 0, axis: SIMD3<Float>(0, 1, 0))
        let rawPosition = SIMD3<Float>(steamRaw.columns.3.x, steamRaw.columns.3.y, steamRaw.columns.3.z)
        let rawRotation = simd_quatf(steamRaw)
        // Velocity from the tracked pose itself (before smoothing).
        if let lastWorldPosition, let lastWorldRotation, now - lastWorldTime > 0.001, now - lastWorldTime < 0.1 {
            let dt = Float(now - lastWorldTime)
            let measured = (rawPosition - lastWorldPosition) / dt
            if controller?.motion != nil {
                velocity += (measured - velocity) * 0.5
            } else {
                // Hand tracking alone: a wrist flick lasts a few samples, and
                // averaging halved its peak so Alyx's gravity gloves ignored
                // it. Follow a rising speed at once and let it decay over
                // ~80 ms so the peak reaches the game.
                let speed = simd_length(measured), current = simd_length(velocity)
                if speed >= current {
                    velocity = measured
                } else {
                    velocity += (measured - velocity) * min(1, dt / 0.08)
                }
            }
            var delta = rawRotation * lastWorldRotation.inverse
            if delta.real < 0 { delta = simd_quatf(real: -delta.real, imag: -delta.imag) }
            let axisLength = simd_length(delta.imag)
            let measuredAngular = axisLength > 1e-6 ? delta.imag / axisLength * (2 * atan2(axisLength, delta.real)) / dt : .zero
            angularVelocity += (measuredAngular - angularVelocity) * 0.5
        } else {
            velocity = .zero
            angularVelocity = .zero
        }
        lastWorldPosition = rawPosition
        lastWorldRotation = rawRotation
        lastWorldTime = now
        if let motion = controller?.motion {
            fuseInertialVelocity(motion, rotation: rawRotation)
            steamRaw = fuseOrientation(steamRaw, rawRotation: rawRotation, motion: motion, now: now)
        } else {
            fusedRotation = nil
            if simd_length(angularVelocity) > Self.flickRotationRate * 1.3 { flickBoostUntil = now + 0.1 }
        }
        if now < flickBoostUntil {
            // Alyx reads the controller's linear velocity toward the player.
            let headPosition = SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z)
            let towardHead = headPosition - rawPosition
            if simd_length(towardHead) > 0.05 {
                let direction = simd_normalize(towardHead)
                let along = simd_dot(velocity, direction)
                if along < Self.flickSpeed { velocity += direction * (Self.flickSpeed - along) }
            }
        }
        if controller != nil, trackingState == 1, let previous = Optional(transform) {
            // A hand wrapped around a Joy-Con is partly hidden, so its tracked
            // pose jitters. Smooth it while the hand is slow; follow it
            // directly when it moves fast so a flick is not blunted.
            let speed = simd_length(velocity)
            let follow: Float = speed > 0.6 ? 1 : speed > 0.2 ? 0.8 : 0.6
            steamRaw = Self.blend(previous, steamRaw, follow)
        }
        transform = steamRaw
        trackingState = 1
        lostAt = nil
        let local = head.inverse * steamRaw
        let localPosition = SIMD3<Float>(local.columns.3.x, local.columns.3.y, local.columns.3.z)
        if let lastLocalPosition, now - lastLocalTime > 0.001, now - lastLocalTime < 0.1 {
            let velocity = (localPosition - lastLocalPosition) / Float(now - lastLocalTime)
            localVelocity += (velocity - localVelocity) * 0.4
        } else {
            localVelocity = .zero
        }
        lastLocalPosition = localPosition
        lastLocalTime = now
        lastMeasuredLocal = local

        if let controller, controller.stickAimsTeleport {
            transform = Self.aimWithStick(transform, stick: controller.stick, head: head)
        }
        if let controller, !controller.gestureTriggerGrip {
            applyController(controller)
            return
        }

        // Grip from finger curl, normalized by each finger's own length so it
        // works for any hand size. 1 = straight finger, ~0.35 = tight fist.
        let extensions = [joints.middleExtension, joints.ringExtension, joints.littleExtension].compactMap { $0 }
        if !extensions.isEmpty {
            let curl = extensions.reduce(0, +) / Float(extensions.count)
            let gripRaw = Self.clamp((0.88 - curl) / 0.30)
            if gripEngaged { gripEngaged = gripRaw > 0.55 } else { gripEngaged = gripRaw >= 0.8 }
            // Alyx clicks at 0.75 and releases at 0.72. Report engaged values
            // above that band and released ones below it so noise cannot toggle it.
            gripValue = Self.byte(gripEngaged ? max(gripRaw, 0.85) : min(gripRaw, 0.7))
        }

        let pinchIndex = joints.thumbDistance(to: joints.indexTip)
        if let pinchIndex {
            let triggerRaw = Self.clamp((0.07 - pinchIndex) / 0.045)
            if triggerEngaged {
                triggerEngaged = pinchIndex < 0.042
            } else {
                // A fist also brings thumb and index together; require an
                // index finger that is not curled into the palm.
                triggerEngaged = pinchIndex <= 0.025 && (joints.indexExtension ?? 1) > 0.45
            }
            triggerValue = Self.byte(triggerEngaged ? max(triggerRaw, 0.85) : min(triggerRaw, 0.7))
        }

        if let controller, controller.gestureTriggerGrip {
            // Stick and buttons come from the gamepad in the other hand; the
            // pinch buttons and stick gesture would collide with grabbing.
            pinchedFinger = nil
            stickPhase = .idle
            stick = controller.stick
            var bits: UInt32 = 0
            if controller.primary { bits |= 1 }
            if controller.secondary { bits |= 2 }
            if controller.stickClick { bits |= 4 }
            if controller.stickClick || simd_length(controller.stick) > 0.1 { bits |= 8 }
            if controller.menu { bits |= 16 }
            buttonBits = bits
            return
        }

        updateFingerPinch(middle: joints.thumbDistance(to: joints.middleTip),
                          ring: joints.thumbDistance(to: joints.ringTip),
                          little: joints.thumbDistance(to: joints.littleTip),
                          index: pinchIndex)
        let palm = SIMD3<Float>(steamRaw.columns.3.x, steamRaw.columns.3.y, steamRaw.columns.3.z)
        updateStick(palm: palm, head: head, now: now)

        var bits: UInt32 = 0
        if pinchedFinger == .ring { bits |= 1 }
        if pinchedFinger == .little { bits |= 2 }
        if stickPhase == .click { bits |= 4 }
        if stickPhase != .idle { bits |= 8 }
        buttonBits = bits
    }

    private mutating func handleLoss(head: simd_float4x4, headTracked: Bool, now: CFTimeInterval,
                                     controller: VRControllerHand?) {
        // (A shoulder-reach guess here moved the hand behind the shoulder on
        // ordinary tracking losses, so the hand jumped away. Removed.)
        let lostSince = lostAt ?? now
        lostAt = lostSince
        lastLocalPosition = nil
        lastWorldPosition = nil
        lastWorldRotation = nil
        if controller?.motion == nil {
            velocity = .zero
            angularVelocity = .zero
        }
        if let controller, controller.gestureTriggerGrip {
            // Free hand with a gamepad in the other: the gestures need a
            // visible hand (trigger released, grip kept), the pad still works.
            triggerEngaged = false
            triggerValue = 0
            pinchedFinger = nil
            stickPhase = .idle
            stick = controller.stick
            var bits: UInt32 = 0
            if controller.primary { bits |= 1 }
            if controller.secondary { bits |= 2 }
            if controller.stickClick { bits |= 4 }
            if controller.stickClick || simd_length(controller.stick) > 0.1 { bits |= 8 }
            buttonBits = bits
        } else if let controller {
            applyController(controller)
        } else {
            // Trigger, pinches and stick need a visible hand; grip is kept while
            // held so the game does not drop what the hand was carrying.
            triggerEngaged = false
            triggerValue = 0
            pinchedFinger = nil
            stickPhase = .idle
            stick = .zero
            buttonBits = 0
        }
        // With a controller the buttons keep working, so keep the hand where
        // it last was relative to the head for as long as the hand is away;
        // dropping it made Alyx fling the hand elsewhere.
        let holdLimit = controller != nil ? CFTimeInterval.infinity : Self.holdDuration
        if let lastMeasuredLocal, headTracked, now - lostSince < holdLimit {
            transform = head * lastMeasuredLocal
            if let motion = controller?.motion { applyInertial(motion) }
            if let controller, controller.stickAimsTeleport {
                transform = Self.aimWithStick(transform, stick: controller.stick, head: head)
            }
            trackingState = 2
            return
        }
        trackingState = 0
        transform = VRPoseSampler.untrackedTransform
        lastMeasuredLocal = nil
        if controller == nil {
            gripEngaged = false
            gripValue = 0
        }
    }

    /// Joy-Con inputs replace the gestures for this hand. Bits: 1 A/X,
    /// 2 B/Y, 4 stick click, 8 stick touch, 16 menu (left system button).
    /// Moves the held hand with the Joy-Con while it is out of view. The
    /// controller's axes are taken as the hand's raw axes: held upright, its
    /// long axis (+Y) runs along the fist (raw -Z) and its face (+Z) faces
    /// the player (raw +Y), which the fixed rotation below encodes.
    /// Replaces the camera-derived angular velocity with the gyro's (rotated
    /// to SteamVR space) and adds the accelerometer's short-term velocity, so
    /// a wrist flick reaches the game at full strength.
    private mutating func fuseInertialVelocity(_ motion: VRControllerMotion, rotation: simd_quatf) {
        let handFromDevice = simd_quatf(angle: -.pi / 2, axis: SIMD3<Float>(1, 0, 0))
        let deviceToWorld = simd_float3x3(rotation) * simd_float3x3(handFromDevice)
        if let rate = motion.rotationRate {
            angularVelocity = deviceToWorld * rate
            if simd_length(rate) > Self.flickRotationRate { flickBoostUntil = CACurrentMediaTime() + 0.1 }
        }
        if let acceleration = motion.userAcceleration, let last = inertialLastTime {
            let dt = Float(motion.timestamp - last)
            if dt > 0, dt < 0.1 {
                let worldAcceleration = deviceToWorld * acceleration * 9.81
                // High-passed: decays within ~0.15 s so only sudden motion remains.
                inertialVelocity = (inertialVelocity + worldAcceleration * dt) * max(0, 1 - 8 * dt)
                velocity += inertialVelocity
            }
        }
        inertialLastTime = motion.timestamp
    }

    /// Complementary filter: integrates the gyro since the last sample and
    /// slerps toward the camera's orientation. Large disagreements (tracking
    /// re-acquired after a loss) snap to the camera.
    private mutating func fuseOrientation(_ pose: simd_float4x4, rawRotation: simd_quatf,
                                          motion: VRControllerMotion, now: CFTimeInterval) -> simd_float4x4 {
        guard let rate = motion.rotationRate else { return pose }
        var fused = fusedRotation ?? rawRotation
        if fusedRotation != nil, motion.timestamp > fusedGyroTimestamp {
            let dt = Float(min(max(now - fusedAt, 0), 0.05))
            let handFromDevice = simd_quatf(angle: -.pi / 2, axis: SIMD3<Float>(1, 0, 0))
            let worldRate = simd_float3x3(fused) * simd_float3x3(handFromDevice) * rate
            let angle = simd_length(worldRate) * dt
            if angle > 0.0005 {
                fused = simd_normalize(simd_quatf(angle: angle, axis: simd_normalize(worldRate)) * fused)
            }
            var delta = rawRotation * fused.inverse
            if delta.real < 0 { delta = simd_quatf(real: -delta.real, imag: -delta.imag) }
            let disagreement = 2 * acos(min(1, delta.real))
            if disagreement > 25 * .pi / 180 {
                fused = rawRotation
            } else {
                fused = simd_normalize(simd_slerp(fused, rawRotation, min(1, dt / 0.1)))
            }
        }
        fusedRotation = fused
        fusedAt = now
        fusedGyroTimestamp = motion.timestamp
        var result = simd_float4x4(fused)
        result.columns.3 = pose.columns.3
        return result
    }

    private mutating func applyInertial(_ motion: VRControllerMotion) {
        guard let last = inertialLastTime else {
            inertialLastTime = motion.timestamp
            return
        }
        let dt = Float(motion.timestamp - last)
        guard dt > 0, dt < 0.1 else { inertialLastTime = motion.timestamp; return }
        inertialLastTime = motion.timestamp
        let handFromDevice = simd_quatf(angle: -.pi / 2, axis: SIMD3<Float>(1, 0, 0))
        let rotation = simd_float3x3(simd_quatf(transform)) * simd_float3x3(handFromDevice)

        if let rate = motion.rotationRate {
            let worldRate = rotation * rate
            angularVelocity = worldRate
            let angle = simd_length(worldRate) * dt
            if angle > 0.0005 {
                inertialRotation = simd_normalize(simd_quatf(angle: angle, axis: simd_normalize(worldRate)) * inertialRotation)
            }
        } else {
            angularVelocity = .zero
        }
        if let acceleration = motion.userAcceleration {
            let worldAcceleration = rotation * acceleration * 9.81
            inertialVelocity = (inertialVelocity + worldAcceleration * dt) * max(0, 1 - 6 * dt)
            let offset = inertialOffset + inertialVelocity * dt
            let distance = simd_length(offset)
            inertialOffset = distance > 0.5 ? offset / distance * 0.5 : offset
        }
        velocity = inertialVelocity

        var moved = simd_float4x4(inertialRotation) * simd_float4x4(simd_quatf(transform))
        let position = SIMD3<Float>(transform.columns.3.x, transform.columns.3.y, transform.columns.3.z) + inertialOffset
        moved.columns.3 = SIMD4<Float>(position, 1)
        transform = moved
    }

    private mutating func applyController(_ controller: VRControllerHand) {
        triggerEngaged = false
        gripEngaged = controller.grip >= 0.5
        pinchedFinger = nil
        stickPhase = .idle
        triggerValue = Self.byte(controller.trigger)
        gripValue = Self.byte(controller.grip)
        stick = controller.stick
        var bits: UInt32 = 0
        if controller.primary { bits |= 1 }
        if controller.secondary { bits |= 2 }
        if controller.stickClick { bits |= 4 }
        if controller.stickClick || simd_length(controller.stick) > 0.1 { bits |= 8 }
        if controller.menu { bits |= 16 }
        buttonBits = bits
    }

    /// Teleport aimed with a stick: beyond the dead zone, the hand's pose is
    /// turned so its aim ray points along the stick's direction (relative to
    /// where the head faces), pitched from steeply down (short hop) at a
    /// light push to slightly up (full reach) at full deflection. The raw
    /// Touch pose's aim ray is tilted down from -Z by UserDefaults
    /// "teleportAimPitch" degrees (default 40).
    private static func aimWithStick(_ pose: simd_float4x4, stick: SIMD2<Float>, head: simd_float4x4) -> simd_float4x4 {
        let magnitude = simd_length(stick)
        guard magnitude > 0.15 else { return pose }
        let forward = SIMD3<Float>(-head.columns.2.x, 0, -head.columns.2.z)
        let headYaw = simd_length_squared(forward) > 1e-4 ? atan2(-forward.x, -forward.z) : 0
        let yaw = headYaw + atan2(-stick.x, stick.y)
        let reach = min(1, max(0, (magnitude - 0.15) / 0.85))
        let pitch = (-65 + 75 * reach) * Float.pi / 180
        var aimPitch = Float(UserDefaults.standard.double(forKey: "teleportAimPitch"))
        if aimPitch == 0 { aimPitch = 40 }
        let aim = simd_quatf(angle: yaw, axis: SIMD3<Float>(0, 1, 0)) * simd_quatf(angle: pitch, axis: SIMD3<Float>(1, 0, 0))
        let raw = aim * simd_quatf(angle: aimPitch * Float.pi / 180, axis: SIMD3<Float>(1, 0, 0))
        var result = simd_float4x4(raw)
        result.columns.3 = pose.columns.3
        return result
    }

    /// A tracked fist (grip engaged by finger curl, no controller) whose palm
    /// is above the given height: the hands-only recenter gesture.
    func isFistRaised(above height: Float) -> Bool {
        trackingState == 1 && gripEngaged && transform.columns.3.y > height
    }

    private mutating func updateFingerPinch(middle: Float?, ring: Float?, little: Float?, index: Float?) {
        let blocked = triggerEngaged || gripEngaged || (index ?? 1) < 0.03
        if let finger = pinchedFinger {
            let distance: Float?
            switch finger {
            case .middle: distance = middle
            case .ring: distance = ring
            case .little: distance = little
            }
            // An occluded fingertip keeps the pinch; a visible open one ends it.
            if blocked || (distance ?? 0) > 0.04 { pinchedFinger = nil }
            return
        }
        guard !blocked else { return }
        let candidates: [(Finger, Float)] = [(.middle, middle), (.ring, ring), (.little, little)]
            .compactMap { finger, distance in distance.map { (finger, $0) } }
        if let nearest = candidates.min(by: { $0.1 < $1.1 }), nearest.1 <= 0.022 {
            pinchedFinger = nearest.0
        }
    }

    private mutating func updateStick(palm: SIMD3<Float>, head: simd_float4x4, now: CFTimeInterval) {
        guard pinchedFinger == .middle else {
            stickPhase = .idle
            stick = .zero
            return
        }
        if stickPhase == .idle {
            stickPhase = .pending
            stickOrigin = palm
            stickStartedAt = now
            // Measure hand motion in the head's horizontal frame at pinch start.
            let forward = SIMD3<Float>(-head.columns.2.x, 0, -head.columns.2.z)
            stickForward = simd_length_squared(forward) > 1e-4 ? simd_normalize(forward) : SIMD3<Float>(0, 0, -1)
            stickRight = SIMD3<Float>(-stickForward.z, 0, stickForward.x)
        }

        let delta = palm - stickOrigin
        let offset = SIMD2<Float>(simd_dot(delta, stickRight), simd_dot(delta, stickForward))
        let distance = simd_length(offset)
        let deadzone: Float = 0.015
        let fullScale: Float = 0.07
        let moved = distance > deadzone

        switch stickPhase {
        case .pending:
            if moved {
                stickPhase = .deflect
            } else if isLeft && now - stickStartedAt >= 0.15 {
                stickPhase = .forward
            } else if !isLeft && now - stickStartedAt >= 0.45 {
                stickPhase = .click
            }
        case .forward:
            if moved { stickPhase = .deflect }
        default:
            break
        }

        switch stickPhase {
        case .deflect where moved:
            let magnitude = min((distance - deadzone) / (fullScale - deadzone), 1)
            stick = offset / distance * magnitude
        case .forward:
            stick = SIMD2<Float>(0, 1)
        default:
            stick = .zero
        }
    }

    /// Converts hand joints to the raw pose an Oculus Rift S controller would
    /// report if this hand held it. The OpenXR grip pose is defined from hand
    /// anatomy; SteamVR's Rift S render model says where that grip sits
    /// relative to the controller's raw pose, which Alyx aligns its hands to.
    private static func touchRawPose(joints: HandJoints, isLeft: Bool) -> simd_float4x4? {
        let forward = joints.middleKnuckle - joints.wrist
        let across = joints.indexKnuckle - joints.littleKnuckle
        guard simd_length(forward) > 0.03, simd_length(across) > 0.02 else { return nil }
        let towardThumbSide = simd_normalize(across)
        // +X: out of the right hand's back / out of the left hand's palm.
        let x = simd_normalize(simd_cross(simd_normalize(forward), towardThumbSide))
        // -Z: through the closed fist from little finger to index finger.
        var z = -towardThumbSide
        z = simd_normalize(z - simd_dot(z, x) * x)
        let y = simd_cross(z, x)
        guard x.x.isFinite, y.x.isFinite, z.x.isFinite else { return nil }

        let palmCenter = (joints.middleMetacarpal + joints.middleKnuckle) * 0.5
        let palmNormal = isLeft ? x : -x
        let gripOrigin = palmCenter + palmNormal * 0.015
        let worldGrip = simd_float4x4(columns: (
            SIMD4<Float>(x, 0), SIMD4<Float>(y, 0), SIMD4<Float>(z, 0), SIMD4<Float>(gripOrigin, 1)
        ))

        // openxr_grip in oculus_rifts_controller_{left,right}.json.
        var rawFromGrip = simd_float4x4(simd_quatf(angle: 20.6 * .pi / 180, axis: SIMD3<Float>(1, 0, 0)))
        rawFromGrip.columns.3 = SIMD4<Float>(isLeft ? 0.007 : -0.007, -0.00182941, 0.1019482, 1)
        return worldGrip * rawFromGrip.inverse
    }

    private static func blend(_ a: simd_float4x4, _ b: simd_float4x4, _ t: Float) -> simd_float4x4 {
        let qa = simd_quatf(a), qb = simd_quatf(b)
        let q = simd_slerp(qa, qb, t)
        var m = simd_float4x4(q)
        m.columns.3 = a.columns.3 + (b.columns.3 - a.columns.3) * t
        m.columns.3.w = 1
        return m
    }

    private static func clamp(_ value: Float) -> Float { min(max(value, 0), 1) }
    private static func byte(_ value: Float) -> UInt8 { UInt8((clamp(value) * 255).rounded()) }
}

/// Joint positions in ARKit world space. The wrist and knuckles define the
/// controller pose; fingertips may be missing when a fist hides them, and the
/// gestures that need them then keep their previous state.
private struct HandJoints {
    let wrist: SIMD3<Float>
    let indexKnuckle: SIMD3<Float>
    let middleKnuckle: SIMD3<Float>
    let littleKnuckle: SIMD3<Float>
    let middleMetacarpal: SIMD3<Float>
    let thumbTip: SIMD3<Float>?
    let indexTip: SIMD3<Float>?
    let middleTip: SIMD3<Float>?
    let ringTip: SIMD3<Float>?
    let littleTip: SIMD3<Float>?
    // Fingertip-to-knuckle distance over finger length: 1 straight, lower curled.
    let indexExtension: Float?
    let middleExtension: Float?
    let ringExtension: Float?
    let littleExtension: Float?

    init?(anchor: HandAnchor, skeleton: HandSkeleton) {
        let origin = anchor.originFromAnchorTransform
        func position(_ name: HandSkeleton.JointName) -> SIMD3<Float>? {
            let joint = skeleton.joint(name)
            guard joint.isTracked else { return nil }
            let p = (origin * joint.anchorFromJointTransform).columns.3
            return SIMD3<Float>(p.x, p.y, p.z)
        }
        func fingerExtension(_ knuckle: HandSkeleton.JointName, _ base: HandSkeleton.JointName,
                             _ middle: HandSkeleton.JointName, _ tip: HandSkeleton.JointName) -> Float? {
            guard let k = position(knuckle), let b = position(base),
                  let m = position(middle), let t = position(tip) else { return nil }
            let length = simd_distance(k, b) + simd_distance(b, m) + simd_distance(m, t)
            guard length > 0.02 else { return nil }
            return simd_distance(k, t) / length
        }
        guard let wrist = position(.wrist),
              let indexKnuckle = position(.indexFingerKnuckle),
              let middleKnuckle = position(.middleFingerKnuckle),
              let littleKnuckle = position(.littleFingerKnuckle),
              let middleMetacarpal = position(.middleFingerMetacarpal)
        else { return nil }
        self.wrist = wrist
        self.indexKnuckle = indexKnuckle
        self.middleKnuckle = middleKnuckle
        self.littleKnuckle = littleKnuckle
        self.middleMetacarpal = middleMetacarpal
        thumbTip = position(.thumbTip)
        indexTip = position(.indexFingerTip)
        middleTip = position(.middleFingerTip)
        ringTip = position(.ringFingerTip)
        littleTip = position(.littleFingerTip)
        indexExtension = fingerExtension(.indexFingerKnuckle, .indexFingerIntermediateBase,
                                         .indexFingerIntermediateTip, .indexFingerTip)
        middleExtension = fingerExtension(.middleFingerKnuckle, .middleFingerIntermediateBase,
                                          .middleFingerIntermediateTip, .middleFingerTip)
        ringExtension = fingerExtension(.ringFingerKnuckle, .ringFingerIntermediateBase,
                                        .ringFingerIntermediateTip, .ringFingerTip)
        littleExtension = fingerExtension(.littleFingerKnuckle, .littleFingerIntermediateBase,
                                          .littleFingerIntermediateTip, .littleFingerTip)
    }

    func thumbDistance(to tip: SIMD3<Float>?) -> Float? {
        guard let thumbTip, let tip else { return nil }
        return simd_distance(thumbTip, tip)
    }
}

@MainActor
final class VisionTrackingManager: ObservableObject {
    @Published private(set) var status = String(localized: "Tracking starts when you press Start.")
    @Published private(set) var inputStatus = String(localized: "waiting for input")
    @Published private(set) var handPositionStatus = String(localized: "waiting for hand positions")
    @Published private(set) var latencyStatus = String(localized: "waiting for video")
    @Published private(set) var controllerStatus = String(localized: "Not connected (hand gestures control the game)")

    private var session = ARKitSession()
    let deviceAnchorHistory = DeviceAnchorHistory()
    let projectionTangents = VisionProjectionTangents()
    let sampler: VRPoseSampler
    private var isRunning = false

    init() {
        sampler = VRPoseSampler(history: deviceAnchorHistory, projectionTangents: projectionTangents)
    }

    func start(send: @escaping (Data) -> Void) async -> Bool {
        guard !isRunning else { return true }

        guard WorldTrackingProvider.isSupported else {
            status = String(localized: "World tracking is not available on this Vision Pro.")
            return false
        }
        guard HandTrackingProvider.isSupported else {
            status = String(localized: "Hand tracking is not available on this Vision Pro.")
            return false
        }

        // ARKit runs a data provider only once, so make new ones for each start.
        let worldTracking = WorldTrackingProvider()
        let handTracking = HandTrackingProvider()
        session = ARKitSession()
        do {
            let requiredAuthorizations = HandTrackingProvider.requiredAuthorizations
            let authorization = await session.requestAuthorization(for: requiredAuthorizations)
            let denied = requiredAuthorizations.filter { authorization[$0] != .allowed }
            guard denied.isEmpty else {
                status = String(localized: "Hand tracking is not allowed. Enable it for VeRantyx in Settings > Privacy & Security > Hand Tracking.")
                return false
            }
            try await session.run([worldTracking, handTracking])
        } catch {
            status = String(localized: "ARKit could not start: \(error.localizedDescription)")
            print("Vision tracking start failed: \(error)")
            session.stop()
            return false
        }

        sampler.activate(worldTracking: worldTracking, handTracking: handTracking, send: send) { [weak self] snapshot in
            Task { @MainActor [weak self] in self?.apply(snapshot) }
        }
        isRunning = true
        status = String(localized: "ARKit started. Tracking head and hands.")
        return true
    }

    func stop() {
        sampler.deactivate()
        session.stop()
        isRunning = false
        status = String(localized: "Tracking stopped")
        inputStatus = String(localized: "waiting for input")
        handPositionStatus = String(localized: "waiting for hand positions")
        latencyStatus = String(localized: "waiting for video")
    }

    private func apply(_ snapshot: VRTrackingSnapshot) {
        guard isRunning else { return }
        if status != snapshot.status { status = snapshot.status }
        if inputStatus != snapshot.inputStatus { inputStatus = snapshot.inputStatus }
        if handPositionStatus != snapshot.handPositionStatus { handPositionStatus = snapshot.handPositionStatus }
        if latencyStatus != snapshot.latencyStatus { latencyStatus = snapshot.latencyStatus }
        if controllerStatus != snapshot.controllerStatus { controllerStatus = snapshot.controllerStatus }
    }
}
