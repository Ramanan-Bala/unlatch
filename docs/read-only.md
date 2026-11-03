# Why is my disk read-only?

Unlatch writes to an NTFS disk only when Windows left it in a clean state. Otherwise writing could lose what Windows still has to write, or damage the disk, so Unlatch mounts it **read-only** and says why: in the menu (the badge next to the disk), in a notification, and in `unlatch status <disk>`. You can always read and copy files off the disk.

| What Unlatch says (`state`) | What it means | What to do |
|---|---|---|
| **Windows is hibernated on this volume** (`hibernated`) | Windows went into hibernation with this disk attached; its open files live in `hiberfil.sys`. | Start Windows, then choose **Shut Down** (not Hibernate or Sleep). |
| **Windows Fast Startup (or an abrupt unplug) left cached metadata** (`hibernated`) | Windows' "Shut Down" with Fast Startup on is really a hibernation of the system; the disk is left in use. An unplug without ejecting can look the same. | In Windows, hold **Shift** while choosing Shut Down, or turn Fast Startup off: Control Panel > Power Options > Choose what the power buttons do > uncheck "Turn on fast startup". |
| **$LogFile has unfinished transactions** (`unclean_log`) | Windows did not finish writing before the disk went away. | Attach the disk to Windows, let it open, then Shut Down (or eject it there). |
| **Volume is marked dirty** (`dirty`) | The disk was unplugged, or the Mac or Windows stopped, while it was being written to, or Windows found a problem. (Unlatch marks a disk only while it writes to it; one unplugged about two seconds after the last change is not marked.) Its records may be half-written. | When Unlatch mounts it, it asks: **Check Disk and Enable Read-Write** looks at it, repairs what is safe and enables read-write if nothing is left (`unlatch check --repair <disk>`). On Windows: `chkdsk X: /f` (X is the disk's letter), then eject it. See [Without Windows](#without-windows). |
| **Write-protected**, or **cannot be opened for writing** | The disk, card or image itself refuses writes (a lock switch, a read-only image, a failing drive). | Check the lock switch; copy a read-only image to a writable one. |
| **Handed over read-only** | You (or a script) asked for read-only, for example `unlatch mount -r`. | Unmount and mount again without `-r`. |
| **NTFS metadata is damaged** (`corrupt`) | Unlatch could not make sense of the disk's own records. It does not mount it at all. | **Check Disk…** says what is wrong (often the backup copy of the file table no longer matches it, after a write was cut off). On Windows: `chkdsk X: /f`. |
| **No NTFS boot sector** (`not_ntfs`) | The partition is not NTFS. | Nothing to do; macOS or another app handles it. |

Unlatch never forces a write to a hibernated disk or one with unfinished transactions (ntfs-3g's `recover` or `remove_hiberfile` options are not used): only Windows can safely finish what Windows started. A disk that is only marked dirty can be checked on the Mac, or made writable without checking at your own risk (below).

## Without Windows

macOS has no tool that checks and repairs NTFS, so Unlatch has its own. When a disk marked as needing a check is mounted (plugged in, or **Mount read-write** in its ⋯ menu), Unlatch mounts it read-only and checks it right away. If nothing is wrong, it clears the mark and mounts it read-write; you are not asked. If the check finds something, Unlatch shows what it found and asks what to do, once each time the disk is plugged in. If you closed the question, **Enable Read-Write…** in the disk's ⋯ menu asks again. Your options, safest first:

1. **Check Disk and Enable Read-Write** (or `unlatch check --repair <disk>`). Unlatch unmounts the disk, reads its file table, its two allocation maps and every folder, and repairs only what is safe to repair. **Check Disk…** in the ⋯ menu (or `unlatch check <disk>`) does the same check without repairing anything first.
   - **Nothing found:** the "needs checking" mark is cleared, and the disk mounts read-write.
   - **Only space marked free that files use, or folder entries that point at a missing file:** Unlatch repairs it: it marks that space as used, so nothing gets overwritten, and removes the names that lead nowhere (a disk pulled out mid-copy leaves them when a new file's record never reached the disk; the file itself was already lost, and Windows' chkdsk removes the same names). At worst some free space stays lost until Windows checks the disk. It checks again, clears the mark when nothing is left, and the disk mounts read-write.
   - **Anything else** (a folder that cannot be read, files no folder lists, two files sharing space, a damaged file record, or a file table whose backup copy differs): Unlatch does not try to repair it, because a wrong repair loses files. The disk stays read-only. Copy what you need off the disk (reading is safe), then run chkdsk on Windows (step 3) or reformat it. **Copy Report** in the alert puts the full list on the clipboard.

   Hibernated disks and disks with unfinished Windows transactions are not repaired: only Windows can finish them.
2. **Copy your files off.** Reading is always safe. If you no longer need the disk as NTFS, reformat it in Disk Utility afterwards.
3. **Check it on any Windows PC**, or in a Windows virtual machine (UTM, Parallels or VMware with the disk passed through): `chkdsk X: /f`.
4. **Enable Read-Write Without Checking** (or `unlatch mount --mark-checked <disk>`). Unlatch mounts the disk read-write, and when you eject it from Unlatch the mark is cleared, so it mounts read-write every time after that.

   This is a risk, and it repairs nothing. A disk cut off mid-write can have space that files use recorded as free, or folders whose listings were never written. Writing to it can then put new files over old ones or damage folders, including files you do not touch. Copy anything important off first. Windows no longer offers to check the disk. If a write is cut off again, the disk is marked again.

   Only a disk marked dirty is offered these choices. Hibernated disks and disks with unfinished transactions always stay read-only, because writing to them loses what Windows has not finished.

## After fixing it

Eject the disk on Windows, plug it into the Mac again, and Unlatch mounts it read-write. Or, if it is still connected: `unlatch unmount <disk>` then `unlatch mount <disk>`.
