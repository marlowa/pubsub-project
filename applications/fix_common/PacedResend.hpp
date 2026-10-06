#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "UnansweredCommandStore.hpp"

namespace fix_common {

/**
 * @brief A copy of the commands a gateway holds unanswered, taken at one moment, to be sent again in
 *        batches.
 *
 * A gateway sends its unanswered commands again after a change of sequencer leader, and on a
 * connection to a sequencer that has been closed and opened again (docs/bug_list.md, BUG-0118). There
 * can be hundreds of thousands of them. Sent all at once, they would queue on the connection faster
 * than the sequencer reads them, and a connection whose queue of waiting sends reaches its limit is
 * closed (BUG-0112): the gateway would connect again, send them all again, and be closed again.
 *
 * So they are copied here when the resend starts, and sent a batch at a time on each connection that
 * needs them, each batch after the connection has reported that everything before it is written. The
 * store itself cannot be read a batch at a time, because commands are answered, and their space reused,
 * while the resend goes on. A command answered after the copy is taken is still sent again; the leader
 * finds it in its log and does not sequence it twice.
 *
 * The copy is held in one buffer, which keeps its memory for the next resend rather than allocating it
 * again.
 */
class UnansweredSnapshot {
  public:
    /// Replaces the copy with every command @p store holds unanswered now, oldest first.
    void take(const UnansweredCommandStore& store) {
        bytes_.clear();
        spans_.clear();
        store.for_each_unanswered([this](const uint8_t* bytes, size_t size) {
            spans_.emplace_back(bytes_.size(), size);
            bytes_.insert(bytes_.end(), bytes, bytes + size);
        });
    }

    [[nodiscard]] size_t count() const {
        return spans_.size();
    }

    /// The encoded envelope of command @p index, oldest first.
    [[nodiscard]] std::pair<const uint8_t*, size_t> command(size_t index) const {
        return {bytes_.data() + spans_[index].first, spans_[index].second};
    }

  private:
    std::vector<uint8_t> bytes_;
    std::vector<std::pair<size_t, size_t>> spans_;
};

/**
 * @brief How far a resend of an UnansweredSnapshot has got on one connection.
 */
class PacedResend {
  public:
    /// The most commands sent in one batch, before waiting to hear that the connection has written them.
    static constexpr size_t batch_size = 256;

    void start(size_t total) {
        total_ = total;
        next_ = 0;
        active_ = total > 0;
    }

    void abandon() {
        active_ = false;
    }

    [[nodiscard]] bool active() const {
        return active_;
    }

    [[nodiscard]] size_t sent() const {
        return next_;
    }

    [[nodiscard]] size_t total() const {
        return total_;
    }

    /**
     * @brief Hands the next batch of commands to @p send, as send(index), and reports whether any
     * remain. When none remain the resend is no longer active.
     */
    template <typename Send> bool send_next_batch(Send&& send) {
        if (!active_) {
            return false;
        }
        const size_t end = next_ + batch_size < total_ ? next_ + batch_size : total_;
        for (; next_ < end; ++next_) {
            send(next_);
        }
        active_ = next_ < total_;
        return active_;
    }

  private:
    size_t total_{0};
    size_t next_{0};
    bool active_{false};
};

} // namespaces
