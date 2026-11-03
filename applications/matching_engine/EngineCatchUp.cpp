// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "EngineCatchUp.hpp"

#include <algorithm>
#include <string>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace matching_engine {

EngineCatchUp::EngineCatchUp(int64_t instance_id, bool ha_enabled, const std::unordered_set<pubsub_itc_fw::ConnectionID>& sequencer_order_connections,
                             EngineCatchUpLinksInterface& links, pubsub_itc_fw::QuillLogger& logger)
    : instance_id_(instance_id), ha_enabled_(ha_enabled), sequencer_order_connections_(sequencer_order_connections), links_(links), logger_(logger) {}

bool EngineCatchUp::begin(const RequestContext& context) {
    // The sequencers connect to this engine's order port. If at least one is already connected, the catch-up is asked
    // for now; otherwise it is asked for when one connects (send_request_on, or begin again).
    request_sent_ = false;
    bool sent = false;
    if (!sequencer_order_connections_.empty()) {
        sent = send_request(context);
    } else {
        PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Info, "EngineCatchUp: RECONCILING -- awaiting sequencer order connection before WAL catch-up");
    }

    // Only a sequencer that leads serves a catch-up; one still deciding drops the request and says nothing. An instance
    // starting with the whole venue asks at exactly that moment, so the request is repeated until it is answered. It is
    // never abandoned: acting without the catch-up is the loss this exists to prevent, and there is no answer to fall
    // back on. A venue whose sequencers are not leading is not trading either, so waiting costs nothing that is not
    // already lost.
    attempts_ = 0;
    records_seen_ = 0;
    records_at_last_tick_ = 0;
    reports_sent_ = 0;
    reports_left_out_ = 0;
    reports_in_wal_through_ = 0;
    records_from_earlier_answers_ = 0;
    links_.stop_retry_timer();
    links_.start_retry_timer();
    return sent;
}

pubsub_itc_fw::WalFollowRequest EngineCatchUp::request(const std::string& follower_name, const RequestContext& context) const {
    pubsub_itc_fw::WalFollowRequest request{};
    request.follower_name = follower_name;
    // A negative position says "I hold nothing and have applied nothing of yours": place me at your head rather than
    // sending me your history. The alternative reading of an instance with no region -- that it is a component at
    // position zero which has fallen the whole day behind -- would have the venue replay everything it still holds into
    // an engine that never held any of it.
    //
    // The position is the last command this instance has applied, however it applied it: from the leader's copy of the
    // book, by a catch-up, complete or not, or live. A catch-up cut short applied a run of records with none missing in
    // the middle, because a connection delivers in order, so the records after the last one applied are exactly those
    // still needed, and none is applied twice (BUG-0212).
    request.last_seq_no = has_position_ ? std::max(position_found_, context.highest_applied) : -1;
    request.request_number = request_number_;
    // Why this instance is asking. A promotion needs the sequencer to route orders here afterwards, and this request is
    // the only thing that tells it to -- the arbiter's decision does not reach the sequencer. A start needs the records
    // and nothing else: this instance may be about to learn from its peer that it is a follower, and a sequencer that
    // moved the order connection here on the strength of the ask would leave the venue routing orders to an instance
    // that will not act on them. See BUG-0077 and ha_test.py scenario 54.
    //
    // Every engine wants new records once caught up. One that is to lead is then sent commands by order routing; any
    // other, with high availability on, follows the WAL and is sent every record as it is committed. A single engine,
    // with high availability off, is the one that acts, so it asks as one that is to lead.
    request.wants_new_records = true;
    request.to_lead = context.to_lead || !ha_enabled_;
    return request;
}

