//
//  NTFSFileSystem.swift
//  NTFSExtension
//
//  FSUnaryFileSystem implementation: probe / load / unload, plus the
//  FSManageableResourceMaintenanceOperations check (fsck) and format (newfs)
//  tasks.
//
//  How FSKit drives this class (macOS 15.4+)
//  ----------------------------------------
//  * probeResource: read-only look at the device; decides whether DiskArbitration
//    offers this module for the partition.
//  * loadResource: hands us the FSBlockDeviceResource and the mount options;
//    we return the (not yet mounted) NTFSVolume. FSKit also calls
//    loadResource before startCheck / startFormat, so loading never mounts
//    and never requires the device to already contain NTFS — the libntfs-3g
//    mount happens in NTFSVolume.activate.
//  * startCheck / startFormat: must validate options synchronously, then do
//    the work on another thread, report through the returned Progress and
//    finish with FSTask.didComplete(error:). DiskArbitration runs a quick
//    check (-q) first and, if that fails, a full check with -y; it only
//    mounts when the check succeeds.
//
//  Option syntax (must match Info.plist, see docs/FSKIT_NOTES.md):
//    FSActivateOptionSyntax  shortOptions "o:rwu:g:"
//    FSCheckOptionSyntax     shortOptions "nqyfl"
//    FSFormatOptionSyntax    shortOptions "L:v:c:p:QfC"
//

import Foundation
import FSKit
import os

private let fsLog = Logger(subsystem: "com.vahidzekic.ntfs4mac.NTFSExtension", category: "FileSystem")

// MARK: - Activation / maintenance coordination

/// Serialises the three things that may touch a loaded device: an active
/// mount, a check and a format. A check against an *active* volume does not
/// take the gate (it only inspects the live mount).
final class NTFSResourceGate: @unchecked Sendable {
    private let lock = NSLock()
    private var active = false
    private var maintenance = false

    /// Claims the device for a mount. False if mounted or under maintenance.
    func beginActivation() -> Bool {
        lock.withLock {
            guard !active, !maintenance else { return false }
            active = true
            return true
        }
    }

    func endActivation() {
        lock.withLock { active = false }
    }

    /// Claims the device for check/format. False if mounted or busy.
    func beginMaintenance() -> Bool {
        lock.withLock {
            guard !active, !maintenance else { return false }
            maintenance = true
            return true
        }
    }

    func endMaintenance() {
        lock.withLock { maintenance = false }
    }
}

// MARK: - Progress reporting

/// Sendable handle to the Progress returned to FSKit. `Progress` is
/// internally synchronised; this wrapper just lets it cross into the worker
/// closure without relying on the SDK's Sendable annotation for it.
final class NTFSProgressReporter: @unchecked Sendable {
    let progress: Progress

    init() {
        progress = Progress(totalUnitCount: 100)
    }

    /// Sets the completed percentage (monotonic, clamped to 0...100).
    func update(_ percent: Double) {
        guard percent.isFinite else { return }
        let value = Int64(min(max(percent, 0), 100).rounded(.down))
        if value > progress.completedUnitCount {
            progress.completedUnitCount = value
        }
    }

    var isCancelled: Bool { progress.isCancelled }

    func finish() {
        progress.completedUnitCount = progress.totalUnitCount
    }
}

// MARK: - Option parsing

/// Options for startCheck (fsck semantics).
struct NTFSCheckOptions: Sendable {
    /// -q: quick check — only report whether the volume needs repair.
    var quick = false
    /// -y: repair without asking.
    var repair = false
    /// -n: never write to the device.
    var noWrite = false
    /// -f: force a full check even if the volume looks clean (accepted; the
    /// full check is always run when -q is absent).
    var force = false
}

/// Parsers for FSTaskOptions.taskOptions (an argv-style [String]).
enum NTFSTaskOptionParser {
    /// One parsed short option.
    struct Flag {
        let letter: Character
        let value: String?
    }

