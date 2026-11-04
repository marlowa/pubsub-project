// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "CommandChecks.hpp"

#include <limits>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

#include "GatewayIds.hpp"
#include "SequencerCommandIdentity.hpp"

namespace sequencer {

CommandChecks::CommandChecks(CommandChecksLinksInterface& links, pubsub_itc_fw::QuillLogger& logger) : links_(links), logger_(logger) {}

CommandChecks::Outcome CommandChecks::check(const pubsub_itc_fw::ConnectionID& came_by, const pubsub_itc_fw_app::WalRecordView& inbound,
                                            Clock::time_point now) {
    const bool sent_again = inbound.has_sent_again && inbound.sent_again;
    const std::optional<pubsub_itc_fw::CommandInWal> found = find(inbound, sent_again, now);
    if (!found.has_value()) {
        if (sent_again) {
            ++sent_again_sequenced_;
        }
        return Outcome::sequence_it;
    }
    if (sent_again) {
        // Its answer comes from the WAL or from the engine, by one of the routes in
        // docs/availability/commands_during_a_change_of_leader.md, section 3.6, and it is not sequenced twice.
        ++sent_again_already_in_wal_;
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug,
                   "CommandChecks: command ClOrdID={} from comp_id='{}' sent again -- the WAL already holds it as record {}, not sequencing it twice",
                   inbound.cl_ord_id, inbound.sender_comp_id, found->record_id);
        if (links_.answer_from_wal(inbound, *found)) {
            ++answered_from_wal_;
        }
        return Outcome::not_sequenced;
    }
    // A member using a ClOrdID it has used today: refused, and nothing is written
    // (docs/availability/one_check_against_sequencing_twice.md, section 4.1).
    ++refused_reused_identifier_;
    checked(now);
    // Info: the member's mistake, which the venue handles correctly.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "CommandChecks: refusing comp_id='{}' ClOrdID={} from gateway protocol={} instance={}: the WAL already holds a command with this ClOrdID, "
               "record {}, and this one is not marked as sent again -- nothing is written",
               inbound.sender_comp_id, inbound.cl_ord_id, inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent,
               inbound.has_gateway_instance_id ? inbound.gateway_instance_id : 0, found->record_id);
    links_.refuse(came_by, inbound, found->record_id);
    return Outcome::not_sequenced;
}

std::optional<pubsub_itc_fw::CommandInWal> CommandChecks::find(const pubsub_itc_fw_app::WalRecordView& inbound, bool sent_again, Clock::time_point now) {
    const size_t identity_size =
        write_command_identity(inbound.has_sender_comp_id ? inbound.sender_comp_id : std::string_view{},
                               inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent,
                               inbound.has_cl_ord_id ? inbound.cl_ord_id : std::string_view{}, identity_buffer_.data(), identity_buffer_.size());
    if (identity_size == 0) {
        // Both gateways put the comp id on every command and drop one without a ClOrdID, so one that does not say whose
        // it is means a gateway is not doing what the check relies on. It is sequenced unchecked.
        ++unchecked_without_identity_;
        if (unchecked_without_identity_ == 1) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                       "CommandChecks: a command from gateway protocol={} instance={} has no comp id or no ClOrdID, so it cannot be checked against the "
                       "WAL -- it is sequenced unchecked, and so is any later one, counted in the summary of checks",
                       inbound.has_origin_gateway_id ? inbound.origin_gateway_id : gateway_ids::default_when_absent,
                       inbound.has_gateway_instance_id ? inbound.gateway_instance_id : 0);
        }
        return std::nullopt;
    }
    // Commands sent again and refusals are what the summary reports; every other command is checked too, and counted
    // only in the WAL's own counts.
    if (sent_again) {
        checked(now);
    }
    // A command sent again was first sent no earlier than its gateway first received it, which bounds any search of the
    // WAL. A command not marked may reuse a ClOrdID from any time today, so a search may read the whole WAL.
    const int64_t first_sent_no_earlier_than = sent_again && inbound.has_gateway_ingress_ns ? inbound.gateway_ingress_ns : std::numeric_limits<int64_t>::min();
    return links_.find_command(std::string_view(identity_buffer_.data(), identity_size), first_sent_no_earlier_than);
}

void CommandChecks::checked(Clock::time_point now) {
    checked_since_summary_ = true;
    last_checked_at_ = now;
}

void CommandChecks::tick(Clock::time_point now) {
    if (checked_since_summary_ && now - last_checked_at_ >= summary_idle_limit) {
        summarise("no command has been checked for a minute");
    }
}

void CommandChecks::stopped_leading() {
    if (checked_since_summary_) {
        summarise("this instance has stopped leading");
    }
}

void CommandChecks::summarise(const char* reason) {
    const int64_t searched_records = links_.discard_command_search();
    const pubsub_itc_fw::ReplicatedWal::CommandCheckCounts counts = links_.command_check_counts();
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "CommandChecks: since the last summary of checks, {} command(s) sent again were already in the WAL and not sequenced twice, {} of "
               "them answered by a report the WAL held, {} sent again were sequenced as new, and {} reused a ClOrdID and were refused. Since this instance "
               "started, the WAL has settled {} checked command(s) by the record of identifiers alone, proved {} by reading the one record at its "
               "position, and searched for {}, {} of them because that record was another command; {} command(s) could not be checked because they did "
               "not say whose they were; {} WAL record(s) read by the search now ended -- {}",
               sent_again_already_in_wal_, answered_from_wal_, sent_again_sequenced_, refused_reused_identifier_, counts.not_held_by_number,
               counts.proved_by_reading, counts.searched, counts.other_command_at_position, unchecked_without_identity_, searched_records, reason);
    checked_since_summary_ = false;
    sent_again_already_in_wal_ = 0;
    answered_from_wal_ = 0;
    sent_again_sequenced_ = 0;
    refused_reused_identifier_ = 0;
}

} // namespaces
