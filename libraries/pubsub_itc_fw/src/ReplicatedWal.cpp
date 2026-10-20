// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/ReplicatedWal.hpp>

#include <algorithm>
#include <array>
#include <utility>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/LeaderEpoch.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

namespace pubsub_itc_fw {

const char* to_string(ReplicaRole role) {
    switch (role) {
        case ReplicaRole::leader:
            return "leader";
        case ReplicaRole::follower:
            return "follower";
        case ReplicaRole::unknown:
            break;
    }
    return "unknown";
}

ReplicatedWal::ReplicatedWal(ReplicatedWalConfiguration configuration, QuillLogger& logger, ReplicationLinksInterface& links, LeaseLinksInterface& lease_links,
                             ReplicatedWalEventsInterface& events, std::optional<PromisesKeptElsewhere> promises_elsewhere)
    : configuration_(std::move(configuration))
    , logger_(logger)
    , links_(links)
    , lease_links_(lease_links)
    , events_(events)
    , epoch_store_(configuration_.wal_directory + "/epoch.state")
    , promise_store_(configuration_.wal_directory + "/lease_promise.state", LeasePromiseStore::current_boot_id())
    , promises_elsewhere_(std::move(promises_elsewhere)) {}

int64_t ReplicatedWal::open(Clock::time_point now) {
    if (!configuration_.helper_cores.empty()) {
        wal_.set_helper_cores(configuration_.helper_cores);
    }

    // The whole WAL is read, rather than from the snapshot onwards, because the table of epochs needs
    // every record's epoch, and the check for a gap needs every record's number.
    int64_t expected = 1;
    int64_t last_before_gap = -1;
    int64_t first_after_gap = 0;
    const int64_t recovered_seq = wal_.open(
        configuration_.wal_directory, configuration_.wal_segment_size,
        [this, &expected, &last_before_gap, &first_after_gap](int64_t seq_no, int16_t /*pdu_id*/, const uint8_t* /*payload*/, size_t /*payload_size*/,
                                                              int64_t /*wall_time_ns*/, int32_t leader_epoch) {
            if (last_before_gap >= 0) {
                return;
            }
            if (seq_no != expected) {
                last_before_gap = expected - 1;
                first_after_gap = seq_no;
                return;
            }
            log_epochs_.note_record(seq_no, leader_epoch);
            ++expected;
        },
        WalOpenMode{WalOpenMode::IgnoreSnapshot});

    if (last_before_gap >= 0) {
        // Records after a gap were sent by a leader, which still holds them and sends them again once
        // this instance's WAL and its leader's agree (docs/availability/follower_log_repair.md, 4.2).
        PUBSUB_LOG(logger_, FwLogLevel::Warning,
                   "{}: the write-ahead log has a gap -- record {} follows record {}. Discarding every record from {} to {}; the leader sends them again",
                   configuration_.owner_name, first_after_gap, last_before_gap, last_before_gap + 1, recovered_seq);
        wal_.truncate_after(last_before_gap);
    }
    highest_replicated_seq_no_ = wal_.last_seq_no();
    next_seq_no_ = wal_.last_seq_no() + 1;
    if (positions_ != nullptr) {
        positions_->publish_written(wal_.last_seq_no(), wal_.write_position());
    }

    // Recover the leadership generation before any lease is asked for. Without this the instance
    // starts at zero, and a pair restarted together would agree a generation already spent.
    epoch_ = epoch_store_.load();
    if (epoch_ > 0) {
        PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: recovered epoch={} from {}", configuration_.owner_name, epoch_, epoch_store_.path());
    } else {
        PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: no stored epoch at {} -- starting from epoch 0", configuration_.owner_name, epoch_store_.path());
    }

    if (!configuration_.ha_enabled) {
        // Running alone: lead at once; every record is committed once written.
        set_epoch(LeaderEpoch::next_for(epoch_, configuration_.instance_id));
        change_role(ReplicaRole::leader, now);
        return wal_.last_seq_no();
    }

    const int64_t peer_id = configuration_.instance_id == 1 ? 2 : 1;
    lease_agent_.emplace(configuration_.owner_name, logger_, lease_links_, configuration_.pair_id, configuration_.instance_id, peer_id,
                         configuration_.third_voter_id, configuration_.third_voter_name, configuration_.lease, now, epoch_,
                         PeerStatementsFlag{PeerStatementsFlag::SayWhetherPeerMayLead});
    if (test_events_ != nullptr) {
        lease_agent_->report_test_events_to(*test_events_);
    }
    if (promises_elsewhere_.has_value()) {
        lease_agent_->keep_promises_in(*promises_elsewhere_->recorder, promises_elsewhere_->kept, promises_elsewhere_->kept_statement, now);
    } else {
        owned_promise_recorder_.emplace(promise_store_);
        lease_agent_->keep_promises_in(*owned_promise_recorder_, promise_store_.load(), promise_store_.load_statement(), now);
    }
    return wal_.last_seq_no();
}

void ReplicatedWal::write(int64_t seq_no, int32_t leader_epoch, int16_t pdu_id, const uint8_t* bytes, size_t size, int64_t wall_time_ns) {
    // The one thing on the order path that touches a disk, so how long it takes is recorded.
    const auto started = Clock::now();
    wal_.append(seq_no, pdu_id, bytes, static_cast<int>(size), wall_time_ns, leader_epoch);
    if (positions_ != nullptr) {
        // After the record is completely written, so that a reader that stops before this position reads
        // only complete records.
        positions_->publish_written(seq_no, wal_.write_position());
    }
    const auto elapsed = Clock::now() - started;
    configuration_.wal_append_histogram.observe(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));

