#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FixedCapacityRingBuffer.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/SlabHandle.hpp>

#include <leader_follower.hpp>

namespace sequencer {

/**
 * @brief An order, or a command of the sequencer's own, held until it may be sent to the leading matching engine.
 *
 * A member's order keeps the inbound message's own buffer, which the envelope's payload points into, and releases it once
 * the order has been sent, so holding an order copies nothing and allocates nothing. A command of the sequencer's own is
 * held with its payload, which no inbound message holds.
 */
struct HeldOrder {
    pubsub_itc_fw_app::WalRecord envelope;
    pubsub_itc_fw::SlabHandle slab_id;
    /// The inbound message's bytes the envelope's payload points into, released once it is sent; null for a command of the
    /// sequencer's own.
    const uint8_t* buffer{nullptr};
    std::chrono::steady_clock::time_point held_at;
    static constexpr size_t own_payload_capacity = 64;
    std::array<uint8_t, own_payload_capacity> own_payload{};
    size_t own_payload_size{0};
};

/**
 * @brief What the sequencer thread supplies so that OrderRelease can read the WAL's positions, send orders, and pause
 *        reading from the gateways.
 */
class OrderReleaseLinksInterface {
  public:
    virtual ~OrderReleaseLinksInterface() = default;

    [[nodiscard]] virtual int64_t committed_through() const = 0;
    [[nodiscard]] virtual int64_t last_seq_no() const = 0;
    /// Whether the leader waits for a voter's confirmation before it commits: its follower's acknowledgement, or a
    /// voter's agreement that the follower may not lead.
    [[nodiscard]] virtual bool waits_for_confirmation() const = 0;
    /// Whether the leader waits for its follower's acknowledgement before it commits.
    [[nodiscard]] virtual bool waits_for_follower() const = 0;
    [[nodiscard]] virtual bool leading() const = 0;
    /// Whether an engine leads that orders can be sent to.
    [[nodiscard]] virtual bool an_engine_takes_orders() const = 0;
    /// The connection orders are routed on, invalid when no engine is routed to.
    [[nodiscard]] virtual pubsub_itc_fw::ConnectionID leading_engine_connection() const = 0;

    /// Sends @p held to the leading engine, unless the reading thread is sending it what it lacks.
    virtual void send_to_leading_engine(const HeldOrder& held) = 0;
    /// Has the reading thread send the leading engine, in order, the orders numbered from @p first that waited in the WAL.
    virtual void send_orders_that_waited_in_the_wal(int64_t first) = 0;
    /// Releases the held orders committed through @p committed_through to the leading engine, and then sends the engines
    /// that follow the WAL every record up to the position released.
    virtual void release_through(int64_t committed_through) = 0;
    /// Has the reading thread send the leading engine every order after @p after, which it never received.
    virtual void send_engine_orders_after(int64_t after) = 0;
    /// Releases an inbound message's buffer.
    virtual void release_message(pubsub_itc_fw::SlabHandle slab_id, const uint8_t* buffer) = 0;
    virtual void pause_reading(const pubsub_itc_fw::ConnectionID& connection) = 0;
    virtual void resume_reading(const pubsub_itc_fw::ConnectionID& connection) = 0;
    /// The last order released has moved to @p released_through, which the reading thread serves up to.
    virtual void released_position_moved(int64_t released_through) = 0;
    /// Asks the engine on @p connection for the highest order it has acted on (EnginePositionQuery).
    virtual void ask_engine_position(const pubsub_itc_fw::ConnectionID& connection, int64_t request_id) = 0;
    /// The test event HeldOrdersFull.
    virtual void held_orders_full(int64_t first_seq_no) = 0;
};

/**
 * @brief Holding each order until it may be sent to the leading matching engine, and sending it
 *        (docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.8).
 *
 * **An order.** The leader writes each order into the WAL and holds it here. When the storage is full the order is not
 * held: the first such order is noted, and no later order is held either, so that orders are sent in order; those that
 * waited in the WAL are sent by the reading thread. Held orders are sent to the leading engine, oldest first, once their
 * records are committed, unless the leader is waiting for the engine's position or no engine takes orders. The last
 * order released (released_through) is the lower of the last record committed and the record before the oldest order
 * still waiting.
 *
 * **Reading from the gateways** is in one of three situations:
 *
 * | Situation                | Entered when | Left when |
 * |--------------------------|--------------|-----------|
 * | reading                  |              |           |
 * | paused for held orders   | leading, waiting for a confirmation or for the engine's position, and three quarters of the storage is used or an order waited
 * in the WAL | the storage falls to a quarter and no order waits in the WAL, or the leader no longer waits for either | | paused for a hand-over   | a
 * follower's hand-over cannot finish because records are released faster than one round between the two threads | the hand-over finishes, or this instance
 * stops leading; reading then resumes unless held orders keep it paused |
 *
 * **The wait after taking the lead.** With high availability on, a new leader asks the leading engine for the highest
 * order it has acted on, and holds new orders until it answers, so that the engine is sent the orders this WAL holds
 * and it never received before any later one (docs/availability/commands_during_a_change_of_leader.md, section 3.5).
 * It asks again every second, saying so at Warning on the fifth ask, and stops waiting if no engine is connected.
 *
 * One thread, the sequencer's, calls every member function.
 */
class OrderRelease {
  public:
    /// The reading situation.
    enum class Reading { reading, paused_for_held_orders, paused_for_hand_over };

