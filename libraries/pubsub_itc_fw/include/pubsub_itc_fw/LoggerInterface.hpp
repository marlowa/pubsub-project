#pragma once

#include <pubsub_itc_fw/LogLevel.hpp>

namespace pubsub_itc_fw {

class LoggerInterface {
public:
    // TODO we do not do enum classes this way and each type should have its own header
    enum class FilenameAppendMode {
        None,
        StartDateTime
    };

    virtual ~LoggerInterface() = default;

    virtual bool should_log(LogLevel log_level) const = 0;
    virtual void set_log_level(LogLevel log_level) = 0;

    virtual void flush() const = 0;
    virtual void set_immediate_flush() = 0;
};

} // namespace pubsub_itc_fw
