#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

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

    /// The name the test knows the component by, such as "sequencer_primary", which the component's threads give in
    /// their introduction. The test chooses it, because only the test knows which of its processes this is.
    std::string component;

    /// The comp ids whose orders an application reports the outcome of, one test event per order, from the optional
    /// `report_orders_of` key. Empty when the key is absent: no outcome is reported for any order. Only the orders
    /// whose reports reach no member, or whose checks no member can make, are meant to be listed here, so that the
    /// per-order events stay confined to the scenarios that need them.
    std::vector<std::string> report_orders_of;

    /// The comp ids whose execution reports are expected to reach no member's session, from the optional
    /// `comp_ids_without_session` key; empty when it is absent. Read by the sequencer: a report for one of these is
    /// counted when it is dropped, but logged at Debug, with no test event, because a test sends commands under such a
    /// comp id straight to the sequencer, with no gateway holding a session for it. Only a component started by a test
    /// has a [test_events] section, so a deployed venue cannot be told to stay quiet about a member.
    std::vector<std::string> comp_ids_without_session;

    /// How long a gateway waits, after one of its links to a sequencer comes up, before it announces the sessions it
    /// holds, in milliseconds, from the optional `delay_session_announcements_ms` key; 0, no wait, when it is absent.
    /// Read by the gateways. It lets a test make a sequencer lead before it has been told of the sessions; see
    /// docs/availability/reports_held_until_sessions_are_announced.md.
    int32_t delay_session_announcements_ms{0};

    /// Whether reports for @p comp_id are expected to reach no member's session.
    [[nodiscard]] bool expects_no_session_for(std::string_view comp_id) const {
        for (const auto& listed : comp_ids_without_session) {
            if (listed == comp_id) {
                return true;
            }
        }
        return false;
    }

    /// Whether the outcome of orders from @p comp_id is to be reported.
    [[nodiscard]] bool reports_orders_of(std::string_view comp_id) const {
        for (const auto& listed : report_orders_of) {
            if (listed == comp_id) {
                return true;
            }
        }
        return false;
    }
};

/**
 * @brief Reads the optional `[test_events]` section, shared by every component that sends test events.
 *
 * Expected shape, written by the test into the configuration it starts the component with:
 *
 *     [test_events]
 *     host = "127.0.0.1"
 *     port = 45123
 *     component = "sequencer_primary"
 *     report_orders_of = ["INJECT-TEST"]
 *
 * The section is optional as a whole, and absent means no test events. Once present, host, port and component are
 * required: a half-filled section is a mistake in the test, and guessing the missing part would hide it.
 * `report_orders_of` is optional, and only an application that reports the outcome of orders reads it. So are
 * `comp_ids_without_session`, read by the sequencer, and `delay_session_announcements_ms`, read by the gateways.
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
        toml.get_required_except("test_events.component", configuration.component);
        if (configuration.component.empty()) {
            throw ConfigurationException("test_events.component must not be empty");
        }
        if (toml.has_key("test_events.report_orders_of")) {
            toml.get_required_except("test_events.report_orders_of", configuration.report_orders_of);
        }
        if (toml.has_key("test_events.comp_ids_without_session")) {
            toml.get_required_except("test_events.comp_ids_without_session", configuration.comp_ids_without_session);
        }
        if (toml.has_key("test_events.delay_session_announcements_ms")) {
            toml.get_required_except("test_events.delay_session_announcements_ms", configuration.delay_session_announcements_ms);
            if (configuration.delay_session_announcements_ms < 0) {
                throw ConfigurationException(
                    fmt::format("test_events.delay_session_announcements_ms must not be negative, got {}", configuration.delay_session_announcements_ms));
            }
        }
        return configuration;
    }
};

} // namespaces
