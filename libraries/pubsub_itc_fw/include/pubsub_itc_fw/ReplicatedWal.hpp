#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/BackgroundPromiseRecorder.hpp>
#include <pubsub_itc_fw/BackgroundPromiseRecorderInterface.hpp>
#include <pubsub_itc_fw/CommandIdentityInterface.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/CpuPinning.hpp>
#include <pubsub_itc_fw/EpochRecording.hpp>
#include <pubsub_itc_fw/GaugeHandle.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/LeaseLinksInterface.hpp>
#include <pubsub_itc_fw/LeasePromiseStore.hpp>
#include <pubsub_itc_fw/LeaseTiming.hpp>
#include <pubsub_itc_fw/LogEpochTable.hpp>
#include <pubsub_itc_fw/LoggedCommandIdentifiers.hpp>
#include <pubsub_itc_fw/LoggedCommandIdentifiersBuilder.hpp>
#include <pubsub_itc_fw/PairLeaseAgent.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TestEventChannel.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalCommandSearch.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>

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

/// Where a replicated WAL holds a command.
struct CommandInWal {
    /// The number of the record that holds the command.
    int64_t record_id{0};
    /// Where that record starts. Known when the record of command identifiers proved the match by reading the record
    /// at the position it keeps; not known when the WAL was searched.
    std::optional<WalPosition> position;
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

    /// While open() reads the whole WAL: the record numbered @p seq_no, which the WAL held when it was opened. Called
    /// for each record in order, up to any gap, so that the application can keep what it needs from the records
    /// without reading the WAL again. @p entry is valid only during the call.
    virtual void read_at_open(int64_t seq_no, const WalEntryContents& entry) = 0;

    /// As the leader: the first time since it took the lead that holds_command() has had to prove whether the
    /// WAL holds a command, because the record of identifiers said that it may. @p last_seq_no is the last
    /// record the WAL holds. Called once in each leadership, not for each command.
    virtual void checking_for_commands_already_held(int64_t last_seq_no) = 0;
};

