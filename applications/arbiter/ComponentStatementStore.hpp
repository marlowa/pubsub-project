#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <fmt/format.h>

#include <LeaderStatement.hpp>
#include <leader_follower.hpp>

namespace arbiter {

/**
 * @brief The arbiter's durable record of the newest statement it holds from each component pair's leader.
 *
 * A leader of the sequencer pair says, on its lease requests, whether its peer may lead, and says it
 * may not while it has the matching engine act on commands the peer does not hold. The arbiter refuses
 * a lease to an instance a statement it holds says may not lead. That record must survive the arbiter
 * restarting, or a restarted arbiter could elect an instance lacking commands the engine acted on
 * (docs/availability/tla/traces/behind-4-arbiter-forgets.txt). The active arbiter also copies each
 * statement to the passive one, which keeps it here too.
 *
 * The file holds one line per group: the group's number, the statement's leader, epoch and number, and
 * 1 or 0 for whether the leader's peer may lead. It is written to a temporary file, flushed, and renamed
 * over the real one, with the directory flushed too, so a reader sees the old record or the new one
 * whatever moment the process dies at. A line that cannot be read is skipped.
 */
class ComponentStatementStore {
  public:
    using Statements = std::map<pubsub_itc_fw_app::ComponentGroup, fix_common::LeaderStatement>;

    /// @param[in] path File to hold the record, or empty for none: nothing is then kept.
    explicit ComponentStatementStore(std::string path) : path_(std::move(path)), temp_path_(path_ + ".tmp") {}

    /// The file this store reads and writes, or empty.
    [[nodiscard]] const std::string& path() const {
        return path_;
    }

    /// Read back every statement recorded. Empty when there is no file.
    [[nodiscard]] Statements load() const {
        Statements statements;
        if (path_.empty()) {
            return statements;
        }
        const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return statements;
        }
        std::string contents;
        char buffer[512];
        for (;;) {
            const ssize_t bytes_read = ::read(fd, buffer, sizeof(buffer));
            if (bytes_read < 0 && errno == EINTR) {
                continue;
            }
            if (bytes_read <= 0) {
                break;
            }
            contents.append(buffer, static_cast<size_t>(bytes_read));
        }
        ::close(fd);
        size_t start = 0;
        while (start < contents.size()) {
            size_t end = contents.find('\n', start);
            if (end == std::string::npos) {
                end = contents.size();
            }
            read_line(contents.substr(start, end - start), statements);
            start = end + 1;
        }
        return statements;
    }

    /**
     * @brief Replace the record with @p statements.
     * @return true when the record reached the disk.
     */
    [[nodiscard]] bool save(const Statements& statements) const {
        if (path_.empty()) {
            return false;
        }
        std::string contents;
        for (const auto& [group, statement] : statements) {
            fmt::format_to(std::back_inserter(contents), "{} {} {} {} {}\n", static_cast<int32_t>(group), statement.leader_id, statement.epoch,
                           statement.number, statement.peer_may_lead ? 1 : 0);
        }
        const int fd = ::open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            return false;
        }
        size_t written = 0;
        while (written < contents.size()) {
            const ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
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

  private:
    static void read_line(const std::string& line, Statements& statements) {
        long long numbers[5] = {0, 0, 0, 0, 0};
        const char* next = line.c_str();
        for (long long& number : numbers) {
            errno = 0;
            char* end = nullptr;
            number = std::strtoll(next, &end, 10);
            if (errno != 0 || end == next) {
                return;
            }
            next = end;
        }
        const long long group = numbers[0];
        const long long leader = numbers[1];
        const long long epoch = numbers[2];
        const long long number = numbers[3];
        const long long may_lead = numbers[4];
        if (group < 1 || group > 3 || (leader != 1 && leader != 2) || epoch < 0 || epoch > std::numeric_limits<int32_t>::max() || number < 1 ||
            (may_lead != 0 && may_lead != 1)) {
            return;
        }
        statements[static_cast<pubsub_itc_fw_app::ComponentGroup>(group)] =
            fix_common::LeaderStatement{static_cast<int64_t>(leader), static_cast<int32_t>(epoch), static_cast<int64_t>(number), may_lead == 1};
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
};

} // namespaces
