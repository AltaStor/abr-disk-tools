# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Cross compiling the tools for 64-bit Windows with MinGW-w64.
#
# -DABR_TARGET=windows-cross selects this file, behind vcpkg's toolchain, which
# loads it in turn. It can also be
# named on its own, with the dependencies then found some other way:
#
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(ABR_MINGW_TRIPLE "x86_64-w64-mingw32" CACHE STRING "Target triple of the MinGW-w64 toolchain")

# The unsuffixed names, since those are what vcpkg builds the dependencies
# with, and a program and the libraries it links have to come from one
# compiler. Debian and Ubuntu make them alternatives for a -win32 and a -posix
# flavour; README.md says which to select and why.
set(CMAKE_C_COMPILER "${ABR_MINGW_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${ABR_MINGW_TRIPLE}-g++")
set(CMAKE_RC_COMPILER "${ABR_MINGW_TRIPLE}-windres")

# vcpkg builds the dependencies with a toolchain file of its own, which looks
# these names up on PATH and, finding nothing, falls back to the host's cc
# without a word. The first port then fails deep in its configure, on a Linux
# linker refusing Windows flags, which says nothing about why. So the compiler
# is looked up here the same way, and a MinGW that is not on PATH stops the
# configure before vcpkg starts: vcpkg.cmake loads this file before it installs
# anything. Searched afresh every time, since a result left in the cache would
# outlive the PATH it was found on.
unset(ABR_MINGW_CXX CACHE)
find_program(ABR_MINGW_CXX "${ABR_MINGW_TRIPLE}-g++")
if(NOT ABR_MINGW_CXX)
    message(FATAL_ERROR
        "${ABR_MINGW_TRIPLE}-g++ is not on PATH. vcpkg builds the dependencies "
        "with the compiler of that name on PATH, so the MinGW-w64 toolchain has "
        "to be there even when it is installed elsewhere; README.md says which "
        "packages provide it.")
endif()

# Libraries, headers and packages come from the target's side only: the MinGW
# sysroot, and vcpkg's tree, which vcpkg.cmake adds to this list. Nothing of the
# build host's may end up in a Windows binary.
#
# The sysroot sits beside the compiler's bin directory -- /usr/<triple> for the
# distribution's packages, somewhere else for a toolchain unpacked by hand --
# so it is found from the compiler, through the alternatives links, rather
# than assumed.
#
# Appended to, never assigned: a first configure reads this file again after
# vcpkg.cmake has put its tree in the list, and assigning would drop it.
get_filename_component(_abr_mingw_bin "${ABR_MINGW_CXX}" REALPATH)
get_filename_component(_abr_mingw_bin "${_abr_mingw_bin}" DIRECTORY)
get_filename_component(_abr_mingw_sysroot "${_abr_mingw_bin}/../${ABR_MINGW_TRIPLE}" ABSOLUTE)
if(NOT _abr_mingw_sysroot IN_LIST CMAKE_FIND_ROOT_PATH)
    list(APPEND CMAKE_FIND_ROOT_PATH "${_abr_mingw_sysroot}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
