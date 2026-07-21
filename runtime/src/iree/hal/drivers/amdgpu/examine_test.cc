// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amdgpu/examine.h"

#include <cstring>
#include <string>

#include "iree/base/api.h"
#include "iree/hal/device_report.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

// Builds a fully-populated device-info struct so the renderer can be exercised
// without any hardware present.
iree_hal_amdgpu_examine_device_info_t MakePopulatedInfo() {
  iree_hal_amdgpu_examine_device_info_t info;
  memset(&info, 0, sizeof(info));

  snprintf(info.product_name, sizeof(info.product_name),
           "AMD Instinct MI300X");
  snprintf(info.uuid, sizeof(info.uuid), "GPU-0e12865a3bf5b7ab");
  snprintf(info.arch, sizeof(info.arch), "gfx942");
  info.node = 2;
  info.has_bdf = true;
  info.pci_domain = 0;
  info.pci_bus = 0xc5;
  info.pci_device = 0x00;
  info.pci_function = 0;
  info.has_compute_units = true;
  info.compute_unit_count = 304;
  info.has_simds_per_cu = true;
  info.simds_per_cu = 4;
  info.has_wavefront_size = true;
  info.wavefront_size = 64;
  info.has_max_clock = true;
  info.max_clock_freq_mhz = 2100;
  info.has_vram = true;
  info.vram_bytes = 206158430208ull;  // 192 GiB

  return info;
}

// Renders a report wrapped in a device object in the requested format.
std::string Render(const iree_hal_amdgpu_examine_device_info_t& info,
                   iree_hal_device_report_format_t format) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  iree_hal_device_report_writer_t w;
  iree_hal_device_report_writer_initialize(format, /*text_indent=*/0, &builder,
                                           &w);
  IREE_CHECK_OK(
      iree_hal_device_report_writer_begin_object(&w, iree_string_view_empty()));
  IREE_CHECK_OK(iree_hal_amdgpu_device_info_append_report(&info, &w));
  IREE_CHECK_OK(iree_hal_device_report_writer_end_object(&w));
  std::string result(iree_string_builder_buffer(&builder),
                     iree_string_builder_size(&builder));
  iree_string_builder_deinitialize(&builder);
  return result;
}

TEST(ExamineTest, JsonHasCommonAndBackendGroups) {
  iree_hal_amdgpu_examine_device_info_t info = MakePopulatedInfo();
  std::string json = Render(info, IREE_HAL_DEVICE_REPORT_FORMAT_JSON);

  EXPECT_NE(json.find("\"common\":{"), std::string::npos);
  EXPECT_NE(json.find("\"vendor\":\"AMD\""), std::string::npos);
  EXPECT_NE(json.find("\"category\":\"gpu\""), std::string::npos);
  EXPECT_NE(json.find("\"architecture\":\"gfx942\""), std::string::npos);
  EXPECT_NE(json.find("\"bdf\":\"0000:c5:00.0\""), std::string::npos);
  EXPECT_NE(json.find("\"backend\":{"), std::string::npos);
  EXPECT_NE(json.find("\"product_name\":\"AMD Instinct MI300X\""),
            std::string::npos);
  EXPECT_NE(json.find("\"uuid\":\"GPU-0e12865a3bf5b7ab\""), std::string::npos);
  EXPECT_NE(json.find("\"compute_units\":304"), std::string::npos);
  EXPECT_NE(json.find("\"max_clock_mhz\":2100"), std::string::npos);
  EXPECT_NE(json.find("\"vram_bytes\":206158430208"), std::string::npos);
}

TEST(ExamineTest, TextIsReadable) {
  iree_hal_amdgpu_examine_device_info_t info = MakePopulatedInfo();
  std::string text = Render(info, IREE_HAL_DEVICE_REPORT_FORMAT_TEXT);

  EXPECT_NE(text.find("common:"), std::string::npos);
  EXPECT_NE(text.find("vendor : AMD"), std::string::npos);
  EXPECT_NE(text.find("architecture : gfx942"), std::string::npos);
  EXPECT_NE(text.find("bdf : 0000:c5:00.0"), std::string::npos);
  EXPECT_NE(text.find("compute_units : 304"), std::string::npos);
  EXPECT_EQ(text.find('{'), std::string::npos);
}

TEST(ExamineTest, OmitsUnsupportedSections) {
  iree_hal_amdgpu_examine_device_info_t info;
  memset(&info, 0, sizeof(info));
  snprintf(info.arch, sizeof(info.arch), "gfx1100");
  std::string json = Render(info, IREE_HAL_DEVICE_REPORT_FORMAT_JSON);

  // Optional sections are absent; identity falls back to N/A.
  EXPECT_EQ(json.find("\"compute_units\":"), std::string::npos);
  EXPECT_EQ(json.find("\"vram_bytes\":"), std::string::npos);
  EXPECT_EQ(json.find("\"max_clock_mhz\":"), std::string::npos);
  EXPECT_NE(json.find("\"bdf\":\"N/A\""), std::string::npos);
  EXPECT_NE(json.find("\"architecture\":\"gfx1100\""), std::string::npos);
}

}  // namespace
