// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Structured device report writer.
//
// A backend-neutral, streaming writer for the typed key/value documents that
// drivers emit from their dump_device_report hook. The same sequence of writer
// calls can be serialized as either machine-readable JSON or human-readable
// pretty text, so a driver describes its device *once* and both formats are
// derived centrally.
//
// The emitted JSON is standard RFC 8259 and can be parsed back with the
// zero-copy parser in iree/base/internal/json.h, giving generic
// deserialization for free.
//
// Convention (soft, for cross-backend consumers):
//   A device report is an object. Drivers SHOULD group standardized fields
//   under IREE_HAL_DEVICE_REPORT_GROUP_COMMON (vendor, category, ...) and MAY
//   place any driver-specific fields under
//   IREE_HAL_DEVICE_REPORT_GROUP_BACKEND. Generic tools can rely on the
//   "common" group; anything under "backend" is free-form and best-effort.
//
// Example (a driver's dump_device_report writing into the current object):
//   iree_hal_device_report_writer_begin_object(w, IREE_SV("common"));
//   iree_hal_device_report_writer_cstring(w, IREE_SV("vendor"), "AMD");
//   iree_hal_device_report_writer_cstring(w, IREE_SV("category"), "npu");
//   iree_hal_device_report_writer_end_object(w);

#ifndef IREE_HAL_DEVICE_REPORT_H_
#define IREE_HAL_DEVICE_REPORT_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Recommended top-level group keys for device reports.
#define IREE_HAL_DEVICE_REPORT_GROUP_COMMON "common"
#define IREE_HAL_DEVICE_REPORT_GROUP_BACKEND "backend"

// Maximum object/array nesting depth. Device reports are shallow; this is a
// safety bound to keep the writer allocation-free.
#define IREE_HAL_DEVICE_REPORT_MAX_DEPTH 16

// Output serialization format.
typedef enum iree_hal_device_report_format_e {
  // Standard RFC 8259 JSON (compact). Round-trips with iree/base/internal/json.
  IREE_HAL_DEVICE_REPORT_FORMAT_JSON = 0,
  // Human-readable indented text (YAML-ish); not intended for machine parsing.
  IREE_HAL_DEVICE_REPORT_FORMAT_TEXT = 1,
} iree_hal_device_report_format_t;

typedef struct iree_hal_device_report_scope_t {
  bool is_array;
  uint32_t child_count;
} iree_hal_device_report_scope_t;

// A streaming typed-document writer that appends to a caller-owned string
// builder. Not thread-safe. All state is inline (no allocation).
typedef struct iree_hal_device_report_writer_t {
  iree_hal_device_report_format_t format;
  iree_string_builder_t* builder;
  // Number of spaces prefixed to every line in TEXT mode (indent base).
  iree_host_size_t text_indent;
  // Builder size captured at initialization, used to suppress a leading
  // newline.
  iree_host_size_t start_size;
  iree_host_size_t depth;
  iree_hal_device_report_scope_t scopes[IREE_HAL_DEVICE_REPORT_MAX_DEPTH];
} iree_hal_device_report_writer_t;

// Initializes a writer that appends to |builder| using |format|.
// |text_indent| spaces are prefixed to each line in TEXT mode (ignored for
// JSON); pass 0 for a top-level document.
IREE_API_EXPORT void iree_hal_device_report_writer_initialize(
    iree_hal_device_report_format_t format, iree_host_size_t text_indent,
    iree_string_builder_t* builder,
    iree_hal_device_report_writer_t* out_writer);

// Opens a nested object. |key| is used when the enclosing scope is an object
// and ignored (may be empty) when it is an array or at the document root.
IREE_API_EXPORT iree_status_t iree_hal_device_report_writer_begin_object(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key);
IREE_API_EXPORT iree_status_t iree_hal_device_report_writer_end_object(
    iree_hal_device_report_writer_t* writer);

// Opens a nested array. |key| semantics match begin_object.
IREE_API_EXPORT iree_status_t iree_hal_device_report_writer_begin_array(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key);
IREE_API_EXPORT iree_status_t iree_hal_device_report_writer_end_array(
    iree_hal_device_report_writer_t* writer);

// Scalar fields. |key| is ignored inside arrays.
IREE_API_EXPORT iree_status_t iree_hal_device_report_writer_string(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key,
    iree_string_view_t value);
IREE_API_EXPORT iree_status_t iree_hal_device_report_writer_cstring(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key,
    const char* value);
IREE_API_EXPORT iree_status_t
iree_hal_device_report_writer_i64(iree_hal_device_report_writer_t* writer,
                                  iree_string_view_t key, int64_t value);
IREE_API_EXPORT iree_status_t
iree_hal_device_report_writer_u64(iree_hal_device_report_writer_t* writer,
                                  iree_string_view_t key, uint64_t value);
IREE_API_EXPORT iree_status_t
iree_hal_device_report_writer_f64(iree_hal_device_report_writer_t* writer,
                                  iree_string_view_t key, double value);
IREE_API_EXPORT iree_status_t
iree_hal_device_report_writer_bool(iree_hal_device_report_writer_t* writer,
                                   iree_string_view_t key, bool value);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_DEVICE_REPORT_H_
