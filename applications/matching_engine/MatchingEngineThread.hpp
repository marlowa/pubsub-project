#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <charconv>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
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
    // Maximum character lengths for fixed-size fields. FIX spec allows up to
    // 20 chars for ClOrdID; 32 provides headroom. Symbol max ~12; 16 is ample.
    // Quantities and prices are decimal strings; 24 covers any realistic value.
    static constexpr size_t max_cl_ord_id_length = 32;
    static constexpr size_t max_symbol_length    = 16;
    static constexpr size_t max_qty_length       = 24;

    // Composite order book key: FIX session + ClOrdID.
    // Fixed-size struct — no heap allocation per lookup.
    struct OrderKey {
        int32_t session_id{};
        uint8_t cl_ord_id_len{};
        std::array<char, max_cl_ord_id_length> cl_ord_id{};

        static OrderKey make(int32_t sid, std::string_view id) noexcept {
            OrderKey k;
            k.session_id = sid;
            k.cl_ord_id_len = static_cast<uint8_t>(std::min(id.size(), max_cl_ord_id_length));
            std::memcpy(k.cl_ord_id.data(), id.data(), k.cl_ord_id_len);
            return k;
        }

        bool operator==(const OrderKey& other) const noexcept {
            return session_id == other.session_id
                && cl_ord_id_len == other.cl_ord_id_len
                && std::memcmp(cl_ord_id.data(), other.cl_ord_id.data(), cl_ord_id_len) == 0;
        }
    };

    struct OrderKeyHash {
        size_t operator()(const OrderKey& key) const noexcept {
            // FNV-1a over session_id bytes then cl_ord_id bytes.
            size_t h = 14695981039346656037ULL;
            const auto* sid_bytes = reinterpret_cast<const uint8_t*>(&key.session_id);
            for (size_t i = 0; i < sizeof(key.session_id); ++i) {
                h ^= sid_bytes[i];
                h *= 1099511628211ULL;
            }
            for (uint8_t i = 0; i < key.cl_ord_id_len; ++i) {
                h ^= static_cast<uint8_t>(key.cl_ord_id[i]);
                h *= 1099511628211ULL;
            }
            return h;
        }
    };

    // Live order stored in the order book from NOS acceptance until cancel.
    // All string fields stored as fixed-size char arrays — no heap allocation.
    struct OrderEntry {
        int64_t order_id_num{};  // counter value; formatted to "ME-ORD-N" on demand
        pubsub_itc_fw_app::Side side{};
        pubsub_itc_fw_app::OrdType ord_type{};
        bool has_price{false};
        uint8_t symbol_len{};
        uint8_t order_qty_len{};
        uint8_t price_len{};
        std::array<char, max_symbol_length> symbol{};
        std::array<char, max_qty_length>    order_qty{};
        std::array<char, max_qty_length>    price{};

        void set_symbol(std::string_view sv) noexcept {
            symbol_len = static_cast<uint8_t>(std::min(sv.size(), max_symbol_length));
            std::memcpy(symbol.data(), sv.data(), symbol_len);
        }
        void set_order_qty(std::string_view sv) noexcept {
            order_qty_len = static_cast<uint8_t>(std::min(sv.size(), max_qty_length));
            std::memcpy(order_qty.data(), sv.data(), order_qty_len);
        }
        void set_price(std::string_view sv) noexcept {
            price_len = static_cast<uint8_t>(std::min(sv.size(), max_qty_length));
            std::memcpy(price.data(), sv.data(), price_len);
        }

        [[nodiscard]] std::string_view get_symbol()    const noexcept { return {symbol.data(),    symbol_len};    }
        [[nodiscard]] std::string_view get_order_qty() const noexcept { return {order_qty.data(), order_qty_len}; }
        [[nodiscard]] std::string_view get_price()     const noexcept { return {price.data(),     price_len};     }
    };

    // Helper: format "ME-ORD-N" or "ME-EXEC-N" into a caller-provided stack buffer.
    // Returns string_view into that buffer. Buffer must outlive the view.
    template <size_t N>
    static std::string_view format_id(std::array<char, N>& buf,
                                      const char* prefix, size_t prefix_len,
                                      int64_t counter) noexcept {
        std::memcpy(buf.data(), prefix, prefix_len);
        auto [end, ec] = std::to_chars(buf.data() + prefix_len, buf.data() + N, counter);
        return {buf.data(), static_cast<size_t>(end - buf.data())};
    }

    void handle_new_order_single(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t seq_no);
    void handle_order_cancel_request(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t seq_no);
    void send_er_to_sequencer(const pubsub_itc_fw_app::ExecutionReport& er, int64_t seq_no);

    const MatchingEngineConfiguration& config_;

    // ConnectionIDs of the outbound connections to the sequencer ER inbound listeners.
    // ERs are sent to all valid connections. The leader routes them to the gateway;
    // the follower discards. This ensures ERs reach whichever sequencer is currently leader.
    pubsub_itc_fw::ConnectionID sequencer_er_conn_id_;
    pubsub_itc_fw::ConnectionID sequencer_er_secondary_conn_id_;

    // Order book keyed by (session_id, cl_ord_id) — scoped per FIX session so
    // concurrent sessions can reuse the same ClOrdID sequence without collision,
    // matching the FIX standard (ClOrdID unique per client session).
    std::unordered_map<OrderKey, OrderEntry, OrderKeyHash> order_book_;

    // Monotonic counters for generated OrderID and ExecID values.
    int64_t order_id_counter_{0};
    int64_t exec_id_counter_{0};
};

} // namespace matching_engine
