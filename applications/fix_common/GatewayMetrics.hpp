#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
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
 * docs/operations/metrics.md builds its registration rules around: one family, several
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

/**
 * @brief Name of the gateway's own decode-and-forward histogram.
 *
 * Measured from the moment the gateway took the client's order in hand to the moment it
 * forwards that order to the sequencer. It covers the work that is the gateway's alone:
 * parsing the client's bytes, validating the fields, and building the envelope.
 *
 * **This is the metric that compares the protocols, and the round trip is not.** The round
 * trip spans the sequencer, the matching engine, the write-ahead log and the whole report
 * path back, and every one of those does byte-identical work whichever gateway took the
 * order. Measured on 2026-09-19 under load, the two gateways' round trips differed by 57us
 * inside 900us, and their inter-thread queue latencies by 1us -- so an end-to-end figure
 * is dominated by what the protocols share rather than by what separates them. Decoding
 * ASCII FIX against decoding a binary layout is what separates them, and this is where it
 * is visible.
 *
 * One name for both protocols, told apart by the component label, for the reasons set out
 * on the round-trip metric above; the same querying trap applies.
 *
 * Recorded for a NewOrderSingle only, which is the population the round-trip histogram
 * measures, so the two can be read against each other without correcting for message mix.
 */
inline constexpr const char* order_ingress_to_forward_metric_name = "order_ingress_to_forward_nanoseconds";

/** @brief Help text for the family, one literal shared by both gateways as above. */
inline constexpr const char* order_ingress_to_forward_help = "Nanoseconds from taking an order off the client connection to forwarding it to the sequencer";

/**
 * @brief Metric scope for the open-order pool, shared by both gateways.
 *
 * The scope names a pool, so that one family of pool gauges covers every pool in the venue
 * and a query tells them apart by label rather than by metric name. Adding a pool then costs
 * a label value instead of a new metric, a new panel and a new alert.
 *
 * The value is the TOML section that sizes the pool, not the pool's own name. An operator
 * reading `pool_expansion_events_total{scope="open_order_pool"} > 0` learns which section to
 * widen; the internal name "BinaryOpenOrderPool" names nothing that can be edited.
 *
 * The trap is the one the round-trip histogram has: a query without `by (scope)` blends the
 * pools of a process into a number that describes none of them.
 */
inline constexpr const char* open_order_pool_metrics_scope = "open_order_pool";

/**
 * @brief How often a gateway samples its pool statistics into the gauges.
 *
 * Matched to the Prometheus scrape interval. Sampling faster does work nothing collects;
 * sampling slower means a scrape reads a value that predates it by more than one interval,
 * which shows up as a gauge that appears to lag the load.
 *
 * Shared by both gateways for the same reason the help text is: two gateways sampling at
 * different rates would make a comparison between them measure the instrumentation.
 */
inline constexpr std::chrono::seconds pool_metrics_sample_interval{5};

/** @brief Configuration path holding the bucket bounds, identical in every gateway file. */
inline constexpr const char* order_round_trip_buckets_key = "metrics.order_round_trip_buckets";

/** @brief Configuration path holding the decode-and-forward bounds, identical in every gateway file. */
inline constexpr const char* order_ingress_to_forward_buckets_key = "metrics.order_ingress_to_forward_buckets";

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
namespace detail {

/**
 * @brief Reads and validates one histogram's bucket bounds.
 *
 * @param[in] toml The gateway's parsed configuration.
 * @param[in] key  Configuration path naming the array of upper bounds.
 * @return Upper bounds in nanoseconds, ascending.
 *
 * Shared by every bucket-bound reader here so that the rules are stated once. Every message
 * names the key it was given, so an operator reads which line of which file to correct.
 */
[[nodiscard]] inline std::vector<double> load_bucket_bounds(const pubsub_itc_fw::TomlConfiguration& toml, const char* key) {
    std::vector<double> buckets;
    toml.get_required_except(key, buckets);

    if (buckets.empty()) {
        throw pubsub_itc_fw::ConfigurationException(fmt::format("{} must not be empty", key));
    }
    for (size_t index = 0; index < buckets.size(); ++index) {
        // An infinite top bound is the plausible-looking mistake here, since every rendered
        // histogram ends in le="+Inf" -- but that bucket is the library's, not the
        // configuration's, and declaring one produces a duplicate. NaN is caught by the same test.
        if (!std::isfinite(buckets[index])) {
            throw pubsub_itc_fw::ConfigurationException(
                fmt::format("{} element {} is not a finite number; the +Inf bucket is added automatically and must not be listed", key, index));
        }
    }
    for (size_t index = 1; index < buckets.size(); ++index) {
        if (buckets[index] <= buckets[index - 1]) {
            throw pubsub_itc_fw::ConfigurationException(fmt::format("{} must be strictly ascending, but element {} ({}) does not exceed element {} ({})", key,
                                                                    index, buckets[index], index - 1, buckets[index - 1]));
        }
    }

    return buckets;
}

} // namespaces

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
    return detail::load_bucket_bounds(toml, order_round_trip_buckets_key);
}

/**
 * @brief Reads and validates the decode-and-forward histogram's bucket bounds.
 *
 * @param[in] toml The gateway's parsed configuration.
 * @return Upper bounds in nanoseconds, ascending.
 *
 * The rules are those of load_order_round_trip_buckets and are not restated. The bounds
 * differ, and must: this histogram measures a part of what the round trip measures, so
 * bounds chosen for the whole put every observation of the part in the lowest bucket.
 *
 * The requirement that both gateways share one set of bounds is stronger here than it is
 * for the round trip, because comparing the two protocols is the entire reason this metric
 * exists rather than one use of it among several.
 */
[[nodiscard]] inline std::vector<double> load_order_ingress_to_forward_buckets(const pubsub_itc_fw::TomlConfiguration& toml) {
    return detail::load_bucket_bounds(toml, order_ingress_to_forward_buckets_key);
}

} // namespaces
