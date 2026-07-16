// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amdxdna/examine.h"

#include <cstring>
#include <string>

#include "iree/base/api.h"
#include "iree/hal/device_report.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

// Builds a fully-populated device-info struct so the renderer can be exercised
// without any hardware present.
iree_hal_amdxdna_native_c_device_info_t MakePopulatedInfo() {
  iree_hal_amdxdna_native_c_device_info_t info;
  memset(&info, 0, sizeof(info));

  snprintf(info.arch, sizeof(info.arch), "Strix");
  snprintf(info.bdf, sizeof(info.bdf), "0000:c5:00.1");
  snprintf(info.driver_version, sizeof(info.driver_version), "2.23.0");
  info.has_firmware_version = true;
  info.firmware_major = 1;
  info.firmware_minor = 2;
  info.firmware_patch = 3;
  info.firmware_build = 4;
  info.has_aie_version = true;
  info.aie_major = 2;
  info.aie_minor = 1;
  info.has_aie_metadata = true;
  info.aie_cols = 8;
  info.aie_rows = 6;
  info.aie_col_size = 0x40000;
  info.aie_core.row_count = 4;
  info.aie_mem.row_count = 1;
  info.aie_shim.row_count = 1;
  info.has_power_mode = true;
  info.power_mode = IREE_HAL_AMDXDNA_NATIVE_C_POWER_MODE_TURBO;
  info.clock_count = 2;
  snprintf(info.clocks[0].name, sizeof(info.clocks[0].name), "MP-NPU Clock");
  info.clocks[0].freq_mhz = 1810;
  snprintf(info.clocks[1].name, sizeof(info.clocks[1].name), "H Clock");
  info.clocks[1].freq_mhz = 800;
  info.sensor_count = 1;
  snprintf(info.sensors[0].label, sizeof(info.sensors[0].label), "Total Power");
  snprintf(info.sensors[0].units, sizeof(info.sensors[0].units), "W");
  info.sensors[0].value = 3500;
  info.sensors[0].scale_exponent = -3;  // 3500 mW -> 3.5 W
  info.context_total = 1;
  info.context_count = 1;
  info.contexts[0].context_id = 7;
  info.contexts[0].start_col = 0;
  info.contexts[0].num_col = 4;
  info.contexts[0].pid = 4321;
  info.contexts[0].command_submissions = 10;
  info.contexts[0].command_completions = 10;
  info.contexts[0].errors = 0;

  return info;
}

// Renders a report wrapped in a device object in the requested format.
std::string Render(const iree_hal_amdxdna_native_c_device_info_t& info,
                   iree_hal_device_report_format_t format) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  iree_hal_device_report_writer_t w;
  iree_hal_device_report_writer_initialize(format, /*text_indent=*/0, &builder,
                                           &w);
  IREE_CHECK_OK(
      iree_hal_device_report_writer_begin_object(&w, iree_string_view_empty()));
  IREE_CHECK_OK(iree_hal_amdxdna_device_info_append_report(&info, &w));
  IREE_CHECK_OK(iree_hal_device_report_writer_end_object(&w));
  std::string result(iree_string_builder_buffer(&builder),
                     iree_string_builder_size(&builder));
  iree_string_builder_deinitialize(&builder);
  return result;
}

TEST(ExamineTest, JsonHasCommonAndBackendGroups) {
  iree_hal_amdxdna_native_c_device_info_t info = MakePopulatedInfo();
  std::string json = Render(info, IREE_HAL_DEVICE_REPORT_FORMAT_JSON);

  EXPECT_NE(json.find("\"common\":{"), std::string::npos);
  EXPECT_NE(json.find("\"vendor\":\"AMD\""), std::string::npos);
  EXPECT_NE(json.find("\"category\":\"npu\""), std::string::npos);
  EXPECT_NE(json.find("\"bdf\":\"0000:c5:00.1\""), std::string::npos);
  EXPECT_NE(json.find("\"backend\":{"), std::string::npos);
  EXPECT_NE(json.find("\"firmware\":{"), std::string::npos);
  EXPECT_NE(json.find("\"power_mode\":\"TURBO\""), std::string::npos);
  EXPECT_NE(json.find("\"freq_mhz\":1810"), std::string::npos);
  // Scaled sensor value: 3500 * 10^-3 = 3.500.
  EXPECT_NE(json.find("\"value\":3.500"), std::string::npos);
}

TEST(ExamineTest, TextIsReadable) {
  iree_hal_amdxdna_native_c_device_info_t info = MakePopulatedInfo();
  std::string text = Render(info, IREE_HAL_DEVICE_REPORT_FORMAT_TEXT);

  EXPECT_NE(text.find("common:"), std::string::npos);
  EXPECT_NE(text.find("vendor : AMD"), std::string::npos);
  EXPECT_NE(text.find("bdf : 0000:c5:00.1"), std::string::npos);
  EXPECT_NE(text.find("power_mode : TURBO"), std::string::npos);
  EXPECT_NE(text.find("freq_mhz : 1810"), std::string::npos);
  EXPECT_EQ(text.find('{'), std::string::npos);
}

TEST(ExamineTest, OmitsUnsupportedSections) {
  iree_hal_amdxdna_native_c_device_info_t info;
  memset(&info, 0, sizeof(info));
  snprintf(info.arch, sizeof(info.arch), "Strix");
  std::string json = Render(info, IREE_HAL_DEVICE_REPORT_FORMAT_JSON);

  // Optional sections are absent; identity falls back to N/A.
  EXPECT_EQ(json.find("\"firmware\":{"), std::string::npos);
  EXPECT_EQ(json.find("\"aie_version\":{"), std::string::npos);
  EXPECT_EQ(json.find("\"clocks\":"), std::string::npos);
  EXPECT_EQ(json.find("\"sensors\":"), std::string::npos);
  EXPECT_NE(json.find("\"bdf\":\"N/A\""), std::string::npos);
  // The contexts summary is always present.
  EXPECT_NE(json.find("\"contexts\":{\"total\":0"), std::string::npos);
}

}  // namespace
