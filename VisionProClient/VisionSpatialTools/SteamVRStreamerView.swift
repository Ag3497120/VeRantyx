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
    @State private var isStarting = false

    init(model: VRStreamerModel) {
        self.model = model
        self.receiver = model.receiver
        self.tracking = model.tracking
        self.decoder = model.decoder
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 22) {
            VStack(alignment: .leading, spacing: 6) {
                Text("VeRantyx")
                    .font(.largeTitle.bold())
                Text("Streams SteamVR from your Mac to the Vision Pro in stereo")
                    .foregroundStyle(.secondary)
            }

            VStack(alignment: .leading, spacing: 12) {
                statusRow(title: "Mac connection", detail: receiver.connectionStatus)
                statusRow(title: "Video decode", detail: decoder.status)
                statusRow(title: "Receive fps", detail: "\(receiver.receivedVideoFPS) fps")
                statusRow(title: "Decode fps", detail: "\(decoder.decodedVideoFPS) fps")
                statusRow(title: "Receive stats", detail: receiver.receiveDiagnostics)
                statusRow(title: "Decode stats", detail: decoder.decodeDiagnostics)
                statusRow(title: "Render fps", detail: model.rendererStatus)
                statusRow(title: "Latency", detail: tracking.latencyStatus)
                statusRow(title: "Head and hand tracking", detail: tracking.status)
                statusRow(title: "Hand positions", detail: tracking.handPositionStatus)
                statusRow(title: "Controller", detail: tracking.controllerStatus)
                statusRow(title: "SteamVR input", detail: tracking.inputStatus)
                statusRow(title: "Poses sent", detail: "\(receiver.trackingPacketsSent) packets")
                statusRow(title: "Display", detail: immersiveStatus)
                statusRow(title: "Diagnostics", detail: model.immersiveDiagnostic)
            }
            .padding(18)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18))

            Toggle("Show real hands too (to check alignment with the game's hands)", isOn: $model.showsRealHands)
                .frame(maxWidth: 560)

            HStack(spacing: 14) {
                Button {
                    guard !isStarting else { return }
                    isStarting = true
                    Task {
                        await startImmersiveVR()
                    }
                } label: {
                    if isStarting {
                        ProgressView().controlSize(.small)
                        Text("Starting…")
                    } else {
                        Text("Start stereo VR")
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(isStarting)

                Button("Stop") {
                    Task {
                        await dismissImmersiveSpace()
                        model.stop()
                        immersiveStatus = String(localized: "VR view is stopped.")
                    }
                }
                .buttonStyle(.bordered)
            }

            Text("Joy-Con (recommended): pair both Joy-Con in visionOS Settings > Bluetooth, then press L and R on both at the same time to merge them into one controller. Hand tracking supplies the hand positions; the Joy-Con supply the buttons. ZL/ZR = trigger, L/R = grip (grab), sticks (with click), right A/B = A, right X/Y = B, left ↓/← = X, left ↑/→ = Y, −/+ = menu. Without Joy-Con, gestures control the game: thumb+index pinch = trigger, curl three fingers = grip, thumb+ring = A/X, thumb+little = B/Y, thumb+middle pinch and move the hand = stick.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(28)
        .frame(minWidth: 720, idealWidth: 760, minHeight: 400)
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
