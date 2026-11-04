// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "CommandChecks.hpp"
#include "GatewayIds.hpp"
#include "SequencerCommandIdentity.hpp"

namespace sequencer::tests {

namespace {

using Outcome = CommandChecks::Outcome;
using std::chrono::seconds;

// The WAL as CommandChecks sees it: the identities of the commands it holds, and which of them a report answers.
struct RecordingLinks : public CommandChecksLinksInterface {
    [[nodiscard]] std::optional<pubsub_itc_fw::CommandInWal> find_command(std::string_view identity, int64_t first_sent_no_earlier_than) override {
        searched_from.push_back(first_sent_no_earlier_than);
        const auto found = held.find(std::string(identity));
        if (found == held.end()) {
            return std::nullopt;
        }
        return pubsub_itc_fw::CommandInWal{found->second, std::nullopt};
    }
    bool answer_from_wal(const pubsub_itc_fw_app::WalRecordView&, const pubsub_itc_fw::CommandInWal& command) override {
        answered.push_back(command.record_id);
        return report_held;
    }
    void refuse(const pubsub_itc_fw::ConnectionID& came_by, const pubsub_itc_fw_app::WalRecordView&, int64_t first_seq_no) override {
        refused_on.push_back(came_by.get_value());
        refused_first.push_back(first_seq_no);
    }
    int64_t discard_command_search() override {
        ++searches_discarded;
        return 0;
    }
    [[nodiscard]] pubsub_itc_fw::ReplicatedWal::CommandCheckCounts command_check_counts() const override {
        return {};
    }

    // Holds the command from @p comp_id with @p cl_ord_id, arriving by the FIX gateway, as record @p record_id.
    void hold(const std::string& comp_id, const std::string& cl_ord_id, int64_t record_id) {
        std::array<char, pubsub_itc_fw::CommandIdentityInterface::max_identity_size> buffer{};
        const size_t size = write_command_identity(comp_id, gateway_ids::fix_order_gateway, cl_ord_id, buffer.data(), buffer.size());
        held[std::string(buffer.data(), size)] = record_id;
    }

    std::map<std::string, int64_t> held;
    bool report_held{true};
    std::vector<int64_t> searched_from;
    std::vector<int64_t> answered;
    std::vector<int> refused_on;
    std::vector<int64_t> refused_first;
    int searches_discarded{0};
};

class CommandChecksTest : public ::testing::Test {
  protected:
    void SetUp() override {
        checks_ = std::make_unique<CommandChecks>(links_, logger_);
    }

    static pubsub_itc_fw_app::WalRecordView command(std::string_view comp_id, std::string_view cl_ord_id, bool sent_again) {
        pubsub_itc_fw_app::WalRecordView view{};
        view.has_sender_comp_id = !comp_id.empty();
        view.sender_comp_id = comp_id;
        view.has_cl_ord_id = !cl_ord_id.empty();
        view.cl_ord_id = cl_ord_id;
        view.has_origin_gateway_id = true;
        view.origin_gateway_id = gateway_ids::fix_order_gateway;
        view.has_sent_again = true;
        view.sent_again = sent_again;
        view.has_gateway_ingress_ns = true;
        view.gateway_ingress_ns = 5000;
        return view;
    }

