// SPDX-License-Identifier: GPL-3.0-or-later
// The mounted volume (plan Step 08 task 7, read side of Step 09).
//
// Every engine call runs on this volume's serial queue; FSKit replies are sent from there
// after the call returns. Items are kept in a table keyed by NTFS file reference, so one
// file is always one FSItem (FSKit maps items to vnodes) until reclaimItem releases it.
import FSKit
import Foundation
import UnlatchCore
import UnlatchEngineC

final class NTFSItem: FSItem, @unchecked Sendable {
    let ino: ue_ino_t
    var parent: ue_ino_t

    init(ino: ue_ino_t, parent: ue_ino_t) {
        self.ino = ino
        self.parent = parent
    }
}

final class NTFSVolume: FSVolume, FSVolume.Handler, FSVolume.ReadWriteHandler, @unchecked Sendable {
    private let engine: OpaquePointer
    private let io: BlockIO                    // kept alive: the engine calls back into it
    private let queue: EngineQueue
    let readOnly: Bool
    let bsdName: String
    private let probe: ue_probe_t
    private var items: [ue_ino_t: NTFSItem] = [:]
    private var root: NTFSItem?
    private var closed = false

    init(engine: OpaquePointer, io: BlockIO, probe: ue_probe_t, readOnly: Bool, bsdName: String) {
        self.engine = engine
        self.io = io
        self.probe = probe
        self.readOnly = readOnly
        self.bsdName = bsdName
        queue = EngineQueue(label: "unlatch.engine.\(bsdName)")
        super.init(volumeID: FSVolume.Identifier(uuid: probe.containerUUID),
                   volumeName: FSFileName(string: probe.labelString.isEmpty ? "NTFS" : probe.labelString))
    }

    // MARK: identity

    /// FSKit file IDs: the root is FSItem.Identifier.rootDirectory (2); every other item uses
    /// its full NTFS reference, (seq << 48) | record, which is never 0, 1 or 2 (D-36).
    private func fileID(_ ino: ue_ino_t) -> FSItem.Identifier {
        ino & 0xFFFF_FFFF_FFFF == UInt64(UE_ROOT_INO) ? .rootDirectory : FSItem.Identifier(ino)
    }

    private func item(for a: ue_attr_t, parent: ue_ino_t) -> NTFSItem {
        if let it = items[a.ino] {
            it.parent = parent
            return it
        }
        let it = NTFSItem(ino: a.ino, parent: parent)
        items[a.ino] = it
        return it
    }

    private func attributes(_ a: ue_attr_t, parent: ue_ino_t) -> FSItem.Attributes {
        let out = FSItem.Attributes()
        out.type = a.type == UInt32(UE_T_DIR) ? .directory : a.type == UInt32(UE_T_LNK) ? .symlink : .file
        var mode = a.mode
        if readOnly {
            mode &= ~0o222
        }
        out.mode = mode
        out.linkCount = a.nlink
        out.uid = 99                           // "unknown": NTFS has no POSIX owners (D-36)
        out.gid = 99
        var flags: UInt32 = 0
        if a.flags & UInt32(UE_F_HIDDEN) != 0 {
            flags |= UInt32(UF_HIDDEN)
        }
        if a.flags & UInt32(UE_F_READONLY) != 0 && a.type != UInt32(UE_T_DIR) {
            flags |= UInt32(UF_IMMUTABLE)
        }
        if a.flags & UInt32(UE_F_COMPRESSED) != 0 {
            flags |= UInt32(UF_COMPRESSED)
        }
        out.flags = flags
        out.size = a.size
        out.allocSize = a.alloc_size
        out.fileID = fileID(a.ino)
        out.parentID = parent == 0 ? .parentOfRoot : fileID(parent)
        out.birthTime = a.birth
        out.modifyTime = a.mtime
        out.changeTime = a.ctime
        out.accessTime = a.atime
        out.inhibitKernelOffloadedIO = true    // no KOIO until Step 15
        return out
    }

    // MARK: engine access

    /// Runs `body` on the volume queue with the engine; replies with EIO after close.
    private func withEngine(_ body: @escaping @Sendable (OpaquePointer) -> Void, failed: @escaping @Sendable (Int32) -> Void) {
        queue.run {
            if self.closed {
                failed(EIO)
                return
            }
            body(self.engine)
        }
    }

