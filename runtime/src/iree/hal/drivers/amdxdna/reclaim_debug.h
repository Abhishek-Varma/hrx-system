// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// TEMPORARY reclaim/context-recreate diagnostics. Gated on HRX_RECLAIM_DEBUG.
// Remove before shipping. Header-only so no CMake changes are required.

#ifndef IREE_HAL_DRIVERS_AMDXDNA_RECLAIM_DEBUG_H_
#define IREE_HAL_DRIVERS_AMDXDNA_RECLAIM_DEBUG_H_

#include <stdio.h>
#include <stdlib.h>

#if defined(__cplusplus)
extern "C" {
#endif

static inline int iree_hal_amdxdna_reclaim_debug_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char* e = getenv("HRX_RECLAIM_DEBUG");
    cached = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  return cached;
}

// When enabled, a dispatch that fails with the ert state 5/8 displacement
// signature triggers a KMD-legal context recreate (destroy + create + PDI
// re-setup on a fresh hardware context) and a single retry, instead of
// surfacing the failure. Gated on HRX_SELF_HEAL_RECREATE so it can be A/B'd
// against the capacity-headroom default.
static inline int iree_hal_amdxdna_self_heal_recreate_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char* e = getenv("HRX_SELF_HEAL_RECREATE");
    cached = (e && e[0] && e[0] != '0') ? 1 : 0;
  }
  return cached;
}

#define HRXRDBG(...)                                 \
  do {                                               \
    if (iree_hal_amdxdna_reclaim_debug_enabled()) {  \
      fprintf(stderr, "[HRXRDBG] " __VA_ARGS__);     \
      fprintf(stderr, "\n");                         \
      fflush(stderr);                                \
    }                                                \
  } while (0)

#if defined(__cplusplus)
}
#endif

#endif  // IREE_HAL_DRIVERS_AMDXDNA_RECLAIM_DEBUG_H_
