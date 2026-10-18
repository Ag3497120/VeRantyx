// Draws the app's icon (a headset visor over a grid floor, in the site's sky-to-violet colours) at every size an
// .icns needs: swift MacApp/make_icon.swift <folder>.iconset, then iconutil -c icns <folder>.iconset.
import AppKit

func draw(_ px: Int) -> NSBitmapImageRep {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    let ctx = NSGraphicsContext(bitmapImageRep: rep)!
    NSGraphicsContext.current = ctx
    let g = ctx.cgContext
    let s = CGFloat(px) / 1024
    g.scaleBy(x: s, y: s)
    let sky = NSColor(red: 0.055, green: 0.647, blue: 0.914, alpha: 1)
    let violet = NSColor(red: 0.655, green: 0.545, blue: 0.980, alpha: 1)

    // The rounded square of macOS icons (824 of 1024, with room for the shadow).
    let tile = CGRect(x: 100, y: 100, width: 824, height: 824)
    let tilePath = CGPath(roundedRect: tile, cornerWidth: 186, cornerHeight: 186, transform: nil)
    g.saveGState()
    g.setShadow(offset: CGSize(width: 0, height: -12), blur: 28, color: NSColor.black.withAlphaComponent(0.35).cgColor)
    g.addPath(tilePath); g.setFillColor(NSColor.black.cgColor); g.fillPath()
    g.restoreGState()
    g.saveGState()
    g.addPath(tilePath); g.clip()
    let bg = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                        colors: [NSColor(red: 0.07, green: 0.06, blue: 0.20, alpha: 1).cgColor, NSColor(red: 0.02, green: 0.09, blue: 0.20, alpha: 1).cgColor] as CFArray,
                        locations: [0, 1])!
    g.drawLinearGradient(bg, start: CGPoint(x: 100, y: 924), end: CGPoint(x: 924, y: 100), options: [])
    // A glow behind the visor.
    let glow = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                          colors: [violet.withAlphaComponent(0.55).cgColor, violet.withAlphaComponent(0).cgColor] as CFArray, locations: [0, 1])!
    g.drawRadialGradient(glow, startCenter: CGPoint(x: 512, y: 560), startRadius: 0, endCenter: CGPoint(x: 512, y: 560), endRadius: 420, options: [])
    // The floor: lines receding to the horizon.
    g.setLineWidth(5)
    for i in 0...12 {
        let x = 100 + CGFloat(i) * 824 / 12
        g.move(to: CGPoint(x: 512 + (x - 512) * 0.18, y: 420)); g.addLine(to: CGPoint(x: x + (x - 512) * 0.9, y: 60))
    }
    for (i, y) in [400, 360, 300, 215, 100].enumerated() {
        g.move(to: CGPoint(x: 0, y: CGFloat(y))); g.addLine(to: CGPoint(x: 1024, y: CGFloat(y)))
        _ = i
    }
    g.setStrokeColor(sky.withAlphaComponent(0.32).cgColor); g.strokePath()
    g.restoreGState()

    // The visor: a wide rounded band with two lenses, lit from above.
    let visor = CGRect(x: 205, y: 455, width: 614, height: 280)
    let visorPath = CGPath(roundedRect: visor, cornerWidth: 140, cornerHeight: 140, transform: nil)
    g.saveGState()
    g.setShadow(offset: CGSize(width: 0, height: -10), blur: 30, color: NSColor.black.withAlphaComponent(0.5).cgColor)
    g.addPath(visorPath)
    g.setFillColor(NSColor(white: 0.96, alpha: 1).cgColor); g.fillPath()
    g.restoreGState()
    g.saveGState()
    g.addPath(visorPath); g.clip()
    let shine = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(),
                           colors: [NSColor.white.cgColor, NSColor(white: 0.80, alpha: 1).cgColor] as CFArray, locations: [0, 1])!
    g.drawLinearGradient(shine, start: CGPoint(x: 0, y: visor.maxY), end: CGPoint(x: 0, y: visor.minY), options: [])
    g.restoreGState()
    // (the lenses: the picture inside, sky to violet)
    let lenses = CGMutablePath()
    lenses.addRoundedRect(in: CGRect(x: 250, y: 500, width: 245, height: 190), cornerWidth: 95, cornerHeight: 95)
    lenses.addRoundedRect(in: CGRect(x: 529, y: 500, width: 245, height: 190), cornerWidth: 95, cornerHeight: 95)
    g.saveGState()
    g.addPath(lenses); g.clip()
    let lens = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: [sky.cgColor, violet.cgColor] as CFArray, locations: [0, 1])!
    g.drawLinearGradient(lens, start: CGPoint(x: 250, y: 690), end: CGPoint(x: 774, y: 500), options: [])
    // (a highlight across both)
    g.setFillColor(NSColor.white.withAlphaComponent(0.28).cgColor)
    g.fill(CGRect(x: 240, y: 640, width: 560, height: 26))
    g.restoreGState()
    // (the bridge of the nose)
    g.saveGState()
    let nose = CGMutablePath()
    nose.move(to: CGPoint(x: 470, y: 455)); nose.addQuadCurve(to: CGPoint(x: 554, y: 455), control: CGPoint(x: 512, y: 512))
    nose.closeSubpath()
    g.addPath(nose)
    g.setFillColor(NSColor(red: 0.07, green: 0.07, blue: 0.18, alpha: 1).cgColor); g.fillPath()
    g.restoreGState()
    // The strap, behind: two short bands to the sides.
    g.setFillColor(NSColor(white: 0.85, alpha: 1).cgColor)
    g.fill(CGRect(x: 160, y: 565, width: 50, height: 60))
    g.fill(CGRect(x: 814, y: 565, width: 50, height: 60))

    NSGraphicsContext.restoreGraphicsState()
    return rep
}

let dir = URL(fileURLWithPath: CommandLine.arguments[1])
try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
for (points, scale) in [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2), (256, 1), (256, 2), (512, 1), (512, 2)] {
    let name = "icon_\(points)x\(points)\(scale == 2 ? "@2x" : "").png"
    try draw(points * scale).representation(using: .png, properties: [:])!.write(to: dir.appendingPathComponent(name))
}
