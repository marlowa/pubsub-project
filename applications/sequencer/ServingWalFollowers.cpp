// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ServingWalFollowers.hpp"

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

#include "PublisherRecordFromWal.hpp"

namespace sequencer {

namespace {

// The arena each converter of records for publishers decodes into.
constexpr size_t publisher_converter_arena_bytes = 64 * 1024;

} // namespaces

ServingWalFollowers::ServingWalFollowers(Converters converters, ServingWalFollowersLinksInterface& links, pubsub_itc_fw::QuillLogger& logger)
    : converters_(converters), links_(links), logger_(logger) {}

void ServingWalFollowers::start(const pubsub_itc_fw::Wal& wal, const std::string& wal_directory, const pubsub_itc_fw::WalFollowingPositions& positions,
                                pubsub_itc_fw::WalHandedOverFollowersLinksInterface& publisher_links, pubsub_itc_fw::WalKeptRecordsCapacity kept) {
    handed_over_publishers_.emplace(wal, wal_directory, make_publisher_converter(publisher_converter_arena_bytes), publisher_links, kept);
    // A follower engine is sent every record as the WAL holds it, as a publisher is.
    handed_over_engines_.emplace(wal, wal_directory, make_publisher_converter(publisher_converter_arena_bytes), publisher_links, kept);
    // Every follower of the WAL is caught up by the reading thread, with high availability on or off.
    service_.emplace(logger_, wal, positions, links_, *this);
}

// ---- Events from the thread ----------------------------------------------------------------------------------------

bool ServingWalFollowers::on_inter_thread_message(const pubsub_itc_fw::EventMessage& message) {
    return service_.has_value() && service_->on_inter_thread_message(message);
}

void ServingWalFollowers::written(int64_t seq_no, int16_t pdu_id, const uint8_t* bytes, size_t size) {
    // A copy for the publishers and the engines that follow the WAL, so that they are sent it without its being read back
    // from the WAL.
    if (handed_over_publishers_.has_value()) {
        handed_over_publishers_->written(seq_no, pdu_id, bytes, size);
    }
    if (handed_over_engines_.has_value()) {
        handed_over_engines_->written(seq_no, pdu_id, bytes, size);
    }
}

void ServingWalFollowers::engine_connection_lost(const pubsub_itc_fw::ConnectionID& connection) {
    if (service_.has_value()) {
        service_->on_connection_lost(connection);
    }
    engine_servings_.erase(connection.get_value());
    if (handed_over_engines_.has_value()) {
        handed_over_engines_->remove(connection);
    }
}

void ServingWalFollowers::publisher_connection_lost(const pubsub_itc_fw::ConnectionID& connection) {
    if (service_.has_value()) {
        service_->on_connection_lost(connection);
    }
    handed_over_publishers_->remove(connection);
}

void ServingWalFollowers::became_leader(int32_t epoch) {
    if (service_.has_value()) {
        service_->on_became_leader(epoch);
    }
}

void ServingWalFollowers::stopped_leading() {
    // Every follower being served stops, with the reading thread's confirmation, so that a WAL about to be cut back is not
    // being read; each asks again of whichever instance leads next.
    if (service_.has_value()) {
        service_->stop_all();
        engine_servings_.clear();
        links_.end_pause_for_hand_over();
    }
    // A publisher handed over is sent nothing more from here either; it asks again when told of the new leader.
    if (handed_over_publishers_.has_value()) {
        handed_over_publishers_->remove_all();
    }
    // So is a follower engine, which also asks again when told of the new leader.
    if (handed_over_engines_.has_value()) {
        handed_over_engines_->remove_all();
    }
}

// ---- Requests from followers ---------------------------------------------------------------------------------------

void ServingWalFollowers::publisher_asks(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowRequestView& request,
                                         pubsub_itc_fw::Leading leading) {
    // Whatever this connection was sent before stops: the publisher has asked afresh, from the last record it holds.
    handed_over_publishers_->remove(connection);
    // The reading thread sends it every record after its position, up to the last committed, and the service hands it
    // over (follower_handed_over), after which it is sent each record from here, as it is committed.
    service_->on_follow_request(connection, request, leading, pubsub_itc_fw::FollowerLimit::LastCommitted, converters_.publisher, 0);
}

void ServingWalFollowers::engine_asks_afresh(const pubsub_itc_fw::ConnectionID& connection) {
    if (handed_over_engines_.has_value()) {
        handed_over_engines_->remove(connection);
    }
}

void ServingWalFollowers::engine_asks_while_not_leading(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowRequestView& request) {
    service_->on_follow_request(connection, request, pubsub_itc_fw::Leading::DoesNotLead, pubsub_itc_fw::FollowerLimit::LastReleased, converters_.engine, 0);
}

void ServingWalFollowers::serve_engine(const pubsub_itc_fw::ConnectionID& connection, int64_t after_seq_no, EngineServing serving) {
    links_.publish_released_position();
    // Every engine is served up to the last record released, so that none applies an order before the engine that leads
    // is sent it (docs/availability/voided_commands.md, section 5.1). An engine that is to lead is sent the commands, as
    // order routing sends them. One that follows the WAL is sent every record, reports included, as a publisher is, so
    // that it can tell that none is missing and which reports the WAL holds.
    const int converter = serving.kind == EngineKind::caught_up_to_follow ? converters_.publisher : converters_.engine;
    const std::optional<pubsub_itc_fw::WalFollowStart> started = serving.started;
    engine_servings_[connection.get_value()] = std::move(serving);
    service_->serve(connection, after_seq_no, pubsub_itc_fw::FollowerLimit::LastReleased, converter, pubsub_itc_fw::CaughtUpMessage::DoNotSend, started);
}

// ---- Sending to followers handed over ------------------------------------------------------------------------------

void ServingWalFollowers::send_committed_to_publishers(int64_t seq_no) {
    if (!handed_over_publishers_.has_value() || handed_over_publishers_->empty()) {
        return;
    }
    for (const auto& [publisher, damaged_seq_no] : handed_over_publishers_->send_to_all(seq_no)) {
        cannot_be_served(publisher, damaged_seq_no, "a matching engine publisher");
    }
}

void ServingWalFollowers::send_released_to_follower_engines(int64_t released_through) {
    if (!handed_over_engines_.has_value() || handed_over_engines_->empty()) {
        return;
    }
    for (const auto& [engine, damaged_seq_no] : handed_over_engines_->send_to_all(released_through)) {
        cannot_be_served(engine, damaged_seq_no, "a matching engine that follows the WAL");
    }
}

// ---- Questions -----------------------------------------------------------------------------------------------------

bool ServingWalFollowers::being_caught_up(const pubsub_itc_fw::ConnectionID& connection) const {
    return service_.has_value() && service_->is_being_caught_up(connection);
}

bool ServingWalFollowers::caught_up_on_its_request(const pubsub_itc_fw::ConnectionID& connection) const {
    const auto serving = engine_servings_.find(connection.get_value());
    return serving != engine_servings_.end() && serving->second.kind != EngineKind::sent_the_orders_it_lacks;
}

bool ServingWalFollowers::follows_the_wal(const pubsub_itc_fw::ConnectionID& connection) const {
    return handed_over_engines_.has_value() && handed_over_engines_->contains(connection);
}

// ---- PeerCatchUpInterface ------------------------------------------------------------------------------------------

void ServingWalFollowers::start_peer_catch_up(int64_t after_seq_no) {
    // On the connection the replicated WAL sends its records on, so that the records sent by the reading thread and the
    // new records sent from here after the hand-over travel on one connection, in order.
    peer_catch_up_connection_ = links_.peer_connection();
    if (!peer_catch_up_connection_.is_valid() || !service_.has_value()) {
        return;
    }
    service_->serve(peer_catch_up_connection_, after_seq_no, pubsub_itc_fw::FollowerLimit::LastWritten, converters_.peer,
                    pubsub_itc_fw::CaughtUpMessage::DoNotSend, std::nullopt);
}

void ServingWalFollowers::stop_peer_catch_up() {
    if (!service_.has_value()) {
        return;
    }
    // Only the follower sequencer's serving stops, with the reading thread's confirmation, so that anything sent to it
    // afterwards follows every record the reading thread sent it. A matching engine being served goes on being served.
    // Everything stops when this instance stops leading (stopped_leading), which is what keeps a WAL about to be cut back
    // from being read.
    if (peer_catch_up_connection_.is_valid()) {
        service_->stop_serving(peer_catch_up_connection_);
        peer_catch_up_connection_ = pubsub_itc_fw::ConnectionID{};
    }
    links_.end_pause_for_hand_over();
}

bool ServingWalFollowers::reading_has_stopped() const {
    return !service_.has_value() || service_->reading_has_stopped();
}

// ---- WalFollowerEventsInterface ------------------------------------------------------------------------------------

void ServingWalFollowers::follower_handed_over(const pubsub_itc_fw::ConnectionID& follower, int64_t through, int64_t records_sent,
                                               pubsub_itc_fw::WalPosition resume_at) {
    if (links_.is_publisher_connection(follower)) {
        // Told it has caught up already, by the service; from here it is sent each record as it is committed, read on from
        // where the reading thread stopped.
        handed_over_through(through);
        handed_over_publishers_->add(follower, through, resume_at);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "ServingWalFollowers: the matching engine publisher on connection {} holds every record through {} ({} sent by the reading thread) -- "
                   "each record from here is sent as it is committed",
                   follower.get_value(), through, records_sent);
        return;
    }
    if (follower == peer_catch_up_connection_) {
        peer_catch_up_connection_ = pubsub_itc_fw::ConnectionID{};
        // The replicated WAL sends the follower sequencer its records from here; the service forgets it, so that it is
        // never told of a change of leader meant for the followers that ask.
        service_->on_connection_lost(follower);
        handed_over_through(through);
        links_.peer_caught_up(through);
        links_.end_pause_for_hand_over();
        return;
    }
    const auto engine = engine_servings_.find(follower.get_value());
    if (engine != engine_servings_.end()) {
        const EngineServing serving = engine->second;
        engine_servings_.erase(engine);
        engine_handed_over(follower, through, records_sent, resume_at, serving);
        links_.end_pause_for_hand_over();
    }
}

