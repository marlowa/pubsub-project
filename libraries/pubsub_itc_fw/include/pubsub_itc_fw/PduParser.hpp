#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * PduParser is responsible for the receive side of the TCP framing layer.
 *
 * It reads bytes from a ByteStreamInterface and reassembles complete PDU frames
 * from the byte stream. Because TCP is a stream protocol, a single recv() call
 * may deliver a partial header, a partial payload, multiple frames, or any
 * combination.
 *
 * Zero-copy receive strategy:
 *   PduParser reads in two phases to avoid copying payload bytes:
 *
 *   Phase 1 -- header:
 *     Bytes are accumulated into a small fixed header_buffer_[sizeof(PduHeader)]
 *     until a complete 24-byte PduHeader has arrived. The canary is validated.
 *
 *   Phase 2 -- payload:
 *     Once the payload size is known from the header, PduParser allocates a slab
 *     chunk of exactly byte_count bytes from the ExpandableSlabAllocator. Payload
 *     bytes from the socket are read directly into that slab chunk -- no intermediate
 *     buffer, no copy.
 *
 *   On a complete frame:
 *     A FrameworkPdu EventMessage is dispatched to the target ApplicationThread.
 *     The EventMessage carries the slab pointer, the payload size, and the slab_id.
 *
 *   The ApplicationThread owns the slab chunk from the moment it dequeues the
 *   EventMessage. It must call:
 *
 *       reactor.inbound_slab_allocator().deallocate(msg.slab_id(), msg.payload())
 *
 *   after it has finished processing the payload. Failure to do so leaks the chunk.
 *   ApplicationThread provides a release_pdu_payload() helper to make this explicit.
 *
 * On peer disconnect (recv returns 0):
 *   The disconnect callback is invoked so the reactor can clean up.
 *
 * Threading model:
 *   All methods must be called from the reactor thread only.
 *
 * Ownership:
 *   PduParser does not own the ByteStreamInterface, ApplicationThread, or
 *   ExpandableSlabAllocator. The caller is responsible for their lifetimes.
 */

#include <cstdint>
#include <functional>
#include <string>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ByteStreamInterface.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PduHeader.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Zero-copy receive-side framing layer for PDU reassembly over TCP.
 *
 * Reads the PduHeader into a small fixed buffer, then allocates a slab chunk
 * and reads the payload directly into it. Dispatches complete PDU frames to
 * the target ApplicationThread as FrameworkPdu EventMessages carrying the
 * slab-allocated payload pointer and slab_id.
 *
 * The ApplicationThread is responsible for freeing each payload chunk after
 * processing by calling release_pdu_payload() or deallocating directly.
 *
 * Inline PDU handler:
 *   An optional InlinePduHandler may be installed via set_inline_handler().
 *   When set, it is called on the reactor thread for every complete PDU frame
 *   before the normal ITC dispatch path. If the handler returns true the PDU
 *   is consumed (slab freed, no EventMessage enqueued). If it returns false
 *   the PDU proceeds to the normal ITC dispatch path as usual.
 *
 *   The inline handler runs on the reactor thread and must not block, allocate
 *   from the heap, or perform I/O other than direct socket sends via the
 *   PduFramer supplied at installation time.
 */
class PduParser {
  public:
    ~PduParser() = default;

    /**
     * @brief Signature of an optional reactor-thread inline PDU handler.
     *
     * @param pdu_id  PDU identifier from the frame header.
     * @param seq_no  Sequence number from the frame header.
     * @param payload Pointer to the payload bytes (valid only during this call).
     * @param size    Payload size in bytes.
     * @return true if the PDU was consumed inline (no ITC dispatch); false to
     *         fall through to the normal ITC dispatch path.
     */
    using InlinePduHandler = std::function<bool(int16_t pdu_id, int64_t seq_no, const uint8_t* payload, size_t size)>;

    /**
     * @brief Installs an optional reactor-thread inline PDU handler.
     *
     * Replaces any previously installed handler. Pass a default-constructed
     * (empty) function to remove the handler.
     *
     * Must be called from the reactor thread only.
     */
    void set_inline_handler(InlinePduHandler handler);

    /**
     * @brief Constructs a PduParser.
     *
     * @param[in] stream             The underlying non-blocking byte stream. Must outlive this object.
     * @param[in] target_thread      The ApplicationThread to which complete PDUs are dispatched.
     * @param[in] slab_allocator     The slab allocator used to allocate payload chunks. Must outlive this object.
     * @param[in] logger             A reference to the logger, used when dumping PDU
     * @param[in] disconnect_handler Called when the peer closes the connection gracefully.
     * @param[in] connection_id      The ConnectionID of the connection this parser is serving.
     *                               Carried in every FrameworkPdu EventMessage so that
     *                               on_framework_pdu_message() can identify the source connection.
     */
    PduParser(ByteStreamInterface& stream, ApplicationThread& target_thread, ExpandableSlabAllocator& slab_allocator, QuillLogger& logger,
              std::function<void()> disconnect_handler, ConnectionID connection_id);

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
     *         slab allocation failure, read error, or peer disconnect).
     */
    [[nodiscard]] std::tuple<bool, std::string> receive();

  private:
    [[nodiscard]] bool has_complete_header() const;
    void dispatch_pdu(int slab_id, void* payload_chunk);
    void reset_header();

    ByteStreamInterface& stream_;
    ApplicationThread& target_thread_;
    ExpandableSlabAllocator& slab_allocator_;
    QuillLogger& logger_;
    std::function<void()> disconnect_handler_;
    ConnectionID connection_id_;

    // Fixed small buffer for the 24-byte frame header only.
    uint8_t header_buffer_[sizeof(PduHeader)];
    size_t header_bytes_{0};
    bool header_validated_{false};
    uint32_t current_payload_size_{0};
    int16_t current_pdu_id_{0};
    int64_t current_seq_no_{0};

    // Slab chunk allocated for the current in-progress payload.
    // Non-null only while a payload is being received.
    void* payload_chunk_{nullptr};
    int payload_slab_id_{-1};
    size_t payload_bytes_received_{0};

    InlinePduHandler inline_handler_;
};

} // namespaces
