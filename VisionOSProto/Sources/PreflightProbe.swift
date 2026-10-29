import Darwin
import Foundation
import Metal

/// What the VXRuntime stack (the Cyberpunk AOT modules, the D3D12 layer) needs from the device, checked without any
/// game file: the modules' fixed image arena, the Metal features the D3D12 layer uses, runtime shader compilation,
/// process creation, and how Metal allocations count against the app's memory limit.
/// `-preflight NO` skips it; `-memfill YES` also fills shared Metal buffers until the limit is near.
enum PreflightProbe {
    /// A fixed reservation (vm_allocate VM_FLAGS_FIXED: fails rather than replacing a mapping), released at once.
    static func fixedReservation(at address: UInt, size: Int) -> String {
        var a = vm_address_t(address)
        let kr = vm_allocate(mach_task_self_, &a, vm_size_t(size), VM_FLAGS_FIXED)
        if kr != KERN_SUCCESS { return "refused (kern \(kr))" }
        vm_deallocate(mach_task_self_, a, vm_size_t(size))
        return "granted"
    }

    static func spawnProbe() -> String {
        var pid: pid_t = 0
        let argv: [UnsafeMutablePointer<CChar>?] = [strdup("/bin/sh"), strdup("-c"), strdup("true"), nil]
        defer { argv.forEach { free($0) } }
        let r = posix_spawn(&pid, "/bin/sh", nil, nil, argv, nil)
        if r != 0 { return "posix_spawn refused (\(r): \(String(cString: strerror(r))))" }
        var status: Int32 = 0
        waitpid(pid, &status, 0)
        return "posix_spawn allowed (status \(status))"
    }

    static func shaderCompile(device: MTLDevice) -> String {
        let src = """
        #include <metal_stdlib>
        using namespace metal;
        struct C { float4 a[64]; };
        kernel void k(device float4 *o [[buffer(0)]], constant C &c [[buffer(1)]], texture2d<float> t [[texture(0)]],
                      sampler s [[sampler(0)]], uint i [[thread_position_in_grid]]) {
            float4 v = 0;
            for (int j = 0; j < 64; j++) v += c.a[j] * t.sample(s, float2(i, j) / 64.0);
            o[i] = v;
        }
        """
        let t0 = Date()
        do {
            let lib = try device.makeLibrary(source: src, options: nil)
            guard let f = lib.makeFunction(name: "k") else { return "no function" }
            _ = try device.makeComputePipelineState(function: f)
            return String(format: "makeLibrary(source:) + pipeline: %.1f ms", Date().timeIntervalSince(t0) * 1000)
        } catch { return "failed: \(error.localizedDescription)" }
    }

    static func report(device: MTLDevice) -> [String] {
        var l: [String] = []
        l.append("preflight: fixed 0x140000000 + 256 MB (exe's own base): \(fixedReservation(at: 0x1_4000_0000, size: 256 << 20))")
        l.append("preflight: fixed 0xf00000000 + 768 MB (AOT image arena, exe + 30 DLLs): \(fixedReservation(at: 0xf_0000_0000, size: 768 << 20))")
        l.append("preflight: fixed 0xf00000000 + 4 GB: \(fixedReservation(at: 0xf_0000_0000, size: 4 << 30))")
        l.append("preflight: fixed 0x7ff000000000 + 64 MB: \(fixedReservation(at: 0x7FF0_0000_0000, size: 64 << 20))")
        l.append("preflight: \(spawnProbe())")
        l.append("preflight: Metal: BC textures \(device.supportsBCTextureCompression), metal3 \(device.supportsFamily(.metal3)), "
                 + "argument buffers tier \(device.argumentBuffersSupport.rawValue), read-write textures tier \(device.readWriteTextureSupport.rawValue), "
                 + "float32 filtering \(device.supports32BitFloatFiltering), barycentrics \(device.supportsShaderBarycentricCoordinates), "
                 + "raytracing \(device.supportsRaytracing), max buffer \(device.maxBufferLength >> 20) MB, "
                 + "sparse tile \(device.sparseTileSizeInBytes) B, depth32+stencil8 MSAA4 \(device.supportsTextureSampleCount(4)), "
                 + "unified memory \(device.hasUnifiedMemory), max threadgroup memory \(device.maxThreadgroupMemoryLength)")
        let rs = MTLResidencySetDescriptor()
        let set = try? device.makeResidencySet(descriptor: rs)
        l.append("preflight: residency set \(set != nil)")
        let q4 = device.makeMTL4CommandQueue()
        l.append("preflight: MTL4 command queue \(q4 != nil)")
        let hd = MTLHeapDescriptor()
        hd.size = 64 << 20
        hd.type = .placement
        hd.maxCompatiblePlacementSparsePageSize = .size64
        l.append("preflight: placement heap with sparse pages (64 KB) \(device.makeHeap(descriptor: hd) != nil)")
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bc7_rgbaUnorm, width: 256, height: 256, mipmapped: true)
        l.append("preflight: BC7 texture \(device.makeTexture(descriptor: td) != nil)")
        let dd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float_stencil8, width: 256, height: 256, mipmapped: false)
        dd.storageMode = .private; dd.usage = [.renderTarget, .shaderRead]
        l.append("preflight: depth32Float_stencil8 target \(device.makeTexture(descriptor: dd) != nil)")
        l.append("preflight: \(shaderCompile(device: device))")
        return l
    }

    /// Shared buffers of 256 MB, written, until os_proc_available_memory is under 512 MB: how the footprint and the
    /// limit move with Metal memory. (The app may be ended by jetsam: the log has every step before it.)
    static func memoryFill(device: MTLDevice, log: (String) -> Void) {
        var keep: [MTLBuffer] = []
        let step = 256 << 20
        log(String(format: "memfill: start: available %.0f MB, footprint %.0f MB", CapabilityProbe.availableMemoryMB(), CapabilityProbe.footprintMB()))
        while CapabilityProbe.availableMemoryMB() > 512 + 256 && keep.count < 64 {
            guard let b = device.makeBuffer(length: step, options: .storageModeShared) else { log("memfill: makeBuffer failed"); break }
            memset(b.contents(), 1, step)
            keep.append(b)
            log(String(format: "memfill: %d MB in Metal buffers: available %.0f MB, footprint %.0f MB, Metal allocated %.0f MB",
                       keep.count * 256, CapabilityProbe.availableMemoryMB(), CapabilityProbe.footprintMB(),
                       Double(device.currentAllocatedSize) / 1_048_576))
        }
        keep.removeAll()
        log(String(format: "memfill: released: available %.0f MB, footprint %.0f MB", CapabilityProbe.availableMemoryMB(), CapabilityProbe.footprintMB()))
    }
}
