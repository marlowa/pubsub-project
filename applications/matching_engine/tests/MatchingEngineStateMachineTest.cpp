// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file MatchingEngineStateMachineTest.cpp
 * @brief Unit tests for matching_engine::MatchingEngineStateMachine.
 *
 * The state machine is the matching engine's handling of commands with nothing else attached: no
 * connections, no clock, no timers. These tests give it commands directly and check the reports it
 * produces and what the book holds afterwards, which is what the engine's high availability tests can
 * only check through a running venue. Every report is copied as it is produced, because its string
 * fields point into buffers that end with the call.
 */

#include <MatchingEngineStateMachine.hpp>

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace matching_engine::tests {

namespace {

// A report as the state machine produced it, with its strings copied out.
struct RecordedReport {
    std::string order_id;
    std::string exec_id;
    pubsub_itc_fw_app::ExecType exec_type{};
    pubsub_itc_fw_app::OrdStatus ord_status{};
    std::string cl_ord_id;
    std::string orig_cl_ord_id;
    std::string symbol;
    std::string order_qty;
    std::string leaves_qty;
    bool has_ord_rej_reason{false};
    pubsub_itc_fw_app::OrdRejReason ord_rej_reason{};
    std::string text;
    int64_t transact_time{0};
    int64_t sequence_number{0};
    std::string comp_id;
    ReportIsRepeat repeat{ReportIsRepeat::no};
};

struct RecordedOutcome {
    std::string cl_ord_id;
    pubsub_itc_fw_app::OrderOutcomeKind outcome{};
    ReportIsRepeat repeat{ReportIsRepeat::no};
    int64_t sequence_number{0};
};

// Records everything the state machine hands to its outputs.
class RecordingOutputs : public MatchingEngineOutputsInterface {
  public:
    void send_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t sequence_number, const fix_common::SessionIdentity& session,
                     ReportIsRepeat repeat) override {
        RecordedReport recorded;
        recorded.order_id = std::string(report.order_id);
        recorded.exec_id = std::string(report.exec_id);
        recorded.exec_type = report.exec_type;
        recorded.ord_status = report.ord_status;
        recorded.cl_ord_id = std::string(report.cl_ord_id);
        recorded.orig_cl_ord_id = report.has_orig_cl_ord_id ? std::string(report.orig_cl_ord_id) : std::string();
        recorded.symbol = std::string(report.symbol);
        recorded.order_qty = report.has_order_qty ? std::string(report.order_qty) : std::string();
        recorded.leaves_qty = std::string(report.leaves_qty);
        recorded.has_ord_rej_reason = report.has_ord_rej_reason;
        recorded.ord_rej_reason = report.ord_rej_reason;
        recorded.text = report.has_text ? std::string(report.text) : std::string();
        recorded.transact_time = report.transact_time;
        recorded.sequence_number = sequence_number;
        recorded.comp_id = std::string(session.comp_id_view());
        recorded.repeat = repeat;
        reports.push_back(recorded);
    }

    void order_added_to_book(int64_t sequence_number, const fix_common::SessionIdentity& /*session*/, std::string_view cl_ord_id,
                             const OrderEntry& /*entry*/) override {
        added.push_back(std::string(cl_ord_id) + "@" + std::to_string(sequence_number));
    }

    void order_removed_from_book(int64_t sequence_number, const fix_common::SessionIdentity& /*session*/, std::string_view cl_ord_id) override {
        removed.push_back(std::string(cl_ord_id) + "@" + std::to_string(sequence_number));
    }

    void order_outcome(const fix_common::SessionIdentity& /*session*/, std::string_view cl_ord_id, pubsub_itc_fw_app::OrderOutcomeKind outcome,
                       ReportIsRepeat repeat, int64_t sequence_number, std::string_view /*orig_cl_ord_id*/) override {
        outcomes.push_back(RecordedOutcome{std::string(cl_ord_id), outcome, repeat, sequence_number});
    }

    void order_accepted() override {
        ++accepted;
    }

    std::vector<RecordedReport> reports;
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<RecordedOutcome> outcomes;
    int accepted{0};
};

constexpr OrderBook::SlotIndex region_capacity = 4;
constexpr size_t map_capacity = 16;
constexpr int64_t taken_at = 1'700'000'000'000'000'000;

const fix_common::SessionIdentity member = fix_common::SessionIdentity::make("MEMBER-A", 1);
const fix_common::SessionIdentity other_member = fix_common::SessionIdentity::make("MEMBER-B", 1);

pubsub_itc_fw_app::NewOrderSingleView new_order(std::string_view cl_ord_id, std::string_view quantity = "100") {
    pubsub_itc_fw_app::NewOrderSingleView view{};
    view.cl_ord_id = cl_ord_id;
    view.side = pubsub_itc_fw_app::Side::Buy;
    view.symbol = "VOD.L";
    view.ord_type = pubsub_itc_fw_app::OrdType::Limit;
    view.order_qty = quantity;
    view.has_price = true;
    view.price = "101.25";
    return view;
}

pubsub_itc_fw_app::OrderCancelRequestView cancel_of(std::string_view orig_cl_ord_id, std::string_view cl_ord_id) {
    pubsub_itc_fw_app::OrderCancelRequestView view{};
    view.cl_ord_id = cl_ord_id;
    view.orig_cl_ord_id = orig_cl_ord_id;
    view.side = pubsub_itc_fw_app::Side::Buy;
    view.symbol = "VOD.L";
    return view;
}

class MatchingEngineStateMachineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Under the working directory rather than a machine-wide location, as the book's own tests do.
        char tmpl[] = "state_machine_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl), nullptr);
        dir_ = tmpl;
        book_.open(dir_ + "/open_orders.region", region_capacity, map_capacity);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::string dir_;
    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& record) { log_.push_back(record); }};
    OrderBook book_;
    RecordingOutputs outputs_;
    MatchingEngineStateMachine machine_{book_, outputs_, logger_};
};