    Outcome check(std::string_view comp_id, std::string_view cl_ord_id, bool sent_again, CommandChecks::Clock::duration after = seconds{0}) {
        return checks_->check(came_by_, command(comp_id, cl_ord_id, sent_again), start_ + after);
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

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<CommandChecks> checks_;
    const pubsub_itc_fw::ConnectionID came_by_{41};
    const CommandChecks::Clock::time_point start_ = CommandChecks::Clock::now();
};

} // namespaces

TEST_F(CommandChecksTest, ACommandTheWalDoesNotHoldIsSequenced) {
    EXPECT_EQ(check("MEMBER1", "A1", false), Outcome::sequence_it);
    EXPECT_EQ(checks_->sent_again_sequenced(), 0) << "only a command sent again is counted";
    EXPECT_EQ(links_.searched_from, (std::vector<int64_t>{std::numeric_limits<int64_t>::min()})) << "a ClOrdID may be reused from any time today";
}

TEST_F(CommandChecksTest, ACommandSentAgainThatTheWalDoesNotHoldIsSequencedAndCounted) {
    EXPECT_EQ(check("MEMBER1", "A1", true), Outcome::sequence_it);
    EXPECT_EQ(checks_->sent_again_sequenced(), 1);
    EXPECT_EQ(links_.searched_from, (std::vector<int64_t>{5000})) << "a command sent again was first sent no earlier than its gateway received it";
}

TEST_F(CommandChecksTest, ACommandThatReusesAClOrdIdIsRefusedOnTheConnectionItCameBy) {
    links_.hold("MEMBER1", "A1", 7);
    EXPECT_EQ(check("MEMBER1", "A1", false), Outcome::not_sequenced);
    EXPECT_EQ(links_.refused_on, (std::vector<int>{41}));
    EXPECT_EQ(links_.refused_first, (std::vector<int64_t>{7}));
    EXPECT_TRUE(links_.answered.empty());
    EXPECT_EQ(checks_->refused_reused_identifier(), 1);
    EXPECT_TRUE(logged("refusing comp_id='MEMBER1' ClOrdID=A1"));
}

TEST_F(CommandChecksTest, TheSameClOrdIdFromAnotherMemberIsAnotherCommand) {
    links_.hold("MEMBER1", "A1", 7);
    EXPECT_EQ(check("MEMBER2", "A1", false), Outcome::sequence_it);
    EXPECT_TRUE(links_.refused_on.empty());
}

TEST_F(CommandChecksTest, TheSameClOrdIdByTheOtherGatewayIsAnotherCommand) {
    links_.hold("MEMBER1", "A1", 7);
    pubsub_itc_fw_app::WalRecordView by_binary = command("MEMBER1", "A1", false);
    by_binary.origin_gateway_id = gateway_ids::binary_order_gateway;
    EXPECT_EQ(checks_->check(came_by_, by_binary, start_), Outcome::sequence_it);
    EXPECT_TRUE(links_.refused_on.empty());
}

TEST_F(CommandChecksTest, ACommandSentAgainThatTheWalHoldsIsAnsweredFromTheWal) {
    links_.hold("MEMBER1", "A1", 7);
    EXPECT_EQ(check("MEMBER1", "A1", true), Outcome::not_sequenced);
    EXPECT_EQ(links_.answered, (std::vector<int64_t>{7}));
    EXPECT_TRUE(links_.refused_on.empty()) << "sent again is not a reuse";
    EXPECT_EQ(checks_->sent_again_already_in_wal(), 1);
    EXPECT_EQ(checks_->answered_from_wal(), 1);
}

TEST_F(CommandChecksTest, ACommandSentAgainWithNoReportInTheWalIsPassedOverForTheEngineToAnswer) {
    links_.hold("MEMBER1", "A1", 7);
    links_.report_held = false;
    EXPECT_EQ(check("MEMBER1", "A1", true), Outcome::not_sequenced);
    EXPECT_EQ(checks_->sent_again_already_in_wal(), 1);
    EXPECT_EQ(checks_->answered_from_wal(), 0);
}

TEST_F(CommandChecksTest, ACommandThatDoesNotSayWhoseItIsIsSequencedUncheckedAndLoggedOnce) {
    EXPECT_EQ(check("", "A1", false), Outcome::sequence_it);
    EXPECT_EQ(check("MEMBER1", "", false), Outcome::sequence_it);
    EXPECT_EQ(checks_->unchecked_without_identity(), 2);
    EXPECT_TRUE(links_.searched_from.empty()) << "nothing to look for";
    logger_.flush();
    int errors = 0;
    for (const std::string& line : log_) {
        errors += line.find("cannot be checked against the WAL") != std::string::npos ? 1 : 0;
    }
    EXPECT_EQ(errors, 1);
}

TEST_F(CommandChecksTest, TheSummaryIsLoggedAMinuteAfterTheLastCommandSentAgainOrRefused) {
    links_.hold("MEMBER1", "A1", 7);
    check("MEMBER1", "A1", false);
    checks_->tick(start_ + CommandChecks::summary_idle_limit - seconds{1});
    EXPECT_EQ(links_.searches_discarded, 0);
    checks_->tick(start_ + CommandChecks::summary_idle_limit);
    EXPECT_EQ(links_.searches_discarded, 1);
    EXPECT_TRUE(logged("since the last summary of checks"));
    EXPECT_EQ(checks_->refused_reused_identifier(), 0) << "the counts start again";
    checks_->tick(start_ + 2 * CommandChecks::summary_idle_limit);
    EXPECT_EQ(links_.searches_discarded, 1) << "nothing checked since";
}

TEST_F(CommandChecksTest, AnOrdinaryCommandDoesNotStartTheWaitForASummary) {
    check("MEMBER1", "A1", false);
    checks_->tick(start_ + CommandChecks::summary_idle_limit);
    EXPECT_EQ(links_.searches_discarded, 0);
}

TEST_F(CommandChecksTest, EachCommandSentAgainPutsTheSummaryOff) {
    check("MEMBER1", "A1", true);
    check("MEMBER1", "A2", true, seconds{30});
    checks_->tick(start_ + CommandChecks::summary_idle_limit);
    EXPECT_EQ(links_.searches_discarded, 0);
    checks_->tick(start_ + seconds{30} + CommandChecks::summary_idle_limit);
    EXPECT_EQ(links_.searches_discarded, 1);
}

TEST_F(CommandChecksTest, StoppingLeadingLogsTheSummaryIfAnythingWasChecked) {
    checks_->stopped_leading();
    EXPECT_EQ(links_.searches_discarded, 0);
    check("MEMBER1", "A1", true);
    checks_->stopped_leading();
    EXPECT_EQ(links_.searches_discarded, 1);
    EXPECT_TRUE(logged("this instance has stopped leading"));
}

} // namespaces
