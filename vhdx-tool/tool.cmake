# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# What abr-vhdx-tool needs settled before the libraries are added.

# The tool is nothing without the Hyper-V backend, so it is asked for outright
# rather than left to abr-disk-lib's reading of the target.
set(ABR_DISK_WITH_VHDX ON CACHE STRING "Hyper-V VHD/VHDX backend: AUTO, ON or OFF" FORCE)
