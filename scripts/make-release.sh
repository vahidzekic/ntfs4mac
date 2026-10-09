#!/bin/bash
# make-release.sh — build NTFS4Mac in Release mode and package it as a macOS
# installer (.pkg) for a GitHub release.
#
# Usage:
#   DEVELOPMENT_TEAM=ABCDE12345 scripts/make-release.sh [--signing development|developer-id]
#
# Signing modes:
#   development   (default) Apple Development signature, exactly like
#                 scripts/dev-install.sh. The app only runs on Macs registered
#                 in your team's development provisioning profile. Good for
#                 your own machines and testers whose Macs you register.
#   developer-id  Developer ID signature for public distribution. Needs a
#                 "Developer ID Application" certificate and a Developer ID
#                 provisioning profile for com.vahidzekic.ntfs4mac.NTFSExtension
#                 with the FSKit Module capability (Xcode creates it with
#                 automatic signing). Set DEVELOPER_ID_INSTALLER to sign the
#                 .pkg and NOTARY_PROFILE to notarize and staple it.
#
# Environment:
#   DEVELOPMENT_TEAM        team ID (required)
#   VERSION                 defaults to MARKETING_VERSION in project.yml
#   ARCHS                   "arm64 x86_64" (universal, default), "x86_64" (Intel) or "arm64"
#   DEVELOPER_ID_INSTALLER  e.g. "Developer ID Installer: Name (TEAMID)"
#   NOTARY_PROFILE          keychain profile from `xcrun notarytool store-credentials`
#
# Output (build/release/):
#   NTFS4Mac-<version>[-intel|-apple-silicon].pkg   installer
#   ntfs-3g_ntfsprogs-<ver>.tgz           corresponding libntfs-3g source (GPL)
#   SHA256SUMS
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/release"
SIGNING="development"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --signing) SIGNING="${2:?}"; shift ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
case "$SIGNING" in development|developer-id) ;; *) echo "bad --signing: $SIGNING" >&2; exit 2 ;; esac

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

: "${DEVELOPMENT_TEAM:?set DEVELOPMENT_TEAM to your team ID}"
VERSION="${VERSION:-$(awk -F'"' '/MARKETING_VERSION:/ {print $2; exit}' "$ROOT/project.yml")}"
[[ -n "$VERSION" ]] || die "could not read MARKETING_VERSION from project.yml"
ARCHS="${ARCHS:-arm64 x86_64}"
PKG_ID="com.vahidzekic.ntfs4mac.pkg"

command -v xcodegen >/dev/null || die "xcodegen not found (brew install xcodegen)"

# productbuild and notarytool need local credentials; check them before the
# long build instead of failing at the end.
if [[ -n "${DEVELOPER_ID_INSTALLER:-}" ]]; then
    security find-identity -v | grep -qF "$DEVELOPER_ID_INSTALLER" || die "\
no '$DEVELOPER_ID_INSTALLER' identity in the keychain.
Create it in Xcode > Settings > Accounts > (team) > Manage Certificates > + >
Developer ID Installer (Account Holder only), then check with:
  security find-identity -v | grep 'Developer ID Installer'"
fi
if [[ -n "${NOTARY_PROFILE:-}" ]]; then
    xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1 || die "\
notary profile '$NOTARY_PROFILE' not found or invalid. Create it with:
  xcrun notarytool store-credentials $NOTARY_PROFILE --apple-id <apple-id> --team-id $DEVELOPMENT_TEAM"
fi

# 1. Third-party libraries ------------------------------------------------------
LIB="$ROOT/ThirdParty/ntfs-3g/lib"
if [[ ! -f "$LIB/libntfs-3g.a" || ! -f "$LIB/libmkntfs.a" ]]; then
    log "building libntfs-3g"
    "$ROOT/scripts/build-libntfs3g.sh"
fi
for arch in $ARCHS; do
    for lib in libntfs-3g.a libmkntfs.a; do
        lipo -archs "$LIB/$lib" | tr ' ' '\n' | grep -qx "$arch" \
            || die "$lib has no $arch slice; re-run scripts/build-libntfs3g.sh"
    done
done

rm -rf "$OUT"
mkdir -p "$OUT"
ARCHIVE="$OUT/NTFS4Mac.xcarchive"

