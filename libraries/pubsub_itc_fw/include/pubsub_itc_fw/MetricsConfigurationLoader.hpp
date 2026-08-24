#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/MetricsConfiguration.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Reads the `[metrics]` section every component shares.
 *
 * Shared rather than repeated in each application's loader for the same reason
 * LoggingConfigurationLoader is: nine loaders parsing the same three keys would be nine
 * places for the validation to drift apart, and the rules below are the sort that only
 * bite in the environment nobody tested.
 *
 * Expected shape:
 *
 *     [metrics]
 *     enabled     = true
 *     listen_host = "127.0.0.1"
 *     listen_port = 9101
 *
 * See docs/operations/metrics.md.
 */
class MetricsConfigurationLoader {
  public:
    static MetricsConfiguration load(const TomlConfiguration& toml) {
        MetricsConfiguration configuration;

        // The section is optional as a whole: a component that has not yet been given one
        // runs with metrics off rather than failing to start. Once present, though, every
        // key in it is required -- a half-filled section is a mistake, and defaulting the
        // missing half would hide it.
        bool enabled = false;
        const auto [has_enabled, enabled_error] = toml.get_required("metrics.enabled", enabled);
        if (!has_enabled) {
            return configuration;
        }
        configuration.enabled = enabled;

        toml.get_required_except("metrics.application", configuration.application);
        toml.get_required_except("metrics.component", configuration.component);
        toml.get_required_except("metrics.listen_host", configuration.listen_endpoint.host);

        int32_t listen_port = 0;
        toml.get_required_except("metrics.listen_port", listen_port);
        // 0 is legal and means "let the operating system choose", which is how tests avoid
        // colliding on a fixed port. Anything outside the port range is not.
        if (listen_port < 0 || listen_port > UINT16_MAX) {
            throw ConfigurationException(fmt::format("metrics.listen_port must be in range [0, 65535], got {}", listen_port));
        }
        configuration.listen_endpoint.port = static_cast<uint16_t>(listen_port);

        // Only checked when metrics are on. A disabled endpoint binds nothing, so an empty
        // host is simply unused -- failing on it would make turning metrics off require
        // filling in a value that will never be read.
        if (configuration.enabled && configuration.listen_endpoint.host.empty()) {
            throw ConfigurationException("metrics.listen_host must not be empty when metrics.enabled is true");
        }

        return configuration;
    }
};

} // namespaces
