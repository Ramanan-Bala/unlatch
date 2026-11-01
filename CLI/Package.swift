// swift-tools-version: 6.0
// SPDX-License-Identifier: GPL-3.0-or-later
import PackageDescription

let package = Package(
    name: "unlatch",
    platforms: [.macOS("27.0")],
    products: [
        .executable(name: "unlatch", targets: ["unlatch"]),
    ],
    dependencies: [
        .package(path: "../Shared"),
        .package(url: "https://github.com/apple/swift-argument-parser.git", from: "1.5.0"),
    ],
    targets: [
        // The commands live in a library so tests can import them; `unlatch` only runs them.
        .target(
            name: "UnlatchCLI",
            dependencies: [
                .product(name: "UnlatchCore", package: "Shared"),
                .product(name: "ArgumentParser", package: "swift-argument-parser"),
            ]
        ),
        .executableTarget(name: "unlatch", dependencies: ["UnlatchCLI"]),
        .testTarget(name: "UnlatchCLITests", dependencies: ["UnlatchCLI"]),
    ]
)
