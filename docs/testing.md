# Testing Unlatch

Never test with a drive that holds real data. Every test here uses image files.

## Engine (host, automatic)

| What | Command | Time |
|---|---|---|
| Unit tests, harness checks, leak loops, written-image checks, fsx regressions | `Scripts/test-engine.sh` | ~2 min |
| Same, without the 4 GiB checks | `UNLATCH_SKIP_BIG=1 Scripts/test-engine.sh` | ~40 s |
| fsx soak (randomized model test) | `Scripts/fsx-soak.sh --seeds 1000 --ops 2000` | ~5 min on 8 cores |
| Seeds that once failed | `Scripts/fsx-soak.sh --regressions` | seconds |
| One fsx run, with an op log | `unlatch-engine fsx <empty.img> --seed N --ops M --verbose` | |

### fsx

`unlatch-engine fsx` runs random operations against an empty image and an in-memory model:
- **Operations:** create, mkdir, write (random offsets and sizes up to 256 KiB, past EOF, small resident writes), truncate, rename (same dir, across dirs, case-only, replace, EXCL, into own subtree), remove (including wrong-type and non-empty cases), release of held items, sync and remount.
- **Expected results:** the model predicts the exact result of every operation, success or a specific errno.
- **Checks:** after every op, the whole tree (names, types, sizes, inode numbers, link counts) and the full contents of the file the op touched. After each remount and at the end, all contents. Finally, the volume must probe CLEAN.

A failure prints the seed, the op number and a rerun command. `fsx-soak.sh` keeps the failing image and log in `build/fsx/fail-<seed>.*`, and appends the seed to `Tests/fsx-regressions.txt`. CI re-runs that file (Step 07).

## Windows `chkdsk` oracle (manual setup, then scripted)

`chkdsk` is the only real consistency checker for NTFS, so images Unlatch wrote are checked on Windows. `Scripts/chkdsk-vm.sh`:
1. wraps an image as a fixed VHD (`Tests/oracle/mkvhd.c`): an MBR with one NTFS partition at 1 MiB, the boot sectors' hidden-sectors field set to 2048, and a VHD footer;
2. attaches it read-only in Windows and runs `chkdsk X:` with no `/f`, so it only reports;
3. saves the report as `Tests/oracle/<name>.chkdsk.txt`, with the image's SHA-256, and passes only if chkdsk "found no problems".

```sh
Scripts/chkdsk-vm.sh --fsx 42                 # fresh fsx image, seed 42, 2000 ops
Scripts/chkdsk-vm.sh build/test-engine/w-rename.img
```

### One-time VM setup (Apple Silicon)

1. Install UTM (free, `brew install --cask utm`) or Parallels Desktop.
2. Create a Windows 11 on Arm VM. UTM's gallery, or Microsoft's Windows 11 Arm64 ISO, both work. Give it 4 GB RAM and 64 GB of disk.
3. In Windows: Settings → System → Optional features → add **OpenSSH Server**. Then, in an elevated PowerShell:
   ```powershell
   Start-Service sshd; Set-Service -Name sshd -StartupType Automatic
   New-NetFirewallRule -Name sshd -DisplayName 'OpenSSH Server' -Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22
   ```
4. Use an administrator account for SSH (`Mount-DiskImage` needs one). Add your public key to `C:\ProgramData\ssh\administrators_authorized_keys`.
5. Find the VM's address. With UTM's "Emulated VLAN" network the VM is `10.0.2.15` and not reachable from the Mac: add a port forward (VM settings → Network → Port Forward: TCP, guest port 22, host 127.0.0.1:2222) and set `WINDOWS_SSH_PORT=2222`. With "Shared Network" it is usually `192.168.64.x` on port 22. Then:
   ```sh
   export WINDOWS_SSH=you@192.168.64.5
   Scripts/chkdsk-vm.sh --fsx 42
   ```

Without `WINDOWS_SSH`, the script builds the VHD and prints the PowerShell steps to run by hand.

Windows-side readability (Steps 10 and 16): copy `Tests/oracle/windows-check.ps1` and a suite image's VHD into the VM and run it. It mounts the VHD read-write (a throwaway copy): Windows refuses to mount a volume read-only while its `$LogFile` is in the blank state libntfs-3g leaves, though `chkdsk` reads it fine.

## FSKit (Steps 08+)

FSKit tests need the extension enabled in System Settings, so they don't run in CI. The manual matrix lives in `docs/manual-matrix.md` (Step 17).
