#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

/// The most characters a connection's service name may have. Every name the venue uses is far shorter.
inline constexpr size_t max_service_name_length = 64;

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
 *   set to "inbound:\<port\>" where \<port\> is the listener port that accepted
 *   the connection. This allows on_connection_established() to distinguish
 *   between multiple inbound listeners on the same ApplicationThread (for
 *   example, an order PDU listener and an execution report PDU listener).
 *
 *   Applications can use service_name().rfind("inbound:", 0) == 0 to
 *   distinguish inbound from outbound connections in on_connection_established().
 *
 * The service name is held in the object, in a fixed array of max_service_name_length characters, and not in a
 * std::string. A ConnectionID is copied into every command an application thread sends the reactor, and again onto the
 * reactor's queue, so a name that lived on the heap made two heap allocations for every message sent
 * (docs/bug_list.md, BUG-0270). Copying this object never allocates. A name built from a string literal is checked
 * against the limit when the code is compiled; any other name is checked when it is given, and one over the limit is
 * refused with a PreconditionAssertion. Every name the venue uses is made in its code, and each kind's longest form is
 * checked at compile time where it is made: ServiceRegistry::add for literals, the sequencer's gateway names, the
 * reactor's inbound names and the topic names.
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
    ConnectionID(int value, std::string_view service_name) : value_(value) {
        if (service_name.size() > max_service_name_length) {
            throw PreconditionAssertion("ConnectionID: the service name '" + std::string(service_name) + "' has " + std::to_string(service_name.size()) +
                                            " characters, more than the " + std::to_string(max_service_name_length) + " allowed",
                                        __FILE__, __LINE__);
        }
        std::copy(service_name.begin(), service_name.end(), service_name_.begin());
        service_name_length_ = static_cast<uint8_t>(service_name.size());
    }

    /**
     * @brief Constructs a ConnectionID whose service name is a string literal, checked against the limit when the code
     *        is compiled.
     *
     * @param[in] value        The integer value of the ID.
     * @param[in] service_name The name of the service, a string literal of at most max_service_name_length characters.
     */
    template <size_t length_with_terminator>
    ConnectionID(int value, const char (&service_name)[length_with_terminator])
        : ConnectionID(value, std::string_view(service_name, length_with_terminator - 1)) {
        static_assert(length_with_terminator - 1 <= max_service_name_length, "a service name may have at most max_service_name_length characters");
    }

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
     * @brief Returns the service name for this connection.
     *
     * For outbound connections, this is the service name passed to
     * connect_to_service() (for example, "gateway", "arbiter").
     *
     * For inbound connections, this is "inbound:\<port\>" where \<port\> is
     * the listener port that accepted the connection.
     *
     * Empty only for ConnectionID values used in internal reactor
     * bookkeeping that are never delivered as part of a
     * ConnectionEstablished event.
     */
    [[nodiscard]] std::string_view service_name() const {
        return std::string_view(service_name_.data(), service_name_length_);
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
    uint8_t service_name_length_{0};
    std::array<char, max_service_name_length> service_name_{};
};

// Copying a ConnectionID must never allocate: it is copied into every command sent to the reactor (BUG-0270).
static_assert(std::is_trivially_copyable_v<ConnectionID>, "a ConnectionID must be copied with no allocation");

} // namespaces

namespace std {

template <> struct hash<pubsub_itc_fw::ConnectionID> {
    [[nodiscard]] size_t operator()(const pubsub_itc_fw::ConnectionID& id) const {
        return std::hash<int>()(id.get_value());
    }
};

} // namespaces
