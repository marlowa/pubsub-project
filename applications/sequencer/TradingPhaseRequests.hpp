#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <string_view>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <leader_follower.hpp>

#include "TradingHaltState.hpp"
#include "TradingPhaseState.hpp"

namespace sequencer {

/**
 * @brief What the sequencer thread supplies so that TradingPhaseRequests can read the facts it decides from, write a
 *        TechnicalEvent into the WAL, ask the matching engine how far it has got, and answer the one who asked.
 */
class TradingPhaseRequestsLinksInterface {
  public:
    virtual ~TradingPhaseRequestsLinksInterface() = default;

    /// Whether this instance leads, and so writes commands.
    [[nodiscard]] virtual bool leading() const = 0;
    /// The trading day this sequencer was started for, as the number yyyymmdd (its --trading-day argument).
    [[nodiscard]] virtual int32_t trading_day_started_for() const = 0;
    /// The trading day and the phase, as the WAL says.
    [[nodiscard]] virtual const TradingPhaseState& trading_phase() const = 0;
    /// Whether trading is halted, and by which TechnicalEvent, as the WAL says.
    [[nodiscard]] virtual const TradingHaltState& trading_halt() const = 0;
    /// The last record in the WAL; zero when the WAL holds none.
    [[nodiscard]] virtual int64_t last_seq_no() const = 0;
    /// The number the next record written will have.
    [[nodiscard]] virtual int64_t next_seq_no() const = 0;
    /// Whether a matching engine takes orders now.
    [[nodiscard]] virtual bool an_engine_takes_orders() const = 0;
    /// Asks the matching engine orders are routed to for the highest command it has acted on (EnginePositionQuery); its
    /// answer comes to TradingPhaseRequests::engine_position.
    virtual void ask_engine_position() = 0;
    /// Writes @p event into the WAL as a command of the sequencer's own. @return false if it was not written.
    virtual bool write_technical_event(const pubsub_itc_fw_app::TechnicalEvent& event) = 0;
    /// Sends the one on @p connection @p answer.
    virtual void answer(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw_app::TradingPhaseAnswer& answer) = 0;
};

/**
 * @brief Requests to move the venue from one phase of the trading day to the next (docs/venue/trading_day_plan.md and
 *        docs/venue/trading_day_and_reference_data.md, "Moving from one phase to the next").
 *
 * Only the leader acts. Every request is answered from the sequencer's current state, so a request sent again, after an
 * answer was lost or a change of leader, does no harm. The phases are entered one at a time, in order: start of day,
 * pre-open, open, closed and end of day. What is done with each request:
 *
 * - To a sequencer that does not lead: NotLeader, and nothing is written.
 * - For a trading day other than the one this sequencer was started for, or the one the WAL holds: WrongTradingDay, and
 *   nothing is written.
 * - For the phase the venue is in: AlreadyInPhase. For a phase the venue has passed: AlreadyPast. Nothing is written.
 * - For the start of day, on a WAL that holds no record: the TechnicalEvent entering the start of day is written as the
 *   WAL's first record. On a WAL that holds records but no start of day: WrongTradingDay, because the WAL belongs to no
 *   trading day and must be archived.
 * - For any later phase that is not the one after the current phase: NotNext, and nothing is written.
 * - For the next phase, while trading is halted because a matching engine cannot account for the orders the venue holds:
 *   NotReady. A person resolves that halt before the venue moves on. Any other halt does not stop the phase moving on.
 * - For pre-open, until the leading matching engine has said it has acted on the start of day; and for the end of day,
 *   until it has said it has acted on the close: NotReady, and the engine is asked how far it has got. The request is to be
 *   sent again.
 * - Otherwise, the TechnicalEvent entering the phase is written.
 *
 * Every change of phase carries whether trading is halted, so a halt is carried into the next phase. Every request and
 * its answer are logged at Warning with the operator's name.
 *
 * One thread, the sequencer's, calls every member function.
 */
class TradingPhaseRequests {
  public:
    TradingPhaseRequests(TradingPhaseRequestsLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    /// Operator @p operator_name, on @p connection, asks for the venue to enter @p phase on trading day @p trading_day.
    void request(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, int32_t trading_day, pubsub_itc_fw_app::TradingPhase phase);

    /// The matching engine orders are routed to has acted on every command through @p highest_applied.
    void engine_position(int64_t highest_applied);

    /// This instance has stopped leading: what it knew of the engine's position belongs to the engine it routed to.
    void stopped_leading();

    /// The highest command the leading engine has said it acted on, since this instance last started leading.
    [[nodiscard]] int64_t engine_applied_through() const {
        return engine_applied_through_;
    }

  private:
    void answer(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, pubsub_itc_fw_app::TradingPhaseOutcome outcome,
                int64_t technical_event_seq_no, const std::string& explanation);
    void start_of_day(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, int32_t trading_day);
    // Enters @p phase, any phase after the start of day, if it is the next one and its conditions hold.
    void move_on(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, pubsub_itc_fw_app::TradingPhase phase);
    // Writes the TechnicalEvent entering @p phase, carrying whether trading is halted. @return its position, or zero if it
    // was not written.
    int64_t write_phase(pubsub_itc_fw_app::TradingPhase phase, int32_t trading_day);

    TradingPhaseRequestsLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;
    int64_t engine_applied_through_{0};
};

} // namespaces