    /// getopt(3)-style parse of `args` against `spec` ("ab:c" = -a, -b VALUE, -c).
    /// Supports clustered flags (-Qf), attached values (-Lname) and "--".
    /// - Parameter strict: throw EINVAL on unknown letters (format) or
    ///   accept them as value-less flags (mount, check — the system may pass
    ///   generic options we don't care about).
    static func getopt(_ args: [String], spec: String, strict: Bool) throws -> (flags: [Flag], operands: [String]) {
        var takesValue: [Character: Bool] = [:]
        let specChars = Array(spec)
        var index = 0
        while index < specChars.count {
            let letter = specChars[index]
            let hasValue = index + 1 < specChars.count && specChars[index + 1] == ":"
            takesValue[letter] = hasValue
            index += hasValue ? 2 : 1
        }

        var flags: [Flag] = []
        var operands: [String] = []
        var position = 0
        while position < args.count {
            let arg = args[position]
            position += 1
            if arg == "--" {
                operands.append(contentsOf: args[position...])
                break
            }
            guard arg.hasPrefix("-"), arg.count > 1, !arg.hasPrefix("--") else {
                operands.append(arg)
                continue
            }
            var letters = Substring(arg.dropFirst())
            while let letter = letters.first {
                letters = letters.dropFirst()
                guard let hasValue = takesValue[letter] else {
                    if strict {
                        fsLog.error("unknown option -\(String(letter), privacy: .public)")
                        throw NTFSError.posix(EINVAL)
                    }
                    flags.append(Flag(letter: letter, value: nil))
                    continue
                }
                if hasValue {
                    if !letters.isEmpty {
                        flags.append(Flag(letter: letter, value: String(letters)))
                    } else if position < args.count {
                        flags.append(Flag(letter: letter, value: args[position]))
                        position += 1
                    } else {
                        fsLog.error("option -\(String(letter), privacy: .public) requires a value")
                        throw NTFSError.posix(EINVAL)
                    }
                    break
                }
                flags.append(Flag(letter: letter, value: nil))
            }
        }
        return (flags, operands)
    }

    // MARK: Mount

    /// Applies mount options to `options`.
    ///
    /// Accepted forms: `-o list`, `-olist`, bare `list` operands (some
    /// callers pass options without `-o`), `-r` (read-only), `-w`
    /// (read-write), `-u uid`, `-g gid`. A list is comma separated:
    ///
    ///   rdonly | ro            read-only mount
    ///   rw                     read-write mount (default)
    ///   remove_hiberfile       delete hiberfil.sys of a hibernated Windows
    ///   recover | norecover    reset an unclean journal (default: recover)
    ///   show_sys_files         expose $MFT etc. | hide_sys_files
    ///   ignore_case            case-insensitive lookups (default)
    ///   case_sensitive | noignore_case
    ///   uid=N, gid=N           owner/group reported for every item
    ///   fmask=OCT, dmask=OCT, umask=OCT
    ///
    /// Unknown list entries (nosuid, nodev, nobrowse, ...) are ignored; they
    /// are handled by the kernel / mount(8). Malformed numbers throw EINVAL.
    static func applyMountOptions(_ args: [String], to options: inout NTFSMountOptions) throws {
        let parsed = try getopt(args, spec: "o:rwu:g:", strict: false)
        for flag in parsed.flags {
            switch flag.letter {
            case "o":
                try applyMountList(flag.value ?? "", to: &options)
            case "r":
                options.readOnly = true
            case "w":
                options.readOnly = false
            case "u":
                options.uid = try decimal(flag.value, name: "uid")
            case "g":
                options.gid = try decimal(flag.value, name: "gid")
            default:
                fsLog.debug("ignoring mount flag -\(String(flag.letter), privacy: .public)")
            }
        }
        for operand in parsed.operands {
            try applyMountList(operand, to: &options)
        }
    }

