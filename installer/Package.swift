// swift-tools-version:5.9
import PackageDescription

// GloveboxKit is everything in the Mac installer that isn't drawing: the engine contract, running
// the engine, recipes, the setup order and diagnostics. It builds on Linux too, so it's tested
// without a Mac. The SwiftUI app lives in App/ (XcodeGen, see App/project.yml).
let package = Package(
    name: "Glovebox",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "GloveboxKit", targets: ["GloveboxKit"]),
    ],
    targets: [
        .target(name: "GloveboxKit"),
        // The tests drive FakeEngine/glovebox, a script that speaks the contract.
        .testTarget(name: "GloveboxKitTests", dependencies: ["GloveboxKit"]),
    ]
)
