# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# abr-chain-reader: a chain of backups opened, checked and merged, and ChainReader, which reads a chain as one disk
# image. It has no CLI around it and does not depend on Tool::context(): the tools' copy loops in abr-tools-common
# and the nbdkit plugin both read chains through it.

add_library(abr-chain-reader STATIC
    src/chain.cpp
    src/chain_reader.cpp
)

target_include_directories(abr-chain-reader PUBLIC "${CMAKE_CURRENT_LIST_DIR}/inc")

# PUBLIC: chain.h names Abr:: types, and chain_reader.h implements abr-disk-lib's IDiskReader.
target_link_libraries(abr-chain-reader PUBLIC backuplib abr-disk-lib)

target_compile_features(abr-chain-reader PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(abr-chain-reader PRIVATE /utf-8 /W3)
else()
    target_compile_options(abr-chain-reader PRIVATE "$<$<CONFIG:Debug>:-g;-O0>")
endif()

# chain.cpp includes s3_backup.h, whose AWS headers give a debug object more sections than plain COFF can number.
if(MINGW)
    target_compile_options(abr-chain-reader PRIVATE "-Wa,-mbig-obj")
endif()

# The unit tests use the GoogleTest that backuplib's own tests bring, and run where the build runs.
if(TARGET GTest::gtest AND NOT CMAKE_CROSSCOMPILING)
    add_executable(abr-chain-reader-tests test/test_chain_reader.cpp)
    target_link_libraries(abr-chain-reader-tests PRIVATE abr-chain-reader GTest::gtest)

    if(MSVC)
        target_compile_options(abr-chain-reader-tests PRIVATE /utf-8 /W3)
    endif()

    include(GoogleTest)
    gtest_discover_tests(abr-chain-reader-tests
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        DISCOVERY_TIMEOUT 60
    )
endif()
