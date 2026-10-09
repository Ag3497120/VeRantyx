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

final class DeviceAnchorHistory {
    private struct Sample {
        let timestamp: CFTimeInterval
        let anchor: DeviceAnchor
    }

    private let lock = NSLock()
    private var samples: [Sample] = []
    private let maximumSamples = 180 // About two seconds at 90 Hz.

    func append(_ anchor: DeviceAnchor, timestamp: CFTimeInterval) {
        lock.lock()
        samples.append(Sample(timestamp: timestamp, anchor: anchor))
        if samples.count > maximumSamples {
            samples.removeFirst(samples.count - maximumSamples)
        }
        lock.unlock()
    }

    func anchor(near timestamp: CFTimeInterval) -> DeviceAnchor? {
        lock.lock()
        defer { lock.unlock() }
        guard let nearest = samples.min(by: {
            abs($0.timestamp - timestamp) < abs($1.timestamp - timestamp)
        }), abs(nearest.timestamp - timestamp) <= 0.04 else {
            return nil
        }
        return nearest.anchor
    }

    func removeAll() {
        lock.lock()
        samples.removeAll(keepingCapacity: true)
        lock.unlock()
    }
}

@MainActor
final class VisionTrackingManager: ObservableObject {
    @Published private(set) var status = "開始ボタンを押すとトラッキングを開始します。"
    @Published private(set) var inputStatus = "入力待ち"
    @Published private(set) var handPositionStatus = "手の位置待ち"

    private let session = ARKitSession()
    let worldTracking = WorldTrackingProvider()
    private let handTracking = HandTrackingProvider()
    let deviceAnchorHistory = DeviceAnchorHistory()
    let projectionTangents = VisionProjectionTangents()
    private var poseTask: Task<Void, Never>?
    private var headTransform = matrix_identity_float4x4
    private var leftHandTransform = matrix_identity_float4x4
    private var rightHandTransform = matrix_identity_float4x4
    private var worldToSteamVR: simd_float4x4?
    private var headIsTracked = false
    private var leftHandIsTracked = false
    private var rightHandIsTracked = false
    private var leftPinch = false
    private var rightPinch = false
    private var leftTriggerValue: UInt8 = 0
    private var rightTriggerValue: UInt8 = 0
    private var leftGrip: UInt8 = 0
    private var rightGrip: UInt8 = 0
    private var leftPrimaryButton = false
    private var leftSecondaryButton = false
    private var rightPrimaryButton = false
    private var rightSecondaryButton = false
    private var leftClutchActive = false
    private var rightClutchActive = false
    private var leftClutchOrigin = SIMD3<Float>.zero
    private var rightClutchOrigin = SIMD3<Float>.zero
    private var leftStick = SIMD2<Float>.zero
    private var rightStick = SIMD2<Float>.zero
    private var lastInputStatusUpdate: CFTimeInterval = 0
    private var lastHandPositionStatusUpdate: CFTimeInterval = 0
    private var sendPacket: ((Data) -> Void)?

    init() {
        headTransform = Self.untrackedTransform
        leftHandTransform = Self.untrackedTransform
        rightHandTransform = Self.untrackedTransform
    }

