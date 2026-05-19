#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <unordered_map>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <fix_equity_orders.hpp>
#include <leader_follower.hpp>

#include "SequencerConfiguration.hpp"
#include "SequencerWal.hpp"

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
 * Leader-follower state machine: TODO -- to be implemented once the protocol
 * state machine is built. The stub behaves as leader unconditionally.
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
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const SequencerConfiguration& config_;

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
    pubsub_itc_fw::ConnectionID peer_inbound_conn_id_;   // inbound: peer connected to us
    pubsub_itc_fw::ConnectionID arbiter_conn_id_;

    // mmap'd on-disk write-ahead log (Slice 3). Opened in on_initial_event()
    // before the sequencer begins accepting connections.
    SequencerWal wal_;

    // Leader-follower state machine (slice 6).
    pubsub_itc_fw_app::Role role_{pubsub_itc_fw_app::Role::unknown};
    int32_t epoch_{0};

    // Leader-follower helpers.
    pubsub_itc_fw::ConnectionID peer_active_conn() const noexcept;
    void adopt_role(pubsub_itc_fw_app::Role new_role);
    void elect_role(int64_t peer_instance_id, int32_t peer_epoch, pubsub_itc_fw_app::Role peer_current_role);
    void send_status_query(pubsub_itc_fw::ConnectionID conn_id);
    void send_status_response(pubsub_itc_fw::ConnectionID conn_id);
    void send_peer_heartbeat();
    void write_fence_file();
    void handle_peer_status_query(pubsub_itc_fw::ConnectionID conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_response(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_heartbeat(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_pdu(pubsub_itc_fw::ConnectionID conn_id, const pubsub_itc_fw::EventMessage& message);

    // cl_ord_id → SenderCompID of the originating FIX client (Slice 5).
    // Populated on each NOS/OCR received; rebuilt from WAL replay on startup.
    // Used to stamp routing_comp_id on forwarded ERs so the gateway can route
    // without maintaining its own cl_ord_id→session map.
    std::unordered_map<std::string, std::string> cl_ord_id_to_comp_id_;
};

} // namespace sequencer
