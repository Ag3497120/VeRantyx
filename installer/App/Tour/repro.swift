// The rehearsal's controls, as a small app of its own (io.github.glovebox-vr.repro), so whatever
// macOS remembers about them never lands on Glovebox. They show the checks can see what they look for.
//
//   Repro -helper <seconds>   a window, and itself started without one, the way build 2026100614 ran
//                             the engine (`Glovebox -spawn-disclaimed <tool>`): macOS 27 listed
//                             Glovebox in Login Items & Extensions as running in the background
//   Repro -hostname           a window, and the Mac's name looked up the way that made macOS ask to
//                             find devices on the local network (ProcessInfo.hostName)
//   Repro -spawn-disclaimed <tool> <arguments…>   (the helper) starts the tool as its own responsible
//                             process, waits for it and exits with its code
//
// Build: swiftc -O -o Repro.app/Contents/MacOS/Repro repro.swift (rehearsal.sh writes the Info.plist).
import AppKit
import Darwin

let args = CommandLine.arguments

func spawnDisclaimed(_ tool: [String]) -> Never {
    var attr: posix_spawnattr_t?
    posix_spawnattr_init(&attr)
    if let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "responsibility_spawnattrs_setdisclaim") {
        typealias Disclaim = @convention(c) (UnsafeMutablePointer<posix_spawnattr_t?>, Int32) -> Int32
        _ = unsafeBitCast(sym, to: Disclaim.self)(&attr, 1)
    }
    var pid: pid_t = 0
    let argv: [UnsafeMutablePointer<CChar>?] = tool.map { strdup($0) } + [nil]
    let envp: [UnsafeMutablePointer<CChar>?] = ProcessInfo.processInfo.environment.map { strdup("\($0.key)=\($0.value)") } + [nil]
    guard posix_spawn(&pid, tool[0], nil, &attr, argv, envp) == 0 else { exit(127) }
    var status: Int32 = 0
    while waitpid(pid, &status, 0) == -1, errno == EINTR {}
    exit((status >> 8) & 0xff)
}

if args.count > 2, args[1] == "-spawn-disclaimed" { spawnDisclaimed(Array(args.dropFirst(2))) }

final class Delegate: NSObject, NSApplicationDelegate {
    var window: NSWindow?
    var helper: Process?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let w = NSWindow(contentRect: NSRect(x: 240, y: 240, width: 360, height: 120), styleMask: [.titled, .closable],
                         backing: .buffered, defer: false)
        w.title = "Repro"
        w.makeKeyAndOrderFront(nil)
        window = w
        NSApp.activate(ignoringOtherApps: true)
        if let i = args.firstIndex(of: "-helper"), i + 1 < args.count, let me = Bundle.main.executablePath {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: me)
            p.arguments = ["-spawn-disclaimed", "/bin/sleep", args[i + 1]]
            try? p.run()
            helper = p
            print("repro: helper \(p.processIdentifier)")
        }
        if args.contains("-hostname") {
            print("repro: host name \(ProcessInfo.processInfo.hostName)")
        }
    }
}

let delegate = Delegate()
NSApplication.shared.delegate = delegate
NSApplication.shared.setActivationPolicy(.regular)
NSApplication.shared.run()
