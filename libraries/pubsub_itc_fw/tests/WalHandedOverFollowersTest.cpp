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
// The type a test gives a record it passes to written(), unlike sent_type, so that a record sent from memory can be
// told apart from one read from the WAL and converted.
constexpr int16_t kept_type = 11;
// The byte every record appended by append_through is filled with.
constexpr uint8_t wal_fill = 0x5a;

class RecordingLinks : public WalHandedOverFollowersLinksInterface {
  public:
    void send_record(const ConnectionID& follower, int16_t pdu_id, int64_t seq_no, const uint8_t* bytes, size_t size) override {
        sent.emplace_back(follower.get_value(), seq_no);
        last_pdu_id = pdu_id;
        pdu_ids.push_back(pdu_id);
        contents.emplace_back(bytes, bytes + size);
    }
    std::vector<std::pair<int, int64_t>> sent;
    int16_t last_pdu_id{0};
    /// For each send, in the same order as sent: the type of message, and its bytes.
    std::vector<int16_t> pdu_ids;
    std::vector<std::vector<uint8_t>> contents;

    /// The bytes and type of the message sent to @p connection as record @p seq_no; fails the test if there was none.
    std::pair<int16_t, std::vector<uint8_t>> message_sent(int connection, int64_t seq_no) const {
        for (size_t i = 0; i < sent.size(); ++i) {
            if (sent[i].first == connection && sent[i].second == seq_no) {
                return {pdu_ids[i], contents[i]};
            }
        }
        ADD_FAILURE() << "record " << seq_no << " was not sent to connection " << connection;
        return {};
    }

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
        const std::vector<uint8_t> payload(200, wal_fill);
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

    // Appends record @p seq_no to the WAL, as append_through does, and then tells @p followers it was written, giving
    // it bytes unlike the WAL's: @p size bytes, each equal to the record's number. The record's message can then be
    // checked to have come from memory and not from the WAL, since the WAL holds other bytes for it.
    void write_and_keep(WalHandedOverFollowers& followers, int64_t seq_no, size_t size = 200) {
        append_through(seq_no);
        const std::vector<uint8_t> bytes(size, static_cast<uint8_t>(seq_no));
        followers.written(seq_no, kept_type, bytes.data(), bytes.size());
    }

    // Whether the message sent to @p connection as record @p seq_no was the copy kept in memory.
    bool sent_from_memory(int connection, int64_t seq_no, size_t size = 200) const {
        const auto [pdu_id, bytes] = links_.message_sent(connection, seq_no);
        return pdu_id == kept_type && bytes == std::vector<uint8_t>(size, static_cast<uint8_t>(seq_no));
    }

    // Whether the message sent to @p connection as record @p seq_no was read from the WAL and converted.
    bool sent_from_wal(int connection, int64_t seq_no) const {
        const auto [pdu_id, bytes] = links_.message_sent(connection, seq_no);
        return pdu_id == sent_type && bytes == std::vector<uint8_t>(200, wal_fill);
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

// ---- Records kept in memory (docs/availability/publishers_sent_from_memory.md) ----------------------------------

TEST_F(WalHandedOverFollowersTest, AKeptRecordIsSentFromMemoryAndTheWalIsNotRead) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_, {16, 64 * 1024});
    followers.add(first_, 10, wal_->scan_start_for(11));
    followers.add(second_, 10, wal_->scan_start_for(11));
    static_cast<void>(followers.send_to_all(10)); // both join the shared reading
    converted_.clear();

    for (int64_t seq_no = 11; seq_no <= 15; ++seq_no) {
        write_and_keep(followers, seq_no);
    }
    EXPECT_EQ(followers.records_kept(), 5U);
    EXPECT_TRUE(followers.send_to_all(15).empty());

    EXPECT_TRUE(converted_.empty()) << "a record sent from memory is not read from the WAL, so the converter is not called";
    for (int64_t seq_no = 11; seq_no <= 15; ++seq_no) {
        EXPECT_TRUE(sent_from_memory(1, seq_no)) << "record " << seq_no;
        EXPECT_TRUE(sent_from_memory(2, seq_no)) << "record " << seq_no;
    }
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{11, 12, 13, 14, 15}));
    EXPECT_EQ(followers.records_kept(), 0U) << "a record sent to every follower in step is no longer kept";
}

TEST_F(WalHandedOverFollowersTest, AKeptRecordIsSentOnlyOnceTheLimitPassesIt) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_, {16, 64 * 1024});
    followers.add(first_, 10, wal_->scan_start_for(11));
    static_cast<void>(followers.send_to_all(10));

    for (int64_t seq_no = 11; seq_no <= 14; ++seq_no) {
        write_and_keep(followers, seq_no);
    }
    static_cast<void>(followers.send_to_all(12));
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{11, 12}));
    EXPECT_EQ(followers.records_kept(), 2U);
    static_cast<void>(followers.send_to_all(14));
    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{11, 12, 13, 14}));
}

TEST_F(WalHandedOverFollowersTest, WhenTheRoomForRecordsIsFullLaterRecordsAreReadFromTheWalInOrderAndNoneTwice) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_, {3, 64 * 1024});
    followers.add(first_, 10, wal_->scan_start_for(11));
    static_cast<void>(followers.send_to_all(10));
    converted_.clear();

    for (int64_t seq_no = 11; seq_no <= 16; ++seq_no) {
        write_and_keep(followers, seq_no);
    }
    EXPECT_EQ(followers.records_kept(), 3U) << "room for three records";
    EXPECT_TRUE(followers.send_to_all(16).empty());

    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{11, 12, 13, 14, 15, 16}));
    EXPECT_TRUE(sent_from_memory(1, 11) && sent_from_memory(1, 12) && sent_from_memory(1, 13));
    EXPECT_TRUE(sent_from_wal(1, 14) && sent_from_wal(1, 15) && sent_from_wal(1, 16));
    EXPECT_EQ(converted_, (std::vector<int64_t>{14, 15, 16})) << "only the records not kept are read from the WAL";

    // With the room empty again, the next records are kept.
    write_and_keep(followers, 17);
    static_cast<void>(followers.send_to_all(17));
    EXPECT_TRUE(sent_from_memory(1, 17));
}