    func start(send: @escaping (Data) -> Void) async -> Bool {
        guard poseTask == nil else { return true }
        sendPacket = send

        guard WorldTrackingProvider.isSupported else {
            status = "このVision Proではワールドトラッキングを利用できません。"
            return false
        }

        do {
            var providers: [any DataProvider] = [worldTracking]
            if HandTrackingProvider.isSupported {
                providers.append(handTracking)

                let requiredAuthorizations = HandTrackingProvider.requiredAuthorizations
                let authorization = await session.requestAuthorization(for: requiredAuthorizations)
                let denied = requiredAuthorizations.filter { authorization[$0] != .allowed }
                guard denied.isEmpty else {
                    status = "手の追跡が許可されていません。Vision Proの「設定 > プライバシーとセキュリティ > 手のトラッキング」でMBP AVP VRを許可してください。"
                    sendPacket = nil
                    return false
                }
            } else {
                status = "このVision Proでは手のトラッキングを利用できません。"
                sendPacket = nil
                return false
            }
            try await session.run(providers)
            headIsTracked = false
            leftHandIsTracked = false
            rightHandIsTracked = false
            worldToSteamVR = nil
            deviceAnchorHistory.removeAll()
            headTransform = Self.untrackedTransform
            leftHandTransform = Self.untrackedTransform
            rightHandTransform = Self.untrackedTransform
            status = "ARKitを起動しました。頭と手を追跡しています。"

            poseTask = Task { @MainActor [weak self] in
                guard let self else { return }
                while !Task.isCancelled {
                    let timestamp = CACurrentMediaTime()
                    if let deviceAnchor = self.worldTracking.queryDeviceAnchor(atTimestamp: timestamp),
                       deviceAnchor.isTracked {
                        self.deviceAnchorHistory.append(deviceAnchor, timestamp: timestamp)
                        let wasTracked = self.headIsTracked
                        let worldHead = deviceAnchor.originFromAnchorTransform
                        if self.worldToSteamVR == nil {
                            self.worldToSteamVR = Self.makeWorldToSteamVR(from: worldHead)
                        }
                        if let worldToSteamVR = self.worldToSteamVR {
                            self.headTransform = worldToSteamVR * worldHead
                            self.headIsTracked = true
                            if !wasTracked { self.updateTrackingStatus() }
                        }
                    } else {
                        let wasTracked = self.headIsTracked
                        self.headIsTracked = false
                        if wasTracked { self.updateTrackingStatus() }
                    }

                    // Anchor updates can arrive before the first head pose establishes
                    // the shared SteamVR origin. Poll the latest anchors after the
                    // recenter transform exists so a stationary hand isn't lost until
                    // the wearer moves it again.
                    let latestHands = self.handTracking.latestAnchors
                    if let leftHand = latestHands.leftHand {
                        self.updateHand(leftHand)
                    } else {
                        self.hideHand(.left)
                    }
                    if let rightHand = latestHands.rightHand {
                        self.updateHand(rightHand)
                    } else {
                        self.hideHand(.right)
                    }
                    self.updateHandPositionStatus()
                    self.sendPacket?(self.makePosePacket(timestamp: timestamp))
                    try? await Task.sleep(nanoseconds: 11_111_111)
                }
            }
            return true
        } catch {
            status = "ARKitを開始できませんでした: \(error.localizedDescription)"
            print("Vision tracking start failed: \(error)")
            session.stop()
            sendPacket = nil
            return false
        }
    }

    func stop() {
        poseTask?.cancel()
        poseTask = nil
        sendPacket = nil
        session.stop()
        headIsTracked = false
        leftHandIsTracked = false
        rightHandIsTracked = false
        leftPinch = false
        rightPinch = false
        leftTriggerValue = 0
        rightTriggerValue = 0
        leftGrip = 0
        rightGrip = 0
        leftPrimaryButton = false
        leftSecondaryButton = false
        rightPrimaryButton = false
        rightSecondaryButton = false
        leftClutchActive = false
        rightClutchActive = false
        leftStick = .zero
        rightStick = .zero
        worldToSteamVR = nil
        deviceAnchorHistory.removeAll()
        status = "トラッキング停止中"
    }

