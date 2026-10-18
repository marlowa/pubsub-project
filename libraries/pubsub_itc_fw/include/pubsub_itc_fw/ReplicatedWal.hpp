#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <pubsub_itc_fw/BackgroundPromiseRecorder.hpp>
#include <pubsub_itc_fw/BackgroundPromiseRecorderInterface.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/CpuPinning.hpp>
#include <pubsub_itc_fw/EpochStore.hpp>
#include <pubsub_itc_fw/GaugeHandle.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/LeaseLinksInterface.hpp>
#include <pubsub_itc_fw/LeasePromiseStore.hpp>
#include <pubsub_itc_fw/LeaseTiming.hpp>
#include <pubsub_itc_fw/LogEpochTable.hpp>
#include <pubsub_itc_fw/PairLeaseAgent.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TestEventChannel.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>

#include <lease_messages.hpp>
#include <replication_messages.hpp>

namespace pubsub_itc_fw {

/// What an instance of a pair is doing: not yet decided, leading, or following its peer.
enum class ReplicaRole { unknown, leader, follower };

/// The word the Quill log uses for a role: "unknown", "leader" or "follower".
[[nodiscard]] const char* to_string(ReplicaRole role);

/**
 * @brief How a ReplicatedWal sends to the other instance of its pair. Implemented by the application.
 *
 * The application makes its connections, as it always has; this is how the replicated WAL uses the one
 * to its peer. A message that cannot be sent, because no connection to the peer is open, is simply not
 * sent: the replicated WAL learns that the connection has gone from on_peer_connection_lost().
 */
class ReplicationLinksInterface {
  public:
    virtual ~ReplicationLinksInterface() = default;
    /// Whether a connection to the peer is open.
    [[nodiscard]] virtual bool peer_connected() const = 0;
    /// As the leader: one record, to the follower.
    virtual void send_record_to_peer(const ReplicatedRecord& record) = 0;
    /// As the follower: that a record has been written. Sent on the connection the record arrived on, so
    /// that the leader's records and their acknowledgements travel on one connection, as a request and
    /// its answer do.
    virtual void send_ack_to_peer(const ConnectionID& on, const WalAck& ack) = 0;
    /// As the follower: where its WAL ends, so that the leader can say where the two agree.
    virtual void send_position_request_to_peer(const LogPositionRequest& request) = 0;
    /// As the leader: where the two WALs agree, or that the follower must ask again.
    virtual void send_position_reply_to_peer(const LogPositionReply& reply) = 0;
};

/**
 * @brief How a ReplicatedWal has the records a rejoining follower lacks sent from a thread of its own.
 *        Implemented by the application, with a WalFollowerService and a WalReadingThread.
 *
 * Without it, the leader reads those records itself, on its own thread, which stops it handling anything
 * else for as long as the reading takes (docs/availability/following_the_wal.md, section 4.1, and
 * docs/bug_list.md, BUG-0155). With it, the leader asks for them to be sent and carries on; the
 * application calls ReplicatedWal::on_peer_caught_up() once the follower has been sent every record and
 * handed over, and from then on the replicated WAL sends it each new record itself.
 */
class PeerCatchUpInterface {
  public:
    virtual ~PeerCatchUpInterface() = default;

    /// Start sending the follower every record after @p after_seq_no, up to the last written, on the
    /// connection the replicated WAL sends its records on.
    virtual void start_peer_catch_up(int64_t after_seq_no) = 0;

    /// Stop sending to the follower: its connection has gone, or this instance has stopped leading.
    virtual void stop_peer_catch_up() = 0;

    /// Whether nothing is reading the WAL for a follower, so that it may be cut back.
    [[nodiscard]] virtual bool reading_has_stopped() const = 0;
};

/**
 * @brief What a ReplicatedWal tells the application. Implemented by the application.
 *
 * Every call is made on the thread that owns the replicated WAL, from inside one of its functions.
 */
class ReplicatedWalEventsInterface {
  public:
    virtual ~ReplicatedWalEventsInterface() = default;

    /// This instance's role has changed. Called after the replicated WAL has done what the change needs
    /// of it, so that, on becoming leader, next_seq_no() is already above every record the WAL holds.
    virtual void role_changed(ReplicaRole from, ReplicaRole to, int32_t epoch) = 0;

