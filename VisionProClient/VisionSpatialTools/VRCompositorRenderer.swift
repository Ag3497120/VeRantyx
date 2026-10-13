import ARKit
import CompositorServices
import CoreGraphics
import CoreText
import CoreVideo
import Foundation
import Metal

struct VRRenderStatistics {
    let renderFPS: Int
    let newFrameFPS: Int
    let frameAgeMilliseconds: Int
    let anchored: Bool
    let gpuErrors: Int
    let eyeGeometry: String
}

/// The eye frustum SteamVR renders with, sent by the Mac encoder. SteamVR
/// fixes it when it starts, which can be before this headset connects, so it
/// may differ from the Vision Pro's own eye frustum.
final class VRSourceProjectionStore {
    static let shared = VRSourceProjectionStore()

    private let lock = NSLock()
    private var tangents: (left: SIMD4<Float>, right: SIMD4<Float>)?

    func update(left: SIMD4<Float>, right: SIMD4<Float>) {
        func valid(_ t: SIMD4<Float>) -> Bool {
            t.x.isFinite && t.y.isFinite && t.z.isFinite && t.w.isFinite &&
                t.x < 0 && t.y > 0 && t.z < 0 && t.w > 0
        }
        guard valid(left), valid(right) else { return }
        lock.lock()
        tangents = (left, right)
        lock.unlock()
    }

    func snapshot() -> (left: SIMD4<Float>, right: SIMD4<Float>)? {
        lock.lock()
        defer { lock.unlock() }
        return tangents
    }
}

final class VRCompositorRenderer {
    private let layerRenderer: LayerRenderer
    private let poseSampler: VRPoseSampler
    private let projectionTangents: VisionProjectionTangents
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let textureCache: CVMetalTextureCache
    private let pipelineState: MTLRenderPipelineState
    private let overlayPipelineState: MTLRenderPipelineState?
    private let depthState: MTLDepthStencilState
    private let samplerState: MTLSamplerState
    // The fps overlay: a head-locked text quad refreshed once a second from
    // VRStreamStatsStore. UserDefaults key shared with SteamVRStreamerView.
    static let showFpsOverlayKey = "showFpsOverlay"
    private var overlayTexture: MTLTexture?
    private var overlayText = ""
    private var overlayRefreshedAt: CFTimeInterval = 0
    private var overlayNewFrames = 0
    private var overlayEnabled = true
    private var didPublishProjectionTangents = false
    private let reportStatistics: (VRRenderStatistics) -> Void
    private let reportState: (String) -> Void
    private var lastReportedState = ""
    private var lastHeartbeatAt: CFTimeInterval = 0
    private var cachedVideoFrame: VRDecodedFrame?
    private var cachedVideoTexture: CVMetalTexture?
    private var cachedSource: DeviceAnchorHistory.Sample?
    private var eyeCenter = SIMD3<Float>.zero
    private var eyeSeparation: Float = 0
    private let statisticsLock = NSLock()
    private var completedFrames = 0
    private var gpuErrors = 0
    private var newVideoFrames = 0
    private var statisticsStartedAt = CACurrentMediaTime()

    init(
        layerRenderer: LayerRenderer,
        poseSampler: VRPoseSampler,
        reportStatistics: @escaping (VRRenderStatistics) -> Void,
        reportState: @escaping (String) -> Void = { _ in }
    ) throws {
        self.reportState = reportState
        self.layerRenderer = layerRenderer
        self.poseSampler = poseSampler
        self.projectionTangents = poseSampler.projectionTangents
        self.reportStatistics = reportStatistics
        self.device = layerRenderer.device

        guard let commandQueue = device.makeCommandQueue() else {
            throw RendererError.commandQueueUnavailable
        }
        self.commandQueue = commandQueue

        var textureCache: CVMetalTextureCache?
        let cacheStatus = CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &textureCache)
        guard cacheStatus == kCVReturnSuccess, let textureCache else {
            throw RendererError.textureCacheUnavailable(cacheStatus)
        }
        self.textureCache = textureCache