    private func updateHand(_ anchor: HandAnchor) {
        guard anchor.isTracked, let skeleton = anchor.handSkeleton else {
            hideHand(anchor.chirality)
            return
        }

        let thumbTip = skeleton.joint(.thumbTip)
        let indexTip = skeleton.joint(.indexFingerTip)
        let middleTip = skeleton.joint(.middleFingerTip)
        let ringTip = skeleton.joint(.ringFingerTip)
        let littleTip = skeleton.joint(.littleFingerTip)
        guard let worldToSteamVR else {
            hideHand(anchor.chirality)
            return
        }

        let metacarpals = [
            skeleton.joint(.indexFingerMetacarpal),
            skeleton.joint(.middleFingerMetacarpal),
            skeleton.joint(.ringFingerMetacarpal),
            skeleton.joint(.littleFingerMetacarpal)
        ]
        // ARKit's hand-anchor origin is at the wrist. Place the virtual controller
        // between the wrist and knuckles so its grip sits in the palm, not at the
        // knuckle line where a full metacarpal average would put it.
        var worldFromPalm = anchor.originFromAnchorTransform
        let trackedMetacarpals = metacarpals.filter(\.isTracked)
        if trackedMetacarpals.count >= 2 {
            let palmPositions = trackedMetacarpals.map {
                (anchor.originFromAnchorTransform * $0.anchorFromJointTransform).columns.3
            }
            let palmCenter = palmPositions.reduce(SIMD3<Float>.zero) {
                $0 + SIMD3<Float>($1.x, $1.y, $1.z)
            } / Float(palmPositions.count)
            let wristPosition = SIMD3<Float>(
                anchor.originFromAnchorTransform.columns.3.x,
                anchor.originFromAnchorTransform.columns.3.y,
                anchor.originFromAnchorTransform.columns.3.z
            )
            let controllerGripCenter = wristPosition + (palmCenter - wristPosition) * 0.55
            worldFromPalm.columns.3 = SIMD4<Float>(controllerGripCenter, 1)
        }

        let thumbPosition = Self.worldPosition(thumbTip, anchorTransform: anchor.originFromAnchorTransform)
        let indexPosition = Self.worldPosition(indexTip, anchorTransform: anchor.originFromAnchorTransform)
        let middlePosition = Self.worldPosition(middleTip, anchorTransform: anchor.originFromAnchorTransform)
        let ringPosition = Self.worldPosition(ringTip, anchorTransform: anchor.originFromAnchorTransform)
        let pinchDistance: Float? = if let thumbPosition, let indexPosition {
            simd_distance(thumbPosition, indexPosition)
        } else {
            nil
        }
        let middlePinch = Self.isPinching(thumbPosition, middlePosition)
        let ringPinch = Self.isPinching(thumbPosition, ringPosition)
        let curledFingers = [
            Self.fingerCurl(skeleton.joint(.middleFingerMetacarpal), middleTip,
                            anchorTransform: anchor.originFromAnchorTransform),
            Self.fingerCurl(skeleton.joint(.ringFingerMetacarpal), ringTip,
                            anchorTransform: anchor.originFromAnchorTransform),
            Self.fingerCurl(skeleton.joint(.littleFingerMetacarpal), littleTip,
                            anchorTransform: anchor.originFromAnchorTransform)
        ].compactMap { $0 }
        let grip = curledFingers.isEmpty ? 0 : curledFingers.reduce(0, +) / Float(curledFingers.count)
        let gripByte = UInt8((min(max(grip, 0), 1) * 255).rounded())

        if anchor.chirality == .left {
            let wasTracked = leftHandIsTracked
            leftHandTransform = worldToSteamVR * worldFromPalm
            leftHandIsTracked = true
            leftTriggerValue = Self.triggerValue(
                for: pinchDistance,
                active: &leftPinch
            )
            leftGrip = gripByte
            leftPrimaryButton = middlePinch && !leftPinch
            leftSecondaryButton = ringPinch && !leftPinch && !middlePinch
            leftStick = Self.clutchStick(
                palm: leftHandTransform.columns.3,
                grip: gripByte,
                isActive: &leftClutchActive,
                origin: &leftClutchOrigin
            )
            if !wasTracked { updateTrackingStatus() }
        } else {
            let wasTracked = rightHandIsTracked
            rightHandTransform = worldToSteamVR * worldFromPalm
            rightHandIsTracked = true
            rightTriggerValue = Self.triggerValue(
                for: pinchDistance,
                active: &rightPinch
            )
            rightGrip = gripByte
            rightPrimaryButton = middlePinch && !rightPinch
            rightSecondaryButton = ringPinch && !rightPinch && !middlePinch
            rightStick = Self.clutchStick(
                palm: rightHandTransform.columns.3,
                grip: gripByte,
                isActive: &rightClutchActive,
                origin: &rightClutchOrigin
            )
            if !wasTracked { updateTrackingStatus() }
        }
        updateInputStatus()
    }

    private func hideHand(_ chirality: HandAnchor.Chirality) {
        if chirality == .left {
            let wasTracked = leftHandIsTracked
            leftHandTransform = Self.untrackedTransform
            leftHandIsTracked = false
            leftPinch = false
            leftTriggerValue = 0
            leftGrip = 0
            leftPrimaryButton = false
            leftSecondaryButton = false
            leftClutchActive = false
            leftStick = .zero
            if wasTracked { updateTrackingStatus() }
        } else {
            let wasTracked = rightHandIsTracked
            rightHandTransform = Self.untrackedTransform
            rightHandIsTracked = false
            rightPinch = false
            rightTriggerValue = 0
            rightGrip = 0
            rightPrimaryButton = false
            rightSecondaryButton = false
            rightClutchActive = false
            rightStick = .zero
            if wasTracked { updateTrackingStatus() }
        }
        updateInputStatus()
    }

