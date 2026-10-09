# Releasing NTFS4Mac

`scripts/make-release.sh` builds a Release archive (universal: Intel and
Apple Silicon), packages `/Applications/NTFS4Mac.app` plus
`/Library/Filesystems/ntfs4mac.fs` into one installer, and prints the
commands to publish it on GitHub.

## Choose a signing mode

| Mode | Who can install it | What you need |
|------|--------------------|---------------|
| `development` (default) | Only Macs registered in your team's development profile | What `scripts/dev-install.sh` already uses |
| `developer-id` | Anyone (Gatekeeper accepts it once notarized) | Developer ID certificates, FSKit capability on a Developer ID profile, notarization credentials |

A **public GitHub release should use `developer-id`**. A development-signed
installer works on your own Mac, but the extension will be rejected on
other Macs because their device IDs aren't in the provisioning profile.

## One-time setup for Developer ID

1. **Certificates** (Xcode › Settings › Accounts › your team › Manage
   Certificates › +): create **Developer ID Application** and **Developer ID
   Installer**. Only the team's Account Holder can create them.
2. **FSKit capability:** on developer.apple.com › Certificates, Identifiers &
   Profiles › Identifiers, make sure `com.vahidzekic.ntfs4mac.NTFSExtension`
   has **FSKit Module** enabled. Xcode's automatic signing then creates the
   Developer ID provisioning profile during export.
3. **Notarization credentials** (app-specific password from
   account.apple.com):
   ```sh
   xcrun notarytool store-credentials ntfs4mac-notary \
       --apple-id you@example.com --team-id MW256GPMA4 --password abcd-efgh-ijkl-mnop
   ```
4. **GitHub CLI:** `brew install gh && gh auth login`.

## Build

```sh
git checkout main && git pull
./scripts/build-libntfs3g.sh            # once; universal libraries

# public release
DEVELOPMENT_TEAM=MW256GPMA4 \
DEVELOPER_ID_INSTALLER="Developer ID Installer: Your Name (MW256GPMA4)" \
NOTARY_PROFILE=ntfs4mac-notary \
./scripts/make-release.sh --signing developer-id

# or: personal / registered-tester build
DEVELOPMENT_TEAM=MW256GPMA4 ./scripts/make-release.sh
```

Add `ARCHS=x86_64` for an Intel-only installer (`NTFS4Mac-<version>-intel.pkg`)
or `ARCHS=arm64` for Apple Silicon; the default is universal.

Output in `build/release/`: `NTFS4Mac-<version>.pkg`, the libntfs-3g source
tarball (the GPL requires shipping or offering it), and `SHA256SUMS`.

## Test the installer before publishing

```sh
sudo installer -pkg build/release/NTFS4Mac-0.1.0*.pkg -target /
diskutil listFilesystems | grep NTFS4Mac
pkgutil --check-signature build/release/NTFS4Mac-0.1.0*.pkg
spctl -a -vv -t install build/release/NTFS4Mac-0.1.0*.pkg   # "accepted" once notarized
```

Enable the extension (By Category), format a RAM disk from Disk Utility, copy
files in Finder, eject.

## Publish

```sh
git tag -a v0.1.0 -m "NTFS4Mac 0.1.0" && git push origin v0.1.0
gh release create v0.1.0 --prerelease --title "NTFS4Mac 0.1.0" \
   --notes-file docs/release-notes/v0.1.0.md \
   build/release/NTFS4Mac-0.1.0*.pkg build/release/*.tgz build/release/SHA256SUMS
```

Without `gh`: GitHub › Releases › Draft a new release, pick the tag, paste
`docs/release-notes/v0.1.0.md`, attach the three files, tick "Set as a
pre-release".

If the build is development-signed, add a line to the release notes saying it
only runs on Macs registered to the team.

## New version

Bump `MARKETING_VERSION` (and `CURRENT_PROJECT_VERSION`) in `project.yml`,
move the `[Unreleased]` entries in `CHANGELOG.md` under the new version,
add `docs/release-notes/v<version>.md`, then repeat Build/Publish.

## Uninstall

`scripts/uninstall.sh` removes the app, the extension registration and the
file-system bundle.
