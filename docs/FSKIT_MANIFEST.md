# FSKit manifest reference (Info.plist, entitlements, packaging)

This file explains every key in `NTFSExtension/Info.plist`,
`NTFSExtension/NTFSExtension.entitlements`, `NTFS4Mac/NTFS4Mac.entitlements`
and the packaging in `project.yml`. For each key it gives the source and how
well that source backs it up.

**Status legend**
- **Apple doc**: stated in Apple's developer documentation.
- **Apple src**: present in an Apple-authored FSKit module (the `msdos.appex`
  `Info.plist` in apple-oss-distributions/msdosfs, msdosfs-788.0.6.0.1).
- **Apple staff**: stated by Apple DTS or Apple engineers on the Developer Forums.
- **3rd-party, tested**: used by an open-source FSKit module that reports it working.
- **Unverified**: inferred. No source confirms the exact semantics.

Apple has no reference page for the FSKit `Info.plist` keys (as of 2026-10). The
FSKit overview says only that a module defines "module attributes … in the
module's `Info.plist` … Boolean keys that indicate feature support and
dictionaries that describe command-line interface access". DTS's advice is to
start from Xcode's *macOS › File System Extension* **target** template. The keys
below match that template (as reproduced in KhaosT/FSKitSample and other
modules) and Apple's own msdos module.

## Sources

