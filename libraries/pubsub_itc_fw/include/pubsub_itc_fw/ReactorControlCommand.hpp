#pragma once

#include <string>
#include <chrono>

#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TimerID.hpp>

namespace pubsub_itc_fw {

class ReactorControlCommand
{
public:
    enum CommandTag
    {
        AddTimer,
        CancelTimer
    };

public:
    explicit ReactorControlCommand(CommandTag tag)
        : tag_(tag)
    {
    }

    CommandTag as_tag() const {
        return tag_;
    }

    std::string as_string() const {
        if (tag_ == AddTimer) {
            return "AddTimer";
        }
        if (tag_ == CancelTimer) {
            return "CancelTimer";
        }
        return fmt::format("unknown ({})", static_cast<int>(tag_));
    }

    bool is_equal(const ReactorControlCommand& rhs) const {
        return tag_ == rhs.tag_;
    }

public:
    // Payload fields — trivially copyable/movable
    ThreadID owner_thread_id_{};
    TimerID timer_id_{};
    std::string timer_name_;
    std::chrono::microseconds interval_{0};
    TimerType timer_type_{TimerType::SingleShot};

private:
    CommandTag tag_{AddTimer};
};

inline bool operator==(const ReactorControlCommand& lhs,
                       const ReactorControlCommand& rhs)
{
    return lhs.is_equal(rhs);
}

inline bool operator!=(const ReactorControlCommand& lhs,
                       const ReactorControlCommand& rhs)
{
    return !lhs.is_equal(rhs);
}

} // namespaces
