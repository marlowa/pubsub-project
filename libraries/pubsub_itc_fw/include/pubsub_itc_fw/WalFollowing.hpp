#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>

/**
 * @file WalFollowing.hpp
 * @brief What the thread that owns a WAL and the thread that reads it for its followers share.
 *
 * A component that follows the WAL is sent every record after the last it holds, read from the WAL by
 * WalReadingThread, and then handed over to the thread that owns the WAL, which sends it each new
 * record. WalFollowerService runs the hand-over on the owning thread. The design is
 * docs/availability/following_the_wal.md.
 */

namespace pubsub_itc_fw {

/**
 * @brief Which of the owning thread's positions a follower is served up to.
 *
 * Section 3.3 of the design: the follower sequencer is served up to the last record written, because
 * replication sends records as they are written; a publisher up to the last committed, because it may
 * publish only what the venue's record holds; a matching engine up to the last order the application
 * has released to it.
 */
enum class FollowerLimit : int8_t { LastWritten, LastCommitted, LastReleased };

/**
 * @brief The positions the thread that owns a WAL publishes for the thread that reads it.
 *
 * Each is written by the owning thread only, with a release store after the thing it describes is
 * done, and read by the reading thread with an acquire load. The reading thread never reads WAL bytes
 * at or past the published write position, so everything it reads was completely written before it
 * read it, and it never sends a record numbered above the follower's limit.
 *
 * The write position is two numbers, a segment and an offset, published together in one 64-bit atomic
 * so that a reader never sees one from one record and the other from another: the segment in the top
 * 24 bits and the offset in the bottom 40. A segment number of 2^24 or an offset of 2^40 bytes would not
 * fit; both are far beyond a trading day, and publishing either is refused rather than wrapped.
 *
 * Publishing costs the owning thread one store to an atomic variable for each record written and for
 * each change of the committed or released position.
 */
class WalFollowingPositions {
  public:
    static constexpr int offset_bits = 40;
    static constexpr uint64_t largest_offset = (uint64_t{1} << offset_bits) - 1;
    static constexpr uint64_t largest_segment = (uint64_t{1} << (64 - offset_bits)) - 1;

    /// Called by the owning thread after it has written record @p seq_no, ending at @p end.
    void publish_written(int64_t seq_no, WalPosition end) {
        if (end.segment > largest_segment || end.offset > largest_offset) {
            throw PreconditionAssertion("WalFollowingPositions::publish_written: position does not fit in 64 bits", __FILE__, __LINE__);
        }
        write_position_.store((end.segment << offset_bits) | end.offset, std::memory_order_release);
        last_written_.store(seq_no, std::memory_order_release);
    }

    /// Called by the owning thread when the last committed record changes.
    void publish_committed(int64_t seq_no) {
        last_committed_.store(seq_no, std::memory_order_release);
    }

    /// Called by the owning thread when the last record it has released to its followers of the third kind changes.
    void publish_released(int64_t seq_no) {
        last_released_.store(seq_no, std::memory_order_release);
    }

    /// The position just after the last record written; reading stops before it.
    [[nodiscard]] WalPosition write_position() const {
        const uint64_t packed = write_position_.load(std::memory_order_acquire);
        return WalPosition{packed >> offset_bits, packed & largest_offset};
    }

    /// The last record a follower of this kind may be sent.
    [[nodiscard]] int64_t limit(FollowerLimit kind) const {
        switch (kind) {
            case FollowerLimit::LastWritten:
                return last_written_.load(std::memory_order_acquire);
            case FollowerLimit::LastCommitted:
                return last_committed_.load(std::memory_order_acquire);
            case FollowerLimit::LastReleased:
                return last_released_.load(std::memory_order_acquire);
        }
        return 0;
    }

  private:
    std::atomic<uint64_t> write_position_{0};
    std::atomic<int64_t> last_written_{0};
    std::atomic<int64_t> last_committed_{0};
    std::atomic<int64_t> last_released_{0};
};

/// What a WalRecordConverter found one WAL record to be.
struct ConvertedRecord {
    enum class Outcome : int8_t {
        Send,     ///< Encoded into the output: send it, as message type pdu_id.
        PassOver, ///< Not for this follower, such as an execution report for a matching engine.
        Damaged   ///< Could not be read, so it may have been one the follower needed: the follower is not handed over.
    };
    Outcome outcome{Outcome::PassOver};
    int16_t pdu_id{0}; ///< Send: the message type of what was encoded.

