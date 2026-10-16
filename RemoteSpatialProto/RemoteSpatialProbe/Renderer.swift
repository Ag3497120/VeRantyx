import ARKit
import CompositorServices
import Metal
import QuartzCore
import simd

/// The probe's render loop: Compositor Services on macOS, both eyes, world-locked
/// content placed with the Vision Pro's device anchor from ARKit on the Mac.
/// Loop shape (queryNextFrame / startUpdate / predictTiming / wait for
/// optimalInputTime / queryDrawables / encodePresent) follows Apple's template.
final class Renderer: @unchecked Sendable {
    static let maxFramesInFlight = 3
    static let dynamicCapacity = 16_384 // vertices per frame

    let layerRenderer: LayerRenderer
    let device: MTLDevice
    let commandQueue: MTLCommandQueue
    let pipeline: MTLRenderPipelineState
    let depthState: MTLDepthStencilState
    let staticVertices: MTLBuffer
    let staticCount: Int
    let dynamicBuffers: [MTLBuffer]
    let inFlight = DispatchSemaphore(value: Renderer.maxFramesInFlight)

    let arSession: ARKitSession?
    let worldTracking = WorldTrackingProvider()
    let control: ProbeControl
    let appModel: AppModel
    let stats = StatsAccumulator()

    private var ring = 0
    private var lastPresentationFrameIndex: UInt64?
    private var lastPresentation: Double?
    private var lastStatsPush: CFTimeInterval = 0
    private var lastTextureDescription = ""
    private var reportedRunning = false

    // MARK: start

    @MainActor
    static func start(_ layerRenderer: LayerRenderer, arSession: ARKitSession?, appModel: AppModel) {
        let renderer = Renderer(layerRenderer, arSession: arSession, appModel: appModel)
        renderer.startARSession()
        let thread = Thread { renderer.renderLoop() }
        thread.name = "RemoteSpatialProbe render"
        thread.qualityOfService = .userInteractive
        thread.start()
    }

    @MainActor
    private init(_ layerRenderer: LayerRenderer, arSession: ARKitSession?, appModel: AppModel) {
        self.layerRenderer = layerRenderer
        self.arSession = arSession
        self.appModel = appModel
        self.control = appModel.control
        let device = layerRenderer.device
        self.device = device
        commandQueue = device.makeCommandQueue()!

        let library = device.makeDefaultLibrary()!
        let desc = MTLRenderPipelineDescriptor()
        desc.label = "Probe"
        desc.vertexFunction = library.makeFunction(name: "probeVertex")
        desc.fragmentFunction = library.makeFunction(name: "probeFragment")
        desc.colorAttachments[0].pixelFormat = layerRenderer.configuration.colorFormat
        desc.depthAttachmentPixelFormat = layerRenderer.configuration.depthFormat
        desc.maxVertexAmplificationCount = layerRenderer.properties.viewCount
        do {
            pipeline = try device.makeRenderPipelineState(descriptor: desc)
        } catch {
            fatalError("pipeline: \(error)")
        }

        let depthDesc = MTLDepthStencilDescriptor()
        depthDesc.depthCompareFunction = .greater // reverse-Z, as computeProjection returns
        depthDesc.isDepthWriteEnabled = true
        depthState = device.makeDepthStencilState(descriptor: depthDesc)!

        var world = Geometry()
        world.addFloorGrid()
        world.addCubes()
        staticCount = world.vertices.count
        staticVertices = device.makeBuffer(bytes: world.vertices,
                                           length: MemoryLayout<ProbeVertex>.stride * world.vertices.count,
                                           options: .storageModeShared)!
        dynamicBuffers = (0..<Self.maxFramesInFlight).map { _ in
            device.makeBuffer(length: MemoryLayout<ProbeVertex>.stride * Self.dynamicCapacity,
                              options: .storageModeShared)!
        }

        let props = layerRenderer.properties
        ProbeLog.shared.note("layer: viewCount=\(props.viewCount) layout=\(ProbeImmersiveContent.name(layerRenderer.configuration.layout)) renderQuality=\(layerRenderer.renderQuality.rawValue) minimumFrameRepeatCount=\(layerRenderer.minimumFrameRepeatCount) device=\(device.name)")
        ProbeLog.shared.note("frames CSV: \(ProbeLog.shared.framesURL.path)")
    }

