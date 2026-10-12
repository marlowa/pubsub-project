#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/ConnectionID.hpp>

#include <leader_follower.hpp>

namespace fix_common {

/**
 * @brief How a PairLeaseAgent reaches the other two voters. Implemented by the thread that owns the agent.
 *
 * The agent decides what to send; the thread knows which connections lead where. For a component
 * instance the peer is the other instance of its pair and the third voter is the arbiter pool, reached
 * on both arbiter connections because only the active arbiter answers. For an arbiter the peer is the
 * other arbiter and the third voter is the witness.
 */
class LeaseLinksInterface {
  public:
    virtual ~LeaseLinksInterface() = default;

    /// Send @p request to the peer, if it can be reached. A request that cannot be sent is simply not sent.
    virtual void send_request_to_peer(const pubsub_itc_fw_app::LeaseRequest& request) = 0;

    /// Send @p request to the third voter on every connection that leads to it.
    virtual void send_request_to_third_voter(const pubsub_itc_fw_app::LeaseRequest& request) = 0;

    /// Answer a request on the connection it arrived on.
    virtual void send_grant(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseGrant& grant) = 0;

    /// Answer a request on the connection it arrived on.
    virtual void send_refusal(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw_app::LeaseRefusal& refusal) = 0;
};

} // namespaces
