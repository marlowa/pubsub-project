// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <fix_orders.hpp>

#include "GatewayIds.hpp"
#include "ReportForwarding.hpp"

namespace sequencer::tests {

namespace {

constexpr int16_t fix = gateway_ids::fix_order_gateway;

// What ReportForwarding asks the thread to do, and the member sessions it reaches through it.
struct RecordingLinks : public ReportForwardingLinksInterface {
    struct Sent {
        int connection{0};
        int64_t er_seq_no{0};
        std::string payload;
        bool encoded{false};
    };
    void send_report(const pubsub_itc_fw::ConnectionID& connection, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope) override {
        sent.push_back(
            Sent{connection.get_value(), er_seq_no, std::string(reinterpret_cast<const char*>(envelope.payload.data), envelope.payload.size), false});
    }
    void send_encoded_report(const pubsub_itc_fw::ConnectionID& connection, int64_t er_seq_no, const uint8_t*, size_t) override {
        sent.push_back(Sent{connection.get_value(), er_seq_no, "", true});
    }
    void acknowledgement_sent(const pubsub_itc_fw_app::WalRecord&) override {
        ++acknowledgements;
    }
    [[nodiscard]] bool leading() const override {
        return is_leading;
    }
    [[nodiscard]] bool expects_no_session_for(std::string_view comp_id) const override {
        return comp_id == "NOSESSION";
    }
    [[nodiscard]] const fix_common::SessionDestination* destination(const fix_common::SessionIdentity& identity) const override {
        const auto it = destinations.find(std::string(identity.comp_id_view()));
        return it == destinations.end() ? nullptr : &it->second;
    }
    void report_forwarded(const fix_common::SessionIdentity&) override {
        ++forwarded;
    }
    void report_dropped_unbound(const fix_common::SessionIdentity&, int64_t er_seq_no) override {
        dropped_unbound.push_back(er_seq_no);
    }
    void order_ended(int64_t seq_no) override {
        ended.push_back(seq_no);
    }
    void reports_held(const fix_common::SessionIdentity&, int32_t) override {
        ++held_events;
    }
    void held_reports_released(std::string_view, int16_t, int64_t sent_count, int64_t dropped) override {
        released.emplace_back(sent_count, dropped);
    }
    void report_dropped_gateway_not_connected(int16_t, int16_t, int64_t er_seq_no) override {
        dropped_no_gateway.push_back(er_seq_no);
    }
    void reports_held_for_gateway(int16_t, int16_t, int64_t) override {
        ++held_for_gateway_events;
    }
    void reports_sent_to_gateway(int16_t, int16_t, int64_t sent_count, int64_t dropped) override {
        sent_to_gateway.emplace_back(sent_count, dropped);
    }
    void held_reports_dropped(int64_t) override {}
    void release_report_chunk(pubsub_itc_fw::SlabHandle slab, const uint8_t* chunk) override {
        released_chunks.emplace_back(slab, chunk);
    }
    std::vector<std::pair<pubsub_itc_fw::SlabHandle, const uint8_t*>> released_chunks;

    bool is_leading{true};
    std::map<std::string, fix_common::SessionDestination> destinations;
    std::vector<Sent> sent;
    int acknowledgements{0};
    int forwarded{0};
    std::vector<int64_t> dropped_unbound;
    std::vector<int64_t> ended;
    int held_events{0};
    std::vector<std::pair<int64_t, int64_t>> released;
    std::vector<int64_t> dropped_no_gateway;
    int held_for_gateway_events{0};
    std::vector<std::pair<int64_t, int64_t>> sent_to_gateway;
};

class ReportForwardingTest : public ::testing::Test {
  protected:
    void SetUp() override {
        forwarding_ = std::make_unique<ReportForwarding>(std::vector<std::pair<int16_t, int16_t>>{{fix, 1}, {fix, 2}}, links_, logger_);
    }

