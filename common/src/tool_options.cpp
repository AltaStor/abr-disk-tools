// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The shared options: the table, the parse, and the checks on what came out
//
#include "tool_options.h"

#include <iostream>
#include <sstream>

const Cli::LongOption Tool::kHeadLongOptions[] = {
    {"help",          Cli::Argument::None,     'h'},
    {"folder",        Cli::Argument::Required, 'F'},
    {"mode",          Cli::Argument::Required, 'm'},
    {"path",          Cli::Argument::Required, 'p'},
    {"name",          Cli::Argument::Required, 'n'},
    {"source",        Cli::Argument::Required, 's'},
    {"target",        Cli::Argument::Required, 't'},
    {"key",           Cli::Argument::Required, 'k'},
    {"compression",   Cli::Argument::Required, 'c'},
    {"force",         Cli::Argument::None,     'f'},

    {"endpoint",      Cli::Argument::Required, kOptEndpoint},
    {"region",        Cli::Argument::Required, kOptRegion},
    {"access-key",    Cli::Argument::Required, kOptAccessKey},
    {"secret-key",    Cli::Argument::Required, kOptSecretKey},
    {"stdin-keys",    Cli::Argument::None,     kOptStdinKeys},
    {"aws-profile",   Cli::Argument::Required, kOptProfile},
    {"path-style",    Cli::Argument::None,     kOptPathStyle},
    {"create-folder", Cli::Argument::None,     kOptCreateFolder},
    {"no-verify-tls", Cli::Argument::None,     kOptNoVerifyTls},
    {"immutable",     Cli::Argument::Required, kOptImmutable},
    {"retention",     Cli::Argument::Required, kOptRetention},

    {"change-id",     Cli::Argument::Required, kOptChangeId},
    {"type",          Cli::Argument::Required, kOptType},
    {"json",          Cli::Argument::None,     kOptJson},

    {nullptr,         Cli::Argument::None,     0},
};

// Last on purpose: "--source" is an exact match for one row here and a prefix
// of eleven others, which is the case Cli::OptionParser's exact-match-wins
// rule exists for. Reordering within this block would change which options an
// ambiguous prefix reports.
const Cli::LongOption Tool::kTailLongOptions[] = {
    {"source-names",         Cli::Argument::Required, kOptSourceNames},
    {"source-mode",          Cli::Argument::Required, kOptSourceMode},
    {"source-folder",        Cli::Argument::Required, kOptSourceFolder},
    {"source-path",          Cli::Argument::Required, kOptSourcePath},
    {"source-key",           Cli::Argument::Required, kOptSourceKey},
    {"source-endpoint",      Cli::Argument::Required, kOptSourceEndpoint},
    {"source-region",        Cli::Argument::Required, kOptSourceRegion},
    {"source-access-key",    Cli::Argument::Required, kOptSourceAccessKey},
    {"source-secret-key",    Cli::Argument::Required, kOptSourceSecretKey},
    {"source-aws-profile",   Cli::Argument::Required, kOptSourceProfile},
    {"source-path-style",    Cli::Argument::None,     kOptSourcePathStyle},
    {"source-no-verify-tls", Cli::Argument::None,     kOptSourceNoVerifyTls},

    {nullptr,                Cli::Argument::None,     0},
};

Tool::OptionTable::OptionTable(const Cli::LongOption* own)
{
    for (const Cli::LongOption* row = kHeadLongOptions; row->name != nullptr; ++row) {
        m_rows.push_back(*row);
    }
    for (const Cli::LongOption* row = own; row != nullptr && row->name != nullptr; ++row) {
        m_rows.push_back(*row);
    }
    for (const Cli::LongOption* row = kTailLongOptions; row->name != nullptr; ++row) {
        m_rows.push_back(*row);
    }
    m_rows.push_back({nullptr, Cli::Argument::None, 0});
}

