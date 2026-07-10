// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <cstddef>
#include <string>
#include <vector>

#include <unistd.h>

#include <pubsub_itc_fw/ConsoleCaptureInterface.hpp>

namespace pubsub_itc_fw {

ConsoleCaptureInterface::ConsoleCaptureInterface(size_t max_line_length) : max_line_length_(max_line_length) {}

void ConsoleCaptureInterface::drain_fd_until_eof(int read_fd) {
    std::vector<char> buffer(4096);

    while (true) {
        const ssize_t n = ::read(read_fd, buffer.data(), buffer.size());
        if (n > 0) {
            feed_bytes(buffer.data(), static_cast<size_t>(n));
        } else if (n == 0) {
            break; // end of stream
        } else if (errno != EINTR) {
            break;
        }
    }

    flush_pending(); // emit any trailing partial line
}

void ConsoleCaptureInterface::feed_bytes(const char* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        const char ch = data[i];
        if (ch == '\n') {
            log_line(pending_line_);
            pending_line_.clear();
        } else {
            pending_line_.push_back(ch);
            if (pending_line_.size() >= max_line_length_) {
                log_line(pending_line_); // force-flush a newline-less spewer
                pending_line_.clear();
            }
        }
    }
}

void ConsoleCaptureInterface::flush_pending() {
    if (!pending_line_.empty()) {
        log_line(pending_line_);
        pending_line_.clear();
    }
}

} // namespaces
