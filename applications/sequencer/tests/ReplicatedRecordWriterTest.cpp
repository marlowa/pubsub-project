// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ReplicatedRecordWriter.hpp"

#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalReader.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

using sequencer::ReplicatedRecordWriter;

namespace {

// Small segments, so that the writers also meet at the moment a segment is full and the next begins.
constexpr size_t segment_size = 16 * 1024;
constexpr int16_t pdu_id = 7;

class ReplicatedRecordWriterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("replicated_record_writer_test");
        wal_.emplace();
        wal_->open(dir_, segment_size);
    }

    void TearDown() override {
        // The log's helper thread creates the next segment ahead of the writer, so the log is shut
        // down, and the thread with it, before its directory is removed.
        wal_.reset();
        std::filesystem::remove_all(dir_);
    }

    ReplicatedRecordWriter::Outcome write(ReplicatedRecordWriter& writer, int64_t seq_no) {
        return writer.write_if_next(
            seq_no, [this] { return wal_->last_seq_no(); },
            [this, seq_no] {
                const std::vector<uint8_t> payload(150, static_cast<uint8_t>(seq_no));
                wal_->append(seq_no, pdu_id, payload.data(), static_cast<int>(payload.size()), seq_no);
                highest_written_.store(seq_no);
            });
    }

    // The last record written, for the threads of a test to watch without touching the log.
    std::atomic<int64_t> highest_written_{0};

    // Every record in the log, in order. Reading throws if the log is damaged.
    std::vector<int64_t> records_in_log() {
        std::vector<int64_t> records;
        static_cast<void>(pubsub_itc_fw::WalReader::replay(dir_, pubsub_itc_fw::WalPosition{0, 0},
                                                           [&records](int64_t record_id, const void*, size_t) { records.push_back(record_id); }));
        return records;
    }

    std::string dir_;
    std::optional<pubsub_itc_fw::Wal> wal_;
};

} // namespaces

TEST_F(ReplicatedRecordWriterTest, TheNextRecordIsWrittenAndOthersAreNot) {
    ReplicatedRecordWriter writer;
    EXPECT_EQ(write(writer, 1), ReplicatedRecordWriter::Outcome::written);
    EXPECT_EQ(write(writer, 1), ReplicatedRecordWriter::Outcome::already_held);
    EXPECT_EQ(write(writer, 3), ReplicatedRecordWriter::Outcome::gap);
    EXPECT_EQ(write(writer, 2), ReplicatedRecordWriter::Outcome::written);
    EXPECT_EQ(records_in_log(), (std::vector<int64_t>{1, 2}));
}

// The condition of BUG-0123: two threads delivering the same records and both writing them. Each
// thread offers every record in turn, as fast as it can, so that they meet on nearly every record. The
// log must hold each record once, in order, and read back without damage.
TEST_F(ReplicatedRecordWriterTest, TwoThreadsWritingTheSameRecordsAtOnceLeaveEachRecordOnceAndInOrder) {
    constexpr int64_t count = 20000;
    ReplicatedRecordWriter writer;
    std::atomic<int64_t> written{0};
    std::atomic<bool> go{false};
    auto offer_every_record = [&] {
        while (!go.load()) {}
        for (int64_t seq_no = 1; seq_no <= count; ++seq_no) {
            // Offered until it is in the log, by this thread or the other.
            while (highest_written_.load() < seq_no) {
                if (write(writer, seq_no) == ReplicatedRecordWriter::Outcome::written) {
                    written.fetch_add(1);
                }
            }
        }
    };
    std::thread first(offer_every_record);
    std::thread second(offer_every_record);
    go.store(true);
    first.join();
    second.join();

    EXPECT_EQ(written.load(), count) << "a record was written more than once";
    std::vector<int64_t> records;
    ASSERT_NO_THROW(records = records_in_log()) << "the log was damaged";
    ASSERT_EQ(records.size(), static_cast<size_t>(count));
    for (int64_t index = 0; index < count; ++index) {
        ASSERT_EQ(records[static_cast<size_t>(index)], index + 1) << "record " << index + 1 << " is out of place";
    }
}

TEST_F(ReplicatedRecordWriterTest, NoRecordIsWrittenWhileTheLogIsBeingChanged) {
    ReplicatedRecordWriter writer;
    std::atomic<bool> inside{false};
    std::atomic<bool> written_while_inside{false};
    std::thread changer([&] {
        writer.change_log([&] {
            inside.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            inside.store(false);
        });
    });
    while (!inside.load()) {}
    static_cast<void>(writer.write_if_next(1, [] { return int64_t{0}; }, [&] { written_while_inside.store(inside.load()); }));
    changer.join();
    EXPECT_FALSE(written_while_inside.load()) << "a record was written while the log was being changed";
}
