#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pthread.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/CommandIdentityInterface.hpp>
#include <pubsub_itc_fw/CpuPinning.hpp>
#include <pubsub_itc_fw/LoggedCommandIdentifiers.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Builds the record of command identifiers on a thread of its own, so that a starting instance does
 *        not wait for it.
 *
 * The record holds the identity of every command in the WAL and where it is, so that the WAL can tell
 * whether it already holds a command (LoggedCommandIdentifiers). Building it means reserving a table of
 * several gigabytes, which takes about a second, and reading every record in the WAL, which grows through
 * the trading day. Done before the instance starts work, that made a restarted sequencer slower to start
 * than its peer's lease, so that a quick restart always changed the leader (docs/bug_list.md, BUG-0121).
 *
 * So the instance starts at once, and this reads the segments it is given, in a thread of its own, into a
 * record of its own. Until the record is finished, the WAL searches itself for any command it must check,
 * notes the commands it writes meanwhile, and adds them when it takes the record over. The segments are
 * listed when the WAL has been opened. The last of them may still be being written: only records whose
 * checksum is complete are read, and anything written after this has read it is among the commands the WAL
 * notes itself.
 *
 * A position recorded here for a record later removed from the WAL, by a repair, does no harm: the WAL
 * reads the record at a position before it relies on it, and searches when what it finds is not the
 * command.
 *
 * **Threading.** start(), finished(), take() and stop() are called by the thread that owns the WAL. The
 * building thread touches only this object's members, and finished() is the only thing the two share while
 * it runs. The destructor stops the thread and waits for it, so that it never outlives the object it writes
 * into; it checks for a stop between segments, so the wait is at most the time to read one segment, or to
 * reserve the table if that is still going on.
 */
class LoggedCommandIdentifiersBuilder {
  public:
    /// One segment to read: its number, which with an offset is a position, and its file.
    struct Segment {
        uint64_t number{0};
        std::string path;
    };

    ~LoggedCommandIdentifiersBuilder() {
        stop();
    }

    LoggedCommandIdentifiersBuilder() = default;
    LoggedCommandIdentifiersBuilder(const LoggedCommandIdentifiersBuilder&) = delete;
    LoggedCommandIdentifiersBuilder& operator=(const LoggedCommandIdentifiersBuilder&) = delete;
    LoggedCommandIdentifiersBuilder(LoggedCommandIdentifiersBuilder&&) = delete;
    LoggedCommandIdentifiersBuilder& operator=(LoggedCommandIdentifiersBuilder&&) = delete;

    /**
     * @brief Starts building a record with room for @p capacity identities from the given segments, oldest
     *        first.
     * @param[in] capacity         How many identities the record has room for.
     * @param[in] segments         The segments to read, oldest first.
     * @param[in] identity         How a command's identity is read from a record. Must outlive the build.
     * @param[in] number_of        How an identity becomes a number.
     * @param[in] background_cores The processors the building thread may run on; empty to leave it where it lands.
     */
    void start(size_t capacity, std::vector<Segment> segments, const CommandIdentityInterface& identity,
               LoggedCommandIdentifiers::NumberFunction number_of = &LoggedCommandIdentifiers::identifier, std::vector<CpuId> background_cores = {}) {
        capacity_ = capacity;
        segments_ = std::move(segments);
        identity_ = &identity;
        number_of_ = number_of;
        background_cores_ = std::move(background_cores);
        thread_.start([this] { build(); });
    }

    /// Whether the record is built and may be taken.
    [[nodiscard]] bool finished() const {
        return finished_.load(std::memory_order_acquire);
    }

    /**
     * @brief Hands over the built record, once finished() is true, and waits for the thread to end, which it
     *        does as soon as it has finished.
     */
    [[nodiscard]] LoggedCommandIdentifiers take() {
        thread_.join();
        LoggedCommandIdentifiers record = std::move(*built_);
        built_.reset();
        return record;
    }

    /// Asks the thread to stop, if it is running, and waits for it.
    void stop() {
        stop_requested_.store(true, std::memory_order_release);
        thread_.join();
    }

    /// How long reserving the table took. Valid once finished.
    [[nodiscard]] std::chrono::milliseconds reserve_time() const {
        return reserve_time_;
    }

    /// How long reading the WAL took. Valid once finished.
    [[nodiscard]] std::chrono::milliseconds read_time() const {
        return read_time_;
    }