    private func startARSession() {
        ProbeLog.shared.note("WorldTrackingProvider.isSupported=\(WorldTrackingProvider.isSupported)")
        guard let arSession else { return }
        let worldTracking = worldTracking
        let appModel = appModel
        Task {
            do {
                try await arSession.run([worldTracking])
                ProbeLog.shared.note("ARKitSession.run([WorldTrackingProvider]) returned; state=\(worldTracking.state)")
            } catch {
                ProbeLog.shared.note("ARKitSession.run failed: \(error)")
                await MainActor.run { appModel.stats.arState = "run failed: \(error.localizedDescription)" }
            }
        }
        Task {
            for await event in arSession.events {
                ProbeLog.shared.note("ARKit event: \(event)")
                let text = "\(worldTracking.state)"
                await MainActor.run { appModel.stats.arState = text }
            }
        }
    }

    // MARK: loop

    func renderLoop() {
        while true {
            switch layerRenderer.state {
            case .invalidated:
                ProbeLog.shared.note("layer invalidated after \(stats.snapshot().framesRendered) frames")
                arSession?.stop()
                ProbeLog.shared.flush()
                setState(.closed, layer: "invalidated")
                return
            case .paused:
                ProbeLog.shared.note("layer paused")
                setState(.inTransition, layer: "paused")
                reportedRunning = false
                layerRenderer.waitUntilRunning()
                ProbeLog.shared.note("layer running")
            default:
                if !reportedRunning {
                    reportedRunning = true
                    setState(.open, layer: "running")
                }
                autoreleasepool { renderFrame() }
            }
        }
    }

    private func setState(_ state: AppModel.ImmersiveSpaceState, layer: String) {
        let appModel = appModel
        Task { @MainActor in
            appModel.immersiveSpaceState = state
            appModel.stats.layerState = layer
        }
    }

    private func renderFrame() {
        guard let frame = layerRenderer.queryNextFrame() else { return }
        let tQuery = CACurrentMediaTime()

        frame.startUpdate()
        let pinches = control.pinches
        frame.endUpdate()

        guard let timing = frame.predictTiming() else { return }
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)
        let tInput = CACurrentMediaTime()
        let flashPress = control.takeFlash(now: tInput)
        let poseDelay = control.poseDelayMs / 1000

        inFlight.wait()
        guard let commandBuffer = commandQueue.makeCommandBuffer() else {
            inFlight.signal()
            return
        }

        let drawables = frame.queryDrawables()
        guard !drawables.isEmpty else {
            inFlight.signal()
            stats.noDrawable()
            return
        }

        frame.startSubmission()

        let predictedPresentation = timing.presentationTime.seconds
        let frameIndex = frame.frameIndex

        // Per-frame world-locked geometry: the frame/time panel, the flash square, pinch markers.
        var dynamic = Geometry()
        dynamic.addPanel(frameIndex: frameIndex, presentationTime: predictedPresentation, poseDelayMs: Int(poseDelay * 1000))
        dynamic.addFlashSquare(lit: flashPress != nil)
        for p in pinches { dynamic.addBox(center: p, size: 0.02, color: SIMD4(1, 0.85, 0.1, 1)) }
        let dynamicBuffer = dynamicBuffers[ring]
        ring = (ring + 1) % Self.maxFramesInFlight
        let dynamicCount = min(dynamic.vertices.count, Self.dynamicCapacity)
        dynamic.vertices.withUnsafeBytes { bytes in
            dynamicBuffer.contents().copyMemory(from: bytes.baseAddress!,
                                                byteCount: MemoryLayout<ProbeVertex>.stride * dynamicCount)
        }

