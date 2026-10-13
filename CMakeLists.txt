# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# abr-disk-tools: one command line tool per virtual disk format, built from
# one tree.
#
#   common/         the CLI every tool shares
#   vmdk-tool/, vhdx-tool/
#                   one folder per format -- see cmake/tools.cmake for what
#                   makes a folder a tool, and README.md for adding one
#   abr-backup-lib/ the .abr format and its storage (submodule)
#   abr-disk-lib/   the disk backends (submodule)
#
# What is built is decided by parameters:
#
#   ABR_TARGET              auto | linux | windows | windows-cross
#   ABR_BUILD_<FMT>_TOOL    AUTO | ON | OFF, per tool; AUTO is "when the target
#                           is one the tool supports"
#   ABR_ENABLED_TOOLS       the tools to build, by format (vmdk, or vmdk,vhdx),
#                           in place of those switches; empty leaves it to them
cmake_minimum_required(VERSION 3.20)

# Has to come before project(), which is where the toolchain is read.
include(cmake/select-toolchain.cmake)

project(abr-disk-tools VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# The toolchain has been read by now, so what was asked for can be held against
# what it actually targets: a toolchain file named beside ABR_TARGET wins the
# choice of compiler, and must not win it silently.
if(ABR_TARGET STREQUAL "linux" AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ABR_TARGET=linux, but the toolchain targets ${CMAKE_SYSTEM_NAME}.")
endif()
if(ABR_TARGET MATCHES "^windows" AND NOT WIN32)
    message(FATAL_ERROR "ABR_TARGET=${ABR_TARGET}, but the toolchain targets ${CMAKE_SYSTEM_NAME}.")
endif()
if(ABR_TARGET STREQUAL "windows-cross" AND NOT CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "ABR_TARGET=windows-cross, but this is not a cross build.")
endif()

# The version resource every Windows tool carries.
if(WIN32)
    enable_language(RC)
endif()

# backuplib enables testing in its own scope, which registers its tests but
# leaves the top of the build tree without the file ctest starts from -- so
# `ctest` run from there reports finding no tests at all. Enabling it here too
# generates that file, and the chain down to the tests below it.
enable_testing()

# --- The tools ----------------------------------------------------------------
#
# One line per format; the tool is the folder <format>-tool. Each enabled
# tool's tool.cmake is read here, ahead of
# the libraries, so that abr-disk-lib is built with the backends they need.

include(cmake/tools.cmake)

abr_tool(vmdk SYSTEMS Linux)
abr_tool(vhdx SYSTEMS Windows)

abr_enabled_tool_folders(_abr_tool_folders)

if(NOT _abr_tool_folders)
    message(FATAL_ERROR
        "No tool is built for ${CMAKE_SYSTEM_NAME} with ABR_ENABLED_TOOLS and the "
        "ABR_BUILD_<FMT>_TOOL switches as they are. See README.md for which tool supports "
        "which target.")
endif()

# --- What every target below inherits ------------------------------------------
#
# Directory-level settings: each target takes them as it is created, backuplib's
# and the libraries' included, so they come before any add_subdirectory.

# A static triplet's libraries are built against the static C runtime (/MT),
# and everything linked with them has to be too.
if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(AWSSDK CONFIG QUIET COMPONENTS s3)

if(NOT AWSSDK_FOUND)
    message(FATAL_ERROR
        "AWS SDK for C++ S3 was not found (required by backuplib).\n"
        "Put vcpkg at $VCPKG_ROOT or ~/vcpkg, where the configure finds it and builds the\n"
        "dependencies in vcpkg.json itself, or name its toolchain explicitly:\n"
        "  cmake -S . -B build "
        "-DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake"
    )
endif()

# MinGW spells the Windows import libraries in lower case, and a Linux file
# system tells libUserenv.a from libuserenv.a, but the AWS SDK asks the linker
# for Userenv and Wininet by their Windows SDK names. Each name gets a one-line
# linker script here that forwards to the real library. A directory property,
# so that backuplib's btest and unit tests, which link the SDK too, find them.
if(MINGW AND NOT CMAKE_HOST_WIN32)
    set(ABR_MINGW_CASE_SHIMS "${CMAKE_CURRENT_BINARY_DIR}/mingw-case-shims")

    foreach(_abr_library IN ITEMS Userenv Wininet)
        string(TOLOWER "${_abr_library}" _abr_lower)
        file(WRITE "${ABR_MINGW_CASE_SHIMS}/lib${_abr_library}.a" "INPUT(-l${_abr_lower})\n")
    endforeach()

    link_directories("${ABR_MINGW_CASE_SHIMS}")
endif()

# libstdc++, libgcc and winpthread linked into every executable -- the tools and
# backuplib's btest and unit tests alike -- so that each runs with nothing
# beside it but Windows itself.
if(MINGW)
    add_link_options(-static)
endif()

# --- The libraries ------------------------------------------------------------
#
# In this order: common/ links the other two.

include(cmake/backuplib.cmake)
include(cmake/disklib.cmake)

add_subdirectory("${ABR_BACKUPLIB_DIR}" backuplib_build)
add_subdirectory("${ABR_DISKLIB_DIR}" disklib_build)
add_subdirectory(common)

# One version for every tool's --version output and version resource.
set(ABR_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(common/version.h.in "${ABR_GENERATED_INCLUDE_DIR}/version.h" @ONLY)

foreach(_abr_tool IN LISTS _abr_tool_folders)
    add_subdirectory(${_abr_tool})
endforeach()

# CMake reserves the name "clean" for its own built-in target, which only
# removes tracked build outputs and leaves the configured build tree in place.
# This instead removes the whole build directory, so the next configure starts
# fresh.
add_custom_target(distclean
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_BINARY_DIR}"
    # Run from the source tree, not the build tree: the command deletes the
    # latter, and a shell whose cwd vanishes mid-command reports a spurious
    # getcwd error even though the removal itself succeeds.
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Removing the build directory"
)
