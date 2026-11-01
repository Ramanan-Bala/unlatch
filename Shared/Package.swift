// swift-tools-version: 6.0
// SPDX-License-Identifier: GPL-3.0-or-later
import PackageDescription

let package = Package(
    name: "UnlatchCore",
    platforms: [.macOS("27.0")],
    products: [
        .library(name: "UnlatchCore", targets: ["UnlatchCore"]),
    ],
    targets: [
        .target(name: "UnlatchCore"),
        .testTarget(name: "UnlatchCoreTests", dependencies: ["UnlatchCore"]),
    ]
)
