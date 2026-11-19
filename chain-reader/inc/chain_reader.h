// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// A chain of backups read as one disk image, which is how nbdkit serves a restore
//
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "chain.h"
#include "disk_device.h"

namespace Chain
{
    //
    // Reads a chain as the disk it restores to: a block comes from the newest backup that holds it, as in a restore of
    // the whole chain, and a block that no backup holds reads as zeros.
    //
    // backuplib decodes a whole stored chunk on every read, however little of it is wanted. The reader keeps the last
    // decoded chunk of each backup, so a run of small reads inside one chunk costs one decode.
    //
    // Like IRestore, one instance serves one caller at a time. It writes nothing to the repository.
    //
    class ChainReader final : public IDiskReader
    {
    public:
        // Takes a chain that openChain opened and checked, and works out where every block comes from.
        bool open(std::unique_ptr<BackupChain> chain);

        const DiskGeometry& geometry() const override { return m_geometry; }

        // The ranges the chain holds data for, merged where they touch.
        bool dataRanges(std::vector<ByteRange>& ranges) override;

        bool read(uint64_t offset, uint32_t length, void* pBuf) override;

        // Stored chunks decoded so far.
        uint64_t chunkLoads() const { return m_chunkLoads; }

    private:
        // The last decoded chunk of one backup, by its data range index.
        struct Chunk {
            int64_t range = -1;
            std::vector<uint8_t> data;
        };

        const uint8_t* chunk(uint32_t backup, uint32_t range);

        std::unique_ptr<BackupChain> m_chain;
        // Sorted by image offset, without overlaps.
        std::vector<MergedPiece> m_plan;
        std::vector<std::vector<Abr::Range>> m_ranges;
        std::vector<Chunk> m_chunks;
        DiskGeometry m_geometry;
        uint64_t m_size = 0;
        uint64_t m_chunkLoads = 0;
    };
}
