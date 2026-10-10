// XboxApp — Xbox Cloud Gaming as a flat visionOS window.
//
// The Quest "Xbox" app (com.xbox.pwa) is Meta's PWA wrapper: no native code at
// all, just a shell that points the system browser at xbox.com/play. There is
// nothing in it for Klepton's translator to run — so the faithful port is the
// same shell built natively: one window, one WKWebView, the same START_URL.
// Controllers reach the page through WebKit's own Gamepad API support, which is
// how the PWA hears them on Quest too.
import SwiftUI
import WebKit
import GameController

@main
struct XboxApp: App {
    init() {
        // Pin the PROCESS locale to English. WKWebView derives Accept-Language
        // from the app's preferred languages, not the page's; on a device set
        // to Spanish every request said es-ES and xbox.com answered in kind.
        UserDefaults.standard.set(["en-US"], forKey: "AppleLanguages")
    }
    var body: some Scene {
        WindowGroup {
            XboxWebView()
                .ignoresSafeArea()
                // The PWA's own manifest asks for landscape; a 16:10 window is
                // the same shape the Quest panel gives it.
                .frame(minWidth: 960, minHeight: 600)
                // visionOS 2 routes a connected controller to the SYSTEM pointer
                // (sticks move the cursor, A/trigger click) unless a view in the
                // scene explicitly claims game-controller events. Without this,
                // only incidental buttons (the d-pad) fall through to the app and
                // the sticks + A never reach GCController. Claiming .gamepad hands
                // the whole pad to the app, which the bridge forwards to the page.
                .handlesGameControllerEvents(matching: .gamepad)
        }
        .defaultSize(width: 1600, height: 1000)
    }
}

struct XboxWebView: UIViewRepresentable {
    // The wrapper's com.oculus.pwa.START_URL, verbatim.
    // en-US pinned in the path: xbox.com geo-routes a bare /play/ to the
    // visitor's country locale (es-ES from Spain) before headers even matter.
    static let startURL = URL(string: "https://www.xbox.com/en-US/play/")!

