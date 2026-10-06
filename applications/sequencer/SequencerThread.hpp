#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <map>
#include <mutex>
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

#include <pubsub_itc_fw/ExternalWalSubscriberRegistry.hpp>
#include <pubsub_itc_fw/Wal.hpp>

#include "BackgroundPromiseRecorder.hpp"
#include "EngineOrderRouting.hpp"
#include "EngineReportPosition.hpp"
#include "EpochStore.hpp"
#include "GatewayIds.hpp"
#include "KeptReportStore.hpp"
#include "LeaseLinksInterface.hpp"
#include "LeasePromiseStore.hpp"
#include "LogEpochTable.hpp"
#include "LogTailIndex.hpp"
#include "LoggedCommandIdentifiers.hpp"
#include "LoggedCommandIdentifiersBuilder.hpp"
#include "PairLeaseAgent.hpp"
#include "ReplicatedRecordWriter.hpp"
#include "SeqNumRanges.hpp"
#include "SequencerConfiguration.hpp"
#include "SessionIdentity.hpp"

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
     */
    SequencerThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                    const SequencerConfiguration& config);

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

    // The number the next record this instance writes as leader will carry. Orders and the
    // execution reports the leader logs are numbered from it alike. Set from the log's last
    // record at startup, and moved past every record replicated to this instance when it takes
    // the lead (adopt_role), because a follower writes its leader's records under the leader's
    // numbers and does not use this counter for them. BUG-0105.
    int64_t next_sequence_number_{1};

    // The highest sequence number of any record this instance has written as a follower, from its
    // leader's replication stream. Atomic because two threads write it: the inline handler on the
    // reactor's thread (install_peer_wal_inline_handler) and handle_wal_record on this thread, which
    // takes the records the inline handler passes on. Read on this thread when the instance takes
    // the lead, so that it numbers new records above every record its log holds.
    std::atomic<int64_t> highest_replicated_seq_no_{0};

    // Repairing a follower's log when it rejoins: docs/availability/follower_log_repair.md.
    //
    // Which leadership wrote each record of this instance's log. Guarded by its mutex, because a
    // follower's records are written on the reactor's thread by the inline handler as well as on
    // this one.
    LogEpochTable log_epochs_;
    mutable std::mutex log_epochs_mutex_;

    // As a follower: whether this log and the leader's are known to agree, so that the records the
    // leader sends may be written. Until then they are discarded. Set and cleared on this thread,
    // read by the inline handler.
    std::atomic<bool> follower_log_agreed_{false};

    // As a follower: replicated records the inline handler passed to this thread that this thread
    // has not yet written or discarded. The inline handler writes a record itself only while this is
    // zero, so that records are written in order and by one thread at a time.
    std::atomic<int64_t> replicated_records_queued_{0};

    // As a leader: whether the follower's log is known to agree with this one, so that live records
    // are sent to it. Until then it is sent none: written after a gap, they would be out of place.
    bool follower_log_matches_{false};

    // As a follower: when this instance last asked its leader where their logs agree.
    std::chrono::steady_clock::time_point last_position_request_at_{};

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

    // mmap'd on-disk write-ahead log. Opened in on_initial_event()
    // before the sequencer begins accepting connections.
    /// Commits one record to the log and records how long it took. Every append goes through
    /// here, so there is one place that knows what a commit costs.
    void append_to_wal(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, int size, int64_t wall_time_ns, int32_t leader_epoch);

    /// Opens the log, builds the table of epochs, and discards everything after the first gap, if there is one.
    void open_wal_trusting_it_up_to_any_gap();

    pubsub_itc_fw::Wal wal_;

    // How long committing one record to the log takes. On the order path and on the reactor
    // thread, which is why it is worth measuring: a commit that blocks is a sequencer that has
    // stopped sequencing, and until this existed the only sign of it was the reactor's stall
    // watchdog saying a callback had not finished -- a log line, correlated by hand.
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

    // External WAL subscriber registry and active connection set.
    // The registry tracks each subscriber's cursor for WAL truncation.
    // wal_subscriber_conn_ids_ is the set of connections that have completed
    // the WalSubscribeRequest handshake and are receiving live WalRecord PDUs.
    pubsub_itc_fw::ExternalWalSubscriberRegistry external_wal_subscriber_registry_;
    std::unordered_set<pubsub_itc_fw::ConnectionID> wal_subscriber_conn_ids_;

    // Leader-follower state machine (slice 6).
    pubsub_itc_fw_app::Role role_{pubsub_itc_fw_app::Role::unknown};

    // The leadership generation. Never assign to this directly: go through
    // set_epoch(), which also writes it to disk. A restart that forgets the
    // epoch lets this node claim a generation the venue has already used.
    int32_t epoch_{0};

    // Where the epoch outlives the process. Read once at startup, rewritten
    // whenever the epoch moves.
    fix_common::EpochStore epoch_store_;

    // Where this instance's promises in deciding which instance leads outlive the process, until
    // the machine reboots. It lets a process restarted by its supervisor keep the lead it held.
    fix_common::LeasePromiseStore lease_promise_store_;
    // Writes promise records on a thread of its own, so that refreshing one does not stop this
    // instance answering lease requests while the disk is written (BUG-0107). Declared after the
    // store it writes to, so it is destroyed first. Constructed only when there is a lease agent.
    std::optional<fix_common::BackgroundPromiseRecorder> background_promise_recorder_;

    // How the lease rules reach the other two voters in deciding which sequencer leads: the peer
    // sequencer, and the arbiter pool on both arbiter connections.
    class SequencerLeaseLinks : public fix_common::LeaseLinksInterface {
      public:
        explicit SequencerLeaseLinks(SequencerThread& owner) : owner_(owner) {}
        void send_request_to_peer(const pubsub_itc_fw_app::LeaseRequest& request) override;
        void send_request_to_third_voter(const pubsub_itc_fw_app::LeaseRequest& request) override;
        void send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseGrant& grant) override;
        void send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseRefusal& refusal) override;

      private:
        SequencerThread& owner_;
    };
    SequencerLeaseLinks lease_links_{*this};

    // Decides whether this sequencer leads: it does only while a majority of three voters -- itself,
    // its peer and the arbiter pool -- has granted it a lease that has not run out. Constructed at
    // the initial event when high availability is on. See fix_common/PairLeaseAgent.hpp.
    std::optional<fix_common::PairLeaseAgent> lease_agent_;

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

    // The highest sequence number the follower has acknowledged. Only ever advances. The follower
    // acknowledges records in the order it receives them, so everything at or below this is on two
    // machines, and a report whose record is at or below it can be forwarded at once. That is why no
    // record of individual acknowledgements is kept (BUG-0111). It is the first of the positions the log's retention will
    // be anchored to -- see the snapshot timer for why nothing is reclaimed yet.
    int64_t peer_acked_through_{0};

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
    void adopt_role(pubsub_itc_fw_app::Role new_role);
    void set_epoch(int32_t new_epoch);
    /// Changes this sequencer's role to follow what the lease rules have just decided.
    void act_on(fix_common::PairLeaseAgent::Change change);
    void send_status_query(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_status_response(const pubsub_itc_fw::ConnectionID& conn_id);
    void handle_peer_status_query(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_response(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_lease_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_lease_grant(const pubsub_itc_fw::EventMessage& message);
    void handle_lease_refusal(const pubsub_itc_fw::EventMessage& message);

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

    // WAL storage / replication helpers (peer follower).
    //
    // WalRecord doubles as the pipeline envelope (Option B): the WAL, the follower
    // replication stream and the external-subscriber stream all carry the stamped
    // WalRecord, so leader and follower WALs stay byte-identical and every reader
    // decodes envelope-then-payload. See docs/fix/pdu_generation.md.
    [[nodiscard]] bool needs_wal_ack() const;
    void append_envelope_to_wal(const pubsub_itc_fw_app::WalRecord& envelope);
    void send_wal_record(const pubsub_itc_fw_app::WalRecord& envelope);
    void handle_wal_record(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_wal_ack(const pubsub_itc_fw::EventMessage& message);
    void install_peer_wal_inline_handler(const pubsub_itc_fw::ConnectionID& conn_id);

    /// As a follower: asks the leader where their logs agree, unless they already do.
    void send_log_position_request();
    /// As a leader: answers a follower, and once the logs agree sends it every record after its last.
    void handle_log_position_request(const pubsub_itc_fw::EventMessage& message);
    /// As a follower: discards what the leader's answer says it does not hold, and asks again or starts writing.
    void handle_log_position_reply(const pubsub_itc_fw::EventMessage& message);
    /// The two logs no longer known to agree, because a peer connection or this instance's role changed.
    void forget_log_agreement();
    /// As a follower: whether a replicated record is the next one this log needs. A record already held is not; one that would leave
    /// a gap is not either, and the logs are then no longer taken to agree. Called on either thread that writes replicated records.
    // Writes a record the leader sent if it is the next one this log needs, from whichever thread
    // delivers it; on finding one missing, stops writing until the logs are found to agree again.
    ReplicatedRecordWriter::Outcome write_replicated_record(int64_t seq_no, const uint8_t* payload, int size, int64_t wall_time_ns, int32_t leader_epoch);
    // Every write of a replicated record, and every change to the log while they may arrive, goes
    // through this, under one lock (docs/bug_list.md, BUG-0123).
    ReplicatedRecordWriter replicated_record_writer_;

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

    // How many orders the leader holds at most while waiting for the follower. When it is full the
    // follower is treated as too far behind and the leader runs as if alone. At the highest rate
    // measured on this venue, about 34,000 orders a second, this is about half a second of orders,
    // well beyond the acknowledgement timeout below, so in practice the timeout is what notices a
    // follower that has stopped, and this only bounds the storage. Each slot holds the envelope and a
    // pointer, so the storage is a few megabytes, allocated once when the sequencer starts.
    static constexpr size_t held_order_capacity = 16384;

    // How long the leader waits, while it holds orders, for any acknowledgement from the follower
    // before it treats the follower as too far behind. A follower on the same network acknowledges
    // in tens of microseconds, so a tenth of a second without one means it has stalled, and members
    // are not kept waiting longer than that for a fault in the backup.
    static constexpr std::chrono::milliseconds follower_acknowledgement_timeout{100};

    // How often the timeout above is checked. A tenth of the timeout, so a stalled follower is
    // noticed between 100 and 110 milliseconds after its last acknowledgement.
    static constexpr std::chrono::milliseconds acknowledgement_watch_interval{10};

    pubsub_itc_fw::FixedCapacityRingBuffer<HeldOrder> held_orders_{held_order_capacity};

    // Orders numbered from here on were written to the log and replicated but not held, because the
    // storage above was full; zero when there are none. They are sent to the engine from the log
    // when they are released, so the storage bounds memory without bounding how many orders can
    // wait. While this is set, no later order is held either, so that orders are sent in order.
    int64_t first_unheld_seq_{0};

    // The connections gateways send orders on, and whether reading from them has been paused because
    // orders are waiting faster than they are released (docs/availability/a_follower_behind_does_not_lead.md, 4.3).
    std::vector<pubsub_itc_fw::ConnectionID> order_connection_ids_;
    bool order_reading_paused_{false};

    // Reading from the gateways is paused when this many orders are held while waiting for a voter's
    // confirmation, leaving room for orders already read, and resumed when the held orders fall to
    // the lower mark. Orders that arrive with the storage full anyway wait in the log.
    static constexpr size_t pause_order_reading_at = held_order_capacity * 3 / 4;
    static constexpr size_t resume_order_reading_at = held_order_capacity / 4;

    // True while a follower is connected but has fallen too far behind, and the leader is sending
    // orders to the engine at once and forwarding reports without waiting, as it does with no
    // follower connected. A loss of resilience, not of service.
    bool running_alone_{false};
    std::chrono::steady_clock::time_point running_alone_since_{};
    std::chrono::steady_clock::time_point last_acknowledgement_at_{};
    pubsub_itc_fw::GaugeHandle running_alone_gauge_;

    void hold_until_acknowledged(const pubsub_itc_fw_app::WalRecord& envelope, const pubsub_itc_fw::EventMessage& message);
    /// Whether the leader may send the engine an order its follower does not hold: HA is off, or a voter has confirmed that the follower
    /// may not lead.
    [[nodiscard]] bool may_act_without_follower() const;
    /// Says, if it does not already, that the follower may not lead, before acting on orders the follower lacks.
    void say_follower_may_not_lead();
    /// Says that the follower may lead again, once it holds every record and the leader waits for its acknowledgements again.
    void say_follower_may_lead_if_it_holds_everything();
    /// Sends the engine every waiting order and every waiting report, once acting without the follower is confirmed.
    void release_if_confirmed();
    /// Sends the engine, in order, the orders numbered from first to through that waited in the log rather than the storage.
    void send_logged_orders(int64_t first, int64_t through);
    /// The last order released to the engine: everything numbered at or below it has been sent or deferred, and nothing above it.
    [[nodiscard]] int64_t released_through() const;
    /// Pauses reading from the gateways while too many orders wait, and resumes it when they have drained.
    void pause_or_resume_order_reading();
    void send_held_order_to_matching_engine(const HeldOrder& held);
    void release_held_orders_through(int64_t acknowledged_seq_no);
    void release_all_held_orders();
    void discard_held_orders();
    void start_running_alone(const char* reason);
    void stop_running_alone(const char* reason);
    void check_follower_acknowledgements();
    void forward_all_pending_er();

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
    // when HA is on, by identifiers_builder_, and touched only by this thread once taken from it. While
    // leading, each command is noted as it is written; while following, the records the reactor thread
    // wrote are read from the log once a second, and the rest on taking the lead.
    std::optional<LoggedCommandIdentifiers> logged_identifiers_;
    bool identifiers_full_reported_{false};
    // While the record is being built: the identifiers of the commands noted meanwhile, added to the
    // record when it is taken; and every command that must be checked is checked against the log itself
    // (docs/bug_list.md, BUG-0121).
    bool identifiers_building_{false};
    std::vector<uint64_t> identifiers_noted_while_building_;
    LoggedCommandIdentifiersBuilder identifiers_builder_;
    // Where the next read of the log for identifiers starts: just after the last record read.
    pubsub_itc_fw::WalPosition identifiers_read_position_{};
    std::chrono::steady_clock::time_point identifiers_read_at_{};
    static constexpr std::chrono::milliseconds identifiers_read_interval{1000};

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
    void read_identifiers_from_log();

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

    // Raise highest_replicated_seq_no_ to seq_no if it is lower. Called from both threads that
    // write replicated records, so it never lowers the value whichever runs last.
    void note_replicated_record(int64_t seq_no);
    void flush_pending_er();
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

    // External WAL subscriber helpers (MEP primary and secondary).
    void handle_wal_subscribe_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_external_wal_ack(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void stream_wal_record_to_external_subscribers(const pubsub_itc_fw_app::WalRecord& envelope);

    // ME failover reconciliation (Slice D).
    //
    // When a promoted ME-secondary sends MePositionRequest on its order connection,
    // the sequencer walks the WAL from the ME's last-applied seq_no to the WAL head,
    // streaming each NOS/OCR to that connection, then sends MePositionAck. After the
    // ack the ME is live and new orders flow to it via engine_routing_.active().
    //
    // me_catchup_conn_id_ tracks the connection currently in catch-up so the handler
    // can stream to a specific ConnectionID rather than the buffered replay path.
    pubsub_itc_fw::ConnectionID me_catchup_conn_id_;

    // Primary is always instance 1 and secondary always 2; the ids are fixed for the life of
    // a deployment and the arbiter's cold-start preference relies on it. Which of them LEADS
    // moves, and is what the announcement reports.
    static constexpr int64_t me_primary_instance_id = 1;
    static constexpr int64_t me_secondary_instance_id = 2;

    /// Handles a matching engine stating which role it holds and under which epoch.
    void handle_role_announcement(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_me_position_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
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
