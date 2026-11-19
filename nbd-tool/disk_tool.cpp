// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// AltaStor NBD backup utility
//
// Backs up a disk exported over NBD, such as the backup export Proxmox VE hands to a backup provider for each disk of
// a VM. Restores write a plain file or device, since PVE restores read their chain through nbdkit instead.
//

#include <iostream>
#include <memory>
#include <string>

#include "cli_options.h"
#include "disk_device.h"
#include "nbd_disk.h"
#include "tool_commands.h"
#include "tool_disk.h"
#include "tool_help.h"
#include "tool_options.h"
#include "version.h"

using namespace Tool;

// The few words common/ has to put in this tool's mouth.
const Tool::Branding kBranding = {
    "abr-nbd-tool",
    ABR_DISK_TOOLS_VERSION,
    "NBD export",
    "closing the NBD connection",
    "disk/volume",
};

// This tool's half of the option table: a value here, a row in kToolLongOptions and a case in setToolOption.
enum LongOnlyOption {
    kOptNbd = Tool::kOptToolFirst,
    kOptExport,
    kOptBwlimit,
    kOptDiscard,
};

const Cli::LongOption kToolLongOptions[] = {
    {"nbd",     Cli::Argument::Required, kOptNbd},
    {"export",  Cli::Argument::Required, kOptExport},
    {"bwlimit", Cli::Argument::Required, kOptBwlimit},
    {"discard", Cli::Argument::None,     kOptDiscard},
    {nullptr,   Cli::Argument::None,     0},
};

struct Options : Tool::CommonOptions {
    std::string nbd;
    std::string exportName;
    // MiB/s, 0 for no limit.
    uint64_t bwlimit = 0;
    bool discard = false;

    bool anyNbdOption() const { return !nbd.empty() || !exportName.empty() || bwlimit != 0 || discard; }
};

void printNbdOptions(bool forRestore)
{
    std::cout << "NBD options:\n";
    if (forRestore) {
        std::cout << "  A restore writes a plain file or device with --target. --nbd and the\n"
                  << "  options below are for backups.\n";
        return;
    }
    std::cout << "      --nbd SERVER        NBD export to read instead of a plain file or device:\n"
              << "                          a Unix socket path, or an NBD URI such as\n"
              << "                          nbd+unix:///drive-scsi0?socket=/run/disk.sock\n"
              << "      --export NAME       Export to open on a socket, such as drive-scsi0\n"
              << "                          (default: the server's default export)\n"
              << "      --bwlimit MIB       Read at most MIB MiB/s (default: no limit)\n"
              << "      --discard           Discard each range after reading it. On a Proxmox VE\n"
              << "                          backup export this frees the fleecing image early.\n"
              << "                          On an export of a disk itself it ERASES the data.\n";
}

void printBackupHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " backup [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name (required)\n"
              << "  -s, --source PATH       Source file or device to back up (required unless\n"
              << "                          --nbd)\n"
              << "  -k, --key KEY           Encryption key (optional)\n"
              << "  -c, --compression NUM   Zstd compression level 0-" << kMaxCompression
              << ", 0 for none (default: " << kDefaultCompression << ")\n"
              << "      --type KIND         'full' (default) copies every block the export does\n"
              << "                          not report as zero; 'incremental' copies only the\n"
              << "                          blocks of the dirty bitmap named by --change-id.\n"
              << "                          Incremental needs --nbd.\n"
              << "      --change-id BITMAP  QEMU dirty bitmap an incremental reads\n"
              << "      --immutable MODE    Hold the backup in 'governance' or 'compliance'\n"
              << "                          mode, so that it cannot be deleted or overwritten\n"
              << "                          before its retention expires (S3 only)\n"
              << "      --retention DAYS    How long --immutable holds the backup, in days\n"
              << "      --retention-till DATE\n"
              << "                          Hold it until DATE instead, in UTC as\n"
              << "                          YYYY-MM-DDTHH:MM:SSZ; one of the two is required\n\n";
    printStorageOptions();
    std::cout << "\n";
    printNbdOptions(false);
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
              << "  -t, --target PATH       Target file or device to restore to (required)\n"
              << "  -k, --key KEY           Decryption key (optional)\n"
              << "  -f, --force             Apply an incremental backup, or a chain that\n"
              << "                          starts with one, onto a target that already\n"
              << "                          holds its parent\n\n";
    printStorageOptions();
    std::cout << "\n";
    printNbdOptions(true);
}

// A whole number of MiB/s, at least 1.
bool parseBwlimit(const std::string& text, uint64_t& value)
{
    value = 0;
    if (!text.empty() && text.find_first_not_of("0123456789") == std::string::npos && text.size() <= 9) {
        value = std::stoull(text);
    }
    if (value == 0) {
        std::cerr << "Error: --bwlimit must be a positive number of MiB/s\n";
        return false;
    }
    return true;
}

