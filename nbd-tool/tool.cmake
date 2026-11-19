# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ (AltaStor.com)
#
# What abr-nbd-tool needs settled before the libraries are added: libnbd, for abr-disk-lib's NBD backend.
# Without it a tool that AUTO chose is left out, so that a Linux build without libnbd-dev still builds the others.

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(ABR_NBD_TOOL_LIBNBD QUIET libnbd>=1.18)
endif()

# pkg-config itself is asked for as well, since the cached result of the check outlives it.
if(NOT PKG_CONFIG_FOUND OR NOT ABR_NBD_TOOL_LIBNBD_FOUND)
    abr_tool_unavailable(nbd "pkg-config finds no libnbd 1.18 or later; install libnbd-dev")
    return()
endif()

set(ABR_DISK_WITH_NBD ON CACHE STRING "NBD backend (libnbd): AUTO, ON or OFF" FORCE)
