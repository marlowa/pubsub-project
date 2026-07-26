#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>

#include "OpenOrderEntry.hpp"

namespace binary_gateway {

/**
 * @brief One logged-on binary client connection.
 *
 * Far smaller than its FIX counterpart, and deliberately so. A FIX session has to
 * track sequence numbers, heartbeat state and a logon deadline because it runs over
 * a bare byte stream that it must itself keep ordered and alive. Here the framework's
 * PDU transport already frames, orders and reaps connections, so all that is left
 * for a session to remember is who it belongs to.
 */
struct BinarySession {
    /** @brief The client's connection, and the key the sequencer routes ERs back by. */
    pubsub_itc_fw::ConnectionID conn_id;

    /** @brief Identity from the Logon PDU; travels on the envelope for audit. */
    std::string comp_id;

    /**
     * @brief Whether Logon has been accepted.
     *
     * A connection exists before its client has identified itself. Orders arriving in
     * that window are refused rather than forwarded, so that every order in the
     * pipeline carries a comp id.
     */
    bool logged_on{false};

    /**
     * @brief Orders this session has resting on the book, keyed by ClOrdID.
     *
     * Populated from the matching engine's acknowledgements rather than at order-forward
     * time, so it holds only orders genuinely on the book -- an order that was rejected,
     * or is still in flight, was never the session's to cancel. Entries are cancelled
     * when the client disconnects, so a dropped connection does not leave live orders
     * behind with nobody to manage them.
     *
     * The counter that makes each generated cancel's ClOrdID unique lives with the
     * session so it keeps counting while the session's orders are drained.
     */
    open_orders::OpenOrderMap open_orders;
    int cancel_id_counter{1};
};

} // namespaces