    private func updateTrackingStatus() {
        let head = headIsTracked ? "追跡中" : "未追跡"
        let left = leftHandIsTracked ? "追跡中" : "未検出"
        let right = rightHandIsTracked ? "追跡中" : "未検出"
        let next = "頭部 \(head) / 左手 \(left) / 右手 \(right)"
        if status != next { status = next }
    }

    private func updateInputStatus() {
        let now = CACurrentMediaTime()
        guard now - lastInputStatusUpdate >= 0.1 else { return }
        lastInputStatusUpdate = now
        let leftPrimary = leftPrimaryButton ? "X" : "–"
        let leftSecondary = leftSecondaryButton ? "Y" : "–"
        let rightPrimary = rightPrimaryButton ? "A" : "–"
        let rightSecondary = rightSecondaryButton ? "B" : "–"
        inputStatus = String(
            format: "左 trigger %d%% grip %d%% stick(%+.2f,%+.2f) %@%@ / 右 trigger %d%% grip %d%% stick(%+.2f,%+.2f) %@%@",
            Int(leftTriggerValue) * 100 / 255,
            Int(leftGrip) * 100 / 255,
            leftStick.x,
            leftStick.y,
            leftPrimary,
            leftSecondary,
            Int(rightTriggerValue) * 100 / 255,
            Int(rightGrip) * 100 / 255,
            rightStick.x,
            rightStick.y,
            rightPrimary,
            rightSecondary
        )
    }

    private func updateHandPositionStatus() {
        let now = CACurrentMediaTime()
        guard now - lastHandPositionStatusUpdate >= 0.25 else { return }
        lastHandPositionStatusUpdate = now

        func positionText(_ transform: simd_float4x4, tracked: Bool) -> String {
            guard tracked else { return "未検出" }
            let position = transform.columns.3
            return String(format: "(%.2f, %.2f, %.2f)m", position.x, position.y, position.z)
        }
        handPositionStatus = "左 \(positionText(leftHandTransform, tracked: leftHandIsTracked)) / 右 \(positionText(rightHandTransform, tracked: rightHandIsTracked))"
    }

    private func makePosePacket(timestamp: CFTimeInterval) -> Data {
        var packet = Data(capacity: 271)
        appendRaw(UInt32(0x45534F50), to: &packet) // "POSE" in little-endian bytes.
        appendRaw(UInt32(0), to: &packet)
        appendRaw(Double(timestamp), to: &packet)
        appendRaw(headTransform, to: &packet)
        appendRaw(leftHandTransform, to: &packet)
        appendRaw(rightHandTransform, to: &packet)
        appendRaw(UInt8(leftHandIsTracked ? 1 : 0), to: &packet)
        appendRaw(UInt8(rightHandIsTracked ? 1 : 0), to: &packet)
        appendRaw(leftTriggerValue, to: &packet)
        appendRaw(rightTriggerValue, to: &packet)
        let rightButtons = (rightPrimaryButton ? UInt32(1 << 0) : 0) |
            (rightSecondaryButton ? UInt32(1 << 1) : 0)
        let leftButtons = (leftPrimaryButton ? UInt32(1 << 8) : 0) |
            (leftSecondaryButton ? UInt32(1 << 9) : 0)
        appendRaw(rightButtons, to: &packet)
        appendRaw(leftButtons, to: &packet)
        appendRaw(rightStick.x, to: &packet)
        appendRaw(rightStick.y, to: &packet)
        appendRaw(leftStick.x, to: &packet)
        appendRaw(leftStick.y, to: &packet)
        // OpenVR orders its vertical clipping edges in reverse: pfTop is the
        // bottom (-Y) edge, and pfBottom is the top (+Y) edge.
        let tangents = projectionTangents.snapshot()
        appendRaw(tangents.left, to: &packet)
        appendRaw(tangents.right, to: &packet)
        appendRaw(leftGrip, to: &packet)
        appendRaw(rightGrip, to: &packet)
        appendRaw(UInt8(headIsTracked ? 1 : 0), to: &packet)
        assert(packet.count == 271, "Vision Pro pose packet must match the Mac receiver layout")
        return packet
    }

