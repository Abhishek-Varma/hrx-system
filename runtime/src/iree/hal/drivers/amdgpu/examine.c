// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amdgpu/examine.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <dirent.h>
#include <stdlib.h>
#endif  // __linux__

#include "iree/hal/drivers/amdgpu/util/libhsa.h"
#include "iree/hal/drivers/amdgpu/util/topology.h"

static const char* iree_hal_amdgpu_or_na(const char* value) {
  return (value != NULL && value[0] != '\0') ? value : "N/A";
}

// Formats the decoded PCIe location as "domain:bus:device.function".
static void iree_hal_amdgpu_format_bdf(
    const iree_hal_amdgpu_examine_device_info_t* info, char* out,
    size_t out_size) {
  if (!info->has_bdf) {
    snprintf(out, out_size, "N/A");
    return;
  }
  snprintf(out, out_size, "%04x:%02x:%02x.%x", info->pci_domain, info->pci_bus,
           info->pci_device, info->pci_function);
}

//===----------------------------------------------------------------------===//
// Structured report renderer
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_amdgpu_append_common(
    const iree_hal_amdgpu_examine_device_info_t* info,
    iree_hal_device_report_writer_t* writer) {
  char bdf[32];
  iree_hal_amdgpu_format_bdf(info, bdf, sizeof(bdf));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
      writer, IREE_SV(IREE_HAL_DEVICE_REPORT_GROUP_COMMON)));
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_writer_cstring(writer, IREE_SV("vendor"), "AMD"));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
      writer, IREE_SV("category"), "gpu"));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
      writer, IREE_SV("architecture"), iree_hal_amdgpu_or_na(info->arch)));
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_writer_cstring(writer, IREE_SV("bdf"), bdf));
  return iree_hal_device_report_writer_end_object(writer);
}

static iree_status_t iree_hal_amdgpu_append_backend(
    const iree_hal_amdgpu_examine_device_info_t* info,
    iree_hal_device_report_writer_t* writer) {
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
      writer, IREE_SV(IREE_HAL_DEVICE_REPORT_GROUP_BACKEND)));

  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
      writer, IREE_SV("product_name"),
      iree_hal_amdgpu_or_na(info->product_name)));
  IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_cstring(
      writer, IREE_SV("uuid"), iree_hal_amdgpu_or_na(info->uuid)));
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_writer_u64(writer, IREE_SV("node"), info->node));

  if (info->has_compute_units) {
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("compute_units"), info->compute_unit_count));
  }
  if (info->has_simds_per_cu) {
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("simds_per_cu"), info->simds_per_cu));
  }
  if (info->has_wavefront_size) {
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("wavefront_size"), info->wavefront_size));
  }
  if (info->has_max_clock) {
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("max_clock_mhz"), info->max_clock_freq_mhz));
  }
  if (info->has_vram) {
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_u64(
        writer, IREE_SV("vram_bytes"), info->vram_bytes));
  }

  return iree_hal_device_report_writer_end_object(writer);
}

iree_status_t iree_hal_amdgpu_device_info_append_report(
    const iree_hal_amdgpu_examine_device_info_t* info,
    iree_hal_device_report_writer_t* writer) {
  IREE_ASSERT_ARGUMENT(info);
  IREE_ASSERT_ARGUMENT(writer);
  IREE_RETURN_IF_ERROR(iree_hal_amdgpu_append_common(info, writer));
  return iree_hal_amdgpu_append_backend(info, writer);
}

//===----------------------------------------------------------------------===//
// Device collection (HSA queries)
//===----------------------------------------------------------------------===//

// Queries a single 32-bit agent attribute, setting |*out_value| and returning
// true only when the query succeeds. Errors are swallowed so a partially
// capable agent still reports the fields it does support.
static bool iree_hal_amdgpu_query_agent_u32(
    const iree_hal_amdgpu_libhsa_t* libhsa, hsa_agent_t agent,
    hsa_agent_info_t attribute, uint32_t* out_value) {
  uint32_t value = 0;
  iree_status_t status = iree_hsa_agent_get_info(IREE_LIBHSA(libhsa), agent,
                                                 attribute, &value);
  if (!iree_status_is_ok(status)) {
    iree_status_ignore(status);
    return false;
  }
  *out_value = value;
  return true;
}

