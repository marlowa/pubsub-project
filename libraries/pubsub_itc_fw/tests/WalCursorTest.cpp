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

#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/WalCursor.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>
namespace pubsub_itc_fw::tests {

namespace {

constexpr size_t segment_size = 4096; // large: no rollover

// Each entry with a 4-byte payload is 24 (header) + 4 (payload) + 4 (CRC) = 32 bytes;
// 128 / 32 = 4 entries per segment before a roll-over.
constexpr size_t small_segment_size = 128;

struct Record {
    int64_t id;
    uint32_t value;
};

} // namespaces

class WalCursorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("wal_cursor_test");
    }

    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    // Append records 1..count, payload = id * 100.
    void write_records(size_t seg_size, int count) {
        WalWriter writer;
        writer.open(dir_, seg_size, {0, 0});
        for (int i = 1; i <= count; ++i) {
            const uint32_t payload = static_cast<uint32_t>(i * 100);
            writer.append(static_cast<int64_t>(i), &payload, sizeof(payload));
        }
    }

    // Read every remaining record from the cursor.
    static std::vector<Record> drain(WalCursor& cursor) {
        std::vector<Record> out;
        int64_t id = 0;
        const uint8_t* payload = nullptr;
        size_t size = 0;
        while (cursor.read_next(id, payload, size)) {
            EXPECT_EQ(size, sizeof(uint32_t));
            uint32_t value = 0;
            std::memcpy(&value, payload, sizeof(value));
            out.push_back({id, value});
        }
        return out;
    }

    std::string dir_;
};

TEST_F(WalCursorTest, ReadsAllRecordsInOrder) {
    write_records(segment_size, 5);

    WalCursor cursor;
    cursor.open(dir_, {0, 0});
    const std::vector<Record> records = drain(cursor);

    ASSERT_EQ(records.size(), 5U);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(records[static_cast<size_t>(i)].id, i + 1);
        EXPECT_EQ(records[static_cast<size_t>(i)].value, static_cast<uint32_t>((i + 1) * 100));
    }
}

TEST_F(WalCursorTest, ReadsAcrossSegments) {
    write_records(small_segment_size, 10); // spans ~3 segments

    WalCursor cursor;
    cursor.open(dir_, {0, 0});
    const std::vector<Record> records = drain(cursor);

    ASSERT_EQ(records.size(), 10U);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(records[static_cast<size_t>(i)].id, i + 1) << "wrong id at index " << i;
        EXPECT_EQ(records[static_cast<size_t>(i)].value, static_cast<uint32_t>((i + 1) * 100));
    }
}

TEST_F(WalCursorTest, EmptyWalReturnsFalse) {
    // Open a valid but empty WAL (writer opened, nothing appended).
    WalWriter writer;
    writer.open(dir_, segment_size, {0, 0});

    WalCursor cursor;
    cursor.open(dir_, {0, 0});
    int64_t id = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;
    EXPECT_FALSE(cursor.read_next(id, payload, size));
}

TEST_F(WalCursorTest, NonexistentDirectoryReturnsFalse) {
    WalCursor cursor;
    cursor.open(dir_ + "/does_not_exist", {0, 0});
    int64_t id = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;
    EXPECT_FALSE(cursor.read_next(id, payload, size));
}

TEST_F(WalCursorTest, ResumeFromPositionContinuesWhereItStopped) {
    write_records(segment_size, 5);

    // Read the first two records, then capture the position.
    WalCursor first;
    first.open(dir_, {0, 0});
    int64_t id = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;
    ASSERT_TRUE(first.read_next(id, payload, size));
    EXPECT_EQ(id, 1);
    ASSERT_TRUE(first.read_next(id, payload, size));
    EXPECT_EQ(id, 2);
    const WalPosition resume = first.position();

    // A fresh cursor opened at that position yields records 3, 4, 5.
    WalCursor second;
    second.open(dir_, resume);
    const std::vector<Record> rest = drain(second);

    ASSERT_EQ(rest.size(), 3U);
    EXPECT_EQ(rest[0].id, 3);
    EXPECT_EQ(rest[1].id, 4);
    EXPECT_EQ(rest[2].id, 5);
}

// Resuming across a segment boundary: read past the first segment, capture the
// position, and confirm a fresh cursor picks up the exact next record.
TEST_F(WalCursorTest, ResumeAcrossSegmentBoundary) {
    write_records(small_segment_size, 10); // 4 records per segment

    WalCursor first;
    first.open(dir_, {0, 0});
    int64_t id = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;
    for (int i = 0; i < 6; ++i) { // read past the first segment (4) into the second
        ASSERT_TRUE(first.read_next(id, payload, size));
        EXPECT_EQ(id, i + 1);
    }
    const WalPosition resume = first.position();

    WalCursor second;
    second.open(dir_, resume);
    const std::vector<Record> rest = drain(second);

    ASSERT_EQ(rest.size(), 4U); // records 7..10
    EXPECT_EQ(rest[0].id, 7);
    EXPECT_EQ(rest[3].id, 10);
}

// Damage in the middle of the log stops the cursor rather than being skipped (BUG-0106).

namespace {

// Changes one byte of a segment file, as damage to the disk would; changing it again undoes it.
void flip_byte(const std::string& path, off_t offset) {
    const int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    uint8_t byte = 0;
    ASSERT_EQ(::pread(fd, &byte, 1, offset), 1);
    byte ^= 0xFF;
    ASSERT_EQ(::pwrite(fd, &byte, 1, offset), 1);
    ::close(fd);
}

// Each entry with a four-byte payload is 32 bytes: a 24-byte header, the payload, a 4-byte checksum.
constexpr off_t entry_bytes = 32;
constexpr off_t payload_offset_in_entry = 24;

} // namespaces

TEST_F(WalCursorTest, ADamagedEntryFollowedByValidOnesIsRefused) {
    write_records(segment_size, 10);
    flip_byte(dir_ + "/wal_000000.log", 2 * entry_bytes + payload_offset_in_entry);
    WalCursor cursor;
    cursor.open(dir_, {0, 0});
    EXPECT_THROW(static_cast<void>(drain(cursor)), PubSubItcException);
}

TEST_F(WalCursorTest, ADamagedEntryAtTheEndOfASegmentFollowedByALaterSegmentIsRefused) {
    write_records(small_segment_size, 6);
    flip_byte(dir_ + "/wal_000000.log", 3 * entry_bytes + payload_offset_in_entry);
    WalCursor cursor;
    cursor.open(dir_, {0, 0});
    EXPECT_THROW(static_cast<void>(drain(cursor)), PubSubItcException);
}

// An invalid entry with nothing valid after it may be one still being written. The cursor stops there
// and reads it once it is complete, rather than moving past it.
TEST_F(WalCursorTest, AnIncompleteLastEntryIsReadOnceItIsComplete) {
    write_records(segment_size, 5);
    const std::string path = dir_ + "/wal_000000.log";
    flip_byte(path, 4 * entry_bytes + payload_offset_in_entry);
    WalCursor cursor;
    cursor.open(dir_, {0, 0});
    EXPECT_EQ(drain(cursor).size(), 4U);
    flip_byte(path, 4 * entry_bytes + payload_offset_in_entry);
    const std::vector<Record> rest = drain(cursor);
    ASSERT_EQ(rest.size(), 1U);
    EXPECT_EQ(rest[0].id, 5);
}

} // namespaces
