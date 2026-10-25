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

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <ReportIdentifiers.hpp>

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
    std::string price;
    bool has_time_in_force{false};
    pubsub_itc_fw_app::TimeInForce time_in_force{};
    bool has_expire_time{false};
    int64_t expire_time{0};
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
        if (fail_next_report) {
            fail_next_report = false;
            throw std::runtime_error("the test made sending this report fail");
        }
        RecordedReport recorded;
        recorded.order_id = std::string(report.order_id);
        recorded.exec_id = std::string(report.exec_id);
        recorded.exec_type = report.exec_type;
        recorded.ord_status = report.ord_status;
        recorded.cl_ord_id = std::string(report.cl_ord_id);
        recorded.orig_cl_ord_id = report.has_orig_cl_ord_id ? std::string(report.orig_cl_ord_id) : std::string();
        recorded.symbol = std::string(report.symbol);
        recorded.order_qty = report.has_order_qty ? std::string(report.order_qty) : std::string();
        recorded.price = report.has_price ? std::string(report.price) : std::string();
        recorded.has_time_in_force = report.has_time_in_force;
        recorded.time_in_force = report.time_in_force;
        recorded.has_expire_time = report.has_expire_time;
        recorded.expire_time = report.expire_time;
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
        if (fail_next_outcome) {
            fail_next_outcome = false;
            throw std::runtime_error("the test made telling it what became of this command fail");
        }
        outcomes.push_back(RecordedOutcome{std::string(cl_ord_id), outcome, repeat, sequence_number});
    }

    void order_accepted() override {
        ++accepted;
    }

    // Set by a test to make the next call of that kind throw, as a defect reached while applying a command would.
    bool fail_next_report{false};
    bool fail_next_outcome{false};

    std::vector<RecordedReport> reports;
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<RecordedOutcome> outcomes;
    int accepted{0};
};

constexpr OrderBook::SlotIndex region_capacity = 4;
constexpr size_t map_capacity = 16;
constexpr int64_t taken_at = 1'700'000'000'000'000'000;
constexpr int32_t trading_day = 20231114;

ReportIdentifiers::Secret test_secret() {
    ReportIdentifiers::Secret secret{};
    for (size_t index = 0; index < secret.size(); ++index) {
        secret[index] = static_cast<uint8_t>(index * 7 + 3);
    }
    return secret;
}

// The identifiers an engine with the test's secret gives on the test's trading date, made by an object of their own
// so that a test compares what the state machine produced with what the definition says, not with itself.
std::string order_id_of(int64_t placed_at_seq_no) {
    ReportIdentifiers identifiers(test_secret());
    identifiers.use_trading_date(trading_day);
    ReportIdentifiers::Buffer buffer{};
    return std::string(identifiers.order_id(buffer, placed_at_seq_no));
}

std::string exec_id_of(int64_t sequence_number, int64_t place) {
    ReportIdentifiers identifiers(test_secret());
    identifiers.use_trading_date(trading_day);
    ReportIdentifiers::Buffer buffer{};
    return std::string(identifiers.exec_id(buffer, sequence_number, place));
}

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
    ReportIdentifiers identifiers_{test_secret()};
    MatchingEngineStateMachine machine_{book_, identifiers_, outputs_, logger_};
    // For a second state machine in a test, as a second engine would hold its own.
    ReportIdentifiers second_identifiers_{test_secret()};
};

// ---- live new orders --------------------------------------------------------------------

