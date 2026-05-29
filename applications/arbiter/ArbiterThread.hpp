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

#include <leader_follower.hpp>

#include "ArbiterConfiguration.hpp"

namespace arbiter {

/**
 * @brief ApplicationThread subclass implementing the arbiter business logic.
 *
 * The arbiter manages the leadership-state map for component pairs (sequencer
 * pair, ME pair). Two arbiter instances form an HA pair. One is the active
 * arbiter (makes leadership decisions for components); the other is the passive
 * arbiter (replicates state, ready to take over on active failure).
 *
 * The arbiter pair elects active/passive using the same StatusQuery /
 * StatusResponse / Heartbeat protocol as the sequencer peer election. When
 * both arbiters are undecided, the witness breaks the tie via
 * ArbiterVoteRequest / ArbiterVoteResponse.
 *
 * Components (sequencer, ME) connect to BOTH arbiter instances:
 *  - Active arbiter: processes ArbitrationReport (200), replies with
 *    ArbitrationDecision (201), replicates result to passive.
 *  - Passive arbiter: drops ArbitrationReport with a log warning.
 *
 * Threading: ThreadID 1.
 */
class ArbiterThread : public pubsub_itc_fw::ApplicationThread {
  public:
    ArbiterThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                  const ArbiterConfiguration& config);

  protected:
    void on_initial_event() override;
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const ArbiterConfiguration& config_;

    // Own active/passive role (independent of component leadership).
    pubsub_itc_fw_app::Role role_{pubsub_itc_fw_app::Role::unknown};
    int32_t epoch_{0};

    // Peer arbiter connections (outbound + inbound).
    pubsub_itc_fw::ConnectionID peer_conn_id_;
    pubsub_itc_fw::ConnectionID peer_inbound_conn_id_;

    // peer instance_id learned from StatusQuery/StatusResponse.
    int64_t peer_instance_id_{0};

    // Witness connection (outbound).
    pubsub_itc_fw::ConnectionID witness_conn_id_;

    // Leadership-state map: component_instance_id → leader_instance_id.
    // Also tracks the epoch for each component pair's last decision.
    struct ComponentState {
        int64_t leader_instance_id{0};
        int64_t follower_instance_id{0};
        int32_t epoch{0};
    };
    std::unordered_map<int64_t, ComponentState> leadership_state_;

    // Pending arbitration requests: component_instance_id → conn_id of requestor.
    // Held until we can send ArbitrationDecision.
    std::unordered_map<int64_t, pubsub_itc_fw::ConnectionID> pending_requests_;

    // Track all connected component instances: instance_id → ConnectionID.
    std::unordered_map<int64_t, pubsub_itc_fw::ConnectionID> component_connections_;

    // Reverse map: connection value → component instance_id (populated on Heartbeat).
    std::unordered_map<int32_t, int64_t> conn_to_component_instance_;

    // Arbiter peer helpers (mirror sequencer peer protocol).
    pubsub_itc_fw::ConnectionID peer_active_conn() const;
    void adopt_role(pubsub_itc_fw_app::Role new_role);
    void elect_role(int64_t peer_instance_id, int32_t peer_epoch, pubsub_itc_fw_app::Role peer_current_role);
    void send_status_query(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_status_response(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_peer_heartbeat();
    void send_witness_heartbeat();
    void request_witness_vote();
    void write_fence_file() const;

    // Peer PDU handlers.
    void handle_peer_pdu(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_query(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_peer_status_response(const pubsub_itc_fw::EventMessage& message);
    void handle_peer_heartbeat(const pubsub_itc_fw::EventMessage& message);
    void handle_arbiter_state_record(const pubsub_itc_fw::EventMessage& message);
    void handle_arbiter_state_ack(const pubsub_itc_fw::EventMessage& message);

    // Witness PDU handlers.
    void handle_arbiter_vote_response(const pubsub_itc_fw::EventMessage& message);

    // Component PDU handlers.
    void handle_component_heartbeat(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_arbitration_report(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);

    // Decision helpers.
    void decide_and_broadcast(int64_t self_instance_id, int64_t peer_instance_id, int32_t epoch, const pubsub_itc_fw::ConnectionID& requester_conn_id);
    void send_arbitration_decision(const pubsub_itc_fw::ConnectionID& conn_id, int64_t leader_id, int64_t follower_id, int32_t epoch);
    void replicate_state_to_peer(int64_t component_instance_id, int64_t leader_id, int32_t epoch);
};

} // namespace arbiter