    if (seq_no != log_epochs_.last_seq_no() + 1) {
        // Records are numbered without a break; one that is not means the table of epochs no longer
        // describes the WAL, and a rejoining follower could be told the WALs agree when they do not.
        PUBSUB_LOG(logger_, FwLogLevel::Error,
                   "{}: record {} was written after record {} -- the log has a gap, and the table of epochs is not extended past it", configuration_.owner_name,
                   seq_no, log_epochs_.last_seq_no());
        return;
    }
    log_epochs_.note_record(seq_no, leader_epoch);
}

int64_t ReplicatedWal::append(int16_t pdu_id, const uint8_t* bytes, size_t size, int64_t wall_time_ns, Clock::time_point now) {
    const int64_t seq_no = next_seq_no_;
    ++next_seq_no_;
    write(seq_no, epoch_, pdu_id, bytes, size, wall_time_ns);

    if (follower_wal_matches_ && links_.peer_connected()) {
        ReplicatedRecord record{};
        record.seq_no = seq_no;
        record.leader_epoch = epoch_;
        record.wall_time_ns = wall_time_ns;
        record.pdu_id = pdu_id;
        record.payload = BytesView{bytes, size};
        links_.send_record_to_peer(record);
    }

    if (waits_for_follower()) {
        if (!waiting_since_.has_value()) {
            waiting_since_ = now;
        }
    } else if (configuration_.ha_enabled) {
        // No follower keeps up: a record the follower lacks is committed only once a voter has confirmed
        // that the follower may not lead (rule 11, docs/availability/a_follower_behind_does_not_lead.md).
        say_follower_may_not_lead(now);
    }
    advance_commit();
    return seq_no;
}

bool ReplicatedWal::waits_for_follower() const {
    return configuration_.ha_enabled && links_.peer_connected() && follower_wal_matches_ && !running_alone_ && peer_acked_through_ >= peer_handed_over_at_;
}

bool ReplicatedWal::may_commit_without_follower() const {
    return !configuration_.ha_enabled || !lease_agent_.has_value() || lease_agent_->may_act_without_peer();
}

void ReplicatedWal::advance_commit() {
    if (role_ != ReplicaRole::leader) {
        return;
    }
    int64_t through = committed_through_;
    if (waits_for_follower()) {
        through = std::max(through, peer_acked_through_);
    } else if (may_commit_without_follower()) {
        through = std::max(through, log_epochs_.last_seq_no());
    } else {
        through = std::max(through, peer_acked_through_);
    }
    if (through > committed_through_) {
        committed_through_ = through;
        if (positions_ != nullptr) {
            positions_->publish_committed(committed_through_);
        }
        events_.committed_through(committed_through_);
    }
}

void ReplicatedWal::follower_too_far_behind(const char* reason, Clock::time_point now) {
    if (waits_for_follower()) {
        start_running_alone(reason, now);
    }
}