    /// Closes the engine (unload). Safe to call twice.
    func close(_ done: @escaping @Sendable (Int32) -> Void) {
        queue.run {
            if self.closed {
                done(0)
                return
            }
            self.closed = true
            self.items.removeAll()
            let err = ue_close(self.engine)
            mountLog.notice("closed \(self.bsdName, privacy: .public): errno \(err)")
            done(err)
        }
    }

    // MARK: FSVolume.PathConfOperations

    var maximumLinkCount: Int { 1024 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { true }
    var truncatesLongNames: Bool { false }
    var maximumFileSizeInBits: Int { 64 }

    // MARK: FSVolume.Handler: volume

    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let c = FSVolume.SupportedCapabilities()
        c.supportsPersistentObjectIDs = true
        c.supports64BitObjectIDs = true
        c.supportsSymbolicLinks = false        // Step 16
        c.supportsHardLinks = false            // Step 16
        c.supportsJournal = false
        c.supportsSparseFiles = true
        c.supportsZeroRuns = true
        c.supportsFastStatFS = true
        c.supports2TBFiles = true
        c.supportsHiddenFiles = true
        c.doesNotSupportSettingFilePermissions = true
        c.caseFormat = .insensitiveCasePreserving
        return c
    }

    var volumeStatistics: FSStatFSResult {
        let s = FSStatFSResult(fileSystemTypeName: "unlatch")
        var total: UInt64 = 0, free: UInt64 = 0
        var cluster: UInt32 = 4096
        _ = queue.sync { ue_statfs(engine, &total, &free, &cluster) }
        s.blockSize = Int(cluster)
        s.ioSize = 1 << 20
        s.totalBytes = total
        s.freeBytes = free
        s.availableBytes = readOnly ? 0 : free
        s.usedBytes = total - free
        s.totalBlocks = total / UInt64(cluster)
        s.freeBlocks = free / UInt64(cluster)
        s.availableBlocks = s.availableBytes / UInt64(cluster)
        s.usedBlocks = s.totalBlocks - s.freeBlocks
        return s
    }

    func activateVolume(options: FSTaskOptions, replyHandler: @escaping (FSActivateResult?, (any Error)?) -> Void) {
        let reply = Unchecked(replyHandler)
        withEngine({ e in
            var a = ue_attr_t()
            let err = ue_getattr(e, ue_ino_t(UE_ROOT_INO), &a)
            if err != 0 {
                reply.value(nil, posixError(err))
                return
            }
            let r = self.item(for: a, parent: 0)
            self.root = r
            reply.value(FSActivateResult(rootItem: r), nil)
        }, failed: { reply.value(nil, posixError($0)) })
    }

    func deactivateVolume(options: FSDeactivateOptions, replyHandler: @escaping ((any Error)?) -> Void) {
        let reply = Unchecked(replyHandler)
        queue.run {
            if !self.closed && !self.readOnly {
                _ = ue_sync(self.engine)
            }
            reply.value(nil)
        }
    }

    func mount(options: FSTaskOptions, replyHandler: @escaping ((any Error)?) -> Void) {
        mountLog.notice("mount \(self.bsdName, privacy: .public)")
        replyHandler(nil)
    }

    func unmount(replyHandler: @escaping () -> Void) {
        let reply = Unchecked(replyHandler)
        queue.run {
            if !self.closed && !self.readOnly {
                _ = ue_sync(self.engine)
            }
            mountLog.notice("unmount \(self.bsdName, privacy: .public)")
            reply.value()
        }
    }

    func synchronize(flags: FSSyncFlags, replyHandler: @escaping ((any Error)?) -> Void) {
        let reply = Unchecked(replyHandler)
        withEngine({ e in
            let err = self.readOnly ? 0 : ue_sync(e)
            reply.value(err == 0 ? nil : posixError(err))
        }, failed: { reply.value(posixError($0)) })
    }

    // MARK: FSVolume.Handler: items

