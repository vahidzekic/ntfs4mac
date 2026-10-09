# Compilation, Linking & Local Testing Guide

This guide takes a Mac (Apple Silicon or Intel) from a clean checkout to a mounted,
writable NTFS test volume served by `NTFSExtension`. Each step has a command
and the expected result. Where a statement rests on a forum post or another
project rather than on Apple documentation, it is marked **(unverified)** and
sourced in docs/FSKIT_MANIFEST.md.

Identifiers used throughout:

| Thing | Value |
|-------|-------|
| Host app | `/Applications/NTFS4Mac.app` (`com.vahidzekic.ntfs4mac`) |
| Extension | `NTFS4Mac.app/Contents/Extensions/NTFSExtension.appex` (`com.vahidzekic.ntfs4mac.NTFSExtension`) |
| Extension point | `com.apple.fskit.fsmodule` |
| FSKit short name (`mount -t`) | `ntfs4mac` |
| Personality | key `NTFS4Mac`, display name `NTFS` |
| Log subsystem | `com.vahidzekic.ntfs4mac.NTFSExtension` (the Swift `Logger` subsystem) |

---

## 1. Prerequisites

| Requirement | Why / notes |
|-------------|-------------|
| Mac (Apple Silicon or Intel) | The project builds universal (`arm64` + `x86_64`); Debug builds only the running Mac's architecture. The libraries are built universal. |
| **macOS 15.4 or later**; **15.6+ or 26.x recommended** | FSKit is public from 15.4. On 15.4/15.5 Disk Arbitration fails to probe FSKit modules (FB17772372, fixed in 15.6 per Apple DTS), so automount only works from 15.6. `mount -F` works on 15.4. |
| **Xcode 16.3+** (or Xcode 26.x) | The FSKit SDK and the *File System Extension* target template first shipped in Xcode 16.3. `xcode-select -p` must point at it: `sudo xcode-select -s /Applications/Xcode.app` |
| Homebrew tools | `brew install xcodegen autoconf automake libtool pkg-config gettext` (`xcbeautify` optional). Only `pkg-config` and `xcodegen` are strictly needed with the Tuxera tarball; autotools are needed for the GitHub fallback. |
| **Paid Apple Developer Program membership** | The extension must be signed with `com.apple.developer.fskit.fsmodule`; `fskitd` refuses to load modules without it, and ad-hoc signing does not work. Enable the **FSKit Module** capability on the App ID `com.vahidzekic.ntfs4mac.NTFSExtension` (Certificates, Identifiers & Profiles → Identifiers). Xcode automatic signing (`-allowProvisioningUpdates`) then creates the development profile. Other developers report it as a standard capability with no request form, and FSKit apps ship on the Mac App Store, so no special approval appears to be needed **(unverified for free Personal Teams)**. |
| Your Team ID | `security find-identity -v -p codesigning` shows `Apple Development: … (TEAMID)`. Export it: `export DEVELOPMENT_TEAM=ABCDE12345`. |

### SIP, Developer Mode and System Extensions: what does and doesn't apply

- **FSKit modules are ExtensionKit app extensions, not System Extensions or
  kexts.** They run in user space, sandboxed, and are enabled per user in System
  Settings. So:
  - **SIP stays on.** No `csrutil` changes are needed.
  - **`systemextensionsctl developer on` is irrelevant.** It only affects
    System Extensions (DriverKit, Endpoint Security, Network Extensions), and it
    only takes effect with SIP disabled. `systemextensionsctl list` will not list
    `NTFSExtension`. Don't run it for this project.
  - **No kernel extension consent** and no reboot.
- **`DevToolsSecurity -enable`** does apply. Run it once with `sudo` so Xcode
  and `lldb` can attach to the extension process without an admin prompt each
  time:
  ```sh
  sudo DevToolsSecurity -enable
  DevToolsSecurity -status        # "Developer mode is currently enabled."
  ```
- The **"Developer Mode"** switch under Privacy & Security is the same
  developer-tools security setting. You don't need anything else.

---

## 2. Build libntfs-3g (once)

```sh
scripts/build-libntfs3g.sh
```

What it does (details and every flag are at the top of the script):

1. Downloads `ntfs-3g_ntfsprogs-2022.10.3.tgz` from `tuxera.com/opensource/`
   (fallback `download.tuxera.com`, then the GitHub tag) and checks its sha256
   (`f20e36ee…854170c`, pinned). `NTFS3G_VERSION` / `NTFS3G_SHA256` override.