typedef struct iree_hal_amdgpu_vram_query_t {
  const iree_hal_amdgpu_libhsa_t* libhsa;
  uint64_t vram_bytes;
  bool any;
} iree_hal_amdgpu_vram_query_t;

// Accumulates the size of every coarse-grained global memory pool exposed by an
// agent, which corresponds to its dedicated VRAM.
static hsa_status_t iree_hal_amdgpu_vram_pool_callback(
    hsa_amd_memory_pool_t memory_pool, void* data) {
  iree_hal_amdgpu_vram_query_t* query = (iree_hal_amdgpu_vram_query_t*)data;

  hsa_amd_segment_t segment = (hsa_amd_segment_t)0;
  iree_status_t status = iree_hsa_amd_memory_pool_get_info(
      IREE_LIBHSA(query->libhsa), memory_pool,
      HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &segment);
  if (!iree_status_is_ok(status)) {
    iree_status_ignore(status);
    return HSA_STATUS_SUCCESS;
  }
  if (segment != HSA_AMD_SEGMENT_GLOBAL) return HSA_STATUS_SUCCESS;

  uint32_t flags = 0;
  status = iree_hsa_amd_memory_pool_get_info(
      IREE_LIBHSA(query->libhsa), memory_pool,
      HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS, &flags);
  if (!iree_status_is_ok(status)) {
    iree_status_ignore(status);
    return HSA_STATUS_SUCCESS;
  }
  if (!(flags & HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_COARSE_GRAINED)) {
    return HSA_STATUS_SUCCESS;
  }

  size_t pool_size = 0;
  status = iree_hsa_amd_memory_pool_get_info(IREE_LIBHSA(query->libhsa),
                                             memory_pool,
                                             HSA_AMD_MEMORY_POOL_INFO_SIZE,
                                             &pool_size);
  if (!iree_status_is_ok(status)) {
    iree_status_ignore(status);
    return HSA_STATUS_SUCCESS;
  }
  query->vram_bytes += (uint64_t)pool_size;
  query->any = true;
  return HSA_STATUS_SUCCESS;
}

static void iree_hal_amdgpu_examine_query_agent(
    const iree_hal_amdgpu_libhsa_t* libhsa, hsa_agent_t agent,
    iree_hal_amdgpu_examine_device_info_t* out_info) {
  memset(out_info, 0, sizeof(*out_info));

  char product_name[64] = {0};
  if (iree_status_is_ok(iree_hsa_agent_get_info(
          IREE_LIBHSA(libhsa), agent,
          (hsa_agent_info_t)HSA_AMD_AGENT_INFO_PRODUCT_NAME, product_name))) {
    const iree_string_view_t trimmed =
        iree_string_view_trim(iree_make_cstring_view(product_name));
    snprintf(out_info->product_name, sizeof(out_info->product_name), "%.*s",
             (int)trimmed.size, trimmed.data);
  }

  char uuid[64] = {0};
  if (iree_status_is_ok(iree_hsa_agent_get_info(
          IREE_LIBHSA(libhsa), agent,
          (hsa_agent_info_t)HSA_AMD_AGENT_INFO_UUID, uuid))) {
    snprintf(out_info->uuid, sizeof(out_info->uuid), "%s", uuid);
  }

  char arch[64] = {0};
  if (iree_status_is_ok(iree_hsa_agent_get_info(IREE_LIBHSA(libhsa), agent,
                                                HSA_AGENT_INFO_NAME, arch))) {
    snprintf(out_info->arch, sizeof(out_info->arch), "%s", arch);
  }

  iree_hal_amdgpu_query_agent_u32(libhsa, agent, HSA_AGENT_INFO_NODE,
                                  &out_info->node);

  uint32_t bdf_id = 0;
  if (iree_hal_amdgpu_query_agent_u32(
          libhsa, agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_BDFID, &bdf_id)) {
    out_info->has_bdf = true;
    out_info->pci_bus = (bdf_id >> 8) & 0xFFu;
    out_info->pci_device = (bdf_id >> 3) & 0x1Fu;
    out_info->pci_function = bdf_id & 0x7u;
    uint32_t domain = 0;
    if (iree_hal_amdgpu_query_agent_u32(
            libhsa, agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_DOMAIN,
            &domain)) {
      out_info->pci_domain = domain;
    }
  }

  out_info->has_compute_units = iree_hal_amdgpu_query_agent_u32(
      libhsa, agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT,
      &out_info->compute_unit_count);
  out_info->has_simds_per_cu = iree_hal_amdgpu_query_agent_u32(
      libhsa, agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_NUM_SIMDS_PER_CU,
      &out_info->simds_per_cu);
  out_info->has_wavefront_size = iree_hal_amdgpu_query_agent_u32(
      libhsa, agent, HSA_AGENT_INFO_WAVEFRONT_SIZE, &out_info->wavefront_size);
  out_info->has_max_clock = iree_hal_amdgpu_query_agent_u32(
      libhsa, agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY,
      &out_info->max_clock_freq_mhz);

  iree_hal_amdgpu_vram_query_t vram_query = {
      .libhsa = libhsa, .vram_bytes = 0, .any = false};
  iree_status_t vram_status = iree_hsa_amd_agent_iterate_memory_pools(
      IREE_LIBHSA(libhsa), agent, iree_hal_amdgpu_vram_pool_callback,
      &vram_query);
  if (iree_status_is_ok(vram_status) && vram_query.any) {
    out_info->has_vram = true;
    out_info->vram_bytes = vram_query.vram_bytes;
  } else {
    iree_status_ignore(vram_status);
  }
}