TEST_F(MatchingEngineStateMachineTest, AcceptsANewOrderOntoTheBookAndAcknowledgesIt) {
    machine_.apply_new_order(new_order("ORD-1"), 5, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    const RecordedReport& report = outputs_.reports[0];
    EXPECT_EQ(report.exec_type, pubsub_itc_fw_app::ExecType::New);
    EXPECT_EQ(report.ord_status, pubsub_itc_fw_app::OrdStatus::New);
    EXPECT_EQ(report.order_id, order_id_of(5)) << "an OrderID is made from the sequence number of the new order that placed it";
    EXPECT_EQ(report.exec_id, exec_id_of(5, 0)) << "an ExecID is made from the sequence number of its command and its place among the command's reports";
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

TEST_F(MatchingEngineStateMachineTest, NamesOrdersAndReportsByTheSequenceNumbersOfTheirCommands) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-2"), 2, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].order_id, order_id_of(2));
    EXPECT_EQ(outputs_.reports[1].exec_id, exec_id_of(2, 0));
}

TEST_F(MatchingEngineStateMachineTest, RefusesASecondOrderWithTheSameClOrdIdFromTheSameSession) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-1"), 2, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    const RecordedReport& refusal = outputs_.reports[1];
    EXPECT_EQ(refusal.exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(refusal.order_id, "NONE");
    EXPECT_TRUE(refusal.has_ord_rej_reason);
    EXPECT_EQ(refusal.ord_rej_reason, pubsub_itc_fw_app::OrdRejReason::DuplicateOrder);
    EXPECT_EQ(outputs_.accepted, 1);
    EXPECT_EQ(outputs_.outcomes.back().outcome, pubsub_itc_fw_app::OrderOutcomeKind::RefusedDuplicate);
    EXPECT_EQ(book_.published(), 1) << "a live refusal changes nothing and is not published, so a restart applies it again";
}

TEST_F(MatchingEngineStateMachineTest, TheSameClOrdIdFromAnotherSessionIsAnotherOrder) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-1"), 2, taken_at, trading_day, other_member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].exec_type, pubsub_itc_fw_app::ExecType::New);
    EXPECT_EQ(book_.size(), 2U);
}

TEST_F(MatchingEngineStateMachineTest, RefusesAnOrderWhoseSequenceNumberDoesNotGoForward) {
    machine_.apply_new_order(new_order("ORD-1"), 7, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-2"), 7, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[1].text, "Venue could not process the order: it arrived out of sequence");
    EXPECT_FALSE(book_.contains(OrderKey::make(member, "ORD-2")));
}

TEST_F(MatchingEngineStateMachineTest, RefusesALiveOrderAtOrBelowThePositionACatchUpReached) {
    machine_.note_caught_up_to(10);
    machine_.apply_new_order(new_order("ORD-1"), 10, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(book_.size(), 0U);
}

TEST_F(MatchingEngineStateMachineTest, RefusesAClOrdIdLongerThanTheBookHolds) {
    const std::string too_long(fix_order_limits::max_cl_ord_id_length + 1, 'X');
    machine_.apply_new_order(new_order(too_long), 1, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[0].text, "ClOrdID exceeds maximum length of " + std::to_string(fix_order_limits::max_cl_ord_id_length));
    EXPECT_EQ(book_.size(), 0U);
    EXPECT_EQ(outputs_.outcomes.back().outcome, pubsub_itc_fw_app::OrderOutcomeKind::RefusedTooLong);
}

TEST_F(MatchingEngineStateMachineTest, RefusesEveryNewOrderWhileHalted) {
    machine_.halt();
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);

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
        machine_.apply_new_order(new_order(cl_ord_id), number, taken_at, trading_day, member, CommandSource::live);
    }
    machine_.apply_new_order(new_order("ORD-FULL"), region_capacity + 1, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), static_cast<size_t>(region_capacity) + 1);
    const RecordedReport& refusal = outputs_.reports.back();
    EXPECT_EQ(refusal.exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(refusal.text, "the venue is holding as many open orders as it can");
    EXPECT_EQ(outputs_.accepted, region_capacity);
}

// The sequencer writes the time of every command into its WAL record, so a record with none is the sequencer's
// defect. The engine refuses the command rather than supply a time of its own, which would differ from one engine
// to the next, and the refusal carries the time the command did, zero.
TEST_F(MatchingEngineStateMachineTest, RefusesANewOrderThatCarriesNoTime) {
    machine_.apply_new_order(new_order("ORD-1"), 1, 0, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[0].text, "Venue could not process the order: it carried no time");
    EXPECT_EQ(outputs_.reports[0].transact_time, 0);
    EXPECT_EQ(outputs_.reports[0].repeat, ReportIsRepeat::no);
    EXPECT_EQ(book_.size(), 0U);
    EXPECT_EQ(outputs_.accepted, 0);
    EXPECT_EQ(machine_.highest_applied(), 1) << "the command was acted on, by refusing it";
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpRefusesAnOrderThatCarriesNoTimeAsAPossibleRepeatAndPublishes) {
    machine_.apply_new_order(new_order("ORD-1"), 8, 0, trading_day, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].text, "Venue could not process the order: it carried no time");
    EXPECT_EQ(outputs_.reports[0].repeat, ReportIsRepeat::yes);
    EXPECT_EQ(book_.size(), 0U);
    EXPECT_EQ(book_.published(), 8);
}

