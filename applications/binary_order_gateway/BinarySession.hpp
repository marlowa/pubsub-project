#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <string>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

#include "OpenOrderEntry.hpp"

namespace binary_order_gateway {

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
     * @brief Whether Logon has been accepted and authentication has succeeded.
     *
     * A connection exists before its client has identified itself, and identifying itself
     * is not enough -- the SCRAM exchange has to complete first. Orders arriving before
     * then are refused, so nothing reaches the book from an unauthenticated session.
     */
    bool logged_on{false};

    /** @brief True between sending the AuthenticationRequest and the result arriving. */
    bool auth_pending{false};

    /** @brief The venue the client named in its Logon, retained for audit. */
    std::string target_comp_id;

    /** @brief Random nonce this session contributed to the SCRAM exchange. */
    std::vector<uint8_t> scram_client_nonce;

    /** @brief ServerSignature this session expects back, to authenticate the server in turn. */
    std::vector<uint8_t> scram_expected_server_signature;

    /**
     * @brief The password from Logon, held only until the SCRAM proof is derived.
     *
     * Zeroed and released the moment the derivation is done, so a session that is merely
     * open is not also a place a password is sitting in memory.
     */
    std::string client_password;

    /** @brief Fires if the authentication service does not answer, so a logon cannot hang. */
    pubsub_itc_fw::TimerID scram_auth_timeout_timer_id{};

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
