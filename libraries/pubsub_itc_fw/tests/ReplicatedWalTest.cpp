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
#include <array>
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
#include <pubsub_itc_fw/TestEventChannel.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/WalReader.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

#include <lease_messages.hpp>
#include <replication_messages.hpp>
#include <test_events.hpp>

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
    void send_ack_to_peer(const ConnectionID& /*on*/, const WalAck& ack) override {
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
// Stands in for the reading thread a real application uses through PeerCatchUpInterface: the leader asks
// it to send a rejoining follower the records it lacks, and it does so at the next step, reading the
// leader's WAL as it then stands, so that records the leader writes in between are part of what it sends,
// as they are with the real thread. It then tells the leader the follower is caught up. Told to stop, it
// confirms that it has stopped only at the next step, as the real thread confirms by a report.
struct SimulatedCatchUp : public PeerCatchUpInterface {
    bool pending{false};
    bool stopping{false};
    int64_t after{0};
    int started{0};

    void start_peer_catch_up(int64_t after_seq_no) override {
        pending = true;
        after = after_seq_no;
        ++started;
    }
    void stop_peer_catch_up() override {
        pending = false;
        stopping = true;
    }
    [[nodiscard]] bool reading_has_stopped() const override {
        return !pending && !stopping;
    }
};

class Pair {
  public:
    ~Pair() {
        for (int i = 0; i < 2; ++i) {
            wals_[i].reset();
            std::filesystem::remove_all(directories_[i]);
        }
    }

    // With @p with_catch_up_thread, a rejoining follower's missing records are sent through a
    // SimulatedCatchUp, as an application with a reading thread has them sent; without, the leader reads
    // and sends them itself.
    explicit Pair(const std::string& name, bool with_catch_up_thread = false)
        : logger_(FwLogLevel::Info, [this](const std::string& line) { log_.push_back(line); })
        , arbiter_(timing_.period, now_, 0)
        , with_catch_up_thread_(with_catch_up_thread) {
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
        catch_ups_[i] = SimulatedCatchUp{};
        if (with_catch_up_thread_) {
            wals_[i]->use_peer_catch_up(catch_ups_[i]);
        }
        if (test_events_[i] != nullptr) {
            wals_[i]->report_test_events_to(*test_events_[i]);
        }
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
        catch_ups_[i] = SimulatedCatchUp{};
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
        run_catch_ups();
        deliver();
    }

    // Each catch-up that was told to stop confirms that it has stopped, and the leader answers any request
    // it held back until then. Each catch-up that has been started sends the follower the records after
    // its position, up to the last the leader has written, and hands it over.
    void run_catch_ups() {
        for (int i = 0; i < 2; ++i) {
            if (catch_ups_[i].stopping) {
                catch_ups_[i].stopping = false;
                if (up_[i]) {
                    wals_[i]->on_peer_catch_up_stopped(now_);
                }
            }
            if (!up_[i] || !catch_ups_[i].pending || wals_[i]->role() != ReplicaRole::leader) {
                continue;
            }
            const int64_t after = catch_ups_[i].after;
            [[maybe_unused]] const auto end = WalReader::replay(directories_[i], wals_[i]->wal().scan_start_for(after + 1),
                                                                [this, i, after](int64_t record_id, const void* payload, size_t size) {
                                                                    const std::optional<WalEntryContents> contents = read_wal_entry_contents(payload, size);
                                                                    if (record_id <= after || !contents.has_value()) {
                                                                        return;
                                                                    }
                                                                    ReplicatedRecord record{};
                                                                    record.seq_no = record_id;
                                                                    record.leader_epoch = contents->leader_epoch;
                                                                    record.wall_time_ns = contents->wall_time_ns;
                                                                    record.pdu_id = contents->pdu_id;
                                                                    record.payload = BytesView{contents->bytes, contents->size};
                                                                    peer_outboxes_[i].send_record_to_peer(record);
                                                                });
            catch_ups_[i].pending = false;
            wals_[i]->on_peer_caught_up(wals_[i]->last_seq_no(), now_);
        }
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
    bool with_catch_up_thread_{false};
    SimulatedCatchUp catch_ups_[2];
    // Where each instance reports its test events, if anywhere.
    TestEventChannel* test_events_[2]{nullptr, nullptr};
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

namespace {

// Records each test event sent, decoding RoleChanged and LeaseChanged.
struct RecordingTestEventLinks : public TestEventLinksInterface {
    void send_test_event([[maybe_unused]] const ConnectionID& connection, int16_t pdu_id, const uint8_t* bytes, size_t size) override {
        std::array<uint8_t, 256> arena_storage{};
        BumpAllocator arena(arena_storage.data(), arena_storage.size());
        size_t consumed = 0;
        size_t arena_needed = 0;
        if (pdu_id == RoleChanged::message_pdu_id) {
            RoleChangedView view{};
            ASSERT_TRUE(decode(view, bytes, size, consumed, arena, arena_needed));
            changes.push_back(Change{std::string(view.owner), view.instance, view.from_role, view.to_role, view.epoch});
        } else if (pdu_id == LeaseChanged::message_pdu_id) {
            LeaseChangedView view{};
            ASSERT_TRUE(decode(view, bytes, size, consumed, arena, arena_needed));
            lease_changes.push_back(LeaseEvent{std::string(view.owner), view.instance, view.change, view.granted_by, view.epoch});
        }
    }
    struct Change {
        std::string owner;
        int64_t instance;
        LeadershipRole from;
        LeadershipRole to;
        int32_t epoch;
    };
    struct LeaseEvent {
        std::string owner;
        int64_t instance;
        LeaseChange change;
        Voter granted_by;
        int32_t epoch;
    };
    std::vector<Change> changes;
    std::vector<LeaseEvent> lease_changes;
};

} // un-named namespace

TEST(ReplicatedWalTest, EachChangeOfRoleIsReportedAsATestEventAndThePresentRoleWhenAsked) {
    RecordingTestEventLinks links;
    TestEventChannel channel(links, "test_component", 1, "TestThread");
    channel.on_connection_established(ConnectionID{3, std::string(TestEventChannel::service_name)});
    Pair pair("replicated_wal_test_events");
    pair.test_events_[0] = &channel;
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_EQ(links.changes.size(), 1U);
    EXPECT_EQ(links.changes[0].owner, "TestReplica1");
    EXPECT_EQ(links.changes[0].instance, 1);
    EXPECT_EQ(links.changes[0].from, LeadershipRole::Unknown);
    EXPECT_EQ(links.changes[0].to, LeadershipRole::Leader);
    EXPECT_EQ(links.changes[0].epoch, pair.wals_[0]->epoch());

    pair.wals_[0]->report_role_to_test_events();
    ASSERT_EQ(links.changes.size(), 2U);
    EXPECT_EQ(links.changes[1].from, LeadershipRole::Unknown);
    EXPECT_EQ(links.changes[1].to, LeadershipRole::Leader);
}

TEST(ReplicatedWalTest, TheLeaseAgentReportsAskingToLeadAndTheGrantThatMadeItLeader) {
    RecordingTestEventLinks links;
    TestEventChannel channel(links, "test_component", 1, "TestThread");
    channel.on_connection_established(ConnectionID{3, std::string(TestEventChannel::service_name)});
    Pair pair("replicated_wal_lease_events");
    pair.test_events_[0] = &channel;
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_GE(links.lease_changes.size(), 2U);
    EXPECT_EQ(links.lease_changes.front().change, LeaseChange::AskingToLead);
    EXPECT_EQ(links.lease_changes.front().instance, 1);
    EXPECT_EQ(links.lease_changes.front().granted_by, Voter::NoVoter);
    const auto& granted = links.lease_changes.back();
    EXPECT_EQ(granted.change, LeaseChange::Granted);
    EXPECT_NE(granted.granted_by, Voter::NoVoter);
    EXPECT_EQ(granted.epoch, pair.wals_[0]->epoch());
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

// A leader cut off from its follower and from the arbiter states that the follower may not lead, and no
// voter echoes the statement. It dies; the follower takes over, as it may, because no voter holds the
// statement. When the old leader restarts it still holds its own statement, and it must take the new
// leader's newer one in its place and follow, rather than refusing the new leader for ever (BUG-0152).
TEST(ReplicatedWalTest, AFormerLeaderHoldingAStatementNoVoterEchoedFollowsTheLeaderThatTookOver) {
    Pair pair("replicated_wal_stale_statement");
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    ASSERT_TRUE(pair.step_until_follower_may_lead(0));

    pair.arbiter_up_ = false;
    pair.disconnect();
    pair.deliver();
    ASSERT_TRUE(pair.log_contains("saying the follower may not lead"));
    pair.crash(0);
    pair.arbiter_up_ = true;
    ASSERT_TRUE(pair.step_until(1, ReplicaRole::leader));

    pair.start(0);
    pair.connect();
    EXPECT_TRUE(pair.step_until(0, ReplicaRole::follower)) << "the old leader never agreed that the new one leads";
    ASSERT_TRUE(pair.step_until_follower_holds_everything(1));
    EXPECT_EQ(read_wal(pair.directories_[0]), read_wal(pair.directories_[1]));
}

// Many runs of random events: records written, messages delayed, the connection lost and restored,
// instances crashing and restarting, the arbiter stopping and starting. After every change of leader,
// the new leader's WAL must hold every record any leader committed, with the same epoch and bytes; and
// once everything is restored, one instance must lead, the other must follow, and the two WALs must be
// the same.
// The randomised runs, with a rejoining follower's records read by the leader itself or sent through the
// catch-up.
void run_random_runs(bool with_catch_up_thread) {
    constexpr int runs = 20;
    constexpr int steps = 400;
    for (int run = 0; run < runs; ++run) {
        Pair pair(std::string(with_catch_up_thread ? "replicated_wal_random_catch_up_" : "replicated_wal_random_") + std::to_string(run), with_catch_up_thread);
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
        int leader = -1;
        for (int n = 0; n < 300 && leader < 0; ++n) {
            pair.step();
            for (int i = 0; i < 2; ++i) {
                if (pair.wals_[i]->role() == ReplicaRole::leader && pair.wals_[1 - i]->role() == ReplicaRole::follower) {
                    leader = i;
                }
            }
        }
        ASSERT_GE(leader, 0) << "run " << run << ": no instance led with the other following once everything was restored";
        ASSERT_TRUE(pair.step_until_follower_holds_everything(leader, 300)) << "run " << run;
        EXPECT_EQ(read_wal(pair.directories_[0]), read_wal(pair.directories_[1])) << "run " << run << ": the two WALs differ";
        const auto final_records = read_wal(pair.directories_[leader]);
        for (const auto& [seq_no, record] : committed) {
            ASSERT_EQ(final_records.count(seq_no), 1U) << "run " << run << ": committed record " << seq_no << " is lost";
            EXPECT_EQ(final_records.at(seq_no), record) << "run " << run << ": committed record " << seq_no << " changed";
        }
    }
}

TEST(ReplicatedWalTest, InRandomRunsNoCommittedRecordIsLostAndTheTwoWalsEndTheSame) {
    run_random_runs(false);
}

TEST(ReplicatedWalTest, InRandomRunsWithTheCatchUpOnItsOwnThreadNoCommittedRecordIsLostAndTheTwoWalsEndTheSame) {
    run_random_runs(true);
}

// The leader has a rejoining follower's missing records sent through the catch-up, not read on its own
// thread, and then sends it each new record itself.
TEST(ReplicatedWalTest, ARejoiningFollowerIsSentWhatItLacksThroughTheCatchUpAndThenNewRecords) {
    Pair pair("replicated_wal_catch_up", true);
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    pair.crash(1);
    for (int n = 0; n < 5; ++n) {
        pair.step();
    }
    for (uint8_t marker = 0; marker < 20; ++marker) {
        static_cast<void>(pair.append(0, marker));
    }
    pair.start(1);
    pair.connect();
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    EXPECT_GE(pair.catch_ups_[0].started, 1);
    EXPECT_TRUE(pair.log_contains("are being sent to it from the WAL"));
    EXPECT_TRUE(pair.log_contains("has been sent every record through"));
    static_cast<void>(pair.append(0, 99));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    EXPECT_EQ(read_wal(pair.directories_[0]), read_wal(pair.directories_[1]));
}

// New records do not wait for a follower that is still being caught up, or that has been handed over but
// has not yet acknowledged every record up to where it was: they would wait behind the whole catch-up
// (BUG-0155). Once it has, they wait for it again.
TEST(ReplicatedWalTest, NewRecordsDoNotWaitForAFollowerUntilItHasAcknowledgedThroughTheHandOver) {
    Pair pair("replicated_wal_hand_over_wait", true);
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    pair.crash(1);
    for (int n = 0; n < 5; ++n) {
        pair.step();
    }
    for (uint8_t marker = 0; marker < 10; ++marker) {
        static_cast<void>(pair.append(0, marker));
    }
    const int started_before = pair.catch_ups_[0].started;
    pair.start(1);
    pair.connect();
    pair.deliver(); // the follower's request reaches the leader, which starts the catch-up
    ASSERT_EQ(pair.catch_ups_[0].started, started_before + 1);
    pair.held_[1] = true; // from here the follower's acknowledgements are held back
    pair.run_catch_ups(); // the follower is sent every record and handed over
    pair.deliver();
    ASSERT_TRUE(pair.log_contains("has been sent every record through"));

    EXPECT_FALSE(pair.wals_[0]->waits_for_follower());
    const int64_t seq_no = pair.append(0, 50);
    EXPECT_GE(pair.wals_[0]->committed_through(), seq_no) << "a new record waited for a follower that had not caught up";

    pair.held_[1] = false;
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    EXPECT_TRUE(pair.wals_[0]->waits_for_follower());
}

// A follower asks on each of its connections, so one request can arrive twice; the catch-up is started once.
TEST(ReplicatedWalTest, ARequestThatArrivesTwiceStartsTheCatchUpOnce) {
    Pair pair("replicated_wal_request_twice", true);
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    pair.crash(1);
    for (int n = 0; n < 5; ++n) {
        pair.step();
    }
    for (uint8_t marker = 0; marker < 10; ++marker) {
        static_cast<void>(pair.append(0, marker));
    }
    pair.start(1);
    pair.connect();
    const int started_before = pair.catch_ups_[0].started;
    LogPositionRequest request{};
    request.last_seq_no = pair.wals_[1]->last_seq_no();
    request.last_epoch = pair.wals_[1]->epoch();
    for (int copy = 0; copy < 2; ++copy) {
        const Encoded encoded = encode_message(request);
        EXPECT_TRUE(pair.wals_[0]->handle_message(ConnectionID{}, encoded.pdu_id, encoded.bytes.data(), encoded.bytes.size(), pair.now_));
    }
    EXPECT_EQ(pair.catch_ups_[0].started, started_before + 1);
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    EXPECT_EQ(read_wal(pair.directories_[0]), read_wal(pair.directories_[1]));
}

// A request from a different position while a catch-up is running stops it. The follower is answered only
// once the reading thread has confirmed it has stopped, so that every record it was still sending is
// queued before the answer; until then the follower is not answered, and asks again.
TEST(ReplicatedWalTest, ARequestFromANewPositionIsAnsweredOnlyAfterTheRunningCatchUpHasStopped) {
    Pair pair("replicated_wal_request_restart", true);
    pair.start_both();
    ASSERT_TRUE(pair.step_until(0, ReplicaRole::leader));
    for (uint8_t marker = 0; marker < 5; ++marker) {
        static_cast<void>(pair.append(0, marker));
    }
    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    pair.crash(1);
    for (int n = 0; n < 5; ++n) {
        pair.step();
    }
    for (uint8_t marker = 0; marker < 10; ++marker) {
        static_cast<void>(pair.append(0, marker));
    }
    pair.start(1);
    pair.connect();
    const int started_before = pair.catch_ups_[0].started;
    LogPositionRequest request{};
    request.last_seq_no = pair.wals_[1]->last_seq_no();
    request.last_epoch = pair.wals_[1]->epoch();
    ASSERT_GE(request.last_seq_no, 2);
    const auto send_request = [&pair](const LogPositionRequest& to_send) {
        const Encoded encoded = encode_message(to_send);
        EXPECT_TRUE(pair.wals_[0]->handle_message(ConnectionID{}, encoded.pdu_id, encoded.bytes.data(), encoded.bytes.size(), pair.now_));
    };
    send_request(request);
    ASSERT_EQ(pair.catch_ups_[0].started, started_before + 1);

    // From one record earlier: the running catch-up stops, and nothing new starts.
    request.last_seq_no -= 1;
    send_request(request);
    EXPECT_FALSE(pair.catch_ups_[0].pending);
    EXPECT_TRUE(pair.log_contains("it is answered once that has stopped"));
    EXPECT_EQ(pair.catch_ups_[0].started, started_before + 1);

    // Asked again before the reading thread has confirmed: still not answered.
    send_request(request);
    EXPECT_EQ(pair.catch_ups_[0].started, started_before + 1);

    // Once it has confirmed, the request held back is answered at once, without the follower asking
    // again, and a new catch-up starts from it.
    pair.run_catch_ups();
    EXPECT_EQ(pair.catch_ups_[0].started, started_before + 2);
    EXPECT_EQ(pair.catch_ups_[0].after, request.last_seq_no);

    ASSERT_TRUE(pair.step_until_follower_holds_everything(0));
    EXPECT_EQ(read_wal(pair.directories_[0]), read_wal(pair.directories_[1]));
}

} // namespaces
