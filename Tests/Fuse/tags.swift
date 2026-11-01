// SPDX-License-Identifier: GPL-3.0-or-later
// Finder-tag round trip (Step 16): tags <src> <dst> copy tags src Red and Work, copies it
// with FileManager (copyfile, as Finder uses) and prints dst's tags; without "copy" it only prints.
import Foundation
let args = CommandLine.arguments
let src = URL(fileURLWithPath: args[1]), dst = URL(fileURLWithPath: args[2])
if args.count > 3 {     // copy mode: tag src, copy to dst
    try? FileManager.default.removeItem(at: src)
    try "tagged content\n".write(to: src, atomically: false, encoding: .utf8)
    var v = URLResourceValues(); v.tagNames = ["Red", "Work"]
    var s = src; try s.setResourceValues(v)
    try? FileManager.default.removeItem(at: dst)
    try FileManager.default.copyItem(at: src, to: dst)
}
let tags = try dst.resourceValues(forKeys: [.tagNamesKey]).tagNames ?? []
print("tags on \(dst.lastPathComponent):", tags)
