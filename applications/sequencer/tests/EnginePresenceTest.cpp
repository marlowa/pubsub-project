// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

#include "EnginePresence.hpp"

namespace sequencer::tests {

namespace {

using EngineForOrders = EnginePresence::EngineForOrders;
using Reason = pubsub_itc_fw_app::OrderAcceptanceReason;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr seconds orders_held_at_most{30};
constexpr seconds recovery_limit{10};
constexpr int64_t one_second_ns = 1000000000;

// What EnginePresence reads from the thread, and what it asks the thread to do.
struct RecordingLinks : public EnginePresenceLinksInterface {
    [[nodiscard]] bool leading() const override {
        return is_leading;
    }
    [[nodiscard]] bool an_engine_takes_orders() const override {
        return engine_takes_orders;
    }
    [[nodiscard]] bool an_engine_is_routed_to() const override {
        return engine_routed;
    }
    [[nodiscard]] bool wal_holds_records() const override {
        return wal_has_records;
    }
    [[nodiscard]] bool trading_halted() const override {
        return halted;
    }
    [[nodiscard]] bool halted_for_a_recovery_too_long() const override {
        return halted_for_recovery;
    }
    void tell_every_gateway() override {
        ++gateways_told;
    }
    void engine_takes_orders_again(int64_t without_ms, int64_t orders_held, bool one_had_before) override {
        again.push_back(Again{without_ms, orders_held, one_had_before});
    }
    void write_recovery_halt(int64_t now_ns) override {
        halts_written.push_back(now_ns);
    }

    struct Again {
        int64_t without_ms;
        int64_t orders_held;
        bool one_had_before;
    };

    bool is_leading{true};
    bool engine_takes_orders{false};
    bool engine_routed{false};
    bool wal_has_records{true};
    bool halted{false};
    bool halted_for_recovery{false};
    int gateways_told{0};
    std::vector<Again> again;
    std::vector<int64_t> halts_written;
};

class EnginePresenceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        scratch_ = pubsub_itc_fw::tests_common::make_scratch_directory("engine_presence");
        presence_ = make_presence();
    }
    void TearDown() override {
        presence_.reset();
        pubsub_itc_fw::tests_common::remove_scratch_directory(scratch_);
    }

    std::unique_ptr<EnginePresence> make_presence() {
        return std::make_unique<EnginePresence>(orders_held_at_most, recovery_limit, links_, logger_);
    }

    void note(EnginePresence::Clock::duration after) {
        presence_->note_whether_an_engine_takes_orders(start_ + after);
    }

    bool logged(const std::string& text) {
        logger_.flush();
        for (const std::string& line : log_) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::string scratch_;
    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<EnginePresence> presence_;
    const EnginePresence::Clock::time_point start_ = EnginePresence::Clock::now();
    // A wall time well after the epoch, as the WAL records them.
    const int64_t wall_start_ns_ = 1'800'000'000 * one_second_ns;
};

} // namespaces

// ---- Whether an engine takes orders ---------------------------------------------------------------------------------

TEST_F(EnginePresenceTest, ATimeWithoutAnEngineBeforeAnyHasTakenOrdersIsNoEngineYet) {
    note(milliseconds{0});
    EXPECT_EQ(presence_->engine_for_orders(), EngineForOrders::no_engine_yet);
    EXPECT_TRUE(logged("no matching engine takes orders"));
    EXPECT_EQ(presence_->time_without_an_engine(start_ + seconds{3}), seconds{3});
}

TEST_F(EnginePresenceTest, AnEngineTakingOrdersAgainIsReportedWithTheTimeAndTheOrdersHeld) {
    note(milliseconds{0});
    presence_->order_held();
    presence_->order_held();
    EXPECT_EQ(presence_->orders_held_without_an_engine(), 2);
    links_.engine_takes_orders = true;
    note(milliseconds{1500});
    EXPECT_EQ(presence_->engine_for_orders(), EngineForOrders::an_engine_takes_orders);
    ASSERT_EQ(links_.again.size(), 1U);
    EXPECT_EQ(links_.again[0].without_ms, 1500);
    EXPECT_EQ(links_.again[0].orders_held, 2);
    EXPECT_FALSE(links_.again[0].one_had_before) << "no engine had taken orders in this leadership";
    EXPECT_EQ(presence_->orders_held_without_an_engine(), 0);
    EXPECT_EQ(presence_->time_without_an_engine(start_ + seconds{5}), EnginePresence::Clock::duration::zero());
}

