#pragma once

#include <chrono>
#include <string>

#include <pubsub_itc_fw/TimerID.hpp>
#include <pubsub_itc_fw/TimerType.hpp>

namespace pubsub_itc_fw {

/** @ingroup threading_subsystem */

/**
 * @brief Manages the attributes for a timer.
 *
 * This class encapsulates all necessary information for a recurring or single-shot timer.
 */
class Timer {
  public:
    Timer(const std::string& name, ThreadID owner_thread_id, TimerID timer_id, TimerType type,
          std::chrono::microseconds interval)
        : name_(name), owner_thread_id_(owner_thread_id),
          timer_id_(timer_id), type_(type), interval_(interval) {}

    /**
     * @brief Returns the name of the timer.
     * @return The name of the timer.
     */
    [[nodiscard]] const std::string& get_name() const {
        return name_;
    }

    ThreadID get_owner_thread_id() const {
        return owner_thread_id_;
    }

    TimerID get_timer_id() const {
        return timer_id_;
    }
    /**
     * @brief Returns the type of the timer.
     * @return The timer's type.
     */
    [[nodiscard]] TimerType get_type() const {
        return type_;
    }

    /**
     * @brief Returns the interval of the timer.
     * @return The timer's interval.
     */
    [[nodiscard]] std::chrono::microseconds get_interval() const {
        return interval_;
    }

  private:
    std::string name_;
    ThreadID owner_thread_id_;
    TimerID timer_id_;
    TimerType type_;
    std::chrono::microseconds interval_;
};

} // namespace pubsub_itc_fw
