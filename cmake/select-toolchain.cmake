# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Turns ABR_TARGET into a toolchain, before the first project() -- the one
# moment CMake reads a toolchain file -- which is why the top-level
# CMakeLists.txt includes this ahead of everything else. Anything here that ran
# after project() would be recorded in the cache and then quietly ignored.
#
#   auto            what the host builds natively, or whatever a toolchain file
#                   named on the command line targets (the default)
#   linux           Linux, natively
#   windows         Windows, natively (MSVC, or MinGW in an MSYS2 shell)
#   windows-cross   Windows, from any other host, with MinGW-w64
#
# Three decisions follow from it, each skipped when the caller already made it:
#
#   1. The compiler: the host's own, or MinGW-w64 for a cross build.
#   2. vcpkg, which builds the AWS SDK and the rest of vcpkg.json for the
#      target, found at $VCPKG_ROOT or ~/vcpkg. A compiler toolchain from
#      step 1 or from the command line is moved behind it, as the one vcpkg's
#      toolchain chainloads.
#   3. The vcpkg triplet. For Windows, static libraries throughout, so that
#      each tool is one self-contained .exe that can be copied onto a host.
#      For Linux, vcpkg's own default.

set(ABR_TARGET "auto" CACHE STRING "What to build for: auto, linux, windows or windows-cross")
set_property(CACHE ABR_TARGET PROPERTY STRINGS auto linux windows windows-cross)

if(NOT ABR_TARGET MATCHES "^(auto|linux|windows|windows-cross)$")
    message(FATAL_ERROR
        "ABR_TARGET is '${ABR_TARGET}'; it has to be one of auto, linux, windows or windows-cross.")
endif()

# CMake resolves a relative toolchain path against the build tree first, then
# the source tree. It is about to be handed to vcpkg's toolchain, which would
# resolve it against neither, so it is made absolute here the same way.
function(_abr_absolute_toolchain out path)
    if(IS_ABSOLUTE "${path}")
        set(${out} "${path}" PARENT_SCOPE)
    elseif(EXISTS "${CMAKE_BINARY_DIR}/${path}")
        set(${out} "${CMAKE_BINARY_DIR}/${path}" PARENT_SCOPE)
    else()
        set(${out} "${CMAKE_SOURCE_DIR}/${path}" PARENT_SCOPE)
    endif()
endfunction()

# 1. The compiler: one named on the command line, or the MinGW-w64 one for a
# cross build. Once vcpkg's toolchain has taken its place (a reconfigure), it
# is VCPKG_CHAINLOAD_TOOLCHAIN_FILE that remembers it.
set(_abr_compiler_toolchain "")
if(CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg\\.cmake$")
    _abr_absolute_toolchain(_abr_compiler_toolchain "${CMAKE_TOOLCHAIN_FILE}")
endif()

# What the build directory already cross compiles with, if anything: a target
# asked for now has to agree with it, since CMake will not read a toolchain a
# second time.
set(_abr_cross_toolchain "${_abr_compiler_toolchain}${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}")

if(ABR_TARGET STREQUAL "linux")
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "ABR_TARGET=linux is a native build and this host is ${CMAKE_HOST_SYSTEM_NAME}; "
            "there is no cross toolchain for Linux here.")
    endif()
    if(_abr_cross_toolchain)
        message(FATAL_ERROR
            "ABR_TARGET=linux is the host's own compiler, but a compiler toolchain is set: "
            "${_abr_cross_toolchain}. Drop one of the two -- in a build directory configured "
            "for another target before, that means starting a fresh one.")
    endif()

elseif(ABR_TARGET STREQUAL "windows")
    if(NOT CMAKE_HOST_WIN32)
        message(FATAL_ERROR
            "ABR_TARGET=windows is a native build and this host is ${CMAKE_HOST_SYSTEM_NAME}; "
            "use -DABR_TARGET=windows-cross here.")
    endif()

elseif(ABR_TARGET STREQUAL "windows-cross")
    if(CMAKE_HOST_WIN32)
        message(FATAL_ERROR
            "ABR_TARGET=windows-cross is for building on another host; "
            "on Windows use -DABR_TARGET=windows.")
    endif()
    # A toolchain file the caller named stands: another MinGW, or a clang
    # targeting Windows, is as good a cross compiler as the one in the tree.
    if(NOT _abr_cross_toolchain)
        set(_abr_compiler_toolchain "${CMAKE_CURRENT_LIST_DIR}/mingw-w64-x86_64.cmake")
    endif()
endif()

# 2. vcpkg.
set(_abr_vcpkg_roots "")
foreach(_abr_variable IN ITEMS VCPKG_ROOT HOME USERPROFILE)
    if(DEFINED ENV{${_abr_variable}} AND NOT "$ENV{${_abr_variable}}" STREQUAL "")
        if(_abr_variable STREQUAL "VCPKG_ROOT")
            list(APPEND _abr_vcpkg_roots "$ENV{VCPKG_ROOT}")
        else()
            list(APPEND _abr_vcpkg_roots "$ENV{${_abr_variable}}/vcpkg")
        endif()
    endif()
endforeach()

set(_abr_vcpkg_toolchain "")
foreach(_abr_root IN LISTS _abr_vcpkg_roots)
    file(TO_CMAKE_PATH "${_abr_root}/scripts/buildsystems/vcpkg.cmake" _abr_candidate)
    if(EXISTS "${_abr_candidate}")
        set(_abr_vcpkg_toolchain "${_abr_candidate}")
        break()
    endif()
endforeach()

if(_abr_vcpkg_toolchain)
    if(_abr_compiler_toolchain)
        set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${_abr_compiler_toolchain}"
            CACHE FILEPATH "Compiler toolchain vcpkg's toolchain loads" FORCE)
    endif()
    if(NOT CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg\\.cmake$")
        set(CMAKE_TOOLCHAIN_FILE "${_abr_vcpkg_toolchain}" CACHE FILEPATH "Toolchain file" FORCE)
    endif()
elseif(_abr_compiler_toolchain AND NOT CMAKE_TOOLCHAIN_FILE)
    # No vcpkg: the dependencies have to be found some other way, and the
    # top-level CMakeLists.txt says so if they are not.
    set(CMAKE_TOOLCHAIN_FILE "${_abr_compiler_toolchain}" CACHE FILEPATH "Toolchain file")
endif()

# 3. The triplet, for Windows only; a Linux build takes vcpkg's default.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    if("${VCPKG_CHAINLOAD_TOOLCHAIN_FILE}${CMAKE_TOOLCHAIN_FILE}" MATCHES "mingw"
       OR (CMAKE_HOST_WIN32 AND DEFINED ENV{MSYSTEM}))
        # A MinGW compiler, cross or in an MSYS2 shell.
        set(VCPKG_TARGET_TRIPLET "x64-mingw-static" CACHE STRING "vcpkg target triplet")
    elseif(CMAKE_HOST_WIN32)
        # MSVC, with the C runtime linked in (/MT) to match.
        set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg target triplet")
    endif()
endif()

# The stock x64-mingw-static cannot build the AWS SDK from a Linux host; the
# overlay of the same name can -- see triplets/x64-mingw-static.cmake. It only
# ever replaces that one triplet, so naming it for every build changes nothing
# for the others.
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_LIST_DIR}/triplets" CACHE STRING "vcpkg overlay triplets")
