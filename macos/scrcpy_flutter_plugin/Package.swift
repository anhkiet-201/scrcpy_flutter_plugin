// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "scrcpy_flutter_plugin",
    platforms: [
        .macOS("10.14")
    ],
    products: [
        .library(
            name: "scrcpy-flutter-plugin",
            targets: ["scrcpy_flutter_plugin"]
        )
    ],
    dependencies: [
        .package(name: "FlutterFramework", path: "../FlutterFramework")
    ],
    targets: [
        .target(
            name: "scrcpy_flutter_plugin",
            dependencies: [
                .product(name: "FlutterFramework", package: "FlutterFramework"),
                "libscrcpy_ffi"
            ],
            resources: [
                .process("Resources")
            ]
        ),
        .binaryTarget(
            name: "libscrcpy_ffi",
            path: "Frameworks/libscrcpy_ffi.xcframework"
        )
    ]
)
