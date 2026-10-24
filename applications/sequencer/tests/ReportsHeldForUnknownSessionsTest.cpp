// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "GatewayIds.hpp"
#include "ReportsHeldForUnknownSessions.hpp"
#include "SessionIdentity.hpp"

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace {

using sequencer::ReportsHeldForUnknownSessions;

fix_common::SessionIdentity session(const std::string& comp_id) {
    return fix_common::SessionIdentity::make(comp_id, gateway_ids::fix_order_gateway);
}

// The contents of report number: size bytes, each the low byte of number plus its position, so that a record
// overwritten in part, or handed back from the wrong place, does not match.
std::vector<uint8_t> report_bytes(int number, size_t size) {
    std::vector<uint8_t> bytes(size);
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<uint8_t>(number + static_cast<int>(index));
    }
    return bytes;
}

void hold(ReportsHeldForUnknownSessions& store, const std::string& comp_id, int number, size_t size) {
    const std::vector<uint8_t> bytes = report_bytes(number, size);
    store.hold(session(comp_id), bytes.data(), bytes.size());
}

// What release() hands back for comp_id, as the byte vectors handed back, in order.
std::vector<std::vector<uint8_t>> release(ReportsHeldForUnknownSessions& store, const std::string& comp_id) {
    std::vector<std::vector<uint8_t>> handed_back;
    store.release(session(comp_id), [&handed_back](const uint8_t* bytes, size_t size) { handed_back.emplace_back(bytes, bytes + size); });
    return handed_back;
}

// What take_all() hands back, as (comp id, bytes) pairs, in order.
std::vector<std::pair<std::string, std::vector<uint8_t>>> take_all(ReportsHeldForUnknownSessions& store) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> taken;
    store.take_all([&taken](const fix_common::SessionIdentity& identity, const uint8_t* bytes, size_t size) {
        taken.emplace_back(std::string(identity.comp_id_view()), std::vector<uint8_t>(bytes, bytes + size));
    });
    return taken;
}

TEST(ReportsHeldForUnknownSessionsTest, ReleaseHandsBackOnlyThatSessionsReportsOldestFirst) {
    ReportsHeldForUnknownSessions store(4096, 16);
    hold(store, "A", 1, 10);
    hold(store, "B", 2, 20);
    hold(store, "A", 3, 30);
    hold(store, "B", 4, 40);

    const auto released = release(store, "A");
    ASSERT_EQ(released.size(), 2U);
    EXPECT_EQ(released[0], report_bytes(1, 10));
    EXPECT_EQ(released[1], report_bytes(3, 30));
    EXPECT_EQ(store.count(), 2U);

    // Releasing again hands back nothing: the reports are no longer held.
    EXPECT_TRUE(release(store, "A").empty());
}

TEST(ReportsHeldForUnknownSessionsTest, SessionsWithTheSameCompIdButDifferentProtocolsAreKeptApart) {
    ReportsHeldForUnknownSessions store(4096, 16);
    const std::vector<uint8_t> fix = report_bytes(1, 10);
    const std::vector<uint8_t> binary = report_bytes(2, 10);
    store.hold(fix_common::SessionIdentity::make("A", gateway_ids::fix_order_gateway), fix.data(), fix.size());
    store.hold(fix_common::SessionIdentity::make("A", gateway_ids::binary_order_gateway), binary.data(), binary.size());

    std::vector<std::vector<uint8_t>> handed_back;
    store.release(fix_common::SessionIdentity::make("A", gateway_ids::binary_order_gateway),
                  [&handed_back](const uint8_t* bytes, size_t size) { handed_back.emplace_back(bytes, bytes + size); });
    ASSERT_EQ(handed_back.size(), 1U);
    EXPECT_EQ(handed_back[0], binary);
    EXPECT_EQ(store.count(), 1U);
}

