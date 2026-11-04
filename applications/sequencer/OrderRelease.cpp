// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "OrderRelease.hpp"

#include <algorithm>
#include <cstring>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace sequencer {

OrderRelease::OrderRelease(size_t capacity, OrderReleaseLinksInterface& links, pubsub_itc_fw::QuillLogger& logger)
    : capacity_(capacity), links_(links), logger_(logger), held_(capacity) {}

// ---- Holding --------------------------------------------------------------------------------------------------------

void OrderRelease::hold(const pubsub_itc_fw_app::WalRecord& envelope, pubsub_itc_fw::SlabHandle slab_id, const uint8_t* buffer) {
    // Once one order waits in the WAL, every later one does too, so that orders are sent in order.
    if (first_unheld_seq_ == 0 && held_.push_back(HeldOrder{envelope, slab_id, buffer, std::chrono::steady_clock::now()})) {
        return;
    }
    if (first_unheld_seq_ == 0) {
        first_unheld_seq_ = envelope.seq_no;
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "OrderRelease: the storage for waiting orders is full -- orders from seq={} wait in the write-ahead log, and are sent to the matching "
                   "engine from there when they are released",
                   envelope.seq_no);
        links_.held_orders_full(envelope.seq_no);
    }
    links_.release_message(slab_id, buffer);
}

void OrderRelease::hold_own_command(const pubsub_itc_fw_app::WalRecord& envelope, const uint8_t* bytes, size_t size) {
    HeldOrder held{envelope, pubsub_itc_fw::SlabHandle{}, nullptr, std::chrono::steady_clock::now()};
    std::memcpy(held.own_payload.data(), bytes, size);
    held.own_payload_size = size;
    held.envelope.payload = pubsub_itc_fw_app::BytesView{};
    if (first_unheld_seq_ != 0 || !held_.push_back(held)) {
        // It waits in the WAL, as an order does once the storage for held orders is full, and is sent from there.
        if (first_unheld_seq_ == 0) {
            first_unheld_seq_ = envelope.seq_no;
        }
    }
}

void OrderRelease::release_to_leading_engine(int64_t committed_through) {
    // While waiting for the engine's position, or while no engine takes orders, every order stays held, so that no
    // engine applies an order before the engine that leads is sent it.
    if (awaiting_engine_position_ || !links_.an_engine_takes_orders()) {
        links_.released_position_moved(released_through());
        pause_or_resume_reading();
        return;
    }
    while (!held_.empty() && held_.front().envelope.seq_no <= committed_through) {
        const HeldOrder& held = held_.front();
        links_.send_to_leading_engine(held);
        if (held.buffer != nullptr) {
            links_.release_message(held.slab_id, held.buffer);
        }
        held_.pop_front();
    }
    if (first_unheld_seq_ != 0 && first_unheld_seq_ <= committed_through) {
        links_.send_orders_that_waited_in_the_wal(first_unheld_seq_);
        first_unheld_seq_ = committed_through >= links_.last_seq_no() ? 0 : committed_through + 1;
    }
    links_.released_position_moved(released_through());
    pause_or_resume_reading();
}

int64_t OrderRelease::released_through() const {
    const int64_t committed = links_.committed_through();
    if (!held_.empty()) {
        return std::min(committed, held_.front().envelope.seq_no - 1);
    }
    if (first_unheld_seq_ != 0) {
        return std::min(committed, first_unheld_seq_ - 1);
    }
    return std::min(committed, links_.last_seq_no());
}

bool OrderRelease::held_orders_wait_on_the_follower() const {
    // Orders held because no engine leads are committed, and say nothing about the follower.
    if (!links_.waits_for_follower() || (!held_.full() && first_unheld_seq_ == 0)) {
        return false;
    }
    const int64_t oldest = held_.empty() ? first_unheld_seq_ : held_.front().envelope.seq_no;
    return oldest > links_.committed_through();
}

void OrderRelease::stopped_leading() {
    awaiting_engine_position_ = false;
    // The orders are in this instance's WAL and were never acted on or answered; the new leader now decides what the
    // matching engine sees.
    if (!held_.empty()) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "OrderRelease: stopped leading while holding {} order(s) the follower had not acknowledged -- they were not sent to the matching engine "
                   "and are discarded",
                   held_.size());
    }
    while (!held_.empty()) {
        if (held_.front().buffer != nullptr) {
            links_.release_message(held_.front().slab_id, held_.front().buffer);
        }
        held_.pop_front();
    }
    first_unheld_seq_ = 0;
    pause_or_resume_reading();
}

// ---- Reading from the gateways --------------------------------------------------------------------------------------

void OrderRelease::order_connection_established(const pubsub_itc_fw::ConnectionID& connection) {
    order_connections_.push_back(connection);
    if (reading_ != Reading::reading) {
        links_.pause_reading(connection);
    }
}

bool OrderRelease::order_connection_lost(const pubsub_itc_fw::ConnectionID& connection) {
    const auto found = std::find(order_connections_.begin(), order_connections_.end(), connection);
    if (found == order_connections_.end()) {
        return false;
    }
    order_connections_.erase(found);
    return true;
}

