// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file MatchingEngineStateMachineFuzzTest.cpp
 * @brief Random, well-formed commands applied to the matching engine's state machine, with the book checked after each.
 *
 * Section 6 of docs/availability/deterministic_matching_engine.md. A deterministic engine pair meets a defect in both
 * engines at the same command, so the state machine is driven here with commands drawn at random from the shapes a
 * member can send: new orders and cancels from a few sessions, over a small set of ClOrdIDs so that duplicates and
 * cancels of orders not on the book happen often, with a region small enough to fill, and now and then a ClOrdID
 * longer than the book holds or a command with no time or no trading date. After every command the test checks:
 *
 * - exactly one report was produced, with an ExecID never produced before;
 * - the book holds exactly the orders a simple model of the rules says it holds, and the report says what the model
 *   says happened;
 * - the book's region and its map agree (OrderBook::consistency_problem).
 *
 * A second state machine, with its own book and its own copy of the secret, is given the same commands, and must
 * produce the same reports, field by field.
 *
 * The seed is fixed, so that a failure can be repeated exactly. The number of commands is 200,000 by default, which
 * runs in a few seconds as an ordinary test; set MATCHING_ENGINE_FUZZ_COMMANDS to run more by hand, and
 * MATCHING_ENGINE_FUZZ_SEED to try another seed.
 */

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <MatchingEngineStateMachine.hpp>
#include <OrderKey.hpp>
#include <ReportIdentifiers.hpp>

namespace matching_engine::tests {

namespace {

// What the test needs of a report, with its strings copied out of the buffers that end with the call.
struct Report {
    std::string exec_id;
    std::string order_id;
    std::string cl_ord_id;
    std::string text;
    pubsub_itc_fw_app::ExecType exec_type{};
    int64_t transact_time{0};
    int64_t sequence_number{0};
    std::string comp_id;

    bool operator==(const Report& other) const {
        return exec_id == other.exec_id && order_id == other.order_id && cl_ord_id == other.cl_ord_id && text == other.text && exec_type == other.exec_type &&
               transact_time == other.transact_time && sequence_number == other.sequence_number && comp_id == other.comp_id;
    }
};

class Outputs : public MatchingEngineOutputsInterface {
  public:
    void send_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t sequence_number, const fix_common::SessionIdentity& session,
                     ReportIsRepeat /*repeat*/) override {
        Report copy;
        copy.exec_id = std::string(report.exec_id);
        copy.order_id = std::string(report.order_id);
        copy.cl_ord_id = std::string(report.cl_ord_id);
        copy.text = report.has_text ? std::string(report.text) : std::string();
        copy.exec_type = report.exec_type;
        copy.transact_time = report.transact_time;
        copy.sequence_number = sequence_number;
        copy.comp_id = std::string(session.comp_id_view());
        reports.push_back(std::move(copy));
    }
    void order_added_to_book(int64_t /*sequence_number*/, const fix_common::SessionIdentity& /*session*/, std::string_view /*cl_ord_id*/,
                             const OrderEntry& /*entry*/) override {}
    void order_removed_from_book(int64_t /*sequence_number*/, const fix_common::SessionIdentity& /*session*/, std::string_view /*cl_ord_id*/) override {}
    void order_outcome(const fix_common::SessionIdentity& /*session*/, std::string_view /*cl_ord_id*/, pubsub_itc_fw_app::OrderOutcomeKind /*outcome*/,
                       ReportIsRepeat /*repeat*/, int64_t /*sequence_number*/, std::string_view /*orig_cl_ord_id*/) override {}
    void order_accepted() override {}

    std::vector<Report> reports;
};

ReportIdentifiers::Secret fuzz_secret() {
    ReportIdentifiers::Secret secret{};
    for (size_t index = 0; index < secret.size(); ++index) {
        secret[index] = static_cast<uint8_t>(255 - index);
    }
    return secret;
}

constexpr OrderBook::SlotIndex region_capacity = 32;
constexpr int32_t trading_day = 20261025;
constexpr int64_t first_time = 1'792'900'000'000'000'000;

int64_t setting_from_environment(const char* name, int64_t fallback) {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::atoll(value);
}

// One engine: its book, its outputs and its state machine.
struct Engine {
    explicit Engine(const std::string& region_path) {
        book.open(region_path, region_capacity, 64);
    }
    pubsub_itc_fw::QuillLogger logger{pubsub_itc_fw::FwLogLevel::Critical, [](const std::string&) {}};
    OrderBook book;
    Outputs outputs;
    ReportIdentifiers identifiers{fuzz_secret()};
    MatchingEngineStateMachine machine{book, identifiers, outputs, logger};
};

class MatchingEngineStateMachineFuzzTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char tmpl[] = "state_machine_fuzz_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl), nullptr);
        dir_ = tmpl;
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::string dir_;
};

} // un-named namespace

