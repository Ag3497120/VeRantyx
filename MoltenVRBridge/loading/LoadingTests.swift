import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
import simd

@main struct LoadingTests {
 static func main() throws {
  let args=CommandLine.arguments;guard args.count==3 else {fatalError("game hlvr directory and output PNG required")}
  let game=URL(fileURLWithPath:args[1]);let root=URL(fileURLWithPath:NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
  try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
  defer {try? FileManager.default.removeItem(at:root)}
  let state=root.appendingPathComponent("loading.json"),stage=root.appendingPathComponent("stage.json")
  try JSONSerialization.data(withJSONObject:["type":"begin_loading","mapName":"a2_quarantine_entrance","chapter":"HALF-LIFE: ALYX","loadingText":"LOADING","visible":true]).write(to:state)
  let model=game.appendingPathComponent("models/ui/dioramas/startup.obj").path
  try JSONSerialization.data(withJSONObject:["model":model,"transform":[1,0,0,0,0,1,0,0,0,0,1,0],"settings":[1,1,1,1,0,0,0,1,0.5,1,1],"cull":0,"greyscale":0,"wireframe":0,"token":17]).write(to:stage)
  let ctx=state.path.withCString { s in game.path.withCString { g in stage.path.withCString { t in loadingCreate(s,g,t) } } }
  defer {loadingDestroy(ctx)}
  var head:[Float]=[1,0,0,0,0,1,0,0,0,0,1,0,0,1.65,0,1]
  var fov:[Float]=[-1,1,-1,1,-1,1,-1,1];var pixels=[UInt8](repeating:0,count:2048*1024*4);var renderedHead=[Float](repeating:0,count:16);var timestamp:Double=0
  func draw() -> UInt32 {
   let now=ProcessInfo.processInfo.systemUptime
   return loadingFrame(ctx,&head,&fov,0.064,now,now,&pixels,pixels.count,&renderedHead,&timestamp)
  }
  let start=ProcessInfo.processInfo.systemUptime;var serial:UInt32=0
  while serial==0 && ProcessInfo.processInfo.systemUptime-start<90 {serial=draw();Thread.sleep(forTimeInterval:0.025)}
  precondition(serial>0,"No actual loading scene rendered")
  let lit=pixels.enumerated().filter {$0.offset%4<3 && $0.element>12}.count
  precondition(lit>1000,"Loading scene is empty")
  var orange=0
  for i in stride(from:0,to:pixels.count,by:4) {
   let b=Int(pixels[i]),g=Int(pixels[i+1]),r=Int(pixels[i+2])
   if r>90 && r>g+12 && g>b+12 {orange+=1}
  }
  precondition(orange>100,"Authored holographic map did not render")
  let ready=stage.path+".ready";precondition((try? String(contentsOfFile:ready))=="17","Stage-ready handshake failed")
  precondition(abs(renderedHead[13]-1.65)<0.0001 && timestamp>0)
  let colour=CGColorSpace(name:CGColorSpace.sRGB)!
  let data=Data(pixels) as CFData;let provider=CGDataProvider(data:data)!
  let image=CGImage(width:2048,height:1024,bitsPerComponent:8,bitsPerPixel:32,bytesPerRow:2048*4,space:colour,bitmapInfo:CGBitmapInfo(rawValue:CGImageAlphaInfo.noneSkipFirst.rawValue|CGBitmapInfo.byteOrder32Little.rawValue),provider:provider,decode:nil,shouldInterpolate:false,intent:.defaultIntent)!
  let out=CGImageDestinationCreateWithURL(URL(fileURLWithPath:args[2]) as CFURL,UTType.png.identifier as CFString,1,nil)!;CGImageDestinationAddImage(out,image,nil);precondition(CGImageDestinationFinalize(out))
  try FileManager.default.removeItem(at:state);Thread.sleep(forTimeInterval:0.12);precondition(draw()==0,"Completed load did not disappear")
  try Data("{broken".utf8).write(to:state);Thread.sleep(forTimeInterval:0.12);precondition(draw()==0)
  precondition(!NativeLoadingMap.validName("../escape"))
  print("Real loading map/diorama: rendered \(lit) nonblack channels, stereo pose metadata and stage handshake valid; clear/corrupt-state recovery passed.")
 }
}
