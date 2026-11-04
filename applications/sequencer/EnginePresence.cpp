// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "EnginePresence.hpp"

#include <fmt/format.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace sequencer {

namespace {

constexpr int64_t nanoseconds_per_second = 1000000000;

} // namespaces

EnginePresence::EnginePresence(std::chrono::seconds orders_held_at_most, std::chrono::seconds automatic_recovery_limit, EnginePresenceLinksInterface& links,
                               pubsub_itc_fw::QuillLogger& logger)
    : orders_held_at_most_(orders_held_at_most)
    , automatic_recovery_limit_(automatic_recovery_limit)
    , links_(links)
    , logger_(logger)
    , recovery_limit_(automatic_recovery_limit) {}

// ---- Whether an engine takes orders --------------------------------------------------------------------------------

void EnginePresence::note_whether_an_engine_takes_orders(Clock::time_point now) {
    if (!links_.leading()) {
        return;
    }
    const bool takes_orders = links_.an_engine_takes_orders();
    if (takes_orders && !no_engine()) {
        engine_for_orders_ = EngineForOrders::an_engine_takes_orders;
        return; // the usual case, on every order
    }
    if (!takes_orders) {
        if (!no_engine()) {
            engine_for_orders_ =
                engine_for_orders_ == EngineForOrders::an_engine_takes_orders ? EngineForOrders::no_engine_since_one_did : EngineForOrders::no_engine_yet;
            no_engine_since_ = now;
            orders_held_without_an_engine_ = 0;
            last_no_engine_warning_ = now;
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "EnginePresence: no matching engine takes orders -- every order and cancel is held, in the WAL, until one leads, and is then "
                       "released to it. After {}s without one, the gateways are told to refuse new orders",
                       orders_held_at_most_.count());
            return;
        }
        // Worked out each time, because the limit must take effect when it is passed, not at the next warning. Only the
        // warning is limited in how often it is given.
        const bool changed = refresh_order_acceptance(now);
        if (now - last_no_engine_warning_ < no_engine_warning_interval) {
            return;
        }
        last_no_engine_warning_ = now;
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "EnginePresence: still no matching engine takes orders after {}s -- {} order(s) and cancel(s) held so far",
                   std::chrono::duration_cast<std::chrono::seconds>(time_without_an_engine(now)).count(), orders_held_without_an_engine_);
        // Said again on the same interval while the venue is not accepting, so that a gateway that missed the change, or
        // connected without one since, is told.
        if (!changed && acceptance_ != pubsub_itc_fw_app::OrderAcceptanceReason::Accepting) {
            links_.tell_every_gateway();
        }
        return;
    }
    const auto without = std::chrono::duration_cast<std::chrono::milliseconds>(time_without_an_engine(now));
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "EnginePresence: a matching engine takes orders again after {} ms without one -- the {} order(s) and cancel(s) held meanwhile are "
               "released to it",
               without.count(), orders_held_without_an_engine_);
    links_.engine_takes_orders_again(without.count(), orders_held_without_an_engine_, engine_for_orders_ == EngineForOrders::no_engine_since_one_did);
    engine_for_orders_ = EngineForOrders::an_engine_takes_orders;
    orders_held_without_an_engine_ = 0;
    // Accepting again is automatic, and deliberately so: a recovery that waits for a person to re-enable acceptance
    // depends on someone watching, and the case it recovers from is one where nobody was. See
    // docs/availability/order_acceptance.md.
    refresh_order_acceptance(now);
}

void EnginePresence::order_held() {
    if (no_engine()) {
        ++orders_held_without_an_engine_;
    }
}

EnginePresence::Clock::duration EnginePresence::time_without_an_engine(Clock::time_point now) const {
    return no_engine() ? now - no_engine_since_ : Clock::duration::zero();
}

int64_t EnginePresence::orders_held_without_an_engine() const {
    return no_engine() ? orders_held_without_an_engine_ : 0;
}