// ---- live new orders --------------------------------------------------------------------

TEST_F(MatchingEngineStateMachineTest, AcceptsANewOrderOntoTheBookAndAcknowledgesIt) {
    machine_.apply_new_order(new_order("ORD-1"), 5, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    const RecordedReport& report = outputs_.reports[0];
    EXPECT_EQ(report.exec_type, pubsub_itc_fw_app::ExecType::New);
    EXPECT_EQ(report.ord_status, pubsub_itc_fw_app::OrdStatus::New);
    EXPECT_EQ(report.order_id, "ME-ORD-1");
    EXPECT_EQ(report.exec_id, "ME-EXEC-1");
    EXPECT_EQ(report.cl_ord_id, "ORD-1");
    EXPECT_EQ(report.leaves_qty, "100");
    EXPECT_EQ(report.transact_time, taken_at) << "the report carries the time it was given, not a clock of its own";
    EXPECT_EQ(report.sequence_number, 5);
    EXPECT_EQ(report.comp_id, "MEMBER-A");
    EXPECT_EQ(report.repeat, ReportIsRepeat::no);

    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
    EXPECT_EQ(book_.published(), 5) << "the book is published to the order once its report has been sent";
    EXPECT_EQ(outputs_.accepted, 1);
    EXPECT_EQ(outputs_.added, std::vector<std::string>{"ORD-1@5"});
    ASSERT_EQ(outputs_.outcomes.size(), 1U);
    EXPECT_EQ(outputs_.outcomes[0].outcome, pubsub_itc_fw_app::OrderOutcomeKind::Accepted);
    EXPECT_EQ(machine_.highest_applied(), 5);
}

TEST_F(MatchingEngineStateMachineTest, NumbersOrdersAndReportsInTheOrderTheyArrive) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-2"), 2, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].order_id, "ME-ORD-2");
    EXPECT_EQ(outputs_.reports[1].exec_id, "ME-EXEC-2");
}

