// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalReadingThread.hpp>

#include <algorithm>
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
            if (!answer_order_.empty()) {
                const uint64_t under_way = answer_order_.front();
                for (auto it = answer_order_.begin(); it != answer_order_.end();) {
                    const auto answer = answers_.find(*it);
                    if (answer != answers_.end() && answer->second->connection == command->connection) {
                        answers_.erase(answer);
                        it = answer_order_.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (!answer_order_.empty() && answer_order_.front() != under_way) {
                    start_next_answer();
                }
            }
            return;
        case WalReadingCommand::Kind::StopAll: {
            followers_.clear();
            answers_.clear();
            answer_order_.clear();
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
        case WalReadingCommand::Kind::Answer:
            answer(*command);
            return;
        case WalReadingCommand::Kind::GatherMore: {
            const auto found = answers_.find(command->answer_id);
            if (found != answers_.end()) {
                gather_segment(*found->second); // otherwise stopped meanwhile
            }
            return;
        }
    }
}

void WalReadingThread::on_connection_writable(ConnectionID id) {
    const auto found = followers_.find(id.get_value());
    if (found != followers_.end() && found->second->sending) {
        send_batch(*found->second);
    }
    // Every answer waiting to send on this connection carries on. Their numbers are taken first, because sending a
    // batch can close an answer and erase it.
    std::vector<uint64_t> waiting;
    for (const auto& [answer_id, answer] : answers_) {
        if (answer->sending && answer->connection == id) {
            waiting.push_back(answer_id);
        }
    }
    for (const uint64_t answer_id : waiting) {
        const auto answer = answers_.find(answer_id);
        if (answer != answers_.end()) {
            send_answer_batch(*answer->second);
        }
    }
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

void WalReadingThread::answer(const WalReadingCommand& command) {
    if (command.answerer == nullptr || !command.answerer->select || !command.answerer->encode_record || !command.answerer->encode_close) {
        throw PreconditionAssertion("WalReadingThread: an answer needs an answerer with all three of its functions", __FILE__, __LINE__);
    }
    auto answer = std::make_unique<Answer>();
    answer->id = command.answer_id;
    answer->connection = command.connection;
    answer->answerer = command.answerer;
    answer->after_seq_no = command.after_seq_no;
    // What it reads up to is fixed when it starts (start_next_answer).
    answer->through_seq_no = command.through_seq_no;
    answer->limit = command.limit;
    answer->keep_newest = command.keep_newest;
    answer->skip_newest = command.skip_newest;
    answer->last_seq_no = command.after_seq_no;
    answers_[command.answer_id] = std::move(answer);
    answer_order_.push_back(command.answer_id);
    // One at a time, in the order asked: a later answer waits until every earlier one is closed.
    if (answer_order_.size() == 1) {
        start_next_answer();
    }
}

void WalReadingThread::start_next_answer() {
    if (answer_order_.empty()) {
        return;
    }
    const auto found = answers_.find(answer_order_.front());
    if (found == answers_.end()) {
        return;
    }
    Answer& answer = *found->second;
    // Records up to the request's limit, and only bytes completely written: the limit is published after the position of
    // the record, so it is never ahead of the write position read after it. Both are read when the answer starts, not
    // when it was asked for, so that it reads everything there is by then, as answering the requests one after the other
    // on the owning thread would.
    const int64_t limit = positions_.limit(answer.limit);
    answer.through_seq_no = answer.through_seq_no > 0 ? std::min(answer.through_seq_no, limit) : limit;
    answer.written = positions_.write_position();
    answer.next_segment = answer.written.segment;
    PUBSUB_LOG(get_logger(), FwLogLevel::Info, "WalReadingThread: answering request {} on connection {}, from the records after {} up to {}", answer.id,
               answer.connection.get_value(), answer.after_seq_no, answer.through_seq_no);
    gather_segment(answer);
}

void WalReadingThread::gather_segment(Answer& answer) {
    const uint64_t segment = answer.next_segment;
    std::vector<WalAnswerRecord> in_segment;
    int64_t first_in_segment = 0;
    bool segment_found = false;
    WalCursor cursor;
    cursor.open(wal_directory_, WalPosition{segment, 0});
    while (before(cursor.position(), answer.written)) {
        int64_t record_id = 0;
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        // A record from a later segment means this one is finished, or that it does not exist and the cursor has moved
        // on to the next that does.
        if (!cursor.read_next(record_id, bytes, size) || cursor.position().segment != segment) {
            break;
        }
        segment_found = true;
        if (first_in_segment == 0) {
            first_in_segment = record_id;
        }
        if (record_id <= answer.after_seq_no || record_id > answer.through_seq_no) {
            continue;
        }
        const std::optional<WalEntryContents> contents = read_wal_entry_contents(bytes, size);
        if (!contents.has_value()) {
            continue;
        }
        WalAnswerRecord chosen;
        if (answer.answerer->select(record_id, *contents, chosen.kept)) {
            chosen.seq_no = record_id;
            chosen.wall_time_ns = contents->wall_time_ns;
            in_segment.push_back(std::move(chosen));
        }
    }
    // The newest are kept: a segment's records go in front of those from later segments, newest first, until there
    // are as many as asked for.
    for (auto record = in_segment.rbegin(); record != in_segment.rend(); ++record) {
        if (answer.keep_newest > 0 && static_cast<int64_t>(answer.chosen.size()) >= answer.keep_newest) {
            break;
        }
        answer.chosen.push_front(std::move(*record));
    }
    const bool enough = answer.keep_newest > 0 && static_cast<int64_t>(answer.chosen.size()) >= answer.keep_newest;
    // A segment beginning at or before the first record wanted holds the oldest the answer can need, and a segment that
    // does not exist means every older one is gone too.
    const bool reached_the_start = (first_in_segment != 0 && first_in_segment <= answer.after_seq_no + 1) || !segment_found || segment == 0;
    if (!enough && !reached_the_start) {
        answer.next_segment = segment - 1;
        auto more = std::make_unique<WalReadingCommand>();
        more->kind = WalReadingCommand::Kind::GatherMore;
        more->answer_id = answer.id;
        post_message(get_thread_id(), carry(get_thread_id(), std::move(more)));
        return;
    }
    for (int64_t skipped = 0; skipped < answer.skip_newest && !answer.chosen.empty(); ++skipped) {
        answer.chosen.pop_back();
    }
    answer.sending = true;
    send_answer_batch(answer);
}

void WalReadingThread::send_answer_batch(Answer& answer) {
    int sent_in_batch = 0;
    while (answer.next_to_send < answer.chosen.size() && sent_in_batch < records_per_batch) {
        const WalAnswerRecord& record = answer.chosen[answer.next_to_send];
        encoded_.clear();
        const int16_t pdu_id = answer.answerer->encode_record(record, encoded_);
        if (!encoded_.empty()) {
            send_pdu_payload(answer.connection, pdu_id, record.seq_no, encoded_.data(), encoded_.size());
            ++answer.records_sent;
            ++sent_in_batch;
        }
        answer.last_seq_no = record.seq_no;
        ++answer.next_to_send;
    }
    if (answer.next_to_send < answer.chosen.size()) {
        // The rest once the connection has taken this batch.
        request_writable_notification_for_connection_owned_elsewhere(answer.connection);
        return;
    }
    WalAnswerSummary summary;
    summary.records_sent = answer.records_sent;
    summary.last_seq_no = answer.last_seq_no;
    summary.asked_for = answer.keep_newest > 0 ? answer.keep_newest - answer.skip_newest : 0;
    encoded_.clear();
    const int16_t close_pdu_id = answer.answerer->encode_close(summary, encoded_);
    if (close_pdu_id != 0 && !encoded_.empty()) {
        send_pdu_payload(answer.connection, close_pdu_id, 0, encoded_.data(), encoded_.size());
    }
    auto answered = std::make_unique<WalReadingReport>();
    answered->kind = WalReadingReport::Kind::Answered;
    answered->connection = answer.connection;
    answered->answer_id = answer.id;
    answered->records_sent = answer.records_sent;
    answered->reached_seq_no = answer.last_seq_no;
    report(std::move(answered));
    PUBSUB_LOG(get_logger(), FwLogLevel::Info, "WalReadingThread: answered request {} on connection {} with {} record(s), the last {}", answer.id,
               answer.connection.get_value(), answer.records_sent, answer.last_seq_no);
    const uint64_t closed = answer.id;
    answers_.erase(closed);
    if (!answer_order_.empty() && answer_order_.front() == closed) {
        answer_order_.pop_front();
        start_next_answer();
    }
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
