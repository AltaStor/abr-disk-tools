// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor VMDK backup utility
//

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "abr_backup.h"

// Abr::immutabilityModeName lives here, and runBackup/runRestore report it.
#include "s3_backup.h"

#include "cli_options.h"
#include "disk_device.h"
#include "tool_backup.h"
#include "tool_commands.h"
#include "tool_help.h"
#include "tool_json.h"
#include "tool_options.h"
#include "vddk_disk.h"
#include "version.h"

// Everything common/ shares between the tools is used here by its plain name,
// as it was when it lived in this file.
using namespace Tool;

// sectors, VDDK allocated-block granularity
constexpr uint64_t kDefaultQueryChunk = 2048;

// The few words common/ has to put in this tool's mouth.
const Tool::Branding kBranding = {
    "abr-vmdk-tool",
    ABR_DISK_TOOLS_VERSION,
    "VMDK",
    "releasing the disk and the VM",
};

// This tool's half of the option table. Adding one of its options means
// a value here, a row in kToolLongOptions below, and a case in setToolOption --
// the same three places a shared option needs, over in common/.
enum LongOnlyOption {
    kOptVmdk = Tool::kOptToolFirst,
    kOptHost,
    kOptUser,
    kOptPassword,
    kOptThumbprint,
    kOptVm,
    kOptPort,
    kOptNfcPort,
    kOptTransport,
    kOptSnapshot,
    kOptQueryChunk,
};

const Cli::LongOption kToolLongOptions[] = {
    {"vmdk",        Cli::Argument::Required, kOptVmdk},
    {"host",        Cli::Argument::Required, kOptHost},
    {"user",        Cli::Argument::Required, kOptUser},
    {"password",    Cli::Argument::Required, kOptPassword},
    {"thumbprint",  Cli::Argument::Required, kOptThumbprint},
    {"vm",          Cli::Argument::Required, kOptVm},
    {"port",        Cli::Argument::Required, kOptPort},
    {"nfc-port",    Cli::Argument::Required, kOptNfcPort},
    {"transport",   Cli::Argument::Required, kOptTransport},
    {"snapshot",    Cli::Argument::Required, kOptSnapshot},
    {"query-chunk", Cli::Argument::Required, kOptQueryChunk},
    {nullptr,       Cli::Argument::None,     0},
};

struct Options : Tool::CommonOptions {
    Vddk::ConnectParams vddk;
    std::string vmdk;
    uint64_t queryChunk = kDefaultQueryChunk;
};

// A VDDK-backed disk only stays valid while its session is alive, so both are
// handed out together.
struct DiskSource {
    std::unique_ptr<Vddk::Session> session;
    std::unique_ptr<IDiskReader> reader;
};

struct DiskTarget {
    std::unique_ptr<Vddk::Session> session;
    std::unique_ptr<IDiskWriter> writer;
};

void printVmdkOptions()
{
    std::cout << "VMDK (VDDK) options:\n"
              << "      --vmdk PATH         VMDK to read/write instead of a local file\n"
              << "      --host HOST         ESXi/vCenter host; omit for a local VMDK file\n"
              << "      --user NAME         User name on the host\n"
              << "      --password PASS     Password on the host; password= with\n"
              << "                          --stdin-keys keeps it off the command line\n"
              << "      --vm SPEC           VM spec (like \"vm-XXXX\")\n"
              << "      --thumbprint PRINT  SSL certificate thumbprint of the host\n"
              << "      --port PORT         Host port (default: 443)\n"
              << "      --nfc-port PORT     NFC port on the ESXi host (default: 902)\n"
              << "      --transport MODES   Transport modes, e.g. \"nbdssl:nbd:hotadd:san\"\n"
              << "      --snapshot MOREF    Managed object reference of the VM snapshot\n";
}

void printBackupHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " backup [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name (required)\n"
              << "  -s, --source PATH       Source file to back up (required unless --vmdk)\n"
              << "  -k, --key KEY           Encryption key (optional)\n"
              << "  -c, --compression NUM   Zstd compression level 0-" << kMaxCompression
              << ", 0 for none (default: " << kDefaultCompression << ")\n"
              << "      --type KIND         'full' (default) copies every allocated block;\n"
              << "                          'incremental' copies only the blocks changed\n"
              << "                          since --change-id. Incremental needs --vmdk,\n"
              << "                          since it relies on VMware changed block tracking.\n"
              << "      --change-id ID      Change id an incremental is taken against\n"
              << "      --query-chunk N     Allocated-block query granularity in sectors (default: "
              << kDefaultQueryChunk << ")\n"
              << "      --immutable MODE    Hold the backup in 'governance' or 'compliance'\n"
              << "                          mode, so that it cannot be deleted or overwritten\n"
              << "                          before its retention expires (S3 only)\n"
              << "      --retention DAYS    How long --immutable holds the backup, in days\n\n";
    printStorageOptions();
    std::cout << "\n";
    printVmdkOptions();
}

