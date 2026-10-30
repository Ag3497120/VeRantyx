// The window: Get started (the checks and the setup, in order), Play, Headset and Help.
import AppKit
import SwiftUI

struct RootView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        NavigationSplitView {
            List(selection: Binding<Page?>(get: { model.page }, set: { if let p = $0 { model.page = p } })) {
                ForEach(Page.allCases) { page in
                    Label(page.name, systemImage: page.icon)
                        .badge(page == .setup && model.setupSteps.done < model.setupSteps.of ? Text("\(model.setupSteps.done)/\(model.setupSteps.of)") : nil)
                        .tag(page)
                }
            }
            .navigationSplitViewColumnWidth(min: 190, ideal: 210)
            .safeAreaInset(edge: .bottom) { SidebarStatus() }
        } detail: {
            Group {
                switch model.page {
                case .setup: SetupView()
                case .play: PlayView()
                case .headset: HeadsetView()
                case .help: HelpView()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .safeAreaInset(edge: .bottom) { JobBar() }
        }
        .sheet(isPresented: $model.showConsole) { ConsoleView() }
        .sheet(isPresented: Binding(get: { model.importChoice != nil }, set: { if !$0 { model.importChoice = nil } })) { ImportSheet() }
        .alert(T("Something went wrong", "うまくいきませんでした"), isPresented: Binding(get: { model.error != nil }, set: { if !$0 { model.error = nil } })) {
            Button("OK") { model.error = nil }
        } message: { Text(model.error ?? "") }
    }
}

// MARK: - Pieces

struct Card<Content: View>: View {
    var number: Int? = nil
    var title: String
    var done: Bool? = nil
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                if let number {
                    ZStack {
                        Circle().fill(done == true ? Color.green.opacity(0.18) : Color.accentColor.opacity(0.14))
                        if done == true { Image(systemName: "checkmark").font(.system(size: 12, weight: .bold)).foregroundStyle(.green) }
                        else { Text("\(number)").font(.system(size: 13, weight: .bold)).foregroundStyle(Color.accentColor) }
                    }
                    .frame(width: 26, height: 26)
                }
                Text(title).font(.title3.weight(.semibold))
                Spacer()
            }
            content
        }
        .padding(18)
        .background(RoundedRectangle(cornerRadius: 14).fill(Color(nsColor: .controlBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(Color.primary.opacity(0.08)))
    }
}

struct Check<Trailing: View>: View {
    var ok: Bool?
    var text: String
    var note: String? = nil
    @ViewBuilder var trailing: Trailing

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Group {
                switch ok {
                case .some(true): Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                case .some(false): Image(systemName: "xmark.circle.fill").foregroundStyle(.orange)
                case .none: ProgressView().controlSize(.small)
                }
            }
            .frame(width: 18)
            VStack(alignment: .leading, spacing: 2) {
                Text(text)
                if let note { Text(note).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
            }
            Spacer(minLength: 12)
            trailing
        }
    }
}

extension Check where Trailing == EmptyView {
    init(ok: Bool?, text: String, note: String? = nil) { self.init(ok: ok, text: text, note: note) { EmptyView() } }
}

struct Chip: View {
    var text: String
    var color: Color
    var body: some View {
        Text(text).font(.caption.weight(.semibold)).padding(.horizontal, 8).padding(.vertical, 3)
            .foregroundStyle(color).background(Capsule().fill(color.opacity(0.13)))
    }
}

struct PageView<Content: View>: View {
    var title: String
    var subtitle: String? = nil
    @ViewBuilder var content: Content
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                VStack(alignment: .leading, spacing: 4) {
                    Text(title).font(.largeTitle.weight(.bold))
                    if let subtitle { Text(subtitle).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
                }
                .padding(.bottom, 4)
                content
            }
            .padding(28)
            .frame(maxWidth: 820, alignment: .leading)
            .frame(maxWidth: .infinity)
        }
    }
}

