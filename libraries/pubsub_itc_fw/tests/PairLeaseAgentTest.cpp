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

#include <lease_messages.hpp>
#include <pubsub_itc_fw/BackgroundPromiseRecorder.hpp>
#include <pubsub_itc_fw/BackgroundPromiseRecorderInterface.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LeaderStatement.hpp>
#include <pubsub_itc_fw/LeaseLinksInterface.hpp>
#include <pubsub_itc_fw/LeasePromiseRecord.hpp>
#include <pubsub_itc_fw/LeasePromiseRecorderInterface.hpp>
#include <pubsub_itc_fw/LeasePromiseStore.hpp>
#include <pubsub_itc_fw/LeaseTiming.hpp>
#include <pubsub_itc_fw/LeaseVoter.hpp>
#include <pubsub_itc_fw/PairLeaseAgent.hpp>
#include <pubsub_itc_fw/PeerStatementsFlag.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr int64_t arbiter_id = 3;

// The id of the secondary instance, the peer to which the primary sends its requests.
constexpr int64_t request_peer_id = 2;

// The number the agents under test put on their lease messages for their pair. Any number will do.
constexpr int32_t test_pair_id = 1;

// What an agent sent, held until the test delivers it.
struct Outbox : public pubsub_itc_fw::LeaseLinksInterface {
    std::vector<pubsub_itc_fw::LeaseRequest> to_peer;
    std::vector<pubsub_itc_fw::LeaseRequest> to_third_voter;
    std::vector<pubsub_itc_fw::LeaseGrant> grants;
    std::vector<pubsub_itc_fw::LeaseRefusal> refusals;
    bool connected_to_peer{true};
    bool connected_to_third_voter{true};

    void send_request_to_peer(const pubsub_itc_fw::LeaseRequest& request) override {
        if (connected_to_peer) {
            to_peer.push_back(request);
        }
    }
    void send_request_to_third_voter(const pubsub_itc_fw::LeaseRequest& request) override {
        if (connected_to_third_voter) {
            to_third_voter.push_back(request);
        }
    }
    void send_grant(const pubsub_itc_fw::ConnectionID& /*conn_id*/, const pubsub_itc_fw::LeaseGrant& grant) override {
        grants.push_back(grant);
    }
    void send_refusal(const pubsub_itc_fw::ConnectionID& /*conn_id*/, const pubsub_itc_fw::LeaseRefusal& refusal) override {
        refusals.push_back(refusal);
    }
};

// Keeps promises in memory, as a disk would across a process restart. With a delay, each record takes
// that long to write, as on a disk whose sync is slow.
struct KeptPromises : public pubsub_itc_fw::LeasePromiseRecorderInterface {
    int64_t promised_to{0};
    Clock::time_point until{};
    pubsub_itc_fw::LeaderStatement statement{};
    bool recorded_anything{false};
    bool fail{false};
    std::chrono::milliseconds delay{0};
    std::atomic<int> records_written{0};

    bool record(const pubsub_itc_fw::LeasePromiseRecord& record) override {
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        if (fail) {
            return false;
        }
        promised_to = record.promised_to;
        until = record.until;
        statement = record.statement;
        recorded_anything = true;
        // Last, so that a test waiting for this count knows the record is complete and the store no longer in use.
        ++records_written;
        return true;
    }

    std::optional<pubsub_itc_fw::LeasePromiseStore::Record> as_record() const {
        if (!recorded_anything) {
            return std::nullopt;
        }
        return pubsub_itc_fw::LeasePromiseStore::Record{promised_to, until, statement};
    }
};

// Writes each record into a KeptPromises as soon as the write is started, on the calling thread, and
// reports the outcome the next time it is asked. To the agent this looks like a background write that
// has always finished by its next look, which keeps the tests that use it deterministic. With a delay
// in the KeptPromises, it is instead a recorder that holds up the calling thread.
struct RecordsAtOnce : public pubsub_itc_fw::BackgroundPromiseRecorderInterface {
    explicit RecordsAtOnce(KeptPromises& kept) : kept_(kept) {}

    bool record_in_background(const pubsub_itc_fw::LeasePromiseRecord& record) override {
        if (outcome_.has_value()) {
            return false;
        }
        outcome_ = kept_.record(record);
        return true;
    }

