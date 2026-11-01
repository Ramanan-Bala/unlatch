#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Windows chkdsk oracle (Step 06). Wraps an NTFS image as a fixed VHD and checks it with
# Windows `chkdsk` (read-only: no /f), then saves the report in Tests/oracle/.
#
#   Scripts/chkdsk-vm.sh <volume.img> [name]   check an existing image
#   Scripts/chkdsk-vm.sh --fsx <seed> [ops]    make a fresh image with fsx first, then check it
#
# With WINDOWS_SSH=user@host (a Windows VM with OpenSSH Server, see docs/testing.md) the
# check runs automatically. Without it, the script prints the manual steps.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
out="$root/build/oracle"
mkdir -p "$out" Tests/oracle
clang -std=c11 -O2 -Wall -Wextra -Werror -o "$out/mkvhd" Tests/oracle/mkvhd.c

if [ "${1:-}" = --fsx ]; then
    seed="${2:?seed}" ops="${3:-2000}" name="fsx-seed$seed-$ops"
    eng="$root/build/DerivedData/Build/Products/Debug/unlatch-engine"
    [ -x "$eng" ] || xcodebuild -scheme unlatch-engine -derivedDataPath build/DerivedData build > /dev/null
    rm -f "$out/$name.img"
    (cd "$out" && "$root/Scripts/make-image.sh" "$name" 512m FSX > /dev/null)
    "$eng" fsx "$out/$name.img" --seed "$seed" --ops "$ops"
    img="$out/$name.img"
else
    img="${1:?usage: $0 <volume.img> [name] | --fsx <seed> [ops]}"
    name="${2:-$(basename "$img" .img)}"
fi
vhd="$out/$name.vhd"
"$out/mkvhd" "$img" "$vhd" > /dev/null
shasum -a 256 "$img" | cut -d' ' -f1 > "$out/$name.img.sha256"
echo "chkdsk-vm: $vhd (volume sha256 $(cat "$out/$name.img.sha256"))"

report="Tests/oracle/$name.chkdsk.txt"
if [ -z "${WINDOWS_SSH:-}" ]; then
    cat <<MANUAL
chkdsk-vm: WINDOWS_SSH is not set; run the check by hand (docs/testing.md):
  1. Copy $vhd into the Windows VM (shared folder, or scp to C:\\unlatch\\).
  2. In an elevated PowerShell:
       \$d = Mount-DiskImage -ImagePath C:\\unlatch\\$name.vhd -Access ReadOnly -PassThru
       \$l = (\$d | Get-Disk | Get-Partition | Get-Volume).DriveLetter
       chkdsk "\${l}:" | Tee-Object C:\\unlatch\\$name.chkdsk.txt
       Dismount-DiskImage -ImagePath C:\\unlatch\\$name.vhd
  3. Copy $name.chkdsk.txt back to $report.
MANUAL
    exit 2
fi

ssh "$WINDOWS_SSH" 'powershell -NoProfile -Command "New-Item -ItemType Directory -Force C:\unlatch | Out-Null"'
scp -q "$vhd" "$WINDOWS_SSH:C:/unlatch/$name.vhd"
ssh "$WINDOWS_SSH" "powershell -NoProfile -Command \"\$ErrorActionPreference='Stop'; \
  \$d = Mount-DiskImage -ImagePath C:\\unlatch\\$name.vhd -Access ReadOnly -PassThru; \
  Start-Sleep 2; \$l = (\$d | Get-Disk | Get-Partition | Get-Volume).DriveLetter; \
  chkdsk (\$l + ':') | Out-File -Encoding utf8 C:\\unlatch\\$name.chkdsk.txt; \
  Dismount-DiskImage -ImagePath C:\\unlatch\\$name.vhd | Out-Null\""
scp -q "$WINDOWS_SSH:C:/unlatch/$name.chkdsk.txt" "$report"
{ echo "# image: $name.img sha256 $(cat "$out/$name.img.sha256")"; echo "# checked: $(date -u +%FT%TZ)"; } >> "$report"
if grep -q "found no problems" "$report"; then
    echo "chkdsk-vm: PASSED ($report)"
else
    cat "$report" >&2
    echo "chkdsk-vm: chkdsk reported problems ($report)" >&2
    exit 1
fi
