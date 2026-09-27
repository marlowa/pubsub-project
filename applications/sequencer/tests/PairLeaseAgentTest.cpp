// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <LeaseLinksInterface.hpp>
#include <LeasePromiseRecorderInterface.hpp>
#include <LeasePromiseStore.hpp>
#include <LeaseTiming.hpp>
#include <LeaseVoter.hpp>
#include <PairLeaseAgent.hpp>
#include <leader_follower.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr int64_t arbiter_id = 3;

// What an agent sent, held until the test delivers it.
struct Outbox : public fix_common::LeaseLinksInterface {
    std::vector<pubsub_itc_fw_app::LeaseRequest> to_peer;
    std::vector<pubsub_itc_fw_app::LeaseRequest> to_third_voter;
    std::vector<pubsub_itc_fw_app::LeaseGrant> grants;
    std::vector<pubsub_itc_fw_app::LeaseRefusal> refusals;
    bool connected_to_peer{true};
    bool connected_to_third_voter{true};

    void send_request_to_peer(const pubsub_itc_fw_app::LeaseRequest& request) override {
        if (connected_to_peer) {
            to_peer.push_back(request);
        }
    }
    void send_request_to_third_voter(const pubsub_itc_fw_app::LeaseRequest& request) override {
        if (connected_to_third_voter) {
            to_third_voter.push_back(request);
        }
    }
    void send_grant(const pubsub_itc_fw::ConnectionID& /*conn_id*/, const pubsub_itc_fw_app::LeaseGrant& grant) override {
        grants.push_back(grant);
    }
    void send_refusal(const pubsub_itc_fw::ConnectionID& /*conn_id*/, const pubsub_itc_fw_app::LeaseRefusal& refusal) override {
        refusals.push_back(refusal);
    }
};

// Keeps promises in memory, as a disk would across a process restart.
struct KeptPromises : public fix_common::LeasePromiseRecorderInterface {
    int64_t promised_to{0};
    Clock::time_point until{};
    bool recorded_anything{false};
    bool fail{false};

    bool record(int64_t to, Clock::time_point when) override {
        if (fail) {
            return false;
        }
        promised_to = to;
        until = when;
        recorded_anything = true;
        return true;
    }

    std::optional<fix_common::LeasePromiseStore::Record> as_record() const {
        if (!recorded_anything) {
            return std::nullopt;
        }
        return fix_common::LeasePromiseStore::Record{promised_to, until};
    }
};

