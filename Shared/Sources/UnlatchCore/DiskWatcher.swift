// SPDX-License-Identifier: GPL-3.0-or-later
import DiskArbitration
import Foundation

/// Watches DiskArbitration for NTFS volumes (plan Step 12 task 1).
///
/// With `claim`, it also answers DiskArbitration's mount approval: a volume the closure
/// claims is refused to every other mounter, so Apple's NTFS driver does not mount it first
/// and Unlatch can (spike S2, D-56).
public final class DiskWatcher: @unchecked Sendable {
    public enum Event: Sendable, Equatable {
        case appeared(DiskInfo)
        case disappeared(String)
        case changed(DiskInfo)
    }

    private let session: DASession
    private let queue = DispatchQueue(label: "\(Unlatch.bundlePrefix).unlatch.diskwatcher")
    private static let onQueue = DispatchSpecificKey<Bool>()
    private let claim: (@Sendable (DiskInfo) -> Bool)?
    private var continuation: AsyncStream<Event>.Continuation?
    private var context: UnsafeMutableRawPointer?
    private var known: Set<String> = []

    /// Fails when DiskArbitration has no session for this process.
    public init?(claim: (@Sendable (DiskInfo) -> Bool)? = nil) {
        guard let s = DASessionCreate(kCFAllocatorDefault) else { return nil }
        session = s
        self.claim = claim
        queue.setSpecific(key: Self.onQueue, value: true)
    }

    deinit { stop() }

    /// Starts watching. The current NTFS volumes arrive first as `.appeared`.
    public func events() -> AsyncStream<Event> {
        let (stream, cont) = AsyncStream<Event>.makeStream()
        queue.sync {
            continuation = cont
            let ctx = Unmanaged.passRetained(self).toOpaque()
            context = ctx
            DARegisterDiskAppearedCallback(session, nil, diskAppeared, ctx)
            DARegisterDiskDisappearedCallback(session, nil, diskDisappeared, ctx)
            DARegisterDiskDescriptionChangedCallback(session, nil, nil, diskChanged, ctx)
            if claim != nil {
                DARegisterDiskMountApprovalCallback(session, nil, mountApproval, ctx)
            }
            DASessionSetDispatchQueue(session, queue)
        }
        cont.onTermination = { [weak self] _ in self?.stop() }
        return stream
    }

    /// Unregisters every callback and ends the stream. Safe to call twice, and from the
    /// stream's own termination (which runs on the watcher's queue).
    public func stop() {
        if DispatchQueue.getSpecific(key: Self.onQueue) == true {
            stopOnQueue()
        } else {
            queue.sync { stopOnQueue() }
        }
    }

    private func stopOnQueue() {
        do {
            guard let ctx = context else { return }
            context = nil
            DASessionSetDispatchQueue(session, nil)
            let cb: UnsafeMutableRawPointer = unsafeBitCast(diskAppeared, to: UnsafeMutableRawPointer.self)
            DAUnregisterCallback(session, cb, ctx)
            DAUnregisterCallback(session, unsafeBitCast(diskDisappeared, to: UnsafeMutableRawPointer.self), ctx)
            DAUnregisterCallback(session, unsafeBitCast(diskChanged, to: UnsafeMutableRawPointer.self), ctx)
            if claim != nil {
                DAUnregisterCallback(session, unsafeBitCast(mountApproval, to: UnsafeMutableRawPointer.self), ctx)
            }
            let c = continuation
            continuation = nil
            c?.finish()
            Unmanaged<DiskWatcher>.fromOpaque(ctx).release()
        }
    }

    // Called on `queue`.
    fileprivate func handle(appeared disk: DADisk) {
        guard let info = DA.info(disk, mounts: Mount.table()) else { return }
        known.insert(info.bsdName)
        continuation?.yield(.appeared(info))
    }

    fileprivate func handle(disappeared disk: DADisk) {
        guard let name = DADiskGetBSDName(disk).map({ String(cString: $0) }), known.remove(name) != nil else { return }
        continuation?.yield(.disappeared(name))
    }

    fileprivate func handle(changed disk: DADisk) {
        guard let info = DA.info(disk, mounts: Mount.table()) else { return }
        continuation?.yield(known.insert(info.bsdName).inserted ? .appeared(info) : .changed(info))
    }

    fileprivate func approve(_ disk: DADisk) -> Bool {
        guard let claim, let info = DA.info(disk, mounts: []) else { return true }
        return !claim(info)
    }
}

private func watcher(_ ctx: UnsafeMutableRawPointer?) -> DiskWatcher {
    Unmanaged<DiskWatcher>.fromOpaque(ctx!).takeUnretainedValue()
}

private let diskAppeared: DADiskAppearedCallback = { disk, ctx in watcher(ctx).handle(appeared: disk) }
private let diskDisappeared: DADiskDisappearedCallback = { disk, ctx in watcher(ctx).handle(disappeared: disk) }
private let diskChanged: DADiskDescriptionChangedCallback = { disk, _, ctx in watcher(ctx).handle(changed: disk) }
private let mountApproval: DADiskMountApprovalCallback = { disk, ctx in
    if watcher(ctx).approve(disk) {
        return nil
    }
    let dissenter = DADissenterCreate(kCFAllocatorDefault, DAReturn(kDAReturnExclusiveAccess),
                                      "Unlatch mounts this NTFS volume" as CFString)
    return Unmanaged.passRetained(dissenter)
}
