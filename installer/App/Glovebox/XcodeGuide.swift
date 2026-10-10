import SwiftUI
import AppKit
import LocalAuthentication

/// The steps that happen in another app: getting Xcode in the App Store, Xcode's first open,
/// signing in to Xcode, pairing in Device Hub, and macOS's keychain window before the first build.
/// None of them can be asked to press a button or
/// open a pane, so Glovebox opens the app (and sets Xcode's Settings to open on Apple Accounts),
/// and floats a glass bubble with the one thing to do next: big keys to press, or an arrow at the
/// exact button to click, with a ring around it. Glovebox follows the windows by their positions
/// alone, which needs no permission, closes the guide the moment the step is done and comes back
/// to the front.
@MainActor
final class XcodeGuide: ObservableObject {
    enum Job: Equatable {
        /// Sign in to Xcode with an Apple Account.
        case signIn
        /// Show Xcode the account again so it sees a new Developer Program membership.
        case refresh
        /// Sign in to Xcode again: its saved sign-in for the account expired, though Xcode still lists the
        /// account (a build then says "Unable to log in with account"). The install's error card starts it,
        /// and the install starts again once Apple's sign-in has come and gone.
        case signInAgain
        /// Pair the headset: Device Hub with Xcode 27 and later, Xcode's Devices window before.
        case pair
        /// Get Xcode in the App Store (or update it): Get, Install, then Apple's sign-in or password.
        case appStore
        /// Xcode's first open: its license, the Mac's password for its components, its first pages.
        case setup
        /// macOS's keychain window, asking before the build may sign with the key Xcode made: the Mac's
        /// password, then Always Allow (Enter is Allow, just once). Install starts and ends it.
        case keychain
    }

    /// What the App Store shows over its page.
    enum Sheet: Equatable {
        /// The App Store's own first page (What's New), before the player clicked anything in it.
        case welcome
        /// Apple's sign-in, when the App Store isn't signed in.
        case signIn
        /// The Apple Account's password, to confirm the download.
        case password
    }

    enum Stage: Int, Comparable {
        /// Xcode's first-launch pages are in the way.
        case welcome
        /// Settings (or Device Hub's +, or Devices) is next.
        case open
        /// Click the button (Sign In…, or Pair Nearby Device in Device Hub's menu).
        case click
        /// Device Hub's pairing window: pick Vision Pro, so it shows the headset's steps.
        case device
        /// The App Store's Install (where Get was); the Mac's password for Xcode's components.
        case confirm
        /// Apple's sign-in sheet, or the headset's part of pairing.
        case finish
        case done

        static func < (a: Stage, b: Stage) -> Bool { a.rawValue < b.rawValue }
    }

    @Published private(set) var isRunning = false
    #if DEBUG
    private var pretending = false
    #endif
    @Published private(set) var job: Job = .signIn
    @Published private(set) var stage: Stage = .open
    /// Xcode 27 and later have Device Hub and say "Sign In…"; Xcode 26 says "Add Apple Account…".
    @Published private(set) var xcodeMajor = 26
    /// Which side of the bubble its arrow is on, when it points at a button; nil beside a window.
    @Published private(set) var arrow: HorizontalEdge?
    /// The arrow's tip, down from the bubble's top.
    @Published private(set) var arrowY: CGFloat = Bubble.defaultArrowY
    /// The arrow comes out of the bubble's top instead, this far from its left: under macOS's keychain
    /// window, which stays above every other window, so nothing can point or ring inside it.
    @Published private(set) var arrowUpX: CGFloat?
    /// What the bubble points up at, in window-server coordinates.
    private var pointedAt: CGRect?
    /// The App Store's job is an update (Xcode is there, but too old).
    @Published private(set) var updating = false
    /// Apple's sheet over the App Store's page, while it's up.
    @Published private(set) var sheet: Sheet?
    /// The ring's button isn't measured for this window: the bubble shows a picture of it instead.
    @Published private(set) var showsPicture = false
    /// Xcode's first-launch page is the one that installs its parts (Install), not a later one
    /// (Continue).
    @Published private(set) var installPage = false
    /// Device Hub asks for the code the headset shows (pairing's last part, on this Mac).
    @Published private(set) var pairCode = false
    /// The tour's stand-in for Device Hub's code window, which needs a real headset to show up.
    var pretendCodeWindow: CGRect?
    /// macOS's prompt for Xcode takes Touch ID on a Mac that has it (Xcode is Apple's), with
    /// Use Password beside it.
    @Published private(set) var touchID = false
    /// The rehearsal's stand-in for a Mac with Touch ID (CI's Macs have none).
    var pretendTouchID: Bool?

    weak var model: InstallerModel?
    private var panel: NSPanel?
    private var ring: NSWindow?
    /// What the ring is around, in window-server coordinates; a click inside moves the guide on.
    private var ringTarget: CGRect?
    private var loop: Task<Void, Never>?
    private var clicks: Any?
    /// Catches the guide's keys pressed while Glovebox is in front, to hand them to Xcode.
    private var keyMonitor: Any?
    private var placedFor: String?
    /// When the player clicked Device Hub's +; its menu shows up a moment later.
    private var plusClicked: Date?
    /// When Device Hub's pairing window was last on screen; it closes when the player cancels.
    private var pairingSeen: Date?
    /// The bubble stays hidden until the app it guides is on screen, so it shows up next to it.
    private var revealed = false
    private var startedAt = Date()
    /// Clicks on the App Store's Get (then Install) button so far.
    private var storeClicks = 0
    /// The App Store's window, and whether the player clicked in it yet: a sheet before any click
    /// is the App Store's own What's New, not Apple's sign-in.
    private var storeFrame: CGRect?
    private var clickedInStore = false
    /// The sheet over the App Store's page, as it was when it came up.
    private var sheetFrame: CGRect?
    /// Xcode's setup asked for the Mac's password: what follows is Xcode installing its parts.
    private var passwordSeen = false
    /// When the password prompt went away. Cancelled, the license is still there: the first check
    /// after this says so.
    private var promptClosed: Date?
    /// The password prompt is up, and whether this Mac had Touch ID at hand when it came up.
    private var promptUp = false
    private var touchIDAtPrompt = false
    /// Signing in again: Apple's sign-in over Settings came up, and whether it's up now.
    private var signInSeen = false
    @Published private(set) var signingIn = false

