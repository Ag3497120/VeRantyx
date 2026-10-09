import ARKit
import CompositorServices
import CoreVideo
import Foundation
import Metal

struct VRRenderStatistics {
    let renderFPS: Int
    let newFrameFPS: Int
    let frameAgeMilliseconds: Int
    let anchored: Bool
    let gpuErrors: Int
}

final class VRCompositorRenderer {
    private let layerRenderer: LayerRenderer
    private let worldTrackingProvider: WorldTrackingProvider
    private let deviceAnchorHistory: DeviceAnchorHistory
    private let projectionTangents: VisionProjectionTangents
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let textureCache: CVMetalTextureCache
    private let pipelineState: MTLRenderPipelineState
    private let samplerState: MTLSamplerState
    private var didPublishProjectionTangents = false
    private let reportStatistics: (VRRenderStatistics) -> Void
    private var cachedVideoFrame: VRDecodedFrame?
    private var cachedVideoTexture: CVMetalTexture?
    private var cachedSourceAnchor: DeviceAnchor?
    private let statisticsLock = NSLock()
    private var completedFrames = 0
    private var gpuErrors = 0
    private var newVideoFrames = 0
    private var statisticsStartedAt = CACurrentMediaTime()

    init(
        layerRenderer: LayerRenderer,
        worldTrackingProvider: WorldTrackingProvider,
        deviceAnchorHistory: DeviceAnchorHistory,
        projectionTangents: VisionProjectionTangents,
        reportStatistics: @escaping (VRRenderStatistics) -> Void
    ) throws {
        self.layerRenderer = layerRenderer
        self.worldTrackingProvider = worldTrackingProvider
        self.deviceAnchorHistory = deviceAnchorHistory
        self.projectionTangents = projectionTangents
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
    }

    func startRenderLoop() {
        Thread.detachNewThread { [self] in
            runRenderLoop()
        }
    }

    private func runRenderLoop() {
        while layerRenderer.state != .invalidated {
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

            frame.startSubmission()
            defer { frame.endSubmission() }

            let drawables = frame.queryDrawables()
            guard let drawable = drawables.first(where: { $0.target == .builtIn }) ?? drawables.first else {
                continue
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
            if videoFrame?.pixelBuffer !== cachedVideoFrame?.pixelBuffer {
                cachedVideoFrame = videoFrame
                cachedVideoTexture = nil
                cachedSourceAnchor = nil
                if videoFrame != nil { newVideoFrames += 1 }
            }
            // The drawable anchor must describe the pose used to make its pixels.
            // For a streamed frame that is the Vision Pro pose timestamped by the
            // Mac capture path. Supplying the display-time pose here tells the
            // compositor that the older image was rendered from a newer viewpoint,
            // which disables useful late reprojection and looks warped when the
            // wearer moves their head.
            if let videoFrame {
                if videoFrame.timestamp > 0,
                   let sourceAnchor = cachedSourceAnchor ?? deviceAnchorHistory.anchor(near: videoFrame.timestamp),
                   sourceAnchor.isTracked {
                    // Keep this frame's source anchor while reusing its pixels.
                    // A slow game can outlive the tracking history's two-second
                    // window; that must not disable head-motion reprojection.
                    cachedSourceAnchor = sourceAnchor
                    drawable.deviceAnchor = sourceAnchor
                } else {
                    // Never label an old or untracked SteamVR frame with the
                    // current head pose. That makes compositor reprojection apply
                    // the wrong transform and bends the streamed image.
                    drawable.deviceAnchor = nil
                }
            } else if let predictedAnchor = worldTrackingProvider.queryDeviceAnchor(atTimestamp: CACurrentMediaTime()),
                      predictedAnchor.isTracked {
                drawable.deviceAnchor = predictedAnchor
            }
            render(drawable: drawable, videoFrame: videoFrame)
            let now = CACurrentMediaTime()
            let interval = now - statisticsStartedAt
            if interval >= 5 {
                statisticsLock.lock()
                let completed = completedFrames
                let errors = gpuErrors
                completedFrames = 0
                statisticsLock.unlock()
                let age = videoFrame.flatMap { $0.timestamp > 0 ? Int(max(0, now - $0.timestamp) * 1000) : nil } ?? -1
                reportStatistics(VRRenderStatistics(
                    renderFPS: Int((Double(completed) / interval).rounded()),
                    newFrameFPS: Int((Double(newVideoFrames) / interval).rounded()),
                    frameAgeMilliseconds: age,
                    anchored: drawable.deviceAnchor != nil,
                    gpuErrors: errors
                ))
                statisticsStartedAt = now
                newVideoFrames = 0
            }
        }
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
        let tangents = SIMD4<Float>(
            (-1 + xOffset) / xScale,
            (1 + xOffset) / xScale,
            (-1 + yOffset) / yScale,
            (1 + yOffset) / yScale
        )
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
            let status = CVMetalTextureCacheCreateTextureFromImage(
                kCFAllocatorDefault,
                textureCache,
                videoFrame.pixelBuffer,
                nil,
                .bgra8Unorm,
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
            renderPass.depthAttachment.clearDepth = 1.0
        }
        if layerRenderer.configuration.layout == .layered {
            renderPass.renderTargetArrayLength = drawable.views.count
        }

        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: renderPass) else {
            return
        }
        encoder.setRenderPipelineState(pipelineState)
        encoder.setCullMode(.none)
        let sourceAspect = videoFrame.map {
            Double(CVPixelBufferGetWidth($0.pixelBuffer)) / 2.0 / Double(CVPixelBufferGetHeight($0.pixelBuffer))
        } ?? (8.0 / 9.0)
        encoder.setViewports(drawable.views.map {
            Self.aspectFitViewport($0.textureMap.viewport, contentAspect: sourceAspect)
        })

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

    private static func aspectFitViewport(_ viewport: MTLViewport, contentAspect: Double) -> MTLViewport {
        guard viewport.width > 0, viewport.height > 0, contentAspect > 0 else { return viewport }
        let viewportAspect = viewport.width / viewport.height
        var fitted = viewport

        if contentAspect > viewportAspect {
            fitted.height = viewport.width / contentAspect
            fitted.originY += (viewport.height - fitted.height) * 0.5
        } else {
            fitted.width = viewport.height * contentAspect
            fitted.originX += (viewport.width - fitted.width) * 0.5
        }
        return fitted
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

        vertex VertexOut vrVertexShader(uint vertexID [[vertex_id]], uint eyeIndex [[amplification_id]]) {
            const float2 positions[6] = {
                float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0),
                float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0)
            };
            const float2 texCoords[6] = {
                float2(0.0, 1.0), float2(1.0, 1.0), float2(0.0, 0.0),
                float2(0.0, 0.0), float2(1.0, 1.0), float2(1.0, 0.0)
            };
            VertexOut out;
            out.position = float4(positions[vertexID], 0.0, 1.0);
            out.uv = texCoords[vertexID];
            out.eyeIndex = eyeIndex;
            return out;
        }

        fragment float4 vrFragmentShader(
            VertexOut in [[stage_in]],
            texture2d<float, access::sample> videoTexture [[texture(0)]],
            sampler videoSampler [[sampler(0)]]) {
            float2 uv = in.uv;
            uv.x = uv.x * 0.5 + (in.eyeIndex == 0 ? 0.0 : 0.5);
            return videoTexture.sample(videoSampler, uv);
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
    }
}
