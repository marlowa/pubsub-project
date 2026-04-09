#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * PduFramer is responsible for the send side of the TCP framing layer.
 *
 * It writes PDU frames to a ByteStreamInterface and handles partial writes
 * transparently. Because the underlying socket is non-blocking, a send() call
 * may transfer fewer bytes than requested. PduFramer tracks the unsent
 * remainder and completes the write on subsequent calls to continue_send(),
 * which the reactor invokes when the socket becomes writable (EPOLLOUT).
 *
 * Two send modes:
 *
 *   Built mode (send()):
 *     The framer constructs the PduHeader internally, copies the caller's
 *     payload into a fixed-size internal buffer, and sends the complete
 *     frame. Suitable for small, fixed-size PDUs. No heap allocation.
 *     Maximum payload size is PduFramer::max_payload_size bytes.
 *
 *   Pre-built mode (send_prebuilt()):
 *     The caller has already assembled the complete frame (PduHeader followed
 *     immediately by the encoded payload) in a slab-allocated buffer. The
 *     framer stores only a pointer and size — no copy is performed. This is
 *     the zero-copy path used for outbound framework PDUs, where the
 *     application thread encodes directly into a slab chunk. The slab chunk
 *     must remain valid until has_pending_data() returns false, at which point
 *     the reactor deallocates it.
 *
 * In both modes, continue_send() and has_pending_data() behave identically.
 * The distinction between modes is encapsulated entirely within this class.
 *
 * Threading model:
 *   All methods must be called from the reactor thread only.
 *
 * Ownership:
 *   PduFramer does not own the ByteStreamInterface or the pre-built frame
 *   buffer. The caller is responsible for their lifetimes.
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
 * Supports two modes of operation:
 *
 *   - send(): framer builds the frame from header fields + payload into an
 *     internal fixed-size buffer. No heap allocation. Max payload is
 *     max_payload_size bytes.
 *
 *   - send_prebuilt(): caller provides a pre-assembled frame (PduHeader +
 *     payload) in a slab-allocated buffer. Zero-copy: the framer stores only
 *     a pointer, never copying the payload.
 *
 * In both modes, partial writes are handled transparently. The reactor calls
 * continue_send() when EPOLLOUT fires and has_pending_data() is true.
 */
class PduFramer {
public:
    /**
     * @brief Maximum payload size supported by the built mode (send()).
     *
     * Sized to cover all leader-follower protocol PDUs. Variable-length PDUs
     * that exceed this limit must use send_prebuilt() instead.
     */
    static constexpr size_t max_payload_size = 256;

    /**
     * @brief Size of the internal frame buffer used by built mode.
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
     * @brief Built mode: constructs a frame internally and sends it.
     *
     * Writes a PduHeader (in network byte order) followed by the caller's
     * payload into the internal fixed-size buffer, then attempts to send the
     * complete frame. If the socket cannot accept all bytes, the remainder
     * is tracked internally. The reactor must call continue_send() when the
     * socket next becomes writable.
     *
     * Must not be called while has_pending_data() is true.
     *
     * @param[in] pdu_id   DSL message ID.
     * @param[in] version  Message version.
     * @param[in] payload  Pointer to encoded payload bytes. Must not be nullptr.
     * @param[in] size     Payload size in bytes. Must be > 0 and <= max_payload_size.
     * @return { success, error_string }.
     */
    [[nodiscard]] std::tuple<bool, std::string> send(int16_t pdu_id, int8_t version,
                                                     const uint8_t* payload, uint32_t size);

    /**
     * @brief Pre-built mode: sends a caller-assembled frame with zero copy.
     *
     * The caller must have written a complete frame into a slab-allocated
     * buffer: a PduHeader (in network byte order) followed immediately by
     * the encoded payload. The framer stores only a pointer — no data is
     * copied. The slab chunk must remain valid until has_pending_data()
     * returns false, at which point the reactor deallocates it.
     *
     * Must not be called while has_pending_data() is true.
     *
     * @param[in] frame        Pointer to the start of the pre-built frame
     *                         (i.e. the PduHeader). Must not be nullptr.
     * @param[in] total_bytes  Total frame size: sizeof(PduHeader) + payload size.
     *                         Must be greater than sizeof(PduHeader).
     * @return { success, error_string }.
     */
    [[nodiscard]] std::tuple<bool, std::string> send_prebuilt(const uint8_t* frame,
                                                              uint32_t total_bytes);

    /**
     * @brief Continues sending any unsent data after a partial write.
     *
     * Must be called by the reactor when EPOLLOUT fires and has_pending_data()
     * returns true. Works identically regardless of which send mode was used.
     *
     * @return { success, error_string }.
     *         success is false only on a non-recoverable socket error.
     */
    [[nodiscard]] std::tuple<bool, std::string> continue_send();

    /**
     * @brief Returns true if there is unsent data waiting for EPOLLOUT.
     *
     * When true, the reactor must keep EPOLLOUT registered for this socket
     * and call continue_send() when it becomes writable. When false, the
     * slab chunk (if using send_prebuilt()) may be safely deallocated.
     */
    [[nodiscard]] bool has_pending_data() const;

private:
    ByteStreamInterface& stream_;

    // Internal buffer used by built mode (send()).
    uint8_t frame_buffer_[frame_buffer_size];

    // Points to the active frame being sent.
    // In built mode: points into frame_buffer_.
    // In pre-built mode: points into the caller's slab chunk.
    const uint8_t* active_frame_ptr_{nullptr};

    size_t frame_size_{0};    // total bytes in the active frame
    size_t send_offset_{0};   // bytes already sent
};

} // namespace pubsub_itc_fw