2. For `arm64` and `x86_64`, with `MACOSX_DEPLOYMENT_TARGET=15.4`, runs
   `configure --disable-shared --enable-static --disable-ntfs-3g --disable-plugins
   --disable-crypto --disable-nfconv …`. `--disable-ntfs-3g` means libfuse and
   macFUSE are never involved.
3. Compiles `ntfsprogs/{attrdef,boot,sd,mkntfs,utils}.c` into `libmkntfs.a`.
   `mkntfs.c` gets `-DNTFSB_MKNTFS_UNIT -include NTFSExtension/Bridge/mkntfs_glue.h`,
   which renames `main` → `ntfsb_mkntfs_main`, adds `ntfsb_mkntfs_reset_state`,
   and routes the device-IO table to `ntfsb_mkntfs_io_ops`. The details belong
   to the bridge (`Tests/bridge/README.md`); see also the note in the script on
   why `ntfs_device_unix_io_ops`, not `ntfs_device_default_io_ops`, has to be
   renamed.
4. `lipo`s both slices and installs into `ThirdParty/ntfs-3g/{lib,include}`.
   It also installs host tools (`mkntfs`, `ntfsinfo`, `ntfsls`, `ntfscat`,
   `ntfsfix`) into `ThirdParty/ntfs-3g/bin` for making and inspecting test images.

Check:
```sh
lipo -archs ThirdParty/ntfs-3g/lib/libntfs-3g.a     # x86_64 arm64
nm -g ThirdParty/ntfs-3g/lib/libmkntfs.a | grep ntfsb_  # T _ntfsb_mkntfs_main, T _ntfsb_mkntfs_reset_state, U _ntfsb_mkntfs_io_ops
cat ThirdParty/ntfs-3g/BUILDINFO
```

> Why not `brew install ntfs-3g`? The homebrew-core formula is Linux-only, and
> `gromgit/fuse/ntfs-3g-mac` needs macFUSE and builds the FUSE driver. We need a
> static, FUSE-free library pinned to our deployment target.

---

## 3. Generate, build and sign

```sh
export DEVELOPMENT_TEAM=ABCDE12345
xcodegen generate                       # → NTFS4Mac.xcodeproj (gitignored)
open NTFS4Mac.xcodeproj                 # optional: build & run the NTFS4Mac scheme in Xcode
```

Or from the command line (this is what `scripts/dev-install.sh` does):

```sh
xcodebuild -project NTFS4Mac.xcodeproj -scheme NTFS4Mac -configuration Debug \
  -destination platform=macOS -derivedDataPath build/DerivedData -allowProvisioningUpdates \
  -allowProvisioningDeviceRegistration \
  DEVELOPMENT_TEAM=$DEVELOPMENT_TEAM build
```

The link line for the extension is `-lmkntfs -lntfs-3g` plus `FSKit.framework`.
Static archives resolve left to right, so `libmkntfs.a` must come first.

Verify the product:

```sh
APP=build/DerivedData/Build/Products/Debug/NTFS4Mac.app
ls "$APP/Contents/Extensions"                          # NTFSExtension.appex
codesign --verify --deep --strict "$APP" && echo signed
codesign -d --entitlements - --xml "$APP/Contents/Extensions/NTFSExtension.appex" | plutil -p -
#   "com.apple.developer.fskit.fsmodule" => true
#   "com.apple.security.app-sandbox" => true
plutil -p "$APP/Contents/Extensions/NTFSExtension.appex/Contents/Info.plist" | grep -A3 FSShortName
```

---

## 4. Install, register, enable

```sh
scripts/dev-install.sh            # build + /Applications + pluginkit registration
scripts/dev-install.sh --restart-agent --open-settings
```

The script installs to `/Applications`. FSKit doesn't strictly require that
(Apple's sample runs from Xcode's build folder), but every build output also
gets registered, and duplicate registrations of one bundle id make it
unpredictable which copy `fskit_agent` launches. The script unregisters the
DerivedData copy for that reason.

Check the registration:

```sh
pluginkit -mAvvv -p com.apple.fskit.fsmodule
#  +    com.vahidzekic.ntfs4mac.NTFSExtension(0.1.0)  <UUID>  …  /Applications/NTFS4Mac.app/Contents/Extensions/NTFSExtension.appex
#  ('+' = elected/enabled by the user, '-' = disabled, no mark = no choice yet)
pluginkit -m -i com.vahidzekic.ntfs4mac.NTFSExtension -v
```

