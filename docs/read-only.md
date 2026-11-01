# Why is my disk read-only?

Unlatch writes to an NTFS disk only when Windows left it in a clean state. Otherwise writing could lose what Windows still has to write, or damage the disk, so Unlatch mounts it **read-only** and says why: in the menu (the badge next to the disk), in a notification, and in `unlatch status <disk>`. You can always read and copy files off the disk.

| What Unlatch says (`state`) | What it means | What to do |
|---|---|---|
| **Windows is hibernated on this volume** (`hibernated`) | Windows went into hibernation with this disk attached; its open files live in `hiberfil.sys`. | Start Windows, then choose **Shut Down** (not Hibernate or Sleep). |
| **Windows Fast Startup (or an abrupt unplug) left cached metadata** (`hibernated`) | Windows' "Shut Down" with Fast Startup on is really a hibernation of the system; the disk is left in use. An unplug without ejecting can look the same. | In Windows, hold **Shift** while choosing Shut Down, or turn Fast Startup off: Control Panel > Power Options > Choose what the power buttons do > uncheck "Turn on fast startup". |
| **$LogFile has unfinished transactions** (`unclean_log`) | Windows did not finish writing before the disk went away. | Attach the disk to Windows, let it open, then Shut Down (or eject it there). |
| **Volume is marked dirty** (`dirty`) | Windows found a problem and wants `chkdsk` to run. | On Windows: `chkdsk X: /f` (X is the disk's letter), then eject it. |
| **Write-protected**, or **cannot be opened for writing** | The disk, card or image itself refuses writes (a lock switch, a read-only image, a failing drive). | Check the lock switch; copy a read-only image to a writable one. |
| **Handed over read-only** | You (or a script) asked for read-only, for example `unlatch mount -r`. | Unmount and mount again without `-r`. |
| **NTFS metadata is damaged** (`corrupt`) | Unlatch could not make sense of the disk's own records. It does not mount it at all. | On Windows: `chkdsk X: /f`. Copy what you can elsewhere first. |
| **No NTFS boot sector** (`not_ntfs`) | The partition is not NTFS. | Nothing to do; macOS or another app handles it. |

Unlatch never forces a write (ntfs-3g's `recover` or `remove_hiberfile` options are not used): only Windows can safely finish what Windows started.

## After fixing it

Eject the disk on Windows, plug it into the Mac again, and Unlatch mounts it read-write. Or, if it is still connected: `unlatch unmount <disk>` then `unlatch mount <disk>`.
