#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <optional>
#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <leader_follower.hpp>

#include "EngineAbsenceRule.hpp"
#include "EngineRoutedStamp.hpp"
#include "RecoveryLimitRule.hpp"

namespace sequencer {

/**
 * @brief What the sequencer thread supplies so that EnginePresence can tell whether a matching engine takes orders, tell
 *        the gateways whether the venue takes orders, and halt trading when the venue cannot recover by itself.
 */
class EnginePresenceLinksInterface {
  public:
    virtual ~EnginePresenceLinksInterface() = default;

    /// Whether this instance leads.
    [[nodiscard]] virtual bool leading() const = 0;
    /// Whether orders can be sent to an engine now: the order connection goes to an engine known to lead, which does not
    /// follow the WAL and is not being caught up in answer to its own request.
    [[nodiscard]] virtual bool an_engine_takes_orders() const = 0;
    /// Whether the order connection goes to any engine, whether or not that engine is known to lead.
    [[nodiscard]] virtual bool an_engine_is_routed_to() const = 0;
    /// Whether the WAL holds any record.
    [[nodiscard]] virtual bool wal_holds_records() const = 0;
    /// Whether trading is halted, as the WAL says.
    [[nodiscard]] virtual bool trading_halted() const = 0;
    /// Whether the latest halt was written because no engine could be trading within the longest recovery the venue does
    /// by itself. Asked only while trading is halted.
    [[nodiscard]] virtual bool halted_for_a_recovery_too_long() const = 0;

    /// Sends every connected gateway what the venue now says about taking orders (EnginePresence::acceptance).
    virtual void tell_every_gateway() = 0;
    /// The test event EngineTakesOrdersAgain: after @p without_ms milliseconds without an engine, with @p orders_held
    /// orders and cancels held meanwhile; @p one_had_before says whether an engine had taken orders before that time.
    virtual void engine_takes_orders_again(int64_t without_ms, int64_t orders_held, bool one_had_before) = 0;

    /**
     * @brief Writes the commands that halt trading because no engine could be trading within the longest recovery the
     *        venue does by itself: voids for the orders held, a CancelBook, and a TechnicalEvent that halts
     *        (docs/availability/voided_commands.md, section 5.3, R-0117).
     */
    virtual void write_recovery_halt(int64_t now_ns) = 0;
};

/**
 * @brief Whether a matching engine takes orders, what the gateways are told about taking orders, and the limit on the
 *        recovery the venue does by itself (docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.9).
 *
 * **Whether an engine takes orders**, while this instance leads, is one of four situations:
 *
 * | Situation                    | Entered when | Done on entering |
 * |------------------------------|--------------|------------------|
 * | not noted                    | the process starts, or this instance stops leading | the time without an engine and the orders held during it are forgotten,
 * and what the gateways are told goes back to Accepting | | an engine takes orders       | an engine is noted taking orders | if there was a time without one,
 * it is logged with its length and the orders held, EngineTakesOrdersAgain is raised, and what the gateways are told is worked out again | | no engine yet | no
 * engine is noted taking orders, and none has since this instance began leading | the start is noted, and logged at Warning | | no engine since one did      |
 * no engine is noted taking orders, and one did earlier in this leadership | as above |
 *
 * While there is no engine, a Warning is repeated every five seconds, and once there has been none for the longest time
 * orders are held, the gateways are told to refuse new orders. The time is measured on the steady clock, because it is
 * an interval: a change to the wall clock must not change how long the venue believes it has had no engine.
 *
 * **What the gateways are told** is Accepting while this instance does not lead. While it leads:
 *
 * | Halted? | No engine for the longest time orders are held? | Told |
 * |---------|-------------------------------------------------|------|
 * | no      | no  | Accepting |
 * | no      | yes | NoMatchingEngine |
 * | yes, for a recovery that took too long, and no engine takes orders | either | RecoveryTooLong |
 * | yes, otherwise | no  | Halted |
 * | yes, otherwise | yes | NoMatchingEngine |
 *
 * **The limit on the recovery the venue does by itself** measures the absence of an engine differently: it asks whether
 * the order connection goes to any engine, whether or not that engine is known to lead, follows the WAL, or is being
 * caught up. It measures from the first moment none was routed to, or, when the whole venue was down, from the time kept
 * in a small file beside the WAL (EngineRoutedStamp). The two measures answer two questions. The first asks whether an
 * order arriving now can be sent on, and so whether to tell the gateways to refuse orders. The second asks how long the
 * venue has been without any engine that could be made to trade, and so whether it has passed the longest recovery it
 * does by itself, after which trading is halted and the book cancelled.
 *
 * **The engine that said it leads** is the matching engine instance that last announced that it leads, and the
 * connection it announced it on, while that connection is up. A request from the other engine to void a command is
 * declined while it is known.
 *
 * Times on the wall clock are nanoseconds since the Unix epoch, as the WAL records them. One thread, the sequencer's,
 * calls every member function.
 */
class EnginePresence {
  public:
    using Clock = std::chrono::steady_clock;

