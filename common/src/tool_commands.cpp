// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The commands that touch no backend, and the scaffolding around all of them
//
#include "tool_commands.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include "plat.h"
#include "tool_backup.h"
#include "tool_json.h"

// Abr::AwsSdkGuard and Abr::immutabilityModeName live here.
// Kept out of the public headers; see tool_backup.cpp.
#include "s3_backup.h"

namespace {
    // Installed by Tool::run for the length of its body.
    // A pointer rather than a value because there is nothing sensible to default-construct it to:
    // a command reached without run() is a programming error, not a state to carry.
    const Tool::Context* g_pContext = nullptr;
}

const Tool::Context& Tool::context()
{
    if (g_pContext == nullptr) {
        // Not a diagnostic the user can act on, so it does not go through
        // the "Error: " convention: it says which contract was broken and stops.
        std::cerr << "abr-disk-tools: a command was run without Tool::run(), "
                     "which is what installs its context\n";
        std::abort();
    }
    return *g_pContext;
}

std::string Tool::programNameOf(const std::string& invokedAs, const char* fallback)
{
    std::string name = invokedAs.substr(invokedAs.find_last_of("\\/") + 1);

    if (name.size() > 4) {
        std::string extension = name.substr(name.size() - 4);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".exe") {
            name.resize(name.size() - 4);
        }
    }
    return name.empty() ? fallback : name;
}

int Tool::runCopy(const std::string& programName, int argc, char* argv[])
{
    CommonOptions options;
    if (!context().parseSharedOptions(argc, argv, options)) {
        return 1;
    }
    if (options.help) {
        printCopyHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage) || !checkImmutabilityOptions(options)) {
        return 1;
    }
    if (options.storage.name.empty() || options.sourceNames.empty()) {
        std::cerr << "Error: copy requires --name and --source-names\n";
        printCopyHelp(programName);
        return 1;
    }

    StorageOptions source = sourceStorage(options);
    if (!checkSourceStorageOptions(source)) {
        return 1;
    }

    // Writing the new backup over one the chain is being read from would lose both.
    // The endpoint is part of what tells two storages apart, since the same bucket
    // name at two S3 services is two different buckets.
    const bool sameStorage = source.mode == options.storage.mode
        && source.folder == options.storage.folder && source.path == options.storage.path
        && source.endpoint == options.storage.endpoint;

    if (sameStorage
        && std::find(options.sourceNames.begin(), options.sourceNames.end(), options.storage.name)
            != options.sourceNames.end()) {
        std::cerr << "Error: '" << options.storage.name
                  << "' is part of the source chain, so the copy would overwrite what it reads.\n"
                  << "       Give the new backup a name of its own, or write it elsewhere with\n"
                  << "       --source-folder/--source-path.\n";
        return 1;
    }

    auto pSourceManager = openManager(source);
    if (!pSourceManager) {
        return 1;
    }

    BackupChain chain;
    if (!openChain(*pSourceManager, source, options.sourceNames, chain)) {
        return 1;
    }

    std::vector<MergedPiece> plan;
    uint64_t total = 0;
    if (!buildCopyPlan(chain, plan, total)) {
        return 1;
    }

    const Abr::BackupInfo& base = chain.backups.front()->getInfo();

    const uint32_t bufferSize = chainBufferSize(chain);

    Abr::BackupInfo info = {};
    info.sectorSize = base.sectorSize;
    info.phySectorSize = base.phySectorSize;
    info.sectors = base.sectors;

    // The copy is described by the options given here, not by the chain: it is
    // compressed, encrypted and held as this command was asked for.
    info.compression = static_cast<uint8_t>(options.storage.compression);
    info.encryption = options.storage.key.empty() ? 0 : 1;

    // A chain from a full backup adds up to a whole disk, so the result is a full backup even
    // when most of what went into it came from incrementals.
    // A chain from an incremental holds only what changed since that one's parent, and the
    // result is an incremental of the same parent, to be applied as restore applies any other.
    const bool incremental = base.incremental != 0;
    info.incremental = incremental ? 1 : 0;
    info.m_immutabilityMode = options.immutability;
    info.m_immutabilityDays = options.retentionDays;
    info.m_immutabilityUntil = options.retentionTill;

    // A manager of its own even when the two sides name the same storage: what
    // makes them the same here is the location, and the credentials reaching it
    // may still differ.
    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    std::shared_ptr<Abr::IBackup> pBackup;
    if (!pManager->createBackup(options.storage.path, options.storage.name, info, pBackup,
                                options.storage.key)) {
        std::cerr << "Error: failed to create backup '" << options.storage.name << "'\n";
        return 1;
    }

    bool ok = copyChainToBackup(chain, plan, total, *pBackup,
                                alignBufferSize(bufferSize, base.sectorSize), options.json);

    info = pBackup->getInfo();

    // On failure this discards everything already written, so a failed run
    // leaves nothing of the copy behind in the repository.
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
        JsonObject json("copy", "ok");
        json.addString("source", storageLocation(source));
        json.addStrings("source_names", chain.names);
        json.addNumber("merged_from", chain.names.size());
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        json.addString("kind", incremental ? "incremental" : "full");
        addRequestedImmutability(json, options);
        addBackupInfo(json, info);
    } else {
        std::string names;
        for (const auto& name : chain.names) {
            names += (names.empty() ? "" : ", ") + name;
        }

        std::cout << "Source:            " << storageLocation(source) << " (" << names << ")\n"
                  << "Backup:            " << backupLocation(options.storage) << "\n"
                  << "Kind:              " << (incremental ? "incremental" : "full")
                  << ", merged from " << chain.names.size() << " backup(s)\n"
                  << "Raw data size:     " << info.orig_data_size << " bytes\n"
                  << "Compression:       "
                  << (info.compression ? ("zstd level " + std::to_string(info.compression))
                                       : std::string("none"))
                  << "\n";
        if (info.compression && info.orig_data_size > 0) {
            std::cout << "Stored data size:  " << info.stored_data_size << " bytes\n"
                      << "Compression ratio: "
                      << static_cast<double>(info.stored_data_size) / info.orig_data_size << "\n";
        }
        std::cout << "Encryption:        " << (info.encryption ? "AES-256-GCM" : "none") << "\n"
                  << "Immutability:      " << requestedImmutability(options) << "\n";
    }
    return 0;
}

