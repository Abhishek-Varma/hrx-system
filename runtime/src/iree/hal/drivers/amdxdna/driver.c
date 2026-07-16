// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "iree/hal/drivers/amdxdna/api.h"
#include "iree/hal/drivers/amdxdna/examine.h"
#include "iree/hal/drivers/amdxdna/native.h"
#include "iree/hal/drivers/amdxdna/util.h"

#define IREE_HAL_AMDXDNA_DEVICE_ID_DEFAULT 0

struct iree_hal_amdxdna_driver {
  iree_hal_resource_t resource;
  iree_allocator_t host_allocator;
  struct iree_hal_amdxdna_driver_options options;
  // + trailing identifier string storage
  iree_string_view_t identifier;
};

static const iree_hal_driver_vtable_t iree_hal_amdxdna_driver_vtable;

static bool iree_hal_amdxdna_power_mode_is_valid(iree_string_view_t value) {
  return iree_string_view_equal(value, IREE_SV("default")) ||
         iree_string_view_equal(value, IREE_SV("low")) ||
         iree_string_view_equal(value, IREE_SV("medium")) ||
         iree_string_view_equal(value, IREE_SV("high")) ||
         iree_string_view_equal(value, IREE_SV("turbo"));
}

static iree_status_t iree_hal_amdxdna_parse_non_negative_int32_option(
    iree_string_view_t key, iree_string_view_t value, int32_t* out_value) {
  if (!iree_string_view_atoi_int32(value, out_value)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "Option '%.*s' expected to be int. Got: '%.*s'",
                            (int)key.size, key.data, (int)value.size,
                            value.data);
  }
  if (*out_value < 0) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "Option '%.*s' expected to be >= 0. Got: '%.*s'",
                            (int)key.size, key.data, (int)value.size,
                            value.data);
  }
  return iree_ok_status();
}

static iree_status_t iree_hal_amdxdna_parse_bool_option(
    iree_string_view_t key, iree_string_view_t value, bool* out_value) {
  if (iree_string_view_equal(value, IREE_SV("1")) ||
      iree_string_view_equal(value, IREE_SV("true"))) {
    *out_value = true;
    return iree_ok_status();
  }
  if (iree_string_view_equal(value, IREE_SV("0")) ||
      iree_string_view_equal(value, IREE_SV("false"))) {
    *out_value = false;
    return iree_ok_status();
  }
  return iree_make_status(
      IREE_STATUS_FAILED_PRECONDITION,
      "Option '%.*s' expected to be bool (0/1/false/true). Got: '%.*s'",
      (int)key.size, key.data, (int)value.size, value.data);
}

void iree_hal_amdxdna_driver_options_initialize(
    struct iree_hal_amdxdna_driver_options* out_options) {
  IREE_TRACE_ZONE_BEGIN(z0);

  memset(out_options, 0, sizeof(*out_options));
  iree_hal_amdxdna_device_options_initialize(
      &out_options->default_device_params);

  IREE_TRACE_ZONE_END(z0);
}

iree_status_t iree_hal_amdxdna_device_options_parse(
    struct iree_hal_amdxdna_device_params* params, iree_host_size_t pairs_size,
    const iree_string_pair_t* pairs) {
  IREE_ASSERT_ARGUMENT(params);
  IREE_ASSERT_ARGUMENT(!pairs_size || pairs);

  for (iree_host_size_t i = 0; i < pairs_size; ++i) {
    iree_string_view_t key = pairs[i].key;
    iree_string_view_t value = pairs[i].value;
    int32_t ivalue = 0;

    if (iree_string_view_equal(key, IREE_SV("amdxdna_n_core_rows"))) {
      IREE_RETURN_IF_ERROR(iree_hal_amdxdna_parse_non_negative_int32_option(
          key, value, &ivalue));
      params->n_core_rows = ivalue;
    } else if (iree_string_view_equal(key, IREE_SV("amdxdna_n_core_cols"))) {
      IREE_RETURN_IF_ERROR(iree_hal_amdxdna_parse_non_negative_int32_option(
          key, value, &ivalue));
      params->n_core_cols = ivalue;
    } else if (iree_string_view_equal(key, IREE_SV("amdxdna_device_path"))) {
      params->device_path = value;
    } else if (iree_string_view_equal(key, IREE_SV("amdxdna_power_mode"))) {
      if (!iree_hal_amdxdna_power_mode_is_valid(value)) {
        return iree_make_status(
            IREE_STATUS_FAILED_PRECONDITION,
            "Option 'amdxdna_power_mode' expected to be default | low | "
            "medium | high | turbo. Got: '%.*s'",
            (int)value.size, value.data);
      }
      params->power_mode = value;
    } else {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "Unrecognized option: %.*s", (int)key.size,
                              key.data);
    }
  }
  return iree_ok_status();
}

