import CoreML
import Darwin
import Foundation
import Metal
#if canImport(MetalFX)
import MetalFX
#endif
import os

/// One-shot runtime checks of what a standalone VXRuntime host can rely on.
/// Nothing here changes system state: memory reservations are released at once.
enum CapabilityProbe {
    static func availableMemoryMB() -> Double {
        Double(os_proc_available_memory()) / 1_048_576
    }

    static func footprintMB() -> Double {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return kr == KERN_SUCCESS ? Double(info.phys_footprint) / 1_048_576 : -1
    }

    /// Largest PROT_NONE reservation that succeeds (extended-virtual-addressing
    /// raises this; VXRuntime needs a large address space for the Windows heap
    /// layout and the 64 GB-scale file mappings).
    static func largestReservationGB() -> Int {
        var best = 0
        for gb in [1, 2, 4, 8, 16, 32, 64, 128, 256] {
            let size = gb << 30
            let p = mmap(nil, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0)
            if p == MAP_FAILED { break }
            munmap(p, size)
            best = gb
        }
        return best
    }

    /// Whether a hint at a PE-style image base is honoured (no MAP_FIXED).
    static func hintedReservation(at address: UInt, size: Int) -> String {
        let hint = UnsafeMutableRawPointer(bitPattern: address)
        let p = mmap(hint, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0)
        if p == MAP_FAILED { return "failed errno \(errno)" }
        defer { munmap(p, size) }
        let got = UInt(bitPattern: p)
        return got == address ? "granted at hint" : String(format: "moved to 0x%llx", UInt64(got))
    }

    /// Can the app create executable memory at runtime? (Third-party visionOS
    /// apps cannot: no dynamic-codesigning, so x86 code must be recompiled ahead of time.)
    static func jitProbe() -> String {
        let size = 16384
        let p = mmap(nil, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0)
        if p == MAP_FAILED { return "MAP_JIT refused (errno \(errno))" }
        munmap(p, size)
        return "MAP_JIT allowed"
    }

    static func report(device: MTLDevice) -> [String] {
        var lines: [String] = []
        let mem = ProcessInfo.processInfo.physicalMemory
        lines.append("Device RAM: \(mem >> 20) MB, os_proc_available_memory: \(Int(availableMemoryMB())) MB, footprint: \(Int(footprintMB())) MB")
        lines.append("Largest VA reservation: \(largestReservationGB()) GB")
        lines.append("Hint 0x140000000 (PE image base), 256 MB: \(hintedReservation(at: 0x1_4000_0000, size: 256 << 20))")
        lines.append("Hint 0x7FF000000000 (high DLL range), 64 MB: \(hintedReservation(at: 0x7FF0_0000_0000, size: 64 << 20))")
        lines.append("JIT: \(jitProbe())")
        lines.append("GPU: \(device.name), recommendedMaxWorkingSet \(device.recommendedMaxWorkingSetSize >> 20) MB, apple9 \(device.supportsFamily(.apple9)), apple8 \(device.supportsFamily(.apple8))")
        lines.append("Rasterization rate maps (2 layers): \(device.supportsRasterizationRateMap(layerCount: 2))")
        #if canImport(MetalFX)
        lines.append("MetalFX spatial scaler: \(MTLFXSpatialScalerDescriptor.supportsDevice(device)); temporal scaler / frame interpolator: API_UNAVAILABLE(visionos) in the visionOS 26.0 SDK")
        #else
        lines.append("MetalFX: framework absent from this SDK (visionOS simulator)")
        #endif
        let computeDevices = MLComputeDevice.allComputeDevices.map { d -> String in
            switch d {
            case .cpu: return "CPU"
            case .gpu(let g): return "GPU(\(g.metalDevice.name))"
            case .neuralEngine(let n): return "NeuralEngine(\(n.totalCoreCount) cores)"
            @unknown default: return "other"
            }
        }
        lines.append("Core ML compute devices: \(computeDevices.joined(separator: ", "))")
        lines.append("Thermal: \(ProcessInfo.processInfo.thermalState.rawValue), low power: \(ProcessInfo.processInfo.isLowPowerModeEnabled)")
        return lines
    }
}
