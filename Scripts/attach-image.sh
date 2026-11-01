#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: Scripts/attach-image.sh <image.img>
# Attaches a raw image as a block device without mounting it and prints the diskN.
# Detach with: hdiutil detach diskN
set -euo pipefail

[ $# -eq 1 ] || { echo "usage: $0 <image.img>" >&2; exit 64; }
[ -f "$1" ] || { echo "attach-image: $1 not found" >&2; exit 1; }

out="$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$1")"
disk="$(printf '%s\n' "$out" | awk 'NR==1 { sub("^/dev/", "", $1); print $1 }')"
[ -n "$disk" ] || { echo "attach-image: could not parse hdiutil output:" >&2; echo "$out" >&2; exit 1; }

echo "$disk"