void ServingWalFollowers::engine_handed_over(const pubsub_itc_fw::ConnectionID& engine, int64_t through, int64_t records_sent,
                                             pubsub_itc_fw::WalPosition resume_at, const EngineServing& serving) {
    links_.engine_catch_up_ended(engine);
    handed_over_through(through);
    const bool answers_catch_up_request = serving.kind != EngineKind::sent_the_orders_it_lacks;
    links_.engine_handed_over(engine, through, records_sent, answers_catch_up_request);
    if (!answers_catch_up_request) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "ServingWalFollowers: the reading thread has sent the matching engine on connection {} the orders it lacked, through {} ({} sent) -- "
                   "orders go to it from here again",
                   engine.get_value(), through, records_sent);
        return;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "ServingWalFollowers: WAL catch-up complete -- {} record(s) streamed to ME connection {}",
               records_sent, engine.get_value());

    // On receipt the engine takes its book as reconciled. What was sent, not what was walked past: the engine checks it
    // received exactly this many and does not act if it did not, which is R-0101. The earliest record the WAL holds
    // tells the engine whether the WAL reaches back to the first record of the day (R-0123).
    const int64_t earliest_held = links_.first_seq_no_held();
    pubsub_itc_fw::WalFollowCaughtUp caught_up{};
    caught_up.last_seq_no = through;
    caught_up.first_seq_no_held = earliest_held;
    caught_up.records_sent = records_sent;
    caught_up.request_number = serving.started.has_value() ? serving.started->message.request_number : 0;
    links_.send_caught_up(engine, caught_up);
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "ServingWalFollowers: WalFollowCaughtUp sent (last_seq_no={}, earliest retained={}, records_sent={}) -- ME is now live", through, earliest_held,
               records_sent);

    // Only for an engine that asked to lead. One that is starting is sent the records and no routing: it does not yet
    // know whether its peer leads, and learns that from the peer over a different channel and on its own schedule. Moving
    // the order connection on the strength of the ask would take the routing off a working leader, and nothing would put
    // it back (BUG-0077, ha_test.py scenario 54). The asker is the only party that knows which case this is, which is
    // why the request carries it.
    if (serving.kind == EngineKind::caught_up_to_follow) {
        // From here it is sent each record as it is released, read on from where the reading thread stopped.
        if (handed_over_engines_.has_value()) {
            handed_over_engines_->add(engine, through, resume_at);
        }
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "ServingWalFollowers: connection {} caught up but is not to lead -- order routing left where it is, and it is sent each record as it is "
                   "committed, following the WAL",
                   engine.get_value());
        return;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "ServingWalFollowers: ME order connection promoted to {} -- sequenced orders now route to the caught-up ME", engine.get_value());
    links_.engine_caught_up_to_lead(engine);
}

