// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "KeptReports.hpp"

#include <algorithm>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace sequencer {

KeptReports::KeptReports(Clock::duration lease_period, Clock::duration drift_allowance, KeptReportsLinksInterface& links, pubsub_itc_fw::QuillLogger& logger)
    : lease_period_(lease_period), drift_allowance_(drift_allowance), links_(links), logger_(logger) {}

KeptReports::Clock::time_point KeptReports::needed_from() const {
    // Before this instance has granted any request, no leader has been seen forwarding anything, so every kept report may
    // be needed.
    if (last_granted_to_leader_ == Clock::time_point{}) {
        return Clock::time_point::min();
    }
    return last_granted_to_leader_ - lease_period_ - drift_allowance_;
}

void KeptReports::keep(int64_t er_seq_no, const uint8_t* bytes, size_t size, const std::optional<EngineReportPosition>& position, Clock::time_point now) {
    const Clock::time_point from = needed_from();
    store_.discard_without_position_received_before(from);
    store_.keep(er_seq_no, bytes, size, now, from, position);
    const auto lost_now = static_cast<int64_t>(store_.lost());
    if (lost_now != lost_reported_) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "KeptReports: the store of reports kept in case this instance takes the lead is full, and {} report(s) still needed have been "
                   "overwritten. If this instance takes the lead now, those reports reach their members only if the leader forwarded them before it "
                   "stopped",
                   lost_now - lost_reported_);
        lost_reported_ = lost_now;
        links_.kept_reports_lost(lost_now);
    }
}

void KeptReports::leader_record_written(int64_t written_ns, const std::optional<EngineReportPosition>& through) {
    latest_record_written_ns_ = std::max(latest_record_written_ns_, written_ns);
    if (!through.has_value()) {
        return;
    }
    // Only a change is worth keeping: the samples say when the leader's position moved.
    if (samples_count_ > 0) {
        const ForwardedSample& newest = samples_[(samples_head_ + samples_count_ - 1) % forwarded_sample_capacity];
        if (!through->is_after(newest.through)) {
            return;
        }
    }
    if (samples_count_ == forwarded_sample_capacity) {
        samples_head_ = (samples_head_ + 1) % forwarded_sample_capacity;
        --samples_count_;
    }
    samples_[(samples_head_ + samples_count_) % forwarded_sample_capacity] = ForwardedSample{written_ns, *through};
    ++samples_count_;
}

void KeptReports::discard_those_the_leader_forwarded() {
    const int64_t cutoff_ns = latest_record_written_ns_ - std::chrono::duration_cast<std::chrono::nanoseconds>(reports_forwarded_delay).count();
    // The newest sample written at or before the cutoff is the one that applies; older ones are no longer needed.
    while (samples_count_ >= 2 && samples_[(samples_head_ + 1) % forwarded_sample_capacity].written_ns <= cutoff_ns) {
        samples_head_ = (samples_head_ + 1) % forwarded_sample_capacity;
        --samples_count_;
    }
    if (samples_count_ == 0 || samples_[samples_head_].written_ns > cutoff_ns) {
        return;
    }
    discarded_as_forwarded_ += static_cast<int64_t>(store_.discard_through(samples_[samples_head_].through));
}

void KeptReports::tick(Clock::time_point now) {
    if (now - discarded_at_ >= discard_interval) {
        discarded_at_ = now;
        discard_those_the_leader_forwarded();
    }
}

void KeptReports::forward_all() {
    store_.discard_without_position_received_before(needed_from());
    // Those the old leader said it forwarded were discarded just before this, on taking the lead; this says how many.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "KeptReports: taking the lead -- {} kept report(s) discarded because the old leader said it had "
               "forwarded them",
               discarded_as_forwarded_);
    const size_t count = store_.count();
    const size_t bytes = store_.bytes_used();
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "KeptReports: taking the lead -- forwarding {} kept report(s) ({} bytes), each marked as a possible repeat", count, bytes);
    links_.kept_reports_forwarded(discarded_as_forwarded_, static_cast<int64_t>(count), static_cast<int64_t>(bytes));
    discarded_as_forwarded_ = 0;
    store_.take_all([this](int64_t er_seq_no, const uint8_t* report, size_t size) { links_.forward_kept_report(er_seq_no, report, size); });
}

} // namespaces