    /// As the leader: every record numbered up to @p seq_no is committed, which means it is held on both
    /// machines, or a voter has confirmed that the follower may not lead. Called each time the position
    /// advances; it never goes back.
    virtual void committed_through(int64_t seq_no) = 0;

    /// As the follower: the record numbered @p seq_no, which the leader sent, has been written. @p entry
    /// is valid only during the call.
    virtual void written_as_follower(int64_t seq_no, const WalEntryContents& entry) = 0;

    /// As the follower: every record after @p seq_no has been discarded, because the leader does not hold
    /// them; the leader sends others under the same numbers.
    virtual void discarded_after(int64_t seq_no) = 0;

    /// This instance has granted its peer a lease, so the peer leads until at least a lease period after
    /// @p at.
    virtual void granted_lease_to_peer(std::chrono::steady_clock::time_point at) = 0;
};

/**
 * @brief Promises kept somewhere other than the WAL's directory: for a test that keeps them in memory and
 *        writes them at once, so that it can control the time.
 */
struct PromisesKeptElsewhere {
    /// Where to write promise records. Must outlive the ReplicatedWal.
    BackgroundPromiseRecorderInterface* recorder{nullptr};
    /// The promise recorded before this instance last stopped, if any.
    std::optional<LeasePromiseStore::Record> kept;
    /// The statement recorded before this instance last stopped.
    LeaderStatement kept_statement{};
};

/// How a ReplicatedWal is set up. Read by the application from its configuration.
struct ReplicatedWalConfiguration {
    /// The name the Quill log lines start with, such as "SequencerThread".
    std::string owner_name;
    /// Where the WAL's segments live. The epoch and the promise records are kept in the same directory.
    std::string wal_directory;
    /// The size of each segment, in bytes.
    size_t wal_segment_size{0};
    /// The processors the WAL's helper thread may run on; empty to leave it where it lands.
    std::vector<CpuId> helper_cores;
    /// False for an instance that runs alone: it leads at once, and every record is committed once written.
    bool ha_enabled{true};
    /// The number of the pair, which every voter in the pair is configured with.
    int32_t pair_id{0};
    /// This instance's id: 1 for the primary, 2 for the secondary.
    int64_t instance_id{0};
    /// The third voter's id, which appears on its grants and refusals.
    int64_t third_voter_id{0};
    /// How the third voter is named in the Quill log, such as "the arbiter".
    std::string third_voter_name;
    /// The lease timings, which every voter shares.
    LeaseTiming lease{};
    /// Where to record how long each write to the WAL takes. Left as constructed, nothing is recorded.
    HistogramHandle wal_append_histogram{};
    /// Set to 1 while the leader carries on without waiting for a follower that is connected but too far
    /// behind, and to 0 otherwise. Left as constructed, nothing is set.
    GaugeHandle running_alone_gauge{};
};

/**
 * @brief A write-ahead log (WAL) kept on both instances of a pair: written and numbered by the leader,
 *        copied to the follower, and committed once both hold it.
 *
 * This is the library's replicated WAL, described in docs/availability/replicated_wal_in_the_library.md.
 * An application that wants one holds it as a member of its application thread, hands it records to
 * write, passes it the messages that arrive from its peer and its voters, calls its two tick functions,
 * and acts on what it is told through ReplicatedWalEventsInterface. Everything else is done here:
 *
 * - **On the leader**, each record is numbered and written, and sent to the follower once the two WALs
 *   are known to agree. A record is committed once the follower has acknowledged it.
 * - **On the follower**, each record the leader sends is written if it is the next one the WAL needs,
 *   and acknowledged. One that would leave a gap stops the writing until the two WALs are found to
 *   agree again.
 * - **Carrying on alone.** With no follower connected, or a follower that has acknowledged nothing for
 *   follower_acknowledgement_timeout while records wait, the leader states that the follower may not
 *   lead, and commits records the follower lacks only once a voter has confirmed that statement. When
 *   the follower holds every record again, the leader says that it may lead
 *   (docs/availability/a_follower_behind_does_not_lead.md).
 * - **Repair.** A rejoining follower asks where the two WALs agree, by the number and epoch of its last
 *   record; it discards what the leader does not hold, and the leader sends it everything after the
 *   point of agreement (docs/availability/follower_log_repair.md).
 * - **Who leads**, through a PairLeaseAgent (docs/availability/majority_leases.md). The epoch is kept on
 *   disk in an EpochStore and the promises in a LeasePromiseStore, both in the WAL's directory.
 *
 * Use: one thread, the one that owns this object, calls every function, and every call to the events
 * interface is made on it. Nothing here takes a lock or waits for another thread (principles F1 and F2
 * in docs/design_principles.md). Every function that depends on the time is given it, so that the
 * application passes the steady clock and a test passes a time of its own.
 */
class ReplicatedWal {
  public:
    using Clock = std::chrono::steady_clock;

