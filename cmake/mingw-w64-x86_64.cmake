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

# The tools are built against the Universal C Runtime, ucrtbase.dll, which is
# part of every Windows from 10 and Server 2016 on, and not against the old
# msvcrt.dll: UCRT honours the UTF-8 code page the manifest asks for, and is
# the only runtime an arm64 Windows build can have. Which of the two a program
# gets is the toolchain's choice, not a flag's -- libstdc++, winpthread and
# mingw-w64's own startup code are built against one of them -- so the choice
# is of a compiler. Debian and Ubuntu package the UCRT one under this triple
# (g++-mingw-w64-ucrt64, posix threads). A UCRT toolchain spelt the classic
# way, x86_64-w64-mingw32, as a hand-built or an MSYS2 one usually is, is named
# with -DABR_MINGW_TRIPLE=x86_64-w64-mingw32; the configure checks that it
# really targets UCRT (the top-level CMakeLists.txt).
set(ABR_MINGW_TRIPLE "x86_64-w64-mingw32ucrt" CACHE STRING "Target triple of the MinGW-w64 toolchain")

set(CMAKE_C_COMPILER "${ABR_MINGW_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${ABR_MINGW_TRIPLE}-g++")
set(CMAKE_RC_COMPILER "${ABR_MINGW_TRIPLE}-windres")

# Looked up on PATH, where vcpkg's ports will look too. A compiler missing
# there would otherwise surface deep in the first port's configure, on the
# host's cc refusing Windows flags, which says nothing about why -- so it stops
# the configure here, before vcpkg starts. Searched afresh every time, since a
# result left in the cache would outlive the PATH it was found on.
unset(ABR_MINGW_CXX CACHE)
find_program(ABR_MINGW_CXX "${ABR_MINGW_TRIPLE}-g++")
if(NOT ABR_MINGW_CXX)
    message(FATAL_ERROR
        "${ABR_MINGW_TRIPLE}-g++ is not on PATH. vcpkg builds the dependencies "
        "with the compiler of that name on PATH, so the MinGW-w64 toolchain has "
        "to be there even when it is installed elsewhere; README.md says which "
        "packages provide it.")
endif()

# A program and the libraries it links have to come from one compiler, and
# vcpkg builds the dependencies with a toolchain file of its own, which only
# knows the classic names. The overlay triplet hands it this one instead
# (triplets/x64-mingw-static.cmake), learning it from the environment, which
# vcpkg inherits: vcpkg.cmake loads this file before it runs vcpkg. By full
# path, the directory found above and the triple: a bare name would be taken
# for a file in the port's build directory.
get_filename_component(_abr_mingw_dir "${ABR_MINGW_CXX}" DIRECTORY)
set(ENV{ABR_MINGW_PREFIX} "${_abr_mingw_dir}/${ABR_MINGW_TRIPLE}")

# And it has to be a UCRT one. The top-level CMakeLists.txt checks that for
# every MinGW build, but only after project() -- by which time vcpkg has built
# every dependency with this compiler, the AWS SDK included. So the cross build
# asks here first, and an msvcrt compiler costs seconds, not ten minutes.
# mingw-w64's headers define _UCRT for a UCRT toolchain. Not again for each
# try_compile, which reads this file too.
if(NOT CMAKE_IN_TRY_COMPILE)
    set(_abr_crt_probe "${CMAKE_BINARY_DIR}/CMakeFiles/abr-crt-probe.cpp")
    file(WRITE "${_abr_crt_probe}" "#include <cstdlib>\n#ifdef _UCRT\nABR_CRT_IS_UCRT\n#endif\n")
    execute_process(
        COMMAND "${ABR_MINGW_CXX}" -E "${_abr_crt_probe}"
        OUTPUT_VARIABLE _abr_crt_probe_out
        ERROR_QUIET)
    if(NOT _abr_crt_probe_out MATCHES "ABR_CRT_IS_UCRT")
        message(FATAL_ERROR
            "${ABR_MINGW_CXX} builds against msvcrt.dll; the tools are built against the "
            "Universal C Runtime. On Debian and Ubuntu install g++-mingw-w64-ucrt64 and keep "
            "the default ABR_MINGW_TRIPLE; README.md has the details.")
    endif()
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