class PairLeaseAgentTest : public ::testing::Test {
  protected:
    PairLeaseAgentTest()
        : logger_(pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& record) { log_.push_back(record); }), arbiter_(timing_.period, start_, 0) {
        for (int i = 0; i < 2; ++i) {
            agents_[i] = std::make_unique<fix_common::PairLeaseAgent>("TestThread", logger_, outboxes_[i], pubsub_itc_fw_app::ComponentGroup::sequencer, i + 1,
                                                                      i == 0 ? 2 : 1, arbiter_id, "the arbiter", timing_, start_, 0);
        }
    }

    // One tick for both agents, then every message delivered at once, until nothing is left to deliver.
    void step() {
        now_ += fix_common::LeaseTiming::tick_interval;
        for (int i = 0; i < 2; ++i) {
            if (!down_[i]) {
                record(i, agents_[i]->on_tick(now_));
            }
        }
        deliver();
    }

    void deliver() {
        bool delivered = true;
        while (delivered) {
            delivered = false;
            for (int i = 0; i < 2; ++i) {
                const int peer = 1 - i;
                Outbox sent = outboxes_[i];
                outboxes_[i] = Outbox{};
                outboxes_[i].connected_to_peer = sent.connected_to_peer;
                outboxes_[i].connected_to_third_voter = sent.connected_to_third_voter;
                for (const auto& request : sent.to_peer) {
                    delivered = true;
                    if (down_[peer]) {
                        continue;
                    }
                    record(peer,
                           agents_[peer]->on_request(pubsub_itc_fw::ConnectionID{}, request.candidate_instance_id, request.epoch, request.request_id, now_));
                    for (const auto& grant : outboxes_[peer].grants) {
                        record(i, agents_[i]->on_grant(grant.voter_instance_id, grant.epoch, grant.request_id, now_));
                    }
                    for (const auto& refusal : outboxes_[peer].refusals) {
                        record(i, agents_[i]->on_refusal(refusal.voter_instance_id, refusal.highest_epoch, refusal.request_id, refusal.reason, now_));
                    }
                    outboxes_[peer].grants.clear();
                    outboxes_[peer].refusals.clear();
                }
                for (const auto& request : sent.to_third_voter) {
                    delivered = true;
                    if (!arbiter_up_) {
                        continue;
                    }
                    const auto answer = arbiter_.consider(request.candidate_instance_id, request.epoch, now_);
                    if (answer.verdict == fix_common::LeaseVoter::Verdict::Granted) {
                        record(i, agents_[i]->on_grant(arbiter_id, request.epoch, request.request_id, now_));
                    } else {
                        record(i, agents_[i]->on_refusal(arbiter_id, answer.highest_epoch, request.request_id,
                                                         pubsub_itc_fw_app::LeaseRefusalReason::promised_elsewhere, now_));
                    }
                }
            }
        }
    }

    // Instance i's process dies. It neither ticks nor answers until restart() is called.
    void crash(int i) {
        down_[i] = true;
    }

    // Instance i's process is started again, as a supervisor would. With keep_promises, the promises
    // it recorded before it died survive the restart.
    // Without keep_epoch it forgets its epoch as well, as an arbiter does.
    void restart(int i, bool keep_promises, bool keep_epoch = true) {
        down_[i] = false;
        agents_[i] = std::make_unique<fix_common::PairLeaseAgent>("TestThread", logger_, outboxes_[i], pubsub_itc_fw_app::ComponentGroup::sequencer, i + 1,
                                                                  i == 0 ? 2 : 1, arbiter_id, "the arbiter", timing_, now_,
                                                                  keep_epoch ? agents_[i]->highest_epoch() : 0);
        if (keep_promises) {
            agents_[i]->keep_promises_in(kept_[i], kept_[i].as_record(), now_);
        }
        changes_[i].clear();
    }

    void keep_promises() {
        for (int i = 0; i < 2; ++i) {
            agents_[i]->keep_promises_in(kept_[i], std::nullopt, now_);
        }
    }

    void record(int i, fix_common::PairLeaseAgent::Change change) {
        if (change != fix_common::PairLeaseAgent::Change::Nothing) {
            changes_[i].push_back(change);
        }
    }

    bool log_contains(const std::string& text) const {
        for (const auto& line : log_) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    fix_common::LeaseTiming timing_{};
    const Clock::time_point start_{};
    Clock::time_point now_{};
    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_;
    Outbox outboxes_[2];
    std::unique_ptr<fix_common::PairLeaseAgent> agents_[2];
    fix_common::LeaseVoter arbiter_;
    bool arbiter_up_{true};
    std::vector<fix_common::PairLeaseAgent::Change> changes_[2];
    KeptPromises kept_[2];
    bool down_[2]{false, false};
};

} // un-named namespace

TEST_F(PairLeaseAgentTest, NobodyAsksDuringTheFirstLeasePeriod) {
    while (now_ < start_ + timing_.period - fix_common::LeaseTiming::tick_interval) {
        step();
    }
    EXPECT_TRUE(changes_[0].empty());
    EXPECT_TRUE(changes_[1].empty());
}

TEST_F(PairLeaseAgentTest, ThePrimaryLeadsWhenBothStartTogether) {
    for (int i = 0; i < 40; ++i) {
        step();
    }
    EXPECT_TRUE(agents_[0]->acting(now_));
    EXPECT_FALSE(agents_[1]->acting(now_));
    ASSERT_FALSE(changes_[0].empty());
    EXPECT_EQ(changes_[0].front(), fix_common::PairLeaseAgent::Change::BecameLeader);
    ASSERT_FALSE(changes_[1].empty());
    EXPECT_EQ(changes_[1].front(), fix_common::PairLeaseAgent::Change::AgreedPeerLeads);
    EXPECT_TRUE(log_contains("TestThread: leading at epoch 1 -- granted a lease by"));
}

TEST_F(PairLeaseAgentTest, ALeaderKeepsLeadingWhileItRenews) {
    for (int i = 0; i < 400; ++i) {
        step();
    }
    EXPECT_TRUE(agents_[0]->acting(now_));
    EXPECT_EQ(std::count(changes_[0].begin(), changes_[0].end(), fix_common::PairLeaseAgent::Change::BecameLeader), 1);
}

TEST_F(PairLeaseAgentTest, TheArbiterBeingDownDoesNotStopTheLeader) {
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    arbiter_up_ = false;
    for (int i = 0; i < 400; ++i) {
        step();
    }
    EXPECT_TRUE(agents_[0]->acting(now_));
}

TEST_F(PairLeaseAgentTest, ALeaderCutOffFromBothVotersStopsLeading) {
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    outboxes_[0].connected_to_peer = false;
    outboxes_[0].connected_to_third_voter = false;
    for (int i = 0; i < 40; ++i) {
        step();
    }
    EXPECT_FALSE(agents_[0]->acting(now_));
    EXPECT_EQ(changes_[0].back(), fix_common::PairLeaseAgent::Change::StoppedLeading);
    EXPECT_TRUE(log_contains("TestThread: lease ran out -- neither the peer nor the arbiter renewed it in time"));
}

