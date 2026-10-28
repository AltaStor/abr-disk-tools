// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The copy loops, the storage they run against, and the chain 'copy' merges
//
#include "tool_backup.h"

#include <algorithm>
#include <iostream>

#include "plat.h"
#include "tool_commands.h"

// Abr::S3BackupManagerParams and Abr::immutabilityModeName live here.
// Kept out of the public headers: s3_backup.h pulls in <aws/core/Aws.h>
// and <aws/s3/S3Client.h>, and the library's own headers stay AWS-free.
#include "s3_backup.h"

bool Tool::interrupted()
{
    if (!Plat::interruptRequested()) {
        return false;
    }
    std::cerr << "\nInterrupted, " << context().branding.interruptNote << "\n";
    return true;
}

std::shared_ptr<Abr::IBackupManager> Tool::openManager(const StorageOptions& storage)
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

// Formats "folder[/path]" for display purposes.
std::string Tool::storageLocation(const StorageOptions& storage)
{
    return storage.path.empty() ? storage.folder : storage.folder + "/" + storage.path;
}

// Formats "folder[/path]/name" for display purposes.
std::string Tool::backupLocation(const StorageOptions& storage, const std::string& name)
{
    return storage.folder + "/" + (storage.path.empty() ? "" : storage.path + "/") + name;
}

std::string Tool::backupLocation(const StorageOptions& storage)
{
    return backupLocation(storage, storage.name);
}

// Rounds a buffer size down to a whole number of sectors, keeping at least one.
uint32_t Tool::alignBufferSize(uint32_t size, uint32_t sectorSize)
{
    if (sectorSize == 0) {
        return size;
    }
    uint32_t aligned = size - (size % sectorSize);
    return aligned ? aligned : sectorSize;
}

// Whether the key a backup was opened with decrypts it. backuplib wraps the
// data key with AES-CTR, which carries no integrity, so openBackup "unwraps" it
// with any password whatever and the mistake only shows at the first data
// chunk -- when a restore has already created, attached and claimed its target.
// Decrypting the first sector here, before anything is touched, turns
// that into a plain refusal. The data itself is AES-GCM, so a wrong key cannot
// pass for a right one.
bool Tool::checkKey(Abr::IRestore& restore, const std::string& name, const std::string& key)
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

bool Tool::copyToBackup(IDiskReader& reader, Abr::IBackup& backup, uint32_t bufferSize,
                  bool linePerUpdate)
{
    std::vector<ByteRange> ranges;
    if (!reader.dataRanges(ranges)) {
        return false;
    }

    uint64_t total = 0;
    for (const auto& range : ranges) {
        total += range.length;
    }

    ProgressPrinter progress(total, linePerUpdate);
    std::vector<uint8_t> buf(bufferSize);

    for (const auto& range : ranges) {
        uint64_t offset = range.offset;
        uint64_t left = range.length;

        while (left > 0) {
            uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(bufferSize, left));

            if (interrupted()) {
                return false;
            }

            if (!reader.read(offset, count, buf.data())) {
                return false;
            }

            if (!backup.saveDataRange(buf.data(), count, offset)) {
                std::cerr << "Error: failed to save data range at offset " << offset << "\n";
                return false;
            }

            offset += count;
            left -= count;
            progress.advance(count);
        }
    }

    progress.finish();
    return true;
}

// Works out where every block of the merged image comes from, without reading
// any data: what the copy loop below then does is drive the plan.
bool Tool::buildCopyPlan(BackupChain& chain, std::vector<MergedPiece>& plan, uint64_t& total)
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

bool Tool::copyChainToBackup(BackupChain& chain, const std::vector<MergedPiece>& plan, uint64_t total,
                       Abr::IBackup& backup, uint32_t bufferSize, bool linePerUpdate)
{
    ProgressPrinter progress(total, linePerUpdate);
    std::vector<uint8_t> buf(bufferSize);

    for (const auto& piece : plan) {
        uint32_t done = 0;

        while (done < piece.length) {
            uint32_t count = std::min(bufferSize, piece.length - done);

            if (interrupted()) {
                return false;
            }

            if (!chain.backups[piece.backup]->loadDataRange(piece.range, buf.data(), count,
                                                            piece.offset + done)) {
                std::cerr << "Error: failed to load data range " << piece.range << " of backup '"
                          << chain.names[piece.backup] << "'\n";
                return false;
            }

            if (!backup.saveDataRange(buf.data(), count, piece.origOffset + done)) {
                std::cerr << "Error: failed to save data range at offset "
                          << (piece.origOffset + done) << "\n";
                return false;
            }

            done += count;
            progress.advance(count);
        }
    }

    progress.finish();
    return true;
}

