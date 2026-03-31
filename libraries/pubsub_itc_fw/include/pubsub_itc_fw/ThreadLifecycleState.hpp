#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.

#include <string>

namespace pubsub_itc_fw {

class ThreadLifecycleState {
public:
    enum Tag {
        NotCreated,
        Created,
        Started,
        InitialProcessed,
        Operational,
        ShuttingDown,
        Terminated
    };

    constexpr explicit ThreadLifecycleState(Tag tag) {
        tag_ = tag;
    }

    Tag as_tag() const { return tag_; }

    bool is_equal(const ThreadLifecycleState& rhs) const {
        return tag_ == rhs.tag_;
    }

    [[nodiscard]] std::string as_string() const {
        return ThreadLifecycleState::to_string(tag_);
    }

    static std::string to_string(Tag tag) {
        if (tag == NotCreated) return "NotCreated";
        if (tag == Created) return "Created";
        if (tag == Started) return "Started";
        if (tag == InitialProcessed) return "InitialProcessed";
        if (tag == Operational) return "Operational";
        if (tag == ShuttingDown) return "ShuttingDown";
        if (tag == Terminated) return "Terminated";
        return fmt::format("unknown ({})", static_cast<int>(tag));
    }

private:
    Tag tag_{NotCreated};
};

inline bool operator==(const ThreadLifecycleState& lhs, const ThreadLifecycleState& rhs)
{
    return lhs.is_equal(rhs);
}

inline bool operator!=(const ThreadLifecycleState& lhs, const ThreadLifecycleState& rhs)
{
    return !lhs.is_equal(rhs);
}

} // namespaces