void ReplicatedWal::start_running_alone(const char* reason, Clock::time_point now) {
    if (running_alone_) {
        return;
    }
    running_alone_ = true;
    running_alone_since_ = now;
    waiting_since_.reset();
    configuration_.running_alone_gauge.set(1.0);
    PUBSUB_LOG(logger_, FwLogLevel::Warning,
               "{}: the follower is too far behind -- {}. Running as if alone: once a voter confirms that the follower may not lead, records are "
               "committed without waiting for it, so resilience is reduced until the follower catches up. The follower has acknowledged through {} of {}",
               configuration_.owner_name, reason, peer_acked_through_, log_epochs_.last_seq_no());
    report(RunningAloneChanged{configuration_.owner_name, configuration_.instance_id, true, reason, peer_acked_through_, log_epochs_.last_seq_no()});
    say_follower_may_not_lead(now);
    advance_commit();
}

void ReplicatedWal::stop_running_alone(const char* reason, Clock::time_point now) {
    if (!running_alone_) {
        return;
    }
    running_alone_ = false;
    configuration_.running_alone_gauge.set(0.0);
    const auto alone_for = std::chrono::duration_cast<std::chrono::milliseconds>(now - running_alone_since_);
    PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: the follower is keeping up again -- {}. Waiting for its acknowledgements again after {} ms running as if alone",
               configuration_.owner_name, reason, alone_for.count());
    report(RunningAloneChanged{configuration_.owner_name, configuration_.instance_id, false, reason, peer_acked_through_, log_epochs_.last_seq_no()});
}

void ReplicatedWal::say_follower_may_not_lead(Clock::time_point now) {
    if (!lease_agent_.has_value() || role_ != ReplicaRole::leader || !lease_agent_->acting(now) || !lease_agent_->says_peer_may_lead()) {
        return;
    }
    lease_agent_->begin_running_without_peer(now);
    PUBSUB_LOG(logger_, FwLogLevel::Info,
               "{}: saying the follower may not lead -- records it does not hold are not committed until a voter confirms that statement",
               configuration_.owner_name);
    report(PeerMayLeadChanged{configuration_.owner_name, configuration_.instance_id, false, 0});
}

void ReplicatedWal::say_follower_may_lead_if_it_holds_everything(Clock::time_point now) {
    if (!lease_agent_.has_value() || role_ != ReplicaRole::leader || !lease_agent_->acting(now) || lease_agent_->says_peer_may_lead()) {
        return;
    }
    // In this order: the leader waits for the follower's acknowledgements again, and the follower has
    // acknowledged every record; only then is it true that the follower holds everything committed.
    if (!waits_for_follower() || peer_acked_through_ < log_epochs_.last_seq_no()) {
        return;
    }
    lease_agent_->peer_holds_everything(now);
    PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: the follower holds every record through {} -- saying it may lead again", configuration_.owner_name,
               peer_acked_through_);
    report(PeerMayLeadChanged{configuration_.owner_name, configuration_.instance_id, true, peer_acked_through_});
}

bool ReplicatedWal::handle_message(const ConnectionID& from, int16_t pdu_id, const uint8_t* payload, size_t size, Clock::time_point now) {
    std::array<uint8_t, 64 * 1024> arena_buffer;
    BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    size_t consumed = 0;
    size_t needed = 0;
    const auto refuse = [this](const char* what) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning, "{}: failed to decode {} -- dropping", configuration_.owner_name, what);
        return true;
    };
    if (pdu_id == ReplicatedRecord::message_pdu_id) {
        ReplicatedRecordView view{};
        if (!decode(view, payload, size, consumed, arena, needed)) {
            return refuse("ReplicatedRecord");
        }
        handle_record(from, view, now);
        return true;
    }
    if (pdu_id == WalAck::message_pdu_id) {
        WalAckView view{};
        if (!decode(view, payload, size, consumed, arena, needed)) {
            return refuse("WalAck");
        }
        handle_ack(view.seq_no, now);
        return true;
    }
    if (pdu_id == LogPositionRequest::message_pdu_id) {
        LogPositionRequestView view{};
        if (!decode(view, payload, size, consumed, arena, needed)) {
            return refuse("LogPositionRequest");
        }
        handle_position_request(view.last_seq_no, view.last_epoch, now);
        return true;
    }
    if (pdu_id == LogPositionReply::message_pdu_id) {
        LogPositionReplyView view{};
        if (!decode(view, payload, size, consumed, arena, needed)) {
            return refuse("LogPositionReply");
        }
        handle_position_reply(view, now);
        return true;
    }
    if (pdu_id == LeaseRequest::message_pdu_id) {
        LeaseRequestView view{};
        if (!decode(view, payload, size, consumed, arena, needed)) {
            return refuse("LeaseRequest");
        }
        handle_lease_request(from, view, now);
        return true;
    }
    if (pdu_id == LeaseGrant::message_pdu_id) {
        LeaseGrantView view{};
        if (!decode(view, payload, size, consumed, arena, needed)) {
            return refuse("LeaseGrant");
        }
        handle_lease_grant(view, now);
        return true;
    }
    if (pdu_id == LeaseRefusal::message_pdu_id) {
        LeaseRefusalView view{};
        if (!decode(view, payload, size, consumed, arena, needed)) {
            return refuse("LeaseRefusal");
        }
        handle_lease_refusal(view, now);
        return true;
    }
    return false;
}