    func start(_ job: Job = .signIn) {
        stop()
        self.job = job
        xcodeMajor = model?.xcodeMajor ?? 26
        updating = model?.setup.results[.xcode]?.code == "xcode-too-old"
        stage = .open
        isRunning = true
        model?.objectWillChange.send()
        if [.signIn, .refresh, .signInAgain].contains(job) { Self.openSettingsOnAccounts() }
        openTarget()
        showPanel(reveal: false)
        startedAt = Date()
        clicks = NSEvent.addGlobalMonitorForEvents(matching: .leftMouseDown) { [weak self] _ in
            let at = NSEvent.mouseLocation
            MainActor.assumeIsolated { self?.clicked(at) }
        }
        // The keys only work in Xcode. Pressed in Glovebox, they bring Xcode to the front instead,
        // and the bubble keeps showing them for another go.
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            let handed = MainActor.assumeIsolated { () -> Bool in
                guard let self, self.isGuideKey(event) else { return false }
                self.focusTarget()
                return true
            }
            return handed ? nil : event
        }
        // Often enough that the bubble jumps to the next button about as soon as it appears.
        loop = Task { [weak self] in
            var tick = 0
            while !Task.isCancelled {
                guard let self else { return }
                self.follow()
                self.fitPanel()
                self.revealWhenPlaced()
                if tick % 10 == 0, self.finished { return self.complete() }
                tick += 1
                try? await Task.sleep(nanoseconds: 150_000_000)
            }
        }
    }

    func stop() {
        loop?.cancel()
        loop = nil
        if let c = clicks { NSEvent.removeMonitor(c) }
        clicks = nil
        if let k = keyMonitor { NSEvent.removeMonitor(k) }
        keyMonitor = nil
        panel?.orderOut(nil)
        ring?.orderOut(nil)
        panel = nil
        ring = nil
        ringTarget = nil
        placedFor = nil
        plusClicked = nil
        pairingSeen = nil
        revealed = false
        arrow = nil
        arrowUpX = nil
        pointedAt = nil
        sheet = nil
        showsPicture = false
        installPage = false
        pairCode = false
        pretendCodeWindow = nil
        storeClicks = 0
        storeFrame = nil
        clickedInStore = false
        sheetFrame = nil
        passwordSeen = false
        promptClosed = nil
        promptUp = false
        signInSeen = false
        signingIn = false
        pretendTouchID = nil
        if isRunning {
            isRunning = false
            model?.objectWillChange.send()
        }
    }

    /// "Show It Again": Xcode or Device Hub in front again, with the bubble beside it.
    func bringForward() {
        guard isRunning else { return }
        focusTarget()
        if revealed { panel?.orderFrontRegardless() }
    }

    /// Xcode (or Device Hub) in front, so what the player presses or clicks goes to it, not to
    /// Glovebox.
    private func focusTarget() {
        if job == .keychain { return }
        if job == .appStore {
            NSApp.yieldActivation(toApplicationWithBundleIdentifier: Self.appStoreID)
        } else if job != .pair || xcodeMajor < 27 {
            NSApp.yieldActivation(toApplicationWithBundleIdentifier: "com.apple.dt.Xcode")
        }
        openTarget()
    }

    /// ⌘, (or ⇧⌘2 for Xcode 26's Devices window) while the bubble shows those keys.
    private func isGuideKey(_ e: NSEvent) -> Bool {
        guard stage == .open else { return false }
        let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
        switch job {
        case .signIn, .refresh, .signInAgain: return mods == .command && e.charactersIgnoringModifiers == ","
        case .pair: return xcodeMajor < 27 && mods == [.command, .shift] && e.keyCode == 19
        case .appStore, .setup, .keychain: return false
        }
    }

    private func openTarget() {
        if job == .keychain { return }
        if job == .appStore {
            NSWorkspace.shared.open(URL(string: MacTools.appStoreXcode)!)
        } else if job == .pair, xcodeMajor >= 27, let hub = Self.deviceHubURL {
            NSWorkspace.shared.openApplication(at: hub, configuration: NSWorkspace.OpenConfiguration())
        } else {
            MacTools.openXcode()
        }
    }

    /// The model's checks, which run every few seconds, say when the step is done.
    private var finished: Bool {
        guard let m = model else { return false }
        switch job {
        case .signIn: return m.setup.results[.team].map { $0.code != "apple-account-missing" } ?? false
        case .refresh: return m.programState == .done
        case .signInAgain:
            // Xcode's lists said signed in all along: Apple's sign-in coming and going is all there is to see.
            return signInSeen && !signingIn
        case .pair: return m.pairState == .done
        case .appStore:
            // The App Store puts Xcode in Applications as soon as the download starts.
            guard let r = m.setup.results[.xcode] else { return false }
            return r.value == "downloading" || !["xcode-missing", "xcode-too-old"].contains(r.code ?? "")
        case .setup:
            // Xcode works, and its first-launch pages are through too.
            return m.xcodeReady && !Self.windows(of: "com.apple.dt.Xcode").contains(where: Self.isFirstLaunch)
        case .keychain:
            // The install says, once macOS's window closes (keychainAnswered).
            return false
        }
    }

    /// The apps the player is in while the guide runs: when it's done, Glovebox comes back from them.
    private var targetApps: Set<String> {
        switch job {
        case .appStore: return [Self.appStoreID]
        case .pair: return ["com.apple.dt.Xcode", "com.apple.dt.Devices", "Device Hub"]
        default: return ["com.apple.dt.Xcode"]
        }
    }

    private func complete() {
        withAnimation(.spring(duration: 0.4, bounce: 0.4)) { stage = .done }
        hideRing()
        resizePanel()
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 1_600_000_000)
            guard let self else { return }
            let apps = self.targetApps, job = self.job
            self.stop()
            self.model?.comeBack(from: apps)
            if job == .signInAgain {
                self.model?.signedInAgain()
            } else {
                self.model?.recheck()
            }
        }
    }

    // MARK: Following the windows

    /// Where things are now: Xcode's first-launch pages, its Settings window, Device Hub.
    private func follow() {
        switch job {
        case .pair: return followPairing()
        case .appStore: return followAppStore()
        case .setup: return followSetup()
        case .keychain: return followKeychain()
        case .signIn, .refresh, .signInAgain: break
        }
        let windows = Self.windows(of: "com.apple.dt.Xcode")
        var signInNow = false
        defer { if signingIn != signInNow { signingIn = signInNow } }
        // Xcode's first-launch pages float above its other windows until they're through.
        if let welcome = windows.first(where: { $0.layer == 8 && $0.frame.height > 400 }) {
            set(.welcome, beside: welcome.frame)
            return
        }
        guard let settings = windows.first(where: { $0.layer == 0 && Self.isSettings($0.frame) }) else {
            set(stage == .welcome ? .open : min(stage, .open), beside: windows.first(where: { $0.layer == 0 })?.frame)
            return
        }
        // A sheet over Settings is Apple's sign-in. On Xcode 27.0 it's 480 × 600 points, and the
        // bubble points at its Apple Account field.
        let sheet = windows.first { $0.layer == 0 && $0.frame != settings.frame && settings.frame.contains(CGPoint(x: $0.frame.midX, y: $0.frame.minY + 2)) }
        if job == .signInAgain {
            // The account, then Apple's sign-in for it (its password once more). After a click on the
            // account the bubble stops pointing: what Xcode shows next for an expired sign-in couldn't
            // be measured off a real account.
            if let sheet {
                signInNow = true
                signInSeen = true
                set(.finish, beside: sheet.frame)
            } else if stage == .finish {
                set(.finish, beside: settings.frame)
            } else {
                set(.click, pointAt: Self.accountRow(in: settings.frame, xcodeMajor: xcodeMajor), from: .trailing, beside: settings.frame)
            }
        } else if let sheet, job == .signIn {
            if (440...520).contains(sheet.frame.width), (540...660).contains(sheet.frame.height) {
                set(.finish, pointAt: Self.accountField(in: sheet.frame), from: .trailing, beside: sheet.frame, ring: false)
            } else {
                set(.finish, beside: sheet.frame)
            }
        } else if stage == .finish || job == .refresh {
            set(job == .refresh ? .click : .finish, beside: settings.frame)
        } else {
            // The button is near Settings' right edge: the bubble goes to its right, pointing back.
            set(.click, pointAt: Self.signInButton(in: settings.frame, xcodeMajor: xcodeMajor), from: .trailing, beside: settings.frame)
        }
    }

    private func followPairing() {
        guard xcodeMajor >= 27 else {
            let windows = Self.windows(of: "com.apple.dt.Xcode").filter { $0.layer == 0 }
            // The Devices window is the big one that opens with ⇧⌘2; until then, Xcode's own.
            let devices = windows.first { $0.frame.width >= 900 && $0.frame.height >= 500 }
            set(devices == nil ? .open : max(stage, .click), beside: (devices ?? windows.first)?.frame)
            return
        }
        let all = Self.deviceHubWindows
        let hub = all.filter { $0.layer == 0 }.max { $0.frame.width < $1.frame.width }
        // Once the Mac picked the headset, Device Hub asks for the code the headset shows, in a
        // window of its own: the code goes in here, on this Mac.
        if let code = pretendCodeWindow ?? all.first(where: { $0.layer == 0 && Self.isCodeWindow($0.frame) })?.frame {
            pairingSeen = Date()
            if !pairCode { withAnimation(.spring(duration: 0.35)) { pairCode = true } }
            if let boxes = Self.codeBoxes(in: code) {
                set(.finish, pointAt: boxes, from: .trailing, beside: code)
            } else {
                set(.finish, beside: code)
            }
            return
        }
        if pairCode { withAnimation(.spring(duration: 0.35)) { pairCode = false } }
        // Pair Nearby Device opens a small window of its own (300 × 433 on Xcode 27.0): first
        // pick Vision Pro there, then the headset's part. Cancel closes it: back to +.
        let pairing = all.first { $0.layer == 0 && $0.frame.width < 520 && $0.frame.height > 300 && $0.frame != hub?.frame }
        if pairing != nil { pairingSeen = Date() }
        if stage >= .device {
            if let pairing {
                if stage == .device {
                    set(.device, pointAt: Self.visionProTile(in: pairing.frame), from: .trailing, beside: pairing.frame)
                } else {
                    set(.finish, beside: pairing.frame)
                }
                return
            }
            // Pairing done closes it too; the model's check catches that a moment later.
            if Date().timeIntervalSince(pairingSeen ?? startedAt) < (stage == .finish ? 10 : 3) {
                set(stage, beside: hub?.frame)
                return
            }
        }
        guard let hub else {
            set(.open, beside: nil)
            return
        }
        // The + is near Device Hub's left edge, so the bubble sits left of it, pointing right. It
        // opens a menu (a window of its own, at the menus' level) with Pair Nearby Device last.
        // Measured on Xcode 27.0: the menu's last item is 24 points tall, 6 in from its edges.
        if let menu = all.first(where: { $0.layer >= 100 && $0.frame.minY >= hub.frame.minY }) {
            let item = CGRect(x: menu.frame.minX + 6, y: menu.frame.maxY - 30, width: menu.frame.width - 12, height: 24)
            set(.click, pointAt: item, from: .leading, beside: hub.frame)
        } else if stage == .click, let at = plusClicked, Date().timeIntervalSince(at) < 4 {
            return
        } else {
            plusClicked = nil
            set(.open, pointAt: Self.plusButton(in: hub.frame), from: .leading, beside: hub.frame)
        }
    }

    /// The App Store's Xcode page: Get (Update, for an old Xcode), then Install in the same place,
    /// then Apple's sheet when it asks to sign in or for the password. The window can be any size:
    /// the button is found from its frame (AppStoreLayout), and where that isn't measured the
    /// bubble sits beside the window with a picture of the button.
    private func followAppStore() {
        let all = Self.windows(of: Self.appStoreID).filter { $0.layer == 0 }
        guard let main = all.max(by: { $0.frame.width * $0.frame.height < $1.frame.width * $1.frame.height }) else {
            set(.open, beside: nil)
            return
        }
        storeFrame = main.frame
        if let over = all.first(where: { $0.frame != main.frame && main.frame.contains(CGPoint(x: $0.frame.midX, y: $0.frame.minY + 2)) }) {
            // Decided when it comes up: the click on its own Continue doesn't make it a sign-in.
            let kind: Sheet = sheetFrame == over.frame ? sheet ?? .signIn
                : !clickedInStore ? .welcome : AppStoreLayout.isSignIn(over.frame) ? .signIn : .password
            sheetFrame = over.frame
            if sheet != kind {
                withAnimation(.spring(duration: 0.35)) { sheet = kind }
                placedFor = nil
            }
            picture(false)
            if kind != .welcome {
                set(.finish, beside: over.frame)
            } else if let button = AppStoreLayout.continueButton(in: over.frame) {
                set(.open, pointAt: button, from: .trailing, beside: over.frame)
            } else {
                set(.open, beside: over.frame)
            }
            return
        }
        sheetFrame = nil
        if sheet != nil {
            withAnimation(.spring(duration: 0.35)) { sheet = nil }
            placedFor = nil
        }
        let s: Stage = storeClicks == 0 ? .click : storeClicks == 1 && !updating ? .confirm : .finish
        if s != .finish, let button = AppStoreLayout.getButton(in: main.frame) {
            picture(false)
            set(s, pointAt: button, from: .trailing, beside: main.frame)
        } else {
            picture(s != .finish)
            set(s, beside: main.frame)
        }
    }

    /// Touch ID set up and at hand (a closed MacBook's isn't). Asking shows nothing.
    static func hasTouchID() -> Bool {
        let c = LAContext()
        return c.canEvaluatePolicy(.deviceOwnerAuthenticationWithBiometrics, error: nil) && c.biometryType == .touchID
    }

    /// Xcode's first open: its license (Agree), then macOS asks for the Mac's password so Xcode can
    /// install its parts, then Xcode's first-launch pages (Continue), until Xcode works.
    private func followSetup() {
        if let prompt = Self.windows(where: { _, owner in owner == "SecurityAgent" }).first {
            if !promptUp { touchIDAtPrompt = Self.hasTouchID() }
            promptUp = true
            let t = pretendTouchID ?? touchIDAtPrompt
            if touchID != t { touchID = t }
            passwordSeen = true
            promptClosed = nil
            set(.confirm, beside: prompt.frame)
            return
        }
        promptUp = false
        if passwordSeen, promptClosed == nil {
            promptClosed = Date()
            model?.recheck()
        }
        let windows = Self.windows(of: "com.apple.dt.Xcode")
        guard let front = windows.first(where: Self.isFirstLaunch) ?? windows.first(where: { $0.layer == 0 }) ?? windows.first else {
            set(.open, beside: nil)
            return
        }
        // The checks say what's left: the license (value "license"), then Xcode's parts ("components").
        let left = model?.setup.results[.xcode].flatMap { $0.code == "xcode-first-launch" ? $0.value : nil }
        // After the password, the license is through, unless a check since says it isn't (the
        // prompt was cancelled).
        let checkedSince = promptClosed.map { (model?.preflightStarted ?? .distantPast) > $0 } ?? false
        if left == "license", !passwordSeen || checkedSince {
            if let agree = Self.agreeButton(in: front.frame) {
                picture(false)
                set(.click, pointAt: agree, from: .trailing, beside: front.frame)
            } else {
                picture(true)
                set(.click, beside: front.frame)
            }
        } else if Self.isFirstLaunch(front) {
            // Its pages: the first installs Xcode's parts, the next ones just go on.
            let install = left != nil && !passwordSeen
            if installPage != install {
                installPage = install
                placedFor = nil
            }
            set(.welcome, pointAt: Self.continueButton(in: front.frame), from: .trailing, beside: front.frame)
        } else {
            // Agreed: Xcode installs what it needs, then shows its first pages.
            set(.finish, beside: front.frame)
        }
    }

    /// macOS's keychain window: the bubble right under it, its arrow up at Always Allow. macOS keeps that
    /// window above every other one (a ring at any level shows only as a glow through its glass), so the
    /// bubble covers none of it. In a window of a size not measured, the bubble sits beside it with a picture
    /// of it instead.
    private func followKeychain() {
        guard let prompt = Self.windows(where: { _, owner in owner == "SecurityAgent" }).first else { return }
        guard stage != .done else { return }
        go(.click)
        hideRing()
        if let button = Self.alwaysAllowButton(in: prompt.frame) {
            picture(false)
            pointedAt = button
            let key = "up \(button) \(prompt.frame)"
            if placedFor != key {
                placedFor = key
                if !pointUp(at: Self.appKit(button), under: Self.appKit(prompt.frame)) { place(beside: Self.appKit(prompt.frame)) }
            }
        } else {
            picture(true)
            pointedAt = nil
            set(.click, beside: prompt.frame)
        }
    }

    /// The keychain window closed and the install read the key again: Always Allow took (a tick, then the
    /// bubble goes), or it didn't (it goes now, and Glovebox's card says what to do).
    func keychainAnswered(allowed: Bool) {
        guard isRunning, job == .keychain else { return }
        guard allowed else { return stop() }
        withAnimation(.spring(duration: 0.4, bounce: 0.4)) { stage = .done }
        hideRing()
        resizePanel()
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 1_200_000_000)
            if let self, self.job == .keychain { self.stop() }
        }
    }

    private func picture(_ on: Bool) {
        guard showsPicture != on else { return }
        showsPicture = on
        placedFor = nil
        resizePanel()
    }

    /// A click inside the ring moves the guide on.
    private func clicked(_ at: NSPoint) {
        if job == .appStore, let store = storeFrame, Self.appKit(store).contains(at) { clickedInStore = true }
        guard let target = ringTarget, Self.appKit(target).insetBy(dx: -4, dy: -4).contains(at) else {
            // In Xcode 26's Devices window the click is on the headset, which Glovebox can't see.
            if job == .pair, stage == .click, xcodeMajor < 27, panel?.frame.contains(at) == false {
                go(.finish)
            }
            return
        }
        if job == .appStore, sheet == .welcome {
            hideRing()
        } else if job == .appStore {
            // Get turns into Install in the same place; an update just starts.
            storeClicks += 1
            hideRing()
            if updating {
                Task { [weak self] in
                    try? await Task.sleep(nanoseconds: 3_000_000_000)
                    if let self, self.isRunning, self.job == .appStore { self.complete() }
                }
            }
        } else if job == .setup {
            hideRing()
        } else if job == .pair, stage == .open {
            plusClicked = Date()
            go(.click)
        } else if job == .pair, stage == .click, xcodeMajor >= 27 {
            pairingSeen = Date()
            go(.device)
            hideRing()
        } else {
            go(.finish)
            hideRing()
        }
    }

    private func go(_ s: Stage) {
        guard s != stage, stage != .done else { return }
        // Xcode's first-launch pages are through: Xcode should have the keyboard for the keys.
        if stage == .welcome, s == .open, NSApp.isActive { focusTarget() }
        withAnimation(.spring(duration: 0.35)) { stage = s }
        resizePanel()
    }

    /// The bubble beside a window, nothing to point at.
    private func set(_ s: Stage, beside window: CGRect?) {
        guard stage != .done else { return }
        go(s)
        hideRing()
        guard let window else { return }
        let key = "beside \(window) \(s)"
        if placedFor != key {
            placedFor = key
            place(beside: Self.appKit(window))
        }
    }

    /// The bubble right next to a button, its arrow at the button, and a ring around it. `from`
    /// is the side of the button the bubble sits on.
    private func set(_ s: Stage, pointAt button: CGRect, from side: HorizontalEdge, beside window: CGRect, ring: Bool = true) {
        guard stage != .done else { return }
        go(s)
        if ring {
            ringTarget = button
            showRing(Self.appKit(button))
        } else {
            hideRing()
        }
        let key = "point \(button) \(side) \(s)"
        if placedFor != key {
            placedFor = key
            if !point(at: Self.appKit(button), from: side) { place(beside: Self.appKit(window)) }
        }
    }

    // MARK: Windows

    struct OtherWindow {
        var frame: CGRect
        var layer: Int
    }

    /// Another app's on-screen windows, front to back. Positions and sizes need no permission.
    static func windows(of bundleID: String) -> [OtherWindow] {
        let pids = Set(NSRunningApplication.runningApplications(withBundleIdentifier: bundleID).map(\.processIdentifier))
        return pids.isEmpty ? [] : windows(where: { pid, _ in pids.contains(pid) })
    }

    /// Device Hub's windows: by its id, where it lives inside Xcode, or its name. On CI's Xcode
    /// 27.0 the id alone found none while a "Device Hub" window was on screen.
    static var deviceHubWindows: [OtherWindow] {
        let path = deviceHubURL?.resolvingSymlinksInPath().path
        let pids = Set(NSWorkspace.shared.runningApplications.filter {
            $0.bundleIdentifier == "com.apple.dt.Devices" || $0.localizedName == "Device Hub"
                || (path != nil && $0.bundleURL?.resolvingSymlinksInPath().path == path)
        }.map(\.processIdentifier))
        return windows(where: { pid, owner in pids.contains(pid) || owner == "Device Hub" })
    }

    private static func windows(where owned: (pid_t, String?) -> Bool) -> [OtherWindow] {
        guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]]
        else { return [] }
        return list.compactMap { w in
            guard let pid = w[kCGWindowOwnerPID as String] as? pid_t, owned(pid, w[kCGWindowOwnerName as String] as? String),
                  let b = w[kCGWindowBounds as String] as? NSDictionary,
                  let frame = CGRect(dictionaryRepresentation: b as CFDictionary),
                  frame.width > 120, frame.height > 80 else { return nil }
            return OtherWindow(frame: frame, layer: w[kCGWindowLayer as String] as? Int ?? 0)
        }
    }

    static let appStoreID = "com.apple.AppStore"

    /// Xcode's first-launch pages float above its other windows until they're through (a 480 × 632
    /// modal on Xcode 27.0).
    static func isFirstLaunch(_ w: OtherWindow) -> Bool { w.layer == 8 && w.frame.height > 400 }

    /// The first-launch pages' Continue, at their bottom right: 94 × 38 points, 19 in from the
    /// right and bottom edges (measured on Xcode 27.0).
    static func continueButton(in w: CGRect) -> CGRect {
        CGRect(x: w.maxX - 19 - 94, y: w.maxY - 19 - 38, width: 94, height: 38).insetBy(dx: -4, dy: -4)
    }

    /// Agree in Xcode's license window, at its bottom right: 91 × 36 points, 20 in from the right
    /// and bottom edges in the 480 × 632 window of macOS 27; 84 × 26, 19 in, in the 503 × 432 one of
    /// macOS 26 (measured). Nil for a window of another size.
    static func agreeButton(in w: CGRect) -> CGRect? {
        let b: (width: CGFloat, height: CGFloat, inset: CGFloat)
        if abs(w.width - 480) <= 12, abs(w.height - 632) <= 12 {
            b = (91, 36, 20)
        } else if abs(w.width - 503) <= 12, abs(w.height - 432) <= 12 {
            b = (84, 26, 19)
        } else {
            return nil
        }
        return CGRect(x: w.maxX - b.inset - b.width, y: w.maxY - b.inset - b.height,
                      width: b.width, height: b.height).insetBy(dx: -4, dy: -4)
    }

    /// Xcode's Settings window opens at 710 × 452 points in Xcode 26 and 27.
    static func isSettings(_ r: CGRect) -> Bool {
        (680...760).contains(r.width) && (420...500).contains(r.height)
    }

    /// The Apple Accounts pane's sign-in button, 20 points in from the window's right edge:
    /// "Sign In…" in Xcode 27, "Add Apple Account…" in Xcode 26 (measured on both).
    static func signInButton(in w: CGRect, xcodeMajor: Int) -> CGRect {
        let size = xcodeMajor >= 27 ? CGSize(width: 76, height: 24) : CGSize(width: 151, height: 24)
        let top: CGFloat = xcodeMajor >= 27 ? 158 : 124
        return CGRect(x: w.maxX - 20 - size.width, y: w.minY + top, width: size.width, height: size.height).insetBy(dx: -7, dy: -7)
    }

    /// The first account in the Apple Accounts pane: where its "Sign in to your Apple Account" box is
    /// with no account, 200 points in and 72 down, 490 × 66, on Xcode 27 ("Sign-in to an Apple Account",
    /// 208 in, 52 down, 482 × 52, on Xcode 26), measured on both.
    static func accountRow(in w: CGRect, xcodeMajor: Int) -> CGRect {
        let r = xcodeMajor >= 27 ? CGRect(x: 200, y: 72, width: 490, height: 66) : CGRect(x: 208, y: 52, width: 482, height: 52)
        return r.offsetBy(dx: w.minX, dy: w.minY).insetBy(dx: -4, dy: -4)
    }

    /// Device Hub's +, at the top of its sidebar (measured on Xcode 27.0).
    static func plusButton(in w: CGRect) -> CGRect {
        CGRect(x: w.minX + 114, y: w.minY + 8, width: 38, height: 38)
    }

    /// The Vision Pro tile in Device Hub's pairing window, the third of three under "Waiting to
    /// pair." (measured on Xcode 27.0).
    static func visionProTile(in w: CGRect) -> CGRect {
        CGRect(x: w.minX + 200, y: w.minY + 138, width: 86, height: 72)
    }

    /// Device Hub's "Enter the verification code displayed on …" window: 470 × 313 on Xcode 27.0
    /// (measured from a real pairing); the device list before it is 300 × 433.
    static func isCodeWindow(_ w: CGRect) -> Bool {
        w.width >= 400 && w.width <= 560 && w.height >= 260 && w.height <= 360
    }

    /// Its six boxes, centered, 163 points down (measured from that window).
    static func codeBoxes(in w: CGRect) -> CGRect? {
        guard abs(w.width - 470) <= 16, abs(w.height - 313) <= 16 else { return nil }
        return CGRect(x: w.midX - 151, y: w.minY + 163, width: 302, height: 46)
    }

    /// Always Allow in macOS's keychain window, at its bottom left: 103 × 22 points, 94 in from the left edge
    /// and 21 up from the bottom of the 434 × 182 window of macOS 27 (measured). A long key name makes the
    /// window taller, not wider, and the buttons stay at the bottom. Nil for a window of another width.
    static func alwaysAllowButton(in w: CGRect) -> CGRect? {
        guard abs(w.width - 434) <= 12, (160...300).contains(w.height) else { return nil }
        return CGRect(x: w.minX + 94, y: w.maxY - 21 - 22, width: 103, height: 22).insetBy(dx: -4, dy: -4)
    }

    /// The Apple Account field in Xcode's sign-in sheet (measured on Xcode 27.0).
    static func accountField(in w: CGRect) -> CGRect {
        CGRect(x: w.minX + 68, y: w.minY + 272, width: 344, height: 36)
    }

    /// Window-server coordinates (top left of the main display) to AppKit's (bottom left).
    static func appKit(_ r: CGRect) -> NSRect {
        let h = NSScreen.screens.first?.frame.height ?? 0
        return NSRect(x: r.minX, y: h - r.maxY, width: r.width, height: r.height)
    }

    static var deviceHubURL: URL? {
        guard let xcode = MacTools.xcodeURL else { return nil }
        let hub = xcode.appendingPathComponent("Contents/Applications/DeviceHub.app")
        return FileManager.default.fileExists(atPath: hub.path) ? hub : nil
    }

    /// Xcode remembers the last Settings pane it showed; set it to Apple Accounts, so ⌘, opens
    /// right there.
    static func openSettingsOnAccounts() {
        UserDefaults(suiteName: "com.apple.dt.Xcode")?.set("IDEKit.IDESettingsPane.Accounts", forKey: "IDELastViewedSettingsPane")
    }

    // MARK: The bubble and the ring

    private func showPanel(reveal: Bool = true) {
        let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 360, height: 200),
                        styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = false
        p.hidesOnDeactivate = false
        p.level = .floating
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        p.isReleasedWhenClosed = false
        p.contentView = FirstClickHostingView(rootView: GuideBubble().environmentObject(self))
        panel = p
        resizePanel()
        if let screen = NSScreen.main?.visibleFrame {
            p.setFrameTopLeftPoint(NSPoint(x: screen.maxX - p.frame.width - 16, y: screen.maxY - 16))
        }
        revealed = reveal
        if reveal { p.orderFrontRegardless() }
    }

    /// Fades the bubble in once it has a place next to Xcode or Device Hub, or after a while
    /// without one.
    private func revealWhenPlaced() {
        guard !revealed, let p = panel, placedFor != nil || Date().timeIntervalSince(startedAt) > 15 else { return }
        revealed = true
        p.alphaValue = 0
        p.orderFrontRegardless()
        NSAnimationContext.runAnimationGroup { $0.duration = 0.3; p.animator().alphaValue = 1 }
        if NSApp.isActive { focusTarget() }
    }

    private func resizePanel() {
        guard let p = panel, let v = p.contentView else { return }
        v.layoutSubtreeIfNeeded()
        let size = v.fittingSize
        let top = p.frame.maxY
        p.setFrame(NSRect(x: p.frame.minX, y: top - size.height, width: size.width, height: size.height), display: true, animate: p.isVisible)
    }

    /// SwiftUI sizes the bubble a moment after its content changes: catch up, keeping its top
    /// edge, and place it again.
    private func fitPanel() {
        guard let p = panel, let v = p.contentView else { return }
        let size = v.fittingSize
        guard abs(size.height - p.frame.height) > 1 || abs(size.width - p.frame.width) > 1 else { return }
        resizePanel()
        placedFor = nil
    }

    /// Beside the window, on whichever side has room, top edges lined up.
    private func place(beside w: NSRect) {
        guard let p = panel else { return }
        arrow = nil
        arrowUpX = nil
        let screen = NSScreen.screens.first { $0.frame.intersects(w) }?.visibleFrame ?? NSScreen.main?.visibleFrame ?? w
        let size = p.frame.size
        let m = Bubble.margin
        var x = w.maxX + 8 - m
        if x + size.width - m > screen.maxX { x = w.minX - 8 - size.width + m }
        if x + m < screen.minX { x = screen.maxX - size.width }
        let top = min(w.maxY + m, screen.maxY)
        p.setFrame(NSRect(x: x, y: max(screen.minY, top - size.height), width: size.width, height: size.height), display: true, animate: true)
    }

    /// Right next to the button, the arrow's tip at its edge. False when there's no room.
    private func point(at b: NSRect, from side: HorizontalEdge) -> Bool {
        guard let p = panel else { return false }
        let screen = NSScreen.screens.first { $0.frame.intersects(b) }?.visibleFrame ?? NSScreen.main?.visibleFrame ?? b
        let size = p.frame.size
        let m = Bubble.margin
        let gap: CGFloat = 6
        func x(_ s: HorizontalEdge) -> CGFloat { s == .leading ? b.minX - gap - size.width + m : b.maxX + gap - m }
        var s = side
        if x(s) + m < screen.minX || x(s) + size.width - m > screen.maxX { s = s == .leading ? .trailing : .leading }
        guard x(s) + m >= screen.minX, x(s) + size.width - m <= screen.maxX else { return false }
        // The tip lines up with the button's middle; near the screen's top or bottom the bubble
        // stays on screen and the arrow moves along its edge instead.
        var top = b.midY + m + Bubble.defaultArrowY
        top = min(top, screen.maxY + m)
        top = max(top, screen.minY + size.height - m)
        arrowY = top - m - b.midY
        // The bubble sits on the button's leading side, so its arrow is on its trailing edge.
        arrow = s == .leading ? .trailing : .leading
        arrowUpX = nil
        p.setFrame(NSRect(x: x(s), y: top - size.height, width: size.width, height: size.height), display: true, animate: true)
        return true
    }

    /// Under the window, the bubble's left edge lined up with the window's, its arrow up at the button. False
    /// when there's no room below.
    private func pointUp(at b: NSRect, under w: NSRect) -> Bool {
        guard let p = panel else { return false }
        let screen = NSScreen.screens.first { $0.frame.intersects(w) }?.visibleFrame ?? NSScreen.main?.visibleFrame ?? w
        let size = p.frame.size
        let m = Bubble.margin
        // Room for the arrow's bob, which mustn't reach the window.
        let top = w.minY - 9 + m
        guard top - size.height >= screen.minY else { return false }
        let x = min(max(w.minX - m, screen.minX - m), screen.maxX - size.width + m)
        arrow = nil
        arrowUpX = b.midX - (x + m)
        p.setFrame(NSRect(x: x, y: top - size.height, width: size.width, height: size.height), display: true, animate: true)
        return true
    }

    private func showRing(_ rect: NSRect) {
        let frame = rect.insetBy(dx: -PulseRing.room, dy: -PulseRing.room)
        if ring == nil {
            let r = NSWindow(contentRect: frame, styleMask: .borderless, backing: .buffered, defer: false)
            r.isOpaque = false
            r.backgroundColor = .clear
            r.hasShadow = false
            r.ignoresMouseEvents = true
            // Over Device Hub's menu too, which sits above ordinary floating windows.
            r.level = .popUpMenu
            r.collectionBehavior = [.canJoinAllSpaces, .transient, .ignoresCycle]
            r.isReleasedWhenClosed = false
            r.contentView = NSHostingView(rootView: PulseRing())
            ring = r
        }
        if ring?.frame != frame { ring?.setFrame(frame, display: true) }
        if ring?.isVisible == false { ring?.orderFrontRegardless() }
    }

    private func hideRing() {
        ring?.orderOut(nil)
        ringTarget = nil
    }
}

