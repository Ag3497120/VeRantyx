import Foundation
import Network

// Whether this process may use the local network (Apple's TN3179), asking no one: a TCP connection
// to the default gateway, and a Bonjour browse for CoreDevice's pairing service, the way devicectl
// finds a headset. Prints one line, "connect=<…> browse=<…>", each "allowed …" or "denied". Stays up
// 8 s, so macOS would have time to show its prompt (it doesn't for a process that exits at once).

func gateway() -> String? {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/sbin/route")
    p.arguments = ["-n", "get", "default"]
    let pipe = Pipe()
    p.standardOutput = pipe
    p.standardError = FileHandle.nullDevice
    guard (try? p.run()) != nil else { return nil }
    let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
    p.waitUntilExit()
    for line in out.split(separator: "\n") {
        if let r = line.range(of: "gateway: ") { return String(line[r.upperBound...]).trimmingCharacters(in: .whitespaces) }
    }
    return nil
}

let queue = DispatchQueue(label: "lanprobe")
var connect = "no answer"
var browse = "no answer"
let host = CommandLine.arguments.dropFirst().first ?? gateway() ?? "192.168.64.1"

let connection = NWConnection(host: NWEndpoint.Host(host), port: 9, using: .tcp)
connection.stateUpdateHandler = { state in
    switch state {
    case .ready: connect = "allowed (connected to \(host))"
    case .waiting(let error):
        connect = connection.currentPath?.unsatisfiedReason == .localNetworkDenied ? "denied" : "allowed (\(host): \(error))"
    case .failed(let error): connect = "allowed? (\(host) failed: \(error))"
    default: break
    }
}

let browser = NWBrowser(for: .bonjour(type: "_remotepairing._tcp", domain: nil), using: .tcp)
browser.stateUpdateHandler = { state in
    switch state {
    case .ready: if browse == "no answer" { browse = "allowed (browsing)" }
    case .waiting(let error):
        if case .dns(let code) = error, code == DNSServiceErrorType(kDNSServiceErr_PolicyDenied) { browse = "denied" }
        else { browse = "waiting (\(error))" }
    case .failed(let error): browse = "failed (\(error))"
    default: break
    }
}
browser.browseResultsChangedHandler = { results, _ in
    browse = "allowed (\(results.count) found)"
}

connection.start(queue: queue)
browser.start(queue: queue)
Thread.sleep(forTimeInterval: 8)
queue.sync { print("connect=\(connect) browse=\(browse)") }
