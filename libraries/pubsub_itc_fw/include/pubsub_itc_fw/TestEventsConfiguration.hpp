#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Where a component sends its test events, if anywhere.
 *
 * Only a test gives a component this section: a component with none sends no test events and opens no connection
 * for them. See docs/availability/test_events.md.
 */
struct TestEventsConfiguration {
    /// Whether the component sends test events. False when the configuration has no [test_events] section.
    bool enabled{false};

    /// The host of the test's listener, normally 127.0.0.1.
    std::string host;

    /// The port of the test's listener, which the operating system chose when the test opened it.
    uint16_t port{0};
};

/**
 * @brief Reads the optional `[test_events]` section, shared by every component that sends test events.
 *
 * Expected shape, written by the test into the configuration it starts the component with:
 *
 *     [test_events]
 *     host = "127.0.0.1"
 *     port = 45123
 *
 * The section is optional as a whole, and absent means no test events. Once present, both keys are required: a
 * half-filled section is a mistake in the test, and guessing the missing half would hide it.
 */
class TestEventsConfigurationLoader {
  public:
    static TestEventsConfiguration load(const TomlConfiguration& toml) {
        TestEventsConfiguration configuration;
        std::string host;
        const auto [has_host, host_error] = toml.get_required("test_events.host", host);
        if (!has_host) {
            return configuration;
        }
        configuration.enabled = true;
        configuration.host = host;
        int32_t port = 0;
        toml.get_required_except("test_events.port", port);
        if (host.empty()) {
            throw ConfigurationException("test_events.host must not be empty");
        }
        if (port <= 0 || port > UINT16_MAX) {
            throw ConfigurationException(fmt::format("test_events.port must be in range [1, 65535], got {}", port));
        }
        configuration.port = static_cast<uint16_t>(port);
        return configuration;
    }
};

} // namespaces
