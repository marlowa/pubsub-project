// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MemberSessions.hpp"

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace sequencer {

const fix_common::SessionDestination* MemberSessions::destination(const fix_common::SessionIdentity& identity) const {
    const auto it = destinations_.find(identity);
    return it == destinations_.end() ? nullptr : &it->second;
}

const MemberSessions::SequenceState* MemberSessions::sequence_state(const fix_common::SessionIdentity& identity) const {
    const auto it = sequence_states_.find(identity);
    return it == sequence_states_.end() ? nullptr : &it->second;
}

MemberSessions::Bound MemberSessions::bind(const fix_common::SessionIdentity& identity, const fix_common::SessionDestination& destination,
                                           bool still_connected) {
    Bound bound{};
    bound.still_connected = still_connected;
    // Connected again, so the next report dropped for it, if any, is logged as a Warning again.
    sessions_with_reports_dropped_.erase(identity);
    const auto existing = destinations_.find(identity);
    // A session established before, announced again because the gateway's link to this instance has come up: this
    // instance may have restarted, and then knows of no session. Only where it is connected is recorded. Its numbering
    // is settled, so there is nothing to send back; and it is not a logon, so a binding already held is not a sign that
    // the previous connection died, and the number the session resumes from is left alone.
    if (still_connected) {
        if (existing == destinations_.end()) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "MemberSessions: session comp_id='{}' protocol={} announced again, still connected to instance={} connection={} -- recorded",
                       identity.comp_id_view(), identity.protocol, destination.instance, destination.conn_id);
        } else if (existing->second.instance != destination.instance || existing->second.conn_id != destination.conn_id) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "MemberSessions: session comp_id='{}' protocol={} announced again, still connected to instance={} connection={} -- it was bound to "
                       "instance={} connection={}, now replaced",
                       identity.comp_id_view(), identity.protocol, destination.instance, destination.conn_id, existing->second.instance,
                       existing->second.conn_id);
        }
        destinations_[identity] = destination;
        links_.session_announced_again(identity, destination);
        return bound;
    }
    // A binding still present means the previous session never unbound -- its gateway died without saying so. That is
    // the only signal the sequencer gets that the position it remembers for this session is stale rather than exact, and
    // it is what decides whether the resume figure is used as-is or deliberately raised.
    bound.previous_session_died = existing != destinations_.end();
    if (existing != destinations_.end()) {
        // The same identity was already bound somewhere. On a reconnect this is the expected case and the whole point --
        // the address is replaced and the session's reports follow it. But it is also what a comp id logged on twice at
        // once looks like, and that is a venue rule violation ("one comp id may hold a session only once venue-wide")
        // which nothing yet enforces. The sequencer is the only component that can see it happen, so it says so.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "MemberSessions: session comp_id='{}' protocol={} re-bound from instance={} connection={} to instance={} connection={}",
                   identity.comp_id_view(), identity.protocol, existing->second.instance, existing->second.conn_id, destination.instance, destination.conn_id);
    } else {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MemberSessions: session comp_id='{}' protocol={} bound to instance={} connection={}",
                   identity.comp_id_view(), identity.protocol, destination.instance, destination.conn_id);
    }
    destinations_[identity] = destination;
    return bound;
}

