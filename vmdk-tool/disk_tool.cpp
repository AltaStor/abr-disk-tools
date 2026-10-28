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

#include <unistd.h>

#include "cli_options.h"
#include "disk_device.h"
#include "tool_commands.h"
#include "tool_disk.h"
#include "tool_help.h"
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
              << "      --retention DAYS    How long --immutable holds the backup, in days\n"
              << "      --retention-till DATE\n"
              << "                          Hold it until DATE instead, in UTC as\n"
              << "                          YYYY-MM-DDTHH:MM:SSZ; one of the two is required\n\n";
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
              << "  -n, --name NAME         Backup name (required unless --source-names)\n"
              << "      --source-names LIST Comma-separated chain to restore at once in place\n"
              << "                          of --name: the backup to start from first, then\n"
              << "                          the incrementals on top of it in the order they\n"
              << "                          were taken. Each block is written once, as the\n"
              << "                          last backup that changed it holds it\n"
              << "  -t, --target PATH       Target file to restore to (required unless --vmdk)\n"
              << "  -k, --key KEY           Decryption key (optional)\n"
              << "  -f, --force             Apply an incremental backup, or a chain that\n"
              << "                          starts with one, onto a target that already\n"
              << "                          holds its parent\n\n";
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

// This tool's half of 'backup' and 'restore'; the rest of both is common/'s.
// A VDDK-backed disk only stays valid while its session is alive, so both are
// handed out together.
class VmdkTool : public Tool::DiskTool
{
public:
    bool parseOptions(int argc, char* argv[]) override { return ::parseOptions(argc, argv, m_options); }
    const Tool::CommonOptions& options() const override { return m_options; }

    void printBackupHelp(const std::string& programName) const override { ::printBackupHelp(programName); }
    void printRestoreHelp(const std::string& programName) const override { ::printRestoreHelp(programName); }

    bool checkOptions(bool) const override { return checkVmdkOptions(m_options); }

    const char* diskOption() const override { return "--vmdk"; }
    const std::string& disk() const override { return m_options.vmdk; }

    std::unique_ptr<Tool::DiskSource> openSource() override
    {
        auto pSource = std::make_unique<Tool::DiskSource>();

        if (m_options.vmdk.empty()) {
            pSource->reader = openFileReader(m_options.source);
        } else {
            auto pSession = std::make_shared<Vddk::Session>();
            pSource->session = pSession;
            if (!pSession->connect(m_options.vddk, true)) {
                return nullptr;
            }
            pSource->reader = pSession->openReader(m_options.vmdk, m_options.changeId,
                                                   m_options.queryChunk);
        }
        return pSource->reader ? std::move(pSource) : nullptr;
    }

    // A VMDK is never created here, so the geometry of the backup goes unused.
    std::unique_ptr<Tool::DiskTarget> openTarget(bool keepExisting, const DiskGeometry&) override
    {
        auto pTarget = std::make_unique<Tool::DiskTarget>();

        if (m_options.vmdk.empty()) {
            pTarget->writer = openFileWriter(m_options.target, keepExisting);
        } else {
            auto pSession = std::make_shared<Vddk::Session>();
            pTarget->session = pSession;
            if (!pSession->connect(m_options.vddk, false)) {
                return nullptr;
            }
            pTarget->writer = pSession->openWriter(m_options.vmdk);
        }
        return pTarget->writer ? std::move(pTarget) : nullptr;
    }

private:
    Options m_options;
};

// The command table. Every command is common/'s; 'backup' and 'restore' reach
// this tool's disks through VmdkTool above, by way of Tool::Context.
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
                                       [] { return std::make_unique<VmdkTool>(); }};

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
