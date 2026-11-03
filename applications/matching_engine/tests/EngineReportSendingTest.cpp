// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "EngineReportSending.hpp"
#include "GatewayIds.hpp"
#include "HeldEngineReport.hpp"
#include "SessionIdentity.hpp"

namespace matching_engine::tests {

namespace {

using Sequencer = EngineReportSending::Sequencer;

// What EngineReportSending asks the thread to do, copied, because the envelope is valid only during the call.
struct RecordingLinks : public EngineReportSendingLinksInterface {
    struct Sent {
        int connection{0};
        int64_t seq_no{0};
        std::string payload;
        int32_t epoch{0};
        int64_t number{0};
        bool poss_resend{false};
        bool has_gateway_ingress_ns{false};
        int64_t gateway_ingress_ns{0};
        std::string comp_id;
    };
    void send_report(const pubsub_itc_fw::ConnectionID& connection, int64_t seq_no, const pubsub_itc_fw_app::WalRecord& envelope) override {
        sent.push_back(Sent{connection.get_value(), seq_no, std::string(reinterpret_cast<const char*>(envelope.payload.data), envelope.payload.size),
                            envelope.report_engine_epoch, envelope.report_number, envelope.poss_resend, envelope.has_gateway_ingress_ns,
                            envelope.gateway_ingress_ns, std::string(envelope.sender_comp_id)});
    }
    void reports_held(int64_t count) override {
        held_events.push_back(count);
    }
    void reports_released(pubsub_itc_fw_app::ReleaseReason reason, int64_t count) override {
        released_events.push_back({reason, count});
    }
    void reports_discarded(int64_t count) override {
        discarded_events.push_back(count);
    }
    // The payloads sent on @p connection, in the order sent.
    [[nodiscard]] std::vector<std::string> sent_on(int connection) const {
        std::vector<std::string> payloads;
        for (const Sent& one : sent) {
            if (one.connection == connection) {
                payloads.push_back(one.payload);
            }
        }
        return payloads;
    }
    std::vector<Sent> sent;
    std::vector<int64_t> held_events;
    std::vector<std::pair<pubsub_itc_fw_app::ReleaseReason, int64_t>> released_events;
    std::vector<int64_t> discarded_events;
};

class EngineReportSendingTest : public ::testing::Test {
  protected:
    void SetUp() override {
        sending_ = std::make_unique<EngineReportSending>(true, links_, logger_);
    }

    // Sends a report whose bytes are @p text, answering command @p seq_no.
    bool send(const std::string& text, int64_t seq_no = 1, int32_t epoch = 7, bool holding_until_entitled = false, bool poss_resend = false) {
        const EngineReportSending::Report report{reinterpret_cast<const uint8_t*>(text.data()), text.size(), seq_no, &session_, poss_resend, false, 0};
        return sending_->send(report, epoch, holding_until_entitled);
    }