std::optional<MemberSessions::Resume> MemberSessions::resume(const fix_common::SessionIdentity& identity, bool reset_seq_nums, bool previous_session_died,
                                                             bool leading, int64_t wal_last_seq_no) {
    // Unless the member asked to start again, in which case there is nothing to hand back and keeping it would be worse
    // than useless. ResetSeqNumFlag=Y on a Logon discards the numbering on both sides, so everything remembered here
    // describes a series the member has abandoned: both sequence numbers, and the record of which outbound numbers held
    // execution reports. Kept, the numbers would be stuck, because the updates that follow report the new low numbers and
    // the guards here refuse to lower; and new ranges from the restarted numbering would be merged into the old ones,
    // which is BUG-0051 arriving by a different road. A member may reset at any logon, so this is an ordinary path.
    if (reset_seq_nums) {
        const size_t forgotten = sequence_states_.erase(identity);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "MemberSessions: session comp_id='{}' protocol={} asked to restart its numbering -- {} remembered sequence state discarded",
                   identity.comp_id_view(), identity.protocol, forgotten > 0 ? "its" : "no");
    }
    // Hand back what the venue remembers of this session's sequence numbers, so the gateway that has just taken it on
    // continues where the last one left off instead of starting at 1.
    const auto state_it = sequence_states_.find(identity);
    const bool known = state_it != sequence_states_.end();
    const SequenceState state = known ? state_it->second : SequenceState{};
    // Only the leader answers. The follower tracks bindings too -- it will need them if it is promoted -- but a session
    // has one venue-side view of its numbering, and two replies would have the gateway apply it twice.
    if (!leading) {
        return std::nullopt;
    }
    // Where to resume the member's numbering. After a clean unbind the reported figure is exact and is used as it stands.
    // After an unclean death it is stale by whatever the gateway sent between its last report and its last breath, so
    // the known part of that -- the reports this sequencer forwarded -- is added, plus an allowance for the admin traffic
    // it cannot see. The result is deliberately too high rather than risk being too low.
    Resume resumed{};
    resumed.known = known;
    resumed.outbound_seq_num = state.outbound_seq_num;
    if (known && previous_session_died) {
        resumed.outbound_seq_num += state.ers_since_report + unclean_resume_admin_allowance;
    }
    // The inbound number gets no such allowance: erring high means the venue expects a number the member has not reached
    // and will treat its next message as having gone backwards. The stored figure is already a lower bound.
    resumed.inbound_seq_num = state.inbound_seq_num;
    // And which of the session's numbers held a report, which a gateway cannot work out for itself.
    resumed.report_seq_nums = state.report_seq_nums;
    // The resumed session starts from the figure handed out, so the running count that fed into it has been spent.
    if (known) {
        SequenceState& stored = sequence_states_[identity];
        stored.outbound_seq_num = resumed.outbound_seq_num;
        stored.ers_since_report = 0;
        // Reports logged from here on are forwarded live to the new binding; those logged before, after the last one the
        // member was delivered, are what its gateway asks for once the session is established.
        stored.bound_at_log_seq_no = wal_last_seq_no;
        stored.delivered_figure_may_be_behind = previous_session_died;
    }
    if (known && previous_session_died) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "MemberSessions: session comp_id='{}' protocol={} previous gateway died without reporting -- resuming at outbound={} (reported {} + {} "
                   "report(s) forwarded since + {} allowance), deliberately ahead so the member sees a gap rather than a fatal low sequence; inbound={} as "
                   "reported, deliberately NOT ahead so the member is not treated as having gone backwards",
                   identity.comp_id_view(), identity.protocol, resumed.outbound_seq_num, state.outbound_seq_num, state.ers_since_report,
                   unclean_resume_admin_allowance, resumed.inbound_seq_num);
    } else {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "MemberSessions: session comp_id='{}' protocol={} sequence state {} -- outbound={} inbound={}",
                   identity.comp_id_view(), identity.protocol, known ? "restored" : "is new", resumed.outbound_seq_num, resumed.inbound_seq_num);
    }
    return resumed;
}

void MemberSessions::unbind(const fix_common::SessionIdentity& identity, int16_t instance, int32_t connection, const Reported& reported) {
    const auto existing = destinations_.find(identity);
    if (existing == destinations_.end()) {
        return;
    }
    // Only the connection that is actually bound may unbind the session. A member that reconnects quickly enough for its
    // new SessionBound to arrive before the old connection's SessionUnbound -- two gateways racing, which is precisely
    // what a failover produces -- would otherwise be unbound a moment after binding.
    if (existing->second.instance != instance || existing->second.conn_id != connection) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "MemberSessions: stale SessionUnbound for comp_id='{}' protocol={} naming instance={} connection={} -- it is bound to instance={} "
                   "connection={}, keeping the newer binding",
                   identity.comp_id_view(), identity.protocol, instance, connection, existing->second.instance, existing->second.conn_id);
        return;
    }
    destinations_.erase(existing);
    // Keep the sequence numbers the departing gateway reports. The destination is gone, but the session is not: this is
    // what the next gateway to take it on will be handed.
    SequenceState& state = sequence_states_[identity];
    state.outbound_seq_num = reported.outbound_seq_num;
    if (reported.inbound_seq_num > state.inbound_seq_num) {
        state.inbound_seq_num = reported.inbound_seq_num;
    }
    // And which of its numbers held a report, for the part the departing gateway had not already reported.
    fix_common::seq_num_ranges::merge(state.report_seq_nums, reported.report_seq_nums);
    fix_common::seq_num_ranges::trim(state.report_seq_nums, fix_common::seq_num_ranges::max_remembered);
    // How far the member was served, exactly: the reports logged for it after this one, while it is away, are delivered
    // when it binds again.
    if (reported.has_last_report_delivered && reported.last_report_delivered > state.last_report_delivered) {
        state.last_report_delivered = reported.last_report_delivered;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "MemberSessions: session comp_id='{}' protocol={} unbound from instance={} connection={} -- remembered outbound={} and {} report range(s); "
               "its reports have nowhere to go until it binds again",
               identity.comp_id_view(), identity.protocol, instance, connection, state.outbound_seq_num, state.report_seq_nums.size());
    links_.session_unbound(identity, instance, connection);
}

