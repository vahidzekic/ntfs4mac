//
//  NTFSVolume.swift
//  NTFSExtension
//
//  The single FSVolume presented by NTFSFileSystem (FSUnaryFileSystem).
//
//  Life cycle (FSKit, macOS 15.4+)
//  -------------------------------
//    loadResource   -> NTFSFileSystem creates this object (nothing mounted yet)
//    activate       -> libntfs-3g mount (ntfsb_mount), root item returned
//    mount          -> notification only
//    ... item operations ...
//    unmount        -> flush
//    deactivate     -> ntfsb_unmount
//    unloadResource -> NTFSFileSystem drops this object
//
//  The libntfs-3g mount is deliberately deferred to `activate` (rather than
//  done in `loadResource`) because FSKit also calls `loadResource` before
//  `startCheck` and `startFormat`, which must be able to run against an
//  unmounted (or not-yet-NTFS) device.
//
//  Protocols: FSKit 15.4's `FSVolume.Operations` family (reply-handler
//  style). The macOS 27 `FSVolume.Handler` family is not used so the module
//  keeps the macOS 15.4 deployment target. Extended attributes are not
//  supported (no `FSVolume.XattrOperations` conformance); the kernel then
//  falls back to AppleDouble ("._") files for Finder metadata.
//
//  Concurrency: FSKit calls these methods concurrently from its own queues.
//  Every method does its (blocking) work synchronously before replying —
//  libntfs-3g is synchronous and serialised per volume by the bridge anyway —
//  so no non-Sendable FSKit object ever crosses a concurrency domain. The
//  volume's own mutable state lives behind `stateLock`.
//

import Foundation
import FSKit
import os

private let volLog = Logger(subsystem: "com.vahidzekic.ntfs4mac.NTFSExtension", category: "Volume")

