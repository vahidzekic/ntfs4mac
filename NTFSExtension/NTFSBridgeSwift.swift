//
//  NTFSBridgeSwift.swift
//  NTFSExtension
//
//  Swift-facing wrappers over the frozen C contract in Bridge/NTFSBridge.h.
//  This file implements exactly the "Swift wrapper API" section of
//  docs/ARCHITECTURE.md; NTFSVolume and NTFSFileSystem only ever talk to
//  libntfs-3g through the types declared here.
//
//  Conventions
//  -----------
//  * Every C call returns 0 or a positive errno. `NTFSError.check(_:)` turns
//    a non-zero value into a thrown `POSIXError`, which FSKit reply handlers
//    accept directly (it bridges to an NSError in NSPOSIXErrorDomain, the same
//    thing `fs_errorForPOSIXError` produces).
//  * The bridge serialises every call per volume, so this layer adds only the
//    locking needed to protect its *own* state (the `ntfsb_volume *` handle).
//  * Strings crossing the boundary are NUL-terminated UTF-8.
//
//  libntfs-3g is GPL-2.0-or-later; see docs/ARCHITECTURE.md ("Licensing").
//

import Foundation

// MARK: - Small C-interop helpers

/// Converts any imported C integer constant (anonymous-enum members import as
/// `Int`, typed ones as `UInt32`/`Int32`) to the `uint32_t` the bridge expects.
@inline(__always)
private func cFlag<T: BinaryInteger>(_ value: T) -> UInt32 {
    UInt32(truncatingIfNeeded: value)
}

/// The root directory's MFT record number (`NTFSB_ROOT_INO`, 5).
let ntfsRootIno: UInt64 = UInt64(NTFSB_ROOT_INO)

/// Decodes a fixed-size C `char[N]` array (imported by Swift as an N-tuple of
/// `CChar`) into a `String`, stopping at the first NUL. The bytes are never
/// read past the tuple's size, so a missing terminator is harmless. Invalid
/// UTF-8 sequences are replaced with U+FFFD rather than failing.
func stringFromCCharArray<Tuple>(_ tuple: Tuple) -> String {
    withUnsafeBytes(of: tuple) { raw -> String in
        let end = raw.firstIndex(of: 0) ?? raw.endIndex
        return String(decoding: raw[raw.startIndex..<end], as: UTF8.self)
    }
}

/// Throws `EINVAL` for strings that cannot be represented as a C string
/// (embedded NUL), which would otherwise be silently truncated by `withCString`.
@inline(__always)
private func validateCString(_ string: String) throws {
    if string.utf8.contains(0) {
        throw NTFSError.posix(EINVAL)
    }
}

// MARK: - NTFSError

/// Error helpers. The bridge reports failure as a positive errno; Swift code
/// throws `POSIXError` so it can be handed straight to FSKit reply handlers.
struct NTFSError {
    /// Throws a `POSIXError` when `rc` is non-zero.
    static func check(_ rc: Int32) throws {
        guard rc != 0 else { return }
        throw posix(rc)
    }

    /// Builds a `POSIXError` for an errno value. Unknown values map to `EIO`.
    static func posix(_ code: Int32) -> POSIXError {
        POSIXError(POSIXErrorCode(rawValue: code) ?? .EIO)
    }

    /// Extracts an errno from an arbitrary Swift/Objective-C error. Errors that
    /// are not in the POSIX domain (for example FSKit or Foundation errors
    /// thrown by `FSBlockDeviceResource`) are reported as `EIO`.
    static func errnoValue(from error: any Error) -> Int32 {
        if let posixError = error as? POSIXError {
            return posixError.code.rawValue
        }
        let nsError = error as NSError
        if nsError.domain == NSPOSIXErrorDomain, nsError.code > 0, nsError.code <= Int(Int32.max) {
            return Int32(nsError.code)
        }
        return EIO
    }

    /// Human-readable description of a bridge errno (for logging).
    static func describe(_ code: Int32) -> String {
        if let message = ntfsb_strerror(code) {
            return String(cString: message)
        }
        return "errno \(code)"
    }
}

// MARK: - Value types

/// Kind of an NTFS item as reported by the bridge.
enum NTFSItemKind: Sendable {
    case file
    case directory
    case symlink
    case unknown

    init(_ type: ntfsb_type) {
        if type == NTFSB_TYPE_FILE {
            self = .file
        } else if type == NTFSB_TYPE_DIR {
            self = .directory
        } else if type == NTFSB_TYPE_SYMLINK {
            self = .symlink
        } else {
            self = .unknown
        }
    }