IREE_API_EXPORT iree_status_t iree_hal_amdxdna_driver_create(
    iree_string_view_t identifier,
    const struct iree_hal_amdxdna_driver_options* options,
    iree_allocator_t host_allocator, iree_hal_driver_t** out_driver) {
  IREE_ASSERT_ARGUMENT(options);
  IREE_ASSERT_ARGUMENT(out_driver);
  *out_driver = NULL;
  IREE_TRACE_ZONE_BEGIN(z0);

  const struct iree_hal_amdxdna_device_params* device_params =
      &options->default_device_params;
  struct iree_hal_amdxdna_driver* driver = NULL;
  iree_host_size_t total_size = sizeof(*driver);
  if (IREE_UNLIKELY(
          !iree_host_size_checked_add(total_size, identifier.size,
                                      &total_size) ||
          !iree_host_size_checked_add(
              total_size, device_params->device_path.size, &total_size) ||
          !iree_host_size_checked_add(
              total_size, device_params->power_mode.size, &total_size))) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "amdxdna driver option strings are too large");
  }
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(host_allocator, total_size, (void**)&driver));
  iree_hal_resource_initialize(&iree_hal_amdxdna_driver_vtable,
                               &driver->resource);
  driver->host_allocator = host_allocator;
  char* string_storage = (char*)driver + sizeof(*driver);
  iree_string_view_append_to_buffer(identifier, &driver->identifier,
                                    string_storage);
  string_storage += identifier.size;
  memcpy(&driver->options, options, sizeof(*options));
  string_storage += iree_string_view_append_to_buffer(
      device_params->device_path,
      &driver->options.default_device_params.device_path, string_storage);
  string_storage += iree_string_view_append_to_buffer(
      device_params->power_mode,
      &driver->options.default_device_params.power_mode, string_storage);
  *out_driver = (iree_hal_driver_t*)driver;

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static void iree_hal_amdxdna_driver_destroy(iree_hal_driver_t* base_driver) {
  struct iree_hal_amdxdna_driver* driver = IREE_HAL_AMDXDNA_CHECKED_VTABLE_CAST(
      base_driver, iree_hal_amdxdna_driver_vtable,
      struct iree_hal_amdxdna_driver);
  iree_allocator_t host_allocator = driver->host_allocator;
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_allocator_free(host_allocator, driver);

  IREE_TRACE_ZONE_END(z0);
}

static iree_status_t iree_hal_amdxdna_driver_query_available_devices(
    iree_hal_driver_t* base_driver, iree_allocator_t host_allocator,
    iree_host_size_t* out_device_info_count,
    iree_hal_device_info_t** out_device_infos) {
  IREE_TRACE_ZONE_BEGIN(z0);
  *out_device_info_count = 0;
  *out_device_infos = NULL;

  struct iree_hal_amdxdna_driver* driver = IREE_HAL_AMDXDNA_CHECKED_VTABLE_CAST(
      base_driver, iree_hal_amdxdna_driver_vtable,
      struct iree_hal_amdxdna_driver);

  // Report the configured logical device without opening the native driver.
  // Platform-specific discovery can be added under the native layer later; the
  // HAL driver keeps this path OS-neutral.
  iree_string_view_t configured_path =
      driver->options.default_device_params.device_path;
  if (iree_string_view_is_empty(configured_path)) {
    configured_path = IREE_SV("amdxdna://default");
  }
  iree_string_view_t arch = IREE_SV("amdxdna");

  // Single allocation: the info struct followed by its path and name bytes, so
  // the caller frees everything with one iree_allocator_free.
  iree_host_size_t total_size =
      sizeof(iree_hal_device_info_t) + configured_path.size + arch.size;
  iree_hal_device_info_t* device_infos = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0,
      iree_allocator_malloc(host_allocator, total_size, (void**)&device_infos));
  memset(device_infos, 0, sizeof(*device_infos));
  char* storage = (char*)device_infos + sizeof(iree_hal_device_info_t);
  device_infos[0].device_id = IREE_HAL_AMDXDNA_DEVICE_ID_DEFAULT;
  memcpy(storage, configured_path.data, configured_path.size);
  device_infos[0].path = iree_make_string_view(storage, configured_path.size);
  storage += configured_path.size;
  memcpy(storage, arch.data, arch.size);
  device_infos[0].name = iree_make_string_view(storage, arch.size);

  *out_device_info_count = 1;
  *out_device_infos = device_infos;

  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

