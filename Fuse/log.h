// SPDX-License-Identifier: GPL-3.0-or-later
// OSLog for the daemon: subsystem io.github.ramanan-bala.unlatch, categories engine and mount.
#ifndef UNLATCH_FUSE_LOG_H
#define UNLATCH_FUSE_LOG_H

#include <os/log.h>

enum { UF_LOG_ENGINE, UF_LOG_MOUNT };

// Sets up both categories and routes the engine's libntfs-3g messages to `engine`.
void uf_log_init(void);

// Logs to OSLog and, before the daemon detaches or in the foreground, to stderr.
void uf_log(int category, os_log_type_t type, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

// After fuse_daemonize, stderr is /dev/null unless the daemon stayed in the foreground.
void uf_log_set_stderr(int enabled);

#endif // UNLATCH_FUSE_LOG_H