    func lookupItem(named name: FSFileName, in directory: FSItem, context: FSContext,
                    replyHandler: @escaping (FSLookupItemResult?, (any Error)?) -> Void) {
        let reply = Unchecked(replyHandler)
        guard let dir = directory as? NTFSItem, let n = engineName(name) else {
            replyHandler(nil, posixError(ENOENT))
            return
        }
        let fsName = Unchecked(name)
        withEngine({ e in
            var a = ue_attr_t()
            let err = ue_lookup(e, dir.ino, n, &a)
            if err != 0 {
                reply.value(nil, posixError(err))
                return
            }
            let it = self.item(for: a, parent: dir.ino)
            reply.value(FSLookupItemResult(foundItem: it, itemName: fsName.value,
                                           itemAttributes: self.attributes(a, parent: dir.ino)), nil)
        }, failed: { reply.value(nil, posixError($0)) })
    }

    func reclaimItem(_ item: FSItem, replyHandler: @escaping ((any Error)?) -> Void) {
        let reply = Unchecked(replyHandler)
        guard let it = item as? NTFSItem else {
            replyHandler(nil)
            return
        }
        queue.run {
            if self.items[it.ino] === it {
                self.items[it.ino] = nil
            }
            if !self.closed {
                ue_release(self.engine, it.ino)
            }
            reply.value(nil)
        }
    }

