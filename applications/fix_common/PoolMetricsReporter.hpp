#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>

#include <pubsub_itc_fw/AllocatorBehaviourStatistics.hpp>
#include <pubsub_itc_fw/GaugeHandle.hpp>
#include <pubsub_itc_fw/PoolStatistics.hpp>
#include <pubsub_itc_fw/PrometheusEndpoint.hpp>

namespace fix_common {

/**
 * @brief Publishes one pool allocator's statistics to Prometheus.
 *
 * ExpandablePoolAllocator records nothing anywhere. It computes its statistics and offers
 * them through get_pool_statistics() and get_behaviour_statistics(), leaving the decision
 * of what to do with them to whoever owns the pool. That separation is deliberate and this
 * class does not disturb it: the allocator still has no idea a metrics system exists, and
 * an owner that wants the numbers in a log, in a shutdown report, or nowhere at all is
 * unaffected.
 *
 * **Why this lives with the applications**
 *
 * It sits in the applications rather than in pubsub_itc_fw for the same reason the
 * allocator has no metrics in it. The framework provides the mechanism -- pools that
 * count what they do, an endpoint that can publish -- and stops there. Which numbers are
 * worth a time series, what they are called and how often they are sampled are decisions
 * about one venue's observability, and they change on a different schedule from the
 * framework. Putting this here keeps a component free to publish a different set, or
 * none, without the framework having an opinion about it.
 *
 * **Why the caller drives the sampling**
 *
 * PrometheusEndpoint is push-style -- a gauge holds whatever was last set into it, and
 * there is no scrape-time callback. Something must therefore call update() periodically.
 * The owner does it, from a recurring timer on its own thread, because the owner is the
 * only party that knows which thread may touch its pool and how often the numbers are
 * worth refreshing.
 *
 * The cost is small enough for a reactor callback thread. get_pool_statistics() walks the
 * pool chain, but per pool it only reads counters that are maintained atomically as
 * allocations happen; it never traverses a free list. A chain of twenty-odd pools is
 * twenty-odd atomic loads, which at the scrape interval is immaterial.
 *
 * **Why these are all gauges, and why none of them ends in `_total`**
 *
 * The lifetime figures -- allocations, expansions, failures -- rise and never fall, which
 * is counter-shaped. They are gauges anyway, and are named accordingly.
 *
 * What the allocator offers is a snapshot: `get_behaviour_statistics()` reports a level at
 * an instant, not a stream of events. Driving a real counter from that would mean holding
 * the previous reading and adding the difference, which reconstructs increments this class
 * never observed and needs a special case for the pool being rebuilt -- the gateway creates
 * its pool in on_app_ready_event, so a fresh pool restarts at zero and the difference goes
 * negative. Setting a gauge says precisely what is known, that the level was this when it
 * was read, and claims nothing further.
 *
 * Given that, `_total` must not appear in the names. Prometheus reserves that suffix for
 * counters, and tooling reads the suffix rather than the TYPE line: `pool_allocations_total`
 * declared as a gauge is classified as a counter by anything following the convention. That
 * these are cumulative belongs in the help text, where it is now.
 *
 * **What to watch**
 *
 * pool_bytes_reserved against pool_bytes_in_use is the pair that says how much of a pool
 * is live and how much is reservation waiting to be used. A pool sized by
 * `initial_pools` claims all of it at startup, so resident memory reveals nothing about
 * demand and this pair is the only way to tell the two apart.
 *
 * **Two names that deliberately avoid `_count`**
 *
 * The number of pools is `pool_segments` and the number of full ones is
 * `pool_chain_full`, rather than the obvious `pool_count` and `pool_full_count`.
 *
 * `pool_segments` deliberately does not say "chain". It is very largely the configured
 * `initial_pools` -- 21 for the binary gateway -- and a reader who sees a number that size
 * next to the word "chain" concludes the pool has been chaining, which it has not. Chaining
 * is `pool_expansion_events`, and that is the number to read for it. Prometheus
 * reserves `<name>_count` as a histogram's or summary's count series, so a gauge ending that
 * way is read by convention-following tooling as part of a family that does not exist. The
 * first spelling was tried and `scripts/pubsub_metrics.py` -- which discovers what is there
 * rather than being told -- duly reported two empty histograms called `pool` and `pool_full`.
 * Grafana would do the same. Do not rename these back.
 *
 * pool_expansion_events is the one to alert on. The allocator chains a fresh pool
 * rather than falling back to the heap, so a pool configured too small keeps working and
 * says so only through this number and a log line. Anything above zero means the
 * configured size was wrong.
 */
class PoolMetricsReporter {
  public:
    ~PoolMetricsReporter() = default;
    PoolMetricsReporter() = default;
    PoolMetricsReporter(const PoolMetricsReporter& other) = default;
    PoolMetricsReporter& operator=(const PoolMetricsReporter& other) = default;