    /// How many segments could not be read, or not read to the end, and were passed over. Valid once finished.
    [[nodiscard]] int segments_not_read() const {
        return segments_not_read_;
    }

    /// Why the last segment passed over could not be read. Valid once finished.
    [[nodiscard]] const std::string& last_segment_error() const {
        return last_segment_error_;
    }

    /// How many WAL records were read. Valid once finished.
    [[nodiscard]] int64_t records_read() const {
        return records_read_;
    }

    /// The size of the table, in bytes. Valid once finished.
    [[nodiscard]] size_t table_bytes() const {
        return table_bytes_;
    }

  private:
    void build() {
        // It puts itself where it belongs. A new thread inherits the processor mask of the thread that
        // created it, which is the owner's own thread, pinned to a processor reserved for the order path:
        // left there, reserving the table and reading the WAL would share that processor with the thread
        // that sequences every order (docs/bug_list.md, BUG-0164). Named, so that it can be told apart from
        // the owner's thread. Where no processors are given, as in a test, it is left where it lands.
        pthread_setname_np(pthread_self(), "IdBuilder");
        if (!background_cores_.empty()) {
            static_cast<void>(apply_thread_affinity_mask(0, background_cores_));
        }
        growth_reporter_.report_threshold_bytes = 1;
        growth_reporter_.on_large_allocation = [this](size_t bytes, size_t /*largest*/) { table_bytes_ = bytes; };
        const auto reserve_started = std::chrono::steady_clock::now();
        built_.emplace(capacity_, &growth_reporter_, number_of_);
        const auto read_started = std::chrono::steady_clock::now();
        reserve_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(read_started - reserve_started);

        for (const Segment& segment : segments_) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                return;
            }
            // A repair of the WAL can cut it back while this reads it: the rest of the segment holding the
            // last record kept is overwritten with zeros, and every later segment is deleted. Reading a
            // deleted segment then fails, and so can reading one while it is being zeroed, which shows an
            // entry partly zeroed followed by one not yet reached. Both happen only at or after the point
            // the WAL was cut back, among records no longer in it, so the segment is passed over and the
            // reading carries on with the next. Anything written after the cut, the WAL notes itself.
            try {
                static_cast<void>(WalReader::replay_segment_with_offsets(
                    segment.path, 0, [this, &segment](int64_t /*record_id*/, size_t offset, const void* payload, size_t size) {
                        note_record(WalPosition{segment.number, offset}, payload, size);
                    }));
            } catch (const PubSubItcException& error) {
                ++segments_not_read_;
                last_segment_error_ = error.what();
            }
        }
        read_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - read_started);
        finished_.store(true, std::memory_order_release);
    }

    // What a record on disk holds is described in pubsub_itc_fw/WalEntryContents.hpp.
    void note_record(WalPosition position, const void* record, size_t size) {
        ++records_read_;
        const std::optional<WalEntryContents> contents = read_wal_entry_contents(record, size);
        if (!contents.has_value() || contents->size == 0) {
            return;
        }
        const size_t identity_size = identity_->identity_of(*contents, scratch_.data(), identity_buffer_.data());
        if (identity_size == 0 || identity_size > identity_buffer_.size()) {
            return;
        }
        // A full record answers "may hold" for everything, so a command that does not fit is safe.
        static_cast<void>(built_->add(built_->number_of(std::string_view(identity_buffer_.data(), identity_size)), position));
    }

    size_t capacity_{0};
    std::vector<Segment> segments_;
    const CommandIdentityInterface* identity_{nullptr};
    LoggedCommandIdentifiers::NumberFunction number_of_{&LoggedCommandIdentifiers::identifier};
    std::vector<CpuId> background_cores_;
    std::optional<LoggedCommandIdentifiers> built_;
    AllocationGrowthReporter growth_reporter_;
    size_t table_bytes_{0};
    std::chrono::milliseconds reserve_time_{0};
    std::chrono::milliseconds read_time_{0};
    int64_t records_read_{0};
    int segments_not_read_{0};
    std::string last_segment_error_;
    std::vector<uint8_t> scratch_ = std::vector<uint8_t>(CommandIdentityInterface::scratch_size);
    std::array<char, CommandIdentityInterface::max_identity_size> identity_buffer_{};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};
    ThreadWithJoinTimeout thread_; ///< Last, so that it starts after every member it uses exists and stops first.
};

} // namespaces