    std::optional<bool> background_result() override {
        const std::optional<bool> outcome = outcome_;
        outcome_.reset();
        return outcome;
    }

    // Forget a write whose outcome was not read, as a process that dies forgets it.
    void forget() {
        outcome_.reset();
    }

  private:
    KeptPromises& kept_;
    std::optional<bool> outcome_;
};

// How long the slow store in the tests below takes to write each record, as a disk whose sync has stopped
// answering for a while.
constexpr std::chrono::milliseconds slow_store_delay{2000};

// Waits up to @p limit for the recorder to report the outcome of its write.
std::optional<bool> wait_for_outcome(pubsub_itc_fw::BackgroundPromiseRecorder& recorder, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::optional<bool> outcome;
    while (!outcome.has_value() && std::chrono::steady_clock::now() < deadline) {
        outcome = recorder.background_result();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return outcome;
}

// Waits up to @p limit for @p store to have written @p count records, so that a store a writer thread may
// still be using does not go out of scope under it.
void wait_for_records(const KeptPromises& store, int count, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (store.records_written < count && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
}

class PairLeaseAgentTest : public ::testing::Test {
  protected:
    PairLeaseAgentTest()
        : logger_(pubsub_itc_fw::FwLogLevel::Info, [this](const std::string& record) { log_.push_back(record); }), arbiter_(timing_.period, start_, 0) {
        for (int i = 0; i < 2; ++i) {
            agents_[i] = std::make_unique<pubsub_itc_fw::PairLeaseAgent>("TestThread", logger_, outboxes_[i], test_pair_id, i + 1, i == 0 ? 2 : 1, arbiter_id,
                                                                         "the arbiter", timing_, start_, 0, peer_statements_);
        }
    }

    // Before the first step: make both instances' leaders say whether their peer may lead, as the sequencer pair's do.
    void make_statements() {
        peer_statements_ = pubsub_itc_fw::PeerStatementsFlag{pubsub_itc_fw::PeerStatementsFlag::SayWhetherPeerMayLead};
        for (int i = 0; i < 2; ++i) {
            agents_[i] = std::make_unique<pubsub_itc_fw::PairLeaseAgent>("TestThread", logger_, outboxes_[i], test_pair_id, i + 1, i == 0 ? 2 : 1, arbiter_id,
                                                                         "the arbiter", timing_, start_, 0, peer_statements_);
        }
    }

    // One tick for both agents, then every message delivered at once, until nothing is left to deliver.
    void step() {
        now_ += pubsub_itc_fw::LeaseTiming::tick_interval;
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
                    if (answer.verdict == pubsub_itc_fw::LeaseVoter::Verdict::Granted) {
                        int64_t echoed = 0;
                        const pubsub_itc_fw::LeaderStatement statement = statement_on(request);
                        if (statement.leader_id != 0) {
                            arbiter_.record_statement(statement);
                            echoed = arbiter_.holds_statement(statement) ? statement.number : 0;
                        }
                        record(i, agents_[i]->on_grant(arbiter_id, request.epoch, request.request_id, echoed, now_));
                    } else {
                        record(i, agents_[i]->on_refusal(arbiter_id, answer.highest_epoch, request.request_id,
                                                         pubsub_itc_fw::LeaseRefusalReason::promised_elsewhere, now_));
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

    static pubsub_itc_fw::LeaderStatement statement_on(const pubsub_itc_fw::LeaseRequest& request) {
        return pubsub_itc_fw::LeaderStatement{request.statement_leader_id, request.statement_epoch, request.statement_number, request.peer_may_lead};
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
        agents_[i] =
            std::make_unique<pubsub_itc_fw::PairLeaseAgent>("TestThread", logger_, outboxes_[i], test_pair_id, i + 1, i == 0 ? 2 : 1, arbiter_id, "the arbiter",
                                                            timing_, now_, keep_epoch ? agents_[i]->highest_epoch() : 0, peer_statements_);
        if (keep_promises) {
            at_once_[i].forget();
            agents_[i]->keep_promises_in(at_once_[i], kept_[i].as_record(), kept_[i].statement, now_);
        }
        changes_[i].clear();
    }

    void keep_promises() {
        for (int i = 0; i < 2; ++i) {
            agents_[i]->keep_promises_in(at_once_[i], std::nullopt, pubsub_itc_fw::LeaderStatement{}, now_);
        }
    }

    void record(int i, pubsub_itc_fw::PairLeaseAgent::Change change) {
        if (change != pubsub_itc_fw::PairLeaseAgent::Change::Nothing) {
            changes_[i].push_back(change);
        }
    }

    // Flushes first, because the logger delivers records on its own thread after the log macro returns.
    bool log_contains(const std::string& text) {
        logger_.flush();
        for (const auto& line : log_) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    pubsub_itc_fw::LeaseTiming timing_{};
    pubsub_itc_fw::PeerStatementsFlag peer_statements_{pubsub_itc_fw::PeerStatementsFlag::PeerAlwaysMayLead};
    const Clock::time_point start_{};
    Clock::time_point now_{};
    std::vector<std::string> log_;
    pubsub_itc_fw::QuillLogger logger_;
    Outbox outboxes_[2];
    std::unique_ptr<pubsub_itc_fw::PairLeaseAgent> agents_[2];
    pubsub_itc_fw::LeaseVoter arbiter_;
    bool arbiter_up_{true};
    std::vector<pubsub_itc_fw::PairLeaseAgent::Change> changes_[2];
    KeptPromises kept_[2];
    RecordsAtOnce at_once_[2]{RecordsAtOnce{kept_[0]}, RecordsAtOnce{kept_[1]}};
    bool down_[2]{false, false};
};

} // un-named namespace

TEST_F(PairLeaseAgentTest, NobodyAsksDuringTheFirstLeasePeriod) {
    while (now_ < start_ + timing_.period - pubsub_itc_fw::LeaseTiming::tick_interval) {
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
    EXPECT_EQ(changes_[0].front(), pubsub_itc_fw::PairLeaseAgent::Change::BecameLeader);
    ASSERT_FALSE(changes_[1].empty());
    EXPECT_EQ(changes_[1].front(), pubsub_itc_fw::PairLeaseAgent::Change::AgreedPeerLeads);
    EXPECT_TRUE(log_contains("TestThread: leading at epoch 1 -- granted a lease by"));
}

TEST_F(PairLeaseAgentTest, ALeaderKeepsLeadingWhileItRenews) {
    for (int i = 0; i < 400; ++i) {
        step();
    }
    EXPECT_TRUE(agents_[0]->acting(now_));
    EXPECT_EQ(std::count(changes_[0].begin(), changes_[0].end(), pubsub_itc_fw::PairLeaseAgent::Change::BecameLeader), 1);
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
    EXPECT_EQ(changes_[0].back(), pubsub_itc_fw::PairLeaseAgent::Change::StoppedLeading);
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
    EXPECT_TRUE(log_contains("could not write the lease promise record"));
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
    ASSERT_EQ(changes_[0].front(), pubsub_itc_fw::PairLeaseAgent::Change::AgreedPeerLeads);
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

// With the arbiter down, a former leader restarted with a record saying it led asks to lead again at
// once, and as a candidate refuses its peer's renewals for a lease period, so the peer's lease runs out.
// The peer asks to lead again at once, and the restarted instance gives way. Its record still names
// itself, so it first refuses with recording_promise and writes a record naming the peer, and the peer,
// asking again at its next renewal interval, is granted. The pair is without a leader for about one
// renewal interval, never has two, and ends with the peer leading.
TEST_F(PairLeaseAgentTest, WithTheArbiterDownARestartedFormerLeaderLeavesThePeerLeadingAfterAShortGap) {
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
    Clock::duration longest_gap{};
    Clock::time_point gap_started{};
    bool in_gap = false;
    for (int i = 0; i < 100; ++i) {
        step();
        ASSERT_FALSE(agents_[0]->acting(now_) && agents_[1]->acting(now_)) << "both instances acting at step " << i;
        if (!agents_[1]->acting(now_) && !in_gap) {
            in_gap = true;
            gap_started = now_;
        } else if (agents_[1]->acting(now_) && in_gap) {
            in_gap = false;
            longest_gap = std::max(longest_gap, now_ - gap_started);
        }
    }
    EXPECT_TRUE(agents_[1]->acting(now_));
    EXPECT_FALSE(agents_[0]->acting(now_));
    EXPECT_LE(longest_gap, timing_.renewal_interval + pubsub_itc_fw::LeaseTiming::tick_interval) << "the pair was without a leader for too long";
    EXPECT_EQ(kept_[0].promised_to, 2) << "the restarted instance's record does not name the peer it grants";
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
        pubsub_itc_fw::BackgroundPromiseRecorder background_0(kept_[0]);
        pubsub_itc_fw::BackgroundPromiseRecorder background_1(kept_[1]);
        agents_[0]->keep_promises_in(background_0, std::nullopt, pubsub_itc_fw::LeaderStatement{}, now_);
        agents_[1]->keep_promises_in(background_1, std::nullopt, pubsub_itc_fw::LeaderStatement{}, now_);
        // Long enough for the first records to be written, and the primary to lead.
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
    EXPECT_EQ(std::count(changes_[0].begin(), changes_[0].end(), pubsub_itc_fw::PairLeaseAgent::Change::BecameLeader), 1);
}

// The same slow record, written by a recorder that writes on the calling thread, does hold up the lease
// thread. This is what makes the test above mean something: its measure of the longest step does see a
// write that blocks.
TEST_F(PairLeaseAgentTest, ARecorderThatWritesOnTheCallingThreadHoldsUpTheLeaseThread) {
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
        now_ += pubsub_itc_fw::LeaseTiming::tick_interval;
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
        now_ += pubsub_itc_fw::LeaseTiming::tick_interval;
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
    // A new leader sends its first statement as it takes the lead, not at its first renewal, so in
    // this harness, which delivers every message at once, the echo is already back.
    EXPECT_TRUE(agents_[0]->may_act_without_peer()) << "the statement sent on taking the lead was not echoed without waiting for a renewal";
    agents_[0]->peer_holds_everything(now_);
    EXPECT_FALSE(agents_[0]->may_act_without_peer());
    agents_[0]->begin_running_without_peer(now_);
    EXPECT_FALSE(agents_[0]->may_act_without_peer()) << "the new statement has not been echoed";
    deliver();
    // The requests sent at once carried the new statement, and were granted, so it has been echoed.
    // The leader's own record of it is written in the background. It is known to be on disk at a later
    // tick: the next one, or the one after if another write was in progress when the statement changed.
    EXPECT_FALSE(agents_[0]->may_act_without_peer()) << "the leader acted on its statement before learning that it was on disk";
    int ticks = 0;
    for (; ticks < 5 && !agents_[0]->may_act_without_peer(); ++ticks) {
        step();
    }
    EXPECT_TRUE(agents_[0]->may_act_without_peer()) << "the statement was echoed, and still the leader may not act after five ticks";
    EXPECT_LE(ticks, 2);
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
    record(0, agents_[0]->on_request(pubsub_itc_fw::ConnectionID{}, 2, 1000, 99, pubsub_itc_fw::LeaderStatement{}, now_));
    EXPECT_TRUE(outboxes_[0].grants.empty()) << "the restarted leader granted a peer its record says may not lead";
    ASSERT_EQ(outboxes_[0].refusals.size(), 1U);
    EXPECT_EQ(outboxes_[0].refusals.front().reason, pubsub_itc_fw::LeaseRefusalReason::may_not_lead);
}

// The first request from the peer, which the record on disk does not cover, is refused with
// recording_promise, and the write that covers it is started. Neither answering the request nor ticking
// waits for the store, which takes two seconds. Once the write has finished, the peer's next request is
// granted, and the record on disk then names the peer.
TEST_F(PairLeaseAgentTest, APromiseNotYetOnDiskIsRefusedAndGrantedOnceTheSlowWriteFinishes) {
    kept_[1].delay = slow_store_delay;
    pubsub_itc_fw::BackgroundPromiseRecorder background(kept_[1]);
    agents_[1]->keep_promises_in(background, std::nullopt, pubsub_itc_fw::LeaderStatement{}, now_);
    // Past the first lease period, during which a voter grants nothing.
    now_ = start_ + timing_.period + milliseconds{500};

    const auto started = std::chrono::steady_clock::now();
    record(1, agents_[1]->on_request(pubsub_itc_fw::ConnectionID{}, 1, 1, 1, pubsub_itc_fw::LeaderStatement{}, now_));
    record(1, agents_[1]->on_tick(now_));
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{100}) << "the lease thread waited for the store";
    ASSERT_EQ(outboxes_[1].refusals.size(), 1U);
    EXPECT_TRUE(outboxes_[1].grants.empty());
    EXPECT_EQ(outboxes_[1].refusals.front().reason, pubsub_itc_fw::LeaseRefusalReason::recording_promise);

    // While the write is in progress, the peer's requests go on being refused, without waiting.
    outboxes_[1] = Outbox{};
    const auto asked_again = std::chrono::steady_clock::now();
    record(1, agents_[1]->on_request(pubsub_itc_fw::ConnectionID{}, 1, 1, 2, pubsub_itc_fw::LeaderStatement{}, now_));
    EXPECT_LT(std::chrono::steady_clock::now() - asked_again, std::chrono::milliseconds{100}) << "the lease thread waited for the store";
    ASSERT_EQ(outboxes_[1].refusals.size(), 1U);
    EXPECT_EQ(outboxes_[1].refusals.front().reason, pubsub_itc_fw::LeaseRefusalReason::recording_promise);

    wait_for_records(kept_[1], 1, slow_store_delay * 2);
    ASSERT_EQ(kept_[1].records_written, 1);
    // The outcome is read at a tick. The writer thread stores it just after the store returns.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    now_ += pubsub_itc_fw::LeaseTiming::tick_interval;
    record(1, agents_[1]->on_tick(now_));
    outboxes_[1] = Outbox{};
    record(1, agents_[1]->on_request(pubsub_itc_fw::ConnectionID{}, 1, 1, 3, pubsub_itc_fw::LeaderStatement{}, now_));
    EXPECT_TRUE(outboxes_[1].refusals.empty());
    ASSERT_EQ(outboxes_[1].grants.size(), 1U) << "the request was not granted once the promise was on disk";
    EXPECT_EQ(kept_[1].promised_to, 1);
    EXPECT_GT(kept_[1].until, now_ + timing_.period);
}

// A candidate the peer refuses with recording_promise asks the peer again at its next renewal interval,
// rather than after the lease period it waits after any other refusal.
TEST_F(PairLeaseAgentTest, ACandidateAsksAgainAtTheNextRenewalIntervalAfterRecordingPromise) {
    for (int i = 0; i < 200 && outboxes_[0].to_peer.empty(); ++i) {
        now_ += pubsub_itc_fw::LeaseTiming::tick_interval;
        record(0, agents_[0]->on_tick(now_));
    }
    ASSERT_EQ(outboxes_[0].to_peer.size(), 1U) << "the primary never asked to lead";
    const pubsub_itc_fw::LeaseRequest request = outboxes_[0].to_peer.front();
    const Clock::time_point asked_at = now_;
    outboxes_[0] = Outbox{};
    record(0, agents_[0]->on_refusal(request_peer_id, request.epoch, request.request_id, pubsub_itc_fw::LeaseRefusalReason::recording_promise, now_));
    while (outboxes_[0].to_peer.empty() && now_ < asked_at + timing_.period) {
        now_ += pubsub_itc_fw::LeaseTiming::tick_interval;
        record(0, agents_[0]->on_tick(now_));
    }
    ASSERT_EQ(outboxes_[0].to_peer.size(), 1U) << "the candidate did not ask again within the lease period";
    EXPECT_EQ(now_ - asked_at, timing_.renewal_interval) << "the candidate did not ask again at its next renewal interval";
    EXPECT_EQ(outboxes_[0].to_peer.front().epoch, request.epoch);
}

// The control for the test above: after any other refusal the candidate does not ask again within the
// renewal interval, so the test above does see the quicker retry.
TEST_F(PairLeaseAgentTest, ACandidateRefusedForAnyOtherReasonDoesNotAskAgainAtTheNextRenewalInterval) {
    for (int i = 0; i < 200 && outboxes_[0].to_peer.empty(); ++i) {
        now_ += pubsub_itc_fw::LeaseTiming::tick_interval;
        record(0, agents_[0]->on_tick(now_));
    }
    ASSERT_EQ(outboxes_[0].to_peer.size(), 1U) << "the primary never asked to lead";
    const pubsub_itc_fw::LeaseRequest request = outboxes_[0].to_peer.front();
    const Clock::time_point asked_at = now_;
    outboxes_[0] = Outbox{};
    record(0, agents_[0]->on_refusal(request_peer_id, request.epoch, request.request_id, pubsub_itc_fw::LeaseRefusalReason::promised_elsewhere, now_));
    while (now_ < asked_at + timing_.renewal_interval * 2) {
        now_ += pubsub_itc_fw::LeaseTiming::tick_interval;
        record(0, agents_[0]->on_tick(now_));
    }
    EXPECT_TRUE(outboxes_[0].to_peer.empty()) << "the candidate asked again within two renewal intervals after an ordinary refusal";
}

// While the store takes two seconds to write, starting a write, reading its outcome, and trying to start a
// second write all return at once. The outcome is reported once, after the write has finished, and only
// then may another write start.
TEST(BackgroundPromiseRecorderTest, NothingTheLeaseThreadCallsWaitsForTheStore) {
    KeptPromises store;
    store.delay = slow_store_delay;
    pubsub_itc_fw::BackgroundPromiseRecorder recorder(store);
    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(recorder.record_in_background(pubsub_itc_fw::LeasePromiseRecord{2, Clock::time_point{std::chrono::seconds{10}}, {}}));
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    EXPECT_FALSE(recorder.background_result().has_value()) << "an outcome was reported before the store had finished writing";
    EXPECT_FALSE(recorder.record_in_background(pubsub_itc_fw::LeasePromiseRecord{2, Clock::time_point{std::chrono::seconds{11}}, {}}))
        << "a second write was started while the first was in progress";
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds{500}) << "a call from the lease thread waited for the store";

    const std::optional<bool> outcome = wait_for_outcome(recorder, slow_store_delay * 2);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(*outcome);
    EXPECT_GE(std::chrono::steady_clock::now() - started, slow_store_delay) << "the outcome was reported before the store had finished";
    EXPECT_FALSE(recorder.background_result().has_value()) << "the outcome was reported twice";
    EXPECT_EQ(store.records_written, 1);
    EXPECT_EQ(store.until, Clock::time_point{std::chrono::seconds{10}});

    store.delay = std::chrono::milliseconds{0};
    ASSERT_TRUE(recorder.record_in_background(pubsub_itc_fw::LeasePromiseRecord{1, Clock::time_point{std::chrono::seconds{20}}, {}}))
        << "no write could be started after the first had finished";
    ASSERT_TRUE(wait_for_outcome(recorder, std::chrono::milliseconds{1000}).value_or(false));
    EXPECT_EQ(store.promised_to, 1);
    EXPECT_EQ(store.records_written, 2);
}

// Destroying the recorder while its write is stuck on the store returns after the one second it waits,
// not when the write finishes.
TEST(BackgroundPromiseRecorderTest, DestroyingTheRecorderDoesNotWaitForAStuckWrite) {
    KeptPromises store;
    store.delay = slow_store_delay * 2;
    std::chrono::steady_clock::duration destruction_took{};
    {
        auto recorder = std::make_unique<pubsub_itc_fw::BackgroundPromiseRecorder>(store);
        ASSERT_TRUE(recorder->record_in_background(pubsub_itc_fw::LeasePromiseRecord{1, Clock::time_point{std::chrono::seconds{10}}, {}}));
        // Give the writer thread time to take the record and start writing it.
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        const auto started = std::chrono::steady_clock::now();
        recorder.reset();
        destruction_took = std::chrono::steady_clock::now() - started;
    }
    EXPECT_GE(destruction_took, std::chrono::milliseconds{900}) << "the destructor did not wait for the write at all";
    EXPECT_LT(destruction_took, std::chrono::milliseconds{1500}) << "the destructor waited for the stuck write";
    // The writer thread is still writing into the store. It must finish before the store goes.
    wait_for_records(store, 1, slow_store_delay * 3);
    EXPECT_EQ(store.records_written, 1);
}

// A background write that fails is reported as failed, once.
TEST(BackgroundPromiseRecorderTest, AFailedBackgroundWriteIsReportedOnce) {
    KeptPromises store;
    store.fail = true;
    pubsub_itc_fw::BackgroundPromiseRecorder recorder(store);
    ASSERT_TRUE(recorder.record_in_background(pubsub_itc_fw::LeasePromiseRecord{1, Clock::time_point{std::chrono::seconds{10}}, {}}));
    std::optional<bool> outcome;
    for (int i = 0; i < 200 && !outcome.has_value(); ++i) {
        outcome = recorder.background_result();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(*outcome);
    EXPECT_FALSE(recorder.background_result().has_value());
}
