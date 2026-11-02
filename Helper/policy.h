// SPDX-License-Identifier: GPL-3.0-or-later
// The helper's access decisions, as plain functions of plain values so they can be tested
// without root, launchd or DiskArbitration (Tests/Helper/test_policy.c).
#ifndef UNLATCH_HELPER_POLICY_H
#define UNLATCH_HELPER_POLICY_H

#include <stdbool.h>
#include <sys/types.h>

// The caller may open disks only if it is the user at the screen, and never root:
// console_ok is false when /dev/console could not be examined.
bool helper_peer_allowed(uid_t peer, bool console_ok, uid_t console_uid);

// kind is DiskArbitration's volume kind and content its partition type, each NULL when it
// has none. Read-write needs macOS to call the file system NTFS. The partition type alone
// (Windows_NTFS, or Microsoft Basic Data) is also used by exFAT, FAT32, BitLocker and
// unformatted partitions, so it allows read-only only; the engine's probe decides then.
bool helper_kind_allowed(const char *kind, const char *content, bool rw);

#endif
