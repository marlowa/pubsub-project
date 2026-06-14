// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/WalReader.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

namespace pubsub_itc_fw::tests {

namespace {

// Large segment used for most tests (no rollover).
constexpr size_t segment_size = 4096;

// Small segment for rollover tests.
// Each entry with a 4-byte payload is 24 (header) + 4 (payload) + 4 (CRC) = 32 bytes.
// With small_segment_size=128, exactly 4 entries fit per segment before the next roll.
constexpr size_t small_segment_size = 128;

struct Captured {
    int64_t id;
    std::vector<uint8_t> data;
};

WalReader::EntryCallback capture(std::vector<Captured>& out) {
    return [&out](int64_t id, const void* payload, size_t size) {
        const auto* p = static_cast<const uint8_t*>(payload);
        out.push_back({id, {p, p + size}});
    };
}

} // namespaces

class WalTest : public ::testing::Test {
  protected:
    void SetUp() override {
        std::string tmpl = "/dev/shm/wal_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
        dir_ = tmpl;
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    std::string dir_;
};

// ---------------------------------------------------------------------------
// WalWriter: construction and basic state
// ---------------------------------------------------------------------------

TEST_F(WalTest, NotOpenByDefault) {
    WalWriter w;
    EXPECT_FALSE(w.is_open());
}

TEST_F(WalTest, IsOpenAfterOpen) {
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});
    EXPECT_TRUE(w.is_open());
}

TEST_F(WalTest, OpenCreatesSegmentZeroFile) {
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});
    EXPECT_TRUE(std::filesystem::exists(dir_ + "/wal_000000.log"));
}

TEST_F(WalTest, OpenSegmentFileHasCorrectSize) {
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});
    EXPECT_EQ(std::filesystem::file_size(dir_ + "/wal_000000.log"), segment_size);
}

TEST_F(WalTest, SegmentSizeTooSmallThrows) {
    WalWriter w;
    EXPECT_THROW(w.open(dir_, 10, {0, 0}), pubsub_itc_fw::PreconditionAssertion);
}

TEST_F(WalTest, CurrentPositionZeroAfterFreshOpen) {
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});
    const WalPosition pos = w.current_position();
    EXPECT_EQ(pos.segment, 0u);
    EXPECT_EQ(pos.offset, 0u);
}

TEST_F(WalTest, AppendAdvancesOffset) {
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});
    const uint32_t payload = 0xDEADBEEFu;
    w.append(1, &payload, sizeof(payload));
    EXPECT_GT(w.current_position().offset, 0u);
}

TEST_F(WalTest, TwoAppendsAdvanceOffsetFurther) {
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});
    const uint32_t payload = 1u;
    w.append(1, &payload, sizeof(payload));
    const size_t after_one = w.current_position().offset;
    w.append(2, &payload, sizeof(payload));
    EXPECT_GT(w.current_position().offset, after_one);
}

TEST_F(WalTest, OversizedPayloadThrows) {
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});
    std::vector<uint8_t> big(segment_size + 1, 0xFFu);
    EXPECT_THROW(w.append(1, big.data(), big.size()), pubsub_itc_fw::PreconditionAssertion);
}

// ---------------------------------------------------------------------------
// WalReader: empty / missing directory
// ---------------------------------------------------------------------------

TEST_F(WalTest, ReplayMissingDirectoryReturnsFrom) {
    const WalPosition from{0, 0};
    const WalPosition end = WalReader::replay("/dev/shm/wal_no_such_dir_xyz_abc", from, nullptr);
    EXPECT_EQ(end.segment, from.segment);
    EXPECT_EQ(end.offset, from.offset);
}

TEST_F(WalTest, ReplayEmptyDirectoryReturnsFrom) {
    const WalPosition from{0, 0};
    const WalPosition end = WalReader::replay(dir_, from, nullptr);
    EXPECT_EQ(end.segment, from.segment);
    EXPECT_EQ(end.offset, from.offset);
}

TEST_F(WalTest, ReplayNullCallbackDoesNotCrash) {
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});
    const uint32_t payload = 42u;
    w.append(1, &payload, sizeof(payload));
    WalPosition end{};
    EXPECT_NO_THROW(end = WalReader::replay(dir_, {0, 0}, nullptr));
    EXPECT_GT(end.offset, 0u);
}

// ---------------------------------------------------------------------------
// Round-trip: write then replay
// ---------------------------------------------------------------------------

