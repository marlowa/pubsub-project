#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <fmt/format.h>

namespace pubsub_itc_fw {

/** @ingroup messaging_subsystem */

/**
 * @brief Event type enumeration for EventMessage classification.
 */
class EventType
{
public:
    /**
     * @brief C-style enumeration of event types.
     */
    enum EventTypeTag
    {
        None,
        Initial,
        AppReady,
        Termination,
        InterthreadCommunication,
        Timer,
        PubSubCommunication,
        RawSocketCommunication
    };

public:
    /**
     * @brief Constructs EventType from tag value.
     * @param tag Event type tag
     */
    constexpr explicit EventType(EventTypeTag tag) {
        event_type_ = tag;
    }

    /**
     * @brief Returns string representation of the event type.
     * @return Event type as string
     */
    [[nodiscard]] std::string as_string() const {
        if (event_type_ == None) {
            return "None";
        } if (event_type_ == Initial) {
            return "Initial";
        } if (event_type_ == AppReady) {
                return "AppReady";
        } if (event_type_ == Termination) {
            return "Termination";
        } if (event_type_ == InterthreadCommunication) {
            return "InterthreadCommunication";
        } if (event_type_ == Timer) {
            return "Timer";
        } if (event_type_ == PubSubCommunication) {
            return "PubSubCommunication";
        } if (event_type_ == RawSocketCommunication) {
            return "RawSocketCommunication";
        }

        return fmt::format("unknown ({})", static_cast<int>(event_type_));
    }

    EventTypeTag as_tag() const {
        return event_type_;
    }

    /**
     * @brief Checks equality with another EventType.
     * @param other EventType to compare with
     * @return True if equal, false otherwise
     */
    bool is_equal(const EventType& rhs) const {
        return event_type_ == rhs.event_type_;
    }

private:
    EventTypeTag event_type_{None};
};

/**
 * @brief Equality operator for EventType.
 * @param[in] lhs Left-hand side EventType
 * @param[in] rhs Right-hand side EventType
 * @return True if equal, false otherwise
 */
inline bool operator==(const EventType& lhs, const EventType& rhs)
{
    return lhs.is_equal(rhs);
}

inline bool operator!=(const EventType& lhs, const EventType& rhs)
{
    return !lhs.is_equal(rhs);
}

}