// ---- What the gateways are told -------------------------------------------------------------------------------------

pubsub_itc_fw_app::OrderAcceptanceReason EnginePresence::acceptance_now(Clock::time_point now) const {
    // Only the leader has an opinion worth sending. A follower sends nothing to a matching engine, and the gateway holds a
    // connection to both sequencers and cannot tell which of them leads.
    if (!links_.leading()) {
        return pubsub_itc_fw_app::OrderAcceptanceReason::Accepting;
    }
    const bool held_too_long = time_without_an_engine(now) >= orders_held_at_most_;
    if (links_.trading_halted()) {
        // Halted because no engine could be trading within the longest recovery the venue does by itself, and still no
        // engine trades: new orders and cancels are both refused, with that reason.
        if (links_.halted_for_a_recovery_too_long() && !links_.an_engine_takes_orders()) {
            return pubsub_itc_fw_app::OrderAcceptanceReason::RecoveryTooLong;
        }
        // Otherwise new orders are refused and cancels passed on, because the engine carries them out while trading is
        // halted, unless no engine has led for longer than orders are held.
        return held_too_long ? pubsub_itc_fw_app::OrderAcceptanceReason::NoMatchingEngine : pubsub_itc_fw_app::OrderAcceptanceReason::Halted;
    }
    return held_too_long ? pubsub_itc_fw_app::OrderAcceptanceReason::NoMatchingEngine : pubsub_itc_fw_app::OrderAcceptanceReason::Accepting;
}

bool EnginePresence::refresh_order_acceptance(Clock::time_point now) {
    const pubsub_itc_fw_app::OrderAcceptanceReason reason = acceptance_now(now);
    if (reason == acceptance_) {
        return false;
    }
    acceptance_ = reason;
    switch (reason) {
        case pubsub_itc_fw_app::OrderAcceptanceReason::Accepting:
            PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Info,
                           "EnginePresence: accepting orders again -- gateways told the venue can process what they are given");
            break;
        case pubsub_itc_fw_app::OrderAcceptanceReason::RecoveryTooLong:
            PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                           "EnginePresence: no longer accepting orders or cancels -- trading is halted because no matching engine could be trading within "
                           "the longest recovery the venue does by itself, and none trades yet");
            break;
        case pubsub_itc_fw_app::OrderAcceptanceReason::Halted:
            PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                           "EnginePresence: trading is halted -- gateways told to refuse new orders and to pass cancels on, which the matching engine "
                           "carries out while halted, until a person lifts the halt");
            break;
        default:
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "EnginePresence: no longer accepting orders -- no matching engine has led for {}s, the longest orders are held ({} order(s) and "
                       "cancel(s) held). The orders held stay held, and are released to the engine that next leads; what stops is taking on new ones",
                       std::chrono::duration_cast<std::chrono::seconds>(time_without_an_engine(now)).count(), orders_held_without_an_engine_);
            break;
    }
    links_.tell_every_gateway();
    return true;
}

// ---- The limit on the recovery the venue does by itself -------------------------------------------------------------

void EnginePresence::started(const std::string& wal_directory, int64_t first_record_ns, int64_t now_ns) {
    if (!engine_routed_stamp_.open(wal_directory)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "EnginePresence: cannot open {}/{} -- after the whole venue was down, how long it was without a matching engine is measured from "
                   "the first record in the WAL, which may be longer than it was",
                   wal_directory, EngineRoutedStamp::file_name);
    }
    engine_absence_.started(engine_routed_stamp_.stamped_at_ns(), first_record_ns);
    if (engine_absence_.absence_began_ns() != 0) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "EnginePresence: this process last knew of a matching engine it routed orders to {} s ago; if this instance leads before one is routed "
                   "to and that is longer than the {} s the venue recovers by itself, it halts trading and cancels the book (R-0117)",
                   (now_ns - engine_absence_.absence_began_ns()) / nanoseconds_per_second, automatic_recovery_limit_.count());
    }
}

