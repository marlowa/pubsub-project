#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <vector>

namespace pubsub_itc_fw::tests_common {

class UnitTestLogger {
  public:
    struct Entry {
        FwLogLevel level;
        enum class Dest { File, Syslog, Console } dest;
    };

    // TODO do we force logger instantiations to specify severity? Dunno
    UnitTestLogger() = default;

    std::vector<Entry> entries;

    // Per-destination log levels
    FwLogLevel file_level = FwLogLevel::Debug;
    FwLogLevel syslog_level = FwLogLevel::Debug;
    FwLogLevel console_level = FwLogLevel::Debug;

    bool immediate = false;
    bool print_to_console = false; // optional for debugging tests
};

} // namespace pubsub_itc_fw::tests_common
