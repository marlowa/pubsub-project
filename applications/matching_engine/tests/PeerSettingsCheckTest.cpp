// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "PeerSettingsCheck.hpp"

namespace matching_engine::tests {

namespace {

constexpr int64_t own_capacity = 1000;
constexpr int64_t own_fingerprint = 77;

// A lease agent reduced to what PeerSettingsCheck asks of it, and the test events it raises.
struct RecordingLinks : public PeerSettingsCheckLinksInterface {
    [[nodiscard]] bool leading() const override {
        return is_leading;
    }
    [[nodiscard]] bool says_peer_may_lead() const override {
        return peer_may_lead;
    }
    void say_peer_may_not_lead() override {
        peer_may_lead = false;
        ++statements;
    }
    void say_peer_may_lead() override {
        peer_may_lead = true;
        ++statements;
    }
    void settings_differ(int64_t own, int64_t peer, bool barred, bool secrets_differ) override {
        events.push_back(Event{own, peer, barred, secrets_differ});
    }
    struct Event {
        int64_t own{0};
        int64_t peer{0};
        bool barred{false};
        bool secrets_differ{false};
    };
    bool is_leading{false};
    bool peer_may_lead{true};
    int statements{0};
    std::vector<Event> events;
};

class PeerSettingsCheckTest : public ::testing::Test {
  protected:
    void SetUp() override {
        check_ = std::make_unique<PeerSettingsCheck>(own_capacity, own_fingerprint, 2, "secret.key", links_, logger_);
    }

    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_{pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }};
    RecordingLinks links_;
    std::unique_ptr<PeerSettingsCheck> check_;
};

} // namespaces

TEST_F(PeerSettingsCheckTest, SettingsNotYetKnownBarNobody) {
    links_.is_leading = true;
    EXPECT_FALSE(check_->bar_peer_if_settings_differ());
    EXPECT_EQ(links_.statements, 0);
    check_->report_present_state();
    EXPECT_TRUE(links_.events.empty());
}

TEST_F(PeerSettingsCheckTest, TheSameSettingsAreNoDifference) {
    links_.is_leading = true;
    check_->peer_settings_arrived(2, own_capacity, own_fingerprint);
    EXPECT_TRUE(check_->known());
    EXPECT_FALSE(check_->differ());
    EXPECT_EQ(links_.statements, 0) << "already saying the peer may lead";
    EXPECT_TRUE(links_.events.empty());
}

TEST_F(PeerSettingsCheckTest, AFollowerToldOfADifferentCapacityReportsItButBarsNobody) {
    check_->peer_settings_arrived(2, own_capacity + 1, own_fingerprint);
    EXPECT_TRUE(check_->differ());
    EXPECT_EQ(links_.statements, 0);
    ASSERT_EQ(links_.events.size(), 1U);
    EXPECT_EQ(links_.events[0].peer, own_capacity + 1);
    EXPECT_FALSE(links_.events[0].barred);
    EXPECT_FALSE(links_.events[0].secrets_differ);
}

TEST_F(PeerSettingsCheckTest, ALeaderBarsAPeerWithADifferentSecretAndReportsItOnce) {
    links_.is_leading = true;
    check_->peer_settings_arrived(2, own_capacity, own_fingerprint + 1);
    EXPECT_FALSE(links_.peer_may_lead);
    ASSERT_EQ(links_.events.size(), 1U) << "reported once, by the bar";
    EXPECT_TRUE(links_.events[0].barred);
    EXPECT_TRUE(links_.events[0].secrets_differ);
    EXPECT_FALSE(check_->bar_peer_if_settings_differ()) << "already saying the peer may not lead";
    EXPECT_EQ(links_.statements, 1);
}

TEST_F(PeerSettingsCheckTest, APeerConfiguredAlikeAgainMayLeadAgain) {
    links_.is_leading = true;
    check_->peer_settings_arrived(2, own_capacity + 1, own_fingerprint);
    ASSERT_FALSE(links_.peer_may_lead);
    check_->peer_settings_arrived(2, own_capacity, own_fingerprint);
    EXPECT_TRUE(links_.peer_may_lead);
    EXPECT_FALSE(check_->differ());
}

TEST_F(PeerSettingsCheckTest, AnInstanceThatTakesTheLeadBarsAPeerItAlreadyKnowsDiffers) {
    check_->peer_settings_arrived(2, own_capacity + 1, own_fingerprint);
    ASSERT_TRUE(links_.peer_may_lead);
    links_.is_leading = true;
    EXPECT_TRUE(check_->bar_peer_if_settings_differ());
    EXPECT_FALSE(links_.peer_may_lead);
}

TEST_F(PeerSettingsCheckTest, TheDifferenceIsReportedAgainAsThePresentState) {
    check_->peer_settings_arrived(2, own_capacity + 1, own_fingerprint);
    links_.events.clear();
    check_->report_present_state();
    EXPECT_EQ(links_.events.size(), 1U);
}

} // namespaces
