# SPDX-License-Identifier: GPL-3.0-or-later
# Windows-side check of a volume written by Tests/Fuse/rw-suite.sh (Steps 10 and 16), run in the
# Windows VM: powershell -ExecutionPolicy Bypass -File windows-check.ps1 -vhd C:\unlatch\rw-suite.vhd
# Mounts read-write: Windows will not mount a volume whose $LogFile libntfs-3g left blank read-only.
param([string]$vhd)
$ErrorActionPreference = 'Continue'
$d = Mount-DiskImage -ImagePath $vhd -Access ReadWrite -PassThru
Start-Sleep 2
$l = ($d | Get-Disk | Get-Partition | Get-Volume).DriveLetter
Start-Sleep 2
$r = "${l}:\"
"--- t.txt content: " + (Get-Content -Raw "$r\t.txt")
"--- streams on t.txt:"
Get-Item "$r\t.txt" -Force -Stream * | ForEach-Object { "  " + $_.Stream + " (" + $_.Length + " bytes)" }
"--- stream com.example.note: " + (Get-Content -Raw "$r\t.txt" -Stream com.example.note)
"--- hard link: " + (Get-Content -Raw "$r\hard") + " links=" + (fsutil hardlink list "$r\t.txt" | Measure-Object).Count
"--- sym:"
$s = Get-Item "$r\sym" -Force
"  attributes: " + $s.Attributes + "; LinkType: " + $s.LinkType + "; Target: " + $s.Target
fsutil reparsepoint query "$r\sym" | Select-Object -First 3
"--- cp/up (relative symlink in a subdir): " + (Get-Item "$r\cp\up" -Force).LinkType
# Step 17 (D-67): on-volume targets are native symlinks, which Windows programs follow.
"--- sym followed by Windows: " + (Get-Content -Raw "$r\sym")
"--- cp/up followed by Windows: " + (Get-Content -Raw "$r\cp\up")
"--- dirlink: LinkType " + (Get-Item "$r\dirlink" -Force).LinkType + "; entries through it: " + (Get-ChildItem "$r\dirlink" | Measure-Object).Count
"--- abslink (WSL, not followed): " + (Get-Item "$r\abslink" -Force).Attributes
"--- git clone files: " + (Get-ChildItem -Recurse -File "$r\clone" | Measure-Object).Count
"--- hidden t.txt: " + ((Get-Item "$r\t.txt" -Force).Attributes)
"--- any ._ files: " + (Get-ChildItem -Recurse -Force "$r" -Filter '._*' | Measure-Object).Count
Dismount-DiskImage -ImagePath $vhd | Out-Null
