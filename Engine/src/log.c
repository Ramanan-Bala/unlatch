// SPDX-License-Identifier: GPL-3.0-or-later
// Routes libntfs-3g's log output to the host's ue_log_fn (D-44).
#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static ue_log_fn log_fn;
static void *log_ctx;

static int ue_log_handler(const char *function, const char *file, int line, u32 level, void *data,
                          const char *format, va_list args)
{
    char msg[1024];
    int n, lvl;

    (void)file;
    (void)line;
    (void)data;
    if (!log_fn)
        return 0;
    if (level & (NTFS_LOG_LEVEL_CRITICAL | NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR))
        lvl = UE_LOG_ERROR;
    else if (level & NTFS_LOG_LEVEL_WARNING)
        lvl = UE_LOG_WARNING;
    else if (level & (NTFS_LOG_LEVEL_INFO | NTFS_LOG_LEVEL_VERBOSE | NTFS_LOG_LEVEL_QUIET))
        lvl = UE_LOG_INFO;
    else
        lvl = UE_LOG_DEBUG;
    n = snprintf(msg, sizeof(msg), "%s: ", function ? function : "ntfs");
    if (n < 0 || (size_t)n >= sizeof(msg))
        n = 0;
    vsnprintf(msg + n, sizeof(msg) - (size_t)n, format, args);
    for (size_t i = strlen(msg); i > 0 && msg[i - 1] == '\n'; i--)
        msg[i - 1] = 0;
    log_fn(log_ctx, lvl, msg);
    return (int)strlen(msg);
}

void ue_logf(int level, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    if (!log_fn)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    log_fn(log_ctx, level, msg);
}

void ue_set_log(ue_log_fn fn, void *ctx)
{
    log_fn = fn;
    log_ctx = ctx;
    if (fn) {
        ntfs_log_set_handler(ue_log_handler);
        ntfs_log_set_levels(NTFS_LOG_LEVEL_CRITICAL | NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR |
                            NTFS_LOG_LEVEL_WARNING | NTFS_LOG_LEVEL_INFO);
    } else {
        ntfs_log_set_handler(ntfs_log_handler_null);
    }
}
