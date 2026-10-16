// A test stream for alvr_probe: both eyes side by side (left | right, as ALVR's clients expect), each with a moving
// bar, a grid and a frame counter, encoded by VideoToolbox (HEVC, the hardware encoder VeRantyx's Mac side uses) and
// written as Annex-B: VPS SPS PPS before each IDR.
//
//     swift tools/hevc_testpattern.swift <out.h265> [eye width] [eye height] [frames]
import CoreGraphics
import CoreMedia
import CoreText
import Foundation
import VideoToolbox

let args = CommandLine.arguments
guard args.count > 1 else { print("usage: hevc_testpattern <out.h265> [eye width] [eye height] [frames]"); exit(2) }
let eyeW = args.count > 2 ? Int(args[2])! : 2144, eyeH = args.count > 3 ? Int(args[3])! : 2016
let count = args.count > 4 ? Int(args[4])! : 144
let width = eyeW * 2, height = eyeH
var out = Data()

var session: VTCompressionSession?
VTCompressionSessionCreate(allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_HEVC,
                           encoderSpecification: [kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: true] as CFDictionary,
                           imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &session)
guard let vt = session else { print("no HEVC encoder"); exit(1) }
VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_HEVC_Main_AutoLevel)
VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_MaxKeyFrameInterval, value: 72 as CFNumber)
VTSessionSetProperty(vt, key: kVTCompressionPropertyKey_AverageBitRate, value: 30_000_000 as CFNumber)

let start: [UInt8] = [0, 0, 0, 1]
func append(_ sample: CMSampleBuffer) {
    let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]]
    let key = !(attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool ?? false)
    if key, let fd = CMSampleBufferGetFormatDescription(sample) {
        for i in 0..<3 {
            var p: UnsafePointer<UInt8>?; var n = 0
            if CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: &p,
                                                                  parameterSetSizeOut: &n, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr, let p {
                out.append(contentsOf: start); out.append(p, count: n)
            }
        }
    }
    guard let block = CMSampleBufferGetDataBuffer(sample) else { return }
    var length = 0; var ptr: UnsafeMutablePointer<CChar>?
    CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &length, dataPointerOut: &ptr)
    guard let base = ptr else { return }
    var off = 0
    while off + 4 <= length {                                       // (AVCC: 4-byte lengths, then the NAL unit)
        let n = Int(UInt32(bigEndian: UnsafeRawPointer(base + off).loadUnaligned(as: UInt32.self)))
        out.append(contentsOf: start)
        out.append(UnsafeRawPointer(base + off + 4).assumingMemoryBound(to: UInt8.self), count: n)
        off += 4 + n
    }
}

for f in 0..<count {
    var pb: CVPixelBuffer?
    CVPixelBufferCreate(nil, width, height, kCVPixelFormatType_32BGRA, [kCVPixelBufferIOSurfacePropertiesKey: [:]] as CFDictionary, &pb)
    guard let buf = pb else { exit(1) }
    CVPixelBufferLockBaseAddress(buf, [])
    let ctx = CGContext(data: CVPixelBufferGetBaseAddress(buf), width: width, height: height, bitsPerComponent: 8,
                        bytesPerRow: CVPixelBufferGetBytesPerRow(buf), space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
    for eye in 0..<2 {
        let x0 = CGFloat(eye * eyeW)
        ctx.setFillColor(eye == 0 ? CGColor(red: 0.10, green: 0.14, blue: 0.30, alpha: 1) : CGColor(red: 0.30, green: 0.12, blue: 0.12, alpha: 1))
        ctx.fill(CGRect(x: x0, y: 0, width: CGFloat(eyeW), height: CGFloat(eyeH)))
        ctx.setStrokeColor(CGColor(gray: 0.8, alpha: 1)); ctx.setLineWidth(3)
        for g in stride(from: 0, through: eyeW, by: eyeW / 8) { ctx.stroke(CGRect(x: x0 + CGFloat(g), y: 0, width: 0, height: CGFloat(eyeH))) }
        for g in stride(from: 0, through: eyeH, by: eyeH / 8) { ctx.stroke(CGRect(x: x0, y: CGFloat(g), width: CGFloat(eyeW), height: 0)) }
        let bar = CGFloat(f % 72) / 72 * CGFloat(eyeW - 120)
        ctx.setFillColor(CGColor(red: 1, green: 0.8, blue: 0.2, alpha: 1)); ctx.fill(CGRect(x: x0 + bar, y: CGFloat(eyeH) * 0.45, width: 120, height: 120))
        let text = "VeRantyx  \(eye == 0 ? "L" : "R")  \(f)" as CFString
        let font = CTFontCreateWithName("Menlo-Bold" as CFString, 110, nil)
        let line = CTLineCreateWithAttributedString(CFAttributedStringCreate(nil, text, [kCTFontAttributeName: font, kCTForegroundColorAttributeName: CGColor(gray: 1, alpha: 1)] as CFDictionary)!)
        ctx.textPosition = CGPoint(x: x0 + 160, y: CGFloat(eyeH) * 0.7); CTLineDraw(line, ctx)
    }
    CVPixelBufferUnlockBaseAddress(buf, [])
    let pts = CMTime(value: CMTimeValue(f), timescale: 72)
    VTCompressionSessionEncodeFrame(vt, imageBuffer: buf, presentationTimeStamp: pts, duration: CMTime(value: 1, timescale: 72),
                                    frameProperties: nil, infoFlagsOut: nil) { status, _, sample in
        if status == noErr, let sample { append(sample) }
    }
}
VTCompressionSessionCompleteFrames(vt, untilPresentationTimeStamp: .invalid)
try! out.write(to: URL(fileURLWithPath: args[1]))
print("hevc_testpattern: \(count) frames of \(width)x\(height), \(out.count) bytes -> \(args[1])")
