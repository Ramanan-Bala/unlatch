// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// Why a mount, unmount or eject failed, in words a user can act on.
public enum MountError: Error, Equatable, Sendable, LocalizedError {
    case notFound(String)
    case notNTFS(String)
    case macFUSEMissing
    case needsHelper(String)
    case busy(String)
    case refused(String)
    case daemonFailed(String)
    case unmountFailed(String, Int32)
    case ejectFailed(String, String)

    public var errorDescription: String? {
        switch self {
        case .notFound(let d): "No disk named \(d)."
        case .notNTFS(let d): "\(d) is not an NTFS volume."
        case .macFUSEMissing:
            "macFUSE is not installed. Install it (brew install --cask macfuse) and allow its system extension."
        case .needsHelper(let d):
            "Opening \(d) needs administrator rights. Install the Unlatch helper once (unlatch install-helper)."
        case .busy(let d): "\(d) is in use; close the files on it and try again."
        case .refused(let why): why
        case .daemonFailed(let why): "Mounting failed: \(why)"
        case .unmountFailed(let p, let e): "Could not unmount \(p): \(String(cString: strerror(e)))."
        case .ejectFailed(let d, let why): "Could not eject \(d): \(why)"
        }
    }
}
