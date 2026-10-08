# ntfs4mac — Architecture & Cross-Team Contract

Native user-space NTFS driver for macOS 15.4+ built on Apple **FSKit** and
**libntfs-3g**. This file is the contract every component is written against.

## Repository layout

```
project.yml                         XcodeGen spec (generates NTFS4Mac.xcodeproj)
NTFS4Mac/                           Host app target (required container for the extension)
  NTFS4MacApp.swift                 SwiftUI app: status + "enable in System Settings" help
  Info.plist
  NTFS4Mac.entitlements
NTFSExtension/                      FSKit app-extension target (com.apple.fskit.fsmodule)
  Info.plist                        EXAppExtensionAttributes: FSShortName (ntfs4mac), FSPersonalities, FSMediaTypes ...
  NTFSExtension.entitlements        com.apple.developer.fskit.fsmodule + sandbox
  NTFSExtensionMain.swift           @main UnaryFileSystemExtension
  NTFSFileSystem.swift              FSUnaryFileSystem: probe / load / unload / check / format
  NTFSVolume.swift                  FSVolume + Operations + ReadWriteOperations (+ XattrOperations, OpenCloseOperations)
  NTFSItem.swift                    FSItem subclass (ino + cached type)
  NTFSBridgeSwift.swift             Swift wrappers over NTFSBridge.h (NTFSMount, NTFSError, ...)
  BlockDeviceIO.swift               FSBlockDeviceResource → ntfsb_io callbacks
  NTFSExtension-Bridging-Header.h   #import "Bridge/NTFSBridge.h"
  Bridge/
    NTFSBridge.h                    FROZEN C API (see header comments)
    NTFSBridge.c                    implementation over libntfs-3g
    ntfsb_devio.c / .h              ntfs_device_operations backed by ntfsb_io (aligned bounce IO)
    mkntfs_glue.c / .h              runs mkntfs main() in-process with redirected device IO
ThirdParty/                         build output of scripts/build-libntfs3g.sh (gitignored)
  ntfs-3g/include, ntfs-3g/lib/libntfs-3g.a, ntfs-3g/lib/libmkntfs.a
scripts/
  build-libntfs3g.sh                fetch + build static, arm64 (+x86_64) libntfs-3g and libmkntfs
  dev-install.sh                    build, copy app to /Applications, register extension
Tests/bridge/                       Host-side (Linux/macOS) C self-test against a disk image
docs/                               ARCHITECTURE.md, TESTING.md, FSKIT_NOTES.md
```

## Identifiers

| Thing                          | Value                                      |
|--------------------------------|--------------------------------------------|
| Host app bundle id             | `com.vahidzekic.ntfs4mac`                  |
| Extension bundle id            | `com.vahidzekic.ntfs4mac.NTFSExtension`    |
| Xcode targets                  | `NTFS4Mac` (app), `NTFSExtension` (appex)  |
| Extension point                | `com.apple.fskit.fsmodule`                 |
| FSShortName / personality     | `ntfs4mac` / `NTFS4Mac` (FSName "NTFS")   |
| Deployment target              | macOS 15.4 (first release with public FSKit) |
| Swift language mode            | Swift 6, strict concurrency complete       |

## Data flow

```
Finder / diskutil / Disk Utility
        │  (mount, format, fsck requests)
        ▼
     fskitd  ──XPC──►  NTFSExtension.appex  (sandboxed, user space)
                          │
     NTFSFileSystem (FSUnaryFileSystem)        probe / load / format / check
                          │
     NTFSVolume (FSVolume.Operations, ReadWrite) lookup / enumerate / read / write / attrs
                          │  Swift → C (NTFSBridgeSwift.swift)
                          ▼
     NTFSBridge.c  ── per-volume mutex ──► libntfs-3g (ntfs_mount, ntfs_readdir, ntfs_attr_pread ...)
                          │  struct ntfs_device_operations (ntfsb_devio.c)
                          ▼
     ntfsb_io callbacks (BlockDeviceIO.swift) ──► FSBlockDeviceResource.read/write
                          ▼
                     /dev/diskXsY (opened by fskitd, never by us)
```

## Rules all components follow

1. **Errors.** C returns 0 / positive errno. Swift converts with
   `NTFSError.check(_:)` into `POSIXError(POSIXErrorCode(rawValue:))`, which is
   what FSKit reply handlers expect (`fs_errorForPOSIXError` equivalent).
2. **Identity.** `FSItem` identity = MFT record number (`ino`). `NTFSVolume`
   keeps a `[UInt64: NTFSItem]` cache guarded by a lock; `reclaimItem` evicts.
   Root item is `NTFSB_ROOT_INO` (5).
3. **Threading.** The bridge serialises per volume. Swift does not need extra
   locking around bridge calls, only around its own caches. FSKit callbacks
   may arrive concurrently.
4. **Alignment.** Swift `BlockDeviceIO` callbacks only ever see
   block-aligned offset/length; the C device layer bounces unaligned IO.
5. **Ownership.** NTFS has no POSIX ownership without a user mapping file.
   Every item is reported with the mount's uid/gid (`uid=`/`gid=` options,
   default 99 "unknown" so the current user gets access). Files get
   `0666 & ~fmask`, directories `0777 & ~dmask` (default masks 022); files with
   FILE_ATTR_READONLY lose their write bits and show UF_IMMUTABLE. READONLY is
   ignored on directories, as Windows uses it there only as a display marker.