static void iree_hal_amdgpu_examine_query_hsa_version(
    const iree_hal_amdgpu_libhsa_t* libhsa, char* out, size_t out_size) {
  if (out_size == 0) return;
  out[0] = '\0';
  uint16_t major = 0;
  uint16_t minor = 0;
  iree_status_t major_status = iree_hsa_system_get_info(
      IREE_LIBHSA(libhsa), HSA_SYSTEM_INFO_VERSION_MAJOR, &major);
  iree_status_t minor_status = iree_hsa_system_get_info(
      IREE_LIBHSA(libhsa), HSA_SYSTEM_INFO_VERSION_MINOR, &minor);
  if (iree_status_is_ok(major_status) && iree_status_is_ok(minor_status)) {
    snprintf(out, out_size, "%u.%u", major, minor);
  }
  iree_status_ignore(major_status);
  iree_status_ignore(minor_status);
}

//===----------------------------------------------------------------------===//
// System-management (rocm-smi-style) collection via sysfs
//===----------------------------------------------------------------------===//

#if defined(__linux__)

// Reads the first line of |path| into |out|, stripping trailing whitespace.
static bool iree_hal_amdgpu_read_sysfs_line(const char* path, char* out,
                                            size_t out_size) {
  if (out_size == 0) return false;
  out[0] = '\0';
  FILE* file = fopen(path, "re");
  if (!file) return false;
  bool ok = fgets(out, (int)out_size, file) != NULL;
  fclose(file);
  if (!ok) return false;
  size_t len = strlen(out);
  while (len != 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                      out[len - 1] == ' ')) {
    out[--len] = '\0';
  }
  return true;
}

// Reads a sysfs file as an unsigned integer (decimal or 0x-prefixed hex).
static bool iree_hal_amdgpu_read_sysfs_u64(const char* path, uint64_t* out) {
  char line[64];
  if (!iree_hal_amdgpu_read_sysfs_line(path, line, sizeof(line))) return false;
  if (line[0] == '\0') return false;
  *out = (uint64_t)strtoull(line, NULL, 0);
  return true;
}

// Locates the /sys/class/drm/cardN/device directory whose PCI slot matches the
// device's BDF, writing its path into |out_dir|.
static bool iree_hal_amdgpu_find_drm_device_dir(
    const iree_hal_amdgpu_examine_device_info_t* info, char* out_dir,
    size_t out_size) {
  char want[32];
  snprintf(want, sizeof(want), "%04x:%02x:%02x.%x", info->pci_domain,
           info->pci_bus, info->pci_device, info->pci_function);
  DIR* dir = opendir("/sys/class/drm");
  if (!dir) return false;
  bool found = false;
  struct dirent* entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "card", 4) != 0) continue;
    // Skip connector nodes like "card1-DP-1"; we only want the base card.
    if (strchr(entry->d_name, '-') != NULL) continue;
    char uevent[300];
    snprintf(uevent, sizeof(uevent), "/sys/class/drm/%s/device/uevent",
             entry->d_name);
    FILE* file = fopen(uevent, "re");
    if (!file) continue;
    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
      const char* prefix = "PCI_SLOT_NAME=";
      const size_t prefix_len = strlen(prefix);
      if (strncmp(line, prefix, prefix_len) != 0) continue;
      char* value = line + prefix_len;
      char* newline = strpbrk(value, "\r\n");
      if (newline) *newline = '\0';
      if (strcmp(value, want) == 0) {
        snprintf(out_dir, out_size, "/sys/class/drm/%s/device", entry->d_name);
        found = true;
      }
      break;
    }
    fclose(file);
    if (found) break;
  }
  closedir(dir);
  return found;
}

