#pragma once

#include <string>

namespace pubsub_itc_fw {

class ReactorLifecycleState {
public:
    enum Tag {
        NotStarted,
        Running,
        ShutdownRequested,
        FinalizingThreads,
        Finished
    };

    constexpr explicit ReactorLifecycleState(Tag tag)
        : tag_(tag)
    {}

    Tag as_tag() const { return tag_; }

    bool is_equal(const ReactorLifecycleState& rhs) const {
        return tag_ == rhs.tag_;
    }

    [[nodiscard]] std::string as_string() const {
        return ReactorLifecycleState::to_string(tag_);
    }

    static std::string to_string(Tag tag) {
        if (tag == NotStarted) return "NotStarted";
        if (tag == Running) return "Running";
        if (tag == ShutdownRequested) return "ShutdownRequested";
        if (tag == FinalizingThreads) return "FinalizingThreads";
        if (tag == Finished) return "Finished";
        return fmt::format("unknown ({})", static_cast<int>(tag));
    }

private:
    Tag tag_{NotStarted};
};

inline bool operator==(const ReactorLifecycleState& lhs, const ReactorLifecycleState& rhs)
{
    return lhs.is_equal(rhs);
}

inline bool operator!=(const ReactorLifecycleState& lhs, const ReactorLifecycleState& rhs)
{
    return !lhs.is_equal(rhs);
}

} // namespace pubsub_itc_fw