    /// The C enumerator for this kind.
    var cValue: ntfsb_type {
        switch self {
        case .file: return NTFSB_TYPE_FILE
        case .directory: return NTFSB_TYPE_DIR
        case .symlink: return NTFSB_TYPE_SYMLINK
        case .unknown: return NTFSB_TYPE_UNKNOWN
        }
    }
}

/// Swift mirror of `ntfsb_stat`.
struct NTFSStat: Sendable {
    var ino: UInt64
    var parentIno: UInt64
    var type: NTFSItemKind
    var mode: UInt32
    var nlink: UInt32
    var uid: UInt32
    var gid: UInt32
    var size: UInt64
    var allocSize: UInt64
    var atime: timespec
    var mtime: timespec
    var ctime: timespec
    var btime: timespec
    /// `NTFSB_FLAG_*` bits.
    var flags: UInt32
    /// MFT sequence number.
    var generation: UInt64

    init(_ c: ntfsb_stat) {
        ino = c.ino
        parentIno = c.parent_ino
        type = NTFSItemKind(c.type)
        mode = c.mode
        nlink = c.nlink
        uid = c.uid
        gid = c.gid
        size = c.size
        allocSize = c.alloc_size
        atime = c.atime
        mtime = c.mtime
        ctime = c.ctime
        btime = c.btime
        flags = c.flags
        generation = c.generation
    }

    var isReadOnlyAttr: Bool { flags & cFlag(NTFSB_FLAG_READONLY) != 0 }
    var isHiddenAttr: Bool { flags & cFlag(NTFSB_FLAG_HIDDEN) != 0 }
}

/// Swift mirror of `ntfsb_probe_info`.
struct NTFSProbeInfo: Sendable {
    var isNTFS: Bool
    var label: String
    var serial: UInt64
    var uuid: UUID
    var bytesPerSector: UInt32
    var clusterSize: UInt32
    var totalSectors: UInt64
    /// `$Volume` dirty flag set (Windows would run chkdsk).
    var dirty: Bool
    /// Windows hibernation / Fast Startup detected.
    var hibernated: Bool

    init(_ c: ntfsb_probe_info) {
        isNTFS = c.is_ntfs
        label = stringFromCCharArray(c.label)
        serial = c.serial
        uuid = UUID(uuid: c.uuid)
        bytesPerSector = c.bytes_per_sector
        clusterSize = c.cluster_size
        totalSectors = c.total_sectors
        dirty = c.dirty
        hibernated = c.hibernated
    }
}

/// Swift mirror of `ntfsb_volume_info`.
struct NTFSVolumeInfo: Sendable {
    var label: String
    var serial: UInt64
    var uuid: UUID
    var clusterSize: UInt32
    var sectorSize: UInt32
    var totalClusters: UInt64
    var freeClusters: UInt64
    var totalMFTRecords: UInt64
    var freeMFTRecords: UInt64
    var majorVersion: UInt8
    var minorVersion: UInt8
    var readOnly: Bool

    init(_ c: ntfsb_volume_info) {
        label = stringFromCCharArray(c.label)
        serial = c.serial
        uuid = UUID(uuid: c.uuid)
        clusterSize = c.cluster_size
        sectorSize = c.sector_size
        totalClusters = c.total_clusters
        freeClusters = c.free_clusters
        totalMFTRecords = c.total_mft_records
        freeMFTRecords = c.free_mft_records
        majorVersion = c.major_ver
        minorVersion = c.minor_ver
        readOnly = c.read_only
    }
}

/// Mount-time options (Swift mirror of `ntfsb_mount_opts`).
struct NTFSMountOptions: Sendable, Equatable {
    /// Mount read-only (`rdonly` / `ro`).
    var readOnly: Bool = false
    /// Delete hiberfil.sys when Windows is hibernated (`remove_hiberfile`). Destroys the hibernated session.
    var removeHiberfile: Bool = false
    /// Reset an unclean `$LogFile` (`recover`, ntfs-3g's default; disable with `norecover`).
    var recover: Bool = true
    /// Expose `$MFT`, `$Bitmap`, ... in the root directory (`show_sys_files`).
    var showSystemFiles: Bool = false
    /// Case-insensitive lookups (`ignore_case`, default on to match Windows and the
    /// `.insensitiveCasePreserving` capability; `case_sensitive` turns it off).
    var ignoreCase: Bool = true
    /// Owner reported for every item. 99 is macOS's "unknown" user, which the
    /// kernel treats as the accessing user so the console user gets access.
    var uid: UInt32 = 99
    /// Group reported for every item.
    var gid: UInt32 = 99
    /// Permission bits cleared from regular files.
    var fmask: UInt32 = 0o022
    /// Permission bits cleared from directories.
    var dmask: UInt32 = 0o022