    static constexpr std::chrono::milliseconds engine_position_ask_interval{1000};
    static constexpr int engine_position_asks_before_warning = 5;

    /// @param[in] capacity The most orders held at once, allocated now.
    OrderRelease(size_t capacity, OrderReleaseLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    // ---- Holding -------------------------------------------------------------------------------------------------

    /// Holds a member's order, whose envelope points into the inbound message's buffer; if it is not held, the buffer
    /// is released at once.
    void hold(const pubsub_itc_fw_app::WalRecord& envelope, pubsub_itc_fw::SlabHandle slab_id, const uint8_t* buffer);

    /// Holds a command of the sequencer's own, @p size bytes at @p bytes, at most HeldOrder::own_payload_capacity.
    void hold_own_command(const pubsub_itc_fw_app::WalRecord& envelope, const uint8_t* bytes, size_t size);

    /// Sends the leading engine every held order whose record is committed through @p committed_through, unless the
    /// leader waits for the engine's position or no engine takes orders; then publishes the position released and
    /// pauses or resumes reading.
    void release_to_leading_engine(int64_t committed_through);

    /// The last record released to the engines: every order at or below it has been sent to the leading engine, and
    /// every record at or below it is committed.
    [[nodiscard]] int64_t released_through() const;

    /// Whether the storage is full of orders the follower has not acknowledged, so that it is too far behind.
    [[nodiscard]] bool held_orders_wait_on_the_follower() const;

    /// This instance has stopped leading: every held order is discarded, the wait for the engine's position ends, and
    /// reading resumes if it can.
    void stopped_leading();

    [[nodiscard]] size_t held() const {
        return held_.size();
    }
    [[nodiscard]] int64_t first_unheld() const {
        return first_unheld_seq_;
    }

    // ---- Reading from the gateways -------------------------------------------------------------------------------

    [[nodiscard]] Reading reading() const {
        return reading_;
    }
    /// A gateway's connection for orders is established; it is paused too while reading is paused.
    void order_connection_established(const pubsub_itc_fw::ConnectionID& connection);
    /// A gateway's connection for orders is lost. @return Whether it was one.
    bool order_connection_lost(const pubsub_itc_fw::ConnectionID& connection);
    /// Pauses reading while too many orders wait, and resumes it when they have drained.
    void pause_or_resume_reading();
    /// Pauses reading until a follower's hand-over finishes.
    void pause_for_hand_over();
    /// The hand-over has finished, or will not: reading resumes unless held orders keep it paused.
    void end_pause_for_hand_over();

    // ---- The wait after taking the lead --------------------------------------------------------------------------

    [[nodiscard]] bool awaiting_engine_position() const {
        return awaiting_engine_position_;
    }
    /// Asks the leading engine for the highest order it has acted on, or stops waiting if no engine is connected.
    void ask_engine_for_position();
    /// A tick, at @p now: asks again if an answer is overdue.
    void tick(std::chrono::steady_clock::time_point now);
    /// The engine's answer to request @p request_id: it has acted on every order through @p highest_applied.
    void engine_position(int64_t request_id, int64_t highest_applied);
    /// The engine asked is gone, or there is none: no more is waited for.
    void stop_awaiting_engine_position(const char* reason);

  private:
    [[nodiscard]] size_t pause_at() const {
        return capacity_ * 3 / 4;
    }
    [[nodiscard]] size_t resume_at() const {
        return capacity_ / 4;
    }
    void release_orders_that_waited_for_the_engine();

    const size_t capacity_;
    OrderReleaseLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    pubsub_itc_fw::FixedCapacityRingBuffer<HeldOrder> held_;
    // Orders numbered from here on were written to the WAL and replicated but not held, because the storage was full;
    // zero when there are none. While this is set, no later order is held either.
    int64_t first_unheld_seq_{0};

    std::vector<pubsub_itc_fw::ConnectionID> order_connections_;
    Reading reading_{Reading::reading};

    bool awaiting_engine_position_{false};
    // The highest record this instance held when it took the lead: the orders the engine may lack.
    int64_t held_at_takeover_{0};
    int64_t engine_position_request_id_{0};
    int engine_position_asks_{0};
    std::chrono::steady_clock::time_point engine_position_asked_at_{};
};

} // namespaces
