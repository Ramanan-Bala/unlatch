#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the third-party notices bundle (plan Step 18) into <dir>: Unlatch's GPL-3.0 text,
# libntfs-3g's COPYING, COPYING.LIB, AUTHORS and CREDITS, swift-argument-parser's license,
# and NOTICES.md with a written source offer for <version>. The app carries it in
# Contents/Resources/Notices; the DMG, the source tarball and the Homebrew install carry a copy.
# macFUSE is not part of Unlatch and is never bundled: its license forbids redistribution.
#
#   Scripts/make-notices.sh <dir> [version]          version defaults to the VERSION file
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:?usage: $0 <dir> [version]}"
version="${2:-$(cat "$root/VERSION")}"
ntfs3g="$(sed -n 's/^tag=//p' "$root/Engine/vendor/VERSION")"
argparser="$(awk -F'"' '/"version"/ {print $4; exit}' "$root/CLI/Package.resolved")"
repo="https://github.com/Ramanan-Bala/unlatch"

mkdir -p "$out"
cp -f "$root/LICENSE" "$out/Unlatch-LICENSE.txt"
for f in COPYING COPYING.LIB AUTHORS CREDITS; do
    cp -f "$root/Engine/vendor/ntfs-3g/$f" "$out/ntfs-3g-$f.txt"
done
cp -f "$root/Packaging/notices/swift-argument-parser-LICENSE.txt" "$out/swift-argument-parser-LICENSE.txt"
cat > "$out/NOTICES.md" <<NOTICES
# Unlatch $version: licenses and source

Unlatch is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version (Unlatch-LICENSE.txt). It comes with ABSOLUTELY
NO WARRANTY.

## Source

The complete corresponding source code of this release, including the libntfs-3g source it was
built from and the build scripts, is at $repo, tag v$version
($repo/tree/v$version), and in the source tarball attached to the release
($repo/releases/tag/v$version). On request, the maintainers will send the same source on a
physical medium for no more than the cost of making and sending it, for at least three years
after the release; open an issue at $repo/issues.

## Third-party software included

| Component | Version | License | Files |
|---|---|---|---|
| libntfs-3g and ntfsprogs (Tuxera, Jean-Pierre André and contributors) | $ntfs3g | GPL-2.0-or-later (library parts LGPL-2.0-or-later) | ntfs-3g-COPYING.txt, ntfs-3g-COPYING.LIB.txt, ntfs-3g-AUTHORS.txt, ntfs-3g-CREDITS.txt |
| swift-argument-parser (Apple Inc.) | $argparser | Apache-2.0 with Runtime Library Exception | swift-argument-parser-LICENSE.txt |

libntfs-3g is linked into unlatch-fuse; it is built from Engine/vendor/ntfs-3g plus the patches in
Engine/vendor/patches.

## Not included

macFUSE (https://macfuse.github.io) is required at run time and installed separately by the
user. Unlatch does not include or redistribute any part of it. unlatch-fuse links the libfuse3
library that the macFUSE installer puts in /usr/local/lib.
NOTICES
echo "$out"
