import Foundation

/// What to do with each file of a depot.
public enum FilePolicy: Sendable {
    case download
    /// Create an empty file with the right name. The Alyx port does this for Linux ELF
    /// libraries: their translated, signed copies ship inside the app bundle.
    case placeholder
    case skip
}

public struct DownloadProgress: Sendable, Equatable {
    public init() {}
    public var totalBytes: UInt64 = 0
    public var doneBytes: UInt64 = 0
    public var totalFiles = 0
    public var doneFiles = 0
    public var bytesPerSecond: Double = 0
    public var currentFile = ""

    public var fraction: Double { totalBytes == 0 ? 0 : Double(doneBytes) / Double(totalBytes) }
    public var secondsLeft: Double? { bytesPerSecond > 1 ? Double(totalBytes - doneBytes) / bytesPerSecond : nil }
}

/// Downloads a depot into a folder, the way the Steam client does: file by file, chunk by chunk,
/// many chunks in flight, every chunk verified. It can stop at any moment and resume later;
/// finished chunks are recorded in a journal next to the files.
public final class DepotDownloader: @unchecked Sendable {
    public let cdn: CDNClient
    public let depotID: UInt32
    public let key: Data
    public let manifest: DepotManifest
    public let root: URL
    public var parallelChunks = 16
    public var policy: (DepotManifest.File) -> FilePolicy = { _ in .download }
    /// Where a depot path lands under `root`; nil leaves the file out.
    public var localPath: (String) -> String? = { $0 }

    private let journalURL: URL
    private var done = Set<String>()
    private let lock = NSLock()

    public init(cdn: CDNClient, depotID: UInt32, key: Data, manifest: DepotManifest, root: URL) {
        self.cdn = cdn
        self.depotID = depotID
        self.key = key
        self.manifest = manifest
        self.root = root
        self.journalURL = root.appendingPathComponent(".steam-journal-\(depotID)-\(manifest.manifestID)")
    }

    private func key(_ f: DepotManifest.File, _ c: DepotManifest.Chunk) -> String { "\(f.name)\t\(c.offset)" }

    private func loadJournal() {
        guard let text = try? String(contentsOf: journalURL, encoding: .utf8) else { return }
        done = Set(text.split(separator: "\n").map(String.init))
    }

    private func record(_ entries: [String]) {
        lock.lock(); defer { lock.unlock() }
        entries.forEach { done.insert($0) }
        let line = Data((entries.joined(separator: "\n") + "\n").utf8)
        if let h = try? FileHandle(forWritingTo: journalURL) {
            h.seekToEndOfFile(); h.write(line); try? h.close()
        } else {
            try? line.write(to: journalURL)
        }
    }

    private func wanted(_ f: DepotManifest.File) -> Bool {
        !f.isDirectory && !f.isSymlink && localPath(f.name) != nil && policy(f) == .download
    }

    /// Bytes this download will fetch in total, given the policy.
    public var plannedBytes: UInt64 {
        manifest.files.filter(wanted).reduce(0) { $0 + $1.size }
    }

    /// Chunks still to fetch, in download order: not yet written (per the journal) and not already
    /// staged. One entry per chunk ID, even where several files share a chunk.
    public func remaining(staged: StagedChunks? = nil) -> [ChunkRequest] {
        loadJournal()
        let present = staged?.names() ?? []
        var seen = Set<String>()
        var out: [ChunkRequest] = []
        for f in manifest.files where wanted(f) {
            for c in f.chunks where !done.contains(key(f, c)) {
                let name = StagedChunks.name(depotID: depotID, c)
                guard !present.contains(name), seen.insert(name).inserted else { continue }
                out.append(ChunkRequest(name: name, path: CDNClient.chunkPath(depotID: depotID, c), bytes: c.compressedSize))
            }
        }
        return out
    }