void ReplicatedWal::handle_record(const ConnectionID& from, const ReplicatedRecordView& record, Clock::time_point /*now*/) {
    if (role_ == ReplicaRole::leader || !follower_wal_agreed_) {
        PUBSUB_LOG(logger_, FwLogLevel::Debug, "{}: replicated record {} discarded -- this log and the leader's are not yet known to agree",
                   configuration_.owner_name, record.seq_no);
        return;
    }
    const int64_t last = log_epochs_.last_seq_no();
    if (record.seq_no <= last) {
        // Already held: sent again after the WALs were found to agree. Not written twice.
        return;
    }
    if (record.seq_no > last + 1) {
        // A record is missing. Writing this one would leave a gap, so nothing more is written until the
        // leader has been asked again where the WALs agree, which the lease tick does.
        follower_wal_agreed_ = false;
        PUBSUB_LOG(logger_, FwLogLevel::Warning,
                   "{}: replicated record {} arrived after record {} -- one is missing, so this log and the leader's are no longer known to agree; asking "
                   "again",
                   configuration_.owner_name, record.seq_no, last);
        return;
    }
    write(record.seq_no, record.leader_epoch, record.pdu_id, record.payload.data, record.payload.size, record.wall_time_ns);
    highest_replicated_seq_no_ = std::max(highest_replicated_seq_no_, record.seq_no);

    WalAck ack{};
    ack.seq_no = record.seq_no;
    links_.send_ack_to_peer(from, ack);

    WalEntryContents entry;
    entry.wall_time_ns = record.wall_time_ns;
    entry.leader_epoch = record.leader_epoch;
    entry.pdu_id = record.pdu_id;
    entry.bytes = record.payload.data;
    entry.size = record.payload.size;
    events_.written_as_follower(record.seq_no, entry);
}

void ReplicatedWal::handle_ack(int64_t seq_no, Clock::time_point now) {
    if (role_ != ReplicaRole::leader) {
        return;
    }
    // Acknowledgements arrive in the order the records were sent, so the highest is also the highest
    // without a gap below it; max guards against a reordering letting the position run ahead.
    peer_acked_through_ = std::max(peer_acked_through_, seq_no);
    if (peer_acked_through_ >= log_epochs_.last_seq_no()) {
        waiting_since_.reset();
    } else if (waits_for_follower()) {
        // The oldest record still waiting was written no later than now, so the timeout runs from here.
        waiting_since_ = now;
    }
    if (running_alone_ && peer_acked_through_ >= log_epochs_.last_seq_no()) {
        stop_running_alone("it has acknowledged every record this leader has written", now);
    }
    advance_commit();
    say_follower_may_lead_if_it_holds_everything(now);
}

void ReplicatedWal::forget_agreement() {
    unanswered_request_.reset();
    follower_wal_agreed_ = false;
    follower_wal_matches_ = false;
    waiting_since_.reset();
    peer_handed_over_at_ = 0;
    if (peer_catching_up_) {
        peer_catching_up_ = false;
        if (peer_catch_up_ != nullptr) {
            peer_catch_up_->stop_peer_catch_up();
        }
    }
    // A leader that stops sending live records says so, or its follower, still believing the WALs
    // agree, would wait for records that never come.
    if (configuration_.ha_enabled && role_ == ReplicaRole::leader && links_.peer_connected()) {
        LogPositionReply reply{};
        reply.ask_again = true;
        links_.send_position_reply_to_peer(reply);
    }
}

