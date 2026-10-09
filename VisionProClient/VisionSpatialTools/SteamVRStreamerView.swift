import SwiftUI

struct SteamVRStreamerView: View {
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @ObservedObject private var receiver: VRUDPReceiver
    @ObservedObject private var tracking: VisionTrackingManager
    @ObservedObject private var decoder: VRVideoDecoder
    @ObservedObject private var model: VRStreamerModel
    @State private var immersiveStatus = "VR表示は停止中です。"
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
                Text("MBP AVP VR")
                    .font(.largeTitle.bold())
                Text("MacのSteamVR映像をVision Proへステレオ配信します")
                    .foregroundStyle(.secondary)
            }

            VStack(alignment: .leading, spacing: 12) {
                statusRow(title: "Mac接続", detail: receiver.connectionStatus)
                statusRow(title: "映像デコード", detail: decoder.status)
                statusRow(title: "受信fps", detail: "\(receiver.receivedVideoFPS) fps")
                statusRow(title: "デコードfps", detail: "\(decoder.decodedVideoFPS) fps")
                statusRow(title: "受信状態", detail: receiver.receiveDiagnostics)
                statusRow(title: "映像処理", detail: decoder.decodeDiagnostics)
                statusRow(title: "描画fps", detail: model.rendererStatus)
                statusRow(title: "頭・手のトラッキング", detail: tracking.status)
                statusRow(title: "手の座標", detail: tracking.handPositionStatus)
                statusRow(title: "SteamVR入力", detail: tracking.inputStatus)
                statusRow(title: "姿勢送信", detail: "\(receiver.trackingPacketsSent) パケット")
                statusRow(title: "表示", detail: immersiveStatus)
                statusRow(title: "起動診断", detail: model.immersiveDiagnostic)
            }
            .padding(18)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18))

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
                        Text("起動中…")
                    } else {
                        Text("ステレオVRを開始")
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(isStarting)

                Button("停止") {
                    Task {
                        await dismissImmersiveSpace()
                        model.stop()
                        immersiveStatus = "VR表示は停止中です。"
                    }
                }
                .buttonStyle(.bordered)
            }

            Text("操作: 親指と人差し指のピンチでトリガー、親指と中指/薬指のピンチでA/B・X/Y、手を握るとグリップになります。握ったまま手を動かすとスティック入力です。起動時の頭の向きと位置を正面・目線高1.65mに合わせて再センターします。初回起動時はローカルネットワークと空間トラッキングを許可してください。MacでSteamVRとAlyxを起動すると、映像を自動で探します。")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(28)
        .frame(minWidth: 720, idealWidth: 760, minHeight: 400)
        .onAppear { model.prepare() }
    }

    private func startImmersiveVR() async {
        defer { isStarting = false }

        if model.isImmersiveSpacePresented {
            immersiveStatus = "没入VRはすでに開いています。Digital Crownで終了してから再度開始してください。"
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
                immersiveStatus = "没入VRを表示中です。映像を待っています。"
            } else {
                let trackingError = tracking.status
                await dismissImmersiveSpace()
                model.stop()
                immersiveStatus = trackingError
            }
        case .userCancelled:
            immersiveStatus = "VR表示がキャンセルされました。"
            model.recordImmersiveEvent("openImmersiveSpace returned userCancelled.")
        case .error:
            immersiveStatus = "visionOSが空間を開けませんでした。別の没入アプリが開いていればDigital Crownで終了してから再試行してください。診断欄にCompositorの状態を表示します。"
            model.recordImmersiveEvent("Immersive retry returned error. ImmersiveSpace appeared=\(model.isImmersiveSpacePresented).")
        @unknown default:
            immersiveStatus = "VR表示の状態を取得できません。"
            model.recordImmersiveEvent("openImmersiveSpace returned an unknown result.")
        }
    }

    private func statusRow(title: String, detail: String) -> some View {
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
