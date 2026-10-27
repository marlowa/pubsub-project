// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalReadingThread.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>

namespace pubsub_itc_fw {

namespace {

// Whether position a comes before position b in the WAL.
bool before(const WalPosition& a, const WalPosition& b) {
    return a.segment < b.segment || (a.segment == b.segment && a.offset < b.offset);
}

} // un-named namespace

ApplicationThreadConfiguration WalReadingThread::thread_configuration() {
    ApplicationThreadConfiguration configuration;
    configuration.stays_on_background_cores = true;
    return configuration;
}

WalReadingThread::WalReadingThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& thread_name, ThreadID thread_id,
                                   const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config, ThreadID owner_thread_id,
                                   std::string wal_directory, const WalFollowingPositions& positions, std::vector<WalRecordConverter> converters,
                                   WalApplicationValueReader application_value_reader)
    : ApplicationThread(token, logger, reactor, thread_name, thread_id, queue_config, allocator_config, thread_configuration())
    , owner_thread_id_(owner_thread_id)
    , wal_directory_(std::move(wal_directory))
    , positions_(positions)
    , converters_(std::move(converters))
    , application_value_reader_(std::move(application_value_reader)) {
    if (converters_.empty()) {
        throw PreconditionAssertion("WalReadingThread: at least one converter is needed", __FILE__, __LINE__);
    }
}

void WalReadingThread::on_itc_message(const EventMessage& message) {
    const std::unique_ptr<WalReadingCommand> command = take_carried<WalReadingCommand>(message);
    if (command == nullptr) {
        PUBSUB_LOG_STR(get_logger(), FwLogLevel::Warning, "WalReadingThread: an inter-thread message that is not a command -- ignored");
        return;
    }
    switch (command->kind) {
        case WalReadingCommand::Kind::Serve:
            serve(*command);
            return;
        case WalReadingCommand::Kind::Continue: {
            const auto found = followers_.find(command->connection.get_value());
            if (found == followers_.end()) {
                return; // stopped meanwhile
            }
            found->second->sending = true;
            found->second->round_limit = positions_.limit(found->second->limit);
            send_batch(*found->second);
            return;
        }
        case WalReadingCommand::Kind::Stop:
            followers_.erase(command->connection.get_value());
            return;
        case WalReadingCommand::Kind::StopAll: {
            followers_.clear();
            auto stopped = std::make_unique<WalReadingReport>();
            stopped->kind = WalReadingReport::Kind::StoppedAll;
            report(std::move(stopped));
            return;
        }
        case WalReadingCommand::Kind::Confirm: {
            // Commands are acted on in the order they arrive, so every earlier one has been.
            auto confirmed = std::make_unique<WalReadingReport>();
            confirmed->kind = WalReadingReport::Kind::Confirmed;
            report(std::move(confirmed));
            return;
        }
    }
}

void WalReadingThread::on_connection_writable(ConnectionID id) {
    const auto found = followers_.find(id.get_value());
    if (found == followers_.end() || !found->second->sending) {
        return; // stopped, or reported and waiting to be told to carry on
    }
    send_batch(*found->second);
}