void printRestoreHelp(const std::string& programName)
{
    std::cout << "Usage: " << programName << " restore [options]\n\n"
              << "Options:\n"
              << "  -h, --help              Show this help message\n"
              << "      --json              Print the command result to stdout as JSON\n"
              << "  -n, --name NAME         Backup name (required)\n"
              << "  -t, --target PATH       Target file to restore to (required unless --vmdk)\n"
              << "  -k, --key KEY           Decryption key (optional)\n"
              << "  -f, --force             Apply an incremental backup onto a target that\n"
              << "                          already holds its parent\n\n";
    printStorageOptions();
    std::cout << "\n";
    printVmdkOptions();
}

// A whole, non-negative number no greater than max. Digits only, so that "3x"
// or "-1" is refused rather than read as 3 or wrapped round -- and so that no
// std::sto* call throws out of the option parser and takes the process down
// with it.
bool parseNumber(const char* option, const std::string& text, uint64_t max, uint64_t& value)
{
    bool valid = !text.empty() && text.find_first_not_of("0123456789") == std::string::npos;

    if (valid) {
        try {
            value = std::stoull(text);
        } catch (const std::exception&) {
            valid = false;
        }
    }

    if (!valid || value > max) {
        std::cerr << "Error: " << option << " must be a whole number";
        if (max != std::numeric_limits<uint64_t>::max()) {
            std::cerr << " from 0 to " << max;
        }
        std::cerr << "\n";
        return false;
    }
    return true;
}

bool setToolOption(Options& options, int value, const std::string& argument)
{
    uint64_t number = 0;

    switch (value) {
        case kOptVmdk: options.vmdk = argument; break;
        case kOptHost: options.vddk.host = argument; break;
        case kOptUser: options.vddk.user = argument; break;
        case kOptPassword: options.vddk.password = argument; break;
        case kOptThumbprint: options.vddk.thumbPrint = argument; break;
        case kOptVm: options.vddk.vmxSpec = argument; break;
        case kOptPort:
            if (!parseNumber("--port", argument, 65535, number)) {
                return false;
            }
            options.vddk.port = static_cast<uint32_t>(number);
            break;
        case kOptNfcPort:
            if (!parseNumber("--nfc-port", argument, 65535, number)) {
                return false;
            }
            options.vddk.nfcHostPort = static_cast<uint32_t>(number);
            break;
        case kOptTransport: options.vddk.transportModes = argument; break;
        case kOptSnapshot: options.vddk.snapshotRef = argument; break;
        case kOptQueryChunk:
            if (!parseNumber("--query-chunk", argument,
                             std::numeric_limits<uint64_t>::max(), number)) {
                return false;
            }
            options.queryChunk = number;
            break;

        default: return false; // unreachable: the two value ranges are disjoint
    }
    return true;
}

bool parseOptions(int argc, char* argv[], Options& options)
{
    static const Tool::OptionTable table(kToolLongOptions);

    // The password of a vCenter administrator, as often as not: the one secret
    // of this tool's own, and as exposed on the command line as the key.
    return Tool::parseSharedOptions(argc, argv, table.rows(), options,
        [&options](int value, const std::string& argument) {
            return setToolOption(options, value, argument);
        },
        {{"password", &options.vddk.password}});
}

// Handed to Tool::Context for the commands common/ runs. Not called parseSharedOptions:
// that is the library's own entry point, and under the using-directive above two functions
// of that name would be one overload set.
//
// The commands want the shared half only, which a full parse yields as its
// base -- and the slice is on purpose: nothing in those commands reads this
// tool's half, but a bad value in it is still an error, as it has always been.
//
bool parseCommonOptions(int argc, char* argv[], Tool::CommonOptions& shared)
{
    Options options;

    if (!parseOptions(argc, argv, options)) {
        return false;
    }
    shared = options;
    return true;
}

bool checkVmdkOptions(const Options& options)
{
    if (options.vmdk.empty()) {
        return true;
    }
    if (options.vddk.host.empty()) {
        return true; // local VMDK file, no credentials needed
    }
    if (options.vddk.user.empty() || options.vddk.password.empty()) {
        std::cerr << "Error: --host requires --user and --password (or password= with --stdin-keys)\n";
        return false;
    }
    if (options.vddk.vmxSpec.empty()) {
        std::cerr << "Error: --host requires --vm\n";
        return false;
    }
    return true;
}

bool openSource(const Options& options, DiskSource& source)
{
    if (options.vmdk.empty()) {
        source.reader = openFileReader(options.source);
        return source.reader != nullptr;
    }

    source.session = std::make_unique<Vddk::Session>();
    if (!source.session->connect(options.vddk, true)) {
        return false;
    }

    source.reader = source.session->openReader(options.vmdk, options.changeId, options.queryChunk);

    return source.reader != nullptr;
}

