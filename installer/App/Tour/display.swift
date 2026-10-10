import CoreGraphics
import Foundation

// display: switches the main display to its biggest mode up to 1920 points wide (1x where there's a
// choice), so the walkthrough has room for the windows and the captions. Prints what it found.
let id = CGMainDisplayID()
let options = [kCGDisplayShowDuplicateLowResolutionModes: kCFBooleanTrue] as CFDictionary
let modes = (CGDisplayCopyAllDisplayModes(id, options) as? [CGDisplayMode]) ?? []
print("display: now \(CGDisplayPixelsWide(id))x\(CGDisplayPixelsHigh(id)), \(modes.count) modes: "
      + Set(modes.map { "\($0.width)x\($0.height)" }).sorted().joined(separator: " "))
let fits = modes.filter { $0.isUsableForDesktopGUI() && $0.width <= 1920 && $0.height <= 1200 }
guard let best = fits.max(by: { ($0.width, -$0.pixelWidth) < ($1.width, -$1.pixelWidth) }),
      best.width > CGDisplayPixelsWide(id) else {
    print("display: keeping the current mode")
    exit(0)
}
var config: CGDisplayConfigRef?
CGBeginDisplayConfiguration(&config)
CGConfigureDisplayWithDisplayMode(config, id, best, nil)
let err = CGCompleteDisplayConfiguration(config, .forSession)
print("display: \(best.width)x\(best.height) (\(best.pixelWidth)x\(best.pixelHeight) px), result \(err.rawValue)")
