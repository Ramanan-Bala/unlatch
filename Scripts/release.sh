#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds a release (plan Step 18, re-scoped by D-69: no Developer ID, so no notarization).
#
#   Scripts/release.sh [--allow-dirty]
#
# Into build/release/<version>/:
#   Unlatch-<version>.dmg          the app (ad-hoc signed, hardened runtime), an Applications
#                                  link, "Read Me First.txt" and the notices
#   unlatch-<version>.tar.gz       the source of this commit (git archive; Git LFS test images
#                                  are pointers, as in any GitHub source archive)
#   SHA256SUMS
#   homebrew/                      the tap's cask and formula with this version and checksums
# It publishes nothing: docs/release.md has the steps that do, each needing the owner.
# macFUSE is never bundled (its license forbids it); the cask depends on the macfuse cask.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
allow_dirty=0
[ "${1:-}" = --allow-dirty ] && allow_dirty=1

Scripts/set-version.sh --check > /dev/null
version="$(cat VERSION)"
if [ $allow_dirty = 0 ] && [ -n "$(git status --porcelain)" ]; then
    echo "release: the working tree has changes; commit them or pass --allow-dirty" >&2
    exit 1
fi
out="$root/build/release/$version"
dd="$root/build/release/DerivedData"
rm -rf "$out"
mkdir -p "$out"

echo "release: building $version (Release, arm64)"
[ -f build/ntfs3g/lib/libntfs-3g.a ] || Scripts/build-ntfs3g.sh
[ -d Unlatch.xcodeproj ] || Scripts/bootstrap.sh > /dev/null
xcodebuild -quiet -scheme Unlatch -configuration Release -derivedDataPath "$dd" ARCHS=arm64 build
app_src="$dd/Build/Products/Release/Unlatch.app"
stage="$out/stage"
mkdir -p "$stage"
ditto "$app_src" "$stage/Unlatch.app"
app="$stage/Unlatch.app"

echo "release: signing ad-hoc, inside out, hardened runtime"
for f in "$app/Contents/Helpers/"*; do
    ent=()
    if [ "$(basename "$f")" = unlatch-fuse ]; then ent=(--entitlements Packaging/unlatch-fuse.entitlements); fi
    codesign --force --options runtime --timestamp=none ${ent[@]+"${ent[@]}"} --sign - "$f"
done
codesign --force --options runtime --timestamp=none --sign - "$app"
codesign --verify --deep --strict --verbose=1 "$app"
# The daemon must still load macFUSE's libfuse3 under the hardened runtime.
"$app/Contents/Helpers/unlatch-fuse" --help 2>&1 | grep -q '^usage: unlatch-fuse' \
    || { echo "release: unlatch-fuse does not start (library validation?)" >&2; exit 1; }
# Expected to be rejected: an ad-hoc signature is not a Developer ID (docs/install.md says how
# to open the app anyway). Printed so the release notes can quote it.
spctl -a -vv "$app" > "$out/spctl.txt" 2>&1 || true
echo "release: Gatekeeper says: $(head -1 "$out/spctl.txt")"

echo "release: DMG"
ln -s /Applications "$stage/Applications"
Scripts/make-notices.sh "$stage/Notices" "$version" > /dev/null
cat > "$stage/Read Me First.txt" <<README
Unlatch $version: read-write NTFS for macOS 27+ on Apple Silicon.

1. Drag Unlatch to Applications.
2. Open it: right-click Unlatch in Applications > Open > Open (it is not signed with an Apple
   Developer ID). If there is no Open button: System Settings > Privacy & Security > Open Anyway.
3. The setup window installs macFUSE for you (from macFUSE's own releases, signature checked),
   shows how to turn on macFUSE's file system extension (System Settings > General > Login
   Items & Extensions; no Recovery, no restart), and installs the helper real disks need.

Step by step: https://github.com/Ramanan-Bala/unlatch#one-time-setup
Licenses and the source offer are in the Notices folder. Unlatch does not include macFUSE.
README
dmg="$out/Unlatch-$version.dmg"
hdiutil create -quiet -fs HFS+ -format UDZO -volname "Unlatch $version" -srcfolder "$stage" "$dmg"

echo "release: source tarball"
git archive --format=tar --prefix="unlatch-$version/" HEAD | gzip -9n > "$out/unlatch-$version.tar.gz"

(cd "$out" && shasum -a 256 "Unlatch-$version.dmg" "unlatch-$version.tar.gz" > SHA256SUMS)
dmg_sha=$(awk 'NR == 1 {print $1}' "$out/SHA256SUMS")
src_sha=$(awk 'NR == 2 {print $1}' "$out/SHA256SUMS")

echo "release: Homebrew tap files"
mkdir -p "$out/homebrew/Casks" "$out/homebrew/Formula"
sed -e "s/@VERSION@/$version/g" -e "s/@SHA256@/$dmg_sha/g" Packaging/homebrew/Casks/unlatch.rb.in \
    > "$out/homebrew/Casks/unlatch.rb"
sed -e "s/@VERSION@/$version/g" -e "s/@SHA256@/$src_sha/g" Packaging/homebrew/Formula/unlatch.rb.in \
    > "$out/homebrew/Formula/unlatch.rb"
rm -rf "$stage"
echo "release: done: $out"
ls -l "$out"