bool EngineCatchUp::send_request(const RequestContext& context) {
    if (!context.may_ask) {
        // The catch-up would apply the command the process before this one died applying, before this instance knows
        // whether it is voided. It is asked for once that is settled.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "EngineCatchUp: not asking to be caught up yet -- waiting to learn whether command seq={}, which the process before this one died "
                   "applying, is voided",
                   context.died_applying);
        return false;
    }
    if (sequencer_order_connections_.empty()) {
        PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Warning, "EngineCatchUp: cannot ask to be caught up -- no sequencer order connection");
        return false;
    }
    // Sent to every sequencer order connection. Only the leading sequencer serves it; one that does not lead declines,
    // re-points its own order connection, and says when it leads (WalLeaderChanged), so that this instance receives
    // exactly one catch-up whichever connection the leader owns. The message refers to the name rather than copying it,
    // so the name must outlive the sends below.
    const std::string follower_name = "matching_engine_" + std::to_string(instance_id_);
    ++request_number_;
    answer_connection_ = pubsub_itc_fw::ConnectionID{};
    const pubsub_itc_fw::WalFollowRequest sent = request(follower_name, context);
    next_followed_seq_no_ = 0;
    // The tally counts the answer to this request, and is set again when the answer starts. R-0101.
    tally_ = pubsub_itc_fw::CatchUpTally(sent.last_seq_no);
    for (const auto& connection : sequencer_order_connections_) {
        links_.send_follow_request(connection, sent);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "EngineCatchUp: asked on connection {} to be caught up (request {}, last_seq_no={}, {})",
                   connection.get_value(), sent.request_number, sent.last_seq_no, context.to_lead ? "asking to lead" : "starting, not asking to lead");
    }
    request_sent_ = true;
    return true;
}

void EngineCatchUp::send_request_on(const pubsub_itc_fw::ConnectionID& connection, const RequestContext& context) {
    // The catch-up has been asked for on the other connections already. The leading sequencer serves every request it
    // is sent, so asking on those again would have it start its answer again while the first is still arriving
    // (BUG-0200). So only this connection is asked, with the same request number: if this connection's sequencer leads,
    // the others declined and nothing has arrived; if it does not, it declines, and the answer under way finishes as it
    // would have. A leader whose own connection was lost and made again in the middle of an answer starts a new one on
    // this connection, from the last command applied, and the tally is set again when it starts.
    const std::string follower_name = "matching_engine_" + std::to_string(instance_id_);
    const pubsub_itc_fw::WalFollowRequest sent = request(follower_name, context);
    links_.send_follow_request(connection, sent);
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "EngineCatchUp: asked on new connection {} to be caught up (request {}, last_seq_no={}, {}), leaving the catch-up asked for on the others "
               "to finish",
               connection.get_value(), sent.request_number, sent.last_seq_no, context.to_lead ? "asking to lead" : "starting, not asking to lead");
}

bool EngineCatchUp::started(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowStartedView& started, bool catching_up) {
    if (!catching_up || started.request_number != request_number_) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "EngineCatchUp: the sequencer on connection {} has started to answer catch-up request {}, which is not the one this instance is waiting "
                   "on (request {}, {}) -- its records are ignored",
                   connection.get_value(), started.request_number, request_number_, catching_up ? "catching up" : "not catching up");
        return false;
    }
    answer_connection_ = connection;
    tally_ = pubsub_itc_fw::CatchUpTally(started.after_seq_no);
    reports_in_wal_through_ = started.application_value;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "EngineCatchUp: the sequencer on connection {} has started to answer catch-up request {}, with the orders after {} up to {} or later; the "
               "WAL holds every report up to command {}, so no report is sent again for a command at or before it",
               connection.get_value(), started.request_number, started.after_seq_no, started.serving_through_seq_no, started.application_value);
    return true;
}

EngineCatchUp::Answer EngineCatchUp::caught_up(const pubsub_itc_fw::ConnectionID& connection, const pubsub_itc_fw::WalFollowCaughtUpView& ack,
                                               size_t book_size) {
    if (ack.request_number != request_number_) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "EngineCatchUp: the sequencer on connection {} has finished answering catch-up request {}, not the latest, {} -- ignored",
                   connection.get_value(), ack.request_number, request_number_);
        return Answer::other_request;
    }
    // R-0101: establish that the catch-up was complete before acting on it. What is checked is that every record the
    // sequencer SENT arrived, exactly once and in ascending order -- not that the numbers run consecutively, because the
    // stream is filtered and the gaps between the numbers that arrive are ordinary. See CatchUpTally, which says what
    // that does and does not establish.
    //
    // Nothing is promoted, reconciled or cancelled on a catch-up that cannot be accounted for. The retry timer is
    // deliberately left running: the next tick finds that nothing has arrived since the last ask and repeats the
    // request, so an incomplete stream is retried rather than becoming a state the engine sits in. The request names the
    // last command applied, and a command at or below it is passed over, so nothing is applied twice.
    if (!tally_.complete_with(ack.records_sent, ack.last_seq_no)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "EngineCatchUp: catch-up was not complete -- {}. Not acting on it, and asking again (R-0101). {} record(s) were applied and are kept, "
                   "because what arrived was not wrong, only incomplete",
                   tally_.describe_shortfall(ack.records_sent, ack.last_seq_no), records_seen_);
        return Answer::incomplete;
    }
    links_.stop_retry_timer();
    position_found_ = ack.last_seq_no;
    has_position_ = true;
    last_records_sent_ = ack.records_sent;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "EngineCatchUp: caught up -- book reconciled to seq_no={} (book_size={}), {} record(s) applied and {} report(s) sent to members, each "
               "marked as a possible repeat, and {} not sent because the WAL holds them; the catch-up accounted for all {} record(s) sent (R-0101); {} "
               "record(s) from earlier answers ignored",
               ack.last_seq_no, book_size, records_seen_, reports_sent_, reports_left_out_, ack.records_sent, records_from_earlier_answers_);
    return Answer::complete;
}