TEST_F(PairLeaseAgentTest, TheFollowerTakesOverWhenTheLeaderIsCutOff) {
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    outboxes_[0].connected_to_peer = false;
    outboxes_[0].connected_to_third_voter = false;
    bool both_acting = false;
    for (int i = 0; i < 200; ++i) {
        step();
        both_acting = both_acting || (agents_[0]->acting(now_) && agents_[1]->acting(now_));
    }
    EXPECT_FALSE(both_acting);
    EXPECT_FALSE(agents_[0]->acting(now_));
    EXPECT_TRUE(agents_[1]->acting(now_));
    EXPECT_EQ(agents_[1]->epoch(), 2);
}

TEST_F(PairLeaseAgentTest, WithTheArbiterAndTheLeaderGoneTheFollowerReportsThatTheGroupHasNoLeader) {
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    arbiter_up_ = false;
    outboxes_[0].connected_to_peer = false;
    outboxes_[0].connected_to_third_voter = false;
    outboxes_[1].connected_to_peer = false;
    for (int i = 0; i < 200; ++i) {
        step();
    }
    EXPECT_FALSE(agents_[1]->acting(now_));
    EXPECT_TRUE(log_contains("this group has no leader"));
    int reports = 0;
    for (const auto& line : log_) {
        reports += line.find("this group has no leader") != std::string::npos ? 1 : 0;
    }
    // Once from each instance, not once per attempt: the follower, and the former leader, which is
    // cut off and asks again too. Both log under the same name here.
    EXPECT_EQ(reports, 2);
}

TEST_F(PairLeaseAgentTest, APromiseIsRecordedBeforeTheGrantIsSent) {
    keep_promises();
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    // The follower promised its vote to the leader, and the record covers that promise.
    EXPECT_EQ(kept_[1].promised_to, 1);
    EXPECT_GT(kept_[1].until, now_);
    // The leader recorded that its vote is its own until beyond the end of its lease.
    EXPECT_EQ(kept_[0].promised_to, 1);
    EXPECT_GT(kept_[0].until, now_);
}

TEST_F(PairLeaseAgentTest, APromiseThatCannotBeRecordedIsNotMade) {
    keep_promises();
    kept_[1].fail = true;
    for (int i = 0; i < 40; ++i) {
        step();
    }
    // The follower refused rather than making a promise it could not keep across a restart, so the
    // primary leads on the arbiter's grant alone.
    EXPECT_TRUE(agents_[0]->acting(now_));
    EXPECT_TRUE(log_contains("could not record the promise"));
}

// In the two tests below the leader's process is down for 1.5 seconds before its supervisor starts it
// again: long enough to be a real restart, and shorter than the 3 second lease period.
TEST_F(PairLeaseAgentTest, ALeaderRestartedWithItsRecordKeepsTheLead) {
    keep_promises();
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    crash(0);
    for (int i = 0; i < 15; ++i) {
        step();
    }
    restart(0, true);
    for (int i = 0; i < 100; ++i) {
        step();
        ASSERT_FALSE(agents_[1]->acting(now_)) << "the peer took over at step " << i;
    }
    EXPECT_TRUE(agents_[0]->acting(now_));
    EXPECT_TRUE(log_contains("this instance was leading, so it asks to lead again at once"));
}

TEST_F(PairLeaseAgentTest, ALeaderRestartedWithoutARecordIsOvertaken) {
    // The case the record exists for, shown failing without it: a restarted instance that has
    // forgotten its promises waits a lease period, and its peer takes over first.
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    crash(0);
    for (int i = 0; i < 15; ++i) {
        step();
    }
    restart(0, false);
    for (int i = 0; i < 100; ++i) {
        step();
    }
    EXPECT_TRUE(agents_[1]->acting(now_));
    EXPECT_FALSE(agents_[0]->acting(now_));
}

TEST_F(PairLeaseAgentTest, ALeadingSecondaryRestartedWithItsRecordKeepsTheLead) {
    keep_promises();
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    // Move the lead to the secondary: cut the primary off from both voters until its lease runs out.
    outboxes_[0].connected_to_peer = false;
    outboxes_[0].connected_to_third_voter = false;
    for (int i = 0; i < 80; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[1]->acting(now_));
    outboxes_[0].connected_to_peer = true;
    outboxes_[0].connected_to_third_voter = true;
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[1]->acting(now_));

    crash(1);
    for (int i = 0; i < 15; ++i) {
        step();
    }
    restart(1, true);
    for (int i = 0; i < 100; ++i) {
        step();
        ASSERT_FALSE(agents_[0]->acting(now_)) << "the primary took over at step " << i;
    }
    EXPECT_TRUE(agents_[1]->acting(now_));
}