void WalReadingThread::serve(const WalReadingCommand& command) {
    if (command.converter < 0 || static_cast<size_t>(command.converter) >= converters_.size()) {
        throw PreconditionAssertion(fmt::format("WalReadingThread: no converter {}", command.converter), __FILE__, __LINE__);
    }
    auto follower = std::make_unique<Follower>();
    follower->connection = command.connection;
    follower->limit = command.limit;
    follower->converter = command.converter;
    follower->last_sent_seq_no = command.after_seq_no;
    follower->serving = command.serving;
    follower->sending = true;
    follower->round_limit = positions_.limit(command.limit);
    follower->cursor.open(wal_directory_, command.start);
    // Every record before the start is one the follower holds: the start is at or before the record after after_seq_no.
    follower->resume_at = command.start;
    Follower& served = *follower;
    followers_[command.connection.get_value()] = std::move(follower);
    PUBSUB_LOG(get_logger(), FwLogLevel::Info, "WalReadingThread: sending connection {} every record after {}, read from the WAL",
               command.connection.get_value(), command.after_seq_no);
    // Before the first record, and after every record of an earlier serving of this connection, which this thread
    // sent before it acted on this command.
    if (command.started.has_value()) {
        WalFollowStarted started = command.started->message;
        if (command.started->application_value_from_wal && application_value_reader_) {
            started.application_value = read_application_value();
            PUBSUB_LOG(get_logger(), FwLogLevel::Info, "WalReadingThread: the application's value for connection {}, read from the newest records, is {}",
                       command.connection.get_value(), started.application_value);
        }
        send_pdu(command.connection, WalFollowStarted::message_pdu_id, 0, started);
    }
    send_batch(served);
}

bool WalReadingThread::read_next_record(Follower& follower, WalPosition written) {
    // Only before the published write position: everything there is completely written.
    while (before(follower.cursor.position(), written)) {
        int64_t record_id = 0;
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        if (!follower.cursor.read_next(record_id, bytes, size)) {
            // The cursor lists the segment files when it is opened, so a segment created since is
            // unknown to it. Opening it again at its own position finds it.
            const WalPosition at = follower.cursor.position();
            follower.cursor.open(wal_directory_, at);
            if (!follower.cursor.read_next(record_id, bytes, size)) {
                return false;
            }
        }
        if (record_id <= follower.last_sent_seq_no) {
            follower.resume_at = follower.cursor.position();
            continue; // the follower holds it already
        }
        follower.holding = true;
        follower.held_seq_no = record_id;
        follower.held_bytes = bytes;
        follower.held_size = size;
        follower.held_end = follower.cursor.position();
        return true;
    }
    return false;
}

void WalReadingThread::send_batch(Follower& follower) {
    // The limit as it was read when this round started, not as it is now: a round ends at it, so that the
    // owning thread can count a round that falls short and act once three in a row have, which is what
    // makes the hand-over end while records go on being released. Read anew at each batch, a round would
    // chase a limit that rises as fast as the follower takes records, and never end. The limit was read
    // before the write position read here, and the owning thread publishes the position of a record
    // before it publishes the record as written, committed or released, so the limit is never ahead of
    // the position.
    const int64_t limit = follower.round_limit;
    const WalPosition written = positions_.write_position();

    int sent_in_batch = 0;
    int examined_in_batch = 0;
    while (sent_in_batch < records_per_batch && examined_in_batch < records_examined_per_batch) {
        if (!follower.holding && !read_next_record(follower, written)) {
            break; // nothing more written yet
        }
        ++examined_in_batch;
        if (follower.held_seq_no > limit) {
            break; // held until the limit passes it
        }
        // A record whose contents cannot be read, or that the converter finds damaged, may have been one
        // the follower needed. Passing over it would let the follower be handed over as complete when it
        // is not, so serving stops here, and the owning thread is told which record it was.
        const std::optional<WalEntryContents> contents = read_wal_entry_contents(follower.held_bytes, follower.held_size);
        ConvertedRecord converted = ConvertedRecord::damaged();
        if (contents.has_value()) {
            encoded_.clear();
            converted = converters_[static_cast<size_t>(follower.converter)](follower.held_seq_no, *contents, encoded_);
        }
        if (converted.outcome == ConvertedRecord::Outcome::Damaged) {
            report_damaged(follower);
            return; // the follower is no longer served, and its entry is gone
        }
        if (converted.outcome == ConvertedRecord::Outcome::Send && !encoded_.empty()) {
            send_pdu_payload(follower.connection, converted.pdu_id, follower.held_seq_no, encoded_.data(), encoded_.size());
            ++follower.records_sent;
            ++sent_in_batch;
        }
        follower.last_sent_seq_no = follower.held_seq_no;
        follower.resume_at = follower.held_end;
        follower.holding = false;
    }

    if (sent_in_batch == records_per_batch || examined_in_batch == records_examined_per_batch) {
        const auto now = std::chrono::steady_clock::now();
        if (now - follower.progress_reported_at >= progress_interval) {
            follower.progress_reported_at = now;
            auto progress = std::make_unique<WalReadingReport>();
            progress->kind = WalReadingReport::Kind::Progress;
            progress->connection = follower.connection;
            progress->reached_seq_no = follower.last_sent_seq_no;
            progress->records_sent = follower.records_sent;
            progress->serving = follower.serving;
            report(std::move(progress));
        }
        // There may be more: send it once the connection has taken this batch. A batch that examined
        // many records and sent few, because the converter passes over most of them, yields the same
        // way, so that no one follower holds this thread for long.
        request_writable_notification_for_connection_owned_elsewhere(follower.connection);
        return;
    }

    // Everything up to the limit as read above has been sent.
    follower.sending = false;
    auto reached = std::make_unique<WalReadingReport>();
    reached->kind = WalReadingReport::Kind::Reached;
    reached->connection = follower.connection;
    reached->reached_seq_no = limit;
    reached->records_sent = follower.records_sent;
    reached->serving = follower.serving;
    reached->resume_at = follower.resume_at;
    report(std::move(reached));
}