// The trading date is what the keys of the identifiers come from, so a command without one is refused, as one without
// a time is: it is a defect in the sequencer, and every engine refuses it alike. The refusal carries the command's time.
TEST_F(MatchingEngineStateMachineTest, RefusesANewOrderThatCarriesNoTradingDate) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, 0, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[0].text, "Venue could not process the order: it carried no trading date");
    EXPECT_EQ(outputs_.reports[0].transact_time, taken_at);
    EXPECT_EQ(book_.size(), 0U);
}

TEST_F(MatchingEngineStateMachineTest, RefusesACancelThatCarriesNoTradingDateAndLeavesTheOrder) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 2, taken_at, 0, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].text, "Venue could not process the cancel: it carried no trading date. The order is unchanged");
    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
}

// Support staff turn an ExecID a member quotes back into the command behind it, with the day key.
TEST_F(MatchingEngineStateMachineTest, AReportsExecIdTurnsBackIntoItsCommandsSequenceNumber) {
    machine_.apply_new_order(new_order("ORD-1"), 41, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 42, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    ReportIdentifiers support(test_secret());
    support.use_trading_date(trading_day);
    const auto accepted = support.position_of(outputs_.reports[0].exec_id);
    const auto cancelled = support.position_of(outputs_.reports[1].exec_id);
    const auto order = support.position_of(outputs_.reports[1].order_id);
    ASSERT_TRUE(accepted.has_value() && cancelled.has_value() && order.has_value());
    EXPECT_EQ(accepted->sequence_number, 41);
    EXPECT_EQ(cancelled->sequence_number, 42);
    EXPECT_EQ(order->sequence_number, 41) << "the cancel's report names the order by the new order that placed it";
}

// An identifier holds sequence numbers below 2^36. A command beyond that could only be given an identifier another report
// already has, so it is not answered at all, and trading halts.
TEST_F(MatchingEngineStateMachineTest, ACommandBeyondWhatAnIdentifierHoldsIsNotAnsweredAndHaltsTrading) {
    machine_.apply_new_order(new_order("ORD-1"), ReportIdentifiers::sequence_number_limit, taken_at, trading_day, member, CommandSource::live);

    EXPECT_TRUE(outputs_.reports.empty());
    EXPECT_EQ(book_.size(), 0U);
    EXPECT_TRUE(machine_.halted());
}

// ---- a command that fails while it is applied --------------------------------------------

// A failure while one command is applied costs that command, not the engine. Here the failure comes before the command
// has changed the book or been answered, in a duplicate's refusal, so it is rejected to its member as one the venue could
// not process, and the next command is applied as usual.
TEST_F(MatchingEngineStateMachineTest, AFailureBeforeACommandChangesAnythingRejectsThatCommandAndTheNextIsApplied) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);
    outputs_.fail_next_outcome = true;
    machine_.apply_new_order(new_order("ORD-1"), 2, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    const RecordedReport& rejection = outputs_.reports[1];
    EXPECT_EQ(rejection.exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(rejection.text, "Venue could not process the order");
    EXPECT_EQ(rejection.exec_id, exec_id_of(2, 0));
    EXPECT_EQ(book_.size(), 1U);
    logger_.flush();
    EXPECT_TRUE(std::any_of(log_.begin(), log_.end(), [](const std::string& line) {
        return line.find("command seq=2 failed while it was applied") != std::string::npos && line.find("is rejected to its member") != std::string::npos;
    }));

    machine_.apply_new_order(new_order("ORD-2"), 3, taken_at, trading_day, member, CommandSource::live);
    ASSERT_EQ(outputs_.reports.size(), 3U);
    EXPECT_EQ(outputs_.reports[2].exec_type, pubsub_itc_fw_app::ExecType::New);
    EXPECT_EQ(book_.size(), 2U);
}

// Here the failure comes after the order was put on the book, in sending its acknowledgement. A rejection would then
// contradict a book that holds the order, so nothing more is sent: the order stands, and the book is published to it.
TEST_F(MatchingEngineStateMachineTest, AFailureAfterTheBookChangedLeavesTheChangeAndSendsNoRejection) {
    outputs_.fail_next_report = true;
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);

    EXPECT_TRUE(outputs_.reports.empty());
    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
    EXPECT_EQ(book_.published(), 1);
    EXPECT_TRUE(book_.consistency_problem().empty());

    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 2, taken_at, trading_day, member, CommandSource::live);
    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Canceled);
}

