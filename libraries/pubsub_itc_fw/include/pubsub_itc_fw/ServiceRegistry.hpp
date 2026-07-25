#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * ServiceRegistry is a simple, read-only-after-construction catalog of logical
 * services. Each service has a name, its primary and secondary network
 * endpoints, and a stable integer id (ServiceID) assigned at registration.
 *
 * The id lets the framework keep service names off the hot path: connect_to_service
 * resolves a name to its ServiceID once (failing fast if the name is unknown), and
 * a Connect control command then carries the integer id rather than a std::string,
 * so no string is copied through the reactor's by-value command queue. The reactor
 * maps the id back to the service's name and endpoints when it processes the command.
 *
 * Design principles:
 *   - The framework never reads configuration files. The application constructs
 *     the registry using plain C++ calls, populating it however it sees fit:
 *     from a file, environment variables, or hardcoded values in unit tests.
 *   - The registry is populated before any threads are started and is read-only
 *     thereafter. No thread-safety is required, so ids are stable for the run.
 *   - Adding a duplicate service name is a precondition violation and throws
 *     PreconditionAssertion immediately, catching configuration bugs early.
 *
 * Typical usage in application code:
 *
 *   ServiceRegistry registry;
 *   registry.add("joe", {"192.168.1.10", 5001}, {"192.168.1.11", 5001});
 *   registry.add("mary", {"192.168.1.10", 5002}, {"192.168.1.11", 5002});
 *
 * Resolve a name to its id (invalid id if unknown), then use the id:
 *
 *   const ServiceID id = registry.resolve("joe");
 *   if (id.is_valid()) {
 *       const ServiceEndpoints& endpoints = registry.endpoints(id);
 *       const std::string& name = registry.service_name(id);
 *   }
 *
 * lookup(name) remains available for name-keyed callers that want endpoints in
 * one call:
 *
 *   auto [endpoints, error] = registry.lookup("joe");
 *   if (!error.empty()) {
 *       // service not found
 *   }
 */

#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/ServiceEndpoints.hpp>
#include <pubsub_itc_fw/ServiceID.hpp>

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
     * @pre name must be non-empty and not already registered. Violating either throws PreconditionAssertion.
     */
    void add(const std::string& name, NetworkEndpointConfiguration primary, NetworkEndpointConfiguration secondary) {
        register_service(name, ServiceEndpoints{std::move(primary), std::move(secondary), std::nullopt}, "add");
    }

    /**
     * @brief Registers a named TLS service with its primary and optional secondary endpoints.
     *
     * Identical to add() but attaches TLS configuration so that every outbound connection
     * to this service performs a TLS handshake before delivering ConnectionEstablished.
     *
     * Must be called before any threads are started.
     *
     * @param[in] name      Logical service name. Must be unique.
     * @param[in] primary   Primary endpoint.
     * @param[in] secondary Secondary endpoint. Set port to 0 if not required.
     * @param[in] tls       TLS configuration (CA path, optional client cert/key, buffer capacity).
     * @pre name must be non-empty and not already registered. Violating either throws PreconditionAssertion.
     */
    void add_tls(const std::string& name, NetworkEndpointConfiguration primary, NetworkEndpointConfiguration secondary, TlsClientConfiguration tls) {
        ServiceEndpoints endpoints;
        endpoints.primary = std::move(primary);
        endpoints.secondary = std::move(secondary);
        endpoints.tls = std::move(tls);
        register_service(name, std::move(endpoints), "add_tls");
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
        auto it = name_to_id_.find(name);
        if (it == name_to_id_.end()) {
            return {ServiceEndpoints{}, "ServiceRegistry::lookup: unknown service: " + name};
        }
        return {services_[index_of(it->second)].endpoints, ""};
    }

    /**
     * @brief Resolves a service name to its stable ServiceID.
     *
     * @return The service's ServiceID, or a default-constructed (invalid) ServiceID
     *         if the name is not registered. Callers fail fast on an invalid id.
     */
    [[nodiscard]] ServiceID resolve(const std::string& name) const {
        auto it = name_to_id_.find(name);
        return it == name_to_id_.end() ? ServiceID{} : it->second;
    }

    /**
     * @brief Returns the endpoints for a previously resolved ServiceID.
     * @pre id must be a valid ServiceID from this registry (else PreconditionAssertion).
     */
    [[nodiscard]] const ServiceEndpoints& endpoints(ServiceID id) const {
        return services_[checked_index(id, "endpoints")].endpoints;
    }

    /**
     * @brief Returns the service name for a previously resolved ServiceID.
     * @pre id must be a valid ServiceID from this registry (else PreconditionAssertion).
     */
    [[nodiscard]] const std::string& service_name(ServiceID id) const {
        return services_[checked_index(id, "service_name")].name;
    }

    /**
     * @brief Returns the number of registered services.
     */
    [[nodiscard]] int size() const {
        return static_cast<int>(services_.size());
    }

    /**
     * @brief Returns true if no services have been registered.
     */
    [[nodiscard]] bool empty() const {
        return services_.empty();
    }

  private:
    // A service's ServiceID is 1-based (value 0 is the invalid sentinel), so the
    // index into services_ is id - 1.
    struct ServiceEntry {
        std::string name;
        ServiceEndpoints endpoints;
    };

    [[nodiscard]] static size_t index_of(ServiceID id) {
        return static_cast<size_t>(id.get_value() - 1);
    }

    [[nodiscard]] size_t checked_index(ServiceID id, const char* who) const {
        const int value = id.get_value();
        if (value < 1 || static_cast<size_t>(value) > services_.size()) {
            throw PreconditionAssertion(std::string("ServiceRegistry::") + who + ": invalid ServiceID", __FILE__, __LINE__);
        }
        return static_cast<size_t>(value - 1);
    }

    void register_service(const std::string& name, ServiceEndpoints endpoints, const char* who) {
        if (name.empty()) {
            throw PreconditionAssertion(std::string("ServiceRegistry::") + who + ": service name must not be empty", __FILE__, __LINE__);
        }
        if (name_to_id_.count(name) != 0) {
            throw PreconditionAssertion(std::string("ServiceRegistry::") + who + ": duplicate service name: " + name, __FILE__, __LINE__);
        }
        const ServiceID id{static_cast<int>(services_.size()) + 1};
        services_.push_back(ServiceEntry{name, std::move(endpoints)});
        name_to_id_.emplace(name, id);
    }

    std::vector<ServiceEntry> services_; // index (id - 1) -> entry
    std::unordered_map<std::string, ServiceID> name_to_id_;
};

} // namespaces
