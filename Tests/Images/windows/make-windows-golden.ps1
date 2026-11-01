# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Windows-made golden image (Step 07 leftover), run in the Windows VM as an
# administrator (Tests/Images/windows/make.sh copies it there and runs it over SSH):
#   powershell -ExecutionPolicy Bypass -File make-windows-golden.ps1 -Dir C:\unlatch\golden
# Windows formats a 64 MB NTFS volume in a fixed VHD and writes everything with its own tools.
# manifest.tsv then records what Windows itself sees (sizes, SHA-256s, attributes, link
# targets, streams); make.sh turns it into the .expect file, so expectations never come from
# Unlatch's engine.
param([string]$Dir = 'C:\unlatch\golden')
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Dir | Out-Null
$vhd = Join-Path $Dir 'windows-content.vhd'
if (Test-Path $vhd) {                      # left attached by an earlier, failed run
    Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
    Remove-Item $vhd -Force
}

# diskpart: a fixed VHD with an MBR and one NTFS partition, labelled WINGOLD, as W:.
$dp = Join-Path $Dir 'create.txt'
@"
create vdisk file="$vhd" maximum=64 type=fixed
select vdisk file="$vhd"
attach vdisk
create partition primary
format fs=ntfs quick label=WINGOLD
assign letter=W
"@ | Set-Content -Encoding ASCII $dp
diskpart /s $dp | Out-Null
Start-Sleep 2
$part = Get-Partition -DriveLetter W
$root = 'W:\'

# Deterministic content: a 32-bit LCG, so the bytes do not depend on the machine.
function Bytes([int]$n, [uint32]$seed) {
    $b = New-Object byte[] $n
    $x = [uint64]$seed
    for ($i = 0; $i -lt $n; $i++) {
        $x = ($x * 1103515245 + 12345) % 4294967296
        $b[$i] = [byte](($x -shr 16) -band 0xFF)
    }
    return ,$b
}
function Put([string]$rel, [byte[]]$data) {
    $p = '\\?\' + $root + $rel
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($p)) | Out-Null
    [IO.File]::WriteAllBytes($p, $data)
}

Put 'plain.bin' (Bytes 100000 1)
Put 'small.txt' ([Text.Encoding]::ASCII.GetBytes("written by Windows`r`n"))

# NTFS compression (LZNT1) on a compressible file, by folder inheritance.
New-Item -ItemType Directory "${root}compressed" | Out-Null
compact /c "${root}compressed" | Out-Null
$text = [Text.Encoding]::ASCII.GetBytes((('Unlatch golden image, compressed by Windows. ' * 8) + "`r`n") * 900)
Put 'compressed\big.txt' $text
Put 'compressed\random.bin' (Bytes 70000 2)

# Sparse: 8 MiB long, 64 KiB of data at 4 MiB, holes elsewhere.
$sp = "${root}sparse.bin"
New-Item -ItemType File $sp | Out-Null
fsutil sparse setflag $sp | Out-Null
$fs = [IO.File]::Open($sp, 'Open', 'ReadWrite')
$fs.SetLength(8MB); $fs.Position = 4MB
$d = Bytes 65536 3; $fs.Write($d, 0, $d.Length); $fs.Close()
fsutil sparse setrange $sp 0 4194304 | Out-Null

# Alternate data streams.
Set-Content -LiteralPath "${root}plain.bin" -Stream 'note' -Value ([Text.Encoding]::ASCII.GetBytes('a note from Windows')) -Encoding Byte
Set-Content -LiteralPath "${root}plain.bin" -Stream 'Zone.Identifier' -Value ([Text.Encoding]::ASCII.GetBytes("[ZoneTransfer]`r`nZoneId=3`r`n")) -Encoding Byte