/// Takes the first click even though the bubble's window never becomes key, so keys typed
/// meanwhile still go to Xcode.
private final class FirstClickHostingView<Content: View>: NSHostingView<Content> {
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

// MARK: Views

/// The bubble: which step of how many, then the one thing to do, big. Keys to press bounce until
/// they're pressed; an arrow nudges toward the button to click.
private struct GuideBubble: View {
    @EnvironmentObject var guide: XcodeGuide
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var pop = false
    @State private var nudge = false

    var body: some View {
        let shape = Bubble(arrow: guide.arrow, arrowY: guide.arrowY, upX: guide.arrowUpX, topRoom: topRoom)
        VStack(alignment: .leading, spacing: 12) {
            header
            if guide.stage == .done {
                HStack(spacing: 10) {
                    Image(systemName: "checkmark.circle.fill").font(.system(size: 30)).foregroundStyle(.white, Theme.accent)
                    Text(doneText).font(.system(size: 22, weight: .bold))
                }
                .padding(.vertical, 6)
                .transition(.scale.combined(with: .opacity))
            } else {
                instruction
                    .id("\(guide.job)-\(guide.stage)")
                    .transition(.asymmetric(insertion: .move(edge: .bottom).combined(with: .opacity), removal: .opacity))
            }
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 20)
        .padding(.top, 14)
        .padding(.bottom, 20)
        .frame(width: width, alignment: .leading)
        .padding(.top, topRoom)
        .padding(.horizontal, Bubble.arrowLength)
        .glass(shape, tint: 0.3)
        .shadow(color: .black.opacity(0.35), radius: 14, y: 6)
        .scaleEffect(pop ? 1.08 : 1, anchor: anchor)
        .offset(x: nudgeOffset, y: nudge && guide.arrowUpX != nil ? -5 : 0)
        .padding(Bubble.margin)
        .environment(\.colorScheme, .dark)
        .task(id: wantsAttention) { await bounce() }
        .onChange(of: pointing) { _, on in
            nudge = false
            guard on, !Motion.reduced(reduceMotion) else { return }
            withAnimation(.easeInOut(duration: 0.55).repeatForever(autoreverses: true)) { nudge = true }
        }
    }

