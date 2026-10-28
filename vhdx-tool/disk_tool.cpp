// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor VHDX backup utility
//

#include <iostream>
#include <memory>
#include <string>

#include "cli_options.h"
#include "disk_device.h"
#include "tool_commands.h"
#include "tool_disk.h"
#include "tool_help.h"
#include "tool_options.h"
#include "vhdx_disk.h"
#include "version.h"

// Everything common/ shares between the tools is used here by its plain name,
// as it was when it lived in this file.
using namespace Tool;

// The few words common/ has to put in this tool's mouth.
const Tool::Branding kBranding = {
    "abr-vhdx-tool",
    ABR_DISK_TOOLS_VERSION,
    "virtual disk",
    "releasing the disk",
};

// This tool's half of the option table. Adding one of its options means a value here,
// a row in kToolLongOptions below, and a case in setToolOption -- the same
// three places a shared option needs, over in common/.
enum LongOnlyOption {
    kOptVhdx = Tool::kOptToolFirst,
};

const Cli::LongOption kToolLongOptions[] = {
    {"vhdx",  Cli::Argument::Required, kOptVhdx},
    {nullptr, Cli::Argument::None,     0},
};

struct Options : Tool::CommonOptions {
    std::string vhdx;
};

void printVhdxOptions(bool forRestore)
{
    std::cout << "Virtual disk (Hyper-V) options:\n"
              << "      --vhdx PATH         VHD, VHDX or AVHDX to read/write through the Windows\n"
              << "                          virtual disk service instead of a plain file or\n"
              << "                          device. Needs an elevated (administrator) process.\n";
    if (forRestore) {
        std::cout << "                          A target that does not exist is created as a\n"
                  << "                          dynamic VHDX, or a VHD if named .vhd.\n";
    } else {
        std::cout << "                          For a running VM, back up a checkpoint's disk.\n";
    }
}

void printBackupHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " backup [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name (required)\n"
              << "  -s, --source PATH       Source file, disk or volume to back up, such as\n"
              << "                          \\\\.\\PhysicalDrive1 (required unless --vhdx)\n"
              << "  -k, --key KEY           Encryption key (optional)\n"
              << "  -c, --compression NUM   Zstd compression level 0-" << kMaxCompression
              << ", 0 for none (default: " << kDefaultCompression << ")\n"
              << "      --type KIND         'full' (default) copies the whole disk;\n"
              << "                          'incremental' copies only the blocks changed\n"
              << "                          since --change-id. Incremental needs --vhdx,\n"
              << "                          since it relies on Hyper-V resilient change\n"
              << "                          tracking (RCT).\n"
              << "      --change-id ID      RCT id an incremental is taken against\n"
              << "      --immutable MODE    Hold the backup in 'governance' or 'compliance'\n"
              << "                          mode, so that it cannot be deleted or overwritten\n"
              << "                          before its retention expires (S3 only)\n"
              << "      --retention DAYS    How long --immutable holds the backup, in days\n"
              << "      --retention-till DATE\n"
              << "                          Hold it until DATE instead, in UTC as\n"
              << "                          YYYY-MM-DDTHH:MM:SSZ; one of the two is required\n\n";
    printStorageOptions();
    std::cout << "\n";
    printVhdxOptions(false);
}

void printRestoreHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " restore [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name (required unless --source-names)\n"
              << "      --source-names LIST Comma-separated chain to restore at once in place\n"
              << "                          of --name: the backup to start from first, then\n"
              << "                          the incrementals on top of it in the order they\n"
              << "                          were taken. Each block is written once, as the\n"
              << "                          last backup that changed it holds it\n"
              << "  -t, --target PATH       Target file, disk or volume to restore to\n"
              << "                          (required unless --vhdx)\n"
              << "  -k, --key KEY           Decryption key (optional)\n"
              << "  -f, --force             Apply an incremental backup, or a chain that\n"
              << "                          starts with one, onto a target that already\n"
              << "                          holds its parent\n\n";
    printStorageOptions();
    std::cout << "\n";
    printVhdxOptions(true);
}

bool setToolOption(Options& options, int value, const std::string& argument)
{
    switch (value) {
        case kOptVhdx:
            options.vhdx = argument;
            return true;
        default:
            return false; // unreachable: the two value ranges are disjoint
    }
}

bool parseOptions(int argc, char* argv[], Options& options)
{
    static const Tool::OptionTable table(kToolLongOptions);

    return Tool::parseSharedOptions(argc, argv, table.rows(), options,
        [&options](int value, const std::string& argument) {
            return setToolOption(options, value, argument);
        });
}

// Handed to Tool::Context for the commands common/ runs. Not called parseSharedOptions:
// that is the library's own entry point, and under the using-directive above two functions
// of that name would be one overload set.
//
// The commands want the shared half only, which a full parse yields as its base -- and the slice is
// on purpose: nothing in those commands reads this tool's half, but a bad
// value in it is still an error, as it has always been.
bool parseCommonOptions(int argc, char* argv[], Tool::CommonOptions& shared)
{
    Options options;

    if (!parseOptions(argc, argv, options)) {
        return false;
    }

    shared = options;
    return true;
}