    func getAttributes(_ desired: FSItem.GetAttributesRequest, of item: FSItem, context: FSContext,
                       replyHandler: @escaping (FSGetAttributesResult?, (any Error)?) -> Void) {
        let reply = Unchecked(replyHandler)
        guard let it = item as? NTFSItem else {
            replyHandler(nil, posixError(EINVAL))
            return
        }
        withEngine({ e in
            var a = ue_attr_t()
            let err = ue_getattr(e, it.ino, &a)
            if err != 0 {
                reply.value(nil, posixError(err))
                return
            }
            reply.value(FSGetAttributesResult(attributes: self.attributes(a, parent: it.parent)), nil)
        }, failed: { reply.value(nil, posixError($0)) })
    }

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie, verifier: FSDirectoryVerifier,
                            attributes: FSItem.GetAttributesRequest?, packer: FSDirectoryEntryPacker,
                            context: FSContext,
                            replyHandler: @escaping (FSEnumerateDirectoryResult?, (any Error)?) -> Void) {
        let reply = Unchecked(replyHandler)
        guard let dir = directory as? NTFSItem else {
            replyHandler(nil, posixError(ENOTDIR))
            return
        }
        let p = Unchecked(packer)
        let wantAttrs = attributes != nil
        withEngine({ e in
            var start = UInt64(cookie.rawValue)
            // Without attributes FSKit wants "." and ".." (FSVolume.h). Cookies 1 and 2 are the
            // engine's own positions for them, which ue_readdir never returns (D-36).
            if !wantAttrs {
                if start == 0 {
                    if !p.value.packEntry(name: FSFileName(string: "."), itemType: .directory,
                                          itemID: self.fileID(dir.ino), nextCookie: FSDirectoryCookie(rawValue: 1),
                                          attributes: nil) {
                        reply.value(FSEnumerateDirectoryResult(verifier: 1), nil)
                        return
                    }
                    start = 1
                }
                if start == 1 {
                    let up: FSItem.Identifier = dir.parent == 0 ? .parentOfRoot : self.fileID(dir.parent)
                    if !p.value.packEntry(name: FSFileName(string: ".."), itemType: .directory, itemID: up,
                                          nextCookie: FSDirectoryCookie(rawValue: 2), attributes: nil) {
                        reply.value(FSEnumerateDirectoryResult(verifier: 1), nil)
                        return
                    }
                    start = 2
                }
            }
            var ctx = EnumContext(volume: self, packer: p.value, parent: dir.ino, wantAttrs: wantAttrs)
            var v: UInt64 = 0
            let err = withUnsafeMutablePointer(to: &ctx) { cp in
                ue_readdir(e, dir.ino, start, { raw, name, attr, next in
                    let c = raw!.assumingMemoryBound(to: EnumContext.self)
                    return c.pointee.pack(String(cString: name!), attr!.pointee, next)
                }, cp, &v)
            }
            if err != 0 {
                reply.value(nil, posixError(err))
                return
            }
            reply.value(FSEnumerateDirectoryResult(verifier: max(v, 1)), nil)
        }, failed: { reply.value(nil, posixError($0)) })
    }

    fileprivate struct EnumContext {
        let volume: NTFSVolume
        let packer: FSDirectoryEntryPacker
        let parent: ue_ino_t
        let wantAttrs: Bool

        func pack(_ name: String, _ a: ue_attr_t, _ next: UInt64) -> Bool {
            let type: FSItem.ItemType = a.type == UInt32(UE_T_DIR) ? .directory : .file
            return packer.packEntry(name: FSFileName(string: name), itemType: type, itemID: volume.fileID(a.ino),
                                    nextCookie: FSDirectoryCookie(rawValue: next),
                                    attributes: wantAttrs ? volume.attributes(a, parent: parent) : nil)
        }
    }

    // MARK: writes (Step 10): read-only for now

    func createItem(named name: FSFileName, type: FSItem.ItemType, in directory: FSItem,
                    attributes: FSItem.SetAttributesRequest, context: FSContext,
                    replyHandler: @escaping (FSCreateItemResult?, (any Error)?) -> Void) {
        replyHandler(nil, posixError(EROFS))
    }

    func createSymbolicLink(named name: FSFileName, in directory: FSItem, attributes: FSItem.SetAttributesRequest,
                            linkContents: FSFileName, context: FSContext,
                            replyHandler: @escaping (FSCreateSymlinkResult?, (any Error)?) -> Void) {
        replyHandler(nil, posixError(readOnly ? EROFS : ENOTSUP))
    }

    func createLink(to item: FSItem, named name: FSFileName, in directory: FSItem, context: FSContext,
                    replyHandler: @escaping (FSCreateLinkResult?, (any Error)?) -> Void) {
        replyHandler(nil, posixError(readOnly ? EROFS : ENOTSUP))
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem, overItem: FSItem?,
                    context: FSContext, replyHandler: @escaping (FSRenameItemResult?, (any Error)?) -> Void) {
        replyHandler(nil, posixError(EROFS))
    }

    func removeItem(_ item: FSItem, named name: FSFileName, from directory: FSItem, context: FSContext,
                    replyHandler: @escaping (FSRemoveItemResult?, (any Error)?) -> Void) {
        replyHandler(nil, posixError(EROFS))
    }

    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on item: FSItem, context: FSContext,
                       replyHandler: @escaping (FSSetAttributesResult?, (any Error)?) -> Void) {
        replyHandler(nil, posixError(EROFS))
    }

    func readSymbolicLink(_ item: FSItem, context: FSContext,
                          replyHandler: @escaping (FSReadSymlinkResult?, (any Error)?) -> Void) {
        replyHandler(nil, posixError(EINVAL))     // no symlinks until Step 16
    }

    // MARK: FSVolume.ReadWriteHandler

    func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer,
              replyHandler: @escaping (FSReadFileResult?, (any Error)?) -> Void) {
        let reply = Unchecked(replyHandler)
        let buf = Unchecked(buffer)
        guard let it = item as? NTFSItem, offset >= 0 else {
            replyHandler(nil, posixError(EINVAL))
            return
        }
        withEngine({ e in
            var done = 0
            // Straight into FSKit's buffer: no intermediate copy.
            let err = buf.value.withUnsafeMutableBytes { raw -> Int32 in
                let n = min(length, raw.count)
                return ue_read(e, it.ino, UInt64(offset), raw.baseAddress, n, &done)
            }
            var a = ue_attr_t()
            if err == 0 {
                _ = ue_getattr(e, it.ino, &a)
            }
            if err != 0 {
                reply.value(nil, posixError(err))
            } else {
                reply.value(FSReadFileResult(bytesRead: done, itemAttributes: self.attributes(a, parent: it.parent)), nil)
            }
        }, failed: { reply.value(nil, posixError($0)) })
    }

    func write(contents: Data, to item: FSItem, at offset: off_t,
               replyHandler: @escaping (FSWriteFileResult?, (any Error)?) -> Void) {
        replyHandler(nil, posixError(EROFS))       // Step 10
    }
}
