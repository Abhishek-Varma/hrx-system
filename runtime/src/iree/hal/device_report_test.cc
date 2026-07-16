// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/device_report.h"

#include <string>

#include "iree/base/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

// Drives a small document through the writer for a given format and returns the
// serialized string.
std::string Emit(iree_hal_device_report_format_t format) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  iree_hal_device_report_writer_t w;
  iree_hal_device_report_writer_initialize(format, /*text_indent=*/0, &builder,
                                           &w);

  IREE_CHECK_OK(
      iree_hal_device_report_writer_begin_object(&w, iree_string_view_empty()));
  IREE_CHECK_OK(iree_hal_device_report_writer_string(&w, IREE_SV("name"),
                                                     IREE_SV("dev0")));
  IREE_CHECK_OK(
      iree_hal_device_report_writer_begin_object(&w, IREE_SV("common")));
  IREE_CHECK_OK(
      iree_hal_device_report_writer_cstring(&w, IREE_SV("vendor"), "AMD"));
  IREE_CHECK_OK(iree_hal_device_report_writer_end_object(&w));
  IREE_CHECK_OK(
      iree_hal_device_report_writer_begin_array(&w, IREE_SV("clocks")));
  IREE_CHECK_OK(
      iree_hal_device_report_writer_begin_object(&w, iree_string_view_empty()));
  IREE_CHECK_OK(iree_hal_device_report_writer_u64(&w, IREE_SV("mhz"), 396));
  IREE_CHECK_OK(iree_hal_device_report_writer_end_object(&w));
  IREE_CHECK_OK(
      iree_hal_device_report_writer_begin_object(&w, iree_string_view_empty()));
  IREE_CHECK_OK(iree_hal_device_report_writer_u64(&w, IREE_SV("mhz"), 800));
  IREE_CHECK_OK(iree_hal_device_report_writer_end_object(&w));
  IREE_CHECK_OK(iree_hal_device_report_writer_end_array(&w));
  IREE_CHECK_OK(iree_hal_device_report_writer_bool(&w, IREE_SV("ok"), true));
  IREE_CHECK_OK(iree_hal_device_report_writer_i64(&w, IREE_SV("n"), -5));
  IREE_CHECK_OK(iree_hal_device_report_writer_end_object(&w));

  std::string result(iree_string_builder_buffer(&builder),
                     iree_string_builder_size(&builder));
  iree_string_builder_deinitialize(&builder);
  return result;
}

TEST(DeviceReportWriterTest, JsonIsExactAndParseable) {
  EXPECT_EQ(
      Emit(IREE_HAL_DEVICE_REPORT_FORMAT_JSON),
      "{\"name\":\"dev0\",\"common\":{\"vendor\":\"AMD\"},\"clocks\":[{\"mhz\":"
      "396},{\"mhz\":800}],\"ok\":true,\"n\":-5}");
}

TEST(DeviceReportWriterTest, TextIsIndentedAndReadable) {
  std::string text = Emit(IREE_HAL_DEVICE_REPORT_FORMAT_TEXT);
  EXPECT_NE(text.find("name : dev0"), std::string::npos);
  EXPECT_NE(text.find("common:"), std::string::npos);
  EXPECT_NE(text.find("  vendor : AMD"), std::string::npos);
  EXPECT_NE(text.find("clocks:"), std::string::npos);
  EXPECT_NE(text.find("mhz : 396"), std::string::npos);
  EXPECT_NE(text.find("ok : true"), std::string::npos);
  EXPECT_NE(text.find("n : -5"), std::string::npos);
  // No JSON punctuation should leak into text output.
  EXPECT_EQ(text.find('{'), std::string::npos);
}

TEST(DeviceReportWriterTest, JsonEscapesStrings) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  iree_hal_device_report_writer_t w;
  iree_hal_device_report_writer_initialize(IREE_HAL_DEVICE_REPORT_FORMAT_JSON,
                                           /*text_indent=*/0, &builder, &w);
  IREE_CHECK_OK(
      iree_hal_device_report_writer_begin_object(&w, iree_string_view_empty()));
  IREE_CHECK_OK(iree_hal_device_report_writer_cstring(&w, IREE_SV("model"),
                                                      "Model \"X\"\n"));
  IREE_CHECK_OK(iree_hal_device_report_writer_end_object(&w));
  std::string json(iree_string_builder_buffer(&builder),
                   iree_string_builder_size(&builder));
  iree_string_builder_deinitialize(&builder);
  EXPECT_EQ(json, "{\"model\":\"Model \\\"X\\\"\\n\"}");
}

TEST(DeviceReportWriterTest, RejectsMismatchedScopes) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(iree_allocator_system(), &builder);
  iree_hal_device_report_writer_t w;
  iree_hal_device_report_writer_initialize(IREE_HAL_DEVICE_REPORT_FORMAT_JSON,
                                           /*text_indent=*/0, &builder, &w);
  IREE_CHECK_OK(
      iree_hal_device_report_writer_begin_object(&w, iree_string_view_empty()));
  // Closing an object with end_array must fail.
  iree_status_t status = iree_hal_device_report_writer_end_array(&w);
  EXPECT_TRUE(iree_status_is_failed_precondition(status));
  iree_status_ignore(status);
  iree_string_builder_deinitialize(&builder);
}

}  // namespace
