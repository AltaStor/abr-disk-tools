# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# The backuplib wiring: where it is, and what has to be declared ahead of it.
#
# include() this from the top-level CMakeLists.txt, then add backuplib:
#
#     include(cmake/backuplib.cmake)
#     add_subdirectory("${ABR_BACKUPLIB_DIR}" backuplib_build)
#
# An include() rather than part of that add_subdirectory on purpose: the
# directory has to be added below the directory-level settings
# (CMAKE_MSVC_RUNTIME_LIBRARY, link_directories, add_link_options) that every
# target created after them inherits, while what is here -- the GoogleTest in
# particular -- has to exist before backuplib looks for it. This file only ever
# declares.

# The backup library: the submodule, unless pointed at another checkout -- for
# working on a change to backuplib together with the tools that need it.
set(ABR_BACKUPLIB_DIR "${CMAKE_SOURCE_DIR}/abr-backup-lib"
    CACHE PATH "backuplib (abr-backup-lib) source tree")

if(NOT EXISTS "${ABR_BACKUPLIB_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "backuplib is missing at ${ABR_BACKUPLIB_DIR}.\n"
        "Run 'git submodule update --init --recursive', or name another checkout\n"
        "with -DABR_BACKUPLIB_DIR=/path/to/abr-backup-lib."
    )
endif()

# backuplib's unit tests are listed by running the test binary as part of the
# build, which a cross build cannot do with a Windows binary, and they have not
# been built with MSVC yet. They stay on for a native build; a cross build can
# have them back with a CMAKE_CROSSCOMPILING_EMULATOR such as wine. btest is a
# plain program and builds everywhere.
if(CMAKE_CROSSCOMPILING OR MSVC)
    set(_abr_unit_tests_default OFF)
else()
    set(_abr_unit_tests_default ON)
endif()
option(ABR_BACKUP_BUILD_UNIT_TESTS "Build backuplib's GoogleTest unit tests" ${_abr_unit_tests_default})

# backuplib's unit tests look for a GoogleTest and, finding none, fall back to
# fetching a release tarball at configure time -- which makes configuring need
# the network. A copy is already in the tree, under backuplib's crc32c
# submodule, so declare it here: ahead of backuplib, its GTest::gtest target
# satisfies that check and the fetch never happens.
set(GTEST_DIR "${ABR_BACKUPLIB_DIR}/third_party/crc32c/third_party/googletest")

if(ABR_BACKUP_BUILD_UNIT_TESTS AND EXISTS "${GTEST_DIR}/CMakeLists.txt")
    message(STATUS "Using the in-tree GoogleTest: ${GTEST_DIR}")

    # A build-time dependency only, so it stays out of the install set. gmock
    # goes unused. The shared CRT is what GoogleTest has to match unless the
    # build links the static one throughout.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    if(NOT CMAKE_MSVC_RUNTIME_LIBRARY)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    endif()

    # EXCLUDE_FROM_ALL keeps GoogleTest's own targets out of a default build.
    # The test executables that link it still pull it in.
    add_subdirectory("${GTEST_DIR}" googletest_build EXCLUDE_FROM_ALL)
elseif(ABR_BACKUP_BUILD_UNIT_TESTS)
    message(STATUS
        "No GoogleTest at ${GTEST_DIR}, so backuplib will fetch one. "
        "Run 'git submodule update --init --recursive' to use the in-tree copy."
    )
endif()