If it isn't listed: `pluginkit -a /Applications/NTFS4Mac.app/Contents/Extensions/NTFSExtension.appex`
then open the app once (`open /Applications/NTFS4Mac.app`).

**Enable it.** This is required and is done per user, again after each reinstall:

1. System Settings → **General → Login Items & Extensions**.
2. Under **Extensions**, choose **By Category**.
3. Click **ⓘ** next to **File System Extensions**.
4. Turn on **NTFS (ntfs4mac)** → **Done**.

The NTFS4Mac app's **Open System Settings…** button opens the pane, using the
undocumented URL `x-apple.systempreferences:com.apple.LoginItems-Settings.extension`,
which shipping FSKit apps use. The app's status line reads `FSClient` to show
whether FSKit considers the module enabled.

> The `pluginkit` "+" election is **not** FSKit's enable switch. FSKit keeps its
> own list (see Troubleshooting) and `FSClient` reports that list.

---

## 5. Make test volumes

All images below are throwaway files. Never point these commands at a real disk.

### 5a. Partitionless volume (simplest, matches `FSMediaTypes › Partitionless`)

```sh
mkfile -n 512m /tmp/ntfs.img
ThirdParty/ntfs-3g/bin/mkntfs -F -Q -L NTFSTEST /tmp/ntfs.img
ThirdParty/ntfs-3g/bin/ntfsinfo -m /tmp/ntfs.img | head        # sanity check, no mount needed

hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount /tmp/ntfs.img
#   /dev/disk7            ← note the node; -nomount stops Disk Arbitration from auto-mounting
diskutil list /dev/disk7
```

A RAM disk works the same way and is the most reliable target for `mount -F`
(see Troubleshooting):

```sh
DEV=$(hdiutil attach -nomount ram://1048576 | awk '{print $1}')   # 1048576 × 512 B = 512 MiB
echo $DEV                                                       # /dev/disk8
```
Format it with NTFSExtension itself (§7) or copy an image onto it:
`dd if=/tmp/ntfs.img of=$DEV bs=1m`.

### 5b. GPT disk with a "Microsoft Basic Data" partition (matches the GPT `FSMediaTypes` key)

```sh
mkfile -n 1g /tmp/ntfs-gpt.img
DISK=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount /tmp/ntfs-gpt.img | awk 'NR==1{print $1}')
# Create one Basic Data partition. ExFAT is only a placeholder: on GPT it gets
# the EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 type, and we overwrite the file system next.
diskutil partitionDisk "$DISK" 1 GPT ExFAT PLACEHOLDER 100%
diskutil unmountDisk "$DISK"
diskutil list "$DISK"                      # find the data slice, e.g. disk9s2 (s1 may be EFI)
PART=${DISK}s2                             # adjust to what diskutil list shows
ThirdParty/ntfs-3g/bin/mkntfs -F -Q -L NTFSGPT "$PART"
diskutil info "$PART" | grep -E 'Partition Type|Content'   # Microsoft Basic Data / EBD0A0A2-…
```

`hdiutil create -size 1g -layout GPTSPUD -partitionType "Microsoft Basic Data" …`
is a shortcut, but which `-partitionType` names `hdiutil` accepts varies by OS
version **(unverified)**. The `mkfile` + `diskutil partitionDisk` route above
only uses documented commands.

### 5c. diskutil / Disk Utility formatting

According to Apple DTS, **`diskutil` does not currently have full FSKit
integration**. Formatting through an FSKit module is done with `newfs_fskit`
(§7). To see whether this macOS release lists FSKit personalities, run:

```sh
diskutil listFilesystems | grep -i -E 'ntfs|NTFS4Mac'
```

If an `NTFS4Mac` row appears, the commands are
`diskutil eraseVolume NTFS4Mac NTFSTEST /dev/diskXsY` and
`diskutil eraseDisk NTFS4Mac NTFSTEST GPT /dev/diskX`. Use the personality
**key** (`NTFS4Mac`); `NTFS` would resolve to Apple's read-only `ntfs.fs`, which
cannot erase **(unverified on 15.x/26.x)**.

---

## 6. Mount

**Run mounts as your login user, not with sudo.** FSKit mounts belong to the
user's session. On macOS 26, `sudo mount -F …` fails with "entitlement no" in
the `fskitd` log.

