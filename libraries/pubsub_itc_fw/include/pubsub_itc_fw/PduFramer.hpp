#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * PduFramer is responsible for the send side of the TCP framing layer.
 *
 * It prepends a PduHeader to each payload and writes the complete frame
 * to a ByteStreamInterface. Because the underlying socket is non-blocking,
 * a send() call may transfer fewer bytes than requested. PduFramer buffers
 * any unsent remainder in a fixed-size internal buffer and completes the
 * write on subsequent calls to continue_send(), which the reactor invokes
 * when the socket becomes writable.
 *
 * No heap allocation is performed. The send buffer is a fixed-size array
 * sized to hold one complete frame (header + maximum payload).
 *
 * Threading model:
 *   All methods must be called from the reactor thread only.
 *
 * Ownership:
 *   PduFramer does not own the ByteStreamInterface. The caller is responsible
 *   for the lifetime of the stream.
 */

#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>

#include <arpa/inet.h>

#include <pubsub_itc_fw/ByteStreamInterface.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Send-side framing layer for PDU transmission over TCP.
 *
 * Constructs and transmits PDU frames with no heap allocation.
 * Handles partial writes transparently via a fixed-size internal buffer.
 * The reactor must call continue_send() when the socket becomes writable
 * and has_pending_data() returns true.
 */
class PduFramer {
public:
    /**
     * @brief Maximum supported payload size.
     *
     * Sized to cover all leader-follower PDUs with room to spare.
     * Variable-length PDU support is a future extension.
     */
    static constexpr size_t max_payload_size = 256;

    /**
     * @brief Total frame buffer size: header + maximum payload.
     */
    static constexpr size_t frame_buffer_size = sizeof(PduHeader) + max_payload_size;

    ~PduFramer() = default;

    /**
     * @brief Constructs a PduFramer over the given byte stream.
     *
     * @param[in] stream The underlying non-blocking byte stream. Must outlive this object.
     */
    explicit PduFramer(ByteStreamInterface& stream);

    PduFramer(const PduFramer&) = delete;
    PduFramer& operator=(const PduFramer&) = delete;

    /**
     * @brief Frames and sends a PDU payload.
     *
     * Prepends a PduHeader (in network byte order) to the payload and attempts
     * to write the complete frame to the stream. If the stream cannot accept
     * all bytes, the remainder is held in the internal fixed-size buffer.
     * The reactor must call continue_send() when the socket next becomes writable.
     *
     * Must not be called while has_pending_data() is true — the previous frame
     * must be fully sent before a new one is submitted.
     *
     * @param[in] pdu_id   DSL message ID.
     * @param[in] version  Message version.
     * @param[in] payload  Pointer to the encoded payload bytes. Must not be nullptr.
     * @param[in] size     Size of the payload in bytes. Must be greater than zero
     *                     and must not exceed max_payload_size.
     * @return A tuple of { success, error_string }.
     */
    [[nodiscard]] std::tuple<bool, std::string> send(int16_t pdu_id, int8_t version,
                                                     const uint8_t* payload, uint32_t size);

    /**
     * @brief Continues sending any buffered data after a partial write.
     *
     * Must be called by the reactor when the socket becomes writable and
     * has_pending_data() returns true.
     *
     * @return A tuple of { success, error_string }.
     *         success is true if all pending data has been sent or progress was made.
     *         success is false if a non-recoverable error occurred.
     */
    [[nodiscard]] std::tuple<bool, std::string> continue_send();

    /**
     * @brief Returns true if there is buffered data waiting to be sent.
     *
     * When true, the reactor must register EPOLLOUT for this socket and
     * call continue_send() when it becomes writable.
     */
    [[nodiscard]] bool has_pending_data() const;

private:
    ByteStreamInterface& stream_;
    uint8_t frame_buffer_[frame_buffer_size];
    size_t frame_size_{0};
    size_t send_offset_{0};
};

} // namespace pubsub_itc_fw
