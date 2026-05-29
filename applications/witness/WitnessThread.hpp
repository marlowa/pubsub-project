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

#include "WitnessConfiguration.hpp"

namespace witness {

/**
 * @brief ApplicationThread subclass implementing the witness side of the
 *        arbiter pool.
 *
 * The witness holds NO state. Its sole purpose is to break ties between the
 * two arbiter instances:
 *
 *   - Accepts inbound connections from arbiter-primary and arbiter-secondary.
 *   - Identifies each connection from the instance_id in ArbiterHeartbeat PDUs.
 *   - Receives ArbiterVoteRequest PDUs (pdu_id=301) from an arbiter that is
 *     contemplating promotion to the active role.
 *   - Replies with ArbiterVoteResponse PDUs (pdu_id=302) granting the vote to
 *     the arbiter with the lower instance_id (or to the requester if its peer
 *     is not currently connected to the witness).
 *
 * The witness never interacts with sequencer or ME instances directly.
 * Sequencer/ME election is managed by the arbiter pair, not the witness.
 *
 * Threading: ThreadID 1.
 */
class WitnessThread : public pubsub_itc_fw::ApplicationThread {
  public:
    WitnessThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                  const WitnessConfiguration& config);

  protected:
    void on_initial_event() override;
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const WitnessConfiguration& config_;

    // Highest epoch seen from any arbiter. Used to assign the epoch in
    // ArbiterVoteResponse (max_observed_epoch_ + 1).
    int32_t max_observed_epoch_{0};

    // connection value → arbiter instance_id, populated on first ArbiterHeartbeat.
    std::unordered_map<int32_t, int64_t> conn_to_instance_id_;

    // arbiter instance_id → ConnectionID, used to identify connected arbiters.
    std::unordered_map<int64_t, pubsub_itc_fw::ConnectionID> instance_to_conn_id_;

    void handle_arbiter_heartbeat(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void handle_arbiter_vote_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& message);
    void send_arbiter_vote_response(const pubsub_itc_fw::ConnectionID& conn_id, int64_t granted_to_instance_id, int32_t epoch);
};

} // namespace witness
