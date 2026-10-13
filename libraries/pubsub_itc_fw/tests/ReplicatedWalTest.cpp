// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file ReplicatedWalTest.cpp
 * @brief Tests of the library's replicated write-ahead log, with two instances joined in memory.
 *
 * Each instance has a WAL of its own in a scratch directory. The messages each sends are encoded, held,
 * and delivered to the other through the same handle_message() an application calls, so decoding is
 * exercised as well. A LeaseVoter stands in for the arbiter. Time is the test's own, so that a lease
 * period or a timeout passes in a step rather than in real time, and promises are kept in memory and
 * written at once.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/BackgroundPromiseRecorderInterface.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LeasePromiseRecorderInterface.hpp>
#include <pubsub_itc_fw/LeaseTiming.hpp>
#include <pubsub_itc_fw/LeaseVoter.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReplicatedWal.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/WalReader.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

#include <lease_messages.hpp>
#include <replication_messages.hpp>

namespace pubsub_itc_fw::tests {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int32_t test_pair_id = 1;
constexpr int64_t arbiter_id = 99;
constexpr size_t segment_size = 64 * 1024;
constexpr int16_t test_pdu_id = 1000;

// One message on its way, encoded as it would be on the wire.
struct Encoded {
    int16_t pdu_id{0};
    std::vector<uint8_t> bytes;
};

template <typename Message> Encoded encode_message(const Message& message) {
    size_t written = 0;
    size_t needed = 0;
    static_cast<void>(encode(message, nullptr, 0, written, needed));
    Encoded encoded;
    encoded.pdu_id = Message::message_pdu_id;
    encoded.bytes.resize(needed);
    EXPECT_TRUE(encode(message, encoded.bytes.data(), encoded.bytes.size(), written, needed));
    encoded.bytes.resize(written);
    return encoded;
}

// What one instance sent its peer, in order.
struct PeerOutbox : public ReplicationLinksInterface {
    bool connected{true};
    std::deque<Encoded> messages;

    [[nodiscard]] bool peer_connected() const override {
        return connected;
    }
    void send_record_to_peer(const ReplicatedRecord& record) override {
        if (connected) {
            messages.push_back(encode_message(record));
        }
    }
    void send_ack_to_peer(const WalAck& ack) override {
        if (connected) {
            messages.push_back(encode_message(ack));
        }
    }
    void send_position_request_to_peer(const LogPositionRequest& request) override {
        if (connected) {
            messages.push_back(encode_message(request));
        }
    }
    void send_position_reply_to_peer(const LogPositionReply& reply) override {
        if (connected) {
            messages.push_back(encode_message(reply));
        }
    }
};

// What one instance's lease rules sent.
struct LeaseOutbox : public LeaseLinksInterface {
    std::vector<LeaseRequest> to_peer;
    std::vector<LeaseRequest> to_third_voter;
    std::vector<LeaseGrant> grants;
    std::vector<LeaseRefusal> refusals;

    void send_request_to_peer(const LeaseRequest& request) override {
        to_peer.push_back(request);
    }
    void send_request_to_third_voter(const LeaseRequest& request) override {
        to_third_voter.push_back(request);
    }
    void send_grant(const ConnectionID& /*conn_id*/, const LeaseGrant& grant) override {
        grants.push_back(grant);
    }
    void send_refusal(const ConnectionID& /*conn_id*/, const LeaseRefusal& refusal) override {
        refusals.push_back(refusal);
    }
};

// What one instance was told.
struct Events : public ReplicatedWalEventsInterface {
    std::vector<ReplicaRole> roles;
    int64_t committed{0};
    std::vector<int64_t> written;
    std::vector<int64_t> discarded_after_seq_nos;

    void role_changed(ReplicaRole /*from*/, ReplicaRole to, int32_t /*epoch*/) override {
        roles.push_back(to);
    }
    void committed_through(int64_t seq_no) override {
        EXPECT_GT(seq_no, committed) << "the committed position went backwards or did not move";
        committed = seq_no;
    }
    void written_as_follower(int64_t seq_no, const WalEntryContents& /*entry*/) override {
        written.push_back(seq_no);
    }
    void discarded_after(int64_t seq_no) override {
        discarded_after_seq_nos.push_back(seq_no);
    }
    void granted_lease_to_peer(Clock::time_point /*at*/) override {}
};

// Promises kept in memory, as a disk would keep them across a process restart, written at once.
struct KeptPromises : public LeasePromiseRecorderInterface, public BackgroundPromiseRecorderInterface {
    std::optional<LeasePromiseStore::Record> record_kept;
    LeaderStatement statement{};
    std::optional<bool> outcome;

