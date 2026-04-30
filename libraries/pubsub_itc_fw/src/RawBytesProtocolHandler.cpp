// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include <pubsub_itc_fw/RawBytesProtocolHandler.hpp>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/TcpSocket.hpp>

namespace pubsub_itc_fw {

RawBytesProtocolHandler::RawBytesProtocolHandler(ConnectionID connection_id,
                                                 TcpSocket& socket,
                                                 ApplicationThread& target_thread,
                                                 int64_t buffer_capacity)
    : connection_id_(connection_id)
    , socket_(socket)
    , target_thread_(target_thread)
    , buffer_(buffer_capacity)
    , framer_(std::make_unique<PduFramer>(socket))
{
}

std::tuple<bool, std::string> RawBytesProtocolHandler::on_data_ready()
{
    const int64_t space = buffer_.space_remaining();
    if (space == 0) {
        return {false, "RawBytesProtocolHandler::on_data_ready: buffer full, application is not consuming fast enough"};
    }

    const ssize_t bytes_read = ::recv(socket_.get_file_descriptor(),
                                      buffer_.write_ptr(),
                                      static_cast<size_t>(space),
                                      MSG_DONTWAIT);

    if (bytes_read == 0) {
        return {false, ""};
    }

    if (bytes_read == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return {true, ""}; // Spurious wakeup, nothing to read.
        }
        return {false, fmt::format("RawBytesProtocolHandler::on_data_ready: recv failed: {}",
                                   StringUtils::get_errno_string())};
    }

    buffer_.advance_head(bytes_read);

    target_thread_.get_queue().enqueue(
        EventMessage::create_raw_socket_message(connection_id_,
                                                buffer_.read_ptr(),
                                                static_cast<int>(buffer_.bytes_available()),
                                                buffer_.tail()));
    return {true, ""};
}

void RawBytesProtocolHandler::commit_bytes(int64_t bytes)
{
    buffer_.advance_tail(bytes);
}

std::tuple<bool, std::string> RawBytesProtocolHandler::send_prebuilt(ExpandableSlabAllocator* allocator,
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
    if (!success) {
        release_pending_send();
        return {false, error};
    }

    if (!framer_->has_pending_data()) {
        release_pending_send();
    }
    return {true, ""};
}

bool RawBytesProtocolHandler::has_pending_send() const
{
    return framer_->has_pending_data();
}

std::tuple<bool, std::string> RawBytesProtocolHandler::continue_send()
{
    auto [success, error] = framer_->continue_send();
    if (!success) {
        release_pending_send();
        return {false, error};
    }

    if (!framer_->has_pending_data()) {
        release_pending_send();
    }
    return {true, ""};
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
