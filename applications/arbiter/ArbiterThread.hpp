#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
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
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const ArbiterConfiguration& config_;

    // Own active/passive role (independent of component leadership).
    pubsub_itc_fw_app::Role role_{pubsub_itc_fw_app::Role::unknown};
    int32_t epoch_{0};

    // Timer ids for this thread's timers (default-constructed = not scheduled).
    // on_timer_event recognises a fired timer by comparing against these.
    pubsub_itc_fw::TimerID peer_heartbeat_timer_id_{};
    pubsub_itc_fw::TimerID witness_heartbeat_timer_id_{};
    pubsub_itc_fw::TimerID peer_heartbeat_timeout_timer_id_{};
    pubsub_itc_fw::TimerID vote_timeout_timer_id_{};

    // Peer arbiter connections (outbound + inbound).
    pubsub_itc_fw::ConnectionID peer_conn_id_;
    pubsub_itc_fw::ConnectionID peer_inbound_conn_id_;

    // peer instance_id learned from StatusQuery/StatusResponse.
    int64_t peer_instance_id_{0};

    // Witness connection (outbound).
    pubsub_itc_fw::ConnectionID witness_conn_id_;

    // A component is identified by (group, instance_id). The arbiter pool is
    // shared by several independent HA pairs (sequencer, matching_engine, ...),
    // each numbering its members instance_id 1/2; the group disambiguates them so
    // one pair's election cannot contaminate another's leadership state.
    struct ComponentKey {
        pubsub_itc_fw_app::ComponentGroup group{pubsub_itc_fw_app::ComponentGroup::unknown};
        int64_t instance_id{0};
        bool operator==(const ComponentKey& other) const {
            return group == other.group && instance_id == other.instance_id;
        }
    };
    struct ComponentKeyHash {
        size_t operator()(const ComponentKey& key) const {
            return (static_cast<size_t>(key.group) * 1099511628211ULL) ^ static_cast<size_t>(key.instance_id);
        }
    };

    // Leadership-state map: (group, instance_id) -> assigned leader/follower/epoch.
    // Tracks the epoch for each component pair's last decision.
    struct ComponentState {
        int64_t leader_instance_id{0};
        int64_t follower_instance_id{0};
        int32_t epoch{0};
    };
    std::unordered_map<ComponentKey, ComponentState, ComponentKeyHash> leadership_state_;

    // Pending arbitration requests: (group, instance_id) -> conn_id of requestor.
    // Held until we can send ArbitrationDecision.
    std::unordered_map<ComponentKey, pubsub_itc_fw::ConnectionID, ComponentKeyHash> pending_requests_;

    // Track all connected component instances: (group, instance_id) -> ConnectionID.
    std::unordered_map<ComponentKey, pubsub_itc_fw::ConnectionID, ComponentKeyHash> component_connections_;

    // Reverse map: connection value -> (group, instance_id) (populated on Heartbeat).
    std::unordered_map<int32_t, ComponentKey> conn_to_component_instance_;

    // Arbiter peer helpers (mirror sequencer peer protocol).
    pubsub_itc_fw::ConnectionID peer_active_conn() const;
    void adopt_role(pubsub_itc_fw_app::Role new_role);
    void elect_role(int64_t peer_instance_id, int32_t peer_epoch, pubsub_itc_fw_app::Role peer_current_role);
    void send_status_query(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_status_response(const pubsub_itc_fw::ConnectionID& conn_id);
    void send_peer_heartbeat();
    void send_witness_heartbeat();
    void request_witness_vote();

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
    void decide_and_broadcast(pubsub_itc_fw_app::ComponentGroup group, int64_t self_instance_id, int64_t peer_instance_id, int32_t epoch,
                              const pubsub_itc_fw::ConnectionID& requester_conn_id);
    void send_arbitration_decision(const pubsub_itc_fw::ConnectionID& conn_id, pubsub_itc_fw_app::ComponentGroup group, int64_t leader_id, int64_t follower_id,
                                   int32_t epoch);
    void replicate_state_to_peer(pubsub_itc_fw_app::ComponentGroup group, int64_t component_instance_id, int64_t leader_id, int32_t epoch);
};

} // namespaces