    bool record(const LeasePromiseRecord& promise) override {
        record_kept = LeasePromiseStore::Record{promise.promised_to, promise.until, promise.statement};
        statement = promise.statement;
        return true;
    }
    bool record_in_background(const LeasePromiseRecord& promise) override {
        if (outcome.has_value()) {
            return false;
        }
        outcome = record(promise);
        return true;
    }
    std::optional<bool> background_result() override {
        const std::optional<bool> result = outcome;
        outcome.reset();
        return result;
    }
};

// Every record a WAL holds, by number: its epoch and its bytes.
std::map<int64_t, std::pair<int32_t, std::vector<uint8_t>>> read_wal(const std::string& directory) {
    std::map<int64_t, std::pair<int32_t, std::vector<uint8_t>>> records;
    static_cast<void>(WalReader::replay(directory, WalPosition{0, 0}, [&records](int64_t record_id, const void* payload, size_t size) {
        const std::optional<WalEntryContents> contents = read_wal_entry_contents(payload, size);
        if (contents.has_value()) {
            records[record_id] = {contents->leader_epoch, std::vector<uint8_t>(contents->bytes, contents->bytes + contents->size)};
        }
    }));
    return records;
}

} // namespaces

// Two instances of a pair and an arbiter.
class Pair {
  public:
    ~Pair() {
        for (int i = 0; i < 2; ++i) {
            wals_[i].reset();
            std::filesystem::remove_all(directories_[i]);
        }
    }