// Reads temperature/frequency/power sensors from the device's hwmon node.
static void iree_hal_amdgpu_collect_hwmon(
    const char* device_dir, iree_hal_amdgpu_examine_smi_t* out_smi) {
  char hwmon_root[320];
  snprintf(hwmon_root, sizeof(hwmon_root), "%s/hwmon", device_dir);
  DIR* dir = opendir(hwmon_root);
  if (!dir) return;
  char hwmon_dir[400] = {0};
  struct dirent* entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "hwmon", 5) != 0) continue;
    snprintf(hwmon_dir, sizeof(hwmon_dir), "%s/%s", hwmon_root, entry->d_name);
    break;
  }
  closedir(dir);
  if (hwmon_dir[0] == '\0') return;

  char path[520];
  char label[32];
  // Temperatures and clocks are label-indexed; scan a small fixed range.
  for (int i = 1; i <= 5; ++i) {
    snprintf(path, sizeof(path), "%s/temp%d_label", hwmon_dir, i);
    if (iree_hal_amdgpu_read_sysfs_line(path, label, sizeof(label)) &&
        strcmp(label, "junction") == 0) {
      snprintf(path, sizeof(path), "%s/temp%d_input", hwmon_dir, i);
      uint64_t millidegrees = 0;
      if (iree_hal_amdgpu_read_sysfs_u64(path, &millidegrees)) {
        out_smi->has_temp = true;
        out_smi->temp_junction_c = (double)millidegrees / 1000.0;
      }
    }
    snprintf(path, sizeof(path), "%s/freq%d_label", hwmon_dir, i);
    if (iree_hal_amdgpu_read_sysfs_line(path, label, sizeof(label))) {
      snprintf(path, sizeof(path), "%s/freq%d_input", hwmon_dir, i);
      uint64_t hz = 0;
      if (iree_hal_amdgpu_read_sysfs_u64(path, &hz)) {
        if (strcmp(label, "sclk") == 0) {
          out_smi->has_sclk = true;
          out_smi->sclk_mhz = (uint32_t)(hz / 1000000ull);
        } else if (strcmp(label, "mclk") == 0) {
          out_smi->has_mclk = true;
          out_smi->mclk_mhz = (uint32_t)(hz / 1000000ull);
        }
      }
    }
  }

  uint64_t microwatts = 0;
  snprintf(path, sizeof(path), "%s/power1_input", hwmon_dir);
  if (iree_hal_amdgpu_read_sysfs_u64(path, &microwatts)) {
    out_smi->has_power = true;
    out_smi->power_w = (double)microwatts / 1000000.0;
  }
  snprintf(path, sizeof(path), "%s/power1_cap", hwmon_dir);
  if (iree_hal_amdgpu_read_sysfs_u64(path, &microwatts)) {
    out_smi->has_pwrcap = true;
    out_smi->pwrcap_w = (double)microwatts / 1000000.0;
  }
  uint64_t pwm = 0;
  snprintf(path, sizeof(path), "%s/pwm1", hwmon_dir);
  if (iree_hal_amdgpu_read_sysfs_u64(path, &pwm)) {
    out_smi->has_fan = true;
    out_smi->fan_pct = (uint32_t)((pwm * 100ull + 127ull) / 255ull);
  }
}

// Reads the KFD gpu_id (rocm-smi's GUID) for the given topology node.
static bool iree_hal_amdgpu_read_kfd_gpu_id(uint32_t node, uint32_t* out_guid) {
  char path[128];
  snprintf(path, sizeof(path),
           "/sys/devices/virtual/kfd/kfd/topology/nodes/%u/gpu_id", node);
  uint64_t value = 0;
  if (!iree_hal_amdgpu_read_sysfs_u64(path, &value) || value == 0) return false;
  *out_guid = (uint32_t)value;
  return true;
}

