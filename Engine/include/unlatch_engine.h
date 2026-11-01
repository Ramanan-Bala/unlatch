// SPDX-License-Identifier: GPL-3.0-or-later
// Engine/include/unlatch_engine.h — the only header code outside Engine/ may include.
// The full seam is written in Step 03; Step 01 only exposes the version.
#ifndef UNLATCH_ENGINE_H
#define UNLATCH_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#define UE_ABI_VERSION 1

/// Engine version string, for example "0.0.0".
const char *ue_version(void);

#ifdef __cplusplus
}
#endif

#endif // UNLATCH_ENGINE_H
