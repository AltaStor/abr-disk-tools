# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# What abr-vmdk-tool needs settled before the libraries are added: where the
# VDDK is, for abr-disk-lib to build its VMware backend against, and the RPATH
# the tool is linked with because of it.

# Beside the checkout rather than inside it by default: the bundle is neither
# part of the repository nor specific to one checkout of it, and a sibling
# directory is shared by every working tree next to it.
set(VDDK_DIR "${CMAKE_SOURCE_DIR}/../vddk" CACHE PATH "Path to the VMware VDDK root")
get_filename_component(VDDK_DIR "${VDDK_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")

set(VDDK_INCLUDE_DIR "${VDDK_DIR}/include")
set(VDDK_LIBRARY_DIR "${VDDK_DIR}/lib64")
set(VDDK_LIBRARY "${VDDK_LIBRARY_DIR}/libvixDiskLib.so")
# The VDDK bundle ships its own libstdc++/libcrypto/libcurl. Those are older than
# the ones this binary is compiled against, so the compiler's runtime directories
# must be searched before the bundle or the tool fails to start.
#
# Only those directories go in. The bundle itself is deliberately left out: its
# location is a runtime choice, made by the LD_LIBRARY_PATH abr-vmdk-tool.sh
# exports, and an RPATH naming the build machine's copy would quietly take
# precedence over it.
#
# The implicit list cannot be trusted as it stands. CMake works it out by test
# linking with the flags the build was configured with, so every -L the caller
# passes ends up in it -- a package build that points the linker at a staging
# tree for a dependency puts that tree in the list, and from there at the front
# of the RPATH of the binary that ships. That path does not exist on the target
# machine, but it is searched ahead of both LD_LIBRARY_PATH and the system
# directories, so any local user who can create it decides where libcrypto,
# libcurl and libzstd come from for a process the backup server runs under its
# own account. It also puts a build machine's directory layout in the artifact.
#
# So keep only what belongs to a system prefix, and drop anything else.
set(VDDK_RPATH_SYSTEM_PREFIXES /lib /lib64 /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64)

set(VDDK_RPATH "")
set(VDDK_RPATH_REJECTED "")

foreach(_abr_vmdk_tool_link_dir IN LISTS CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES)
    # Normalized first so that a "." or ".." component cannot walk out of
    # a prefix that the untouched string appears to sit inside.
    cmake_path(SET _abr_vmdk_tool_normalized NORMALIZE "${_abr_vmdk_tool_link_dir}")

    set(_abr_vmdk_tool_is_system FALSE)

    foreach(_abr_vmdk_tool_prefix IN LISTS VDDK_RPATH_SYSTEM_PREFIXES)
        # The prefix itself, or a directory below it. The trailing separator is
        # what keeps /usr/libexec from passing as a subdirectory of /usr/lib.
        if(_abr_vmdk_tool_normalized STREQUAL _abr_vmdk_tool_prefix
           OR _abr_vmdk_tool_normalized MATCHES "^${_abr_vmdk_tool_prefix}/")
            set(_abr_vmdk_tool_is_system TRUE)
            break()
        endif()
    endforeach()

    if(_abr_vmdk_tool_is_system)
        list(APPEND VDDK_RPATH "${_abr_vmdk_tool_normalized}")
    else()
        list(APPEND VDDK_RPATH_REJECTED "${_abr_vmdk_tool_normalized}")
    endif()
endforeach()

list(REMOVE_DUPLICATES VDDK_RPATH)

# Worth saying out loud: a rejected directory is one the link needs but the
# runtime will not get, so a library that lives only there has to be found some
# other way -- through the launcher's LD_LIBRARY_PATH, or by being installed.
if(VDDK_RPATH_REJECTED)
    list(REMOVE_DUPLICATES VDDK_RPATH_REJECTED)
    message(STATUS
        "Keeping these out of the RPATH, as they are outside any system prefix: "
        "${VDDK_RPATH_REJECTED}"
    )
endif()

