# TODO

Open work, newest first. Details and history are in `docs/PROGRESS.md` and `docs/DECISIONS.md`.

## Check Disk: rebuild a folder index it cannot read, as chkdsk does (after D-100, D-103)

Goal: every cut-off in the sweeps (`test_engine cutsweep`, `UNLATCH_CUT_FULL=1`), and every real disk like the one below, leaves a disk that Check Disk repairs to what Windows' chkdsk calls clean, with no write-order trade-off. Beta.14 ships D-103's `BC_PH_EXT_RECORDS` order (a folder's extension records after its base record), which fixed the one unrepairable cut (attr-lists 184) but raised the big-folder sweep's names that are listed and cannot be opened from 80 to 128 of about 1,700 cuts. Check Disk does not detect those.

**Real disk, beta.14 (2026-11-05):** the owner's Seagate 1 TB (real data, ~350,000 entries) after a Finder copy of a photo folder (`~/Documents/VIETNAM`, 8 folders, 1,548 JPGs, 8.9 GB) did not end cleanly. Check Disk (read-only): `index_unreadable` ×2, `/VIETNAM` (record 28372) and `/VIETNAM/DAY-5` (record 310397), both `ntfs_readdir` failing with EIO; `orphan` ×110 (108 photos of `DAY-5`, plus `DAY-5` itself and one more folder under `/VIETNAM`); `record_unmarked` ×52 (`DAY-5` photos); `entry_free_record` ×1 (a `._` AppleDouble file in the root); 2,754 lost records. Everything else on the disk read without errors. Finder showed `/VIETNAM` empty (`ls`: Input/output error). Verdict "needs Windows", so the disk stayed read-only. **Windows `chkdsk X: /f` in the UTM VM repaired it, and photos that Finder had not shown came back into `/VIETNAM`** (chkdsk rebuilt the two indexes and put the orphans back); Unlatch must do the same, with the same result: every photo whose record is whole is listed again, in its own folder. To compare, when the disk is back on the Mac: list `/VIETNAM` against `~/Documents/VIETNAM` (names and sizes) and look for `found.000`. How the copy ended (pull, sleep, crash) was not recorded, and no snapshot was taken before chkdsk ran, so the cause of the EIO (torn index block, a block outside `$INDEX_ALLOCATION`, bad fixups, a bitmap/allocation mismatch) is unknown. Reproduce it with the sweeps (a folder of many 5 MB files copied while subfolders are created) and capture the next real case with `Tests/Fuse/with-device.py <disk> ro --snapshot` of the two folders' records and index blocks before any repair.

- [ ] **Find why `ntfs_readdir` fails** on such a folder and report it precisely (which block, why: magic, fixups, VCN, outside the allocation, bitmap past the allocation, a node pointing nowhere), instead of one `index_unreadable`, so the repair knows what it may trust.
- [ ] **Rebuild the folder's index from its files' records** (chkdsk's "Correcting error in index $I30" and "Recovering orphaned file … into directory"): collect every in-use base record with a `$FILE_NAME` whose parent is this folder (and matching sequence number), pass 1 already knows them (`c->parent`, `c->multi` for several names); remove the folder's `$INDEX_ALLOCATION` and `$I30` `$BITMAP` and reset `$INDEX_ROOT` to an empty small index (as `ntfs_create` builds one; libntfs-3g has no public call, so this is our code in `check.c`, through `ntfs_attr_rm` and `ntfs_resident_attr_value_resize`); re-add every name, DOS+Win32 pairs together, with `ntfs_index_add_filename`. Only when the folder's own record is whole; its old index clusters are freed by the D-102 pass afterwards. Then the files that were orphans because the index was unreadable are listed again, and a nested folder (`DAY-5` inside `VIETNAM`) is rebuilt in the same run (rounds, D-103).
- [ ] With the rebuild, `index_unreadable` becomes repairable for a folder whose record is whole, and the `orphan`/`record_unmarked`/`entry_*` problems around it are repaired in the same `check --repair` (bitmaps first, then rebuilds, then entries, then orphans).
- [ ] Planted damage for it in `mkfixture` (an index block zeroed, torn, outside the allocation, a node pointing past it; `zero-index DIR` exists) and the Seagate's mix in one fixture: a nested folder whose parent's index is unreadable, orphans in it, unmarked records; Windows chkdsk on the repaired fixtures, clean.
- [ ] Detect, per folder, what the sweeps' walk finds: entries outside the key range their parent node gives their block (a split's old block not cut down yet: names listed twice, or listed and not found by lookup), and an entry whose name is not among its record's `$FILE_NAME`s in that folder (a rename's old name; extend D-96's wrong-parent test to compare the full name).
- [ ] Repair them, preferably by rebuilding the folder's index from the `$FILE_NAME`s of the records that name it, as chkdsk does (empty the index, re-add every name with `ntfs_index_add_filename`), when everything else about the folder is whole.
- [ ] Repair a node pointing past `$INDEX_ALLOCATION` (attr-lists 184 without the extension-record order): the new block is still on the disk in clusters marked used that nothing owns; find it (an `INDX` block with that VCN whose entries name records in this folder), grow `$INDEX_ALLOCATION` and its `$I30` bitmap to hold it and copy it in. Or rely on the rebuild above.
- [ ] Then decide whether to keep or drop `BC_PH_EXT_RECORDS` from the counts of both orders, and make the sweeps fail on the walk's findings (`unreachable`, `duplicates`, `extra entries`) once Check Disk repairs them.
- [ ] Windows chkdsk on the repaired cut images of every sweep (`UNLATCH_CUT_EXEC`, `Scripts/chkdsk-vm.sh --scan`/`--detail`), all clean.
- [ ] Then the README and site can say Windows is not needed after a cut-off (wording drafted in the D-101 entry of `docs/PROGRESS.md`).

## Check Disk: check and repair a disk marked dirty on the Mac (D-90, D-92, D-93)

Today a Mac-only user's choices for a disk marked dirty are read-only, "Mount Read-Write Anyway" or "Mark as Checked", none of which looks at the disk. Goal: Unlatch checks it and clears the mark only when it is consistent. macOS has no NTFS checker (ntfs-3g's ntfsck is 2006 code, quarantined upstream, check-only).

