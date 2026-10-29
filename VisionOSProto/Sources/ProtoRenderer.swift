import ARKit
import CompositorServices
import Metal
#if canImport(MetalFX)
import MetalFX
#endif
import QuartzCore
import os
import simd

/// Uniform block shared with ProtoShaders.metal (SceneUniforms).
private struct SceneUniforms {
    var viewProj: (simd_float4x4, simd_float4x4)
    var eyePos: (SIMD4<Float>, SIMD4<Float>)
    var time: Float
    var iterations: UInt32
    var animate: UInt32
    var pad: UInt32 = 0
}

/// One finished (or in-progress) "game" frame for hold mode.
private final class GameSlot {
    let lowColor: MTLTexture       // render resolution, 2 layers
    let depth: MTLTexture          // render resolution, 2 layers
    let fullColor: MTLTexture?     // MetalFX output at logical size (nil when scale == 1)
    var anchor: DeviceAnchor?
    init(lowColor: MTLTexture, depth: MTLTexture, fullColor: MTLTexture?) {
        self.lowColor = lowColor; self.depth = depth; self.fullColor = fullColor
    }
}

/// Render loop for the prototype immersive space. Runs on its own thread, as
/// CompositorServices expects.
final class ProtoRenderer: @unchecked Sendable {
    private let layerRenderer: LayerRenderer
    private let settings: SettingsBox
    private let report: @Sendable (RenderStats) -> Void
    private let log: @Sendable (String) -> Void

    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let gameQueue: MTLCommandQueue
    private let scenePipeline: MTLRenderPipelineState
    private let offscreenPipeline: MTLRenderPipelineState
    private let farDepthPipeline: MTLRenderPipelineState
    private let presentPipeline: MTLRenderPipelineState
    private let sceneDepth: MTLDepthStencilState
    /// Vertex amplification (one pass for both eyes). The visionOS simulator lacks it.
    private let amp: Bool
    private let alwaysDepth: MTLDepthStencilState

    private let arSession = ARKitSession()
    private let worldTracking = WorldTrackingProvider()
    private let startTime = CACurrentMediaTime()

    // Hold-mode state.
    private var slots: [GameSlot] = []
    private var slotKey = ""
    #if canImport(MetalFX)
    private var scaler: MTLFXSpatialScaler?
    #else
    private var scaler: AnyObject?
    #endif
    private let gameLock = OSAllocatedUnfairLock(initialState: (ready: -1, inFlight: false))
    private var writeIndex = 0
    private var nextGameAt = 0.0

    // Statistics.
    private struct Acc {
        var displayed = 0, games = 0, late = 0
        var heavyMs = 0.0, heavyN = 0, presentMs = 0.0, presentN = 0
        var presentationDeltas = 0.0, deltaN = 0
    }
    private let acc = OSAllocatedUnfairLock(initialState: Acc())
    private var lastPresentation = 0.0
    /// Measured compositor rate (90 on Vision Pro by default, 60 in the simulator).
    private var displayHz = 90.0
    private var lastReport = CACurrentMediaTime()
    private var logical = SIMD2<Int>(0, 0)
    private var physical = SIMD2<Int>(0, 0)

    init(layerRenderer: LayerRenderer,
         settings: SettingsBox,
         report: @escaping @Sendable (RenderStats) -> Void,
         log: @escaping @Sendable (String) -> Void) throws {
        self.layerRenderer = layerRenderer
        self.settings = settings
        self.report = report
        self.log = log
        let dev = layerRenderer.device
        device = dev
        let ampOK = dev.supportsVertexAmplificationCount(2)
        amp = ampOK
        guard let q = dev.makeCommandQueue(), let g = dev.makeCommandQueue(),
              let library = dev.makeDefaultLibrary() else { throw ProtoError.metal("queue/library") }
        queue = q; gameQueue = g
        q.label = "present"; g.label = "game"
        let cfg = layerRenderer.configuration

        func pipeline(_ v: String, _ f: String?, color: MTLPixelFormat, depth: MTLPixelFormat, writeColor: Bool = true) throws -> MTLRenderPipelineState {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: v)
            d.fragmentFunction = f.flatMap { library.makeFunction(name: $0) }
            d.colorAttachments[0].pixelFormat = color
            if !writeColor { d.colorAttachments[0].writeMask = [] }
            d.depthAttachmentPixelFormat = depth
            d.maxVertexAmplificationCount = ampOK ? 2 : 1
            d.inputPrimitiveTopology = .triangle
            return try dev.makeRenderPipelineState(descriptor: d)
        }
        scenePipeline = try pipeline("sceneVertex", "sceneFragment", color: cfg.colorFormat, depth: cfg.depthFormat)
        offscreenPipeline = try pipeline("sceneVertex", "sceneFragment", color: .rgba16Float, depth: .depth32Float)
        farDepthPipeline = try pipeline("fullscreenVertex", "farDepthFragment", color: cfg.colorFormat, depth: cfg.depthFormat, writeColor: false)
        presentPipeline = try pipeline("fullscreenVertex", "presentFragment", color: cfg.colorFormat, depth: cfg.depthFormat)

