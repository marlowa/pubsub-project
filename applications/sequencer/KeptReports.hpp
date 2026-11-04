#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <optional>
#include <vector>

#include <pubsub_itc_fw/QuillLogger.hpp>

#include "EngineReportPosition.hpp"
#include "KeptReportStore.hpp"

namespace sequencer {

/**
 * @brief What the sequencer thread supplies so that KeptReports can forward reports and tell the test.
 */
class KeptReportsLinksInterface {
  public:
    virtual ~KeptReportsLinksInterface() = default;

    /// Forwards a kept report, @p size bytes at @p bytes, the report's envelope as the engine sent it, with the number
    /// @p er_seq_no sent with it, as a report kept while this instance did not lead.
    virtual void forward_kept_report(int64_t er_seq_no, const uint8_t* bytes, size_t size) = 0;

    /// The gauge of kept reports lost because the store was full, now @p total.
    virtual void kept_reports_lost(int64_t total) = 0;

    /// The test event KeptReportsForwarded.
    virtual void kept_reports_forwarded(int64_t discarded_as_forwarded, int64_t count, int64_t bytes) = 0;
};

/**
 * @brief The reports from the matching engine kept while this instance does not lead, so that it can forward them if it
 *        takes the lead, and the discarding of those no longer needed
 *        (docs/availability/change_of_sequencer_leader.md, section 4.4;
 *        docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.16).
 *
 * A kept report is no longer needed when either:
 *
 * - it arrived more than a lease period plus the drift allowance before the last time this instance granted its leader
 *   a lease, because the leader forwarded everything it received in that time. Reports age only against that time, not
 *   against the clock: once the leader stops asking, because it has died, nothing more ages however long the change of
 *   leader takes; or
 * - a record the leader wrote at least reports_forwarded_delay before the latest record this instance holds says the
 *   leader had forwarded it. The leader's "forwarded" means handed to its connection to the gateway, not received
 *   there, and a report still in its send buffers when its machine fails is lost; hence the delay, by the leader's clock.
 *
 * On taking the lead, those the leader had forwarded are discarded, and the rest forwarded to their members, marked as
 * possible repeats.
 *
 * The store is sized for what a follower must hold when its leader dies: up to a renewal interval plus a lease period
 * plus the drift allowance of reports, 4.25 seconds with the usual timings. At about 34,000 orders a second, the highest
 * rate measured on this venue, and one and a half reports an order, that is about 212,000 reports. The store holds up to
 * 262,144, and 128 MiB of their bytes, allocated once.
 *
 * One thread, the sequencer's, calls every member function.
 */
class KeptReports {
  public:
    using Clock = std::chrono::steady_clock;

    static constexpr size_t capacity_bytes = 128U * 1024U * 1024U;
    static constexpr size_t capacity_reports = 262144;
    /// How long before the latest record a record saying how far the leader had forwarded must have been written.
    static constexpr std::chrono::milliseconds reports_forwarded_delay{100};
    /// How often, while not leading, the reports the leader forwarded are discarded.
    static constexpr std::chrono::milliseconds discard_interval{1000};

    /**
     * @param[in] lease_period    The lease period.
     * @param[in] drift_allowance The allowance for the drift between the two machines' clocks.
     */
    KeptReports(Clock::duration lease_period, Clock::duration drift_allowance, KeptReportsLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    /// This instance granted its leader's request to lead at @p at.
    void granted_lease_to_leader(Clock::time_point at) {
        last_granted_to_leader_ = at;
    }

    /// Keeps a report from the engine, received at @p now while this instance does not lead: @p size bytes at @p bytes,
    /// with the number @p er_seq_no sent with it, at @p position in the engine's reports if it says.
    void keep(int64_t er_seq_no, const uint8_t* bytes, size_t size, const std::optional<EngineReportPosition>& position, Clock::time_point now);

    /// A record the leader wrote at @p written_ns, by its clock, says it had forwarded every report through @p through,
    /// if it says.
    void leader_record_written(int64_t written_ns, const std::optional<EngineReportPosition>& through);

    /// Discards the kept reports the leader had forwarded at least reports_forwarded_delay before the latest record.
    void discard_those_the_leader_forwarded();

    /// A tick of the timer, at @p now, while this instance does not lead: discards those the leader forwarded, once a
    /// discard_interval.
    void tick(Clock::time_point now);

    /// This instance takes the lead: every kept report still needed is forwarded, and the store emptied.
    void forward_all();

    [[nodiscard]] size_t count() const {
        return store_.count();
    }
    [[nodiscard]] size_t lost() const {
        return store_.lost();
    }

  private:
    // How long before the last grant to the leader a kept report must have arrived to be no longer needed.
    [[nodiscard]] Clock::time_point needed_from() const;

    struct ForwardedSample {
        int64_t written_ns{0};
        EngineReportPosition through{};
    };
    static constexpr size_t forwarded_sample_capacity = 4096;

    const Clock::duration lease_period_;
    const Clock::duration drift_allowance_;
    KeptReportsLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    KeptReportStore store_{capacity_bytes, capacity_reports};
    Clock::time_point last_granted_to_leader_{};
    // The number of kept reports lost when that was last said, so that a loss is said when it happens rather than for
    // every report lost after it.
    int64_t lost_reported_{0};
    Clock::time_point discarded_at_{};
    int64_t discarded_as_forwarded_{0};
    // The positions the leader's records have said it forwarded through, oldest first, each when it moved.
    std::vector<ForwardedSample> samples_ = std::vector<ForwardedSample>(forwarded_sample_capacity);
    size_t samples_head_{0};
    size_t samples_count_{0};
    int64_t latest_record_written_ns_{0};
};

} // namespaces
