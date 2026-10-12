# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# vcpkg's community x64-mingw-static triplet, plus what building it from a
# Linux host needs. The top-level CMakeLists.txt puts this directory on
# VCPKG_OVERLAY_TRIPLETS, so it replaces the stock triplet of the same name.
#
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_ENV_PASSTHROUGH PATH)

set(VCPKG_CMAKE_SYSTEM_NAME MinGW)

# aws-sdk-cpp picks its platform layer from the machine doing the build rather
# than from the one the build is for, unless TARGET_ARCH says otherwise. Left
# to itself, a cross build on Linux gets the Linux layer: the curl HTTP client
# and POSIX headers, which a MinGW compiler cannot build. The port only passes
# TARGET_ARCH for Android, so it is passed here. On a Windows host the SDK
# would infer the same thing, so this changes nothing there.
if(PORT STREQUAL "aws-sdk-cpp")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS "-DTARGET_ARCH=WINDOWS")

    # The port builds the SDK as one translation unit per library, and in a
    # debug build that object has more sections than plain COFF can number.
    set(VCPKG_C_FLAGS "-Wa,-mbig-obj")

    # The SDK's Windows sources include <Windows.h>, <WinSock2.h> and the like,
    # which MinGW ships in lower case -- see ../mingw-case-shims/README.md.
    if(NOT CMAKE_HOST_WIN32)
        string(APPEND VCPKG_C_FLAGS " -isystem ${CMAKE_CURRENT_LIST_DIR}/../mingw-case-shims")
    endif()

    set(VCPKG_CXX_FLAGS "${VCPKG_C_FLAGS}")
endif()
