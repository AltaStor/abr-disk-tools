# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# The CLI every tool shares: the option parser and table, the --json writer,
# the copy loops, the chain merge, the commands that touch no disk backend, the
# host shim, and Tool::run.
#
# No project() here, and nothing to decide about the toolchain, the build type
# or the MSVC runtime: the top-level CMakeLists.txt has settled them, and has
# added backuplib and abr-disk-lib, which this links.

add_library(abr-tools-common STATIC
    src/cli_options.cpp
    src/tool_json.cpp
    src/tool_options.cpp
    src/tool_help.cpp
    src/tool_backup.cpp
    src/tool_commands.cpp
)

# One half of the host shim, never both: separate translation units rather than
# one file behind #ifdef, so that neither host's headers have to be reachable
# from the other's build. plat_windows.cpp leans on win_util, which is
# abr-disk-lib's -- the disk layers need it first.
if(WIN32)
    target_sources(abr-tools-common PRIVATE src/plat_windows.cpp)
else()
    target_sources(abr-tools-common PRIVATE src/plat_posix.cpp)
endif()

target_include_directories(abr-tools-common PUBLIC "${CMAKE_CURRENT_LIST_DIR}/inc")

# PUBLIC: Abr:: types are in this library's own headers, so anything that
# includes them needs backuplib's include directory too -- which is also how
# each tool's disk_tool.cpp finds "abr_backup.h" by its bare name. The same
# goes for abr-disk-lib: tool_backup.h takes an IDiskReader and an IDiskWriter,
# which are declared in its disk_device.h.
target_link_libraries(abr-tools-common PUBLIC backuplib abr-disk-lib)

target_compile_features(abr-tools-common PUBLIC cxx_std_20)

if(MSVC)
    # The sources are UTF-8 (the copyright lines, at least), which MSVC reads
    # as the ANSI code page unless told otherwise.
    target_compile_options(abr-tools-common PRIVATE /utf-8 /W3)
else()
    target_compile_options(abr-tools-common PRIVATE "$<$<CONFIG:Debug>:-g;-O0>")
endif()

# Two of the sources include "s3_backup.h", which pulls in <aws/s3/S3Client.h>;
# those headers give a debug object more sections than plain COFF can number.
# A target option, which is why abr_add_tool() sets it again for each tool.
if(MINGW)
    target_compile_options(abr-tools-common PRIVATE "-Wa,-mbig-obj")
endif()
