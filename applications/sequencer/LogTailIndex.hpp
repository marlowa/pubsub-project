#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <fmt/format.h>

#include <fix_orders.hpp>
#include <leader_follower.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/WalReader.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

#include "GatewayIds.hpp"

namespace sequencer {

/**
 * @brief An exact index of the commands in the most recent part of a sequencer's write-ahead log,
 *        built on demand, for checking the commands a gateway sends again after a change of leader.
 *
 * The record of identifiers (`LoggedCommandIdentifiers`) says only whether the log may hold a command.
 * When it says yes, the new leader must find out for certain, by looking for a record with exactly the
 * same comp id, gateway protocol and `ClOrdID`. A command sent again was first sent no earlier than
 * the time the gateway received it, which its envelope carries, so only the log from that time on
 * needs to be looked at.
 *
 * Right after a change of leader there can be many such commands, all recent. Searching the log for
 * each separately would read the same records many times, so this index reads each record at most
 * once:
 *
 * - **Forwards:** every lookup first reads the records written since the last one, so the index
 *   includes commands this leader sequenced after it was built.
 * - **Backwards:** it reads older segments, newest first, only as far back as a lookup needs, and
 *   remembers how far back it has read.
 *
 * Each command it reads is held as its exact identity, not a hash, so a lookup's answer is exact. The
 * memory grows with how far back lookups reach, which is normally a few seconds of the log, and the
 * sequencer discards the index once commands stop being sent again.
 *
 * **The clocks.** The time a gateway received a command is read from the gateway's clock, and the time
 * of a record from the sequencer's. The index reads `clock_margin_ns` further back than asked, so the
 * answer stays exact while the machines' clocks agree to within that, which time synchronisation
 * gives with a large margin to spare.
 *
 * It reads segments with `WalReader`, which returns only records whose checksum is complete. It is
 * used on the thread that writes the log, while leading, so nothing appends to the log during a read.
 */
class LogTailIndex {
  public:
    /// How much further back than asked the index reads, to allow for the gateway's and the sequencer's clocks differing.
    static constexpr int64_t clock_margin_ns = 5'000'000'000LL;

    explicit LogTailIndex(std::string directory) : directory_(std::move(directory)) {
        find_newest_segment_with_records();
    }

    /**
     * @brief Whether the log holds a command with exactly this comp id, protocol and `ClOrdID`.
     * @param[in] not_before_ns The command was first sent no earlier than this, by the gateway's clock.
     *            Records older than this, less `clock_margin_ns`, need not be read. The smallest
     *            possible value reads the whole log, for a command that carries no time.
     */
    [[nodiscard]] bool holds(std::string_view comp_id, int16_t protocol, std::string_view cl_ord_id, int64_t not_before_ns) {
        read_forwards();
        const int64_t read_back_to =
            not_before_ns < std::numeric_limits<int64_t>::min() + clock_margin_ns ? std::numeric_limits<int64_t>::min() : not_before_ns - clock_margin_ns;
        while (oldest_read_ns_ > read_back_to && next_older_ >= 0) {
            read_segment(segments_[static_cast<size_t>(next_older_)]);
            --next_older_;
        }
        return commands_.count(key(comp_id, protocol, cl_ord_id)) != 0;
    }

    /// How many distinct commands the index holds.
    [[nodiscard]] size_t size() const {
        return commands_.size();
    }

    /// How many records it has read, counting each once.
    [[nodiscard]] int64_t records_read() const {
        return records_read_;
    }

  private:
    // Forward reads start at the newest segment that holds a record. The writer creates the next
    // segment before it is needed, so the newest file is usually empty, and starting there would
    // never read the records still being appended to the one before it.
    void find_newest_segment_with_records() {
        segments_ = segment_numbers(directory_);
        std::ptrdiff_t newest = static_cast<std::ptrdiff_t>(segments_.size()) - 1;
        while (newest > 0 && !segment_has_records(segments_[static_cast<size_t>(newest)])) {
            --newest;
        }
        if (newest >= 0) {
            forward_from_ = pubsub_itc_fw::WalPosition{segments_[static_cast<size_t>(newest)], 0};
            next_older_ = newest - 1;
            started_ = true;
        }
    }

