// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// The copy loops, the storage they run against, and the chain 'copy' merges
//
#include "tool_backup.h"

#include <algorithm>
#include <iostream>

#include "plat.h"
#include "tool_commands.h"

bool Tool::interrupted()
{
    if (!Plat::interruptRequested()) {
        return false;
    }
    std::cerr << "\nInterrupted, " << context().branding.interruptNote << "\n";
    return true;
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