| Id | Source |
|----|--------|
| S1 | FSKit overview: https://developer.apple.com/documentation/fskit |
| S2 | Entitlement page: https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.fskit.fsmodule |
| S3 | *Building a passthrough file system* (Apple sample, macOS 26): https://developer.apple.com/documentation/fskit/building-a-passthrough-file-system |
| S4 | `FSStatFSResult.fileSystemSubType` (refers to `FSPersonalities` / `FSSubType` inside `EXAppExtensionAttributes`): https://developer.apple.com/documentation/fskit/fsstatfsresult/filesystemsubtype |
| S5 | Apple msdos FSKit module manifest: https://github.com/apple-oss-distributions/msdosfs/blob/main/msdos_appex/Info.plist (and `msdosFileSystem.entitlements` next to it) |
| S6 | Developer Forums, "How can I get the system to use my FSModule for probing?" (Apple DTS + Apple engineers on `FSMediaTypes`, `fsck_fskit`, `newfs_fskit`, `mount -F`, DiskArbitration's check requirement): https://developer.apple.com/forums/thread/786270 |
| S7 | Developer Forums, FSKit Info.plist / enablement thread (Apple engineer: enable via System Settings; DTS: use the Xcode target template, there is no official sample): https://developer.apple.com/forums/thread/776322 |
| S8 | KhaosT/FSKitSample (Xcode template output, `extensionkit-extension` product type, entitlements): https://github.com/KhaosT/FSKitSample |
| S9 | ttntfs, an FSKit NTFS module, with measured notes on `FSShortName: ntfs`, `FSProbeOrder`, macOS 26 enablement: https://github.com/dr-kbadawi/ttntfs (`fskit/project.yml`, `fskit/README.md`, `docs/TESTING.md`) |
| S10 | xntfs, an FSKit NTFS module (ntfs-3g based): https://github.com/HuanchuanTech/xntfs (`ntfs3g/Info.plist`) |
| S11 | ExtendFS, an FSKit ext2/3/4 module shipped on the Mac App Store: https://github.com/kthchew/ExtendFS |
| S12 | GHFS FSKit notes (`mount -F`, option-syntax gotchas, log predicates): https://github.com/indragiek/GHFS/blob/main/FSKIT.md |
| S13 | Developer Forums, "FSKit module mount fails with permission error on physical disks": https://developer.apple.com/forums/thread/788609 |

## Packaging

| Item | Value here | Status / source |
|------|-----------|-----------------|
| Product type | `com.apple.product-type.extensionkit-extension` (XcodeGen `type: extensionkit-extension`) | Apple template output, S8 `project.pbxproj`; DTS: FSKit is "based on modern appex technology, that is, ExtensionFoundation / ExtensionKit" (S7) |
| Embed location | `NTFS4Mac.app/Contents/Extensions/NTFSExtension.appex` (copy phase: products directory + `$(EXTENSIONS_FOLDER_PATH)`) | S8 `project.pbxproj` ("Embed ExtensionKit Extensions", `dstSubfolderSpec = 16`) |
| Entry point | Swift `@main` type conforming to `UnaryFileSystemExtension` (no `EXExtensionPrincipalClass`) | S1. DTS: "your module's main entry point must be in Swift" (S7). Apple's msdos module sets `EXExtensionPrincipalClass` because it predates the public API, so we don't copy it. |
| Not a System Extension | `systemextensionsctl` does not manage it | S1/S7: an app extension, enabled under Login Items & Extensions |

## `EXAppExtensionAttributes`

| Key | Our value | Meaning | Status / source |
|-----|-----------|---------|-----------------|
| `EXExtensionPointIdentifier` | `com.apple.fskit.fsmodule` | ExtensionKit extension point for FSKit modules | Apple src S5; S8; S3 |
| `FSShortName` | `ntfs4mac` | File system type name for `mount -t`, `fsck_fskit -t`, `newfs_fskit -t`. Disk Arbitration also records it as the volume kind | Apple doc S3 ("the `FSShortName` key … provides the `passthrough` name that serves as a file system type when using the `mount` command"); S5 (`msdos`). See the decision below. |
| `FSSupportsBlockResources` | `true` | Module accepts `FSBlockDeviceResource` (a `/dev/diskXsY`) | S5, S8, S9, S10 |
| `FSSupportsPathURLs` | `false` | Module accepts `FSPathURLResource` (directory paths, as in the passthrough sample) | S8, S10, S11 (template key). Semantics inferred from S3 |
| `FSSupportsGenericURLResources` | `false` | Module accepts `FSGenericURLResource` | S8, S11 (template key) |
| `FSSupportsServerURLs` | `false` | Network/server URLs (not supported by FSKit's unary model) | S5, S8 |
| `FSRequiresSecurityScopedPathURLResources` | `false` | Path resources arrive as security-scoped URLs. Only matters with path URLs | S8, S11 (template key); S12 uses `true` for a path-based FS |
| `FSSupportsKernelOffloadedIO` | `false` | Module implements `FSVolumeKernelOffloadedIOOperations` (kernel does data IO via extents). We do all IO through libntfs-3g in user space | S5 (`true` for msdos); S9 (`false`) |
| `FSActivateOptionSyntax` → `shortOptions` | `o:rwu:g:` | getopt string for options passed when a volume is loaded/activated (the `mount` path) | S5 (`u:g:m:o:`), S8 template (`g:m:o:u:`), S9 (`o:`, automount tested). We add `-r`/`-w` and `-u`/`-g` (template-style uid/gid) because the Swift parser handles them. S12: the key must be present or probing breaks. **Unverified**: exactly which `mount -o` words reach the module. Standard words such as `rdonly` may be turned into FSKit's own `--rdonly` flag instead (S12: "Only `-f` and `--rdonly` are available" for load). |
| `FSCheckOptionSyntax` → `shortOptions` | `nqyfl` | getopt string for `fsck_fskit` / DiskArbitration checks | S5 (`pynfqM:`), S8 (`nqy`). **Apple staff (S6)**: DiskArbitration needs `-q` (quick) and `-y` (repair), and requires a successful check before it automounts. |
| `FSFormatOptionSyntax` → `shortOptions` | `L:v:c:p:QfC` | getopt string for `newfs_fskit` (format task) | Key: S5 (`NB:F:…v:`), S8 (`v`). Letters are ours. `-v name` is an extra alias for `-L` because Apple's formatters (`newfs_msdos -v`) and the template use `v`. `-p` = hidden sectors (partition offset written to the boot sector). **Unverified** whether any Apple UI passes `-v`. |
| `FSPersonalities` | one entry, key `NTFS4Mac` | Named variants of the file system | S4 (Apple doc names `FSPersonalities` inside `EXAppExtensionAttributes`), S5, S8 |
| ↳ `FSName` | `NTFS` | Human-readable personality name | S5 (`MS-DOS (FAT32)`), S8; legacy meaning in `fsproperties.h` (`kFSNameKey`) |
| ↳ `FSSubType` | `0` | Must match `FSStatFSResult.fileSystemSubType` the volume reports | Apple doc S4; S5 |
| ↳ `FSfileObjectsAreCaseSensitive` | `false` | Name-case semantics FSKit assumes for the volume | S8, S10 (`false`), S11 (`true`). **Unverified semantics.** `false` matches the Swift volume's default (`ignoreCase = true`, capability `.insensitiveCasePreserving`, like Windows). `-o case_sensitive` switches one mount to case-sensitive; the static plist value can't follow that. |
| ↳ `FSFormatContentMask` | `Windows_NTFS` | Partition content type a formatter would request (MBR 0x07 hint) | S5 (`DOS_FAT_32`), S9. **Unverified** whether any FSKit format path reads it today. |
| ↳ `FSFormatMinimumSize` / `FSFormatMaximumSize` | 1 MiB / 2^48 B (256 TiB) | Size limits shown or enforced by format UIs | S5, S9. **Unverified** for FSKit format |
| `FSMediaTypes` | `Windows_NTFS`, `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7`, `Partitionless` | Which IOMedia objects Disk Arbitration offers to the module for probing. Matched on the IOKit properties in `FSMediaProperties` | Apple src S5 uses the GUID key plus `Partitionless`. Forum OP (S6) needed it for probing. S9/S10 use the same NTFS set |
| ↳ `FSMediaProperties` → `Content Hint` | `Windows_NTFS` (MBR type 0x07); `EBD0A0A2-…` (GPT "Microsoft Basic Data") | IOMedia `Content Hint` to match | S5 (`EBD0A0A2-…`, `Windows_FAT_32`, …), S6 |
| ↳ `FSMediaProperties` → `Leaf` | `true` | Only leaf media (partitions or unpartitioned disks), never a partition-map container | S5, S6 |
| ↳ `FSMediaProperties` → `Whole` | `true` (in `Partitionless` only) | The whole disk, no partition map | S5 |
| ↳ `FSProbeOrder` | 500 / 1500 / 3500 | Lower is probed earlier. Apple's msdos uses 1000/2000/4000, and ttntfs reports Apple's `ntfs.fs` at 1000 | S5. Ordering semantics inferred from S5 values; specific numbers from S9 (tested). |

We left out these msdos keys on purpose: `FSProbeExecutable`, `FSProbeArguments`,
`FSFormatExecutable`, `FSMountExecutable`, `FSRepairExecutable`,
`FSVerificationExecutable` (and their argument keys), plus `autodiskmount`. They
name helper binaries in Apple's legacy `/System/Library/Filesystems/*.fs`
bundles. A third-party FSKit module does probe/check/format inside the
extension (`FSUnaryFileSystemOperations`, `FSManageableResourceMaintenanceOperations`).

The user's request used the names `FSSupportedMediaTypes`, `FSMediaTypeFormatable` and
`FSMediaTypeMountable`. None of these appear in any Apple module, the Xcode
template or any FSKit project we found, so they are not used. The system reads
`FSMediaTypes`, and the format/mount capabilities come from the option-syntax
keys and the protocols the module implements.

## FSShortName and personality: decision

**Choice: `FSShortName = "ntfs4mac"`, personality key `NTFS4Mac`, `FSName = "NTFS"`.**

Why not `FSShortName = "ntfs"`:
1. macOS ships its own read-only NTFS driver, `/System/Library/Filesystems/ntfs.fs`
   (a kext-backed bundle). An Apple engineer (S6) says that when a kext and an
   FSKit module share a short name, "any existing KEXT implementations are
   preferred over FSKit implementations", and only `mount -F` forces FSKit.
   So `mount -t ntfs` and automount would keep picking Apple's read-only driver.
2. ttntfs measured it on macOS 26 (S9, `docs/TESTING.md`, 2026-09-23). With
   `FSShortName: ntfs` the module loads and mounts internally, then `fskitd`
   refuses the mount with `ECONNREFUSED` ("Could not get file provider
   connection"). The bad state persists until `fskitd` is restarted. Their
   summary: "the name is genuinely taken".
3. A unique short name keeps `mount -t ntfs4mac` unambiguous, and `-F` becomes
   optional (S6: with an FSKit-only name, `mount(8)` selects FSKit).

What it costs ("NTFS" in Disk Utility):
- **Volume kind label.** Disk Arbitration records the module's `FSShortName`
  as the volume kind. `diskutil` resolves that name against the bundles in
  `/System/Library/Filesystems`, and `ntfs4mac` matches none of them. So
  mounted volumes may show as "Unknown" (Disk Utility) or by partition-type
  guess (`diskutil info`) rather than "Windows NT File System (NTFS)" (S9,
  measured). The `statfs` type name (`f_fstypename`) comes from the volume's
  `FSStatFSResult` and should be `"ntfs"` (Swift engineer), so `mount`, `df`
  and Finder's Get Info show NTFS.
- **Format menu.** We found no evidence that Disk Utility's Format pop-up or
  `diskutil eraseVolume` lists third-party FSKit personalities on macOS 15/26.
  DTS (S6): "`diskutil` does not currently have full integration with FSKit;
  you can test/use `startFormat(task:options:)` by running `/sbin/newfs_fskit`".
  The FSKit formats Disk Utility does show (ExFAT, MS-DOS) come from Apple's
  own `*.fs` bundles, whose personalities declare `FSFormatExecutable`.
  Picking `ntfs` would not change this: Apple's `ntfs.fs` has no formatter, so
  its personality is never in the Format menu either. We still set
  `FSName = "NTFS"` and a format content mask, so that **if** Disk Utility starts
  listing FSKit personalities it shows plain "NTFS" and nothing named "NTFS4Mac".
  **Unverified**: run `diskutil listFilesystems` on the target OS. If an
  `NTFS4Mac`/`NTFS` row appears, `diskutil eraseVolume NTFS4Mac <label> diskXsY`
  is the command line.
- **Personality key `NTFS4Mac`, not `NTFS`.** `diskutil` matches personalities
  by name across every installed file system. Apple's `ntfs.fs` uses `NTFS` as
  its personality key (**unverified**; check
  `/System/Library/Filesystems/ntfs.fs/Contents/Info.plist`), so a second `NTFS`
  key could resolve to Apple's read-only driver. xntfs (S10) uses key `NTFS`;
  ttntfs (S9) uses a unique key.

Summary: "NTFS" appears wherever FSKit uses the personality's `FSName` and
wherever the volume's statfs type is shown. It does not appear in Disk
Utility's Format pop-up, because no FSKit short name can achieve that today.
Formatting goes through `newfs_fskit -t ntfs4mac` (see docs/TESTING.md).

## Declared option syntax (contract for the Swift engineer)

| Task | getopt string | Options |
|------|---------------|---------|
| Activate / mount | `o:rwu:g:` | `-o rdonly,remove_hiberfile,recover,show_sys_files,ignore_case,uid=N,gid=N` (comma-separated, repeatable; `ro`, `rw`, `norecover`, `hide_sys_files`, `case_sensitive`, `fmask=`, `dmask=`, `umask=` also accepted; unknown words such as `nobrowse` are ignored) · `-r` / `-w` read-only / read-write · `-u uid` · `-g gid`. FSKit's own `--rdonly` and `-f` may also arrive (S3, S12). |
| Check | `nqyfl` | `-n` check only, no writes · `-q` quick check: boot sector valid and `$Volume` not dirty/hibernated (DiskArbitration) · `-y` repair (DiskArbitration) · `-f` force a check even if clean · `-l` accepted and ignored |
| Format | `L:v:c:p:QfC` | `-L label` (`-v label` alias) · `-c clustersize` (bytes, power of two 512 … 2 MiB) · `-p sectors` hidden sectors · `-Q` quick (**default**) · `-f` full (zero the volume) · `-C` enable compression |

The strings come from `NTFSExtension/NTFSFileSystem.swift`'s parsers, which
are the authority; the plist must match them exactly.
These map onto `NTFSMountOptions` / `NTFSFormatOptions` in docs/ARCHITECTURE.md.

## Entitlements

| Key | Where | Status / source |
|-----|-------|-----------------|
| `com.apple.developer.fskit.fsmodule` = `true` | extension | Apple doc S2 ("indicates an extension provides an FSKit filesystem"); DTS (S6 / forum): modules must be signed with it. The user's `com.apple.developer.fskit` is not a real key. |
| `com.apple.security.app-sandbox` = `true` | extension | S5, S8. Template default |
| `com.apple.security.app-sandbox` = `true` | host app | Our choice. The app needs no file access |
| `com.apple.security.temporary-exception.mach-lookup.global-name` = `[com.apple.filesystems.fskitd]` | host app | S8's host app. Lets the sandboxed app call `FSClient.fetchInstalledExtensions`. **Unverified** that it is strictly required. Temporary exceptions are not accepted on the Mac App Store, so drop it there |
| `com.apple.developer.fskit.mount` | not used | Listed under FSKit entitlements (S1), used by xntfs for an app-initiated mount API on macOS 27. Not needed here |
| App groups | not used | Nothing is shared between the app and extension. On macOS 15+ a group container also needs a provisioning profile that authorizes it (S9), and S12 reports app groups not working for an FSKit appex |

**Getting the entitlement.** `com.apple.developer.fskit.fsmodule` is
provisioned through the App ID's **FSKit Module** capability, and Xcode's
automatic signing adds it to the development profile. ttntfs (S9) describes it
as a managed capability available to paid developer accounts with no request
form. ExtendFS (S11) ships on the Mac App Store, so ordinary teams can get it.
**Unverified**: no Apple page says whether a free Personal Team can use it.
Ad-hoc signed modules are rejected by `fskitd` (S9).
