// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// hrx-smi: a system-management interface for HRX, modeled after `xrt-smi`. The
// `examine` subcommand reports host configuration and device information.
//
// Text output is a curated, human-readable per-backend report (rocm-smi-style
// for amdgpu GPUs, xrt-smi-style for amdxdna NPUs) selected from whichever
// accelerator driver is linked in; when none is linked it falls back to a
// generic report derived from the HAL device_report writer. JSON output
// (--format=json) is backend-neutral: it walks the HAL driver registry and
// emits each linked driver's structured device report, so it round-trips as
// typed data for tooling.
//
// The driver-specific curated paths are compiled in only when the corresponding
// driver is enabled (HRX_SMI_HAVE_AMDGPU / HRX_SMI_HAVE_AMDXDNA), so hrx-smi
// builds and runs even with no accelerator driver present.

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "iree/hal/drivers/init.h"

#if defined(HRX_SMI_HAVE_AMDGPU)
#include "iree/hal/drivers/amdgpu/examine.h"
#endif  // HRX_SMI_HAVE_AMDGPU

#if defined(HRX_SMI_HAVE_AMDXDNA)
#include "iree/hal/drivers/amdxdna/examine.h"
#endif  // HRX_SMI_HAVE_AMDXDNA

#if defined(IREE_PLATFORM_LINUX) || defined(__linux__)
#include <sys/utsname.h>
#include <unistd.h>
#define IREE_HRX_SMI_HAVE_HOST_INFO 1
#endif  // __linux__

//===----------------------------------------------------------------------===//
// Host information collection
//===----------------------------------------------------------------------===//

typedef struct iree_hrx_smi_host_info_t {
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
} iree_hrx_smi_host_info_t;

#if defined(IREE_HRX_SMI_HAVE_HOST_INFO)

// Reads the first line of |path| into |out| (NUL-terminated, newline stripped).
// Leaves |out| as an empty string when the file cannot be read.
static void iree_hrx_smi_read_first_line(const char* path, char* out,
                                         size_t out_size) {
  if (out_size == 0) return;
  out[0] = '\0';
  FILE* file = fopen(path, "re");
  if (file == NULL) return;
  if (fgets(out, (int)out_size, file) != NULL) {
    const size_t len = strlen(out);
    if (len != 0 && out[len - 1] == '\n') out[len - 1] = '\0';
  }
  fclose(file);
}

// Extracts a KEY="value" (or KEY=value) entry from a shell-style file such as
// /etc/os-release or /proc/cpuinfo (":"-separated for the latter).
static void iree_hrx_smi_read_key(const char* path, const char* key,
                                  char separator, char* out, size_t out_size) {
  if (out_size == 0) return;
  out[0] = '\0';
  FILE* file = fopen(path, "re");
  if (file == NULL) return;
  const size_t key_len = strlen(key);
  char line[512];
  while (fgets(line, sizeof(line), file) != NULL) {
    if (strncmp(line, key, key_len) != 0) continue;
    const char* cursor = line + key_len;
    // Skip optional whitespace then the separator then whitespace/quotes.
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor != separator) continue;
    ++cursor;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\"') ++cursor;
    size_t out_len = 0;
    while (*cursor != '\0' && *cursor != '\n' && *cursor != '\"' &&
           out_len + 1 < out_size) {
      out[out_len++] = *cursor++;
    }
    out[out_len] = '\0';
    break;
  }
  fclose(file);
}