    /// How long the leader waits, while records wait for the follower, for any acknowledgement before it
    /// treats the follower as too far behind. A follower on the same network acknowledges in tens of
    /// microseconds, so a tenth of a second without one means it has stalled.
    static constexpr std::chrono::milliseconds follower_acknowledgement_timeout{100};

    /// How often on_acknowledgement_watch() is meant to be called: a tenth of the timeout above, so that a
    /// stalled follower is noticed between 100 and 110 milliseconds after its last acknowledgement.
    static constexpr std::chrono::milliseconds acknowledgement_watch_interval{10};

    /// How often a follower whose WAL is not known to agree with its leader's asks again.
    static constexpr std::chrono::seconds position_request_interval{1};

    ~ReplicatedWal() = default;

    /**
     * @param[in] configuration How this instance is set up. Copied.
     * @param[in] logger Where to log. Must outlive this object.
     * @param[in] links How to send to the peer. Must outlive this object.
     * @param[in] lease_links How the lease rules reach the other two voters. Must outlive this object.
     * @param[in] events What to tell the application. Must outlive this object.
     * @param[in] promises_elsewhere For a test: where promises are kept instead. Without it, they are kept in
     *            a LeasePromiseStore in the WAL's directory and written on a thread of their own.
     */
    ReplicatedWal(ReplicatedWalConfiguration configuration, QuillLogger& logger, ReplicationLinksInterface& links, LeaseLinksInterface& lease_links,
                  ReplicatedWalEventsInterface& events, std::optional<PromisesKeptElsewhere> promises_elsewhere = std::nullopt);

    /**
     * @brief Have a rejoining follower's missing records sent from a thread of its own, through
     *        @p catch_up, rather than read on this thread. Call before open().
     */
    void use_peer_catch_up(PeerCatchUpInterface& catch_up) {
        peer_catch_up_ = &catch_up;
    }

    /**
     * @brief Report each change of role to the test that started the component, through @p channel
     *        (docs/availability/test_events.md). Call before open(). Without it, nothing is reported.
     */
    void report_test_events_to(TestEventChannel& channel) {
        test_events_ = &channel;
    }

    /**
     * @brief Report the present role as a test event, as a change from unknown, when it is known.
     *
     * For the owner to call when the connection to the test is made: a change of role made before it was is not
     * reported when it happens, and the test must still learn the role.
     */
    void report_role_to_test_events();

    /**
     * @brief Publish the WAL's positions to @p positions as they change: the position after each record
     *        written, and the last committed record. For a WalReadingThread. Call before open().
     */
    void publish_positions_to(WalFollowingPositions& positions) {
        positions_ = &positions;
    }

    /**
     * @brief The follower has been sent every record up to @p through by the catch-up and handed over:
     *        the replicated WAL sends it each new record from now on. Only through PeerCatchUpInterface.
     */
    void on_peer_caught_up(int64_t through, Clock::time_point now);

    /**
     * @brief The catch-up has confirmed that it has stopped sending to the follower: a request from the
     *        follower left unanswered until then is answered now. Only through PeerCatchUpInterface.
     */
    void on_peer_catch_up_stopped(Clock::time_point now);

    ReplicatedWal(const ReplicatedWal&) = delete;
    ReplicatedWal& operator=(const ReplicatedWal&) = delete;
    ReplicatedWal(ReplicatedWal&&) = delete;
    ReplicatedWal& operator=(ReplicatedWal&&) = delete;

