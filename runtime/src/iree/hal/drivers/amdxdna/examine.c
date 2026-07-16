// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amdxdna/examine.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "iree/hal/drivers/amdxdna/api.h"

static const char* iree_hal_amdxdna_or_na(const char* value) {
  return (value != NULL && value[0] != '\0') ? value : "N/A";
}

static const char* iree_hal_amdxdna_power_mode_string(
    iree_hal_amdxdna_native_c_power_mode_t mode) {
  switch (mode) {
    case IREE_HAL_AMDXDNA_NATIVE_C_POWER_MODE_DEFAULT:
      return "DEFAULT";
    case IREE_HAL_AMDXDNA_NATIVE_C_POWER_MODE_LOW:
      return "LOW";
    case IREE_HAL_AMDXDNA_NATIVE_C_POWER_MODE_MEDIUM:
      return "MEDIUM";
    case IREE_HAL_AMDXDNA_NATIVE_C_POWER_MODE_HIGH:
      return "HIGH";
    case IREE_HAL_AMDXDNA_NATIVE_C_POWER_MODE_TURBO:
      return "TURBO";
    default:
      return "UNKNOWN";
  }
}

// Returns |value * 10^exponent| as a double without depending on libm.
static double iree_hal_amdxdna_scale_value(uint32_t value, int8_t exponent) {
  double result = (double)value;
  for (int8_t i = 0; i < exponent; ++i) result *= 10.0;
  for (int8_t i = 0; i > exponent; --i) result /= 10.0;
  return result;
}

static iree_status_t iree_hal_amdxdna_append_common(
    const iree_hal_amdxdna_native_c_device_info_t* info,
    iree_hal_device_report_writer_t* writer) {
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
      writer, IREE_SV(IREE_HAL_DEVICE_REPORT_GROUP_COMMON)));
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_writer_cstring(writer, IREE_SV("vendor"), "AMD"));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
      writer, IREE_SV("category"), "npu"));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
      writer, IREE_SV("architecture"), iree_hal_amdxdna_or_na(info->arch)));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
      writer, IREE_SV("bdf"), iree_hal_amdxdna_or_na(info->bdf)));
  return iree_hal_device_report_writer_end_object(writer);
}

static iree_status_t iree_hal_amdxdna_append_backend(
    const iree_hal_amdxdna_native_c_device_info_t* info,
    iree_hal_device_report_writer_t* writer) {
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
      writer, IREE_SV(IREE_HAL_DEVICE_REPORT_GROUP_BACKEND)));

  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
      writer, IREE_SV("driver_version"),
      iree_hal_amdxdna_or_na(info->driver_version)));

  if (info->has_power_mode) {
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
        writer, IREE_SV("power_mode"),
        iree_hal_amdxdna_power_mode_string(info->power_mode)));
  }

  if (info->has_firmware_version) {
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
        writer, IREE_SV("firmware")));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("major"), info->firmware_major));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("minor"), info->firmware_minor));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("patch"), info->firmware_patch));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("build"), info->firmware_build));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(writer));
  }

  if (info->has_aie_version) {
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
        writer, IREE_SV("aie_version")));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("major"), info->aie_major));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("minor"), info->aie_minor));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(writer));
  }

  if (info->has_aie_metadata) {
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
        writer, IREE_SV("aie_array")));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("cols"), info->aie_cols));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("rows"), info->aie_rows));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("col_size"), info->aie_col_size));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("core_rows"), info->aie_core.row_count));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("mem_rows"), info->aie_mem.row_count));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("shim_rows"), info->aie_shim.row_count));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(writer));
  }

  if (info->clock_count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_hal_device_report_writer_begin_array(writer, IREE_SV("clocks")));
    for (uint32_t c = 0; c < info->clock_count; ++c) {
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
          writer, iree_string_view_empty()));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
          writer, IREE_SV("name"),
          iree_hal_amdxdna_or_na(info->clocks[c].name)));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
          writer, IREE_SV("freq_mhz"), info->clocks[c].freq_mhz));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(writer));
    }
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_array(writer));
  }

  if (info->sensor_count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_hal_device_report_writer_begin_array(writer, IREE_SV("sensors")));
    for (uint32_t s = 0; s < info->sensor_count; ++s) {
      const iree_hal_amdxdna_native_c_sensor_t* sensor = &info->sensors[s];
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
          writer, iree_string_view_empty()));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
          writer, IREE_SV("label"), iree_hal_amdxdna_or_na(sensor->label)));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_f64(
          writer, IREE_SV("value"),
          iree_hal_amdxdna_scale_value(sensor->value, sensor->scale_exponent)));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
          writer, IREE_SV("units"), iree_hal_amdxdna_or_na(sensor->units)));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(writer));
    }
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_array(writer));
  }

  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_writer_begin_object(writer, IREE_SV("contexts")));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
      writer, IREE_SV("total"), info->context_total));
  if (info->context_count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_hal_device_report_writer_begin_array(writer, IREE_SV("active")));
    for (uint32_t k = 0; k < info->context_count; ++k) {
      const iree_hal_amdxdna_native_c_context_stats_t* ctx = &info->contexts[k];
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
          writer, iree_string_view_empty()));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
          writer, IREE_SV("id"), ctx->context_id));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
          writer, IREE_SV("start_col"), ctx->start_col));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
          writer, IREE_SV("num_col"), ctx->num_col));
      IREE_RETURN_IF_ERROR(
          iree_hal_device_report_writer_i64(writer, IREE_SV("pid"), ctx->pid));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
          writer, IREE_SV("submitted"), ctx->command_submissions));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
          writer, IREE_SV("completed"), ctx->command_completions));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
          writer, IREE_SV("errors"), ctx->errors));
      IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(writer));
    }
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_array(writer));
  }
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(writer));

  return iree_hal_device_report_writer_end_object(writer);
}

