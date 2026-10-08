# FSKit API notes (ntfs4mac)

Every FSKit API the extension uses, with the Swift signature as published
in Apple's documentation (fetched 2026-10-08 from the `.md` renderings of
`https://developer.apple.com/documentation/fskit/...`), plus what we could
not verify. A real-world cross-check is KhaosT/FSKitSample
(<https://github.com/KhaosT/FSKitSample>), which builds against the macOS 15.4 SDK.

Deployment target is macOS 15.4, so the module uses the **reply-handler**
form of the `FSVolume.Operations` protocol family. Apple's docs now mark
those protocols deprecated in favour of `FSVolume.Handler` (macOS 27.0+).
Moving to `Handler` would require raising the deployment target. All reply
handlers are `@escaping @Sendable`. Our implementations do their work
synchronously before replying, so no non-Sendable FSKit object (`FSItem`,
`FSVolume`, `FSFileName`, `FSItem.Attributes`, ...) crosses an isolation
boundary.

Base URL below: `D = https://developer.apple.com/documentation/fskit`

## Extension entry point

| API | Verified signature | Source |
|---|---|---|
| `UnaryFileSystemExtension` | `protocol UnaryFileSystemExtension : AppExtension` | D/unaryfilesystemextension |
| `fileSystem` | `var fileSystem: Self.FileSystem` | same |
| `FileSystem` | `associatedtype FileSystem : FSUnaryFileSystem, FSUnaryFileSystemOperations` | same |

## File system (`NTFSFileSystem`)

| API | Verified signature | Source |
|---|---|---|
| `FSUnaryFileSystem` | `class FSUnaryFileSystem` (conforms to `FSFileSystemBase`) | D/fsunaryfilesystem |
| `containerStatus` | `@NSCopying var containerStatus: FSContainerStatus { get set }` | D/fsfilesystembase/containerstatus |
| `FSContainerStatus.ready` | `class var ready: FSContainerStatus { get }` | D/fscontainerstatus |
| `probeResource` | `func probeResource(resource: FSResource, replyHandler reply: @escaping @Sendable (FSProbeResult?, (any Error)?) -> Void)` | D/fsunaryfilesystemoperations/proberesource(resource:replyhandler:) |
| `loadResource` | `func loadResource(resource: FSResource, options: FSTaskOptions, replyHandler reply: @escaping @Sendable (FSVolume?, (any Error)?) -> Void)` | D/fsunaryfilesystemoperations/loadresource(resource:options:replyhandler:) |
| `unloadResource` | `func unloadResource(resource: FSResource, options: FSTaskOptions, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void)` | D/fsunaryfilesystemoperations/unloadresource(resource:options:replyhandler:) |
| `didFinishLoading` | `optional func didFinishLoading()` | D/fsunaryfilesystemoperations/didfinishloading() |
| `FSProbeResult.usable` | `class func usable(name: String, containerID: FSContainerIdentifier) -> Self` | D/fsproberesult |
| `FSProbeResult.usableButLimited` | `class func usableButLimited(name: String, containerID: FSContainerIdentifier) -> Self` | same |
| `FSProbeResult.recognized` | `class func recognized(name: String, containerID: FSContainerIdentifier) -> Self` (not used) | same |
| `FSProbeResult.notRecognized` | `class var notRecognized: FSProbeResult { get }` | same |
| `FSContainerIdentifier(uuid:)` | inherited `init(uuid: UUID)` from `FSEntityIdentifier` | D/fsentityidentifier |
| `FSManageableResourceMaintenanceOperations` | `protocol FSManageableResourceMaintenanceOperations : NSObjectProtocol` | D/fsmanageableresourcemaintenanceoperations |
| `startCheck` | `func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress` | D/fsmanageableresourcemaintenanceoperations/startcheck(task:options:) |
| `startFormat` | `func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress` | D/fsmanageableresourcemaintenanceoperations/startformat(task:options:) |
| `FSTask.logMessage` | `func logMessage(_ str: String)` | D/fstask/logmessage(_:) |
| `FSTask.didComplete` | `func didComplete(error: (any Error)?)` | D/fstask/didcomplete(error:) |
| `FSTask` | `Sendable` (it may be captured by the worker closure) | D/fstask |
| `FSTaskOptions.taskOptions` | `var taskOptions: [String] { get }` | D/fstaskoptions |

How check and format behave, as an Apple DTS engineer explained in
<https://developer.apple.com/forums/thread/786270>:

- `startCheck` and `startFormat` run after `loadResource`, which supplies the
  resource. This is why our `loadResource` never mounts and accepts non-NTFS
  devices.
- Options are parsed synchronously. A bad option throws.
- The work itself runs on another thread. It ends with `task.didComplete(error:)`.
- DiskArbitration runs `-q` first. If that fails, it runs `-y`, and it mounts
  only when the check succeeds.
- To test, use `fsck_fskit -t <shortname>` and `newfs_fskit -t <shortname>`.

`FSTask.cancellationHandler` exists only on macOS 26.0 and later, so we don't
use it. We detect cancellation through `Progress.isCancelled`.

## Block device (`BlockDeviceIO`)

| API | Verified signature | Source |
|---|---|---|
| `bsdName` | `var bsdName: String` | D/fsblockdeviceresource |
| `isWritable` | `var isWritable: Bool { get }` | same |
| `blockSize` | `var blockSize: UInt64 { get }` (logical) | same |
| `blockCount` | `var blockCount: UInt64` | same |
| `physicalBlockSize` | `var physicalBlockSize: UInt64` (not used) | same |
| `read` | `func read(into: UnsafeMutableRawBufferPointer, startingAt: off_t, length: Int) throws -> Int` | D/fsblockdeviceresource/read(into:startingat:length:)-4ax6s |
| `write` | `func write(from: UnsafeRawBufferPointer, startingAt: off_t, length: Int) throws -> Int` | D/fsblockdeviceresource/write(from:startingat:length:)-2fmgt |
| `metadataFlush` | `func metadataFlush() throws` | D/fsblockdeviceresource/metadataflush() |

Apple warns: "Don't mix direct I/O operations (read/write) with metadata
operations (metadataRead/metadataWrite/delayedMetadataWrite) on the same
range." We use direct I/O only.

## Volume (`NTFSVolume`)

| API | Verified signature | Source |
|---|---|---|
| `FSVolume.init` | `init(volumeID: FSVolume.Identifier, volumeName: FSFileName)` | D/fsvolume/init(volumeid:volumename:) |
| `FSVolume.name` | `@NSCopying var name: FSFileName { get set }` | D/fsvolume/name |
| `FSVolume.Identifier(uuid:)` | inherited `init(uuid: UUID)` | D/fsentityidentifier |
| `activate` | `func activate(options: FSTaskOptions, replyHandler reply: @escaping @Sendable (FSItem?, (any Error)?) -> Void)` | D/fsvolume/operations/activate(options:replyhandler:) |
| `deactivate` | `func deactivate(options: FSDeactivateOptions = [], replyHandler reply: @escaping @Sendable ((any Error)?) -> Void)` | D/fsvolume/operations/deactivate(options:replyhandler:) |
| `FSDeactivateOptions.force` | `static var force: FSDeactivateOptions` | D/fsdeactivateoptions |
| `mount` | `func mount(options: FSTaskOptions, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void)` | D/fsvolume/commonoperations/mount(options:replyhandler:) |
| `unmount` | `func unmount(replyHandler reply: @escaping @Sendable () -> Void)` | D/fsvolume/commonoperations/unmount(replyhandler:) |
| `synchronize` | `func synchronize(flags: FSSyncFlags, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void)` | D/fsvolume/commonoperations/synchronize(flags:replyhandler:) |
| `reclaimItem` | `func reclaimItem(_ item: FSItem, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void)` | D/fsvolume/commonoperations/reclaimitem(_:replyhandler:) |
| `supportedVolumeCapabilities` | `var supportedVolumeCapabilities: FSVolume.SupportedCapabilities { get }` | D/fsvolume/commonoperations/supportedvolumecapabilities |
| `volumeStatistics` | `var volumeStatistics: FSStatFSResult { get }` | D/fsvolume/commonoperations/volumestatistics |
| `getAttributes` | `func getAttributes(_ desiredAttributes: FSItem.GetAttributesRequest, of item: FSItem, replyHandler reply: @escaping @Sendable (FSItem.Attributes?, (any Error)?) -> Void)` | D/fsvolume/operations/getattributes(_:of:replyhandler:) |
| `setAttributes` | `func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on item: FSItem, replyHandler reply: @escaping @Sendable (FSItem.Attributes?, (any Error)?) -> Void)` | D/fsvolume/operations/setattributes(_:on:replyhandler:) |
| `lookupItem` | `func lookupItem(named name: FSFileName, inDirectory directory: FSItem, replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void)` | D/fsvolume/operations/lookupitem(named:indirectory:replyhandler:) |
| `readSymbolicLink` | `func readSymbolicLink(_ item: FSItem, replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void)` | D/fsvolume/operations/readsymboliclink(_:replyhandler:) |
| `createItem` | `func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem, attributes newAttributes: FSItem.SetAttributesRequest, replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void)` | D/fsvolume/operations/createitem(named:type:indirectory:attributes:replyhandler:) |
| `createSymbolicLink` | `func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem, attributes newAttributes: FSItem.SetAttributesRequest, linkContents contents: FSFileName, replyHandler reply: @escaping @Sendable (FSItem?, FSFileName?, (any Error)?) -> Void)` | D/fsvolume/operations/createsymboliclink(...) |
| `createLink` | `func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem, replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void)` | D/fsvolume/operations/createlink(to:named:indirectory:replyhandler:) |
| `removeItem` | `func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void)` | D/fsvolume/operations/removeitem(_:named:fromdirectory:replyhandler:) |
| `renameItem` | `func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName, to destinationName: FSFileName, inDirectory destinationDirectory: FSItem, overItem: FSItem?, replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void)` | D/fsvolume/operations/renameitem(...) |
| `enumerateDirectory` | `func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie, verifier: FSDirectoryVerifier, attributes: FSItem.GetAttributesRequest?, packer: FSDirectoryEntryPacker, replyHandler reply: @escaping @Sendable (FSDirectoryVerifier, (any Error)?) -> Void)` | D/fsvolume/operations/enumeratedirectory(...) |
| `packEntry` | `func packEntry(name: FSFileName, itemType: FSItem.ItemType, itemID: FSItem.Identifier, nextCookie: FSDirectoryCookie, attributes: FSItem.Attributes?) -> Bool` | D/fsdirectoryentrypacker |
| `FSDirectoryCookie` | `init(_ rawValue: UInt64)`, `init(rawValue:)`, `static let initial` | D/fsdirectorycookie |
| `FSDirectoryVerifier` | `init(_:)`, `init(rawValue:)`, `static let initial` | D/fsdirectoryverifier |
| PathConf | `var maximumLinkCount: Int`, `var maximumNameLength: Int`, `var restrictsOwnershipChanges: Bool`, `var truncatesLongNames: Bool`, `optional var maximumFileSize: UInt64` | D/fsvolume/pathconfoperations |
| `read` | `func read(from item: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer, replyHandler reply: @escaping @Sendable (Int, (any Error)?) -> Void)` | D/fsvolume/readwriteoperations/read(from:at:length:into:replyhandler:) |
| `write` | `func write(contents: Data, to item: FSItem, at offset: off_t, replyHandler reply: @escaping @Sendable (Int, (any Error)?) -> Void)` | D/fsvolume/readwriteoperations/write(contents:to:at:replyhandler:) |
| `FSMutableFileDataBuffer` | `var length: Int`, `func withUnsafeMutableBytes<R, E>(_ body: (UnsafeMutableRawBufferPointer) throws(E) -> R) throws(E) -> R` | D/fsmutablefiledatabuffer |
| `openItem` | `func openItem(_ item: FSItem, modes: FSVolume.OpenModes, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void)` | D/fsvolume/opencloseoperations |
| `closeItem` | `func closeItem(_ item: FSItem, modes: FSVolume.OpenModes, replyHandler reply: @escaping @Sendable ((any Error)?) -> Void)` | same |
| `FSVolume.OpenModes` | OptionSet with `.read`, `.write` | D/fsvolume/openmodes |
| `setVolumeName` | `func setVolumeName(_ name: FSFileName, replyHandler reply: @escaping @Sendable (FSFileName?, (any Error)?) -> Void)` (protocol `FSVolume.RenameOperations`) | D/fsvolume/renameoperations |

## Items, attributes, names

| API | Verified | Source |
|---|---|---|
| `FSItem.Identifier` | `enum`. Cases `.invalid`, `.parentOfRoot`, `.rootDirectory`. `init?(rawValue: UInt64)` (15.4). `init(_:)` is `@backDeployed(before: macOS 27.0)`, so we use `init?(rawValue:)` | D/fsitem/identifier |
| `FSItem.ItemType` | `.file, .directory, .symlink, .fifo, .charDevice, .blockDevice, .socket, .unknown` | D/fsitem/itemtype |
| `FSItem.Attribute` | `OptionSet`. Members `.fileID, .parentID, .type, .mode, .linkCount, .uid, .gid, .flags, .size, .allocSize, .supportsLimitedXAttrs, .inhibitKernelOffloadedIO, .accessTime, .modifyTime, .changeTime, .birthTime, .backupTime, .addedTime` | D/fsitem/attribute |
| `FSItem.Attributes` | the properties above (timespec times), `func isValid(_ attribute: FSItem.Attribute) -> Bool` | D/fsitem/attributes |
| `FSItem.GetAttributesRequest` | `var wantedAttributes`, `func isAttributeWanted(_ attribute: FSItem.Attribute) -> Bool` | D/fsitem/getattributesrequest |
| `FSItem.SetAttributesRequest` | subclass of `Attributes`, `var consumedAttributes: FSItem.Attribute { get set }`, `wasAttributeConsumed(_:)` | D/fsitem/setattributesrequest |
| `FSFileName` | `convenience init(string name: String)`, `var string: String?`, `var data: Data` | D/fsfilename |
| `FSStatFSResult` | `init(fileSystemTypeName: String)`. Settable: `blockSize: Int`, `ioSize: Int`, `totalBlocks/availableBlocks/freeBlocks/usedBlocks/totalBytes/availableBytes/freeBytes/usedBytes/totalFiles/freeFiles: UInt64`, `fileSystemSubType: Int` | D/fsstatfsresult |
| `FSVolume.SupportedCapabilities` | `supportsPersistentObjectIDs, supports64BitObjectIDs, supportsSymbolicLinks, supportsHardLinks, supportsJournal, supportsActiveJournal, supportsSparseFiles, supportsHiddenFiles, supports2TBFiles, ...`, `caseFormat: FSVolume.CaseFormat` (`.sensitive`, `.insensitive`, `.insensitiveCasePreserving`) | D/fsvolume/supportedcapabilities |
| `FSError` | struct. `FSError.Code`: `.invalidDirectoryCookie, .moduleLoadFailed, .resourceDamaged, .resourceUnrecognized, .resourceUnusable, .statusOperationInProgress, .statusOperationPaused` | D/fserror |
| `fs_errorForPOSIXError` | `func fs_errorForPOSIXError(_: Int32) -> any Error` (we pass `POSIXError` directly instead, which gives the same NSPOSIXErrorDomain error) | D/fs_errorforposixerror(_:) |

Semantics we follow from the docs:

- **enumerateDirectory.** The cookie and verifier start at `.initial`. Reply
  with a non-zero verifier. When `attributes == nil`, pack "." and "..". When
  attributes are requested, don't pack them. If the cookie is invalid,
  reply with `invalidDirectoryCookie`.
- **lookupItem.** The reply may return a different `FSFileName` (for example,
  the stored case). A missing item replies with `ENOENT`.
- **setAttributes.** Size changes on directories and symlinks are ignored
  silently. Attributes the format can't store are left unconsumed.
- **renameItem.** `overItem` is marked deleted.
- **read.** Reading past EOF returns 0, not an error.
- **createItem.** Only `.file` and `.directory` are passed. An existing name
  replies with `EEXIST`, which the bridge returns.

## Info.plist option syntax (for the config engineer)

```xml
<key>FSActivateOptionSyntax</key> <dict><key>shortOptions</key><string>o:rwu:g:</string></dict>
<key>FSCheckOptionSyntax</key>    <dict><key>shortOptions</key><string>nqyfl</string></dict>
<key>FSFormatOptionSyntax</key>   <dict><key>shortOptions</key><string>L:v:c:p:QfC</string></dict>
```

- **Mount** (both `loadResource` and `activate` options are parsed and merged):
  `-o list`, `-r`, `-w`, `-u uid`, `-g gid`. Bare option-list operands are
  also accepted.
  - List entries: `rdonly|ro`, `rw`, `remove_hiberfile`,
    `recover|norecover` (default recover), `show_sys_files|hide_sys_files`,
    `ignore_case` (default) `|case_sensitive|noignore_case`, `uid=N`, `gid=N`,
    `fmask=OCT`, `dmask=OCT`, `umask=OCT`.
  - Unknown entries are ignored.
  - Defaults: uid/gid 99, fmask/dmask 022.
- **Check:**
  - `-q`: quick. Fails only if the volume is dirty and not hibernated.
  - `-y`: repair. Resets the journal and clears dirty through a recover mount.
  - `-n`: no writes.
  - `-f`: force. Accepted; the full check always runs without `-q`.
  - `-l`: ignored.
  - Unknown flags are ignored, so they never block automount.
- **Format:**
  - `-L label` or `-v label`: label. `-v` is the newfs_* convention.
  - `-c size`: cluster size, a power of two from 512 to 2M. `k` and `m`
    suffixes are accepted.
  - `-p sectors`: hidden sectors.
  - `-Q`: quick, the default.
  - `-f`: full (zeroing).
  - `-C`: compression.
  - Unknown flags fail with `EINVAL`.

The `FSPersonalities` entry should declare `FSfileObjectsAreCaseSensitive = false`
to match the default `ignore_case`.

## Not verifiable here (no macOS SDK / swiftc on the build host)

1. **Not compiled.** No Swift toolchain was available, so nothing was compiled.
   The signatures above come from Apple's docs, not from the SDK headers.
2. **Where mount options arrive.** We don't know whether `mount -o …` options
   arrive in `loadResource` or `activate` `taskOptions`, or how they are
   tokenised. We parse both and accept `-o list`, `-olist` and bare lists.
3. **Sendable C structs.** We assume `timespec` and other imported C structs are
   implicitly `Sendable`, which `NTFSStat: Sendable` depends on. If they are
   not, add `extension timespec: @retroactive @unchecked Sendable {}`.
4. **Progress.** `Progress` sendability is avoided by wrapping it in
   `NTFSProgressReporter` (`@unchecked Sendable`).
5. **Device flush.** `metadataFlush()` is used as the device flush for
   `ntfsb_io.sync`. FSKit has no documented "flush device write cache" call
   for direct I/O.
6. **invalidDirectoryCookie domain.** The docs say to fail with "domain
   NSPOSIXErrorDomain and code `FSError.Code.invalidDirectoryCookie`", which
   is self-contradictory. We throw `FSError(.invalidDirectoryCookie)`, which
   uses the FSKit domain.
7. **Item reclaim race.** `FSItem.tryReclaim(_:)` (the official fix for the
   reclaim/lookup race) is macOS 27+ and is not used. We evict a cache entry
   only if it is still the same object.
8. **Read-only fallback.** `FSVolume.MountOptions` and `requestedMountOptions`
   (macOS 26.4+) are not used. A volume that falls back to read-only is still
   mounted read-write by the kernel; mutating calls fail with `EROFS`.
9. **Type name clash.** `FSStatFSResult.fileSystemTypeName` is "ntfs", which
   collides with Apple's built-in read-only `ntfs` file system name. Choose
   `FSShortName` with that in mind.
10. **Bridge semantics we assumed** (the header doesn't state them):
    - `ntfsb_unmount(force: true)` frees the volume even when it returns an error.
    - The `ntfsb_format` progress closure returns `true` to continue.
    - `ntfsb_lookup` is not required to handle "." and "..". The volume
      resolves those itself.
11. **UF_IMMUTABLE.** It is reported for READONLY files only, not for
    directories. Windows uses READONLY on folders as a customisation marker.