bool Tool::parseSharedOptions(int argc, char* argv[], const Cli::LongOption* longOptions,
                              CommonOptions& options, const ToolOptionHandler& setToolOption)
{
    Cli::OptionParser parser(argc, argv, kShortOptions, longOptions);

    int opt;
    while ((opt = parser.next()) != -1) {
        const std::string& arg = parser.argument();

        switch (opt) {
            case 'h': options.help = true; break;
            case 'F': options.storage.folder = arg; break;
            case 'm': options.storage.mode = arg; break;
            case 'p': options.storage.path = arg; break;
            case 'n': options.storage.name = arg; break;
            case 's': options.source = arg; break;
            case 't': options.target = arg; break;
            case 'k': options.storage.key = arg; break;
            case 'c':
                if (!parseCompression(arg, options.storage.compression)) {
                    return false;
                }
                break;
            case 'f': options.force = true; break;

            case kOptEndpoint: options.storage.endpoint = arg; break;
            case kOptRegion: options.storage.region = arg; break;
            case kOptAccessKey: options.storage.accessKey = arg; break;
            case kOptSecretKey: options.storage.secretKey = arg; break;
            case kOptStdinKeys: options.stdinKeys = true; break;
            case kOptProfile: options.storage.profile = arg; break;
            case kOptPathStyle: options.storage.pathStyle = true; break;
            case kOptCreateFolder: options.storage.createFolder = true; break;
            case kOptNoVerifyTls: options.storage.verifyTls = false; break;
            case kOptImmutable:
                if (!parseImmutabilityMode(arg, options.immutability)) {
                    return false;
                }
                break;
            case kOptRetention:
                if (!parseRetentionDays(arg, options.retentionDays)) {
                    return false;
                }
                break;

            case kOptChangeId: options.changeId = arg; break;
            case kOptType: options.type = arg; break;
            case kOptJson: options.json = true; break;

            case kOptSourceNames:
                if (!parseBackupNames(arg, options.sourceNames)) {
                    return false;
                }
                break;
            case kOptSourceMode: options.sourceStorage.mode = arg; break;
            case kOptSourceFolder: options.sourceStorage.folder = arg; break;
            case kOptSourcePath: options.sourceStorage.path = arg; break;
            case kOptSourceKey: options.sourceStorage.key = arg; break;
            case kOptSourceEndpoint: options.sourceStorage.endpoint = arg; break;
            case kOptSourceRegion: options.sourceStorage.region = arg; break;
            case kOptSourceAccessKey: options.sourceStorage.accessKey = arg; break;
            case kOptSourceSecretKey: options.sourceStorage.secretKey = arg; break;
            case kOptSourceProfile: options.sourceStorage.profile = arg; break;
            case kOptSourcePathStyle: options.sourceStorage.pathStyle = true; break;
            case kOptSourceNoVerifyTls: options.sourceStorage.verifyTls = false; break;

            default:
                // '?' has been reported by the parser already; anything else is
                // a tool's own row, which only the tool can set.
                if (opt == '?' || !setToolOption || !setToolOption(opt, arg)) {
                    return false;
                }
                break;
        }
    }

    // Read once the whole command line is in, so that a secret given there too
    // is caught; and not for --help, which would otherwise sit waiting on a
    // console for input nobody means to type.
    if (options.stdinKeys && !options.help) {
        return readStdinKeys(std::cin, options);
    }
    return true;
}

