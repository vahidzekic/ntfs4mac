# Changelog

All notable changes to NTFS4Mac are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project
uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.1] - 2026-10-09

Universal build (Intel and Apple Silicon), tested on an M4 MacBook Air with
macOS 15.7.

### Fixed
- **Formatting from Disk Utility on macOS 15.** On macOS 15, formatting
  from Disk Utility or `diskutil eraseDisk|eraseVolume NTFS4Mac …` failed
  with "File system formatter failed" (-69832). Disk Utility runs the
  format helper as root, and on macOS 15 `newfs_fskit` rejects requests
  from root (EINVAL) before they reach the module. The `newfs_ntfs4mac`
  and `fsck_ntfs4mac` helpers now run the FSKit tools as the logged-in
  user on macOS 15. macOS 26 is unchanged.

### Known issues
- On macOS 15 the NTFS4Mac app can show "Not registered with FSKit" even
  when the extension is enabled and working.

## [0.1.0] - 2026-10-09

First public preview, by Vahid Zekic. Tested on an Intel Mac with
macOS 26.5.

### Added
- **FSKit file-system extension** (`com.vahidzekic.ntfs4mac.NTFSExtension`,
  short name `ntfs4mac`) that mounts NTFS volumes read-write in user space,
  without a kernel extension or macFUSE.
- **C bridge over libntfs-3g 2022.10.3**: lookup, directory listing with
  resumable cookies, read/write, create, rename with POSIX replace
  semantics, delete, hard links, symlinks (native Windows and WSL), times,
  file attributes, volume label and statistics.
- **Format as NTFS** with `mkntfs` running inside the extension:
  `newfs_fskit -t ntfs4mac`, quick or full format, label, cluster size,
  compression.
- **Disk Utility integration**: `/Library/Filesystems/ntfs4mac.fs` adds
  **NTFS** to Erase › Format and enables
  `diskutil eraseDisk|eraseVolume NTFS4Mac …`.
- **Check** (`fsck_fskit -t ntfs4mac`): reports dirty and hibernated
  volumes and resets the journal with `-y`.
- **Hibernation and Fast Startup detection**, with an automatic read-only
  fallback.
- **Mount options**: `rdonly`, `remove_hiberfile`, `recover`,
  `show_sys_files`, `ignore_case` / `case_sensitive`, `uid`, `gid`, `fmask`,
  `dmask` and `umask`.
- **Host app NTFS4Mac**: shows whether the extension is enabled and how to
  turn it on, plus an About panel and the GPL notice.
- **App icon**.
- **macOS installer** (`.pkg`) that installs the app and the file-system
  bundle, built by `scripts/make-release.sh` with optional Developer ID
  signing and notarization. `scripts/uninstall.sh` removes everything.
- **Host-side C self-test** (`Tests/bridge`) against disk images: 13064
  checks at 512- and 4096-byte sectors, clean under ASan/UBSan.
- **Documentation**: README, architecture, testing guide, FSKit notes and
  manifest reference, release guide.

### Fixed during bring-up
- **Format failing with EIO.** `newfs_fskit` reported EIO at mkntfs' final
  "Syncing device" step although the volume had been written correctly.
- **Lost log messages.** libntfs-3g and mkntfs messages were sent to
  stderr, which an app extension cannot show. They now go to the unified
  log.
- **Intel builds.** The build failed on Intel Macs; the project is now
  universal.
- **"Open System Settings" button** did nothing from the sandboxed app.
- **Missing app icon.** The asset catalog was left out of the app target.

### Known issues
- No full `chkdsk`. Repair corrupted volumes in Windows.
- Apple's built-in read-only NTFS driver can mount a disk before
  NTFS4Mac does. Unmount the disk and mount it again.
- On macOS 26 the switch in System Settings' **By App** view does not
  work. Use **By Category**.
- Remounting a device immediately after unmounting it can fail with
  EINVAL. Wait a few seconds before trying again.
- No extended attributes or alternate data streams, and no POSIX ownership
  mapping.

[Unreleased]: https://github.com/vahidzekic/ntfs4mac/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/vahidzekic/ntfs4mac/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/vahidzekic/ntfs4mac/releases/tag/v0.1.0
