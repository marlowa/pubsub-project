// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <BackgroundPromiseRecorder.hpp>
#include <LeaderStatement.hpp>
#include <LeaseLinksInterface.hpp>
#include <LeasePromiseRecord.hpp>
#include <LeasePromiseRecorderInterface.hpp>
#include <LeasePromiseStore.hpp>
#include <LeaseTiming.hpp>
#include <LeaseVoter.hpp>
#include <PairLeaseAgent.hpp>
#include <PeerStatementsFlag.hpp>
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

// Keeps promises in memory, as a disk would across a process restart. With a delay, each record takes
// that long to write, as on a disk whose sync is slow.
struct KeptPromises : public fix_common::LeasePromiseRecorderInterface {
    int64_t promised_to{0};
    Clock::time_point until{};
    fix_common::LeaderStatement statement{};
    bool recorded_anything{false};
    bool fail{false};
    std::chrono::milliseconds delay{0};
    std::atomic<int> records_written{0};

    bool record(const fix_common::LeasePromiseRecord& record) override {
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        if (fail) {
            return false;
        }
        ++records_written;
        promised_to = record.promised_to;
        until = record.until;
        statement = record.statement;
        recorded_anything = true;
        return true;
    }

    std::optional<fix_common::LeasePromiseStore::Record> as_record() const {
        if (!recorded_anything) {
            return std::nullopt;
        }
        return fix_common::LeasePromiseStore::Record{promised_to, until, statement};
    }
};