    private static func applyMountList(_ list: String, to options: inout NTFSMountOptions) throws {
        for rawItem in list.split(separator: ",") {
            let item = rawItem.trimmingCharacters(in: .whitespaces)
            guard !item.isEmpty else { continue }
            let key: String
            let value: String?
            if let equals = item.firstIndex(of: "=") {
                key = String(item[..<equals]).lowercased()
                value = String(item[item.index(after: equals)...])
            } else {
                key = item.lowercased()
                value = nil
            }
            switch key {
            case "rdonly", "ro":
                options.readOnly = true
            case "rw":
                options.readOnly = false
            case "remove_hiberfile":
                options.removeHiberfile = true
            case "recover":
                options.recover = true
            case "norecover":
                options.recover = false
            case "show_sys_files":
                options.showSystemFiles = true
            case "hide_sys_files":
                options.showSystemFiles = false
            case "ignore_case":
                options.ignoreCase = true
            case "case_sensitive", "noignore_case":
                options.ignoreCase = false
            case "uid":
                options.uid = try decimal(value, name: "uid")
            case "gid":
                options.gid = try decimal(value, name: "gid")
            case "fmask":
                options.fmask = try octalMask(value, name: "fmask")
            case "dmask":
                options.dmask = try octalMask(value, name: "dmask")
            case "umask":
                let mask = try octalMask(value, name: "umask")
                options.fmask = mask
                options.dmask = mask
            default:
                fsLog.debug("ignoring mount option '\(item, privacy: .public)'")
            }
        }
    }

    private static func decimal(_ value: String?, name: String) throws -> UInt32 {
        guard let value, let number = UInt32(value) else {
            fsLog.error("invalid \(name, privacy: .public) value '\(value ?? "", privacy: .public)'")
            throw NTFSError.posix(EINVAL)
        }
        return number
    }

    private static func octalMask(_ value: String?, name: String) throws -> UInt32 {
        guard let value, let number = UInt32(value, radix: 8), number <= 0o7777 else {
            fsLog.error("invalid \(name, privacy: .public) value '\(value ?? "", privacy: .public)'")
            throw NTFSError.posix(EINVAL)
        }
        return number
    }

    // MARK: Format

    /// Parses format options (FSFormatOptionSyntax "L:v:c:p:QfC"):
    ///
    ///   -L label | -v label   volume label (≤ 32 UTF-16 units; -v is the
    ///                         newfs_* convention used by Disk Utility)
    ///   -c size               cluster size in bytes, power of two
    ///                         512…2M; "4k"/"64K"/"1M" suffixes accepted
    ///   -p sectors            hidden sectors (partition start), default 0
    ///   -Q                    quick format (default)
    ///   -f                    full format: zero the whole volume
    ///   -C                    enable compression on the root directory
    ///
    /// Unknown options throw EINVAL. Operands are ignored.
    static func parseFormatOptions(_ args: [String]) throws -> NTFSFormatOptions {
        let parsed = try getopt(args, spec: "L:v:c:p:QfC", strict: true)
        var options = NTFSFormatOptions()
        for flag in parsed.flags {
            switch flag.letter {
            case "L", "v":
                let label = flag.value ?? ""
                guard label.utf16.count <= 32, !label.utf8.contains(0) else {
                    fsLog.error("volume label too long (max 32 UTF-16 units)")
                    throw NTFSError.posix(EINVAL)
                }
                options.label = label
            case "c":
                options.clusterSize = try clusterSize(flag.value)
            case "p":
                guard let value = flag.value, let sectors = UInt64(value) else {
                    throw NTFSError.posix(EINVAL)
                }
                options.hiddenSectors = sectors
            case "Q":
                options.quick = true
            case "f":
                options.quick = false
            case "C":
                options.enableCompression = true
            default:
                throw NTFSError.posix(EINVAL)
            }
        }
        if !parsed.operands.isEmpty {
            fsLog.debug("format: ignoring operands \(parsed.operands.joined(separator: " "), privacy: .public)")
        }
        return options
    }

