// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The usage text every tool prints the same way
//
#pragma once

#include <string>

namespace Tool
{
    //
    // The handful of words that differ between the tools, in one place, so
    // that everything around them can be shared verbatim.
    //
    struct Branding {
        // The name to fall back on when argv[0] is empty.
        const char* programName;

        // What --version reports.
        const char* version;

        // What the command list calls the thing being backed up: "virtual
        // disk" for abr-vhdx-tool, "VMDK" for abr-vmdk-tool.
        const char* diskNoun;

        // What interrupted() says is being released, which differs because
        // what each tool holds does: a VM with its methods disabled is
        // abr-vmdk-tool's problem alone.
        const char* interruptNote;
    };

    void printMainHelp(const std::string& programName, const Branding& branding);

    void printStorageOptions();
    void printSourceStorageOptions();

    // printBackupHelp and printRestoreHelp stay with each tool: their wording
    // differs in several places and they end by printing the tool's own disk
    // options, which only it can describe.
    void printCopyHelp(const std::string& programName);
    void printListHelp(const std::string& programName);
    void printInfoHelp(const std::string& programName);
    void printVerifyHelp(const std::string& programName);
    void printRemoveHelp(const std::string& programName);
    void printRetentionHelp(const std::string& programName);
    void printMetaSaveHelp(const std::string& programName);
    void printMetaLoadHelp(const std::string& programName);
}