    explicit Pair(const std::string& name)
        : logger_(FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); }), arbiter_(timing_.period, now_, 0) {
        for (int i = 0; i < 2; ++i) {
            directories_[i] = tests_common::make_scratch_directory(name + "_" + std::to_string(i + 1));
        }
    }

    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;

    // Starts instance i, which has its WAL in directory i, keeping the promises it recorded before.
    void start(int i, bool ha_enabled = true) {
        ReplicatedWalConfiguration configuration;
        configuration.owner_name = "TestReplica" + std::to_string(i + 1);
        configuration.wal_directory = directories_[i];
        configuration.wal_segment_size = segment_size;
        configuration.ha_enabled = ha_enabled;
        configuration.pair_id = test_pair_id;
        configuration.instance_id = i + 1;
        configuration.third_voter_id = arbiter_id;
        configuration.third_voter_name = "the arbiter";
        configuration.lease = timing_;
        events_[i] = Events{};
        // A write whose outcome the process never read is forgotten with the process.
        kept_[i].outcome.reset();
        wals_[i] = std::make_unique<ReplicatedWal>(configuration, logger_, peer_outboxes_[i], lease_outboxes_[i], events_[i],
                                                   PromisesKeptElsewhere{&kept_[i], kept_[i].record_kept, kept_[i].statement});
        up_[i] = true;
        static_cast<void>(wals_[i]->open(now_));
    }

    void start_both() {
        start(0);
        start(1);
        connect();
    }

    // Instance i's process dies: it neither ticks nor answers, and its connection to the peer goes. A
    // process that is killed runs nothing more, so only the survivor is told the connection has gone.
    void crash(int i) {
        wals_[i].reset();
        up_[i] = false;
        lease_outboxes_[i] = LeaseOutbox{};
        disconnect();
    }

    // The connection between the two opens, and each is told.
    void connect() {
        for (int i = 0; i < 2; ++i) {
            peer_outboxes_[i].connected = up_[0] && up_[1];
        }
        if (up_[0] && up_[1]) {
            for (int i = 0; i < 2; ++i) {
                wals_[i]->on_peer_connection_established(now_);
            }
        }
    }

    // The connection between the two closes: what was on its way is lost, and each is told.
    void disconnect() {
        for (int i = 0; i < 2; ++i) {
            peer_outboxes_[i].connected = false;
            peer_outboxes_[i].messages.clear();
        }
        for (int i = 0; i < 2; ++i) {
            if (up_[i]) {
                wals_[i]->on_peer_connection_lost(now_);
            }
        }
    }

    // One lease tick for both, with the acknowledgement watch run at every interval within it, then
    // everything delivered.
    void step() {
        for (Clock::duration elapsed{}; elapsed < LeaseTiming::tick_interval; elapsed += ReplicatedWal::acknowledgement_watch_interval) {
            now_ += ReplicatedWal::acknowledgement_watch_interval;
            for (int i = 0; i < 2; ++i) {
                if (up_[i]) {
                    wals_[i]->on_acknowledgement_watch(now_);
                }
            }
        }
        for (int i = 0; i < 2; ++i) {
            if (up_[i]) {
                wals_[i]->on_lease_tick(now_);
            }
        }
        deliver();
    }

    // Delivers every message, both ways, until none is left. Messages from an instance whose sending is
    // held stay where they are.
    void deliver() {
        // Ordinarily a few rounds settle everything. One that never settles, such as a follower and a
        // leader asking and answering for ever, is a fault, and fails here rather than hanging the test.
        constexpr int round_limit = 10000;
        bool delivered = true;
        for (int round = 0; delivered; ++round) {
            if (round == round_limit) {
                ADD_FAILURE() << "messages were still being exchanged after " << round_limit << " rounds";
                for (int i = 0; i < 2; ++i) {
                    peer_outboxes_[i].messages.clear();
                    lease_outboxes_[i] = LeaseOutbox{};
                }
                return;
            }
            delivered = false;
            for (int i = 0; i < 2; ++i) {
                const int peer = 1 - i;
                delivered = deliver_leases(i, peer) || delivered;
                while (!held_[i] && !peer_outboxes_[i].messages.empty()) {
                    const Encoded message = peer_outboxes_[i].messages.front();
                    peer_outboxes_[i].messages.pop_front();
                    delivered = true;
                    if (up_[peer]) {
                        EXPECT_TRUE(wals_[peer]->handle_message(ConnectionID{}, message.pdu_id, message.bytes.data(), message.bytes.size(), now_));
                    }
                }
            }
        }
    }

    bool deliver_leases(int i, int peer) {
        bool delivered = false;
        LeaseOutbox sent = std::move(lease_outboxes_[i]);
        lease_outboxes_[i] = LeaseOutbox{};
        for (const LeaseRequest& request : sent.to_peer) {
            delivered = true;
            if (!up_[peer] || !peer_outboxes_[i].connected) {
                continue;
            }
            const Encoded encoded = encode_message(request);
            EXPECT_TRUE(wals_[peer]->handle_message(ConnectionID{}, encoded.pdu_id, encoded.bytes.data(), encoded.bytes.size(), now_));
            LeaseOutbox answers = std::move(lease_outboxes_[peer]);
            lease_outboxes_[peer] = LeaseOutbox{};
            lease_outboxes_[peer].to_peer = answers.to_peer;
            lease_outboxes_[peer].to_third_voter = answers.to_third_voter;
            for (const LeaseGrant& grant : answers.grants) {
                const Encoded reply = encode_message(grant);
                EXPECT_TRUE(wals_[i]->handle_message(ConnectionID{}, reply.pdu_id, reply.bytes.data(), reply.bytes.size(), now_));
            }
            for (const LeaseRefusal& refusal : answers.refusals) {
                const Encoded reply = encode_message(refusal);
                EXPECT_TRUE(wals_[i]->handle_message(ConnectionID{}, reply.pdu_id, reply.bytes.data(), reply.bytes.size(), now_));
            }
        }
        for (const LeaseRequest& request : sent.to_third_voter) {
            delivered = true;
            if (!arbiter_up_ || !up_[i]) {
                continue;
            }
            const LeaseVoter::Answer answer = arbiter_.consider(request.candidate_instance_id, request.epoch, now_);
            if (answer.verdict == LeaseVoter::Verdict::Granted) {
                int64_t echoed = 0;
                const LeaderStatement statement{request.statement_leader_id, request.statement_epoch, request.statement_number, request.peer_may_lead};
                if (statement.leader_id != 0) {
                    arbiter_.record_statement(statement);
                    echoed = arbiter_.holds_statement(statement) ? statement.number : 0;
                }
                LeaseGrant grant{};
                grant.pair_id = test_pair_id;
                grant.voter_instance_id = arbiter_id;
                grant.epoch = request.epoch;
                grant.request_id = request.request_id;
                grant.echoed_statement_number = echoed;
                const Encoded reply = encode_message(grant);
                EXPECT_TRUE(wals_[i]->handle_message(ConnectionID{}, reply.pdu_id, reply.bytes.data(), reply.bytes.size(), now_));
            } else {
                LeaseRefusal refusal{};
                refusal.pair_id = test_pair_id;
                refusal.voter_instance_id = arbiter_id;
                refusal.highest_epoch = answer.highest_epoch;
                refusal.request_id = request.request_id;
                refusal.reason = LeaseRefusalReason::promised_elsewhere;
                const Encoded reply = encode_message(refusal);
                EXPECT_TRUE(wals_[i]->handle_message(ConnectionID{}, reply.pdu_id, reply.bytes.data(), reply.bytes.size(), now_));
            }
        }
        // Answers this instance gave to requests it was sent are delivered with those requests above.
        return delivered;
    }

    bool step_until(int i, ReplicaRole role, int limit = 200) {
        for (int n = 0; n < limit && !(up_[i] && wals_[i]->role() == role); ++n) {
            step();
        }
        return up_[i] && wals_[i]->role() == role;
    }

    // Steps until the leader has said its follower may lead, which a new leadership says only once the
    // follower holds every record. Until then the follower may not take over if the leader dies.
    bool step_until_follower_may_lead(int leader, int limit = 100) {
        const size_t said_before = count_log("saying it may lead again");
        for (int n = 0; n < limit; ++n) {
            if (count_log("saying it may lead again") > said_before) {
                return true;
            }
            step();
        }
        static_cast<void>(leader);
        return false;
    }

    size_t count_log(const std::string& text) {
        logger_.flush();
        return static_cast<size_t>(std::count_if(log_.begin(), log_.end(), [&text](const std::string& line) { return line.find(text) != std::string::npos; }));
    }

    // Steps until the follower's WAL agrees with the leader's and every record has been acknowledged.
    bool step_until_follower_holds_everything(int leader, int limit = 100) {
        for (int n = 0; n < limit; ++n) {
            const int follower = 1 - leader;
            if (wals_[follower]->last_seq_no() == wals_[leader]->last_seq_no() && wals_[leader]->waits_for_follower() &&
                wals_[leader]->committed_through() >= wals_[leader]->last_seq_no()) {
                return true;
            }
            step();
        }
        return false;
    }

    int64_t append(int i, uint8_t marker) {
        const std::vector<uint8_t> bytes{marker, static_cast<uint8_t>(marker + 1), static_cast<uint8_t>(marker + 2)};
        const int64_t seq_no = wals_[i]->append(test_pdu_id, bytes.data(), bytes.size(), 0, now_);
        deliver();
        return seq_no;
    }

    void advance(Clock::duration by) {
        now_ += by;
    }

    bool log_contains(const std::string& text) {
        logger_.flush();
        return std::any_of(log_.begin(), log_.end(), [&text](const std::string& line) { return line.find(text) != std::string::npos; });
    }

    LeaseTiming timing_{};
    Clock::time_point now_{};
    std::vector<std::string> log_;
    QuillLogger logger_;
    std::string directories_[2];
    PeerOutbox peer_outboxes_[2];
    LeaseOutbox lease_outboxes_[2];
    Events events_[2];
    KeptPromises kept_[2];
    std::unique_ptr<ReplicatedWal> wals_[2];
    bool up_[2]{false, false};
    // While set, what instance i sends its peer is held rather than delivered.
    bool held_[2]{false, false};
    LeaseVoter arbiter_;
    bool arbiter_up_{true};
};

