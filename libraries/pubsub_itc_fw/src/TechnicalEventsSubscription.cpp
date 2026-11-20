// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/TechnicalEventsSubscription.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/LastStateFile.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>

#include <leader_follower.hpp>

namespace pubsub_itc_fw {

namespace {

uint16_t port_of(const TomlConfiguration& toml, const std::string& key, const std::string& host, const std::string& component) {
    int32_t port = 0;
    toml.get_required_except(key, port);
    if (host.empty()) {
        return 0;
    }
    if (port < 1 || port > 65'535) {
        throw ConfigurationException(component + ": " + key + " must be in range [1, 65535] when its host is given, got " + std::to_string(port));
    }
    return static_cast<uint16_t>(port);
}

} // namespaces

TechnicalEventsSubscription TechnicalEventsSubscription::load(const TomlConfiguration& toml, const std::string& component) {
    TechnicalEventsSubscription subscription;
    toml.get_required_except("technical_events.primary_host", subscription.primary_host);
    subscription.primary_port = port_of(toml, "technical_events.primary_port", subscription.primary_host, component);
    toml.get_required_except("technical_events.secondary_host", subscription.secondary_host);
    subscription.secondary_port = port_of(toml, "technical_events.secondary_port", subscription.secondary_host, component);
    return subscription;
}

std::vector<std::string> TechnicalEventsSubscription::publisher_services() const {
    std::vector<std::string> services;
    if (!primary_host.empty()) {
        services.emplace_back(primary_service);
    }
    if (!secondary_host.empty()) {
        services.emplace_back(secondary_service);
    }
    return services;
}

std::vector<std::string> TechnicalEventsSubscription::register_publishers(ServiceRegistry& registry) const {
    std::vector<std::string> services;
    if (!primary_host.empty()) {
        registry.add(std::string(primary_service), NetworkEndpointConfiguration{primary_host, primary_port}, NetworkEndpointConfiguration{});
        services.emplace_back(primary_service);
    }
    if (!secondary_host.empty()) {
        registry.add(std::string(secondary_service), NetworkEndpointConfiguration{secondary_host, secondary_port}, NetworkEndpointConfiguration{});
        services.emplace_back(secondary_service);
    }
    return services;
}

bool TechnicalEventsSubscription::is_end_of_day_end(const EventMessage& message, int32_t trading_day) {
    if (message.pdu_id() != TechnicalEvent::message_pdu_id) {
        return false;
    }
    std::vector<uint8_t> arena(static_cast<size_t>(message.payload_size()) + 256);
    BumpAllocator allocator(arena.data(), arena.size());
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;
    TechnicalEventView event{};
    if (!decode(event, message.payload(), static_cast<size_t>(message.payload_size()), bytes_consumed, allocator, arena_bytes_needed)) {
        return false;
    }
    return event.phase == TradingPhase::EndOfDayEnd && event.trading_day == trading_day;
}

void TechnicalEventsSubscription::record_the_end(const std::string& last_state_file, int32_t trading_day) {
    LastStateFile(last_state_file, trading_day).record(to_string(TradingPhase::EndOfDayEnd));
}

bool TechnicalEventsSubscription::already_ended(const std::string& last_state_file, int32_t trading_day) {
    return LastStateFile(last_state_file, trading_day).says(to_string(TradingPhase::EndOfDayEnd));
}

} // namespaces
