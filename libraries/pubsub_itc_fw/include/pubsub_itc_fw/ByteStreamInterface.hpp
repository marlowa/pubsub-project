#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// C++ headers whose names start with 'c'
#include <cstddef>
#include <cstdint> // IWYU pragma: keep

// System C++ headers
#include <memory> // For std::unique_ptr (for get_peer_address)
#include <string> // For std::string in error messages
#include <tuple>  // For std::tuple return types

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/InetAddress.hpp> // For get_peer_address
#include <pubsub_itc_fw/SimpleSpan.hpp>  // For SimpleSpan

namespace pubsub_itc_fw {

/**
 * @brief Abstract interface for generic byte-stream communication.
 *
 * This interface defines fundamental operations for sending and receiving
 * binary data over a connection. It serves as a polymorphic base for
 * concrete implementations like `TcpSocket` (for raw TCP) and future
 * `SslStream` (for encrypted TLS connections), allowing the application
 * to interact with different connection types uniformly.
 *
 * All methods are designed to be non-blocking where possible, returning
 * status indicating bytes processed or error codes, and do not throw exceptions.
 * Error reporting is done via `std::tuple<int, std::string>` or
 * `std::tuple<std::unique_ptr<T>, std::string>`.
 */
class ByteStreamInterface {
  public:
    /**
     * @brief Virtual destructor for ByteStreamInterface.
     * Ensures proper cleanup of derived concrete classes.
     */
    virtual ~ByteStreamInterface() = default;

    /**
     * @brief Sends a specified amount of binary data over the stream.
     *
     * This method attempts to send the data and returns the number of bytes
     * successfully sent. It should be non-blocking; if the send buffer is full,
     * it will return 0 or a negative error code (e.g., -EWOULDBLOCK) along with
     * an error string, rather than blocking the calling thread.
     *
     * @param[in] data A `SimpleSpan<const uint8_t>` representing the binary
     * data to be sent. The data is treated as read-only.
     * @return A `std::tuple` containing:
     * - An `int` indicating the number of bytes sent (>= 0) on success,
     * or a negative value (representing an `errno` code) on a non-recoverable
     * error or if the operation would block.
     * - A `std::string` containing an error message if the operation failed.
     * It will be empty on success.
     */
    [[nodiscard]] virtual std::tuple<int, std::string> send(SimpleSpan<const uint8_t> data) = 0;

    /// The most parts send_gathered() accepts in one call (docs/framework/gathering_sends.md, decision 2).
    static constexpr size_t max_gathered_parts = 64;

    /**
     * @brief Sends several separate pieces of data as one stream, in order, in as few system calls as the stream
     *        allows (docs/framework/gathering_sends.md).
     *
     * Whatever the stream takes is a prefix of the pieces joined end to end: never a later piece while an earlier one is
     * not wholly taken. So the number returned says exactly where writing stopped.
     *
     * This implementation sends each piece with send() in turn and stops at the first that is not taken whole. A stream
     * that can write several pieces in one system call overrides it.
     *
     * @param[in] parts The pieces, in order. Each must be non-empty.
     * @param[in] count How many pieces: at least one, and at most max_gathered_parts.
     * @return A `std::tuple` containing:
     * - the number of bytes taken (>= 0), counted from the start of the first piece; or, when nothing was taken, a
     *   negative `errno` code: -EAGAIN when the stream has no room, any other for a failure;
     * - an error message when the call failed; empty otherwise.
     */
    [[nodiscard]] virtual std::tuple<int64_t, std::string> send_gathered(const SimpleSpan<const uint8_t>* parts, size_t count) {
        int64_t taken = 0;
        for (size_t index = 0; index < count; ++index) {
            auto [sent, error] = send(parts[index]);
            if (sent < 0) {
                // Bytes already taken are reported; the failure comes back on the next call, as sendmsg() does.
                return taken > 0 ? std::tuple<int64_t, std::string>{taken, ""} : std::tuple<int64_t, std::string>{sent, error};
            }
            taken += sent;
            if (static_cast<size_t>(sent) < parts[index].size()) {
                break;
            }
        }
        return {taken, ""};
    }

    /**
     * @brief Receives binary data from the stream into the provided buffer.
     *
     * This method attempts to read data into the buffer and returns the number
     * of bytes successfully received. It should be non-blocking; if no data
     * is immediately available, it will return 0 or a negative error code
     * (e.g., -EWOULDBLOCK) along with an error string.
     *
     * @param[out] buffer A `SimpleSpan<uint8_t>` representing the buffer
     * where received data will be stored.
     * @return A `std::tuple` containing:
     * - An `int` indicating the number of bytes received (>= 0) on success.
     * A value of 0 typically means the peer has closed its writing half
     * of the connection. A negative value (representing an `errno` code)
     * indicates a non-recoverable error or if the operation would block.
     * - A `std::string` containing an error message if the operation failed.
     * It will be empty on success.
     */
    [[nodiscard]] virtual std::tuple<int, std::string> receive(SimpleSpan<uint8_t> buffer) = 0;

    /**
     * @brief Closes the byte stream connection.
     *
     * This method releases any underlying resources (e.g., socket file descriptor).
     * After this call, no further send or receive operations should be attempted.
     */
    virtual void close() = 0;

    /**
     * @brief Retrieves the remote (peer) network address associated with this stream.
     *
     * This allows the client to identify the other end of the connection.
     *
     * @returns A `std::tuple` containing:
     * - A `std::unique_ptr<InetAddress>` to the peer's network address
     * on success. This pointer will be `nullptr` on failure.
     * - A `std::string` containing an error message if retrieving the
     * address failed. It will be empty on success.
     */
    [[nodiscard]] virtual std::tuple<std::unique_ptr<InetAddress>, std::string> get_peer_address() const = 0;
};

} // namespaces
