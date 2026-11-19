# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ (AltaStor.com)
#
# Whether nbdkit-abr-plugin is built. Read ahead of the libraries, like a tool's tool.cmake, because a plugin is a
# shared object: every static library it links has to be built position independent, and that is decided before
# they are added.
#
#   ABR_BUILD_NBDKIT_PLUGIN   AUTO | ON | OFF; AUTO builds it on Linux when pkg-config finds nbdkit 1.30 or later
#
# Sets ABR_NBDKIT_PLUGIN to ON or OFF, and PkgConfig::ABR_NBDKIT for the plugin's headers.

set(ABR_BUILD_NBDKIT_PLUGIN AUTO CACHE STRING "Build nbdkit-abr-plugin: AUTO, ON or OFF")
set_property(CACHE ABR_BUILD_NBDKIT_PLUGIN PROPERTY STRINGS AUTO ON OFF)

if(NOT ABR_BUILD_NBDKIT_PLUGIN MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "ABR_BUILD_NBDKIT_PLUGIN is '${ABR_BUILD_NBDKIT_PLUGIN}'; it has to be AUTO, ON or OFF.")
endif()

set(ABR_NBDKIT_PLUGIN OFF)
set(_abr_nbdkit_why_not "")

if(ABR_BUILD_NBDKIT_PLUGIN STREQUAL "OFF")
    set(_abr_nbdkit_why_not "ABR_BUILD_NBDKIT_PLUGIN is OFF")
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_abr_nbdkit_why_not "Linux only")
else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(ABR_NBDKIT QUIET IMPORTED_TARGET GLOBAL nbdkit>=1.30)
    endif()

    if(TARGET PkgConfig::ABR_NBDKIT)
        set(ABR_NBDKIT_PLUGIN ON)
    else()
        set(_abr_nbdkit_why_not "pkg-config finds no nbdkit 1.30 or later; install nbdkit-plugin-dev")
    endif()
endif()

if(ABR_NBDKIT_PLUGIN)
    message(STATUS "nbdkit-abr-plugin: yes")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
elseif(ABR_BUILD_NBDKIT_PLUGIN STREQUAL "ON")
    message(FATAL_ERROR "ABR_BUILD_NBDKIT_PLUGIN is ON, but the plugin cannot be built: ${_abr_nbdkit_why_not}.")
else()
    message(STATUS "nbdkit-abr-plugin: no (${_abr_nbdkit_why_not})")
endif()