    private var pointing: Bool { guide.arrow != nil || guide.arrowUpX != nil }

    /// Room above for an arrow out of the top: kept for the keychain window's job all along, so the
    /// bubble's size doesn't change when it starts pointing.
    private var topRoom: CGFloat { guide.job == .keychain ? 18 : 0 }

    private var width: CGFloat { guide.job == .pair && guide.stage == .finish ? 310 : guide.job == .keychain ? 290 : 260 }

    private var nudgeOffset: CGFloat {
        guard nudge, let a = guide.arrow else { return 0 }
        return a == .leading ? -7 : 7
    }

    private var anchor: UnitPoint {
        if guide.arrowUpX != nil { return .top }
        switch guide.arrow {
        case .leading?: return .leading
        case .trailing?: return .trailing
        case nil: return .center
        }
    }

    /// Keys to press: the bubble pops every couple of seconds until they're pressed.
    private var wantsAttention: Bool { !keys.isEmpty && guide.stage != .done }

    private func bounce() async {
        pop = false
        guard wantsAttention, !Motion.reduced(reduceMotion) else { return }
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 1_400_000_000)
            withAnimation(.spring(duration: 0.22, bounce: 0.5)) { pop = true }
            try? await Task.sleep(nanoseconds: 220_000_000)
            withAnimation(.spring(duration: 0.5, bounce: 0.6)) { pop = false }
        }
    }

    // MARK: Content

    private var header: some View {
        HStack(spacing: 6) {
            ForEach(0..<(count > 1 ? count : 0), id: \.self) { i in
                Capsule()
                    .fill(i < index ? Theme.accent : i == index ? Color.white : Color.white.opacity(0.25))
                    .frame(width: i == index ? 18 : 6, height: 6)
            }
            Spacer()
            Button { guide.stop() } label: {
                Image(systemName: "xmark")
                    .font(.system(size: 10, weight: .bold))
                    .frame(width: 20, height: 20)
                    .background(Circle().fill(.white.opacity(0.14)))
            }
            .buttonStyle(.plain)
            .help("Stop the guide")
        }
        .animation(.spring(duration: 0.35), value: index)
    }

    @ViewBuilder private var instruction: some View {
        VStack(alignment: .leading, spacing: 12) {
            headlineText
                .font(.system(size: 22, weight: .bold))
                .tracking(-0.3)
                .fixedSize(horizontal: false, vertical: true)
            if !keys.isEmpty {
                PressKeys(keys: keys)
            }
            if guide.showsPicture, guide.job == .appStore {
                AppStoreButtonMock(title: guide.updating ? "Update" : "Get")
            }
            if guide.showsPicture, guide.job == .keychain {
                KeychainPromptArt()
                    .scaleEffect(0.8, anchor: .topLeading)
                    .frame(width: 248, height: 96, alignment: .topLeading)
            }
            if guide.job == .pair, guide.stage == .finish, guide.xcodeMajor >= 27, !guide.pairCode {
                // Device Hub's tiles are device types; the headset itself shows up in the list under them once
                // Remote Devices on the headset makes it discoverable (the Mac isn't listed there). Then the Mac
                // picks it, the headset shows a code, and it's typed here on the Mac (pairCode).
                GuideStep(number: 1, text: "On your Vision Pro, open Settings › General › Remote Devices") {
                    RemoteDevicesMock(macName: MacTools.macName)
                        .scaleEffect(0.75, anchor: .topLeading)
                        .frame(width: 210, height: 135, alignment: .topLeading)
                }
                GuideStep(number: 2, text: "On this Mac, pick it and click Next") { EmptyView() }
            }
            if let detail {
                Text(detail)
                    .font(.system(size: 13))
                    .foregroundStyle(.white.opacity(0.72))
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let menu {
                // For keyboards without ⌘, or when the keys don't take: the same thing by mouse.
                HStack(spacing: 6) {
                    Image(systemName: "menubar.rectangle")
                    Text("Or click \(Text(menu).fontWeight(.semibold)) in the menu bar.")
                }
                .font(.system(size: 13))
                .foregroundStyle(.white.opacity(0.72))
                .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private var x27: Bool { guide.xcodeMajor >= 27 }

    /// The guide's stages for this job, in order, for the dots at the top.
    private var stages: [XcodeGuide.Stage] {
        switch guide.job {
        case .signIn: return [.open, .click, .finish]
        case .refresh: return [.open, .click]
        case .signInAgain: return [.open, .click, .finish]
        case .pair: return x27 ? [.open, .click, .device, .finish] : [.open, .click, .finish]
        case .appStore: return guide.updating ? [.click, .finish] : [.click, .confirm, .finish]
        case .setup: return [.click, .confirm, .welcome]
        case .keychain: return [.click]
        }
    }

    private var count: Int { stages.count }

    private var index: Int {
        if guide.stage == .done { return count }
        if guide.job == .setup {
            // Agree; Install, the password and Xcode installing; then its pages.
            switch guide.stage {
            case .click, .open: return 0
            case .welcome where !guide.installPage: return 2
            default: return 1
            }
        }
        return stages.firstIndex(of: guide.stage) ?? 0
    }

    private var keys: [String] {
        switch (guide.job, guide.stage) {
        case (.signIn, .open), (.refresh, .open), (.signInAgain, .open): return ["⌘", ","]
        case (.pair, .open) where !x27: return ["⇧", "⌘", "2"]
        default: return []
        }
    }

    private var headline: String {
        switch (guide.job, guide.stage) {
        case (.appStore, .open): return guide.sheet == .welcome ? "Click Continue" : "Opening the App Store"
        case (.appStore, .click):
            if guide.updating { return "Click Update" }
            return guide.showsPicture ? "Click Get, then Install" : "Click Get"
        case (.appStore, .confirm): return "Click Install"
        case (.appStore, .finish):
            switch guide.sheet {
            case .signIn?: return "Sign in with your Apple Account"
            case .password?: return "Type your Apple Account password"
            case .welcome?, nil: return guide.updating ? "Xcode is updating" : "Xcode is downloading"
            }
        case (.setup, .open): return "Opening Xcode"
        case (.setup, .click): return "Click Agree"
        case (.setup, .confirm): return guide.touchID ? "Use Touch ID" : "Type your Mac's password"
        case (.setup, .finish): return "Xcode is installing"
        case (.setup, .welcome): return guide.installPage ? "Click Install" : "Click Continue"
        case (_, .welcome): return "Click Continue in Xcode"
        case (.signIn, .open), (.refresh, .open), (.signInAgain, .open): return "Press these keys"
        case (.signIn, .click): return x27 ? "Click Sign In…" : "Click Add Apple Account…"
        case (.signIn, .finish): return "Sign in with your Apple Account"
        case (.refresh, _): return "Click your account"
        case (.signInAgain, .click): return "Click your Apple Account"
        case (.signInAgain, .finish): return guide.signingIn ? "Type your Apple Account password" : "Sign in again"
        case (.pair, .open): return x27 ? "Click +" : "Press these keys"
        case (.pair, .click): return x27 ? "Click Pair Nearby Device" : "Click your Vision Pro, then Pair"
        case (.pair, .device): return "Click Vision Pro"
        case (.pair, .finish): return x27 && !guide.pairCode ? "Find your Vision Pro" : "Type the code your Vision Pro shows"
        default: return ""
        }
    }

    /// The headline; in macOS's keychain window, the button's name in the logo's orange.
    private var headlineText: Text {
        if guide.job == .keychain {
            return Text("Type your Mac password, then click \(Text("Always Allow").foregroundStyle(Theme.accent))")
        }
        return Text(headline)
    }

    private var detail: String? {
        switch (guide.job, guide.stage) {
        case (.signIn, .open), (.refresh, .open), (.signInAgain, .open): return "Opens Xcode Settings."
        case (.signInAgain, .click): return "Then sign in again."
        case (.signInAgain, .finish): return guide.signingIn ? "Same as on your iPhone." : "Xcode asks for your Apple Account password."
        case (.signIn, .finish): return "Same as on your iPhone."
        case (.refresh, _): return "Xcode checks your membership again."
        case (.pair, .open): return x27 ? nil : "First, on your Vision Pro, open Settings › General › Remote Devices."
        case (.pair, .finish) where guide.pairCode || !x27: return "It's on your Vision Pro. Type it here, on this Mac."
        case (.appStore, .click) where guide.showsPicture: return "Under Xcode's name, near the top."
        case (.appStore, .confirm): return "Same place as Get."
        case (.appStore, .finish):
            switch guide.sheet {
            case .signIn?: return "Same as on your iPhone."
            case .password?: return "It confirms the download. Touch ID works too."
            case .welcome?, nil: return nil
            }
        case (.setup, .click): return guide.showsPicture ? "Xcode's license, at the bottom right." : "Xcode's license."
        case (.setup, .confirm):
            return guide.touchID ? "Or click Use Password and type your Mac's password. Xcode needs it to finish installing."
                : "The one you unlock this Mac with. Xcode needs it to finish installing."
        case (.setup, .finish): return "If it shows Install, click it."
        case (.keychain, _): return "Don't press Enter. Glovebox never sees your password."
        default: return nil
        }
    }

    /// The menu-bar path to the same place as the keys.
    private var menu: String? {
        switch (guide.job, guide.stage) {
        case (.signIn, .open), (.refresh, .open), (.signInAgain, .open): return "Xcode › Settings…"
        case (.pair, .open) where !x27: return "Window › Devices and Simulators"
        default: return nil
        }
    }

    private var doneText: String {
        switch guide.job {
        case .signIn, .signInAgain: return "Signed in"
        case .refresh: return "Membership found"
        case .pair: return "Paired"
        case .appStore: return guide.updating ? "Updating" : "Downloading"
        case .setup: return "Xcode is ready"
        case .keychain: return "Allowed"
        }
    }
}

/// The App Store's button, for when the guide can't point at it: Xcode's icon and name, and the
/// blue Get under them, ringed.
private struct AppStoreButtonMock: View {
    var title: String
    @EnvironmentObject var guide: XcodeGuide
    @State private var on = false

    var body: some View {
        HStack(spacing: 12) {
            XcodeIcon(size: 46, installed: guide.model?.xcodeIcon)
            VStack(alignment: .leading, spacing: 2) {
                Text("Xcode").font(.system(size: 15, weight: .bold))
                Text("Developer Tools").font(.system(size: 11)).foregroundStyle(.secondary)
                Text(title)
                    .font(.system(size: 12, weight: .bold))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 4)
                    .background(Capsule().fill(Color(red: 0, green: 0.48, blue: 1)))
                    .overlay(Capsule().inset(by: -4).stroke(Theme.accent, lineWidth: 2.5).opacity(on ? 1 : 0.45))
                    .padding(.top, 6)
            }
            Spacer(minLength: 0)
        }
        .foregroundStyle(Color(white: 0.1))
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(.white))
        .environment(\.colorScheme, .light)
        .onAppear { withAnimation(.easeInOut(duration: 0.7).repeatForever(autoreverses: true)) { on = true } }
        .accessibilityLabel("The App Store's \(title) button, under Xcode's name")
    }
}

/// Big keycaps that press themselves, like fingers on the keyboard.
private struct PressKeys: View {
    var keys: [String]
    @State private var pressed = 0
    /// With Reduce Motion the keys glow instead of moving.
    @State private var glow = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        HStack(spacing: 8) {
            ForEach(Array(keys.enumerated()), id: \.offset) { i, key in
                let down = i < pressed
                Text(key)
                    .font(.system(size: 30, weight: .medium, design: .rounded))
                    .foregroundStyle(Color(white: 0.15))
                    .frame(width: 64, height: 58)
                    .background(
                        RoundedRectangle(cornerRadius: 10, style: .continuous)
                            .fill(down ? Color(red: 1, green: 0.93, blue: 0.84) : Color.white)
                            .shadow(color: .black.opacity(0.45), radius: 0, y: down ? 1 : 4)
                    )
                    .shadow(color: Theme.accent.opacity(glow ? 0.9 : 0), radius: 10)
                    .offset(y: down ? 3 : 0)
            }
        }
        .padding(.bottom, 4)
        .accessibilityLabel(keys.joined(separator: " "))
        .task(id: keys) {
            if Motion.reduced(reduceMotion) {
                while !Task.isCancelled {
                    withAnimation(.easeInOut(duration: 0.6)) { glow.toggle() }
                    try? await Task.sleep(nanoseconds: 700_000_000)
                }
                return
            }
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 700_000_000)
                for i in 1...keys.count {
                    withAnimation(.easeOut(duration: 0.12)) { pressed = i }
                    try? await Task.sleep(nanoseconds: 160_000_000)
                }
                try? await Task.sleep(nanoseconds: 350_000_000)
                withAnimation(.spring(duration: 0.3, bounce: 0.4)) { pressed = 0 }
            }
        }
    }
}

