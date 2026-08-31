// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/WalReader.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>
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
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("wal_test");
    }

    void TearDown() override {
        pubsub_itc_fw::tests_common::remove_scratch_directory(dir_);
    }

    std::string dir_;
};

// WalWriter: construction and basic state

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

TEST_F(WalTest, ANewSegmentHasItsBlocksAlreadyAllocated) {
    // The property this exists to keep. A segment created with ftruncate alone is SPARSE: the
    // size is set and no block is allocated, so every page gets its block on first write --
    // inside a page fault, on whichever thread happens to be appending. Allocating changes
    // filesystem metadata, metadata changes need the journal, and waiting for a journal
    // transaction is uninterruptible: measured on the sequencer, which appends every order the
    // venue takes, those waits reached 557 ms. See docs/bug_list.md BUG-0070.
    //
    // st_blocks counts the 512-byte units actually allocated, so a sparse file reports far
    // fewer than its size implies and a written one reports at least as many. That is the
    // difference being asserted, and it is what a return to ftruncate would break.
    WalWriter w;
    w.open(dir_, segment_size, {0, 0});

    struct stat info {};
    ASSERT_EQ(::stat((dir_ + "/wal_000000.log").c_str(), &info), 0);
    const blkcnt_t needed = static_cast<blkcnt_t>(segment_size / 512);
    EXPECT_GE(info.st_blocks, needed) << "the segment is sparse: " << info.st_blocks << " blocks allocated of " << needed
                                      << " the file claims. Appending to it will allocate on the writing thread.";
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

// WalReader: empty / missing directory

TEST_F(WalTest, ReplayMissingDirectoryReturnsFrom) {
    const WalPosition from{0, 0};
    const WalPosition end = WalReader::replay(pubsub_itc_fw::tests_common::scratch_path_that_does_not_exist("wal_dir"), from, nullptr);
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

// Round-trip: write then replay

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

// Segment rollover

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

// Replay from a non-zero anchor

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

// CRC corruption

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

// Determinism

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

// Preparing the next segment ahead of the writer (BUG-0070).
//
// Filling a segment removes the per-page cost of a sparse file, but if the writer does the
// filling at roll-over it pays the whole segment's allocation in one go, on the order path. A
// twenty-minute run showed that exactly: 673 appends over 1 ms against 684 rotations, one slow
// append per roll. These cases are about the fill happening on the helper instead, and about
// the writer being correct whether or not it does.

TEST_F(WalTest, TheSegmentAfterTheCurrentOneIsReadyBeforeItIsNeeded) {
    WalWriter w;
    w.open(dir_, small_segment_size, {0, 0});

    ASSERT_TRUE(w.wait_until_prepared(std::chrono::seconds{5})) << "the helper never prepared a segment";

    // Ready means created, filled and mapped -- so it is on disk with its blocks allocated
    // before a single append has gone anywhere near it.
    struct stat info {};
    ASSERT_EQ(::stat((dir_ + "/wal_000001.log").c_str(), &info), 0) << "segment 1 was not created ahead of time";
    EXPECT_EQ(static_cast<size_t>(info.st_size), small_segment_size);
    EXPECT_GE(info.st_blocks, static_cast<blkcnt_t>(small_segment_size / 512))
        << "segment 1 was prepared but left sparse, so the writer will still allocate on the order path";
}

TEST_F(WalTest, RollingOverAdoptsThePreparedSegmentRatherThanMakingOne) {
    WalWriter w;
    w.open(dir_, small_segment_size, {0, 0});
    ASSERT_TRUE(w.wait_until_prepared(std::chrono::seconds{5}));

    // Only the first segment should ever be made by the writer: nothing can prepare that one.
    EXPECT_EQ(w.segments_filled_inline(), 1U);

    const std::vector<uint8_t> payload(16, 0xABu);
    while (w.current_position().segment == 0) {
        w.append(1, payload.data(), payload.size());
    }

    EXPECT_EQ(w.current_position().segment, 1U);
    EXPECT_EQ(w.segments_filled_inline(), 1U) << "the writer filled a segment itself, so preparation is not keeping pace";
}

TEST_F(WalTest, RecordsSurviveManyRollsWhilePreparationRunsAlongside) {
    // The case that does not care about timing. Whatever the two threads did between them, the
    // log either reads back exactly what was appended or it does not.
    constexpr int record_count = 5000;

    WalWriter w;
    w.open(dir_, small_segment_size, {0, 0});

    std::vector<uint8_t> payload(24, 0);
    for (int i = 0; i < record_count; ++i) {
        payload[0] = static_cast<uint8_t>(i & 0xFF);
        w.append(i, payload.data(), payload.size());
    }
    ASSERT_GT(w.current_position().segment, 10U) << "too few rolls to exercise the handoff";

    std::vector<int64_t> seen;
    const WalPosition replayed = WalReader::replay(dir_, {0, 0}, [&](int64_t id, const void*, size_t) { seen.push_back(id); });
    EXPECT_GT(replayed.segment, 0U);

    ASSERT_EQ(seen.size(), static_cast<size_t>(record_count));
    for (int i = 0; i < record_count; ++i) {
        ASSERT_EQ(seen[static_cast<size_t>(i)], i) << "records came back out of order at " << i;
    }
}

TEST_F(WalTest, WithPreparationOffTheWriterStillProducesAllocatedSegments) {
    // The fallback, which is what runs whenever the helper is not ready. It must be correct on
    // its own: slower at each roll, but never sparse.
    WalWriter w;
    w.disable_preparation();
    w.open(dir_, small_segment_size, {0, 0});

    const std::vector<uint8_t> payload(16, 0xCDu);
    while (w.current_position().segment < 3) {
        w.append(1, payload.data(), payload.size());
    }

    EXPECT_EQ(w.segments_filled_inline(), 4U) << "with preparation off the writer should have made every segment";

    for (int seg = 0; seg <= 3; ++seg) {
        const std::string path = fmt::format("{}/wal_{:06}.log", dir_, seg);
        struct stat info {};
        ASSERT_EQ(::stat(path.c_str(), &info), 0) << path;
        EXPECT_GE(info.st_blocks, static_cast<blkcnt_t>(small_segment_size / 512)) << path << " is sparse";
    }
}

TEST_F(WalTest, AnUnusedPreparedSegmentReadsAsEmptyAndIsResumedInto) {
    // Shutdown leaves the prepared segment on disk deliberately. Replay must not mistake a
    // filled-with-zeros segment for data, and the position it reports must be somewhere the
    // writer can carry on from.
    WalPosition end;
    {
        WalWriter w;
        w.open(dir_, small_segment_size, {0, 0});
        ASSERT_TRUE(w.wait_until_prepared(std::chrono::seconds{5}));
        const std::vector<uint8_t> payload(16, 0x5Au);
        w.append(7, payload.data(), payload.size());
    }

    ASSERT_TRUE(std::filesystem::exists(dir_ + "/wal_000001.log")) << "the prepared segment should be left for the next start";

    std::vector<int64_t> seen;
    end = WalReader::replay(dir_, {0, 0}, [&](int64_t id, const void*, size_t) { seen.push_back(id); });
    ASSERT_EQ(seen.size(), 1U) << "the zero-filled prepared segment was replayed as though it held records";
    EXPECT_EQ(seen[0], 7);

    // Resuming from where replay left off must work, and must find the records already there.
    WalWriter again;
    again.open(dir_, small_segment_size, end);
    const std::vector<uint8_t> more(16, 0x77u);
    again.append(8, more.data(), more.size());

    std::vector<int64_t> after;
    const WalPosition final_position = WalReader::replay(dir_, {0, 0}, [&](int64_t id, const void*, size_t) { after.push_back(id); });
    EXPECT_GT(final_position.offset, 0U);
    ASSERT_EQ(after.size(), 2U);
    EXPECT_EQ(after[0], 7);
    EXPECT_EQ(after[1], 8);
}

TEST_F(WalTest, ClosingWithAPreparedSegmentOutstandingLeaksNoDescriptors) {
    const auto open_descriptors = []() {
        size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
            (void)entry;
            ++count;
        }
        return count;
    };

    const size_t before = open_descriptors();
    {
        WalWriter w;
        w.open(dir_, small_segment_size, {0, 0});
        ASSERT_TRUE(w.wait_until_prepared(std::chrono::seconds{5}));
    }
    EXPECT_LE(open_descriptors(), before) << "the prepared segment's descriptor or its eventfd was not closed";
}

} // namespaces
