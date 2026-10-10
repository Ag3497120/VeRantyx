// swift-tools-version:5.9
import PackageDescription

// Opt-in stutter reports from the headset app: which pipelines the game needed and which ones stuttered, with
// shader code replaced by fingerprints. The launcher's consent screens and its upload timer call this library.
let package = Package(
    name: "GloveboxReports",
    platforms: [.visionOS(.v1), .macOS(.v14), .iOS(.v17)],
    products: [
        .library(name: "GloveboxReports", targets: ["GloveboxReports"]),
        .executable(name: "reportctl", targets: ["reportctl"]),
    ],
    targets: [
        .target(name: "GloveboxReports"),
        .executableTarget(name: "reportctl", dependencies: ["GloveboxReports"]),
        .testTarget(name: "GloveboxReportsTests", dependencies: ["GloveboxReports"]),
    ]
)
