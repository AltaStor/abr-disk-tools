// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// 'backup' and 'restore', with the format's part of them reached through Tool::DiskTool
//
#include "tool_disk.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "abr_backup.h"
#include "tool_backup.h"
#include "tool_commands.h"

namespace {
    // The disk the run reads or writes, as the report names it.
    std::string diskPath(const Tool::DiskTool& tool, const std::string& file)
    {
        return tool.disk().empty() ? file : tool.disk();
    }
}

int Tool::runBackup(const std::string& programName, int argc, char* argv[])
{
    std::unique_ptr<DiskTool> pTool = context().diskTool();
    if (!pTool->parseOptions(argc, argv)) {
        return 1;
    }

    const CommonOptions& options = pTool->options();
    if (options.help) {
        pTool->printBackupHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage) || !pTool->checkOptions(false)
        || !checkBackupType(options, pTool->diskOption(), !pTool->disk().empty())
        || !checkImmutabilityOptions(options)) {
        return 1;
    }
    if (options.storage.name.empty() || (options.source.empty() && pTool->disk().empty())) {
        std::cerr << "Error: backup requires --name and either --source or " << pTool->diskOption()
                  << "\n";
        pTool->printBackupHelp(programName);
        return 1;
    }

    std::unique_ptr<DiskSource> pSource = pTool->openSource();
    if (!pSource) {
        return 1;
    }

    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    const DiskGeometry& geometry = pSource->reader->geometry();

    Abr::BackupInfo info = {};
    info.sectorSize = static_cast<uint16_t>(geometry.sectorSize);
    info.phySectorSize = static_cast<uint16_t>(geometry.phySectorSize);
    info.sectors = geometry.sectors;
    info.compression = static_cast<uint8_t>(options.storage.compression);
    info.encryption = options.storage.key.empty() ? 0 : 1;
    info.incremental = options.incremental() ? 1 : 0;
    info.m_immutabilityMode = options.immutability;
    info.m_immutabilityDays = options.retentionDays;
    info.m_immutabilityUntil = options.retentionTill;

    std::shared_ptr<Abr::IBackup> pBackup;
    if (!pManager->createBackup(options.storage.path, options.storage.name, info, pBackup,
                                options.storage.key)) {
        std::cerr << "Error: failed to create backup '" << options.storage.name << "'\n";
        return 1;
    }

    bool ok = copyToBackup(*pSource->reader, *pBackup,
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

    const std::string sourcePath = diskPath(*pTool, options.source);

    if (options.json) {
        JsonObject json("backup", "ok");
        json.addString("source", sourcePath);
        json.addString("backup", backupLocation(options.storage));
        json.addString("name", options.storage.name);
        json.addString("kind", options.incremental() ? "incremental" : "full");
        if (options.incremental()) {
            json.addString("change_id", options.changeId);
        }
        pSource->addDetails(json);
        addRequestedImmutability(json, options);
        addBackupInfo(json, info);
    } else {
        const std::string description = pSource->description();

        std::cout << "Source:            " << sourcePath
                  << (description.empty() ? "" : " (" + description + ")") << "\n"
                  << "Backup:            " << backupLocation(options.storage) << "\n"
                  << "Kind:              "
                  << (options.incremental() ? "incremental (changed since " + options.changeId + ")"
                                            : std::string("full")) << "\n";
        pSource->printDetails();
        std::cout << "Raw data size:     " << info.orig_data_size << " bytes\n"
                  << "Compression:       " << (info.compression ?
                            ("zstd level " + std::to_string(info.compression)) : std::string("none")) << "\n";

        if (info.compression && info.orig_data_size > 0) {
            std::cout << "Stored data size:  " << info.stored_data_size << " bytes\n"
                << "Compression ratio: " << static_cast<double>(info.stored_data_size) / info.orig_data_size << "\n";
        }
        std::cout << "Encryption:        " << (info.encryption ? "AES-256-GCM" : "none") << "\n"
                  << "Immutability:      " << requestedImmutability(options) << "\n";
    }
    return 0;
}

int Tool::runRestore(const std::string& programName, int argc, char* argv[])
{
    std::unique_ptr<DiskTool> pTool = context().diskTool();
    if (!pTool->parseOptions(argc, argv)) {
        return 1;
    }

    const CommonOptions& options = pTool->options();
    if (options.help) {
        pTool->printRestoreHelp(programName);
        return 0;
    }
    if (!checkStorageOptions(options.storage) || !pTool->checkOptions(true)) {
        return 1;
    }
    if ((options.storage.name.empty() && options.sourceNames.empty())
        || (options.target.empty() && pTool->disk().empty())) {
        std::cerr << "Error: restore requires --name or --source-names, and either --target or "
                  << pTool->diskOption() << "\n";
        pTool->printRestoreHelp(programName);
        return 1;
    }
    if (!checkRestoreNames(options)) {
        return 1;
    }

    auto pManager = openManager(options.storage);
    if (!pManager) {
        return 1;
    }

    // A single --name is a chain of one, so both are restored the same way.
    BackupChain chain;
    std::vector<MergedPiece> plan;
    uint64_t total = 0;
    if (!openRestoreChain(*pManager, options, chain, plan, total)) {
        return 1;
    }

    const Abr::BackupInfo& info = chain.backups.front()->getInfo();
    const bool incremental = info.incremental != 0;

    DiskGeometry image;
    image.sectors = info.sectors;
    image.sectorSize = info.sectorSize;
    image.phySectorSize = info.phySectorSize;

    std::unique_ptr<DiskTarget> pTarget = pTool->openTarget(incremental, image);
    if (!pTarget) {
        return 1;
    }

    const DiskGeometry& geometry = pTarget->writer->geometry();

    if (geometry.sectors != 0) {
        if (info.sectorSize != geometry.sectorSize) {
            std::cerr << "Error: backup sector size " << info.sectorSize
                      << " does not match target sector size " << geometry.sectorSize << "\n";
            return 1;
        }
        if (info.sectors != geometry.sectors) {
            std::cerr << "Error: backup holds " << info.sectors << " sectors but the target has "
                      << geometry.sectors << "\n";
            return 1;
        }
    }

    if (!pTarget->writer->setImageSize(info.sectors * info.sectorSize)) {
        return 1;
    }

    bool ok = copyChainToDisk(chain, plan, total, *pTarget->writer,
                              alignBufferSize(chainBufferSize(chain), geometry.sectorSize), options.json);

    // Released here rather than at the end of the scope, so that a disk this restore
    // created and did not finish is gone before the failure is reported.
    pTarget->writer.reset();

    if (!ok) {
        return 1;
    }

    const std::string restoreTarget = diskPath(*pTool, options.target);
    const bool created = pTarget->created.value_or(false);

    if (options.json) {
        JsonObject json("restore", "ok");
        addRestoredFrom(json, options);
        json.addString("target", restoreTarget);
        json.addString("kind", incremental ? "incremental" : "full");
        if (pTarget->created) {
            json.addBool("created", created);
        }
        json.addBool("decrypted", !options.storage.key.empty());
    } else {
        std::cout << (incremental ? "Applied incremental " : "Restored ") << restoredFrom(options)
                  << " to " << (created ? "new virtual disk '" : "'") << restoreTarget << "'"
                  << (options.storage.key.empty() ? "" : " (decrypted)") << "\n";
    }
    return 0;
}