struct JobBar: View {
    @Environment(AppModel.self) private var model
    var body: some View {
        if let job = model.job {
            HStack(spacing: 14) {
                if let p = job.progress { ProgressView(value: p).frame(width: 180) } else { ProgressView().controlSize(.small) }
                VStack(alignment: .leading, spacing: 1) {
                    Text(job.title).font(.callout.weight(.medium))
                    if !job.detail.isEmpty || job.progress != nil {
                        Text([job.detail, job.progress.map { String(format: "%.0f %%", $0 * 100) } ?? ""].filter { !$0.isEmpty }.joined(separator: " · "))
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                Spacer()
                if model.console.running && !model.showConsole { Button(T("Show steamcmd", "steamcmd を表示")) { model.showConsole = true } }
                if model.setupDownloading { Button(T("Cancel", "キャンセル")) { model.cancelSetupDownload() } }
            }
            .padding(.horizontal, 20).padding(.vertical, 10)
            .background(.bar)
        }
    }
}

struct SidebarStatus: View {
    @Environment(AppModel.self) private var model
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let play = model.play, play.running {
                Label(play.title, systemImage: "gamecontroller.fill").font(.callout.weight(.semibold)).lineLimit(1)
                Text(play.headsetConnected ? T("Headset connected", "ヘッドセット接続中") : T("Waiting for the headset", "ヘッドセットを待っています"))
                    .font(.caption).foregroundStyle(.secondary)
            }
            HStack(spacing: 6) {
                Circle().fill(model.steamRunning ? Color.green : Color.orange).frame(width: 7, height: 7)
                Text(model.steamRunning ? T("Steam is running", "Steam 起動中") : T("Steam is not running", "Steam は起動していません")).font(.caption).foregroundStyle(.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
    }
}

// MARK: - Get started

struct SetupView: View {
    @Environment(AppModel.self) private var model
    @State private var showWindowsGames = false

    var body: some View {
        @Bindable var model = model
        ScrollViewReader { scroll in
            PageView(title: T("Get started", "はじめに"),
                     subtitle: T("Choose how games run, then prepare their files. Games stream from this Mac to your headset.", "ゲームの起動方法を選び、ファイルを準備します。この Mac からヘッドセットへ配信します。")) {
                Card(number: 1, title: T("This Mac", "この Mac"), done: model.appleSilicon && model.macOSOK) {
                    Check(ok: model.appleSilicon, text: T("Apple silicon (M1 or later)", "Apple silicon（M1 以降）"))
                    Check(ok: model.macOSOK, text: T("macOS 14.2 or later", "macOS 14.2 以降"), note: model.macOSOK ? nil : T("Update macOS in System Settings.", "システム設定から macOS を更新してください。"))
                }
                Card(number: 2, title: T("Game launch mode", "ゲームの起動モード"), done: model.launchModeReady) {
                    Picker(T("Game launch mode", "ゲームの起動モード"), selection: Binding(get: { model.effectivePreference }, set: { model.chooseBackend($0) })) {
                        ForEach(GameBackend.allCases) { Text($0.name).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .disabled(model.configurationLocked)
                    Text(model.effectivePreference == .nativeARM64
                         ? T("Uses Native ARM64 for supported games. Other games use Windows and are marked below. Both installations are kept when you switch modes.", "対応ゲームはネイティブ ARM64 で動作します。ほかのゲームは Windows と表示されます。モードを切り替えても両方のインストールを保持します。")
                         : T("Uses Windows game versions through VXRuntime. Native installations are kept when you switch modes.", "Windows 版を VXRuntime で起動します。モードを切り替えてもネイティブ版は保持します。"))
                        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    if model.setupNeedsWindows { RosettaPreparationCheck() }
                }
                Card(number: 3, title: "Steam for Mac", done: model.steamInstalled && model.steamRunning) {
                    Check(ok: model.steamInstalled, text: T("Installed", "インストール済み"),
                          note: T("The games use the Steam you sign in to on this Mac, with your own account.", "ゲームはこの Mac でサインインした Steam（あなたのアカウント）を使います。")) {
                        if !model.steamInstalled { Button(T("Get Steam", "Steam を入手")) { NSWorkspace.shared.open(URL(string: "https://store.steampowered.com/about/")!) } }
                    }
                    Check(ok: model.steamRunning, text: T("Running and signed in", "起動してサインイン済み"),
                          note: model.steamRunning ? nil : T("Open Steam and sign in with the account that owns your games. Keep it open while you play.", "Steam を開き、ゲームを持っているアカウントでサインインしてください。プレイ中は開いたままにします。")) {
                        if model.steamInstalled && !model.steamRunning { Button(T("Open Steam", "Steam を開く")) { Steam.open() } }
                    }
                }
                Card(number: 4, title: T("Where the games go", "ゲームの置き場所"), done: model.freeBytes != nil) {
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(model.drive.path).font(.system(.body, design: .monospaced)).lineLimit(1).truncationMode(.middle)
                            Text(model.freeBytes.map { TF("%@ free", "空き %@", gigabytes($0)) } ?? "").font(.callout).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button(T("Change…", "変更…")) { model.chooseDrive() }.disabled(model.configurationLocked)
                    }
                    Text(T("An external SSD works too. Compatible existing game content is reused where possible; preparation may need extra space on disks without file cloning.", "外付け SSD も使えます。互換性のある既存データは可能な範囲で再利用します。ファイルクローン非対応のディスクでは準備に追加容量が必要です。"))
                        .font(.callout).foregroundStyle(.secondary)
                }
                Card(number: 5, title: T("Games", "ゲーム"), done: model.setupGamesDone) {
                    if model.effectivePreference == .nativeARM64 {
                        ForEach(model.primarySetupTitles) { SetupGameRow(title: $0).id($0.id) }
                        DisclosureGroup(T("Windows games", "Windows のゲーム"), isExpanded: $showWindowsGames) {
                            Text(T("These games use Windows. Select them to prepare their Windows components too.", "これらのゲームは Windows 版を使います。選択すると Windows 用の部品も準備します。"))
                                .font(.caption).foregroundStyle(.secondary)
                            ForEach(model.setupTitles.filter { $0.nativeSupport == nil }) { SetupGameRow(title: $0).id($0.id) }
                        }
                    } else {
                        ForEach(model.setupTitles) { SetupGameRow(title: $0).id($0.id) }
                    }
                    if model.setupNeedsWindows {
                        Check(ok: model.steamVR, text: "SteamVR", note: T("Prepared automatically for the selected Windows VR games.", "選択した Windows VR ゲーム用に自動で準備します。"))
                    }
                    Divider()
                    Text(T("Download with your Steam account", "Steam アカウントでダウンロード")).fontWeight(.semibold)
                    HStack {
                        TextField(T("Steam account name (not the e-mail)", "Steam のアカウント名（メールアドレスではなく）"), text: $model.account)
                            .textFieldStyle(.roundedBorder).frame(maxWidth: 300)
                        Button(T("Download", "ダウンロード")) { Task { await model.download() } }
                            .buttonStyle(.borderedProminent)
                            .disabled(model.configurationLocked || model.account.trimmingCharacters(in: .whitespaces).isEmpty || model.installationPlan(for: model.wanted).isEmpty)
                    }
                    Text(T("Valve's steamcmd downloads them. It asks for your Steam password and Steam Guard code once, in the window that opens; it remembers the sign-in itself. VeRantyx keeps neither.",
                           "Valve の steamcmd がダウンロードします。最初の一回だけ、開くウィンドウで Steam のパスワードと Steam Guard コードを聞かれます（サインインは steamcmd 自身が覚えます）。VeRantyx はどちらも保存しません。"))
                        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    Divider()
                    HStack(alignment: .top) {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(T("Or copy them from a Steam library you have", "または手元の Steam ライブラリからコピー")).fontWeight(.semibold)
                            Text(T("Import Windows game files; native preparation can reuse compatible content after checking its version.", "Windows のゲームファイルを取り込みます。バージョンを確認して互換性があれば、ネイティブ版の準備で再利用します。"))
                                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                        }
                        Spacer()
                        Button(T("Choose a folder…", "フォルダを選ぶ…")) { model.chooseImport() }.disabled(model.configurationLocked)
                    }
                }
                if model.flow != nil { FlowCard() }
                Card(number: 6, title: model.effectivePreference == .nativeARM64 ? T("Native ARM64 components", "ネイティブ ARM64 の部品") : T("Windows runtime files", "Windows のランタイムファイル"), done: model.setupComponentsReady) {
                    if model.effectivePreference == .nativeARM64 {
                        ForEach(model.primarySetupTitles) { title in
                            Check(ok: title.nativeSupport?.layout(drive: model.drive, runtime: Paths.runtime).bundled == true,
                                  text: title.name + " · Native ARM64")
                        }
                        Check(ok: model.primarySetupTitles.contains { model.route(forID: $0.id) == .alvr }, text: "ALVR (Pico / Quest)",
                              note: T("Native ARM64 uses ALVR. Choose ALVR on the Headset page or this game's route.", "ネイティブ ARM64 は ALVR を使います。ヘッドセットページかゲームの経路で ALVR を選んでください。")) {
                            Button(T("Headset", "ヘッドセット")) { model.page = .headset }
                        }
                    } else { WindowsRuntimeChecks() }
                }
                if model.effectivePreference == .nativeARM64 && model.setupNeedsWindows {
                    Card(title: T("Windows runtime files", "Windows のランタイムファイル"), done: model.windowsRuntimeReady) {
                        Text(T("Needed for the Windows games selected above.", "上で選択した Windows ゲームに必要です。"))
                            .font(.callout).foregroundStyle(.secondary)
                        WindowsRuntimeChecks()
                    }
                }
                if model.setupNeedsWindows && (model.windowsSteamInstalled || model.setupFocusID.flatMap(Title.named)?.windowsSteam == true) { WindowsSteamCard() }
                if model.ready {
                    HStack {
                        Image(systemName: "checkmark.seal.fill").foregroundStyle(.green).font(.title2)
                        Text(T("Ready. Choose your headset, then play.", "準備ができました。ヘッドセットを選んで、プレイしましょう。")).font(.headline)
                        Spacer()
                        Button(T("Headset", "ヘッドセット")) { model.page = .headset }
                        Button(T("Play", "プレイ")) { model.page = .play }.buttonStyle(.borderedProminent)
                    }
                    .padding(16).background(RoundedRectangle(cornerRadius: 14).fill(Color.green.opacity(0.08)))
                }
            }
            .onAppear { revealFocusedGame(scroll) }
            .onChange(of: model.setupFocusID) { _, _ in revealFocusedGame(scroll) }
        }
    }
    private func revealFocusedGame(_ scroll: ScrollViewProxy) {
        guard let id = model.setupFocusID else { return }
        if model.backend(forID: id) == .vxrt { showWindowsGames = true }
        DispatchQueue.main.async { withAnimation { scroll.scrollTo(id, anchor: .center) } }
    }
}

struct GameRuntimeBadge: View {
    @Environment(AppModel.self) private var model
    var id: String
    var body: some View {
        let backend = model.backend(forID: id)
        Chip(text: backend.name, color: backend == .nativeARM64 ? .blue : .secondary)
    }
}

struct SetupGameRow: View {
    @Environment(AppModel.self) private var model
    var title: Title
    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            if model.isInstalled(title.id) {
                Image(systemName: "checkmark.circle.fill").foregroundStyle(.green).frame(width: 18)
            } else if title.source == .windowsSteam && model.backend(forID: title.id) == .vxrt {
                Image(systemName: "circle.dashed").foregroundStyle(.secondary).frame(width: 18)
            } else {
                Toggle("", isOn: Binding(get: { model.wanted.contains(title.id) }, set: { if $0 { model.wanted.insert(title.id) } else { model.wanted.remove(title.id) } }))
                    .labelsHidden().toggleStyle(.checkbox).frame(width: 18).disabled(model.configurationLocked)
            }
            VStack(alignment: .leading, spacing: 5) {
                HStack {
                    Text(title.name).fontWeight(.medium)
                    GameRuntimeBadge(id: title.id)
                    if title.free { Chip(text: T("Free", "無料"), color: .blue) }
                }
                Text(model.summary(for: title)).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                if model.backend(forID: title.id) == .nativeARM64, model.isInstalled(title.id) {
                    if let reason = model.nativeReason(for: title) { Text(reason).font(.caption).foregroundStyle(.orange) }
                    Button(T("Update / repair native files", "ネイティブファイルを更新・修復")) { model.getAndPlay(title, flat: false, reinstall: true, startAfterInstall: false) }
                        .disabled(model.configurationLocked)
                }
                if title.source == .windowsSteam && model.backend(forID: title.id) == .vxrt && !model.isInstalled(title.id) {
                    Button(T("Download selected version", "選択したバージョンをダウンロード")) { model.getAndPlay(title, flat: false, startAfterInstall: false) }
                        .disabled(model.configurationLocked)
                }
            }
            Spacer(minLength: 4)
            Text(model.isInstalled(title.id) ? T("Installed", "インストール済み") : gigabytes(model.estimatedBytes(for: title)))
                .font(.callout).foregroundStyle(.secondary)
        }
        .padding(.vertical, 5)
    }
}

struct RosettaPreparationCheck: View {
    @Environment(AppModel.self) private var model
    var body: some View {
        Check(ok: model.rosetta, text: "Rosetta 2",
              note: T("Runs the games' Windows (x86) code. Installed by macOS itself; it asks for your password.", "ゲームの Windows（x86）コードを動かします。macOS 自身がインストールし、パスワードを聞かれます。")) {
            if model.rosetta == false { Button(T("Install Rosetta 2", "Rosetta 2 をインストール")) { Task { await model.installRosetta() } }.disabled(model.configurationLocked) }
        }
    }
}

struct WindowsRuntimeChecks: View {
    @Environment(AppModel.self) private var model
    var body: some View {
        Check(ok: model.redistCount > 0, text: model.redistCount > 0 ? TF("%@ files ready", "%@ 個のファイルを用意済み", "\(model.redistCount)") : T("Not gathered yet", "まだ集めていません"),
              note: T("Microsoft's runtime DLLs the games need, taken from the installers SteamVR and the games bring with them. Done by itself after a download or copy.",
                      "ゲームが必要とする Microsoft のランタイム DLL です。SteamVR やゲームに付いてくるインストーラーから取り出します。ダウンロードやコピーの後に自動で行います。")) {
            Button(model.redistCount > 0 ? T("Gather again", "集め直す") : T("Gather", "集める")) { Task { await model.collect() } }
                .disabled(model.configurationLocked || !model.steamVR)
        }
    }
}

struct ImportSheet: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var chosen: Set<String> = []

