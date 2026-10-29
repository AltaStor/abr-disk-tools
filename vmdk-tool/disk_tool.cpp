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
#include <optional>
#include <string>

#include <unistd.h>

#include "cli_options.h"
#include "disk_device.h"
#include "tool_commands.h"
#include "tool_disk.h"
#include "tool_help.h"
#include "soap_disk.h"
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
    kOptApi,
    kOptCluster,
    kOptResourcePool,
    kOptQuiesce,
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
    {"api",         Cli::Argument::Required, kOptApi},
    {"cluster",     Cli::Argument::Required, kOptCluster},
    {"resource-pool", Cli::Argument::Required, kOptResourcePool},
    {"quiesce",     Cli::Argument::None,     kOptQuiesce},
    {nullptr,       Cli::Argument::None,     0},
};

// How a VMware disk is reached: VMware's own VDDK, or the vSphere SOAP API through vsphere-soap-lib.
// The default is the VDDK wherever the build has it.
enum class Api { Vddk, Soap };

#ifdef ABR_DISK_HAVE_VDDK
constexpr Api kDefaultApi = Api::Vddk;
#else
constexpr Api kDefaultApi = Api::Soap;
#endif

struct Options : Tool::CommonOptions {
    Vddk::ConnectParams vddk;
    std::string vmdk;
    Api api = kDefaultApi;
    std::optional<uint64_t> queryChunk;

    // SOAP only: where a new disk's temporary VM runs, and a quiesced snapshot of a running VM.
    std::string cluster;
    std::string resourcePool;
    bool quiesce = false;
};

