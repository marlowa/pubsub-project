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

#include <fix_equity_orders.hpp>

#include "MatchingEngineConfiguration.hpp"

namespace matching_engine {

/**
 * @brief ApplicationThread subclass implementing the matching engine stub.
 *
 * Receives sequenced order PDUs from the sequencer on the inbound listener,
 * maintains a primitive order book keyed by ClOrdID, and sends ExecutionReport
 * PDUs back to the sequencer over the outbound `sequencer_er` connections.
 * The sequencer routes ERs to the originating gateway.
 *
 * Order lifecycle:
 *   NOS (new ClOrdID)      → ER ExecType=New  / OrdStatus=New (order enters book)
 *   NOS (duplicate ClOrdID)→ ER ExecType=Rejected / OrdRejReason=DuplicateOrder
 *   OCR (known OrigClOrdID)→ ER ExecType=Canceled / OrdStatus=Canceled (removed from book)
 *   OCR (unknown OrigClOrdID) → ER ExecType=Rejected / OrdRejReason=UnknownOrder
 *
 * There is no real matching logic; orders sit as New until cancelled.
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
    // Live order stored in the order book from NOS acceptance until cancel.
    struct OrderEntry {
        std::string order_id;
        std::string symbol;
        pubsub_itc_fw_app::Side side;
        std::string order_qty;
        bool has_price{false};
        std::string price;
        pubsub_itc_fw_app::OrdType ord_type{};
    };

    void handle_new_order_single(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t seq_no);
    void handle_order_cancel_request(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t seq_no);
    void send_er_to_sequencer(const pubsub_itc_fw_app::ExecutionReport& er, int64_t seq_no);
    [[nodiscard]] std::string generate_order_id();
    [[nodiscard]] std::string generate_exec_id();

    const MatchingEngineConfiguration& config_;

    // ConnectionIDs of the outbound connections to the sequencer ER inbound listeners.
    // ERs are sent to all valid connections. The leader routes them to the gateway;
    // the follower discards. This ensures ERs reach whichever sequencer is currently leader.
    pubsub_itc_fw::ConnectionID sequencer_er_conn_id_;
    pubsub_itc_fw::ConnectionID sequencer_er_secondary_conn_id_;

    // Order book: composite key (gateway_session_conn_id + ':' + cl_ord_id) → live order entry.
    // Scoped per FIX session so concurrent sessions can reuse the same ClOrdID sequence
    // without collision, matching the FIX standard (ClOrdID unique per client session).
    // Orders are inserted on NOS acceptance and erased on cancel confirmation.
    std::unordered_map<std::string, OrderEntry> order_book_;

    // Monotonic counters for generated OrderID and ExecID strings.
    int64_t order_id_counter_{0};
    int64_t exec_id_counter_{0};
};

} // namespace matching_engine