    /**
     * @brief Registers this pool's gauges, after which update() publishes.
     * @param[in] endpoint  The endpoint to register with. Must outlive this reporter.
     * @param[in] pool_name The pool's name, used as the metric scope. It becomes a single
     *                      token of a MetricKey, so it must match [A-Za-z0-9_]+ -- a name
     *                      carrying a dot cannot be expressed as a scope and is rejected
     *                      when the key is built.
     *
     * Registering twice would add a second set of gauges for the same series, so this
     * returns without doing anything when it has already been called.
     */
    void register_metrics(pubsub_itc_fw::PrometheusEndpoint& endpoint, const char* pool_name) {
        if (is_registered()) {
            return;
        }
        objects_allocated_ = endpoint.register_gauge(pool_name, "pool_objects_allocated", "Objects currently allocated from this pool chain");
        objects_available_ = endpoint.register_gauge(pool_name, "pool_objects_available", "Objects available for allocation across this pool chain");
        objects_per_pool_ = endpoint.register_gauge(pool_name, "pool_objects_per_pool", "Objects each pool in the chain can hold");
        pool_count_ = endpoint.register_gauge(pool_name, "pool_segments", "Memory segments this pool comprises, mostly the configured initial_pools");
        full_pool_count_ = endpoint.register_gauge(pool_name, "pool_chain_full", "Pools in the chain with no free slot left");
        object_size_bytes_ = endpoint.register_gauge(pool_name, "pool_object_size_bytes", "Size of one pooled object in bytes");
        bytes_reserved_ = endpoint.register_gauge(pool_name, "pool_bytes_reserved", "Bytes claimed by the whole pool chain, live or not");
        bytes_in_use_ = endpoint.register_gauge(pool_name, "pool_bytes_in_use", "Bytes held by objects currently allocated");
        allocations_ = endpoint.register_gauge(pool_name, "pool_allocations", "Successful allocations over the life of this pool");
        fast_path_allocations_ = endpoint.register_gauge(pool_name, "pool_fast_path_allocations", "Allocations served without a chain search");
        slow_path_allocations_ = endpoint.register_gauge(pool_name, "pool_slow_path_allocations", "Allocations that had to search the chain");
        expansion_events_ = endpoint.register_gauge(pool_name, "pool_expansion_events", "Times a fresh pool was chained after exhaustion");
        allocation_failures_ = endpoint.register_gauge(pool_name, "pool_allocation_failures", "Allocations that failed even after chaining");
    }

    /**
     * @brief Publishes one pair of snapshots.
     * @param[in] pool_statistics      From ExpandablePoolAllocator::get_pool_statistics().
     * @param[in] behaviour_statistics From ExpandablePoolAllocator::get_behaviour_statistics().
     *
     * Safe to call before register_metrics(): an unbound GaugeHandle records nowhere.
     */
    void update(const pubsub_itc_fw::PoolStatistics& pool_statistics, const pubsub_itc_fw::AllocatorBehaviourStatistics& behaviour_statistics) {
        // Widened before multiplying. A pool chain sized for millions of objects exceeds
        // what the int members of PoolStatistics can express once multiplied out: at 21
        // pools of 2^20 objects the reserved figure is over three billion bytes, which
        // overflows a 32-bit product while every factor still fits comfortably.
        const size_t pool_count = static_cast<size_t>(pool_statistics.number_of_pools_);
        const size_t per_pool = static_cast<size_t>(pool_statistics.number_of_objects_per_pool_);
        const size_t allocated = static_cast<size_t>(pool_statistics.number_of_allocated_objects_);
        const size_t object_size = pool_statistics.object_size_;

        objects_allocated_.set(static_cast<double>(allocated));
        objects_available_.set(static_cast<double>(pool_statistics.number_of_objects_available_));
        objects_per_pool_.set(static_cast<double>(per_pool));
        pool_count_.set(static_cast<double>(pool_count));
        full_pool_count_.set(static_cast<double>(pool_statistics.number_of_full_pools_));
        object_size_bytes_.set(static_cast<double>(object_size));
        bytes_reserved_.set(static_cast<double>(pool_count * per_pool * object_size));
        bytes_in_use_.set(static_cast<double>(allocated * object_size));

        allocations_.set(static_cast<double>(behaviour_statistics.total_allocations));
        fast_path_allocations_.set(static_cast<double>(behaviour_statistics.fast_path_allocations));
        slow_path_allocations_.set(static_cast<double>(behaviour_statistics.slow_path_allocations));
        expansion_events_.set(static_cast<double>(behaviour_statistics.expansion_events));
        allocation_failures_.set(static_cast<double>(behaviour_statistics.failed_allocations));
    }

    /** @brief Whether register_metrics() has bound this reporter to an endpoint. */
    [[nodiscard]] bool is_registered() const {
        return objects_allocated_.is_bound();
    }

  private:
    pubsub_itc_fw::GaugeHandle objects_allocated_;
    pubsub_itc_fw::GaugeHandle objects_available_;
    pubsub_itc_fw::GaugeHandle objects_per_pool_;
    pubsub_itc_fw::GaugeHandle pool_count_;
    pubsub_itc_fw::GaugeHandle full_pool_count_;
    pubsub_itc_fw::GaugeHandle object_size_bytes_;
    pubsub_itc_fw::GaugeHandle bytes_reserved_;
    pubsub_itc_fw::GaugeHandle bytes_in_use_;
    pubsub_itc_fw::GaugeHandle allocations_;
    pubsub_itc_fw::GaugeHandle fast_path_allocations_;
    pubsub_itc_fw::GaugeHandle slow_path_allocations_;
    pubsub_itc_fw::GaugeHandle expansion_events_;
    pubsub_itc_fw::GaugeHandle allocation_failures_;
};

} // namespaces
