#pragma once

// C headers
#include <arpa/inet.h>  // For inet_pton, inet_ntop
#include <cstdint>      // For uint8_t, uint16_t
#include <netinet/in.h> // For sockaddr_in, sockaddr_in6
#include <sys/socket.h> // For sockaddr, sockaddr_storage, AF_INET, AF_INET6, socklen_t

// System C++ headers
#include <string> // For std::string
#include <tuple>  // For std::tuple (used in error returns)
#include <vector> // For potential raw address buffer

// Project headers
#include <pubsub_itc_fw/IpAddressInterface.hpp> // Base interface

namespace pubsub_itc_fw {

/**
 * @brief Concrete implementation of `IpAddressInterface` for Internet Protocol addresses.
 *
 * This class encapsulates both IPv4 and IPv6 addresses and their associated port numbers,
 * providing a unified, IP-version-agnostic interface. It uses `sockaddr_storage`
 * internally to hold the underlying system socket address structure, enabling
 * seamless handling of different IP versions without explicit casting at the API level.
 *
 * It provides constructors for initialization from hostnames/IP strings and port,
 * or from existing system `sockaddr` structures, making it suitable for both
 * active (client) and passive (server) connection management.
 */
class InetAddress final : public IpAddressInterface {
  public:
    // Destructor is first, as per coding style.
    /**
     * @brief Default virtual destructor for `InetAddress`.
     * Ensures proper cleanup and allows for polymorphic destruction.
     */
    ~InetAddress() override = default;

    /**
     * @brief Static factory method to construct an `InetAddress` from an IP address string and a port number.
     *
     * This method attempts to resolve the given IP address string (which can be
     * an IPv4 dotted-decimal, IPv6 hexadecimal, or a hostname) and port.
     * It uses `getaddrinfo` to perform resolution, preferentially trying IPv6, then falling back to IPv4.
     *
     * @param[in] ip_address_str The IP address or hostname string.
     * @param[in] port The port number.
     * @return A `std::unique_ptr<InetAddress>` if successful, or `nullptr` on failure,
     * along with an error string.
     */
    [[nodiscard]] static std::tuple<std::unique_ptr<InetAddress>, std::string> create(const std::string& ip_address_str, uint16_t port);

    /**
     * @brief Static factory method to construct an `InetAddress` from an existing system `sockaddr` structure.
     *
     * This constructor is typically used when an address is obtained from low-level
     * socket calls like `accept()` or `recvfrom()`. It ensures the `sockaddr_ptr` is valid
     * and its length is appropriate for internal storage.
     *
     * @param[in] sockaddr_ptr A pointer to the system `sockaddr` structure.
     * @param[in] sockaddr_len The size of the `sockaddr` structure.
     * @return A `std::unique_ptr<InetAddress>` if successful, or `nullptr` on failure,
     * along with an error string.
     */
    [[nodiscard]] static std::tuple<std::unique_ptr<InetAddress>, std::string> create(const struct sockaddr* sockaddr_ptr, socklen_t sockaddr_len);

    /**
     * @brief Returns the IP address as a human-readable string.
     *
     * Converts the internal binary IP address representation into a string format
     * (e.g., "192.168.1.1" for IPv4, "::1" for IPv6). It handles both IPv4 and IPv6
     * addresses stored in `addr_storage_`.
     * @returns A `std::string` containing the IP address, or an empty string on error.
     */
    std::string get_ip_address_string() const override;

    /**
     * @brief Returns the port number associated with this IP address.
     *
     * Extracts the port number from the internal `sockaddr_storage` structure.
     * The port is returned in host byte order.
     * @returns A `uint16_t` representing the port number, or 0 if address family is unknown.
     */
    uint16_t get_port() const override;

    /**
     * @brief Checks if the encapsulated address is an IPv4 address.
     * @returns `true` if the address is IPv4, `false` otherwise.
     */
    bool is_ipv4() const override;

    /**
     * @brief Checks if the encapsulated address is an IPv6 address.
     * @returns `true` if the address is IPv6, `false` otherwise.
     */
    bool is_ipv6() const override;

    /**
     * @brief Provides a pointer to the underlying system `sockaddr` structure.
     * @returns A constant pointer to the `sockaddr` structure, or `nullptr` if the address is invalid.
     */
    const struct sockaddr* get_sockaddr() const override;

    /**
     * @brief Returns the size of the underlying system `sockaddr` structure.
     * @returns A `socklen_t` representing the size of the `sockaddr` structure.
     */
    socklen_t get_sockaddr_size() const override;

    /**
     * @brief Compares this `InetAddress` with another `IpAddressInterface` for equality.
     *
     * This comparison includes both the IP address bytes and the port number.
     * It handles different IP versions and ensures a robust comparison.
     * @param[in] rhs The other `IpAddressInterface` to compare against.
     * @returns `true` if the addresses and ports are equal, `false` otherwise.
     */
    bool is_equal(const IpAddressInterface& rhs) const override;

    /**
     * @brief Compares this `InetAddress` with another `IpAddressInterface` for ordering.
     *
     * This allows `IpAddressInterface` objects to be used in ordered
     * containers (e.g., `std::map`, `std::set`). The ordering is primarily
     * by address family, then by raw address bytes, then by port.
     * @param[in] rhs The other `IpAddressInterface` to compare against.
     * @returns `true` if this address is less than `rhs`, `false` otherwise.
     */
    bool is_less_than(const IpAddressInterface& rhs) const override;

  private:
    /**
     * @brief Private constructor to be used by the static factory methods.
     * Initializes the internal `sockaddr_storage` with the provided address data.
     * This constructor is used by the static factory methods.
     * @param[in] sockaddr_ptr A pointer to the system `sockaddr` structure.
     * @param[in] sockaddr_len The size of the `sockaddr` structure.
     */
    InetAddress(const struct sockaddr* sockaddr_ptr, socklen_t sockaddr_len);

    /**
     * @brief The internal storage for the IP address and port.
     * `sockaddr_storage` is large enough to hold either `sockaddr_in` (IPv4)
     * or `sockaddr_in6` (IPv6) structures, making the class IP-version-agnostic.
     */
    struct sockaddr_storage addr_storage_;

    /**
     * @brief Stores the actual length of the address stored in `addr_storage_`.
     */
    socklen_t addr_len_;
};

} // namespace pubsub_itc_fw
