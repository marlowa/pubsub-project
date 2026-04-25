#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include "MatchingEngineConfiguration.hpp"

namespace matching_engine {

/**
 * @brief ApplicationThread subclass implementing the matching engine stub.
 *
 * Receives SequencedMessage PDUs from the sequencer, unwraps the inner
 * order PDU, and sends a placeholder ExecutionReport PDU back to the
 * gateway.
 *
 * The order book and matching logic are TODO. The stub logs each inbound
 * PDU and sends a minimal filled ER so the end-to-end flow can be verified.
 *
 * Threading: ThreadID 1.
 */
class MatchingEngineThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @param[in] token    Constructor token to force use of factory.
     * @param[in] logger   Logger. Must outlive this object.
     * @param[in] reactor  Owning Reactor. Must outlive this object.
     * @param[in] config   Matching engine configuration.
     */
    MatchingEngineThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token,
                         pubsub_itc_fw::QuillLogger& logger,
                         pubsub_itc_fw::Reactor& reactor,
                         const MatchingEngineConfiguration& config);

  protected:
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const MatchingEngineConfiguration& config_;

    // ConnectionID of the outbound gateway ER connection.
    // TODO: replace with pub/sub fanout when implemented.
    pubsub_itc_fw::ConnectionID gateway_conn_id_;
};

} // namespace matching_engine
