// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file WalClassTest.cpp
 * @brief Unit tests for pubsub_itc_fw::Wal.
 *
 * Wal wraps WalWriter/WalReader with the application-level payload format
 * (wall_time_ns | pdu_id | PDU bytes) and snapshot management. These tests
 * verify that framing, round-trip fidelity, snapshot creation, UseSnapshot
 * vs IgnoreSnapshot replay semantics, and CRC-protected snapshot integrity
 * all work correctly.
 */

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

namespace pubsub_itc_fw::tests {

namespace {

constexpr size_t segment_size = 4096;

struct CapturedRecord {
    int64_t seq_no{};
    int16_t pdu_id{};
    std::vector<uint8_t> payload;
    int64_t wall_time_ns{};
};

Wal::ReplayCallback capture(std::vector<CapturedRecord>& out) {
    return [&out](int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t size, int64_t wall_time_ns) {
        const auto* p = static_cast<const uint8_t*>(payload);
        out.push_back({seq_no, pdu_id, {p, p + size}, wall_time_ns});
    };
}

} // namespaces

class WalClassTest : public ::testing::Test {
  protected:
    void SetUp() override {
        std::string tmpl = "/dev/shm/wal_class_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
        dir_ = tmpl;
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    std::string snapshot_path() const {
        return dir_ + "/snapshot.bin";
    }

    void corrupt_snapshot_crc() {
        // The snapshot CRC sits at byte offset 40. Flip the first byte to break it.
        const std::string path = snapshot_path();
        const int fd = ::open(path.c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        uint8_t byte = 0;
        ASSERT_EQ(::pread(fd, &byte, 1, 40), 1);
        byte ^= 0xFF;
        ASSERT_EQ(::pwrite(fd, &byte, 1, 40), 1);
        ::close(fd);
    }

    std::string dir_;
};

// ---------------------------------------------------------------------------
// Construction and initial state
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, NotOpenByDefault) {
    Wal wal;
    EXPECT_FALSE(wal.is_open());
}

TEST_F(WalClassTest, IsOpenAfterOpen) {
    Wal wal;
    wal.open(dir_, segment_size);
    EXPECT_TRUE(wal.is_open());
}

TEST_F(WalClassTest, FreshOpenReturnsZero) {
    Wal wal;
    const int64_t last = wal.open(dir_, segment_size);
    EXPECT_EQ(last, 0);
}

TEST_F(WalClassTest, FreshOpenHasZeroRecordCount) {
    Wal wal;
    wal.open(dir_, segment_size);
    EXPECT_EQ(wal.record_count(), 0u);
}

TEST_F(WalClassTest, FreshOpenHasZeroLastSeqNo) {
    Wal wal;
    wal.open(dir_, segment_size);
    EXPECT_EQ(wal.last_seq_no(), 0);
}

// ---------------------------------------------------------------------------
// Single record round-trip
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, SingleRecordReplaySeqNo) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x01, 0x02, 0x03};
        wal.append(42, 1000, payload, static_cast<int>(sizeof(payload)), 999);
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records));
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].seq_no, 42);
}

TEST_F(WalClassTest, SingleRecordReplayPduId) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0xAA};
        wal.append(1, 1001, payload, 1, 0);
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records));
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].pdu_id, 1001);
}

TEST_F(WalClassTest, SingleRecordReplayPayload) {
    const std::vector<uint8_t> expected = {0x10, 0x20, 0x30, 0x40};
    {
        Wal wal;
        wal.open(dir_, segment_size);
        wal.append(1, 1000, expected.data(), static_cast<int>(expected.size()), 0);
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records));
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].payload, expected);
}

TEST_F(WalClassTest, SingleRecordReplayWallTimeNs) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        wal.append(1, 1000, payload, 1, 1234567890LL);
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records));
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].wall_time_ns, 1234567890LL);
}

// ---------------------------------------------------------------------------
// Counters and last_seq_no
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, OpenReturnsLastSeqNo) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        wal.append(7, 1000, payload, 1, 0);
        wal.append(8, 1000, payload, 1, 0);
        wal.append(9, 1000, payload, 1, 0);
    }

    Wal wal;
    const int64_t last = wal.open(dir_, segment_size);
    EXPECT_EQ(last, 9);
}

TEST_F(WalClassTest, RecordCountAfterReplay) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 5; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
    }

    Wal wal;
    wal.open(dir_, segment_size, nullptr);
    EXPECT_EQ(wal.record_count(), 5u);
    EXPECT_EQ(wal.last_seq_no(), 5);
}

