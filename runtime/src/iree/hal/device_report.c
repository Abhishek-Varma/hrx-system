// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/device_report.h"

#include <inttypes.h>
#include <string.h>

void iree_hal_device_report_writer_initialize(
    iree_hal_device_report_format_t format, iree_host_size_t text_indent,
    iree_string_builder_t* builder,
    iree_hal_device_report_writer_t* out_writer) {
  memset(out_writer, 0, sizeof(*out_writer));
  out_writer->format = format;
  out_writer->builder = builder;
  out_writer->text_indent = text_indent;
  out_writer->start_size = iree_string_builder_size(builder);
  out_writer->depth = 0;
}

static iree_status_t iree_hal_device_report_append_json_escaped(
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

// Emits the separator, key (in objects) or dash (in arrays), and the trailing
// punctuation that precedes a value. |scalar| distinguishes leaf values (which
// take " : " in text) from nested containers (which take ":").
static iree_status_t iree_hal_device_report_emit_prefix(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key,
    bool scalar) {
  iree_string_builder_t* builder = writer->builder;
  if (writer->depth == 0) {
    // Root value: no separator or key.
    return iree_ok_status();
  }
  iree_hal_device_report_scope_t* scope = &writer->scopes[writer->depth - 1];
  if (writer->format == IREE_HAL_DEVICE_REPORT_FORMAT_JSON) {
    if (scope->child_count != 0) {
      IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, ","));
    }
    if (!scope->is_array) {
      IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "\""));
      IREE_RETURN_IF_ERROR(
          iree_hal_device_report_append_json_escaped(builder, key));
      IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "\":"));
    }
  } else {
    // Suppress the newline before the very first emitted line.
    if (iree_string_builder_size(builder) != writer->start_size) {
      IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "\n"));
    }
    IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
        builder, "%*s", (int)(writer->text_indent + 2 * writer->depth), ""));
    if (scope->is_array) {
      IREE_RETURN_IF_ERROR(
          iree_string_builder_append_cstring(builder, scalar ? "- " : "-"));
    } else {
      IREE_RETURN_IF_ERROR(iree_string_builder_append_string(builder, key));
      IREE_RETURN_IF_ERROR(
          iree_string_builder_append_cstring(builder, scalar ? " : " : ":"));
    }
  }
  scope->child_count++;
  return iree_ok_status();
}

static iree_status_t iree_hal_device_report_push(
    iree_hal_device_report_writer_t* writer, bool is_array) {
  if (writer->depth >= IREE_HAL_DEVICE_REPORT_MAX_DEPTH) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "device report nesting exceeds %d levels",
                            IREE_HAL_DEVICE_REPORT_MAX_DEPTH);
  }
  writer->scopes[writer->depth].is_array = is_array;
  writer->scopes[writer->depth].child_count = 0;
  writer->depth++;
  return iree_ok_status();
}

iree_status_t iree_hal_device_report_writer_begin_object(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key) {
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_emit_prefix(writer, key, /*scalar=*/false));
  if (writer->format == IREE_HAL_DEVICE_REPORT_FORMAT_JSON) {
    IREE_RETURN_IF_ERROR(
        iree_string_builder_append_cstring(writer->builder, "{"));
  }
  return iree_hal_device_report_push(writer, /*is_array=*/false);
}

iree_status_t iree_hal_device_report_writer_end_object(
    iree_hal_device_report_writer_t* writer) {
  if (writer->depth == 0 || writer->scopes[writer->depth - 1].is_array) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "end_object without matching begin_object");
  }
  writer->depth--;
  if (writer->format == IREE_HAL_DEVICE_REPORT_FORMAT_JSON) {
    IREE_RETURN_IF_ERROR(
        iree_string_builder_append_cstring(writer->builder, "}"));
  }
  return iree_ok_status();
}

iree_status_t iree_hal_device_report_writer_begin_array(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key) {
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_emit_prefix(writer, key, /*scalar=*/false));
  if (writer->format == IREE_HAL_DEVICE_REPORT_FORMAT_JSON) {
    IREE_RETURN_IF_ERROR(
        iree_string_builder_append_cstring(writer->builder, "["));
  }
  return iree_hal_device_report_push(writer, /*is_array=*/true);
}

iree_status_t iree_hal_device_report_writer_end_array(
    iree_hal_device_report_writer_t* writer) {
  if (writer->depth == 0 || !writer->scopes[writer->depth - 1].is_array) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "end_array without matching begin_array");
  }
  writer->depth--;
  if (writer->format == IREE_HAL_DEVICE_REPORT_FORMAT_JSON) {
    IREE_RETURN_IF_ERROR(
        iree_string_builder_append_cstring(writer->builder, "]"));
  }
  return iree_ok_status();
}

iree_status_t iree_hal_device_report_writer_string(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key,
    iree_string_view_t value) {
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_emit_prefix(writer, key, /*scalar=*/true));
  iree_string_builder_t* builder = writer->builder;
  if (writer->format == IREE_HAL_DEVICE_REPORT_FORMAT_JSON) {
    IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "\""));
    IREE_RETURN_IF_ERROR(
        iree_hal_device_report_append_json_escaped(builder, value));
    return iree_string_builder_append_cstring(builder, "\"");
  }
  return iree_string_builder_append_string(builder, value);
}

iree_status_t iree_hal_device_report_writer_cstring(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key,
    const char* value) {
  return iree_hal_device_report_writer_string(
      writer, key, iree_make_cstring_view(value ? value : ""));
}

iree_status_t iree_hal_device_report_writer_i64(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key,
    int64_t value) {
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_emit_prefix(writer, key, /*scalar=*/true));
  return iree_string_builder_append_format(writer->builder, "%" PRId64, value);
}

iree_status_t iree_hal_device_report_writer_u64(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key,
    uint64_t value) {
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_emit_prefix(writer, key, /*scalar=*/true));
  return iree_string_builder_append_format(writer->builder, "%" PRIu64, value);
}

iree_status_t iree_hal_device_report_writer_f64(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key,
    double value) {
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_emit_prefix(writer, key, /*scalar=*/true));
  return iree_string_builder_append_format(writer->builder, "%.3f", value);
}

iree_status_t iree_hal_device_report_writer_bool(
    iree_hal_device_report_writer_t* writer, iree_string_view_t key,
    bool value) {
  IREE_RETURN_IF_ERROR(
      iree_hal_device_report_emit_prefix(writer, key, /*scalar=*/true));
  return iree_string_builder_append_cstring(writer->builder,
                                            value ? "true" : "false");
}
