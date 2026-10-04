// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

#include <gtest/gtest.h>

#include "LogEpochTable.hpp"

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace {

// Appends records first..last, all from one epoch.
void append(sequencer::LogEpochTable& table, int64_t first, int64_t last, int32_t epoch) {
    for (int64_t seq_no = first; seq_no <= last; ++seq_no) {
        table.note_record(seq_no, epoch);
    }
}

// Runs the exchange between a follower and its leader until they agree, as the sequencers do, and
// returns the record the follower keeps its log through. Counts the rounds it took.
int64_t repair(sequencer::LogEpochTable& follower, const sequencer::LogEpochTable& leader, int& rounds) {
    rounds = 0;
    for (;;) {
        ++rounds;
        const auto answer = leader.answer_position(follower.last_seq_no(), follower.last_epoch());
        const auto step = follower.follower_step(answer);
        follower.truncate_after(step.keep_through);
        if (step.agreed) {
            return step.keep_through;
        }
    }
}

} // un-named namespace

TEST(LogEpochTableTest, TheEpochOfEachRecordIsKnown) {
    sequencer::LogEpochTable table;
    append(table, 1, 100, 1);
    append(table, 101, 150, 6);
    EXPECT_EQ(table.epoch_of(1), 1);
    EXPECT_EQ(table.epoch_of(100), 1);
    EXPECT_EQ(table.epoch_of(101), 6);
    EXPECT_EQ(table.last_epoch(), 6);
    EXPECT_EQ(table.last_record_at_or_before(5), 100);
    EXPECT_EQ(table.last_record_at_or_before(6), 150);
    EXPECT_EQ(table.last_record_at_or_before(0), 0);
}

TEST(LogEpochTableTest, ARecordOutOfOrderIsRefused) {
    sequencer::LogEpochTable table;
    append(table, 1, 10, 1);
    EXPECT_THROW(table.note_record(12, 1), pubsub_itc_fw::PreconditionAssertion);
}

TEST(LogEpochTableTest, AFollowerThatIsMerelyBehindKeepsEverything) {
    sequencer::LogEpochTable leader;
    append(leader, 1, 500, 5);
    sequencer::LogEpochTable follower;
    append(follower, 1, 200, 5);
    int rounds = 0;
    EXPECT_EQ(repair(follower, leader, rounds), 200);
    EXPECT_EQ(rounds, 1);
}

TEST(LogEpochTableTest, AnOldLeaderDiscardsTheRecordsItWroteThatTheNewLeaderDoesNotHave) {
    // The old leader wrote 101-200 at epoch 5 that never reached its follower; the follower then led at
    // epoch 6 and wrote its own 101-150.
    sequencer::LogEpochTable leader;
    append(leader, 1, 100, 5);
    append(leader, 101, 150, 6);
    sequencer::LogEpochTable old_leader;
    append(old_leader, 1, 200, 5);
    int rounds = 0;
    EXPECT_EQ(repair(old_leader, leader, rounds), 100);
    EXPECT_EQ(rounds, 1);
    EXPECT_EQ(old_leader.last_seq_no(), 100);
}

TEST(LogEpochTableTest, ADivergenceAcrossTwoChangesOfLeaderIsFound) {
    sequencer::LogEpochTable leader;
    append(leader, 1, 50, 1);
    append(leader, 51, 80, 6);
    append(leader, 81, 120, 9);
    sequencer::LogEpochTable follower;
    append(follower, 1, 50, 1);
    append(follower, 51, 90, 5);
    append(follower, 91, 95, 7);
    int rounds = 0;
    EXPECT_EQ(repair(follower, leader, rounds), 50);
    EXPECT_LE(rounds, 3);
}

TEST(LogEpochTableTest, AnEmptyFollowerAgreesAtNothing) {
    sequencer::LogEpochTable leader;
    append(leader, 1, 10, 2);
    sequencer::LogEpochTable follower;
    int rounds = 0;
    EXPECT_EQ(repair(follower, leader, rounds), 0);
}

TEST(LogEpochTableTest, AnEpochThatWentBackwardsStillEndsInAgreement) {
    // The follower holds a record at 61 from epoch 9 where the leader's 61 is from epoch 7 after the
    // leader's epoch went back from 9 to 7: the answer lands on a record from a different epoch, and the
    // follower steps back until the logs agree.
    sequencer::LogEpochTable leader;
    append(leader, 1, 60, 9);
    append(leader, 61, 70, 7);
    sequencer::LogEpochTable follower;
    append(follower, 1, 60, 9);
    append(follower, 61, 62, 10);
    int rounds = 0;
    EXPECT_EQ(repair(follower, leader, rounds), 60);
}

TEST(LogEpochTableTest, TruncatingRemovesTheRunsAfterThePoint) {
    sequencer::LogEpochTable table;
    append(table, 1, 100, 1);
    append(table, 101, 150, 6);
    table.truncate_after(100);
    EXPECT_EQ(table.last_seq_no(), 100);
    EXPECT_EQ(table.last_epoch(), 1);
    table.note_record(101, 8);
    EXPECT_EQ(table.epoch_of(101), 8);
}

TEST(LogEpochTableTest, AFollowerWhoseRecordsAreFromALaterEpochThanTheLeadersJumpsBackPastThemAll) {
    // The follower led at epoch 8 and wrote 101-500; the leader, whose log lacked them, led again at
    // epoch 9 holding its own 101-450 from epoch 6. The follower's records from epoch 8 are all
    // discarded in one step, not one record at a time.
    sequencer::LogEpochTable leader;
    append(leader, 1, 100, 5);
    append(leader, 101, 450, 6);
    append(leader, 451, 460, 9);
    sequencer::LogEpochTable follower;
    append(follower, 1, 100, 5);
    append(follower, 101, 500, 8);
    int rounds = 0;
    EXPECT_EQ(repair(follower, leader, rounds), 100);
    EXPECT_LE(rounds, 2);
}
