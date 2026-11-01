#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: Scripts/make-image.sh <name> <size> [label]
# Creates a sparse <name>.img (size as for mkfile: 512m, 4g, ...) in the current
# directory, formats it NTFS with the vendored mkntfs, and prints its path.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
mkntfs="$root/build/ntfs3g/bin/mkntfs"

[ $# -ge 2 ] && [ $# -le 3 ] || { echo "usage: $0 <name> <size> [label]" >&2; exit 64; }
name="$1"; size="$2"; label="${3:-$1}"
img="$PWD/${name%.img}.img"

[ -x "$mkntfs" ] || { echo "make-image: $mkntfs missing; run Scripts/build-ntfs3g.sh" >&2; exit 1; }
[ -e "$img" ] && { echo "make-image: $img already exists" >&2; exit 1; }

mkfile -n "$size" "$img"
# -F: target is a file, not a block device. -Q: quick format (no zeroing, no bad-sector scan).
if ! log="$("$mkntfs" -F -Q -L "$label" "$img" 2>&1)"; then
    rm -f "$img"
    echo "make-image: mkntfs failed:" >&2
    echo "$log" >&2
    exit 1
fi

# mkntfs seeds its serial from the clock, so images made in the same second share one, and
# status records are keyed by serial (D-41): give each image a random serial.
serial=$(od -An -N8 -tx8 /dev/urandom | tr -d ' \n')
"$root/build/ntfs3g/bin/ntfslabel" --new-serial="$serial" "$img" > /dev/null 2>&1 || true

echo "$img"