/// One numbered thing to do, with a picture of it under the words.
private struct GuideStep<Art: View>: View {
    var number: Int
    var text: String
    @ViewBuilder var art: Art

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Text("\(number)")
                .font(.system(size: 13, weight: .bold).monospacedDigit())
                .foregroundStyle(Color(white: 0.08))
                .frame(width: 24, height: 24)
                .background(Circle().fill(Theme.accent))
            VStack(alignment: .leading, spacing: 8) {
                Text(text)
                    .font(.system(size: 15, weight: .semibold))
                    .fixedSize(horizontal: false, vertical: true)
                art
            }
        }
    }
}

/// A rounded rectangle with an arrow on one side, its tip `arrowY` down from the top. The arrow's
/// room is kept on both sides so the bubble's size doesn't change when it starts pointing.
struct Bubble: Shape {
    static let margin: CGFloat = 18
    static let arrowLength: CGFloat = 12
    static let defaultArrowY: CGFloat = 34
    var arrow: HorizontalEdge?
    var arrowY: CGFloat
    /// An arrow out of the top edge instead, its tip this far from the left.
    var upX: CGFloat?
    /// Room kept above the body for that arrow.
    var topRoom: CGFloat = 0
    var radius: CGFloat = 22

    func path(in rect: CGRect) -> Path {
        var body = rect.insetBy(dx: Self.arrowLength, dy: 0)
        body.origin.y += topRoom
        body.size.height -= topRoom
        var p = Path(roundedRect: body, cornerRadius: radius, style: .continuous)
        let half: CGFloat = 11
        if let upX {
            let half: CGFloat = 15
            let x = min(max(rect.minX + upX, body.minX + radius + half), body.maxX - radius - half)
            let mid = (body.minY + rect.minY) / 2
            p.move(to: CGPoint(x: x - half, y: body.minY + 1))
            p.addQuadCurve(to: CGPoint(x: x, y: rect.minY), control: CGPoint(x: x - 3, y: mid))
            p.addQuadCurve(to: CGPoint(x: x + half, y: body.minY + 1), control: CGPoint(x: x + 3, y: mid))
            p.closeSubpath()
            return p
        }
        guard let arrow else { return p }
        let y = min(max(rect.minY + arrowY, body.minY + radius + half), body.maxY - radius - half)
        let base = arrow == .leading ? body.minX + 1 : body.maxX - 1
        let tip = arrow == .leading ? rect.minX : rect.maxX
        p.move(to: CGPoint(x: base, y: y - half))
        p.addQuadCurve(to: CGPoint(x: tip, y: y), control: CGPoint(x: (base + tip) / 2, y: y - 3))
        p.addQuadCurve(to: CGPoint(x: base, y: y + half), control: CGPoint(x: (base + tip) / 2, y: y + 3))
        p.closeSubpath()
        return p
    }
}

