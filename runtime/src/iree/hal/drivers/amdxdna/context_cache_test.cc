// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amdxdna/context_cache.h"

#include <cstdint>
#include <vector>

#include "iree/hal/drivers/amdxdna/executable_internal.h"
#include "iree/testing/gtest.h"

namespace {

iree_const_byte_span_t Bytes(const std::vector<uint8_t>& data) {
  return iree_make_const_byte_span(data.data(), data.size());
}

iree_hal_amdxdna_u32_list_t List(std::vector<uint32_t>& words) {
  iree_hal_amdxdna_u32_list_t list = {};
  list.data = words.data();
  list.count = words.size();
  return list;
}

iree_hal_amdxdna_context_cache_key_t Key(
    iree_const_byte_span_t pdi, const char* kernel_name,
    const iree_hal_amdxdna_u32_list_t* control_codes,
    iree_host_size_t control_code_count) {
  iree_hal_amdxdna_context_cache_key_t key = {};
  key.pdi = pdi;
  key.xclbin = iree_const_byte_span_empty();
  key.kernel_name = iree_make_cstring_view(kernel_name);
  key.control_codes = control_codes;
  key.control_code_count = control_code_count;
  return key;
}

// Cross-executable reuse: two distinct executables that lowered to byte-for-byte
// identical PDI + kernel name + control code -- through independent backing
// storage -- must be treated as the same context so the cache is shared.
TEST(ContextCacheKeyTest, IdenticalContentThroughDistinctStorageMatches) {
  std::vector<uint8_t> pdi_a = {1, 2, 3, 4};
  std::vector<uint8_t> pdi_b = {1, 2, 3, 4};  // distinct storage, same bytes
  std::vector<uint32_t> cc_a = {100, 101, 102, 103};
  std::vector<uint32_t> cc_b = {100, 101, 102, 103};
  iree_hal_amdxdna_u32_list_t list_a = List(cc_a);
  iree_hal_amdxdna_u32_list_t list_b = List(cc_b);

  auto lhs = Key(Bytes(pdi_a), "MLIR_AIE", &list_a, 1);
  auto rhs = Key(Bytes(pdi_b), "MLIR_AIE", &list_b, 1);

  EXPECT_TRUE(iree_hal_amdxdna_context_cache_key_equal(&lhs, &rhs));
}

// Core regression: two GEMMs (e.g. PRED vs FAIL / a K/N-transpose pair) share a
// byte-identical PDI and kernel name but differ in control code. They must NOT
// share a hardware context; otherwise one runs against the other's residual
// array state, producing wrong results or a firmware hang.
TEST(ContextCacheKeyTest, SameImageDifferentControlCodeMisses) {
  std::vector<uint8_t> pdi = {9, 8, 7, 6};
  std::vector<uint32_t> pred = {100, 101, 102, 103};
  std::vector<uint32_t> fail = {100, 999, 102, 103};  // same shape, diff content
  iree_hal_amdxdna_u32_list_t pred_list = List(pred);
  iree_hal_amdxdna_u32_list_t fail_list = List(fail);

  auto lhs = Key(Bytes(pdi), "MLIR_AIE", &pred_list, 1);
  auto rhs = Key(Bytes(pdi), "MLIR_AIE", &fail_list, 1);

  EXPECT_FALSE(iree_hal_amdxdna_context_cache_key_equal(&lhs, &rhs))
      << "same PDI + kernel name but different control code must not reuse a "
         "context";
}

TEST(ContextCacheKeyTest, DifferentControlCodeCountMisses) {
  std::vector<uint8_t> pdi = {1, 1, 1};
  std::vector<uint32_t> a = {1, 2, 3};
  std::vector<uint32_t> b = {1, 2, 3};
  iree_hal_amdxdna_u32_list_t one = List(a);
  iree_hal_amdxdna_u32_list_t two[2] = {List(a), List(b)};

  auto lhs = Key(Bytes(pdi), "MLIR_AIE", &one, 1);
  auto rhs = Key(Bytes(pdi), "MLIR_AIE", two, 2);

  EXPECT_FALSE(iree_hal_amdxdna_context_cache_key_equal(&lhs, &rhs));
}

TEST(ContextCacheKeyTest, DifferentPdiMisses) {
  std::vector<uint8_t> pdi_a = {1, 2, 3, 4};
  std::vector<uint8_t> pdi_b = {1, 2, 3, 5};
  std::vector<uint32_t> cc = {7, 7, 7};
  iree_hal_amdxdna_u32_list_t list = List(cc);

  auto lhs = Key(Bytes(pdi_a), "MLIR_AIE", &list, 1);
  auto rhs = Key(Bytes(pdi_b), "MLIR_AIE", &list, 1);

  EXPECT_FALSE(iree_hal_amdxdna_context_cache_key_equal(&lhs, &rhs));
}

TEST(ContextCacheKeyTest, DifferentKernelNameMisses) {
  std::vector<uint8_t> pdi = {2, 4, 6};
  std::vector<uint32_t> cc = {1};
  iree_hal_amdxdna_u32_list_t list = List(cc);

  auto lhs = Key(Bytes(pdi), "kernel_a", &list, 1);
  auto rhs = Key(Bytes(pdi), "kernel_b", &list, 1);

  EXPECT_FALSE(iree_hal_amdxdna_context_cache_key_equal(&lhs, &rhs));
}

// An image with no control code (empty run list) still matches itself: the key
// falls back to PDI + kernel name and must not spuriously miss.
TEST(ContextCacheKeyTest, EmptyControlCodeMatches) {
  std::vector<uint8_t> pdi = {5, 5, 5, 5};

  auto lhs = Key(Bytes(pdi), "MLIR_AIE", nullptr, 0);
  auto rhs = Key(Bytes(pdi), "MLIR_AIE", nullptr, 0);

  EXPECT_TRUE(iree_hal_amdxdna_context_cache_key_equal(&lhs, &rhs));
}

// Multi-control-code run lists compare element-wise: a difference in any inner
// list (here the second) must miss even when the first list is identical.
TEST(ContextCacheKeyTest, MultiListComparesEveryList) {
  std::vector<uint8_t> pdi = {3, 3, 3};
  std::vector<uint32_t> first = {10, 11};
  std::vector<uint32_t> second_a = {20, 21};
  std::vector<uint32_t> second_b = {20, 22};
  iree_hal_amdxdna_u32_list_t lists_a[2] = {List(first), List(second_a)};
  iree_hal_amdxdna_u32_list_t lists_b[2] = {List(first), List(second_b)};
  iree_hal_amdxdna_u32_list_t lists_a_copy[2] = {List(first), List(second_a)};

  auto lhs = Key(Bytes(pdi), "MLIR_AIE", lists_a, 2);
  auto rhs_diff = Key(Bytes(pdi), "MLIR_AIE", lists_b, 2);
  auto rhs_same = Key(Bytes(pdi), "MLIR_AIE", lists_a_copy, 2);

  EXPECT_FALSE(iree_hal_amdxdna_context_cache_key_equal(&lhs, &rhs_diff));
  EXPECT_TRUE(iree_hal_amdxdna_context_cache_key_equal(&lhs, &rhs_same));
}

}  // namespace