TEST(ReportsHeldForUnknownSessionsTest, TakeAllHandsBackEveryReportStillHeldOldestFirstAndEmptiesTheStore) {
    ReportsHeldForUnknownSessions store(4096, 16);
    hold(store, "A", 1, 10);
    hold(store, "B", 2, 20);
    hold(store, "C", 3, 30);
    (void)release(store, "B");

    const auto taken = take_all(store);
    ASSERT_EQ(taken.size(), 2U);
    EXPECT_EQ(taken[0].first, "A");
    EXPECT_EQ(taken[0].second, report_bytes(1, 10));
    EXPECT_EQ(taken[1].first, "C");
    EXPECT_EQ(taken[1].second, report_bytes(3, 30));
    EXPECT_EQ(store.count(), 0U);
    EXPECT_EQ(store.bytes_used(), 0U);
    EXPECT_TRUE(take_all(store).empty());
}

TEST(ReportsHeldForUnknownSessionsTest, ForEachHeldShowsEveryReportStillHeldOldestFirstAndKeepsThem) {
    ReportsHeldForUnknownSessions store(4096, 16);
    hold(store, "A", 1, 10);
    hold(store, "B", 2, 20);
    hold(store, "C", 3, 30);
    (void)release(store, "B");

    std::vector<std::pair<std::string, std::vector<uint8_t>>> seen;
    store.for_each_held([&seen](const fix_common::SessionIdentity& identity, const uint8_t* bytes, size_t size) {
        seen.emplace_back(std::string(identity.comp_id_view()), std::vector<uint8_t>(bytes, bytes + size));
    });
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[0].first, "A");
    EXPECT_EQ(seen[0].second, report_bytes(1, 10));
    EXPECT_EQ(seen[1].first, "C");
    EXPECT_EQ(store.count(), 2U);
    EXPECT_EQ(take_all(store).size(), 2U);
}

TEST(ReportsHeldForUnknownSessionsTest, DiscardAllHandsNothingBack) {
    ReportsHeldForUnknownSessions store(4096, 16);
    hold(store, "A", 1, 10);
    hold(store, "B", 2, 20);
    store.discard_all();
    EXPECT_EQ(store.count(), 0U);
    EXPECT_TRUE(release(store, "A").empty());
    EXPECT_TRUE(take_all(store).empty());
    EXPECT_EQ(store.dropped_for_room(), 0);
}

TEST(ReportsHeldForUnknownSessionsTest, WhenTheListIsFullTheOldestReportIsDroppedAndCounted) {
    ReportsHeldForUnknownSessions store(4096, 3);
    hold(store, "A", 1, 10);
    hold(store, "B", 2, 10);
    hold(store, "C", 3, 10);
    hold(store, "D", 4, 10);

    EXPECT_EQ(store.dropped_for_room(), 1);
    EXPECT_EQ(store.count(), 3U);
    EXPECT_TRUE(release(store, "A").empty());
    const auto taken = take_all(store);
    ASSERT_EQ(taken.size(), 3U);
    EXPECT_EQ(taken[0].first, "B");
    EXPECT_EQ(taken[2].first, "D");
    EXPECT_EQ(taken[2].second, report_bytes(4, 10));
}

TEST(ReportsHeldForUnknownSessionsTest, WhenTheBytesAreFullTheOldestReportsAreDroppedUntilThereIsRoom) {
    ReportsHeldForUnknownSessions store(100, 16);
    hold(store, "A", 1, 40);
    hold(store, "B", 2, 40);
    // 60 bytes are needed and 20 are free at the end, so the record goes at the start, where A's 40 bytes and
    // then B's must be given up.
    hold(store, "C", 3, 60);

    EXPECT_EQ(store.dropped_for_room(), 2);
    const auto taken = take_all(store);
    ASSERT_EQ(taken.size(), 1U);
    EXPECT_EQ(taken[0].first, "C");
    EXPECT_EQ(taken[0].second, report_bytes(3, 60));
}

TEST(ReportsHeldForUnknownSessionsTest, AReportHandedBackIsNotCountedAsDroppedWhenItsRoomIsReused) {
    ReportsHeldForUnknownSessions store(100, 16);
    hold(store, "A", 1, 40);
    hold(store, "B", 2, 40);
    (void)release(store, "A");
    hold(store, "C", 3, 40);

    EXPECT_EQ(store.dropped_for_room(), 0);
    const auto taken = take_all(store);
    ASSERT_EQ(taken.size(), 2U);
    EXPECT_EQ(taken[0].second, report_bytes(2, 40));
    EXPECT_EQ(taken[1].second, report_bytes(3, 40));
}

