import SwiftUI

struct SteamVRStreamerView: View {
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.dismissWindow) private var dismissWindow
    @ObservedObject private var receiver: VRUDPReceiver
    @ObservedObject private var tracking: VisionTrackingManager
    @ObservedObject private var decoder: VRVideoDecoder
    @ObservedObject private var model: VRStreamerModel
    @State private var immersiveStatus = String(localized: "VR view is stopped.")
    @AppStorage("gamepadHold") private var gamepadHold = 1
    @AppStorage("mergedJoyConOneHanded") private var mergedJoyConOneHanded = true
    // Read by VRCompositorRenderer (UserDefaults) once a second.
    @AppStorage(VRCompositorRenderer.showFpsOverlayKey) private var showFpsOverlay = true
    @AppStorage("trackLeftHandWithCameras") private var trackLeftHandWithCameras = true
    @State private var isStarting = false
    @State private var isStopping = false
    @State private var showsHowTo = true
    @State private var showsControls = false
    @State private var showsSettings = false
    @State private var showsDetails = false

    init(model: VRStreamerModel) {
        self.model = model
        self.receiver = model.receiver
        self.tracking = model.tracking
        self.decoder = model.decoder
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                header
                connectionCard
                actionButtons
                Text(immersiveStatus)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)

                section("How to start", isExpanded: $showsHowTo) { howToStart }
                section("Controls", isExpanded: $showsControls) { controlsHelp }
                section("Settings", isExpanded: $showsSettings) { settings }
                section("Details", isExpanded: $showsDetails) { details }
            }
            .padding(28)
        }
        .frame(minWidth: 680, idealWidth: 720, minHeight: 640, idealHeight: 760)
        .onAppear {
            model.prepare()
            // visionOS reopens this window when the wearer returns to the app
            // after Home View or Control Center covered the game. The VR space
            // is still open then, so go straight back to it.
            if model.isImmersiveSpacePresented {
                model.recordImmersiveEvent("Window reopened while VR is still open; returning to VR.")
                dismissWindow(id: VRStreamerApp.mainWindowID)
            }
        }
    }

    // MARK: - Parts

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(verbatim: "VeRantyx")
                .font(.largeTitle.bold())
            Text("Play PC VR games from your Mac in Vision Pro")
                .foregroundStyle(.secondary)
        }
    }

    /// Green while video arrives, orange otherwise; the three numbers that
    /// matter during play, and the full status line.
    private var connectionCard: some View {
        let receiving = receiver.receivedVideoFPS > 0
        return VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 10) {
                Circle()
                    .fill(receiving ? Color.green : Color.orange)
                    .frame(width: 12, height: 12)
                    .accessibilityHidden(true)
                Text(receiving ? "Receiving from your Mac" : "Waiting for your Mac")
                    .font(.headline)
            }
            Text(receiver.connectionStatus)
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            HStack(alignment: .top, spacing: 28) {
                metric("Game fps", receiver.gameFrameStatus)
                metric("Received", String(localized: "\(receiver.receivedVideoFPS) fps"))
                metric("Latency", tracking.latencyStatus)
            }
        }
        .padding(18)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18))
    }

    private var actionButtons: some View {
        HStack(spacing: 14) {
            Button {
                guard !isStarting, !isStopping else { return }
                isStarting = true
                Task { await startImmersiveVR() }
            } label: {
                HStack(spacing: 8) {
                    if isStarting {
                        ProgressView().controlSize(.small)
                        Text("Starting…")
                    } else {
                        Image(systemName: "visionpro")
                        Text("Start stereo VR")
                    }
                }
                .padding(.horizontal, 8)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .disabled(isStarting || isStopping)

            Button("Stop") {
                guard !isStopping else { return }
                isStopping = true
                Task {
                    defer { isStopping = false }
                    if model.isImmersiveSpacePresented { await dismissImmersiveSpace() }
                    model.stop()
                    immersiveStatus = String(localized: "VR view is stopped.")
                }
            }
            .buttonStyle(.bordered)
            .controlSize(.large)
            .disabled(isStarting || isStopping)
        }
    }

    private var howToStart: some View {
        VStack(alignment: .leading, spacing: 8) {
            step(1, "On your Mac, start VeRantyx (see the setup guide on GitHub: github.com/Ag3497120/VeRantyx).")
            step(2, "Keep this Vision Pro on the same Wi-Fi network as the Mac.")
            step(3, "Press Start stereo VR. The game appears as soon as the Mac starts streaming.")
            step(4, "To leave VR, press the Digital Crown. This window comes back.")
        }
    }

    private var controlsHelp: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Joy-Con")
                .font(.headline)
            Text("Pair each Joy-Con in Settings > Bluetooth. Use them separately (one per hand, each with its own motion sensors), or press L and R on both to join them into one controller. Hand tracking supplies where your hands are; the Joy-Con supply the buttons.")
            Text("ZL/ZR = trigger, L/R = grip (grab), sticks (with click), right A/B = A, right X/Y = B, left ↓/← = X, left ↑/→ = Y, −/+ = menu.")
            Text("Hands only")
                .font(.headline)
                .padding(.top, 4)
            Text("Thumb+index pinch = trigger, curl three fingers = grip, thumb+ring = A/X, thumb+little = B/Y, thumb+middle pinch and move the hand = stick.")
        }
        .font(.callout)
        .foregroundStyle(.secondary)
        .fixedSize(horizontal: false, vertical: true)
    }

    private var settings: some View {
        VStack(alignment: .leading, spacing: 14) {
            Toggle("Show real hands too (to check alignment with the game's hands)", isOn: $model.showsRealHands)
            Toggle("Hold the merged Joy-Con pair in the left hand (right hand free)", isOn: $mergedJoyConOneHanded)
            Toggle("Show the fps overlay in VR (game / received / latency / audio)", isOn: $showFpsOverlay)
            Toggle("Track the left hand with the cameras as well (fixed gamepad pose when it is out of view)", isOn: $trackLeftHandWithCameras)
            Picker("One-handed gamepad hold", selection: $gamepadHold) {
                Text("Upright").tag(0)
                Text("Turned left").tag(1)
                Text("Turned right").tag(2)
            }
            .pickerStyle(.segmented)
        }
    }

    private var details: some View {
        VStack(alignment: .leading, spacing: 12) {
            statusRow(title: "Mac connection", detail: receiver.connectionStatus)
            statusRow(title: "Video decode", detail: decoder.status)
            statusRow(title: "Game fps", detail: receiver.gameFrameStatus)
            statusRow(title: "Receive fps", detail: String(localized: "\(receiver.receivedVideoFPS) fps"))
            statusRow(title: "Decode fps", detail: String(localized: "\(decoder.decodedVideoFPS) fps"))
            statusRow(title: "Receive stats", detail: receiver.receiveDiagnostics)
            statusRow(title: "Decode stats", detail: decoder.decodeDiagnostics)
            statusRow(title: "Render fps", detail: model.rendererStatus)
            statusRow(title: "Latency", detail: tracking.latencyStatus)
            statusRow(title: "Head and hand tracking", detail: tracking.status)
            statusRow(title: "Hand positions", detail: tracking.handPositionStatus)
            statusRow(title: "Controller", detail: tracking.controllerStatus)
            statusRow(title: "SteamVR input", detail: tracking.inputStatus)
            statusRow(title: "Poses sent", detail: String(localized: "\(receiver.trackingPacketsSent) packets"))
            statusRow(title: "Display", detail: immersiveStatus)
            statusRow(title: "Diagnostics", detail: model.immersiveDiagnostic)
        }
    }

    private func section<Content: View>(_ title: LocalizedStringKey, isExpanded: Binding<Bool>,
                                        @ViewBuilder content: () -> Content) -> some View {
        let body = content()
        return DisclosureGroup(isExpanded: isExpanded) {
            body
                .padding(.top, 10)
                .frame(maxWidth: .infinity, alignment: .leading)
        } label: {
            Text(title).font(.title3.weight(.semibold))
        }
        .padding(18)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18))
    }

    private func metric(_ title: LocalizedStringKey, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .font(.caption)
                .foregroundStyle(.secondary)
            Text(value)
                .font(.title3.monospacedDigit())
                .lineLimit(2)
        }
    }

    private func step(_ number: Int, _ text: LocalizedStringKey) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text(verbatim: "\(number).")
                .font(.headline)
                .frame(width: 22, alignment: .trailing)
            Text(text)
                .fixedSize(horizontal: false, vertical: true)
        }
        .font(.callout)
    }

    // MARK: - Immersive space

    private func startImmersiveVR() async {
        defer { isStarting = false }

        if model.isImmersiveSpacePresented {
            immersiveStatus = String(localized: "The immersive VR view is already open. Press the Digital Crown to close it, then start again.")
            return
        }

        model.recordImmersiveEvent("Requested openImmersiveSpace(id: SteamVR).")
        var result = await openImmersiveSpace(id: "SteamVR")

        if case .error = result {
            model.recordImmersiveEvent("First immersive open returned error; resetting this app's immersive space and retrying.")
            await dismissImmersiveSpace()
            try? await Task.sleep(nanoseconds: 500_000_000)
            model.recordImmersiveEvent("Retrying openImmersiveSpace(id: SteamVR).")
            result = await openImmersiveSpace(id: "SteamVR")
        }

        switch result {
        case .opened:
            model.recordImmersiveEvent("openImmersiveSpace returned opened.")
            if await model.start() {
                immersiveStatus = String(localized: "Immersive VR is showing. See “Receive fps” and “Render fps” for the video state.")
                // Pinches meant for the game must not land on this window's
                // buttons (Stop would end immersion). It comes back when the
                // Digital Crown closes the immersive space.
                model.recordImmersiveEvent("Closing the main window during immersion; the Digital Crown brings it back.")
                dismissWindow(id: VRStreamerApp.mainWindowID)
            } else {
                let trackingError = tracking.status
                await dismissImmersiveSpace()
                model.stop()
                immersiveStatus = trackingError
            }
        case .userCancelled:
            immersiveStatus = String(localized: "The VR view was cancelled.")
            model.recordImmersiveEvent("openImmersiveSpace returned userCancelled.")
        case .error:
            immersiveStatus = String(localized: "visionOS could not open the space. If another immersive app is open, close it with the Digital Crown and try again. The Diagnostics row shows the compositor state.")
            model.recordImmersiveEvent("Immersive retry returned error. ImmersiveSpace appeared=\(model.isImmersiveSpacePresented).")
        @unknown default:
            immersiveStatus = String(localized: "The VR view state is unknown.")
            model.recordImmersiveEvent("openImmersiveSpace returned an unknown result.")
        }
    }

    private func statusRow(title: LocalizedStringKey, detail: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 14) {
            Text(title)
                .font(.headline)
                .frame(width: 170, alignment: .leading)
            Text(detail)
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}