int Tool::runList(const std::string& programName, int argc, char* argv[])
{
    CommonOptions options;
    if (!context().parseSharedOptions(argc, argv, options)) {
        return 1;
    }
    if (options.help) {
        printListHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage)) {
        return 1;
    }

    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    std::vector<std::string> backups;
    if (!pManager->listBackups(options.storage.path, backups)) {
        std::cerr << "Error: failed to list backups in '" << options.storage.folder << "'\n";
        return 1;
    }

    std::string location = storageLocation(options.storage);

    if (options.json) {
        JsonObject json("list", "ok");
        json.addString("location", location);
        json.addNumber("count", backups.size());
        json.addStrings("backups", backups);
    } else if (backups.empty()) {
        std::cout << "No backups found in '" << location << "'\n";
    } else {
        std::cout << "Backups in '" << location << "':\n";
        for (const auto& backup : backups) {
            std::cout << "  " << backup << "\n";
        }
    }
    return 0;
}

int Tool::runInfo(const std::string& programName, int argc, char* argv[])
{
    CommonOptions options;
    if (!context().parseSharedOptions(argc, argv, options)) {
        return 1;
    }
    if (options.help) {
        printInfoHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage)) {
        return 1;
    }

    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    // Without a backup to look at, report what the storage folder itself does:
    // how immutable it can hold backups, and how it holds the new ones.
    const Abr::BackupFolderInfo& folder = pManager->getFolderInfo();

    if (options.storage.name.empty()) {
        std::string location = storageLocation(options.storage);

        if (options.json) {
            JsonObject json("info", "ok");
            json.addString("location", location);
            addFolderInfo(json, folder);
        } else {
            std::cout << "Folder:            " << location << "\n"
                      << "Max immutability:  "
                      << Abr::immutabilityModeName(folder.m_immutabilitySupported) << "\n"
                      << "Immutability:      " << folderImmutability(folder) << "\n";
        }
        return 0;
    }

    std::shared_ptr<Abr::IRestore> pRestore;
    if (!pManager->openBackup(options.storage.path, options.storage.name, pRestore)) {
        std::cerr << "Error: failed to open backup '" << options.storage.name << "'\n";
        return 1;
    }

    const Abr::BackupInfo& info = pRestore->getInfo();

    if (options.json) {
        JsonObject json("info", "ok");
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        json.addString("kind", info.incremental ? "incremental" : "full");
        addBackupInfo(json, info);
        addBackupImmutability(json, info, folder);
    } else {
        std::cout << "Backup:            " << backupLocation(options.storage) << "\n"
                  << "Kind:              " << (info.incremental ? "incremental" : "full") << "\n"
                  << "Sectors:           " << info.sectors << "\n"
                  << "Sector size:       " << info.sectorSize << " bytes\n"
                  << "Physical sector:   " << info.phySectorSize << " bytes\n"
                  << "Raw data size:     " << info.orig_data_size << " bytes\n"
                  << "Compression:       " << (info.compression ? ("zstd level " + std::to_string(info.compression)) : std::string("none")) << "\n";
        if (info.compression && info.orig_data_size > 0) {
            std::cout << "Stored data size:  " << info.stored_data_size << " bytes\n"
                << "Compression ratio: " << static_cast<double>(info.stored_data_size) / info.orig_data_size << "\n";
        }
        std::cout << "Encryption:        " << (info.encryption ? "AES-256-GCM" : "none") << "\n"
                  << "Max immutability:  "
                  << Abr::immutabilityModeName(folder.m_immutabilitySupported) << "\n"
                  << "Immutability:      " << backupImmutability(info) << "\n";
    }

    pRestore->close();
    return 0;
}

