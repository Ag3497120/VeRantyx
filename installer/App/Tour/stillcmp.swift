import CoreGraphics
import Foundation
import ImageIO

// stillcmp <a.png> <b.png>: how many pixels differ by more than a hair (12 of 255 in any channel).
// Glass blurs dither by one step from frame to frame; a video behind the window moves whole areas.
// Exits 0 when under 0.1% of the pixels differ.
func pixels(_ path: String) -> (Int, Int, [UInt8])? {
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { return nil }
    let w = img.width, h = img.height
    var buf = [UInt8](repeating: 0, count: w * h * 4)
    let ok = buf.withUnsafeMutableBytes { raw -> Bool in
        guard let ctx = CGContext(data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
        return true
    }
    return ok ? (w, h, buf) : nil
}
let args = CommandLine.arguments
guard args.count == 3, let a = pixels(args[1]), let b = pixels(args[2]) else { print("stillcmp: can't read the pictures"); exit(2) }
guard a.0 == b.0, a.1 == b.1 else { print("stillcmp: sizes differ (\(a.0)x\(a.1), \(b.0)x\(b.1))"); exit(1) }
var differ = 0
var i = 0
while i < a.2.count {
    if abs(Int(a.2[i]) - Int(b.2[i])) > 12 || abs(Int(a.2[i + 1]) - Int(b.2[i + 1])) > 12 || abs(Int(a.2[i + 2]) - Int(b.2[i + 2])) > 12 { differ += 1 }
    i += 4
}
let share = Double(differ) / Double(a.0 * a.1)
print("stillcmp: \(differ) of \(a.0 * a.1) pixels differ (\(String(format: "%.3f", share * 100))%)")
exit(share < 0.001 ? 0 : 1)
