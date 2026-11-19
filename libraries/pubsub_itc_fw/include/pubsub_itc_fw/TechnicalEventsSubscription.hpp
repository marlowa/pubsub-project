#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Where a component that sends no orders finds the technical_events topic, so that it learns of the end of the
 *        trading day (docs/venue/start_and_end_of_day.md, section 4.4, decision 10c).
 *
 * The arbiters, the witness and the authentication services take part in no trading, so nothing on the order path tells
 * them the day has ended. Each subscribes to the technical_events topic, which each matching engine publisher serves from
 * its copy of the WAL, through TopicSubscriberThread, and stops for the end of day when the end-of-day end of its own
 * trading day arrives.
 *
 * The addresses are those of the two publishers' technical_events listeners, filled in by the deployment from the
 * environment. A publisher the environment does not start is given an empty host, and is not connected to; with neither,
 * the component subscribes to nothing.
 */
struct TechnicalEventsSubscription {
    /// The topic's name, as the publishers serve it.
    static constexpr std::string_view topic_name{"technical_events"};
    /// The names under which the publishers are registered as services.
    static constexpr std::string_view primary_service{"technical_events_primary"};
    static constexpr std::string_view secondary_service{"technical_events_secondary"};

    std::string primary_host;
    uint16_t primary_port{0};
    std::string secondary_host;
    uint16_t secondary_port{0};

    /**
     * @brief Reads the `[technical_events]` section: `primary_host`, `primary_port`, `secondary_host` and
     *        `secondary_port`, all required. An empty host means that publisher is not run; its port is then not checked.
     *
     * @param[in] toml       The component's configuration.
     * @param[in] component  The component's name, for the message of a ConfigurationException.
     */
    static TechnicalEventsSubscription load(const TomlConfiguration& toml, const std::string& component);

    /// The names of the services of the publishers that are run, as register_publishers() registers them.
    [[nodiscard]] std::vector<std::string> publisher_services() const;

    /**
     * @brief Registers each publisher that is run as a service in @p registry.
     * @return The names of the services registered, for TopicSubscriberThread.
     */
    [[nodiscard]] std::vector<std::string> register_publishers(ServiceRegistry& registry) const;

    /**
     * @brief Whether @p message, a record delivered on the technical_events topic, is the end-of-day end of
     *        @p trading_day.
     *
     * The end of another day is not this component's: a component started for one day stops only for that day's end.
     */
    [[nodiscard]] static bool is_end_of_day_end(const EventMessage& message, int32_t trading_day);

    /// Records in the last-state file at @p last_state_file that the end-of-day end of @p trading_day has been seen
    /// (LastStateFile). An empty path records nothing.
    static void record_the_end(const std::string& last_state_file, int32_t trading_day);

    /// Whether the last-state file at @p last_state_file says the end-of-day end of @p trading_day has been seen: a
    /// component started again after the end of its day has nothing to do, and exits at once for the end of day.
    [[nodiscard]] static bool already_ended(const std::string& last_state_file, int32_t trading_day);
};

} // namespaces