    var body: some View {
        if let choice = model.importChoice {
            VStack(alignment: .leading, spacing: 14) {
                Text(T("Copy into VeRantyx", "VeRantyx へコピー")).font(.title2.weight(.semibold))
                Text(choice.library.path).font(.system(.callout, design: .monospaced)).foregroundStyle(.secondary).lineLimit(2)
                List(choice.apps) { app in
                    Toggle(isOn: Binding(get: { chosen.contains(app.id) }, set: { if $0 { chosen.insert(app.id) } else { chosen.remove(app.id) } })) {
                        HStack {
                            Text(app.name.isEmpty ? app.id : app.name)
                            if model.isInstalled(app.id, backend: .vxrt) { Chip(text: T("Already here", "入っています"), color: .green) }
                            Spacer()
                            Text(gigabytes(app.sizeOnDisk)).foregroundStyle(.secondary)
                        }
                    }
                    .disabled(model.isInstalled(app.id, backend: .vxrt))
                }
                .frame(minHeight: 220)
                HStack {
                    Spacer()
                    Button(T("Cancel", "キャンセル")) { model.importChoice = nil; dismiss() }
                    Button(T("Copy", "コピー")) {
                        let apps = choice.apps.filter { chosen.contains($0.id) }
                        Task { await model.importApps(apps, from: choice.library) }
                    }
                    .buttonStyle(.borderedProminent).disabled(chosen.isEmpty)
                }
            }
            .padding(22)
            .frame(width: 560)
            .onAppear {
                chosen = Set(choice.apps.map(\.id).filter { Title.named($0).map { !$0.windowsSteam } == true && !model.isInstalled($0, backend: .vxrt) })
            }
        }
    }
}