    init() {}

    /// The C representation.
    var cValue: ntfsb_mount_opts {
        var flags: UInt32 = 0
        if readOnly { flags |= cFlag(NTFSB_MOUNT_RDONLY) }
        if removeHiberfile { flags |= cFlag(NTFSB_MOUNT_REMOVE_HIBER) }
        if recover { flags |= cFlag(NTFSB_MOUNT_RECOVER) }
        if showSystemFiles { flags |= cFlag(NTFSB_MOUNT_SHOW_SYS_FILES) }
        if ignoreCase { flags |= cFlag(NTFSB_MOUNT_IGNORE_CASE) }
        var opts = ntfsb_mount_opts()
        opts.flags = flags
        opts.uid = uid
        opts.gid = gid
        opts.fmask = fmask
        opts.dmask = dmask
        return opts
    }
}

/// Format (mkntfs) options (Swift mirror of `ntfsb_format_opts`).
struct NTFSFormatOptions: Sendable, Equatable {
    /// Volume label; `nil` or empty for none. At most 32 UTF-16 code units.
    var label: String?
    /// Cluster size in bytes; 0 lets mkntfs choose.
    var clusterSize: UInt32
    /// Skip zeroing the volume (mkntfs `-Q`).
    var quick: Bool
    /// Enable NTFS compression on the root directory (mkntfs `-C`).
    var enableCompression: Bool
    /// Partition start in sectors, written to the boot sector (mkntfs `-p`).
    var hiddenSectors: UInt64

    init(label: String? = nil, clusterSize: UInt32 = 0, quick: Bool = true,
         enableCompression: Bool = false, hiddenSectors: UInt64 = 0) {
        self.label = label
        self.clusterSize = clusterSize
        self.quick = quick
        self.enableCompression = enableCompression
        self.hiddenSectors = hiddenSectors
    }
}

/// Attribute changes for `NTFSMount.setattr`. `nil` fields are left untouched.
struct NTFSSetAttrRequest: Sendable {
    var size: UInt64?
    /// Only the owner write bit is honoured (maps to FILE_ATTR_READONLY).
    var mode: UInt32?
    /// Accepted and ignored by the bridge (no ownership on NTFS without a mapping file).
    var uid: UInt32?
    /// Accepted and ignored by the bridge.
    var gid: UInt32?
    var atime: timespec?
    var mtime: timespec?
    var ctime: timespec?
    var btime: timespec?
    /// `NTFSB_FLAG_*` bits; only READONLY / HIDDEN / SYSTEM / ARCHIVE are applied.
    var flags: UInt32?

    init() {}

    /// True when no field is set.
    var isEmpty: Bool {
        size == nil && mode == nil && uid == nil && gid == nil && atime == nil
            && mtime == nil && ctime == nil && btime == nil && flags == nil
    }

    /// The C representation, with `mask` describing which fields are valid.
    var cValue: ntfsb_setattr_req {
        var req = ntfsb_setattr_req()
        var mask: UInt32 = 0
        if let size { req.size = size; mask |= cFlag(NTFSB_SET_SIZE) }
        if let mode { req.mode = mode; mask |= cFlag(NTFSB_SET_MODE) }
        if let uid { req.uid = uid; mask |= cFlag(NTFSB_SET_UID) }
        if let gid { req.gid = gid; mask |= cFlag(NTFSB_SET_GID) }
        if let atime { req.atime = atime; mask |= cFlag(NTFSB_SET_ATIME) }
        if let mtime { req.mtime = mtime; mask |= cFlag(NTFSB_SET_MTIME) }
        if let ctime { req.ctime = ctime; mask |= cFlag(NTFSB_SET_CTIME) }
        if let btime { req.btime = btime; mask |= cFlag(NTFSB_SET_BTIME) }
        if let flags { req.flags = flags; mask |= cFlag(NTFSB_SET_FLAGS) }
        req.mask = mask
        return req
    }
}

// MARK: - Closure boxes for C callbacks