int Tool::runVerify(const std::string& programName, int argc, char* argv[])
{
    CommonOptions options;
    if (!context().parseSharedOptions(argc, argv, options)) {
        return 1;
    }
    if (options.help) {
        printVerifyHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage)) {
        return 1;
    }
    if (options.storage.name.empty()) {
        std::cerr << "Error: verify requires --name\n";
        printVerifyHelp(programName);
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

    // The checksums below are verified without decrypting, so a key given
    // here is only worth anything if it is tried.
    if (!options.storage.key.empty()
        && !checkKey(*pRestore, options.storage.name, options.storage.key)) {
        pRestore->close();
        return 1;
    }

    std::vector<Abr::Range> ranges;
    if (!pRestore->getDataRanges(ranges)) {
        std::cerr << "Error: failed to read backup data ranges\n";
        pRestore->close();
        return 1;
    }

    uint64_t total = 0;
    for (const auto& range : ranges) {
        total += range.length;
    }

    // Every range is checked rather than stopping at the first bad one,
    // so a single run reports the full extent of the damage.
    ProgressPrinter progress(total, options.json);
    std::vector<char> tempBuf;
    uint32_t corrupt = 0;

    for (uint32_t i = 0; i < ranges.size(); ++i) {
        if (interrupted()) {
            pRestore->close();
            return 1;
        }

        if (!pRestore->checkDataRange(i, tempBuf)) {
            std::cerr << "Error: data range " << i << " at offset " << ranges[i].offset << " ("
                      << ranges[i].length << " bytes) is corrupt\n";
            corrupt++;
        }

        progress.advance(ranges[i].length);
    }

    progress.finish();
    pRestore->close();

    // A corrupt backup is a result rather than a malfunction, so it is reported
    // through the same document as an intact one -- with a non-zero exit status
    // and an "error" status field, since the check did not pass.
    if (options.json) {
        JsonObject json("verify", corrupt ? "error" : "ok");
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        json.addBool("intact", corrupt == 0);
        json.addNumber("data_ranges", ranges.size());
        json.addNumber("corrupt_ranges", corrupt);
        json.addNumber("bytes_verified", total);
        if (corrupt) {
            json.addString("error", "backup is corrupt: " + std::to_string(corrupt) + " of "
                                        + std::to_string(ranges.size())
                                        + " data ranges failed verification");
        }
    } else if (corrupt) {
        std::cerr << "Backup '" << backupLocation(options.storage) << "' is CORRUPT: " << corrupt
                  << " of " << ranges.size() << " data ranges failed verification\n";
    } else {
        std::cout << "Backup '" << backupLocation(options.storage) << "' is intact: " << ranges.size()
                  << " data ranges (" << total << " bytes) verified\n";
    }

    return corrupt ? 1 : 0;
}

