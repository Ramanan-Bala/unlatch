# Why is my disk read-only?

Unlatch writes to an NTFS disk only when Windows left it in a clean state. Otherwise writing could lose what Windows still has to write, or damage the disk, so Unlatch mounts it **read-only** and says why: in the menu (the badge next to the disk), in a notification, and in `unlatch status <disk>`. You can always read and copy files off the disk.

| What Unlatch says (`state`) | What it means | What to do |
|---|---|---|
| **Windows is hibernated on this volume** (`hibernated`) | Windows went into hibernation with this disk attached; its open files live in `hiberfil.sys`. | Start Windows, then choose **Shut Down** (not Hibernate or Sleep). |
| **Windows Fast Startup (or an abrupt unplug) left cached metadata** (`hibernated`) | Windows' "Shut Down" with Fast Startup on is really a hibernation of the system; the disk is left in use. An unplug without ejecting can look the same. | In Windows, hold **Shift** while choosing Shut Down, or turn Fast Startup off: Control Panel > Power Options > Choose what the power buttons do > uncheck "Turn on fast startup". |
| **$LogFile has unfinished transactions** (`unclean_log`) | Windows did not finish writing before the disk went away. | Attach the disk to Windows, let it open, then Shut Down (or eject it there). |
| **Volume is marked dirty** (`dirty`) | The disk was unplugged, or the Mac or Windows stopped, while it was being written to, or Windows found a problem. Its records may be half-written. | On Windows: `chkdsk X: /f` (X is the disk's letter), then eject it. No Windows PC: see [Without Windows](#without-windows). |
| **Write-protected**, or **cannot be opened for writing** | The disk, card or image itself refuses writes (a lock switch, a read-only image, a failing drive). | Check the lock switch; copy a read-only image to a writable one. |
| **Handed over read-only** | You (or a script) asked for read-only, for example `unlatch mount -r`. | Unmount and mount again without `-r`. |
| **NTFS metadata is damaged** (`corrupt`) | Unlatch could not make sense of the disk's own records. It does not mount it at all. | On Windows: `chkdsk X: /f`. Copy what you can elsewhere first. |
| **No NTFS boot sector** (`not_ntfs`) | The partition is not NTFS. | Nothing to do; macOS or another app handles it. |

Unlatch never forces a write to a hibernated disk or one with unfinished transactions (ntfs-3g's `recover` or `remove_hiberfile` options are not used): only Windows can safely finish what Windows started. A disk that is only marked dirty can be written to anyway, at your own risk (below).

## Without Windows

macOS has no tool that checks and repairs NTFS, so a disk marked dirty stays read-only until it is checked somewhere. Your options, safest first:

1. **Copy your files off.** Reading is always safe. If you no longer need the disk as NTFS, reformat it in Disk Utility afterwards.
2. **Check it on any Windows PC**, or in a Windows virtual machine (UTM, Parallels or VMware with the disk passed through): `chkdsk X: /f`.
3. **Mount it read-write anyway.** In the Unlatch menu, open the disk's ⋯ menu and choose **Mount Read-Write Anyway…**, then confirm; or run `unlatch mount --dirty-rw <disk>`. The badge then says "Read-write, unchecked".

   This is a risk. A disk cut off mid-write can have space that files use recorded as free, or folders whose listings were never written. Writing to it can then put new files over old ones or damage folders, including files you do not touch. Copy anything important off first. The disk stays marked as needing a check, so Windows still offers to check it the next time, and Unlatch keeps mounting it read-only until you choose this again.

   Only a disk marked dirty can be forced. Hibernated disks and disks with unfinished transactions always stay read-only, because writing to them loses what Windows has not finished.

## After fixing it

Eject the disk on Windows, plug it into the Mac again, and Unlatch mounts it read-write. Or, if it is still connected: `unlatch unmount <disk>` then `unlatch mount <disk>`.