```sh
mkdir -p /tmp/ntfs
mount -F -t ntfs4mac /dev/disk7 /tmp/ntfs                 # read-write
mount -F -t ntfs4mac -o rdonly /dev/disk7 /tmp/ntfs       # read-only
mount -F -t ntfs4mac -o remove_hiberfile,uid=$(id -u),gid=$(id -g) /dev/disk7 /tmp/ntfs
mount | grep /tmp/ntfs
ls -la /tmp/ntfs && df -h /tmp/ntfs
echo hello > /tmp/ntfs/hello.txt && cat /tmp/ntfs/hello.txt
umount /tmp/ntfs                                           # or: diskutil unmount /tmp/ntfs
hdiutil detach /dev/disk7
```

About `-F`: it forces `mount(8)` to use the FSKit module. An Apple engineer
says it is only needed when a kext-based file system and an FSKit module share
the short name. `ntfs4mac` is unique, so `mount -t ntfs4mac …` also works.
Without `-F`, a type with no FSKit match makes `mount` look for
`/Library/Filesystems/<type>.fs/…/mount_<type>`, which is the "No such file
or directory" error you see when the module isn't enabled.

Mount options (`FSActivateOptionSyntax` = `o:rwu:g:`): `-o` takes `rdonly`,
`remove_hiberfile`, `recover`, `show_sys_files`, `ignore_case`, `uid=N`, `gid=N`
(comma-separated; also `ro`, `rw`, `norecover`, `case_sensitive`, `fmask=`,
`dmask=`, `umask=`). `-r`/`-w`/`-u N`/`-g N` work too.
`rdonly` may arrive at the module as FSKit's own `--rdonly` flag rather than
`-o rdonly`; the module accepts both **(unverified which one)**.

### Automount (Disk Arbitration)

Once the module is enabled, attaching an image **without** `-nomount` (or
`diskutil mount /dev/diskXsY`) lets Disk Arbitration probe it. On macOS 15.6+:

1. DA asks each module whose `FSMediaTypes` match, in `FSProbeOrder` order.
2. Our probe must return `.usable`. DA treats anything else, including
   `.usableButLimited`, as failure and moves on **(third-party measurement)**.
3. DA runs a **check with `-q`**, and if that fails, with `-y`. A module
   without `startCheck` is never automounted (Apple, forum 786270).
4. It mounts under `/Volumes/<label>`.

Apple's built-in read-only NTFS driver is a kext, and kexts are preferred over
FSKit modules. If `mount` shows the volume as Apple's `ntfs` with `read-only`,
Apple's driver won. Unmount and mount again
(`diskutil unmount /dev/diskXsY && diskutil mount /dev/diskXsY`), or use
`mount -F -t ntfs4mac`. Another developer reports the first probe after
enabling or restarting `fskit_agent` often loses and the second wins
**(third-party report)**.

---

## 7. Check and format with the FSKit command-line tools

macOS 15.4+ ships `/sbin/fsck_fskit` and `/sbin/newfs_fskit`, which run a
module's `startCheck` / `startFormat` tasks. Apple staff have only documented
`-t <shortname>`; check `man fsck_fskit` and `man newfs_fskit` on your OS for
the full syntax **(argument order unverified)**. Options after the type are
parsed with the module's getopt strings.

```sh
# Check (FSCheckOptionSyntax "nqyfl")
fsck_fskit -t ntfs4mac -n /dev/disk7        # report only
fsck_fskit -t ntfs4mac -q /dev/disk7        # quick check, what Disk Arbitration runs first
fsck_fskit -t ntfs4mac -y /dev/disk7        # repair

# Format (FSFormatOptionSyntax "L:v:c:p:QfC") — the device must not be mounted
newfs_fskit -t ntfs4mac -L NTFSTEST /dev/disk8                 # quick format (default, -Q)
newfs_fskit -t ntfs4mac -L DATA -c 65536 -f /dev/disk8         # 64 KiB clusters, full (zeroing) format
newfs_fskit -t ntfs4mac -L ARCHIVE -C /dev/disk8               # compression enabled
```

Cross-check the result with the host tools: `ThirdParty/ntfs-3g/bin/ntfsinfo -m /dev/disk8`
(or on the image file after detaching).

---

## 8. Logs

