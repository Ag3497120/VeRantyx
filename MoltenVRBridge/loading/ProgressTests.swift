import Foundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers
@main struct ProgressTests {
 static func main() throws {
  let args=CommandLine.arguments;precondition(args.count==3)
  let root=URL(fileURLWithPath:NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString);try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true);defer {try? FileManager.default.removeItem(at:root)}
  let progress=root.appendingPathComponent("progress.json");setenv("ALYX_PREWARM_PROGRESS",progress.path,1)
  let state=root.appendingPathComponent("state"),stage=root.appendingPathComponent("stage")
  let ctx=state.path.withCString{s in args[1].withCString{g in stage.path.withCString{t in loadingCreate(s,g,t)}}};defer{loadingDestroy(ctx)}
  var head:[Float]=[1,0,0,0,0,1,0,0,0,0,1,0,0,1.6,0,1];var fov:[Float]=[-1,1,-1,1,-1,1,-1,1]
  var pixels=[UInt8](repeating:0,count:2048*1024*4);var renderedHead=[Float](repeating:0,count:16);var renderedTime:Double=0
  func draw()->UInt32 {let now=ProcessInfo.processInfo.systemUptime;return loadingFrame(ctx,&head,&fov,0.064,now,now,&pixels,pixels.count,&renderedHead,&renderedTime)}
  func write(_ state:Int,_ done:Int,_ phase:String="preparing") throws {try JSONSerialization.data(withJSONObject:["state":state,"done":done,"total":1200,"phase":phase]).write(to:progress,options:.atomic)}
  func green()->Int {var n=0;for i in stride(from:0,to:pixels.count,by:4){let b=Int(pixels[i]),g=Int(pixels[i+1]),r=Int(pixels[i+2]);if r>g+25&&g>b+25{n+=1}};return n}
  func settle() {let start=ProcessInfo.processInfo.systemUptime;while ProcessInfo.processInfo.systemUptime-start<2 { _=draw();Thread.sleep(forTimeInterval:0.025) }}
  try write(0,0,"checking");settle();precondition(draw()>0);let zero=green()
  try write(1,600);settle();let half=green();precondition(half>zero+100)
  let data=Data(pixels) as CFData;let provider=CGDataProvider(data:data)!
  let image=CGImage(width:2048,height:1024,bitsPerComponent:8,bitsPerPixel:32,bytesPerRow:8192,space:CGColorSpace(name:CGColorSpace.sRGB)!,bitmapInfo:CGBitmapInfo(rawValue:CGImageAlphaInfo.noneSkipFirst.rawValue|CGBitmapInfo.byteOrder32Little.rawValue),provider:provider,decode:nil,shouldInterpolate:false,intent:.defaultIntent)!
  let output=CGImageDestinationCreateWithURL(URL(fileURLWithPath:args[2]) as CFURL,UTType.png.identifier as CFString,1,nil)!;CGImageDestinationAddImage(output,image,nil);precondition(CGImageDestinationFinalize(output))
  try write(1,1200);settle();let full=green();precondition(full>half+100)
  try write(2,1200);Thread.sleep(forTimeInterval:0.15);precondition(draw()==0)
  print("VR progress: real stereo render of cache-check,50%,100%, then automatic hide passed. Orange pixel counts: \(zero),\(half),\(full).")
 }
}