static void iree_hal_amdgpu_examine_collect_smi(
    const iree_hal_amdgpu_examine_device_info_t* info,
    iree_hal_amdgpu_examine_smi_t* out_smi) {
  memset(out_smi, 0, sizeof(*out_smi));

  if (iree_hal_amdgpu_read_kfd_gpu_id(info->node, &out_smi->guid)) {
    out_smi->has_guid = true;
  }
  if (!info->has_bdf) return;

  char device_dir[256];
  if (!iree_hal_amdgpu_find_drm_device_dir(info, device_dir,
                                           sizeof(device_dir))) {
    return;
  }

  char path[300];
  uint64_t value = 0;
  snprintf(path, sizeof(path), "%s/device", device_dir);
  if (iree_hal_amdgpu_read_sysfs_u64(path, &value)) {
    out_smi->has_did = true;
    out_smi->did = (uint32_t)value;
  }
  snprintf(path, sizeof(path), "%s/gpu_busy_percent", device_dir);
  if (iree_hal_amdgpu_read_sysfs_u64(path, &value)) {
    out_smi->has_gpu_pct = true;
    out_smi->gpu_pct = (uint32_t)value;
  }
  uint64_t vram_used = 0;
  uint64_t vram_total = 0;
  snprintf(path, sizeof(path), "%s/mem_info_vram_used", device_dir);
  bool have_used = iree_hal_amdgpu_read_sysfs_u64(path, &vram_used);
  snprintf(path, sizeof(path), "%s/mem_info_vram_total", device_dir);
  bool have_total = iree_hal_amdgpu_read_sysfs_u64(path, &vram_total);
  if (have_used && have_total && vram_total != 0) {
    out_smi->has_vram_pct = true;
    out_smi->vram_pct = (uint32_t)((vram_used * 100ull) / vram_total);
  }
  snprintf(path, sizeof(path), "%s/power_dpm_force_performance_level",
           device_dir);
  iree_hal_amdgpu_read_sysfs_line(path, out_smi->perf_level,
                                  sizeof(out_smi->perf_level));
  snprintf(path, sizeof(path), "%s/current_memory_partition", device_dir);
  iree_hal_amdgpu_read_sysfs_line(path, out_smi->memory_partition,
                                  sizeof(out_smi->memory_partition));
  snprintf(path, sizeof(path), "%s/current_compute_partition", device_dir);
  iree_hal_amdgpu_read_sysfs_line(path, out_smi->compute_partition,
                                  sizeof(out_smi->compute_partition));

  iree_hal_amdgpu_collect_hwmon(device_dir, out_smi);
}

#else

static void iree_hal_amdgpu_examine_collect_smi(
    const iree_hal_amdgpu_examine_device_info_t* info,
    iree_hal_amdgpu_examine_smi_t* out_smi) {
  (void)info;
  memset(out_smi, 0, sizeof(*out_smi));
}

#endif  // __linux__

iree_status_t iree_hal_amdgpu_examine_collect_devices(
    iree_allocator_t host_allocator,
    iree_hal_amdgpu_examine_report_t* out_report) {
  IREE_ASSERT_ARGUMENT(out_report);
  out_report->device_count = 0;

  iree_hal_amdgpu_libhsa_t libhsa;
  IREE_RETURN_IF_ERROR(iree_hal_amdgpu_libhsa_initialize(
      IREE_HAL_AMDGPU_LIBHSA_FLAG_NONE, iree_string_view_list_empty(),
      host_allocator, &libhsa));

  iree_hal_amdgpu_examine_query_hsa_version(&libhsa,
                                            out_report->hsa_runtime_version,
                                            sizeof(out_report->hsa_runtime_version));

  iree_hal_amdgpu_topology_t topology;
  iree_status_t status =
      iree_hal_amdgpu_topology_initialize_with_defaults(&libhsa, &topology);
  if (iree_status_is_ok(status)) {
    iree_host_size_t count = topology.gpu_agent_count;
    if (count > IREE_HAL_AMDGPU_EXAMINE_MAX_DEVICES) {
      count = IREE_HAL_AMDGPU_EXAMINE_MAX_DEVICES;
    }
    out_report->device_count = count;
    for (iree_host_size_t i = 0; i < count; ++i) {
      iree_hal_amdgpu_examine_device_t* entry = &out_report->devices[i];
      memset(entry, 0, sizeof(*entry));
      iree_hal_amdgpu_examine_query_agent(&libhsa, topology.gpu_agents[i],
                                          &entry->info);
      entry->info_valid = true;
      iree_hal_amdgpu_examine_collect_smi(&entry->info, &entry->smi);
    }
    iree_hal_amdgpu_topology_deinitialize(&topology);
  }

  iree_hal_amdgpu_libhsa_deinitialize(&libhsa);
  return status;
}

