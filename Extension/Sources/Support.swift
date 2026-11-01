// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers shared by the extension: errno mapping, name normalization, Sendable boxes.
import FSKit
import Foundation
import os
import UnlatchCore
import UnlatchEngineC

let engineLog = Unlatch.logger("engine")
let ioLog = Unlatch.logger("io")
let mountLog = Unlatch.logger("mount")

/// The single errno -> Error mapping (plan Step 08 task 9).
func posixError(_ code: Int32) -> Error {
    fs_errorForPOSIXError(code)
}

/// Maps a thrown error back to errno, for the C engine's I/O callbacks.
func errnoValue(of error: Error) -> Int32 {
    let ns = error as NSError
    if ns.domain == NSPOSIXErrorDomain {
        return Int32(ns.code)
    }
    return EIO
}

/// Every name from FSKit goes through here: NTFS stores what Windows writes (NFC), and the
/// engine compares without normalizing (D-22). Returns nil for names that are not UTF-8.
func engineName(_ name: FSFileName) -> String? {
    guard let s = name.string else {
        return nil
    }
    return s.precomposedStringWithCanonicalMapping
}

/// Lets FSKit reply handlers (not marked @Sendable in the SDK) cross onto the engine queue.
/// Every reply handler is still called exactly once, from that queue.
struct Unchecked<T>: @unchecked Sendable {
    let value: T
    init(_ value: T) { self.value = value }
}

/// Serial queue for one engine instance. The engine is not thread-safe (seam contract), and
/// libntfs-3g has globals, so probe and open across volumes share `global`.
final class EngineQueue: @unchecked Sendable {
    static let global = EngineQueue(label: "unlatch.engine.global")

    let queue: DispatchQueue

    init(label: String) {
        queue = DispatchQueue(label: label, qos: .userInitiated)
    }

    /// Runs `work` on the queue. FSKit replies are sent from inside `work`, after the engine
    /// call returns, never from the caller's thread.
    func run(_ work: @escaping @Sendable () -> Void) {
        queue.async(execute: work)
    }

    func sync<T>(_ work: () -> T) -> T {
        queue.sync(execute: work)
    }
}

extension VolumeStatus.State {
    init(_ s: ue_vol_state_t) {
        switch s {
        case UE_VOL_CLEAN: self = .clean
        case UE_VOL_DIRTY: self = .dirty
        case UE_VOL_HIBERNATED: self = .hibernated
        case UE_VOL_UNCLEAN_LOG: self = .uncleanLog
        case UE_VOL_CORRUPT: self = .corrupt
        default: self = .notNTFS
        }
    }
}

extension ue_probe_t {
    var labelString: String {
        withUnsafeBytes(of: label) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
    }

    var reasonString: String {
        withUnsafeBytes(of: reason) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
    }

    var serialHex: String {
        String(format: "%016llX", serial)
    }

    /// A stable container UUID from the NTFS serial number: fixed prefix + serial (D-36).
    var containerUUID: UUID {
        var b = [UInt8](repeating: 0, count: 16)
        let prefix: [UInt8] = [0x55, 0x4E, 0x4C, 0x54, 0x4E, 0x54, 0x46, 0x53]   // "UNLTNTFS"
        for i in 0..<8 {
            b[i] = prefix[i]
            b[8 + i] = UInt8(truncatingIfNeeded: serial >> (8 * UInt64(7 - i)))
        }
        b[6] = (b[6] & 0x0F) | 0x80        // version 8 (vendor-defined)
        b[8] = (b[8] & 0x3F) | 0x80        // RFC 9562 variant
        return UUID(uuid: (b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                           b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]))
    }
}
