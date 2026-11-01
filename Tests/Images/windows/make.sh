#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Makes Tests/Images/windows-content.img.zst and its .expect in the Windows VM (Step 07
# leftover). Needs WINDOWS_SSH (and WINDOWS_SSH_PORT), as Scripts/chkdsk-vm.sh does.
#
#   Tests/Images/windows/make.sh
#
# Windows builds and fills the volume (make-windows-golden.ps1) and reports what it sees in
# manifest.tsv; this script extracts the NTFS partition from the VHD and writes the
# expectations from the manifest, so none of them come from Unlatch.
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
here="$root/Tests/Images/windows"
work="$root/build/windows-golden"
: "${WINDOWS_SSH:?set WINDOWS_SSH=user@host (docs/testing.md)}"
port="${WINDOWS_SSH_PORT:-22}"
ssh() { command ssh -o BatchMode=yes -p "$port" "$WINDOWS_SSH" "$@"; }
scp() { command scp -o BatchMode=yes -P "$port" "$@"; }

mkdir -p "$work"
# Windows PowerShell 5.1 reads a script without a BOM as ANSI: add one for the Unicode names.
printf '\xef\xbb\xbf' > "$work/make-windows-golden.ps1"
cat "$here/make-windows-golden.ps1" >> "$work/make-windows-golden.ps1"
ssh 'powershell -NoProfile -Command "New-Item -ItemType Directory -Force C:\unlatch\golden | Out-Null"'
scp -q "$work/make-windows-golden.ps1" "$WINDOWS_SSH:C:/unlatch/golden/make-windows-golden.ps1"
ssh 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\unlatch\golden\make-windows-golden.ps1 -Dir C:\unlatch\golden'
scp -q "$WINDOWS_SSH:C:/unlatch/golden/windows-content.vhd" "$work/windows-content.vhd"
scp -q "$WINDOWS_SSH:C:/unlatch/golden/manifest.tsv" "$work/manifest.tsv"
ver=$(ssh ver | tr -d '\r' | grep -o 'Version [0-9.]*' || true)

offset=$(tr -d '\r' < "$work/manifest.tsv" | awk -F'\t' '$1 == "offset" {print $2}')
size=$(( $(stat -f %z "$work/windows-content.vhd") - 512 - offset ))     # less the VHD footer
dd if="$work/windows-content.vhd" of="$work/windows-content.img" bs=512 skip=$((offset / 512)) \
   count=$((size / 512)) status=none
zstd -q -19 -f "$work/windows-content.img" -o "$root/Tests/Images/windows-content.img.zst"
python3 -I "$here/manifest-to-expect.py" "$work/manifest.tsv" "${ver:-Windows}" > "$root/Tests/Images/windows-content.expect"
echo "make: Tests/Images/windows-content.img.zst and .expect ($ver)"