iree_status_t iree_hal_amdxdna_device_info_append_report(
    const iree_hal_amdxdna_native_c_device_info_t* info,
    iree_hal_device_report_writer_t* writer) {
  IREE_ASSERT_ARGUMENT(info);
  IREE_ASSERT_ARGUMENT(writer);
  IREE_RETURN_IF_ERROR(iree_hal_amdxdna_append_common(info, writer));
  return iree_hal_amdxdna_append_backend(info, writer);
}

//===----------------------------------------------------------------------===//
// Device collection
//===----------------------------------------------------------------------===//

iree_status_t iree_hal_amdxdna_examine_collect_devices(
    iree_allocator_t host_allocator,
    iree_hal_amdxdna_examine_report_t* out_report) {
  IREE_ASSERT_ARGUMENT(out_report);

  iree_hal_amdxdna_native_c_device_list_t list;
  IREE_RETURN_IF_ERROR(iree_hal_amdxdna_native_c_enumerate_devices(&list));

  iree_host_size_t count = list.count;
  if (count > IREE_HAL_AMDXDNA_NATIVE_C_MAX_DEVICES) {
    count = IREE_HAL_AMDXDNA_NATIVE_C_MAX_DEVICES;
  }
  out_report->device_count = count;

  for (iree_host_size_t i = 0; i < count; ++i) {
    iree_hal_amdxdna_examine_device_t* entry = &out_report->devices[i];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->path, sizeof(entry->path), "%s", list.paths[i]);

    struct iree_hal_amdxdna_device_params params;
    iree_hal_amdxdna_device_options_initialize(&params);
    params.device_path = iree_make_cstring_view(entry->path);

    iree_hal_amdxdna_native_device_t* device = NULL;
    iree_status_t status = iree_hal_amdxdna_native_device_c_create(
        &params, host_allocator, &device);
    if (iree_status_is_ok(status)) {
      status =
          iree_hal_amdxdna_native_device_c_query_info(device, &entry->info);
      iree_hal_amdxdna_native_device_c_destroy(device);
    }

    if (iree_status_is_ok(status)) {
      entry->info_valid = true;
    } else {
      entry->info_valid = false;
      snprintf(entry->status_message, sizeof(entry->status_message), "%s",
               iree_status_code_string(iree_status_code(status)));
      iree_status_ignore(status);
    }
  }

  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Curated text formatting
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_amdxdna_append_row(iree_string_builder_t* builder,
                                                 const char* key,
                                                 const char* value) {
  return iree_string_builder_append_format(builder, "  %-20s : %s\n", key,
                                           iree_hal_amdxdna_or_na(value));
}

static iree_status_t iree_hal_amdxdna_examine_format_host_text(
    const iree_hal_amdxdna_host_info_t* host, iree_string_builder_t* builder) {
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_cstring(builder, "System Configuration\n"));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_append_row(builder, "OS Name", host->os_name));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_append_row(builder, "Release", host->release));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_append_row(builder, "Machine", host->machine));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %" PRIu32 "\n", "CPU Cores", host->cpu_cores));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %" PRIu64 " MB\n", "Memory",
      host->memory_bytes / (1024u * 1024u)));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_append_row(builder, "Distribution", host->distribution));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_append_row(builder, "Model", host->model));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_append_row(builder, "BIOS Vendor", host->bios_vendor));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_append_row(builder, "BIOS Version", host->bios_version));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_append_row(builder, "Processor", host->processor));
  return iree_ok_status();
}