//===----------------------------------------------------------------------===//
// Curated text formatting
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_amdgpu_append_row(iree_string_builder_t* builder,
                                                const char* key,
                                                const char* value) {
  return iree_string_builder_append_format(builder, "  %-20s : %s\n", key,
                                           iree_hal_amdgpu_or_na(value));
}

static iree_status_t iree_hal_amdgpu_examine_format_host_text(
    const iree_hal_amdgpu_host_info_t* host, iree_string_builder_t* builder) {
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_cstring(builder, "System Configuration\n"));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdgpu_append_row(builder, "OS Name", host->os_name));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdgpu_append_row(builder, "Release", host->release));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdgpu_append_row(builder, "Machine", host->machine));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %" PRIu32 "\n", "CPU Cores", host->cpu_cores));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %" PRIu64 " MB\n", "Memory",
      host->memory_bytes / (1024u * 1024u)));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdgpu_append_row(builder, "Distribution", host->distribution));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdgpu_append_row(builder, "Model", host->model));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdgpu_append_row(builder, "BIOS Vendor", host->bios_vendor));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdgpu_append_row(builder, "BIOS Version", host->bios_version));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdgpu_append_row(builder, "Processor", host->processor));
  return iree_ok_status();
}

// Shared column format for the rocm-smi-style concise table. The header,
// sub-header, and data rows all use the same width specifiers so the columns
// stay aligned. The temperature column is passed a caller-computed width to
// compensate for the multi-byte UTF-8 degree sign in data rows.
#define IREE_HAL_AMDGPU_SMI_ROW_FORMAT                                        \
  "%-7s%-5s %-18s %-*s %-9s %-19s %-9s %-9s %-5s %-5s %-9s %-6s %s\n"

static iree_status_t iree_hal_amdgpu_examine_format_smi_header(
    iree_string_builder_t* builder) {
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, IREE_HAL_AMDGPU_SMI_ROW_FORMAT, "Device", "Node", "IDs", 10,
      "Temp", "Power", "Partitions", "SCLK", "MCLK", "Fan", "Perf", "PwrCap",
      "VRAM%", "GPU%"));
  return iree_string_builder_append_format(
      builder, IREE_HAL_AMDGPU_SMI_ROW_FORMAT, "", "", "(DID, GUID)", 10,
      "(Junction)", "(Socket)", "(Mem, Compute, ID)", "", "", "", "", "", "",
      "");
}