// --vhdx takes the place of --source or --target; naming both would leave the tool
// to guess which disk was meant.
bool checkDiskOptions(const std::string& fileOption, const std::string& file, const Options& options)
{
    if (!file.empty() && !options.vhdx.empty()) {
        std::cerr << "Error: " << fileOption << " and --vhdx name the same disk; give one\n";
        return false;
    }
    return true;
}

// What a virtual disk says about itself travels with its reader, for 'backup'
// to report next to the data it read.
struct VhdxSource : Tool::DiskSource {
    Vhdx::DiskProperties properties;

    std::string description() const override
    {
        if (properties.format.empty()) {
            return {};
        }
        return properties.format + (properties.type.empty() ? "" : ", " + properties.type);
    }

    // What the next incremental of this disk is taken against.
    void addDetails(Tool::JsonObject& json) const override
    {
        if (!properties.changeTrackingId.empty()) {
            json.addString("rct_id", properties.changeTrackingId);
        }
    }

    void printDetails() const override
    {
        if (!properties.changeTrackingId.empty()) {
            std::cout << "RCT id:            " << properties.changeTrackingId << "\n";
        }
    }
};

// This tool's half of 'backup' and 'restore'; the rest of both is common/'s.
class VhdxTool : public Tool::DiskTool
{
public:
    bool parseOptions(int argc, char* argv[]) override { return ::parseOptions(argc, argv, m_options); }
    const Tool::CommonOptions& options() const override { return m_options; }

    void printBackupHelp(const std::string& programName) const override { ::printBackupHelp(programName); }
    void printRestoreHelp(const std::string& programName) const override { ::printRestoreHelp(programName); }

    bool checkOptions(bool forRestore) const override
    {
        return forRestore ? checkDiskOptions("--target", m_options.target, m_options)
                          : checkDiskOptions("--source", m_options.source, m_options);
    }

    const char* diskOption() const override { return "--vhdx"; }
    const std::string& disk() const override { return m_options.vhdx; }

    std::unique_ptr<Tool::DiskSource> openSource() override
    {
        auto pSource = std::make_unique<VhdxSource>();

        if (m_options.vhdx.empty()) {
            pSource->reader = openFileReader(m_options.source);
        } else {
            pSource->reader = Vhdx::openReader(m_options.vhdx, m_options.changeId, pSource->properties);
        }
        return pSource->reader ? std::move(pSource) : nullptr;
    }

    // A --vhdx target that is not there yet is created with the geometry of the backup,
    // and the report then says so.
    std::unique_ptr<Tool::DiskTarget> openTarget(bool keepExisting, const DiskGeometry& image) override
    {
        auto pTarget = std::make_unique<Tool::DiskTarget>();

        if (m_options.vhdx.empty()) {
            pTarget->writer = openFileWriter(m_options.target, keepExisting);
        } else {
            bool created = false;
            pTarget->writer = Vhdx::openWriter(m_options.vhdx, keepExisting, image, created);
            pTarget->created = created;
        }
        return pTarget->writer ? std::move(pTarget) : nullptr;
    }

private:
    Options m_options;
};

// The command table. Every command is common/'s; 'backup' and 'restore' reach
// this tool's disks through VhdxTool above, by way of Tool::Context.
// Adding a command means a branch here and in every other tool's table, a run<Cmd> and
// a print<Cmd>Help in common/, and, if it touches a disk, what it needs added to
// Tool::DiskTool.
//
int dispatch(const std::string& programName, const std::string& command, int argc, char* argv[])
{
    if (command == "backup") {
        return Tool::runBackup(programName, argc, argv);
    } else if (command == "restore") {
        return Tool::runRestore(programName, argc, argv);
    } else if (command == "copy") {
        return Tool::runCopy(programName, argc, argv);
    } else if (command == "list") {
        return Tool::runList(programName, argc, argv);
    } else if (command == "info") {
        return Tool::runInfo(programName, argc, argv);
    } else if (command == "verify") {
        return Tool::runVerify(programName, argc, argv);
    } else if (command == "remove") {
        return Tool::runRemove(programName, argc, argv);
    } else if (command == "retention") {
        return Tool::runRetention(programName, argc, argv);
    } else if (command == "meta_save") {
        return Tool::runMetaSave(programName, argc, argv);
    } else if (command == "meta_load") {
        return Tool::runMetaLoad(programName, argc, argv);
    }
    return Tool::kUnknownCommand;
}

int main(int argc, char* argv[])
{
    static const Tool::Context context{kBranding, parseCommonOptions,
                                       [] { return std::make_unique<VhdxTool>(); }};

    return Tool::run(argc, argv, context, dispatch);
}
