import CompositorServices
import SwiftUI

@main
struct VRStreamerApp: App {
    @StateObject private var model = VRStreamerModel()

    private struct VRCompositorConfiguration: CompositorLayerConfiguration {
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
            if depthFormats.contains(.depth32Float) {
                configuration.depthFormat = .depth32Float
            } else if let firstDepthFormat = depthFormats.first {
                configuration.depthFormat = firstDepthFormat
            }

            let diagnostic = "Compositor capabilities: layouts=\(layouts), color=\(colorFormats), depth=\(depthFormats); selected=\(configuration.layout), \(configuration.colorFormat), \(configuration.depthFormat)"
            Task { @MainActor in
                model.recordImmersiveEvent(diagnostic)
            }
        }
    }

    var body: some Scene {
        WindowGroup {
            SteamVRStreamerView(model: model)
        }
        .windowResizability(.contentSize)

        ImmersiveSpace(id: "SteamVR") {
            CompositorLayer(configuration: VRCompositorConfiguration(model: model)) { layerRenderer in
                model.recordImmersiveEvent("CompositorLayer renderer initialization started.")
                do {
                    let renderer = try VRCompositorRenderer(
                        layerRenderer: layerRenderer,
                        worldTrackingProvider: model.tracking.worldTracking,
                        deviceAnchorHistory: model.tracking.deviceAnchorHistory,
                        projectionTangents: model.tracking.projectionTangents,
                        reportStatistics: { [weak model] statistics in
                            Task { @MainActor in model?.recordRenderStatistics(statistics) }
                        }
                    )
                    renderer.startRenderLoop()
                    model.recordImmersiveEvent("Compositor renderer started.")
                } catch {
                    model.recordImmersiveEvent("Compositor renderer failed: \(error.localizedDescription)")
                }
            }
            .onAppear { model.immersiveSpaceDidAppear() }
            .onDisappear { model.immersiveSpaceDidDisappear() }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
        .upperLimbVisibility(.automatic)
    }
}
