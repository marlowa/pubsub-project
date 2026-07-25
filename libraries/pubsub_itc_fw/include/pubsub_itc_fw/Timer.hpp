#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>

#include <pubsub_itc_fw/TimerID.hpp>
#include <pubsub_itc_fw/TimerType.hpp>

namespace pubsub_itc_fw {

/** @ingroup threading_subsystem */

/**
 * @brief Manages the attributes for a timer.
 *
 * This class encapsulates all necessary information for a recurring or single-shot timer.
 *
 * A timer is identified purely by its integer TimerID. The framework carries no
 * timer name: if an application wants a human-readable label for its own debug
 * logging it keeps its own TimerID-to-name map, so no string is ever constructed
 * on the reactor control path.
 */
class Timer {
  public:
    Timer(ThreadID owner_thread_id, TimerID timer_id, TimerType type, std::chrono::microseconds interval)
        : owner_thread_id_(owner_thread_id), timer_id_(timer_id), type_(type), interval_(interval) {}

    [[nodiscard]] ThreadID get_owner_thread_id() const {
        return owner_thread_id_;
    }

    [[nodiscard]] TimerID get_timer_id() const {
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
    ThreadID owner_thread_id_;
    TimerID timer_id_;
    TimerType type_;
    std::chrono::microseconds interval_;
};

} // namespaces