        let sd = MTLDepthStencilDescriptor()
        sd.depthCompareFunction = .greater     // reverse Z: 1 = near, 0 = far
        sd.isDepthWriteEnabled = true
        let ad = MTLDepthStencilDescriptor()
        ad.depthCompareFunction = .always
        ad.isDepthWriteEnabled = true
        guard let s1 = dev.makeDepthStencilState(descriptor: sd),
              let s2 = dev.makeDepthStencilState(descriptor: ad) else { throw ProtoError.metal("depth state") }
        sceneDepth = s1; alwaysDepth = s2
    }

    func start() {
        Task {
            if WorldTrackingProvider.isSupported {
                do { try await arSession.run([worldTracking]) }
                catch { log("World tracking failed: \(error.localizedDescription)") }
            } else {
                log("World tracking not supported here; rendering head-locked.")
            }
            let thread = Thread { [self] in self.loop() }
            thread.name = "VXProto render"
            thread.qualityOfService = .userInteractive
            thread.start()
        }
    }

    private func loop() {
        log("Render loop started; layout \(layerRenderer.configuration.layout), foveation \(layerRenderer.configuration.isFoveationEnabled)")
        while true {
            switch layerRenderer.state {
            case .invalidated:
                layerRenderer.minimumFrameRepeatCount = 0
                log("Render loop ended (layer invalidated).")
                return
            case .paused:
                Thread.sleep(forTimeInterval: 0.05)
                continue
            default:
                autoreleasepool { renderFrame() }
            }
        }
    }

    private static func seconds(_ i: LayerRenderer.Clock.Instant) -> Double {
        let c = LayerRenderer.Clock.Instant.epoch.duration(to: i).components
        return Double(c.seconds) + Double(c.attoseconds) * 1e-18
    }

    private func renderFrame() {
        guard let frame = layerRenderer.queryNextFrame() else { Thread.sleep(forTimeInterval: 0.001); return }
        frame.startUpdate()
        frame.endUpdate()
        guard let timing = frame.predictTiming() else { return }
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)

        let s = settings.value
        // displayHz / (repeat + 1); at 90 Hz: 0 -> 90, 1 -> 45, 2 -> 30, 3 -> 22.5, 4 -> 18.
        let repeatCount = s.mode == .frameRepeat ? max(0, Int((displayHz / s.fps).rounded()) - 1) : 0
        if layerRenderer.minimumFrameRepeatCount != Int32(repeatCount) {
            layerRenderer.minimumFrameRepeatCount = Int32(repeatCount)
            log("minimumFrameRepeatCount = \(repeatCount)")
        }

        let presentation = Self.seconds(timing.presentationTime)
        let deadline = Self.seconds(timing.renderingDeadline)
        if lastPresentation > 0 {
            let d = presentation - lastPresentation
            if d > 0, d < 0.5 { acc.withLock { $0.presentationDeltas += d / Double(repeatCount + 1); $0.deltaN += 1 } }
        }
        lastPresentation = presentation

        frame.startSubmission()
        let drawables = frame.queryDrawables()
        // No drawables = the system cancelled this frame; ending its submission aborts the app.
        guard let first = drawables.first else { return }
        let anchor = worldTracking.queryDeviceAnchor(atTimestamp: Self.seconds(timing.trackableAnchorTime))
        noteSizes(first)

        switch s.mode {
        case .frameRepeat:
            for d in drawables {
                d.deviceAnchor = anchor
                drawSceneIntoDrawable(d, anchor: anchor, time: Float(presentation - startTime), settings: s, deadline: deadline)
            }
            acc.withLock { $0.games += 1 }
        case .hold:
            maybeStartGameFrame(template: first, now: CACurrentMediaTime(), presentation: presentation,
                                trackable: Self.seconds(timing.trackableAnchorTime), settings: s)
            let ready = gameLock.withLock { $0.ready }
            let slot = ready >= 0 && ready < slots.count ? slots[ready] : nil
            for d in drawables {
                d.deviceAnchor = slot?.anchor ?? anchor
                present(slot: slot, into: d, settings: s, deadline: deadline)
            }
        }
        frame.endSubmission()
        acc.withLock { $0.displayed += 1 }
        maybeReport(settings: s, repeatCount: repeatCount)
    }

    // MARK: Frame-repeat mode: heavy scene straight into the drawable.

    private func viewProjections(_ d: LayerRenderer.Drawable, anchor: DeviceAnchor?) -> [simd_float4x4] {
        let originFromDevice = anchor?.originFromAnchorTransform ?? matrix_identity_float4x4
        return d.views.indices.map { i in
            let view = (originFromDevice * d.views[i].transform).inverse
            return d.computeProjection(convention: .rightUpBack, viewIndex: i) * view
        }
    }

    private func uniforms(_ vp: [simd_float4x4], eyes: [SIMD4<Float>], time: Float, s: ProtoSettings) -> SceneUniforms {
        SceneUniforms(viewProj: (vp[0], vp.count > 1 ? vp[1] : vp[0]),
                      eyePos: (eyes.first ?? .zero, eyes.count > 1 ? eyes[1] : .zero),
                      time: time, iterations: UInt32(s.load), animate: s.animate ? 1 : 0)
    }

    private func encodeScene(_ enc: MTLRenderCommandEncoder, u: SceneUniforms) {
        var u = u
        enc.setVertexBytes(&u, length: MemoryLayout<SceneUniforms>.stride, index: 0)
        enc.setFragmentBytes(&u, length: MemoryLayout<SceneUniforms>.stride, index: 0)
        enc.setCullMode(.back)
        enc.setFrontFacing(.counterClockwise)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 36, instanceCount: 1 + 48 + 24 + 1)
    }

    /// Render passes for one drawable: one layered pass, or one per view for
    /// a dedicated layout. `body(encoder, viewIndices)` draws.
    private func forEachPass(_ d: LayerRenderer.Drawable, cb: MTLCommandBuffer, clear: Bool,
                             _ body: (MTLRenderCommandEncoder, [Int]) -> Void) {
        let layered = layerRenderer.configuration.layout == .layered
        let groups: [[Int]] = layered && amp ? [Array(d.views.indices)] : d.views.indices.map { [$0] }
        for group in groups {
            let t = layered ? 0 : group[0]
            let slice = layered && !amp ? group[0] : 0
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = d.colorTextures[t]
            pass.colorAttachments[0].loadAction = clear ? .clear : .load
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.02, green: 0.02, blue: 0.04, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].slice = slice
            pass.depthAttachment.texture = d.depthTextures[t]
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 0
            pass.depthAttachment.storeAction = .store
            pass.depthAttachment.slice = slice
            // A layered rate map covers both slices, so it needs the amplified pass.
            if t < d.rasterizationRateMaps.count && (amp || !layered) { pass.rasterizationRateMap = d.rasterizationRateMaps[t] }
            if layered && amp { pass.renderTargetArrayLength = d.views.count }
            guard let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { continue }
            enc.setViewports(group.map { d.views[$0].textureMap.viewport })
            var mappings = group.indices.map {
                MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32($0),
                                                  renderTargetArrayIndexOffset: UInt32(layered ? $0 : 0))
            }
            if amp { enc.setVertexAmplificationCount(group.count, viewMappings: &mappings) }
            var eyeBase = UInt32(group[0])
            enc.setVertexBytes(&eyeBase, length: 4, index: 1)
            body(enc, group)
            enc.endEncoding()
        }
    }

    private func drawSceneIntoDrawable(_ d: LayerRenderer.Drawable, anchor: DeviceAnchor?, time: Float,
                                       settings s: ProtoSettings, deadline: Double) {
        guard let cb = queue.makeCommandBuffer() else { return }
        let vp = viewProjections(d, anchor: anchor)
        forEachPass(d, cb: cb, clear: true) { enc, group in
            enc.setRenderPipelineState(scenePipeline)
            enc.setDepthStencilState(sceneDepth)
            encodeScene(enc, u: uniforms(group.map { vp[$0] }, eyes: [], time: time, s: s))
            if !s.providesDepth {
                enc.setRenderPipelineState(farDepthPipeline)
                enc.setDepthStencilState(alwaysDepth)
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            }
        }
        d.encodePresent(commandBuffer: cb)
        cb.addCompletedHandler { [acc] cb in
            let ms = (cb.gpuEndTime - cb.gpuStartTime) * 1000
            acc.withLock { a in
                a.heavyMs += ms; a.heavyN += 1
                if cb.gpuEndTime > deadline { a.late += 1 }
            }
        }
        cb.commit()
    }

    // MARK: Hold mode: game renders offscreen at its own rate; 90 Hz present pass.

    private func noteSizes(_ d: LayerRenderer.Drawable) {
        if let map = d.rasterizationRateMaps.first {
            logical = SIMD2(map.screenSize.width, map.screenSize.height)
        } else {
            let v = d.views[0].textureMap.viewport
            logical = SIMD2(Int(v.width), Int(v.height))
        }
        physical = SIMD2(d.colorTextures[0].width, d.colorTextures[0].height)
    }

    private func ensureSlots(scale: Double) {
        let w = max(64, Int(Double(logical.x) * scale)), h = max(64, Int(Double(logical.y) * scale))
        #if canImport(MetalFX)
        let useScaler = scale < 0.99 && MTLFXSpatialScalerDescriptor.supportsDevice(device)
        #else
        let useScaler = false
        #endif
        let key = "\(w)x\(h)->\(logical.x)x\(logical.y) fx=\(useScaler)"
        if key == slotKey { return }
        // Wait for any in-flight game frame before freeing its textures.
        while gameLock.withLock({ $0.inFlight }) { Thread.sleep(forTimeInterval: 0.001) }
        var outputUsage: MTLTextureUsage = [.shaderRead]
        #if canImport(MetalFX)
        if useScaler {
            let sd = MTLFXSpatialScalerDescriptor()
            sd.inputWidth = w; sd.inputHeight = h
            sd.outputWidth = logical.x; sd.outputHeight = logical.y
            sd.colorTextureFormat = .rgba16Float
            sd.outputTextureFormat = .rgba16Float
            sd.colorProcessingMode = .linear
            scaler = sd.makeSpatialScaler(device: device)
            if let sc = scaler { outputUsage = sc.outputTextureUsage.union(.shaderRead) }
        } else {
            scaler = nil
        }
        #endif
        func tex(_ f: MTLPixelFormat, _ w: Int, _ h: Int, _ usage: MTLTextureUsage) -> MTLTexture? {
            let t = MTLTextureDescriptor()
            t.textureType = .type2DArray
            t.arrayLength = 2
            t.pixelFormat = f; t.width = w; t.height = h
            t.usage = usage
            t.storageMode = .private
            return device.makeTexture(descriptor: t)
        }
        slots = (0..<3).compactMap { _ in
            guard let c = tex(.rgba16Float, w, h, [.renderTarget, .shaderRead]),
                  let z = tex(.depth32Float, w, h, [.renderTarget, .shaderRead]) else { return nil }
            var full: MTLTexture?
            if scaler != nil {
                full = tex(.rgba16Float, logical.x, logical.y, outputUsage)
            }
            return GameSlot(lowColor: c, depth: z, fullColor: full)
        }
        gameLock.withLock { $0.ready = -1 }
        slotKey = key
        log("Hold slots: \(key)")
    }

    private func maybeStartGameFrame(template d: LayerRenderer.Drawable, now: Double, presentation: Double,
                                     trackable: Double, settings s: ProtoSettings) {
        let period = 1.0 / s.fps
        if now < nextGameAt || gameLock.withLock({ $0.inFlight }) { return }
        nextGameAt = (now - nextGameAt > period) ? now + period : nextGameAt + period
        ensureSlots(scale: s.renderScale)
        guard slots.count == 3 else { return }
        let ready = gameLock.withLock { $0.ready }
        var w = (writeIndex + 1) % 3
        if w == ready { w = (w + 1) % 3 }
        writeIndex = w
        let slotIndex = w
        let slot = slots[w]
        // Pose for the game frame: the next display pose (the reprojection hides the rest).
        let anchor = worldTracking.queryDeviceAnchor(atTimestamp: trackable)
        slot.anchor = anchor
        let vp = viewProjections(d, anchor: anchor)
        guard let cb = gameQueue.makeCommandBuffer() else { return }
        cb.label = "game frame"
        for eyes in (amp ? [[0, 1]] : [[0], [1]]) {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = slot.lowColor
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.02, green: 0.02, blue: 0.04, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = slot.depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 0
            pass.depthAttachment.storeAction = .store
            if amp {
                pass.renderTargetArrayLength = 2
            } else {
                pass.colorAttachments[0].slice = eyes[0]
                pass.depthAttachment.slice = eyes[0]
            }
            guard let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { continue }
            let vpw = MTLViewport(originX: 0, originY: 0, width: Double(slot.lowColor.width),
                                  height: Double(slot.lowColor.height), znear: 0, zfar: 1)
            enc.setViewports(eyes.map { _ in vpw })
            if amp {
                var mappings = (0..<2).map {
                    MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32($0), renderTargetArrayIndexOffset: UInt32($0))
                }
                enc.setVertexAmplificationCount(2, viewMappings: &mappings)
            }
            enc.setRenderPipelineState(offscreenPipeline)
            enc.setDepthStencilState(sceneDepth)
            encodeScene(enc, u: uniforms(eyes.map { vp[min($0, vp.count - 1)] }, eyes: [], time: Float(presentation - startTime), s: s))
            enc.endEncoding()
        }
        #if canImport(MetalFX)
        if let scaler, let full = slot.fullColor {
            for eye in 0..<2 {
                guard let src = slot.lowColor.makeTextureView(pixelFormat: .rgba16Float, textureType: .type2D,
                                                              levels: 0..<1, slices: eye..<(eye + 1)),
                      let dst = full.makeTextureView(pixelFormat: .rgba16Float, textureType: .type2D,
                                                     levels: 0..<1, slices: eye..<(eye + 1)) else { continue }
                scaler.colorTexture = src
                scaler.outputTexture = dst
                scaler.inputContentWidth = src.width
                scaler.inputContentHeight = src.height
                scaler.encode(commandBuffer: cb)
            }
        }
        #endif
        gameLock.withLock { $0.inFlight = true }
        cb.addCompletedHandler { [gameLock, acc] cb in
            let ms = (cb.gpuEndTime - cb.gpuStartTime) * 1000
            gameLock.withLock { $0.ready = slotIndex; $0.inFlight = false }
            acc.withLock { $0.heavyMs += ms; $0.heavyN += 1; $0.games += 1 }
        }
        cb.commit()
    }

    private func present(slot: GameSlot?, into d: LayerRenderer.Drawable, settings s: ProtoSettings, deadline: Double) {
        guard let cb = queue.makeCommandBuffer() else { return }
        forEachPass(d, cb: cb, clear: true) { enc, group in
            guard let slot else { return }
            enc.setRenderPipelineState(presentPipeline)
            enc.setDepthStencilState(alwaysDepth)
            enc.setFragmentTexture(slot.fullColor ?? slot.lowColor, index: 0)
            enc.setFragmentTexture(slot.depth, index: 1)
            var writeDepth: UInt32 = s.providesDepth ? 1 : 0
            enc.setFragmentBytes(&writeDepth, length: 4, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }
        d.encodePresent(commandBuffer: cb)
        cb.addCompletedHandler { [acc] cb in
            let ms = (cb.gpuEndTime - cb.gpuStartTime) * 1000
            acc.withLock { a in
                a.presentMs += ms; a.presentN += 1
                if cb.gpuEndTime > deadline { a.late += 1 }
            }
        }
        cb.commit()
    }

    // MARK: Reporting

    private func maybeReport(settings s: ProtoSettings, repeatCount: Int) {
        let now = CACurrentMediaTime()
        let dt = now - lastReport
        guard dt >= 1 else { return }
        lastReport = now
        let a = acc.withLock { a -> Acc in let c = a; a = Acc(); return c }
        var r = RenderStats()
        r.mode = s.mode.rawValue
        r.displayHz = a.deltaN > 0 ? Double(a.deltaN) / a.presentationDeltas : 0
        if r.displayHz > 20 { displayHz = r.displayHz }
        r.targetFps = s.mode == .frameRepeat ? displayHz / Double(repeatCount + 1) : s.fps
        r.appFrames = Double(a.displayed) / dt
        r.gameFps = Double(a.games) / dt
        r.heavyGpuMs = a.heavyN > 0 ? a.heavyMs / Double(a.heavyN) : 0
        r.presentGpuMs = a.presentN > 0 ? a.presentMs / Double(a.presentN) : 0
        r.lateFrames = a.late
        r.repeatCount = repeatCount
        r.foveated = layerRenderer.configuration.isFoveationEnabled
        r.depth = s.providesDepth
        r.logicalSize = "\(logical.x)x\(logical.y)"
        r.physicalSize = "\(physical.x)x\(physical.y)"
        r.availableMemoryMB = CapabilityProbe.availableMemoryMB()
        r.footprintMB = CapabilityProbe.footprintMB()
        report(r)
    }
}

enum ProtoError: Error { case metal(String) }