/**
 * @brief A promise recorder and previously stored promises supplied by the caller.
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
    /// Where to record how long recording a new epoch on disk takes, in milliseconds (BUG-0228). Left as constructed,
    /// nothing is recorded.
    HistogramHandle epoch_record_milliseconds{};
    /// Where to record how much of the lease was left, in milliseconds, when this instance acted on taking the lead,
    /// after recording its new epoch; zero if none was. Left as constructed, nothing is recorded.
    HistogramHandle lease_left_when_leading_milliseconds{};
    /// What the lease agent records: how long the promise record's background writes take, and how many leases ran out.
    LeaseMetrics lease_metrics{};
    /// Set, when the WAL is opened, to the longest of a few trial records of the epoch, in milliseconds: how fast the
    /// WAL's disk syncs (EpochRecording::check_disk). Left as constructed, nothing is set.
    GaugeHandle disk_check_worst_milliseconds{};
    /// Set to 1 while this instance leads and commits records without waiting for its follower, whatever the reason:
    /// no follower is connected, the follower's WAL is not yet known to agree with this one, the follower is being
    /// sent records it missed, or it is too far behind. Set to 0 otherwise, which includes every instance that follows
    /// and every instance with ha_enabled false. Left as constructed, nothing is set.
    GaugeHandle running_alone_gauge{};
    /// How a command's identity is read from a record. When set, the WAL keeps a record of the identity and the
    /// position of every command it holds, and holds_command() says whether it holds one
    /// (docs/availability/one_check_against_sequencing_twice.md). Left null, it keeps none. Must outlive the WAL.
    const CommandIdentityInterface* command_identity{nullptr};
    /// How many identities that record has room for. It is reserved in full when it is built.
    size_t identifiers_reserved{0};
    /// How an identity becomes a number in that record. Replaced only by a test, to make two identities collide.
    LoggedCommandIdentifiers::NumberFunction command_number{&LoggedCommandIdentifiers::identifier};
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
 *   disk through EpochRecording, which times each record, and the promises in a LeasePromiseStore, both in the WAL's directory.
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

    /// As the leader: whether records the follower lacks wait for confirmation that the follower may not
    /// lead, because no follower keeps up, and neither the follower nor both arbiters yet hold that statement.
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
    void handle_statement_held(const StatementHeldView& held);
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

    /**
     * @brief Starts building the record of the commands the WAL holds, from the WAL as it stands, on a thread of
     *        its own on the helper cores. Call once, after open(), and only with a command_identity configured.
     *
     * Until the record is built, holds_command() searches the WAL, and the commands written meanwhile are noted
     * and added when the record is taken over.
     */
    void start_building_record_of_commands();

    /// Takes the record of commands over once its thread has built it. holds_command() does this too; call it
    /// periodically as well, so that the memory the noted commands take is given back soon after startup.
    void take_record_of_commands_if_built();

    /**
     * @brief Whether the WAL holds a command with this identity
     *        (docs/availability/one_check_against_sequencing_twice.md, section 3.2).
     *
     * A number the record does not hold means no. One it holds is proved by reading the one record at the
     * position beside it and comparing identities. When that cannot settle it, because the record is full or
     * not yet built, or the record at the position is another command, the WAL is searched (WalCommandSearch).
     *
     * @param[in] identity      The command's identity, as the configured CommandIdentityInterface writes it.
     * @param[in] not_before_ns The command was first sent no earlier than this, by the sender's clock, which
     *                          bounds a search. The smallest possible value lets a search read the whole WAL.
     * @return false when no command_identity is configured.
     */
    [[nodiscard]] bool holds_command(std::string_view identity, int64_t not_before_ns);

    /**
     * @brief As holds_command(), and where: the number of a record in the WAL holding a command with this identity.
     *
     * When the record of identifiers proves the match, the record is the one at its stored position, which is the
     * first the WAL was given for that identity's number. When the WAL is searched, it is the earliest record the
     * search has read that holds the command.
     *
     * @param[in] identity      The command's identity, as the configured CommandIdentityInterface writes it.
     * @param[in] not_before_ns As for holds_command().
     * @return The record's number and, when the record of identifiers proved the match, its position; or nothing when
     *         the WAL does not hold the command or no command_identity is configured.
     */
    [[nodiscard]] std::optional<CommandInWal> find_command(std::string_view identity, int64_t not_before_ns);

    /// Whether a search of the WAL is being kept for holds_command(), with what it has read.
    [[nodiscard]] bool command_search_in_use() const {
        return command_search_.has_value();
    }

    /// Discards the search kept for holds_command(), giving back its memory, and returns how many records it
    /// read. A later search starts again from the end of the WAL. The WAL discards it itself on ceasing to lead.
    int64_t discard_command_search();

    /// The record of commands, once built and taken over; null before.
    [[nodiscard]] const LoggedCommandIdentifiers* record_of_commands() const {
        return commands_.has_value() ? &*commands_ : nullptr;
    }

    /// How holds_command() has settled each command it was asked about.
    struct CommandCheckCounts {
        int64_t not_held_by_number{0};        ///< the record held no such number
        int64_t proved_by_reading{0};         ///< the record at the position was the command
        int64_t searched{0};                  ///< the WAL was searched
        int64_t other_command_at_position{0}; ///< of those searched, how many because the record at the position was another command
    };
    [[nodiscard]] const CommandCheckCounts& command_check_counts() const {
        return command_check_counts_;
    }

    /// Whether this instance's lease agent says that its follower may lead, which a leader says once the follower holds every record.
    [[nodiscard]] bool says_follower_may_lead() const {
        return lease_agent_.has_value() && lease_agent_->says_peer_may_lead();
    }

  private:
    void write(int64_t seq_no, int32_t leader_epoch, int16_t pdu_id, const uint8_t* bytes, size_t size, int64_t wall_time_ns);
    // The body of handle_message, which handle_message follows with note_whether_the_follower_is_waited_for.
    bool dispatch_message(const ConnectionID& from, int16_t pdu_id, const uint8_t* payload, size_t size, Clock::time_point now);
    void act_on(PairLeaseAgent::Change change, Clock::time_point now);
    void change_role(ReplicaRole new_role, Clock::time_point now);
    void set_epoch(int32_t new_epoch);
    void forget_agreement();
    void send_position_request(Clock::time_point now);
    void say_follower_may_not_lead(Clock::time_point now);
    void say_follower_may_lead_if_it_holds_everything(Clock::time_point now);
    void start_running_alone(const char* reason, Clock::time_point now);
    void stop_running_alone(const char* reason, Clock::time_point now);
    // Sets running_alone_gauge, and logs, when this instance starts or stops committing records without waiting for
    // its follower, and logs when the reason changes. Called at the end of everything that can change the answer.
    void note_whether_the_follower_is_waited_for(Clock::time_point now);
    [[nodiscard]] bool may_commit_without_follower() const;
    void advance_commit();
    // Notes the identity and position of the command a record just written holds, if it holds one.
    void note_command_written(WalPosition position, const WalEntryContents& entry);
    void add_command(uint64_t number, WalPosition position);
    // Whether the record at @p position holds the command with @p identity.
    [[nodiscard]] std::optional<int64_t> command_at(WalPosition position, std::string_view identity);
    [[nodiscard]] std::optional<int64_t> search_for_command(std::string_view identity, int64_t not_before_ns);

    ReplicatedWalConfiguration configuration_;
    QuillLogger& logger_;
    ReplicationLinksInterface& links_;
    LeaseLinksInterface& lease_links_;
    ReplicatedWalEventsInterface& events_;

    Wal wal_;
    // Which leadership wrote each record of the WAL. Extended on every write, cut back when a follower
    // discards records.
    LogEpochTable log_epochs_;

    EpochRecording epoch_recording_;
    // Writes the epoch this instance asks to lead in before the request is sent, through epoch_recording_.
    EpochRecordedBeforeAsking epoch_recorded_before_asking_;
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
    // Sends a test event, if the owner gave this instance a channel (report_test_events_to).
    template <typename Message> void report(const Message& message) {
        if (test_events_ != nullptr) {
            test_events_->send_event(Message::message_pdu_id, message);
        }
    }
    // While records wait for the follower: since when, for the timeout. Reset to the time of each
    // acknowledgement that leaves records still waiting.
    std::optional<Clock::time_point> waiting_since_;

    // As the leader: carrying on without waiting for a follower that is connected but too far behind.
    bool running_alone_{false};
    Clock::time_point running_alone_since_{};

    // As the leader: committing records without waiting for the follower, for any of the reasons running_alone_gauge
    // lists; since when; and the reason last logged, so that a change of reason is logged once. The reason always
    // views a string literal.
    bool committing_without_follower_{false};
    Clock::time_point committing_without_follower_since_{};
    std::string_view without_follower_reason_;

    // As the leader: the last record committed. Only ever advances within one leadership.
    int64_t committed_through_{0};

    // The record of the commands the WAL holds, each with its position, once built and taken over
    // (docs/availability/one_check_against_sequencing_twice.md); and what builds it at startup.
    std::optional<LoggedCommandIdentifiers> commands_;
    LoggedCommandIdentifiersBuilder commands_builder_;
    bool commands_building_{false};
    // The commands written while the record was being built: each one's number and position.
    std::vector<std::pair<uint64_t, WalPosition>> commands_noted_while_building_;
    bool commands_full_reported_{false};
    // The search for commands the record cannot settle, kept while it is in use.
    std::optional<WalCommandSearch> command_search_;
    // Whether this leadership has reported checking_for_commands_already_held.
    bool command_check_reported_{false};
    CommandCheckCounts command_check_counts_;
    // Space for reading identities on this thread.
    std::vector<uint8_t> identity_scratch_ = std::vector<uint8_t>(CommandIdentityInterface::scratch_size);
    std::array<char, CommandIdentityInterface::max_identity_size> identity_buffer_{};
};

} // namespaces