    func makeUIView(context: Context) -> WKWebView {
        let cfg = WKWebViewConfiguration()
        // Persistent store: the Microsoft sign-in must survive relaunches, the
        // way the PWA's browser profile does on Quest.
        cfg.websiteDataStore = .default()
        cfg.allowsInlineMediaPlayback = true
        cfg.mediaTypesRequiringUserActionForPlayback = []
        // Present the QUEST browser, not Safari: xbox.com/play keys its Quest
        // experience (controller-first UI, the layout the PWA gets) off the
        // "OculusBrowser" token, and detects Apple through navigator fields as
        // well as the UA. The user script below runs before any page script and
        // masks the ones WebKit exposes: platform reads Linux, touch reads
        // absent. navigator.userAgent itself follows customUserAgent for free.
        let spoof = WKUserScript(source: """
            (() => {
              const def = (o, k, v) => { try {
                Object.defineProperty(o, k, { get: () => v, configurable: true });
              } catch (e) {} };
              def(navigator, 'platform', 'Linux x86_64');
              def(navigator, 'maxTouchPoints', 0);
              def(navigator, 'vendor', 'Google Inc.');
              def(navigator, 'language', 'en-US');
              def(navigator, 'languages', ['en-US', 'en']);
              // The Quest path probes microphone devices for party chat. In a
              // WKWebView without mic entitlements navigator.mediaDevices does
              // not exist at all, and the app's probe then throws "undefined is
              // not an object" as a fullscreen error. Answer like a browser
              // with no devices attached instead: enumerate to an empty list,
              // refuse capture the way a denied permission would.
              if (!navigator.mediaDevices) {
                const md = {
                  enumerateDevices: () => Promise.resolve([]),
                  getUserMedia: () => Promise.reject(
                      new DOMException('Permission denied', 'NotAllowedError')),
                  getSupportedConstraints: () => ({}),
                  addEventListener: () => {}, removeEventListener: () => {},
                  ondevicechange: null,
                };
                def(navigator, 'mediaDevices', md);
              }
            })();
            """, injectionTime: .atDocumentStart, forMainFrameOnly: false)
        cfg.userContentController.addUserScript(spoof)
        // A native GameController -> JS Gamepad API bridge. xbox.com/play's
        // controller-first (Quest) UI drives entirely off navigator.getGamepads()
        // and the gamepadconnected event; WebKit's own Gamepad API in a visionOS
        // WKWebView does not surface the paired controller to the page (it works
        // in Safari, not here), so the page saw no pad and every button was dead.
        // Rather than depend on that, synthesise the API from the live
        // GCController the app receives directly: the Coordinator polls the
        // extended gamepad and pushes W3C "standard"-mapping state into
        // window.__klPad below. The override yields to a real WebKit gamepad if
        // one ever appears, so this cannot double up.
        //
        // For this to work the app must OWN the controller, which is why the
        // Info.plist sets GCSupportsControllerUserInteraction = NO. With it YES,
        // visionOS claims the pad for its own focus/pointer UI (thumbstick moves
        // a cursor, A/trigger is a system "select") and never hands it to the app
        // as a GCController — GCController.controllers() comes back empty, pump()
        // no-ops, and the page sees no pad at all. NO gives the app the raw
        // controller so every button reaches the shim, and the page's own
        // controller-first UI does the navigating.
        //
        // Two robustness points learned the hard way in a WKWebView:
        //   - getGamepads() is defined on BOTH navigator and Navigator.prototype,
        //     via defineProperty, because a plain `navigator.getGamepads = fn`
        //     can be shadowed/ignored and the page may read either object.
        //   - the gamepadconnected event is dispatched with a fallback plain
        //     Event when the GamepadEvent constructor is absent (it is, when the
        //     WebView's own Gamepad API is off) — many pages don't start polling
        //     until they see that event.
        let padShim = WKUserScript(source: #"""
            (() => {
              const st = { b: new Array(17).fill(0), a: [0, 0, 0, 0], t: 0,
                           on: false };
              const pad = () => ({
                id: 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 02ea)',
                index: 0, connected: true, mapping: 'standard', timestamp: st.t,
                axes: st.a.slice(),
                buttons: st.b.map(v => ({ pressed: v > 0.5, touched: v > 0.1, value: v })),
                hapticActuators: [], vibrationActuator: null,
              });
              const fireConnected = () => {
                let ev;
                try { ev = new GamepadEvent('gamepadconnected', { gamepad: pad() }); }
                catch (e) {
                  ev = new Event('gamepadconnected');
                  try { Object.defineProperty(ev, 'gamepad', { value: pad() }); }
                  catch (e2) { ev.gamepad = pad(); }
                }
                try { window.dispatchEvent(ev); } catch (e) {}
              };
              window.__klPad = (b, a) => {
                st.b = b; st.a = a; st.t = performance.now();
                if (!st.on) { st.on = true; fireConnected(); }
              };
              const orig = navigator.getGamepads ? navigator.getGamepads.bind(navigator) : null;
              const getGP = function () {
                const real = orig ? orig() : [];
                if (real && Array.prototype.some.call(real, g => g)) return real;
                return st.on ? [pad(), null, null, null] : [null, null, null, null];
              };
              const install = (o) => { try {
                Object.defineProperty(o, 'getGamepads',
                  { value: getGP, configurable: true, writable: true });
              } catch (e) {} };
              install(navigator);
              if (window.Navigator) install(Navigator.prototype);
              try { if ('webkitGetGamepads' in navigator) navigator.webkitGetGamepads = getGP; }
              catch (e) {}
              // Register THIS frame so native can push pad state into it. The
              // in-game stream reads getGamepads() in its own child frame, which
              // the main-frame push never reaches; re-announce for a few seconds
              // to beat frame/handler timing races.
              try {
                const fid = 'f' + Math.random().toString(36).slice(2);
                const reg = () => { try {
                  window.webkit.messageHandlers.klreg.postMessage(fid);
                } catch (e) {} };
                reg();
                let n = 0;
                const iv = setInterval(() => { reg(); if (++n > 20) clearInterval(iv); }, 250);
              } catch (e) {}
            })();
            """#, injectionTime: .atDocumentStart, forMainFrameOnly: false)
        cfg.userContentController.addUserScript(padShim)
        // Each frame (main + the in-game stream's child frame) posts here so the
        // Coordinator learns its WKFrameInfo and can push pad state into it.
        cfg.userContentController.add(context.coordinator, name: "klreg")
        let web = WKWebView(frame: .zero, configuration: cfg)
        context.coordinator.attach(web)
        web.customUserAgent = "Mozilla/5.0 (X11; Linux x86_64; Quest 3) "
            + "AppleWebKit/537.36 (KHTML, like Gecko) OculusBrowser/33.0.0.x.0 "
            + "SamsungBrowser/4.0 Chrome/126.0.0.0 VR Safari/537.36"
        web.navigationDelegate = context.coordinator
        web.uiDelegate = context.coordinator
        web.load(URLRequest(url: Self.startURL))
        return web
    }

    func updateUIView(_ web: WKWebView, context: Context) {}
    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator: NSObject, WKNavigationDelegate, WKUIDelegate,
                             WKScriptMessageHandler {
        private weak var web: WKWebView?
        private var timer: Timer?
        // Frames that registered via the klreg handler, keyed by the id the
        // frame's shim generated. Includes the main frame and any child frames
        // (the in-game stream). Stale entries are dropped when a push errors.
        private var frames: [String: WKFrameInfo] = [:]

        func userContentController(_ ucc: WKUserContentController,
                                   didReceive message: WKScriptMessage) {
            guard message.name == "klreg", let fid = message.body as? String else { return }
            frames[fid] = message.frameInfo
        }

        // Start polling the controller once the WebView exists. 60 Hz is the rate
        // getGamepads() is expected to be sampled at; each tick pushes the live
        // state into the page's __klPad shim. Pumped even with no controller (a
        // cheap no-op) so hot-plugging one mid-session just starts working.
        func attach(_ web: WKWebView) {
            self.web = web
            // Claim any already-paired controller for this app (give it a player
            // index) and start discovery so a Bluetooth pad the system knows
            // about is delivered to the process; also catch late connects.
            GCController.startWirelessControllerDiscovery(completionHandler: {})
            for c in GCController.controllers() { c.playerIndex = .index1 }
            NotificationCenter.default.addObserver(
                forName: .GCControllerDidConnect, object: nil, queue: .main) { note in
                (note.object as? GCController)?.playerIndex = .index1
            }
            timer?.invalidate()
            timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0,
                                         repeats: true) { [weak self] _ in self?.pump() }
        }

        private func pump() {
            guard let web = web else { return }
            let controllers = GCController.controllers()
            guard let gp = controllers.lazy.compactMap({ $0.extendedGamepad }).first
            else { return }
            func p(_ b: GCControllerButtonInput?) -> Float { (b?.isPressed ?? false) ? 1 : 0 }
            func v(_ b: GCControllerButtonInput?) -> Float { b?.value ?? 0 }
            // W3C "standard" mapping order — what xbox.com/play reads by index.
            let b: [Float] = [
                p(gp.buttonA), p(gp.buttonB), p(gp.buttonX), p(gp.buttonY),
                p(gp.leftShoulder), p(gp.rightShoulder),
                v(gp.leftTrigger), v(gp.rightTrigger),
                p(gp.buttonOptions), p(gp.buttonMenu),
                p(gp.leftThumbstickButton), p(gp.rightThumbstickButton),
                p(gp.dpad.up), p(gp.dpad.down), p(gp.dpad.left), p(gp.dpad.right),
                p(gp.buttonHome),
            ]
            // Standard axes are +right/+DOWN; GCController thumbsticks are +up, so
            // the Y axes are negated.
            let a: [Float] = [
                gp.leftThumbstick.xAxis.value, -gp.leftThumbstick.yAxis.value,
                gp.rightThumbstick.xAxis.value, -gp.rightThumbstick.yAxis.value,
            ]
            let bs = b.map { String(format: "%.3f", $0) }.joined(separator: ",")
            let axs = a.map { String(format: "%.4f", $0) }.joined(separator: ",")
            let padJS = "window.__klPad&&window.__klPad([\(bs)],[\(axs)])"
            // Main frame (legacy call = main frame).
            web.evaluateJavaScript(padJS, completionHandler: nil)
            // Every registered CHILD frame — the in-game stream reads the pad in
            // its own frame, so the main-frame push alone leaves it dead.
            for (fid, info) in frames where !info.isMainFrame {
                // The in:contentWorld: overload reports via Result, not (Any?, Error?).
                web.evaluateJavaScript(padJS, in: info, contentWorld: .page) { [weak self] result in
                    if case .failure = result { self?.frames[fid] = nil }
                }
            }
        }

        // The Microsoft login flow opens target=_blank windows. There is no
        // second window here; load them in place, which is what the PWA's
        // single-activity browser does with them as well.
        func webView(_ webView: WKWebView,
                     createWebViewWith configuration: WKWebViewConfiguration,
                     for navigationAction: WKNavigationAction,
                     windowFeatures: WKWindowFeatures) -> WKWebView? {
            if let url = navigationAction.request.url {
                webView.load(URLRequest(url: url))
            }
            return nil
        }
    }
}
