// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FollowerReportComparison.hpp"

#include <cerrno>
#include <cstring>

#include <sys/mman.h>

namespace matching_engine {

FollowerReportComparison::FollowerReportComparison(FollowerReportComparisonLinksInterface& links, pubsub_itc_fw::QuillLogger& logger, size_t most_reports,
                                                   size_t most_bytes)
    // Value-initialised, so every page is written now rather than on first use during the day.
    : links_(links), logger_(logger), bytes_(most_bytes), entries_(most_reports) {}

FollowerReportComparison::~FollowerReportComparison() {
    release_from_memory();
}

int FollowerReportComparison::hold_in_memory() {
    if (locked_ || (bytes_.empty() && entries_.empty())) {
        return 0;
    }
    if (!bytes_.empty() && ::mlock(bytes_.data(), bytes_.size()) != 0) {
        return errno;
    }
    if (!entries_.empty() && ::mlock(entries_.data(), entries_.size() * sizeof(KeptReport)) != 0) {
        const int error = errno;
        if (!bytes_.empty()) {
            ::munlock(bytes_.data(), bytes_.size());
        }
        return error;
    }
    locked_ = true;
    return 0;
}

void FollowerReportComparison::release_from_memory() {
    if (!locked_) {
        return;
    }
    if (!bytes_.empty()) {
        ::munlock(bytes_.data(), bytes_.size());
    }
    if (!entries_.empty()) {
        ::munlock(entries_.data(), entries_.size() * sizeof(KeptReport));
    }
    locked_ = false;
}

std::optional<size_t> FollowerReportComparison::place_for(size_t size) const {
    if (count_ == entries_.size()) {
        return std::nullopt;
    }
    if (count_ == 0) {
        return size <= bytes_.size() ? std::optional<size_t>(0) : std::nullopt;
    }
    const size_t oldest_offset = entries_[oldest_].offset;
    const KeptReport& newest = entries_[(oldest_ + count_ - 1) % entries_.size()];
    const size_t free_from = newest.offset + newest.size;
    if (newest.offset >= oldest_offset) {
        // The kept bytes run from the oldest report to the newest without passing the end of the block: there is room
        // after the newest, up to the end, and at the start, up to the oldest.
        if (size <= bytes_.size() - free_from) {
            return free_from;
        }
        if (size <= oldest_offset) {
            return 0;
        }
        return std::nullopt;
    }
    // The newer reports have been placed at the start of the block: the room is between the newest and the oldest.
    if (size <= oldest_offset - free_from) {
        return free_from;
    }
    return std::nullopt;
}

void FollowerReportComparison::drop_oldest() {
    kept_bytes_ -= entries_[oldest_].size;
    oldest_ = (oldest_ + 1) % entries_.size();
    --count_;
}

void FollowerReportComparison::store_full(int64_t produced_by, size_t size) {
    if (overflowed_) {
        return;
    }
    overflowed_ = true;
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
               "FollowerReportComparison: following the WAL, {} reports of {} bytes in all are kept that the WAL does not yet hold the leader's of, and "
               "there is no room for the next, of {} bytes: at most {} reports and {} bytes are kept. The leading engine's reports are that far behind its "
               "commands in the WAL. Later reports are not kept, so this instance can no longer send every report the WAL lacks if it takes the lead",
               count_, kept_bytes_, size, entries_.size(), bytes_.size());
    refuse_to_lead(produced_by, pubsub_itc_fw_app::FollowerCannotLeadReason::KeptReportsFull, "it kept as many reports as it has room for");
}

void FollowerReportComparison::start_keeping_after(int64_t after_seq_no) {
    if (count_ == 0) {
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
    while (count_ > 0 && entries_[oldest_].produced_by < command) {
        if (!entries_[oldest_].compared) {
            report_difference(entries_[oldest_].produced_by, pubsub_itc_fw_app::FollowerCannotLeadReason::ReportNotSent);
        }
        drop_oldest();
    }
    // The leader's reports of one command arrive in the order it produced them, as this instance's were kept.
    for (size_t position = 0; position < count_; ++position) {
        KeptReport& kept = entry_at(position);
        if (kept.produced_by != command) {
            break;
        }
        if (kept.compared) {
            if (kept.size == size && std::memcmp(bytes_.data() + kept.offset, bytes, size) == 0) {
                return; // a report compared already, written again
            }
            continue;
        }
        kept.compared = true;
        if (kept.size != size || std::memcmp(bytes_.data() + kept.offset, bytes, size) != 0) {
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
