#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

#include <pubsub_itc_fw/ByteStreamInterface.hpp>
#include <pubsub_itc_fw/GatheredSends.hpp>
#include <pubsub_itc_fw/PduFramer.hpp>
#include <pubsub_itc_fw/WaitingSendQueue.hpp>

namespace pubsub_itc_fw::gathered_writing {

/**
 * @brief The two ways a connection's framework messages are written several at a time, with one system call
 *        (docs/framework/gathering_sends.md), shared by the outbound and the inbound connection managers.
 *
 * A connection used here provides:
 *
 * - `gathered_sends()`, its GatheredSends;
 * - `waiting_sends()`, its WaitingSendQueue;
 * - `has_pending_send()`, whether a message is part written;
 * - `write_gathered(sends, count)`, which writes the sends with PduFramer::send_gathered(), returns the chunks of those
 *   written whole to their allocators, holds the one written in part, if any, as its message in progress, and returns
 *   the framer's outcome. The sends after those are untouched, and still the caller's.
 *
 * One thread, the reactor's, calls these.
 */

/// How far writing got.
enum class Result {
    /// Everything given was written whole; nothing is in progress or waiting because of it.
    all_written,
    /// Some is still to be written, in progress or waiting: the connection must be watched for room to write.
    must_wait,
    /// The stream failed, or the waiting queue had no room; the connection must be closed, with `error` as the reason.
    failed
};

struct Step {
    Result result{Result::all_written};
    std::string error;
    /// The stream said it was short of memory for the moment (ENOBUFS or ENOMEM); treated as having no room.
    bool short_of_memory{false};
};

/**
 * @brief Writes the sends gathered on @p connection in this pass, which come before anything waiting.
 *
 * Those not written, after the one written in part if any, go to the back of the waiting queue, which is empty, since
 * a send is gathered only while nothing waits; if the queue refuses one, for its limits, the rest are returned to their
 * allocators and the result is failed. The gathered sends are empty afterwards, whatever the result.
 */
template <typename Connection> Step write_gathered(Connection& connection, size_t maximum_sends, size_t maximum_bytes) {
    GatheredSends& gathered = connection.gathered_sends();
    Step step;
    if (gathered.empty()) {
        return step;
    }
    const PduFramer::GatheredOutcome outcome = connection.write_gathered(gathered.data(), gathered.size());
    if (!outcome.ok) {
        // Nothing was written: the stream reports a failure only when it took nothing.
        gathered.release_all();
        step.result = Result::failed;
        step.error = outcome.error;
        return step;
    }
    step.short_of_memory = outcome.short_of_memory;
    const size_t not_written_from = outcome.written_whole + (outcome.front_cut ? 1 : 0);
    for (size_t index = not_written_from; index < gathered.size(); ++index) {
        if (!connection.waiting_sends().add(gathered.at(index), maximum_sends, maximum_bytes)) {
            for (size_t refused = index; refused < gathered.size(); ++refused) {
                const WaitingSend& send = gathered.at(refused);
                send.allocator->deallocate(send.slab_id, send.chunk);
            }
            gathered.clear();
            step.result = Result::failed;
            step.error = "the queue of sends waiting to be written is full";
            return step;
        }
    }
    const bool everything_written = not_written_from == gathered.size() && !outcome.front_cut;
    gathered.clear();
    step.result = everything_written ? Result::all_written : Result::must_wait;
    return step;
}

/**
 * @brief Writes the sends waiting on @p connection, oldest first, ByteStreamInterface::max_gathered_parts at a time,
 *        until none is left, one is written in part, or the stream has no room. Nothing may be in progress.
 *
 * A send written whole leaves the queue, its chunk returned; one written in part leaves it too, now the connection's
 * message in progress. On a failure nothing has left the queue, and closing the connection returns every chunk.
 */
template <typename Connection> Step write_waiting(Connection& connection) {
    WaitingSendQueue& waiting = connection.waiting_sends();
    Step step;
    std::array<WaitingSend, ByteStreamInterface::max_gathered_parts> batch{};
    while (!waiting.empty() && !connection.has_pending_send()) {
        const size_t count = std::min(waiting.size(), batch.size());
        for (size_t index = 0; index < count; ++index) {
            batch[index] = waiting.at(index);
        }
        const PduFramer::GatheredOutcome outcome = connection.write_gathered(batch.data(), count);
        if (!outcome.ok) {
            step.result = Result::failed;
            step.error = outcome.error;
            return step;
        }
        step.short_of_memory = step.short_of_memory || outcome.short_of_memory;
        const size_t left_the_queue = outcome.written_whole + (outcome.front_cut ? 1 : 0);
        for (size_t index = 0; index < left_the_queue; ++index) {
            waiting.pop_front();
        }
        if (left_the_queue == 0) {
            // No room, or short of memory: wait for room to write.
            break;
        }
    }
    step.result = waiting.empty() && !connection.has_pending_send() ? Result::all_written : Result::must_wait;
    return step;
}

} // namespaces
