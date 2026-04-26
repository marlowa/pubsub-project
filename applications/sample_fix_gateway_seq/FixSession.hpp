#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include "FixParser.hpp"

namespace sample_fix_gateway_seq {

/**
 * @brief Holds the state for a single active FIX 5.0SP2 / FIXT 1.1 session.
 *
 * One FixSession is created per accepted inbound connection and destroyed
 * when the connection is lost. It owns the FixParser for that connection's
 * TCP stream -- parsers are stateful (they accumulate bytes between recv()
 * calls) so each connection must have its own independent instance.
 *
 * The FixSerialiser is stateless and is shared across all sessions by
 * FixGatewaySeqThread.
 *
 * Sequence numbers reset to 1 on each new connection. A production gateway
 * would persist sequence numbers across connections but that is out of scope
 * for this sample.
 */
struct FixSession {
    /**
     * @brief Constructs a FixSession for the given connection.
     *
     * @param[in] id       The ConnectionID assigned by the reactor.
     * @param[in] logger   Logger instance. Must outlive this object.
     * @param[in] callback Called by the parser for each complete FIX message.
     */
    FixSession(pubsub_itc_fw::ConnectionID id,
               pubsub_itc_fw::QuillLogger& logger,
               FixParser::MessageCallback callback)
        : conn_id(id)
        , parser(logger, std::move(callback))
    {}

    // Not copyable -- FixParser holds a std::string buffer and std::function.
    FixSession(const FixSession&) = delete;
    FixSession& operator=(const FixSession&) = delete;

    // Moveable for unordered_map insertion.
    FixSession(FixSession&&) = default;
    FixSession& operator=(FixSession&&) = default;

    // ----------------------------------------------------------------
    // Identity
    // ----------------------------------------------------------------

    pubsub_itc_fw::ConnectionID conn_id;

    // ----------------------------------------------------------------
    // Parser -- one per connection since the FIX byte stream is stateful
    // ----------------------------------------------------------------

    FixParser parser;

    // ----------------------------------------------------------------
    // Session state
    // ----------------------------------------------------------------

    /**
     * @brief True once the inbound byte stream has been verified to start
     *        with the expected FIX preamble (8=FIXT.1.1<SOH>).
     *
     * Checked in on_raw_socket_message() before bytes are fed to the parser.
     * A connection whose first bytes do not match the preamble is disconnected
     * immediately without any FIX-level response.
     */
    bool preamble_verified{false};

    /**
     * @brief True once a valid Logon (MsgType=A) has been received and
     *        the gateway has responded with its own Logon.
     */
    bool session_established{false};

    /**
     * @brief The SenderCompID sent by the client in the Logon message.
     *
     * Used to validate subsequent messages and to set TargetCompID in
     * outbound messages to this client.
     */
    std::string client_comp_id;

    /**
     * @brief Outbound sequence number for messages sent to this client.
     * Incremented by FixGatewayThread::send_fix_to_session().
     */
    int outbound_seq_num{1};

    /**
     * @brief Counter for generating unique OrderID values for this session.
     */
    int order_id_counter{1};

    /**
     * @brief Counter for generating unique ExecID values for this session.
     */
    int exec_id_counter{1};

    /**
     * @brief Returns the timer name used for this session's logon timeout.
     *
     * Each session gets a unique name so the FixGatewayThread can start,
     * cancel, and identify the timer on a per-session basis.
     */
    [[nodiscard]] std::string logon_timeout_timer_name() const {
        return "logon_timeout_" + std::to_string(conn_id.get_value());
    }
};

} // namespace sample_fix_gateway_seq
