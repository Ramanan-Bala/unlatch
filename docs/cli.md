# `unlatch` command-line tool

```
unlatch list [--json]                      NTFS volumes and who has them mounted
unlatch mount <disk> [-r|--read-only] [--json]
unlatch unmount <disk> [--force]
unlatch unmount --all [--force]            every volume Unlatch has mounted (before an update)
unlatch eject <disk> [--force]             unmount, then eject the whole disk
unlatch status [<disk>] [--json]           Unlatch's verdict per volume (clean, dirty, hibernated, ...)
unlatch doctor                             check macOS, macFUSE, unlatch-fuse, the helper
sudo unlatch install-helper                once, for real disks (not needed for disk images)
sudo unlatch uninstall-helper
```

`<disk>` is a BSD name (`disk6s1`), a device path (`/dev/disk6s1` or `/dev/rdisk6s1`), a mount path (`/Volumes/Seagate`), or a volume label (case-insensitive; must name one volume).

`mount` mounts at `/Volumes/<label>`. It is read-write only when the volume is clean and nothing forces read-only. If Apple's read-only NTFS driver had the volume, Unlatch unmounts it first and gives it back if mounting fails.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | error (the message says why) |
| 2 | usage: unknown command or option, missing argument |
| 3 | mounted, but read-only although read-write was asked for. The reason is on stderr, for example `dirty: volume is marked dirty; run chkdsk on Windows before writing to it` |
| 4 | macFUSE missing or its kernel extension not allowed (`doctor` says which) |

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

Example: `unlatch status --json | jq -r '.volumes[] | select(.owner == "unlatch") | .mountPoint'`

## The helper

Real disks' device nodes are readable only by root. `sudo unlatch install-helper` installs a launchd job, `io.github.ramanan-bala.unlatch.helper`, that listens on `/var/run/io.github.ramanan-bala.unlatch.helper.sock`. It opens an NTFS volume's node, for the console user only, and hands the descriptor back. `unlatch-fuse` then runs as you. The helper never reads or writes the disk itself (D-57). Disk images you attach are yours and do not need it.

macOS's privacy protection (TCC) also requires the helper to have **Full Disk Access**: System Settings → Privacy & Security → Full Disk Access → **+** → ⌘⇧G → `/Library/PrivilegedHelperTools/io.github.ramanan-bala.unlatch.helper`. Without it, macOS refuses the helper's open of a removable disk, even as root, and `unlatch mount` says so (D-60).

Finding the other binaries: `$UNLATCH_FUSE` and `$UNLATCH_HELPER`, else next to `unlatch`, else the repository's Debug build.
