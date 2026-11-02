// SPDX-License-Identifier: GPL-3.0-or-later
// Check Disk's output, shared by `unlatch-engine check` and `unlatch-fuse --check` (D-93).
#ifndef UE_CHECK_REPORT_H
#define UE_CHECK_REPORT_H

#include <stdbool.h>
#include <stdio.h>

#include "unlatch_engine.h"

// Exit codes, as fsck's: nothing found, everything found was repaired, problems are left,
// the check could not run.
enum { CHECK_CLEAN = 0, CHECK_REPAIRED = 1, CHECK_PROBLEMS = 4, CHECK_FAILED = 8 };

// Runs ue_check on io and prints the problems and a summary to out: text for people, or one
// JSON document (schema 1; the fields are in docs/cli.md). Returns one of the codes above.
int check_report(const ue_io_t *io, bool repair, bool json, FILE *out);

#endif // UE_CHECK_REPORT_H
