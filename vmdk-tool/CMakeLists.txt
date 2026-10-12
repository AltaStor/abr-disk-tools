# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# abr-vmdk-tool: VMware VMDKs, local or on an ESXi host or a vCenter, through
# the VDDK. A binary and the launcher that points it at a VDDK installation.
#
# VDDK_DIR and the RPATH lists used below come from tool.cmake, which the
# top-level CMakeLists.txt read before the libraries were added.

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(AWS_S3_LINK_TARGETS "")

if(AWSSDK_LINK_LIBRARIES)
    list(APPEND AWS_S3_LINK_TARGETS ${AWSSDK_LINK_LIBRARIES})
elseif(TARGET aws-cpp-sdk-s3)
    list(APPEND AWS_S3_LINK_TARGETS aws-cpp-sdk-s3)
elseif(TARGET AWS::aws-cpp-sdk-s3)
    list(APPEND AWS_S3_LINK_TARGETS AWS::aws-cpp-sdk-s3)
endif()

if(NOT AWS_S3_LINK_TARGETS)
    message(FATAL_ERROR "AWS SDK S3 headers were found, but no link libraries were resolved.")
endif()

abr_add_tool(abr-vmdk-tool
    DESCRIPTION "AltaStor VMDK backup utility"
    SOURCES disk_tool.cpp
)

target_link_libraries(
    abr-vmdk-tool
    PRIVATE
        ${AWS_S3_LINK_TARGETS}
        crc32c
        zstd
        ssl
        crypto
        curl
        z
        cares
        dl
        Threads::Threads
)

set_target_properties(
    abr-vmdk-tool
    PROPERTIES
        BUILD_RPATH "${VDDK_RPATH}"
        INSTALL_RPATH "${VDDK_INSTALL_RPATH}"
        # Left to itself CMake appends the directory of every linked library to
        # the build tree's RPATH, which would put the build machine's VDDK back
        # in -- and ahead of LD_LIBRARY_PATH, so the launcher would appear to
        # select a bundle while a different one was quietly being used. Building
        # against the install RPATH keeps the binary developers run identical to
        # the one they ship.
        BUILD_WITH_INSTALL_RPATH ON
)

# Emit DT_RPATH rather than the default DT_RUNPATH. RUNPATH applies only to this
# binary's direct dependencies, so the VDDK's dlopen'd plugins would not inherit
# it; RPATH is searched for everything loaded afterwards. That matters because
# RPATH is also searched ahead of LD_LIBRARY_PATH: the plugins pick up the
# system libstdc++/libgcc_s from the directories above rather than the bundle's
# older copies, while still finding the VDDK's own libraries through the
# LD_LIBRARY_PATH the launcher exports.
target_link_options(abr-vmdk-tool PRIVATE "-Wl,--disable-new-dtags")

# The launcher looks for the binary beside itself, so a build tree needs a copy
# too -- not just an installed prefix -- for `build/abr-vmdk-tool.sh` to work
# straight out of the build directory.
add_custom_command(
    TARGET abr-vmdk-tool POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_LIST_DIR}/abr-vmdk-tool.sh"
        "$<TARGET_FILE_DIR:abr-vmdk-tool>/abr-vmdk-tool.sh"
    COMMENT "Copying abr-vmdk-tool.sh beside abr-vmdk-tool"
)

# The copy above isn't a tracked build output, so the built-in `clean` target
# would otherwise leave it behind.
set_property(
    DIRECTORY APPEND
    PROPERTY ADDITIONAL_CLEAN_FILES "$<TARGET_FILE_DIR:abr-vmdk-tool>/abr-vmdk-tool.sh"
)

# The launcher has to land beside the binary: that is where it looks for it, and
# it is what callers are expected to run.
install(
    PROGRAMS "${CMAKE_CURRENT_LIST_DIR}/abr-vmdk-tool.sh"
    DESTINATION bin
)