// Appends the driver's configured (static) parameters as a typed fallback used
// when no live device can be opened/queried, so callers always get something
// useful. |status_text| describes why the live device was unavailable.
static iree_status_t iree_hal_amdxdna_driver_append_configured_report(
    struct iree_hal_amdxdna_driver* driver, const char* status_text,
    iree_hal_device_report_writer_t* writer) {
  const struct iree_hal_amdxdna_device_params* params =
      &driver->options.default_device_params;
  const iree_string_view_t device_path =
      params->device_path.size ? params->device_path : IREE_SV("<auto>");
  const iree_string_view_t power_mode =
      params->power_mode.size ? params->power_mode : IREE_SV("<unchanged>");

  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
      writer, IREE_SV(IREE_HAL_DEVICE_REPORT_GROUP_COMMON)));
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_writer_cstring(writer, IREE_SV("vendor"), "AMD"));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
      writer, IREE_SV("category"), "npu"));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(writer));

  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
      writer, IREE_SV(IREE_HAL_DEVICE_REPORT_GROUP_BACKEND)));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
      writer, IREE_SV("status"), status_text));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_string(
      writer, IREE_SV("device_path"), device_path));
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_writer_begin_object(writer, IREE_SV("core_grid")));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_i64(
      writer, IREE_SV("rows"), params->n_core_rows));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_i64(
      writer, IREE_SV("cols"), params->n_core_cols));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(writer));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_string(
      writer, IREE_SV("power_mode"), power_mode));
  return iree_hal_device_report_writer_end_object(writer);
}

// Writes the amdxdna device report into |writer|'s current object.
// Opens/queries a live device when present; otherwise falls back to configured
// parameters. Always returns success unless the writer itself fails.
static iree_status_t iree_hal_amdxdna_driver_append_live_report(
    struct iree_hal_amdxdna_driver* driver,
    iree_hal_device_report_writer_t* writer) {
  iree_hal_amdxdna_native_c_device_list_t list;
  iree_status_t enum_status =
      iree_hal_amdxdna_native_c_enumerate_devices(&list);
  if (iree_status_is_ok(enum_status) && list.count > 0) {
    struct iree_hal_amdxdna_device_params params =
        driver->options.default_device_params;
    params.device_path = iree_make_cstring_view(list.paths[0]);
    iree_hal_amdxdna_native_device_t* device = NULL;
    iree_status_t status = iree_hal_amdxdna_native_device_c_create(
        &params, driver->host_allocator, &device);
    if (iree_status_is_ok(status)) {
      iree_hal_amdxdna_native_c_device_info_t info;
      status = iree_hal_amdxdna_native_device_c_query_info(device, &info);
      iree_hal_amdxdna_native_device_c_destroy(device);
      if (iree_status_is_ok(status)) {
        return iree_hal_amdxdna_device_info_append_report(&info, writer);
      }
    }
    // A device is present but could not be opened/queried; report why but still
    // succeed so the aggregator can continue with other drivers/devices.
    const char* code = iree_status_code_string(iree_status_code(status));
    iree_status_ignore(status);
    return iree_hal_amdxdna_driver_append_configured_report(driver, code,
                                                            writer);
  }
  iree_status_ignore(enum_status);
  return iree_hal_amdxdna_driver_append_configured_report(
      driver, "no device bound", writer);
}

static iree_status_t iree_hal_amdxdna_driver_dump_device_report(
    iree_hal_driver_t* base_driver, iree_hal_device_id_t device_id,
    iree_hal_device_report_writer_t* writer) {
  struct iree_hal_amdxdna_driver* driver = IREE_HAL_AMDXDNA_CHECKED_VTABLE_CAST(
      base_driver, iree_hal_amdxdna_driver_vtable,
      struct iree_hal_amdxdna_driver);
  if (device_id != IREE_HAL_AMDXDNA_DEVICE_ID_DEFAULT) {
    return iree_make_status(IREE_STATUS_NOT_FOUND,
                            "no amdxdna device with id %" PRIu64,
                            (uint64_t)device_id);
  }
  return iree_hal_amdxdna_driver_append_live_report(driver, writer);
}