TEST_F(EnginePresenceTest, ATimeWithoutAnEngineAfterOneTookOrdersIsToldApart) {
    links_.engine_takes_orders = true;
    note(milliseconds{0});
    EXPECT_TRUE(links_.again.empty()) << "an engine taking orders from the start is the usual case, and says nothing";
    links_.engine_takes_orders = false;
    note(milliseconds{10});
    EXPECT_EQ(presence_->engine_for_orders(), EngineForOrders::no_engine_since_one_did);
    links_.engine_takes_orders = true;
    note(milliseconds{20});
    ASSERT_EQ(links_.again.size(), 1U);
    EXPECT_TRUE(links_.again[0].one_had_before);
}

TEST_F(EnginePresenceTest, NothingIsNotedWhileThisInstanceDoesNotLead) {
    links_.is_leading = false;
    note(milliseconds{0});
    EXPECT_EQ(presence_->engine_for_orders(), EngineForOrders::not_noted);
    EXPECT_FALSE(presence_->refresh_order_acceptance(start_ + seconds{60}));
    EXPECT_EQ(presence_->acceptance(), Reason::Accepting) << "a follower always says the venue accepts";
    links_.halted = true;
    EXPECT_FALSE(presence_->refresh_order_acceptance(start_));
    EXPECT_EQ(presence_->acceptance(), Reason::Accepting) << "even while trading is halted";
}

TEST_F(EnginePresenceTest, TheWarningIsRepeatedEveryFiveSecondsAndTheGatewaysToldAgainWhileRefusing) {
    note(milliseconds{0});
    log_.clear();
    note(seconds{4});
    EXPECT_FALSE(logged("still no matching engine")) << "not yet five seconds since the warning";
    note(seconds{5});
    EXPECT_TRUE(logged("still no matching engine"));
    EXPECT_EQ(links_.gateways_told, 0) << "still accepting, so nothing to repeat";
    note(orders_held_at_most);
    EXPECT_EQ(presence_->acceptance(), Reason::NoMatchingEngine);
    EXPECT_EQ(links_.gateways_told, 1) << "told of the change once";
    note(orders_held_at_most + seconds{5});
    EXPECT_EQ(links_.gateways_told, 2) << "told again with the warning, for a gateway that missed it";
}

// ---- What the gateways are told -------------------------------------------------------------------------------------

TEST_F(EnginePresenceTest, TheGatewaysAreToldToRefuseOnceNoEngineHasTakenOrdersForTheLongestOrdersAreHeld) {
    note(milliseconds{0});
    EXPECT_FALSE(presence_->refresh_order_acceptance(start_ + orders_held_at_most - milliseconds{1}));
    EXPECT_TRUE(presence_->refresh_order_acceptance(start_ + orders_held_at_most));
    EXPECT_EQ(presence_->acceptance(), Reason::NoMatchingEngine);
    EXPECT_EQ(links_.gateways_told, 1);
    links_.engine_takes_orders = true;
    note(orders_held_at_most + seconds{1});
    EXPECT_EQ(presence_->acceptance(), Reason::Accepting) << "accepting again without a person";
    EXPECT_EQ(links_.gateways_told, 2);
}

TEST_F(EnginePresenceTest, WhileHaltedTheGatewaysAreToldWhy) {
    links_.engine_takes_orders = true;
    note(milliseconds{0});
    links_.halted = true;
    presence_->refresh_order_acceptance(start_);
    EXPECT_EQ(presence_->acceptance(), Reason::Halted);

    links_.halted_for_recovery = true;
    presence_->refresh_order_acceptance(start_);
    EXPECT_EQ(presence_->acceptance(), Reason::Halted) << "halted for a recovery that took too long, but an engine trades now";

    links_.engine_takes_orders = false;
    note(milliseconds{10});
    presence_->refresh_order_acceptance(start_ + milliseconds{10});
    EXPECT_EQ(presence_->acceptance(), Reason::RecoveryTooLong);
    presence_->refresh_order_acceptance(start_ + orders_held_at_most + seconds{1});
    EXPECT_EQ(presence_->acceptance(), Reason::RecoveryTooLong) << "the recovery reason wins over the time without an engine";

    links_.halted_for_recovery = false;
    presence_->refresh_order_acceptance(start_ + orders_held_at_most + seconds{1});
    EXPECT_EQ(presence_->acceptance(), Reason::NoMatchingEngine);
}