// ---- live cancels -----------------------------------------------------------------------

TEST_F(MatchingEngineStateMachineTest, RefusesACancelThatCarriesNoTimeAndLeavesTheOrder) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 2, 0, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].text, "Venue could not process the cancel: it carried no time. The order is unchanged");
    EXPECT_EQ(outputs_.reports[1].orig_cl_ord_id, "ORD-1");
    EXPECT_EQ(outputs_.reports[1].transact_time, 0);
    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpRefusesACancelThatCarriesNoTimeAndLeavesTheOrder) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::catch_up);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 2, 0, trading_day, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[1].repeat, ReportIsRepeat::yes);
    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
    EXPECT_EQ(book_.published(), 2);
}

TEST_F(MatchingEngineStateMachineTest, CancelsAnOpenOrderAndReportsItsTerms) {
    machine_.apply_new_order(new_order("ORD-1", "250"), 1, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 2, taken_at + 1, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    const RecordedReport& cancelled = outputs_.reports[1];
    EXPECT_EQ(cancelled.exec_type, pubsub_itc_fw_app::ExecType::Canceled);
    EXPECT_EQ(cancelled.order_id, order_id_of(1)) << "the report names the order cancelled";
    EXPECT_EQ(cancelled.exec_id, exec_id_of(2, 0)) << "the report is named by the cancel's own sequence number";
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
    machine_.apply_cancel(cancel_of("ORD-9", "CXL-1"), 1, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[0].ord_rej_reason, pubsub_itc_fw_app::OrdRejReason::UnknownOrder);
    EXPECT_EQ(outputs_.reports[0].orig_cl_ord_id, "ORD-9") << "carried so the gateway answers with an OrderCancelReject";
    EXPECT_TRUE(outputs_.removed.empty());
    EXPECT_EQ(outputs_.outcomes.back().outcome, pubsub_itc_fw_app::OrderOutcomeKind::CancelRefusedUnknown);
}

TEST_F(MatchingEngineStateMachineTest, ACancelFromAnotherSessionDoesNotFindTheOrder) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 2, taken_at, trading_day, other_member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].ord_rej_reason, pubsub_itc_fw_app::OrdRejReason::UnknownOrder);
    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
}

TEST_F(MatchingEngineStateMachineTest, RefusesACancelWhoseSequenceNumberDoesNotGoForward) {
    machine_.apply_new_order(new_order("ORD-1"), 4, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 3, taken_at, trading_day, member, CommandSource::live);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].text, "Venue could not process the cancel: it arrived out of sequence. The order is unchanged");
    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
}

// ---- catch-up ---------------------------------------------------------------------------

TEST_F(MatchingEngineStateMachineTest, ACatchUpAppliesAnOrderAndReportsItAsAPossibleRepeat) {
    machine_.apply_new_order(new_order("ORD-1"), 3, taken_at, trading_day, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::New);
    EXPECT_EQ(outputs_.reports[0].repeat, ReportIsRepeat::yes);
    EXPECT_TRUE(book_.contains(OrderKey::make(member, "ORD-1")));
    EXPECT_EQ(book_.published(), 3);
    EXPECT_EQ(outputs_.accepted, 0) << "an order applied from a catch-up is not counted as processed";
    EXPECT_TRUE(outputs_.added.empty()) << "nor replicated to the follower";
    EXPECT_EQ(machine_.highest_applied(), 0) << "a catch-up moves the position only when it completes";
}

