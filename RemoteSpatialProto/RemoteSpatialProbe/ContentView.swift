import AppKit
import SwiftUI

struct ContentView: View {
    @Environment(AppModel.self) private var appModel
    @Environment(\.supportsRemoteScenes) private var supportsRemoteScenes
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace

    @State private var autoFlash = false
    @State private var poseDelayMs = 0.0
    @State private var keyMonitor: Any?

    var body: some View {
        let s = appModel.stats
        VStack(alignment: .leading, spacing: 12) {
            Text("RemoteSpatialProbe").font(.title2.bold())
            Text("supportsRemoteScenes: \(supportsRemoteScenes ? "yes" : "no")")
                .foregroundStyle(supportsRemoteScenes ? Color.primary : Color.red)

            HStack {
                Button("Open remote immersive space") { open() }
                    .disabled(!supportsRemoteScenes || appModel.immersiveSpaceState != .closed)
                Button("Close") { close() }
                    .disabled(appModel.immersiveSpaceState == .closed)
                Text("space: \(appModel.immersiveSpaceState.rawValue)  open result: \(appModel.lastOpenResult)")
                    .foregroundStyle(.secondary)
            }

            HStack {
                Button("Flash (space bar)") { appModel.control.requestFlash() }
                Toggle("Auto-flash every 1 s", isOn: $autoFlash)
                    .onChange(of: autoFlash) { appModel.control.autoFlash = autoFlash }
            }
            HStack {
                Text("Pose delay (reprojection test): \(Int(poseDelayMs)) ms")
                Slider(value: $poseDelayMs, in: 0...200, step: 10)
                    .frame(width: 200)
                    .onChange(of: poseDelayMs) {
                        appModel.control.poseDelayMs = poseDelayMs
                        ProbeLog.shared.note("pose delay set to \(Int(poseDelayMs)) ms")
                    }
            }

            GroupBox("Live") {
                Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
                    row("layer / ARKit", "\(s.layerState) / \(s.arState)")
                    row("frames rendered", "\(s.framesRendered)")
                    row("late (GPU done after renderingDeadline)", "\(s.framesLate)")
                    row("skipped compositor frames", "\(s.framesSkipped)")
                    row("frames without drawable", "\(s.framesWithoutDrawable)")
                    row("frame interval (median)", String(format: "%.2f ms", s.frameIntervalMs))
                    row("input → predicted presentation", String(format: "%.2f ms", s.inputToPresentMs))
                    row("query → predicted presentation", String(format: "%.2f ms", s.queryToPresentMs))
                    row("GPU time (median)", String(format: "%.2f ms", s.gpuMs))
                    row("device anchor", String(format: "%.0f %% of frames, tracked: %@", s.anchorFraction * 100, s.anchorTracked ? "yes" : "no"))
                    row("head position", String(format: "(%.2f, %.2f, %.2f) m", s.head.x, s.head.y, s.head.z))
                    row("hand tracking", "not available on macOS (SDK); pinch events: \(s.spatialEvents)")
                    row("last spatial event", s.lastSpatialEvent)
                    row("last flash", s.lastFlash)
                }
                .font(.system(.body, design: .monospaced))
            }

            GroupBox("Capabilities (from makeConfiguration)") {
                VStack(alignment: .leading) {
                    ForEach(appModel.capabilities, id: \.self) { Text($0) }
                    if appModel.capabilities.isEmpty { Text("open the space to query").foregroundStyle(.secondary) }
                }
                .font(.system(.caption, design: .monospaced))
                .frame(maxWidth: .infinity, alignment: .leading)
            }

            Text("Logs: \(ProbeLog.shared.directory.path)")
                .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
        }
        .padding()
        .frame(minWidth: 640)
        .onAppear(perform: installSpaceBar)
        .onDisappear {
            if let keyMonitor { NSEvent.removeMonitor(keyMonitor) }
        }
    }

    private func row(_ label: String, _ value: String) -> some View {
        GridRow {
            Text(label).foregroundStyle(.secondary)
            Text(value)
        }
    }

    /// Space bar anywhere in the app (the window keeps key focus while the
    /// headset shows the space) requests a flash.
    private func installSpaceBar() {
        guard keyMonitor == nil else { return }
        let control = appModel.control
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
            if event.keyCode == 49 && !event.isARepeat {
                control.requestFlash()
                return nil
            }
            return event
        }
    }

    private func open() {
        Task { @MainActor in
            appModel.immersiveSpaceState = .inTransition
            ProbeLog.shared.note("openImmersiveSpace requested")
            let result = await openImmersiveSpace(id: appModel.immersiveSpaceID)
            appModel.lastOpenResult = "\(result)"
            ProbeLog.shared.note("openImmersiveSpace result: \(result)")
            switch result {
            case .opened:
                break // the render loop sets .open once the layer runs
            default:
                appModel.immersiveSpaceState = .closed
            }
        }
    }

    private func close() {
        Task { @MainActor in
            appModel.immersiveSpaceState = .inTransition
            ProbeLog.shared.note("dismissImmersiveSpace requested")
            await dismissImmersiveSpace()
            ProbeLog.shared.flush()
        }
    }
}