class PairLeaseAgentTest : public ::testing::Test {
  protected:
    PairLeaseAgentTest()
        : logger_(pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& record) { log_.push_back(record); }), arbiter_(timing_.period, start_, 0) {
        for (int i = 0; i < 2; ++i) {
            agents_[i] = std::make_unique<fix_common::PairLeaseAgent>("TestThread", logger_, outboxes_[i], pubsub_itc_fw_app::ComponentGroup::sequencer, i + 1,
                                                                      i == 0 ? 2 : 1, arbiter_id, "the arbiter", timing_, start_, 0, peer_statements_);
        }
    }

    // Before the first step: make both instances' leaders say whether their peer may lead, as the sequencer pair's do.
    void make_statements() {
        peer_statements_ = fix_common::PeerStatementsFlag{fix_common::PeerStatementsFlag::SayWhetherPeerMayLead};
        for (int i = 0; i < 2; ++i) {
            agents_[i] = std::make_unique<fix_common::PairLeaseAgent>("TestThread", logger_, outboxes_[i], pubsub_itc_fw_app::ComponentGroup::sequencer, i + 1,
                                                                      i == 0 ? 2 : 1, arbiter_id, "the arbiter", timing_, start_, 0, peer_statements_);
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
                    record(peer, agents_[peer]->on_request(pubsub_itc_fw::ConnectionID{}, request.candidate_instance_id, request.epoch, request.request_id,
                                                           statement_on(request), now_));
                    for (const auto& grant : outboxes_[peer].grants) {
                        record(i, agents_[i]->on_grant(grant.voter_instance_id, grant.epoch, grant.request_id, grant.echoed_statement_number, now_));
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
                        int64_t echoed = 0;
                        const fix_common::LeaderStatement statement = statement_on(request);
                        if (statement.leader_id != 0) {
                            arbiter_.record_statement(statement);
                            echoed = arbiter_.holds_statement(statement) ? statement.number : 0;
                        }
                        record(i, agents_[i]->on_grant(arbiter_id, request.epoch, request.request_id, echoed, now_));
                    } else {
                        record(i, agents_[i]->on_refusal(arbiter_id, answer.highest_epoch, request.request_id,
                                                         pubsub_itc_fw_app::LeaseRefusalReason::promised_elsewhere, now_));
                    }
                }
            }
        }
    }

    // Steps until instance 1 leads, which with nothing failing it does first.
    bool step_until_the_primary_leads() {
        for (int i = 0; i < 80 && !agents_[0]->acting(now_); ++i) {
            step();
        }
        return agents_[0]->acting(now_);
    }

    static fix_common::LeaderStatement statement_on(const pubsub_itc_fw_app::LeaseRequest& request) {
        return fix_common::LeaderStatement{request.statement_leader_id, request.statement_epoch, request.statement_number, request.peer_may_lead};
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
                                                                  keep_epoch ? agents_[i]->highest_epoch() : 0, peer_statements_);
        if (keep_promises) {
            agents_[i]->keep_promises_in(kept_[i], kept_[i].as_record(), kept_[i].statement, now_);
        }
        changes_[i].clear();
    }

    void keep_promises() {
        for (int i = 0; i < 2; ++i) {
            agents_[i]->keep_promises_in(kept_[i], std::nullopt, fix_common::LeaderStatement{}, now_);
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
    fix_common::PeerStatementsFlag peer_statements_{fix_common::PeerStatementsFlag::PeerAlwaysMayLead};
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

// A record that takes 200 ms to write, as on a disk whose sync has slowed. Written in the background,
// it never holds up the thread handling leases, and the leader keeps its lease throughout (BUG-0107).
// Each step is paced in real time, so that background writes have real time in which to finish.
TEST_F(PairLeaseAgentTest, ASlowRecordWrittenInTheBackgroundDoesNotHoldUpTheLeaseThread) {
    kept_[0].delay = std::chrono::milliseconds{200};
    kept_[1].delay = std::chrono::milliseconds{200};
    int records_before_measuring = 0;
    std::chrono::steady_clock::duration longest_step{};
    {
        fix_common::BackgroundPromiseRecorder background_0(kept_[0]);
        fix_common::BackgroundPromiseRecorder background_1(kept_[1]);
        agents_[0]->keep_promises_in(background_0, std::nullopt, fix_common::LeaderStatement{}, now_);
        agents_[1]->keep_promises_in(background_1, std::nullopt, fix_common::LeaderStatement{}, now_);
        // The first records are written on the lease thread, because nothing is on disk yet.
        for (int i = 0; i < 60; ++i) {
            step();
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        ASSERT_TRUE(agents_[0]->acting(now_));
        records_before_measuring = kept_[0].records_written + kept_[1].records_written;
        for (int i = 0; i < 300; ++i) {
            const auto started = std::chrono::steady_clock::now();
            step();
            longest_step = std::max(longest_step, std::chrono::steady_clock::now() - started);
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        EXPECT_TRUE(agents_[0]->acting(now_));
    }
    EXPECT_LT(longest_step, std::chrono::milliseconds{100}) << "a step of the lease thread waited for a record to be written";
    EXPECT_GE(kept_[0].records_written + kept_[1].records_written - records_before_measuring, 4)
        << "the records were not refreshed in the background while the leader renewed";
    EXPECT_EQ(std::count(changes_[0].begin(), changes_[0].end(), fix_common::PairLeaseAgent::Change::BecameLeader), 1);
}

// The same slow record written on the lease thread does hold it up. This is what makes the test above
// mean something: its measure of the longest step does see a write that blocks.
TEST_F(PairLeaseAgentTest, WithoutABackgroundWriterASlowRecordHoldsUpTheLeaseThread) {
    kept_[0].delay = std::chrono::milliseconds{200};
    kept_[1].delay = std::chrono::milliseconds{200};
    keep_promises();
    for (int i = 0; i < 60; ++i) {
        step();
    }
    std::chrono::steady_clock::duration longest_step{};
    for (int i = 0; i < 300; ++i) {
        const auto started = std::chrono::steady_clock::now();
        step();
        longest_step = std::max(longest_step, std::chrono::steady_clock::now() - started);
    }
    EXPECT_GE(longest_step, std::chrono::milliseconds{150});
}

TEST_F(PairLeaseAgentTest, APairThatMakesNoStatementsSendsNone) {
    keep_promises();
    ASSERT_TRUE(step_until_the_primary_leads());
    for (int i = 0; i < 40; ++i) {
        now_ += fix_common::LeaseTiming::tick_interval;
        record(0, agents_[0]->on_tick(now_));
        for (const auto& request : outboxes_[0].to_peer) {
            EXPECT_EQ(request.statement_leader_id, 0);
        }
        deliver();
    }
    EXPECT_EQ(kept_[1].statement.leader_id, 0);
}

TEST_F(PairLeaseAgentTest, ALeaderSaysItsPeerMayNotLeadOnItsRequestsAndRecordsItFirst) {
    make_statements();
    keep_promises();
    ASSERT_TRUE(step_until_the_primary_leads());
    EXPECT_EQ(kept_[0].statement.instance_that_may_not_lead(), 2) << "the leader's own statement is not on disk";
    bool carried = false;
    for (int i = 0; i < 40 && !carried; ++i) {
        now_ += fix_common::LeaseTiming::tick_interval;
        record(0, agents_[0]->on_tick(now_));
        for (const auto& request : outboxes_[0].to_peer) {
            carried = request.statement_leader_id == 1 && !request.peer_may_lead;
        }
        deliver();
    }
    EXPECT_TRUE(carried) << "no renewal carried the statement";
}

TEST_F(PairLeaseAgentTest, AFollowerTheLeaderSaysMayNotLeadDoesNotTakeOverWhenTheLeaderDies) {
    // Nothing here says the follower has caught up, so the leader goes on saying that its peer may
    // not lead, as it does while it acts on commands the peer lacks. When the leader dies, neither the
    // follower nor the arbiter, both of which recorded that, lets the follower lead.
    make_statements();
    keep_promises();
    ASSERT_TRUE(step_until_the_primary_leads());
    for (int i = 0; i < 40; ++i) {
        step();
    }
    EXPECT_EQ(kept_[1].statement.instance_that_may_not_lead(), 2);
    EXPECT_TRUE(log_contains("says this instance may not lead"));
    crash(0);
    for (int i = 0; i < 200; ++i) {
        step();
        ASSERT_FALSE(agents_[1]->acting(now_)) << "the follower took the lead at step " << i;
    }
}

TEST_F(PairLeaseAgentTest, OnceThePeerHoldsEverythingItTakesOverWhenTheLeaderDies) {
    make_statements();
    keep_promises();
    ASSERT_TRUE(step_until_the_primary_leads());
    agents_[0]->peer_holds_everything(now_);
    deliver();
    for (int i = 0; i < 40; ++i) {
        step();
    }
    EXPECT_EQ(kept_[1].statement.instance_that_may_not_lead(), 0);
    crash(0);
    for (int i = 0; i < 200 && !agents_[1]->acting(now_); ++i) {
        step();
    }
    EXPECT_TRUE(agents_[1]->acting(now_));
}

TEST_F(PairLeaseAgentTest, ALeaderMayActWithoutItsPeerOnlyOnceAnotherVoterHasEchoedItsStatement) {
    make_statements();
    keep_promises();
    ASSERT_TRUE(step_until_the_primary_leads());
    EXPECT_FALSE(agents_[0]->may_act_without_peer()) << "no request has carried the statement yet";
    for (int i = 0; i < 40 && !agents_[0]->may_act_without_peer(); ++i) {
        step();
    }
    EXPECT_TRUE(agents_[0]->may_act_without_peer());
    agents_[0]->peer_holds_everything(now_);
    EXPECT_FALSE(agents_[0]->may_act_without_peer());
    agents_[0]->begin_running_without_peer(now_);
    EXPECT_FALSE(agents_[0]->may_act_without_peer()) << "the new statement has not been echoed";
    deliver();
    EXPECT_TRUE(agents_[0]->may_act_without_peer()) << "the requests sent at once carried the new statement, and were granted";
}

TEST_F(PairLeaseAgentTest, ALeaderWhoseOwnStatementCannotBeWrittenNeverActsWithoutItsPeer) {
    make_statements();
    keep_promises();
    ASSERT_TRUE(step_until_the_primary_leads());
    agents_[0]->peer_holds_everything(now_);
    deliver();
    kept_[0].fail = true;
    agents_[0]->begin_running_without_peer(now_);
    for (int i = 0; i < 40; ++i) {
        step();
        ASSERT_FALSE(agents_[0]->may_act_without_peer()) << "acting on a command the peer lacks, with the leader's own statement not on disk";
    }
}

TEST_F(PairLeaseAgentTest, ARestartedLeaderRefusesAPeerItsRecordSaysMayNotLead) {
    // The leader stays down until its record of leading has run out, so that after restarting it is not
    // resuming the lead, and so not voting for itself. Its peer's request then meets only the statement
    // the restarted leader took back from its record.
    make_statements();
    keep_promises();
    ASSERT_TRUE(step_until_the_primary_leads());
    arbiter_up_ = false;
    crash(0);
    for (int i = 0; i < 200; ++i) {
        step();
    }
    ASSERT_LT(kept_[0].until, now_) << "the record of leading has not run out, so this test proves nothing";
    restart(0, true);
    outboxes_[0].refusals.clear();
    outboxes_[0].grants.clear();
    record(0, agents_[0]->on_request(pubsub_itc_fw::ConnectionID{}, 2, 1000, 99, fix_common::LeaderStatement{}, now_));
    EXPECT_TRUE(outboxes_[0].grants.empty()) << "the restarted leader granted a peer its record says may not lead";
    ASSERT_EQ(outboxes_[0].refusals.size(), 1U);
    EXPECT_EQ(outboxes_[0].refusals.front().reason, pubsub_itc_fw_app::LeaseRefusalReason::may_not_lead);
}

// A record written on the lease thread while a background write is in progress waits for it, and the
// record it writes is the one left on disk; the background write's outcome is not reported afterwards.
TEST(BackgroundPromiseRecorderTest, ARecordWrittenAtOnceWaitsForTheBackgroundWriteAndReplacesIt) {
    KeptPromises store;
    store.delay = std::chrono::milliseconds{300};
    {
        fix_common::BackgroundPromiseRecorder recorder(store);
        ASSERT_TRUE(recorder.record_in_background(fix_common::LeasePromiseRecord{2, Clock::time_point{std::chrono::seconds{10}}, {}}));
        EXPECT_FALSE(recorder.record_in_background(fix_common::LeasePromiseRecord{2, Clock::time_point{std::chrono::seconds{11}}, {}}))
            << "a second background write was started";
        const auto started = std::chrono::steady_clock::now();
        ASSERT_TRUE(recorder.record(fix_common::LeasePromiseRecord{1, Clock::time_point{std::chrono::seconds{20}}, {}}));
        EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{500}) << "the write at once did not wait for the background one";
        EXPECT_FALSE(recorder.background_result().has_value());
    }
    EXPECT_EQ(store.promised_to, 1);
    EXPECT_EQ(store.until, Clock::time_point{std::chrono::seconds{20}});
    EXPECT_EQ(store.records_written, 2);
}

// A background write that fails is reported as failed, once.
TEST(BackgroundPromiseRecorderTest, AFailedBackgroundWriteIsReportedOnce) {
    KeptPromises store;
    store.fail = true;
    fix_common::BackgroundPromiseRecorder recorder(store);
    ASSERT_TRUE(recorder.record_in_background(fix_common::LeasePromiseRecord{1, Clock::time_point{std::chrono::seconds{10}}, {}}));
    std::optional<bool> outcome;
    for (int i = 0; i < 200 && !outcome.has_value(); ++i) {
        outcome = recorder.background_result();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(*outcome);
    EXPECT_FALSE(recorder.background_result().has_value());
}
