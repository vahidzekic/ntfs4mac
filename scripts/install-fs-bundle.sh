#!/bin/bash
# install-fs-bundle.sh — install (or remove) /Library/Filesystems/ntfs4mac.fs.
#
# The bundle makes "NTFS" appear in Disk Utility's Erase > Format menu and in
# `diskutil listFilesystems` / `diskutil eraseVolume NTFS4Mac ...`. Its helper
# tools only forward to newfs_fskit / fsck_fskit / mount -F for the ntfs4mac
# FSKit module, which must be installed and enabled (scripts/dev-install.sh).
#
# Usage:  scripts/install-fs-bundle.sh            # install / update (asks for sudo)
#         scripts/install-fs-bundle.sh --uninstall
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/FilesystemBundle/ntfs4mac.fs"
DEST="/Library/Filesystems/ntfs4mac.fs"

log() { printf '\n==> %s\n' "$*"; }

if [[ "${1:-}" == "--uninstall" ]]; then
    log "removing $DEST"
    sudo rm -rf "$DEST"
    sudo killall storagekitd 2>/dev/null || true
    exit 0
fi

[[ -f "$SRC/Contents/Info.plist" ]] || { echo "missing $SRC" >&2; exit 1; }
plutil -lint "$SRC/Contents/Info.plist" >/dev/null

log "installing $DEST"
sudo rm -rf "$DEST"
sudo ditto "$SRC" "$DEST"
sudo chown -R root:wheel "$DEST"
sudo chmod 755 "$DEST/Contents/Resources/"*_ntfs4mac
# Ad-hoc signature so the bundle has a valid (if anonymous) code signature.
sudo codesign --force --deep --sign - "$DEST" >/dev/null 2>&1 || \
    echo "warning: ad-hoc codesign failed; continuing unsigned" >&2

# storagekitd (behind diskutil and Disk Utility) caches the file-system list.
sudo killall storagekitd 2>/dev/null || true

log "diskutil listFilesystems"
diskutil listFilesystems | grep -i -E "PERSONALITY|NTFS" || true

cat <<'MSG'

If "NTFS4Mac  NTFS" is listed above, restart Disk Utility; NTFS should now be
in Erase > Format. From Terminal:
    diskutil eraseVolume NTFS4Mac MYDISK /dev/diskXsY
    diskutil eraseDisk   NTFS4Mac MYDISK GPT /dev/diskX
The ntfs4mac FSKit module must be enabled for these to work.
MSG