    private static func makeWorldToSteamVR(from initialHead: simd_float4x4) -> simd_float4x4 {
        let position = SIMD3<Float>(initialHead.columns.3.x, initialHead.columns.3.y, initialHead.columns.3.z)
        let forward = -SIMD3<Float>(initialHead.columns.2.x, 0, initialHead.columns.2.z)
        let yaw = simd_length_squared(forward) > 0.0001
            ? atan2(forward.x, -forward.z)
            : 0
        let recenterYaw = simd_float4x4(simd_quatf(angle: yaw, axis: SIMD3<Float>(0, 1, 0)))
        var removeInitialPosition = matrix_identity_float4x4
        removeInitialPosition.columns.3 = SIMD4<Float>(-position.x, -position.y, -position.z, 1)
        var standingEyeHeight = matrix_identity_float4x4
        standingEyeHeight.columns.3.y = 1.65
        return standingEyeHeight * recenterYaw * removeInitialPosition
    }

    private static var untrackedTransform: simd_float4x4 {
        simd_float4x4(columns: (
            SIMD4<Float>.zero,
            SIMD4<Float>.zero,
            SIMD4<Float>.zero,
            SIMD4<Float>.zero
        ))
    }

    private static func worldPosition(
        _ joint: HandSkeleton.Joint,
        anchorTransform: simd_float4x4
    ) -> SIMD3<Float>? {
        guard joint.isTracked else { return nil }
        let position = (anchorTransform * joint.anchorFromJointTransform).columns.3
        return SIMD3<Float>(position.x, position.y, position.z)
    }

    private static func isPinching(_ first: SIMD3<Float>?, _ second: SIMD3<Float>?) -> Bool {
        guard let first, let second else { return false }
        return simd_distance(first, second) < 0.035
    }

    private static func triggerValue(for distance: Float?, active: inout Bool) -> UInt8 {
        guard let distance else {
            active = false
            return 0
        }
        let engageDistance: Float = 0.028
        let releaseDistance: Float = 0.052
        active = active ? distance < releaseDistance : distance <= engageDistance
        guard active else { return 0 }
        let strength = min(max((releaseDistance - distance) / (releaseDistance - engageDistance), 0), 1)
        return UInt8((strength * 255).rounded())
    }

    private static func fingerCurl(
        _ metacarpal: HandSkeleton.Joint,
        _ fingertip: HandSkeleton.Joint,
        anchorTransform: simd_float4x4
    ) -> Float? {
        guard metacarpal.isTracked, fingertip.isTracked else { return nil }
        let metacarpalPosition = (anchorTransform * metacarpal.anchorFromJointTransform).columns.3
        let fingertipPosition = (anchorTransform * fingertip.anchorFromJointTransform).columns.3
        let distance = simd_distance(
            SIMD3<Float>(metacarpalPosition.x, metacarpalPosition.y, metacarpalPosition.z),
            SIMD3<Float>(fingertipPosition.x, fingertipPosition.y, fingertipPosition.z)
        )
        return min(max((0.105 - distance) / 0.065, 0), 1)
    }

    private static func clutchStick(
        palm: SIMD4<Float>,
        grip: UInt8,
        isActive: inout Bool,
        origin: inout SIMD3<Float>
    ) -> SIMD2<Float> {
        let position = SIMD3<Float>(palm.x, palm.y, palm.z)
        let gripping = grip >= 150
        guard gripping else {
            isActive = false
            return .zero
        }
        if !isActive {
            origin = position
            isActive = true
            return .zero
        }

        let delta = position - origin
        let raw = SIMD2<Float>(delta.x, -delta.z)
        let distance = simd_length(raw)
        let deadzone: Float = 0.025
        let fullScale: Float = 0.12
        guard distance > deadzone else { return .zero }
        let magnitude = min((distance - deadzone) / (fullScale - deadzone), 1)
        return raw / distance * magnitude
    }

    private func appendRaw<T>(_ value: T, to data: inout Data) {
        var value = value
        withUnsafeBytes(of: &value) { data.append(contentsOf: $0) }
    }
}
