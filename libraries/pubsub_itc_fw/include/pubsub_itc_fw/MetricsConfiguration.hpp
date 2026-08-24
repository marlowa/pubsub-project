#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Configuration for this process's Prometheus scrape endpoint.
 *
 * See docs/operations/metrics.md.
 */
struct MetricsConfiguration {
    /**
     * @brief Whether metrics are collected and served at all.
     *
     * When false no registry and no listener are created, and every registered metric is a
     * shared no-op. Registration still validates, so a metric path or a duplicate
     * registration that is wrong is wrong whether or not metrics happen to be switched on
     * in the environment that runs first.
     *
     * Sourced from a shared placeholder so the whole venue turns on or off in one edit.
     */
    bool enabled{false};

    /**
     * @brief The first token of every metric key this process emits.
     *
     * Process-wide: every metric here carries the same value, so it is configured once
     * rather than repeated in each metric key. Framework code composes keys from this and
     * could not do so otherwise -- the Reactor is shared by every component, so a metric it
     * declares cannot name any one of them.
     */
    std::string application{"pubsub"};

    /**
     * @brief The second token of every metric key this process emits.
     *
     * The component **instance**, such as "fix_order_gateway_a" rather than
     * "fix_order_gateway". Performance investigation is always about one process: knowing a
     * queue is deep across the gateways is not the question, knowing which gateway is. It
     * is still easy to aggregate over instances when that is wanted, with a matcher such as
     * component=~"fix_order_gateway_.*".
     *
     * deploy.py already resolves ${component_name} per component file, so this costs no new
     * plumbing -- it is the same value cpu_layout_component uses to find this process in
     * the CPU layout.
     *
     * The defaults for this and application are valid tokens rather than empty, so that a
     * component with no [metrics] section at all can still compose keys. Nothing is
     * exposed in that case, since metrics are then disabled; the values merely have to
     * pass MetricKey's validation.
     */
    std::string component{"unconfigured"};

    /**
     * @brief Where the scrape listener binds.
     *
     * Per process, not per venue: roughly fifteen components run on one host in dev and
     * they cannot share a port.
     *
     * A port of 0 binds an ephemeral port, which is how tests avoid colliding on a fixed
     * one; PrometheusEndpoint::listening_port() reports what was actually bound.
     *
     * The host is configurable rather than fixed because the right answer differs by
     * environment. 0.0.0.0 is needed where Prometheus scrapes from another host, which is
     * the usual production topology. 127.0.0.1 is right in dev, where the scrape is local
     * and binding every interface would expose an unauthenticated port per process to the
     * network -- this endpoint has no authentication and no TLS, so whatever reaches it
     * gets the full metric set.
     */
    NetworkEndpointConfiguration listen_endpoint;
};

} // namespaces