TEST_F(EnginePresenceTest, StoppingLeadingForgetsTheTimeWithoutAnEngine) {
    note(milliseconds{0});
    presence_->order_held();
    presence_->refresh_order_acceptance(start_ + orders_held_at_most);
    ASSERT_EQ(presence_->acceptance(), Reason::NoMatchingEngine);
    presence_->stopped_leading(wall_start_ns_);
    EXPECT_EQ(presence_->engine_for_orders(), EngineForOrders::not_noted);
    EXPECT_EQ(presence_->acceptance(), Reason::Accepting);
    EXPECT_EQ(presence_->orders_held_without_an_engine(), 0);
    note(orders_held_at_most + seconds{1});
    EXPECT_EQ(presence_->engine_for_orders(), EngineForOrders::no_engine_yet) << "leading again, a new time without an engine begins";
    EXPECT_EQ(presence_->acceptance(), Reason::Accepting);
}

// ---- The limit on the recovery the venue does by itself -------------------------------------------------------------

TEST_F(EnginePresenceTest, TradingIsHaltedOnceWhenNoEngineIsRoutedToForLongerThanTheLimit) {
    presence_->started(scratch_, 0, wall_start_ns_);
    presence_->check_absence(wall_start_ns_, start_);
    EXPECT_EQ(presence_->absence_began_ns(), wall_start_ns_);
    presence_->check_absence(wall_start_ns_ + recovery_limit.count() * one_second_ns, start_);
    EXPECT_TRUE(links_.halts_written.empty()) << "not yet longer than the limit";
    presence_->check_absence(wall_start_ns_ + (recovery_limit.count() + 1) * one_second_ns, start_);
    EXPECT_EQ(links_.halts_written.size(), 1U);
    EXPECT_TRUE(logged("no matching engine has been able to trade"));
    presence_->check_absence(wall_start_ns_ + (recovery_limit.count() + 2) * one_second_ns, start_);
    EXPECT_EQ(links_.halts_written.size(), 1U) << "written once for each absence";
    links_.engine_routed = true;
    presence_->check_absence(wall_start_ns_ + (recovery_limit.count() + 3) * one_second_ns, start_);
    EXPECT_EQ(presence_->absence_began_ns(), 0);
}

TEST_F(EnginePresenceTest, AnInstanceThatDoesNotLeadDoesNotHalt) {
    links_.is_leading = false;
    presence_->started(scratch_, 0, wall_start_ns_);
    presence_->check_absence(wall_start_ns_, start_);
    presence_->check_absence(wall_start_ns_ + (recovery_limit.count() + 1) * one_second_ns, start_);
    EXPECT_TRUE(links_.halts_written.empty());
}

TEST_F(EnginePresenceTest, ACatchUpThatWillTakeTooLongHaltsBeforeTheLimitAndItsEndStopsTheEstimate) {
    presence_->started(scratch_, 0, wall_start_ns_);
    presence_->check_absence(wall_start_ns_, start_);
    const pubsub_itc_fw::ConnectionID engine{71};
    presence_->catch_up_progress(engine, RecoveryLimitRule::CatchUpProgress{0, 1000});
    presence_->check_absence(wall_start_ns_ + one_second_ns, start_);
    presence_->catch_up_progress(engine, RecoveryLimitRule::CatchUpProgress{10, 1000});
    presence_->check_absence(wall_start_ns_ + 2 * one_second_ns, start_);
    EXPECT_EQ(links_.halts_written.size(), 1U) << "990 records to go at ten a second is far longer than the limit";
}

