//
//  BlockDeviceIO.swift
//  NTFSExtension
//
//  Adapts an FSKit `FSBlockDeviceResource` to the bridge's `ntfsb_io`
//  callback table, so that libntfs-3g / mkntfs perform all sector IO through
//  the device handle fskitd opened for us (the sandboxed extension never
//  opens /dev nodes itself).
//
//  IO path
//  -------
//  libntfs-3g caches nothing below its own inode/attribute layer and needs
//  *all* sectors (metadata and file data alike), so we use FSKit's direct IO
//  (`read(into:startingAt:length:)` / `write(from:startingAt:length:)`)
//  exclusively and never the buffer-cache ("metadata") variants. Apple warns
//  against mixing the two on the same range; using only one avoids that.
//
//  The bridge guarantees every request is aligned to `block_size` in both
//  offset and length (it bounces unaligned IO internally), so no alignment
//  handling is needed here.
//

import Foundation
import FSKit
import os

private let ioLog = Logger(subsystem: "com.vahidzekic.ntfs4mac.NTFSExtension", category: "BlockDeviceIO")

/// Wraps an `FSBlockDeviceResource` and exposes it as an `ntfsb_io`.
///
/// `@unchecked Sendable`: all stored properties are immutable after `init`,
/// and `FSBlockDeviceResource`'s synchronous IO methods are safe to call from
/// any thread (they are thin wrappers over pread/pwrite on fskitd's fd).
final class BlockDeviceIO: @unchecked Sendable {
    /// The underlying FSKit resource.
    let resource: FSBlockDeviceResource
    /// True when writes are refused (requested, or the resource isn't writable).
    let readOnly: Bool
    /// Logical block size the device accepts (bytes).
    let blockSize: UInt32
    /// Total device size in bytes.
    let sizeBytes: UInt64

    /// Upper bound for a single resource call. Larger bridge requests are
    /// split; the bound is a multiple of every sane block size (512 … 64 KiB).
    private static let maxChunk = 8 * 1024 * 1024

    /// - Parameters:
    ///   - resource: The block device FSKit handed to probe/load.
    ///   - readOnly: Request read-only access. Forced to `true` when the
    ///     resource reports `isWritable == false`.
    init(resource: FSBlockDeviceResource, readOnly: Bool) {
        self.resource = resource
        self.readOnly = readOnly || !resource.isWritable
        let logical = resource.blockSize
        // FSKit reports 512 or 4096 in practice; clamp defensively so a bogus
        // value can never yield a zero/oversized block size in the C struct.
        let clamped = (logical >= 512 && logical <= 65536) ? logical : 512
        self.blockSize = UInt32(clamped)
        let (bytes, overflow) = resource.blockCount.multipliedReportingOverflow(by: logical)
        self.sizeBytes = overflow ? UInt64.max : bytes
    }

    /// The C callback table. `ctx` is an *unretained* pointer to `self`; the
    /// owner (`NTFSMount`, or the caller of `NTFSBridge.probe/format`) keeps
    /// this object alive for as long as the bridge may call back.
    var io: ntfsb_io {
        var table = ntfsb_io()
        table.ctx = Unmanaged.passUnretained(self).toOpaque()
        table.pread = { ctx, buffer, count, offset -> Int64 in
            guard let ctx, let buffer else { return -Int64(EINVAL) }
            let device = Unmanaged<BlockDeviceIO>.fromOpaque(ctx).takeUnretainedValue()
            return device.readBlocks(into: buffer, count: count, offset: offset)
        }
        if readOnly {
            table.pwrite = nil
        } else {
            table.pwrite = { ctx, buffer, count, offset -> Int64 in
                guard let ctx, let buffer else { return -Int64(EINVAL) }
                let device = Unmanaged<BlockDeviceIO>.fromOpaque(ctx).takeUnretainedValue()
                return device.writeBlocks(from: buffer, count: count, offset: offset)
            }
        }
        table.sync = { ctx -> Int32 in
            guard let ctx else { return EINVAL }
            let device = Unmanaged<BlockDeviceIO>.fromOpaque(ctx).takeUnretainedValue()
            return device.synchronize()
        }
        table.size_bytes = sizeBytes
        table.block_size = blockSize
        table.read_only = readOnly
        return table
    }

    // MARK: - Callback implementations

    /// Reads `count` bytes at `offset`. Returns bytes read (short only at the
    /// end of the device) or a negative errno.
    fileprivate func readBlocks(into buffer: UnsafeMutableRawPointer, count: UInt64, offset: Int64) -> Int64 {
        guard offset >= 0, count <= UInt64(Int.max) else { return -Int64(EINVAL) }
        let start = UInt64(offset)
        guard count > 0, start < sizeBytes else { return 0 }
        let wanted = Int(min(count, sizeBytes - start))
        var done = 0
        while done < wanted {
            let chunk = min(wanted - done, Self.maxChunk)
            do {
                let target = UnsafeMutableRawBufferPointer(start: buffer + done, count: chunk)
                let got = try resource.read(into: target,
                                            startingAt: off_t(offset) + off_t(done),
                                            length: chunk)
                if got <= 0 {
                    break // end of device
                }
                done += got
            } catch {
                let code = NTFSError.errnoValue(from: error)
                ioLog.error("read failed at \(offset + Int64(done), privacy: .public) (+\(chunk, privacy: .public)): \(error.localizedDescription, privacy: .public)")
                return -Int64(code)
            }
        }
        return Int64(done)
    }

    /// Writes `count` bytes at `offset`. Returns bytes written or a negative errno.
    fileprivate func writeBlocks(from buffer: UnsafeRawPointer, count: UInt64, offset: Int64) -> Int64 {
        guard !readOnly else { return -Int64(EROFS) }
        guard offset >= 0, count <= UInt64(Int.max) else { return -Int64(EINVAL) }
        let start = UInt64(offset)
        guard count > 0 else { return 0 }
        guard start < sizeBytes else { return -Int64(ENOSPC) }
        let wanted = Int(min(count, sizeBytes - start))
        var done = 0
        while done < wanted {
            let chunk = min(wanted - done, Self.maxChunk)
            do {
                let source = UnsafeRawBufferPointer(start: buffer + done, count: chunk)
                let put = try resource.write(from: source,
                                             startingAt: off_t(offset) + off_t(done),
                                             length: chunk)
                if put <= 0 {
                    ioLog.error("short write at \(offset + Int64(done), privacy: .public)")
                    return done > 0 ? Int64(done) : -Int64(EIO)
                }
                done += put
            } catch {
                let code = NTFSError.errnoValue(from: error)
                ioLog.error("write failed at \(offset + Int64(done), privacy: .public) (+\(chunk, privacy: .public)): \(error.localizedDescription, privacy: .public)")
                return -Int64(code)
            }
        }
        return Int64(done)
    }

    /// Flushes the resource. All IO here goes through the direct
    /// `read(into:)` / `write(from:)` calls, which bypass FSKit's metadata
    /// cache, so there is nothing of ours for `metadataFlush()` to push.
    /// A flush error is therefore logged but not reported as a failure:
    /// treating it as fatal made mkntfs fail its final "Syncing device" step
    /// (EIO from newfs_fskit) after the volume had been written completely.
    /// Always returns 0.
    fileprivate func synchronize() -> Int32 {
        guard !readOnly else { return 0 }
        do {
            try resource.metadataFlush()
        } catch {
            ioLog.error("device flush failed (ignored, direct IO is uncached): \(error.localizedDescription, privacy: .public)")
        }
        return 0
    }
}