TEST_F(PairLeaseAgentTest, AFormerLeaderThatGrantsItsPeerRecordsThePeer) {
    keep_promises();
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    ASSERT_EQ(kept_[0].promised_to, 1);
    outboxes_[0].connected_to_peer = false;
    outboxes_[0].connected_to_third_voter = false;
    for (int i = 0; i < 80 && !agents_[1]->acting(now_); ++i) {
        step();
    }
    ASSERT_TRUE(agents_[1]->acting(now_));
    outboxes_[0].connected_to_peer = true;
    outboxes_[0].connected_to_third_voter = true;
    // Step only until this instance first grants its peer a renewal, while the old record still names
    // this instance until a time beyond the new promise. It grants nothing for a while: it asked to
    // lead while cut off, and a candidate votes for itself until its request lapses.
    const Clock::time_point recorded_self_until = kept_[0].until;
    changes_[0].clear();
    for (int i = 0; i < 150 && changes_[0].empty(); ++i) {
        step();
    }
    ASSERT_FALSE(changes_[0].empty()) << "this instance granted its peer nothing";
    ASSERT_EQ(changes_[0].front(), fix_common::PairLeaseAgent::Change::AgreedPeerLeads);
    ASSERT_LT(now_ + timing_.period, recorded_self_until) << "the old record no longer covers the new promise, so this test proves nothing";
    // The record said this instance's vote was its own, until well after the peer began asking for it.
    // Granting the peer must replace that, or a restart would read the old record and ask to lead.
    EXPECT_EQ(kept_[0].promised_to, 2);
    EXPECT_GT(kept_[0].until, now_);
}

TEST_F(PairLeaseAgentTest, ALeaderRestartedWithItsRecordButNoEpochKeepsTheLead) {
    keep_promises();
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    // Move the lead to the secondary and back, so that the epoch is well above the one a restarted
    // instance with no epoch asks for, and that request is refused as behind.
    for (int j : {0, 1}) {
        outboxes_[j].connected_to_peer = false;
        outboxes_[j].connected_to_third_voter = false;
        for (int i = 0; i < 80 && !agents_[1 - j]->acting(now_); ++i) {
            step();
        }
        ASSERT_TRUE(agents_[1 - j]->acting(now_));
        outboxes_[j].connected_to_peer = true;
        outboxes_[j].connected_to_third_voter = true;
        for (int i = 0; i < 40; ++i) {
            step();
        }
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    const int32_t epoch_before = agents_[0]->epoch();
    ASSERT_GT(epoch_before, 4);
    // Restart it late: a quarter of a second before its lease runs out as it counts it, which leaves
    // about half a second before the voters' promises to it run out. That is less than the shortest
    // random wait after a refusal, so only an instance that asks again at once keeps the lead.
    const Clock::time_point lease_end = agents_[0]->lease_expires_at();
    crash(0);
    while (now_ < lease_end - milliseconds{250}) {
        step();
    }
    restart(0, true, false);
    for (int i = 0; i < 100; ++i) {
        step();
        ASSERT_FALSE(agents_[1]->acting(now_)) << "the peer took over at step " << i;
    }
    EXPECT_TRUE(agents_[0]->acting(now_));
    EXPECT_GT(agents_[0]->epoch(), epoch_before);
}

TEST_F(PairLeaseAgentTest, AFormerLeaderRestartedWithItsRecordDoesNotDisturbAPeerThatTookOver) {
    keep_promises();
    for (int i = 0; i < 40; ++i) {
        step();
    }
    ASSERT_TRUE(agents_[0]->acting(now_));
    // The leader dies and the peer takes over with the arbiter's vote.
    crash(0);
    for (int i = 0; i < 80 && !agents_[1]->acting(now_); ++i) {
        step();
    }
    ASSERT_TRUE(agents_[1]->acting(now_));
    // The arbiter goes, so the peer can now renew only with the restarted instance's vote. The
    // restarted instance's record says it was leading, and still covers the present.
    arbiter_up_ = false;
    ASSERT_EQ(kept_[0].promised_to, 1);
    ASSERT_GT(kept_[0].until, now_);
    restart(0, true);
    for (int i = 0; i < 100; ++i) {
        step();
        ASSERT_TRUE(agents_[1]->acting(now_)) << "the peer lost its lead at step " << i;
    }
    EXPECT_FALSE(agents_[0]->acting(now_));
}
