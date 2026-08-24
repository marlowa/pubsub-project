#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

namespace pubsub_itc_fw {

/**
 * @brief A metric's identity, written as a dotted key and split into the parts
 *        Prometheus wants.
 *
 * The key has the form:
 *
 *     <application>.<component>[.<scope>].<metricName>
 *
 * Four tokens with a scope, three without. Scope is a single token, not a list, so a key
 * never has more than four parts.
 *
 * For example:
 *
 *     pubsub.gateway.binary.socket_latency_seconds
 *
 * yields the time series:
 *
 *     socket_latency_seconds{application="pubsub", component="gateway", scope="binary"}
 *
 * **The dotted key itself is never given to Prometheus.** Only the individual parts are:
 * the leaf becomes the metric name, and the others become label values.
 *
 * **Validation**
 *
 * Every rule is checked in the constructor, which throws ConfigurationException on any
 * violation. Metric paths arrive from configuration, so a malformed one is a deployment
 * error and belongs to that exception rather than to PreconditionAssertion. Checking here
 * means a bad key fails when configuration is read, naming the key and the reason,
 * instead of surfacing much later as a rejected metric inside the Prometheus client or --
 * worse -- as a time series nobody notices is missing.
 *
 * The rules:
 *
 * - three or four tokens, no more and no fewer;
 * - every token is one or more characters from `[A-Za-z0-9_]`, so no token may be empty,
 *   which in turn rejects a leading dot, a trailing dot and any doubled dot. Because the
 *   the key is split on dots and capped at four tokens, a scope containing a dot cannot be
 *   expressed at all -- it simply reads as too many tokens;
 * - the **leaf token must additionally not start with a digit**, because it becomes the
 *   metric name and Prometheus requires those to match `[a-zA-Z_:][a-zA-Z0-9_:]*`. The
 *   other tokens become label values and carry no such restriction, so a component or
 *   scope of "5xx" is fine while a metric name of "5xx_total" is not.
 *
 * Colons are excluded by the character set above, deliberately: Prometheus permits them in
 * metric names but reserves them by convention for recording rules, so emitting one would
 * collide with that convention.
 */
class MetricKey {
  public:
    /**
     * @brief Parses and validates a dotted metric key.
     * @param[in] full_name Dot-separated key; see the class documentation for the grammar.
     *
     * Raises ConfigurationException if @p full_name is null or breaks any rule above. The
     * message names the offending key and what is wrong with it.
     */
    explicit MetricKey(const char* full_name);

    /**
     * @brief Builds a key from its parts rather than parsing one.
     *
     * For metric keys that are not written in configuration. Framework code needs this:
     * the Reactor declares the same metric in every component, so its key cannot be a
     * literal -- the application and component differ per process and are known only at
     * run time.
     *
     * @param[in] application First token; from configuration.
     * @param[in] component   Second token; the component instance, from configuration.
     * @param[in] scope       Third token, or empty for a key with no scope.
     * @param[in] metric_name The metric name itself.
     *
     * Validates exactly as the parsing constructor does, because it builds the dotted form
     * and hands it to that constructor. One set of rules, one implementation -- a second
     * validation path would be a second thing to keep in step.
     */
    [[nodiscard]] static MetricKey compose(const std::string& application, const std::string& component, const std::string& scope,
                                           const std::string& metric_name);

    /** @brief First token; the `application` label value. */
    [[nodiscard]] const std::string& application() const {
        return application_;
    }

    /** @brief Second token; the `component` label value. */
    [[nodiscard]] const std::string& component() const {
        return component_;
    }

    /**
     * @brief Third token when present; the `scope` label value.
     *
     * Empty when the key had exactly three tokens. A token can never itself be empty, so
     * an empty string here is unambiguous: it means there is no scope, and the caller
     * should omit the label rather than emit `scope=""`.
     */
    [[nodiscard]] const std::string& scope() const {
        return scope_;
    }

    /** @brief Whether this key has a scope, and therefore whether to emit the label. */
    [[nodiscard]] bool has_scope() const {
        return !scope_.empty();
    }

    /** @brief Leaf token; the Prometheus metric name. */
    [[nodiscard]] const std::string& metric_name() const {
        return metric_name_;
    }

    /** @brief The original dotted key, as supplied. Useful in log and error messages. */
    [[nodiscard]] const std::string& full_name() const {
        return full_name_;
    }

  private:
    std::string full_name_;
    std::string application_;
    std::string component_;
    std::string scope_;
    std::string metric_name_;
};

} // namespaces
