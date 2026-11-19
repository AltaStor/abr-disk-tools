// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// A chain of backups read as one disk image, which is how nbdkit serves a restore
//
#include "chain_reader.h"

#include <algorithm>
#include <cstring>
#include <iostream>

bool Chain::ChainReader::open(std::unique_ptr<BackupChain> chain)
{
    if (!chain || chain->backups.empty()) {
        std::cerr << "Error: no backup to read\n";
        return false;
    }

    std::vector<MergedPiece> plan;
    uint64_t total = 0;
    if (!buildCopyPlan(*chain, plan, total)) {
        return false;
    }

    // The plan names data ranges by index; their lengths say how much one decode yields.
    std::vector<std::vector<Abr::Range>> ranges(chain->backups.size());
    for (size_t i = 0; i < chain->backups.size(); ++i) {
        if (!chain->backups[i]->getDataRanges(ranges[i])) {
            std::cerr << "Error: failed to read the data ranges of backup '" << chain->names[i] << "'\n";
            return false;
        }
    }

    const Abr::BackupInfo& info = chain->backups.front()->getInfo();
    m_geometry.sectors = info.sectors;
    m_geometry.sectorSize = info.sectorSize;
    m_geometry.phySectorSize = info.phySectorSize;
    m_size = info.sectors * info.sectorSize;

    m_plan = std::move(plan);
    m_ranges = std::move(ranges);
    m_chunks.assign(chain->backups.size(), Chunk());
    m_chain = std::move(chain);
    m_chunkLoads = 0;
    return true;
}

bool Chain::ChainReader::dataRanges(std::vector<ByteRange>& ranges)
{
    ranges.clear();
    if (!m_chain) {
        std::cerr << "Error: range query on a chain that is not open\n";
        return false;
    }

    for (const MergedPiece& piece : m_plan) {
        if (!ranges.empty() && ranges.back().offset + ranges.back().length == piece.origOffset) {
            ranges.back().length += piece.length;
        } else {
            ranges.push_back({piece.origOffset, piece.length});
        }
    }
    return true;
}

bool Chain::ChainReader::read(uint64_t offset, uint32_t length, void* pBuf)
{
    if (!m_chain) {
        std::cerr << "Error: read on a chain that is not open\n";
        return false;
    }
    if (offset > m_size || length > m_size - offset) {
        std::cerr << "Error: a read of " << length << " bytes at offset " << offset << " is past the end of the "
                  << "image (" << m_size << " bytes)\n";
        return false;
    }

    auto* pOut = static_cast<uint8_t*>(pBuf);
    const uint64_t end = offset + length;

    // The first piece that reaches past the offset.
    auto it = std::upper_bound(m_plan.begin(), m_plan.end(), offset,
                               [](uint64_t value, const MergedPiece& piece) { return value < piece.origOffset; });
    if (it != m_plan.begin() && std::prev(it)->origOffset + std::prev(it)->length > offset) {
        --it;
    }

    uint64_t cursor = offset;
    while (cursor < end) {
        // No backup holds what lies before the next piece, so it reads as zeros.
        const uint64_t next = (it == m_plan.end()) ? end : std::min(it->origOffset, end);
        if (next > cursor) {
            std::memset(pOut + (cursor - offset), 0, next - cursor);
            cursor = next;
            continue;
        }

        const uint64_t skip = cursor - it->origOffset;
        const uint64_t count = std::min<uint64_t>(it->length - skip, end - cursor);
        const uint8_t* pData = chunk(it->backup, it->range);
        if (!pData) {
            return false;
        }
        std::memcpy(pOut + (cursor - offset), pData + it->offset + skip, count);
        cursor += count;
        ++it;
    }
    return true;
}

// The decoded data range of one backup, from the cache when it is the one decoded last.
const uint8_t* Chain::ChainReader::chunk(uint32_t backup, uint32_t range)
{
    Chunk& cached = m_chunks[backup];
    if (cached.range == static_cast<int64_t>(range)) {
        return cached.data.data();
    }

    const uint32_t length = m_ranges[backup][range].length;
    cached.range = -1;
    cached.data.resize(length);
    if (!m_chain->backups[backup]->loadDataRange(range, cached.data.data(), length, 0)) {
        std::cerr << "Error: failed to load data range " << range << " of backup '" << m_chain->names[backup] << "'\n";
        return nullptr;
    }

    cached.range = range;
    ++m_chunkLoads;
    return cached.data.data();
}
