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

    var body: some View {
        @Bindable var model = model
        PageView(title: T("Get started", "はじめに"),
             subtitle: T("VeRantyx plays Windows SteamVR games such as Half-Life: Alyx on this Mac and streams them to your headset. These steps set it up once; each one checks itself.",
                         "VeRantyx は Half-Life: Alyx などの Windows 版 SteamVR ゲームをこの Mac で動かし、ヘッドセットへ送ります。最初に一度だけ、上から順に準備します。各項目は自動で確認されます。")) {
            Card(number: 1, title: T("This Mac", "この Mac"), done: model.appleSilicon && model.macOSOK && model.rosetta == true) {
                Check(ok: model.appleSilicon, text: T("Apple silicon (M1 or later)", "Apple silicon（M1 以降）"))
                Check(ok: model.macOSOK, text: T("macOS 14.2 or later", "macOS 14.2 以降"), note: model.macOSOK ? nil : T("Update macOS in System Settings.", "システム設定から macOS を更新してください。"))
                Check(ok: model.rosetta, text: "Rosetta 2",
                      note: T("Runs the games' Windows (x86) code. Installed by macOS itself; it asks for your password.", "ゲームの Windows（x86）コードを動かします。macOS 自身がインストールし、パスワードを聞かれます。")) {
                    if model.rosetta == false { Button(T("Install Rosetta 2", "Rosetta 2 をインストール")) { Task { await model.installRosetta() } }.disabled(model.job != nil) }
                }
            }
            Card(number: 2, title: "Steam for Mac", done: model.steamInstalled && model.steamRunning) {
                Check(ok: model.steamInstalled, text: T("Installed", "インストール済み"),
                      note: T("The games use the Steam you sign in to on this Mac, with your own account.", "ゲームはこの Mac でサインインした Steam（あなたのアカウント）を使います。")) {
                    if !model.steamInstalled { Button(T("Get Steam", "Steam を入手")) { NSWorkspace.shared.open(URL(string: "https://store.steampowered.com/about/")!) } }
                }
                Check(ok: model.steamRunning, text: T("Running and signed in", "起動してサインイン済み"),
                      note: model.steamRunning ? nil : T("Open Steam and sign in with the account that owns your games. Keep it open while you play.", "Steam を開き、ゲームを持っているアカウントでサインインしてください。プレイ中は開いたままにします。")) {
                    if model.steamInstalled && !model.steamRunning { Button(T("Open Steam", "Steam を開く")) { Steam.open() } }
                }
            }
            Card(number: 3, title: T("Where the games go", "ゲームの置き場所"), done: model.freeBytes != nil) {
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(model.drive.path).font(.system(.body, design: .monospaced)).lineLimit(1).truncationMode(.middle)
                        Text(model.freeBytes.map { TF("%@ free", "空き %@", gigabytes($0)) } ?? "").font(.callout).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button(T("Change…", "変更…")) { model.chooseDrive() }.disabled(model.job != nil || model.play?.running == true)
                }
                Text(T("Half-Life: Alyx takes about 74 GB and SteamVR about 6 GB. An external SSD works too.", "Half-Life: Alyx は約 74 GB、SteamVR は約 6 GB 使います。外付け SSD でも動きます。"))
                    .font(.callout).foregroundStyle(.secondary)
            }
            Card(number: 4, title: T("SteamVR and games (Windows versions)", "SteamVR とゲーム（Windows 版）"), done: model.steamVR && model.gamesInstalled) {
                VStack(spacing: 8) {
                    ForEach(Title.all) { t in
                        HStack(spacing: 10) {
                            if model.isInstalled(t.id) {
                                Image(systemName: "checkmark.circle.fill").foregroundStyle(.green).frame(width: 18)
                            } else {
                                Toggle("", isOn: Binding(get: { model.wanted.contains(t.id) || t.id == Title.steamVR },
                                                         set: { if $0 { model.wanted.insert(t.id) } else if t.id != Title.steamVR { model.wanted.remove(t.id) } }))
                                    .labelsHidden().toggleStyle(.checkbox).frame(width: 18)
                                    .disabled(t.id == Title.steamVR)
                            }
                            VStack(alignment: .leading, spacing: 1) {
                                HStack(spacing: 6) {
                                    Text(t.name).fontWeight(.medium)
                                    if t.free { Chip(text: T("Free", "無料"), color: .blue) }
                                }
                                Text(t.summary).font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Text(model.isInstalled(t.id) ? T("Installed", "インストール済み") : gigabytes(t.bytes)).font(.callout).foregroundStyle(.secondary)
                        }
                    }
                }
                Divider()
                Text(T("Download with your Steam account", "Steam アカウントでダウンロード")).fontWeight(.semibold)
                HStack {
                    TextField(T("Steam account name (not the e-mail)", "Steam のアカウント名（メールアドレスではなく）"), text: $model.account)
                        .textFieldStyle(.roundedBorder).frame(maxWidth: 300)
                    Button(T("Download", "ダウンロード")) { Task { await model.download() } }
                        .buttonStyle(.borderedProminent)
                        .disabled(model.job != nil || model.account.trimmingCharacters(in: .whitespaces).isEmpty || Title.all.allSatisfy { !model.wanted.contains($0.id) || model.isInstalled($0.id) })
                }
                Text(T("Valve's steamcmd downloads them. It asks for your Steam password and Steam Guard code once, in the window that opens; it remembers the sign-in itself. VeRantyx keeps neither.",
                       "Valve の steamcmd がダウンロードします。最初の一回だけ、開くウィンドウで Steam のパスワードと Steam Guard コードを聞かれます（サインインは steamcmd 自身が覚えます）。VeRantyx はどちらも保存しません。"))
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Divider()
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(T("Or copy them from a Steam library you have", "または手元の Steam ライブラリからコピー")).fontWeight(.semibold)
                        Text(T("A Windows PC's game drive, a CrossOver or Whisky bottle: no download. On the same disk the copy takes no extra space.",
                               "Windows PC のゲーム用ドライブ、CrossOver や Whisky のボトルなど。ダウンロードは不要で、同じディスクなら容量も増えません。"))
                            .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    }
                    Spacer()
                    Button(T("Choose a folder…", "フォルダを選ぶ…")) { model.chooseImport() }.disabled(model.job != nil)
                }
            }
            Card(number: 5, title: T("Windows runtime files", "Windows のランタイムファイル"), done: model.redistCount > 0) {
                Check(ok: model.redistCount > 0, text: model.redistCount > 0 ? TF("%@ files ready", "%@ 個のファイルを用意済み", "\(model.redistCount)") : T("Not gathered yet", "まだ集めていません"),
                      note: T("Microsoft's runtime DLLs the games need, taken from the installers SteamVR and the games bring with them. Done by itself after a download or copy.",
                              "ゲームが必要とする Microsoft のランタイム DLL です。SteamVR やゲームに付いてくるインストーラーから取り出します。ダウンロードやコピーの後に自動で行います。")) {
                    Button(model.redistCount > 0 ? T("Gather again", "集め直す") : T("Gather", "集める")) { Task { await model.collect() } }
                        .disabled(model.job != nil || !model.steamVR)
                }
            }
            if model.ready {
                HStack {
                    Image(systemName: "checkmark.seal.fill").foregroundStyle(.green).font(.title2)
                    Text(T("Ready. Choose your headset, then play.", "準備ができました。ヘッドセットを選んで、プレイしましょう。")).font(.headline)
                    Spacer()
                    Button(T("Headset", "ヘッドセット")) { model.page = .headset }
                    Button(T("Play", "プレイ")) { model.page = .play }.buttonStyle(.borderedProminent)
                }
                .padding(16)
                .background(RoundedRectangle(cornerRadius: 14).fill(Color.green.opacity(0.08)))
            }
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
                            if model.isInstalled(app.id) { Chip(text: T("Already here", "入っています"), color: .green) }
                            Spacer()
                            Text(gigabytes(app.sizeOnDisk)).foregroundStyle(.secondary)
                        }
                    }
                    .disabled(model.isInstalled(app.id))
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
                chosen = Set(choice.apps.map(\.id).filter { (Title.named($0) != nil) && !model.isInstalled($0) })
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
            if !model.ready {
                Banner(icon: "checklist", text: T("Finish Get started first.", "先に「はじめに」を済ませてください。"), action: (T("Get started", "はじめに"), { model.page = .setup }))
            } else if !model.steamRunning {
                Banner(icon: "exclamationmark.triangle", text: T("Open Steam and sign in: the games need it.", "Steam を開いてサインインしてください。ゲームに必要です。"), action: (T("Open Steam", "Steam を開く"), { Steam.open() }))
            }
            if !model.elsewhere.isEmpty {
                Banner(icon: "terminal", text: T("A game or SteamVR started elsewhere (Terminal) is running.", "別の場所（ターミナル）で起動したゲームか SteamVR が動いています。"),
                       action: (T("Stop it", "停止する"), { Task { await Play.stopEverything() } }))
            }
            if let play = model.play { SessionCard(play: play) }

            Card(title: T("Settings for this session", "今回の設定")) {
                HStack {
                    Label(model.headset.name, systemImage: model.headset == .quest ? "visionpro" : "visionpro.fill")
                    if model.headset == .quest { Chip(text: model.wired ? T("USB cable", "USB ケーブル") : "Wi-Fi", color: .secondary) }
                    Spacer()
                    Button(T("Change headset…", "ヘッドセットを変更…")) { model.page = .headset }
                }
                Picker(T("Picture", "画質"), selection: $model.quality) {
                    ForEach(Quality.choices(for: model.headset)) { Text($0.name).tag($0) }
                }
                .pickerStyle(.segmented)
                Text(model.quality.detail).font(.callout).foregroundStyle(.secondary)
            }

            if model.games.isEmpty {
                Card(title: T("No games yet", "まだゲームがありません")) {
                    Text(T("Download or copy them in Get started, step 4.", "「はじめに」の手順 4 でダウンロードかコピーをしてください。")).foregroundStyle(.secondary)
                }
            } else {
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 240), spacing: 14)], spacing: 14) {
                    ForEach(model.games) { GameCard(game: $0) }
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
                else if game.title?.works == true { Chip(text: T("Plays", "プレイ可"), color: .green) }
                else if game.title != nil { Chip(text: T("Runs", "動作"), color: .blue) }
                else { Chip(text: T("Not tested", "未検証"), color: .secondary) }
            }
            Text(game.title?.summary ??
                 (game.antiCheat.isEmpty ? T("Not tried on VeRantyx yet: it may not start. A report helps.", "VeRantyx ではまだ試されていません。起動しないかもしれません。報告が助けになります。")
                                         : TF("Uses %@; VeRantyx does not run games with anti-cheat.", "%@ を使うため、VeRantyx では起動しません。", game.antiCheat.joined(separator: ", "))))
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            HStack {
                Text(gigabytes(game.bytes)).font(.caption).foregroundStyle(.tertiary)
                Spacer()
                Button {
                    model.start(game)
                } label: {
                    Label(T("Play", "プレイ"), systemImage: "play.fill").frame(minWidth: 70)
                }
                .buttonStyle(.borderedProminent)
                .disabled(!game.playable || !model.ready || !model.steamRunning || model.play?.running == true || !model.elsewhere.isEmpty)
            }
        }
        .padding(16)
        .frame(minHeight: 150, alignment: .top)
        .background(RoundedRectangle(cornerRadius: 14).fill(Color(nsColor: .controlBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(Color.primary.opacity(0.08)))
    }
}