bool Tool::readStdinKeys(std::istream& in, CommonOptions& options)
{
    struct Secret {
        const char* name;
        bool onCommandLine;
        std::function<void(const std::string&)> set;
        bool seen = false;
    };
    Secret secrets[] = {
        {"key", !options.storage.key.empty(),
         [&](const std::string& v) { options.storage.key = v; }},
        {"secret-key", !options.storage.secretKey.empty(),
         [&](const std::string& v) { options.storage.secretKey = v; }},
        {"source-key", options.sourceStorage.key.has_value(),
         [&](const std::string& v) { options.sourceStorage.key = v; }},
        {"source-secret-key", options.sourceStorage.secretKey.has_value(),
         [&](const std::string& v) { options.sourceStorage.secretKey = v; }},
    };

    std::string line;
    while (std::getline(in, line)) {
        // Standard input is binary on Windows (see Plat::StdioSetup), so a line
        // written by PowerShell or typed at a console keeps its carriage return.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }

        // Only ever the name in a message: the value is the one thing on this
        // line that must not end up in a log.
        size_t equals = line.find('=');
        std::string name = line.substr(0, equals);
        Secret* secret = nullptr;
        for (Secret& candidate : secrets) {
            if (name == candidate.name) {
                secret = &candidate;
            }
        }

        if (secret == nullptr) {
            // A line without a name may well be a bare password, and an
            // unknown "name" its first half; only something that looks like
            // an option name is repeated back.
            bool printable = equals != std::string::npos && !name.empty() && name.size() <= 32
                             && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") == std::string::npos;
            std::cerr << "Error: --stdin-keys: ";
            if (printable) {
                std::cerr << "unknown secret '" << name << "'";
            } else {
                std::cerr << "a line is not name=value";
            }
            std::cerr << "; expected key, secret-key, source-key or source-secret-key\n";
            return false;
        }
        if (equals == std::string::npos || equals + 1 == line.size()) {
            std::cerr << "Error: --stdin-keys: no value for '" << name << "'\n";
            return false;
        }
        if (secret->seen) {
            std::cerr << "Error: --stdin-keys: '" << name << "' given twice\n";
            return false;
        }
        if (secret->onCommandLine) {
            std::cerr << "Error: --" << name << " given both on the command line and on standard input\n";
            return false;
        }

        secret->set(line.substr(equals + 1));
        secret->seen = true;
    }

    if (in.bad()) {
        std::cerr << "Error: --stdin-keys: failed to read standard input\n";
        return false;
    }
    return true;
}

// Only the names of the enum are accepted, so that a typo is rejected instead
// of quietly passing for "no immutability at all".
bool Tool::parseImmutabilityMode(const std::string& text, Abr::ImmutabilityMode& mode)
{
    if (text == "none") {
        mode = Abr::ImmutabilityMode::NONE;
    } else if (text == "governance") {
        mode = Abr::ImmutabilityMode::GOVERNANCE;
    } else if (text == "compliance") {
        mode = Abr::ImmutabilityMode::COMPLIANCE;
    } else {
        std::cerr << "Error: --immutable must be 'none', 'governance' or 'compliance'\n";
        return false;
    }
    return true;
}

// A retention period, counted in days like everywhere else in the library.
// A retention period, counted in days like everywhere else in the library.
bool Tool::parseRetentionDays(const std::string& text, int& days)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        days = 0;
    } else {
        try {
            days = std::stoi(text);
        } catch (const std::exception&) {
            days = 0;
        }
    }

    if (days <= 0) {
        std::cerr << "Error: --retention must be a positive number of days\n";
        return false;
    }
    return true;
}

// A compression level. Digits only, so that "3x" or "-1" is refused rather
// than read as 3 or wrapped round -- and so that no std::stoi throws out of the
// parser and takes the process down with it.
// A compression level. Digits only, so that "3x" or "-1" is refused rather
// than read as 3 or wrapped round -- and so that no std::stoi throws out of the
// parser and takes the process down with it.
bool Tool::parseCompression(const std::string& text, int& level)
{
    level = -1;

    if (!text.empty() && text.size() <= 2
        && text.find_first_not_of("0123456789") == std::string::npos) {
        level = std::stoi(text);
    }

    if (level < 0 || level > kMaxCompression) {
        std::cerr << "Error: --compression must be a level from 0 to " << kMaxCompression << "\n";
        return false;
    }
    return true;
}

// --source-names carries the whole chain as one comma-separated list. Nothing
// records which backup an incremental was taken against -- the format keeps no
// parent link -- so the order given here is what defines the chain.
// --source-names carries the whole chain as one comma-separated list. Nothing
// records which backup an incremental was taken against -- the format keeps no
// parent link -- so the order given here is what defines the chain.
bool Tool::parseBackupNames(const std::string& text, std::vector<std::string>& names)
{
    std::istringstream list(text);
    std::string name;

    while (std::getline(list, name, ',')) {
        size_t begin = name.find_first_not_of(" \t");
        size_t end = name.find_last_not_of(" \t");

        // A stray comma is a typo rather than a backup with no name, and taking
        // it for one would only fail later, against the storage.
        if (begin == std::string::npos) {
            std::cerr << "Error: --source-names holds an empty backup name\n";
            return false;
        }
        names.push_back(name.substr(begin, end - begin + 1));
    }

    if (names.empty()) {
        std::cerr << "Error: --source-names requires at least one backup name\n";
        return false;
    }
    return true;
}
bool Tool::checkStorageOptions(const StorageOptions& storage)
{
    if (storage.folder.empty()) {
        std::cerr << "Error: --folder is required\n";
        return false;
    }
    if (storage.mode != "file" && storage.mode != "s3") {
        std::cerr << "Error: --mode must be 'file' or 's3'\n";
        return false;
    }
    return true;
}

