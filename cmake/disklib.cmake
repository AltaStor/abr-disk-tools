# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Where abr-disk-lib is: every disk backend -- files and devices, Hyper-V
# virtual disks, VMware disks -- behind IDiskReader/IDiskWriter.
#
# Like backuplib.cmake this only ever declares. The add_subdirectory stays in
# the top-level CMakeLists.txt for the same reason backuplib's does: the
# library's target has to be created below the directory-level settings that
# every target created after them inherits -- and ahead of common/, which
# links it.
#
# Which backends the library is built with is each tool's to say, in its
# tool.cmake, by setting ABR_DISK_WITH_VHDX, ABR_DISK_WITH_VDDK and the like
# before the add_subdirectory; a backend no enabled tool asked for is left to
# the library's own reading of the target.

# The disk library: the submodule, unless pointed at another checkout -- for
# working on a change to abr-disk-lib together with the tools that need it.
set(ABR_DISKLIB_DIR "${CMAKE_SOURCE_DIR}/abr-disk-lib"
    CACHE PATH "abr-disk-lib source tree")

if(NOT EXISTS "${ABR_DISKLIB_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "abr-disk-lib is missing at ${ABR_DISKLIB_DIR}.\n"
        "Run 'git submodule update --init --recursive', or name another checkout\n"
        "with -DABR_DISKLIB_DIR=/path/to/abr-disk-lib."
    )
endif()