        // Pose: the device anchor predicted for the drawable's presentation time
        // (optionally aged by poseDelay to test the compositor's reprojection).
        var row = FrameRow(frameIndex: frameIndex, tQuery: tQuery, tInput: tInput, timing: timing,
                           drawables: drawables.count, poseDelayMs: poseDelay * 1000,
                           flash: flashPress != nil, repeatCount: Int(layerRenderer.minimumFrameRepeatCount))
        for (i, drawable) in drawables.enumerated() {
            let presentation = drawable.frameTiming.presentationTime.seconds
            let queryTime = presentation - poseDelay
            let anchor = arSession == nil ? nil : worldTracking.queryDeviceAnchor(atTimestamp: queryTime)
            drawable.deviceAnchor = anchor
            if i == 0 {
                row.drawablePresentation = presentation
                row.presentationFrameIndex = drawable.presentationFrameIndex
                row.poseQueryTime = queryTime
                row.anchor = anchor
            }
            logTexturesIfChanged(drawable)
            encode(drawable: drawable, anchor: anchor, commandBuffer: commandBuffer,
                   dynamicBuffer: dynamicBuffer, dynamicCount: dynamicCount)
            drawable.encodePresent(commandBuffer: commandBuffer)
        }

        if let press = flashPress {
            let text = String(format: "flash frame=%llu t_press=%.6f t_input=%.6f predicted_presentation=%.6f press_to_presentation_ms=%.2f",
                              frameIndex, press, tInput, row.drawablePresentation, (row.drawablePresentation - press) * 1000)
            ProbeLog.shared.note(text)
            let summary = String(format: "frame %llu: press → predicted presentation %.1f ms", frameIndex,
                                 (row.drawablePresentation - press) * 1000)
            let appModel = appModel
            Task { @MainActor in appModel.stats.lastFlash = summary }
        }

        // Skipped compositor frames: presentationFrameIndex should advance by
        // (repeat count + 1) per app frame.
        if let last = lastPresentationFrameIndex, row.presentationFrameIndex > last + UInt64(1 + max(0, row.repeatCount)) {
            stats.skipped(Int(row.presentationFrameIndex - last) - 1 - max(0, row.repeatCount))
        }
        lastPresentationFrameIndex = row.presentationFrameIndex
        let interval = lastPresentation.map { (row.drawablePresentation - $0) * 1000 }
        lastPresentation = row.drawablePresentation

        let deadline = timing.renderingDeadline.seconds
        let stats = stats
        let inFlight = inFlight
        row.tCommit = CACurrentMediaTime() // just before commit()
        let baseRow = row
        commandBuffer.addCompletedHandler { cb in
            var row = baseRow
            row.tGPUDone = CACurrentMediaTime()
            row.gpuMs = (cb.gpuEndTime - cb.gpuStartTime) * 1000
            row.late = row.tGPUDone > deadline
            ProbeLog.shared.frameRow(row.csvFields)
            stats.add(row, intervalMs: interval)
            inFlight.signal()
        }
        commandBuffer.commit()
        frame.endSubmission()

