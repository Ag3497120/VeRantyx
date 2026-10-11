import CompositorServices
import SwiftUI

@main
struct VRStreamerApp: App {
    @StateObject private var model = VRStreamerModel()

    struct VRCompositorConfiguration: CompositorLayerConfiguration {
        let model: VRStreamerModel

        func makeConfiguration(
            capabilities: LayerRenderer.Capabilities,
            configuration: inout LayerRenderer.Configuration
        ) {
            let layouts = capabilities.supportedLayouts(options: [])
            let colorFormats = capabilities.supportedColorFormats(options: [])
            let depthFormats = capabilities.supportedDepthFormats

            if layouts.contains(.layered) {
                configuration.layout = .layered
            } else if let firstLayout = layouts.first {
                configuration.layout = firstLayout
            }
            if colorFormats.contains(.bgra8Unorm_srgb) {
                configuration.colorFormat = .bgra8Unorm_srgb
            } else if let firstColorFormat = colorFormats.first {
                configuration.colorFormat = firstColorFormat
            }
            // Without a rasterization rate map in the render pass, foveated
            // drawables stretch the image differently in each eye. The video
            // quad is cheap to draw at full resolution, so keep foveation off.
            configuration.isFoveationEnabled = false
            if depthFormats.contains(.depth32Float) {
                configuration.depthFormat = .depth32Float
            } else if let firstDepthFormat = depthFormats.first {
                configuration.depthFormat = firstDepthFormat
            }

            let diagnostic = "Compositor capabilities: layouts=\(layouts), color=\(colorFormats), depth=\(depthFormats), foveation supported=\(capabilities.supportsFoveation); selected=\(configuration.layout), \(configuration.colorFormat), \(configuration.depthFormat), foveation=\(configuration.isFoveationEnabled)"
            Task { @MainActor in
                model.recordImmersiveEvent(diagnostic)
            }
        }
    }

    static let mainWindowID = "main"

    var body: some Scene {
        WindowGroup(id: Self.mainWindowID) {
            SteamVRStreamerView(model: model)
        }
        .windowResizability(.contentSize)

        ImmersiveSpace(id: "SteamVR") {
            VRImmersiveContent(model: model)
        }
        .immersionStyle(selection: .constant(.full), in: .full)
        // Alyx draws its own hands from the tracked hand pose. Showing the
        // real hands as well doubled them; hide them unless the wearer turns
        // them on to check alignment.
        .upperLimbVisibility(model.showsRealHands ? .visible : .hidden)
        // Keeps the palm-up Home/Control Center indicator from popping up
        // during hand gestures in the game. This is all visionOS offers: with
        // `.hidden` in a Full Space the Home indicator no longer appears from
        // a glance at the palm, but a look-and-tap on the palm still reveals
        // it and a second pinch on it opens Home View (SwiftUI docs for
        // persistentSystemOverlays(_:) and HIG "Working with system overlays
        // in visionOS", Apple engineers on forum threads 765091 / 762297 /
        // 757044). No entitlement or API disables the gesture itself; the
        // system may also ignore this preference. Home View does not close
        // this space, and SteamVRStreamerView returns to it when the app
        // comes back to the front.
        .persistentSystemOverlays(.hidden)
    }
}

/// The streamed VR view. The main window is closed while this is open (a
/// pinch aimed at the game could otherwise press its Stop button), so it is
/// reopened here once the Digital Crown ends the immersive space.
private struct VRImmersiveContent: CompositorContent {
    @Environment(\.openWindow) private var openWindow
    let model: VRStreamerModel

    var body: some CompositorContent {
        CompositorLayer(configuration: VRStreamerApp.VRCompositorConfiguration(model: model)) { layerRenderer in
            model.recordImmersiveEvent("CompositorLayer renderer initialization started.")
            do {
                let renderer = try VRCompositorRenderer(
                    layerRenderer: layerRenderer,
                    poseSampler: model.tracking.sampler,
                    reportStatistics: { [weak model] statistics in
                        Task { @MainActor in model?.recordRenderStatistics(statistics) }
                    },
                    reportState: { [weak model] state in
                        Task { @MainActor in model?.recordImmersiveEvent("Immersive renderer \(state).") }
                    }
                )
                renderer.startRenderLoop()
                model.recordImmersiveEvent("Compositor renderer started.")
            } catch {
                model.recordImmersiveEvent("Compositor renderer failed: \(error.localizedDescription)")
            }
        }
        // A CompositorLayer space needs these on its content as well: the
        // scene-level modifiers alone left the palm Home indicator active, so
        // a pinch while looking at a hand ended immersion.
        .persistentSystemOverlays(.hidden)
        .upperLimbVisibility(model.showsRealHands ? .visible : .hidden)
        .onAppear { model.immersiveSpaceDidAppear() }
        .onDisappear {
            model.immersiveSpaceDidDisappear()
            openWindow(id: VRStreamerApp.mainWindowID)
        }
    }
}
