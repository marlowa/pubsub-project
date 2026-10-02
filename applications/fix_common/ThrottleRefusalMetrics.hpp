#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/CounterHandle.hpp>
#include <pubsub_itc_fw/PrometheusEndpoint.hpp>

#include "ThrottledCommand.hpp"

namespace fix_common {

/**
 * @brief A gateway's counts of the commands its throttles refused, one counter for each kind.
 *
 * Both gateways register the same three metric names, told apart by the component label, which
 * is the process instance, as with the gateways' other metrics (see GatewayMetrics.hpp). A count
 * that rises says some member is sending faster than it is provisioned for; the gateway's log
 * says which member and when (docs/venue/gateway_throttles.md, section 7).
 *
 * The handles record nowhere until registered, and nowhere when metrics are disabled, so a
 * gateway that never registers them still counts safely.
 */
class ThrottleRefusalMetrics {
  public:
    /// Metric names, one for each kind. Shared literals because Prometheus allows one help
    /// string per family, and both gateways register these families.
    static constexpr const char* place_metric_name = "throttled_new_orders_total";
    static constexpr const char* amend_metric_name = "throttled_amends_total";
    static constexpr const char* cancel_metric_name = "throttled_cancels_total";

    /**
     * @brief Registers the three counters with this process's metrics endpoint.
     * @param[in,out] metrics The endpoint, which must outlive this object.
     * @param[in]     scope   The metric scope, which names the registering thread.
     */
    void register_metrics(pubsub_itc_fw::PrometheusEndpoint& metrics, const char* scope) {
        place_ = metrics.register_counter(scope, place_metric_name, "New orders refused because the session had reached its limit per second");
        amend_ = metrics.register_counter(scope, amend_metric_name, "Amends refused because the session had reached its limit per second");
        cancel_ = metrics.register_counter(scope, cancel_metric_name, "Cancels refused because the session had reached its limit per second");
    }

    /**
     * @brief Counts one refused command.
     * @param[in] command The kind of command refused.
     */
    void count_refusal(ThrottledCommand command) {
        switch (command) {
            case ThrottledCommand::Place:
                place_.increment();
                return;
            case ThrottledCommand::Amend:
                amend_.increment();
                return;
            case ThrottledCommand::Cancel:
                cancel_.increment();
                return;
        }
    }

  private:
    pubsub_itc_fw::CounterHandle place_;
    pubsub_itc_fw::CounterHandle amend_;
    pubsub_itc_fw::CounterHandle cancel_;
};

} // namespaces