    void connect_both() {
        sending_->connection_established(Sequencer::primary, primary_);
        sending_->connection_established(Sequencer::secondary, secondary_);
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<EngineReportSending> sending_;
    const fix_common::SessionIdentity session_ = fix_common::SessionIdentity::make("MEMBER", gateway_ids::fix_order_gateway);
    const pubsub_itc_fw::ConnectionID primary_{11};
    const pubsub_itc_fw::ConnectionID secondary_{12};
};

} // namespaces

TEST_F(EngineReportSendingTest, AReportIsSentToBothSequencersNumberedFromOneUnderItsEpoch) {
    connect_both();
    ASSERT_TRUE(send("report-1"));
    ASSERT_TRUE(send("report-2"));
    EXPECT_EQ(links_.sent_on(11), (std::vector<std::string>{"report-1", "report-2"}));
    EXPECT_EQ(links_.sent_on(12), (std::vector<std::string>{"report-1", "report-2"}));
    EXPECT_EQ(links_.sent[0].number, 1);
    EXPECT_EQ(links_.sent[1].number, 1) << "the same number on each connection";
    EXPECT_EQ(links_.sent[2].number, 2);
    EXPECT_EQ(links_.sent[0].epoch, 7);
    EXPECT_EQ(links_.sent[0].comp_id, "MEMBER");
}

TEST_F(EngineReportSendingTest, ANewEpochNumbersFromOneAgain) {
    connect_both();
    ASSERT_TRUE(send("report-1", 1, 7));
    ASSERT_TRUE(send("report-2", 2, 7));
    ASSERT_TRUE(send("report-3", 3, 8));
    EXPECT_EQ(links_.sent.back().number, 1);
    EXPECT_EQ(links_.sent.back().epoch, 8);
}

TEST_F(EngineReportSendingTest, TheMarkOfAPossibleRepeatAndTheGatewaysTimeAreCarried) {
    connect_both();
    const std::string text = "report";
    const EngineReportSending::Report report{reinterpret_cast<const uint8_t*>(text.data()), text.size(), 5, &session_, true, true, 123456789};
    ASSERT_TRUE(sending_->send(report, 7, false));
    EXPECT_TRUE(links_.sent[0].poss_resend);
    EXPECT_TRUE(links_.sent[0].has_gateway_ingress_ns);
    EXPECT_EQ(links_.sent[0].gateway_ingress_ns, 123456789);
    EXPECT_EQ(links_.sent[0].seq_no, 5);
}

TEST_F(EngineReportSendingTest, ReportsAreHeldUntilTheEngineServesAndThenSentNumberedUnderTheEpochThen) {
    connect_both();
    EXPECT_FALSE(send("report-1", 1, 7, true));
    EXPECT_FALSE(send("report-2", 2, 7, true));
    EXPECT_TRUE(links_.sent.empty());
    sending_->release_for_connection(7, true);
    EXPECT_TRUE(links_.sent.empty()) << "a connection made does not release reports held until the engine knows whether it serves";
    sending_->release_now_serving(9);
    EXPECT_EQ(links_.sent_on(11), (std::vector<std::string>{"report-1", "report-2"}));
    EXPECT_EQ(links_.sent_on(12), (std::vector<std::string>{"report-1", "report-2"}));
    EXPECT_EQ(links_.sent[0].epoch, 9);
    ASSERT_EQ(links_.released_events.size(), 1U);
    EXPECT_EQ(links_.released_events[0].first, pubsub_itc_fw_app::ReleaseReason::NowServes);
    EXPECT_EQ(links_.released_events[0].second, 2);
    EXPECT_EQ(sending_->held(), 0U);
}

TEST_F(EngineReportSendingTest, ReportsHeldUntilTheEngineServesAreDiscardedWhenItFollows) {
    connect_both();
    EXPECT_FALSE(send("report-1", 1, 7, true));
    sending_->discard("this instance follows");
    EXPECT_EQ(links_.discarded_events, (std::vector<int64_t>{1}));
    sending_->release_now_serving(7);
    EXPECT_TRUE(links_.sent.empty());
}

TEST_F(EngineReportSendingTest, WithNoConnectionReportsAreHeldAndTheStartIsSaidOnce) {
    EXPECT_FALSE(send("report-1"));
    EXPECT_FALSE(send("report-2"));
    EXPECT_EQ(links_.held_events, (std::vector<int64_t>{1})) << "said once, counting the report that began it";
    EXPECT_EQ(sending_->held(), 2U);
}

TEST_F(EngineReportSendingTest, ReportsHeldForAConnectionAreSentWhenOneIsEstablished) {
    EXPECT_FALSE(send("report-1"));
    sending_->connection_established(Sequencer::primary, primary_);
    sending_->release_for_connection(7, false);
    EXPECT_EQ(links_.sent_on(11), (std::vector<std::string>{"report-1"}));
    ASSERT_EQ(links_.released_events.size(), 1U);
    EXPECT_EQ(links_.released_events[0].first, pubsub_itc_fw_app::ReleaseReason::ConnectionMade);
    EXPECT_EQ(sending_->held(), 1U) << "kept until it has been sent to the secondary too";
}

TEST_F(EngineReportSendingTest, AHeldReportSentToOneSequencerGoesToTheOtherWithTheSameNumber) {
    EXPECT_FALSE(send("report-1"));
    sending_->connection_established(Sequencer::primary, primary_);
    sending_->release_for_connection(7, false);
    sending_->connection_established(Sequencer::secondary, secondary_);
    sending_->release_for_connection(8, false);
    ASSERT_EQ(links_.sent.size(), 2U);
    EXPECT_EQ(links_.sent[1].connection, 12);
    EXPECT_EQ(links_.sent[1].number, links_.sent[0].number);
    EXPECT_EQ(links_.sent[1].epoch, 7) << "the number it was given when first sent";
    EXPECT_EQ(sending_->held(), 0U);
    links_.held_events.clear();
    sending_->connection_lost(Sequencer::primary);
    sending_->connection_lost(Sequencer::secondary);
    EXPECT_FALSE(send("report-2"));
    EXPECT_EQ(links_.held_events, (std::vector<int64_t>{1})) << "holding for want of a connection is said again once it has ended";
}

TEST_F(EngineReportSendingTest, WithHighAvailabilityOffAReportSentToThePrimaryIsNotKeptForASecondary) {
    sending_ = std::make_unique<EngineReportSending>(false, links_, logger_);
    EXPECT_FALSE(send("report-1"));
    sending_->connection_established(Sequencer::primary, primary_);
    sending_->release_for_connection(7, false);
    EXPECT_EQ(sending_->held(), 0U);
}

TEST_F(EngineReportSendingTest, PastTheLimitWhileHoldingUntilTheEngineServesReportsAreSent) {
    connect_both();
    for (size_t index = 0; index < EngineReportSending::max_held_reports; ++index) {
        ASSERT_FALSE(send("held", 1, 7, true));
    }
    EXPECT_TRUE(send("over-the-limit", 2, 7, true));
    EXPECT_EQ(links_.sent_on(11), (std::vector<std::string>{"over-the-limit"}));
    EXPECT_EQ(sending_->held(), EngineReportSending::max_held_reports);
}

TEST_F(EngineReportSendingTest, PastTheLimitWithNoConnectionReportsAreLost) {
    for (size_t index = 0; index < EngineReportSending::max_held_reports; ++index) {
        ASSERT_FALSE(send("held"));
    }
    EXPECT_FALSE(send("over-the-limit"));
    EXPECT_EQ(sending_->held(), EngineReportSending::max_held_reports);
    sending_->connection_established(Sequencer::primary, primary_);
    sending_->release_for_connection(7, false);
    const std::vector<std::string> sent = links_.sent_on(11);
    EXPECT_EQ(sent.size(), EngineReportSending::max_held_reports);
    EXPECT_EQ(sent.back(), "held");
}

TEST_F(EngineReportSendingTest, ReportsKeptWhileFollowingAreSentWhenTheEngineTakesTheLead) {
    HeldEngineReport kept;
    kept.payload = {'k', 'e', 'p', 't'};
    kept.seq_no = 4;
    kept.session = session_;
    kept.poss_resend = true;
    sending_->hold(kept);
    sending_->send_held_on_the_connections_up(7);
    EXPECT_TRUE(links_.sent.empty()) << "no connection is up";
    connect_both();
    sending_->send_held_on_the_connections_up(7);
    EXPECT_EQ(links_.sent_on(11), (std::vector<std::string>{"kept"}));
    EXPECT_EQ(links_.sent_on(12), (std::vector<std::string>{"kept"}));
    EXPECT_TRUE(links_.sent[0].poss_resend);
    EXPECT_TRUE(links_.released_events.empty()) << "said nothing";
}

} // namespaces