void printVmdkOptions()
{
    std::cout << "VMDK options (VDDK or SOAP):\n"
              << "      --vmdk PATH         VMDK to read/write instead of a local file\n"
              << "      --api API           'vddk' or 'soap', how the disk is reached (default: "
              << (kDefaultApi == Api::Vddk ? "vddk" : "soap") << ")\n"
              << "      --host HOST         ESXi/vCenter host; omit for a local VMDK file\n"
              << "      --user NAME         User name on the host\n"
              << "      --password PASS     Password on the host; password= with\n"
              << "                          --stdin-keys keeps it off the command line\n"
              << "      --vm SPEC           VM spec (like \"vm-XXXX\")\n"
              << "      --thumbprint PRINT  SSL certificate thumbprint of the host\n"
              << "      --port PORT         Host port (default: 443)\n"
              << "      --snapshot MOREF    Managed object reference of the VM snapshot\n"
              << "  VDDK only:\n"
              << "      --nfc-port PORT     NFC port on the ESXi host (default: 902)\n"
              << "      --transport MODES   Transport modes, e.g. \"nbdssl:nbd:hotadd:san\"\n"
              << "  SOAP only:\n"
              << "      --quiesce           Quiesce the guest when snapshotting a running VM\n"
              << "      --cluster NAME      Cluster or host a new disk is uploaded through\n"
              << "      --resource-pool NAME\n"
              << "                          Resource pool for the same, by name\n";
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
        case kOptApi:
            if (argument == "vddk") {
#ifndef ABR_DISK_HAVE_VDDK
                std::cerr << "Error: this abr-vmdk-tool is built without the VDDK; use --api soap\n";
                return false;
#endif
                options.api = Api::Vddk;
            } else if (argument == "soap") {
#ifndef ABR_DISK_HAVE_SOAP
                std::cerr << "Error: this abr-vmdk-tool is built without the SOAP API; use --api vddk\n";
                return false;
#endif
                options.api = Api::Soap;
            } else {
                std::cerr << "Error: --api must be 'vddk' or 'soap'\n";
                return false;
            }
            break;
        case kOptCluster: options.cluster = argument; break;
        case kOptResourcePool: options.resourcePool = argument; break;
        case kOptQuiesce: options.quiesce = true; break;

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

// Options that only one of the two APIs takes, refused with the other rather than ignored.
bool checkApiOptions(const Options& options)
{
    const char* unused = nullptr;
    if (options.api == Api::Soap) {
        if (!options.vddk.transportModes.empty()) {
            unused = "--transport";
        } else if (options.vddk.nfcHostPort != 0) {
            unused = "--nfc-port";
        } else if (options.queryChunk) {
            unused = "--query-chunk";
        }
    } else {
        if (!options.cluster.empty()) {
            unused = "--cluster";
        } else if (!options.resourcePool.empty()) {
            unused = "--resource-pool";
        } else if (options.quiesce) {
            unused = "--quiesce";
        }
    }
    if (unused) {
        std::cerr << "Error: " << unused << " is for --api " << (options.api == Api::Soap ? "vddk" : "soap")
                  << " only\n";
        return false;
    }
    return true;
}

bool checkVmdkOptions(const Options& options, bool forRestore)
{
    if (!checkApiOptions(options)) {
        return false;
    }
    if (options.vmdk.empty()) {
        return true;
    }
    if (options.api == Api::Soap) {
        // Always remote, and only a restore may leave out the VM: the new disk then stays unattached.
        if (options.vddk.host.empty()) {
            std::cerr << "Error: --api soap requires --host; a local VMDK needs --api vddk\n";
            return false;
        }
    } else if (options.vddk.host.empty()) {
        return true; // local VMDK file, no credentials needed
    }
    if (options.vddk.user.empty() || options.vddk.password.empty()) {
        std::cerr << "Error: --host requires --user and --password (or password= with --stdin-keys)\n";
        return false;
    }
    if (options.vddk.vmxSpec.empty() && !(options.api == Api::Soap && forRestore)) {
        std::cerr << "Error: --host requires --vm\n";
        return false;
    }
    return true;
}

Soap::ConnectParams soapParams(const Options& options)
{
    Soap::ConnectParams params;
    params.host = options.vddk.host;
    params.user = options.vddk.user;
    params.password = options.vddk.password;
    params.thumbPrint = options.vddk.thumbPrint;
    params.port = options.vddk.port;
    params.vm = options.vddk.vmxSpec;
    params.snapshot = options.vddk.snapshotRef;
    params.identity = options.vddk.identity;
    params.quiesce = options.quiesce;
    params.compute = options.cluster;
    params.resourcePool = options.resourcePool;
    return params;
}

// A disk read over SOAP reports the change id its next incremental is taken against: the snapshot it was read at
// is the tool's own, so nobody else has it.
struct SoapSource : Tool::DiskSource {
    std::string nextChangeId;

    std::string description() const override { return "SOAP"; }

    void addDetails(Tool::JsonObject& json) const override
    {
        if (!nextChangeId.empty()) {
            json.addString("next_change_id", nextChangeId);
        }
    }

    void printDetails() const override
    {
        if (!nextChangeId.empty()) {
            std::cout << "Next change id:    " << nextChangeId << "\n";
        }
    }
};

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

    bool checkOptions(bool forRestore) const override { return checkVmdkOptions(m_options, forRestore); }

    const char* diskOption() const override { return "--vmdk"; }
    const std::string& disk() const override { return m_options.vmdk; }

    std::unique_ptr<Tool::DiskSource> openSource() override
    {
        if (!m_options.vmdk.empty() && m_options.api == Api::Soap) {
            return openSoapSource();
        }

        auto pSource = std::make_unique<Tool::DiskSource>();

        if (m_options.vmdk.empty()) {
            pSource->reader = openFileReader(m_options.source);
        } else {
#ifdef ABR_DISK_HAVE_VDDK
            auto pSession = std::make_shared<Vddk::Session>();
            pSource->session = pSession;
            if (!pSession->connect(m_options.vddk, true)) {
                return nullptr;
            }
            pSource->reader = pSession->openReader(m_options.vmdk, m_options.changeId,
                                                   m_options.queryChunk.value_or(kDefaultQueryChunk));
#endif
        }
        return pSource->reader ? std::move(pSource) : nullptr;
    }

    // The VDDK never creates a VMDK, so only a new disk over SOAP is sized from the backup's geometry.
    std::unique_ptr<Tool::DiskTarget> openTarget(bool keepExisting, const DiskGeometry& image) override
    {
        auto pTarget = std::make_unique<Tool::DiskTarget>();

        if (m_options.vmdk.empty()) {
            pTarget->writer = openFileWriter(m_options.target, keepExisting);
        } else if (m_options.api == Api::Soap) {
            return openSoapTarget(keepExisting, image);
        } else {
#ifdef ABR_DISK_HAVE_VDDK
            auto pSession = std::make_shared<Vddk::Session>();
            pTarget->session = pSession;
            if (!pSession->connect(m_options.vddk, false)) {
                return nullptr;
            }
            pTarget->writer = pSession->openWriter(m_options.vmdk);
#endif
        }
        return pTarget->writer ? std::move(pTarget) : nullptr;
    }

private:
    // Reached only with --api soap, which a build without the SOAP API refuses while parsing.
    std::unique_ptr<Tool::DiskSource> openSoapSource()
    {
#ifndef ABR_DISK_HAVE_SOAP
        return nullptr;
#else
        auto pReader = std::make_unique<Soap::DiskReader>();
        if (!pReader->open(soapParams(m_options), m_options.vmdk, m_options.changeId)) {
            return nullptr;
        }
        auto pSource = std::make_unique<SoapSource>();
        pSource->nextChangeId = pReader->nextChangeId();
        pSource->reader = std::move(pReader);
        return pSource;
#endif
    }

    // The backup's geometry sizes a new disk; an overwritten one is checked against it by the restore.
    std::unique_ptr<Tool::DiskTarget> openSoapTarget(bool keepExisting, const DiskGeometry& image)
    {
#ifndef ABR_DISK_HAVE_SOAP
        (void)keepExisting;
        (void)image;
        return nullptr;
#else
        if (keepExisting) {
            std::cerr << "Error: --api soap cannot write into part of a disk, so an incremental cannot be applied "
                         "on its own; restore the chain from its full backup with --source-names, or use --api vddk\n";
            return nullptr;
        }
        auto pWriter = std::make_unique<Soap::DiskWriter>();
        if (!pWriter->open(soapParams(m_options), m_options.vmdk, image.sectors * image.sectorSize)) {
            return nullptr;
        }
        auto pTarget = std::make_unique<Tool::DiskTarget>();
        pTarget->created = pWriter->creates();
        pTarget->writer = std::move(pWriter);
        return pTarget;
#endif
    }

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