        let samplerDescriptor = MTLSamplerDescriptor()
        samplerDescriptor.minFilter = .linear
        samplerDescriptor.magFilter = .linear
        samplerDescriptor.sAddressMode = .clampToEdge
        samplerDescriptor.tAddressMode = .clampToEdge
        guard let samplerState = device.makeSamplerState(descriptor: samplerDescriptor) else {
            throw RendererError.samplerUnavailable
        }
        self.samplerState = samplerState
        self.pipelineState = try Self.makePipeline(device: device, configuration: layerRenderer.configuration)
        // The overlay is optional: a failure here must not stop the video.
        self.overlayPipelineState = try? Self.makeOverlayPipeline(device: device, configuration: layerRenderer.configuration)
        let depthDescriptor = MTLDepthStencilDescriptor()
        depthDescriptor.depthCompareFunction = .always
        depthDescriptor.isDepthWriteEnabled = true
        guard let depthState = device.makeDepthStencilState(descriptor: depthDescriptor) else {
            throw RendererError.depthStateUnavailable
        }
        self.depthState = depthState
    }

    func startRenderLoop() {
        Thread.detachNewThread { [self] in
            runRenderLoop()
        }
    }

    private func runRenderLoop() {
        // This thread also paces the pose stream to the Mac.
        Thread.current.qualityOfService = .userInteractive
        defer { reportState("loop exited (layer invalidated)") }
        while layerRenderer.state != .invalidated {
            // Log when visionOS pauses the space (Home View or Control Center
            // opened over it) and when it resumes.
            let stateName = layerRenderer.state == .paused ? "paused" : "running"
            if stateName != lastReportedState {
                lastReportedState = stateName
                reportState(stateName)
            }
            // Heartbeat: a crash report without this thread would otherwise
            // leave no trace of whether the loop had stalled or exited.
            let heartbeatNow = CACurrentMediaTime()
            if heartbeatNow - lastHeartbeatAt >= 15 {
                lastHeartbeatAt = heartbeatNow
                reportState("alive (\(stateName), video \(cachedVideoFrame == nil ? "none" : "cached"))")
            }
            if layerRenderer.state == .paused {
                Thread.sleep(forTimeInterval: 0.05)
                continue
            }

            guard let frame = layerRenderer.queryNextFrame() else {
                Thread.sleep(forTimeInterval: 0.001)
                continue
            }
            frame.startUpdate()
            frame.endUpdate()

            guard let timing = frame.predictTiming() else {
                Thread.sleep(forTimeInterval: 0.001)
                continue
            }
            LayerRenderer.Clock().wait(until: timing.optimalInputTime)
            let presentationTime = Self.seconds(timing.presentationTime)
            // One pose packet per displayed frame keeps SteamVR's head stream
            // as even as the display itself.
            poseSampler.sample(presentationTime: presentationTime, now: CACurrentMediaTime())

            frame.startSubmission()
            let drawables = frame.queryDrawables()
            // No drawables means visionOS cancelled the frame (the space is
            // paused, for example while a level loads or Home View is open).
            // Such a frame is invalid: ending its submission made visionOS
            // abort the app with "BUG IN CLIENT".
            guard let drawable = drawables.first(where: { $0.target == .builtIn }) ?? drawables.first else {
                continue
            }

            if drawable.views.count >= 2 {
                // SteamVR places its eyes ±IPD/2 around the HMD pose. Send the
                // midpoint of the real eyes as that pose so near objects line up.
                let t0 = drawable.views[0].transform.columns.3
                let t1 = drawable.views[1].transform.columns.3
                eyeCenter = SIMD3<Float>((t0.x + t1.x) / 2, (t0.y + t1.y) / 2, (t0.z + t1.z) / 2)
                eyeSeparation = simd_distance(SIMD3<Float>(t0.x, t0.y, t0.z), SIMD3<Float>(t1.x, t1.y, t1.z))
                poseSampler.updateEyeCenter(eyeCenter)
            }

            if !didPublishProjectionTangents,
               drawable.views.count >= 2,
               let leftTangents = Self.openVRTangents(from: drawable, viewIndex: 0),
               let rightTangents = Self.openVRTangents(from: drawable, viewIndex: 1) {
                projectionTangents.update(left: leftTangents, right: rightTangents)
                didPublishProjectionTangents = true
                print("Vision Pro eye frustums published to SteamVR: left=\(leftTangents), right=\(rightTangents)")
            }

            let videoFrame = VRVideoFrameStore.shared.snapshot()
            var isNewVideoFrame = false
            if videoFrame?.pixelBuffer !== cachedVideoFrame?.pixelBuffer {
                cachedVideoFrame = videoFrame
                cachedVideoTexture = nil
                cachedSource = nil
                isNewVideoFrame = videoFrame != nil
                if videoFrame != nil {
                    newVideoFrames += 1
                    overlayNewFrames += 1
                }
            }
            refreshOverlayIfDue(now: CACurrentMediaTime())
            // The drawable anchor must describe the pose used to make its pixels.
            // The frame carries the ID of the exact pose SteamVR rendered with,
            // so the compositor reprojects from that pose to the display pose.
            // Supplying the display-time pose instead would tell it the image
            // was rendered from a newer viewpoint, which warps head motion.
            var anchor: DeviceAnchor?
            if let videoFrame {
                if videoFrame.timestamp > 0,
                   let source = cachedSource ?? poseSampler.history.sample(for: videoFrame.timestamp),
                   source.anchor.isTracked,
                   // While the game loads a level no frames arrive for many
                   // seconds. Reprojecting the last frame from a pose that old
                   // swings it across the view and gives the compositor's warp
                   // pass an enormous delta; show it head-locked instead.
                   presentationTime - source.sampledAt < 0.5 {
                    // Keep this frame's source anchor while reusing its pixels.
                    cachedSource = source
                    anchor = source.anchor
                    if isNewVideoFrame {
                        poseSampler.reportDisplayed(poseID: source.id, sampledAt: source.sampledAt,
                                                    presentationTime: presentationTime)
                    }
                }
            }
            if anchor == nil {
                // No fresh frame (level load, or a frame whose pose is unknown):
                // give the compositor the predicted display pose, which shows
                // the last image head-locked. Both a seconds-old anchor and no
                // anchor at all were followed by the system killing the app a
                // few seconds into every level load.
                anchor = poseSampler.queryDeviceAnchor(at: Self.seconds(timing.trackableAnchorTime))
            }
            // Every drawable must be presented, including the extra one
            // visionOS adds while recording or taking a screenshot.
            for target in drawables {
                target.deviceAnchor = anchor
                render(drawable: target, videoFrame: videoFrame)
            }
            frame.endSubmission()
            let now = CACurrentMediaTime()
            let interval = now - statisticsStartedAt
            if interval >= 5 {
                statisticsLock.lock()
                let completed = completedFrames
                let errors = gpuErrors
                completedFrames = 0
                statisticsLock.unlock()
                let age = cachedSource.map { Int(max(0, presentationTime - $0.sampledAt) * 1000) } ?? -1
                reportStatistics(VRRenderStatistics(
                    renderFPS: Int((Double(completed) / interval).rounded()),
                    newFrameFPS: Int((Double(newVideoFrames) / interval).rounded()),
                    frameAgeMilliseconds: age,
                    anchored: drawable.deviceAnchor != nil,
                    gpuErrors: errors,
                    eyeGeometry: String(format: "eye_center=(%.3f,%.3f,%.3f) ipd=%.1fmm",
                                        eyeCenter.x, eyeCenter.y, eyeCenter.z, eyeSeparation * 1000)
                ))
                statisticsStartedAt = now
                newVideoFrames = 0
            }
        }
    }

    private static func seconds(_ instant: LayerRenderer.Clock.Instant) -> CFTimeInterval {
        let components = LayerRenderer.Clock.Instant.epoch.duration(to: instant).components
        return CFTimeInterval(components.seconds) + CFTimeInterval(components.attoseconds) * 1e-18
    }

    private static func openVRTangents(
        from drawable: LayerRenderer.Drawable,
        viewIndex: Int
    ) -> SIMD4<Float>? {
        let projection = drawable.computeProjection(convention: .rightUpBack, viewIndex: viewIndex)
        let xScale = projection.columns.0.x
        let yScale = projection.columns.1.y
        let xOffset = projection.columns.2.x
        let yOffset = projection.columns.2.y
        guard xScale.isFinite, yScale.isFinite, xOffset.isFinite, yOffset.isFinite,
              abs(xScale) > 0.0001, abs(yScale) > 0.0001 else {
            return nil
        }

        // In right-up-back clip space, left/right are -1/+1 and bottom/top are
        // -1/+1. OpenVR names the vertical clipping arguments in the opposite
        // order, so SIMD lanes are [left, right, bottom, top].
        let left = (-1 + xOffset) / xScale
        let right = (1 + xOffset) / xScale
        let bottom = (-1 + yOffset) / yScale
        let top = (1 + yOffset) / yScale
        // SteamVR renders each eye facing straight ahead from the head. If a
        // Vision Pro view is rotated relative to the device, express its
        // frustum in device axes so the game's image still covers it.
        let viewTransform = drawable.views[viewIndex].transform
        let rotation = simd_float3x3(
            SIMD3<Float>(viewTransform.columns.0.x, viewTransform.columns.0.y, viewTransform.columns.0.z),
            SIMD3<Float>(viewTransform.columns.1.x, viewTransform.columns.1.y, viewTransform.columns.1.z),
            SIMD3<Float>(viewTransform.columns.2.x, viewTransform.columns.2.y, viewTransform.columns.2.z)
        )
        var bounds = SIMD4<Float>(.infinity, -.infinity, .infinity, -.infinity)
        for (u, v) in [(left, bottom), (right, bottom), (left, top), (right, top)] {
            let direction = rotation * SIMD3<Float>(u, v, -1)
            guard direction.z < -0.0001 else { return nil }
            let deviceU = direction.x / -direction.z
            let deviceV = direction.y / -direction.z
            bounds = SIMD4<Float>(min(bounds.x, deviceU), max(bounds.y, deviceU),
                                  min(bounds.z, deviceV), max(bounds.w, deviceV))
        }
        let tangents = bounds
        guard tangents.x < 0, tangents.y > 0, tangents.z < 0, tangents.w > 0,
              tangents.x.isFinite, tangents.y.isFinite,
              tangents.z.isFinite, tangents.w.isFinite else {
            return nil
        }
        return tangents
    }

    private func render(drawable: LayerRenderer.Drawable, videoFrame: VRDecodedFrame?) {
        guard let commandBuffer = commandQueue.makeCommandBuffer() else {
            return
        }

        var videoTexture: MTLTexture?
        var cvMetalTexture = cachedVideoTexture
        if cvMetalTexture == nil, let videoFrame {
            let width = CVPixelBufferGetWidth(videoFrame.pixelBuffer)
            let height = CVPixelBufferGetHeight(videoFrame.pixelBuffer)
            // 10-bit streams arrive as half floats holding sRGB-encoded values: the shader linearizes them.
            let halfFloat = CVPixelBufferGetPixelFormatType(videoFrame.pixelBuffer) == kCVPixelFormatType_64RGBAHalf
            let status = CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault,
                textureCache,
                videoFrame.pixelBuffer,
                nil,
                // The frames carry sRGB-encoded colour (SteamVR's display output). Read as sRGB, they come out
                // linear and the sRGB drawable encodes them once; read as plain UNORM they were encoded a second
                // time, which washed the picture out (pale, low-contrast colours).
                halfFloat ? .rgba16Float : .bgra8Unorm_srgb,
                width,
                height,
                0,
                &cvMetalTexture
            )
            if status == kCVReturnSuccess, let cvMetalTexture {
                cachedVideoTexture = cvMetalTexture
            }
        }
        if let cvMetalTexture {
            videoTexture = CVMetalTextureGetTexture(cvMetalTexture)
        }

        let renderPass = MTLRenderPassDescriptor()
        renderPass.colorAttachments[0].texture = drawable.colorTextures[0]
        renderPass.colorAttachments[0].loadAction = .clear
        renderPass.colorAttachments[0].storeAction = .store
        renderPass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        if !drawable.depthTextures.isEmpty {
            renderPass.depthAttachment.texture = drawable.depthTextures[0]
            renderPass.depthAttachment.loadAction = .clear
            renderPass.depthAttachment.storeAction = .store
            // Compositor Services uses reverse-Z: 1 is the near plane, 0 is
            // far. The video quad writes its own depth below.
            renderPass.depthAttachment.clearDepth = 0.0
        }
        if layerRenderer.configuration.layout == .layered {
            renderPass.renderTargetArrayLength = drawable.views.count
        }

        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: renderPass) else {
            return
        }
        encoder.setRenderPipelineState(pipelineState)
        encoder.setDepthStencilState(depthState)
        encoder.setCullMode(.none)
        var corners = Self.videoCorners(drawable: drawable, source: VRSourceProjectionStore.shared.snapshot(),
                                        eyeCenter: eyeCenter)
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
        encoder.setViewports(drawable.views.map { $0.textureMap.viewport })

        if layerRenderer.configuration.layout == .layered {
            var mappings = drawable.views.indices.map { index in
                MTLVertexAmplificationViewMapping(
                    viewportArrayIndexOffset: UInt32(index),
                    renderTargetArrayIndexOffset: UInt32(index)
                )
            }
            encoder.setVertexAmplificationCount(drawable.views.count, viewMappings: &mappings)
        }

        if let videoTexture {
            encoder.setFragmentTexture(videoTexture, index: 0)
            encoder.setFragmentSamplerState(samplerState, index: 0)
            var linearize: UInt32 = videoTexture.pixelFormat == .rgba16Float ? 1 : 0
            encoder.setFragmentBytes(&linearize, length: MemoryLayout<UInt32>.size, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6)
        }
        if overlayEnabled, let overlayPipelineState, let overlayTexture {
            var overlayCorners = Self.overlayCorners(drawable: drawable, texture: overlayTexture)
            encoder.setRenderPipelineState(overlayPipelineState)
            encoder.setVertexBytes(&overlayCorners, length: MemoryLayout<SIMD4<Float>>.stride * overlayCorners.count, index: 0)
            encoder.setFragmentTexture(overlayTexture, index: 0)
            encoder.setFragmentSamplerState(samplerState, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6)
        }
        encoder.endEncoding()

        drawable.encodePresent(commandBuffer: commandBuffer)
        // The drawable texture is sampled until GPU completion. Keep both CoreVideo
        // wrappers alive for that whole interval instead of releasing them on return.
        commandBuffer.addCompletedHandler { [self, cvMetalTexture, videoFrame] buffer in
            _ = cvMetalTexture
            _ = videoFrame
            statisticsLock.lock()
            if buffer.status == .completed { completedFrames += 1 }
            else if buffer.status == .error { gpuErrors += 1 }
            statisticsLock.unlock()
        }
        commandBuffer.commit()
    }

    /// Clip-space corners of each eye's video image, 4 per view: bottom-left,
    /// bottom-right, top-left, top-right. Each eye image is the plane SteamVR
    /// rendered: its frustum `source` (tangents in head axes) seen from that
    /// eye, placed 10 m away. Projecting it with the Vision Pro view keeps the
    /// image at the true angles even when SteamVR's frustum differs from the
    /// display's, instead of stretching it over the viewport. The 10 m depth
    /// also gives the compositor a real distance: depth 1 (the near plane)
    /// made its reprojection swing the image, and depth 0 hid it.
    private static func videoCorners(
        drawable: LayerRenderer.Drawable,
        source: (left: SIMD4<Float>, right: SIMD4<Float>)?,
        eyeCenter: SIMD3<Float>
    ) -> [SIMD4<Float>] {
        let distance: Float = 10
        let halfIPD: Float = 0.0315 // SteamVR's default 63 mm, which the driver reports.
        var corners: [SIMD4<Float>] = []
        for index in 0..<min(drawable.views.count, 2) {
            let fallback = SIMD4<Float>(-1, 1, -1, 1)
            let tangents = (index == 0 ? source?.left : source?.right)
                ?? openVRTangents(from: drawable, viewIndex: index) ?? fallback
            let projection = drawable.computeProjection(convention: .rightUpBack, viewIndex: index)
            let viewFromDevice = drawable.views[index].transform.inverse
            // SteamVR's eyes: the sent HMD pose (the real eye midpoint) ± IPD/2.
            let eye = eyeCenter + SIMD3<Float>(index == 0 ? -halfIPD : halfIPD, 0, 0)
            for (u, v) in [(tangents.x, tangents.z), (tangents.y, tangents.z), (tangents.x, tangents.w), (tangents.y, tangents.w)] {
                let point = eye + SIMD3<Float>(u, v, -1) * distance
                corners.append(projection * viewFromDevice * SIMD4<Float>(point, 1))
            }
        }
        while corners.count < 8 { corners.append(SIMD4<Float>(0, 0, 0, 1)) }
        return corners
    }

    // MARK: - fps overlay

    private static let overlayTextureWidth = 1024
    private static let overlayTextureHeight = 128

    /// Rebuilds the overlay text (and its texture) once a second on the
    /// render thread. Reading UserDefaults here keeps the toggle in the main
    /// window effective without any main-thread hop.
    private func refreshOverlayIfDue(now: CFTimeInterval) {
        let elapsed = now - overlayRefreshedAt
        guard elapsed >= 1 else { return }
        let shownFPS = overlayRefreshedAt > 0 ? Int((Double(overlayNewFrames) / elapsed).rounded()) : 0
        overlayNewFrames = 0
        overlayRefreshedAt = now
        overlayEnabled = UserDefaults.standard.object(forKey: Self.showFpsOverlayKey) as? Bool ?? true
        guard overlayEnabled, overlayPipelineState != nil else { return }

        let stats = VRStreamStatsStore.shared.snapshot()
        // The Mac's numbers go stale when its packets stop (level load, link loss).
        let statsFresh = stats.statsReceivedAt > 0 && ProcessInfo.processInfo.systemUptime - stats.statsReceivedAt < 3
        let gameFPS = statsFresh ? String(Int(stats.gameFPS.rounded())) : "–"
        let latency = stats.latencyMilliseconds >= 0 ? "\(stats.latencyMilliseconds) ms" : "–"
        let line1 = String(format: String(localized: "game %@ fps / received %d fps / shown %d fps / latency %@"),
                           gameFPS, stats.receivedFPS, shownFPS, latency)
        let engine = stats.audioEngineRunning ? String(localized: "running") : String(localized: "stopped")
        let line2 = String(format: String(localized: "audio %d pkt/s (Mac %d) / played %d / dropped %d / underruns %d / engine %@"),
                           stats.audioPacketsPerSecond, Int(stats.macAudioPacketsPerSecond.rounded()),
                           stats.audioPlayed, stats.audioDropped, stats.audioUnderruns, engine)
        let text = line1 + "\n" + line2
        guard text != overlayText || overlayTexture == nil else { return }
        overlayText = text
        if let texture = makeOverlayTexture(lines: [line1, line2]) {
            overlayTexture = texture
        }
    }

    /// Draws the lines with Core Text into a premultiplied BGRA bitmap and
    /// uploads it as a shared texture.
    private func makeOverlayTexture(lines: [String]) -> MTLTexture? {
        let width = Self.overlayTextureWidth
        let height = Self.overlayTextureHeight
        let bitmapInfo = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
        guard let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                                      bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: bitmapInfo) else { return nil }
        context.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 0.6))
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        let font = CTFontCreateUIFontForLanguage(.system, 36, nil) ?? CTFontCreateWithName("Helvetica" as CFString, 36, nil)
        let attributes: [CFString: Any] = [
            kCTFontAttributeName: font,
            kCTForegroundColorAttributeName: CGColor(red: 1, green: 1, blue: 1, alpha: 1),
        ]
        let lineHeight = CGFloat(height) / CGFloat(max(1, lines.count))
        for (index, line) in lines.enumerated() {
            guard let attributed = CFAttributedStringCreate(nil, line as CFString, attributes as CFDictionary) else { continue }
            let ctLine = CTLineCreateWithAttributedString(attributed)
            let baseline = CGFloat(height) - lineHeight * CGFloat(index + 1) + lineHeight * 0.28
            context.textPosition = CGPoint(x: 24, y: baseline)
            CTLineDraw(ctLine, context)
        }
        guard let data = context.data else { return nil }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: width,
                                                                  height: height, mipmapped: false)
        descriptor.usage = .shaderRead
        descriptor.storageMode = .shared
        // A new texture each refresh (once a second): the previous one may
        // still be read by a frame in flight.
        guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
        texture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0,
                        withBytes: data, bytesPerRow: width * 4)
        return texture
    }

    /// Clip-space corners (same layout as `videoCorners`) of a head-locked
    /// quad 1.2 m in front of the eyes, below the centre of view, so the
    /// text sits over the game without hiding the aim point.
    private static func overlayCorners(drawable: LayerRenderer.Drawable, texture: MTLTexture) -> [SIMD4<Float>] {
        let distance: Float = 1.2
        let halfWidth: Float = 0.30
        let halfHeight = halfWidth * Float(texture.height) / Float(texture.width)
        let centerY: Float = -0.24
        var corners: [SIMD4<Float>] = []
        for index in 0..<min(drawable.views.count, 2) {
            let projection = drawable.computeProjection(convention: .rightUpBack, viewIndex: index)
            let viewFromDevice = drawable.views[index].transform.inverse
            for (x, y) in [(-halfWidth, centerY - halfHeight), (halfWidth, centerY - halfHeight),
                           (-halfWidth, centerY + halfHeight), (halfWidth, centerY + halfHeight)] {
                let point = SIMD4<Float>(x, y, -distance, 1)
                corners.append(projection * viewFromDevice * point)
            }
        }
        while corners.count < 8 { corners.append(SIMD4<Float>(0, 0, 0, 1)) }
        return corners
    }

    private static func makeOverlayPipeline(
        device: MTLDevice,
        configuration: LayerRenderer.Configuration
    ) throws -> MTLRenderPipelineState {
        let source = """
        #include <metal_stdlib>
        using namespace metal;

        struct VertexOut {
            float4 position [[position]];
            float2 uv;
        };

        vertex VertexOut overlayVertexShader(uint vertexID [[vertex_id]], uint eyeIndex [[amplification_id]],
                                             constant float4 *corners [[buffer(0)]]) {
            const uint cornerForVertex[6] = { 0, 1, 2, 2, 1, 3 };
            const float2 texCoords[4] = {
                float2(0.0, 1.0), float2(1.0, 1.0), float2(0.0, 0.0), float2(1.0, 0.0)
            };
            uint corner = cornerForVertex[vertexID];
            VertexOut out;
            out.position = corners[eyeIndex * 4 + corner];
            out.uv = texCoords[corner];
            return out;
        }

        // The texture holds premultiplied alpha; the pipeline blends with one / one-minus-source-alpha.
        fragment float4 overlayFragmentShader(
            VertexOut in [[stage_in]],
            texture2d<float, access::sample> overlayTexture [[texture(0)]],
            sampler overlaySampler [[sampler(0)]]) {
            return overlayTexture.sample(overlaySampler, in.uv);
        }
        """

        let library = try device.makeLibrary(source: source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "overlayVertexShader")
        descriptor.fragmentFunction = library.makeFunction(name: "overlayFragmentShader")
        descriptor.colorAttachments[0].pixelFormat = configuration.colorFormat
        descriptor.colorAttachments[0].isBlendingEnabled = true
        descriptor.colorAttachments[0].sourceRGBBlendFactor = .one
        descriptor.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        descriptor.colorAttachments[0].sourceAlphaBlendFactor = .one
        descriptor.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        descriptor.depthAttachmentPixelFormat = configuration.depthFormat
        descriptor.maxVertexAmplificationCount = configuration.layout == .layered ? 2 : 1
        descriptor.inputPrimitiveTopology = .triangle
        return try device.makeRenderPipelineState(descriptor: descriptor)
    }

    private static func makePipeline(
        device: MTLDevice,
        configuration: LayerRenderer.Configuration
    ) throws -> MTLRenderPipelineState {
        let source = """
        #include <metal_stdlib>
        using namespace metal;

        struct VertexOut {
            float4 position [[position]];
            float2 uv;
            uint eyeIndex;
        };

        vertex VertexOut vrVertexShader(uint vertexID [[vertex_id]], uint eyeIndex [[amplification_id]],
                                        constant float4 *corners [[buffer(0)]]) {
            // Corners per eye: bottom-left, bottom-right, top-left, top-right.
            const uint cornerForVertex[6] = { 0, 1, 2, 2, 1, 3 };
            const float2 texCoords[4] = {
                float2(0.0, 1.0), float2(1.0, 1.0), float2(0.0, 0.0), float2(1.0, 0.0)
            };
            uint corner = cornerForVertex[vertexID];
            VertexOut out;
            out.position = corners[eyeIndex * 4 + corner];
            out.uv = texCoords[corner];
            out.eyeIndex = eyeIndex;
            return out;
        }

        fragment float4 vrFragmentShader(
            VertexOut in [[stage_in]],
            texture2d<float, access::sample> videoTexture [[texture(0)]],
            sampler videoSampler [[sampler(0)]],
            constant uint &linearize [[buffer(0)]]) {
            float2 uv = in.uv;
            uv.x = uv.x * 0.5 + (in.eyeIndex == 0 ? 0.0 : 0.5);
            float4 c = videoTexture.sample(videoSampler, uv);
            if (linearize != 0) {        // sRGB-encoded half floats (10-bit streams): to linear, as an sRGB texture would
                float3 e = saturate(c.rgb);
                c.rgb = select(pow((e + 0.055) / 1.055, 2.4), e / 12.92, e <= 0.04045);
            }
            return c;
        }
        """

        let library = try device.makeLibrary(source: source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "vrVertexShader")
        descriptor.fragmentFunction = library.makeFunction(name: "vrFragmentShader")
        descriptor.colorAttachments[0].pixelFormat = configuration.colorFormat
        descriptor.depthAttachmentPixelFormat = configuration.depthFormat
        descriptor.maxVertexAmplificationCount = configuration.layout == .layered ? 2 : 1
        descriptor.inputPrimitiveTopology = .triangle
        return try device.makeRenderPipelineState(descriptor: descriptor)
    }

    private enum RendererError: Error {
        case commandQueueUnavailable
        case textureCacheUnavailable(CVReturn)
        case samplerUnavailable
        case depthStateUnavailable
    }
}
