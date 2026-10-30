// The compositor loop of VX Runtime's immersive space (VISIONOS_PORT.md, "VR presentation"): every compositor frame
// (90 Hz on the headset), the head's pose at the frame's presentation time (ARKit world tracking), then each eye: the
// newest game frame warped to that eye by the runtime (src/xros_vr.m: the warp core with the game's depth: head
// rotation reprojection, the eyes' parallax, the NPU race's predictions from the game's motion vectors), depth into
// the drawable for the system's own late reprojection, the drawable's device anchor the pose the eyes were made for.
// The game renders at its own rate; its sync-interval presents are paced by these frames (vx_xros_vr_begin_frame).
import ARKit
import CompositorServices
import Metal
import QuartzCore
import simd

final class VXImmersiveRenderer: @unchecked Sendable {
    private let layerRenderer: LayerRenderer
    private let queue: MTLCommandQueue
    private let arSession = ARKitSession()
    private let worldTracking = WorldTrackingProvider()
    private var tracking = false
    private var lastReport = CACurrentMediaTime()
    private var cleared = 0
    private static let near: Float = 0.1, far: Float = 10000

    static func start(_ layerRenderer: LayerRenderer) {
        guard let q = layerRenderer.device.makeCommandQueue() else {
            vxrt_jlog("vr_error", "\"what\":\"no command queue\"")
            return
        }
        let r = VXImmersiveRenderer(layerRenderer, q)
        Task {
            if WorldTrackingProvider.isSupported {
                do { try await r.arSession.run([r.worldTracking]); r.tracking = true }
                catch { vxrt_jlog("vr_error", "\"what\":\"world tracking: \(error.localizedDescription)\"") }
            }
            let thread = Thread { r.loop() }
            thread.name = "VX compositor"
            thread.qualityOfService = .userInteractive
            thread.start()
        }
    }

    private init(_ layerRenderer: LayerRenderer, _ queue: MTLCommandQueue) {
        self.layerRenderer = layerRenderer
        self.queue = queue
        queue.label = "VX compositor"
    }

    private static func seconds(_ i: LayerRenderer.Clock.Instant) -> Double {
        let c = LayerRenderer.Clock.Instant.epoch.duration(to: i).components
        return Double(c.seconds) + Double(c.attoseconds) * 1e-18
    }

    private func loop() {
        let cfg = layerRenderer.configuration
        vxrt_jlog("vr_start", "\"layout\":\"\(cfg.layout)\",\"color\":\(cfg.colorFormat.rawValue),\"depth\":\(cfg.depthFormat.rawValue),\"tracking\":\(tracking)")
        while true {
            switch layerRenderer.state {
            case .invalidated:
                vxrt_jlog("vr_end", "\"why\":\"layer invalidated\"")
                return
            case .paused:
                layerRenderer.waitUntilRunning()
                continue
            default:
                autoreleasepool { frame() }
            }
        }
    }

    private func frame() {
        guard let frame = layerRenderer.queryNextFrame() else { Thread.sleep(forTimeInterval: 0.001); return }
        frame.startUpdate()
        frame.endUpdate()
        guard let timing = frame.predictTiming() else { return }
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)
        frame.startSubmission()
        let drawables = frame.queryDrawables()
        // (no drawables: the system cancelled this frame; ending its submission would abort the app)
        guard !drawables.isEmpty else { return }
        let presentation = Self.seconds(timing.presentationTime)
        let anchor = tracking ? worldTracking.queryDeviceAnchor(atTimestamp: presentation) : nil
        var head = anchor?.originFromAnchorTransform ?? matrix_identity_float4x4
        let have = withUnsafePointer(to: &head) {
            $0.withMemoryRebound(to: Float.self, capacity: 16) { vx_xros_vr_begin_frame($0, presentation) }
        }
        guard let cb = queue.makeCommandBuffer() else { vx_xros_vr_end_frame(nil); frame.endSubmission(); return }
        cb.label = "VX eyes"
        let cbp = Unmanaged.passUnretained(cb as AnyObject).toOpaque()
        for d in drawables {
            d.deviceAnchor = anchor
            d.depthRange = SIMD2<Float>(Self.far, Self.near)
            for i in d.views.indices {
                let color = d.colorTextures[min(i, d.colorTextures.count - 1)]
                let depth = d.depthTextures[min(i, d.depthTextures.count - 1)]
                var ok: Int32 = 0
                if have != 0 {
                    var viewFromOrigin = (head * d.views[i].transform).inverse
                    // the eye's frustum from its projection (right-up-back: x_ndc = (P00 x + P20 z) / -z)
                    let p = d.computeProjection(convention: .rightUpBack, viewIndex: i)
                    var tan = SIMD4<Float>((1 - p.columns.2.x) / p.columns.0.x, (1 + p.columns.2.x) / p.columns.0.x,
                                           (1 + p.columns.2.y) / p.columns.1.y, (1 - p.columns.2.y) / p.columns.1.y)
                    ok = withUnsafePointer(to: &viewFromOrigin) { vp in
                        vp.withMemoryRebound(to: Float.self, capacity: 16) { v in
                            withUnsafePointer(to: &tan) { tp in
                                tp.withMemoryRebound(to: Float.self, capacity: 4) { t in
                                    vx_xros_vr_draw_eye(cbp, Int32(i), v, t, Self.near,
                                                        Unmanaged.passUnretained(color as AnyObject).toOpaque(),
                                                        Unmanaged.passUnretained(depth as AnyObject).toOpaque())
                                }
                            }
                        }
                    }
                }
                if ok == 0 { clear(cb, color, depth) }
            }
            d.encodePresent(commandBuffer: cb)
        }
        vx_xros_vr_end_frame(cbp)
        cb.commit()
        frame.endSubmission()
        report()
    }

    // (no game frame yet: a dark grey room, depth far)
    private func clear(_ cb: MTLCommandBuffer, _ color: MTLTexture, _ depth: MTLTexture) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = color
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.03, green: 0.03, blue: 0.05, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 0
        pass.depthAttachment.storeAction = .store
        cb.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        cleared += 1
    }

    private func report() {
        let now = CACurrentMediaTime()
        guard now - lastReport >= 1 else { return }
        lastReport = now
        var buf = [CChar](repeating: 0, count: 768)
        vx_xros_vr_stats(&buf, UInt32(buf.count))
        let fields = String(cString: buf) + ",\"cleared_eyes\":\(cleared)"
        cleared = 0
        vxrt_jlog("vr", fields)
    }
}