    private static func clusterSize(_ value: String?) throws -> UInt32 {
        guard var text = value?.trimmingCharacters(in: .whitespaces), !text.isEmpty else {
            throw NTFSError.posix(EINVAL)
        }
        var multiplier: UInt64 = 1
        if let last = text.last?.lowercased().first {
            if last == "k" { multiplier = 1024; text.removeLast() }
            else if last == "m" { multiplier = 1024 * 1024; text.removeLast() }
        }
        guard let base = UInt64(text) else { throw NTFSError.posix(EINVAL) }
        let bytes = base.multipliedReportingOverflow(by: multiplier)
        guard !bytes.overflow,
              bytes.partialValue >= 512, bytes.partialValue <= 2 * 1024 * 1024,
              bytes.partialValue.nonzeroBitCount == 1 else {
            fsLog.error("invalid cluster size '\(value ?? "", privacy: .public)' (power of two, 512…2M)")
            throw NTFSError.posix(EINVAL)
        }
        return UInt32(bytes.partialValue)
    }

    // MARK: Check

    /// Parses check options (FSCheckOptionSyntax "nqyfl"). Unknown flags are
    /// ignored so that a new DiskArbitration flag can never block automount.
    static func parseCheckOptions(_ args: [String]) -> NTFSCheckOptions {
        var options = NTFSCheckOptions()
        guard let parsed = try? getopt(args, spec: "nqyfl", strict: false) else {
            return options
        }
        for flag in parsed.flags {
            switch flag.letter {
            case "q": options.quick = true
            case "y": options.repair = true
            case "n": options.noWrite = true
            case "f": options.force = true
            default:
                fsLog.debug("check: ignoring flag -\(String(flag.letter), privacy: .public)")
            }
        }
        if options.noWrite { options.repair = false }
        return options
    }
}

// MARK: - Check / format workers

/// Runs on a background queue; communicates only through Sendable values.
private enum NTFSMaintenance {
    /// FSKit error used when a check finds a problem it can't (or may not) fix.
    static func damaged() -> any Error {
        FSError(.resourceDamaged)
    }