    static ReportForwarding::Pending pending(const std::string& comp_id, int64_t seq_no, const std::string& payload,
                                             std::optional<EngineReportPosition> position = std::nullopt) {
        ReportForwarding::Pending report{};
        report.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
        report.seq_no = seq_no;
        report.own_copy_of(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        report.identity = fix_common::SessionIdentity::make(comp_id, fix);
        report.position = position;
        return report;
    }

    // A member connected through gateway instance 1 on connection 7, with the gateway's connection for reports up as 31.
    void member_connected() {
        fix_common::SessionDestination destination{};
        destination.instance = 1;
        destination.conn_id = 7;
        links_.destinations["MEMBER"] = destination;
        forwarding_->gateway_connected(fix, 1, pubsub_itc_fw::ConnectionID{31});
    }

    void both_gateways_announce() {
        forwarding_->sessions_announced(fix, 1, pubsub_itc_fw::ConnectionID{41}, 1);
        forwarding_->sessions_announced(fix, 2, pubsub_itc_fw::ConnectionID{42}, 0);
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<ReportForwarding> forwarding_;
};

} // namespaces

TEST_F(ReportForwardingTest, AReportToAConnectedGatewayIsSentAndAnAcknowledgementTimed) {
    member_connected();
    pubsub_itc_fw_app::WalRecord envelope{};
    forwarding_->send_to_gateway(fix, 1, 5, envelope, true, std::nullopt);
    ASSERT_EQ(links_.sent.size(), 1U);
    EXPECT_EQ(links_.sent[0].connection, 31);
    EXPECT_EQ(links_.acknowledgements, 1);
}

TEST_F(ReportForwardingTest, AReportForAGatewayWithNoConnectionIsHeldAndSentWhenTheConnectionIsMade) {
    pubsub_itc_fw_app::WalRecord envelope{};
    forwarding_->send_to_gateway(fix, 1, 5, envelope, false, std::nullopt);
    forwarding_->send_to_gateway(fix, 1, 6, envelope, false, std::nullopt);
    EXPECT_TRUE(links_.sent.empty());
    EXPECT_EQ(links_.held_for_gateway_events, 1) << "said once";
    EXPECT_EQ(forwarding_->held_for_gateway(fix, 1), 2U);
    forwarding_->gateway_connected(fix, 1, pubsub_itc_fw::ConnectionID{31});
    ASSERT_EQ(links_.sent.size(), 2U);
    EXPECT_TRUE(links_.sent[0].encoded);
    EXPECT_EQ(links_.sent[0].er_seq_no, 5);
    EXPECT_EQ(links_.sent_to_gateway, (std::vector<std::pair<int64_t, int64_t>>{{2, 0}}));
}

TEST_F(ReportForwardingTest, AFollowerDropsAReportForAGatewayWithNoConnection) {
    links_.is_leading = false;
    pubsub_itc_fw_app::WalRecord envelope{};
    forwarding_->send_to_gateway(fix, 1, 5, envelope, false, std::nullopt);
    EXPECT_EQ(links_.dropped_no_gateway, (std::vector<int64_t>{5}));
    EXPECT_EQ(forwarding_->held_for_gateway(fix, 1), 0U);
}

TEST_F(ReportForwardingTest, AReportForAGatewayNotConfiguredIsDropped) {
    pubsub_itc_fw_app::WalRecord envelope{};
    forwarding_->send_to_gateway(fix, 9, 5, envelope, false, std::nullopt);
    EXPECT_EQ(links_.dropped_no_gateway, (std::vector<int64_t>{5}));
}

TEST_F(ReportForwardingTest, AReportForAnUnannouncedSessionIsHeldUntilEveryGatewayHasAnnounced) {
    EXPECT_TRUE(forwarding_->hold_for_unannounced_session(pending("AWAY", 5, "r5")));
    EXPECT_TRUE(forwarding_->hold_for_unannounced_session(pending("AWAY", 6, "r6")));
    EXPECT_EQ(links_.held_events, 1) << "said once";
    forwarding_->sessions_announced(fix, 1, pubsub_itc_fw::ConnectionID{41}, 1);
    EXPECT_EQ(forwarding_->held_for_sessions(), 2U) << "one gateway has not announced yet";
    forwarding_->sessions_announced(fix, 2, pubsub_itc_fw::ConnectionID{42}, 0);
    EXPECT_EQ(forwarding_->held_for_sessions(), 0U);
    EXPECT_EQ(links_.dropped_unbound, (std::vector<int64_t>{5, 6}));
    EXPECT_EQ(links_.released, (std::vector<std::pair<int64_t, int64_t>>{{0, 2}}));
}

TEST_F(ReportForwardingTest, AReportIsHeldForAnUnannouncedSessionOnlyByALeaderWhileAGatewayHasNotAnnounced) {
    links_.is_leading = false;
    EXPECT_FALSE(forwarding_->hold_for_unannounced_session(pending("AWAY", 5, "r5")));
    links_.is_leading = true;
    EXPECT_FALSE(forwarding_->hold_for_unannounced_session(pending("NOSESSION", 5, "r5"))) << "a test expects no session for it";
    both_gateways_announce();
    EXPECT_FALSE(forwarding_->hold_for_unannounced_session(pending("AWAY", 5, "r5")));
}

TEST_F(ReportForwardingTest, ReportsHeldForASessionAreSentWhenItBinds) {
    EXPECT_TRUE(forwarding_->hold_for_unannounced_session(pending("MEMBER", 5, "r5")));
    member_connected();
    forwarding_->send_held_for_session(fix_common::SessionIdentity::make("MEMBER", fix));
    ASSERT_EQ(links_.sent.size(), 1U);
    EXPECT_EQ(links_.sent[0].payload, "r5");
    EXPECT_EQ(links_.forwarded, 1);
    EXPECT_EQ(links_.released, (std::vector<std::pair<int64_t, int64_t>>{{1, 0}}));
}

TEST_F(ReportForwardingTest, AFixLogonLeavesItsHeldReportsToTheReportsItAsksFor) {
    EXPECT_TRUE(forwarding_->hold_for_unannounced_session(pending("MEMBER", 5, "r5")));
    EXPECT_EQ(forwarding_->release_held_for_session(fix_common::SessionIdentity::make("MEMBER", fix)), 1U);
    EXPECT_TRUE(links_.sent.empty());
    EXPECT_EQ(links_.released, (std::vector<std::pair<int64_t, int64_t>>{{0, 1}}));
}

TEST_F(ReportForwardingTest, AReportWaitsForItsRecordAndIsSentWhenItIsCommitted) {
    member_connected();
    forwarding_->wait_for_commit(10, pending("MEMBER", 10, "r10"));
    forwarding_->wait_for_commit(12, pending("MEMBER", 12, "r12"));
    forwarding_->deliver_committed_through(11);
    ASSERT_EQ(links_.sent.size(), 1U);
    EXPECT_EQ(links_.sent[0].payload, "r10");
    EXPECT_EQ(forwarding_->waiting(), 1U);
    forwarding_->deliver_committed_through(12);
    EXPECT_EQ(links_.sent.size(), 2U);
}

TEST_F(ReportForwardingTest, AReportForASessionConnectedNowhereIsCountedAndItsOrderForgotten) {
    both_gateways_announce();
    ReportForwarding::Pending report = pending("AWAY", 5, "r5");
    report.erase_routing_entry = true;
    forwarding_->deliver(report);
    EXPECT_TRUE(links_.sent.empty());
    EXPECT_EQ(links_.dropped_unbound, (std::vector<int64_t>{5}));
    EXPECT_EQ(links_.ended, (std::vector<int64_t>{5}));
}

TEST_F(ReportForwardingTest, HowFarReportsAreForwardedStopsBeforeTheOldestWaitingOrHeld) {
    EXPECT_FALSE(forwarding_->forwarded_through().has_value());
    forwarding_->report_seen(EngineReportPosition{3, 10});
    ASSERT_TRUE(forwarding_->forwarded_through().has_value());
    EXPECT_EQ(forwarding_->forwarded_through()->number, 10);
    forwarding_->wait_for_commit(20, pending("MEMBER", 20, "r", EngineReportPosition{3, 8}));
    EXPECT_EQ(forwarding_->forwarded_through()->number, 7);
    EXPECT_TRUE(forwarding_->hold_for_unannounced_session(pending("AWAY", 21, "r", EngineReportPosition{3, 6})));
    EXPECT_EQ(forwarding_->forwarded_through()->number, 5);
}

TEST_F(ReportForwardingTest, AReportHeldForAGatewayHoldsBackHowFarReportsAreForwardedUntilItIsSent) {
    // BUG-0226: a report held for a gateway with no connection for reports has not been forwarded.
    forwarding_->report_seen(EngineReportPosition{3, 10});
    pubsub_itc_fw_app::WalRecord envelope{};
    forwarding_->send_to_gateway(fix, 1, 5, envelope, false, EngineReportPosition{3, 8});
    forwarding_->send_to_gateway(fix, 1, 6, envelope, false, EngineReportPosition{3, 9});
    ASSERT_EQ(forwarding_->held_for_gateway(fix, 1), 2U);
    EXPECT_EQ(forwarding_->forwarded_through()->number, 7);
    forwarding_->gateway_connected(fix, 1, pubsub_itc_fw::ConnectionID{31});
    EXPECT_EQ(links_.sent.size(), 2U);
    EXPECT_EQ(forwarding_->forwarded_through()->number, 10) << "sent, so forwarded";
}

TEST_F(ReportForwardingTest, AReportSentAgainFromTheWalAndHeldForAGatewayHoldsNothingBack) {
    forwarding_->report_seen(EngineReportPosition{3, 10});
    pubsub_itc_fw_app::WalRecord envelope{};
    forwarding_->send_to_gateway(fix, 1, 0, envelope, false, std::nullopt);
    ASSERT_EQ(forwarding_->held_for_gateway(fix, 1), 1U);
    EXPECT_EQ(forwarding_->forwarded_through()->number, 10);
}

TEST_F(ReportForwardingTest, StoppingLeadingForgetsTheOldestReportHeldForAGateway) {
    forwarding_->report_seen(EngineReportPosition{3, 10});
    pubsub_itc_fw_app::WalRecord envelope{};
    forwarding_->send_to_gateway(fix, 1, 5, envelope, false, EngineReportPosition{3, 8});
    forwarding_->stopped_leading();
    EXPECT_EQ(forwarding_->forwarded_through()->number, 10);
}

TEST_F(ReportForwardingTest, StoppingLeadingDiscardsEveryHeldReport) {
    EXPECT_TRUE(forwarding_->hold_for_unannounced_session(pending("AWAY", 5, "r5")));
    pubsub_itc_fw_app::WalRecord envelope{};
    forwarding_->send_to_gateway(fix, 1, 6, envelope, false, std::nullopt);
    forwarding_->stopped_leading();
    EXPECT_EQ(forwarding_->held_for_sessions(), 0U);
    EXPECT_EQ(forwarding_->held_for_gateway(fix, 1), 0U);
    forwarding_->gateway_connected(fix, 1, pubsub_itc_fw::ConnectionID{31});
    EXPECT_TRUE(links_.sent.empty());
}

TEST_F(ReportForwardingTest, ALostLinkMakesItsGatewayUnannouncedAndALostConnectionForgetsTheGateway) {
    both_gateways_announce();
    EXPECT_FALSE(forwarding_->hold_for_unannounced_session(pending("AWAY", 5, "r5")));
    EXPECT_FALSE(forwarding_->connection_lost(pubsub_itc_fw::ConnectionID{41}).has_value()) << "a link, not a connection for reports";
    EXPECT_TRUE(forwarding_->hold_for_unannounced_session(pending("AWAY", 6, "r6"))) << "the gateway must announce again";
    forwarding_->gateway_connected(fix, 2, pubsub_itc_fw::ConnectionID{32});
    const auto lost = forwarding_->connection_lost(pubsub_itc_fw::ConnectionID{32});
    ASSERT_TRUE(lost.has_value());
    EXPECT_EQ(*lost, ReportForwarding::gateway_key(fix, 2));
}

// ---- The ring of reports waiting for their record to be committed ---------------------------------------------------

namespace {
// A report kept in a slab chunk, as one from an engine waits: the chunk is the bytes of @p chunk, and the report is the
// whole of it.
ReportForwarding::Pending in_chunk(const std::string& comp_id, int64_t seq_no, const std::vector<uint8_t>& chunk, int slot) {
    ReportForwarding::Pending report{};
    report.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
    report.seq_no = seq_no;
    report.keep_in_chunk(pubsub_itc_fw::make_slab_handle(slot, 1), chunk.data(), chunk.data(), chunk.size());
    report.identity = fix_common::SessionIdentity::make(comp_id, gateway_ids::fix_order_gateway);
    report.set_cl_ord_id("ORD-" + std::to_string(seq_no));
    return report;
}
} // namespaces

// A report kept in the chunk it arrived in is sent from the chunk, with its ClOrdID, once its record is committed, and the
// chunk is released then, once; never before.
TEST_F(ReportForwardingTest, AReportKeptInItsChunkIsSentFromItAndTheChunkReleasedOnce) {
    member_connected();
    const std::vector<uint8_t> chunk{'r', '1', '0'};
    forwarding_->wait_for_commit(10, in_chunk("MEMBER", 10, chunk, 3));
    forwarding_->deliver_committed_through(9);
    EXPECT_TRUE(links_.sent.empty());
    EXPECT_TRUE(links_.released_chunks.empty()) << "a chunk is held while its report waits";
    forwarding_->deliver_committed_through(10);
    ASSERT_EQ(links_.sent.size(), 1U);
    EXPECT_EQ(links_.sent[0].payload, "r10");
    ASSERT_EQ(links_.released_chunks.size(), 1U);
    EXPECT_EQ(links_.released_chunks[0].first, pubsub_itc_fw::make_slab_handle(3, 1));
    EXPECT_EQ(links_.released_chunks[0].second, chunk.data());
    forwarding_->deliver_committed_through(20);
    EXPECT_EQ(links_.released_chunks.size(), 1U) << "released once";
}

// Reports are released from the front, in the order they arrived. One that arrived out of order, waiting for an earlier
// record than the one before it, waits behind it: delayed, and never sent before its own record is committed.
TEST_F(ReportForwardingTest, WaitingReportsAreReleasedInOrderAndOneOutOfOrderWaitsBehindTheOneBeforeIt) {
    member_connected();
    forwarding_->wait_for_commit(10, pending("MEMBER", 10, "r10"));
    forwarding_->wait_for_commit(14, pending("MEMBER", 14, "r14"));
    forwarding_->wait_for_commit(12, pending("MEMBER", 12, "r12"));
    forwarding_->deliver_committed_through(12);
    ASSERT_EQ(links_.sent.size(), 1U) << "r12's record is committed, but it waits behind r14";
    EXPECT_EQ(links_.sent[0].payload, "r10");
    forwarding_->deliver_committed_through(14);
    ASSERT_EQ(links_.sent.size(), 3U);
    EXPECT_EQ(links_.sent[1].payload, "r14");
    EXPECT_EQ(links_.sent[2].payload, "r12");
}

// The places left fall as reports wait and rise as they are sent; a ring that fills keeps every report beyond it, in
// order, and sends them all.
TEST_F(ReportForwardingTest, AFullRingLosesNothingAndKeepsTheOrder) {
    member_connected();
    EXPECT_EQ(forwarding_->waiting_places_left(), ReportForwarding::waiting_capacity);
    const int64_t total = static_cast<int64_t>(ReportForwarding::waiting_capacity) + 3;
    for (int64_t seq = 1; seq <= total; ++seq) {
        forwarding_->wait_for_commit(seq, pending("MEMBER", seq, "r" + std::to_string(seq)));
    }
    EXPECT_EQ(forwarding_->waiting_places_left(), 0U);
    EXPECT_EQ(forwarding_->waiting(), static_cast<size_t>(total));
    forwarding_->deliver_committed_through(total);
    ASSERT_EQ(links_.sent.size(), static_cast<size_t>(total));
    EXPECT_EQ(links_.sent.back().payload, "r" + std::to_string(total));
    EXPECT_EQ(links_.sent[static_cast<size_t>(total) - 4].payload, "r" + std::to_string(total - 3)) << "in the order they arrived";
    EXPECT_EQ(forwarding_->waiting(), 0U);
    EXPECT_EQ(forwarding_->waiting_places_left(), ReportForwarding::waiting_capacity);
}

} // namespaces
