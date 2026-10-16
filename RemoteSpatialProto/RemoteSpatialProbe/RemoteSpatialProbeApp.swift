import ARKit
import CompositorServices
import Spatial
import SwiftUI

/// The remote immersive space's content: one CompositorLayer drawn with Metal
/// on the Mac and shown on the Vision Pro (structure follows Xcode 26's
/// "Spatial Rendering App" template).
struct ProbeImmersiveContent: CompositorContent {
    @Environment(\.remoteDeviceIdentifier) private var remoteDeviceIdentifier

    var appModel: AppModel

    var body: some CompositorContent {
        CompositorLayer(configuration: self) { @MainActor layerRenderer in
            let control = appModel.control
            layerRenderer.onSpatialEvent = { events in
                Self.handle(events, control: control, appModel: appModel)
            }

            let session: ARKitSession?
            if let remoteDeviceIdentifier {
                session = ARKitSession(device: remoteDeviceIdentifier)
                ProbeLog.shared.note("remoteDeviceIdentifier present (\(remoteDeviceIdentifier.hashValue)); ARKitSession(device:) created")
            } else {
                session = nil
                ProbeLog.shared.note("remoteDeviceIdentifier is nil: no ARKit session, rendering without a device anchor")
            }
            Renderer.start(layerRenderer, arSession: session, appModel: appModel)
        }
    }

    @MainActor
    private static func handle(_ events: SpatialEventCollection, control: ProbeControl, appModel: AppModel) {
        var active: [SIMD3<Float>] = []
        for event in events {
            let pose = event.inputDevicePose.map { p -> String in
                let v = p.pose3D.position
                return String(format: "(%.3f, %.3f, %.3f)", v.x, v.y, v.z)
            } ?? "nil"
            let ray = event.selectionRay.map { r in
                String(format: "origin (%.2f, %.2f, %.2f) dir (%.2f, %.2f, %.2f)",
                       r.origin.x, r.origin.y, r.origin.z, r.direction.x, r.direction.y, r.direction.z)
            } ?? "nil"
            let text = "spatial event kind=\(event.kind) phase=\(event.phase) chirality=\(String(describing: event.chirality)) devicePose=\(pose) ray=\(ray) t=\(event.timestamp.s6)"
            ProbeLog.shared.note(text)
            appModel.stats.spatialEvents += 1
            appModel.stats.lastSpatialEvent = "\(event.kind) \(event.phase) pose \(pose)"
            if event.phase == .active, let p = event.inputDevicePose?.pose3D.position {
                active.append(SIMD3<Float>(Float(p.x), Float(p.y), Float(p.z)))
            }
        }
        control.pinches = active
    }
}

extension ProbeImmersiveContent: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        let foveation = capabilities.supportsFoveation
        configuration.isFoveationEnabled = foveation
        let options: LayerRenderer.Capabilities.SupportedLayoutsOptions = foveation ? [.foveationEnabled] : []
        let layouts = capabilities.supportedLayouts(options: options)
        configuration.layout = layouts.contains(.layered) ? .layered : .dedicated

        var lines: [String] = []
        lines.append("supportsFoveation=\(foveation)")
        lines.append("supportedLayouts(options: []) = \(capabilities.supportedLayouts(options: []).map(Self.name))")
        lines.append("supportedLayouts(options: .foveationEnabled) = \(capabilities.supportedLayouts(options: [.foveationEnabled]).map(Self.name))")
        lines.append("supportedLayouts(options: .progressiveImmersionEnabled) = \(capabilities.supportedLayouts(options: [.progressiveImmersionEnabled]).map(Self.name))")
        lines.append("supportedColorFormats = \(capabilities.supportedColorFormats(options: []).map(\.rawValue))")
        lines.append("supportedDepthFormats = \(capabilities.supportedDepthFormats.map(\.rawValue))")
        lines.append("supportedMinimumNearPlaneDistance = \(capabilities.supportedMinimumNearPlaneDistance)")
        lines.append("defaultRenderQuality = \(capabilities.defaultRenderQuality.rawValue)")
        lines.append("chosen: layout=\(Self.name(configuration.layout)) foveation=\(configuration.isFoveationEnabled) colorFormat=\(configuration.colorFormat.rawValue) depthFormat=\(configuration.depthFormat.rawValue) maxRenderQuality=\(configuration.maxRenderQuality.rawValue)")
        lines.append("immersionStyle=full (macOS supports .full and .progressive; .mixed is unavailable in the SDK)")
        lines.append("handTracking: HandTrackingProvider is @available(macOS, unavailable) in the macOS 26 SDK; only pinch events via onSpatialEvent")
        lines.append("connectionType: not exposed by any public API (Wi-Fi or Developer Strap USB are both allowed per Apple)")
        for line in lines { ProbeLog.shared.note("capability \(line)") }
        appModel.capabilities = lines
    }

    static func name(_ layout: LayerRenderer.Layout) -> String {
        switch layout {
        case .dedicated: "dedicated"
        case .shared: "shared"
        case .layered: "layered"
        @unknown default: "layout(\(layout.rawValue))"
        }
    }
}

@main
struct RemoteSpatialProbeApp: App {
    @State private var appModel = AppModel()

    var body: some Scene {
        WindowGroup("RemoteSpatialProbe") {
            ContentView()
                .environment(appModel)
        }

        RemoteImmersiveSpace(id: appModel.immersiveSpaceID) {
            ProbeImmersiveContent(appModel: appModel)
        }
        .immersionStyle(selection: .constant(.full), in: .full)
    }
}