void ServingWalFollowers::hand_over_falling_behind(const pubsub_itc_fw::ConnectionID& /*follower*/) {
    links_.pause_for_hand_over();
}

void ServingWalFollowers::follower_progress(const pubsub_itc_fw::ConnectionID& follower, int64_t sent_through) {
    // Only a matching engine that asked to be caught up: the recovery limit measures how long its catch-up will take.
    if (caught_up_on_its_request(follower)) {
        links_.engine_catch_up_progress(follower, sent_through);
    }
}

void ServingWalFollowers::follower_cannot_be_served(const pubsub_itc_fw::ConnectionID& follower, int64_t damaged_seq_no) {
    std::string which_follower = "the matching engine";
    if (follower == peer_catch_up_connection_) {
        peer_catch_up_connection_ = pubsub_itc_fw::ConnectionID{};
        which_follower = "the follower sequencer";
    } else if (links_.is_publisher_connection(follower)) {
        which_follower = "a matching engine publisher";
    } else {
        engine_servings_.erase(follower.get_value());
    }
    cannot_be_served(follower, damaged_seq_no, which_follower);
    links_.end_pause_for_hand_over();
}

void ServingWalFollowers::declined_while_not_leading(const pubsub_itc_fw::ConnectionID& /*follower*/) {}

