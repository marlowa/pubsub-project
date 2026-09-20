#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <vector>

#include <pubsub_itc_fw/PrometheusEndpoint.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include "GatewayMetrics.hpp"

/**
 * @file OrderPathMetrics.hpp
 * @brief Timing an order at several points on its way through the venue.
 *
 * The venue already measures how long an order takes altogether: a gateway notes the time
 * when it reads the order off the client connection, and notes the time again when it starts
 * sending the report back. The difference between those two is the round trip. What that
 * does not say is where the time went. An order passes through the gateway, the sequencer
 * and the matching engine on the way out, and back through the sequencer and the gateway on
 * the way in, and the round trip covers all of it in one number.
 *
 * So each of those components also notes the time when the order reaches it, and again when
 * it sends it onwards. Each one subtracts the time the gateway recorded at the very start,
 * and records the result. What every component records is therefore the same kind of thing:
 * how long the order had already been inside the venue when it got to that point.
 *
 * Because they all count from the same starting point, subtracting one component's number
 * from the next one's gives how long the order spent in between. That is the measurement
 * this file exists for. Taken on its own, one component's number is not very interesting --
 * it only says how far along the journey that component sits.
 *
 * This works without any extra plumbing because the order already carries the starting time
 * with it. The gateway writes it into the `gateway_ingress_ns` field of the envelope the
 * order travels in, and that field survives the whole journey: the sequencer keeps a copy
 * and writes it back onto the report going the other way, and the matching engine receives
 * it in the envelope it unpacks. Without that, the components would have to compare clocks
 * with each other, which is a much harder thing to get right.
 */
namespace order_path_metrics {

/**
 * @brief The name every component records these timings under.
 *
 * All of the timings share one name and are told apart by two labels that Prometheus
 * attaches to each one: `component` says which process recorded it, and `scope` says which
 * point on the order's journey it was recorded at. The gateway's round-trip timing is
 * arranged the same way, with one name shared by the two gateways and the `component` label
 * telling them apart.
 *
 * Sharing one name is what lets the whole journey be fetched by a single query and grouped
 * by those two labels, rather than one query per point stitched together afterwards. Adding
 * a new point later then costs a new label value, instead of a new name, a new dashboard
 * panel and a new alert.
 *
 * The mistake this invites is worth stating, because it is easy to make and the result looks
 * plausible. A query that does not group by both `component` and `scope` averages together
 * numbers taken at opposite ends of the journey. The answer will describe no point on the
 * path at all, and nothing about it will look wrong.
 */
inline constexpr const char* order_path_elapsed_metric_name = "order_path_elapsed_nanoseconds";

/**
 * @brief The description shown alongside the timings.
 *
 * Every component must pass this same text. Prometheus allows only one description for a
 * given metric name, and PrometheusEndpoint deliberately fails with a PreconditionAssertion
 * if a second component registers the same name with different words. So the wording here
 * has to suit every point on the journey and name none of them in particular.
 */
inline constexpr const char* order_path_elapsed_help = "Nanoseconds from taking an order off the client connection to reaching this point on its path";

/**
 * @brief Label for the moment an order arrives at a component.
 *
 * These four names say what happened, not which component it happened in. Prometheus already
 * attaches a `component` label saying which process recorded the timing, so putting the
 * component into this name as well -- "sequencer_order_in" -- would say it twice, and would
 * mean each component needed its own set of names rather than sharing these.
 */
inline constexpr const char* order_in_scope = "order_in";

/** @brief Label for the moment a component sends an order onwards, further into the venue. */
inline constexpr const char* order_out_scope = "order_out";

/** @brief Label for the moment a report arrives back at a component, on its way to the member. */
inline constexpr const char* er_in_scope = "er_in";

/** @brief Label for the moment a component sends a report onwards, back towards the member. */
inline constexpr const char* er_out_scope = "er_out";

/** @brief Where the bucket boundaries are written in every component's configuration file. */
inline constexpr const char* order_path_elapsed_buckets_key = "metrics.order_path_elapsed_buckets";

/**
 * @brief Reads the histogram's bucket boundaries out of a component's configuration.
 *
 * @param[in] toml The component's parsed configuration.
 * @return Upper bounds in nanoseconds, in ascending order.
 *
 * The checks applied -- that the setting is present, is a non-empty list of numbers, rises
 * strictly, and does not include an infinite top bound -- are the ones
 * gateway_metrics::load_order_round_trip_buckets describes, and they are not repeated here.
 * They are written down once so that no component on the path can end up held to a laxer
 * standard than the one next to it.
 *
 * **Every component on the path must be given the same boundaries.** This matters more here
 * than it does for the gateway metrics. There, two gateways with different boundaries would
 * spoil a comparison between the two of them. Here it would spoil the only use these numbers
 * have: the time an order spends between two components is worked out by subtracting one
 * component's figure from another's, and subtracting a percentile measured against one set
 * of boundaries from a percentile measured against a different set does not give a roughly
 * right answer. It gives one that means nothing.
 *
 * That is why every component reads this setting from one shared value in the environment
 * file rather than each having its own copy. Nothing can catch it if they drift apart: each
 * process only ever sees its own configuration, and Prometheus will happily serve two
 * components' timings with different boundaries without complaining.
 */
[[nodiscard]] inline std::vector<double> load_order_path_elapsed_buckets(const pubsub_itc_fw::TomlConfiguration& toml) {
    return gateway_metrics::detail::load_bucket_bounds(toml, order_path_elapsed_buckets_key);
}

/**
 * @brief Records one timing, unless the numbers behind it would not mean what they appear to.
 *
 * @param[in] histogram  Where to record it. Unbound when this component has metrics switched
 *                       off, in which case recording through it does nothing and is safe.
 * @param[in] has_origin Whether this envelope carries the time the gateway first read the
 *                       order.
 * @param[in] origin_ns  That time. Meaningless when @p has_origin is false.
 * @param[in] now_ns     The time now, read by the caller, so that one reading of the clock
 *                       can serve both this and whatever else the caller needs it for.
 *
 * Written once and shared by all nine places that record a timing, rather than repeated at
 * each of them. Both of the two rules below are easy to forget, and forgetting either one
 * does not produce an obvious failure. It produces recorded times that are wrong, in a
 * direction that no later query could detect.
 *
 * **If the starting time is missing, record nothing at all rather than treating it as zero.**
 * The field really is absent sometimes. Records that one sequencer sends another to keep it
 * in step were never sent by any client. The reports a matching engine produces when it
 * cancels orders after a failover never had a client order behind them. And an order being
 * replayed from the write-ahead log carries the time a client sent it, which may be hours
 * ago. Treating a missing value as zero would mean recording the time since 1970, which
 * would land every such reading in the top bucket and drag the average with it.
 *
 * **If the elapsed time comes out negative, discard it.** After a gateway fails over, the
 * two ends of the measurement are taken by two different processes, and their clocks are not
 * guaranteed to agree to the nanosecond. The round-trip timing already throws these away for
 * the same reason. A single negative value permanently corrupts the running total Prometheus
 * keeps, so every average drawn from it afterwards is wrong, and no later query can undo it.
 */
inline void observe_checkpoint(pubsub_itc_fw::HistogramHandle& histogram, bool has_origin, int64_t origin_ns, int64_t now_ns) {
    if (!has_origin) {
        return;
    }
    const int64_t elapsed_ns = now_ns - origin_ns;
    if (elapsed_ns < 0) {
        return;
    }
    histogram.observe(static_cast<double>(elapsed_ns));
}

} // namespaces
