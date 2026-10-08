#!/usr/bin/env bash
#
# dev-install.sh — build NTFS4Mac (Debug, arm64), install it to /Applications
# and register the NTFSExtension FSKit module.
#
# Usage:  DEVELOPMENT_TEAM=ABCDE12345 scripts/dev-install.sh [options]
#   --release          build the Release configuration instead of Debug
#   --no-install       build only; leave the app in build/DerivedData
#   --restart-agent    SIGKILL fskit_agent afterwards so it forgets the old
#                      extension identity (each rebuild re-registers the appex)
#   --open-settings    open System Settings > Login Items & Extensions at the end
#
# Requirements: macOS 15.4+, Xcode 16.3+ (FSKit SDK), XcodeGen
# (brew install xcodegen), a development team whose provisioning profile for
# com.vahidzekic.ntfs4mac.NTFSExtension carries the FSKit Module capability
# (entitlement com.apple.developer.fskit.fsmodule), and
# ThirdParty/ntfs-3g from scripts/build-libntfs3g.sh (built on demand here).
#
# Why /Applications? FSKit does not document an install-location requirement
# (Apple's own sample is run straight from Xcode), but every Xcode build output
# is also registered with LaunchServices/PlugInKit, and two registered copies of
# the same bundle id make it unpredictable which one fskit_agent launches.
# Installing one copy in /Applications and unregistering the DerivedData copy
# avoids that.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_NAME="NTFS4Mac"
APP_ID="com.vahidzekic.ntfs4mac"
EXT_ID="com.vahidzekic.ntfs4mac.NTFSExtension"
EXT_POINT="com.apple.fskit.fsmodule"
SHORT_NAME="ntfs4mac"
DERIVED="$ROOT/build/DerivedData"
INSTALL_DIR="/Applications"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"

CONFIG="Debug"
DO_INSTALL=1
RESTART_AGENT=0
OPEN_SETTINGS=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)       CONFIG="Release" ;;
        --no-install)    DO_INSTALL=0 ;;
        --restart-agent) RESTART_AGENT=1 ;;
        --open-settings) OPEN_SETTINGS=1 ;;
        -h|--help)       sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

log()  { printf '\n==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Darwin" ]] || die "macOS only"
command -v xcodebuild >/dev/null || die "xcodebuild not found (install Xcode 16.3+ and run: sudo xcode-select -s /Applications/Xcode.app)"
command -v xcodegen  >/dev/null || die "xcodegen not found (brew install xcodegen)"

os_ver="$(sw_vers -productVersion)"
os_major="${os_ver%%.*}"; os_rest="${os_ver#*.}"; os_minor="${os_rest%%.*}"
if (( os_major < 15 )) || { (( os_major == 15 )) && (( os_minor < 4 )); }; then
    die "macOS $os_ver: FSKit modules need macOS 15.4 or later"
fi
if (( os_major == 15 )) && (( os_minor < 6 )); then
    warn "macOS $os_ver: DiskArbitration probing of FSKit modules is broken before 15.6 (FB17772372); use mount -F for testing"
fi

if [[ -z "${DEVELOPMENT_TEAM:-}" ]]; then
    warn "DEVELOPMENT_TEAM is not set. Without a team the extension cannot carry"
    warn "com.apple.developer.fskit.fsmodule and fskitd will refuse to load it."
    warn "Find your team id with: security find-identity -v -p codesigning"
fi

# 1. Third-party library -------------------------------------------------------
if [[ ! -f "$ROOT/ThirdParty/ntfs-3g/lib/libntfs-3g.a" || ! -f "$ROOT/ThirdParty/ntfs-3g/lib/libmkntfs.a" ]]; then
    log "ThirdParty/ntfs-3g missing; running scripts/build-libntfs3g.sh"
    "$ROOT/scripts/build-libntfs3g.sh"
fi

# 2. Project -------------------------------------------------------------------
log "xcodegen generate"
(cd "$ROOT" && xcodegen generate --spec project.yml)

# 3. Build ---------------------------------------------------------------------
log "xcodebuild ($CONFIG, arm64)"
build_args=(
    -project "$ROOT/$APP_NAME.xcodeproj"
    -scheme "$APP_NAME"
    -configuration "$CONFIG"
    -arch arm64
    -destination "platform=macOS,arch=arm64"
    -derivedDataPath "$DERIVED"
    -allowProvisioningUpdates
)
[[ -n "${DEVELOPMENT_TEAM:-}" ]] && build_args+=("DEVELOPMENT_TEAM=$DEVELOPMENT_TEAM")
if command -v xcbeautify >/dev/null; then
    xcodebuild "${build_args[@]}" build | xcbeautify
else
    xcodebuild "${build_args[@]}" build
fi

BUILT_APP="$DERIVED/Build/Products/$CONFIG/$APP_NAME.app"
BUILT_EXT="$BUILT_APP/Contents/Extensions/NTFSExtension.appex"
[[ -d "$BUILT_APP" ]] || die "build product missing: $BUILT_APP"
[[ -d "$BUILT_EXT" ]] || die "extension was not embedded at Contents/Extensions/NTFSExtension.appex (check the Embed ExtensionKit Extensions phase)"

# 4. Signature sanity ----------------------------------------------------------
log "checking code signature and entitlements"
codesign --verify --deep --strict "$BUILT_APP" || die "codesign verification failed"
ext_ents="$(codesign -d --entitlements - --xml "$BUILT_EXT" 2>/dev/null || true)"
if [[ "$ext_ents" != *"com.apple.developer.fskit.fsmodule"* ]]; then
    die "the extension is not signed with com.apple.developer.fskit.fsmodule; set DEVELOPMENT_TEAM and enable the FSKit Module capability for $EXT_ID"
fi
if [[ "$ext_ents" != *"com.apple.security.app-sandbox"* ]]; then
    die "the extension is not sandboxed"
fi
codesign -dv "$BUILT_EXT" 2>&1 | grep -E '^(Identifier|TeamIdentifier|Authority)=' | sed 's/^/    /' || true

if [[ $DO_INSTALL -eq 0 ]]; then
    log "built $BUILT_APP (not installed)"
    exit 0
fi

# 5. Install -------------------------------------------------------------------
DEST="$INSTALL_DIR/$APP_NAME.app"
log "installing to $DEST"
osascript -e "tell application id \"$APP_ID\" to quit" >/dev/null 2>&1 || true
# Stop idle extension instances so the new binary is used. Killing an
# instance that serves a mounted volume would drop that volume. `mount` shows
# the statfs type name the module reports (likely "ntfs"), not the short name,
# so detect a live volume by the extension holding a disk device open.
for pid in $(pgrep -f "Contents/Extensions/NTFSExtension.appex" 2>/dev/null || true); do
    fds="$(lsof -p "$pid" 2>/dev/null || true)"
    case "$fds" in
        *"/dev/disk"*|*"/dev/rdisk"*)
            die "NTFSExtension (pid $pid) is serving a mounted volume; unmount it first (umount <mountpoint>)" ;;
    esac
