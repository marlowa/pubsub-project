#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/FixedCapacityRingBuffer.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/SlabHandle.hpp>

namespace pubsub_itc_fw {

/**
 * @brief One send that could not be started yet: the bytes to write, and the slab chunk that holds them.
 *
 * The chunk belongs to the allocator of the application thread that asked for the send. Whoever takes
 * the send out of the queue either hands the chunk to the connection's protocol handler, which returns
 * it once the bytes are written, or returns it to the allocator itself.
 */
struct WaitingSend {
    ExpandableSlabAllocator* allocator{nullptr};
    SlabHandle slab_id{invalid_slab_handle};
    void* chunk{nullptr};
    /// The number of bytes to write, starting at chunk: for a framed message, its header and payload.
    uint32_t byte_count{0};
};

/**
 * @brief What became of a send the reactor was asked for.
 */
enum class SendDisposition {
    /// No connection of that manager has the id. The chunk has not been touched.
    NoSuchConnection,
    /// The bytes were handed to the connection, or the attempt failed and the connection was closed.
    /// Either way the chunk is no longer the caller's.
    Started,
    /// The connection was still writing an earlier send, so this one joined the connection's queue,
    /// or the queue was full and the connection was closed. Either way the chunk is no longer the
    /// caller's.
    Waiting
};

/**
 * @brief The sends waiting for one connection, oldest first.
 *
 * A connection writes one send at a time. When its socket cannot take all of a send at once, the
 * protocol handler keeps the rest and writes it when the socket has room again. Any further send asked
 * for on that connection meanwhile waits here, in the order it was asked for, and is started once the
 * sends before it are written. Each connection has its own queue, so a peer that reads slowly holds up
 * only the sends to itself, and the reactor goes on serving every other connection.
 *
 * **Limits.** The queue holds at most a given number of sends and a given number of bytes, both passed
 * to add(). The connection manager closes a connection whose queue is full: a peer that has stopped
 * reading would otherwise hold slab memory without limit. The limits are passed on each call, rather
 * than when the queue is created, because they are the reactor's configuration and a connection does
 * not otherwise need to know it.
 *
 * **Memory.** Nothing is allocated until a send first has to wait. The storage then starts with room for
 * 64 sends and doubles each time it fills, up to the limit, and is released as soon as the queue is
 * empty again. A process with thousands of connections that keep up therefore holds no storage for
 * them, and a connection that fell behind once does not keep its largest storage for the rest of its
 * life.
 *
 * **The chunks.** A send in the queue still holds its slab chunk. release_all() returns every chunk
 * to its allocator, and the connection manager calls it when it closes a connection. The destructor
 * does not: when the reactor shuts down, its connections can be destroyed after the application
 * threads whose allocators own the chunks, and returning a chunk to an allocator that no longer exists
 * would be worse than the memory not being returned at exit.
 *
 * **Threading.** Not thread-safe. Used only by the reactor thread.
 */
class WaitingSendQueue {
  public:
    ~WaitingSendQueue() = default;
    WaitingSendQueue() = default;

    WaitingSendQueue(const WaitingSendQueue& other) = delete;
    WaitingSendQueue& operator=(const WaitingSendQueue& other) = delete;
    WaitingSendQueue(WaitingSendQueue&& other) = delete;
    WaitingSendQueue& operator=(WaitingSendQueue&& other) = delete;

    /**
     * @brief Adds a send at the back, unless that would take the queue past either limit.
     *
     * An empty queue always accepts one send, whatever its size, so that a single send larger than the
     * byte limit is still written rather than refused for ever.
     *
     * @param[in] send          The send to add.
     * @param[in] maximum_sends The most sends the queue may hold. Must be greater than zero.
     * @param[in] maximum_bytes The most bytes the sends in the queue may add up to.
     * @return True if the send was added. False if the queue is full, in which case nothing changed
     *         and the send's chunk is still the caller's.
     */
    [[nodiscard]] bool add(const WaitingSend& send, size_t maximum_sends, size_t maximum_bytes) {
        if (maximum_sends == 0) {
            throw PreconditionAssertion("WaitingSendQueue::add: the most sends allowed must be greater than zero", __FILE__, __LINE__);
        }
        if (!empty() && (size() >= maximum_sends || bytes_ + send.byte_count > maximum_bytes)) {
            return false;
        }
        if (!ring_.has_value()) {
            ring_.emplace(std::min(initial_capacity, maximum_sends));
        } else if (ring_->full()) {
            grow(maximum_sends);
        }
        // Cannot be refused: the ring was just made, or had room, or was given more.
        static_cast<void>(ring_->push_back(send));
        bytes_ += send.byte_count;
        return true;
    }

    /// The oldest send, which is the next to be started. The queue must not be empty.
    [[nodiscard]] const WaitingSend& front() const {
        if (empty()) {
            throw PreconditionAssertion("WaitingSendQueue::front: the queue is empty", __FILE__, __LINE__);
        }
        return ring_->front();
    }

    /**
     * @brief Removes the oldest send, without touching its chunk, which passes to the caller.
     *
     * The queue must not be empty. When it becomes empty its storage is released.
     */
    void pop_front() {
        if (empty()) {
            throw PreconditionAssertion("WaitingSendQueue::pop_front: the queue is empty", __FILE__, __LINE__);
        }
        bytes_ -= ring_->front().byte_count;
        ring_->pop_front();
        if (ring_->empty()) {
            ring_.reset();
        }
    }

    /// Returns the chunk of every send still waiting to its allocator, and empties the queue.
    void release_all() {
        while (!empty()) {
            const WaitingSend send = ring_->front();
            pop_front();
            send.allocator->deallocate(send.slab_id, send.chunk);
        }
    }

    [[nodiscard]] bool empty() const {
        return !ring_.has_value() || ring_->empty();
    }

    /// The number of sends waiting.
    [[nodiscard]] size_t size() const {
        return ring_.has_value() ? ring_->size() : 0;
    }

    /// The number of bytes the waiting sends add up to.
    [[nodiscard]] size_t bytes() const {
        return bytes_;
    }

    /// How many sends the current storage has room for; zero when none is held.
    [[nodiscard]] size_t capacity() const {
        return ring_.has_value() ? ring_->capacity() : 0;
    }

  private:
    static constexpr size_t initial_capacity = 64;

    /// Moves the sends, in order, into storage twice the size, or as large as the limit allows.
    void grow(size_t maximum_sends) {
        FixedCapacityRingBuffer<WaitingSend> larger(std::min(ring_->capacity() * 2, std::max(maximum_sends, ring_->capacity())));
        while (!ring_->empty()) {
            static_cast<void>(larger.push_back(ring_->front()));
            ring_->pop_front();
        }
        ring_.emplace(std::move(larger));
    }

    std::optional<FixedCapacityRingBuffer<WaitingSend>> ring_;
    size_t bytes_{0};
};

} // namespaces
