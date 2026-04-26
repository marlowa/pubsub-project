#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <functional>
#include <string>

namespace pubsub_itc_fw {

/**
 * @brief A strongly typed identifier for a TCP connection managed by the Reactor.
 *
 * ConnectionIDs are assigned by the Reactor when a connection is established,
 * either inbound (via TcpAcceptor) or outbound (via TcpConnector). The integer
 * value is monotonically increasing starting from 1. A value of zero indicates
 * an invalid or unestablished connection (see is_valid()).
 *
 * Service name:
 *   For outbound connections (established via connect_to_service()), the service
 *   name is set to the name passed to connect_to_service() and is carried through
 *   to the ConnectionEstablished event delivered to the ApplicationThread. This
 *   allows on_connection_established() to identify which outbound service the
 *   connection belongs to without any application-level bookkeeping.
 *
 *   For inbound connections (accepted by a TcpAcceptor), the service name is
 *   always empty. Applications can use service_name().empty() to distinguish
 *   inbound from outbound connections in on_connection_established().
 *
 * Backward compatibility:
 *   All existing construction sites using ConnectionID{} and ConnectionID{n}
 *   remain valid -- both constructors are preserved. The service name defaults
 *   to empty and is only populated by the reactor when delivering outbound
 *   ConnectionEstablished events.
 */
class ConnectionID {
  public:
    /**
     * @brief Constructs an invalid (zero) ConnectionID with no service name.
     */
    ConnectionID() : value_(0) {}

    /**
     * @brief Constructs a ConnectionID from an integer value with no service name.
     *
     * Used for inbound connections and internal reactor bookkeeping where the
     * service name is not relevant.
     *
     * @param[in] value The integer value of the ID.
     */
    explicit ConnectionID(int value) : value_(value) {}

    /**
     * @brief Constructs a ConnectionID with both an integer value and a service name.
     *
     * Used by the OutboundConnectionManager when delivering ConnectionEstablished
     * events so that on_connection_established() can identify which outbound
     * service the connection belongs to.
     *
     * @param[in] value        The integer value of the ID.
     * @param[in] service_name The name of the outbound service.
     */
    ConnectionID(int value, std::string service_name)
        : value_(value)
        , service_name_(std::move(service_name))
    {}

    /**
     * @brief Checks if the ID is valid (non-zero).
     * @return true if the ID is valid, false otherwise.
     */
    [[nodiscard]] bool is_valid() const {
        return value_ != 0;
    }

    /**
     * @brief Returns the integer value of the ID.
     */
    [[nodiscard]] int get_value() const {
        return value_;
    }

    /**
     * @brief Returns the service name for outbound connections.
     *
     * Empty for inbound connections and for ConnectionID values used in
     * internal reactor bookkeeping. Populated when the reactor delivers a
     * ConnectionEstablished event for an outbound connection.
     */
    [[nodiscard]] const std::string& service_name() const {
        return service_name_;
    }

    [[nodiscard]] bool operator==(const ConnectionID& other) const {
        return value_ == other.value_;
    }

    [[nodiscard]] bool operator!=(const ConnectionID& other) const {
        return value_ != other.value_;
    }

    [[nodiscard]] bool operator<(const ConnectionID& other) const {
        return value_ < other.value_;
    }

    ConnectionID& operator++() {
        ++value_;
        return *this;
    }

    ConnectionID operator++(int) {
        ConnectionID temp(*this);
        ++value_;
        return temp;
    }

  private:
    int value_;
    std::string service_name_;
};

} // namespace pubsub_itc_fw

namespace std {

template <> struct hash<pubsub_itc_fw::ConnectionID> {
    [[nodiscard]] std::size_t operator()(const pubsub_itc_fw::ConnectionID& id) const {
        return std::hash<int>()(id.get_value());
    }
};

} // namespace std
