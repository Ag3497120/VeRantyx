// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "SteamLauncherKit",
    platforms: [.visionOS(.v1), .macOS(.v14), .iOS(.v17)],
    products: [
        .library(name: "SteamLauncherKit", targets: ["SteamLauncherKit"]),
        .executable(name: "steamctl", targets: ["steamctl"]),
    ],
    targets: [
        .target(
            name: "CSteamSupport",
            exclude: ["zstd/LICENSE"],
            cSettings: [.define("ZSTD_NO_TRACE"), .unsafeFlags(["-w"])],
            linkerSettings: [
                .linkedLibrary("z"),
                .linkedLibrary("crypto", .when(platforms: [.linux])),
            ]
        ),
        .target(name: "SteamLauncherKit", dependencies: ["CSteamSupport"]),
        .executableTarget(name: "steamctl", dependencies: ["SteamLauncherKit"]),
        .testTarget(name: "SteamLauncherKitTests", dependencies: ["SteamLauncherKit"]),
    ]
)
