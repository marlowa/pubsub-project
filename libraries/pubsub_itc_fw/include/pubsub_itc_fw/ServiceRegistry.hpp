#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * ServiceRegistry is a simple, read-only-after-construction map from logical
 * service names to their primary and secondary network endpoints.
 *
 * Design principles:
 *   - The framework never reads configuration files. The application constructs
 *     the registry using plain C++ calls, populating it however it sees fit:
 *     from a file, environment variables, or hardcoded values in unit tests.
 *   - The registry is populated before any threads are started and is read-only
 *     thereafter. No thread-safety is required.
 *   - Adding a duplicate service name is a precondition violation and throws
 *     PreconditionAssertion immediately, catching configuration bugs early.
 *
 * Typical usage in application code:
 *
 *   ServiceRegistry registry;
 *   registry.add("joe", {"192.168.1.10", 5001}, {"192.168.1.11", 5001});
 *   registry.add("mary", {"192.168.1.10", 5002}, {"192.168.1.11", 5002});
 *
 * Typical usage in unit tests (no files, no command line):
 *
 *   ServiceRegistry registry;
 *   registry.add("joe", {"127.0.0.1", 9001}, {});
 *
 * Lookup:
 *
 *   auto [endpoints, error] = registry.lookup("joe");
 *   if (!error.empty()) {
 *       // service not found
 *   }
 */

#include <string>
#include <tuple>
#include <unordered_map>

#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/ServiceEndpoints.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A static registry mapping logical service names to their network endpoints.
 *
 * Populated by the application before any threads are started. Read-only thereafter.
 * The framework uses it to resolve service names to addresses when establishing
 * outbound TCP connections.
 */
class ServiceRegistry {
  public:
    ~ServiceRegistry() = default;
    ServiceRegistry() = default;

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;

    /**
     * @brief Registers a named service with its primary and optional secondary endpoints.
     *
     * Must be called before any threads are started. The registry is read-only
     * after construction is complete.
     *
     * @param[in] name      Logical service name (e.g. "joe", "mary"). Must be unique.
     * @param[in] primary   Primary endpoint. The reactor tries this address first.
     * @param[in] secondary Secondary (fallback) endpoint. Set port to 0 if not required.
     * @throws PreconditionAssertion if name is empty or has already been registered.
     */
    void add(const std::string& name, NetworkEndpointConfiguration primary, NetworkEndpointConfiguration secondary) {
        if (name.empty()) {
            throw PreconditionAssertion("ServiceRegistry::add: service name must not be empty", __FILE__, __LINE__);
        }
        if (entries_.count(name) != 0) {
            throw PreconditionAssertion("ServiceRegistry::add: duplicate service name: " + name, __FILE__, __LINE__);
        }
        entries_.emplace(name, ServiceEndpoints{std::move(primary), std::move(secondary)});
    }

    /**
     * @brief Looks up the endpoints for a named service.
     *
     * @param[in] name Logical service name to look up.
     * @return A tuple of { ServiceEndpoints, error_string }.
     *         On success the error string is empty.
     *         On failure (unknown service name) ServiceEndpoints is default-constructed
     *         and the error string describes the problem.
     */
    [[nodiscard]] std::tuple<ServiceEndpoints, std::string> lookup(const std::string& name) const {
        auto it = entries_.find(name);
        if (it == entries_.end()) {
            return {ServiceEndpoints{}, "ServiceRegistry::lookup: unknown service: " + name};
        }
        return {it->second, ""};
    }

    /**
     * @brief Returns the number of registered services.
     */
    [[nodiscard]] int size() const {
        return static_cast<int>(entries_.size());
    }

    /**
     * @brief Returns true if no services have been registered.
     */
    [[nodiscard]] bool empty() const {
        return entries_.empty();
    }

  private:
    std::unordered_map<std::string, ServiceEndpoints> entries_;
};

} // namespace pubsub_itc_fw
