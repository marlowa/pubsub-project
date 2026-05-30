#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <string>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <fix_equity_orders.hpp>

#include "MatchingEngineConfiguration.hpp"

namespace matching_engine {

/**
 * @brief ApplicationThread subclass implementing the matching engine stub.
 *
 * Receives sequenced order PDUs from the sequencer on the inbound listener,
 * fabricates a fully-filled ExecutionReport, and sends it back to the
 * sequencer over the outbound `sequencer_er` connection. The sequencer is
 * responsible for routing the ER on to the originating gateway.
 *
 * The order book and matching logic are TODO. The stub fills every order
 * at the requested limit price (or a sentinel zero for market orders) so
 * the end-to-end comms round-trip can be verified.
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
    MatchingEngineThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor,
                         const MatchingEngineConfiguration& config);

  protected:
    void on_app_ready_event() override;
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    void handle_new_order_single(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t seq_no);
    [[nodiscard]] std::string generate_order_id();
    [[nodiscard]] std::string generate_exec_id();

    const MatchingEngineConfiguration& config_;

    // ConnectionIDs of the outbound connections to the sequencer ER inbound listeners.
    // ERs are sent to all valid connections. The leader routes them to the gateway;
    // the follower discards. This ensures ERs reach whichever sequencer is currently leader.
    pubsub_itc_fw::ConnectionID sequencer_er_conn_id_;
    pubsub_itc_fw::ConnectionID sequencer_er_secondary_conn_id_;

    // Monotonic counters for fabricated OrderID and ExecID strings.
    int64_t order_id_counter_{0};
    int64_t exec_id_counter_{0};
};

} // namespace matching_engine