// --vhdx takes the place of --source or --target; naming both would leave the
// tool to guess which disk was meant.
bool Tool::checkBackupType(const CommonOptions& options, const char* diskOption, bool diskGiven)
{
    if (options.type != "full" && !options.incremental()) {
        std::cerr << "Error: --type must be 'full' or 'incremental'\n";
        return false;
    }

    if (!options.incremental()) {
        if (!options.changeId.empty()) {
            std::cerr << "Error: --change-id is only meaningful with --type incremental\n";
            return false;
        }
        return true;
    }

    // Changed block tracking is the hypervisor's: resilient change tracking on
    // Hyper-V, changed block tracking on VMware. There is no equivalent for a
    // plain file, and silently falling back to a full copy would be a trap.
    if (!diskGiven) {
        std::cerr << "Error: --type incremental requires " << diskOption << "\n";
        return false;
    }

    if (options.changeId.empty()) {
        std::cerr << "Error: --type incremental requires --change-id\n";
        return false;
    }
    return true;
}

// The two halves of a retention are worth nothing apart: a mode with no period
// holds a backup for no time at all, and a period with no mode is not enforced.
// Whether the bucket can actually deliver the mode is the storage's call, and is
// answered when the backup is created.
bool Tool::checkImmutabilityOptions(const CommonOptions& options)
{
    if (options.immutability == Abr::ImmutabilityMode::NONE) {
        if (options.retentionDays > 0) {
            std::cerr << "Error: --retention has no effect without --immutable\n";
            return false;
        }
        return true;
    }

    if (options.storage.mode != "s3") {
        std::cerr << "Error: --immutable applies to S3 storage only\n";
        return false;
    }

    if (options.retentionDays <= 0) {
        std::cerr << "Error: --immutable requires --retention\n";
        return false;
    }
    return true;
}
// The storage a 'copy' reads its chain from: the destination's, with whatever
// the --source-<...> options overrode put over it.
Tool::StorageOptions Tool::sourceStorage(const CommonOptions& options)
{
    StorageOptions storage = options.storage;
    const SourceStorageOptions& source = options.sourceStorage;

    // Neither belongs to the source: the chain has to be there already, and
    // nothing is written to it.
    storage.createFolder = false;
    storage.name.clear();

    if (source.mode) storage.mode = *source.mode;
    if (source.folder) storage.folder = *source.folder;
    if (source.path) storage.path = *source.path;
    if (source.key) storage.key = *source.key;
    if (source.endpoint) storage.endpoint = *source.endpoint;
    if (source.region) storage.region = *source.region;
    if (source.accessKey) storage.accessKey = *source.accessKey;
    if (source.secretKey) storage.secretKey = *source.secretKey;
    if (source.profile) storage.profile = *source.profile;
    if (source.pathStyle) storage.pathStyle = *source.pathStyle;
    if (source.verifyTls) storage.verifyTls = *source.verifyTls;

    return storage;
}

// The source storage inherits the destination's folder and mode, both already
// checked by then, so only what --source-<...> actually overrode can be wrong.
// The source storage inherits the destination's folder and mode, both already
// checked by then, so only what --source-<...> actually overrode can be wrong.
bool Tool::checkSourceStorageOptions(const StorageOptions& source)
{
    if (source.folder.empty()) {
        std::cerr << "Error: --source-folder is required\n";
        return false;
    }
    if (source.mode != "file" && source.mode != "s3") {
        std::cerr << "Error: --source-mode must be 'file' or 's3'\n";
        return false;
    }
    return true;
}