TEST(ReplicatedWalTest, AnInstanceRunningAloneLeadsAtOnceAndCommitsEachRecordOnceWritten) {
    Pair pair("replicated_wal_alone");
    pair.start(0, /*ha_enabled=*/false);
    ASSERT_EQ(pair.wals_[0]->role(), ReplicaRole::leader);
    const int64_t seq_no = pair.append(0, 10);
    EXPECT_EQ(seq_no, 1);
    EXPECT_EQ(pair.events_[0].committed, 1);
    EXPECT_EQ(pair.wals_[0]->next_seq_no(), 2);
}

TEST(ReplicatedWalTest, ARecordIsCommittedOnlyOnceTheFollowerAcknowledgesIt) {
    Pair pair("replicated_wal_ack");
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_TRUE(pair.step_until(1, ReplicaRole::follower));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));

    // The follower's acknowledgements are held: the record reaches it and is written, but not committed.
    pair.held_[1] = true;
    const int64_t seq_no = pair.append(0, 20);
    EXPECT_EQ(pair.wals_[1]->last_seq_no(), seq_no);
    EXPECT_LT(pair.events_[0].committed, seq_no);

    pair.held_[1] = false;
    pair.deliver();
    EXPECT_EQ(pair.events_[0].committed, seq_no);

    // The two WALs hold the same record: the same number, epoch and bytes.
    const auto leader_records = read_wal(pair.directories_[0]);
    const auto follower_records = read_wal(pair.directories_[1]);
    ASSERT_EQ(follower_records.count(seq_no), 1U);
    EXPECT_EQ(follower_records.at(seq_no), leader_records.at(seq_no));
    EXPECT_EQ(follower_records.at(seq_no).first, pair.wals_[0]->epoch());
}