void WalReadingThread::report_damaged(Follower& follower) {
    // Info, not Error: a follower asks again after this, and is refused again, so the application, which can
    // tell the first time from the next, says it once as an Error.
    PUBSUB_LOG(get_logger(), FwLogLevel::Info,
               "WalReadingThread: record {} in the WAL cannot be read for connection {} -- serving stops, and the follower is not handed over",
               follower.held_seq_no, follower.connection.get_value());
    auto damaged = std::make_unique<WalReadingReport>();
    damaged->kind = WalReadingReport::Kind::Damaged;
    damaged->connection = follower.connection;
    damaged->reached_seq_no = follower.held_seq_no;
    damaged->records_sent = follower.records_sent;
    damaged->serving = follower.serving;
    report(std::move(damaged));
    followers_.erase(follower.connection.get_value());
}

int64_t WalReadingThread::read_application_value() {
    // Only committed records, and only bytes completely written: the committed position is published after the
    // position of the record, so it is never ahead of the write position read after it.
    const int64_t committed = positions_.limit(FollowerLimit::LastCommitted);
    const WalPosition written = positions_.write_position();
    uint64_t segment = written.segment;
    for (int read = 0; read < application_value_segments; ++read) {
        WalCursor cursor;
        cursor.open(wal_directory_, WalPosition{segment, 0});
        std::optional<int64_t> highest;
        while (before(cursor.position(), written)) {
            int64_t record_id = 0;
            const uint8_t* bytes = nullptr;
            size_t size = 0;
            // A record from a later segment means this one is finished, or that it does not exist and the cursor has
            // moved on to the next that does.
            if (!cursor.read_next(record_id, bytes, size) || cursor.position().segment != segment) {
                break;
            }
            if (record_id > committed) {
                continue;
            }
            const std::optional<WalEntryContents> contents = read_wal_entry_contents(bytes, size);
            if (!contents.has_value()) {
                continue;
            }
            const std::optional<int64_t> value = application_value_reader_(record_id, *contents);
            if (value.has_value() && (!highest.has_value() || *value > *highest)) {
                highest = value;
            }
        }
        if (highest.has_value()) {
            return *highest;
        }
        if (segment == 0) {
            break;
        }
        --segment;
    }
    return 0;
}

void WalReadingThread::report(std::unique_ptr<WalReadingReport> report_to_send) {
    post_message(owner_thread_id_, carry(get_thread_id(), std::move(report_to_send)));
}

} // namespaces
