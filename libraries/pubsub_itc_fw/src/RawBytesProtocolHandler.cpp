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
#include <pubsub_itc_fw/utils/SimpleSpan.hpp>

namespace pubsub_itc_fw {

RawBytesProtocolHandler::RawBytesProtocolHandler(ConnectionID connection_id, TcpSocket& socket, ApplicationThread& target_thread, int64_t buffer_capacity)
    : connection_id_(connection_id), socket_(socket), target_thread_(target_thread), buffer_(std::make_shared<MirroredBuffer>(buffer_capacity)) {}

std::tuple<bool, std::string, bool> RawBytesProtocolHandler::on_data_ready() {
    const int64_t space = buffer_->space_remaining();
    if (space == 0) {
        // Defensive fallback. With the high-water pause logic in place this
        // path should not be reachable in normal operation; if it does fire,
        // something went wrong upstream and tearing down the connection is
        // safer than trying to recover.
        return {false, "RawBytesProtocolHandler::on_data_ready: buffer full, application is not consuming fast enough", false};
    }

    const ssize_t bytes_read = ::recv(socket_.get_file_descriptor(), buffer_->write_ptr(), static_cast<size_t>(space), MSG_DONTWAIT);

    if (bytes_read == 0) {
        return {false, "", false};
    }

    if (bytes_read == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return {true, "", false}; // Spurious wakeup, nothing to read.
        }
        return {false, fmt::format("RawBytesProtocolHandler::on_data_ready: recv failed: {}", StringUtils::get_errno_string()), false};
    }

    buffer_->advance_head(bytes_read);

    // Pass a shared_ptr copy of the buffer into the event so the buffer
    // outlives this handler if the connection is torn down while events for
    // it are still in the application thread's queue.
    target_thread_.get_queue().enqueue(
        EventMessage::create_raw_socket_message(connection_id_, buffer_->read_ptr(), static_cast<int>(buffer_->bytes_available()), buffer_->tail(), buffer_));

    // High-water check. Edge-triggered: only emit a pause signal on the
    // false->true transition so the manager doesn't re-issue epoll_ctl
    // unnecessarily on every read while already paused.
    bool emit_pause = false;
    if (!reads_paused_) {
        const int64_t fill = buffer_->bytes_available();
        const int64_t capacity = buffer_->capacity();
        if (fill * water_denominator >= capacity * high_water_numerator) {
            reads_paused_ = true;
            emit_pause = true;
        }
    }

    return {true, "", emit_pause};
}

bool RawBytesProtocolHandler::commit_bytes(int64_t bytes) {
    buffer_->advance_tail(bytes);

    // Low-water check. Edge-triggered: only emit a resume signal on the
    // true->false transition. If reads aren't currently paused there is
    // nothing to do here, and if the fill is still above the low-water mark
    // we wait for further commits before resuming.
    if (!reads_paused_) {
        return false;
    }

    const int64_t fill = buffer_->bytes_available();
    const int64_t capacity = buffer_->capacity();
    if (fill * water_denominator <= capacity * low_water_numerator) {
        reads_paused_ = false;
        return true;
    }
    return false;
}

std::tuple<bool, std::string> RawBytesProtocolHandler::send_prebuilt(ExpandableSlabAllocator* allocator, int slab_id, void* chunk_ptr, uint32_t total_bytes) {
    if (allocator == nullptr) {
        throw PreconditionAssertion("RawBytesProtocolHandler::send_prebuilt: allocator must not be nullptr", __FILE__, __LINE__);
    }
    if (chunk_ptr == nullptr) {
        throw PreconditionAssertion("RawBytesProtocolHandler::send_prebuilt: chunk_ptr must not be nullptr", __FILE__, __LINE__);
    }
    if (total_bytes == 0) {
        throw PreconditionAssertion("RawBytesProtocolHandler::send_prebuilt: total_bytes must be greater than zero", __FILE__, __LINE__);
    }
    if (active_frame_ptr_ != nullptr) {
        throw PreconditionAssertion("RawBytesProtocolHandler::send_prebuilt: previous send still in flight", __FILE__, __LINE__);
    }

    current_allocator_ = allocator;
    current_slab_id_ = slab_id;
    current_chunk_ptr_ = chunk_ptr;
    current_total_bytes_ = total_bytes;

    active_frame_ptr_ = static_cast<const uint8_t*>(chunk_ptr);
    frame_size_ = total_bytes;
    send_offset_ = 0;

    auto [success, error] = attempt_send_remaining();
    if (!success) {
        release_pending_send();
        return {false, error};
    }

    if (send_offset_ == frame_size_) {
        release_pending_send();
    }
    return {true, ""};
}

bool RawBytesProtocolHandler::has_pending_send() const {
    return active_frame_ptr_ != nullptr && send_offset_ < frame_size_;
}

std::tuple<bool, std::string> RawBytesProtocolHandler::continue_send() {
    if (active_frame_ptr_ == nullptr) {
        // EPOLLOUT can fire spuriously or arrive after the send has already
        // completed. Treat as a no-op.
        return {true, ""};
    }

    auto [success, error] = attempt_send_remaining();
    if (!success) {
        release_pending_send();
        return {false, error};
    }

    if (send_offset_ == frame_size_) {
        release_pending_send();
    }
    return {true, ""};
}

void RawBytesProtocolHandler::deallocate_pending_send() {
    release_pending_send();
}

void RawBytesProtocolHandler::release_pending_send() {
    active_frame_ptr_ = nullptr;
    frame_size_ = 0;
    send_offset_ = 0;

    if (current_allocator_ != nullptr) {
        current_allocator_->deallocate(current_slab_id_, current_chunk_ptr_);
        current_allocator_ = nullptr;
        current_slab_id_ = -1;
        current_chunk_ptr_ = nullptr;
        current_total_bytes_ = 0;
    }
}

std::tuple<bool, std::string> RawBytesProtocolHandler::attempt_send_remaining() {
    while (send_offset_ < frame_size_) {
        const uint32_t remaining = frame_size_ - send_offset_;
        utils::SimpleSpan<const uint8_t> data(active_frame_ptr_ + send_offset_, remaining);

        auto [result, error] = socket_.send(data);

        if (result == -EAGAIN) {
            // Socket send buffer full. Caller will register EPOLLOUT and the
            // reactor will invoke continue_send() when the socket is writable.
            return {true, ""};
        }
        if (result < 0) {
            return {false, fmt::format("RawBytesProtocolHandler::attempt_send_remaining: send failed: {}", error)};
        }

        send_offset_ += static_cast<uint32_t>(result);
    }
    return {true, ""};
}

} // namespace pubsub_itc_fw