TEST(ReplicatedWalTest, TheFollowerWritesNothingAfterAMissingRecordUntilTheTwoWalsAgreeAgain) {
    Pair pair("replicated_wal_gap");
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));

    // The leader's next record is lost on the way, as if dropped: the one after it would leave a gap.
    pair.held_[0] = true;
    const int64_t lost = pair.append(0, 30);
    pair.peer_outboxes_[0].messages.clear();
    pair.held_[0] = false;
    const int64_t after = pair.append(0, 40);
    EXPECT_EQ(pair.wals_[1]->last_seq_no(), lost - 1) << "the follower wrote a record after a missing one";

    // It asks again where the WALs agree, and is sent both.
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    EXPECT_EQ(pair.wals_[1]->last_seq_no(), after);
    EXPECT_EQ(read_wal(pair.directories_[1]), read_wal(pair.directories_[0]));
}

TEST(ReplicatedWalTest, AFollowerThatStopsAcknowledgingIsLeftBehindOnlyOnceAVoterConfirmsTheStatement) {
    Pair pair("replicated_wal_alone_after_stall");
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    ASSERT_TRUE(pair.step_until_follower_may_lead(0));

    // The follower stalls: it receives records and acknowledges none.
    pair.held_[1] = true;
    const int64_t seq_no = pair.append(0, 50);
    pair.advance(ReplicatedWal::follower_acknowledgement_timeout);
    pair.wals_[0]->on_acknowledgement_watch(pair.now_);
    EXPECT_TRUE(pair.log_contains("the follower is too far behind"));
    EXPECT_TRUE(pair.log_contains("saying the follower may not lead"));
    EXPECT_LT(pair.events_[0].committed, seq_no) << "committed before a voter confirmed that the follower may not lead";

    // The statement goes to the arbiter with the leader's next renewal, and the grant confirms it.
    for (int n = 0; n < 20 && pair.events_[0].committed < seq_no; ++n) {
        pair.step();
    }
    EXPECT_EQ(pair.events_[0].committed, seq_no);

    // The follower recovers: it acknowledges everything, and the leader says it may lead again.
    pair.held_[1] = false;
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    EXPECT_TRUE(pair.log_contains("the follower is keeping up again"));
    for (int n = 0; n < 20 && !pair.log_contains("saying it may lead again"); ++n) {
        pair.step();
    }
    EXPECT_TRUE(pair.log_contains("saying it may lead again"));
}