struct ConsoleView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.dismiss) private var dismiss
    @State private var input = ""
    @FocusState private var focused: Bool

    var body: some View {
        let console = model.console
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("steamcmd").font(.title2.weight(.semibold))
                Text(T("Valve's Steam downloader", "Valve の Steam ダウンローダー")).foregroundStyle(.secondary)
                Spacer()
                if let p = console.progress, console.running {
                    ProgressView(value: p).frame(width: 160)
                    Text(String(format: "%.0f %%", p * 100)).monospacedDigit().foregroundStyle(.secondary)
                }
            }
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 1) {
                        ForEach(Array((console.lines + [console.partial]).enumerated()), id: \.offset) { _, line in
                            Text(line.isEmpty ? " " : line).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                        }
                        Color.clear.frame(height: 1).id("end")
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(10)
                }
                .background(RoundedRectangle(cornerRadius: 8).fill(Color(nsColor: .textBackgroundColor)))
                .onChange(of: console.lines.count) { proxy.scrollTo("end", anchor: .bottom) }
                .onChange(of: console.partial) { proxy.scrollTo("end", anchor: .bottom) }
            }
            if console.phoneConfirmation {
                Label(T("Approve the sign-in in the Steam Mobile app on your phone.", "スマートフォンの Steam モバイルアプリでサインインを承認してください。"), systemImage: "iphone")
                    .padding(10).frame(maxWidth: .infinity, alignment: .leading)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Color.accentColor.opacity(0.1)))
            }
            if let ask = console.asking {
                VStack(alignment: .leading, spacing: 6) {
                    switch ask {
                    case .secret(let label):
                        Text(label).fontWeight(.medium)
                        HStack {
                            SecureField(label, text: $input).textFieldStyle(.roundedBorder).focused($focused).onSubmit(send)
                            Button(T("Send", "送信"), action: send).buttonStyle(.borderedProminent).disabled(input.isEmpty)
                        }
                        Text(T("Goes to steamcmd only; VeRantyx does not keep it.", "steamcmd にだけ渡り、VeRantyx は保存しません。")).font(.caption).foregroundStyle(.secondary)
                    case .code(let label):
                        Text(label).fontWeight(.medium)
                        HStack {
                            TextField("ABC12", text: $input).textFieldStyle(.roundedBorder).focused($focused).onSubmit(send)
                            Button(T("Send", "送信"), action: send).buttonStyle(.borderedProminent).disabled(input.isEmpty)
                        }
                    }
                }
                .padding(12)
                .background(RoundedRectangle(cornerRadius: 10).fill(Color.accentColor.opacity(0.08)))
                .onAppear { focused = true }
            }
            HStack {
                if console.running {
                    Text(T("You can close this window; the download goes on.", "このウィンドウを閉じてもダウンロードは続きます。")).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                if console.running { Button(T("Stop the download", "ダウンロードを中止")) { console.cancel() } }
                Button(T("Close", "閉じる")) { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(20)
        .frame(width: 760, height: 520)
    }

    private func send() {
        guard !input.isEmpty else { return }
        model.console.send(input)
        input = ""                                        // (dropped: kept nowhere)
    }
}

// MARK: - Play

struct PlayView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        PageView(title: T("Play", "プレイ")) {
            if !model.playSetupReady {
                Banner(icon: "checklist", text: T("Finish Get started first.", "先に「はじめに」を済ませてください。"), action: (T("Get started", "はじめに"), { model.page = .setup }))
            } else if !model.steamRunning {
                Banner(icon: "exclamationmark.triangle", text: T("Open Steam and sign in: the games need it.", "Steam を開いてサインインしてください。ゲームに必要です。"), action: (T("Open Steam", "Steam を開く"), { Steam.open() }))
            }
            if !model.elsewhereBesidesWindowsSteam.isEmpty {
                Banner(icon: "terminal", text: T("A game or SteamVR started elsewhere (Terminal) is running.", "別の場所（ターミナル）で起動したゲームか SteamVR が動いています。"),
                       action: (T("Stop it", "停止する"), { Task { await Play.stopEverything() } }))
            }
            if let play = model.play { SessionCard(play: play) }
            if model.flow != nil { FlowCard() }
            if model.windowsSteamInstalled || model.games.contains(where: \.viaWindowsSteam) { WindowsSteamCard() }

            Card(title: T("Settings for this session", "今回の設定")) {
                HStack {
                    Label(model.headset.name, systemImage: model.headset == .quest ? "visionpro" : "visionpro.fill")
                    if model.headset == .quest { Chip(text: model.wired ? T("USB cable", "USB ケーブル") : "Wi-Fi", color: .secondary) }
                    Spacer()
                    Button(T("Change headset…", "ヘッドセットを変更…")) { model.page = .headset }
                }
                Picker(T("Picture", "画質"), selection: $model.quality) {
                    ForEach(model.sessionQualityChoices) { Text($0.name).tag($0) }
                }
                .pickerStyle(.segmented)
                .disabled(model.play?.running == true)
                Text(model.sessionQualityDetail).font(.callout).foregroundStyle(.secondary)
                Text(T("Each game can take its own way to the headset: the ALVR app or the VeRantyx app, chosen on its card and kept for it.",
                       "ゲームごとにヘッドセットへの経路（ALVR アプリか VeRantyx アプリ）を選べます。各カードで選び、ゲームごとに保存されます。"))
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }

            if model.games.isEmpty {
                Card(title: T("No games yet", "まだゲームがありません")) {
                    Text(T("Download or import games in Get started → Games.", "「はじめに」→「ゲーム」でダウンロードか取り込みをしてください。")).foregroundStyle(.secondary)
                }
            } else {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 280), spacing: 14)], spacing: 14) {
                    ForEach(model.games) { GameCard(game: $0) }
                }
            }
            let more = Title.all.filter { title in title.id != Title.steamVR && !model.games.contains(where: { $0.id == title.id }) }
            if !more.isEmpty {
                Text(T("Not installed yet", "まだ入っていないゲーム")).font(.title3.weight(.semibold)).padding(.top, 6)
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 280), spacing: 14)], spacing: 14) {
                    ForEach(more) { TitleCard(title: $0) }
                }
            }
        }
    }
}

struct Banner: View {
    var icon: String
    var text: String
    var action: (String, () -> Void)? = nil
    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: icon).font(.title3).foregroundStyle(.orange)
            Text(text).fixedSize(horizontal: false, vertical: true)
            Spacer()
            if let action { Button(action.0, action: action.1) }
        }
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color.orange.opacity(0.1)))
    }
}

struct GameCard: View {
    @Environment(AppModel.self) private var model
    var game: Game

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top) {
                Text(game.name).font(.headline).lineLimit(2)
                Spacer()
                if !game.antiCheat.isEmpty { Chip(text: T("Anti-cheat", "アンチチート"), color: .red) }
                else if model.backend(forID: game.id) == .nativeARM64 { Chip(text: T("Not tested", "未検証"), color: .secondary) }
                else if game.title?.works == true { Chip(text: T("Plays", "プレイ可"), color: .green) }
                else if game.flat { Chip(text: T("Mac screen + VR", "Mac 画面 + VR"), color: .purple) }
                else if game.title != nil { Chip(text: T("Runs", "動作"), color: .blue) }
                else { Chip(text: T("Not tested", "未検証"), color: .secondary) }
            }
            Text(game.title.map { model.summary(for: $0) } ??
                 (game.antiCheat.isEmpty ? T("Not tried on VeRantyx yet: it may not start. A report helps.", "VeRantyx ではまだ試されていません。起動しないかもしれません。報告が助けになります。")
                                         : TF("Uses %@; VeRantyx does not run games with anti-cheat.", "%@ を使うため、VeRantyx では起動しません。", game.antiCheat.joined(separator: ", "))))
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            GameRuntimeBadge(id: game.id)
            if game.playable { RoutePicker(id: game.id) }
            if !model.isInstalled(game.id), let title = game.title {
                Button(T("Download selected version", "選択したバージョンをダウンロード")) { model.showInstallation(title) }
                    .disabled(model.configurationLocked)
            } else if game.flat {
                // (two ways to play, one above the other and each as wide as the card: side by side, the labels were
                //  cut short in the narrowest cards)
                Text(gigabytes(game.bytes)).font(.caption).foregroundStyle(.tertiary)
                VStack(spacing: 6) {
                    Button {
                        model.start(game, flat: true)
                    } label: {
                        Label(T("On this Mac", "Mac の画面で"), systemImage: "desktopcomputer").frame(maxWidth: .infinity)
                    }
                    .help(T("Plays in a window on this Mac, without a headset or SteamVR.", "ヘッドセットも SteamVR も使わず、この Mac のウィンドウで遊びます。"))
                    .disabled(!model.canStart(game, flat: true))
                    Button {
                        model.start(game)
                    } label: {
                        Label(T("Headset", "ヘッドセット"), systemImage: "visionpro").frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!model.canStart(game, flat: false))
                }
            } else {
                HStack {
                    Text(gigabytes(game.bytes)).font(.caption).foregroundStyle(.tertiary)
                    Spacer()
                    Button {
                        model.start(game)
                    } label: {
                        Label(T("Play", "プレイ"), systemImage: "play.fill").frame(minWidth: 70)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(!model.canStart(game, flat: false))
                }
            }
            if model.isInstalled(game.id), !model.canStart(game, flat: false), !model.configurationLocked, let title = game.title {
                Button(T("Get started", "はじめに")) { model.showInstallation(title) }
                    .font(.caption)
            }
        }
        .padding(16)
        .frame(minHeight: 150, alignment: .top)
        .background(RoundedRectangle(cornerRadius: 14).fill(Color(nsColor: .controlBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(Color.primary.opacity(0.08)))
    }
}

