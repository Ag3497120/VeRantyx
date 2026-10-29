import CompositorServices
import Metal
import SwiftUI

@MainActor
@Observable
final class ProtoModel {
    var settings = ProtoSettings.fromLaunch() { didSet { box.value = settings } }
    var stats = RenderStats()
    var capabilities: [String] = []
    var events: [String] = []
    var immersiveOpen = false
    let box = SettingsBox()
    private let logURL: URL

    init() {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        logURL = docs.appendingPathComponent("proto_log.jsonl")
        box.value = settings
        let d = UserDefaults.standard
        if let device = MTLCreateSystemDefaultDevice() {
            capabilities = CapabilityProbe.report(device: device)
            if d.object(forKey: "preflight") == nil || d.bool(forKey: "preflight") {
                capabilities += PreflightProbe.report(device: device)
            }
        }
        for line in capabilities { append(["event": "capability", "text": line]) }
        append(["event": "launch", "settings": "\(settings)"])
        if d.bool(forKey: "memfill"), let device = MTLCreateSystemDefaultDevice() {
            PreflightProbe.memoryFill(device: device) { self.append(["event": "memfill", "text": $0]) }
        }
    }

    func record(_ text: String) {
        events.append(text)
        if events.count > 12 { events.removeFirst(events.count - 12) }
        append(["event": "log", "text": text])
    }

    func record(stats: RenderStats) {
        self.stats = stats
        if let data = try? JSONEncoder().encode(stats), let s = String(data: data, encoding: .utf8) {
            append(["event": "stats", "stats": s])
        }
    }

    private func append(_ object: [String: String]) {
        var o = object
        o["t"] = String(format: "%.3f", Date().timeIntervalSince1970)
        guard let data = try? JSONSerialization.data(withJSONObject: o),
              var line = String(data: data, encoding: .utf8) else { return }
        line += "\n"
        if let h = try? FileHandle(forWritingTo: logURL) {
            h.seekToEndOfFile(); h.write(Data(line.utf8)); try? h.close()
        } else {
            try? Data(line.utf8).write(to: logURL)
        }
    }
}

@main
struct VisionOSProtoApp: App {
    @State private var model = ProtoModel()

    var body: some Scene {
        WindowGroup(id: "control") {
            ControlView(model: model)
        }
        .windowResizability(.contentSize)

        ImmersiveSpace(id: "proto") {
            CompositorLayer(configuration: ProtoLayerConfiguration(foveation: model.settings.foveation)) { layerRenderer in
                let box = model.box
                do {
                    let renderer = try ProtoRenderer(
                        layerRenderer: layerRenderer, settings: box,
                        report: { s in Task { @MainActor in model.record(stats: s) } },
                        log: { t in Task { @MainActor in model.record(t) } })
                    renderer.start()
                } catch {
                    Task { @MainActor in model.record("Renderer failed: \(error)") }
                }
            }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
    }
}

struct ProtoLayerConfiguration: CompositorLayerConfiguration {
    let foveation: Bool

    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        let useFoveation = foveation && capabilities.supportsFoveation
        let layouts = capabilities.supportedLayouts(options: useFoveation ? [.foveationEnabled] : [])
        configuration.layout = layouts.contains(.layered) ? .layered : (layouts.first ?? .dedicated)
        configuration.isFoveationEnabled = useFoveation
        let colors = capabilities.supportedColorFormats(options: [])
        if colors.contains(.bgra8Unorm_srgb) { configuration.colorFormat = .bgra8Unorm_srgb }
        // Depth is what lets the compositor reproject per pixel (position, not
        // only rotation) when it repeats or late-warps a frame.
        if capabilities.supportedDepthFormats.contains(.depth32Float) {
            configuration.depthFormat = .depth32Float
        }
    }
}

struct ControlView: View {
    @Bindable var model: ProtoModel
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @State private var autoOpened = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("VXRuntime standalone presentation prototype").font(.title2)
            HStack {
                Picker("Game rate", selection: $model.settings.fps) {
                    ForEach([18.0, 20.0, 22.5, 30.0, 45.0, 90.0], id: \.self) { Text(String(format: "%g fps", $0)).tag($0) }
                }
                Picker("Pacing", selection: $model.settings.mode) {
                    ForEach(PacingMode.allCases) { Text($0.label).tag($0) }
                }
            }
            HStack {
                Toggle("Give depth to compositor", isOn: $model.settings.providesDepth)
                Toggle("Moving objects", isOn: $model.settings.animate)
                Toggle("Foveation (reopen space)", isOn: $model.settings.foveation)
            }
            HStack {
                Stepper("GPU load: \(model.settings.load) iterations", value: $model.settings.load, in: 1...400, step: 8)
                Stepper(String(format: "Hold render scale %.2f", model.settings.renderScale),
                        value: $model.settings.renderScale, in: 0.4...1.0, step: 0.1)
            }
            HStack {
                Button(model.immersiveOpen ? "Close immersive space" : "Open immersive space") {
                    Task { await toggle() }
                }
            }
            if model.settings.mode == .frameRepeat && model.settings.fps == 20 {
                Text("20 fps is not a 90 Hz divisor: frame repeat gives 18 fps here. Use Hold for exact 20.")
                    .font(.caption).foregroundStyle(.orange)
            }
            GroupBox("Live") {
                let s = model.stats
                Text(String(format: "display %.1f Hz | app frames %.1f/s | game %.1f fps (target %.1f) | repeat %d | late %d",
                            s.displayHz, s.appFrames, s.gameFps, s.targetFps, s.repeatCount, s.lateFrames))
                Text(String(format: "heavy GPU %.1f ms | present GPU %.2f ms | eye %@ logical, %@ physical | foveated %@ | depth %@",
                            s.heavyGpuMs, s.presentGpuMs, s.logicalSize, s.physicalSize,
                            s.foveated ? "yes" : "no", s.depth ? "yes" : "no"))
                Text(String(format: "available memory %.0f MB | footprint %.0f MB", s.availableMemoryMB, s.footprintMB))
            }.font(.system(.footnote, design: .monospaced))
            GroupBox("Platform checks") {
                ForEach(model.capabilities, id: \.self) { Text($0) }
            }.font(.system(.caption, design: .monospaced))
            GroupBox("Events") {
                ForEach(Array(model.events.enumerated()), id: \.offset) { Text($0.element) }
            }.font(.system(.caption2, design: .monospaced))
        }
        .padding(28)
        .frame(width: 1100)
        .task {
            if !autoOpened && UserDefaults.standard.bool(forKey: "autoImmersive") {
                autoOpened = true
                await toggle()
            }
        }
    }

    private func toggle() async {
        if model.immersiveOpen {
            await dismissImmersiveSpace()
            model.immersiveOpen = false
        } else {
            switch await openImmersiveSpace(id: "proto") {
            case .opened: model.immersiveOpen = true
            default: model.record("Immersive space did not open.")
            }
        }
    }
}