bool openTarget(const Options& options, DiskTarget& target, bool keepExisting)
{
    if (options.vmdk.empty()) {
        target.writer = openFileWriter(options.target, keepExisting);
        return target.writer != nullptr;
    }

    target.session = std::make_unique<Vddk::Session>();
    if (!target.session->connect(options.vddk, false)) {
        return false;
    }

    target.writer = target.session->openWriter(options.vmdk);

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
    if (!checkStorageOptions(options.storage) || !checkVmdkOptions(options)
        || !checkBackupType(options, "--vmdk", !options.vmdk.empty()) || !checkImmutabilityOptions(options)) {
        return 1;
    }
    if (options.storage.name.empty() || (options.source.empty() && options.vmdk.empty())) {
        std::cerr << "Error: backup requires --name and either --source or --vmdk\n";
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

    if (options.json) {
        JsonObject json("backup", "ok");
        json.addString("source", options.vmdk.empty() ? options.source : options.vmdk);
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        json.addString("kind", options.incremental() ? "incremental" : "full");
        if (options.incremental()) {
            json.addString("change_id", options.changeId);
        }
        json.addString("immutability", Abr::immutabilityModeName(options.immutability));
        if (options.immutability != Abr::ImmutabilityMode::NONE) {
            json.addNumber("retention_days", static_cast<uint64_t>(options.retentionDays));
        }
        addBackupInfo(json, info);
    } else {
        std::cout << "Source:            " << (options.vmdk.empty() ? options.source : options.vmdk) << "\n"
                  << "Backup:            " << backupLocation(options.storage) << "\n"
                  << "Kind:              "
                  << (options.incremental() ? "incremental (changed since " + options.changeId + ")"
                                            : std::string("full")) << "\n"
                  << "Raw data size:     " << info.orig_data_size << " bytes\n"
                  << "Compression:       " << (info.compression ?
                            ("zstd level " + std::to_string(info.compression)) : std::string("none")) << "\n";

        if (info.compression && info.orig_data_size > 0) {
            std::cout << "Stored data size:  " << info.stored_data_size << " bytes\n"
                << "Compression ratio: " << static_cast<double>(info.stored_data_size) / info.orig_data_size << "\n";
        }
        std::cout << "Encryption:        " << (info.encryption ? "AES-256-GCM" : "none") << "\n"
                  << "Immutability:      "
                  << (options.immutability == Abr::ImmutabilityMode::NONE
                          ? std::string("none")
                          : std::string(Abr::immutabilityModeName(options.immutability))
                                + ", retained for " + std::to_string(options.retentionDays) + " day(s)") << "\n";
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
    if (!checkStorageOptions(options.storage) || !checkVmdkOptions(options)) {
        return 1;
    }
    if (options.storage.name.empty() || (options.target.empty() && options.vmdk.empty())) {
        std::cerr << "Error: restore requires --name and either --target or --vmdk\n";
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
    // by itself leaves every other block untouched -- an empty disk if the target
    // started empty. Applying one onto a target that already carries the full
    // restore is legitimate, and that is what --force says.
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

    DiskTarget target;
    if (!openTarget(options, target, info.incremental != 0)) {
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
    if (!ok) {
        return 1;
    }

    const std::string restoreTarget = options.vmdk.empty() ? options.target : options.vmdk;

    if (options.json) {
        JsonObject json("restore", "ok");
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        json.addString("target", restoreTarget);
        json.addString("kind", incremental ? "incremental" : "full");
        json.addBool("decrypted", !options.storage.key.empty());
    } else {
        std::cout << (incremental ? "Applied incremental '" : "Restored '")
                  << backupLocation(options.storage) << "' to '" << restoreTarget << "'"
                  << (options.storage.key.empty() ? "" : " (decrypted)") << "\n";
    }
    return 0;
}

// The command table. Everything but 'backup' and 'restore' is common/'s,
// since only those two touch the VDDK. 
// Adding a command means a branch here, a // print<Cmd>Help, and -- if it is one
// every tool shares -- the run<Cmd> in common/ rather than above.
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

    const int result = Tool::run(argc, argv, context, dispatch);

    // FIXME: The exit status is the one verdict every caller reads, so it must not be left to exit().
    // Once the VDDK has been loaded, libvmacore has an atexit handler in the process that
    // ends it with Vmacore::System::QuickExit(0) (a hard-coded 0, whatever main returned) 
    // which turned every backup or restore that failed after opening its disk, a CRC32C
    // mismatch included, into a success for the caller.
    // Leaving through _exit() skips that handler, and with it the static destructors and
    // atexit handlers that QuickExit skips anyway; every session, manager and the AWS SDK are
    // torn down inside Tool::run by then. What _exit() does not do is flush, so the
    // JSON document and the last diagnostics are pushed out first.
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    _exit(result);
}