bool setToolOption(Options& options, int value, const std::string& argument)
{
    switch (value) {
        case kOptNbd:
            options.nbd = argument;
            return true;
        case kOptExport:
            options.exportName = argument;
            return true;
        case kOptBwlimit:
            return parseBwlimit(argument, options.bwlimit);
        case kOptDiscard:
            options.discard = true;
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

// Handed to Tool::Context for the commands common/ runs, which read the shared half of the options only.
// A bad value in this tool's half is still an error there.
bool parseCommonOptions(int argc, char* argv[], Tool::CommonOptions& shared)
{
    Options options;

    if (!parseOptions(argc, argv, options)) {
        return false;
    }

    shared = options;
    return true;
}

// A backup reads either --source or --nbd, and the other NBD options go with --nbd.
bool checkBackupOptions(const Options& options)
{
    if (!options.source.empty() && !options.nbd.empty()) {
        std::cerr << "Error: --source and --nbd name the same disk; give one\n";
        return false;
    }
    if (options.nbd.empty() && options.anyNbdOption()) {
        std::cerr << "Error: --export, --bwlimit and --discard go with --nbd\n";
        return false;
    }
    return true;
}

// A restore writes a plain file or device: a PVE restore reads its chain through nbdkit, not through this tool.
bool checkRestoreOptions(const Options& options)
{
    if (options.anyNbdOption()) {
        std::cerr << "Error: --nbd, --export, --bwlimit and --discard are for backups; a restore writes --target\n";
        return false;
    }
    if (options.target.empty()) {
        std::cerr << "Error: restore requires --target\n";
        return false;
    }
    return true;
}

// What the export says about itself travels with its reader, for 'backup' to report next to the data it read.
struct NbdSource : Tool::DiskSource {
    // The reader below, for its properties and how much it discarded; null for a plain file or device.
    const Nbd::DiskReader* nbd = nullptr;
    std::string exportName;
    bool discard = false;

    std::string description() const override
    {
        if (!nbd) {
            return {};
        }
        const std::string& context = nbd->properties().context;
        return (exportName.empty() ? std::string("default export") : "export " + exportName) + ", "
             + (context.empty() ? std::string("no allocation status") : context);
    }

    void addDetails(Tool::JsonObject& json) const override
    {
        if (!nbd) {
            return;
        }
        json.addString("export", exportName);
        json.addString("block_status", nbd->properties().context);
        if (discard) {
            json.addNumber("discarded_bytes", nbd->discardedBytes());
        }
    }

    void printDetails() const override
    {
        if (nbd && discard) {
            std::cout << "Discarded:         " << nbd->discardedBytes() << " bytes\n";
        }
    }
};

// This tool's half of 'backup' and 'restore'; the rest of both is common/'s.
class NbdTool : public Tool::DiskTool
{
public:
    bool parseOptions(int argc, char* argv[]) override { return ::parseOptions(argc, argv, m_options); }
    const Tool::CommonOptions& options() const override { return m_options; }

    void printBackupHelp(const std::string& programName) const override { ::printBackupHelp(programName); }
    void printRestoreHelp(const std::string& programName) const override { ::printRestoreHelp(programName); }

    bool checkOptions(bool forRestore) const override
    {
        return forRestore ? checkRestoreOptions(m_options) : checkBackupOptions(m_options);
    }

    const char* diskOption() const override { return "--nbd"; }
    const std::string& disk() const override { return m_options.nbd; }

    std::unique_ptr<Tool::DiskSource> openSource() override
    {
        auto pSource = std::make_unique<NbdSource>();

        if (m_options.nbd.empty()) {
            pSource->reader = openFileReader(m_options.source);
        } else {
            Nbd::ReaderParams params;
            params.server = m_options.nbd;
            params.exportName = m_options.exportName;
            params.bitmap = m_options.incremental() ? m_options.changeId : std::string();
            params.bytesPerSecond = m_options.bwlimit * 1024 * 1024;
            params.discard = m_options.discard;

            std::unique_ptr<Nbd::DiskReader> pReader = Nbd::openReader(params);
            pSource->nbd = pReader.get();
            pSource->exportName = m_options.exportName;
            pSource->discard = m_options.discard;
            pSource->reader = std::move(pReader);
        }
        return pSource->reader ? std::move(pSource) : nullptr;
    }

    std::unique_ptr<Tool::DiskTarget> openTarget(bool keepExisting, const DiskGeometry& image) override
    {
        (void)image;
        auto pTarget = std::make_unique<Tool::DiskTarget>();
        pTarget->writer = openFileWriter(m_options.target, keepExisting);
        return pTarget->writer ? std::move(pTarget) : nullptr;
    }

private:
    Options m_options;
};

// The command table. Every command is common/'s; 'backup' and 'restore' reach this tool's disks through NbdTool
// above, by way of Tool::Context.
int dispatch(const std::string& programName, const std::string& command, int argc, char* argv[])
{
    if (command == "backup") {
        return Tool::runBackup(programName, argc, argv);
    } else if (command == "restore") {
        return Tool::runRestore(programName, argc, argv);
    } else if (command == "copy") {
        return Tool::runCopy(programName, argc, argv);
    } else if (command == "empty") {
        return Tool::runEmpty(programName, argc, argv);
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
                                       [] { return std::make_unique<NbdTool>(); }};

    return Tool::run(argc, argv, context, dispatch);
}