// The halt is a decision this engine takes alone, and the records a catch-up applies are ones the venue took before it.
TEST_F(MatchingEngineStateMachineTest, ACatchUpIsNotRefusedWhileHalted) {
    machine_.halt();
    machine_.apply_new_order(new_order("ORD-1"), 5, taken_at, trading_day, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::New);
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpRefusesAnOverLongClOrdIdAsAPossibleRepeatAndPublishes) {
    const std::string too_long(fix_order_limits::max_cl_ord_id_length + 1, 'X');
    machine_.apply_new_order(new_order(too_long), 6, taken_at, trading_day, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::Rejected);
    EXPECT_EQ(outputs_.reports[0].repeat, ReportIsRepeat::yes);
    EXPECT_EQ(book_.published(), 6);
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpCancelOfAnOpenOrderIsReportedAsAPossibleRepeat) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::catch_up);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 2, taken_at, trading_day, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].exec_type, pubsub_itc_fw_app::ExecType::Canceled);
    EXPECT_EQ(outputs_.reports[1].order_id, order_id_of(1));
    EXPECT_EQ(outputs_.reports[1].repeat, ReportIsRepeat::yes);
    EXPECT_EQ(book_.size(), 0U);
    EXPECT_TRUE(outputs_.removed.empty());
}

// A catch-up can deliver records the engine has already acted on live. They are not applied again: no second report,
// no refusal as a duplicate, and the published position does not move back.
TEST_F(MatchingEngineStateMachineTest, ACatchUpDoesNotApplyAgainACommandAlreadyActedOn) {
    machine_.apply_new_order(new_order("ORD-1"), 5, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-2"), 6, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 7, taken_at, trading_day, member, CommandSource::live);
    outputs_.reports.clear();
    outputs_.outcomes.clear();

    machine_.apply_new_order(new_order("ORD-1"), 5, taken_at, trading_day, member, CommandSource::catch_up);
    machine_.apply_new_order(new_order("ORD-2"), 6, taken_at, trading_day, member, CommandSource::catch_up);
    machine_.apply_cancel(cancel_of("ORD-1", "CXL-1"), 7, taken_at, trading_day, member, CommandSource::catch_up);
    EXPECT_TRUE(outputs_.reports.empty()) << "a record already acted on produces no report";
    EXPECT_TRUE(outputs_.outcomes.empty());
    EXPECT_EQ(book_.size(), 1U);
    EXPECT_EQ(book_.published(), 7);

    machine_.apply_new_order(new_order("ORD-3"), 8, taken_at, trading_day, member, CommandSource::catch_up);
    ASSERT_EQ(outputs_.reports.size(), 1U) << "a record above what was acted on is applied";
    EXPECT_EQ(outputs_.reports[0].exec_type, pubsub_itc_fw_app::ExecType::New);
    EXPECT_EQ(book_.published(), 8);
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpRefusesADuplicateClOrdIdAsALiveOrderIs) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::catch_up);
    machine_.apply_new_order(new_order("ORD-1"), 2, taken_at, trading_day, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 2U);
    EXPECT_EQ(outputs_.reports[1].ord_rej_reason, pubsub_itc_fw_app::OrdRejReason::DuplicateOrder);
    EXPECT_EQ(outputs_.reports[1].repeat, ReportIsRepeat::yes);
    EXPECT_EQ(book_.size(), 1U);
    EXPECT_EQ(book_.published(), 2);
}

TEST_F(MatchingEngineStateMachineTest, ACatchUpRefusesACancelOfAnOrderNotOnTheBookAsALiveCancelIs) {
    machine_.apply_cancel(cancel_of("ORD-9", "CXL-1"), 4, taken_at, trading_day, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].ord_rej_reason, pubsub_itc_fw_app::OrdRejReason::UnknownOrder);
    EXPECT_EQ(outputs_.reports[0].repeat, ReportIsRepeat::yes);
    EXPECT_EQ(book_.published(), 4);
}

