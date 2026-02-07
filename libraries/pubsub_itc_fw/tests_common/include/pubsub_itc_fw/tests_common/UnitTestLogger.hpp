#pragma once

#include <pubsub_itc_fw/QuillLogger.hpp>
#include <vector>

namespace pubsub_itc_fw::tests_common {

class UnitTestLogger {
public:
    struct Entry {
        LogLevel level;
        enum class Dest { File, Syslog, Console } dest;
    };

    // TODO do we force logger instantiations to specify severity? Dunno
    UnitTestLogger() = default;

    std::vector<Entry> entries;

    // Per-destination log levels
    LogLevel file_level    = LogLevel::Debug;
    LogLevel syslog_level  = LogLevel::Debug;
    LogLevel console_level = LogLevel::Debug;

    bool immediate = false;
    bool print_to_console = false; // optional for debugging tests
};

} // namespace pubsub_itc_fw
