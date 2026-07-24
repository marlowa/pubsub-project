#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <charconv>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <optional>
#include <unordered_set>
#include <vector>

#include <tsl/robin_map.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <matching_engine_replication.hpp>

#include "FixOrderLimits.hpp"
#include "MatchingEngineConfiguration.hpp"

namespace matching_engine {

/**
 * @brief HA state for the matching-engine instance (Slice C+D).
 *
 * Unknown    -- HA disabled or not yet classified.
 * Follower   -- passive replica: receives BookUpdate PDUs, discards order and ER PDUs.
 * Reconciling-- promotion in progress: replaying WAL catch-up from the sequencer,
 *               applying NOS/OCR to the book WITHOUT emitting ERs.
 * Leader     -- active: processes orders and emits ERs normally.
 */
enum class MeRole { Unknown, Follower, Reconciling, Leader };

/**
 * @brief ApplicationThread subclass implementing the matching engine stub.
 *
 * Receives sequenced order PDUs from the sequencer on the inbound listener,
 * maintains a primitive order book keyed by ClOrdID, and sends ExecutionReport
 * PDUs back to the sequencer over the outbound `sequencer_er` connections.
 * The sequencer routes ERs to the originating gateway.
 *
 * Order lifecycle:
 *   NOS (new ClOrdID)      -> ER ExecType=New  / OrdStatus=New (order enters book)
 *   NOS (duplicate ClOrdID)-> ER ExecType=Rejected / OrdRejReason=DuplicateOrder
 *   OCR (known OrigClOrdID)-> ER ExecType=Canceled / OrdStatus=Canceled (removed from book)
 *   OCR (unknown OrigClOrdID) -> ER ExecType=Rejected / OrdRejReason=UnknownOrder
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
    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    // ClOrdID length is the single shared limit (fix_order_limits): the gateway validates
    // every inbound ClOrdID against it, so this fixed-size book key is never truncated.
    // Symbol max ~12; 16 is ample. Quantities and prices are decimal strings; 24 covers any realistic value.
    static constexpr size_t max_cl_ord_id_length = fix_order_limits::max_cl_ord_id_length;
    static constexpr size_t max_symbol_length = 16;
    static constexpr size_t max_qty_length = 24;

    // Composite order book key: FIX session + ClOrdID.
    // Fixed-size struct -- no heap allocation per lookup.
    struct OrderKey {
        int32_t session_id{};
        uint8_t cl_ord_id_len{};
        std::array<char, max_cl_ord_id_length> cl_ord_id{};

        static OrderKey make(int32_t sid, std::string_view id) {
            OrderKey k;
            k.session_id = sid;
            k.cl_ord_id_len = static_cast<uint8_t>(std::min(id.size(), max_cl_ord_id_length));
            std::memcpy(k.cl_ord_id.data(), id.data(), k.cl_ord_id_len);
            return k;
        }

        bool operator==(const OrderKey& other) const {
            return session_id == other.session_id && cl_ord_id_len == other.cl_ord_id_len &&
                   std::memcmp(cl_ord_id.data(), other.cl_ord_id.data(), cl_ord_id_len) == 0;
        }
    };

    struct OrderKeyHash {
        size_t operator()(const OrderKey& key) const {
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
    // All string fields stored as fixed-size char arrays -- no heap allocation.
    struct OrderEntry {
        int64_t order_id_num{};            // counter value; formatted to "ME-ORD-N" on demand
        int32_t gateway_session_conn_id{}; // originating FIX session; used to route cancel ERs on failover
        pubsub_itc_fw_app::Side side{};
        pubsub_itc_fw_app::OrdType ord_type{};
        bool has_price{false};
        uint8_t symbol_len{};
        uint8_t order_qty_len{};
        uint8_t price_len{};
        std::array<char, max_symbol_length> symbol{};
        std::array<char, max_qty_length> order_qty{};
        std::array<char, max_qty_length> price{};

        void set_symbol(std::string_view sv) {
            symbol_len = static_cast<uint8_t>(std::min(sv.size(), max_symbol_length));
            std::memcpy(symbol.data(), sv.data(), symbol_len);
        }
        void set_order_qty(std::string_view sv) {
            order_qty_len = static_cast<uint8_t>(std::min(sv.size(), max_qty_length));
            std::memcpy(order_qty.data(), sv.data(), order_qty_len);
        }
        void set_price(std::string_view sv) {
            price_len = static_cast<uint8_t>(std::min(sv.size(), max_qty_length));
            std::memcpy(price.data(), sv.data(), price_len);
        }

        [[nodiscard]] std::string_view get_symbol() const {
            return {symbol.data(), symbol_len};
        }
        [[nodiscard]] std::string_view get_order_qty() const {
            return {order_qty.data(), order_qty_len};
        }
        [[nodiscard]] std::string_view get_price() const {
            return {price.data(), price_len};
        }
    };

    // Helper: format "ME-ORD-N" or "ME-EXEC-N" into a caller-provided stack buffer.
    // Returns string_view into that buffer. Buffer must outlive the view.
    template <size_t N> static std::string_view format_id(std::array<char, N>& buf, const char* prefix, size_t prefix_len, int64_t counter) {
        std::memcpy(buf.data(), prefix, prefix_len);
        auto [end, ec] = std::to_chars(buf.data() + prefix_len, buf.data() + N, counter);
        return {buf.data(), static_cast<size_t>(end - buf.data())};
    }

    // Both come from the WalRecord envelope, not the (DD-derived) FIX PDU:
    // sequenced_at_ns is the sequencer's wall time used as transact_time (0 => not
    // stamped, fall back to the local wall clock); gateway_session_conn_id is the
    // originating FIX session's connection id, forming half of the order key (0 if absent).
    void handle_new_order_single(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t seq_no, int64_t sequenced_at_ns, int32_t gateway_session_conn_id);
    void handle_order_cancel_request(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t seq_no, int64_t sequenced_at_ns,
                                     int32_t gateway_session_conn_id);
    // Reusable scratch buffer for encoding an ExecutionReport before wrapping it in a
    // WalRecord envelope (send_er_to_sequencer). Grown to the largest ER seen and
    // reused -- no fixed cap that could silently drop an ER, no per-ER allocation.
    std::vector<uint8_t> er_encode_buffer_;

    // Wraps the ER in a WalRecord envelope and sends it to the sequencer(s). Routing
    // metadata for ERs not tied to a sequenced order (the seq_no==0 cancel-on-failover
    // ERs) rides on the envelope, so the ER PDU itself stays purely DD-derived. For
    // ordinary ERs the sequencer routes by the echoed seq_no, so no conn id is supplied.
    void send_er_to_sequencer(const pubsub_itc_fw_app::ExecutionReport& er, int64_t seq_no, std::optional<int32_t> gateway_session_conn_id = std::nullopt);

    const MatchingEngineConfiguration& config_;

    // HA role flags (set from configuration at construction).
    bool ha_enabled_{false};
    bool is_primary_{true};

    // Primary: outbound connection to ME-secondary's book replication listener.
    pubsub_itc_fw::ConnectionID secondary_replication_conn_id_;

    // Secondary: inbound replication connection from ME-primary.
    // Any inbound connection on the secondary IS the replication channel
    // (the secondary only has one inbound listener: the replication port).
    pubsub_itc_fw::ConnectionID primary_replication_conn_id_;

    // Secondary: seq_no of the most recently applied BookUpdate.
    // Carried forward to WAL reconciliation at promotion time (Slice C).
    int64_t last_replicated_seq_no_{0};

    // ConnectionIDs of the outbound connections to the sequencer ER inbound listeners.
    // ERs are sent to all valid connections. The leader routes them to the gateway;
    // the follower discards. This ensures ERs reach whichever sequencer is currently leader.
    pubsub_itc_fw::ConnectionID sequencer_er_conn_id_;
    pubsub_itc_fw::ConnectionID sequencer_er_secondary_conn_id_;

    // Order book keyed by (session_id, cl_ord_id).
    // Primary:   live orders currently on the book.
    // Secondary: replica of the primary's book, maintained via BookUpdate PDUs.
    tsl::robin_map<OrderKey, OrderEntry, OrderKeyHash> order_book_;

    // Monotonic counters for generated OrderID and ExecID values (primary only).
    int64_t order_id_counter_{0};
    int64_t exec_id_counter_{0};

    // Helper: send a BookUpdate PDU to ME-secondary (primary only).
    void send_book_update(int64_t seq_no, pubsub_itc_fw_app::BookUpdateType update_type, int32_t session_id, std::string_view cl_ord_id,
                          const OrderEntry* entry); // nullptr for Remove updates

    // Helper: apply a received BookUpdate PDU to the replica book (secondary only).
    void apply_book_update(const pubsub_itc_fw::EventMessage& message);

    // HA state machine (Slice C+D)

    // Current HA role. Unknown for a non-HA (single-instance) ME and for the
    // primary before it adopts leadership; Follower for the passive secondary.
    MeRole ha_role_state_{MeRole::Unknown};

    // Secondary: true while the promotion-timeout timer is armed (primary
    // replication connection has been lost and we are waiting to see if it
    // reconnects before requesting arbitration).
    bool promotion_pending_{false};

    // Leadership generation. Adopted from ArbitrationDecision, or self-incremented
    // on degraded self-promotion.
    int32_t epoch_{0};

    // Secondary: instance_id of the primary (peer). Fixed at 1 by convention.
    static constexpr int64_t primary_instance_id = 1;

    // Outbound connections to the arbiter pool (both roles when HA is enabled).
    pubsub_itc_fw::ConnectionID arbiter_primary_conn_id_;
    pubsub_itc_fw::ConnectionID arbiter_secondary_conn_id_;

    // Inbound order connections from the sequencers. Both the leader and follower
    // sequencer open a (pre-warmed) order connection to this ME, so there may be
    // more than one. On promotion we send MePositionRequest to every one of them:
    // the ME cannot tell which sequencer is the leader, so it asks all and lets the
    // leader serve WAL catch-up while followers re-point without streaming (see
    // SequencerThread::handle_me_position_request). This guarantees the leader --
    // whichever it is -- reconciles this ME and re-routes live orders here.
    std::unordered_set<pubsub_itc_fw::ConnectionID> sequencer_order_conn_ids_;

    // Arbiter-mediated promotion helpers.
    void enter_follower_state();
    void adopt_leader_role();
    void send_arbitration_report();
    void handle_arbitration_decision(const pubsub_itc_fw::EventMessage& message);
    void send_arbiter_heartbeat();

    // WAL reconciliation (RECONCILING state).
    void begin_reconciliation();
    void send_me_position_request();
    void handle_me_position_ack(const pubsub_itc_fw::EventMessage& message);
    void cancel_all_orders_on_failover();
};

} // namespaces
