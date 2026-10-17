// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalCursor.hpp>
#include <pubsub_itc_fw/WalHandedOverFollowers.hpp>

#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace pubsub_itc_fw::tests {

namespace {

// Small, so that a few records fill a segment and the tests cross from one segment to the next.
constexpr size_t small_segment_size = 4096;
constexpr int16_t record_type = 7;
constexpr int16_t sent_type = 9;

class RecordingLinks : public WalHandedOverFollowersLinksInterface {
  public:
    void send_record(const ConnectionID& follower, int16_t pdu_id, int64_t seq_no, const uint8_t* /*bytes*/, size_t /*size*/) override {
        sent.emplace_back(follower.get_value(), seq_no);
        last_pdu_id = pdu_id;
    }
    std::vector<std::pair<int, int64_t>> sent;
    int16_t last_pdu_id{0};

    std::vector<int64_t> sent_to(int connection) const {
        std::vector<int64_t> numbers;
        for (const auto& [to, seq_no] : sent) {
            if (to == connection) {
                numbers.push_back(seq_no);
            }
        }
        return numbers;
    }
};

class WalHandedOverFollowersTest : public ::testing::Test {
  protected:
    void SetUp() override {
        directory_ = tests_common::make_scratch_directory("wal_handed_over_followers");
        wal_.emplace();
        wal_->open(directory_, small_segment_size);
    }

    void TearDown() override {
        // The WAL's helper thread creates the next segment ahead of the writer, so the WAL is shut down, and
        // the thread with it, before its directory is removed.
        wal_.reset();
        tests_common::remove_scratch_directory(directory_);
    }

    void append_through(int64_t last) {
        const std::vector<uint8_t> payload(200, 0x5a);
        while (last_seq_no_ < last) {
            ++last_seq_no_;
            wal_->append(last_seq_no_, record_type, payload.data(), static_cast<int>(payload.size()), last_seq_no_, 1);
        }
    }

    // The position just after record @p seq_no, found by reading the WAL from its start.
    WalPosition position_after(int64_t seq_no) const {
        WalCursor cursor;
        cursor.open(directory_, WalPosition{});
        int64_t record_id = 0;
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        while (cursor.read_next(record_id, bytes, size)) {
            if (record_id == seq_no) {
                return cursor.position();
            }
        }
        ADD_FAILURE() << "record " << seq_no << " not found";
        return WalPosition{};
    }

    // Sends every record as one message of sent_type, except those listed as damaged or passed over. Each record
    // converted is added to converted_, so that a test can see how many times each record was read.
    WalRecordConverter converter(std::vector<int64_t> damaged = {}, std::vector<int64_t> passed_over = {}) {
        return [this, damaged, passed_over](int64_t seq_no, const WalEntryContents& entry, std::vector<uint8_t>& out) -> ConvertedRecord {
            converted_.push_back(seq_no);
            for (const int64_t number : damaged) {
                if (number == seq_no) {
                    return ConvertedRecord::damaged();
                }
            }
            for (const int64_t number : passed_over) {
                if (number == seq_no) {
                    return ConvertedRecord::pass_over();
                }
            }
            out.assign(entry.bytes, entry.bytes + entry.size);
            return ConvertedRecord::send(sent_type);
        };
    }

    std::string directory_;
    std::optional<Wal> wal_;
    int64_t last_seq_no_{0};
    RecordingLinks links_;
    std::vector<int64_t> converted_;
    const ConnectionID first_{1};
    const ConnectionID second_{2};
    const ConnectionID third_{3};
};

} // un-named namespace

TEST_F(WalHandedOverFollowersTest, SendsEachRecordAboveTheLastHeldUpToTheLimitAndNoFurther) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    followers.add(first_, 4, wal_->scan_start_for(5));

    EXPECT_EQ(followers.send_to(first_, 7).records_sent, 3);
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{5, 6, 7}));
    EXPECT_EQ(links_.last_pdu_id, sent_type) << "each record is sent as the message the converter made";

    EXPECT_EQ(followers.send_to(first_, 10).records_sent, 3);
    EXPECT_EQ(followers.send_to(first_, 10).records_sent, 0) << "nothing is sent twice";
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{5, 6, 7, 8, 9, 10}));
    EXPECT_EQ(followers.last_sent(first_), 10);
}

TEST_F(WalHandedOverFollowersTest, ALimitBeyondWhatIsWrittenSendsWhatIsWrittenAndTheRestLater) {
    append_through(6);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    followers.add(first_, 3, wal_->scan_start_for(4));

    EXPECT_EQ(followers.send_to(first_, 9).records_sent, 3);
    append_through(9);
    EXPECT_EQ(followers.send_to(first_, 9).records_sent, 3);
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{4, 5, 6, 7, 8, 9}));
}