// ---------------------------------------------------------------------------
// Multiple records: ordering
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, MultipleRecordsReplayedInOrder) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        for (int i = 1; i <= 4; ++i) {
            const uint8_t payload = static_cast<uint8_t>(i * 10);
            wal.append(static_cast<int64_t>(i), static_cast<int16_t>(1000 + i), &payload, 1,
                       static_cast<int64_t>(i) * 100);
        }
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records));
    ASSERT_EQ(records.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(records[static_cast<size_t>(i)].seq_no, i + 1) << "index " << i;
        EXPECT_EQ(records[static_cast<size_t>(i)].pdu_id, 1000 + i + 1) << "index " << i;
        EXPECT_EQ(records[static_cast<size_t>(i)].wall_time_ns, (i + 1) * 100) << "index " << i;
    }
}

// ---------------------------------------------------------------------------
// Null callback
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, NullCallbackDoesNotCrash) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0xBB};
        wal.append(1, 1000, payload, 1, 0);
    }

    Wal wal;
    EXPECT_NO_THROW(wal.open(dir_, segment_size, nullptr));
    EXPECT_EQ(wal.record_count(), 1u);
}

// ---------------------------------------------------------------------------
// Snapshot: creation
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, TakeSnapshotCreatesFile) {
    Wal wal;
    wal.open(dir_, segment_size);
    const uint8_t payload[] = {0x01};
    wal.append(1, 1000, payload, 1, 0);
    wal.take_snapshot();
    EXPECT_TRUE(std::filesystem::exists(snapshot_path()));
}

// ---------------------------------------------------------------------------
// Snapshot: UseSnapshot replay semantics
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, UseSnapshotSkipsPreSnapshotRecords) {
    // Append records 1-3, take snapshot, then append 4-5.
    // Reopening with UseSnapshot should replay only records 4-5.
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 3; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
        wal.take_snapshot();
        for (int i = 4; i <= 5; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records), WalOpenMode{WalOpenMode::UseSnapshot});
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].seq_no, 4);
    EXPECT_EQ(records[1].seq_no, 5);
}

TEST_F(WalClassTest, UseSnapshotRestoresCountersFromSnapshot) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 3; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
        wal.take_snapshot();
    }

    Wal wal;
    wal.open(dir_, segment_size, nullptr, WalOpenMode{WalOpenMode::UseSnapshot});
    EXPECT_EQ(wal.record_count(), 3u);
    EXPECT_EQ(wal.last_seq_no(), 3);
}

TEST_F(WalClassTest, UseSnapshotWithNoSnapshotFileReplaysAll) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 3; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
        // No take_snapshot() call -- snapshot.bin does not exist.
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records), WalOpenMode{WalOpenMode::UseSnapshot});
    EXPECT_EQ(records.size(), 3u);
}

// ---------------------------------------------------------------------------
// Snapshot: IgnoreSnapshot replay semantics
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, IgnoreSnapshotReplaysAllRecords) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 3; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
        wal.take_snapshot();
        for (int i = 4; i <= 5; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records), WalOpenMode{WalOpenMode::IgnoreSnapshot});
    ASSERT_EQ(records.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(records[static_cast<size_t>(i)].seq_no, i + 1) << "index " << i;
    }
}

// ---------------------------------------------------------------------------
// Snapshot: CRC integrity
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, CorruptedSnapshotFallsBackToFullReplay) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 3; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
        wal.take_snapshot();
        for (int i = 4; i <= 5; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
    }

    corrupt_snapshot_crc();

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records), WalOpenMode{WalOpenMode::UseSnapshot});
    EXPECT_EQ(records.size(), 5u);
}

// ---------------------------------------------------------------------------
// Resume writing after reopen
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, ResumeWritingAfterReopenNoGap) {
    // Write 1-3, close, reopen, write 4-5, close, verify full replay gives 1-5.
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 3; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
    }
    {
        Wal wal;
        wal.open(dir_, segment_size, nullptr);
        const uint8_t payload[] = {0x00};
        for (int i = 4; i <= 5; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records));
    ASSERT_EQ(records.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(records[static_cast<size_t>(i)].seq_no, i + 1) << "index " << i;
    }
}

// ---------------------------------------------------------------------------
// Oversized payload
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, OversizedPayloadThrows) {
    Wal wal;
    wal.open(dir_, segment_size);
    std::vector<uint8_t> big(segment_size + 1, 0xCC);
    EXPECT_THROW(wal.append(1, 1000, big.data(), static_cast<int>(big.size()), 0),
                 pubsub_itc_fw::PreconditionAssertion);
}

