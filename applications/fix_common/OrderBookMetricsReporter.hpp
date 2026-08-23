#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>

#include <pubsub_itc_fw/GaugeHandle.hpp>
#include <pubsub_itc_fw/PrometheusEndpoint.hpp>

namespace fix_common {

/**
 * @brief Publishes the size and shape of a matching engine's order book.
 *
 * The book is a hash map of orders that are resting -- accepted and not yet
 * cancelled. Nothing else in the venue reports how big it is. Until this
 * existed, the only sign of its size was one log line per doubling of the
 * table, which says the storage grew but never says how full it is, and says
 * nothing at all in between doublings.
 *
 * That gap has cost something twice. Choosing `[order_book] initial_capacity`
 * was guesswork, because nobody could see the number of resting orders it was
 * meant to cover. And a book that grows without bound looks exactly like one
 * that is merely busy until the process is killed for it.
 *
 * ## Entries against slots
 *
 * `entries` is what the venue is holding. `slots` is what the map has claimed
 * to hold it in, and is deliberately larger: the load factor is held at a half,
 * so a table settles at twice the entries it carries, rounded up to a power of
 * two. Reading slots as though it were orders overstates the book by at least
 * two to one.
 *
 * `slots` also counts both tables while the map is growing, because during a
 * migration both exist and both are paid for. A doubling therefore shows as a
 * step to three times the settled figure, falling back to two once the old
 * table is released. That is the true memory cost at that moment and is the
 * reason to report it rather than smooth it away. `migrating` says when this is
 * happening, so a step in `slots` can be told apart from a step in the book.
 *
 * ## Names
 *
 * No `_count` or `_total` suffix, matching PoolMetricsReporter: Prometheus
 * reserves `_total` for counters, and these are gauges -- they fall as well as
 * rise. `largest_allocation_bytes` is the exception in spirit, since it only
 * rises, but it is a high-water mark rather than an accumulating count, and the
 * name says which.
 */
class OrderBookMetricsReporter {
  public:
    /**
     * @brief Registers the gauges. Idempotent.
     * @param[in] endpoint    Endpoint to register with.
     * @param[in] scope_name  Metric scope; must match [A-Za-z0-9_]+.
     */
    void register_metrics(pubsub_itc_fw::PrometheusEndpoint& endpoint, const char* scope_name) {
        if (is_registered()) {
            return;
        }
        entries_ = endpoint.register_gauge(scope_name, "order_book_entries", "Orders resting in the book: accepted and not yet cancelled");
        slots_ = endpoint.register_gauge(scope_name, "order_book_slots", "Hash slots claimed to hold them, across both tables while growing");
        migrating_ = endpoint.register_gauge(scope_name, "order_book_migrating", "1 while the book is being moved into a larger table, 0 when settled");
        largest_allocation_bytes_ =
            endpoint.register_gauge(scope_name, "order_book_largest_allocation_bytes", "Largest single table allocation the book has ever made");
    }

    /**
     * @brief Publishes one snapshot.
     * @param[in] entries                   Orders currently resting.
     * @param[in] slots                     Hash slots across both tables.
     * @param[in] migrating                 Whether a migration is in progress.
     * @param[in] largest_allocation_bytes  High-water mark of a single table allocation.
     *
     * Safe to call before register_metrics(): an unbound GaugeHandle records nowhere.
     */
    void update(size_t entries, size_t slots, bool migrating, size_t largest_allocation_bytes) {
        entries_.set(static_cast<double>(entries));
        slots_.set(static_cast<double>(slots));
        migrating_.set(migrating ? 1.0 : 0.0);
        largest_allocation_bytes_.set(static_cast<double>(largest_allocation_bytes));
    }

    /// True once register_metrics() has bound the gauges.
    [[nodiscard]] bool is_registered() const {
        return entries_.is_bound();
    }

  private:
    pubsub_itc_fw::GaugeHandle entries_;
    pubsub_itc_fw::GaugeHandle slots_;
    pubsub_itc_fw::GaugeHandle migrating_;
    pubsub_itc_fw::GaugeHandle largest_allocation_bytes_;
};

} // namespaces