    /**
     * @brief Opens the WAL, trusting it only up to its first gap, and starts deciding who leads.
     *
     * Every record is read, to build the table of epochs and to find a gap. Records after a gap are
     * discarded: the leader sends them again. With high availability off, this instance leads at once,
     * and role_changed() is called before this returns.
     *
     * @return The number of the last record the WAL holds, or zero for an empty WAL.
     */
    int64_t open(Clock::time_point now);

    // ---- As the leader ----------------------------------------------------------------------------

    /// The number the next record will carry. A record that carries its number inside its own bytes is
    /// encoded with this number before it is handed to append().
    [[nodiscard]] int64_t next_seq_no() const {
        return next_seq_no_;
    }

    /**
     * @brief As the leader: numbers a record with next_seq_no(), writes it, and sends it to the follower.
     *
     * Commits it at once when this instance runs alone with high availability off, or when no follower
     * keeps up and a voter has confirmed that the follower may not lead. Otherwise it is committed once
     * the follower acknowledges it. committed_through() may be called before this returns.
     *
     * @return The number the record was given.
     */
    int64_t append(int16_t pdu_id, const uint8_t* bytes, size_t size, int64_t wall_time_ns, Clock::time_point now);

    /**
     * @brief As the leader: stop waiting for a follower the application judges too far behind.
     *
     * The application may know this before any timeout does, for example when its own storage for
     * commands waiting on the follower is full. The leader then carries on as if alone until the follower
     * has acknowledged every record.
     */
    void follower_too_far_behind(const char* reason, Clock::time_point now);

    /// As the leader: the last record committed.
    [[nodiscard]] int64_t committed_through() const {
        return committed_through_;
    }

    /// As the leader: whether records are committed only once the follower acknowledges them, because a
    /// follower is connected, its WAL agrees with this one, and it keeps up.
    [[nodiscard]] bool waits_for_follower() const;

    /// As the leader: whether records the follower lacks wait for a voter to confirm that the follower may
    /// not lead, because no follower keeps up and no voter has yet echoed that statement.
    [[nodiscard]] bool waits_for_confirmation() const {
        return configuration_.ha_enabled && !waits_for_follower() && !may_commit_without_follower();
    }

    // ---- Messages from the peer and the voters ----------------------------------------------------

    /**
     * @brief Handles one message, if it is one of the library's replication or lease messages.
     * @return True if it was, whether or not it changed anything; false if it is not one of them, and
     *         the application should handle it itself.
     */
    bool handle_message(const ConnectionID& from, int16_t pdu_id, const uint8_t* payload, size_t size, Clock::time_point now);

    void handle_record(const ConnectionID& from, const ReplicatedRecordView& record, Clock::time_point now);
    void handle_ack(int64_t seq_no, Clock::time_point now);
    void handle_position_request(int64_t last_seq_no, int32_t last_epoch, Clock::time_point now);
    void handle_position_reply(const LogPositionReplyView& reply, Clock::time_point now);
    void handle_lease_request(const ConnectionID& from, const LeaseRequestView& request, Clock::time_point now);
    void handle_lease_grant(const LeaseGrantView& grant, Clock::time_point now);
    void handle_lease_refusal(const LeaseRefusalView& refusal, Clock::time_point now);

    /// A connection to the peer has been established.
    void on_peer_connection_established(Clock::time_point now);
    /// The connection to the peer has gone.
    void on_peer_connection_lost(Clock::time_point now);

    // ---- Ticks ------------------------------------------------------------------------------------

    /// Applies the lease rules. Called every LeaseTiming::tick_interval.
    void on_lease_tick(Clock::time_point now);
    /// Notices a follower that has stopped acknowledging. Called every acknowledgement_watch_interval.
    void on_acknowledgement_watch(Clock::time_point now);

    // ---- What it holds ----------------------------------------------------------------------------

