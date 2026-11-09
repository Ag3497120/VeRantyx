// Adapted from VeRantyx421b682, MIT. See repository LICENSE.
import Foundation
import Metal
import CoreImage
import CoreText
import CoreVideo
import SceneKit
import AppKit
import simd
func loadingHeightOffset(_ history: UnsafeRawPointer) -> Float {
    // Match the driver's standing origin, as the Windows capture proxy does.
    // Native producers and older drivers leave this metadata absent.
    guard history.loadUnaligned(fromByteOffset: 4, as: UInt32.self) == 0x5348_5056,
          history.loadUnaligned(fromByteOffset: 8, as: UInt32.self) == 0x464f_4856 else { return 0 }
    let value = history.loadUnaligned(fromByteOffset: 12, as: Float.self)
    return value.isFinite ? value : 0
}

final class NativeLoadingMap {
    struct State: Decodable, Equatable {
        let type: String
        let mapName: String?
        let chapter: String?
        let description: String?
        let loadingText: String?
        let facingAngle: Float?
        var messageText: String?
        var visible: Bool?
        let waitingForInput: Bool?
    }
    struct Preparation: Decodable {
        let state: Int
        let phase: String?
        let done: Int
        let total: Int
        var active: Bool { (state == 0 || state == 1) && done >= 0 && total >= 0 }
        var fraction: Float { total > 0 ? min(1,max(0,Float(done)/Float(total))) : -1 }
        var title: String { state == 1 ? "Preparing shaders" : phase == "checking" ? "Checking shader cache" : "Starting Half-Life: Alyx" }
        var count: String { total > 0 ? "\(min(done,total)) / \(total)" : "Please wait" }
    }
    struct Stage: Decodable, Equatable {
        let model: String
        let transform: [Float]
        let settings: [Float]
        let cull: Int
        let greyscale: Int
        let wireframe: Int
        let token: UInt64
    }
    struct View {
        let image: CGImage
        let head: simd_float4x4
        let timestamp: Double
        let mapName: String
        let stageModel: String?
    }
    struct MapInfo: Decodable {
        struct Position: Decodable { let x: Float?; let y: Float?; let z: Float? }
        let pos: Position
        let cables: Int
        let hidden: Bool?
        let drawVault: Bool?
    }
    let path: String?
    let game: URL?
    let stagePath: String?
    fileprivate let queue = DispatchQueue(label: "verantyx.loading-scene", qos: .userInitiated, autoreleaseFrequency: .workItem)
    private let lock = NSLock()
    private var generation = 0
    private var image: View?
    private var pending = false
    private var lastCheck = -Double.infinity
    private(set) var state: State?
    private(set) var stage: Stage?
    var active: Bool { state != nil }
    var visible: Bool { state != nil && state?.visible != false }
    fileprivate var renderer: Scene?

    init(path: String?, game: String?, stagePath: String? = nil) {
        self.path = path; self.stagePath = stagePath
        self.game = game.map { URL(fileURLWithPath: $0).resolvingSymlinksInPath() }
    }
    static func validName(_ name: String) -> Bool {
        !name.isEmpty && name.utf8.count <= 96 && name.utf8.allSatisfy {
            (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || $0 == 95 || $0 == 45
        }
    }
    static func read<T: Decodable>(_ path: String?, as: T.Type) -> T? {
        guard let path, let size = (try? FileManager.default.attributesOfItem(atPath: path)[.size]) as? NSNumber,
              size.intValue <= 16384, let data = FileManager.default.contents(atPath: path) else { return nil }
        return try? JSONDecoder().decode(T.self, from: data)
    }
    func poll(now: Double, inputReady: Bool = true) {
        guard now - lastCheck >= 0.1 else { return }; lastCheck = now
        var next = Self.read(path, as: State.self)
        if next?.type != "begin_loading" || !Self.validName(next?.mapName ?? "") || game == nil { next = nil }
        let preparation = Self.read(ProcessInfo.processInfo.environment["ALYX_PREWARM_PROGRESS"], as: Preparation.self)
        if let preparation, preparation.active {
            next = State(type:"begin_loading",mapName:"__shader_preparation",chapter:preparation.title,
                         description:String(preparation.fraction),loadingText:preparation.count,facingAngle:0,
                         messageText:preparation.state == 1 ? "Using cached data and compiling missing shaders" : "Preparing your native VR session",
                         visible:true,waitingForInput:false)
        }
        // The native bridge can display the initial startup interstitial too.
        if next?.mapName == "startup" { next?.visible = true }
        // Windows keeps its existing shader-idle policy for the trigger hint.
        if next?.waitingForInput == true && !inputReady { next?.messageText = "" }
        var nextStage = next?.mapName == "__shader_preparation" ? nil : Self.read(stagePath, as: Stage.self)
        if let value = nextStage, value.transform.count != 12 || value.settings.count != 11 ||
            !(value.transform + value.settings).allSatisfy(\.isFinite) { nextStage = nil }
        guard next != state || nextStage != stage else { return }
        state = next; stage = nextStage
        lock.lock()
        generation += 1
        // Mailbox text and stage requests can change while assets are uploading.
        // Keep the last complete stereo pair until its replacement is ready.
        if !visible { image = nil }
        lock.unlock()
        // A game may wait for StageOverrideReady before beginning its load.
        if let game {
            let stage = nextStage, epoch = generation
            queue.async { [self] in
                if renderer == nil { renderer = Scene(game: game) }
                // A missing optional room must not deadlock level loading.
                // The map/text fallback remains available when its assets exist.
                _ = renderer?.setStage(stage)
                if let stage, let stagePath {
                    lock.lock(); defer { lock.unlock() }
                    if generation == epoch {
                        try? Data(String(stage.token).utf8).write(to: URL(fileURLWithPath: stagePath + ".ready"), options: .atomic)
                    }
                }
            }
        }
    }
    @discardableResult func draw(into buffer: CVPixelBuffer, tangents: [Float]?, ipd: Double,
                                 head: simd_float4x4 = simd_float4x4(diagonal: SIMD4(1,1,1,1)), timestamp: Double = 0,
                                 now: Double = ProcessInfo.processInfo.systemUptime) -> View? {
        guard visible, let state, let game else { return nil }
        let width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer)
        lock.lock()
        let current = image
        if !pending {
            pending = true
            let epoch = generation
            queue.async { [self] in
                if renderer == nil { renderer = Scene(game: game) }
                let rendered = renderer?.render(state, width: width, height: height, tangents: tangents,
                                                ipd: Float(ipd), head: head, time: now)
                lock.lock(); defer { lock.unlock() }
                if epoch == generation, let rendered {
                    image = View(image: rendered, head: head, timestamp: timestamp, mapName: state.mapName ?? "",
                                 stageModel: renderer?.stageNode == nil ? nil : renderer?.stage?.model)
                }
                pending = false
            }
        }
        lock.unlock()
        guard let current, current.image.width == width, current.image.height == height,
              CVPixelBufferLockBaseAddress(buffer, []) == kCVReturnSuccess else { return nil }
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        guard let context = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: width, height: height, bitsPerComponent: 8,
            bytesPerRow: CVPixelBufferGetBytesPerRow(buffer), space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        context.draw(current.image, in: CGRect(x: 0, y: 0, width: width, height: height))
        return current
    }