TEST(ReplicatedWalTest, ARejoiningFollowerDiscardsTheRecordsOfAnOlderLeadershipAndTakesItsLeaders) {
    Pair pair("replicated_wal_repair");
    // Before either starts: both hold records 1 to 3 from epoch 1. Instance 2 also holds 4 and 5 from
    // epoch 1, which its leader never had; instance 1 holds 4 to 6 from epoch 2.
    {
        Wal first;
        Wal second;
        first.open(pair.directories_[0], segment_size);
        second.open(pair.directories_[1], segment_size);
        const uint8_t old_bytes[] = {1};
        const uint8_t new_bytes[] = {2};
        for (int64_t seq_no = 1; seq_no <= 3; ++seq_no) {
            first.append(seq_no, test_pdu_id, old_bytes, 1, 0, 1);
            second.append(seq_no, test_pdu_id, old_bytes, 1, 0, 1);
        }
        for (int64_t seq_no = 4; seq_no <= 5; ++seq_no) {
            second.append(seq_no, test_pdu_id, old_bytes, 1, 0, 1);
        }
        for (int64_t seq_no = 4; seq_no <= 6; ++seq_no) {
            first.append(seq_no, test_pdu_id, new_bytes, 1, 0, 2);
        }
    }
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));

    ASSERT_EQ(pair.events_[1].discarded_after_seq_nos.size(), 1U);
    EXPECT_EQ(pair.events_[1].discarded_after_seq_nos[0], 3);
    EXPECT_TRUE(pair.log_contains("from this instance's log -- the leader does not hold them"));
    const auto follower_records = read_wal(pair.directories_[1]);
    EXPECT_EQ(follower_records, read_wal(pair.directories_[0]));
    EXPECT_EQ(follower_records.at(5).first, 2);
}

TEST(ReplicatedWalTest, ANewLeaderNumbersItsRecordsAboveEveryRecordItHolds) {
    Pair pair("replicated_wal_new_leader");
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    ASSERT_TRUE(pair.step_until_follower_may_lead(0));
    for (uint8_t marker = 0; marker < 5; ++marker) {
        static_cast<void>(pair.append(0, static_cast<uint8_t>(60 + marker)));
    }
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    const int64_t last = pair.wals_[0]->last_seq_no();

    pair.crash(0);
    ASSERT_TRUE(pair.step_until(1, ReplicaRole::leader));
    EXPECT_EQ(pair.wals_[1]->next_seq_no(), last + 1);
    EXPECT_EQ(pair.wals_[1]->committed_through(), last);
    EXPECT_EQ(pair.append(1, 70), last + 1);
}

TEST(ReplicatedWalTest, ARecordTheFollowerWroteButTheLeaderNeverSawAcknowledgedSurvivesAChangeOfLeader) {
    Pair pair("replicated_wal_unacknowledged");
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    ASSERT_TRUE(pair.step_until_follower_may_lead(0));

    // The follower writes the record, and its acknowledgement never reaches the leader, which dies.
    pair.held_[1] = true;
    const int64_t seq_no = pair.append(0, 80);
    ASSERT_EQ(pair.wals_[1]->last_seq_no(), seq_no);
    pair.crash(0);
    pair.held_[1] = false;

    ASSERT_TRUE(pair.step_until(1, ReplicaRole::leader));
    EXPECT_EQ(pair.wals_[1]->last_seq_no(), seq_no);
    EXPECT_EQ(pair.append(1, 90), seq_no + 1);

    // The old leader rejoins as follower and is brought up to date.
    pair.start(0);
    pair.connect();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::follower));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(1));
    EXPECT_EQ(read_wal(pair.directories_[0]), read_wal(pair.directories_[1]));
}