    /// Whether an engine takes orders, while this instance leads.
    enum class EngineForOrders { not_noted, an_engine_takes_orders, no_engine_yet, no_engine_since_one_did };

    /// How often the venue says out loud that no matching engine takes orders. Once per order would bury the condition
    /// in the Quill log rather than report it (docs/availability/order_acceptance.md).
    static constexpr std::chrono::seconds no_engine_warning_interval{5};

    /**
     * @param[in] orders_held_at_most     How long orders are held without an engine before the gateways are told to refuse
     *                                    new ones.
     * @param[in] automatic_recovery_limit The longest recovery the venue does by itself.
     */
    EnginePresence(std::chrono::seconds orders_held_at_most, std::chrono::seconds automatic_recovery_limit, EnginePresenceLinksInterface& links,
                   pubsub_itc_fw::QuillLogger& logger);

    // ---- Whether an engine takes orders ----------------------------------------------------------------------------

    /**
     * @brief Notes whether a matching engine takes orders, and so when none has since, and says when one does again.
     *
     * Called wherever that can change: on each order held, when an engine connection is lost, on an engine's
     * announcement of its role, when an engine is handed over after a catch-up, and with the absence check, about once a
     * second. Does nothing while this instance does not lead.
     */
    void note_whether_an_engine_takes_orders(Clock::time_point now);

    /// An order or cancel has been held; it is counted if no engine takes orders.
    void order_held();

    [[nodiscard]] EngineForOrders engine_for_orders() const {
        return engine_for_orders_;
    }
    /// How long no matching engine has taken orders, zero while one does or nothing is noted.
    [[nodiscard]] Clock::duration time_without_an_engine(Clock::time_point now) const;
    /// The orders and cancels held while no engine took orders, zero while one does.
    [[nodiscard]] int64_t orders_held_without_an_engine() const;

    // ---- What the gateways are told ---------------------------------------------------------------------------------

    /**
     * @brief Works out again what the gateways are told, and tells every gateway if that has changed.
     *
     * Called when an order arrives, when a gateway connects, when a record that halts or lifts trading is written, and
     * with the absence check, about once a second.
     *
     * @return Whether it changed, in which case every connected gateway has been told.
     */
    bool refresh_order_acceptance(Clock::time_point now);

    /// What the venue is telling its gateways about taking new orders: Accepting, or why not.
    [[nodiscard]] pubsub_itc_fw_app::OrderAcceptanceReason acceptance() const {
        return acceptance_;
    }

    // ---- The limit on the recovery the venue does by itself ---------------------------------------------------------

    /**
     * @brief What this process finds when it starts: the time kept beside the WAL of the last engine routed to.
     * @param[in] wal_directory   Where the file is kept.
     * @param[in] first_record_ns The wall time of the first record in the WAL, or 0 if it is empty: before the first record
     *                            there was nothing to recover.
     */
    void started(const std::string& wal_directory, int64_t first_record_ns, int64_t now_ns);

    /**
     * @brief The absence check, about once a second and when this instance takes the lead.
     *
     * Keeps the time in the file if an engine is routed to; halts trading if no engine could be trading within the
     * longest recovery the venue does by itself; then notes whether an engine takes orders and works out again what the
     * gateways are told.
     */
    void check_absence(int64_t now_ns, Clock::time_point now);

