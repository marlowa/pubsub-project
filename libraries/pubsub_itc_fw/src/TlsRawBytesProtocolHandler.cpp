// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <string>
#include <tuple>

#include <fmt/format.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <pubsub_itc_fw/TlsRawBytesProtocolHandler.hpp>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/MirroredBuffer.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>
#include <pubsub_itc_fw/TlsContext.hpp>
#include <pubsub_itc_fw/TlsState.hpp>

namespace pubsub_itc_fw {

namespace {

std::string collect_openssl_errors() {
    std::string result;
    std::array<char, 256> buffer{};
    unsigned long code{0};
    while ((code = ERR_get_error()) != 0) {
        ERR_error_string_n(code, buffer.data(), buffer.size());
        if (!result.empty()) {
            result += "; ";
        }
        result += buffer.data();
    }
    return result.empty() ? "unknown OpenSSL error" : result;
}

} // un-named namespace

TlsRawBytesProtocolHandler::TlsRawBytesProtocolHandler(ConnectionID connection_id, TcpSocket& socket, ApplicationThread& target_thread, int64_t buffer_capacity,
                                                       TlsContext& tls_context, bool is_server)
    : connection_id_(connection_id), socket_(socket), target_thread_(target_thread), plaintext_buffer_(std::make_shared<MirroredBuffer>(buffer_capacity)) {
    tls_state_.rbio = BIO_new(BIO_s_mem());
    if (tls_state_.rbio == nullptr) {
        throw PreconditionAssertion("TlsRawBytesProtocolHandler: BIO_new for rbio failed", __FILE__, __LINE__);
    }

    tls_state_.wbio = BIO_new(BIO_s_mem());
    if (tls_state_.wbio == nullptr) {
        BIO_free(tls_state_.rbio);
        tls_state_.rbio = nullptr;
        throw PreconditionAssertion("TlsRawBytesProtocolHandler: BIO_new for wbio failed", __FILE__, __LINE__);
    }

    tls_state_.ssl = SSL_new(tls_context.get());
    if (tls_state_.ssl == nullptr) {
        BIO_free(tls_state_.rbio);
        BIO_free(tls_state_.wbio);
        tls_state_.rbio = nullptr;
        tls_state_.wbio = nullptr;
        throw PreconditionAssertion("TlsRawBytesProtocolHandler: SSL_new failed", __FILE__, __LINE__);
    }

    // SSL_set_bio transfers ownership of both BIOs to the SSL object.
    // SSL_free() will release them; they must not be freed independently after this call.
    SSL_set_bio(tls_state_.ssl, tls_state_.rbio, tls_state_.wbio);

    if (is_server) {
        SSL_set_accept_state(tls_state_.ssl);
    } else {
        SSL_set_connect_state(tls_state_.ssl);
    }
}

std::tuple<bool, std::string, bool> TlsRawBytesProtocolHandler::on_data_ready() {
    uint8_t encrypted_buffer[encrypted_read_buffer_size];
    const ssize_t bytes_read = ::recv(socket_.get_file_descriptor(), encrypted_buffer, sizeof(encrypted_buffer), MSG_DONTWAIT);

    if (bytes_read == 0) {
        return {false, "", false};
    }

    if (bytes_read == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return {true, "", false};
        }
        return {false, fmt::format("TlsRawBytesProtocolHandler::on_data_ready: recv failed: {}", StringUtils::get_errno_string()), false};
    }

    BIO_write(tls_state_.rbio, encrypted_buffer, static_cast<int>(bytes_read));

    if (tls_state_.handshake_phase == TlsState::HandshakePhase::Pending) {
        auto [ok, error] = drive_handshake();
        if (!ok) {
            return {false, error, false};
        }
        if (tls_state_.handshake_phase != TlsState::HandshakePhase::Complete) {
            return {true, "", false};
        }
        // Handshake just completed. Fall through in case plaintext arrived in
        // the same segment as the final handshake record.
    }

    return drain_plaintext();
}

std::tuple<bool, std::string> TlsRawBytesProtocolHandler::drive_handshake() {
    const int result = SSL_do_handshake(tls_state_.ssl);

    auto [flush_ok, flush_error] = flush_wbio();
    if (!flush_ok) {
        tls_state_.handshake_phase = TlsState::HandshakePhase::Failed;
        return {false, flush_error};
    }

    if (result == 1) {
        tls_state_.handshake_phase = TlsState::HandshakePhase::Complete;
        return {true, ""};
    }

    const int ssl_error = SSL_get_error(tls_state_.ssl, result);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
        return {true, ""};
    }

    tls_state_.handshake_phase = TlsState::HandshakePhase::Failed;
    return {false, "TlsRawBytesProtocolHandler: TLS handshake failed: " + collect_openssl_errors()};
}

