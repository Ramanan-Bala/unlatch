# `unlatch` command-line tool

```
unlatch list [--json]                      NTFS volumes and who has them mounted
unlatch mount <disk> [-r|--read-only] [--mark-checked] [--json]
unlatch unmount <disk> [--force]
unlatch unmount --all [--force]            every volume Unlatch has mounted (before an update)
unlatch eject <disk> [--force]             unmount, then eject the whole disk
unlatch status [<disk>] [--json]           Unlatch's verdict per volume (clean, dirty, hibernated, ...)
unlatch check <disk> [--repair] [--json] [--no-remount]
                                           Check Disk: look for damage; --repair does the safe repair
                                           and clears the dirty mark when nothing is left (D-93)
unlatch doctor                             check macOS, macFUSE, unlatch-fuse, the helper
unlatch setup [--yes] [--no-helper]        install or update macFUSE (verified download), explain its
                                           one-time approval, install the helper
sudo unlatch install-helper                once, for real disks (not needed for disk images)
sudo unlatch uninstall-helper
```

`<disk>` is a BSD name (`disk6s1`), a device path (`/dev/disk6s1` or `/dev/rdisk6s1`), a mount path (`/Volumes/Seagate`), or a volume label (case-insensitive; must name one volume).

`mount` mounts at `/Volumes/<label>`. It is read-write only when the volume is clean and nothing forces read-only. A volume marked dirty mounts read-only (exit 3), and the CLI points at the two ways to enable read-write: `unlatch check --repair <disk>` checks it first and clears the mark when nothing is left (D-93), and `--mark-checked` enables read-write without checking: it replaces any Unlatch mount of the volume, and its clean unmount or eject clears the mark, so the volume mounts read-write from then on (D-92, D-94). It repairs nothing, so writing can damage files on a disk that was cut off mid-write ([read-only.md](read-only.md#without-windows)). Hibernated, unclean and damaged volumes stay read-only with it. If Apple's read-only NTFS driver had the volume, Unlatch unmounts it first and gives it back if mounting fails.

`check` unmounts the volume (whoever has it), opens its device the way a mount does (through the helper for real disks; read-only unless `--repair`), runs `unlatch-fuse --check`, and mounts it again with Unlatch unless `--no-remount`. The check reads only. It looks for: records in use that `$MFT`'s bitmap marks free (`record_unmarked`), clusters files use that `$Bitmap` marks free (`clusters_unmarked`), clusters two files claim (`clusters_shared`), runlists that cannot be read or run past the volume (`bad_runlist`), records that cannot be read (`bad_record`), folders whose index cannot be read (`index_unreadable`), folder entries that point at a free record or the wrong sequence number (`entry_free_record`, `entry_wrong_seq`), named files no folder lists (`orphan`), and a `$MFTMirr` that does not match `$MFT` (`mft_mirror`; libntfs-3g cannot open such a volume at all, so that is all it reports). `--repair` sets the bitmap bits for the first two kinds, and only when nothing else was found; then it checks again and clears the dirty mark if nothing is left. Everything else is reported for Windows' chkdsk; [read-only.md](read-only.md#without-windows) says what to do.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | error (the message says why) |
| 2 | usage: unknown command or option, missing argument |
| 3 | mounted, but read-only although read-write was asked for. The reason is on stderr, for example `dirty: volume is marked dirty; run chkdsk on Windows before writing to it` |
| 4 | macFUSE missing or older than 5.5 |
| 5 | `check`: problems are left on the volume (none found, or all repaired, is 0) |

## JSON (`"schema": 1`)

Within a schema version, field names and meanings do not change; fields may be added. `list --json` and `status --json` print:

```json
{
  "schema" : 1,
  "volumes" : [
    {
      "autoMount" : true,
      "bsdName" : "disk6s1",
      "devicePath" : "/dev/disk6s1",
      "ejectable" : true,
      "internal" : false,
      "label" : "Seagate",
      "mountPoint" : "/Volumes/Seagate",
      "owner" : "unlatch",
      "removable" : true,
      "size" : 1000204886016,
      "status" : {
        "mounted" : true,
        "readOnly" : false,
        "reason" : "",
        "serial" : "54DDA118567EC0ED",
        "state" : "clean",
        "timestamp" : "2026-11-01T14:00:00Z"
      },
      "volumeUUID" : "…"
    }
  ]
}
```

- `owner`: `none`, `apple` (Apple's read-only driver), `unlatch`, or `other:<fstype>`.
- `status`: present in `status --json` once Unlatch has probed the volume, otherwise `null`. Its `state` is one of `clean`, `dirty`, `hibernated`, `unclean_log`, `corrupt`, `not_ntfs`.
- `mount --json` prints `{"schema": 1, "bsdName", "mountPoint", "readOnly", "state", "reason"}`.
- `check --json` prints `{"schema": 1, "bsdName", "verdict", "advice", "report"}`. `verdict` is `clean`, `repaired`, `repairable` (everything found can be repaired with `--repair`) or `needs_windows`. `report` is `unlatch-fuse --check`'s own JSON: `problems` (each `{"kind", "repairable", "record", "other"?, "lcn"?, "count"?, "path", "detail"}`; `other` is the record an entry points at, or the first owner of shared clusters), `state` (before the check), `records`, `recordsInUse`, `directories`, `entries`, `clusters`, `clustersInUse`, `lostRecords` and `lostClusters` (marked used, used by nothing: space only, not problems), `found`, `repairable`, `repaired` (bitmap bits set), `remaining`, `repairRan`, `dirtyCleared`, `exitCode` (0 nothing found, 1 all repaired, 4 problems left, 8 could not check) and `error` when the check could not run.

Example: `unlatch status --json | jq -r '.volumes[] | select(.owner == "unlatch") | .mountPoint'`

## The helper

Real disks' device nodes are readable only by root. `sudo unlatch install-helper` installs a launchd job, `io.github.ramanan-bala.unlatch.helper`, that listens on `/var/run/io.github.ramanan-bala.unlatch.helper.sock`. It opens an NTFS volume's node, for the console user only, and hands the descriptor back. `unlatch-fuse` then runs as you. The helper never reads or writes the disk itself (D-57). Disk images you attach are yours and do not need it.

macOS's privacy protection (TCC) also requires the helper to have **Full Disk Access**: System Settings → Privacy & Security → Full Disk Access, then drag the helper in from Finder (`open -R "/Library/PrivilegedHelperTools/Unlatch Helper"`), or **+** → ⌘⇧G → that path. `unlatch setup` opens both for you. The grant is the helper's, not the app's: the helper is what opens the disk (D-74). Without it, macOS refuses the helper's open of a removable disk, even as root, and `unlatch mount` says so (D-60).

Finding the other binaries: `$UNLATCH_FUSE` and `$UNLATCH_HELPER`, else next to `unlatch`, else the repository's Debug build.

## macFUSE backend

Mounts go through macFUSE's kernel extension when it is allowed on this Mac (loaded, or approved at the installed macFUSE version), because it is up to 8 times faster with many small files; otherwise through macFUSE's FSKit backend, which needs no kernel extension (D-79). Unlatch never asks macOS to load the kernel extension on its own. `unlatch doctor` says which one mounts use. `UNLATCH_BACKEND=kext` or `UNLATCH_BACKEND=fskit` forces one (for testing; `kext` on a Mac where it is not allowed makes macOS show its "blocked extension" prompt).

## `unlatch setup`

Runs the one-time setup in order, asking before each change (`--yes` skips the questions):

1. macFUSE: if it is missing or older than 5.5, downloads the newest release from github.com/macfuse/macfuse, checks the published SHA-256, the package signature (Developer ID Installer, team 3T5GSNBU6W) and Apple's notarization, and runs `sudo installer -pkg … -target /`.
2. How macFUSE mounts (D-78, D-79): if its kernel extension is allowed on this Mac, Unlatch uses it and there is nothing to do. Otherwise it prints how to turn on macFUSE's file system extension (System Settings > General > Login Items & Extensions > File System Extensions > macFUSE) and offers to open that pane. Only the user can switch it on, and FSKit does not report whether it is on, so the first mount shows it; a failed mount says how to turn it on.
3. The helper: `sudo unlatch install-helper`, then opens Full Disk Access in System Settings.