void EnginePresence::check_absence(int64_t now_ns, Clock::time_point now) {
    const bool engine_routed = links_.an_engine_is_routed_to();
    if (engine_routed) {
        static_cast<void>(engine_routed_stamp_.write(now_ns));
    }
    engine_absence_.observe(engine_routed, now_ns);
    if (recovery_limit_.halt_due(now_ns, engine_absence_.absence_began_ns(), links_.leading(), links_.wal_holds_records(), engine_catch_up_progress_)) {
        halt_for_a_recovery_too_long(now_ns);
    }
    // Whether an engine takes orders, and how long none has: once none has for the longest time orders are held, the
    // gateways are told to refuse new orders, whether or not any order arrives meanwhile.
    note_whether_an_engine_takes_orders(now);
    // Trading stays halted when an engine trades again, until a person lifts the halt; what changes is the reason the
    // gateways are given, and with it that they pass cancels on (docs/availability/voided_commands.md, section 6.1).
    refresh_order_acceptance(now);
}

void EnginePresence::halt_for_a_recovery_too_long(int64_t now_ns) {
    const std::optional<int64_t> estimate_ns = recovery_limit_.estimate_ns();
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "EnginePresence: no matching engine has been able to trade for {} s, and {} -- longer together than the {} s the venue recovers by "
               "itself. Voiding the orders held, cancelling every order on the book and halting trading, and telling the gateways to refuse orders; "
               "a person must lift the halt (R-0117, R-0023)",
               (now_ns - engine_absence_.absence_began_ns()) / nanoseconds_per_second,
               estimate_ns.has_value() ? fmt::format("the catch-up under way will take about {} s more", *estimate_ns / nanoseconds_per_second)
                                       : std::string("no catch-up under way says when one will finish"),
               automatic_recovery_limit_.count());
    recovery_limit_.halt_written();
    links_.write_recovery_halt(now_ns);
}

void EnginePresence::catch_up_progress(const pubsub_itc_fw::ConnectionID& connection, RecoveryLimitRule::CatchUpProgress progress) {
    engine_catch_up_connection_ = connection;
    engine_catch_up_progress_ = progress;
}

void EnginePresence::catch_up_ended(const pubsub_itc_fw::ConnectionID& connection) {
    if (connection == engine_catch_up_connection_) {
        engine_catch_up_connection_ = pubsub_itc_fw::ConnectionID{};
        engine_catch_up_progress_.reset();
    }
}

// ---- The engine that said it leads ----------------------------------------------------------------------------------

void EnginePresence::engine_announced(int64_t instance_id, bool leads, const pubsub_itc_fw::ConnectionID& connection) {
    if (leads) {
        leading_engine_instance_ = instance_id;
        leading_engine_announced_on_ = connection;
    } else if (instance_id == leading_engine_instance_) {
        leading_engine_instance_ = 0;
        leading_engine_announced_on_ = pubsub_itc_fw::ConnectionID{};
    }
}

std::optional<int64_t> EnginePresence::connection_lost(const pubsub_itc_fw::ConnectionID& connection) {
    if (!leading_engine_announced_on_.is_valid() || !(connection == leading_engine_announced_on_)) {
        return std::nullopt;
    }
    const int64_t instance_id = leading_engine_instance_;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "EnginePresence: the connection matching engine instance {} announced that it leads on is lost -- it is not taken to lead until it says "
               "so again",
               instance_id);
    leading_engine_instance_ = 0;
    leading_engine_announced_on_ = pubsub_itc_fw::ConnectionID{};
    return instance_id;
}

// ---- The role of this instance --------------------------------------------------------------------------------------

void EnginePresence::stopped_leading(int64_t now_ns) {
    engine_absence_.another_instance_leads(now_ns);
    engine_for_orders_ = EngineForOrders::not_noted;
    orders_held_without_an_engine_ = 0;
    acceptance_ = pubsub_itc_fw_app::OrderAcceptanceReason::Accepting;
}

} // namespaces
