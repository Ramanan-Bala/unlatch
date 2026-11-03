#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Windows chkdsk oracle (Step 06). Wraps an NTFS image as a fixed VHD and checks it with
# Windows `chkdsk` (read-only: no /f), then saves the report in Tests/oracle/.
#
#   Scripts/chkdsk-vm.sh <volume.img> [name]   check an existing image
#   Scripts/chkdsk-vm.sh --fsx <seed> [ops]    make a fresh image with fsx first, then check it
#   Scripts/chkdsk-vm.sh --scan <img>...       check many images with read-only `chkdsk X:` (D-101):
#       each report goes to build/chkdsk/<name>.chkdsk.txt (CHKDSK_OUT sets the folder), one
#       line per image to its summary.tsv (name, clean|problems|error, chkdsk's exit code);
#       exits 1 when any image is not clean. Names are the images' basenames, so keep them apart.
#   Scripts/chkdsk-vm.sh --detail <img>...     the same with `chkdsk X: /f` on a throwaway read-write
#       copy, for the corrections read-only mode does not list (<name>.chkdsk-f.txt, detail.tsv)
#
# With WINDOWS_SSH=user@host (a Windows VM with OpenSSH Server, see docs/testing.md) the
# check runs automatically; WINDOWS_SSH_PORT sets the port (UTM port forward: 2222).
# Without it, the script prints the manual steps.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
out="$root/build/oracle"
mkdir -p "$out" Tests/oracle
clang -std=c11 -O2 -Wall -Wextra -Werror -o "$out/mkvhd" Tests/oracle/mkvhd.c

port="${WINDOWS_SSH_PORT:-22}"
ssh() { command ssh -o BatchMode=yes -p "$port" "$@"; }
scp() { command scp -o BatchMode=yes -P "$port" "$@"; }

if [ "${1:-}" = --scan ] || [ "${1:-}" = --detail ]; then
    fix=0 suffix=chkdsk
    [ "$1" = --detail ] && fix=1 suffix=chkdsk-f
    shift
    [ $# -gt 0 ] || { echo "usage: $0 --scan <img>..." >&2; exit 64; }
    [ -n "${WINDOWS_SSH:-}" ] || { echo "chkdsk-vm: --scan needs WINDOWS_SSH (docs/testing.md)" >&2; exit 2; }
    dest="${CHKDSK_OUT:-$root/build/chkdsk}"
    vhds="$dest/vhd"
    mkdir -p "$dest" "$vhds"
    summary="$dest/$([ $fix = 1 ] && echo detail || echo summary).tsv"
    # Mounts each VHD read-only, runs chkdsk with no /f (read-only mode: /scan needs a shadow copy,
    # which a read-only VHD cannot give, and fails with exit 11), saves its output with the exit code,
    # dismounts and deletes the VHD (the VM's disk is small). With -Fix it attaches the VHD (a copy
    # made for this run) read-write and runs chkdsk /f, which prints each correction.
    cat > "$vhds/scan.ps1" <<'PS'
param([string]$Dir, [string]$Names, [string]$Suffix, [switch]$Fix)
foreach ($n in $Names -split ',') {
    $vhd = Join-Path $Dir "$n.vhd"; $rep = Join-Path $Dir "$n.$Suffix.txt"
    try {
        $d = Mount-DiskImage -ImagePath $vhd -Access $(if ($Fix) { 'ReadWrite' } else { 'ReadOnly' }) -PassThru
        Start-Sleep 2
        $p = $d | Get-Disk | Get-Partition
        $l = ($p | Get-Volume).DriveLetter
        if ($Fix -and -not $l) { $p | Add-PartitionAccessPath -AssignDriveLetter; Start-Sleep 1; $l = ($d | Get-Disk | Get-Partition | Get-Volume).DriveLetter }
        if (-not $l) { throw "no drive letter (Windows did not mount the volume)" }
        $o = if ($Fix) { & chkdsk.exe "${l}:" /f /x 2>&1 } else { & chkdsk.exe "${l}:" 2>&1 }
        $rc = $LASTEXITCODE
        $o | Out-File -Encoding utf8 $rep
        "# exit: $rc" | Out-File -Append -Encoding utf8 $rep
    } catch {
        "# error: $_" | Out-File -Encoding utf8 $rep
    } finally {
        Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
        Remove-Item $vhd -ErrorAction SilentlyContinue
    }
}
PS
    ssh "$WINDOWS_SSH" 'powershell -NoProfile -Command "New-Item -ItemType Directory -Force C:\unlatch\scan | Out-Null"'
    scp -q "$vhds/scan.ps1" "$WINDOWS_SSH:C:/unlatch/scan/scan.ps1"
    bad=0
    while [ $# -gt 0 ]; do          # batches of 16 VHDs, about 0.5 GB on the VM at a time
        batch=()
        while [ $# -gt 0 ] && [ ${#batch[@]} -lt 16 ]; do
            n="$(basename "$1" .img)"
            "$out/mkvhd" "$1" "$vhds/$n.vhd" > /dev/null
            shasum -a 256 "$1" | cut -d' ' -f1 > "$dest/$n.img.sha256"
            batch+=("$n"); shift
        done
        files=()
        for n in "${batch[@]}"; do files+=("$vhds/$n.vhd"); done
        scp -q "${files[@]}" "$WINDOWS_SSH:C:/unlatch/scan/" || { echo "chkdsk-vm: upload failed (VM disk full?)" >&2; exit 1; }
        ssh "$WINDOWS_SSH" "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\unlatch\\scan\\scan.ps1 \
            -Dir C:\\unlatch\\scan -Suffix $suffix $([ $fix = 1 ] && echo -Fix) -Names $(IFS=,; echo "${batch[*]}")"
        for n in "${batch[@]}"; do
            rep="$dest/$n.$suffix.txt"
            scp -q "$WINDOWS_SSH:C:/unlatch/scan/$n.$suffix.txt" "$rep" || echo "# error: no report" > "$rep"
            ssh "$WINDOWS_SSH" "del C:\\unlatch\\scan\\$n.$suffix.txt" > /dev/null 2>&1 || true
            rm -f "$vhds/$n.vhd"
            { echo "# image: $n.img sha256 $(cat "$dest/$n.img.sha256")"; echo "# checked: $(date -u +%FT%TZ)"; } >> "$rep"
            rc="$(sed -n 's/^.*# exit: \([0-9]*\).*$/\1/p' "$rep" | head -1)"
            if grep -q '# error' "$rep"; then v=error
            elif [ "$rc" = 0 ] && grep -q "found no problems" "$rep"; then v=clean
            elif [ $fix = 1 ] && [ "$rc" = 1 ]; then v=corrected
            else v=problems; fi
            [ $v = clean ] || bad=$((bad + 1))
            printf '%s\t%s\t%s\n' "$n" "$v" "${rc:--}" | tee -a "$summary"
        done
    done
    echo "chkdsk-vm: $bad not clean (reports and summary.tsv in $dest)"
    [ $bad = 0 ]
    exit
fi

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
