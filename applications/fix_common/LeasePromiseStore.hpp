#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <fmt/format.h>

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
 * The file holds one line: the boot id, the instance promised to (zero for none), and the expiry in
 * nanoseconds on the steady clock. It is written the way EpochStore writes the epoch: to a temporary
 * file, flushed, and renamed over the real one, with the directory flushed too, so a reader sees the
 * old record or the new one whatever moment the process dies at. A missing, damaged or foreign record
 * reads as none, which is safe: the instance waits.
 */
class LeasePromiseStore : public LeasePromiseRecorderInterface {
  public:
    using Clock = std::chrono::steady_clock;

    /// A promise read back from the record.
    struct Record {
        int64_t promised_to{0};
        Clock::time_point until{};
    };

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
        if (boot_id_.empty()) {
            return std::nullopt;
        }
        const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::nullopt;
        }
        char buffer[160] = {};
        const ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer) - 1);
        ::close(fd);
        if (bytes_read <= 0) {
            return std::nullopt;
        }
        const std::string line(buffer, static_cast<size_t>(bytes_read));
        const size_t first_space = line.find(' ');
        const size_t second_space = first_space == std::string::npos ? std::string::npos : line.find(' ', first_space + 1);
        if (second_space == std::string::npos || line.substr(0, first_space) != boot_id_) {
            return std::nullopt;
        }
        errno = 0;
        char* end = nullptr;
        const long long promised_to = std::strtoll(line.c_str() + first_space + 1, &end, 10);
        if (errno != 0 || end != line.c_str() + second_space || promised_to < 0) {
            return std::nullopt;
        }
        const long long until_ns = std::strtoll(line.c_str() + second_space + 1, &end, 10);
        if (errno != 0 || end == line.c_str() + second_space + 1) {
            return std::nullopt;
        }
        return Record{static_cast<int64_t>(promised_to), Clock::time_point{std::chrono::nanoseconds{until_ns}}};
    }

    [[nodiscard]] bool record(int64_t promised_to, Clock::time_point until) override {
        if (boot_id_.empty()) {
            // Without a boot id the record could not be told apart from one written before a reboot.
            return false;
        }
        const std::string line =
            fmt::format("{} {} {}\n", boot_id_, promised_to, std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count());
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
