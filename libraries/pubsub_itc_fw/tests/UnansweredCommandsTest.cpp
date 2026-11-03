// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/PacedResend.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/UnansweredCommands.hpp>

namespace pubsub_itc_fw::tests {

namespace {

constexpr size_t primary = 0;
constexpr size_t secondary = 1;

// Records what UnansweredCommands asks the client to do.
struct RecordingLinks : public UnansweredCommandLinksInterface {
    struct Sent {
        int connection;
        std::string command;
    };
    void send_again(const ConnectionID& connection, const uint8_t* kept, size_t size) override {
        sent.push_back(Sent{connection.get_value(), std::string(reinterpret_cast<const char*>(kept), size)});
    }
    void request_writable_notification(const ConnectionID& connection) override {
        writable_requested.push_back(connection.get_value());
    }
    // The commands sent again on @p connection, in the order sent.
    [[nodiscard]] std::vector<std::string> sent_on(int connection) const {
        std::vector<std::string> commands;
        for (const Sent& one : sent) {
            if (one.connection == connection) {
                commands.push_back(one.command);
            }
        }
        return commands;
    }
    std::vector<Sent> sent;
    std::vector<int> writable_requested;
};

class UnansweredCommandsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        commands_ = std::make_unique<UnansweredCommands>(1024 * 1024, 4096, std::vector<std::string>{"primary sequencer", "secondary sequencer"}, links_,
                                                         logger_, "UnansweredCommandsTest");
    }

    // Keeps the command (MEMBER, @p cl_ord_id), whose bytes are its ClOrdID.
    UnansweredCommands::Kept keep(const std::string& cl_ord_id, const std::string& comp_id = "MEMBER") {
        return commands_->keep(comp_id, cl_ord_id, reinterpret_cast<const uint8_t*>(cl_ord_id.data()), cl_ord_id.size());
    }

    // Connects both destinations for the first time, as connections 11 and 12.
    void connect_both() {
        commands_->on_connection_established(primary, primary_connection_);
        commands_->on_connection_established(secondary, secondary_connection_);
    }

    std::vector<std::string> log_;
    QuillLogger logger_{FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<UnansweredCommands> commands_;
    const ConnectionID primary_connection_{11};
    const ConnectionID secondary_connection_{12};
};

} // namespaces

TEST_F(UnansweredCommandsTest, NothingIsSentAgainWhenAConnectionIsFirstEstablished) {
    ASSERT_EQ(keep("order-1"), UnansweredCommands::Kept::kept);
    connect_both();
    EXPECT_TRUE(links_.sent.empty());
}

TEST_F(UnansweredCommandsTest, EveryCopyIsSentAgainOnAConnectionEstablishedAgainAndOnNoOther) {
    connect_both();
    ASSERT_EQ(keep("order-1"), UnansweredCommands::Kept::kept);
    ASSERT_EQ(keep("order-2"), UnansweredCommands::Kept::kept);
    commands_->on_connection_established(primary, ConnectionID{21});
    EXPECT_EQ(links_.sent_on(21), (std::vector<std::string>{"order-1", "order-2"})) << "oldest first";
    EXPECT_TRUE(links_.sent_on(12).empty()) << "the secondary's connection was not closed";
}

TEST_F(UnansweredCommandsTest, ANewLeaderIsSentEveryCopyOnEveryConnectedDestination) {
    connect_both();
    ASSERT_EQ(keep("order-1"), UnansweredCommands::Kept::kept);
    commands_->on_new_leader(2, false);
    EXPECT_EQ(links_.sent_on(11), (std::vector<std::string>{"order-1"}));
    EXPECT_EQ(links_.sent_on(12), (std::vector<std::string>{"order-1"}));
}

TEST_F(UnansweredCommandsTest, TheFirstLeaderIsSentCopiesOnlyWhenThereAreAny) {
    connect_both();
    commands_->on_new_leader(1, true);
    EXPECT_TRUE(links_.sent.empty());
    ASSERT_EQ(keep("order-1"), UnansweredCommands::Kept::kept);
    commands_->on_new_leader(1, true);
    EXPECT_EQ(links_.sent_on(11), (std::vector<std::string>{"order-1"})) << "accepted before any leader, and discarded by an instance not yet leading";
}

TEST_F(UnansweredCommandsTest, ADestinationNeverConnectedIsNotSentTo) {
    commands_->on_connection_established(primary, primary_connection_);
    ASSERT_EQ(keep("order-1"), UnansweredCommands::Kept::kept);
    commands_->on_new_leader(2, false);
    EXPECT_EQ(links_.sent.size(), 1U);
    EXPECT_EQ(links_.sent_on(11), (std::vector<std::string>{"order-1"}));
}

TEST_F(UnansweredCommandsTest, AnsweredCommandsAndThoseOfADroppedSessionAreNotSentAgain) {
    connect_both();
    ASSERT_EQ(keep("order-1"), UnansweredCommands::Kept::kept);
    ASSERT_EQ(keep("order-2"), UnansweredCommands::Kept::kept);
    ASSERT_EQ(keep("order-3", "OTHER"), UnansweredCommands::Kept::kept);
    EXPECT_TRUE(commands_->answered("MEMBER", "order-1"));
    EXPECT_FALSE(commands_->answered("MEMBER", "order-1")) << "answered once only";
    EXPECT_EQ(commands_->drop_session("OTHER"), 1U);
    EXPECT_EQ(commands_->unanswered(), 1U);
    commands_->on_new_leader(2, false);
    EXPECT_EQ(links_.sent_on(11), (std::vector<std::string>{"order-2"}));
}