# 2. Archive --------------------------------------------------------------------
log "xcodegen generate"
(cd "$ROOT" && xcodegen generate --spec project.yml)

log "xcodebuild archive (Release, $ARCHS, $SIGNING)"
xcodebuild archive \
    -project "$ROOT/NTFS4Mac.xcodeproj" \
    -scheme NTFS4Mac \
    -configuration Release \
    -destination "generic/platform=macOS" \
    -archivePath "$ARCHIVE" \
    -derivedDataPath "$ROOT/build/DerivedData-release" \
    -allowProvisioningUpdates \
    -allowProvisioningDeviceRegistration \
    DEVELOPMENT_TEAM="$DEVELOPMENT_TEAM" \
    ARCHS="$ARCHS" ONLY_ACTIVE_ARCH=NO \
    MARKETING_VERSION="$VERSION"

# 3. Export the app -------------------------------------------------------------
APP="$OUT/export/NTFS4Mac.app"
if [[ "$SIGNING" == "developer-id" ]]; then
    log "exporting with Developer ID"
    cat > "$OUT/ExportOptions.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>method</key><string>developer-id</string>
    <key>teamID</key><string>$DEVELOPMENT_TEAM</string>
    <key>signingStyle</key><string>automatic</string>
</dict>
</plist>
EOF
    xcodebuild -exportArchive \
        -archivePath "$ARCHIVE" \
        -exportOptionsPlist "$OUT/ExportOptions.plist" \
        -exportPath "$OUT/export" \
        -allowProvisioningUpdates
else
    # The archive's app already carries the Apple Development signature.
    mkdir -p "$OUT/export"
    ditto "$ARCHIVE/Products/Applications/NTFS4Mac.app" "$APP"
fi
[[ -d "$APP" ]] || die "export produced no NTFS4Mac.app"

log "verifying the app"
codesign --verify --deep --strict "$APP"
APPEX="$APP/Contents/Extensions/NTFSExtension.appex"
codesign -d --entitlements - "$APPEX" 2>/dev/null | grep -q "com.apple.developer.fskit.fsmodule" \
    || die "extension lacks com.apple.developer.fskit.fsmodule"
for arch in $ARCHS; do
    lipo -archs "$APPEX/Contents/MacOS/NTFSExtension" | tr ' ' '\n' | grep -qx "$arch" \
        || die "extension binary has no $arch slice"
done
echo "    architectures: $(lipo -archs "$APPEX/Contents/MacOS/NTFSExtension")"

# 4. Payload: app + file-system bundle -----------------------------------------
log "staging payload"
PAYLOAD="$OUT/payload"
mkdir -p "$PAYLOAD/Applications" "$PAYLOAD/Library/Filesystems"
ditto "$APP" "$PAYLOAD/Applications/NTFS4Mac.app"
ditto "$ROOT/FilesystemBundle/ntfs4mac.fs" "$PAYLOAD/Library/Filesystems/ntfs4mac.fs"
chmod 755 "$PAYLOAD/Library/Filesystems/ntfs4mac.fs/Contents/Resources/"*_ntfs4mac
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" \
    "$PAYLOAD/Library/Filesystems/ntfs4mac.fs/Contents/Info.plist"
FS_SIGN_ID="-"
if [[ "$SIGNING" == "developer-id" ]]; then
    # Xcode's export can sign the app with a cloud-managed Developer ID
    # certificate, so a local identity may not exist. The fs bundle holds only
    # a plist and shell scripts (no Mach-O), so an ad-hoc signature is enough.
    FS_SIGN_ID="$(security find-identity -v -p codesigning | awk -F'"' "/Developer ID Application:.*\\($DEVELOPMENT_TEAM\\)/ {print \$2; exit}")"
    if [[ -z "$FS_SIGN_ID" ]]; then
        echo "    no local Developer ID Application identity; signing ntfs4mac.fs ad hoc" >&2
        FS_SIGN_ID="-"
    fi
fi
if [[ "$FS_SIGN_ID" == "-" ]]; then
    codesign --force --sign - "$PAYLOAD/Library/Filesystems/ntfs4mac.fs"
else
    codesign --force --timestamp --sign "$FS_SIGN_ID" "$PAYLOAD/Library/Filesystems/ntfs4mac.fs"
fi

