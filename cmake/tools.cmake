# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# What makes a folder a tool.
#
# A tool is one virtual disk format's command line: a folder named
# <format>-tool, holding
#
#   tool.cmake       read early, before any library is added. Says which
#                    backends abr-disk-lib has to be built with, and checks
#                    whatever only that format needs (an SDK, say).
#   CMakeLists.txt   read late, after the libraries. Calls abr_add_tool().
#   disk_tool.cpp    the format's options and help, openSource/openTarget,
#                    runBackup, runRestore and the command table.
#
# and one line in the top-level CMakeLists.txt:
#
#   abr_tool(vmdk SYSTEMS Linux)
#
# Two phases because of what abr-disk-lib is: one library, built once, with the
# backends of every enabled tool. Each tool has to have had its say before that
# library's target is created, and can only be linked after.

# Declares a tool and decides whether this build has it.
#
#   abr_declare_tool(<format> SYSTEMS <CMAKE_SYSTEM_NAME>...)
#
# The tool is the folder <format>-tool. Creates ABR_BUILD_<FORMAT>_TOOL --
# AUTO, ON or OFF -- where AUTO means "when
# the target is one of SYSTEMS". ON for any other target is an error rather
# than a tool that fails to compile a screen later.
#
# Three states rather than an option(): an option's default is fixed by the
# first configure, and the answer here depends on a target that a fresh build
# directory may choose differently.
function(abr_declare_tool format)
    cmake_parse_arguments(PARSE_ARGV 1 _tool "" "" "SYSTEMS")

    set(_folder "${format}-tool")

    string(TOUPPER "${format}" _upper)
    set(_switch "ABR_BUILD_${_upper}_TOOL")

    set(${_switch} "AUTO" CACHE STRING "Build abr-${format}-tool: AUTO, ON or OFF")
    set_property(CACHE ${_switch} PROPERTY STRINGS AUTO ON OFF)

    if(NOT ${_switch} MATCHES "^(AUTO|ON|OFF)$")
        message(FATAL_ERROR "${_switch} is '${${_switch}}'; it has to be AUTO, ON or OFF.")
    endif()

    if(CMAKE_SYSTEM_NAME IN_LIST _tool_SYSTEMS)
        set(_supported TRUE)
    else()
        set(_supported FALSE)
    endif()

    string(REPLACE ";" ", " _systems "${_tool_SYSTEMS}")

    if(${_switch} STREQUAL "AUTO")
        set(_enabled ${_supported})
    elseif(${_switch} AND NOT _supported)
        message(FATAL_ERROR
            "${_switch}=ON, but abr-${format}-tool is built for ${_systems} and this build "
            "targets ${CMAKE_SYSTEM_NAME}. See README.md for the targets and how to pick one.")
    else()
        set(_enabled ${${_switch}})
    endif()

    set_property(GLOBAL APPEND PROPERTY ABR_ALL_TOOLS "${_folder}")

    if(_enabled)
        set_property(GLOBAL APPEND PROPERTY ABR_ENABLED_TOOLS "${_folder}")
        message(STATUS "abr-${format}-tool: yes")

        # In the caller's scope, so that what it sets -- ABR_DISK_WITH_<X> in
        # the cache, VDDK_DIR and the like as plain variables -- is there for
        # the libraries added afterwards.
        set(_abr_tool_config "${CMAKE_SOURCE_DIR}/${_folder}/tool.cmake" PARENT_SCOPE)
    else()
        message(STATUS "abr-${format}-tool: no (built for ${_systems})")
        set(_abr_tool_config "" PARENT_SCOPE)
    endif()
endfunction()

# The function above cannot include() on the caller's behalf -- a function has
# a scope of its own, and a tool.cmake's variables would end with it -- so the
# declaration is this macro: the function decides, the macro includes.
macro(abr_tool format)
    abr_declare_tool(${format} ${ARGN})
    if(_abr_tool_config AND EXISTS "${_abr_tool_config}")
        include("${_abr_tool_config}")
    endif()
endmacro()

# Creates a tool's executable, with everything every tool has in common.
#
#   abr_add_tool(<target>
#       DESCRIPTION <what the Windows version resource calls it>
#       SOURCES <file>...)
#
# The target links the common CLI layer, abr-disk-lib and backuplib, finds the
# generated version.h, lands in <build>/bin and is installed to bin. On Windows
# it also gets its version resource and its manifest, generated from the
# templates in common/windows, so that every tool carries the same manifest --
# which matters: backuplib opens files by narrow path, and only the manifest's
# activeCodePage makes those UTF-8.
function(abr_add_tool target)
    cmake_parse_arguments(PARSE_ARGV 1 _tool "" "DESCRIPTION" "SOURCES")

    if(NOT _tool_SOURCES)
        message(FATAL_ERROR "abr_add_tool(${target}): no SOURCES")
    endif()
    if(NOT _tool_DESCRIPTION)
        set(_tool_DESCRIPTION "${target}")
    endif()

    add_executable(${target} ${_tool_SOURCES})

    target_include_directories(${target}
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${ABR_GENERATED_INCLUDE_DIR}"
    )

    target_link_libraries(${target} PRIVATE abr-tools-common abr-disk-lib backuplib)

    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    if(MSVC)
        # The sources are UTF-8 (the copyright lines, at least), which MSVC
        # reads as the ANSI code page unless told.
        target_compile_options(${target} PRIVATE /utf-8 /W3)
    else()
        target_compile_options(${target} PRIVATE "$<$<CONFIG:Debug>:-g;-O0>")
    endif()

    # The AWS SDK's headers give a debug object more sections than plain COFF
    # can number. A target option, so it is set per target; it does not
    # propagate the way the directory-level -static does.
    if(MINGW)
        target_compile_options(${target} PRIVATE "-Wa,-mbig-obj")
    endif()

    if(WIN32)
        # Windows 10 is the oldest the tools run on. abr-disk-lib names the
        # same version for its own sources, which are the ones that need what
        # arrived with it; here it keeps the headers a tool sees in step.
        target_compile_definitions(${target}
            PRIVATE
                UNICODE
                _UNICODE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
                _WIN32_WINNT=0x0A00
                WINVER=0x0A00
                NTDDI_VERSION=0x0A000002
        )

        set(ABR_TOOL_NAME "${target}")
        set(ABR_TOOL_DESCRIPTION "${_tool_DESCRIPTION}")
        set(_resources "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
        set(_manifest "${CMAKE_CURRENT_BINARY_DIR}/${target}.manifest")

        configure_file("${CMAKE_SOURCE_DIR}/common/windows/tool.rc.in" "${_resources}" @ONLY)
        configure_file("${CMAKE_SOURCE_DIR}/common/windows/tool.manifest.in" "${_manifest}" @ONLY)

        target_sources(${target} PRIVATE "${_resources}")

        # The manifest goes in through the linker with MSVC, which merges it
        # with the one it writes itself, and through the resource script with
        # MinGW, whose linker writes none -- never both, or the two resources
        # would collide.
        if(MSVC)
            target_sources(${target} PRIVATE "${_manifest}")
        else()
            set_source_files_properties("${_resources}" PROPERTIES COMPILE_DEFINITIONS ABR_EMBED_MANIFEST)
        endif()
    endif()

    install(TARGETS ${target} RUNTIME DESTINATION bin)
endfunction()