/// Holds the (non-escaping, made temporarily escapable) readdir body so its
/// address can travel through the bridge's `void *ctx`.
private final class ReaddirContext {
    let body: (String, UInt64, NTFSItemKind, UInt64) -> Bool
    init(body: @escaping (String, UInt64, NTFSItemKind, UInt64) -> Bool) {
        self.body = body
    }
}

/// Holds the format progress closure for the same reason.
private final class ProgressContext {
    let body: (Double) -> Bool
    init(body: @escaping (Double) -> Bool) {
        self.body = body
    }
}

// MARK: - NTFSBridge (volume-independent entry points)

enum NTFSBridge {
    /// Read-only probe of the device described by `io`. Returns
    /// `isNTFS == false` (not an error) for non-NTFS media.
    static func probe(io: BlockDeviceIO) throws -> NTFSProbeInfo {
        var cio = io.io
        var info = ntfsb_probe_info()
        let rc = withExtendedLifetime(io) {
            ntfsb_probe(&cio, &info)
        }
        try NTFSError.check(rc)
        return NTFSProbeInfo(info)
    }

    /// Creates a fresh NTFS file system on `io` (which must be writable).
    ///
    /// - Parameter progress: Called with a percentage in [0, 100]. Return
    ///   `true` to continue or `false` to request cancellation (best effort,
    ///   honoured between mkntfs phases). Called synchronously on the
    ///   formatting thread; never retained after `format` returns.
    static func format(io: BlockDeviceIO, options: NTFSFormatOptions,
                       progress: (Double) -> Bool) throws {
        guard !io.readOnly else { throw NTFSError.posix(EROFS) }
        if let label = options.label {
            try validateCString(label)
        }
        var cio = io.io
        let label = (options.label?.isEmpty ?? true) ? nil : options.label

        let rc: Int32 = withoutActuallyEscaping(progress) { escapableProgress -> Int32 in
            let context = ProgressContext(body: escapableProgress)
            return withExtendedLifetime((context, io)) { () -> Int32 in
                withOptionalCString(label) { labelPtr -> Int32 in
                    var opts = ntfsb_format_opts()
                    opts.label_utf8 = labelPtr
                    opts.cluster_size = options.clusterSize
                    opts.quick = options.quick
                    opts.enable_compression = options.enableCompression
                    opts.hidden_sectors = options.hiddenSectors
                    return ntfsb_format(&cio, &opts, { ctx, percent -> Int32 in
                        guard let ctx else { return 0 }
                        let box = Unmanaged<ProgressContext>.fromOpaque(ctx).takeUnretainedValue()
                        return box.body(percent) ? 0 : 1
                    }, Unmanaged.passUnretained(context).toOpaque())
                }
            }
        }
        try NTFSError.check(rc)
    }

    /// libntfs-3g version string, e.g. "2022.10.3".
    static var libraryVersion: String {
        guard let version = ntfsb_libntfs_version() else { return "unknown" }
        return String(cString: version)
    }

    /// Calls `body` with a C string for `string`, or with `nil`.
    private static func withOptionalCString<R>(_ string: String?,
                                               _ body: (UnsafePointer<CChar>?) -> R) -> R {
        guard let string else { return body(nil) }
        return string.withCString { body($0) }
    }
}

// MARK: - NTFSMount

/// A mounted NTFS volume. Owns the `ntfsb_volume *` and keeps the
/// `BlockDeviceIO` (whose address is the C `ctx`) alive until unmount.
///
/// Thread safety: every method takes `lock` for the duration of the bridge
/// call. The bridge already serialises per volume, so this costs no
/// parallelism, and it guarantees `unmount` can never free the handle while
/// another thread is inside a call. Consequently a `readdir` body must not
/// call back into the same `NTFSMount` (it would deadlock); `NTFSVolume`
/// collects entries first and post-processes them after `readdir` returns.
final class NTFSMount: @unchecked Sendable {
    /// The device this volume lives on. Retained so `io.ctx` stays valid.
    let io: BlockDeviceIO
    /// Options the volume was mounted with.
    let options: NTFSMountOptions

    private let lock = NSLock()
    private var handle: OpaquePointer?