void EngineCatchUp::on_retry_tick(bool catching_up, const RequestContext& context) {
    if (!catching_up) {
        links_.stop_retry_timer();
        return;
    }
    if (!context.may_ask) {
        return; // nothing has been asked for yet, and it is asked for once the question is settled
    }
    ++attempts_;
    if (sequencer_order_connections_.empty()) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "EngineCatchUp: RECONCILING for {} attempt(s) with no sequencer order connection -- the venue has no matching engine until one connects "
                   "and answers",
                   attempts_);
        return;
    }
    if (records_seen_ != records_at_last_tick_) {
        // Records are arriving, so a sequencer is serving this catch-up and the silence is its length rather than its
        // absence. Asking again here would have the whole catch-up sent a second time, arriving after promotion and
        // processed as live.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "EngineCatchUp: RECONCILING -- {} record(s) applied so far, catch-up still arriving",
                   records_seen_);
        records_at_last_tick_ = records_seen_;
        return;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "EngineCatchUp: not caught up and no record after {} attempt(s) -- asking again. A sequencer that does not lead declines and says when it "
               "leads; this is for when that message, or the answer, did not come",
               attempts_);
    static_cast<void>(send_request(context));
}

bool EngineCatchUp::next_followed_record(int64_t seq_no) {
    if (next_followed_seq_no_ == 0) {
        return true; // the catch-up has not handed over yet: what arrives is still its answer
    }
    if (seq_no == next_followed_seq_no_) {
        ++next_followed_seq_no_;
        return true;
    }
    return false;
}

void EngineCatchUp::account_for_unusable_region(const pubsub_itc_fw::WalFollowCaughtUpView& ack, bool to_lead_or_alone, size_t book_size) {
    // The catch-up was asked for from the first record, so the book holds every order the WAL holds. Whether that is
    // every order the venue holds is whether the WAL begins with the first command of the trading day.
    if (ack.first_seq_no_held <= 1) {
        region_was_unusable_ = false;
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "EngineCatchUp: the order book region could not be used, and the book was rebuilt from the WAL, which reaches the first command of the "
                   "trading day -- carrying on with the {} order(s) it holds through seq_no={}, cancelling nothing, because it is exactly the venue's book "
                   "(R-0123)",
                   book_size, ack.last_seq_no);
        links_.region_rebuilt(ack.last_seq_no);
        return;
    }
    // Orders taken before the WAL's first record cannot be named, so this book is not the venue's. Cancelling the orders
    // it can name would leave every other one unmentioned, which is the silence R-0020 exists to prevent, reached while
    // appearing to have dealt with it; so nothing is cancelled.
    if (!to_lead_or_alone) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "EngineCatchUp: the order book region could not be used, and the WAL starts at seq_no={}, so the orders taken before it cannot be named "
                   "-- this instance follows, so the engine that leads goes on trading; if this instance is to lead, it halts trading first (R-0123)",
                   ack.first_seq_no_held);
        return;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "EngineCatchUp: cannot account for what the venue is holding -- the order book region could not be used, and the WAL starts at seq_no={}, "
               "so the orders taken before it cannot be named. Asking the leading sequencer to halt trading, cancelling nothing (R-0123)",
               ack.first_seq_no_held);
    const bool timer_running = halt_awaited_;
    halt_awaited_ = true;
    links_.request_halt();
    if (!timer_running) {
        links_.start_halt_timer();
    }
}

void EngineCatchUp::stop_awaiting_halt() {
    if (!halt_awaited_) {
        return;
    }
    halt_awaited_ = false;
    links_.stop_halt_timer();
}

void EngineCatchUp::on_halt_tick(bool halted) {
    // A request lost to a change of sequencer leader is asked for again.
    if (halt_awaited_ && !halted) {
        links_.request_halt();
    }
}

} // namespaces
