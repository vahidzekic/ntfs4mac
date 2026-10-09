#!/bin/bash
# uninstall.sh — remove NTFS4Mac (app, FSKit extension and file-system bundle).
# Unmount any NTFS4Mac volumes first.
set -euo pipefail

if mount | grep -E "\(ntfs.*fskit" >/dev/null; then
    echo "Unmount NTFS volumes mounted through NTFS4Mac first:" >&2
    mount | grep -E "\(ntfs.*fskit" >&2
    exit 1
fi

APP=/Applications/NTFS4Mac.app
pluginkit -r "$APP/Contents/Extensions/NTFSExtension.appex" 2>/dev/null || true
sudo rm -rf "$APP" /Library/Filesystems/ntfs4mac.fs
sudo pkgutil --forget com.vahidzekic.ntfs4mac.pkg >/dev/null 2>&1 || true
sudo killall storagekitd 2>/dev/null || true
killall fskit_agent 2>/dev/null || true
echo "NTFS4Mac removed."
