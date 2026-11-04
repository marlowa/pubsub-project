// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "OrderRelease.hpp"

namespace sequencer::tests {

namespace {

using Reading = OrderRelease::Reading;

// The WAL's positions, the engine and the gateways as OrderRelease sees them, and what it asks of them.
struct RecordingLinks : public OrderReleaseLinksInterface {
    [[nodiscard]] int64_t committed_through() const override {
        return committed;
    }
    [[nodiscard]] int64_t last_seq_no() const override {
        return last;
    }
    [[nodiscard]] bool waits_for_confirmation() const override {
        return waits;
    }
    [[nodiscard]] bool waits_for_follower() const override {
        return waits;
    }
    [[nodiscard]] bool leading() const override {
        return is_leading;
    }
    [[nodiscard]] bool an_engine_takes_orders() const override {
        return engine_takes_orders;
    }
    [[nodiscard]] pubsub_itc_fw::ConnectionID leading_engine_connection() const override {
        return engine;
    }
    void send_to_leading_engine(const HeldOrder& held) override {
        sent.push_back(held.envelope.seq_no);
        if (held.own_payload_size > 0) {
            own_payloads.emplace_back(reinterpret_cast<const char*>(held.own_payload.data()), held.own_payload_size);
        }
    }
    void send_orders_that_waited_in_the_wal(int64_t first) override {
        waited_in_wal.push_back(first);
    }
    void release_through(int64_t committed_through) override {
        ++release_through_calls;
        part->release_to_leading_engine(committed_through);
    }
    void send_engine_orders_after(int64_t after) override {
        engine_sent_after.push_back(after);
    }
    void release_message(pubsub_itc_fw::SlabHandle, const uint8_t*) override {
        ++messages_released;
    }
    void pause_reading(const pubsub_itc_fw::ConnectionID& connection) override {
        paused.push_back(connection.get_value());
    }
    void resume_reading(const pubsub_itc_fw::ConnectionID& connection) override {
        resumed.push_back(connection.get_value());
    }
    void released_position_moved(int64_t released_through) override {
        released.push_back(released_through);
    }
    void ask_engine_position(const pubsub_itc_fw::ConnectionID&, int64_t request_id) override {
        asks.push_back(request_id);
    }
    void held_orders_full(int64_t first_seq_no) override {
        full_events.push_back(first_seq_no);
    }

    OrderRelease* part{nullptr};
    int64_t committed{0};
    int64_t last{0};
    bool waits{true};
    bool is_leading{true};
    bool engine_takes_orders{true};
    pubsub_itc_fw::ConnectionID engine{51};
    std::vector<int64_t> sent;
    std::vector<std::string> own_payloads;
    std::vector<int64_t> waited_in_wal;
    int release_through_calls{0};
    std::vector<int64_t> engine_sent_after;
    int messages_released{0};
    std::vector<int> paused;
    std::vector<int> resumed;
    std::vector<int64_t> released;
    std::vector<int64_t> asks;
    std::vector<int64_t> full_events;
};

constexpr size_t capacity = 8;

class OrderReleaseTest : public ::testing::Test {
  protected:
    void SetUp() override {
        release_ = std::make_unique<OrderRelease>(capacity, links_, logger_);
        links_.part = release_.get();
    }

    // Writes order @p seq_no into the WAL, as far as OrderRelease can tell, and holds it.
    void order(int64_t seq_no) {
        links_.last = seq_no;
        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.seq_no = seq_no;
        release_->hold(envelope, pubsub_itc_fw::SlabHandle{}, buffer_);
    }

