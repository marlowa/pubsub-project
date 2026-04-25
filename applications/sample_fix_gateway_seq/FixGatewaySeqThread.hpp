#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <unordered_map>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include "FixGatewaySeqConfiguration.hpp"

namespace sample_fix_gateway_seq {

/**
 * @brief ApplicationThread subclass for the sequencer-backed FIX gateway.
 *
 * Unlike the simple gateway, this thread does not generate ExecutionReports
 * directly. Instead it:
 *
 *   1. Accepts inbound FIX client connections (RawBytesProtocolHandler).
 *   2. Parses inbound FIX NewOrderSingle and OrderCancelRequest messages.
 *   3. Encodes them as fix_equity_orders PDUs and forwards to both the
 *      primary and secondary sequencer via outbound PDU connections.
 *   4. Receives ExecutionReport PDUs from the matching engine on a separate
 *      inbound PDU connection (stub for future pub/sub fanout).
 *   5. Decodes the ER PDU, looks up the originating FIX session by cl_ord_id,
 *      serialises a FIX ExecutionReport, and sends it to that client.
 *
 * Connection IDs for the sequencer and ME connections are set by
 * SampleFixGatewaySeq during construction and stored here for use when
 * forwarding PDUs.
 *
 * Threading: ThreadID 1.
 */
class FixGatewaySeqThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @param[in] token    Constructor token to force use of factory.
     * @param[in] logger   Logger instance. Must outlive this object.
     * @param[in] reactor  The owning Reactor. Must outlive this object.
     * @param[in] config   Gateway configuration.
     */
    FixGatewaySeqThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token,
                        pubsub_itc_fw::QuillLogger& logger,
                        pubsub_itc_fw::Reactor& reactor,
                        const FixGatewaySeqConfiguration& config);

  protected:
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_raw_socket_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const FixGatewaySeqConfiguration& config_;

    // ConnectionID of the primary sequencer outbound connection.
    // Set when on_connection_established fires for the primary sequencer.
    pubsub_itc_fw::ConnectionID sequencer_primary_conn_id_;

    // ConnectionID of the secondary sequencer outbound connection.
    pubsub_itc_fw::ConnectionID sequencer_secondary_conn_id_;

    // cl_ord_id -> ConnectionID of the originating FIX client session.
    // Used to route ExecutionReport PDUs back to the correct FIX client.
    // ERs with no cl_ord_id are logged and dropped.
    std::unordered_map<std::string, pubsub_itc_fw::ConnectionID> cl_ord_id_to_session_;

    // Set of active FIX client connection IDs.
    // Used to distinguish FIX client connections from sequencer/ME connections
    // in on_connection_established and on_connection_lost.
    std::unordered_map<pubsub_itc_fw::ConnectionID, bool> fix_client_sessions_;
};

} // namespace sample_fix_gateway_seq