// The same plan as copyChainToBackup drives, written to a disk instead: each block once,
// from the backup of the chain that changed it last, rather than once per backup that holds it.
bool Tool::copyChainToDisk(BackupChain& chain, const std::vector<MergedPiece>& plan, uint64_t total,
                           IDiskWriter& writer, uint32_t bufferSize, bool linePerUpdate)
{
    ProgressPrinter progress(total, linePerUpdate);
    std::vector<uint8_t> buf(bufferSize);

    for (const auto& piece : plan) {
        uint32_t done = 0;

        while (done < piece.length) {
            uint32_t count = std::min(bufferSize, piece.length - done);

            if (interrupted()) {
                return false;
            }

            if (!chain.backups[piece.backup]->loadDataRange(piece.range, buf.data(), count,
                                                            piece.offset + done)) {
                std::cerr << "Error: failed to load data range " << piece.range << " of backup '"
                          << chain.names[piece.backup] << "'\n";
                return false;
            }

            if (!writer.write(piece.origOffset + done, count, buf.data())) {
                return false;
            }

            done += count;
            progress.advance(count);
        }
    }

    progress.finish();
    return writer.finish();
}

// Opens the chain and checks it is one: a full backup or an incremental, then incrementals
// of the same disk.
// Nothing links an incremental to its parent in the format, so the order
// --source-names gives is taken as the chain of backups.
// A chain that starts with an incremental is not a whole disk, but merging it is still sound:
// the result applied onto the parent of its first backup leaves the disk each of them
// applied in turn would have left.
// That is what lets a repository be kept up to date one new backup at a time.
bool Tool::openChain(Abr::IBackupManager& manager, const StorageOptions& source,
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

uint32_t Tool::chainBufferSize(const BackupChain& chain)
{
    // A read never crosses a data range, so a buffer as large as the largest
    // block any of them stores is as much as one read can use.
    uint32_t bufferSize = 0;
    for (const auto& pBackup : chain.backups) {
        bufferSize = std::max(bufferSize, pBackup->getInfo().maxDataBlock);
    }
    return bufferSize ? bufferSize : kDefaultChunkSize;
}

bool Tool::checkRestoreNames(const CommonOptions& options)
{
    if (!options.storage.name.empty() && !options.sourceNames.empty()) {
        std::cerr << "Error: restore takes either --name or --source-names, not both\n";
        return false;
    }

    // Restore reads from one storage only, so a --source-<...> override would be ignored
    // and the chain silently read from somewhere else than the caller meant.
    const SourceStorageOptions& source = options.sourceStorage;

    if (source.mode || source.folder || source.path || source.key || source.endpoint || source.region
        || source.accessKey || source.secretKey || source.profile || source.pathStyle
        || source.verifyTls) {
        std::cerr << "Error: restore reads its backups from the storage --folder, --mode and --path\n"
                  << "       describe, with --key; the --source-<...> storage options are copy's\n";
        return false;
    }
    return true;
}

bool Tool::openRestoreChain(Abr::IBackupManager& manager, const CommonOptions& options,
                            BackupChain& chain, std::vector<MergedPiece>& plan, uint64_t& total)
{
    const std::vector<std::string> names =
        options.sourceNames.empty() ? std::vector<std::string>{options.storage.name} : options.sourceNames;

    if (!openChain(manager, options.storage, names, chain)) {
        return false;
    }

    // An incremental holds only what changed since its parent, and so does a chain that starts
    // with one: restored by itself it leaves every other block untouched, an empty disk if
    // the target started empty.
    // Applying one onto a target that already carries its parent is legitimate, and that is what
    // --force says.
    if (chain.backups.front()->getInfo().incremental && !options.force) {
        std::cerr << "Error: '" << chain.names.front()
                  << "' is an incremental backup and holds only the blocks that changed since\n"
                  << "       its parent. Restore the full backup to the target first, then apply\n"
                  << "       this one to the same target with --force, or restore the whole chain\n"
                  << "       at once with --source-names, the full backup first.\n";
        return false;
    }

    return buildCopyPlan(chain, plan, total);
}

std::string Tool::restoredFrom(const CommonOptions& options)
{
    if (options.sourceNames.empty()) {
        return "'" + backupLocation(options.storage) + "'";
    }

    std::string names;
    for (const auto& name : options.sourceNames) {
        names += (names.empty() ? "" : ", ") + name;
    }
    return "'" + storageLocation(options.storage) + "' (" + names + ")";
}