TEST_F(MatchingEngineStateMachineTest, RefusesASecondOrderWithTheSameClOrdIdFromTheSameSession) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-1"), 2, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    const RecordedReport& refusal = outputs_.reports[1];
    EXPECT_EQ(refusal.exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(refusal.order_id, "NONE");
    EXPECT_TRUE(refusal.has_ord_rej_reason);
    EXPECT_EQ(refusal.ord_rej_reason, pubsub_itc_fw_app::OrdRejReason::DuplicateOrder);
    EXPECT_EQ(outputs_.accepted, 1);
    EXPECT_EQ(outputs_.outcomes.back().outcome, pubsub_itc_fw_app::OrderOutcomeKind::RefusedDuplicate);
}

TEST_F(MatchingEngineStateMachineTest, TheSameClOrdIdFromAnotherSessionIsAnotherOrder) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-1"), 2, taken_at, other_member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].exec_type, pubsub_itc_fw_app::ExecType::New);
    EXPECT_EQ(book_.size(), 2U);
}

TEST_F(MatchingEngineStateMachineTest, RefusesAnOrderWhoseSequenceNumberDoesNotGoForward) {
    machine_.apply_new_order(new_order("ORD-1"), 7, taken_at, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-2"), 7, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[1].text, "Venue could not process the order: it arrived out of sequence");
    EXPECT_FALSE(book_.contains(OrderKey::make(member, "ORD-2")));
}

TEST_F(MatchingEngineStateMachineTest, RefusesALiveOrderAtOrBelowThePositionACatchUpReached) {
    machine_.note_caught_up_to(10);
    machine_.apply_new_order(new_order("ORD-1"), 10, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(book_.size(), 0U);
}

TEST_F(MatchingEngineStateMachineTest, RefusesAClOrdIdLongerThanTheBookHolds) {
    const std::string too_long(fix_order_limits::max_cl_ord_id_length + 1, 'X');
    machine_.apply_new_order(new_order(too_long), 1, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[0].text, "ClOrdID exceeds maximum length of " + std::to_string(fix_order_limits::max_cl_ord_id_length));
    EXPECT_EQ(book_.size(), 0U);
    EXPECT_EQ(outputs_.outcomes.back().outcome, pubsub_itc_fw_app::OrderOutcomeKind::RefusedTooLong);
}

TEST_F(MatchingEngineStateMachineTest, RefusesEveryNewOrderWhileHalted) {
    machine_.halt();
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[0].ord_rej_reason, pubsub_itc_fw_app::OrdRejReason::ExchangeClosed);
    EXPECT_EQ(outputs_.reports[0].text, "trading is halted");
    EXPECT_EQ(book_.size(), 0U);
    EXPECT_TRUE(machine_.halted());
}

TEST_F(MatchingEngineStateMachineTest, RefusesAnOrderWhenTheRegionIsFull) {
    for (int64_t number = 1; number <= region_capacity; ++number) {
        const std::string cl_ord_id = "ORD-" + std::to_string(number);
        machine_.apply_new_order(new_order(cl_ord_id), number, taken_at, member, CommandSource::live);
    }
    machine_.apply_new_order(new_order("ORD-FULL"), region_capacity + 1, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), static_cast<size_t>(region_capacity) + 1);
    const RecordedReport& refusal = outputs_.reports.back();
    EXPECT_EQ(refusal.exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(refusal.text, "the venue is holding as many open orders as it can");
    EXPECT_EQ(outputs_.accepted, region_capacity);
}

// ---- live cancels -----------------------------------------------------------------------

TEST_F(MatchingEngineStateMachineTest, CancelsAnOpenOrderAndReportsItsTerms) {
    machine_.apply_new_order(new_order("ORD-1", "250"), 1, taken_at, member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 2, taken_at + 1, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    const RecordedReport& cancelled = outputs_.reports[1];
    EXPECT_EQ(cancelled.exec_type, pubsub_itc_fw_app::ExecType::Canceled);
    EXPECT_EQ(cancelled.order_id, "ME-ORD-1") << "the report names the order cancelled";
    EXPECT_EQ(cancelled.cl_ord_id, "CXL-1");
    EXPECT_EQ(cancelled.orig_cl_ord_id, "ORD-1");
    EXPECT_EQ(cancelled.order_qty, "250") << "the quantity comes from the order on the book, not the cancel";
    EXPECT_EQ(cancelled.transact_time, taken_at + 1);
    EXPECT_FALSE(book_.contains(OrderKey::make(member, "ORD-1")));
    EXPECT_EQ(book_.published(), 2);
    EXPECT_EQ(outputs_.removed, std::vector<std::string>{"ORD-1@2"});
    EXPECT_EQ(outputs_.outcomes.back().outcome, pubsub_itc_fw_app::OrderOutcomeKind::Cancelled);
}

TEST_F(MatchingEngineStateMachineTest, RefusesACancelOfAnOrderTheBookDoesNotHold) {
    machine_.apply_cancel(cancel_of("ORD-9", "CXL-1"), 1, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[0].ord_rej_reason, pubsub_itc_fw_app::OrdRejReason::UnknownOrder);
    EXPECT_EQ(outputs_.reports[0].orig_cl_ord_id, "ORD-9") << "carried so the gateway answers with an OrderCancelReject";
    EXPECT_TRUE(outputs_.removed.empty());
    EXPECT_EQ(outputs_.outcomes.back().outcome, pubsub_itc_fw_app::OrderOutcomeKind::CancelRefusedUnknown);
}

TEST_F(MatchingEngineStateMachineTest, ACancelFromAnotherSessionDoesNotFindTheOrder) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 2, taken_at, other_member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].ord_rej_reason, pubsub_itc_fw_app::OrdRejReason::UnknownOrder);
    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
}

