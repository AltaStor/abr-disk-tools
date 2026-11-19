// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// ChainReader against chains written with backuplib into a file repository, compared with the image the same writes
// make in memory
//
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "chain.h"
#include "chain_reader.h"

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kImageSize = 32ull * 1024 * 1024;
constexpr uint64_t kKiB = 1024;
constexpr uint64_t kMiB = 1024 * 1024;

// One write of a backup: where, how much, and the seed of its bytes.
struct Extent {
    uint64_t offset;
    uint64_t length;
    uint8_t seed;
};

// Bytes that depend on their position, so that data copied to the wrong place does not compare equal.
uint8_t patternByte(uint8_t seed, uint64_t position)
{
    return static_cast<uint8_t>(seed + position * 7 + (position >> 12) * 13);
}

class ChainReaderTest : public testing::Test
{
protected:
    void SetUp() override
    {
        m_folder = fs::current_path() / ("chain-test-" + std::string(testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(m_folder);
        fs::create_directories(m_folder);
        fs::permissions(m_folder, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
        m_image.assign(kImageSize, 0);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::permissions(m_folder, fs::perms::owner_all, ec);
        fs::remove_all(m_folder, ec);
    }

    Chain::StorageOptions storage(const std::string& key = {}, bool create = false) const
    {
        Chain::StorageOptions options;
        options.folder = m_folder.string();
        options.key = key;
        options.createFolder = create;
        return options;
    }

    // Writes a backup of the extents with backuplib, and applies them to the image a restore of the chain makes.
    void writeBackup(const std::string& name, bool incremental, const std::vector<Extent>& extents,
                     const std::string& key = {})
    {
        auto pManager = Chain::openManager(storage(key, true));
        ASSERT_TRUE(pManager);

        Abr::BackupInfo info = {};
        info.sectors = kImageSize / 512;
        info.sectorSize = 512;
        info.phySectorSize = 512;
        info.compression = 3;
        info.encryption = key.empty() ? 0 : 1;
        info.incremental = incremental ? 1 : 0;

        std::shared_ptr<Abr::IBackup> pBackup;
        ASSERT_TRUE(pManager->createBackup("", name, info, pBackup, key));
        for (const Extent& extent : extents) {
            std::vector<uint8_t> data(extent.length);
            for (uint64_t i = 0; i < extent.length; ++i) {
                data[i] = patternByte(extent.seed, extent.offset + i);
            }
            ASSERT_TRUE(pBackup->saveDataRange(data.data(), static_cast<uint32_t>(data.size()), extent.offset));
            std::memcpy(m_image.data() + extent.offset, data.data(), data.size());
        }
        ASSERT_TRUE(pBackup->finalizeBackup(false));
    }

    // A full and two incrementals that overlap it and each other.
    void writeChain(const std::string& key = {})
    {
        writeBackup("full", false, {{0, 1 * kMiB, 0x11}, {8 * kMiB, 64 * kKiB, 0x22}}, key);
        writeBackup("inc1", true, {{512 * kKiB, 128 * kKiB, 0x33}, {16 * kMiB, 4 * kKiB, 0x44}}, key);
        writeBackup("inc2", true, {{0, 4 * kKiB, 0x55}, {8 * kMiB + 32 * kKiB, 64 * kKiB, 0x66}}, key);
    }

    bool openReader(Chain::ChainReader& reader, const std::vector<std::string>& names, const std::string& key = {})
    {
        auto pManager = Chain::openManager(storage(key));
        if (!pManager) {
            return false;
        }
        auto pChain = std::make_unique<Chain::BackupChain>();
        return Chain::openChain(*pManager, storage(key), names, *pChain) && reader.open(std::move(pChain));
    }

    // Reads the whole image in pieces of the given size and compares it with the expected one.
    void expectImage(Chain::ChainReader& reader, uint32_t pieceSize)
    {
        std::vector<uint8_t> buffer(pieceSize);
        for (uint64_t offset = 0; offset < kImageSize; offset += pieceSize) {
            ASSERT_TRUE(reader.read(offset, pieceSize, buffer.data()));
            ASSERT_EQ(0, std::memcmp(buffer.data(), m_image.data() + offset, pieceSize)) << "at offset " << offset;
        }
    }

    fs::path m_folder;
    std::vector<uint8_t> m_image;
};

TEST_F(ChainReaderTest, ReadsTheNewestBlockAndZerosElsewhere)
{
    writeChain();
    Chain::ChainReader reader;
    ASSERT_TRUE(openReader(reader, {"full", "inc1", "inc2"}));

    EXPECT_EQ(kImageSize / 512, reader.geometry().sectors);
    EXPECT_EQ(512u, reader.geometry().sectorSize);
    expectImage(reader, 1 * kMiB);
}

TEST_F(ChainReaderTest, UnalignedReadsAcrossPiecesAndGaps)
{
    writeChain();
    Chain::ChainReader reader;
    ASSERT_TRUE(openReader(reader, {"full", "inc1", "inc2"}));

    // Reads that start and end anywhere, across the boundaries of the pieces and the gaps between them.
    const std::vector<std::pair<uint64_t, uint32_t>> reads = {
        {0, 1}, {4095, 2}, {4 * kKiB - 1, 600 * 1024}, {512 * kKiB - 3, 128 * 1024 + 6}, {1 * kMiB - 100, 200},
        {8 * kMiB - 7, 100 * 1024}, {8 * kMiB + 95 * kKiB, 2 * 1024}, {16 * kMiB + 4095, 1}, {kImageSize - 1, 1},
    };
    for (const auto& [offset, length] : reads) {
        std::vector<uint8_t> buffer(length, 0xee);
        ASSERT_TRUE(reader.read(offset, length, buffer.data()));
        EXPECT_EQ(0, std::memcmp(buffer.data(), m_image.data() + offset, length))
            << "read of " << length << " bytes at " << offset;
    }
}

TEST_F(ChainReaderTest, DataRangesAreWhatTheChainCovers)
{
    writeChain();
    Chain::ChainReader reader;
    ASSERT_TRUE(openReader(reader, {"full", "inc1", "inc2"}));

    std::vector<ByteRange> ranges;
    ASSERT_TRUE(reader.dataRanges(ranges));
    ASSERT_EQ(3u, ranges.size());
    EXPECT_EQ(0u, ranges[0].offset);
    EXPECT_EQ(1 * kMiB, ranges[0].length);
    EXPECT_EQ(8 * kMiB, ranges[1].offset);
    EXPECT_EQ(96 * kKiB, ranges[1].length);
    EXPECT_EQ(16 * kMiB, ranges[2].offset);
    EXPECT_EQ(4 * kKiB, ranges[2].length);
}

TEST_F(ChainReaderTest, SmallReadsDecodeEachChunkOnce)
{
    writeChain();
    Chain::ChainReader reader;
    ASSERT_TRUE(openReader(reader, {"full", "inc1", "inc2"}));

    // The first MiB comes from three chunks: inc2's, the full's around inc1's, and inc1's. The full's is needed
    // again after inc1's, and its cache entry still holds it.
    std::vector<uint8_t> buffer(4 * kKiB);
    for (uint64_t offset = 0; offset < 1 * kMiB; offset += buffer.size()) {
        ASSERT_TRUE(reader.read(offset, static_cast<uint32_t>(buffer.size()), buffer.data()));
    }
    EXPECT_EQ(3u, reader.chunkLoads());
}

TEST_F(ChainReaderTest, EncryptedChainNeedsItsKey)
{
    writeChain("chain-key");

    Chain::ChainReader wrong;
    EXPECT_FALSE(openReader(wrong, {"full", "inc1", "inc2"}, "another-key"));
    Chain::ChainReader missing;
    EXPECT_FALSE(openReader(missing, {"full", "inc1", "inc2"}));

    Chain::ChainReader reader;
    ASSERT_TRUE(openReader(reader, {"full", "inc1", "inc2"}, "chain-key"));
    expectImage(reader, 1 * kMiB);
}

TEST_F(ChainReaderTest, EmptyIncrementalChangesNothing)
{
    writeChain();
    writeBackup("inc3", true, {});
    Chain::ChainReader reader;
    ASSERT_TRUE(openReader(reader, {"full", "inc1", "inc2", "inc3"}));

    std::vector<ByteRange> ranges;
    ASSERT_TRUE(reader.dataRanges(ranges));
    EXPECT_EQ(3u, ranges.size());
    expectImage(reader, 1 * kMiB);
}

#ifndef _WIN32
// A restore reads the repository through a read-only mount. Mode bits stand in for it here: a process that is not
// root cannot write to a folder without the write bit.
TEST_F(ChainReaderTest, ReadsFromAReadOnlyFolder)
{
    writeChain();

    auto listing = [this]() {
        std::set<std::string> entries;
        for (const auto& entry : fs::directory_iterator(m_folder)) {
            entries.insert(entry.path().filename().string() + ":" + std::to_string(entry.file_size()) + ":"
                           + std::to_string(entry.last_write_time().time_since_epoch().count()));
        }
        return entries;
    };
    const auto before = listing();
    fs::permissions(m_folder, fs::perms::owner_read | fs::perms::owner_exec);

    {
        Chain::ChainReader reader;
        ASSERT_TRUE(openReader(reader, {"full", "inc1", "inc2"}));
        expectImage(reader, 256 * kKiB);
    }

    fs::permissions(m_folder, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    EXPECT_EQ(before, listing());
}
#endif

TEST_F(ChainReaderTest, RefusesReadsPastTheEndAndWhenNotOpen)
{
    Chain::ChainReader unopened;
    std::vector<ByteRange> ranges;
    uint8_t byte = 0;
    EXPECT_EQ(0u, unopened.geometry().sectors);
    EXPECT_FALSE(unopened.dataRanges(ranges));
    EXPECT_FALSE(unopened.read(0, 1, &byte));
    EXPECT_FALSE(unopened.open(nullptr));

    writeChain();
    Chain::ChainReader reader;
    ASSERT_TRUE(openReader(reader, {"full"}));
    EXPECT_FALSE(reader.read(kImageSize, 1, &byte));
    EXPECT_FALSE(reader.read(kImageSize - 1, 2, &byte));
}

} // namespace

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