    /// Check of an unmounted device.
    static func check(io: BlockDeviceIO, options: NTFSCheckOptions,
                      task: FSTask, reporter: NTFSProgressReporter) throws {
        func say(_ message: String) {
            fsLog.notice("check: \(message, privacy: .public)")
            task.logMessage(message)
        }

        reporter.update(5)
        let info = try NTFSBridge.probe(io: io)
        guard info.isNTFS else {
            say("The device does not contain an NTFS file system.")
            throw FSError(.resourceUnrecognized)
        }
        say("""
            NTFS volume "\(info.label)", serial \(String(format: "%016llX", info.serial)), \
            \(info.totalSectors) sectors of \(info.bytesPerSector) bytes, cluster \(info.clusterSize) bytes.
            """)
        reporter.update(15)

        if info.hibernated {
            say("""
                WARNING: Windows is hibernated or used Fast Startup. The volume will be mounted \
                read-only to protect the hibernated session. Shut Windows down fully (Shift+Shut Down) \
                or mount with -o remove_hiberfile to discard it.
                """)
        }
        if info.dirty {
            say("The volume is marked dirty (Windows requested a consistency check).")
        }

        // Quick mode: decide only whether a repair pass is needed.
        if options.quick {
            if info.dirty && !info.hibernated {
                say("Quick check: repair needed.")
                throw damaged()
            }
            reporter.finish()
            say("Quick check passed.")
            return
        }

        // Structural sanity check through a read-only libntfs-3g mount.
        // NOTE: libntfs-3g has no equivalent of Windows chkdsk; this verifies
        // that the boot sector, $MFT, $Volume, $Bitmap and the root index are
        // readable and consistent enough to mount, and that the root
        // directory's entries resolve. It does not scan every MFT record.
        var readOnlyOptions = NTFSMountOptions()
        readOnlyOptions.readOnly = true
        readOnlyOptions.recover = false
        let mount: NTFSMount
        do {
            mount = try NTFSMount(io: io, options: readOnlyOptions)
        } catch {
            say("The volume cannot be mounted read-only: \(error.localizedDescription)")
            throw damaged()
        }

        var unreadable = 0
        do {
            let volume = try mount.volumeInfo()
            say("""
                NTFS version \(volume.majorVersion).\(volume.minorVersion); \
                \(volume.freeClusters) of \(volume.totalClusters) clusters free; \
                \(volume.totalMFTRecords - min(volume.freeMFTRecords, volume.totalMFTRecords)) MFT records in use.
                """)
            reporter.update(35)

            let root = try mount.getattr(ntfsRootIno)
            guard root.type == .directory else {
                say("The root directory record is not a directory.")
                throw damaged()
            }

            // Collect first, then stat: the readdir body runs under the
            // bridge's volume lock and must not re-enter the bridge.
            var entries: [(name: String, ino: UInt64)] = []
            _ = try mount.readdir(dir: ntfsRootIno, cookie: 0) { name, ino, _, _ in
                if name != "." && name != ".." {
                    entries.append((name, ino))
                }
                return entries.count < 512
            }
            reporter.update(50)
            for (index, entry) in entries.enumerated() {
                do {
                    _ = try mount.getattr(entry.ino)
                } catch {
                    unreadable += 1
                    say("Cannot read \"\(entry.name)\" (MFT record \(entry.ino)): \(error.localizedDescription)")
                }
                if !entries.isEmpty {
                    reporter.update(50 + 20 * Double(index + 1) / Double(entries.count))
                }
            }
        } catch {
            try? mount.unmount(force: true)
            throw error
        }
        try? mount.unmount(force: true)
        reporter.update(70)

        if unreadable > 0 {
            say("\(unreadable) root directory entries are unreadable; run chkdsk /f on Windows.")
            throw damaged()
        }

        // Repair: libntfs-3g can reset an unclean $LogFile ("recover") and
        // clears the in-use state on a clean unmount. Never touch a
        // hibernated volume; never write with -n or without -y.
        if info.dirty && !info.hibernated {
            guard options.repair, !options.noWrite, !io.readOnly else {
                say("Repair needed; run the check with -y to reset the journal, or chkdsk /f on Windows.")
                throw damaged()
            }
            say("Resetting the NTFS journal and clearing the dirty state…")
            var repairOptions = NTFSMountOptions()
            repairOptions.recover = true
            let repairMount = try NTFSMount(io: io, options: repairOptions)
            do {
                try repairMount.sync()
                try repairMount.unmount(force: false)
            } catch {
                try? repairMount.unmount(force: true)
                throw error
            }
            reporter.update(90)
            let after = try NTFSBridge.probe(io: io)
            if after.dirty {
                say("The dirty flag is still set; the volume is usable, but run chkdsk /f on Windows when possible.")
            } else {
                say("The volume is now clean.")
            }
        }

        reporter.finish()
        say("Check complete.")
    }

    /// Check while the volume is mounted: report status from the live mount
    /// without opening a second libntfs-3g instance on the same device.
    static func checkLive(mount: NTFSMount, task: FSTask, reporter: NTFSProgressReporter) throws {
        let volume = try mount.volumeInfo()
        reporter.update(50)
        let root = try mount.getattr(ntfsRootIno)
        guard root.type == .directory else { throw damaged() }
        let message = """
            Volume "\(volume.label)" is mounted (\(volume.readOnly ? "read-only" : "read-write")); \
            NTFS \(volume.majorVersion).\(volume.minorVersion), \(volume.freeClusters) of \
            \(volume.totalClusters) clusters free. Unmount it for a full check.
            """
        fsLog.notice("check: \(message, privacy: .public)")
        task.logMessage(message)
        reporter.finish()
    }
}

// MARK: - NTFSFileSystem