    void commit(int64_t through) {
        links_.committed = through;
        release_->release_to_leading_engine(through);
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<OrderRelease> release_;
    const uint8_t buffer_[1]{};
};

} // namespaces

TEST_F(OrderReleaseTest, AnOrderIsSentOnceItsRecordIsCommitted) {
    order(1);
    order(2);
    commit(1);
    EXPECT_EQ(links_.sent, (std::vector<int64_t>{1}));
    EXPECT_EQ(links_.messages_released, 1) << "the inbound buffer is released once the order is sent";
    EXPECT_EQ(release_->released_through(), 1);
    commit(2);
    EXPECT_EQ(links_.sent, (std::vector<int64_t>{1, 2}));
    EXPECT_EQ(release_->released_through(), 2);
}

TEST_F(OrderReleaseTest, OrdersStayHeldWhileNoEngineTakesThem) {
    links_.engine_takes_orders = false;
    order(1);
    commit(1);
    EXPECT_TRUE(links_.sent.empty());
    EXPECT_EQ(release_->released_through(), 0);
    links_.engine_takes_orders = true;
    commit(1);
    EXPECT_EQ(links_.sent, (std::vector<int64_t>{1}));
}

TEST_F(OrderReleaseTest, OrdersBeyondTheStorageWaitInTheWalAndAreSentFromThereInOrder) {
    for (int64_t seq_no = 1; seq_no <= static_cast<int64_t>(capacity) + 2; ++seq_no) {
        order(seq_no);
    }
    EXPECT_EQ(release_->first_unheld(), static_cast<int64_t>(capacity) + 1);
    EXPECT_EQ(links_.full_events, (std::vector<int64_t>{static_cast<int64_t>(capacity) + 1}));
    EXPECT_EQ(links_.messages_released, 2) << "an order not held has its buffer released at once";
    commit(static_cast<int64_t>(capacity) + 2);
    EXPECT_EQ(links_.sent.size(), capacity);
    EXPECT_EQ(links_.waited_in_wal, (std::vector<int64_t>{static_cast<int64_t>(capacity) + 1}));
    EXPECT_EQ(release_->first_unheld(), 0) << "every order that waited is released";
}

TEST_F(OrderReleaseTest, ACommandOfTheSequencersOwnIsHeldWithItsPayload) {
    links_.last = 1;
    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = 1;
    const std::string payload = "halt";
    release_->hold_own_command(envelope, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    commit(1);
    EXPECT_EQ(links_.own_payloads, (std::vector<std::string>{"halt"}));
    EXPECT_EQ(links_.messages_released, 0) << "no inbound buffer to release";
}

TEST_F(OrderReleaseTest, ReadingPausesAtThreeQuartersAndResumesAtAQuarter) {
    release_->order_connection_established(pubsub_itc_fw::ConnectionID{61});
    for (int64_t seq_no = 1; seq_no <= 6; ++seq_no) {
        order(seq_no);
        release_->pause_or_resume_reading();
    }
    EXPECT_EQ(release_->reading(), Reading::paused_for_held_orders);
    EXPECT_EQ(links_.paused, (std::vector<int>{61}));
    release_->order_connection_established(pubsub_itc_fw::ConnectionID{62});
    EXPECT_EQ(links_.paused, (std::vector<int>{61, 62})) << "a gateway that connects while reading is paused is paused too";
    commit(3);
    EXPECT_EQ(release_->reading(), Reading::paused_for_held_orders) << "three still held, above a quarter";
    commit(4);
    EXPECT_EQ(release_->reading(), Reading::reading);
    EXPECT_EQ(links_.resumed, (std::vector<int>{61, 62}));
}

TEST_F(OrderReleaseTest, ReadingIsNotPausedWhenTheLeaderWaitsForNobody) {
    links_.waits = false;
    release_->order_connection_established(pubsub_itc_fw::ConnectionID{61});
    links_.engine_takes_orders = false;
    for (int64_t seq_no = 1; seq_no <= 7; ++seq_no) {
        order(seq_no);
        release_->pause_or_resume_reading();
    }
    EXPECT_EQ(release_->reading(), Reading::reading);
}

TEST_F(OrderReleaseTest, AHandOverPausesReadingUntilItFinishes) {
    release_->order_connection_established(pubsub_itc_fw::ConnectionID{61});
    release_->pause_for_hand_over();
    EXPECT_EQ(release_->reading(), Reading::paused_for_hand_over);
    release_->pause_or_resume_reading();
    EXPECT_EQ(release_->reading(), Reading::paused_for_hand_over) << "held orders do not end a pause for a hand-over";
    release_->end_pause_for_hand_over();
    EXPECT_EQ(release_->reading(), Reading::reading);
    EXPECT_EQ(links_.resumed, (std::vector<int>{61}));
}

TEST_F(OrderReleaseTest, ANewLeaderHoldsOrdersUntilTheEngineSaysWhereItIs) {
    links_.last = 5;
    release_->ask_engine_for_position();
    EXPECT_TRUE(release_->awaiting_engine_position());
    EXPECT_EQ(links_.asks, (std::vector<int64_t>{1}));
    order(6);
    commit(6);
    EXPECT_TRUE(links_.sent.empty()) << "held while the engine's position is awaited";
    release_->engine_position(1, 3);
    EXPECT_FALSE(release_->awaiting_engine_position());
    EXPECT_EQ(links_.engine_sent_after, (std::vector<int64_t>{3})) << "the engine lacks 4 and 5";
    EXPECT_EQ(links_.release_through_calls, 1);
    EXPECT_EQ(links_.sent, (std::vector<int64_t>{6}));
}

TEST_F(OrderReleaseTest, AnAnswerToAnEarlierAskIsIgnoredAndALateAnswerIsAskedForAgain) {
    links_.last = 5;
    release_->ask_engine_for_position();
    const auto now = std::chrono::steady_clock::now();
    release_->tick(now);
    EXPECT_EQ(links_.asks.size(), 1U) << "not yet overdue";
    release_->tick(now + OrderRelease::engine_position_ask_interval + std::chrono::milliseconds{1});
    EXPECT_EQ(links_.asks, (std::vector<int64_t>{1, 2}));
    release_->engine_position(1, 5);
    EXPECT_TRUE(release_->awaiting_engine_position()) << "the answer to the first ask, not the latest";
    release_->engine_position(2, 5);
    EXPECT_FALSE(release_->awaiting_engine_position());
    EXPECT_TRUE(links_.engine_sent_after.empty()) << "the engine lacks nothing";
}

TEST_F(OrderReleaseTest, WithNoEngineToAskNothingIsWaitedFor) {
    links_.engine = pubsub_itc_fw::ConnectionID{};
    release_->ask_engine_for_position();
    EXPECT_FALSE(release_->awaiting_engine_position());
    EXPECT_TRUE(links_.asks.empty());
}

TEST_F(OrderReleaseTest, StoppingLeadingDiscardsTheHeldOrders) {
    order(1);
    order(2);
    links_.last = 5;
    release_->ask_engine_for_position();
    release_->stopped_leading();
    EXPECT_EQ(release_->held(), 0U);
    EXPECT_EQ(links_.messages_released, 2);
    EXPECT_FALSE(release_->awaiting_engine_position());
    commit(2);
    EXPECT_TRUE(links_.sent.empty());
}

TEST_F(OrderReleaseTest, AFullStorageOfUnacknowledgedOrdersMeansTheFollowerIsTooFarBehind) {
    for (int64_t seq_no = 1; seq_no <= static_cast<int64_t>(capacity); ++seq_no) {
        order(seq_no);
    }
    EXPECT_TRUE(release_->held_orders_wait_on_the_follower());
    links_.committed = 1;
    EXPECT_FALSE(release_->held_orders_wait_on_the_follower()) << "the oldest is committed, so they wait for an engine, not the follower";
}

} // namespaces
