// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <string>

#include <pubsub_itc_fw/ConsoleCaptureInterface.hpp>

namespace pubsub_itc_fw {

ConsoleCaptureInterface::ConsoleCaptureInterface(size_t max_line_length) : max_line_length_(max_line_length) {}

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