6. **Format.** mkntfs is not a library. `scripts/build-libntfs3g.sh` compiles
   `ntfsprogs/{attrdef,boot,sd,mkntfs,utils}.c` into `libmkntfs.a`:
   - all five: `-DHAVE_CONFIG_H -Dexit=ntfsb_mkntfs_exit`
   - `mkntfs.c` additionally: `-DNTFSB_MKNTFS_UNIT -include
     NTFSExtension/Bridge/mkntfs_glue.h -Dmain=ntfsb_mkntfs_main`

   A command-line `-Dntfs_device_default_io_ops=...` does **not** work:
   `device_io.h` itself `#define`s that name to `ntfs_device_unix_io_ops`.
   `mkntfs_glue.h` therefore renames `ntfs_device_unix_io_ops` to
   `ntfsb_mkntfs_io_ops` and emits `ntfsb_mkntfs_reset_state()` inside
   mkntfs.c, because mkntfs keeps static state that is not re-entrant.
   `ntfsb_format()` builds an argv (`-F [-Q] [-C] [-L label] [-c size] -s
   <block size> -p <hidden> -H 255 -S 63 ntfsb-format-device <sectors>`) and
   calls `ntfsb_mkntfs_main` under a process-wide mutex.
   Tests/bridge/README.md holds the verified build recipe.
7. **Licensing.** libntfs-3g / mkntfs are GPL-2.0-or-later; the shipped
   extension must therefore be distributed under GPL-compatible terms.

## Swift wrapper API (NTFSBridgeSwift.swift) — used by NTFSVolume / NTFSFileSystem

```swift
struct NTFSError { static func check(_ rc: Int32) throws }   // throws POSIXError
struct NTFSStat: Sendable { ino, parentIno, type: NTFSItemKind, mode, nlink, uid, gid,
                            size, allocSize, atime, mtime, ctime, btime: timespec, flags: UInt32, generation }
enum NTFSItemKind: Sendable { case file, directory, symlink, unknown }
struct NTFSProbeInfo: Sendable { isNTFS, label, serial, uuid: UUID, bytesPerSector, clusterSize, totalSectors, dirty, hibernated }
struct NTFSVolumeInfo: Sendable { label, serial, uuid, clusterSize, sectorSize, totalClusters, freeClusters,
                                  totalMFTRecords, freeMFTRecords, majorVersion, minorVersion, readOnly }
struct NTFSMountOptions: Sendable { readOnly, removeHiberfile, recover, showSystemFiles, ignoreCase, uid, gid, fmask, dmask }
struct NTFSFormatOptions: Sendable { label: String?, clusterSize: UInt32, quick: Bool, enableCompression: Bool, hiddenSectors: UInt64 }
struct NTFSSetAttrRequest: Sendable { size?, mode?, uid?, gid?, atime?, mtime?, ctime?, btime?: timespec?, flags? }

enum NTFSBridge {
    static func probe(io: BlockDeviceIO) throws -> NTFSProbeInfo
    static func format(io: BlockDeviceIO, options: NTFSFormatOptions, progress: (Double) -> Bool) throws
    static var libraryVersion: String
}

final class NTFSMount: @unchecked Sendable {      // owns ntfsb_volume*, keeps BlockDeviceIO alive
    init(io: BlockDeviceIO, options: NTFSMountOptions) throws
    func unmount(force: Bool) throws
    func sync() throws
    func volumeInfo() throws -> NTFSVolumeInfo
    func setLabel(_ label: String) throws
    func getattr(_ ino: UInt64) throws -> NTFSStat
    func setattr(_ ino: UInt64, _ req: NTFSSetAttrRequest) throws -> NTFSStat
    func lookup(dir: UInt64, name: String) throws -> NTFSStat
    /// body returns false to stop; returns true when EOF reached
    func readdir(dir: UInt64, cookie: UInt64,
                 _ body: (_ name: String, _ ino: UInt64, _ kind: NTFSItemKind, _ nextCookie: UInt64) -> Bool) throws -> Bool
    func create(dir: UInt64, name: String, kind: NTFSItemKind, mode: UInt32) throws -> NTFSStat
    func symlink(dir: UInt64, name: String, target: String) throws -> NTFSStat
    func readlink(_ ino: UInt64) throws -> String
    func link(_ ino: UInt64, dir: UInt64, name: String) throws
    func unlink(dir: UInt64, name: String) throws
    func rename(srcDir: UInt64, srcName: String, dstDir: UInt64, dstName: String) throws
    func read(_ ino: UInt64, into: UnsafeMutableRawBufferPointer, offset: Int64) throws -> Int
    func write(_ ino: UInt64, from: UnsafeRawBufferPointer, offset: Int64) throws -> Int
    func truncate(_ ino: UInt64, size: UInt64) throws
}

final class BlockDeviceIO: @unchecked Sendable {  // wraps FSBlockDeviceResource
    init(resource: FSBlockDeviceResource, readOnly: Bool)
    var io: ntfsb_io { get }                      // ctx = Unmanaged.passUnretained(self)
}
```