void OrderRelease::pause_or_resume_reading() {
    if (reading_ == Reading::paused_for_hand_over) {
        return; // stays paused until the follower is handed over (end_pause_for_hand_over)
    }
    const bool waiting_for_confirmation = links_.leading() && (links_.waits_for_confirmation() || awaiting_engine_position_);
    if (reading_ == Reading::reading) {
        if (waiting_for_confirmation && (held_.size() >= pause_at() || first_unheld_seq_ != 0)) {
            for (const pubsub_itc_fw::ConnectionID& connection : order_connections_) {
                links_.pause_reading(connection);
            }
            reading_ = Reading::paused_for_held_orders;
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "OrderRelease: {} orders wait for a voter to confirm that the follower may not lead -- reading from the {} gateway connection(s) is "
                       "paused until they are released",
                       held_.size(), order_connections_.size());
        }
        return;
    }
    if (waiting_for_confirmation && (held_.size() > resume_at() || first_unheld_seq_ != 0)) {
        return;
    }
    for (const pubsub_itc_fw::ConnectionID& connection : order_connections_) {
        links_.resume_reading(connection);
    }
    reading_ = Reading::reading;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "OrderRelease: reading from the gateways resumed -- {} order(s) still held", held_.size());
}

void OrderRelease::pause_for_hand_over() {
    if (reading_ == Reading::paused_for_hand_over) {
        return;
    }
    const bool was_reading = reading_ == Reading::reading;
    reading_ = Reading::paused_for_hand_over;
    if (was_reading) {
        for (const pubsub_itc_fw::ConnectionID& connection : order_connections_) {
            links_.pause_reading(connection);
        }
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "OrderRelease: reading from the {} gateway connection(s) is paused until the follower the reading thread is catching up is handed over to "
               "new records -- orders were sequenced faster than one round between this thread and the reading thread",
               order_connections_.size());
}

void OrderRelease::end_pause_for_hand_over() {
    if (reading_ != Reading::paused_for_hand_over) {
        return;
    }
    // Paused for held orders until pause_or_resume_reading says otherwise, which resumes reading if nothing keeps it
    // paused.
    reading_ = Reading::paused_for_held_orders;
    pause_or_resume_reading();
}

// ---- The wait after taking the lead ---------------------------------------------------------------------------------

void OrderRelease::ask_engine_for_position() {
    const pubsub_itc_fw::ConnectionID engine = links_.leading_engine_connection();
    if (!engine.is_valid()) {
        // No engine to ask. Whichever engine acts next catches up from this WAL before it acts, and is sent everything
        // after its own position, the orders asked about here included.
        if (awaiting_engine_position_) {
            stop_awaiting_engine_position("no matching engine is connected; whichever engine acts next catches up from this log");
        }
        return;
    }
    if (!awaiting_engine_position_) {
        awaiting_engine_position_ = true;
        held_at_takeover_ = links_.last_seq_no();
        engine_position_asks_ = 0;
    }
    ++engine_position_request_id_;
    ++engine_position_asks_;
    engine_position_asked_at_ = std::chrono::steady_clock::now();
    links_.ask_engine_position(engine, engine_position_request_id_);
    // A few asks are routine: when the venue starts, the engine is still catching up as the first leader takes the lead,
    // and answers only once it has finished, a second or two later. Five asks means it has not answered for four seconds
    // while new orders wait, which is worth a Warning, once.
    PUBSUB_LOG(logger_, engine_position_asks_ == engine_position_asks_before_warning ? pubsub_itc_fw::FwLogLevel::Warning : pubsub_itc_fw::FwLogLevel::Info,
               "OrderRelease: asking the matching engine on connection {} for the highest order it has acted on (ask {}, request {}) -- this log holds "
               "records through {}, and new orders wait until the engine has every one it lacks",
               engine.get_value(), engine_position_asks_, engine_position_request_id_, held_at_takeover_);
}

void OrderRelease::tick(std::chrono::steady_clock::time_point now) {
    if (awaiting_engine_position_ && now - engine_position_asked_at_ >= engine_position_ask_interval) {
        ask_engine_for_position();
    }
}

void OrderRelease::engine_position(int64_t request_id, int64_t highest_applied) {
    if (!awaiting_engine_position_ || !links_.leading() || request_id != engine_position_request_id_) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "OrderRelease: EnginePosition for request {} not awaited (awaiting request {}) -- ignoring",
                   request_id, awaiting_engine_position_ ? engine_position_request_id_ : 0);
        return;
    }
    awaiting_engine_position_ = false;
    if (highest_applied < held_at_takeover_) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "OrderRelease: the matching engine has acted on orders through {} -- sending it the orders this log holds from {} to {}, which it never "
                   "received",
                   highest_applied, highest_applied + 1, held_at_takeover_);
        if (links_.an_engine_takes_orders()) {
            // The reading thread sends them, followed by the orders that waited for the engine's answer, which are
            // released below and are at or below the limit it serves up to.
            links_.send_engine_orders_after(highest_applied);
        } else {
            // The engine that next leads is caught up through them before it leads.
            PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Info,
                           "OrderRelease: no matching engine leads now -- the one that next leads is caught up through them first");
        }
    } else {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "OrderRelease: the matching engine has acted on orders through {}, and this log holds nothing after that it has not been sent",
                   highest_applied);
    }
    release_orders_that_waited_for_the_engine();
}

void OrderRelease::stop_awaiting_engine_position(const char* reason) {
    awaiting_engine_position_ = false;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning, "OrderRelease: no longer waiting for the matching engine's position -- {}", reason);
    release_orders_that_waited_for_the_engine();
}

void OrderRelease::release_orders_that_waited_for_the_engine() {
    // The orders that arrived meanwhile now go the way they would have gone: once their records are committed, which may
    // already be the case.
    links_.release_through(links_.committed_through());
}

} // namespaces
