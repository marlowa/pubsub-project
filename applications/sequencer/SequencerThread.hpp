#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/FixedCapacityRingBuffer.hpp>
#include <pubsub_itc_fw/GaugeHandle.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <replication_messages.hpp>
#include <sequencer_test_events.hpp>

#include <pubsub_itc_fw/ReplicatedWal.hpp>
#include <pubsub_itc_fw/TestEventChannel.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalFollowerService.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>
#include <pubsub_itc_fw/WalHandedOverFollowers.hpp>

#include "EngineAbsenceRule.hpp"
#include "EngineOrderRouting.hpp"
#include "EngineRecordFromWal.hpp"
#include "EngineReportPosition.hpp"
#include "EngineRoutedStamp.hpp"
#include "GatewayIds.hpp"
#include "KeptReportStore.hpp"
#include "MemberSessions.hpp"
#include "RecoveryLimitRule.hpp"
#include "ReportForwarding.hpp"
#include "ReportsHeldForUnknownSessions.hpp"
#include "SeqNumRanges.hpp"
#include "SequencerCommandIdentity.hpp"
#include "SequencerConfiguration.hpp"
#include "SessionIdentity.hpp"
#include "TradingHaltState.hpp"
#include "VenueStateFromTheWal.hpp"
#include "VoidFromWal.hpp"
#include "VoidedCommandList.hpp"
#include <pubsub_itc_fw/LeaseLinksInterface.hpp>