    /// Mounts the volume. Throws `EPERM` when Windows is hibernated or the
    /// volume is unclean and a read-write mount was requested; callers may
    /// retry read-only or with `removeHiberfile` / `recover`.
    init(io: BlockDeviceIO, options: NTFSMountOptions) throws {
        self.io = io
        var effective = options
        if io.readOnly {
            effective.readOnly = true
        }
        self.options = effective

        var cio = io.io
        var copts = effective.cValue
        var volume: OpaquePointer?
        let rc = ntfsb_mount(&cio, &copts, &volume)
        try NTFSError.check(rc)
        guard let volume else { throw NTFSError.posix(EIO) }
        handle = volume
    }

    deinit {
        // Last-resort cleanup if the owner never unmounted (e.g. the
        // extension is torn down). Force so open inodes don't block it.
        if let handle {
            _ = ntfsb_unmount(handle, true)
        }
    }

    /// True until `unmount` succeeds.
    var isMounted: Bool {
        lock.withLock { handle != nil }
    }

    /// Runs `body` with the live handle while holding the lock.
    private func withHandle<R>(_ body: (OpaquePointer) throws -> R) throws -> R {
        lock.lock()
        defer { lock.unlock() }
        guard let handle else { throw NTFSError.posix(ENXIO) }
        return try body(handle)
    }

    /// Flushes and unmounts. With `force`, the handle is considered released
    /// even if the bridge reports an error (the header documents that a
    /// forced unmount always frees the volume); without `force`, a failure
    /// (typically `EBUSY`) leaves the volume mounted.
    func unmount(force: Bool) throws {
        lock.lock()
        defer { lock.unlock() }
        guard let current = handle else { return }
        let rc = ntfsb_unmount(current, force)
        if rc == 0 || force {
            handle = nil
        }
        try NTFSError.check(rc)
    }

    func sync() throws {
        try withHandle { try NTFSError.check(ntfsb_sync($0)) }
    }

    func volumeInfo() throws -> NTFSVolumeInfo {
        try withHandle { vol in
            var info = ntfsb_volume_info()
            try NTFSError.check(ntfsb_volume_info_get(vol, &info))
            return NTFSVolumeInfo(info)
        }
    }

    func setLabel(_ label: String) throws {
        try validateCString(label)
        try withHandle { vol in
            try NTFSError.check(label.withCString { ntfsb_set_label(vol, $0) })
        }
    }

    func getattr(_ ino: UInt64) throws -> NTFSStat {
        try withHandle { vol in
            var st = ntfsb_stat()
            try NTFSError.check(ntfsb_getattr(vol, ino, &st))
            return NTFSStat(st)
        }
    }

    func setattr(_ ino: UInt64, _ req: NTFSSetAttrRequest) throws -> NTFSStat {
        try withHandle { vol in
            var creq = req.cValue
            var st = ntfsb_stat()
            try NTFSError.check(ntfsb_setattr(vol, ino, &creq, &st))
            return NTFSStat(st)
        }
    }

    func lookup(dir: UInt64, name: String) throws -> NTFSStat {
        try validateCString(name)
        return try withHandle { vol in
            var st = ntfsb_stat()
            try NTFSError.check(name.withCString { ntfsb_lookup(vol, dir, $0, &st) })
            return NTFSStat(st)
        }
    }

    /// Enumerates `dir` starting after `cookie` (0 = beginning). `body` is
    /// called synchronously for each entry, including "." and ".."; return
    /// `false` from it to stop. Returns `true` when the end of the directory
    /// was reached, `false` when `body` stopped early. A stale cookie throws
    /// `EINVAL`. `body` must not call back into this `NTFSMount`.
    func readdir(dir: UInt64, cookie: UInt64,
                 _ body: (_ name: String, _ ino: UInt64, _ kind: NTFSItemKind, _ nextCookie: UInt64) -> Bool) throws -> Bool {
        try withoutActuallyEscaping(body) { escapableBody -> Bool in
            let context = ReaddirContext(body: escapableBody)
            return try withHandle { vol -> Bool in
                var eof = false
                let rc = withExtendedLifetime(context) { () -> Int32 in
                    ntfsb_readdir(vol, dir, cookie, { ctx, namePtr, ino, type, nextCookie -> Int32 in
                        guard let ctx else { return 1 }
                        let box = Unmanaged<ReaddirContext>.fromOpaque(ctx).takeUnretainedValue()
                        // A NULL name would be a bridge bug; skip the entry rather than crash.
                        guard let namePtr else { return 0 }
                        let name = String(cString: namePtr)
                        return box.body(name, ino, NTFSItemKind(type), nextCookie) ? 0 : 1
                    }, Unmanaged.passUnretained(context).toOpaque(), &eof)
                }
                try NTFSError.check(rc)
                return eof
            }
        }
    }

