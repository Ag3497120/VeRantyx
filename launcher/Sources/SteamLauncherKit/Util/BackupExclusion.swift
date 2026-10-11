import Foundation

/// Keeps what can be downloaded or built again out of the device's iCloud backup. The game (about
/// 60 GB for Half-Life: Alyx) lives in the app's Documents folder, which the system backs up unless
/// a file or folder is marked "excluded from backup". A marked folder leaves out everything in it,
/// so only the folders on the way to something worth keeping (the saves) are opened; everything
/// beside them is marked as a whole.
public enum BackupExclusion {
    /// Marks every item under `root` excluded from backup, except the `keeping` paths (relative to
    /// `root`, any case) and the folders on the way to them, which stay in the backup with only
    /// what is kept inside. Returns what is out of the backup now. Cheap enough for every launch:
    /// it lists only the folders on the way to the kept paths.
    @discardableResult
    public static func apply(under root: URL, keeping: [String]) -> [URL] {
        var out: [URL] = []
        walk(root, keeping.map { $0.lowercased().split(separator: "/").map(String.init) }, &out)
        return out
    }

    private static func walk(_ folder: URL, _ keep: [[String]], _ out: inout [URL]) {
        guard let items = try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil) else { return }
        for item in items {
            let name = item.lastPathComponent.lowercased()
            let inside = keep.filter { $0.first == name }.map { Array($0.dropFirst()) }
            if inside.contains(where: \.isEmpty) {
                continue  // kept, with everything in it
            } else if !inside.isEmpty {
                walk(item, inside, &out)  // on the way to something kept
            } else if exclude(item) {
                out.append(item)
            }
        }
    }

    /// Marks one item (again, if it was: a file replaced by a download loses the mark); true when it is marked.
    static func exclude(_ item: URL) -> Bool {
        #if canImport(Darwin)
        var item = item
        var v = URLResourceValues()
        v.isExcludedFromBackup = true
        return (try? item.setResourceValues(v)) != nil
        #else
        return true  // no backups to keep anything out of (the Linux tests check what would be marked)
        #endif
    }

    /// Whether the item is marked, read fresh, where the platform has the mark.
    static func isExcluded(_ item: URL) -> Bool? {
        #if canImport(Darwin)
        var item = item
        item.removeAllCachedResourceValues()
        return (try? item.resourceValues(forKeys: [.isExcludedFromBackupKey]))?.isExcludedFromBackup
        #else
        return nil
        #endif
    }
}
