// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// The copy loops, the storage they run against, and the chain 'copy' merges
//
#pragma once

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "abr_backup.h"
#include "chain.h"
#include "disk_device.h"
#include "tool_options.h"

namespace Tool
{
    // Reports the interruption once, at the point a copy loop notices it.
    // The catching is Plat::installInterruptHandler's; the wording comes from
    // the running tool's Branding, since what has to be released differs.
    bool interrupted();

    class ProgressPrinter
    {
    public:
        // Interactively each percentage rewrites the one before it, which needs
        // a carriage return. A --json caller is reading stdout a line at a time, so
        // there each gets a line of its own instead -- still plain "NN%" text,
        // ahead of the result document rather than part of it.
        ProgressPrinter(uint64_t total, bool linePerUpdate)
            : m_total(total)
            , m_terminator(linePerUpdate ? '\n' : '\r')
        {}

        void advance(uint64_t bytes)
        {
            m_done += bytes;

            int percent = m_total ? static_cast<int>(m_done * 100 / m_total) : 100;

            if (percent != m_lastPercent && percent < 100) {
                m_lastPercent = percent;
                std::cout << percent << "%" << m_terminator << std::flush;
            }
        }

        void finish() { std::cout << "100%\n" << std::flush; }

    private:
        uint64_t m_total;
        char m_terminator;
        uint64_t m_done = 0;
        int m_lastPercent = -1;
    };

    // The chain is abr-chain-reader's (chain.h), shared with the nbdkit plugin; the copy loops here drive it.
    using Chain::openManager;
    using Chain::checkKey;
    using Chain::BackupChain;
    using Chain::CoveredRanges;
    using Chain::MergedPiece;
    using Chain::buildCopyPlan;
    using Chain::openChain;
    using Chain::chainBufferSize;

    std::string storageLocation(const StorageOptions& storage);
    std::string backupLocation(const StorageOptions& storage, const std::string& name);
    std::string backupLocation(const StorageOptions& storage);

    uint32_t alignBufferSize(uint32_t size, uint32_t sectorSize);

    bool copyToBackup(IDiskReader& reader, Abr::IBackup& backup, uint32_t bufferSize,
                      bool linePerUpdate);

    bool copyChainToBackup(BackupChain& chain, const std::vector<MergedPiece>& plan,
                           uint64_t total, Abr::IBackup& backup, uint32_t bufferSize,
                           bool linePerUpdate);
    bool copyChainToDisk(BackupChain& chain, const std::vector<MergedPiece>& plan,
                         uint64_t total, IDiskWriter& writer, uint32_t bufferSize,
                         bool linePerUpdate);

    // Whether restore was given what it applies in one way only: one backup by --name, or a
    // chain by --source-names, read from the storage the plain storage options describe.
    bool checkRestoreNames(const CommonOptions& options);

    // Opens what restore applies and works out where every block of it comes from, refusing
    // an incremental at the start of it unless --force says the target holds its parent.
    bool openRestoreChain(Abr::IBackupManager& manager, const CommonOptions& options,
                          BackupChain& chain, std::vector<MergedPiece>& plan, uint64_t& total);

    // How restore names what it applied: the backup, or the storage and the chain read from it.
    std::string restoredFrom(const CommonOptions& options);
}