TEST_F(MatchingEngineStateMachineTest, RefusesACancelWhoseSequenceNumberDoesNotGoForward) {
    machine_.apply_new_order(new_order("ORD-1"), 4, taken_at, member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 3, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].text, "Venue could not process the cancel: it arrived out of sequence. The order is unchanged");
    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
}

// ---- catch-up ---------------------------------------------------------------------------

TEST_F(MatchingEngineStateMachineTest, ACatchUpAppliesAnOrderAndReportsItAsAPossibleRepeat) {
    machine_.apply_new_order(new_order("ORD-1"), 3, taken_at, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::New);
    EXPECT_EQ(outputs_.reports[0].repeat, ReportIsRepeat::yes);
    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
    EXPECT_EQ(book_.published(), 3);
    EXPECT_EQ(outputs_.accepted, 0) << "an order applied from a catch-up is not counted as processed";
    EXPECT_TRUE(outputs_.added.empty()) << "nor replicated to the follower";
    EXPECT_EQ(machine_.highest_applied(), 0) << "a catch-up moves the position only when it completes";
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpIgnoresAnOrderTheBookAlreadyHolds) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::catch_up);

    EXPECT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(book_.size(), 1U);
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpDoesNotRefuseByOrderOfSequenceNumbersNorWhileHalted) {
    machine_.note_caught_up_to(10);
    machine_.halt();
    machine_.apply_new_order(new_order("ORD-1"), 5, taken_at, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::New);
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpRefusesAnOverLongClOrdIdAsAPossibleRepeatAndPublishes) {
    const std::string too_long(fix_order_limits::max_cl_ord_id_length + 1, 'X');
    machine_.apply_new_order(new_order(too_long), 6, taken_at, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[0].repeat, ReportIsRepeat::yes);
    EXPECT_EQ(book_.published(), 6);
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpCancelOfAnOpenOrderIsReportedAsAPossibleRepeat) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::catch_up);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 2, taken_at, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].exec_type, pubsub_itc_fw_app::ExecType::Canceled);
    EXPECT_EQ(outputs_.reports[1].order_id, "ME-ORD-1");
    EXPECT_EQ(outputs_.reports[1].repeat, ReportIsRepeat::yes);
    EXPECT_EQ(book_.size(), 0U);
    EXPECT_TRUE(outputs_.removed.empty());
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpCancelOfAnOrderNotOnTheBookProducesNoReportButPublishes) {
    machine_.apply_cancel(cancel_of("ORD-9", "CXL-1"), 4, taken_at, member, CommandSource::catch_up);

    EXPECT_TRUE(outputs_.reports.empty());
    EXPECT_EQ(book_.published(), 4);
}