static void iree_hrx_smi_collect_host_info(iree_hrx_smi_host_info_t* out_host) {
  memset(out_host, 0, sizeof(*out_host));

  struct utsname uts;
  if (uname(&uts) == 0) {
    snprintf(out_host->os_name, sizeof(out_host->os_name), "%s", uts.sysname);
    snprintf(out_host->release, sizeof(out_host->release), "%s", uts.release);
    snprintf(out_host->machine, sizeof(out_host->machine), "%s", uts.machine);
  }

  const long cores = sysconf(_SC_NPROCESSORS_ONLN);
  if (cores > 0) out_host->cpu_cores = (uint32_t)cores;
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long page_size = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && page_size > 0) {
    out_host->memory_bytes = (uint64_t)pages * (uint64_t)page_size;
  }

  iree_hrx_smi_read_key("/etc/os-release", "PRETTY_NAME", '=',
                        out_host->distribution, sizeof(out_host->distribution));
  iree_hrx_smi_read_first_line("/sys/class/dmi/id/product_name",
                               out_host->model, sizeof(out_host->model));
  iree_hrx_smi_read_first_line("/sys/class/dmi/id/bios_vendor",
                               out_host->bios_vendor,
                               sizeof(out_host->bios_vendor));
  iree_hrx_smi_read_first_line("/sys/class/dmi/id/bios_version",
                               out_host->bios_version,
                               sizeof(out_host->bios_version));
  iree_hrx_smi_read_key("/proc/cpuinfo", "model name", ':', out_host->processor,
                        sizeof(out_host->processor));
}

#else

static void iree_hrx_smi_collect_host_info(iree_hrx_smi_host_info_t* out_host) {
  memset(out_host, 0, sizeof(*out_host));
}

#endif  // IREE_HRX_SMI_HAVE_HOST_INFO

//===----------------------------------------------------------------------===//
// Formatting helpers
//===----------------------------------------------------------------------===//

static const char* iree_hrx_smi_or_na(const char* value) {
  return (value != NULL && value[0] != '\0') ? value : "N/A";
}

// Appends |value| as a JSON string body (without surrounding quotes), escaping
// control characters and the characters the JSON grammar requires.
static iree_status_t iree_hrx_smi_append_json_escaped(
    iree_string_builder_t* builder, iree_string_view_t value) {
  for (iree_host_size_t i = 0; i < value.size; ++i) {
    const unsigned char c = (unsigned char)value.data[i];
    switch (c) {
      case '\"': {
        IREE_RETURN_IF_ERROR(
            iree_string_builder_append_cstring(builder, "\\\""));
        break;
      }
      case '\\': {
        IREE_RETURN_IF_ERROR(
            iree_string_builder_append_cstring(builder, "\\\\"));
        break;
      }
      case '\n': {
        IREE_RETURN_IF_ERROR(
            iree_string_builder_append_cstring(builder, "\\n"));
        break;
      }
      case '\r': {
        IREE_RETURN_IF_ERROR(
            iree_string_builder_append_cstring(builder, "\\r"));
        break;
      }
      case '\t': {
        IREE_RETURN_IF_ERROR(
            iree_string_builder_append_cstring(builder, "\\t"));
        break;
      }
      default:
        if (c < 0x20) {
          IREE_RETURN_IF_ERROR(
              iree_string_builder_append_format(builder, "\\u%04x", c));
        } else {
          IREE_RETURN_IF_ERROR(
              iree_string_builder_append_format(builder, "%c", c));
        }
        break;
    }
  }
  return iree_ok_status();
}

static iree_status_t iree_hrx_smi_append_json_string_field(
    iree_string_builder_t* builder, const char* key, const char* value) {
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_format(builder, "\"%s\": \"", key));
  IREE_RETURN_IF_ERROR(iree_hrx_smi_append_json_escaped(
      builder, iree_make_cstring_view(iree_hrx_smi_or_na(value))));
  return iree_string_builder_append_cstring(builder, "\"");
}

//===----------------------------------------------------------------------===//
// Host report
//===----------------------------------------------------------------------===//