// The same commands, applied live by one state machine and from a catch-up by another, give the same reports,
// differing only in being marked as possible repeats, and the same book. An order's terms -- its time in force and
// its expiry -- are held and reported alike, so that a cancel after a catch-up describes the order as one after live
// trading does.
TEST_F(MatchingEngineStateMachineTest, TheSameCommandsLiveAndFromACatchUpGiveTheSameReportsAndBook) {
    OrderBook caught_up_book;
    caught_up_book.open(dir_ + "/caught_up.region", region_capacity, map_capacity);
    RecordingOutputs caught_up_outputs;
    MatchingEngineStateMachine caught_up{caught_up_book, second_identifiers_, caught_up_outputs, logger_};

    pubsub_itc_fw_app::NewOrderSingleView good_till_date = new_order("GTD-1", "300");
    good_till_date.has_time_in_force = true;
    good_till_date.time_in_force = pubsub_itc_fw_app::TimeInForce::GoodTillDate;
    good_till_date.has_expire_time = true;
    good_till_date.expire_time = taken_at + 86'400'000'000'000;

    const auto apply_all = [&](MatchingEngineStateMachine& machine, CommandSource source) {
        machine.apply_new_order(good_till_date, 1, taken_at, trading_day, member, source);
        machine.apply_new_order(new_order("ORD-2"), 2, taken_at + 1, trading_day, member, source);
        machine.apply_new_order(new_order("ORD-2"), 3, taken_at + 2, trading_day, member, source);
        machine.apply_cancel(cancel_of("ORD-9", "CXL-9"), 4, taken_at + 3, trading_day, member, source);
        machine.apply_cancel(cancel_of("GTD-1", "CXL-1"), 5, taken_at + 4, trading_day, member, source);
        machine.apply_new_order(new_order("ORD-3"), 6, taken_at + 5, trading_day, other_member, source);
    };
    apply_all(machine_, CommandSource::live);
    apply_all(caught_up, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), caught_up_outputs.reports.size());
    for (size_t index = 0; index < outputs_.reports.size(); ++index) {
        const RecordedReport& live_report = outputs_.reports[index];
        const RecordedReport& caught_up_report = caught_up_outputs.reports[index];
        EXPECT_EQ(live_report.order_id, caught_up_report.order_id) << "report " << index;
        EXPECT_EQ(live_report.exec_id, caught_up_report.exec_id) << "report " << index;
        EXPECT_EQ(live_report.exec_type, caught_up_report.exec_type) << "report " << index;
        EXPECT_EQ(live_report.ord_status, caught_up_report.ord_status) << "report " << index;
        EXPECT_EQ(live_report.cl_ord_id, caught_up_report.cl_ord_id) << "report " << index;
        EXPECT_EQ(live_report.orig_cl_ord_id, caught_up_report.orig_cl_ord_id) << "report " << index;
        EXPECT_EQ(live_report.symbol, caught_up_report.symbol) << "report " << index;
        EXPECT_EQ(live_report.order_qty, caught_up_report.order_qty) << "report " << index;
        EXPECT_EQ(live_report.price, caught_up_report.price) << "report " << index;
        EXPECT_EQ(live_report.has_time_in_force, caught_up_report.has_time_in_force) << "report " << index;
        EXPECT_EQ(live_report.time_in_force, caught_up_report.time_in_force) << "report " << index;
        EXPECT_EQ(live_report.has_expire_time, caught_up_report.has_expire_time) << "report " << index;
        EXPECT_EQ(live_report.expire_time, caught_up_report.expire_time) << "report " << index;
        EXPECT_EQ(live_report.leaves_qty, caught_up_report.leaves_qty) << "report " << index;
        EXPECT_EQ(live_report.ord_rej_reason, caught_up_report.ord_rej_reason) << "report " << index;
        EXPECT_EQ(live_report.text, caught_up_report.text) << "report " << index;
        EXPECT_EQ(live_report.transact_time, caught_up_report.transact_time) << "report " << index;
        EXPECT_EQ(live_report.sequence_number, caught_up_report.sequence_number) << "report " << index;
        EXPECT_EQ(live_report.repeat, ReportIsRepeat::no) << "report " << index;
        EXPECT_EQ(caught_up_report.repeat, ReportIsRepeat::yes) << "report " << index;
    }
    // The cancel of the good-till-date order reports its terms, in both.
    EXPECT_TRUE(caught_up_outputs.reports[4].has_time_in_force);
    EXPECT_TRUE(caught_up_outputs.reports[4].has_expire_time);

    EXPECT_EQ(book_.size(), caught_up_book.size());
    EXPECT_EQ(book_.published(), caught_up_book.published());
    for (const char* cl_ord_id : {"ORD-2", "ORD-3"}) {
        const fix_common::SessionIdentity& owner = std::string_view(cl_ord_id) == "ORD-3" ? other_member : member;
        const OrderEntry* live_entry = book_.find(OrderKey::make(owner, cl_ord_id));
        const OrderEntry* caught_up_entry = caught_up_book.find(OrderKey::make(owner, cl_ord_id));
        ASSERT_NE(live_entry, nullptr) << cl_ord_id;
        ASSERT_NE(caught_up_entry, nullptr) << cl_ord_id;
        // Field by field rather than byte by byte: the bytes between fields are not guaranteed to be the same.
        EXPECT_EQ(live_entry->placed_at_seq_no, caught_up_entry->placed_at_seq_no) << cl_ord_id;
        EXPECT_EQ(live_entry->side, caught_up_entry->side) << cl_ord_id;
        EXPECT_EQ(live_entry->ord_type, caught_up_entry->ord_type) << cl_ord_id;
        EXPECT_EQ(live_entry->has_time_in_force, caught_up_entry->has_time_in_force) << cl_ord_id;
        EXPECT_EQ(live_entry->time_in_force, caught_up_entry->time_in_force) << cl_ord_id;
        EXPECT_EQ(live_entry->has_expire_time, caught_up_entry->has_expire_time) << cl_ord_id;
        EXPECT_EQ(live_entry->expire_time, caught_up_entry->expire_time) << cl_ord_id;
        EXPECT_EQ(live_entry->has_price, caught_up_entry->has_price) << cl_ord_id;
        EXPECT_EQ(live_entry->get_symbol(), caught_up_entry->get_symbol()) << cl_ord_id;
        EXPECT_EQ(live_entry->get_order_qty(), caught_up_entry->get_order_qty()) << cl_ord_id;
        EXPECT_EQ(live_entry->get_price(), caught_up_entry->get_price()) << cl_ord_id;
        EXPECT_EQ(live_entry->get_cl_ord_id(), caught_up_entry->get_cl_ord_id()) << cl_ord_id;
        EXPECT_EQ(live_entry->session.comp_id_view(), caught_up_entry->session.comp_id_view()) << cl_ord_id;
    }
}

