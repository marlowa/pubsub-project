#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstdint>
#include <functional>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

/**
 * @brief How many messages from one connection are waiting in its application thread's queue, and whether the reactor
 *        has stopped reading the connection because too many are (docs/framework/bounding_messages_waiting.md).
 *
 * The reactor reads a connection carrying PDUs into the owning thread's queue as fast as the network delivers, so a
 * thread that falls behind its peer would otherwise hold the whole backlog in memory. With a limit, the reactor stops
 * reading the connection once that many of its messages are waiting, so that the backlog stays in the peer's socket and
 * TCP slows the peer; and the thread has the reactor read it again once it has taken the waiting messages down to half
 * the limit. Half, so that reading is not stopped and started for every message when the two run at the same speed.
 *
 * Two threads use one of these, without a lock. The reactor thread calls message_queued() for each message it puts on
 * the thread's queue, and the application thread calls message_taken() for each it takes off. Each changes the count
 * with one atomic operation.
 *
 * **Two races this closes**, one on each side, each by looking at the count a second time.
 *
 * - The reactor reaches the limit and then records that it has stopped. Between the two, the thread may take every
 *   waiting message, see no stop recorded, and so never ask for reading to start again, leaving the connection stopped
 *   for good with nothing waiting. So after recording the stop, the reactor looks at the count again, and if the thread
 *   has already taken it down to half the limit, takes the stop back itself.
 * - The thread takes a message and finds the count at or below half; before it looks at the stop, the reactor queues
 *   more, reaches the limit and records a stop. The count the thread found is then out of date, and acting on it would
 *   start reading again with the limit's worth still waiting. So when the thread finds a stop, it looks at the count
 *   again and acts only if that is at or below half. If it is not, messages are waiting, and the thread will look again
 *   when it takes the next.
 *
 * Whichever of the two clears the stop, by a compare-and-exchange, is the one that starts reading again, so it is
 * started exactly once. Both races were found by a test running the two threads as fast as they can.
 */
class MessagesWaitingLimit {
  public:
    /// The smallest limit allowed, so that reading is never stopped and started for almost every message.
    static constexpr int64_t smallest_limit = 16;

    /// @param[in] limit The most messages that may wait; at least smallest_limit.
    ~MessagesWaitingLimit() = default;
    MessagesWaitingLimit(const MessagesWaitingLimit&) = delete;
    MessagesWaitingLimit& operator=(const MessagesWaitingLimit&) = delete;
    MessagesWaitingLimit(MessagesWaitingLimit&&) = delete;
    MessagesWaitingLimit& operator=(MessagesWaitingLimit&&) = delete;

    explicit MessagesWaitingLimit(int64_t limit) : limit_(limit) {
        if (limit < smallest_limit) {
            throw PreconditionAssertion("MessagesWaitingLimit: the limit must be at least 16", __FILE__, __LINE__);
        }
    }

    /**
     * @brief The reactor is about to put one more message on the thread's queue. Called on the reactor thread only, and
     *        before the message is put on the queue: called after, the thread could take the message first and leave the
     *        count one short, which would let one more than the limit wait.
     * @return Whether the reactor must stop reading the connection now. When it returns true, the application thread
     *         will ask for reading to start again once it has taken the waiting messages down to half the limit.
     */
    [[nodiscard]] bool message_queued() {
        const int64_t waiting = waiting_.fetch_add(1, std::memory_order_seq_cst) + 1;
        if (waiting < limit_) {
            return false;
        }
        if (between_reaching_the_limit_and_recording_the_stop_) {
            between_reaching_the_limit_and_recording_the_stop_();
        }
        stopped_.store(true, std::memory_order_seq_cst);
        if (waiting_.load(std::memory_order_seq_cst) <= limit_ / 2) {
            // The thread took the messages down before the stop was recorded, so it will not ask for reading again.
            bool expected = true;
            if (stopped_.compare_exchange_strong(expected, false, std::memory_order_seq_cst)) {
                return false;
            }
            // The thread cleared it first, and is asking for reading to start again.
        }
        return true;
    }

    /**
     * @brief The application thread has taken one message off its queue. Called on that thread only.
     * @return Whether the thread must now ask the reactor to read the connection again. True at most once for each time
     *         message_queued() returned true.
     */
    [[nodiscard]] bool message_taken() {
        // The reactor only ever adds, so the count after this subtraction is never more than the count at any later moment:
        // a count above half here is above half now.
        const int64_t waiting = waiting_.fetch_sub(1, std::memory_order_seq_cst) - 1;
        if (waiting > limit_ / 2) {
            return false;
        }
        if (between_taking_and_looking_at_the_stop_) {
            between_taking_and_looking_at_the_stop_();
        }
        if (!stopped_.load(std::memory_order_seq_cst) || waiting_.load(std::memory_order_seq_cst) > limit_ / 2) {
            return false;
        }
        bool expected = true;
        return stopped_.compare_exchange_strong(expected, false, std::memory_order_seq_cst);
    }

    /// Whether the reactor has stopped reading the connection for this limit.
    [[nodiscard]] bool stopped() const {
        return stopped_.load(std::memory_order_seq_cst);
    }

    /// How many messages are waiting now.
    [[nodiscard]] int64_t waiting() const {
        return waiting_.load(std::memory_order_seq_cst);
    }

    /// The most messages that may wait.
    [[nodiscard]] int64_t limit() const {
        return limit_;
    }

    /// For a test only: called on the reactor thread when the limit is reached, before the stop is recorded, the moment
    /// the first race above happens in, so that a test can make the race happen on every run rather than by chance.
    void set_between_reaching_the_limit_and_recording_the_stop(std::function<void()> action) {
        between_reaching_the_limit_and_recording_the_stop_ = std::move(action);
    }

    /// For a test only: called on the application thread after it has taken a message and found the count at or below
    /// half, before it looks at the stop, the moment the second race above happens in.
    void set_between_taking_and_looking_at_the_stop(std::function<void()> action) {
        between_taking_and_looking_at_the_stop_ = std::move(action);
    }

  private:
    std::function<void()> between_reaching_the_limit_and_recording_the_stop_;
    std::function<void()> between_taking_and_looking_at_the_stop_;
    const int64_t limit_;
    std::atomic<int64_t> waiting_{0};
    std::atomic<bool> stopped_{false};
};

} // namespaces
