#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <cstdint> // IWYU pragma: keep
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include <fmt/format.h>

#include <pubsub_itc_fw/EpochStore.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/GaugeHandle.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Recording an instance's epoch on disk, timed, and saying so when the disk is too slow for the lease
 *        (docs/bug_list.md, BUG-0228).
 *
 * When an instance moves to a new generation it records the epoch on disk, with EpochStore, before it acts on the
 * generation. That waits for the disk, on the application's own thread, and the lease the instance has just been
 * granted keeps running meanwhile. On a disk that is busy the wait can be longer than the lease, and the instance stops
 * leading as it starts. This class does the recording for the sequencer's replicated WAL and for the matching engine
 * alike, and makes that visible:
 *
 * - every record of an epoch is timed, into a histogram if one is given;
 * - a record that takes more than a quarter of the lease is logged at Warning, naming the file, the time it took, the
 *   lease and the setting, even when the lease survives it;
 * - when the instance takes the lead, the lease left is recorded, into a second histogram if one is given;
 * - when the instance then stops leading, and the record of the epoch before taking the lead took more than a quarter
 *   of the lease, that is logged at Error as the probable cause.
 *
 * Only durations are compared, never two readings of different clocks, so it gives the same answers whichever clock
 * the caller keeps the lease by. One thread, the application's, calls every member function, which makes it the one
 * writer of each histogram.
 */
class EpochRecording {
  public:
    using Clock = std::chrono::steady_clock;

    /// A record of the epoch that takes more than the lease divided by this is warned of.
    static constexpr int lease_divided_by_for_warning = 4;

    /**
     * @param[in] path       The file to hold the epoch.
     * @param[in] owner_name How the owner is named in the Quill log.
     * @param[in] logger     Where the warnings and the result of the disk check are logged.
     */
    EpochRecording(std::string path, std::string owner_name, QuillLogger& logger)
        : store_(std::move(path)), owner_name_(std::move(owner_name)), logger_(logger) {}

    /// Where to record how long each record of the epoch takes, and the lease left on taking the lead, in milliseconds.
    /// Left as constructed, nothing is recorded.
    void measure_into(HistogramHandle record_milliseconds, HistogramHandle lease_left_milliseconds) {
        record_milliseconds_ = record_milliseconds;
        lease_left_milliseconds_ = lease_left_milliseconds;
    }

    /// The epoch on disk, or 0 if there is none (EpochStore::load).
    [[nodiscard]] int32_t load() const {
        return store_.load();
    }

    /// The file that holds the epoch.
    [[nodiscard]] const std::string& path() const {
        return store_.path();
    }

    /**
     * @brief Records @p epoch on disk, timed, and warns if it took more than a quarter of @p lease_period.
     * @return Whether it reached the disk (EpochStore::store).
     */
    bool record(int32_t epoch, std::chrono::milliseconds lease_period) {
        const Clock::time_point began = Clock::now();
        const bool recorded = store_.store(epoch);
        last_record_took_ = Clock::now() - began;
        const double milliseconds = std::chrono::duration<double, std::milli>(last_record_took_).count();
        record_milliseconds_.observe(milliseconds);
        if (too_slow_for(last_record_took_, lease_period)) {
            PUBSUB_LOG(logger_, FwLogLevel::Warning,
                       "{}: recording epoch {} in {} took {:.0f} ms, more than a quarter of the {} ms lease -- the disk is slow to sync, and a "
                       "record that takes longer than the lease costs this instance the lead as it takes it. See [lease] period_milliseconds and "
                       "BUG-0228",
                       owner_name_, epoch, store_.path(), milliseconds, lease_period.count());
        }
        return recorded;
    }

    /**
     * @brief This instance acts on taking the lead at @p now, with its lease running out at @p lease_expires_at: the lease
     *        left is recorded, and the time the last record of the epoch took is kept, to explain stopping leading
     *        soon after.
     */
    void acted_on_taking_the_lead(Clock::time_point lease_expires_at, Clock::time_point now) {
        const double left = std::chrono::duration<double, std::milli>(lease_expires_at - now).count();
        lease_left_milliseconds_.observe(left > 0.0 ? left : 0.0);
        record_before_leading_took_ = last_record_took_;
    }