    /// How far the catch-up of a matching engine that asked to be caught up has been sent, on @p connection.
    void catch_up_progress(const pubsub_itc_fw::ConnectionID& connection, RecoveryLimitRule::CatchUpProgress progress);
    /// The engine on @p connection has been handed over, or its connection lost: if its catch-up was being measured, that
    /// ends.
    void catch_up_ended(const pubsub_itc_fw::ConnectionID& connection);

    /// When the present absence of any engine began, or 0 if an engine is routed to or nothing is known yet.
    [[nodiscard]] int64_t absence_began_ns() const {
        return engine_absence_.absence_began_ns();
    }

    // ---- The engine that said it leads ------------------------------------------------------------------------------

    /// Matching engine instance @p instance_id announced on @p connection that it leads, if @p leads, or that it follows.
    void engine_announced(int64_t instance_id, bool leads, const pubsub_itc_fw::ConnectionID& connection);
    /// The instance that said it leads, while the connection it said so on is up; 0 otherwise.
    [[nodiscard]] int64_t leading_engine_instance() const {
        return leading_engine_instance_;
    }
    /// Whether an engine other than @p instance_id is known to lead.
    [[nodiscard]] bool another_engine_leads(int64_t instance_id) const {
        return leading_engine_announced_on_.is_valid() && leading_engine_instance_ != instance_id;
    }

    /**
     * @brief A connection is lost. If it is the one the leading engine announced on, that engine is no longer taken to
     *        lead until it says so again, so that a restarted process is not sent orders on its predecessor's word.
     * @return The instance that is no longer taken to lead, or none if the connection was not that one.
     */
    std::optional<int64_t> connection_lost(const pubsub_itc_fw::ConnectionID& connection);

    // ---- The role of this instance ----------------------------------------------------------------------------------

    /**
     * @brief This instance has stopped leading.
     *
     * Another instance leads, so the venue was not down as a whole, and what this process found when it started no longer
     * says how long it has been without an engine. A follower sends nothing to an engine and holds nothing for one, so the
     * time without an engine is forgotten and what the gateways are told goes back to Accepting. Forgetting it matters if
     * this instance leads again: a time without an engine begun in an earlier leadership would otherwise still be open,
     * and the instance would lead already refusing orders, with a time measured from an absence that ended long ago.
     */
    void stopped_leading(int64_t now_ns);

  private:
    [[nodiscard]] bool no_engine() const {
        return engine_for_orders_ == EngineForOrders::no_engine_yet || engine_for_orders_ == EngineForOrders::no_engine_since_one_did;
    }
    // What the gateways should be told now.
    [[nodiscard]] pubsub_itc_fw_app::OrderAcceptanceReason acceptance_now(Clock::time_point now) const;
    void halt_for_a_recovery_too_long(int64_t now_ns);

    const std::chrono::seconds orders_held_at_most_;
    const std::chrono::seconds automatic_recovery_limit_;
    EnginePresenceLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    EngineForOrders engine_for_orders_{EngineForOrders::not_noted};
    // When the present time without an engine began, while there is one.
    Clock::time_point no_engine_since_{};
    int64_t orders_held_without_an_engine_{0};
    Clock::time_point last_no_engine_warning_{};

    // Starts Accepting: a sequencer that has just started and has not yet failed to send anything on is accepting.
    pubsub_itc_fw_app::OrderAcceptanceReason acceptance_{pubsub_itc_fw_app::OrderAcceptanceReason::Accepting};

    EngineAbsenceRule engine_absence_;
    EngineRoutedStamp engine_routed_stamp_;
    RecoveryLimitRule recovery_limit_;
    // How far the catch-up of an engine that asked to be caught up has been sent, while one is under way, and its
    // connection.
    std::optional<RecoveryLimitRule::CatchUpProgress> engine_catch_up_progress_;
    pubsub_itc_fw::ConnectionID engine_catch_up_connection_{};

    int64_t leading_engine_instance_{0};
    pubsub_itc_fw::ConnectionID leading_engine_announced_on_{};
};

} // namespaces