int Tool::runRemove(const std::string& programName, int argc, char* argv[])
{
    CommonOptions options;
    if (!context().parseSharedOptions(argc, argv, options)) {
        return 1;
    }
    if (options.help) {
        printRemoveHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage)) {
        return 1;
    }
    if (options.storage.name.empty()) {
        std::cerr << "Error: remove requires --name\n";
        printRemoveHelp(programName);
        return 1;
    }

    if (!options.force) {
        // The prompt goes to stderr in --json mode: it is not a line of output,
        // and a caller reading stdout would be left waiting on a half-written one.
        std::ostream& prompt = options.json ? std::cerr : std::cout;
        prompt << "Remove backup '" << backupLocation(options.storage) << "'? [y/N] " << std::flush;

        std::string answer;
        std::getline(std::cin, answer);

        // Standard input is binary (see main), so an answer typed at a console
        // arrives with the carriage return of its line ending still on it.
        if (!answer.empty() && answer.back() == '\r') {
            answer.pop_back();
        }

        if (answer != "y" && answer != "Y") {
            if (options.json) {
                JsonObject json("remove", "aborted");
                json.addString("backup", backupLocation(options.storage));
                json.addString("name", options.storage.name);
            } else {
                std::cout << "Aborted.\n";
            }
            return 1;
        }
    }

    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    if (!pManager->deleteBackup(options.storage.path, options.storage.name)) {
        std::cerr << "Error: failed to remove backup '" << options.storage.name << "'\n";
        return 1;
    }

    if (options.json) {
        JsonObject json("remove", "ok");
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
    } else {
        std::cout << "Removed backup '" << backupLocation(options.storage) << "'\n";
    }
    return 0;
}

int Tool::runRetention(const std::string& programName, int argc, char* argv[])
{
    CommonOptions options;
    if (!context().parseSharedOptions(argc, argv, options)) {
        return 1;
    }
    if (options.help) {
        printRetentionHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage)) {
        return 1;
    }
    if (options.storage.name.empty()) {
        std::cerr << "Error: retention requires --name\n";
        printRetentionHelp(programName);
        return 1;
    }

    // A backup that is already held stays held until its date, and Object Lock
    // has no way to say "none", so releasing one is not on offer here.
    if (options.immutability == Abr::ImmutabilityMode::NONE) {
        std::cerr << "Error: retention requires --immutable 'governance' or 'compliance'\n";
        return 1;
    }
    if (!checkImmutabilityOptions(options)) {
        return 1;
    }

    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    const bool updated = options.retentionTill != 0
        ? pManager->updateRetentionUntil(options.storage.path, options.storage.name,
                                         options.immutability, options.retentionTill)
        : pManager->updateRetention(options.storage.path, options.storage.name,
                                    options.immutability, options.retentionDays);
    if (!updated) {
        std::cerr << "Error: failed to update the retention of backup '"
                  << options.storage.name << "'\n";
        return 1;
    }

    if (options.json) {
        JsonObject json("retention", "ok");
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        addRequestedImmutability(json, options);
    } else {
        std::cout << "Backup '" << backupLocation(options.storage) << "' is now held in "
                  << Abr::immutabilityModeName(options.immutability) << " mode, "
                  << requestedRetention(options) << "\n";
    }
    return 0;
}

int Tool::runMetaSave(const std::string& programName, int argc, char* argv[])
{
    CommonOptions options;
    if (!context().parseSharedOptions(argc, argv, options)) {
        return 1;
    }
    if (options.help) {
        printMetaSaveHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage)) {
        return 1;
    }
    if (options.storage.name.empty()) {
        std::cerr << "Error: meta_save requires --name\n";
        printMetaSaveHelp(programName);
        return 1;
    }

    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    bool fromStdin = options.source.empty() || options.source == "-";
    std::ifstream file;

    if (!fromStdin) {
        file.open(Plat::path(options.source), std::ios::binary);
        if (!file) {
            std::cerr << "Error: failed to open metadata source '" << options.source << "'\n";
            return 1;
        }
    }

    std::istream& metaData = fromStdin ? std::cin : file;

    if (!pManager->saveMetaData(options.storage.path, options.storage.name, metaData)) {
        std::cerr << "Error: failed to save metadata for '" << options.storage.name << "'\n";
        return 1;
    }

    if (options.json) {
        JsonObject json("meta_save", "ok");
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        json.addString("source", fromStdin ? "-" : options.source);
    } else {
        std::cout << "Saved metadata for '" << backupLocation(options.storage) << "'\n";
    }
    return 0;
}