/// The ring around the button to click: a bright outline that breathes, and a halo that keeps
/// rippling out from it so the eye finds it.
private struct PulseRing: View {
    /// Room around the button for the halo.
    static let room: CGFloat = 16
    @State private var on = false
    @State private var ripple = false

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .stroke(Theme.accent, lineWidth: 3)
                .scaleEffect(ripple ? 1.25 : 1)
                .opacity(ripple ? 0 : 0.8)
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .stroke(Theme.accent, lineWidth: 4)
                .shadow(color: Theme.accent.opacity(0.9), radius: on ? 10 : 3)
                .opacity(on ? 1 : 0.7)
        }
        .padding(Self.room - 2)
        .onAppear {
            withAnimation(.easeInOut(duration: 0.7).repeatForever(autoreverses: true)) { on = true }
            withAnimation(.easeOut(duration: 1.2).repeatForever(autoreverses: false)) { ripple = true }
        }
    }
}

/// Where the App Store's Xcode page has its Get button, and how big Apple's sign-in sheet is, from
/// the window's frame (measured on CI's App Store, research/appstore-probe). Nil where a window's
/// layout isn't measured: the guide then shows a picture of the button instead of pointing.
enum AppStoreLayout {
    static func getButton(in w: CGRect) -> CGRect? { nil }

