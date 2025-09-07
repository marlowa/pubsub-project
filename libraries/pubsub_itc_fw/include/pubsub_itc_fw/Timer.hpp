#pragma once

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <chrono>
#include <string>

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/TimerType.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Manages the state and logic for a single timer.
 *
 * This class is an internal component of `ApplicationThread` and encapsulates
 * all necessary information for a recurring or single-shot timer.
 */
class Timer final {
  public:
    /**
     * @brief Constructs a Timer.
     * @param [in] name The unique name of the timer.
     * @param [in] type The type of timer (single-shot or recurring).
     * @param [in] interval The interval for the timer.
     */
    Timer(const std::string& name, TimerType type, std::chrono::microseconds interval)
        : name_(name), type_(type), interval_(interval) {}

    /**
     * @brief Returns the name of the timer.
     * @return The name of the timer.
     */
    [[nodiscard]] const std::string& get_name() const {
        return name_;
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
    TimerType type_;
    std::chrono::microseconds interval_;
};

} // namespace pubsub_itc_fw