static iree_status_t iree_hal_amdxdna_examine_format_device_text(
    iree_host_size_t index, const iree_hal_amdxdna_examine_device_t* device,
    iree_string_builder_t* builder) {
  if (!device->info_valid) {
    return iree_string_builder_append_format(
        builder, "  [%" PRIhsz "] %s  (unable to query: %s)\n", index,
        device->path, iree_hal_amdxdna_or_na(device->status_message));
  }

  const iree_hal_amdxdna_native_c_device_info_t* info = &device->info;
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  [%" PRIhsz "] %s\n", index, device->path));
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_format(builder, "        %-16s : %s\n", "BDF",
                                        iree_hal_amdxdna_or_na(info->bdf)));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "        %-16s : %s\n", "Architecture",
      iree_hal_amdxdna_or_na(info->arch)));
  if (info->has_power_mode) {
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder, "        %-16s : %s\n", "Power Mode",
        iree_hal_amdxdna_power_mode_string(info->power_mode)));
  }
  if (info->has_firmware_version) {
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder,
        "        %-16s : %" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32 "\n",
        "Firmware", info->firmware_major, info->firmware_minor,
        info->firmware_patch, info->firmware_build));
  }
  if (info->has_aie_version) {
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder, "        %-16s : %" PRIu32 ".%" PRIu32 "\n", "AIE Version",
        info->aie_major, info->aie_minor));
  }
  if (info->has_aie_metadata) {
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder,
        "        %-16s : %" PRIu16 " cols x %" PRIu16
        " rows (col size 0x%" PRIx32 ")\n",
        "AIE Array", info->aie_cols, info->aie_rows, info->aie_col_size));
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder,
        "          core/mem/shim rows : %" PRIu16 "/%" PRIu16 "/%" PRIu16 "\n",
        info->aie_core.row_count, info->aie_mem.row_count,
        info->aie_shim.row_count));
  }
  if (info->clock_count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_string_builder_append_cstring(builder, "        Clocks\n"));
    for (uint32_t c = 0; c < info->clock_count; ++c) {
      IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
          builder, "          %-14s : %" PRIu32 " MHz\n",
          iree_hal_amdxdna_or_na(info->clocks[c].name),
          info->clocks[c].freq_mhz));
    }
  }
  if (info->sensor_count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_string_builder_append_cstring(builder, "        Sensors\n"));
    for (uint32_t s = 0; s < info->sensor_count; ++s) {
      const iree_hal_amdxdna_native_c_sensor_t* sensor = &info->sensors[s];
      const double value =
          iree_hal_amdxdna_scale_value(sensor->value, sensor->scale_exponent);
      IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
          builder, "          %-14s : %.3f %s\n",
          iree_hal_amdxdna_or_na(sensor->label), value,
          iree_hal_amdxdna_or_na(sensor->units)));
    }
  }
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "        %-16s : %" PRIu32 " active\n", "HW Contexts",
      info->context_total));
  for (uint32_t k = 0; k < info->context_count; ++k) {
    const iree_hal_amdxdna_native_c_context_stats_t* ctx = &info->contexts[k];
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder,
        "          [ctx %" PRIu32 "] cols %" PRIu32 "-%" PRIu32 " pid %" PRId64
        " submitted %" PRIu64 " completed %" PRIu64 " errors %" PRIu64 "\n",
        ctx->context_id, ctx->start_col,
        ctx->start_col + (ctx->num_col ? ctx->num_col - 1 : 0), ctx->pid,
        ctx->command_submissions, ctx->command_completions, ctx->errors));
  }
  return iree_ok_status();
}

iree_status_t iree_hal_amdxdna_examine_format_text(
    const iree_hal_amdxdna_examine_report_t* report,
    iree_string_builder_t* builder) {
  IREE_ASSERT_ARGUMENT(report);
  IREE_ASSERT_ARGUMENT(builder);

  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_examine_format_host_text(&report->host, builder));

  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "\nHRX\n"));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_append_row(builder, "Version", report->runtime_version));
  // The amdxdna kernel driver version is a system property; surface the first
  // device's reading when available.
  const char* driver_version = "";
  for (iree_host_size_t i = 0; i < report->device_count; ++i) {
    if (report->devices[i].info_valid &&
        report->devices[i].info.driver_version[0] != '\0') {
      driver_version = report->devices[i].info.driver_version;
      break;
    }
  }
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_append_row(builder, "amdxdna Version", driver_version));

  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_cstring(builder, "\nDevice(s) Present\n"));
  if (report->device_count == 0) {
    return iree_string_builder_append_cstring(builder, "  0 devices found\n");
  }
  for (iree_host_size_t i = 0; i < report->device_count; ++i) {
    IREE_RETURN_IF_ERROR(iree_hal_amdxdna_examine_format_device_text(
        i, &report->devices[i], builder));
  }
  return iree_ok_status();
}
