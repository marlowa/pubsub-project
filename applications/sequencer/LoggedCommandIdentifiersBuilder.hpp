#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include "GatewayIds.hpp"
#include "LoggedCommandIdentifiers.hpp"

namespace sequencer {

/**
 * @brief The identifier a logged command is recorded under, or nothing if the record is not a command
 *        that can be sent again: a new order or a request to cancel, saying whose it is.
 *
 * The one place that decides this, used both by the sequencer as it writes and reads records and by
 * LoggedCommandIdentifiersBuilder as it reads the log in the background, so that the two cannot record
 * different things.
 */
[[nodiscard]] inline std::optional<uint64_t> command_identifier(std::string_view comp_id, int16_t protocol, int16_t inner_pdu_id, std::string_view cl_ord_id) {
    if (cl_ord_id.empty() || comp_id.empty()) {
        return std::nullopt;
    }
    if (inner_pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) &&
        inner_pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        return std::nullopt;
    }
    return LoggedCommandIdentifiers::identifier(comp_id, protocol, cl_ord_id);
}

/// The identifier of the command in a decoded log record, as command_identifier above.
[[nodiscard]] inline std::optional<uint64_t> command_identifier(const pubsub_itc_fw_app::WalRecordView& view) {
    return command_identifier(view.has_sender_comp_id ? view.sender_comp_id : std::string_view{},
                              view.has_origin_gateway_id ? view.origin_gateway_id : gateway_ids::default_when_absent, view.pdu_id,
                              view.has_cl_ord_id ? view.cl_ord_id : std::string_view{});
}

/**
 * @brief Builds the record of command identifiers on a thread of its own, so that a starting sequencer
 *        does not wait for it.
 *
 * The record holds the identifier of every command in the write-ahead log, so that a leader can tell
 * whether its log already holds a command a gateway sends again (part 4.3 of
 * docs/availability/a_follower_behind_does_not_lead.md). Building it means reserving a table of several
 * gigabytes, which takes about a second, and reading every record in the log, which grows through the
 * trading day. Done before the sequencer starts work, that made a restarted sequencer slower to start
 * than its peer's lease, so that a quick restart always changed the leader (docs/bug_list.md, BUG-0121).
 *
 * So the sequencer starts at once, and this reads the log segments it is given, in a thread of its own,
 * into a record of its own. The sequencer checks every command it must check against the log itself
 * while this runs, notes the commands it writes or reads meanwhile, and takes the record once it is
 * finished. The segments are listed by the sequencer when it has opened the log. The last of them may
 * still be being written: only records whose checksum is complete are read, and anything written after
 * this has read it is among the commands the sequencer notes itself.
 *
 * The record is only a filter. Every command it says may be in the log is checked exactly against the
 * log, so an identifier recorded from a record later removed from the log, by a repair of the log, does
 * no harm.
 *
 * **Threading.** start(), finished(), take() and stop() are called by the sequencer's thread. The
 * building thread touches only this object's members, and finished() is the only thing the two share
 * while it runs. The destructor stops the thread and waits for it, so that it never outlives the object
 * it writes into; it checks for a stop between segments, so the wait is at most the time to read one
 * segment, or to reserve the table if that is still going on.
 */
class LoggedCommandIdentifiersBuilder {
  public:
    ~LoggedCommandIdentifiersBuilder() {
        stop();
    }

    LoggedCommandIdentifiersBuilder() = default;
    LoggedCommandIdentifiersBuilder(const LoggedCommandIdentifiersBuilder&) = delete;
    LoggedCommandIdentifiersBuilder& operator=(const LoggedCommandIdentifiersBuilder&) = delete;
    LoggedCommandIdentifiersBuilder(LoggedCommandIdentifiersBuilder&&) = delete;
    LoggedCommandIdentifiersBuilder& operator=(LoggedCommandIdentifiersBuilder&&) = delete;

    /**
     * @brief Starts building a record with room for @p capacity identifiers from the given log segments,
     *        oldest first.
     */
    void start(size_t capacity, std::vector<std::string> segment_paths) {
        capacity_ = capacity;
        segment_paths_ = std::move(segment_paths);
        thread_.start([this] { build(); });
    }

    /// Whether the record is built and may be taken.
    [[nodiscard]] bool finished() const {
        return finished_.load(std::memory_order_acquire);
    }

    /**
     * @brief Hands over the built record, once finished() is true, and waits for the thread to end, which
     *        it does as soon as it has finished.
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

    /// How long reading the log took. Valid once finished.
    [[nodiscard]] std::chrono::milliseconds read_time() const {
        return read_time_;
    }

    /// How many log records were read. Valid once finished.
    [[nodiscard]] int64_t records_read() const {
        return records_read_;
    }

    /// The size of the table, in bytes. Valid once finished.
    [[nodiscard]] size_t table_bytes() const {
        return table_bytes_;
    }

  private:
    void build() {
        growth_reporter_.report_threshold_bytes = 1;
        growth_reporter_.on_large_allocation = [this](size_t bytes, size_t /*largest*/) { table_bytes_ = bytes; };
        const auto reserve_started = std::chrono::steady_clock::now();
        built_.emplace(capacity_, &growth_reporter_);
        const auto read_started = std::chrono::steady_clock::now();
        reserve_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(read_started - reserve_started);

        for (const std::string& path : segment_paths_) {
            if (stop_requested_.load(std::memory_order_acquire)) {
                return;
            }
            static_cast<void>(pubsub_itc_fw::WalReader::replay_segment(
                path, 0, [this](int64_t /*record_id*/, const void* payload, size_t size) { note_record(static_cast<const uint8_t*>(payload), size); }));
        }
        read_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - read_started);
        finished_.store(true, std::memory_order_release);
    }

    // A record on disk is [written at : int64][pdu id : int16][payload].
    void note_record(const uint8_t* record, size_t size) {
        ++records_read_;
        constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
        if (size <= header_size) {
            return;
        }
        int16_t pdu_id{};
        std::memcpy(&pdu_id, record + sizeof(int64_t), sizeof(int16_t));
        if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
            return;
        }
        pubsub_itc_fw::BumpAllocator arena(arena_.data(), arena_.size());
        size_t bytes_consumed = 0;
        size_t arena_bytes_needed = 0;
        pubsub_itc_fw_app::WalRecordView view{};
        if (!pubsub_itc_fw_app::decode(view, record + header_size, size - header_size, bytes_consumed, arena, arena_bytes_needed)) {
            return;
        }
        const std::optional<uint64_t> id = command_identifier(view);
        if (id.has_value()) {
            // A full record answers "may hold" for everything, so a command that does not fit is safe.
            static_cast<void>(built_->add(*id));
        }
    }

    size_t capacity_{0};
    std::vector<std::string> segment_paths_;
    std::optional<LoggedCommandIdentifiers> built_;
    pubsub_itc_fw::AllocationGrowthReporter growth_reporter_;
    size_t table_bytes_{0};
    std::chrono::milliseconds reserve_time_{0};
    std::chrono::milliseconds read_time_{0};
    int64_t records_read_{0};
    std::vector<uint8_t> arena_ = std::vector<uint8_t>(64 * 1024);
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> finished_{false};
    pubsub_itc_fw::ThreadWithJoinTimeout thread_; ///< Last, so that it starts after every member it uses exists and stops first.
};

} // namespaces
