// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

/// Reads the daemon's status records (`status/<serial>.json`, D-41) and reports changes.
public final class StatusStore: @unchecked Sendable {
    public let directory: URL

    public init(directory: URL? = VolumeStatus.directory()) {
        self.directory = directory ?? URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("unlatch-status")
    }

    private static let decoder: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }()

    /// Every readable record, newest first. Unreadable files are skipped. Timestamps have 1 s
    /// resolution; on a tie a mounted record comes first (it is the volume there now).
    public func all() -> [VolumeStatus] {
        let files = (try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.pathExtension == "json" }
            .compactMap { try? Self.decoder.decode(VolumeStatus.self, from: Data(contentsOf: $0)) }
            .sorted { $0.timestamp != $1.timestamp ? $0.timestamp > $1.timestamp : ($0.mounted && !$1.mounted) }
    }

    /// The newest record for a BSD name (`disk6s1`), or for an image file name.
    public func latest(bsdName: String) -> VolumeStatus? {
        all().first { $0.bsdName == bsdName }
    }

    /// Yields whenever a record is written. Ends when the task is cancelled.
    public func changes() -> AsyncStream<Void> {
        AsyncStream { continuation in
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let fd = open(directory.path, O_EVTONLY)
            guard fd >= 0 else {
                continuation.finish()
                return
            }
            let source = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .rename],
                                                                   queue: .global(qos: .utility))
            source.setEventHandler { continuation.yield() }
            source.setCancelHandler { close(fd) }
            continuation.onTermination = { _ in source.cancel() }
            source.resume()
        }
    }
}
