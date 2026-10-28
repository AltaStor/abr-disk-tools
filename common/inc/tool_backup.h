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

    std::shared_ptr<Abr::IBackupManager> openManager(const StorageOptions& storage);

    std::string storageLocation(const StorageOptions& storage);
    std::string backupLocation(const StorageOptions& storage, const std::string& name);
    std::string backupLocation(const StorageOptions& storage);

    uint32_t alignBufferSize(uint32_t size, uint32_t sectorSize);

    // Whether the key a backup was opened with decrypts it; see the definition
    // for why this has to be asked before a restore touches its target.
    bool checkKey(Abr::IRestore& restore, const std::string& name, const std::string& key);

    bool copyToBackup(IDiskReader& reader, Abr::IBackup& backup, uint32_t bufferSize,
                      bool linePerUpdate);

    // Every backup of the chain stays open for the whole copy, since a block may
    // have to be read from any of them. Closing them from one place keeps the error
    // paths from having to.
    struct BackupChain {
        std::vector<std::string> names;
        std::vector<std::shared_ptr<Abr::IRestore>> backups;

        ~BackupChain()
        {
            for (const auto& pBackup : backups) {
                pBackup->close();
            }
        }
    };

    // The part of the image the copy has already claimed for a newer backup, as
    // non-overlapping [begin, end) intervals kept in ascending order.
    class CoveredRanges
    {
    public:
        struct Gap {
            uint64_t begin;
            uint64_t end;
        };

        // Adds [begin, end) and reports back the parts of it that nothing had taken
        // yet, in ascending order. Those are what still has to be read from
        // the backup the interval came from.
        void add(uint64_t begin, uint64_t end, std::vector<Gap>& gaps)
        {
            if (begin >= end) {
                return;
            }

            auto it = m_ranges.upper_bound(begin);

            // The interval before the first one starting past 'begin' may still
            // reach into it, or end exactly where it starts -- merging those two
            // keeps the map from growing an entry per data range.
            if (it != m_ranges.begin() && std::prev(it)->second >= begin) {
                --it;
            }

            uint64_t cursor = begin;
            uint64_t mergedBegin = begin;
            uint64_t mergedEnd = end;

            // Everything from here up to 'end' is absorbed into one interval, and
            // whatever lies between those intervals is what is left to copy.
            while (it != m_ranges.end() && it->first <= end) {
                if (it->first > cursor) {
                    gaps.push_back({cursor, it->first});
                }

                mergedBegin = std::min(mergedBegin, it->first);
                mergedEnd = std::max(mergedEnd, it->second);
                cursor = std::max(cursor, it->second);

                it = m_ranges.erase(it);
            }

            if (cursor < end) {
                gaps.push_back({cursor, end});
            }

            m_ranges[mergedBegin] = mergedEnd;
        }

    private:
        std::map<uint64_t, uint64_t> m_ranges; // begin -> end
    };

    // One run of bytes the copy takes from one data range of one backup of
    // the chain, and where in the image it belongs.
    struct MergedPiece {
        uint64_t origOffset; // offset in the original image
        uint32_t backup;     // index in the chain
        uint32_t range;      // data range index within that backup
        uint32_t offset;     // offset within that data range
        uint32_t length;
    };

    bool buildCopyPlan(BackupChain& chain, std::vector<MergedPiece>& plan, uint64_t& total);

    bool copyChainToBackup(BackupChain& chain, const std::vector<MergedPiece>& plan,
                           uint64_t total, Abr::IBackup& backup, uint32_t bufferSize,
                           bool linePerUpdate);
    bool copyChainToDisk(BackupChain& chain, const std::vector<MergedPiece>& plan,
                         uint64_t total, IDiskWriter& writer, uint32_t bufferSize,
                         bool linePerUpdate);
    bool openChain(Abr::IBackupManager& manager, const StorageOptions& source,
                   const std::vector<std::string>& names, BackupChain& chain);

    // The largest block any backup of the chain stores, which is as much as one read can use.
    uint32_t chainBufferSize(const BackupChain& chain);

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
