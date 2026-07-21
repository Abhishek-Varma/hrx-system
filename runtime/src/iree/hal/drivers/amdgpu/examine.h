// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// amdgpu device introspection ("examine").
//
// Two complementary views of the same OS-neutral device-info struct gathered by
// querying HSA agents:
//   * A structured device-report renderer that writes typed fields into a HAL
//     device_report writer (serialized to JSON or text centrally). This is what
//     the generic aggregator/JSON path and the driver dump_device_report hook
//     consume.
//   * A curated, rocm-smi-style plain-text report (host + HRX + device sections)
//     used for the human-readable `hrx-smi examine` text output.
// The renderer and text formatter operate on a plain struct with no HSA types so
// both can be unit tested without hardware; the (HSA-dependent) collection code
// lives in examine.c.

#ifndef IREE_HAL_DRIVERS_AMDGPU_EXAMINE_H_
#define IREE_HAL_DRIVERS_AMDGPU_EXAMINE_H_

#include "iree/base/api.h"
#include "iree/hal/device_report.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Maximum number of physical GPU devices captured by a single examine pass.
// Matches the topology GPU-agent bitfield capacity.
#define IREE_HAL_AMDGPU_EXAMINE_MAX_DEVICES 64

//===----------------------------------------------------------------------===//
// Device info model
//===----------------------------------------------------------------------===//

// OS-neutral snapshot of a single GPU agent's static capabilities as queried
// from HSA. Fields guarded by |has_*| flags are omitted from reports when the
// underlying query was unavailable. Empty strings mean "unavailable".
typedef struct iree_hal_amdgpu_examine_device_info_t {
  // Marketing/product name (HSA_AMD_AGENT_INFO_PRODUCT_NAME), e.g.
  // "AMD Instinct MI300X".
  char product_name[64];
  // Agent UUID (HSA_AMD_AGENT_INFO_UUID), e.g. "GPU-0e12865a3bf5b7ab".
  char uuid[64];
  // gfx architecture / ISA processor name (HSA_AGENT_INFO_NAME), e.g. "gfx942".
  char arch[32];
  // HSA topology node index (HSA_AGENT_INFO_NODE).
  uint32_t node;

  // PCIe bus/device/function, decoded from HSA_AMD_AGENT_INFO_DOMAIN and
  // HSA_AMD_AGENT_INFO_BDFID.
  bool has_bdf;
  uint32_t pci_domain;
  uint32_t pci_bus;
  uint32_t pci_device;
  uint32_t pci_function;

  bool has_compute_units;
  uint32_t compute_unit_count;  // HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT
  bool has_simds_per_cu;
  uint32_t simds_per_cu;  // HSA_AMD_AGENT_INFO_NUM_SIMDS_PER_CU
  bool has_wavefront_size;
  uint32_t wavefront_size;  // HSA_AGENT_INFO_WAVEFRONT_SIZE
  bool has_max_clock;
  uint32_t max_clock_freq_mhz;  // HSA_AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY

  // Total coarse-grained global (VRAM) memory pool capacity in bytes.
  bool has_vram;
  uint64_t vram_bytes;
} iree_hal_amdgpu_examine_device_info_t;

// Appends a typed description of |info| into |writer|'s current object using the
// common/backend group convention. Sections whose |has_*| flags are false are
// omitted.
iree_status_t iree_hal_amdgpu_device_info_append_report(
    const iree_hal_amdgpu_examine_device_info_t* info,
    iree_hal_device_report_writer_t* writer);

//===----------------------------------------------------------------------===//
// Curated text report model
//===----------------------------------------------------------------------===//

// Host system facts mirroring the "System Configuration" block of vendor SMI
// tools. Empty strings / zero mean the value was unavailable.
typedef struct iree_hal_amdgpu_host_info_t {
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
} iree_hal_amdgpu_host_info_t;

// Live system-management fields collected from the amdgpu kernel driver via
// sysfs (Linux only). These mirror the columns of `rocm-smi`'s concise output;
// values that could not be read leave their |has_*| flag false.
typedef struct iree_hal_amdgpu_examine_smi_t {
  bool has_did;
  uint32_t did;  // PCI device id, e.g. 0x74a1
  bool has_guid;
  uint32_t guid;  // KFD gpu_id (rocm-smi GUID), e.g. 28851
  bool has_temp;
  double temp_junction_c;
  bool has_power;
  double power_w;  // current socket/package power draw
  bool has_pwrcap;
  double pwrcap_w;
  bool has_sclk;
  uint32_t sclk_mhz;
  bool has_mclk;
  uint32_t mclk_mhz;
  bool has_fan;
  uint32_t fan_pct;
  bool has_vram_pct;
  uint32_t vram_pct;
  bool has_gpu_pct;
  uint32_t gpu_pct;
  // Performance level (e.g. "auto"); empty when unavailable.
  char perf_level[16];
  // Memory / compute partition modes (e.g. "NPS1" / "SPX"); empty if absent.
  char memory_partition[16];
  char compute_partition[16];
  uint32_t partition_id;
} iree_hal_amdgpu_examine_smi_t;

typedef struct iree_hal_amdgpu_examine_device_t {
  // True when |info| was successfully queried. When false, |status_message|
  // explains why and |info| is zeroed.
  bool info_valid;
  char status_message[128];
  iree_hal_amdgpu_examine_device_info_t info;
  // Live sysfs-sourced fields for the rocm-smi-style concise table.
  iree_hal_amdgpu_examine_smi_t smi;
} iree_hal_amdgpu_examine_device_t;

typedef struct iree_hal_amdgpu_examine_report_t {
  iree_hal_amdgpu_host_info_t host;
  // Runtime version string of the reporting stack (empty when unset).
  char runtime_version[32];
  // HSA runtime version string "major.minor" (empty when unavailable).
  char hsa_runtime_version[32];
  iree_host_size_t device_count;
  iree_hal_amdgpu_examine_device_t
      devices[IREE_HAL_AMDGPU_EXAMINE_MAX_DEVICES];
} iree_hal_amdgpu_examine_report_t;

// Enumerates visible GPU agents and fills |out_report->devices| by querying each
// through a transiently loaded HSA runtime. Host and runtime_version fields are
// left untouched (the caller owns those). A device that enumerates but fails to
// query is still recorded with |info_valid| == false and a populated
// |status_message|.
//
// Returns OK (possibly with zero devices) when HSA is available, or
// IREE_STATUS_UNIMPLEMENTED / a load error where HSA cannot be initialized.
iree_status_t iree_hal_amdgpu_examine_collect_devices(
    iree_allocator_t host_allocator,
    iree_hal_amdgpu_examine_report_t* out_report);

// Appends the curated, human-readable rocm-smi-style report to |builder|.
iree_status_t iree_hal_amdgpu_examine_format_text(
    const iree_hal_amdgpu_examine_report_t* report,
    iree_string_builder_t* builder);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_DRIVERS_AMDGPU_EXAMINE_H_
