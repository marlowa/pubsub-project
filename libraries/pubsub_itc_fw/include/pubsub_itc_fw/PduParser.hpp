#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * PduParser is responsible for the receive side of the TCP framing layer.
 *
 * It reads bytes from a ByteStreamInterface and reassembles complete PDU frames
 * from the byte stream. Because TCP is a stream protocol, a single recv() call
 * may deliver a partial header, a partial payload, multiple frames, or any
 * combination. PduParser accumulates bytes in a fixed-size internal buffer
 * until a complete frame is available.
 *
 * On a complete frame:
 *   - The canary is validated. A mismatch causes the connection to be closed.
 *   - The pdu_id and payload are extracted.
 *   - A FrameworkPdu EventMessage is dispatched to the target ApplicationThread.
 *
 * On peer disconnect (recv returns 0):
 *   - The disconnect callback is invoked so the reactor can clean up.
 *
 * Threading model:
 *   All methods must be called from the reactor thread only.
 *
 * Ownership:
 *   PduParser does not own the ByteStreamInterface or the ApplicationThread.
 *   The caller is responsible for their lifetimes.
 *
 * Buffer sizing:
 *   The receive buffer is sized to hold one complete frame (header + payload).
 *   For the leader-follower protocol all PDUs are fixed-size and small, so a
 *   buffer of PduParser::max_payload_size bytes plus sizeof(PduHeader) suffices.
 *   For variable-length PDUs the slab allocator will be used instead; that
 *   path is not yet implemented.
 */

#include <cstdint>
#include <functional>
#include <string>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ByteStreamInterface.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Receive-side framing layer for PDU reassembly over TCP.
 *
 * Accumulates bytes from a non-blocking stream and dispatches complete
 * PDU frames to the owning ApplicationThread as FrameworkPdu EventMessages.
 */
class PduParser {
public:
    /**
     * @brief Maximum supported payload size for fixed-size PDUs.
     *
     * Sized conservatively to cover all leader-follower PDUs with room to spare.
     * Variable-length PDU support (using the slab allocator) is a future extension.
     */
    static constexpr size_t max_payload_size = 256;

    /**
     * @brief Total receive buffer size: header + maximum payload.
     */
    static constexpr size_t receive_buffer_size = sizeof(PduHeader) + max_payload_size;

    ~PduParser() = default;

    /**
     * @brief Constructs a PduParser.
     *
     * @param[in] stream             The underlying non-blocking byte stream. Must outlive this object.
     * @param[in] target_thread      The ApplicationThread to which complete PDUs are dispatched.
     * @param[in] disconnect_handler Called when the peer closes the connection gracefully.
     */
    PduParser(ByteStreamInterface& stream,
              ApplicationThread& target_thread,
              std::function<void()> disconnect_handler);

    PduParser(const PduParser&) = delete;
    PduParser& operator=(const PduParser&) = delete;

    /**
     * @brief Reads available bytes from the stream and dispatches any complete frames.
     *
     * Must be called by the reactor when the socket is readable (EPOLLIN).
     * May dispatch zero, one, or multiple FrameworkPdu events in a single call
     * if multiple complete frames are available in the stream.
     *
     * @return A tuple of { success, error_string }.
     *         success is false if the connection must be closed (canary mismatch,
     *         read error, or peer disconnect).
     */
    [[nodiscard]] std::tuple<bool, std::string> receive();

private:
    [[nodiscard]] bool has_complete_header() const;
    [[nodiscard]] bool has_complete_payload() const;
    void dispatch_pdu();
    void consume_frame();
    void reset_buffer();

    ByteStreamInterface& stream_;
    ApplicationThread& target_thread_;
    std::function<void()> disconnect_handler_;

    uint8_t buffer_[receive_buffer_size];
    size_t bytes_in_buffer_{0};
    uint32_t current_payload_size_{0};
    bool header_validated_{false};
};

} // namespace pubsub_itc_fw