void ReplicatedWal::send_position_request(Clock::time_point now) {
    if (!configuration_.ha_enabled || role_ == ReplicaRole::leader || !links_.peer_connected() || follower_wal_agreed_) {
        return;
    }
    LogPositionRequest request{};
    request.last_seq_no = log_epochs_.last_seq_no();
    request.last_epoch = log_epochs_.last_epoch();
    last_position_request_at_ = now;
    links_.send_position_request_to_peer(request);
    PUBSUB_LOG(logger_, FwLogLevel::Debug, "{}: asking the leader where its log and this one agree (last record {} from epoch {})", configuration_.owner_name,
               request.last_seq_no, request.last_epoch);
}

void ReplicatedWal::handle_position_request(int64_t last_seq_no, int32_t last_epoch, Clock::time_point /*now*/) {
    if (role_ != ReplicaRole::leader || !links_.peer_connected()) {
        return;
    }
    const LogEpochTable::PositionAnswer answer = log_epochs_.answer_position(last_seq_no, last_epoch);
    LogPositionReply reply{};
    reply.seq_no = answer.seq_no;
    reply.epoch = answer.epoch;
    reply.agreed = answer.seq_no == last_seq_no && (answer.seq_no == 0 || answer.epoch == last_epoch);

    if (peer_catch_up_ != nullptr) {
        // A follower asks on each of its connections when they open, so the same request can arrive twice
        // in quick succession. While the records after that position are being sent, the second is the
        // same request and is not answered again, or the follower would be sent every record twice
        // (BUG-0155).
        if (reply.agreed && peer_catching_up_ && last_seq_no == peer_catch_up_after_) {
            PUBSUB_LOG(logger_, FwLogLevel::Debug,
                       "{}: the follower asked again from record {}, which it is already being sent the records after -- not answered again",
                       configuration_.owner_name, last_seq_no);
            return;
        }
        // Any other request while records are being sent means the follower no longer agrees: what is
        // being sent stops. It is not answered, whether the answer agrees or not, until the reading thread
        // has confirmed that it has stopped, so that every record it was still sending is queued on the
        // connection before the answer, and the follower, having discarded them, takes up from the answer
        // cleanly. It is answered as soon as that is confirmed (on_peer_catch_up_stopped).
        if (peer_catching_up_) {
            PUBSUB_LOG(logger_, FwLogLevel::Info,
                       "{}: the follower asked again from record {} -- what was being sent to it stops, and it is answered once that has stopped",
                       configuration_.owner_name, last_seq_no);
            peer_catch_up_->stop_peer_catch_up();
            peer_catching_up_ = false;
            unanswered_request_ = UnansweredRequest{last_seq_no, last_epoch};
            return;
        }
        if (!peer_catch_up_->reading_has_stopped()) {
            unanswered_request_ = UnansweredRequest{last_seq_no, last_epoch};
            PUBSUB_LOG(logger_, FwLogLevel::Debug,
                       "{}: the follower asked from record {}, but the reading thread has not yet stopped what it was sending -- answered when it has",
                       configuration_.owner_name, last_seq_no);
            return;
        }
    }

    // On the connection the records go on, so that the follower has the answer before the records that
    // follow it.
    unanswered_request_.reset();
    links_.send_position_reply_to_peer(reply);
    if (!reply.agreed) {
        PUBSUB_LOG(logger_, FwLogLevel::Info,
                   "{}: the follower's log ends at record {} from epoch {}; it agrees with this one at most through record {} -- told to discard the rest",
                   configuration_.owner_name, last_seq_no, last_epoch, answer.seq_no);
        return;
    }

    // A request after the hand-over means the follower no longer agrees, after a gap: it is answered
    // afresh. New records sent to it before now are queued ahead of the answer, and it discards them.
    if (follower_wal_matches_) {
        PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: the follower asked again from record {} -- answered afresh", configuration_.owner_name, last_seq_no);
        follower_wal_matches_ = false;
        peer_handed_over_at_ = 0;
    }
    const int64_t after = last_seq_no;
    peer_acked_through_ = after;

    if (peer_catch_up_ != nullptr) {
        // The records are read and sent by a thread of its own; this one carries on, sending the follower
        // nothing until it is handed over (on_peer_caught_up).
        peer_catching_up_ = true;
        peer_catch_up_after_ = after;
        peer_catch_up_->start_peer_catch_up(after);
        PUBSUB_LOG(logger_, FwLogLevel::Info,
                   "{}: the follower's log agrees with this one through record {} -- the {} record(s) after that are being sent to it from the WAL; live "
                   "records follow once it has them",
                   configuration_.owner_name, after, log_epochs_.last_seq_no() - after);
        report(WalsAgree{configuration_.owner_name, configuration_.instance_id, WalSide::Leader, after});
        return;
    }

    // Without a catch-up thread: every record after it is read and sent here, then live records. Nothing
    // is written while this runs, because this thread is the one that writes, so the live records follow
    // without a gap.
    int64_t sent = 0;
    [[maybe_unused]] const auto end_position = WalReader::replay(configuration_.wal_directory, wal_.scan_start_for(after + 1),
                                                                 [this, after, &sent](int64_t record_id, const void* payload, size_t size) {
                                                                     const std::optional<WalEntryContents> contents = read_wal_entry_contents(payload, size);
                                                                     if (record_id <= after || !contents.has_value()) {
                                                                         return;
                                                                     }
                                                                     ReplicatedRecord record{};
                                                                     record.seq_no = record_id;
                                                                     record.leader_epoch = contents->leader_epoch;
                                                                     record.wall_time_ns = contents->wall_time_ns;
                                                                     record.pdu_id = contents->pdu_id;
                                                                     record.payload = BytesView{contents->bytes, contents->size};
                                                                     links_.send_record_to_peer(record);
                                                                     ++sent;
                                                                 });
    follower_wal_matches_ = true;
    // Not waited for until it has acknowledged everything just sent, or new records would wait behind it.
    peer_handed_over_at_ = log_epochs_.last_seq_no();
    PUBSUB_LOG(logger_, FwLogLevel::Info,
               "{}: the follower's log agrees with this one through record {} -- sent it the {} record(s) after that; live records follow",
               configuration_.owner_name, after, sent);
    report(WalsAgree{configuration_.owner_name, configuration_.instance_id, WalSide::Leader, after});
}

