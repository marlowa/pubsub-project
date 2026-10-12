#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <string>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace fix_common {

/**
 * @brief The timings of the lease rules that decide which instance of a pair leads.
 *
 * Every voter and every instance holding a lease must use the same values, which is why each
 * component's configuration expands them from the environment's [shared] section rather than
 * setting its own. See docs/availability/majority_leases.md.
 */
class LeaseTiming {
  public:
    /**
     * @brief How often an instance applies the lease rules: renewing, and noticing that its lease has run out.
     *
     * An instance therefore goes on acting as leader for up to one tick after its own count of the
     * lease has run out, which is why the drift allowance must be larger than one tick.
     */
    static constexpr std::chrono::milliseconds tick_interval{100};

    /// How long a grant lasts, and how long a voter that has just started grants nothing.
    std::chrono::milliseconds period{3000};

    /// How much shorter than the period an instance takes a lease it holds to be.
    std::chrono::milliseconds drift_allowance{250};

    /// How often a leader asks for its lease to be renewed.
    std::chrono::milliseconds renewal_interval{1000};

    /**
     * @brief Read the [lease] section of a component's configuration and check that the values make sense together.
     *
     * A leader must be able to renew at least twice within the part of the lease it relies on,
     * so that a single lost renewal does not end its leadership.
     */
    static LeaseTiming load(const pubsub_itc_fw::TomlConfiguration& toml, const std::string& loader_name) {
        int32_t period_milliseconds = 0;
        int32_t drift_allowance_milliseconds = 0;
        int32_t renewal_interval_milliseconds = 0;
        toml.get_required_except("lease.period_milliseconds", period_milliseconds);
        toml.get_required_except("lease.drift_allowance_milliseconds", drift_allowance_milliseconds);
        toml.get_required_except("lease.renewal_interval_milliseconds", renewal_interval_milliseconds);

        if (period_milliseconds <= 0) {
            throw pubsub_itc_fw::ConfigurationException(loader_name + ": lease.period_milliseconds must be positive");
        }
        const int64_t minimum_drift_allowance_milliseconds = 2 * tick_interval.count();
        if (drift_allowance_milliseconds < minimum_drift_allowance_milliseconds || drift_allowance_milliseconds >= period_milliseconds) {
            throw pubsub_itc_fw::ConfigurationException(loader_name + ": lease.drift_allowance_milliseconds must be at least " +
                                                        std::to_string(minimum_drift_allowance_milliseconds) +
                                                        ", two ticks of the lease rules, and less than the period");
        }
        if (renewal_interval_milliseconds <= 0 || 2 * renewal_interval_milliseconds > period_milliseconds - drift_allowance_milliseconds) {
            throw pubsub_itc_fw::ConfigurationException(loader_name +
                                                        ": lease.renewal_interval_milliseconds must be positive and at most half of the period less "
                                                        "the drift allowance, so that one lost renewal does not end a leadership");
        }

        LeaseTiming timing;
        timing.period = std::chrono::milliseconds{period_milliseconds};
        timing.drift_allowance = std::chrono::milliseconds{drift_allowance_milliseconds};
        timing.renewal_interval = std::chrono::milliseconds{renewal_interval_milliseconds};
        return timing;
    }
};

} // namespaces
