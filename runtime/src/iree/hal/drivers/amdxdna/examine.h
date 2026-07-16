// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// amdxdna device introspection ("examine").
//
// Two complementary views of the same OS-neutral device-info struct gathered by
// the native layer:
//   * A structured device-report renderer that writes typed fields into a HAL
//     device_report writer (serialized to JSON or text centrally). This is what
//     the generic aggregator/JSON path consumes.
//   * A curated, xrt-smi-style plain-text report (host + HRX + device sections)
//     used for the human-readable `hrx-smi examine` text output.
// Kept separate from the driver vtable so both can be unit tested without
// hardware.

#ifndef IREE_HAL_DRIVERS_AMDXDNA_EXAMINE_H_
#define IREE_HAL_DRIVERS_AMDXDNA_EXAMINE_H_

#include "iree/base/api.h"
#include "iree/hal/device_report.h"
#include "iree/hal/drivers/amdxdna/native.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Appends a typed description of |info| into |writer|'s current object using
// the common/backend group convention. Sections whose |has_*| flags are false
// (unsupported by the driver/firmware) are omitted.
iree_status_t iree_hal_amdxdna_device_info_append_report(
    const iree_hal_amdxdna_native_c_device_info_t* info,
    iree_hal_device_report_writer_t* writer);

//===----------------------------------------------------------------------===//
// Curated text report model
//===----------------------------------------------------------------------===//

// Host system facts mirroring the "System Configuration" block of vendor SMI
// tools. Empty strings / zero mean the value was unavailable.
typedef struct iree_hal_amdxdna_host_info_t {
  char os_name[128];
  char release[128];
  char machine[128];
  char distribution[256];
  char model[256];
  char bios_vendor[128];
  char bios_version[128];
  char processor[256];
  uint32_t cpu_cores;
  uint64_t memory_bytes;
} iree_hal_amdxdna_host_info_t;

typedef struct iree_hal_amdxdna_examine_device_t {
  // Native device node path (e.g. "/dev/accel/accel0").
  char path[IREE_HAL_AMDXDNA_NATIVE_C_MAX_PATH];
  // True when |info| was successfully queried. When false, |status_message|
  // explains why and |info| is zeroed.
  bool info_valid;
  char status_message[128];
  iree_hal_amdxdna_native_c_device_info_t info;
} iree_hal_amdxdna_examine_device_t;

typedef struct iree_hal_amdxdna_examine_report_t {
  iree_hal_amdxdna_host_info_t host;
  // Runtime version string of the reporting stack (empty when unset).
  char runtime_version[32];
  iree_host_size_t device_count;
  iree_hal_amdxdna_examine_device_t
      devices[IREE_HAL_AMDXDNA_NATIVE_C_MAX_DEVICES];
} iree_hal_amdxdna_examine_report_t;

// Enumerates amdxdna devices and fills |out_report->devices| by opening and
// querying each. Host and runtime_version fields are left untouched (the caller
// owns those). A device that enumerates but fails to open/query is still
// recorded with |info_valid| == false and a populated |status_message|.
//
// Returns OK (possibly with zero devices) on supported platforms, or
// IREE_STATUS_UNIMPLEMENTED where native discovery is unavailable.
iree_status_t iree_hal_amdxdna_examine_collect_devices(
    iree_allocator_t host_allocator,
    iree_hal_amdxdna_examine_report_t* out_report);

// Appends the curated, human-readable xrt-smi-style report to |builder|.
iree_status_t iree_hal_amdxdna_examine_format_text(
    const iree_hal_amdxdna_examine_report_t* report,
    iree_string_builder_t* builder);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_DRIVERS_AMDXDNA_EXAMINE_H_