static iree_status_t iree_hrx_smi_format_host_json(
    const iree_hrx_smi_host_info_t* host, iree_string_builder_t* builder) {
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_cstring(builder, "\"host\": {"));
  IREE_RETURN_IF_ERROR(
      iree_hrx_smi_append_json_string_field(builder, "model", host->model));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, ", "));
  IREE_RETURN_IF_ERROR(
      iree_hrx_smi_append_json_string_field(builder, "os", host->distribution));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, ", "));
  IREE_RETURN_IF_ERROR(iree_hrx_smi_append_json_string_field(
      builder, "kernel_name", host->os_name));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, ", "));
  IREE_RETURN_IF_ERROR(iree_hrx_smi_append_json_string_field(
      builder, "kernel_release", host->release));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, ", "));
  IREE_RETURN_IF_ERROR(
      iree_hrx_smi_append_json_string_field(builder, "machine", host->machine));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, ", "));
  IREE_RETURN_IF_ERROR(iree_hrx_smi_append_json_string_field(
      builder, "processor", host->processor));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, ", \"cpu_cores\": %u", host->cpu_cores));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, ", \"memory_bytes\": %" PRIu64, host->memory_bytes));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, ", "));
  IREE_RETURN_IF_ERROR(iree_hrx_smi_append_json_string_field(
      builder, "bios_vendor", host->bios_vendor));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, ", "));
  IREE_RETURN_IF_ERROR(iree_hrx_smi_append_json_string_field(
      builder, "bios_version", host->bios_version));
  return iree_string_builder_append_cstring(builder, "}");
}

//===----------------------------------------------------------------------===//
// Driver/device aggregation
//===----------------------------------------------------------------------===//

#if defined(HRX_SMI_HAVE_AMDGPU)
// Builds the curated, rocm-smi-style text report for amdgpu GPUs.
static iree_status_t iree_hrx_smi_format_text_amdgpu(
    const iree_hrx_smi_host_info_t* host, iree_allocator_t host_allocator,
    iree_string_builder_t* builder) {
  iree_hal_amdgpu_examine_report_t report;
  memset(&report, 0, sizeof(report));
  // Map the collected host facts into the amdgpu report's host block.
  snprintf(report.host.os_name, sizeof(report.host.os_name), "%s",
           host->os_name);
  snprintf(report.host.release, sizeof(report.host.release), "%s",
           host->release);
  snprintf(report.host.machine, sizeof(report.host.machine), "%s",
           host->machine);
  snprintf(report.host.distribution, sizeof(report.host.distribution), "%s",
           host->distribution);
  snprintf(report.host.model, sizeof(report.host.model), "%s", host->model);
  snprintf(report.host.bios_vendor, sizeof(report.host.bios_vendor), "%s",
           host->bios_vendor);
  snprintf(report.host.bios_version, sizeof(report.host.bios_version), "%s",
           host->bios_version);
  snprintf(report.host.processor, sizeof(report.host.processor), "%s",
           host->processor);
  report.host.cpu_cores = host->cpu_cores;
  report.host.memory_bytes = host->memory_bytes;

  iree_status_t status =
      iree_hal_amdgpu_examine_collect_devices(host_allocator, &report);
  if (!iree_status_is_ok(status)) {
    // A host without a usable HSA runtime still yields a host-only report.
    iree_status_ignore(status);
    report.device_count = 0;
  }
  return iree_hal_amdgpu_examine_format_text(&report, builder);
}
#endif  // HRX_SMI_HAVE_AMDGPU