void MemberSessions::sequence_update(const fix_common::SessionIdentity& identity, const Reported& reported) {
    SequenceState& state = sequence_states_[identity];
    // Never lowered. Reports can arrive out of order across a reconnect -- a late one from the instance that has just lost
    // the session would otherwise wind the position backwards, and backwards is the direction that kills the session.
    if (reported.outbound_seq_num > state.outbound_seq_num) {
        state.outbound_seq_num = reported.outbound_seq_num;
        // The reported figure now accounts for everything sent so far, so the running count starts again.
        state.ers_since_report = 0;
    }
    if (reported.has_last_report_delivered && reported.last_report_delivered > state.last_report_delivered) {
        state.last_report_delivered = reported.last_report_delivered;
    }
    // Tracked independently of the number above, because the two move for different reasons. Never lowered.
    if (reported.inbound_seq_num > state.inbound_seq_num) {
        state.inbound_seq_num = reported.inbound_seq_num;
    }
    // Merged unconditionally: the ranges describe numbers already sent, so a late update still carries facts about numbers
    // that really did hold reports, and merging cannot wind anything backwards.
    fix_common::seq_num_ranges::merge(state.report_seq_nums, reported.report_seq_nums);
    fix_common::seq_num_ranges::trim(state.report_seq_nums, fix_common::seq_num_ranges::max_remembered);
}

std::optional<MemberSessions::Undelivered> MemberSessions::take_undelivered(const fix_common::SessionIdentity& identity) {
    const auto state_it = sequence_states_.find(identity);
    const fix_common::SessionDestination* bound_to = destination(identity);
    if (state_it == sequence_states_.end() || bound_to == nullptr || state_it->second.bound_at_log_seq_no == 0) {
        return std::nullopt;
    }
    SequenceState& state = state_it->second;
    Undelivered undelivered{state.last_report_delivered, state.bound_at_log_seq_no, state.delivered_figure_may_be_behind, *bound_to};
    // Answered once for each binding.
    state.bound_at_log_seq_no = 0;
    if (undelivered.through <= undelivered.after) {
        return std::nullopt;
    }
    return undelivered;
}

void MemberSessions::report_forwarded(const fix_common::SessionIdentity& identity) {
    if (identity.empty()) {
        return;
    }
    // Deliberately counts what was sent, not what was acknowledged. A report handed to a gateway that then dies still
    // consumed a sequence number there, and counting it makes the resume position too high rather than too low.
    ++sequence_states_[identity].ers_since_report;
}

void MemberSessions::report_dropped_unbound(const fix_common::SessionIdentity& identity, int64_t er_seq_no) {
    ++reports_dropped_unbound_;
    links_.reports_dropped_unbound(reports_dropped_unbound_);
    // A test sends commands under such a comp id straight to this instance, with no gateway holding a session for it.
    if (links_.expects_no_session_for(identity.comp_id_view())) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug,
                   "MemberSessions: ER seq_no={} for comp_id='{}' protocol={} dropped -- the test expects no session for it", er_seq_no,
                   identity.comp_id_view(), identity.protocol);
        return;
    }
    // A member that is away is told of these reports when it logs on again, so a dropped report is not always a fault. A
    // member that is connected, and whose gateway has not told this instance so, is never told of them. The Warning names
    // the session once, on its first report dropped since it was last connected; the count says how many.
    if (sessions_with_reports_dropped_.insert(identity).second) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "MemberSessions: ER seq_no={} for session comp_id='{}' protocol={} dropped -- the session is connected to no gateway that has told this "
                   "instance so. Its later reports are dropped too, logged at Debug, until it is connected again; {} report(s) dropped for this reason in all",
                   er_seq_no, identity.comp_id_view(), identity.protocol, reports_dropped_unbound_);
        links_.report_dropped(identity, er_seq_no);
    } else {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug,
                   "MemberSessions: ER seq_no={} for session comp_id='{}' protocol={} -- session not bound to any instance, dropping", er_seq_no,
                   identity.comp_id_view(), identity.protocol);
    }
}

} // namespaces
