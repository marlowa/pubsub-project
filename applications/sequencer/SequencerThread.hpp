#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <fix_equity_orders.hpp>
#include <leader_follower.hpp>

#include <pubsub_itc_fw/ExternalWalSubscriberRegistry.hpp>
#include <pubsub_itc_fw/Wal.hpp>

#include "SequencerConfiguration.hpp"

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
    void on_connection_lost(const pubsub_itc_fw::ConnectionID &id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const SequencerConfiguration& config_;

    // Precomputed inbound service name strings derived from config ports.
    // Used in on_framework_pdu_message to classify inbound PDUs without
    // constructing strings on every call.
    const std::string order_inbound_svc_;
    const std::string er_inbound_svc_;
    const std::string wal_subscriber_inbound_svc_;

    // Monotonically increasing sequence number. Incremented for every PDU
    // forwarded to the matching engine. Never resets within a process lifetime.
    int64_t next_sequence_number_{1};

    // ConnectionID of the outbound gateway connection for ER forwarding.
    pubsub_itc_fw::ConnectionID gateway_conn_id_;

    // ConnectionID of the outbound matching-engine order connection.
    // The sequencer connects outbound to the ME's order listener on
    // matching_engine_port and forwards sequenced order PDUs over this
    // connection. Set when the outbound connection is established.
    pubsub_itc_fw::ConnectionID me_outbound_order_conn_id_;

    // ConnectionIDs of the outbound peer and arbiter connections.
    pubsub_itc_fw::ConnectionID peer_conn_id_;
    pubsub_itc_fw::ConnectionID peer_inbound_conn_id_; // inbound: peer connected to us
    pubsub_itc_fw::ConnectionID arbiter_primary_conn_id_;
    pubsub_itc_fw::ConnectionID arbiter_secondary_conn_id_;

    // instance_id of the peer sequencer, learned from StatusQuery/StatusResponse.
    // Used to populate ArbitrationReport.peer_instance_id.
    int64_t peer_instance_id_{0};

    // mmap'd on-disk write-ahead log. Opened in on_initial_event()
    // before the sequencer begins accepting connections.
    pubsub_itc_fw::Wal wal_;

    // External WAL subscriber registry and active connection set.
    // The registry tracks each subscriber's cursor for WAL truncation.
    // wal_subscriber_conn_ids_ is the set of connections that have completed
    // the WalSubscribeRequest handshake and are receiving live WalRecord PDUs.
    pubsub_itc_fw::ExternalWalSubscriberRegistry external_wal_subscriber_registry_;
    std::unordered_set<pubsub_itc_fw::ConnectionID> wal_subscriber_conn_ids_;

    // Leader-follower state machine (slice 6).
    pubsub_itc_fw_app::Role role_{pubsub_itc_fw_app::Role::unknown};
    int32_t epoch_{0};

    // WAL replication state (Slice 7).
    //
    // An ExecutionReport from the ME is held here until the follower has
    // confirmed it wrote the corresponding WAL entry.  The gateway only sees
    // the ER once the follower has durably committed it.
    struct PendingEr {
        int16_t pdu_id{};
        int64_t seq_no{};
        std::vector<uint8_t> payload;           // copy of the raw encoded ER from ME
        bool has_gateway_session_conn_id{false};
        int32_t gateway_session_conn_id{0};
        bool erase_routing_entry{false};
    };

    std::unordered_map<int64_t, PendingEr> pending_er_;      // seq_no -> buffered ER
    std::unordered_set<int64_t> wal_acked_seq_nos_;           // acked but ER not yet received

    // Leader-follower helpers.
    pubsub_itc_fw::ConnectionID peer_active_conn() const;
    void adopt_role(pubsub_itc_fw_app::Role new_role);
    void elect_role(int64_t peer_instance_id, int32_t peer_epoch, pubsub_itc_fw_app::Role peer_current_role);
    void send_status_query(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_status_response(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_peer_heartbeat();
    void send_arbiter_heartbeat();
    void send_arbitration_report();
    void write_fence_file() const;
    void handle_peer_status_query(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_response(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_heartbeat(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_arbitration_decision(const pubsub_itc_fw::EventMessage& message);

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
    bool replay_me_order_ready_{false};  // outbound sequencer->ME order connection up
    bool replay_me_er_ready_{false};     // inbound ME->sequencer ER connection up
    void try_dispatch_replay();          // dispatches once both flags are set
    void dispatch_replay_records();

    // WAL replication helpers (peer follower).
    [[nodiscard]] bool needs_wal_ack() const;
    void send_wal_record(int64_t seq, int16_t pdu_id, const pubsub_itc_fw::EventMessage& message, int64_t wall_time_ns);
    void handle_wal_record(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_wal_ack(const pubsub_itc_fw::EventMessage& message);
    void install_peer_wal_inline_handler(const pubsub_itc_fw::ConnectionID& conn_id);
    void flush_pending_er();
    void forward_pending_er(const PendingEr& pending);

    // External WAL subscriber helpers (MEP primary and secondary).
    void handle_wal_subscribe_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_external_wal_ack(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void stream_wal_record_to_external_subscribers(int64_t seq, int16_t pdu_id, const pubsub_itc_fw::EventMessage& message, int64_t wall_time_ns);

    // seq_no -> gateway_session_conn_id of the originating FIX session.
    // Keyed by the sequence number assigned to each NOS/OCR (globally unique,
    // unlike ClOrdID which is only unique per FIX session).  Populated on each
    // sequenced NOS/OCR; rebuilt from WAL replay on startup.  Used to stamp
    // gateway_session_conn_id on forwarded ERs so the gateway can route each
    // ER directly to the exact FIX session that placed the order, even when
    // multiple sessions share the same SenderCompID or reuse ClOrdIDs.
    std::unordered_map<int64_t, int32_t> seq_no_to_session_conn_id_;
};

} // namespaces
