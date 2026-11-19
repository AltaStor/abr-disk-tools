# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ (AltaStor.com)
#
# abr-nbd-tool: disks exported over NBD, such as the backup exports Proxmox VE hands to a backup provider.
# One binary on Linux; it links libnbd from the system, and backuplib brings the rest.

get_target_property(_abr_nbd_tool_disk_defs abr-disk-lib INTERFACE_COMPILE_DEFINITIONS)
if(NOT "ABR_DISK_HAVE_NBD" IN_LIST _abr_nbd_tool_disk_defs)
    message(FATAL_ERROR "abr-nbd-tool needs abr-disk-lib's NBD backend, and the library was configured without it.")
endif()

abr_add_tool(abr-nbd-tool
    DESCRIPTION "AltaStor NBD backup utility"
    SOURCES disk_tool.cpp
)

# The end-to-end test reads qemu-storage-daemon exports, so it needs QEMU's tools; without them it is left out.
find_program(ABR_NBD_TOOL_QEMU_STORAGE_DAEMON qemu-storage-daemon)
find_program(ABR_NBD_TOOL_QEMU_IMG qemu-img)
find_program(ABR_NBD_TOOL_QEMU_IO qemu-io)
if(ABR_NBD_TOOL_QEMU_STORAGE_DAEMON AND ABR_NBD_TOOL_QEMU_IMG AND ABR_NBD_TOOL_QEMU_IO)
    add_test(NAME abr-nbd-tool
        COMMAND "${CMAKE_SOURCE_DIR}/test/nbd/test-nbd-tool.sh" "$<TARGET_FILE:abr-nbd-tool>"
                "${CMAKE_CURRENT_BINARY_DIR}/test-scratch")
else()
    message(STATUS "abr-nbd-tool: the end-to-end test is left out; it needs qemu-storage-daemon, qemu-img and qemu-io")
endif()