    /// Continue on the App Store's What's New sheet, at its bottom right: 77 × 27 points, 15 in
    /// from the right and bottom edges of the 480 × 565 sheet (measured on macOS 15). Nil for a
    /// sheet of another size.
    static func continueButton(in sheet: CGRect) -> CGRect? {
        guard abs(sheet.width - 480) <= 12, abs(sheet.height - 565) <= 12 else { return nil }
        return CGRect(x: sheet.maxX - 15 - 77, y: sheet.maxY - 15 - 27, width: 77, height: 27).insetBy(dx: -4, dy: -4)
    }

    static func isSignIn(_ sheet: CGRect) -> Bool { sheet.height >= 400 }
}

#if DEBUG
extension XcodeGuide {
    /// The bubble alone, at one stage, for screenshots.
    func showForDemo(_ job: Job, stage: Stage, xcodeMajor: Int) {
        self.job = job
        self.stage = stage
        self.xcodeMajor = xcodeMajor
        // Pointing up at Always Allow, as under macOS's window.
        if job == .keychain { arrowUpX = 94 + 51 }
        showPanel()
    }

    /// Signing in again: Apple's sign-in came and went, as if the player signed in.
    func pretendSignedInAgain() { signInSeen = true }

    /// Glovebox's card while a guide runs, without the guide (nil: back to not running, if only pretending).
    func pretendRunning(_ job: Job?) {
        guard let job else { if pretending { isRunning = false; pretending = false }; return }
        self.job = job
        isRunning = true
        pretending = true
    }

    var demoPanel: NSWindow? { panel }
    var demoRing: NSWindow? { ring?.isVisible == true ? ring : nil }
    /// What the bubble points up at (window-server coordinates), while it does.
    var demoPointedAt: CGRect? { arrowUpX != nil ? pointedAt : nil }
}
#endif
