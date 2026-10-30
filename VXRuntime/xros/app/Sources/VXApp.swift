// VX Runtime's app: the window (the runtime's flat display, main.m's VXRootController: the program starts there) and
// the immersive space the program's swap chain is shown in (VXImmersive.swift). The space opens by itself at launch
// unless the app was started with -present flat (the window then is the only picture: a debug fallback).
import CompositorServices
import SwiftUI

@main
struct VXRuntimeApp: App {
    @Environment(\.scenePhase) private var phase
    init() {
        vxrt_jlog("app_init", nil)
        vxrt_boot_fallback()   // (main.m: the program starts without the window if no window has appeared within a few seconds)
    }
    var body: some Scene {
        WindowGroup(id: "display") {
            VXDisplayView()
        }
        .onChange(of: phase) { _, p in vxrt_jlog("scene_phase", "\"phase\":\"\(p)\"") }
        .windowResizability(.contentSize)

        ImmersiveSpace(id: "vr") {
            CompositorLayer(configuration: VXLayerConfiguration()) { layerRenderer in
                VXImmersiveRenderer.start(layerRenderer)
            }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
    }
}

struct VXLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        // One texture per eye (the warp writes each eye's whole texture), no foveation for now (the warp would have to
        // draw through the rasterization rate map: VISIONOS_PORT.md), depth for the compositor's late reprojection.
        let layouts = capabilities.supportedLayouts(options: [])
        configuration.layout = layouts.contains(.dedicated) ? .dedicated : (layouts.first ?? .dedicated)
        configuration.isFoveationEnabled = false
        let colors = capabilities.supportedColorFormats(options: [])
        if colors.contains(.bgra8Unorm_srgb) { configuration.colorFormat = .bgra8Unorm_srgb }
        if capabilities.supportedDepthFormats.contains(.depth32Float) { configuration.depthFormat = .depth32Float }
    }
}

struct VXRootControllerView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> UIViewController { vxrt_make_root_controller() }
    func updateUIViewController(_ controller: UIViewController, context: Context) {}
}

struct VXDisplayView: View {
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @State private var opened = false

    var body: some View {
        VXRootControllerView()
            .frame(width: 1280, height: 720)
            .onAppear { vxrt_jlog("display_view", nil) }
            .task {
                guard !opened, vxrt_present_vr() != 0 else { return }
                opened = true
                let result = await openImmersiveSpace(id: "vr")
                let text: String
                switch result {
                case .opened: text = "opened"
                case .userCancelled: text = "cancelled"
                case .error: text = "error"
                @unknown default: text = "unknown"
                }
                vxrt_jlog("immersive_space", "\"result\":\"\(text)\"")
            }
    }
}