TEST_F(EnginePresenceTest, TheEndOfACatchUpOnAnotherConnectionLeavesItsProgressInPlace) {
    presence_->started(scratch_, 0, wall_start_ns_);
    presence_->check_absence(wall_start_ns_, start_);
    const pubsub_itc_fw::ConnectionID engine{71};
    presence_->catch_up_progress(engine, RecoveryLimitRule::CatchUpProgress{0, 1000});
    presence_->check_absence(wall_start_ns_ + one_second_ns, start_);
    presence_->catch_up_progress(engine, RecoveryLimitRule::CatchUpProgress{10, 1000});
    presence_->catch_up_ended(pubsub_itc_fw::ConnectionID{72});
    presence_->check_absence(wall_start_ns_ + 2 * one_second_ns, start_);
    EXPECT_EQ(links_.halts_written.size(), 1U);
}

TEST_F(EnginePresenceTest, TheEndOfTheCatchUpBeingMeasuredStopsTheEstimate) {
    presence_->started(scratch_, 0, wall_start_ns_);
    presence_->check_absence(wall_start_ns_, start_);
    const pubsub_itc_fw::ConnectionID engine{71};
    presence_->catch_up_progress(engine, RecoveryLimitRule::CatchUpProgress{0, 1000});
    presence_->check_absence(wall_start_ns_ + one_second_ns, start_);
    presence_->catch_up_progress(engine, RecoveryLimitRule::CatchUpProgress{10, 1000});
    presence_->catch_up_ended(engine);
    presence_->check_absence(wall_start_ns_ + 2 * one_second_ns, start_);
    EXPECT_TRUE(links_.halts_written.empty()) << "with no catch-up under way, the time so far alone decides";
}

TEST_F(EnginePresenceTest, AfterTheWholeVenueWasDownTheAbsenceIsMeasuredFromTheLastEngineRoutedTo) {
    presence_->started(scratch_, 0, wall_start_ns_);
    links_.engine_routed = true;
    const int64_t routed_at_ns = wall_start_ns_ + 5 * one_second_ns;
    presence_->check_absence(routed_at_ns, start_);
    presence_.reset();

    presence_ = make_presence();
    presence_->started(scratch_, wall_start_ns_, routed_at_ns + 60 * one_second_ns);
    EXPECT_EQ(presence_->absence_began_ns(), routed_at_ns) << "the time kept beside the WAL, later than the first record";
    EXPECT_TRUE(logged("this process last knew of a matching engine"));

    presence_->stopped_leading(routed_at_ns + 61 * one_second_ns);
    EXPECT_EQ(presence_->absence_began_ns(), routed_at_ns + 61 * one_second_ns) << "another instance leads, so the venue was not down as a whole";
}

TEST_F(EnginePresenceTest, AnEmptyWalHasNothingToRecover) {
    presence_->started(scratch_, 0, wall_start_ns_);
    EXPECT_EQ(presence_->absence_began_ns(), 0);
}

// ---- The engine that said it leads ----------------------------------------------------------------------------------

TEST_F(EnginePresenceTest, TheEngineThatSaidItLeadsIsForgottenWhenItsConnectionIsLostOrItSaysItFollows) {
    const pubsub_itc_fw::ConnectionID announced_on{81};
    presence_->engine_announced(2, true, announced_on);
    EXPECT_EQ(presence_->leading_engine_instance(), 2);
    EXPECT_TRUE(presence_->another_engine_leads(1));
    EXPECT_FALSE(presence_->another_engine_leads(2));
    EXPECT_FALSE(presence_->connection_lost(pubsub_itc_fw::ConnectionID{82}).has_value());
    EXPECT_EQ(presence_->connection_lost(announced_on), std::optional<int64_t>{2});
    EXPECT_FALSE(presence_->another_engine_leads(1));

    presence_->engine_announced(2, true, announced_on);
    presence_->engine_announced(1, false, pubsub_itc_fw::ConnectionID{83});
    EXPECT_EQ(presence_->leading_engine_instance(), 2) << "the other instance saying it follows changes nothing";
    presence_->engine_announced(2, false, announced_on);
    EXPECT_EQ(presence_->leading_engine_instance(), 0);
}

} // namespaces