    func create(dir: UInt64, name: String, kind: NTFSItemKind, mode: UInt32) throws -> NTFSStat {
        guard kind == .file || kind == .directory else { throw NTFSError.posix(EINVAL) }
        try validateCString(name)
        return try withHandle { vol in
            var st = ntfsb_stat()
            try NTFSError.check(name.withCString {
                ntfsb_create(vol, dir, $0, kind.cValue, mode & 0o7777, &st)
            })
            return NTFSStat(st)
        }
    }

    func symlink(dir: UInt64, name: String, target: String) throws -> NTFSStat {
        try validateCString(name)
        try validateCString(target)
        return try withHandle { vol in
            var st = ntfsb_stat()
            try NTFSError.check(name.withCString { namePtr in
                target.withCString { targetPtr in
                    ntfsb_symlink(vol, dir, namePtr, targetPtr, &st)
                }
            })
            return NTFSStat(st)
        }
    }

    /// Reads a symlink target. Starts with a PATH_MAX-sized buffer and grows
    /// on `ERANGE` (NTFS reparse targets can exceed PATH_MAX).
    func readlink(_ ino: UInt64) throws -> String {
        try withHandle { vol in
            var capacity = 4096
            let maximum = 64 * 1024
            while true {
                var buffer = [CChar](repeating: 0, count: capacity)
                var length = 0
                let rc = buffer.withUnsafeMutableBufferPointer { ptr in
                    ntfsb_readlink(vol, ino, ptr.baseAddress, capacity, &length)
                }
                if rc == ERANGE && capacity < maximum {
                    capacity *= 4
                    continue
                }
                try NTFSError.check(rc)
                let usable = min(max(length, 0), capacity)
                return buffer.withUnsafeBufferPointer { ptr in
                    String(decoding: UnsafeRawBufferPointer(rebasing: UnsafeRawBufferPointer(ptr)[0..<usable]),
                           as: UTF8.self)
                }
            }
        }
    }

    func link(_ ino: UInt64, dir: UInt64, name: String) throws {
        try validateCString(name)
        try withHandle { vol in
            try NTFSError.check(name.withCString { ntfsb_link(vol, ino, dir, $0) })
        }
    }

    func unlink(dir: UInt64, name: String) throws {
        try validateCString(name)
        try withHandle { vol in
            try NTFSError.check(name.withCString { ntfsb_unlink(vol, dir, $0) })
        }
    }

    func rename(srcDir: UInt64, srcName: String, dstDir: UInt64, dstName: String) throws {
        try validateCString(srcName)
        try validateCString(dstName)
        try withHandle { vol in
            try NTFSError.check(srcName.withCString { srcPtr in
                dstName.withCString { dstPtr in
                    ntfsb_rename(vol, srcDir, srcPtr, dstDir, dstPtr)
                }
            })
        }
    }

    /// Reads up to `into.count` bytes at `offset`; returns the byte count
    /// (short at EOF, 0 past EOF).
    func read(_ ino: UInt64, into buffer: UnsafeMutableRawBufferPointer, offset: Int64) throws -> Int {
        guard offset >= 0 else { throw NTFSError.posix(EINVAL) }
        guard let base = buffer.baseAddress, buffer.count > 0 else { return 0 }
        return try withHandle { vol in
            var done: UInt64 = 0
            try NTFSError.check(ntfsb_read(vol, ino, base, UInt64(buffer.count), offset, &done))
            return Int(min(done, UInt64(buffer.count)))
        }
    }

    /// Writes `from` at `offset`, extending the file as needed; returns the
    /// number of bytes written.
    func write(_ ino: UInt64, from buffer: UnsafeRawBufferPointer, offset: Int64) throws -> Int {
        guard offset >= 0 else { throw NTFSError.posix(EINVAL) }
        guard let base = buffer.baseAddress, buffer.count > 0 else { return 0 }
        return try withHandle { vol in
            var done: UInt64 = 0
            try NTFSError.check(ntfsb_write(vol, ino, base, UInt64(buffer.count), offset, &done))
            return Int(min(done, UInt64(buffer.count)))
        }
    }

    func truncate(_ ino: UInt64, size: UInt64) throws {
        try withHandle { vol in
            try NTFSError.check(ntfsb_truncate(vol, ino, size))
        }
    }
}