void ReplicatedWal::on_peer_caught_up(int64_t through, Clock::time_point /*now*/) {
    if (!peer_catching_up_ || role_ != ReplicaRole::leader) {
        return;
    }
    peer_catching_up_ = false;
    follower_wal_matches_ = true;
    peer_handed_over_at_ = through;
    PUBSUB_LOG(logger_, FwLogLevel::Info,
               "{}: the follower has been sent every record through {} -- this instance sends it each new record from now, and waits for it again once "
               "it has acknowledged them all",
               configuration_.owner_name, through);
    report(FollowerHandedOver{configuration_.owner_name, configuration_.instance_id, through});
}

void ReplicatedWal::on_peer_catch_up_stopped(Clock::time_point now) {
    if (!unanswered_request_.has_value() || peer_catch_up_ == nullptr || !peer_catch_up_->reading_has_stopped()) {
        return;
    }
    const UnansweredRequest request = *unanswered_request_;
    unanswered_request_.reset();
    handle_position_request(request.last_seq_no, request.last_epoch, now);
}

void ReplicatedWal::handle_position_reply(const LogPositionReplyView& reply, Clock::time_point now) {
    if (role_ == ReplicaRole::leader) {
        return;
    }
    if (reply.ask_again) {
        follower_wal_agreed_ = false;
        send_position_request(now);
        return;
    }
    if (follower_wal_agreed_) {
        return;
    }

    // Nothing writes the WAL but this thread, so no record is being written while records are discarded.
    const int64_t last_before = log_epochs_.last_seq_no();
    const LogEpochTable::FollowerStep step = log_epochs_.follower_step(LogEpochTable::PositionAnswer{reply.seq_no, reply.epoch});
    const int64_t keep_through = step.keep_through;
    const bool agreed = reply.agreed && step.agreed && keep_through == last_before;
    if (keep_through < last_before && peer_catch_up_ != nullptr && !peer_catch_up_->reading_has_stopped()) {
        // A thread may still be reading this WAL for a follower this instance served while it led, and
        // cutting the WAL back under it would have it read a segment being shortened or deleted. Asked
        // again at the next tick, by which time it will have stopped.
        PUBSUB_LOG(logger_, FwLogLevel::Debug, "{}: records {} to {} must be discarded, but the WAL is still being read for a follower -- asking again later",
                   configuration_.owner_name, keep_through + 1, last_before);
        return;
    }
    if (keep_through < last_before) {
        wal_.truncate_after(keep_through);
        if (positions_ != nullptr) {
            positions_->publish_written(keep_through, wal_.write_position());
        }
        log_epochs_.truncate_after(keep_through);
        highest_replicated_seq_no_ = keep_through;
        // A record this instance numbered itself, while it led, may be among those discarded. Its next
        // number follows the last record kept, so that if it leads again it does not number past a gap;
        // taking the lead raises it above every record replicated since.
        next_seq_no_ = keep_through + 1;
        PUBSUB_LOG(logger_, FwLogLevel::Warning,
                   "{}: discarded records {} to {} from this instance's log -- the leader does not hold them. They were never committed, so nothing has "
                   "acted on them",
                   configuration_.owner_name, keep_through + 1, last_before);
        report(RecordsDiscarded{configuration_.owner_name, configuration_.instance_id, keep_through + 1, last_before});
        events_.discarded_after(keep_through);
    }
    if (agreed) {
        follower_wal_agreed_ = true;
        PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: this log agrees with the leader's through record {} -- writing the records it sends from here",
                   configuration_.owner_name, keep_through);
        report(WalsAgree{configuration_.owner_name, configuration_.instance_id, WalSide::Follower, keep_through});
        return;
    }
    send_position_request(now);
}

