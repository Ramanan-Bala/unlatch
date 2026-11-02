// SPDX-License-Identifier: GPL-3.0-or-later
// The helper's access decisions (Helper/policy.c): who may open disks, and which.
#include <stdio.h>

#include "policy.h"

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

int main(void)
{
    const char *basic_data = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";

    // Kinds: macOS calls it NTFS, any mode.
    CHECK(helper_kind_allowed("ntfs", "Windows_NTFS", true));
    CHECK(helper_kind_allowed("ntfs", NULL, false));
    // Another file system on an NTFS-typed partition: never.
    CHECK(!helper_kind_allowed("exfat", "Windows_NTFS", false));
    CHECK(!helper_kind_allowed("exfat", basic_data, true));
    CHECK(!helper_kind_allowed("msdos", basic_data, false));
    // No kind (BitLocker, unformatted, or NTFS macOS cannot read): read-only by partition type.
    CHECK(!helper_kind_allowed(NULL, "Windows_NTFS", true));
    CHECK(!helper_kind_allowed(NULL, basic_data, true));
    CHECK(helper_kind_allowed(NULL, "Windows_NTFS", false));
    CHECK(helper_kind_allowed(NULL, basic_data, false));
    CHECK(!helper_kind_allowed(NULL, "Apple_HFS", false));
    CHECK(!helper_kind_allowed(NULL, NULL, false));

    // Callers: only the console user, never root.
    CHECK(helper_peer_allowed(501, true, 501));
    CHECK(!helper_peer_allowed(502, true, 501));
    CHECK(!helper_peer_allowed(0, true, 0));
    CHECK(!helper_peer_allowed(0, true, 501));
    CHECK(!helper_peer_allowed(501, false, 501));

    printf("%s helper policy\n", failures ? "FAIL" : "ok  ");
    return failures != 0;
}
