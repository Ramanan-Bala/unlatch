// SPDX-License-Identifier: GPL-3.0-or-later
// The FSKit unary file system: probe, load, unload, check, format (plan Step 08 tasks 4-6).
//
// The safety decision lives here (review A3): ue_probe runs on the device FSKit hands the
// extension, and the verdict goes to the App Group status record for the app and CLI.
import FSKit
import Foundation
import UnlatchCore
import UnlatchEngineC

final class UnlatchFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations,
    FSManageableResourceMaintenanceOperations, @unchecked Sendable
{
    static let shared = UnlatchFileSystem()

    /// The loaded resource and its probe verdict (a unary file system loads one resource).
    private var loaded: (resource: FSBlockDeviceResource, probe: ue_probe_t, volume: NTFSVolume)?

    // MARK: probe

    static func probe(_ resource: FSBlockDeviceResource) -> (Int32, ue_probe_t) {
        let io = BlockIO(resource: resource, writable: false)
        var p = ue_probe_t()
        var ioCopy = io.io
        let err = EngineQueue.global.sync { ue_probe(&ioCopy, &p) }
        withExtendedLifetime(io) {}
        return (err, p)
    }

    func probeResource(resource: FSResource, replyHandler: @escaping (FSProbeResult?, (any Error)?) -> Void) {
        guard let dev = resource as? FSBlockDeviceResource else {
            replyHandler(.notRecognized, nil)
            return
        }
        let (err, p) = Self.probe(dev)
        if err != 0 {
            engineLog.error("probe \(dev.bsdName, privacy: .public) failed: errno \(err)")
            replyHandler(nil, posixError(err))
            return
        }
        let id = FSContainerIdentifier(uuid: p.containerUUID)
        engineLog.info("probe \(dev.bsdName, privacy: .public): \(String(cString: ue_vol_state_name(p.state)), privacy: .public)")
        switch p.state {
        case UE_VOL_NOT_NTFS:
            replyHandler(.notRecognized, nil)
        case UE_VOL_CORRUPT:
            replyHandler(.recognized(name: p.labelString, containerID: id), nil)
        case UE_VOL_CLEAN:
            replyHandler(.usable(name: p.labelString, containerID: id), nil)
        default:                       // dirty, hibernated, unclean log: read-only only
            replyHandler(.usableButLimited(name: p.labelString, containerID: id), nil)
        }
    }

    // MARK: load / unload

    func loadResource(resource: FSResource, options: FSTaskOptions,
                      replyHandler: @escaping (FSVolume?, (any Error)?) -> Void) {
        guard let dev = resource as? FSBlockDeviceResource else {
            replyHandler(nil, posixError(ENODEV))
            return
        }
        let (perr, p) = Self.probe(dev)
        let state = VolumeStatus.State(p.state)
        if perr != 0 || state == .notNTFS || state == .corrupt {
            let err = posixError(perr != 0 ? perr : EINVAL)
            containerStatus = .blocked(status: err)
            writeStatus(dev, p, readOnly: true, mounted: false)
            replyHandler(nil, err)
            return
        }
        // When in doubt, read-only: anything but CLEAN, a read-only device, or `rdonly`.
        let askedReadOnly = options.taskOptions.contains { $0 == "--rdonly" || $0 == "-r" || $0 == "rdonly" }
        let readOnly = state != .clean || !dev.isWritable || askedReadOnly
        let io = BlockIO(resource: dev, writable: !readOnly)
        var engine: OpaquePointer?
        var info = ue_probe_t()
        var ioCopy = io.io
        let err = EngineQueue.global.sync {
            ue_open(&ioCopy, readOnly ? UInt32(UE_OPEN_RDONLY) : 0, &engine, &info)
        }
        guard err == 0, let engine else {
            engineLog.error("open \(dev.bsdName, privacy: .public) failed: errno \(err)")
            containerStatus = .blocked(status: posixError(err))
            replyHandler(nil, posixError(err))
            return
        }
        let volume = NTFSVolume(engine: engine, io: io, probe: info, readOnly: readOnly, bsdName: dev.bsdName)
        loaded = (dev, p, volume)
        containerStatus = .ready
        writeStatus(dev, p, readOnly: readOnly, mounted: false)
        mountLog.notice("loaded \(dev.bsdName, privacy: .public) \(readOnly ? "read-only" : "read-write", privacy: .public) (\(state.rawValue, privacy: .public))")
        replyHandler(volume, nil)
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions,
                        replyHandler: @escaping ((any Error)?) -> Void) {
        let volume = loaded?.volume
        loaded = nil
        containerStatus = .notReady(status: posixError(ENODEV))
        guard let volume else {
            replyHandler(nil)
            return
        }
        let reply = Unchecked(replyHandler)
        volume.close { err in
            reply.value(err == 0 ? nil : posixError(err))
        }
    }

    // MARK: check / format

    /// The safety probe as FSKit's required check (review A4). It fails only for CORRUPT and
    /// NOT_NTFS; a dirty or hibernated volume passes but was loaded read-only.
    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        guard let (dev, _, _) = loaded else {
            throw posixError(ENODEV)
        }
        let progress = Progress(totalUnitCount: 1)
        let t = Unchecked(task), d = Unchecked(dev)
        DispatchQueue.global(qos: .userInitiated).async {
            let dev = d.value
            let (err, p) = Self.probe(dev)
            let state = VolumeStatus.State(p.state)
            self.writeStatus(dev, p, readOnly: self.loaded?.volume.readOnly ?? true, mounted: false)
            progress.completedUnitCount = 1
            if err != 0 {
                t.value.didComplete(error: posixError(err))
            } else if state == .corrupt || state == .notNTFS {
                t.value.didComplete(error: fs_errorForPOSIXError(EIO))
            } else {
                t.value.didComplete(error: nil)
            }
        }
        return progress
    }

    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        throw posixError(ENOTSUP)
    }

    // MARK: status record

    func writeStatus(_ dev: FSBlockDeviceResource, _ p: ue_probe_t, readOnly: Bool, mounted: Bool) {
        let status = VolumeStatus(bsdName: dev.bsdName, serial: p.serialHex, label: p.labelString,
                                  state: VolumeStatus.State(p.state), reason: p.reasonString,
                                  readOnly: readOnly, mounted: mounted, engineABI: Int(UE_ABI_VERSION))
        do {
            try status.write()
        } catch {
            mountLog.error("cannot write status for \(dev.bsdName, privacy: .public): \(error.localizedDescription, privacy: .public)")
        }
    }
}