    final class Scene {
        let game: URL
        let device: MTLDevice
        let renderer: SCNRenderer
        let scene = SCNScene()
        let camera = SCNNode()
        var geometry: [String: SCNGeometry] = [:]
        var textures: [String: MTLTexture] = [:]
        var stage: Stage?
        var stageNode: SCNNode?
        var mapNode: SCNNode?
        var lastState: State?
        var spinners: [SCNNode] = []
        var loadingLabels: [SCNNode] = []
        var info: [String: MapInfo] = [:]
        var order: [String] = []
        var tertiary: [String] = []
        var cables: [String] = []
        var fonts: [String] = []
        var uiFonts: [String:String] = [:]
        init?(game: URL) {
            guard let device = MTLCreateSystemDefaultDevice() else { return nil }
            self.game = game; self.device = device
            renderer = SCNRenderer(device: device, options: nil)
            renderer.scene = scene; renderer.pointOfView = camera
            camera.camera = SCNCamera(); camera.camera?.automaticallyAdjustsZRange = false
            scene.rootNode.addChildNode(camera)
            scene.background.contents = NSColor.black
            // Read only the authored data tables, without executing the game's JS.
            if let script = asset("webui/interstitials.js", limit: 2 * 1024 * 1024),
               let text = try? String(contentsOf: script, encoding: .utf8) {
                func table(_ field: String, object: Bool = false) -> Data? {
                    let pattern = "\\." + field + "=(" + (object ? "\\{.*?\\}),[A-Za-z_$][A-Za-z_0-9$]*\\.k_" : "\\[[^\\]]*\\]),")
                    guard let re = try? NSRegularExpression(pattern: pattern),
                          let match = re.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
                          let range = Range(match.range(at: 1), in: text) else { return nil }
                    var value = String(text[range])
                    for (pattern, replacement) in [("([,{])([A-Za-z_][A-Za-z_0-9]*):", "$1\"$2\":"),
                        ("!0", "true"), ("!1", "false")] {
                        value = value.replacingOccurrences(of: pattern, with: replacement, options: .regularExpression)
                    }
                    value = value.replacingOccurrences(of: ":.", with: ":0.").replacingOccurrences(of: ":-.", with: ":-0.")
                    return value.data(using: .utf8)
                }
                if let data = table("k_MapInfo", object: true) { info = (try? JSONDecoder().decode([String: MapInfo].self, from: data)) ?? [:] }
                for field in ["k_sGameMaps", "k_sTertiaryMaps", "k_sVaultCables"] {
                    let names = table(field).flatMap { try? JSONDecoder().decode([String].self, from: $0) } ?? []
                    if field == "k_sGameMaps" { order = names }
                    else if field == "k_sTertiaryMaps" { tertiary = names }
                    else { cables = names }
                }
            }
            for weight in ["light","regular","semibold"] {
                if let font=asset("webui/fonts/raju-"+weight+".otf",limit:2*1024*1024) {
                    CTFontManagerRegisterFontsForURL(font as CFURL,.process,nil)
                    if let descriptors=CTFontManagerCreateFontDescriptorsFromURL(font as CFURL) as? [CTFontDescriptor],let first=descriptors.first,
                       let name=CTFontDescriptorCopyAttribute(first,kCTFontNameAttribute) as? String {fonts.append(name);uiFonts[weight]=name}
                }
            }
        }
        func asset(_ relative: String, limit: Int = 64 * 1024 * 1024) -> URL? {
            guard !relative.contains("\\"), !relative.split(separator: "/").contains("..") else { return nil }
            let url = game.appendingPathComponent(relative).resolvingSymlinksInPath()
            guard url.path.hasPrefix(game.path + "/"),
                  let n = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size]) as? NSNumber,
                  (1...limit).contains(n.intValue) else { return nil }
            return url
        }
        func mesh(_ relative: String) -> SCNGeometry? {
            if let known = geometry[relative] { return known }
            guard let url = asset(relative, limit: 8 * 1024 * 1024), let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
            var v: [SCNVector3] = [], n: [SCNVector3] = [], uv: [CGPoint] = []
            var positions: [SCNVector3] = [], normals: [SCNVector3] = [], coords: [CGPoint] = []
            func index(_ value: Substring, _ count: Int) -> Int? {
                guard let i = Int(value), i != 0 else { return nil }
                let at = i > 0 ? i - 1 : count + i
                return (0..<count).contains(at) ? at : nil
            }
            for line in text.split(whereSeparator: \.isNewline) {
                let p = line.split(whereSeparator: \.isWhitespace)
                if (p.first == "v" || p.first == "vn"), p.count >= 4,
                   let x = Float(p[1]), let y = Float(p[2]), let z = Float(p[3]),
                   [x,y,z].allSatisfy({ $0.isFinite && abs($0) < 10000 }) {
                    if p[0] == "v" { v.append(SCNVector3(x,y,z)) } else { n.append(SCNVector3(x,y,z)) }
                } else if p.first == "vt", p.count >= 3, let x = Double(p[1]), let y = Double(p[2]), x.isFinite, y.isFinite {
                    uv.append(CGPoint(x: x, y: y))
                } else if p.first == "f", (4...17).contains(p.count), positions.count < 600000 {
                    let face = p.dropFirst().map { $0.split(separator: "/", omittingEmptySubsequences: false) }
                    for i in 1..<(face.count - 1) {
                        let triangle = [face[0], face[i], face[i+1]]
                        let vi = triangle.compactMap { $0.first.flatMap { index($0, v.count) } }
                        guard vi.count == 3 else { continue }
                        let a = SIMD3<Float>(Float(v[vi[0]].x),Float(v[vi[0]].y),Float(v[vi[0]].z))
                        let b = SIMD3<Float>(Float(v[vi[1]].x),Float(v[vi[1]].y),Float(v[vi[1]].z))
                        let c = SIMD3<Float>(Float(v[vi[2]].x),Float(v[vi[2]].y),Float(v[vi[2]].z))
                        let cross = simd_cross(b-a,c-a), normal = simd_length(cross) > 1e-8 ? simd_normalize(cross) : SIMD3<Float>(0,1,0)
                        for j in 0..<3 {
                            positions.append(v[vi[j]])
                            let f = triangle[j]
                            normals.append(f.count > 2 ? index(f[2], n.count).map { n[$0] } ?? SCNVector3(normal.x,normal.y,normal.z) : SCNVector3(normal.x,normal.y,normal.z))
                            coords.append(f.count > 1 ? index(f[1], uv.count).map { uv[$0] } ?? .zero : .zero)
                        }
                    }
                }
            }
            guard !positions.isEmpty else { return nil }
            let indices = Array(0..<UInt32(positions.count))
            let result = SCNGeometry(sources: [SCNGeometrySource(vertices: positions), SCNGeometrySource(normals: normals), SCNGeometrySource(textureCoordinates: coords)],
                elements: [SCNGeometryElement(indices: indices, primitiveType: .triangles)])
            geometry[relative] = result; return result
        }
        func texture(_ relative: String) -> MTLTexture? {
            if let known = textures[relative] { return known }
            // Shipped dioramas contain 8192-square DDS files (~85 MiB). Map the
            // container and upload a bounded mip, rather than rejecting the room.
            guard let url = asset(relative, limit: 512 * 1024 * 1024),
                  let data = try? Data(contentsOf: url, options: .mappedIfSafe), data.count >= 128 else { return nil }
            func word(_ at: Int) -> UInt32 { data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: at, as: UInt32.self) } }
            guard word(0) == 0x20534444 else { return nil }
            var width = Int(word(16)), height = Int(word(12)), offset = 128, block = 16
            let format: MTLPixelFormat
            switch word(84) {
            case 0x55344342: format = .bc4_rUnorm; block = 8 // BC4U
            case 0x30315844:
                guard data.count >= 148 else { return nil }; offset = 148
                switch word(128) {
                case 71: format = .bc1_rgba; block = 8
                case 72: format = .bc1_rgba_srgb; block = 8
                case 77: format = .bc3_rgba
                case 78: format = .bc3_rgba_srgb
                case 80: format = .bc4_rUnorm; block = 8
                case 98: format = .bc7_rgbaUnorm
                case 99: format = .bc7_rgbaUnorm_srgb
                default: return nil
                }
            default: return nil
            }
            guard (1...16384).contains(width), (1...16384).contains(height) else { return nil }
            var levels = max(1, Int(word(28)))
            guard levels <= 1 + Int(log2(Double(max(width,height)))) else { return nil }
            while max(width,height) > 4096 && levels > 1 {
                offset += ((width+3)/4) * ((height+3)/4) * block
                width = max(1,width/2); height = max(1,height/2); levels -= 1
            }
            var mips: [(offset: Int, width: Int, height: Int, row: Int)] = []
            var w = width, h = height
            for _ in 0..<levels {
                let row = ((w+3)/4) * block, size = row * ((h+3)/4)
                guard offset + size <= data.count else { return nil }
                mips.append((offset,w,h,row)); offset += size
                w = max(1,w/2); h = max(1,h/2)
            }
            let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: width, height: height, mipmapped: levels > 1)
            desc.mipmapLevelCount = levels
            desc.usage = .shaderRead; desc.storageMode = .shared
            guard let result = device.makeTexture(descriptor: desc) else { return nil }
            data.withUnsafeBytes { raw in
                for (level, mip) in mips.enumerated() {
                    result.replace(region: MTLRegionMake2D(0,0,mip.width,mip.height), mipmapLevel: level,
                        withBytes: raw.baseAddress!.advanced(by: mip.offset), bytesPerRow: mip.row)
                }
            }
            textures[relative] = result; return result
        }
        func mapTexture(_ name: String, kind: String) -> MTLTexture? {
            let folder = "models/ui/map_hologram_steamvr/"
            // Several cable models share one material. OBJ/MTL references name
            // PNGs, while the shipped images are DDS files with the same stem.
            if let url = asset(folder + name + ".mtl", limit: 16384), let text = try? String(contentsOf: url, encoding: .utf8) {
                for line in text.split(whereSeparator: \.isNewline) {
                    let p = line.split(whereSeparator: \.isWhitespace)
                    guard p.first == "map_Kd", p.count == 2 else { continue }
                    let file = String(p[1]), stem = (file as NSString).deletingPathExtension
                    guard NativeLoadingMap.validName(stem) else { continue }
                    let target = kind == "ao" ? stem.replacingOccurrences(of: "_wire", with: "_ao") : stem
                    if let result = texture(folder + target + ".dds") { return result }
                }
            }
            return texture(folder + name + "_" + kind + ".dds")
        }
        func material(_ colour: UInt32, opacity: CGFloat = 1, texture: MTLTexture? = nil, additive: Bool = false) -> SCNMaterial {
            let m = SCNMaterial(); m.lightingModel = .constant; m.isDoubleSided = true
            let tint = NSColor(srgbRed: CGFloat((colour >> 16) & 255)/255, green: CGFloat((colour >> 8) & 255)/255,
                               blue: CGFloat(colour & 255)/255, alpha: 1)
            m.diffuse.contents = texture ?? tint as Any
            if texture != nil {
                // DDS rows run downward; OBJ V runs upward. Without this the
                // texture atlas lands on unrelated triangles of the room/map.
                m.diffuse.contentsTransform = SCNMatrix4Scale(SCNMatrix4MakeTranslation(0, 1, 0), 1, -1, 1)
                m.diffuse.mipFilter = .linear
            }
            m.multiply.contents = texture == nil ? NSColor.white : tint
            m.transparency = opacity
            m.blendMode = additive ? .add : .alpha
            m.writesToDepthBuffer = !additive
            return m
        }
        func node(_ geometry: SCNGeometry, material: SCNMaterial, parent: SCNNode) -> SCNNode {
            let copy = geometry.copy() as! SCNGeometry; copy.materials = [material]
            let node = SCNNode(geometry: copy); parent.addChildNode(node); return node
        }
        @discardableResult func setStage(_ next: Stage?) -> Bool {
            if next == stage { return stageNode != nil }
            stageNode?.removeFromParentNode(); stageNode = nil
            geometry = geometry.filter { !$0.key.hasPrefix("models/ui/dioramas/") }
            textures = textures.filter { !$0.key.hasPrefix("models/ui/dioramas/") }
            stage = next
            guard let next else { return false }
            // Native uses an absolute path; the Windows bridge supplies an
            // install-relative model. Both stay confined to this game's assets.
            let model = next.model.replacingOccurrences(of: "\\", with: "/")
            let url = (model.hasPrefix("models/ui/dioramas/") ? game.appendingPathComponent(model)
                : URL(fileURLWithPath: model)).resolvingSymlinksInPath()
            let root = game.appendingPathComponent("models/ui/dioramas").path + "/"
            guard url.path.hasPrefix(root), url.pathExtension == "obj", Self.validStage(next),
                  let mesh = mesh("models/ui/dioramas/" + url.lastPathComponent) else { return false }
            let name = url.deletingPathExtension().lastPathComponent
            let tex = texture("models/ui/dioramas/" + name + ".dds")
            guard tex != nil else { return false }
            let m = material(0xffffff, texture: tex)
            let s = next.settings
            let grey = next.greyscale != 0 || tex?.pixelFormat == .bc4_rUnorm
            m.shaderModifiers = [.surface: """
                float3 colour = _surface.diffuse.rgb;
                \(grey ? (tex?.pixelFormat == .bc4_rUnorm ? "colour = float3(colour.r);" : "colour = float3(dot(colour, float3(0.2126, 0.7152, 0.0722)));") : "")
                float3 primary = float3(\(s[0]), \(s[1]), \(s[2]));
                float3 secondary = float3(\(s[4]), \(s[5]), \(s[6]));
                float fade = 0.0;
                \(s[9] > s[8] ? "float3 world = (scn_frame.inverseViewTransform * float4(_surface.position, 1.0)).xyz; fade = smoothstep(\(s[8]), \(s[9]), length(world));" : "")
                \(s[10] > 0 ? "fade = pow(1.0 - abs(dot(normalize(_surface.normal), normalize(_surface.view))), \(s[10]));" : "")
                _surface.diffuse.rgb = mix(colour * primary, secondary, fade);
                _surface.diffuse.a *= mix(\(s[3]), \(s[7]), fade);
                """]
            m.isDoubleSided = next.cull == 0; m.fillMode = next.wireframe == 0 ? .fill : .lines
            let room = node(mesh, material: m, parent: scene.rootNode)
            let t = next.transform
            room.simdTransform = simd_float4x4(SIMD4(t[0],t[4],t[8],0), SIMD4(t[1],t[5],t[9],0),
                SIMD4(t[2],t[6],t[10],0), SIMD4(t[3],t[7],t[11],1))
            stageNode = room
            guard renderer.prepare(room, shouldAbortBlock: nil) else { room.removeFromParentNode(); stageNode = nil; return false }
            return true
        }
        static func validStage(_ stage: Stage) -> Bool { stage.transform.count == 12 && stage.settings.count == 11 }
        func text(_ value: String, height: CGFloat, parent: SCNNode, position: SCNVector3, left: Bool = false, weight: String = "light", colour: UInt32 = 0xffffff) -> SCNNode {
            let font = CTFontCreateWithName((uiFonts[weight] ?? fonts.first ?? "HelveticaNeue-Light") as CFString, 128, nil)
            let line = CTLineCreateWithAttributedString(NSAttributedString(string: String(value.prefix(256)), attributes: [
                kCTFontAttributeName as NSAttributedString.Key: font,
                kCTForegroundColorAttributeName as NSAttributedString.Key: CGColor(red:CGFloat((colour>>16)&255)/255,green:CGFloat((colour>>8)&255)/255,blue:CGFloat(colour&255)/255,alpha:1)]))
            let bounds = CTLineGetBoundsWithOptions(line, [.useOpticalBounds])
            let w = max(1, Int(ceil(bounds.width + 16))), h = 176
            let context = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w*4,
                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            context.textPosition = CGPoint(x: 8 - bounds.minX, y: 36); CTLineDraw(line, context)
            let plane = SCNPlane(width: height * CGFloat(w)/CGFloat(h), height: height)
            let m = material(0xffffff); m.diffuse.contents = context.makeImage(); m.writesToDepthBuffer = false
            let result = node(plane, material: m, parent: parent); result.position = position; result.renderingOrder = 10
            if left { result.position.x += plane.width/2 }
            return result
        }
        func buildMap(_ state: State) {
            mapNode?.removeFromParentNode(); spinners = []; loadingLabels = []
            let root = SCNNode(); scene.rootNode.addChildNode(root); mapNode = root
            root.eulerAngles.y = CGFloat(state.facingAngle ?? 0) * .pi / 180
            let name = state.mapName ?? "", current = info[name]
            if name == "__shader_preparation" {
                let fraction=Float(state.description ?? "") ?? -1
                _ = text((state.chapter ?? "Preparing shaders").uppercased(),height:0.11,parent:root,position:SCNVector3(0,1.83,-1.7),weight:"semibold",colour:0xffbe55)
                let width:CGFloat=1.05
                let track=node(SCNPlane(width:width,height:0.026),material:material(0x30291f),parent:root)
                track.position=SCNVector3(0,1.57,-1.7)
                let amount=CGFloat(fraction<0 ? 0.12 : max(0,min(1,fraction)))
                let fill=node(SCNPlane(width:max(0.001,width*amount),height:0.026),material:material(0xffbe55),parent:root)
                fill.position=SCNVector3((width*amount-width)/2,1.57,-1.695)
                let label=fraction<0 ? (state.loadingText ?? "Please wait") : "\(Int(fraction*100))%    \(state.loadingText ?? "")"
                _ = text(label,height:0.078,parent:root,position:SCNVector3(0,1.39,-1.7),weight:"regular")
                _ = text(state.messageText ?? "",height:0.044,parent:root,position:SCNVector3(0,1.24,-1.7))
                return
            }

            let visible = current != nil && current?.hidden != true
            var prompt = state.messageText?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            if ["Press Right Hand To Start", "Press Left Hand To Start", "Press Trigger To Start"].contains(prompt) { prompt = "Pull Trigger To Start" }
            if !prompt.isEmpty {
                _ = text(prompt, height: 0.11, parent: root, position: SCNVector3(0,1.5,-1.5))
            } else if !visible {
                _ = text(state.loadingText ?? "LOADING", height: 0.12, parent: root, position: SCNVector3(0,1.8,-2))
            }
            guard visible else { return }
            let map = SCNNode(); root.addChildNode(map)
            map.position = SCNVector3(0,1.1,-1.5); map.eulerAngles.x = .pi / 6; map.scale = SCNVector3(0.8,0.8,0.8)
            let models = SCNNode(); map.addChildNode(models); models.scale = SCNVector3(0.3,0.3,0.3)
            let at = order.firstIndex(of: name) ?? max(0,order.count-1)
            let selectedName = order.indices.contains(at) ? order[at] : name
            let played = Array(order.prefix(at + 1))
            let cableNames = Array(cables.dropFirst(max(0,current?.cables ?? cables.count)))
            var names = tertiary + played + cableNames + ["combine_wall", "cables_citadel"]
            if current?.drawVault == true { names.append("a5_vault") }
            if at < order.count-1 { names.append("a4_c17_parking_garage_precrash") }
            for name in names where NativeLoadingMap.validName(name) {
                guard let g = mesh("models/ui/map_hologram_steamvr/" + name + ".obj") else { continue }
                let selected = name == selectedName, visited = played.contains(name)
                let cable = cableNames.contains(name) || name == "cables_citadel"
                let vault = name == "a5_vault", precrash = name == "a4_c17_parking_garage_precrash"
                let ao = mapTexture(name, kind: "ao")
                let fill = material(selected ? 0xffbe55 : visited || cableNames.contains(name) ? 0x805f2a : 0x553c18,
                    opacity: selected || visited || vault ? 1 : 0.75, texture: ao, additive: true)
                let rimStrength = cable ? 0.1 : selected || visited || vault ? 0.5 : 0.75
                fill.shaderModifiers = [.surface: """
                    \(ao != nil ? "_surface.diffuse.rgb = float3(_surface.diffuse.r);" : "")
                    float rim = pow(clamp(1.0 - abs(dot(normalize(_surface.normal), normalize(_surface.view))), 0.0, 1.0), \(rimStrength));
                    _surface.diffuse.a = clamp(_surface.diffuse.a * (1.0 \(cable ? "+" : "-") rim), 0.0, 1.0);
                    """]
                let mask = material(0); mask.colorBufferWriteMask = []
                // The colour and wire shaders round coincident depths
                // differently. Put only the occlusion shell slightly behind
                // them, preserving the visible mesh and its stereo position.
                mask.shaderModifiers = [.geometry: """
                    float4 viewPosition = scn_node.modelViewTransform * _geometry.position;
                    viewPosition.xyz *= 1.00002;
                    _geometry.position = scn_node.inverseModelViewTransform * viewPosition;
                    """]
                let depth = node(g, material: mask, parent: models); depth.renderingOrder = -2
                let surface = node(g, material: fill, parent: models); surface.renderingOrder = 2
                let wire = material(selected ? 0xffd155 : visited ? 0xcc953d : vault ? 0x805924 : 0x66471d,
                    opacity: selected ? 1 : visited || tertiary.contains(name) || vault || precrash ? 0.75 : name == "cables_citadel" ? 0.15 : 0.25,
                    texture: mapTexture(name, kind: "wire"), additive: true)
                if wire.diffuse.contents is MTLTexture {
                    wire.shaderModifiers = [.surface: "_surface.transparent.a = _surface.diffuse.r; _surface.diffuse.rgb = float3(1.0);"]
                } else { wire.fillMode = .lines }
                let lines = node(g, material: wire, parent: models); lines.renderingOrder = 3
            }
            if let streets = mesh("models/ui/map_hologram_steamvr/map_streets.obj") {
                let m = material(0x664c22, opacity: 0.1, texture: texture("models/ui/map_hologram_steamvr/map_streets_col.dds"), additive: true)
                _ = node(streets, material: m, parent: models)
            }
            // The faint points outside the city are authored quads, not a
            // screen-space pattern: they must keep the map's stereo depth.
            if let grid = mesh("models/ui/map_hologram_steamvr/voxel_cubes.obj") {
                let dots = material(0xffe2a8, opacity: 0.35, texture: mapTexture("voxel_cubes", kind: "ao"), additive: true)
                // The authored lower mips fade these subpixel marks away.
                // Keep their base image; the city retains filtered mip chains.
                dots.diffuse.mipFilter = .none
                dots.shaderModifiers = [.surface: """
                    _surface.diffuse.rgb = float3(_surface.diffuse.r);
                    """]
                let points = node(grid, material: dots, parent: models); points.renderingOrder = 4
                points.name = "loading-map-points"
                let wire = material(0xffbe55, opacity: 0.1, texture: mapTexture("voxel_cubes", kind: "wire"), additive: true)
                wire.diffuse.mipFilter = .none
                if wire.diffuse.contents is MTLTexture {
                    wire.shaderModifiers = [.surface: "_surface.transparent.a = _surface.diffuse.r; _surface.diffuse.rgb = float3(1.0);"]
                } else { wire.fillMode = .lines }
                let lines = node(grid, material: wire, parent: models); lines.renderingOrder = 4
                lines.name = "loading-map-point-wire"
            }
            // The amber panel lies beneath the city, with its far edge elevated
            // in the viewer's field of view just as in the authored interstitial.
            let panel = SCNNode(); map.addChildNode(panel); panel.eulerAngles.x = -.pi/2
            panel.position = SCNVector3(0,0.01,0.25)
            func backdrop(_ colour: UInt32, height: CGFloat, opacity: CGFloat) -> SCNNode {
                let m = material(colour, opacity: opacity, additive: true)
                m.readsFromDepthBuffer = false
                let context = CGContext(data: nil, width: 256, height: 2, bitsPerComponent: 8, bytesPerRow: 1024,
                    space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
                let gradient = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                    colors: [CGColor(gray: 1, alpha: 0), CGColor(gray: 1, alpha: 1), CGColor(gray: 1, alpha: 1), CGColor(gray: 1, alpha: 0)] as CFArray,
                    locations: [0,0.4,0.6,1])!
                context.drawLinearGradient(gradient, start: .zero, end: CGPoint(x: 256,y: 0), options: [])
                m.diffuse.contents = context.makeImage()
                m.multiply.contents = material(colour).diffuse.contents
                let result = node(SCNPlane(width: 8, height: height), material: m, parent: panel)
                result.renderingOrder = 1
                return result
            }
            _ = backdrop(0x553c18, height: 1.8, opacity: 0.2)
            let edge = backdrop(0xffbe55, height: 0.1, opacity: 0.2)
            edge.position.y = 0.9
            if prompt.isEmpty, let p = current?.pos {
                let anchor = SCNNode(); map.addChildNode(anchor); anchor.position = SCNVector3(p.x ?? 0,p.y ?? 0,p.z ?? 0)
                let stem = node(SCNCylinder(radius: 0.0015, height: 0.1), material: material(0xffffff), parent: anchor)
                stem.position.y = 0.05
                let elbow = node(SCNCylinder(radius: 0.0015, height: 0.01), material: material(0xffffff), parent: anchor)
                elbow.position = SCNVector3(0.005,0.1,0); elbow.eulerAngles.z = -.pi/2
                let callout = SCNNode(); anchor.addChildNode(callout)
                callout.position = SCNVector3(0.02,0.1,0); callout.eulerAngles.x = -.pi/3
                for count in 0...3 {
                    let label = text((state.loadingText ?? "LOADING") + String(repeating: ".", count: count),
                        height: 0.08, parent: callout, position: SCNVector3(0.035,0,0), left: true)
                    loadingLabels.append(label)
                }
                for index in 0..<6 {
                    let ink = material(0xffffff); ink.writesToDepthBuffer = false
                    if let url = asset(String(format: "webui/images/arc_%02d.svg", index+1), limit: 16384),
                       let image = NSImage(contentsOf: url) {
                        ink.diffuse.contents = image
                    } else {
                        let context = CGContext(data: nil, width: 96, height: 96, bitsPerComponent: 8, bytesPerRow: 384,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
                        context.setStrokeColor(CGColor(gray: 1, alpha: 1)); context.setLineWidth(4)
                        let start = Double(index % 3) * 2 * .pi / 3
                        context.addArc(center: CGPoint(x: 48,y: 48), radius: index < 3 ? 27 : 39,
                                       startAngle: start, endAngle: start + .pi/2, clockwise: false)
                        context.strokePath(); ink.diffuse.contents = context.makeImage()
                    }
                    let arc = node(SCNPlane(width: 0.075,height: 0.075), material: ink, parent: callout)
                    arc.renderingOrder = 10; spinners.append(arc)
                }
            }
        }
        func render(_ state: State, width: Int, height: Int, tangents: [Float]?, ipd: Float,
                    head: simd_float4x4, time: Double) -> CGImage? {
            guard width > 0, height > 0, width <= 8192, height <= 4096,
                  head.columns.0.x.isFinite, time.isFinite else { return nil }
            if state != lastState { buildMap(state); lastState = state }
            let phase = max(0,time.truncatingRemainder(dividingBy: 2))
            for (index, arc) in spinners.enumerated() {
                let progress = min(1,max(0,(phase - Double(index)*0.1)/0.9))
                arc.eulerAngles.z = CGFloat(-2 * .pi * (1 - pow(1-progress,3)))
            }
            for (index, label) in loadingLabels.enumerated() { label.isHidden = index != Int(phase*2) }
            guard let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                bytesPerRow: width*4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
            for eye in 0..<2 {
                let t = tangents?.count == 8 ? Array(tangents!.dropFirst(eye*4).prefix(4)) : [-1,1,-1,1]
                let l = t[0], r = t[1], b = t[2], u = t[3], near: Float = 0.05, far: Float = 100
                guard t.allSatisfy(\.isFinite), r-l > 0.01, u-b > 0.01 else { return nil }
                camera.camera?.projectionTransform = SCNMatrix4(simd_float4x4(
                    SIMD4(2/(r-l),0,0,0), SIMD4(0,2/(u-b),0,0),
                    SIMD4((r+l)/(r-l),(u+b)/(u-b),-(far+near)/(far-near),-1), SIMD4(0,0,-2*far*near/(far-near),0)))
                var offset = matrix_identity_float4x4; offset.columns.3.x = (eye == 0 ? -0.5 : 0.5)*ipd
                camera.simdTransform = head * offset
                let shot = renderer.snapshot(atTime: time, with: CGSize(width: width/2,height: height), antialiasingMode: .multisampling4X)
                guard let image = shot.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return nil }
                context.draw(image, in: CGRect(x: eye*width/2,y: 0,width: width/2,height: height))
            }
            return context.makeImage()
        }
    }
}