TEST_F(UnansweredCommandsTest, CopiesGoInBatchesEachAfterTheConnectionHasWrittenTheLast) {
    connect_both();
    const size_t total = 2 * PacedResend::batch_size + 10;
    for (size_t number = 0; number < total; ++number) {
        ASSERT_EQ(keep("order-" + std::to_string(number)), UnansweredCommands::Kept::kept);
    }
    commands_->on_connection_established(primary, ConnectionID{21});
    EXPECT_EQ(links_.sent_on(21).size(), PacedResend::batch_size);
    EXPECT_EQ(links_.writable_requested, (std::vector<int>{21}));
    EXPECT_FALSE(commands_->on_connection_writable(secondary_connection_)) << "nothing is being sent on the secondary";
    EXPECT_TRUE(commands_->on_connection_writable(ConnectionID{21}));
    EXPECT_EQ(links_.sent_on(21).size(), 2 * PacedResend::batch_size);
    EXPECT_TRUE(commands_->on_connection_writable(ConnectionID{21}));
    EXPECT_EQ(links_.sent_on(21).size(), total);
    EXPECT_EQ(links_.writable_requested.size(), 2U) << "no notification is asked for after the last batch";
    EXPECT_FALSE(commands_->on_connection_writable(ConnectionID{21})) << "the resend has finished";
    EXPECT_EQ(links_.sent_on(21).back(), "order-" + std::to_string(total - 1));
}

TEST_F(UnansweredCommandsTest, ADestinationStillBeingSentToStartsAgainFromTheBeginningOfANewCopy) {
    connect_both();
    const size_t total = PacedResend::batch_size + 10;
    for (size_t number = 0; number < total; ++number) {
        ASSERT_EQ(keep("order-" + std::to_string(number)), UnansweredCommands::Kept::kept);
    }
    commands_->on_connection_established(primary, ConnectionID{21});
    ASSERT_EQ(links_.sent_on(21).size(), PacedResend::batch_size);
    // The secondary's connection is established again while the primary's resend is half done: both start again.
    commands_->on_connection_established(secondary, ConnectionID{22});
    ASSERT_EQ(links_.sent_on(21).size(), 2 * PacedResend::batch_size);
    EXPECT_EQ(links_.sent_on(21)[PacedResend::batch_size], "order-0") << "started again from the oldest";
    EXPECT_EQ(links_.sent_on(22).size(), PacedResend::batch_size);
}

TEST_F(UnansweredCommandsTest, NothingMoreIsSentOnALostConnectionAndEverythingIsSentWhenItIsEstablishedAgain) {
    connect_both();
    const size_t total = PacedResend::batch_size + 10;
    for (size_t number = 0; number < total; ++number) {
        ASSERT_EQ(keep("order-" + std::to_string(number)), UnansweredCommands::Kept::kept);
    }
    commands_->on_new_leader(2, false);
    ASSERT_EQ(links_.sent_on(11).size(), PacedResend::batch_size);
    commands_->on_connection_lost(primary);
    EXPECT_FALSE(commands_->on_connection_writable(primary_connection_)) << "the lost connection's resend is abandoned";
    EXPECT_EQ(links_.sent_on(11).size(), PacedResend::batch_size);
    commands_->on_new_leader(3, false);
    EXPECT_EQ(links_.sent_on(11).size(), PacedResend::batch_size) << "a new leader sends nothing on a lost connection";
    commands_->on_connection_established(primary, ConnectionID{21});
    EXPECT_EQ(links_.sent_on(21).size(), PacedResend::batch_size);
    EXPECT_EQ(links_.sent_on(21).front(), "order-0");
}

TEST_F(UnansweredCommandsTest, ACommandHeldUnansweredIsNotKeptASecondTime) {
    ASSERT_EQ(keep("order-1"), UnansweredCommands::Kept::kept);
    EXPECT_EQ(keep("order-1"), UnansweredCommands::Kept::already_held);
    EXPECT_EQ(commands_->unanswered(), 1U);
    EXPECT_EQ(commands_->refused_because_full(), 0);
}

TEST_F(UnansweredCommandsTest, AFullStoreRefusesCommandsAndCountsThem) {
    commands_ = std::make_unique<UnansweredCommands>(1024, 2, std::vector<std::string>{"sequencer"}, links_, logger_, "UnansweredCommandsTest");
    ASSERT_EQ(keep("order-1"), UnansweredCommands::Kept::kept);
    ASSERT_EQ(keep("order-2"), UnansweredCommands::Kept::kept);
    EXPECT_EQ(keep("order-3"), UnansweredCommands::Kept::full);
    EXPECT_EQ(keep("order-4"), UnansweredCommands::Kept::full);
    EXPECT_EQ(commands_->refused_because_full(), 2);
    EXPECT_TRUE(commands_->answered("MEMBER", "order-1"));
    EXPECT_EQ(keep("order-5"), UnansweredCommands::Kept::kept) << "room again once a command is answered";
}

} // namespaces
