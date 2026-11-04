// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "GatewayIds.hpp"
#include "MemberSessions.hpp"

namespace sequencer::tests {

namespace {

// What MemberSessions asks the thread to do.
struct RecordingLinks : public MemberSessionsLinksInterface {
    [[nodiscard]] bool expects_no_session_for(std::string_view comp_id) const override {
        return comp_id == "NOSESSION";
    }
    void reports_dropped_unbound(int64_t total) override {
        gauge = total;
    }
    void report_dropped(const fix_common::SessionIdentity&, int64_t er_seq_no) override {
        dropped_events.push_back(er_seq_no);
    }
    void session_announced_again(const fix_common::SessionIdentity&, const fix_common::SessionDestination&) override {
        ++announced_again;
    }
    void session_unbound(const fix_common::SessionIdentity&, int16_t, int32_t) override {
        ++unbound_events;
    }
    int64_t gauge{0};
    std::vector<int64_t> dropped_events;
    int announced_again{0};
    int unbound_events{0};
};

class MemberSessionsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        sessions_ = std::make_unique<MemberSessions>(links_, logger_);
    }

    static fix_common::SessionDestination at(int16_t instance, int32_t connection) {
        fix_common::SessionDestination destination{};
        destination.instance = instance;
        destination.conn_id = connection;
        return destination;
    }

    static MemberSessions::Reported reported(int32_t outbound, int32_t inbound, int64_t last_delivered = 0) {
        MemberSessions::Reported numbers{};
        numbers.outbound_seq_num = outbound;
        numbers.inbound_seq_num = inbound;
        numbers.has_last_report_delivered = last_delivered != 0;
        numbers.last_report_delivered = last_delivered;
        return numbers;
    }

    // Logs on, as the leader sees it, and returns what is handed back.
    std::optional<MemberSessions::Resume> log_on(const fix_common::SessionDestination& destination, int64_t wal_last = 100, bool reset = false) {
        const MemberSessions::Bound bound = sessions_->bind(member_, destination, false);
        return sessions_->resume(member_, reset, bound.previous_session_died, true, wal_last);
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<MemberSessions> sessions_;
    const fix_common::SessionIdentity member_ = fix_common::SessionIdentity::make("MEMBER", gateway_ids::fix_order_gateway);
};

} // namespaces

TEST_F(MemberSessionsTest, ANewSessionIsBoundAndStartsItsNumberingAtOne) {
    const auto resumed = log_on(at(1, 11));
    ASSERT_TRUE(resumed.has_value());
    EXPECT_FALSE(resumed->known);
    EXPECT_EQ(resumed->outbound_seq_num, 1);
    ASSERT_NE(sessions_->destination(member_), nullptr);
    EXPECT_EQ(sessions_->destination(member_)->conn_id, 11);
}

TEST_F(MemberSessionsTest, OnlyTheLeaderResumesASession) {
    const MemberSessions::Bound bound = sessions_->bind(member_, at(1, 11), false);
    EXPECT_FALSE(sessions_->resume(member_, false, bound.previous_session_died, false, 100).has_value());
    EXPECT_NE(sessions_->destination(member_), nullptr) << "a follower keeps the binding too";
}

TEST_F(MemberSessionsTest, AfterACleanUnbindTheReportedNumbersAreHandedBackExactly) {
    static_cast<void>(log_on(at(1, 11)));
    sessions_->report_forwarded(member_);
    sessions_->unbind(member_, 1, 11, reported(40, 25));
    EXPECT_EQ(sessions_->destination(member_), nullptr);
    EXPECT_EQ(links_.unbound_events, 1);
    const auto resumed = log_on(at(2, 22));
    ASSERT_TRUE(resumed.has_value());
    EXPECT_TRUE(resumed->known);
    EXPECT_EQ(resumed->outbound_seq_num, 40);
    EXPECT_EQ(resumed->inbound_seq_num, 25);
}

TEST_F(MemberSessionsTest, AfterAGatewayDiedTheOutboundNumberIsRaisedAndTheInboundIsNot) {
    static_cast<void>(log_on(at(1, 11)));
    sessions_->sequence_update(member_, reported(40, 25));
    sessions_->report_forwarded(member_);
    sessions_->report_forwarded(member_);
    const auto resumed = log_on(at(2, 22)); // bound still: the first gateway died without unbinding
    ASSERT_TRUE(resumed.has_value());
    EXPECT_EQ(resumed->outbound_seq_num, 40 + 2 + MemberSessions::unclean_resume_admin_allowance);
    EXPECT_EQ(resumed->inbound_seq_num, 25);
    EXPECT_TRUE(sessions_->sequence_state(member_)->delivered_figure_may_be_behind);
    EXPECT_EQ(sessions_->sequence_state(member_)->ers_since_report, 0) << "spent by the resume";
}

