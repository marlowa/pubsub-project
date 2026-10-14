// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalFollowerService.hpp>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace pubsub_itc_fw {

WalFollowerService::WalFollowerService(QuillLogger& logger, const Wal& wal, const WalFollowingPositions& positions, WalFollowerLinksInterface& links,
                                       WalFollowerEventsInterface& events)
    : logger_(logger), wal_(wal), positions_(positions), links_(links), events_(events) {}

void WalFollowerService::on_follow_request(const ConnectionID& connection, const WalFollowRequestView& request, bool leading, FollowerLimit limit,
                                           int converter) {
    if (!leading) {
        declined_[connection.get_value()] = connection;
        WalFollowDeclined declined{};
        declined.last_seq_no = request.last_seq_no;
        links_.send_declined(connection, declined);
        PUBSUB_LOG(logger_, FwLogLevel::Info,
                   "WalFollowerService: {} asked on connection {} to follow from record {}, but this instance does not lead -- declined", request.follower_name,
                   connection.get_value(), request.last_seq_no);
        events_.declined_while_not_leading(connection);
        return;
    }
    declined_.erase(connection.get_value());

    // A request made again on a connection already being served starts that follower afresh.
    if (followers_.find(connection.get_value()) != followers_.end()) {
        on_connection_lost(connection);
    }

    Follower& follower = followers_[connection.get_value()];
    follower.connection = connection;
    follower.limit = limit;
    follower.tell_when_caught_up = true;

    // A follower that holds nothing and wants nothing from the past is placed at the newest record it
    // may be sent, and sent nothing older.
    if (request.last_seq_no < 0) {
        PUBSUB_LOG(logger_, FwLogLevel::Info, "WalFollowerService: {} on connection {} holds nothing and is placed at record {}, sent nothing older",
                   request.follower_name, connection.get_value(), positions_.limit(limit));
        hand_over(follower, positions_.limit(limit), 0);
        return;
    }

    auto command = std::make_unique<WalReadingCommand>();
    command->kind = WalReadingCommand::Kind::Serve;
    command->connection = connection;
    command->after_seq_no = request.last_seq_no;
    command->start = wal_.scan_start_for(request.last_seq_no + 1);
    command->limit = limit;
    command->converter = converter;
    PUBSUB_LOG(logger_, FwLogLevel::Info, "WalFollowerService: {} on connection {} holds records through {} -- the reading thread sends it the rest",
               request.follower_name, connection.get_value(), request.last_seq_no);
    links_.send_to_reading_thread(std::move(command));
}

bool WalFollowerService::on_inter_thread_message(const EventMessage& message) {
    const std::unique_ptr<WalReadingReport> report = take_carried<WalReadingReport>(message);
    if (report == nullptr) {
        return false;
    }
    if (report->kind == WalReadingReport::Kind::StoppedAll) {
        stop_pending_ = false;
        events_.reading_stopped();
        return true;
    }

    const auto found = followers_.find(report->connection.get_value());
    if (found == followers_.end() || found->second.handed_over) {
        return true; // the connection went, or was asked about again, while the report was on its way
    }
    Follower& follower = found->second;

    // Equal to the limit now: nothing has been released since the reading thread looked, so the
    // follower is handed over at exactly this record. Short: records were released meanwhile.
    const int64_t limit_now = positions_.limit(follower.limit);
    if (report->reached_seq_no >= limit_now) {
        hand_over(follower, report->reached_seq_no, report->records_sent);
        return true;
    }

    ++follower.short_rounds;
    if (follower.short_rounds == short_rounds_before_telling) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning,
                   "WalFollowerService: connection {} has fallen short of being handed over {} times in a row -- records are released faster than one "
                   "round between the two threads",
                   follower.connection.get_value(), follower.short_rounds);
        events_.hand_over_falling_behind(follower.connection);
    }
    auto command = std::make_unique<WalReadingCommand>();
    command->kind = WalReadingCommand::Kind::Continue;
    command->connection = follower.connection;
    links_.send_to_reading_thread(std::move(command));
    return true;
}

void WalFollowerService::hand_over(Follower& follower, int64_t through, int64_t records_sent) {
    follower.handed_over = true;
    follower.short_rounds = 0;
    if (follower.tell_when_caught_up) {
        WalFollowCaughtUp caught_up{};
        caught_up.last_seq_no = through;
        caught_up.records_sent = records_sent;
        caught_up.first_seq_no_held = wal_.first_seq_no();
        links_.send_caught_up(follower.connection, caught_up);
    }
    PUBSUB_LOG(logger_, FwLogLevel::Info, "WalFollowerService: connection {} caught up through record {} ({} records sent) and is handed over to new records",
               follower.connection.get_value(), through, records_sent);
    events_.follower_handed_over(follower.connection, through);
}

void WalFollowerService::on_connection_lost(const ConnectionID& connection) {
    declined_.erase(connection.get_value());
    const auto found = followers_.find(connection.get_value());
    if (found == followers_.end()) {
        return;
    }
    if (!found->second.handed_over) {
        auto command = std::make_unique<WalReadingCommand>();
        command->kind = WalReadingCommand::Kind::Stop;
        command->connection = connection;
        links_.send_to_reading_thread(std::move(command));
    }
    followers_.erase(found);
}

void WalFollowerService::on_became_leader(int32_t epoch) {
    WalLeaderChanged changed{};
    changed.epoch = epoch;
    for (const auto& [value, connection] : declined_) {
        links_.send_leader_changed(connection, changed);
    }
    for (const auto& [value, follower] : followers_) {
        links_.send_leader_changed(follower.connection, changed);
    }
    declined_.clear();
    followers_.clear();
}

void WalFollowerService::stop_all() {
    // Every follower forgets this instance, which no longer leads; each asks again when told of a new leader.
    for (const auto& [value, follower] : followers_) {
        declined_[value] = follower.connection;
    }
    followers_.clear();
    stop_pending_ = true;
    auto command = std::make_unique<WalReadingCommand>();
    command->kind = WalReadingCommand::Kind::StopAll;
    links_.send_to_reading_thread(std::move(command));
}

bool WalFollowerService::is_handed_over(const ConnectionID& connection) const {
    const auto found = followers_.find(connection.get_value());
    return found != followers_.end() && found->second.handed_over;
}

std::vector<std::pair<ConnectionID, FollowerLimit>> WalFollowerService::handed_over_followers() const {
    std::vector<std::pair<ConnectionID, FollowerLimit>> result;
    for (const auto& [value, follower] : followers_) {
        if (follower.handed_over) {
            result.emplace_back(follower.connection, follower.limit);
        }
    }
    return result;
}

} // namespaces