std::tuple<bool, std::string, bool> TlsRawBytesProtocolHandler::drain_plaintext() {
    bool any_data_decrypted = false;

    while (true) {
        const int64_t space = plaintext_buffer_->space_remaining();
        if (space == 0) {
            return {false, "TlsRawBytesProtocolHandler::drain_plaintext: plaintext buffer full, application is not consuming fast enough", false};
        }

        const int bytes_decrypted = SSL_read(tls_state_.ssl, plaintext_buffer_->write_ptr(), static_cast<int>(space));

        if (bytes_decrypted > 0) {
            plaintext_buffer_->advance_head(bytes_decrypted);
            any_data_decrypted = true;
            continue;
        }

        const int ssl_error = SSL_get_error(tls_state_.ssl, bytes_decrypted);
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            // Peer sent TLS close_notify: treat as graceful disconnect.
            return {false, "", false};
        }
        if (ssl_error == SSL_ERROR_WANT_READ) {
            break;
        }
        return {false, "TlsRawBytesProtocolHandler: SSL_read failed: " + collect_openssl_errors(), false};
    }

    if (!any_data_decrypted) {
        return {true, "", false};
    }

    target_thread_.enqueue(EventMessage::create_raw_socket_message(
        connection_id_, plaintext_buffer_->read_ptr(), static_cast<int>(plaintext_buffer_->bytes_available()), plaintext_buffer_->tail(), plaintext_buffer_));

    bool emit_pause = false;
    if (!reads_paused_) {
        const int64_t fill = plaintext_buffer_->bytes_available();
        const int64_t capacity = plaintext_buffer_->capacity();
        if (fill * water_denominator >= capacity * high_water_numerator) {
            reads_paused_ = true;
            emit_pause = true;
        }
    }
    return {true, "", emit_pause};
}

std::tuple<bool, std::string> TlsRawBytesProtocolHandler::flush_wbio() {
    // Drain all bytes from the write BIO into the outbound buffer.
    uint8_t temp_buffer[encrypted_read_buffer_size];
    int bytes_drained;
    while ((bytes_drained = BIO_read(tls_state_.wbio, temp_buffer, sizeof(temp_buffer))) > 0) {
        tls_state_.pending_outbound.insert(tls_state_.pending_outbound.end(), temp_buffer, temp_buffer + bytes_drained);
    }

    // Attempt a non-blocking send of all pending bytes.
    while (tls_state_.has_pending_outbound()) {
        const uint8_t* data = tls_state_.pending_outbound.data() + tls_state_.pending_outbound_offset;
        const size_t size = tls_state_.pending_outbound.size() - tls_state_.pending_outbound_offset;
        const ssize_t sent = ::send(socket_.get_file_descriptor(), data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return {true, ""};
            }
            tls_state_.clear_pending_outbound();
            return {false, fmt::format("TlsRawBytesProtocolHandler: flush_wbio send failed: {}", StringUtils::get_errno_string())};
        }
        tls_state_.pending_outbound_offset += static_cast<size_t>(sent);
    }
    tls_state_.clear_pending_outbound();
    return {true, ""};
}

std::tuple<bool, std::string> TlsRawBytesProtocolHandler::send_prebuilt(ExpandableSlabAllocator* allocator, int slab_id, void* chunk_ptr,
                                                                        uint32_t total_bytes) {
    if (allocator == nullptr) {
        throw PreconditionAssertion("TlsRawBytesProtocolHandler::send_prebuilt: allocator must not be nullptr", __FILE__, __LINE__);
    }
    if (chunk_ptr == nullptr) {
        throw PreconditionAssertion("TlsRawBytesProtocolHandler::send_prebuilt: chunk_ptr must not be nullptr", __FILE__, __LINE__);
    }
    if (total_bytes == 0) {
        throw PreconditionAssertion("TlsRawBytesProtocolHandler::send_prebuilt: total_bytes must be greater than zero", __FILE__, __LINE__);
    }
    if (tls_state_.handshake_phase != TlsState::HandshakePhase::Complete) {
        allocator->deallocate(slab_id, chunk_ptr);
        return {false, "TlsRawBytesProtocolHandler::send_prebuilt: TLS handshake not yet complete"};
    }

    const int result = SSL_write(tls_state_.ssl, chunk_ptr, static_cast<int>(total_bytes));

    // The plaintext has been consumed by SSL_write regardless of outcome.
    // OpenSSL copies it into its internal record buffer, so we release the
    // slab chunk now and track only the ciphertext in pending_outbound.
    allocator->deallocate(slab_id, chunk_ptr);

    if (result <= 0) {
        return {false, "TlsRawBytesProtocolHandler::send_prebuilt: SSL_write failed: " + collect_openssl_errors()};
    }

    return flush_wbio();
}

bool TlsRawBytesProtocolHandler::has_pending_send() const {
    return tls_state_.has_pending_outbound();
}

std::tuple<bool, std::string> TlsRawBytesProtocolHandler::continue_send() {
    while (tls_state_.has_pending_outbound()) {
        const uint8_t* data = tls_state_.pending_outbound.data() + tls_state_.pending_outbound_offset;
        const size_t size = tls_state_.pending_outbound.size() - tls_state_.pending_outbound_offset;
        const ssize_t sent = ::send(socket_.get_file_descriptor(), data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return {true, ""};
            }
            tls_state_.clear_pending_outbound();
            return {false, fmt::format("TlsRawBytesProtocolHandler::continue_send: send failed: {}", StringUtils::get_errno_string())};
        }
        tls_state_.pending_outbound_offset += static_cast<size_t>(sent);
    }
    tls_state_.clear_pending_outbound();
    return {true, ""};
}

void TlsRawBytesProtocolHandler::deallocate_pending_send() {
    // The slab chunk is freed in send_prebuilt() immediately after SSL_write(),
    // so there is no slab to release here. Clear pending ciphertext only.
    tls_state_.clear_pending_outbound();
}

std::tuple<bool, std::string> TlsRawBytesProtocolHandler::start_outbound_handshake() {
    return drive_handshake();
}

bool TlsRawBytesProtocolHandler::commit_bytes(int64_t bytes) {
    plaintext_buffer_->advance_tail(bytes);

    if (!reads_paused_) {
        return false;
    }

    const int64_t fill = plaintext_buffer_->bytes_available();
    const int64_t capacity = plaintext_buffer_->capacity();
    if (fill * water_denominator <= capacity * low_water_numerator) {
        reads_paused_ = false;
        return true;
    }
    return false;
}

} // namespaces