TEST_F(WalTest, SingleRecordRoundTrip) {
    const uint32_t val = 0xCAFEBABEu;
    {
        WalWriter w;
        w.open(dir_, segment_size, {0, 0});
        w.append(99, &val, sizeof(val));
    }
    std::vector<Captured> entries;
    const WalPosition end = WalReader::replay(dir_, {0, 0}, capture(entries));
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].id, 99);
    ASSERT_EQ(entries[0].data.size(), sizeof(val));
    EXPECT_EQ(std::memcmp(entries[0].data.data(), &val, sizeof(val)), 0);
    EXPECT_GT(end.offset, 0u);
}

TEST_F(WalTest, MultipleRecordsReplayedInOrder) {
    constexpr int N = 10;
    {
        WalWriter w;
        w.open(dir_, segment_size, {0, 0});
        for (int i = 0; i < N; ++i) {
            const uint32_t v = static_cast<uint32_t>(i * 100);
            w.append(static_cast<int64_t>(i), &v, sizeof(v));
        }
    }
    std::vector<Captured> entries;
    const WalPosition end = WalReader::replay(dir_, {0, 0}, capture(entries));
    ASSERT_EQ(entries.size(), static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(entries[i].id, i);
        uint32_t v{};
        std::memcpy(&v, entries[i].data.data(), sizeof(v));
        EXPECT_EQ(v, static_cast<uint32_t>(i * 100));
    }
    EXPECT_EQ(end.segment, 0u);
    EXPECT_GT(end.offset, 0u);
}

TEST_F(WalTest, VariablePayloadSizesRoundTrip) {
    std::vector<std::vector<uint8_t>> payloads = {
        {0x01},
        {0x02, 0x03},
        {0x04, 0x05, 0x06, 0x07, 0x08},
        {0x09, 0x0A, 0x0B},
    };
    {
        WalWriter w;
        w.open(dir_, segment_size, {0, 0});
        for (int i = 0; i < static_cast<int>(payloads.size()); ++i) {
            w.append(static_cast<int64_t>(i), payloads[i].data(), payloads[i].size());
        }
    }
    std::vector<Captured> entries;
    const WalPosition end = WalReader::replay(dir_, {0, 0}, capture(entries));
    ASSERT_EQ(entries.size(), payloads.size());
    for (size_t i = 0; i < payloads.size(); ++i) {
        EXPECT_EQ(entries[i].data, payloads[i]);
    }
    EXPECT_GT(end.offset, 0u);
}

TEST_F(WalTest, ReplayReturnsEndPositionMatchingWriter) {
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});
    const uint32_t payload = 1u;
    w.append(1, &payload, sizeof(payload));
    w.append(2, &payload, sizeof(payload));
    const WalPosition writer_pos = w.current_position();

    const WalPosition replay_end = WalReader::replay(dir_, {0, 0}, nullptr);
    EXPECT_EQ(replay_end.segment, writer_pos.segment);
    EXPECT_EQ(replay_end.offset, writer_pos.offset);
}

TEST_F(WalTest, ResumeWritingFromReplayPosition) {
    // Write records 1-3, close writer, open a new writer at the replay end,
    // write records 4-5. Full replay from {0,0} should yield all 5.
    {
        WalWriter w;
        w.open(dir_, segment_size, {0, 0});
        for (int i = 1; i <= 3; ++i) {
            const uint32_t v = static_cast<uint32_t>(i);
            w.append(i, &v, sizeof(v));
        }
    }
    const WalPosition mid = WalReader::replay(dir_, {0, 0}, nullptr);
    {
        WalWriter w;
        w.open(dir_, segment_size, mid);
        for (int i = 4; i <= 5; ++i) {
            const uint32_t v = static_cast<uint32_t>(i);
            w.append(i, &v, sizeof(v));
        }
    }
    std::vector<Captured> entries;
    const WalPosition final_end = WalReader::replay(dir_, {0, 0}, capture(entries));
    ASSERT_EQ(entries.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(entries[i].id, i + 1);
    }
    EXPECT_GT(final_end.offset, mid.offset);
}

// ---------------------------------------------------------------------------
// Segment rollover
// ---------------------------------------------------------------------------

TEST_F(WalTest, SegmentRolloverCreatesSecondFile) {
    // small_segment_size=128 holds exactly 4 entries of 32 bytes each.
    // Writing a 5th entry forces creation of wal_000001.log.
    WalWriter w;
    w.open(dir_, small_segment_size, {0, 0});
    const uint32_t v = 0u;
    for (int i = 0; i < 5; ++i) {
        w.append(i, &v, sizeof(v));
    }
    EXPECT_TRUE(std::filesystem::exists(dir_ + "/wal_000001.log"));
}