# 5. Component and product packages --------------------------------------------
log "pkgbuild"
COMPONENTS="$OUT/components.plist"
pkgbuild --analyze --root "$PAYLOAD" "$COMPONENTS"
# Install exactly where we say, never into a copy of the app found elsewhere.
i=0
while /usr/libexec/PlistBuddy -c "Print :$i" "$COMPONENTS" >/dev/null 2>&1; do
    # The analysed entries don't always carry the key (e.g. the fs bundle):
    # drop it if present, then add it with the value we want.
    /usr/libexec/PlistBuddy -c "Delete :$i:BundleIsRelocatable" "$COMPONENTS" >/dev/null 2>&1 || true
    /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$COMPONENTS"
    i=$((i + 1))
done
pkgbuild \
    --root "$PAYLOAD" \
    --component-plist "$COMPONENTS" \
    --identifier "$PKG_ID" \
    --version "$VERSION" \
    --scripts "$ROOT/packaging/scripts" \
    --install-location / \
    "$OUT/NTFS4Mac-component.pkg"

log "productbuild"
RES="$OUT/resources"
mkdir -p "$RES"
cp "$ROOT/packaging/resources/"*.html "$RES/"
cp "$ROOT/LICENSE" "$RES/LICENSE.txt"
sed "s/@VERSION@/$VERSION/g" "$ROOT/packaging/Distribution.xml" > "$OUT/Distribution.xml"
# Name the installer after its architectures: -intel / -apple-silicon, or
# nothing for a universal build.
case "$(echo "$ARCHS" | xargs)" in
    x86_64) PKG_SUFFIX="-intel" ;;
    arm64)  PKG_SUFFIX="-apple-silicon" ;;
    *)      PKG_SUFFIX="" ;;
esac
PKG="$OUT/NTFS4Mac-$VERSION$PKG_SUFFIX.pkg"
sign_args=()
if [[ -n "${DEVELOPER_ID_INSTALLER:-}" ]]; then
    sign_args=(--sign "$DEVELOPER_ID_INSTALLER" --timestamp)
fi
productbuild \
    --distribution "$OUT/Distribution.xml" \
    --resources "$RES" \
    --package-path "$OUT" \
    ${sign_args[@]+"${sign_args[@]}"} \
    "$PKG"

# 6. Notarize (Developer ID only) ----------------------------------------------
if [[ "$SIGNING" == "developer-id" && -n "${NOTARY_PROFILE:-}" ]]; then
    [[ -n "${DEVELOPER_ID_INSTALLER:-}" ]] || die "notarization needs a signed pkg (DEVELOPER_ID_INSTALLER)"
    log "notarizing"
    xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$PKG"
fi

# 7. GPL source and checksums ---------------------------------------------------
NTFS3G_TGZ="$(find "$ROOT/build/ntfs-3g" -maxdepth 1 -name 'ntfs-3g_ntfsprogs-*.tgz' 2>/dev/null | head -1 || true)"
if [[ -n "$NTFS3G_TGZ" ]]; then
    cp "$NTFS3G_TGZ" "$OUT/"
else
    echo "warning: libntfs-3g source tarball not found; attach it to the release by hand (GPL)" >&2
fi

rm -rf "$PAYLOAD" "$OUT/export" "$RES" "$OUT/NTFS4Mac-component.pkg" \
       "$OUT/components.plist" "$OUT/Distribution.xml" "$OUT/ExportOptions.plist"
(cd "$OUT" && shasum -a 256 ./*.pkg ./*.tgz 2>/dev/null > SHA256SUMS)

log "done"
ls -lh "$OUT"
pkgutil --check-signature "$PKG" || true
cat <<MSG

Installer: $PKG
Signing:   $SIGNING$( [[ "$SIGNING" == development ]] && echo " (runs only on Macs registered in team $DEVELOPMENT_TEAM)")

Publish (needs the GitHub CLI: brew install gh && gh auth login):
  git tag -a v$VERSION -m "NTFS4Mac $VERSION" && git push origin v$VERSION
  gh release create v$VERSION --prerelease --title "NTFS4Mac $VERSION" \\
     --notes-file docs/release-notes/v$VERSION.md \\
     "$PKG" "$OUT"/*.tgz "$OUT/SHA256SUMS"
MSG