done
pkill -f "Contents/Extensions/NTFSExtension.appex" 2>/dev/null || true

# Unregister the DerivedData copy so only /Applications is registered.
pluginkit -r "$BUILT_EXT" >/dev/null 2>&1 || true
[[ -x "$LSREGISTER" ]] && "$LSREGISTER" -u "$BUILT_APP" >/dev/null 2>&1 || true

if [[ -e "$DEST" ]]; then
    rm -rf "$DEST" 2>/dev/null || sudo rm -rf "$DEST"
fi
if ! ditto "$BUILT_APP" "$DEST" 2>/dev/null; then
    sudo ditto "$BUILT_APP" "$DEST"
fi

# 6. Register ------------------------------------------------------------------
log "registering with LaunchServices and PlugInKit"
[[ -x "$LSREGISTER" ]] && "$LSREGISTER" -f -R -trusted "$DEST" || true
pluginkit -a "$DEST/Contents/Extensions/NTFSExtension.appex" || warn "pluginkit -a failed (the first launch below registers it too)"
# Launching the container app once is what reliably registers its extensions.
open -g -a "$DEST" || true
sleep 2

if [[ $RESTART_AGENT -eq 1 ]]; then
    log "restarting fskit_agent (launchd respawns it on demand)"
    pkill -9 -x fskit_agent 2>/dev/null || true
fi

log "pluginkit view of $EXT_POINT modules"
pluginkit -m -v -p "$EXT_POINT" | sed 's/^/    /' || true
if pluginkit -m -v -p "$EXT_POINT" 2>/dev/null | grep -q "$EXT_ID"; then
    echo "    -> $EXT_ID is registered."
else
    warn "$EXT_ID is not listed yet. Open $DEST once, then re-run: pluginkit -mAvvv -p $EXT_POINT"
fi

cat <<EOF

Next steps
----------
1. Enable the module (once per (re)install):
     System Settings > General > Login Items & Extensions > Extensions ("By Category")
     > File System Extensions (i) > turn on "NTFS (ntfs4mac)" > Done
   (open "x-apple.systempreferences:com.apple.LoginItems-Settings.extension")
   If the toggle does not stick, see docs/TESTING.md, "Troubleshooting".
2. Make a test volume and mount it (as your user, not with sudo):
     ThirdParty/ntfs-3g/bin/mkntfs -F -Q -L NTFSTEST /tmp/ntfs.img   # after: mkfile -n 512m /tmp/ntfs.img
     hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount /tmp/ntfs.img
     mkdir -p /tmp/ntfs && mount -F -t $SHORT_NAME /dev/diskN /tmp/ntfs
3. Watch the logs:
     log stream --level debug --predicate 'subsystem == "$EXT_ID"'
     log stream --predicate 'process IN {"fskitd","fskit_agent","diskarbitrationd"}'
Full guide: docs/TESTING.md
EOF

if [[ $OPEN_SETTINGS -eq 1 ]]; then
    open "x-apple.systempreferences:com.apple.LoginItems-Settings.extension" || true
fi
