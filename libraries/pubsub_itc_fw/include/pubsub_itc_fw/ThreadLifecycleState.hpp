#pragma once

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
        if (tag_ == NotCreated) return "NotCreated";
        if (tag_ == Created) return "Created";
        if (tag_ == Started) return "Started";
        if (tag_ == InitialProcessed) return "InitialProcessed";
        if (tag_ == Operational) return "Operational";
        if (tag_ == ShuttingDown) return "ShuttingDown";
        if (tag_ == Terminated) return "Terminated";
        return fmt::format("unknown ({})", static_cast<int>(tag_));
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
