#pragma once
// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <string>

namespace sequencer {

/**
 * @brief A small file beside the sequencer's WAL holding the last wall-clock time this process knew of a matching engine
 * it routed orders to.
 *
 * When the whole venue has been down, sequencers included, the leading sequencer measures how long no engine could trade
 * from this time (EngineAbsenceRule). Written about once a second while an engine is routed to, by whichever instance of
 * the pair this is, since both are told which engine leads. A write is eight bytes to the file's page in memory, with no
 * flush: a process that dies leaves the page to the kernel, which writes it; a machine that loses power loses the last
 * few writes, which makes the absence measured longer, not shorter.
 *
 * Kept in the WAL's directory, so that it is cleared and archived with the WAL at the end of the trading day. The WAL
 * reads only the files named as its segments are.
 */
class EngineRoutedStamp {
  public:
    ~EngineRoutedStamp() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    EngineRoutedStamp() = default;
    EngineRoutedStamp(const EngineRoutedStamp&) = delete;
    EngineRoutedStamp& operator=(const EngineRoutedStamp&) = delete;
    EngineRoutedStamp(EngineRoutedStamp&&) = delete;
    EngineRoutedStamp& operator=(EngineRoutedStamp&&) = delete;

    /// The name of the file in the WAL's directory.
    static constexpr const char* file_name = "engine_routed_at";

    /**
     * @brief Opens the file, creating it if it is absent, and reads the time it holds.
     * @return false if the file could not be opened; the stamp then holds 0 and writes do nothing.
     */
    bool open(const std::string& wal_directory) {
        fd_ = ::open((wal_directory + "/" + file_name).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ < 0) {
            return false;
        }
        int64_t held = 0;
        if (::pread(fd_, &held, sizeof(held), 0) == static_cast<ssize_t>(sizeof(held))) {
            stamped_at_ns_ = held;
        }
        return true;
    }

    /// The time the file held when it was opened, or 0 if it held none.
    [[nodiscard]] int64_t stamped_at_ns() const {
        return stamped_at_ns_;
    }

    /// Records that an engine was routed to at this time. Returns false if the write failed.
    bool write(int64_t now_ns) {
        if (fd_ < 0) {
            return false;
        }
        return ::pwrite(fd_, &now_ns, sizeof(now_ns), 0) == static_cast<ssize_t>(sizeof(now_ns));
    }

  private:
    int fd_{-1};
    int64_t stamped_at_ns_{0};
};

} // namespaces
