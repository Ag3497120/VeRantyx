import Foundation
import simd

/// CPU-built triangle lists in world space (immersive-space origin on the floor
/// at the user's feet, metres, -Z forward). Everything is world-locked.
struct Geometry {
    var vertices: [ProbeVertex] = []

    mutating func addQuad(_ a: SIMD3<Float>, _ b: SIMD3<Float>, _ c: SIMD3<Float>, _ d: SIMD3<Float>, _ color: SIMD4<Float>) {
        for p in [a, b, c, a, c, d] {
            vertices.append(ProbeVertex(position: SIMD4(p, 1), color: color))
        }
    }

    /// An axis-aligned rectangle in the plane z = `z`, facing +Z.
    mutating func addRect(x0: Float, y0: Float, x1: Float, y1: Float, z: Float, _ color: SIMD4<Float>) {
        addQuad(SIMD3(x0, y0, z), SIMD3(x1, y0, z), SIMD3(x1, y1, z), SIMD3(x0, y1, z), color)
    }

    mutating func addBox(center c: SIMD3<Float>, size: Float, color: SIMD4<Float>) {
        let h = size / 2
        func p(_ x: Float, _ y: Float, _ z: Float) -> SIMD3<Float> { c + SIMD3(x, y, z) * h }
        func shade(_ k: Float) -> SIMD4<Float> { SIMD4(color.x * k, color.y * k, color.z * k, color.w) }
        addQuad(p(-1, -1, 1), p(1, -1, 1), p(1, 1, 1), p(-1, 1, 1), shade(1.0))     // +Z
        addQuad(p(1, -1, -1), p(-1, -1, -1), p(-1, 1, -1), p(1, 1, -1), shade(0.5))  // -Z
        addQuad(p(1, -1, 1), p(1, -1, -1), p(1, 1, -1), p(1, 1, 1), shade(0.8))      // +X
        addQuad(p(-1, -1, -1), p(-1, -1, 1), p(-1, 1, 1), p(-1, 1, -1), shade(0.7))  // -X
        addQuad(p(-1, 1, 1), p(1, 1, 1), p(1, 1, -1), p(-1, 1, -1), shade(0.9))      // +Y
        addQuad(p(-1, -1, -1), p(1, -1, -1), p(1, -1, 1), p(-1, -1, 1), shade(0.4))  // -Y
    }

    /// Floor grid at y = 0: a line every 0.5 m over 10 x 10 m, brighter every metre,
    /// red line along +X and blue along -Z (forward) through the origin.
    mutating func addFloorGrid() {
        let extent: Float = 5, w: Float = 0.006, y: Float = 0.001
        for i in -10...10 {
            let v = Float(i) * 0.5
            let major = i % 2 == 0
            var color: SIMD4<Float> = major ? SIMD4(0.55, 0.55, 0.6, 1) : SIMD4(0.25, 0.25, 0.3, 1)
            if i == 0 { color = SIMD4(0.2, 0.35, 1, 1) }
            // line along Z at x = v
            addQuad(SIMD3(v - w, y, extent), SIMD3(v + w, y, extent), SIMD3(v + w, y, -extent), SIMD3(v - w, y, -extent), color)
            if i == 0 { color = SIMD4(1, 0.25, 0.2, 1) }
            // line along X at z = v
            addQuad(SIMD3(-extent, y, v + w), SIMD3(extent, y, v + w), SIMD3(extent, y, v - w), SIMD3(-extent, y, v - w), color)
        }
    }

    /// Cubes at 1, 2 and 3 m in front, one to the side and one behind.
    mutating func addCubes() {
        addBox(center: SIMD3(0.0, 1.1, -1.0), size: 0.15, color: SIMD4(1, 0.2, 0.2, 1))
        addBox(center: SIMD3(0.6, 1.3, -2.0), size: 0.25, color: SIMD4(0.2, 1, 0.3, 1))
        addBox(center: SIMD3(-0.6, 0.9, -2.0), size: 0.25, color: SIMD4(0.3, 0.5, 1, 1))
        addBox(center: SIMD3(0.0, 1.5, -3.0), size: 0.35, color: SIMD4(1, 0.6, 0.1, 1))
        addBox(center: SIMD3(1.5, 1.0, -1.5), size: 0.2, color: SIMD4(1, 0.3, 1, 1))
        addBox(center: SIMD3(0.0, 1.2, 2.5), size: 0.3, color: SIMD4(0.7, 0.7, 0.7, 1))
    }

    /// World-locked panel 2 m ahead: frame index on top, predicted presentation
    /// time (seconds, mod 10000) below, pose delay (ms) when it is not zero.
    mutating func addPanel(frameIndex: UInt64, presentationTime: Double, poseDelayMs: Int) {
        let z: Float = -2.0
        addRect(x0: -0.6, y0: 1.55, x1: 0.6, y1: 2.05, z: z, SIMD4(0.08, 0.08, 0.1, 1))
        let white = SIMD4<Float>(1, 1, 1, 1)
        addText("\(frameIndex)", x: -0.55, y: 1.86, height: 0.14, z: z + 0.005, white)
        addText(String(format: "%.3f", presentationTime.truncatingRemainder(dividingBy: 10_000)),
                x: -0.55, y: 1.62, height: 0.14, z: z + 0.005, SIMD4(0.5, 1, 0.6, 1))
        if poseDelayMs > 0 {
            addText("\(poseDelayMs)", x: 0.36, y: 1.62, height: 0.08, z: z + 0.005, SIMD4(1, 0.6, 0.1, 1))
        }
    }

    /// 1 m square to the left; white only on the flash frame.
    mutating func addFlashSquare(lit: Bool) {
        let c: SIMD4<Float> = lit ? SIMD4(1, 1, 1, 1) : SIMD4(0.12, 0.12, 0.12, 1)
        addRect(x0: -2.1, y0: 0.9, x1: -1.1, y1: 1.9, z: -2.2, c)
    }

    // MARK: seven-segment text (digits, '.', '-')

    private static let segments: [(Float, Float, Float, Float)] = [
        (0.1, 0.9, 0.5, 1.0),   // a top
        (0.5, 0.5, 0.6, 0.95),  // b upper right
        (0.5, 0.05, 0.6, 0.5),  // c lower right
        (0.1, 0.0, 0.5, 0.1),   // d bottom
        (0.0, 0.05, 0.1, 0.5),  // e lower left
        (0.0, 0.5, 0.1, 0.95),  // f upper left
        (0.1, 0.45, 0.5, 0.55), // g middle
    ]
    private static let digitMasks: [UInt8] = [
        0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
        0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111,
    ]

    mutating func addText(_ text: String, x: Float, y: Float, height h: Float, z: Float, _ color: SIMD4<Float>) {
        var cursor = x
        for ch in text {
            if ch == "." {
                addRect(x0: cursor, y0: y, x1: cursor + 0.1 * h, y1: y + 0.1 * h, z: z, color)
                cursor += 0.3 * h
                continue
            }
            let mask: UInt8
            if let d = ch.wholeNumberValue { mask = Self.digitMasks[d] } else if ch == "-" { mask = 0b1000000 } else { mask = 0 }
            for (i, s) in Self.segments.enumerated() where mask & (1 << i) != 0 {
                addRect(x0: cursor + s.0 * h, y0: y + s.1 * h, x1: cursor + s.2 * h, y1: y + s.3 * h, z: z, color)
            }
            cursor += 0.8 * h
        }
    }
}
