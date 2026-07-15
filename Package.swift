// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "TunHub",
    platforms: [.macOS(.v13)],
    dependencies: [
        .package(url: "https://github.com/weichsel/ZIPFoundation.git", from: "0.9.16")
    ],
    targets: [
        .target(
            name: "TunHubShared",
            path: "Sources/TunHubShared"
        ),
        .executableTarget(
            name: "TunHubApp",
            dependencies: [
                "TunHubShared",
                .product(name: "ZIPFoundation", package: "ZIPFoundation")
            ],
            path: "Sources/TunHubApp"
        ),
        .executableTarget(
            name: "tunhubd",
            dependencies: ["TunHubShared"],
            path: "Sources/tunhubd"
        )
    ],
    swiftLanguageModes: [.v5]
)