    [[nodiscard]] static ConvertedRecord send(int16_t message_pdu_id) {
        return ConvertedRecord{Outcome::Send, message_pdu_id};
    }
    [[nodiscard]] static ConvertedRecord pass_over() {
        return ConvertedRecord{Outcome::PassOver, 0};
    }
    [[nodiscard]] static ConvertedRecord damaged() {
        return ConvertedRecord{Outcome::Damaged, 0};
    }
};

/**
 * @brief Turns one WAL record into the message a follower is sent, passes it over, or finds it damaged.
 *
 * Supplied by the application, one for each kind of follower, and run on the reading thread: it may
 * read only the record it is given and write only @p out, and must touch nothing that belongs to the
 * application's thread. It writes the encoded message into @p out and returns ConvertedRecord::send with
 * its message type; returns ConvertedRecord::pass_over if the record is not for this follower; and returns
 * ConvertedRecord::damaged if the record cannot be read, in which case the reading thread stops serving the
 * follower and it is never handed over (docs/availability/following_the_wal.md, section 3.2).
 */
using WalRecordConverter = std::function<ConvertedRecord(int64_t seq_no, const WalEntryContents& entry, std::vector<uint8_t>& out)>;

/**
 * @brief An instruction from the owning thread to the reading thread.
 *
 * Passed as a heap object whose ownership moves to the reading thread with the message that carries
 * it. These pass at the start and end of a catch-up, never for each record.
 */
struct WalReadingCommand {
    /// Identifies the object at the start of an inter-thread message.
    static constexpr uint32_t marker = 0x57524331; // "WRC1"

    enum class Kind : int8_t {
        Serve,    ///< Start sending a follower the records after after_seq_no.
        Continue, ///< Carry on sending to a follower that reported reaching a position short of the limit.
        Stop,     ///< Stop sending to a follower, whose connection has gone.
        StopAll,  ///< Stop sending to every follower; answered with a StoppedAll report.
        Confirm,  ///< Answered with a Confirmed report, once every earlier command has been acted on.
    };

    uint32_t identity{marker};
    Kind kind{Kind::Serve};
    ConnectionID connection{};
    int64_t after_seq_no{0}; ///< Serve: the last record the follower holds.
    WalPosition start{};     ///< Serve: where to start reading, at or before the record after after_seq_no.
    FollowerLimit limit{FollowerLimit::LastCommitted};
    int converter{0}; ///< Serve: which of the reading thread's converters to use.
    /// Serve: numbers this serving of the follower, echoed on its reports, so that a report from an
    /// earlier serving of the same connection, still on its way, is told apart and ignored.
    uint64_t serving{0};
};

/**
 * @brief A report from the reading thread to the owning thread. Passed like a WalReadingCommand.
 */
struct WalReadingReport {
    static constexpr uint32_t marker = 0x57525231; // "WRR1"

    enum class Kind : int8_t {
        Reached,    ///< Sent every record up to reached_seq_no, the limit as it last read it, and stopped sending.
        StoppedAll, ///< Has stopped sending to every follower, in answer to StopAll.
        Confirmed,  ///< Has acted on every command before a Confirm, in answer to it.
        Damaged,    ///< Found record reached_seq_no damaged, and has stopped sending to this follower.
    };

    uint32_t identity{marker};
    Kind kind{Kind::Reached};
    ConnectionID connection{};
    int64_t reached_seq_no{0};
    int64_t records_sent{0}; ///< Records sent to this follower since it was first served.
    uint64_t serving{0};     ///< Reached: the serving it reports on, from the Serve command.
};

/**
 * @brief Takes ownership of a T carried by an inter-thread message, if the message carries one.
 *
 * The message's payload points at the object, which the sender made on the heap and gave up. Returns
 * nothing if the message carries something else, so that a thread can offer it every message it gets.
 */
template <typename T> [[nodiscard]] std::unique_ptr<T> take_carried(const EventMessage& message) {
    if (message.type().as_tag() != EventType::InterthreadCommunication || message.payload() == nullptr ||
        message.payload_size() != static_cast<int>(sizeof(T))) {
        return nullptr;
    }
    uint32_t identity = 0;
    std::memcpy(&identity, message.payload(), sizeof(identity));
    if (identity != T::marker) {
        return nullptr;
    }
    // The sender made the object with new and passed its address; ownership comes with it.
    return std::unique_ptr<T>(const_cast<T*>(reinterpret_cast<const T*>(message.payload())));
}

/// An inter-thread message that carries @p object, giving up ownership of it to the receiver.
template <typename T> [[nodiscard]] EventMessage carry(ThreadID from, std::unique_ptr<T> object) {
    T* raw = object.release();
    return EventMessage::create_itc_message(from, reinterpret_cast<const uint8_t*>(raw), static_cast<int>(sizeof(T)));
}

} // namespaces