    [[nodiscard]] bool segment_has_records(uint64_t segment) const {
        const int fd = ::open(segment_path(segment).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        uint32_t magic = 0;
        const ssize_t got = ::pread(fd, &magic, sizeof(magic), 0);
        ::close(fd);
        return got == static_cast<ssize_t>(sizeof(magic)) && magic == pubsub_itc_fw::WalWriter::entry_magic;
    }

    [[nodiscard]] std::string segment_path(uint64_t segment) const {
        return fmt::format("{}/wal_{:06}.log", directory_, segment);
    }

    void read_forwards() {
        if (!started_) {
            find_newest_segment_with_records();
            if (!started_) {
                return;
            }
        }
        forward_from_ = pubsub_itc_fw::WalReader::replay(directory_, forward_from_,
                                                         [this](int64_t /*record_id*/, const void* payload, size_t size) { add_record(payload, size); });
    }

    void read_segment(uint64_t segment) {
        static_cast<void>(pubsub_itc_fw::WalReader::replay_segment(
            segment_path(segment), 0, [this](int64_t /*record_id*/, const void* payload, size_t size) { add_record(payload, size); }));
    }

    void add_record(const void* payload, size_t size) {
        ++records_read_;
        constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
        if (size <= header_size) {
            return;
        }
        int64_t wall_time_ns{};
        int16_t pdu_id{};
        std::memcpy(&wall_time_ns, payload, sizeof(int64_t));
        std::memcpy(&pdu_id, static_cast<const uint8_t*>(payload) + sizeof(int64_t), sizeof(int16_t));
        oldest_read_ns_ = std::min(oldest_read_ns_, wall_time_ns);
        if (pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
            return;
        }
        pubsub_itc_fw::BumpAllocator arena(arena_buffer_.data(), arena_buffer_.size());
        size_t bytes_consumed = 0;
        size_t arena_bytes_needed = 0;
        pubsub_itc_fw_app::WalRecordView view{};
        if (!pubsub_itc_fw_app::decode(view, static_cast<const uint8_t*>(payload) + header_size, size - header_size, bytes_consumed, arena,
                                       arena_bytes_needed)) {
            return;
        }
        if (view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) &&
            view.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
            return;
        }
        if (!view.has_cl_ord_id || !view.has_sender_comp_id) {
            return;
        }
        commands_.insert(key(view.sender_comp_id, view.has_origin_gateway_id ? view.origin_gateway_id : gateway_ids::default_when_absent, view.cl_ord_id));
    }

    // The exact identity of a command. The separator cannot occur in a FIX field value.
    [[nodiscard]] static std::string key(std::string_view comp_id, int16_t protocol, std::string_view cl_ord_id) {
        std::string result;
        result.reserve(comp_id.size() + cl_ord_id.size() + 8);
        result.append(comp_id);
        result.push_back('\x01');
        result.append(std::to_string(protocol));
        result.push_back('\x01');
        result.append(cl_ord_id);
        return result;
    }

    [[nodiscard]] static std::vector<uint64_t> segment_numbers(const std::string& directory) {
        std::vector<uint64_t> numbers;
        DIR* dir = ::opendir(directory.c_str());
        if (dir == nullptr) {
            return numbers;
        }
        while (const dirent* entry = ::readdir(dir)) {
            uint64_t number = 0;
            if (std::sscanf(entry->d_name, "wal_%06" SCNu64 ".log", &number) == 1) {
                numbers.push_back(number);
            }
        }
        ::closedir(dir);
        std::sort(numbers.begin(), numbers.end());
        return numbers;
    }

    std::string directory_;
    std::vector<uint64_t> segments_;
    // Whether a segment holding a record has been found to read forwards from.
    bool started_{false};
    // Where the next forward read starts.
    pubsub_itc_fw::WalPosition forward_from_{};
    // The index in segments_ of the next older segment to read, or -1 once every one has been read.
    std::ptrdiff_t next_older_{-1};
    // The oldest record time read so far.
    int64_t oldest_read_ns_{std::numeric_limits<int64_t>::max()};
    int64_t records_read_{0};
    std::unordered_set<std::string> commands_;
    std::array<uint8_t, 64 * 1024> arena_buffer_{};
};

} // namespaces
