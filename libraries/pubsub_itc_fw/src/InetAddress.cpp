#include <arpa/inet.h>  // For inet_pton, inet_ntop
#include <netdb.h>      // For getaddrinfo, freeaddrinfo, gai_strerror
#include <netinet/in.h> // For sockaddr_in, sockaddr_in6
#include <sys/socket.h> // For sockaddr, sockaddr_storage, AF_INET, AF_INET6, socklen_t
#include <unistd.h>     // For close

// C++ headers whose names start with ‘c’
#include <cstdint> // For uint8_t, uint16_t
#include <cstring> // For memset
#include <cerrno>  // For errno

// System C++ headers
#include <algorithm> // For std::min
#include <memory>    // For std::unique_ptr, std::make_unique
#include <string>    // For std::string
#include <tuple>     // For std::tuple
#include <utility>   // for std::move
#include <array>

// Third party headers
#include <fmt/format.h> // For fmt::format for error messages

// Project headers
#include <pubsub_itc_fw/IpAddressInterface.hpp> // Header for this class
#include <pubsub_itc_fw/InetAddress.hpp> // Header for this class

namespace pubsub_itc_fw {

// Define the maximum length for an IP address string.
// IPv6 can be quite long, e.g., "fe80:0000:0000:0000:0202:b3ff:fe1e:8329%eth0"
// INET6_ADDRSTRLEN is typically 46, but this includes the null terminator.
// We allow extra space for potential scope IDs.
constexpr int MaximumIpAddressStringLength = INET6_ADDRSTRLEN + 16;

InetAddress::InetAddress(const struct sockaddr* sockaddr_ptr, socklen_t sockaddr_len) : addr_len_(sockaddr_len) {
    if (sockaddr_ptr != nullptr && sockaddr_len > 0 && sockaddr_len <= sizeof(addr_storage_)) {
        // Copy the sockaddr structure into the internal storage.
        // Use memset first to clear any potential padding bytes, then memcpy.
        memset(&addr_storage_, 0, sizeof(addr_storage_));
        memcpy(&addr_storage_, sockaddr_ptr, sockaddr_len);
    } else {
        // Handle invalid input, though factory methods should prevent this.
        // For robustness, initialize to empty if an invalid pointer or length is passed.
        memset(&addr_storage_, 0, sizeof(addr_storage_));
        addr_len_ = 0;
    }
}

[[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> InetAddress::create(const std::string& ip_address_str, uint16_t port) {
    struct addrinfo hints{};
    struct addrinfo* result = nullptr; // Pointer to store the linked list of results

    // Clear hints structure
    memset(&hints, 0, sizeof(hints));

    // AI_PASSIVE for a listening socket, AI_NUMERICHOST to prevent DNS lookups if ip_address_str is numeric
    // For a client, we usually don't use AI_PASSIVE. For hostname, we want DNS.
    // If ip_address_str is empty, assume "0.0.0.0" or "::" for binding.
    if (ip_address_str.empty() || ip_address_str == "0.0.0.0" || ip_address_str == "::") {
        hints.ai_flags = AI_PASSIVE; // Use for server-side bind to all available interfaces
    } else {
        // For client or specific IP, do not use AI_PASSIVE.
        // AI_NUMERICHOST could be added if we only expect numeric IPs, but we want hostname resolution.
        hints.ai_flags = 0;
    }

    hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // We are primarily interested in stream sockets (TCP)

    const std::string port_str = fmt::format("{}", port); // Convert port to string for getaddrinfo

    const int status = getaddrinfo(ip_address_str.empty() ? nullptr : ip_address_str.c_str(), port_str.c_str(), &hints, &result);

    if (status != 0) {
        const std::string error_message = fmt::format("Failed to resolve address {}:{}. Error: {}", ip_address_str, port, gai_strerror(status));
        return {nullptr, error_message};
    }

    std::unique_ptr<InetAddress> new_address = nullptr;
    std::string error_detail;

    // Iterate through the results to find a suitable address.
    // Prefer IPv6 if available, then IPv4.
    for (struct addrinfo* ainfo = result; ainfo != nullptr; ainfo = ainfo->ai_next) {
        if (ainfo->ai_family == AF_INET6 || ainfo->ai_family == AF_INET) {
            new_address = std::unique_ptr<InetAddress>(new InetAddress(ainfo->ai_addr, ainfo->ai_addrlen));
            break; // Found a suitable address, prefer the first one.
        }
    }

    // Free the linked list of results
    freeaddrinfo(result);

    if (new_address) {
        return {std::move(new_address), ""};
    }
    error_detail = fmt::format("No suitable IPv4 or IPv6 address found for {}:{}.", ip_address_str, port);
    return {nullptr, error_detail};
}

[[nodiscard]] std::tuple<std::unique_ptr<InetAddress>, std::string> InetAddress::create(const struct sockaddr* sockaddr_ptr, socklen_t sockaddr_len) {
    if (sockaddr_ptr == nullptr || sockaddr_len == 0) {
        return {nullptr, "Invalid sockaddr pointer or zero length provided."};
    }

    if (sockaddr_len > sizeof(sockaddr_storage)) {
        return {nullptr, fmt::format("Provided sockaddr length ({}) exceeds internal storage capacity ({}).", sockaddr_len, sizeof(sockaddr_storage))};
    }

    // Use private constructor, then wrap in unique_ptr
    return {std::unique_ptr<InetAddress>(new InetAddress(sockaddr_ptr, sockaddr_len)), ""};
}

std::string InetAddress::get_ip_address_string() const {
    std::array<char, MaximumIpAddressStringLength> ipStringBuffer;
    const void* src_address = nullptr;

    if (addr_storage_.ss_family == AF_INET) {
        const auto* const ipv4_addr = reinterpret_cast<const struct sockaddr_in*>(&addr_storage_);
        src_address = &(ipv4_addr->sin_addr);
    } else if (addr_storage_.ss_family == AF_INET6) {
        const auto* const ipv6_addr = reinterpret_cast<const struct sockaddr_in6*>(&addr_storage_);
        src_address = &(ipv6_addr->sin6_addr);
    } else {
        // Unknown or invalid address family
        return "";
    }

    if (inet_ntop(addr_storage_.ss_family, src_address, ipStringBuffer.data(), MaximumIpAddressStringLength) == nullptr) {
        // Error converting address to string
        return "";
    }

    return {ipStringBuffer.data()};
}

uint16_t InetAddress::get_port() const {
    if (addr_storage_.ss_family == AF_INET) {
        const auto* const ipv4_addr = reinterpret_cast<const struct sockaddr_in*>(&addr_storage_);
        return ntohs(ipv4_addr->sin_port); // Convert from network byte order to host byte order
    }
    if (addr_storage_.ss_family == AF_INET6) {
        const auto* const ipv6_addr = reinterpret_cast<const struct sockaddr_in6*>(&addr_storage_);
        return ntohs(ipv6_addr->sin6_port); // Convert from network byte order to host byte order
    }
    return 0; // Unknown or invalid address family
}

bool InetAddress::is_ipv4() const {
    return addr_storage_.ss_family == AF_INET && addr_len_ == sizeof(sockaddr_in);
}

bool InetAddress::is_ipv6() const {
    return addr_storage_.ss_family == AF_INET6 && addr_len_ == sizeof(sockaddr_in6);
}

const struct sockaddr* InetAddress::get_sockaddr() const {
    if (addr_len_ == 0) {
        return nullptr; // Indicate invalid address
    }
    return reinterpret_cast<const struct sockaddr*>(&addr_storage_);
}

socklen_t InetAddress::get_sockaddr_size() const {
    return addr_len_;
}

bool InetAddress::is_equal(const IpAddressInterface& rhs) const {
    // If the rhs is not an InetAddress, we can't compare directly.
    // This assumes that only InetAddress concrete types will be compared.
    // If other concrete types of IpAddressInterface are introduced, this would need refinement.
    const auto* const other = dynamic_cast<const InetAddress*>(&rhs);
    if (other == nullptr) {
        return false; // Can only compare with other InetAddress objects
    }

    if (addr_storage_.ss_family != other->addr_storage_.ss_family) {
        return false; // Different address families are not equal
    }

    if (addr_len_ != other->addr_len_) {
        return false; // Different lengths are not equal
    }

    // Compare the raw address data
    // Use std::min to avoid reading past the smaller valid length if somehow addr_len_ is inconsistent
    return memcmp(&addr_storage_, &other->addr_storage_, std::min(addr_len_, other->addr_len_)) == 0;
}

bool InetAddress::is_less_than(const IpAddressInterface& rhs) const {
    const auto* const other = dynamic_cast<const InetAddress*>(&rhs);
    if (other == nullptr) {
        // If comparing with a different concrete type, perhaps use a predefined order
        // For now, assume any non-InetAddress is "greater" for ordering purposes.
        return true;
    }

    // 1. Compare by address family
    if (addr_storage_.ss_family < other->addr_storage_.ss_family) {
        return true;
    }
    if (addr_storage_.ss_family > other->addr_storage_.ss_family) {
        return false;
    }

    // 2. Address families are the same, compare by raw address bytes
    // Use std::min to handle potential length differences safely
    const int cmp = memcmp(&addr_storage_, &other->addr_storage_, std::min(addr_len_, other->addr_len_));
    if (cmp < 0) {
        return true;
    }
    if (cmp > 0) {
        return false;
    }

    // 3. Address bytes are identical (or one is a prefix of another), compare by length
    if (addr_len_ < other->addr_len_) {
        return true;
    }
    if (addr_len_ > other->addr_len_) {
        return false;
    }

    // 4. Everything else is identical, compare by port
    return get_port() < other->get_port();
}

} // namespace pubsub_itc_fw