final class NTFSVolume: FSVolume, FSVolume.Operations, FSVolume.ReadWriteOperations,
                        FSVolume.OpenCloseOperations, FSVolume.RenameOperations {

    // MARK: Immutable configuration

    /// Device the volume lives on.
    private let io: BlockDeviceIO
    /// Options parsed from `loadResource`; `activate` options are merged on top.
    private let baseOptions: NTFSMountOptions
    /// Coordinates activation with check/format on the same resource.
    private let gate: NTFSResourceGate
    /// False when the device did not probe as NTFS (load happened for a format).
    private let isNTFS: Bool

    /// Entries fetched from the bridge per readdir round trip.
    private static let readdirBatch = 128
    /// Preferred IO size reported in statfs.
    private static let preferredIOSize = 1 << 20

    // MARK: Mutable state (guarded by stateLock)

    private let stateLock = NSLock()
    private var ntfsMount: NTFSMount?
    private var isReadOnly = true
    private var effectiveOptions: NTFSMountOptions
    /// Live items by ino. Hard links share one entry.
    private var items: [UInt64: NTFSItem] = [:]
    /// Per-directory mutation counters used as enumeration verifiers.
    /// Directories never mutated use the implicit value 1.
    private var dirGenerations: [UInt64: UInt64] = [:]
    /// Last successful statfs, served if the bridge call fails.
    private var lastVolumeInfo: NTFSVolumeInfo?

    // MARK: Init

    /// - Parameters:
    ///   - volumeID: Stable identifier (derived from the NTFS serial number).
    ///   - volumeName: Label shown by Finder.
    ///   - io: Device access (writable unless the resource is read-only).
    ///   - options: Mount options parsed at load time.
    ///   - gate: Shared activation/maintenance coordinator.
    ///   - isNTFS: Whether the device probed as NTFS.
    init(volumeID: FSVolume.Identifier, volumeName: FSFileName, io: BlockDeviceIO,
         options: NTFSMountOptions, gate: NTFSResourceGate, isNTFS: Bool) {
        self.io = io
        self.baseOptions = options
        self.effectiveOptions = options
        self.gate = gate
        self.isNTFS = isNTFS
        super.init(volumeID: volumeID, volumeName: volumeName)
    }

    // MARK: - Internal helpers

    /// The mounted volume, or `ENXIO` before activate / after deactivate.
    private func currentMount() throws -> NTFSMount {
        guard let mount = stateLock.withLock({ ntfsMount }) else {
            throw NTFSError.posix(ENXIO)
        }
        return mount
    }

    /// The mounted volume for a mutating operation; `EROFS` on read-only mounts.
    private func writableMount() throws -> NTFSMount {
        let (mount, readOnly) = stateLock.withLock { (ntfsMount, isReadOnly) }
        guard let mount else { throw NTFSError.posix(ENXIO) }
        guard !readOnly else { throw NTFSError.posix(EROFS) }
        return mount
    }

    /// The live mount if activated (used by NTFSFileSystem.startCheck).
    func liveMount() -> NTFSMount? {
        stateLock.withLock { ntfsMount }
    }

    /// Casts an FSKit item to ours.
    private func ntfsItem(_ item: FSItem) throws -> NTFSItem {
        guard let ntfs = item as? NTFSItem else { throw NTFSError.posix(EINVAL) }
        return ntfs
    }

    /// Like `ntfsItem`, but fails with `ENOENT` for items whose inode is gone.
    private func liveItem(_ item: FSItem) throws -> NTFSItem {
        let ntfs = try ntfsItem(item)
        guard !ntfs.isDeleted else { throw NTFSError.posix(ENOENT) }
        return ntfs
    }

    /// Converts an FSKit name to a validated String.
    private func nameString(_ name: FSFileName) throws -> String {
        guard let string = name.string, !string.isEmpty else {
            throw NTFSError.posix(EINVAL)
        }
        // NTFS limit: 255 UTF-16 code units per component.
        guard string.utf16.count <= 255 else { throw NTFSError.posix(ENAMETOOLONG) }
        return string
    }

    /// Returns the cached item for `stat.ino`, or creates and caches one.
    private func cachedItem(for stat: NTFSStat) -> NTFSItem {
        let item: NTFSItem = stateLock.withLock {
            if let existing = items[stat.ino], !existing.isDeleted, existing.kind == stat.type {
                return existing
            }
            let fresh = NTFSItem(ino: stat.ino, kind: stat.type)
            items[stat.ino] = fresh
            return fresh
        }
        item.remember(stat)
        return item
    }

    /// Marks an item deleted and drops it from the cache.
    private func evict(_ item: NTFSItem) {
        item.markDeleted()
        stateLock.withLock {
            if items[item.ino] === item {
                items[item.ino] = nil
            }
        }
    }

    /// Current enumeration verifier for a directory (never 0).
    private func generation(of dirIno: UInt64) -> UInt64 {
        stateLock.withLock { dirGenerations[dirIno] ?? 1 }
    }

    /// Invalidates outstanding enumeration cookies for a directory.
    private func bumpGeneration(_ dirIno: UInt64) {
        stateLock.withLock {
            var next = (dirGenerations[dirIno] ?? 1) &+ 1
            if next == 0 { next = 1 }
            dirGenerations[dirIno] = next
        }
    }

    /// Link count of `ino`, or 1 if it can't be read (the conservative answer
    /// for deciding whether removing a name deletes the inode).
    private func linkCount(_ mount: NTFSMount, _ ino: UInt64) -> UInt32 {
        (try? mount.getattr(ino).nlink) ?? 1
    }

    /// True when removing one name of `item` (with `nlink` links) frees the inode.
    private func removalDeletesInode(_ item: NTFSItem, nlink: UInt32) -> Bool {
        item.kind == .directory || nlink <= 1
    }

    /// Builds FSKit attributes from bridge attributes, filling only what
    /// `request` asks for (everything when `request` is nil).
    private func makeAttributes(_ stat: NTFSStat, request: FSItem.GetAttributesRequest?) -> FSItem.Attributes {
        let attrs = FSItem.Attributes()
        func wanted(_ attribute: FSItem.Attribute) -> Bool {
            request?.isAttributeWanted(attribute) ?? true
        }
        if wanted(.type) { attrs.type = stat.type.fsItemType }
        if wanted(.mode) { attrs.mode = stat.mode }
        if wanted(.linkCount) { attrs.linkCount = stat.nlink }
        if wanted(.uid) { attrs.uid = stat.uid }
        if wanted(.gid) { attrs.gid = stat.gid }
        if wanted(.size) { attrs.size = stat.size }
        if wanted(.allocSize) { attrs.allocSize = stat.allocSize }
        if wanted(.fileID) { attrs.fileID = NTFSItem.identifier(forIno: stat.ino) }
        if wanted(.parentID) {
            if stat.ino == ntfsRootIno {
                attrs.parentID = .parentOfRoot
            } else if stat.parentIno != 0 {
                attrs.parentID = NTFSItem.identifier(forIno: stat.parentIno)
            }
        }
        if wanted(.flags) { attrs.flags = bsdFlags(for: stat) }
        if wanted(.accessTime) { attrs.accessTime = stat.atime }
        if wanted(.modifyTime) { attrs.modifyTime = stat.mtime }
        if wanted(.changeTime) { attrs.changeTime = stat.ctime }
        if wanted(.birthTime) { attrs.birthTime = stat.btime }
        return attrs
    }

    /// NTFS attribute bits -> BSD st_flags.
    ///
    /// FILE_ATTR_READONLY maps to UF_IMMUTABLE for files and symlinks only.
    /// Windows sets READONLY on many directories purely as a "has
    /// desktop.ini customisation" marker (e.g. user profile folders); mapping
    /// it to UF_IMMUTABLE there would make those folders undeletable and
    /// unwritable on macOS, so directories never report it.
    private func bsdFlags(for stat: NTFSStat) -> UInt32 {
        var flags: UInt32 = 0
        if stat.isHiddenAttr { flags |= UInt32(UF_HIDDEN) }
        if stat.isReadOnlyAttr && stat.type != .directory { flags |= UInt32(UF_IMMUTABLE) }
        return flags
    }

    /// BSD st_flags requested by the kernel -> full NTFSB_FLAG_* word.
    /// Bits we don't map (SYSTEM, ARCHIVE) are preserved from `current`; a
    /// directory's READONLY bit is preserved too (see `bsdFlags(for:)`).
    private func ntfsFlags(fromBSD bsd: UInt32, current: NTFSStat) -> UInt32 {
        let readOnlyBit = UInt32(truncatingIfNeeded: NTFSB_FLAG_READONLY)
        let hiddenBit = UInt32(truncatingIfNeeded: NTFSB_FLAG_HIDDEN)
        let systemBit = UInt32(truncatingIfNeeded: NTFSB_FLAG_SYSTEM)
        let archiveBit = UInt32(truncatingIfNeeded: NTFSB_FLAG_ARCHIVE)

        var flags = current.flags & (systemBit | archiveBit)
        if bsd & UInt32(UF_HIDDEN) != 0 { flags |= hiddenBit }
        if current.type == .directory {
            flags |= current.flags & readOnlyBit
        } else if bsd & UInt32(UF_IMMUTABLE) != 0 {
            flags |= readOnlyBit
        }
        return flags
    }

    /// Translates an FSKit set-attributes request into a bridge request and
    /// records which attributes we handle in `consumedAttributes`. Attributes
    /// NTFS cannot store (addedTime, backupTime, ...) are left unconsumed so
    /// the upper layers can report them, per FSKit's contract.
    private func translate(_ request: FSItem.SetAttributesRequest, for item: NTFSItem,
                           mount: NTFSMount, allowSize: Bool) throws -> (NTFSSetAttrRequest, FSItem.Attribute) {
        var req = NTFSSetAttrRequest()
        var consumed: FSItem.Attribute = []

        if allowSize, request.isValid(.size) {
            // Sizes of directories and symlinks are silently ignored (FSKit docs).
            if item.kind == .file || item.kind == .unknown {
                req.size = request.size
            }
            consumed.insert(.size)
        }
        if request.isValid(.mode) {
            req.mode = request.mode
            consumed.insert(.mode)
        }
        if request.isValid(.uid) {
            req.uid = request.uid
            consumed.insert(.uid)
        }
        if request.isValid(.gid) {
            req.gid = request.gid
            consumed.insert(.gid)
        }
        if request.isValid(.accessTime) {
            req.atime = request.accessTime
            consumed.insert(.accessTime)
        }
        if request.isValid(.modifyTime) {
            req.mtime = request.modifyTime
            consumed.insert(.modifyTime)
        }
        if request.isValid(.changeTime) {
            req.ctime = request.changeTime
            consumed.insert(.changeTime)
        }
        if request.isValid(.birthTime) {
            req.btime = request.birthTime
            consumed.insert(.birthTime)
        }
        if request.isValid(.flags) {
            let current = try mount.getattr(item.ino)
            req.flags = ntfsFlags(fromBSD: request.flags, current: current)
            consumed.insert(.flags)
        }
        return (req, consumed)
    }

    /// Mounts, retrying read-only when a read-write mount is refused with
    /// EPERM (Windows hibernated / Fast Startup, or an unclean journal that
    /// `recover` could not reset).
    private func mountWithFallback(_ options: inout NTFSMountOptions) throws -> NTFSMount {
        do {
            return try NTFSMount(io: io, options: options)
        } catch let error as POSIXError where error.code == .EPERM && !options.readOnly {
            volLog.warning("""
                read-write mount refused (EPERM): Windows is hibernated (Fast Startup) or the \
                volume is unclean. Falling back to read-only. Shut Windows down fully, or mount \
                with -o remove_hiberfile to discard the hibernated session.
                """)
            options.readOnly = true
            return try NTFSMount(io: io, options: options)
        }
    }

    // MARK: - FSVolume.PathConfOperations

    var maximumLinkCount: Int { 1024 }

    var maximumNameLength: Int { 255 }

    var restrictsOwnershipChanges: Bool { false }

    var truncatesLongNames: Bool { false }

    /// libntfs-3g uses signed 64-bit sizes.
    var maximumFileSize: UInt64 { UInt64(Int64.max) }

    // MARK: - FSVolume.Operations: properties

    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let ignoreCase = stateLock.withLock { effectiveOptions.ignoreCase }
        let caps = FSVolume.SupportedCapabilities()
        caps.supportsPersistentObjectIDs = true
        caps.supports64BitObjectIDs = true
        caps.supportsSymbolicLinks = true
        caps.supportsHardLinks = true
        caps.supportsSparseFiles = true
        caps.supportsHiddenFiles = true
        caps.supports2TBFiles = true
        // libntfs-3g does not replay or write the $LogFile journal.
        caps.supportsJournal = false
        caps.supportsActiveJournal = false
        caps.caseFormat = ignoreCase ? .insensitiveCasePreserving : .sensitive
        return caps
    }

    var volumeStatistics: FSStatFSResult {
        let result = FSStatFSResult(fileSystemTypeName: "ntfs")
        result.fileSystemSubType = 0
        result.ioSize = Self.preferredIOSize

        let mount = stateLock.withLock { ntfsMount }
        var info: NTFSVolumeInfo?
        if let mount {
            do {
                let fresh = try mount.volumeInfo()
                stateLock.withLock { lastVolumeInfo = fresh }
                info = fresh
            } catch {
                volLog.error("statfs: volume info failed: \(error.localizedDescription, privacy: .public)")
                info = stateLock.withLock { lastVolumeInfo }
            }
        }
        guard let info, info.clusterSize > 0 else {
            result.blockSize = Int(io.blockSize)
            return result
        }

        let cluster = UInt64(info.clusterSize)
        let total = info.totalClusters
        let free = min(info.freeClusters, total)
        result.blockSize = Int(info.clusterSize)
        result.totalBlocks = total
        result.freeBlocks = free
        result.availableBlocks = free
        result.usedBlocks = total - free
        result.totalBytes = total.multipliedReportingOverflow(by: cluster).overflow ? .max : total * cluster
        result.freeBytes = free.multipliedReportingOverflow(by: cluster).overflow ? .max : free * cluster
        result.availableBytes = result.freeBytes
        result.usedBytes = result.totalBytes - result.freeBytes
        result.totalFiles = info.totalMFTRecords
        result.freeFiles = min(info.freeMFTRecords, info.totalMFTRecords)
        return result
    }

    // MARK: - FSVolume.Operations: activation & mounting

    func activate(options: FSTaskOptions,
                  replyHandler reply: @escaping @Sendable (FSItem?, (any Error)?) -> Void) {
        guard isNTFS else {
            reply(nil, FSError(.resourceUnrecognized))
            return
        }
        guard gate.beginActivation() else {
            volLog.error("activate refused: a check or format is running, or the volume is already active")
            reply(nil, NTFSError.posix(EBUSY))
            return
        }
        do {
            var opts = baseOptions
            try NTFSTaskOptionParser.applyMountOptions(options.taskOptions, to: &opts)
            if io.readOnly { opts.readOnly = true }

            let mount = try mountWithFallback(&opts)
            let root: NTFSItem
            let info: NTFSVolumeInfo
            do {
                info = try mount.volumeInfo()
                let rootStat = try mount.getattr(ntfsRootIno)
                guard rootStat.type == .directory else { throw NTFSError.posix(ENOTDIR) }
                root = NTFSItem(ino: ntfsRootIno, kind: .directory, stat: rootStat)
            } catch {
                try? mount.unmount(force: true)
                throw error
            }

            let readOnly = opts.readOnly || info.readOnly
            stateLock.withLock {
                ntfsMount = mount
                isReadOnly = readOnly
                effectiveOptions = opts
                items = [ntfsRootIno: root]
                dirGenerations = [:]
                lastVolumeInfo = info
            }
            volLog.notice("""
                activated NTFS \(info.majorVersion, privacy: .public).\(info.minorVersion, privacy: .public) \
                volume '\(info.label, privacy: .public)' (\(readOnly ? "read-only" : "read-write", privacy: .public), \
                cluster \(info.clusterSize, privacy: .public) B, libntfs-3g \(NTFSBridge.libraryVersion, privacy: .public))
                """)
            reply(root, nil)
        } catch {
            gate.endActivation()
            volLog.error("activate failed: \(error.localizedDescription, privacy: .public)")
            reply(nil, error)
        }
    }

    func deactivate(options: FSDeactivateOptions,
                    replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        let force = options.contains(.force)
        let mount: NTFSMount? = stateLock.withLock { ntfsMount }
        guard let mount else {
            reply(nil)
            return
        }
        do {
            try mount.unmount(force: force)
        } catch {
            if !force {
                volLog.error("deactivate: unmount failed: \(error.localizedDescription, privacy: .public)")
                reply(error)
                return
            }
            volLog.error("deactivate: forced unmount reported: \(error.localizedDescription, privacy: .public)")
        }
        shutdownState()
        reply(nil)
    }

    /// Clears per-mount state and releases the activation gate.
    private func shutdownState() {
        let released: [NTFSItem] = stateLock.withLock {
            let all = Array(items.values)
            ntfsMount = nil
            items = [:]
            dirGenerations = [:]
            isReadOnly = true
            return all
        }
        // Any FSItem FSKit still holds must never touch the device again.
        released.forEach { $0.markDeleted() }
        gate.endActivation()
    }

    /// Forced teardown used by NTFSFileSystem.unloadResource when FSKit
    /// unloads without a prior deactivate.
    func forceShutdown() {
        guard let mount = stateLock.withLock({ ntfsMount }) else { return }
        do {
            try mount.unmount(force: true)
        } catch {
            volLog.error("forced unmount reported: \(error.localizedDescription, privacy: .public)")
        }
        shutdownState()
    }

    func mount(options: FSTaskOptions,
               replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        // All work happened in activate; nothing extra to do per mount.
        reply(nil)
    }

    func unmount(replyHandler reply: @escaping @Sendable () -> Void) {
        // Flush now so data is durable even if deactivate is delayed.
        if let mount = stateLock.withLock({ isReadOnly ? nil : ntfsMount }) {
            do {
                try mount.sync()
            } catch {
                volLog.error("unmount: sync failed: \(error.localizedDescription, privacy: .public)")
            }
        }
        reply()
    }

    func synchronize(flags: FSSyncFlags,
                     replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        do {
            let (mount, readOnly) = stateLock.withLock { (ntfsMount, isReadOnly) }
            guard let mount else { throw NTFSError.posix(ENXIO) }
            if !readOnly {
                try mount.sync()
            }
            reply(nil)
        } catch {
            reply(error)
        }
    }

    // MARK: - FSVolume.Operations: attributes

    func getAttributes(_ desiredAttributes: FSItem.GetAttributesRequest, of item: FSItem,
                       replyHandler reply: @escaping @Sendable (FSItem.Attributes?, (any Error)?) -> Void) {
        do {
            let ntfs = try ntfsItem(item)
            if ntfs.isDeleted {
                // fstat() on a file unlinked while open: serve the last known
                // attributes with no links rather than failing.
                guard var stat = ntfs.lastStat else { throw NTFSError.posix(ENOENT) }
                stat.nlink = 0
                reply(makeAttributes(stat, request: desiredAttributes), nil)
                return
            }
            let stat = try currentMount().getattr(ntfs.ino)
            ntfs.remember(stat)
            reply(makeAttributes(stat, request: desiredAttributes), nil)
        } catch {
            reply(nil, error)
        }
    }

    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on item: FSItem,
                       replyHandler reply: @escaping @Sendable (FSItem.Attributes?, (any Error)?) -> Void) {
        do {
            let ntfs = try liveItem(item)
            let mount = try writableMount()
            let (req, consumed) = try translate(newAttributes, for: ntfs, mount: mount, allowSize: true)
            let stat = try req.isEmpty ? mount.getattr(ntfs.ino) : mount.setattr(ntfs.ino, req)
            newAttributes.consumedAttributes = consumed
            ntfs.remember(stat)
            reply(makeAttributes(stat, request: nil), nil)
        } catch {
            reply(nil, error)
        }
    }

    // MARK: - FSVolume.Operations: namespace

    func lookupItem(named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void) {
        do {
            let dir = try liveItem(directory)
            guard dir.kind == .directory else { throw NTFSError.posix(ENOTDIR) }
            let mount = try currentMount()
            guard let string = name.string, !string.isEmpty else { throw NTFSError.posix(ENOENT) }

            let stat: NTFSStat
            switch string {
            case ".":
                stat = try mount.getattr(dir.ino)
            case "..":
                let dirStat = try mount.getattr(dir.ino)
                let parent = (dir.isRoot || dirStat.parentIno == 0) ? ntfsRootIno : dirStat.parentIno
                stat = try mount.getattr(parent)
            default:
                guard string.utf16.count <= 255 else { throw NTFSError.posix(ENAMETOOLONG) }
                stat = try mount.lookup(dir: dir.ino, name: string)
            }
            let found = cachedItem(for: stat)
            reply(found, name, nil)
        } catch {
            reply(nil, nil, error)
        }
    }

    func reclaimItem(_ item: FSItem,
                     replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        // FSKit no longer references the item; drop it from the cache unless a
        // concurrent lookup already replaced the entry with a newer object.
        if let ntfs = item as? NTFSItem {
            stateLock.withLock {
                if items[ntfs.ino] === ntfs {
                    items[ntfs.ino] = nil
                }
            }
        }
        reply(nil)
    }

    func readSymbolicLink(_ item: FSItem,
                          replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void) {
        do {
            let link = try liveItem(item)
            guard link.kind == .symlink else { throw NTFSError.posix(EINVAL) }
            let target = try currentMount().readlink(link.ino)
            reply(FSFileName(string: target), nil)
        } catch {
            reply(nil, error)
        }
    }

    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem,
                    attributes newAttributes: FSItem.SetAttributesRequest,
                    replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void) {
        do {
            let dir = try liveItem(directory)
            guard dir.kind == .directory else { throw NTFSError.posix(ENOTDIR) }
            let mount = try writableMount()
            let string = try nameString(name)

            let kind: NTFSItemKind
            switch type {
            case .file: kind = .file
            case .directory: kind = .directory
            default:
                // FIFOs, sockets and device nodes have no NTFS representation.
                throw NTFSError.posix(ENOTSUP)
            }
            let defaultMode: UInt32 = kind == .directory ? 0o755 : 0o644
            let mode = newAttributes.isValid(.mode) ? (newAttributes.mode & 0o7777) : defaultMode

            var stat = try mount.create(dir: dir.ino, name: string, kind: kind, mode: mode)
            bumpGeneration(dir.ino)
            let created = cachedItem(for: stat)

            // Apply the remaining requested attributes (times, flags). The item
            // exists at this point, so a failure here is logged, not fatal.
            do {
                var (req, consumed) = try translate(newAttributes, for: created, mount: mount, allowSize: false)
                req.mode = nil // already applied by create
                req.uid = nil  // ownership is synthesised by the bridge
                req.gid = nil
                if !req.isEmpty {
                    stat = try mount.setattr(stat.ino, req)
                    created.remember(stat)
                }
                consumed.insert(.mode)
                newAttributes.consumedAttributes = consumed
            } catch {
                volLog.error("create: applying initial attributes failed: \(error.localizedDescription, privacy: .public)")
                newAttributes.consumedAttributes = [.mode, .uid, .gid]
            }
            reply(created, name, nil)
        } catch {
            reply(nil, nil, error)
        }
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem,
                            attributes newAttributes: FSItem.SetAttributesRequest,
                            linkContents contents: FSFileName,
                            replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void) {
        do {
            let dir = try liveItem(directory)
            guard dir.kind == .directory else { throw NTFSError.posix(ENOTDIR) }
            let mount = try writableMount()
            let string = try nameString(name)
            guard let target = contents.string, !target.isEmpty else { throw NTFSError.posix(EINVAL) }

            let stat = try mount.symlink(dir: dir.ino, name: string, target: target)
            bumpGeneration(dir.ino)
            let link = cachedItem(for: stat)
            // Symlink mode/ownership are synthesised; accept them as handled.
            newAttributes.consumedAttributes = [.mode, .uid, .gid]
            reply(link, name, nil)
        } catch {
            reply(nil, nil, error)
        }
    }

    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void) {
        do {
            let target = try liveItem(item)
            let dir = try liveItem(directory)
            guard dir.kind == .directory else { throw NTFSError.posix(ENOTDIR) }
            guard target.kind != .directory else { throw NTFSError.posix(EPERM) }
            let mount = try writableMount()
            let string = try nameString(name)

            try mount.link(target.ino, dir: dir.ino, name: string)
            bumpGeneration(dir.ino)
            if let stat = try? mount.getattr(target.ino) {
                target.remember(stat)
            }
            reply(name, nil)
        } catch {
            reply(nil, error)
        }
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem,
                    replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        do {
            let victim = try liveItem(item)
            let dir = try liveItem(directory)
            let mount = try writableMount()
            let string = try nameString(name)

            let nlink = linkCount(mount, victim.ino)
            try mount.unlink(dir: dir.ino, name: string)
            bumpGeneration(dir.ino)
            if removalDeletesInode(victim, nlink: nlink) {
                evict(victim)
                // The directory's own enumeration generation is meaningless now.
                if victim.kind == .directory {
                    stateLock.withLock { dirGenerations[victim.ino] = nil }
                }
            } else if let stat = try? mount.getattr(victim.ino) {
                victim.remember(stat)
            }
            reply(nil)
        } catch {
            reply(error)
        }
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem,
                    overItem: FSItem?,
                    replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void) {
        do {
            let moving = try liveItem(item)
            let srcDir = try liveItem(sourceDirectory)
            let dstDir = try liveItem(destinationDirectory)
            guard srcDir.kind == .directory, dstDir.kind == .directory else { throw NTFSError.posix(ENOTDIR) }
            let mount = try writableMount()
            let srcName = try nameString(sourceName)
            let dstName = try nameString(destinationName)

            // Whatever is being replaced must be inspected before the rename.
            var replaced: (item: NTFSItem, nlink: UInt32)?
            if let over = overItem as? NTFSItem, over !== moving, !over.isDeleted {
                if moving.kind == .directory && over.kind != .directory { throw NTFSError.posix(ENOTDIR) }
                if moving.kind != .directory && over.kind == .directory { throw NTFSError.posix(EISDIR) }
                replaced = (over, linkCount(mount, over.ino))
            }

            try mount.rename(srcDir: srcDir.ino, srcName: srcName, dstDir: dstDir.ino, dstName: dstName)
            bumpGeneration(srcDir.ino)
            if dstDir.ino != srcDir.ino {
                bumpGeneration(dstDir.ino)
            }
            if let replaced {
                if removalDeletesInode(replaced.item, nlink: replaced.nlink) {
                    evict(replaced.item)
                } else if let stat = try? mount.getattr(replaced.item.ino) {
                    replaced.item.remember(stat)
                }
            }
            reply(destinationName, nil)
        } catch {
            reply(nil, error)
        }
    }

    // MARK: - FSVolume.Operations: enumeration

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie,
                            verifier: FSDirectoryVerifier, attributes: FSItem.GetAttributesRequest?,
                            packer: FSDirectoryEntryPacker,
                            replyHandler reply: @escaping @Sendable (FSDirectoryVerifier, (any Error)?) -> Void) {
        do {
            let dir = try liveItem(directory)
            guard dir.kind == .directory else { throw NTFSError.posix(ENOTDIR) }
            let mount = try currentMount()

            // Verifier = per-directory mutation generation. A resumed
            // enumeration (non-initial cookie) whose verifier no longer
            // matches saw the directory change underneath it.
            let current = generation(of: dir.ino)
            if cookie.rawValue != FSDirectoryCookie.initial.rawValue,
               verifier.rawValue != FSDirectoryVerifier.initial.rawValue,
               verifier.rawValue != current {
                throw FSError(.invalidDirectoryCookie)
            }

            typealias Entry = (name: String, ino: UInt64, kind: NTFSItemKind, nextCookie: UInt64)
            var position = cookie.rawValue
            var packerFull = false

            while !packerFull {
                // 1. Fetch a batch. The readdir body runs while the bridge's
                //    volume lock is held, so it only collects; attribute
                //    lookups (which re-enter the bridge) and packing happen
                //    after readdir returns.
                var batch: [Entry] = []
                batch.reserveCapacity(Self.readdirBatch)
                let reachedEnd: Bool
                do {
                    reachedEnd = try mount.readdir(dir: dir.ino, cookie: position) { name, ino, kind, next in
                        batch.append((name, ino, kind, next))
                        return batch.count < Self.readdirBatch
                    }
                } catch let error as POSIXError where error.code == .EINVAL {
                    throw FSError(.invalidDirectoryCookie)
                }

                // 2. Pack. Stop as soon as the packer is full; FSKit resumes
                //    from the nextCookie of the last entry that was packed.
                for entry in batch {
                    let isDotEntry = entry.name == "." || entry.name == ".."
                    var entryAttributes: FSItem.Attributes?
                    if let attributes {
                        // FSKit: don't pack "." / ".." when attributes are requested.
                        if isDotEntry {
                            position = entry.nextCookie
                            continue
                        }
                        guard let stat = try? mount.getattr(entry.ino) else {
                            // Vanished between readdir and getattr; skip it.
                            position = entry.nextCookie
                            continue
                        }
                        entryAttributes = makeAttributes(stat, request: attributes)
                    }
                    let itemType: FSItem.ItemType = isDotEntry ? .directory : entry.kind.fsItemType
                    let packed = packer.packEntry(name: FSFileName(string: entry.name),
                                                  itemType: itemType,
                                                  itemID: NTFSItem.identifier(forIno: entry.ino),
                                                  nextCookie: FSDirectoryCookie(entry.nextCookie),
                                                  attributes: entryAttributes)
                    if !packed {
                        packerFull = true
                        break
                    }
                    position = entry.nextCookie
                }

                if reachedEnd || batch.isEmpty {
                    break
                }
            }
            reply(FSDirectoryVerifier(current), nil)
        } catch {
            reply(FSDirectoryVerifier.initial, error)
        }
    }

    // MARK: - FSVolume.ReadWriteOperations

    func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer,
              replyHandler reply: @escaping @Sendable (Int, (any Error)?) -> Void) {
        do {
            let file = try liveItem(item)
            guard file.kind != .directory else { throw NTFSError.posix(EISDIR) }
            guard offset >= 0 else { throw NTFSError.posix(EINVAL) }
            let mount = try currentMount()
            let wanted = min(length, buffer.length)
            guard wanted > 0 else {
                reply(0, nil)
                return
            }
            let count = try buffer.withUnsafeMutableBytes { raw -> Int in
                let window = UnsafeMutableRawBufferPointer(rebasing: raw[0..<min(wanted, raw.count)])
                return try mount.read(file.ino, into: window, offset: offset)
            }
            reply(count, nil)
        } catch {
            reply(0, error)
        }
    }

    func write(contents: Data, to item: FSItem, at offset: off_t,
               replyHandler reply: @escaping @Sendable (Int, (any Error)?) -> Void) {
        do {
            let file = try liveItem(item)
            guard file.kind != .directory else { throw NTFSError.posix(EISDIR) }
            guard offset >= 0 else { throw NTFSError.posix(EINVAL) }
            let mount = try writableMount()
            guard !contents.isEmpty else {
                reply(0, nil)
                return
            }
            let written = try contents.withUnsafeBytes { raw -> Int in
                try mount.write(file.ino, from: raw, offset: offset)
            }
            reply(written, nil)
        } catch {
            reply(0, error)
        }
    }

    // MARK: - FSVolume.OpenCloseOperations

    // libntfs-3g opens inodes per call inside the bridge, so open/close need
    // no state. Conforming (rather than not) keeps FSKit's open-mode checks
    // in play and lets us refuse write opens on read-only mounts early.

    func openItem(_ item: FSItem, modes: FSVolume.OpenModes,
                  replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        if modes.contains(.write) {
            let readOnly = stateLock.withLock { isReadOnly }
            if readOnly {
                reply(NTFSError.posix(EROFS))
                return
            }
        }
        reply(nil)
    }

    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes,
                   replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        reply(nil)
    }

    // MARK: - FSVolume.RenameOperations

    func setVolumeName(_ name: FSFileName,
                       replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void) {
        do {
            let mount = try writableMount()
            guard let label = name.string else { throw NTFSError.posix(EINVAL) }
            // NTFS labels hold at most 32 UTF-16 code units.
            guard label.utf16.count <= 32 else { throw NTFSError.posix(ENAMETOOLONG) }
            try mount.setLabel(label)
            self.name = name
            reply(name, nil)
        } catch {
            reply(nil, error)
        }
    }
}