TEST_F(WalTest, SegmentRolloverAllRecordsReplayed) {
    // Write 6 records across 2 segments; verify all 6 survive replay.
    constexpr int N = 6;
    {
        WalWriter w;
        w.open(dir_, small_segment_size, {0, 0});
        for (int i = 1; i <= N; ++i) {
            const uint32_t v = static_cast<uint32_t>(i);
            w.append(i, &v, sizeof(v));
        }
    }
    std::vector<Captured> entries;
    const WalPosition end = WalReader::replay(dir_, {0, 0}, capture(entries));
    ASSERT_EQ(entries.size(), static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(entries[i].id, i + 1);
    }
    EXPECT_EQ(end.segment, 1u);
}

TEST_F(WalTest, SegmentRolloverWriterPositionIsOnSecondSegment) {
    WalWriter w;
    w.open(dir_, small_segment_size, {0, 0});
    const uint32_t v = 0u;
    for (int i = 0; i < 5; ++i) {
        w.append(i, &v, sizeof(v));
    }
    EXPECT_EQ(w.current_position().segment, 1u);
}

// ---------------------------------------------------------------------------
// Replay from a non-zero anchor
// ---------------------------------------------------------------------------

TEST_F(WalTest, ReplayFromAnchorSkipsEarlierRecords) {
    // Write 5 records, snapshot the position after record 3, replay from there.
    WalPosition anchor{};
    {
        WalWriter w;
        w.open(dir_, segment_size, {0, 0});
        for (int i = 1; i <= 5; ++i) {
            const uint32_t v = static_cast<uint32_t>(i);
            w.append(i, &v, sizeof(v));
            if (i == 3) {
                anchor = w.current_position();
            }
        }
    }
    std::vector<Captured> entries;
    const WalPosition end = WalReader::replay(dir_, anchor, capture(entries));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].id, 4);
    EXPECT_EQ(entries[1].id, 5);
    EXPECT_GT(end.offset, anchor.offset);
}

TEST_F(WalTest, ReplayFromEndPositionYieldsNoEntries) {
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});
    const uint32_t v = 1u;
    w.append(1, &v, sizeof(v));
    const WalPosition end = w.current_position();

    std::vector<Captured> entries;
    const WalPosition end2 = WalReader::replay(dir_, end, capture(entries));
    EXPECT_EQ(entries.size(), 0u);
    EXPECT_EQ(end2.segment, end.segment);
    EXPECT_EQ(end2.offset, end.offset);
}

// ---------------------------------------------------------------------------
// CRC corruption
// ---------------------------------------------------------------------------

TEST_F(WalTest, CrcCorruptionStopsReplay) {
    // Write records 1 and 2. Corrupt a payload byte in record 2.
    // Replay should return only record 1, with end == position after record 1.
    WalPosition pos_after_first{};
    {
        WalWriter w;
        w.open(dir_, segment_size, {0, 0});
        const uint32_t v1 = 0xAAAAAAAAu;
        w.append(1, &v1, sizeof(v1));
        pos_after_first = w.current_position();
        const uint32_t v2 = 0xBBBBBBBBu;
        w.append(2, &v2, sizeof(v2));
    }

    // Record 2 starts at pos_after_first.offset. Its payload begins 24 bytes in
    // (after the WalEntryHeader). Flip the first payload byte.
    const std::string seg0 = dir_ + "/wal_000000.log";
    const int fd = ::open(seg0.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    const off_t corrupt_at = static_cast<off_t>(pos_after_first.offset) + 24;
    uint8_t flipped = 0xFFu;
    ASSERT_EQ(::pwrite(fd, &flipped, 1, corrupt_at), 1);
    ::close(fd);

    std::vector<Captured> entries;
    const WalPosition end = WalReader::replay(dir_, {0, 0}, capture(entries));
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].id, 1);
    EXPECT_EQ(end.segment, pos_after_first.segment);
    EXPECT_EQ(end.offset, pos_after_first.offset);
}

// ---------------------------------------------------------------------------
// Determinism
// ---------------------------------------------------------------------------

TEST_F(WalTest, ReplayIsDeterministic) {
    {
        WalWriter w;
        w.open(dir_, segment_size, {0, 0});
        for (int i = 1; i <= 5; ++i) {
            const uint32_t v = static_cast<uint32_t>(i);
            w.append(i, &v, sizeof(v));
        }
    }
    std::vector<Captured> first, second;
    const WalPosition end1 = WalReader::replay(dir_, {0, 0}, capture(first));
    const WalPosition end2 = WalReader::replay(dir_, {0, 0}, capture(second));
    ASSERT_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].id, second[i].id);
        EXPECT_EQ(first[i].data, second[i].data);
    }
    EXPECT_EQ(end1.segment, end2.segment);
    EXPECT_EQ(end1.offset, end2.offset);
}

} // namespaces
