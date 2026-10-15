# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# The version every tool reports, given by whoever builds it -- a package build
# passes its own, a developer's build is 1.0.0. Two of them, because Windows
# wants two:
#
#   ABR_VERSION          numeric, one to four parts: project(VERSION), and the
#                        FILEVERSION and PRODUCTVERSION of each .exe, which are
#                        what Explorer sorts by and what a WDAC publisher rule
#                        compares. A release line and a build number, 26.11.31.
#   ABR_VERSION_STRING   any text: what --version prints, and the FileVersion
#                        and ProductVersion strings of the version resource. The
#                        package's full version, 26.11~pre+31.g6a11c8981539.
#                        Empty is ABR_VERSION.
#
# Read before project(), which takes ABR_VERSION, so it decides nothing about
# the toolchain and may use nothing that project() sets. Leaves the parts in
# ABR_VERSION_MAJOR, _MINOR, _PATCH and _TWEAK, absent ones 0, and the text in
# ABR_VERSION_TEXT, for common/version.h.in.

set(ABR_VERSION "1.0.0" CACHE STRING
    "Numeric version, one to four parts of 0..65535, e.g. 26.11.31")
set(ABR_VERSION_STRING "" CACHE STRING
    "Full version for --version and the version resource's strings, e.g. 26.11~pre+31.g6a11c89; empty is ABR_VERSION")

# A VERSIONINFO field is a 16-bit WORD, and the resource compiler truncates a
# larger number without a word -- so a build number of 65536 would come out as
# 0 in the file's properties while --version said otherwise.
if(NOT ABR_VERSION MATCHES "^[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?(\\.[0-9]+)?$")
    message(FATAL_ERROR
        "ABR_VERSION is '${ABR_VERSION}'; it has to be one to four numbers separated by dots, "
        "e.g. 26.11.31. Text such as ~pre or +g<hash> goes in ABR_VERSION_STRING.")
endif()

string(REPLACE "." ";" _abr_version_parts "${ABR_VERSION}")

foreach(_abr_part IN LISTS _abr_version_parts)
    # Compared as a string first: a number past 64 bits would overflow math().
    string(LENGTH "${_abr_part}" _abr_digits)
    if(_abr_digits GREATER 5 OR _abr_part GREATER 65535)
        message(FATAL_ERROR
            "ABR_VERSION is '${ABR_VERSION}'; each part has to be 0..65535, the range of a field "
            "of the Windows version resource.")
    endif()
endforeach()

list(APPEND _abr_version_parts 0 0 0)
foreach(_abr_name IN ITEMS MAJOR MINOR PATCH TWEAK)
    list(POP_FRONT _abr_version_parts _abr_part)
    # Through math() to drop leading zeros, which C would read as octal.
    math(EXPR ABR_VERSION_${_abr_name} "${_abr_part}")
endforeach()

if(ABR_VERSION_STRING STREQUAL "")
    set(ABR_VERSION_TEXT "${ABR_VERSION}")
else()
    set(ABR_VERSION_TEXT "${ABR_VERSION_STRING}")
endif()

# The text lands in a C string literal and in a resource script's; a quote or a
# backslash would end or escape it, and a line break would end the line.
if(ABR_VERSION_TEXT MATCHES "[\"\\\\\n\r]")
    message(FATAL_ERROR
        "ABR_VERSION_STRING is '${ABR_VERSION_STRING}'; it may not contain quotes, backslashes "
        "or line breaks.")
endif()

unset(_abr_version_parts)
unset(_abr_part)
unset(_abr_digits)
unset(_abr_name)
