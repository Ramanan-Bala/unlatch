// SPDX-License-Identifier: GPL-3.0-or-later
// ue_io_t over an FSBlockDeviceResource (plan Step 08 task 2).
//
// Uses only the synchronous read(into:startingAt:length:) and write(from:startingAt:length:)
// and never the metadata-cache APIs, so there is one path to the device. The device wants
// block-aligned transfers but libntfs-3g asks for byte ranges, so an unaligned head or tail
// block goes through a preallocated scratch block; the aligned middle goes straight to the
// caller's buffer. Callbacks allocate nothing. Called only from the volume's engine queue.
import FSKit
import Foundation
import UnlatchEngineC

final class BlockIO: @unchecked Sendable {
    let resource: FSBlockDeviceResource
    let blockSize: Int
    let sizeBytes: UInt64
    private let scratch: UnsafeMutableRawBufferPointer
    private(set) var io = ue_io_t()
    /// Set after the device disappears (ENXIO) or fails; every later transfer fails fast.
    private(set) var failed = false

    init(resource: FSBlockDeviceResource, writable: Bool) {
        self.resource = resource
        blockSize = Int(max(resource.blockSize, 512))
        sizeBytes = resource.blockSize * resource.blockCount
        scratch = UnsafeMutableRawBufferPointer.allocate(byteCount: blockSize, alignment: 4096)
        io.ctx = Unmanaged.passUnretained(self).toOpaque()
        io.pread = blockIORead
        io.pwrite = writable && resource.isWritable ? blockIOWrite : nil
        io.flush = nil                    // the synchronous writes are already on the device
        io.size_bytes = sizeBytes
        io.block_size = UInt32(blockSize)
        io.writable = writable && resource.isWritable
    }

    deinit {
        scratch.deallocate()
    }

    // MARK: device transfers (aligned, looping on short transfers)

    private func deviceRead(_ dst: UnsafeMutableRawPointer, _ len: Int, _ off: UInt64) -> Int32 {
        var done = 0
        while done < len {
            do {
                let n = try resource.read(into: UnsafeMutableRawBufferPointer(start: dst + done, count: len - done),
                                          startingAt: off_t(off) + off_t(done), length: len - done)
                if n <= 0 {
                    return EIO
                }
                done += n
            } catch {
                return fail(errnoValue(of: error), "read", off + UInt64(done), len - done)
            }
        }
        return 0
    }

    private func deviceWrite(_ src: UnsafeRawPointer, _ len: Int, _ off: UInt64) -> Int32 {
        var done = 0
        while done < len {
            do {
                let n = try resource.write(from: UnsafeRawBufferPointer(start: src + done, count: len - done),
                                           startingAt: off_t(off) + off_t(done), length: len - done)
                if n <= 0 {
                    return EIO
                }
                done += n
            } catch {
                return fail(errnoValue(of: error), "write", off + UInt64(done), len - done)
            }
        }
        return 0
    }

    private func fail(_ err: Int32, _ what: StaticString, _ off: UInt64, _ len: Int) -> Int32 {
        if err == ENXIO || err == ENODEV {
            failed = true                 // surprise removal: Step 10 fails the engine from here
        }
        ioLog.error("device \(what, privacy: .public) of \(len) bytes at \(off) failed: errno \(err)")
        return err
    }

    // MARK: ue_io_t callbacks

    func read(_ buf: UnsafeMutableRawPointer, _ len: Int, _ off: UInt64) -> Int32 {
        if failed {
            return EIO
        }
        guard off <= sizeBytes, UInt64(len) <= sizeBytes - off else {
            return EIO
        }
        let bs = UInt64(blockSize)
        var pos = off, out = buf, left = len
        // Unaligned head.
        if pos % bs != 0 {
            let base = pos - pos % bs, skip = Int(pos - base), n = min(left, blockSize - skip)
            let err = deviceRead(scratch.baseAddress!, blockSize, base)
            if err != 0 {
                return err
            }
            out.copyMemory(from: scratch.baseAddress! + skip, byteCount: n)
            pos += UInt64(n); out += n; left -= n
        }
        // Aligned middle, straight into the caller's buffer.
        let middle = left - left % blockSize
        if middle > 0 {
            let err = deviceRead(out, middle, pos)
            if err != 0 {
                return err
            }
            pos += UInt64(middle); out += middle; left -= middle
        }
        // Unaligned tail.
        if left > 0 {
            let err = deviceRead(scratch.baseAddress!, blockSize, pos)
            if err != 0 {
                return err
            }
            out.copyMemory(from: scratch.baseAddress!, byteCount: left)
        }
        return 0
    }

    func write(_ buf: UnsafeRawPointer, _ len: Int, _ off: UInt64) -> Int32 {
        if failed {
            return EIO
        }
        guard off <= sizeBytes, UInt64(len) <= sizeBytes - off else {
            return EIO
        }
        let bs = UInt64(blockSize)
        var pos = off, src = buf, left = len
        // Unaligned head: read-modify-write one block.
        if pos % bs != 0 {
            let base = pos - pos % bs, skip = Int(pos - base), n = min(left, blockSize - skip)
            var err = deviceRead(scratch.baseAddress!, blockSize, base)
            if err != 0 {
                return err
            }
            (scratch.baseAddress! + skip).copyMemory(from: src, byteCount: n)
            err = deviceWrite(scratch.baseAddress!, blockSize, base)
            if err != 0 {
                return err
            }
            pos += UInt64(n); src += n; left -= n
        }
        let middle = left - left % blockSize
        if middle > 0 {
            let err = deviceWrite(src, middle, pos)
            if err != 0 {
                return err
            }
            pos += UInt64(middle); src += middle; left -= middle
        }
        if left > 0 {
            var err = deviceRead(scratch.baseAddress!, blockSize, pos)
            if err != 0 {
                return err
            }
            scratch.baseAddress!.copyMemory(from: src, byteCount: left)
            err = deviceWrite(scratch.baseAddress!, blockSize, pos)
            if err != 0 {
                return err
            }
        }
        return 0
    }
}

// C callbacks for ue_io_t; ctx is an unretained BlockIO.
private func blockIORead(_ ctx: UnsafeMutableRawPointer?, _ buf: UnsafeMutableRawPointer?, _ len: Int,
                         _ off: UInt64) -> Int32 {
    Unmanaged<BlockIO>.fromOpaque(ctx!).takeUnretainedValue().read(buf!, len, off)
}

private func blockIOWrite(_ ctx: UnsafeMutableRawPointer?, _ buf: UnsafeRawPointer?, _ len: Int,
                          _ off: UInt64) -> Int32 {
    Unmanaged<BlockIO>.fromOpaque(ctx!).takeUnretainedValue().write(buf!, len, off)
}
