// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// A test library that makes one process's sends on chosen connections stop reaching the other
// end, from the moment a flag file appears, while the process carries on as though they had
// been sent.
//
// It exists to test what happens when a leading sequencer dies holding records its follower
// never received (BUG-0103). Stopping the follower with SIGSTOP does not produce that: what the
// leader sends while the follower is stopped waits in the follower's kernel receive buffer, and
// the follower reads it as soon as it runs again. Killing the leader at the right microsecond,
// between sending an order to the matching engine and sending its record to the follower, cannot
// be done on purpose. This library makes the gap as long as the test needs: once the flag file
// exists, nothing more the leader sends to its peer arrives, so every order it takes from then on
// is held by the leader and the engine and by nothing else.
//
// It is loaded into one process only, with LD_PRELOAD, and is configured by two environment
// variables. Without both, it passes every send straight through and does nothing else:
//
//   PUBSUB_TEST_BLOCK_PORTS   the ports to block, separated by commas. A connection is blocked
//                             if either its local port or its remote port is one of them, so the
//                             listening port of each side covers both the connection this process
//                             accepted and the one it opened.
//   PUBSUB_TEST_BLOCK_FLAG    the path of the flag file. Sends pass through until it exists.
//
// For example, with everything on one command line:
//
//   LD_PRELOAD=lib/libblock_sends_to_ports.so PUBSUB_TEST_BLOCK_PORTS=11003,11004 PUBSUB_TEST_BLOCK_FLAG=/path/to/flag bin/sequencer ...
//
// It replaces send(), which is the only call the framework uses to write to a TCP socket
// (TcpSocket::send, and the TLS handler's sends). A blocked send reports every byte as sent, so
// the process does not retry or fail. Blocking is all at once and never undone: once the flag
// has been seen, no later send on a blocked connection arrives, so the other end receives a whole
// number of the frames sent before the flag appeared and nothing after.
//
// It writes one line to standard error when blocking begins, which the test reads as evidence
// that the block really was in force rather than assuming it.
//
// It is not for use in a live venue.

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <string_view>

namespace {

using SendFunction = ssize_t (*)(int, const void*, size_t, int);

struct Configuration {
    std::set<uint16_t> blocked_ports;
    std::string flag_path;
    SendFunction real_send{nullptr};
};

std::set<uint16_t> parse_ports(const char* text) {
    std::set<uint16_t> ports;
    if (text == nullptr) {
        return ports;
    }
    std::string_view remaining(text);
    while (!remaining.empty()) {
        const size_t comma = remaining.find(',');
        const std::string port_text(remaining.substr(0, comma));
        const long port = std::strtol(port_text.c_str(), nullptr, 10);
        if (port > 0 && port <= 65535) {
            ports.insert(static_cast<uint16_t>(port));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(comma + 1);
    }
    return ports;
}

// Read once, on the first send, which happens long after the process has started and its
// environment is settled. A function-local static is initialised exactly once even when several
// threads send at the same moment.
const Configuration& configuration() {
    static const Configuration loaded = [] {
        Configuration result;
        result.blocked_ports = parse_ports(std::getenv("PUBSUB_TEST_BLOCK_PORTS"));
        const char* flag = std::getenv("PUBSUB_TEST_BLOCK_FLAG");
        result.flag_path = flag == nullptr ? "" : flag;
        result.real_send = reinterpret_cast<SendFunction>(dlsym(RTLD_NEXT, "send"));
        return result;
    }();
    return loaded;
}

std::atomic<bool> blocking{false};

uint16_t port_of(const sockaddr_storage& address) {
    if (address.ss_family == AF_INET) {
        sockaddr_in ipv4{};
        std::memcpy(&ipv4, &address, sizeof(ipv4));
        return ntohs(ipv4.sin_port);
    }
    if (address.ss_family == AF_INET6) {
        sockaddr_in6 ipv6{};
        std::memcpy(&ipv6, &address, sizeof(ipv6));
        return ntohs(ipv6.sin6_port);
    }
    return 0;
}

bool connection_is_blocked(int file_descriptor, const std::set<uint16_t>& blocked_ports) {
    sockaddr_storage local{};
    socklen_t local_length = sizeof(local);
    if (getsockname(file_descriptor, reinterpret_cast<sockaddr*>(&local), &local_length) == 0 && blocked_ports.count(port_of(local)) != 0) {
        return true;
    }
    sockaddr_storage remote{};
    socklen_t remote_length = sizeof(remote);
    return getpeername(file_descriptor, reinterpret_cast<sockaddr*>(&remote), &remote_length) == 0 && blocked_ports.count(port_of(remote)) != 0;
}

// Whether blocking is in force, checking for the flag file until it is. The check is one system
// call per send while the flag is absent, which is acceptable in the one process under test.
bool blocking_has_begun(const Configuration& config) {
    if (blocking.load(std::memory_order_acquire)) {
        return true;
    }
    if (access(config.flag_path.c_str(), F_OK) != 0) {
        return false;
    }
    if (!blocking.exchange(true, std::memory_order_acq_rel)) {
        constexpr std::string_view announcement = "block_sends_to_ports: the flag file exists; sends to the blocked ports are now discarded\n";
        [[maybe_unused]] const ssize_t written = write(STDERR_FILENO, announcement.data(), announcement.size());
    }
    return true;
}

} // namespaces

extern "C" ssize_t send(int file_descriptor, const void* data, size_t size, int flags) {
    const Configuration& config = configuration();
    if (config.real_send == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    if (config.blocked_ports.empty() || config.flag_path.empty()) {
        return config.real_send(file_descriptor, data, size, flags);
    }
    if (blocking_has_begun(config) && connection_is_blocked(file_descriptor, config.blocked_ports)) {
        return static_cast<ssize_t>(size);
    }
    return config.real_send(file_descriptor, data, size, flags);
}