#if defined(HRX_SMI_HAVE_AMDXDNA)
// Builds the curated, xrt-smi-style text report for amdxdna NPUs.
static iree_status_t iree_hrx_smi_format_text_amdxdna(
    const iree_hrx_smi_host_info_t* host, iree_allocator_t host_allocator,
    iree_string_builder_t* builder) {
  iree_hal_amdxdna_examine_report_t report;
  memset(&report, 0, sizeof(report));
  // Map the collected host facts into the amdxdna report's host block.
  snprintf(report.host.os_name, sizeof(report.host.os_name), "%s",
           host->os_name);
  snprintf(report.host.release, sizeof(report.host.release), "%s",
           host->release);
  snprintf(report.host.machine, sizeof(report.host.machine), "%s",
           host->machine);
  snprintf(report.host.distribution, sizeof(report.host.distribution), "%s",
           host->distribution);
  snprintf(report.host.model, sizeof(report.host.model), "%s", host->model);
  snprintf(report.host.bios_vendor, sizeof(report.host.bios_vendor), "%s",
           host->bios_vendor);
  snprintf(report.host.bios_version, sizeof(report.host.bios_version), "%s",
           host->bios_version);
  snprintf(report.host.processor, sizeof(report.host.processor), "%s",
           host->processor);
  report.host.cpu_cores = host->cpu_cores;
  report.host.memory_bytes = host->memory_bytes;

  iree_status_t status =
      iree_hal_amdxdna_examine_collect_devices(host_allocator, &report);
  if (!iree_status_is_ok(status)) {
    // A backend/platform without native discovery still yields a host report.
    if (iree_status_code(status) == IREE_STATUS_UNIMPLEMENTED) {
      iree_status_ignore(status);
      report.device_count = 0;
    } else {
      return status;
    }
  }
  return iree_hal_amdxdna_examine_format_text(&report, builder);
}
#endif  // HRX_SMI_HAVE_AMDXDNA

// Generic host-only text report used when no curated backend is linked in.
// Device detail is still available via the JSON output's registry walk.
static iree_status_t iree_hrx_smi_format_text_generic(
    const iree_hrx_smi_host_info_t* host, iree_string_builder_t* builder) {
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_cstring(builder, "System Configuration\n"));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %s\n", "OS Name", iree_hrx_smi_or_na(host->os_name)));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %s\n", "Release",
      iree_hrx_smi_or_na(host->release)));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %s\n", "Machine",
      iree_hrx_smi_or_na(host->machine)));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %" PRIu32 "\n", "CPU Cores", host->cpu_cores));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %" PRIu64 " MB\n", "Memory",
      host->memory_bytes / (1024u * 1024u)));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %s\n", "Distribution",
      iree_hrx_smi_or_na(host->distribution)));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %s\n", "Model", iree_hrx_smi_or_na(host->model)));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "  %-20s : %s\n", "Processor",
      iree_hrx_smi_or_na(host->processor)));
  return iree_string_builder_append_cstring(
      builder,
      "\nNo curated device backend linked in; use --format=json for device "
      "details.\n");
}

// Selects the curated report for whichever accelerator backend is linked in,
// preferring a GPU (amdgpu) view, then an NPU (amdxdna) view, then a generic
// host-only view. Only the JSON path walks the full registry.
static iree_status_t iree_hrx_smi_format_text(
    const iree_hrx_smi_host_info_t* host, iree_allocator_t host_allocator,
    iree_string_builder_t* builder) {
  (void)host_allocator;
#if defined(HRX_SMI_HAVE_AMDGPU)
  return iree_hrx_smi_format_text_amdgpu(host, host_allocator, builder);
#elif defined(HRX_SMI_HAVE_AMDXDNA)
  return iree_hrx_smi_format_text_amdxdna(host, host_allocator, builder);
#else
  return iree_hrx_smi_format_text_generic(host, builder);
#endif  // HRX_SMI_HAVE_AMDGPU
}