// ---- the whole book ---------------------------------------------------------------------

TEST_F(MatchingEngineStateMachineTest, CancellingEveryOrderReportsEachAndEmptiesTheBook) {
    machine_.apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-2"), 2, taken_at, trading_day, other_member, CommandSource::live);
    outputs_.reports.clear();

    const MatchingEngineStateMachine::BookCancelled cancelled = machine_.cancel_every_order(2, 1, taken_at + 99, trading_day);

    EXPECT_EQ(cancelled.open_orders, 2);
    EXPECT_EQ(cancelled.reports_sent, 2);
    ASSERT_EQ(outputs_.reports.size(), 2U);
    for (const RecordedReport& report : outputs_.reports) {
        EXPECT_EQ(report.exec_type, pubsub_itc_fw_app::ExecType::Canceled);
        EXPECT_EQ(report.sequence_number, 0) << "no command produced these cancels";
        EXPECT_EQ(report.transact_time, taken_at + 99);
    }
    EXPECT_EQ(outputs_.reports[0].exec_id, exec_id_of(2, 1)) << "named by the sequence number given, from the first place given";
    EXPECT_EQ(outputs_.reports[1].exec_id, exec_id_of(2, 2));
    EXPECT_EQ(book_.size(), 0U);
}

// Cancelled in the order the orders were placed, so that an engine that built its book live and one that built it from
// a catch-up in a different order of arrival into the map send the same cancels in the same order.
TEST_F(MatchingEngineStateMachineTest, CancellingEveryOrderReportsThemInTheOrderTheyWerePlaced) {
    machine_.apply_new_order(new_order("ORD-C"), 3, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-A"), 4, taken_at, trading_day, other_member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-B"), 5, taken_at, trading_day, member, CommandSource::live);
    machine_.apply_new_order(new_order("ORD-D"), 6, taken_at, trading_day, other_member, CommandSource::live);
    machine_.apply_cancel(cancel_of("ORD-A", "CXL-A"), 7, taken_at, trading_day, other_member, CommandSource::live);
    outputs_.reports.clear();

    static_cast<void>(machine_.cancel_every_order(7, 1, taken_at, trading_day));

    std::vector<std::string> order_ids;
    for (const RecordedReport& report : outputs_.reports) {
        order_ids.push_back(report.order_id);
    }
    EXPECT_EQ(order_ids, (std::vector<std::string>{order_id_of(3), order_id_of(5), order_id_of(6)}));
    EXPECT_EQ(outputs_.reports[0].cl_ord_id, "ORD-C");
}