set(VDDK_INSTALL_RPATH "${VDDK_RPATH}" CACHE STRING "RPATH to embed in the installed binary")

message(STATUS "Selected VDDK_DIR: ${VDDK_DIR}")

# Only the headers are needed to build, and it is abr-disk-lib that compiles
# against them: the library itself is bound at runtime, from wherever the launcher
# points the loader, so the machine that compiles the tool does not have to be
# the one that has the bundle installed.
# Without them the tool still builds, with the SOAP API alone; -DABR_DISK_WITH_VDDK=ON
# makes them required, as a package build that ships the VDDK path should.
if(NOT EXISTS "${VDDK_INCLUDE_DIR}/vixDiskLib.h")
    message(STATUS
        "VDDK headers not found at ${VDDK_INCLUDE_DIR}/vixDiskLib.h; abr-vmdk-tool is built "
        "without the VDDK unless -DABR_DISK_WITH_VDDK=ON. Point CMake at an extracted VDDK "
        "with -DVDDK_DIR=/path/to/vddk"
    )
endif()

if(NOT EXISTS "${VDDK_LIBRARY}")
    message(STATUS
        "VDDK library not present at ${VDDK_LIBRARY}; "
        "an installation will have to be in place at runtime for the launcher to find"
    )
endif()

# vsphere-soap-lib, the SOAP API's library, finds OpenSSL and zlib with find_package, which
# under vcpkg takes the static copies vcpkg installs for the AWS SDK. Linked into the binary,
# a static OpenSSL then answers the VDDK's own TLS calls next to the system's shared one,
# and the two mixed crash the first VDDK connection. The tool links the system's (ssl, crypto
# and z in vmdk-tool/CMakeLists.txt), so find_package is pointed at those: the system paths
# searched as they are, not re-rooted into vcpkg's tree.
find_path(ABR_SYSTEM_OPENSSL_INCLUDE_DIR openssl/ssl.h NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(ABR_SYSTEM_SSL_LIBRARY ssl NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(ABR_SYSTEM_CRYPTO_LIBRARY crypto NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_path(ABR_SYSTEM_ZLIB_INCLUDE_DIR zlib.h NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_library(ABR_SYSTEM_ZLIB_LIBRARY z NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(ABR_SYSTEM_OPENSSL_INCLUDE_DIR AND ABR_SYSTEM_SSL_LIBRARY AND ABR_SYSTEM_CRYPTO_LIBRARY)
    set(OPENSSL_INCLUDE_DIR "${ABR_SYSTEM_OPENSSL_INCLUDE_DIR}" CACHE PATH "" FORCE)
    set(OPENSSL_SSL_LIBRARY "${ABR_SYSTEM_SSL_LIBRARY}" CACHE FILEPATH "" FORCE)
    set(OPENSSL_CRYPTO_LIBRARY "${ABR_SYSTEM_CRYPTO_LIBRARY}" CACHE FILEPATH "" FORCE)
endif()
if(ABR_SYSTEM_ZLIB_INCLUDE_DIR AND ABR_SYSTEM_ZLIB_LIBRARY)
    set(ZLIB_INCLUDE_DIR "${ABR_SYSTEM_ZLIB_INCLUDE_DIR}" CACHE PATH "" FORCE)
    set(ZLIB_LIBRARY "${ABR_SYSTEM_ZLIB_LIBRARY}" CACHE FILEPATH "" FORCE)
    set(ZLIB_LIBRARY_RELEASE "${ABR_SYSTEM_ZLIB_LIBRARY}" CACHE FILEPATH "" FORCE)
    set(ZLIB_LIBRARY_DEBUG "${ABR_SYSTEM_ZLIB_LIBRARY}" CACHE FILEPATH "" FORCE)
endif()

# The tool reaches VMware through the VDDK, the vSphere SOAP API, or both: abr-disk-lib
# builds each by its own AUTO reading (the VDDK where its headers are, SOAP where its
# submodule is), and vmdk-tool/CMakeLists.txt refuses a library with neither.