// ---------------------------------------------------------------------------
// Segment deletion after snapshot
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, SnapshotDeletesOldSegments) {
    // Each Wal entry with a 1-byte PDU payload occupies:
    //   24 (WalEntryHeader) + 8 (wall_time_ns) + 2 (pdu_id) + 1 (PDU) + 4 (CRC) = 39 bytes.
    // With segment_size=128: 3 entries fit (3*39=117), the 4th triggers rollover to segment 1.
    // take_snapshot() then calls delete_segments_before(1), removing segment 0.
    constexpr size_t small_segment = 128;
    {
        Wal wal;
        wal.open(dir_, small_segment);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 4; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
        wal.take_snapshot();
    }

    EXPECT_FALSE(std::filesystem::exists(dir_ + "/wal_000000.log"))
        << "Segment 0 should have been deleted by take_snapshot()";
    EXPECT_TRUE(std::filesystem::exists(dir_ + "/wal_000001.log"))
        << "Segment 1 (current write segment) must still exist";
}

TEST_F(WalClassTest, SnapshotDeletesOldSegmentsAndReopenWorks) {
    constexpr size_t small_segment = 128;
    {
        Wal wal;
        wal.open(dir_, small_segment);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 4; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
        wal.take_snapshot();
        const uint8_t p[] = {0x00};
        wal.append(5, 1000, p, 1, 0);
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    const int64_t last = wal.open(dir_, small_segment, capture(records), WalOpenMode{WalOpenMode::UseSnapshot});
    EXPECT_EQ(last, 5);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].seq_no, 5);
}

// ---------------------------------------------------------------------------
// Snapshot format errors: truncated file and wrong header fields
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, TruncatedSnapshotFallsBackToFullReplay) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 3; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
    }

    {
        const int fd = ::open(snapshot_path().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ASSERT_GE(fd, 0);
        const uint8_t stub[10] = {};
        ASSERT_EQ(::write(fd, stub, sizeof(stub)), static_cast<ssize_t>(sizeof(stub)));
        ::close(fd);
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records), WalOpenMode{WalOpenMode::UseSnapshot});
    EXPECT_EQ(records.size(), 3u) << "Truncated snapshot must be ignored; all records replayed";
}

TEST_F(WalClassTest, WrongMagicSnapshotFallsBackToFullReplay) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 3; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
        wal.take_snapshot();
    }

    {
        const int fd = ::open(snapshot_path().c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        const uint32_t bad_magic = 0xDEADBEEFU;
        ASSERT_EQ(::pwrite(fd, &bad_magic, sizeof(bad_magic), 0),
                  static_cast<ssize_t>(sizeof(bad_magic)));
        ::close(fd);
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records), WalOpenMode{WalOpenMode::UseSnapshot});
    EXPECT_EQ(records.size(), 3u) << "Wrong magic must be ignored; all records replayed";
}

TEST_F(WalClassTest, WrongVersionSnapshotFallsBackToFullReplay) {
    {
        Wal wal;
        wal.open(dir_, segment_size);
        const uint8_t payload[] = {0x00};
        for (int i = 1; i <= 3; ++i) {
            wal.append(i, 1000, payload, 1, 0);
        }
        wal.take_snapshot();
    }

    {
        const int fd = ::open(snapshot_path().c_str(), O_RDWR);
        ASSERT_GE(fd, 0);
        const uint32_t bad_version = 0xFFFFFFFFU;
        ASSERT_EQ(::pwrite(fd, &bad_version, sizeof(bad_version), 4),
                  static_cast<ssize_t>(sizeof(bad_version)));
        ::close(fd);
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records), WalOpenMode{WalOpenMode::UseSnapshot});
    EXPECT_EQ(records.size(), 3u) << "Wrong version must be ignored; all records replayed";
}

// ---------------------------------------------------------------------------
// Malformed WAL entry: payload too small to contain the Wal header prefix
// ---------------------------------------------------------------------------

TEST_F(WalClassTest, MalformedWalEntryIsSkipped) {
    // Write a raw WalWriter entry whose payload is smaller than the Wal
    // header prefix (wall_time_ns(8) + pdu_id(2) = 10 bytes). Wal::open()
    // must skip it silently rather than misinterpreting the bytes.
    {
        WalWriter writer;
        writer.open(dir_, segment_size, {0, 0});
        const uint8_t tiny[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        writer.append(1, tiny, sizeof(tiny));
    }

    std::vector<CapturedRecord> records;
    Wal wal;
    wal.open(dir_, segment_size, capture(records));
    EXPECT_EQ(records.size(), 0u) << "Malformed entry must be silently skipped";
    EXPECT_EQ(wal.record_count(), 0u);
}

} // namespaces