```sh
# The extension's own os_log / Logger output
log stream --level debug --predicate 'subsystem == "com.vahidzekic.ntfs4mac.NTFSExtension"'

# FSKit daemons: loading, probing, mount refusals, enablement
log stream --predicate 'process == "fskitd"'
log stream --level debug --predicate 'process IN {"fskitd","fskit_agent","extensionkitservice","diskarbitrationd","mount"} OR subsystem IN {"com.apple.FSKit","com.apple.LiveFS"}'

# After the fact (last 10 minutes)
log show --last 10m --info --debug --predicate 'subsystem == "com.vahidzekic.ntfs4mac.NTFSExtension" OR process == "fskitd"'
```

`mount` reports little more than an exit code; the real error is almost always
in the `fskitd` log.

---

## 9. Debug the extension in Xcode

The extension runs as its own process, `NTFSExtension`, started on demand by
`fskit_agent`/ExtensionKit. It isn't a child of the app, so **Run** in Xcode
doesn't debug it.

1. `sudo DevToolsSecurity -enable` (once).
2. Build Debug, install (`scripts/dev-install.sh`) and enable the module.
3. In Xcode: **Debug → Attach to Process by PID or Name…** → Name: `NTFSExtension`
   → tick **Wait for launch** (or attach directly if it's already running:
   `pgrep -fl NTFSExtension`) → **Attach**.
4. Trigger it with `mount -F -t ntfs4mac /dev/diskN /tmp/ntfs`. Breakpoints in
   `NTFSFileSystem.probeResource` / `loadResource` and in the C bridge
   (`ntfsb_mount`) are hit.

From a terminal: `lldb -n NTFSExtension -w`, then `continue`.

Notes:
- Debug builds carry `get-task-allow` (Xcode adds it), which is what makes
  attaching possible. Release has `CODE_SIGN_INJECT_BASE_ENTITLEMENTS = NO`
  for notarization, so it can't be attached to.
- A paused extension holds up `fskitd`, so `mount` may time out while you sit at
  a breakpoint. That is expected.

### Restarting FSKit components

| Command | Effect | Caveat |
|---------|--------|--------|
| `pkill -f Contents/Extensions/NTFSExtension.appex` | Stops the extension; it relaunches on the next request | Drops any volume it serves; unmount first |
| `killall -9 fskit_agent` | Per-user agent re-reads its enabled list and forgets cached extension identities (fixes stale `_EXExtensionIdentity` / `NSCocoaErrorDomain 4099` after a rebuild) | launchd respawns it; plain SIGTERM is reportedly ignored |
| `killall -9 extensionkitservice` | Clears stale ExtensionKit state | Same caveat |
| `sudo killall fskitd` | Root daemon; clears per-volume state that survives everything else (e.g. "resource state is 5", or after experimenting with `FSShortName`) | Unmounts **all** FSKit volumes (ExFAT/FAT too); launchd restarts it |

---

## 10. C bridge self-test (no FSKit, no signing)

The bridge can be tested directly against a disk image on Linux or macOS. The
targets, image fixtures and expected output are documented by the bridge
engineer in `Tests/bridge/README.md`. In short:

```sh
make -C Tests/bridge          # builds the selftest against libntfs-3g and runs it
```

Run this first after any change under `NTFSExtension/Bridge/`. It is much
faster than the install/enable/mount cycle.

---