        if tInput - lastStatsPush > 0.25 {
            lastStatsPush = tInput
            let snapshot = stats.snapshot()
            let appModel = appModel
            Task { @MainActor in
                var s = snapshot
                s.spatialEvents = appModel.stats.spatialEvents
                s.lastSpatialEvent = appModel.stats.lastSpatialEvent
                s.lastFlash = appModel.stats.lastFlash
                s.layerState = appModel.stats.layerState
                s.arState = appModel.stats.arState
                appModel.stats = s
            }
        }
    }

    // MARK: encode

    private func encode(drawable: LayerRenderer.Drawable, anchor: DeviceAnchor?, commandBuffer: MTLCommandBuffer,
                        dynamicBuffer: MTLBuffer, dynamicCount: Int) {
        let origin = anchor?.originFromAnchorTransform ?? matrix_identity_float4x4
        func viewProjection(_ i: Int) -> simd_float4x4 {
            let view = (origin * drawable.views[i].transform).inverse
            return drawable.computeProjection(convention: .rightUpBack, viewIndex: i) * view
        }

        let layout = layerRenderer.configuration.layout
        // One pass per texture: layered/shared put both eyes in one texture
        // (vertex amplification); dedicated gives each eye its own texture.
        let passes: [[Int]] = layout == .dedicated
            ? drawable.views.indices.map { [$0] }
            : [Array(drawable.views.indices)]

        for viewIndices in passes {
            let textureIndex = drawable.views[viewIndices[0]].textureMap.textureIndex
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = drawable.colorTextures[textureIndex]
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.02, green: 0.03, blue: 0.05, alpha: 1)
            pass.depthAttachment.texture = drawable.depthTextures[textureIndex]
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.storeAction = .store
            pass.depthAttachment.clearDepth = 0
            if textureIndex < drawable.rasterizationRateMaps.count {
                pass.rasterizationRateMap = drawable.rasterizationRateMaps[textureIndex]
            }
            if layout == .layered {
                pass.renderTargetArrayLength = viewIndices.count
            }

            guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else { continue }
            encoder.label = "Probe pass"
            encoder.setRenderPipelineState(pipeline)
            encoder.setDepthStencilState(depthState)
            encoder.setCullMode(.none)
            encoder.setViewports(viewIndices.map { drawable.views[$0].textureMap.viewport })
            if viewIndices.count > 1 {
                var mappings = viewIndices.indices.map {
                    MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32($0),
                                                      renderTargetArrayIndexOffset: layout == .layered ? UInt32($0) : 0)
                }
                encoder.setVertexAmplificationCount(viewIndices.count, viewMappings: &mappings)
            }
            let first = viewProjection(viewIndices[0])
            let second = viewIndices.count > 1 ? viewProjection(viewIndices[1]) : first
            var uniforms = ProbeUniforms(viewProjection: (first, second))
            encoder.setVertexBytes(&uniforms, length: MemoryLayout<ProbeUniforms>.stride, index: Int(ProbeBufferUniforms))

            encoder.setVertexBuffer(staticVertices, offset: 0, index: Int(ProbeBufferVertices))
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: staticCount)
            encoder.setVertexBuffer(dynamicBuffer, offset: 0, index: Int(ProbeBufferVertices))
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: dynamicCount)
            encoder.endEncoding()
        }
    }

    private func logTexturesIfChanged(_ drawable: LayerRenderer.Drawable) {
        let textures = drawable.colorTextures.map { "\($0.width)x\($0.height)x\($0.arrayLength) fmt \($0.pixelFormat.rawValue)" }
        let eyes = drawable.views.map { v -> String in
            let t = v.transform.columns.3
            let vp = v.textureMap.viewport
            return String(format: "eye pos (%.4f, %.4f, %.4f) viewport %.0fx%.0f slice %d", t.x, t.y, t.z, vp.width, vp.height, v.textureMap.sliceIndex)
        }
        var ipd = ""
        if drawable.views.count == 2 {
            let a = drawable.views[0].transform.columns.3, b = drawable.views[1].transform.columns.3
            ipd = String(format: " ipd=%.1f mm", simd_distance(SIMD3(a.x, a.y, a.z), SIMD3(b.x, b.y, b.z)) * 1000)
        }
        let description = "drawable: target=\(drawable.target) colorTextures=\(textures) rasterizationRateMaps=\(drawable.rasterizationRateMaps.count) views=\(eyes)\(ipd) depthRange=\(drawable.depthRange)"
        if description != lastTextureDescription {
            lastTextureDescription = description
            ProbeLog.shared.note(description)
        }
    }
}

// MARK: - per-frame record and stats

struct FrameRow {
    var frameIndex: UInt64
    var tQuery: Double
    var tInput: Double
    var optimalInput: Double
    var deadline: Double
    var predictedPresentation: Double
    var trackableAnchorTime: Double
    var drawablePresentation = 0.0
    var presentationFrameIndex: UInt64 = 0
    var drawables: Int
    var poseQueryTime = 0.0
    var poseDelayMs: Double
    var anchor: DeviceAnchor?
    var tCommit = 0.0
    var tGPUDone = 0.0
    var gpuMs = 0.0
    var late = false
    var flash: Bool
    var repeatCount: Int