// Legacy free-form text hook, derived from the structured report so there is a
// single source of truth for both formats.
static iree_status_t iree_hal_amdxdna_driver_dump_device_info(
    iree_hal_driver_t* base_driver, iree_hal_device_id_t device_id,
    iree_string_builder_t* builder) {
  struct iree_hal_amdxdna_driver* driver = IREE_HAL_AMDXDNA_CHECKED_VTABLE_CAST(
      base_driver, iree_hal_amdxdna_driver_vtable,
      struct iree_hal_amdxdna_driver);
  if (device_id != IREE_HAL_AMDXDNA_DEVICE_ID_DEFAULT) {
    return iree_make_status(IREE_STATUS_NOT_FOUND,
                            "no amdxdna device with id %" PRIu64,
                            (uint64_t)device_id);
  }
  iree_hal_device_report_writer_t writer;
  iree_hal_device_report_writer_initialize(IREE_HAL_DEVICE_REPORT_FORMAT_TEXT,
                                           /*text_indent=*/0, builder, &writer);
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
      &writer, iree_string_view_empty()));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdxdna_driver_append_live_report(driver, &writer));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(&writer));
  return iree_string_builder_append_cstring(builder, "\n");
}

static iree_status_t iree_hal_amdxdna_driver_create_device_by_id(
    iree_hal_driver_t* base_driver, iree_hal_device_id_t device_id,
    iree_host_size_t param_count, const iree_string_pair_t* params,
    const iree_hal_device_create_params_t* create_params,
    iree_allocator_t host_allocator, iree_hal_device_t** out_device) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_TRACE_ZONE_APPEND_VALUE_I64(z0, device_id);
  if (device_id != IREE_HAL_AMDXDNA_DEVICE_ID_DEFAULT) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_NOT_FOUND,
                            "no amdxdna device with id %" PRIu64,
                            (uint64_t)device_id);
  }

  struct iree_hal_amdxdna_driver* driver = IREE_HAL_AMDXDNA_CHECKED_VTABLE_CAST(
      base_driver, iree_hal_amdxdna_driver_vtable,
      struct iree_hal_amdxdna_driver);
  struct iree_hal_amdxdna_device_params options =
      driver->options.default_device_params;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_hal_amdxdna_device_options_parse(&options, param_count, params));

  IREE_TRACE_ZONE_END(z0);
  return iree_hal_amdxdna_device_create(
      driver->identifier, &options, create_params, host_allocator, out_device);
}

static iree_status_t iree_hal_amdxdna_driver_create_device_by_path(
    iree_hal_driver_t* base_driver, iree_string_view_t driver_name,
    iree_string_view_t device_path, iree_host_size_t param_count,
    const iree_string_pair_t* params,
    const iree_hal_device_create_params_t* create_params,
    iree_allocator_t host_allocator, iree_hal_device_t** out_device) {
  IREE_TRACE_ZONE_BEGIN(z0);
  (void)driver_name;

  struct iree_hal_amdxdna_driver* driver = IREE_HAL_AMDXDNA_CHECKED_VTABLE_CAST(
      base_driver, iree_hal_amdxdna_driver_vtable,
      struct iree_hal_amdxdna_driver);
  struct iree_hal_amdxdna_device_params options =
      driver->options.default_device_params;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_hal_amdxdna_device_options_parse(&options, param_count, params));
  if (!iree_string_view_is_empty(device_path)) {
    options.device_path = device_path;
  }

  IREE_TRACE_ZONE_END(z0);
  return iree_hal_amdxdna_device_create(
      driver->identifier, &options, create_params, host_allocator, out_device);
}

static const iree_hal_driver_vtable_t iree_hal_amdxdna_driver_vtable = {
    .destroy = iree_hal_amdxdna_driver_destroy,
    .query_available_devices = iree_hal_amdxdna_driver_query_available_devices,
    .dump_device_info = iree_hal_amdxdna_driver_dump_device_info,
    .create_device_by_id = iree_hal_amdxdna_driver_create_device_by_id,
    .create_device_by_path = iree_hal_amdxdna_driver_create_device_by_path,
    .dump_device_report = iree_hal_amdxdna_driver_dump_device_report,
};