TEST_F(MemberSessionsTest, AStaleUnbindDoesNotUnbindANewerBinding) {
    static_cast<void>(log_on(at(1, 11)));
    static_cast<void>(log_on(at(2, 22)));
    sessions_->unbind(member_, 1, 11, reported(40, 25));
    ASSERT_NE(sessions_->destination(member_), nullptr);
    EXPECT_EQ(sessions_->destination(member_)->conn_id, 22);
    EXPECT_EQ(links_.unbound_events, 0);
}

TEST_F(MemberSessionsTest, SequenceUpdatesNeverLowerTheNumbers) {
    sessions_->sequence_update(member_, reported(40, 25, 500));
    sessions_->sequence_update(member_, reported(30, 20, 400));
    const MemberSessions::SequenceState* state = sessions_->sequence_state(member_);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->outbound_seq_num, 40);
    EXPECT_EQ(state->inbound_seq_num, 25);
    EXPECT_EQ(state->last_report_delivered, 500);
}

TEST_F(MemberSessionsTest, ALogonThatResetsDiscardsWhatIsRemembered) {
    sessions_->sequence_update(member_, reported(40, 25));
    const auto resumed = log_on(at(1, 11), 100, true);
    ASSERT_TRUE(resumed.has_value());
    EXPECT_FALSE(resumed->known);
    EXPECT_EQ(resumed->outbound_seq_num, 1);
}

TEST_F(MemberSessionsTest, ASessionAnnouncedAgainIsRecordedWithoutBeingResumed) {
    const MemberSessions::Bound bound = sessions_->bind(member_, at(1, 11), true);
    EXPECT_TRUE(bound.still_connected);
    EXPECT_EQ(links_.announced_again, 1);
    EXPECT_NE(sessions_->destination(member_), nullptr);
    const MemberSessions::Bound later = sessions_->bind(member_, at(2, 22), false);
    EXPECT_TRUE(later.previous_session_died) << "a binding was present at the logon";
}

TEST_F(MemberSessionsTest, TheReportsMissedWhileAwayAreAskedForOncePerBinding) {
    static_cast<void>(log_on(at(1, 11)));
    sessions_->unbind(member_, 1, 11, reported(40, 25, 500));
    static_cast<void>(log_on(at(2, 22), 900));
    const auto undelivered = sessions_->take_undelivered(member_);
    ASSERT_TRUE(undelivered.has_value());
    EXPECT_EQ(undelivered->after, 500);
    EXPECT_EQ(undelivered->through, 900);
    EXPECT_FALSE(undelivered->possible_repeat);
    EXPECT_EQ(undelivered->destination.conn_id, 22);
    EXPECT_FALSE(sessions_->take_undelivered(member_).has_value()) << "once for each binding";
}

TEST_F(MemberSessionsTest, NothingIsOwedWhenNothingWasLoggedWhileAway) {
    static_cast<void>(log_on(at(1, 11)));
    sessions_->unbind(member_, 1, 11, reported(40, 25, 900));
    static_cast<void>(log_on(at(2, 22), 900));
    EXPECT_FALSE(sessions_->take_undelivered(member_).has_value());
}

TEST_F(MemberSessionsTest, ADroppedReportIsSaidOnceUntilTheSessionIsConnectedAgain) {
    sessions_->report_dropped_unbound(member_, 7);
    sessions_->report_dropped_unbound(member_, 8);
    EXPECT_EQ(links_.dropped_events, (std::vector<int64_t>{7}));
    EXPECT_EQ(links_.gauge, 2);
    static_cast<void>(sessions_->bind(member_, at(1, 11), false));
    sessions_->report_dropped_unbound(member_, 9);
    EXPECT_EQ(links_.dropped_events, (std::vector<int64_t>{7, 9}));
}

TEST_F(MemberSessionsTest, AReportForASessionATestExpectsNotToHaveIsCountedButNotSaid) {
    sessions_->report_dropped_unbound(fix_common::SessionIdentity::make("NOSESSION", gateway_ids::fix_order_gateway), 7);
    EXPECT_EQ(links_.gauge, 1);
    EXPECT_TRUE(links_.dropped_events.empty());
}

TEST_F(MemberSessionsTest, TheSessionThatPlacedAnOrderIsKeptUntilAReportEndsIt) {
    MemberSessions::OriginSession origin{};
    origin.identity = member_;
    origin.has_ingress_ns = true;
    origin.gateway_ingress_ns = 1234;
    sessions_->order_placed(10, origin);
    ASSERT_NE(sessions_->origin(10), nullptr);
    EXPECT_EQ(sessions_->origin(10)->gateway_ingress_ns, 1234);
    sessions_->order_ended(10);
    EXPECT_EQ(sessions_->origin(10), nullptr);
}

} // namespaces
