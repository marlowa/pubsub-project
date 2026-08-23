#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include <fmt/format.h>

namespace sequencer {

/**
 * @brief Durable store for the leadership epoch.
 *
 * The epoch is the venue's generation counter for sequencer leadership. Every
 * component that receives a sequencer PDU checks it, and rejects anything from
 * an older generation. That check only works if the counter never goes
 * backwards.
 *
 * Held only in memory, it does go backwards. The epoch starts at zero on every
 * start, so a pair of sequencers that are restarted together both come back at
 * zero, elect a leader between them, and start stamping messages with a
 * generation the venue used long ago. Downstream cannot tell the new leader
 * from the old one, and a message left over from the older generation now
 * compares as current. Restarting one node at a time hides this, because the
 * survivor tells the returning node what generation it is in.
 *
 * So the epoch is written to a small file that outlives the process. The file
 * holds one decimal number and a newline, which makes it readable with cat and
 * repairable with an editor when something has gone wrong at three in the
 * morning.
 *
 * Writes are atomic: the value goes to a temporary file, which is flushed and
 * then renamed over the real one. A reader therefore sees either the previous
 * epoch or the new one, never a half-written number, whatever moment the
 * process dies at. The containing directory is flushed as well so the rename
 * itself survives a power cut and not merely a crash.
 *
 * A missing, empty, or unparsable file reads as epoch zero. That is the
 * correct answer for a genuinely new deployment, and for a damaged file it is
 * the safe one: zero loses to every real epoch, so the node defers to its peer
 * or to the arbiter instead of claiming a generation it cannot substantiate.
 *
 * This class stores what it is given and applies no policy. Keeping the epoch
 * moving in one direction is the caller's business, because only the caller
 * knows which values are entitled to set it.
 */
class EpochStore {
  public:
    /**
     * @param[in] path  File to hold the epoch. Its directory must already
     *                  exist; the sequencer uses its WAL directory, which
     *                  deploy.py creates.
     */
    explicit EpochStore(std::string path) : path_{std::move(path)}, temp_path_{path_ + ".tmp"} {}

    /**
     * @brief Read the stored epoch.
     * @return The stored epoch, or 0 if the file is absent, empty, or damaged.
     */
    int32_t load() const {
        const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return 0;
        }
        char buf[32] = {};
        const ssize_t bytes_read = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        if (bytes_read <= 0) {
            return 0;
        }
        buf[bytes_read] = '\0';

        errno = 0;
        char* end = nullptr;
        const long value = std::strtol(buf, &end, 10);
        if (end == buf || errno != 0 || value < 0 || value > static_cast<long>(std::numeric_limits<int32_t>::max())) {
            return 0;
        }
        return static_cast<int32_t>(value);
    }

    /**
     * @brief Write the epoch durably, replacing any previous value.
     * @param[in] epoch  Epoch to record.
     * @return True if the value reached the disk; false if it did not, in which
     *         case the previously stored value is still intact.
     */
    bool store(int32_t epoch) const {
        // fmt::format_to_n rather than snprintf: bounded write into a buffer this
        // function owns, with no allocation on the path.
        char buf[32];
        const auto formatted = fmt::format_to_n(buf, sizeof(buf), "{}\n", epoch);
        if (formatted.size == 0 || formatted.size > sizeof(buf)) {
            return false;
        }
        const int len = static_cast<int>(formatted.size);

        const int fd = ::open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            return false;
        }

        int written = 0;
        while (written < len) {
            const ssize_t n = ::write(fd, buf + written, static_cast<size_t>(len - written));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return abandon_temp(fd);
            }
            written += static_cast<int>(n);
        }

        if (::fsync(fd) != 0) {
            return abandon_temp(fd);
        }
        if (::close(fd) != 0) {
            ::unlink(temp_path_.c_str());
            return false;
        }
        if (::rename(temp_path_.c_str(), path_.c_str()) != 0) {
            ::unlink(temp_path_.c_str());
            return false;
        }
        sync_parent_directory();
        return true;
    }

    /// @return The file this store reads and writes.
    const std::string& path() const {
        return path_;
    }

  private:
    /// Close and remove a temporary file a failed write left behind, so a later attempt starts clean.
    bool abandon_temp(int fd) const {
        ::close(fd);
        ::unlink(temp_path_.c_str());
        return false;
    }

    /// Flush the containing directory so the rename survives loss of power, not merely loss of the process.
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
