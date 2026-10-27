// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalFollowerService.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace pubsub_itc_fw {

WalFollowerService::WalFollowerService(QuillLogger& logger, const Wal& wal, const WalFollowingPositions& positions, WalFollowerLinksInterface& links,
                                       WalFollowerEventsInterface& events)
    : logger_(logger), wal_(wal), positions_(positions), links_(links), events_(events) {}

void WalFollowerService::on_follow_request(const ConnectionID& connection, const WalFollowRequestView& request, Leading leading, FollowerLimit limit,
                                           int converter, int64_t application_value) {
    if (leading == Leading::DoesNotLead) {
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

    // The reading thread sends it, before the first record of the answer. It is the thread that sends the records, and
    // it acts on its commands in the order they arrive, so every record of an earlier answer on this connection is
    // queued before it, and every record of this answer after it.
    WalFollowStart started{};
    started.message.request_number = request.request_number;
    started.message.after_seq_no = request.last_seq_no;
    started.message.serving_through_seq_no = positions_.limit(limit);
    started.message.application_value = application_value;

    // A follower that holds nothing and wants nothing from the past is placed at the newest record it may be sent, and
    // sent nothing older: it is served from there, so that it is sent any record released since.
    int64_t after_seq_no = request.last_seq_no;
    if (after_seq_no < 0) {
        after_seq_no = started.message.serving_through_seq_no;
        PUBSUB_LOG(logger_, FwLogLevel::Info, "WalFollowerService: {} on connection {} holds nothing and is placed at record {}, sent nothing older",
                   request.follower_name, connection.get_value(), after_seq_no);
    } else {
        PUBSUB_LOG(logger_, FwLogLevel::Info, "WalFollowerService: {} on connection {} holds records through {} -- the reading thread sends it the rest",
                   request.follower_name, connection.get_value(), after_seq_no);
    }
    serve(connection, after_seq_no, limit, converter, CaughtUpMessage::Send, started);
    followers_[connection.get_value()].request_number = request.request_number;
}

void WalFollowerService::serve(const ConnectionID& connection, int64_t after_seq_no, FollowerLimit limit, int converter, CaughtUpMessage caught_up_message,
                               const std::optional<WalFollowStart>& started) {
    // A connection already being served starts afresh: the reading thread stops the earlier serving first,
    // and a report from it still on its way carries the earlier serving's number and is ignored.
    if (followers_.find(connection.get_value()) != followers_.end()) {
        on_connection_lost(connection);
    }
    Follower& follower = followers_[connection.get_value()];
    follower.connection = connection;
    follower.limit = limit;
    follower.tell_when_caught_up = caught_up_message == CaughtUpMessage::Send;
    follower.serving = next_serving_++;

    auto command = std::make_unique<WalReadingCommand>();
    command->kind = WalReadingCommand::Kind::Serve;
    command->connection = connection;
    command->after_seq_no = after_seq_no;
    command->start = wal_.scan_start_for(after_seq_no + 1);
    command->limit = limit;
    command->converter = converter;
    command->serving = follower.serving;
    command->started = started;
    links_.send_to_reading_thread(std::move(command));
}

bool WalFollowerService::on_inter_thread_message(const EventMessage& message) {
    const std::unique_ptr<WalReadingReport> report = take_carried<WalReadingReport>(message);
    if (report == nullptr) {
        return false;
    }
    if (report->kind == WalReadingReport::Kind::StoppedAll || report->kind == WalReadingReport::Kind::Confirmed) {
        if (confirmations_awaited_ > 0) {
            --confirmations_awaited_;
        }
        if (confirmations_awaited_ == 0) {
            events_.reading_stopped();
        }
        return true;
    }

    const auto found = followers_.find(report->connection.get_value());
    if (found == followers_.end() || found->second.handed_over || found->second.serving != report->serving) {
        return true; // the connection went, or was served afresh, while the report was on its way
    }
    Follower& follower = found->second;

    if (report->kind == WalReadingReport::Kind::Progress) {
        events_.follower_progress(follower.connection, report->reached_seq_no);
        return true;
    }

    // A damaged record: never handed over. The reading thread has stopped serving the follower already.
    if (report->kind == WalReadingReport::Kind::Damaged) {
        const ConnectionID connection = follower.connection;
        followers_.erase(found);
        events_.follower_cannot_be_served(connection, report->reached_seq_no);
        return true;
    }

    // Equal to the limit now: nothing has been released since the reading thread looked, so the
    // follower is handed over at exactly this record. Short: records were released meanwhile.
    const int64_t limit_now = positions_.limit(follower.limit);
    if (report->reached_seq_no >= limit_now) {
        hand_over(follower, report->reached_seq_no, report->records_sent, report->resume_at);
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

void WalFollowerService::hand_over(Follower& follower, int64_t through, int64_t records_sent, WalPosition resume_at) {
    follower.handed_over = true;
    follower.short_rounds = 0;
    if (follower.tell_when_caught_up) {
        WalFollowCaughtUp caught_up{};
        caught_up.last_seq_no = through;
        caught_up.records_sent = records_sent;
        caught_up.first_seq_no_held = wal_.first_seq_no();
        caught_up.request_number = follower.request_number;
        links_.send_caught_up(follower.connection, caught_up);
    }
    PUBSUB_LOG(logger_, FwLogLevel::Info, "WalFollowerService: connection {} caught up through record {} ({} records sent) and is handed over to new records",
               follower.connection.get_value(), through, records_sent);
    events_.follower_handed_over(follower.connection, through, records_sent, resume_at);
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
    ++confirmations_awaited_;
    auto command = std::make_unique<WalReadingCommand>();
    command->kind = WalReadingCommand::Kind::StopAll;
    links_.send_to_reading_thread(std::move(command));
}

void WalFollowerService::stop_serving(const ConnectionID& connection) {
    on_connection_lost(connection);
    ++confirmations_awaited_;
    auto command = std::make_unique<WalReadingCommand>();
    command->kind = WalReadingCommand::Kind::Confirm;
    links_.send_to_reading_thread(std::move(command));
}

bool WalFollowerService::is_handed_over(const ConnectionID& connection) const {
    const auto found = followers_.find(connection.get_value());
    return found != followers_.end() && found->second.handed_over;
}

bool WalFollowerService::is_being_caught_up(const ConnectionID& connection) const {
    const auto found = followers_.find(connection.get_value());
    return found != followers_.end() && !found->second.handed_over;
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