    [[nodiscard]] ReplicaRole role() const {
        return role_;
    }
    [[nodiscard]] int32_t epoch() const {
        return epoch_;
    }
    /// The last record the WAL holds.
    [[nodiscard]] int64_t last_seq_no() const {
        return log_epochs_.last_seq_no();
    }
    /// The WAL itself, for reading: where a record starts, which segments exist, its directory.
    [[nodiscard]] const Wal& wal() const {
        return wal_;
    }
    /// Records where the WAL has reached, so that a restart need not read all of it. Deletes nothing.
    void take_snapshot() {
        wal_.take_snapshot();
    }
    /// Whether this instance runs alone, with high availability off.
    [[nodiscard]] bool alone() const {
        return !configuration_.ha_enabled;
    }

  private:
    void write(int64_t seq_no, int32_t leader_epoch, int16_t pdu_id, const uint8_t* bytes, size_t size, int64_t wall_time_ns);
    void act_on(PairLeaseAgent::Change change, Clock::time_point now);
    void change_role(ReplicaRole new_role, Clock::time_point now);
    void set_epoch(int32_t new_epoch);
    void forget_agreement();
    void send_position_request(Clock::time_point now);
    void say_follower_may_not_lead(Clock::time_point now);
    void say_follower_may_lead_if_it_holds_everything(Clock::time_point now);
    void start_running_alone(const char* reason, Clock::time_point now);
    void stop_running_alone(const char* reason, Clock::time_point now);
    [[nodiscard]] bool may_commit_without_follower() const;
    void advance_commit();

    ReplicatedWalConfiguration configuration_;
    QuillLogger& logger_;
    ReplicationLinksInterface& links_;
    LeaseLinksInterface& lease_links_;
    ReplicatedWalEventsInterface& events_;

    Wal wal_;
    // Which leadership wrote each record of the WAL. Extended on every write, cut back when a follower
    // discards records.
    LogEpochTable log_epochs_;

    EpochStore epoch_store_;
    LeasePromiseStore promise_store_;
    // Writes promise records on a thread of its own, when no recorder was given. Declared after the store
    // it writes to, so that it is destroyed first.
    std::optional<BackgroundPromiseRecorder> owned_promise_recorder_;
    std::optional<PromisesKeptElsewhere> promises_elsewhere_;
    std::optional<PairLeaseAgent> lease_agent_;

    ReplicaRole role_{ReplicaRole::unknown};
    // The leadership generation. Changed only through set_epoch(), which also writes it to disk.
    int32_t epoch_{0};

    // The number the next record this instance writes as leader will carry.
    int64_t next_seq_no_{1};
    // The highest number of any record this instance has written as a follower.
    int64_t highest_replicated_seq_no_{0};

    // As the follower: whether this WAL and the leader's are known to agree, so that the records it sends
    // may be written; and when this instance last asked where they agree.
    bool follower_wal_agreed_{false};
    Clock::time_point last_position_request_at_{};

    // As the leader: whether the follower's WAL is known to agree with this one, so that live records are
    // sent to it; the highest number it has acknowledged; and when it last acknowledged anything.
    bool follower_wal_matches_{false};
    int64_t peer_acked_through_{0};
    // While the catch-up sends a rejoining follower the records it lacks, this instance sends it nothing
    // itself, and is not waited for; once handed over, it is waited for again only once it has
    // acknowledged every record up to where it was handed over, because until then new records would
    // wait behind the whole catch-up (BUG-0155).
    bool peer_catching_up_{false};
    int64_t peer_catch_up_after_{0}; // the record after which the catch-up started
    int64_t peer_handed_over_at_{0};
    PeerCatchUpInterface* peer_catch_up_{nullptr};
    // A request from the follower left unanswered until the catch-up has confirmed that it has stopped.
    struct UnansweredRequest {
        int64_t last_seq_no{0};
        int32_t last_epoch{0};
    };
    std::optional<UnansweredRequest> unanswered_request_;
    WalFollowingPositions* positions_{nullptr};
    TestEventChannel* test_events_{nullptr};
    // While records wait for the follower: since when, for the timeout. Reset to the time of each
    // acknowledgement that leaves records still waiting.
    std::optional<Clock::time_point> waiting_since_;

    // As the leader: carrying on without waiting for a follower that is connected but too far behind.
    bool running_alone_{false};
    Clock::time_point running_alone_since_{};

    // As the leader: the last record committed. Only ever advances within one leadership.
    int64_t committed_through_{0};
};

} // namespaces
