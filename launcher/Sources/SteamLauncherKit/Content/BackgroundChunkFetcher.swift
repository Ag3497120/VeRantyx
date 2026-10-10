#if !os(Linux)
import Foundation

/// Keeps the game downloading while the launcher is out of sight, suspended or closed, using the
/// system's background download service (a background `URLSession`).
///
/// The system downloads raw chunks into `StagedChunks`; the launcher's next install run unpacks
/// them (it holds the depot key). Work is split so both sides are always busy without overlap:
/// the app fetches from the front of the remaining list, the system takes a batch from the back.
/// Batches are created while the app is in front where possible, because the system starts tasks
/// created in the background only when it judges conditions good (power, Wi-Fi). When a batch
/// finishes the system wakes the app, which hands it the next batch from the saved queue.
public final class BackgroundChunkFetcher: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    public let identifier: String
    public let staged: StagedChunks
    /// Chunks per batch (Steam chunks are up to 1 MB). Large enough to keep going for a long
    /// while between wake-ups, small enough not to bog down the system's download service.
    public var batchSize = 3000
    public var log: @Sendable (String) -> Void = { _ in }

    private let queueURL: URL
    private let lock = NSLock()
    private var finishEvents: (() -> Void)?
    private var _session: URLSession?

    struct Queue: Codable {
        var servers: [URL]
        var chunks: [ChunkRequest]
    }

    public init(identifier: String, installRoot: URL) {
        self.identifier = identifier
        self.staged = StagedChunks(installRoot: installRoot)
        self.queueURL = installRoot.appendingPathComponent(".launcher-background-queue.json")
        super.init()
    }

    private var session: URLSession {
        lock.lock(); defer { lock.unlock() }
        if let s = _session { return s }
        let cfg = URLSessionConfiguration.background(withIdentifier: identifier)
        cfg.sessionSendsLaunchEvents = true
        cfg.isDiscretionary = false
        cfg.httpMaximumConnectionsPerHost = 8
        cfg.requestCachePolicy = .reloadIgnoringLocalCacheData
        cfg.urlCache = nil
        let queue = OperationQueue()
        queue.maxConcurrentOperationCount = 1
        let s = URLSession(configuration: cfg, delegate: self, delegateQueue: queue)
        _session = s
        return s
    }

    /// Reconnects to the system's downloads at launch, so chunks that finished while the app
    /// was away are handed over.
    public func activate() { _ = session }

    /// The app delegate's `handleEventsForBackgroundURLSession`: the system woke the app to
    /// deliver finished downloads. `completion` is called once they are all stored.
    public func handleEvents(completion: @escaping () -> Void) {
        lock.lock(); finishEvents = completion; lock.unlock()
        activate()
    }

    /// Names of chunks the system is still fetching.
    public func pendingNames() async -> Set<String> {
        Set(await session.allTasks.filter { $0.state == .running || $0.state == .suspended }.compactMap(\.taskDescription))
    }

    /// Saves what is left to fetch and, unless a batch is already running, hands the system the
    /// tail end of it. `firstBatchShare` caps that batch to a share of what's left, so the app
    /// keeps the front for itself while it is in the foreground.
    public func plan(_ remaining: [ChunkRequest], servers: [URL], firstBatchShare: Double = 1) async {
        let pending = await pendingNames()
        save(Queue(servers: servers, chunks: remaining.filter { !pending.contains($0.name) }))
        guard pending.isEmpty else { return }
        enqueueNextBatch(limit: min(batchSize, Int(Double(remaining.count) * firstBatchShare)))
    }

    /// Cancels the system's downloads and forgets the queue: the app is back and finishes on its own.
    public func stop() async {
        try? FileManager.default.removeItem(at: queueURL)
        for t in await session.allTasks { t.cancel() }
    }

    private func enqueueNextBatch(limit: Int) {
        guard limit > 0, var q = load(), let base = q.servers.first else { return }
        let present = staged.names()
        var batch: [ChunkRequest] = []
        while batch.count < limit, let c = q.chunks.popLast() {
            if !present.contains(c.name) { batch.append(c) }
        }
        save(q)
        try? FileManager.default.createDirectory(at: staged.folder, withIntermediateDirectories: true)
        for c in batch {
            guard let url = URL(string: c.path, relativeTo: base)?.absoluteURL else { continue }
            let t = session.downloadTask(with: url)
            t.taskDescription = c.name
            t.countOfBytesClientExpectsToSend = 300
            t.countOfBytesClientExpectsToReceive = Int64(c.bytes) + 1024
            t.resume()
        }
        log("background: queued \(batch.count) chunks, \(q.chunks.count) waiting")
    }

    private func load() -> Queue? {
        (try? Data(contentsOf: queueURL)).flatMap { try? JSONDecoder().decode(Queue.self, from: $0) }
    }

    private func save(_ q: Queue) {
        if q.chunks.isEmpty { try? FileManager.default.removeItem(at: queueURL); return }
        try? JSONEncoder().encode(q).write(to: queueURL, options: .atomic)
    }

    // MARK: URLSessionDownloadDelegate

    public func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        // The file at `location` is deleted when this returns, so it is moved now.
        guard let name = downloadTask.taskDescription,
              (downloadTask.response as? HTTPURLResponse)?.statusCode == 200 else { return }
        let fm = FileManager.default
        let dest = staged.url(name)
        try? fm.createDirectory(at: staged.folder, withIntermediateDirectories: true)
        try? fm.removeItem(at: dest)
        do { try fm.moveItem(at: location, to: dest) } catch { log("background: couldn't keep \(name): \(error)") }
    }

    public func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        // A failed chunk is left out of the staging folder; the app fetches it next time it runs.
        if let error = error as? URLError, error.code != .cancelled {
            log("background: \(task.taskDescription ?? "?") failed: \(error.code.rawValue)")
        }
    }

    public func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
        // The batch is done and everything is stored: line up the next one before the app sleeps.
        enqueueNextBatch(limit: batchSize)
        lock.lock()
        let done = finishEvents
        finishEvents = nil
        lock.unlock()
        DispatchQueue.main.async { done?() }
    }
}
#endif