// Formats one concise-table cell into |out|, substituting "N/A" when the
// corresponding value is unavailable.
static const char* iree_hal_amdgpu_smi_cell(char* out, size_t out_size,
                                            bool has_value, const char* format,
                                            ...) {
  if (!has_value) {
    snprintf(out, out_size, "N/A");
    return out;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(out, out_size, format, args);
  va_end(args);
  return out;
}

static iree_status_t iree_hal_amdgpu_examine_format_smi_row(
    iree_host_size_t index, const iree_hal_amdgpu_examine_device_t* device,
    iree_string_builder_t* builder) {
  const iree_hal_amdgpu_examine_device_info_t* info = &device->info;
  const iree_hal_amdgpu_examine_smi_t* smi = &device->smi;
  if (!device->info_valid) {
    return iree_string_builder_append_format(
        builder, "%-7" PRIhsz " (unable to query: %s)\n", index,
        iree_hal_amdgpu_or_na(device->status_message));
  }

  char device_text[8], node_text[8];
  char ids[24], temp[16], power[12], parts[28], sclk[12], mclk[12], fan[8];
  char pwrcap[12], vram[8], gpu[8];
  char guid_text[16];
  snprintf(device_text, sizeof(device_text), "%" PRIhsz, index);
  snprintf(node_text, sizeof(node_text), "%" PRIu32, info->node);
  snprintf(guid_text, sizeof(guid_text), "%" PRIu32, smi->guid);
  iree_hal_amdgpu_smi_cell(ids, sizeof(ids), smi->has_did, "0x%04" PRIx32
                           ", %s", smi->did,
                           smi->has_guid ? guid_text : "N/A");
  iree_hal_amdgpu_smi_cell(temp, sizeof(temp), smi->has_temp,
                           "%.1f\xC2\xB0" "C", smi->temp_junction_c);
  iree_hal_amdgpu_smi_cell(power, sizeof(power), smi->has_power, "%.1fW",
                           smi->power_w);
  iree_hal_amdgpu_smi_cell(
      parts, sizeof(parts), smi->memory_partition[0] != '\0',
      "%s, %s, %" PRIu32, smi->memory_partition,
      smi->compute_partition[0] != '\0' ? smi->compute_partition : "N/A",
      smi->partition_id);
  iree_hal_amdgpu_smi_cell(sclk, sizeof(sclk), smi->has_sclk, "%" PRIu32 "Mhz",
                           smi->sclk_mhz);
  iree_hal_amdgpu_smi_cell(mclk, sizeof(mclk), smi->has_mclk, "%" PRIu32 "Mhz",
                           smi->mclk_mhz);
  iree_hal_amdgpu_smi_cell(fan, sizeof(fan), smi->has_fan, "%" PRIu32 "%%",
                           smi->fan_pct);
  iree_hal_amdgpu_smi_cell(pwrcap, sizeof(pwrcap), smi->has_pwrcap, "%.1fW",
                           smi->pwrcap_w);
  iree_hal_amdgpu_smi_cell(vram, sizeof(vram), smi->has_vram_pct,
                           "%" PRIu32 "%%", smi->vram_pct);
  iree_hal_amdgpu_smi_cell(gpu, sizeof(gpu), smi->has_gpu_pct, "%" PRIu32 "%%",
                           smi->gpu_pct);

  // Data rows carry a 2-byte UTF-8 degree sign in the temperature cell, so
  // widen that column by one byte to keep the visible width aligned.
  int temp_pad = 10 + (smi->has_temp ? 1 : 0);
  return iree_string_builder_append_format(
      builder, IREE_HAL_AMDGPU_SMI_ROW_FORMAT, device_text, node_text, ids,
      temp_pad, temp, power, parts, sclk, mclk, fan,
      smi->perf_level[0] != '\0' ? smi->perf_level : "N/A", pwrcap, vram, gpu);
}

iree_status_t iree_hal_amdgpu_examine_format_text(
    const iree_hal_amdgpu_examine_report_t* report,
    iree_string_builder_t* builder) {
  IREE_ASSERT_ARGUMENT(report);
  IREE_ASSERT_ARGUMENT(builder);

  IREE_RETURN_IF_ERROR(
      iree_hal_amdgpu_examine_format_host_text(&report->host, builder));

  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "\nHRX\n"));
  IREE_RETURN_IF_ERROR(
      iree_hal_amdgpu_append_row(builder, "Version", report->runtime_version));
  IREE_RETURN_IF_ERROR(iree_hal_amdgpu_append_row(builder, "HSA Runtime",
                                                  report->hsa_runtime_version));

  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(
      builder,
      "\n===================== HRX System Management Interface "
      "=====================\n"
      "============================== Concise Info "
      "==============================\n"));
  if (report->device_count == 0) {
    return iree_string_builder_append_cstring(builder, "  0 devices found\n");
  }
  IREE_RETURN_IF_ERROR(iree_hal_amdgpu_examine_format_smi_header(builder));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(
      builder,
      "==========================================================="
      "=================\n"));
  for (iree_host_size_t i = 0; i < report->device_count; ++i) {
    IREE_RETURN_IF_ERROR(iree_hal_amdgpu_examine_format_smi_row(
        i, &report->devices[i], builder));
  }
  return iree_string_builder_append_cstring(
      builder,
      "==========================================================="
      "=================\n"
      "======================== End of HRX SMI Log "
      "==============================\n");
}
