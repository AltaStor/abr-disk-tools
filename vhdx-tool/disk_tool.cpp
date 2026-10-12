// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor VHDX backup utility
//

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include "abr_backup.h"
#include "s3_backup.h"

#include "cli_options.h"
#include "disk_device.h"
#include "plat.h"
#include "tool_json.h"
#include "tool_backup.h"
#include "tool_commands.h"
#include "tool_help.h"
#include "tool_options.h"
#include "vhdx_disk.h"
#include "version.h"
#include "win_util.h"

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

// This tool's half of the option table. Adding one of its options means a
// value here, a row in kToolLongOptions below, and a case in setToolOption --
// the same three places a shared option needs, over in common/.
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

// What a virtual disk says about itself travels with its reader, for 'backup'
// to report next to the data it read.
struct DiskSource {
    std::unique_ptr<IDiskReader> reader;
    Vhdx::DiskProperties properties;
};

struct DiskTarget {
    std::unique_ptr<IDiskWriter> writer;
    bool created = false;
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
              << "      --retention DAYS    How long --immutable holds the backup, in days\n\n";
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
              << "  -n, --name NAME         Backup name (required)\n"
              << "  -t, --target PATH       Target file, disk or volume to restore to\n"
              << "                          (required unless --vhdx)\n"
              << "  -k, --key KEY           Decryption key (optional)\n"
              << "  -f, --force             Apply an incremental backup onto a target that\n"
              << "                          already holds its parent\n\n";
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

// Handed to Tool::Context for the commands common/ runs. Not called
// parseSharedOptions: that is the library's own entry point, and under the
// using-directive above two functions of that name would be one overload set.
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

// --vhdx takes the place of --source or --target; naming both would leave the
// tool to guess which disk was meant.
bool checkDiskOptions(const std::string& fileOption, const std::string& file, const Options& options)
{
    if (!file.empty() && !options.vhdx.empty()) {
        std::cerr << "Error: " << fileOption << " and --vhdx name the same disk; give one\n";
        return false;
    }
    return true;
}

bool openSource(const Options& options, DiskSource& source)
{
    if (options.vhdx.empty()) {
        source.reader = openFileReader(options.source);
    } else {
        source.reader = Vhdx::openReader(options.vhdx, options.changeId, source.properties);
    }
    return source.reader != nullptr;
}

// image is the geometry of the backup, which a virtual disk that is not there
// yet is created with.
bool openTarget(const Options& options, DiskTarget& target, bool keepExisting,
                const DiskGeometry& image)
{
    if (options.vhdx.empty()) {
        target.writer = openFileWriter(options.target, keepExisting);
    } else {
        target.writer = Vhdx::openWriter(options.vhdx, keepExisting, image, target.created);
    }
    return target.writer != nullptr;
}

int runBackup(const std::string& programName, int argc, char* argv[])
{
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 1;
    }
    if (options.help) {
        printBackupHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage) || !checkDiskOptions("--source", options.source, options)
        || !checkBackupType(options, "--vhdx", !options.vhdx.empty())
        || !checkImmutabilityOptions(options)) {
        return 1;
    }
    if (options.storage.name.empty() || (options.source.empty() && options.vhdx.empty())) {
        std::cerr << "Error: backup requires --name and either --source or --vhdx\n";
        printBackupHelp(programName);
        return 1;
    }

    DiskSource source;
    if (!openSource(options, source)) {
        return 1;
    }

    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    const DiskGeometry& geometry = source.reader->geometry();

    Abr::BackupInfo info = {};
    info.sectorSize = static_cast<uint16_t>(geometry.sectorSize);
    info.phySectorSize = static_cast<uint16_t>(geometry.phySectorSize);
    info.sectors = geometry.sectors;
    info.compression = static_cast<uint8_t>(options.storage.compression);
    info.encryption = options.storage.key.empty() ? 0 : 1;
    info.incremental = options.incremental() ? 1 : 0;
    info.m_immutabilityMode = options.immutability;
    info.m_immutabilityDays = options.retentionDays;

    std::shared_ptr<Abr::IBackup> pBackup;
    if (!pManager->createBackup(options.storage.path, options.storage.name, info, pBackup,
                                options.storage.key)) {
        std::cerr << "Error: failed to create backup '" << options.storage.name << "'\n";
        return 1;
    }

    bool ok = copyToBackup(*source.reader, *pBackup,
                           alignBufferSize(kDefaultChunkSize, geometry.sectorSize), options.json);

    info = pBackup->getInfo();

    // On failure this discards everything already written, so a failed run leaves
    // nothing of the backup behind in the repository.
    if (!pBackup->finalizeBackup(!ok)) {
        if (ok) {
            std::cerr << "Error: failed to finalize backup '" << options.storage.name << "'\n";
            ok = false;
        } else {
            std::cerr << "Error: the partial backup '" << backupLocation(options.storage)
                      << "' could not be removed, delete it with the 'remove' command\n";
        }
    }

    if (!ok) {
        return 1;
    }

    const std::string sourcePath = options.vhdx.empty() ? options.source : options.vhdx;
    const Vhdx::DiskProperties& disk = source.properties;

    if (options.json) {
        JsonObject json("backup", "ok");
        json.addString("source", sourcePath);
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        json.addString("kind", options.incremental() ? "incremental" : "full");
        if (options.incremental()) {
            json.addString("change_id", options.changeId);
        }
        // What the next incremental of this disk is taken against.
        if (!disk.changeTrackingId.empty()) {
            json.addString("rct_id", disk.changeTrackingId);
        }
        json.addString("immutability", Abr::immutabilityModeName(options.immutability));
        if (options.immutability != Abr::ImmutabilityMode::NONE) {
            json.addNumber("retention_days", static_cast<uint64_t>(options.retentionDays));
        }
        addBackupInfo(json, info);
    } else {
        std::string sourceKind;
        if (!disk.format.empty()) {
            sourceKind = " (" + disk.format + (disk.type.empty() ? "" : ", " + disk.type) + ")";
        }

        std::cout << "Source:            " << sourcePath << sourceKind << "\n"
                  << "Backup:            " << backupLocation(options.storage) << "\n"
                  << "Kind:              "
                  << (options.incremental() ? "incremental (changed since " + options.changeId + ")"
                                            : std::string("full")) << "\n";
        if (!disk.changeTrackingId.empty()) {
            std::cout << "RCT id:            " << disk.changeTrackingId << "\n";
        }
        std::cout << "Raw data size:     " << info.orig_data_size << " bytes\n"
                  << "Compression:       " << (info.compression ? ("zstd level " + std::to_string(info.compression)) : std::string("none")) << "\n";
        if (info.compression && info.orig_data_size > 0) {
            std::cout << "Stored data size:  " << info.stored_data_size << " bytes\n"
                << "Compression ratio: " << static_cast<double>(info.stored_data_size) / info.orig_data_size << "\n";
        }
        std::cout << "Encryption:        " << (info.encryption ? "AES-256-GCM" : "none") << "\n"
                  << "Immutability:      "
                  << (options.immutability == Abr::ImmutabilityMode::NONE
                          ? std::string("none")
                          : std::string(Abr::immutabilityModeName(options.immutability))
                                + ", retained for " + std::to_string(options.retentionDays) + " day(s)")
                  << "\n";
    }
    return 0;
}