/// A known game that is not installed yet: "Get and play" fetches it its own way (after a yes), then starts it.
struct TitleCard: View {
    @Environment(AppModel.self) private var model
    var title: Title
    private var busy: Bool { model.configurationLocked }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top) {
                Text(title.name).font(.headline).lineLimit(2)
                Spacer()
                if title.source == .windowsSteam { Chip(text: T("Windows Steam", "Windows 版 Steam"), color: .secondary) }
            }
            Text(model.summary(for: title)).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            GameRuntimeBadge(id: title.id)
            RoutePicker(id: title.id)
            Text(TF("About %@", "約 %@", gigabytes(model.estimatedBytes(for: title)))).font(.caption).foregroundStyle(.tertiary)
            Button { model.showInstallation(title) } label: {
                Label(T("Download selected version", "選択したバージョンをダウンロード"), systemImage: "arrow.down.circle").frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(busy)
        }
        .padding(16)
        .frame(minHeight: 150, alignment: .top)
        .background(RoundedRectangle(cornerRadius: 14).fill(Color(nsColor: .controlBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(Color.primary.opacity(0.08)))
    }
}

/// The get-and-play flow: the question (size, free space, how it is fetched), then where it is.
struct FlowCard: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        if let f = model.flow {
            Card(title: TF("Get %@", "%@ を入手", f.title.name)) {
                switch f.stage {
                case .confirm:
                    Text(TF("About %@ to download; %@ free on that disk.", "ダウンロードは約 %@。そのディスクの空きは %@ です。",
                            gigabytes(f.bytes), f.free.map(gigabytes) ?? "?"))
                        .fontWeight(.medium)
                    if !f.steamcmd.isEmpty {
                        Label(TF("Valve's steamcmd downloads %@ with your Steam account. It asks for your password and Steam Guard code itself, in its console; VeRantyx keeps neither.",
                                 "Valve の steamcmd が、あなたの Steam アカウントで %@ をダウンロードします。パスワードと Steam Guard コードは steamcmd 自身がコンソールで聞きます。VeRantyx はどちらも保存しません。",
                                 f.steamcmd.compactMap { Title.named($0)?.name }.joined(separator: ", ")), systemImage: "arrow.down.circle")
                            .fixedSize(horizontal: false, vertical: true)
                        if model.account.trimmingCharacters(in: .whitespaces).isEmpty {
                            TextField(T("Steam account name (not the e-mail)", "Steam のアカウント名（メールアドレスではなく）"), text: $model.account)
                                .textFieldStyle(.roundedBorder).frame(maxWidth: 300)
                        }
                    }
                    if f.windowsSteam {
                        Label(T("When you press Download and play, your Windows Steam starts the download. If Steam opens a window of its own (where to install, an agreement), answer it there yourself. Only games your account owns are installed.",
                                "「ダウンロードして起動」を押すと、Windows 版 Steam がダウンロードを始めます。Steam が自分の窓（インストール先の確認や利用規約など）を出したときは、そこでご自身で答えてください。アカウントが持っているゲームだけが入ります。"),
                              systemImage: "arrow.down.circle")
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Text(!f.startAfterInstall
                         ? T("Downloads and prepares the selected version. Start the game later from Play.", "選択したバージョンをダウンロードして準備します。起動はあとで「プレイ」から行います。")
                         : f.backend == .nativeARM64
                         ? T("The app prepares the ARM64 libraries and starts ALVR. Open ALVR on your Pico / Quest to continue. Existing compatible content is reused when possible.", "ARM64 ライブラリを準備し ALVR を起動します。Pico・Quest で ALVR を開いてください。互換性のある既存データは再利用します。")
                         : T("Then the first-time setup (the Windows runtime files) and the start.", "そのあと初回の準備（Windows のランタイムファイル）をして起動します。"))
                        .font(.callout).foregroundStyle(.secondary)
                    HStack {
                        Spacer()
                        Button(T("Cancel", "キャンセル")) { model.cancelFlow() }
                        Button(f.startAfterInstall ? T("Download and play", "ダウンロードして起動") : T("Download", "ダウンロード")) { Task { await model.confirmFlow() } }
                            .buttonStyle(.borderedProminent)
                            .disabled(!f.steamcmd.isEmpty && model.account.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                case .openWindowsSteam:
                    Label(T("Open Windows Steam (the card above) and sign in in its window yourself; then Continue.",
                            "Windows 版 Steam を開き（上のカード）、そのウィンドウでご自身でサインインしてから「続ける」を押してください。"), systemImage: "person.badge.key")
                        .fixedSize(horizontal: false, vertical: true)
                    HStack {
                        Spacer()
                        Button(T("Cancel", "キャンセル")) { model.cancelFlow() }
                        Button(T("Continue", "続ける")) { Task { await model.askWindowsSteam() } }
                            .buttonStyle(.borderedProminent).disabled(!model.windowsSteamRunning)
                    }
                case .windowsSteamInstalling(let p):
                    HStack(spacing: 12) {
                        if let p { ProgressView(value: p).frame(width: 200); Text(String(format: "%.0f %%", p * 100)).monospacedDigit() }
                        else { ProgressView().controlSize(.small) }
                        Text(T("Windows Steam is installing it. If it opens a window of its own, answer it there yourself.", "Windows 版 Steam がインストール中です。Steam が自分の窓を出したときは、そこでご自身で答えてください。"))
                            .fixedSize(horizontal: false, vertical: true)
                        Spacer()
                        Button(T("Cancel", "キャンセル")) { model.cancelFlow() }
                    }
                case .working:
                    HStack {
                        ProgressView().controlSize(.small)
                        Text(T("Downloading and setting up…", "ダウンロードと準備をしています…"))
                        Spacer()
                        if f.backend == .nativeARM64 { Button(T("Cancel", "キャンセル")) { model.cancelNativeDownload() } }
                    }
                case .stopped(let why):
                    Label(why, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
                    HStack { Spacer(); Button(T("Close", "閉じる")) { model.cancelFlow() } }
                }
            }
        }
    }
}

/// A game's way to the headset, on its card next to its Headset (Play) button: the ALVR app (a Vision Pro's, a Quest's
/// or a Pico's) or the VeRantyx app on an Apple Vision Pro; kept for the game (AppModel.routes).
struct RoutePicker: View {
    @Environment(AppModel.self) private var model
    var id: String

    var body: some View {
        let headset = model.headset(forID: id)
        VStack(alignment: .leading, spacing: 3) {
            Picker(T("Way to the headset", "ヘッドセットへの経路"), selection: Binding(get: { model.route(forID: id) }, set: { model.setRoute($0, forID: id) })) {
                ForEach(Route.allCases) { Text($0.name).tag($0) }
            }
            .pickerStyle(.menu)
            .labelsHidden()
            .disabled(model.play?.running == true)
            .help(T("ALVR app: ALVR on the headset (Apple Vision Pro from the App Store, Meta Quest, Pico; the Headset page says which). VeRantyx app: VeRantyx's own app on an Apple Vision Pro (hands and Joy-Con). Kept for this game.",
                    "ALVR アプリ：ヘッドセットの ALVR（App Store の Apple Vision Pro 版、Meta Quest、Pico。どれかはヘッドセットのページで選びます）。VeRantyx アプリ：Apple Vision Pro 用の VeRantyx 自身のアプリ（手と Joy-Con）。このゲーム用に保存されます。"))
            Text(headset.name)
                .font(.caption2).foregroundStyle(.secondary).lineLimit(2).fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// The player's Windows Steam on VXRuntime, for the games it starts (No Man's Sky): whether it runs, and a button
/// that opens it. Signing in and every window of Steam's are the player's own to answer.
struct WindowsSteamCard: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Card(title: T("Windows Steam (for No Man's Sky)", "Windows 版 Steam（No Man's Sky 用）")) {
            Check(ok: model.windowsSteamInstalled, text: T("Installed on VXRuntime", "VXRuntime に導入済み"),
                  note: model.windowsSteamInstalled ? nil : T("Install Windows Steam into its own VXRuntime root first (the repository: scripts/launch_vxrt_windows_steam.sh).",
                                                                "先に Windows 版 Steam を専用の VXRuntime のルートに入れてください（リポジトリの scripts/launch_vxrt_windows_steam.sh）。"))
            if model.windowsSteamInstalled {
                Check(ok: model.windowsSteamRunning, text: model.windowsSteamRunning ? T("Running", "起動中") : T("Not running", "起動していません"),
                      note: T("Sign in in Steam's own window yourself. If “Steam Service Error” shows, choose Cancel yourself. VeRantyx never signs in, accepts an agreement or presses a button in Steam for you.",
                              "Steam のウィンドウでご自身でサインインしてください。「Steam Service Error」が出たら、ご自身で Cancel を選びます。VeRantyx が代わりにサインインしたり、規約に同意したり、Steam のボタンを押したりすることはありません。")) {
                    if !model.windowsSteamRunning {
                        Button(T("Open Windows Steam", "Windows 版 Steam を開く")) { model.openWindowsSteam() }
                            .disabled(!model.elsewhereBesidesWindowsSteam.isEmpty || model.play?.running == true)
                    }
                }
                if model.windowsSteamRunning {
                    Text(T("While it runs, only the games it starts can be played here; quit it from its own window for the others.",
                           "起動中は、ここで遊べるのはこの Steam から起動するゲームだけです。ほかのゲームは、Steam をそのウィンドウから終了してから。"))
                        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }
}

struct SessionCard: View {
    @Environment(AppModel.self) private var model
    var play: Play
    @State private var showLog = false

    var body: some View {
        Card(title: play.running ? TF("Playing %@", "%@ をプレイ中", play.title) : TF("%@ has ended", "%@ は終了しました", play.title)) {
            HStack(spacing: 18) {
                if play.flat {
                    Label(T("On this Mac", "Mac の画面で"), systemImage: "desktopcomputer").font(.callout).foregroundStyle(.secondary)
                } else {
                    Step(done: play.phase != .starting, active: play.phase == .starting, text: play.backend == .nativeARM64 ? "Native ARM64 / ALVR" : T("SteamVR", "SteamVR"))
                    Step(done: play.headsetConnected, active: play.running && !play.headsetConnected && play.phase != .starting,
                         text: play.headsetConnected ? T("Headset connected", "ヘッドセット接続") : T("Open the app on the headset", "ヘッドセットでアプリを開く"))
                }
                if play.usesLauncher {
                    Step(done: play.gameStarted, active: play.running && !play.gameStarted && (play.flat || play.phase != .starting),
                         text: T("Launcher: sign in, PLAY", "ランチャー：サインイン、PLAY"))
                }
                Step(done: play.usesLauncher ? play.gameStarted : play.phase == .playing,
                     active: play.usesLauncher ? false : play.phase == .steamVR, text: T("Game", "ゲーム"))
                Spacer()
                if let fps = play.fps, play.running {
                    // (with ALVR, the Mac's frames and what the headset shows of them, from its own statistics: they
                    //  differ when the network holds the stream up)
                    if let shown = play.headsetFPS {
                        VStack(alignment: .trailing, spacing: 0) {
                            Text(String(format: "%.0f", shown)).font(.system(size: 30, weight: .bold, design: .rounded)).monospacedDigit()
                                .foregroundStyle(play.headsetHint == nil ? Color.primary : Color.orange)
                            Text(play.headsetLatency.map { TF("fps in the headset, %@ ms", "fps（ヘッドセット、%@ ms）", String(format: "%.0f", $0)) }
                                 ?? T("fps in the headset", "fps（ヘッドセット）"))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        .help(T("Frames a second the headset shows, and the time from your movement to the picture, as the headset's ALVR app reports them.",
                                "ヘッドセットに表示されている毎秒のフレーム数と、動いてから映像が出るまでの時間（ヘッドセットの ALVR アプリの報告）。"))
                    }
                    VStack(alignment: .trailing, spacing: 0) {
                        Text(String(format: "%.0f", fps)).font(.system(size: 30, weight: .bold, design: .rounded)).monospacedDigit()
                        Text(play.backend == .nativeARM64 || play.headsetFPS != nil ? T("fps on the Mac", "fps（Mac）") : "fps").font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            if play.running && (play.usesLauncher ? !play.gameStarted : play.phase != .playing) {
                Text(T("The first start compiles the game's shaders: it can take a few minutes. Later starts are quick.",
                       "初回はゲームのシェーダをコンパイルするため、数分かかることがあります。2 回目からは速くなります。"))
                    .font(.callout).foregroundStyle(.secondary)
            }
            if play.viaWindowsSteam && play.running {
                Text(T("Stop ends the game (and SteamVR); Windows Steam stays open.", "停止するとゲーム（と SteamVR）が終わります。Windows 版 Steam は開いたままです。"))
                    .font(.callout).foregroundStyle(.secondary)
            }
            if let notice = play.notice, play.running {
                Label(notice, systemImage: play.usesLauncher ? "macwindow" : "visionpro").foregroundStyle(.blue).fixedSize(horizontal: false, vertical: true)
            }
            if play.flat && play.usesLauncher && play.gameStarted && play.running {
                Label(T("Keyboard and mouse work as in Windows; for a DualSense, choose its preset in the game's Options > Controls. Esc opens the game's menu.",
                        "キーボードとマウスは Windows と同じく使えます。DualSense はゲームの オプション > 操作 でそのプリセットを選んでください。Esc でゲームのメニュー。"),
                      systemImage: "keyboard")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            if let hint = play.headsetHint, play.running {
                Label(hint, systemImage: "wifi.exclamationmark").foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
            if let problem = play.problem {
                Label(problem, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
            if play.appID == "546560" { FPSComparisonView(play: play) }
            DisclosureGroup(T("The game's log", "ゲームのログ"), isExpanded: $showLog) {
                VStack(alignment: .leading, spacing: 1) {
                    ForEach(Array(play.gameLines.enumerated()), id: \.offset) { _, line in
                        Text(line).font(.system(size: 10, design: .monospaced)).lineLimit(1).truncationMode(.tail).textSelection(.enabled)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(8)
                .background(RoundedRectangle(cornerRadius: 6).fill(Color(nsColor: .textBackgroundColor)))
            }
            if !play.running && !play.missingFunctions.isEmpty {
                // (names only, nothing from the game: one click sends what the runtime should implement next)
                Label(TF("The game called %@ Windows functions VXRuntime does not have yet.",
                         "ゲームが、VXRuntime にまだない Windows の関数を %@ 個呼びました。", "\(play.missingFunctions.count)"),
                      systemImage: "puzzlepiece.extension")
                    .font(.callout).foregroundStyle(.secondary)
            }
            HStack {
                if !play.running {
                    // (reports anyone can send: they build the list of games that run, and what to implement next)
                    if !play.fpsSamples.isEmpty {
                        Button { NSWorkspace.shared.open(model.runsReportURL(play)) } label: {
                            Label(T("Report: it ran", "報告：動いた"), systemImage: "checkmark.seal")
                        }
                        .help(T("Opens a GitHub form filled in with the game, this Mac and the frame rate; nothing personal. You press Submit.",
                                "ゲーム、この Mac、fps を記入した GitHub のフォームを開きます。個人情報は含みません。送信はご自身で。"))
                    }
                    if !play.missingFunctions.isEmpty {
                        Button { NSWorkspace.shared.open(model.missingFunctionsURL(play)) } label: {
                            Label(T("Send the missing functions", "足りない関数を送る"), systemImage: "paperplane")
                        }
                        .help(T("Opens a GitHub form with the names of the functions only. You press Submit.",
                                "関数の名前だけを記入した GitHub のフォームを開きます。送信はご自身で。"))
                    }
                }
                Spacer()
                if play.running {
                    if play.nativeQuitTimedOut {
                        Button(role: .destructive) { play.forceNativeStop() } label: {
                            Label(T("Force stop", "強制終了"), systemImage: "exclamationmark.octagon")
                        }
                        .help(T("Ends the game immediately. Unsaved progress may be lost.", "直ちにゲームを終了します。未保存の進行状況は失われる場合があります。"))
                    }
                    Button(role: .destructive) { play.stop() } label: { Label(T("Stop", "停止"), systemImage: "stop.fill") }
                        .disabled(play.phase == .stopping)
                } else {
                    Button(T("Close", "閉じる")) { model.play = nil }
                }
            }
        }
    }
}

struct Step: View {
    var done: Bool
    var active: Bool
    var text: String
    var body: some View {
        HStack(spacing: 6) {
            if done { Image(systemName: "checkmark.circle.fill").foregroundStyle(.green) }
            else if active { ProgressView().controlSize(.small) }
            else { Image(systemName: "circle").foregroundStyle(.tertiary) }
            Text(text).font(.callout).foregroundStyle(done || active ? .primary : .secondary)
        }
    }
}

// MARK: - Headset

struct HeadsetView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        PageView(title: T("Headset", "ヘッドセット"), subtitle: T("Which headset shows the game. The Mac renders it; the headset only shows the picture and sends your movements back.",
                                                             "どのヘッドセットで遊ぶか。描画は Mac が行い、ヘッドセットは映像を表示して動きを送り返します。")) {
            HStack(spacing: 12) {
                ForEach(Headset.allCases) { h in
                    Button { model.headset = h } label: {
                        VStack(alignment: .leading, spacing: 6) {
                            Image(systemName: h == .quest ? "visionpro" : "visionpro.fill").font(.title2)
                            Text(h.name).font(.headline).multilineTextAlignment(.leading).fixedSize(horizontal: false, vertical: true)
                            Text(h == .quest ? T("Quest 2/3/3S/Pro, Pico 4…", "Quest 2/3/3S/Pro、Pico 4 など") : h == .visionApp ? T("Hands + Joy-Con, no VR controllers", "手と Joy-Con。VR コントローラ不要") : T("ALVR from the App Store", "App Store の ALVR"))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        .padding(14).frame(maxWidth: .infinity, minHeight: 120, alignment: .topLeading)
                        .background(RoundedRectangle(cornerRadius: 14).fill(model.headset == h ? Color.accentColor.opacity(0.14) : Color(nsColor: .controlBackgroundColor)))
                        .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(model.headset == h ? Color.accentColor : Color.primary.opacity(0.08), lineWidth: model.headset == h ? 2 : 1))
                    }
                    .buttonStyle(.plain)
                }
            }
            switch model.headset {
            case .quest:
                Card(title: T("On the headset", "ヘッドセット側")) {
                    Numbered(1, T("Install ALVR from the Meta Horizon Store (for Pico: from ALVR's releases on GitHub). Version 20.x.", "Meta Horizon Store から ALVR をインストール（Pico は GitHub の ALVR のリリースから）。バージョン 20.x。"))
                    Link(T("ALVR in the Meta Horizon Store", "Meta Horizon Store の ALVR"), destination: URL(string: "https://www.meta.com/experiences/7674846229245715")!)
                    Numbered(2, T("Start a game here in Play, then open ALVR on the headset. It finds the Mac by itself.", "ここの「プレイ」でゲームを始め、ヘッドセットで ALVR を開きます。Mac は自動で見つかります。"))
                    Numbered(3, T("The first time it connects once, takes the headset's display settings and connects again.", "初回は一度つながってヘッドセットの画面設定を受け取り、もう一度つながり直します。"))
                }
                Card(title: T("Wi-Fi or a USB cable", "Wi-Fi か USB ケーブル")) {
                    Toggle(T("Connect with a USB cable", "USB ケーブルで接続する"), isOn: $model.wired)
                    if model.wired {
                        Numbered(1, T("Turn on developer mode for the headset: Meta Horizon app on your phone > Devices > Headset settings > Developer mode.", "ヘッドセットの開発者モードをオン：スマートフォンの Meta Horizon アプリ > デバイス > ヘッドセットの設定 > 開発者モード。"))
                        Numbered(2, T("Connect it to the Mac with a USB-C cable (USB 3 if you can), put it on and allow USB debugging for this Mac.", "USB-C ケーブル（できれば USB 3）で Mac につなぎ、ヘッドセットを着けて、この Mac の USB デバッグを許可します。"))
                        Numbered(3, T("ALVR opens on the headset by itself when a game starts. The first time, Google's Android tools (adb) are downloaded once, unless Homebrew's are installed.", "ゲームを始めるとヘッドセットの ALVR が自動で開きます。初回だけ Google の Android ツール（adb）をダウンロードします（Homebrew のものがあればそれを使います）。"))
                    } else {
                        Text(T("Use 5 GHz Wi-Fi (an 80 MHz channel if your router has it), with the Mac close to the router or wired to it. On 2.4 GHz the picture stutters and smears; if the picture blurs or the controllers lag, try the USB cable.",
                               "5 GHz の Wi-Fi（できれば 80 MHz のチャンネル）を使い、Mac はルーターの近くか有線で。2.4 GHz だとカクついて滲みます。画質が落ちたりコントローラが遅れる時は USB ケーブルを。"))
                            .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    }
                }
                Banner(icon: "person.2.wave.2", text: T("Quest and Pico have not been tested by the author yet (no headset). If you try, a report helps a lot, whether it works or not.",
                                                       "Quest と Pico は作者がまだ試せていません（実機がないため）。試したら、動いても動かなくても報告が大きな助けになります。"),
                       action: (T("Report", "報告する"), { Report.start(model) }))
            case .visionApp:
                Card(title: T("On the Vision Pro", "Vision Pro 側")) {
                    Numbered(1, T("Install the VeRantyx app for Vision Pro (TestFlight when the author shares a link, or from Xcode: see the repository).", "Vision Pro 用の VeRantyx アプリをインストール（作者が共有する TestFlight、または Xcode から：リポジトリ参照）。"))
                    Numbered(2, T("Start a game here, then open the app on the Vision Pro (same Wi-Fi). Hands are tracked by the Vision Pro; a pair of Joy-Con gives the buttons and sticks.", "ここでゲームを始め、Vision Pro でアプリを開きます（同じ Wi-Fi）。手は Vision Pro が追跡し、Joy-Con 2 本がボタンとスティックになります。"))
                    Numbered(3, T("Picture settings: 3K on Pro and Max chips; Recommended on others.", "画質：Pro・Max チップは 3K、それ以外は「おすすめ」。"))
                    Link(T("The Vision Pro app in the repository", "リポジトリの Vision Pro アプリ"), destination: URL(string: "https://github.com/Ag3497120/VeRantyx#readme")!)
                }
            case .visionALVR:
                Card(title: T("On the Vision Pro", "Vision Pro 側")) {
                    Numbered(1, T("Install ALVR from the App Store.", "App Store から ALVR をインストール。"))
                    Link(T("ALVR in the App Store", "App Store の ALVR"), destination: URL(string: "https://apps.apple.com/app/alvr/id6479728026")!)
                    Numbered(2, T("Start a game here, then open ALVR on the Vision Pro (same Wi-Fi). Hand tracking works; game audio comes through ALVR.", "ここでゲームを始め、Vision Pro で ALVR を開きます（同じ Wi-Fi）。ハンドトラッキングが使え、ゲームの音も ALVR から出ます。"))
                }
            }
        }
    }
}

struct Numbered: View {
    var n: Int
    var text: String
    init(_ n: Int, _ text: String) { self.n = n; self.text = text }
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Text("\(n).").fontWeight(.semibold).foregroundStyle(.secondary).frame(width: 18, alignment: .trailing)
            Text(text).fixedSize(horizontal: false, vertical: true)
        }
    }
}

// MARK: - Help

struct HelpView: View {
    @Environment(AppModel.self) private var model
    @State private var log: (String, String)? = nil
    @State private var copied = false
    @AppStorage("language") private var language = ""

    var body: some View {
        PageView(title: T("Help", "ヘルプ")) {
            Card(title: T("When something does not work", "うまく動かない時")) {
                HStack {
                    Button(T("Open the logs folder", "ログのフォルダを開く")) {
                        try? FileManager.default.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
                        NSWorkspace.shared.open(Paths.logs)
                    }
                    Button(T("Copy information about this Mac", "この Mac の情報をコピー")) {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(model.systemReport(), forType: .string)
                        copied = true
                    }
                    if copied { Text(T("Copied", "コピーしました")).foregroundStyle(.secondary) }
                }
                HStack {
                    Button(T("Report a problem…", "問題を報告…")) { Report.start(model) }
                    Text(T("Opens a form on GitHub (an account is needed). Attach the logs if you can.", "GitHub のフォームを開きます（アカウントが必要）。できればログを添付してください。")).font(.callout).foregroundStyle(.secondary)
                }
                HStack {
                    Button(T("Stop everything", "すべて停止"), role: .destructive) { Task { await Play.stopEverything() } }
                    Text(T("Stops SteamVR, the game and the streaming if anything is left running.", "SteamVR・ゲーム・配信が残っていたら止めます。")).font(.callout).foregroundStyle(.secondary)
                }
            }
            Card(title: T("Good to know", "知っておくと良いこと")) {
                Numbered(1, T("Steam for Mac must stay open and signed in while you play: the game uses your Steam session.", "プレイ中は Steam for Mac を開いてサインインしたままにします。ゲームがあなたの Steam を使います。"))
                Numbered(2, T("The first start of a game compiles its shaders, which can take several minutes. Later starts are quick.", "ゲームの初回起動はシェーダのコンパイルで数分かかることがあります。2 回目からは速くなります。"))
                Numbered(3, T("Games with anti-cheat are not supported, and no copy protection is worked around. SteamVR and the games come from your own Steam account.", "アンチチートのあるゲームは対象外で、コピープロテクトの回避も一切しません。SteamVR とゲームはあなた自身の Steam アカウントから入れます。"))
                Numbered(4, T("If turning your head warps the picture, the game runs below the headset's rate: choose Recommended.", "頭を動かすと映像が歪む時は、ゲームがヘッドセットの速さに追いついていません。「おすすめ」を選んでください。"))
            }
            Card(title: T("About", "このアプリについて")) {
                Text("VeRantyx \(AppBuild.version)").fontWeight(.semibold)
                Text(verbatim: "build \(AppBuild.value("CFBundleVersion") ?? "dev"), commit \(AppBuild.commit)")   // (as in reports: not translated)
                    .font(.callout).foregroundStyle(.secondary).textSelection(.enabled)
                Text(T("A personal open-source project (MIT), not affiliated with Valve, Apple or Meta. It contains no Valve or Microsoft files.",
                       "個人のオープンソースプロジェクト（MIT）です。Valve・Apple・Meta とは関係ありません。Valve や Microsoft のファイルは含みません。"))
                    .font(.callout).foregroundStyle(.secondary)
                HStack {
                    Link("GitHub", destination: Paths.repoURL)
                    Button(T("Licenses", "ライセンス")) {
                        let url = Bundle.main.resourceURL!.appendingPathComponent("licenses")
                        NSWorkspace.shared.open(FileManager.default.fileExists(atPath: url.path) ? url : Paths.repoURL)
                    }
                }
                Divider()
                HStack {
                    Picker(T("Language", "言語"), selection: $language) {
                        Text(T("Automatic", "自動")).tag("")
                        ForEach(Language.all, id: \.code) { Text($0.name).tag($0.code) }
                    }
                    .fixedSize()
                    .onChange(of: language) { Language.choose(language) }
                    if language != Language.chosenAtStart {
                        Text(T("Takes effect when VeRantyx starts again.", "VeRantyx を開き直すと切り替わります。")).font(.callout).foregroundStyle(.secondary)
                        Button(T("Restart", "開き直す")) { Language.relaunch() }.disabled(model.play?.running == true || model.job != nil)
                    }
                }
            }
        }
    }
}
