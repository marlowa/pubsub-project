#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

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

#include "EngineCatchUpFromWal.hpp"
#include "EngineOrderRouting.hpp"
#include "EngineRecordFromWal.hpp"
#include "EngineReportPosition.hpp"
#include "GatewayIds.hpp"
#include "KeptReportStore.hpp"
#include "LogTailIndex.hpp"
#include "LoggedCommandIdentifiers.hpp"
#include "LoggedCommandIdentifiersBuilder.hpp"
#include "SeqNumRanges.hpp"
#include "SequencerConfiguration.hpp"
#include "SessionIdentity.hpp"
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

    // Outbound gateway connections for ER forwarding, keyed by (protocol, instance).
    //
    // More than one gateway feeds the same book -- the ASCII FIX one and the binary one --
    // and each may run as several instances, so neither axis identifies a process on its
    // own. Protocol says which wire format the report is encoded in; instance says which
    // process to send it to. See fix_common/GatewayIds.hpp and docs/availability/gateway_ha.md.
    //
    // Packed into one integer key rather than a std::pair so the map needs no custom hash.
    using GatewayKey = int32_t;

    /** @brief Packs (protocol, instance) into the key gateway_conn_ids_ is indexed by. */
    static constexpr GatewayKey gateway_key(int16_t protocol, int16_t instance) {
        return (static_cast<GatewayKey>(protocol) << 16) | static_cast<GatewayKey>(static_cast<uint16_t>(instance));
    }

    std::unordered_map<GatewayKey, pubsub_itc_fw::ConnectionID> gateway_conn_ids_;

    // (protocol, instance) pairs already reported as absent from the configuration.
    // Keeps the deployment-error message to once per pair rather than once per report.
    std::unordered_set<GatewayKey> unknown_gateways_warned_;

    /** @brief The connection for a gateway instance, or nullptr when it is not connected. */
    const pubsub_itc_fw::ConnectionID* gateway_connection(int16_t protocol, int16_t instance) const {
        auto it = gateway_conn_ids_.find(gateway_key(protocol, instance));
        if (it == gateway_conn_ids_.end() || !it->second.is_valid()) {
            return nullptr;
        }
        return &it->second;
    }

    // Which of this sequencer's order connections to the matching engines carries orders: the one to
    // the instance that leads, as the engines' announcements and requests to catch up establish. It
    // keeps the order connection to each instance while that connection is open, so a request to
    // catch up from either engine is always recognised (BUG-0108). See EngineOrderRouting.hpp.
    EngineOrderRouting engine_routing_;

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
    // Until a publisher is handed over, the reading thread sends it what it lacks; with high availability
    // off there is no reading thread, and this catches it up as well, on this thread.
    class SequencerPublisherLinks : public pubsub_itc_fw::WalHandedOverFollowersLinksInterface {
      public:
        explicit SequencerPublisherLinks(SequencerThread& owner) : owner_(owner) {}
        void send_record(const pubsub_itc_fw::ConnectionID& follower, int16_t pdu_id, int64_t seq_no, const uint8_t* bytes, size_t size) override;

      private:
        SequencerThread& owner_;
    };
    SequencerPublisherLinks publisher_links_{*this};
    std::optional<pubsub_itc_fw::WalHandedOverFollowers> handed_over_publishers_;

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

      private:
        SequencerThread& owner_;
    };
    SequencerReplicatedWalEvents replicated_wal_events_{*this};

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
    };
    std::unordered_map<int, EngineServing> engine_servings_;
    // The component's background cores, from the CPU layout, for the helper threads this thread starts;
    // empty when no layout was read.
    std::vector<pubsub_itc_fw::CpuId> background_cores_;
    // Has the reading thread serve @p engine from the record after @p after_seq_no, up to the last order released.
    void serve_engine(const pubsub_itc_fw::ConnectionID& engine, int64_t after_seq_no, EngineServing serving);
    // The engine the reading thread was serving has been handed over at @p through.
    void engine_handed_over(const pubsub_itc_fw::ConnectionID& engine, int64_t through, int64_t records_sent, EngineServing serving);
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

    // WAL replication state (Slice 7).
    //
    // An ExecutionReport from the ME is held here until the follower has
    // confirmed it wrote the corresponding WAL entry.  The gateway only sees
    // the ER once the follower has durably committed it.
    struct PendingEr {
        int16_t pdu_id{};
        int64_t seq_no{};
        std::vector<uint8_t> payload; // copy of the raw encoded ER from ME
        // The ClOrdID of the command the report answers, copied onto the envelope for the gateway.
        std::string cl_ord_id;
        // The report's own sequence number in the log, carried on the envelope as a report sent at
        // once carries it, so that a gateway knows how far a member has been served.
        int64_t log_seq_no{0};
        // Where the report stands in the engine's reports. While it waits here, the leader has not
        // forwarded every report up to it, which is what reports_forwarded_through() says.
        std::optional<EngineReportPosition> position;
        // Whose report this is. Deliberately the identity and not a resolved destination:
        // this record exists precisely because delivery is being deferred until the
        // follower acks, and a session can reconnect during that wait -- which is the case
        // a gateway failover produces. Resolving the address when the ER was buffered would
        // send it to whichever connection was current a moment before the failover.
        fix_common::SessionIdentity identity;
        // The originating gateway's ingress stamp, carried through the WalAck wait so the
        // gateway still gets it when the ER is released. Buffering here is the live HA
        // path, so dropping it would leave the round-trip metric empty in exactly the
        // configuration the venue actually runs.
        bool has_gateway_ingress_ns{false};
        int64_t gateway_ingress_ns{0};
        // Whether this is the report that acknowledges a new order, which is the only kind
        // the order-path checkpoints record. Carried through the wait for the same reason
        // the ingress stamp above is: the buffered path is the live HA path, so a value
        // dropped here is a value missing in the configuration the venue actually runs --
        // and a checkpoint missing on one leg of the path is worse than no checkpoint at
        // all, because the difference against its neighbour reads as a stage taking no time.
        bool is_new_order_ack{false};
        // Whether this report repeats one the member may already hold. Carried through the
        // wait for the same reason the ingress stamp is: the buffered path is the live HA
        // path, so a mark dropped here is a mark the member never sees in the configuration
        // the venue actually runs. See R-0122.
        bool poss_resend{false};
        bool erase_routing_entry{false};
    };

    // Reports waiting for the follower to acknowledge the record they depend on, keyed by that
    // record's sequence number and ordered by it, so that an acknowledgement releases every report
    // it covers from the front. More than one report may wait on one record.
    std::multimap<int64_t, PendingEr> pending_er_;

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
        const uint8_t* buffer{nullptr};
        std::chrono::steady_clock::time_point held_at;
    };

    // The orders held while waiting for the follower, at most SequencerConfiguration::orders_held_at_most,
    // allocated once when the sequencer starts.
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

    // 1 while the leader carries on without waiting for a follower that is connected but too far
    // behind. Registered here and set by the replicated WAL.
    pubsub_itc_fw::GaugeHandle running_alone_gauge_;

    void hold_until_acknowledged(const pubsub_itc_fw_app::WalRecord& envelope, const pubsub_itc_fw::EventMessage& message);
    /// Has the engine sent, in order, the orders numbered from first to through that waited in the log rather than the storage:
    /// by the reading thread when there is one and an engine to send to, otherwise by send_logged_orders.
    void send_orders_that_waited_in_the_log(int64_t first, int64_t through);
    /// Reads the WAL on this thread and sends the engine the orders numbered from first to through, or counts them as deferred
    /// when no engine is connected. Used only without a reading thread, or without an engine.
    void send_logged_orders(int64_t first, int64_t through);
    /// The last order released to the engine: everything numbered at or below it has been sent or deferred, and nothing above it.
    [[nodiscard]] int64_t released_through() const;
    /// Pauses reading from the gateways while too many orders wait, and resumes it when they have drained.
    void pause_or_resume_order_reading();
    void send_held_order_to_matching_engine(const HeldOrder& held);
    void release_held_orders_through(int64_t acknowledged_seq_no);
    void release_all_held_orders();
    void discard_held_orders();
    /// Forwards every waiting report whose record is committed through @p seq_no.
    void forward_pending_er_through(int64_t seq_no);

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

    // The identifiers of the commands this instance's log holds, so that a command a gateway sends
    // again after a change of leader is not sequenced twice (LoggedCommandIdentifiers, and
    // docs/availability/commands_during_a_change_of_leader.md, section 3.4). Built in the background
    // when HA is on, by identifiers_builder_, and touched only by this thread once taken from it. Each
    // command is noted as it is written, whether this instance leads or follows.
    std::optional<LoggedCommandIdentifiers> logged_identifiers_;
    bool identifiers_full_reported_{false};
    // While the record is being built: the identifiers of the commands noted meanwhile, added to the
    // record when it is taken; and every command that must be checked is checked against the log itself
    // (docs/bug_list.md, BUG-0121).
    bool identifiers_building_{false};
    std::vector<uint64_t> identifiers_noted_while_building_;
    LoggedCommandIdentifiersBuilder identifiers_builder_;
    // As a follower: when the kept reports the leader has said it forwarded were last discarded.
    std::chrono::steady_clock::time_point kept_reports_discarded_at_{};
    static constexpr std::chrono::milliseconds kept_reports_discard_interval{1000};

    // Starts building the record of identifiers in the background from the segments of the log as it
    // stands once opened.
    void start_building_record_of_identifiers();
    // Takes the record from the builder if it has finished, and adds what was noted meanwhile.
    void take_record_of_identifiers_if_built();
    void note_logged_command(const pubsub_itc_fw_app::WalRecordView& view);
    void note_logged_command(std::string_view comp_id, int16_t protocol, int16_t inner_pdu_id, std::string_view cl_ord_id);
    // Notes an identifier: added to the record, or kept until the record is built.
    void note_logged_identifier(uint64_t id);
    // Adds an identifier to the record, reporting once if it is full.
    void record_identifier(uint64_t id);

    // An exact index of the end of the log, built when the first command is sent again after a
    // change of leader and discarded a minute after the last, or when this instance stops leading.
    std::optional<LogTailIndex> log_tail_index_;
    std::chrono::steady_clock::time_point log_tail_index_used_at_{};
    static constexpr std::chrono::seconds log_tail_index_idle_limit{60};
    int64_t sent_again_already_logged_{0};
    int64_t sent_again_sequenced_{0};

    // Whether this log already holds a command a gateway has sent again. Exact: the record of
    // identifiers rules a command out, and the index of the end of the log confirms one it does not.
    [[nodiscard]] bool command_already_logged(const pubsub_itc_fw_app::WalRecordView& inbound);
    void discard_log_tail_index(const char* reason);

    /**
     * @brief The connections carrying commands from each gateway instance.
     *
     * A gateway opens its own connection to each sequencer to send commands on. When that connection's
     * queue of waiting sends fills, because this sequencer stopped reading from it, the gateway closes
     * it and opens another, and sends again, marked as sent again, every command still unanswered.
     * Commands the gateway had already written to the old connection may still be unread in this
     * machine's buffers for it, and may be read after their copies on the new connection. Those
     * originals are not marked, so they would not be checked against the log, and a command could be
     * sequenced twice (docs/bug_list.md, BUG-0118).
     *
     * So while a gateway instance has more than one connection open here, every command from it is
     * checked against the log, marked or not, until the older connection has been read to its end.
     * The gateway instance is learned from the first command on each connection, which says which
     * gateway sent it.
     */
    struct CommandConnections {
        int open{0};
        int64_t already_logged{0};
    };
    std::unordered_map<int, GatewayKey> command_connection_gateway_;
    std::unordered_map<GatewayKey, CommandConnections> command_connections_;

    // Records which gateway instance a connection carries commands from, and returns whether that
    // instance has more than one such connection open, in which case every command from it is checked.
    [[nodiscard]] bool note_command_connection(const pubsub_itc_fw::ConnectionID& id, const pubsub_itc_fw_app::WalRecordView& inbound);
    // Forgets a connection that carried commands, once it has been read to its end.
    void forget_command_connection(const pubsub_itc_fw::ConnectionID& id);

    // The latest report from the engine this instance has handled while leading: forwarded, waiting
    // for an acknowledgement, or dropped because its session was not connected.
    std::optional<EngineReportPosition> latest_report_seen_;
    // The position up to which this leader has forwarded every report from the engine: the latest
    // seen, or just before the oldest still waiting for an acknowledgement. Written on every record
    // this leader writes, for a follower to read (docs/bug_list.md, BUG-0116).
    [[nodiscard]] std::optional<EngineReportPosition> reports_forwarded_through() const;
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

    void forward_pending_er(const PendingEr& pending);

    /**
     * @brief Sends an envelope-wrapped ER to the gateway the order originated from.
     * @param[in] protocol Which client protocol (see fix_common/GatewayIds.hpp).
     * @param[in] instance Which instance of that protocol, numbered from 1.
     * @param[in] er_seq_no  Sequence number stamped on the transport header.
     * @param[in] envelope   The WalRecord-wrapped ER.
     *
     * Drops the ER with a warning when that gateway instance is not currently connected;
     * the client behind it has no route, and no other instance can serve its session
     * until session provisioning and report replay land (steps 4-6 of gateway_ha.md).
     */
    void send_er_to_origin_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope, bool is_new_order_ack);

    // A matching engine publisher asking to follow the WAL (WalFollowRequest): declined unless this instance
    // leads, otherwise caught up from the position it gives, up to the last record committed, and then sent
    // each record as it is committed.
    void handle_publisher_follow_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    // Sends every publisher that has been handed over each record committed through @p seq_no.
    void send_committed_records_to_publishers(int64_t seq_no);
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

    // Which client session an order came from. The identity, not the address: where its
    // reports go is looked up separately, at the moment of sending, so that a session which
    // has reconnected somewhere else in the meantime is still reachable.
    struct OriginSession {
        fix_common::SessionIdentity identity;

        // Wall-clock nanoseconds at which the gateway read the order off the client socket.
        // Carried here purely so it can be stamped back onto the ER envelope: the gateway
        // measures its own round trip, and nothing in the sequencer reads this value.
        //
        // It rides with the routing data rather than in a map of its own because it has
        // exactly the same lifetime and the same key -- a second map would be a second
        // thing to insert into, erase from and rebuild on replay, with no way to notice
        // when the two drifted apart.
        //
        // has_ingress_ns false means the order arrived without one -- a WAL replay, or a
        // gateway that does not stamp. It is a separate flag rather than a zero sentinel
        // so an absent stamp cannot be mistaken for an epoch-zero one.
        bool has_ingress_ns{false};
        int64_t gateway_ingress_ns{0};
    };

    // seq_no -> the session that placed the order.
    // Keyed by the sequence number assigned to each NOS/OCR (globally unique, unlike
    // ClOrdID which is only unique per client session). Populated on each sequenced
    // NOS/OCR; rebuilt from WAL replay on startup.
    //
    // This used to hold the originating *connection* and its reports were addressed
    // straight at it. That made a report undeliverable the moment the socket closed, and
    // undeliverable to the right member even after it came back, because the connection id
    // it named was gateway-local and renumbered on reconnect. Holding the identity and
    // resolving the address at send time is what lets a reconnect -- to the same instance
    // or to the member's backup -- inherit reports for orders it placed on the old one.
    std::unordered_map<int64_t, OriginSession> seq_no_to_session_;

    // session identity -> where that session's reports go right now.
    //
    // Maintained from the SessionBound and SessionUnbound PDUs the gateways send as
    // sessions come and go, which is the only way the sequencer can know: it listens on one
    // port and accepts, so it cannot tell instances apart from a connection alone, and a
    // member that reconnects and sends no order would otherwise never announce itself.
    //
    // An absent entry means the session is not connected anywhere. Its reports have nowhere
    // to go and are dropped, exactly as they were before -- making them replayable instead
    // is step 6, and it is deliberately not smuggled in here.
    std::unordered_map<fix_common::SessionIdentity, fix_common::SessionDestination, fix_common::SessionIdentityHash> session_destinations_;

    // Every session that has had a report dropped, because it was connected nowhere, since it was
    // last connected. The first report dropped for a session is logged as a Warning and the rest at
    // Debug, so that a member left untold shows once in the Quill log rather than once per report.
    // A session leaves this set when it is connected again.
    std::unordered_set<fix_common::SessionIdentity, fix_common::SessionIdentityHash> sessions_with_reports_dropped_;
    // How many reports have been dropped because their session was connected nowhere.
    int64_t reports_dropped_unbound_{0};
    pubsub_itc_fw::GaugeHandle reports_dropped_unbound_gauge_;
    // Counts and logs a report dropped because its session, @p identity, is connected nowhere.
    void note_report_dropped_unbound(const fix_common::SessionIdentity& identity, int64_t er_seq_no);

    /**
     * @brief What the venue remembers about a session between connections.
     *
     * Sequence numbers belong to the session, not to the connection carrying it, so a
     * gateway that has just taken a session on has no way to know where it had reached.
     * The sequencer does, because every instance of every protocol binds through it -- and
     * this is the state that makes a reconnect a continuation rather than a reset.
     *
     * The numbers are *reported* by the gateway at unbind rather than counted here. They
     * have to be: the FIX outbound number counts every message sent to the member,
     * including the heartbeats and session-level rejects the sequencer never sees. So this
     * is as current as the last clean unbind, and a killed gateway leaves it behind --
     * which the member's own ResendRequest is what resolves.
     *
     * Kept separately from session_destinations_ because the lifetimes differ: a
     * destination is erased the moment a session disconnects, whereas this must outlive
     * exactly that event to be of any use.
     */
    struct SessionSequenceState {
        /// Highest position the gateway has reported. Never lowered.
        int32_t outbound_seq_num{1};

        /// Execution reports forwarded to this session since that report arrived.
        ///
        /// Each one consumed an outbound sequence number at the gateway, so this is how much
        /// the reported figure is known to be behind. Exact, because the sequencer resolves a
        /// destination for every report it sends -- and reports are the bulk of what a member
        /// is sent. What it cannot see is the admin traffic (heartbeats, rejects), which is
        /// what the allowance below covers.
        int32_t ers_since_report{0};

        /**
         * @brief Highest position the gateway has reported for what the member SENDS it.
         *
         * The counterpart of `outbound_seq_num`, and **resumed differently from it**, which is
         * the trap of having the two side by side.
         *
         * `outbound_seq_num` is handed back deliberately high after an unclean death: too low
         * sends the member a number below what it expects, which FIX requires it to treat as
         * fatal. Every term of that reverses here. Resuming this one too high makes the venue
         * treat a member that has done nothing wrong as having gone backwards, which is equally
         * fatal and this time to the innocent party. So it is handed back exactly as reported,
         * with no allowance and nothing added for what the sequencer has seen since -- the
         * reported figure is already a lower bound, because a member can only have sent more.
         *
         * Nothing reads it yet; see docs/fix/inbound_sequence_checking.md and BUG-0038.
         */
        int32_t inbound_seq_num{1};

        /**
         * @brief Which of this session's outbound numbers held an execution report.
         *
         * Reported by the gateway, for the same reason the number above it is: the sequencer
         * cannot derive it. The FIX numbering covers every message the member was sent, and
         * the heartbeats, Logons and rejects among them never come near the sequencer.
         *
         * Held here rather than in the gateway because a resend is served by whichever
         * instance holds the session *now*, which after a failover is not the instance that
         * sent the messages being asked about. A record kept in the gateway is empty in
         * exactly the case it exists for. See docs/availability/resend_provenance.md.
         */
        std::vector<fix_common::SeqNumRange> report_seq_nums;

        /**
         * @brief The log sequence number of the last execution report the member was delivered,
         * as its gateway last reported it: exactly, when the session unbound, or as of the last
         * sequence update, when the gateway died without unbinding it. Never lowered.
         */
        int64_t last_report_delivered{0};

        /**
         * @brief The last record in the log when the session bound again: reports logged after it
         * were forwarded live, so only those up to it can have been missed. Zero until the session
         * binds again after an unbind, and zero again once the missed reports have been sent.
         */
        int64_t bound_at_log_seq_no{0};

        /// Whether the member's previous gateway died without unbinding it, so last_report_delivered
        /// may be behind and the reports sent from the log must be marked as possible repeats.
        bool delivered_figure_may_be_behind{false};
    };

    /// Added when resuming a session whose gateway died without reporting.
    ///
    /// Covers the admin messages the sequencer never sees. Small, because a report is only
    /// seconds old, and heartbeats are the main thing it misses. Erring high is deliberate:
    /// resuming ABOVE the true position leaves a gap the member closes with a ResendRequest,
    /// whereas resuming below sends it a sequence number lower than it expects, which FIX
    /// requires it to treat as fatal. The two errors are not symmetrical.
    static constexpr int32_t unclean_resume_admin_allowance = 64;
    std::unordered_map<fix_common::SessionIdentity, SessionSequenceState, fix_common::SessionIdentityHash> session_sequence_state_;

    /**
     * @brief How often the venue says out loud that it is deferring orders.
     *
     * It used to say so once per order, at Info, which produced 1,087,912 lines in one incident
     * and hid the condition rather than reporting it. See BUG-0009 and
     * docs/availability/order_acceptance.md.
     */
    static constexpr auto order_deferral_warning_interval = std::chrono::seconds{5};

    /// Orders deferred since the matching engine was last reachable, and when that began.
    ///
    /// A steady clock because this measures an interval rather than naming a moment: a wall clock
    /// adjustment mid-outage would otherwise change how long the venue believes it has been
    /// degraded. The same reasoning as the slab allocator's drain tripwire.
    int64_t deferred_order_count_{0};
    std::chrono::steady_clock::time_point deferral_began_{};
    std::chrono::steady_clock::time_point last_deferral_warning_{};
    bool deferring_orders_{false};

    /// Records one order deferred because no matching engine is reachable, and says so at a rate
    /// a reader can follow.
    void note_order_deferred(int64_t seq_no);

    /// Reports what the outage cost, once the venue can forward again.
    void note_matching_engine_reachable();

    /**
     * @brief How long the venue defers orders before it stops accepting new ones.
     *
     * Age is the honest measure, because what is at stake is a member's exposure and exposure
     * is measured in time. The threshold has to clear a NORMAL failover, or the venue would
     * refuse orders during a routine recovery that members currently survive: the
     * matching-engine pair uses a 15-second peer heartbeat timeout, and promotion,
     * reconnection and WAL reconciliation follow it. Three times that timeout is comfortably
     * above a normal failover rather than level with it, and still short enough that a member
     * is wrong about its own position for under a minute.
     */
    static constexpr auto order_deferral_refusal_age = std::chrono::seconds{45};

    /**
     * @brief How many orders may be deferred before the venue stops accepting, whatever the age.
     *
     * A backstop for the case age alone handles badly: a burst defers a great many orders in the
     * seconds *before* the age threshold trips. Age bounds the outage in time; without this,
     * nothing bounds it in volume. At the peak rate measured on this venue, ~34,500 orders/s, the
     * age threshold alone would allow about 1.55 million orders to be accepted and not processed
     * before it spoke. This caps that at roughly a sixth.
     *
     * At the measured trading-day rate of ~1,926 orders/s it takes over two minutes to reach, so
     * at ordinary rates the age always speaks first, which is the intent. At peak it is reached
     * in about seven seconds, inside a normal failover, and that is deliberate rather than an
     * oversight: a burst is exactly when the volume the venue is taking on runs away from it.
     *
     * **This counts orders, not members, and the harm is per member.** A venue has a few hundred
     * to a few thousand comp ids, not a few hundred thousand; each posts continuously and one
     * member holds several. So this figure is a proxy -- what it really bounds is how much of any
     * one member's position can be wrong, and the venue-wide total stands in for that because
     * deferrals are counted globally rather than per session. A per-session count would model the
     * harm directly and is the better answer if this proxy ever proves too blunt.
     */
    static constexpr int64_t order_deferral_refusal_count = 250000;

    /**
     * @brief Whether the venue is currently telling its gateways it can take new orders.
     *
     * Starts true: a sequencer that has just come up and has not yet failed to forward
     * anything is accepting. Moved only by refresh_order_acceptance().
     */
    bool accepting_orders_{true};

    /**
     * @brief Recomputes acceptance from the age and size of the current deferral.
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

    /// Records that one execution report was sent to this session, so a resume after an
    /// unclean death can account for what the gateway sent since its last report.
    void note_report_forwarded(const fix_common::SessionIdentity& identity);

    /// Records a single replay may return before it truncates, when the caller names no cap.
    static constexpr int32_t default_replay_max_records = 10000;

    // Resolves where a session's reports go, or nullptr when it is not bound anywhere.
    const fix_common::SessionDestination* session_destination(const fix_common::SessionIdentity& identity) const;
};

} // namespaces
