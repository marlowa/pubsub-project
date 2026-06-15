#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>
#include <string>
#include <tuple>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/MirroredBuffer.hpp>
#include <pubsub_itc_fw/ProtocolHandlerInterface.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/TlsContext.hpp>
#include <pubsub_itc_fw/TlsState.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Concrete protocol handler for TLS-protected raw byte streams.
 *
 * This is the TLS counterpart of RawBytesProtocolHandler. It accepts an
 * already-connected TCP socket and performs the TLS handshake non-blockingly
 * before delivering plaintext bytes to the target ApplicationThread as
 * RawSocketCommunication events.
 *
 * OpenSSL memory BIOs are used so that all socket I/O is performed by the
 * reactor's epoll dispatch and the reactor thread never blocks. Ciphertext is
 * read from or written to the socket with MSG_DONTWAIT. Any bytes that cannot
 * be sent immediately are held in TlsState::pending_outbound and retried when
 * epoll delivers EPOLLOUT.
 *
 * Inbound path:
 *   on_data_ready() reads ciphertext from the socket, writes it to the read
 *   BIO, and drives SSL_do_handshake() during the handshake phase. Once
 *   established it loops SSL_read() to extract plaintext, places it in a
 *   MirroredBuffer, and delivers a RawSocketCommunication event. Backpressure
 *   is applied on the plaintext buffer via the same high-water / low-water
 *   watermark scheme as RawBytesProtocolHandler.
 *
 * Outbound path:
 *   send_prebuilt() encrypts plaintext via SSL_write() and releases the slab
 *   chunk immediately -- OpenSSL copies the plaintext internally. The resulting
 *   ciphertext in the write BIO is flushed to the socket. Unsent ciphertext is
 *   tracked in TlsState::pending_outbound and sent by continue_send() when
 *   EPOLLOUT fires.
 *
 * Threading model:
 *   All methods must be called exclusively from the reactor thread.
 */
class TlsRawBytesProtocolHandler : public ProtocolHandlerInterface {
public:
    ~TlsRawBytesProtocolHandler() override = default;

    TlsRawBytesProtocolHandler(const TlsRawBytesProtocolHandler&) = delete;
    TlsRawBytesProtocolHandler& operator=(const TlsRawBytesProtocolHandler&) = delete;

    /**
     * @brief Constructs a TlsRawBytesProtocolHandler for an already-connected socket.
     *
     * @param[in] connection_id   The ConnectionID assigned to this connection.
     * @param[in] socket          The connected TCP socket. Must outlive this object.
     * @param[in] target_thread   The ApplicationThread to receive events. Must outlive
     *                            this object.
     * @param[in] buffer_capacity Minimum plaintext MirroredBuffer capacity in bytes.
     * @param[in] tls_context     The shared TlsContext from which the SSL object is created.
     *                            Must outlive this object.
     * @param[in] is_server       True for server-side (SSL_accept path), false for
     *                            client-side (SSL_connect path).
     */
    TlsRawBytesProtocolHandler(ConnectionID connection_id, TcpSocket& socket, ApplicationThread& target_thread,
                               int64_t buffer_capacity, TlsContext& tls_context, bool is_server,
                               QuillLogger& logger);

    /**
     * @brief Services a readable socket event (EPOLLIN).
     *
     * During the handshake phase, reads ciphertext and drives SSL_do_handshake().
     * Once established, decrypts available records and delivers plaintext as a
     * RawSocketCommunication event.
     *
     * @return {true, "", pause} on success where pause signals read backpressure;
     *         {false, "", false} on graceful peer close or TLS close_notify;
     *         {false, error_description, false} on hard failure.
     */
    [[nodiscard]] std::tuple<bool, std::string, bool> on_data_ready() override;

    /**
     * @brief Encrypts and initiates sending of a plaintext slab chunk.
     *
     * The slab chunk is encrypted via SSL_write() and released immediately,
     * regardless of whether the resulting ciphertext could be fully sent.
     *
     * @param[in] allocator   Slab allocator owning chunk_ptr. Must not be nullptr.
     * @param[in] slab_id     Slab ID for deallocation.
     * @param[in] chunk_ptr   Plaintext data. Must not be nullptr.
     * @param[in] total_bytes Plaintext byte count. Must be greater than zero.
     *
     * @return {true, ""} on success or partial send; {false, error_description} on failure.
     */
    [[nodiscard]] std::tuple<bool, std::string> send_prebuilt(ExpandableSlabAllocator* allocator, int slab_id, void* chunk_ptr, uint32_t total_bytes) override;