// ---- the whole book ---------------------------------------------------------------------

TEST_F(MatchingEngineStateMachineTest, CancellingEveryOrderReportsEachAndEmptiesTheBook) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-2"), 2, taken_at, other_member, CommandSource::live);
    outputs_.reports.clear();

    const MatchingEngineStateMachine::BookCancelled cancelled = machine_.cancel_every_order(taken_at + 99);

    EXPECT_EQ(cancelled.open_orders, 2);
    EXPECT_EQ(cancelled.reports_sent, 2);
    ASSERT_EQ(outputs_.reports.size(), 2U);
    for (const RecordedReport& report : outputs_.reports) {
        EXPECT_EQ(report.exec_type, pubsub_itc_fw_app::ExecType::Canceled);
        EXPECT_EQ(report.sequence_number, 0) << "no command produced these cancels";
        EXPECT_EQ(report.transact_time, taken_at + 99);
    }
    EXPECT_EQ(book_.size(), 0U);
}

TEST_F(MatchingEngineStateMachineTest, OrdersAcceptedAfterARecoveryAreNumberedAboveThoseRecovered) {
    machine_.note_recovered_orders(41);
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].order_id, "ME-ORD-42");
}

// ---- the same commands, the same results --------------------------------------------------

// Two state machines given the same commands produce the same reports. Phase 7 depends on this holding
// for every command, across restarts and changes of leader; here it is checked for the simplest case,
// one sequence of commands applied live to two fresh books.
TEST_F(MatchingEngineStateMachineTest, TwoStateMachinesGivenTheSameCommandsProduceTheSameReports) {
    OrderBook second_book;
    second_book.open(dir_ + "/second.region", region_capacity, map_capacity);
    RecordingOutputs second_outputs;
    MatchingEngineStateMachine second{second_book, second_outputs, logger_};

    for (MatchingEngineStateMachine* machine : {&machine_, &second}) {
        machine->apply_new_order(new_order("ORD-1"), 1, taken_at, member, CommandSource::live);
        machine->apply_new_order(new_order("ORD-1"), 2, taken_at, member, CommandSource::live);
        machine->apply_new_order(new_order("ORD-2"), 3, taken_at, other_member, CommandSource::live);
        machine->apply_cancel(cancel_of("ORD-1", "CXL-1"), 4, taken_at, member, CommandSource::live);
        machine->apply_cancel(cancel_of("ORD-1", "CXL-2"), 5, taken_at, member, CommandSource::live);
    }

    ASSERT_EQ(outputs_.reports.size(), second_outputs.reports.size());
    for (size_t index = 0; index < outputs_.reports.size(); ++index) {
        const RecordedReport& first_report = outputs_.reports[index];
        const RecordedReport& second_report = second_outputs.reports[index];
        EXPECT_EQ(first_report.order_id, second_report.order_id) << "report " << index;
        EXPECT_EQ(first_report.exec_id, second_report.exec_id) << "report " << index;
        EXPECT_EQ(first_report.exec_type, second_report.exec_type) << "report " << index;
        EXPECT_EQ(first_report.cl_ord_id, second_report.cl_ord_id) << "report " << index;
        EXPECT_EQ(first_report.transact_time, second_report.transact_time) << "report " << index;
    }
}

} // un-named namespace

} // namespaces
