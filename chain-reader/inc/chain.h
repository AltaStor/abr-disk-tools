// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// A chain of backups: the storage it is read from, opened and checked, and the plan that merges it
//
// abr-chain-reader has no CLI around it and does not depend on Tool::context(), so that the nbdkit plugin can read a
// chain as the tools do. The tools reach these through the same names in Tool, see tool_backup.h.
//
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "abr_backup.h"

namespace Chain
{
    constexpr uint32_t kDefaultChunkSize = 4 * 1024 * 1024;
    constexpr const char* kDefaultMode = "file";
    constexpr int kDefaultCompression = 3;

    // The longest a tool or the nbdkit plugin waits for a repository that does not answer: a day.
    constexpr uint32_t kMaxStallSeconds = 86400;

    struct StorageOptions {
        std::string mode = kDefaultMode;
        std::string folder;
        std::string path;
        std::string name;
        std::string key;
        int compression = kDefaultCompression;

        std::string endpoint;
        std::string region;
        std::string accessKey;
        std::string secretKey;
        std::string profile;
        bool pathStyle = false;
        bool verifyTls = true;
        bool createFolder = false;

        // How long requests go on trying while the repository does not answer, in seconds, up to kMaxStallSeconds;
        // 0 gives up within seconds. The stop check ends the wait at once.
        uint32_t stallSeconds = 0;
        std::function<bool()> stopRequested;
    };

    std::shared_ptr<Abr::IBackupManager> openManager(const StorageOptions& storage);

    // Whether the key a backup was opened with decrypts it; see the definition
    // for why this has to be asked before a restore touches its target.
    bool checkKey(Abr::IRestore& restore, const std::string& name, const std::string& key);

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

    bool openChain(Abr::IBackupManager& manager, const StorageOptions& source,
                   const std::vector<std::string>& names, BackupChain& chain);

    // The largest block any backup of the chain stores, which is as much as one read can use.
    uint32_t chainBufferSize(const BackupChain& chain);
}