    /**
     * @brief Returns true if unsent ciphertext bytes are waiting in pending_outbound.
     */
    [[nodiscard]] bool has_pending_send() const override;

    /**
     * @brief Continues sending pending_outbound ciphertext bytes on EPOLLOUT.
     *
     * @return {true, ""} on progress or completion; {false, error_description} on failure.
     */
    [[nodiscard]] std::tuple<bool, std::string> continue_send() override;

    /**
     * @brief Discards pending_outbound on connection teardown.
     *
     * No slab chunk deallocation is needed: the slab is freed inside send_prebuilt()
     * immediately after SSL_write().
     */
    void deallocate_pending_send() override;

    /**
     * @brief Advances the plaintext MirroredBuffer tail by the consumed byte count.
     *
     * @param[in] bytes Number of bytes the application has consumed.
     * @return true if EPOLLIN should be re-registered (low-water mark crossed).
     */
    bool commit_bytes(int64_t bytes) override;

    /**
     * @brief Returns true if reads are currently paused for backpressure.
     */
    [[nodiscard]] bool is_reads_paused() const override {
        return reads_paused_;
    }

    /**
     * @brief Returns true once the TLS handshake has completed.
     */
    [[nodiscard]] bool is_handshake_complete() const override {
        return tls_state_.handshake_phase == TlsState::HandshakePhase::Complete;
    }

    /**
     * @brief Drives the first client-side handshake step, generating and sending
     *        the ClientHello record.
     *
     * Must be called once immediately after the TCP connection is established,
     * before any data arrives from the server. Subsequent steps are driven by
     * on_data_ready() as server responses arrive.
     *
     * @return {true, ""} on success (including partial send held in pending_outbound);
     *         {false, error} on hard failure.
     */
    [[nodiscard]] std::tuple<bool, std::string> start_outbound_handshake() override;

private:
    /**
     * @brief Drains the write BIO and attempts a non-blocking send to the socket.
     *
     * Any bytes that cannot be sent immediately are appended to
     * TlsState::pending_outbound for retry via continue_send(). Called after
     * every SSL operation that may produce output records (handshake steps,
     * SSL_write).
     *
     * @return {true, ""} unless a hard send error occurred.
     */
    [[nodiscard]] std::tuple<bool, std::string> flush_wbio();

    /**
     * @brief Drives the TLS handshake state machine.
     *
     * Calls SSL_do_handshake() once and flushes the write BIO. Updates
     * handshake_phase to Complete or Failed as appropriate.
     *
     * @return {true, ""} while in progress or on completion; {false, error} on failure.
     */
    [[nodiscard]] std::tuple<bool, std::string> drive_handshake();

    /**
     * @brief Loops SSL_read() into the plaintext buffer and delivers one event.
     *
     * Called once the handshake is complete. Loops until SSL_ERROR_WANT_READ.
     *
     * @return {true, "", pause} on success; {false, "", false} on close_notify;
     *         {false, error, false} on hard failure.
     */
    [[nodiscard]] std::tuple<bool, std::string, bool> drain_plaintext();

    // Maximum bytes to read from the socket in one recv() call.
    // A TLS record carries at most 16 384 bytes of payload plus a 5-byte header.
    static constexpr int encrypted_read_buffer_size = 16384 + 5;

    static constexpr int64_t water_denominator = 4;
    static constexpr int64_t high_water_numerator = 3; // 75%
    static constexpr int64_t low_water_numerator = 2;  // 50%

    ConnectionID connection_id_;
    TcpSocket& socket_;
    ApplicationThread& target_thread_;
    QuillLogger& logger_;

    TlsState tls_state_;
    std::shared_ptr<MirroredBuffer> plaintext_buffer_;

    bool reads_paused_{false};
};

} // namespaces