    init(frameIndex: UInt64, tQuery: Double, tInput: Double, timing: LayerRenderer.Frame.Timing,
         drawables: Int, poseDelayMs: Double, flash: Bool, repeatCount: Int) {
        self.frameIndex = frameIndex
        self.tQuery = tQuery
        self.tInput = tInput
        optimalInput = timing.optimalInputTime.seconds
        deadline = timing.renderingDeadline.seconds
        predictedPresentation = timing.presentationTime.seconds
        trackableAnchorTime = timing.trackableAnchorTime.seconds
        self.drawables = drawables
        self.poseDelayMs = poseDelayMs
        self.flash = flash
        self.repeatCount = repeatCount
    }

    var csvFields: [String] {
        let head = anchor?.originFromAnchorTransform.columns.3
        return [
            "\(frameIndex)", tQuery.s6, tInput.s6, optimalInput.s6, deadline.s6, predictedPresentation.s6,
            trackableAnchorTime.s6, drawablePresentation.s6, "\(presentationFrameIndex)", "\(drawables)",
            poseQueryTime.s6, poseDelayMs.ms3,
            anchor == nil ? "0" : "1", anchor?.isTracked == true ? "1" : "0",
            anchor.map { $0.timestamp.s6 } ?? "",
            head.map { String(format: "%.4f", $0.x) } ?? "", head.map { String(format: "%.4f", $0.y) } ?? "",
            head.map { String(format: "%.4f", $0.z) } ?? "",
            tCommit.s6, tGPUDone.s6, gpuMs.ms3, late ? "1" : "0", flash ? "1" : "0", "\(repeatCount)",
        ]
    }
}

/// Counters shared by the render thread and Metal's completion handlers.
final class StatsAccumulator: @unchecked Sendable {
    private let lock = NSLock()
    private var s = ProbeStats()
    private var anchored = 0
    private var inputToPresent: [Double] = []
    private var queryToPresent: [Double] = []
    private var gpu: [Double] = []
    private var intervals: [Double] = []

    func add(_ row: FrameRow, intervalMs: Double?) {
        lock.withLock {
            s.framesRendered += 1
            if row.late { s.framesLate += 1 }
            if row.anchor != nil { anchored += 1 }
            s.anchorTracked = row.anchor?.isTracked ?? false
            if let t = row.anchor?.originFromAnchorTransform.columns.3 { s.head = SIMD3(t.x, t.y, t.z) }
            s.anchorFraction = Double(anchored) / Double(s.framesRendered)
            Self.push(&inputToPresent, (row.drawablePresentation - row.tInput) * 1000)
            Self.push(&queryToPresent, (row.drawablePresentation - row.tQuery) * 1000)
            Self.push(&gpu, row.gpuMs)
            if let intervalMs { Self.push(&intervals, intervalMs) }
        }
    }

    func skipped(_ n: Int) { lock.withLock { s.framesSkipped += max(0, n) } }
    func noDrawable() { lock.withLock { s.framesWithoutDrawable += 1 } }

    func snapshot() -> ProbeStats {
        lock.withLock {
            var out = s
            out.inputToPresentMs = Self.median(inputToPresent)
            out.queryToPresentMs = Self.median(queryToPresent)
            out.gpuMs = Self.median(gpu)
            out.frameIntervalMs = Self.median(intervals)
            return out
        }
    }

    private static func push(_ a: inout [Double], _ v: Double) {
        a.append(v)
        if a.count > 240 { a.removeFirst(a.count - 240) }
    }

    private static func median(_ a: [Double]) -> Double {
        guard !a.isEmpty else { return 0 }
        let sorted = a.sorted()
        return sorted[sorted.count / 2]
    }
}

extension LayerRenderer.Clock.Instant {
    /// Seconds on the mach-absolute clock (same timebase as CACurrentMediaTime).
    var seconds: Double {
        let c = LayerRenderer.Clock.Instant.epoch.duration(to: self).components
        return Double(c.seconds) + Double(c.attoseconds) * 1e-18
    }
}