final class NTFSFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations,
                            FSManageableResourceMaintenanceOperations {

    /// Everything that belongs to the currently loaded resource.
    private struct Loaded {
        let resource: FSBlockDeviceResource
        let io: BlockDeviceIO
        let volume: NTFSVolume
        let gate: NTFSResourceGate
    }

    private let lock = NSLock()
    private var loaded: Loaded?

    // MARK: FSUnaryFileSystemOperations

    func didFinishLoading() {
        fsLog.notice("NTFS module loaded (libntfs-3g \(NTFSBridge.libraryVersion, privacy: .public))")
    }

    func probeResource(resource: FSResource,
                       replyHandler reply: @escaping @Sendable (FSProbeResult?, (any Error)?) -> Void) {
        guard let device = resource as? FSBlockDeviceResource else {
            reply(FSProbeResult.notRecognized, nil)
            return
        }
        do {
            let info = try NTFSBridge.probe(io: BlockDeviceIO(resource: device, readOnly: true))
            guard info.isNTFS else {
                reply(FSProbeResult.notRecognized, nil)
                return
            }
            let containerID = FSContainerIdentifier(uuid: info.uuid)
            fsLog.info("""
                probe \(device.bsdName, privacy: .public): NTFS "\(info.label, privacy: .public)" \
                dirty=\(info.dirty, privacy: .public) hibernated=\(info.hibernated, privacy: .public)
                """)
            if info.hibernated {
                // Mountable, but only read-only until Windows is shut down fully.
                reply(FSProbeResult.usableButLimited(name: info.label, containerID: containerID), nil)
            } else {
                reply(FSProbeResult.usable(name: info.label, containerID: containerID), nil)
            }
        } catch {
            fsLog.error("probe \(device.bsdName, privacy: .public) failed: \(error.localizedDescription, privacy: .public)")
            reply(nil, error)
        }
    }

    func loadResource(resource: FSResource, options: FSTaskOptions,
                      replyHandler reply: @escaping @Sendable (FSVolume?, (any Error)?) -> Void) {
        do {
            guard let device = resource as? FSBlockDeviceResource else {
                throw FSError(.resourceUnrecognized)
            }
            if let existing = lock.withLock({ loaded }), existing.volume.liveMount() != nil {
                fsLog.error("load refused: a volume is already active")
                throw NTFSError.posix(EBUSY)
            }

            var mountOptions = NTFSMountOptions()
            try NTFSTaskOptionParser.applyMountOptions(options.taskOptions, to: &mountOptions)

            // Identify the volume. A non-NTFS (or unreadable) device still
            // loads, because FSKit loads before formatting; activation of
            // such a volume fails with resourceUnrecognized.
            var probe: NTFSProbeInfo?
            do {
                probe = try NTFSBridge.probe(io: BlockDeviceIO(resource: device, readOnly: true))
            } catch {
                fsLog.error("load: probe of \(device.bsdName, privacy: .public) failed: \(error.localizedDescription, privacy: .public)")
            }
            let ntfs = probe.flatMap { $0.isNTFS ? $0 : nil }
            if let ntfs {
                if ntfs.hibernated {
                    fsLog.warning("""
                        \(device.bsdName, privacy: .public): Windows is hibernated / Fast Startup; \
                        a read-write mount will fall back to read-only unless remove_hiberfile is given
                        """)
                }
                if ntfs.dirty {
                    fsLog.warning("\(device.bsdName, privacy: .public): volume is marked dirty")
                }
            }

            let volumeUUID = ntfs?.uuid ?? UUID()
            let label = ntfs?.label ?? ""
            let gate = NTFSResourceGate()
            let io = BlockDeviceIO(resource: device, readOnly: false)
            let volume = NTFSVolume(volumeID: FSVolume.Identifier(uuid: volumeUUID),
                                    volumeName: FSFileName(string: label.isEmpty ? "Untitled" : label),
                                    io: io,
                                    options: mountOptions,
                                    gate: gate,
                                    isNTFS: ntfs != nil)
            lock.withLock {
                loaded = Loaded(resource: device, io: io, volume: volume, gate: gate)
            }
            containerStatus = FSContainerStatus.ready
            fsLog.notice("loaded \(device.bsdName, privacy: .public) (writable=\(device.isWritable, privacy: .public), ntfs=\(ntfs != nil, privacy: .public))")
            reply(volume, nil)
        } catch {
            fsLog.error("load failed: \(error.localizedDescription, privacy: .public)")
            reply(nil, error)
        }
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions,
                        replyHandler reply: @escaping @Sendable ((any Error)?) -> Void) {
        let previous: Loaded? = lock.withLock {
            let current = loaded
            loaded = nil
            return current
        }
        // Normally FSKit deactivated the volume already; make sure no
        // libntfs-3g instance outlives the resource.
        previous?.volume.forceShutdown()
        fsLog.notice("unloaded resource")
        reply(nil)
    }

    // MARK: FSManageableResourceMaintenanceOperations

    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        let checkOptions = NTFSTaskOptionParser.parseCheckOptions(options.taskOptions)
        guard let current = lock.withLock({ loaded }) else {
            fsLog.error("check requested without a loaded resource")
            throw NTFSError.posix(ENXIO)
        }
        let io = current.io
        let gate = current.gate
        let liveMount = current.volume.liveMount()
        if liveMount == nil {
            guard gate.beginMaintenance() else { throw NTFSError.posix(EBUSY) }
        }

        let reporter = NTFSProgressReporter()
        fsLog.notice("""
            check \(current.resource.bsdName, privacy: .public) started \
            (quick=\(checkOptions.quick, privacy: .public) repair=\(checkOptions.repair, privacy: .public) \
            noWrite=\(checkOptions.noWrite, privacy: .public) live=\(liveMount != nil, privacy: .public))
            """)

        DispatchQueue.global(qos: .utility).async {
            var failure: (any Error)?
            do {
                if let liveMount {
                    try NTFSMaintenance.checkLive(mount: liveMount, task: task, reporter: reporter)
                } else {
                    try NTFSMaintenance.check(io: io, options: checkOptions, task: task, reporter: reporter)
                }
            } catch {
                failure = error
                fsLog.error("check failed: \(error.localizedDescription, privacy: .public)")
            }
            if liveMount == nil {
                gate.endMaintenance()
            }
            task.didComplete(error: failure)
        }
        return reporter.progress
    }

    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        let formatOptions = try NTFSTaskOptionParser.parseFormatOptions(options.taskOptions)
        guard let current = lock.withLock({ loaded }) else {
            fsLog.error("format requested without a loaded resource")
            throw NTFSError.posix(ENXIO)
        }
        guard !current.io.readOnly else { throw NTFSError.posix(EROFS) }
        let io = current.io
        let gate = current.gate
        guard gate.beginMaintenance() else { throw NTFSError.posix(EBUSY) }

        let deviceName = current.resource.bsdName
        let reporter = NTFSProgressReporter()
        let summary = """
            Formatting \(deviceName) as NTFS (label "\(formatOptions.label ?? "")", \
            cluster \(formatOptions.clusterSize == 0 ? "default" : "\(formatOptions.clusterSize) bytes"), \
            \(formatOptions.quick ? "quick" : "full"), compression \(formatOptions.enableCompression ? "on" : "off"))
            """
        fsLog.notice("\(summary, privacy: .public)")

        DispatchQueue.global(qos: .userInitiated).async {
            task.logMessage(summary)
            do {
                try NTFSBridge.format(io: io, options: formatOptions) { percent in
                    reporter.update(percent)
                    return !reporter.isCancelled
                }
                reporter.finish()
                gate.endMaintenance()
                fsLog.notice("format of \(deviceName, privacy: .public) complete")
                task.logMessage("Format complete.")
                task.didComplete(error: nil)
            } catch {
                gate.endMaintenance()
                fsLog.error("format of \(deviceName, privacy: .public) failed: \(error.localizedDescription, privacy: .public)")
                task.logMessage("Format failed: \(error.localizedDescription)")
                task.didComplete(error: error)
            }
        }
        return reporter.progress
    }
}
