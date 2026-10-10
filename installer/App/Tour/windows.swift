import CoreGraphics
import Foundation
// Prints the on-screen windows of the named apps: owner, title (needs Screen Recording), layer, frame.
let owners = Set(CommandLine.arguments.dropFirst())
let list = (CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]]) ?? []
for w in list {
    let owner = w[kCGWindowOwnerName as String] as? String ?? "?"
    guard owners.isEmpty || owners.contains(owner) else { continue }
    let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
    print("window owner=\(owner) name=\(w[kCGWindowName as String] as? String ?? "-") layer=\(w[kCGWindowLayer as String] ?? "?") x=\(b["X"] ?? "?") y=\(b["Y"] ?? "?") w=\(b["Width"] ?? "?") h=\(b["Height"] ?? "?")")
}