    /// Downloads whatever the journal doesn't have yet. Chunks waiting in `staged` are unpacked
    /// first instead of fetched; chunks named in `skipping` (still on their way through the
    /// system's background downloader) are left for a later run.
    public func run(staged: StagedChunks? = nil, skipping: Set<String> = [],
                    progress: @escaping @Sendable (DownloadProgress) -> Void) async throws {
        let fm = FileManager.default
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        loadJournal()

        var p = DownloadProgress()
        var paths: [String: URL] = [:]
        var work: [(DepotManifest.File, DepotManifest.Chunk)] = []
        for f in manifest.files {
            guard let rel = localPath(f.name) else { continue }
            let url = root.appendingPathComponent(rel)
            if f.isDirectory { try fm.createDirectory(at: url, withIntermediateDirectories: true); continue }
            try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            if f.isSymlink, let target = f.linkTarget {
                try? fm.removeItem(at: url)
                try fm.createSymbolicLink(atPath: url.path, withDestinationPath: target)
                continue
            }
            switch policy(f) {
            case .skip: continue
            case .placeholder:
                if !fm.fileExists(atPath: url.path) { fm.createFile(atPath: url.path, contents: Data()) }
                continue
            case .download: break
            }
            p.totalFiles += 1
            paths[f.name] = url
            p.totalBytes += f.size
            if !fm.fileExists(atPath: url.path) { fm.createFile(atPath: url.path, contents: nil) }
            let h = try FileHandle(forWritingTo: url)
            try h.truncate(atOffset: f.size)
            try h.close()
            let pending = f.chunks.filter { !done.contains(key(f, $0)) }
            p.doneBytes += f.size - pending.reduce(0) { $0 + UInt64($1.size) }
            if pending.isEmpty { p.doneFiles += 1 }
            work += pending.map { (f, $0) }
        }
        progress(p)

        let state = ProgressState(p)
        let started = Date()
        let startBytes = p.doneBytes
        var remainingPerFile: [String: Int] = [:]
        for (f, _) in work { remainingPerFile[f.name, default: 0] += 1 }
        let targets = paths

        /// Records finished chunks and reports progress.
        func finished(_ items: [(DepotManifest.File, DepotManifest.Chunk)], measureSpeed: Bool) {
            record(items.map { key($0.0, $0.1) })
            for (f, c) in items {
                remainingPerFile[f.name]! -= 1
                let snapshot = state.update { s in
                    s.doneBytes += UInt64(c.size)
                    s.currentFile = f.name
                    if remainingPerFile[f.name] == 0 { s.doneFiles += 1 }
                    let dt = Date().timeIntervalSince(started)
                    if measureSpeed, dt > 1 { s.bytesPerSecond = Double(s.doneBytes - startBytes) / dt }
                }
                progress(snapshot)
            }
        }

        // 1. Unpack what the background downloader already fetched, on every core.
        if let staged {
            let present = staged.names()
            var groups: [String: [(DepotManifest.File, DepotManifest.Chunk)]] = [:]
            var rest: [(DepotManifest.File, DepotManifest.Chunk)] = []
            for item in work {
                let name = StagedChunks.name(depotID: depotID, item.1)
                if present.contains(name) { groups[name, default: []].append(item) } else { rest.append(item) }
            }
            if !groups.isEmpty {
                try await withThrowingTaskGroup(of: (String, [(DepotManifest.File, DepotManifest.Chunk)], Bool).self) { group in
                    var it = groups.makeIterator()
                    func addNext() -> Bool {
                        guard let (name, items) = it.next() else { return false }
                        group.addTask { [self] in
                            do {
                                let plain = try CDNClient.decode(Data(contentsOf: staged.url(name)), items[0].1, key: self.key)
                                for (f, c) in items { try Self.write(plain, to: targets[f.name]!, at: c.offset) }
                                return (name, items, true)
                            } catch {
                                return (name, items, false)
                            }
                        }
                        return true
                    }
                    for _ in 0..<max(2, ProcessInfo.processInfo.activeProcessorCount) { if !addNext() { break } }
                    while let (name, items, ok) = try await group.next() {
                        try? fm.removeItem(at: staged.url(name))
                        // A bad staged chunk (cut short, or an error page) is simply fetched again.
                        if ok { finished(items, measureSpeed: false) } else { rest += items }
                        try Task.checkCancellation()
                        _ = addNext()
                    }
                }
            }
            work = rest
        }

        // 2. Fetch the rest from the CDN, leaving chunks the system is still fetching.
        if !skipping.isEmpty {
            work.removeAll { skipping.contains(StagedChunks.name(depotID: depotID, $0.1)) }
        }
        try await withThrowingTaskGroup(of: (DepotManifest.File, DepotManifest.Chunk).self) { group in
            var it = work.makeIterator()
            func addNext() -> Bool {
                guard let (f, c) = it.next() else { return false }
                let url = targets[f.name]!
                group.addTask { [self] in
                    let data = try await cdn.chunk(depotID: depotID, c, key: self.key)
                    try Self.write(data, to: url, at: c.offset)
                    return (f, c)
                }
                return true
            }
            for _ in 0..<parallelChunks { if !addNext() { break } }
            var batch: [(DepotManifest.File, DepotManifest.Chunk)] = []
            while let item = try await group.next() {
                batch.append(item)
                if batch.count >= 32 { finished(batch, measureSpeed: true); batch.removeAll() }
                try Task.checkCancellation()
                _ = addNext()
            }
            if !batch.isEmpty { finished(batch, measureSpeed: true) }
        }

        for f in manifest.files where f.isExecutable && policy(f) == .download {
            if let url = paths[f.name] { try? fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: url.path) }
        }
    }

    private static func write(_ data: Data, to url: URL, at offset: UInt64) throws {
        let h = try FileHandle(forWritingTo: url)
        defer { try? h.close() }
        try h.seek(toOffset: offset)
        h.write(data)
    }

    /// Checks every downloaded file against the manifest's SHA-1 (the Steam client's "verify").
    public func verify(progress: ((Int, Int) -> Void)? = nil) throws -> [String] {
        var bad: [String] = []
        let files = manifest.files.filter(wanted)
        for (n, f) in files.enumerated() {
            let url = root.appendingPathComponent(localPath(f.name)!)
            // An empty file's hash in the manifest is all zeros, not the SHA-1 of nothing.
            guard let data = try? Data(contentsOf: url, options: .mappedIfSafe), UInt64(data.count) == f.size,
                  f.size == 0 || f.sha.isEmpty || Codec.sha1(data) == f.sha else {
                bad.append(f.name)
                continue
            }
            progress?(n + 1, files.count)
        }
        return bad
    }
}

private final class ProgressState: @unchecked Sendable {
    private var value: DownloadProgress
    private let lock = NSLock()
    init(_ v: DownloadProgress) { value = v }
    func update(_ f: (inout DownloadProgress) -> Void) -> DownloadProgress {
        lock.lock(); defer { lock.unlock() }
        f(&value)
        return value
    }
}