    /**
     * @brief This instance has stopped leading. If the record of the epoch before it took the lead took more than a quarter
     *        of @p lease_period, that is the probable cause, and is said at Error.
     * @return Whether it was said.
     */
    bool stopped_leading(std::chrono::milliseconds lease_period) {
        const Clock::duration took = record_before_leading_took_;
        record_before_leading_took_ = Clock::duration::zero();
        if (!too_slow_for(took, lease_period)) {
            return false;
        }
        PUBSUB_LOG(logger_, FwLogLevel::Error,
                   "{}: this instance stopped leading, and recording its epoch in {} before it acted on the lead took {:.0f} ms of the {} ms "
                   "lease, which is the probable cause -- the disk holding it is too slow to sync within the lease. Move the file to a faster "
                   "disk, or lengthen [lease] period_milliseconds for every voter (BUG-0228)",
                   owner_name_, store_.path(), std::chrono::duration<double, std::milli>(took).count(), lease_period.count());
        return true;
    }

    /// What check_disk found: how many trial records it made, and the median and the longest, in milliseconds.
    struct DiskCheck {
        int records{0};
        double median_milliseconds{0.0};
        double worst_milliseconds{0.0};
    };

    /// The most trial records check_disk makes, and the longest it spends making them.
    static constexpr int disk_check_records = 20;
    static constexpr std::chrono::milliseconds disk_check_time_limit{2000};

    /**
     * @brief Measures, at start-up, how long the disk takes to record an epoch, by recording a trial one in a file of its
     *        own beside the real one, as the real one is recorded, up to disk_check_records times or for
     *        disk_check_time_limit, whichever comes first. The trial file is removed afterwards.
     *
     * The median and the longest are logged at Info, and at Warning if the longest is more than a quarter of
     * @p lease_period; the longest is set on @p worst_milliseconds_gauge. A disk that is always slow is found this way;
     * one that is slow only under load, as a disk with a small fast cache is, is found by record() when it happens.
     * With no file for the epoch, nothing is checked, written or logged.
     */
    DiskCheck check_disk(std::chrono::milliseconds lease_period, GaugeHandle worst_milliseconds_gauge) {
        // With no file to hold the epoch there is no disk to check, and a trial file would go wherever the process is.
        if (store_.path().empty()) {
            return DiskCheck{};
        }
        const std::string trial_path = store_.path() + ".disk_check";
        const EpochStore trial(trial_path);
        std::vector<double> milliseconds;
        const Clock::time_point began = Clock::now();
        while (static_cast<int>(milliseconds.size()) < disk_check_records && Clock::now() - began < disk_check_time_limit) {
            const Clock::time_point one_began = Clock::now();
            if (!trial.store(static_cast<int32_t>(milliseconds.size()) + 1)) {
                break;
            }
            milliseconds.push_back(std::chrono::duration<double, std::milli>(Clock::now() - one_began).count());
        }
        ::unlink(trial_path.c_str());
        DiskCheck found{};
        if (milliseconds.empty()) {
            PUBSUB_LOG(logger_, FwLogLevel::Warning, "{}: could not record a trial epoch in {}, so how fast the disk syncs is not known", owner_name_,
                       trial_path);
            return found;
        }
        std::sort(milliseconds.begin(), milliseconds.end());
        found.records = static_cast<int>(milliseconds.size());
        found.median_milliseconds = milliseconds[milliseconds.size() / 2];
        found.worst_milliseconds = milliseconds.back();
        worst_milliseconds_gauge.set(found.worst_milliseconds);
        const bool too_slow = found.worst_milliseconds * lease_divided_by_for_warning > static_cast<double>(lease_period.count());
        PUBSUB_LOG(logger_, too_slow ? FwLogLevel::Warning : FwLogLevel::Info,
                   "{}: recording an epoch in {} took {:.1f} ms at the median and {:.1f} ms at the longest, over {} trial record(s){}", owner_name_,
                   store_.path(), found.median_milliseconds, found.worst_milliseconds, found.records,
                   too_slow ? fmt::format(" -- more than a quarter of the {} ms lease: the disk is too slow to sync within it reliably, and an instance "
                                          "may stop leading as it takes the lead. Move the file to a faster disk, or lengthen [lease] period_milliseconds "
                                          "for every voter (BUG-0228)",
                                          lease_period.count())
                            : std::string{});
        return found;
    }

    /// How long the last record of the epoch took.
    [[nodiscard]] Clock::duration last_record_took() const {
        return last_record_took_;
    }

  private:
    [[nodiscard]] static bool too_slow_for(Clock::duration took, std::chrono::milliseconds lease_period) {
        return took * lease_divided_by_for_warning > lease_period;
    }

    EpochStore store_;
    std::string owner_name_;
    QuillLogger& logger_;
    HistogramHandle record_milliseconds_{};
    HistogramHandle lease_left_milliseconds_{};
    Clock::duration last_record_took_{};
    // How long the record of the epoch took before this instance last took the lead; zero once it has been used to
    // explain stopping leading.
    Clock::duration record_before_leading_took_{};
};

} // namespaces
