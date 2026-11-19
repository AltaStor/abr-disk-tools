// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// A chain of backups: the storage it is read from, opened and checked, and the plan that merges it
//
#include "chain.h"

#include <algorithm>
#include <iostream>

// Abr::S3BackupManagerParams and Abr::immutabilityModeName live here.
// Kept out of the public headers: s3_backup.h pulls in <aws/core/Aws.h>
// and <aws/s3/S3Client.h>, and the library's own headers stay AWS-free.
#include "s3_backup.h"

std::shared_ptr<Abr::IBackupManager> Chain::openManager(const StorageOptions& storage)
{
    std::shared_ptr<Abr::IBackupManager> pManager;

    if (storage.mode == "s3") {
        Abr::S3BackupManagerParams params;
        params.m_size = sizeof(params);
        params.m_createFolderIfMissing = storage.createFolder;
        params.m_folder = storage.folder;
        params.m_serverUrl = storage.endpoint;
        params.m_region = storage.region;
        params.m_accessKey = storage.accessKey;
        params.m_secretKey = storage.secretKey;
        params.m_profile = storage.profile;
        params.m_pathStyle = storage.pathStyle;
        params.m_verifyTls = storage.verifyTls;
        pManager = Abr::createBackupManager(storage.mode, params);
    } else {
        Abr::BackupManagerParams params;
        params.m_size = sizeof(params);
        params.m_createFolderIfMissing = storage.createFolder;
        params.m_folder = storage.folder;
        pManager = Abr::createBackupManager(storage.mode, params);
    }

    if (!pManager) {
        std::cerr << "Error: failed to initialize '" << storage.mode << "' backup storage at '"
                  << storage.folder << "'\n";
    }
    return pManager;
}

// Whether the key a backup was opened with decrypts it. backuplib wraps the
// data key with AES-CTR, which carries no integrity, so openBackup "unwraps" it
// with any password whatever and the mistake only shows at the first data
// chunk -- when a restore has already created, attached and claimed its target.
// Decrypting the first sector here, before anything is touched, turns
// that into a plain refusal. The data itself is AES-GCM, so a wrong key cannot
// pass for a right one.
bool Chain::checkKey(Abr::IRestore& restore, const std::string& name, const std::string& key)
{
    const Abr::BackupInfo& info = restore.getInfo();
    if (!info.encryption) {
        return true;
    }

    if (key.empty()) {
        std::cerr << "Error: backup '" << name << "' is encrypted; pass its key with --key\n";
        return false;
    }

    std::vector<Abr::Range> ranges;
    if (!restore.getDataRanges(ranges)) {
        std::cerr << "Error: failed to read backup data ranges\n";
        return false;
    }

    // A backup that stored nothing has nothing to decrypt, and nothing a wrong
    // key could get wrong either.
    if (ranges.empty() || ranges.front().length == 0) {
        return true;
    }

    std::vector<uint8_t> sector(std::min<uint32_t>(ranges.front().length, info.sectorSize));

    if (!restore.loadDataRange(0, sector.data(), static_cast<uint32_t>(sector.size()), 0)) {
        std::cerr << "Error: the key does not decrypt backup '" << name << "'\n";
        return false;
    }
    return true;
}

// Works out where every block of the merged image comes from, without reading
// any data: what the copy loop below then does is drive the plan.
bool Chain::buildCopyPlan(BackupChain& chain, std::vector<MergedPiece>& plan, uint64_t& total)
{
    CoveredRanges covered;
    std::vector<CoveredRanges::Gap> gaps;

    // Newest backup first, and within it the last data range first: both are
    // the order a restore applies them in, so the block that wins here is
    // the one a restore of the whole chain would have left on the disk.
    for (size_t i = chain.backups.size(); i-- > 0;) {
        std::vector<Abr::Range> ranges;

        if (!chain.backups[i]->getDataRanges(ranges)) {
            std::cerr << "Error: failed to read the data ranges of backup '" << chain.names[i]
                      << "'\n";
            return false;
        }

        for (size_t j = ranges.size(); j-- > 0;) {
            gaps.clear();
            covered.add(ranges[j].offset, ranges[j].offset + ranges[j].length, gaps);

            for (const auto& gap : gaps) {
                MergedPiece piece = {};
                piece.origOffset = gap.begin;
                piece.backup = static_cast<uint32_t>(i);
                piece.range = static_cast<uint32_t>(j);
                piece.offset = static_cast<uint32_t>(gap.begin - ranges[j].offset);
                piece.length = static_cast<uint32_t>(gap.end - gap.begin);

                plan.push_back(piece);
                total += piece.length;
            }
        }
    }

    // The destination is written front to back, the way a backup of a disk is.
    std::sort(plan.begin(), plan.end(), [](const MergedPiece& left, const MergedPiece& right) {
        return left.origOffset < right.origOffset;
    });

    return true;
}

// Opens the chain and checks it is one: a full backup or an incremental, then incrementals
// of the same disk.
// Nothing links an incremental to its parent in the format, so the order
// --source-names gives is taken as the chain of backups.
// A chain that starts with an incremental is not a whole disk, but merging it is still sound:
// the result applied onto the parent of its first backup leaves the disk each of them
// applied in turn would have left.
// That is what lets a repository be kept up to date one new backup at a time.
bool Chain::openChain(Abr::IBackupManager& manager, const StorageOptions& source,
               const std::vector<std::string>& names, BackupChain& chain)
{
    for (const auto& name : names) {
        std::shared_ptr<Abr::IRestore> pRestore;

        if (!manager.openBackup(source.path, name, pRestore, source.key)) {
            std::cerr << "Error: failed to open backup '" << name << "'\n";
            return false;
        }

        chain.names.push_back(name);
        chain.backups.push_back(pRestore);

        // Found now rather than partway through the copy, after the new backup
        // has been created and then has to be discarded again.
        if (!checkKey(*pRestore, name, source.key)) {
            return false;
        }
    }

    const Abr::BackupInfo& base = chain.backups.front()->getInfo();

    for (size_t i = 1; i < chain.backups.size(); ++i) {
        const Abr::BackupInfo& info = chain.backups[i]->getInfo();

        if (!info.incremental) {
            std::cerr << "Error: '" << chain.names[i]
                      << "' is a full backup, and only the first backup of a chain may be one.\n"
                      << "       --source-names takes the backup to start from first, then the\n"
                      << "       incrementals taken on top of it, in order.\n";
            return false;
        }

        // A chain of backups of two different disks would merge into an image
        // that never existed, so the geometry has to match all the way down.
        if (info.sectorSize != base.sectorSize || info.sectors != base.sectors) {
            std::cerr << "Error: '" << chain.names[i] << "' holds " << info.sectors << " sectors of "
                      << info.sectorSize << " bytes, but '" << chain.names.front() << "' holds "
                      << base.sectors << " of " << base.sectorSize << "\n";
            return false;
        }
    }

    return true;
}

uint32_t Chain::chainBufferSize(const BackupChain& chain)
{
    // A read never crosses a data range, so a buffer as large as the largest
    // block any of them stores is as much as one read can use.
    uint32_t bufferSize = 0;
    for (const auto& pBackup : chain.backups) {
        bufferSize = std::max(bufferSize, pBackup->getInfo().maxDataBlock);
    }
    return bufferSize ? bufferSize : kDefaultChunkSize;
}