int Tool::runMetaLoad(const std::string& programName, int argc, char* argv[])
{
    CommonOptions options;
    if (!context().parseSharedOptions(argc, argv, options)) {
        return 1;
    }
    if (options.help) {
        printMetaLoadHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage)) {
        return 1;
    }
    if (options.storage.name.empty()) {
        std::cerr << "Error: meta_load requires --name\n";
        printMetaLoadHelp(programName);
        return 1;
    }

    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    bool toStdout = options.target.empty() || options.target == "-";
    std::ofstream file;
    // In --json mode a blob bound for stdout is collected here instead, and
    // handed back as a string field of the result document, which would
    // otherwise have raw metadata written across it.
    std::ostringstream buffer;

    if (!toStdout) {
        file.open(Plat::path(options.target),
                  std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "Error: failed to open metadata target '" << options.target << "'\n";
            return 1;
        }
    }

    std::ostream* pMetaData = &file;
    if (toStdout) {
        if (options.json) {
            pMetaData = &buffer;
        } else {
            pMetaData = &std::cout;
        }
    }

    if (!pManager->loadMetaData(options.storage.path, options.storage.name, *pMetaData)) {
        std::cerr << "Error: failed to load metadata for '" << options.storage.name << "'\n";
        return 1;
    }

    if (toStdout) {
        // Without --json the metadata itself is the output, so stdout is left
        // free of chatter and there is nothing further to print.
        if (options.json) {
            JsonObject json("meta_load", "ok");
            json.addString("backup", backupLocation(options.storage));
            json.addString("name", options.storage.name);
            json.addString("target", "-");
            json.addString("metadata", buffer.str());
        }
        std::cout.flush();
        return 0;
    }

    // Not !flush(): writing an empty metadata blob leaves failbit set
    // (nothing was inserted) even though the file is exactly right.
    file.flush();
    if (file.bad()) {
        std::cerr << "Error: failed to write metadata to '" << options.target << "'\n";
        return 1;
    }

    if (options.json) {
        JsonObject json("meta_load", "ok");
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        json.addString("target", options.target);
    } else {
        std::cout << "Loaded metadata of '" << backupLocation(options.storage) << "' into '"
                  << options.target << "'\n";
    }
    return 0;
}

int Tool::run(int argc, char* argv[], const Context& context, const Dispatch& dispatch)
{
    Plat::StdioSetup console;
    std::vector<std::string> arguments = Plat::arguments(argc, argv);
    std::vector<char*> argvStorage;

    g_pContext = &context;

    for (auto& argument : arguments) {
        argvStorage.push_back(argument.data());
    }
    argvStorage.push_back(nullptr);

    argc = static_cast<int>(arguments.size());
    argv = argvStorage.data();

    // Owns the AWS SDK lifecycle for the whole process (required before any S3
    // backup manager is created); it must outlive every manager created below,
    // which it does since those are all local to the dispatch call further down.
    Abr::AwsSdkGuard awsSdkGuard;

    Plat::installInterruptHandler();

    std::string programName = programNameOf(argc > 0 ? argv[0] : "", context.branding.programName);

    if (argc < 2) {
        printMainHelp(programName, context.branding);
        return 1;
    }

    std::string command = argv[1];
    const bool json = jsonRequested(argc, argv);

    // Help stays plain text: it is written for a reader, and a caller asking
    // for JSON is not asking for the manual.
    if (command == "-h" || command == "--help") {
        printMainHelp(programName, context.branding);
        return 0;
    }

    if (command == "-v" || command == "--version") {
        if (json) {
            JsonObject result("version", "ok");
            result.addString("version", context.branding.version);
        } else {
            std::cout << context.branding.version << "\n";
        }
        return 0;
    }

    // Alive for the whole run, so that whatever the command reported on stderr
    // can be repeated in the JSON failure document below.
    std::unique_ptr<ErrorCollector> pErrors;
    if (json) {
        pErrors = std::make_unique<ErrorCollector>(std::cerr);
    }

    // Reparse options starting at the command token, so the option parser
    // inside each command handler treats argv[1] ("<command>") as its name.
    int cmdArgc = argc - 1;
    char** cmdArgv = argv + 1;
    int result = 1;

    // Nothing below is expected to throw, but the storage libraries may; one
    // that does is a failure to report like any other, not a crash dialog.
    try {
        result = dispatch(programName, command, cmdArgc, cmdArgv);

        if (result == kUnknownCommand) {
            result = 1;
            std::cerr << "Error: unknown command: " << command << "\n";
            if (!json) {
                std::cerr << "\n";
                printMainHelp(programName, context.branding);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        result = 1;
    }

    // A command that failed before it could report anything on stdout still
    // owes the caller a document.
    if (json && result != 0 && !jsonEmitted()) {
        const std::string& message = pErrors->firstError();

        JsonObject failure(command, "error");
        failure.addString("error", message.empty() ? "command failed" : message);
    }

    g_pContext = nullptr;

    return result;
}
