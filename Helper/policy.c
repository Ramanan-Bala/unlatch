// SPDX-License-Identifier: GPL-3.0-or-later
#include "policy.h"

#include <string.h>

bool helper_peer_allowed(uid_t peer, bool console_ok, uid_t console_uid)
{
    return console_ok && peer != 0 && peer == console_uid;
}

bool helper_kind_allowed(const char *kind, const char *content, bool rw)
{
    if (kind)
        return strcmp(kind, "ntfs") == 0;
    if (rw || !content)
        return false;
    return strcmp(content, "Windows_NTFS") == 0 ||
           strcmp(content, "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7") == 0;
}
