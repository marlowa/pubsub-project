#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <fmt/format.h>

#include <LeaderStatement.hpp>
#include <LeasePromiseRecord.hpp>
#include <LeasePromiseRecorderInterface.hpp>

namespace fix_common {

/**
 * @brief Durable record of the promise an instance has made as a voter, valid until the machine reboots.
 *
 * The promise is stored as the instance it was made to and the moment it runs out on the steady
 * clock. On Linux the steady clock counts from boot and is the same for every process, so a moment
 * recorded by one process means the same moment to the next process on the same machine, until the
 * machine reboots. The record therefore also holds the kernel's boot id, and a record from a different
 * boot is not believed: the instance then knows nothing of what it promised, and waits out one lease
 * period as any voter that has forgotten must.
 *
 * The record also holds the newest leader's statement the instance holds about which instance may not
 * lead (LeaderStatement). A statement does not depend on the clock, so it is believed whichever boot
 * wrote it: an instance that has recorded that its peer may not lead must go on refusing that peer
 * after the machine reboots.
 *
 * The file holds one line: the boot id, the instance promised to (zero for none), the expiry in
 * nanoseconds on the steady clock, and then the statement as its leader (zero for none), epoch, number,
 * and 1 or 0 for whether the leader's peer may lead. A line without the statement reads as holding
 * none. It is written the way EpochStore writes the epoch: to a temporary file, flushed, and renamed
 * over the real one, with the directory flushed too, so a reader sees the old record or the new one
 * whatever moment the process dies at. A missing or damaged record reads as none, which is safe for
 * the promise, because the instance then waits; a lost statement is the risk described in
 * docs/availability/tla/findings.md section 12.5.
 */
class LeasePromiseStore : public LeasePromiseRecorderInterface {
  public:
    using Clock = std::chrono::steady_clock;

    /// What the record holds.
    using Record = LeasePromiseRecord;

    /**
     * @param[in] path File to hold the record. Its directory must already exist.
     * @param[in] boot_id The identity of this boot of the machine; current_boot_id() in production.
     */
    LeasePromiseStore(std::string path, std::string boot_id) : path_(std::move(path)), temp_path_(path_ + ".tmp"), boot_id_(std::move(boot_id)) {}

    /// The kernel's identity for this boot of the machine, or an empty string if it cannot be read.
    [[nodiscard]] static std::string current_boot_id() {
        const int fd = ::open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return {};
        }
        char buffer[64] = {};
        const ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer) - 1);
        ::close(fd);
        if (bytes_read <= 0) {
            return {};
        }
        std::string id(buffer, static_cast<size_t>(bytes_read));
        while (!id.empty() && (id.back() == '\n' || id.back() == ' ')) {
            id.pop_back();
        }
        return id;
    }

    /**
     * @brief Read the record back.
     * @return The recorded promise, or nothing when there is no record, it cannot be read, or it was
     *         written during a different boot. An instance that gets nothing must wait out a lease period.
     */
    [[nodiscard]] std::optional<Record> load() const {
        const std::optional<std::pair<std::string, Record>> read = read_line();
        if (!read.has_value() || boot_id_.empty() || read->first != boot_id_) {
            return std::nullopt;
        }
        return read->second;
    }

    /**
     * @brief Read back only the statement, whichever boot of the machine wrote it.
     * @return The statement recorded, or one naming no leader when there is no readable record.
     */
    [[nodiscard]] LeaderStatement load_statement() const {
        const std::optional<std::pair<std::string, Record>> read = read_line();
        return read.has_value() ? read->second.statement : LeaderStatement{};
    }

    [[nodiscard]] bool record(const LeasePromiseRecord& record) override {
        if (boot_id_.empty()) {
            // Without a boot id the record could not be told apart from one written before a reboot.
            return false;
        }
        const LeaderStatement& statement = record.statement;
        const std::string line = fmt::format("{} {} {} {} {} {} {}\n", boot_id_, record.promised_to,
                                             std::chrono::duration_cast<std::chrono::nanoseconds>(record.until.time_since_epoch()).count(), statement.leader_id,
                                             statement.epoch, statement.number, statement.peer_may_lead ? 1 : 0);
        const int fd = ::open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            return false;
        }
        size_t written = 0;
        while (written < line.size()) {
            const ssize_t n = ::write(fd, line.data() + written, line.size() - written);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return abandon_temp(fd);
            }
            written += static_cast<size_t>(n);
        }
        if (::fsync(fd) != 0) {
            return abandon_temp(fd);
        }
        if (::close(fd) != 0 || ::rename(temp_path_.c_str(), path_.c_str()) != 0) {
            ::unlink(temp_path_.c_str());
            return false;
        }
        sync_parent_directory();
        return true;
    }

    /// The file this store reads and writes.
    [[nodiscard]] const std::string& path() const {
        return path_;
    }

  private:
    // The boot id and the record from the file, or nothing if the file is missing or damaged.
    [[nodiscard]] std::optional<std::pair<std::string, Record>> read_line() const {
        const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::nullopt;
        }
        char buffer[256] = {};
        const ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer) - 1);
        ::close(fd);
        if (bytes_read <= 0) {
            return std::nullopt;
        }
        const std::string line(buffer, static_cast<size_t>(bytes_read));
        const size_t first_space = line.find(' ');
        if (first_space == std::string::npos || first_space == 0) {
            return std::nullopt;
        }
        // Up to six numbers follow the boot id: two for the promise, then four for the statement.
        long long numbers[6] = {0, 0, 0, 0, 0, 0};
        int count = 0;
        const char* next = line.c_str() + first_space;
        for (; count < 6; ++count) {
            while (*next == ' ') {
                ++next;
            }
            if (*next == '\n' || *next == '\0') {
                break;
            }
            errno = 0;
            char* end = nullptr;
            numbers[count] = std::strtoll(next, &end, 10);
            if (errno != 0 || end == next) {
                return std::nullopt;
            }
            next = end;
        }
        if ((count != 2 && count != 6) || numbers[0] < 0) {
            return std::nullopt;
        }
        Record record{static_cast<int64_t>(numbers[0]), Clock::time_point{std::chrono::nanoseconds{numbers[1]}}, LeaderStatement{}};
        if (count == 6) {
            const bool names_a_leader = numbers[2] == 1 || numbers[2] == 2;
            if ((numbers[2] != 0 && !names_a_leader) || (names_a_leader && numbers[4] < 1) || numbers[3] < 0 ||
                numbers[3] > std::numeric_limits<int32_t>::max() || (numbers[5] != 0 && numbers[5] != 1)) {
                return std::nullopt;
            }
            if (names_a_leader) {
                record.statement =
                    LeaderStatement{static_cast<int64_t>(numbers[2]), static_cast<int32_t>(numbers[3]), static_cast<int64_t>(numbers[4]), numbers[5] == 1};
            }
        }
        return std::make_pair(line.substr(0, first_space), record);
    }

    bool abandon_temp(int fd) const {
        ::close(fd);
        ::unlink(temp_path_.c_str());
        return false;
    }

    void sync_parent_directory() const {
        const size_t slash = path_.find_last_of('/');
        std::string dir{"."};
        if (slash == 0) {
            dir = "/";
        } else if (slash != std::string::npos) {
            dir = path_.substr(0, slash);
        }
        const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
        if (dfd < 0) {
            return;
        }
        ::fsync(dfd);
        ::close(dfd);
    }

    std::string path_;
    std::string temp_path_;
    std::string boot_id_;
};

} // namespaces
