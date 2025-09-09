#pragma once

// C headers
#include <cstdint> // For uint8_t

// System C++ headers
#include <string> // For std::string
#include <tuple>  // For std::tuple
#include <vector> // For potential raw address buffer

// Third party headers
// (None directly here, as per Pimpl for dependencies)

// Project headers
// (No specific project headers needed for this base interface)

// Forward declaration for system socket address structure
struct sockaddr;

namespace pubsub_itc_fw {

/**
 * @brief Abstract interface for an Internet Protocol (IP) address and port.
 *
 * This interface provides a common abstraction for handling network endpoints,
 * supporting both IPv4 and IPv6 without exposing the underlying system-specific
 * `sockaddr_in` or `sockaddr_in6` details. It encapsulates the core
 * functionalities expected from an IP address object, such as retrieving
 * string representation, port, and access to the raw address structure for
 * system calls.
 *
 * This design is inspired by the `ACE_INET_Addr` class from the ACE framework,
 * focusing solely on Internet addresses as domain sockets are not in scope.
 */
class IpAddressInterface {
  public:
    // Destructor is first, as per coding style, and must be virtual for an interface.
    /**
     * @brief Virtual destructor for the IpAddressInterface.
     * Ensures proper cleanup of derived concrete classes.
     */
    virtual ~IpAddressInterface() = default;

    /**
     * @brief Returns the IP address as a human-readable string.
     *
     * This method converts the internal binary IP address representation
     * into a string format (e.g., "192.168.1.1" for IPv4, "::1" for IPv6).
     * @returns A `std::string` containing the IP address.
     */
    virtual std::string get_ip_address_string() const = 0;

    /**
     * @brief Returns the port number associated with this IP address.
     *
     * @returns A `uint16_t` representing the port number.
     */
    virtual uint16_t get_port() const = 0;

    /**
     * @brief Checks if the encapsulated address is an IPv4 address.
     *
     * @returns `true` if the address is IPv4, `false` otherwise.
     */
    virtual bool is_ipv4() const = 0;

    /**
     * @brief Checks if the encapsulated address is an IPv6 address.
     *
     * @returns `true` if the address is IPv6, `false` otherwise.
     */
    virtual bool is_ipv6() const = 0;

    /**
     * @brief Provides a pointer to the underlying system `sockaddr` structure.
     *
     * This is essential for low-level socket API calls like `bind()`, `connect()`,
     * `sendto()`, and `recvfrom()`.
     * @returns A constant pointer to the `sockaddr` structure. The caller must
     * not modify the returned pointer or its contents.
     */
    virtual const struct sockaddr* get_sockaddr() const = 0;

    /**
     * @brief Returns the size of the underlying system `sockaddr` structure.
     *
     * This size is needed for system calls that operate on `sockaddr` structures.
     * @returns A `socklen_t` representing the size of the `sockaddr` structure.
     */
    virtual socklen_t get_sockaddr_size() const = 0;

    /**
     * @brief Compares this IP address with another for equality.
     *
     * This comparison includes both the IP address and the port number.
     * @param[in] rhs The other `IpAddressInterface` to compare against.
     * @returns `true` if the addresses and ports are equal, `false` otherwise.
     */
    virtual bool is_equal(const IpAddressInterface& rhs) const = 0;

    /**
     * @brief Compares this IP address with another for ordering.
     *
     * This allows `IpAddressInterface` objects to be used in ordered
     * containers (e.g., `std::map`, `std::set`). The specific ordering
     * is implementation-defined but must be consistent.
     * @param[in] rhs The other `IpAddressInterface` to compare against.
     * @returns `true` if this address is less than `rhs`, `false` otherwise.
     */
    virtual bool is_less_than(const IpAddressInterface& rhs) const = 0;
};

// --- Non-member comparison operators for convenience ---
inline bool operator==(const IpAddressInterface& lhs, const IpAddressInterface& rhs) {
    return lhs.is_equal(rhs);
}

inline bool operator!=(const IpAddressInterface& lhs, const IpAddressInterface& rhs) {
    return !lhs.is_equal(rhs);
}

inline bool operator<(const IpAddressInterface& lhs, const IpAddressInterface& rhs) {
    return lhs.is_less_than(rhs);
}

inline bool operator<=(const IpAddressInterface& lhs, const IpAddressInterface& rhs) {
    return lhs.is_less_than(rhs) || lhs.is_equal(rhs);
}

inline bool operator>(const IpAddressInterface& lhs, const IpAddressInterface& rhs) {
    return !lhs.is_less_than(rhs) && !lhs.is_equal(rhs);
}

inline bool operator>=(const IpAddressInterface& lhs, const IpAddressInterface& rhs) {
    return !lhs.is_less_than(rhs);
}

} // namespace pubsub_itc_fw