TEST_F(WalHandedOverFollowersTest, ARecordAboveTheLimitIsHeldWhenTheNumbersJumpAndSentOnceTheLimitPassesIt) {
    append_through(5);
    last_seq_no_ = 7; // the next record written is 8: numbers 6 and 7 are never written
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    followers.add(first_, 4, wal_->scan_start_for(5));

    static_cast<void>(followers.send_to(first_, 6));
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{5})) << "record 8 is above the limit of 6, and must not be sent";
    static_cast<void>(followers.send_to(first_, 9));
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{5, 8, 9}));
}

TEST_F(WalHandedOverFollowersTest, ReadingStartsAtTheResumePositionSoRecordsBeforeItAreNotSent) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    // The follower was handed over at record 4 but the reading thread had already read it records 5 and 6,
    // which it holds: reading starts after record 6.
    followers.add(first_, 4, position_after(6));

    static_cast<void>(followers.send_to(first_, 10));
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{7, 8, 9, 10}));
}

TEST_F(WalHandedOverFollowersTest, RecordsWrittenOneAtATimeAcrossSegmentsAreEachSentOnceInOrder) {
    // Each record is sent as soon as it is written, so the cursor reaches the end of what is written again
    // and again, including the unwritten end of a segment and the first record of a segment created after it
    // was opened. Neither may be skipped.
    append_through(1);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    followers.add(first_, 0, wal_->scan_start_for(1));
    std::vector<int64_t> expected;
    for (int64_t seq_no = 1; seq_no <= 100; ++seq_no) {
        append_through(seq_no);
        static_cast<void>(followers.send_to(first_, seq_no));
        expected.push_back(seq_no);
    }
    ASSERT_GT(wal_->segments_on_disk().size(), 3U) << "the records must cross several segments for this test to mean anything";
    EXPECT_EQ(links_.sent_to(1), expected);
}

TEST_F(WalHandedOverFollowersTest, ARecordPassedOverIsNotSentButTheFollowerMovesPastIt) {
    append_through(6);
    WalHandedOverFollowers followers(*wal_, directory_, converter({}, {4}), links_);
    followers.add(first_, 2, wal_->scan_start_for(3));

    const WalHandedOverFollowers::Outcome outcome = followers.send_to(first_, 6);
    EXPECT_EQ(outcome.records_sent, 3);
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{3, 5, 6}));
    EXPECT_EQ(followers.last_sent(first_), 6);
}

TEST_F(WalHandedOverFollowersTest, ADamagedRecordStopsTheFollowerAndIsReported) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter({6}), links_);
    followers.add(first_, 4, wal_->scan_start_for(5));

    const WalHandedOverFollowers::Outcome outcome = followers.send_to(first_, 10);
    EXPECT_EQ(outcome.damaged_seq_no, 6);
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{5})) << "nothing after a damaged record is sent";
    EXPECT_FALSE(followers.contains(first_));
    EXPECT_EQ(followers.send_to(first_, 10).records_sent, 0);
}

TEST_F(WalHandedOverFollowersTest, EachFollowerIsSentFromItsOwnPositionAndADamagedOneDoesNotStopTheOther) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter({3}), links_);
    followers.add(first_, 2, wal_->scan_start_for(3));
    followers.add(second_, 5, wal_->scan_start_for(6));

    const std::vector<std::pair<ConnectionID, int64_t>> damaged = followers.send_to_all(8);
    ASSERT_EQ(damaged.size(), 1U);
    EXPECT_EQ(damaged[0].first, first_);
    EXPECT_EQ(damaged[0].second, 3);
    EXPECT_TRUE(links_.sent_to(1).empty());
    EXPECT_EQ(links_.sent_to(2), (std::vector<int64_t>{6, 7, 8}));
}

TEST_F(WalHandedOverFollowersTest, ARemovedFollowerIsSentNothingAndOneAddedAgainStartsAfresh) {
    append_through(8);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    followers.add(first_, 2, wal_->scan_start_for(3));
    static_cast<void>(followers.send_to(first_, 4));
    followers.remove(first_);
    EXPECT_FALSE(followers.contains(first_));
    EXPECT_EQ(followers.send_to(first_, 8).records_sent, 0);

    followers.add(first_, 6, wal_->scan_start_for(7));
    static_cast<void>(followers.send_to(first_, 8));
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{3, 4, 7, 8}));
    followers.remove_all();
    EXPECT_TRUE(followers.empty());
}

