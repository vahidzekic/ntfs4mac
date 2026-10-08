# ntfs4mac

A native, user-space **read/write NTFS driver for macOS 15.4+**, built as an
Apple **FSKit** file-system extension around **libntfs-3g** (the engine behind
ntfs-3g). It lets Finder, `diskutil`, Disk Utility and `mount` use Windows NTFS
volumes for reading, writing and formatting. It does not need a kernel
extension or macFUSE, and you do not have to lower system security to use it.

> **Status:** Work in progress. Not yet built on real hardware.
>
> **Data-loss warning:** Writing to NTFS from a non-Windows implementation
> always carries some risk, and this project is new and has not been
> battle-tested. **Back up everything** before you mount a volume with
> important data read/write. Never write to a volume that Windows left
> hibernated or "Fast Startup" suspended. Run `chkdsk` from Windows whenever
> a volume is marked dirty. If you are unsure, use the `rdonly` mount option.

---

## Table of contents

- [Features](#features)
- [Known limitations](#known-limitations)
- [Why FSKit?](#why-fskit)
- [Architecture](#architecture)
  - [Data flow](#data-flow)
  - [Components](#components)
  - [Key design decisions](#key-design-decisions)
- [Requirements](#requirements)
- [Quick start](#quick-start)
  - [1. Build libntfs-3g](#1-build-libntfs-3g)
  - [2. Generate the Xcode project](#2-generate-the-xcode-project)
  - [3. Build and install](#3-build-and-install)
  - [4. Enable the extension](#4-enable-the-extension)
  - [5. Use it](#5-use-it)
- [Mount options](#mount-options)
- [Format options](#format-options)
- [Debugging](#debugging)
- [Troubleshooting / FAQ](#troubleshooting--faq)
- [Project layout](#project-layout)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)
- [Credits](#credits)

---

## Features

| Feature | Notes |
|---|---|
| **Read and write** | Reads and writes file data through the unnamed `$DATA` stream. Files grow as you write to them and can be truncated or extended. |
| **Create / rename / delete** | Files and directories. Rename follows POSIX semantics: it replaces an existing destination, returns `ENOTEMPTY` for a non-empty destination directory, and returns `EINVAL` when you move a directory into its own subtree. |
| **Hard links** | NTFS natively supports multiple names per MFT record. `nlink` is reported. |
| **Symbolic links** | Stored as Windows-compatible **NTFS reparse points**. Existing symlinks, junctions and WSL symlinks are reported as symlinks. |
| **Timestamps and flags** | Supports access, modify, change and birth (creation) times. The `READONLY` attribute maps to `UF_IMMUTABLE` and `HIDDEN` maps to `UF_HIDDEN`. `SYSTEM` and `ARCHIVE` are preserved. |
| **Format from Disk Utility / `diskutil`** | `mkntfs` runs **in-process** inside the extension, and its device IO goes to the FSKit block device. Quick format is the default. Options cover the label, cluster size and compression. |
| **Probe** | Recognises NTFS volumes and reports their label, serial number and a stable UUID derived from the serial. |
| **Hibernation and dirty detection** | Detects `hiberfil.sys`/Fast Startup and the `$Volume` dirty flag. In that case a read/write mount fails with `EPERM`, and you can mount read-only, or explicitly opt in to `remove_hiberfile` / `recover`. |
| **Volume label rename** | Renames the volume label of a mounted volume (up to 32 UTF-16 code units). |
| **Unicode names** | The UI shows UTF-8. On disk, names are stored as NTFS UTF-16LE. DOS 8.3 short names are hidden. |
| **Case sensitivity** | NTFS POSIX namespace (case-sensitive) by default. Optional `ignore_case`. |
| **Hidden metadata** | `$MFT`, `$Bitmap` and the other metadata files are hidden unless you set `show_sys_files`. |

## Known limitations

- **No full `chkdsk`/repair.** The extension can detect a dirty or unclean
  volume and can reset the journal (`recover`), but it cannot repair a
  corrupted file system. Use Windows `chkdsk /f` for that.
- **No POSIX ownership mapping.** NTFS stores Windows security descriptors,
  not uid/gid. Every item is reported with the mount's uid/gid and with mode
  `0755` (directories) or `0644` (files). Write permission is removed when
  the item has the NTFS `READONLY` attribute. `chown`/`chgrp` are accepted
  but have no effect. A `chmod` changes only the write bit, which toggles
  `READONLY`. There is no support for ntfs-3g user mapping files.
- **Encrypted (EFS) files are not supported.** They are flagged, but you
  cannot read their contents.
- **Compressed files have limited support.** Compressed files can be read.
  Writing to them is limited or unsupported, following libntfs-3g's own
  restrictions.
- **No extended attributes or alternate data streams (ADS), initially.**
  Only the unnamed `$DATA` stream is exposed. This means Finder metadata
  that macOS stores in xattrs (tags, quarantine, resource forks) is not
  kept on NTFS volumes in the first release.
- **Sparse files** are reported through a flag. They have no special
  hole-punching API.
- **Performance:** libntfs-3g is not thread-safe, so all operations on a
  given volume are serialised. Concurrent IO from many apps to the same
  volume will not scale.

## Why FSKit?

| Approach | Drawbacks | ntfs4mac (FSKit) |
|---|---|---|
| **Kernel extension (kext)** | On Apple Silicon you have to lower security in Recovery to load third-party kexts. A bug can panic the machine. Apple has deprecated the model. | Runs entirely in **user space**, in a **sandboxed app extension**. A crash takes down only the extension process. |
| **macFUSE** | Installs its own kernel extension, or uses its FSKit-based backend that is still evolving. Needs a separate install and approval. The ntfs-3g FUSE daemon runs with broad device access. | Uses **Apple's own FSKit** framework, which ships with macOS 15.4+. Nothing to install apart from the app. `fskitd` opens the block device and hands the extension only the resource it is allowed to use. |
| **Apple's built-in NTFS** | **Read-only.** Cannot format. | Read **and write**, plus formatting from Disk Utility, label rename and symlinks. |

FSKit integrates with the system tools. `diskutil`, Disk Utility and
`mount -F` talk to `fskitd`, which loads the extension on demand over XPC.
The extension never opens `/dev/disk*` itself.

## Architecture

The binding design is in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and
the frozen C API is in
[`NTFSExtension/Bridge/NTFSBridge.h`](NTFSExtension/Bridge/NTFSBridge.h).
Notes on FSKit specifics are in [`docs/FSKIT_NOTES.md`](docs/FSKIT_NOTES.md).

### Data flow

```
 Finder / diskutil / Disk Utility / mount -F
        │  mount, unmount, format, check requests
        ▼
     fskitd  ──── XPC ────►  NTFSExtension.appex   (sandboxed, user space)
                                   │
          NTFSFileSystem  (FSUnaryFileSystem)       probe / load / unload / check / format
                                   │
          NTFSVolume  (FSVolume.Operations,         lookup / enumerate / read / write /
                       ReadWriteOperations)          attributes / create / rename / remove
                                   │   Swift → C   (NTFSBridgeSwift.swift)
                                   ▼
          NTFSBridge.c  ── per-volume mutex ──►  libntfs-3g
                                   │               (ntfs_mount, ntfs_readdir, ntfs_attr_pread, ...)
                                   │   struct ntfs_device_operations (ntfsb_devio.c,
                                   │   aligned bounce buffers)
                                   ▼
          ntfsb_io callbacks  (BlockDeviceIO.swift)
                                   │
                                   ▼
          FSBlockDeviceResource.read / write
                                   │
                                   ▼
          /dev/diskXsY   (opened by fskitd, never by the extension)
```

Formatting follows the same path, except that `ntfsb_format()` calls the
`mkntfs` code compiled into the bridge (`mkntfs_glue.c`) instead of
libntfs-3g's mount path.

### Components

| File | Responsibility |
|---|---|
| `project.yml` | XcodeGen spec that generates `NTFS4Mac.xcodeproj`. |
| `NTFS4Mac/NTFS4MacApp.swift` | SwiftUI host app. It is the container that FSKit requires. It shows status and explains how to enable the extension in System Settings. |
| `NTFS4Mac/Info.plist`, `NTFS4Mac.entitlements` | Host app metadata and entitlements. |
| `NTFSExtension/Info.plist` | `EXAppExtensionAttributes`: `FSShortName`, `FSPersonalities`, `FSMediaTypes`, and so on. This is the authoritative source for the file-system short name and the personality names. |
| `NTFSExtension/NTFSExtension.entitlements` | `com.apple.developer.fskit.fsmodule` and the App Sandbox. |
| `NTFSExtension/NTFSExtensionMain.swift` | `@main` `UnaryFileSystemExtension` entry point. |
| `NTFSExtension/NTFSFileSystem.swift` | `FSUnaryFileSystem`: probe, load, unload, check and format. |
| `NTFSExtension/NTFSVolume.swift` | `FSVolume` plus `Operations` and `ReadWriteOperations` (and `XattrOperations`, `OpenCloseOperations`). Maps FSKit calls to `NTFSMount`. |
| `NTFSExtension/NTFSItem.swift` | `FSItem` subclass that carries the MFT record number (`ino`) and a cached type. |
| `NTFSExtension/NTFSBridgeSwift.swift` | Swift wrappers over the C API: `NTFSMount`, `NTFSError`, `NTFSStat`, `NTFSMountOptions`, `NTFSFormatOptions`, and so on. |
| `NTFSExtension/BlockDeviceIO.swift` | Adapts `FSBlockDeviceResource` to the `ntfsb_io` callback table. |
| `NTFSExtension/NTFSExtension-Bridging-Header.h` | Imports `Bridge/NTFSBridge.h` into Swift. |
| `NTFSExtension/Bridge/NTFSBridge.h` | **Frozen** C API between Swift and libntfs-3g. |
| `NTFSExtension/Bridge/NTFSBridge.c` | Implements the C API on top of libntfs-3g (UTF-8 to UTF-16 conversion, errno mapping, locking). |
| `NTFSExtension/Bridge/ntfsb_devio.c/.h` | `ntfs_device_operations` backed by `ntfsb_io`, with read-modify-write bouncing for unaligned IO. |
| `NTFSExtension/Bridge/mkntfs_glue.c/.h` | Runs mkntfs's `main()` in-process, with its device IO redirected to `ntfsb_io`. |
| `scripts/build-libntfs3g.sh` | Fetches and builds static `libntfs-3g.a` and `libmkntfs.a` (arm64, and x86_64) into `ThirdParty/`. |
| `scripts/dev-install.sh` | Builds the project, copies the app to `/Applications` and registers the extension. |
| `Tests/bridge/` | Host-side (Linux/macOS) C self-test of the bridge against a disk image. |
| `docs/` | `ARCHITECTURE.md`, `TESTING.md`, `FSKIT_NOTES.md`. |

### Key design decisions

- **Errno convention.** Every bridge function returns `0` on success or a
  **positive POSIX errno** on failure. The bridge never uses the global
  `errno`. Swift converts the result with `NTFSError.check(_:)` into
  `POSIXError`, which is what FSKit reply handlers expect. When a mount
  returns `EPERM`, the volume is hibernated or unclean.
- **Inode identity.** An `FSItem` is identified by its NTFS **MFT record
  number** (the low 48 bits of the MFT reference). The root directory is
  `NTFSB_ROOT_INO` (5). Records 0–15 are NTFS metadata. `NTFSVolume` keeps a
  lock-guarded `[UInt64: NTFSItem]` cache, and `reclaimItem` evicts entries
  from it. The MFT sequence number is reported as `generation`.
- **Per-volume mutex.** libntfs-3g is not thread-safe. The bridge serialises
  every call on one volume through a mutex that it owns, so Swift can call in
  from any task without extra locking, apart from locks around its own
  caches. `ntfsb_format()` is serialised **process-wide** because mkntfs
  uses global state.
- **Aligned IO bouncing.** The `ntfsb_io` callbacks, and therefore
  `FSBlockDeviceResource`, only ever see offsets and lengths that are
  multiples of the device block size (512 or 4096). The C device layer does
  read-modify-write bouncing for anything that is not aligned.
- **mkntfs in-process, with redirected device ops.** mkntfs is a program,
  not a library. The build script compiles `ntfsprogs/mkntfs.c` into
  `libmkntfs.a` with `-Dmain=ntfsb_mkntfs_main` and
  `-Dntfs_device_default_io_ops=ntfsb_mkntfs_io_ops`. `ntfsb_format()` then
  builds an argv (`-F -Q -L label -c size -s sector -p hidden -H 0 -S 0
  <dummy-dev> <sectors>`) and calls it, so every sector mkntfs writes goes
  through the FSKit resource.
- **No `/dev` access.** The extension never opens device nodes. `fskitd`
  owns the device and passes in an `FSBlockDeviceResource`.
- **Swift 6, strict concurrency.** Wrapper types are `Sendable`. `NTFSMount`
  owns the `ntfsb_volume*` and keeps the `BlockDeviceIO` alive until
  unmount.

## Requirements

- A Mac with **Apple Silicon or Intel**.
- **macOS 15.4 or later**. This is the first release with public FSKit.
- **Xcode 16 or later**.
- **Homebrew** build tools for libntfs-3g:
  ```sh
  brew install autoconf automake libtool pkg-config gettext
  ```
- **XcodeGen**: `brew install xcodegen`
- An **Apple Developer team** for code signing. The extension needs the
  `com.apple.developer.fskit.fsmodule` entitlement, so the app and the
  extension must be signed with a team that can provision it.

## Quick start

> The authoritative, step-by-step procedure (including test images and
> expected output) is in [`docs/TESTING.md`](docs/TESTING.md). The commands
> below are an overview. The exact file-system short name and personality
> names come from `FSShortName` / `FSPersonalities` in
> [`NTFSExtension/Info.plist`](NTFSExtension/Info.plist). The examples use
> `ntfs` and `NTFS`.

### 1. Build libntfs-3g

```sh
./scripts/build-libntfs3g.sh
```

This fetches the ntfs-3g sources and builds static `libntfs-3g.a` and
`libmkntfs.a` (arm64, and x86_64) into `ThirdParty/ntfs-3g/`. The
`ThirdParty/` directory is gitignored.

### 2. Generate the Xcode project

```sh
xcodegen generate        # produces NTFS4Mac.xcodeproj from project.yml
```

### 3. Build and install

In Xcode: open `NTFS4Mac.xcodeproj`, set your development team on both
targets (`NTFS4Mac` and `NTFSExtension`), and build the `NTFS4Mac` scheme.

From the command line:

```sh
xcodebuild -project NTFS4Mac.xcodeproj -scheme NTFS4Mac \
           -configuration Debug DEVELOPMENT_TEAM=<YOUR_TEAM_ID> build
```

Then copy the built `NTFS4Mac.app` to `/Applications` and launch it once so
that the system registers the embedded extension. `scripts/dev-install.sh`
automates the build, copy and register steps.

### 4. Enable the extension

Open **System Settings → General → Login Items & Extensions → File System
Extensions** and switch on **NTFS4Mac**. Confirm that it is registered:

```sh
pluginkit -m -p com.apple.fskit.fsmodule -v
```

### 5. Use it

**Create a scratch disk image to test with.** Do this before you go near a
real disk.

```sh
hdiutil create -size 1g -layout NONE scratch.dmg   # blank 1 GiB image, no partition map
hdiutil attach -nomount scratch.dmg                # prints e.g. /dev/disk7
```

**Find disks:**

```sh
diskutil list
```

**Format** (this **destroys** all data on the target):

```sh
# Format an existing volume/partition
diskutil eraseVolume NTFS MyVolume /dev/disk7

# Partition a whole disk with a single NTFS volume (MBR for best Windows compatibility)
diskutil eraseDisk NTFS MyVolume MBR /dev/disk7
```

You can also use Disk Utility, where **NTFS** appears in the Format menu
once the extension is enabled.

**Mount:**

```sh
diskutil mount /dev/disk7s1

# or explicitly, via FSKit
mkdir -p /tmp/ntfs
mount -F -t ntfs /dev/disk7s1 /tmp/ntfs

# read-only
mount -F -t ntfs -o rdonly /dev/disk7s1 /tmp/ntfs
```

**Unmount:**

```sh
diskutil unmount /dev/disk7s1        # or: umount /tmp/ntfs
hdiutil detach /dev/disk7            # when done with a disk image
```

## Mount options

Pass these with `mount -F -t ntfs -o opt1,opt2,... <device> <mountpoint>`.
They map to `NTFSMountOptions` / `ntfsb_mount_opts`.

| Option | Bridge flag | Effect |
|---|---|---|
| `rdonly` | `NTFSB_MOUNT_RDONLY` | Mount read-only. This is used automatically as a fallback when the volume is hibernated or dirty. |
| `remove_hiberfile` | `NTFSB_MOUNT_REMOVE_HIBER` | If Windows is hibernated or Fast Startup suspended, **delete `hiberfil.sys`** and mount read/write. **Any unsaved Windows session state is lost.** |
| `recover` | `NTFSB_MOUNT_RECOVER` | Reset the NTFS journal (`$LogFile`) when the volume was not cleanly unmounted. This does not repair corruption. |
| `show_sys_files` | `NTFSB_MOUNT_SHOW_SYS_FILES` | Show NTFS metadata files (`$MFT`, `$Bitmap`, ...) in the root directory. |
| `ignore_case` | `NTFSB_MOUNT_IGNORE_CASE` | Case-insensitive name lookups. |
| `uid=N` | `ntfsb_mount_opts.uid` | Owner reported for every item. Defaults to the uid that FSKit passes, otherwise 99 ("unknown"). |
| `gid=N` | `ntfsb_mount_opts.gid` | Group reported for every item. Same default as `uid`. |

The bridge also supports `fmask`/`dmask` (permission bits removed from files
and directories). Whether they are exposed as mount options depends on the
FSKit option parsing in `NTFSFileSystem.swift`.

## Format options

These are used by `diskutil eraseVolume` / `eraseDisk`, by Disk Utility, and
by FSKit's format request. They map to `NTFSFormatOptions` /
`ntfsb_format_opts` and then to `mkntfs` arguments.

| Option | mkntfs arg | Effect |
|---|---|---|
| `-L label` | `-L` | Volume label. Taken from the name given to `diskutil`. |
| `-c clustersize` | `-c` | Cluster size in bytes, for example `4096`. `0` or unset means the mkntfs default for the volume size. |
| `-Q` | `-Q` | **Quick format (default):** do not zero the volume. |
| `-f` | *(no `-Q`)* | Full format: zero the whole volume first. Slow on large disks. |
| `-C` | `-C` | Enable compression on the volume by default. |

The bridge always passes `-F` (force, because the target is a partition
image), the device sector size (`-s`), hidden sectors (`-p`, the partition
start), and `-H 0 -S 0` for the geometry. Format progress is reported back
to FSKit, and you can request cancellation, which is honoured between mkntfs
phases.

## Debugging

**Unified log.** The extension logs under its bundle id.

```sh
# Extension logs
log stream --level debug --predicate 'subsystem == "com.vahidzekic.ntfs4mac.NTFSExtension"'

# Extension + fskitd together
log stream --level debug --predicate \
  'subsystem == "com.vahidzekic.ntfs4mac.NTFSExtension" OR process == "fskitd" OR subsystem == "com.apple.FSKit"'

# After the fact
log show --last 10m --predicate 'process == "fskitd"'
```

**Extension registration:**

```sh
pluginkit -m -p com.apple.fskit.fsmodule -v
```

**Attach the Xcode debugger.** Mount a volume (or run `diskutil list`) so
that `fskitd` launches the extension. In Xcode, choose **Debug → Attach to
Process** and select `NTFSExtension`. Alternatively, choose **Debug → Attach
to Process by PID or Name…**, enter `NTFSExtension` and check *Wait for
launch*, then trigger a mount.

**C bridge self-test.** This runs on the host (Linux or macOS) against a disk
image, without FSKit:

```sh
make -C Tests/bridge
```

Use it to separate bridge and libntfs-3g bugs from FSKit integration issues.
See [`docs/TESTING.md`](docs/TESTING.md) for details.

## Troubleshooting / FAQ

**The extension is not listed in System Settings, or in `pluginkit` output.**
- Make sure the app is in `/Applications` and that you launched it at least
  once.
- Check the code signing: `codesign -dv --entitlements - /Applications/NTFS4Mac.app/Contents/Extensions/NTFSExtension.appex`
  must show `com.apple.developer.fskit.fsmodule`. Unsigned or ad-hoc builds
  without the entitlement are not loaded.
- Make sure you are on macOS 15.4 or later.
- Run `pluginkit -m -p com.apple.fskit.fsmodule -v`. If a stale copy (for
  example one in DerivedData) is registered, remove it, or unregister it with
  `pluginkit -r <path>`.

**Mount fails with `EPERM` / "Operation not permitted".**
The volume is most likely **hibernated**. Windows *Fast Startup* (on by
default) hibernates the kernel on shutdown, which leaves `hiberfil.sys`
active. Writing to the volume from macOS would corrupt it.
- The best fix: in Windows, turn off Fast Startup (Control Panel → Power
  Options → "Choose what the power buttons do"), or use **Restart** or
  `shutdown /s /f /t 0` instead of Shut down.
- Or mount read-only with `-o rdonly`.
- Or, accepting the loss of the hibernated Windows session, use
  `-o remove_hiberfile`.

**The volume is marked dirty or was not cleanly unmounted.**
Probing reports this, and a read/write mount falls back to read-only. Run
`chkdsk /f X:` in Windows. If you know the volume is consistent, you can use
`-o recover` to reset the journal, but this does not repair anything.

**NTFS is not in the Disk Utility Format menu.**
- Make sure the extension is enabled (see above). Disk Utility only lists
  file systems from enabled FSKit modules.
- Quit Disk Utility and start it again after you enable the extension.
- Check that `FSPersonalities` in `NTFSExtension/Info.plist` declares a
  format-capable personality. `diskutil listFilesystems` should show it.
- Use `diskutil eraseVolume NTFS ...` from Terminal to see the error.

**Files show the wrong owner or permissions.**
This is expected: see [Known limitations](#known-limitations). Use
`-o uid=N,gid=N` to choose the owner that is reported.

## Project layout

```
.
├── README.md
├── project.yml                     XcodeGen spec
├── NTFS4Mac/                       Host app (container for the extension)
│   ├── NTFS4MacApp.swift
│   ├── Info.plist
│   └── NTFS4Mac.entitlements
├── NTFSExtension/                  FSKit app extension (com.apple.fskit.fsmodule)
│   ├── Info.plist
│   ├── NTFSExtension.entitlements
│   ├── NTFSExtensionMain.swift
│   ├── NTFSFileSystem.swift
│   ├── NTFSVolume.swift
│   ├── NTFSItem.swift
│   ├── NTFSBridgeSwift.swift
│   ├── BlockDeviceIO.swift
│   ├── NTFSExtension-Bridging-Header.h
│   └── Bridge/
│       ├── NTFSBridge.h            frozen C API
│       ├── NTFSBridge.c
│       ├── ntfsb_devio.c / .h
│       └── mkntfs_glue.c / .h
├── ThirdParty/                     build output (gitignored)
│   └── ntfs-3g/{include,lib}
├── scripts/
│   ├── build-libntfs3g.sh
│   └── dev-install.sh
├── Tests/
│   └── bridge/                     host-side C self-test
└── docs/
    ├── ARCHITECTURE.md
    ├── TESTING.md
    └── FSKIT_NOTES.md
```

## Roadmap

- [x] Architecture contract and frozen C bridge API
- [ ] C bridge over libntfs-3g, with aligned device IO and mkntfs glue
- [ ] FSKit extension: probe, mount, read/write, namespace operations, format
- [ ] First build and validation on real hardware (Apple Silicon and Intel)
- [ ] Bridge self-test coverage and an FSKit integration test plan
      (`docs/TESTING.md`)
- [ ] Extended attributes and alternate data streams (xattr ↔ ADS)
- [ ] Better compressed-file write support
- [ ] Optional user-mapping support for POSIX ownership
- [ ] Signed, notarized release builds

## Contributing

Contributions are welcome. Please:

1. Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first. It is the
   contract between components.
2. Treat `NTFSExtension/Bridge/NTFSBridge.h` as **frozen**. Discuss API
   changes in an issue before you open a PR.
3. Follow the conventions: positive-errno returns in C, `NTFSError.check` in
   Swift, Swift 6 strict concurrency, and no direct `/dev` access.
4. Run `make -C Tests/bridge` and the relevant steps in
   `docs/TESTING.md` before you submit. Test on disk images, never on disks
   that hold data you care about.
5. Include logs (`log stream ...`, see [Debugging](#debugging)) in bug
   reports.

## License

ntfs4mac links **libntfs-3g** and compiles **mkntfs** into the extension.
Both are licensed under the **GNU General Public License v2.0 or later**.
For that reason, ntfs4mac as a whole is distributed under
**GPL-2.0-or-later**. Anything that links the C bridge inherits the same
terms.

## Credits

- **[ntfs-3g / libntfs-3g](https://github.com/tuxera/ntfs-3g)** and
  **ntfsprogs (mkntfs)**: by Tuxera and the ntfs-3g / Linux-NTFS project
  authors. This project is only a thin layer over their work.
- Apple's **FSKit** framework.
