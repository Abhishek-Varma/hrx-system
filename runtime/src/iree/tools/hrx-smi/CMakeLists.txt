# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# NOTE: hand-maintained (autogeneration disabled) so accelerator drivers can be
# linked conditionally. The curated text report is per-backend and each driver
# only exists as a CMake target when enabled, so hrx-smi links whichever are
# available (or none) and gates the matching code paths with HRX_SMI_HAVE_*.

iree_add_all_subdirs()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")

set(_HRX_SMI_DEPS
  iree::base
  iree::hal
  iree::hal::drivers
)
set(_HRX_SMI_DEFINES)

if(IREE_HAL_DRIVER_AMDGPU)
  list(APPEND _HRX_SMI_DEPS iree::hal::drivers::amdgpu)
  list(APPEND _HRX_SMI_DEFINES "HRX_SMI_HAVE_AMDGPU=1")
endif()

if(IREE_HAL_DRIVER_AMDXDNA)
  list(APPEND _HRX_SMI_DEPS iree::hal::drivers::amdxdna)
  list(APPEND _HRX_SMI_DEFINES "HRX_SMI_HAVE_AMDXDNA=1")
endif()

iree_cc_binary(
  NAME
    hrx-smi
  SRCS
    "hrx-smi.c"
  DEPS
    ${_HRX_SMI_DEPS}
  DEFINES
    ${_HRX_SMI_DEFINES}
)

endif()