TEST_F(WalHandedOverFollowersTest, FollowersInStepHaveEachRecordReadAndConvertedOnce) {
    append_through(4);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    followers.add(first_, 4, wal_->scan_start_for(5));
    followers.add(second_, 4, wal_->scan_start_for(5));
    static_cast<void>(followers.send_to_all(4)); // both join the shared reading
    converted_.clear();

    for (int64_t seq_no = 5; seq_no <= 60; ++seq_no) {
        append_through(seq_no);
        EXPECT_TRUE(followers.send_to_all(seq_no).empty());
    }
    std::vector<int64_t> expected;
    for (int64_t seq_no = 5; seq_no <= 60; ++seq_no) {
        expected.push_back(seq_no);
    }
    ASSERT_GT(wal_->segments_on_disk().size(), 3U) << "the records must cross several segments for this test to mean anything";
    EXPECT_EQ(converted_, expected) << "each record is read and converted once, not once for each follower";
    EXPECT_EQ(links_.sent_to(1), expected);
    EXPECT_EQ(links_.sent_to(2), expected);
}

TEST_F(WalHandedOverFollowersTest, AFollowerAddedBehindCatchesUpOnItsOwnCursorAndThenJoinsTheSharedReading) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    followers.add(first_, 10, wal_->scan_start_for(11));
    static_cast<void>(followers.send_to_all(10));
    append_through(12);
    static_cast<void>(followers.send_to_all(12));

    followers.add(second_, 3, wal_->scan_start_for(4));
    append_through(14);
    static_cast<void>(followers.send_to_all(14));
    converted_.clear();
    append_through(20);
    static_cast<void>(followers.send_to_all(20));

    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{11, 12, 13, 14, 15, 16, 17, 18, 19, 20}));
    EXPECT_EQ(links_.sent_to(2), (std::vector<int64_t>{4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20}));
    EXPECT_EQ(converted_, (std::vector<int64_t>{15, 16, 17, 18, 19, 20})) << "once caught up, the second follower is served by the shared reading";
}

TEST_F(WalHandedOverFollowersTest, ADamagedRecordOnTheSharedReadingStopsOnlyTheFollowersThatLackIt) {
    append_through(9);
    WalHandedOverFollowers followers(*wal_, directory_, converter({6}), links_);
    followers.add(first_, 2, wal_->scan_start_for(3));
    followers.add(second_, 2, wal_->scan_start_for(3));
    static_cast<void>(followers.send_to_all(4)); // both in step at 4
    // The third holds records through 7, past the damaged record, and joins ahead of the shared reading.
    followers.add(third_, 7, wal_->scan_start_for(8));

    std::vector<std::pair<ConnectionID, int64_t>> damaged = followers.send_to_all(9);
    ASSERT_EQ(damaged.size(), 2U);
    std::sort(damaged.begin(), damaged.end(), [](const auto& a, const auto& b) { return a.first.get_value() < b.first.get_value(); });
    EXPECT_EQ(damaged[0], std::make_pair(first_, int64_t{6}));
    EXPECT_EQ(damaged[1], std::make_pair(second_, int64_t{6}));
    EXPECT_FALSE(followers.contains(first_));
    EXPECT_FALSE(followers.contains(second_));
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{3, 4, 5}));
    EXPECT_EQ(links_.sent_to(3), (std::vector<int64_t>{8, 9}));

    append_through(11);
    EXPECT_TRUE(followers.send_to_all(11).empty());
    EXPECT_EQ(links_.sent_to(3), (std::vector<int64_t>{8, 9, 10, 11})) << "the follower past the damaged record goes on being sent records";
}

TEST_F(WalHandedOverFollowersTest, WhenNoFollowerIsLeftInStepTheNextOneToCatchUpBecomesTheSharedReading) {
    append_through(6);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    followers.add(first_, 2, wal_->scan_start_for(3));
    static_cast<void>(followers.send_to_all(6));
    followers.remove(first_);

    followers.add(second_, 1, wal_->scan_start_for(2));
    followers.add(third_, 4, wal_->scan_start_for(5));
    append_through(9);
    static_cast<void>(followers.send_to_all(9));
    converted_.clear();
    append_through(12);
    static_cast<void>(followers.send_to_all(12));

    EXPECT_EQ(links_.sent_to(2), (std::vector<int64_t>{2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
    EXPECT_EQ(links_.sent_to(3), (std::vector<int64_t>{5, 6, 7, 8, 9, 10, 11, 12}));
    EXPECT_EQ(converted_, (std::vector<int64_t>{10, 11, 12}));
}

TEST_F(WalHandedOverFollowersTest, SendToSendsNothingToAFollowerInStepWithTheSharedReading) {
    append_through(6);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    followers.add(first_, 2, wal_->scan_start_for(3));
    static_cast<void>(followers.send_to_all(4));
    append_through(8);
    EXPECT_EQ(followers.send_to(first_, 8).records_sent, 0) << "send_to_all serves a follower in step";
    static_cast<void>(followers.send_to_all(8));
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{3, 4, 5, 6, 7, 8}));
}

} // namespaces