namespace sequencer {

/**
 * @brief ApplicationThread subclass implementing the sequencer business logic.
 *
 * The sequencer is the sole writer to the matching engine's input stream. It
 * imposes a total order on all inbound order PDUs by stamping a monotonically
 * increasing sequence number and wrapping each PDU in a SequencedMessage
 * envelope before forwarding to the ME.
 *
 * Only the leader forwards to the ME. The follower receives PDUs from the
 * gateway (staying in sync) but does not forward. On promotion the follower
 * begins forwarding from the next sequence number with no gaps.
 *
 * The sequencer is the sole chokepoint for all traffic in both directions.
 * Order PDUs from gateways are sequenced and forwarded to the ME. ER PDUs
 * from the ME are forwarded back to the originating gateway. This matches
 * the Aeron cluster ingress/egress pattern exactly.
 *
 * Threading: ThreadID 1.
 */
class SequencerThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @param[in] token    Constructor token to force use of factory.
     * @param[in] logger   Logger. Must outlive this object.
     * @param[in] reactor  Owning Reactor. Must outlive this object.
     * @param[in] config   Sequencer configuration.
     * @param[in] wal_positions The positions this thread publishes for the WAL's reading thread. Must
     *                          outlive this object.
     */
    SequencerThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                    const SequencerConfiguration& config, pubsub_itc_fw::WalFollowingPositions& wal_positions);

    /// The thread that reads the WAL for its followers (docs/availability/following_the_wal.md).
    static constexpr pubsub_itc_fw::ThreadID wal_reading_thread_id{2};
    /// The reading thread's converter for the follower sequencer: each record as a ReplicatedRecord.
    static constexpr int peer_converter = 0;
    /// The reading thread's converter for a matching engine: each order and cancel as the WalRecord
    /// engine_record_from_wal builds, and nothing else.
    static constexpr int engine_converter = 1;
    /// The reading thread's converter for a matching engine publisher: every record, as the WalRecord envelope
    /// the WAL holds (publisher_record_from_wal).
    static constexpr int publisher_converter = 2;

  protected:
    void on_initial_event() override;
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    // The bytes of the listener for operators, which is TLS: each whole OperatorHaltRequest is dealt with as it arrives.
    void on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;
    bool prioritise_data_over_timers() const override {
        return true;
    }

  private:
    const SequencerConfiguration& config_;

    // Precomputed inbound service name strings derived from config ports.
    // Used in on_framework_pdu_message to classify inbound PDUs without
    // constructing strings on every call.
    const std::string order_inbound_svc_;
    const std::string er_inbound_svc_;
    const std::string wal_subscriber_inbound_svc_;

    // How ReportForwarding sends reports, reaches the member sessions, and tells the test.
    class ReportLinks : public ReportForwardingLinksInterface {
      public:
        explicit ReportLinks(SequencerThread& owner) : owner_(owner) {}
        void send_report(const pubsub_itc_fw::ConnectionID& connection, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope) override;
        void send_encoded_report(const pubsub_itc_fw::ConnectionID& connection, int64_t er_seq_no, const uint8_t* bytes, size_t size) override;
        void acknowledgement_sent(const pubsub_itc_fw_app::WalRecord& envelope) override;
        [[nodiscard]] bool leading() const override;
        [[nodiscard]] bool expects_no_session_for(std::string_view comp_id) const override;
        [[nodiscard]] const fix_common::SessionDestination* destination(const fix_common::SessionIdentity& identity) const override;
        void report_forwarded(const fix_common::SessionIdentity& identity) override;
        void report_dropped_unbound(const fix_common::SessionIdentity& identity, int64_t er_seq_no) override;
        void order_ended(int64_t seq_no) override;
        void reports_held(const fix_common::SessionIdentity& identity, int32_t gateways_not_announced) override;
        void held_reports_released(std::string_view comp_id, int16_t protocol, int64_t sent, int64_t dropped) override;
        void report_dropped_gateway_not_connected(int16_t protocol, int16_t instance, int64_t er_seq_no) override;
        void reports_held_for_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no) override;
        void reports_sent_to_gateway(int16_t protocol, int16_t instance, int64_t sent, int64_t dropped) override;
        void held_reports_dropped(int64_t total) override;

      private:
        SequencerThread& owner_;
    };
    ReportLinks report_links_{*this};
    // The configured gateway instances, as protocol and instance.
    static std::vector<std::pair<int16_t, int16_t>> configured_gateways(const SequencerConfiguration& config);
    // Getting each report from the matching engine to its member's gateway: the reports waiting for their record to be
    // committed, held for unannounced sessions, or held for gateways with no connection for reports, the gateway
    // connections, and how far reports have been forwarded (section 6.11 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md).
    ReportForwarding reports_{configured_gateways(config_), report_links_, get_logger()};
    // The count of reports held for unannounced sessions that were dropped to make room, which ReportForwarding keeps.
    pubsub_itc_fw::GaugeHandle held_reports_dropped_gauge_;

    // Which of this sequencer's order connections to the matching engines carries orders: the one to
    // the instance that leads, as the engines' announcements and requests to catch up establish. It
    // keeps the order connection to each instance while that connection is open, so a request to
    // catch up from either engine is always recognised (BUG-0108). See EngineOrderRouting.hpp.
    EngineOrderRouting engine_routing_;

    // How long the venue has had no matching engine that can trade, for the limit on automatic recovery below.
    // EngineAbsenceRule measures it; the stamp beside the WAL says when this process last knew of an engine it routed
    // to, for when the whole venue was down. Checked on engine_absence_timer_id_ about once a second.
    EngineAbsenceRule engine_absence_;
    EngineRoutedStamp engine_routed_stamp_;
    static constexpr std::chrono::seconds engine_absence_check_interval{1};

    // The longest recovery the venue does by itself: while no engine is routed to, when that time and how long the
    // catch-up under way will still take together pass the limit, the leading sequencer voids the orders it holds,
    // cancels the book and halts trading, and tells the gateways to refuse orders with the reason
    // (docs/availability/far_back_catch_up.md, section 5; docs/availability/voided_commands.md, section 5.3). RecoveryLimitRule decides when, given the absence
    // engine_absence_ measures and the progress of the engine's catch-up, which the reading thread reports. Checked with the absence.
    RecoveryLimitRule recovery_limit_{std::chrono::seconds(config_.automatic_recovery_limit_seconds)};
    // How far the catch-up of a matching engine that asked to be caught up has been sent, while one is under way.
    std::optional<RecoveryLimitRule::CatchUpProgress> engine_catch_up_progress_;
    pubsub_itc_fw::ConnectionID engine_catch_up_connection_{};

    // ConnectionIDs of the outbound peer and arbiter connections.
    pubsub_itc_fw::ConnectionID peer_conn_id_;
    pubsub_itc_fw::ConnectionID peer_inbound_conn_id_; // inbound: peer connected to us
    pubsub_itc_fw::ConnectionID arbiter_primary_conn_id_;
    pubsub_itc_fw::ConnectionID arbiter_secondary_conn_id_;

    // instance_id of the peer sequencer, learned from StatusQuery/StatusResponse.
    int64_t peer_instance_id_{0};

    // How long committing one record to the log takes. On the order path, which is why it is
    // worth measuring: a commit that blocks is a sequencer that has stopped sequencing, and until
    // this existed the only sign of it was the reactor's stall watchdog saying a callback had not
    // finished -- a log line, correlated by hand.
    pubsub_itc_fw::HistogramHandle wal_append_histogram_;
    pubsub_itc_fw::GaugeHandle wal_segments_filled_inline_gauge_;
    pubsub_itc_fw::GaugeHandle wal_segments_waited_for_gauge_;

    // Four points on the order's journey, each recording how much of the round trip had
    // already elapsed when it got here. The sequencer contributes four of the family's
    // checkpoints because the order passes through it twice: once outbound to the matching
    // engine and once more as the report coming back.
    //
    // The pairs are what the numbers are for. order_in against order_out is what this
    // component costs an order, which includes the write-ahead log commit; er_in against
    // er_out is what it costs the report. order_out against the matching engine's order_in
    // is the hop between the two processes, which nothing else measures at all.
    //
    // See OrderPathMetrics.hpp for why they share one metric name and differ only by scope.
    pubsub_itc_fw::HistogramHandle order_in_elapsed_histogram_;
    pubsub_itc_fw::HistogramHandle order_out_elapsed_histogram_;
    pubsub_itc_fw::HistogramHandle er_in_elapsed_histogram_;
    pubsub_itc_fw::HistogramHandle er_out_elapsed_histogram_;

    // The matching engine publishers that have been caught up and handed over to new records, each sent every
    // record as it is committed, read back from the WAL (docs/availability/following_the_wal.md, section 4.3).
    // Until a publisher is handed over, the reading thread sends it what it lacks.
    class SequencerPublisherLinks : public pubsub_itc_fw::WalHandedOverFollowersLinksInterface {
      public:
        explicit SequencerPublisherLinks(SequencerThread& owner) : owner_(owner) {}
        void send_record(const pubsub_itc_fw::ConnectionID& follower, int16_t pdu_id, int64_t seq_no, const uint8_t* bytes, size_t size) override;

      private:
        SequencerThread& owner_;
    };
    SequencerPublisherLinks publisher_links_{*this};
    std::optional<pubsub_itc_fw::WalHandedOverFollowers> handed_over_publishers_;
    // The matching engines that follow the WAL rather than lead, once caught up: each is sent every record as it is
    // committed, as a publisher is, and applies the commands among them (docs/availability/both_engines_apply_the_wal.md).
    std::optional<pubsub_itc_fw::WalHandedOverFollowers> handed_over_engines_;

    // The test events this thread sends, when a test started this instance (docs/availability/test_events.md).
    class SequencerTestEventLinks : public pubsub_itc_fw::TestEventLinksInterface {
      public:
        explicit SequencerTestEventLinks(SequencerThread& owner) : owner_(owner) {}
        void send_test_event(const pubsub_itc_fw::ConnectionID& connection, int16_t pdu_id, const uint8_t* bytes, size_t size) override;

      private:
        SequencerThread& owner_;
    };
    SequencerTestEventLinks test_event_links_{*this};
    std::optional<pubsub_itc_fw::TestEventChannel> test_events_;

    // Sends one of the sequencer's own test events (sequencer_test_events.dsl), if a test started this instance.
    template <typename Message> void report(const Message& message) {
        if (test_events_.has_value()) {
            test_events_->send_event(Message::message_pdu_id, message);
        }
    }

    // This instance's role, as the replicated WAL last said it changed.
    pubsub_itc_fw_app::Role role_{pubsub_itc_fw_app::Role::unknown};

    // How the lease rules reach the other two voters in deciding which sequencer leads: the peer
    // sequencer, and the arbiter pool on both arbiter connections.
    class SequencerLeaseLinks : public pubsub_itc_fw::LeaseLinksInterface {
      public:
        explicit SequencerLeaseLinks(SequencerThread& owner) : owner_(owner) {}
        void send_request_to_peer(const pubsub_itc_fw::LeaseRequest& request) override;
        void send_request_to_third_voter(const pubsub_itc_fw::LeaseRequest& request) override;
        void send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseGrant& grant) override;
        void send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::LeaseRefusal& refusal) override;

      private:
        SequencerThread& owner_;
    };
    SequencerLeaseLinks lease_links_{*this};

    // How the replicated WAL sends to the peer sequencer: on whichever peer connection is open.
    class SequencerReplicationLinks : public pubsub_itc_fw::ReplicationLinksInterface {
      public:
        explicit SequencerReplicationLinks(SequencerThread& owner) : owner_(owner) {}
        [[nodiscard]] bool peer_connected() const override;
        void send_record_to_peer(const pubsub_itc_fw::ReplicatedRecord& record) override;
        void send_ack_to_peer(const pubsub_itc_fw::ConnectionID& on, const pubsub_itc_fw::WalAck& ack) override;
        void send_position_request_to_peer(const pubsub_itc_fw::LogPositionRequest& request) override;
        void send_position_reply_to_peer(const pubsub_itc_fw::LogPositionReply& reply) override;

      private:
        SequencerThread& owner_;
    };
    SequencerReplicationLinks replication_links_{*this};

    // What the replicated WAL tells this sequencer.
    class SequencerReplicatedWalEvents : public pubsub_itc_fw::ReplicatedWalEventsInterface {
      public:
        explicit SequencerReplicatedWalEvents(SequencerThread& owner) : owner_(owner) {}
        void role_changed(pubsub_itc_fw::ReplicaRole from, pubsub_itc_fw::ReplicaRole to, int32_t epoch) override;
        void committed_through(int64_t seq_no) override;
        void written_as_follower(int64_t seq_no, const pubsub_itc_fw::WalEntryContents& entry) override;
        void discarded_after(int64_t seq_no) override;
        void granted_lease_to_peer(std::chrono::steady_clock::time_point at) override;
        void read_at_open(int64_t seq_no, const pubsub_itc_fw::WalEntryContents& entry) override;
        void checking_for_commands_already_held(int64_t last_seq_no) override;

      private:
        SequencerThread& owner_;
    };
    SequencerReplicatedWalEvents replicated_wal_events_{*this};

    // How the replicated WAL reads a command's identity from a record, so that it keeps a record of the commands it
    // holds (docs/availability/one_check_against_sequencing_twice.md). Declared before the WAL, which uses it.
    SequencerCommandIdentity command_identity_;

    // The sequencer's write-ahead log, kept on both instances of the pair, and the deciding of which
    // instance leads: the library's replicated WAL (docs/availability/replicated_wal_in_the_library.md).
    // Constructed at the initial event, except in replay mode, which reads the log and writes nothing.
    std::optional<pubsub_itc_fw::ReplicatedWal> replicated_wal_;

    // Following the WAL (docs/availability/following_the_wal.md). The records a rejoining follower
    // sequencer lacks are read from the WAL and sent by the reading thread, not on this thread; this is
    // the replicated WAL's catch-up, the service's links to the reading thread, and what the service
    // tells this thread.
    class SequencerWalFollowing : public pubsub_itc_fw::PeerCatchUpInterface,
                                  public pubsub_itc_fw::WalFollowerLinksInterface,
                                  public pubsub_itc_fw::WalFollowerEventsInterface {
      public:
        explicit SequencerWalFollowing(SequencerThread& owner) : owner_(owner) {}
        // PeerCatchUpInterface
        void start_peer_catch_up(int64_t after_seq_no) override;
        void stop_peer_catch_up() override;
        [[nodiscard]] bool reading_has_stopped() const override;
        // WalFollowerLinksInterface
        void send_to_reading_thread(std::unique_ptr<pubsub_itc_fw::WalReadingCommand> command) override;
        void send_declined(const pubsub_itc_fw::ConnectionID& to, const pubsub_itc_fw::WalFollowDeclined& declined) override;
        void send_caught_up(const pubsub_itc_fw::ConnectionID& to, const pubsub_itc_fw::WalFollowCaughtUp& caught_up) override;
        void send_leader_changed(const pubsub_itc_fw::ConnectionID& to, const pubsub_itc_fw::WalLeaderChanged& changed) override;
        // WalFollowerEventsInterface
        void follower_handed_over(const pubsub_itc_fw::ConnectionID& follower, int64_t through, int64_t records_sent,
                                  pubsub_itc_fw::WalPosition resume_at) override;
        void hand_over_falling_behind(const pubsub_itc_fw::ConnectionID& follower) override;
        void follower_progress(const pubsub_itc_fw::ConnectionID& follower, int64_t sent_through) override;
        void follower_cannot_be_served(const pubsub_itc_fw::ConnectionID& follower, int64_t damaged_seq_no) override;
        void declined_while_not_leading(const pubsub_itc_fw::ConnectionID& follower) override;
        void reading_stopped() override;

      private:
        SequencerThread& owner_;
        // The connection the follower sequencer is being sent its missing records on, while it is.
        pubsub_itc_fw::ConnectionID catch_up_connection_;
    };
    SequencerWalFollowing wal_following_{*this};

    // A matching engine's connection that the reading thread is serving, and why. While it is, this
    // thread sends that engine no order itself: an order released meanwhile is at or below the limit the
    // reading thread serves up to, and the reading thread sends it (following_the_wal.md, section 5.3).
    struct EngineServing {
        // Whether the engine asked to be caught up with WalFollowRequest, and so is sent WalFollowCaughtUp
        // when it is handed over; otherwise it is being sent orders it lacks.
        bool answers_catch_up_request{false};
        // For a catch-up request: whether the engine asked to lead, and so has the orders routed to it
        // once it is caught up.
        bool asking_to_lead{false};
        // For a catch-up request from an engine that is not to lead: it is sent every record, not only commands, and
        // once caught up is sent each record as it is committed (handed_over_engines_).
        bool follows_the_wal{false};
        // For a catch-up request: the start of the answer, which the reading thread sends before the first record, and
        // whose request number WalFollowCaughtUp echoes. It carries the last command the WAL holds every report of,
        // which the reading thread reads from the WAL.
        std::optional<pubsub_itc_fw::WalFollowStart> started;
    };
    std::unordered_map<int, EngineServing> engine_servings_;
    // The component's background cores, from the CPU layout, for the helper threads this thread starts;
    // empty when no layout was read.
    std::vector<pubsub_itc_fw::CpuId> background_cores_;
    // Has the reading thread serve @p engine from the record after @p after_seq_no, up to the last order released.
    void serve_engine(const pubsub_itc_fw::ConnectionID& engine, int64_t after_seq_no, EngineServing serving);
    // The engine the reading thread was serving has been handed over at @p through.
    void engine_handed_over(const pubsub_itc_fw::ConnectionID& engine, int64_t through, int64_t records_sent, pubsub_itc_fw::WalPosition resume_at,
                            EngineServing serving);
    // Whether the engine orders are routed to follows the WAL instead, and so is sent every command as a follower.
    [[nodiscard]] bool routed_engine_follows_the_wal() const;
    // Publishes the last order released (released_through) as the limit a matching engine is served up to.
    void publish_released_position();

    // A record in the WAL that the reading thread could not read, which stops a follower being caught up
    // past it. Said as an Error the first time it stops each kind of follower, and as Info when the same
    // follower asks again and is refused again; and shown, as long as it stands, by a gauge holding the
    // record's number, which is 0 when there is none.
    std::unordered_set<std::string> damaged_records_reported_;
    int64_t damaged_record_standing_{0};
    pubsub_itc_fw::GaugeHandle damaged_record_gauge_;
    void report_follower_cannot_be_served(const pubsub_itc_fw::ConnectionID& follower, int64_t damaged_seq_no, const std::string& which_follower);
    // A follower has been handed over through @p through: a damaged record at or before it no longer stands.
    void note_handed_over_through(int64_t through);
    pubsub_itc_fw::WalFollowingPositions& wal_positions_;
    std::optional<pubsub_itc_fw::WalFollowerService> wal_follower_service_;
    // While set, reading orders from the gateways is paused so that a follower's hand-over can finish:
    // records were released faster than one round between the two threads (section 3.4 of the design).
    bool paused_for_hand_over_{false};
    void pause_order_reading_for_hand_over();
    void end_pause_for_hand_over();

    // The epoch the sequencer stamps on what it sends: the replicated WAL's, or 1 in replay mode.
    [[nodiscard]] int32_t current_epoch() const {
        return replicated_wal_.has_value() ? replicated_wal_->epoch() : 1;
    }

    // Timer ids (default-constructed = not scheduled); on_timer_event compares
    // a fired timer's id against these to identify it.
    pubsub_itc_fw::TimerID wal_snapshot_timer_id_{};
    pubsub_itc_fw::TimerID lease_tick_timer_id_{};
    pubsub_itc_fw::TimerID acknowledgement_watch_timer_id_{};
    pubsub_itc_fw::TimerID engine_absence_timer_id_{};

    // A gateway instance has announced its sessions on its present link to this instance (SessionsAnnounced): given to
    // ReportForwarding.
    void handle_sessions_announced(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);

    // Warn when the log has grown past this without anything having been reclaimed. Not a
    // limit: nothing is deleted on reaching it. It exists because the venue currently cannot
    // establish what is safe to discard, so the log grows for as long as it runs, and an
    // operator should learn that from a log line rather than from a full disk.
    static constexpr size_t wal_growth_warning_records = 5'000'000;

    // Reusable scratch buffer for encoding the WalRecord envelope before appending it
    // to the WAL (append_envelope_to_wal). Grown to the largest envelope seen and
    // reused -- no fixed cap that could silently fail to persist, no per-record alloc.
    std::vector<uint8_t> wal_encode_buffer_;

    // Leader-follower helpers.
    pubsub_itc_fw::ConnectionID peer_active_conn() const;
    /// Does what this sequencer must on a change of role, after the replicated WAL has done its part.
    void on_role_changed(pubsub_itc_fw_app::Role new_role, int32_t epoch);
    void send_status_query(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_status_response(const pubsub_itc_fw::ConnectionID& conn_id);
    void handle_peer_status_query(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_response(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);

    // Replay mode helpers.
    //
    // replay_buffer_ accumulates records during WAL replay in on_initial_event().
    // dispatch_replay_records() sends them to the ME once the ME connection is up.
    struct ReplayRecord {
        int64_t seq_no{};
        int16_t pdu_id{};
        int64_t wall_time_ns{};
        std::vector<uint8_t> payload;
    };

    std::vector<ReplayRecord> replay_buffer_;
    bool replay_me_order_ready_{false}; // outbound sequencer->ME order connection up
    bool replay_me_er_ready_{false};    // inbound ME->sequencer ER connection up
    void try_dispatch_replay();         // dispatches once both flags are set
    void dispatch_replay_records();

    // WalRecord doubles as the pipeline envelope (Option B): the WAL, the follower's copy of it and the
    // external-subscriber stream all carry the stamped WalRecord, so leader and follower WALs stay
    // byte-identical and every reader decodes envelope-then-payload. See docs/fix/pdu_generation.md.
    //
    // Writes a record through the replicated WAL, which numbers it, sends it to the follower and
    // commits it. The envelope must already carry the number the replicated WAL gives next.
    void append_envelope_to_wal(const pubsub_itc_fw_app::WalRecord& envelope);

    // ---- Sending an order to the matching engine only once the follower holds it -------------
    //
    // While a follower is connected and keeping up, the leader appends each order to its log,
    // sends the record to the follower, and holds the order until the follower acknowledges it;
    // only then is the order sent to the matching engine. So the engine never acts on an order the
    // follower does not hold, and a leader that dies leaves nothing on the book that the surviving
    // log lacks. See docs/availability/change_of_sequencer_leader.md, part 4.2.
    //
    // A held order keeps the inbound message's own buffer, which the decoded envelope points into,
    // and releases it once the order has been sent to the engine, so holding an order copies nothing
    // and allocates nothing.
    struct HeldOrder {
        pubsub_itc_fw_app::WalRecord envelope;
        pubsub_itc_fw::SlabHandle slab_id;
        // The inbound message's bytes the envelope's payload points into, released once it is sent; null for a command
        // this sequencer wrote itself, whose payload is held below instead.
        const uint8_t* buffer{nullptr};
        std::chrono::steady_clock::time_point held_at;
        // The payload of a command this sequencer wrote itself (write_own_command), which no inbound message holds.
        static constexpr size_t own_payload_capacity = 64;
        std::array<uint8_t, own_payload_capacity> own_payload{};
        size_t own_payload_size{0};
    };

    // The orders held, at most SequencerConfiguration::orders_held_at_most, allocated once when the sequencer
    // starts. An order is held until its record is committed and an engine leads to send it to
    // (docs/availability/voided_commands.md, section 5.1).
    pubsub_itc_fw::FixedCapacityRingBuffer<HeldOrder> held_orders_{config_.orders_held_at_most};

    // Orders numbered from here on were written to the log and replicated but not held, because the
    // storage above was full; zero when there are none. They are sent to the engine from the log
    // when they are released, so the storage bounds memory without bounding how many orders can
    // wait. While this is set, no later order is held either, so that orders are sent in order.
    int64_t first_unheld_seq_{0};

    // The connections gateways send orders on, and whether reading from them has been paused because
    // orders are waiting faster than they are released (docs/availability/a_follower_behind_does_not_lead.md, 4.3).
    std::vector<pubsub_itc_fw::ConnectionID> order_connection_ids_;
    bool order_reading_paused_{false};

    // Reading from the gateways is paused when three quarters of the storage for held orders is used
    // while waiting for a voter's confirmation, leaving room for orders already read, and resumed when
    // the held orders fall to a quarter. Orders that arrive with the storage full anyway wait in the log.
    [[nodiscard]] size_t pause_order_reading_at() const {
        return config_.orders_held_at_most * 3 / 4;
    }
    [[nodiscard]] size_t resume_order_reading_at() const {
        return config_.orders_held_at_most / 4;
    }

    // 1 while this sequencer leads and commits orders without waiting for its follower, for any reason.
    // Registered here and set by the replicated WAL.
    pubsub_itc_fw::GaugeHandle running_alone_gauge_;

    void hold_until_acknowledged(const pubsub_itc_fw_app::WalRecord& envelope, const pubsub_itc_fw::EventMessage& message);
    /// Has the reading thread send the leading engine, in order, the orders numbered from @p first that waited in the log
    /// rather than the storage.
    void send_orders_that_waited_in_the_log(int64_t first);
    /// The last record released to the engines: every order numbered at or below it has been sent to the leading engine,
    /// and every record at or below it is committed. An engine is sent nothing above it.
    [[nodiscard]] int64_t released_through() const;

    /**
     * @brief Whether an engine leads that orders can be sent to: the order connection goes to an engine that neither
     *        follows the WAL nor is being caught up.
     *
     * While none can, every order stays held, so that no engine applies an order before the engine that leads is sent
     * it (docs/availability/voided_commands.md, section 5.1).
     */
    [[nodiscard]] bool an_engine_takes_orders() const;
    /// Whether the storage for held orders is full of orders the follower sequencer has not acknowledged, so that it is
    /// too far behind; orders held because no engine leads are committed, and say nothing about the follower.
    [[nodiscard]] bool held_orders_wait_on_the_follower() const;
    /// Pauses reading from the gateways while too many orders wait, and resumes it when they have drained.
    void pause_or_resume_order_reading();
    void send_held_order_to_matching_engine(const HeldOrder& held);
    /// Sends the leading engine every held order whose record is committed through @p acknowledged_seq_no, and then sends
    /// the engines that follow the WAL every record up to the position released.
    void release_held_orders_through(int64_t acknowledged_seq_no);
    /// The first half of release_held_orders_through: sends the leading engine the held orders, and nothing to the engines
    /// that follow the WAL.
    void release_held_orders_to_the_leading_engine(int64_t acknowledged_seq_no);
    void discard_held_orders();

    // The reports from the matching engine kept while this instance is not leading, so that it can
    // forward them if it takes the lead (docs/availability/change_of_sequencer_leader.md, section 4.4).
    //
    // How much the store must hold: a follower grants its leader's request once every renewal
    // interval, and keeps the reports that arrived up to a lease period plus the drift allowance
    // before the last grant. When the leader dies, the store therefore holds up to a renewal
    // interval plus a lease period plus the drift allowance of reports, 4.25 seconds with the usual
    // timings. At the highest rate measured on this venue, about 34,000 orders a second, and taking
    // one and a half reports an order, that is about 212,000 reports. The store holds up to
    // 262,144, and 128 MiB of their bytes, which allows an average of 512 bytes a report. Both are
    // allocated once, when the sequencer starts.
    static constexpr size_t kept_report_capacity_bytes = 128U * 1024U * 1024U;
    static constexpr size_t kept_report_capacity_reports = 262144;
    KeptReportStore kept_reports_{kept_report_capacity_bytes, kept_report_capacity_reports};

    // When this instance last granted its peer's request to lead. The peer leads, and forwards the
    // reports it receives, for a lease period after each grant, so a kept report that arrived more
    // than a lease period plus the drift allowance before this time was forwarded and is no longer
    // needed. Reports age only against this time, not against the clock: once the leader stops
    // asking, because it has died, nothing more ages however long the change of leader takes.
    std::chrono::steady_clock::time_point last_granted_to_leader_{};

    // The number of kept reports lost when this instance last said so, so that a loss is reported
    // when it happens rather than for every report lost after it.
    int64_t kept_reports_lost_reported_{0};
    pubsub_itc_fw::GaugeHandle kept_reports_lost_gauge_;

    // How long before the last grant to the leader a kept report must have arrived to be no longer needed.
    [[nodiscard]] std::chrono::steady_clock::time_point kept_reports_needed_from() const;
    // Keeps a copy of a report from the matching engine, received while this instance is not leading.
    void keep_report_from_engine(const pubsub_itc_fw::EventMessage& message);
    // On taking the lead: forwards every kept report to its member's gateway, marked as a possible repeat, and empties the store.
    void forward_kept_reports();

    // From taking the lead until the leading matching engine says the highest order it has acted
    // on. Meanwhile new orders are written and replicated but wait, held or in the log, so that the
    // engine is sent the orders this log holds and it never received before any later one
    // (docs/availability/commands_during_a_change_of_leader.md, section 3.5).
    //
    // The engine is asked on a connection other than the one the previous leader used, so in
    // principle an order that leader sent could still be on its way when the engine answers. It is
    // not in practice: this instance leads only once the previous leader's lease has run out,
    // seconds after it last sent anything, and the engine has long since read what was sent.
    bool awaiting_engine_position_{false};
    // The highest record this instance held when it took the lead: the orders the engine may lack.
    int64_t held_at_takeover_{0};
    int64_t engine_position_request_id_{0};
    int engine_position_asks_{0};
    std::chrono::steady_clock::time_point engine_position_asked_at_{};
    // How long to wait for the engine's answer before asking again.
    static constexpr std::chrono::milliseconds engine_position_ask_interval{1000};
    // Which ask is logged as a Warning: the engine has then not answered for four seconds.
    static constexpr int engine_position_asks_before_warning = 5;

    // Asks the leading matching engine for the highest order it has acted on, or stops waiting if
    // no engine is connected.
    void ask_engine_for_position();
    void handle_engine_position(const pubsub_itc_fw::EventMessage& message);
    void stop_awaiting_engine_position(const char* reason);
    void release_orders_that_waited_for_the_engine();

    // As a follower: when the kept reports the leader has said it forwarded were last discarded.
    std::chrono::steady_clock::time_point kept_reports_discarded_at_{};
    static constexpr std::chrono::milliseconds kept_reports_discard_interval{1000};

    // The facts read from the WAL, which every instance keeps whether it leads or follows: whether trading is halted, the
    // voided commands, the trading date, and the highest command a report answers (section 6.10 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md).
    VenueStateFromTheWal venue_state_{get_logger()};
    // The trading date to write on a command sequenced at this wall time: the UTC date of the first record in the day's
    // WAL, as a follower is told it by its leader's commands, or as this instance reads it from its WAL once. The matching
    // engine makes its identifiers with keys derived from it (docs/availability/deterministic_matching_engine.md, section
    // 3.3).
    [[nodiscard]] int32_t trading_date_for(int64_t wall_time_ns);

    // Sends one matching engine connection every void whose record is committed, if there are any.
    void send_voided_commands(const pubsub_itc_fw::ConnectionID& engine);
    // Sends the voids to every engine connected, once the record of the latest is committed through @p seq_no. Called
    // before anything committed with it is released to an engine, so that every engine knows of a void before it is sent
    // a command the void names.
    void send_voids_committed_through(int64_t seq_no);

    /**
     * @brief Writes a command of this sequencer's own into the WAL and sends it to the matching engine once released, as
     * an order is: a decision that changes the book, which every engine is to apply at the same position
     * (docs/availability/deterministic_matching_engine.md, section 4). Only while leading.
     * @param[in] inner_pdu_id The command's message type, carried in the envelope as an order's is.
     * @param[in] bytes        The command, encoded; at most HeldOrder::own_payload_capacity bytes.
     * @param[in] size         How many bytes.
     */
    void write_own_command(int16_t inner_pdu_id, const uint8_t* bytes, size_t size);
    // Stamps the time if an engine is routed to, tells engine_absence_ whether one is, and halts trading if no engine
    // could be trading within the limit on automatic recovery. Called on engine_absence_timer_id_, and when this instance
    // takes the lead.
    void check_engine_absence();

    /**
     * @brief Halts trading because no engine could be trading within the longest recovery the venue does by itself
     *        (docs/availability/voided_commands.md, section 5.3, R-0117).
     *
     * Writes three commands, in this order: a VoidCommands naming every new order held, so that the engine that next
     * leads rejects each instead of applying it; a CancelBook that cancels every order on the book, each reported to its
     * member; and a TechnicalEvent that halts trading. They are held like the orders before them, and sent to the engine
     * that next leads, in order. Then tells the gateways the venue is not accepting orders, with the reason.
     */
    void write_recovery_halt(int64_t now_ns);
    // Writes the VoidCommands of write_recovery_halt, if any order or cancel is held; voids only commands no engine has
    // been sent and no report in the WAL answers.
    void void_held_orders_at_the_halt();
    // Encodes @p command and writes it with write_own_command. @return false, having logged why, if it cannot be encoded.
    template <typename Command> bool write_own_encoded_command(const Command& command, const char* name);
    // A matching engine about to lead that cannot account for the orders the venue holds, asking for trading to be halted.
    void handle_halt_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);

    /**
     * @brief An operator asking, over the listener for operators, for trading to be halted or the halt lifted
     *        (docs/availability/voided_commands.md, section 6.2).
     *
     * Only the leader acts. To halt, it writes a TechnicalEvent that halts trading, with the reason DeclaredByOperator,
     * and cancels nothing. To lift, it writes one that does not halt, with the reason LiftedByOperator, only if a matching
     * engine leads and the WAL holds a report answering the last member's command before the halt; otherwise it declines,
     * saying why. Every request is answered and logged at Warning with the operator's name.
     */
    void handle_operator_halt_request(const pubsub_itc_fw::ConnectionID& conn_id, const uint8_t* payload, size_t size);
    // Sends the operator's tool the answer, and logs it at Warning.
    void answer_operator(const pubsub_itc_fw::ConnectionID& conn_id, std::string_view operator_name, pubsub_itc_fw_app::OperatorHaltOutcome outcome,
                         int64_t technical_event_seq_no, const std::string& explanation);

    // A matching engine that died applying a command, and whose peer has not applied it, asking for it to be voided
    // (docs/availability/voided_commands.md, section 4.3). Only the leader acts: it writes a VoidCommands command naming
    // the command, if that is safe, answers with the voids if the command is voided already, and otherwise answers
    // VoidDeclined.
    void handle_void_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    // Answers a VoidRequest on @p conn_id with VoidDeclined, saying why at Error.
    void decline_void(const pubsub_itc_fw::ConnectionID& conn_id, int64_t instance_id, int64_t seq_no, pubsub_itc_fw_app::VoidDeclinedReason reason,
                      const char* why);
    // What the WAL holds at @p seq_no: a member's command, something else, or nothing (@p found false). Reads the WAL on
    // this thread, from the start of the segment holding the record; done only for a VoidRequest, which comes after an
    // engine has died.
    [[nodiscard]] CommandInRecord command_in_wal_at(int64_t seq_no, bool& found);

    // The matching engine instance that last announced that it leads, and the connection it announced it on, while that
    // connection is up; 0 and none otherwise. EngineOrderRouting remembers the last leader after its process dies, so
    // that orders go to it again when it reconnects; this does not, because a void must not be refused on the strength
    // of an engine that has died. Cleared when that instance announces that it follows, or the connection is lost.
    int64_t leading_engine_instance_{0};
    pubsub_itc_fw::ConnectionID leading_engine_announced_on_{};

    // Whether a command has been sent again or refused since the last summary of checks, and when the last was. The
    // summary is logged a minute after the last, or when this instance stops leading.
    bool commands_checked_since_summary_{false};
    std::chrono::steady_clock::time_point last_command_checked_at_{};
    static constexpr std::chrono::seconds command_check_summary_idle_limit{60};
    // Since the last summary: commands sent again that the WAL already held, commands sent again that it did not, and
    // commands refused because they reused a ClOrdID.
    int64_t sent_again_already_logged_{0};
    int64_t sent_again_sequenced_{0};
    int64_t commands_refused_reused_identifier_{0};
    // Since the last summary: commands sent again that were answered by a report the WAL held.
    int64_t commands_answered_from_wal_{0};
    // Since this instance started: commands sequenced unchecked because they had no comp id or no ClOrdID.
    int64_t commands_unchecked_without_identity_{0};
    // Where the identity of a command to be checked is written.
    std::array<char, pubsub_itc_fw::CommandIdentityInterface::max_identity_size> command_identity_buffer_{};

    // The record in the WAL that holds a command with the same comp id, protocol and ClOrdID as this one, if any. Exact:
    // the replicated WAL proves it (ReplicatedWal::find_command). @p sent_again says whether the gateway marked the
    // command as sent again, which bounds how far back a search of the WAL need read. A command that does not say
    // whose it is cannot be checked, and is counted and treated as not held.
    [[nodiscard]] std::optional<pubsub_itc_fw::CommandInWal> find_logged_command(const pubsub_itc_fw_app::WalRecordView& inbound, bool sent_again);
    // Refuses a command not marked as sent again whose ClOrdID the WAL holds as record @p first_seq_no: sends its
    // gateway CommandRefused, and writes nothing (docs/availability/one_check_against_sequencing_twice.md, section 4.1).
    // A report in the WAL: the record that holds it, and the encoded report.
    struct ReportInWal {
        int64_t report_seq_no{0};
        std::vector<uint8_t> payload;
    };
    // How far past a command the WAL is read for the report answering it. An order is normally answered within
    // about 100 microseconds, a few records later; a command not answered within this many records is waiting for
    // its engine, which answers it by the ordinary path.
    static constexpr int64_t reports_read_for_an_answer_at_most = 4096;
    // The first report in the WAL answering the command @p command, read forward from it through the committed
    // records, at most reports_read_for_an_answer_at_most of them. Reading starts at the command's own record when
    // its position is known, and otherwise at the start of the segment that holds it.
    [[nodiscard]] std::optional<ReportInWal> first_report_answering(const pubsub_itc_fw::CommandInWal& command);
    // Answers a command sent again that the WAL holds as @p command with the first report of it the WAL holds,
    // forwarded to the member's session marked as a possible repeat; when the WAL holds none, the matching engine
    // answers it (docs/availability/one_check_against_sequencing_twice.md, section 3.3).
    void answer_from_wal(const pubsub_itc_fw_app::WalRecordView& inbound, const pubsub_itc_fw::CommandInWal& command);
    // It is sent on @p came_by, the connection the command arrived on.
    void refuse_reused_identifier(const pubsub_itc_fw::ConnectionID& came_by, const pubsub_itc_fw_app::WalRecordView& inbound, int64_t first_seq_no);
    // Logs what the checks since the last summary found, and gives back the memory of any search of the WAL.
    void summarise_command_checks(const char* reason);

    // Writes on @p envelope how far this leader has forwarded every report from the engine, for a follower to read
    // (docs/bug_list.md, BUG-0116).
    void stamp_reports_forwarded_through(pubsub_itc_fw_app::WalRecord& envelope) const;

    // What a follower reads back from its leader's records. The leader's "forwarded" means handed to
    // its connection to the gateway, not received there, and a report still in the leader's send
    // buffers when its machine or network fails is lost. So a follower discards only the kept reports
    // covered by the position on records written at least reports_forwarded_delay before the latest
    // record it holds, both times by the leader's clock.
    struct ForwardedSample {
        int64_t written_ns{0};
        EngineReportPosition through{};
    };
    static constexpr std::chrono::milliseconds reports_forwarded_delay{100};
    static constexpr size_t forwarded_sample_capacity = 4096;
    std::vector<ForwardedSample> forwarded_samples_ = std::vector<ForwardedSample>(forwarded_sample_capacity);
    size_t forwarded_samples_head_{0};
    size_t forwarded_samples_count_{0};
    int64_t latest_record_written_ns_{0};
    int64_t kept_reports_discarded_as_forwarded_{0};
    // Notes the position a record read from the log carries, and when the leader wrote it.
    void note_forwarded_sample(int64_t written_ns, const pubsub_itc_fw_app::WalRecordView& view);
    // Discards the kept reports the leader had forwarded at least reports_forwarded_delay before the latest record.
    void discard_kept_reports_the_leader_forwarded();

    // Where a report about to be forwarded came from.
    enum class ReportSource {
        // The matching engine has just sent it to this instance, which is leading.
        matching_engine,
        // It was kept while this instance was not leading, and the leader then may or may not have forwarded it.
        kept_while_not_leading
    };
    // Gives a report from the matching engine its own record in the log, replicates and streams it,
    // and forwards it to its member's gateway once the record it depends on is acknowledged.
    // @p bytes is the report's WalRecord envelope as the engine sent it, and @p er_seq_no the
    // sequence number sent with it. The caller keeps @p bytes valid until this returns.
    void forward_report_from_engine(const uint8_t* bytes, size_t size, int64_t er_seq_no, ReportSource source);

    // A matching engine publisher asking to follow the WAL (WalFollowRequest): declined unless this instance
    // leads, otherwise caught up from the position it gives, up to the last record committed, and then sent
    // each record as it is committed.
    void handle_publisher_follow_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    // Sends every publisher that has been handed over each record committed through @p seq_no.
    void send_committed_records_to_publishers(int64_t seq_no);
    // Sends each matching engine that follows the WAL, once handed over, every record up to the position released, so that
    // it applies no order before the leading engine is sent it (docs/availability/voided_commands.md, section 5.1).
    void send_released_records_to_follower_engines();
    // Whether @p connection is a matching engine publisher's.
    [[nodiscard]] bool is_publisher_connection(const pubsub_itc_fw::ConnectionID& connection) const {
        return connection.service_name() == wal_subscriber_inbound_svc_;
    }

    // ME failover reconciliation (Slice D).
    //

    // Primary is always instance 1 and secondary always 2; the ids are fixed for the life of
    // a deployment and the arbiter's cold-start preference relies on it. Which of them LEADS
    // moves, and is what the announcement reports.
    static constexpr int64_t me_primary_instance_id = 1;
    static constexpr int64_t me_secondary_instance_id = 2;

    /// Handles a matching engine stating which role it holds and under which epoch.
    void handle_role_announcement(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    // A matching engine asking to be caught up (WalFollowRequest): declined unless this instance leads,
    // otherwise served from the position it gives, up to the last order released.
    void handle_wal_follow_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    /// Streams one catch-up record to a matching engine, and says whether it sent anything.
    ///
    /// It withholds execution report envelopes, which are outputs rather than inputs, so a record
    /// walked past is not a record sent. The engine is told how many were sent and checks it got
    /// them all (R-0101), so the two counts have to mean the same thing.
    [[nodiscard]] bool stream_wal_record_to_me(const pubsub_itc_fw::ConnectionID& conn_id, int64_t record_id, int16_t pdu_id, const uint8_t* pdu_payload,
                                               size_t pdu_size, int64_t wall_time_ns);

    // How MemberSessions counts and tells the test.
    class SessionLinks : public MemberSessionsLinksInterface {
      public:
        explicit SessionLinks(SequencerThread& owner) : owner_(owner) {}
        [[nodiscard]] bool expects_no_session_for(std::string_view comp_id) const override;
        void reports_dropped_unbound(int64_t total) override;
        void report_dropped(const fix_common::SessionIdentity& identity, int64_t er_seq_no) override;
        void session_announced_again(const fix_common::SessionIdentity& identity, const fix_common::SessionDestination& destination) override;
        void session_unbound(const fix_common::SessionIdentity& identity, int16_t instance, int32_t connection) override;

      private:
        SequencerThread& owner_;
    };
    SessionLinks session_links_{*this};
    // Which gateway connection each member's session is bound to, what the venue remembers about each session between
    // connections, and which session placed each order still open (section 6.12 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md).
    MemberSessions sessions_{session_links_, get_logger()};
    // The count of reports dropped because their session was connected nowhere, which MemberSessions keeps.
    pubsub_itc_fw::GaugeHandle reports_dropped_unbound_gauge_;

    /**
     * @brief How often the venue says out loud that no matching engine leads while it holds orders.
     *
     * Once per order would bury the condition in the Quill log rather than report it (BUG-0009 and
     * docs/availability/order_acceptance.md).
     */
    static constexpr auto no_engine_warning_interval = std::chrono::seconds{5};

    /// While this instance leads and no matching engine takes orders (an_engine_takes_orders), when that began; empty
    /// while one does. A steady clock, because this measures an interval: a change to the wall clock must not change how
    /// long the venue believes it has had no engine.
    std::optional<std::chrono::steady_clock::time_point> no_engine_since_;
    /// The orders and cancels that arrived while no matching engine took orders, all of them held.
    int64_t orders_held_without_an_engine_{0};
    /// Whether an engine has taken orders since this instance began leading: false while the venue starts, so that the
    /// time without one is told apart from a change of engine leader (EngineTakesOrdersAgain).
    bool an_engine_took_orders_{false};
    std::chrono::steady_clock::time_point last_no_engine_warning_{};

    /**
     * @brief Notes whether a matching engine takes orders, and so when none has since, and says when one does again.
     *
     * Called wherever that can change: on each order held, when an engine connection is lost, on an engine's
     * announcement of its role, when an engine is handed over after a catch-up, and on the engine absence timer, about
     * once a second. Each time no engine has led for orders_held_at_most_seconds, the gateways are told to refuse new
     * orders (refresh_order_acceptance).
     */
    void note_whether_an_engine_takes_orders();

    /// The order connection, or whether the engine on it is known to lead, may have changed: notes whether an engine takes
    /// orders, and releases to it the orders held while none did.
    void engine_routing_changed();

    /// How long no matching engine has taken orders, zero while one does.
    [[nodiscard]] std::chrono::steady_clock::duration time_without_an_engine() const;

    /**
     * @brief What the venue is currently telling its gateways about taking new orders: Accepting, or why not.
     *
     * Starts Accepting: a sequencer that has just come up and has not yet failed to forward
     * anything is accepting. Moved only by refresh_order_acceptance(), and reset when this
     * instance stops leading.
     */
    pubsub_itc_fw_app::OrderAcceptanceReason order_acceptance_{pubsub_itc_fw_app::OrderAcceptanceReason::Accepting};
    // What order_acceptance_ should be now, from how long no matching engine has led and whether trading is halted.
    [[nodiscard]] pubsub_itc_fw_app::OrderAcceptanceReason order_acceptance_now() const;

    /**
     * @brief Recomputes acceptance from how long no matching engine has led, and from whether trading is halted
     *        (order_acceptance_now).
     *
     * @return true if the state changed, in which case every connected gateway has been told.
     */
    bool refresh_order_acceptance();

    /// Sends the current acceptance state to one gateway connection.
    void send_order_acceptance(const pubsub_itc_fw::ConnectionID& conn_id);

    /// Sends the current acceptance state to every connected gateway.
    void broadcast_order_acceptance();

    void handle_session_bound(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_session_unbound(const pubsub_itc_fw::EventMessage& message);
    void handle_session_replay_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    // Sends a member, from the log, the execution reports produced for it while its session had no
    // connection (R-0005, docs/bug_list.md BUG-0088).
    void handle_undelivered_reports_request(const pubsub_itc_fw::EventMessage& message);
    void handle_session_sequence_update(const pubsub_itc_fw::EventMessage& message);

    /// Records a single replay may return before it truncates, when the caller names no cap.
    static constexpr int32_t default_replay_max_records = 10000;
};

} // namespaces