TEST_F(MatchingEngineStateMachineFuzzTest, RandomCommandsKeepTheBookConsistentAndTwoEnginesAgree) {
    const int64_t commands = setting_from_environment("MATCHING_ENGINE_FUZZ_COMMANDS", 200'000);
    const auto seed = static_cast<uint64_t>(setting_from_environment("MATCHING_ENGINE_FUZZ_SEED", 20261025));
    std::mt19937_64 random(seed);

    Engine engine(dir_ + "/first.region");
    Engine other(dir_ + "/second.region");

    const std::vector<fix_common::SessionIdentity> sessions = {
        fix_common::SessionIdentity::make("MEMBER-A", 1), fix_common::SessionIdentity::make("MEMBER-B", 1), fix_common::SessionIdentity::make("MEMBER-B", 2)};
    const std::string too_long(fix_order_limits::max_cl_ord_id_length + 1, 'X');

    // The model: which orders are open, by session and ClOrdID. The rules it follows are the state machine's
    // documented ones, written out again here so that the two can disagree.
    std::set<std::pair<size_t, std::string>> open;
    std::set<std::string> exec_ids;

    for (int64_t sequence_number = 1; sequence_number <= commands; ++sequence_number) {
        const size_t session_index = random() % sessions.size();
        const fix_common::SessionIdentity& session = sessions[session_index];
        const std::string cl_ord_id = "C" + std::to_string(random() % 48);
        const uint64_t oddity = random() % 1000;
        const bool over_long = oddity < 5;
        const bool no_time = oddity >= 5 && oddity < 8;
        const bool no_date = oddity >= 8 && oddity < 11;
        const int64_t transact_time = no_time ? 0 : first_time + sequence_number;
        const int32_t trading_date = no_date ? 0 : trading_day;
        const bool is_new_order = random() % 100 < 60;

        // Only this command's reports are kept, so that a long run does not hold every report of the day.
        engine.outputs.reports.clear();
        other.outputs.reports.clear();
        const size_t reports_before = 0;
        if (is_new_order) {
            pubsub_itc_fw_app::NewOrderSingleView order{};
            order.cl_ord_id = over_long ? std::string_view(too_long) : std::string_view(cl_ord_id);
            order.side = random() % 2 == 0 ? pubsub_itc_fw_app::Side::Buy : pubsub_itc_fw_app::Side::Sell;
            order.symbol = "VOD.L";
            order.ord_type = pubsub_itc_fw_app::OrdType::Limit;
            order.order_qty = "100";
            order.has_price = true;
            order.price = "101.25";
            engine.machine.apply_new_order(order, sequence_number, transact_time, trading_date, session, CommandSource::live);
            other.machine.apply_new_order(order, sequence_number, transact_time, trading_date, session, CommandSource::live);

            const bool accepted_by_model = !over_long && !no_time && !no_date && open.count({session_index, cl_ord_id}) == 0 && open.size() < region_capacity;
            ASSERT_EQ(engine.outputs.reports.size(), reports_before + 1) << "command " << sequence_number << " gave other than one report";
            const Report& report = engine.outputs.reports.back();
            ASSERT_EQ(report.exec_type == pubsub_itc_fw_app::ExecType::New, accepted_by_model)
                << "command " << sequence_number << ": a new order "
                << (accepted_by_model ? "the model accepts was refused: " : "the model refuses was accepted") << report.text;
            if (accepted_by_model) {
                open.insert({session_index, cl_ord_id});
            }
        } else {
            const std::string orig_cl_ord_id = "C" + std::to_string(random() % 48);
            const std::string cancel_id = "X" + std::to_string(sequence_number);
            pubsub_itc_fw_app::OrderCancelRequestView cancel{};
            cancel.cl_ord_id = cancel_id;
            cancel.orig_cl_ord_id = over_long ? std::string_view(too_long) : std::string_view(orig_cl_ord_id);
            cancel.side = pubsub_itc_fw_app::Side::Buy;
            cancel.symbol = "VOD.L";
            engine.machine.apply_cancel(cancel, sequence_number, transact_time, trading_date, session, CommandSource::live);
            other.machine.apply_cancel(cancel, sequence_number, transact_time, trading_date, session, CommandSource::live);

            const bool cancelled_by_model = !over_long && !no_time && !no_date && open.count({session_index, orig_cl_ord_id}) == 1;
            ASSERT_EQ(engine.outputs.reports.size(), reports_before + 1) << "command " << sequence_number << " gave other than one report";
            const Report& report = engine.outputs.reports.back();
            ASSERT_EQ(report.exec_type == pubsub_itc_fw_app::ExecType::Canceled, cancelled_by_model)
                << "command " << sequence_number << ": a cancel " << (cancelled_by_model ? "the model carries out was refused: " : "the model refuses was done")
                << report.text;
            if (cancelled_by_model) {
                open.erase({session_index, orig_cl_ord_id});
            }
        }

        const Report& report = engine.outputs.reports.back();
        ASSERT_TRUE(exec_ids.insert(report.exec_id).second) << "command " << sequence_number << " gave ExecID " << report.exec_id << ", given before";
        ASSERT_EQ(engine.book.size(), open.size()) << "after command " << sequence_number;
        const std::string problem = engine.book.consistency_problem();
        ASSERT_TRUE(problem.empty()) << "after command " << sequence_number << ": " << problem;
        ASSERT_EQ(other.outputs.reports.size(), engine.outputs.reports.size()) << "after command " << sequence_number;
        ASSERT_TRUE(other.outputs.reports.back() == report) << "the two engines answered command " << sequence_number << " differently";
    }
    for (const auto& [session_index, cl_ord_id] : open) {
        EXPECT_TRUE(engine.book.contains(OrderKey::make(sessions[session_index], cl_ord_id))) << cl_ord_id;
    }
    EXPECT_GT(engine.book.size(), 0U) << "the run ended with an empty book, which suggests the mix of commands is wrong";
}

} // namespaces