void ReplicatedWal::handle_lease_request(const ConnectionID& from, const LeaseRequestView& request, Clock::time_point now) {
    if (request.pair_id != configuration_.pair_id || !lease_agent_.has_value()) {
        PUBSUB_LOG(logger_, FwLogLevel::Warning, "{}: LeaseRequest for pair {} on the peer link -- dropping", configuration_.owner_name, request.pair_id);
        return;
    }
    const PairLeaseAgent::Change change =
        lease_agent_->on_request(from, request.candidate_instance_id, request.epoch, request.request_id,
                                 LeaderStatement{request.statement_leader_id, request.statement_epoch, request.statement_number, request.peer_may_lead}, now);
    if (change == PairLeaseAgent::Change::AgreedPeerLeads) {
        events_.granted_lease_to_peer(now);
    }
    act_on(change, now);
}

void ReplicatedWal::handle_lease_grant(const LeaseGrantView& grant, Clock::time_point now) {
    if (grant.pair_id != configuration_.pair_id || !lease_agent_.has_value()) {
        return;
    }
    act_on(lease_agent_->on_grant(grant.voter_instance_id, grant.epoch, grant.request_id, grant.echoed_statement_number, now), now);
    // A grant may carry the echo that confirms the leader's statement, which lets records commit.
    advance_commit();
}

void ReplicatedWal::handle_lease_refusal(const LeaseRefusalView& refusal, Clock::time_point now) {
    if (refusal.pair_id != configuration_.pair_id || !lease_agent_.has_value()) {
        return;
    }
    act_on(lease_agent_->on_refusal(refusal.voter_instance_id, refusal.highest_epoch, refusal.request_id, refusal.reason, now), now);
}

void ReplicatedWal::on_peer_connection_established(Clock::time_point now) {
    forget_agreement();
    send_position_request(now);
}

void ReplicatedWal::on_peer_connection_lost(Clock::time_point now) {
    forget_agreement();
    if (running_alone_) {
        stop_running_alone("it has disconnected, and the leader runs alone as it does with no follower", now);
    }
    if (configuration_.ha_enabled) {
        say_follower_may_not_lead(now);
    }
    advance_commit();
}

void ReplicatedWal::on_lease_tick(Clock::time_point now) {
    if (!lease_agent_.has_value()) {
        return;
    }
    act_on(lease_agent_->on_tick(now), now);
    advance_commit();
    say_follower_may_lead_if_it_holds_everything(now);
    if (!follower_wal_agreed_ && now - last_position_request_at_ >= position_request_interval) {
        send_position_request(now);
    }
}

void ReplicatedWal::on_acknowledgement_watch(Clock::time_point now) {
    if (!waiting_since_.has_value() || !waits_for_follower()) {
        return;
    }
    if (now - *waiting_since_ >= follower_acknowledgement_timeout) {
        start_running_alone("no acknowledgement from it for at least 100 ms while records were waiting", now);
    }
}

