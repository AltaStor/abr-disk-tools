# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ (AltaStor.com)
#
# nbdkit-abr-plugin: serves a chain of .abr backups read-only over NBD, for Proxmox VE restores.
# A shared object nbdkit loads, built when plugin.cmake found nbdkit's headers. It reads the chain through
# abr-chain-reader, and backuplib brings the rest.

add_library(nbdkit-abr-plugin MODULE abr_plugin.cpp)

target_include_directories(nbdkit-abr-plugin PRIVATE "${ABR_GENERATED_INCLUDE_DIR}")

target_link_libraries(nbdkit-abr-plugin PRIVATE abr-chain-reader backuplib PkgConfig::ABR_NBDKIT)

# nbdkit-<name>-plugin.so is the name nbdkit finds a plugin by. Only plugin_init is exported, see exports.map.
set_target_properties(nbdkit-abr-plugin PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/exports.map"
)
target_link_options(nbdkit-abr-plugin PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
target_compile_options(nbdkit-abr-plugin PRIVATE "$<$<CONFIG:Debug>:-g;-O0>")

install(TARGETS nbdkit-abr-plugin LIBRARY DESTINATION lib)

# The end-to-end test makes its chains with abr-nbd-tool from qemu-storage-daemon exports, and reads them back with
# qemu-img and nbdinfo; without them it is left out.
find_program(ABR_NBDKIT_PLUGIN_NBDKIT nbdkit)
find_program(ABR_NBDKIT_PLUGIN_NBDINFO nbdinfo)
find_program(ABR_NBDKIT_PLUGIN_QEMU_STORAGE_DAEMON qemu-storage-daemon)
find_program(ABR_NBDKIT_PLUGIN_QEMU_IMG qemu-img)
find_program(ABR_NBDKIT_PLUGIN_QEMU_IO qemu-io)
if(TARGET abr-nbd-tool AND ABR_NBDKIT_PLUGIN_NBDKIT AND ABR_NBDKIT_PLUGIN_NBDINFO
   AND ABR_NBDKIT_PLUGIN_QEMU_STORAGE_DAEMON AND ABR_NBDKIT_PLUGIN_QEMU_IMG AND ABR_NBDKIT_PLUGIN_QEMU_IO)
    add_test(NAME nbdkit-abr-plugin
        COMMAND "${CMAKE_SOURCE_DIR}/test/nbdkit/test-nbdkit-plugin.sh" "$<TARGET_FILE:abr-nbd-tool>"
                "$<TARGET_FILE:nbdkit-abr-plugin>" "${CMAKE_CURRENT_BINARY_DIR}/test-scratch")
else()
    message(STATUS "nbdkit-abr-plugin: the end-to-end test is left out; it needs abr-nbd-tool, nbdkit, nbdinfo, "
                   "qemu-storage-daemon, qemu-img and qemu-io")
endif()
