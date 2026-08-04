#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cmath>
#include <cstddef>
#include <vector>

#include <fmt/format.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace gateway_metrics {

/**
 * @brief Name of the order round-trip histogram, registered by every order gateway.
 *
 * Measured from the moment the gateway took the client's order in hand to the moment it
 * starts sending the acknowledging ExecutionReport back. It deliberately excludes the time
 * the ER spends in the kernel and on the wire to the client, which the gateway cannot
 * observe and which is not what the venue is answerable for.
 *
 * **One name for every protocol, told apart by the component label**, which is the process
 * instance -- "fix_order_gateway_a" against "binary_order_gateway_a". This is the case
 * docs/design/metrics.md builds its registration rules around: one family, several
 * labelled children. Comparing the two protocols is then one query grouped by component
 * rather than two queries stitched together, and a third protocol would cost a label value
 * rather than a new metric, a new panel and a new alert.
 *
 * The trap that comes with it: a query written without `by (component)` blends the
 * protocols into one number that describes neither. That is true of every metric here --
 * all of them are per-instance -- so it is a querying habit, not a reason to fold the
 * protocol into the name.
 */
inline constexpr const char* order_round_trip_metric_name = "order_round_trip_nanoseconds";

/**
 * @brief Help text for the family.
 *
 * One literal shared by both gateways, because Prometheus permits a single help string per
 * family and PrometheusEndpoint raises PreconditionAssertion on a second registration whose
 * help differs. It is therefore worded for both protocols, naming neither.
 */
inline constexpr const char* order_round_trip_help = "Nanoseconds from taking an order off the client connection to starting to send its ExecutionReport";

/** @brief Configuration path holding the bucket bounds, identical in every gateway file. */
inline constexpr const char* order_round_trip_buckets_key = "metrics.order_round_trip_buckets";

/**
 * @brief Reads and validates the round-trip histogram's bucket bounds.
 *
 * @param[in] toml The gateway's parsed configuration.
 * @return Upper bounds in nanoseconds, ascending.
 *
 * Raises ConfigurationException if the key is missing, is not an array of numbers, is
 * empty, is not strictly ascending, or names a non-finite bound. Prometheus requires
 * ascending bounds and silently misbehaves given anything else, so this is checked at load
 * time where the operator gets a message naming the key rather than a dashboard that
 * quietly reads wrong months later.
 *
 * **The bounds must not include an infinite top bound.** prometheus-cpp allocates one more
 * counter than there are boundaries and renders it as `le="+Inf"` itself, so an infinity
 * written here produces two of them. Nothing above the top bound is lost by leaving it out:
 * such an observation lands in that automatic bucket and still counts towards `_sum` and
 * `_count`, so the mean and the total stay exact. What degrades is quantile resolution,
 * since histogram_quantile cannot interpolate within an unbounded bucket -- which is the
 * reason the top bound belongs well above the range actually being served, not the reason
 * to try to cap it.
 *
 * **Both gateways must be configured with the same bounds.** The metric exists to compare
 * the ASCII FIX gateway against the binary one, and histograms with different boundaries
 * cannot be compared or aggregated -- a percentile drawn across two such series is not so
 * much wrong as meaningless. That is why the value comes from a single shared placeholder
 * expanded into both files rather than being written out per component; nothing here can
 * detect divergence, because each process sees only its own configuration.
 *
 * Note that nothing downstream will catch it either: bucket bounds are a property of each
 * child, not of the family, so prometheus-cpp accepts two children of one family with
 * different bounds and renders both without complaint. The single placeholder is the only
 * thing keeping them in step.
 *
 * There is deliberately no default. A default would be the one value nobody ever revisits,
 * and bucket bounds that do not bracket the latencies actually being served are worse
 * than no histogram: every observation lands in one bucket and every percentile reads as
 * that bucket's bound.
 */
[[nodiscard]] inline std::vector<double> load_order_round_trip_buckets(const pubsub_itc_fw::TomlConfiguration& toml) {
    std::vector<double> buckets;
    toml.get_required_except(order_round_trip_buckets_key, buckets);

    if (buckets.empty()) {
        throw pubsub_itc_fw::ConfigurationException(fmt::format("{} must not be empty", order_round_trip_buckets_key));
    }
    for (size_t index = 0; index < buckets.size(); ++index) {
        // An infinite top bound is the plausible-looking mistake here, since every rendered
        // histogram ends in le="+Inf" -- but that bucket is the library's, not the
        // and declaring one produces a duplicate. NaN is caught by the same test.
        if (!std::isfinite(buckets[index])) {
            throw pubsub_itc_fw::ConfigurationException(fmt::format(
                "{} element {} is not a finite number; the +Inf bucket is added automatically and must not be listed", order_round_trip_buckets_key, index));
        }
    }
    for (size_t index = 1; index < buckets.size(); ++index) {
        if (buckets[index] <= buckets[index - 1]) {
            throw pubsub_itc_fw::ConfigurationException(fmt::format("{} must be strictly ascending, but element {} ({}) does not exceed element {} ({})",
                                                                    order_round_trip_buckets_key, index, buckets[index], index - 1, buckets[index - 1]));
        }
    }

    return buckets;
}

} // namespaces
