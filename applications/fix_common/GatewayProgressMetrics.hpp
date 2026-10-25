#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/CounterHandle.hpp>
#include <pubsub_itc_fw/PrometheusEndpoint.hpp>

namespace fix_common {

/**
 * @brief A gateway's running totals of what it took from members and what it answered, as counters.
 *
 * The same totals the gateway prints on its progress line, published so that a program can read them
 * exactly, at any moment, without reading the gateway's Quill log: perf_run.py and callgrind_run.py
 * decide from them when a run has finished and whether every order was answered. Both gateways register
 * the same names, told apart by the component label, which is the process instance, as with the
 * gateways' other metrics (see GatewayMetrics.hpp).
 *
 * Every new order a gateway takes is answered in one of three ways: an execution report delivered to the
 * member, one dropped because the member's session had gone by the time it arrived, or a refusal because
 * the venue said it could not process orders. The report counters count every report, cancels' included,
 * so in a run of new orders alone, each answered with one report, orders received equals reports sent
 * plus reports dropped plus orders refused once the run has drained.
 *
 * The handles record nowhere until registered, and nowhere when metrics are disabled, so a gateway that
 * never registers them still counts safely. An increment takes no lock.
 */
class GatewayProgressMetrics {
  public:
    /// Metric names. Shared literals because Prometheus allows one help string per family, and both
    /// gateways register these families.
    static constexpr const char* orders_received_metric_name = "gateway_orders_received_total";
    static constexpr const char* reports_sent_metric_name = "gateway_execution_reports_sent_total";
    static constexpr const char* reports_dropped_metric_name = "gateway_execution_reports_dropped_total";
    static constexpr const char* orders_refused_metric_name = "gateway_orders_refused_total";
    static constexpr const char* sessions_established_metric_name = "gateway_sessions_established_total";
    static constexpr const char* sessions_lost_metric_name = "gateway_sessions_lost_total";
    static constexpr const char* gap_fills_sent_metric_name = "gateway_gap_fills_sent_total";

    /**
     * @brief Registers the counters with this process's metrics endpoint.
     * @param[in,out] metrics The endpoint, which must outlive this object.
     * @param[in]     scope   The metric scope, which names the registering thread.
     */
    void register_metrics(pubsub_itc_fw::PrometheusEndpoint& metrics, const char* scope) {
        orders_received_ = metrics.register_counter(scope, orders_received_metric_name, "New orders taken from members");
        reports_sent_ = metrics.register_counter(scope, reports_sent_metric_name, "Execution reports delivered to members");
        reports_dropped_ =
            metrics.register_counter(scope, reports_dropped_metric_name, "Execution reports dropped because the member's session had gone when they arrived");
        orders_refused_ = metrics.register_counter(scope, orders_refused_metric_name, "New orders refused because the venue said it could not process orders");
        sessions_established_ = metrics.register_counter(scope, sessions_established_metric_name, "Member sessions that logged on");
        sessions_lost_ = metrics.register_counter(scope, sessions_lost_metric_name, "Member connections that closed");
        gap_fills_sent_ = metrics.register_counter(scope, gap_fills_sent_metric_name, "Gap fills sent to members in answer to a resend request");
    }

    /// Counts a new order taken from a member.
    void count_order_received() {
        orders_received_.increment();
    }

    /// Counts an execution report delivered to a member.
    void count_report_sent() {
        reports_sent_.increment();
    }

    /// Counts an execution report dropped because the member's session had gone.
    void count_report_dropped() {
        reports_dropped_.increment();
    }

    /// Counts a new order refused because the venue said it could not process orders.
    void count_order_refused() {
        orders_refused_.increment();
    }

    /// Counts a member session that logged on.
    void count_session_established() {
        sessions_established_.increment();
    }

    /// Counts a member connection that closed.
    void count_session_lost() {
        sessions_lost_.increment();
    }

    /// Counts a gap fill sent to a member.
    void count_gap_fill_sent() {
        gap_fills_sent_.increment();
    }

  private:
    pubsub_itc_fw::CounterHandle orders_received_;
    pubsub_itc_fw::CounterHandle reports_sent_;
    pubsub_itc_fw::CounterHandle reports_dropped_;
    pubsub_itc_fw::CounterHandle orders_refused_;
    pubsub_itc_fw::CounterHandle sessions_established_;
    pubsub_itc_fw::CounterHandle sessions_lost_;
    pubsub_itc_fw::CounterHandle gap_fills_sent_;
};

} // namespaces