// Many runs of random events: records written, messages delayed, the connection lost and restored,
// instances crashing and restarting, the arbiter stopping and starting. After every change of leader,
// the new leader's WAL must hold every record any leader committed, with the same epoch and bytes; and
// once everything is restored, one instance must lead and the two WALs must be the same.
TEST(ReplicatedWalTest, InRandomRunsNoCommittedRecordIsLostAndTheTwoWalsEndTheSame) {
    constexpr int runs = 20;
    constexpr int steps = 400;
    for (int run = 0; run < runs; ++run) {
        Pair pair("replicated_wal_random_" + std::to_string(run));
        std::mt19937 random(static_cast<std::mt19937::result_type>(run + 1));
        const auto chance = [&random](double probability) { return std::uniform_real_distribution<double>(0.0, 1.0)(random) < probability; };
        pair.start_both();

        std::map<int64_t, std::pair<int32_t, std::vector<uint8_t>>> committed;
        int64_t seen_committed[2]{0, 0};
        ReplicaRole roles[2]{ReplicaRole::unknown, ReplicaRole::unknown};
        uint8_t marker = 0;

        for (int step = 0; step < steps; ++step) {
            for (int i = 0; i < 2; ++i) {
                if (pair.up_[i] && pair.wals_[i]->role() == ReplicaRole::leader && chance(0.5)) {
                    static_cast<void>(pair.append(i, marker++));
                }
            }
            if (chance(0.05)) {
                pair.held_[0] = !pair.held_[0];
            }
            if (chance(0.05)) {
                pair.held_[1] = !pair.held_[1];
            }
            if (chance(0.02)) {
                if (pair.peer_outboxes_[0].connected) {
                    pair.disconnect();
                } else {
                    pair.connect();
                }
            }
            if (chance(0.01)) {
                const int victim = chance(0.5) ? 0 : 1;
                if (pair.up_[victim] && pair.up_[1 - victim]) {
                    pair.crash(victim);
                }
            }
            for (int i = 0; i < 2; ++i) {
                if (!pair.up_[i] && chance(0.05)) {
                    pair.start(i);
                    pair.connect();
                }
            }
            if (chance(0.01)) {
                pair.arbiter_up_ = !pair.arbiter_up_;
            }
            pair.step();

            for (int i = 0; i < 2; ++i) {
                if (!pair.up_[i]) {
                    roles[i] = ReplicaRole::unknown;
                    seen_committed[i] = 0;
                    continue;
                }
                const ReplicaRole role = pair.wals_[i]->role();
                const auto records = read_wal(pair.directories_[i]);
                if (role == ReplicaRole::leader && roles[i] != ReplicaRole::leader) {
                    for (const auto& [seq_no, record] : committed) {
                        ASSERT_EQ(records.count(seq_no), 1U)
                            << "run " << run << " step " << step << ": instance " << i + 1 << " took the lead without committed record " << seq_no;
                        ASSERT_EQ(records.at(seq_no), record)
                            << "run " << run << " step " << step << ": instance " << i + 1 << " took the lead holding a different record " << seq_no;
                    }
                    seen_committed[i] = pair.wals_[i]->committed_through();
                }
                roles[i] = role;
                if (role == ReplicaRole::leader) {
                    for (int64_t seq_no = seen_committed[i] + 1; seq_no <= pair.events_[i].committed; ++seq_no) {
                        ASSERT_EQ(records.count(seq_no), 1U);
                        committed[seq_no] = records.at(seq_no);
                    }
                    seen_committed[i] = std::max(seen_committed[i], pair.events_[i].committed);
                }
            }
        }

        // Everything restored: both running, connected, nothing held, the arbiter up.
        for (int i = 0; i < 2; ++i) {
            pair.held_[i] = false;
            if (!pair.up_[i]) {
                pair.start(i);
            }
        }
        pair.arbiter_up_ = true;
        pair.disconnect();
        pair.connect();
        // One instance leads. The other is not required to have become follower: an instance holding an
        // older statement that its peer may not lead goes on refusing the peer's renewals, so it never
        // agrees that the peer leads (docs/bug_list.md, BUG-0152). Its WAL is kept up to date all the same,
        // because replication does not wait for its role.
        int leader = -1;
        for (int n = 0; n < 300 && leader < 0; ++n) {
            pair.step();
            for (int i = 0; i < 2; ++i) {
                if (pair.wals_[i]->role() == ReplicaRole::leader && pair.wals_[1 - i]->role() != ReplicaRole::leader) {
                    leader = i;
                }
            }
        }
        ASSERT_GE(leader, 0) << "run " << run << ": no instance led once everything was restored";
        ASSERT_TRUE(pair.step_until_follower_holds_everything(leader, 300)) << "run " << run;
        EXPECT_EQ(read_wal(pair.directories_[0]), read_wal(pair.directories_[1])) << "run " << run << ": the two WALs differ";
        const auto final_records = read_wal(pair.directories_[leader]);
        for (const auto& [seq_no, record] : committed) {
            ASSERT_EQ(final_records.count(seq_no), 1U) << "run " << run << ": committed record " << seq_no << " is lost";
            EXPECT_EQ(final_records.at(seq_no), record) << "run " << run << ": committed record " << seq_no << " changed";
        }
    }
}

} // namespaces