## 11. Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `mount: Unable to invoke task`; `fskitd` log: `Module … is disabled!` / `Attempt to start disabled extension` | Module not enabled (or re-registered by a reinstall) | Enable in System Settings (§4), then `killall -9 fskit_agent` |
| System Settings toggle flips back off; log: `Failed to enabled FSExtension: … NSPOSIXErrorDomain Code=1` or `did not find team ID` | Bug reported on macOS 26.x for third-party modules (settings host lacks a team id when calling `fskitd`) **(third-party reports)** | Workaround used by other FSKit projects (unofficial; edits a file Apple owns): `python3 -c 'import plistlib,sys;p=sys.argv[1];l=plistlib.load(open(p,"rb"));i="com.vahidzekic.ntfs4mac.NTFSExtension";l.append(i) if i not in l else None;plistlib.dump(l,open(p,"wb"))' ~/Library/Group\ Containers/group.com.apple.fskit.settings/enabledModules.plist && killall -9 fskit_agent` (back the file up first) |
| `pluginkit -m -p com.apple.fskit.fsmodule` doesn't list the module | Not registered | Run the app once from `/Applications`; `pluginkit -a …/NTFSExtension.appex`; check for duplicates with `pluginkit -mAvvv -p com.apple.fskit.fsmodule` |
| Module listed twice in System Settings | DerivedData copy also registered | `pluginkit -r build/DerivedData/…/NTFSExtension.appex`; re-run `scripts/dev-install.sh` |
| `fskitd`: `Hello FSClient! entitlement no`, or the extension is killed at launch | Signed without `com.apple.developer.fskit.fsmodule`, ad-hoc signed, or mounting via `sudo` | Set `DEVELOPMENT_TEAM`, enable the FSKit Module capability, rebuild; mount as your user |
| `codesign` OK but `fskitd` "Can't find the extension" during probe (15.4/15.5) | DiskArbitration probe bug FB17772372 | Use 15.6+ or `mount -F` |
| `mount -F` on a **physical** disk: `EACCES` opening `/dev/rdiskN` | Known FSKit permission issue, acknowledged by DTS | Test with `hdiutil`-attached images or RAM disks; for real disks use Disk Arbitration (`diskutil mount`) |
| Volume mounts read-only as plain `ntfs`, not `ntfs4mac` | Apple's kext driver won the probe | `diskutil unmount` then `mount -F -t ntfs4mac …` or remount (§6) |
| `ECONNREFUSED` (61) from `ReallyMountVolume` after changing `FSShortName` | Stale per-volume state in `fskitd` | `sudo killall fskitd` |
| `mount: Unable to invoke task` / `Invalid argument` (22) right after `umount` of the same device | FSKit tears the previous volume down asynchronously after `umount`; a mount sent before that finishes is rejected | Wait a few seconds (`sleep 3`) and mount again. Observed on macOS 26.5 with a RAM disk; the second mount succeeds after the pause |
| `mount` hangs | Extension paused in a debugger or deadlocked | Detach the debugger; check the bridge's per-volume mutex; `pkill -f NTFSExtension.appex` |
| `EPERM` on read-write mount | Volume is hibernated / Fast Startup or unclean | Mount `-o rdonly`, or `-o remove_hiberfile` / `-o recover` (both write to the volume), or shut Windows down fully |
| Disk Utility shows the volume as "Unknown" | `FSShortName` `ntfs4mac` doesn't map to a `/System/Library/Filesystems` bundle | Expected; see docs/FSKIT_MANIFEST.md "FSShortName and personality: decision" |
| Link error `_ntfsb_mkntfs_io_ops` / `_ntfsb_mkntfs_reset_state` undefined | Bridge glue missing, or `libmkntfs.a` built before `NTFSExtension/Bridge/mkntfs_glue.h` existed | Re-run `scripts/build-libntfs3g.sh` (it force-includes the glue header into mkntfs.c); the bridge defines `ntfsb_mkntfs_io_ops` |
| Link error `_ntfsb_mkntfs_exit` undefined | A newer NTFS-3G mkntfs calls `exit()`; the build renames it so it can't kill the extension | Implement `ntfsb_mkntfs_exit` in the bridge (e.g. longjmp back into `ntfsb_format`) or stay on 2022.10.3 |
| Link errors for `ntfs_*` symbols | Wrong library order | `OTHER_LDFLAGS` must be `-lmkntfs -lntfs-3g` |
| `build-libntfs3g.sh`: `config.h differs between arm64 and x86_64` | A configure run-time check guessed differently when cross-compiling | Inspect the diff; add the right `ac_cv_*` cache value in the script, or `NTFS3G_ALLOW_CONFIG_DIFF=1` if harmless |

---

## 12. Licensing (GPL)

libntfs-3g and mkntfs are **GPL-2.0-or-later** (`ThirdParty/ntfs-3g/COPYING`).
`NTFSExtension.appex` links both statically, so the extension, and the app
bundle that ships it, must be distributed under GPL-compatible terms:

- Offer the **complete corresponding source** with every binary you distribute:
  this repository at the exact commit, plus the NTFS-3G tarball (version and
  sha256 are in `ThirdParty/ntfs-3g/BUILDINFO`) and the build scripts.
- Keep the copyright and no-warranty notices visible. The app's License panel
  and `NSHumanReadableCopyright` carry them.
- Don't add terms that restrict redistribution. Note that Mac App Store terms
  are widely regarded as incompatible with plain GPLv2 without an additional
  permission from every copyright holder, which NTFS-3G doesn't grant. Plan on
  Developer ID + notarization.
