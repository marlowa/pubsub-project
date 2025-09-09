#pragma once

#include <chrono>
#include <string>

#include <pubsub_itc_fw/TimerType.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Manages the state and logic for a single timer.
 *
 * This class is an internal component of ApplicationThread and encapsulates
 * all necessary information for a recurring or single-shot timer.
 */
class TimerHandler {
public:
    /**
     * @brief Constructs a TimerHandler.
     * @param [in] name The unique name of the timer.
     * @param [in] type The type of timer (single-shot or recurring).
     * @param [in] interval The interval for the timer.
     */
    TimerHandler(const std::string& name, TimerType type, std::chrono::microseconds interval)
        : name_(name),
          type_(type),
          interval_(interval) {}

    /**
     * @brief Returns the name of the timer.
     * @returns std::string The name of the timer.
     */
    std::string get_name() const { return name_; }

    /**
     * @brief Returns the type of the timer.
     * @returns TimerType The timer's type.
     */
    TimerType get_type() const { return type_; }

    /**
     * @brief Returns the interval of the timer.
     * @returns std::chrono::microseconds The timer's interval.
     */
    std::chrono::microseconds get_interval() const { return interval_; }

private:
    std::string name_;
    TimerType type_;
    std::chrono::microseconds interval_;
};

} // namespace pubsub_itc_fw