- [x] A read-only checker in the engine (`ue_check`): every in-use MFT record is marked in `$MFT`'s bitmap; every cluster a file's runlist uses is marked in `$Bitmap`; no cluster belongs to two files; every directory index can be read and points at in-use records with matching sequence numbers.
- [x] Safe repair only: set bitmap bits for records and clusters in use (at worst space is lost, nothing is overwritten). Anything else (an unreadable index, a shared cluster) is reported, never repaired.
- [x] Clear the mark (`UE_OPEN_CLEAR_DIRTY`) only when the check finds nothing left.
- [x] `unlatch check <disk> [--repair] [--json]` and a "Check Disk…" item in the app's ⋯ menu with a report of what was found.
- [x] Tests: fixture images with each kind of damage (`mkfixture`), cut-off copies (`test_check_cut_copy`) and `interrupt.sh kill` through a real mount.
- [ ] The owner's SanDisk stick snapshot (two folders whose index blocks were never written): run `unlatch-engine check` on it when the owner offers it. `test_check_cut_copy` reproduces the same damage.
- [ ] The app's Check Disk alerts by hand (built, not clicked through).

## Full Disk Access on the app instead of the helper (D-74)

Today macOS asks for Full Disk Access for `io.github.ramanan-bala.unlatch.helper`, because the helper is what opens the root-only disk devices. Goal: the user grants it to **Unlatch** once.

- [ ] Ship the helper inside the app as a bare executable in `Unlatch.app/Contents/MacOS/` (no bundle of its own), with its launchd plist in `Contents/Library/LaunchDaemons/` (`BundleProgram`, the socket, `AssociatedBundleIdentifiers`).
- [ ] Register it with `SMAppService.daemon(plistName:)` from the app's setup window (and unregister it in Uninstall); the user approves it in Login Items. Check Apple's ServiceManagement docs first (AGENTS.md).
- [ ] Find out whether SMAppService registers a daemon of an **ad-hoc signed** app at all.
- [ ] Test with a **disposable USB stick**: grant Full Disk Access to Unlatch only, then mount the stick read-write. The only removable disk on the dev Mac holds real data and must not be used.
- [ ] Check that the grant survives an update (ad-hoc signatures change every build).
- [ ] If it works: replace `unlatch install-helper`'s `/Library/PrivilegedHelperTools` install, update `HelperOpener`, the README's one-time setup, the cask's uninstall step and D-57/D-60/D-74. If not: keep the current helper and record why.

Background: Apple DTS says a daemon generally does not inherit its app's TCC grants, even via SMAppService ([forum 853725](https://developer.apple.com/forums/thread/853725)); the bare-executable layout is undocumented and was reported to work for Accessibility, not tested for Full Disk Access.

## Other open items

- [ ] Full benchmark rerun (`Bench/baselines.sh`, all setups) and record it in `docs/M0-gate.md`.
- [ ] Hardware tests in `docs/test-matrix.md`: USB stick pulled mid-copy, sleep and wake, a large real disk, the 24-hour soak, a clean-Mac install.
- [ ] Windows-made images still wanted: Fast Startup, dirty, fragmented `$MFT` (`Tests/Images/README.md`).
