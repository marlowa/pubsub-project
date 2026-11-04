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

#include "CommandChecks.hpp"
#include "EngineOrderRouting.hpp"
#include "EnginePresence.hpp"
#include "EngineRecordFromWal.hpp"
#include "EngineReportPosition.hpp"
#include "GatewayIds.hpp"
#include "HaltAndVoidRequests.hpp"
#include "KeptReports.hpp"
#include "MemberSessions.hpp"
#include "OrderRelease.hpp"
#include "ReportForwarding.hpp"
#include "ReportsHeldForUnknownSessions.hpp"
#include "SeqNumRanges.hpp"
#include "SequencerCommandIdentity.hpp"
#include "SequencerConfiguration.hpp"
#include "ServingWalFollowers.hpp"
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

    // How often the absence of a matching engine is checked (EnginePresence::check_absence).
    static constexpr std::chrono::seconds engine_absence_check_interval{1};

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

    // How a record is sent to a matching engine publisher, or to a matching engine that follows the WAL, once it has been
    // caught up and handed over (ServingWalFollowers).
    class SequencerPublisherLinks : public pubsub_itc_fw::WalHandedOverFollowersLinksInterface {
      public:
        explicit SequencerPublisherLinks(SequencerThread& owner) : owner_(owner) {}
        void send_record(const pubsub_itc_fw::ConnectionID& follower, int16_t pdu_id, int64_t seq_no, const uint8_t* bytes, size_t size) override;

      private:
        SequencerThread& owner_;
    };
    SequencerPublisherLinks publisher_links_{*this};

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
    // Constructed at the initial event.
    std::optional<pubsub_itc_fw::ReplicatedWal> replicated_wal_;

    // The component's background cores, from the CPU layout, for the helper threads this thread starts;
    // empty when no layout was read.
    std::vector<pubsub_itc_fw::CpuId> background_cores_;
    // Publishes the last order released (released_through) as the limit a matching engine is served up to.
    void publish_released_position();

    // The number of a record in the WAL the reading thread could not read, which stops a follower being caught up past it,
    // while it stands; 0 when there is none (ServingWalFollowers).
    pubsub_itc_fw::GaugeHandle damaged_record_gauge_;
    pubsub_itc_fw::WalFollowingPositions& wal_positions_;

    class ServingLinks : public ServingWalFollowersLinksInterface {
      public:
        explicit ServingLinks(SequencerThread& owner) : owner_(owner) {}
        void send_to_reading_thread(std::unique_ptr<pubsub_itc_fw::WalReadingCommand> command) override;
        void send_declined(const pubsub_itc_fw::ConnectionID& to, const pubsub_itc_fw::WalFollowDeclined& declined) override;
        void send_caught_up(const pubsub_itc_fw::ConnectionID& to, const pubsub_itc_fw::WalFollowCaughtUp& caught_up) override;
        void send_leader_changed(const pubsub_itc_fw::ConnectionID& to, const pubsub_itc_fw::WalLeaderChanged& changed) override;
        [[nodiscard]] pubsub_itc_fw::ConnectionID peer_connection() const override;
        [[nodiscard]] bool is_publisher_connection(const pubsub_itc_fw::ConnectionID& connection) const override;
        [[nodiscard]] int64_t first_seq_no_held() const override;
        void publish_released_position() override;
        void pause_for_hand_over() override;
        void end_pause_for_hand_over() override;
        void peer_caught_up(int64_t through) override;
        void peer_catch_up_stopped() override;
        void engine_catch_up_progress(const pubsub_itc_fw::ConnectionID& connection, int64_t sent_through) override;
        void engine_catch_up_ended(const pubsub_itc_fw::ConnectionID& connection) override;
        void engine_handed_over(const pubsub_itc_fw::ConnectionID& connection, int64_t through, int64_t records_sent, bool answers_catch_up_request) override;
        void engine_caught_up_to_lead(const pubsub_itc_fw::ConnectionID& connection) override;
        void set_damaged_record_gauge(int64_t seq_no) override;

      private:
        SequencerThread& owner_;
    };
    ServingLinks serving_links_{*this};
    // What this instance does for those that follow its WAL: the follower sequencer's catch-up, the publishers, and the
    // matching engines being caught up or following the WAL (section 6.15 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md).
    ServingWalFollowers serving_{ServingWalFollowers::Converters{peer_converter, engine_converter, publisher_converter}, serving_links_, get_logger()};

    // The epoch the sequencer stamps on what it sends: the replicated WAL's, or 1 before it is made.
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

    // WalRecord doubles as the pipeline envelope (Option B): the WAL, the follower's copy of it and the
    // external-subscriber stream all carry the stamped WalRecord, so leader and follower WALs stay
    // byte-identical and every reader decodes envelope-then-payload. See docs/fix/pdu_generation.md.
    //
    // Writes a record through the replicated WAL, which numbers it, sends it to the follower and
    // commits it. The envelope must already carry the number the replicated WAL gives next.
    void append_envelope_to_wal(const pubsub_itc_fw_app::WalRecord& envelope);

    // How OrderRelease reads the WAL's positions, sends orders, and pauses reading from the gateways.
    class OrderLinks : public OrderReleaseLinksInterface {
      public:
        explicit OrderLinks(SequencerThread& owner) : owner_(owner) {}
        [[nodiscard]] int64_t committed_through() const override;
        [[nodiscard]] int64_t last_seq_no() const override;
        [[nodiscard]] bool waits_for_confirmation() const override;
        [[nodiscard]] bool waits_for_follower() const override;
        [[nodiscard]] bool leading() const override;
        [[nodiscard]] bool an_engine_takes_orders() const override;
        [[nodiscard]] pubsub_itc_fw::ConnectionID leading_engine_connection() const override;
        void send_to_leading_engine(const HeldOrder& held) override;
        void send_orders_that_waited_in_the_wal(int64_t first) override;
        void release_through(int64_t committed_through) override;
        void send_engine_orders_after(int64_t after) override;
        void release_message(pubsub_itc_fw::SlabHandle slab_id, const uint8_t* buffer) override;
        void pause_reading(const pubsub_itc_fw::ConnectionID& connection) override;
        void resume_reading(const pubsub_itc_fw::ConnectionID& connection) override;
        void released_position_moved(int64_t released_through) override;
        void ask_engine_position(const pubsub_itc_fw::ConnectionID& connection, int64_t request_id) override;
        void held_orders_full(int64_t first_seq_no) override;

      private:
        SequencerThread& owner_;
    };
    OrderLinks order_links_{*this};
    // Holding each order until it may be sent to the leading matching engine, and sending it; reading from the gateways,
    // paused while too many orders wait or while a follower is handed over; and the wait after taking the lead for the
    // engine's position (section 6.8 of docs/venue/dividing_the_sequencer_and_matching_engine.md). The storage for the
    // orders held, SequencerConfiguration::orders_held_at_most of them, is allocated once.
    OrderRelease orders_{config_.orders_held_at_most, order_links_, get_logger()};

    // 1 while this sequencer leads and commits orders without waiting for its follower, for any reason.
    // Registered here and set by the replicated WAL.
    pubsub_itc_fw::GaugeHandle running_alone_gauge_;

    /**
     * @brief Whether an engine leads that orders can be sent to: the order connection goes to an engine that neither
     *        follows the WAL nor is being caught up.
     *
     * While none can, every order stays held, so that no engine applies an order before the engine that leads is sent
     * it (docs/availability/voided_commands.md, section 5.1).
     */
    [[nodiscard]] bool an_engine_takes_orders() const;
    /// Sends @p held to the leading engine, unless the reading thread is sending the engine what it lacks.
    void send_held_order_to_matching_engine(const HeldOrder& held);
    /// Sends the leading engine every held order whose record is committed through @p acknowledged_seq_no, and then sends
    /// the engines that follow the WAL every record up to the position released.
    void release_held_orders_through(int64_t acknowledged_seq_no);

    // How KeptReports forwards a kept report and tells the test.
    class KeptReportLinks : public KeptReportsLinksInterface {
      public:
        explicit KeptReportLinks(SequencerThread& owner) : owner_(owner) {}
        void forward_kept_report(int64_t er_seq_no, const uint8_t* bytes, size_t size) override;
        void kept_reports_lost(int64_t total) override;
        void kept_reports_forwarded(int64_t discarded_as_forwarded, int64_t count, int64_t bytes) override;

      private:
        SequencerThread& owner_;
    };
    KeptReportLinks kept_report_links_{*this};
    // The reports from the matching engine kept while this instance does not lead, so that it can forward them if it
    // takes the lead, and the discarding of those no longer needed (section 6.16 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md).
    KeptReports kept_reports_{config_.lease.period, config_.lease.drift_allowance, kept_report_links_, get_logger()};
    // The count of kept reports lost because the store was full, which KeptReports keeps.
    pubsub_itc_fw::GaugeHandle kept_reports_lost_gauge_;
    // Keeps a copy of a report from the matching engine, received while this instance is not leading.
    void keep_report_from_engine(const pubsub_itc_fw::EventMessage& message);

    // The leading matching engine's answer to a new leader's question of the highest order it has acted on
    // (EnginePosition): given to OrderRelease.
    void handle_engine_position(const pubsub_itc_fw::EventMessage& message);
    // The orders held while no engine took them, or while the engine's position was awaited, go now if their records are
    // committed.
    void release_orders_that_waited_for_the_engine();

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
    // The absence check of EnginePresence: called on engine_absence_timer_id_, and when this instance takes the lead.
    void check_engine_absence();

    // Halts trading because no engine could be trading within the longest recovery the venue does by itself
    // (HaltAndVoidRequests::recovery_halt), then tells the gateways the venue is not accepting orders, with the reason.
    // Called by EnginePresence, which decides when.
    void write_recovery_halt(int64_t now_ns);
    // Encodes @p command and writes it with write_own_command. @return false, having logged why, if it cannot be encoded.
    template <typename Command> bool write_own_encoded_command(const Command& command, const char* name);
    // A matching engine about to lead that cannot account for the orders the venue holds, asking for trading to be halted:
    // decoded, and handed to HaltAndVoidRequests.
    void handle_halt_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);

    // An operator asking, over the listener for operators, for trading to be halted or the halt lifted
    // (docs/availability/voided_commands.md, section 6.2): decoded, and handed to HaltAndVoidRequests.
    void handle_operator_halt_request(const pubsub_itc_fw::ConnectionID& conn_id, const uint8_t* payload, size_t size);
    // A matching engine that died applying a command, and whose peer has not applied it, asking for it to be voided
    // (docs/availability/voided_commands.md, section 4.3): decoded, and handed to HaltAndVoidRequests.
    void handle_void_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    // What the WAL holds at @p seq_no: a member's command, something else, or nothing (@p found false). Reads the WAL on
    // this thread, from the start of the segment holding the record; done only for a VoidRequest, which comes after an
    // engine has died.
    [[nodiscard]] CommandInRecord command_in_wal_at(int64_t seq_no, bool& found);

    class HaltLinks : public HaltAndVoidRequestsLinksInterface {
      public:
        explicit HaltLinks(SequencerThread& owner) : owner_(owner) {}
        [[nodiscard]] bool leading() const override;
        [[nodiscard]] bool orders_routed_to(const pubsub_itc_fw::ConnectionID& connection) const override;
        [[nodiscard]] bool an_engine_takes_orders() const override;
        [[nodiscard]] bool another_engine_leads(int64_t instance_id) const override;
        [[nodiscard]] const TradingHaltState& trading_halt() const override;
        [[nodiscard]] int64_t highest_command_answered() const override;
        [[nodiscard]] int64_t released_through() const override;
        [[nodiscard]] int64_t last_seq_no() const override;
        [[nodiscard]] int64_t next_seq_no() const override;
        [[nodiscard]] int32_t trading_day(int64_t now_ns) override;
        [[nodiscard]] bool voided_already(int64_t seq_no) const override;
        [[nodiscard]] CommandInRecord command_in_wal_at(int64_t seq_no, bool& found) override;
        bool write_technical_event(const pubsub_itc_fw_app::TechnicalEvent& event) override;
        bool write_void_commands(const pubsub_itc_fw_app::VoidCommands& command) override;
        bool write_cancel_book(const pubsub_itc_fw_app::CancelBook& command) override;
        void send_voids(const pubsub_itc_fw::ConnectionID& connection) override;
        void decline_void(const pubsub_itc_fw::ConnectionID& connection, int64_t seq_no, pubsub_itc_fw_app::VoidDeclinedReason reason) override;
        void answer_operator(const pubsub_itc_fw::ConnectionID& connection, pubsub_itc_fw_app::OperatorHaltOutcome outcome, int64_t technical_event_seq_no,
                             const std::string& explanation) override;

      private:
        SequencerThread& owner_;
    };
    HaltLinks halt_links_{*this};
    // The requests to halt trading or void commands, and the halt written when the venue cannot recover by itself
    // (section 6.14 of docs/venue/dividing_the_sequencer_and_matching_engine.md).
    HaltAndVoidRequests halts_{halt_links_, get_logger()};

    class CommandLinks : public CommandChecksLinksInterface {
      public:
        explicit CommandLinks(SequencerThread& owner) : owner_(owner) {}
        [[nodiscard]] std::optional<pubsub_itc_fw::CommandInWal> find_command(std::string_view identity, int64_t first_sent_no_earlier_than) override;
        bool answer_from_wal(const pubsub_itc_fw_app::WalRecordView& inbound, const pubsub_itc_fw::CommandInWal& command) override;
        void refuse(const pubsub_itc_fw::ConnectionID& came_by, const pubsub_itc_fw_app::WalRecordView& inbound, int64_t first_seq_no) override;
        int64_t discard_command_search() override;
        [[nodiscard]] pubsub_itc_fw::ReplicatedWal::CommandCheckCounts command_check_counts() const override;

      private:
        SequencerThread& owner_;
    };
    CommandLinks command_links_{*this};
    // The check of each command against those already in the WAL, and what follows from a match (section 6.13 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md).
    CommandChecks command_checks_{command_links_, get_logger()};

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
    // forwarded to the member's session marked as a possible repeat; when the WAL holds none, or the session is bound to
    // no gateway, the matching engine answers it (docs/availability/one_check_against_sequencing_twice.md, section 3.3).
    // @return Whether a report was forwarded.
    bool answer_from_wal(const pubsub_itc_fw_app::WalRecordView& inbound, const pubsub_itc_fw::CommandInWal& command);

    // Writes on @p envelope how far this leader has forwarded every report from the engine, for a follower to read
    // (docs/bug_list.md, BUG-0116).
    void stamp_reports_forwarded_through(pubsub_itc_fw_app::WalRecord& envelope) const;

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
     * @brief Notes whether a matching engine takes orders, and so when none has since, and says when one does again.
     *
     * Called wherever that can change: on each order held, when an engine connection is lost, on an engine's
     * announcement of its role, when an engine is handed over after a catch-up, and on the engine absence timer, about
     * once a second (EnginePresence::note_whether_an_engine_takes_orders).
     */
    void note_whether_an_engine_takes_orders();

    /// The order connection, or whether the engine on it is known to lead, may have changed: notes whether an engine takes
    /// orders, and releases to it the orders held while none did.
    void engine_routing_changed();

    class PresenceLinks : public EnginePresenceLinksInterface {
      public:
        explicit PresenceLinks(SequencerThread& owner) : owner_(owner) {}
        [[nodiscard]] bool leading() const override;
        [[nodiscard]] bool an_engine_takes_orders() const override;
        [[nodiscard]] bool an_engine_is_routed_to() const override;
        [[nodiscard]] bool wal_holds_records() const override;
        [[nodiscard]] bool trading_halted() const override;
        [[nodiscard]] bool halted_for_a_recovery_too_long() const override;
        void tell_every_gateway() override;
        void engine_takes_orders_again(int64_t without_ms, int64_t orders_held, bool one_had_before) override;
        void write_recovery_halt(int64_t now_ns) override;

      private:
        SequencerThread& owner_;
    };
    PresenceLinks presence_links_{*this};
    // Whether a matching engine takes orders and since when none has, what the gateways are told about taking orders, the
    // limit on the recovery the venue does by itself, and the engine that said it leads (section 6.9 of
    // docs/venue/dividing_the_sequencer_and_matching_engine.md).
    EnginePresence presence_{std::chrono::seconds{config_.orders_held_at_most_seconds}, std::chrono::seconds{config_.automatic_recovery_limit_seconds},
                             presence_links_, get_logger()};

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
