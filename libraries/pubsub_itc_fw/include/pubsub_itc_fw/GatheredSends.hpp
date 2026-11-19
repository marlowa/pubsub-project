#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>

#include <pubsub_itc_fw/ByteStreamInterface.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/WaitingSendQueue.hpp>

namespace pubsub_itc_fw {

/**
 * @brief The sends asked for on one connection during the reactor's present pass of control commands, written together
 *        at the end of the pass with one system call (docs/framework/gathering_sends.md, section 3.1).
 *
 * Room for ByteStreamInterface::max_gathered_parts sends, held in the connection itself, so that gathering never
 * allocates. A send is gathered only while the connection has nothing in progress and nothing waiting, so the gathered
 * sends always come before anything later asked for on the connection, and are written before it.
 *
 * Each send holds its slab chunk until it is written, or until the connection is closed (release_all()).
 *
 * One thread, the reactor's, uses it.
 */
class GatheredSends {
  public:
    static constexpr size_t capacity = ByteStreamInterface::max_gathered_parts;

    /// Adds @p send at the back. There must be room: full() is false.
    void add(const WaitingSend& send) {
        if (full()) {
            throw PreconditionAssertion("GatheredSends::add: no room; the gathered sends must be written first", __FILE__, __LINE__);
        }
        sends_[count_] = send;
        ++count_;
    }

    [[nodiscard]] const WaitingSend* data() const {
        return sends_.data();
    }

    [[nodiscard]] const WaitingSend& at(size_t position) const {
        if (position >= count_) {
            throw PreconditionAssertion("GatheredSends::at: no send at that position", __FILE__, __LINE__);
        }
        return sends_[position];
    }

    [[nodiscard]] size_t size() const {
        return count_;
    }

    [[nodiscard]] bool empty() const {
        return count_ == 0;
    }

    [[nodiscard]] bool full() const {
        return count_ == capacity;
    }

    /// Forgets every send, without touching its chunk, which the caller has dealt with.
    void clear() {
        count_ = 0;
    }

    /// Returns every send's chunk to its allocator, and empties the array: for a connection being closed.
    void release_all() {
        for (size_t index = 0; index < count_; ++index) {
            sends_[index].allocator->deallocate(sends_[index].slab_id, sends_[index].chunk);
        }
        count_ = 0;
    }

    /// True the first time it is called, false after: the connection's stream being short of memory (ENOBUFS or ENOMEM)
    /// is said once, with a Warning, not at every call.
    [[nodiscard]] bool first_shortage_of_memory() {
        const bool first = !short_of_memory_reported_;
        short_of_memory_reported_ = true;
        return first;
    }

  private:
    std::array<WaitingSend, capacity> sends_{};
    size_t count_{0};
    bool short_of_memory_reported_{false};
};

} // namespaces