# Names: Unicode (NFC and a decomposed form, as Windows keeps them), emoji, 255 characters,
# and a path longer than MAX_PATH.
Put 'names\日本語ファイル.txt' (Bytes 1000 4)
Put ('names\' + [char]0x00E9 + 't' + [char]0x00E9 + '.txt') (Bytes 10 5)            # NFC é
Put ('names\e' + [char]0x0301 + 'te-nfd.txt') (Bytes 11 6)                         # NFD e + U+0301
Put ('names\' + [char]::ConvertFromUtf32(0x1F4C1) + ' emoji ' + [char]::ConvertFromUtf32(0x1F389) + '.txt') (Bytes 12 7)
Put ('names\' + ('N' * 251) + '.txt') (Bytes 13 8)
$long = 'long\' + ((1..6 | ForEach-Object { ('d' + $_) * 12 }) -join '\') + '\' + ('f' * 200) + '.bin'
Put $long (Bytes 14 9)

# Attributes.
Put 'attrs\hidden.txt' (Bytes 15 10);   (Get-Item -Force "${root}attrs\hidden.txt").Attributes = 'Hidden'
Put 'attrs\readonly.txt' (Bytes 16 11); (Get-Item -Force "${root}attrs\readonly.txt").Attributes = 'ReadOnly'
Put 'attrs\system.txt' (Bytes 17 12);   (Get-Item -Force "${root}attrs\system.txt").Attributes = 'System'

# A directory with 500 entries (several index blocks).
New-Item -ItemType Directory "${root}many" | Out-Null
for ($i = 0; $i -lt 500; $i++) { [IO.File]::WriteAllBytes(("${root}many\w{0:D4}.txt" -f $i), [byte[]]@()) }

# Links, made by cmd's mklink as a user would.
Push-Location $root
cmd /c mklink 'link-file.txt' 'plain.bin' | Out-Null          # relative file symlink
cmd /c mklink /D 'link-dir' 'names' | Out-Null                 # relative directory symlink
cmd /c mklink 'link-abs.txt' 'W:\plain.bin' | Out-Null         # absolute symlink on this volume
cmd /c mklink /J 'junction' 'W:\names' | Out-Null              # junction
Pop-Location

# Manifest: kind, path (/-separated, from the root), and what Windows reports.
$out = New-Object Collections.Generic.List[string]
$out.Add("label`tWINGOLD")
$out.Add("offset`t$($part.Offset)")
Get-ChildItem -LiteralPath $root -Recurse -Force -Attributes !ReparsePoint | Where-Object {
    $_.FullName -notmatch '\\System Volume Information' -and -not ($_.FullName -like '*\link-dir\*') -and -not ($_.FullName -like '*\junction\*')
} | ForEach-Object {
    $rel = '/' + $_.FullName.Substring(3).Replace('\', '/')
    if ($_.PSIsContainer) {
        $out.Add("count`t$rel`t" + @(Get-ChildItem -LiteralPath $_.FullName -Force).Count)
    } else {
        # .NET through \\?\ : Get-FileHash fails on paths longer than MAX_PATH.
        $fb = [IO.File]::ReadAllBytes('\\?\' + $_.FullName)
        $h = ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($fb))).Replace('-', '').ToLower()
        $out.Add("file`t$rel`t$($_.Length)`t$h")
        $a = $_.Attributes.ToString()
        foreach ($f in 'Hidden', 'ReadOnly', 'System', 'Compressed', 'SparseFile') {
            if ($a -match $f) { $out.Add("attr`t$rel`t$f") }
        }
        Get-Item -Force -LiteralPath $_.FullName -Stream * -ErrorAction SilentlyContinue | Where-Object { $_.Stream -ne ':$DATA' } | ForEach-Object {
            # Get-FileHash cannot open a stream in Windows PowerShell 5.1: hash its bytes.
            $sb = [byte[]](Get-Content -Force -LiteralPath $_.FileName -Stream $_.Stream -Encoding Byte -Raw)
            $sh = ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($sb))).Replace('-', '').ToLower()
            $out.Add("stream`t$rel`t$($_.Stream)`t$($_.Length)`t$sh")
        }
    }
}
# The root count includes the hidden System Volume Information folder, which Unlatch lists too.
$out.Add("count`t/`t" + @(Get-ChildItem -LiteralPath $root -Force).Count)
foreach ($l in 'link-file.txt', 'link-dir', 'link-abs.txt', 'junction') {
    $it = Get-Item -Force "${root}$l"
    $out.Add("link`t/$l`t$($it.LinkType)`t$($it.Target)")
}
[IO.File]::WriteAllLines((Join-Path $Dir 'manifest.tsv'), $out, (New-Object Text.UTF8Encoding $false))

@"
select vdisk file="$vhd"
detach vdisk
"@ | Set-Content -Encoding ASCII $dp
diskpart /s $dp | Out-Null
"done: $vhd"