static iree_status_t iree_hrx_smi_append_device_json(
    iree_hal_driver_t* driver, const iree_hal_device_info_t* device_info,
    iree_allocator_t host_allocator, iree_string_builder_t* builder) {
  IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
      builder, "{\"id\": %" PRIu64 ", ", (uint64_t)device_info->device_id));
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_cstring(builder, "\"name\": \""));
  IREE_RETURN_IF_ERROR(
      iree_hrx_smi_append_json_escaped(builder, device_info->name));
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_cstring(builder, "\", \"path\": \""));
  IREE_RETURN_IF_ERROR(
      iree_hrx_smi_append_json_escaped(builder, device_info->path));

  if (iree_hal_driver_supports_device_report(driver)) {
    // Preferred path: emit a real typed JSON object under "report".
    IREE_RETURN_IF_ERROR(
        iree_string_builder_append_cstring(builder, "\", \"report\": "));
    iree_hal_device_report_writer_t writer;
    iree_hal_device_report_writer_initialize(IREE_HAL_DEVICE_REPORT_FORMAT_JSON,
                                             /*text_indent=*/0, builder,
                                             &writer);
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_begin_object(
        &writer, iree_string_view_empty()));
    IREE_RETURN_IF_ERROR(iree_hal_driver_dump_device_report(
        driver, device_info->device_id, &writer));
    IREE_RETURN_IF_ERROR(iree_hal_device_report_writer_end_object(&writer));
    return iree_string_builder_append_cstring(builder, "}");
  }

  // Fallback: embed the driver's free-form text dump as an escaped string.
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_cstring(builder, "\", \"detail\": \""));
  iree_string_builder_t detail;
  iree_string_builder_initialize(host_allocator, &detail);
  iree_status_t status =
      iree_hal_driver_dump_device_info(driver, device_info->device_id, &detail);
  if (iree_status_is_ok(status)) {
    status = iree_hrx_smi_append_json_escaped(
        builder, iree_string_builder_view(&detail));
  }
  iree_string_builder_deinitialize(&detail);
  IREE_RETURN_IF_ERROR(status);

  return iree_string_builder_append_cstring(builder, "\"}");
}

static iree_status_t iree_hrx_smi_append_driver_json(
    iree_hal_driver_registry_t* registry,
    const iree_hal_driver_info_t* driver_info, iree_allocator_t host_allocator,
    iree_string_builder_t* builder) {
  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "{"));
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_cstring(builder, "\"name\": \""));
  IREE_RETURN_IF_ERROR(
      iree_hrx_smi_append_json_escaped(builder, driver_info->driver_name));
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_cstring(builder, "\", \"full_name\": \""));
  IREE_RETURN_IF_ERROR(
      iree_hrx_smi_append_json_escaped(builder, driver_info->full_name));
  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "\", "));

  iree_hal_driver_t* driver = NULL;
  iree_status_t status = iree_hal_driver_registry_try_create(
      registry, driver_info->driver_name, host_allocator, &driver);
  if (!iree_status_is_ok(status)) {
    const char* code = iree_status_code_string(iree_status_code(status));
    iree_status_ignore(status);
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder, "\"available\": false, \"status\": \"%s\", \"devices\": []}",
        code));
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(
      builder, "\"available\": true, \"devices\": ["));

  iree_host_size_t device_count = 0;
  iree_hal_device_info_t* device_infos = NULL;
  status = iree_hal_driver_query_available_devices(
      driver, host_allocator, &device_count, &device_infos);
  if (iree_status_is_ok(status)) {
    for (iree_host_size_t i = 0; i < device_count; ++i) {
      if (i != 0) {
        status = iree_string_builder_append_cstring(builder, ", ");
        if (!iree_status_is_ok(status)) break;
      }
      status = iree_hrx_smi_append_device_json(driver, &device_infos[i],
                                               host_allocator, builder);
      if (!iree_status_is_ok(status)) break;
    }
  }

  iree_allocator_free(host_allocator, device_infos);
  iree_hal_driver_release(driver);
  IREE_RETURN_IF_ERROR(status);
  return iree_string_builder_append_cstring(builder, "]}");
}