void ReplicatedWal::act_on(PairLeaseAgent::Change change, Clock::time_point now) {
    // Whatever happened, the highest epoch this instance knows may have risen, and it is kept on disk.
    set_epoch(lease_agent_->highest_epoch());
    switch (change) {
        case PairLeaseAgent::Change::BecameLeader:
            change_role(ReplicaRole::leader, now);
            break;
        case PairLeaseAgent::Change::StoppedLeading:
        case PairLeaseAgent::Change::AgreedPeerLeads:
            change_role(ReplicaRole::follower, now);
            break;
        case PairLeaseAgent::Change::Nothing:
            break;
    }
}

namespace {

// A role as a test event names it.
LeadershipRole leadership_role(ReplicaRole role) {
    switch (role) {
        case ReplicaRole::leader:
            return LeadershipRole::Leader;
        case ReplicaRole::follower:
            return LeadershipRole::Follower;
        case ReplicaRole::unknown:
            break;
    }
    return LeadershipRole::Unknown;
}

} // un-named namespace

void ReplicatedWal::report_role_to_test_events() {
    if (test_events_ == nullptr || role_ == ReplicaRole::unknown) {
        return;
    }
    test_events_->role_changed(configuration_.owner_name, configuration_.instance_id, LeadershipRole::Unknown, leadership_role(role_), epoch_);
}

void ReplicatedWal::change_role(ReplicaRole new_role, Clock::time_point now) {
    if (new_role == role_) {
        return;
    }
    const ReplicaRole old_role = role_;
    role_ = new_role;
    forget_agreement();

    if (new_role == ReplicaRole::leader) {
        // Number new records above every record the WAL holds. Records replicated while this instance
        // followed were written under its leader's numbers and did not move next_seq_no_, so without
        // this a new leader would give new records numbers its WAL already holds.
        if (highest_replicated_seq_no_ >= next_seq_no_) {
            PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: taking the lead -- numbering new records from {}, above the highest replicated record {} (was {})",
                       configuration_.owner_name, highest_replicated_seq_no_ + 1, highest_replicated_seq_no_, next_seq_no_);
            next_seq_no_ = highest_replicated_seq_no_ + 1;
        }
        // Every record this instance holds was written by it or acknowledged by it as a follower, so it
        // is on the machines it can be on; the new leadership commits from here.
        committed_through_ = log_epochs_.last_seq_no();
        if (positions_ != nullptr) {
            positions_->publish_committed(committed_through_);
        }
        peer_acked_through_ = 0;
        waiting_since_.reset();
    } else if (new_role == ReplicaRole::follower) {
        send_position_request(now);
        if (running_alone_) {
            running_alone_ = false;
            configuration_.running_alone_gauge.set(0.0);
        }
    }
    if (test_events_ != nullptr) {
        test_events_->role_changed(configuration_.owner_name, configuration_.instance_id, leadership_role(old_role), leadership_role(new_role), epoch_);
    }
    events_.role_changed(old_role, new_role, epoch_);
}

void ReplicatedWal::set_epoch(int32_t new_epoch) {
    if (new_epoch < epoch_) {
        // Nothing should ask for this. Refusing keeps the counter monotonic even if a future caller gets
        // it wrong, because an epoch that moves backwards silently disarms every check that relies on it.
        PUBSUB_LOG(logger_, FwLogLevel::Warning, "{}: refusing to move epoch backwards ({} -> {}) -- keeping {}", configuration_.owner_name, epoch_, new_epoch,
                   epoch_);
        return;
    }
    if (new_epoch == epoch_) {
        return;
    }
    const int32_t previous = epoch_;
    epoch_ = new_epoch;
    // Written before the new epoch is acted on, so that a crash in between cannot bring the instance back
    // believing it still owns a generation it has spent.
    if (!epoch_store_.store(epoch_)) {
        // Carry on in the new generation regardless. Stopping would take out an instance that is otherwise
        // healthy, and the epoch in memory is still right for as long as this process lives. What is lost
        // is the guarantee across a restart, and that is worth an alert.
        PUBSUB_LOG(logger_, FwLogLevel::Error, "{}: epoch advanced {} -> {} but could not be written to {} -- a restart from here may reuse a spent generation",
                   configuration_.owner_name, previous, epoch_, epoch_store_.path());
        return;
    }
    PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: epoch advanced {} -> {} (recorded)", configuration_.owner_name, previous, epoch_);
}

} // namespaces