int runRestore(const std::string& programName, int argc, char* argv[])
{
    Options options;
    if (!parseOptions(argc, argv, options)) {
        return 1;
    }
    if (options.help) {
        printRestoreHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage) || !checkDiskOptions("--target", options.target, options)) {
        return 1;
    }
    if (options.storage.name.empty() || (options.target.empty() && options.vhdx.empty())) {
        std::cerr << "Error: restore requires --name and either --target or --vhdx\n";
        printRestoreHelp(programName);
        return 1;
    }

    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    std::shared_ptr<Abr::IRestore> pRestore;
    if (!pManager->openBackup(options.storage.path, options.storage.name, pRestore,
                              options.storage.key)) {
        std::cerr << "Error: failed to open backup '" << options.storage.name << "'\n";
        return 1;
    }

    const Abr::BackupInfo& info = pRestore->getInfo();

    // An incremental holds only what changed since its parent, so restoring it
    // by itself leaves every other block untouched -- an empty disk if the
    // target started empty. Applying one onto a target that already carries the
    // full restore is legitimate, and that is what --force says.
    if (info.incremental && !options.force) {
        std::cerr << "Error: '" << options.storage.name
                  << "' is an incremental backup and holds only the blocks that changed since\n"
                  << "       its parent. Restore the full backup to the target first, then apply\n"
                  << "       this one to the same target with --force.\n";
        pRestore->close();
        return 1;
    }

    if (!checkKey(*pRestore, options.storage.name, options.storage.key)) {
        pRestore->close();
        return 1;
    }

    DiskGeometry image;
    image.sectors = info.sectors;
    image.sectorSize = info.sectorSize;
    image.phySectorSize = info.phySectorSize;

    DiskTarget target;
    if (!openTarget(options, target, info.incremental != 0, image)) {
        pRestore->close();
        return 1;
    }

    const DiskGeometry& geometry = target.writer->geometry();

    if (geometry.sectors != 0) {
        if (info.sectorSize != geometry.sectorSize) {
            std::cerr << "Error: backup sector size " << info.sectorSize
                      << " does not match target sector size " << geometry.sectorSize << "\n";
            pRestore->close();
            return 1;
        }
        if (info.sectors != geometry.sectors) {
            std::cerr << "Error: backup holds " << info.sectors << " sectors but the target has "
                      << geometry.sectors << "\n";
            pRestore->close();
            return 1;
        }
    }

    if (!target.writer->setImageSize(info.sectors * info.sectorSize)) {
        pRestore->close();
        return 1;
    }

    uint32_t bufferSize = info.maxDataBlock ? info.maxDataBlock : kDefaultChunkSize;
    // Taken before close(), which is what invalidates the info reference.
    const bool incremental = info.incremental != 0;
    bool ok = copyFromBackup(*pRestore, *target.writer,
                             alignBufferSize(bufferSize, geometry.sectorSize), options.json);

    pRestore->close();

    // Released here rather than at the end of the scope, so that a virtual disk
    // this restore created and did not finish is gone before the failure is
    // reported.
    target.writer.reset();

    if (!ok) {
        return 1;
    }

    const std::string restoreTarget = options.vhdx.empty() ? options.target : options.vhdx;

    if (options.json) {
        JsonObject json("restore", "ok");
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        json.addString("target", restoreTarget);
        json.addString("kind", incremental ? "incremental" : "full");
        if (!options.vhdx.empty()) {
            json.addBool("created", target.created);
        }
        json.addBool("decrypted", !options.storage.key.empty());
    } else {
        std::cout << (incremental ? "Applied incremental '" : "Restored '")
                  << backupLocation(options.storage) << "' to "
                  << (target.created ? "new virtual disk '" : "'") << restoreTarget << "'"
                  << (options.storage.key.empty() ? "" : " (decrypted)") << "\n";
    }
    return 0;
}

//
// The command table. Everything but 'backup' and 'restore' is common/'s,
// since only those two touch the virtual disk layer.
// Adding a command means a branch here, a print<Cmd>Help, and -- if it is one
// every tool shares -- the run<Cmd> in common/ rather than below.
//
int dispatch(const std::string& programName, const std::string& command, int argc, char* argv[])
{
    if (command == "backup") {
        return runBackup(programName, argc, argv);
    } else if (command == "restore") {
        return runRestore(programName, argc, argv);
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
    static const Tool::Context context{kBranding, parseCommonOptions};

    return Tool::run(argc, argv, context, dispatch);
}