struct SessionCard: View {
    @Environment(AppModel.self) private var model
    var play: Play
    @State private var showLog = false

    var body: some View {
        Card(title: play.running ? TF("Playing %@", "%@ をプレイ中", play.title) : TF("%@ has ended", "%@ は終了しました", play.title)) {
            HStack(spacing: 18) {
                Step(done: play.phase != .starting, active: play.phase == .starting, text: T("SteamVR", "SteamVR"))
                Step(done: play.headsetConnected, active: play.running && !play.headsetConnected && play.phase != .starting,
                     text: play.headsetConnected ? T("Headset connected", "ヘッドセット接続") : T("Open the app on the headset", "ヘッドセットでアプリを開く"))
                Step(done: play.phase == .playing, active: play.phase == .steamVR, text: T("Game", "ゲーム"))
                Spacer()
                if let fps = play.fps, play.running {
                    VStack(alignment: .trailing, spacing: 0) {
                        Text(String(format: "%.0f", fps)).font(.system(size: 30, weight: .bold, design: .rounded)).monospacedDigit()
                        Text("fps").font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            if play.running && play.phase != .playing {
                Text(T("The first start compiles the game's shaders: it can take a few minutes. Later starts are quick.",
                       "初回はゲームのシェーダをコンパイルするため、数分かかることがあります。2 回目からは速くなります。"))
                    .font(.callout).foregroundStyle(.secondary)
            }
            if let notice = play.notice {
                Label(notice, systemImage: "visionpro").foregroundStyle(.blue).fixedSize(horizontal: false, vertical: true)
            }
            if let problem = play.problem {
                Label(problem, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            }
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
            HStack {
                Spacer()
                if play.running {
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
