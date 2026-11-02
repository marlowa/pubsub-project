#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <fmt/format.h>

#include <pubsub_itc_fw/CommandIdentityInterface.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>
#include <pubsub_itc_fw/WalReader.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

namespace pubsub_itc_fw {

/**
 * @brief An exact search of a WAL for a command, by its identity, reading backwards from the end of the
 *        WAL, for the cases in which the record of identifiers cannot settle whether the WAL holds a command.
 *
 * The record of identifiers (LoggedCommandIdentifiers) holds each command's position, and normally the WAL
 * reads that one record to prove a match. It cannot when the record is full, when it is still being built,
 * when two commands gave the same number, or when the WAL was cut back and a command written again
 * elsewhere (docs/availability/one_check_against_sequencing_twice.md, section 3.2). The WAL then searches
 * with this.
 *
 * Right after a change of leader there can be many commands to check, all recent. Searching the WAL for
 * each separately would read the same records many times, so this reads each record at most once:
 *
 * - **Forwards:** every search first reads the records written since the last one, so it includes commands
 *   written after it was created.
 * - **Backwards:** it reads older segments, newest first, only as far back as a search needs, and remembers
 *   how far back it has read.
 *
 * Each command it reads is held as its exact identity, so a search's answer is exact. The memory grows with
 * how far back searches reach, and the WAL discards the search once it is no longer needed.
 *
 * **The clocks.** The time a command was first sent is read from the sender's clock, and the time of a
 * record from the leader's. The search reads `clock_margin_ns` further back than asked, so the answer stays
 * exact while the machines' clocks agree to within that, which time synchronisation gives with a large
 * margin to spare.
 *
 * It reads segments with WalReader, which returns only records whose checksum is complete. It is used on
 * the thread that writes the WAL, so nothing appends to the WAL during a read.
 */
class WalCommandSearch {
  public:
    /// How much further back than asked the search reads, to allow for the sender's and the leader's clocks differing.
    static constexpr int64_t clock_margin_ns = 5'000'000'000LL;

    /**
     * @param[in] directory The WAL's directory.
     * @param[in] identity  How a command's identity is read from a record. Must outlive this object.
     */
    WalCommandSearch(std::string directory, const CommandIdentityInterface& identity) : directory_(std::move(directory)), identity_(&identity) {
        find_newest_segment_with_records();
    }

    /**
     * @brief The number of the earliest record the search has read that holds a command with exactly this identity.
     * @param[in] identity      The command's identity, as CommandIdentityInterface writes it.
     * @param[in] not_before_ns The command was first sent no earlier than this, by the sender's clock. Records
     *                          older than this, less `clock_margin_ns`, need not be read. The smallest possible
     *                          value reads the whole WAL.
     * @return The record's number, or nothing when no record the search read holds the command.
     */
    [[nodiscard]] std::optional<int64_t> find(std::string_view identity, int64_t not_before_ns) {
        read_forwards();
        const int64_t read_back_to =
            not_before_ns < std::numeric_limits<int64_t>::min() + clock_margin_ns ? std::numeric_limits<int64_t>::min() : not_before_ns - clock_margin_ns;
        while (oldest_read_ns_ > read_back_to && next_older_ >= 0) {
            read_segment(segments_[static_cast<size_t>(next_older_)]);
            --next_older_;
        }
        const auto found = commands_.find(std::string(identity));
        if (found == commands_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    /// Whether the WAL holds a command with exactly this identity: find(), without the record's number.
    [[nodiscard]] bool holds(std::string_view identity, int64_t not_before_ns) {
        return find(identity, not_before_ns).has_value();
    }

    /// How many distinct commands the search holds.
    [[nodiscard]] size_t size() const {
        return commands_.size();
    }

    /// How many records it has read, counting each once.
    [[nodiscard]] int64_t records_read() const {
        return records_read_;
    }

  private:
    // Forward reads start at the newest segment that holds a record. The writer creates the next segment
    // before it is needed, so the newest file is usually empty, and starting there would never read the
    // records still being appended to the one before it.
    void find_newest_segment_with_records() {
        segments_ = segment_numbers(directory_);
        std::ptrdiff_t newest = static_cast<std::ptrdiff_t>(segments_.size()) - 1;
        while (newest > 0 && !segment_has_records(segments_[static_cast<size_t>(newest)])) {
            --newest;
        }
        if (newest >= 0) {
            forward_from_ = WalPosition{segments_[static_cast<size_t>(newest)], 0};
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
        return got == static_cast<ssize_t>(sizeof(magic)) && magic == WalWriter::entry_magic;
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
        forward_from_ =
            WalReader::replay(directory_, forward_from_, [this](int64_t record_id, const void* payload, size_t size) { add_record(record_id, payload, size); });
    }

    void read_segment(uint64_t segment) {
        static_cast<void>(WalReader::replay_segment(segment_path(segment), 0,
                                                    [this](int64_t record_id, const void* payload, size_t size) { add_record(record_id, payload, size); }));
    }

    void add_record(int64_t record_id, const void* payload, size_t size) {
        ++records_read_;
        const std::optional<WalEntryContents> contents = read_wal_entry_contents(payload, size);
        if (!contents.has_value() || contents->size == 0) {
            return;
        }
        oldest_read_ns_ = std::min(oldest_read_ns_, contents->wall_time_ns);
        const size_t identity_size = identity_->identity_of(*contents, scratch_.data(), identity_buffer_.data());
        if (identity_size == 0 || identity_size > identity_buffer_.size()) {
            return;
        }
        // The earliest record is kept: the backwards reads reach older records after newer ones.
        const auto [held, inserted] = commands_.emplace(std::string(identity_buffer_.data(), identity_size), record_id);
        if (!inserted && record_id < held->second) {
            held->second = record_id;
        }
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
    const CommandIdentityInterface* identity_;
    std::vector<uint64_t> segments_;
    // Whether a segment holding a record has been found to read forwards from.
    bool started_{false};
    // Where the next forward read starts.
    WalPosition forward_from_{};
    // The index in segments_ of the next older segment to read, or -1 once every one has been read.
    std::ptrdiff_t next_older_{-1};
    // The oldest record time read so far.
    int64_t oldest_read_ns_{std::numeric_limits<int64_t>::max()};
    int64_t records_read_{0};
    // Each command read, by its exact identity, with the number of the earliest record holding it.
    std::unordered_map<std::string, int64_t> commands_;
    std::vector<uint8_t> scratch_ = std::vector<uint8_t>(CommandIdentityInterface::scratch_size);
    std::array<char, CommandIdentityInterface::max_identity_size> identity_buffer_{};
};

} // namespaces