// Narrow C ABI used by the OpenXR bridge. Rendering stays on the scene worker.
private final class LoadingContext {
    let scene: NativeLoadingMap
    var buffer: CVPixelBuffer?
    var width=2048,height=1024
    var lastRequest = -Double.infinity
    var renderedTimestamp = -Double.infinity
    var serial: UInt32 = 0
    init(_ state: String, _ game: String, _ stage: String) {
        scene = NativeLoadingMap(path: state, game: game, stagePath: stage)
        CVPixelBufferCreate(nil,2048,1024,kCVPixelFormatType_32BGRA,
            [kCVPixelBufferCGImageCompatibilityKey: true,kCVPixelBufferCGBitmapContextCompatibilityKey:true] as CFDictionary,&buffer)
    }
}
@_cdecl("alyx_loading_create")
public func loadingCreate(_ state: UnsafePointer<CChar>, _ game: UnsafePointer<CChar>, _ stage: UnsafePointer<CChar>) -> UnsafeMutableRawPointer {
    Unmanaged.passRetained(LoadingContext(String(cString:state),String(cString:game),String(cString:stage))).toOpaque()
}
@_cdecl("alyx_loading_destroy")
public func loadingDestroy(_ value: UnsafeMutableRawPointer) { Unmanaged<LoadingContext>.fromOpaque(value).release() }
@_cdecl("alyx_loading_frame")
public func loadingFrame(_ value: UnsafeMutableRawPointer, _ head: UnsafePointer<Float>, _ tangents: UnsafePointer<Float>,
                        _ ipd: Float, _ timestamp: Double, _ now: Double, _ destination: UnsafeMutablePointer<UInt8>,
                        _ capacity: Int, _ outputHead: UnsafeMutablePointer<Float>, _ outputTime: UnsafeMutablePointer<Double>) -> UInt32 {
    let ctx=Unmanaged<LoadingContext>.fromOpaque(value).takeUnretainedValue()
    ctx.scene.poll(now:now)
    guard ctx.scene.visible, let buffer=ctx.buffer, capacity>=ctx.width*ctx.height*4 else { ctx.serial=0;ctx.renderedTimestamp = -Double.infinity;return 0 }
    guard now-ctx.lastRequest>=1.0/24.0 else { return ctx.serial }
    ctx.lastRequest=now
    let h=simd_float4x4(SIMD4(head[0],head[1],head[2],head[3]),SIMD4(head[4],head[5],head[6],head[7]),
                      SIMD4(head[8],head[9],head[10],head[11]),SIMD4(head[12],head[13],head[14],head[15]))
    guard let view=ctx.scene.draw(into:buffer,tangents:Array(UnsafeBufferPointer(start:tangents,count:8)),ipd:Double(ipd),head:h,timestamp:timestamp,now:now),
          view.timestamp != ctx.renderedTimestamp else { return ctx.serial }
    guard CVPixelBufferLockBaseAddress(buffer,.readOnly)==kCVReturnSuccess else { return ctx.serial }
    defer { CVPixelBufferUnlockBaseAddress(buffer,.readOnly) }
    if let data=CVPixelBufferGetBaseAddress(buffer) {
        let stride=CVPixelBufferGetBytesPerRow(buffer)
        for row in 0..<ctx.height { memcpy(destination+row*ctx.width*4,data+row*stride,ctx.width*4) }
        var matrix=view.head
        _ = withUnsafeBytes(of:&matrix) { memcpy(outputHead,$0.baseAddress!,64) }
        outputTime.pointee=view.timestamp;ctx.renderedTimestamp=view.timestamp
        ctx.serial=ctx.serial==UInt32.max ? 1 : ctx.serial+1
    }
    return ctx.serial
}

@_cdecl("alyx_loading_resize")
public func loadingResize(_ value: UnsafeMutableRawPointer,_ width: Int32,_ height: Int32)->Int32 {
 let ctx=Unmanaged<LoadingContext>.fromOpaque(value).takeUnretainedValue()
 guard width>=128 && width<=8192 && width%2==0 && height>=128 && height<=4096 else{return 0}
 var buffer:CVPixelBuffer?
 guard CVPixelBufferCreate(nil,Int(width),Int(height),kCVPixelFormatType_32BGRA,[kCVPixelBufferCGImageCompatibilityKey:true,kCVPixelBufferCGBitmapContextCompatibilityKey:true] as CFDictionary,&buffer)==kCVReturnSuccess else{return 0}
 ctx.width=Int(width);ctx.height=Int(height);ctx.buffer=buffer;ctx.serial=0;ctx.renderedTimestamp = -Double.infinity
 return 1
}
