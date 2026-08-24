#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <cstring>
#include <optional>
#include <unordered_set>
#include <vector>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/CounterHandle.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/IncrementalRehashMap.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <matching_engine_replication.hpp>

#include "EpochStore.hpp"
#include "FixOrderLimits.hpp"
#include "GatewayIds.hpp"
#include "MatchingEngineConfiguration.hpp"
#include "OrderBookMetricsReporter.hpp"
#include "OrderKey.hpp"

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
    void on_timer_event(pubsub_itc_fw::TimerID id) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    // ClOrdID length is the single shared limit (fix_order_limits): the gateway validates
    // every inbound ClOrdID against it, so this fixed-size book key is never truncated.
    // Symbol max ~12; 16 is ample. Quantities and prices are decimal strings; 24 covers any realistic value.
    static constexpr size_t max_cl_ord_id_length = fix_order_limits::max_cl_ord_id_length;
    static constexpr size_t max_symbol_length = 16;
    static constexpr size_t max_qty_length = 24;

    // Live order stored in the order book from NOS acceptance until cancel.
    // All string fields stored as fixed-size char arrays -- no heap allocation.
    struct OrderEntry {
        int64_t order_id_num{}; // counter value; formatted to "ME-ORD-N" on demand
        // Whose order this is: the comp id and the protocol, which together are the key
        // this entry is filed under and the address-free way to reach the member again.
        //
        // It used to be the originating connection id, plus the protocol and instance
        // needed to disambiguate it. That identified a socket rather than a member, which
        // failed in exactly the case this field exists for: a cancel-on-failover ER is
        // emitted when a process has died, so the connection it named was already gone,
        // and a member reconnecting elsewhere could neither be sent the report nor cancel
        // what it had left resting. The sequencer now resolves this identity to wherever
        // the session is currently bound. See docs/design/gateway_ha.md.
        fix_common::SessionIdentity session;
        pubsub_itc_fw_app::Side side{};
        pubsub_itc_fw_app::OrdType ord_type{};
        // Echoed back on every ExecutionReport for this order. The gateway needs it to
        // decide whether cancel-on-disconnect applies: a GoodTillCancel or GoodTillDate
        // order is by definition meant to outlive the session that placed it, so a
        // dropped socket must not retire it. has_time_in_force is false when the client
        // sent no tag 59, in which case no exemption is claimed and Day is implied.
        bool has_time_in_force{false};
        pubsub_itc_fw_app::TimeInForce time_in_force{};
        // When a GoodTillDate order expires, exactly as the member stated it.
        //
        // Echoed back unaltered, and stored here rather than re-read from the order because
        // reports are emitted long after the NewOrderSingle has gone.
        //
        // Worth saying explicitly, where the same about the symbol or the quantity would be
        // noise: **adjusting a GTD expiry is a real venue behaviour**, not a bug. Venues
        // truncate to a maximum lifetime, round to session close, or resolve a date against
        // a trading calendar, and are not misbehaving when they do. This venue does not, so
        // the member's stated instant is the one that governs. That is a choice between
        // defensible alternatives, which is why it is written down and tested rather than
        // left to be inferred from the absence of conversion code.
        bool has_expire_time{false};
        int64_t expire_time{};
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
    // stamped, fall back to the local wall clock); session is the identity of the client
    // session that placed the order -- its comp id and protocol -- which forms half of the
    // order key. Deliberately not the connection it arrived on: that is where the member
    // was, and the book has to be keyed on who it is, or an order becomes unmanageable the
    // moment its connection drops. An empty identity means a record with no originating
    // client session at all.
    void handle_new_order_single(const pubsub_itc_fw_app::NewOrderSingleView& view, int64_t seq_no, int64_t sequenced_at_ns,
                                 const fix_common::SessionIdentity& session);
    void handle_order_cancel_request(const pubsub_itc_fw_app::OrderCancelRequestView& view, int64_t seq_no, int64_t sequenced_at_ns,
                                     const fix_common::SessionIdentity& session);
    // Reusable scratch buffer for encoding an ExecutionReport before wrapping it in a
    // WalRecord envelope (send_er_to_sequencer). Grown to the largest ER seen and
    // reused -- no fixed cap that could silently drop an ER, no per-ER allocation.
    std::vector<uint8_t> er_encode_buffer_;

    // Reusable arena backing the ER's echoed group element arrays (handle_new_order_single).
    // Sized to need (grow-and-retry), not a fixed cap that could silently drop echoed groups.
    static constexpr size_t initial_er_group_arena_size = 4096;
    static constexpr size_t max_er_group_arena_size = 1u << 20; // 1 MiB sanity ceiling
    std::vector<uint8_t> er_group_arena_buffer_ = std::vector<uint8_t>(initial_er_group_arena_size);

    // Wraps the ER in a WalRecord envelope and sends it to the sequencer(s). Routing
    // metadata for ERs not tied to a sequenced order (the seq_no==0 cancel-on-failover
    // ERs) rides on the envelope, so the ER PDU itself stays purely DD-derived. For
    // ordinary ERs the sequencer routes by the echoed seq_no, so no session is supplied.
    //
    // What travels is the session's identity, never an address. The ME has no idea where a
    // session is connected -- and on the path that needs this most, promotion after a
    // gateway or ME failure, any address it remembered would name a process that has since
    // died. The sequencer holds the live bindings and resolves the identity when it sends.
    void send_er_to_sequencer(const pubsub_itc_fw_app::ExecutionReport& er, int64_t seq_no,
                              const fix_common::SessionIdentity& session = fix_common::SessionIdentity{});

    const MatchingEngineConfiguration& config_;

    /**
     * Orders accepted onto the book, incremented on the one path that puts one there.
     *
     * Deliberately not a count of order PDUs arriving. The reconciliation path replays the
     * WAL into the book without acknowledging anything and the follower path discards
     * orders outright, so counting arrivals would make the series jump by the whole
     * replayed backlog at each failover and stop meaning "orders the venue processed".
     * Duplicate ClOrdIDs, which are rejected, are not counted either.
     *
     * Cancels are not counted here. A cancel is a different operation on an existing
     * order, and folding the two together would leave neither rate readable.
     */
    pubsub_itc_fw::CounterHandle orders_processed_counter_;

    // HA role flags (set from configuration at construction).
    bool ha_enabled_{false};
    bool is_primary_{true};

    // Primary: outbound connection to ME-secondary's book replication listener.
    pubsub_itc_fw::ConnectionID outbound_replication_conn_id_;

    // Secondary: inbound replication connection from ME-primary.
    // Any inbound connection on the secondary IS the replication channel
    // (the secondary only has one inbound listener: the replication port).
    pubsub_itc_fw::ConnectionID inbound_replication_conn_id_;

    // Secondary: seq_no of the most recently applied BookUpdate.
    // Carried forward to WAL reconciliation at promotion time (Slice C).
    int64_t last_replicated_seq_no_{0};

    // ConnectionIDs of the outbound connections to the sequencer ER inbound listeners.
    // ERs are sent to all valid connections. The leader routes them to the gateway;
    // the follower discards. This ensures ERs reach whichever sequencer is currently leader.
    pubsub_itc_fw::ConnectionID sequencer_er_conn_id_;
    pubsub_itc_fw::ConnectionID sequencer_er_secondary_conn_id_;

    // Order book keyed by (session identity, cl_ord_id).
    // Primary:   live orders currently on the book.
    // Secondary: replica of the primary's book, maintained via BookUpdate PDUs.
    //
    // On IncrementalRehashMap rather than a std-style map because growth must not stall the
    // reactor callback thread. A conventional hash map rehashes its whole table inside the
    // one insert that crosses the load factor: at 2^23 orders that was measured at over a
    // second, during which this thread matches nothing. IncrementalRehashMap spreads the
    // same work across the following operations, a fixed few slots at a time, so the cost is
    // paid in bounded instalments by many orders instead of in full by one.
    //
    // The map reports growth directly to book_growth_reporter_ rather than through an
    // allocator: it owns its tables outright and knows each one's size at the point it
    // allocates it, so the reporter is told the truth without an allocator in between. The
    // instrumentation matters because the framework's pool and slab allocators cover objects
    // with a message lifetime, and the book is long-lived state that grows -- uninstrumented,
    // it reached 9.9 GB and the process was OOM-killed having logged no memory warning.
    pubsub_itc_fw::AllocationGrowthReporter book_growth_reporter_;

    // How big the book actually is. Sampled on a timer rather than written on every
    // order: the value is wanted as a trend over a trading day, and touching a gauge
    // on the order path would put metrics work in the hot path to buy resolution
    // nobody reads.
    fix_common::OrderBookMetricsReporter book_metrics_;

    // Scope for the book's gauges. A metric key token, so [A-Za-z0-9_]+ only.
    static constexpr const char* book_metrics_scope = "order_book";

    // Often enough to show a burst of resting orders building, rare enough to be
    // invisible against order flow. Matches the gateways' pool sampling.
    static constexpr std::chrono::seconds book_metrics_sample_interval{5};
    pubsub_itc_fw::IncrementalRehashMap<OrderKey, OrderEntry, OrderKeyHash> order_book_;

    // Monotonic counters for generated OrderID and ExecID values (primary only).
    int64_t order_id_counter_{0};
    int64_t exec_id_counter_{0};

    // Helper: send a BookUpdate PDU to ME-secondary (primary only).
    void send_book_update(int64_t seq_no, pubsub_itc_fw_app::BookUpdateType update_type, const fix_common::SessionIdentity& session, std::string_view cl_ord_id,
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
    // The leadership generation. Never assign to this directly: go through
    // set_epoch(), which also writes it to disk. A restart that forgets the
    // epoch lets this node claim a generation the venue has already spent.
    int32_t epoch_{0};

    // Where the epoch outlives the process. Read once at startup, rewritten
    // whenever the epoch moves.
    fix_common::EpochStore epoch_store_;

    // Timer ids (default-constructed = not scheduled); on_timer_event compares
    // a fired timer's id against these to identify it.
    pubsub_itc_fw::TimerID promotion_timeout_timer_id_{};
    pubsub_itc_fw::TimerID arbiter_heartbeat_timer_id_{};

    // Armed when a primary asks the arbiter who leads at startup, and cancelled by the
    // answer. If it fires, no arbiter replied and the venue would otherwise have no matching
    // engine leader at all, so the instance-id rule is applied locally and logged as degraded
    // -- the same fallback the sequencer has, and for the same reason.
    pubsub_itc_fw::TimerID startup_arbitration_timer_id_{};
    pubsub_itc_fw::TimerID book_metrics_timer_id_{};

    /// How many times a starting instance asks the arbiter before giving up and degrading.
    /// More than one because an arbiter that has itself restarted declines to answer until it
    /// knows who leads, and that silence is a reason to wait rather than to promote.
    static constexpr int max_startup_arbitration_attempts = 3;
    int startup_arbitration_attempts_{0};

    // Secondary: instance_id of the primary (peer). Fixed at 1 by convention.
    // The pair's fixed identities. Primary is always the lower id -- the arbiter's cold-start
    // preference relies on it -- and neither ever changes for the life of a deployment.
    // Which of them LEADS is a separate question and moves; see design-notes-for-ha.md 11.
    static constexpr int64_t primary_instance_id = 1;
    static constexpr int64_t secondary_instance_id = 2;

    /// The other instance of this pair. Was hard-coded to the primary back when only the
    /// secondary ever asked for arbitration; now that a restarting primary asks too, an
    /// instance that named the primary unconditionally would report itself as its own peer.
    [[nodiscard]] int64_t peer_instance_id() const {
        return config_.instance_id == primary_instance_id ? secondary_instance_id : primary_instance_id;
    }

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
    /**
     * @brief Asks the arbiter which instance leads, rather than assuming it is this one.
     *
     * A primary used to adopt LEADER the moment its first arbiter connection came up. That is
     * harmless on a cold start, where the arbiter would name it anyway, and wrong on a restart:
     * the peer may already have been promoted and be serving, and the venue ends up with two
     * leaders. See docs/bug_list.md, BUG-0042.
     */
    void request_startup_arbitration();

    /**
     * @brief Tells the sequencers which role this instance now holds, and under which epoch.
     *
     * The sequencer used to decide where to send orders by which socket had connected, which
     * was correct only while primary and leader meant the same thing. Announcing the role
     * explicitly is what lets it route to whoever leads. See design-notes-for-ha.md 11b.
     */
    void announce_role();

    /**
     * @brief Starts the recurring message to the arbiters, in whatever role this instance holds.
     *
     * Previously started only on becoming leader, which meant a follower never reached the
     * arbiter at all -- and the arbiter registers a component when it hears from it, so it had
     * no way of knowing a follower was connected. Its cold-start rule asks exactly that.
     */
    void start_arbiter_heartbeats();

    /// The role as a word, for log lines that a person will read after an incident.
    [[nodiscard]] static const char* me_role_name(MeRole role);
    void set_epoch(int32_t new_epoch);
    void publish_book_metrics();
    void handle_peer_role_announcement(const pubsub_itc_fw::EventMessage& message);

    void begin_reconciliation();
    void send_me_position_request();
    void handle_me_position_ack(const pubsub_itc_fw::EventMessage& message);
    void cancel_all_orders_on_failover();
};

} // namespaces
