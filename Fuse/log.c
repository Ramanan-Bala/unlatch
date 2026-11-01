// SPDX-License-Identifier: GPL-3.0-or-later
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include "unlatch_engine.h"

#define UF_SUBSYSTEM "io.github.ramanan-bala.unlatch"

static os_log_t logs[2];
static int to_stderr = 1;

static void engine_log(void *ctx, int level, const char *msg)
{
    (void)ctx;
    os_log_type_t type = level == UE_LOG_ERROR ? OS_LOG_TYPE_ERROR
                       : level == UE_LOG_DEBUG ? OS_LOG_TYPE_DEBUG
                       : OS_LOG_TYPE_DEFAULT;
    uf_log(UF_LOG_ENGINE, type, "%s", msg);
}

void uf_log_init(void)
{
    logs[UF_LOG_ENGINE] = os_log_create(UF_SUBSYSTEM, "engine");
    logs[UF_LOG_MOUNT] = os_log_create(UF_SUBSYSTEM, "mount");
    ue_set_log(engine_log, NULL);
}

void uf_log_set_stderr(int enabled)
{
    to_stderr = enabled;
}

void uf_log(int category, os_log_type_t type, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    os_log_with_type(logs[category], type, "%{public}s", msg);
    if (to_stderr)
        fprintf(stderr, "unlatch-fuse: %s\n", msg);
}