TEST(ReportsHeldForUnknownSessionsTest, AReportHandedBackBehindAnOlderOneKeepsItsRoomUntilTheOlderOneGoes) {
    ReportsHeldForUnknownSessions store(100, 16);
    hold(store, "A", 1, 30);
    hold(store, "B", 2, 30);
    hold(store, "C", 3, 30);
    (void)release(store, "B");
    // B's room is between A's and C's, so it cannot be used while A is held: 10 bytes are free at the end and none at
    // the start, so A is dropped to make room.
    hold(store, "D", 4, 30);

    EXPECT_EQ(store.dropped_for_room(), 1);
    const auto taken = take_all(store);
    ASSERT_EQ(taken.size(), 2U);
    EXPECT_EQ(taken[0].first, "C");
    EXPECT_EQ(taken[1].first, "D");
    EXPECT_EQ(taken[1].second, report_bytes(4, 30));
}

TEST(ReportsHeldForUnknownSessionsTest, OnceTheReportsBeforeTheBlockContinuedFromItsStartAreGoneTheRoomIsOneStretchAgain) {
    ReportsHeldForUnknownSessions store(100, 16);
    hold(store, "A", 1, 60); // 0 to 60
    hold(store, "B", 2, 30); // 60 to 90
    (void)release(store, "A");
    hold(store, "C", 3, 50); // 10 bytes free at the end, so 0 to 50
    (void)release(store, "B");
    // C alone is held, from 0 to 50, in one stretch. 60 bytes are needed: 50 are free at the end, which is not
    // enough, and none at the start, so C must be dropped and D written at the start. Taking the room as still being
    // in two stretches would write D from 50 to 110, past the end of the block.
    hold(store, "D", 4, 60);

    EXPECT_EQ(store.dropped_for_room(), 1);
    const auto taken = take_all(store);
    ASSERT_EQ(taken.size(), 1U);
    EXPECT_EQ(taken[0].first, "D");
    EXPECT_EQ(taken[0].second, report_bytes(4, 60));
}

TEST(ReportsHeldForUnknownSessionsTest, ARecordLargerThanTheBlockIsDroppedAndCounted) {
    ReportsHeldForUnknownSessions store(100, 16);
    hold(store, "A", 1, 101);
    EXPECT_EQ(store.dropped_for_room(), 1);
    EXPECT_EQ(store.count(), 0U);
}

TEST(ReportsHeldForUnknownSessionsTest, ARecordOfNoBytesIsRefused) {
    ReportsHeldForUnknownSessions store(100, 16);
    const uint8_t byte = 0;
    EXPECT_THROW(store.hold(session("A"), &byte, 0), pubsub_itc_fw::PreconditionAssertion);
}

TEST(ReportsHeldForUnknownSessionsTest, ManyRoundsThroughTheBlockHandBackExactlyWhatWasHeld) {
    // Each round holds a report for a new session and releases the previous round's, so one report is always still
    // held and the block continues from its start again and again. Sizes vary, so the records land everywhere.
    ReportsHeldForUnknownSessions store(1000, 8);
    auto size_of = [](int round) { return static_cast<size_t>(17 + (round * 37) % 150); };
    hold(store, "S0", 0, size_of(0));
    for (int round = 1; round < 2000; ++round) {
        hold(store, "S" + std::to_string(round), round, size_of(round));
        const auto released = release(store, "S" + std::to_string(round - 1));
        ASSERT_EQ(released.size(), 1U) << "round " << round;
        EXPECT_EQ(released[0], report_bytes(round - 1, size_of(round - 1))) << "round " << round;
        ASSERT_EQ(store.count(), 1U) << "round " << round;
    }
    EXPECT_EQ(store.dropped_for_room(), 0);
}

} // namespaces