// The OrderID and the ExecID are made from the sequence number, so an engine that applies an order live and one that
// applies it while catching up, each with its own copy of the secret, give it the same identifiers.
TEST_F(MatchingEngineStateMachineTest, AnOrderHasTheSameOrderIdLiveAndFromACatchUp) {
    OrderBook second_book;
    second_book.open(dir_ + "/second.region", region_capacity, map_capacity);
    RecordingOutputs second_outputs;
    MatchingEngineStateMachine second{second_book, second_identifiers_, second_outputs, logger_};

    machine_.apply_new_order(new_order("ORD-1"), 17, taken_at, trading_day, member, CommandSource::live);
    second.apply_new_order(new_order("ORD-1"), 17, taken_at, trading_day, member, CommandSource::catch_up);

    ASSERT_EQ(outputs_.reports.size(), 1U);
    ASSERT_EQ(second_outputs.reports.size(), 1U);
    EXPECT_EQ(outputs_.reports[0].order_id, order_id_of(17));
    EXPECT_EQ(second_outputs.reports[0].order_id, order_id_of(17));
    EXPECT_EQ(outputs_.reports[0].exec_id, second_outputs.reports[0].exec_id) << "and its report the same ExecID";
    EXPECT_EQ(book_.find(OrderKey::make(member, "ORD-1"))->placed_at_seq_no, 17);
}

// ---- the same commands, the same results --------------------------------------------------

// Two state machines given the same commands produce the same reports. Phase 7 depends on this holding
// for every command, across restarts and changes of leader; here it is checked for the simplest case,
// one sequence of commands applied live to two fresh books.
TEST_F(MatchingEngineStateMachineTest, TwoStateMachinesGivenTheSameCommandsProduceTheSameReports) {
    OrderBook second_book;
    second_book.open(dir_ + "/second.region", region_capacity, map_capacity);
    RecordingOutputs second_outputs;
    MatchingEngineStateMachine second{second_book, second_identifiers_, second_outputs, logger_};

    for (MatchingEngineStateMachine* machine : {&machine_, &second}) {
        machine->apply_new_order(new_order("ORD-1"), 1, taken_at, trading_day, member, CommandSource::live);
        machine->apply_new_order(new_order("ORD-1"), 2, taken_at, trading_day, member, CommandSource::live);
        machine->apply_new_order(new_order("ORD-2"), 3, taken_at, trading_day, other_member, CommandSource::live);
        machine->apply_cancel(cancel_of("ORD-1", "CXL-1"), 4, taken_at, trading_day, member, CommandSource::live);
        machine->apply_cancel(cancel_of("ORD-1", "CXL-2"), 5, taken_at, trading_day, member, CommandSource::live);
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
