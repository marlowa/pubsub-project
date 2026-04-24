// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <cstdint>
#include <tuple>

#include <sys/socket.h>
#include <unistd.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/RawBytesProtocolHandler.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>

namespace pubsub_itc_fw {

RawBytesProtocolHandler::RawBytesProtocolHandler(ConnectionID connection_id,
                                                 TcpSocket& socket,
                                                 ApplicationThread& target_thread,
                                                 int64_t buffer_capacity,
                                                 std::function<void()> disconnect_handler,
                                                 QuillLogger& logger)
    : connection_id_(connection_id)
    , socket_(socket)
    , target_thread_(target_thread)
    , disconnect_handler_(std::move(disconnect_handler))
    , logger_(logger)
    , buffer_(buffer_capacity)
    , framer_(std::make_unique<PduFramer>(socket))
{
}

void RawBytesProtocolHandler::on_data_ready()
{
    const int64_t space = buffer_.space_remaining();
    if (space == 0) {
        PUBSUB_LOG_STR(logger_, FwLogLevel::Error,
            "RawBytesProtocolHandler::on_data_ready: buffer full, application is not consuming fast enough");
        disconnect_handler_();
        return;
    }

    const ssize_t bytes_read = ::recv(socket_.get_file_descriptor(),
                                      buffer_.write_ptr(),
                                      static_cast<size_t>(space),
                                      MSG_DONTWAIT);

    if (bytes_read == 0) {
        PUBSUB_LOG_STR(logger_, FwLogLevel::Info,
            "RawBytesProtocolHandler::on_data_ready: peer closed connection");
        disconnect_handler_();
        return;
    }

    if (bytes_read == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; // spurious wakeup, nothing to read
        }
        PUBSUB_LOG(logger_, FwLogLevel::Error,
            "RawBytesProtocolHandler::on_data_ready: recv failed: {}",
            StringUtils::get_errno_string());
        disconnect_handler_();
        return;
    }

    buffer_.advance_head(bytes_read);

    target_thread_.get_queue().enqueue(
        EventMessage::create_raw_socket_message(connection_id_,
                                                buffer_.read_ptr(),
                                                static_cast<int>(buffer_.bytes_available()),
                                                buffer_.tail()));
}

void RawBytesProtocolHandler::commit_bytes(int64_t bytes)
{
    buffer_.advance_tail(bytes);
}

void RawBytesProtocolHandler::send_prebuilt(ExpandableSlabAllocator* allocator,
                                            int slab_id,
                                            void* chunk_ptr,
                                            uint32_t total_bytes)
{
    if (allocator == nullptr) {
        throw PreconditionAssertion(
            "RawBytesProtocolHandler::send_prebuilt: allocator must not be nullptr",
            __FILE__, __LINE__);
    }
    if (chunk_ptr == nullptr) {
        throw PreconditionAssertion(
            "RawBytesProtocolHandler::send_prebuilt: chunk_ptr must not be nullptr",
            __FILE__, __LINE__);
    }

    current_allocator_   = allocator;
    current_slab_id_     = slab_id;
    current_chunk_ptr_   = chunk_ptr;
    current_total_bytes_ = total_bytes;

    const uint8_t* frame = static_cast<const uint8_t*>(chunk_ptr);
    auto [success, error] = framer_->send_prebuilt(frame, total_bytes);
    if (!success && !error.empty()) {
        PUBSUB_LOG(logger_, FwLogLevel::Error,
            "RawBytesProtocolHandler::send_prebuilt failed: {}", error);
        release_pending_send();
        disconnect_handler_();
        return;
    }

    if (!framer_->has_pending_data()) {
        release_pending_send();
    }
}

bool RawBytesProtocolHandler::has_pending_send() const
{
    return framer_->has_pending_data();
}

void RawBytesProtocolHandler::continue_send()
{
    auto [success, error] = framer_->continue_send();
    if (!success && !error.empty()) {
        PUBSUB_LOG(logger_, FwLogLevel::Error,
            "RawBytesProtocolHandler::continue_send failed: {}", error);
        release_pending_send();
        disconnect_handler_();
        return;
    }

    if (!framer_->has_pending_data()) {
        release_pending_send();
    }
}

void RawBytesProtocolHandler::deallocate_pending_send()
{
    release_pending_send();
}

void RawBytesProtocolHandler::release_pending_send()
{
    if (current_allocator_ != nullptr) {
        current_allocator_->deallocate(current_slab_id_, current_chunk_ptr_);
        current_allocator_   = nullptr;
        current_slab_id_     = -1;
        current_chunk_ptr_   = nullptr;
        current_total_bytes_ = 0;
    }
}

} // namespace pubsub_itc_fw