void ServingWalFollowers::reading_stopped() {
    // The follower sequencer's request, if one waited for the reading thread to stop, is answered now.
    links_.peer_catch_up_stopped();
}

// ---- Damaged records -----------------------------------------------------------------------------------------------

void ServingWalFollowers::cannot_be_served(const pubsub_itc_fw::ConnectionID& follower, int64_t damaged_seq_no, const std::string& which_follower) {
    damaged_record_standing_ = damaged_seq_no;
    links_.set_damaged_record_gauge(damaged_seq_no);
    const bool first_time = damaged_records_reported_.insert(which_follower + " " + std::to_string(damaged_seq_no)).second;
    PUBSUB_LOG(logger_, first_time ? pubsub_itc_fw::FwLogLevel::Error : pubsub_itc_fw::FwLogLevel::Info,
               "ServingWalFollowers: record {} in the write-ahead log cannot be read, so {} on connection {} is not caught up past it and is not handed "
               "over -- it does not act on what it holds until a person deals with the record{}",
               damaged_seq_no, which_follower, follower.get_value(), first_time ? "" : " (asked again, and refused again)");
}

void ServingWalFollowers::handed_over_through(int64_t through) {
    if (damaged_record_standing_ != 0 && through >= damaged_record_standing_) {
        damaged_record_standing_ = 0;
        links_.set_damaged_record_gauge(0);
    }
}

} // namespaces
