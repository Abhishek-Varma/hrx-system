// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdlib>

#include "iree/hal/drivers/amdxdna/context_cache.h"
#include "iree/hal/drivers/amdxdna/native.h"
#include "iree/testing/gtest.h"

namespace {

constexpr char kCapacityEnv[] = "IREE_HAL_AMDXDNA_CONTEXT_CACHE_CAPACITY";

// The per-architecture budget sizes the cache, so each supported NPU generation
// must map to its characterized value and anything else to 0 (unknown).
TEST(HardwareContextBudgetTest, MapsKnownArchitectures) {
  EXPECT_EQ(
      iree_hal_amdxdna_hardware_context_budget_for_arch(IREE_SV("Phoenix")), 6u);
  EXPECT_EQ(iree_hal_amdxdna_hardware_context_budget_for_arch(IREE_SV("Strix")),
            32u);
  EXPECT_EQ(iree_hal_amdxdna_hardware_context_budget_for_arch(
                IREE_SV("Strix Halo")),
            32u);
  EXPECT_EQ(
      iree_hal_amdxdna_hardware_context_budget_for_arch(IREE_SV("Krackan")),
      32u);
}

TEST(HardwareContextBudgetTest, UnknownArchitectureIsZero) {
  EXPECT_EQ(iree_hal_amdxdna_hardware_context_budget_for_arch(IREE_SV("Navi")),
            0u);
  EXPECT_EQ(
      iree_hal_amdxdna_hardware_context_budget_for_arch(iree_string_view_empty()),
      0u);
}

// Capacity precedence with no env override: a nonzero device budget wins, and a
// zero budget (unknown architecture) falls back to the built-in default of 8.
TEST(ContextCacheCapacityTest, PrefersDeviceBudget) {
  unsetenv(kCapacityEnv);
  EXPECT_EQ(iree_hal_amdxdna_context_cache_resolve_capacity(32), 32u);
}

TEST(ContextCacheCapacityTest, FallsBackToDefaultWhenBudgetUnknown) {
  unsetenv(kCapacityEnv);
  EXPECT_EQ(iree_hal_amdxdna_context_cache_resolve_capacity(0), 8u);
}

// A valid env value overrides the device budget; 0 there disables the bound.
TEST(ContextCacheCapacityTest, EnvironmentOverridesBudget) {
  setenv(kCapacityEnv, "4", 1);
  EXPECT_EQ(iree_hal_amdxdna_context_cache_resolve_capacity(32), 4u);
  setenv(kCapacityEnv, "0", 1);
  EXPECT_EQ(iree_hal_amdxdna_context_cache_resolve_capacity(32), 0u);
  unsetenv(kCapacityEnv);
}

// A malformed env value is ignored in favor of the device budget.
TEST(ContextCacheCapacityTest, MalformedEnvIgnored) {
  setenv(kCapacityEnv, "notanumber", 1);
  EXPECT_EQ(iree_hal_amdxdna_context_cache_resolve_capacity(32), 32u);
  unsetenv(kCapacityEnv);
}

}  // namespace
