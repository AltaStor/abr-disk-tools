# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# abr-vhdx-tool: Hyper-V VHD, VHDX and AVHDX, through the Windows virtual disk
# service. One self-contained .exe; no launcher.

abr_add_tool(abr-vhdx-tool
    DESCRIPTION "AltaStor VHDX backup utility"
    SOURCES disk_tool.cpp
)
