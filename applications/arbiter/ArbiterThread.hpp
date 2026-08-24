#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
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
#include "LeadershipDecision.hpp"

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

        // Whether this instance is still believed to hold leadership, as opposed to merely
        // having held it once. A lease that never expires is not a lease, it is a fact -- and
        // treating it as one meant the arbiter kept naming an instance as leader after that
        // instance had restarted and come back as a follower, because it was still connected
        // and connection says nothing about leadership. Cleared when the recorded leader
        // disconnects, and when the lease goes unrenewed; set again only by a lease or a
        // fresh decision. See docs/availability/design_notes.md 11d.
        bool leadership_confirmed{false};
        std::chrono::steady_clock::time_point leased_at{};
    };

    /// How long a lease stands without renewal before the arbiter stops believing it.
    ///
    /// A backstop rather than the mechanism: disconnection is the precise signal and clears
    /// the record at once. This catches what disconnection cannot -- a leader that holds its
    /// socket open and stops renewing, which from the outside looks identical to a healthy one.
    static constexpr std::chrono::seconds leadership_lease_ttl{10};
    // Keyed by GROUP, not by instance. "Which instance leads the matching engine?" has one
    // answer, and keying it per instance made it unanswerable without already knowing who to
    // ask about -- which is exactly what a rejoining instance does not know. See
    // LeadershipDecision.hpp.
    std::unordered_map<pubsub_itc_fw_app::ComponentGroup, ComponentState> leadership_state_;

    // When this arbiter started. leadership_state_ is memory only and nothing reads it back,
    // so a restarted arbiter begins knowing nothing -- and an arbiter that knows nothing
    // would apply the cold-start tie-break and hand leadership to the lower instance id,
    // which after a failover is the instance that just restarted with no state. For a short
    // period after starting it therefore declines to decide about a group it has heard
    // nothing about, rather than deciding wrongly. See docs/availability/design_notes.md 11c.
    std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};

    /// How long after startup the arbiter waits to be informed before it will guess.
    static constexpr std::chrono::seconds startup_learning_period{10};

    /// True while this arbiter may still be ignorant rather than genuinely facing a cold start.
    [[nodiscard]] bool within_startup_learning_period() const {
        return std::chrono::steady_clock::now() - started_at_ < startup_learning_period;
    }

    /// Replays what this arbiter knows about leadership to a peer that has just connected.
    void replay_leadership_to_peer(const pubsub_itc_fw::ConnectionID& conn_id);

    /// Records who leads a group, as asserted by the instance that holds it.
    void handle_leadership_lease(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);

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