static iree_status_t iree_hrx_smi_format_report(
    iree_hal_driver_registry_t* registry, const iree_hrx_smi_host_info_t* host,
    bool want_json, iree_allocator_t host_allocator,
    iree_string_builder_t* builder) {
  // Text: curated amdxdna-focused report (no registry walk needed).
  if (!want_json) {
    return iree_hrx_smi_format_text(host, host_allocator, builder);
  }

  // JSON: generic, walking the whole HAL driver registry and emitting each
  // driver's structured device report (or free-form detail fallback).
  iree_host_size_t driver_count = 0;
  iree_hal_driver_info_t* driver_infos = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_driver_registry_enumerate(
      registry, host_allocator, &driver_count, &driver_infos));

  iree_status_t status = iree_string_builder_append_cstring(builder, "{");
  if (iree_status_is_ok(status)) {
    status = iree_hrx_smi_format_host_json(host, builder);
  }
  if (iree_status_is_ok(status)) {
    status = iree_string_builder_append_cstring(builder, ", \"drivers\": [");
  }
  for (iree_host_size_t i = 0; iree_status_is_ok(status) && i < driver_count;
       ++i) {
    if (i != 0) {
      status = iree_string_builder_append_cstring(builder, ", ");
      if (!iree_status_is_ok(status)) break;
    }
    status = iree_hrx_smi_append_driver_json(registry, &driver_infos[i],
                                             host_allocator, builder);
  }
  if (iree_status_is_ok(status)) {
    status = iree_string_builder_append_cstring(builder, "]}\n");
  }

  iree_allocator_free(host_allocator, driver_infos);
  return status;
}

//===----------------------------------------------------------------------===//
// Command line
//===----------------------------------------------------------------------===//

static void iree_hrx_smi_print_usage(FILE* stream) {
  fprintf(stream,
          "Usage: hrx-smi examine [--format text|json]\n"
          "\n"
          "Reports host configuration and information for every HAL device\n"
          "provided by the drivers linked into this binary.\n"
          "\n"
          "Options:\n"
          "  --format text|json   Output format (default: text).\n"
          "  -h, --help           Show this help.\n");
}

int main(int argc, char* argv[]) {
  bool want_json = false;
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (strcmp(arg, "examine") == 0) {
      continue;  // Only subcommand today; accepted for xrt-smi familiarity.
    } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      iree_hrx_smi_print_usage(stdout);
      return 0;
    } else if (strcmp(arg, "--json") == 0) {
      want_json = true;
    } else if (strcmp(arg, "--format") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --format requires an argument\n");
        return 1;
      }
      const char* value = argv[++i];
      if (strcmp(value, "json") == 0) {
        want_json = true;
      } else if (strcmp(value, "text") == 0) {
        want_json = false;
      } else {
        fprintf(stderr, "error: unknown --format value '%s'\n", value);
        return 1;
      }
    } else if (strncmp(arg, "--format=", 9) == 0) {
      const char* value = arg + 9;
      if (strcmp(value, "json") == 0) {
        want_json = true;
      } else if (strcmp(value, "text") == 0) {
        want_json = false;
      } else {
        fprintf(stderr, "error: unknown --format value '%s'\n", value);
        return 1;
      }
    } else {
      fprintf(stderr, "error: unrecognized argument '%s'\n", arg);
      iree_hrx_smi_print_usage(stderr);
      return 1;
    }
  }

  iree_allocator_t host_allocator = iree_allocator_system();

  iree_hrx_smi_host_info_t host;
  iree_hrx_smi_collect_host_info(&host);

  iree_hal_driver_registry_t* registry = NULL;
  iree_status_t status =
      iree_hal_driver_registry_allocate(host_allocator, &registry);
  if (iree_status_is_ok(status)) {
    status = iree_hal_register_all_available_drivers(registry);
  }
  if (!iree_status_is_ok(status)) {
    fprintf(stderr, "error: failed to initialize HAL driver registry\n");
    iree_status_fprint(stderr, status);
    iree_status_ignore(status);
    if (registry) iree_hal_driver_registry_free(registry);
    return 1;
  }

  iree_string_builder_t builder;
  iree_string_builder_initialize(host_allocator, &builder);
  status = iree_hrx_smi_format_report(registry, &host, want_json,
                                      host_allocator, &builder);
  int exit_code = 0;
  if (iree_status_is_ok(status)) {
    fwrite(iree_string_builder_buffer(&builder), 1,
           iree_string_builder_size(&builder), stdout);
  } else {
    fprintf(stderr, "error: failed to format report\n");
    iree_status_fprint(stderr, status);
    iree_status_ignore(status);
    exit_code = 1;
  }
  iree_string_builder_deinitialize(&builder);
  iree_hal_driver_registry_free(registry);
  return exit_code;
}
