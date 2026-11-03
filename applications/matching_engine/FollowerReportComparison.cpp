// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FollowerReportComparison.hpp"

#include <cstring>

namespace matching_engine {

FollowerReportComparison::FollowerReportComparison(FollowerReportComparisonLinksInterface& links, pubsub_itc_fw::QuillLogger& logger)
    : links_(links), logger_(logger) {}

void FollowerReportComparison::keep(HeldEngineReport report) {
    if (!has_room()) {
        if (!overflowed_) {
            overflowed_ = true;
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                       "FollowerReportComparison: following the WAL, {} reports are kept that the WAL does not yet hold the leader's of, which is as many as "
                       "are kept -- the leading engine has had no report written for that many commands. Later reports are not kept, so this instance can no "
                       "longer send every report the WAL lacks if it takes the lead",
                       kept_.size());
            refuse_to_lead(report.seq_no, pubsub_itc_fw_app::FollowerCannotLeadReason::KeptReportsFull, "it kept as many reports as it has room for");
        }
        return;
    }
    kept_.push_back(std::move(report));
}

void FollowerReportComparison::start_keeping_after(int64_t after_seq_no) {
    if (kept_.empty()) {
        keeping_from_ = after_seq_no + 1;
    }
}

void FollowerReportComparison::leader_report(int64_t record_seq_no, int64_t command, const uint8_t* bytes, size_t size, int64_t reports_in_wal_through) {
    if (record_seq_no <= last_record_accounted_) {
        return;
    }
    last_record_accounted_ = record_seq_no;
    // A sequencer that takes the lead writes the reports it kept while following, so a report the WAL holds already may
    // be written again. One of a command before every report still kept was accounted for when it first came: the
    // leading engine sends every report of a command before it applies the next, so no report of an earlier command
    // can arrive for the first time after a later command's.
    if (command < highest_command_compared_) {
        return;
    }
    highest_command_compared_ = command;
    // A report of a later command means the leader sent no more of the earlier ones: any of those this instance kept and
    // was not matched is one the leader did not send.
    while (!kept_.empty() && kept_.front().seq_no < command) {
        if (!kept_.front().compared) {
            report_difference(kept_.front().seq_no, pubsub_itc_fw_app::FollowerCannotLeadReason::ReportNotSent);
        }
        kept_.pop_front();
    }
    // The leader's reports of one command arrive in the order it produced them, as this instance's were kept.
    for (HeldEngineReport& kept : kept_) {
        if (kept.seq_no != command) {
            break;
        }
        if (kept.compared) {
            if (kept.payload.size() == size && std::memcmp(kept.payload.data(), bytes, size) == 0) {
                return; // a report compared already, written again
            }
            continue;
        }
        kept.compared = true;
        if (kept.payload.size() != size || std::memcmp(kept.payload.data(), bytes, size) != 0) {
            report_difference(command, pubsub_itc_fw_app::FollowerCannotLeadReason::ReportDiffers);
        }
        return;
    }
    // Nothing kept to compare with. Expected for a command whose reports this instance does not keep: one the WAL held
    // every report of when it began following, one before it began, or one after its kept reports were full.
    if (command > reports_in_wal_through && command >= keeping_from_ && !overflowed_) {
        report_difference(command, pubsub_itc_fw_app::FollowerCannotLeadReason::ReportNotProduced);
    }
}

void FollowerReportComparison::report_difference(int64_t command, pubsub_itc_fw_app::FollowerCannotLeadReason reason) {
    ++differences_;
    links_.difference_found();
    if (refused_to_lead_) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "FollowerReportComparison: following the WAL, another difference from the leader at command {}",
                   command);
        return;
    }
    const char* const what = reason == pubsub_itc_fw_app::FollowerCannotLeadReason::ReportDiffers ? "the leader's report of it differs from this instance's"
                             : reason == pubsub_itc_fw_app::FollowerCannotLeadReason::ReportNotProduced
                                 ? "the leader sent a report of it that this instance did not produce"
                                 : "this instance produced a report of it that the leader did not send";
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "FollowerReportComparison: following the WAL, command {}: {}. The two engines did not apply the same command alike, so this instance's book "
               "may differ from the one the members were told of",
               command, what);
    refuse_to_lead(command, reason, "its reports differ from the leader's");
}

void FollowerReportComparison::refuse_to_lead(int64_t command, pubsub_itc_fw_app::FollowerCannotLeadReason reason, const char* why) {
    if (refused_to_lead_) {
        return;
    }
    refused_to_lead_ = true;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "FollowerReportComparison: this instance does not lead, because {} -- it no longer asks for a lease, and goes on following, so that the "
               "venue has no standby it can trust until this instance is restarted. Its region is marked unusable, so that, restarted, it rebuilds "
               "its book from the WAL. The leader trades on (V11)",
               why);
    links_.stop_leading_for_good(command, reason);
}

} // namespaces