TEST_F(WalHandedOverFollowersTest, ARecordRefusedForLackOfRoomStopsLaterRecordsBeingKeptUntilTheRoomEmpties) {
    append_through(10);
    // Room for two records of 200 bytes, not three.
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_, {16, 450});
    followers.add(first_, 10, wal_->scan_start_for(11));
    static_cast<void>(followers.send_to_all(10));

    write_and_keep(followers, 11);
    write_and_keep(followers, 12);
    write_and_keep(followers, 13);     // no room
    write_and_keep(followers, 14, 10); // would fit, but does not follow the last record kept
    EXPECT_EQ(followers.records_kept(), 2U) << "the records kept must stay consecutive";
    EXPECT_TRUE(followers.send_to_all(14).empty());

    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{11, 12, 13, 14}));
    EXPECT_TRUE(sent_from_memory(1, 11) && sent_from_memory(1, 12));
    EXPECT_TRUE(sent_from_wal(1, 13) && sent_from_wal(1, 14));
}

TEST_F(WalHandedOverFollowersTest, RecordsKeptAfterTheRoomContinuesFromItsStartAreSentIntact) {
    append_through(10);
    // Room for 500 bytes: two records of 200 fit at 0 and 200; the third goes back to the start once the first has
    // been sent, and a fourth then has no room, because it would reach the second's bytes.
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_, {16, 500});
    followers.add(first_, 10, wal_->scan_start_for(11));
    static_cast<void>(followers.send_to_all(10));

    write_and_keep(followers, 11);
    write_and_keep(followers, 12);
    static_cast<void>(followers.send_to_all(11));
    write_and_keep(followers, 13);
    write_and_keep(followers, 14);
    EXPECT_EQ(followers.records_kept(), 2U);
    static_cast<void>(followers.send_to_all(14));

    EXPECT_EQ(links_.sent_to(1), (std::vector<int64_t>{11, 12, 13, 14}));
    EXPECT_TRUE(sent_from_memory(1, 12)) << "the second record's bytes must not have been overwritten by the third's";
    EXPECT_TRUE(sent_from_memory(1, 13));
    EXPECT_TRUE(sent_from_wal(1, 14));
}

TEST_F(WalHandedOverFollowersTest, AFollowerOnItsOwnCursorIsNeverSentFromMemory) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_, {16, 64 * 1024});
    followers.add(first_, 10, wal_->scan_start_for(11));
    static_cast<void>(followers.send_to_all(10));
    // The second holds only up to 5, so it reads on its own cursor while it catches up.
    followers.add(second_, 5, wal_->scan_start_for(6));

    for (int64_t seq_no = 11; seq_no <= 13; ++seq_no) {
        write_and_keep(followers, seq_no);
    }
    static_cast<void>(followers.send_to(second_, 13));
    EXPECT_EQ(links_.sent_to(2), (std::vector<int64_t>{6, 7, 8, 9, 10, 11, 12, 13}));
    for (int64_t seq_no = 11; seq_no <= 13; ++seq_no) {
        EXPECT_TRUE(sent_from_wal(2, seq_no)) << "record " << seq_no;
    }
    static_cast<void>(followers.send_to_all(13));
    for (int64_t seq_no = 11; seq_no <= 13; ++seq_no) {
        EXPECT_TRUE(sent_from_memory(1, seq_no)) << "record " << seq_no;
    }
}

TEST_F(WalHandedOverFollowersTest, NothingIsKeptWhileNoFollowerIsInStepWithTheSharedReading) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_, {16, 64 * 1024});
    write_and_keep(followers, 11);
    EXPECT_EQ(followers.records_kept(), 0U) << "no follower at all";

    followers.add(first_, 11, wal_->scan_start_for(12));
    write_and_keep(followers, 12);
    EXPECT_EQ(followers.records_kept(), 0U) << "a follower on its own cursor only";
}

TEST_F(WalHandedOverFollowersTest, LosingEveryFollowerInStepDiscardsTheKeptRecords) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_, {16, 64 * 1024});
    followers.add(first_, 10, wal_->scan_start_for(11));
    static_cast<void>(followers.send_to_all(10));
    write_and_keep(followers, 11);
    write_and_keep(followers, 12);
    ASSERT_EQ(followers.records_kept(), 2U);
    followers.remove(first_);
    EXPECT_EQ(followers.records_kept(), 0U);

    followers.add(first_, 12, wal_->scan_start_for(13));
    static_cast<void>(followers.send_to_all(12));
    write_and_keep(followers, 13);
    ASSERT_EQ(followers.records_kept(), 1U);
    followers.remove_all();
    EXPECT_EQ(followers.records_kept(), 0U);
}

TEST_F(WalHandedOverFollowersTest, WithoutRoomEveryRecordIsReadFromTheWal) {
    append_through(10);
    WalHandedOverFollowers followers(*wal_, directory_, converter(), links_);
    followers.add(first_, 10, wal_->scan_start_for(11));
    static_cast<void>(followers.send_to_all(10));
    write_and_keep(followers, 11);
    EXPECT_EQ(followers.records_kept(), 0U);
    static_cast<void>(followers.send_to_all(11));
    EXPECT_TRUE(sent_from_wal(1, 11));
}

} // namespaces
